#include "pbreceiver/receiver_ingress.h"

#include "pbcompression/segment_compression.h"
#include "pbouterfec/direct_repeat.h"
#include "pbouterfec/wirehair_v2.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/control_fragment_codec.h"
#include "pbprotocol/descriptor_codec.h"
#include "pbprotocol/protocol_version.h"
#include "pbprotocol/resume_state.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace
{

struct EncodedTransportBlock
{
    std::uint32_t outerBlockId = 0;
    std::uint16_t declaredPayloadBytes = 0;
    std::vector<std::byte> paddedPayload;
};

[[nodiscard]] std::vector<std::byte> MakeBytes(
    const std::size_t byteCount,
    const std::uint8_t seed)
{
    std::vector<std::byte> bytes(byteCount);
    for (std::size_t byteIndex = 0; byteIndex < byteCount; byteIndex++)
    {
        bytes[byteIndex] = static_cast<std::byte>(
            seed + static_cast<std::uint8_t>(byteIndex % 23U));
    }
    return bytes;
}

[[nodiscard]] pbprotocol::SessionId MakeSessionId(const std::uint8_t seed)
{
    pbprotocol::SessionId sessionId;
    for (std::size_t byteIndex = 0;
         byteIndex < sessionId.bytes.size();
         byteIndex++)
    {
        sessionId.bytes[byteIndex] = static_cast<std::byte>(
            seed + static_cast<std::uint8_t>(byteIndex));
    }
    return sessionId;
}

[[nodiscard]] pbprotocol::SessionDescriptor MakeSessionDescriptor(
    const pbprotocol::SessionId& sessionId,
    const std::uint64_t originalFileSize,
    const std::uint64_t segmentCount)
{
    return pbprotocol::SessionDescriptor{
        pbprotocol::GetProtocolVersion(),
        sessionId,
        originalFileSize,
        segmentCount,
        pbprotocol::DigestAlgorithm::Blake3_256};
}

[[nodiscard]] pbprotocol::SegmentDescriptor MakeSegmentDescriptor(
    const pbprotocol::SessionDescriptor& sessionDescriptor,
    const std::uint64_t segmentOrdinal,
    const std::uint64_t rawOffset,
    const std::span<const std::byte> rawBytes,
    const std::span<const std::byte> encodedBytes,
    const pbprotocol::CompressionCodec compressionCodec,
    const pbprotocol::OuterFecMode outerFecMode,
    const std::uint32_t outerBlockBytes,
    const std::optional<pbprotocol::WirehairV2SerializedProfile>
        wirehairProfile = std::nullopt)
{
    pbprotocol::SegmentDescriptor segmentDescriptor;
    segmentDescriptor.sessionTag = pbprotocol::DeriveSessionTag(
        sessionDescriptor.sessionId);
    segmentDescriptor.segmentOrdinal = segmentOrdinal;
    segmentDescriptor.rawOffset = rawOffset;
    segmentDescriptor.rawSize = static_cast<std::uint64_t>(rawBytes.size());
    segmentDescriptor.encodedSize = static_cast<std::uint64_t>(
        encodedBytes.size());
    segmentDescriptor.compressionCodec = compressionCodec;
    segmentDescriptor.outerFecMode = outerFecMode;
    segmentDescriptor.outerBlockBytes = outerBlockBytes;
    segmentDescriptor.rawDigest = pbprotocol::RawDigest{
        pbprotocol::ComputeBlake3Digest(rawBytes)};
    segmentDescriptor.encodedDigest = pbprotocol::EncodedDigest{
        pbprotocol::ComputeBlake3Digest(encodedBytes)};
    segmentDescriptor.wirehairV2SerializedProfile = wirehairProfile;
    return segmentDescriptor;
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
    std::vector<std::byte> recordBytes(sizeResult.Value());
    REQUIRE(pbprotocol::SerializeControlRecord(record, recordBytes));
    return recordBytes;
}

[[nodiscard]] std::vector<std::byte> MakeSessionControlRecord(
    const pbprotocol::SessionDescriptor& sessionDescriptor,
    const pbprotocol::ReceiverResourcePolicy& resourcePolicy,
    const std::uint64_t controlSequence = 1)
{
    std::array<std::byte, pbprotocol::kSessionDescriptorPayloadBytes> payload{};
    REQUIRE(pbprotocol::SerializeSessionDescriptor(
        sessionDescriptor,
        resourcePolicy,
        payload));
    return WrapControlPayload(
        pbprotocol::ControlRecordType::SessionDescriptor,
        controlSequence,
        pbprotocol::DeriveSessionTag(sessionDescriptor.sessionId),
        payload);
}

[[nodiscard]] std::vector<std::byte> MakeSegmentControlRecord(
    const pbprotocol::SegmentDescriptor& segmentDescriptor,
    const pbprotocol::SessionDescriptor& sessionDescriptor,
    const pbprotocol::ReceiverResourcePolicy& resourcePolicy,
    const std::uint64_t controlSequence = 2)
{
    const auto sizeResult = pbprotocol::GetSerializedSize(segmentDescriptor);
    REQUIRE(sizeResult);
    std::vector<std::byte> payload(sizeResult.Value());
    REQUIRE(pbprotocol::SerializeSegmentDescriptor(
        segmentDescriptor,
        sessionDescriptor,
        resourcePolicy,
        payload));
    return WrapControlPayload(
        pbprotocol::ControlRecordType::SegmentDescriptor,
        controlSequence,
        segmentDescriptor.sessionTag,
        payload);
}

[[nodiscard]] pbprotocol::FinalManifest MakeFinalManifest(
    const pbprotocol::SessionDescriptor& sessionDescriptor,
    const std::span<const std::byte> wholeFileBytes)
{
    return pbprotocol::FinalManifest{
        sessionDescriptor.sessionId,
        sessionDescriptor.originalFileSize,
        sessionDescriptor.segmentCount,
        pbprotocol::WholeFileDigest{
            pbprotocol::ComputeBlake3Digest(wholeFileBytes)},
        pbprotocol::DigestAlgorithm::Blake3_256};
}

[[nodiscard]] std::vector<std::byte> MakeManifestControlRecord(
    const pbprotocol::FinalManifest& finalManifest,
    const pbprotocol::SessionDescriptor& sessionDescriptor,
    const pbprotocol::ReceiverResourcePolicy& resourcePolicy,
    const std::uint64_t controlSequence = 3)
{
    std::array<std::byte, pbprotocol::kFinalManifestPayloadBytes> payload{};
    REQUIRE(pbprotocol::SerializeFinalManifest(
        finalManifest,
        sessionDescriptor,
        resourcePolicy,
        payload));
    return WrapControlPayload(
        pbprotocol::ControlRecordType::FinalManifest,
        controlSequence,
        pbprotocol::DeriveSessionTag(finalManifest.sessionId),
        payload);
}

[[nodiscard]] std::vector<std::byte> MakeControlFragment(
    const std::span<const std::byte> recordBytes,
    const std::uint64_t controlRecordId,
    const std::uint16_t fragmentIndex,
    const std::uint16_t maximumFragmentPayloadBytes = 16)
{
    const auto fragmentResult = pbprotocol::GetControlFragment(
        controlRecordId,
        recordBytes,
        fragmentIndex,
        maximumFragmentPayloadBytes);
    REQUIRE(fragmentResult);
    const auto sizeResult = pbprotocol::GetSerializedSize(
        fragmentResult.Value());
    REQUIRE(sizeResult);
    std::vector<std::byte> fragmentBytes(sizeResult.Value());
    REQUIRE(pbprotocol::SerializeControlFragment(
        fragmentResult.Value(),
        fragmentBytes));
    return fragmentBytes;
}

[[nodiscard]] std::vector<EncodedTransportBlock> EncodeDirectRepeatBlocks(
    const std::span<const std::byte> encodedBytes,
    const std::uint32_t outerBlockBytes)
{
    auto encoderResult = pbouterfec::DirectRepeatEncoder::Create(
        encodedBytes,
        outerBlockBytes);
    REQUIRE(encoderResult);
    pbouterfec::DirectRepeatEncoder encoder =
        std::move(encoderResult).Value();

    std::vector<EncodedTransportBlock> blocks;
    for (std::uint64_t blockIndex = 0;
         blockIndex < encoder.GetBlockCount();
         blockIndex++)
    {
        EncodedTransportBlock block;
        block.outerBlockId = static_cast<std::uint32_t>(blockIndex);
        block.paddedPayload.resize(outerBlockBytes);
        const auto encodeResult = encoder.EncodeBlock(
            block.outerBlockId,
            block.paddedPayload);
        REQUIRE(encodeResult);
        block.declaredPayloadBytes = static_cast<std::uint16_t>(
            encodeResult.Value());
        blocks.push_back(std::move(block));
    }
    return blocks;
}

[[nodiscard]] std::vector<EncodedTransportBlock> EncodeWirehairBlocks(
    const std::span<const std::byte> encodedBytes,
    const std::uint32_t outerBlockBytes,
    pbprotocol::WirehairV2SerializedProfile& serializedProfile)
{
    auto encoderResult = pbouterfec::WirehairV2Encoder::Create(
        encodedBytes,
        outerBlockBytes);
    REQUIRE(encoderResult);
    pbouterfec::WirehairV2Encoder encoder = std::move(encoderResult).Value();
    serializedProfile = encoder.GetSerializedProfile();

    std::vector<EncodedTransportBlock> blocks;
    for (std::uint32_t blockId = 0;
         blockId < encoder.GetBlockCount();
         blockId++)
    {
        EncodedTransportBlock block;
        block.outerBlockId = blockId;
        block.paddedPayload.resize(outerBlockBytes);
        const auto encodeResult = encoder.EncodeBlock(
            blockId,
            block.paddedPayload);
        REQUIRE(encodeResult);
        block.declaredPayloadBytes = static_cast<std::uint16_t>(
            encodeResult.Value());
        blocks.push_back(std::move(block));
    }
    return blocks;
}

[[nodiscard]] pbreceiver::ReceivedTransportBlock MakeReceivedBlock(
    const pbprotocol::SessionTag sessionTag,
    const std::uint64_t segmentOrdinal,
    const EncodedTransportBlock& block)
{
    return pbreceiver::ReceivedTransportBlock{
        sessionTag,
        segmentOrdinal,
        block.outerBlockId,
        block.declaredPayloadBytes,
        block.paddedPayload};
}

[[nodiscard]] pbreceiver::ReceiverIngress MakeReceiver(
    const pbprotocol::ReceiverResourcePolicy& resourcePolicy,
    const std::uint32_t outerBlockBytes)
{
    auto receiverResult = pbreceiver::ReceiverIngress::Create(
        resourcePolicy,
        outerBlockBytes);
    REQUIRE(receiverResult);
    return std::move(receiverResult).Value();
}

void RequireProtocolError(
    const pbreceiver::ReceiverError& receiverError,
    const pbprotocol::ProtocolErrorCode expectedCode)
{
    REQUIRE(std::holds_alternative<pbprotocol::ProtocolError>(receiverError));
    REQUIRE(std::get<pbprotocol::ProtocolError>(receiverError).code ==
        expectedCode);
}

void RequireOuterFecError(
    const pbreceiver::ReceiverError& receiverError,
    const pbouterfec::OuterFecErrorCode expectedCode)
{
    REQUIRE(std::holds_alternative<pbouterfec::OuterFecError>(receiverError));
    REQUIRE(std::get<pbouterfec::OuterFecError>(receiverError).code ==
        expectedCode);
}

} // namespace

