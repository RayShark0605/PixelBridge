#include "encoder_monitor_catalog.h"
#include "evidence_journal.h"
#include "gui_visual_mode.h"
#include "run_report.h"
#include "encoder_presentation_options.h"
#include "encoder_display_options.h"
#include "application_build_identity.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <sstream>

namespace
{
using namespace pbrenderd3d;

[[nodiscard]] pbapp::EncoderConfig MakeConfig()
{
    std::vector<pbapp::MonitorInfo> monitors;
    REQUIRE(pbapp::EnumerateEncoderMonitors(monitors));
    REQUIRE_FALSE(monitors.empty());
    const auto& monitor = monitors.front();
    auto config = pbapp::MakeUnifiedEncoderConfig(L"fixture.bin", 30,
        PhysicalPoint{monitor.physicalRect.left, monitor.physicalRect.top});
    config.singleMonitorFullscreen = monitor;
    // Win32 display-device names are ASCII; no device or display state is changed.
    for (const wchar_t character : monitor.deviceName)
    {
        REQUIRE(character >= 0);
        REQUIRE(character <= 127);
        config.remoteMetadata.experimentMonitorIdentity.push_back(static_cast<char>(character));
    }
    REQUIRE(pbapp::ApplyGuiEncoderVisualMode(config, 2, false));
    return config;
}

[[nodiscard]] PresentationContract MakeContract(const PresentationModel model, const std::uint32_t width, const std::uint32_t height)
{
    PresentationContract contract{width, height, 2, 1, FlipEffect::Discard,
        true, true, true, true, true, true, true, false, true, true, true, true};
    if (model == PresentationModel::ComposedBlt)
    {
        contract.presentationModel = model;
        contract.swapEffect = PresentationSwapEffect::BltDiscard;
        contract.pacing = PresentationPacing::GpuCompletionAndVsync;
        contract.scalingNone = false;
        contract.scalingStretch = true;
        contract.latencyWaitable = false;
    }
    else if (model == PresentationModel::QtRaster)
    {
        contract.presentationModel = model;
        contract.swapEffect = PresentationSwapEffect::NotApplicable;
        contract.pacing = PresentationPacing::QtEventLoop;
        contract.bufferCount = 0;
        contract.maximumFrameLatency = 0;
        contract.latencyWaitable = false;
        contract.rasterPaintEngine = true;
    }
    return contract;
}

struct State
{
    std::mutex mutex;
    DataWindowConfig received;
    bool initialized = false;
    bool stopped = false;
    std::uint64_t submissions = 0;
    std::vector<std::byte> pixels;
    std::function<void(DataWindowSnapshot&)> mutateSnapshot;
};

class Presentation final : public pbapp::EncoderPresentation
{
public:
    Presentation(std::shared_ptr<State> state, const PresentationModel actual) : state_(std::move(state)), actual_(actual)
    {
    }
    DataWindowSnapshot GetSnapshot() const override
    {
        const std::lock_guard lock(state_->mutex);
        DataWindowSnapshot result;
        result.state = state_->stopped ? WindowState::Stopped : WindowState::Running;
        result.contract = MakeContract(actual_, state_->received.width, state_->received.height);
        result.environment.clientWidth = state_->received.width;
        result.environment.clientHeight = state_->received.height;
        result.environment.adapterAvailable = true;
        result.candidateContractSatisfied = true;
        result.viewport.disposition = PresentationViewportDisposition::Active;
        result.viewport.scale = 1;
        result.timing.presentationEpoch = 1;
        result.timing.issue = pbpresenttiming::TimingIssue::UnsupportedStatistics;
        result.submittedFrames = state_->submissions;
        result.totalSuccessfulPresents = state_->submissions;
        result.sourceTextureReplacements = state_->submissions;
        result.pendingFrame = !state_->stopped && state_->submissions != 0;
        if (actual_ == PresentationModel::QtRaster)
        {
            result.environment.adapterAvailable = false;
            result.softwarePaintCalls = state_->submissions + 5;
            result.softwareFramesPainted = state_->submissions;
            result.totalSuccessfulPresents = 0;
            result.sourceTextureReplacements = 0;
            // Stale timing must never turn a software paint into DXGI telemetry.
            result.timing.presentCallFps = 61.0;
            result.timing.presentedVisualFps = 59.0;
        }
        if (state_->mutateSnapshot)
        {
            state_->mutateSnapshot(result);
        }
        return result;
    }
    PresentationStatus SubmitFrame(const CanonicalBgraFrameView& frame) override
    {
        const std::lock_guard lock(state_->mutex);
        state_->submissions++;
        state_->pixels.assign(frame.pixels.begin(), frame.pixels.end());
        return {};
    }
    void RequestStop() noexcept override
    {
        const std::lock_guard lock(state_->mutex);
        state_->stopped = true;
    }
    void Stop() noexcept override
    {
        RequestStop();
    }
private:
    std::shared_ptr<State> state_;
    const PresentationModel actual_;
};

class Scratch
{
public:
    Scratch()
    {
        root_ = std::filesystem::absolute(PB_TEST_SCRATCH_ROOT).lexically_normal();
        path_ = root_ / (L"presentation-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        REQUIRE(std::filesystem::create_directory(path_));
    }
    ~Scratch()
    {
        if (path_.parent_path() == root_ && path_.filename().wstring().starts_with(L"presentation-"))
        {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }
    }
    std::filesystem::path path_;
private:
    std::filesystem::path root_;
};
}

TEST_CASE("Display selection remains explicit and cannot change across active cleanup or fixed modes", "[encoder-presentation][qt-display]")
{
    CHECK(pbencoder::GetNextEncoderDisplayMode(PresentationModel::Flip) == PresentationModel::QtRaster);
    CHECK(pbencoder::GetNextEncoderDisplayMode(PresentationModel::QtRaster) == PresentationModel::Flip);
    CHECK_FALSE(pbencoder::GetNextEncoderDisplayMode(PresentationModel::ComposedBlt));
    CHECK_FALSE(pbencoder::GetNextEncoderDisplayMode(static_cast<PresentationModel>(255)));
    for (const auto state : {pbapp::EncoderState::Idle, pbapp::EncoderState::Stopped, pbapp::EncoderState::Failed})
    {
        for (int locks = 0; locks < 8; locks++)
        {
            CHECK(pbencoder::CanChangeEncoderDisplayMode(state, (locks & 1) != 0, (locks & 2) != 0, (locks & 4) != 0) == (locks == 0));
        }
    }
    for (const auto state : {pbapp::EncoderState::Preparing, pbapp::EncoderState::Broadcasting, pbapp::EncoderState::Stopping,
        static_cast<pbapp::EncoderState>(255)})
    {
        CHECK_FALSE(pbencoder::CanChangeEncoderDisplayMode(state, false, false, false));
    }
    const wchar_t* valid[]{L"encoder", L"--gui-display-test"};
    CHECK(pbencoder::ParseGuiDisplayTestArguments(2, valid));
    CHECK_FALSE(pbencoder::ParseGuiDisplayTestArguments(1, valid));
    CHECK_FALSE(pbencoder::ParseGuiDisplayTestArguments(2, nullptr));
    const wchar_t* extra[]{L"encoder", L"--gui-display-test", L"extra"};
    CHECK_FALSE(pbencoder::ParseGuiDisplayTestArguments(3, extra));
    const wchar_t* wrong[]{L"encoder", L"--gui-presentation-mode"};
    CHECK_FALSE(pbencoder::ParseGuiDisplayTestArguments(2, wrong));
    const wchar_t* nullToken[]{L"encoder", nullptr};
    CHECK_FALSE(pbencoder::ParseGuiDisplayTestArguments(2, nullToken));
}

TEST_CASE("Composed blt is explicit PAM4-only authority and never a measurement fallback", "[encoder-presentation]")
{
    Scratch scratch;
    const auto source = scratch.path_ / L"fixture.bin";
    {
        std::ofstream stream(source, std::ios::binary);
        stream << "presentation configuration fixture";
        REQUIRE(stream.good());
    }
    auto config = MakeConfig();
    config.sourcePath = source.wstring();
    REQUIRE(config.presentationModel == PresentationModel::Flip);
    REQUIRE(pbapp::ValidateEncoderConfig(config));
    config.presentationModel = PresentationModel::ComposedBlt;
    REQUIRE(pbapp::ValidateEncoderConfig(config));
    auto invalid = config;
    invalid.visualProfile = pbapp::VisualProfile::UnifiedLc4;
    invalid.segmentTargetBytes = 0;
    CHECK_FALSE(pbapp::ValidateEncoderConfig(invalid));
    invalid = config;
    invalid.presentationModel = static_cast<PresentationModel>(255);
    CHECK_FALSE(pbapp::ValidateEncoderConfig(invalid));
    invalid = config;
    invalid.singleMonitorFullscreen.reset();
    CHECK_FALSE(pbapp::ValidateEncoderConfig(invalid));
    invalid = config;
    invalid.measurement = std::make_shared<pbapp::RunMeasurementRecorder>();
    CHECK_FALSE(pbapp::ValidateEncoderConfig(invalid));
}

TEST_CASE("Renderer strategy is wired through the real encoder runtime without a visible presenter", "[encoder-presentation]")
{
    Scratch scratch;
    const auto source = scratch.path_ / L"fixture.bin";
    {
        std::ofstream stream(source, std::ios::binary);
        stream << "presentation regression fixture";
        REQUIRE(stream.good());
    }
    for (const auto model : {PresentationModel::Flip, PresentationModel::ComposedBlt})
    {
        auto config = MakeConfig();
        config.sourcePath = source.wstring();
        config.sessionStateRoot = scratch.path_ / L"sessions";
        config.presentationModel = model;
        const auto state = std::make_shared<State>();
        pbapp::EncoderRuntime runtime([state, model](const DataWindowConfig& received)
        {
            const std::lock_guard lock(state->mutex);
            state->received = received;
            state->initialized = true;
            return std::make_unique<Presentation>(state, model);
        });
        REQUIRE(pbapp::ValidateEncoderConfig(config));
        REQUIRE(runtime.Start(config));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline && runtime.GetSnapshot().submittedFrames == 0 &&
            runtime.GetSnapshot().state != pbapp::EncoderState::Failed)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        runtime.Stop();
        const auto snapshot = runtime.GetSnapshot();
        INFO(snapshot.errorDetail);
        REQUIRE(snapshot.state == pbapp::EncoderState::Stopped);
        REQUIRE(state->initialized);
        CHECK(state->received.presentationModel == model);
        CHECK(state->received.topmost);
        CHECK_FALSE(state->received.repeatActiveFrame);
        CHECK(snapshot.requestedPresentationModel == model);
        CHECK(snapshot.submittedFrames == 1);
        CHECK(snapshot.presentationContract);
        if (snapshot.presentationContract)
        {
            CHECK(snapshot.presentationContract->presentationModel == model);
        }
        CHECK_FALSE(snapshot.presentedVisualFps);
        CHECK(state->pixels.size() == static_cast<std::size_t>(state->received.width) * state->received.height * 4);
    }
}

