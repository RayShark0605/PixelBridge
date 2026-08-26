#include "pbreceiver/resume_replay.h"

#include "pbouterfec/direct_repeat.h"
#include "pbouterfec/wirehair_v2.h"

#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/control_fragment_codec.h"
#include "pbprotocol/control_plane_receiver.h"
#include "pbprotocol/descriptor_codec.h"
#include "pbprotocol/protocol_types.h"
#include "pbprotocol/resume_state.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <utility>
#include <vector>

namespace {

// Crash-restart integration gate (design doc section 31, FastResume phase):
// receiver A makes partial progress and persists resume.state; all in-memory
// state is destroyed to simulate a crash; receiver B restarts from the saved
// control record bytes plus resume.state only, replays cached validated outer
// blocks into freshly created production decoders, completes the remaining
// blocks live, and the reconstructed file must hash to the FinalManifest
// whole-file digest. No hidden transport is involved: everything B consumes
// comes from (1) replayed serialized control record bytes and (2) one local
// resume.state file written through the bounded file IO API.

[[nodiscard]] constexpr std::byte Byte(const std::uint8_t value) noexcept
{
    return static_cast<std::byte>(value);
}

[[nodiscard]] std::vector<std::byte> MakeSegmentRawBytes(
    const std::size_t byteCount,
    const std::uint8_t seed)
{
    // Independent of the FEC-suite message pattern so that a content mix-up
    // between segments is visible in the recovered bytes.
    std::vector<std::byte> rawBytes(byteCount);
    for (std::size_t byteIndex = 0; byteIndex < rawBytes.size(); byteIndex++)
    {
        rawBytes[byteIndex] = static_cast<std::byte>(static_cast<std::uint8_t>(
            static_cast<unsigned int>(seed) * 131U + byteIndex * 97U));
    }
    return rawBytes;
}

[[nodiscard]] pbprotocol::SessionId MakeIntegrationSessionId(
    const std::uint8_t seed) noexcept
{
    pbprotocol::SessionId sessionId{};
    for (std::size_t byteIndex = 0; byteIndex < sessionId.bytes.size(); byteIndex++)
    {
        sessionId.bytes[byteIndex] = static_cast<std::byte>(static_cast<std::uint8_t>(
            seed + byteIndex * 13U));
    }
    return sessionId;
}

[[nodiscard]] pbprotocol::SessionDescriptor MakeSession(
    const pbprotocol::SessionId& sessionId,
    const std::uint64_t originalFileSize,
    const std::uint64_t segmentCount) noexcept
{
    return pbprotocol::SessionDescriptor{
        pbprotocol::GetProtocolVersion(),
        sessionId,
        originalFileSize,
        segmentCount,
        pbprotocol::DigestAlgorithm::Blake3_256};
}

[[nodiscard]] std::vector<std::byte> WrapControlPayload(
    const pbprotocol::ControlRecordType recordType,
    const std::uint64_t controlSequence,
    const pbprotocol::SessionTag sessionTag,
    const std::span<const std::byte> payload)
{
    const pbprotocol::ControlRecordView record{
        pbprotocol::kControlVersion,
        recordType,
        controlSequence,
        sessionTag,
        payload};
    const auto sizeResult = pbprotocol::GetSerializedSize(record);
    REQUIRE(sizeResult);
    std::vector<std::byte> bytes(sizeResult.Value());
    REQUIRE(pbprotocol::SerializeControlRecord(record, bytes));
    return bytes;
}

[[nodiscard]] std::vector<std::byte> MakeSessionControlRecord(
    const pbprotocol::SessionDescriptor& descriptor,
    const std::uint64_t controlSequence)
{
    std::array<std::byte, pbprotocol::kSessionDescriptorPayloadBytes> payload{};
    REQUIRE(pbprotocol::SerializeSessionDescriptor(descriptor, payload));
    return WrapControlPayload(
        pbprotocol::ControlRecordType::SessionDescriptor,
        controlSequence,
        pbprotocol::DeriveSessionTag(descriptor.sessionId),
        payload);
}

[[nodiscard]] std::vector<std::byte> MakeSegmentControlRecord(
    const pbprotocol::SegmentDescriptor& descriptor,
    const pbprotocol::SessionDescriptor& sessionDescriptor,
    const std::uint64_t controlSequence)
{
    const auto sizeResult = pbprotocol::GetSerializedSize(descriptor);
    REQUIRE(sizeResult);
    std::vector<std::byte> payload(sizeResult.Value());
    REQUIRE(pbprotocol::SerializeSegmentDescriptor(
        descriptor,
        sessionDescriptor,
        pbprotocol::GetDefaultReceiverResourcePolicy(),
        payload));
    return WrapControlPayload(
        pbprotocol::ControlRecordType::SegmentDescriptor,
        controlSequence,
        descriptor.sessionTag,
        payload);
}

[[nodiscard]] std::vector<std::byte> MakeManifestControlRecord(
    const pbprotocol::FinalManifest& finalManifest,
    const pbprotocol::SessionDescriptor& sessionDescriptor,
    const std::uint64_t controlSequence)
{
    std::array<std::byte, pbprotocol::kFinalManifestPayloadBytes> payload{};
    REQUIRE(pbprotocol::SerializeFinalManifest(
        finalManifest,
        sessionDescriptor,
        pbprotocol::GetDefaultReceiverResourcePolicy(),
        payload));

    // Sender-side oracle self-check: the serialized manifest must parse back
    // to exactly the constructed fields (independent of the receiver).
    const auto parsedResult = pbprotocol::ParseFinalManifest(
        std::span<const std::byte>(payload),
        sessionDescriptor,
        pbprotocol::GetDefaultReceiverResourcePolicy());
    REQUIRE(parsedResult);
    CHECK(parsedResult.Value() == finalManifest);

    return WrapControlPayload(
        pbprotocol::ControlRecordType::FinalManifest,
        controlSequence,
        pbprotocol::DeriveSessionTag(finalManifest.sessionId),
        payload);
}

[[nodiscard]] std::filesystem::path MakeScratchRoot(const char* const caseName)
{
    const std::filesystem::path scratchRoot =
        std::filesystem::path(PB_TEST_SCRATCH_ROOT) / caseName;
    (void)std::filesystem::remove_all(scratchRoot);
    REQUIRE(std::filesystem::create_directories(scratchRoot));
    return scratchRoot;
}

[[nodiscard]] pbprotocol::ReceiverResourcePolicy MakeIntegrationPolicy() noexcept
{
    // Small resume budget: every document produced here fits with a wide
    // margin, so an accidental over-budget write fails loudly instead of
    // silently succeeding.
    pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    resourcePolicy.maxResumeBytes = 4096ULL;
    return resourcePolicy;
}

[[nodiscard]] std::array<std::byte, pbprotocol::kDigestBytes> Blake3(
    const std::span<const std::byte> data) noexcept
{
    return pbprotocol::ComputeBlake3Digest(data);
}

// Concatenates segment bytes in ordinal order for the whole-file oracle.
[[nodiscard]] std::vector<std::byte> ConcatenateSegments(
    const std::vector<std::span<const std::byte>>& parts)
{
    std::size_t totalBytes = 0;
    for (const auto& part : parts)
    {
        totalBytes += part.size();
    }
    std::vector<std::byte> concatenated(totalBytes);
    std::size_t writeOffset = 0;
    for (const auto& part : parts)
    {
        std::copy(part.begin(), part.end(),
                  concatenated.begin() + static_cast<std::ptrdiff_t>(writeOffset));
        writeOffset += part.size();
    }
    return concatenated;
}

void ReceiveControlRecordOrDie(
    pbprotocol::ControlPlaneReceiver& receiver,
    const std::vector<std::byte>& recordBytes)
{
    const auto receiveResult =
        receiver.ReceiveControlRecord(std::span<const std::byte>(recordBytes));
    REQUIRE(receiveResult);
}

// The production restart contract: decoders are created from the bound
// descriptor plus the session's expected OuterBlockBytes, never from cached
// codec handles (design doc section 31.2/31.3).
[[nodiscard]] pbouterfec::WirehairV2Decoder MakeProductionWirehairDecoder(
    const pbprotocol::BoundSegmentDescriptor& boundDescriptor,
    const std::uint32_t outerBlockBytes,
    const pbouterfec::OuterFecDecoderResourceManager& resourceManager)
{
    auto decoderResult = pbouterfec::WirehairV2Decoder::Create(
        boundDescriptor, outerBlockBytes, resourceManager);
    REQUIRE(decoderResult);
    return std::move(decoderResult).Value();
}

[[nodiscard]] pbouterfec::DirectRepeatDecoder MakeProductionDirectRepeatDecoder(
    const pbprotocol::BoundSegmentDescriptor& boundDescriptor,
    const std::uint32_t outerBlockBytes,
    const pbouterfec::OuterFecDecoderResourceManager& resourceManager)
{
    auto decoderResult = pbouterfec::DirectRepeatDecoder::Create(
        boundDescriptor, outerBlockBytes, resourceManager);
    REQUIRE(decoderResult);
    return std::move(decoderResult).Value();
}

[[nodiscard]] pbprotocol::BoundSegmentDescriptor GetBoundOrDie(
    const pbprotocol::ControlPlaneReceiver& receiver,
    const pbprotocol::SessionTag sessionTag,
    const std::uint64_t segmentOrdinal)
{
    auto boundResult =
        receiver.GetBoundSegmentDescriptor(sessionTag, segmentOrdinal);
    REQUIRE(boundResult);
    return std::move(boundResult).Value();
}
std::vector<std::byte> RecoverOrDie(
    pbouterfec::WirehairV2Decoder& decoder,
    const std::size_t expectedBytes)
{
    std::vector<std::byte> recovered(expectedBytes);
    const auto recoverResult = decoder.Recover(recovered);
    REQUIRE(recoverResult);
    CHECK(static_cast<std::uint64_t>(expectedBytes) == recoverResult.Value());
    return recovered;
}

std::vector<std::byte> RecoverOrDie(
    pbouterfec::DirectRepeatDecoder& decoder,
    const std::size_t expectedBytes)
{
    std::vector<std::byte> recovered(expectedBytes);
    const auto recoverResult = decoder.Recover(recovered);
    REQUIRE(recoverResult);
    CHECK(static_cast<std::uint64_t>(expectedBytes) == recoverResult.Value());
    return recovered;
}

// Completes a Wirehair decoder live from a fresh encoder, requiring Ready on
// exactly the final fill (deterministic systematic slices for any instance).
void CompleteWirehairDecoder(
    pbouterfec::WirehairV2Decoder& decoder,
    const std::span<const std::byte> segmentBytes,
    const std::uint32_t outerBlockBytes,
    const std::vector<std::uint32_t>& blockIds)
{
    auto encoderResult = pbouterfec::WirehairV2Encoder::Create(
        segmentBytes, outerBlockBytes);
    REQUIRE(encoderResult);
    pbouterfec::WirehairV2Encoder encoder = std::move(encoderResult).Value();

    for (std::size_t idIndex = 0; idIndex < blockIds.size(); idIndex++)
    {
        std::vector<std::byte> payload(encoder.GetOuterBlockBytes());
        const auto encodeResult = encoder.EncodeBlock(blockIds[idIndex], payload);
        REQUIRE(encodeResult);
        payload.resize(encodeResult.Value());

        const auto decodeResult = decoder.DecodeBlock(
            blockIds[idIndex], std::span<const std::byte>(payload));
        REQUIRE(decodeResult);
        const bool isFinalFill = idIndex == blockIds.size() - 1U;
        CHECK((decodeResult.Value() == pbouterfec::DecodeDisposition::Ready)
              == isFinalFill);
    }
}

void CompleteDirectRepeatDecoder(
    pbouterfec::DirectRepeatDecoder& decoder,
    const std::span<const std::byte> segmentBytes,
    const std::uint32_t outerBlockBytes,
    const std::vector<std::uint32_t>& blockIds)
{
    auto encoderResult = pbouterfec::DirectRepeatEncoder::Create(
        segmentBytes, outerBlockBytes);
    REQUIRE(encoderResult);
    pbouterfec::DirectRepeatEncoder encoder = std::move(encoderResult).Value();

    for (std::size_t idIndex = 0; idIndex < blockIds.size(); idIndex++)
    {
        std::vector<std::byte> padded(encoder.GetOuterBlockBytes());
        const auto encodeResult = encoder.EncodeBlock(blockIds[idIndex], padded);
        REQUIRE(encodeResult);

        const auto decodeResult = decoder.DecodeBlock(
            blockIds[idIndex], encodeResult.Value(),
            std::span<const std::byte>(padded));
        REQUIRE(decodeResult);
        const bool isFinalFill = idIndex == blockIds.size() - 1U;
        CHECK((decodeResult.Value() == pbouterfec::DecodeDisposition::Ready)
              == isFinalFill);
    }
}

struct WirehairLiveCapture
{
    pbprotocol::WirehairV2SerializedProfile serializedProfile{};
    std::vector<std::pair<std::uint32_t, std::vector<std::byte>>> entries;
};

// Encodes the requested outer block ids with a fresh encoder and feeds them to
// the decoder live, recording exactly the payload spans handed to DecodeBlock.
[[nodiscard]] WirehairLiveCapture CaptureWirehairBlocks(
    const std::span<const std::byte> segmentBytes,
    const std::uint32_t outerBlockBytes,
    pbouterfec::WirehairV2Decoder& decoder,
    const std::vector<std::uint32_t>& blockIds)
{
    WirehairLiveCapture capture;
    auto encoderResult = pbouterfec::WirehairV2Encoder::Create(
        segmentBytes, outerBlockBytes);
    REQUIRE(encoderResult);
    pbouterfec::WirehairV2Encoder encoder = std::move(encoderResult).Value();
    capture.serializedProfile = encoder.GetSerializedProfile();

    for (const std::uint32_t blockId : blockIds)
    {
        std::vector<std::byte> payload(encoder.GetOuterBlockBytes());
        const auto encodeResult = encoder.EncodeBlock(blockId, payload);
        REQUIRE(encodeResult);
        payload.resize(encodeResult.Value());

        const auto decodeResult = decoder.DecodeBlock(
            blockId, std::span<const std::byte>(payload));
        REQUIRE(decodeResult);
        capture.entries.emplace_back(blockId, std::move(payload));
    }
    return capture;
}

// Live-captures DirectRepeat blocks the same way, filling a resume record.
[[nodiscard]] pbprotocol::ResumeActiveDirectRepeatRecord CaptureDirectBlocks(
    const std::span<const std::byte> segmentBytes,
    const std::uint32_t outerBlockBytes,
    const std::uint64_t segmentOrdinal,
    pbouterfec::DirectRepeatDecoder& decoder,
    const std::vector<std::uint32_t>& blockIds)
{
    pbprotocol::ResumeActiveDirectRepeatRecord record;
    record.segmentOrdinal = segmentOrdinal;

    auto encoderResult = pbouterfec::DirectRepeatEncoder::Create(
        segmentBytes, outerBlockBytes);
    REQUIRE(encoderResult);
    pbouterfec::DirectRepeatEncoder encoder = std::move(encoderResult).Value();
    record.directBlockCount = static_cast<std::uint32_t>(encoder.GetBlockCount());

    for (const std::uint32_t blockId : blockIds)
    {
        std::vector<std::byte> padded(encoder.GetOuterBlockBytes());
        const auto encodeResult = encoder.EncodeBlock(blockId, padded);
        REQUIRE(encodeResult);
        const std::uint32_t realPayloadBytes = encodeResult.Value();

        const auto decodeResult = decoder.DecodeBlock(
            blockId, realPayloadBytes, std::span<const std::byte>(padded));
        REQUIRE(decodeResult);

        pbprotocol::ResumeDirectRepeatEntry entry;
        entry.blockOrdinal = blockId;
        entry.realPayloadBytes = realPayloadBytes;
        entry.paddedPayload = std::move(padded);
        record.entries.push_back(std::move(entry));
    }
    return record;
}

// Persists one resume document through the bounded file IO API.
void WriteResumeDocumentOrDie(
    const std::filesystem::path& filePath,
    const std::span<const std::byte> document)
{
    const auto writeResult = pbreceiver::WriteResumeStateFile(filePath, document);
    REQUIRE(writeResult);
    CHECK(static_cast<std::uint64_t>(document.size()) == writeResult.Value());
}

// Loads one resume document through the bounded file IO API.
[[nodiscard]] std::vector<std::byte> ReadResumeDocumentOrDie(
    const std::filesystem::path& filePath,
    const pbprotocol::ReceiverResourcePolicy& resourcePolicy)
{
    auto readResult =
        pbreceiver::ReadResumeStateFile(filePath, resourcePolicy);
    REQUIRE(readResult);
    return std::move(readResult).Value();
}

pbprotocol::LoadedResumeState LoadDocumentOrDie(
    const std::span<const std::byte> document,
    const pbprotocol::ReceiverResourcePolicy& resourcePolicy)
{
    auto loadResult =
        pbprotocol::LoadResumeState(document, resourcePolicy);
    REQUIRE(loadResult);
    return std::move(loadResult).Value();
}

// Builds the full serialized control record stream (session, segments in
// ordinal order, manifest last). These bytes are the only "wire" evidence that
// survives the simulated crash.
struct ControlRecordStream
{
    std::vector<std::byte> sessionRecord;
    std::vector<std::vector<std::byte>> segmentRecords;
    std::vector<std::byte> manifestRecord;

