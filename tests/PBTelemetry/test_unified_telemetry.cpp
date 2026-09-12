#include "pbtelemetry/unified_telemetry.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <limits>
#include <sstream>

namespace
{
pbmodulation::UnifiedVisualObservation Observation(const std::uint64_t sequence)
{
    pbmodulation::UnifiedVisualObservation observation;
    observation.inputValid = true;
    observation.frameErasure = pbmodulation::UnifiedErasureReason::None;
    observation.bootstrap.erasure = pbmodulation::LocalDesktopErasureReason::None;
    observation.bootstrapRecord.sessionTag.value = 7;
    observation.bootstrapRecord.frameSequence = sequence;
    observation.bootstrapRecord.visualProfileId = pbprotocol::kUnifiedVisualProfileId;
    observation.bootstrapRecord.visualLayoutVersion = pbprotocol::kUnifiedVisualLayoutVersion;
    // Product-identity observation: only the fifteen SC6 slots are active;
    // the storage tail belongs to the larger gray carrier and is never
    // populated or evaluated for this identity.
    for (std::uint32_t index = 0; index < pbmodulation::kUnifiedCodewordCount; index++)
    {
        auto& slot = observation.slots[index];
        slot.lane = pbmodulation::FindUnifiedLaneForCodewordSlot(index)->lane;
        slot.rejection = pbmodulation::UnifiedSlotRejection::None;
        slot.fecValid = slot.paddingValid = slot.crcValid = slot.identityValid = slot.accepted = true;
        slot.acceptedBytes = 100;
    }
    observation.slots[0].kind = pbmodulation::UnifiedSlotKind::Control;
    observation.acceptedBlocks = pbmodulation::kUnifiedCodewordCount;
    observation.acceptedTransportBlocks = observation.acceptedBlocks - 1;
    observation.acceptedControlRecords = 1;
    constexpr std::array<std::uint32_t, 3> codewords{
        pbmodulation::kUnifiedLaneCapacities[0].codewordCount,
        pbmodulation::kUnifiedLaneCapacities[1].codewordCount,
        pbmodulation::kUnifiedLaneCapacities[2].codewordCount};
    for (std::size_t lane = 0; lane < codewords.size(); lane++)
    {
        const std::uint32_t samples = codewords[lane] * 16200;
        observation.laneMetrics[lane] = {true, samples, 0, 0, 64, static_cast<std::uint64_t>(samples) * 64};
    }
    return observation;
}
} // namespace

TEST_CASE("G17 counts actual observed frames across capture epochs without extrapolating gaps", "[telemetry][g17]")
{
    pbtelemetry::UnifiedTelemetryAccumulator telemetry;
    REQUIRE(telemetry.BindSession(7));
    REQUIRE(telemetry.Record(Observation(10), 1, 0));
    REQUIRE(telemetry.Record(Observation(10), 1, 100000));
    REQUIRE(telemetry.Record(Observation(10), 2, 500000));
    REQUIRE(telemetry.Record(Observation(13), 2, 10000000));
    const auto snapshot = telemetry.GetSnapshot();
    REQUIRE(snapshot.uniqueFrames == 2);
    REQUIRE(snapshot.duplicateObservations == 2);
    REQUIRE(snapshot.uniqueVisualFps.has_value());
    REQUIRE(*snapshot.uniqueVisualFps == Catch::Approx(1.0));
    REQUIRE(snapshot.lanes[0].evaluatedSlots == 4 * pbmodulation::kUnifiedLaneCapacities[0].codewordCount);
    REQUIRE(snapshot.lanes[1].acceptedSlots == 4 * pbmodulation::kUnifiedLaneCapacities[1].codewordCount);
    REQUIRE(snapshot.lanes[2].metricSamples ==
        4 * pbmodulation::kUnifiedLaneCapacities[2].codewordCount * 16200);
    REQUIRE(snapshot.acceptedControlSlots == 4);
    REQUIRE(snapshot.lanes[0].fecFailures == 0);
    REQUIRE(pbtelemetry::EvaluatePublishedFrameMetric(snapshot, 20000, true, true, true, false).bytesPerUniqueFrame == 10000.0);
    REQUIRE_FALSE(pbtelemetry::EvaluatePublishedFrameMetric(snapshot, 20000, false, true, true, false).bytesPerUniqueFrame);
    REQUIRE_FALSE(pbtelemetry::EvaluatePublishedFrameMetric(snapshot, 20000, true, false, true, false).bytesPerUniqueFrame);
    REQUIRE_FALSE(pbtelemetry::EvaluatePublishedFrameMetric(snapshot, 20000, true, true, false, false).bytesPerUniqueFrame);
    REQUIRE(std::string(pbtelemetry::EvaluatePublishedFrameMetric(snapshot, 20000, true, true, true, true).unavailableReason) ==
        "ResumeHasNoLifetimeFrameCoverage");
    REQUIRE(pbtelemetry::EvaluatePublishedFrameMetric(snapshot, 0, true, true, true, false).bytesPerUniqueFrame == 0.0);
}

