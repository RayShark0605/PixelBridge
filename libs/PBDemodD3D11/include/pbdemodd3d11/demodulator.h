#pragma once

#include "pbcapturenormalize/screen_capture_frame.h"
#include "pbdesktoplevels/reference_channel.h"
#include "pbmodulation/remote_visual_low_fps.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace pbdemodd3d11
{

enum class DemodError : std::uint8_t
{
    None, InvalidConfiguration, WrongThread, WrongDevice, AdapterMismatch, InvalidFrame, InvalidBinding,
    UnsupportedProfile, ResourceLimit, Busy, ShaderCompileFailure, NativeFailure, DeviceLost,
    InvalidSubmission, MapFailure, NonFiniteMetric, CalibrationFailure, Cancelled, ShutdownRequired
};

enum class DemodStage : std::uint8_t
{
    None, Configuration, Device, Shader, Resource, Binding, Submission, Dispatch, Completion, Readback, Calibration, Fec, Shutdown
};

struct DemodStatus
{
    DemodError code = DemodError::None;
    DemodStage stage = DemodStage::None;
    std::int32_t nativeError = 0;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return code == DemodError::None;
    }
    [[nodiscard]] static DemodStatus Failure(DemodError code, DemodStage stage, std::int32_t nativeError = 0) noexcept
    {
        return {code == DemodError::None ? DemodError::NativeFailure : code, stage, nativeError};
    }
    bool operator==(const DemodStatus&) const = default;
};

struct DemodConfig
{
    std::uint32_t readbackSlotCount = 3;
    std::uint64_t maximumResidentBytes = 64ULL * 1024 * 1024;
    pbdesktoplevels::EvaluationMode evaluationMode = pbdesktoplevels::EvaluationMode::DiagnosticTruth;
};

// Exact fixed reservation used by Create, including all per-slot GPU buffers,
// CPU metric/FEC state, and bounded evaluator processing storage. Failure does
// not change output.
[[nodiscard]] DemodStatus CalculateDemodulatorResidentBytes(const DemodConfig& config, std::uint64_t& output) noexcept;

struct DemodSubmission
{
    std::uint32_t slotIndex = UINT32_MAX;
    std::uint64_t slotGeneration = 0;
    pbcapturenormalize::ScreenCaptureDomain domain;
    std::uint64_t captureObservation = 0;
    bool operator==(const DemodSubmission&) const = default;
};

struct DemodFrameResult
{
    pbcapturenormalize::ScreenCaptureFrameMetadata metadata;
    std::uint64_t visualProfileId = 0;
    pbdesktoplevels::FrameEvaluation evaluation;
    std::array<pbdesktoplevels::AcceptedTransportBlock, pbdesktoplevels::kMaximumCodewords> acceptedTransportBlocks{};
    std::uint32_t acceptedTransportBlockCount = 0;
    std::array<pbdesktoplevels::AcceptedRemoteControlBlock, pbmodulation::kRemoteVisualLowFpsCodewords>
        acceptedRemoteControlBlocks{};
    std::uint32_t acceptedRemoteControlBlockCount = 0;
    std::uint64_t metricReadbackBytes = 0;
    // Timestamp scope: constants/upload, calibration dispatch, data dispatch,
    // and metric/calibration copies into staging. This is GPU execution time,
    // not CPU submission time or the upstream ROI-copy duration.
    std::uint64_t gpuTime100ns = 0;
    bool gpuTimingValid = false;
    // RemoteVisual metric/freshness diagnostics. The resolver can replace all
    // data metrics from a stale region with zero before the unchanged FEC
    // gate; these counters only report that fail-closed erasure decision.
    bool remoteMetricSummaryAvailable = false;
    std::uint32_t remoteMetricSamples = 0;
    std::uint32_t remoteZeroMagnitudeMetrics = 0;
    double remoteMinimumAbsoluteMetric = 0;
    double remoteMeanAbsoluteMetric = 0;
    std::uint32_t remoteFreshnessRegions = 0;
    std::uint32_t remoteStaleRegions = 0;
    std::uint32_t remoteFreshnessTagMismatches = 0;
    std::uint32_t remoteFreshnessTagErasures = 0;
    std::uint32_t remoteFreshnessErasedDataMetrics = 0;
    std::uint32_t remoteUnreliableSymbols = 0;
};

struct DemodPollResult
{
    DemodStatus status;
    bool ready = false;
};

struct DemodSnapshot
{
    LUID adapterLuid{};
    std::uint64_t submittedFrames = 0;
    std::uint64_t completedFrames = 0;
    std::uint64_t cancelledFrames = 0;
    std::uint64_t failedFrames = 0;
    std::uint64_t metricReadbackBytes = 0;
    std::uint64_t rawPixelReadbackBytes = 0;
    std::uint64_t gpuTimingSamples = 0;
    std::uint64_t gpuTimingUnavailable = 0;
    std::uint64_t gpuTimeTotal100ns = 0;
    std::uint64_t gpuTimeHighWater100ns = 0;
    std::uint32_t pendingFrames = 0;
    std::uint32_t highWater = 0;
    std::uint64_t residentBytes = 0;
    bool shutdown = false;
};

