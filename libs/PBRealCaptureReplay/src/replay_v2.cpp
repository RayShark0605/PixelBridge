#include "pbrealcapturereplay/replay_v2.h"

#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/checked_integer.h"
#include "pbprotocol/crc32c.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace pbrealcapturereplay
{
namespace
{

constexpr char fileMagic[8] = {'P', 'B', 'R', 'C', 'V', '0', '0', '2'};
constexpr char recordMagic[8] = {'P', 'B', 'R', 'O', 'V', '0', '0', '2'};
constexpr char footerMagic[8] = {'P', 'B', 'R', 'V', 'F', '0', '0', '2'};
constexpr std::uint32_t endianSentinel = 0x01020304;
constexpr std::uint64_t unavailableUint64 = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint64_t noRoiCopyTime = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint32_t captureFlagSenderRaster = 1u << 0;
constexpr std::uint32_t captureFlagCanonicalBootstrap = 1u << 1;
constexpr std::uint8_t demodFlagBootstrapAttempted = 1u << 0;
constexpr std::uint8_t demodFlagBootstrapSucceeded = 1u << 1;
constexpr std::uint8_t demodFlagFrameSequenceAvailable = 1u << 2;
constexpr std::uint8_t demodFlagTransportProduced = 1u << 3;
constexpr std::uint8_t demodFlagReceiverAdmitted = 1u << 4;
constexpr std::uint8_t demodFlagProductionDetail = 1u << 5;
constexpr std::uint8_t demodDetailFlagLayoutAvailable = 1u << 0;
constexpr std::uint8_t demodDetailFlagGeometryAvailable = 1u << 1;
constexpr std::uint8_t demodDetailFlagEvaluationAvailable = 1u << 2;
constexpr std::uint8_t demodDetailFlagMetricSummaryAvailable = 1u << 3;
constexpr std::uint8_t demodDetailFlagCarrierAccepted = 1u << 4;
constexpr std::uint8_t demodDetailFlagReceiverStateAdvanced = 1u << 5;
constexpr std::uint8_t demodDetailFlagSenderTruthAvailable = 1u << 6;
constexpr std::uint8_t demodDetailFlagPaddingValid = 1u << 7;
constexpr std::uint8_t demodTimingFlagGpuTimingAvailable = 1u << 0;
constexpr std::uint8_t captureFlagHdr = 1u << 0;
constexpr std::uint8_t captureFlagCursorExcluded = 1u << 1;
constexpr std::size_t fileHeaderCrcOffset = 252;
constexpr std::size_t recordHeaderCrcOffset = 508;
constexpr std::size_t footerCrcOffset = 92;
constexpr std::uint32_t maximumMetadataHardBytes = 1024 * 1024;
constexpr std::uint32_t maximumBootstrapHardBytes = 1024 * 1024;

struct CaptureIdentity
{
    std::uint64_t captureEpoch = 0;
    std::uint64_t captureObservation = 0;
    bool hasDemodObservation = false;
};

template<std::size_t Size>
void StoreMagic(std::array<std::byte, Size>& output, const char (&magic)[8]) noexcept
{
    static_assert(Size >= 8);
    std::memcpy(output.data(), magic, 8);
}

bool HasMagic(const std::span<const std::byte> input, const char (&magic)[8]) noexcept
{
    return input.size() >= 8 && std::memcmp(input.data(), magic, 8) == 0;
}

void StoreUint16(const std::span<std::byte> output, const std::size_t offset, const std::uint16_t value) noexcept
{
    output[offset] = static_cast<std::byte>(value & 0xFF);
    output[offset + 1] = static_cast<std::byte>(value >> 8);
}

void StoreUint32(const std::span<std::byte> output, const std::size_t offset, const std::uint32_t value) noexcept
{
    for (std::size_t index = 0; index < 4; index++)
    {
        output[offset + index] = static_cast<std::byte>((value >> (index * 8)) & 0xFF);
    }
}

void StoreUint64(const std::span<std::byte> output, const std::size_t offset, const std::uint64_t value) noexcept
{
    for (std::size_t index = 0; index < 8; index++)
    {
        output[offset + index] = static_cast<std::byte>((value >> (index * 8)) & 0xFF);
    }
}

void StoreInt32(const std::span<std::byte> output, const std::size_t offset, const std::int32_t value) noexcept
{
    StoreUint32(output, offset, std::bit_cast<std::uint32_t>(value));
}

void StoreInt64(const std::span<std::byte> output, const std::size_t offset, const std::int64_t value) noexcept
{
    StoreUint64(output, offset, std::bit_cast<std::uint64_t>(value));
}

void StoreDouble(const std::span<std::byte> output, const std::size_t offset, const double value) noexcept
{
    StoreUint64(output, offset, std::bit_cast<std::uint64_t>(value));
}

std::uint16_t LoadUint16(const std::span<const std::byte> input, const std::size_t offset) noexcept
{
    return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(input[offset]) |
        (std::to_integer<std::uint16_t>(input[offset + 1]) << 8));
}

std::uint32_t LoadUint32(const std::span<const std::byte> input, const std::size_t offset) noexcept
{
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; index++)
    {
        value |= std::to_integer<std::uint32_t>(input[offset + index]) << (index * 8);
    }
    return value;
}

std::uint64_t LoadUint64(const std::span<const std::byte> input, const std::size_t offset) noexcept
{
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; index++)
    {
        value |= std::to_integer<std::uint64_t>(input[offset + index]) << (index * 8);
    }
    return value;
}

std::int32_t LoadInt32(const std::span<const std::byte> input, const std::size_t offset) noexcept
{
    return std::bit_cast<std::int32_t>(LoadUint32(input, offset));
}

std::int64_t LoadInt64(const std::span<const std::byte> input, const std::size_t offset) noexcept
{
    return std::bit_cast<std::int64_t>(LoadUint64(input, offset));
}

double LoadDouble(const std::span<const std::byte> input, const std::size_t offset) noexcept
{
    return std::bit_cast<double>(LoadUint64(input, offset));
}

bool IsZero(const std::span<const std::byte> bytes) noexcept
{
    return std::ranges::all_of(bytes, [](const std::byte value) { return value == std::byte{0}; });
}

bool IsNonzero(const std::span<const std::byte> bytes) noexcept
{
    return std::ranges::any_of(bytes, [](const std::byte value) { return value != std::byte{0}; });
}

std::array<std::byte, 32> LoadDigest(const std::span<const std::byte> bytes, const std::size_t offset) noexcept
{
    std::array<std::byte, 32> digest{};
    std::copy_n(bytes.begin() + offset, digest.size(), digest.begin());
    return digest;
}

