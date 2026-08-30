#include "pbscreenregion/screen_region.h"
#include "monitor_catalog.h"

#include <Windows.h>

#include <exception>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
void Usage()
{
    std::cerr << "Usage: PixelBridgeDecoder --select-region | --list-monitors\n"
                 "Drag a region fully inside one monitor. Escape or right-click cancels.\n"
                 "Output: physical-desktop RECT, transient HMONITOR, effective DPI, DXGI rotation.\n"
                 "--list-monitors is read-only and emits the physical monitor catalog without capturing pixels.\n"
                 "No screen capture, payload recovery, or capture certification is performed.\n";
}

[[nodiscard]] std::string WideToUtf8(const std::wstring_view value)
{
    if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    {
        throw std::length_error("monitor identity is too long");
    }
    if (value.empty())
    {
        return {};
    }
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0)
    {
        throw std::runtime_error("monitor identity is invalid UTF-16");
    }
    std::string output(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        output.data(), count, nullptr, nullptr) != count)
    {
        throw std::runtime_error("monitor identity conversion failed");
    }
    return output;
}

void WriteJsonString(std::ostream& stream, const std::string_view value)
{
    constexpr char hex[] = "0123456789abcdef";
    stream << '"';
    for (const unsigned char character : value)
    {
        switch (character)
        {
        case '"': stream << "\\\""; break;
        case '\\': stream << "\\\\"; break;
        case '\b': stream << "\\b"; break;
        case '\f': stream << "\\f"; break;
        case '\n': stream << "\\n"; break;
        case '\r': stream << "\\r"; break;
        case '\t': stream << "\\t"; break;
        default:
            if (character < 0x20)
            {
                stream << "\\u00" << hex[character >> 4U] << hex[character & 0x0FU];
            }
            else
            {
                stream << static_cast<char>(character);
            }
            break;
        }
    }
    stream << '"';
}

[[nodiscard]] const char* RotationName(const DXGI_MODE_ROTATION rotation) noexcept
{
    switch (rotation)
    {
    case DXGI_MODE_ROTATION_IDENTITY: return "Identity";
    case DXGI_MODE_ROTATION_ROTATE90: return "Rotate90";
    case DXGI_MODE_ROTATION_ROTATE180: return "Rotate180";
    case DXGI_MODE_ROTATION_ROTATE270: return "Rotate270";
    default: return "Unspecified";
    }
}

int ListMonitors()
{
    std::vector<pbapp::MonitorInfo> monitors;
    const pbapp::MonitorCatalogStatus status = pbapp::EnumerateMonitors(monitors);
    if (!status)
    {
        std::cerr << "MonitorCatalog error=" << static_cast<unsigned int>(status.code) <<
            " native=" << status.nativeError << '\n';
        return 1;
    }
    std::ostringstream stream;
    stream << "{\"schema\":\"PixelBridge.MonitorCatalog.1\",\"monitorCount\":" << monitors.size() <<
        ",\"monitors\":[";
    for (std::size_t index = 0; index < monitors.size(); index++)
    {
        const pbapp::MonitorInfo& monitor = monitors[index];
        if (index != 0)
        {
            stream << ',';
        }
        stream << "{\"deviceName\":";
        WriteJsonString(stream, WideToUtf8(monitor.deviceName));
        stream << ",\"physicalRect\":{\"left\":" << monitor.physicalRect.left << ",\"top\":" <<
            monitor.physicalRect.top << ",\"right\":" << monitor.physicalRect.right << ",\"bottom\":" <<
            monitor.physicalRect.bottom << "},\"workRect\":{\"left\":" << monitor.workRect.left <<
            ",\"top\":" << monitor.workRect.top << ",\"right\":" << monitor.workRect.right <<
            ",\"bottom\":" << monitor.workRect.bottom << "},\"resolution\":{\"width\":" <<
            monitor.physicalRect.right - monitor.physicalRect.left << ",\"height\":" <<
            monitor.physicalRect.bottom - monitor.physicalRect.top << "},\"refreshRate\":" << monitor.refreshRate <<
            ",\"dpiX\":" << monitor.dpiX << ",\"dpiY\":" << monitor.dpiY << ",\"rotation\":";
        WriteJsonString(stream, RotationName(monitor.rotation));
        stream << ",\"adapterLuid\":{\"high\":" << monitor.adapterLuid.HighPart << ",\"low\":" <<
            monitor.adapterLuid.LowPart << "},\"primary\":" << (monitor.primary ? "true" : "false") <<
            ",\"supportsPhase1Canvas\":" << (monitor.supportsPhase1Canvas ? "true" : "false") <<
            ",\"phase1ReferenceGeometry\":" << (monitor.phase1ReferenceGeometry ? "true" : "false") << '}';
    }
    stream << "]}";
    std::cout << stream.str() << '\n';
    std::cout.flush();
    return 0;
}
} // namespace

int RunScreenRegionCommand(const int argumentCount, wchar_t* arguments[])
{
    if (argumentCount == 2 && (std::wstring_view(arguments[1]) == L"--help" || std::wstring_view(arguments[1]) == L"-h"))
    {
        Usage();
        return 0;
    }
    if (argumentCount == 2 && std::wstring_view(arguments[1]) == L"--list-monitors")
    {
        try
        {
            return ListMonitors();
        }
        catch (const std::exception& error)
        {
            std::cerr << "MonitorCatalog: " << error.what() << '\n';
            return 1;
        }
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
