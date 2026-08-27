#pragma once

#include "pbscreenregion/screen_region.h"

#include <d3d11.h>
#include <cstdint>
#include <memory>
#include <optional>

namespace pbscreencapturewgc
{

enum class CaptureError : std::uint8_t
{
    None, InvalidConfiguration, ResourceLimit, Unsupported, InvalidFrame, RegionChanged,
    DeviceLost, AccessLost, NativeFailure, ConsumerFailure, Timeout, WrongThread, OutOfMemory, InternalError, DpiAwarenessRequired
};

enum class CaptureStage : std::uint8_t
{
    None, Configuration, Apartment, Region, Adapter, Device, CaptureItem, FramePool, Session,
    Callback, Surface, TextureRing, Copy, Completion, Consumer, Recreate, Shutdown
};

struct CaptureStatus
{
    CaptureError code = CaptureError::None;
    CaptureStage stage = CaptureStage::None;
    std::int32_t nativeError = 0;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return code == CaptureError::None;
    }
    [[nodiscard]] static CaptureStatus Failure(CaptureError code, CaptureStage stage, std::int32_t nativeError = 0) noexcept
    {
        return {code == CaptureError::None ? CaptureError::InternalError : code, stage, nativeError};
    }
    bool operator==(const CaptureStatus&) const = default;
};

struct CaptureSize
{
    std::int32_t width = 0;
    std::int32_t height = 0;
    bool operator==(const CaptureSize&) const = default;
};

struct WgcCaptureConfig
{
    pbscreenregion::ScreenCaptureRegion region;
    std::uint64_t initialCaptureEpoch = 1;
    std::uint32_t queuedFrameLimit = 2;
    std::uint32_t roiTextureCount = 3;
    // Pool buffers = queuedFrameLimit + roiTextureCount + one acquisition slot.
    // These quotas include ALL pool surfaces and owned ROI textures, not just one image.
    std::uint64_t maximumCaptureBytes = 512ull * 1024 * 1024;
    std::uint64_t maximumRoiBytes = 64ull * 1024 * 1024;
    std::uint32_t gpuTimeoutMilliseconds = 3000;
    std::uint32_t maximumDeviceRecoveries = 1;
    DXGI_FORMAT pixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    // Optional hints, never requirements for capture correctness or measured FPS.
    bool requestBorderless = false;
    std::optional<std::int64_t> minUpdateInterval100ns;
};

struct CaptureCapabilities
{
    bool cursorDisableAvailable = false;
    bool cursorExcluded = false;
    std::int32_t cursorNativeError = 0;
    bool borderlessAvailable = false;
    bool borderlessAccessRequested = false;
    bool borderlessAccessGranted = false;
    // A successful setter does NOT prove that no other session requires a border.
    bool borderlessSettingApplied = false;
    std::int32_t borderlessNativeError = 0;
    bool minUpdateIntervalAvailable = false;
    bool minUpdateIntervalApplied = false;
    std::int64_t actualMinUpdateInterval100ns = 0;
    std::int32_t minUpdateIntervalNativeError = 0;
    bool fenceRetirement = false;
};

struct CaptureEnvironment
{
    pbscreenregion::ScreenCaptureRegion region;
    CaptureSize contentSize;
    DXGI_FORMAT pixelFormat = DXGI_FORMAT_UNKNOWN;
    LUID adapterLuid{};
    std::uint32_t displayFrequency = 0;
    std::uint32_t bitsPerColor = 0;
    std::uint32_t outputColorSpace = 0;
    bool hdr = false;
};

struct RoiFrameMetadata
{
    std::uint64_t captureEpoch = 0;
    std::uint64_t arrivalOrdinal = 0;
    std::int64_t systemRelativeTime100ns = 0;
    CaptureEnvironment environment;
    CaptureCapabilities capabilities;
};

// Invoked on the dedicated D3D submission owner, NEVER on FrameArrived.
// Submit may enqueue D3D11 work on this context only. All pointers are borrowed:
// do not retain/map the ROI later, use another context/thread, or retain bindings
// for future GPU submissions. Return promptly; the ring is retired by a marker
// AFTER Submit, including when Submit reports failure. No hidden CPU readback.
// EpochStarted must invalidate geometry/calibration and device-specific state;
// the next Submit can only contain pixels from that epoch. No GPU submission here.
class RoiConsumer
{
public:
    virtual ~RoiConsumer() = default;
    [[nodiscard]] virtual CaptureStatus EpochStarted(std::uint64_t captureEpoch, const CaptureEnvironment& environment, ID3D11Device* device) = 0;
    [[nodiscard]] virtual CaptureStatus Submit(const RoiFrameMetadata& metadata, ID3D11Texture2D* texture, ID3D11DeviceContext* context) = 0;
};

enum class CaptureState : std::uint8_t
{
    Starting, Running, Draining, Recreating, Stopped, Failed
};

struct WgcCaptureSnapshot
{
    CaptureState state = CaptureState::Starting;
    CaptureStatus error;
    CaptureStatus lastDeviceLoss;
    CaptureEnvironment environment;
    CaptureCapabilities capabilities;
    std::uint64_t captureEpoch = 0;
    std::uint64_t arrivedFrames = 0;
    std::uint64_t droppedFrames = 0;
    std::uint64_t copiedFrames = 0;
    std::uint64_t deliveredFrames = 0;
    std::uint64_t recreates = 0;
    std::uint32_t deviceRecoveries = 0;
    std::uint32_t queuedFrames = 0;
    std::uint32_t liveFrameLeases = 0;
    std::uint32_t frameLeaseHighWater = 0;
    std::uint32_t busyRoiTextures = 0;
    bool shutdownComplete = false;
    // On a GPU timeout, Stop returns without prematurely returning pool leases.
    // A preallocated OS completion/device-removed wait owns the bounded resources
    // until safe retirement. No retry loop, detached polling thread or new capture.
    bool deferredCleanup = false;
};

[[nodiscard]] CaptureStatus ValidateWgcCaptureConfig(const WgcCaptureConfig& config) noexcept;
[[nodiscard]] const char* GetCaptureErrorName(CaptureError error) noexcept;

class WgcCaptureTestAccess;

// The host declares PMv2 before creating windows. The module owns its MTA and
// capture-adapter D3D11 device. Consumer ownership lasts through GPU retirement.
// Create leaves output unchanged on failure. RequestStop/GetSnapshot are safe
// from the consumer; Stop is idempotent but must not run on the owner thread.
// Destruction must not race other calls or run inside a consumer callback.
// A permanently wedged OS/GPU can retain
// at most this instance's bounded deferred resources; it is never called success.
class WgcCapture
{
public:
    [[nodiscard]] static CaptureStatus Create(const WgcCaptureConfig& config, std::shared_ptr<RoiConsumer> consumer,
                                              std::unique_ptr<WgcCapture>& output) noexcept;
    ~WgcCapture();
    WgcCapture(const WgcCapture&) = delete;
    WgcCapture& operator=(const WgcCapture&) = delete;
    [[nodiscard]] WgcCaptureSnapshot GetSnapshot() const noexcept;
    void RequestStop() noexcept;
    [[nodiscard]] CaptureStatus Stop() noexcept;

private:
    friend class WgcCaptureTestAccess;
    struct Implementation;
    explicit WgcCapture(std::shared_ptr<Implementation> implementation) noexcept;
    std::shared_ptr<Implementation> implementation_;
};

} // namespace pbscreencapturewgc
