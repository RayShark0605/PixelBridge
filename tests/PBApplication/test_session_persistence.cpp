#include "decoder_resume_store.h"
#include "encoder_atomic_replace_retry.h"
#include "encoder_session_store.h"

#include "pbreceiver/receiver_ingress.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/byte_io.h"
#include "pbprotocol/descriptor_codec.h"
#include "pbstorage/output_file.h"

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace
{

class ScratchDirectory
{
public:
    explicit ScratchDirectory(const wchar_t* name)
    {
        path_ = std::filesystem::path(PB_TEST_SCRATCH_ROOT) / name;
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        error.clear();
        REQUIRE(std::filesystem::create_directories(path_, error));
        REQUIRE_FALSE(error);
    }

    ~ScratchDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& GetPath() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] pbprotocol::SessionId MakeSessionId(const std::uint8_t seed) noexcept
{
    pbprotocol::SessionId sessionId;
    for (std::size_t index = 0; index < sessionId.bytes.size(); index++)
    {
        sessionId.bytes[index] = static_cast<std::byte>(seed + static_cast<std::uint8_t>(index));
    }
    return sessionId;
}

[[nodiscard]] pbapp::EncoderSourceIdentity MakeSourceIdentity(const std::uint64_t volumeSerialNumber,
    const std::uint8_t fileIdSeed, const std::uint64_t fileBytes, const std::uint64_t lastWriteTime) noexcept
{
    pbapp::EncoderSourceIdentity identity;
    identity.volumeSerialNumber = volumeSerialNumber;
    for (std::size_t index = 0; index < identity.fileId.size(); index++)
    {
        identity.fileId[index] = static_cast<std::byte>(fileIdSeed + static_cast<std::uint8_t>(index));
    }
    identity.fileBytes = fileBytes;
    identity.lastWriteTime = lastWriteTime;
    return identity;
}

[[nodiscard]] std::vector<std::byte> MakeBytes(const std::size_t count)
{
    std::vector<std::byte> bytes(count);
    for (std::size_t index = 0; index < bytes.size(); index++)
    {
        bytes[index] = static_cast<std::byte>((index * 53U + 19U) & 0xFFU);
    }
    return bytes;
}

[[nodiscard]] std::vector<std::byte> ReadAllBytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    REQUIRE(input);
    const std::streamoff byteCount = input.tellg();
    REQUIRE(byteCount >= 0);
    input.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<std::size_t>(byteCount));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(input.good());
    return bytes;
}

class ScopedStateReader final
{
public:
    ScopedStateReader(const std::filesystem::path& path, const DWORD shareMode)
        : handle_(CreateFileW(path.c_str(), GENERIC_READ, shareMode, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr))
    {
        REQUIRE(handle_ != INVALID_HANDLE_VALUE);
    }
    ~ScopedStateReader()
    {
        static_cast<void>(Close());
    }
    ScopedStateReader(const ScopedStateReader&) = delete;
    ScopedStateReader& operator=(const ScopedStateReader&) = delete;
    [[nodiscard]] bool Close() noexcept
    {
        if (handle_ == INVALID_HANDLE_VALUE)
        {
            return true;
        }
        const HANDLE handle = handle_;
        handle_ = INVALID_HANDLE_VALUE;
        return CloseHandle(handle) != FALSE;
    }
private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

[[nodiscard]] pbapp::EncoderSessionStoreCreateConfig MakeReplaceTestConfig(const std::filesystem::path& root)
{
    pbapp::EncoderSessionStoreCreateConfig config;
    config.rootDirectory = root;
    config.sourceIdentity = MakeSourceIdentity(91, 31, 32, 17);
    config.sourcePathUtf8 = "NON_TRANSFER_STATE_REPLACEMENT_TEST";
    config.buildIdentity = "G18 state-store test";
    config.compressionIdentity = "test-only";
    config.outerFecIdentity = "test-only";
    config.sessionId = MakeSessionId(0x80);
    config.segmentCount = 2;
    config.descriptorBundle = MakeBytes(32);
    return config;
}

[[nodiscard]] std::vector<std::byte> WrapControl(const pbprotocol::ControlRecordType recordType,
    const std::uint64_t controlSequence, const pbprotocol::SessionTag sessionTag,
    const std::span<const std::byte> payload)
{
    const pbprotocol::ControlRecordView view{pbprotocol::kControlVersion, recordType,
        controlSequence, sessionTag, payload};
    const auto size = pbprotocol::GetSerializedSize(view);
    REQUIRE(size);
    std::vector<std::byte> bytes(size.Value());
    REQUIRE(pbprotocol::SerializeControlRecord(view, bytes));
    return bytes;
}

struct ResumeFixture
{
    pbprotocol::ReceiverResourcePolicy policy = pbprotocol::GetDefaultReceiverResourcePolicy();
    pbprotocol::SessionDescriptor session;
    pbprotocol::SegmentDescriptor segment;
    pbprotocol::FinalManifest manifest;
    std::vector<std::byte> rawBytes = MakeBytes(64);
    std::vector<std::byte> sessionControl;
    std::vector<std::byte> segmentControl;
    std::vector<std::byte> manifestControl;

