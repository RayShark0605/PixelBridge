#include "local_desktop_runtime.h"
#include "run_report.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#ifndef PB_GIT_COMMIT
#define PB_GIT_COMMIT "unknown"
#endif

namespace
{

enum class Mode
{
    Describe,
    Broadcast
};

struct Options
{
    Mode mode = Mode::Describe;
    std::wstring sourcePath;
    std::wstring reportPath;
    pbrenderd3d::PhysicalPoint origin{};
    std::uint32_t seconds = 0;
    std::uint32_t logicalVisualFps = 5;
    std::uint32_t controlRepetitions = 1;
    bool sourceSpecified = false;
    bool reportSpecified = false;
    bool originSpecified = false;
    bool secondsSpecified = false;
    bool logicalVisualFpsSpecified = false;
    bool controlRepetitionsSpecified = false;
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
    output = negative ? magnitude == 2147483648ULL ? (std::numeric_limits<std::int32_t>::min)() :
        -static_cast<std::int32_t>(magnitude) : static_cast<std::int32_t>(magnitude);
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
    if (argumentCount < 2)
    {
        return false;
    }
    Options options;
    const std::wstring_view mode(arguments[1]);
    if (mode == L"describe")
    {
        if (argumentCount != 2)
        {
            return false;
        }
        output = options;
        return true;
    }
    if (mode != L"broadcast")
    {
        return false;
    }
    options.mode = Mode::Broadcast;
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
            if (value == nullptr || options.sourceSpecified || *value == L'\0')
            {
                return false;
            }
            options.sourcePath = value;
            options.sourceSpecified = true;
        }
        else if (option == L"--report")
        {
            const wchar_t* const value = nextArgument();
            if (value == nullptr || options.reportSpecified || *value == L'\0')
            {
                return false;
            }
            options.reportPath = value;
            options.reportSpecified = true;
        }
        else if (option == L"--origin")
        {
            const wchar_t* const x = nextArgument();
            const wchar_t* const y = nextArgument();
            if (x == nullptr || y == nullptr || options.originSpecified ||
                !ParseSigned(x, options.origin.x) || !ParseSigned(y, options.origin.y))
            {
                return false;
            }
            options.originSpecified = true;
        }
        else if (option == L"--seconds")
        {
            const wchar_t* const value = nextArgument();
            if (value == nullptr || options.secondsSpecified || !ParseUnsigned(value, options.seconds) ||
                options.seconds < 2 || options.seconds > 600)
            {
                return false;
            }
            options.secondsSpecified = true;
        }
        else if (option == L"--logical-fps")
        {
            const wchar_t* const value = nextArgument();
            if (value == nullptr || options.logicalVisualFpsSpecified ||
                !ParseUnsigned(value, options.logicalVisualFps) ||
                options.logicalVisualFps == 0 || options.logicalVisualFps > 5)
            {
                return false;
            }
            options.logicalVisualFpsSpecified = true;
        }
        else if (option == L"--control-repetitions")
        {
            const wchar_t* const value = nextArgument();
            if (value == nullptr || options.controlRepetitionsSpecified ||
                !ParseUnsigned(value, options.controlRepetitions) ||
                options.controlRepetitions == 0 || options.controlRepetitions > 64)
            {
                return false;
            }
            options.controlRepetitionsSpecified = true;
        }
        else
        {
            return false;
        }
    }
    if (!options.sourceSpecified || !options.originSpecified || !options.secondsSpecified)
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

[[nodiscard]] bool WriteCreateOnly(const std::filesystem::path& path, const std::string_view bytes,
    std::string& error) noexcept
{
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        error = "cannot create report without overwrite, Win32=" + std::to_string(GetLastError());
        return false;
    }
    bool success = true;
    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        const std::size_t remaining = bytes.size() - offset;
        const DWORD requested = static_cast<DWORD>((std::min)(remaining,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD written = 0;
        if (WriteFile(file, bytes.data() + offset, requested, &written, nullptr) == FALSE)
        {
            error = "report write failed, Win32=" + std::to_string(GetLastError());
            success = false;
            break;
        }
        if (written == 0)
        {
            error = "report write made no forward progress";
            success = false;
            break;
        }
        offset += written;
    }
    if (success && FlushFileBuffers(file) == FALSE)
    {
        error = "report flush failed, Win32=" + std::to_string(GetLastError());
        success = false;
    }
    if (CloseHandle(file) == FALSE && success)
    {
        error = "report close failed, Win32=" + std::to_string(GetLastError());
        success = false;
    }
    if (!success)
    {
        DeleteFileW(path.c_str());
    }
    return success;
}

