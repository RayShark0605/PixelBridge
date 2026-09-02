#include "pbtelemetry/telemetry.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>

namespace
{

pbcapturenormalize::ScreenCaptureDomain MakeDomain(const std::byte identity, const std::uint64_t epoch)
{
    pbcapturenormalize::ScreenCaptureDomain domain;
    domain.sourceId[0] = identity;
    domain.captureEpoch = epoch;
    return domain;
}

std::array<std::byte, 32> Digest(const std::byte value)
{
    std::array<std::byte, 32> digest{};
    digest.fill(value);
    return digest;
}

pbdesktoplevels::FrameEvaluation Evaluation(const bool verified)
{
    pbdesktoplevels::FrameEvaluation evaluation;
    evaluation.evaluated = true;
    evaluation.paddingValid = true;
    evaluation.codewords = 10;
    evaluation.acceptedTransportBlocks = verified ? 10 : 8;
    evaluation.fecFailures = verified ? 0 : 1;
    evaluation.crcFailures = verified ? 0 : 1;
    evaluation.comparedCodedBits = 10 * pbdesktoplevels::kCodewordBits;
    evaluation.erroneousCodedBits = verified ? 0 : 100;
    return evaluation;
}

pbtelemetry::RemoteMetricSample RemoteMetric(const pbcapturenormalize::ScreenCaptureDomain& domain,
    const std::uint64_t captureObservation, const pbtelemetry::RemoteMetricFrameClass frameClass,
    const std::uint32_t zeroMagnitudeMetrics, const double minimumAbsoluteMetric,
    const double meanAbsoluteMetric, const std::uint32_t unreliableSymbols,
    const std::uint32_t staleRegions, const std::uint32_t freshnessTagMismatches,
    const std::uint32_t freshnessTagErasures, const std::uint32_t erasedDataMetrics)
{
    return {domain, captureObservation, 100, zeroMagnitudeMetrics, minimumAbsoluteMetric, meanAbsoluteMetric,
        16, unreliableSymbols, 4, staleRegions, freshnessTagMismatches, freshnessTagErasures,
        erasedDataMetrics, frameClass};
}

} // namespace