    ResumeFixture()
    {
        session.protocolVersion = pbprotocol::GetProtocolVersion();
        session.sessionId = MakeSessionId(0x20);
        session.originalFileSize = rawBytes.size();
        session.segmentCount = 1;
        session.digestAlgorithm = pbprotocol::DigestAlgorithm::Blake3_256;
        session.sessionVisualProfileId = pbprotocol::kUnifiedVisualProfileId;
        session.fileNameUtf8 = "resume.bin";
        const pbprotocol::SessionTag sessionTag = pbprotocol::DeriveSessionTag(session.sessionId);
        const auto digest = pbprotocol::ComputeBlake3Digest(rawBytes);
        segment.sessionTag = sessionTag;
        segment.segmentOrdinal = 0;
        segment.rawOffset = 0;
        segment.rawSize = rawBytes.size();
        segment.encodedSize = rawBytes.size();
        segment.compressionCodec = pbprotocol::CompressionCodec::Raw;
        segment.outerFecMode = pbprotocol::OuterFecMode::DirectRepeat;
        segment.outerBlockBytes = 16;
        segment.rawDigest = pbprotocol::RawDigest{digest};
        segment.encodedDigest = pbprotocol::EncodedDigest{digest};
        manifest.sessionId = session.sessionId;
        manifest.originalFileSize = session.originalFileSize;
        manifest.segmentCount = session.segmentCount;
        manifest.wholeFileDigest = pbprotocol::WholeFileDigest{digest};
        manifest.digestAlgorithm = session.digestAlgorithm;

        const auto sessionSize = pbprotocol::GetSerializedSize(session);
        REQUIRE(sessionSize);
        std::vector<std::byte> sessionPayload(sessionSize.Value());
        REQUIRE(pbprotocol::SerializeSessionDescriptor(session, policy, sessionPayload));
        sessionControl = WrapControl(pbprotocol::ControlRecordType::SessionDescriptor, 1,
            sessionTag, sessionPayload);

        const auto segmentSize = pbprotocol::GetSerializedSize(segment);
        REQUIRE(segmentSize);
        std::vector<std::byte> segmentPayload(segmentSize.Value());
        REQUIRE(pbprotocol::SerializeSegmentDescriptor(segment, session, policy, segmentPayload));
        segmentControl = WrapControl(pbprotocol::ControlRecordType::SegmentDescriptor, 3,
            sessionTag, segmentPayload);

        const auto manifestSize = pbprotocol::GetSerializedSize(manifest);
        REQUIRE(manifestSize != 0);
        std::vector<std::byte> manifestPayload(manifestSize);
        REQUIRE(pbprotocol::SerializeFinalManifest(manifest, session, policy, manifestPayload));
        manifestControl = WrapControl(pbprotocol::ControlRecordType::FinalManifest, 2,
            sessionTag, manifestPayload);
    }
};

[[nodiscard]] std::size_t FindLastJournalRecordOffset(const std::vector<std::byte>& bytes)
{
    const std::array<std::byte, 4> recordMagic{
        std::byte{'P'}, std::byte{'B'}, std::byte{'J'}, std::byte{'R'}};
    const auto position = std::find_end(bytes.begin(), bytes.end(), recordMagic.begin(), recordMagic.end());
    REQUIRE(position != bytes.end());
    return static_cast<std::size_t>(std::distance(bytes.begin(), position));
}

void AppendTruncatedRecordPrefix(const std::filesystem::path& path)
{
    const std::vector<std::byte> bytes = ReadAllBytes(path);
    const std::size_t recordOffset = FindLastJournalRecordOffset(bytes);
    constexpr std::size_t prefixBytes = 11;
    REQUIRE(recordOffset + prefixBytes <= bytes.size());
    std::ofstream output(path, std::ios::binary | std::ios::app);
    REQUIRE(output);
    output.write(reinterpret_cast<const char*>(bytes.data() + recordOffset),
        static_cast<std::streamsize>(prefixBytes));
    REQUIRE(output.good());
}

void AppendAmbiguousTail(const std::filesystem::path& path)
{
    std::ofstream output(path, std::ios::binary | std::ios::app);
    REQUIRE(output);
    const std::vector<std::byte> tail = MakeBytes(11);
    output.write(reinterpret_cast<const char*>(tail.data()), static_cast<std::streamsize>(tail.size()));
    REQUIRE(output.good());
}

void InflateLastJournalRecordLength(const std::filesystem::path& path)
{
    const std::vector<std::byte> bytes = ReadAllBytes(path);
    const std::size_t recordOffset = FindLastJournalRecordOffset(bytes);
    const std::array<char, 4> invalidTotalBytes{
        static_cast<char>(0xFF), static_cast<char>(0xFF), static_cast<char>(0xFF), static_cast<char>(0x7F)};
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    REQUIRE(file);
    file.seekp(static_cast<std::streamoff>(recordOffset + 8), std::ios::beg);
    file.write(invalidTotalBytes.data(), static_cast<std::streamsize>(invalidTotalBytes.size()));
    REQUIRE(file.good());
}

void CorruptLastByte(const std::filesystem::path& path)
{
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    REQUIRE(file);
    file.seekg(-1, std::ios::end);
    char value = 0;
    file.read(&value, 1);
    REQUIRE(file.good());
    value = static_cast<char>(static_cast<unsigned char>(value) ^ 0x80U);
    file.seekp(-1, std::ios::end);
    file.write(&value, 1);
    REQUIRE(file.good());
}

} // namespace

