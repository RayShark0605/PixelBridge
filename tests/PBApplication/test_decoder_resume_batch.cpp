#include "decoder_resume_store.h"
#include "decoder_resume_io_test_hook.h"
#include "pbouterfec/wirehair_v2.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/byte_io.h"
#include "pbprotocol/crc32c.h"
#include "pbprotocol/descriptor_codec.h"
#include "pbprotocol/product_visual_profile.h"
#include "pbprotocol/protocol_version.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <new>
#include <optional>
#include <string>

namespace
{

struct IoState
{
    std::size_t seekCalls = 0;
    std::size_t writeCalls = 0;
    std::size_t flushCalls = 0;
    std::size_t maximumWriteBytes = 0;
    std::size_t failSeekCall = 0;
    std::size_t failWriteCall = 0;
    std::size_t failFlushCall = 0;
    std::size_t partialWriteBytes = 0;
    std::size_t prepareCalls = 0;
    std::size_t failPrepareCall = 0;
    bool shortWriteSuccess = false;
};

// Only this test executable owns the hook state. Product targets never compile
// the hook branch or expose an input that can select it.
thread_local IoState* activeIoState = nullptr;

class ScopedIo
{
public:
    explicit ScopedIo(IoState& state) : previous_(activeIoState)
    {
        activeIoState = &state;
    }
    ~ScopedIo()
    {
        activeIoState = previous_;
    }
private:
    IoState* previous_;
};

class ScratchDirectory
{
public:
    ScratchDirectory()
    {
        static std::uint64_t ordinal = 0;
        const auto root = std::filesystem::absolute(PB_TEST_SCRATCH_ROOT).lexically_normal();
        path_ = root / (L"resume-batch-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(ordinal++));
        REQUIRE(path_.parent_path() == root);
        REQUIRE_FALSE(std::filesystem::exists(path_));
        REQUIRE(std::filesystem::create_directory(path_));
    }
    ~ScratchDirectory()
    {
        const auto root = std::filesystem::absolute(PB_TEST_SCRATCH_ROOT).lexically_normal();
        if (path_.parent_path() == root && path_.filename().wstring().starts_with(L"resume-batch-"))
        {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }
    }
    const std::filesystem::path& GetPath() const noexcept
    {
        return path_;
    }
private:
    std::filesystem::path path_;
};

std::vector<std::byte> MakeBytes(const std::size_t count)
{
    std::vector<std::byte> bytes(count);
    for (std::size_t index = 0; index < count; index++)
    {
        bytes[index] = static_cast<std::byte>((index * 73 + 19) & 255);
    }
    return bytes;
}

std::vector<std::byte> ReadBytes(const std::filesystem::path& path)
{
    const auto size = std::filesystem::file_size(path);
    REQUIRE(size <= 4 * 1048576);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream);
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(stream.gcount() == static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

void WriteBytes(const std::filesystem::path& path, const std::span<const std::byte> bytes)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    REQUIRE(stream);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    REQUIRE(stream);
}

std::vector<std::byte> WrapControl(const pbprotocol::ControlRecordType type, const std::uint64_t sequence,
    const pbprotocol::SessionTag tag, const std::span<const std::byte> payload)
{
    const pbprotocol::ControlRecordView view{pbprotocol::kControlVersion, type, sequence, tag, payload};
    const auto size = pbprotocol::GetSerializedSize(view);
    REQUIRE(size);
    std::vector<std::byte> bytes(size.Value());
    REQUIRE(pbprotocol::SerializeControlRecord(view, bytes));
    return bytes;
}

struct Fixture
{
    ScratchDirectory scratch;
    pbprotocol::ReceiverResourcePolicy policy = pbprotocol::GetDefaultReceiverResourcePolicy();
    pbprotocol::SessionDescriptor session;
    pbprotocol::SessionTag tag;
    std::vector<std::byte> sessionControl;
    std::vector<std::byte> segmentControl;
    pbapp::DecoderResumeAcceptedBlock block;
    pbapp::DecoderResumeLoadedState loaded;
    std::unique_ptr<pbapp::DecoderResumeStore> store;
    std::filesystem::path journalPath;

