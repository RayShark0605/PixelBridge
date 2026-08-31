#include "receiver_evidence.h"

#include "pbouterfec/outer_fec_result.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/descriptor_codec.h"
#include "pbprotocol/transport_block_codec.h"
#include "pbreceiver/receiver_ingress.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace pbremotevisualreceiverevidence
{
namespace
{

constexpr std::array<std::byte, pbprotocol::kSessionIdBytes> kEvidenceSessionIdBytes{
    std::byte{0x40}, std::byte{0x41}, std::byte{0x42}, std::byte{0x43},
    std::byte{0x44}, std::byte{0x45}, std::byte{0x46}, std::byte{0x47},
    std::byte{0x48}, std::byte{0x49}, std::byte{0x4A}, std::byte{0x4B},
    std::byte{0x4C}, std::byte{0x4D}, std::byte{0x4E}, std::byte{0x4F}};

struct ReceiverFixture
{
    pbprotocol::ReceiverResourcePolicy resourcePolicy;
    pbprotocol::SessionDescriptor sessionDescriptor;
    std::vector<pbprotocol::SegmentDescriptor> segmentDescriptors;
    pbprotocol::FinalManifest finalManifest;
    std::vector<std::vector<std::byte>> controlRecords;
    std::vector<std::byte> wholeFileBytes;
};

std::string DigestToHex(const std::array<std::byte, pbprotocol::kDigestBytes>& digest)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string output;
    output.resize(digest.size() * 2);
    for (std::size_t index = 0; index < digest.size(); index++)
    {
        const unsigned value = std::to_integer<unsigned>(digest[index]);
        output[index * 2] = kHex[value >> 4];
        output[index * 2 + 1] = kHex[value & 0x0FU];
    }
    return output;
}

std::vector<std::byte> WrapControlPayload(const pbprotocol::ControlRecordType recordType,
    const std::uint64_t controlSequence, const pbprotocol::SessionTag sessionTag,
    const std::span<const std::byte> payload)
{
    const pbprotocol::ControlRecordView record{
        pbprotocol::kControlVersion, recordType, controlSequence, sessionTag, payload};
    const auto sizeResult = pbprotocol::GetSerializedSize(record);
    if (!sizeResult)
    {
        throw std::runtime_error("control record size calculation failed");
    }
    std::vector<std::byte> bytes(sizeResult.Value());
    if (!pbprotocol::SerializeControlRecord(record, bytes))
    {
        throw std::runtime_error("control record serialization failed");
    }
    return bytes;
}

std::vector<std::byte> MakeSessionControlRecord(const pbprotocol::SessionDescriptor& descriptor,
    const pbprotocol::ReceiverResourcePolicy& resourcePolicy, const std::uint64_t controlSequence)
{
    std::array<std::byte, pbprotocol::kSessionDescriptorPayloadBytes> payload{};
    if (!pbprotocol::SerializeSessionDescriptor(descriptor, resourcePolicy, payload))
    {
        throw std::runtime_error("SessionDescriptor serialization failed");
    }
    return WrapControlPayload(pbprotocol::ControlRecordType::SessionDescriptor, controlSequence,
        pbprotocol::DeriveSessionTag(descriptor.sessionId), payload);
}

std::vector<std::byte> MakeSegmentControlRecord(const pbprotocol::SegmentDescriptor& descriptor,
    const pbprotocol::SessionDescriptor& sessionDescriptor,
    const pbprotocol::ReceiverResourcePolicy& resourcePolicy, const std::uint64_t controlSequence)
{
    const auto sizeResult = pbprotocol::GetSerializedSize(descriptor);
    if (!sizeResult)
    {
        throw std::runtime_error("SegmentDescriptor size calculation failed");
    }
    std::vector<std::byte> payload(sizeResult.Value());
    if (!pbprotocol::SerializeSegmentDescriptor(descriptor, sessionDescriptor, resourcePolicy, payload))
    {
        throw std::runtime_error("SegmentDescriptor serialization failed");
    }
    return WrapControlPayload(pbprotocol::ControlRecordType::SegmentDescriptor, controlSequence,
        descriptor.sessionTag, payload);
}

std::vector<std::byte> MakeFinalManifestControlRecord(const pbprotocol::FinalManifest& manifest,
    const pbprotocol::SessionDescriptor& sessionDescriptor,
    const pbprotocol::ReceiverResourcePolicy& resourcePolicy, const std::uint64_t controlSequence)
{
    std::array<std::byte, pbprotocol::kFinalManifestPayloadBytes> payload{};
    if (!pbprotocol::SerializeFinalManifest(manifest, sessionDescriptor, resourcePolicy, payload))
    {
        throw std::runtime_error("FinalManifest serialization failed");
    }
    return WrapControlPayload(pbprotocol::ControlRecordType::FinalManifest, controlSequence,
        pbprotocol::DeriveSessionTag(manifest.sessionId), payload);
}

std::vector<std::byte> BuildExpectedSegment(const std::uint64_t segmentOrdinal)
{
    std::vector<std::byte> segmentBytes;
    segmentBytes.reserve(static_cast<std::size_t>(kReceiverEvidenceSegmentBytes));
    for (std::uint32_t slot = 0; slot < 4; slot++)
    {
        pbdesktoplevels::AcceptedTransportBlock expected;
        std::string error;
        if (!MakeReceiverEvidenceExpectedTransportBlock(segmentOrdinal, slot, expected, error))
        {
            throw std::runtime_error(error);
        }
        const auto parsedResult = pbprotocol::ParseTransportBlock(
            std::span(expected.bytes).first(expected.byteCount));
        if (!parsedResult)
        {
            throw std::runtime_error("diagnostic sender Transport parse failed");
        }
        const pbprotocol::TransportBlockView& parsed = parsedResult.Value();
        if (parsed.header.sessionTag != GetReceiverEvidenceSessionTag() ||
            parsed.header.segmentOrdinal != segmentOrdinal || parsed.header.outerBlockId != slot ||
            parsed.header.payloadBytes != kReceiverEvidenceOuterBlockBytes ||
            parsed.payload.size() != kReceiverEvidenceOuterBlockBytes)
        {
            throw std::runtime_error("diagnostic sender Transport identity changed unexpectedly");
        }
        segmentBytes.insert(segmentBytes.end(), parsed.payload.begin(), parsed.payload.end());
    }
    if (segmentBytes.size() != kReceiverEvidenceSegmentBytes)
    {
        throw std::runtime_error("diagnostic sender Segment size changed unexpectedly");
    }
    return segmentBytes;
}

ReceiverFixture BuildFixture(const std::uint32_t segmentCount)
{
    if (segmentCount == 0 || segmentCount > kMaximumReceiverEvidenceSegments)
    {
        throw std::invalid_argument("receiver evidence segment count is outside the bounded policy");
    }

    ReceiverFixture fixture;
    fixture.resourcePolicy = pbprotocol::GetDefaultReceiverResourcePolicy();
    fixture.wholeFileBytes.reserve(static_cast<std::size_t>(segmentCount) *
        static_cast<std::size_t>(kReceiverEvidenceSegmentBytes));
    std::vector<std::vector<std::byte>> segments;
    segments.reserve(segmentCount);
    for (std::uint32_t segmentOrdinal = 0; segmentOrdinal < segmentCount; segmentOrdinal++)
    {
        segments.push_back(BuildExpectedSegment(segmentOrdinal));
        fixture.wholeFileBytes.insert(fixture.wholeFileBytes.end(), segments.back().begin(), segments.back().end());
    }

    fixture.sessionDescriptor = {pbprotocol::GetProtocolVersion(), GetReceiverEvidenceSessionId(),
        fixture.wholeFileBytes.size(), segmentCount, pbprotocol::DigestAlgorithm::Blake3_256};
    const pbprotocol::SessionTag sessionTag = GetReceiverEvidenceSessionTag();
    fixture.segmentDescriptors.reserve(segmentCount);
    for (std::uint32_t segmentOrdinal = 0; segmentOrdinal < segmentCount; segmentOrdinal++)
    {
        const std::vector<std::byte>& segmentBytes = segments[segmentOrdinal];
        pbprotocol::SegmentDescriptor descriptor;
        descriptor.sessionTag = sessionTag;
        descriptor.segmentOrdinal = segmentOrdinal;
        descriptor.rawOffset = static_cast<std::uint64_t>(segmentOrdinal) * kReceiverEvidenceSegmentBytes;
        descriptor.rawSize = segmentBytes.size();
        descriptor.encodedSize = segmentBytes.size();
        descriptor.compressionCodec = pbprotocol::CompressionCodec::Raw;
        descriptor.outerFecMode = pbprotocol::OuterFecMode::DirectRepeat;
        descriptor.outerBlockBytes = kReceiverEvidenceOuterBlockBytes;
        descriptor.rawDigest = pbprotocol::RawDigest{pbprotocol::ComputeBlake3Digest(segmentBytes)};
        descriptor.encodedDigest = pbprotocol::EncodedDigest{pbprotocol::ComputeBlake3Digest(segmentBytes)};
        fixture.segmentDescriptors.push_back(descriptor);
    }
    fixture.finalManifest = {GetReceiverEvidenceSessionId(), fixture.wholeFileBytes.size(), segmentCount,
        pbprotocol::WholeFileDigest{pbprotocol::ComputeBlake3Digest(fixture.wholeFileBytes)},
        pbprotocol::DigestAlgorithm::Blake3_256};

    fixture.controlRecords.reserve(static_cast<std::size_t>(segmentCount) + 2);
    fixture.controlRecords.push_back(MakeSessionControlRecord(fixture.sessionDescriptor,
        fixture.resourcePolicy, 1));
    for (std::uint32_t segmentOrdinal = 0; segmentOrdinal < segmentCount; segmentOrdinal++)
    {
        fixture.controlRecords.push_back(MakeSegmentControlRecord(fixture.segmentDescriptors[segmentOrdinal],
            fixture.sessionDescriptor, fixture.resourcePolicy, 2ULL + segmentOrdinal));
    }
    fixture.controlRecords.push_back(MakeFinalManifestControlRecord(fixture.finalManifest,
        fixture.sessionDescriptor, fixture.resourcePolicy, 2ULL + segmentCount));
    return fixture;
}

void CountOuterAdmission(const pbreceiver::ReceiverOuterSymbolAdmission admission,
    ReceiverEvidenceSummary& summary)
{
    switch (admission)
    {
    case pbreceiver::ReceiverOuterSymbolAdmission::NotApplicable:
        break;
    case pbreceiver::ReceiverOuterSymbolAdmission::Unique:
        summary.uniqueOuterSymbols++;
        break;
    case pbreceiver::ReceiverOuterSymbolAdmission::IdenticalDuplicate:
        summary.identicalDuplicateOuterSymbols++;
        break;
    case pbreceiver::ReceiverOuterSymbolAdmission::RecoveryAlreadyReady:
        summary.recoveryReadyOuterSymbols++;
        break;
    case pbreceiver::ReceiverOuterSymbolAdmission::AlreadyCompleted:
        summary.alreadyCompletedOuterSymbols++;
        break;
    }
}

bool IsOuterConflict(const pbreceiver::ReceiverError& error)
{
    if (const auto* outerError = std::get_if<pbouterfec::OuterFecError>(&error))
    {
        return outerError->code == pbouterfec::OuterFecErrorCode::OuterBlockConflict;
    }
    if (const auto* protocolError = std::get_if<pbprotocol::ProtocolError>(&error))
    {
        return protocolError->code == pbprotocol::ProtocolErrorCode::OrphanPayloadConflict;
    }
    return false;
}

} // namespace