TEST_CASE("G17 malformed observations cannot partially mutate lane counters", "[telemetry][g17]")
{
    pbtelemetry::UnifiedTelemetryAccumulator telemetry;
    REQUIRE(telemetry.BindSession(7));
    REQUIRE(telemetry.Record(Observation(1), 1, 0));
    auto invalid = Observation(2);
    SECTION("metric dimension")
    {
        invalid.laneMetrics[2].samples++;
    }
    SECTION("accepted FEC conflict")
    {
        invalid.slots[pbmodulation::kUnifiedCodewordCount - 1].fecValid = false;
    }
    SECTION("accepted total conflict")
    {
        invalid.acceptedBlocks--;
    }
    SECTION("unknown slot kind")
    {
        invalid.slots[pbmodulation::kUnifiedCodewordCount - 1].kind =
            static_cast<pbmodulation::UnifiedSlotKind>(255);
    }
    SECTION("unknown rejection")
    {
        invalid.slots[pbmodulation::kUnifiedCodewordCount - 1].rejection =
            static_cast<pbmodulation::UnifiedSlotRejection>(255);
    }
    SECTION("erased frame cannot report accepted bytes")
    {
        invalid.frameErasure = pbmodulation::UnifiedErasureReason::CanvasClipped;
    }
    REQUIRE_FALSE(telemetry.Record(invalid, 1, 1));
    const auto snapshot = telemetry.GetSnapshot();
    REQUIRE(snapshot.observations == 1);
    REQUIRE(snapshot.lanes[0].metricObservations == 1);
    REQUIRE_FALSE(snapshot.frameCoverageComplete);
    REQUIRE_FALSE(pbtelemetry::EvaluatePublishedFrameMetric(snapshot, 20, true, true, true, false).bytesPerUniqueFrame);
    REQUIRE_FALSE(telemetry.BindSession(8));
}

TEST_CASE("G17 lane failures and Bootstrap-only erasures keep separate measured denominators", "[telemetry][g17]")
{
    pbtelemetry::UnifiedTelemetryAccumulator telemetry;
    REQUIRE(telemetry.BindSession(7));
    auto observation = Observation(1);
    constexpr std::array<std::uint32_t, 5> failedSlots{1, 9, 10, 11, 12};
    for (const auto slot : failedSlots)
    {
        observation.slots[slot].accepted = false;
        observation.slots[slot].acceptedBytes = 0;
    }
    observation.slots[1].rejection = pbmodulation::UnifiedSlotRejection::InnerFecFailure;
    observation.slots[1].fecValid = false;
    observation.slots[1].iterationsUsed = 12;
    observation.slots[9].rejection = pbmodulation::UnifiedSlotRejection::TransportCrcFailure;
    observation.slots[9].crcValid = false;
    observation.slots[10].rejection = pbmodulation::UnifiedSlotRejection::LaneErasure;
    observation.slots[10].fecValid = false;
    observation.slots[11].rejection = pbmodulation::UnifiedSlotRejection::IdentityFailure;
    observation.slots[11].identityValid = false;
    observation.slots[12].rejection = pbmodulation::UnifiedSlotRejection::InvalidInformation;
    observation.acceptedBlocks -= static_cast<std::uint32_t>(failedSlots.size());
    observation.acceptedTransportBlocks -= static_cast<std::uint32_t>(failedSlots.size());
    REQUIRE(telemetry.Record(observation, 1, 0));
    REQUIRE(telemetry.RecordUnavailableFrame(Observation(2).bootstrapRecord, 1, 10000000));
    const auto snapshot = telemetry.GetSnapshot();
    REQUIRE(snapshot.observations == 2);
    REQUIRE(snapshot.frameErasedObservations == 1);
    REQUIRE(snapshot.uniqueFrames == 2);
    REQUIRE(snapshot.lanes[0].metricObservations == 1);
    REQUIRE(snapshot.lanes[0].fecFailures == 1);
    REQUIRE(snapshot.lanes[0].fecIterations == 12);
    REQUIRE(snapshot.lanes[1].crcFailures == 1);
    REQUIRE(snapshot.lanes[2].identityFailures == 1);
    REQUIRE(snapshot.lanes[2].fecAttempts == 4);
    REQUIRE(snapshot.lanes[2].erasedSlots == 3);
    REQUIRE(snapshot.unclassifiedSlots == 3);
    REQUIRE(snapshot.classifiedControlSlots == 1);
    REQUIRE(pbtelemetry::EvaluatePublishedFrameMetric(snapshot, 200, true, true, true, false).bytesPerUniqueFrame == 100.0);
}

