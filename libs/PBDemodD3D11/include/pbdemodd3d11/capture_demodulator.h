#pragma once

#include "pbdemodd3d11/demodulator.h"
#include "pbmodulation/local_desktop_decode.h"
#include "pbmodulation/reference_visual_profile.h"
#include "pbmodulation/visual_temporal.h"
#include "pbprotocol/bootstrap_control_codec.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace pbdemodd3d11
{

inline constexpr std::uint32_t maximumCaptureDemodResultQueue = 256;
inline constexpr std::uint32_t maximumCaptureDemodDuplicateRefinementAttempts = 4;
inline constexpr std::size_t localDesktopErasureCount =
    static_cast<std::size_t>(pbmodulation::LocalDesktopErasureReason::ExcessResidual) + 1;

enum class CaptureDemodulatorResultKind : std::uint8_t
{
    Transport = 0, ControlRecord = 1, ControlFragment = 2, TelemetryOnly = 3, UnifiedFrame = 4
};

enum class CaptureDemodulatorGeometryStatus : std::uint8_t
{
    NotApplicable, ExactCanvas, Scaled, Letterboxed, Rejected
};

enum class CaptureDemodulatorTemporalDisposition : std::uint8_t
{
    NotApplicable, Unique, DuplicateRefinement, DuplicateSuppressed, Reordered, StaleCompletion
};

struct CaptureDemodulatorResult
{
    CaptureDemodulatorResultKind kind = CaptureDemodulatorResultKind::Transport;
    pbcapturenormalize::ScreenCaptureFrameMetadata metadata;
    std::array<std::byte, pbprotocol::kBootstrapRecordBytes> bootstrapRecord{};
    std::array<std::byte, pbmodulation::kReferenceControlWindowBytes> controlBytes{};
    std::uint32_t controlByteCount = 0;
    pbmodulation::LocalDesktopObservation bootstrap;
    CaptureDemodulatorGeometryStatus geometryStatus = CaptureDemodulatorGeometryStatus::NotApplicable;
    CaptureDemodulatorTemporalDisposition temporalDisposition = CaptureDemodulatorTemporalDisposition::NotApplicable;
    DemodFrameResult demodulation;
    // Indices into demodulation.acceptedTransportBlocks that are newly admitted
    // by the bounded LF4 temporal gate. Non-Unified strict profiles expose
    // every accepted Transport block here. Unified accepted blocks remain in
    // demodulation.acceptedUnifiedBlocks. Consumers must not infer temporal
    // admission from the raw observation count in DemodFrameResult.
    std::array<std::uint32_t, pbdesktoplevels::kMaximumCodewords> admittedTransportBlockIndices{};
    std::uint32_t admittedTransportBlockCount = 0;
    std::array<std::uint32_t, pbmodulation::kRemoteVisualLowFpsCodewords> admittedRemoteControlBlockIndices{};
    std::uint32_t admittedRemoteControlBlockCount = 0;
};

struct CaptureDemodulatorConfig
{
    std::uint64_t visualProfileId = 0;
    std::uint32_t slotCount = 3;
    std::uint32_t maximumFrameAgeMilliseconds = 250;
    std::uint32_t resultQueueCapacity = 64;
    std::uint64_t maximumResidentBytes = 128ULL * 1024 * 1024;
    pbdesktoplevels::EvaluationMode evaluationMode = pbdesktoplevels::EvaluationMode::Transport;
    // LF4 and Unified only: the reservation is calculated from these hard
    // bounds before CaptureNormalize allocates its pool/ring. Historical strict
    // 1:1 profiles continue to reserve and require the canonical canvas.
    std::uint32_t maximumRoiWidth = 3840;
    std::uint32_t maximumRoiHeight = 2160;
    pbmodulation::RemoteVisualLowFpsDecodePolicy remoteVisualLowFpsPolicy;
    // LF4 only. A duplicate may re-evaluate missing codeword slots this many
    // times after the first observation. Zero suppresses every duplicate.
    std::uint32_t maximumDuplicateRefinementAttempts = 1;
    pbmodulation::UnifiedVisualDecodePolicy unifiedVisualPolicy;
};

struct CaptureDemodulatorBudget
{
    std::uint64_t demodulatorBytes = 0;
    std::uint64_t bootstrapStagingBytes = 0;
    std::uint64_t referenceScratchBytes = 0;
    std::uint64_t resultQueueBytes = 0;
    std::uint64_t fixedOverheadBytes = 0;
    std::uint64_t totalBytes = 0;
    bool operator==(const CaptureDemodulatorBudget&) const = default;
};

struct CaptureDemodulatorSnapshot
{
    bool active = false;
    pbcapturenormalize::ScreenCaptureDomain domain;
    pbcapturenormalize::CaptureStatus error;
    DemodStatus lastDemodStatus;
    CaptureDemodulatorBudget reservation;
    DemodSnapshot demodulator;
    std::uint64_t domainStarts = 0;
    std::uint64_t invalidations = 0;
    std::uint64_t submittedFrames = 0;
    std::uint64_t completedFrames = 0;
    std::uint64_t cancelledFrames = 0;
    std::uint64_t expiredFrames = 0;
    std::uint64_t captureErasures = 0;
    std::uint64_t bootstrapAcceptedFrames = 0;
    std::uint64_t bootstrapRejectedFrames = 0;
    std::uint64_t controlFrames = 0;
    std::uint64_t controlFrameFailures = 0;
    std::uint64_t stagedGpuSubmissions = 0;
    std::uint64_t stagedGpuCompletions = 0;
    std::uint64_t exactGeometryFrames = 0;
    std::uint64_t scaledGeometryFrames = 0;
    std::uint64_t letterboxedGeometryFrames = 0;
    std::uint64_t rejectedGeometryFrames = 0;
    CaptureDemodulatorGeometryStatus lastGeometryStatus = CaptureDemodulatorGeometryStatus::NotApplicable;
    pbmodulation::LocalDesktopGeometry lastGeometry;
    std::uint64_t temporalUniqueFrames = 0;
    std::uint64_t temporalDuplicateFrames = 0;
    std::uint64_t temporalReorderedFrames = 0;
    std::uint64_t temporalGapEvents = 0;
    std::uint64_t temporalSkippedSequences = 0;
    std::uint64_t duplicateRefinementAttempts = 0;
    std::uint64_t duplicateRefinementRecoveries = 0;
    std::uint64_t duplicateRefinementLimitDrops = 0;
    std::uint64_t temporalSuppressedFrames = 0;
    std::uint64_t temporalStaleCompletionDrops = 0;
    std::uint64_t temporallyAdmittedTransportBlocks = 0;
    std::array<std::uint64_t, localDesktopErasureCount> bootstrapErasures{};
    std::uint64_t bootstrapMapCalls = 0;
    std::uint64_t bootstrapReadbackBytes = 0;
    std::uint64_t bootstrapCpuTimeTotal100ns = 0;
    std::uint64_t bootstrapCpuTimeHighWater100ns = 0;
    std::uint64_t bootstrapCpuTimingSamples = 0;
    std::uint64_t demodulationCpuTimeTotal100ns = 0;
    std::uint64_t demodulationCpuTimeHighWater100ns = 0;
    std::uint64_t demodulationCpuTimingSamples = 0;
    std::uint64_t cpuTimingUnavailable = 0;
    std::uint64_t demodulationRejectedFrames = 0;
    std::uint64_t verifiedFrames = 0;
    std::uint64_t postFecFailedFrames = 0;
    std::uint64_t acceptedTransportBlocks = 0;
    std::uint64_t resultQueueDrops = 0;
    std::uint64_t staleResultDrops = 0;
    std::uint64_t resultsTaken = 0;
    std::uint32_t pendingFrames = 0;
    std::uint32_t pendingHighWater = 0;
    std::uint32_t queuedResults = 0;
    std::uint32_t resultQueueHighWater = 0;
    std::uint64_t acceptedUnifiedBlocks = 0;
};

// Calculates every fixed allocation before capture pool/ring creation. Failure
// leaves output unchanged.
[[nodiscard]] pbcapturenormalize::CaptureStatus CalculateCaptureDemodulatorBudget(
    const CaptureDemodulatorConfig& config, CaptureDemodulatorBudget& output) noexcept;

// Same-frame LocalDesktop Bootstrap plus D3D11 metric/FEC consumer. Strict 1:1
// profiles queue Bootstrap staging and GPU demodulation in Submit. LF4 and
// Unified layout 8 queue only Bootstrap staging there; their first staged
// completion resolves continuous geometry and then submits direct-texture GPU
// work while the exact ROI remains leased. Results enter a fixed ring after a
// retirement marker. TelemetryOnly reports a Bootstrap/signal erasure or a
// frame without protocol payload, while Transport carries only blocks that
// passed QC-LDPC, canonical framing/CRC, and Bootstrap SessionTag validation;
// UnifiedFrame exposes the corresponding mixed Control/Transport result.
class CaptureDemodulator final : public pbcapturenormalize::ScreenCaptureConsumer
{
public:
    [[nodiscard]] static pbcapturenormalize::CaptureStatus Create(const CaptureDemodulatorConfig& config,
        std::shared_ptr<CaptureDemodulator>& output) noexcept;
    ~CaptureDemodulator() override;
    [[nodiscard]] std::uint64_t ReservedBytes() const noexcept override;
    [[nodiscard]] pbcapturenormalize::CaptureStatus ValidateConfiguration(
        const pbcapturenormalize::CaptureConfig& config) const noexcept override;
    [[nodiscard]] pbcapturenormalize::CaptureStatus DomainStarted(
        const pbcapturenormalize::ScreenCaptureDomain& domain,
        const pbcapturenormalize::CaptureEnvironment& environment, ID3D11Device* device) override;
    void DomainInvalidated(const pbcapturenormalize::ScreenCaptureDomain& domain) noexcept override;
    [[nodiscard]] pbcapturenormalize::CaptureStatus Submit(
        const pbcapturenormalize::ScreenCaptureFrame& frame, ID3D11DeviceContext* context) override;
    [[nodiscard]] pbcapturenormalize::CaptureConsumerCompletion CompleteStage(
        const pbcapturenormalize::ScreenCaptureFrameMetadata& metadata, ID3D11Texture2D* texture,
        ID3D11DeviceContext* context, bool cancelled) override;
    [[nodiscard]] pbcapturenormalize::CaptureStatus Completed(
        const pbcapturenormalize::ScreenCaptureFrameMetadata& metadata,
        ID3D11DeviceContext* context, bool cancelled) override;
    void Erased(const pbcapturenormalize::CaptureErasure& erasure) noexcept override;
    [[nodiscard]] bool TakeResult(CaptureDemodulatorResult& output) noexcept;
    [[nodiscard]] CaptureDemodulatorSnapshot GetSnapshot() const noexcept;

    CaptureDemodulator(const CaptureDemodulator&) = delete;
    CaptureDemodulator& operator=(const CaptureDemodulator&) = delete;

private:
    struct Implementation;
    [[nodiscard]] pbcapturenormalize::CaptureStatus CompleteInternal(
        const pbcapturenormalize::ScreenCaptureFrameMetadata& metadata, ID3D11Texture2D* texture,
        ID3D11DeviceContext* context, bool cancelled, bool allowContinuation, bool& gpuWorkSubmitted);
    explicit CaptureDemodulator(std::unique_ptr<Implementation> implementation) noexcept;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace pbdemodd3d11
