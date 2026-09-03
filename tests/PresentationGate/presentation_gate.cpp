#include "gate_support.h"
#include "local_desktop_runtime.h"
#include "pbmodulation/reference_raster.h"
#include "pbmodulation/remote_visual_low_fps.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/session_random.h"
#include "run_report.h"

#include <array>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
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

std::vector<std::byte> ReadLf4Golden(const char* const name, const std::size_t expectedBytes)
{
    const std::filesystem::path path = std::filesystem::path(PB_REMOTE_VISUAL_LF4_GOLDEN_DIR) / name;
    std::error_code error;
    const std::uintmax_t fileBytes = std::filesystem::file_size(path, error);
    Require(!error && fileBytes == expectedBytes, "LF4 Golden has an unexpected size: " + path.string());
    std::ifstream input(path, std::ios::binary);
    input.exceptions(std::ios::badbit);
    std::vector<std::byte> bytes(expectedBytes);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Require(input.gcount() == static_cast<std::streamsize>(bytes.size()) && input.peek() == std::char_traits<char>::eof(),
        "LF4 Golden read was incomplete or has trailing bytes: " + path.string());
    return bytes;
}

std::string DigestHex(const std::array<std::byte, pbprotocol::kDigestBytes>& digest)
{
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const std::byte value : digest)
    {
        output << std::setw(2) << std::to_integer<unsigned int>(value);
    }
    return output.str();
}

void WriteCreateOnly(const std::filesystem::path& path, const std::span<const std::byte> bytes)
{
    Require(bytes.size() <= (std::numeric_limits<DWORD>::max)(), "create-only evidence exceeds the bounded Win32 write size");
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    Require(file.Get() != INVALID_HANDLE_VALUE, "create-only evidence path already exists or cannot be created: " + path.string());
    DWORD written = 0;
    Require(WriteFile(file.Get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != FALSE &&
        written == static_cast<DWORD>(bytes.size()),
        "create-only evidence write was incomplete: " + path.string());
    Require(FlushFileBuffers(file.Get()) != FALSE, "create-only evidence flush failed: " + path.string());
}

void WriteCreateOnly(const std::filesystem::path& path, const std::string& text)
{
    WriteCreateOnly(path, std::as_bytes(std::span(text)));
}

std::string GetUtcTimestamp()
{
    SYSTEMTIME time{};
    GetSystemTime(&time);
    std::ostringstream output;
    output << std::setfill('0') << std::setw(4) << time.wYear << '-' << std::setw(2) << time.wMonth << '-' <<
        std::setw(2) << time.wDay << 'T' << std::setw(2) << time.wHour << ':' << std::setw(2) << time.wMinute << ':' <<
        std::setw(2) << time.wSecond << 'Z';
    return output.str();
}

bool CurrentProcessOwnsForegroundWindow() noexcept
{
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr)
    {
        return false;
    }
    DWORD processId = 0;
    static_cast<void>(GetWindowThreadProcessId(foreground, &processId));
    return processId == GetCurrentProcessId();
}

DWORD GetForegroundProcessId()
{
    const HWND foreground = GetForegroundWindow();
    Require(foreground != nullptr, "BLOCKED: no foreground window is available for the focus-preservation Gate");
    DWORD processId = 0;
    static_cast<void>(GetWindowThreadProcessId(foreground, &processId));
    Require(processId != 0, "BLOCKED: foreground process identity is unavailable");
    return processId;
}