TEST_CASE("PBTelemetry preserves independent presentation, capture, visual, FEC and verified-goodput denominators",
    "[telemetry][rates][fec]")
{
    pbtelemetry::TelemetryAccumulator telemetry;
    const auto domain = MakeDomain(std::byte{0x41}, 9);
    constexpr std::int64_t epochStart = 10000000;
    REQUIRE(telemetry.BeginCaptureEpoch(domain, epochStart));

    pbpresenttiming::TimingSnapshot presentation;
    presentation.presentationEpoch = 3;
    presentation.state = pbpresenttiming::TimingState::Valid;
    presentation.qpcFrequency = 1000000;
    presentation.epochStartQpc = 10;
    presentation.sampleQpc = 1000010;
    presentation.presentCallFps = 60.0;
    presentation.presentedVisualFps = 59.94;
    REQUIRE(telemetry.RecordPresentation(presentation));

    const auto firstDigest = Digest(std::byte{0x11});
    const auto secondDigest = Digest(std::byte{0x22});
    REQUIRE(telemetry.RecordCapture({domain, 1, epochStart, 1000, firstDigest}));
    REQUIRE(telemetry.RecordCapture({domain, 2, epochStart + 1000000, 2000, firstDigest}));
    REQUIRE(telemetry.RecordCapture({domain, 3, epochStart + 2200000, std::nullopt, secondDigest}));
    REQUIRE(telemetry.RecordCapture({domain, 4, epochStart + 3000000, 3000, secondDigest}));
    telemetry.RecordDroppedFrames(2);

    REQUIRE(telemetry.RecordBootstrap({domain, 1, false}));
    REQUIRE(telemetry.RecordBootstrap({domain, 2, true, 1.0, 1.0, 0.125, -0.125}));
    REQUIRE(telemetry.RecordBootstrap({domain, 3, true, 1.001, 0.999, 0.0625, -0.0625}));
    REQUIRE(telemetry.RecordFec({domain, 2, Evaluation(true)}));
    REQUIRE(telemetry.RecordFec({domain, 3, Evaluation(false)}));
    telemetry.RecordOuterSymbol(pbtelemetry::OuterSymbolDisposition::AcceptedUnique);
    telemetry.RecordOuterSymbol(pbtelemetry::OuterSymbolDisposition::AcceptedUnique);
    telemetry.RecordOuterSymbol(pbtelemetry::OuterSymbolDisposition::Duplicate);
    telemetry.RecordOuterSymbol(pbtelemetry::OuterSymbolDisposition::Conflict);
    telemetry.RecordOuterSymbol(pbtelemetry::OuterSymbolDisposition::Rejected);
    REQUIRE(telemetry.RecordVerifiedEncodedBytes(1000, epochStart + 4000000));

    const auto snapshot = telemetry.GetSnapshot();
    REQUIRE(snapshot.captureFps == Catch::Approx(10.0));
    REQUIRE(snapshot.uniqueVisualFps == Catch::Approx(10.0 / 3.0));
    REQUIRE(snapshot.duplicateFrameRatio == Catch::Approx(0.5));
    REQUIRE(snapshot.frameArrivalJitterMilliseconds == Catch::Approx(16.3299316185545));
    REQUIRE(snapshot.droppedFrames == 2);
    REQUIRE(snapshot.roiCopyTimingSamples == 3);
    REQUIRE(snapshot.roiCopyTimeAverageMilliseconds == Catch::Approx(0.2));
    REQUIRE(snapshot.roiCopyTimeMaximumMilliseconds == Catch::Approx(0.3));
    REQUIRE(snapshot.bootstrapSuccessRate == Catch::Approx(2.0 / 3.0));
    REQUIRE(snapshot.scaleX == Catch::Approx(1.001));
    REQUIRE(snapshot.phaseY == Catch::Approx(-0.0625));
    REQUIRE(snapshot.preFecBerEstimate == Catch::Approx(100.0 / 324000.0));
    REQUIRE(snapshot.fecEvaluatedFrames == 2);
    REQUIRE(snapshot.fecFrameErrorRate == Catch::Approx(0.5));
    REQUIRE(snapshot.fecCodewordFailureRate == Catch::Approx(0.05));
    REQUIRE(snapshot.acceptedTransportCodewords == 18);
    REQUIRE(snapshot.acceptedTransportCodewordRate == Catch::Approx(0.9));
    REQUIRE(snapshot.crcFailures == 1);
    REQUIRE(snapshot.postFecFailedFrames == 1);
    REQUIRE(snapshot.uniqueOuterSymbols == 2);
    REQUIRE(snapshot.duplicateOuterSymbols == 1);
    REQUIRE(snapshot.acceptedOuterSymbols == 2);
    REQUIRE(snapshot.outerSymbolConflicts == 1);
    REQUIRE(snapshot.rejectedOuterSymbols == 1);
    REQUIRE(snapshot.verifiedEncodedGoodputBitsPerSecond == Catch::Approx(20000.0));
    REQUIRE(snapshot.presentation->presentCallFps == Catch::Approx(60.0));
    REQUIRE(snapshot.presentation->presentedVisualFps == Catch::Approx(59.94));

    std::ostringstream json;
    pbtelemetry::WriteTelemetryJson(json, snapshot);
    const auto text = json.str();
    REQUIRE(text.find("\"PresentCallFPS\":60") != std::string::npos);
    REQUIRE(text.find("\"PresentedVisualFPS\":59.939") != std::string::npos);
    REQUIRE(text.find("\"CaptureFPS\":10") != std::string::npos);
    REQUIRE(text.find("\"UniqueVisualFPS\":3.333") != std::string::npos);
    REQUIRE(text.find("\"CaptureEpoch\":\"9\"") != std::string::npos);
    REQUIRE(text.find("\"PreFecBER\":") != std::string::npos);
    REQUIRE(text.find("\"FER\":0.5") != std::string::npos);
    REQUIRE(text.find("\"FecCodewordFailureRate\":0.050") != std::string::npos);
    REQUIRE(text.find("\"AcceptedTransportCodewordRate\":0.900") != std::string::npos);
    REQUIRE(text.find("\"CRCFailure\":1") != std::string::npos);
    REQUIRE(text.find("\"VerifiedEncodedGoodput\":20000") != std::string::npos);
}

