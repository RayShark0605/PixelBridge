#include "local_desktop_runtime.h"
#include "run_report.h"
#include "unified_decoder_test_support.h"
#include "pbprotocol/product_visual_profile.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <QJsonDocument>
#include <QJsonObject>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <limits>

namespace
{

class Scratch final
{
public:
    Scratch() : root_(std::filesystem::absolute(PB_TEST_SCRATCH_ROOT).lexically_normal())
    {
        static std::atomic<std::uint64_t> nextOrdinal = 0;
        path_ = root_ / (L"single-capture-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(nextOrdinal.fetch_add(1)));
        REQUIRE(path_.parent_path() == root_);
        REQUIRE(std::filesystem::create_directory(path_));
        for (const auto* name : {L"output", L"probe-output", L"state"})
        {
            REQUIRE(std::filesystem::create_directory(path_ / name));
        }
    }
    ~Scratch()
    {
        if (path_.parent_path() == root_ && path_.filename().wstring().starts_with(L"single-capture-"))
        {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }
    }
    [[nodiscard]] const std::filesystem::path& Path() const noexcept
    {
        return path_;
    }
private:
    const std::filesystem::path root_;
    std::filesystem::path path_;
};

pbapp::MonitorInfo Target(const LONG width = 1920, const LONG height = 1080)
{
    pbapp::MonitorInfo monitor;
    monitor.monitor = reinterpret_cast<HMONITOR>(std::uintptr_t{1});
    monitor.deviceName = L"PB-TEST-SINGLE-NOT-A-REAL-MONITOR";
    monitor.physicalRect = {0, 0, width, height};
    monitor.workRect = monitor.physicalRect;
    monitor.dpiX = 96;
    monitor.dpiY = 96;
    monitor.refreshRate = 60;
    monitor.rotation = DXGI_MODE_ROTATION_IDENTITY;
    monitor.adapterLuid = {1, 0};
    monitor.primary = true;
    return monitor;
}

pbapp::DecoderConfig Configuration(const Scratch& scratch, const pbapp::VisualProfile profile,
    const pbapp::MonitorInfo& monitor = Target())
{
    pbapp::DecoderConfig config;
    config.outputDirectory = (scratch.Path() / L"output").wstring();
    config.visualProfile = profile;
    config.singleMonitorCapture = monitor;
    config.region = {monitor.monitor, monitor.physicalRect, monitor.physicalRect, monitor.dpiX, monitor.dpiY, monitor.rotation};
    config.remoteMetadata.channelType = pbapp::ChannelType::RemoteVisual;
    config.remoteMetadata.remoteProvider = "OfflineSingleMonitorValidationOnly";
    config.remoteMetadata.experimentMonitorIdentity = "PB-TEST-SINGLE-NOT-A-REAL-MONITOR";
    return config;
}

struct CatalogState
{
    explicit CatalogState(const pbapp::MonitorInfo& monitor) : monitors{monitor}
    {
    }
    std::mutex mutex;
    std::vector<pbapp::MonitorInfo> monitors;
    pbapp::MonitorCatalogStatus status;
    bool throwOnEnumeration = false;

