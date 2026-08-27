#pragma once

#include <Windows.h>
#include <dxgi.h>

#include <cstdint>
#include <iosfwd>

namespace pbscreenregion
{

enum class ScreenRegionErrorCode : std::uint8_t
{
    None,
    Cancelled,
    DpiAwarenessRequired,
    InvalidRectangle,
    NotSingleMonitor,
    AmbiguousMonitor,
    MetadataUnavailable,
    DisplayChanged,
    ResourceLimit,
    NativeFailure,
    OutOfMemory,
    InternalError
};

enum class ScreenRegionStage : std::uint8_t
{
    None,
    DpiAwareness,
    Geometry,
    MonitorEnumeration,
    DpiQuery,
    OutputQuery,
    WindowClass,
    Window,
    OverlayStyle,
    Font,
    Capture,
    Cursor,
    Paint,
    MessageWait,
    Input,
    Revalidation,
    Cleanup,
    InputContext
};

struct ScreenRegionStatus
{
    ScreenRegionErrorCode code = ScreenRegionErrorCode::None;
    ScreenRegionStage stage = ScreenRegionStage::None;
    std::int32_t nativeError = 0;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return code == ScreenRegionErrorCode::None;
    }
    [[nodiscard]] static ScreenRegionStatus Success() noexcept
    {
        return {};
    }
    [[nodiscard]] static ScreenRegionStatus Failure(ScreenRegionErrorCode code, ScreenRegionStage stage, std::int32_t nativeError = 0) noexcept;
    bool operator==(const ScreenRegionStatus&) const = default;
};

struct ScreenCaptureRegion
{
    // Borrowed, transient OS identity. Re-resolve after a display/DPI change;
    // neither this handle nor the structure is a persistent/wire format.
    HMONITOR monitor = nullptr;
    // Signed PHYSICAL desktop pixels, [left, right) x [top, bottom).
    // DPI and rotation are metadata, never additional transforms of these RECTs.
    RECT physicalRect{};
    RECT monitorPhysicalRect{};
    UINT dpiX = 0;
    UINT dpiY = 0;
    DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_UNSPECIFIED;
};

// The host must declare PMv2 in its manifest before creating HWNDs; these functions never change
// DPI awareness. Call synchronously on the selector's UI owner thread, with no
// concurrent selector on that thread. They do not initialize COM or create a
// capture device. All failure/cancellation paths leave region unchanged.
[[nodiscard]] ScreenRegionStatus SelectScreenCaptureRegion(ScreenCaptureRegion& region) noexcept;
[[nodiscard]] ScreenRegionStatus ResolveScreenCaptureRegion(const RECT& physicalRect, ScreenCaptureRegion& region) noexcept;

[[nodiscard]] const char* GetScreenRegionErrorName(ScreenRegionErrorCode code) noexcept;
// Diagnostic JSON only, not a payload transport or a capture certification.
// May throw on stream/allocation failure; respects the caller's stream state.
void WriteScreenCaptureRegionJson(std::ostream& stream, const ScreenCaptureRegion& region);

} // namespace pbscreenregion
