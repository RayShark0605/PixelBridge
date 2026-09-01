#include "metric_calibration_core.h"
#include "calibration_pipeline.h"

#include "pbdesktoplevels/reference_channel.h"
#include "pbmodulation/remote_visual_low_fps.h"
#include "pbprotocol/bootstrap_control_codec.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <string>
#include <vector>

namespace
{

constexpr std::uint64_t testSessionTag = 0x1020304050607080ULL;

std::array<std::byte, pbprotocol::kBootstrapRecordBytes> MakeBootstrap(const std::uint64_t sequence)
{
    pbprotocol::BootstrapRecord record;
    record.protocolVersion = pbprotocol::GetProtocolVersion();
    record.visualLayoutVersion = pbmodulation::kRemoteVisualLowFpsLayoutVersion;
    record.visualProfileId = pbmodulation::kRemoteVisualLowFpsProfileId;
    record.sessionTag.value = testSessionTag;
    record.frameSequence = sequence;
    std::array<std::byte, pbprotocol::kBootstrapRecordBytes> output{};
    REQUIRE(pbprotocol::SerializeBootstrapRecord(record, output));
    return output;
}

std::vector<float> MakeMetrics(const std::span<const std::byte> bootstrap, const float magnitude,
    const std::span<const std::size_t> flippedBits = {})
{
    std::vector<std::byte> data(pbmodulation::kRemoteVisualLowFpsDataBytes);
    REQUIRE(pbdesktoplevels::GenerateDiagnosticData(bootstrap, data));
    std::vector<float> metrics(pbmodulation::kRemoteVisualLowFpsCodedBits);
    for (std::size_t bit = 0; bit < metrics.size(); bit++)
    {
        const bool one = (std::to_integer<unsigned>(data[bit / 8]) & (1u << (bit % 8))) != 0;
        metrics[bit] = one ? -magnitude : magnitude;
    }
    for (const std::size_t bit : flippedBits)
    {
        REQUIRE(bit < metrics.size());
        metrics[bit] = -metrics[bit];
    }
    return metrics;
}

pbremotevisualmetriccalibration::MetricObservation MakeObservation(
    const pbremotevisualmetriccalibration::DatasetSplit split, const std::string& runId,
    const std::uint64_t sequence, const float magnitude)
{
    pbremotevisualmetriccalibration::MetricObservation observation;
    observation.datasetId = "unit-dataset";
    observation.runId = runId;
    observation.frameId = "frame-0";
    observation.signalProfile = "PB-Signal-Test-1";
    observation.policyId = pbremotevisualmetriccalibration::kDefaultPolicyId;
    observation.policyManifestJson = "{\"minimumSymbolMargin\":0.08}";
    observation.split = split;
    observation.sourceBootstrap = MakeBootstrap(sequence);
    observation.receiverBootstrap = observation.sourceBootstrap;
    observation.receiverBootstrapAvailable = true;
    observation.modulationAccepted = true;
    observation.modulationErasure = "None";
    observation.rawMetrics = MakeMetrics(observation.sourceBootstrap, magnitude);
    return observation;
}

std::vector<pbremotevisualmetriccalibration::MetricObservation> MakeCompleteDataset()
{
    using pbremotevisualmetriccalibration::DatasetSplit;
    return {
        MakeObservation(DatasetSplit::Train, "train-run", 1, 0.1f),
        MakeObservation(DatasetSplit::Validation, "validation-run", 2, 0.1f),
        MakeObservation(DatasetSplit::Holdout, "holdout-run", 3, 0.1f),
        MakeObservation(DatasetSplit::External, "external-run", 4, 0.1f)};
}

const pbremotevisualmetriccalibration::CandidateReport& FindCandidate(
    const pbremotevisualmetriccalibration::CalibrationReport& report, const std::string& candidateId)
{
    const auto found = std::ranges::find_if(report.candidates, [&](const auto& candidate)
    {
        return candidate.candidateId == candidateId;
    });
    REQUIRE(found != report.candidates.end());
    return *found;
}

} // namespace