TEST_CASE("Encoder reports and journal distinguish unavailable actual blt timing from requested policy", "[encoder-presentation]")
{
    pbapp::EncoderSnapshot snapshot;
    snapshot.visualProfile = pbapp::VisualProfile::ExperimentalPam4;
    snapshot.requestedPresentationModel = PresentationModel::ComposedBlt;
    const auto absent = pbapp::BuildEncoderRunReportJson({}, snapshot);
    CHECK(absent.find("\"actualModel\":null") != std::string::npos);
    snapshot.presentationContract = MakeContract(PresentationModel::ComposedBlt, 1920, 1080);
    snapshot.presentationWindowState = WindowState::Running;
    snapshot.presentationTimingIssue = pbpresenttiming::TimingIssue::UnsupportedStatistics;
    snapshot.successfulPresentCalls = 7;
    for (const auto& text : {pbapp::BuildEncoderRunReportJson({}, snapshot), pbapp::BuildEncoderJournalRecord(1, snapshot)})
    {
        CHECK(text.find("\"actualModel\":\"composed-blt\"") != std::string::npos);
        CHECK(text.find("\"swapEffect\":\"blt-discard\"") != std::string::npos);
        CHECK(text.find("\"pacing\":\"gpu-completion-and-vsync\"") != std::string::npos);
        CHECK(text.find("\"latencyWaitable\":false") != std::string::npos);
        CHECK(text.find("\"statisticsUnavailableReason\":\"WindowedBltStatisticsUnsupported\"") != std::string::npos);
        CHECK(text.find("\"presentedVisualFps\":null") != std::string::npos);
        CHECK(text.find("\"successfulPresentCalls\":7") != std::string::npos);
    }
}

