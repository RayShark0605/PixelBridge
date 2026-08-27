#include "pbpresenttiming/present_timing.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

namespace pbpresenttiming
{
class PresentTimingTestAccess
{
public:
    static void SetEpoch(PresentTiming& timing, const std::uint64_t epoch)
    {
        timing.snapshot_.presentationEpoch = epoch;
    }
    static void SetExtendedRefresh(PresentTiming& timing, const std::uint64_t count)
    {
        timing.snapshot_.presentRefreshCountExtended = count;
    }
    static void SetCalls(PresentTiming& timing, const std::uint64_t count)
    {
        timing.snapshot_.presentCalls = count;
    }
};
}

namespace
{
using namespace pbpresenttiming;

void Submit(PresentTiming& timing, const std::uint32_t presentId, const std::uint64_t sequence, const std::int64_t begin)
{
    timing.RecordPresent(sequence, begin, begin + 1, PresentOutcome::Success, presentId);
}

void Observe(PresentTiming& timing, const std::uint32_t presentId, const std::uint32_t refresh, const std::int64_t qpc)
{
    timing.ObserveStatistics(StatisticsStatus::Valid, FrameStatistics{presentId, refresh, refresh, qpc}, qpc + 1);
}

void Seed(PresentTiming& timing)
{
    REQUIRE(timing.BeginEpoch(EpochReason::Initial, 0));
    Submit(timing, 1, 10, 90);
    Observe(timing, 1, 10, 100);
}

}

TEST_CASE("Timing distinguishes calls from confirmed visuals with an independent clock oracle")
{
    PresentTiming timing(1000);
    Seed(timing);
    REQUIRE(timing.GetSnapshot(101).state == TimingState::WarmingUp);
    REQUIRE_FALSE(timing.GetSnapshot(101).presentedVisualFps);
    Submit(timing, 2, 11, 190);
    Observe(timing, 2, 11, 200);
    const auto snapshot = timing.GetSnapshot(250);
    REQUIRE(snapshot.state == TimingState::Valid);
    REQUIRE(snapshot.presentCallFps == Catch::Approx(8.0));
    REQUIRE(snapshot.presentedVisualFps == Catch::Approx(10.0));
    REQUIRE(snapshot.presentQueueLatencyMs == Catch::Approx(10.0));
    REQUIRE(snapshot.observedUniqueVisuals == 2);
    REQUIRE(snapshot.presentRefreshCountExtended == 11);
    REQUIRE(snapshot.pendingHistory == 0);
}

TEST_CASE("Repeated frame identities and repeated statistics cannot inflate unique visuals or glitches")
{
    PresentTiming timing(1000);
    Seed(timing);
    Submit(timing, 2, 10, 290);
    Observe(timing, 2, 12, 300);
    for (int index = 0; index < 30; index++)
    {
        Observe(timing, 2, 12, 300);
    }
    const auto snapshot = timing.GetSnapshot(350);
    REQUIRE(snapshot.observedPresents == 2);
    REQUIRE(snapshot.observedUniqueVisuals == 1);
    REQUIRE(snapshot.presentedVisualFps == Catch::Approx(0.0));
    REQUIRE(snapshot.presentGlitchCount == 1);
    Submit(timing, 3, 11, 390);
    Observe(timing, 3, 13, 400);
    REQUIRE(timing.GetSnapshot(401).presentGlitchCount == 1);
}

TEST_CASE("A statistics gap is only a lower bound and is never reconstructed from ID subtraction")
{
    PresentTiming timing(1000);
    Seed(timing);
    Submit(timing, 2, 11, 190);
    Submit(timing, 3, 12, 290);
    Observe(timing, 3, 12, 300);
    const auto snapshot = timing.GetSnapshot(301);
    REQUIRE(snapshot.presentCalls == 3);
    REQUIRE(snapshot.observedPresents == 2);
    REQUIRE(snapshot.observedUniqueVisuals == 2);
    REQUIRE_FALSE(snapshot.presentedVisualFps);
    REQUIRE(snapshot.observedVisualFpsLowerBound == Catch::Approx(5.0));
    REQUIRE_FALSE(snapshot.presentQueueLatencyMs);
    REQUIRE(snapshot.issue == TimingIssue::ObservationGap);
    REQUIRE_FALSE(snapshot.observationCoverageComplete);
    Submit(timing, 4, 13, 390);
    Observe(timing, 4, 13, 400);
    REQUIRE_FALSE(timing.GetSnapshot(401).presentedVisualFps);
}

