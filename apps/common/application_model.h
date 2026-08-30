#pragma once

#include "pbprotocol/protocol_types.h"

#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pbapp
{

inline constexpr std::uint64_t maximumInstantFileBytes = 8ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t minimumInstantFileBytes = 1;
inline constexpr std::uint32_t phase1CanvasWidth = 1920;
inline constexpr std::uint32_t phase1CanvasHeight = 1080;

enum class VisualProfile : std::uint8_t
{
    DirectLevels2x2,
    ShapeChroma
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
    SunloginRemoteVisual,
    Other
};

enum class ChromaMode : std::uint8_t
{
    Unknown,
    Chroma444,
    Chroma420
};

struct RuntimeCapabilities
{
    bool instantLocalDesktop = true;
    bool offlineMp4 = false;
    bool multiSegment = false;
    bool automaticCaptureFallback = false;
};

enum class WindowCloseAction : std::uint8_t
{
    Accept,
    RequestStopAndDefer,
    Defer
};

struct RemoteRunMetadata
{
    ChannelType channelType = ChannelType::LocalDesktop;
    std::string providerVersion;
    std::string remoteMode;
    std::optional<double> targetFps;
    std::optional<double> observedFps;
    ChromaMode chromaMode = ChromaMode::Unknown;
    std::string remoteResolution;
    std::string remoteWindowScale;
    std::string networkNote;
    std::optional<double> observedBandwidthMbps;
    std::optional<double> observedLatencyMilliseconds;
};

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
    std::uint64_t presentationEpoch = 0;
    std::optional<double> presentedVisualFps;
    std::optional<double> presentCallFps;
    double generatedVisualFramesPerSecond = 0;
    double generatedPayloadBytesPerSecond = 0;
    std::uint64_t submittedFrames = 0;
    std::uint64_t replacedPendingFrames = 0;
    std::uint32_t pendingFrames = 0;
    std::uint32_t pendingHighWater = 0;
    bool candidateContractSatisfied = false;
    bool sourceStable = true;
    std::int32_t dataWindowLeft = 0;
    std::int32_t dataWindowTop = 0;
    std::uint32_t dataWindowWidth = phase1CanvasWidth;
    std::uint32_t dataWindowHeight = phase1CanvasHeight;
    std::string statusMessage;
    std::string errorDetail;
    RemoteRunMetadata remoteMetadata;
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
    bool descriptorKnown = false;
    std::uint64_t originalFileBytes = 0;
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
    pbprotocol::CompressionCodec compressionCodec = pbprotocol::CompressionCodec::Raw;
    pbprotocol::OuterFecMode outerFecMode = pbprotocol::OuterFecMode::DirectRepeat;
    std::uint64_t captureEpoch = 0;
    std::uint64_t captureEpochResets = 0;
    std::uint64_t captureArrivedFrames = 0;
    std::uint64_t captureDeliveredFrames = 0;
    std::uint64_t captureDroppedFrames = 0;
    std::uint64_t captureRecreates = 0;
    std::uint32_t captureDeviceRecoveries = 0;
    std::uint64_t telemetryCapturedFrames = 0;
    std::uint64_t telemetryDroppedFrames = 0;
    std::uint64_t fingerprintedFrames = 0;
    std::optional<double> captureFps;
    std::optional<double> uniqueVisualFps;
    std::uint64_t frameSequenceGapEvents = 0;
    std::uint64_t skippedFrameSequences = 0;
    std::uint64_t duplicateFrameSequences = 0;
    std::uint64_t reorderedFrameSequences = 0;
    std::optional<double> admittedFrameSequenceFps;
    std::uint64_t telemetryBootstrapAttempts = 0;
    std::uint64_t telemetryBootstrapSuccesses = 0;
    std::optional<double> bootstrapSuccessRate;
    std::uint64_t bootstrapAcceptedFrames = 0;
    std::uint64_t bootstrapRejectedFrames = 0;
    std::uint64_t bootstrapMismatchFrames = 0;
    std::uint64_t bootstrapControlFrameFailures = 0;
    std::uint64_t evaluatedDataFrames = 0;
    std::uint64_t postFecFailedFrames = 0;
    std::optional<double> preFecBerEstimate;
    std::optional<double> fecFrameErrorRate;
    std::uint64_t acceptedTransportBlocks = 0;
    std::uint64_t comparedCodedBits = 0;
    std::uint64_t erroneousCodedBits = 0;
    std::uint64_t fecFailures = 0;
    std::uint64_t crcFailures = 0;
    std::uint64_t identityFailures = 0;
    std::uint64_t falseAcceptedCodewords = 0;
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

struct VisualIdentitySnapshot
{
    std::uint64_t uniqueFrames = 0;
    std::uint64_t duplicateFrames = 0;
    std::uint64_t reorderedFrames = 0;
    std::uint64_t gapEvents = 0;
    std::uint64_t skippedSequences = 0;
    std::optional<double> framesPerSecond;
};

enum class VisualIdentityDisposition : std::uint8_t
{
    Invalid,
    Unique,
    Duplicate,
    Reordered
};

// FrameSequence is authoritative only inside one CaptureEpoch. A new epoch
// establishes a fresh sequence baseline while the diagnostic counters remain
// cumulative for the current application run.
class VisualIdentityTracker
{
public:
    [[nodiscard]] VisualIdentityDisposition Observe(std::uint64_t sequence, std::uint64_t captureEpoch,
        std::int64_t timestamp100ns) noexcept;
    [[nodiscard]] VisualIdentitySnapshot GetSnapshot() const noexcept;

private:
    std::uint64_t maximumSequence_ = 0;
    std::uint64_t lastCaptureEpoch_ = 0;
    std::int64_t lastTimestamp100ns_ = 0;
    std::uint64_t uniqueFrames_ = 0;
    std::uint64_t duplicateFrames_ = 0;
    std::uint64_t reorderedFrames_ = 0;
    std::uint64_t gapEvents_ = 0;
    std::uint64_t skippedSequences_ = 0;
    std::uint64_t intervalCount_ = 0;
    std::uint64_t intervalTime100ns_ = 0;
    bool hasBaseline_ = false;
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
[[nodiscard]] const char* GetVisualProfileName(VisualProfile profile) noexcept;
[[nodiscard]] const char* GetCaptureBackendName(CaptureBackend backend) noexcept;
[[nodiscard]] const char* GetCompressionCodecName(pbprotocol::CompressionCodec codec) noexcept;
[[nodiscard]] const char* GetOuterFecModeName(pbprotocol::OuterFecMode mode) noexcept;
[[nodiscard]] RuntimeCapabilities GetRuntimeCapabilities() noexcept;
[[nodiscard]] bool IsEncoderStateActive(EncoderState state) noexcept;
[[nodiscard]] bool IsDecoderStateActive(DecoderState state) noexcept;
[[nodiscard]] WindowCloseAction GetWindowCloseAction(bool active, bool closeAlreadyPending) noexcept;

} // namespace pbapp
