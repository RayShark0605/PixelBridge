#pragma once

#include "application_model.h"

#include <Windows.h>

#include <cstdint>
#include <optional>
#include <string>
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

[[nodiscard]] bool IsPhase1ReferenceMonitor(const RECT& physicalRect,
    std::uint32_t refreshRate) noexcept;
[[nodiscard]] bool CanHostPhase1Canvas(const RECT& physicalRect) noexcept;
[[nodiscard]] std::optional<POINT> GetPhase1CanvasOrigin(const RECT& physicalRect,
    const RECT& workRect) noexcept;
[[nodiscard]] MonitorCatalogStatus EnumerateMonitors(std::vector<MonitorInfo>& output) noexcept;

} // namespace pbapp
