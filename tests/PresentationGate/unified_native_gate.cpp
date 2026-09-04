#include "local_desktop_runtime.h"
#include "diagnostic_file.h"
#include "run_report.h"
#include "sender_carousel_scheduler.h"
#include "pbcore/build_info.h"

#include <Windows.h>

#include <array>
#include <chrono>
#include <climits>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace
{

using Clock = std::chrono::steady_clock;

void Require(const bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

std::uint32_t ParseBounded(const std::wstring_view text, const std::uint32_t minimum, const std::uint32_t maximum)
{
    Require(!text.empty(), "empty integer");
    std::uint32_t value = 0;
    for (const wchar_t character : text)
    {
        Require(character >= L'0' && character <= L'9', "invalid integer");
        const auto digit = static_cast<std::uint32_t>(character - L'0');
        Require(digit <= maximum && value <= (maximum - digit) / 10, "integer out of bounds");
        value = value * 10 + digit;
    }
    Require(value >= minimum, "integer below minimum");
    return value;
}

void WriteNew(const std::filesystem::path& path, const std::string& json)
{
    pbdiagnostic::DiagnosticFile file(path.c_str());
    file.Write(json);
    file.Finish();
}

class Evidence
{
public:
    explicit Evidence(const std::filesystem::path& root) : root_(root)
    {
        Require(std::filesystem::create_directory(root_), "evidence directory must be new");
        samples_.exceptions(std::ios::failbit | std::ios::badbit);
        samples_.open(root_ / "samples.jsonl", std::ios::binary);
    }

    void Sample(const std::string& json)
    {
        Require(json.size() < 64 * 1024 && bytes_ + json.size() + 1 <= 32 * 1024 * 1024, "evidence budget exhausted");
        samples_ << json << '\n';
        samples_.flush();
        bytes_ += json.size() + 1;
    }

    void Record(const std::string& name, const std::string& json) const
    {
        WriteNew(root_ / name, json);
    }

private:
    const std::filesystem::path root_;
    std::ofstream samples_;
    std::size_t bytes_ = 0;
};

pbapp::RunReportContext ReportContext(const std::string& role)
{
    return {"PBUnifiedNativeGate." + role, pbcore::GetBuildInfo().version, PB_GIT_COMMIT, ""};
}

std::string EncoderReport(const pbapp::RunReportContext& context, const pbapp::EncoderSnapshot& snapshot)
{
    auto json = pbapp::BuildEncoderRunReportJson(context, snapshot);
    json.pop_back();
    json += ",\"nativeGate\":{\"steadyMilliseconds\":" + std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count()) +
        ",\"clientLeft\":" + std::to_string(snapshot.dataWindowLeft) + ",\"clientTop\":" + std::to_string(snapshot.dataWindowTop) +
        ",\"clientWidth\":" + std::to_string(snapshot.dataWindowWidth) + ",\"clientHeight\":" + std::to_string(snapshot.dataWindowHeight) +
        ",\"frameSequence\":" + std::to_string(snapshot.frameSequence) + ",\"sourceTextureReplacements\":" + std::to_string(snapshot.sourceTextureReplacements) +
        ",\"repeatedPresentCalls\":" + std::to_string(snapshot.repeatedPresentCalls) + ",\"logicalDwellViolationCount\":" + std::to_string(snapshot.logicalDwellViolationCount) +
        ",\"pendingFrames\":" + std::to_string(snapshot.pendingFrames) + ",\"presentationEpoch\":" + std::to_string(snapshot.presentationEpoch) +
        ",\"candidateContractSatisfied\":" + (snapshot.candidateContractSatisfied ? "true" : "false") +
        ",\"rawSegmentCount\":" + std::to_string(snapshot.rawSegmentCount) + ",\"zstdSegmentCount\":" + std::to_string(snapshot.zstdSegmentCount) + "}}";
    return json;
}

pbapp::MonitorSafetySelection ResolveSafety()
{
    pbapp::MonitorSafetySelection safety;
    const auto status = pbapp::ResolveMonitorSafetySelection(L"\\\\.\\DISPLAY1", L"\\\\.\\DISPLAY2", safety);
    Require(static_cast<bool>(status), std::string("monitor safety: ") + pbapp::GetMonitorSafetyErrorName(status.code));
    const auto& right = safety.experimentMonitor;
    const auto& left = safety.protectedMonitor;
    Require(right.physicalRect.right - static_cast<std::int64_t>(right.physicalRect.left) == 2560 &&
        right.physicalRect.bottom - static_cast<std::int64_t>(right.physicalRect.top) == 1440 &&
        right.physicalRect.left >= left.physicalRect.right && right.rotation == DXGI_MODE_ROTATION_IDENTITY,
        "approved right-monitor topology is unavailable");
    return safety;
}

POINT GetClientOrigin(const pbapp::MonitorSafetySelection& safety)
{
    const auto& rectangle = safety.experimentMonitor.physicalRect;
    // Bounds leave room for normal PMv2 window chrome, including the largest
    // approved client size. All arithmetic is checked before narrowing.
    const std::int64_t x = static_cast<std::int64_t>(rectangle.left) + 64;
    const std::int64_t y = static_cast<std::int64_t>(rectangle.top) + 64;
    Require(x <= LONG_MAX - 2400 && y <= LONG_MAX - 1360, "monitor coordinates exceed gate bounds");
    return {static_cast<LONG>(x), static_cast<LONG>(y)};
}

RECT MakeClientRect(const POINT origin, const std::uint32_t width, const std::uint32_t height)
{
    Require(width >= 1200 && width <= 2304 && height >= 675 && height <= 1296, "client size exceeds approved gate scope");
    Require(static_cast<std::int64_t>(origin.x) + width <= LONG_MAX &&
        static_cast<std::int64_t>(origin.y) + height <= LONG_MAX, "client rectangle overflow");
    return {origin.x, origin.y, static_cast<LONG>(origin.x + static_cast<std::int64_t>(width)),
        static_cast<LONG>(origin.y + static_cast<std::int64_t>(height))};
}

void CheckTarget(const pbapp::MonitorSafetySelection& safety, const RECT& rectangle)
{
    const auto topology = pbapp::RevalidateMonitorSafetySelection(safety);
    Require(static_cast<bool>(topology), "monitor topology changed");
    const auto target = pbapp::ValidateMonitorSafetyTarget(safety, rectangle, MonitorFromRect(&rectangle, MONITOR_DEFAULTTONULL));
    Require(static_cast<bool>(target), std::string("protected-screen containment: ") + pbapp::GetMonitorSafetyErrorName(target.code));
}

void CheckInitialWindow(const pbapp::MonitorSafetySelection& safety, const RECT& client)
{
    CheckTarget(safety, client);
    // Match both DPI computations performed by the production native backend
    // before its SW_SHOWNOACTIVATE. Do not rely on a post-show safety check.
    for (const UINT dpi : {GetDpiForSystem(), safety.experimentMonitor.dpiX})
    {
        RECT outer = client;
        Require(AdjustWindowRectExForDpi(&outer, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_NOACTIVATE, dpi) != FALSE,
            "cannot calculate window chrome");
        CheckTarget(safety, outer);
    }
}

HWND FindOwnWindow()
{
    std::array<HWND, 2> found{};
    Require(EnumWindows([](const HWND window, const LPARAM context) -> BOOL
    {
        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        if (processId == GetCurrentProcessId())
        {
            wchar_t title[64]{};
            GetWindowTextW(window, title, 64);
            if (std::wstring_view(title) == L"PixelBridge Data Window")
            {
                auto& matches = *reinterpret_cast<std::array<HWND, 2>*>(context);
                matches[matches[0] == nullptr ? 0 : 1] = window;
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&found)) != FALSE, "cannot enumerate own data window");
    Require(found[1] == nullptr, "ambiguous owned data windows");
    return found[0];
}

RECT CheckOwnWindow(const HWND window, const pbapp::MonitorSafetySelection& safety)
{
    DWORD processId = 0;
    Require(GetWindowThreadProcessId(window, &processId) != 0 && processId == GetCurrentProcessId(), "window is not gate-owned");
    Require((GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_NOACTIVATE) != 0 && GetForegroundWindow() != window,
        "data window no-activation contract failed");
    RECT outer{};
    Require(GetWindowRect(window, &outer) != FALSE, "cannot inspect owned window");
    CheckTarget(safety, outer);
    return outer;
}

void ResizeOwnWindow(const HWND window, const pbapp::MonitorSafetySelection& safety,
    const std::uint32_t width, const std::uint32_t height)
{
    static_cast<void>(CheckOwnWindow(window, safety));
    RECT target = MakeClientRect(GetClientOrigin(safety), width, height);
    Require(AdjustWindowRectExForDpi(&target, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)), FALSE,
        static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE)), GetDpiForWindow(window)) != FALSE, "cannot size owned window");
    CheckTarget(safety, target);
    // This is an owned test-window geometry operation, not input automation.
    // Topmost prevents unrelated right-screen content from covering the raster;
    // no external HWND is moved and activation is explicitly forbidden.
    Require(SetWindowPos(window, HWND_TOPMOST, target.left, target.top, target.right - target.left,
        target.bottom - target.top, SWP_NOACTIVATE) != FALSE, "owned resize failed");
    static_cast<void>(CheckOwnWindow(window, safety));
}