TEST_CASE("Encoder session store leases IDs durably and resumes only exact source and codec identities",
    "[application][encoder][sender][session][persistence][resume][durable-lease]")
{
    ScratchDirectory scratch(L"encoder-session-store");
    pbapp::EncoderSessionStoreCreateConfig config;
    config.rootDirectory = scratch.GetPath();
    config.sourceIdentity = MakeSourceIdentity(0x0123456789ABCDEFULL, 0x30, 17,
        0x0102030405060708ULL);
    config.sourcePathUtf8 = "D:/fixtures/source.bin";
    config.buildIdentity = "PixelBridge-0.1.0";
    config.compressionIdentity = "zstd-1.5.7";
    config.outerFecIdentity = "wirehair-v2-profile-1;outer-block-1314";
    config.sessionId = MakeSessionId(0x40);
    config.segmentCount = 2;
    config.descriptorBundle = MakeBytes(4096);

    std::unique_ptr<pbapp::EncoderSessionStore> store;
    REQUIRE(pbapp::EncoderSessionStore::Create(config, store));
    REQUIRE(store);
    REQUIRE_FALSE(store->WasResumed());
    REQUIRE(store->GetFrameSequenceStart() == 0);
    REQUIRE(store->EnsureFrameSequenceLease(1));
    REQUIRE(store->GetFrameSequenceLeaseEnd() == pbapp::encoderDurableIdLeaseSize);
    REQUIRE(store->EnsureFrameSequenceLease(pbapp::encoderDurableIdLeaseSize));
    REQUIRE(store->EnsureRepairIdLease(1, 301));
    REQUIRE(store->GetRepairIdLeaseEnd(1) == pbapp::encoderDurableIdLeaseSize);
    REQUIRE(store->UpdateCarouselPosition(7, 1));
    const std::vector<std::byte> descriptorStateBytes = ReadAllBytes(
        store->GetSessionDirectory() / L"descriptors.bin");
    pbprotocol::ByteReader descriptorReader(descriptorStateBytes);
    REQUIRE(descriptorReader.ReadFixedBytes<4>().Value() == std::array<std::byte, 4>{
        std::byte{'P'}, std::byte{'B'}, std::byte{'E'}, std::byte{'D'}});
    REQUIRE(descriptorReader.ReadUint16().Value() == 2);
    REQUIRE(descriptorReader.ReadUint16().Value() == 0);
    REQUIRE(descriptorReader.ReadUint64().Value() == descriptorStateBytes.size());
    REQUIRE(descriptorReader.ReadFixedBytes<pbprotocol::kSessionIdBytes>().Value() == config.sessionId.bytes);
    REQUIRE(descriptorReader.ReadUint64().Value() == config.sourceIdentity.volumeSerialNumber);
    REQUIRE(descriptorReader.ReadFixedBytes<16>().Value() == config.sourceIdentity.fileId);
    REQUIRE(descriptorReader.ReadUint64().Value() == config.sourceIdentity.fileBytes);
    REQUIRE(descriptorReader.ReadUint64().Value() == config.sourceIdentity.lastWriteTime);
    REQUIRE(descriptorReader.ReadUint64().Value() == config.segmentCount);
    REQUIRE(descriptorReader.ReadUint32().Value() == config.sourcePathUtf8.size());
    REQUIRE(descriptorReader.ReadUint32().Value() == config.buildIdentity.size());
    REQUIRE(descriptorReader.ReadUint32().Value() == config.compressionIdentity.size());
    REQUIRE(descriptorReader.ReadUint32().Value() == config.outerFecIdentity.size());
    REQUIRE(descriptorReader.ReadUint64().Value() == config.descriptorBundle.size());
    REQUIRE(descriptorReader.Position() == 104);
    const std::filesystem::path runtimePath = store->GetSessionDirectory() / L"runtime.state";

    std::unique_ptr<pbapp::EncoderSessionStore> collision;
    REQUIRE_FALSE(pbapp::EncoderSessionStore::Create(config, collision));
    REQUIRE_FALSE(collision);
    store.reset();

    bool found = false;
    REQUIRE(pbapp::EncoderSessionStore::FindMatching(config.rootDirectory, config.sourceIdentity,
        config.buildIdentity, config.compressionIdentity, config.outerFecIdentity, store, found));
    REQUIRE(found);
    REQUIRE(store);
    REQUIRE(store->WasResumed());
    REQUIRE(store->GetSessionId() == config.sessionId);
    REQUIRE(store->MatchesDescriptorBundle(config.descriptorBundle));
    REQUIRE(store->GetFrameSequenceStart() == pbapp::encoderDurableIdLeaseSize);
    REQUIRE(store->GetRepairIdStart(1, 301) == pbapp::encoderDurableIdLeaseSize);
    REQUIRE(store->GetCarouselPass() == 7);
    REQUIRE(store->GetSegmentOrdinal() == 1);
    store.reset();

    REQUIRE(pbapp::EncoderSessionStore::FindMatching(config.rootDirectory, config.sourceIdentity,
        "different-build", config.compressionIdentity, config.outerFecIdentity, store, found));
    REQUIRE_FALSE(found);
    REQUIRE_FALSE(store);

    REQUIRE(pbapp::EncoderSessionStore::FindMatching(config.rootDirectory, config.sourceIdentity,
        config.buildIdentity, "different-compression", config.outerFecIdentity, store, found));
    REQUIRE_FALSE(found);
    REQUIRE_FALSE(store);

    REQUIRE(pbapp::EncoderSessionStore::FindMatching(config.rootDirectory, config.sourceIdentity,
        config.buildIdentity, config.compressionIdentity, "different-outer-fec", store, found));
    REQUIRE_FALSE(found);
    REQUIRE_FALSE(store);

    pbapp::EncoderSourceIdentity changedSource = config.sourceIdentity;
    changedSource.fileBytes++;
    REQUIRE(pbapp::EncoderSessionStore::FindMatching(config.rootDirectory, changedSource,
        config.buildIdentity, config.compressionIdentity, config.outerFecIdentity, store, found));
    REQUIRE_FALSE(found);
    REQUIRE_FALSE(store);

    changedSource = config.sourceIdentity;
    changedSource.lastWriteTime++;
    REQUIRE(pbapp::EncoderSessionStore::FindMatching(config.rootDirectory, changedSource,
        config.buildIdentity, config.compressionIdentity, config.outerFecIdentity, store, found));
    REQUIRE_FALSE(found);
    REQUIRE_FALSE(store);

    CorruptLastByte(runtimePath);
    REQUIRE_FALSE(pbapp::EncoderSessionStore::FindMatching(config.rootDirectory, config.sourceIdentity,
        config.buildIdentity, config.compressionIdentity, config.outerFecIdentity, store, found));
    REQUIRE_FALSE(found);
    REQUIRE_FALSE(store);
}

