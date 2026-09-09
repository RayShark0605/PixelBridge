#include "held_presentation.h"
#include "run_contract.h"
#include "encoder_monitor_catalog.h"
#include "run_measurement.h"
#include "run_report.h"
#include "support.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <sstream>
#include <thread>

namespace
{
using pbstep3b::Require;

class NativePresentation final : public pbapp::EncoderPresentation
{
public:
    explicit NativePresentation(const pbrenderd3d::DataWindowConfig& config)
    {
        auto result = pbrenderd3d::DataWindow::Create(config);
        Require(static_cast<bool>(result), "Native DataWindow create failed");
        window_ = std::move(result).Value();
    }
    pbrenderd3d::DataWindowSnapshot GetSnapshot() const override
    {
        return window_->GetSnapshot();
    }
    pbrenderd3d::PresentationStatus SubmitFrame(const pbrenderd3d::CanonicalBgraFrameView& frame) override
    {
        return window_->SubmitFrame(frame);
    }
    void RequestStop() noexcept override
    {
        window_->RequestStop();
    }
    void Stop() noexcept override
    {
        window_->Stop();
    }
private:
    std::unique_ptr<pbrenderd3d::DataWindow> window_;
};

void WriteNewEvidence(const std::filesystem::path& path, const std::string& text)
{
    pbstep3b::RequireLocal(path);
    Require(text.size() <= pbexperiment::maximumEvidenceFileBytes, "Evidence file exceeds 4 MiB cap");
    const pbstep3b::Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    pbstep3b::WriteExact(file.Get(), std::as_bytes(std::span(text)));
    Require(FlushFileBuffers(file.Get()), "Evidence flush failed");
}

std::vector<pbapp::MonitorInfo> ReadMonitors()
{
    Require(SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) ||
        AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2), "Per-monitor V2 required");
    std::vector<pbapp::MonitorInfo> monitors;
    Require(static_cast<bool>(pbapp::EnumerateEncoderMonitors(monitors)), "Encoder monitor inventory failed");
    return monitors;
}

std::string BuildMonitorJson(const std::vector<pbapp::MonitorInfo>& monitors)
{
    std::ostringstream output;
    output << "{\"schema\":\"PixelBridge.ExperimentalEncoderMonitors.1\",\"captureAuthority\":false,\"monitors\":[";
    for (std::size_t index = 0; index < monitors.size(); index++)
    {
        const auto& monitor = monitors[index];
        // DISPLAY device names are Win32 ASCII identifiers, not file names.
        Require(std::all_of(monitor.deviceName.begin(), monitor.deviceName.end(), [](const wchar_t value) { return value > 0 && value < 128; }), "Non-ASCII device identifier");
        std::string name;
        name.reserve(monitor.deviceName.size());
        for (const auto character : monitor.deviceName)
        {
            name += static_cast<char>(character);
        }
        output << (index == 0 ? "" : ",") << "{\"device\":" << pbstep3b::JsonString(name)
            << ",\"left\":" << monitor.physicalRect.left << ",\"top\":" << monitor.physicalRect.top
            << ",\"right\":" << monitor.physicalRect.right << ",\"bottom\":" << monitor.physicalRect.bottom
            << ",\"dpiX\":" << monitor.dpiX << ",\"dpiY\":" << monitor.dpiY
            << ",\"refreshHz\":" << monitor.refreshRate << ",\"primary\":" << (monitor.primary ? "true" : "false") << '}';
    }
    output << "]}\n";
    return output.str();
}

void RequireBoundedJob()
{
    BOOL inJob = FALSE;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    Require(IsProcessInJob(GetCurrentProcess(), nullptr, &inJob) && inJob &&
        QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &limits, sizeof(limits), nullptr), "Run through the bounded process runner, not direct launch");
    constexpr DWORD required = JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_JOB_MEMORY | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    Require((limits.BasicLimitInformation.LimitFlags & required) == required && limits.ProcessMemoryLimit != 0 &&
        limits.JobMemoryLimit != 0 && limits.ProcessMemoryLimit <= 2147483648ULL && limits.JobMemoryLimit <= 2147483648ULL,
        "A 2 GiB-or-smaller process/job and kill-on-close limits are required");
}