TEST_CASE("ReceiverIngress DirectRepeat orphan flow is authoritative end to end",
          "[pbreceiver][orphan][direct-repeat][integration]")
{
    constexpr std::uint32_t outerBlockBytes = 8;
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    const std::vector<std::byte> message = MakeBytes(20, 0x31);
    const pbprotocol::SessionDescriptor sessionDescriptor =
        MakeSessionDescriptor(
            MakeSessionId(0x10),
            message.size(),
            1);
    const pbprotocol::SegmentDescriptor segmentDescriptor =
        MakeSegmentDescriptor(
            sessionDescriptor,
            0,
            0,
            message,
            message,
            pbprotocol::CompressionCodec::Raw,
            pbprotocol::OuterFecMode::DirectRepeat,
            outerBlockBytes);
    const std::vector<EncodedTransportBlock> blocks =
        EncodeDirectRepeatBlocks(message, outerBlockBytes);
    REQUIRE(blocks.size() == 3);

    pbreceiver::ReceiverIngress receiver = MakeReceiver(
        resourcePolicy,
        outerBlockBytes);
    for (const EncodedTransportBlock& block : blocks)
    {
        const auto dataResult = receiver.ReceiveDataBlock(
            MakeReceivedBlock(segmentDescriptor.sessionTag, 0, block));
        REQUIRE(dataResult);
        REQUIRE(dataResult.Value().disposition ==
            pbreceiver::ReceiverDataDisposition::CachedOrphan);
        REQUIRE(dataResult.Value().outerSymbolAdmission ==
            pbreceiver::ReceiverOuterSymbolAdmission::Unique);
    }
    const auto repeatedOrphan = receiver.ReceiveDataBlock(
        MakeReceivedBlock(segmentDescriptor.sessionTag, 0, blocks[0]));
    REQUIRE(repeatedOrphan);
    REQUIRE(repeatedOrphan.Value().outerSymbolAdmission ==
        pbreceiver::ReceiverOuterSymbolAdmission::IdenticalDuplicate);
    pbreceiver::ReceiverResourceTelemetrySnapshot telemetry =
        receiver.GetTelemetry();
    REQUIRE(telemetry.orphanCachedBlockCount == 3);
    REQUIRE(telemetry.orphanCachedBytes == 24);
    REQUIRE(telemetry.activeOuterFecDecoderCount == 0);
    REQUIRE(telemetry.reservedOuterFecDecoderBytes == 0);
    REQUIRE(telemetry.outputReservationAutoAcceptedCount == 0);
    REQUIRE(telemetry.outputReservationRequiresConfirmationCount == 0);
    REQUIRE(telemetry.outputReservationDeniedCount == 0);
    REQUIRE(telemetry.decompressionResourceRejectedCount == 0);

    const auto sessionResult = receiver.ReceiveControlRecord(
        MakeSessionControlRecord(sessionDescriptor, resourcePolicy));
    REQUIRE(sessionResult);
    REQUIRE(sessionResult.Value().outputReservationDecision ==
        pbprotocol::OutputReservationDecision::AutoAccept);
    REQUIRE_FALSE(sessionResult.Value().completedSegment.has_value());

    auto segmentResult = receiver.ReceiveControlRecord(
        MakeSegmentControlRecord(
            segmentDescriptor,
            sessionDescriptor,
            resourcePolicy));
    REQUIRE(segmentResult);
    REQUIRE(segmentResult.Value().completedSegment.has_value());
    REQUIRE(segmentResult.Value().replayedOrphanBlocks.size() == blocks.size());
    for (std::size_t blockIndex = 0; blockIndex < blocks.size(); blockIndex++)
    {
        const pbprotocol::OrphanTransportBlockEntry& replayed =
            segmentResult.Value().replayedOrphanBlocks[blockIndex];
        REQUIRE(replayed.outerBlockId == blocks[blockIndex].outerBlockId);
        REQUIRE(replayed.declaredPayloadBytes == blocks[blockIndex].declaredPayloadBytes);
        REQUIRE(replayed.paddedPayload == blocks[blockIndex].paddedPayload);
    }
    REQUIRE(segmentResult.Value().completedSegment->encodedBytes == message);
    REQUIRE(segmentResult.Value().completedSegment->boundSegmentDescriptor.
        GetDescriptor() == segmentDescriptor);

    telemetry = receiver.GetTelemetry();
    REQUIRE(telemetry.orphanCachedBlockCount == 0);
    REQUIRE(telemetry.orphanCachedBytes == 0);
    REQUIRE(telemetry.activeOuterFecDecoderCount == 1);
    REQUIRE(telemetry.reservedOuterFecDecoderBytes > 0);
    REQUIRE(telemetry.orphanAdmittedBlockCount == 3);
    REQUIRE(telemetry.outputReservationAutoAcceptedCount == 1);

    auto verifiedResult = receiver.VerifyRecoveredSegment(
        std::move(*segmentResult.Value().completedSegment));
    REQUIRE(verifiedResult);
    REQUIRE(std::ranges::equal(verifiedResult.Value().GetRawBytes(), message));
    const auto commitResult = receiver.CommitStoredSegment(
        std::move(verifiedResult).Value());
    REQUIRE(commitResult);
    REQUIRE(commitResult.Value() ==
        pbreceiver::ReceiverSegmentCommitDisposition::Committed);
    REQUIRE(receiver.GetTelemetry().reservedOuterFecDecoderBytes == 0);

    const auto repeatedBlockResult = receiver.ReceiveDataBlock(
        MakeReceivedBlock(segmentDescriptor.sessionTag, 0, blocks[0]));
    REQUIRE(repeatedBlockResult);
    REQUIRE(repeatedBlockResult.Value().disposition ==
        pbreceiver::ReceiverDataDisposition::AlreadyCompleted);
    REQUIRE(repeatedBlockResult.Value().outerSymbolAdmission ==
        pbreceiver::ReceiverOuterSymbolAdmission::AlreadyCompleted);
    REQUIRE(receiver.GetTelemetry().activeOuterFecDecoderCount == 0);

    const auto removeResult = receiver.RemoveSession(sessionDescriptor.sessionId);
    REQUIRE(removeResult);
    REQUIRE(removeResult.Value());
    REQUIRE(receiver.GetTelemetry().activeSessionCount == 0);
}

TEST_CASE("ReceiverIngress commits completion only after verified storage",
          "[pbreceiver][completion][finalization][regression]")
{
    constexpr std::uint32_t outerBlockBytes = 8;
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    const std::vector<std::byte> message = MakeBytes(20, 0x35);
    const pbprotocol::SessionDescriptor sessionDescriptor =
        MakeSessionDescriptor(MakeSessionId(0x14), message.size(), 1);
    const pbprotocol::SegmentDescriptor segmentDescriptor =
        MakeSegmentDescriptor(
            sessionDescriptor,
            0,
            0,
            message,
            message,
            pbprotocol::CompressionCodec::Raw,
            pbprotocol::OuterFecMode::DirectRepeat,
            outerBlockBytes);
    const pbprotocol::FinalManifest finalManifest = MakeFinalManifest(
        sessionDescriptor,
        message);
    const std::vector<EncodedTransportBlock> blocks =
        EncodeDirectRepeatBlocks(message, outerBlockBytes);

    pbreceiver::ReceiverIngress receiver = MakeReceiver(
        resourcePolicy,
        outerBlockBytes);
    REQUIRE(receiver.ReceiveControlRecord(
        MakeSessionControlRecord(sessionDescriptor, resourcePolicy)));
    REQUIRE(receiver.ReceiveControlRecord(MakeSegmentControlRecord(
        segmentDescriptor,
        sessionDescriptor,
        resourcePolicy)));
    REQUIRE(receiver.ReceiveControlRecord(MakeManifestControlRecord(
        finalManifest,
        sessionDescriptor,
        resourcePolicy)));

    pbreceiver::ReceiverDataAdmission readyAdmission;
    for (const EncodedTransportBlock& block : blocks)
    {
        auto dataResult = receiver.ReceiveDataBlock(
            MakeReceivedBlock(segmentDescriptor.sessionTag, 0, block));
        REQUIRE(dataResult);
        readyAdmission = std::move(dataResult).Value();
    }
    REQUIRE(readyAdmission.disposition ==
        pbreceiver::ReceiverDataDisposition::EncodedSegmentReady);
    REQUIRE(readyAdmission.completedSegment);

    const auto outerReadyFinalization = receiver.PrepareFinalization(
        segmentDescriptor.sessionTag);
    REQUIRE_FALSE(outerReadyFinalization);
    RequireProtocolError(
        outerReadyFinalization.Error(),
        pbprotocol::ProtocolErrorCode::SegmentRecoveryIncomplete);

    {
        auto verifiedResult = receiver.VerifyRecoveredSegment(
            std::move(*readyAdmission.completedSegment));
        REQUIRE(verifiedResult);
        REQUIRE(std::ranges::equal(
            verifiedResult.Value().GetRawBytes(),
            message));

        const auto storageFailureFinalization = receiver.PrepareFinalization(
            segmentDescriptor.sessionTag);
        REQUIRE_FALSE(storageFailureFinalization);
        RequireProtocolError(
            storageFailureFinalization.Error(),
            pbprotocol::ProtocolErrorCode::SegmentRecoveryIncomplete);
        // The verified capability is deliberately discarded without commit,
        // exactly as a failed .part write/flush path must behave.
    }

    for (const EncodedTransportBlock& block : blocks)
    {
        auto dataResult = receiver.ReceiveDataBlock(
            MakeReceivedBlock(segmentDescriptor.sessionTag, 0, block));
        REQUIRE(dataResult);
        readyAdmission = std::move(dataResult).Value();
    }
    REQUIRE(readyAdmission.completedSegment);
    auto verifiedResult = receiver.VerifyRecoveredSegment(
        std::move(*readyAdmission.completedSegment));
    REQUIRE(verifiedResult);
    pbreceiver::ReceiverVerifiedSegment verifiedSegment =
        std::move(verifiedResult).Value();

    const auto movedFromCommitResult = receiver.CommitStoredSegment(
        std::move(verifiedResult).Value());
    REQUIRE_FALSE(movedFromCommitResult);
    RequireProtocolError(
        movedFromCommitResult.Error(),
        pbprotocol::ProtocolErrorCode::SegmentRecoveryIncomplete);
    REQUIRE_FALSE(receiver.PrepareFinalization(segmentDescriptor.sessionTag));

    const auto commitResult = receiver.CommitStoredSegment(
        std::move(verifiedSegment));
    REQUIRE(commitResult);
    REQUIRE(commitResult.Value() ==
        pbreceiver::ReceiverSegmentCommitDisposition::Committed);
    const auto repeatedCommitResult = receiver.CommitStoredSegment(
        std::move(verifiedSegment));
    REQUIRE(repeatedCommitResult);
    REQUIRE(repeatedCommitResult.Value() ==
        pbreceiver::ReceiverSegmentCommitDisposition::AlreadyCommitted);

    const auto finalizationResult = receiver.PrepareFinalization(
        segmentDescriptor.sessionTag);
    REQUIRE(finalizationResult);
    REQUIRE(finalizationResult.Value() == finalManifest);
}