TEST_CASE("Diagnostic GUI arguments are exact, failure immutable, and cannot register shell handlers", "[encoder-presentation]")
{
    for (const auto* const token : {L"flip", L"composed-blt"})
    {
        const wchar_t* arguments[]{L"encoder", L"--gui-presentation-mode", token};
        auto model = PresentationModel::Flip;
        CHECK(pbencoder::ParseGuiPresentationModeArguments(3, arguments, model));
        CHECK(model == (std::wstring_view(token) == L"flip" ? PresentationModel::Flip : PresentationModel::ComposedBlt));
    }
    for (const auto* const token : {L"", L"Flip", L"blt", L"composed-blt ", L"--gui-smoke"})
    {
        const wchar_t* arguments[]{L"encoder", L"--gui-presentation-mode", token};
        auto model = PresentationModel::ComposedBlt;
        CHECK_FALSE(pbencoder::ParseGuiPresentationModeArguments(3, arguments, model));
        CHECK(model == PresentationModel::ComposedBlt);
    }
    const wchar_t* missing[]{L"encoder", L"--gui-presentation-mode"};
    const wchar_t* conflict[]{L"encoder", L"--gui-presentation-mode", L"flip", L"--gui-measurement"};
    auto model = PresentationModel::ComposedBlt;
    CHECK_FALSE(pbencoder::ParseGuiPresentationModeArguments(2, missing, model));
    CHECK_FALSE(pbencoder::ParseGuiPresentationModeArguments(4, conflict, model));
    CHECK(pbencoder::ShouldRegisterEncoderShell(false, false, false, false, false));
    for (unsigned int flags = 0; flags < 16; flags++)
    {
        CHECK_FALSE(pbencoder::ShouldRegisterEncoderShell(true, (flags & 1U) != 0, (flags & 2U) != 0, (flags & 4U) != 0, (flags & 8U) != 0));
    }
}