bool ValidRunId(const std::string_view value) noexcept
{
    return value.size() == 32 && std::ranges::all_of(value, [](const char character)
    {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
}

bool ValidUtf8(const std::string_view text, const bool allowEmpty) noexcept
{
    if (text.empty())
    {
        return allowEmpty;
    }
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    std::size_t index = 0;
    while (index < text.size())
    {
        const unsigned char first = bytes[index];
        if (first >= 1 && first <= 0x7F)
        {
            index++;
            continue;
        }
        std::size_t continuationCount = 0;
        unsigned char secondMinimum = 0x80;
        unsigned char secondMaximum = 0xBF;
        if (first >= 0xC2 && first <= 0xDF)
        {
            continuationCount = 1;
        }
        else if (first >= 0xE0 && first <= 0xEF)
        {
            continuationCount = 2;
            if (first == 0xE0)
            {
                secondMinimum = 0xA0;
            }
            else if (first == 0xED)
            {
                secondMaximum = 0x9F;
            }
        }
        else if (first >= 0xF0 && first <= 0xF4)
        {
            continuationCount = 3;
            if (first == 0xF0)
            {
                secondMinimum = 0x90;
            }
            else if (first == 0xF4)
            {
                secondMaximum = 0x8F;
            }
        }
        else
        {
            return false;
        }
        if (continuationCount > text.size() - index - 1 || bytes[index + 1] < secondMinimum || bytes[index + 1] > secondMaximum)
        {
            return false;
        }
        for (std::size_t continuation = 2; continuation <= continuationCount; continuation++)
        {
            if (bytes[index + continuation] < 0x80 || bytes[index + continuation] > 0xBF)
            {
                return false;
            }
        }
        index += continuationCount + 1;
    }
    return true;
}

bool ValidCaptureBackend(const pbcapturenormalize::CaptureBackendKind value) noexcept
{
    return value == pbcapturenormalize::CaptureBackendKind::Wgc || value == pbcapturenormalize::CaptureBackendKind::Dxgi;
}

bool ValidTimestampDomain(const pbcapturenormalize::CaptureTimestampDomain value) noexcept
{
    return value == pbcapturenormalize::CaptureTimestampDomain::WgcSystemRelative100ns ||
        value == pbcapturenormalize::CaptureTimestampDomain::DxgiQpcTicks;
}

bool ValidSignalEncoding(const pbcapturenormalize::CaptureSignalEncoding value) noexcept
{
    return value == pbcapturenormalize::CaptureSignalEncoding::Unknown ||
        value == pbcapturenormalize::CaptureSignalEncoding::SdrRgb ||
        value == pbcapturenormalize::CaptureSignalEncoding::LinearScRgb;
}

bool ValidCursorState(const pbcapturenormalize::CursorState value) noexcept
{
    return value >= pbcapturenormalize::CursorState::Unknown && value <= pbcapturenormalize::CursorState::KnownAbsent;
}

bool ValidRotation(const DXGI_MODE_ROTATION value) noexcept
{
    return value >= DXGI_MODE_ROTATION_IDENTITY && value <= DXGI_MODE_ROTATION_ROTATE270;
}

bool ValidDisposition(const ReplayV2DemodDisposition value) noexcept
{
    return value >= ReplayV2DemodDisposition::Unavailable && value <= ReplayV2DemodDisposition::Accepted;
}

bool ValidResultKind(const ReplayV2DemodResultKind value) noexcept
{
    return value >= ReplayV2DemodResultKind::Unavailable && value <= ReplayV2DemodResultKind::TelemetryOnly;
}

bool ValidGeometryStatus(const ReplayV2GeometryStatus value) noexcept
{
    return value >= ReplayV2GeometryStatus::Unavailable && value <= ReplayV2GeometryStatus::Rejected;
}

bool ValidTemporalDisposition(const ReplayV2TemporalDisposition value) noexcept
{
    return value >= ReplayV2TemporalDisposition::Unavailable && value <= ReplayV2TemporalDisposition::StaleCompletion;
}

bool HasEmptyProductionDetail(const ReplayV2DemodObservationView& observation) noexcept
{
    return !observation.layoutAvailable && observation.visualLayoutVersion == 0 &&
        observation.resultKind == ReplayV2DemodResultKind::Unavailable && !observation.geometryAvailable &&
        observation.geometryStatus == ReplayV2GeometryStatus::Unavailable && observation.geometryOriginX == 0 &&
        observation.geometryOriginY == 0 && observation.geometryScaleX == 0 && observation.geometryScaleY == 0 &&
        observation.temporalDisposition == ReplayV2TemporalDisposition::Unavailable && !observation.evaluationAvailable &&
        !observation.paddingValid && !observation.senderTruthAvailable && observation.codewords == 0 &&
        observation.fecFailures == 0 && observation.crcFailures == 0 && observation.identityFailures == 0 &&
        observation.falseAcceptedCodewords == 0 && observation.acceptedTransportBlocks == 0 &&
        observation.acceptedRemoteControlBlocks == 0 && observation.admittedTransportBlocks == 0 &&
        observation.admittedRemoteControlBlocks == 0 && observation.iterationsTotal == 0 &&
        observation.iterationsMaximum == 0 && observation.comparedCodedBits == 0 &&
        observation.erroneousCodedBits == 0 && !observation.metricSummaryAvailable && observation.metricSamples == 0 &&
        observation.zeroMagnitudeMetrics == 0 && observation.minimumAbsoluteMetric == 0 &&
        observation.meanAbsoluteMetric == 0 && observation.freshnessRegions == 0 && observation.staleRegions == 0 &&
        observation.freshnessTagMismatches == 0 && observation.freshnessTagErasures == 0 &&
        observation.freshnessErasedDataMetrics == 0 && observation.unreliableSymbols == 0 &&
        observation.metricReadbackBytes == 0 && !observation.gpuTimingAvailable && observation.gpuTime100ns == 0 &&
        !observation.carrierAccepted && !observation.receiverStateAdvanced;
}

std::optional<std::uint32_t> BytesPerPixel(const DXGI_FORMAT format) noexcept
{
    switch (format)
    {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM: return 4;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
    default: return std::nullopt;
    }
}

ReplayStatus AddWithinLimit(const std::uint64_t left, const std::uint64_t right, const std::uint64_t limit,
    std::uint64_t& output, const ReplayStage stage, const std::uint64_t offset = 0) noexcept
{
    const auto result = pbprotocol::CheckedAddUint64WithinLimit(left, right, limit);
    if (!result)
    {
        return ReplayStatus::Failure(ReplayError::ResourceLimit, stage, 0, offset);
    }
    output = result.Value();
    return {};
}

ReplayStatus ValidateLimits(const ReplayV2Limits& limits) noexcept
{
    const std::uint64_t minimumFileBytes = kReplayV2FileHeaderBytes + kReplayV2FileFooterBytes;
    if (limits.maximumFileBytes < minimumFileBytes || limits.maximumFileBytes > kReplayV2HardMaximumFileBytes ||
        limits.maximumRasterBytesPerFrame == 0 || limits.maximumRasterBytesPerFrame > kReplayV2HardMaximumFileBytes ||
        limits.maximumTotalRasterBytes == 0 || limits.maximumTotalRasterBytes > limits.maximumFileBytes ||
        limits.maximumCaptureFrames == 0 || limits.maximumCaptureFrames > kReplayV2HardMaximumCaptureFrames ||
        limits.maximumMetadataBytes == 0 || limits.maximumMetadataBytes > maximumMetadataHardBytes ||
        limits.maximumDisplayIdentityBytes == 0 || limits.maximumDisplayIdentityBytes > maximumMetadataHardBytes ||
        limits.maximumCanonicalBootstrapBytes > maximumBootstrapHardBytes || limits.maximumDimension == 0 || limits.maximumDimension > 16384)
    {
        return ReplayStatus::Failure(ReplayError::InvalidArgument, ReplayStage::Configuration);
    }
    return {};
}

ReplayStatus ValidateDescriptor(const ReplayV2FileDescriptor& descriptor, const ReplayV2Limits& limits) noexcept
{
    if (descriptor.datasetClass != ReplayDatasetClass::RemoteVisual || !IsNonzero(descriptor.datasetId) ||
        !ValidRunId(descriptor.runId) || descriptor.visualProfileId == 0 || descriptor.createdUtc100ns <= 0 ||
        descriptor.remoteMetadataJsonUtf8.empty() || descriptor.remoteMetadataJsonUtf8.size() > limits.maximumMetadataBytes ||
        !ValidUtf8(descriptor.remoteMetadataJsonUtf8, false))
    {
        return ReplayStatus::Failure(ReplayError::InvalidArgument, ReplayStage::Configuration);
    }
    return {};
}

ReplayStatus ValidateRaster(const ReplayRasterView& raster, const ReplayV2Limits& limits, const bool allowEmpty) noexcept
{
    if (allowEmpty && raster.width == 0 && raster.height == 0 && raster.rowPitch == 0 &&
        raster.pixelFormat == DXGI_FORMAT_UNKNOWN && raster.pixels.empty())
    {
        return {};
    }
    const auto bytesPerPixel = BytesPerPixel(raster.pixelFormat);
    if (raster.width == 0 || raster.height == 0 || raster.width > limits.maximumDimension ||
        raster.height > limits.maximumDimension || !bytesPerPixel)
    {
        return ReplayStatus::Failure(ReplayError::InvalidArgument, ReplayStage::RecordHeader);
    }
    const auto minimumPitch = pbprotocol::CheckedMultiplyUint64(raster.width, *bytesPerPixel);
    const auto expectedBytes = pbprotocol::CheckedMultiplyUint64(raster.rowPitch, raster.height);
    if (!minimumPitch || !expectedBytes || raster.rowPitch < minimumPitch.Value() || expectedBytes.Value() != raster.pixels.size() ||
        expectedBytes.Value() > limits.maximumRasterBytesPerFrame)
    {
        return ReplayStatus::Failure(ReplayError::ResourceLimit, ReplayStage::RecordPayload);
    }
    return {};
}

ReplayStatus ValidateCapture(const ReplayV2CaptureView& frame, const ReplayV2Limits& limits) noexcept
{
    const auto capturedStatus = ValidateRaster(frame.capturedRoi, limits, false);
    if (!capturedStatus)
    {
        return capturedStatus;
    }
    if (frame.senderCanonicalRaster)
    {
        const auto senderStatus = ValidateRaster(*frame.senderCanonicalRaster, limits, false);
        if (!senderStatus)
        {
            return senderStatus;
        }
    }
    const auto& capture = frame.capture;
    if (!IsNonzero(capture.domain.sourceId) || capture.domain.captureEpoch == 0 || capture.captureObservation == 0 ||
        !ValidCaptureBackend(capture.backend) || !ValidTimestampDomain(capture.timestamp.domain) ||
        !ValidSignalEncoding(capture.signalEncoding) || !ValidCursorState(capture.sourceCursorState) ||
        !ValidRotation(capture.displayRotation) || !ValidRotation(capture.sourceTransform) ||
        capture.timestamp.rawFrequency <= 0 || capture.timestamp.monotonic100ns < 0 || capture.timestamp.arrivalQpc100ns < -1 ||
        capture.roiSize.width <= 0 || capture.roiSize.height <= 0 || capture.sourceContentSize.width <= 0 ||
        capture.sourceContentSize.height <= 0 || capture.sourceExtent.width <= 0 || capture.sourceExtent.height <= 0 ||
        capture.sourceContentSize.width > static_cast<std::int32_t>(limits.maximumDimension) ||
        capture.sourceContentSize.height > static_cast<std::int32_t>(limits.maximumDimension) ||
        capture.sourceExtent.width > static_cast<std::int32_t>(limits.maximumDimension) ||
        capture.sourceExtent.height > static_cast<std::int32_t>(limits.maximumDimension) ||
        capture.roiSize.width != static_cast<std::int32_t>(frame.capturedRoi.width) ||
        capture.roiSize.height != static_cast<std::int32_t>(frame.capturedRoi.height) ||
        capture.pixelFormat != frame.capturedRoi.pixelFormat || !BytesPerPixel(capture.sourcePixelFormat) ||
        frame.dpiX == 0 || frame.dpiY == 0 || frame.dpiX > 960 || frame.dpiY > 960 ||
        !std::isfinite(frame.scaleX) || !std::isfinite(frame.scaleY) || frame.scaleX <= 0 || frame.scaleY <= 0 ||
        frame.scaleX > 16 || frame.scaleY > 16 || frame.displayIdentityUtf8.empty() ||
        frame.displayIdentityUtf8.size() > limits.maximumDisplayIdentityBytes || !ValidUtf8(frame.displayIdentityUtf8, false) ||
        frame.canonicalBootstrap.size() > limits.maximumCanonicalBootstrapBytes)
    {
        return ReplayStatus::Failure(ReplayError::InvalidArgument, ReplayStage::RecordHeader);
    }
    const auto physicalWidth = static_cast<std::int64_t>(capture.physicalRoi.right) - capture.physicalRoi.left;
    const auto physicalHeight = static_cast<std::int64_t>(capture.physicalRoi.bottom) - capture.physicalRoi.top;
    if (physicalWidth != capture.roiSize.width || physicalHeight != capture.roiSize.height ||
        (capture.pointer.separateVisible && !capture.pointer.positionKnown) || capture.pointer.shapeBytes > 16 * 1024 * 1024)
    {
        return ReplayStatus::Failure(ReplayError::InvalidArgument, ReplayStage::RecordHeader);
    }
    return {};
}

ReplayStatus ValidateDemodObservation(const ReplayV2DemodObservationView& observation) noexcept
{
    if (observation.captureEpoch == 0 || observation.captureObservation == 0 || observation.visualProfileId == 0 ||
        !ValidDisposition(observation.disposition) || (observation.bootstrapSucceeded && !observation.bootstrapAttempted) ||
        (observation.receiverAdmitted && !observation.transportProduced) ||
        (!observation.frameSequenceAvailable && observation.frameSequence != 0) ||
        (observation.disposition == ReplayV2DemodDisposition::Accepted && !observation.receiverAdmitted))
    {
        return ReplayStatus::Failure(ReplayError::InvalidArgument, ReplayStage::RecordHeader);
    }
    if (!observation.productionDetailAvailable)
    {
        return HasEmptyProductionDetail(observation) ? ReplayStatus{} :
            ReplayStatus::Failure(ReplayError::InvalidArgument, ReplayStage::RecordHeader);
    }
    if (!ValidResultKind(observation.resultKind) || observation.resultKind == ReplayV2DemodResultKind::Unavailable ||
        !ValidGeometryStatus(observation.geometryStatus) ||
        observation.geometryStatus == ReplayV2GeometryStatus::Unavailable ||
        !ValidTemporalDisposition(observation.temporalDisposition) ||
        observation.temporalDisposition == ReplayV2TemporalDisposition::Unavailable ||
        observation.layoutAvailable != (observation.visualLayoutVersion != 0) ||
        observation.layoutAvailable != observation.frameSequenceAvailable ||
        observation.frameSequenceAvailable != observation.bootstrapSucceeded ||
        observation.receiverStateAdvanced && !observation.carrierAccepted ||
        observation.receiverAdmitted != (observation.transportProduced && observation.receiverStateAdvanced) ||
        (observation.disposition == ReplayV2DemodDisposition::Accepted) != observation.receiverAdmitted)
    {
        return ReplayStatus::Failure(ReplayError::InvalidArgument, ReplayStage::RecordHeader);
    }
    if (observation.geometryAvailable)
    {
        if (observation.geometryStatus != ReplayV2GeometryStatus::ExactCanvas &&
            observation.geometryStatus != ReplayV2GeometryStatus::Scaled &&
            observation.geometryStatus != ReplayV2GeometryStatus::Letterboxed)
        {
            return ReplayStatus::Failure(ReplayError::InvalidArgument, ReplayStage::RecordHeader);
        }
        if (!std::isfinite(observation.geometryOriginX) || !std::isfinite(observation.geometryOriginY) ||
            !std::isfinite(observation.geometryScaleX) || !std::isfinite(observation.geometryScaleY) ||
            observation.geometryScaleX <= 0 || observation.geometryScaleY <= 0 ||
            observation.geometryScaleX > 16 || observation.geometryScaleY > 16)
        {
            return ReplayStatus::Failure(ReplayError::InvalidArgument, ReplayStage::RecordHeader);
        }
    }
    else if (observation.geometryOriginX != 0 || observation.geometryOriginY != 0 ||
        observation.geometryScaleX != 0 || observation.geometryScaleY != 0 ||
        (observation.geometryStatus != ReplayV2GeometryStatus::NotApplicable &&
         observation.geometryStatus != ReplayV2GeometryStatus::Rejected))
    {
        return ReplayStatus::Failure(ReplayError::InvalidArgument, ReplayStage::RecordHeader);
    }
    if (observation.evaluationAvailable)
    {
        if (observation.codewords == 0 || observation.codewords > kReplayV2HardMaximumCodewordsPerObservation ||
            observation.fecFailures > observation.codewords || observation.crcFailures > observation.codewords ||
            observation.identityFailures > observation.codewords || observation.falseAcceptedCodewords > observation.codewords ||
            observation.acceptedTransportBlocks > observation.codewords ||
            observation.acceptedRemoteControlBlocks > observation.codewords ||
            observation.admittedTransportBlocks > observation.acceptedTransportBlocks ||
            observation.admittedRemoteControlBlocks > observation.acceptedRemoteControlBlocks ||
            observation.iterationsMaximum > observation.iterationsTotal ||
            observation.erroneousCodedBits > observation.comparedCodedBits ||
            (!observation.senderTruthAvailable && (observation.falseAcceptedCodewords != 0 ||
                observation.comparedCodedBits != 0 || observation.erroneousCodedBits != 0)))
        {
            return ReplayStatus::Failure(ReplayError::InvalidArgument, ReplayStage::RecordHeader);
        }
    }
    else if (observation.paddingValid || observation.senderTruthAvailable || observation.codewords != 0 ||
        observation.fecFailures != 0 || observation.crcFailures != 0 || observation.identityFailures != 0 ||
        observation.falseAcceptedCodewords != 0 || observation.acceptedTransportBlocks != 0 ||
        observation.acceptedRemoteControlBlocks != 0 || observation.admittedTransportBlocks != 0 ||
        observation.admittedRemoteControlBlocks != 0 || observation.iterationsTotal != 0 ||
        observation.iterationsMaximum != 0 || observation.comparedCodedBits != 0 || observation.erroneousCodedBits != 0)
    {
        return ReplayStatus::Failure(ReplayError::InvalidArgument, ReplayStage::RecordHeader);
    }
    if (observation.metricSummaryAvailable)
    {
        if (!observation.evaluationAvailable || observation.metricSamples == 0 ||
            observation.zeroMagnitudeMetrics > observation.metricSamples ||
            observation.freshnessErasedDataMetrics > observation.metricSamples ||
            observation.unreliableSymbols > observation.metricSamples ||
            !std::isfinite(observation.minimumAbsoluteMetric) || !std::isfinite(observation.meanAbsoluteMetric) ||
            observation.minimumAbsoluteMetric < 0 || observation.meanAbsoluteMetric < observation.minimumAbsoluteMetric ||
            observation.metricReadbackBytes == 0)
        {
            return ReplayStatus::Failure(ReplayError::InvalidArgument, ReplayStage::RecordHeader);
        }
    }
    else if (observation.metricSamples != 0 || observation.zeroMagnitudeMetrics != 0 ||
        observation.minimumAbsoluteMetric != 0 || observation.meanAbsoluteMetric != 0 ||
        observation.freshnessRegions != 0 || observation.staleRegions != 0 ||
        observation.freshnessTagMismatches != 0 || observation.freshnessTagErasures != 0 ||
        observation.freshnessErasedDataMetrics != 0 || observation.unreliableSymbols != 0)
    {
        return ReplayStatus::Failure(ReplayError::InvalidArgument, ReplayStage::RecordHeader);
    }
    if ((!observation.gpuTimingAvailable && observation.gpuTime100ns != 0) ||
        observation.transportProduced != (observation.resultKind == ReplayV2DemodResultKind::Transport &&
            observation.acceptedTransportBlocks != 0) ||
        (observation.resultKind == ReplayV2DemodResultKind::TelemetryOnly &&
            (observation.admittedTransportBlocks != 0 || observation.admittedRemoteControlBlocks != 0 ||
             observation.carrierAccepted || observation.receiverStateAdvanced)))
    {
        return ReplayStatus::Failure(ReplayError::InvalidArgument, ReplayStage::RecordHeader);
    }
    return {};
}

CaptureIdentity* FindCapture(std::vector<CaptureIdentity>& captures, const std::uint64_t epoch,
    const std::uint64_t observation) noexcept
{
    const auto iterator = std::ranges::find_if(captures, [epoch, observation](const CaptureIdentity& capture)
    {
        return capture.captureEpoch == epoch && capture.captureObservation == observation;
    });
    return iterator == captures.end() ? nullptr : &*iterator;
}

ReplayStatus ReadExactAt(const HANDLE file, const std::uint64_t offset, const std::span<std::byte> output,
    const ReplayStage stage) noexcept
{
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max()))
    {
        return ReplayStatus::Failure(ReplayError::ResourceLimit, stage, 0, offset);
    }
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN))
    {
        return ReplayStatus::Failure(ReplayError::IoFailure, stage, static_cast<std::int32_t>(GetLastError()), offset);
    }
    std::size_t completed = 0;
    while (completed < output.size())
    {
        const auto remaining = output.size() - completed;
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(remaining, 1u << 30));
        DWORD bytesRead = 0;
        if (!ReadFile(file, output.data() + completed, request, &bytesRead, nullptr))
        {
            return ReplayStatus::Failure(ReplayError::IoFailure, stage, static_cast<std::int32_t>(GetLastError()), offset + completed);
        }
        if (bytesRead == 0)
        {
            return ReplayStatus::Failure(ReplayError::TruncatedInput, stage, 0, offset + completed);
        }
        completed += bytesRead;
    }
    return {};
}