TEST_CASE("ReceiverIngress pending verified capabilities cannot bypass terminal errors",
          "[pbreceiver][completion][conflict][regression]")
{
    constexpr std::uint32_t outerBlockBytes = 8;
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    const std::vector<std::byte> message = MakeBytes(20, 0xA5);
    const pbprotocol::SessionDescriptor sessionDescriptor =
        MakeSessionDescriptor(MakeSessionId(0x74), message.size(), 1);
    const pbprotocol::SegmentDescriptor segmentDescriptor = MakeSegmentDescriptor(
        sessionDescriptor, 0, 0, message, message,
        pbprotocol::CompressionCodec::Raw,
        pbprotocol::OuterFecMode::DirectRepeat, outerBlockBytes);
    const pbprotocol::FinalManifest finalManifest = MakeFinalManifest(
        sessionDescriptor, message);
    pbreceiver::ReceiverIngress receiver = MakeReceiver(resourcePolicy, outerBlockBytes);
    REQUIRE(receiver.ReceiveControlRecord(
        MakeSessionControlRecord(sessionDescriptor, resourcePolicy)));
    const auto segmentResult = receiver.ReceiveControlRecord(MakeSegmentControlRecord(
        segmentDescriptor, sessionDescriptor, resourcePolicy));
    REQUIRE(segmentResult);
    REQUIRE(segmentResult.Value().controlAdmission.boundSegmentDescriptor);
    REQUIRE(receiver.ReceiveControlRecord(MakeManifestControlRecord(
        finalManifest, sessionDescriptor, resourcePolicy)));
    std::vector<EncodedTransportBlock> blocks = EncodeDirectRepeatBlocks(message, outerBlockBytes);
    pbreceiver::ReceiverDataAdmission readyAdmission;
    for (const EncodedTransportBlock& block : blocks)
    {
        auto dataResult = receiver.ReceiveDataBlock(MakeReceivedBlock(segmentDescriptor.sessionTag, 0, block));
        REQUIRE(dataResult);
        readyAdmission = std::move(dataResult).Value();
    }
    REQUIRE(readyAdmission.completedSegment);
    auto verifiedResult = receiver.VerifyRecoveredSegment(std::move(*readyAdmission.completedSegment));
    REQUIRE(verifiedResult);

    SECTION("Control descriptor conflict remains terminal")
    {
        pbprotocol::SegmentDescriptor conflictingDescriptor = segmentDescriptor;
        conflictingDescriptor.rawDigest.bytes[0] ^= std::byte{1};
        conflictingDescriptor.encodedDigest.bytes[0] ^= std::byte{1};
        const auto conflictResult = receiver.ReceiveControlRecord(MakeSegmentControlRecord(
            conflictingDescriptor, sessionDescriptor, resourcePolicy, 4));
        REQUIRE_FALSE(conflictResult);
        RequireProtocolError(conflictResult.Error(), pbprotocol::ProtocolErrorCode::DescriptorConflict);
        const auto commitResult = receiver.CommitStoredSegment(std::move(verifiedResult).Value());
        REQUIRE_FALSE(commitResult);
        RequireProtocolError(commitResult.Error(), pbprotocol::ProtocolErrorCode::DescriptorConflict);
        const auto finalizationResult = receiver.PrepareFinalization(segmentDescriptor.sessionTag);
        REQUIRE_FALSE(finalizationResult);
        RequireProtocolError(finalizationResult.Error(), pbprotocol::ProtocolErrorCode::DescriptorConflict);
    }

    SECTION("Data payload conflict remains terminal")
    {
        blocks[0].paddedPayload[0] ^= std::byte{1};
        const auto conflictResult = receiver.ReceiveDataBlock(
            MakeReceivedBlock(segmentDescriptor.sessionTag, 0, blocks[0]));
        REQUIRE_FALSE(conflictResult);
        const auto commitResult = receiver.CommitStoredSegment(std::move(verifiedResult).Value());
        REQUIRE_FALSE(commitResult);
        REQUIRE(std::holds_alternative<pbouterfec::OuterFecError>(commitResult.Error()));
        REQUIRE(std::get<pbouterfec::OuterFecError>(commitResult.Error()).code ==
            pbouterfec::OuterFecErrorCode::OuterBlockConflict);
        const auto finalizationResult = receiver.PrepareFinalization(segmentDescriptor.sessionTag);
        REQUIRE_FALSE(finalizationResult);
        REQUIRE(std::holds_alternative<pbouterfec::OuterFecError>(finalizationResult.Error()));
        REQUIRE(std::get<pbouterfec::OuterFecError>(finalizationResult.Error()).code ==
            pbouterfec::OuterFecErrorCode::OuterBlockConflict);
    }
    REQUIRE(receiver.GetTelemetry().activeOuterFecDecoderCount == 0);
    REQUIRE(receiver.GetTelemetry().reservedOuterFecDecoderBytes == 0);
}

TEST_CASE("ReceiverIngress retains ready Wirehair fingerprints until storage commit",
          "[pbreceiver][completion][wirehair][conflict][regression]")
{
    constexpr std::uint32_t outerBlockBytes = 64;
    const pbprotocol::ReceiverResourcePolicy resourcePolicy = pbprotocol::GetDefaultReceiverResourcePolicy();
    const std::vector<std::byte> message = MakeBytes(2053, 0xD3);
    const pbprotocol::SessionDescriptor sessionDescriptor = MakeSessionDescriptor(MakeSessionId(0x75), message.size(), 1);
    pbprotocol::WirehairV2SerializedProfile serializedProfile{};
    std::vector<EncodedTransportBlock> blocks = EncodeWirehairBlocks(message, outerBlockBytes, serializedProfile);
    const pbprotocol::SegmentDescriptor segmentDescriptor = MakeSegmentDescriptor(
        sessionDescriptor, 0, 0, message, message, pbprotocol::CompressionCodec::Raw,
        pbprotocol::OuterFecMode::WirehairV2, outerBlockBytes, serializedProfile);
    const pbprotocol::FinalManifest finalManifest = MakeFinalManifest(sessionDescriptor, message);
    pbreceiver::ReceiverIngress receiver = MakeReceiver(resourcePolicy, outerBlockBytes);
    REQUIRE(receiver.ReceiveControlRecord(MakeSessionControlRecord(sessionDescriptor, resourcePolicy)));
    REQUIRE(receiver.ReceiveControlRecord(MakeSegmentControlRecord(segmentDescriptor, sessionDescriptor, resourcePolicy)));
    REQUIRE(receiver.ReceiveControlRecord(MakeManifestControlRecord(finalManifest, sessionDescriptor, resourcePolicy)));

    pbreceiver::ReceiverDataAdmission readyAdmission;
    for (const EncodedTransportBlock& block : blocks)
    {
        auto dataResult = receiver.ReceiveDataBlock(MakeReceivedBlock(segmentDescriptor.sessionTag, 0, block));
        REQUIRE(dataResult);
        readyAdmission = std::move(dataResult).Value();
    }
    REQUIRE(readyAdmission.completedSegment);
    auto verifiedResult = receiver.VerifyRecoveredSegment(std::move(*readyAdmission.completedSegment));
    REQUIRE(verifiedResult);

    SECTION("an accepted ID with conflicting bytes remains terminal after Ready")
    {
        blocks[0].paddedPayload[0] ^= std::byte{1};
        const auto conflictResult = receiver.ReceiveDataBlock(MakeReceivedBlock(segmentDescriptor.sessionTag, 0, blocks[0]));
        REQUIRE_FALSE(conflictResult);
        RequireOuterFecError(conflictResult.Error(), pbouterfec::OuterFecErrorCode::OuterBlockConflict);
        const auto commitResult = receiver.CommitStoredSegment(std::move(verifiedResult).Value());
        REQUIRE_FALSE(commitResult);
        RequireOuterFecError(commitResult.Error(), pbouterfec::OuterFecErrorCode::OuterBlockConflict);
        const auto finalizationResult = receiver.PrepareFinalization(segmentDescriptor.sessionTag);
        REQUIRE_FALSE(finalizationResult);
        RequireOuterFecError(finalizationResult.Error(), pbouterfec::OuterFecErrorCode::OuterBlockConflict);
    }

    SECTION("new repair IDs do not poison a pending verified Segment")
    {
        const auto telemetryBefore = receiver.GetTelemetry();
        REQUIRE(telemetryBefore.activeOuterFecDecoderCount == 1);
        REQUIRE(telemetryBefore.reservedOuterFecDecoderBytes > 0);
        auto encoderResult = pbouterfec::WirehairV2Encoder::Recreate(message, segmentDescriptor);
        REQUIRE(encoderResult);
        EncodedTransportBlock repairBlock;
        repairBlock.outerBlockId = encoderResult.Value().GetBlockCount();
        repairBlock.paddedPayload.resize(outerBlockBytes);
        const auto encodeResult = encoderResult.Value().EncodeBlock(repairBlock.outerBlockId, repairBlock.paddedPayload);
        REQUIRE(encodeResult);
        repairBlock.declaredPayloadBytes = static_cast<std::uint16_t>(encodeResult.Value());
        const auto repairResult = receiver.ReceiveDataBlock(MakeReceivedBlock(segmentDescriptor.sessionTag, 0, repairBlock));
        REQUIRE(repairResult);
        REQUIRE(repairResult.Value().disposition == pbreceiver::ReceiverDataDisposition::EncodedSegmentReady);
        REQUIRE(repairResult.Value().completedSegment);
        REQUIRE(repairResult.Value().completedSegment->encodedBytes == message);
        REQUIRE(receiver.GetTelemetry().activeOuterFecDecoderCount == 1);
        REQUIRE(receiver.GetTelemetry().reservedOuterFecDecoderBytes == telemetryBefore.reservedOuterFecDecoderBytes);
        const auto duplicateResult = receiver.ReceiveDataBlock(MakeReceivedBlock(segmentDescriptor.sessionTag, 0, blocks[0]));
        REQUIRE(duplicateResult);
        REQUIRE(duplicateResult.Value().completedSegment);
        REQUIRE(duplicateResult.Value().completedSegment->encodedBytes == message);
        REQUIRE_FALSE(receiver.PrepareFinalization(segmentDescriptor.sessionTag));
        const auto commitResult = receiver.CommitStoredSegment(std::move(verifiedResult).Value());
        REQUIRE(commitResult);
        REQUIRE(commitResult.Value() == pbreceiver::ReceiverSegmentCommitDisposition::Committed);
        REQUIRE(receiver.PrepareFinalization(segmentDescriptor.sessionTag));
    }
    REQUIRE(receiver.GetTelemetry().activeOuterFecDecoderCount == 0);
    REQUIRE(receiver.GetTelemetry().reservedOuterFecDecoderBytes == 0);
}

