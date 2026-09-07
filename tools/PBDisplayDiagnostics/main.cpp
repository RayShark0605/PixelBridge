#include "application_console.h"
#include "monitor_catalog.h"

#include <ShellScalingApi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{

constexpr std::size_t maximumEntries = 64;

template<std::size_t count>
[[nodiscard]] std::wstring_view ArrayText(const wchar_t (&characters)[count]) noexcept
{
    return {characters, static_cast<std::size_t>(std::find(characters, characters + count, L'\0') - characters)};
}

void WriteJsonString(std::ostream& output, const std::string_view value)
{
    constexpr char hex[] = "0123456789abcdef";
    output << '"';
    for (const unsigned char character : value)
    {
        if (character == '"' || character == '\\')
        {
            output << '\\' << character;
        }
        else if (character < 0x20)
        {
            output << "\\u00" << hex[character >> 4U] << hex[character & 0x0FU];
        }
        else
        {
            output << character;
        }
    }
    output << '"';
}

void WriteJsonWide(std::ostream& output, const std::wstring_view value)
{
    if (value.empty())
    {
        WriteJsonString(output, {});
        return;
    }
    if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    {
        throw std::length_error("display text length exceeded");
    }
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0)
    {
        // Preserve the rest of the diagnostic if a driver returns bad text.
        output << "null";
        return;
    }
    std::string converted(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), converted.data(), count, nullptr, nullptr) != count)
    {
        throw std::runtime_error("display text conversion failed");
    }
    WriteJsonString(output, converted);
}

void WriteRect(std::ostream& output, const RECT& rectangle)
{
    output << '[' << rectangle.left << ',' << rectangle.top << ',' << rectangle.right << ',' << rectangle.bottom << ']';
}

[[nodiscard]] bool RunSelfTest()
{
    std::ostringstream escaped;
    WriteJsonString(escaped, std::string_view("\"\\\n\t\0", 5));
    std::ostringstream unicode;
    WriteJsonWide(unicode, L"虚拟屏");
    std::ostringstream invalid;
    const wchar_t unpairedSurrogate[] = {static_cast<wchar_t>(0xD800)};
    WriteJsonWide(invalid, ArrayText(unpairedSurrogate));
    std::ostringstream rectangle;
    WriteRect(rectangle, RECT{-1920, -100, 0, 980});
    const wchar_t unterminated[] = {L'A', L'B'};
    const wchar_t terminated[] = {L'A', L'\0', L'B'};
    return escaped.str() == "\"\\\"\\\\\\u000a\\u0009\\u0000\"" && unicode.str() == "\"虚拟屏\"" &&
        invalid.str() == "null" && rectangle.str() == "[-1920,-100,0,980]" &&
        ArrayText(unterminated) == L"AB" && ArrayText(terminated) == L"A";
}

struct MonitorProbe
{
    HMONITOR monitor = nullptr;
    MONITORINFOEXW information{};
    DWORD informationError = ERROR_SUCCESS;
    bool informationAvailable = false;
    UINT dpiX = 0;
    UINT dpiY = 0;
    HRESULT dpiResult = E_FAIL;
    DEVMODEW mode{};
    DWORD modeError = ERROR_SUCCESS;
    bool modeAvailable = false;
};

struct MonitorWalk
{
    std::array<MonitorProbe, maximumEntries> monitors{};
    std::size_t count = 0;
    bool limitReached = false;
};

BOOL CALLBACK ReadMonitor(const HMONITOR monitor, HDC, LPRECT, const LPARAM parameter) noexcept
{
    auto& walk = *reinterpret_cast<MonitorWalk*>(parameter);
    if (walk.count == walk.monitors.size())
    {
        walk.limitReached = true;
        return FALSE;
    }
    MonitorProbe& probe = walk.monitors[walk.count];
    walk.count++;
    probe.monitor = monitor;
    probe.information.cbSize = sizeof(probe.information);
    SetLastError(ERROR_SUCCESS);
    probe.informationAvailable = GetMonitorInfoW(monitor, &probe.information) != FALSE;
    probe.informationError = probe.informationAvailable ? ERROR_SUCCESS : GetLastError();
    probe.dpiResult = GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &probe.dpiX, &probe.dpiY);
    if (probe.informationAvailable)
    {
        probe.mode.dmSize = sizeof(probe.mode);
        SetLastError(ERROR_SUCCESS);
        probe.modeAvailable = EnumDisplaySettingsExW(probe.information.szDevice, ENUM_CURRENT_SETTINGS, &probe.mode, 0) != FALSE;
        probe.modeError = probe.modeAvailable ? ERROR_SUCCESS : GetLastError();
    }
    return TRUE;
}