TEST_CASE("Metric calibration selects without holdout leakage and improves truth-labelled external data",
    "[tools][remote-visual][calibration][holdout][truth]")
{
    const auto observations = MakeCompleteDataset();
    pbremotevisualmetriccalibration::CalibrationReport first;
    pbremotevisualmetriccalibration::CalibrationReport second;
    std::string error;
    REQUIRE(pbremotevisualmetriccalibration::BuildCalibrationReport(observations, first, error));
    REQUIRE(error.empty());
    REQUIRE(pbremotevisualmetriccalibration::BuildCalibrationReport(observations, second, error));
    REQUIRE(error.empty());
    REQUIRE(first.canonicalJson == second.canonicalJson);
    REQUIRE(first.splitIsolationValid);
    REQUIRE_FALSE(first.selectionUsedHoldout);
    REQUIRE_FALSE(first.acceptanceAuthorityChanged);
    REQUIRE_FALSE(first.productionDefaultsChanged);
    REQUIRE(first.selectedCandidateId != first.baselineCandidateId);
    REQUIRE(first.holdoutImproved);
    REQUIRE(first.externalImproved);
    REQUIRE(first.gatePassed);

    const auto& baseline = FindCandidate(first, first.baselineCandidateId);
    const auto& selected = FindCandidate(first, first.selectedCandidateId);
    REQUIRE(baseline.validation.verifiedFrames == 1);
    REQUIRE(selected.validation.verifiedFrames == 1);
    REQUIRE(selected.validation.falseAcceptedTransportBlocks == 0);
    REQUIRE(selected.holdout.logLossSum < baseline.holdout.logLossSum);
    REQUIRE(selected.external.logLossSum < baseline.external.logLossSum);
    REQUIRE(selected.holdout.fecFailures <= baseline.holdout.fecFailures);
    REQUIRE(selected.holdout.crcFailures <= baseline.holdout.crcFailures);
    REQUIRE(selected.holdout.identityFailures <= baseline.holdout.identityFailures);
    REQUIRE(selected.holdout.iterationsTotal <= baseline.holdout.iterationsTotal);
    REQUIRE(selected.holdout.brierScoreSum < baseline.holdout.brierScoreSum);
    REQUIRE(selected.holdout.expectedCalibrationError < baseline.holdout.expectedCalibrationError);
    REQUIRE(selected.external.fecFailures <= baseline.external.fecFailures);
    REQUIRE(selected.external.crcFailures <= baseline.external.crcFailures);
    REQUIRE(selected.external.identityFailures <= baseline.external.identityFailures);
    REQUIRE(selected.external.iterationsTotal <= baseline.external.iterationsTotal);
    REQUIRE(selected.external.brierScoreSum < baseline.external.brierScoreSum);
    REQUIRE(selected.external.expectedCalibrationError < baseline.external.expectedCalibrationError);
    REQUIRE(selected.validation.rocCurve.size() == 23);
    REQUIRE(selected.validation.rocCurve.front().truePositive == selected.validation.expectedOneBits);
    REQUIRE(selected.validation.rocCurve.front().falsePositive == selected.validation.expectedZeroBits);
    REQUIRE(selected.validation.rocCurve.back().truePositive == 0);
    REQUIRE(selected.validation.rocCurve.back().falsePositive == 0);
    REQUIRE(first.canonicalJson.find("\"selectionUsedHoldout\":false") != std::string::npos);
    REQUIRE(first.canonicalJson.find("\"acceptanceAuthorityChanged\":false") != std::string::npos);
    REQUIRE(first.canonicalJson.find("\"wholeFileOutputEvaluated\":false") != std::string::npos);
}

TEST_CASE("Admission-relaxing policies are reported but cannot become the selected candidate",
    "[tools][remote-visual][calibration][admission][negative]")
{
    auto observations = MakeCompleteDataset();
    const std::size_t defaultObservations = observations.size();
    for (std::size_t index = 0; index < defaultObservations; index++)
    {
        auto relaxed = observations[index];
        relaxed.policyId = "lf4-relaxed";
        relaxed.policyManifestJson = "{\"minimumSymbolMargin\":0.01}";
        relaxed.policyRelaxesDefaultAdmission = true;
        relaxed.rawMetrics = MakeMetrics(relaxed.sourceBootstrap, 6.0f);
        observations.push_back(std::move(relaxed));
    }
    pbremotevisualmetriccalibration::CalibrationReport report;
    std::string error;
    REQUIRE(pbremotevisualmetriccalibration::BuildCalibrationReport(observations, report, error));
    REQUIRE(error.empty());
    REQUIRE(report.selectedCandidateId.find("lf4-relaxed/") != 0);
    const auto& relaxed = FindCandidate(report, "lf4-relaxed/Raw");
    REQUIRE(relaxed.policyRelaxesDefaultAdmission);
    REQUIRE(report.canonicalJson.find("\"policyRelaxesDefaultAdmission\":true") != std::string::npos);
}