void Describe()
{
    std::cout << "{\"schema\":\"PixelBridge.RemoteVisualLf4DynamicPresenter.1\","
                 "\"profile\":\"PB-RemoteVisual-LF4-X1\",\"productionEncoderRuntime\":true,"
                 "\"dynamicCarousel\":true,\"logicalFpsMinimum\":1,\"logicalFpsMaximum\":5,"
                 "\"secondsMinimum\":2,\"secondsMaximum\":600,\"productProfileExposed\":false}\n";
}

void PrintProgress(const pbapp::EncoderSnapshot& snapshot, const std::uint64_t elapsedSeconds)
{
    std::cout << "PROGRESS elapsedSeconds=" << elapsedSeconds << " frameSequence=" << snapshot.frameSequence <<
        " cycle=" << snapshot.cycleCount << " position=" << snapshot.cyclePosition << '/' <<
        snapshot.cycleFrameCount << " sourceReplacements=" << snapshot.sourceTextureReplacements <<
        " repeatedPresents=" << snapshot.repeatedPresentCalls << " generatedVisualFps=" <<
        snapshot.generatedVisualFramesPerSecond << '\n' << std::flush;
}

[[nodiscard]] int Broadcast(const Options& options)
{
    pbapp::EncoderConfig config;
    config.sourcePath = options.sourcePath;
    config.visualProfile = pbapp::VisualProfile::RemoteVisualLowFps;
    config.monitorClientOrigin = options.origin;
    config.logicalVisualFps = options.logicalVisualFps;
    config.controlRepetitions = options.controlRepetitions;
    config.remoteMetadata.channelType = pbapp::ChannelType::RemoteVisual;
    config.remoteMetadata.remoteProvider = "PBRemoteVisualLf4DynamicPresenter";
    config.remoteMetadata.remoteMode = "EvidenceOnlyDynamicFieldPilot";
    config.remoteMetadata.notes = "Production EncoderRuntime LF4 hidden candidate; no product GUI or CLI admission";

    pbapp::EncoderRuntime runtime;
    const pbapp::RuntimeStatus started = runtime.Start(config);
    if (!started)
    {
        std::cerr << "PBRemoteVisualLf4DynamicPresenter failed to start: " << started.message << '\n';
        return 1;
    }
    const auto commandStarted = std::chrono::steady_clock::now();
    const auto readyDeadline = commandStarted + std::chrono::seconds(30);
    std::optional<std::chrono::steady_clock::time_point> broadcastStarted;
    std::optional<std::chrono::steady_clock::time_point> nextProgress;
    std::uint64_t readyFrameSequence = 0;
    std::uint64_t readySourceReplacements = 0;
    bool requestedStopAfterDuration = false;
    bool readyTimeout = false;
    for (;;)
    {
        const auto now = std::chrono::steady_clock::now();
        const pbapp::EncoderSnapshot snapshot = runtime.GetSnapshot();
        if (!broadcastStarted && snapshot.state == pbapp::EncoderState::Broadcasting &&
            snapshot.candidateContractSatisfied && snapshot.activeFrame && snapshot.sourceTextureReplacements != 0)
        {
            broadcastStarted = now;
            nextProgress = now + std::chrono::seconds(1);
            readyFrameSequence = snapshot.frameSequence;
            readySourceReplacements = snapshot.sourceTextureReplacements;
            std::cout << "READY profile=PB-RemoteVisual-LF4-X1 origin=" << options.origin.x << ',' << options.origin.y <<
                " size=" << pbapp::phase1CanvasWidth << 'x' << pbapp::phase1CanvasHeight <<
                " logicalFps=" << options.logicalVisualFps << " sourceBytes=" << snapshot.sourceBytes <<
                " outerFec=" << pbapp::GetOuterFecModeName(snapshot.outerFecMode) <<
                " controlRepetitions=" << options.controlRepetitions << " cycleFrames=" << snapshot.cycleFrameCount <<
                " frameSequence=" << snapshot.frameSequence << " dynamicCarousel=true productionEncoderRuntime=true\n" <<
                std::flush;
        }
        if (broadcastStarted && nextProgress && now >= *nextProgress)
        {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - *broadcastStarted).count();
            PrintProgress(snapshot, elapsed < 0 ? 0 : static_cast<std::uint64_t>(elapsed));
            do
            {
                *nextProgress += std::chrono::seconds(1);
            } while (*nextProgress <= now);
        }
        if (snapshot.state == pbapp::EncoderState::Failed || snapshot.state == pbapp::EncoderState::Stopped)
        {
            break;
        }
        if (broadcastStarted && now - *broadcastStarted >= std::chrono::seconds(options.seconds))
        {
            requestedStopAfterDuration = true;
            runtime.RequestStop();
        }
        else if (!broadcastStarted && now >= readyDeadline)
        {
            readyTimeout = true;
            runtime.RequestStop();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    runtime.Stop();
    const pbapp::EncoderSnapshot finalSnapshot = runtime.GetSnapshot();
    const pbapp::RunReportContext reportContext{"PBRemoteVisualLf4DynamicPresenter", "1", PB_GIT_COMMIT, UtcNow()};
    const std::string report = pbapp::BuildEncoderRunReportJson(reportContext, finalSnapshot) + "\n";
    std::cout << report;
    if (!options.reportPath.empty())
    {
        std::string error;
        if (!WriteCreateOnly(options.reportPath, report, error))
        {
            std::cerr << "PBRemoteVisualLf4DynamicPresenter report failed: " << error << '\n';
            return 2;
        }
    }
    const bool dynamicFrameAdvance = broadcastStarted && finalSnapshot.frameSequence > readyFrameSequence &&
        finalSnapshot.sourceTextureReplacements > readySourceReplacements;
    const bool passed = finalSnapshot.state == pbapp::EncoderState::Stopped && requestedStopAfterDuration &&
        !readyTimeout && dynamicFrameAdvance && finalSnapshot.sourceStable &&
        finalSnapshot.logicalDwellViolationCount == 0 && finalSnapshot.errorDetail.empty();
    if (!passed)
    {
        std::cerr << "PBRemoteVisualLf4DynamicPresenter failed: state=" <<
            pbapp::GetEncoderStateName(finalSnapshot.state) << " ready=" << std::boolalpha <<
            broadcastStarted.has_value() << " requestedStop=" << requestedStopAfterDuration <<
            " readyTimeout=" << readyTimeout << " dynamicFrameAdvance=" << dynamicFrameAdvance <<
            " error=" << finalSnapshot.errorDetail << '\n';
        return 1;
    }
    std::cout << "COMPLETE profile=PB-RemoteVisual-LF4-X1 frameSequence=" << finalSnapshot.frameSequence <<
        " cycles=" << finalSnapshot.cycleCount << " sourceReplacements=" << finalSnapshot.sourceTextureReplacements <<
        " repeatedPresents=" << finalSnapshot.repeatedPresentCalls << " dynamicFrameAdvance=true\n";
    return 0;
}

void Usage()
{
    std::cerr << "usage: PBRemoteVisualLf4DynamicPresenter describe\n"
                 "       PBRemoteVisualLf4DynamicPresenter broadcast --source PATH --origin X Y --seconds 2..600 "
                 "[--logical-fps 1..5] [--control-repetitions 1..64] [--report NEW_PATH]\n";
}

} // namespace

int wmain(const int argumentCount, const wchar_t* const arguments[])
{
    Options options;
    if (!ParseOptions(argumentCount, arguments, options))
    {
        Usage();
        return 2;
    }
    if (options.mode == Mode::Describe)
    {
        Describe();
        return 0;
    }
    try
    {
        return Broadcast(options);
    }
    catch (const std::exception& exception)
    {
        std::cerr << "PBRemoteVisualLf4DynamicPresenter failed: " << exception.what() << '\n';
        return 1;
    }
    catch (...)
    {
        std::cerr << "PBRemoteVisualLf4DynamicPresenter failed with an unknown exception\n";
        return 1;
    }
}