void WriteWin32Monitors(std::ostream& output)
{
    MonitorWalk walk;
    SetLastError(ERROR_SUCCESS);
    const bool success = EnumDisplayMonitors(nullptr, nullptr, &ReadMonitor, reinterpret_cast<LPARAM>(&walk)) != FALSE;
    const DWORD error = success ? ERROR_SUCCESS : GetLastError();
    output << "{\"enumerationSucceeded\":" << success << ",\"nativeError\":" << error << ",\"limitReached\":" << walk.limitReached << ",\"monitors\":[";
    for (std::size_t index = 0; index < walk.count; index++)
    {
        if (index != 0)
        {
            output << ',';
        }
        const MonitorProbe& probe = walk.monitors[index];
        output << "{\"monitorToken\":" << reinterpret_cast<std::uintptr_t>(probe.monitor) << ",\"getMonitorInfoSucceeded\":" << probe.informationAvailable << ",\"getMonitorInfoNativeError\":" << probe.informationError << ",\"deviceName\":";
        WriteJsonWide(output, ArrayText(probe.information.szDevice));
        output << ",\"physicalRect\":";
        WriteRect(output, probe.information.rcMonitor);
        output << ",\"workRect\":";
        WriteRect(output, probe.information.rcWork);
        output << ",\"primary\":" << ((probe.information.dwFlags & MONITORINFOF_PRIMARY) != 0) << ",\"dpiHresult\":" << static_cast<std::int32_t>(probe.dpiResult) << ",\"dpiX\":" << probe.dpiX << ",\"dpiY\":" << probe.dpiY;
        output << ",\"currentModeSucceeded\":" << probe.modeAvailable << ",\"currentModeNativeError\":" << probe.modeError << ",\"modeFields\":" << probe.mode.dmFields << ",\"width\":" << probe.mode.dmPelsWidth << ",\"height\":" << probe.mode.dmPelsHeight << ",\"bitsPerPixel\":" << probe.mode.dmBitsPerPel;
        output << ",\"refreshRateRaw\":" << probe.mode.dmDisplayFrequency << ",\"defaultRefreshMarker\":" << (probe.modeAvailable && probe.mode.dmDisplayFrequency <= 1) << ",\"modeOrientationRaw\":" << probe.mode.dmDisplayOrientation << '}';
    }
    output << "]}";
}

void WriteDisplayDevices(std::ostream& output)
{
    output << "{\"devices\":[";
    DWORD index = 0;
    for (; index < maximumEntries; index++)
    {
        DISPLAY_DEVICEW device{};
        device.cb = sizeof(device);
        if (EnumDisplayDevicesW(nullptr, index, &device, 0) == FALSE)
        {
            break;
        }
        if (index != 0)
        {
            output << ',';
        }
        output << "{\"deviceName\":";
        WriteJsonWide(output, ArrayText(device.DeviceName));
        output << ",\"description\":";
        WriteJsonWide(output, ArrayText(device.DeviceString));
        output << ",\"stateFlags\":" << device.StateFlags << '}';
    }
    output << "],\"limitReached\":" << (index == maximumEntries) << '}';
}