TEST_CASE("Encoder atomic replacement retry policy is bounded and preserves the decisive error",
    "[application][encoder][durable-lease][atomic-replace][g18]")
{
    std::vector<DWORD> errors{ERROR_SUCCESS};
    std::uint64_t now = 0;
    DWORD waitStep = 25;
    std::uint64_t operationTime = 0;
    std::uint32_t expectedAttempts = 1;
    std::uint32_t expectedWaits = 0;
    SECTION("normal replacement performs no wait")
    {
    }
    SECTION("only access denied and sharing violation can recover")
    {
        errors = {ERROR_ACCESS_DENIED, ERROR_SHARING_VIOLATION, ERROR_SUCCESS};
        expectedAttempts = 3;
        expectedWaits = 2;
    }
    SECTION("a nonretryable initial failure stops immediately")
    {
        errors = {ERROR_DISK_FULL};
    }
    SECTION("a nonretryable later failure preserves the last error")
    {
        errors = {ERROR_ACCESS_DENIED, ERROR_PATH_NOT_FOUND};
        expectedAttempts = 2;
        expectedWaits = 1;
    }
    SECTION("persistent access denial exhausts the time budget")
    {
        errors = {ERROR_ACCESS_DENIED};
        expectedAttempts = 10;
        expectedWaits = 10;
    }
    SECTION("a stalled clock still exhausts the attempt budget")
    {
        errors = {ERROR_SHARING_VIOLATION};
        waitStep = 0;
        expectedAttempts = 11;
        expectedWaits = 10;
    }
    SECTION("scheduler oversleep cannot start a late retry")
    {
        errors = {ERROR_ACCESS_DENIED};
        waitStep = 500;
        expectedWaits = 1;
    }
    SECTION("time spent in the system call counts against retries")
    {
        errors = {ERROR_ACCESS_DENIED};
        operationTime = 300;
    }
    std::size_t calls = 0;
    std::uint32_t waits = 0;
    std::uint64_t requestedWaitMilliseconds = 0;
    const auto result = pbapp::detail::RetryEncoderStateReplace([&]() noexcept
    {
        now += operationTime;
        const DWORD error = errors[(std::min)(calls, errors.size() - 1)];
        calls++;
        return error;
    }, [&]() noexcept { return now; }, [&](const DWORD milliseconds) noexcept
    {
        waits++;
        requestedWaitMilliseconds += milliseconds;
        now += waitStep;
    });
    REQUIRE(result.attempts == expectedAttempts);
    REQUIRE(calls == expectedAttempts);
    REQUIRE(waits == expectedWaits);
    REQUIRE(result.firstError == errors.front());
    REQUIRE(result.error == errors.back());
    REQUIRE(result.elapsedMilliseconds == now);
    REQUIRE(requestedWaitMilliseconds <= pbapp::detail::encoderReplaceBudgetMilliseconds);
}