TEST_CASE("Disjoint starts a fresh epoch without retaining pending associations")
{
    PresentTiming timing(1000);
    Seed(timing);
    Submit(timing, 2, 11, 190);
    timing.ObserveStatistics(StatisticsStatus::Disjoint, {}, 200, -123);
    auto snapshot = timing.GetSnapshot(201);
    REQUIRE(snapshot.presentationEpoch == 2);
    REQUIRE(snapshot.epochReason == EpochReason::StatisticsDisjoint);
    REQUIRE(snapshot.presentCalls == 0);
    REQUIRE(snapshot.pendingHistory == 0);
    REQUIRE(snapshot.lastStatisticsNativeStatus == -123);
    Observe(timing, 2, 11, 200);
    REQUIRE(timing.GetSnapshot(202).observedPresents == 0);
    Submit(timing, 3, 12, 290);
    Observe(timing, 3, 12, 300);
    REQUIRE_FALSE(timing.GetSnapshot(301).presentedVisualFps);
    Submit(timing, 4, 13, 390);
    Observe(timing, 4, 13, 400);
    REQUIRE(timing.GetSnapshot(401).presentedVisualFps == Catch::Approx(10.0));
    timing.ObserveStatistics(StatisticsStatus::Disjoint, {}, 500);
    timing.ObserveStatistics(StatisticsStatus::Disjoint, {}, 600);
    REQUIRE(timing.GetSnapshot(601).presentationEpoch == 4);
}

TEST_CASE("Unavailable statistics keep call diagnostics but cannot provide certified timing")
{
    PresentTiming timing(1000);
    Seed(timing);
    for (const auto status : {StatisticsStatus::Unavailable, StatisticsStatus::Error})
    {
        timing.ObserveStatistics(status, {}, 150, -1);
        const auto snapshot = timing.GetSnapshot(150);
        REQUIRE(snapshot.state == TimingState::Unavailable);
        REQUIRE(snapshot.presentCallFps);
        REQUIRE_FALSE(snapshot.presentedVisualFps);
        REQUIRE_FALSE(snapshot.presentQueueLatencyMs);
    }
    Observe(timing, 1, 10, 200);
    REQUIRE(timing.GetSnapshot(201).epochReason == EpochReason::StatisticsRecovered);
    Submit(timing, 2, 11, 290);
    Observe(timing, 2, 12, 300);
    Submit(timing, 3, 12, 390);
    Observe(timing, 3, 13, 400);
    REQUIRE(timing.GetSnapshot(401).state == TimingState::Valid);
}

TEST_CASE("Stale statistics are not kept alive by repeated polling")
{
    PresentTiming timing(1000);
    Seed(timing);
    Submit(timing, 2, 11, 190);
    Observe(timing, 2, 11, 200);
    timing.ObserveStatistics(StatisticsStatus::Valid, {2, 11, 11, 200}, 1300);
    const auto snapshot = timing.GetSnapshot(1301);
    REQUIRE(snapshot.issue == TimingIssue::StaleStatistics);
    REQUIRE_FALSE(snapshot.presentedVisualFps);
    REQUIRE_FALSE(snapshot.presentQueueLatencyMs);
    REQUIRE_FALSE(snapshot.observedVisualFpsLowerBound);
}