    void Change(const int mutation)
    {
        const std::scoped_lock lock(mutex);
        switch (mutation)
        {
        case 0: monitors.clear(); break;
        case 1: monitors[0].monitor = reinterpret_cast<HMONITOR>(std::uintptr_t{2}); break;
        case 2: monitors[0].deviceName += L"-changed"; break;
        case 3: monitors[0].physicalRect.right--; break;
        case 4: monitors[0].dpiX++; break;
        case 5: monitors[0].dpiY++; break;
        case 6: monitors[0].rotation = DXGI_MODE_ROTATION_ROTATE90; break;
        case 7: monitors[0].refreshRate++; break;
        case 8: monitors[0].adapterLuid.LowPart++; break;
        case 9: monitors[0].adapterLuid.HighPart++; break;
        case 10: monitors[0].primary = !monitors[0].primary; break;
        case 11: monitors.push_back(monitors[0]); break;
        case 12: status = {pbapp::MonitorCatalogError::MetadataUnavailable, 0}; break;
        case 13: throwOnEnumeration = true; break;
        case 14: monitors[0].dxgiOutputIdentityAvailable = false; break;
        default: throw std::logic_error("unknown monitor mutation");
        }
    }
};

pbapp::DecoderRuntimeServices Services(const std::shared_ptr<g16test::ReceiveState>& receive,
    const std::shared_ptr<CatalogState>& catalog)
{
    auto services = g16test::Services(receive);
    services.enumerateMonitors = [catalog](std::vector<pbapp::MonitorInfo>& output)
    {
        const std::scoped_lock lock(catalog->mutex);
        if (catalog->throwOnEnumeration)
        {
            throw std::runtime_error("injected catalog exception");
        }
        output = catalog->monitors;
        return catalog->status;
    };
    return services;
}

void CheckCaptureStopped(const std::shared_ptr<g16test::ReceiveState>& receive, const std::uint32_t starts)
{
    const std::scoped_lock lock(receive->mutex);
    REQUIRE(receive->sessionStarts == starts);
    REQUIRE_FALSE(receive->normalized.active);
    if (starts != 0)
    {
        REQUIRE(receive->capture.shutdownComplete);
        REQUIRE(receive->demod.demodulator.shutdown);
    }
}

void CheckAuthorityReport(const pbapp::DecoderSnapshot& snapshot)
{
    REQUIRE(snapshot.singleMonitorCaptureEnabled);
    const auto report = pbapp::BuildDecoderRunReportJson({}, snapshot);
    const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(report));
    REQUIRE(document.isObject());
    const auto root = document.object();
    REQUIRE(root.contains("singleMonitorCapture"));
    REQUIRE(root.contains("monitorSafety"));
    const auto single = root.value("singleMonitorCapture").toObject();
    const auto dual = root.value("monitorSafety").toObject();
    REQUIRE(single.value("enabled").toBool());
    REQUIRE(single.value("preflightPassed").toBool() == snapshot.singleMonitorCapturePreflightPassed);
    REQUIRE(single.value("status").toString().toStdString() == snapshot.singleMonitorCaptureStatus);
    REQUIRE(single.value("revalidationCount").toInteger() == static_cast<qint64>(snapshot.singleMonitorCaptureRevalidationCount));
    REQUIRE_FALSE(dual.value("preflightPassed").toBool());
    REQUIRE(dual.value("revalidationCount").toInteger() == 0);
    REQUIRE(dual.value("status").toString() == "NotApplicableSingleMonitorCapture");
}

}

TEST_CASE("Single-screen PAM4 accepts the entire selected screen and contained physical ROI", "[single-monitor][configuration]")
{
    const auto profile = GENERATE(pbapp::VisualProfile::ExperimentalPam4, pbapp::VisualProfile::ExperimentalPam4Wide);
    const auto dimensions = GENERATE(std::pair{1920L, 1080L}, std::pair{2560L, 1440L}, std::pair{2560L, 1600L}, std::pair{3840L, 2160L});
    Scratch scratch;
    auto monitor = Target(dimensions.first, dimensions.second);
    const auto config = Configuration(scratch, profile, monitor);
    REQUIRE(pbapp::ValidateDecoderConfig(config));
    REQUIRE(EqualRect(&config.region.physicalRect, &config.region.monitorPhysicalRect));
    REQUIRE_FALSE(config.monitorSafety);
    pbcapturenormalize::CaptureNormalizeConfig capture;
    pbdemodd3d11::CaptureDemodulatorConfig demod;
    REQUIRE(pbapp::ApplicationRuntimeTestAccess::ProbeExperimentalPam4CaptureConfig(config, capture, demod));
    REQUIRE(EqualRect(&capture.capture.region.physicalRect, &monitor.physicalRect));
    REQUIRE(demod.visualProfileId == (profile == pbapp::VisualProfile::ExperimentalPam4 ?
        pbprotocol::kPam4ExperimentalProfile.visualProfileId : pbprotocol::kPam4WideExperimentalProfile.visualProfileId));
    auto region = config;
    region.region.physicalRect = {120, 90, 1560, 900};
    REQUIRE(pbapp::ValidateDecoderConfig(region));
    monitor.physicalRect = {-dimensions.first, -dimensions.second, 0, 0};
    monitor.dpiX = 144;
    monitor.dpiY = 144;
    REQUIRE(pbapp::ValidateDecoderConfig(Configuration(scratch, profile, monitor)));
}