std::string ScreenMatteSamples(const HWND window, const pbapp::MonitorSafetySelection& safety)
{
    static_cast<void>(CheckOwnWindow(window, safety));
    RECT client{};
    POINT origin{};
    Require(GetClientRect(window, &client) && ClientToScreen(window, &origin), "cannot inspect paused client");
    const RECT physical = MakeClientRect(origin, static_cast<std::uint32_t>(client.right), static_cast<std::uint32_t>(client.bottom));
    CheckTarget(safety, physical);
    const HDC screen = GetDC(nullptr);
    Require(screen != nullptr, "cannot read bounded screen matte samples");
    bool matched = true;
    for (const int row : {1, 2, 3})
    {
        for (const int column : {1, 2, 3})
        {
            const COLORREF pixel = GetPixel(screen, origin.x + client.right * column / 4, origin.y + client.bottom * row / 4);
            const auto neutral = pbrenderd3d::dataWindowNeutralMatteCodeValue;
            matched = matched && pixel == RGB(neutral, neutral, neutral);
        }
    }
    ReleaseDC(nullptr, screen);
    Require(matched, "actual right-screen matte probe mismatch");
    return "{\"actualScreenGdiSamples\":9,\"neutralMatteMatched\":true,\"fullRasterClaim\":false}";
}