TEST_CASE("UINT32 counters wrap forward but resets and pathological jumps cannot produce enormous FPS")
{
    const auto maximum = std::numeric_limits<std::uint32_t>::max();
    PresentTiming timing(1000);
    REQUIRE(timing.BeginEpoch(EpochReason::Initial, 0));
    Submit(timing, maximum, 100, 90);
    Observe(timing, maximum, maximum, 100);
    Submit(timing, 0, 101, 190);
    Observe(timing, 0, 0, 200);
    auto snapshot = timing.GetSnapshot(201);
    REQUIRE(snapshot.presentedVisualFps == Catch::Approx(10.0));
    REQUIRE(snapshot.presentRefreshCountExtended == std::uint64_t{maximum} + 1);
    Submit(timing, 1, 102, 290);
    Observe(timing, 1, maximum, 300);
    snapshot = timing.GetSnapshot(301);
    REQUIRE(snapshot.epochReason == EpochReason::CounterDiscontinuity);
    REQUIRE(snapshot.presentationEpoch == 2);
    REQUIRE_FALSE(snapshot.presentedVisualFps);
    REQUIRE(snapshot.lastRawStatistics == FrameStatistics{1, maximum, maximum, 300});
    REQUIRE(snapshot.observedPresents == 0);
}

TEST_CASE("Timing rejects malformed clocks and statistics without divisions by zero")
{
    SECTION("Invalid frequency and negative epoch")
    {
        PresentTiming timing(0);
        REQUIRE_FALSE(timing.BeginEpoch(EpochReason::Initial, 0));
        REQUIRE(timing.GetSnapshot(100).state == TimingState::Failed);
        PresentTiming negative(1000);
        REQUIRE_FALSE(negative.BeginEpoch(EpochReason::Initial, -1));
    }
    SECTION("Future, zero, reversed and equal clocks")
    {
        for (const auto qpc : {std::int64_t{0}, std::int64_t{-1}, std::int64_t{1000}})
        {
            PresentTiming timing(1000);
            Seed(timing);
            Submit(timing, 2, 11, 190);
            timing.ObserveStatistics(StatisticsStatus::Valid, {2, 11, 11, qpc}, 201);
            REQUIRE(timing.GetSnapshot(202).presentationEpoch == 2);
            REQUIRE_FALSE(timing.GetSnapshot(202).presentedVisualFps);
        }
    }
    SECTION("Backwards Present clock")
    {
        PresentTiming timing(1000);
        Seed(timing);
        timing.RecordPresent(11, 80, 79, PresentOutcome::Success, 2);
        REQUIRE(timing.GetSnapshot(202).epochReason == EpochReason::InvalidClock);
    }
    SECTION("Same refresh counter for a different Present")
    {
        PresentTiming timing(1000);
        Seed(timing);
        Submit(timing, 2, 11, 190);
        Observe(timing, 2, 10, 200);
        const auto snapshot = timing.GetSnapshot(201);
        REQUIRE(snapshot.presentationEpoch == 1);
        REQUIRE(snapshot.state == TimingState::Unavailable);
        REQUIRE(snapshot.issue == TimingIssue::RefreshClockUnavailable);
        REQUIRE_FALSE(snapshot.presentedVisualFps);
        REQUIRE_FALSE(snapshot.observedVisualFpsLowerBound);
        REQUIRE_FALSE(snapshot.presentQueueLatencyMs);
    }
}

