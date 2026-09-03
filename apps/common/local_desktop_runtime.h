#pragma once

#include "application_model.h"
#include "monitor_catalog.h"

#include "pbcompression/segment_compression.h"
#include "pbrenderd3d/data_window.h"
#include "pbscreenregion/screen_region.h"

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace pbapp
{

struct EncoderConfig
{
    std::wstring sourcePath;
    bool compressionEnabled = false;
    int compressionLevel = 3;
    VisualProfile visualProfile = VisualProfile::DirectLevels2x2;
    std::optional<pbrenderd3d::PhysicalPoint> monitorClientOrigin;
    std::optional<MonitorSafetySelection> monitorSafety;
    // Explicit sender-only kiosk authority. This permits one selected monitor
    // to be occupied by the LF4 raster and therefore cannot satisfy the
    // dual-monitor ProtectedMonitor field-gate contract.
    std::optional<MonitorInfo> singleMonitorFullscreen;
    std::string runId;
    RemoteRunMetadata remoteMetadata;
    // Zero preserves the historical presentation-driven cadence for local
    // profiles only. RemoteVisual requires 1..5 and therefore establishes at
    // least 200 ms stable dwell; it never changes the encoded bytes.
    std::uint32_t logicalVisualFps = 0;
    std::uint32_t controlRepetitions = 4;
    // Empty selects %LOCALAPPDATA%\PixelBridge\EncoderSessions. Tests and
    // headless automation may provide an isolated root without changing wire
    // semantics.
    std::filesystem::path sessionStateRoot;
};

struct DecoderConfig
{
    std::wstring outputDirectory;
    CaptureBackend captureBackend = CaptureBackend::Auto;
    VisualProfile visualProfile = VisualProfile::DirectLevels2x2;
    pbscreenregion::ScreenCaptureRegion region;
    std::string runId;
    RemoteRunMetadata remoteMetadata;
    std::optional<MonitorSafetySelection> monitorSafety;
    // Empty disables replay. Enabling is diagnostic-only and excludes the run
    // from main goodput baselines; it never changes demod/Receiver acceptance.
    std::wstring replayOutputPath;
    // Allows a non-Phase-1 ROI only for bounded receiver-side replay capture.
    // No Bootstrap, demodulation, FEC, Receiver mutation, or publish is run.
    bool diagnosticCaptureOnly = false;
    // Optional descriptor identity for capture-only evidence. This only tags
    // the sealed receiver-side raster; it never selects a production demodulator.
    std::optional<std::uint64_t> replayEvidenceVisualProfileId;
    // Nonempty selects bounded offline Replay v2 input instead of live WGC or
    // DXGI capture. The replay adapter only reconstructs the PB-owned BGRA ROI
    // texture and then enters the same configured production
    // CaptureDemodulator/Receiver pipeline. The selected public profile remains
    // explicit; Replay input never auto-detects or changes its wire identity.
    std::wstring replayInputPath;
    std::uint32_t replayMaximumCaptureFrames = 256;
    std::uint64_t replayMaximumFileBytes = 2ULL * 1024 * 1024 * 1024;
    // Optional Replay evidence policy. Zero records every capture delivery;
    // 1..60 applies an authoritative pre-readback time sampler. For production
    // LF4 the primary GPU demodulator remains unsampled.
    std::uint32_t replayMaximumCaptureFramesPerSecond = 0;
};

struct RuntimeStatus
{
    bool success = true;
    std::string message;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return success;
    }

    [[nodiscard]] static RuntimeStatus Failure(std::string message)
    {
        return {false, std::move(message)};
    }
};

[[nodiscard]] RuntimeStatus ValidateEncoderConfig(const EncoderConfig& config);
[[nodiscard]] RuntimeStatus ValidateDecoderConfig(const DecoderConfig& config);
[[nodiscard]] pbcompression::CompressionResult<pbcompression::EncodedSegment> PrepareEncodedSegment(
    std::span<const std::byte> rawBytes, bool compressionEnabled, int compressionLevel);

struct EncoderCarouselProbeSnapshot
{
    std::uint64_t framesBuilt = 0;
    std::uint64_t controlFrames = 0;
    std::uint64_t dataFrames = 0;
    std::uint64_t acceptedRemoteControlCopies = 0;
    std::uint64_t acceptedTransportBlocks = 0;
    std::uint64_t completedCarouselCycles = 0;
    std::uint64_t framesBuiltAfterExternalCompletionMarker = 0;
    std::uint32_t cycleFrameCount = 0;
    std::uint64_t visualProfileId = 0;
    std::uint8_t layoutVersion = 0;
    std::uint32_t codedDataBytes = 0;
    std::uint32_t codewords = 0;
};