TEST_CASE("An uncommitted diagnostic binary never identifies itself only by the base commit", "[encoder-presentation]")
{
    const std::string fingerprint(64, 'a');
    std::ostringstream diagnostic;
    pbapp::WriteApplicationBuildIdentity(diagnostic, "PixelBridgeEncoder", pbcore::GetBuildInfo(), pbprotocol::GetProtocolVersion(), "base-commit", fingerprint);
    CHECK(diagnostic.str().find("\"sourceState\":\"UncommittedDiagnosticCandidate\"") != std::string::npos);
    CHECK(diagnostic.str().find("\"sourceFingerprintSha256\":\"" + fingerprint + "\"") != std::string::npos);
    std::ostringstream ordinary;
    pbapp::WriteApplicationBuildIdentity(ordinary, "PixelBridgeEncoder", pbcore::GetBuildInfo(), pbprotocol::GetProtocolVersion(), "base-commit");
    CHECK(ordinary.str().find("sourceState") == std::string::npos);
    CHECK(ordinary.str().find("sourceFingerprintSha256") == std::string::npos);
}

TEST_CASE("Qt raster is explicit and native creation rejects it", "[encoder-presentation][qt-contract]")
{
    CHECK(pbapp::MakeUnifiedEncoderConfig(L"sample.bin").presentationModel == PresentationModel::Flip);
    DataWindowConfig qtConfiguration;
    qtConfiguration.presentationModel = PresentationModel::QtRaster;
    CHECK_FALSE(ValidateDataWindowConfig(qtConfiguration));
    const auto native = DataWindow::Create(qtConfiguration);
    REQUIRE_FALSE(native);
    CHECK(native.Error().code == PresentationErrorCode::InvalidConfiguration);
    CHECK(GetPresentationModelName(PresentationModel::QtRaster) == std::string_view("qt-raster"));

    Scratch scratch;
    const auto source = scratch.path_ / L"fixture.bin";
    {
        std::ofstream stream(source, std::ios::binary);
        stream << "Qt admission fixture";
        REQUIRE(stream.good());
    }
    for (int mode = 0; mode < 4; mode++)
    {
        auto config = MakeConfig();
        config.sourcePath = source.wstring();
        config.sessionStateRoot = scratch.path_ / L"not-created";
        config.segmentTargetBytes = 0;
        config.fullscreenRasterWidth = 0;
        auto& monitor = *config.singleMonitorFullscreen;
        monitor.physicalRect.right = monitor.physicalRect.left + 2560;
        monitor.physicalRect.bottom = monitor.physicalRect.top + 1440;
        REQUIRE(pbapp::ApplyGuiEncoderVisualMode(config, mode, false));
        config.presentationModel = PresentationModel::QtRaster;
        INFO(mode);
        REQUIRE(pbapp::ValidateEncoderConfig(config));
        pbapp::EncoderRuntime nativeRuntime;
        CHECK_FALSE(nativeRuntime.Start(config));
        CHECK(nativeRuntime.GetSnapshot().state == pbapp::EncoderState::Idle);
        CHECK_FALSE(std::filesystem::exists(config.sessionStateRoot));
        auto invalid = config;
        invalid.measurement = std::make_shared<pbapp::RunMeasurementRecorder>();
        CHECK_FALSE(pbapp::ValidateEncoderConfig(invalid));
        invalid = config;
        invalid.visualProfile = pbapp::VisualProfile::UnifiedGray;
        CHECK_FALSE(pbapp::ValidateEncoderConfig(invalid));
        invalid = config;
        invalid.visualProfile = static_cast<pbapp::VisualProfile>(255);
        CHECK_FALSE(pbapp::ValidateEncoderConfig(invalid));
    }
}

