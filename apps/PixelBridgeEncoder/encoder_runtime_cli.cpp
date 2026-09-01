#include "local_desktop_runtime.h"
#include "run_report.h"
#include "diagnostic_file.h"
#include "evidence_journal.h"
#include "remote_visual_metadata_preset_qt.h"

#include "pbcore/build_info.h"

#include <Windows.h>

#include <QString>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
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
    std::wstring journalPath;
    std::string runId;
    pbapp::VisualProfile profile = pbapp::VisualProfile::DirectLevels2x2;
    bool compression = false;
    int compressionLevel = 3;
    pbrenderd3d::PhysicalPoint origin{};
    std::uint32_t seconds = 30;
    std::uint32_t logicalVisualFps = 0;
    std::uint32_t controlRepetitions = 4;
    bool hasOrigin = false;
    bool logicalVisualFpsSpecified = false;
    bool controlRepetitionsSpecified = false;
    bool remoteChannel = false;
    bool channelSpecified = false;
    std::wstring remoteProvider;
    std::wstring remoteMetadataPath;
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
        else if (option == L"--run-id")
        {
            const wchar_t* const value = nextArgument();
            if (value == nullptr)
            {
                return false;
            }
            const std::wstring_view wideValue(value);
            if (wideValue.size() != 32 || !std::ranges::all_of(wideValue, [](const wchar_t character)
                { return (character >= L'0' && character <= L'9') || (character >= L'a' && character <= L'f'); }))
            {
                return false;
            }
            options.runId.resize(wideValue.size());
            std::transform(wideValue.begin(), wideValue.end(), options.runId.begin(),
                [](const wchar_t character) { return static_cast<char>(character); });
        }
        else if (option == L"--journal")
        {
            const wchar_t* const value = nextArgument();
            if (value == nullptr)
            {
                return false;
            }
            options.journalPath = value;
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
            else if (profile == L"remote")
            {
                options.profile = pbapp::VisualProfile::RemoteVisualResilient;
            }
            else
            {
                return false;
            }
        }
        else if (option == L"--channel")
        {
            const wchar_t* const value = nextArgument();
            if (value == nullptr)
            {
                return false;
            }
            const std::wstring_view channel(value);
            if (channel == L"local")
            {
                options.remoteChannel = false;
            }
            else if (channel == L"remote")
            {
                options.remoteChannel = true;
            }
            else
            {
                return false;
            }
            options.channelSpecified = true;
        }
        else if (option == L"--remote-provider")
        {
            const wchar_t* const value = nextArgument();
            if (value == nullptr)
            {
                return false;
            }
            options.remoteProvider = value;
        }
        else if (option == L"--remote-metadata")
        {
            const wchar_t* const value = nextArgument();
            if (value == nullptr)
            {
                return false;
            }
            options.remoteMetadataPath = value;
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
        else if (option == L"--logical-fps")
        {
            const wchar_t* const value = nextArgument();
            if (value == nullptr || !ParseUnsigned(value, options.logicalVisualFps) ||
                options.logicalVisualFps > 240)
            {
                return false;
            }
            options.logicalVisualFpsSpecified = true;
        }
        else if (option == L"--control-repetitions")
        {
            const wchar_t* const value = nextArgument();
            if (value == nullptr || !ParseUnsigned(value, options.controlRepetitions) ||
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
    if (options.sourcePath.empty() || !options.hasOrigin)
    {
        return false;
    }
    if (options.profile == pbapp::VisualProfile::RemoteVisualResilient)
    {
        if (!options.channelSpecified)
        {
            options.remoteChannel = true;
        }
        if (!options.logicalVisualFpsSpecified)
        {
            options.logicalVisualFps = 2;
        }
        if (!options.controlRepetitionsSpecified)
        {
            options.controlRepetitions = 12;
        }
        if (options.logicalVisualFps == 0 || options.logicalVisualFps > 5)
        {
            return false;
        }
    }
    if ((options.remoteChannel && options.remoteProvider.empty() && options.remoteMetadataPath.empty()) ||
        (!options.remoteChannel && !options.remoteMetadataPath.empty()))
    {
        return false;
    }
    output = std::move(options);
    return true;
}

[[nodiscard]] std::string WideToUtf8(const std::wstring_view value)
{
    if (value.empty() || value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    {
        throw std::length_error("remote provider is empty or too long");
    }
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0)
    {
        throw std::runtime_error("remote provider is not valid UTF-16");
    }
    std::string output(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        output.data(), count, nullptr, nullptr) != count)
    {
        throw std::runtime_error("remote provider conversion failed");
    }
    return output;
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

[[nodiscard]] std::uint64_t UnixNowMilliseconds() noexcept
{
    FILETIME fileTime{};
    GetSystemTimeAsFileTime(&fileTime);
    ULARGE_INTEGER ticks{};
    ticks.LowPart = fileTime.dwLowDateTime;
    ticks.HighPart = fileTime.dwHighDateTime;
    constexpr std::uint64_t windowsToUnixEpoch100ns = 116444736000000000ULL;
    return ticks.QuadPart < windowsToUnixEpoch100ns ? 0 :
        (ticks.QuadPart - windowsToUnixEpoch100ns) / 10000ULL;
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
    std::cerr << "usage: PixelBridgeEncoder --headless-broadcast --source PATH --profile direct|shape|remote "
                 "--channel local|remote [--remote-provider NAME] [--remote-metadata PATH] --compression off|on --origin X Y --seconds 1..600 [--logical-fps 0..240; remote=1..5] "
                 "[--control-repetitions 1..64] [--run-id 32_LOWERCASE_HEX] "
                 "[--compression-level 1..22] [--journal NEW_PATH] [--report NEW_PATH]\n";
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
    config.logicalVisualFps = options.logicalVisualFps;
    config.controlRepetitions = options.controlRepetitions;
    config.runId = options.runId;
    if (options.remoteChannel)
    {
        config.remoteMetadata.channelType = pbapp::ChannelType::RemoteVisual;
        try
        {
            if (!options.remoteMetadataPath.empty())
            {
                QString errorMessage;
                if (!pbapp::LoadRemoteVisualMetadataPreset(QString::fromStdWString(options.remoteMetadataPath),
                    config.remoteMetadata, errorMessage))
                {
                    std::cerr << "RemoteVisual metadata preset failed: " << errorMessage.toStdString() << '\n';
                    return 2;
                }
                if (!config.runId.empty() && config.runId != config.remoteMetadata.runId)
                {
                    std::cerr << "RunId conflicts with RemoteVisual metadata preset\n";
                    return 2;
                }
                config.runId = config.remoteMetadata.runId;
            }
            if (!options.remoteProvider.empty())
            {
                const std::string provider = WideToUtf8(options.remoteProvider);
                if (!config.remoteMetadata.remoteProvider.empty() && config.remoteMetadata.remoteProvider != provider)
                {
                    std::cerr << "RemoteVisual provider conflicts with metadata preset\n";
                    return 2;
                }
                config.remoteMetadata.remoteProvider = provider;
            }
            config.remoteMetadata.runId = config.runId;
        }
        catch (const std::exception& exception)
        {
            std::cerr << "remote provider conversion failed: " << exception.what() << '\n';
            return 2;
        }
    }
    pbapp::EncoderRuntime runtime;
    const pbapp::RuntimeStatus started = runtime.Start(config);
    if (!started)
    {
        std::cerr << started.message << '\n';
        return 2;
    }
    const auto commandStarted = std::chrono::steady_clock::now();
    std::unique_ptr<pbapp::RunEvidenceJournal> journal;
    pbapp::RunJournalSnapshot journalSnapshot;
    bool journalCreateAttempted = false;
    std::optional<std::uint64_t> lastJournalAttemptMilliseconds;
    std::optional<std::chrono::steady_clock::time_point> broadcastStarted;
    const auto overallDeadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(options.seconds) + std::chrono::seconds(30);
    bool requestedStopAfterDuration = false;
    bool overallTimeout = false;
    for (;;)
    {
        const pbapp::EncoderSnapshot snapshot = runtime.GetSnapshot();
        const auto elapsedCount = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - commandStarted).count();
        const std::uint64_t elapsedMilliseconds = elapsedCount < 0 ? 0 : static_cast<std::uint64_t>(elapsedCount);
        if (!options.journalPath.empty() && !journalCreateAttempted && snapshot.runId != "pending")
        {
            journalCreateAttempted = true;
            journalSnapshot = pbapp::RunEvidenceJournal::Create(std::filesystem::path(options.journalPath), {}, journal);
        }
        if (journal && (!lastJournalAttemptMilliseconds ||
            elapsedMilliseconds - *lastJournalAttemptMilliseconds >= 1000))
        {
            lastJournalAttemptMilliseconds = elapsedMilliseconds;
            journalSnapshot = journal->AppendSample(elapsedMilliseconds,
                pbapp::BuildEncoderJournalRecord(UnixNowMilliseconds(), snapshot));
        }
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
    pbapp::EncoderSnapshot snapshot = runtime.GetSnapshot();
    const auto finalElapsedCount = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - commandStarted).count();
    const std::uint64_t finalElapsedMilliseconds = finalElapsedCount < 0 ? 0 :
        static_cast<std::uint64_t>(finalElapsedCount);
    if (!options.journalPath.empty() && !journalCreateAttempted)
    {
        journalSnapshot = pbapp::RunEvidenceJournal::Create(std::filesystem::path(options.journalPath), {}, journal);
    }
    if (journal)
    {
        // Finish() returns the journal's authoritative snapshot, so it already carries any
        // invalidation or truncation recorded by the terminal append.
        static_cast<void>(journal->AppendTerminal(finalElapsedMilliseconds,
            pbapp::BuildEncoderJournalRecord(UnixNowMilliseconds(), snapshot)));
        journalSnapshot = journal->Finish();
    }
    if (!options.journalPath.empty())
    {
        pbapp::ApplyJournalSnapshot(journalSnapshot, snapshot);
    }
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
