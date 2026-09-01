#include "local_desktop_runtime.h"
#include "run_report.h"
#include "diagnostic_file.h"
#include "evidence_journal.h"
#include "remote_visual_metadata_preset_qt.h"

#include "pbcore/build_info.h"
#include "pbmodulation/desktop_levels.h"
#include "pbmodulation/remote_visual_low_fps.h"
#include "pbmodulation/shape_chroma.h"

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
    std::wstring outputDirectory;
    std::wstring reportPath;
    std::wstring journalPath;
    std::wstring replayOutputPath;
    std::wstring replayInputPath;
    std::string runId;
    pbapp::CaptureBackend backend = pbapp::CaptureBackend::Wgc;
    pbapp::VisualProfile profile = pbapp::VisualProfile::DirectLevels2x2;
    RECT roi{};
    std::uint32_t timeoutSeconds = 120;
    std::uint32_t replayMaximumFrames = 256;
    std::uint32_t replayMaximumMebibytes = 2048;
    std::uint32_t replayMaximumFramesPerSecond = 0;
    std::optional<std::uint64_t> replayEvidenceVisualProfileId;
    bool hasRoi = false;
    bool remoteChannel = false;
    bool channelSpecified = false;
    bool offlineReplay = false;
    bool diagnosticCaptureOnly = false;
    std::wstring remoteProvider;
    std::wstring remoteMetadataPath;
    std::wstring protectedMonitorDeviceName;
    std::wstring experimentMonitorDeviceName;
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
    options.offlineReplay = argumentCount > 1 && std::wstring_view(arguments[1]) == L"--headless-replay";
    if (options.offlineReplay)
    {
        options.profile = pbapp::VisualProfile::RemoteVisualResilient;
        options.remoteChannel = true;
    }
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
        else if (option == L"--replay-output")
        {
            const wchar_t* const value = nextArgument();
            if (value == nullptr)
            {
                return false;
            }
            options.replayOutputPath = value;
        }
        else if (option == L"--replay-input")
        {
            const wchar_t* const value = nextArgument();
            if (value == nullptr)
            {
                return false;
            }
            options.replayInputPath = value;
        }
        else if (option == L"--diagnostic-capture-only")
        {
            options.diagnosticCaptureOnly = true;
        }
        else if (option == L"--replay-evidence-profile")
        {
            const wchar_t* const value = nextArgument();
            if (value == nullptr)
            {
                return false;
            }
            const std::wstring_view profile(value);
            if (profile == L"direct")
            {
                options.replayEvidenceVisualProfileId = pbmodulation::kDesktopLevels2ProfileId;
            }
            else if (profile == L"shape")
            {
                options.replayEvidenceVisualProfileId = pbmodulation::kShapeChromaProfileId;
            }
            else if (profile == L"lf4")
            {
                options.replayEvidenceVisualProfileId = pbmodulation::kRemoteVisualLowFpsProfileId;
            }
            else
            {
                return false;
            }
        }
        else if (option == L"--replay-frames")
        {
            const wchar_t* const value = nextArgument();
            if (value == nullptr || !ParseUnsigned(value, options.replayMaximumFrames) ||
                options.replayMaximumFrames == 0 || options.replayMaximumFrames > 2048)
            {
                return false;
            }
        }
        else if (option == L"--replay-max-mib")
        {
            const wchar_t* const value = nextArgument();
            if (value == nullptr || !ParseUnsigned(value, options.replayMaximumMebibytes) ||
                options.replayMaximumMebibytes < 16 || options.replayMaximumMebibytes > 16384)
            {
                return false;
            }
        }
        else if (option == L"--replay-sample-fps")
        {
            const wchar_t* const value = nextArgument();
            if (value == nullptr || !ParseUnsigned(value, options.replayMaximumFramesPerSecond) ||
                options.replayMaximumFramesPerSecond == 0 || options.replayMaximumFramesPerSecond > 60)
            {
                return false;
            }
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
        else if (option == L"--protected-monitor")
        {
            const wchar_t* const value = nextArgument();
            if (value == nullptr)
            {
                return false;
            }
            options.protectedMonitorDeviceName = value;
        }
        else if (option == L"--experiment-monitor")
        {
            const wchar_t* const value = nextArgument();
            if (value == nullptr)
            {
                return false;
            }
            options.experimentMonitorDeviceName = value;
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
    if (!options.channelSpecified && options.profile == pbapp::VisualProfile::RemoteVisualResilient)
    {
        options.remoteChannel = true;
    }
    if (options.offlineReplay)
    {
        if (options.outputDirectory.empty() || options.replayInputPath.empty() ||
            !options.replayOutputPath.empty() || options.hasRoi || !options.remoteChannel ||
            options.profile != pbapp::VisualProfile::RemoteVisualResilient ||
            (options.remoteProvider.empty() && options.remoteMetadataPath.empty()) ||
            !options.protectedMonitorDeviceName.empty() || !options.experimentMonitorDeviceName.empty() ||
            options.diagnosticCaptureOnly || options.replayEvidenceVisualProfileId || options.replayMaximumFramesPerSecond != 0)
        {
            return false;
        }
        output = std::move(options);
        return true;
    }
    if (options.outputDirectory.empty() || !options.replayInputPath.empty() || !options.hasRoi ||
        (options.remoteChannel && ((options.remoteProvider.empty() && options.remoteMetadataPath.empty()) ||
            options.protectedMonitorDeviceName.empty() ||
            options.experimentMonitorDeviceName.empty())) ||
        (!options.remoteChannel && !options.remoteMetadataPath.empty()) ||
        (!options.replayOutputPath.empty() && (!options.remoteChannel ||
            options.profile != pbapp::VisualProfile::RemoteVisualResilient)) ||
        (options.diagnosticCaptureOnly && (options.replayOutputPath.empty() || !options.remoteChannel ||
            options.profile != pbapp::VisualProfile::RemoteVisualResilient)) ||
        (options.replayEvidenceVisualProfileId && !options.diagnosticCaptureOnly) ||
        (options.replayMaximumFramesPerSecond != 0 && !options.diagnosticCaptureOnly))
    {
        return false;
    }
    output = std::move(options);
    return true;
}

[[nodiscard]] std::string WideToUtf8(const std::wstring_view value)
{
    if (value.empty())
    {
        return {};
    }
    if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    {
        throw std::length_error("monitor identity is too long");
    }
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0)
    {
        throw std::runtime_error("monitor identity is not valid UTF-16");
    }
    std::string output(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        output.data(), count, nullptr, nullptr) != count)
    {
        throw std::runtime_error("monitor identity conversion failed");
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
    std::cerr << "usage: PixelBridgeDecoder --headless-receive --output-dir DIR --backend wgc|dxgi "
                 "--profile direct|shape|remote --channel local|remote [--remote-provider NAME] [--remote-metadata PATH] --roi LEFT TOP RIGHT BOTTOM --timeout 1..600 "
                 "[--protected-monitor DEVICE --experiment-monitor DEVICE] "
                 "[--replay-output NEW_PATH --diagnostic-capture-only --replay-evidence-profile direct|shape|lf4 "
                 "--replay-frames 1..2048 --replay-max-mib 16..16384 --replay-sample-fps 1..60] "
                 "[--run-id 32_LOWERCASE_HEX] [--journal NEW_PATH] [--report NEW_PATH]\n";
    std::cerr << "       PixelBridgeDecoder --headless-replay --replay-input PATH --output-dir DIR "
                 "[--remote-provider NAME] [--remote-metadata PATH] [--profile remote] [--timeout 1..600] "
                 "[--replay-frames 1..2048 --replay-max-mib 16..16384] "
                 "[--run-id MATCHING_32_LOWERCASE_HEX] [--journal NEW_PATH] [--report NEW_PATH]\n";
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
    if (!options.offlineReplay)
    {
        const auto regionStatus = pbscreenregion::ResolveScreenCaptureRegion(options.roi, region);
        if (!regionStatus)
        {
            std::cerr << "ROI resolution failed: " << pbscreenregion::GetScreenRegionErrorName(regionStatus.code) << '\n';
            return 2;
        }
    }
    pbapp::DecoderConfig config;
    config.outputDirectory = options.outputDirectory;
    config.captureBackend = options.backend;
    config.visualProfile = options.profile;
    config.runId = options.runId;
    config.region = region;
    config.replayOutputPath = options.replayOutputPath;
    config.replayInputPath = options.replayInputPath;
    config.diagnosticCaptureOnly = options.diagnosticCaptureOnly;
    config.replayEvidenceVisualProfileId = options.replayEvidenceVisualProfileId;
    config.replayMaximumCaptureFrames = options.replayMaximumFrames;
    config.replayMaximumFileBytes = static_cast<std::uint64_t>(options.replayMaximumMebibytes) * 1024ULL * 1024ULL;
    config.replayMaximumCaptureFramesPerSecond = options.replayMaximumFramesPerSecond;
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
    if (!options.offlineReplay)
    {
        const std::int64_t roiWidth = static_cast<std::int64_t>(region.physicalRect.right) -
            region.physicalRect.left;
        const std::int64_t roiHeight = static_cast<std::int64_t>(region.physicalRect.bottom) -
            region.physicalRect.top;
        const bool strictGeometry = roiWidth == pbapp::phase1CanvasWidth &&
            roiHeight == pbapp::phase1CanvasHeight && region.rotation == DXGI_MODE_ROTATION_IDENTITY;
        config.remoteMetadata.selectedRoiPhysicalRect = pbapp::MetadataPhysicalRect{region.physicalRect.left,
            region.physicalRect.top, region.physicalRect.right, region.physicalRect.bottom};
        config.remoteMetadata.estimatedScaleX = static_cast<double>(roiWidth) / pbapp::phase1CanvasWidth;
        config.remoteMetadata.estimatedScaleY = static_cast<double>(roiHeight) / pbapp::phase1CanvasHeight;
        config.remoteMetadata.letterboxStatus = "Unknown";
        config.remoteMetadata.cropStatus = "Unknown";
        config.remoteMetadata.geometryStatus = options.diagnosticCaptureOnly ?
            strictGeometry ? "DiagnosticCaptureOnlyStrictROI; no demod/decode/publish" :
                "DiagnosticOnlyIncompatibleROI; no resampling/decode/publish" :
            strictGeometry ? "CompatibleStrictPhysical1:1" : "IncompatiblePhysicalROI; no resampling permitted";
        config.remoteMetadata.geometryProvenance = pbapp::MetadataProvenance::PixelBridgeObserved;
    }
    if (options.remoteChannel)
    {
        config.remoteMetadata.channelType = pbapp::ChannelType::RemoteVisual;
        try
        {
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
        if (options.offlineReplay)
        {
            config.remoteMetadata.geometryStatus =
                "Sealed Replay v2 capture records require strict 1920x1080, 1:1, identity-rotation validation";
            config.remoteMetadata.geometryProvenance = pbapp::MetadataProvenance::PixelBridgeObserved;
        }
        else
        {
            pbapp::MonitorSafetySelection safetySelection;
            const pbapp::MonitorSafetyStatus safety = pbapp::ResolveMonitorSafetySelection(
                options.protectedMonitorDeviceName, options.experimentMonitorDeviceName, safetySelection);
            if (!safety)
            {
                std::cerr << "monitor safety selection failed: " << pbapp::GetMonitorSafetyErrorName(safety.code) << '\n';
                return 2;
            }
            const pbapp::MonitorSafetyStatus targetSafety = pbapp::ValidateMonitorSafetyTarget(safetySelection,
                region.physicalRect, region.monitor);
            if (!targetSafety)
            {
                std::cerr << "Geometry incompatible with current experimental profile: " <<
                    pbapp::GetMonitorSafetyErrorName(targetSafety.code) << '\n';
                return 2;
            }
            config.monitorSafety = safetySelection;
            try
            {
                config.remoteMetadata.protectedMonitorIdentity = WideToUtf8(safetySelection.protectedMonitor.deviceName);
                config.remoteMetadata.experimentMonitorIdentity = WideToUtf8(safetySelection.experimentMonitor.deviceName);
            }
            catch (const std::exception& exception)
            {
                std::cerr << "monitor identity conversion failed: " << exception.what() << '\n';
                return 2;
            }
            config.remoteMetadata.computerADisplayResolution = std::to_string(
                safetySelection.experimentMonitor.physicalRect.right - safetySelection.experimentMonitor.physicalRect.left) +
                "x" + std::to_string(safetySelection.experimentMonitor.physicalRect.bottom -
                    safetySelection.experimentMonitor.physicalRect.top);
            config.remoteMetadata.computerARefreshRate = safetySelection.experimentMonitor.refreshRate;
        }
    }
    pbapp::DecoderRuntime runtime;
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
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(options.timeoutSeconds);
    for (;;)
    {
        const pbapp::DecoderSnapshot snapshot = runtime.GetSnapshot();
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
                pbapp::BuildDecoderJournalRecord(UnixNowMilliseconds(), snapshot));
        }
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
    pbapp::DecoderSnapshot snapshot = runtime.GetSnapshot();
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
            pbapp::BuildDecoderJournalRecord(UnixNowMilliseconds(), snapshot)));
        journalSnapshot = journal->Finish();
    }
    if (!options.journalPath.empty())
    {
        pbapp::ApplyJournalSnapshot(journalSnapshot, snapshot);
    }
    const pbcore::BuildInfo buildInfo = pbcore::GetBuildInfo();
    const pbapp::RunReportContext context{"PixelBridgeDecoder", buildInfo.version, PB_GIT_COMMIT, UtcNow()};
    const std::string report = pbapp::BuildDecoderRunReportJson(context, snapshot);
    std::cout << report << '\n';
    if (!WriteNewReport(options.reportPath, report))
    {
        std::cerr << "failed to create report path\n";
        return 2;
    }
    if (options.diagnosticCaptureOnly)
    {
        return snapshot.state == pbapp::DecoderState::Stopped && snapshot.replayCaptureOnly &&
            snapshot.replayFinalized && snapshot.replayEvidenceValid && snapshot.replayWrittenFrames != 0 ? 0 : 1;
    }
    return snapshot.state == pbapp::DecoderState::Completed && snapshot.wholeFileDigestVerified &&
        snapshot.finalPublishSucceeded ? 0 : 1;
}