TEST_CASE("Successful DXGI queries with nonadvancing refresh counters do not starve presentation")
{
    PresentTiming timing(1000);
    REQUIRE(timing.BeginEpoch(EpochReason::Initial, 0));
    timing.ObserveStatistics(StatisticsStatus::Valid, {}, 1);
    REQUIRE(timing.GetSnapshot(2).presentationEpoch == 1);
    for (std::uint32_t index = 1; index <= 600; index++)
    {
        const std::int64_t now = static_cast<std::int64_t>(index) * 100;
        Submit(timing, index, index, now - 10);
        timing.ObserveStatistics(StatisticsStatus::Valid, {index, 0, 0, now}, now + 1);
        const auto snapshot = timing.GetSnapshot(now + 2);
        REQUIRE(snapshot.presentationEpoch == 1);
        REQUIRE(snapshot.presentCalls == index);
        REQUIRE(snapshot.pendingHistory == 0);
        REQUIRE(snapshot.historyOverflows == 0);
        REQUIRE_FALSE(snapshot.presentedVisualFps);
        REQUIRE_FALSE(snapshot.observedVisualFpsLowerBound);
        REQUIRE_FALSE(snapshot.presentQueueLatencyMs);
    }
    REQUIRE(timing.GetSnapshot(60002).issue == TimingIssue::RefreshClockUnavailable);
    Submit(timing, 601, 601, 60090);
    Observe(timing, 601, 1, 60100);
    REQUIRE(timing.GetSnapshot(60101).presentationEpoch == 2);
    REQUIRE(timing.GetSnapshot(60101).epochReason == EpochReason::StatisticsRecovered);
    REQUIRE_FALSE(timing.GetSnapshot(60101).presentedVisualFps);
    Submit(timing, 602, 602, 60190);
    Observe(timing, 602, 2, 60200);
    Submit(timing, 603, 603, 60290);
    Observe(timing, 603, 3, 60300);
    REQUIRE(timing.GetSnapshot(60301).presentedVisualFps == Catch::Approx(10.0));
    const auto reversed = timing.GetSnapshot(60200);
    REQUIRE(reversed.issue == TimingIssue::InvalidClock);
    REQUIRE_FALSE(reversed.presentedVisualFps);
    REQUIRE_FALSE(reversed.presentQueueLatencyMs);
}

TEST_CASE("History storage is bounded and recovers only from a fresh observed interval")
{
    PresentTiming timing(1000);
    REQUIRE(timing.BeginEpoch(EpochReason::Initial, 0));
    for (std::uint32_t index = 0; index < PresentTiming::kHistoryCapacity + 1; index++)
    {
        Submit(timing, index, index, static_cast<std::int64_t>(index) * 2);
    }
    const auto snapshot = timing.GetSnapshot(1000);
    REQUIRE(snapshot.pendingHistory == PresentTiming::kHistoryCapacity);
    REQUIRE(snapshot.historyOverflows == 1);
    REQUIRE_FALSE(snapshot.presentedVisualFps);
    Observe(timing, 0, 1, 600);
    REQUIRE(timing.GetSnapshot(601).observedPresents == 0);
    Observe(timing, 255, 10, 700);
    Observe(timing, 256, 11, 800);
    REQUIRE(timing.GetSnapshot(801).presentedVisualFps == Catch::Approx(10.0));
}

TEST_CASE("Epoch exhaustion fails closed and telemetry counters saturate")
{
    PresentTiming timing(1000);
    Seed(timing);
    PresentTimingTestAccess::SetCalls(timing, std::numeric_limits<std::uint64_t>::max());
    Submit(timing, 2, 11, 190);
    REQUIRE(timing.GetSnapshot(200).presentCalls == std::numeric_limits<std::uint64_t>::max());
    REQUIRE(timing.GetSnapshot(200).counterSaturated);
    REQUIRE(timing.GetSnapshot(200).issue == TimingIssue::CounterOverflow);
    REQUIRE_FALSE(timing.GetSnapshot(200).presentCallFps);
    PresentTimingTestAccess::SetExtendedRefresh(timing, std::numeric_limits<std::uint64_t>::max());
    Observe(timing, 2, 11, 200);
    REQUIRE(timing.GetSnapshot(201).epochReason == EpochReason::CounterDiscontinuity);
    PresentTimingTestAccess::SetEpoch(timing, std::numeric_limits<std::uint64_t>::max());
    REQUIRE_FALSE(timing.BeginEpoch(EpochReason::Resize, 300));
    REQUIRE(timing.GetSnapshot(301).state == TimingState::Failed);
    REQUIRE(timing.GetSnapshot(301).issue == TimingIssue::EpochExhausted);
}