std::array<std::byte, kReplayV2FileHeaderBytes> MakeFileHeader(const ReplayV2FileDescriptor& descriptor,
    const std::uint64_t recordsOffset, const std::array<std::byte, 32>& metadataDigest) noexcept
{
    std::array<std::byte, kReplayV2FileHeaderBytes> header{};
    StoreMagic(header, fileMagic);
    StoreUint16(header, 8, kReplayV2FormatVersion);
    StoreUint16(header, 10, static_cast<std::uint16_t>(kReplayV2FileHeaderBytes));
    StoreUint32(header, 12, endianSentinel);
    StoreUint32(header, 16, static_cast<std::uint32_t>(descriptor.datasetClass));
    std::copy(descriptor.datasetId.begin(), descriptor.datasetId.end(), header.begin() + 24);
    std::memcpy(header.data() + 40, descriptor.runId.data(), descriptor.runId.size());
    StoreInt64(header, 72, descriptor.createdUtc100ns);
    StoreUint32(header, 80, static_cast<std::uint32_t>(descriptor.remoteMetadataJsonUtf8.size()));
    StoreUint64(header, 88, recordsOffset);
    std::copy(metadataDigest.begin(), metadataDigest.end(), header.begin() + 96);
    StoreUint64(header, 128, descriptor.visualProfileId);
    StoreUint32(header, fileHeaderCrcOffset, pbprotocol::ComputeCrc32c(std::span<const std::byte>(header).first(fileHeaderCrcOffset)));
    return header;
}

std::array<std::byte, kReplayV2RecordHeaderBytes> MakeCaptureHeader(const ReplayV2CaptureView& frame,
    const std::uint32_t ordinal, const std::uint64_t recordBytes) noexcept
{
    std::array<std::byte, kReplayV2RecordHeaderBytes> header{};
    StoreMagic(header, recordMagic);
    StoreUint16(header, 8, kReplayV2FormatVersion);
    StoreUint16(header, 10, static_cast<std::uint16_t>(kReplayV2RecordHeaderBytes));
    StoreUint64(header, 12, recordBytes);
    StoreUint32(header, 20, ordinal);
    StoreUint32(header, 24, static_cast<std::uint32_t>(ReplayV2RecordType::Capture));
    const std::uint32_t presenceFlags = (frame.senderCanonicalRaster ? captureFlagSenderRaster : 0) |
        (!frame.canonicalBootstrap.empty() ? captureFlagCanonicalBootstrap : 0);
    StoreUint32(header, 28, presenceFlags);
    StoreUint32(header, 32, static_cast<std::uint32_t>(frame.canonicalBootstrap.size()));
    StoreUint32(header, 36, static_cast<std::uint32_t>(frame.displayIdentityUtf8.size()));
    StoreUint32(header, 40, frame.capturedRoi.width);
    StoreUint32(header, 44, frame.capturedRoi.height);
    StoreUint32(header, 48, frame.capturedRoi.rowPitch);
    StoreUint32(header, 52, static_cast<std::uint32_t>(frame.capturedRoi.pixelFormat));
    StoreUint64(header, 56, frame.capturedRoi.pixels.size());
    if (frame.senderCanonicalRaster)
    {
        StoreUint32(header, 64, frame.senderCanonicalRaster->width);
        StoreUint32(header, 68, frame.senderCanonicalRaster->height);
        StoreUint32(header, 72, frame.senderCanonicalRaster->rowPitch);
        StoreUint32(header, 76, static_cast<std::uint32_t>(frame.senderCanonicalRaster->pixelFormat));
        StoreUint64(header, 80, frame.senderCanonicalRaster->pixels.size());
    }
    StoreUint64(header, 88, frame.capture.captureObservation);
    StoreUint64(header, 96, frame.capture.domain.captureEpoch);
    std::copy(frame.capture.domain.sourceId.begin(), frame.capture.domain.sourceId.end(), header.begin() + 104);
    StoreUint64(header, 120, unavailableUint64);
    header[138] = static_cast<std::byte>(frame.capture.backend);
    header[139] = static_cast<std::byte>(frame.capture.timestamp.domain);
    header[140] = static_cast<std::byte>(frame.capture.signalEncoding);
    header[141] = static_cast<std::byte>((frame.capture.hdr ? captureFlagHdr : 0) |
        (frame.capture.isCursorExcluded ? captureFlagCursorExcluded : 0));
    header[142] = static_cast<std::byte>(frame.capture.sourceCursorState);
    StoreUint64(header, 144, frame.capture.sourceGeneration);
    StoreUint64(header, 152, frame.capture.slotGeneration);
    StoreUint32(header, 160, frame.capture.slotIndex);
    StoreInt32(header, 164, frame.capture.physicalRoi.left);
    StoreInt32(header, 168, frame.capture.physicalRoi.top);
    StoreInt32(header, 172, frame.capture.physicalRoi.right);
    StoreInt32(header, 176, frame.capture.physicalRoi.bottom);
    StoreInt32(header, 180, frame.capture.sourceContentSize.width);
    StoreInt32(header, 184, frame.capture.sourceContentSize.height);
    StoreInt32(header, 188, frame.capture.sourceExtent.width);
    StoreInt32(header, 192, frame.capture.sourceExtent.height);
    StoreInt32(header, 196, frame.capture.roiSize.width);
    StoreInt32(header, 200, frame.capture.roiSize.height);
    StoreUint32(header, 204, static_cast<std::uint32_t>(frame.capture.displayRotation));
    StoreUint32(header, 208, static_cast<std::uint32_t>(frame.capture.sourceTransform));
    StoreUint32(header, 212, static_cast<std::uint32_t>(frame.capture.sourcePixelFormat));
    StoreUint32(header, 216, static_cast<std::uint32_t>(frame.capture.pixelFormat));
    StoreInt32(header, 220, frame.capture.adapterLuid.HighPart);
    StoreUint32(header, 224, frame.capture.adapterLuid.LowPart);
    StoreUint32(header, 228, frame.capture.bitsPerColor);
    StoreUint32(header, 232, frame.capture.outputColorSpace);
    StoreInt64(header, 236, frame.capture.timestamp.rawValue);
    StoreInt64(header, 244, frame.capture.timestamp.rawFrequency);
    StoreInt64(header, 252, frame.capture.timestamp.monotonic100ns);
    StoreInt64(header, 260, frame.capture.timestamp.arrivalQpc100ns);
    StoreUint64(header, 268, frame.capture.roiCopyTime100ns.value_or(noRoiCopyTime));
    StoreUint32(header, 276, frame.dpiX);
    StoreUint32(header, 280, frame.dpiY);
    StoreDouble(header, 284, frame.scaleX);
    StoreDouble(header, 292, frame.scaleY);
    const std::array<std::byte, 32> senderDigest = frame.senderCanonicalRaster ?
        pbprotocol::ComputeBlake3Digest(frame.senderCanonicalRaster->pixels) : std::array<std::byte, 32>{};
    const auto capturedDigest = pbprotocol::ComputeBlake3Digest(frame.capturedRoi.pixels);
    const std::array<std::byte, 32> bootstrapDigest = frame.canonicalBootstrap.empty() ?
        std::array<std::byte, 32>{} : pbprotocol::ComputeBlake3Digest(frame.canonicalBootstrap);
    std::copy(senderDigest.begin(), senderDigest.end(), header.begin() + 300);
    std::copy(capturedDigest.begin(), capturedDigest.end(), header.begin() + 332);
    std::copy(bootstrapDigest.begin(), bootstrapDigest.end(), header.begin() + 364);
    const auto& pointer = frame.capture.pointer;
    header[396] = static_cast<std::byte>((pointer.positionKnown ? 1u : 0u) |
        (pointer.shapeKnown ? 2u : 0u) | (pointer.separateVisible ? 4u : 0u));
    StoreInt64(header, 400, pointer.physicalLeft);
    StoreInt64(header, 408, pointer.physicalTop);
    StoreInt64(header, 416, pointer.rawUpdateTimestamp);
    StoreUint32(header, 424, pointer.shapeType);
    StoreUint32(header, 428, pointer.shapeWidth);
    StoreUint32(header, 432, pointer.shapeRawHeight);
    StoreUint32(header, 436, pointer.shapeVisibleHeight);
    StoreUint32(header, 440, pointer.shapePitch);
    StoreUint32(header, 444, pointer.shapeBytes);
    StoreInt32(header, 448, pointer.hotspotX);
    StoreInt32(header, 452, pointer.hotspotY);
    StoreUint32(header, recordHeaderCrcOffset,
        pbprotocol::ComputeCrc32c(std::span<const std::byte>(header).first(recordHeaderCrcOffset)));
    return header;
}

