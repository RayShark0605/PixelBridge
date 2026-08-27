#include "gate_support.h"
#include "pbmodulation/reference_raster.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/session_random.h"

#include <array>
#include <algorithm>
#include <iostream>
#include <string_view>

namespace presentationgate
{
using namespace pbrenderd3d;

Evidence::Evidence(const std::filesystem::path& root, const std::string& name)
{
    LARGE_INTEGER counter{};
    Require(QueryPerformanceCounter(&counter) != FALSE, "QPC unavailable");
    std::filesystem::create_directories(root);
    directory_ = root / (name + "-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(counter.QuadPart));
    Require(std::filesystem::create_directory(directory_), "evidence path already exists");
    notes_.exceptions(std::ios::badbit | std::ios::failbit);
    snapshots_.exceptions(std::ios::badbit | std::ios::failbit);
    notes_.open(directory_ / "gate.txt");
    snapshots_.open(directory_ / "snapshots.jsonl");
    std::cout << "Evidence: " << directory_.string() << '\n';
    Note("scope=presentation-candidate; GPU readback is not screen capture; DXGI timing is not receiver UniqueVisualFPS");
}

void Evidence::Note(const std::string& text)
{
    notes_ << text << '\n';
    notes_.flush();
    std::cout << text << '\n';
}

void Evidence::Record(const std::string& event, const DataWindowSnapshot& snapshot)
{
    Require(count_ < 4096, "bounded snapshot evidence exhausted");
    notes_ << "snapshot=" << count_ << " event=" << event << '\n';
    WriteDataWindowSnapshotJson(snapshots_, snapshot);
    snapshots_ << '\n';
    notes_.flush();
    snapshots_.flush();
    count_++;
}

std::vector<Monitor> GetMonitors()
{
    std::vector<Monitor> monitors;
    monitors.reserve(16);
    const BOOL enumerated = EnumDisplayMonitors(
        nullptr, nullptr,
        [](const HMONITOR handle, HDC, LPRECT, const LPARAM parameter) -> BOOL
        {
            auto& result = *reinterpret_cast<std::vector<Monitor>*>(parameter);
            if (result.size() == result.capacity())
            {
                return FALSE;
            }
            Monitor monitor;
            monitor.handle = handle;
            monitor.info.cbSize = sizeof(monitor.info);
            if (!GetMonitorInfoW(handle, &monitor.info))
            {
                return FALSE;
            }
            result.push_back(monitor);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&monitors));
    Require(enumerated != FALSE && !monitors.empty(), "BLOCKED: cannot enumerate up to 16 active monitors");
    return monitors;
}

PhysicalPoint GetOrigin(const Monitor& monitor, const std::uint32_t width, const std::uint32_t height)
{
    const std::int64_t monitorWidth = static_cast<std::int64_t>(monitor.info.rcMonitor.right) - monitor.info.rcMonitor.left;
    const std::int64_t monitorHeight = static_cast<std::int64_t>(monitor.info.rcMonitor.bottom) - monitor.info.rcMonitor.top;
    Require(monitorWidth >= width && monitorHeight >= height, "BLOCKED: canonical frame does not fit the output");
    return {static_cast<std::int32_t>(monitor.info.rcMonitor.left + (monitorWidth - width) / 2),
            static_cast<std::int32_t>(monitor.info.rcMonitor.top + (monitorHeight - height) / 2)};
}

std::unique_ptr<DataWindow> CreateDataWindow(const DataWindowConfig& config, const NativeBackendTestOptions& options)
{
    auto result = DataWindowTestAccess::Create(config, MakeNativeBackend(options));
    Require(static_cast<bool>(result), "create: " + Describe(result.Error()));
    return std::move(result).Value();
}

std::vector<std::byte> MakePixelOracle(const std::uint32_t width, const std::uint32_t height, const std::size_t rowPitch, const unsigned int seed)
{
    Require(width <= 1920 && height <= 1080 && rowPitch >= static_cast<std::size_t>(width) * 4 && rowPitch <= 16384, "invalid test oracle dimensions");
    std::vector<std::byte> pixels(rowPitch * height, std::byte{0xA7});
    for (std::size_t row = 0; row < height; row++)
    {
        for (std::size_t column = 0; column < width; column++)
        {
            const std::size_t offset = row * rowPitch + column * 4;
            pixels[offset] = static_cast<std::byte>((row * 13 + seed) & 255);
            pixels[offset + 1] = static_cast<std::byte>((column * 37 + seed * 11) & 255);
            pixels[offset + 2] = ((row + column + seed) % 2) == 0 ? std::byte{0} : std::byte{255};
            pixels[offset + 3] = std::byte{255};
        }
    }
    return pixels;
}

void PresentAndVerify(DataWindow& window, const DataWindowConfig& config, const std::span<const std::byte> pixels, const std::size_t pitch,
                      const std::uint64_t sequence, Evidence& evidence)
{
    const auto before = window.GetSnapshot();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool submitted = false;
    for (;;)
    {
        const auto current = window.GetSnapshot();
        Require(current.state != WindowState::Failed, "present: " + Describe(current.error));
        Require(current.state != WindowState::Stopped, "unexpected stopped window");
        if (current.totalSuccessfulPresents > before.totalSuccessfulPresents && !current.inFlightFrame)
        {
            Require(current.candidateContractSatisfied, "post-Present contract mismatch");
            Require(current.totalPresentCalls == current.totalSuccessfulPresents, "occluded or failed data Present in integration gate");
            evidence.Record("present", current);
            return;
        }
        if (!current.pendingFrame && !current.inFlightFrame && current.state == WindowState::Running)
        {
            // A real epoch change may invalidate an accepted CPU slot. Retry
            // only that explicitly discarded frame, not a failed Present.
            Require(!submitted || current.timing.presentationEpoch != before.timing.presentationEpoch, "submitted frame vanished without epoch change");
            const auto status = window.SubmitFrame({pixels, config.width, config.height, pitch, sequence, current.timing.presentationEpoch});
            Require(status || status.code == PresentationErrorCode::EpochMismatch || status.code == PresentationErrorCode::Paused,
                    "submit: " + Describe(status));
            submitted = submitted || static_cast<bool>(status);
        }
        Require(std::chrono::steady_clock::now() < deadline, "timed out waiting for real Present");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

namespace
{

void CheckInitialContract(const DataWindowSnapshot& snapshot, const DataWindowConfig& config)
{
    Require(snapshot.candidateContractSatisfied && snapshot.state == WindowState::Running, "candidate contract not satisfied");
    const auto& contract = snapshot.contract;
    Require(contract.bufferWidth == config.width && contract.bufferHeight == config.height && contract.bufferCount == config.bufferCount &&
                contract.maximumFrameLatency == config.maximumFrameLatency && contract.flipEffect == config.flipEffect,
            "descriptor readback mismatch");
    Require(contract.bgraUnorm && contract.noMsaa && contract.alphaIgnored && contract.scalingNone && contract.tearingDisabled && contract.latencyWaitable &&
                contract.perMonitorV2,
            "native contract flag mismatch");
    Require(snapshot.environment.clientWidth == config.width && snapshot.environment.clientHeight == config.height && snapshot.environment.singleMonitor,
            "physical geometry readback mismatch");
}

void ResizeAndRestore(DataWindow& window, const DataWindowConfig& config, Evidence& evidence)
{
    const HWND handle = reinterpret_cast<HWND>(DataWindowTestAccess::GetWindowToken(window));
    const auto before = window.GetSnapshot();
    Require(SetWindowPos(handle, nullptr, 0, 0, static_cast<int>(config.width - 1), static_cast<int>(config.height - 1),
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE,
            "native resize failed");
    const auto paused = WaitFor(
        window,
        [&before, &config](const auto& snapshot)
        {
            return snapshot.state == WindowState::Paused && snapshot.timing.presentationEpoch > before.timing.presentationEpoch &&
                   snapshot.contract.bufferWidth == config.width - 1;
        },
        "noncanonical resize pause");
    Require(!paused.candidateContractSatisfied && !paused.timing.presentedVisualFps, "resized pixels incorrectly certified");
    Require(paused.totalPresentCalls == before.totalPresentCalls, "resize spontaneously presented data");
    evidence.Record("noncanonical-resize", paused);
    Require(SetWindowPos(handle, nullptr, 0, 0, static_cast<int>(config.width), static_cast<int>(config.height), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) !=
                FALSE,
            "native restore size failed");
    const auto restored = WaitFor(
        window,
        [&paused](const auto& snapshot)
        {
            return snapshot.candidateContractSatisfied && snapshot.timing.presentationEpoch > paused.timing.presentationEpoch;
        },
        "canonical restore");
    Require(restored.bufferGeneration >= before.bufferGeneration + 2, "ResizeBuffers did not rebuild both back buffers");
    Require(restored.swapChainGeneration == before.swapChainGeneration, "same-adapter resize recreated the swap chain");
    Require(restored.timing.observedPresents == 0 && !restored.timing.presentedVisualFps, "old observations survived resize");
    evidence.Record("canonical-size-restored", restored);
}

void CheckNativeSwapChainReplacement(const bool warp, Evidence& evidence)
{
    // Exercise the real backend's destroy/recreate path on one physical
    // adapter. Altered normalized adapter metadata forces that path; this
    // is NOT evidence of physical cross-adapter monitor migration.
    struct NativeScope
    {
        std::unique_ptr<PresentationBackend> backend;
        ~NativeScope()
        {
            backend->Shutdown();
        }
    };
    NativeScope scope{MakeNativeBackend({warp, true, true})};
    DataWindowConfig config;
    config.width = 257;
    config.height = 129;
    const auto initialized = scope.backend->Initialize(config);
    Require(static_cast<bool>(initialized), "replacement fixture initialize: " + Describe(initialized));
    const std::uintptr_t originalWindow = scope.backend->GetWindowToken();
    for (unsigned int iteration = 0; iteration < 4; iteration++)
    {
        if (iteration != 0)
        {
            const auto polled = scope.backend->PollEnvironment();
            Require(static_cast<bool>(polled), "replacement environment unavailable");
            auto environment = polled.Value();
            environment.adapterLuidLow ^= 1;
            const auto replaced = scope.backend->Reconfigure(environment);
            Require(static_cast<bool>(replaced), "native flip-chain replacement: " + Describe(replaced));
        }
        Require(scope.backend->GetWindowToken() == originalWindow, "replacement escaped HWND-exclusivity by making another window");
        Require(scope.backend->GetDiagnostics().swapChainGeneration == iteration + 1, "replacement did not create a new flip chain");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (;;)
        {
            const auto waited = scope.backend->Wait(true, 50);
            Require(waited.wake != BackendWake::Failed, "replacement wait failed: " + Describe(waited.error));
            if (waited.wake == BackendWake::FramePermit)
            {
                break;
            }
            Require(std::chrono::steady_clock::now() < deadline, "new flip chain failed to grant its first permit");
        }
        const auto pixels = MakePixelOracle(config.width, config.height, static_cast<std::size_t>(config.width) * 4, iteration * 61);
        const auto uploaded = scope.backend->Upload(pixels);
        Require(static_cast<bool>(uploaded), "replacement byte oracle failed: " + Describe(uploaded));
        const auto presented = scope.backend->Present();
        Require(presented.error && presented.outcome == pbpresenttiming::PresentOutcome::Success, "replacement Present failed");
    }
    const auto diagnostics = scope.backend->GetDiagnostics();
    Require(diagnostics.verifiedUploads == 4 && diagnostics.debugErrors == 0 && diagnostics.framePermits == 4 && diagnostics.presentCalls == 4,
            "replacement lifecycle/readback/debug verification incomplete");
    scope.backend->Shutdown();
    Require(scope.backend->GetDiagnostics().liveGraphicsObjects == 0 && IsWindow(reinterpret_cast<HWND>(originalWindow)) == FALSE,
            "replacement fixture leaked native resources");
    evidence.Note("same-HWND native swap-chain replacement PASS (forced path, one physical adapter; not cross-adapter certification)");
}

void RunGpu(const bool warp, Evidence& evidence)
{
    CheckNativeSwapChainReplacement(warp, evidence);
    DataWindowConfig config;
    config.width = 257;
    config.height = 129;
    if (warp)
    {
        for (const auto stage : {PresentationStage::WakeEvent, PresentationStage::WindowClass, PresentationStage::Window, PresentationStage::Adapter,
                                 PresentationStage::Device, PresentationStage::SwapChain, PresentationStage::FrameLatency, PresentationStage::BackBuffer})
        {
            BackendDiagnostics cleanup;
            const NativeBackendTestOptions options{true, true, true, stage, &cleanup};
            const auto result = DataWindowTestAccess::Create(config, MakeNativeBackend(options));
            Require(!result && result.Error().stage == stage && result.Error().nativeError == static_cast<std::int32_t>(E_FAIL),
                    "initialization failure injection was not reached: " + Describe(result.Error()));
            Require(cleanup.liveGraphicsObjects == 0 && cleanup.liveOwnedHandles == 1, "partial native initialization leaked resources");
            evidence.Note(std::string("cleanup-stage=") + GetPresentationStageName(stage) + " PASS; wake handle remains owned until backend destructor");
        }
    }
    for (const auto effect : {FlipEffect::Discard, FlipEffect::Sequential})
    {
        for (const std::uint32_t latency : {1u, 2u})
        {
            config.flipEffect = effect;
            config.maximumFrameLatency = latency;
            BackendDiagnostics cleanup;
            const auto window = CreateDataWindow(config, {warp, true, true, PresentationStage::None, &cleanup});
            CheckInitialContract(window->GetSnapshot(), config);
            const std::size_t pitch = static_cast<std::size_t>(config.width) * 4 + 29;
            for (unsigned int frame = 0; frame < 4; frame++)
            {
                const auto pixels = MakePixelOracle(config.width, config.height, pitch, frame * 43);
                PresentAndVerify(*window, config, pixels, pitch, frame, evidence);
            }
            ResizeAndRestore(*window, config, evidence);
            const auto pixels = MakePixelOracle(config.width, config.height, pitch, 201);
            PresentAndVerify(*window, config, pixels, pitch, 4, evidence);
            const auto diagnostics = DataWindowTestAccess::GetDiagnostics(*window);
            Require(window->GetSnapshot().softwareRasterizer == warp, "telemetry did not label the explicit software test backend");
            Require(diagnostics.warp == warp && diagnostics.verifiedUploads >= 5 && diagnostics.debugErrors == 0,
                    "GPU byte-oracle/debug-layer verification incomplete");
            Require(diagnostics.framePermits >= diagnostics.presentCalls && diagnostics.presentCalls >= 5, "data Present bypassed a frame permit");
            const HWND originalWindow = reinterpret_cast<HWND>(DataWindowTestAccess::GetWindowToken(*window));
            Require(IsWindow(originalWindow) != FALSE, "test HWND was not alive before Stop");
            window->Stop();
            window->Stop();
            Require(cleanup.liveGraphicsObjects == 0 && cleanup.liveOwnedHandles == 1, "native shutdown retained GPU resources");
            Require(IsWindow(originalWindow) == FALSE && DataWindowTestAccess::GetWindowToken(*window) == 0, "HWND outlived owner shutdown");
            evidence.Note("GPU oracle PASS: warp=" + std::to_string(warp) + " effect=" + std::to_string(static_cast<unsigned int>(effect)) +
                          " latency=" + std::to_string(latency));
        }
    }
    config = {};
    const auto window = CreateDataWindow(config, {warp, true, true});
    std::array<std::byte, pbmodulation::kReferenceBootstrapRecordBytes> bootstrap{};
    std::array<std::byte, pbmodulation::kReferenceControlWindowBytes> control{};
    const std::vector<std::byte> data(pbmodulation::kReferenceDataRegionBytes, std::byte{0x96});
    std::vector<std::byte> pixels(pbmodulation::kReferenceFrameBgraBytes);
    const auto session = pbprotocol::GenerateRandomSessionId();
    Require(static_cast<bool>(session), "CSPRNG failed");
    for (std::uint64_t sequence = 0; sequence < 2; sequence++)
    {
        const pbprotocol::BootstrapRecord record{pbprotocol::kBootstrapVersion,
                                                 pbprotocol::GetProtocolVersion(),
                                                 1,
                                                 0x5042524546524153ULL,
                                                 pbprotocol::DeriveSessionTag(session.Value()),
                                                 sequence,
                                                 0,
                                                 0};
        Require(static_cast<bool>(pbprotocol::SerializeBootstrapRecord(record, bootstrap)), "Bootstrap serialization failed");
        Require(static_cast<bool>(pbmodulation::EncodeReferenceFrame({bootstrap, control, data}, pixels)), "canonical encode failed");
        PresentAndVerify(*window, config, pixels, static_cast<std::size_t>(config.width) * 4, sequence, evidence);
    }
    Require(DataWindowTestAccess::GetDiagnostics(*window).verifiedUploads >= 2, "canonical raster GPU verification missing");
    window->Stop();
    evidence.Note("canonical BGRA GPU byte verification PASS; no capture/physical-link claim");
}

void RunLive(Evidence& evidence)
{
    const auto monitors = GetMonitors();
    Require(monitors.size() >= 2, "BLOCKED: monitor migration requires two active outputs");
    DataWindowConfig config;
    config.clientOrigin = GetOrigin(monitors[0], config.width, config.height);
    const auto window = CreateDataWindow(config);
    CheckInitialContract(window->GetSnapshot(), config);
    const std::size_t pitch = static_cast<std::size_t>(config.width) * 4;
    auto pixels = MakePixelOracle(config.width, config.height, pitch, 9);
    std::uint64_t sequence = 0;
    const auto presentNext = [&]
    {
        StampSequence(pixels, sequence);
        PresentAndVerify(*window, config, pixels, pitch, sequence, evidence);
        sequence++;
    };
    for (unsigned int frame = 0; frame < 90; frame++)
    {
        presentNext();
    }
    ResizeAndRestore(*window, config, evidence);
    presentNext();
    const HWND handle = reinterpret_cast<HWND>(DataWindowTestAccess::GetWindowToken(*window));
    const auto beforeMinimize = window->GetSnapshot();
    ShowWindow(handle, SW_MINIMIZE);
    const auto minimized = WaitFor(
        *window,
        [](const auto& snapshot)
        {
            return snapshot.state == WindowState::Paused && snapshot.environment.minimized;
        },
        "minimize pause");
    Require(minimized.bufferGeneration == beforeMinimize.bufferGeneration, "minimize resized a zero/hidden client buffer");
    evidence.Record("minimized", minimized);
    ShowWindow(handle, SW_SHOWNOACTIVATE);
    const auto restored = WaitFor(
        *window,
        [&minimized](const auto& snapshot)
        {
            return snapshot.candidateContractSatisfied && snapshot.timing.presentationEpoch > minimized.timing.presentationEpoch;
        },
        "minimize restore");
    Require(!restored.timing.presentedVisualFps && restored.timing.observedPresents == 0, "restore reused old timing samples");
    evidence.Record("minimize-restored", restored);
    presentNext();
    const auto& firstBounds = monitors[0].info.rcMonitor;
    const auto& secondBounds = monitors[1].info.rcMonitor;
    const LONG commonTop = std::max(firstBounds.top, secondBounds.top);
    const LONG commonLeft = std::max(firstBounds.left, secondBounds.left);
    const std::int64_t commonHeight = static_cast<std::int64_t>(std::min(firstBounds.bottom, secondBounds.bottom)) - commonTop;
    const std::int64_t commonWidth = static_cast<std::int64_t>(std::min(firstBounds.right, secondBounds.right)) - commonLeft;
    PhysicalPoint transition;
    if ((firstBounds.right == secondBounds.left || secondBounds.right == firstBounds.left) && commonHeight >= config.height)
    {
        const LONG boundary = firstBounds.right == secondBounds.left ? firstBounds.right : secondBounds.right;
        transition = {static_cast<std::int32_t>(static_cast<std::int64_t>(boundary) - config.width / 2), commonTop};
    }
    else
    {
        Require((firstBounds.bottom == secondBounds.top || secondBounds.bottom == firstBounds.top) && commonWidth >= config.width,
                "BLOCKED: two adjacent outputs required to validate a real straddling transition");
        const LONG boundary = firstBounds.bottom == secondBounds.top ? firstBounds.bottom : secondBounds.bottom;
        transition = {commonLeft, static_cast<std::int32_t>(static_cast<std::int64_t>(boundary) - config.height / 2)};
    }
    const auto beforeStraddle = window->GetSnapshot();
    Require(SetWindowPos(handle, nullptr, transition.x, transition.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE,
            "straddling SetWindowPos failed");
    const auto straddled = WaitFor(
        *window,
        [&beforeStraddle](const auto& snapshot)
        {
            return snapshot.state == WindowState::Paused && !snapshot.environment.singleMonitor &&
                   snapshot.timing.presentationEpoch > beforeStraddle.timing.presentationEpoch;
        },
        "real cross-monitor transition pause");
    Require(window->SubmitFrame({pixels, config.width, config.height, pitch, sequence, straddled.timing.presentationEpoch}).code ==
                PresentationErrorCode::Paused,
            "straddling window accepted a data frame");
    Require(straddled.totalPresentCalls == beforeStraddle.totalPresentCalls && straddled.swapChainGeneration == beforeStraddle.swapChainGeneration,
            "straddling transition presented data or rebuilt before reaching a single monitor");
    evidence.Record("straddling-paused", straddled);
    for (const std::size_t monitorIndex : {std::size_t{1}, std::size_t{0}})
    {
        const auto before = window->GetSnapshot();
        const PhysicalPoint origin = GetOrigin(monitors[monitorIndex], config.width, config.height);
        Require(SetWindowPos(handle, nullptr, origin.x, origin.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE,
                "monitor migration SetWindowPos failed");
        const auto moved = WaitFor(
            *window,
            [&before, &monitors, monitorIndex](const auto& snapshot)
            {
                return snapshot.candidateContractSatisfied && snapshot.timing.presentationEpoch > before.timing.presentationEpoch &&
                       snapshot.environment.monitorIdentity == reinterpret_cast<std::uintptr_t>(monitors[monitorIndex].handle);
            },
            "monitor migration revalidation");
        Require(moved.timing.observedPresents == 0 && !moved.timing.presentedVisualFps, "monitor migration retained stale observations");
        evidence.Record("monitor-migrated", moved);
        for (unsigned int frame = 0; frame < 12; frame++)
        {
            presentNext();
        }
    }
    const auto final = window->GetSnapshot();
    Require(DataWindowTestAccess::GetDiagnostics(*window).verifiedUploads == 0, "live timing path unexpectedly used readback");
    evidence.Record("live-final", final);
    evidence.Note(std::string("DXGI timing state=") + pbpresenttiming::GetTimingStateName(final.timing.state) + " issue=" +
                  pbpresenttiming::GetTimingIssueName(final.timing.issue) + "; unavailable/gapped statistics remain unavailable, never inferred from calls");
    window->Stop();
}

}
}

int wmain(const int argumentCount, wchar_t* arguments[])
{
    using namespace presentationgate;
    try
    {
        if (argumentCount >= 2 && std::wstring_view(arguments[1]) == L"--mode-child")
        {
            return RunModeChild(argumentCount, arguments);
        }
        Require(argumentCount == 3, "usage: PBPresentationGate --gpu-warp|--gpu-hardware|--live|--mode-supervisor EVIDENCE_ROOT");
        const std::wstring_view mode(arguments[1]);
        Require(mode == L"--gpu-warp" || mode == L"--gpu-hardware" || mode == L"--live" || mode == L"--mode-supervisor", "invalid gate mode argument");
        const std::filesystem::path root(arguments[2]);
        const std::string name = Utf8(mode.substr(2));
        Evidence evidence(root, name);
        try
        {
            if (mode == L"--gpu-warp" || mode == L"--gpu-hardware")
            {
                RunGpu(mode == L"--gpu-warp", evidence);
            }
            else if (mode == L"--live")
            {
                RunLive(evidence);
            }
            else if (mode == L"--mode-supervisor")
            {
                RunModeSupervisor(root, evidence);
            }
            else
            {
                throw std::runtime_error("unknown presentation gate mode");
            }
            evidence.Note("PASS");
            return 0;
        }
        catch (const std::exception& exception)
        {
            evidence.Note(std::string("FAIL: ") + exception.what());
            throw;
        }
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Presentation gate failed: " << exception.what() << '\n';
        return 1;
    }
}