TEST_CASE("Estimated display timestamps use SyncQPC and refresh differences, not Present duration")
{
    PresentTiming timing(1000);
    REQUIRE(timing.BeginEpoch(EpochReason::Initial, 0));
    timing.RecordPresent(10, 290, 350, PresentOutcome::Success, 1);
    timing.ObserveStatistics(StatisticsStatus::Valid, {1, 12, 10, 100}, 351);
    timing.RecordPresent(11, 390, 395, PresentOutcome::Success, 2);
    timing.ObserveStatistics(StatisticsStatus::Valid, {2, 13, 11, 200}, 401);
    auto snapshot = timing.GetSnapshot(402);
    REQUIRE(snapshot.presentedVisualFps == Catch::Approx(10.0));
    REQUIRE(snapshot.presentQueueLatencyMs == Catch::Approx(10.0));
    timing.RecordPresent(12, 500, 510, PresentOutcome::Success, 3);
    timing.ObserveStatistics(StatisticsStatus::Valid, {3, 14, 12, 300}, 511);
    REQUIRE(timing.GetSnapshot(512).presentQueueLatencyMs == Catch::Approx(0.0));
    timing.RecordPresent(13, 601, 610, PresentOutcome::Success, 4);
    timing.ObserveStatistics(StatisticsStatus::Valid, {4, 15, 13, 400}, 611);
    snapshot = timing.GetSnapshot(612);
    REQUIRE_FALSE(snapshot.presentQueueLatencyMs);
    REQUIRE(snapshot.presentedVisualFps == Catch::Approx(10.0));
}

TEST_CASE("Ambiguous half-range jumps and backwards frame identities invalidate exact observations")
{
    SECTION("Present IDs cannot jump beyond the modular half range")
    {
        PresentTiming timing(1000);
        Seed(timing);
        Submit(timing, 0x80000002u, 11, 190);
        REQUIRE(timing.GetSnapshot(201).epochReason == EpochReason::CounterDiscontinuity);
        REQUIRE_FALSE(timing.GetSnapshot(201).presentedVisualFps);
    }
    SECTION("Sync counters cannot jump beyond the modular half range")
    {
        PresentTiming timing(1000);
        Seed(timing);
        Submit(timing, 2, 11, 190);
        timing.ObserveStatistics(StatisticsStatus::Valid, {2, 11, 0x8000000bu, 200}, 201);
        REQUIRE(timing.GetSnapshot(202).epochReason == EpochReason::CounterDiscontinuity);
    }
    SECTION("Older FrameSequence cannot masquerade as a new unique frame")
    {
        PresentTiming timing(1000);
        Seed(timing);
        Submit(timing, 2, 9, 190);
        Observe(timing, 2, 11, 200);
        const auto snapshot = timing.GetSnapshot(201);
        REQUIRE(snapshot.observedUniqueVisuals == 1);
        REQUIRE_FALSE(snapshot.presentedVisualFps);
        REQUIRE(snapshot.observedVisualFpsLowerBound == Catch::Approx(0.0));
    }
    SECTION("Idle cadence does not count an intentionally held image as a glitch")
    {
        PresentTiming timing(1000);
        Seed(timing);
        timing.BreakCadence();
        Submit(timing, 2, 11, 990);
        Observe(timing, 2, 19, 1000);
        REQUIRE(timing.GetSnapshot(1001).presentGlitchCount == 0);
    }
}

TEST_CASE("Large absolute QPC timestamps retain short relative intervals without precision loss")
{
    PresentTiming timing(1000);
    const std::int64_t base = std::numeric_limits<std::int64_t>::max() - 1000;
    REQUIRE(timing.BeginEpoch(EpochReason::Initial, base));
    Submit(timing, 1, 1, base + 90);
    Observe(timing, 1, 10, base + 100);
    Submit(timing, 2, 2, base + 190);
    Observe(timing, 2, 11, base + 200);
    const auto snapshot = timing.GetSnapshot(base + 250);
    REQUIRE(snapshot.presentCallFps == Catch::Approx(8.0));
    REQUIRE(snapshot.presentedVisualFps == Catch::Approx(10.0));
    REQUIRE(snapshot.presentQueueLatencyMs == Catch::Approx(10.0));
}