TEST_CASE("ReceiverIngress verification failure never commits recovered state",
          "[pbreceiver][completion][digest][regression]")
{
    constexpr std::uint32_t outerBlockBytes = 64;
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    const std::vector<std::byte> message(4096, std::byte{0x5A});
    pbcompression::CompressionSettings compressionSettings;
    compressionSettings.maxWindowLog = 12;
    const auto compressionResult = pbcompression::CompressSegment(
        message,
        compressionSettings);
    REQUIRE(compressionResult);
    REQUIRE(compressionResult.Value().codec ==
        pbprotocol::CompressionCodec::Zstandard);
    std::vector<std::byte> encodedBytes = compressionResult.Value().bytes;
    bool corruptFrame = false;
    SECTION("wrong RawDigest after successful decompression")
    {
        corruptFrame = false;
    }
    SECTION("malformed zstd with a matching encoded digest")
    {
        encodedBytes[0] ^= std::byte{0x80};
        corruptFrame = true;
    }
    const pbprotocol::SessionDescriptor sessionDescriptor =
        MakeSessionDescriptor(MakeSessionId(0x15), message.size(), 1);
    pbprotocol::SegmentDescriptor segmentDescriptor = MakeSegmentDescriptor(
        sessionDescriptor,
        0,
        0,
        message,
        encodedBytes,
        pbprotocol::CompressionCodec::Zstandard,
        pbprotocol::OuterFecMode::DirectRepeat,
        outerBlockBytes);
    if (!corruptFrame)
    {
        segmentDescriptor.rawDigest.bytes[0] ^= std::byte{0x40};
    }
    const pbprotocol::FinalManifest finalManifest = MakeFinalManifest(
        sessionDescriptor,
        message);
    const std::vector<EncodedTransportBlock> blocks =
        EncodeDirectRepeatBlocks(encodedBytes, outerBlockBytes);

    pbreceiver::ReceiverIngress receiver = MakeReceiver(
        resourcePolicy,
        outerBlockBytes);
    REQUIRE(receiver.ReceiveControlRecord(
        MakeSessionControlRecord(sessionDescriptor, resourcePolicy)));
    REQUIRE(receiver.ReceiveControlRecord(MakeSegmentControlRecord(
        segmentDescriptor,
        sessionDescriptor,
        resourcePolicy)));
    REQUIRE(receiver.ReceiveControlRecord(MakeManifestControlRecord(
        finalManifest,
        sessionDescriptor,
        resourcePolicy)));

    pbreceiver::ReceiverDataAdmission readyAdmission;
    for (const EncodedTransportBlock& block : blocks)
    {
        auto dataResult = receiver.ReceiveDataBlock(
            MakeReceivedBlock(segmentDescriptor.sessionTag, 0, block));
        REQUIRE(dataResult);
        readyAdmission = std::move(dataResult).Value();
    }
    REQUIRE(readyAdmission.completedSegment);
    const auto verifyResult = receiver.VerifyRecoveredSegment(
        std::move(*readyAdmission.completedSegment));
    REQUIRE_FALSE(verifyResult);
    if (corruptFrame)
    {
        REQUIRE(std::holds_alternative<pbcompression::CompressionError>(verifyResult.Error()));
        REQUIRE(std::get<pbcompression::CompressionError>(verifyResult.Error()).code ==
            pbcompression::CompressionErrorCode::CorruptedFrame);
    }
    else
    {
        RequireProtocolError(verifyResult.Error(), pbprotocol::ProtocolErrorCode::DigestMismatch);
    }

    const auto finalizationResult = receiver.PrepareFinalization(
        segmentDescriptor.sessionTag);
    REQUIRE_FALSE(finalizationResult);
    RequireProtocolError(
        finalizationResult.Error(),
        pbprotocol::ProtocolErrorCode::SegmentRecoveryIncomplete);

    for (const EncodedTransportBlock& block : blocks)
    {
        const auto replayResult = receiver.ReceiveDataBlock(
            MakeReceivedBlock(segmentDescriptor.sessionTag, 0, block));
        REQUIRE(replayResult);
        if (static_cast<std::size_t>(block.outerBlockId) + 1U == blocks.size())
        {
            REQUIRE(replayResult.Value().disposition ==
                pbreceiver::ReceiverDataDisposition::EncodedSegmentReady);
        }
    }
}

TEST_CASE("ReceiverIngress finalizes a canonical empty Session without Segments",
          "[pbreceiver][completion][finalization][empty]")
{
    constexpr std::uint32_t outerBlockBytes = 8;
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    const pbprotocol::SessionDescriptor sessionDescriptor =
        MakeSessionDescriptor(MakeSessionId(0x16), 0, 0);
    const pbprotocol::FinalManifest finalManifest{
        sessionDescriptor.sessionId,
        0,
        0,
        pbprotocol::GetEmptyBlake3WholeFileDigest(),
        pbprotocol::DigestAlgorithm::Blake3_256};
    const pbprotocol::SessionTag sessionTag = pbprotocol::DeriveSessionTag(
        sessionDescriptor.sessionId);

    pbreceiver::ReceiverIngress receiver = MakeReceiver(
        resourcePolicy,
        outerBlockBytes);
    REQUIRE(receiver.ReceiveControlRecord(
        MakeSessionControlRecord(sessionDescriptor, resourcePolicy)));
    const auto missingManifestResult = receiver.PrepareFinalization(sessionTag);
    REQUIRE_FALSE(missingManifestResult);
    RequireProtocolError(
        missingManifestResult.Error(),
        pbprotocol::ProtocolErrorCode::MissingFinalManifest);

    REQUIRE(receiver.ReceiveControlRecord(MakeManifestControlRecord(
        finalManifest,
        sessionDescriptor,
        resourcePolicy)));
    const auto finalizationResult = receiver.PrepareFinalization(sessionTag);
    REQUIRE(finalizationResult);
    REQUIRE(finalizationResult.Value() == finalManifest);
}

TEST_CASE("ReceiverIngress restores committed raw bytes independently of their compression codec",
          "[pbreceiver][completion][resume-completed][regression]")
{
    constexpr std::uint32_t outerBlockBytes = 64;
    const auto resourcePolicy = pbprotocol::GetDefaultReceiverResourcePolicy();
    const std::vector<std::byte> rawBytes(4096, std::byte{0x5A});
    const auto compressed = pbcompression::CompressSegment(rawBytes, pbcompression::CompressionSettings{});
    REQUIRE(compressed);
    REQUIRE(compressed.Value().codec == pbprotocol::CompressionCodec::Zstandard);
    for (const bool useCompression : {false, true})
    {
        const auto session = MakeSessionDescriptor(MakeSessionId(useCompression ? 0x79 : 0x78), rawBytes.size(), 1);
        const auto descriptor = MakeSegmentDescriptor(session, 0, 0, rawBytes,
            useCompression ? compressed.Value().bytes : rawBytes,
            useCompression ? pbprotocol::CompressionCodec::Zstandard : pbprotocol::CompressionCodec::Raw,
            pbprotocol::OuterFecMode::DirectRepeat, outerBlockBytes);
        const auto sessionControl = MakeSessionControlRecord(session, resourcePolicy);
        const auto segmentControl = MakeSegmentControlRecord(descriptor, session, resourcePolicy);
        const auto finalManifest = MakeFinalManifest(session, rawBytes);
        const auto finalControl = MakeManifestControlRecord(finalManifest, session, resourcePolicy);
        const pbprotocol::ResumeCompletedSegmentRecord record{
            session.sessionId, 0, 0, descriptor.rawSize, descriptor.rawDigest};
        {
            auto originalReceiver = MakeReceiver(resourcePolicy, outerBlockBytes);
            REQUIRE(originalReceiver.ReceiveControlRecord(sessionControl));
            const auto admission = originalReceiver.ReceiveControlRecord(segmentControl);
            REQUIRE(admission);
            REQUIRE(admission.Value().controlAdmission.boundSegmentDescriptor);
            auto verified = originalReceiver.VerifyRecoveredSegment(pbreceiver::ReceiverCompletedSegment(
                *admission.Value().controlAdmission.boundSegmentDescriptor,
                useCompression ? compressed.Value().bytes : rawBytes));
            REQUIRE(verified);
            REQUIRE(originalReceiver.CommitStoredSegment(std::move(verified).Value()));
        }
        auto receiver = MakeReceiver(resourcePolicy, outerBlockBytes);
        REQUIRE(receiver.ReceiveControlRecord(sessionControl));
        const auto segmentAdmission = receiver.ReceiveControlRecord(segmentControl);
        REQUIRE(segmentAdmission);
        REQUIRE(segmentAdmission.Value().controlAdmission.boundSegmentDescriptor);
        REQUIRE(receiver.ReceiveControlRecord(finalControl));
        if (useCompression)
        {
            // Preserve the original failure primitive after fixing the resume
            // caller: stored raw bytes cannot enter the encoded recovery API.
            const auto oldPath = receiver.VerifyRecoveredSegment(pbreceiver::ReceiverCompletedSegment(
                *segmentAdmission.Value().controlAdmission.boundSegmentDescriptor, rawBytes));
            REQUIRE_FALSE(oldPath);
            REQUIRE(std::holds_alternative<pbcompression::CompressionError>(oldPath.Error()));
            const auto& shapeError = std::get<pbcompression::CompressionError>(oldPath.Error());
            REQUIRE(shapeError.code == pbcompression::CompressionErrorCode::EncodedSizeMismatch);
            REQUIRE(shapeError.detail == rawBytes.size());
            REQUIRE_FALSE(receiver.PrepareFinalization(descriptor.sessionTag));
        }
        std::vector<std::byte> storedRawBytes = rawBytes;
        auto verified = receiver.VerifyResumedStoredSegment(record, std::move(storedRawBytes));
        REQUIRE(verified);
        REQUIRE(std::ranges::equal(verified.Value().GetRawBytes(), rawBytes));
        REQUIRE_FALSE(receiver.PrepareFinalization(descriptor.sessionTag));
        REQUIRE(receiver.GetTelemetry().activeOuterFecDecoderCount == 0);
        const auto commit = receiver.CommitStoredSegment(std::move(verified).Value());
        REQUIRE(commit);
        REQUIRE(commit.Value() == pbreceiver::ReceiverSegmentCommitDisposition::Committed);
        const auto repeated = receiver.CommitStoredSegment(std::move(verified).Value());
        REQUIRE(repeated);
        REQUIRE(repeated.Value() == pbreceiver::ReceiverSegmentCommitDisposition::AlreadyCommitted);
        auto resumedAgain = receiver.VerifyResumedStoredSegment(record, std::vector<std::byte>(rawBytes));
        REQUIRE(resumedAgain);
        const auto committedAgain = receiver.CommitStoredSegment(std::move(resumedAgain).Value());
        REQUIRE(committedAgain);
        REQUIRE(committedAgain.Value() == pbreceiver::ReceiverSegmentCommitDisposition::AlreadyCommitted);
        const auto prepared = receiver.PrepareFinalization(descriptor.sessionTag);
        REQUIRE(prepared);
        REQUIRE(prepared.Value() == finalManifest);
    }
}

