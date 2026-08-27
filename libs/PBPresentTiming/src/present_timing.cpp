#include "pbpresenttiming/present_timing.h"
#include "pbprotocol/checked_integer.h"

#include <cmath>
#include <limits>

namespace pbpresenttiming
{
namespace
{

[[nodiscard]] std::optional<std::uint32_t> ForwardDelta(const std::uint32_t newer, const std::uint32_t older) noexcept
{
    const std::uint32_t delta = newer - older;
    if (delta > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()))
    {
        return std::nullopt;
    }
    return delta;
}

[[nodiscard]] std::int64_t GetRefreshOffset(const FrameStatistics& statistics) noexcept
{
    // The two refresh counters refer to the same epoch. Interpret their
    // difference modulo 2^32 without implementation-defined signed casts.
    const std::uint32_t unsignedDifference = statistics.presentRefreshCount - statistics.syncRefreshCount;
    return unsignedDifference <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())
               ? static_cast<std::int64_t>(unsignedDifference)
               : static_cast<std::int64_t>(unsignedDifference) - (std::int64_t{1} << 32);
}

[[nodiscard]] double EstimateRelativeDisplayQpc(const FrameStatistics& statistics, const std::int64_t reference, const double period) noexcept
{
    // All accepted QPC values are nonnegative. Subtract integers before the
    // floating-point conversion to retain short intervals near INT64_MAX.
    return static_cast<double>(statistics.syncQpcTime - reference) + static_cast<double>(GetRefreshOffset(statistics)) * period;
}

}

PresentTiming::PresentTiming(const std::int64_t qpcFrequency) noexcept
{
    snapshot_.qpcFrequency = qpcFrequency;
    if (qpcFrequency <= 0)
    {
        snapshot_.state = TimingState::Failed;
        snapshot_.issue = TimingIssue::InvalidClock;
    }
}

bool PresentTiming::BeginEpoch(const EpochReason reason, const std::int64_t nowQpc) noexcept
{
    if (snapshot_.presentationEpoch == std::numeric_limits<std::uint64_t>::max() || snapshot_.qpcFrequency <= 0 || nowQpc < 0)
    {
        snapshot_.state = TimingState::Failed;
        snapshot_.issue = snapshot_.presentationEpoch == std::numeric_limits<std::uint64_t>::max() ? TimingIssue::EpochExhausted : TimingIssue::InvalidClock;
        return false;
    }
    const std::uint64_t nextEpoch = snapshot_.presentationEpoch + 1;
    const std::int64_t frequency = snapshot_.qpcFrequency;
    snapshot_ = TimingSnapshot{};
    snapshot_.presentationEpoch = nextEpoch;
    snapshot_.qpcFrequency = frequency;
    snapshot_.epochStartQpc = nowQpc;
    snapshot_.epochReason = reason;
    historyHead_ = 0;
    historySize_ = 0;
    firstObserved_.reset();
    lastObserved_.reset();
    maximumObservedSequence_.reset();
    lastSubmittedId_.reset();
    lastPresentEndQpc_ = nowQpc;
    lastObservationQpc_ = nowQpc;
    cadence_ = 0;
    cadenceAnchor_.reset();
    refreshPeriodQpc_.reset();
    refreshClockUnavailable_ = false;
    statisticsInterrupted_ = false;
    return true;
}

void PresentTiming::SetPaused(const bool paused) noexcept
{
    if (snapshot_.state != TimingState::Failed)
    {
        snapshot_.state = paused ? TimingState::Paused : TimingState::WarmingUp;
    }
}

void PresentTiming::SetFailed() noexcept
{
    snapshot_.state = TimingState::Failed;
}

void PresentTiming::BreakCadence() noexcept
{
    if (cadence_ == std::numeric_limits<std::uint64_t>::max())
    {
        SetFailed();
        snapshot_.issue = TimingIssue::EpochExhausted;
        return;
    }
    cadence_++;
    cadenceAnchor_.reset();
}

const PresentTiming::PresentRecord& PresentTiming::RecordAt(const std::size_t offset) const noexcept
{
    return history_[(historyHead_ + offset) % kHistoryCapacity];
}

void PresentTiming::InvalidateCoverage(const TimingIssue issue) noexcept
{
    snapshot_.observationCoverageComplete = false;
    snapshot_.issue = issue;
    snapshot_.presentedVisualFps.reset();
    snapshot_.presentQueueLatencyMs.reset();
    if (snapshot_.state == TimingState::Valid)
    {
        snapshot_.state = TimingState::Unavailable;
    }
}