inline constexpr std::uint32_t encoderProbeNoOuterBlockId = 0xFFFFFFFFU;

struct EncoderStreamingSegmentProbeSnapshot
{
    std::uint64_t segmentOrdinal = 0;
    std::uint64_t rawBytes = 0;
    std::uint64_t encodedBytes = 0;
    pbprotocol::CompressionCodec compressionCodec = pbprotocol::CompressionCodec::Raw;
    pbprotocol::OuterFecMode outerFecMode = pbprotocol::OuterFecMode::DirectRepeat;
    std::uint32_t systematicBlockCount = 0;
    std::uint64_t repairEquationsPerRound = 0;
    std::uint64_t paddingDuplicateSlotsPerRound = 0;
    std::uint32_t firstRepairIdPass0 = encoderProbeNoOuterBlockId;
    std::uint32_t firstRepairIdPass1 = encoderProbeNoOuterBlockId;
    std::uint32_t persistedRepairLeaseEnd = 0;
    std::uint32_t restartedRepairIdStart = 0;
};

struct EncoderStreamingCarouselProbeSnapshot
{
    std::uint64_t sourceBytes = 0;
    std::uint64_t segmentCount = 0;
    pbprotocol::SessionId sessionId{};
    std::array<std::byte, pbprotocol::kDigestBytes> wholeFileDigest{};
    std::uint64_t completedCarouselPasses = 0;
    std::uint64_t scheduledFrames = 0;
    std::uint64_t controlFrames = 0;
    std::uint64_t dataFrames = 0;
    std::uint64_t scheduledSystematicEquations = 0;
    std::uint64_t scheduledRepairEquations = 0;
    std::uint64_t paddingDuplicateSlots = 0;
    std::uint64_t descriptorResidentEncodedBytes = 0;
    std::uint32_t peakResidentEncodedSegmentCount = 0;
    std::uint64_t peakResidentEncodedSegmentBytes = 0;
    std::uint64_t initialFrameSequence = 0;
    std::uint64_t lastUsedFrameSequence = 0;
    std::uint64_t persistedFrameSequenceLeaseEnd = 0;
    std::uint64_t restartedFrameSequenceStart = 0;
    bool sourceWriteShareDenied = false;
    bool sourceDeleteShareDenied = false;
    bool restartWasResumed = false;
    bool equationReproducible = false;
    std::vector<EncoderStreamingSegmentProbeSnapshot> segments;
};

// Narrow test seam over the production SenderFrameBuilder. The external
// completion marker is deliberately not passed into the builder: the probe
// proves that sender carousel progression has no receiver-completion input.
class EncoderRuntimeTestAccess
{
public:
    [[nodiscard]] static RuntimeStatus ProbeRemoteVisualLowFpsCarousel(std::span<const std::byte> rawBytes,
        std::uint32_t controlRepetitions, std::uint32_t completedCyclesBeforeMarker,
        EncoderCarouselProbeSnapshot& output) noexcept;
    [[nodiscard]] static RuntimeStatus ProbeRemoteVisualFullscreenComposition(std::span<const std::byte> source,
        std::uint32_t destinationWidth, std::uint32_t destinationHeight,
        std::vector<std::byte>& output) noexcept;
    // Opens the real source handle and exercises production pre-scan,
    // descriptor persistence, current/next encoded Segment buffering,
    // Carousel scheduling, durable ID leases, and restart recreation without
    // creating a DataWindow or performing any screen operation.
    [[nodiscard]] static RuntimeStatus ProbeStreamingCarouselFile(const std::wstring& sourcePath,
        const std::filesystem::path& sessionStateRoot, bool compressionEnabled, int compressionLevel,
        std::uint32_t logicalVisualFps, std::uint32_t completedPasses,
        EncoderStreamingCarouselProbeSnapshot& output) noexcept;
};