TEST_CASE("ReceiverIngress rejects invalid completed resume bytes without committing or consuming them",
          "[pbreceiver][completion][resume-completed][negative]")
{
    constexpr std::uint32_t outerBlockBytes = 64;
    auto resourcePolicy = pbprotocol::GetDefaultReceiverResourcePolicy();
    resourcePolicy.maxRawSegmentBytes = 4096;
    const std::vector<std::byte> rawBytes(4096, std::byte{0x5A});
    const auto compressed = pbcompression::CompressSegment(rawBytes, pbcompression::CompressionSettings{});
    REQUIRE(compressed);
    REQUIRE(compressed.Value().codec == pbprotocol::CompressionCodec::Zstandard);
    const auto session = MakeSessionDescriptor(MakeSessionId(0x7A), rawBytes.size(), 1);
    const auto descriptor = MakeSegmentDescriptor(session, 0, 0, rawBytes, compressed.Value().bytes,
        pbprotocol::CompressionCodec::Zstandard, pbprotocol::OuterFecMode::DirectRepeat, outerBlockBytes);
    const auto segmentControl = MakeSegmentControlRecord(descriptor, session, resourcePolicy);
    auto receiver = MakeReceiver(resourcePolicy, outerBlockBytes);
    REQUIRE(receiver.ReceiveControlRecord(MakeSessionControlRecord(session, resourcePolicy)));
    REQUIRE(receiver.ReceiveControlRecord(segmentControl));
    REQUIRE(receiver.ReceiveControlRecord(MakeManifestControlRecord(MakeFinalManifest(session, rawBytes), session, resourcePolicy)));
    pbprotocol::ResumeCompletedSegmentRecord record{session.sessionId, 0, 0, descriptor.rawSize, descriptor.rawDigest};
    std::vector<std::byte> storedRawBytes = rawBytes;
    auto expectedError = pbprotocol::ProtocolErrorCode::ResumeRecordConflict;
    bool terminalConflict = false;
    SECTION("unknown full SessionId")
    {
        record.sessionId.bytes[0] ^= std::byte{1};
        expectedError = pbprotocol::ProtocolErrorCode::UnknownSession;
    }
    SECTION("ordinal outside Session")
    {
        record.segmentOrdinal = 1;
        expectedError = pbprotocol::ProtocolErrorCode::SegmentOrdinalOutOfRange;
    }
    SECTION("hostile offset is not used for IO")
    {
        record.rawOffset = UINT64_MAX;
    }
    SECTION("metadata raw size mismatch")
    {
        record.rawSize--;
    }
    SECTION("metadata raw digest mismatch")
    {
        record.rawDigest.bytes[0] ^= std::byte{1};
    }
    SECTION("stored bytes have a wrong length")
    {
        storedRawBytes.pop_back();
        expectedError = pbprotocol::ProtocolErrorCode::InvalidRecordSize;
    }
    SECTION("stored bytes fail the bound raw digest")
    {
        storedRawBytes[0] ^= std::byte{1};
        expectedError = pbprotocol::ProtocolErrorCode::DigestMismatch;
    }
    SECTION("metadata exceeds policy before hashing")
    {
        record.rawSize = UINT64_MAX;
        expectedError = pbprotocol::ProtocolErrorCode::ResourceLimitExceeded;
    }
    SECTION("actual bytes exceed policy before hashing")
    {
        storedRawBytes.push_back(std::byte{0});
        expectedError = pbprotocol::ProtocolErrorCode::ResourceLimitExceeded;
    }
    SECTION("terminal descriptor conflict cannot be bypassed")
    {
        auto conflictingDescriptor = descriptor;
        conflictingDescriptor.rawDigest.bytes[0] ^= std::byte{1};
        REQUIRE_FALSE(receiver.ReceiveControlRecord(MakeSegmentControlRecord(
            conflictingDescriptor, session, resourcePolicy, 9)));
        expectedError = pbprotocol::ProtocolErrorCode::DescriptorConflict;
        terminalConflict = true;
    }
    const auto beforeBytes = storedRawBytes;
    const auto failed = receiver.VerifyResumedStoredSegment(record, std::move(storedRawBytes));
    REQUIRE_FALSE(failed);
    RequireProtocolError(failed.Error(), expectedError);
    REQUIRE(storedRawBytes == beforeBytes);
    REQUIRE_FALSE(receiver.PrepareFinalization(descriptor.sessionTag));
    REQUIRE(receiver.GetTelemetry().activeOuterFecDecoderCount == 0);
    REQUIRE(receiver.GetTelemetry().reservedOuterFecDecoderBytes == 0);
    REQUIRE(receiver.GetTelemetry().totalResourcePolicyRejectedCount ==
        (expectedError == pbprotocol::ProtocolErrorCode::ResourceLimitExceeded ? 1 : 0));
    if (!terminalConflict)
    {
        const pbprotocol::ResumeCompletedSegmentRecord validRecord{session.sessionId, 0, 0, descriptor.rawSize, descriptor.rawDigest};
        auto valid = receiver.VerifyResumedStoredSegment(validRecord, std::vector<std::byte>(rawBytes));
        REQUIRE(valid);
        REQUIRE(receiver.CommitStoredSegment(std::move(valid).Value()));
        REQUIRE(receiver.PrepareFinalization(descriptor.sessionTag));
    }
}

TEST_CASE("ReceiverIngress verifies encoded and raw digests before returning raw bytes",
          "[pbreceiver][compression][digest]")
{
    constexpr std::uint32_t outerBlockBytes = 8;
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    const std::vector<std::byte> message = MakeBytes(16, 0x3A);

    SECTION("encoded bytes must match the bound descriptor")
    {
        const pbprotocol::SessionDescriptor sessionDescriptor =
            MakeSessionDescriptor(MakeSessionId(0x18), message.size(), 1);
        const pbprotocol::SegmentDescriptor segmentDescriptor =
            MakeSegmentDescriptor(
                sessionDescriptor,
                0,
                0,
                message,
                message,
                pbprotocol::CompressionCodec::Raw,
                pbprotocol::OuterFecMode::DirectRepeat,
                outerBlockBytes);
        pbreceiver::ReceiverIngress receiver = MakeReceiver(
            resourcePolicy,
            outerBlockBytes);
        REQUIRE(receiver.ReceiveControlRecord(
            MakeSessionControlRecord(sessionDescriptor, resourcePolicy)));
        const auto segmentResult = receiver.ReceiveControlRecord(
            MakeSegmentControlRecord(
                segmentDescriptor,
                sessionDescriptor,
                resourcePolicy));
        REQUIRE(segmentResult);
        REQUIRE(segmentResult.Value().controlAdmission.boundSegmentDescriptor);

        std::vector<std::byte> changedMessage = message;
        changedMessage[0] ^= std::byte{0x80};
        const auto decompressionResult = receiver.DecompressSegment(
            *segmentResult.Value().controlAdmission.boundSegmentDescriptor,
            changedMessage);
        REQUIRE_FALSE(decompressionResult);
        RequireOuterFecError(
            decompressionResult.Error(),
            pbouterfec::OuterFecErrorCode::EncodedDigestMismatch);
    }

    SECTION("decompressed bytes must match the bound raw digest")
    {
        const std::vector<std::byte> rawBytes(4096, std::byte{0x5A});
        pbcompression::CompressionSettings compressionSettings;
        compressionSettings.maxWindowLog = 12;
        const auto compressionResult = pbcompression::CompressSegment(
            rawBytes,
            compressionSettings);
        REQUIRE(compressionResult);
        REQUIRE(compressionResult.Value().codec ==
            pbprotocol::CompressionCodec::Zstandard);
        const std::vector<std::byte>& encodedBytes =
            compressionResult.Value().bytes;
        const pbprotocol::SessionDescriptor sessionDescriptor =
            MakeSessionDescriptor(MakeSessionId(0x19), rawBytes.size(), 1);
        pbprotocol::SegmentDescriptor segmentDescriptor = MakeSegmentDescriptor(
            sessionDescriptor,
            0,
            0,
            rawBytes,
            encodedBytes,
            pbprotocol::CompressionCodec::Zstandard,
            pbprotocol::OuterFecMode::DirectRepeat,
            outerBlockBytes);
        segmentDescriptor.rawDigest.bytes[0] ^= std::byte{0x40};
        pbreceiver::ReceiverIngress receiver = MakeReceiver(
            resourcePolicy,
            outerBlockBytes);
        REQUIRE(receiver.ReceiveControlRecord(
            MakeSessionControlRecord(sessionDescriptor, resourcePolicy)));
        const auto segmentResult = receiver.ReceiveControlRecord(
            MakeSegmentControlRecord(
                segmentDescriptor,
                sessionDescriptor,
                resourcePolicy));
        REQUIRE(segmentResult);
        REQUIRE(segmentResult.Value().controlAdmission.boundSegmentDescriptor);

        const auto decompressionResult = receiver.DecompressSegment(
            *segmentResult.Value().controlAdmission.boundSegmentDescriptor,
            encodedBytes);
        REQUIRE_FALSE(decompressionResult);
        RequireProtocolError(
            decompressionResult.Error(),
            pbprotocol::ProtocolErrorCode::DigestMismatch);
    }
}

TEST_CASE("ReceiverIngress Wirehair orphan replay recovers without sender metadata",
          "[pbreceiver][orphan][wirehair][integration]")
{
    constexpr std::uint32_t outerBlockBytes = 8;
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    // K=3 with a 4-byte systematic tail proves replay uses the cached
    // declaredPayloadBytes rather than sender-side metadata.
    const std::vector<std::byte> message = MakeBytes(20, 0x41);
    const pbprotocol::SessionDescriptor sessionDescriptor =
        MakeSessionDescriptor(
            MakeSessionId(0x20),
            message.size(),
            1);
    pbprotocol::WirehairV2SerializedProfile serializedProfile{};
    const std::vector<EncodedTransportBlock> blocks = EncodeWirehairBlocks(
        message,
        outerBlockBytes,
        serializedProfile);
    const pbprotocol::SegmentDescriptor segmentDescriptor =
        MakeSegmentDescriptor(
            sessionDescriptor,
            0,
            0,
            message,
            message,
            pbprotocol::CompressionCodec::Raw,
            pbprotocol::OuterFecMode::WirehairV2,
            outerBlockBytes,
            serializedProfile);

    pbreceiver::ReceiverIngress receiver = MakeReceiver(
        resourcePolicy,
        outerBlockBytes);
    for (const EncodedTransportBlock& block : blocks)
    {
        REQUIRE(receiver.ReceiveDataBlock(
            MakeReceivedBlock(segmentDescriptor.sessionTag, 0, block)));
    }
    REQUIRE(receiver.ReceiveControlRecord(
        MakeSessionControlRecord(sessionDescriptor, resourcePolicy)));
    auto segmentResult = receiver.ReceiveControlRecord(
        MakeSegmentControlRecord(
            segmentDescriptor,
            sessionDescriptor,
            resourcePolicy));
    REQUIRE(segmentResult);
    REQUIRE(segmentResult.Value().completedSegment.has_value());
    REQUIRE(segmentResult.Value().replayedOrphanBlocks.size() == blocks.size());
    for (std::size_t blockIndex = 0; blockIndex < blocks.size(); blockIndex++)
    {
        const pbprotocol::OrphanTransportBlockEntry& replayed =
            segmentResult.Value().replayedOrphanBlocks[blockIndex];
        REQUIRE(replayed.outerBlockId == blocks[blockIndex].outerBlockId);
        REQUIRE(replayed.declaredPayloadBytes == blocks[blockIndex].declaredPayloadBytes);
        REQUIRE(replayed.paddedPayload == blocks[blockIndex].paddedPayload);
    }
    REQUIRE(segmentResult.Value().completedSegment->encodedBytes == message);
    REQUIRE(receiver.GetTelemetry().activeOuterFecDecoderCount == 1);
    auto verifiedResult = receiver.VerifyRecoveredSegment(std::move(*segmentResult.Value().completedSegment));
    REQUIRE(verifiedResult);
    REQUIRE(receiver.CommitStoredSegment(std::move(verifiedResult).Value()));
    REQUIRE(receiver.GetTelemetry().activeOuterFecDecoderCount == 0);
}