TEST_CASE("Encoder retries the same flushed temporary file after a controlled native rename denial",
    "[application][encoder][durable-lease][atomic-replace][g18]")
{
    ScratchDirectory scratch(L"encoder-replace-native");
    const auto config = MakeReplaceTestConfig(scratch.GetPath());
    std::unique_ptr<pbapp::EncoderSessionStore> store;
    REQUIRE(pbapp::EncoderSessionStore::Create(config, store));
    const auto target = store->GetSessionDirectory() / L"runtime.state";
    const auto temporary = store->GetSessionDirectory() / L"runtime.state.tmp";
    const auto oldBytes = ReadAllBytes(target);
    const auto newBytes = MakeBytes(47);
    const HANDLE temporaryFile = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    REQUIRE(temporaryFile != INVALID_HANDLE_VALUE);
    DWORD writtenBytes = 0;
    const bool written = WriteFile(temporaryFile, newBytes.data(), static_cast<DWORD>(newBytes.size()), &writtenBytes, nullptr) != FALSE;
    const bool flushed = FlushFileBuffers(temporaryFile) != FALSE;
    const bool closed = CloseHandle(temporaryFile) != FALSE;
    REQUIRE(written);
    REQUIRE(writtenBytes == newBytes.size());
    REQUIRE(flushed);
    REQUIRE(closed);
    ScopedStateReader reader(target, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
    DWORD firstNativeError = ERROR_SUCCESS;
    std::uint32_t attempts = 0;
    std::uint64_t now = 0;
    bool released = false;
    const auto result = pbapp::detail::RetryEncoderStateReplace([&]() noexcept
    {
        const DWORD error = MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ?
            ERROR_SUCCESS : GetLastError();
        if (attempts == 0)
        {
            firstNativeError = error;
        }
        attempts++;
        return error;
    }, [&]() noexcept { return now; }, [&](const DWORD milliseconds) noexcept
    {
        released = reader.Close();
        now += milliseconds;
    });
    REQUIRE(firstNativeError == ERROR_ACCESS_DENIED);
    REQUIRE(released);
    REQUIRE(result.error == ERROR_SUCCESS);
    REQUIRE(result.attempts == 2);
    REQUIRE(ReadAllBytes(target) == newBytes);
    REQUIRE_FALSE(std::filesystem::exists(temporary));
    REQUIRE(oldBytes != newBytes);
}

TEST_CASE("Encoder state persistence survives a short target reader without spending a second generation",
    "[application][encoder][durable-lease][atomic-replace][transient-reader][g18]")
{
    ScratchDirectory scratch(L"encoder-replace-transient");
    const auto config = MakeReplaceTestConfig(scratch.GetPath());
    std::unique_ptr<pbapp::EncoderSessionStore> store;
    REQUIRE(pbapp::EncoderSessionStore::Create(config, store));
    REQUIRE(store->EnsureRepairIdLease(1, 8192));
    const auto target = store->GetSessionDirectory() / L"runtime.state";
    const auto generation = store->GetGeneration();
    ScopedStateReader reader(target, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
    bool released = false;
    std::jthread releaser([&]() noexcept
    {
        Sleep(50);
        released = reader.Close();
    });
    const auto status = store->UpdateCarouselPosition(3, 1);
    releaser.join();
    REQUIRE(released);
    INFO(status.message);
    REQUIRE(status);
    REQUIRE(store->GetGeneration() == generation + 1);
    REQUIRE(store->GetRepairIdLeaseEnd(1) == 8192);
    REQUIRE_FALSE(std::filesystem::exists(target.wstring() + L".tmp"));
    store.reset();
    bool found = false;
    REQUIRE(pbapp::EncoderSessionStore::FindMatching(config.rootDirectory, config.sourceIdentity,
        config.buildIdentity, config.compressionIdentity, config.outerFecIdentity, store, found));
    REQUIRE(found);
    REQUIRE(store->GetGeneration() == generation + 1);
    REQUIRE(store->GetCarouselPass() == 3);
    REQUIRE(store->GetSegmentOrdinal() == 1);
    REQUIRE(store->GetRepairIdStart(1, 32) == 8192);
}

TEST_CASE("Encoder persistent replacement denial rolls back memory and retains exact durable leases",
    "[application][encoder][durable-lease][atomic-replace][g18]")
{
    ScratchDirectory scratch(L"encoder-replace-permanent");
    const auto config = MakeReplaceTestConfig(scratch.GetPath());
    std::unique_ptr<pbapp::EncoderSessionStore> store;
    REQUIRE(pbapp::EncoderSessionStore::Create(config, store));
    REQUIRE(store->EnsureFrameSequenceLease(4096));
    REQUIRE(store->EnsureRepairIdLease(1, 8192));
    const auto generation = store->GetGeneration();
    const auto target = store->GetSessionDirectory() / L"runtime.state";
    const auto oldBytes = ReadAllBytes(target);
    ScopedStateReader reader(target, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
    pbapp::EncoderSessionStoreStatus status;
    const auto started = GetTickCount64();
    SECTION("frame lease")
    {
        status = store->EnsureFrameSequenceLease(8192);
    }
    SECTION("repair lease")
    {
        status = store->EnsureRepairIdLease(1, 12288);
    }
    SECTION("position after already committed repair lease")
    {
        status = store->UpdateCarouselPosition(3, 1);
    }
    const auto elapsed = GetTickCount64() - started;
    REQUIRE(reader.Close());
    INFO(status.message);
    REQUIRE_FALSE(status);
    REQUIRE(status.message.find("session state atomic replace failed; win32=5") != std::string::npos);
    REQUIRE(elapsed < 3000);
    REQUIRE(store->GetGeneration() == generation);
    REQUIRE(store->GetFrameSequenceLeaseEnd() == 4096);
    REQUIRE(store->GetRepairIdLeaseEnd(1) == 8192);
    REQUIRE(store->GetCarouselPass() == 0);
    REQUIRE(store->GetSegmentOrdinal() == 0);
    REQUIRE(ReadAllBytes(target) == oldBytes);
    REQUIRE_FALSE(std::filesystem::exists(target.wstring() + L".tmp"));
    store.reset();
    bool found = false;
    REQUIRE(pbapp::EncoderSessionStore::FindMatching(config.rootDirectory, config.sourceIdentity,
        config.buildIdentity, config.compressionIdentity, config.outerFecIdentity, store, found));
    REQUIRE(found);
    REQUIRE(store->GetGeneration() == generation);
    REQUIRE(store->GetFrameSequenceStart() == 4096);
    REQUIRE(store->GetRepairIdStart(1, 32) == 8192);
    REQUIRE(store->GetCarouselPass() == 0);
    REQUIRE(store->GetSegmentOrdinal() == 0);
}

TEST_CASE("Decoder resume journal repairs only a torn tail and compacts completed Segment state",
    "[application][decoder][resume][journal]")
{
    ScratchDirectory scratch(L"decoder-resume-store");
    ResumeFixture fixture;
    const pbprotocol::SessionTag sessionTag = pbprotocol::DeriveSessionTag(fixture.session.sessionId);
    pbapp::DecoderResumeLoadedState loaded;
    std::unique_ptr<pbapp::DecoderResumeStore> store;
    REQUIRE(pbapp::DecoderResumeStore::Open(scratch.GetPath(), sessionTag, fixture.sessionControl,
        fixture.policy, store, loaded));
    REQUIRE(store);
    REQUIRE_FALSE(loaded.resumed);
    REQUIRE(loaded.segmentControlRecords.empty());

    pbapp::DecoderResumeAcceptedBlock block;
    block.segmentOrdinal = 0;
    block.outerBlockId = 0;
    block.declaredPayloadBytes = 16;
    block.paddedPayload.assign(fixture.rawBytes.begin(), fixture.rawBytes.begin() + 16);
    REQUIRE_FALSE(store->RecordAcceptedBlock(block));
    REQUIRE(store->RecordSegmentControl(fixture.segmentControl));
    const std::vector<std::byte> equivalentSegmentControl = WrapControl(
        pbprotocol::ControlRecordType::SegmentDescriptor, 99, sessionTag,
        pbprotocol::ParseControlRecord(fixture.segmentControl).Value().payload);
    REQUIRE(store->RecordSegmentControl(equivalentSegmentControl));
    REQUIRE(store->RecordAcceptedBlock(block));
    REQUIRE(store->RecordAcceptedBlock(block));
    pbapp::DecoderResumeAcceptedBlock conflictingBlock = block;
    conflictingBlock.paddedPayload[0] ^= std::byte{0x80};
    REQUIRE_FALSE(store->RecordAcceptedBlock(conflictingBlock));
    REQUIRE(store->GetPendingBlockCount() == 1);
    REQUIRE(store->Checkpoint());
    REQUIRE(store->GetPendingBlockCount() == 0);
    const std::filesystem::path journalPath = store->GetPath();
    const std::uint64_t generationBeforeRestart = store->GetGeneration();
    store.reset();

    AppendTruncatedRecordPrefix(journalPath);
    REQUIRE(pbapp::DecoderResumeStore::Open(scratch.GetPath(), sessionTag, fixture.sessionControl,
        fixture.policy, store, loaded));
    REQUIRE(loaded.resumed);
    REQUIRE(loaded.hadTruncatedTail);
    REQUIRE(loaded.segmentControlRecords.size() == 1);
    REQUIRE(loaded.activeBlocks.size() == 1);
    REQUIRE(loaded.activeBlocks.front() == block);
    REQUIRE(store->GetGeneration() > generationBeforeRestart);

    pbprotocol::ResumeCompletedSegmentRecord completed;
    completed.sessionId = fixture.session.sessionId;
    completed.segmentOrdinal = fixture.segment.segmentOrdinal;
    completed.rawOffset = fixture.segment.rawOffset;
    completed.rawSize = fixture.segment.rawSize;
    completed.rawDigest = fixture.segment.rawDigest;
    auto invalidCompleted = completed;
    invalidCompleted.rawSize--;
    REQUIRE_FALSE(store->RecordCompletedSegment(invalidCompleted));
    REQUIRE(store->RecordCompletedSegment(completed));
    store.reset();

    REQUIRE(pbapp::DecoderResumeStore::Open(scratch.GetPath(), sessionTag, fixture.sessionControl,
        fixture.policy, store, loaded));
    REQUIRE(loaded.resumed);
    REQUIRE_FALSE(loaded.hadTruncatedTail);
    REQUIRE(loaded.completedSegments.size() == 1);
    REQUIRE(loaded.completedSegments.front() == completed);
    REQUIRE(loaded.activeBlocks.empty());
    REQUIRE(store->RemoveAfterPublish());
    REQUIRE_FALSE(std::filesystem::exists(journalPath));
}

TEST_CASE("Decoder resume journal rejects ambiguous tails and internally corrupted records",
    "[application][decoder][resume][journal][corruption]")
{
    ScratchDirectory scratch(L"decoder-resume-corruption");
    ResumeFixture fixture;
    const pbprotocol::SessionTag sessionTag = pbprotocol::DeriveSessionTag(fixture.session.sessionId);
    pbapp::DecoderResumeLoadedState loaded;
    std::unique_ptr<pbapp::DecoderResumeStore> store;
    REQUIRE(pbapp::DecoderResumeStore::Open(scratch.GetPath(), sessionTag, fixture.sessionControl,
        fixture.policy, store, loaded));
    REQUIRE(store->RecordSegmentControl(fixture.segmentControl));
    pbapp::DecoderResumeAcceptedBlock block;
    block.segmentOrdinal = 0;
    block.outerBlockId = 0;
    block.declaredPayloadBytes = 16;
    block.paddedPayload.assign(fixture.rawBytes.begin(), fixture.rawBytes.begin() + 16);
    REQUIRE(store->RecordAcceptedBlock(block));
    REQUIRE(store->Checkpoint());
    const std::filesystem::path journalPath = store->GetPath();
    store.reset();

    SECTION("arbitrary bytes are not a clearly torn record")
    {
        AppendAmbiguousTail(journalPath);
        const pbapp::DecoderResumeStoreStatus status = pbapp::DecoderResumeStore::Open(scratch.GetPath(),
            sessionTag, fixture.sessionControl, fixture.policy, store, loaded);
        REQUIRE_FALSE(status);
        REQUIRE(status.message == "resume journal tail is not a valid truncated record prefix");
        REQUIRE_FALSE(store);
    }

    SECTION("an inflated complete record header is internal length corruption")
    {
        InflateLastJournalRecordLength(journalPath);
        const pbapp::DecoderResumeStoreStatus status = pbapp::DecoderResumeStore::Open(scratch.GetPath(),
            sessionTag, fixture.sessionControl, fixture.policy, store, loaded);
        REQUIRE_FALSE(status);
        REQUIRE(status.message == "resume journal record header is invalid or non-monotonic");
        REQUIRE_FALSE(store);
    }

    SECTION("a complete record with a bad CRC is rejected")
    {
        CorruptLastByte(journalPath);
        const pbapp::DecoderResumeStoreStatus status = pbapp::DecoderResumeStore::Open(scratch.GetPath(),
            sessionTag, fixture.sessionControl, fixture.policy, store, loaded);
        REQUIRE_FALSE(status);
        REQUIRE(status.message == "resume journal internal record CRC is invalid");
        REQUIRE_FALSE(store);
    }
}

TEST_CASE("Decoder resume journal rejects active cache state above the restart decoder budget",
    "[application][decoder][resume][journal][quota]")
{
    ScratchDirectory scratch(L"decoder-resume-active-quota");
    ResumeFixture fixture;
    fixture.policy.maxActiveOuterFecDecoders = 2;
    fixture.session.originalFileSize = fixture.rawBytes.size() * 2ULL;
    fixture.session.segmentCount = 2;
    const pbprotocol::SessionTag sessionTag = pbprotocol::DeriveSessionTag(fixture.session.sessionId);
    const auto sessionSize = pbprotocol::GetSerializedSize(fixture.session);
    REQUIRE(sessionSize);
    std::vector<std::byte> sessionPayload(sessionSize.Value());
    REQUIRE(pbprotocol::SerializeSessionDescriptor(fixture.session, fixture.policy, sessionPayload));
    fixture.sessionControl = WrapControl(pbprotocol::ControlRecordType::SessionDescriptor, 1,
        sessionTag, sessionPayload);

    std::array<pbprotocol::SegmentDescriptor, 2> descriptors{fixture.segment, fixture.segment};
    descriptors[0].rawOffset = 0;
    descriptors[1].segmentOrdinal = 1;
    descriptors[1].rawOffset = fixture.rawBytes.size();
    std::array<std::vector<std::byte>, 2> segmentControls;
    for (std::size_t segmentIndex = 0; segmentIndex < descriptors.size(); segmentIndex++)
    {
        const auto descriptorSize = pbprotocol::GetSerializedSize(descriptors[segmentIndex]);
        REQUIRE(descriptorSize);
        std::vector<std::byte> descriptorPayload(descriptorSize.Value());
        REQUIRE(pbprotocol::SerializeSegmentDescriptor(descriptors[segmentIndex], fixture.session,
            fixture.policy, descriptorPayload));
        segmentControls[segmentIndex] = WrapControl(pbprotocol::ControlRecordType::SegmentDescriptor,
            2 + segmentIndex, sessionTag, descriptorPayload);
    }

    pbapp::DecoderResumeLoadedState loaded;
    std::unique_ptr<pbapp::DecoderResumeStore> store;
    REQUIRE(pbapp::DecoderResumeStore::Open(scratch.GetPath(), sessionTag, fixture.sessionControl,
        fixture.policy, store, loaded));
    for (std::size_t segmentIndex = 0; segmentIndex < segmentControls.size(); segmentIndex++)
    {
        REQUIRE(store->RecordSegmentControl(segmentControls[segmentIndex]));
        pbapp::DecoderResumeAcceptedBlock block;
        block.segmentOrdinal = segmentIndex;
        block.outerBlockId = 0;
        block.declaredPayloadBytes = 16;
        block.paddedPayload.assign(fixture.rawBytes.begin(), fixture.rawBytes.begin() + 16);
        REQUIRE(store->RecordAcceptedBlock(block));
    }
    REQUIRE(store->Checkpoint());
    store.reset();

    pbprotocol::ReceiverResourcePolicy restartPolicy = fixture.policy;
    restartPolicy.maxActiveOuterFecDecoders = 1;
    const pbapp::DecoderResumeStoreStatus restartStatus = pbapp::DecoderResumeStore::Open(scratch.GetPath(),
        sessionTag, fixture.sessionControl, restartPolicy, store, loaded);
    REQUIRE_FALSE(restartStatus);
    REQUIRE(restartStatus.message == "resume journal exceeds the active Segment limit");
    REQUIRE_FALSE(store);
}

TEST_CASE("Decoder restart revalidates completed part bytes before storage adoption",
    "[application][decoder][resume][restart][raw-digest]")
{
    ScratchDirectory scratch(L"decoder-resume-completed-revalidation");
    ResumeFixture fixture;
    const pbprotocol::SessionTag sessionTag = pbprotocol::DeriveSessionTag(fixture.session.sessionId);
    pbstorage::OutputFileConfig storageConfig;
    storageConfig.outputDirectory = scratch.GetPath().wstring();
    storageConfig.sessionTag = sessionTag;
    storageConfig.fileBytes = fixture.session.originalFileSize;
    storageConfig.maximumFileBytes = fixture.policy.maxAcceptedFileBytes;
    storageConfig.originalFileNameUtf8 = fixture.session.fileNameUtf8;

    std::unique_ptr<pbstorage::OutputFile> storage;
    REQUIRE(pbstorage::OutputFile::CreateOrResume(storageConfig, storage));
    const std::wstring partFileName = std::filesystem::path(storage->GetSnapshot().partPath).filename().wstring();
    REQUIRE(partFileName.starts_with(L"PixelBridge-"));
    REQUIRE(partFileName.ends_with(L".part"));
    pbapp::DecoderResumeLoadedState loaded;
    std::unique_ptr<pbapp::DecoderResumeStore> store;
    REQUIRE(pbapp::DecoderResumeStore::Open(scratch.GetPath(), sessionTag, fixture.sessionControl,
        fixture.policy, store, loaded));
    REQUIRE(store->RecordSegmentControl(fixture.segmentControl));
    REQUIRE(storage->WriteVerifiedSegment(fixture.segment.rawOffset, fixture.rawBytes));
    REQUIRE(storage->FlushVerifiedSegment());
    REQUIRE(storage->Checkpoint());
    pbprotocol::ResumeCompletedSegmentRecord completed;
    completed.sessionId = fixture.session.sessionId;
    completed.segmentOrdinal = fixture.segment.segmentOrdinal;
    completed.rawOffset = fixture.segment.rawOffset;
    completed.rawSize = fixture.segment.rawSize;
    completed.rawDigest = fixture.segment.rawDigest;
    REQUIRE(store->RecordCompletedSegment(completed));
    const std::filesystem::path partPath = storage->GetSnapshot().partPath;
    storage.reset();
    store.reset();

    bool expectValidResume = true;
    SECTION("matching completed bytes are adopted")
    {
        REQUIRE(std::filesystem::exists(partPath));
    }
    SECTION("corrupted completed bytes are rejected")
    {
        CorruptLastByte(partPath);
        expectValidResume = false;
    }

    REQUIRE(pbstorage::OutputFile::CreateOrResume(storageConfig, storage));
    REQUIRE(storage->GetSnapshot().resumed);
    REQUIRE(pbapp::DecoderResumeStore::Open(scratch.GetPath(), sessionTag, fixture.sessionControl,
        fixture.policy, store, loaded));
    REQUIRE(loaded.resumed);
    REQUIRE(loaded.completedSegments.size() == 1);
    auto receiverResult = pbreceiver::ReceiverIngress::Create(fixture.policy, fixture.segment.outerBlockBytes);
    REQUIRE(receiverResult);
    pbreceiver::ReceiverIngress receiver = std::move(receiverResult).Value();
    REQUIRE(receiver.ReceiveControlRecord(fixture.sessionControl));
    REQUIRE(receiver.ReceiveControlRecord(fixture.segmentControl));
    std::vector<std::byte> storedRawBytes(static_cast<std::size_t>(completed.rawSize));
    REQUIRE(storage->ReadRange(completed.rawOffset, storedRawBytes));
    auto verified = receiver.VerifyResumedStoredSegment(completed, std::move(storedRawBytes));
    if (expectValidResume)
    {
        REQUIRE(verified);
        REQUIRE(storage->AdoptVerifiedSegment(completed.rawOffset, completed.rawSize));
        const auto committed = receiver.CommitStoredSegment(std::move(verified).Value());
        REQUIRE(committed);
        REQUIRE(committed.Value() == pbreceiver::ReceiverSegmentCommitDisposition::Committed);
        REQUIRE(storage->GetSnapshot().verifiedBytes == completed.rawSize);
    }
    else
    {
        REQUIRE_FALSE(verified);
        REQUIRE(std::holds_alternative<pbprotocol::ProtocolError>(verified.Error()));
        REQUIRE(std::get<pbprotocol::ProtocolError>(verified.Error()).code ==
            pbprotocol::ProtocolErrorCode::DigestMismatch);
        REQUIRE(storage->GetSnapshot().verifiedBytes == 0);
    }
}

TEST_CASE("Decoder durable publish intent recovers the rename-before-journal-delete crash window",
    "[application][decoder][resume][publish-recovery][g05]")
{
    ScratchDirectory scratch(L"decoder-post-rename-recovery");
    ResumeFixture fixture;
    const pbprotocol::SessionTag sessionTag = pbprotocol::DeriveSessionTag(fixture.session.sessionId);
    pbstorage::OutputFileConfig storageConfig;
    storageConfig.outputDirectory = scratch.GetPath().wstring();
    storageConfig.sessionTag = sessionTag;
    storageConfig.fileBytes = fixture.session.originalFileSize;
    storageConfig.maximumFileBytes = fixture.policy.maxAcceptedFileBytes;
    storageConfig.originalFileNameUtf8 = fixture.session.fileNameUtf8;
    pbstorage::OutputFileReservation reservation;
    REQUIRE(pbstorage::OutputFile::PlanReservation(storageConfig, reservation));

    pbapp::DecoderResumeLoadedState loaded;
    std::unique_ptr<pbapp::DecoderResumeStore> store;
    REQUIRE(pbapp::DecoderResumeStore::Open(scratch.GetPath(), sessionTag, fixture.sessionControl,
        fixture.policy, store, loaded));
    REQUIRE_FALSE(store->RecordPublishIntent(fixture.manifest.wholeFileDigest));
    REQUIRE(store->RecordOutputReservation(reservation.finalFileNameUtf8));
    REQUIRE(store->RecordOutputReservation(reservation.finalFileNameUtf8));
    REQUIRE(store->RecordSegmentControl(fixture.segmentControl));
    REQUIRE(store->RecordManifestControl(fixture.manifestControl));

    std::unique_ptr<pbstorage::OutputFile> storage;
    REQUIRE(pbstorage::OutputFile::CreateOrResume(storageConfig, reservation, std::nullopt, storage));
    REQUIRE(storage->WriteVerifiedSegment(fixture.segment.rawOffset, fixture.rawBytes));
    REQUIRE(storage->FlushVerifiedSegment());
    REQUIRE(storage->Checkpoint());
    pbprotocol::ResumeCompletedSegmentRecord completed;
    completed.sessionId = fixture.session.sessionId;
    completed.segmentOrdinal = fixture.segment.segmentOrdinal;
    completed.rawOffset = fixture.segment.rawOffset;
    completed.rawSize = fixture.segment.rawSize;
    completed.rawDigest = fixture.segment.rawDigest;
    REQUIRE(store->RecordCompletedSegment(completed));
    REQUIRE(store->RecordPublishIntent(fixture.manifest.wholeFileDigest));
    REQUIRE(store->RecordPublishIntent(fixture.manifest.wholeFileDigest));
    const std::filesystem::path journalPath = store->GetPath();
    const std::wstring finalPath = storage->GetSnapshot().finalPath;
    const std::wstring partPath = storage->GetSnapshot().partPath;
    REQUIRE(storage->Publish(fixture.manifest.wholeFileDigest));
    REQUIRE(std::filesystem::exists(finalPath));
    REQUIRE_FALSE(std::filesystem::exists(partPath));
    storage.reset();
    store.reset();

    REQUIRE(pbapp::DecoderResumeStore::Open(scratch.GetPath(), sessionTag, fixture.sessionControl,
        fixture.policy, store, loaded));
    REQUIRE(loaded.resumed);
    REQUIRE(loaded.outputReservationFileNameUtf8 == reservation.finalFileNameUtf8);
    REQUIRE(loaded.publishIntent == fixture.manifest.wholeFileDigest);
    REQUIRE(loaded.completedSegments.size() == 1);
    REQUIRE(loaded.manifestControlRecord == fixture.manifestControl);
    REQUIRE(pbstorage::OutputFile::CreateOrResume(storageConfig, reservation, loaded.publishIntent, storage));
    REQUIRE(storage->GetSnapshot().published);
    REQUIRE(storage->GetSnapshot().recoveredPublished);
    REQUIRE(storage->Publish(fixture.manifest.wholeFileDigest));
    REQUIRE(store->RemoveAfterPublish());
    REQUIRE_FALSE(std::filesystem::exists(journalPath));
    REQUIRE(std::filesystem::exists(finalPath));
    REQUIRE_FALSE(std::filesystem::exists(partPath));
}