TEST_CASE("PBTelemetry keeps LF4 signal observations separate from unique FEC and Receiver admission denominators",
    "[telemetry][remote-visual][denominators]")
{
    pbtelemetry::TelemetryAccumulator telemetry;
    const auto domain = MakeDomain(std::byte{0x63}, 8);
    REQUIRE(telemetry.BeginCaptureEpoch(domain, 100));
    REQUIRE(telemetry.RecordCapture({domain, 1, 100}));
    REQUIRE(telemetry.RecordCapture({domain, 2, 200}));
    REQUIRE(telemetry.RecordCapture({domain, 3, 300}));

    const auto unavailable = telemetry.GetSnapshot();
    REQUIRE(unavailable.remoteMetricFrames == 0);
    REQUIRE_FALSE(unavailable.remoteZeroMagnitudeMetricRate);
    REQUIRE_FALSE(unavailable.remoteUnreliableSymbolRate);
    REQUIRE_FALSE(unavailable.remoteStaleRegionRate);
    REQUIRE_FALSE(unavailable.remoteFreshnessErasedDataMetricRate);

    REQUIRE(telemetry.RecordRemoteMetric(RemoteMetric(domain, 1, pbtelemetry::RemoteMetricFrameClass::Other,
        0, 0.25, 0.5, 0, 0, 0, 0, 0)));
    REQUIRE(telemetry.RecordRemoteMetric(RemoteMetric(domain, 2, pbtelemetry::RemoteMetricFrameClass::TransportVerified,
        10, 0.1, 1.0, 2, 1, 1, 1, 25)));
    REQUIRE(telemetry.RecordRemoteMetric(RemoteMetric(domain, 3, pbtelemetry::RemoteMetricFrameClass::TransportRejected,
        40, 0.0, 0.25, 8, 4, 2, 3, 50)));

    const auto snapshot = telemetry.GetSnapshot();
    REQUIRE(snapshot.remoteMetricFrames == 3);
    REQUIRE(snapshot.remoteMetricSamples == 300);
    REQUIRE(snapshot.remoteZeroMagnitudeMetrics == 50);
    REQUIRE(snapshot.remoteZeroMagnitudeMetricRate == Catch::Approx(1.0 / 6.0));
    REQUIRE(snapshot.remoteMinimumAbsoluteMetric == Catch::Approx(0.0));
    REQUIRE(snapshot.remoteMeanAbsoluteMetric == Catch::Approx(7.0 / 12.0));
    REQUIRE(snapshot.remoteSymbolSamples == 48);
    REQUIRE(snapshot.remoteUnreliableSymbols == 10);
    REQUIRE(snapshot.remoteUnreliableSymbolRate == Catch::Approx(5.0 / 24.0));
    REQUIRE(snapshot.remoteTransportVerifiedMetricFrames == 1);
    REQUIRE(snapshot.remoteTransportRejectedMetricFrames == 1);
    REQUIRE(snapshot.remoteVerifiedMeanAbsoluteMetric == Catch::Approx(1.0));
    REQUIRE(snapshot.remoteRejectedMeanAbsoluteMetric == Catch::Approx(0.25));
    REQUIRE(snapshot.remoteRejectedZeroMagnitudeMetricRate == Catch::Approx(0.4));
    REQUIRE(snapshot.remoteFreshnessRegions == 12);
    REQUIRE(snapshot.remoteFreshRegions == 7);
    REQUIRE(snapshot.remoteStaleRegions == 5);
    REQUIRE(snapshot.remoteStaleRegionRate == Catch::Approx(5.0 / 12.0));
    REQUIRE(snapshot.remoteFramesWithStaleRegions == 2);
    REQUIRE(snapshot.remoteFreshnessTagMismatches == 3);
    REQUIRE(snapshot.remoteFreshnessTagErasures == 4);
    REQUIRE(snapshot.remoteFreshnessErasedDataMetrics == 75);
    REQUIRE(snapshot.remoteFreshnessErasedDataMetricRate == Catch::Approx(0.25));
    REQUIRE(snapshot.fecEvaluatedFrames == 0);
    REQUIRE(snapshot.acceptedTransportCodewords == 0);
    REQUIRE(snapshot.acceptedOuterSymbols == 0);

    std::ostringstream json;
    pbtelemetry::WriteTelemetryJson(json, snapshot);
    const auto text = json.str();
    REQUIRE(text.find("\"remoteMetric\":{\"frames\":3,\"samples\":300") != std::string::npos);
    REQUIRE(text.find("\"symbolSamples\":48,\"unreliableSymbols\":10") != std::string::npos);
    REQUIRE(text.find("\"freshnessRegions\":12,\"freshRegions\":7,\"staleRegions\":5") != std::string::npos);
}