    explicit Fixture(const std::uint32_t blockBytes = 1629, const std::optional<std::size_t> recordCapacity = std::nullopt)
    {
        const auto raw = MakeBytes(1048576);
        auto encoder = pbouterfec::WirehairV2Encoder::Create(raw, blockBytes);
        REQUIRE(encoder);
        session.protocolVersion = pbprotocol::GetProtocolVersion();
        for (std::size_t index = 0; index < session.sessionId.bytes.size(); index++)
        {
            session.sessionId.bytes[index] = static_cast<std::byte>(index + 31);
        }
        tag = pbprotocol::DeriveSessionTag(session.sessionId);
        session.originalFileSize = raw.size();
        session.segmentCount = 1;
        session.digestAlgorithm = pbprotocol::DigestAlgorithm::Blake3_256;
        session.sessionVisualProfileId = pbprotocol::kUnifiedVisualProfileId;
        session.fileNameUtf8 = "resume-batch.bin";
        const auto sessionSize = pbprotocol::GetSerializedSize(session);
        REQUIRE(sessionSize);
        std::vector<std::byte> sessionPayload(sessionSize.Value());
        REQUIRE(pbprotocol::SerializeSessionDescriptor(session, policy, sessionPayload));
        sessionControl = WrapControl(pbprotocol::ControlRecordType::SessionDescriptor, 1, tag, sessionPayload);
        pbprotocol::SegmentDescriptor segment;
        segment.sessionTag = tag;
        segment.rawSize = raw.size();
        segment.encodedSize = raw.size();
        segment.compressionCodec = pbprotocol::CompressionCodec::Raw;
        segment.outerFecMode = pbprotocol::OuterFecMode::WirehairV2;
        segment.outerBlockBytes = blockBytes;
        segment.wirehairV2SerializedProfile = encoder.Value().GetSerializedProfile();
        const auto digest = pbprotocol::ComputeBlake3Digest(raw);
        segment.rawDigest = pbprotocol::RawDigest{digest};
        segment.encodedDigest = pbprotocol::EncodedDigest{digest};
        const auto segmentSize = pbprotocol::GetSerializedSize(segment);
        REQUIRE(segmentSize);
        std::vector<std::byte> segmentPayload(segmentSize.Value());
        REQUIRE(pbprotocol::SerializeSegmentDescriptor(segment, session, policy, segmentPayload));
        segmentControl = WrapControl(pbprotocol::ControlRecordType::SegmentDescriptor, 2, tag, segmentPayload);
        if (recordCapacity)
        {
            policy.maxResumeBytes = 28 + sessionControl.size() + 36 + segmentControl.size() + *recordCapacity * (56 + blockBytes);
        }
        REQUIRE(pbapp::DecoderResumeStore::Open(scratch.GetPath(), tag, sessionControl, policy, store, loaded));
        REQUIRE(store->RecordSegmentControl(segmentControl));
        journalPath = store->GetPath();
        block.declaredPayloadBytes = static_cast<std::uint16_t>(blockBytes);
        block.paddedPayload = MakeBytes(blockBytes);
    }

    void Accept(const std::size_t count)
    {
        for (std::size_t index = 0; index < count; index++)
        {
            block.outerBlockId = static_cast<std::uint32_t>(index);
            REQUIRE(store->RecordAcceptedBlock(block));
        }
    }