    [[nodiscard]] std::size_t RecordCount() const noexcept
    {
        return 1U + static_cast<std::size_t>(segmentRecords.size()) + 1U;
    }
};

ControlRecordStream BuildControlRecordStream(
    const pbprotocol::SessionDescriptor& sessionDescriptor,
    const std::vector<pbprotocol::SegmentDescriptor>& segmentDescriptors,
    const pbprotocol::FinalManifest& finalManifest)
{
    ControlRecordStream stream;
    std::uint64_t controlSequence = 1U;

    stream.sessionRecord = MakeSessionControlRecord(
        sessionDescriptor, controlSequence++);
    for (const auto& descriptor : segmentDescriptors)
    {
        stream.segmentRecords.push_back(MakeSegmentControlRecord(
            descriptor, sessionDescriptor, controlSequence++));
    }
    stream.manifestRecord = MakeManifestControlRecord(
        finalManifest, sessionDescriptor, controlSequence);
    return stream;
}

void ReplayControlStreamOrDie(
    pbprotocol::ControlPlaneReceiver& receiver,
    const ControlRecordStream& stream)
{
    ReceiveControlRecordOrDie(receiver, stream.sessionRecord);
    for (const auto& recordBytes : stream.segmentRecords)
    {
        const auto receiveResult =
            receiver.ReceiveControlRecord(std::span<const std::byte>(recordBytes));
        REQUIRE(receiveResult);
        // Segment records must admit their bound descriptor.
        CHECK(receiveResult.Value().boundSegmentDescriptor.has_value());
    }
    ReceiveControlRecordOrDie(receiver, stream.manifestRecord);
}

} // namespace
TEST_CASE("Crash-restart: resume.state plus control replay reconstructs all segments bit-exactly",
          "[resume-integration][restart]")
{
    constexpr std::uint32_t kOuterBlockBytes = 16U;

    // ---- Sender-side oracles (ground truth for the whole run). -------------
    const pbprotocol::SessionId sessionId = MakeIntegrationSessionId(0x21);
    const pbprotocol::SessionTag sessionTag = pbprotocol::DeriveSessionTag(sessionId);
    const std::vector<std::byte> rawSegment0 = MakeSegmentRawBytes(10U, 1U); // DirectRepeat, completes pre-crash.
    const std::vector<std::byte> rawSegment1 = MakeSegmentRawBytes(117U, 2U); // Wirehair short tail (117 % 16 == 5).
    const std::vector<std::byte> rawSegment2 = MakeSegmentRawBytes(37U, 3U); // DirectRepeat short tail (37 % 16 == 5).

    CHECK(static_cast<std::uint64_t>(rawSegment0.size()) +
              static_cast<std::uint64_t>(rawSegment1.size()) +
              static_cast<std::uint64_t>(rawSegment2.size()) ==
          164ULL);
    const std::array<std::byte, pbprotocol::kDigestBytes> expectedWholeFileDigest =
        Blake3(ConcatenateSegments({rawSegment0, rawSegment1, rawSegment2}));

    // Real codec geometry probes: K=8 for 117/16 Wirehair; 3 blocks for 37/16.
    auto wirehairProbeResult = pbouterfec::WirehairV2Encoder::Create(
        rawSegment1, kOuterBlockBytes);
    REQUIRE(wirehairProbeResult);
    pbouterfec::WirehairV2Encoder wirehairProbe = std::move(wirehairProbeResult).Value();
    CHECK(wirehairProbe.GetBlockCount() == 8U);
    const pbprotocol::WirehairV2SerializedProfile segment1Profile =
        wirehairProbe.GetSerializedProfile();

    auto directProbeResult = pbouterfec::DirectRepeatEncoder::Create(
        rawSegment2, kOuterBlockBytes);
    REQUIRE(directProbeResult);
    pbouterfec::DirectRepeatEncoder directProbe = std::move(directProbeResult).Value();
    CHECK(static_cast<std::uint64_t>(directProbe.GetBlockCount()) == 3ULL);

    const pbprotocol::SessionDescriptor sessionDescriptor = MakeSession(
        sessionId, 164ULL, 3U);

    const std::array<std::byte, pbprotocol::kDigestBytes> segment0Digest =
        Blake3(rawSegment0);
    const std::array<std::byte, pbprotocol::kDigestBytes> segment1Digest =
        Blake3(rawSegment1);
    const std::array<std::byte, pbprotocol::kDigestBytes> segment2Digest =
        Blake3(rawSegment2);

    const std::vector<pbprotocol::SegmentDescriptor> segmentDescriptors{
        {sessionTag, 0U, 0U, 10ULL, 10ULL,
         pbprotocol::CompressionCodec::Raw, pbprotocol::OuterFecMode::DirectRepeat,
         kOuterBlockBytes, pbprotocol::RawDigest{segment0Digest}, pbprotocol::EncodedDigest{segment0Digest},
         std::nullopt},
        {sessionTag, 1U, 10ULL, 117ULL, 117ULL,
         pbprotocol::CompressionCodec::Raw, pbprotocol::OuterFecMode::WirehairV2,
         kOuterBlockBytes, pbprotocol::RawDigest{segment1Digest}, pbprotocol::EncodedDigest{segment1Digest},
         segment1Profile},
        {sessionTag, 2U, 127ULL, 37ULL, 37ULL,
         pbprotocol::CompressionCodec::Raw, pbprotocol::OuterFecMode::DirectRepeat,
         kOuterBlockBytes, pbprotocol::RawDigest{segment2Digest}, pbprotocol::EncodedDigest{segment2Digest},
         std::nullopt}};

    const pbprotocol::FinalManifest finalManifest{
        sessionId, 164ULL, 3U, pbprotocol::WholeFileDigest{expectedWholeFileDigest},
        pbprotocol::DigestAlgorithm::Blake3_256};

    const ControlRecordStream controlStream = BuildControlRecordStream(
        sessionDescriptor, segmentDescriptors, finalManifest);
    CHECK(controlStream.RecordCount() == 5U);

    // The resume records A will persist are plain data (not codec state) so
    // they legitimately outlive the crash scope and serve as B's deep-equality
    // oracle.
    pbprotocol::ResumeCompletedSegmentRecord expectedCompleted;
    expectedCompleted.sessionId = sessionId;
    expectedCompleted.segmentOrdinal = 0U;
    expectedCompleted.rawOffset = 0U;
    expectedCompleted.rawSize = static_cast<std::uint64_t>(rawSegment0.size());
    // Independent digest oracle: computed here from ground truth, never copied
    // from any decoder or builder output.
    expectedCompleted.rawDigest = pbprotocol::RawDigest{segment0Digest};

    pbprotocol::ResumeActiveWirehairCacheRecord expectedWirehairCache;
    pbprotocol::ResumeActiveDirectRepeatRecord expectedDirectRecord;

    const std::filesystem::path scratchRoot = MakeScratchRoot("resume_restart_main");
    const std::filesystem::path resumeFilePath = scratchRoot / "resume.state";

    // ---- Receiver A: live partial progress, then the crash boundary. -------
    {
        pbprotocol::ControlPlaneReceiver receiverA = [&controlStream]()
        {
            auto result =
                pbprotocol::ControlPlaneReceiver::Create(MakeIntegrationPolicy());
            REQUIRE(result);
            return std::move(result).Value();
        }();

        ReplayControlStreamOrDie(receiverA, controlStream);
        CHECK(receiverA.HasFinalManifest(sessionTag));

        auto managerResult =
            pbouterfec::OuterFecDecoderResourceManager::Create(
                pbprotocol::GetDefaultReceiverResourcePolicy());
        REQUIRE(managerResult);
        pbouterfec::OuterFecDecoderResourceManager decoderManager =
            std::move(managerResult).Value();

        // Segment 0 completes fully before the crash (its raw bytes would sit
        // in output.part; its digest is metadata only per section 31.1).
        {
            const pbprotocol::BoundSegmentDescriptor boundSegment0 = GetBoundOrDie(
                receiverA, sessionTag, 0U);
            pbouterfec::DirectRepeatDecoder decoder0 = MakeProductionDirectRepeatDecoder(
                boundSegment0, kOuterBlockBytes, decoderManager);

            std::vector<std::byte> block0Padded(kOuterBlockBytes);
            auto encoderResult = pbouterfec::DirectRepeatEncoder::Create(
                rawSegment0, kOuterBlockBytes);
            REQUIRE(encoderResult);
            pbouterfec::DirectRepeatEncoder encoder0 = std::move(encoderResult).Value();
            CHECK(static_cast<std::uint64_t>(encoder0.GetBlockCount()) == 1ULL);
            const auto encodeResult = encoder0.EncodeBlock(0U, block0Padded);
            REQUIRE(encodeResult);
            CHECK(encodeResult.Value() == 10U);

            const auto decodeResult = decoder0.DecodeBlock(
                0U, encodeResult.Value(), std::span<const std::byte>(block0Padded));
            REQUIRE(decodeResult);
            CHECK(decodeResult.Value() == pbouterfec::DecodeDisposition::Ready);
            const std::vector<std::byte> recovered0 = RecoverOrDie(decoder0, 10U);
            CHECK(std::equal(
                recovered0.begin(), recovered0.end(), rawSegment0.begin()));

            REQUIRE(receiverA.MarkSegmentCompleted(sessionTag, 0U));
            const auto completedFlagResult = receiverA.IsSegmentCompleted(sessionTag, 0U);
            REQUIRE(completedFlagResult);
            CHECK(completedFlagResult.Value());
        }

        // Segment 1 (Wirehair): live-capture ids 0/2/4 plus the short-tail id 7
        // and one repair equation before the crash.
        {
            const pbprotocol::BoundSegmentDescriptor boundSegment1 = GetBoundOrDie(
                receiverA, sessionTag, 1U);
            pbouterfec::WirehairV2Decoder decoder1 = MakeProductionWirehairDecoder(
                boundSegment1, kOuterBlockBytes, decoderManager);

            const WirehairLiveCapture capture = CaptureWirehairBlocks(
                rawSegment1, kOuterBlockBytes, decoder1, {0U, 2U, 4U, 7U});
            // Independent short-tail oracle: id 7 carries exactly 5 bytes.
            REQUIRE(capture.entries.size() == 4U);
            CHECK(capture.entries[3].second.size() == 5U);

            expectedWirehairCache.segmentOrdinal = 1U;
            expectedWirehairCache.wirehairProfile = capture.serializedProfile;
            for (const auto& [blockId, payload] : capture.entries)
            {
                pbprotocol::ResumeWirehairCacheEntry entry;
                entry.outerBlockId = blockId;
                entry.payload = payload;
                expectedWirehairCache.entries.push_back(std::move(entry));
            }

            // One repair equation on top (5 accepted < K=8, still NeedMore).
            const WirehairLiveCapture repairCapture = CaptureWirehairBlocks(
                rawSegment1, kOuterBlockBytes, decoder1, {8U});
            REQUIRE(repairCapture.entries.size() == 1U);
            CHECK(repairCapture.entries[0].second.size() == 16U);
            pbprotocol::ResumeWirehairCacheEntry repairEntry;
            repairEntry.outerBlockId = repairCapture.entries[0].first;
            repairEntry.payload = repairCapture.entries[0].second;
            expectedWirehairCache.entries.push_back(std::move(repairEntry));
        }

        // Segment 2 (DirectRepeat): live-capture blocks 0 and the short-tail 2.
        {
            const pbprotocol::BoundSegmentDescriptor boundSegment2 = GetBoundOrDie(
                receiverA, sessionTag, 2U);
            pbouterfec::DirectRepeatDecoder decoder2 = MakeProductionDirectRepeatDecoder(
                boundSegment2, kOuterBlockBytes, decoderManager);

            expectedDirectRecord = CaptureDirectBlocks(
                rawSegment2, kOuterBlockBytes, 2U, decoder2, {0U, 2U});
            CHECK(expectedDirectRecord.directBlockCount == 3U);
            REQUIRE(expectedDirectRecord.entries.size() == 2U);
            // Short-tail oracle: block 2 has real=5 with canonical zero padding.
            CHECK(expectedDirectRecord.entries[1].realPayloadBytes == 5U);
            const std::vector<std::byte>& tailPadded =
                expectedDirectRecord.entries[1].paddedPayload;
            CHECK(std::all_of(
                tailPadded.begin() + 5, tailPadded.end(),
                [](const std::byte value) noexcept { return value == Byte(0x00); }));
        }

        // Persist the resume document: one completed record plus both active
        // caches. All three appends must succeed under the 4 KiB budget.
        auto builderResult = pbprotocol::ResumeStateBuilder::Create(
            MakeIntegrationPolicy());
        REQUIRE(builderResult);
        pbprotocol::ResumeStateBuilder builder = std::move(builderResult).Value();

        REQUIRE(builder.AppendCompletedSegment(expectedCompleted));
        REQUIRE(builder.AppendActiveWirehairCache(expectedWirehairCache));
        REQUIRE(builder.AppendDirectRepeatReceivedBlocks(expectedDirectRecord));

        const std::span<const std::byte> documentBytes = builder.GetDocument();
        CHECK(!documentBytes.empty());
        WriteResumeDocumentOrDie(resumeFilePath, documentBytes);
    } // ---- Crash boundary: every decoder, receiver, encoder and the builder
      // die here; only the control record bytes above plus resume.state on
      // disk remain (FastResume semantics, no crash-safe flush ordering).
    // ---- Receiver B: restart from persisted evidence only. -----------------
    pbprotocol::ControlPlaneReceiver receiverB = [&controlStream]()
    {
        auto result =
            pbprotocol::ControlPlaneReceiver::Create(MakeIntegrationPolicy());
        REQUIRE(result);
        return std::move(result).Value();
    }();

    ReplayControlStreamOrDie(receiverB, controlStream);
    CHECK(receiverB.HasFinalManifest(sessionTag));

    const pbprotocol::ReceiverResourcePolicy policyB = MakeIntegrationPolicy();
    const std::vector<std::byte> documentBytes = ReadResumeDocumentOrDie(
        resumeFilePath, policyB);
    const pbprotocol::LoadedResumeState loadedState = LoadDocumentOrDie(
        std::span<const std::byte>(documentBytes), policyB);

    // The persisted state must be deep-equal to the records A appended (and
    // carry no torn-tail flag for a clean write).
    CHECK_FALSE(loadedState.hasTruncatedTail);
    pbprotocol::LoadedResumeState expectedLoaded;
    expectedLoaded.completedSegments.push_back(expectedCompleted);
    expectedLoaded.activeWirehairCaches.push_back(expectedWirehairCache);
    expectedLoaded.activeDirectRepeatRecords.push_back(expectedDirectRecord);
    CHECK(loadedState == expectedLoaded);

    // Completed metadata cross-checked against the independent oracle digest.
    REQUIRE(loadedState.completedSegments.size() == 1U);
    const pbprotocol::ResumeCompletedSegmentRecord& completedRecord =
        loadedState.completedSegments.front();
    CHECK(completedRecord.rawDigest.bytes == Blake3(rawSegment0));

    // Segment 1 restart: replay the cached spans into a fresh production
    // decoder, then complete the Fountain rank live (5 accepted + fills 1/3/5).
    std::vector<std::byte> recoveredSegment1;
    {
        auto managerResult =
            pbouterfec::OuterFecDecoderResourceManager::Create(
                pbprotocol::GetDefaultReceiverResourcePolicy());
        REQUIRE(managerResult);
        pbouterfec::OuterFecDecoderResourceManager decoderManagerB =
            std::move(managerResult).Value();

        const pbprotocol::BoundSegmentDescriptor boundSegment1 = GetBoundOrDie(
            receiverB, sessionTag, 1U);
        pbouterfec::WirehairV2Decoder decoder1B = MakeProductionWirehairDecoder(
            boundSegment1, kOuterBlockBytes, decoderManagerB);

        REQUIRE(loadedState.activeWirehairCaches.size() == 1U);
        const auto replayResult = pbreceiver::ReplayActiveWirehairCache(
            loadedState.activeWirehairCaches.front(), decoder1B);
        REQUIRE(replayResult);
        CHECK(static_cast<std::uint64_t>(expectedWirehairCache.entries.size()) ==
              static_cast<std::uint64_t>(replayResult.Value().replayedEntryCount));
        CHECK_FALSE(replayResult.Value().decoderReady); // 5 < K=8.

        // Fills stop at exactly K=8 distinct accepted ids: the decoder's
        // finite admission table rejects a ninth with ExtraInsufficient.
        CompleteWirehairDecoder(decoder1B, rawSegment1, kOuterBlockBytes, {1U, 3U, 5U});
        recoveredSegment1 = RecoverOrDie(
            decoder1B, static_cast<std::size_t>(rawSegment1.size()));
    }

    // Segment 2 restart: replay blocks 0/2, then live-fill block 1.
    std::vector<std::byte> recoveredSegment2;
    {
        auto managerResult =
            pbouterfec::OuterFecDecoderResourceManager::Create(
                pbprotocol::GetDefaultReceiverResourcePolicy());
        REQUIRE(managerResult);
        pbouterfec::OuterFecDecoderResourceManager decoderManagerB =
            std::move(managerResult).Value();

        const pbprotocol::BoundSegmentDescriptor boundSegment2 = GetBoundOrDie(
            receiverB, sessionTag, 2U);
        pbouterfec::DirectRepeatDecoder decoder2B = MakeProductionDirectRepeatDecoder(
            boundSegment2, kOuterBlockBytes, decoderManagerB);

        REQUIRE(loadedState.activeDirectRepeatRecords.size() == 1U);
        const auto replayResult = pbreceiver::ReplayDirectRepeatBlocks(
            loadedState.activeDirectRepeatRecords.front(), decoder2B);
        REQUIRE(replayResult);
        CHECK(static_cast<std::uint64_t>(expectedDirectRecord.entries.size()) ==
              static_cast<std::uint64_t>(replayResult.Value().replayedEntryCount));
        CHECK_FALSE(replayResult.Value().decoderReady); // 2 of 3 blocks.

        CompleteDirectRepeatDecoder(decoder2B, rawSegment2, kOuterBlockBytes, {1U});
        recoveredSegment2 = RecoverOrDie(
            decoder2B, static_cast<std::size_t>(rawSegment2.size()));
    }

    // Bit-exact recovery of the restarted segments.
    CHECK(std::equal(
        recoveredSegment1.begin(), recoveredSegment1.end(), rawSegment1.begin()));
    CHECK(std::equal(
        recoveredSegment2.begin(), recoveredSegment2.end(), rawSegment2.begin()));

    // Authoritative convergence: completion is marked only after content was
    // verified (segment 0 against the oracle digest; segments 1/2 by full
    // recovery), mirroring the section 31.5 re-verification allowance, and the
    // complete segment map must validate on the restarted receiver.
    REQUIRE(receiverB.MarkSegmentCompleted(sessionTag, 0U));
    REQUIRE(receiverB.MarkSegmentCompleted(sessionTag, 1U));
    REQUIRE(receiverB.MarkSegmentCompleted(sessionTag, 2U));
    for (const std::uint64_t ordinal : {0ULL, 1ULL, 2ULL})
    {
        const auto completedFlagResult = receiverB.IsSegmentCompleted(sessionTag, ordinal);
        REQUIRE(completedFlagResult);
        CHECK(completedFlagResult.Value());
    }
    REQUIRE(receiverB.ValidateCompleteSegmentMap(sessionTag));

    // Whole-file gate: the reconstructed file hashes to exactly the
    // FinalManifest digest computed independently by the test oracle.
    const std::vector<std::byte> finalFileBytes = ConcatenateSegments(
        {rawSegment0, recoveredSegment1, recoveredSegment2});
    CHECK(static_cast<std::uint64_t>(finalFileBytes.size()) == 164ULL);
    CHECK(Blake3(finalFileBytes) == expectedWholeFileDigest);

    std::error_code cleanupErrorCode;
    (void)std::filesystem::remove_all(scratchRoot, cleanupErrorCode);
}

