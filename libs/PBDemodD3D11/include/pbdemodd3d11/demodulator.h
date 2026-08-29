#pragma once

#include "pbcapturenormalize/screen_capture_frame.h"
#include "pbdesktoplevels/reference_channel.h"

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
};

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
    std::uint64_t metricReadbackBytes = 0;
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
    // Nonblocking: ready=false means the event query is not complete and no Map
    // occurred. On ready=true, output changes only for a successful status.
    [[nodiscard]] DemodPollResult Poll(ID3D11DeviceContext* context, const DemodSubmission& submission,
        DemodFrameResult& output) noexcept;
    // Marks matching pending work cancelled. GPU/source retirement still occurs
    // only through Poll after its event query completes.
    [[nodiscard]] DemodStatus InvalidateDomain(const pbcapturenormalize::ScreenCaptureDomain& domain) noexcept;
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