template <typename Predicate>
pbapp::EncoderSnapshot WaitForEncoder(pbapp::EncoderRuntime& runtime, Predicate predicate, const char* const description,
    const std::chrono::milliseconds timeout = std::chrono::seconds(20))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    pbapp::EncoderSnapshot lastSnapshot;
    do
    {
        const auto snapshot = runtime.GetSnapshot();
        lastSnapshot = snapshot;
        Require(snapshot.state != pbapp::EncoderState::Failed,
            std::string(description) + " failed: " + snapshot.errorDetail);
        if (predicate(snapshot))
        {
            return snapshot;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error(std::string("timeout: ") + description + " state=" +
        pbapp::GetEncoderStateName(lastSnapshot.state) + " frameSequence=" + std::to_string(lastSnapshot.frameSequence) +
        " cycleCount=" + std::to_string(lastSnapshot.cycleCount) + " sourceReplacements=" +
        std::to_string(lastSnapshot.sourceTextureReplacements) + " repeatedPresents=" +
        std::to_string(lastSnapshot.repeatedPresentCalls) + " status=" + lastSnapshot.statusMessage);
}

const Monitor& GetRightmostCanonicalMonitor(const std::vector<Monitor>& monitors)
{
    Require(monitors.size() >= 2, "BLOCKED: LF4 right-monitor containment requires two active monitors");
    const auto rightmost = std::max_element(monitors.begin(), monitors.end(), [](const Monitor& left, const Monitor& right)
    {
        return left.info.rcMonitor.left < right.info.rcMonitor.left;
    });
    Require(rightmost != monitors.end(), "BLOCKED: rightmost monitor selection failed");
    const std::int64_t width = static_cast<std::int64_t>(rightmost->info.rcMonitor.right) - rightmost->info.rcMonitor.left;
    const std::int64_t height = static_cast<std::int64_t>(rightmost->info.rcMonitor.bottom) - rightmost->info.rcMonitor.top;
    Require(width >= pbmodulation::kLocalDesktopCanvasWidth && height >= pbmodulation::kLocalDesktopCanvasHeight,
        "BLOCKED: rightmost monitor cannot contain the canonical LF4 canvas");
    return *rightmost;
}

const Monitor& GetLeftmostProtectedMonitor(const std::vector<Monitor>& monitors, const Monitor& experimentMonitor)
{
    const Monitor* protectedMonitor = nullptr;
    for (const Monitor& candidate : monitors)
    {
        if (candidate.handle == experimentMonitor.handle)
        {
            continue;
        }
        if (protectedMonitor == nullptr || candidate.info.rcMonitor.left < protectedMonitor->info.rcMonitor.left)
        {
            protectedMonitor = &candidate;
        }
    }
    Require(protectedMonitor != nullptr, "BLOCKED: no distinct ProtectedMonitor is available");
    Require(protectedMonitor->info.rcMonitor.left < experimentMonitor.info.rcMonitor.left,
        "BLOCKED: right-monitor Gate cannot prove that the ProtectedMonitor remains on the left");
    return *protectedMonitor;
}

void CheckInitialContract(const DataWindowSnapshot& snapshot, const DataWindowConfig& config)
{
    Require(snapshot.candidateContractSatisfied && snapshot.state == WindowState::Running, "candidate contract not satisfied");
    const auto& contract = snapshot.contract;
    Require(contract.bufferWidth == config.width && contract.bufferHeight == config.height && contract.bufferCount == config.bufferCount &&
                contract.maximumFrameLatency == config.maximumFrameLatency && contract.flipEffect == config.flipEffect,
            "descriptor readback mismatch");
    Require(contract.bgraUnorm && contract.noMsaa && contract.alphaIgnored && contract.scalingNone && contract.tearingDisabled && contract.latencyWaitable &&
                contract.perMonitorV2 && contract.resizableChrome && contract.immutableCanonicalSource && contract.pointSampled &&
                contract.centeredLetterbox && contract.neutralMatteBelowMinimum,
            "native contract flag mismatch");
    Require(snapshot.environment.clientWidth == config.width && snapshot.environment.clientHeight == config.height && snapshot.environment.singleMonitor,
            "physical geometry readback mismatch");
}

void ResizeClientArea(const HWND handle, const std::uint32_t width, const std::uint32_t height)
{
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR style = GetWindowLongPtrW(handle, GWL_STYLE);
    Require(style != 0 || GetLastError() == ERROR_SUCCESS, "Data Window style readback failed");
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR extendedStyle = GetWindowLongPtrW(handle, GWL_EXSTYLE);
    Require(extendedStyle != 0 || GetLastError() == ERROR_SUCCESS, "Data Window extended style readback failed");
    RECT outer{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    Require(AdjustWindowRectExForDpi(&outer, static_cast<DWORD>(style), GetMenu(handle) != nullptr,
        static_cast<DWORD>(extendedStyle), GetDpiForWindow(handle)) != FALSE,
        "Data Window client-to-outer size conversion failed");
    Require(SetWindowPos(handle, nullptr, 0, 0, outer.right - outer.left, outer.bottom - outer.top,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE, "native client resize failed");
}

void CheckNoActivateWindow(const HWND handle)
{
    Require(handle != nullptr && IsWindow(handle) != FALSE, "Data Window HWND is unavailable");
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR extendedStyle = GetWindowLongPtrW(handle, GWL_EXSTYLE);
    Require(extendedStyle != 0 || GetLastError() == ERROR_SUCCESS, "Data Window extended style readback failed");
    Require((extendedStyle & WS_EX_NOACTIVATE) != 0, "Data Window can activate and steal the user's foreground focus");
}

void ResizeAndRestore(DataWindow& window, const DataWindowConfig& config, Evidence& evidence)
{
    const HWND handle = reinterpret_cast<HWND>(DataWindowTestAccess::GetWindowToken(window));
    const auto before = window.GetSnapshot();
    const std::uint32_t resizedWidth = config.width - 1;
    const std::uint32_t resizedHeight = config.height - 1;
    ResizeClientArea(handle, resizedWidth, resizedHeight);
    const auto resized = WaitFor(
        window,
        [&before, resizedWidth, resizedHeight](const auto& snapshot)
        {
            return snapshot.state == WindowState::Running && snapshot.candidateContractSatisfied &&
                snapshot.timing.presentationEpoch > before.timing.presentationEpoch &&
                snapshot.contract.bufferWidth == resizedWidth && snapshot.contract.bufferHeight == resizedHeight &&
                CanPresentData(snapshot.viewport.disposition);
        },
        "resizable aspect-fit presentation");
    Require(!resized.timing.presentedVisualFps, "resized epoch reused old presentation observations");
    Require(resized.totalPresentCalls == before.totalPresentCalls, "resize spontaneously presented data");
    evidence.Record("aspect-fit-resize", resized);
    ResizeClientArea(handle, config.width, config.height);
    const auto restored = WaitFor(
        window,
        [&resized](const auto& snapshot)
        {
            return snapshot.candidateContractSatisfied && snapshot.timing.presentationEpoch > resized.timing.presentationEpoch;
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
    CheckNoActivateWindow(reinterpret_cast<HWND>(originalWindow));
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
            Require(cleanup.liveGraphicsObjects == 0 && cleanup.liveOwnedHandles == 0, "partial native initialization leaked resources");
            evidence.Note(std::string("cleanup-stage=") + GetPresentationStageName(stage) +
                " PASS; graphics objects and owned handles released by Shutdown");
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
            Require(cleanup.liveGraphicsObjects == 0 && cleanup.liveOwnedHandles == 0, "native shutdown retained GPU resources or handles");
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

void RunLf4Encoder(const bool warp, Evidence& evidence)
{
    const auto monitors = GetMonitors();
    const Monitor& monitor = GetRightmostCanonicalMonitor(monitors);
    DataWindowConfig config;
    config.width = pbmodulation::kLocalDesktopCanvasWidth;
    config.height = pbmodulation::kLocalDesktopCanvasHeight;
    config.repeatActiveFrame = true;
    config.clientOrigin = GetOrigin(monitor, config.width, config.height);
    const auto window = CreateDataWindow(config, {warp, true, true});
    CheckInitialContract(window->GetSnapshot(), config);
    const HWND handle = reinterpret_cast<HWND>(DataWindowTestAccess::GetWindowToken(*window));
    CheckNoActivateWindow(handle);
    Require(GetForegroundWindow() != handle, "LF4 Data Window stole foreground activation");
    const auto bootstrap = ReadLf4Golden("lf4-bootstrap.bin", pbprotocol::kBootstrapRecordBytes);
    const auto codedData = ReadLf4Golden("lf4-coded-data.bin", pbmodulation::kRemoteVisualLowFpsDataBytes);
    const auto digestPinBytes = ReadLf4Golden("lf4-raster.blake3", 65);
    Require(digestPinBytes.back() == std::byte{'\n'} &&
        std::ranges::all_of(std::span(digestPinBytes).first(64), [](const std::byte value)
        {
            const char character = static_cast<char>(std::to_integer<unsigned char>(value));
            return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
        }), "LF4 raster digest pin is not canonical lowercase hex plus newline");
    const std::string digestPin(reinterpret_cast<const char*>(digestPinBytes.data()), 64);
    std::vector<std::byte> pixels(pbmodulation::kLocalDesktopFrameBgraBytes);
    Require(static_cast<bool>(pbmodulation::EncodeRemoteVisualLowFpsFrame(bootstrap, codedData, pixels)),
        "canonical LF4 raster generation failed");
    const auto cpuDigest = pbprotocol::ComputeBlake3Digest(pixels);
    Require(DigestHex(cpuDigest) == digestPin, "canonical LF4 CPU raster digest drifted from Step 08");

    const auto parsed = pbprotocol::ParseBootstrapRecord(bootstrap);
    Require(static_cast<bool>(parsed), "LF4 Golden Bootstrap parse failed");
    auto warmupRecord = parsed.Value();
    warmupRecord.frameSequence = 16;
    std::array<std::byte, pbprotocol::kBootstrapRecordBytes> warmupBootstrap{};
    Require(static_cast<bool>(pbprotocol::SerializeBootstrapRecord(warmupRecord, warmupBootstrap)), "warm-up LF4 Bootstrap serialization failed");
    std::vector<std::byte> warmupPixels(pbmodulation::kLocalDesktopFrameBgraBytes);
    Require(static_cast<bool>(pbmodulation::EncodeRemoteVisualLowFpsFrame(warmupBootstrap, codedData, warmupPixels)),
        "warm-up LF4 raster generation failed");
    const auto initial = window->GetSnapshot();
    Require(static_cast<bool>(window->SubmitFrame({warmupPixels, config.width, config.height, static_cast<std::size_t>(config.width) * 4,
        16, initial.timing.presentationEpoch})), "warm-up LF4 submit failed");
    const auto warmup = WaitFor(*window, [&initial](const DataWindowSnapshot& snapshot)
    {
        return !snapshot.inFlightFrame && (snapshot.invalidatedActiveFrames > initial.invalidatedActiveFrames ||
            (snapshot.activeFrame && snapshot.totalSuccessfulPresents >= initial.totalSuccessfulPresents + 8));
    }, "LF4 initial DXGI timing epoch warm-up", std::chrono::seconds(20));
    evidence.Record("lf4-timing-warmup", warmup);

    Require(static_cast<bool>(window->SubmitFrame({pixels, config.width, config.height, static_cast<std::size_t>(config.width) * 4,
        17, warmup.timing.presentationEpoch})), "canonical LF4 submit failed");
    const std::uint64_t canonicalSourceReplacement = warmup.sourceTextureReplacements + 1;
    const auto repeated = WaitFor(*window, [&warmup, canonicalSourceReplacement](const DataWindowSnapshot& snapshot)
    {
        return snapshot.totalSuccessfulPresents >= warmup.totalSuccessfulPresents + 24 &&
            snapshot.sourceTextureReplacements == canonicalSourceReplacement &&
            snapshot.repeatedPresentCalls >= warmup.repeatedPresentCalls + 23 && !snapshot.inFlightFrame;
    }, "LF4 immutable source repeated Presents", std::chrono::seconds(20));
    Require(repeated.activeFrame && repeated.activeFrameSequence == 17 &&
        repeated.activeFramePresentationEpoch == repeated.timing.presentationEpoch,
        "LF4 repeated Present changed active source identity");
    Require(repeated.submittedFrames == 2 && repeated.replacedPendingFrames == 0,
        "LF4 repeat path synthesized or replaced a logical frame");
    BackendDiagnostics diagnostics = DataWindowTestAccess::GetDiagnostics(*window);
    Require(diagnostics.verifiedUploads == canonicalSourceReplacement && diagnostics.immutableSourceCreations == canonicalSourceReplacement &&
        diagnostics.sourceReadbackBlake3Valid && diagnostics.lastSourceReadbackBlake3 == cpuDigest,
        "LF4 immutable source GPU readback does not match the Step 08 CPU raster");
    Require(diagnostics.sourceRendersToBackBuffer == diagnostics.presentCalls &&
        diagnostics.presentCalls == repeated.totalPresentCalls,
        "LF4 Present did not redraw the complete immutable source through the point sampler for every flip");
    Require(GetForegroundWindow() != handle, "LF4 repeated Present activated the Data Window");
    evidence.Record("lf4-step08-source-repeated", repeated);
    evidence.Note("LF4 Step08 CPU/GPU source BLAKE3=" + digestPin + " PASS; immutableSources=" +
        std::to_string(diagnostics.immutableSourceCreations) + " repeatedPresents=" + std::to_string(repeated.repeatedPresentCalls));

    auto nextRecord = parsed.Value();
    nextRecord.frameSequence = 18;
    std::array<std::byte, pbprotocol::kBootstrapRecordBytes> nextBootstrap{};
    Require(static_cast<bool>(pbprotocol::SerializeBootstrapRecord(nextRecord, nextBootstrap)), "next LF4 Bootstrap serialization failed");
    Require(static_cast<bool>(pbmodulation::EncodeRemoteVisualLowFpsFrame(nextBootstrap, codedData, pixels)),
        "next complete LF4 raster generation failed");
    const auto beforeReplacement = window->GetSnapshot();
    Require(static_cast<bool>(window->SubmitFrame({pixels, config.width, config.height, static_cast<std::size_t>(config.width) * 4,
        18, beforeReplacement.timing.presentationEpoch})), "next LF4 complete source submit failed");
    const auto replaced = WaitFor(*window, [&repeated, canonicalSourceReplacement](const DataWindowSnapshot& snapshot)
    {
        return snapshot.sourceTextureReplacements == canonicalSourceReplacement + 1 && snapshot.activeFrameSequence == 18 &&
            snapshot.totalSuccessfulPresents >= repeated.totalSuccessfulPresents + 12 && !snapshot.inFlightFrame;
    }, "LF4 complete source replacement", std::chrono::seconds(20));
    Require(replaced.repeatedPresentCalls >= repeated.repeatedPresentCalls + 11 && replaced.submittedFrames == 3,
        "LF4 replacement did not retain the new stable raster across repeated Presents");
    diagnostics = DataWindowTestAccess::GetDiagnostics(*window);
    Require(diagnostics.verifiedUploads == canonicalSourceReplacement + 1 &&
        diagnostics.immutableSourceCreations == canonicalSourceReplacement + 1 &&
        diagnostics.sourceRendersToBackBuffer == diagnostics.presentCalls && diagnostics.debugErrors == 0,
        "LF4 replacement lifecycle/readback/debug verification incomplete");
    Require(GetForegroundWindow() != handle, "LF4 replacement activated the Data Window");
    evidence.Record("lf4-next-source-repeated", replaced);
    window->Stop();
    const auto stoppedDiagnostics = DataWindowTestAccess::GetDiagnostics(*window);
    Require(stoppedDiagnostics.liveGraphicsObjects == 0 && stoppedDiagnostics.liveOwnedHandles == 0 &&
        IsWindow(handle) == FALSE,
        "LF4 native gate leaked its HWND, D3D11 resources, or owned handles");
    evidence.Note(std::string("LF4 native immutable/repeat gate PASS; backend=") + (warp ? "WARP" : "hardware") +
        " rightMonitor=" + Utf8(monitor.info.szDevice));
}

void RunLf4ProductionEncoder(Evidence& evidence)
{
    constexpr std::uint32_t maximumLf4LogicalFps = 5;
    const auto monitors = GetMonitors();
    const Monitor& experimentMonitor = GetRightmostCanonicalMonitor(monitors);
    const Monitor& protectedMonitor = GetLeftmostProtectedMonitor(monitors, experimentMonitor);
    pbapp::MonitorSafetySelection monitorSafety;
    const pbapp::MonitorSafetyStatus resolvedSafety = pbapp::ResolveMonitorSafetySelection(
        protectedMonitor.info.szDevice, experimentMonitor.info.szDevice, monitorSafety);
    Require(static_cast<bool>(resolvedSafety), std::string("BLOCKED: cannot bind explicit ProtectedMonitor/ExperimentMonitor: ") +
        pbapp::GetMonitorSafetyErrorName(resolvedSafety.code));
    const DWORD foregroundProcessIdBefore = GetForegroundProcessId();
    const std::filesystem::path sourcePath = evidence.Directory() / "lf4-production-source.bin";
    const std::array sourceBytes{std::byte{0x53}};
    WriteCreateOnly(sourcePath, sourceBytes);

    pbapp::EncoderConfig config;
    config.sourcePath = sourcePath.wstring();
    config.visualProfile = pbapp::VisualProfile::RemoteVisualLowFps;
    config.monitorClientOrigin = GetOrigin(experimentMonitor, pbapp::phase1CanvasWidth, pbapp::phase1CanvasHeight);
    config.monitorSafety = monitorSafety;
    config.logicalVisualFps = maximumLf4LogicalFps;
    config.controlRepetitions = 1;
    config.remoteMetadata.channelType = pbapp::ChannelType::RemoteVisual;
    config.remoteMetadata.remoteProvider = "Step17NativeGate";
    config.remoteMetadata.protectedMonitorIdentity = Utf8(protectedMonitor.info.szDevice);
    config.remoteMetadata.experimentMonitorIdentity = Utf8(experimentMonitor.info.szDevice);
    pbapp::EncoderRuntime runtime;
    const auto started = runtime.Start(config);
    Require(static_cast<bool>(started), "production LF4 EncoderRuntime Start failed: " + started.message);
    const auto broadcasting = WaitForEncoder(runtime, [](const pbapp::EncoderSnapshot& snapshot)
    {
        return snapshot.state == pbapp::EncoderState::Broadcasting && snapshot.cycleCount >= 2 &&
            snapshot.cycleFrameCount != 0 && snapshot.frameSequence >= static_cast<std::uint64_t>(snapshot.cycleFrameCount) * 2 &&
            snapshot.submittedFrames >= snapshot.frameSequence && snapshot.sourceTextureReplacements >= snapshot.frameSequence &&
            snapshot.pendingFrames == 0 && snapshot.activeFrame;
    }, "production LF4 EncoderRuntime two Carousel cycles", std::chrono::seconds(30));
    Require(broadcasting.visualProfileId == pbmodulation::kRemoteVisualLowFpsProfileId &&
        broadcasting.visualLayoutVersion == pbmodulation::kRemoteVisualLowFpsLayoutVersion &&
        broadcasting.codedDataBytesPerFrame == pbmodulation::kRemoteVisualLowFpsDataBytes &&
        broadcasting.codewordsPerFrame == pbmodulation::kRemoteVisualLowFpsCodewords,
        "production LF4 EncoderRuntime did not bind the frozen Step08 wire profile");
    Require(broadcasting.configuredLogicalVisualFps == maximumLf4LogicalFps &&
        broadcasting.configuredLogicalDwellMilliseconds &&
        *broadcasting.configuredLogicalDwellMilliseconds == 200.0 &&
        broadcasting.minimumObservedLogicalDwellMilliseconds &&
        *broadcasting.minimumObservedLogicalDwellMilliseconds >= *broadcasting.configuredLogicalDwellMilliseconds &&
        broadcasting.logicalDwellViolationCount == 0 && broadcasting.generatedVisualFramesPerSecond &&
        *broadcasting.generatedVisualFramesPerSecond > 0 &&
        *broadcasting.generatedVisualFramesPerSecond <= maximumLf4LogicalFps,
        "production LF4 EncoderRuntime violated the configured 5 Hz logical dwell");
    Require(broadcasting.dataWindowLeft == config.monitorClientOrigin->x &&
        broadcasting.dataWindowTop == config.monitorClientOrigin->y &&
        broadcasting.dataWindowWidth == pbapp::phase1CanvasWidth &&
        broadcasting.dataWindowHeight == pbapp::phase1CanvasHeight &&
        broadcasting.candidateContractSatisfied,
        "production LF4 EncoderRuntime did not retain the right-monitor physical Data Window contract");
    Require(broadcasting.monitorSafetyPreflightPassed && broadcasting.monitorSafetyRevalidationCount != 0 &&
        broadcasting.monitorSafetyStatus == "PASS",
        "production LF4 EncoderRuntime did not retain the explicit dual-monitor safety authority");
    Require(broadcasting.repeatedPresentCalls > broadcasting.sourceTextureReplacements &&
        broadcasting.statusMessage.find("receiver completion is visible only on Decoder") != std::string::npos,
        "production LF4 EncoderRuntime did not preserve independent continuous sender broadcast semantics");
    Require(!CurrentProcessOwnsForegroundWindow(), "production LF4 EncoderRuntime stole foreground activation");

    const pbapp::RunReportContext reportContext{"PBPresentationGate", "Step17", "worktree-precommit", GetUtcTimestamp()};
    WriteCreateOnly(evidence.Directory() / "encoder-broadcasting-report.json",
        pbapp::BuildEncoderRunReportJson(reportContext, broadcasting) + "\n");
    const std::uint64_t externalCompletionMarkerFrame = broadcasting.frameSequence;
    evidence.Note("Production LF4 reached two Carousel cycles at frameSequence=" +
        std::to_string(externalCompletionMarkerFrame) + "; test-local decoder completion marker was not passed to EncoderRuntime");

    const auto continued = WaitForEncoder(runtime, [externalCompletionMarkerFrame](const pbapp::EncoderSnapshot& snapshot)
    {
        return snapshot.state == pbapp::EncoderState::Broadcasting && snapshot.frameSequence > externalCompletionMarkerFrame &&
            snapshot.submittedFrames >= snapshot.frameSequence && snapshot.sourceTextureReplacements >= snapshot.frameSequence &&
            snapshot.pendingFrames == 0 && snapshot.activeFrame;
    }, "production LF4 continuation after external completion marker");
    Require(continued.logicalDwellViolationCount == 0 && continued.cycleCount >= broadcasting.cycleCount &&
        continued.repeatedPresentCalls > broadcasting.repeatedPresentCalls && !CurrentProcessOwnsForegroundWindow(),
        "production LF4 EncoderRuntime did not continue stable broadcasting after the external completion observation");
    WriteCreateOnly(evidence.Directory() / "encoder-after-marker-report.json",
        pbapp::BuildEncoderRunReportJson(reportContext, continued) + "\n");

    runtime.RequestStop();
    runtime.Stop();
    const auto stopped = runtime.GetSnapshot();
    Require(stopped.state == pbapp::EncoderState::Stopped && stopped.runEndedUnixMilliseconds &&
        stopped.sourceStable && stopped.pendingFrames == 0 && !stopped.activeFrame && stopped.errorDetail.empty() &&
        stopped.statusMessage.find("no sender-side receiver completion was inferred") != std::string::npos,
        "production LF4 EncoderRuntime did not complete explicit bounded shutdown");
    const DWORD foregroundProcessIdAfter = GetForegroundProcessId();
    Require(foregroundProcessIdAfter == foregroundProcessIdBefore,
        "production LF4 EncoderRuntime changed the foreground process");
    WriteCreateOnly(evidence.Directory() / "encoder-stopped-report.json",
        pbapp::BuildEncoderRunReportJson(reportContext, stopped) + "\n");
    evidence.Note("Production LF4 EncoderRuntime native gate PASS; cycles=" + std::to_string(stopped.cycleCount) +
        " frames=" + std::to_string(stopped.frameSequence) + " sourceReplacements=" +
        std::to_string(stopped.sourceTextureReplacements) + " repeatedPresents=" +
        std::to_string(stopped.repeatedPresentCalls) + " protectedMonitor=" + Utf8(protectedMonitor.info.szDevice) +
        " experimentMonitor=" + Utf8(experimentMonitor.info.szDevice) + " foregroundProcessId=" +
        std::to_string(foregroundProcessIdAfter));
}

void RunLive(Evidence& evidence)
{
    const auto monitors = GetMonitors();
    Require(monitors.size() >= 2, "BLOCKED: monitor migration requires two active outputs");
    DataWindowConfig config;
    config.clientOrigin = GetOrigin(monitors[0], config.width, config.height);
    const auto window = CreateDataWindow(config);
    CheckInitialContract(window->GetSnapshot(), config);
    CheckNoActivateWindow(reinterpret_cast<HWND>(DataWindowTestAccess::GetWindowToken(*window)));
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
        Require(argumentCount == 3,
            "usage: PBPresentationGate --gpu-warp|--gpu-hardware|--lf4-encoder-warp|--lf4-encoder-hardware|--lf4-production-encoder|--live|--mode-supervisor EVIDENCE_ROOT");
        const std::wstring_view mode(arguments[1]);
        Require(mode == L"--gpu-warp" || mode == L"--gpu-hardware" || mode == L"--lf4-encoder-warp" ||
            mode == L"--lf4-encoder-hardware" || mode == L"--lf4-production-encoder" || mode == L"--live" ||
            mode == L"--mode-supervisor",
            "invalid gate mode argument");
        const std::filesystem::path root(arguments[2]);
        const std::string name = Utf8(mode.substr(2));
        Evidence evidence(root, name);
        try
        {
            if (mode == L"--gpu-warp" || mode == L"--gpu-hardware")
            {
                RunGpu(mode == L"--gpu-warp", evidence);
            }
            else if (mode == L"--lf4-encoder-warp" || mode == L"--lf4-encoder-hardware")
            {
                RunLf4Encoder(mode == L"--lf4-encoder-warp", evidence);
            }
            else if (mode == L"--lf4-production-encoder")
            {
                RunLf4ProductionEncoder(evidence);
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