TEST_CASE("Restart gate edge session: zero-byte file uses the empty resume document path",
          "[resume-integration][restart][edge]")
{
    const pbprotocol::SessionId sessionId = MakeIntegrationSessionId(0x37);
    const pbprotocol::SessionTag sessionTag = pbprotocol::DeriveSessionTag(sessionId);

    const pbprotocol::SessionDescriptor sessionDescriptor = MakeSession(sessionId, 0ULL, 0U);
    const pbprotocol::FinalManifest finalManifest{
        sessionId, 0ULL, 0U,
        pbprotocol::GetEmptyBlake3WholeFileDigest(),
        pbprotocol::DigestAlgorithm::Blake3_256};

    const ControlRecordStream controlStream = BuildControlRecordStream(
        sessionDescriptor, {}, finalManifest);
    CHECK(controlStream.RecordCount() == 2U);

    const std::filesystem::path scratchRoot = MakeScratchRoot("resume_restart_empty");
    const std::filesystem::path resumeFilePath = scratchRoot / "resume.state";

    {
        pbprotocol::ControlPlaneReceiver receiverA = [&controlStream]()
        {
            auto result =
                pbprotocol::ControlPlaneReceiver::Create(MakeIntegrationPolicy());
            REQUIRE(result);
            return std::move(result).Value();
        }();
        ReplayControlStreamOrDie(receiverA, controlStream);

        // An empty builder yields a zero-byte document; the file write must
        // succeed and produce exactly that.
        auto builderResult = pbprotocol::ResumeStateBuilder::Create(
            MakeIntegrationPolicy());
        REQUIRE(builderResult);
        pbprotocol::ResumeStateBuilder builder = std::move(builderResult).Value();
        CHECK(builder.GetByteCount() == 0U);
        WriteResumeDocumentOrDie(resumeFilePath, builder.GetDocument());
    } // crash boundary

    pbprotocol::ControlPlaneReceiver receiverB = [&controlStream]()
    {
        auto result =
            pbprotocol::ControlPlaneReceiver::Create(MakeIntegrationPolicy());
        REQUIRE(result);
        return std::move(result).Value();
    }();
    ReplayControlStreamOrDie(receiverB, controlStream);

    const pbprotocol::ReceiverResourcePolicy policyB = MakeIntegrationPolicy();
    const std::vector<std::byte> documentBytes = ReadResumeDocumentOrDie(
        resumeFilePath, policyB);
    CHECK(documentBytes.empty()); // zero-byte file read back as an empty span.

    const pbprotocol::LoadedResumeState loadedState = LoadDocumentOrDie(
        std::span<const std::byte>(documentBytes), policyB);
    CHECK(!loadedState.hasTruncatedTail);
    CHECK(loadedState.completedSegments.empty());
    CHECK(loadedState.activeWirehairCaches.empty());
    CHECK(loadedState.activeDirectRepeatRecords.empty());

    const auto boundCountResult = receiverB.BoundSegmentCount(sessionTag);
    REQUIRE(boundCountResult);
    CHECK(boundCountResult.Value() == 0U);
    CHECK(receiverB.HasFinalManifest(sessionTag));

    std::error_code cleanupErrorCode;
    (void)std::filesystem::remove_all(scratchRoot, cleanupErrorCode);
}
TEST_CASE("Restart gate edge session: one-byte segment persists a completed record only",
          "[resume-integration][restart][edge]")
{
    const pbprotocol::SessionId sessionId = MakeIntegrationSessionId(0x4B);
    const pbprotocol::SessionTag sessionTag = pbprotocol::DeriveSessionTag(sessionId);

    const std::vector<std::byte> rawSegment = {Byte(0x42)}; // high-compressible 1-byte segment.
    const std::array<std::byte, pbprotocol::kDigestBytes> segmentDigest =
        Blake3(rawSegment);

    constexpr std::uint32_t kOuterBlockBytes = 16U;
    const pbprotocol::SessionDescriptor sessionDescriptor = MakeSession(
        sessionId, static_cast<std::uint64_t>(rawSegment.size()), 1ULL);

    const std::vector<pbprotocol::SegmentDescriptor> segmentDescriptors{
        {sessionTag, 0U, 0U,
         static_cast<std::uint64_t>(rawSegment.size()),
         static_cast<std::uint64_t>(rawSegment.size()),
         pbprotocol::CompressionCodec::Raw, pbprotocol::OuterFecMode::DirectRepeat,
         kOuterBlockBytes, pbprotocol::RawDigest{segmentDigest}, pbprotocol::EncodedDigest{segmentDigest},
         std::nullopt}};

    const pbprotocol::FinalManifest finalManifest{
        sessionId, static_cast<std::uint64_t>(rawSegment.size()), 1ULL,
        pbprotocol::WholeFileDigest{Blake3(ConcatenateSegments({rawSegment}))},
        pbprotocol::DigestAlgorithm::Blake3_256};

    const ControlRecordStream controlStream = BuildControlRecordStream(
        sessionDescriptor, segmentDescriptors, finalManifest);

    pbprotocol::ResumeCompletedSegmentRecord expectedCompleted;
    expectedCompleted.sessionId = sessionId;
    expectedCompleted.segmentOrdinal = 0U;
    expectedCompleted.rawOffset = 0U;
    expectedCompleted.rawSize = static_cast<std::uint64_t>(rawSegment.size());
    expectedCompleted.rawDigest = pbprotocol::RawDigest{segmentDigest};

    const std::filesystem::path scratchRoot = MakeScratchRoot("resume_restart_one_byte");
    const std::filesystem::path resumeFilePath = scratchRoot / "resume.state";

    {
        pbprotocol::ControlPlaneReceiver receiverA = [&controlStream]()
        {
            auto result =
                pbprotocol::ControlPlaneReceiver::Create(MakeIntegrationPolicy());
            REQUIRE(result);
            return std::move(result).Value();
        }();
        ReplayControlStreamOrDie(receiverA, controlStream);

        auto managerResult =
            pbouterfec::OuterFecDecoderResourceManager::Create(
                pbprotocol::GetDefaultReceiverResourcePolicy());
        REQUIRE(managerResult);
        pbouterfec::OuterFecDecoderResourceManager decoderManager =
            std::move(managerResult).Value();

        const pbprotocol::BoundSegmentDescriptor boundSegment = GetBoundOrDie(
            receiverA, sessionTag, 0U);
        pbouterfec::DirectRepeatDecoder decoder = MakeProductionDirectRepeatDecoder(
            boundSegment, kOuterBlockBytes, decoderManager);

        std::vector<std::byte> blockPadded(kOuterBlockBytes);
        auto encoderResult = pbouterfec::DirectRepeatEncoder::Create(
            rawSegment, kOuterBlockBytes);
        REQUIRE(encoderResult);
        pbouterfec::DirectRepeatEncoder encoder = std::move(encoderResult).Value();
        CHECK(static_cast<std::uint64_t>(encoder.GetBlockCount()) == 1ULL);
        const auto encodeResult = encoder.EncodeBlock(0U, blockPadded);
        REQUIRE(encodeResult);
        CHECK(encodeResult.Value() == 1U); // single real byte, zero-padded to 16.

        const auto decodeResult = decoder.DecodeBlock(
            0U, encodeResult.Value(), std::span<const std::byte>(blockPadded));
        REQUIRE(decodeResult);
        CHECK(decodeResult.Value() == pbouterfec::DecodeDisposition::Ready);
        const std::vector<std::byte> recovered = RecoverOrDie(decoder, 1U);
        CHECK(std::equal(recovered.begin(), recovered.end(), rawSegment.begin()));

        REQUIRE(receiverA.MarkSegmentCompleted(sessionTag, 0U));

        auto builderResult = pbprotocol::ResumeStateBuilder::Create(
            MakeIntegrationPolicy());
        REQUIRE(builderResult);
        pbprotocol::ResumeStateBuilder builder = std::move(builderResult).Value();
        REQUIRE(builder.AppendCompletedSegment(expectedCompleted));
        WriteResumeDocumentOrDie(resumeFilePath, builder.GetDocument());
    } // crash boundary

    pbprotocol::ControlPlaneReceiver receiverB = [&controlStream]()
    {
        auto result =
            pbprotocol::ControlPlaneReceiver::Create(MakeIntegrationPolicy());
        REQUIRE(result);
        return std::move(result).Value();
    }();
    ReplayControlStreamOrDie(receiverB, controlStream);

    const pbprotocol::ReceiverResourcePolicy policyB = MakeIntegrationPolicy();
    const std::vector<std::byte> documentBytes = ReadResumeDocumentOrDie(
        resumeFilePath, policyB);
    const pbprotocol::LoadedResumeState loadedState = LoadDocumentOrDie(
        std::span<const std::byte>(documentBytes), policyB);

    // Completed-only state: exactly one record, deep-equal to the oracle.
    CHECK(!loadedState.hasTruncatedTail);
    REQUIRE(loadedState.completedSegments.size() == 1U);
    CHECK(loadedState.completedSegments.front() == expectedCompleted);
    CHECK(loadedState.activeWirehairCaches.empty());
    CHECK(loadedState.activeDirectRepeatRecords.empty());

    std::error_code cleanupErrorCode;
    (void)std::filesystem::remove_all(scratchRoot, cleanupErrorCode);
}
TEST_CASE("Restart gate edge session: encoded sizes divisible by OuterBlockBytes",
          "[resume-integration][restart][edge]")
{
    // Both segments hit the no-short-tail boundary: Wirehair 32/16 (K=2, all
    // blocks full size) and DirectRepeat 48/16 (exactly three full blocks). A
    // hidden short-tail assumption in replay would break this session.
    constexpr std::uint32_t kOuterBlockBytes = 16U;

    const pbprotocol::SessionId sessionId = MakeIntegrationSessionId(0x5E);
    const pbprotocol::SessionTag sessionTag = pbprotocol::DeriveSessionTag(sessionId);
    const std::vector<std::byte> rawWirehairSegment = MakeSegmentRawBytes(32U, 7U);
    const std::vector<std::byte> rawDirectSegment = MakeSegmentRawBytes(48U, 8U);

    auto wirehairProbeResult = pbouterfec::WirehairV2Encoder::Create(
        rawWirehairSegment, kOuterBlockBytes);
    REQUIRE(wirehairProbeResult);
    pbouterfec::WirehairV2Encoder wirehairProbe = std::move(wirehairProbeResult).Value();
    CHECK(wirehairProbe.GetBlockCount() == 2U); // K=2, no short tail.
    const pbprotocol::WirehairV2SerializedProfile wirehairProfile =
        wirehairProbe.GetSerializedProfile();

    auto directProbeResult = pbouterfec::DirectRepeatEncoder::Create(
        rawDirectSegment, kOuterBlockBytes);
    REQUIRE(directProbeResult);
    pbouterfec::DirectRepeatEncoder directProbe = std::move(directProbeResult).Value();
    CHECK(static_cast<std::uint64_t>(directProbe.GetBlockCount()) == 3ULL);

    const std::array<std::byte, pbprotocol::kDigestBytes> wirehairSegmentDigest =
        Blake3(rawWirehairSegment);
    const std::array<std::byte, pbprotocol::kDigestBytes> directSegmentDigest =
        Blake3(rawDirectSegment);

    const pbprotocol::SessionDescriptor sessionDescriptor = MakeSession(
        sessionId, 80ULL, 2U);
    CHECK(static_cast<std::uint64_t>(rawWirehairSegment.size()) +
              static_cast<std::uint64_t>(rawDirectSegment.size()) ==
          80ULL);

    const std::vector<pbprotocol::SegmentDescriptor> segmentDescriptors{
        {sessionTag, 0U, 0U, 32ULL, 32ULL,
         pbprotocol::CompressionCodec::Raw, pbprotocol::OuterFecMode::WirehairV2,
         kOuterBlockBytes, pbprotocol::RawDigest{wirehairSegmentDigest},
         pbprotocol::EncodedDigest{wirehairSegmentDigest}, wirehairProfile},
        {sessionTag, 1U, 32ULL, 48ULL, 48ULL,
         pbprotocol::CompressionCodec::Raw, pbprotocol::OuterFecMode::DirectRepeat,
         kOuterBlockBytes, pbprotocol::RawDigest{directSegmentDigest},
         pbprotocol::EncodedDigest{directSegmentDigest}, std::nullopt}};

    const pbprotocol::FinalManifest finalManifest{
        sessionId, 80ULL, 2U,
        pbprotocol::WholeFileDigest{Blake3(ConcatenateSegments({rawWirehairSegment, rawDirectSegment}))},
        pbprotocol::DigestAlgorithm::Blake3_256};

    const ControlRecordStream controlStream = BuildControlRecordStream(
        sessionDescriptor, segmentDescriptors, finalManifest);

    pbprotocol::ResumeActiveWirehairCacheRecord expectedWirehairCache;
    pbprotocol::ResumeActiveDirectRepeatRecord expectedDirectRecord;

    const std::filesystem::path scratchRoot = MakeScratchRoot("resume_restart_divisible");
    const std::filesystem::path resumeFilePath = scratchRoot / "resume.state";

    {
        pbprotocol::ControlPlaneReceiver receiverA = [&controlStream]()
        {
            auto result =
                pbprotocol::ControlPlaneReceiver::Create(MakeIntegrationPolicy());
            REQUIRE(result);
            return std::move(result).Value();
        }();
        ReplayControlStreamOrDie(receiverA, controlStream);

        auto managerResult =
            pbouterfec::OuterFecDecoderResourceManager::Create(
                pbprotocol::GetDefaultReceiverResourcePolicy());
        REQUIRE(managerResult);
        pbouterfec::OuterFecDecoderResourceManager decoderManager =
            std::move(managerResult).Value();

        // Wirehair: capture id 0 only; its payload must be the full block size
        // (the divisible-size boundary has no short tail even at K=2).
        {
            const pbprotocol::BoundSegmentDescriptor boundWirehair = GetBoundOrDie(
                receiverA, sessionTag, 0U);
            pbouterfec::WirehairV2Decoder decoder = MakeProductionWirehairDecoder(
                boundWirehair, kOuterBlockBytes, decoderManager);

            const WirehairLiveCapture capture = CaptureWirehairBlocks(
                rawWirehairSegment, kOuterBlockBytes, decoder, {0U});
            REQUIRE(capture.entries.size() == 1U);
            CHECK(capture.entries[0].second.size() ==
                  static_cast<std::size_t>(kOuterBlockBytes));

            expectedWirehairCache.segmentOrdinal = 0U;
            expectedWirehairCache.wirehairProfile = capture.serializedProfile;
            pbprotocol::ResumeWirehairCacheEntry entry;
            entry.outerBlockId = capture.entries[0].first;
            entry.payload = capture.entries[0].second;
            expectedWirehairCache.entries.push_back(std::move(entry));
        }

        // DirectRepeat: capture block 2 (the last one); the real payload is the
        // full 16 bytes because 48 divides 16.
        {
            const pbprotocol::BoundSegmentDescriptor boundDirect = GetBoundOrDie(
                receiverA, sessionTag, 1U);
            pbouterfec::DirectRepeatDecoder decoder = MakeProductionDirectRepeatDecoder(
                boundDirect, kOuterBlockBytes, decoderManager);

            expectedDirectRecord = CaptureDirectBlocks(
                rawDirectSegment, kOuterBlockBytes, 1U, decoder, {2U});
            REQUIRE(expectedDirectRecord.entries.size() == 1U);
            CHECK(expectedDirectRecord.directBlockCount == 3U);
            CHECK(expectedDirectRecord.entries[0].realPayloadBytes == 16U);
        }

        auto builderResult = pbprotocol::ResumeStateBuilder::Create(
            MakeIntegrationPolicy());
        REQUIRE(builderResult);
        pbprotocol::ResumeStateBuilder builder = std::move(builderResult).Value();
        REQUIRE(builder.AppendActiveWirehairCache(expectedWirehairCache));
        REQUIRE(builder.AppendDirectRepeatReceivedBlocks(expectedDirectRecord));
        WriteResumeDocumentOrDie(resumeFilePath, builder.GetDocument());
    } // crash boundary

    pbprotocol::ControlPlaneReceiver receiverB = [&controlStream]()
    {
        auto result =
            pbprotocol::ControlPlaneReceiver::Create(MakeIntegrationPolicy());
        REQUIRE(result);
        return std::move(result).Value();
    }();
    ReplayControlStreamOrDie(receiverB, controlStream);

    const pbprotocol::ReceiverResourcePolicy policyB = MakeIntegrationPolicy();
    const std::vector<std::byte> documentBytes = ReadResumeDocumentOrDie(
        resumeFilePath, policyB);
    const pbprotocol::LoadedResumeState loadedState = LoadDocumentOrDie(
        std::span<const std::byte>(documentBytes), policyB);

    CHECK_FALSE(loadedState.hasTruncatedTail);
    REQUIRE(loadedState.activeWirehairCaches.size() == 1U);
    REQUIRE(loadedState.activeDirectRepeatRecords.size() == 1U);
    CHECK(loadedState.completedSegments.empty());
    CHECK(loadedState.activeWirehairCaches.front() == expectedWirehairCache);
    CHECK(loadedState.activeDirectRepeatRecords.front() == expectedDirectRecord);

    // Restart the Wirehair segment: replay id 0, then live-fill id 1 (the full
    // second systematic block). K=2 -> Ready exactly on that fill.
    std::vector<std::byte> recoveredWirehair;
    {
        auto managerResult =
            pbouterfec::OuterFecDecoderResourceManager::Create(
                pbprotocol::GetDefaultReceiverResourcePolicy());
        REQUIRE(managerResult);
        pbouterfec::OuterFecDecoderResourceManager decoderManagerB =
            std::move(managerResult).Value();

        const pbprotocol::BoundSegmentDescriptor boundWirehair = GetBoundOrDie(
            receiverB, sessionTag, 0U);
        pbouterfec::WirehairV2Decoder decoder = MakeProductionWirehairDecoder(
            boundWirehair, kOuterBlockBytes, decoderManagerB);

        const auto replayResult = pbreceiver::ReplayActiveWirehairCache(
            loadedState.activeWirehairCaches.front(), decoder);
        REQUIRE(replayResult);
        CHECK(static_cast<std::uint64_t>(expectedWirehairCache.entries.size()) ==
              static_cast<std::uint64_t>(replayResult.Value().replayedEntryCount));
        CHECK_FALSE(replayResult.Value().decoderReady); // 1 < K=2.

        CompleteWirehairDecoder(decoder, rawWirehairSegment, kOuterBlockBytes, {1U});
        recoveredWirehair = RecoverOrDie(decoder, rawWirehairSegment.size());
    }

    // Restart the DirectRepeat segment: replay block 2, then live-fill 0 and 1.
    std::vector<std::byte> recoveredDirect;
    {
        auto managerResult =
            pbouterfec::OuterFecDecoderResourceManager::Create(
                pbprotocol::GetDefaultReceiverResourcePolicy());
        REQUIRE(managerResult);
        pbouterfec::OuterFecDecoderResourceManager decoderManagerB =
            std::move(managerResult).Value();

        const pbprotocol::BoundSegmentDescriptor boundDirect = GetBoundOrDie(
            receiverB, sessionTag, 1U);
        pbouterfec::DirectRepeatDecoder decoder = MakeProductionDirectRepeatDecoder(
            boundDirect, kOuterBlockBytes, decoderManagerB);

        const auto replayResult = pbreceiver::ReplayDirectRepeatBlocks(
            loadedState.activeDirectRepeatRecords.front(), decoder);
        REQUIRE(replayResult);
        CHECK(static_cast<std::uint64_t>(expectedDirectRecord.entries.size()) ==
              static_cast<std::uint64_t>(replayResult.Value().replayedEntryCount));
        CHECK_FALSE(replayResult.Value().decoderReady); // 1 of 3 blocks.

        CompleteDirectRepeatDecoder(decoder, rawDirectSegment, kOuterBlockBytes, {0U, 1U});
        recoveredDirect = RecoverOrDie(decoder, rawDirectSegment.size());
    }

    CHECK(std::equal(recoveredWirehair.begin(), recoveredWirehair.end(),
                     rawWirehairSegment.begin()));
    CHECK(std::equal(
        recoveredDirect.begin(), recoveredDirect.end(), rawDirectSegment.begin()));

    const std::vector<std::byte> finalFileBytes = ConcatenateSegments(
        {recoveredWirehair, recoveredDirect});
    CHECK(static_cast<std::uint64_t>(finalFileBytes.size()) == 80ULL);
    CHECK(Blake3(finalFileBytes) ==
          Blake3(ConcatenateSegments({rawWirehairSegment, rawDirectSegment})));

    std::error_code cleanupErrorCode;
    (void)std::filesystem::remove_all(scratchRoot, cleanupErrorCode);
}
