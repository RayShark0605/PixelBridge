#include "encoder_monitor_catalog.h"

#include <ShellScalingApi.h>

#include <algorithm>
#include <array>
#include <new>

namespace pbapp
{
namespace
{

struct EncoderMonitorWalk
{
    std::array<MonitorInfo, 64> monitors{};
    std::size_t count = 0;
    MonitorCatalogStatus status;
};

BOOL CALLBACK ReadEncoderMonitor(const HMONITOR monitor, HDC, LPRECT, const LPARAM parameter) noexcept
{
    auto& walk = *reinterpret_cast<EncoderMonitorWalk*>(parameter);
    if (walk.count == walk.monitors.size())
    {
        walk.status = {MonitorCatalogError::ResourceLimit, ERROR_TOO_MANY_NAMES};
        return FALSE;
    }
    MONITORINFOEXW information{};
    information.cbSize = sizeof(information);
    SetLastError(ERROR_SUCCESS);
    if (GetMonitorInfoW(monitor, &information) == FALSE)
    {
        walk.status = {MonitorCatalogError::NativeFailure, static_cast<std::int32_t>(GetLastError())};
        return FALSE;
    }
    UINT dpiX = 0;
    UINT dpiY = 0;
    const HRESULT dpiResult = GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    if (FAILED(dpiResult))
    {
        walk.status = {MonitorCatalogError::MetadataUnavailable, dpiResult};
        return FALSE;
    }
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    SetLastError(ERROR_SUCCESS);
    if (EnumDisplaySettingsExW(information.szDevice, ENUM_CURRENT_SETTINGS, &mode, 0) == FALSE)
    {
        walk.status = {MonitorCatalogError::MetadataUnavailable, static_cast<std::int32_t>(GetLastError())};
        return FALSE;
    }
    walk.status = BuildEncoderMonitor(monitor, information, mode, dpiX, dpiY, walk.monitors[walk.count]);
    if (!walk.status)
    {
        return FALSE;
    }
    walk.count++;
    return TRUE;
}

} // namespace

MonitorCatalogStatus BuildEncoderMonitor(const HMONITOR monitor, const MONITORINFOEXW& information,
    const DEVMODEW& mode, const UINT dpiX, const UINT dpiY, MonitorInfo& output) noexcept
{
    const auto nameEnd = std::find(std::begin(information.szDevice), std::end(information.szDevice), L'\0');
    if (monitor == nullptr || nameEnd == std::begin(information.szDevice) || nameEnd == std::end(information.szDevice) ||
        !RectContains(information.rcMonitor, information.rcMonitor) || dpiX == 0 || dpiY == 0 ||
        (mode.dmFields & DM_DISPLAYORIENTATION) == 0 || mode.dmDisplayOrientation > DMDO_270 ||
        mode.dmPelsWidth == 0 || mode.dmPelsHeight == 0 || mode.dmDisplayFrequency <= 1)
    {
        return {MonitorCatalogError::MetadataUnavailable, ERROR_INVALID_DATA};
    }
    // DEVMODE orientation is counterclockwise; DXGI rotation is clockwise.
    // Sender fullscreen still accepts only Identity. Capture never uses this.
    constexpr std::array rotations{DXGI_MODE_ROTATION_IDENTITY, DXGI_MODE_ROTATION_ROTATE270,
        DXGI_MODE_ROTATION_ROTATE180, DXGI_MODE_ROTATION_ROTATE90};
    try
    {
        MonitorInfo candidate;
        candidate.monitor = monitor;
        candidate.deviceName.assign(information.szDevice, nameEnd);
        candidate.physicalRect = information.rcMonitor;
        candidate.workRect = information.rcWork;
        candidate.dpiX = dpiX;
        candidate.dpiY = dpiY;
        candidate.refreshRate = mode.dmDisplayFrequency;
        candidate.rotation = rotations[mode.dmDisplayOrientation];
        candidate.dxgiOutputIdentityAvailable = false;
        candidate.primary = (information.dwFlags & MONITORINFOF_PRIMARY) != 0;
        const auto origin = GetPhase1CanvasOrigin(candidate.physicalRect, candidate.workRect);
        candidate.supportsPhase1Canvas = origin.has_value();
        if (origin)
        {
            candidate.phase1CanvasOrigin = *origin;
        }
        candidate.phase1ReferenceGeometry = IsPhase1ReferenceMonitor(candidate.physicalRect, candidate.refreshRate);
        output = std::move(candidate);
        return {};
    }
    catch (const std::bad_alloc&)
    {
        return {MonitorCatalogError::OutOfMemory, ERROR_NOT_ENOUGH_MEMORY};
    }
}

MonitorCatalogStatus EnumerateEncoderMonitors(std::vector<MonitorInfo>& output) noexcept
{
    if (!AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
    {
        return {MonitorCatalogError::DpiAwarenessRequired, ERROR_INVALID_STATE};
    }
    EncoderMonitorWalk walk;
    SetLastError(ERROR_SUCCESS);
    if (EnumDisplayMonitors(nullptr, nullptr, &ReadEncoderMonitor, reinterpret_cast<LPARAM>(&walk)) == FALSE)
    {
        if (!walk.status)
        {
            return walk.status;
        }
        const DWORD error = GetLastError();
        return {MonitorCatalogError::NativeFailure, static_cast<std::int32_t>(error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error)};
    }
    if (walk.count == 0)
    {
        return {MonitorCatalogError::MetadataUnavailable, ERROR_NOT_FOUND};
    }
    try
    {
        std::vector<MonitorInfo> candidate(walk.monitors.begin(), walk.monitors.begin() + static_cast<std::ptrdiff_t>(walk.count));
        output = std::move(candidate);
        return {};
    }
    catch (const std::bad_alloc&)
    {
        return {MonitorCatalogError::OutOfMemory, ERROR_NOT_ENOUGH_MEMORY};
    }
}

} // namespace pbapp
