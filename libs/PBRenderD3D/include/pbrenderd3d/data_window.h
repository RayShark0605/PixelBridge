#pragma once

#include "pbpresenttiming/present_timing.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <iosfwd>
#include <optional>
#include <span>
#include <utility>

namespace pbrenderd3d
{

enum class PresentationErrorCode : std::uint8_t
{
    None,
    InvalidConfiguration,
    InvalidFrame,
    ResourceLimit,
    EpochMismatch,
    NotRunning,
    Paused,
    DpiAwarenessRequired,
    NativeFailure,
    WaitFailed,
    Timeout,
    DeviceLost,
    ContractViolation,
    OutOfMemory,
    InternalError
};

enum class PresentationStage : std::uint8_t
{
    None,
    Configuration,
    FrameValidation,
    Thread,
    WakeEvent,
    DpiAwareness,
    WindowClass,
    Window,
    Adapter,
    Device,
    SwapChain,
    FrameLatency,
    BackBuffer,
    Environment,
    GpuDrain,
    Resize,
    Wait,
    Upload,
    Readback,
    Present,
    Statistics,
    DebugLayer
};

struct PresentationStatus
{
    PresentationErrorCode code = PresentationErrorCode::None;
    PresentationStage stage = PresentationStage::None;
    std::int32_t nativeError = 0;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return code == PresentationErrorCode::None;
    }
    [[nodiscard]] static PresentationStatus Success() noexcept
    {
        return {};
    }
    [[nodiscard]] static PresentationStatus Failure(const PresentationErrorCode code, const PresentationStage stage,
                                                    const std::int32_t nativeError = 0) noexcept
    {
        return {code == PresentationErrorCode::None ? PresentationErrorCode::InternalError : code, stage, nativeError};
    }
    bool operator==(const PresentationStatus&) const = default;
};

template <typename ValueType> class PresentationResult
{
public:
    [[nodiscard]] static PresentationResult Success(ValueType value)
    {
        return PresentationResult(std::move(value));
    }
    [[nodiscard]] static PresentationResult Failure(PresentationStatus error)
    {
        if (error)
        {
            error = PresentationStatus::Failure(PresentationErrorCode::InternalError, PresentationStage::None);
        }
        return PresentationResult(error);
    }
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return value_.has_value();
    }
    [[nodiscard]] ValueType& Value() &
    {
        return value_.value();
    }
    [[nodiscard]] const ValueType& Value() const&
    {
        return value_.value();
    }
    [[nodiscard]] ValueType&& Value() &&
    {
        return std::move(value_).value();
    }
    [[nodiscard]] PresentationStatus Error() const noexcept
    {
        return error_;
    }

private:
    explicit PresentationResult(ValueType value) : value_(std::move(value))
    {
    }
    explicit PresentationResult(const PresentationStatus error) : error_(error)
    {
    }
    std::optional<ValueType> value_;
    PresentationStatus error_;
};

enum class FlipEffect : std::uint8_t
{
    Discard,
    Sequential
};

struct PhysicalPoint
{
    std::int32_t x = 0;
    std::int32_t y = 0;
    bool operator==(const PhysicalPoint&) const = default;
};

struct DataWindowConfig
{
    std::uint32_t width = 1920;
    std::uint32_t height = 1080;
    std::uint32_t bufferCount = 2;
    std::uint32_t maximumFrameLatency = 1;
    FlipEffect flipEffect = FlipEffect::Discard;
    std::uint64_t maximumFrameBytes = 64ull * 1024 * 1024;
    std::uint32_t waitTimeoutMilliseconds = 5000;
    // When enabled, frame-latency permits continue to Present the most recent
    // complete source raster until another complete submission replaces it.
    // The repeated Presents retain the same FrameSequence and never reuse a
    // source across a presentation epoch.
    bool repeatActiveFrame = false;
    // A sender-only fullscreen presenter may cover the selected monitor while
    // retaining WS_EX_NOACTIVATE so the control console keeps keyboard focus.
    bool topmost = false;
    // Physical desktop coordinates, including negative monitor origins.
    // Unspecified: center the client on the primary monitor.
    std::optional<PhysicalPoint> clientOrigin;
};

struct CanonicalBgraFrameView
{
    std::span<const std::byte> pixels;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t rowPitch = 0;
    std::uint64_t frameSequence = 0;
    std::uint64_t presentationEpoch = 0;
};

