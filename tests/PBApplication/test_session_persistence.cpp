#include "decoder_resume_store.h"
#include "encoder_session_store.h"

#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/byte_io.h"
#include "pbprotocol/descriptor_codec.h"

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
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
    std::vector<std::byte> rawBytes = MakeBytes(64);
    std::vector<std::byte> sessionControl;
    std::vector<std::byte> segmentControl;

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
    }
};

void AppendTruncatedTail(const std::filesystem::path& path)
{
    std::ofstream output(path, std::ios::binary | std::ios::app);
    REQUIRE(output);
    const std::vector<std::byte> tail = MakeBytes(11);
    output.write(reinterpret_cast<const char*>(tail.data()), static_cast<std::streamsize>(tail.size()));
    REQUIRE(output.good());
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

    AppendTruncatedTail(journalPath);
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