void PresentTiming::IncrementCounter(std::uint64_t& counter) noexcept
{
    if (counter == std::numeric_limits<std::uint64_t>::max())
    {
        snapshot_.counterSaturated = true;
        return;
    }
    counter++;
}

void PresentTiming::ResetForBadStatistics(const EpochReason reason, const std::int64_t nowQpc) noexcept
{
    if (BeginEpoch(reason, nowQpc))
    {
        snapshot_.issue = TimingIssue::InvalidStatistics;
    }
}

void PresentTiming::ObserveUnusableClock(const FrameStatistics& statistics, const std::size_t matchedOffset, const std::int64_t nowQpc,
                                         const TimingIssue issue) noexcept
{
    refreshClockUnavailable_ = true;
    refreshPeriodQpc_.reset();
    firstObserved_.reset();
    cadenceAnchor_.reset();
    snapshot_.state = TimingState::Unavailable;
    snapshot_.issue = issue;
    snapshot_.observationCoverageComplete = false;
    snapshot_.presentQueueLatencyMs.reset();
    lastObserved_ = statistics;
    lastObservationQpc_ = nowQpc;
    historyHead_ = (historyHead_ + matchedOffset + 1) % kHistoryCapacity;
    historySize_ -= matchedOffset + 1;
}

void PresentTiming::RecordPresent(const std::uint64_t frameSequence, const std::int64_t beginQpc, const std::int64_t endQpc, const PresentOutcome outcome,
                                  const std::optional<std::uint32_t> presentId) noexcept
{
    if (snapshot_.state == TimingState::Failed || snapshot_.state == TimingState::Paused)
    {
        return;
    }
    if (beginQpc < lastPresentEndQpc_ || endQpc < beginQpc || snapshot_.presentationEpoch == 0)
    {
        ResetForBadStatistics(EpochReason::InvalidClock, endQpc);
        return;
    }
    lastPresentEndQpc_ = endQpc;
    snapshot_.lastPresent = PresentSample{frameSequence, beginQpc, endQpc, outcome, presentId};
    IncrementCounter(snapshot_.presentCalls);
    if (outcome != PresentOutcome::Success)
    {
        if (outcome == PresentOutcome::Occluded)
        {
            IncrementCounter(snapshot_.occludedPresents);
        }
        else
        {
            IncrementCounter(snapshot_.failedPresents);
        }
        BreakCadence();
        return;
    }
    IncrementCounter(snapshot_.successfulPresents);
    if (!presentId)
    {
        InvalidateCoverage(TimingIssue::UnmatchedPresent);
        return;
    }
    if (lastSubmittedId_)
    {
        const auto delta = ForwardDelta(*presentId, *lastSubmittedId_);
        if (!delta || *delta == 0)
        {
            ResetForBadStatistics(EpochReason::CounterDiscontinuity, endQpc);
            return;
        }
        if (*delta != 1)
        {
            InvalidateCoverage(TimingIssue::ObservationGap);
        }
    }
    lastSubmittedId_ = presentId;
    if (historySize_ == kHistoryCapacity)
    {
        historyHead_ = (historyHead_ + 1) % kHistoryCapacity;
        historySize_--;
        IncrementCounter(snapshot_.historyOverflows);
        InvalidateCoverage(TimingIssue::HistoryOverflow);
    }
    history_[(historyHead_ + historySize_) % kHistoryCapacity] = PresentRecord{*presentId, frameSequence, snapshot_.successfulPresents, cadence_, beginQpc};
    historySize_++;
}