TEST_CASE("Qt raster reports paints without DXGI statistics", "[encoder-presentation][qt-contract]")
{
    pbapp::EncoderSnapshot snapshot;
    snapshot.visualProfile = pbapp::VisualProfile::ExperimentalPam4;
    snapshot.requestedPresentationModel = PresentationModel::QtRaster;
    snapshot.presentationContract = MakeContract(PresentationModel::QtRaster, 1920, 1080);
    snapshot.presentationWindowState = WindowState::Running;
    snapshot.presentationTimingIssue = pbpresenttiming::TimingIssue::UnsupportedStatistics;
    snapshot.softwarePaintCalls = 17;
    snapshot.softwareFramesPainted = 4;
    snapshot.presentCallFps = 61.0;
    snapshot.presentedVisualFps = 59.0;
    for (const auto& text : {pbapp::BuildEncoderRunReportJson({}, snapshot), pbapp::BuildEncoderJournalRecord(1, snapshot)})
    {
        CHECK(text.find("\"actualModel\":\"qt-raster\"") != std::string::npos);
        CHECK(text.find("\"swapEffect\":\"not-applicable\"") != std::string::npos);
        CHECK(text.find("\"pacing\":\"qt-event-loop\"") != std::string::npos);
        CHECK(text.find("\"statisticsUnavailableReason\":\"QtRasterHasNoDxgiStatistics\"") != std::string::npos);
        CHECK(text.find("\"rasterPaintEngine\":true") != std::string::npos);
        CHECK(text.find("\"softwarePaintCalls\":17") != std::string::npos);
        CHECK(text.find("\"softwareFramesPainted\":4") != std::string::npos);
        CHECK(text.find("\"presentedVisualFps\":null") != std::string::npos);
        CHECK(text.find("\"presentCallFps\":null") != std::string::npos);
        CHECK(text.find("\"presentedVisualFps\":59") == std::string::npos);
        CHECK(text.find("\"presentCallFps\":61") == std::string::npos);
        CHECK(text.find("\"successfulPresentCalls\":0") != std::string::npos);
    }
    const auto report = pbapp::BuildEncoderRunReportJson({}, snapshot);
    CHECK(report.find("\"adapterUnavailableReason\":\"QtRasterDoesNotUseDxgiAdapter\"") != std::string::npos);
    CHECK(report.find("Qt raster software paints; not receiver recovery or Citrix certification") != std::string::npos);
    CHECK(pbapp::BuildEncoderDiagnostics(snapshot).find("DXGI Presents: not applicable") != std::string::npos);
}

