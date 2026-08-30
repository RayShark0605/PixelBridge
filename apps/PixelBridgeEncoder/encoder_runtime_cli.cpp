#include "local_desktop_runtime.h"
#include "run_report.h"
#include "diagnostic_file.h"

#include "pbcore/build_info.h"

#include <Windows.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace
{

struct Options
{
    std::wstring sourcePath;
    std::wstring reportPath;
    pbapp::VisualProfile profile = pbapp::VisualProfile::DirectLevels2x2;
    bool compression = false;
    int compressionLevel = 3;
    pbrenderd3d::PhysicalPoint origin{};
    std::uint32_t seconds = 30;
    bool hasOrigin = false;
};

[[nodiscard]] bool ParseSigned(const std::wstring_view text, std::int32_t& output) noexcept
{
    if (text.empty())
    {
        return false;
    }
    const bool negative = text.front() == L'-';
    if (negative && text.size() == 1)
    {
        return false;
    }
    const std::uint64_t maximumMagnitude = negative ? 2147483648ULL : 2147483647ULL;
    std::uint64_t magnitude = 0;
    for (std::size_t index = negative ? 1 : 0; index < text.size(); index++)
    {
        const wchar_t character = text[index];
        if (character < L'0' || character > L'9')
        {
            return false;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(character - L'0');
        if (magnitude > (maximumMagnitude - digit) / 10ULL)
        {
            return false;
        }
        magnitude = magnitude * 10ULL + digit;
    }
    if (negative)
    {
        output = magnitude == 2147483648ULL ? (std::numeric_limits<std::int32_t>::min)() :
            -static_cast<std::int32_t>(magnitude);
    }
    else
    {
        output = static_cast<std::int32_t>(magnitude);
    }
    return true;
}

[[nodiscard]] bool ParseUnsigned(const std::wstring_view text, std::uint32_t& output) noexcept
{
    if (text.empty())
    {
        return false;
    }
    std::uint64_t value = 0;
    for (const wchar_t character : text)
    {
        if (character < L'0' || character > L'9')
        {
            return false;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(character - L'0');
        if (value > ((std::numeric_limits<std::uint32_t>::max)() - digit) / 10ULL)
        {
            return false;
        }
        value = value * 10ULL + digit;
    }
    output = static_cast<std::uint32_t>(value);
    return true;
}

[[nodiscard]] bool ParseOptions(const int argumentCount, const wchar_t* const arguments[], Options& output)
{
    Options options;
    for (int index = 2; index < argumentCount; index++)
    {
        const std::wstring_view option(arguments[index]);
        const auto nextArgument = [&]() -> const wchar_t*
        {
            index++;
            return index < argumentCount ? arguments[index] : nullptr;
        };
        if (option == L"--source")
        {
            const wchar_t* const value = nextArgument();
            if (value == nullptr)
            {
                return false;
            }
            options.sourcePath = value;
        }
        else if (option == L"--report")
        {
            const wchar_t* const value = nextArgument();
            if (value == nullptr)
            {
                return false;
            }
            options.reportPath = value;
        }
        else if (option == L"--profile")
        {
            const wchar_t* const value = nextArgument();
            if (value == nullptr)
            {
                return false;
            }
            const std::wstring_view profile(value);
            if (profile == L"direct")
            {
                options.profile = pbapp::VisualProfile::DirectLevels2x2;
            }
            else if (profile == L"shape")
            {
                options.profile = pbapp::VisualProfile::ShapeChroma;
            }
            else
            {
                return false;
            }
        }
        else if (option == L"--compression")
        {
            const wchar_t* const value = nextArgument();
            if (value == nullptr)
            {
                return false;
            }
            const std::wstring_view compression(value);
            if (compression == L"on")
            {
                options.compression = true;
            }
            else if (compression == L"off")
            {
                options.compression = false;
            }
            else
            {
                return false;
            }
        }
        else if (option == L"--compression-level")
        {
            const wchar_t* const value = nextArgument();
            std::uint32_t level = 0;
            if (value == nullptr || !ParseUnsigned(value, level) || level < 1 || level > 22)
            {
                return false;
            }
            options.compressionLevel = static_cast<int>(level);
        }
        else if (option == L"--origin")
        {
            const wchar_t* const x = nextArgument();
            const wchar_t* const y = nextArgument();
            if (x == nullptr || y == nullptr || !ParseSigned(x, options.origin.x) ||
                !ParseSigned(y, options.origin.y))
            {
                return false;
            }
            options.hasOrigin = true;
        }
        else if (option == L"--seconds")
        {
            const wchar_t* const value = nextArgument();
            if (value == nullptr || !ParseUnsigned(value, options.seconds) || options.seconds == 0 ||
                options.seconds > 600)
            {
                return false;
            }
        }
        else
        {
            return false;
        }
    }
    if (options.sourcePath.empty() || !options.hasOrigin)
    {
        return false;
    }
    output = std::move(options);
    return true;
}

[[nodiscard]] std::string UtcNow()
{
    SYSTEMTIME time{};
    GetSystemTime(&time);
    char buffer[32]{};
    const int count = sprintf_s(buffer, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", time.wYear,
        time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    return count > 0 ? std::string(buffer, static_cast<std::size_t>(count)) : std::string();
}

[[nodiscard]] bool WriteNewReport(const std::wstring& path, const std::string& report)
{
    try
    {
        pbdiagnostic::DiagnosticFile file(path.empty() ? nullptr : path.c_str());
        file.Write(report);
        file.Finish();
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

void Usage()
{
    std::cerr << "usage: PixelBridgeEncoder --headless-broadcast --source PATH --profile direct|shape "
                 "--compression off|on --origin X Y --seconds 1..600 [--compression-level 1..22] [--report NEW_PATH]\n";
}

} // namespace

int RunEncoderRuntimeCommand(const int argumentCount, const wchar_t* const arguments[])
{
    Options options;
    if (!ParseOptions(argumentCount, arguments, options))
    {
        Usage();
        return 2;
    }
    pbapp::EncoderConfig config;
    config.sourcePath = options.sourcePath;
    config.compressionEnabled = options.compression;
    config.compressionLevel = options.compressionLevel;
    config.visualProfile = options.profile;
    config.monitorClientOrigin = options.origin;
    pbapp::EncoderRuntime runtime;
    const pbapp::RuntimeStatus started = runtime.Start(config);
    if (!started)
    {
        std::cerr << started.message << '\n';
        return 2;
    }
    std::optional<std::chrono::steady_clock::time_point> broadcastStarted;
    const auto overallDeadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(options.seconds) + std::chrono::seconds(30);
    bool requestedStopAfterDuration = false;
    bool overallTimeout = false;
    for (;;)
    {
        const pbapp::EncoderSnapshot snapshot = runtime.GetSnapshot();
        if (snapshot.state == pbapp::EncoderState::Broadcasting && !broadcastStarted)
        {
            broadcastStarted = std::chrono::steady_clock::now();
        }
        if (broadcastStarted && std::chrono::steady_clock::now() - *broadcastStarted >=
            std::chrono::seconds(options.seconds))
        {
            requestedStopAfterDuration = true;
            runtime.RequestStop();
        }
        else if (std::chrono::steady_clock::now() >= overallDeadline)
        {
            overallTimeout = true;
            runtime.RequestStop();
        }
        if (snapshot.state == pbapp::EncoderState::Stopped || snapshot.state == pbapp::EncoderState::Failed)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    runtime.Stop();
    const pbapp::EncoderSnapshot snapshot = runtime.GetSnapshot();
    const pbcore::BuildInfo buildInfo = pbcore::GetBuildInfo();
    const pbapp::RunReportContext context{"PixelBridgeEncoder", buildInfo.version, PB_GIT_COMMIT, UtcNow()};
    const std::string report = pbapp::BuildEncoderRunReportJson(context, snapshot);
    std::cout << report << '\n';
    if (!WriteNewReport(options.reportPath, report))
    {
        std::cerr << "failed to create report path\n";
        return 2;
    }
    return snapshot.state == pbapp::EncoderState::Stopped && broadcastStarted && requestedStopAfterDuration &&
        !overallTimeout ? 0 : 1;
}
