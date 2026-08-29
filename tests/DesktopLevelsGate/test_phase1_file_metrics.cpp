#include "phase1_file_metrics.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

TEST_CASE("Phase1 sender prepares the next bounded frame while the copied pending slot presents",
    "[phase1][file-gate][sender-cadence]")
{
    REQUIRE(phase1gate::GetSenderPreparationDecision(true, false, true) ==
        phase1gate::SenderPreparationDecision{true, false});
    REQUIRE(phase1gate::GetSenderPreparationDecision(true, true, true) ==
        phase1gate::SenderPreparationDecision{false, false});
    REQUIRE(phase1gate::GetSenderPreparationDecision(true, false, false) ==
        phase1gate::SenderPreparationDecision{true, true});
    REQUIRE(phase1gate::GetSenderPreparationDecision(true, true, false) ==
        phase1gate::SenderPreparationDecision{false, true});
    REQUIRE(phase1gate::GetSenderPreparationDecision(false, false, false) ==
        phase1gate::SenderPreparationDecision{false, false});
}

TEST_CASE("Phase1 unique visual cadence excludes duplicates gaps and CaptureEpoch recovery pauses",
    "[phase1][file-gate][unique-visual-fps]")
{
    phase1gate::UniqueVisualRateAccumulator accumulator;
    phase1gate::VisualIdentityDisposition disposition = phase1gate::VisualIdentityDisposition::Reordered;
    REQUIRE(accumulator.Observe(10, 1, 1'000'000, disposition));
    REQUIRE(disposition == phase1gate::VisualIdentityDisposition::Unique);
    REQUIRE(accumulator.Observe(11, 1, 1'166'667, disposition));
    REQUIRE(disposition == phase1gate::VisualIdentityDisposition::Unique);
    REQUIRE(accumulator.Observe(11, 1, 1'200'000, disposition));
    REQUIRE(disposition == phase1gate::VisualIdentityDisposition::Duplicate);
    REQUIRE(accumulator.Observe(9, 1, 1'250'000, disposition));
    REQUIRE(disposition == phase1gate::VisualIdentityDisposition::Reordered);
    REQUIRE(accumulator.Observe(15, 1, 1'833'335, disposition));
    REQUIRE(accumulator.Observe(16, 1, 2'000'002, disposition));
    REQUIRE(accumulator.Observe(20, 2, 100, disposition));
    REQUIRE(accumulator.Observe(21, 2, 166'767, disposition));

    const auto snapshot = accumulator.GetSnapshot();
    REQUIRE(snapshot.uniqueFrames == 6);
    REQUIRE(snapshot.duplicateFrames == 1);
    REQUIRE(snapshot.reorderedFrames == 1);
    REQUIRE(snapshot.cadenceIntervals == 3);
    REQUIRE(snapshot.cadenceTime100ns == 500'001);
    REQUIRE(snapshot.gapEvents == 1);
    REQUIRE(snapshot.skippedSequences == 3);
    REQUIRE(snapshot.captureEpochBoundaries == 1);
    REQUIRE(std::abs(accumulator.GetCadenceFps() - 59.99988000024) < 0.000001);
}

TEST_CASE("Phase1 unique visual cadence rejects invalid timestamps and preserves state atomically",
    "[phase1][file-gate][unique-visual-fps][errors]")
{
    phase1gate::UniqueVisualRateAccumulator accumulator;
    phase1gate::VisualIdentityDisposition disposition = phase1gate::VisualIdentityDisposition::Unique;
    REQUIRE(accumulator.Observe(7, 4, 500, disposition));
    const auto saved = accumulator.GetSnapshot();
    disposition = phase1gate::VisualIdentityDisposition::Reordered;
    REQUIRE_FALSE(accumulator.Observe(8, 4, 500, disposition));
    REQUIRE(accumulator.GetSnapshot() == saved);
    REQUIRE(disposition == phase1gate::VisualIdentityDisposition::Reordered);
    REQUIRE_FALSE(accumulator.Observe(8, 0, 600, disposition));
    REQUIRE(accumulator.GetSnapshot() == saved);
    REQUIRE_FALSE(accumulator.Observe(8, 4, -1, disposition));
    REQUIRE(accumulator.GetSnapshot() == saved);
    REQUIRE_FALSE(accumulator.Observe(9, 3, 700, disposition));
    REQUIRE(accumulator.GetSnapshot() == saved);
    REQUIRE(accumulator.Observe(8, 5, 1, disposition));
    REQUIRE(disposition == phase1gate::VisualIdentityDisposition::Unique);
    REQUIRE(accumulator.GetSnapshot().captureEpochBoundaries == 1);
    REQUIRE(accumulator.GetCadenceFps() == 0);
}

TEST_CASE("Phase1 receiver cadence sample floor is a strict sixty adjacent intervals",
    "[phase1][file-gate][unique-visual-fps]")
{
    REQUIRE(phase1gate::minimumUniqueVisualCadenceIntervals == 60);
    REQUIRE_FALSE(phase1gate::HasMinimumUniqueVisualCadence(0));
    REQUIRE_FALSE(phase1gate::HasMinimumUniqueVisualCadence(59));
    REQUIRE(phase1gate::HasMinimumUniqueVisualCadence(60));
    REQUIRE(phase1gate::HasMinimumUniqueVisualCadence(std::numeric_limits<std::uint64_t>::max()));
}

TEST_CASE("Phase1 file receiver scopes accepted visuals to its authoritative SessionTag",
    "[phase1][file-gate][session-identity]")
{
    using phase1gate::SessionIdentityDisposition;
    REQUIRE(phase1gate::ClassifySessionIdentity(false, 0, 0) == SessionIdentityDisposition::Unbound);
    REQUIRE(phase1gate::ClassifySessionIdentity(false, 7, 9) == SessionIdentityDisposition::Unbound);
    REQUIRE(phase1gate::ClassifySessionIdentity(true, 7, 7) == SessionIdentityDisposition::Matching);
    REQUIRE(phase1gate::ClassifySessionIdentity(true, 7, 9) == SessionIdentityDisposition::Foreign);
    REQUIRE(phase1gate::ClassifySessionIdentity(true, std::numeric_limits<std::uint64_t>::max(),
        std::numeric_limits<std::uint64_t>::max()) == SessionIdentityDisposition::Matching);
    REQUIRE(phase1gate::ClassifySessionIdentity(true, std::numeric_limits<std::uint64_t>::max(), 0) ==
        SessionIdentityDisposition::Foreign);
}
