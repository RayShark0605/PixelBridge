#pragma once

#include "pbscreenregion/screen_region.h"

#include <d3d11.h>
#include <cstdint>
#include <memory>
#include <optional>

namespace pbcapturenormalize
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

// Result of an owner-thread consumer completion callback. A true
// gpuWorkSubmitted is a precise lifetime claim: the callback enqueued more work
// on the supplied immediate context, so the PB-owned ROI slot must remain
// leased until a new backend marker retires that work.
struct CaptureConsumerCompletion
{
    CaptureStatus status;
    bool gpuWorkSubmitted = false;
};

struct CaptureSize
{
    std::int32_t width = 0;
    std::int32_t height = 0;
    bool operator==(const CaptureSize&) const = default;
};

struct CaptureConfig
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
    // Zero preserves the legacy raw API; normalized consumers require a finite limit.
    std::uint32_t maximumFrameAgeMilliseconds = 0;
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

enum class CaptureBackendKind : std::uint8_t
{
    Wgc, Dxgi
};
enum class CursorState : std::uint8_t
{
    // KnownAbsent is per-frame proof the pointer is not in this frame's
    // pixels (a well-formed source report stating the pointer is absent).
    // Unlike Excluded it carries no backend capability claim.
    Unknown, Excluded, SeparatePointer, PossiblyComposited, KnownAbsent
};
enum class CaptureTimestampDomain : std::uint8_t
{
    WgcSystemRelative100ns, DxgiQpcTicks
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
    CaptureBackendKind backendKind = CaptureBackendKind::Wgc;
    CaptureSize sourceSize;
    DXGI_MODE_ROTATION sourceRotation = DXGI_MODE_ROTATION_IDENTITY;
};

struct CapturePointerMetadata
{
    bool positionKnown = false;
    bool shapeKnown = false;
    bool separateVisible = false;
    std::int64_t physicalLeft = 0;
    std::int64_t physicalTop = 0;
    std::int64_t rawUpdateTimestamp = 0;
    std::uint32_t shapeType = 0;
    std::uint32_t shapeWidth = 0;
    std::uint32_t shapeRawHeight = 0;
    std::uint32_t shapeVisibleHeight = 0;
    std::uint32_t shapePitch = 0;
    std::uint32_t shapeBytes = 0;
    // The hotspot is metadata, never an offset applied to the top-left position.
    std::int32_t hotspotX = 0;
    std::int32_t hotspotY = 0;
    bool operator==(const CapturePointerMetadata&) const = default;
};

struct RawRoiFrameMetadata
{
    std::uint64_t captureEpoch = 0;
    std::uint64_t arrivalOrdinal = 0;
    std::int64_t systemRelativeTime100ns = 0;
    CaptureEnvironment environment;
    CaptureCapabilities capabilities;
    CursorState cursorState = CursorState::Unknown;
    std::uint64_t sourceGeneration = 0;
    std::uint64_t slotGeneration = 0;
    std::uint32_t slotIndex = 0;
    CaptureTimestampDomain timestampDomain = CaptureTimestampDomain::WgcSystemRelative100ns;
    std::int64_t rawTimestamp = 0;
    std::int64_t rawFrequency = 10000000;
    // QPC-derived 100ns sampled when the frame entered the inbox (the arrival).
    // Capture always precedes arrival, so this stays a valid age-bound even when
    // the backend claim is untrusted (WGC SystemRelativeTime can stamp ahead of
    // delivery). -1 = not measured.
    std::int64_t arrivalQpc100ns = -1;
    // GPU timestamp duration covering ROI crop/copy plus required rotation.
    // Absent means the timestamp query was unsupported/disjoint, never zero by
    // inference from CPU submission or completion time.
    std::optional<std::uint64_t> roiCopyTime100ns;
    CapturePointerMetadata pointer;
};