    void Reopen()
    {
        store.reset();
        loaded = {};
        REQUIRE(pbapp::DecoderResumeStore::Open(scratch.GetPath(), tag, sessionControl, policy, store, loaded));
        REQUIRE(loaded.resumed);
    }
};

} // namespace

namespace pbapp::test
{

BOOL SeekJournalAppend(const HANDLE file, const LARGE_INTEGER offset) noexcept
{
    if (activeIoState != nullptr)
    {
        activeIoState->seekCalls++;
        if (activeIoState->seekCalls == activeIoState->failSeekCall)
        {
            SetLastError(ERROR_SEEK);
            return FALSE;
        }
    }
    return SetFilePointerEx(file, offset, nullptr, FILE_BEGIN);
}

BOOL WriteJournalAppend(const HANDLE file, const std::span<const std::byte> bytes, DWORD& writtenBytes) noexcept
{
    if (activeIoState != nullptr)
    {
        activeIoState->writeCalls++;
        activeIoState->maximumWriteBytes = (std::max)(activeIoState->maximumWriteBytes, bytes.size());
        if (activeIoState->writeCalls == activeIoState->failWriteCall)
        {
            const auto count = static_cast<DWORD>((std::min)(activeIoState->partialWriteBytes, bytes.size()));
            writtenBytes = 0;
            if (count != 0 && WriteFile(file, bytes.data(), count, &writtenBytes, nullptr) == FALSE)
            {
                return FALSE;
            }
            SetLastError(ERROR_DISK_FULL);
            return activeIoState->shortWriteSuccess ? TRUE : FALSE;
        }
    }
    return WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &writtenBytes, nullptr);
}

BOOL FlushJournalAppend(const HANDLE file) noexcept
{
    if (activeIoState != nullptr)
    {
        activeIoState->flushCalls++;
        if (activeIoState->flushCalls == activeIoState->failFlushCall)
        {
            SetLastError(ERROR_WRITE_FAULT);
            return FALSE;
        }
    }
    return FlushFileBuffers(file);
}

void BeforeJournalBatchSerialize()
{
    if (activeIoState != nullptr)
    {
        activeIoState->prepareCalls++;
        if (activeIoState->prepareCalls == activeIoState->failPrepareCall)
        {
            throw std::bad_alloc();
        }
    }
}

} // namespace pbapp::test

TEST_CASE("Resume checkpoint batches preserve record bytes and reduce native write calls", "[application][resume][journal][batch]")
{
    Fixture batched;
    Fixture serial;
    constexpr std::size_t count = 100;
    batched.Accept(count);
    IoState observed;
    {
        ScopedIo scope(observed);
        REQUIRE(batched.store->Checkpoint());
    }
    REQUIRE(observed.writeCalls == 3);
    REQUIRE(observed.seekCalls == 3);
    REQUIRE(observed.flushCalls == 1);
    REQUIRE(observed.maximumWriteBytes <= 65536);
    REQUIRE(batched.store->GetGeneration() == 1 + count);
    REQUIRE(batched.store->GetPendingBlockCount() == 0);
    for (std::size_t index = 0; index < count; index++)
    {
        serial.block.outerBlockId = static_cast<std::uint32_t>(index);
        REQUIRE(serial.store->RecordAcceptedBlock(serial.block));
        REQUIRE(serial.store->Checkpoint());
    }
    batched.store.reset();
    serial.store.reset();
    REQUIRE(ReadBytes(batched.journalPath) == ReadBytes(serial.journalPath));
    batched.Reopen();
    REQUIRE(batched.loaded.activeBlocks.size() == count);
    for (std::size_t index = 0; index < count; index++)
    {
        REQUIRE(batched.loaded.activeBlocks[index].outerBlockId == index);
        REQUIRE(batched.loaded.activeBlocks[index].paddedPayload == batched.block.paddedPayload);
    }
}

TEST_CASE("Resume checkpoint preflights its entire quota before writing any prefix", "[application][resume][journal][batch][quota]")
{
    Fixture fixture(1629, 5);
    const auto initialBytes = fixture.store->GetFileBytes();
    const auto initialGeneration = fixture.store->GetGeneration();
    fixture.Accept(6);
    IoState observed;
    {
        ScopedIo scope(observed);
        REQUIRE_FALSE(fixture.store->Checkpoint());
        REQUIRE_FALSE(fixture.store->Checkpoint());
    }
    REQUIRE(fixture.store->GetFileBytes() == initialBytes);
    REQUIRE(fixture.store->GetGeneration() == initialGeneration);
    REQUIRE(fixture.store->GetPendingBlockCount() == 6);
    REQUIRE(observed.seekCalls == 0);
    REQUIRE(observed.writeCalls == 0);
    fixture.Reopen();
    REQUIRE(fixture.loaded.activeBlocks.empty());
}