struct DecoderAdmissionProbeSnapshot
{
    DecoderSnapshot decoder;
    pbprotocol::OuterFecMode outerFecMode = pbprotocol::OuterFecMode::DirectRepeat;
    std::uint64_t processedResults = 0;
    std::uint64_t suppressedDuplicateResults = 0;
    std::uint64_t duplicateRefinementResults = 0;
    std::uint64_t rawAcceptedTransportBlocks = 0;
    std::uint64_t temporallyAdmittedTransportBlocks = 0;
};

struct DecoderReplayProbeSnapshot
{
    DecoderSnapshot decoder;
    std::uint64_t replayFileBytes = 0;
    std::uint64_t captureFrames = 0;
    std::uint64_t demodObservations = 0;
    std::uint64_t droppedCaptureFrames = 0;
    std::uint64_t droppedDemodObservations = 0;
    std::uint64_t duplicateSuppressedResults = 0;
    std::uint32_t recorderQueueHighWater = 0;
};

struct ApplicationHeadlessSegmentProbeSnapshot
{
    std::uint64_t segmentOrdinal = 0;
    std::uint64_t rawBytes = 0;
    std::uint64_t encodedBytes = 0;
    pbprotocol::CompressionCodec compressionCodec = pbprotocol::CompressionCodec::Raw;
    pbprotocol::OuterFecMode outerFecMode = pbprotocol::OuterFecMode::DirectRepeat;
    std::uint32_t systematicBlockCount = 0;
    std::uint64_t submittedSystematicBlocks = 0;
    std::uint64_t submittedRepairBlocks = 0;
    std::uint64_t intentionallySkippedSystematicBlocks = 0;
};

struct ApplicationHeadlessProbeOptions
{
    bool compressionEnabled = false;
    int compressionLevel = 3;
    bool exerciseFourActiveBusy = false;
};

struct ApplicationHeadlessProbeSnapshot
{
    std::uint64_t sourceBytes = 0;
    std::uint64_t segmentCount = 0;
    pbprotocol::SessionId sessionId{};
    std::array<std::byte, pbprotocol::kDigestBytes> wholeFileDigest{};
    std::filesystem::path publishedPath;
    std::uint64_t processedControlRecords = 0;
    std::uint64_t repeatedControlRecords = 0;
    std::uint64_t reversedSegmentDescriptors = 0;
    std::uint64_t exactDuplicateSegmentDescriptors = 0;
    std::uint64_t preDescriptorOrphanBlocks = 0;
    std::uint64_t submittedSystematicBlocks = 0;
    std::uint64_t submittedRepairBlocks = 0;
    std::uint64_t intentionallySkippedSystematicBlocks = 0;
    std::uint64_t deferredResourceBusyCount = 0;
    std::uint64_t successfulBusyRetryCount = 0;
    std::uint32_t peakSenderResidentEncodedSegmentCount = 0;
    std::uint64_t peakSenderResidentEncodedSegmentBytes = 0;
    std::uint64_t peakReceiverActiveOuterFecDecoderCount = 0;
    std::uint64_t peakReceiverReservedOuterFecDecoderBytes = 0;
    std::uint64_t peakReceiverOrphanCachedBytes = 0;
    std::uint64_t peakReceiverResumeActivePayloadBytes = 0;
    std::uint64_t peakReceiverResumePendingPayloadBytes = 0;
    std::uint64_t peakReceiverResumeResidentPayloadBytes = 0;
    std::uint64_t receiverActiveOuterFecDecoderLimit = 0;
    std::uint64_t receiverTotalOuterFecDecoderByteLimit = 0;
    std::uint64_t receiverResumeByteLimit = 0;
    bool sourceStable = false;
    bool authoritativePublish = false;
    DecoderSnapshot decoder;
    std::vector<ApplicationHeadlessSegmentProbeSnapshot> segments;
};

struct ApplicationLargeOutputConfirmationProbeSnapshot
{
    DecoderSnapshot awaitingDecision;
    DecoderSnapshot afterDecision;
    std::uint64_t orphanCachedBytesBeforeDecision = 0;
    bool transportSuppressedBeforeDecision = false;
    bool partExistedBeforeDecision = false;
    bool resumeExistedBeforeDecision = false;
    bool partExistsAfterDecision = false;
    bool resumeExistsAfterDecision = false;
};

