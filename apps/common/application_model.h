#pragma once

#include "pbprotocol/protocol_types.h"
#include "pbmodulation/visual_temporal.h"

#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pbapp
{

inline constexpr std::uint64_t maximumInstantFileBytes = 500ULL * 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t minimumInstantFileBytes = 0;
inline constexpr std::uint32_t phase1CanvasWidth = 1920;
inline constexpr std::uint32_t phase1CanvasHeight = 1080;

enum class VisualProfile : std::uint8_t
{
    DirectLevels2x2,
    ShapeChroma,
    RemoteVisualResilient,
    RemoteVisualLowFps
};

struct VisualProfileOption
{
    VisualProfile profile = VisualProfile::DirectLevels2x2;
    std::string_view cliToken;
    std::string_view displayName;
    bool remoteVisual = false;
    std::uint32_t defaultLogicalVisualFps = 0;
    std::uint32_t defaultControlRepetitions = 4;
};

enum class CaptureBackend : std::uint8_t
{
    Wgc,
    Dxgi
};

enum class EncoderState : std::uint8_t
{
    Idle,
    Preparing,
    Broadcasting,
    Stopping,
    Stopped,
    Failed
};

enum class DecoderState : std::uint8_t
{
    Idle,
    WaitingForBootstrap,
    AwaitingLargeOutputConfirmation,
    ReceivingControl,
    Receiving,
    Recovering,
    Verifying,
    Publishing,
    Completed,
    Failed,
    Stopping,
    Stopped
};

enum class TransitionResult : std::uint8_t
{
    Applied,
    NoChange,
    Rejected,
    Stale
};

enum class ChannelType : std::uint8_t
{
    LocalDesktop,
    RemoteVisual,
    Other
};

enum class ChromaMode : std::uint8_t
{
    Unknown,
    Chroma444,
    Chroma420
};

enum class MetadataProvenance : std::uint8_t
{
    NotProvided,
    Manual,
    PixelBridgeObserved,
    RemoteUiVisible
};

struct MetadataPhysicalRect
{
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
    bool operator==(const MetadataPhysicalRect&) const = default;
};

struct RuntimeCapabilities
{
    bool instantLocalDesktop = true;
    bool offlineMp4 = false;
    bool multiSegment = true;
    bool automaticCaptureFallback = false;
};

enum class LargeOutputConfirmationState : std::uint8_t
{
    NotRequired,
    AwaitingDecision,
    Accepted,
    Rejected
};

struct LargeOutputConfirmationSnapshot
{
    LargeOutputConfirmationState state = LargeOutputConfirmationState::NotRequired;
    std::uint64_t runGeneration = 0;
    std::uint64_t requestId = 0;
    pbprotocol::SessionId sessionId{};
    pbprotocol::SessionTag sessionTag{};
    std::uint64_t originalFileBytes = 0;
    std::string fileNameUtf8;
};

// Thread-safe Qt-free request/response boundary. The worker publishes one
// immutable request identity per Session; UI responses must echo both the run
// generation and request ID so stale callbacks cannot authorize allocation.
class LargeOutputConfirmationController
{
public:
    [[nodiscard]] TransitionResult BeginRun(std::uint64_t runGeneration) noexcept;
    [[nodiscard]] TransitionResult Request(std::uint64_t runGeneration,
        const pbprotocol::SessionDescriptor& session, pbprotocol::SessionTag sessionTag);
    [[nodiscard]] TransitionResult Resolve(std::uint64_t runGeneration,
        std::uint64_t requestId, bool accepted) noexcept;
    [[nodiscard]] LargeOutputConfirmationSnapshot GetSnapshot() const;

private:
    mutable std::mutex mutex_;
    LargeOutputConfirmationSnapshot snapshot_;
};

enum class WindowCloseAction : std::uint8_t
{
    Accept,
    RequestStopAndDefer,
    Defer
};

struct RemoteVisualRunMetadata
{
    std::string runId;
    ChannelType channelType = ChannelType::LocalDesktop;
    std::string remoteProvider;
    std::string providerVersion;
    std::string remoteMode;
    std::optional<double> targetFps;
    std::optional<double> observedFps;
    ChromaMode chromaMode = ChromaMode::Unknown;
    std::string computerBDisplayResolution;
    std::optional<double> computerBRefreshRate;
    std::string computerADisplayResolution;
    std::optional<double> computerARefreshRate;
    std::string remoteResolution;
    std::optional<MetadataPhysicalRect> remoteWindowPhysicalRect;
    std::optional<MetadataPhysicalRect> selectedRoiPhysicalRect;
    std::optional<double> estimatedScaleX;
    std::optional<double> estimatedScaleY;
    std::string letterboxStatus;
    std::string cropStatus;
    std::string geometryStatus;
    std::string networkType;
    std::optional<double> observedBandwidthMbps;
    std::optional<double> observedLatencyMilliseconds;
    std::string protectedMonitorIdentity;
    std::string experimentMonitorIdentity;
    MetadataProvenance remoteUiProvenance = MetadataProvenance::NotProvided;
    MetadataProvenance geometryProvenance = MetadataProvenance::NotProvided;
    MetadataProvenance networkProvenance = MetadataProvenance::NotProvided;
    std::string notes;
};

using RemoteRunMetadata = RemoteVisualRunMetadata;

struct EncoderSnapshot
{
    EncoderState state = EncoderState::Idle;
    std::uint64_t runGeneration = 0;
    std::string runId;
    std::uint64_t runStartedUnixMilliseconds = 0;
    std::optional<std::uint64_t> runEndedUnixMilliseconds;
    std::string sourcePath;
    std::uint64_t sourceBytes = 0;
    std::string sessionIdHex;
    std::uint64_t sessionTag = 0;
    std::string wholeFileDigestHex;
    VisualProfile visualProfile = VisualProfile::DirectLevels2x2;
    std::uint64_t visualProfileId = 0;
    std::uint8_t visualLayoutVersion = 0;
    std::uint32_t codedDataBytesPerFrame = 0;
    std::uint32_t codewordsPerFrame = 0;
    pbprotocol::CompressionCodec compressionCodec = pbprotocol::CompressionCodec::Raw;
    pbprotocol::OuterFecMode outerFecMode = pbprotocol::OuterFecMode::DirectRepeat;
    std::uint32_t outerBlockCount = 0;
    std::uint64_t broadcastRuntimeMilliseconds = 0;
    std::uint64_t cycleCount = 0;
    std::uint32_t cyclePosition = 0;
    std::uint32_t cycleFrameCount = 0;
    std::uint64_t currentSegmentOrdinal = 0;
    std::uint64_t segmentCount = 0;
    std::uint32_t currentOuterBlockId = 0;
    std::uint64_t frameSequence = 0;
    bool resumedSession = false;
    std::string sessionStateDirectory;
    std::uint64_t sessionStateGeneration = 0;
    std::uint64_t durableFrameSequenceLeaseEnd = 0;
    std::uint32_t durableRepairIdLeaseEnd = 0;
    std::uint64_t presentationEpoch = 0;
    std::optional<double> presentedVisualFps;
    std::optional<double> presentCallFps;
    // Interval-authoritative logical cadence: (N - 1) / (last - first).
    // The initial submitted frame is not treated as one elapsed interval.
    std::optional<double> generatedVisualFramesPerSecond;
    std::optional<double> generatedPayloadBytesPerSecond;
    std::uint64_t rawVisualBitsPerLogicalFrame = 0;
    std::uint64_t innerFecInformationBytesPerLogicalFrame = 0;
    std::uint64_t transportPayloadCeilingBytesPerLogicalFrame = 0;
    std::optional<std::uint64_t> configuredTransportPayloadCeilingBytesPerSecond;
    std::uint32_t configuredLogicalVisualFps = 0;
    std::optional<double> configuredLogicalDwellMilliseconds;
    std::optional<double> minimumObservedLogicalDwellMilliseconds;
    std::uint64_t logicalDwellViolationCount = 0;
    std::uint32_t configuredControlRepetitions = 4;
    std::uint64_t submittedFrames = 0;
    std::uint64_t replacedPendingFrames = 0;
    std::uint64_t sourceTextureReplacements = 0;
    std::uint64_t repeatedPresentCalls = 0;
    std::uint64_t invalidatedActiveFrames = 0;
    std::uint32_t pendingFrames = 0;
    std::uint32_t pendingHighWater = 0;
    bool activeFrame = false;
    std::uint64_t activeFrameSequence = 0;
    bool candidateContractSatisfied = false;
    bool sourceStable = true;
    std::int32_t dataWindowLeft = 0;
    std::int32_t dataWindowTop = 0;
    std::uint32_t dataWindowWidth = phase1CanvasWidth;
    std::uint32_t dataWindowHeight = phase1CanvasHeight;
    bool singleMonitorFullscreen = false;
    bool monitorSafetyPreflightPassed = false;
    std::uint64_t monitorSafetyRevalidationCount = 0;
    std::string monitorSafetyStatus;
    std::optional<double> processCpuAveragePercent;
    std::optional<double> processCpuPeakPercent;
    std::optional<double> processCpuEquivalentCores;
    std::string processCpuUnavailableReason;
    std::optional<double> processGpuEngineAveragePercent;
    std::optional<double> processGpuEnginePeakPercent;
    std::string processGpuUnavailableReason = "PID-scoped GPU Engine counter not sampled";
    bool journalEnabled = false;
    bool evidenceValid = true;
    bool journalTruncated = false;
    bool journalFinished = false;
    std::uint64_t journalSamples = 0;
    std::uint64_t journalBytes = 0;
    std::string evidenceInvalidReason;
    std::string statusMessage;
    std::string errorDetail;
    RemoteRunMetadata remoteMetadata;
};

struct ObservedLocatorGeometrySnapshot
{
    std::uint64_t samples = 0;
    std::optional<double> lastOriginX;
    std::optional<double> lastOriginY;
    std::optional<double> lastScaleX;
    std::optional<double> lastScaleY;
    std::optional<double> lastMarkerResidualPixels;
    std::optional<double> minimumOriginX;
    std::optional<double> maximumOriginX;
    std::optional<double> minimumOriginY;
    std::optional<double> maximumOriginY;
    std::optional<double> minimumScaleX;
    std::optional<double> maximumScaleX;
    std::optional<double> minimumScaleY;
    std::optional<double> maximumScaleY;
    std::optional<double> minimumMarkerResidualPixels;
    std::optional<double> maximumMarkerResidualPixels;
    std::optional<double> maximumScaleAnisotropy;
};

struct DecoderSnapshot
{
    DecoderState state = DecoderState::Idle;
    std::uint64_t runGeneration = 0;
    std::string runId;
    std::uint64_t runStartedUnixMilliseconds = 0;
    std::optional<std::uint64_t> runEndedUnixMilliseconds;
    CaptureBackend requestedBackend = CaptureBackend::Wgc;
    std::optional<CaptureBackend> actualBackend;
    std::string backendReason;
    VisualProfile visualProfile = VisualProfile::DirectLevels2x2;
    std::uint64_t visualProfileId = 0;
    std::uint8_t visualLayoutVersion = 0;
    std::uint32_t codedDataBytesPerFrame = 0;
    std::uint32_t codewordsPerFrame = 0;
    bool descriptorKnown = false;
    std::uint64_t originalFileBytes = 0;
    LargeOutputConfirmationState largeOutputConfirmationState = LargeOutputConfirmationState::NotRequired;
    std::uint64_t largeOutputConfirmationRequestId = 0;
    std::string largeOutputConfirmationFileNameUtf8;
    std::uint64_t outputAvailableBytesBeforeReservation = 0;
    std::uint64_t outputRequestedAllocationBytes = 0;
    std::uint64_t outputActualAllocationBytes = 0;
    bool outputPreallocationAttempted = false;
    bool outputPreallocationFullyAllocated = false;
    bool outputFileSparse = false;
    bool outputFileCompressed = false;
    bool outputVolumeSupportsSparseFiles = false;
    bool outputVolumeSupportsCompression = false;
    bool outputVolumeCompressed = false;
    bool outputRecoveredAfterPublish = false;
    std::uint64_t verifiedRawBytes = 0;
    std::uint64_t remainingRawBytes = 0;
    std::optional<double> recoveryProgress;
    double instantVerifiedRawGoodputBytesPerSecond = 0;
    double smoothedVerifiedRawGoodputBytesPerSecond = 0;
    double averageVerifiedRawGoodputBytesPerSecond = 0;
    std::uint64_t verifiedEncodedBytes = 0;
    std::optional<double> verifiedEncodedGoodputBitsPerSecond;
    std::optional<std::uint64_t> etaMilliseconds;
    std::string sessionIdHex;
    std::uint64_t sessionTag = 0;
    std::uint64_t segmentCount = 0;
    std::uint64_t currentSegmentOrdinal = 0;
    bool resumeStateLoaded = false;
    bool resumeStateTruncatedTail = false;
    std::uint64_t resumeStateGeneration = 0;
    std::uint64_t resumeStateBytes = 0;
    std::string resumeStatePath;
    pbprotocol::CompressionCodec compressionCodec = pbprotocol::CompressionCodec::Raw;
    pbprotocol::OuterFecMode outerFecMode = pbprotocol::OuterFecMode::DirectRepeat;
    std::uint64_t captureEpoch = 0;
    std::uint64_t captureEpochResets = 0;
    std::uint64_t captureArrivedFrames = 0;
    std::uint64_t captureCopiedFrames = 0;
    std::uint64_t captureDeliveredFrames = 0;
    std::uint64_t captureDroppedFrames = 0;
    std::uint64_t captureAcquireTimeouts = 0;
    std::uint64_t capturePointerOnlyFrames = 0;
    std::uint64_t captureAccumulatedFrames = 0;
    std::uint64_t captureAccessLostEvents = 0;
    std::uint64_t captureExpiredFrames = 0;
    std::uint64_t captureStaleFrames = 0;
    std::uint64_t captureCursorErasures = 0;
    std::uint64_t captureFrameAgeHighWater100ns = 0;
    std::uint64_t captureReadbackDropEvents = 0;
    std::uint64_t captureRecreates = 0;
    std::uint32_t captureDeviceRecoveries = 0;
    std::uint64_t telemetryCapturedFrames = 0;
    std::uint64_t telemetryDroppedFrames = 0;
    std::uint64_t fingerprintedFrames = 0;
    std::optional<double> captureFps;
    std::optional<double> roiPixelDigestUniqueVisualFps;
    std::optional<double> uniqueVisualFps;
    std::uint64_t frameSequenceGapEvents = 0;
    std::uint64_t skippedFrameSequences = 0;
    std::uint64_t duplicateFrameSequences = 0;
    std::uint64_t reorderedFrameSequences = 0;
    std::optional<double> admittedFrameSequenceFps;
    std::uint64_t telemetryBootstrapAttempts = 0;
    std::uint64_t telemetryBootstrapSuccesses = 0;
    std::optional<double> bootstrapSuccessRate;
    ObservedLocatorGeometrySnapshot observedLocatorGeometry;
    std::uint64_t bootstrapAcceptedFrames = 0;
    std::uint64_t bootstrapRejectedFrames = 0;
    std::uint64_t bootstrapMismatchFrames = 0;
    std::uint64_t bootstrapControlFrameFailures = 0;
    std::uint64_t evaluatedDataFrames = 0;
    std::uint64_t evaluatedCodewords = 0;
    std::uint64_t postFecFailedFrames = 0;
    std::optional<double> preFecBerEstimate;
    std::optional<double> fecFrameErrorRate;
    std::optional<double> fecCodewordFailureRate;
    std::uint64_t fecAcceptedTransportBlocks = 0;
    std::optional<double> fecAcceptedTransportBlockRate;
    // Historical field retained for report compatibility. It is the same
    // Receiver-bound count as temporallyAdmittedTransportBlocks, not the raw
    // number accepted by repeated FEC evaluations.
    std::uint64_t acceptedTransportBlocks = 0;
    std::uint64_t temporallyAdmittedTransportBlocks = 0;
    std::uint64_t comparedCodedBits = 0;
    std::uint64_t erroneousCodedBits = 0;
    std::uint64_t fecFailures = 0;
    std::uint64_t crcFailures = 0;
    std::uint64_t identityFailures = 0;
    std::uint64_t falseAcceptedCodewords = 0;
    bool falseAcceptedCodewordsAvailable = false;
    std::string falseAcceptedCodewordsUnavailableReason = "Production receive has no independent truth oracle";
    std::uint64_t endToEndUniqueFrameSequences = 0;
    std::optional<double> endToEndUniqueVisualFps;
    std::uint64_t remoteDuplicateRefinementAttempts = 0;
    std::uint64_t remoteDuplicateRefinementRecoveries = 0;
    std::uint64_t remoteMetricFrames = 0;
    std::uint64_t remoteMetricSamples = 0;
    std::uint64_t remoteZeroMagnitudeMetrics = 0;
    std::optional<double> remoteZeroMagnitudeMetricRate;
    std::optional<double> remoteMinimumAbsoluteMetric;
    std::optional<double> remoteMeanAbsoluteMetric;
    std::uint64_t remoteSymbolSamples = 0;
    std::uint64_t remoteUnreliableSymbols = 0;
    std::optional<double> remoteUnreliableSymbolRate;
    std::uint64_t remoteVerifiedMetricFrames = 0;
    std::uint64_t remoteRejectedMetricFrames = 0;
    std::optional<double> remoteVerifiedMeanAbsoluteMetric;
    std::optional<double> remoteRejectedMeanAbsoluteMetric;
    std::optional<double> remoteRejectedZeroMagnitudeMetricRate;
    std::uint64_t remoteFreshnessRegions = 0;
    std::uint64_t remoteFreshRegions = 0;
    std::uint64_t remoteStaleRegions = 0;
    std::optional<double> remoteStaleRegionRate;
    std::uint64_t remoteFramesWithStaleRegions = 0;
    std::uint64_t remoteFreshnessTagMismatches = 0;
    std::uint64_t remoteFreshnessTagErasures = 0;
    std::uint64_t remoteFreshnessErasedDataMetrics = 0;
    std::optional<double> remoteFreshnessErasedDataMetricRate;
    std::uint64_t outerUniqueSymbols = 0;
    std::uint64_t outerIdenticalDuplicateSymbols = 0;
    std::uint64_t outerRecoveryAlreadyReadySymbols = 0;
    std::uint64_t outerAlreadyCompletedSymbols = 0;
    std::uint64_t outerRecoveryReadyEvents = 0;
    std::uint64_t outerResourceRejections = 0;
    std::uint64_t outerConflictRejections = 0;
    std::uint64_t captureStallCount = 0;
    std::uint64_t captureStallTotalMilliseconds = 0;
    std::uint64_t captureStallMaximumMilliseconds = 0;
    bool captureStallActive = false;
    std::uint64_t visualStallCount = 0;
    std::uint64_t visualStallTotalMilliseconds = 0;
    std::uint64_t visualStallMaximumMilliseconds = 0;
    bool visualStallActive = false;
    std::optional<double> processCpuAveragePercent;
    std::optional<double> processCpuPeakPercent;
    std::optional<double> processCpuEquivalentCores;
    std::string processCpuUnavailableReason;
    std::optional<double> processGpuEngineAveragePercent;
    std::optional<double> processGpuEnginePeakPercent;
    std::string processGpuUnavailableReason = "PID-scoped GPU Engine counter not sampled";
    bool journalEnabled = false;
    bool evidenceValid = true;
    bool journalTruncated = false;
    bool journalFinished = false;
    std::uint64_t journalSamples = 0;
    std::uint64_t journalBytes = 0;
    std::string evidenceInvalidReason;
    bool monitorSafetyPreflightPassed = false;
    std::uint64_t monitorSafetyRevalidationCount = 0;
    std::string monitorSafetyStatus;
    bool replayEnabled = false;
    bool replayDiagnosticOnly = false;
    bool replayCaptureOnly = false;
    bool replayOfflineMode = false;
    bool replayEvidenceValid = true;
    bool replayFinalized = false;
    std::uint64_t replayWrittenFrames = 0;
    std::uint64_t replayDroppedFrames = 0;
    std::uint64_t replaySampledOutFrames = 0;
    std::uint32_t replayMaximumCaptureFramesPerSecond = 0;
    std::uint64_t replaySamplingInterval100ns = 0;
    std::uint64_t replayWrittenDemodObservations = 0;
    std::uint64_t replayDroppedDemodObservations = 0;
    std::uint32_t replayQueueHighWater = 0;
    std::uint64_t replayFileBytes = 0;
    std::uint64_t replayOfflineCaptureFrames = 0;
    std::uint64_t replayOfflineDemodResults = 0;
    std::uint64_t replayOfflineObservationComparisons = 0;
    std::uint64_t replayOfflineObservationMismatches = 0;
    std::string replayPath;
    std::string replayError;
    std::uint32_t frameLeaseHighWater = 0;
    std::uint32_t demodPendingHighWater = 0;
    std::uint32_t resultQueueHighWater = 0;
    std::uint64_t staleResultDrops = 0;
    std::uint64_t roiGpuTimeTotal100ns = 0;
    std::uint64_t demodGpuTimeTotal100ns = 0;
    std::uint64_t bootstrapCpuTimeTotal100ns = 0;
    std::uint64_t postGpuFecCpuTimeTotal100ns = 0;
    bool wholeFileDigestVerified = false;
    bool finalPublishSucceeded = false;
    std::string wholeFileDigestHex;
    std::string outputPath;
    std::uint64_t recoveryRuntimeMilliseconds = 0;
    std::int32_t roiLeft = 0;
    std::int32_t roiTop = 0;
    std::uint32_t roiWidth = 0;
    std::uint32_t roiHeight = 0;
    std::int32_t monitorLeft = 0;
    std::int32_t monitorTop = 0;
    std::uint32_t monitorWidth = 0;
    std::uint32_t monitorHeight = 0;
    std::uint32_t dpiX = 0;
    std::uint32_t dpiY = 0;
    std::uint32_t rotation = 0;
    std::string statusMessage;
    std::string errorDetail;
    RemoteRunMetadata remoteMetadata;
};

class EncoderStateMachine
{
public:
    [[nodiscard]] TransitionResult Start(std::uint64_t runGeneration) noexcept;
    [[nodiscard]] TransitionResult MarkBroadcasting(std::uint64_t runGeneration) noexcept;
    [[nodiscard]] TransitionResult RequestStop(std::uint64_t runGeneration) noexcept;
    [[nodiscard]] TransitionResult MarkStopped(std::uint64_t runGeneration) noexcept;
    [[nodiscard]] TransitionResult Fail(std::uint64_t runGeneration) noexcept;
    [[nodiscard]] EncoderState GetState() const noexcept;
    [[nodiscard]] std::uint64_t GetRunGeneration() const noexcept;

private:
    [[nodiscard]] bool IsCurrent(std::uint64_t runGeneration) const noexcept;
    EncoderState state_ = EncoderState::Idle;
    std::uint64_t runGeneration_ = 0;
};

class DecoderStateMachine
{
public:
    [[nodiscard]] TransitionResult Start(std::uint64_t runGeneration) noexcept;
    [[nodiscard]] TransitionResult Advance(std::uint64_t runGeneration, DecoderState nextState) noexcept;
    [[nodiscard]] TransitionResult RequestStop(std::uint64_t runGeneration) noexcept;
    [[nodiscard]] TransitionResult MarkStopped(std::uint64_t runGeneration) noexcept;
    [[nodiscard]] TransitionResult Fail(std::uint64_t runGeneration) noexcept;
    [[nodiscard]] DecoderState GetState() const noexcept;
    [[nodiscard]] std::uint64_t GetRunGeneration() const noexcept;

private:
    [[nodiscard]] bool IsCurrent(std::uint64_t runGeneration) const noexcept;
    DecoderState state_ = DecoderState::Idle;
    std::uint64_t runGeneration_ = 0;
};

struct CarouselSnapshot
{
    std::uint64_t cycleCount = 0;
    std::uint32_t cyclePosition = 0;
    std::uint32_t cycleFrameCount = 0;
};

class CarouselCounter
{
public:
    [[nodiscard]] bool Reset(std::uint32_t cycleFrameCount) noexcept;
    [[nodiscard]] bool Advance() noexcept;
    [[nodiscard]] CarouselSnapshot GetSnapshot() const noexcept;

private:
    CarouselSnapshot snapshot_;
};

struct ProgressSnapshot
{
    bool descriptorKnown = false;
    std::uint64_t totalRawBytes = 0;
    std::uint64_t verifiedRawBytes = 0;
    std::uint64_t remainingRawBytes = 0;
    std::optional<double> progress;
    double instantBytesPerSecond = 0;
    double smoothedBytesPerSecond = 0;
    double averageBytesPerSecond = 0;
    std::optional<std::uint64_t> etaMilliseconds;
    std::uint32_t positiveSampleCount = 0;
};

using VisualIdentitySnapshot = pbmodulation::VisualIdentitySnapshot;

struct StallIntervalSnapshot
{
    std::uint64_t count = 0;
    std::uint64_t totalMilliseconds = 0;
    std::uint64_t maximumMilliseconds = 0;
    std::uint64_t currentMilliseconds = 0;
    bool active = false;
};

struct ChannelStallSnapshot
{
    StallIntervalSnapshot capture;
    StallIntervalSnapshot visual;
};

using VisualIdentityDisposition = pbmodulation::VisualIdentityDisposition;
using VisualIdentityTracker = pbmodulation::VisualIdentityTracker;

// Telemetry only. A capture stall begins after one second without any new
// capture observation. A visual stall begins after one second of continued
// capture observations without a new legal FrameSequence. Neither state is an
// admission or acceptance input.
class ChannelStallTracker
{
public:
    void Observe(std::uint64_t monotonicMilliseconds, std::uint64_t captureObservations,
        std::uint64_t legalVisualObservations) noexcept;
    void ResetDomain(std::uint64_t monotonicMilliseconds, std::uint64_t captureObservations,
        std::uint64_t legalVisualObservations) noexcept;
    void Finish(std::uint64_t monotonicMilliseconds) noexcept;
    [[nodiscard]] ChannelStallSnapshot GetSnapshot() const noexcept;

private:
    static void StartInterval(StallIntervalSnapshot& interval, std::uint64_t startedMilliseconds,
        std::uint64_t& storedStartedMilliseconds) noexcept;
    static void EndInterval(StallIntervalSnapshot& interval, std::uint64_t endedMilliseconds,
        std::uint64_t& startedMilliseconds) noexcept;
    std::uint64_t lastObservationMilliseconds_ = 0;
    std::uint64_t lastCaptureChangeMilliseconds_ = 0;
    std::uint64_t lastVisualChangeMilliseconds_ = 0;
    std::uint64_t captureStallStartedMilliseconds_ = 0;
    std::uint64_t visualStallStartedMilliseconds_ = 0;
    std::uint64_t lastCaptureObservations_ = 0;
    std::uint64_t lastLegalVisualObservations_ = 0;
    ChannelStallSnapshot snapshot_;
    bool initialized_ = false;
};

struct RemoteDuplicateRefinementSnapshot
{
    std::uint64_t attempts = 0;
    std::uint64_t recoveries = 0;
    std::uint64_t limitDrops = 0;
    bool currentSequenceAdmitted = false;
};

// Remote codecs can progressively refine repeated presentations of one
// FrameSequence. At most the first later duplicate may be admitted when the
// initial capture produced no valid carrier. Successful admission makes the
// identity terminal; no frame data is combined across observations.
class RemoteDuplicateRefinementGate
{
public:
    void StartSequence(std::uint64_t captureEpoch, std::uint64_t frameSequence) noexcept;
    [[nodiscard]] bool ShouldAttemptDuplicate(std::uint64_t captureEpoch, std::uint64_t frameSequence,
        bool hasAcceptedCarrier) noexcept;
    [[nodiscard]] bool MarkAdmission(std::uint64_t captureEpoch, std::uint64_t frameSequence,
        bool duplicateRefinement) noexcept;
    void ResetEpoch() noexcept;
    [[nodiscard]] RemoteDuplicateRefinementSnapshot GetSnapshot() const noexcept;

private:
    std::uint64_t currentCaptureEpoch_ = 0;
    std::uint64_t currentFrameSequence_ = 0;
    RemoteDuplicateRefinementSnapshot snapshot_;
    bool hasCurrentSequence_ = false;
    bool currentSequenceAttempted_ = false;
};

// Uses verified raw-byte mutations only. Capture frames, symbols, sender rate,
// and theoretical profile capacity never enter this estimator.
class DecoderProgressTracker
{
public:
    [[nodiscard]] bool BindDescriptor(std::uint64_t totalRawBytes,
        std::uint64_t monotonicMilliseconds) noexcept;
    [[nodiscard]] bool ObserveVerifiedRawBytes(std::uint64_t verifiedRawBytes,
        std::uint64_t monotonicMilliseconds) noexcept;
    void ResetForCaptureEpoch(std::uint64_t monotonicMilliseconds) noexcept;
    void ObserveStall(std::uint64_t monotonicMilliseconds) noexcept;
    [[nodiscard]] ProgressSnapshot GetSnapshot() const noexcept;

private:
    void RecalculateEta() noexcept;
    ProgressSnapshot snapshot_;
    std::uint64_t startMilliseconds_ = 0;
    std::uint64_t lastSampleMilliseconds_ = 0;
    std::uint64_t lastPositiveSampleMilliseconds_ = 0;
    std::uint64_t lastVerifiedRawBytes_ = 0;
};

template <typename SnapshotType>
class SnapshotStore
{
public:
    [[nodiscard]] SnapshotType Get() const
    {
        const std::scoped_lock lock(mutex_);
        return snapshot_;
    }

    template <typename UpdateFunction>
    void Update(UpdateFunction&& update)
    {
        const std::scoped_lock lock(mutex_);
        update(snapshot_);
    }

    void Replace(SnapshotType snapshot)
    {
        const std::scoped_lock lock(mutex_);
        snapshot_ = std::move(snapshot);
    }

private:
    mutable std::mutex mutex_;
    SnapshotType snapshot_;
};

[[nodiscard]] const char* GetEncoderStateName(EncoderState state) noexcept;
[[nodiscard]] const char* GetDecoderStateName(DecoderState state) noexcept;
[[nodiscard]] const char* GetLargeOutputConfirmationStateName(LargeOutputConfirmationState state) noexcept;
[[nodiscard]] std::span<const VisualProfileOption> GetVisualProfileOptions() noexcept;
[[nodiscard]] const VisualProfileOption* FindVisualProfileOption(VisualProfile profile) noexcept;
[[nodiscard]] std::optional<VisualProfile> ParseVisualProfileToken(std::string_view token) noexcept;
[[nodiscard]] std::optional<VisualProfile> ParseVisualProfileToken(std::wstring_view token) noexcept;
[[nodiscard]] bool IsRemoteVisualProfile(VisualProfile profile) noexcept;
[[nodiscard]] const char* GetVisualProfileName(VisualProfile profile) noexcept;
[[nodiscard]] const char* GetCaptureBackendName(CaptureBackend backend) noexcept;
[[nodiscard]] const char* GetCompressionCodecName(pbprotocol::CompressionCodec codec) noexcept;
[[nodiscard]] const char* GetOuterFecModeName(pbprotocol::OuterFecMode mode) noexcept;
[[nodiscard]] const char* GetMetadataProvenanceName(MetadataProvenance provenance) noexcept;
[[nodiscard]] RuntimeCapabilities GetRuntimeCapabilities() noexcept;
[[nodiscard]] bool IsEncoderStateActive(EncoderState state) noexcept;
[[nodiscard]] bool IsDecoderStateActive(DecoderState state) noexcept;
[[nodiscard]] WindowCloseAction GetWindowCloseAction(bool active, bool closeAlreadyPending) noexcept;

} // namespace pbapp
