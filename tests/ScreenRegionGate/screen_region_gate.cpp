#include "gate_support.h"

#include <imm.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <exception>
#include <iostream>
#include <mutex>
#include <string_view>

namespace screenregiongate
{
using Microsoft::WRL::ComPtr;

Evidence::Evidence(const std::filesystem::path& root, const char* const name)
{
    LARGE_INTEGER counter{};
    Require(QueryPerformanceCounter(&counter) != FALSE, "QPC unavailable");
    std::filesystem::create_directories(root);
    directory_ = root / (std::string(name) + "-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(counter.QuadPart));
    Require(std::filesystem::create_directory(directory_), "evidence already exists");
    notes_.exceptions(std::ios::badbit | std::ios::failbit);
    records_.exceptions(std::ios::badbit | std::ios::failbit);
    notes_.open(directory_ / "gate.txt");
    records_.open(directory_ / "regions.jsonl");
    std::cout << "Evidence: " << directory_.string() << '\n';
    Note("scope=screen-region; real input/current topology only; no display setting changes; no capture certification");
}

void Evidence::Note(const std::string& message)
{
    Require(count_ < 1024, "evidence entry limit");
    count_++;
    notes_ << message << '\n';
    notes_.flush();
    std::cout << message << '\n';
}

void Evidence::Record(const char* const event, const ScreenCaptureRegion& region)
{
    Note(std::string("region=") + event);
    WriteScreenCaptureRegionJson(records_, region);
    records_ << '\n';
    records_.flush();
}

std::vector<OracleMonitor> ReadOracle()
{
    Require(AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2), "gate manifest is not PMv2");
    std::vector<OracleMonitor> monitors;
    monitors.reserve(64);
    const BOOL enumerated = EnumDisplayMonitors(
        nullptr, nullptr,
        [](const HMONITOR monitor, HDC, LPRECT, const LPARAM parameter) -> BOOL
        {
            auto& result = *reinterpret_cast<std::vector<OracleMonitor>*>(parameter);
            if (result.size() == result.capacity())
            {
                return FALSE;
            }
            MONITORINFO info{};
            info.cbSize = sizeof(info);
            if (!GetMonitorInfoW(monitor, &info))
            {
                return FALSE;
            }
            result.push_back({monitor, info.rcMonitor, 0, DXGI_MODE_ROTATION_UNSPECIFIED});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&monitors));
    Require(enumerated && !monitors.empty(), "BLOCKED: cannot enumerate active physical displays");
    ComPtr<IDXGIFactory1> factory;
    Require(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()))), "BLOCKED: DXGI factory unavailable");
    for (auto& monitor : monitors)
    {
        const HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"Screen region independent oracle", WS_POPUP, monitor.rect.left, monitor.rect.top, 1,
                                            1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Require(window != nullptr, "oracle DPI HWND creation failed");
        monitor.dpi = GetDpiForWindow(window);
        const bool matched = MonitorFromWindow(window, MONITOR_DEFAULTTONULL) == monitor.monitor &&
                             AreDpiAwarenessContextsEqual(GetWindowDpiAwarenessContext(window), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const BOOL destroyed = DestroyWindow(window);
        Require(destroyed && matched && monitor.dpi != 0, "oracle DPI HWND mismatch or cleanup failure");
        unsigned int matches = 0;
        for (UINT adapterIndex = 0; adapterIndex <= 64; adapterIndex++)
        {
            ComPtr<IDXGIAdapter1> adapter;
            const HRESULT adapterResult = factory->EnumAdapters1(adapterIndex, adapter.GetAddressOf());
            if (adapterResult == DXGI_ERROR_NOT_FOUND)
            {
                break;
            }
            Require(SUCCEEDED(adapterResult) && adapterIndex < 64, "oracle adapter enumeration failed/overflowed");
            for (UINT outputIndex = 0; outputIndex <= 64; outputIndex++)
            {
                ComPtr<IDXGIOutput> output;
                const HRESULT outputResult = adapter->EnumOutputs(outputIndex, output.GetAddressOf());
                if (outputResult == DXGI_ERROR_NOT_FOUND)
                {
                    break;
                }
                Require(SUCCEEDED(outputResult) && outputIndex < 64, "oracle output enumeration failed/overflowed");
                DXGI_OUTPUT_DESC description{};
                Require(SUCCEEDED(output->GetDesc(&description)), "oracle output descriptor failed");
                if (description.AttachedToDesktop && description.Monitor == monitor.monitor)
                {
                    Require(EqualRect(description.DesktopCoordinates, monitor.rect), "oracle Win32/DXGI physical rectangle disagreement");
                    monitor.rotation = description.Rotation;
                    matches++;
                }
            }
        }
        Require(matches == 1 && monitor.rotation >= DXGI_MODE_ROTATION_IDENTITY && monitor.rotation <= DXGI_MODE_ROTATION_ROTATE270,
                "BLOCKED: unique DXGI rotation is unavailable for an active display");
    }
    Require(factory->IsCurrent() != FALSE, "oracle topology changed during enumeration");
    return monitors;
}