void WriteDxgi(std::ostream& stream)
{
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    const HRESULT factoryResult = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    stream << "{\"factoryHresult\":" << static_cast<std::int32_t>(factoryResult) << ",\"adapters\":[";
    UINT adapterIndex = 0;
    HRESULT adapterEnd = factoryResult;
    if (SUCCEEDED(factoryResult))
    {
        for (; adapterIndex < maximumEntries; adapterIndex++)
        {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            adapterEnd = factory->EnumAdapters1(adapterIndex, &adapter);
            if (FAILED(adapterEnd))
            {
                break;
            }
            if (adapterIndex != 0)
            {
                stream << ',';
            }
            DXGI_ADAPTER_DESC1 description{};
            const HRESULT descriptionResult = adapter->GetDesc1(&description);
            stream << "{\"index\":" << adapterIndex << ",\"descriptionHresult\":" << static_cast<std::int32_t>(descriptionResult) << ",\"description\":";
            WriteJsonWide(stream, ArrayText(description.Description));
            stream << ",\"flags\":" << description.Flags << ",\"software\":" << ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) << ",\"adapterLuid\":{\"high\":" << description.AdapterLuid.HighPart << ",\"low\":" << description.AdapterLuid.LowPart << "},\"outputs\":[";
            UINT outputIndex = 0;
            HRESULT outputEnd = S_OK;
            for (; outputIndex < maximumEntries; outputIndex++)
            {
                Microsoft::WRL::ComPtr<IDXGIOutput> output;
                outputEnd = adapter->EnumOutputs(outputIndex, &output);
                if (FAILED(outputEnd))
                {
                    break;
                }
                if (outputIndex != 0)
                {
                    stream << ',';
                }
                DXGI_OUTPUT_DESC outputDescription{};
                const HRESULT outputResult = output->GetDesc(&outputDescription);
                stream << "{\"index\":" << outputIndex << ",\"descriptionHresult\":" << static_cast<std::int32_t>(outputResult) << ",\"deviceName\":";
                WriteJsonWide(stream, ArrayText(outputDescription.DeviceName));
                stream << ",\"monitorToken\":" << reinterpret_cast<std::uintptr_t>(outputDescription.Monitor) << ",\"attachedToDesktop\":" << (outputDescription.AttachedToDesktop != FALSE) << ",\"rotation\":" << static_cast<unsigned int>(outputDescription.Rotation) << ",\"desktopRect\":";
                WriteRect(stream, outputDescription.DesktopCoordinates);
                stream << '}';
            }
            stream << "],\"outputEnumerationEndHresult\":" << static_cast<std::int32_t>(outputEnd) << ",\"outputLimitReached\":" << (outputIndex == maximumEntries) << '}';
        }
    }
    stream << "],\"adapterEnumerationEndHresult\":" << static_cast<std::int32_t>(adapterEnd) << ",\"adapterLimitReached\":" << (adapterIndex == maximumEntries) << '}';
}

[[nodiscard]] std::string MakeReport()
{
    std::ostringstream output;
    output << std::boolalpha;
    output << "{\"schema\":\"PixelBridge.DisplayDiagnostics.1\",\"metadataOnly\":true,\"capturesPixels\":false,\"testsPresentation\":false,\"checksEncoderWindow\":false,\"changesDisplaySettings\":false,\"baseGitCommit\":\"" << PB_DIAGNOSTIC_BASE_COMMIT << "\",\"probeSourceSha256\":\"" << PB_DIAGNOSTIC_SOURCE_HASH << "\",\"monitorCatalogSourceSha256\":\"" << PB_DIAGNOSTIC_CATALOG_HASH << '"';
    const bool perMonitorV2 = AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE;
    DWORD sessionId = 0;
    const bool sessionAvailable = ProcessIdToSessionId(GetCurrentProcessId(), &sessionId) != FALSE;
    output << ",\"probePerMonitorV2\":" << perMonitorV2 << ",\"sessionId\":";
    if (sessionAvailable)
    {
        output << sessionId;
    }
    else
    {
        output << "null";
    }
    // SM_REMOTESESSION is only a hint; false must not be interpreted as
    // excluding Citrix, indirect displays, or other remote display systems.
    output << ",\"remoteSessionHint\":" << (GetSystemMetrics(SM_REMOTESESSION) != 0);
    std::vector<pbapp::MonitorInfo> productionMonitors;
    const pbapp::MonitorCatalogStatus status = pbapp::EnumerateMonitors(productionMonitors);
    output << ",\"productionMonitorCatalog\":{\"succeeded\":" << static_cast<bool>(status) << ",\"code\":" << static_cast<unsigned int>(status.code) << ",\"nativeError\":" << status.nativeError << ",\"monitorCount\":" << productionMonitors.size() << '}';
    output << ",\"win32\":";
    WriteWin32Monitors(output);
    output << ",\"displayDevices\":";
    WriteDisplayDevices(output);
    output << ",\"dxgi\":";
    WriteDxgi(output);
    output << "}\n";
    return output.str();
}