bool ReceiverEvidenceSummary::IsSafe() const noexcept
{
    return receiverRejections == 0 && outerConflictRejections == 0 &&
        resourcePolicyRejections == 0 && wholeFileDigestDisposition != WholeFileDigestDisposition::Mismatch &&
        wholeFileDigestDisposition != WholeFileDigestDisposition::ReceiverRejected;
}

const pbprotocol::SessionId& GetReceiverEvidenceSessionId() noexcept
{
    static const pbprotocol::SessionId sessionId{kEvidenceSessionIdBytes};
    return sessionId;
}

pbprotocol::SessionTag GetReceiverEvidenceSessionTag() noexcept
{
    return pbprotocol::DeriveSessionTag(GetReceiverEvidenceSessionId());
}

bool MakeReceiverEvidenceBootstrapRecord(const std::uint64_t frameSequence,
    std::array<std::byte, pbprotocol::kBootstrapRecordBytes>& output, std::string& error)
{
    error.clear();
    pbprotocol::BootstrapRecord record;
    record.protocolVersion = pbprotocol::GetProtocolVersion();
    record.visualLayoutVersion = pbmodulation::kRemoteVisualLowFpsLayoutVersion;
    record.visualProfileId = pbmodulation::kRemoteVisualLowFpsProfileId;
    record.sessionTag = GetReceiverEvidenceSessionTag();
    record.frameSequence = frameSequence;
    if (!pbprotocol::SerializeBootstrapRecord(record, output))
    {
        error = "receiver evidence Bootstrap serialization failed";
        return false;
    }
    return true;
}