TEST_CASE("Single-screen authority does not relax profile geometry metadata or resource admission", "[single-monitor][configuration][negative]")
{
    const auto profile = GENERATE(pbapp::VisualProfile::ExperimentalPam4, pbapp::VisualProfile::ExperimentalPam4Wide);
    Scratch scratch;
    const auto valid = Configuration(scratch, profile);
    for (int mutation = 0; mutation < 30; mutation++)
    {
        auto config = valid;
        switch (mutation)
        {
        case 0: config.singleMonitorCapture.reset(); break;
        case 1: config.monitorSafety = pbapp::MonitorSafetySelection{Target(), Target()}; break;
        case 2: config.singleMonitorCapture->monitor = nullptr; break;
        case 3: config.region.physicalRect.left = -1; break;
        case 4: config.singleMonitorCapture->physicalRect.right--; break;
        case 5: config.singleMonitorCapture->dpiX++; break;
        case 6: config.singleMonitorCapture->rotation = DXGI_MODE_ROTATION_ROTATE90; break;
        case 7: config.singleMonitorCapture->deviceName.clear(); break;
        case 8: config.remoteMetadata.experimentMonitorIdentity.clear(); break;
        case 9: config.remoteMetadata.experimentMonitorIdentity += "-conflict"; break;
        case 10: config.remoteMetadata.protectedMonitorIdentity = "not-permitted"; break;
        case 11: config.replayInputPath = L"not-opened"; break;
        case 12: config.replayOutputPath = L"not-created"; break;
        case 13: config.diagnosticCaptureOnly = true; break;
        case 14: config.captureBackend = pbapp::CaptureBackend::Wgc; break;
        case 15: config.visualProfile = pbapp::VisualProfile::UnifiedLc4; break;
        case 16: config.visualProfile = pbapp::VisualProfile::DirectLevels2x2; break;
        case 17: config.remoteMetadata.channelType = pbapp::ChannelType::LocalDesktop; break;
        case 18: config.measurement = std::make_shared<pbapp::RunMeasurementRecorder>(); break;
        case 19: config.replayEvidenceVisualProfileId = pbprotocol::kUnifiedVisualProfileId; break;
        case 20: config.replayMaximumCaptureFramesPerSecond = 1; break;
        case 21: config.singleMonitorCapture->dxgiOutputIdentityAvailable = false; break;
        case 22: config.region.physicalRect.right = 1439; break;
        case 23: config.region.physicalRect.bottom = 809; break;
        case 24: config.region.dpiX = 0; break;
        case 25: config.region.physicalRect.right = (std::numeric_limits<LONG>::max)(); break;
        case 26: config.remoteMetadata.remoteProvider.clear(); break;
        case 27: config.singleMonitorCapture->deviceName = std::wstring(1, static_cast<wchar_t>(0xD800)); break;
        case 28: config.memoryBudget = pbapp::DecoderMemoryBudget{}; break;
        case 29: config.visualProfile = pbapp::VisualProfile::RemoteVisualLowFps; break;
        }
        CAPTURE(mutation);
        REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
        REQUIRE_FALSE(pbapp::ApplicationRuntimeTestAccess::ValidateUnifiedReplayConfig(config));
    }
    auto budget = valid;
    budget.budgetBoundDecoders = true;
    budget.memoryBudget = pbapp::DecoderMemoryBudget{};
    REQUIRE(pbapp::ValidateDecoderConfig(budget));
    auto dual = valid;
    dual.singleMonitorCapture.reset();
    auto protectedMonitor = Target();
    protectedMonitor.deviceName = L"PB-TEST-PROTECTED";
    protectedMonitor.monitor = reinterpret_cast<HMONITOR>(std::uintptr_t{2});
    protectedMonitor.physicalRect = {-1920, 0, 0, 1080};
    dual.monitorSafety = pbapp::MonitorSafetySelection{protectedMonitor, Target()};
    dual.remoteMetadata.protectedMonitorIdentity = "PB-TEST-PROTECTED";
    REQUIRE(pbapp::ValidateDecoderConfig(dual));
    dual.monitorSafety->protectedMonitor = Target();
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(dual));
    REQUIRE(std::filesystem::is_empty(scratch.Path() / L"output"));
}

