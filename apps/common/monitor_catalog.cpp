#include "monitor_catalog.h"

#include <ShellScalingApi.h>
#include <wrl/client.h>

#include <array>
#include <cwchar>
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

bool GetDxgiOutputIdentity(const wchar_t* deviceName, DXGI_MODE_ROTATION& rotation, LUID& adapterLuid) noexcept
{
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        return false;
    }
    for (UINT adapterIndex = 0;; adapterIndex++)
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        const HRESULT adapterResult = factory->EnumAdapters1(adapterIndex, &adapter);
        if (adapterResult == DXGI_ERROR_NOT_FOUND)
        {
            break;
        }
        if (FAILED(adapterResult))
        {
            return false;
        }
        DXGI_ADAPTER_DESC1 adapterDescription{};
        if (FAILED(adapter->GetDesc1(&adapterDescription)))
        {
            return false;
        }
        for (UINT outputIndex = 0;; outputIndex++)
        {
            Microsoft::WRL::ComPtr<IDXGIOutput> output;
            const HRESULT outputResult = adapter->EnumOutputs(outputIndex, &output);
            if (outputResult == DXGI_ERROR_NOT_FOUND)
            {
                break;
            }
            if (FAILED(outputResult))
            {
                return false;
            }
            DXGI_OUTPUT_DESC outputDescription{};
            if (FAILED(output->GetDesc(&outputDescription)))
            {
                return false;
            }
            if (std::wcscmp(outputDescription.DeviceName, deviceName) == 0)
            {
                rotation = outputDescription.Rotation;
                adapterLuid = adapterDescription.AdapterLuid;
                return true;
            }
        }
    }
    return false;
}

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
    DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_UNSPECIFIED;
    LUID adapterLuid{};
    if (!GetDxgiOutputIdentity(monitorInfo.szDevice, rotation, adapterLuid) ||
        rotation < DXGI_MODE_ROTATION_IDENTITY || rotation > DXGI_MODE_ROTATION_ROTATE270)
    {
        context.status = {MonitorCatalogError::MetadataUnavailable, ERROR_NOT_FOUND};
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
    candidate.rotation = rotation;
    candidate.adapterLuid = adapterLuid;
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

bool RectContains(const RECT& container, const RECT& target) noexcept
{
    const bool validContainer = container.right > container.left && container.bottom > container.top;
    const bool validTarget = target.right > target.left && target.bottom > target.top;
    return validContainer && validTarget && target.left >= container.left && target.top >= container.top &&
        target.right <= container.right && target.bottom <= container.bottom;
}

bool RectIntersects(const RECT& left, const RECT& right) noexcept
{
    if (left.right <= left.left || left.bottom <= left.top || right.right <= right.left || right.bottom <= right.top)
    {
        return false;
    }
    return left.left < right.right && right.left < left.right && left.top < right.bottom && right.top < left.bottom;
}

bool SameMonitorIdentity(const MonitorInfo& left, const MonitorInfo& right) noexcept
{
    return left.monitor == right.monitor && left.deviceName == right.deviceName &&
        EqualRect(&left.physicalRect, &right.physicalRect) != FALSE &&
        left.dpiX == right.dpiX && left.dpiY == right.dpiY && left.refreshRate == right.refreshRate &&
        left.rotation == right.rotation && left.adapterLuid.HighPart == right.adapterLuid.HighPart &&
        left.adapterLuid.LowPart == right.adapterLuid.LowPart && left.primary == right.primary;
}

MonitorSafetyStatus ValidateMonitorSafetyTarget(const MonitorSafetySelection& selection,
    const RECT& target, const HMONITOR targetMonitor) noexcept
{
    const MonitorInfo& protectedMonitor = selection.protectedMonitor;
    const MonitorInfo& experimentMonitor = selection.experimentMonitor;
    if (protectedMonitor.monitor == nullptr || experimentMonitor.monitor == nullptr ||
        protectedMonitor.deviceName.empty() || experimentMonitor.deviceName.empty() ||
        !RectContains(protectedMonitor.physicalRect, protectedMonitor.physicalRect) ||
        !RectContains(experimentMonitor.physicalRect, experimentMonitor.physicalRect))
    {
        return {MonitorSafetyError::InvalidSelection, {}};
    }
    if (protectedMonitor.monitor == experimentMonitor.monitor || protectedMonitor.deviceName == experimentMonitor.deviceName ||
        RectIntersects(protectedMonitor.physicalRect, experimentMonitor.physicalRect))
    {
        return {MonitorSafetyError::SameMonitor, {}};
    }
    if (!RectContains(experimentMonitor.physicalRect, target))
    {
        return {MonitorSafetyError::TargetOutsideExperimentMonitor, {}};
    }
    if (RectIntersects(protectedMonitor.physicalRect, target))
    {
        return {MonitorSafetyError::TargetIntersectsProtectedMonitor, {}};
    }
    if (targetMonitor != nullptr && targetMonitor != experimentMonitor.monitor)
    {
        return {MonitorSafetyError::TargetMonitorMismatch, {}};
    }
    return {};
}

MonitorSafetyStatus ResolveMonitorSafetySelection(const std::wstring_view protectedDeviceName,
    const std::wstring_view experimentDeviceName, MonitorSafetySelection& output) noexcept
{
    if (protectedDeviceName.empty() || experimentDeviceName.empty() || protectedDeviceName == experimentDeviceName)
    {
        return {MonitorSafetyError::InvalidSelection, {}};
    }
    std::vector<MonitorInfo> monitors;
    const MonitorCatalogStatus catalog = EnumerateMonitors(monitors);
    if (!catalog)
    {
        return {MonitorSafetyError::CatalogFailure, catalog};
    }
    const auto Find = [&](const std::wstring_view deviceName) -> const MonitorInfo*
    {
        for (const MonitorInfo& monitor : monitors)
        {
            if (monitor.deviceName == deviceName)
            {
                return &monitor;
            }
        }
        return nullptr;
    };
    const MonitorInfo* const protectedMonitor = Find(protectedDeviceName);
    const MonitorInfo* const experimentMonitor = Find(experimentDeviceName);
    if (protectedMonitor == nullptr || experimentMonitor == nullptr)
    {
        return {MonitorSafetyError::InvalidSelection, {}};
    }
    MonitorSafetySelection candidate{*protectedMonitor, *experimentMonitor};
    const MonitorSafetyStatus safety = ValidateMonitorSafetyTarget(candidate, experimentMonitor->physicalRect,
        experimentMonitor->monitor);
    if (!safety)
    {
        return safety;
    }
    output = std::move(candidate);
    return {};
}

MonitorSafetyStatus RevalidateMonitorSafetySelection(const MonitorSafetySelection& selection) noexcept
{
    std::vector<MonitorInfo> monitors;
    const MonitorCatalogStatus catalog = EnumerateMonitors(monitors);
    if (!catalog)
    {
        return {MonitorSafetyError::CatalogFailure, catalog};
    }
    bool protectedMatched = false;
    bool experimentMatched = false;
    for (const MonitorInfo& monitor : monitors)
    {
        protectedMatched = protectedMatched || SameMonitorIdentity(selection.protectedMonitor, monitor);
        experimentMatched = experimentMatched || SameMonitorIdentity(selection.experimentMonitor, monitor);
    }
    return protectedMatched && experimentMatched ? MonitorSafetyStatus{} :
        MonitorSafetyStatus{MonitorSafetyError::TopologyChanged, {}};
}

const char* GetMonitorSafetyErrorName(const MonitorSafetyError error) noexcept
{
    switch (error)
    {
    case MonitorSafetyError::None: return "None";
    case MonitorSafetyError::InvalidSelection: return "InvalidSelection";
    case MonitorSafetyError::SameMonitor: return "SameMonitor";
    case MonitorSafetyError::TargetOutsideExperimentMonitor: return "TargetOutsideExperimentMonitor";
    case MonitorSafetyError::TargetIntersectsProtectedMonitor: return "TargetIntersectsProtectedMonitor";
    case MonitorSafetyError::TargetMonitorMismatch: return "TargetMonitorMismatch";
    case MonitorSafetyError::TopologyChanged: return "TopologyChanged";
    case MonitorSafetyError::CatalogFailure: return "CatalogFailure";
    }
    return "Unknown";
}

} // namespace pbapp
