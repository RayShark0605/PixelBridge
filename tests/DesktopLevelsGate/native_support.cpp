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
    // Client/fixture intersection in screen coordinates for the window whose
    // children are being enumerated; empty until CheckOcclusion proves the
    // parent client area reaches the fixture.
    RECT parentClient{};
    bool visible = true;
    bool reachedTarget = false;
};

BOOL CALLBACK CheckChildOcclusion(const HWND child, const LPARAM parameter)
{
    auto& search = *reinterpret_cast<VisibilitySearch*>(parameter);
    // Children of a non-iconic, visible, non-cloaked parent (the CheckOcclusion
    // pre-filter) cannot be minimized themselves, so only IsWindowVisible
    // decides. A child draws only inside its parent's client area, so its
    // screen rectangle must first be clipped to search.parentClient (already
    // the client/fixture intersection). That removes the false positives of
    // an unclipped child rectangle while every real occluder still intersects
    // the clipped region, so no true positive is lost.
    if (!IsWindowVisible(child))
    {
        return TRUE;
    }
    RECT childRect{};
    RECT clipped{};
    RECT intersection{};
    if (GetWindowRect(child, &childRect) && IntersectRect(&clipped, &childRect, &search.parentClient) &&
        IntersectRect(&intersection, &clipped, &search.rectangle))
    {
        search.visible = false;
        return FALSE;
    }
    return TRUE;
}

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
    // Children can only cover the part of the fixture their parent's client
    // area reaches; enumerate them only then and hand the child pass the
    // client/fixture intersection, which is bounded by the on-screen fixture
    // and therefore exact and LONG-safe.
    RECT parentClient{};
    POINT clientOrigin{};
    if (GetClientRect(window, &parentClient) && ClientToScreen(window, &clientOrigin) && parentClient.right > 0 && parentClient.bottom > 0)
    {
        const std::int64_t clientLeft = static_cast<std::int64_t>(clientOrigin.x) + parentClient.left;
        const std::int64_t clientTop = static_cast<std::int64_t>(clientOrigin.y) + parentClient.top;
        const std::int64_t clientRight = clientLeft + parentClient.right;
        const std::int64_t clientBottom = clientTop + parentClient.bottom;
        if (clientLeft < search.rectangle.right && clientRight > search.rectangle.left &&
            clientTop < search.rectangle.bottom && clientBottom > search.rectangle.top)
        {
            const std::int64_t fixtureLeft = search.rectangle.left;
            const std::int64_t fixtureTop = search.rectangle.top;
            const std::int64_t fixtureRight = search.rectangle.right;
            const std::int64_t fixtureBottom = search.rectangle.bottom;
            search.parentClient = {static_cast<LONG>(clientLeft > fixtureLeft ? clientLeft : fixtureLeft),
                static_cast<LONG>(clientTop > fixtureTop ? clientTop : fixtureTop),
                static_cast<LONG>(clientRight < fixtureRight ? clientRight : fixtureRight),
                static_cast<LONG>(clientBottom < fixtureBottom ? clientBottom : fixtureBottom)};
            EnumChildWindows(window, CheckChildOcclusion, parameter);
        }
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

// Conservative fail-closed cursor probe: for any cursor shape the bitmap lies
// within [hotspot - (size - 1), hotspot + size), so that box is used; any
// overlap with the region means it cannot be treated as cursor-free. Definitive
// "cursor not drawn" states report false; API failures report true so a gate
// aborts with a clear reason instead of an obscure phase shortfall.
bool PointerInsideRectangle(const RECT& rectangle)
{
    CURSORINFO cursorInfo{};
    cursorInfo.cbSize = sizeof(cursorInfo);
    if (!GetCursorInfo(&cursorInfo))
    {
        return true;
    }
    if ((cursorInfo.flags & CURSOR_SHOWING) == 0 || (cursorInfo.flags & CURSOR_SUPPRESSED) != 0)
    {
        return false;
    }
    POINT position{};
    const int cursorWidth = GetSystemMetrics(SM_CXCURSOR);
    const int cursorHeight = GetSystemMetrics(SM_CYCURSOR);
    if (!GetCursorPos(&position) || cursorWidth <= 0 || cursorHeight <= 0)
    {
        return true;
    }
    const std::int64_t left = static_cast<std::int64_t>(position.x) - (cursorWidth - 1);
    const std::int64_t top = static_cast<std::int64_t>(position.y) - (cursorHeight - 1);
    const std::int64_t right = left + 2 * cursorWidth - 1;
    const std::int64_t bottom = top + 2 * cursorHeight - 1;
    return left < static_cast<std::int64_t>(rectangle.right) && right > static_cast<std::int64_t>(rectangle.left) &&
        top < static_cast<std::int64_t>(rectangle.bottom) && bottom > static_cast<std::int64_t>(rectangle.top);
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
    const bool pointerInsideRoi = PointerInsideRectangle(rectangle);
    std::cout << "{\"SDR\":" << (sdr ? "true" : "false") << ",\"hdr\":" << (environment.hdr ? "true" : "false")
        << ",\"bitsPerColor\":" << environment.bitsPerColor << ",\"colorSpace\":" << environment.outputColorSpace
        << ",\"adapterHigh\":" << environment.adapterLuid.HighPart << ",\"adapterLow\":" << environment.adapterLuid.LowPart
        << ",\"dpiX\":" << region.dpiX << ",\"dpiY\":" << region.dpiY << ",\"roi\":[" << rectangle.left << ',' << rectangle.top << ','
        << rectangle.right << ',' << rectangle.bottom << "],\"pointerInsideRoi\":" << (pointerInsideRoi ? "true" : "false") << "}\n";
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
    const bool observeMode = count == 3 && std::wstring_view(arguments[1]) == L"--window-observe";
    const bool windowMode = count == 3 && std::wstring_view(arguments[1]) == L"--window";
    const bool closeMode = count == 3 && std::wstring_view(arguments[1]) == L"--close";
    if (!observeMode && !windowMode && !closeMode)
    {
        return 2;
    }
    DWORD processId = 0;
    if (!Decimal(arguments[2], processId))
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
    // Only fixture mode mutates z-order; observation mode reports the true
    // desktop state without any intervention.
    bool raisedApplied = false;
    if (windowMode)
    {
        if (!SetWindowPos(search.found, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW))
        {
            return 1;
        }
        // A third-party security hook (observed locally: HIPS strips the
        // attribute) may silently drop WS_EX_TOPMOST while SetWindowPos still
        // reports success. Verify the attribute instead of trusting the
        // return value; the occlusion scan below is z-order based and remains
        // authoritative either way. GetWindowLongPtrW reports failure as -1,
        // whose bits would fake the attribute, so require a live window.
        const LONG_PTR exStyle = GetWindowLongPtrW(search.found, GWL_EXSTYLE);
        raisedApplied = IsWindow(search.found) && exStyle != -1 && (exStyle & WS_EX_TOPMOST) != 0;
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
    const bool pointerInsideRoi = PointerInsideRectangle(visibility.rectangle);
    std::cout << "{\"found\":true,\"visible\":" << (visible ? "true" : "false") << ",\"probeRaised\":"
        << (windowMode ? "true" : "false");
    if (windowMode)
    {
        std::cout << ",\"raisedApplied\":" << (raisedApplied ? "true" : "false");
    }
    std::cout << ",\"roi\":[" << origin.x << ',' << origin.y << ',' << right << ',' << bottom
        << "],\"pointerInsideRoi\":" << (pointerInsideRoi ? "true" : "false") << "}\n";
    return visible ? 0 : 4;
}
