#include "native_support.h"

#include <dwmapi.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>

namespace
{
using namespace pbcapturenormalize;
using Microsoft::WRL::ComPtr;

struct WindowSearch
{
    DWORD processId = 0;
    HWND found = nullptr;
};

BOOL CALLBACK FindOwnedWindow(const HWND window, const LPARAM parameter)
{
    auto& search = *reinterpret_cast<WindowSearch*>(parameter);
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    std::array<wchar_t, 128> title{};
    if (processId == search.processId && GetWindowTextW(window, title.data(), static_cast<int>(title.size())) != 0 &&
        std::wstring_view(title.data()) == L"PixelBridge Data Window")
    {
        search.found = window;
        return FALSE;
    }
    return TRUE;
}

struct VisibilitySearch
{
    HWND target = nullptr;
    RECT rectangle{};
    bool visible = true;
    bool reachedTarget = false;
};

BOOL CALLBACK CheckOcclusion(const HWND window, const LPARAM parameter)
{
    auto& search = *reinterpret_cast<VisibilitySearch*>(parameter);
    if (window == search.target)
    {
        search.reachedTarget = true;
        return FALSE;
    }
    if (!IsWindowVisible(window) || IsIconic(window))
    {
        return TRUE;
    }
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0)
    {
        return TRUE;
    }
    RECT other{};
    RECT intersection{};
    // Fail conservatively on any visible top-level window above the fixture
    // intersecting its full rectangle, not just five sampled points.
    if (GetWindowRect(window, &other) && IntersectRect(&intersection, &search.rectangle, &other))
    {
        search.visible = false;
    }
    return TRUE;
}

bool Decimal(const std::wstring_view text, DWORD& output)
{
    std::uint64_t value = 0;
    if (text.empty())
    {
        return false;
    }
    for (const auto character : text)
    {
        if (character < L'0' || character > L'9')
        {
            return false;
        }
        value = value * 10 + static_cast<unsigned>(character - L'0');
        if (value > std::numeric_limits<DWORD>::max())
        {
            return false;
        }
    }
    output = static_cast<DWORD>(value);
    return output != 0;
}

int Environment()
{
    const auto primary = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!primary || !GetMonitorInfoW(primary, &monitor))
    {
        return 1;
    }
    const auto width = static_cast<std::int64_t>(monitor.rcMonitor.right) - monitor.rcMonitor.left;
    const auto height = static_cast<std::int64_t>(monitor.rcMonitor.bottom) - monitor.rcMonitor.top;
    if (width < 1920 || height < 1080)
    {
        std::cout << "{\"error\":\"InsufficientPhysicalDesktop\",\"width\":" << width << ",\"height\":" << height << "}\n";
        return 1;
    }
    const auto left = static_cast<LONG>(static_cast<std::int64_t>(monitor.rcMonitor.left) + (width - 1920) / 2);
    const auto top = static_cast<LONG>(static_cast<std::int64_t>(monitor.rcMonitor.top) + (height - 1080) / 2);
    // Both additions are bounded by the existing monitor's signed RECT.
    const RECT rectangle{left, top, static_cast<LONG>(left + 1920), static_cast<LONG>(top + 1080)};
    pbscreenregion::ScreenCaptureRegion region;
    if (!pbscreenregion::ResolveScreenCaptureRegion(rectangle, region))
    {
        return 1;
    }
    ComPtr<IDXGIFactory1> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput6> output;
    CaptureEnvironment environment;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !detail::FindCaptureAdapter(factory.Get(), region, adapter, output, environment))
    {
        return 1;
    }
    const bool sdr = !environment.hdr && environment.outputColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    std::cout << "{\"SDR\":" << (sdr ? "true" : "false") << ",\"hdr\":" << (environment.hdr ? "true" : "false")
        << ",\"bitsPerColor\":" << environment.bitsPerColor << ",\"colorSpace\":" << environment.outputColorSpace
        << ",\"adapterHigh\":" << environment.adapterLuid.HighPart << ",\"adapterLow\":" << environment.adapterLuid.LowPart
        << ",\"dpiX\":" << region.dpiX << ",\"dpiY\":" << region.dpiY << ",\"roi\":[" << rectangle.left << ',' << rectangle.top << ','
        << rectangle.right << ',' << rectangle.bottom << "]}\n";
    return sdr ? 0 : 1;
}
} // namespace

int wmain(const int count, const wchar_t* const arguments[])
{
    if (!AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
    {
        std::cerr << "PMv2 process manifest is required; no display settings are changed\n";
        return 1;
    }
    if (count == 2 && std::wstring_view(arguments[1]) == L"--environment")
    {
        return Environment();
    }
    DWORD processId = 0;
    if (count != 3 || !Decimal(arguments[2], processId) ||
        (std::wstring_view(arguments[1]) != L"--window" && std::wstring_view(arguments[1]) != L"--close"))
    {
        return 2;
    }
    WindowSearch search{processId};
    EnumWindows(FindOwnedWindow, reinterpret_cast<LPARAM>(&search));
    if (!search.found)
    {
        std::cout << "{\"found\":false}\n";
        return 3;
    }
    if (std::wstring_view(arguments[1]) == L"--close")
    {
        return PostMessageW(search.found, WM_CLOSE, 0, 0) ? 0 : 1;
    }
    if (!SetWindowPos(search.found, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW))
    {
        return 1;
    }
    RECT client{};
    POINT origin{};
    if (!GetClientRect(search.found, &client) || !ClientToScreen(search.found, &origin))
    {
        return 1;
    }
    const std::int64_t right = static_cast<std::int64_t>(origin.x) + client.right;
    const std::int64_t bottom = static_cast<std::int64_t>(origin.y) + client.bottom;
    if (right > LONG_MAX || bottom > LONG_MAX || client.left != 0 || client.top != 0 || client.right != 1920 || client.bottom != 1080)
    {
        return 1;
    }
    VisibilitySearch visibility{search.found, {origin.x, origin.y, static_cast<LONG>(right), static_cast<LONG>(bottom)}};
    EnumWindows(CheckOcclusion, reinterpret_cast<LPARAM>(&visibility));
    const bool visible = visibility.visible && visibility.reachedTarget && IsWindowVisible(search.found) && !IsIconic(search.found);
    std::cout << "{\"found\":true,\"visible\":" << (visible ? "true" : "false") << ",\"roi\":[" << origin.x << ',' << origin.y << ',' << right << ',' << bottom << "]}\n";
    return visible ? 0 : 4;
}
