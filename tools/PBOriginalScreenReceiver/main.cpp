#include "encoder_monitor_catalog.h"
#include "local_desktop_runtime.h"
#include "run_measurement.h"
#include "run_report.h"
#include "support.h"
#include "../PBExperimentalVisualSender/run_contract.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <sstream>
#include <thread>

namespace
{
using pbstep3b::Require;

pbscreenregion::ScreenCaptureRegion ResolveExplicitMonitor(const std::wstring_view device)
{
    Require(SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) ||
        AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2), "Per-monitor V2 required");
    std::vector<pbapp::MonitorInfo> monitors;
    Require(static_cast<bool>(pbapp::EnumerateEncoderMonitors(monitors)), "Read-only monitor inventory failed");
    const auto selected = std::find_if(monitors.begin(), monitors.end(), [device](const pbapp::MonitorInfo& monitor)
    {
        return monitor.deviceName == device;
    });
    Require(selected != monitors.end(), "Explicit device not found; no automatic monitor selection");
    const auto width = static_cast<std::int64_t>(selected->physicalRect.right) - selected->physicalRect.left;
    const auto height = static_cast<std::int64_t>(selected->physicalRect.bottom) - selected->physicalRect.top;
    Require(width >= 1920 && width <= 2560 && height >= 1080 && height <= 1440 &&
        selected->rotation == DXGI_MODE_ROTATION_IDENTITY, "Initial local integration monitor envelope exceeded");
    pbscreenregion::ScreenCaptureRegion region;
    const auto resolved = pbscreenregion::ResolveScreenCaptureRegion(selected->physicalRect, region);
    Require(static_cast<bool>(resolved), pbscreenregion::GetScreenRegionErrorName(resolved.code));
    Require(EqualRect(&region.physicalRect, &selected->physicalRect) && EqualRect(&region.monitorPhysicalRect, &selected->physicalRect) &&
        region.dpiX == selected->dpiX && region.dpiY == selected->dpiY && region.rotation == selected->rotation, "Topology changed while resolving explicit monitor");
    return region;
}

std::string RegionJson(const pbscreenregion::ScreenCaptureRegion& region)
{
    std::ostringstream stream;
    pbscreenregion::WriteScreenCaptureRegionJson(stream, region);
    return stream.str();
}

void RequireBoundedJob()
{
    BOOL inJob = FALSE;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    Require(IsProcessInJob(GetCurrentProcess(), nullptr, &inJob) && inJob &&
        QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &limits, sizeof(limits), nullptr), "A bounded process runner Job is required");
    constexpr DWORD required = JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_JOB_MEMORY | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    Require((limits.BasicLimitInformation.LimitFlags & required) == required && limits.ProcessMemoryLimit != 0 &&
        limits.JobMemoryLimit != 0 && limits.ProcessMemoryLimit <= 2147483648ULL && limits.JobMemoryLimit <= 2147483648ULL,
        "A 2 GiB-or-smaller process/Job and kill-on-close limits are required");
}