bool MakeReceiverEvidenceExpectedTransportBlock(const std::uint64_t segmentOrdinal,
    const std::uint32_t slot, pbdesktoplevels::AcceptedTransportBlock& output, std::string& error)
{
    error.clear();
    if (slot >= 4 || segmentOrdinal >= kMaximumReceiverEvidenceSegments)
    {
        error = "receiver evidence Transport truth identity is outside the bounded fixture";
        return false;
    }
    try
    {
        std::array<std::byte, pbprotocol::kBootstrapRecordBytes> bootstrap{};
        if (!MakeReceiverEvidenceBootstrapRecord(segmentOrdinal, bootstrap, error))
        {
            return false;
        }
        std::vector<std::byte> logicalData(pbmodulation::kRemoteVisualLowFpsDataBytes);
        if (!pbdesktoplevels::GenerateDiagnosticData(bootstrap, logicalData))
        {
            throw std::runtime_error("diagnostic sender data generation failed");
        }
        const auto infoBlock = std::span(logicalData).subspan(
            static_cast<std::size_t>(slot) * pbdesktoplevels::kCodewordBytes,
            pbdesktoplevels::kInfoBytes);
        const auto blockResult = pbprotocol::ExtractTransportBlockFromInfoBlock(infoBlock);
        if (!blockResult)
        {
            throw std::runtime_error("diagnostic sender Transport extraction failed");
        }
        if (blockResult.Value().size() > output.bytes.size())
        {
            throw std::runtime_error("diagnostic sender Transport exceeds evidence storage");
        }
        pbdesktoplevels::AcceptedTransportBlock expected;
        expected.slot = slot;
        expected.byteCount = static_cast<std::uint32_t>(blockResult.Value().size());
        std::copy(blockResult.Value().begin(), blockResult.Value().end(), expected.bytes.begin());
        output = expected;
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
    }
    catch (...)
    {
        error = "unknown Transport truth generation failure";
    }
    return false;
}