int Run(const std::optional<pbexperiment::Treatment> treatment, const std::filesystem::path& source,
    const std::filesystem::path& root, const std::wstring_view device, const std::uint32_t seconds, const bool comparison)
{
    Require(seconds >= 5 && seconds <= (comparison ? pbexperiment::maximumComparisonSeconds : pbexperiment::maximumShortRunSeconds), "Explicit run budget exceeded");
    pbstep3b::RequireLocal(source);
    pbstep3b::RequireLocal(root);
    Require(root.wstring().size() <= 96 && !std::filesystem::exists(root) && std::filesystem::is_directory(root.parent_path()),
        "New short root (<=96 characters) with an existing parent required");
    Require(std::filesystem::is_regular_file(source) && std::filesystem::file_size(source) <= pbapp::step1MaximumSourceBytes, "Initial experimental source cap is 64 MiB");
    RequireBoundedJob();
    const auto monitors = ReadMonitors();
    const auto selected = std::find_if(monitors.begin(), monitors.end(), [device](const pbapp::MonitorInfo& monitor) { return monitor.deviceName == device; });
    Require(selected != monitors.end(), "Explicit monitor device not found; no automatic selection");
    const auto width = static_cast<std::int64_t>(selected->physicalRect.right) - selected->physicalRect.left;
    const auto height = static_cast<std::int64_t>(selected->physicalRect.bottom) - selected->physicalRect.top;
    Require(width >= 1920 && height >= 1080 && width <= 2560 && height <= 1440 && selected->rotation == DXGI_MODE_ROTATION_IDENTITY,
        "Initial native treatment canvas must be landscape 1920x1080..2560x1440");
    Require(std::filesystem::create_directory(root), "Evidence root create-only failed");
    WriteNewEvidence(root / "monitors-before.json", BuildMonitorJson(monitors));
    WriteNewEvidence(root / "experiment.json", std::string("{\"schema\":\"PixelBridge.ExperimentalSenderRun.2\",\"treatment\":\"") +
        (!treatment ? "OriginalNative" : (*treatment == pbexperiment::Treatment::NeutralChroma ? "NeutralChroma" : "ColorControl")) +
        "\",\"productDefaultChanged\":false,\"certifiedProfile\":false,\"originalLogicalClockHz\":15,\"maximumSeconds\":" + std::to_string(seconds) +
        ",\"comparisonBudgetOptIn\":" + (comparison ? "true" : "false") + ",\"heldWrapper\":" + (treatment ? "true" : "false") +
        ",\"originalMeansCurrentFrozenRuntimeNotHistoricalA\":true" +
        ",\"exactOfflineH2TimingEquivalent\":false,\"receiverCompletionInferred\":false,\"readOnlySourceAuditIsNotPayloadTransport\":true}");
    const pbstep3b::Handle sourceLease(CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    std::string sourceBefore;
    const auto before = pbapp::AuditUnifiedSource(source.wstring(), sourceBefore);
    Require(static_cast<bool>(before), before.message);
    WriteNewEvidence(root / "source-before.json", sourceBefore);
    const auto evidence = treatment ? std::make_shared<pbexperiment::PresentationEvidence>() : nullptr;
    const auto measurement = std::make_shared<pbapp::RunMeasurementRecorder>();
    pbapp::EncoderPresentationFactory presentationFactory;
    if (treatment)
    {
        presentationFactory = [selectedTreatment = *treatment, evidence](const pbrenderd3d::DataWindowConfig& config)
        {
            Require(config.width <= 2560 && config.height <= 1440, "Window dimensions exceed experimental allocation envelope");
            return std::make_unique<pbexperiment::HeldPresentation>(config, selectedTreatment,
                std::make_unique<NativePresentation>(config), evidence, pbapp::MeasurementNowNanoseconds);
        };
    }
    // An empty factory uses the original runtime's native presentation directly.
    pbapp::EncoderRuntime encoder(std::move(presentationFactory));
    auto config = pbapp::MakeUnifiedEncoderConfig(source.wstring(), 15,
        pbrenderd3d::PhysicalPoint{selected->physicalRect.left, selected->physicalRect.top});
    config.singleMonitorFullscreen = *selected;
    for (const auto character : selected->deviceName)
    {
        config.remoteMetadata.experimentMonitorIdentity += static_cast<char>(character);
    }
    config.sessionStateRoot = root / "state";
    config.measurement = measurement;
    config.diagnostics = std::make_shared<pbcore::StageDiagnostics>();
    std::ostringstream submitted;
    std::size_t submittedBytes = 0;
    std::size_t submittedRecords = 0;
    const auto drain = [&]()
    {
        pbapp::SubmittedFrameIdentity identity;
        while (measurement->TakeSubmitted(identity))
        {
            const auto row = pbapp::BuildSubmittedFrameJson(identity) + '\n';
            Require(row.size() <= pbexperiment::maximumSubmittedEvidenceRowBytes && submittedRecords < pbexperiment::maximumSubmittedEvidenceRecords &&
                submittedBytes <= pbexperiment::maximumEvidenceFileBytes - row.size(), "Submitted identity evidence cap");
            submitted << row;
            submittedBytes += row.size();
            submittedRecords++;
        }
    };
    const auto start = encoder.Start(config);
    Require(static_cast<bool>(start), start.message);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline)
    {
        drain();
        const auto state = encoder.GetSnapshot().state;
        if (state == pbapp::EncoderState::Failed || state == pbapp::EncoderState::Stopped)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    encoder.Stop();
    drain();
    const auto snapshot = encoder.GetSnapshot();
    WriteNewEvidence(root / "sender-report.json", pbapp::BuildEncoderRunReportJson(
        {"PBExperimentalVisualSender", "tool-only-bounded-comparison-not-remote-certification", "see-runtime-manifest", "not-recorded"}, snapshot));
    WriteNewEvidence(root / "submitted.jsonl", submitted.str());
    if (evidence)
    {
        std::ostringstream presentation;
        pbexperiment::WritePresentationEvidenceJson(presentation, *evidence);
        WriteNewEvidence(root / "presentation.json", presentation.str());
    }
    else
    {
        WriteNewEvidence(root / "presentation.json", "{\"schema\":\"PixelBridge.OriginalNativePresentation.1\",\"heldWrapper\":false,\"records\":null,\"nativePresentIsReceiverAck\":false}");
    }
    std::string sourceAfter;
    const auto after = pbapp::AuditUnifiedSource(source.wstring(), sourceAfter);
    Require(static_cast<bool>(after), after.message);
    WriteNewEvidence(root / "source-after.json", sourceAfter);
    WriteNewEvidence(root / "monitors-after.json", BuildMonitorJson(ReadMonitors()));
    Require(sourceBefore == sourceAfter, "Source ledger changed");
    Require(snapshot.state == pbapp::EncoderState::Stopped && snapshot.errorDetail.empty() && snapshot.sourceStable && snapshot.successfulPresentCalls > 0 &&
        (!evidence || (evidence->failure && evidence->observedFrames > 0)) && measurement->GetSnapshot().failure == pbapp::MeasurementFailure::None,
        "Experimental Sender failed or did not show an observed local Present; keep all evidence: " + snapshot.errorDetail);
    std::cout << "{\"status\":\"LOCAL_SENDER_STOPPED\",\"remoteThroughputGainEstablished\":false}\n";
    return 0;
}
} // namespace