void PresentTiming::ObserveStatistics(const StatisticsStatus status, const FrameStatistics& statistics, const std::int64_t nowQpc,
                                      const std::int32_t nativeStatus) noexcept
{
    if (snapshot_.state == TimingState::Failed || snapshot_.state == TimingState::Paused)
    {
        return;
    }
    const auto resetForObservedStatistics = [&](const EpochReason reason)
    {
        ResetForBadStatistics(reason, nowQpc);
        // Retain the rejected raw query for diagnostics, but never use it as
        // the new epoch's association or refresh baseline.
        snapshot_.lastStatisticsStatus = status;
        snapshot_.lastStatisticsNativeStatus = nativeStatus;
        if (status == StatisticsStatus::Valid)
        {
            snapshot_.lastRawStatistics = statistics;
        }
    };
    if (nowQpc < snapshot_.epochStartQpc || nowQpc < lastObservationQpc_)
    {
        resetForObservedStatistics(EpochReason::InvalidClock);
        return;
    }
    if (status == StatisticsStatus::Disjoint)
    {
        static_cast<void>(BeginEpoch(EpochReason::StatisticsDisjoint, nowQpc));
        snapshot_.lastStatisticsStatus = status;
        snapshot_.lastStatisticsNativeStatus = nativeStatus;
        return;
    }
    snapshot_.lastStatisticsStatus = status;
    snapshot_.lastStatisticsNativeStatus = nativeStatus;
    if (status != StatisticsStatus::Valid)
    {
        snapshot_.state = TimingState::Unavailable;
        snapshot_.issue = status == StatisticsStatus::Unavailable ? TimingIssue::UnsupportedStatistics : TimingIssue::StatisticsError;
        snapshot_.presentQueueLatencyMs.reset();
        statisticsInterrupted_ = true;
        return;
    }
    if (statisticsInterrupted_)
    {
        static_cast<void>(BeginEpoch(EpochReason::StatisticsRecovered, nowQpc));
        snapshot_.lastStatisticsStatus = status;
        snapshot_.lastRawStatistics = statistics;
        return;
    }
    snapshot_.lastRawStatistics = statistics;
    if (!lastObserved_ && statistics == FrameStatistics{})
    {
        // Before the first completed Present DXGI can succeed with an empty
        // sample. Resetting the epoch here would starve the first queued frame.
        return;
    }
    if (statistics.syncQpcTime <= 0 || statistics.syncQpcTime > nowQpc)
    {
        resetForObservedStatistics(EpochReason::InvalidClock);
        return;
    }
    if (lastObserved_ && statistics.presentId == lastObserved_->presentId)
    {
        // Holding the same image is not a new visual frame. Do not refresh
        // the freshness deadline just because a caller polled it again.
        if (statistics.presentRefreshCount != lastObserved_->presentRefreshCount || statistics.syncQpcTime < lastObserved_->syncQpcTime)
        {
            resetForObservedStatistics(EpochReason::CounterDiscontinuity);
        }
        return;
    }
    std::size_t matchedOffset = 0;
    while (matchedOffset < historySize_ && RecordAt(matchedOffset).presentId != statistics.presentId)
    {
        matchedOffset++;
    }
    if (matchedOffset == historySize_)
    {
        IncrementCounter(snapshot_.unmatchedStatistics);
        // Old-epoch statistics can arrive after a reset. They may not seed
        // a baseline, but do not make a newly created swap chain unusable.
        if (lastObserved_)
        {
            InvalidateCoverage(TimingIssue::UnmatchedPresent);
        }
        return;
    }
    const PresentRecord matched = RecordAt(matchedOffset);
    if (lastObserved_)
    {
        const auto idDelta = ForwardDelta(statistics.presentId, lastObserved_->presentId);
        const auto refreshDelta = ForwardDelta(statistics.presentRefreshCount, lastObserved_->presentRefreshCount);
        const auto syncDelta = ForwardDelta(statistics.syncRefreshCount, lastObserved_->syncRefreshCount);
        if (!idDelta || !refreshDelta || !syncDelta || statistics.syncQpcTime < lastObserved_->syncQpcTime)
        {
            resetForObservedStatistics(EpochReason::CounterDiscontinuity);
            return;
        }
        if (*refreshDelta == 0 || *syncDelta == 0 || statistics.syncQpcTime == lastObserved_->syncQpcTime)
        {
            // S_OK does not guarantee usable refresh counters. Some windowed
            // paths advance PresentCount while both refresh counters remain
            // zero. Suspend timing, not the producer's presentation epoch;
            // otherwise each sample would discard the next queued raster.
            // A real UINT32 wrap is still accepted by ForwardDelta above.
            ObserveUnusableClock(statistics, matchedOffset, nowQpc, TimingIssue::RefreshClockUnavailable);
            return;
        }
        const double period = static_cast<double>(statistics.syncQpcTime - lastObserved_->syncQpcTime) / static_cast<double>(*syncDelta);
        if (!std::isfinite(period) || period <= 0 || EstimateRelativeDisplayQpc(statistics, nowQpc, period) > 0 ||
            EstimateRelativeDisplayQpc(statistics, snapshot_.epochStartQpc, period) < 0 ||
            EstimateRelativeDisplayQpc(*lastObserved_, lastObservationQpc_, period) > 0 ||
            EstimateRelativeDisplayQpc(*lastObserved_, snapshot_.epochStartQpc, period) < 0)
        {
            ObserveUnusableClock(statistics, matchedOffset, nowQpc, TimingIssue::InvalidStatistics);
            return;
        }
        if (refreshClockUnavailable_)
        {
            static_cast<void>(BeginEpoch(EpochReason::StatisticsRecovered, nowQpc));
            snapshot_.lastStatisticsStatus = status;
            snapshot_.lastStatisticsNativeStatus = nativeStatus;
            snapshot_.lastRawStatistics = statistics;
            return;
        }
        if (*idDelta != 1 || matchedOffset != 0)
        {
            InvalidateCoverage(TimingIssue::ObservationGap);
        }
        const auto extended = pbprotocol::CheckedAddUnsigned(snapshot_.presentRefreshCountExtended, static_cast<std::uint64_t>(*refreshDelta));
        if (!extended)
        {
            resetForObservedStatistics(EpochReason::CounterDiscontinuity);
            return;
        }
        snapshot_.presentRefreshCountExtended = extended.Value();
        refreshPeriodQpc_ = period;
    }
    else
    {
        firstObserved_ = statistics;
        snapshot_.presentRefreshCountExtended = statistics.presentRefreshCount;
        // Nothing preceding the first confirmed frame belongs to the
        // measured interval. Future gaps are sticky until a new epoch.
        snapshot_.observationCoverageComplete = true;
        snapshot_.issue = TimingIssue::NoSamples;
    }
    if (cadenceAnchor_ && cadenceAnchor_->cadence == matched.cadence && matched.ordinal > cadenceAnchor_->ordinal)
    {
        const std::uint64_t ordinalDelta = matched.ordinal - cadenceAnchor_->ordinal;
        const auto actualDelta = ForwardDelta(statistics.presentRefreshCount, cadenceAnchorRefresh_);
        if (actualDelta && static_cast<std::uint64_t>(*actualDelta) > ordinalDelta)
        {
            IncrementCounter(snapshot_.presentGlitchCount);
        }
    }
    // Re-anchor after each observed frame, so one late frame is not counted
    // repeatedly as a permanent phase offset on every subsequent frame.
    cadenceAnchor_ = matched;
    cadenceAnchorRefresh_ = statistics.presentRefreshCount;
    snapshot_.presentQueueLatencyMs.reset();
    if (refreshPeriodQpc_ && snapshot_.observationCoverageComplete)
    {
        const double latencyQpc = EstimateRelativeDisplayQpc(statistics, matched.beginQpc, *refreshPeriodQpc_);
        if (std::isfinite(latencyQpc) && latencyQpc >= 0)
        {
            snapshot_.presentQueueLatencyMs = latencyQpc * 1000.0 / static_cast<double>(snapshot_.qpcFrequency);
        }
    }
    IncrementCounter(snapshot_.observedPresents);
    if (!maximumObservedSequence_ || matched.frameSequence > *maximumObservedSequence_)
    {
        IncrementCounter(snapshot_.observedUniqueVisuals);
        maximumObservedSequence_ = matched.frameSequence;
    }
    else if (matched.frameSequence < *maximumObservedSequence_)
    {
        InvalidateCoverage(TimingIssue::ObservationGap);
    }
    lastObserved_ = statistics;
    lastObservationQpc_ = nowQpc;
    historyHead_ = (historyHead_ + matchedOffset + 1) % kHistoryCapacity;
    historySize_ -= matchedOffset + 1;
    if (snapshot_.observedPresents >= 2 && refreshPeriodQpc_ && snapshot_.observationCoverageComplete)
    {
        snapshot_.state = TimingState::Valid;
        snapshot_.issue = TimingIssue::None;
    }
    else
    {
        snapshot_.state = snapshot_.observationCoverageComplete ? TimingState::WarmingUp : TimingState::Unavailable;
    }
}