TEST_CASE("G17 timestamp regression withdraws evidence but foreign Session cannot rebind it", "[telemetry][g17]")
{
    pbtelemetry::UnifiedTelemetryAccumulator telemetry;
    REQUIRE(telemetry.BindSession(7));
    REQUIRE(telemetry.Record(Observation(1), 1, 100));
    auto foreign = Observation(2);
    foreign.bootstrapRecord.sessionTag.value = 8;
    REQUIRE_FALSE(telemetry.Record(foreign, 1, 101));
    REQUIRE(telemetry.GetSnapshot().frameCoverageComplete);
    REQUIRE(telemetry.GetSnapshot().sessionTag == 7);
    REQUIRE_FALSE(telemetry.Record(Observation(2), 1, 99));
    REQUIRE_FALSE(telemetry.GetSnapshot().frameCoverageComplete);
    REQUIRE(telemetry.GetSnapshot().observations == 1);
    REQUIRE_FALSE(telemetry.GetSnapshot().uniqueVisualFps);
}

TEST_CASE("G17 bounded identity history withdraws rates rather than guessing evicted old frames", "[telemetry][g17]")
{
    pbtelemetry::UnifiedTelemetryAccumulator telemetry;
    REQUIRE(telemetry.BindSession(7));
    for (std::uint64_t sequence = 1; sequence <= 4100; sequence++)
    {
        REQUIRE(telemetry.Record(Observation(sequence), 1, static_cast<std::int64_t>(sequence) * 100));
    }
    REQUIRE(telemetry.Record(Observation(1), 2, 500000));
    const auto snapshot = telemetry.GetSnapshot();
    REQUIRE(snapshot.uniqueFrames == 4100);
    REQUIRE_FALSE(snapshot.frameCoverageComplete);
    REQUIRE_FALSE(snapshot.uniqueVisualFps);
}

TEST_CASE("G21 bounded identity history counts provably new out-of-order frames exactly", "[telemetry][g21]")
{
    pbtelemetry::UnifiedTelemetryAccumulator telemetry;
    REQUIRE(telemetry.BindSession(7));
    REQUIRE(telemetry.Record(Observation(10), 1, 100));
    REQUIRE(telemetry.Record(Observation(12), 1, 200));
    REQUIRE(telemetry.Record(Observation(11), 1, 300));
    REQUIRE(telemetry.Record(Observation(11), 1, 400));
    const auto snapshot = telemetry.GetSnapshot();
    REQUIRE(snapshot.uniqueFrames == 3);
    REQUIRE(snapshot.duplicateObservations == 1);
    REQUIRE(snapshot.frameCoverageComplete);
    REQUIRE(snapshot.uniqueVisualFps.has_value());
}