int wmain(const int argc, wchar_t** argv)
{
    try
    {
        if (argc == 2 && std::wstring_view(argv[1]) == L"--list-monitors")
        {
            std::cout << BuildMonitorJson(ReadMonitors());
            return 0;
        }
        if (argc == 7 && (std::wstring_view(argv[1]) == L"--run" || std::wstring_view(argv[1]) == L"--comparison-run"))
        {
            const bool comparison = std::wstring_view(argv[1]) == L"--comparison-run";
            const std::wstring_view mode = argv[2];
            Require(mode == L"neutral" || mode == (comparison ? L"original" : L"color"), "Explicit original|neutral comparison or color|neutral short run required");
            const auto seconds = pbexperiment::ParseRunSeconds(argv[6], comparison);
            const auto treatment = mode == L"original" ? std::nullopt : std::optional{
                mode == L"neutral" ? pbexperiment::Treatment::NeutralChroma : pbexperiment::Treatment::ColorControl};
            return Run(treatment, argv[3], argv[4], argv[5], seconds, comparison);
        }
        throw std::invalid_argument("Use --list-monitors | --run color|neutral SOURCE NEW_SHORT_ROOT DEVICE SECONDS | --comparison-run original|neutral SOURCE NEW_SHORT_ROOT DEVICE SECONDS; no default action");
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