TEST_CASE("One-monitor catalog starts full-screen capture without a protected display and stops cleanly", "[single-monitor][runtime]")
{
    const auto profile = GENERATE(pbapp::VisualProfile::ExperimentalPam4, pbapp::VisualProfile::ExperimentalPam4Wide);
    Scratch scratch;
    const auto config = Configuration(scratch, profile);
    const auto receive = std::make_shared<g16test::ReceiveState>();
    const auto catalog = std::make_shared<CatalogState>(*config.singleMonitorCapture);
    pbapp::DecoderRuntime runtime(Services(receive, catalog));
    REQUIRE(runtime.Start(config));
    REQUIRE(g16test::WaitFor([&]() { return runtime.GetSnapshot().singleMonitorCaptureRevalidationCount >= 2; }));
    runtime.Stop();
    const auto snapshot = runtime.GetSnapshot();
    REQUIRE(snapshot.state == pbapp::DecoderState::Stopped);
    REQUIRE(snapshot.singleMonitorCapturePreflightPassed);
    REQUIRE(snapshot.singleMonitorCaptureStatus == "PASS");
    REQUIRE_FALSE(snapshot.finalPublishSucceeded);
    REQUIRE_FALSE(snapshot.finalReopenVerified.value_or(false));
    CheckCaptureStopped(receive, 1);
    REQUIRE(EqualRect(&receive->requestedCapture.capture.region.physicalRect, &config.region.physicalRect));
    CheckAuthorityReport(snapshot);
    REQUIRE(std::filesystem::is_empty(scratch.Path() / L"output"));
}

TEST_CASE("Stale or unavailable single-screen identity fails before creating any capture", "[single-monitor][runtime][negative]")
{
    const auto profile = GENERATE(pbapp::VisualProfile::ExperimentalPam4, pbapp::VisualProfile::ExperimentalPam4Wide);
    Scratch scratch;
    const auto config = Configuration(scratch, profile);
    for (int mutation = 0; mutation < 16; mutation++)
    {
        const auto receive = std::make_shared<g16test::ReceiveState>();
        const auto catalog = std::make_shared<CatalogState>(*config.singleMonitorCapture);
        auto services = Services(receive, catalog);
        if (mutation == 15)
        {
            services.enumerateMonitors = {};
        }
        else
        {
            catalog->Change(mutation);
        }
        pbapp::DecoderRuntime runtime(std::move(services));
        REQUIRE(runtime.Start(config));
        REQUIRE(g16test::WaitFor([&]() { return runtime.GetSnapshot().state == pbapp::DecoderState::Failed; }));
        runtime.Stop();
        const auto snapshot = runtime.GetSnapshot();
        CAPTURE(mutation, snapshot.errorDetail);
        REQUIRE_FALSE(snapshot.singleMonitorCapturePreflightPassed);
        REQUIRE(snapshot.singleMonitorCaptureRevalidationCount == 0);
        REQUIRE(snapshot.singleMonitorCaptureStatus == "FAIL");
        REQUIRE(snapshot.errorDetail.find("before startup") != std::string::npos);
        REQUIRE_FALSE(snapshot.actualBackend);
        REQUIRE(snapshot.captureBackendAttempts == 0);
        REQUIRE_FALSE(snapshot.finalPublishSucceeded);
        CheckCaptureStopped(receive, 0);
        CheckAuthorityReport(snapshot);
    }
    REQUIRE(std::filesystem::is_empty(scratch.Path() / L"output"));
}

TEST_CASE("Single-screen identity is rechecked during receive and safely retires capture on change", "[single-monitor][runtime][negative]")
{
    const auto profile = GENERATE(pbapp::VisualProfile::ExperimentalPam4, pbapp::VisualProfile::ExperimentalPam4Wide);
    const int mutation = GENERATE(0, 3, 4, 7, 8, 11, 12, 13);
    Scratch scratch;
    const auto config = Configuration(scratch, profile);
    const auto receive = std::make_shared<g16test::ReceiveState>();
    const auto catalog = std::make_shared<CatalogState>(*config.singleMonitorCapture);
    pbapp::DecoderRuntime runtime(Services(receive, catalog));
    REQUIRE(runtime.Start(config));
    REQUIRE(g16test::WaitFor([&]() { return runtime.GetSnapshot().singleMonitorCaptureRevalidationCount >= 2; }));
    catalog->Change(mutation);
    REQUIRE(g16test::WaitFor([&]() { return runtime.GetSnapshot().state == pbapp::DecoderState::Failed; }));
    runtime.Stop();
    const auto snapshot = runtime.GetSnapshot();
    REQUIRE(snapshot.singleMonitorCapturePreflightPassed);
    REQUIRE(snapshot.singleMonitorCaptureStatus == "FAIL");
    REQUIRE(snapshot.errorDetail.find("during run") != std::string::npos);
    REQUIRE_FALSE(snapshot.finalPublishSucceeded);
    REQUIRE_FALSE(snapshot.finalReopenVerified.value_or(false));
    CheckCaptureStopped(receive, 1);
    CheckAuthorityReport(snapshot);
    REQUIRE(std::filesystem::is_empty(scratch.Path() / L"output"));
}