TEST_CASE("ReceiverIngress enforces one receiver-wide decoder quota without fallback",
          "[pbreceiver][outer-fec][quota]")
{
    constexpr std::uint32_t outerBlockBytes = 8;
    pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    resourcePolicy.maxActiveOuterFecDecoders = 1;

    const std::vector<std::byte> firstMessage = MakeBytes(16, 0x51);
    const std::vector<std::byte> secondMessage = MakeBytes(16, 0x61);
    const pbprotocol::SessionDescriptor sessionDescriptor =
        MakeSessionDescriptor(
            MakeSessionId(0x30),
            firstMessage.size() + secondMessage.size(),
            2);
    const pbprotocol::SegmentDescriptor firstDescriptor =
        MakeSegmentDescriptor(
            sessionDescriptor,
            0,
            0,
            firstMessage,
            firstMessage,
            pbprotocol::CompressionCodec::Raw,
            pbprotocol::OuterFecMode::DirectRepeat,
            outerBlockBytes);
    const pbprotocol::SegmentDescriptor secondDescriptor =
        MakeSegmentDescriptor(
            sessionDescriptor,
            1,
            firstMessage.size(),
            secondMessage,
            secondMessage,
            pbprotocol::CompressionCodec::Raw,
            pbprotocol::OuterFecMode::DirectRepeat,
            outerBlockBytes);
    const std::vector<EncodedTransportBlock> firstBlocks =
        EncodeDirectRepeatBlocks(firstMessage, outerBlockBytes);
    const std::vector<EncodedTransportBlock> secondBlocks =
        EncodeDirectRepeatBlocks(secondMessage, outerBlockBytes);

    pbreceiver::ReceiverIngress receiver = MakeReceiver(
        resourcePolicy,
        outerBlockBytes);
    REQUIRE(receiver.ReceiveControlRecord(
        MakeSessionControlRecord(sessionDescriptor, resourcePolicy)));
    REQUIRE(receiver.ReceiveControlRecord(MakeSegmentControlRecord(
        firstDescriptor,
        sessionDescriptor,
        resourcePolicy,
        2)));

    // Segment 1 remains unknown while its first block enters only the bounded
    // orphan cache. No second decoder can exist yet.
    const auto orphanResult = receiver.ReceiveDataBlock(
        MakeReceivedBlock(secondDescriptor.sessionTag, 1, secondBlocks[0]));
    REQUIRE(orphanResult);
    REQUIRE(orphanResult.Value().disposition ==
        pbreceiver::ReceiverDataDisposition::CachedOrphan);
    REQUIRE(receiver.GetTelemetry().orphanCachedBlockCount == 1);

    const auto firstNeedMore = receiver.ReceiveDataBlock(
        MakeReceivedBlock(firstDescriptor.sessionTag, 0, firstBlocks[0]));
    REQUIRE(firstNeedMore);
    REQUIRE(firstNeedMore.Value().disposition ==
        pbreceiver::ReceiverDataDisposition::AcceptedNeedMore);
    REQUIRE(firstNeedMore.Value().outerSymbolAdmission ==
        pbreceiver::ReceiverOuterSymbolAdmission::Unique);
    const auto firstDuplicate = receiver.ReceiveDataBlock(
        MakeReceivedBlock(firstDescriptor.sessionTag, 0, firstBlocks[0]));
    REQUIRE(firstDuplicate);
    REQUIRE(firstDuplicate.Value().disposition ==
        pbreceiver::ReceiverDataDisposition::AcceptedNeedMore);
    REQUIRE(firstDuplicate.Value().outerSymbolAdmission ==
        pbreceiver::ReceiverOuterSymbolAdmission::IdenticalDuplicate);
    REQUIRE(receiver.GetTelemetry().activeOuterFecDecoderCount == 1);

    // Binding the descriptor attempts lazy creation because an orphan exists.
    // The receiver-wide quota fails before Drain, so the orphan must survive.
    const std::vector<std::byte> secondDescriptorRecord =
        MakeSegmentControlRecord(
            secondDescriptor,
            sessionDescriptor,
            resourcePolicy,
            3);
    const auto quotaResult = receiver.ReceiveControlRecord(
        secondDescriptorRecord);
    REQUIRE(quotaResult);
    REQUIRE_FALSE(quotaResult.Value().completedSegment);
    REQUIRE(quotaResult.Value().replayedOrphanBlocks.empty());
    pbreceiver::ReceiverResourceTelemetrySnapshot telemetry =
        receiver.GetTelemetry();
    REQUIRE(telemetry.activeOuterFecDecoderCount == 1);
    REQUIRE(telemetry.orphanCachedBlockCount == 1);
    REQUIRE(telemetry.orphanCachedBytes == outerBlockBytes);
    REQUIRE(telemetry.outerFecQuotaExceededCount == 1);
    REQUIRE(telemetry.deferredResourceBusyCount == 1);
    REQUIRE(telemetry.totalResourcePolicyRejectedCount == 0);

    const auto deferredDataResult = receiver.ReceiveDataBlock(
        MakeReceivedBlock(secondDescriptor.sessionTag, 1, secondBlocks[0]));
    REQUIRE(deferredDataResult);
    REQUIRE(deferredDataResult.Value().disposition ==
        pbreceiver::ReceiverDataDisposition::DeferredResourceBusy);
    REQUIRE(deferredDataResult.Value().outerSymbolAdmission ==
        pbreceiver::ReceiverOuterSymbolAdmission::DeferredResourceBusy);

    auto firstReady = receiver.ReceiveDataBlock(
        MakeReceivedBlock(firstDescriptor.sessionTag, 0, firstBlocks[1]));
    REQUIRE(firstReady);
    REQUIRE(firstReady.Value().completedSegment.has_value());
    REQUIRE(firstReady.Value().completedSegment->encodedBytes == firstMessage);
    const auto pendingQuotaResult = receiver.ReceiveControlRecord(secondDescriptorRecord);
    REQUIRE(pendingQuotaResult);
    REQUIRE_FALSE(pendingQuotaResult.Value().completedSegment);
    REQUIRE(receiver.GetTelemetry().outerFecQuotaExceededCount == 3);
    REQUIRE(receiver.GetTelemetry().deferredResourceBusyCount == 3);
    REQUIRE(receiver.GetTelemetry().orphanCachedBlockCount == 1);
    auto firstVerified = receiver.VerifyRecoveredSegment(std::move(*firstReady.Value().completedSegment));
    REQUIRE(firstVerified);
    REQUIRE(receiver.CommitStoredSegment(std::move(firstVerified).Value()));
    REQUIRE(receiver.GetTelemetry().activeOuterFecDecoderCount == 0);

    // A later Carousel Data observation retries the already-bound Segment,
    // drains both cached equations after capacity is released, and exposes
    // exactly the accepted blocks for durable application checkpointing.
    auto secondReady = receiver.ReceiveDataBlock(
        MakeReceivedBlock(secondDescriptor.sessionTag, 1, secondBlocks[1]));
    REQUIRE(secondReady);
    REQUIRE(secondReady.Value().completedSegment.has_value());
    REQUIRE(secondReady.Value().replayedOrphanBlocks.size() == secondBlocks.size());
    for (std::size_t blockIndex = 0; blockIndex < secondBlocks.size(); blockIndex++)
    {
        const pbprotocol::OrphanTransportBlockEntry& replayed =
            secondReady.Value().replayedOrphanBlocks[blockIndex];
        REQUIRE(replayed.outerBlockId == secondBlocks[blockIndex].outerBlockId);
        REQUIRE(replayed.declaredPayloadBytes == secondBlocks[blockIndex].declaredPayloadBytes);
        REQUIRE(replayed.paddedPayload == secondBlocks[blockIndex].paddedPayload);
    }
    REQUIRE(secondReady.Value().completedSegment->encodedBytes == secondMessage);
    REQUIRE(receiver.GetTelemetry().orphanCachedBlockCount == 0);
    REQUIRE(receiver.GetTelemetry().activeOuterFecDecoderCount == 1);
    auto secondVerified = receiver.VerifyRecoveredSegment(std::move(*secondReady.Value().completedSegment));
    REQUIRE(secondVerified);
    REQUIRE(receiver.CommitStoredSegment(std::move(secondVerified).Value()));
    REQUIRE(receiver.GetTelemetry().activeOuterFecDecoderCount == 0);
}

