#include "monitor_catalog.h"

#include <ShellScalingApi.h>

#include <array>
#include <new>

namespace pbapp
{
namespace
{

inline constexpr std::size_t maximumMonitors = 64;

struct EnumerationContext
{
    std::array<MonitorInfo, maximumMonitors> monitors{};
    std::size_t count = 0;
    MonitorCatalogStatus status;
};

BOOL CALLBACK EnumerateMonitor(const HMONITOR monitor, HDC, LPRECT,
    const LPARAM parameter) noexcept
{
    auto& context = *reinterpret_cast<EnumerationContext*>(parameter);
    if (context.count == context.monitors.size())
    {
        context.status = {MonitorCatalogError::ResourceLimit, ERROR_TOO_MANY_NAMES};
        return FALSE;
    }
    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (GetMonitorInfoW(monitor, &monitorInfo) == FALSE)
    {
        context.status = {MonitorCatalogError::NativeFailure, static_cast<std::int32_t>(GetLastError())};
        return FALSE;
    }
    const std::int64_t width = static_cast<std::int64_t>(monitorInfo.rcMonitor.right) - monitorInfo.rcMonitor.left;
    const std::int64_t height = static_cast<std::int64_t>(monitorInfo.rcMonitor.bottom) - monitorInfo.rcMonitor.top;
    if (width <= 0 || height <= 0)
    {
        context.status = {MonitorCatalogError::MetadataUnavailable, ERROR_INVALID_DATA};
        return FALSE;
    }
    UINT dpiX = 0;
    UINT dpiY = 0;
    const HRESULT dpiResult = GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    if (FAILED(dpiResult) || dpiX == 0 || dpiY == 0)
    {
        context.status = {MonitorCatalogError::MetadataUnavailable, dpiResult};
        return FALSE;
    }
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    const BOOL modeResult = EnumDisplaySettingsExW(monitorInfo.szDevice, ENUM_CURRENT_SETTINGS, &mode, 0);
    if (modeResult == FALSE || mode.dmDisplayFrequency == 0 || mode.dmDisplayFrequency == 1)
    {
        context.status = {MonitorCatalogError::MetadataUnavailable,
            static_cast<std::int32_t>(modeResult == FALSE ? GetLastError() : ERROR_INVALID_DATA)};
        return FALSE;
    }
    MonitorInfo& candidate = context.monitors[context.count];
    candidate.monitor = monitor;
    try
    {
        candidate.deviceName = monitorInfo.szDevice;
    }
    catch (const std::bad_alloc&)
    {
        context.status = {MonitorCatalogError::OutOfMemory, ERROR_NOT_ENOUGH_MEMORY};
        return FALSE;
    }
    candidate.physicalRect = monitorInfo.rcMonitor;
    candidate.workRect = monitorInfo.rcWork;
    candidate.dpiX = dpiX;
    candidate.dpiY = dpiY;
    candidate.refreshRate = mode.dmDisplayFrequency;
    candidate.primary = (monitorInfo.dwFlags & MONITORINFOF_PRIMARY) != 0;
    const std::optional<POINT> phase1CanvasOrigin = GetPhase1CanvasOrigin(candidate.physicalRect,
        candidate.workRect);
    candidate.supportsPhase1Canvas = phase1CanvasOrigin.has_value();
    if (phase1CanvasOrigin)
    {
        candidate.phase1CanvasOrigin = *phase1CanvasOrigin;
    }
    candidate.phase1ReferenceGeometry = IsPhase1ReferenceMonitor(candidate.physicalRect,
        candidate.refreshRate);
    context.count++;
    return TRUE;
}

} // namespace

bool IsPhase1ReferenceMonitor(const RECT& physicalRect, const std::uint32_t refreshRate) noexcept
{
    const std::int64_t width = static_cast<std::int64_t>(physicalRect.right) - physicalRect.left;
    const std::int64_t height = static_cast<std::int64_t>(physicalRect.bottom) - physicalRect.top;
    return width == phase1CanvasWidth && height == phase1CanvasHeight && refreshRate == 60;
}

bool CanHostPhase1Canvas(const RECT& physicalRect) noexcept
{
    const std::int64_t width = static_cast<std::int64_t>(physicalRect.right) - physicalRect.left;
    const std::int64_t height = static_cast<std::int64_t>(physicalRect.bottom) - physicalRect.top;
    return width >= phase1CanvasWidth && height >= phase1CanvasHeight;
}

std::optional<POINT> GetPhase1CanvasOrigin(const RECT& physicalRect, const RECT& workRect) noexcept
{
    if (!CanHostPhase1Canvas(physicalRect))
    {
        return std::nullopt;
    }
    const bool workContained = workRect.left >= physicalRect.left && workRect.top >= physicalRect.top &&
        workRect.right <= physicalRect.right && workRect.bottom <= physicalRect.bottom;
    const RECT& hostRect = workContained && CanHostPhase1Canvas(workRect) ? workRect : physicalRect;
    const std::int64_t hostWidth = static_cast<std::int64_t>(hostRect.right) - hostRect.left;
    const std::int64_t hostHeight = static_cast<std::int64_t>(hostRect.bottom) - hostRect.top;
    const std::int64_t originX = static_cast<std::int64_t>(hostRect.left) +
        (hostWidth - static_cast<std::int64_t>(phase1CanvasWidth)) / 2;
    const std::int64_t originY = static_cast<std::int64_t>(hostRect.top) +
        (hostHeight - static_cast<std::int64_t>(phase1CanvasHeight)) / 2;
    return POINT{static_cast<LONG>(originX), static_cast<LONG>(originY)};
}

MonitorCatalogStatus EnumerateMonitors(std::vector<MonitorInfo>& output) noexcept
{
    if (!AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(),
        DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
    {
        return {MonitorCatalogError::DpiAwarenessRequired, ERROR_INVALID_STATE};
    }
    EnumerationContext context;
    SetLastError(ERROR_SUCCESS);
    if (EnumDisplayMonitors(nullptr, nullptr, &EnumerateMonitor,
        reinterpret_cast<LPARAM>(&context)) == FALSE)
    {
        if (!context.status)
        {
            return context.status;
        }
        const DWORD error = GetLastError();
        return {MonitorCatalogError::NativeFailure,
            static_cast<std::int32_t>(error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error)};
    }
    if (context.count == 0)
    {
        return {MonitorCatalogError::MetadataUnavailable, ERROR_NOT_FOUND};
    }
    try
    {
        std::vector<MonitorInfo> candidate(context.monitors.begin(),
            context.monitors.begin() + static_cast<std::ptrdiff_t>(context.count));
        output = std::move(candidate);
    }
    catch (const std::bad_alloc&)
    {
        return {MonitorCatalogError::OutOfMemory, ERROR_NOT_ENOUGH_MEMORY};
    }
    return {};
}

} // namespace pbapp