struct WindowEnvironment
{
    PhysicalPoint clientOrigin;
    std::uint32_t clientWidth = 0;
    std::uint32_t clientHeight = 0;
    std::uint32_t dpi = 0;
    std::uint32_t modeWidth = 0;
    std::uint32_t modeHeight = 0;
    std::uint32_t modeFrequency = 0;
    std::uint32_t modeOrientation = 0;
    std::uint32_t adapterLuidLow = 0;
    std::int32_t adapterLuidHigh = 0;
    std::uint64_t monitorIdentity = 0;
    std::array<wchar_t, 32> displayName{};
    std::array<wchar_t, 128> adapterDescription{};
    bool singleMonitor = false;
    bool adapterAvailable = false;
    bool minimized = false;
    bool occluded = false;
    bool closed = false;
    // Event serials preserve an actual WM_DISPLAYCHANGE even if a mode was
    // changed and restored between two environment polls.
    std::uint64_t modeChangeSerial = 0;
    std::uint64_t dpiChangeSerial = 0;
    bool operator==(const WindowEnvironment&) const = default;
};

struct PresentationContract
{
    std::uint32_t bufferWidth = 0;
    std::uint32_t bufferHeight = 0;
    std::uint32_t bufferCount = 0;
    std::uint32_t maximumFrameLatency = 0;
    FlipEffect flipEffect = FlipEffect::Discard;
    bool bgraUnorm = false;
    bool noMsaa = false;
    bool alphaIgnored = false;
    bool scalingNone = false;
    bool tearingDisabled = false;
    bool latencyWaitable = false;
    bool perMonitorV2 = false;
};

enum class WindowState : std::uint8_t
{
    Starting,
    Running,
    Paused,
    Stopped,
    Failed
};

struct DataWindowSnapshot
{
    WindowState state = WindowState::Starting;
    PresentationStatus error;
    WindowEnvironment environment;
    PresentationContract contract;
    pbpresenttiming::TimingSnapshot timing;
    std::uint64_t submittedFrames = 0;
    std::uint64_t replacedPendingFrames = 0;
    std::uint64_t discardedEpochFrames = 0;
    std::uint64_t sourceTextureReplacements = 0;
    std::uint64_t repeatedPresentCalls = 0;
    std::uint64_t invalidatedActiveFrames = 0;
    std::uint64_t totalPresentCalls = 0;
    std::uint64_t totalSuccessfulPresents = 0;
    std::uint64_t swapChainGeneration = 0;
    std::uint64_t bufferGeneration = 0;
    std::int32_t lastPresentIdNativeStatus = 0;
    bool pendingFrame = false;
    bool inFlightFrame = false;
    bool activeFrame = false;
    std::uint64_t activeFrameSequence = 0;
    std::uint64_t activeFramePresentationEpoch = 0;
    bool candidateContractSatisfied = false;
    // True only for the explicitly injected software GPU correctness backend;
    // the production Create path never silently selects WARP.
    bool softwareRasterizer = false;
};

[[nodiscard]] PresentationStatus ValidateDataWindowConfig(const DataWindowConfig& config) noexcept;
[[nodiscard]] PresentationStatus ValidateCanonicalBgraFrame(const DataWindowConfig& config, const CanonicalBgraFrameView& frame) noexcept;
[[nodiscard]] const char* GetPresentationErrorName(PresentationErrorCode code) noexcept;
[[nodiscard]] const char* GetPresentationStageName(PresentationStage stage) noexcept;
// Writes one bounded diagnostic object, never pixels or protocol payload.
// File ownership, retention and stream error handling belong to the caller.
void WriteDataWindowSnapshotJson(std::ostream& destination, const DataWindowSnapshot& snapshot);

class DataWindowTestAccess;

// No Qt or graphics object escapes this API. Create starts the dedicated
// HWND/context owner. SubmitFrame copies before returning; only one pending
// image is retained. Stop is idempotent and joins the owner; it must not race
// destruction of the DataWindow object itself with other member calls.
class DataWindow
{
public:
    [[nodiscard]] static PresentationResult<std::unique_ptr<DataWindow>> Create(const DataWindowConfig& config) noexcept;
    ~DataWindow();
    DataWindow(const DataWindow&) = delete;
    DataWindow& operator=(const DataWindow&) = delete;
    [[nodiscard]] PresentationStatus SubmitFrame(const CanonicalBgraFrameView& frame) noexcept;
    [[nodiscard]] DataWindowSnapshot GetSnapshot() const noexcept;
    void RequestStop() noexcept;
    void Stop() noexcept;

private:
    friend class DataWindowTestAccess;
    struct Implementation;
    explicit DataWindow(std::unique_ptr<Implementation> implementation) noexcept;
    std::unique_ptr<Implementation> implementation_;
};

}