TEST_CASE("ReceiverIngress detects a cached conflict before quota retry drain",
          "[pbreceiver][orphan][outer-fec][conflict]")
{
    constexpr std::uint32_t outerBlockBytes = 8;
    pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    resourcePolicy.maxActiveOuterFecDecoders = 1;
    const std::vector<std::byte> firstMessage = MakeBytes(16, 0x64);
    const std::vector<std::byte> secondMessage = MakeBytes(16, 0x65);
    const pbprotocol::SessionDescriptor sessionDescriptor =
        MakeSessionDescriptor(
            MakeSessionId(0x31),
            firstMessage.size() + secondMessage.size(),
            2);
    const pbprotocol::SegmentDescriptor firstDescriptor =
        MakeSegmentDescriptor(
            sessionDescriptor,
            0,
            0,
            firstMessage,
            firstMessage,
            pbprotocol::CompressionCodec::Raw,
            pbprotocol::OuterFecMode::DirectRepeat,
            outerBlockBytes);
    const pbprotocol::SegmentDescriptor secondDescriptor =
        MakeSegmentDescriptor(
            sessionDescriptor,
            1,
            firstMessage.size(),
            secondMessage,
            secondMessage,
            pbprotocol::CompressionCodec::Raw,
            pbprotocol::OuterFecMode::DirectRepeat,
            outerBlockBytes);
    const std::vector<EncodedTransportBlock> firstBlocks =
        EncodeDirectRepeatBlocks(firstMessage, outerBlockBytes);
    const std::vector<EncodedTransportBlock> secondBlocks =
        EncodeDirectRepeatBlocks(secondMessage, outerBlockBytes);

    pbreceiver::ReceiverIngress receiver = MakeReceiver(
        resourcePolicy,
        outerBlockBytes);
    REQUIRE(receiver.ReceiveControlRecord(
        MakeSessionControlRecord(sessionDescriptor, resourcePolicy)));
    REQUIRE(receiver.ReceiveControlRecord(MakeSegmentControlRecord(
        firstDescriptor,
        sessionDescriptor,
        resourcePolicy,
        2)));
    REQUIRE(receiver.ReceiveDataBlock(MakeReceivedBlock(
        secondDescriptor.sessionTag,
        1,
        secondBlocks[0])));
    REQUIRE(receiver.ReceiveDataBlock(MakeReceivedBlock(
        firstDescriptor.sessionTag,
        0,
        firstBlocks[0])));
    const auto quotaResult = receiver.ReceiveControlRecord(
        MakeSegmentControlRecord(
            secondDescriptor,
            sessionDescriptor,
            resourcePolicy,
            3));
    REQUIRE(quotaResult);
    REQUIRE_FALSE(quotaResult.Value().completedSegment);

    EncodedTransportBlock conflictingBlock = secondBlocks[0];
    conflictingBlock.paddedPayload[0] ^= std::byte{0x80};
    const auto conflictResult = receiver.ReceiveDataBlock(
        MakeReceivedBlock(
            secondDescriptor.sessionTag,
            1,
            conflictingBlock));
    REQUIRE_FALSE(conflictResult);
    RequireProtocolError(
        conflictResult.Error(),
        pbprotocol::ProtocolErrorCode::OrphanPayloadConflict);
    const pbreceiver::ReceiverResourceTelemetrySnapshot telemetry =
        receiver.GetTelemetry();
    REQUIRE(telemetry.orphanConflictRejectionCount == 1);
    REQUIRE(telemetry.orphanCachedBlockCount == 0);
    REQUIRE(telemetry.activeOuterFecDecoderCount == 0);
    REQUIRE(telemetry.reservedOuterFecDecoderBytes == 0);

    const auto latchedResult = receiver.ReceiveDataBlock(MakeReceivedBlock(
        firstDescriptor.sessionTag,
        0,
        firstBlocks[1]));
    REQUIRE_FALSE(latchedResult);
    RequireProtocolError(
        latchedResult.Error(),
        pbprotocol::ProtocolErrorCode::OrphanPayloadConflict);
}

TEST_CASE("ReceiverIngress rejects malformed unknown blocks before caching",
          "[pbreceiver][orphan][validation]")
{
    constexpr std::uint32_t outerBlockBytes = 8;
    pbreceiver::ReceiverIngress receiver = MakeReceiver(
        pbprotocol::GetDefaultReceiverResourcePolicy(),
        outerBlockBytes);
    std::vector<std::byte> paddedPayload = MakeBytes(4, 0x71);
    paddedPayload.resize(outerBlockBytes, std::byte{0});
    paddedPayload[4] = std::byte{1};
    const pbreceiver::ReceivedTransportBlock block{
        pbprotocol::SessionTag{77},
        0,
        0,
        4,
        paddedPayload};
    const auto result = receiver.ReceiveDataBlock(block);
    REQUIRE_FALSE(result);
    RequireProtocolError(
        result.Error(),
        pbprotocol::ProtocolErrorCode::NonCanonicalPadding);
    REQUIRE(receiver.GetTelemetry().orphanCachedBlockCount == 0);
    REQUIRE(receiver.GetTelemetry().activeOuterFecDecoderCount == 0);
}

TEST_CASE("ReceiverIngress descriptor profile mismatch clears orphan state terminally",
          "[pbreceiver][orphan][profile][terminal]")
{
    constexpr std::uint32_t receiverOuterBlockBytes = 8;
    constexpr std::uint32_t descriptorOuterBlockBytes = 4;
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    const std::vector<std::byte> message = MakeBytes(8, 0x72);
    const pbprotocol::SessionDescriptor sessionDescriptor =
        MakeSessionDescriptor(MakeSessionId(0x35), message.size(), 1);
    const pbprotocol::SegmentDescriptor segmentDescriptor =
        MakeSegmentDescriptor(
            sessionDescriptor,
            0,
            0,
            message,
            message,
            pbprotocol::CompressionCodec::Raw,
            pbprotocol::OuterFecMode::DirectRepeat,
            descriptorOuterBlockBytes);
    const EncodedTransportBlock orphanBlock{
        0,
        receiverOuterBlockBytes,
        MakeBytes(receiverOuterBlockBytes, 0x73)};

    pbreceiver::ReceiverIngress receiver = MakeReceiver(
        resourcePolicy,
        receiverOuterBlockBytes);
    REQUIRE(receiver.ReceiveDataBlock(MakeReceivedBlock(
        segmentDescriptor.sessionTag,
        0,
        orphanBlock)));
    REQUIRE(receiver.GetTelemetry().orphanCachedBlockCount == 1);
    REQUIRE(receiver.ReceiveControlRecord(
        MakeSessionControlRecord(sessionDescriptor, resourcePolicy)));

    const auto segmentResult = receiver.ReceiveControlRecord(
        MakeSegmentControlRecord(
            segmentDescriptor,
            sessionDescriptor,
            resourcePolicy));
    REQUIRE_FALSE(segmentResult);
    RequireOuterFecError(
        segmentResult.Error(),
        pbouterfec::OuterFecErrorCode::OuterBlockBytesMismatch);
    REQUIRE(receiver.GetTelemetry().orphanCachedBlockCount == 0);
    REQUIRE(receiver.GetTelemetry().activeOuterFecDecoderCount == 0);

    const auto latchedResult = receiver.ReceiveDataBlock(MakeReceivedBlock(
        segmentDescriptor.sessionTag,
        0,
        orphanBlock));
    REQUIRE_FALSE(latchedResult);
    RequireOuterFecError(
        latchedResult.Error(),
        pbouterfec::OuterFecErrorCode::OuterBlockBytesMismatch);
}

TEST_CASE("ReceiverIngress session removal releases decoder and orphan resources",
          "[pbreceiver][cleanup][raii]")
{
    constexpr std::uint32_t outerBlockBytes = 8;
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    const std::vector<std::byte> firstMessage = MakeBytes(16, 0x74);
    const std::vector<std::byte> secondMessage = MakeBytes(16, 0x75);
    const pbprotocol::SessionDescriptor sessionDescriptor =
        MakeSessionDescriptor(
            MakeSessionId(0x36),
            firstMessage.size() + secondMessage.size(),
            2);
    const pbprotocol::SegmentDescriptor firstDescriptor =
        MakeSegmentDescriptor(
            sessionDescriptor,
            0,
            0,
            firstMessage,
            firstMessage,
            pbprotocol::CompressionCodec::Raw,
            pbprotocol::OuterFecMode::DirectRepeat,
            outerBlockBytes);
    const std::vector<EncodedTransportBlock> firstBlocks =
        EncodeDirectRepeatBlocks(firstMessage, outerBlockBytes);
    const std::vector<EncodedTransportBlock> secondBlocks =
        EncodeDirectRepeatBlocks(secondMessage, outerBlockBytes);

    pbreceiver::ReceiverIngress receiver = MakeReceiver(
        resourcePolicy,
        outerBlockBytes);
    REQUIRE(receiver.ReceiveControlRecord(
        MakeSessionControlRecord(sessionDescriptor, resourcePolicy)));
    REQUIRE(receiver.ReceiveControlRecord(MakeSegmentControlRecord(
        firstDescriptor,
        sessionDescriptor,
        resourcePolicy)));
    REQUIRE(receiver.ReceiveDataBlock(MakeReceivedBlock(
        firstDescriptor.sessionTag,
        0,
        firstBlocks[0])));
    REQUIRE(receiver.ReceiveDataBlock(MakeReceivedBlock(
        firstDescriptor.sessionTag,
        1,
        secondBlocks[0])));
    REQUIRE(receiver.GetTelemetry().activeOuterFecDecoderCount == 1);
    REQUIRE(receiver.GetTelemetry().orphanCachedBlockCount == 1);

    const auto removeResult = receiver.RemoveSession(
        sessionDescriptor.sessionId);
    REQUIRE(removeResult);
    REQUIRE(removeResult.Value());
    const pbreceiver::ReceiverResourceTelemetrySnapshot telemetry =
        receiver.GetTelemetry();
    REQUIRE(telemetry.activeSessionCount == 0);
    REQUIRE(telemetry.activeOuterFecDecoderCount == 0);
    REQUIRE(telemetry.reservedOuterFecDecoderBytes == 0);
    REQUIRE(telemetry.orphanCachedBlockCount == 0);
    REQUIRE(telemetry.orphanCachedBytes == 0);
}

TEST_CASE("ReceiverIngress releases Data-plane resources after terminal Control conflict",
          "[pbreceiver][control][conflict][cleanup]")
{
    constexpr std::uint32_t outerBlockBytes = 8;
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    const std::vector<std::byte> firstMessage = MakeBytes(16, 0x79);
    const std::vector<std::byte> secondMessage = MakeBytes(16, 0x7A);
    const pbprotocol::SessionDescriptor sessionDescriptor =
        MakeSessionDescriptor(
            MakeSessionId(0x37),
            firstMessage.size() + secondMessage.size(),
            2);
    const pbprotocol::SegmentDescriptor firstDescriptor =
        MakeSegmentDescriptor(
            sessionDescriptor,
            0,
            0,
            firstMessage,
            firstMessage,
            pbprotocol::CompressionCodec::Raw,
            pbprotocol::OuterFecMode::DirectRepeat,
            outerBlockBytes);
    const std::vector<EncodedTransportBlock> firstBlocks =
        EncodeDirectRepeatBlocks(firstMessage, outerBlockBytes);
    const std::vector<EncodedTransportBlock> secondBlocks =
        EncodeDirectRepeatBlocks(secondMessage, outerBlockBytes);

    pbreceiver::ReceiverIngress receiver = MakeReceiver(
        resourcePolicy,
        outerBlockBytes);
    REQUIRE(receiver.ReceiveControlRecord(
        MakeSessionControlRecord(sessionDescriptor, resourcePolicy)));
    REQUIRE(receiver.ReceiveControlRecord(MakeSegmentControlRecord(
        firstDescriptor,
        sessionDescriptor,
        resourcePolicy)));
    REQUIRE(receiver.ReceiveDataBlock(MakeReceivedBlock(
        firstDescriptor.sessionTag,
        0,
        firstBlocks[0])));
    REQUIRE(receiver.ReceiveDataBlock(MakeReceivedBlock(
        firstDescriptor.sessionTag,
        1,
        secondBlocks[0])));
    REQUIRE(receiver.GetTelemetry().activeOuterFecDecoderCount == 1);
    REQUIRE(receiver.GetTelemetry().orphanCachedBlockCount == 1);

    pbprotocol::SegmentDescriptor conflictingDescriptor = firstDescriptor;
    conflictingDescriptor.rawOffset = 1;
    const auto conflictResult = receiver.ReceiveControlRecord(
        MakeSegmentControlRecord(
            conflictingDescriptor,
            sessionDescriptor,
            resourcePolicy,
            3));
    REQUIRE_FALSE(conflictResult);
    RequireProtocolError(
        conflictResult.Error(),
        pbprotocol::ProtocolErrorCode::DescriptorConflict);

    const pbreceiver::ReceiverResourceTelemetrySnapshot telemetry =
        receiver.GetTelemetry();
    REQUIRE(telemetry.activeOuterFecDecoderCount == 0);
    REQUIRE(telemetry.reservedOuterFecDecoderBytes == 0);
    REQUIRE(telemetry.orphanCachedBlockCount == 0);
    REQUIRE(telemetry.orphanCachedBytes == 0);

    const auto terminalResult = receiver.ReceiveDataBlock(MakeReceivedBlock(
        firstDescriptor.sessionTag,
        0,
        firstBlocks[1]));
    REQUIRE_FALSE(terminalResult);
    RequireProtocolError(
        terminalResult.Error(),
        pbprotocol::ProtocolErrorCode::DescriptorConflict);
}

