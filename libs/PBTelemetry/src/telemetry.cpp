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
    firstCaptureTimestamp100ns_ = 0;
    lastCaptureTimestamp100ns_ = 0;
    firstVisualTimestamp100ns_ = 0;
    lastVisualTimestamp100ns_ = 0;
    verifiedTimestamp100ns_ = 0;
    intervalMean100ns_ = 0;
    intervalM2_ = 0;
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
    if (sample.success && (!ValidOptionalMetric(sample.scaleX, true) || !ValidOptionalMetric(sample.scaleY, true) ||
        !sample.scaleX || !sample.scaleY || !sample.phaseX || !sample.phaseY || !std::isfinite(*sample.phaseX) || !std::isfinite(*sample.phaseY)))
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
    if (!evaluation.IsVerified())
    {
        static_cast<void>(Add(snapshot_.postFecFailedFrames, 1));
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
    output << "},\"PreFecBER\":";
    WriteOptional(output, snapshot.preFecBerEstimate);
    output << ",\"FER\":";
    WriteOptional(output, snapshot.fecFrameErrorRate);
    output << ",\"FecCodewordFailureRate\":";
    WriteOptional(output, snapshot.fecCodewordFailureRate);
    output << ",\"CRCFailure\":" << snapshot.crcFailures << ",\"OuterSymbols\":{\"unique\":" << snapshot.uniqueOuterSymbols
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