TEST_CASE("Resume checkpoint accepts exact batch and quota boundaries and an oversized single record", "[application][resume][journal][batch][boundary]")
{
    for (const std::size_t count : {0u, 1u, 38u, 39u, 76u, 77u})
    {
        CAPTURE(count);
        Fixture fixture(1629, count);
        fixture.Accept(count);
        IoState observed;
        {
            ScopedIo scope(observed);
            REQUIRE(fixture.store->Checkpoint());
        }
        REQUIRE(fixture.store->GetFileBytes() == fixture.policy.maxResumeBytes);
        REQUIRE(observed.writeCalls == (count + 37) / 38);
        REQUIRE(observed.flushCalls == (count != 0 ? 1 : 0));
        REQUIRE(fixture.store->GetPendingBlockCount() == 0);
        fixture.Reopen();
        REQUIRE(fixture.loaded.activeBlocks.size() == count);
    }
    Fixture oversized(65535, 2);
    oversized.Accept(2);
    IoState observed;
    {
        ScopedIo scope(observed);
        REQUIRE(oversized.store->Checkpoint());
    }
    REQUIRE(observed.writeCalls == 2);
    REQUIRE(observed.maximumWriteBytes == 65591);
    oversized.Reopen();
    REQUIRE(oversized.loaded.activeBlocks.size() == 2);
}

TEST_CASE("Resume checkpoint I/O failures are terminal and a partial batch reopens only validated records", "[application][resume][journal][batch][failure]")
{
    Fixture fixture;
    fixture.Accept(100);
    IoState fault;
    std::size_t expectedRecords = 0;
    bool expectedTruncatedTail = false;
    SECTION("seek failure")
    {
        fault.failSeekCall = 1;
    }
    SECTION("short successful write inside second batch")
    {
        fault.failWriteCall = 2;
        fault.partialWriteBytes = 1685 + 7;
        fault.shortWriteSuccess = true;
        expectedRecords = 39;
        expectedTruncatedTail = true;
    }
    SECTION("failed write after partial second batch")
    {
        fault.failWriteCall = 2;
        fault.partialWriteBytes = 1685 + 7;
        expectedRecords = 39;
        expectedTruncatedTail = true;
    }
    SECTION("checkpoint flush failure")
    {
        fault.failFlushCall = 1;
        expectedRecords = 100;
    }
    SECTION("allocation failure after a complete prefix batch")
    {
        fault.failPrepareCall = 40;
        expectedRecords = 38;
    }
    {
        ScopedIo scope(fault);
        REQUIRE_FALSE(fixture.store->Checkpoint());
        const auto writesAfterFailure = fault.writeCalls;
        REQUIRE(fixture.store->GetPendingBlockCount() == 100);
        REQUIRE_FALSE(fixture.store->Checkpoint());
        fixture.block.outerBlockId = 100;
        REQUIRE_FALSE(fixture.store->RecordAcceptedBlock(fixture.block));
        REQUIRE(fault.writeCalls == writesAfterFailure);
    }
    fixture.Reopen();
    REQUIRE(fixture.loaded.hadTruncatedTail == expectedTruncatedTail);
    REQUIRE(fixture.loaded.activeBlocks.size() == expectedRecords);
}