std::array<std::byte, kReplayV2RecordHeaderBytes> MakeDemodHeader(const ReplayV2DemodObservationView& observation,
    const std::uint32_t ordinal) noexcept
{
    std::array<std::byte, kReplayV2RecordHeaderBytes> header{};
    StoreMagic(header, recordMagic);
    StoreUint16(header, 8, kReplayV2FormatVersion);
    StoreUint16(header, 10, static_cast<std::uint16_t>(kReplayV2RecordHeaderBytes));
    StoreUint64(header, 12, kReplayV2RecordHeaderBytes + 4);
    StoreUint32(header, 20, ordinal);
    StoreUint32(header, 24, static_cast<std::uint32_t>(ReplayV2RecordType::DemodObservation));
    StoreUint64(header, 88, observation.captureObservation);
    StoreUint64(header, 96, observation.captureEpoch);
    StoreUint64(header, 120, observation.frameSequenceAvailable ? observation.frameSequence : unavailableUint64);
    StoreUint32(header, 132, observation.diagnosticCode);
    header[136] = static_cast<std::byte>(observation.disposition);
    header[137] = static_cast<std::byte>((observation.bootstrapAttempted ? demodFlagBootstrapAttempted : 0) |
        (observation.bootstrapSucceeded ? demodFlagBootstrapSucceeded : 0) |
        (observation.frameSequenceAvailable ? demodFlagFrameSequenceAvailable : 0) |
        (observation.transportProduced ? demodFlagTransportProduced : 0) |
        (observation.receiverAdmitted ? demodFlagReceiverAdmitted : 0) |
        (observation.productionDetailAvailable ? demodFlagProductionDetail : 0));
    if (observation.productionDetailAvailable)
    {
        header[138] = static_cast<std::byte>(kReplayV2DemodDetailVersion);
        header[139] = static_cast<std::byte>(observation.visualLayoutVersion);
        header[140] = static_cast<std::byte>(observation.resultKind);
        header[141] = static_cast<std::byte>(observation.geometryStatus);
        header[142] = static_cast<std::byte>(observation.temporalDisposition);
        header[143] = static_cast<std::byte>((observation.layoutAvailable ? demodDetailFlagLayoutAvailable : 0) |
            (observation.geometryAvailable ? demodDetailFlagGeometryAvailable : 0) |
            (observation.evaluationAvailable ? demodDetailFlagEvaluationAvailable : 0) |
            (observation.metricSummaryAvailable ? demodDetailFlagMetricSummaryAvailable : 0) |
            (observation.carrierAccepted ? demodDetailFlagCarrierAccepted : 0) |
            (observation.receiverStateAdvanced ? demodDetailFlagReceiverStateAdvanced : 0) |
            (observation.senderTruthAvailable ? demodDetailFlagSenderTruthAvailable : 0) |
            (observation.paddingValid ? demodDetailFlagPaddingValid : 0));
        header[144] = static_cast<std::byte>(observation.gpuTimingAvailable ? demodTimingFlagGpuTimingAvailable : 0);
        StoreDouble(header, 152, observation.geometryOriginX);
        StoreDouble(header, 160, observation.geometryOriginY);
        StoreDouble(header, 168, observation.geometryScaleX);
        StoreDouble(header, 176, observation.geometryScaleY);
        StoreUint32(header, 184, observation.codewords);
        StoreUint32(header, 188, observation.fecFailures);
        StoreUint32(header, 192, observation.crcFailures);
        StoreUint32(header, 196, observation.identityFailures);
        StoreUint32(header, 200, observation.falseAcceptedCodewords);
        StoreUint32(header, 204, observation.acceptedTransportBlocks);
        StoreUint32(header, 208, observation.acceptedRemoteControlBlocks);
        StoreUint32(header, 212, observation.admittedTransportBlocks);
        StoreUint32(header, 216, observation.admittedRemoteControlBlocks);
        StoreUint32(header, 220, observation.iterationsTotal);
        StoreUint32(header, 224, observation.iterationsMaximum);
        StoreUint64(header, 228, observation.comparedCodedBits);
        StoreUint64(header, 236, observation.erroneousCodedBits);
        StoreUint32(header, 244, observation.metricSamples);
        StoreUint32(header, 248, observation.zeroMagnitudeMetrics);
        StoreDouble(header, 252, observation.minimumAbsoluteMetric);
        StoreDouble(header, 260, observation.meanAbsoluteMetric);
        StoreUint32(header, 268, observation.freshnessRegions);
        StoreUint32(header, 272, observation.staleRegions);
        StoreUint32(header, 276, observation.freshnessTagMismatches);
        StoreUint32(header, 280, observation.freshnessTagErasures);
        StoreUint32(header, 284, observation.freshnessErasedDataMetrics);
        StoreUint32(header, 288, observation.unreliableSymbols);
        StoreUint64(header, 292, observation.metricReadbackBytes);
        StoreUint64(header, 300, observation.gpuTime100ns);
    }
    StoreUint64(header, 456, observation.visualProfileId);
    StoreUint32(header, recordHeaderCrcOffset,
        pbprotocol::ComputeCrc32c(std::span<const std::byte>(header).first(recordHeaderCrcOffset)));
    return header;
}

std::array<std::byte, kReplayV2FileFooterBytes> MakeFooter(const std::uint32_t captures,
    const std::uint32_t observations, const std::uint32_t records, const std::uint64_t bytesBeforeFooter,
    const std::uint32_t streamCrc, const std::array<std::byte, 32>& streamDigest) noexcept
{
    std::array<std::byte, kReplayV2FileFooterBytes> footer{};
    StoreMagic(footer, footerMagic);
    StoreUint16(footer, 8, kReplayV2FormatVersion);
    StoreUint16(footer, 10, static_cast<std::uint16_t>(kReplayV2FileFooterBytes));
    StoreUint32(footer, 12, captures);
    StoreUint32(footer, 16, observations);
    StoreUint32(footer, 20, records);
    StoreUint64(footer, 28, bytesBeforeFooter);
    StoreUint32(footer, 36, streamCrc);
    std::copy(streamDigest.begin(), streamDigest.end(), footer.begin() + 40);
    StoreUint32(footer, footerCrcOffset, pbprotocol::ComputeCrc32c(std::span<const std::byte>(footer).first(footerCrcOffset)));
    return footer;
}

ReplayStatus ValidateFixedHeader(const std::span<const std::byte> header) noexcept
{
    if (!HasMagic(header, fileMagic) || LoadUint16(header, 10) != kReplayV2FileHeaderBytes ||
        LoadUint32(header, 12) != endianSentinel || !IsZero(header.subspan(20, 4)) ||
        !IsZero(header.subspan(136, fileHeaderCrcOffset - 136)))
    {
        return ReplayStatus::Failure(ReplayError::MalformedHeader, ReplayStage::Header);
    }
    if (LoadUint16(header, 8) != kReplayV2FormatVersion)
    {
        return ReplayStatus::Failure(ReplayError::UnsupportedVersion, ReplayStage::Header);
    }
    if (LoadUint32(header, fileHeaderCrcOffset) != pbprotocol::ComputeCrc32c(header.first(fileHeaderCrcOffset)))
    {
        return ReplayStatus::Failure(ReplayError::ChecksumMismatch, ReplayStage::Header);
    }
    return {};
}

ReplayStatus ValidateRecordFixedHeader(const std::span<const std::byte> header, const std::uint32_t expectedOrdinal) noexcept
{
    if (!HasMagic(header, recordMagic) || LoadUint16(header, 10) != kReplayV2RecordHeaderBytes ||
        LoadUint32(header, 20) != expectedOrdinal || LoadUint32(header, recordHeaderCrcOffset) !=
        pbprotocol::ComputeCrc32c(header.first(recordHeaderCrcOffset)))
    {
        return ReplayStatus::Failure(ReplayError::MalformedFrame, ReplayStage::RecordHeader);
    }
    if (LoadUint16(header, 8) != kReplayV2FormatVersion)
    {
        return ReplayStatus::Failure(ReplayError::UnsupportedVersion, ReplayStage::RecordHeader);
    }
    return {};
}

ReplayStatus ValidateFooter(const std::span<const std::byte> footer, const std::uint64_t fileBytes,
    const ReplayV2Limits& limits, std::uint32_t& captures, std::uint32_t& observations, std::uint32_t& records,
    std::uint64_t& bytesBeforeFooter, std::uint32_t& streamCrc, std::array<std::byte, 32>& streamDigest) noexcept
{
    if (!HasMagic(footer, footerMagic) || LoadUint16(footer, 10) != kReplayV2FileFooterBytes ||
        !IsZero(footer.subspan(24, 4)) || !IsZero(footer.subspan(72, footerCrcOffset - 72)))
    {
        return ReplayStatus::Failure(ReplayError::MalformedHeader, ReplayStage::Footer);
    }
    if (LoadUint16(footer, 8) != kReplayV2FormatVersion)
    {
        return ReplayStatus::Failure(ReplayError::UnsupportedVersion, ReplayStage::Footer);
    }
    if (LoadUint32(footer, footerCrcOffset) != pbprotocol::ComputeCrc32c(footer.first(footerCrcOffset)))
    {
        return ReplayStatus::Failure(ReplayError::ChecksumMismatch, ReplayStage::Footer);
    }
    captures = LoadUint32(footer, 12);
    observations = LoadUint32(footer, 16);
    records = LoadUint32(footer, 20);
    bytesBeforeFooter = LoadUint64(footer, 28);
    streamCrc = LoadUint32(footer, 36);
    std::copy(footer.begin() + 40, footer.begin() + 72, streamDigest.begin());
    const auto expectedRecords = pbprotocol::CheckedAddUint64(captures, observations);
    std::uint64_t expectedFileBytes = 0;
    if (!expectedRecords || expectedRecords.Value() != records || !AddWithinLimit(bytesBeforeFooter,
        kReplayV2FileFooterBytes, limits.maximumFileBytes, expectedFileBytes, ReplayStage::Footer) ||
        expectedFileBytes != fileBytes)
    {
        return ReplayStatus::Failure(ReplayError::FrameCountMismatch, ReplayStage::Footer);
    }
    if (captures > limits.maximumCaptureFrames || observations > limits.maximumCaptureFrames)
    {
        return ReplayStatus::Failure(ReplayError::ResourceLimit, ReplayStage::Footer);
    }
    return {};
}

ReplayStatus ValidateWholeStream(const HANDLE file, const std::uint64_t bytesBeforeFooter,
    const std::uint32_t expectedCrc, const std::array<std::byte, 32>& expectedDigest) noexcept
{
    std::array<std::byte, 64 * 1024> buffer{};
    pbprotocol::Crc32c crc;
    pbprotocol::Blake3Hasher digest;
    std::uint64_t offset = 0;
    while (offset < bytesBeforeFooter)
    {
        const auto remaining = bytesBeforeFooter - offset;
        const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
        const auto status = ReadExactAt(file, offset, std::span<std::byte>(buffer).first(chunk), ReplayStage::Checksum);
        if (!status)
        {
            return status;
        }
        const auto bytes = std::span<const std::byte>(buffer).first(chunk);
        crc.Update(bytes);
        digest.Update(bytes);
        offset += chunk;
    }
    if (crc.Finalize() != expectedCrc)
    {
        return ReplayStatus::Failure(ReplayError::ChecksumMismatch, ReplayStage::Checksum);
    }
    return digest.Finalize() == expectedDigest ? ReplayStatus{} :
        ReplayStatus::Failure(ReplayError::DigestMismatch, ReplayStage::Checksum);
}

} // namespace

struct ReplayV2Writer::Implementation
{
    Implementation() = default;
    Implementation(const Implementation&) = delete;
    Implementation& operator=(const Implementation&) = delete;
    Implementation(Implementation&&) = delete;
    Implementation& operator=(Implementation&&) = delete;

    ~Implementation()
    {
        if (file != INVALID_HANDLE_VALUE)
        {
            CloseHandle(file);
        }
        if (!complete && !partialPath.empty())
        {
            DeleteFileW(partialPath.c_str());
        }
    }