TEST_CASE("PBTelemetry distinguishes a measured zero LF4 rate from an unavailable rate and rejects duplicate metrics",
    "[telemetry][remote-visual][null-semantics][errors]")
{
    pbtelemetry::TelemetryAccumulator telemetry;
    const auto domain = MakeDomain(std::byte{0x64}, 9);
    REQUIRE(telemetry.BeginCaptureEpoch(domain, 100));
    REQUIRE(telemetry.RecordCapture({domain, 1, 100}));
    REQUIRE(telemetry.RecordCapture({domain, 2, 200}));
    const auto zeroSample = RemoteMetric(domain, 1, pbtelemetry::RemoteMetricFrameClass::TransportVerified,
        0, 0.5, 0.5, 0, 0, 0, 0, 0);
    REQUIRE(telemetry.RecordRemoteMetric(zeroSample));

    const auto measuredZero = telemetry.GetSnapshot();
    REQUIRE(measuredZero.remoteZeroMagnitudeMetricRate == 0.0);
    REQUIRE(measuredZero.remoteUnreliableSymbolRate == 0.0);
    REQUIRE(measuredZero.remoteStaleRegionRate == 0.0);
    REQUIRE(measuredZero.remoteFreshnessErasedDataMetricRate == 0.0);
    REQUIRE(telemetry.RecordRemoteMetric(zeroSample).code == pbtelemetry::TelemetryError::ObservationOrder);

    auto malformed = RemoteMetric(domain, 2, pbtelemetry::RemoteMetricFrameClass::TransportRejected,
        0, 0.5, 0.25, 0, 0, 0, 0, 0);
    REQUIRE(telemetry.RecordRemoteMetric(malformed).code == pbtelemetry::TelemetryError::InvalidSample);
    malformed = RemoteMetric(domain, 2, pbtelemetry::RemoteMetricFrameClass::TransportRejected,
        0, 0.0, 0.5, 17, 0, 0, 0, 0);
    REQUIRE(telemetry.RecordRemoteMetric(malformed).code == pbtelemetry::TelemetryError::InvalidSample);
    malformed = RemoteMetric(domain, 2, pbtelemetry::RemoteMetricFrameClass::TransportRejected,
        0, 0.0, 0.5, 0, 5, 0, 0, 0);
    REQUIRE(telemetry.RecordRemoteMetric(malformed).code == pbtelemetry::TelemetryError::InvalidSample);
    malformed = RemoteMetric(domain, 2, pbtelemetry::RemoteMetricFrameClass::TransportRejected,
        0, 0.0, (std::numeric_limits<double>::quiet_NaN)(), 0, 0, 0, 0, 0);
    REQUIRE(telemetry.RecordRemoteMetric(malformed).code == pbtelemetry::TelemetryError::InvalidSample);
    const auto afterRejected = telemetry.GetSnapshot();
    REQUIRE(afterRejected.remoteMetricFrames == measuredZero.remoteMetricFrames);
    REQUIRE(afterRejected.remoteMetricSamples == measuredZero.remoteMetricSamples);

    const auto nextDomain = MakeDomain(std::byte{0x65}, 10);
    REQUIRE(telemetry.BeginCaptureEpoch(nextDomain, 1000));
    const auto reset = telemetry.GetSnapshot();
    REQUIRE(reset.remoteMetricFrames == 0);
    REQUIRE(reset.remoteMetricSamples == 0);
    REQUIRE_FALSE(reset.remoteZeroMagnitudeMetricRate);
    REQUIRE_FALSE(reset.remoteMinimumAbsoluteMetric);
    REQUIRE_FALSE(reset.remoteMeanAbsoluteMetric);
    REQUIRE_FALSE(reset.remoteUnreliableSymbolRate);
}