void MovePointer(const POINT point)
{
    const auto width = static_cast<std::int64_t>(GetSystemMetrics(SM_CXVIRTUALSCREEN));
    const auto height = static_cast<std::int64_t>(GetSystemMetrics(SM_CYVIRTUALSCREEN));
    const auto localX = static_cast<std::int64_t>(point.x) - GetSystemMetrics(SM_XVIRTUALSCREEN);
    const auto localY = static_cast<std::int64_t>(point.y) - GetSystemMetrics(SM_YVIRTUALSCREEN);
    // This limit belongs only to the test input device, whose normalized axes
    // have 65536 positions. Never silently round an unrepresentable test pixel.
    Require(width > 0 && height > 0 && width <= 65536 && height <= 65536 && localX >= 0 && localX < width && localY >= 0 && localY < height,
            "BLOCKED: physical test point is outside the representable virtual-desktop input range");
    INPUT input{};
    input.type = INPUT_MOUSE;
    // Aim at the center of the physical pixel. This is Win32 input-device
    // normalization, not DPI scaling and never a capture-coordinate transform.
    input.mi.dx = static_cast<LONG>(((2 * localX + 1) * 65536) / (2 * width));
    input.mi.dy = static_cast<LONG>(((2 * localY + 1) * 65536) / (2 * height));
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE_NOCOALESCE;
    Require(SendInput(1, &input, sizeof(input)) == 1, "mouse move injection blocked");
    // Keep movement and buttons in one ordered SendInput stream, rather than
    // mixing a direct SetPhysicalCursorPos warp with a queued relative move.
    // SendInput acknowledges insertion, not processing: wait for the exact
    // physical point before sending the next event. The caller also waits for
    // the production drag preview, so this cannot hide a wrong selected RECT.
    POINT actual{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    do
    {
        Require(GetPhysicalCursorPos(&actual) != FALSE, "cannot verify injected physical cursor position");
        if (actual.x == point.x && actual.y == point.y)
        {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    RECT clip{};
    const BOOL clipped = GetClipCursor(&clip);
    throw std::runtime_error("physical pointer target is unreachable: requested=" + std::to_string(point.x) + "," + std::to_string(point.y) +
                             "; actual=" + std::to_string(actual.x) + "," + std::to_string(actual.y) + "; clip-valid=" + std::to_string(clipped) + "; clip=" +
                             std::to_string(clip.left) + "," + std::to_string(clip.top) + "," + std::to_string(clip.right) + "," + std::to_string(clip.bottom));
}

void MouseButton(const DWORD flags)
{
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flags;
    Require(SendInput(1, &input, sizeof(input)) == 1, "mouse button injection blocked");
}

void Escape()
{
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = VK_ESCAPE;
    Require(SendInput(1, &input, sizeof(input)) == 1, "Escape down injection blocked");
    input.ki.dwFlags = KEYEVENTF_KEYUP;
    Require(SendInput(1, &input, sizeof(input)) == 1, "Escape up injection blocked");
}

RECT SmallRect(const OracleMonitor& monitor)
{
    const auto width = static_cast<std::int64_t>(monitor.rect.right) - monitor.rect.left;
    const auto height = static_cast<std::int64_t>(monitor.rect.bottom) - monitor.rect.top;
    Require(width >= 100 && height >= 100, "BLOCKED: test ROI does not fit the output");
    return {monitor.rect.left + 16, monitor.rect.top + 24, monitor.rect.left + 80, monitor.rect.top + 72};
}

namespace
{
class Observer final : public NativeObserver
{
public:
    void OnReady(const MonitorSnapshot& value, const std::array<HWND, maximumMonitors>& handles) noexcept override
    {
        const std::lock_guard lock(mutex);
        snapshot = value;
        windows = handles;
        ready = true;
        condition.notify_all();
    }
    void OnPreview(const SelectionPreview& value) noexcept override
    {
        {
            const std::lock_guard lock(mutex);
            preview = value;
            serial++;
            condition.notify_all();
        }
        if (cancelOnAcceptedPreview && !value.dragging && value.hasRectangle && value.validation)
        {
            SendMessageW(windows[0], WM_CANCELMODE, 0, 0);
        }
    }
    template <typename Predicate> void Await(Predicate predicate, const char* message)
    {
        std::unique_lock lock(mutex);
        const bool available = condition.wait_for(lock, std::chrono::seconds(10),
                                                  [&]
                                                  {
                                                      return finished || predicate();
                                                  });
        if (!available || !predicate())
        {
            POINT actual{};
            (void)GetPhysicalCursorPos(&actual);
            throw std::runtime_error(std::string(message) + "; finished=" + std::to_string(finished) + "; serial=" + std::to_string(serial) +
                                     "; dragging=" + std::to_string(preview.dragging) + "; preview=" + std::to_string(preview.physicalRect.left) + "," +
                                     std::to_string(preview.physicalRect.top) + "," + std::to_string(preview.physicalRect.right) + "," +
                                     std::to_string(preview.physicalRect.bottom) + "; cursor=" + std::to_string(actual.x) + "," + std::to_string(actual.y));
        }
    }
    void Finish()
    {
        const std::lock_guard lock(mutex);
        finished = true;
        condition.notify_all();
    }
    std::uint64_t Serial()
    {
        const std::lock_guard lock(mutex);
        return serial;
    }
    std::mutex mutex;
    std::condition_variable condition;
    MonitorSnapshot snapshot;
    std::array<HWND, maximumMonitors> windows{};
    SelectionPreview preview;
    std::uint64_t serial = 0;
    bool ready = false;
    bool finished = false;
    bool cancelOnAcceptedPreview = false;
};

class ControlWindow
{
public:
    explicit ControlWindow(const bool detachInputContext = true)
    {
        Require(swprintf_s(className_.data(), className_.size(), L"PB.Region.Control.%p", static_cast<void*>(this)) > 0, "control class name failed");
        WNDCLASSW description{};
        description.hInstance = GetModuleHandleW(nullptr);
        description.lpfnWndProc = &Procedure;
        description.lpszClassName = className_.data();
        Require(RegisterClassW(&description) != 0, "control class registration failed");
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW, className_.data(), L"Region gate focus/capture control", WS_POPUP, 8, 8, 16, 16, nullptr, nullptr,
                                  description.hInstance, this);
        if (window_ == nullptr)
        {
            UnregisterClassW(className_.data(), description.hInstance);
            throw std::runtime_error("control window creation failed");
        }
        if (detachInputContext && !ImmAssociateContextEx(window_, nullptr, 0))
        {
            DestroyWindow(window_);
            UnregisterClassW(className_.data(), description.hInstance);
            throw std::runtime_error("non-text control window IME detachment failed");
        }
    }
    ~ControlWindow()
    {
        DestroyWindow(window_);
        UnregisterClassW(className_.data(), GetModuleHandleW(nullptr));
    }
    ControlWindow(const ControlWindow&) = delete;
    ControlWindow& operator=(const ControlWindow&) = delete;
    HWND Window() const noexcept
    {
        return window_;
    }
    bool captureStolen = false;
    bool focusTaken = false;

private:
    static LRESULT CALLBACK Procedure(const HWND window, const UINT message, const WPARAM word, const LPARAM parameter)
    {
        auto* control = reinterpret_cast<ControlWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(parameter);
            control = static_cast<ControlWindow*>(creation->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(control));
        }
        if (control != nullptr && message == WM_APP + 1)
        {
            SetCapture(window);
            control->captureStolen = GetCapture() == window;
            ReleaseCapture();
            return 0;
        }
        if (control != nullptr && message == WM_APP + 2)
        {
            ShowWindow(window, SW_SHOW);
            (void)SetForegroundWindow(window);
            control->focusTaken = GetForegroundWindow() == window;
            return 0;
        }
        return DefWindowProcW(window, message, word, parameter);
    }
    HWND window_ = nullptr;
    std::array<wchar_t, 80> className_{};
};

void Drag(Observer& observer, const POINT first, const POINT last, const RECT expected, const bool rejected)
{
    const auto before = observer.Serial();
    MovePointer(first);
    MouseButton(MOUSEEVENTF_LEFTDOWN);
    observer.Await(
        [&]
        {
            return observer.serial > before && observer.preview.dragging;
        },
        "no down acknowledgment from actual window input");
    const auto afterDown = observer.Serial();
    MovePointer(last);
    observer.Await(
        [&]
        {
            return observer.serial > afterDown && observer.preview.dragging && observer.preview.hasRectangle &&
                   EqualRect(observer.preview.physicalRect, expected);
        },
        "physical drag preview mismatch");
    const auto afterMove = observer.Serial();
    MouseButton(MOUSEEVENTF_LEFTUP);
    if (rejected)
    {
        observer.Await(
            [&]
            {
                return observer.serial > afterMove && !observer.preview.dragging && observer.preview.validation.code == ScreenRegionErrorCode::NotSingleMonitor;
            },
            "cross-monitor ROI was not rejected");
    }
}

void VerifyClean(const NativeDiagnostics& diagnostics, const Observer* observer = nullptr)
{
    Require(diagnostics.liveWindows == 0 && diagnostics.liveFonts == 0 && diagnostics.livePens == 0 && diagnostics.liveClasses == 0 &&
                !diagnostics.ownsCapture && GetCapture() == nullptr,
            "native window/GDI/capture cleanup failed");
    if (observer != nullptr)
    {
        for (const HWND window : observer->windows)
        {
            Require(window == nullptr || !IsWindow(window), "overlay survived the public result");
        }
    }
}

enum class Scenario
{
    Forward,
    Reverse,
    WholeMonitor,
    CrossRetry,
    EscapeCancel,
    RightCancel,
    CancelMode,
    CaptureLost,
    DisplayChange,
    Quit,
    FocusLoss,
    LateCancel
};

ScreenCaptureRegion RunSession(const std::vector<OracleMonitor>& oracle, const std::size_t monitorIndex, const Scenario scenario, Evidence& evidence)
{
    Observer observer;
    observer.cancelOnAcceptedPreview = scenario == Scenario::LateCancel;
    const auto control = scenario == Scenario::FocusLoss || scenario == Scenario::CaptureLost ? std::make_unique<ControlWindow>() : nullptr;
    NativeDiagnostics diagnostics;
    const auto backend = MakeNativeBackend({ScreenRegionStage::None, 1, &diagnostics, &observer});
    const DWORD ownerThread = GetCurrentThreadId();
    std::exception_ptr inputFailure;
    const RECT expected = scenario == Scenario::WholeMonitor ? oracle[monitorIndex].rect : SmallRect(oracle[monitorIndex]);
    std::thread driver(
        [&]
        {
            try
            {
                observer.Await(
                    [&]
                    {
                        return observer.ready && observer.serial != 0;
                    },
                    "selector did not create its real overlay");
                for (std::size_t index = 0; index < observer.snapshot.count; index++)
                {
                    RECT actual{};
                    Require(GetWindowRect(observer.windows[index], &actual) && EqualRect(actual, observer.snapshot.monitors[index].physicalRect),
                            "real overlay physical bounds mismatch");
                    Require(AreDpiAwarenessContextsEqual(GetWindowDpiAwarenessContext(observer.windows[index]), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2),
                            "overlay is not actually PMv2");
                    const HIMC context = ImmGetContext(observer.windows[index]);
                    if (context != nullptr)
                    {
                        ImmReleaseContext(observer.windows[index], context);
                    }
                    Require(context == nullptr, "non-text overlay retained an IME context");
                }
                if (scenario == Scenario::CrossRetry)
                {
                    Require(oracle.size() >= 2, "BLOCKED: cross-monitor live gate needs two displays");
                    const RECT first = SmallRect(oracle[0]);
                    const RECT second = SmallRect(oracle[1]);
                    const POINT start{first.left, first.top};
                    const POINT finish{second.right - 1, second.bottom - 1};
                    const RECT crossing{(std::min)(start.x, finish.x), (std::min)(start.y, finish.y), (std::max)(start.x, finish.x) + 1,
                                        (std::max)(start.y, finish.y) + 1};
                    Drag(observer, start, finish, crossing, true);
                }
                if (scenario == Scenario::Forward || scenario == Scenario::WholeMonitor || scenario == Scenario::CrossRetry || scenario == Scenario::LateCancel)
                {
                    Drag(observer, {expected.left, expected.top}, {expected.right - 1, expected.bottom - 1}, expected, false);
                }
                else if (scenario == Scenario::Reverse)
                {
                    Drag(observer, {expected.right - 1, expected.bottom - 1}, {expected.left, expected.top}, expected, false);
                }
                else
                {
                    const auto serial = observer.Serial();
                    MovePointer({expected.left, expected.top});
                    MouseButton(MOUSEEVENTF_LEFTDOWN);
                    observer.Await(
                        [&]
                        {
                            return observer.serial > serial && observer.preview.dragging;
                        },
                        "no drag before cancellation");
                    const HWND window = observer.windows[0];
                    switch (scenario)
                    {
                    case Scenario::EscapeCancel:
                        Escape();
                        break;
                    case Scenario::RightCancel:
                        MouseButton(MOUSEEVENTF_RIGHTDOWN);
                        MouseButton(MOUSEEVENTF_RIGHTUP);
                        break;
                    case Scenario::CancelMode:
                        Require(PostMessageW(window, WM_CANCELMODE, 0, 0), "WM_CANCELMODE post failed");
                        break;
                    case Scenario::CaptureLost:
                        Require(PostMessageW(control->Window(), WM_APP + 1, 0, 0), "capture-steal command failed");
                        break;
                    case Scenario::FocusLoss:
                        Require(PostMessageW(control->Window(), WM_APP + 2, 0, 0), "focus-transfer command failed");
                        break;
                    case Scenario::DisplayChange:
                        Require(PostMessageW(window, WM_DISPLAYCHANGE, 32, 0), "display-change notification failed");
                        break;
                    case Scenario::Quit:
                        Require(PostThreadMessageW(ownerThread, WM_QUIT, 37, 0), "WM_QUIT post failed");
                        break;
                    default:
                        break;
                    }
                    MouseButton(MOUSEEVENTF_LEFTUP);
                }
            }
            catch (...)
            {
                inputFailure = std::current_exception();
                try
                {
                    MouseButton(MOUSEEVENTF_LEFTUP);
                    MouseButton(MOUSEEVENTF_RIGHTUP);
                }
                catch (...)
                {
                }
                // Failure containment only; this cannot turn an input failure into
                // a passing test. The owner reposts WM_QUIT and the gate consumes it.
                (void)PostThreadMessageW(ownerThread, WM_QUIT, 91, 0);
            }
        });
    ScreenCaptureRegion output;
    output.dpiX = 777;
    const auto status = RunSelection(*backend, output);
    observer.Finish();
    driver.join();
    MSG quit{};
    const bool hadQuit = PeekMessageW(&quit, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE) != FALSE;
    VerifyClean(diagnostics, &observer);
    if (inputFailure)
    {
        std::rethrow_exception(inputFailure);
    }
    if (scenario == Scenario::CaptureLost)
    {
        Require(control->captureStolen, "capture loss was not produced by actual SetCapture");
    }
    if (scenario == Scenario::FocusLoss)
    {
        Require(control->focusTaken, "focus loss was not produced by actual foreground activation");
    }
    const bool accepted =
        scenario == Scenario::Forward || scenario == Scenario::Reverse || scenario == Scenario::WholeMonitor || scenario == Scenario::CrossRetry;
    if (accepted)
    {
        Require(static_cast<bool>(status), "selection failed: " + Describe(status));
        const auto& monitor = oracle[monitorIndex];
        Require(EqualRect(output.physicalRect, expected) && EqualRect(output.monitorPhysicalRect, monitor.rect) && output.monitor == monitor.monitor &&
                    output.dpiX == monitor.dpi && output.dpiY == monitor.dpi && output.rotation == monitor.rotation,
                "independent oracle mismatch");
        evidence.Record("accepted", output);
    }
    else
    {
        const auto expectedCode = scenario == Scenario::DisplayChange ? ScreenRegionErrorCode::DisplayChanged : ScreenRegionErrorCode::Cancelled;
        Require(status.code == expectedCode && output.dpiX == 777 && output.monitor == nullptr,
                "cancellation/error did not preserve output: " + Describe(status));
        evidence.Note("negative=" + std::to_string(static_cast<unsigned int>(scenario)) + " result=" + Describe(status));
    }
    Require(hadQuit == (scenario == Scenario::Quit) && (!hadQuit || quit.wParam == 37), "WM_QUIT was lost or unexpectedly introduced");
    return output;
}

void RunNativeGate(Evidence& evidence)
{
    RestorePointer cursor;
    const ControlWindow caller(false);
    const auto inputContext = ImmGetContext(caller.Window());
    if (inputContext != nullptr)
    {
        Require(ImmReleaseContext(caller.Window(), inputContext), "caller input context release failed");
    }
    const auto keyboardLayout = GetKeyboardLayout(0);
    const auto before = ReadOracle();
    Require(before.size() >= 2, "BLOCKED: this live gate requires at least two active displays");
    for (const auto& monitor : before)
    {
        evidence.Record("oracle-before", {monitor.monitor, monitor.rect, monitor.rect, monitor.dpi, monitor.dpi, monitor.rotation});
        ScreenCaptureRegion resolved;
        const auto status = ResolveScreenCaptureRegion(SmallRect(monitor), resolved);
        Require(static_cast<bool>(status), "public resolve failed: " + Describe(status));
        Require(resolved.monitor == monitor.monitor && resolved.dpiX == monitor.dpi && resolved.dpiY == monitor.dpi && resolved.rotation == monitor.rotation,
                "public resolve metadata mismatch");
    }
    for (std::size_t index = 0; index < before.size(); index++)
    {
        (void)RunSession(before, index, Scenario::Forward, evidence);
        (void)RunSession(before, index, Scenario::Reverse, evidence);
    }
    (void)RunSession(before, 0, Scenario::WholeMonitor, evidence);
    (void)RunSession(before, 0, Scenario::CrossRetry, evidence);
    for (const auto scenario : {Scenario::EscapeCancel, Scenario::RightCancel, Scenario::CancelMode, Scenario::CaptureLost, Scenario::DisplayChange,
                                Scenario::Quit, Scenario::FocusLoss, Scenario::LateCancel})
    {
        (void)RunSession(before, 0, scenario, evidence);
    }
    const std::array<ScreenRegionStage, 11> faults{ScreenRegionStage::DpiAwareness, ScreenRegionStage::MonitorEnumeration,
                                                   ScreenRegionStage::DpiQuery,     ScreenRegionStage::OutputQuery,
                                                   ScreenRegionStage::WindowClass,  ScreenRegionStage::Window,
                                                   ScreenRegionStage::OverlayStyle, ScreenRegionStage::Font,
                                                   ScreenRegionStage::Paint,        ScreenRegionStage::MessageWait,
                                                   ScreenRegionStage::InputContext};
    for (const auto stage : faults)
    {
        for (const std::size_t occurrence : {std::size_t{1}, std::size_t{2}})
        {
            if (occurrence == 2 && stage != ScreenRegionStage::Window && stage != ScreenRegionStage::OverlayStyle && stage != ScreenRegionStage::Font &&
                stage != ScreenRegionStage::DpiQuery && stage != ScreenRegionStage::InputContext)
            {
                continue;
            }
            NativeDiagnostics diagnostics;
            const auto backend = MakeNativeBackend({stage, occurrence, &diagnostics, nullptr});
            ScreenCaptureRegion output;
            output.dpiX = 777;
            const auto status = RunSelection(*backend, output);
            Require(status.code == ScreenRegionErrorCode::NativeFailure && status.stage == stage && output.dpiX == 777 && output.monitor == nullptr,
                    "fault stage did not fail closed: " + Describe(status));
            VerifyClean(diagnostics);
            evidence.Note("injected-failure stage=" + std::to_string(static_cast<unsigned int>(stage)) + " occurrence=" + std::to_string(occurrence));
        }
    }
    NativeDiagnostics cleanupDiagnostics;
    const auto failingCleanup = MakeNativeBackend({ScreenRegionStage::Cleanup, 1, &cleanupDiagnostics, nullptr});
    ScreenCaptureRegion output;
    output.dpiX = 777;
    const auto cleanupStatus = RunResolve(*failingCleanup, SmallRect(before[0]), output);
    Require(cleanupStatus.code == ScreenRegionErrorCode::NativeFailure && cleanupStatus.stage == ScreenRegionStage::Cleanup && output.dpiX == 777,
            "cleanup failure published a result");
    VerifyClean(cleanupDiagnostics);
    const auto after = ReadOracle();
    Require(after.size() == before.size(), "display count changed");
    for (const auto& monitor : before)
    {
        const auto found = std::find_if(after.begin(), after.end(),
                                        [&](const auto& current)
                                        {
                                            return current.monitor == monitor.monitor;
                                        });
        Require(found != after.end() && EqualRect(found->rect, monitor.rect) && found->dpi == monitor.dpi && found->rotation == monitor.rotation,
                "display topology/DPI/rotation changed");
        evidence.Record("oracle-after", {found->monitor, found->rect, found->rect, found->dpi, found->dpi, found->rotation});
    }
    cursor.Restore();
    const auto contextAfter = ImmGetContext(caller.Window());
    if (contextAfter != nullptr)
    {
        Require(ImmReleaseContext(caller.Window(), contextAfter), "caller input context release failed");
    }
    Require(contextAfter == inputContext && GetKeyboardLayout(0) == keyboardLayout, "selector changed the caller's IME or keyboard layout");
    evidence.Note("PASS: real physical input, current monitor metadata, cancellation, partial initialization and cleanup; no display settings changed");
}

void RunCancelRegression(Evidence& evidence)
{
    RestorePointer cursor;
    const auto oracle = ReadOracle();
    (void)RunSession(oracle, 0, Scenario::LateCancel, evidence);
    cursor.Restore();
}

} // namespace
} // namespace screenregiongate

int wmain(const int argumentCount, wchar_t* arguments[])
{
    try
    {
        if (argumentCount == 3 && std::wstring_view(arguments[1]) == L"--cancel-regression")
        {
            screenregiongate::Evidence evidence(arguments[2], "cancel-regression");
            screenregiongate::RunCancelRegression(evidence);
            return 0;
        }
        if (argumentCount == 3 && std::wstring_view(arguments[1]) == L"--native")
        {
            screenregiongate::RunSupervisedNativeGate(arguments[2]);
            return 0;
        }
        if (argumentCount == 3 && std::wstring_view(arguments[1]) == L"--native-worker")
        {
            screenregiongate::Evidence evidence(arguments[2], "native");
            screenregiongate::RunNativeGate(evidence);
            return 0;
        }
        if (argumentCount == 4 && std::wstring_view(arguments[1]) == L"--decoder")
        {
            screenregiongate::Evidence evidence(arguments[3], "decoder");
            screenregiongate::RunCliGate(arguments[2], evidence);
            return 0;
        }
        return 2;
    }
    catch (const std::exception& error)
    {
        std::cerr << "SCREEN REGION GATE FAILED: " << error.what() << '\n';
        return 1;
    }
}
