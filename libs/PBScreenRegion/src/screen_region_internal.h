#pragma once

#include "pbscreenregion/screen_region.h"

#include <array>
#include <cstddef>
#include <memory>
#include <string_view>

namespace pbscreenregion::detail
{

inline constexpr std::size_t maximumMonitors = 64;

struct MonitorMetadata
{
    HMONITOR monitor = nullptr;
    RECT physicalRect{};
    UINT dpiX = 0;
    UINT dpiY = 0;
    DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_UNSPECIFIED;
    std::array<wchar_t, 32> deviceName{};
};

struct MonitorSnapshot
{
    std::array<MonitorMetadata, maximumMonitors> monitors{};
    std::size_t count = 0;
};

enum class SelectionEventType : std::uint8_t
{
    PointerDown,
    PointerMove,
    PointerUp,
    Cancel,
    DisplayChanged,
    CheckEnvironment
};

struct SelectionEvent
{
    SelectionEventType type = SelectionEventType::Cancel;
    POINT physicalPoint{};
};

struct SelectionPreview
{
    RECT physicalRect{};
    bool hasRectangle = false;
    bool dragging = false;
    ScreenRegionStatus validation;
};

// Only OS effects are replaceable. Fake/native backends run the same geometry,
// admission, revalidation and selection loop below; no second test selector.
class ScreenRegionBackend
{
public:
    virtual ~ScreenRegionBackend() = default;
    [[nodiscard]] virtual ScreenRegionStatus CheckAwareness() noexcept = 0;
    [[nodiscard]] virtual ScreenRegionStatus ReadTopology(MonitorSnapshot& snapshot) noexcept = 0;
    [[nodiscard]] virtual ScreenRegionStatus OpenOverlay(const MonitorSnapshot& snapshot) noexcept = 0;
    [[nodiscard]] virtual ScreenRegionStatus GetEvent(SelectionEvent& event) noexcept = 0;
    [[nodiscard]] virtual ScreenRegionStatus AcquirePointer(POINT physicalPoint) noexcept = 0;
    [[nodiscard]] virtual ScreenRegionStatus ReleasePointer() noexcept = 0;
    [[nodiscard]] virtual ScreenRegionStatus Draw(const SelectionPreview& preview) noexcept = 0;
    [[nodiscard]] virtual ScreenRegionStatus Close() noexcept = 0;
};

[[nodiscard]] bool EqualRect(const RECT& first, const RECT& second) noexcept;
[[nodiscard]] bool DeclaresPerMonitorV2(std::wstring_view setting) noexcept;
[[nodiscard]] bool EqualTopology(const MonitorSnapshot& first, const MonitorSnapshot& second) noexcept;
[[nodiscard]] ScreenRegionStatus ValidatePhysicalRect(const RECT& rect) noexcept;
[[nodiscard]] ScreenRegionStatus ValidateTopology(const MonitorSnapshot& snapshot) noexcept;
[[nodiscard]] ScreenRegionStatus BuildDragRect(POINT first, POINT last, RECT& rect) noexcept;
[[nodiscard]] ScreenRegionStatus ResolveFromTopology(const MonitorSnapshot& snapshot, const RECT& rect, ScreenCaptureRegion& region) noexcept;
[[nodiscard]] ScreenRegionStatus RunSelection(ScreenRegionBackend& backend, ScreenCaptureRegion& region) noexcept;
[[nodiscard]] ScreenRegionStatus RunResolve(ScreenRegionBackend& backend, const RECT& rect, ScreenCaptureRegion& region) noexcept;

struct NativeDiagnostics
{
    std::size_t liveWindows = 0;
    std::size_t liveFonts = 0;
    std::size_t livePens = 0;
    std::size_t liveClasses = 0;
    bool ownsCapture = false;
    std::size_t createdWindows = 0;
};

class NativeObserver
{
public:
    virtual ~NativeObserver() = default;
    virtual void OnReady(const MonitorSnapshot& snapshot, const std::array<HWND, maximumMonitors>& windows) noexcept = 0;
    virtual void OnPreview(const SelectionPreview& preview) noexcept = 0;
};

// Private, non-installed fault/observation seam. Does not change public CLI or
// input processing. Native gates can observe acknowledgments before moving the
// real cursor again, without treating synthetic window messages as mouse input.
struct NativeBackendOptions
{
    ScreenRegionStage failAfterStage = ScreenRegionStage::None;
    std::size_t failOccurrence = 1;
    NativeDiagnostics* diagnostics = nullptr;
    NativeObserver* observer = nullptr;
};

[[nodiscard]] std::unique_ptr<ScreenRegionBackend> MakeNativeBackend(const NativeBackendOptions& options = {});

} // namespace pbscreenregion::detail