// Narrow headless seam over the production LF4 SenderFrameBuilder,
// ReferenceChannel truth boundary, ReceiverPipeline, ReceiverIngress,
// WholeFileDigest verification, and PBStorage publish path. This seam remains
// useful as deterministic headless coverage after public LF4 exposure.
class DecoderRuntimeTestAccess
{
public:
    [[nodiscard]] static RuntimeStatus ProbeRemoteVisualLowFpsReceiver(std::span<const std::byte> rawBytes,
        const std::wstring& outputDirectory, std::uint32_t suppressedDuplicateResults,
        DecoderAdmissionProbeSnapshot& output) noexcept;
    // Headless production LF4 source raster -> WARP CaptureDemodulator ->
    // ReceiverPipeline path with the actual bounded asynchronous Replay v2
    // recorder. Captures contain only the selected receiver ROI; sender truth
    // and canonical Bootstrap remain absent by construction.
    [[nodiscard]] static RuntimeStatus ProbeRemoteVisualLowFpsReplay(std::span<const std::byte> rawBytes,
        const std::wstring& replayPath, const std::wstring& outputDirectory,
        std::uint32_t duplicateFrames, DecoderReplayProbeSnapshot& output) noexcept;
};

// Narrow no-raster checkpoint seam over the same durable Sender preparation,
// SenderFrameBuilder Transport serialization, ReceiverPipeline, ReceiverIngress,
// resume journal, PBStorage and authoritative publish used by the application.
// It does not encode, display, capture or demodulate pixels.
class ApplicationRuntimeTestAccess
{
public:
    [[nodiscard]] static RuntimeStatus ProbeHeadlessMultiSegmentFile(const std::wstring& sourcePath,
        const std::filesystem::path& sessionStateRoot, const std::wstring& outputDirectory,
        const ApplicationHeadlessProbeOptions& options, ApplicationHeadlessProbeSnapshot& output) noexcept;
    [[nodiscard]] static RuntimeStatus ProbeLargeOutputConfirmation(std::span<const std::byte> rawBytes,
        const std::wstring& outputDirectory, bool accepted,
        ApplicationLargeOutputConfirmationProbeSnapshot& output) noexcept;
};

// Qt-free application controller. Start launches one bounded worker and
// returns immediately. UI code reads immutable snapshot copies; it never owns
// DataWindow/FEC resources. Stop is idempotent and joins before destruction.
class EncoderRuntime
{
public:
    EncoderRuntime() = default;
    ~EncoderRuntime();
    EncoderRuntime(const EncoderRuntime&) = delete;
    EncoderRuntime& operator=(const EncoderRuntime&) = delete;

    [[nodiscard]] RuntimeStatus Start(const EncoderConfig& config);
    [[nodiscard]] RuntimeStatus SetLogicalVisualFps(std::uint32_t logicalVisualFps) noexcept;
    void RequestStop() noexcept;
    void Stop() noexcept;
    [[nodiscard]] EncoderSnapshot GetSnapshot() const;

private:
    void Run(EncoderConfig config, std::uint64_t runGeneration) noexcept;
    mutable std::mutex lifecycleMutex_;
    SnapshotStore<EncoderSnapshot> snapshot_;
    std::thread worker_;
    std::atomic<bool> stopRequested_ = false;
    std::atomic<bool> workerRunning_ = false;
    std::atomic<std::uint32_t> requestedLogicalVisualFps_ = 0;
    std::uint64_t nextRunGeneration_ = 1;
};

class DecoderRuntime
{
public:
    DecoderRuntime() = default;
    ~DecoderRuntime();
    DecoderRuntime(const DecoderRuntime&) = delete;
    DecoderRuntime& operator=(const DecoderRuntime&) = delete;

    [[nodiscard]] RuntimeStatus Start(const DecoderConfig& config);
    [[nodiscard]] RuntimeStatus ResolveLargeOutputConfirmation(std::uint64_t runGeneration,
        std::uint64_t requestId, bool accepted) noexcept;
    void RequestStop() noexcept;
    void Stop() noexcept;
    [[nodiscard]] DecoderSnapshot GetSnapshot() const;

private:
    void Run(const DecoderConfig& config, std::uint64_t runGeneration) noexcept;
    mutable std::mutex lifecycleMutex_;
    SnapshotStore<DecoderSnapshot> snapshot_;
    std::thread worker_;
    std::atomic<bool> stopRequested_ = false;
    std::atomic<bool> workerRunning_ = false;
    LargeOutputConfirmationController largeOutputConfirmation_;
    std::uint64_t nextRunGeneration_ = 1;
};

} // namespace pbapp