TEST_CASE("PBTelemetry rejects cross-epoch, reordered and malformed samples without mutating committed counters",
    "[telemetry][errors][epoch]")
{
    pbtelemetry::TelemetryAccumulator telemetry;
    const auto domain = MakeDomain(std::byte{0x51}, 4);
    const auto wrongDomain = MakeDomain(std::byte{0x52}, 4);
    REQUIRE(telemetry.BeginCaptureEpoch(domain, 100));
    REQUIRE(telemetry.RecordCapture({domain, 1, 100, std::nullopt, Digest(std::byte{1})}));
    const auto before = telemetry.GetSnapshot();
    REQUIRE(telemetry.RecordCapture({wrongDomain, 2, 200}).code == pbtelemetry::TelemetryError::DomainMismatch);
    REQUIRE(telemetry.RecordCapture({domain, 1, 200}).code == pbtelemetry::TelemetryError::ObservationOrder);
    REQUIRE(telemetry.RecordCapture({domain, 2, 99}).code == pbtelemetry::TelemetryError::TimestampOrder);
    REQUIRE(telemetry.RecordBootstrap({domain, 1, true, -1.0, 1.0, 0.0, 0.0}).code == pbtelemetry::TelemetryError::InvalidSample);
    auto malformed = Evaluation(true);
    malformed.erroneousCodedBits = malformed.comparedCodedBits + 1;
    REQUIRE(telemetry.RecordFec({domain, 1, malformed}).code == pbtelemetry::TelemetryError::InvalidSample);
    malformed = Evaluation(true);
    malformed.comparedCodedBits = 0;
    malformed.erroneousCodedBits = 1;
    REQUIRE(telemetry.RecordFec({domain, 1, malformed}).code == pbtelemetry::TelemetryError::InvalidSample);
    malformed = Evaluation(true);
    malformed.comparedCodedBits--;
    REQUIRE(telemetry.RecordFec({domain, 1, malformed}).code == pbtelemetry::TelemetryError::InvalidSample);
    malformed = Evaluation(true);
    malformed.acceptedTransportBlocks--;
    REQUIRE(telemetry.RecordFec({domain, 1, malformed}).code == pbtelemetry::TelemetryError::InvalidSample);
    malformed = Evaluation(true);
    malformed.iterationsTotal = malformed.codewords * 48 + 1;
    REQUIRE(telemetry.RecordFec({domain, 1, malformed}).code == pbtelemetry::TelemetryError::InvalidSample);
    const auto after = telemetry.GetSnapshot();
    REQUIRE(after.capturedFrames == before.capturedFrames);
    REQUIRE(after.bootstrapAttempts == before.bootstrapAttempts);
    REQUIRE(after.fecCodewords == before.fecCodewords);
    REQUIRE(telemetry.RecordVerifiedEncodedBytes(0, 200).code == pbtelemetry::TelemetryError::InvalidSample);

    telemetry.EndCaptureEpoch();
    telemetry.RecordDroppedFrames(7);
    telemetry.RecordOuterSymbol(pbtelemetry::OuterSymbolDisposition::AcceptedUnique);
    REQUIRE(telemetry.RecordCapture({domain, 2, 200}).code == pbtelemetry::TelemetryError::InactiveEpoch);
    const auto ended = telemetry.GetSnapshot();
    REQUIRE_FALSE(ended.active);
    REQUIRE(ended.droppedFrames == before.droppedFrames);
    REQUIRE(ended.uniqueOuterSymbols == before.uniqueOuterSymbols);

    const auto nextDomain = MakeDomain(std::byte{0x61}, 5);
    REQUIRE(telemetry.BeginCaptureEpoch(nextDomain, 1000));
    const auto reset = telemetry.GetSnapshot();
    REQUIRE(reset.epochStarts == 2);
    REQUIRE(reset.domain == nextDomain);
    REQUIRE(reset.capturedFrames == 0);
    REQUIRE_FALSE(reset.captureFps);
    REQUIRE_FALSE(reset.presentation);
}

TEST_CASE("PBTelemetry records production FER while leaving PreFecBER unavailable without sender truth",
    "[telemetry][fec][coverage]")
{
    pbtelemetry::TelemetryAccumulator telemetry;
    const auto domain = MakeDomain(std::byte{0x62}, 7);
    REQUIRE(telemetry.BeginCaptureEpoch(domain, 100));
    REQUIRE(telemetry.RecordCapture({domain, 1, 100}));
    auto productionEvaluation = Evaluation(false);
    productionEvaluation.comparedCodedBits = 0;
    productionEvaluation.erroneousCodedBits = 0;
    REQUIRE(telemetry.RecordFec({domain, 1, productionEvaluation}));

    const auto snapshot = telemetry.GetSnapshot();
    REQUIRE(snapshot.fecEvaluatedFrames == 1);
    REQUIRE(snapshot.fecFrameErrorRate == 1.0);
    REQUIRE(snapshot.fecCodewordFailureRate == Catch::Approx(0.1));
    REQUIRE(snapshot.comparedCodedBits == 0);
    REQUIRE(snapshot.erroneousCodedBits == 0);
    REQUIRE_FALSE(snapshot.preFecBerEstimate);
}

TEST_CASE("PBTelemetry saturates hostile counters and withdraws derived rates",
    "[telemetry][overflow]")
{
    pbtelemetry::TelemetryAccumulator telemetry;
    REQUIRE(telemetry.BeginCaptureEpoch(MakeDomain(std::byte{0x71}, 1), 0));
    telemetry.RecordDroppedFrames(std::numeric_limits<std::uint64_t>::max());
    telemetry.RecordDroppedFrames(1);
    const auto snapshot = telemetry.GetSnapshot();
    REQUIRE(snapshot.droppedFrames == std::numeric_limits<std::uint64_t>::max());
    REQUIRE(snapshot.counterSaturated);
    REQUIRE_FALSE(snapshot.captureFps);
    REQUIRE_FALSE(snapshot.verifiedEncodedGoodputBitsPerSecond);
}