// One D3D11 immediate-context owner. Create, Submit, Poll, InvalidateDomain and
// Shutdown must all run on that same thread and context. Submit retains the
// borrowed PB-owned ROI texture until Poll reports ready (or a cancelled job
// retires); the caller must not recycle the texture earlier.
//
// bootstrapRecord is not an out-of-band identity shortcut: the caller must
// supply the canonical record previously recovered from the same admitted
// pixel observation/domain. This baseline starts after locate/bootstrap and
// performs only calibrated data demodulation plus the shared CPU FEC gate.
class Demodulator
{
public:
    // Public only so translation-unit helpers can name the opaque state type;
    // callers cannot construct or inspect it because its definition is private
    // to the implementation file.
    struct Implementation;

    Demodulator() noexcept;
    Demodulator(Demodulator&&) noexcept;
    Demodulator& operator=(Demodulator&&) noexcept;
    ~Demodulator();
    Demodulator(const Demodulator&) = delete;
    Demodulator& operator=(const Demodulator&) = delete;

    [[nodiscard]] static DemodStatus Create(ID3D11Device* device, const DemodConfig& config,
        std::unique_ptr<Demodulator>& output) noexcept;
    [[nodiscard]] DemodStatus Submit(const pbcapturenormalize::ScreenCaptureFrame& frame, ID3D11DeviceContext* context,
        std::span<const std::byte> bootstrapRecord, DemodSubmission& output) noexcept;
    // Experimental Step-10 LF4 path. geometry and bootstrapRecord must both
    // have been recovered from this exact admitted pixel observation/domain.
    // The GPU samples the PB-owned source texture directly and reads back only
    // compact logical metrics, calibration data, and freshness counters.
    [[nodiscard]] DemodStatus SubmitRemoteVisualLowFps(const pbcapturenormalize::ScreenCaptureFrame& frame,
        ID3D11DeviceContext* context, std::span<const std::byte> bootstrapRecord,
        const pbmodulation::LocalDesktopGeometry& geometry,
        const pbmodulation::RemoteVisualLowFpsDecodePolicy& policy, DemodSubmission& output) noexcept;
    // Fixed-profile first stage for a same-frame Bootstrap readback pipeline.
    // The profile selects only GPU geometry; no sender identity or sequence is
    // trusted here. PollUnbound must later receive the canonical Bootstrap
    // recovered from this exact frame before any Transport block is evaluated.
    [[nodiscard]] DemodStatus SubmitUnbound(const pbcapturenormalize::ScreenCaptureFrame& frame, ID3D11DeviceContext* context,
        std::uint64_t expectedVisualProfileId, DemodSubmission& output) noexcept;
    // Nonblocking: ready=false means the event query is not complete and no Map
    // occurred. On ready=true, output changes only for a successful status.
    [[nodiscard]] DemodPollResult Poll(ID3D11DeviceContext* context, const DemodSubmission& submission,
        DemodFrameResult& output) noexcept;
    [[nodiscard]] DemodPollResult PollUnbound(ID3D11DeviceContext* context, const DemodSubmission& submission,
        std::span<const std::byte> bootstrapRecord, DemodFrameResult& output) noexcept;
    // Capture-runtime cancellation path only. The caller must already have
    // proof from a later GPU marker, or confirmed device removal, that every
    // command submitted for this frame is retired. No context access occurs,
    // so deferred cleanup may call this from its cleanup thread.
    [[nodiscard]] DemodStatus RetireAfterExternalCompletion(const DemodSubmission& submission) noexcept;
    // Marks matching pending work cancelled. GPU/source retirement still occurs
    // only through Poll after its event query completes.
    [[nodiscard]] DemodStatus InvalidateDomain(const pbcapturenormalize::ScreenCaptureDomain& domain) noexcept;
    // Completes deferred-cleanup bookkeeping after every pending submission was
    // retired through an externally proven marker/device-removal path. It does
    // not inspect or submit to a D3D context.
    [[nodiscard]] DemodStatus ShutdownAfterExternalCompletion() noexcept;
    // Succeeds only with no pending work. It never Flushes and pretends that
    // submission equals completion.
    [[nodiscard]] DemodStatus Shutdown(ID3D11DeviceContext* context) noexcept;
    [[nodiscard]] DemodSnapshot GetSnapshot() const noexcept;

private:
    explicit Demodulator(std::unique_ptr<Implementation> implementation) noexcept;
    std::unique_ptr<Implementation> implementation_;
};

[[nodiscard]] const char* GetDemodErrorName(DemodError error) noexcept;

} // namespace pbdemodd3d11
