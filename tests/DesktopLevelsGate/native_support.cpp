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
    HWND firstOccluder = nullptr;
    RECT firstOccluderRect{};
    DWORD firstOccluderProcessId = 0;
    bool firstOccluderIsChild = false;
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
        if (search.firstOccluder == nullptr)
        {
            search.firstOccluder = child;
            search.firstOccluderRect = clipped;
            GetWindowThreadProcessId(child, &search.firstOccluderProcessId);
            search.firstOccluderIsChild = true;
        }
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
        if (search.firstOccluder == nullptr)
        {
            search.firstOccluder = window;
            search.firstOccluderRect = other;
            GetWindowThreadProcessId(window, &search.firstOccluderProcessId);
        }
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

bool SignedDecimal(const std::wstring_view text, std::int32_t& output)
{
    if (text.empty())
    {
        return false;
    }
    const bool negative = text.front() == L'-';
    const std::wstring_view magnitude = negative ? text.substr(1) : text;
    if (magnitude.empty())
    {
        return false;
    }
    const std::uint64_t maximum = negative ? static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) + 1ULL :
        static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max());
    std::uint64_t value = 0;
    for (const wchar_t character : magnitude)
    {
        if (character < L'0' || character > L'9')
        {
            return false;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(character - L'0');
        if (value > (maximum - digit) / 10)
        {
            return false;
        }
        value = value * 10 + digit;
    }
    if (negative && value == maximum)
    {
        output = std::numeric_limits<std::int32_t>::min();
    }
    else
    {
        const std::int32_t narrowed = static_cast<std::int32_t>(value);
        output = negative ? -narrowed : narrowed;
    }
    return true;
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

int Environment(const std::int32_t originX, const std::int32_t originY)
{
    const POINT requestedOrigin{originX, originY};
    const auto selectedMonitor = MonitorFromPoint(requestedOrigin, MONITOR_DEFAULTTONULL);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!selectedMonitor || !GetMonitorInfoW(selectedMonitor, &monitor))
    {
        std::cout << "{\"error\":\"RequestedOriginIsNotOnAnActiveMonitor\",\"origin\":[" << originX << ',' << originY << "]}\n";
        return 1;
    }
    const auto width = static_cast<std::int64_t>(monitor.rcMonitor.right) - monitor.rcMonitor.left;
    const auto height = static_cast<std::int64_t>(monitor.rcMonitor.bottom) - monitor.rcMonitor.top;
    if (width < 1920 || height < 1080)
    {
        std::cout << "{\"error\":\"InsufficientPhysicalDesktop\",\"width\":" << width << ",\"height\":" << height << "}\n";
        return 1;
    }
    const std::int64_t left = originX;
    const std::int64_t top = originY;
    const std::int64_t right = left + 1920;
    const std::int64_t bottom = top + 1080;
    if (right > std::numeric_limits<LONG>::max() || bottom > std::numeric_limits<LONG>::max() ||
        left < monitor.rcMonitor.left || top < monitor.rcMonitor.top || right > monitor.rcMonitor.right || bottom > monitor.rcMonitor.bottom)
    {
        std::cout << "{\"error\":\"RequestedRoiDoesNotFitSelectedMonitor\",\"requestedRoi\":[" << left << ',' << top << ',' << right << ',' << bottom
                  << "],\"monitor\":[" << monitor.rcMonitor.left << ',' << monitor.rcMonitor.top << ',' << monitor.rcMonitor.right << ',' << monitor.rcMonitor.bottom << "]}\n";
        return 1;
    }
    const RECT rectangle{static_cast<LONG>(left), static_cast<LONG>(top), static_cast<LONG>(right), static_cast<LONG>(bottom)};
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
    if (count == 4 && std::wstring_view(arguments[1]) == L"--environment")
    {
        std::int32_t originX = 0;
        std::int32_t originY = 0;
        if (!SignedDecimal(arguments[2], originX) || !SignedDecimal(arguments[3], originY))
        {
            return 2;
        }
        return Environment(originX, originY);
    }
    const bool observeMode = count == 3 && std::wstring_view(arguments[1]) == L"--window-observe";
    const bool windowMode = count == 3 && std::wstring_view(arguments[1]) == L"--window";
    const bool closeMode = count == 3 && std::wstring_view(arguments[1]) == L"--close";
    const bool resizeMode = count == 5 && std::wstring_view(arguments[1]) == L"--resize";
    if (!observeMode && !windowMode && !closeMode && !resizeMode)
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
    if (resizeMode)
    {
        DWORD width = 0;
        DWORD height = 0;
        if (!Decimal(arguments[3], width) || !Decimal(arguments[4], height) ||
            width < 64 || height < 64 || width > 16384 || height > 16384 ||
            width > static_cast<DWORD>(std::numeric_limits<int>::max()) ||
            height > static_cast<DWORD>(std::numeric_limits<int>::max()))
        {
            return 2;
        }
        if (!SetWindowPos(search.found, nullptr, 0, 0, static_cast<int>(width), static_cast<int>(height),
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW))
        {
            return 1;
        }
        RECT client{};
        if (!GetClientRect(search.found, &client) || client.left != 0 || client.top != 0)
        {
            return 1;
        }
        const std::int64_t observedWidth = static_cast<std::int64_t>(client.right) - client.left;
        const std::int64_t observedHeight = static_cast<std::int64_t>(client.bottom) - client.top;
        std::cout << "{\"found\":true,\"requestedClient\":[" << width << ',' << height
            << "],\"observedClient\":[" << observedWidth << ',' << observedHeight << "]}\n";
        return observedWidth == static_cast<std::int64_t>(width) &&
            observedHeight == static_cast<std::int64_t>(height) ? 0 : 1;
    }
    // Only fixture mode mutates z-order; observation mode reports the true
    // desktop state without any intervention.
    bool raisedApplied = false;
    bool fallbackRaised = false;
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
        // Some local window-protection hooks remove WS_EX_TOPMOST even though
        // SetWindowPos reports success. A non-topmost z-order raise is the
        // narrow reversible fallback: the subsequent full-rectangle scan still
        // has to prove that no window is above the fixture before capture may
        // proceed, and every periodic probe repeats that proof.
        if (!raisedApplied)
        {
            fallbackRaised = SetWindowPos(search.found, HWND_TOP, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW) != FALSE;
        }
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
        std::cout << ",\"raisedApplied\":" << (raisedApplied ? "true" : "false")
            << ",\"fallbackRaised\":" << (fallbackRaised ? "true" : "false");
    }
    std::cout << ",\"roi\":[" << origin.x << ',' << origin.y << ',' << right << ',' << bottom
        << "],\"pointerInsideRoi\":" << (pointerInsideRoi ? "true" : "false") << ",\"occluder\":{\"hwnd\":\""
        << reinterpret_cast<std::uintptr_t>(visibility.firstOccluder) << "\",\"processId\":" << visibility.firstOccluderProcessId
        << ",\"child\":" << (visibility.firstOccluderIsChild ? "true" : "false") << ",\"rect\":["
        << visibility.firstOccluderRect.left << ',' << visibility.firstOccluderRect.top << ',' << visibility.firstOccluderRect.right << ','
        << visibility.firstOccluderRect.bottom << "]}}\n";
    return visible ? 0 : 4;
}