TEST_CASE("ReceiverIngress aggregates resume output and zstd resource telemetry",
          "[pbreceiver][telemetry][resource]")
{
    constexpr std::uint32_t outerBlockBytes = 64;
    pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    resourcePolicy.maxAcceptedFileBytes = 8192;
    resourcePolicy.maxEncodedSegmentBytes = 8192;
    resourcePolicy.maxOutputPreallocationBytesWithoutPrompt = 4096;
    resourcePolicy.maxResumeBytes =
        pbprotocol::kResumeRecordEnvelopeBytes + 4;
    resourcePolicy.maxZstdWindowBytes = 1024;
    pbreceiver::ReceiverIngress receiver = MakeReceiver(
        resourcePolicy,
        outerBlockBytes);

    const auto automaticResult = receiver.EvaluateOutputReservation(4096);
    REQUIRE(automaticResult);
    REQUIRE(automaticResult.Value() ==
        pbprotocol::OutputReservationDecision::AutoAccept);
    const auto confirmationResult = receiver.EvaluateOutputReservation(4097);
    REQUIRE(confirmationResult);
    REQUIRE(confirmationResult.Value() ==
        pbprotocol::OutputReservationDecision::RequiresUserConfirmation);
    const auto deniedResult = receiver.EvaluateOutputReservation(8193);
    REQUIRE_FALSE(deniedResult);
    RequireProtocolError(
        deniedResult.Error(),
        pbprotocol::ProtocolErrorCode::OutputReservationDenied);

    const std::vector<std::byte> resumePayload = MakeBytes(5, 0x81);
    std::vector<std::byte> resumeRecord(
        pbprotocol::kResumeRecordEnvelopeBytes + resumePayload.size());
    REQUIRE(pbprotocol::SerializeResumeRecord(resumePayload, resumeRecord));
    const auto resumeResult = receiver.ParseResumeState(resumeRecord);
    REQUIRE_FALSE(resumeResult);
    RequireProtocolError(
        resumeResult.Error(),
        pbprotocol::ProtocolErrorCode::ResourceLimitExceeded);

    const std::vector<std::byte> rawBytes(4096, std::byte{0x5A});
    pbcompression::CompressionSettings compressionSettings;
    compressionSettings.maxWindowLog = 12;
    const auto compressionResult = pbcompression::CompressSegment(
        rawBytes,
        compressionSettings);
    REQUIRE(compressionResult);
    REQUIRE(compressionResult.Value().codec ==
        pbprotocol::CompressionCodec::Zstandard);
    const std::vector<std::byte>& encodedBytes =
        compressionResult.Value().bytes;

    const pbprotocol::SessionDescriptor sessionDescriptor =
        MakeSessionDescriptor(
            MakeSessionId(0x40),
            rawBytes.size(),
            1);
    const pbprotocol::SegmentDescriptor segmentDescriptor =
        MakeSegmentDescriptor(
            sessionDescriptor,
            0,
            0,
            rawBytes,
            encodedBytes,
            pbprotocol::CompressionCodec::Zstandard,
            pbprotocol::OuterFecMode::DirectRepeat,
            outerBlockBytes);
    REQUIRE(receiver.ReceiveControlRecord(
        MakeSessionControlRecord(sessionDescriptor, resourcePolicy)));
    const auto segmentResult = receiver.ReceiveControlRecord(
        MakeSegmentControlRecord(
            segmentDescriptor,
            sessionDescriptor,
            resourcePolicy));
    REQUIRE(segmentResult);
    REQUIRE(segmentResult.Value().controlAdmission.boundSegmentDescriptor);
    const auto decompressionResult = receiver.DecompressSegment(
        *segmentResult.Value().controlAdmission.boundSegmentDescriptor,
        encodedBytes);
    REQUIRE_FALSE(decompressionResult);
    REQUIRE(std::holds_alternative<pbcompression::CompressionError>(
        decompressionResult.Error()));
    REQUIRE(std::get<pbcompression::CompressionError>(
        decompressionResult.Error()).code ==
            pbcompression::CompressionErrorCode::WindowLimitExceeded);

    const std::vector<std::byte> oversizedEncodedInput(
        resourcePolicy.maxEncodedSegmentBytes + 1ULL,
        std::byte{0});
    const auto inputLimitResult = receiver.DecompressSegment(
        *segmentResult.Value().controlAdmission.boundSegmentDescriptor,
        oversizedEncodedInput);
    REQUIRE_FALSE(inputLimitResult);
    REQUIRE(std::holds_alternative<pbcompression::CompressionError>(
        inputLimitResult.Error()));
    REQUIRE(std::get<pbcompression::CompressionError>(
        inputLimitResult.Error()).code ==
            pbcompression::CompressionErrorCode::InputLimitExceeded);

    const pbreceiver::ReceiverResourceTelemetrySnapshot telemetry =
        receiver.GetTelemetry();
    REQUIRE(telemetry.outputReservationAutoAcceptedCount == 2);
    REQUIRE(telemetry.outputReservationRequiresConfirmationCount == 1);
    REQUIRE(telemetry.outputReservationDeniedCount == 1);
    REQUIRE(telemetry.resumeQuotaRejectedCount == 1);
    REQUIRE(telemetry.decompressionResourceRejectedCount == 2);
    REQUIRE(telemetry.decompressionInputQuotaRejectedCount == 1);
    REQUIRE(telemetry.decompressionOutputQuotaRejectedCount == 0);
    REQUIRE(telemetry.decompressionWindowQuotaRejectedCount == 1);
    REQUIRE(telemetry.decompressionAllocationFailureCount == 0);
    REQUIRE(telemetry.totalResourcePolicyRejectedCount == 4);
}

TEST_CASE("ReceiverIngress counts fragmented Control quota once and resets occupancy",
          "[pbreceiver][control][fragment][telemetry]")
{
    constexpr std::uint32_t outerBlockBytes = 8;
    pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    resourcePolicy.maxConcurrentControlReassemblies = 1;
    pbreceiver::ReceiverIngress receiver = MakeReceiver(
        resourcePolicy,
        outerBlockBytes);

    const pbprotocol::SessionDescriptor firstSession =
        MakeSessionDescriptor(MakeSessionId(0x51), 16, 1);
    const pbprotocol::SessionDescriptor secondSession =
        MakeSessionDescriptor(MakeSessionId(0x52), 16, 1);
    const std::vector<std::byte> firstRecord = MakeSessionControlRecord(
        firstSession,
        resourcePolicy);
    const std::vector<std::byte> secondRecord = MakeSessionControlRecord(
        secondSession,
        resourcePolicy);
    const std::vector<std::byte> firstFragment = MakeControlFragment(
        firstRecord,
        100,
        0);
    const std::vector<std::byte> secondFragment = MakeControlFragment(
        secondRecord,
        101,
        0);

    const auto storedResult = receiver.ReceiveControlFragment(
        firstFragment,
        1);
    REQUIRE(storedResult);
    REQUIRE(storedResult.Value().disposition ==
        pbprotocol::ControlFragmentReceiveDisposition::Stored);
    const auto quotaResult = receiver.ReceiveControlFragment(
        secondFragment,
        2);
    REQUIRE_FALSE(quotaResult);
    RequireProtocolError(
        quotaResult.Error(),
        pbprotocol::ProtocolErrorCode::ControlReassemblyQuotaExceeded);

    pbreceiver::ReceiverResourceTelemetrySnapshot telemetry =
        receiver.GetTelemetry();
    REQUIRE(telemetry.controlRejectedByResourcePolicyCount == 1);
    REQUIRE(telemetry.totalResourcePolicyRejectedCount == 1);
    REQUIRE(telemetry.activeControlReassemblyCount == 1);
    REQUIRE(telemetry.controlReassemblyBytesInUse > 0);

    REQUIRE(receiver.ResetCaptureEpoch(outerBlockBytes));
    telemetry = receiver.GetTelemetry();
    REQUIRE(telemetry.controlRejectedByResourcePolicyCount == 1);
    REQUIRE(telemetry.totalResourcePolicyRejectedCount == 1);
    REQUIRE(telemetry.activeControlReassemblyCount == 0);
    REQUIRE(telemetry.controlReassemblyBytesInUse == 0);
}

TEST_CASE("ReceiverIngress capture reset clears state and preserves counters",
          "[pbreceiver][reset][telemetry]")
{
    constexpr std::uint32_t outerBlockBytes = 8;
    pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    resourcePolicy.maxAcceptedFileBytes = 100;
    resourcePolicy.maxOutputPreallocationBytesWithoutPrompt = 50;
    pbreceiver::ReceiverIngress receiver = MakeReceiver(
        resourcePolicy,
        outerBlockBytes);

    const EncodedTransportBlock orphanBlock{
        0,
        8,
        MakeBytes(8, 0x91)};
    REQUIRE(receiver.ReceiveDataBlock(
        MakeReceivedBlock(pbprotocol::SessionTag{91}, 0, orphanBlock)));
    REQUIRE_FALSE(receiver.EvaluateOutputReservation(101));
    REQUIRE(receiver.GetTelemetry().orphanCachedBlockCount == 1);
    REQUIRE(receiver.GetTelemetry().totalResourcePolicyRejectedCount == 1);

    const auto resetResult = receiver.ResetCaptureEpoch(outerBlockBytes);
    REQUIRE(resetResult);
    REQUIRE(resetResult.Value());
    const pbreceiver::ReceiverResourceTelemetrySnapshot telemetry =
        receiver.GetTelemetry();
    REQUIRE(telemetry.orphanCachedBlockCount == 0);
    REQUIRE(telemetry.orphanCachedBytes == 0);
    REQUIRE(telemetry.orphanAdmittedBlockCount == 1);
    REQUIRE(telemetry.outputReservationDeniedCount == 1);
    REQUIRE(telemetry.totalResourcePolicyRejectedCount == 1);
    REQUIRE(telemetry.activeSessionCount == 0);
    REQUIRE(telemetry.activeOuterFecDecoderCount == 0);
}
