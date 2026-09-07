#pragma once

#include "monitor_catalog.h"

namespace pbapp
{

// Encoder presentation only. Never substitute this catalog for capture/ROI
// selection: rotation comes from DEVMODE and no DXGI output/LUID is asserted.
[[nodiscard]] MonitorCatalogStatus EnumerateEncoderMonitors(std::vector<MonitorInfo>& output) noexcept;

// Pure metadata validation shared by the native walk and boundary tests.
[[nodiscard]] MonitorCatalogStatus BuildEncoderMonitor(HMONITOR monitor, const MONITORINFOEXW& information,
    const DEVMODEW& mode, UINT dpiX, UINT dpiY, MonitorInfo& output) noexcept;

} // namespace pbapp
