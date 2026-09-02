#include "pbtelemetry/telemetry.h"

#include "pbprotocol/checked_integer.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <ostream>
#include <ranges>

namespace pbtelemetry
{
namespace
{

bool NonzeroSourceId(const pbcapturenormalize::ScreenCaptureDomain& domain) noexcept
{
    return std::ranges::any_of(domain.sourceId, [](const std::byte value) { return value != std::byte{0}; });
}

bool ValidOptionalMetric(const std::optional<double>& value, const bool positive = false) noexcept
{
    return !value || (std::isfinite(*value) && (positive ? *value > 0 : *value >= 0));
}

void ExtendRange(std::optional<double>& minimum, std::optional<double>& maximum, const double value) noexcept
{
    minimum = minimum ? std::min(*minimum, value) : value;
    maximum = maximum ? std::max(*maximum, value) : value;
}

void WriteOptional(std::ostream& output, const std::optional<double>& value)
{
    if (value && std::isfinite(*value))
    {
        output << *value;
    }
    else
    {
        output << "null";
    }
}

void WriteDomain(std::ostream& output, const pbcapturenormalize::ScreenCaptureDomain& domain)
{
    constexpr char digits[] = "0123456789abcdef";
    output << "{\"sourceId\":\"";
    for (const auto value : domain.sourceId)
    {
        const auto byte = std::to_integer<unsigned>(value);
        output << digits[byte >> 4] << digits[byte & 15];
    }
    output << "\",\"captureEpoch\":\"" << domain.captureEpoch << "\"}";
}

} // namespace

bool TelemetryAccumulator::Add(std::uint64_t& target, const std::uint64_t value) noexcept
{
    const auto result = pbprotocol::SaturatingAddUnsigned(target, value);
    const bool exact = result >= target && result - target == value;
    target = result;
    snapshot_.counterSaturated = snapshot_.counterSaturated || !exact;
    return exact;
}

TelemetryStatus TelemetryAccumulator::ValidateDomain(const pbcapturenormalize::ScreenCaptureDomain& domain) const noexcept
{
    if (!snapshot_.active)
    {
        return TelemetryStatus::Failure(TelemetryError::InactiveEpoch);
    }
    return domain == snapshot_.domain ? TelemetryStatus{} : TelemetryStatus::Failure(TelemetryError::DomainMismatch);
}

TelemetryStatus TelemetryAccumulator::BeginCaptureEpoch(const pbcapturenormalize::ScreenCaptureDomain& domain,
    const std::int64_t epochStart100ns) noexcept
{
    if (domain.captureEpoch == 0 || !NonzeroSourceId(domain) || epochStart100ns < 0 || lifetimeEpochStarts_ == UINT64_MAX)
    {
        return TelemetryStatus::Failure(TelemetryError::InvalidEpoch);
    }
    lifetimeEpochStarts_++;
    snapshot_ = {};
    snapshot_.active = true;
    snapshot_.domain = domain;
    snapshot_.epochStart100ns = epochStart100ns;
    snapshot_.epochStarts = lifetimeEpochStarts_;
    lastCaptureObservation_ = 0;
    lastBootstrapObservation_ = 0;
    lastFecObservation_ = 0;
    lastRemoteMetricObservation_ = 0;
    firstCaptureTimestamp100ns_ = 0;
    lastCaptureTimestamp100ns_ = 0;
    firstVisualTimestamp100ns_ = 0;
    lastVisualTimestamp100ns_ = 0;
    verifiedTimestamp100ns_ = 0;
    intervalMean100ns_ = 0;
    intervalM2_ = 0;
    remoteAbsoluteMetricSum_ = 0;
    remoteVerifiedAbsoluteMetricSum_ = 0;
    remoteRejectedAbsoluteMetricSum_ = 0;
    remoteVerifiedMetricSamples_ = 0;
    remoteRejectedMetricSamples_ = 0;
    remoteRejectedZeroMagnitudeMetrics_ = 0;
    previousPixelDigest_.reset();
    return {};
}

void TelemetryAccumulator::EndCaptureEpoch() noexcept
{
    snapshot_.active = false;
}

TelemetryStatus TelemetryAccumulator::RecordPresentation(const pbpresenttiming::TimingSnapshot& sample) noexcept
{
    if (!snapshot_.active)
    {
        return TelemetryStatus::Failure(TelemetryError::InactiveEpoch);
    }
    if (sample.presentationEpoch == 0 || sample.qpcFrequency <= 0 || sample.epochStartQpc < 0 || sample.sampleQpc < sample.epochStartQpc ||
        !ValidOptionalMetric(sample.presentCallFps) || !ValidOptionalMetric(sample.presentedVisualFps) ||
        !ValidOptionalMetric(sample.observedVisualFpsLowerBound) || !ValidOptionalMetric(sample.presentQueueLatencyMs))
    {
        return TelemetryStatus::Failure(TelemetryError::InvalidSample);
    }
    snapshot_.presentation = sample;
    return {};
}

TelemetryStatus TelemetryAccumulator::RecordCapture(const CaptureSample& sample) noexcept
{
    const auto domainStatus = ValidateDomain(sample.domain);
    if (!domainStatus)
    {
        return domainStatus;
    }
    if (sample.captureObservation == 0 || sample.captureObservation <= lastCaptureObservation_)
    {
        return TelemetryStatus::Failure(TelemetryError::ObservationOrder);
    }
    if (sample.timestamp100ns < snapshot_.epochStart100ns ||
        (snapshot_.capturedFrames != 0 && sample.timestamp100ns < lastCaptureTimestamp100ns_))
    {
        return TelemetryStatus::Failure(TelemetryError::TimestampOrder);
    }

    if (snapshot_.capturedFrames == 0)
    {
        firstCaptureTimestamp100ns_ = sample.timestamp100ns;
    }
    else
    {
        const auto interval = static_cast<double>(sample.timestamp100ns - lastCaptureTimestamp100ns_);
        const auto nextCount = snapshot_.captureIntervals == std::numeric_limits<std::uint64_t>::max() ?
            std::numeric_limits<std::uint64_t>::max() : snapshot_.captureIntervals + 1;
        const double delta = interval - intervalMean100ns_;
        intervalMean100ns_ += delta / static_cast<double>(nextCount);
        intervalM2_ += delta * (interval - intervalMean100ns_);
        static_cast<void>(Add(snapshot_.captureIntervals, 1));
    }
    lastCaptureTimestamp100ns_ = sample.timestamp100ns;
    lastCaptureObservation_ = sample.captureObservation;
    static_cast<void>(Add(snapshot_.capturedFrames, 1));
    if (sample.roiCopyTime100ns)
    {
        static_cast<void>(Add(snapshot_.roiCopyTimingSamples, 1));
        static_cast<void>(Add(snapshot_.roiCopyTimeTotal100ns, *sample.roiCopyTime100ns));
        snapshot_.roiCopyTimeMaximum100ns = std::max(snapshot_.roiCopyTimeMaximum100ns, *sample.roiCopyTime100ns);
    }
    if (sample.pixelDigest)
    {
        if (snapshot_.fingerprintedFrames == 0)
        {
            firstVisualTimestamp100ns_ = sample.timestamp100ns;
            static_cast<void>(Add(snapshot_.uniqueVisualFrames, 1));
        }
        else if (previousPixelDigest_ == sample.pixelDigest)
        {
            static_cast<void>(Add(snapshot_.duplicateFrames, 1));
        }
        else
        {
            static_cast<void>(Add(snapshot_.uniqueVisualFrames, 1));
        }
        lastVisualTimestamp100ns_ = sample.timestamp100ns;
        previousPixelDigest_ = sample.pixelDigest;
        static_cast<void>(Add(snapshot_.fingerprintedFrames, 1));
    }
    return snapshot_.counterSaturated ? TelemetryStatus::Failure(TelemetryError::CounterOverflow) : TelemetryStatus{};
}

void TelemetryAccumulator::RecordDroppedFrames(const std::uint64_t count) noexcept
{
    if (snapshot_.active)
    {
        static_cast<void>(Add(snapshot_.droppedFrames, count));
    }
}

TelemetryStatus TelemetryAccumulator::RecordBootstrap(const BootstrapSample& sample) noexcept
{
    const auto domainStatus = ValidateDomain(sample.domain);
    if (!domainStatus)
    {
        return domainStatus;
    }
    if (sample.captureObservation == 0 || sample.captureObservation > lastCaptureObservation_ ||
        sample.captureObservation <= lastBootstrapObservation_)
    {
        return TelemetryStatus::Failure(TelemetryError::ObservationOrder);
    }
    const bool completeGeometry = sample.scaleX && sample.scaleY && sample.phaseX && sample.phaseY && sample.originX &&
        sample.originY && sample.markerResidualPixels;
    const double scaleAnisotropy = completeGeometry ? std::abs(*sample.scaleX - *sample.scaleY) : 0;
    if (sample.success && (!completeGeometry || !ValidOptionalMetric(sample.scaleX, true) ||
        !ValidOptionalMetric(sample.scaleY, true) || !std::isfinite(*sample.phaseX) || !std::isfinite(*sample.phaseY) ||
        !std::isfinite(*sample.originX) || !std::isfinite(*sample.originY) ||
        !ValidOptionalMetric(sample.markerResidualPixels) || !std::isfinite(scaleAnisotropy)))
    {
        return TelemetryStatus::Failure(TelemetryError::InvalidSample);
    }
    lastBootstrapObservation_ = sample.captureObservation;
    static_cast<void>(Add(snapshot_.bootstrapAttempts, 1));
    if (sample.success)
    {
        static_cast<void>(Add(snapshot_.bootstrapSuccesses, 1));
        snapshot_.scaleX = sample.scaleX;
        snapshot_.scaleY = sample.scaleY;
        snapshot_.phaseX = sample.phaseX;
        snapshot_.phaseY = sample.phaseY;
        auto& geometry = snapshot_.observedLocatorGeometry;
        static_cast<void>(Add(geometry.samples, 1));
        geometry.lastOriginX = sample.originX;
        geometry.lastOriginY = sample.originY;
        geometry.lastScaleX = sample.scaleX;
        geometry.lastScaleY = sample.scaleY;
        geometry.lastMarkerResidualPixels = sample.markerResidualPixels;
        ExtendRange(geometry.minimumOriginX, geometry.maximumOriginX, *sample.originX);
        ExtendRange(geometry.minimumOriginY, geometry.maximumOriginY, *sample.originY);
        ExtendRange(geometry.minimumScaleX, geometry.maximumScaleX, *sample.scaleX);
        ExtendRange(geometry.minimumScaleY, geometry.maximumScaleY, *sample.scaleY);
        ExtendRange(geometry.minimumMarkerResidualPixels, geometry.maximumMarkerResidualPixels, *sample.markerResidualPixels);
        geometry.maximumScaleAnisotropy = geometry.maximumScaleAnisotropy ?
            std::max(*geometry.maximumScaleAnisotropy, scaleAnisotropy) : scaleAnisotropy;
    }
    return snapshot_.counterSaturated ? TelemetryStatus::Failure(TelemetryError::CounterOverflow) : TelemetryStatus{};
}

TelemetryStatus TelemetryAccumulator::RecordFec(const FecSample& sample) noexcept
{
    const auto domainStatus = ValidateDomain(sample.domain);
    if (!domainStatus)
    {
        return domainStatus;
    }
    const auto& evaluation = sample.evaluation;
    const bool preFecCoverageUnavailable = evaluation.comparedCodedBits == 0 && evaluation.erroneousCodedBits == 0;
    const bool completePreFecCoverage = evaluation.comparedCodedBits ==
        static_cast<std::uint64_t>(evaluation.codewords) * pbdesktoplevels::kCodewordBits &&
        evaluation.erroneousCodedBits <= evaluation.comparedCodedBits;
    if (sample.captureObservation == 0 || sample.captureObservation > lastCaptureObservation_ ||
        sample.captureObservation <= lastFecObservation_)
    {
        return TelemetryStatus::Failure(TelemetryError::ObservationOrder);
    }
    if (!evaluation.evaluated || evaluation.codewords == 0 || evaluation.codewords > pbdesktoplevels::kMaximumCodewords ||
        (!preFecCoverageUnavailable && !completePreFecCoverage) || evaluation.fecFailures > evaluation.codewords ||
        evaluation.crcFailures > evaluation.codewords || evaluation.identityFailures > evaluation.codewords ||
        static_cast<std::uint64_t>(evaluation.fecFailures) + evaluation.crcFailures > evaluation.codewords ||
        evaluation.falseAcceptedCodewords > evaluation.codewords - evaluation.fecFailures - evaluation.crcFailures ||
        evaluation.iterationsMaximum > 48 || evaluation.iterationsTotal > static_cast<std::uint64_t>(evaluation.codewords) * 48 ||
        static_cast<std::uint64_t>(evaluation.acceptedTransportBlocks) + evaluation.fecFailures + evaluation.crcFailures +
            evaluation.identityFailures != evaluation.codewords)
    {
        return TelemetryStatus::Failure(TelemetryError::InvalidSample);
    }
    lastFecObservation_ = sample.captureObservation;
    static_cast<void>(Add(snapshot_.fecEvaluatedFrames, 1));
    static_cast<void>(Add(snapshot_.comparedCodedBits, evaluation.comparedCodedBits));
    static_cast<void>(Add(snapshot_.erroneousCodedBits, evaluation.erroneousCodedBits));
    static_cast<void>(Add(snapshot_.fecCodewords, evaluation.codewords));
    static_cast<void>(Add(snapshot_.fecFailures, evaluation.fecFailures));
    static_cast<void>(Add(snapshot_.crcFailures, evaluation.crcFailures));
    static_cast<void>(Add(snapshot_.identityFailures, evaluation.identityFailures));
    static_cast<void>(Add(snapshot_.acceptedTransportCodewords, evaluation.acceptedTransportBlocks));
    if (!evaluation.IsVerified())
    {
        static_cast<void>(Add(snapshot_.postFecFailedFrames, 1));
    }
    return snapshot_.counterSaturated ? TelemetryStatus::Failure(TelemetryError::CounterOverflow) : TelemetryStatus{};
}

TelemetryStatus TelemetryAccumulator::RecordRemoteMetric(const RemoteMetricSample& sample) noexcept
{
    const auto domainStatus = ValidateDomain(sample.domain);
    if (!domainStatus)
    {
        return domainStatus;
    }
    const bool validFrameClass = sample.frameClass == RemoteMetricFrameClass::Other ||
        sample.frameClass == RemoteMetricFrameClass::TransportVerified ||
        sample.frameClass == RemoteMetricFrameClass::TransportRejected;
    const double weightedMetricSum = sample.meanAbsoluteMetric * static_cast<double>(sample.metricSamples);
    if (sample.captureObservation == 0 || sample.captureObservation > lastCaptureObservation_ ||
        sample.captureObservation <= lastRemoteMetricObservation_)
    {
        return TelemetryStatus::Failure(TelemetryError::ObservationOrder);
    }
    if (!validFrameClass || sample.metricSamples == 0 || sample.zeroMagnitudeMetrics > sample.metricSamples ||
        !std::isfinite(sample.minimumAbsoluteMetric) || !std::isfinite(sample.meanAbsoluteMetric) ||
        sample.minimumAbsoluteMetric < 0 || sample.meanAbsoluteMetric < sample.minimumAbsoluteMetric ||
        !std::isfinite(weightedMetricSum) || sample.unreliableSymbols > sample.symbolSamples ||
        (sample.symbolSamples == 0 && sample.unreliableSymbols != 0) || sample.freshnessRegions == 0 ||
        sample.staleRegions > sample.freshnessRegions ||
        sample.freshnessErasedDataMetrics > sample.metricSamples)
    {
        return TelemetryStatus::Failure(TelemetryError::InvalidSample);
    }
    const double nextMetricSum = remoteAbsoluteMetricSum_ + weightedMetricSum;
    const double nextVerifiedMetricSum = sample.frameClass == RemoteMetricFrameClass::TransportVerified ?
        remoteVerifiedAbsoluteMetricSum_ + weightedMetricSum : remoteVerifiedAbsoluteMetricSum_;
    const double nextRejectedMetricSum = sample.frameClass == RemoteMetricFrameClass::TransportRejected ?
        remoteRejectedAbsoluteMetricSum_ + weightedMetricSum : remoteRejectedAbsoluteMetricSum_;
    if (!std::isfinite(nextMetricSum) || !std::isfinite(nextVerifiedMetricSum) || !std::isfinite(nextRejectedMetricSum))
    {
        snapshot_.counterSaturated = true;
        return TelemetryStatus::Failure(TelemetryError::CounterOverflow);
    }

    lastRemoteMetricObservation_ = sample.captureObservation;
    const bool firstMetricFrame = snapshot_.remoteMetricFrames == 0;
    static_cast<void>(Add(snapshot_.remoteMetricFrames, 1));
    static_cast<void>(Add(snapshot_.remoteMetricSamples, sample.metricSamples));
    static_cast<void>(Add(snapshot_.remoteZeroMagnitudeMetrics, sample.zeroMagnitudeMetrics));
    static_cast<void>(Add(snapshot_.remoteSymbolSamples, sample.symbolSamples));
    static_cast<void>(Add(snapshot_.remoteUnreliableSymbols, sample.unreliableSymbols));
    static_cast<void>(Add(snapshot_.remoteFreshnessRegions, sample.freshnessRegions));
    static_cast<void>(Add(snapshot_.remoteFreshRegions, sample.freshnessRegions - sample.staleRegions));
    static_cast<void>(Add(snapshot_.remoteStaleRegions, sample.staleRegions));
    static_cast<void>(Add(snapshot_.remoteFramesWithStaleRegions, static_cast<std::uint64_t>(sample.staleRegions != 0)));
    static_cast<void>(Add(snapshot_.remoteFreshnessTagMismatches, sample.freshnessTagMismatches));
    static_cast<void>(Add(snapshot_.remoteFreshnessTagErasures, sample.freshnessTagErasures));
    static_cast<void>(Add(snapshot_.remoteFreshnessErasedDataMetrics, sample.freshnessErasedDataMetrics));
    snapshot_.remoteMinimumAbsoluteMetric = firstMetricFrame ? sample.minimumAbsoluteMetric :
        std::min(*snapshot_.remoteMinimumAbsoluteMetric, sample.minimumAbsoluteMetric);
    remoteAbsoluteMetricSum_ = nextMetricSum;
    if (sample.frameClass == RemoteMetricFrameClass::TransportVerified)
    {
        static_cast<void>(Add(snapshot_.remoteTransportVerifiedMetricFrames, 1));
        static_cast<void>(Add(remoteVerifiedMetricSamples_, sample.metricSamples));
        remoteVerifiedAbsoluteMetricSum_ = nextVerifiedMetricSum;
    }
    else if (sample.frameClass == RemoteMetricFrameClass::TransportRejected)
    {
        static_cast<void>(Add(snapshot_.remoteTransportRejectedMetricFrames, 1));
        static_cast<void>(Add(remoteRejectedMetricSamples_, sample.metricSamples));
        static_cast<void>(Add(remoteRejectedZeroMagnitudeMetrics_, sample.zeroMagnitudeMetrics));
        remoteRejectedAbsoluteMetricSum_ = nextRejectedMetricSum;
    }
    return snapshot_.counterSaturated ? TelemetryStatus::Failure(TelemetryError::CounterOverflow) : TelemetryStatus{};
}

void TelemetryAccumulator::RecordOuterSymbol(const OuterSymbolDisposition disposition) noexcept
{
    if (!snapshot_.active)
    {
        return;
    }
    switch (disposition)
    {
    case OuterSymbolDisposition::AcceptedUnique:
        static_cast<void>(Add(snapshot_.uniqueOuterSymbols, 1));
        static_cast<void>(Add(snapshot_.acceptedOuterSymbols, 1));
        break;
    case OuterSymbolDisposition::Duplicate:
        static_cast<void>(Add(snapshot_.duplicateOuterSymbols, 1));
        break;
    case OuterSymbolDisposition::Conflict:
        static_cast<void>(Add(snapshot_.outerSymbolConflicts, 1));
        break;
    case OuterSymbolDisposition::Rejected:
        static_cast<void>(Add(snapshot_.rejectedOuterSymbols, 1));
        break;
    }
}

TelemetryStatus TelemetryAccumulator::RecordVerifiedEncodedBytes(const std::uint64_t bytes,
    const std::int64_t timestamp100ns) noexcept
{
    if (!snapshot_.active)
    {
        return TelemetryStatus::Failure(TelemetryError::InactiveEpoch);
    }
    if (bytes == 0)
    {
        return TelemetryStatus::Failure(TelemetryError::InvalidSample);
    }
    if (snapshot_.capturedFrames == 0 || timestamp100ns < lastCaptureTimestamp100ns_ ||
        (verifiedTimestamp100ns_ != 0 && timestamp100ns < verifiedTimestamp100ns_))
    {
        return TelemetryStatus::Failure(TelemetryError::TimestampOrder);
    }
    verifiedTimestamp100ns_ = timestamp100ns;
    static_cast<void>(Add(snapshot_.verifiedEncodedBytes, bytes));
    return snapshot_.counterSaturated ? TelemetryStatus::Failure(TelemetryError::CounterOverflow) : TelemetryStatus{};
}

TelemetrySnapshot TelemetryAccumulator::GetSnapshot() const noexcept
{
    TelemetrySnapshot result = snapshot_;
    if (result.counterSaturated)
    {
        return result;
    }
    const auto Rate = [](const std::uint64_t numerator, const std::int64_t elapsed100ns) -> std::optional<double>
    {
        if (elapsed100ns <= 0)
        {
            return std::nullopt;
        }
        return static_cast<double>(numerator) * 10000000.0 / static_cast<double>(elapsed100ns);
    };
    if (result.capturedFrames >= 2)
    {
        result.captureFps = Rate(result.capturedFrames - 1, lastCaptureTimestamp100ns_ - firstCaptureTimestamp100ns_);
    }
    if (result.fingerprintedFrames >= 2)
    {
        result.uniqueVisualFps = Rate(result.uniqueVisualFrames - 1, lastVisualTimestamp100ns_ - firstVisualTimestamp100ns_);
    }
    if (result.fingerprintedFrames != 0)
    {
        result.duplicateFrameRatio = static_cast<double>(result.duplicateFrames) / static_cast<double>(result.fingerprintedFrames);
    }
    if (result.captureIntervals != 0)
    {
        result.frameArrivalJitterMilliseconds = std::sqrt(std::max(0.0, intervalM2_ / static_cast<double>(result.captureIntervals))) / 10000.0;
    }
    if (result.roiCopyTimingSamples != 0)
    {
        result.roiCopyTimeAverageMilliseconds = static_cast<double>(result.roiCopyTimeTotal100ns) /
            static_cast<double>(result.roiCopyTimingSamples) / 10000.0;
        result.roiCopyTimeMaximumMilliseconds = static_cast<double>(result.roiCopyTimeMaximum100ns) / 10000.0;
    }
    if (result.bootstrapAttempts != 0)
    {
        result.bootstrapSuccessRate = static_cast<double>(result.bootstrapSuccesses) / static_cast<double>(result.bootstrapAttempts);
    }
    if (result.comparedCodedBits != 0)
    {
        result.preFecBerEstimate = static_cast<double>(result.erroneousCodedBits) / static_cast<double>(result.comparedCodedBits);
    }
    if (result.fecEvaluatedFrames != 0)
    {
        result.fecFrameErrorRate = static_cast<double>(result.postFecFailedFrames) / static_cast<double>(result.fecEvaluatedFrames);
    }
    if (result.fecCodewords != 0)
    {
        result.fecCodewordFailureRate = static_cast<double>(result.fecFailures) / static_cast<double>(result.fecCodewords);
        result.acceptedTransportCodewordRate = static_cast<double>(result.acceptedTransportCodewords) /
            static_cast<double>(result.fecCodewords);
    }
    if (result.remoteMetricSamples != 0)
    {
        result.remoteZeroMagnitudeMetricRate = static_cast<double>(result.remoteZeroMagnitudeMetrics) /
            static_cast<double>(result.remoteMetricSamples);
        result.remoteMeanAbsoluteMetric = remoteAbsoluteMetricSum_ / static_cast<double>(result.remoteMetricSamples);
        result.remoteFreshnessErasedDataMetricRate = static_cast<double>(result.remoteFreshnessErasedDataMetrics) /
            static_cast<double>(result.remoteMetricSamples);
    }
    if (result.remoteSymbolSamples != 0)
    {
        result.remoteUnreliableSymbolRate = static_cast<double>(result.remoteUnreliableSymbols) /
            static_cast<double>(result.remoteSymbolSamples);
    }
    if (remoteVerifiedMetricSamples_ != 0)
    {
        result.remoteVerifiedMeanAbsoluteMetric = remoteVerifiedAbsoluteMetricSum_ /
            static_cast<double>(remoteVerifiedMetricSamples_);
    }
    if (remoteRejectedMetricSamples_ != 0)
    {
        result.remoteRejectedMeanAbsoluteMetric = remoteRejectedAbsoluteMetricSum_ /
            static_cast<double>(remoteRejectedMetricSamples_);
        result.remoteRejectedZeroMagnitudeMetricRate = static_cast<double>(remoteRejectedZeroMagnitudeMetrics_) /
            static_cast<double>(remoteRejectedMetricSamples_);
    }
    if (result.remoteFreshnessRegions != 0)
    {
        result.remoteStaleRegionRate = static_cast<double>(result.remoteStaleRegions) /
            static_cast<double>(result.remoteFreshnessRegions);
    }
    if (result.verifiedEncodedBytes != 0 && verifiedTimestamp100ns_ > firstCaptureTimestamp100ns_ &&
        result.verifiedEncodedBytes <= std::numeric_limits<std::uint64_t>::max() / 8)
    {
        result.verifiedEncodedGoodputBitsPerSecond = Rate(result.verifiedEncodedBytes * 8,
            verifiedTimestamp100ns_ - firstCaptureTimestamp100ns_);
    }
    return result;
}

void WriteTelemetryJson(std::ostream& output, const TelemetrySnapshot& snapshot)
{
    output.imbue(std::locale::classic());
    output << std::setprecision(17) << std::boolalpha << "{\"event\":\"pb-telemetry\",\"active\":" << snapshot.active << ",\"domain\":";
    WriteDomain(output, snapshot.domain);
    output << ",\"PresentCallFPS\":";
    WriteOptional(output, snapshot.presentation ? snapshot.presentation->presentCallFps : std::nullopt);
    output << ",\"PresentedVisualFPS\":";
    WriteOptional(output, snapshot.presentation ? snapshot.presentation->presentedVisualFps : std::nullopt);
    output << ",\"CaptureFPS\":";
    WriteOptional(output, snapshot.captureFps);
    output << ",\"UniqueVisualFPS\":";
    WriteOptional(output, snapshot.uniqueVisualFps);
    output << ",\"DuplicateFrameRatio\":";
    WriteOptional(output, snapshot.duplicateFrameRatio);
    output << ",\"FrameArrivalJitterMs\":";
    WriteOptional(output, snapshot.frameArrivalJitterMilliseconds);
    output << ",\"DroppedByDecoder\":" << snapshot.droppedFrames << ",\"CaptureEpoch\":\"" << snapshot.domain.captureEpoch
           << "\",\"RoiGpuCopyTimeMs\":{\"samples\":" << snapshot.roiCopyTimingSamples << ",\"average\":";
    WriteOptional(output, snapshot.roiCopyTimeAverageMilliseconds);
    output << ",\"maximum\":";
    WriteOptional(output, snapshot.roiCopyTimeMaximumMilliseconds);
    output << "},\"BootstrapSuccessRate\":";
    WriteOptional(output, snapshot.bootstrapSuccessRate);
    output << ",\"bootstrap\":{\"attempts\":" << snapshot.bootstrapAttempts << ",\"successes\":" << snapshot.bootstrapSuccesses
           << ",\"scaleX\":";
    WriteOptional(output, snapshot.scaleX);
    output << ",\"scaleY\":";
    WriteOptional(output, snapshot.scaleY);
    output << ",\"phaseX\":";
    WriteOptional(output, snapshot.phaseX);
    output << ",\"phaseY\":";
    WriteOptional(output, snapshot.phaseY);
    const auto& geometry = snapshot.observedLocatorGeometry;
    output << "},\"observedLocatorGeometry\":{\"authority\":\"AcceptedBootstrapLocatorPixels\",\"samples\":" <<
        geometry.samples << ",\"lastOriginX\":";
    WriteOptional(output, geometry.lastOriginX);
    output << ",\"lastOriginY\":";
    WriteOptional(output, geometry.lastOriginY);
    output << ",\"lastScaleX\":";
    WriteOptional(output, geometry.lastScaleX);
    output << ",\"lastScaleY\":";
    WriteOptional(output, geometry.lastScaleY);
    output << ",\"lastMarkerResidualPixels\":";
    WriteOptional(output, geometry.lastMarkerResidualPixels);
    output << ",\"minimumOriginX\":";
    WriteOptional(output, geometry.minimumOriginX);
    output << ",\"maximumOriginX\":";
    WriteOptional(output, geometry.maximumOriginX);
    output << ",\"minimumOriginY\":";
    WriteOptional(output, geometry.minimumOriginY);
    output << ",\"maximumOriginY\":";
    WriteOptional(output, geometry.maximumOriginY);
    output << ",\"minimumScaleX\":";
    WriteOptional(output, geometry.minimumScaleX);
    output << ",\"maximumScaleX\":";
    WriteOptional(output, geometry.maximumScaleX);
    output << ",\"minimumScaleY\":";
    WriteOptional(output, geometry.minimumScaleY);
    output << ",\"maximumScaleY\":";
    WriteOptional(output, geometry.maximumScaleY);
    output << ",\"minimumMarkerResidualPixels\":";
    WriteOptional(output, geometry.minimumMarkerResidualPixels);
    output << ",\"maximumMarkerResidualPixels\":";
    WriteOptional(output, geometry.maximumMarkerResidualPixels);
    output << ",\"maximumScaleAnisotropy\":";
    WriteOptional(output, geometry.maximumScaleAnisotropy);
    output << "},\"PreFecBER\":";
    WriteOptional(output, snapshot.preFecBerEstimate);
    output << ",\"FER\":";
    WriteOptional(output, snapshot.fecFrameErrorRate);
    output << ",\"FecCodewordFailureRate\":";
    WriteOptional(output, snapshot.fecCodewordFailureRate);
    output << ",\"AcceptedTransportCodewordRate\":";
    WriteOptional(output, snapshot.acceptedTransportCodewordRate);
    output << ",\"remoteMetric\":{\"frames\":" << snapshot.remoteMetricFrames
           << ",\"samples\":" << snapshot.remoteMetricSamples
           << ",\"zeroMagnitudeMetrics\":" << snapshot.remoteZeroMagnitudeMetrics
           << ",\"zeroMagnitudeRate\":";
    WriteOptional(output, snapshot.remoteZeroMagnitudeMetricRate);
    output << ",\"minimumAbsoluteMetric\":";
    WriteOptional(output, snapshot.remoteMinimumAbsoluteMetric);
    output << ",\"meanAbsoluteMetric\":";
    WriteOptional(output, snapshot.remoteMeanAbsoluteMetric);
    output << ",\"verifiedFrames\":" << snapshot.remoteTransportVerifiedMetricFrames
           << ",\"rejectedFrames\":" << snapshot.remoteTransportRejectedMetricFrames
           << ",\"verifiedMeanAbsoluteMetric\":";
    WriteOptional(output, snapshot.remoteVerifiedMeanAbsoluteMetric);
    output << ",\"rejectedMeanAbsoluteMetric\":";
    WriteOptional(output, snapshot.remoteRejectedMeanAbsoluteMetric);
    output << ",\"rejectedZeroMagnitudeMetricRate\":";
    WriteOptional(output, snapshot.remoteRejectedZeroMagnitudeMetricRate);
    output << ",\"symbolSamples\":" << snapshot.remoteSymbolSamples
           << ",\"unreliableSymbols\":" << snapshot.remoteUnreliableSymbols
           << ",\"unreliableSymbolRate\":";
    WriteOptional(output, snapshot.remoteUnreliableSymbolRate);
    output << ",\"freshnessRegions\":" << snapshot.remoteFreshnessRegions
           << ",\"freshRegions\":" << snapshot.remoteFreshRegions
           << ",\"staleRegions\":" << snapshot.remoteStaleRegions
           << ",\"staleRegionRate\":";
    WriteOptional(output, snapshot.remoteStaleRegionRate);
    output << ",\"framesWithStaleRegions\":" << snapshot.remoteFramesWithStaleRegions
           << ",\"freshnessTagMismatches\":" << snapshot.remoteFreshnessTagMismatches
           << ",\"freshnessTagErasures\":" << snapshot.remoteFreshnessTagErasures
           << ",\"freshnessErasedDataMetrics\":" << snapshot.remoteFreshnessErasedDataMetrics
           << ",\"freshnessErasedDataMetricRate\":";
    WriteOptional(output, snapshot.remoteFreshnessErasedDataMetricRate);
    output << "},\"CRCFailure\":" << snapshot.crcFailures << ",\"OuterSymbols\":{\"unique\":" << snapshot.uniqueOuterSymbols
           << ",\"duplicate\":" << snapshot.duplicateOuterSymbols << ",\"accepted\":" << snapshot.acceptedOuterSymbols
           << ",\"conflict\":" << snapshot.outerSymbolConflicts << ",\"rejected\":" << snapshot.rejectedOuterSymbols
           << "},\"VerifiedEncodedGoodput\":";
    WriteOptional(output, snapshot.verifiedEncodedGoodputBitsPerSecond);
    output << ",\"verifiedEncodedBytes\":" << snapshot.verifiedEncodedBytes << ",\"counterSaturated\":" << snapshot.counterSaturated << '}';
}

const char* GetTelemetryErrorName(const TelemetryError error) noexcept
{
    switch (error)
    {
    case TelemetryError::None: return "None";
    case TelemetryError::InvalidEpoch: return "InvalidEpoch";
    case TelemetryError::InactiveEpoch: return "InactiveEpoch";
    case TelemetryError::DomainMismatch: return "DomainMismatch";
    case TelemetryError::ObservationOrder: return "ObservationOrder";
    case TelemetryError::TimestampOrder: return "TimestampOrder";
    case TelemetryError::InvalidSample: return "InvalidSample";
    case TelemetryError::CounterOverflow: return "CounterOverflow";
    }
    return "Unknown";
}

} // namespace pbtelemetry