TEST_CASE("Single-screen live receive loop recovers only pixel-derived PAM4 payload through final reopen", "[single-monitor][runtime][file][pixels]")
{
    const auto profile = GENERATE(pbapp::VisualProfile::ExperimentalPam4, pbapp::VisualProfile::ExperimentalPam4Wide);
    Scratch scratch;
    const auto monitor = profile == pbapp::VisualProfile::ExperimentalPam4 ? Target() : Target(2560, 1600);
    const auto config = Configuration(scratch, profile, monitor);
    const auto receive = std::make_shared<g16test::ReceiveState>();
    const auto catalog = std::make_shared<CatalogState>(monitor);
    pbapp::DecoderRuntime runtime(Services(receive, catalog));
    const auto sourceBytes = g16test::RawBytes(192U * 1024U);
    const auto source = scratch.Path() / L"single-screen-pixels.bin";
    {
        std::ofstream file(source, std::ios::binary);
        file.write(reinterpret_cast<const char*>(sourceBytes.data()), static_cast<std::streamsize>(sourceBytes.size()));
        REQUIRE(file.good());
    }
    REQUIRE(runtime.Start(config));
    REQUIRE(g16test::WaitFor([&]() { return runtime.GetSnapshot().singleMonitorCaptureRevalidationCount >= 2; }));
    pbapp::ExperimentalPam4FileProbeOptions options;
    options.visualProfile = profile;
    options.maximumFrames = 96;
    std::uint64_t delivered = 0;
    options.decodedFrameForTest = [&](const pbdemodd3d11::CaptureDemodulatorResult& frame)
    {
        // The probe provides no source bytes, descriptors or expected identities
        // to this receiver. Only the CPU-decoded raster result crosses its
        // existing OS/GPU test edge, with at most one outstanding observation.
        g16test::Check(frame.bootstrap.IsAccepted(), "pixel decode did not accept Bootstrap");
        receive->Push(frame);
        delivered++;
        g16test::Check(g16test::WaitFor([&]() { return receive->Delivered() == delivered; }), "live receive did not consume pixel result");
    };
    pbapp::ExperimentalPam4FileProbeSnapshot probe;
    const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeExperimentalPam4File(source.wstring(),
        scratch.Path() / L"state", (scratch.Path() / L"probe-output").wstring(), options, probe);
    INFO(status.message);
    REQUIRE(status);
    REQUIRE(probe.independentlyReopenedEqual);
    REQUIRE(g16test::WaitFor([&]() { return runtime.GetSnapshot().state == pbapp::DecoderState::Completed; }));
    runtime.Stop();
    const auto snapshot = runtime.GetSnapshot();
    INFO(snapshot.errorDetail);
    REQUIRE(g16test::VerifyOutput(snapshot, sourceBytes));
    REQUIRE(snapshot.finalReopenVerified == true);
    REQUIRE(snapshot.visualProfile == profile);
    REQUIRE(snapshot.singleMonitorCaptureStatus == "PASS");
    REQUIRE(delivered == probe.decodedFrames);
    REQUIRE(delivered <= options.maximumFrames);
    REQUIRE(snapshot.outerConflictRejections == 0);
    REQUIRE(snapshot.outerResourceRejections == 0);
    CheckCaptureStopped(receive, 1);
    CheckAuthorityReport(snapshot);
}

TEST_CASE("Default report does not invent a single-screen capture authority", "[single-monitor][report]")
{
    pbapp::DecoderSnapshot snapshot;
    snapshot.visualProfile = GENERATE(pbapp::VisualProfile::DirectLevels2x2, pbapp::VisualProfile::UnifiedLc4,
        pbapp::VisualProfile::UnifiedGrayFast, pbapp::VisualProfile::ExperimentalPam4, pbapp::VisualProfile::ExperimentalPam4Wide);
    const auto report = pbapp::BuildDecoderRunReportJson({}, snapshot);
    const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(report));
    REQUIRE(document.isObject());
    REQUIRE(document.object().contains("singleMonitorCapture"));
    const auto single = document.object().value("singleMonitorCapture").toObject();
    REQUIRE_FALSE(single.value("enabled").toBool());
    REQUIRE_FALSE(single.value("preflightPassed").toBool());
    REQUIRE(single.value("revalidationCount").toInteger() == 0);
}