    HANDLE file = INVALID_HANDLE_VALUE;
    std::filesystem::path targetPath;
    std::filesystem::path partialPath;
    ReplayV2FileDescriptor descriptor;
    ReplayV2Limits limits;
    pbprotocol::Crc32c streamCrc;
    pbprotocol::Blake3Hasher streamDigest;
    std::vector<CaptureIdentity> captures;
    std::uint64_t fileBytes = 0;
    std::uint64_t totalRasterBytes = 0;
    std::uint32_t captureFrames = 0;
    std::uint32_t demodObservations = 0;
    std::uint32_t records = 0;
    bool failed = false;
    bool complete = false;
};

struct ReplayV2Reader::Implementation
{
    Implementation() = default;
    Implementation(const Implementation&) = delete;
    Implementation& operator=(const Implementation&) = delete;
    Implementation(Implementation&&) = delete;
    Implementation& operator=(Implementation&&) = delete;

    ~Implementation()
    {
        if (file != INVALID_HANDLE_VALUE)
        {
            CloseHandle(file);
        }
    }

    HANDLE file = INVALID_HANDLE_VALUE;
    ReplayV2FileDescriptor descriptor;
    ReplayV2Limits limits;
    std::vector<CaptureIdentity> captures;
    std::uint64_t fileBytes = 0;
    std::uint64_t bytesBeforeFooter = 0;
    std::uint64_t currentOffset = 0;
    std::uint64_t totalRasterBytes = 0;
    std::uint32_t expectedCaptureFrames = 0;
    std::uint32_t expectedDemodObservations = 0;
    std::uint32_t expectedRecords = 0;
    std::uint32_t captureFrames = 0;
    std::uint32_t demodObservations = 0;
    std::uint32_t records = 0;
    bool failed = false;
};

namespace
{

ReplayStatus FailWriter(ReplayV2Writer::Implementation& implementation, const ReplayStatus& status) noexcept
{
    implementation.failed = true;
    return status;
}

ReplayStatus FailReader(ReplayV2Reader::Implementation& implementation, const ReplayStatus& status) noexcept
{
    implementation.failed = true;
    return status;
}

ReplayStatus WriteTracked(ReplayV2Writer::Implementation& implementation, const std::span<const std::byte> bytes,
    const ReplayStage stage, const bool includeInStream = true) noexcept
{
    std::uint64_t nextFileBytes = 0;
    const auto sizeStatus = AddWithinLimit(implementation.fileBytes, bytes.size(), implementation.limits.maximumFileBytes,
        nextFileBytes, stage, implementation.fileBytes);
    if (!sizeStatus)
    {
        return FailWriter(implementation, sizeStatus);
    }
    std::size_t completed = 0;
    while (completed < bytes.size())
    {
        const auto remaining = bytes.size() - completed;
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(remaining, 1u << 30));
        DWORD bytesWritten = 0;
        if (!WriteFile(implementation.file, bytes.data() + completed, request, &bytesWritten, nullptr) || bytesWritten == 0)
        {
            return FailWriter(implementation, ReplayStatus::Failure(ReplayError::IoFailure, stage,
                static_cast<std::int32_t>(GetLastError()), implementation.fileBytes + completed));
        }
        if (includeInStream)
        {
            const auto chunk = bytes.subspan(completed, bytesWritten);
            implementation.streamCrc.Update(chunk);
            implementation.streamDigest.Update(chunk);
        }
        completed += bytesWritten;
    }
    implementation.fileBytes = nextFileBytes;
    return {};
}

ReplayStatus WriteRecord(ReplayV2Writer::Implementation& implementation,
    const std::array<std::byte, kReplayV2RecordHeaderBytes>& header,
    const std::span<const std::byte> displayIdentity, const std::span<const std::byte> bootstrap,
    const std::span<const std::byte> senderRaster, const std::span<const std::byte> capturedRaster) noexcept
{
    pbprotocol::Crc32c recordCrc;
    for (const auto bytes : {std::span<const std::byte>(header), displayIdentity, bootstrap, senderRaster, capturedRaster})
    {
        recordCrc.Update(bytes);
    }
    std::array<std::byte, 4> trailer{};
    StoreUint32(trailer, 0, recordCrc.Finalize());
    for (const auto [bytes, stage] : std::array<std::pair<std::span<const std::byte>, ReplayStage>, 6>{
        std::pair<std::span<const std::byte>, ReplayStage>{header, ReplayStage::RecordHeader},
        {displayIdentity, ReplayStage::RecordPayload}, {bootstrap, ReplayStage::RecordPayload},
        {senderRaster, ReplayStage::RecordPayload}, {capturedRaster, ReplayStage::RecordPayload},
        {trailer, ReplayStage::RecordPayload}})
    {
        const auto status = WriteTracked(implementation, bytes, stage);
        if (!status)
        {
            return status;
        }
    }
    return {};
}

} // namespace

ReplayV2Writer::ReplayV2Writer() noexcept = default;
ReplayV2Writer::ReplayV2Writer(std::unique_ptr<Implementation> implementation) noexcept : implementation_(std::move(implementation)) {}
ReplayV2Writer::ReplayV2Writer(ReplayV2Writer&&) noexcept = default;
ReplayV2Writer& ReplayV2Writer::operator=(ReplayV2Writer&&) noexcept = default;
ReplayV2Writer::~ReplayV2Writer() = default;