void RunEncoder(const int count, wchar_t* arguments[])
{
    Require(count == 9, "encoder requires SOURCE NEW_EVIDENCE WIDTH HEIGHT FPS SECONDS PAUSE");
    const auto width = ParseBounded(arguments[4], 1440, 2304);
    const auto height = ParseBounded(arguments[5], 810, 1296);
    const auto fps = ParseBounded(arguments[6], 1, 60);
    const auto seconds = ParseBounded(arguments[7], 15, 90);
    const bool pause = ParseBounded(arguments[8], 0, 1) != 0;
    Require(!pause || (width == 1920 && height == 1080), "pause fixture requires initial 1x");
    const auto safety = ResolveSafety();
    const auto origin = GetClientOrigin(safety);
    CheckInitialWindow(safety, MakeClientRect(origin, 1920, 1080));
    CheckInitialWindow(safety, MakeClientRect(origin, width, height));
    Evidence evidence(arguments[3]);
    auto config = pbapp::MakeUnifiedEncoderConfig(arguments[2], fps, pbrenderd3d::PhysicalPoint{origin.x, origin.y});
    config.sessionStateRoot = std::filesystem::path(arguments[3]) / "session-state";
    pbapp::EncoderRuntime runtime; // Default production native presentation, no injected edge.
    const auto started = runtime.Start(config);
    Require(static_cast<bool>(started), started.message);
    const auto context = ReportContext("Encoder");
    const auto began = Clock::now();
    std::optional<Clock::time_point> readyAt;
    std::optional<pbapp::EncoderSnapshot> pausedBaseline;
    bool resized = false;
    bool restored = false;
    bool pauseRequested = false;
    std::uint64_t samples = 0;
    HWND window = nullptr;
    try
    {
        while (Clock::now() - began < std::chrono::seconds(seconds + 15))
        {
            if (window == nullptr)
            {
                window = FindOwnWindow();
            }
            if (window != nullptr)
            {
                if (!resized)
                {
                    ResizeOwnWindow(window, safety, width, height);
                    resized = true;
                }
                static_cast<void>(CheckOwnWindow(window, safety));
            }
            const auto snapshot = runtime.GetSnapshot();
            const auto json = EncoderReport(context, snapshot);
            evidence.Sample(json);
            samples++;
            Require(snapshot.state != pbapp::EncoderState::Failed && snapshot.state != pbapp::EncoderState::Stopped, snapshot.statusMessage);
            if (!readyAt && resized && snapshot.dataWindowWidth == width && snapshot.dataWindowHeight == height &&
                snapshot.sourceTextureReplacements >= 2 && snapshot.candidateContractSatisfied)
            {
                readyAt = Clock::now();
                evidence.Record("ready.json", json);
            }
            if (readyAt)
            {
                const auto elapsed = Clock::now() - *readyAt;
                if (pause && elapsed >= std::chrono::seconds(3) && !pauseRequested)
                {
                    ResizeOwnWindow(window, safety, 1344, 756);
                    pauseRequested = true;
                }
                if (pause && elapsed >= std::chrono::seconds(5) && !pausedBaseline)
                {
                    Require(snapshot.dataWindowWidth == 1344 && snapshot.dataWindowHeight == 756 &&
                        !snapshot.candidateContractSatisfied && snapshot.pendingFrames == 0, "native pause did not settle");
                    pausedBaseline = snapshot;
                    evidence.Record("paused.json", json);
                    evidence.Record("matte.json", ScreenMatteSamples(window, safety));
                }
                if (pausedBaseline && !restored)
                {
                    Require(snapshot.frameSequence == pausedBaseline->frameSequence &&
                        snapshot.submittedLogicalFrames == pausedBaseline->submittedLogicalFrames &&
                        snapshot.sourceTextureReplacements == pausedBaseline->sourceTextureReplacements &&
                        snapshot.sessionIdHex == pausedBaseline->sessionIdHex, "logical sender advanced while paused");
                    if (elapsed >= std::chrono::seconds(9))
                    {
                        evidence.Record("paused-end.json", json);
                        ResizeOwnWindow(window, safety, width, height);
                        restored = true;
                    }
                }
                if (elapsed >= std::chrono::seconds(seconds))
                {
                    Require(!pause || (restored && pausedBaseline && snapshot.frameSequence > pausedBaseline->frameSequence &&
                        snapshot.sessionIdHex == pausedBaseline->sessionIdHex && snapshot.candidateContractSatisfied),
                        "same-session native resume failed");
                    evidence.Record("active-final.json", json);
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        runtime.Stop();
        const auto snapshot = runtime.GetSnapshot();
        evidence.Record("final.json", EncoderReport(context, snapshot));
        Require(readyAt && Clock::now() - *readyAt >= std::chrono::seconds(seconds), "encoder gate readiness deadline expired");
        Require(snapshot.state == pbapp::EncoderState::Stopped && FindOwnWindow() == nullptr, "encoder did not cleanly release its window");
        evidence.Record("gate.json", "{\"passed\":true,\"defaultProductionRuntime\":true,\"safetySamples\":" + std::to_string(samples) + "}");
    }
    catch (...)
    {
        runtime.Stop();
        evidence.Record("failure-final.json", EncoderReport(context, runtime.GetSnapshot()));
        throw;
    }
}

// Read-only diagnostic adapter over the same native demodulator. Used only
// after a failed Gate; successful acceptance runs use the default factory.
class DiagnosticDemodulator final : public pbapp::DecoderDemodulator
{
public:
    DiagnosticDemodulator(std::shared_ptr<pbdemodd3d11::CaptureDemodulator> native, std::filesystem::path evidence) :
        native_(std::move(native)), evidence_(std::move(evidence))
    {
    }
    ~DiagnosticDemodulator() override
    {
        try
        {
            const auto value = native_->GetSnapshot();
            WriteNew(evidence_ / "demod-final.json", "{\"submitted\":" + std::to_string(value.submittedFrames) +
                ",\"completed\":" + std::to_string(value.completedFrames) + ",\"expired\":" + std::to_string(value.expiredFrames) +
                ",\"cancelled\":" + std::to_string(value.cancelledFrames) + ",\"bootstrapAccepted\":" + std::to_string(value.bootstrapAcceptedFrames) +
                ",\"stagedGpuSubmissions\":" + std::to_string(value.stagedGpuSubmissions) +
                ",\"stagedGpuCompletions\":" + std::to_string(value.stagedGpuCompletions) +
                ",\"demodRejected\":" + std::to_string(value.demodulationRejectedFrames) +
                ",\"postFecFailed\":" + std::to_string(value.postFecFailedFrames) +
                ",\"acceptedUnifiedBlocks\":" + std::to_string(value.acceptedUnifiedBlocks) + "}");
        }
        catch (...)
        {
            std::cerr << "diagnostic final snapshot write failed\n";
        }
    }
    std::shared_ptr<pbcapturenormalize::ScreenCaptureConsumer> GetConsumer() const override
    {
        return native_;
    }
    bool TakeResult(pbdemodd3d11::CaptureDemodulatorResult& result) override
    {
        const bool available = native_->TakeResult(result);
        if (available && !resultRecorded_)
        {
            const auto& value = result.demodulation.unifiedObservation;
            WriteNew(evidence_ / "first-result.json", "{\"kind\":" + std::to_string(static_cast<unsigned int>(result.kind)) +
                ",\"frameErasure\":" + std::to_string(static_cast<unsigned int>(value.frameErasure)) +
                ",\"baseErasure\":" + std::to_string(static_cast<unsigned int>(value.baseLuma.erasureReason)) +
                ",\"fineErasure\":" + std::to_string(static_cast<unsigned int>(value.fineLuma.erasureReason)) +
                ",\"chromaErasure\":" + std::to_string(static_cast<unsigned int>(value.chroma.erasureReason)) +
                ",\"acceptedBlocks\":" + std::to_string(value.acceptedBlocks) + "}");
            resultRecorded_ = true;
        }
        return available;
    }
    pbdemodd3d11::CaptureDemodulatorSnapshot GetSnapshot() const override
    {
        const auto snapshot = native_->GetSnapshot();
        if (!recorded_ && snapshot.lastDemodStatus.code != pbdemodd3d11::DemodError::None)
        {
            WriteNew(evidence_ / "first-demod-rejection.json", "{\"errorCode\":" + std::to_string(static_cast<unsigned int>(snapshot.lastDemodStatus.code)) +
                ",\"stage\":" + std::to_string(static_cast<unsigned int>(snapshot.lastDemodStatus.stage)) +
                ",\"nativeError\":" + std::to_string(snapshot.lastDemodStatus.nativeError) +
                ",\"rejectedFrames\":" + std::to_string(snapshot.demodulationRejectedFrames) +
                ",\"acceptedUnifiedBlocks\":" + std::to_string(snapshot.acceptedUnifiedBlocks) + "}");
            recorded_ = true;
        }
        return snapshot;
    }
private:
    std::shared_ptr<pbdemodd3d11::CaptureDemodulator> native_;
    const std::filesystem::path evidence_;
    mutable bool recorded_ = false;
    bool resultRecorded_ = false;
};

void InspectRoi(const int count, wchar_t* arguments[])
{
    Require(count == 6, "inspect requires BGRA NEW_REPORT WIDTH HEIGHT");
    const auto width = ParseBounded(arguments[4], 1440, 2304);
    const auto height = ParseBounded(arguments[5], 810, 1296);
    const std::size_t size = static_cast<std::size_t>(width) * height * 4;
    Require(std::filesystem::file_size(arguments[2]) == size, "BGRA dimensions mismatch");
    std::vector<std::byte> pixels(size);
    std::ifstream input(std::filesystem::path(arguments[2]), std::ios::binary);
    Require(static_cast<bool>(input.read(reinterpret_cast<char*>(pixels.data()), static_cast<std::streamsize>(size))), "BGRA read failed");
    auto created = pbmodulation::UnifiedVisualCpuOracle::Create(pbmodulation::UnifiedVisualCpuOracle::RequiredBytes());
    Require(static_cast<bool>(created), "CPU oracle creation failed");
    auto oracle = std::move(created).Value();
    const auto observation = oracle.DecodeMixedFrame({pixels, width, height, static_cast<std::size_t>(width) * 4,
        pbmodulation::LumaPixelFormat::Bgra8});
    std::ostringstream report;
    report << "{\"offlineActualScreenCpuOracle\":true,\"inputValid\":" << observation.inputValid <<
        ",\"frameErasure\":" << static_cast<unsigned int>(observation.frameErasure) <<
        ",\"bootstrapErasure\":\"" << pbmodulation::GetLocalDesktopErasureName(observation.bootstrap.erasure) << '"' <<
        ",\"originX\":" << observation.bootstrap.geometry.originX << ",\"originY\":" << observation.bootstrap.geometry.originY <<
        ",\"markerResidual\":" << observation.bootstrap.geometry.markerResidualPixels <<
        ",\"frameSequence\":" << observation.bootstrapRecord.frameSequence <<
        ",\"scaleX\":" << observation.bootstrap.geometry.scaleX << ",\"scaleY\":" << observation.bootstrap.geometry.scaleY <<
        ",\"baseErasure\":" << static_cast<unsigned int>(observation.baseLuma.erasureReason) <<
        ",\"fineErasure\":" << static_cast<unsigned int>(observation.fineLuma.erasureReason) <<
        ",\"chromaErasure\":" << static_cast<unsigned int>(observation.chroma.erasureReason) <<
        ",\"acceptedBlocks\":" << observation.acceptedBlocks << ",\"freshness\":[";
    for (std::size_t index = 0; index < observation.freshness.size(); index++)
    {
        const auto& value = observation.freshness[index];
        report << (index == 0 ? "" : ",") << "{\"current\":" << value.current << ",\"bitErrors\":" << value.bitErrors <<
            ",\"residual\":" << value.residual << '}';
    }
    report << "]}";
    WriteNew(arguments[3], report.str());
    std::cout << report.str() << '\n';
}

void RunDecoder(const int count, wchar_t* arguments[], const bool diagnostic)
{
    Require(count == 7, "decoder requires OUTPUT NEW_EVIDENCE WIDTH HEIGHT TIMEOUT");
    const auto width = ParseBounded(arguments[4], 1440, 2304);
    const auto height = ParseBounded(arguments[5], 810, 1296);
    const auto seconds = ParseBounded(arguments[6], 10, 90);
    const auto safety = ResolveSafety();
    const auto rectangle = MakeClientRect(GetClientOrigin(safety), width, height);
    CheckTarget(safety, rectangle);
    pbscreenregion::ScreenCaptureRegion region;
    Require(static_cast<bool>(pbscreenregion::ResolveScreenCaptureRegion(rectangle, region)), "cannot resolve gate ROI");
    Require(region.monitor == safety.experimentMonitor.monitor, "ROI resolved to wrong monitor");
    Evidence evidence(arguments[3]);
    pbapp::DecoderRuntimeServices services;
    if (diagnostic)
    {
        services.demodulatorFactory = [path = std::filesystem::path(arguments[3])](const auto& config, auto& output)
        {
            std::shared_ptr<pbdemodd3d11::CaptureDemodulator> native;
            const auto status = pbdemodd3d11::CaptureDemodulator::Create(config, native);
            if (status)
            {
                output = std::make_shared<DiagnosticDemodulator>(std::move(native), path);
            }
            return status;
        };
    }
    pbapp::DecoderRuntime runtime(services); // Default Auto WGC/DXGI and real Receiver/storage in both modes.
    const auto started = runtime.Start(pbapp::MakeUnifiedDecoderConfig(arguments[2], region));
    Require(static_cast<bool>(started), started.message);
    const auto context = ReportContext("Decoder");
    const auto deadline = Clock::now() + std::chrono::seconds(seconds);
    try
    {
        while (Clock::now() < deadline)
        {
            CheckTarget(safety, rectangle);
            const auto snapshot = runtime.GetSnapshot();
            evidence.Sample(pbapp::BuildDecoderRunReportJson(context, snapshot));
            if (snapshot.state == pbapp::DecoderState::Completed || snapshot.state == pbapp::DecoderState::Failed)
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        runtime.Stop();
        const auto snapshot = runtime.GetSnapshot();
        evidence.Record("final.json", pbapp::BuildDecoderRunReportJson(context, snapshot));
        Require(snapshot.state == pbapp::DecoderState::Completed && snapshot.wholeFileDigestCheck.value_or(false) &&
            snapshot.finalPublishSucceeded && snapshot.finalReopenVerified.value_or(false), "native receiver did not authoritatively publish");
        evidence.Record("gate.json", std::string("{\"passed\":true,\"sourcePathProvided\":false,\"diagnosticAdapter\":") + (diagnostic ? "true}" : "false}"));
    }
    catch (...)
    {
        runtime.Stop();
        evidence.Record("failure-final.json", pbapp::BuildDecoderRunReportJson(context, runtime.GetSnapshot()));
        throw;
    }
}

void RunPolicyChecks()
{
    // G09 uses absolute tick deadlines, not legacy minimum time-between-submit
    // semantics. A slower frame followed by a faster one legitimately has a
    // shorter completed-submit interval. Preserve the raw legacy counter in
    // evidence; it must not masquerade as a Unified scheduling failure.
    pbapp::SenderLogicalFrameClock clock;
    Require(static_cast<bool>(pbapp::SenderLogicalFrameClock::Create(15, 0, clock)), "clock fixture creation failed");
    pbapp::SenderLogicalFrameTick tick;
    Require(static_cast<bool>(clock.Acquire(0, tick)) && tick.logicalTickOrdinal == 0, "initial tick failed");
    Require(static_cast<bool>(clock.Commit(10000000)), "initial completion failed");
    Require(static_cast<bool>(clock.Acquire(66666667, tick)) && tick.logicalTickOrdinal == 1, "second tick failed");
    Require(static_cast<bool>(clock.Commit(80666667)), "second completion failed");
    Require(static_cast<bool>(clock.Acquire(133333334, tick)) && tick.logicalTickOrdinal == 2, "third tick failed");
    Require(static_cast<bool>(clock.Commit(143333334)), "third completion failed");
    Require(143333334ULL - 80666667ULL < 1000000000ULL / 15 && clock.GetSnapshot().droppedTickCount == 0,
        "absolute-deadline fixture must admit shorter completion intervals without dropping ticks");
    const RECT right{2560, 0, 5120, 1440};
    const RECT left{0, 0, 2560, 1440};
    for (const auto dimensions : {std::array{1920U, 1080U}, std::array{1440U, 810U}, std::array{2160U, 1215U},
        std::array{2240U, 1120U}, std::array{1344U, 756U}})
    {
        const RECT target = MakeClientRect({2624, 64}, dimensions[0], dimensions[1]);
        Require(pbapp::RectContains(right, target) && !pbapp::RectIntersects(left, target), "policy valid fixture failed");
    }
    Require(!pbapp::RectContains(right, RECT{2500, 0, 3940, 810}) &&
        pbapp::RectIntersects(left, RECT{2500, 0, 3940, 810}), "left-screen overlap not rejected");
    for (const std::wstring_view invalid : {L"", L"-1", L"61", L"4294967296", L"15x"})
    {
        bool rejected = false;
        try
        {
            static_cast<void>(ParseBounded(invalid, 1, 60));
        }
        catch (const std::exception&)
        {
            rejected = true;
        }
        Require(rejected, "invalid bounded argument accepted");
    }
    std::cout << "PASS: gate policy only; no window, capture, display mutation or input\n";
}

} // namespace

int wmain(const int count, wchar_t* arguments[])
{
    try
    {
        Require(count >= 2, "expected --encoder, --decoder or --self-test");
        const std::wstring_view role(arguments[1]);
        if (role == L"--self-test" && count == 2)
        {
            RunPolicyChecks();
        }
        else if (role == L"--encoder")
        {
            RunEncoder(count, arguments);
        }
        else if (role == L"--decoder" || role == L"--diagnose-decoder")
        {
            RunDecoder(count, arguments, role == L"--diagnose-decoder");
        }
        else if (role == L"--inspect-roi")
        {
            InspectRoi(count, arguments);
        }
        else
        {
            throw std::runtime_error("invalid gate role");
        }
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Unified native gate failed: " << exception.what() << '\n';
        return 1;
    }
}
