#pragma once

#include "application_model.h"

#include <Windows.h>
#include <dxgi.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pbapp
{

struct MonitorInfo
{
    HMONITOR monitor = nullptr;
    std::wstring deviceName;
    RECT physicalRect{};
    RECT workRect{};
    POINT phase1CanvasOrigin{};
    std::uint32_t dpiX = 0;
    std::uint32_t dpiY = 0;
    std::uint32_t refreshRate = 0;
    DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_UNSPECIFIED;
    LUID adapterLuid{};
    bool primary = false;
    bool supportsPhase1Canvas = false;
    bool phase1ReferenceGeometry = false;
};

enum class MonitorCatalogError : std::uint8_t
{
    None,
    DpiAwarenessRequired,
    MetadataUnavailable,
    ResourceLimit,
    NativeFailure,
    OutOfMemory
};

struct MonitorCatalogStatus
{
    MonitorCatalogError code = MonitorCatalogError::None;
    std::int32_t nativeError = 0;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return code == MonitorCatalogError::None;
    }
};

struct MonitorSafetySelection
{
    MonitorInfo protectedMonitor;
    MonitorInfo experimentMonitor;
};

enum class MonitorSafetyError : std::uint8_t
{
    None,
    InvalidSelection,
    SameMonitor,
    TargetOutsideExperimentMonitor,
    TargetIntersectsProtectedMonitor,
    TargetMonitorMismatch,
    TopologyChanged,
    CatalogFailure
};

struct MonitorSafetyStatus
{
    MonitorSafetyError code = MonitorSafetyError::None;
    MonitorCatalogStatus catalogStatus;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return code == MonitorSafetyError::None;
    }
};

[[nodiscard]] bool IsPhase1ReferenceMonitor(const RECT& physicalRect,
    std::uint32_t refreshRate) noexcept;
[[nodiscard]] bool CanHostPhase1Canvas(const RECT& physicalRect) noexcept;
[[nodiscard]] std::optional<POINT> GetPhase1CanvasOrigin(const RECT& physicalRect,
    const RECT& workRect) noexcept;
[[nodiscard]] MonitorCatalogStatus EnumerateMonitors(std::vector<MonitorInfo>& output) noexcept;
[[nodiscard]] bool RectContains(const RECT& container, const RECT& target) noexcept;
[[nodiscard]] bool RectIntersects(const RECT& left, const RECT& right) noexcept;
[[nodiscard]] bool SameMonitorIdentity(const MonitorInfo& left, const MonitorInfo& right) noexcept;
[[nodiscard]] MonitorSafetyStatus ValidateMonitorSafetyTarget(const MonitorSafetySelection& selection,
    const RECT& target, HMONITOR targetMonitor = nullptr) noexcept;
[[nodiscard]] MonitorSafetyStatus ResolveMonitorSafetySelection(std::wstring_view protectedDeviceName,
    std::wstring_view experimentDeviceName, MonitorSafetySelection& output) noexcept;
[[nodiscard]] MonitorSafetyStatus RevalidateMonitorSafetySelection(const MonitorSafetySelection& selection) noexcept;
[[nodiscard]] const char* GetMonitorSafetyErrorName(MonitorSafetyError error) noexcept;

} // namespace pbapp