TimingSnapshot PresentTiming::GetSnapshot(const std::int64_t nowQpc) const noexcept
{
    TimingSnapshot result = snapshot_;
    result.sampleQpc = nowQpc;
    result.pendingHistory = historySize_;
    if (nowQpc < result.epochStartQpc || nowQpc < lastObservationQpc_ || nowQpc < lastPresentEndQpc_ || result.qpcFrequency <= 0)
    {
        result.state = TimingState::Unavailable;
        result.issue = TimingIssue::InvalidClock;
        if (snapshot_.state == TimingState::Failed)
        {
            result.state = TimingState::Failed;
            result.issue = snapshot_.issue;
        }
        result.presentQueueLatencyMs.reset();
        return result;
    }
    if (result.counterSaturated)
    {
        if (result.state != TimingState::Failed && result.state != TimingState::Paused)
        {
            result.state = TimingState::Unavailable;
            result.issue = TimingIssue::CounterOverflow;
        }
        result.presentQueueLatencyMs.reset();
        return result;
    }
    if (nowQpc > result.epochStartQpc)
    {
        result.presentCallFps =
            static_cast<double>(result.presentCalls) * static_cast<double>(result.qpcFrequency) / static_cast<double>(nowQpc - result.epochStartQpc);
    }
    if (lastObserved_ && nowQpc >= lastObservationQpc_ && nowQpc - lastObservationQpc_ > result.qpcFrequency && result.state != TimingState::Paused &&
        result.state != TimingState::Failed)
    {
        result.state = TimingState::Unavailable;
        result.issue = TimingIssue::StaleStatistics;
        result.presentQueueLatencyMs.reset();
    }
    if (result.state == TimingState::Paused || result.state == TimingState::Failed || result.issue == TimingIssue::StaleStatistics ||
        result.lastStatisticsStatus != StatisticsStatus::Valid)
    {
        result.presentQueueLatencyMs.reset();
        return result;
    }
    if (firstObserved_ && lastObserved_ && refreshPeriodQpc_ && result.observedPresents >= 2)
    {
        const double elapsedQpc = static_cast<double>(lastObserved_->syncQpcTime - firstObserved_->syncQpcTime) +
                                  static_cast<double>(GetRefreshOffset(*lastObserved_) - GetRefreshOffset(*firstObserved_)) * *refreshPeriodQpc_;
        if (std::isfinite(elapsedQpc) && elapsedQpc > 0 && result.observedUniqueVisuals > 0)
        {
            const double fps = static_cast<double>(result.observedUniqueVisuals - 1) * static_cast<double>(result.qpcFrequency) / elapsedQpc;
            result.observedVisualFpsLowerBound = fps;
            if (result.state == TimingState::Valid && result.observationCoverageComplete)
            {
                result.presentedVisualFps = fps;
            }
        }
    }
    return result;
}

