#include "pbscreenregion/screen_region.h"

#include <string_view>

int main(const int argumentCount, char* arguments[])
{
    if (argumentCount != 2)
    {
        return 10;
    }
    const std::string_view mode(arguments[1]);
    const auto context = mode == "unaware"                    ? DPI_AWARENESS_CONTEXT_UNAWARE
                         : mode == "system"                   ? DPI_AWARENESS_CONTEXT_SYSTEM_AWARE
                         : mode == "per-monitor-v1"           ? DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE
                         : mode == "thread-only-v2"           ? DPI_AWARENESS_CONTEXT_UNAWARE
                         : mode == "process-v2-thread-system" ? DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
                                                              : nullptr;
    // Test hosts deliberately have no PMv2 manifest. Set the process default
    // once, before any HWND or any library call; never patch a running GUI host.
    if (context == nullptr || !SetProcessDpiAwarenessContext(context))
    {
        return 11;
    }
    if (mode == "thread-only-v2" && SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) == nullptr)
    {
        return 13;
    }
    if (mode == "process-v2-thread-system" && SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE) == nullptr)
    {
        return 14;
    }
    const DWORD before = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    pbscreenregion::ScreenCaptureRegion output;
    output.dpiX = 123;
    const auto selection = pbscreenregion::SelectScreenCaptureRegion(output);
    const auto resolution = pbscreenregion::ResolveScreenCaptureRegion(RECT{0, 0, 1, 1}, output);
    const DWORD after = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    const bool rejected = selection.code == pbscreenregion::ScreenRegionErrorCode::DpiAwarenessRequired &&
                          selection.stage == pbscreenregion::ScreenRegionStage::DpiAwareness && resolution == selection;
    return rejected && before == after && output.dpiX == 123 && output.monitor == nullptr ? 0 : 12;
}