int Run(const std::filesystem::path& root, const std::wstring_view device, const std::uint32_t seconds, const bool comparison)
{
    pbstep3b::RequireLocal(root);
    Require(root.wstring().size() <= 96 && !std::filesystem::exists(root) && std::filesystem::is_directory(root.parent_path()), "New short root (<=96 characters) with existing parent required");
    RequireBoundedJob();
    const auto region = ResolveExplicitMonitor(device);
    Require(std::filesystem::create_directory(root), "Evidence root create-only failed");
    const auto output = root / "output";
    Require(std::filesystem::create_directory(output), "New receiver output directory required");
    pbstep3b::WriteNewText(root / "region-before.json", RegionJson(region));
    pbstep3b::WriteNewText(root / "experiment.json", "{\"schema\":\"PixelBridge.OriginalScreenReceiver.2\",\"remoteCertificationInferred\":false,\"comparisonBudgetOptIn\":" + std::string(comparison ? "true" : "false") + ",\"sourceArgument\":false,\"replayEnabled\":false,\"injectedRuntimeServices\":false,\"receiverCompletionFeedback\":false,\"maximumSeconds\":" + std::to_string(seconds) + "}");
    auto config = pbapp::MakeUnifiedDecoderConfig(output.wstring(), region);
    config.measurement = std::make_shared<pbapp::RunMeasurementRecorder>();
    config.diagnostics = std::make_shared<pbcore::StageDiagnostics>();
    pbapp::DecoderRuntime receiver;
    const auto start = receiver.Start(config);
    pbstep3b::WriteNewText(root / "start.json", "{\"accepted\":" + std::string(start ? "true" : "false") + ",\"message\":" + pbstep3b::JsonString(start.message) + "}");
    if (!start)
    {
        receiver.Stop();
        pbstep3b::WriteNewText(root / "receiver-report.json", pbapp::BuildDecoderRunReportJson({"PBOriginalScreenReceiver", "local-integration-only", "see-runtime-manifest", "not-recorded"}, receiver.GetSnapshot()));
        Require(false, start.message);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    bool timedOut = true;
    bool confirmationRequired = false;
    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto snapshot = receiver.GetSnapshot();
        if (snapshot.state == pbapp::DecoderState::Completed || snapshot.state == pbapp::DecoderState::Failed || snapshot.state == pbapp::DecoderState::Stopped)
        {
            timedOut = false;
            break;
        }
        if (snapshot.state == pbapp::DecoderState::AwaitingLargeOutputConfirmation)
        {
            confirmationRequired = true;
            timedOut = false;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    // Stop joins the original worker; the outer Job timeout bounds a cleanup failure.
    receiver.Stop();
    const auto snapshot = receiver.GetSnapshot();
    pbstep3b::WriteNewText(root / "receiver-report.json", pbapp::BuildDecoderRunReportJson({"PBOriginalScreenReceiver", "local-integration-only", "see-runtime-manifest", "not-recorded"}, snapshot));
    const auto regionAfter = ResolveExplicitMonitor(device);
    pbstep3b::WriteNewText(root / "region-after.json", RegionJson(regionAfter));
    const bool topologyUnchanged = region.monitor == regionAfter.monitor && EqualRect(&region.physicalRect, &regionAfter.physicalRect) &&
        region.dpiX == regionAfter.dpiX && region.dpiY == regionAfter.dpiY && region.rotation == regionAfter.rotation;
    const bool verified = snapshot.state == pbapp::DecoderState::Completed && snapshot.wholeFileDigestVerified && snapshot.finalPublishSucceeded &&
        snapshot.wholeFileDigestCheck.value_or(false) && snapshot.finalRenameSucceeded.value_or(false) && snapshot.finalReopenVerified.value_or(false);
    const bool measurementValid = snapshot.measurement && snapshot.measurement->failure == pbapp::MeasurementFailure::None;
    pbstep3b::WriteNewText(root / "outcome.json", "{\"verifiedWholeFile\":" + std::string(verified ? "true" : "false") +
        ",\"measurementValid\":" + (measurementValid ? "true" : "false") + ",\"topologyUnchanged\":" + (topologyUnchanged ? "true" : "false") +
        ",\"timeout\":" + (timedOut ? "true" : "false") + ",\"largeOutputConfirmationRequired\":" + (confirmationRequired ? "true" : "false") + "}");
    Require(!timedOut && !confirmationRequired && topologyUnchanged && verified && measurementValid, "Original screen receiver did not pass bounded complete-file integration; inspect retained report");
    std::cout << "PASS: local actual-screen capture, original Receiver digest/publish/reopen. Not remote certification.\n";
    return 0;
}
}

int wmain(const int argc, wchar_t** argv)
{
    try
    {
        if (argc == 3 && std::wstring_view(argv[1]) == L"--preflight")
        {
            std::cout << RegionJson(ResolveExplicitMonitor(argv[2])) << '\n';
            return 0;
        }
        Require(argc == 5 && (std::wstring_view(argv[1]) == L"--run" || std::wstring_view(argv[1]) == L"--comparison-run"), "Usage: --preflight DEVICE | --run NEW_SHORT_ROOT DEVICE SECONDS | --comparison-run NEW_SHORT_ROOT DEVICE SECONDS; no payload input argument");
        const bool comparison = std::wstring_view(argv[1]) == L"--comparison-run";
        const auto seconds = pbexperiment::ParseRunSeconds(argv[4], comparison);
        return Run(std::filesystem::path(argv[2]), argv[3], seconds, comparison);
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