[[nodiscard]] std::filesystem::path DefaultReportPath()
{
    std::wstring module(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (length == 0 || length >= module.size())
    {
        throw std::runtime_error("executable path unavailable");
    }
    module.resize(length);
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    const std::uint64_t stamp = (static_cast<std::uint64_t>(time.dwHighDateTime) << 32U) | time.dwLowDateTime;
    return std::filesystem::path(module).parent_path() / (L"display-diagnostics-" + std::to_wstring(stamp) + L"-" + std::to_wstring(GetCurrentProcessId()) + L".json");
}

void WriteNewReport(const std::filesystem::path& path, const std::string& report)
{
    if (report.size() > (std::numeric_limits<DWORD>::max)())
    {
        throw std::length_error("report exceeds write bound");
    }
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        throw std::runtime_error("cannot create NEW report, Win32=" + std::to_string(GetLastError()));
    }
    DWORD written = 0;
    const bool writeSucceeded = WriteFile(file, report.data(), static_cast<DWORD>(report.size()), &written, nullptr) != FALSE;
    DWORD error = writeSucceeded ? ERROR_SUCCESS : GetLastError();
    if (writeSucceeded && written != report.size())
    {
        error = ERROR_WRITE_FAULT;
    }
    if (error == ERROR_SUCCESS && FlushFileBuffers(file) == FALSE)
    {
        error = GetLastError();
    }
    const bool closed = CloseHandle(file) != FALSE;
    if (!closed && error == ERROR_SUCCESS)
    {
        error = GetLastError();
    }
    if (error != ERROR_SUCCESS || !closed)
    {
        // Do not delete by path after closing: another process could have
        // replaced it. A write failure can leave an incomplete report.
        throw std::runtime_error("report write/close failed, Win32=" + std::to_string(error));
    }
}

} // namespace

int wmain(const int argumentCount, wchar_t* arguments[])
{
    if (argumentCount > 1 && !pbapp::PrepareCommandLineStreams())
    {
        return 2;
    }
    if (argumentCount == 2 && std::wstring_view(arguments[1]) == L"--self-test")
    {
        const bool passed = RunSelfTest();
        std::cout << "{\"serializationSelfTestPassed\":" << std::boolalpha << passed << ",\"checks\":6}\n";
        return passed && std::cout.good() ? 0 : 2;
    }
    const bool stdoutOnly = argumentCount == 2 && std::wstring_view(arguments[1]) == L"--stdout";
    const bool explicitOutput = argumentCount == 3 && std::wstring_view(arguments[1]) == L"--output" && arguments[2][0] != L'\0';
    if (argumentCount != 1 && !stdoutOnly && !explicitOutput)
    {
        std::cerr << "PBDisplayDiagnostics [--stdout | --output NEW_JSON_PATH]\nNo arguments: write a new display-diagnostics-*.json beside this EXE, no window.\nMetadata only: no capture, presentation, device creation, input, network, or display setting change.\n";
        return argumentCount == 2 && std::wstring_view(arguments[1]) == L"--help" ? 0 : 2;
    }
    try
    {
        const std::string report = MakeReport();
        if (stdoutOnly)
        {
            std::cout << report << std::flush;
            return std::cout.good() ? 0 : 2;
        }
        WriteNewReport(explicitOutput ? std::filesystem::path(arguments[2]) : DefaultReportPath(), report);
        return 0;
    }
    catch (const std::exception& error)
    {
        if (argumentCount > 1)
        {
            std::cerr << "PBDisplayDiagnostics: " << error.what() << '\n';
        }
        return 2;
    }
}