std::uint32_t CountProductionTruthMismatches(const std::uint64_t expectedSegmentOrdinal,
    const std::span<const pbdesktoplevels::AcceptedTransportBlock> acceptedBlocks)
{
    std::uint32_t mismatches = 0;
    for (const pbdesktoplevels::AcceptedTransportBlock& accepted : acceptedBlocks)
    {
        if (accepted.byteCount == 0 || accepted.byteCount > accepted.bytes.size())
        {
            mismatches++;
            continue;
        }
        const auto parsedResult = pbprotocol::ParseTransportBlock(
            std::span(accepted.bytes).first(accepted.byteCount));
        if (!parsedResult || parsedResult.Value().header.segmentOrdinal != expectedSegmentOrdinal ||
            accepted.slot != parsedResult.Value().header.outerBlockId ||
            parsedResult.Value().header.outerBlockId >= 4)
        {
            mismatches++;
            continue;
        }
        pbdesktoplevels::AcceptedTransportBlock expected;
        std::string error;
        if (!MakeReceiverEvidenceExpectedTransportBlock(expectedSegmentOrdinal,
                parsedResult.Value().header.outerBlockId, expected, error) || expected.byteCount != accepted.byteCount ||
            !std::equal(expected.bytes.begin(), expected.bytes.begin() + expected.byteCount, accepted.bytes.begin()))
        {
            mismatches++;
        }
    }
    return mismatches;
}

