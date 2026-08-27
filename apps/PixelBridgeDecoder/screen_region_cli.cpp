#include "pbscreenregion/screen_region.h"

#include <exception>
#include <iostream>
#include <string_view>

namespace
{
void Usage()
{
    std::cerr << "Usage: PixelBridgeDecoder --select-region\n"
                 "Drag a region fully inside one monitor. Escape or right-click cancels.\n"
                 "Output: physical-desktop RECT, transient HMONITOR, effective DPI, DXGI rotation.\n"
                 "No screen capture, payload recovery, or capture certification is performed.\n";
}
} // namespace

int RunScreenRegionCommand(const int argumentCount, wchar_t* arguments[])
{
    if (argumentCount == 2 && (std::wstring_view(arguments[1]) == L"--help" || std::wstring_view(arguments[1]) == L"-h"))
    {
        Usage();
        return 0;
    }
    if (argumentCount != 2 || std::wstring_view(arguments[1]) != L"--select-region")
    {
        Usage();
        return 2;
    }
    try
    {
        pbscreenregion::ScreenCaptureRegion region;
        const auto status = pbscreenregion::SelectScreenCaptureRegion(region);
        if (!status)
        {
            std::cerr << "Screen region: " << pbscreenregion::GetScreenRegionErrorName(status.code) << "; stage=" << static_cast<unsigned int>(status.stage)
                      << "; native=" << status.nativeError << '\n';
            return status.code == pbscreenregion::ScreenRegionErrorCode::Cancelled ? 3 : 1;
        }
        std::cout.exceptions(std::ios::badbit | std::ios::failbit);
        pbscreenregion::WriteScreenCaptureRegionJson(std::cout, region);
        std::cout << '\n';
        std::cout.flush();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Screen region: " << error.what() << '\n';
        return 1;
    }
}