TEST_CASE("Metric calibration rejects dataset run leakage and leaves output unchanged",
    "[tools][remote-visual][calibration][split][negative]")
{
    auto observations = MakeCompleteDataset();
    observations[1].runId = observations[0].runId;
    observations[1].frameId = "frame-1";
    pbremotevisualmetriccalibration::CalibrationReport output;
    output.selectedCandidateId = "sentinel";
    output.canonicalJson = "sentinel-json";
    std::string error;
    REQUIRE_FALSE(pbremotevisualmetriccalibration::BuildCalibrationReport(observations, output, error));
    REQUIRE(error.find("leaks across") != std::string::npos);
    REQUIRE(output.selectedCandidateId == "sentinel");
    REQUIRE(output.canonicalJson == "sentinel-json");
}

TEST_CASE("Receiver-only external data keeps false acceptance unavailable and cannot pass the Gate",
    "[tools][remote-visual][calibration][receiver-only][negative]")
{
    auto observations = MakeCompleteDataset();
    auto& external = observations.back();
    external.truthAvailability =
        pbremotevisualmetriccalibration::TruthAvailability::UnavailableReceiverOnly;
    external.sourceBootstrap.fill(std::byte{0});
    pbremotevisualmetriccalibration::CalibrationReport report;
    std::string error;
    REQUIRE(pbremotevisualmetriccalibration::BuildCalibrationReport(observations, report, error));
    REQUIRE(error.empty());
    REQUIRE_FALSE(report.externalImproved);
    REQUIRE_FALSE(report.gatePassed);
    const auto& baseline = FindCandidate(report, report.baselineCandidateId);
    REQUIRE(baseline.external.truthFrames == 0);
    REQUIRE(baseline.external.receiverOnlyFrames == 1);
    REQUIRE(baseline.external.comparedBits == 0);
}

TEST_CASE("CRC-valid non-truth Transport makes the validation baseline unsafe",
    "[tools][remote-visual][calibration][false-confidence][transport][negative]")
{
    auto observations = MakeCompleteDataset();
    auto& validation = observations[1];
    const auto conflictingBootstrap = MakeBootstrap(99);
    validation.rawMetrics = MakeMetrics(conflictingBootstrap, 1.0f);
    pbremotevisualmetriccalibration::CalibrationReport output;
    output.canonicalJson = "sentinel";
    std::string error;
    REQUIRE_FALSE(pbremotevisualmetriccalibration::BuildCalibrationReport(observations, output, error));
    REQUIRE(error.find("baseline") != std::string::npos);
    REQUIRE(output.canonicalJson == "sentinel");
}

TEST_CASE("Calibration pipeline rejects an incomplete split manifest before touching file inputs",
    "[tools][remote-visual][calibration][pipeline][negative]")
{
    pbremotevisualmetriccalibration::CalibrationPipelineInput input;
    pbremotevisualmetriccalibration::CodecSequenceInput sequence;
    sequence.split = pbremotevisualmetriccalibration::DatasetSplit::Train;
    sequence.signalProfile = "PB-Signal-Test-1";
    sequence.runId = "train-only";
    sequence.gray8Path = "does-not-exist.gray";
    sequence.expectedBlake3 = std::string(64, '0');
    input.codecSequences.push_back(std::move(sequence));
    input.realReplay.collectionDatasetId = std::string(32, '0');
    input.realReplay.replayPath = "does-not-exist.pbrv2";
    input.realReplay.expectedReplayBlake3 = std::string(64, '0');
    input.realReplay.expectedPresenterRasterBlake3 = std::string(64, '0');
    pbremotevisualmetriccalibration::CalibrationPipelineReport output;
    output.canonicalJson = "sentinel";
    std::string error;
    REQUIRE_FALSE(pbremotevisualmetriccalibration::BuildCalibrationEvidence(input, output, error));
    REQUIRE(error.find("Train, Validation and Holdout") != std::string::npos);
    REQUIRE(output.canonicalJson == "sentinel");
}