bool EvaluateReceiverEvidence(const std::uint32_t segmentCount,
    const std::span<const pbdesktoplevels::AcceptedTransportBlock> acceptedBlocks,
    ReceiverEvidenceSummary& output, std::string& error)
{
    error.clear();
    try
    {
        if (acceptedBlocks.size() > kMaximumReceiverEvidenceInputBlocks)
        {
            throw std::invalid_argument("receiver evidence input block count exceeds the bounded policy");
        }
        const ReceiverFixture fixture = BuildFixture(segmentCount);
        ReceiverEvidenceSummary summary;
        summary.configuredSegments = segmentCount;
        summary.inputTransportBlocks = static_cast<std::uint32_t>(acceptedBlocks.size());
        summary.expectedWholeFileBlake3 = DigestToHex(fixture.finalManifest.wholeFileDigest.bytes);
        auto receiverResult = pbreceiver::ReceiverIngress::Create(fixture.resourcePolicy,
            kReceiverEvidenceOuterBlockBytes);
        if (!receiverResult)
        {
            throw std::runtime_error("ReceiverIngress creation failed");
        }
        pbreceiver::ReceiverIngress receiver = std::move(receiverResult).Value();
        for (const std::vector<std::byte>& controlRecord : fixture.controlRecords)
        {
            const auto controlResult = receiver.ReceiveControlRecord(controlRecord);
            if (!controlResult)
            {
                throw std::runtime_error("ReceiverIngress rejected canonical evidence Control");
            }
        }

        std::vector<std::byte> storedBytes(fixture.wholeFileBytes.size(), std::byte{0});
        for (const pbdesktoplevels::AcceptedTransportBlock& accepted : acceptedBlocks)
        {
            if (accepted.byteCount == 0 || accepted.byteCount > accepted.bytes.size())
            {
                summary.receiverRejections++;
                summary.wholeFileDigestDisposition = WholeFileDigestDisposition::ReceiverRejected;
                continue;
            }
            const auto serialized = std::span(accepted.bytes).first(accepted.byteCount);
            const auto parsedResult = pbprotocol::ParseTransportBlock(serialized);
            if (!parsedResult)
            {
                summary.receiverRejections++;
                summary.wholeFileDigestDisposition = WholeFileDigestDisposition::ReceiverRejected;
                continue;
            }
            const pbprotocol::TransportBlockView& parsed = parsedResult.Value();
            summary.parsedTransportBlocks++;
            const pbreceiver::ReceivedTransportBlock received{parsed.header.sessionTag,
                parsed.header.segmentOrdinal, parsed.header.outerBlockId, parsed.header.payloadBytes, parsed.payload};
            auto dataResult = receiver.ReceiveDataBlock(received);
            if (!dataResult)
            {
                summary.receiverRejections++;
                summary.outerConflictRejections += IsOuterConflict(dataResult.Error()) ? 1U : 0U;
                summary.wholeFileDigestDisposition = WholeFileDigestDisposition::ReceiverRejected;
                continue;
            }
            CountOuterAdmission(dataResult.Value().outerSymbolAdmission, summary);
            if (!dataResult.Value().completedSegment.has_value())
            {
                continue;
            }

            auto verifiedResult = receiver.VerifyRecoveredSegment(
                std::move(*dataResult.Value().completedSegment));
            if (!verifiedResult)
            {
                summary.receiverRejections++;
                summary.wholeFileDigestDisposition = WholeFileDigestDisposition::ReceiverRejected;
                continue;
            }
            pbreceiver::ReceiverVerifiedSegment verified = std::move(verifiedResult).Value();
            const pbprotocol::SegmentDescriptor& descriptor =
                verified.GetBoundSegmentDescriptor().GetDescriptor();
            const std::span<const std::byte> rawBytes = verified.GetRawBytes();
            const std::size_t verifiedRawSize = rawBytes.size();
            if (descriptor.rawOffset > storedBytes.size() || rawBytes.size() > storedBytes.size() - descriptor.rawOffset)
            {
                throw std::runtime_error("verified Segment exceeds bounded evidence storage");
            }
            std::copy(rawBytes.begin(), rawBytes.end(), storedBytes.begin() +
                static_cast<std::ptrdiff_t>(descriptor.rawOffset));
            const auto commitResult = receiver.CommitStoredSegment(std::move(verified));
            if (!commitResult || commitResult.Value() != pbreceiver::ReceiverSegmentCommitDisposition::Committed)
            {
                throw std::runtime_error("ReceiverIngress storage commit failed");
            }
            summary.verifiedSegments++;
            summary.verifiedRawBytes += verifiedRawSize;
        }

        const pbreceiver::ReceiverResourceTelemetrySnapshot telemetry = receiver.GetTelemetry();
        summary.resourcePolicyRejections = telemetry.totalResourcePolicyRejectedCount;
        if (summary.wholeFileDigestDisposition == WholeFileDigestDisposition::ReceiverRejected)
        {
            output = std::move(summary);
            return true;
        }
        if (summary.verifiedSegments != segmentCount)
        {
            summary.wholeFileDigestDisposition = WholeFileDigestDisposition::NotReady;
            output = std::move(summary);
            return true;
        }

        const auto finalizationResult = receiver.PrepareFinalization(GetReceiverEvidenceSessionTag());
        if (!finalizationResult || finalizationResult.Value() != fixture.finalManifest)
        {
            throw std::runtime_error("ReceiverIngress finalization did not reproduce FinalManifest");
        }
        summary.finalizationPrepared = true;
        const auto observedDigest = pbprotocol::ComputeBlake3Digest(storedBytes);
        summary.observedWholeFileBlake3 = DigestToHex(observedDigest);
        summary.wholeFileDigestDisposition = observedDigest == fixture.finalManifest.wholeFileDigest.bytes &&
            storedBytes == fixture.wholeFileBytes ? WholeFileDigestDisposition::Pass :
            WholeFileDigestDisposition::Mismatch;
        output = std::move(summary);
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
    }
    catch (...)
    {
        error = "unknown Receiver evidence failure";
    }
    return false;
}

const char* GetWholeFileDigestDispositionName(const WholeFileDigestDisposition disposition) noexcept
{
    switch (disposition)
    {
    case WholeFileDigestDisposition::NotReady:
        return "NotReady";
    case WholeFileDigestDisposition::Pass:
        return "Pass";
    case WholeFileDigestDisposition::Mismatch:
        return "Mismatch";
    case WholeFileDigestDisposition::ReceiverRejected:
        return "ReceiverRejected";
    }
    return "ReceiverRejected";
}

} // namespace pbremotevisualreceiverevidence
