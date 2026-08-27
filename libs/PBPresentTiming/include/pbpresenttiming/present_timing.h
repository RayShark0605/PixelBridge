#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace pbpresenttiming
{

enum class EpochReason : std::uint8_t
{
    Initial,
    Resize,
    MonitorChanged,
    DpiChanged,
    DisplayModeChanged,
    StatisticsDisjoint,
    StatisticsRecovered,
    CounterDiscontinuity,
    InvalidClock,
    Occluded,
    Restored,
    DeviceLost
};

enum class TimingState : std::uint8_t
{
    WarmingUp,
    Valid,
    Unavailable,
    Paused,
    Failed
};

enum class StatisticsStatus : std::uint8_t
{
    Valid,
    Disjoint,
    Unavailable,
    Error
};

enum class TimingIssue : std::uint8_t
{
    None,
    NoSamples,
    UnsupportedStatistics,
    StatisticsError,
    RefreshClockUnavailable,
    UnmatchedPresent,
    ObservationGap,
    HistoryOverflow,
    InvalidStatistics,
    StaleStatistics,
    InvalidClock,
    CounterOverflow,
    EpochExhausted
};

enum class PresentOutcome : std::uint8_t
{
    Success,
    Occluded,
    Failure
};

// Normalized DXGI fields. No Windows types, wall clock, inferred call count,
// or receiver/capture observations enter this value type.
struct FrameStatistics
{
    std::uint32_t presentId = 0;
    std::uint32_t presentRefreshCount = 0;
    std::uint32_t syncRefreshCount = 0;
    std::int64_t syncQpcTime = 0;
    // Preserved as raw telemetry only; never a latency or FPS timebase.
    std::int64_t syncGpuTime = 0;

    bool operator==(const FrameStatistics&) const = default;
};

struct PresentSample
{
    std::uint64_t frameSequence = 0;
    std::int64_t beginQpc = 0;
    std::int64_t endQpc = 0;
    PresentOutcome outcome = PresentOutcome::Failure;
    std::optional<std::uint32_t> presentId;
};

struct TimingSnapshot
{
    std::uint64_t presentationEpoch = 0;
    EpochReason epochReason = EpochReason::Initial;
    TimingState state = TimingState::WarmingUp;
    TimingIssue issue = TimingIssue::NoSamples;
    std::int64_t qpcFrequency = 0;
    std::int64_t epochStartQpc = 0;
    std::int64_t sampleQpc = 0;
    std::uint64_t presentCalls = 0;
    std::uint64_t successfulPresents = 0;
    std::uint64_t failedPresents = 0;
    std::uint64_t occludedPresents = 0;
    std::uint64_t observedPresents = 0;
    std::uint64_t observedUniqueVisuals = 0;
    std::uint64_t presentGlitchCount = 0;
    std::uint64_t unmatchedStatistics = 0;
    std::uint64_t historyOverflows = 0;
    std::uint64_t presentRefreshCountExtended = 0;
    std::size_t pendingHistory = 0;
    bool observationCoverageComplete = true;
    bool counterSaturated = false;
    StatisticsStatus lastStatisticsStatus = StatisticsStatus::Unavailable;
    std::int32_t lastStatisticsNativeStatus = 0;
    std::optional<FrameStatistics> lastRawStatistics;
    std::optional<PresentSample> lastPresent;
    std::optional<double> presentCallFps;
    // Exact only within the observed interval and only with complete ID
    // coverage. A gap never becomes an inferred number of visual frames.
    std::optional<double> presentedVisualFps;
    std::optional<double> observedVisualFpsLowerBound;
    // EstimatedFromDxgi: not Present call duration, capture latency, or an
    // independently measured photon/display timestamp.
    std::optional<double> presentQueueLatencyMs;
};

class PresentTimingTestAccess;

// Single-owner, allocation-free state machine. The renderer synchronizes its
// own snapshot copies. FrameSequence is monotonic within an epoch; replays
// of older sequences invalidate exact coverage, not the display itself.
class PresentTiming
{
public:
    static constexpr std::size_t kHistoryCapacity = 256;

    explicit PresentTiming(std::int64_t qpcFrequency) noexcept;
    [[nodiscard]] bool BeginEpoch(EpochReason reason, std::int64_t nowQpc) noexcept;
    // The environment owner begins a fresh epoch before resuming a pause.
    void SetPaused(bool paused) noexcept;
    void SetFailed() noexcept;
    void BreakCadence() noexcept;
    void RecordPresent(std::uint64_t frameSequence, std::int64_t beginQpc, std::int64_t endQpc, PresentOutcome outcome,
                       std::optional<std::uint32_t> presentId) noexcept;
    void ObserveStatistics(StatisticsStatus status, const FrameStatistics& statistics, std::int64_t nowQpc, std::int32_t nativeStatus = 0) noexcept;
    [[nodiscard]] TimingSnapshot GetSnapshot(std::int64_t nowQpc) const noexcept;

private:
    friend class PresentTimingTestAccess;

    struct PresentRecord
    {
        std::uint32_t presentId = 0;
        std::uint64_t frameSequence = 0;
        std::uint64_t ordinal = 0;
        std::uint64_t cadence = 0;
        std::int64_t beginQpc = 0;
    };

    void InvalidateCoverage(TimingIssue issue) noexcept;
    void IncrementCounter(std::uint64_t& counter) noexcept;
    void ObserveUnusableClock(const FrameStatistics& statistics, std::size_t matchedOffset, std::int64_t nowQpc, TimingIssue issue) noexcept;
    void ResetForBadStatistics(EpochReason reason, std::int64_t nowQpc) noexcept;
    [[nodiscard]] const PresentRecord& RecordAt(std::size_t offset) const noexcept;

    TimingSnapshot snapshot_;
    std::array<PresentRecord, kHistoryCapacity> history_{};
    std::size_t historyHead_ = 0;
    std::size_t historySize_ = 0;
    std::optional<FrameStatistics> firstObserved_;
    std::optional<FrameStatistics> lastObserved_;
    std::optional<std::uint64_t> maximumObservedSequence_;
    std::optional<std::uint32_t> lastSubmittedId_;
    std::int64_t lastPresentEndQpc_ = 0;
    std::int64_t lastObservationQpc_ = 0;
    std::uint64_t cadence_ = 0;
    std::optional<PresentRecord> cadenceAnchor_;
    std::uint32_t cadenceAnchorRefresh_ = 0;
    std::optional<double> refreshPeriodQpc_;
    bool refreshClockUnavailable_ = false;
    bool statisticsInterrupted_ = false;
};

[[nodiscard]] const char* GetTimingStateName(TimingState state) noexcept;
[[nodiscard]] const char* GetTimingIssueName(TimingIssue issue) noexcept;
[[nodiscard]] const char* GetEpochReasonName(EpochReason reason) noexcept;

}