TEST_CASE("Resume checkpoint rejects generation overflow before touching disk", "[application][resume][journal][batch][overflow]")
{
    Fixture fixture;
    fixture.store.reset();
    auto bytes = ReadBytes(fixture.journalPath);
    const auto recordStart = 28 + fixture.sessionControl.size();
    pbprotocol::ByteWriter generationWriter(std::span(bytes).subspan(recordStart + 16, 8));
    REQUIRE(generationWriter.WriteUint64((std::numeric_limits<std::uint64_t>::max)() - 1));
    const auto crc = pbprotocol::ComputeCrc32c(std::span<const std::byte>(bytes).subspan(recordStart, bytes.size() - recordStart - 4));
    pbprotocol::ByteWriter crcWriter(std::span(bytes).last(4));
    REQUIRE(crcWriter.WriteUint32(crc));
    WriteBytes(fixture.journalPath, bytes);
    fixture.Reopen();
    REQUIRE(fixture.store->GetGeneration() == (std::numeric_limits<std::uint64_t>::max)() - 1);
    fixture.Accept(2);
    IoState observed;
    {
        ScopedIo scope(observed);
        REQUIRE_FALSE(fixture.store->Checkpoint());
    }
    REQUIRE(observed.writeCalls == 0);
    REQUIRE(fixture.store->GetGeneration() == (std::numeric_limits<std::uint64_t>::max)() - 1);
    fixture.store.reset();
    REQUIRE(ReadBytes(fixture.journalPath) == bytes);
    fixture.Reopen();
    fixture.Accept(1);
    REQUIRE(fixture.store->Checkpoint());
    REQUIRE(fixture.store->GetGeneration() == (std::numeric_limits<std::uint64_t>::max)());
    fixture.Accept(2);
    REQUIRE_FALSE(fixture.store->Checkpoint());
    fixture.Reopen();
    REQUIRE(fixture.loaded.activeBlocks.size() == 1);
}

TEST_CASE("Resume checkpoint allocation failure before writes can retry without duplicated records", "[application][resume][journal][batch][allocation]")
{
    Fixture fixture;
    fixture.Accept(100);
    const auto initialBytes = fixture.store->GetFileBytes();
    IoState fault;
    fault.failPrepareCall = 1;
    {
        ScopedIo scope(fault);
        REQUIRE_FALSE(fixture.store->Checkpoint());
    }
    REQUIRE(fault.writeCalls == 0);
    REQUIRE(fixture.store->GetFileBytes() == initialBytes);
    REQUIRE(fixture.store->GetGeneration() == 1);
    REQUIRE(fixture.store->GetPendingBlockCount() == 100);
    REQUIRE(fixture.store->Checkpoint());
    REQUIRE(fixture.store->GetGeneration() == 101);
    fixture.Reopen();
    REQUIRE(fixture.loaded.activeBlocks.size() == 100);
}

TEST_CASE("Resume checkpoint batch torn tails and internal corruption keep the existing strict recovery contract", "[application][resume][journal][batch][corruption]")
{
    Fixture fixture;
    const auto initialBytes = fixture.store->GetFileBytes();
    fixture.Accept(100);
    REQUIRE(fixture.store->Checkpoint());
    fixture.store.reset();
    const auto complete = ReadBytes(fixture.journalPath);
    constexpr std::size_t recordBytes = 1685;
    for (const std::size_t retained : {0u, 1u, 15u, 31u, 32u, 56u, 1684u, 1685u, 1686u, 64029u, 64030u, 64031u, 168499u, 168500u})
    {
        CAPTURE(retained);
        fixture.store.reset();
        WriteBytes(fixture.journalPath, std::span<const std::byte>(complete).first(static_cast<std::size_t>(initialBytes) + retained));
        fixture.Reopen();
        REQUIRE(fixture.loaded.hadTruncatedTail == (retained % recordBytes != 0));
        REQUIRE(fixture.loaded.activeBlocks.size() == retained / recordBytes);
        for (std::size_t index = 0; index < fixture.loaded.activeBlocks.size(); index++)
        {
            REQUIRE(fixture.loaded.activeBlocks[index].outerBlockId == index);
            REQUIRE(fixture.loaded.activeBlocks[index].paddedPayload == fixture.block.paddedPayload);
        }
    }
    fixture.store.reset();
    auto corrupt = complete;
    corrupt[static_cast<std::size_t>(initialBytes) + 37 * recordBytes + 55] ^= std::byte{0x20};
    WriteBytes(fixture.journalPath, corrupt);
    const auto rejected = pbapp::DecoderResumeStore::Open(fixture.scratch.GetPath(), fixture.tag, fixture.sessionControl,
        fixture.policy, fixture.store, fixture.loaded);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.message.find("CRC") != std::string::npos);
    REQUIRE_FALSE(fixture.store);
    REQUIRE(ReadBytes(fixture.journalPath) == corrupt);
}

#include "decoder_resume_coalesced_test_cases.inc"