const char* GetTimingStateName(const TimingState state) noexcept
{
    switch (state)
    {
    case TimingState::WarmingUp:
        return "warming-up";
    case TimingState::Valid:
        return "valid-dxgi-observation";
    case TimingState::Unavailable:
        return "unavailable";
    case TimingState::Paused:
        return "paused";
    case TimingState::Failed:
        return "failed";
    }
    return "invalid";
}

const char* GetTimingIssueName(const TimingIssue issue) noexcept
{
    switch (issue)
    {
    case TimingIssue::None:
        return "none";
    case TimingIssue::NoSamples:
        return "no-samples";
    case TimingIssue::UnsupportedStatistics:
        return "statistics-unavailable";
    case TimingIssue::StatisticsError:
        return "statistics-error";
    case TimingIssue::RefreshClockUnavailable:
        return "refresh-clock-unavailable";
    case TimingIssue::UnmatchedPresent:
        return "unmatched-present";
    case TimingIssue::ObservationGap:
        return "observation-gap";
    case TimingIssue::HistoryOverflow:
        return "history-overflow";
    case TimingIssue::InvalidStatistics:
        return "invalid-statistics";
    case TimingIssue::StaleStatistics:
        return "stale-statistics";
    case TimingIssue::InvalidClock:
        return "invalid-clock";
    case TimingIssue::CounterOverflow:
        return "counter-overflow";
    case TimingIssue::EpochExhausted:
        return "epoch-exhausted";
    }
    return "invalid";
}

const char* GetEpochReasonName(const EpochReason reason) noexcept
{
    switch (reason)
    {
    case EpochReason::Initial:
        return "initial";
    case EpochReason::Resize:
        return "resize";
    case EpochReason::MonitorChanged:
        return "monitor-changed";
    case EpochReason::DpiChanged:
        return "dpi-changed";
    case EpochReason::DisplayModeChanged:
        return "display-mode-changed";
    case EpochReason::StatisticsDisjoint:
        return "statistics-disjoint";
    case EpochReason::StatisticsRecovered:
        return "statistics-recovered";
    case EpochReason::CounterDiscontinuity:
        return "counter-discontinuity";
    case EpochReason::InvalidClock:
        return "invalid-clock";
    case EpochReason::Occluded:
        return "occluded";
    case EpochReason::Restored:
        return "restored";
    case EpochReason::DeviceLost:
        return "device-lost";
    }
    return "invalid";
}

}