TEST_CASE("Qt runtime publishes a zero-swapchain contract and rejects mismatched raster authority", "[encoder-presentation][qt-contract]")
{
    Scratch scratch;
    const auto source = scratch.path_ / L"fixture.bin";
    {
        std::ofstream stream(source, std::ios::binary);
        stream << "Qt runtime contract fixture";
        REQUIRE(stream.good());
    }
    for (int mutation = 0; mutation < 7; mutation++)
    {
        auto config = MakeConfig();
        config.sourcePath = source.wstring();
        config.sessionStateRoot = scratch.path_ / (L"sessions-" + std::to_wstring(mutation));
        config.presentationModel = PresentationModel::QtRaster;
        const auto state = std::make_shared<State>();
        state->mutateSnapshot = [mutation](DataWindowSnapshot& snapshot)
        {
            switch (mutation)
            {
            case 1: snapshot.contract.rasterPaintEngine = false; break;
            case 2: snapshot.contract.presentationModel = PresentationModel::Flip; break;
            case 3: snapshot.contract.bufferCount = 2; break;
            case 4: snapshot.contract.latencyWaitable = true; break;
            case 5: snapshot.contract.bufferWidth++; break;
            case 6: snapshot.softwareRasterizer = true; break;
            default: break;
            }
        };
        pbapp::EncoderRuntime runtime([state](const DataWindowConfig& received)
        {
            const std::lock_guard lock(state->mutex);
            state->received = received;
            state->initialized = true;
            return std::make_unique<Presentation>(state, PresentationModel::QtRaster);
        });
        INFO(mutation);
        REQUIRE(runtime.Start(config));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline && runtime.GetSnapshot().submittedFrames == 0 &&
            runtime.GetSnapshot().state != pbapp::EncoderState::Failed)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        runtime.Stop();
        const auto snapshot = runtime.GetSnapshot();
        INFO(snapshot.errorDetail);
        if (mutation != 0)
        {
            CHECK(snapshot.submittedFrames == 0);
            CHECK(snapshot.state == pbapp::EncoderState::Failed);
            continue;
        }
        REQUIRE(snapshot.state == pbapp::EncoderState::Stopped);
        REQUIRE(snapshot.submittedFrames == 1);
        REQUIRE(snapshot.presentationContract);
        CHECK(snapshot.presentationContract->presentationModel == PresentationModel::QtRaster);
        CHECK(snapshot.presentationContract->bufferCount == 0);
        CHECK(snapshot.softwarePaintCalls == 6);
        CHECK(snapshot.softwareFramesPainted == 1);
        CHECK(snapshot.successfulPresentCalls == 0);
        CHECK_FALSE(snapshot.presentCallFps);
        CHECK_FALSE(snapshot.presentedVisualFps);
        CHECK_FALSE(snapshot.presentationAdapter);
    }
}