TEST_CASE("G21 full identity window distinguishes a nearby missing frame from an evicted ambiguity", "[telemetry][g21]")
{
    pbtelemetry::UnifiedTelemetryAccumulator telemetry;
    REQUIRE(telemetry.BindSession(7));
    for (std::uint64_t sequence = 1; sequence <= 4100; sequence++)
    {
        if (sequence != 4098)
        {
            REQUIRE(telemetry.Record(Observation(sequence), 1, static_cast<std::int64_t>(sequence) * 100));
        }
    }
    REQUIRE(telemetry.Record(Observation(4098), 1, 500000));
    REQUIRE(telemetry.GetSnapshot().uniqueFrames == 4100);
    REQUIRE(telemetry.GetSnapshot().frameCoverageComplete);
    REQUIRE(telemetry.Record(Observation(1), 1, 500100));
    REQUIRE(telemetry.GetSnapshot().uniqueFrames == 4100);
    REQUIRE_FALSE(telemetry.GetSnapshot().frameCoverageComplete);
}

TEST_CASE("G17 telemetry distinguishes unavailable observations from measured zero failures", "[telemetry][g17]")
{
    pbtelemetry::UnifiedTelemetrySnapshot snapshot;
    snapshot.uniqueVisualFps = (std::numeric_limits<double>::infinity)();
    std::ostringstream output;
    pbtelemetry::WriteUnifiedTelemetryJson(output, snapshot);
    REQUIRE(output.str().find("\"metrics\":null") != std::string::npos);
    REQUIRE(output.str().find("\"fec\":null") != std::string::npos);
    REQUIRE(output.str().find("\"uniqueVisualFps\":null") != std::string::npos);
    REQUIRE(output.str().find(":inf") == std::string::npos);
    REQUIRE(output.str().find("\"firstCoverageFailure\":null") != std::string::npos);
}

TEST_CASE("G21 first coverage failure is bounded immutable evidence without counter repair", "[telemetry][g21][coverage-diagnostic]")
{
    pbtelemetry::UnifiedTelemetryAccumulator telemetry;
    REQUIRE(telemetry.BindSession(7));
    REQUIRE(telemetry.Record(Observation(100), 1, 1000));
    REQUIRE_FALSE(telemetry.GetSnapshot().firstCoverageFailure);
    REQUIRE_FALSE(telemetry.Record(Observation(101), 1, 999));
    const auto first = telemetry.GetSnapshot();
    REQUIRE_FALSE(first.frameCoverageComplete);
    REQUIRE(first.observations == 1);
    REQUIRE(first.uniqueFrames == 1);
    REQUIRE(first.firstCoverageFailure.has_value());
    REQUIRE(first.firstCoverageFailure->reason == pbtelemetry::UnifiedCoverageFailureReason::TimestampOrder);
    REQUIRE(first.firstCoverageFailure->observationsBeforeFailure == 1);
    REQUIRE(first.firstCoverageFailure->uniqueFramesBeforeFailure == 1);
    REQUIRE(first.firstCoverageFailure->frameSequence == 101);
    REQUIRE(first.firstCoverageFailure->captureEpoch == 1);
    REQUIRE(first.firstCoverageFailure->timestamp100ns == 999);
    REQUIRE(first.firstCoverageFailure->previousTimestamp100ns == 1000);
    auto invalid = Observation(102);
    invalid.inputValid = false;
    REQUIRE_FALSE(telemetry.Record(invalid, 1, 1001));
    telemetry.InvalidateFrameCoverage();
    REQUIRE(telemetry.Record(Observation(103), 1, 1002));
    const auto after = telemetry.GetSnapshot();
    REQUIRE(after.firstCoverageFailure->reason == pbtelemetry::UnifiedCoverageFailureReason::TimestampOrder);
    REQUIRE(after.firstCoverageFailure->frameSequence == 101);
    REQUIRE(after.firstCoverageFailure->observationsBeforeFailure == 1);
    REQUIRE(after.observations == 2);
    REQUIRE(after.uniqueFrames == 2);
    REQUIRE_FALSE(after.frameCoverageComplete);
    REQUIRE_FALSE(after.uniqueVisualFps);
    REQUIRE_FALSE(pbtelemetry::EvaluatePublishedFrameMetric(after, 32768, true, true, true, false).bytesPerUniqueFrame);
    std::ostringstream output;
    pbtelemetry::WriteUnifiedTelemetryJson(output, after);
    REQUIRE(output.str().find("\"reason\":\"TimestampOrder\"") != std::string::npos);
    REQUIRE(output.str().find("\"timestamp100ns\":999,\"previousTimestamp100ns\":1000") != std::string::npos);
}