ReplayStatus ReplayV2Writer::Create(const std::filesystem::path& targetPath, const ReplayV2FileDescriptor& descriptor,
    const ReplayV2Limits& limits, std::unique_ptr<ReplayV2Writer>& output) noexcept
{
    const auto limitsStatus = ValidateLimits(limits);
    if (!limitsStatus)
    {
        return limitsStatus;
    }
    const auto descriptorStatus = ValidateDescriptor(descriptor, limits);
    if (!descriptorStatus)
    {
        return descriptorStatus;
    }
    if (targetPath.empty())
    {
        return ReplayStatus::Failure(ReplayError::InvalidArgument, ReplayStage::Configuration);
    }
    try
    {
        const DWORD finalAttributes = GetFileAttributesW(targetPath.c_str());
        if (finalAttributes != INVALID_FILE_ATTRIBUTES)
        {
            return ReplayStatus::Failure(ReplayError::AlreadyExists, ReplayStage::Open);
        }
        const DWORD attributesError = GetLastError();
        if (attributesError != ERROR_FILE_NOT_FOUND && attributesError != ERROR_PATH_NOT_FOUND)
        {
            return ReplayStatus::Failure(ReplayError::IoFailure, ReplayStage::Open, static_cast<std::int32_t>(attributesError));
        }
        const auto metadataBytes = std::as_bytes(std::span(descriptor.remoteMetadataJsonUtf8.data(),
            descriptor.remoteMetadataJsonUtf8.size()));
        std::uint64_t recordsOffset = 0;
        if (!AddWithinLimit(kReplayV2FileHeaderBytes, metadataBytes.size(), limits.maximumFileBytes,
            recordsOffset, ReplayStage::Metadata))
        {
            return ReplayStatus::Failure(ReplayError::ResourceLimit, ReplayStage::Metadata);
        }
        std::uint64_t minimumPublishedBytes = 0;
        if (!AddWithinLimit(recordsOffset, kReplayV2FileFooterBytes, limits.maximumFileBytes,
            minimumPublishedBytes, ReplayStage::Metadata))
        {
            return ReplayStatus::Failure(ReplayError::ResourceLimit, ReplayStage::Metadata);
        }
        auto implementation = std::make_unique<Implementation>();
        implementation->targetPath = targetPath;
        implementation->partialPath = targetPath;
        implementation->partialPath += L".partial";
        implementation->descriptor = descriptor;
        implementation->limits = limits;
        implementation->captures.reserve(limits.maximumCaptureFrames);
        implementation->file = CreateFileW(implementation->partialPath.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (implementation->file == INVALID_HANDLE_VALUE)
        {
            const DWORD error = GetLastError();
            return ReplayStatus::Failure(error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS ?
                ReplayError::AlreadyExists : ReplayError::IoFailure, ReplayStage::Open, static_cast<std::int32_t>(error));
        }
        const auto header = MakeFileHeader(descriptor, recordsOffset, pbprotocol::ComputeBlake3Digest(metadataBytes));
        auto status = WriteTracked(*implementation, header, ReplayStage::Header);
        if (status)
        {
            status = WriteTracked(*implementation, metadataBytes, ReplayStage::Metadata);
        }
        if (!status)
        {
            return status;
        }
        output = std::unique_ptr<ReplayV2Writer>(new ReplayV2Writer(std::move(implementation)));
        return {};
    }
    catch (const std::bad_alloc&)
    {
        return ReplayStatus::Failure(ReplayError::OutOfMemory, ReplayStage::Open);
    }
    catch (const std::length_error&)
    {
        return ReplayStatus::Failure(ReplayError::ResourceLimit, ReplayStage::Open);
    }
    catch (const std::filesystem::filesystem_error& error)
    {
        return ReplayStatus::Failure(ReplayError::IoFailure, ReplayStage::Open, error.code().value());
    }
}

ReplayStatus ReplayV2Writer::AppendCapture(const ReplayV2CaptureView& capture) noexcept
{
    if (!implementation_ || implementation_->failed || implementation_->complete || implementation_->file == INVALID_HANDLE_VALUE)
    {
        return ReplayStatus::Failure(ReplayError::InvalidState, ReplayStage::RecordHeader);
    }
    auto& implementation = *implementation_;
    if (implementation.captureFrames >= implementation.limits.maximumCaptureFrames)
    {
        return ReplayStatus::Failure(ReplayError::ResourceLimit, ReplayStage::RecordHeader, 0, implementation.fileBytes);
    }
    const auto validationStatus = ValidateCapture(capture, implementation.limits);
    if (!validationStatus)
    {
        return validationStatus;
    }
    if (FindCapture(implementation.captures, capture.capture.domain.captureEpoch, capture.capture.captureObservation))
    {
        return ReplayStatus::Failure(ReplayError::InvalidArgument, ReplayStage::RecordHeader, 0, implementation.fileBytes);
    }
    const auto displayBytes = std::as_bytes(std::span(capture.displayIdentityUtf8.data(), capture.displayIdentityUtf8.size()));
    const auto senderBytes = capture.senderCanonicalRaster ? capture.senderCanonicalRaster->pixels : std::span<const std::byte>{};
    std::uint64_t recordBytes = kReplayV2RecordHeaderBytes + 4;
    for (const auto size : {displayBytes.size(), capture.canonicalBootstrap.size(), senderBytes.size(), capture.capturedRoi.pixels.size()})
    {
        const auto result = pbprotocol::CheckedAddUint64(recordBytes, size);
        if (!result)
        {
            return ReplayStatus::Failure(ReplayError::ResourceLimit, ReplayStage::RecordPayload, 0, implementation.fileBytes);
        }
        recordBytes = result.Value();
    }
    std::uint64_t projectedFileBytes = 0;
    std::uint64_t recordAndFooter = 0;
    if (!AddWithinLimit(recordBytes, kReplayV2FileFooterBytes, implementation.limits.maximumFileBytes,
        recordAndFooter, ReplayStage::RecordPayload) || !AddWithinLimit(implementation.fileBytes, recordAndFooter,
        implementation.limits.maximumFileBytes, projectedFileBytes, ReplayStage::RecordPayload, implementation.fileBytes))
    {
        return ReplayStatus::Failure(ReplayError::ResourceLimit, ReplayStage::RecordPayload, 0, implementation.fileBytes);
    }
    const auto senderSize = capture.senderCanonicalRaster ? capture.senderCanonicalRaster->pixels.size() : 0;
    const auto rasterBytes = pbprotocol::CheckedAddUint64(senderSize, capture.capturedRoi.pixels.size());
    std::uint64_t nextRasterBytes = 0;
    if (!rasterBytes || !AddWithinLimit(implementation.totalRasterBytes, rasterBytes.Value(),
        implementation.limits.maximumTotalRasterBytes, nextRasterBytes, ReplayStage::RecordPayload, implementation.fileBytes))
    {
        return ReplayStatus::Failure(ReplayError::ResourceLimit, ReplayStage::RecordPayload, 0, implementation.fileBytes);
    }
    const auto header = MakeCaptureHeader(capture, implementation.records, recordBytes);
    const auto status = WriteRecord(implementation, header, displayBytes, capture.canonicalBootstrap, senderBytes,
        capture.capturedRoi.pixels);
    if (!status)
    {
        return status;
    }
    try
    {
        implementation.captures.push_back({capture.capture.domain.captureEpoch, capture.capture.captureObservation, false});
    }
    catch (const std::bad_alloc&)
    {
        return FailWriter(implementation, ReplayStatus::Failure(ReplayError::OutOfMemory, ReplayStage::RecordHeader,
            0, implementation.fileBytes));
    }
    implementation.totalRasterBytes = nextRasterBytes;
    implementation.captureFrames++;
    implementation.records++;
    return {};
}

ReplayStatus ReplayV2Writer::AppendDemodObservation(const ReplayV2DemodObservationView& observation) noexcept
{
    if (!implementation_ || implementation_->failed || implementation_->complete || implementation_->file == INVALID_HANDLE_VALUE)
    {
        return ReplayStatus::Failure(ReplayError::InvalidState, ReplayStage::RecordHeader);
    }
    auto& implementation = *implementation_;
    const auto validationStatus = ValidateDemodObservation(observation);
    if (!validationStatus)
    {
        return validationStatus;
    }
    if (observation.visualProfileId != implementation.descriptor.visualProfileId)
    {
        return ReplayStatus::Failure(ReplayError::InvalidArgument, ReplayStage::RecordHeader, 0, implementation.fileBytes);
    }
    CaptureIdentity* const capture = FindCapture(implementation.captures, observation.captureEpoch, observation.captureObservation);
    if (!capture || capture->hasDemodObservation)
    {
        return ReplayStatus::Failure(ReplayError::InvalidArgument, ReplayStage::RecordHeader, 0, implementation.fileBytes);
    }
    std::uint64_t projectedFileBytes = 0;
    const std::uint64_t recordBytes = kReplayV2RecordHeaderBytes + 4;
    if (!AddWithinLimit(implementation.fileBytes, recordBytes + kReplayV2FileFooterBytes,
        implementation.limits.maximumFileBytes, projectedFileBytes, ReplayStage::RecordPayload, implementation.fileBytes))
    {
        return ReplayStatus::Failure(ReplayError::ResourceLimit, ReplayStage::RecordPayload, 0, implementation.fileBytes);
    }
    const auto header = MakeDemodHeader(observation, implementation.records);
    const auto status = WriteRecord(implementation, header, {}, {}, {}, {});
    if (!status)
    {
        return status;
    }
    capture->hasDemodObservation = true;
    implementation.demodObservations++;
    implementation.records++;
    return {};
}

ReplayStatus ReplayV2Writer::Finalize() noexcept
{
    if (!implementation_ || implementation_->failed || implementation_->complete || implementation_->file == INVALID_HANDLE_VALUE)
    {
        return ReplayStatus::Failure(ReplayError::InvalidState, ReplayStage::Footer);
    }
    auto& implementation = *implementation_;
    if (implementation.captureFrames == 0)
    {
        return ReplayStatus::Failure(ReplayError::FrameCountMismatch, ReplayStage::Footer, 0, implementation.fileBytes);
    }
    const auto footer = MakeFooter(implementation.captureFrames, implementation.demodObservations,
        implementation.records, implementation.fileBytes, implementation.streamCrc.Finalize(), implementation.streamDigest.Finalize());
    const auto footerStatus = WriteTracked(implementation, footer, ReplayStage::Footer, false);
    if (!footerStatus)
    {
        return footerStatus;
    }
    if (!FlushFileBuffers(implementation.file))
    {
        return FailWriter(implementation, ReplayStatus::Failure(ReplayError::IoFailure, ReplayStage::Flush,
            static_cast<std::int32_t>(GetLastError()), implementation.fileBytes));
    }
    const HANDLE file = std::exchange(implementation.file, INVALID_HANDLE_VALUE);
    if (!CloseHandle(file))
    {
        return FailWriter(implementation, ReplayStatus::Failure(ReplayError::IoFailure, ReplayStage::Flush,
            static_cast<std::int32_t>(GetLastError()), implementation.fileBytes));
    }
    if (!MoveFileExW(implementation.partialPath.c_str(), implementation.targetPath.c_str(), MOVEFILE_WRITE_THROUGH))
    {
        const DWORD error = GetLastError();
        return FailWriter(implementation, ReplayStatus::Failure(error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS ?
            ReplayError::AlreadyExists : ReplayError::IoFailure, ReplayStage::Publish, static_cast<std::int32_t>(error), implementation.fileBytes));
    }
    implementation.complete = true;
    implementation.partialPath.clear();
    return {};
}

ReplayV2FileSnapshot ReplayV2Writer::GetSnapshot() const noexcept
{
    if (!implementation_)
    {
        return {};
    }
    return {implementation_->descriptor, implementation_->fileBytes, implementation_->totalRasterBytes,
        implementation_->captureFrames, implementation_->demodObservations, implementation_->records, implementation_->complete};
}

ReplayV2Reader::ReplayV2Reader() noexcept = default;
ReplayV2Reader::ReplayV2Reader(std::unique_ptr<Implementation> implementation) noexcept : implementation_(std::move(implementation)) {}
ReplayV2Reader::ReplayV2Reader(ReplayV2Reader&&) noexcept = default;
ReplayV2Reader& ReplayV2Reader::operator=(ReplayV2Reader&&) noexcept = default;
ReplayV2Reader::~ReplayV2Reader() = default;

ReplayStatus ReplayV2Reader::Open(const std::filesystem::path& path, const ReplayV2Limits& limits,
    std::unique_ptr<ReplayV2Reader>& output) noexcept
{
    const auto limitsStatus = ValidateLimits(limits);
    if (!limitsStatus)
    {
        return limitsStatus;
    }
    if (path.empty())
    {
        return ReplayStatus::Failure(ReplayError::InvalidArgument, ReplayStage::Configuration);
    }
    try
    {
        auto implementation = std::make_unique<Implementation>();
        implementation->limits = limits;
        implementation->captures.reserve(limits.maximumCaptureFrames);
        implementation->file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (implementation->file == INVALID_HANDLE_VALUE)
        {
            const DWORD error = GetLastError();
            return ReplayStatus::Failure(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ?
                ReplayError::NotFound : ReplayError::IoFailure, ReplayStage::Open, static_cast<std::int32_t>(error));
        }
        LARGE_INTEGER fileSize{};
        if (!GetFileSizeEx(implementation->file, &fileSize))
        {
            return ReplayStatus::Failure(ReplayError::IoFailure, ReplayStage::Open, static_cast<std::int32_t>(GetLastError()));
        }
        if (fileSize.QuadPart < static_cast<LONGLONG>(kReplayV2FileHeaderBytes + kReplayV2FileFooterBytes))
        {
            return ReplayStatus::Failure(ReplayError::TruncatedInput, ReplayStage::Open);
        }
        if (static_cast<std::uint64_t>(fileSize.QuadPart) > limits.maximumFileBytes)
        {
            return ReplayStatus::Failure(ReplayError::ResourceLimit, ReplayStage::Open);
        }
        implementation->fileBytes = static_cast<std::uint64_t>(fileSize.QuadPart);
        std::array<std::byte, kReplayV2FileHeaderBytes> header{};
        auto status = ReadExactAt(implementation->file, 0, header, ReplayStage::Header);
        if (!status)
        {
            return status;
        }
        status = ValidateFixedHeader(header);
        if (!status)
        {
            return status;
        }
        const std::uint32_t metadataSize = LoadUint32(header, 80);
        const std::uint64_t recordsOffset = LoadUint64(header, 88);
        std::uint64_t expectedRecordsOffset = 0;
        if (metadataSize == 0 || metadataSize > limits.maximumMetadataBytes ||
            !AddWithinLimit(kReplayV2FileHeaderBytes, metadataSize, limits.maximumFileBytes,
            expectedRecordsOffset, ReplayStage::Metadata) || recordsOffset != expectedRecordsOffset ||
            recordsOffset > implementation->fileBytes - kReplayV2FileFooterBytes)
        {
            return ReplayStatus::Failure(ReplayError::MalformedHeader, ReplayStage::Metadata);
        }
        std::string metadata(metadataSize, '\0');
        status = ReadExactAt(implementation->file, kReplayV2FileHeaderBytes,
            std::as_writable_bytes(std::span(metadata.data(), metadata.size())), ReplayStage::Metadata);
        if (!status)
        {
            return status;
        }
        if (!ValidUtf8(metadata, false))
        {
            return ReplayStatus::Failure(ReplayError::MalformedHeader, ReplayStage::Metadata);
        }
        if (pbprotocol::ComputeBlake3Digest(std::as_bytes(std::span(metadata.data(), metadata.size()))) !=
            LoadDigest(header, 96))
        {
            return ReplayStatus::Failure(ReplayError::DigestMismatch, ReplayStage::Metadata);
        }
        ReplayV2FileDescriptor descriptor;
        descriptor.datasetClass = static_cast<ReplayDatasetClass>(LoadUint32(header, 16));
        descriptor.visualProfileId = LoadUint64(header, 128);
        std::copy(header.begin() + 24, header.begin() + 40, descriptor.datasetId.begin());
        descriptor.runId.assign(reinterpret_cast<const char*>(header.data() + 40), 32);
        descriptor.createdUtc100ns = LoadInt64(header, 72);
        descriptor.remoteMetadataJsonUtf8 = std::move(metadata);
        const auto descriptorStatus = ValidateDescriptor(descriptor, limits);
        if (!descriptorStatus)
        {
            return ReplayStatus::Failure(ReplayError::MalformedHeader, ReplayStage::Header);
        }
        std::array<std::byte, kReplayV2FileFooterBytes> footer{};
        status = ReadExactAt(implementation->file, implementation->fileBytes - kReplayV2FileFooterBytes,
            footer, ReplayStage::Footer);
        if (!status)
        {
            return status;
        }
        std::uint32_t streamCrc = 0;
        std::array<std::byte, 32> streamDigest{};
        status = ValidateFooter(footer, implementation->fileBytes, limits, implementation->expectedCaptureFrames,
            implementation->expectedDemodObservations, implementation->expectedRecords,
            implementation->bytesBeforeFooter, streamCrc, streamDigest);
        if (!status)
        {
            return status;
        }
        if (implementation->bytesBeforeFooter < recordsOffset)
        {
            return ReplayStatus::Failure(ReplayError::TruncatedInput, ReplayStage::Footer);
        }
        status = ValidateWholeStream(implementation->file, implementation->bytesBeforeFooter, streamCrc, streamDigest);
        if (!status)
        {
            return status;
        }
        implementation->descriptor = std::move(descriptor);
        implementation->currentOffset = recordsOffset;
        output = std::unique_ptr<ReplayV2Reader>(new ReplayV2Reader(std::move(implementation)));
        return {};
    }
    catch (const std::bad_alloc&)
    {
        return ReplayStatus::Failure(ReplayError::OutOfMemory, ReplayStage::Open);
    }
    catch (const std::length_error&)
    {
        return ReplayStatus::Failure(ReplayError::ResourceLimit, ReplayStage::Open);
    }
}

ReplayStatus ReplayV2Reader::ReadNext(ReplayV2Record& output) noexcept
{
    if (!implementation_ || implementation_->failed || implementation_->file == INVALID_HANDLE_VALUE)
    {
        return ReplayStatus::Failure(ReplayError::InvalidState, ReplayStage::Read);
    }
    auto& implementation = *implementation_;
    if (implementation.records == implementation.expectedRecords)
    {
        return implementation.currentOffset == implementation.bytesBeforeFooter ?
            ReplayStatus::Failure(ReplayError::EndOfFile, ReplayStage::Read, 0, implementation.currentOffset) :
            FailReader(implementation, ReplayStatus::Failure(ReplayError::FrameCountMismatch, ReplayStage::Read, 0,
                implementation.currentOffset));
    }
    if (implementation.currentOffset > implementation.bytesBeforeFooter ||
        implementation.bytesBeforeFooter - implementation.currentOffset < kReplayV2RecordHeaderBytes + 4)
    {
        return FailReader(implementation, ReplayStatus::Failure(ReplayError::TruncatedInput, ReplayStage::RecordHeader,
            0, implementation.currentOffset));
    }
    std::array<std::byte, kReplayV2RecordHeaderBytes> header{};
    auto status = ReadExactAt(implementation.file, implementation.currentOffset, header, ReplayStage::RecordHeader);
    if (!status)
    {
        return FailReader(implementation, status);
    }
    status = ValidateRecordFixedHeader(header, implementation.records);
    if (!status)
    {
        return FailReader(implementation, status);
    }
    const std::uint64_t recordBytes = LoadUint64(header, 12);
    std::uint64_t nextOffset = 0;
    if (recordBytes < kReplayV2RecordHeaderBytes + 4 || !AddWithinLimit(implementation.currentOffset, recordBytes,
        implementation.bytesBeforeFooter, nextOffset, ReplayStage::RecordPayload, implementation.currentOffset))
    {
        return FailReader(implementation, ReplayStatus::Failure(ReplayError::TruncatedInput, ReplayStage::RecordPayload,
            0, implementation.currentOffset));
    }
    const auto type = static_cast<ReplayV2RecordType>(LoadUint32(header, 24));
    ReplayV2Record temporary;
    temporary.type = type;
    temporary.ordinal = implementation.records;
    try
    {
        if (type == ReplayV2RecordType::Capture)
        {
            const std::uint32_t presenceFlags = LoadUint32(header, 28);
            const std::uint32_t bootstrapSize = LoadUint32(header, 32);
            const std::uint32_t displaySize = LoadUint32(header, 36);
            const std::uint64_t capturedSize = LoadUint64(header, 56);
            const std::uint64_t senderSize = LoadUint64(header, 80);
            const auto headerBytes = std::span<const std::byte>(header);
            if ((presenceFlags & ~(captureFlagSenderRaster | captureFlagCanonicalBootstrap)) != 0 ||
                displaySize == 0 || displaySize > implementation.limits.maximumDisplayIdentityBytes ||
                bootstrapSize > implementation.limits.maximumCanonicalBootstrapBytes ||
                ((presenceFlags & captureFlagCanonicalBootstrap) == 0) != (bootstrapSize == 0) ||
                ((presenceFlags & captureFlagSenderRaster) == 0) != (senderSize == 0) ||
                ((presenceFlags & captureFlagSenderRaster) == 0 && !IsZero(headerBytes.subspan(64, 24))))
            {
                return FailReader(implementation, ReplayStatus::Failure(ReplayError::MalformedFrame,
                    ReplayStage::RecordHeader, 0, implementation.currentOffset));
            }
            std::uint64_t expectedRecordBytes = kReplayV2RecordHeaderBytes + 4;
            for (const auto size : {static_cast<std::uint64_t>(displaySize), static_cast<std::uint64_t>(bootstrapSize), senderSize, capturedSize})
            {
                const auto addResult = pbprotocol::CheckedAddUint64(expectedRecordBytes, size);
                if (!addResult)
                {
                    return FailReader(implementation, ReplayStatus::Failure(ReplayError::ResourceLimit,
                        ReplayStage::RecordPayload, 0, implementation.currentOffset));
                }
                expectedRecordBytes = addResult.Value();
            }
            if (expectedRecordBytes != recordBytes)
            {
                return FailReader(implementation, ReplayStatus::Failure(ReplayError::MalformedFrame,
                    ReplayStage::RecordPayload, 0, implementation.currentOffset));
            }
            if (capturedSize > implementation.limits.maximumRasterBytesPerFrame ||
                senderSize > implementation.limits.maximumRasterBytesPerFrame)
            {
                return FailReader(implementation, ReplayStatus::Failure(ReplayError::ResourceLimit,
                    ReplayStage::RecordPayload, 0, implementation.currentOffset));
            }
            temporary.capture.displayIdentityUtf8.resize(displaySize);
            temporary.capture.canonicalBootstrap.resize(bootstrapSize);
            temporary.capture.capturedRoi.width = LoadUint32(header, 40);
            temporary.capture.capturedRoi.height = LoadUint32(header, 44);
            temporary.capture.capturedRoi.rowPitch = LoadUint32(header, 48);
            temporary.capture.capturedRoi.pixelFormat = static_cast<DXGI_FORMAT>(LoadUint32(header, 52));
            temporary.capture.capturedRoi.pixels.resize(static_cast<std::size_t>(capturedSize));
            if (presenceFlags & captureFlagSenderRaster)
            {
                temporary.capture.senderCanonicalRaster.emplace();
                temporary.capture.senderCanonicalRaster->width = LoadUint32(header, 64);
                temporary.capture.senderCanonicalRaster->height = LoadUint32(header, 68);
                temporary.capture.senderCanonicalRaster->rowPitch = LoadUint32(header, 72);
                temporary.capture.senderCanonicalRaster->pixelFormat = static_cast<DXGI_FORMAT>(LoadUint32(header, 76));
                temporary.capture.senderCanonicalRaster->pixels.resize(static_cast<std::size_t>(senderSize));
            }
            std::uint64_t payloadOffset = implementation.currentOffset + kReplayV2RecordHeaderBytes;
            auto ReadPayload = [&](const std::span<std::byte> bytes) -> ReplayStatus
            {
                const auto readStatus = ReadExactAt(implementation.file, payloadOffset, bytes, ReplayStage::RecordPayload);
                payloadOffset += bytes.size();
                return readStatus;
            };
            status = ReadPayload(std::as_writable_bytes(std::span(temporary.capture.displayIdentityUtf8.data(),
                temporary.capture.displayIdentityUtf8.size())));
            if (status)
            {
                status = ReadPayload(temporary.capture.canonicalBootstrap);
            }
            if (status && temporary.capture.senderCanonicalRaster)
            {
                status = ReadPayload(temporary.capture.senderCanonicalRaster->pixels);
            }
            if (status)
            {
                status = ReadPayload(temporary.capture.capturedRoi.pixels);
            }
            if (!status)
            {
                return FailReader(implementation, status);
            }
            std::array<std::byte, 4> trailer{};
            status = ReadPayload(trailer);
            if (!status)
            {
                return FailReader(implementation, status);
            }
            pbprotocol::Crc32c recordCrc;
            recordCrc.Update(header);
            recordCrc.Update(std::as_bytes(std::span(temporary.capture.displayIdentityUtf8.data(), temporary.capture.displayIdentityUtf8.size())));
            recordCrc.Update(temporary.capture.canonicalBootstrap);
            if (temporary.capture.senderCanonicalRaster)
            {
                recordCrc.Update(temporary.capture.senderCanonicalRaster->pixels);
            }
            recordCrc.Update(temporary.capture.capturedRoi.pixels);
            if (recordCrc.Finalize() != LoadUint32(trailer, 0))
            {
                return FailReader(implementation, ReplayStatus::Failure(ReplayError::ChecksumMismatch,
                    ReplayStage::RecordPayload, 0, implementation.currentOffset));
            }
            const auto storedSenderDigest = LoadDigest(header, 300);
            const auto storedCapturedDigest = LoadDigest(header, 332);
            const auto storedBootstrapDigest = LoadDigest(header, 364);
            const std::array<std::byte, 32> senderDigest = temporary.capture.senderCanonicalRaster ?
                pbprotocol::ComputeBlake3Digest(temporary.capture.senderCanonicalRaster->pixels) : std::array<std::byte, 32>{};
            const std::array<std::byte, 32> bootstrapDigest = temporary.capture.canonicalBootstrap.empty() ?
                std::array<std::byte, 32>{} : pbprotocol::ComputeBlake3Digest(temporary.capture.canonicalBootstrap);
            if (storedSenderDigest != senderDigest || storedCapturedDigest !=
                pbprotocol::ComputeBlake3Digest(temporary.capture.capturedRoi.pixels) || storedBootstrapDigest != bootstrapDigest)
            {
                return FailReader(implementation, ReplayStatus::Failure(ReplayError::DigestMismatch,
                    ReplayStage::RecordPayload, 0, implementation.currentOffset));
            }
            auto& capture = temporary.capture.capture;
            capture.captureObservation = LoadUint64(header, 88);
            capture.domain.captureEpoch = LoadUint64(header, 96);
            std::copy(header.begin() + 104, header.begin() + 120, capture.domain.sourceId.begin());
            capture.backend = static_cast<pbcapturenormalize::CaptureBackendKind>(std::to_integer<std::uint8_t>(header[138]));
            capture.timestamp.domain = static_cast<pbcapturenormalize::CaptureTimestampDomain>(std::to_integer<std::uint8_t>(header[139]));
            capture.signalEncoding = static_cast<pbcapturenormalize::CaptureSignalEncoding>(std::to_integer<std::uint8_t>(header[140]));
            const std::uint8_t captureFlags = std::to_integer<std::uint8_t>(header[141]);
            if ((captureFlags & ~(captureFlagHdr | captureFlagCursorExcluded)) != 0)
            {
                return FailReader(implementation, ReplayStatus::Failure(ReplayError::MalformedFrame,
                    ReplayStage::RecordHeader, 0, implementation.currentOffset));
            }
            capture.hdr = (captureFlags & captureFlagHdr) != 0;
            capture.isCursorExcluded = (captureFlags & captureFlagCursorExcluded) != 0;
            capture.sourceCursorState = static_cast<pbcapturenormalize::CursorState>(std::to_integer<std::uint8_t>(header[142]));
            capture.sourceGeneration = LoadUint64(header, 144);
            capture.slotGeneration = LoadUint64(header, 152);
            capture.slotIndex = LoadUint32(header, 160);
            capture.physicalRoi = {LoadInt32(header, 164), LoadInt32(header, 168), LoadInt32(header, 172), LoadInt32(header, 176)};
            capture.sourceContentSize = {LoadInt32(header, 180), LoadInt32(header, 184)};
            capture.sourceExtent = {LoadInt32(header, 188), LoadInt32(header, 192)};
            capture.roiSize = {LoadInt32(header, 196), LoadInt32(header, 200)};
            capture.displayRotation = static_cast<DXGI_MODE_ROTATION>(LoadUint32(header, 204));
            capture.sourceTransform = static_cast<DXGI_MODE_ROTATION>(LoadUint32(header, 208));
            capture.sourcePixelFormat = static_cast<DXGI_FORMAT>(LoadUint32(header, 212));
            capture.pixelFormat = static_cast<DXGI_FORMAT>(LoadUint32(header, 216));
            capture.adapterLuid.HighPart = LoadInt32(header, 220);
            capture.adapterLuid.LowPart = LoadUint32(header, 224);
            capture.bitsPerColor = LoadUint32(header, 228);
            capture.outputColorSpace = LoadUint32(header, 232);
            capture.timestamp.rawValue = LoadInt64(header, 236);
            capture.timestamp.rawFrequency = LoadInt64(header, 244);
            capture.timestamp.monotonic100ns = LoadInt64(header, 252);
            capture.timestamp.arrivalQpc100ns = LoadInt64(header, 260);
            const std::uint64_t roiCopyTime = LoadUint64(header, 268);
            if (roiCopyTime != noRoiCopyTime)
            {
                capture.roiCopyTime100ns = roiCopyTime;
            }
            temporary.capture.dpiX = LoadUint32(header, 276);
            temporary.capture.dpiY = LoadUint32(header, 280);
            temporary.capture.scaleX = LoadDouble(header, 284);
            temporary.capture.scaleY = LoadDouble(header, 292);
            const std::uint8_t pointerFlags = std::to_integer<std::uint8_t>(header[396]);
            if ((pointerFlags & ~7u) != 0)
            {
                return FailReader(implementation, ReplayStatus::Failure(ReplayError::MalformedFrame,
                    ReplayStage::RecordHeader, 0, implementation.currentOffset));
            }
            capture.pointer.positionKnown = (pointerFlags & 1u) != 0;
            capture.pointer.shapeKnown = (pointerFlags & 2u) != 0;
            capture.pointer.separateVisible = (pointerFlags & 4u) != 0;
            capture.pointer.physicalLeft = LoadInt64(header, 400);
            capture.pointer.physicalTop = LoadInt64(header, 408);
            capture.pointer.rawUpdateTimestamp = LoadInt64(header, 416);
            capture.pointer.shapeType = LoadUint32(header, 424);
            capture.pointer.shapeWidth = LoadUint32(header, 428);
            capture.pointer.shapeRawHeight = LoadUint32(header, 432);
            capture.pointer.shapeVisibleHeight = LoadUint32(header, 436);
            capture.pointer.shapePitch = LoadUint32(header, 440);
            capture.pointer.shapeBytes = LoadUint32(header, 444);
            capture.pointer.hotspotX = LoadInt32(header, 448);
            capture.pointer.hotspotY = LoadInt32(header, 452);
            const ReplayV2CaptureView view{capture, temporary.capture.dpiX, temporary.capture.dpiY,
                temporary.capture.scaleX, temporary.capture.scaleY, temporary.capture.displayIdentityUtf8,
                {temporary.capture.capturedRoi.width, temporary.capture.capturedRoi.height,
                    temporary.capture.capturedRoi.rowPitch, temporary.capture.capturedRoi.pixelFormat,
                    temporary.capture.capturedRoi.pixels},
                temporary.capture.senderCanonicalRaster ? std::optional<ReplayRasterView>{ReplayRasterView{
                    temporary.capture.senderCanonicalRaster->width, temporary.capture.senderCanonicalRaster->height,
                    temporary.capture.senderCanonicalRaster->rowPitch, temporary.capture.senderCanonicalRaster->pixelFormat,
                    temporary.capture.senderCanonicalRaster->pixels}} : std::nullopt,
                temporary.capture.canonicalBootstrap};
            const auto captureStatus = ValidateCapture(view, implementation.limits);
            if (!captureStatus || LoadUint64(header, 120) != unavailableUint64 ||
                !IsZero(headerBytes.subspan(128, 10)) || !IsZero(headerBytes.subspan(143, 1)) ||
                !IsZero(headerBytes.subspan(456, recordHeaderCrcOffset - 456)))
            {
                return FailReader(implementation, ReplayStatus::Failure(ReplayError::MalformedFrame,
                    ReplayStage::RecordHeader, 0, implementation.currentOffset));
            }
            if (FindCapture(implementation.captures, capture.domain.captureEpoch, capture.captureObservation))
            {
                return FailReader(implementation, ReplayStatus::Failure(ReplayError::MalformedFrame,
                    ReplayStage::RecordHeader, 0, implementation.currentOffset));
            }
            std::uint64_t nextRasterBytes = 0;
            if (!AddWithinLimit(implementation.totalRasterBytes, capturedSize + senderSize,
                implementation.limits.maximumTotalRasterBytes, nextRasterBytes, ReplayStage::RecordPayload,
                implementation.currentOffset))
            {
                return FailReader(implementation, ReplayStatus::Failure(ReplayError::ResourceLimit,
                    ReplayStage::RecordPayload, 0, implementation.currentOffset));
            }
            implementation.captures.push_back({capture.domain.captureEpoch, capture.captureObservation, false});
            implementation.totalRasterBytes = nextRasterBytes;
            implementation.captureFrames++;
        }
        else if (type == ReplayV2RecordType::DemodObservation)
        {
            const auto headerBytes = std::span<const std::byte>(header);
            if (recordBytes != kReplayV2RecordHeaderBytes + 4 || !IsZero(headerBytes.subspan(28, 60)) ||
                !IsZero(headerBytes.subspan(104, 16)) || !IsZero(headerBytes.subspan(128, 4)) ||
                !IsZero(headerBytes.subspan(464, recordHeaderCrcOffset - 464)))
            {
                return FailReader(implementation, ReplayStatus::Failure(ReplayError::MalformedFrame,
                    ReplayStage::RecordHeader, 0, implementation.currentOffset));
            }
            std::array<std::byte, 4> trailer{};
            status = ReadExactAt(implementation.file, implementation.currentOffset + kReplayV2RecordHeaderBytes,
                trailer, ReplayStage::RecordPayload);
            if (!status)
            {
                return FailReader(implementation, status);
            }
            pbprotocol::Crc32c recordCrc;
            recordCrc.Update(header);
            if (recordCrc.Finalize() != LoadUint32(trailer, 0))
            {
                return FailReader(implementation, ReplayStatus::Failure(ReplayError::ChecksumMismatch,
                    ReplayStage::RecordPayload, 0, implementation.currentOffset));
            }
            auto& observation = temporary.demodObservation;
            observation.captureObservation = LoadUint64(header, 88);
            observation.captureEpoch = LoadUint64(header, 96);
            observation.frameSequence = LoadUint64(header, 120);
            observation.visualProfileId = LoadUint64(header, 456);
            observation.diagnosticCode = LoadUint32(header, 132);
            observation.disposition = static_cast<ReplayV2DemodDisposition>(std::to_integer<std::uint8_t>(header[136]));
            const std::uint8_t flags = std::to_integer<std::uint8_t>(header[137]);
            observation.bootstrapAttempted = (flags & demodFlagBootstrapAttempted) != 0;
            observation.bootstrapSucceeded = (flags & demodFlagBootstrapSucceeded) != 0;
            observation.frameSequenceAvailable = (flags & demodFlagFrameSequenceAvailable) != 0;
            observation.transportProduced = (flags & demodFlagTransportProduced) != 0;
            observation.receiverAdmitted = (flags & demodFlagReceiverAdmitted) != 0;
            observation.productionDetailAvailable = (flags & demodFlagProductionDetail) != 0;
            if (!observation.frameSequenceAvailable)
            {
                observation.frameSequence = 0;
            }
            if ((flags & ~(demodFlagBootstrapAttempted | demodFlagBootstrapSucceeded |
                demodFlagFrameSequenceAvailable | demodFlagTransportProduced | demodFlagReceiverAdmitted |
                demodFlagProductionDetail)) != 0 ||
                (!observation.frameSequenceAvailable && LoadUint64(header, 120) != unavailableUint64))
            {
                return FailReader(implementation, ReplayStatus::Failure(ReplayError::MalformedFrame,
                    ReplayStage::RecordHeader, 0, implementation.currentOffset));
            }
            if (observation.productionDetailAvailable)
            {
                const std::uint8_t detailFlags = std::to_integer<std::uint8_t>(header[143]);
                const std::uint8_t timingFlags = std::to_integer<std::uint8_t>(header[144]);
                if (std::to_integer<std::uint8_t>(header[138]) != kReplayV2DemodDetailVersion ||
                    (timingFlags & ~demodTimingFlagGpuTimingAvailable) != 0 ||
                    !IsZero(headerBytes.subspan(145, 7)) || !IsZero(headerBytes.subspan(308, 148)))
                {
                    return FailReader(implementation, ReplayStatus::Failure(ReplayError::MalformedFrame,
                        ReplayStage::RecordHeader, 0, implementation.currentOffset));
                }
                observation.layoutAvailable = (detailFlags & demodDetailFlagLayoutAvailable) != 0;
                observation.geometryAvailable = (detailFlags & demodDetailFlagGeometryAvailable) != 0;
                observation.evaluationAvailable = (detailFlags & demodDetailFlagEvaluationAvailable) != 0;
                observation.metricSummaryAvailable = (detailFlags & demodDetailFlagMetricSummaryAvailable) != 0;
                observation.carrierAccepted = (detailFlags & demodDetailFlagCarrierAccepted) != 0;
                observation.receiverStateAdvanced = (detailFlags & demodDetailFlagReceiverStateAdvanced) != 0;
                observation.senderTruthAvailable = (detailFlags & demodDetailFlagSenderTruthAvailable) != 0;
                observation.paddingValid = (detailFlags & demodDetailFlagPaddingValid) != 0;
                observation.gpuTimingAvailable = (timingFlags & demodTimingFlagGpuTimingAvailable) != 0;
                observation.visualLayoutVersion = std::to_integer<std::uint8_t>(header[139]);
                observation.resultKind = static_cast<ReplayV2DemodResultKind>(std::to_integer<std::uint8_t>(header[140]));
                observation.geometryStatus = static_cast<ReplayV2GeometryStatus>(std::to_integer<std::uint8_t>(header[141]));
                observation.temporalDisposition = static_cast<ReplayV2TemporalDisposition>(std::to_integer<std::uint8_t>(header[142]));
                observation.geometryOriginX = LoadDouble(header, 152);
                observation.geometryOriginY = LoadDouble(header, 160);
                observation.geometryScaleX = LoadDouble(header, 168);
                observation.geometryScaleY = LoadDouble(header, 176);
                observation.codewords = LoadUint32(header, 184);
                observation.fecFailures = LoadUint32(header, 188);
                observation.crcFailures = LoadUint32(header, 192);
                observation.identityFailures = LoadUint32(header, 196);
                observation.falseAcceptedCodewords = LoadUint32(header, 200);
                observation.acceptedTransportBlocks = LoadUint32(header, 204);
                observation.acceptedRemoteControlBlocks = LoadUint32(header, 208);
                observation.admittedTransportBlocks = LoadUint32(header, 212);
                observation.admittedRemoteControlBlocks = LoadUint32(header, 216);
                observation.iterationsTotal = LoadUint32(header, 220);
                observation.iterationsMaximum = LoadUint32(header, 224);
                observation.comparedCodedBits = LoadUint64(header, 228);
                observation.erroneousCodedBits = LoadUint64(header, 236);
                observation.metricSamples = LoadUint32(header, 244);
                observation.zeroMagnitudeMetrics = LoadUint32(header, 248);
                observation.minimumAbsoluteMetric = LoadDouble(header, 252);
                observation.meanAbsoluteMetric = LoadDouble(header, 260);
                observation.freshnessRegions = LoadUint32(header, 268);
                observation.staleRegions = LoadUint32(header, 272);
                observation.freshnessTagMismatches = LoadUint32(header, 276);
                observation.freshnessTagErasures = LoadUint32(header, 280);
                observation.freshnessErasedDataMetrics = LoadUint32(header, 284);
                observation.unreliableSymbols = LoadUint32(header, 288);
                observation.metricReadbackBytes = LoadUint64(header, 292);
                observation.gpuTime100ns = LoadUint64(header, 300);
            }
            else if (!IsZero(headerBytes.subspan(138, 318)))
            {
                return FailReader(implementation, ReplayStatus::Failure(ReplayError::MalformedFrame,
                    ReplayStage::RecordHeader, 0, implementation.currentOffset));
            }
            const auto observationStatus = ValidateDemodObservation(observation);
            CaptureIdentity* const capture = FindCapture(implementation.captures, observation.captureEpoch,
                observation.captureObservation);
            if (!observationStatus || observation.visualProfileId != implementation.descriptor.visualProfileId ||
                !capture || capture->hasDemodObservation)
            {
                return FailReader(implementation, ReplayStatus::Failure(ReplayError::MalformedFrame,
                    ReplayStage::RecordHeader, 0, implementation.currentOffset));
            }
            capture->hasDemodObservation = true;
            implementation.demodObservations++;
        }
        else
        {
            return FailReader(implementation, ReplayStatus::Failure(ReplayError::MalformedFrame,
                ReplayStage::RecordHeader, 0, implementation.currentOffset));
        }
    }
    catch (const std::bad_alloc&)
    {
        return FailReader(implementation, ReplayStatus::Failure(ReplayError::OutOfMemory,
            ReplayStage::RecordPayload, 0, implementation.currentOffset));
    }
    catch (const std::length_error&)
    {
        return FailReader(implementation, ReplayStatus::Failure(ReplayError::ResourceLimit,
            ReplayStage::RecordPayload, 0, implementation.currentOffset));
    }
    implementation.currentOffset = nextOffset;
    implementation.records++;
    if (implementation.captureFrames > implementation.expectedCaptureFrames ||
        implementation.demodObservations > implementation.expectedDemodObservations)
    {
        return FailReader(implementation, ReplayStatus::Failure(ReplayError::FrameCountMismatch,
            ReplayStage::Read, 0, implementation.currentOffset));
    }
    if (implementation.records == implementation.expectedRecords &&
        (implementation.captureFrames != implementation.expectedCaptureFrames ||
            implementation.demodObservations != implementation.expectedDemodObservations ||
            implementation.currentOffset != implementation.bytesBeforeFooter))
    {
        return FailReader(implementation, ReplayStatus::Failure(ReplayError::FrameCountMismatch,
            ReplayStage::Read, 0, implementation.currentOffset));
    }
    output = std::move(temporary);
    return {};
}

ReplayV2FileSnapshot ReplayV2Reader::GetSnapshot() const noexcept
{
    if (!implementation_)
    {
        return {};
    }
    const bool complete = !implementation_->failed && implementation_->records == implementation_->expectedRecords &&
        implementation_->currentOffset == implementation_->bytesBeforeFooter;
    return {implementation_->descriptor, implementation_->fileBytes, implementation_->totalRasterBytes,
        implementation_->captureFrames, implementation_->demodObservations, implementation_->records, complete};
}

} // namespace pbrealcapturereplay