TEST_CASE("A future or pre-epoch inferred display time is unavailable, not a fabricated visual rate")
{
    for (const std::uint32_t refresh : {0u, 100u})
    {
        PresentTiming timing(1000);
        REQUIRE(timing.BeginEpoch(EpochReason::Initial, 50));
        Submit(timing, 1, 1, 90);
        timing.ObserveStatistics(StatisticsStatus::Valid, {1, refresh, 10, 100}, 101);
        Submit(timing, 2, 2, 190);
        timing.ObserveStatistics(StatisticsStatus::Valid, {2, refresh + 1, 11, 200}, 201);
        const auto snapshot = timing.GetSnapshot(202);
        REQUIRE(snapshot.presentationEpoch == 1);
        REQUIRE(snapshot.issue == TimingIssue::InvalidStatistics);
        REQUIRE(snapshot.presentCalls == 2);
        REQUIRE_FALSE(snapshot.presentedVisualFps);
        REQUIRE_FALSE(snapshot.observedVisualFpsLowerBound);
        REQUIRE_FALSE(snapshot.presentQueueLatencyMs);
    }
}

TEST_CASE("Occluded and failed calls are diagnostics, not visual frames")
{
    PresentTiming timing(1000);
    Seed(timing);
    timing.RecordPresent(11, 190, 191, PresentOutcome::Occluded, 2);
    timing.RecordPresent(12, 290, 291, PresentOutcome::Failure, 3);
    const auto snapshot = timing.GetSnapshot(300);
    REQUIRE(snapshot.presentCalls == 3);
    REQUIRE(snapshot.successfulPresents == 1);
    REQUIRE(snapshot.occludedPresents == 1);
    REQUIRE(snapshot.failedPresents == 1);
    REQUIRE(snapshot.observedPresents == 1);
    REQUIRE_FALSE(snapshot.presentedVisualFps);
}

TEST_CASE("Every environmental epoch discards old associations and paused states mask measurements")
{
    for (const auto reason : {EpochReason::Resize, EpochReason::MonitorChanged, EpochReason::DpiChanged, EpochReason::DisplayModeChanged, EpochReason::Occluded,
                              EpochReason::Restored, EpochReason::DeviceLost})
    {
        PresentTiming timing(1000);
        Seed(timing);
        REQUIRE(timing.BeginEpoch(reason, 200));
        REQUIRE(timing.GetSnapshot(201).pendingHistory == 0);
        REQUIRE(timing.GetSnapshot(201).observedUniqueVisuals == 0);
        timing.SetPaused(true);
        Submit(timing, 2, 11, 290);
        Observe(timing, 2, 12, 300);
        REQUIRE(timing.GetSnapshot(301).presentCalls == 0);
        REQUIRE(timing.GetSnapshot(301).state == TimingState::Paused);
        timing.SetFailed();
        timing.SetPaused(false);
        REQUIRE(timing.GetSnapshot(400).state == TimingState::Failed);
    }
}

TEST_CASE("A fixed-seed adversarial timing stream remains bounded and finite")
{
    PresentTiming timing(1000000);
    REQUIRE(timing.BeginEpoch(EpochReason::Initial, 0));
    std::uint32_t random = 0xA1745201u;
    for (std::uint32_t index = 1; index <= 10000; index++)
    {
        random = random * 1664525u + 1013904223u;
        const std::int64_t now = static_cast<std::int64_t>(index) * 10000;
        Submit(timing, index, random % 400, now);
        if ((random & 15u) == 0)
        {
            timing.ObserveStatistics(StatisticsStatus::Disjoint, {}, now + 10);
        }
        else
        {
            timing.ObserveStatistics(StatisticsStatus::Valid, {index - (random % 3), random, random, now + 5}, now + 10);
        }
        const auto snapshot = timing.GetSnapshot(now + 20);
        REQUIRE(snapshot.pendingHistory <= PresentTiming::kHistoryCapacity);
        for (const auto metric : {snapshot.presentCallFps, snapshot.presentedVisualFps, snapshot.observedVisualFpsLowerBound, snapshot.presentQueueLatencyMs})
        {
            if (metric)
            {
                REQUIRE(std::isfinite(*metric));
                REQUIRE(*metric >= 0);
            }
        }
    }
}
