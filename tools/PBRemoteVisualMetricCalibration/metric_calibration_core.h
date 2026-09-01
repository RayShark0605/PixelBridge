#pragma once

#include "pbmodulation/local_desktop_bootstrap.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pbremotevisualmetriccalibration
{

inline constexpr char kMetricCalibrationSchema[] = "PixelBridge.RemoteVisualMetricCalibration.1";
inline constexpr std::uint32_t kMetricCalibrationVersion = 1;
inline constexpr char kDefaultPolicyId[] = "lf4-default";

enum class DatasetSplit : std::uint8_t
{
    Train,
    Validation,
    Holdout,
    External
};

enum class TruthAvailability : std::uint8_t
{
    DiagnosticSenderFixture,
    UnavailableReceiverOnly
};

// One immutable result of running the production LF4 modulation decoder with one
// named policy over one raster. The source Bootstrap is sender truth supplied by
// the dataset builder and never enters modulation. receiverBootstrap is populated
// only when the production modulation decoder accepted the frame.
struct MetricObservation
{
    std::string datasetId;
    std::string runId;
    std::string frameId;
    std::string signalProfile;
    std::string policyId;
    std::string policyManifestJson;
    bool policyRelaxesDefaultAdmission = false;
    DatasetSplit split = DatasetSplit::Train;
    TruthAvailability truthAvailability = TruthAvailability::DiagnosticSenderFixture;
    std::array<std::byte, pbmodulation::kLocalDesktopBootstrapRecordBytes> sourceBootstrap{};
    std::array<std::byte, pbmodulation::kLocalDesktopBootstrapRecordBytes> receiverBootstrap{};
    bool receiverBootstrapAvailable = false;
    bool modulationAccepted = false;
    std::string modulationErasure;
    std::vector<float> rawMetrics;
};

enum class MetricModelKind : std::uint8_t
{
    Raw,
    GlobalScale,
    PerSignalScale,
    PiecewiseLookup
};

struct ReliabilityBin
{
    double probabilityUpper = 0;
    std::uint64_t samples = 0;
    std::uint64_t errors = 0;
    double predictedErrorSum = 0;
};

struct ConfidencePoint
{
    double minimumMagnitude = 0;
    std::uint64_t samples = 0;
    std::uint64_t errors = 0;
};

struct RocPoint
{
    double minimumOneScore = 0;
    std::uint64_t truePositive = 0;
    std::uint64_t falsePositive = 0;
};

struct EvaluationSummary
{
    std::uint32_t frames = 0;
    std::uint32_t truthFrames = 0;
    std::uint32_t receiverOnlyFrames = 0;
    std::uint32_t modulationErasures = 0;
    std::uint32_t verifiedFrames = 0;
    std::uint64_t comparedBits = 0;
    std::uint64_t bitErrors = 0;
    std::uint64_t expectedZeroBits = 0;
    std::uint64_t expectedOneBits = 0;
    std::uint64_t zeroBitErrors = 0;
    std::uint64_t oneBitErrors = 0;
    std::uint64_t highConfidenceBits = 0;
    std::uint64_t falseConfidenceErrors = 0;
    std::uint64_t expectedTransportBlocks = 0;
    std::uint64_t acceptedTransportBlocks = 0;
    std::uint64_t falseAcceptedTransportBlocks = 0;
    std::uint64_t acceptedControlBlocks = 0;
    std::uint64_t falseAcceptedControlBlocks = 0;
    std::uint64_t fecFailures = 0;
    std::uint64_t crcFailures = 0;
    std::uint64_t identityFailures = 0;
    std::uint64_t iterationsTotal = 0;
    std::uint32_t iterationsMaximum = 0;
    double logLossSum = 0;
    double brierScoreSum = 0;
    double expectedCalibrationError = 0;
    std::vector<ReliabilityBin> reliability;
    std::vector<ConfidencePoint> confidenceCurve;
    std::vector<RocPoint> rocCurve;

    [[nodiscard]] bool IsAcceptanceSafe() const noexcept
    {
        return falseAcceptedTransportBlocks == 0 && falseAcceptedControlBlocks == 0;
    }
};

struct ScaleBinding
{
    std::string signalProfile;
    double scale = 1;
};

struct PiecewiseBinding
{
    double rawMagnitudeUpper = 0;
    double calibratedMagnitude = 0;
    std::uint64_t trainingSamples = 0;
    std::uint64_t trainingErrors = 0;
};

struct MetricModel
{
    MetricModelKind kind = MetricModelKind::Raw;
    double globalScale = 1;
    std::vector<ScaleBinding> signalScales;
    std::vector<PiecewiseBinding> piecewise;
};

struct CandidateReport
{
    std::string candidateId;
    std::string policyId;
    std::string policyManifestJson;
    MetricModel model;
    bool deployableWithoutSignalProfileBinding = true;
    bool policyRelaxesDefaultAdmission = false;
    EvaluationSummary train;
    EvaluationSummary validation;
    EvaluationSummary holdout;
    EvaluationSummary external;
};

struct DatasetGroupReport
{
    std::string datasetId;
    std::string runId;
    std::string signalProfile;
    DatasetSplit split = DatasetSplit::Train;
    TruthAvailability truthAvailability = TruthAvailability::DiagnosticSenderFixture;
    std::uint32_t frames = 0;
};

struct CalibrationReport
{
    std::uint32_t inputObservations = 0;
    std::uint32_t inputFrames = 0;
    std::string inputBlake3;
    std::vector<DatasetGroupReport> datasetGroups;
    std::string selectedCandidateId;
    std::string baselineCandidateId;
    bool splitIsolationValid = false;
    bool selectionUsedHoldout = false;
    bool acceptanceAuthorityChanged = false;
    bool productionDefaultsChanged = false;
    bool holdoutImproved = false;
    bool externalImproved = false;
    bool gatePassed = false;
    std::vector<CandidateReport> candidates;
    std::string canonicalJson;
};

// Fits every policy using Train observations, selects one deployable candidate
// using Validation only, and only then evaluates the frozen selection on Holdout
// and External. Every accepted block is compared with sender truth after the
// production Transport boundary; receiver-only observations retain unavailable
// false-accept denominators and cannot satisfy the external-improvement Gate.
// Failure leaves output unchanged.
[[nodiscard]] bool BuildCalibrationReport(std::span<const MetricObservation> observations,
    CalibrationReport& output, std::string& error) noexcept;

[[nodiscard]] const char* GetDatasetSplitName(DatasetSplit split) noexcept;
[[nodiscard]] const char* GetTruthAvailabilityName(TruthAvailability availability) noexcept;
[[nodiscard]] const char* GetMetricModelKindName(MetricModelKind kind) noexcept;

} // namespace pbremotevisualmetriccalibration