// Invoked on the dedicated D3D submission owner, NEVER on FrameArrived.
// Submit may enqueue D3D11 work on this context only. All pointers are borrowed:
// do not retain/map the ROI later, use another context/thread, or retain bindings
// for future GPU submissions. Return promptly; the ring is retired by a marker
// AFTER Submit, including when Submit reports failure. No hidden CPU readback.
// EpochStarted must invalidate geometry/calibration and device-specific state;
// the next Submit can only contain pixels from that epoch. No GPU submission here.
class RawRoiConsumer
{
public:
    virtual ~RawRoiConsumer() = default;
    [[nodiscard]] virtual CaptureStatus EpochStarted(std::uint64_t captureEpoch, const CaptureEnvironment& environment, ID3D11Device* device) = 0;
    [[nodiscard]] virtual CaptureStatus Submit(const RawRoiFrameMetadata& metadata, ID3D11Texture2D* texture, ID3D11DeviceContext* context) = 0;
    // Optional hooks preserve the legacy raw API. Strict consumers invalidate
    // CPU admission before GPU drain, then retire each submitted consumer job.
    virtual void EpochInvalidated(std::uint64_t) noexcept {}
    // The runtime permits at most one non-cancelled continuation. The first
    // callback may enqueue one additional bounded GPU stage and return true;
    // the following callback must return false. A callback that throws after
    // it might have submitted work is conservatively treated as a continuation.
    // cancelled always withholds GPU objects and must never request or submit
    // a continuation.
    [[nodiscard]] virtual CaptureConsumerCompletion CompleteStage(const RawRoiFrameMetadata& metadata, ID3D11Texture2D*, ID3D11DeviceContext* context, bool cancelled)
    {
        return {Completed(metadata, context, cancelled), false};
    }
    // Legacy completion hook. No GPU work may be submitted here.
    [[nodiscard]] virtual CaptureStatus Completed(const RawRoiFrameMetadata&, ID3D11DeviceContext*, bool) { return {}; }
};

enum class CaptureState : std::uint8_t
{
    Starting, Running, Draining, Recreating, Stopped, Failed, WaitingForEnvironment
};

enum class CaptureRebuildReason : std::uint8_t
{
    None, Requested, AccessLost, DisplayChanged, DesktopChanged, SourceDescriptionChanged, DeviceLost, UnavailableEnvironment
};

struct CaptureSnapshot
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
    std::uint64_t acquireTimeouts = 0;
    std::uint64_t pointerOnlyFrames = 0;
    std::uint64_t accumulatedFrames = 0;
    std::uint64_t accessLostEvents = 0;
    std::uint64_t expiredFrames = 0;
    std::uint64_t cursorErasures = 0;
    CaptureRebuildReason lastRebuildReason = CaptureRebuildReason::None;
    std::int32_t waitingNativeError = 0;
    std::uint32_t environmentAttempts = 0;
    std::uint64_t frameAgeHighWater100ns = 0;
    std::uint64_t staleFrames = 0;
    std::uint64_t roiCopyTimingSamples = 0;
    std::uint64_t roiCopyTimingUnavailable = 0;
    std::uint64_t roiCopyTimeTotal100ns = 0;
    std::uint64_t roiCopyTimeHighWater100ns = 0;
    std::uint64_t consumerContinuationSubmissions = 0;
    std::uint64_t consumerContinuationCompletions = 0;
    std::uint64_t consumerContinuationRejections = 0;
    // Separate lifecycle evidence from the triggering capture error. Auto may
    // replace a backend only after synchronous retirement, never after a timeout.
    CaptureStatus shutdownStatus;
    bool deviceRebuildFailed = false;
};

enum class CaptureFrameAgeDisposition : std::uint8_t
{
    Current, Expired, InvalidTimestamp
};

struct CaptureFrameAgeResult
{
    CaptureFrameAgeDisposition disposition = CaptureFrameAgeDisposition::InvalidTimestamp;
    std::uint64_t age100ns = 0;
    bool operator==(const CaptureFrameAgeResult&) const = default;
};

// A zero limit disables this age gate, preserving the raw capture API. It does
// not replace general source metadata validation. Finite limits include equality.
[[nodiscard]] CaptureFrameAgeResult ClassifyFrameAge(std::int64_t now100ns, std::int64_t timestamp100ns, std::uint32_t maximumFrameAgeMilliseconds) noexcept;

[[nodiscard]] CaptureStatus ValidateCaptureConfig(const CaptureConfig& config, CaptureBackendKind backendKind = CaptureBackendKind::Wgc) noexcept;
[[nodiscard]] const char* GetCaptureErrorName(CaptureError error) noexcept;
// Invalid/overflowing input leaves output unchanged; no floating point rounding.
[[nodiscard]] bool ConvertQpcTo100ns(std::int64_t ticks, std::int64_t frequency, std::int64_t& output) noexcept;

// Effective capture time for the frame-age gate: the smallest non-negative
// candidate among the backend claim and the measured QPC arrival. A claim that
// stamps ahead of its own arrival (observed with WGC SystemRelativeTime) is
// physically impossible for the capture instant and is superseded by the
// arrival. -1 = no valid time source; the caller must fail closed.
[[nodiscard]] std::int64_t ResolveEffectiveCaptureTime100ns(std::int64_t claimed100ns, std::int64_t arrivalQpc100ns) noexcept;


} // namespace pbcapturenormalize
