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
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace
{

struct Options
{
    std::wstring outputDirectory;
    std::wstring reportPath;
    pbapp::CaptureBackend backend = pbapp::CaptureBackend::Wgc;
    pbapp::VisualProfile profile = pbapp::VisualProfile::DirectLevels2x2;
    RECT roi{};
    std::uint32_t timeoutSeconds = 120;
    bool hasRoi = false;
};

[[nodiscard]] bool ParseSigned(const std::wstring_view text, LONG& output) noexcept
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
        output = magnitude == 2147483648ULL ? (std::numeric_limits<LONG>::min)() :
            -static_cast<LONG>(magnitude);
    }
    else
    {
        output = static_cast<LONG>(magnitude);
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
        if (option == L"--output-dir")
        {
            const wchar_t* const value = nextArgument();
            if (value == nullptr)
            {
                return false;
            }
            options.outputDirectory = value;
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
        else if (option == L"--backend")
        {
            const wchar_t* const value = nextArgument();
            if (value == nullptr)
            {
                return false;
            }
            const std::wstring_view backend(value);
            if (backend == L"wgc")
            {
                options.backend = pbapp::CaptureBackend::Wgc;
            }
            else if (backend == L"dxgi")
            {
                options.backend = pbapp::CaptureBackend::Dxgi;
            }
            else
            {
                return false;
            }
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
        else if (option == L"--roi")
        {
            const wchar_t* const left = nextArgument();
            const wchar_t* const top = nextArgument();
            const wchar_t* const right = nextArgument();
            const wchar_t* const bottom = nextArgument();
            if (left == nullptr || top == nullptr || right == nullptr || bottom == nullptr ||
                !ParseSigned(left, options.roi.left) || !ParseSigned(top, options.roi.top) ||
                !ParseSigned(right, options.roi.right) || !ParseSigned(bottom, options.roi.bottom))
            {
                return false;
            }
            options.hasRoi = true;
        }
        else if (option == L"--timeout")
        {
            const wchar_t* const value = nextArgument();
            if (value == nullptr || !ParseUnsigned(value, options.timeoutSeconds) ||
                options.timeoutSeconds == 0 || options.timeoutSeconds > 600)
            {
                return false;
            }
        }
        else
        {
            return false;
        }
    }
    if (options.outputDirectory.empty() || !options.hasRoi)
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
    std::cerr << "usage: PixelBridgeDecoder --headless-receive --output-dir DIR --backend wgc|dxgi "
                 "--profile direct|shape --roi LEFT TOP RIGHT BOTTOM --timeout 1..600 [--report NEW_PATH]\n";
}

} // namespace

int RunDecoderRuntimeCommand(const int argumentCount, const wchar_t* const arguments[])
{
    Options options;
    if (!ParseOptions(argumentCount, arguments, options))
    {
        Usage();
        return 2;
    }
    pbscreenregion::ScreenCaptureRegion region;
    const auto regionStatus = pbscreenregion::ResolveScreenCaptureRegion(options.roi, region);
    if (!regionStatus)
    {
        std::cerr << "ROI resolution failed: " << pbscreenregion::GetScreenRegionErrorName(regionStatus.code) << '\n';
        return 2;
    }
    pbapp::DecoderConfig config;
    config.outputDirectory = options.outputDirectory;
    config.captureBackend = options.backend;
    config.visualProfile = options.profile;
    config.region = region;
    pbapp::DecoderRuntime runtime;
    const pbapp::RuntimeStatus started = runtime.Start(config);
    if (!started)
    {
        std::cerr << started.message << '\n';
        return 2;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(options.timeoutSeconds);
    for (;;)
    {
        const pbapp::DecoderSnapshot snapshot = runtime.GetSnapshot();
        if (snapshot.state == pbapp::DecoderState::Completed || snapshot.state == pbapp::DecoderState::Failed ||
            snapshot.state == pbapp::DecoderState::Stopped)
        {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            runtime.RequestStop();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    runtime.Stop();
    const pbapp::DecoderSnapshot snapshot = runtime.GetSnapshot();
    const pbcore::BuildInfo buildInfo = pbcore::GetBuildInfo();
    const pbapp::RunReportContext context{"PixelBridgeDecoder", buildInfo.version, PB_GIT_COMMIT, UtcNow()};
    const std::string report = pbapp::BuildDecoderRunReportJson(context, snapshot);
    std::cout << report << '\n';
    if (!WriteNewReport(options.reportPath, report))
    {
        std::cerr << "failed to create report path\n";
        return 2;
    }
    return snapshot.state == pbapp::DecoderState::Completed && snapshot.wholeFileDigestVerified &&
        snapshot.finalPublishSucceeded ? 0 : 1;
}
