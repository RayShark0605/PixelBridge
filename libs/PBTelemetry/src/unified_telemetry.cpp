#include "pbtelemetry/unified_telemetry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ostream>

namespace pbtelemetry
{
namespace
{
bool Add(std::uint64_t& target, const std::uint64_t value) noexcept
{
    if (value > (std::numeric_limits<std::uint64_t>::max)() - target)
    {
        return false;
    }
    target += value;
    return true;
}

std::size_t LaneIndex(const pbmodulation::UnifiedLane lane) noexcept
{
    return lane == pbmodulation::UnifiedLane::BaseLuma ? 0 : lane == pbmodulation::UnifiedLane::FineLuma ? 1 : 2;
}

const char* CoverageFailureName(const UnifiedCoverageFailureReason reason) noexcept
{
    switch (reason)
    {
    case UnifiedCoverageFailureReason::InvalidSample: return "InvalidSample";
    case UnifiedCoverageFailureReason::TimestampOrder: return "TimestampOrder";
    case UnifiedCoverageFailureReason::InvalidIdentity: return "InvalidIdentity";
    case UnifiedCoverageFailureReason::ReorderedIdentity: return "ReorderedIdentity";
    case UnifiedCoverageFailureReason::SessionBinding: return "SessionBinding";
    case UnifiedCoverageFailureReason::AmbiguousEvictedIdentity: return "AmbiguousEvictedIdentity";
    case UnifiedCoverageFailureReason::CounterOverflow: return "CounterOverflow";
    case UnifiedCoverageFailureReason::ExternalInvalidation: return "ExternalInvalidation";
    }
    return "Unknown";
}

template <typename Integer>
void WriteOptionalInteger(std::ostream& output, const std::optional<Integer> value)
{
    if (value)
    {
        output << *value;
    }
    else
    {
        output << "null";
    }
}
} // namespace

TelemetryStatus UnifiedTelemetryAccumulator::BindSession(const std::uint64_t sessionTag) noexcept
{
    if (snapshot_.sessionBound && snapshot_.sessionTag != sessionTag)
    {
        return TelemetryStatus::Failure(TelemetryError::DomainMismatch);
    }
    snapshot_.sessionBound = true;
    snapshot_.sessionTag = sessionTag;
    return {};
}

TelemetryStatus UnifiedTelemetryAccumulator::Record(const pbmodulation::UnifiedVisualObservation& observation,
    const std::uint64_t captureEpoch, const std::int64_t timestamp100ns) noexcept
{
    if (!observation.inputValid || !observation.bootstrap.IsAccepted() ||
        (!observation.IsFrameAvailable() && (observation.acceptedBlocks != 0 ||
            observation.acceptedControlRecords != 0 || observation.acceptedTransportBlocks != 0)))
    {
        InvalidateFrameCoverage(UnifiedCoverageFailureReason::InvalidSample, &observation.bootstrapRecord, captureEpoch, timestamp100ns);
        return TelemetryStatus::Failure(TelemetryError::InvalidSample);
    }
    return RecordSample(observation.bootstrapRecord, &observation, captureEpoch, timestamp100ns);
}

TelemetryStatus UnifiedTelemetryAccumulator::RecordUnavailableFrame(const pbprotocol::BootstrapRecord& bootstrap,
    const std::uint64_t captureEpoch, const std::int64_t timestamp100ns) noexcept
{
    // A valid Bootstrap still identifies an observed frame when the backend
    // supplies no data demodulation result. Do not synthesize lane/FEC samples.
    return RecordSample(bootstrap, nullptr, captureEpoch, timestamp100ns);
}

TelemetryStatus UnifiedTelemetryAccumulator::RecordSample(const pbprotocol::BootstrapRecord& bootstrap,
    const pbmodulation::UnifiedVisualObservation* const observation, const std::uint64_t captureEpoch,
    const std::int64_t timestamp100ns) noexcept
{
    if (!snapshot_.sessionBound || bootstrap.sessionTag.value != snapshot_.sessionTag || captureEpoch == 0)
    {
        return TelemetryStatus::Failure(TelemetryError::DomainMismatch);
    }
    if (timestamp100ns < 0 || (snapshot_.observationAvailable && timestamp100ns < lastObservationTimestamp100ns_))
    {
        InvalidateFrameCoverage(UnifiedCoverageFailureReason::TimestampOrder, &bootstrap, captureEpoch, timestamp100ns);
        return TelemetryStatus::Failure(TelemetryError::TimestampOrder);
    }
    // The experimental gray-state identity (layout 12) renders the same
    // manifest and slot semantics as the product profile, so its samples are
    // valid unified telemetry. Other identities (including blank-control
    // bands) stay rejected.
    const bool grayFrame = pbmodulation::IsUnifiedGrayCarrierPair(
        bootstrap.visualProfileId, bootstrap.visualLayoutVersion);
    if ((bootstrap.visualProfileId != pbprotocol::kUnifiedVisualProfileId ||
        bootstrap.visualLayoutVersion != pbmodulation::kUnifiedVisualProfile.productProfile.visualLayoutVersion) &&
        !grayFrame)
    {
        InvalidateFrameCoverage(UnifiedCoverageFailureReason::InvalidSample, &bootstrap, captureEpoch, timestamp100ns);
        return TelemetryStatus::Failure(TelemetryError::InvalidSample);
    }
    auto next = snapshot_;
    const bool frameAvailable = observation != nullptr && observation->IsFrameAvailable();
    bool valid = Add(next.observations, 1) && Add(next.frameErasedObservations, !frameAvailable);
    next.observationAvailable = true;
    if (frameAvailable)
    {
        for (std::size_t lane = 0; lane < next.lanes.size(); lane++)
        {
            const auto& metric = observation->laneMetrics[lane];
            if (!metric.available)
            {
                continue;
            }
            // Gray carries every metric on Base Luma; the SC6 lane table
            // covers only the product's three-lane split.
            const std::uint32_t expectedSamples = grayFrame ?
                (lane == 0 ? static_cast<std::uint32_t>(pbmodulation::kUnifiedGraySoftMetricCount) : 0) :
                pbmodulation::kUnifiedLaneCapacities[lane].codewordCount *
                    pbmodulation::kUnifiedVisualProfile.innerCodewordBits;
            if (metric.samples != expectedSamples || metric.zeroMetrics > metric.samples || metric.erasedMetrics > metric.zeroMetrics ||
                metric.minimumAbsoluteMetric > 32768 || metric.absoluteMetricSum > static_cast<std::uint64_t>(metric.samples) * 32768)
            {
                InvalidateFrameCoverage(UnifiedCoverageFailureReason::InvalidSample, &bootstrap, captureEpoch, timestamp100ns);
                return TelemetryStatus::Failure(TelemetryError::InvalidSample);
            }
            auto& target = next.lanes[lane];
            valid &= Add(target.metricObservations, 1) && Add(target.metricSamples, metric.samples) &&
                Add(target.zeroMetrics, metric.zeroMetrics) && Add(target.erasedMetrics, metric.erasedMetrics) &&
                Add(target.absoluteMetricSum, metric.absoluteMetricSum);
            target.minimumAbsoluteMetric = target.minimumAbsoluteMetric ?
                std::min(*target.minimumAbsoluteMetric, metric.minimumAbsoluteMetric) : metric.minimumAbsoluteMetric;
        }
        std::uint32_t accepted = 0;
        std::uint32_t controls = 0;
        // Slot accounting covers only this carrier's active prefix: fifteen
        // for SC6, eighteen for gray. The storage array is sized for the
        // larger carrier, so its default-constructed tail must never be
        // evaluated as a slot sample.
        const std::uint32_t frameSlotCount = observation->frameSlotCount;
        if (frameSlotCount != (grayFrame ? pbmodulation::kUnifiedGrayFrameCodewordCount :
            pbmodulation::kUnifiedCodewordCount))
        {
            InvalidateFrameCoverage(UnifiedCoverageFailureReason::InvalidSample, &bootstrap, captureEpoch, timestamp100ns);
            return TelemetryStatus::Failure(TelemetryError::InvalidSample);
        }
        for (std::uint32_t slot = 0; slot < frameSlotCount; slot++)
        {
            const auto& sample = observation->slotObservations[slot];
            // Gray slots all ride Base Luma; the manifest lane table only
            // covers the fifteen product slots.
            const auto* const contract = grayFrame ? nullptr :
                pbmodulation::FindUnifiedLaneForCodewordSlot(slot);
            const bool laneMismatch = grayFrame ?
                sample.lane != pbmodulation::UnifiedLane::BaseLuma :
                (contract == nullptr || sample.lane != contract->lane);
            if (laneMismatch ||
                (sample.kind != pbmodulation::UnifiedSlotKind::Control && sample.kind != pbmodulation::UnifiedSlotKind::Transport) ||
                sample.rejection > pbmodulation::UnifiedSlotRejection::IdentityFailure ||
                (sample.accepted && (!sample.fecValid || !sample.paddingValid || !sample.crcValid || !sample.identityValid ||
                    sample.rejection != pbmodulation::UnifiedSlotRejection::None)))
            {
                InvalidateFrameCoverage(UnifiedCoverageFailureReason::InvalidSample, &bootstrap, captureEpoch, timestamp100ns);
                return TelemetryStatus::Failure(TelemetryError::InvalidSample);
            }
            auto& target = next.lanes[LaneIndex(sample.lane)];
            const bool attempted = sample.rejection != pbmodulation::UnifiedSlotRejection::FrameErasure &&
                sample.rejection != pbmodulation::UnifiedSlotRejection::LaneErasure;
            valid &= Add(target.evaluatedSlots, 1) && Add(target.fecAttempts, attempted) &&
                Add(target.fecFailures, sample.rejection == pbmodulation::UnifiedSlotRejection::InnerFecFailure) &&
                Add(target.crcFailures, sample.rejection == pbmodulation::UnifiedSlotRejection::TransportCrcFailure ||
                    sample.rejection == pbmodulation::UnifiedSlotRejection::ControlCrcFailure) &&
                Add(target.identityFailures, sample.rejection == pbmodulation::UnifiedSlotRejection::IdentityFailure) &&
                Add(target.acceptedSlots, sample.accepted) && Add(target.erasedSlots, !sample.accepted) &&
                Add(target.fecIterations, sample.iterationsUsed);
            valid &= Add(next.unclassifiedSlots, !sample.fecValid ||
                (sample.kind == pbmodulation::UnifiedSlotKind::Transport && sample.rejection == pbmodulation::UnifiedSlotRejection::InvalidInformation));
            const bool control = sample.fecValid && sample.kind == pbmodulation::UnifiedSlotKind::Control;
            valid &= Add(next.classifiedControlSlots, control) && Add(next.acceptedControlSlots, control && sample.accepted);
            accepted += static_cast<std::uint32_t>(sample.accepted);
            controls += static_cast<std::uint32_t>(control && sample.accepted);
        }
        if (accepted != observation->acceptedBlocks || controls != observation->acceptedControlRecords ||
            accepted - controls != observation->acceptedTransportBlocks)
        {
            InvalidateFrameCoverage(UnifiedCoverageFailureReason::InvalidSample, &bootstrap, captureEpoch, timestamp100ns);
            return TelemetryStatus::Failure(TelemetryError::InvalidSample);
        }
    }
    const std::uint64_t sequence = bootstrap.frameSequence;
    const bool duplicate = std::find(recentSequences_.begin(), recentSequences_.begin() + recentCount_, sequence) !=
        recentSequences_.begin() + recentCount_;
    const bool unknownOldSequence = !duplicate && hasEvictedSequence_ && sequence <= maximumEvictedSequence_;
    if (unknownOldSequence)
    {
        // A very old observation may be new or evicted. Never invent a count;
        // bounded coverage loss withdraws derived performance, not reception.
        InvalidateFrameCoverage(UnifiedCoverageFailureReason::AmbiguousEvictedIdentity, &bootstrap, captureEpoch, timestamp100ns);
        next.frameCoverageComplete = false;
        next.firstCoverageFailure = snapshot_.firstCoverageFailure;
    }
    if (duplicate)
    {
        valid &= Add(next.duplicateObservations, 1);
    }
    else if (!unknownOldSequence)
    {
        valid &= Add(next.uniqueFrames, 1);
    }
    if (!valid)
    {
        snapshot_.counterOverflow = true;
        InvalidateFrameCoverage(UnifiedCoverageFailureReason::CounterOverflow, &bootstrap, captureEpoch, timestamp100ns);
        return TelemetryStatus::Failure(TelemetryError::CounterOverflow);
    }
    if (!duplicate && !unknownOldSequence)
    {
        if (snapshot_.uniqueFrames == 0)
        {
            firstUniqueTimestamp100ns_ = timestamp100ns;
        }
        lastUniqueTimestamp100ns_ = timestamp100ns;
        if (recentCount_ == recentSequences_.size())
        {
            maximumEvictedSequence_ = hasEvictedSequence_ ?
                (std::max)(maximumEvictedSequence_, recentSequences_[nextSequence_]) : recentSequences_[nextSequence_];
            hasEvictedSequence_ = true;
        }
        else
        {
            recentCount_++;
        }
        recentSequences_[nextSequence_] = sequence;
        nextSequence_ = (nextSequence_ + 1) % recentSequences_.size();
    }
    lastObservationTimestamp100ns_ = timestamp100ns;
    snapshot_ = next;
    return {};
}

void UnifiedTelemetryAccumulator::InvalidateFrameCoverage(const UnifiedCoverageFailureReason reason,
    const pbprotocol::BootstrapRecord* const bootstrap, const std::uint64_t captureEpoch,
    const std::int64_t timestamp100ns) noexcept
{
    if (!snapshot_.firstCoverageFailure)
    {
        UnifiedCoverageFailure failure;
        failure.reason = reason;
        failure.observationsBeforeFailure = snapshot_.observations;
        failure.uniqueFramesBeforeFailure = snapshot_.uniqueFrames;
        if (bootstrap != nullptr)
        {
            failure.frameSequence = bootstrap->frameSequence;
        }
        if (captureEpoch != 0)
        {
            failure.captureEpoch = captureEpoch;
        }
        if (timestamp100ns >= 0)
        {
            failure.timestamp100ns = timestamp100ns;
        }
        if (snapshot_.observationAvailable)
        {
            failure.previousTimestamp100ns = lastObservationTimestamp100ns_;
        }
        snapshot_.firstCoverageFailure = failure;
    }
    snapshot_.frameCoverageComplete = false;
}

UnifiedTelemetrySnapshot UnifiedTelemetryAccumulator::GetSnapshot() const noexcept
{
    auto result = snapshot_;
    if (result.frameCoverageComplete && !result.counterOverflow && result.uniqueFrames > 1 &&
        lastUniqueTimestamp100ns_ > firstUniqueTimestamp100ns_)
    {
        result.uniqueVisualFps = static_cast<double>(result.uniqueFrames - 1) * 10000000.0 /
            static_cast<double>(lastUniqueTimestamp100ns_ - firstUniqueTimestamp100ns_);
    }
    return result;
}

PublishedFrameMetric EvaluatePublishedFrameMetric(const UnifiedTelemetrySnapshot& telemetry,
    const std::uint64_t encodedBytes, const bool wholeDigestVerified, const bool published,
    const bool finalReopenVerified, const bool resumed) noexcept
{
    if (!wholeDigestVerified || !published || !finalReopenVerified)
    {
        return {};
    }
    if (resumed)
    {
        return {std::nullopt, "ResumeHasNoLifetimeFrameCoverage"};
    }
    if (!telemetry.sessionBound || !telemetry.observationAvailable || !telemetry.frameCoverageComplete ||
        telemetry.counterOverflow || telemetry.uniqueFrames == 0)
    {
        return {std::nullopt, "IncompleteObservedFrameCoverage"};
    }
    return {static_cast<double>(encodedBytes) / static_cast<double>(telemetry.uniqueFrames), ""};
}

void WriteUnifiedTelemetryJson(std::ostream& output, const UnifiedTelemetrySnapshot& snapshot)
{
    output << "{\"schema\":\"PixelBridge.UnifiedTelemetry.1\",\"observationAvailable\":" << (snapshot.observationAvailable ? "true" : "false")
        << ",\"frameCoverageComplete\":" << (snapshot.frameCoverageComplete ? "true" : "false")
        << ",\"counterOverflow\":" << (snapshot.counterOverflow ? "true" : "false") << ",\"firstCoverageFailure\":";
    if (snapshot.firstCoverageFailure)
    {
        const auto& failure = *snapshot.firstCoverageFailure;
        output << "{\"reason\":\"" << CoverageFailureName(failure.reason) << "\",\"observationsBeforeFailure\":" << failure.observationsBeforeFailure
            << ",\"uniqueFramesBeforeFailure\":" << failure.uniqueFramesBeforeFailure << ",\"frameSequence\":";
        WriteOptionalInteger(output, failure.frameSequence);
        output << ",\"captureEpoch\":";
        WriteOptionalInteger(output, failure.captureEpoch);
        output << ",\"timestamp100ns\":";
        WriteOptionalInteger(output, failure.timestamp100ns);
        output << ",\"previousTimestamp100ns\":";
        WriteOptionalInteger(output, failure.previousTimestamp100ns);
        output << '}';
    }
    else
    {
        output << "null";
    }
    output << ",\"observations\":" << snapshot.observations << ",\"uniqueLogicalFrames\":" << snapshot.uniqueFrames
        << ",\"frameErasedObservations\":" << snapshot.frameErasedObservations
        << ",\"duplicateObservations\":" << snapshot.duplicateObservations << ",\"uniqueVisualFps\":";
    if (snapshot.uniqueVisualFps && std::isfinite(*snapshot.uniqueVisualFps))
    {
        output << *snapshot.uniqueVisualFps;
    }
    else
    {
        output << "null";
    }
    output << ",\"uniqueVisualFpsBasis\":\"(observed distinct SessionTag/FrameSequence count-1)/(last-first unique observation time); capture epoch does not reset identity; no inferred skipped frames\""
        << ",\"laneCounterBasis\":\"actual FEC evaluations, including duplicate evaluations; never the unique-frame denominator\""
        << ",\"classifiedControlSlots\":" << snapshot.classifiedControlSlots << ",\"acceptedControlSlots\":" << snapshot.acceptedControlSlots
        << ",\"unclassifiedSlots\":" << snapshot.unclassifiedSlots << ",\"lanes\":[";
    constexpr std::array<const char*, 3> names{"BaseLuma", "FineLuma", "Chroma"};
    for (std::size_t lane = 0; lane < snapshot.lanes.size(); lane++)
    {
        if (lane != 0)
        {
            output << ',';
        }
        const auto& value = snapshot.lanes[lane];
        output << "{\"name\":\"" << names[lane] << "\",\"metrics\":";
        if (value.metricObservations == 0)
        {
            output << "null";
        }
        else
        {
            output << "{\"observations\":" << value.metricObservations << ",\"samples\":" << value.metricSamples
                << ",\"zero\":" << value.zeroMetrics << ",\"erased\":" << value.erasedMetrics
                << ",\"absoluteSum\":" << value.absoluteMetricSum << ",\"minimumAbsolute\":";
            if (value.minimumAbsoluteMetric)
            {
                output << *value.minimumAbsoluteMetric;
            }
            else
            {
                output << "null";
            }
            output << '}';
        }
        output << ",\"fec\":";
        if (value.evaluatedSlots == 0)
        {
            output << "null";
        }
        else
        {
            output << "{\"evaluatedSlots\":" << value.evaluatedSlots << ",\"attempts\":" << value.fecAttempts
                << ",\"failures\":" << value.fecFailures << ",\"crcFailures\":" << value.crcFailures
                << ",\"identityFailures\":" << value.identityFailures << ",\"accepted\":" << value.acceptedSlots
                << ",\"erased\":" << value.erasedSlots << ",\"iterations\":" << value.fecIterations << '}';
        }
        output << '}';
    }
    output << "]}";
}

} // namespace pbtelemetry
