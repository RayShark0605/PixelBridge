#include "screen_region_internal.h"

#include <imm.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <limits>

namespace pbscreenregion::detail
{
namespace
{
using Microsoft::WRL::ComPtr;

ScreenRegionStatus NativeError(const ScreenRegionStage stage, const std::int32_t error) noexcept
{
    return ScreenRegionStatus::Failure(ScreenRegionErrorCode::NativeFailure, stage, error);
}

ScreenRegionStatus LastError(const ScreenRegionStage stage) noexcept
{
    const DWORD error = GetLastError();
    return NativeError(stage, static_cast<std::int32_t>(error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error));
}

struct LayoutContext
{
    MonitorSnapshot snapshot;
    ScreenRegionStatus status;
};

BOOL CALLBACK EnumerateMonitor(const HMONITOR monitor, HDC, LPRECT, const LPARAM parameter) noexcept
{
    auto& context = *reinterpret_cast<LayoutContext*>(parameter);
    if (context.snapshot.count == maximumMonitors)
    {
        context.status = ScreenRegionStatus::Failure(ScreenRegionErrorCode::ResourceLimit, ScreenRegionStage::MonitorEnumeration);
        return FALSE;
    }
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info))
    {
        context.status = LastError(ScreenRegionStage::MonitorEnumeration);
        return FALSE;
    }
    // CW_USEDEFAULT must never reinterpret a physical desktop origin.
    if (!ValidatePhysicalRect(info.rcMonitor) || info.rcMonitor.left == CW_USEDEFAULT || info.rcMonitor.top == CW_USEDEFAULT)
    {
        context.status = ScreenRegionStatus::Failure(ScreenRegionErrorCode::MetadataUnavailable, ScreenRegionStage::Geometry);
        return FALSE;
    }
    auto& entry = context.snapshot.monitors[context.snapshot.count];
    entry.monitor = monitor;
    entry.physicalRect = info.rcMonitor;
    std::copy_n(info.szDevice, entry.deviceName.size(), entry.deviceName.data());
    context.snapshot.count++;
    return TRUE;
}

ScreenRegionStatus ReadLayout(MonitorSnapshot& snapshot) noexcept
{
    LayoutContext context;
    if (!EnumDisplayMonitors(nullptr, nullptr, &EnumerateMonitor, reinterpret_cast<LPARAM>(&context)))
    {
        return context.status ? LastError(ScreenRegionStage::MonitorEnumeration) : context.status;
    }
    if (context.snapshot.count == 0)
    {
        return ScreenRegionStatus::Failure(ScreenRegionErrorCode::MetadataUnavailable, ScreenRegionStage::MonitorEnumeration);
    }
    snapshot = context.snapshot;
    return ScreenRegionStatus::Success();
}

bool EqualLayout(const MonitorSnapshot& first, const MonitorSnapshot& second) noexcept
{
    if (first.count != second.count || first.count > maximumMonitors)
    {
        return false;
    }
    for (std::size_t index = 0; index < first.count; index++)
    {
        bool found = false;
        for (std::size_t other = 0; other < second.count; other++)
        {
            const auto& original = first.monitors[index];
            const auto& current = second.monitors[other];
            if (original.monitor == current.monitor && EqualRect(original.physicalRect, current.physicalRect) && original.deviceName == current.deviceName)
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            return false;
        }
    }
    return true;
}

class NativeBackend final : public ScreenRegionBackend
{
public:
    explicit NativeBackend(const NativeBackendOptions& options) noexcept : options_(options)
    {
    }
    ~NativeBackend() override
    {
        (void)Close();
    }
    NativeBackend(const NativeBackend&) = delete;
    NativeBackend& operator=(const NativeBackend&) = delete;

    ScreenRegionStatus CheckAwareness() noexcept override
    {
        if (!AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        {
            return ScreenRegionStatus::Failure(ScreenRegionErrorCode::DpiAwarenessRequired, ScreenRegionStage::DpiAwareness);
        }
        // A same-process "process DPI" query can return the overridden thread
        // context on Windows. Require the host's PMv2 manifest as well, rather
        // than accepting an unaware host patched with SetThreadDpiAwarenessContext.
        std::array<wchar_t, 256> setting{};
        if (!QueryActCtxSettingsW(0, nullptr, L"http://schemas.microsoft.com/SMI/2016/WindowsSettings", L"dpiAwareness", setting.data(), setting.size(),
                                  nullptr))
        {
            const DWORD error = GetLastError();
            return ScreenRegionStatus::Failure(error == ERROR_INSUFFICIENT_BUFFER ? ScreenRegionErrorCode::ResourceLimit
                                                                                  : ScreenRegionErrorCode::DpiAwarenessRequired,
                                               ScreenRegionStage::DpiAwareness, static_cast<std::int32_t>(error));
        }
        const auto end = std::find(setting.begin(), setting.end(), L'\0');
        if (end == setting.end() || !DeclaresPerMonitorV2(std::wstring_view(setting.data(), static_cast<std::size_t>(end - setting.begin()))))
        {
            return ScreenRegionStatus::Failure(ScreenRegionErrorCode::DpiAwarenessRequired, ScreenRegionStage::DpiAwareness);
        }
        return Fault(ScreenRegionStage::DpiAwareness);
    }

    ScreenRegionStatus ReadTopology(MonitorSnapshot& snapshot) noexcept override
    {
        auto status = CheckAwareness();
        if (status)
        {
            status = PendingInterruptions();
        }
        MonitorSnapshot candidate;
        if (status)
        {
            status = ReadLayout(candidate);
        }
        if (status)
        {
            status = Fault(ScreenRegionStage::MonitorEnumeration);
        }
        ComPtr<IDXGIFactory1> factory;
        if (status)
        {
            const HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()));
            if (FAILED(result))
            {
                status = NativeError(ScreenRegionStage::OutputQuery, result);
            }
        }
        if (status)
        {
            status = ReadRotations(*factory.Get(), candidate);
        }
        for (std::size_t index = 0; status && index < candidate.count; index++)
        {
            status = ReadDpi(candidate.monitors[index]);
        }
        MonitorSnapshot verified;
        if (status)
        {
            status = ReadLayout(verified);
        }
        if (status && (!factory->IsCurrent() || !EqualLayout(candidate, verified)))
        {
            status = ScreenRegionStatus::Failure(ScreenRegionErrorCode::DisplayChanged, ScreenRegionStage::Revalidation);
        }
        if (status)
        {
            status = ValidateTopology(candidate);
        }
        if (status)
        {
            status = PendingInterruptions();
        }
        if (status)
        {
            snapshot = candidate;
        }
        return status;
    }

    ScreenRegionStatus OpenOverlay(const MonitorSnapshot& snapshot) noexcept override
    {
        auto status = ValidateTopology(snapshot);
        if (!status)
        {
            return status;
        }
        topology_ = snapshot;
        instance_ = GetModuleHandleW(nullptr);
        if (instance_ == nullptr ||
            swprintf_s(className_.data(), className_.size(), L"PixelBridge.Region.%lu.%p", GetCurrentProcessId(), static_cast<void*>(this)) <= 0)
        {
            return LastError(ScreenRegionStage::WindowClass);
        }
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &WindowProcedure;
        windowClass.hInstance = instance_;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        windowClass.lpszClassName = className_.data();
        if (windowClass.hCursor == nullptr || RegisterClassExW(&windowClass) == 0)
        {
            return LastError(ScreenRegionStage::WindowClass);
        }
        classRegistered_ = true;
        diagnostics_.liveClasses++;
        status = Fault(ScreenRegionStage::WindowClass);
        validPen_ = CreatePen(PS_SOLID, 2, RGB(80, 255, 120));
        invalidPen_ = CreatePen(PS_SOLID, 2, RGB(255, 100, 90));
        diagnostics_.livePens = static_cast<std::size_t>(validPen_ != nullptr) + static_cast<std::size_t>(invalidPen_ != nullptr);
        if (status && (validPen_ == nullptr || invalidPen_ == nullptr))
        {
            status = LastError(ScreenRegionStage::Paint);
        }
        for (std::size_t index = 0; status && index < snapshot.count; index++)
        {
            const auto& monitor = snapshot.monitors[index];
            const RECT rect = monitor.physicalRect;
            const auto width = static_cast<int>(static_cast<std::int64_t>(rect.right) - rect.left);
            const auto height = static_cast<int>(static_cast<std::int64_t>(rect.bottom) - rect.top);
            HWND& window = windows_[index];
            window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED, className_.data(), L"PixelBridge Region Selector | ready", WS_POPUP,
                                     rect.left, rect.top, width, height, nullptr, nullptr, instance_, this);
            if (window == nullptr)
            {
                return LastError(ScreenRegionStage::Window);
            }
            WindowCreated();
            status = Fault(ScreenRegionStage::Window);
            if (status)
            {
                // A mouse/Escape-only overlay has no text input. Detach just
                // this window before activation; do not disable the thread's
                // IME or mutate its shared/default input context.
                status = ImmAssociateContextEx(window, nullptr, 0) ? Fault(ScreenRegionStage::InputContext)
                                                                   : NativeError(ScreenRegionStage::InputContext, ERROR_GEN_FAILURE);
            }
            RECT actual{};
            if (status && (!GetWindowRect(window, &actual) || !EqualRect(rect, actual) || MonitorFromWindow(window, MONITOR_DEFAULTTONULL) != monitor.monitor ||
                           !AreDpiAwarenessContextsEqual(GetWindowDpiAwarenessContext(window), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)))
            {
                status = ScreenRegionStatus::Failure(ScreenRegionErrorCode::DisplayChanged, ScreenRegionStage::Window);
            }
            if (status && !SetLayeredWindowAttributes(window, 0, 176, LWA_ALPHA))
            {
                status = LastError(ScreenRegionStage::OverlayStyle);
            }
            if (status)
            {
                status = Fault(ScreenRegionStage::OverlayStyle);
            }
            const UINT dpi = GetDpiForWindow(window);
            if (status && (dpi != monitor.dpiX || dpi != monitor.dpiY))
            {
                status = ScreenRegionStatus::Failure(ScreenRegionErrorCode::DisplayChanged, ScreenRegionStage::DpiQuery);
            }
            const auto fontHeight = (static_cast<std::uint64_t>(dpi) * 18 + 95) / 96;
            if (status && (fontHeight == 0 || fontHeight > 512))
            {
                status = ScreenRegionStatus::Failure(ScreenRegionErrorCode::ResourceLimit, ScreenRegionStage::Font);
            }
            if (status)
            {
                fonts_[index] = CreateFontW(-static_cast<int>(fontHeight), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                            CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
                if (fonts_[index] == nullptr)
                {
                    status = LastError(ScreenRegionStage::Font);
                }
                else
                {
                    diagnostics_.liveFonts++;
                    status = Fault(ScreenRegionStage::Font);
                }
            }
        }
        if (!status)
        {
            return status;
        }
        if (SetTimer(windows_[0], 1, 1000, nullptr) == 0)
        {
            return LastError(ScreenRegionStage::MessageWait);
        }
        for (std::size_t index = 0; index < snapshot.count; index++)
        {
            ShowWindow(windows_[index], SW_SHOWNOACTIVATE);
        }
        ready_ = true;
        // Focus acquisition is a best-effort UI request, not an admission
        // fallback. Windows can deny foreground activation; clicking the overlay
        // activates it normally. No input is accepted from Qt/logical coordinates.
        (void)SetForegroundWindow(windows_[0]);
        activated_ = IsOwnWindow(GetForegroundWindow());
        if (options_.observer != nullptr)
        {
            options_.observer->OnReady(topology_, windows_);
        }
        return pendingError_;
    }

    ScreenRegionStatus GetEvent(SelectionEvent& event) noexcept override
    {
        const auto injected = Fault(ScreenRegionStage::MessageWait);
        if (!injected)
        {
            return injected;
        }
        while (pendingError_)
        {
            if (eventCount_ != 0)
            {
                event = events_[eventHead_];
                eventHead_ = (eventHead_ + 1) % events_.size();
                eventCount_--;
                return ScreenRegionStatus::Success();
            }
            MSG message{};
            const BOOL result = GetMessageW(&message, nullptr, 0, 0);
            if (result == -1)
            {
                return LastError(ScreenRegionStage::MessageWait);
            }
            if (result == 0)
            {
                quitCode_ = static_cast<int>(message.wParam);
                repostQuit_ = true;
                event = {SelectionEventType::Cancel, {}};
                return ScreenRegionStatus::Success();
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return pendingError_;
    }

    ScreenRegionStatus AcquirePointer(const POINT physicalPoint) noexcept override
    {
        for (std::size_t index = 0; index < topology_.count; index++)
        {
            const RECT rect = topology_.monitors[index].physicalRect;
            if (physicalPoint.x >= rect.left && physicalPoint.x < rect.right && physicalPoint.y >= rect.top && physicalPoint.y < rect.bottom)
            {
                captureWindow_ = windows_[index];
                SetCapture(captureWindow_);
                ownsCapture_ = GetCapture() == captureWindow_;
                diagnostics_.ownsCapture = ownsCapture_;
                if (!ownsCapture_)
                {
                    return NativeError(ScreenRegionStage::Capture, ERROR_ACCESS_DENIED);
                }
                return Fault(ScreenRegionStage::Capture);
            }
        }
        return ScreenRegionStatus::Failure(ScreenRegionErrorCode::NotSingleMonitor, ScreenRegionStage::Capture);
    }

    ScreenRegionStatus ReleasePointer() noexcept override
    {
        if (!ownsCapture_ || GetCapture() != captureWindow_)
        {
            ownsCapture_ = false;
            diagnostics_.ownsCapture = false;
            return ScreenRegionStatus::Failure(ScreenRegionErrorCode::Cancelled, ScreenRegionStage::Capture);
        }
        // ReleaseCapture synchronously sends WM_CAPTURECHANGED. Change ownership
        // first so this expected notification cannot cancel an accepted drag.
        ownsCapture_ = false;
        diagnostics_.ownsCapture = false;
        return ReleaseCapture() ? ScreenRegionStatus::Success() : LastError(ScreenRegionStage::Capture);
    }

    ScreenRegionStatus Draw(const SelectionPreview& preview) noexcept override
    {
        preview_ = preview;
        const wchar_t* const title = preview.dragging      ? L"PixelBridge Region Selector | dragging"
                                     : !preview.validation ? L"PixelBridge Region Selector | invalid"
                                                           : L"PixelBridge Region Selector | ready";
        for (std::size_t index = 0; index < topology_.count; index++)
        {
            if (!SetWindowTextW(windows_[index], title) || !InvalidateRect(windows_[index], nullptr, FALSE) || !UpdateWindow(windows_[index]))
            {
                return LastError(ScreenRegionStage::Paint);
            }
        }
        if (pendingError_)
        {
            pendingError_ = Fault(ScreenRegionStage::Paint);
        }
        if (pendingError_ && options_.observer != nullptr)
        {
            options_.observer->OnPreview(preview);
        }
        return pendingError_;
    }

    ScreenRegionStatus Close() noexcept override
    {
        if (closed_)
        {
            return cleanupStatus_;
        }
        cleanupStatus_ = PendingInterruptions();
        MSG quit{};
        if (ready_ && !repostQuit_ && PeekMessageW(&quit, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE))
        {
            quitCode_ = static_cast<int>(quit.wParam);
            repostQuit_ = true;
            cleanupStatus_ = ScreenRegionStatus::Failure(ScreenRegionErrorCode::Cancelled, ScreenRegionStage::Input);
        }
        // PeekMessage can itself dispatch incoming sent messages even when the
        // WM_QUIT filter finds nothing. Include their terminal events too.
        if (cleanupStatus_)
        {
            cleanupStatus_ = PendingInterruptions();
        }
        closed_ = true;
        ready_ = false;
        if (captureWindow_ != nullptr && GetCapture() == captureWindow_)
        {
            ownsCapture_ = false;
            if (!ReleaseCapture())
            {
                cleanupStatus_ = LastError(ScreenRegionStage::Cleanup);
            }
        }
        ownsCapture_ = false;
        diagnostics_.ownsCapture = false;
        for (std::size_t index = 0; index < windows_.size(); index++)
        {
            if (windows_[index] != nullptr)
            {
                (void)KillTimer(windows_[index], 1);
                // Even an unexpected OS destruction failure cannot leave a
                // window procedure with a pointer to the destroyed backend.
                SetWindowLongPtrW(windows_[index], GWLP_USERDATA, 0);
                if (!DestroyWindow(windows_[index]))
                {
                    cleanupStatus_ = LastError(ScreenRegionStage::Cleanup);
                }
                else
                {
                    diagnostics_.liveWindows--;
                }
                windows_[index] = nullptr;
            }
            if (fonts_[index] != nullptr)
            {
                if (!DeleteObject(fonts_[index]))
                {
                    cleanupStatus_ = LastError(ScreenRegionStage::Cleanup);
                }
                else
                {
                    diagnostics_.liveFonts--;
                }
                fonts_[index] = nullptr;
            }
        }
        for (HPEN* const pen : {&validPen_, &invalidPen_})
        {
            if (*pen != nullptr)
            {
                if (!DeleteObject(*pen))
                {
                    cleanupStatus_ = LastError(ScreenRegionStage::Cleanup);
                }
                else
                {
                    diagnostics_.livePens--;
                }
                *pen = nullptr;
            }
        }
        if (classRegistered_)
        {
            if (!UnregisterClassW(className_.data(), instance_))
            {
                cleanupStatus_ = LastError(ScreenRegionStage::Cleanup);
            }
            else
            {
                diagnostics_.liveClasses--;
            }
            classRegistered_ = false;
        }
        if (cleanupStatus_)
        {
            cleanupStatus_ = Fault(ScreenRegionStage::Cleanup);
        }
        if (options_.diagnostics != nullptr)
        {
            *options_.diagnostics = diagnostics_;
        }
        if (repostQuit_)
        {
            PostQuitMessage(quitCode_);
            repostQuit_ = false;
        }
        return cleanupStatus_;
    }

private:
    ScreenRegionStatus PendingInterruptions() const noexcept
    {
        if (!pendingError_)
        {
            return pendingError_;
        }
        // SendMessage/paint/focus callbacks can enqueue a terminal event during
        // Draw or a native metadata query, after the mouse-up event was consumed.
        // Such an observed cancellation must win before the result is committed.
        for (std::size_t index = 0; index < eventCount_; index++)
        {
            const auto type = events_[(eventHead_ + index) % events_.size()].type;
            if (type == SelectionEventType::Cancel)
            {
                return ScreenRegionStatus::Failure(ScreenRegionErrorCode::Cancelled, ScreenRegionStage::Input);
            }
            if (type == SelectionEventType::DisplayChanged)
            {
                return ScreenRegionStatus::Failure(ScreenRegionErrorCode::DisplayChanged, ScreenRegionStage::Revalidation);
            }
        }
        return ScreenRegionStatus::Success();
    }

    ScreenRegionStatus Fault(const ScreenRegionStage stage) noexcept
    {
        if (options_.failAfterStage == stage)
        {
            faultVisits_++;
            if (faultVisits_ == options_.failOccurrence)
            {
                return NativeError(stage, ERROR_GEN_FAILURE);
            }
        }
        return ScreenRegionStatus::Success();
    }

    void WindowCreated() noexcept
    {
        diagnostics_.liveWindows++;
        if (diagnostics_.createdWindows != (std::numeric_limits<std::size_t>::max)())
        {
            diagnostics_.createdWindows++;
        }
    }

    ScreenRegionStatus ReadDpi(MonitorMetadata& monitor) noexcept
    {
        HWND window = nullptr;
        for (std::size_t index = 0; index < topology_.count; index++)
        {
            if (monitor.monitor == topology_.monitors[index].monitor)
            {
                window = windows_[index];
                break;
            }
        }
        const bool temporary = window == nullptr;
        if (temporary)
        {
            window = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"PixelBridge DPI query", WS_POPUP, monitor.physicalRect.left, monitor.physicalRect.top, 1, 1,
                                     nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (window == nullptr)
            {
                return LastError(ScreenRegionStage::DpiQuery);
            }
            WindowCreated();
        }
        ScreenRegionStatus status;
        const UINT dpi = GetDpiForWindow(window);
        if (dpi == 0 || MonitorFromWindow(window, MONITOR_DEFAULTTONULL) != monitor.monitor ||
            !AreDpiAwarenessContextsEqual(GetWindowDpiAwarenessContext(window), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        {
            status = ScreenRegionStatus::Failure(ScreenRegionErrorCode::MetadataUnavailable, ScreenRegionStage::DpiQuery);
        }
        if (status)
        {
            status = Fault(ScreenRegionStage::DpiQuery);
        }
        if (temporary)
        {
            if (!DestroyWindow(window))
            {
                status = LastError(ScreenRegionStage::Cleanup);
            }
            else
            {
                diagnostics_.liveWindows--;
            }
        }
        if (status)
        {
            monitor.dpiX = dpi;
            monitor.dpiY = dpi;
        }
        return status;
    }

    ScreenRegionStatus ReadRotations(IDXGIFactory1& factory, MonitorSnapshot& snapshot) noexcept
    {
        std::array<bool, maximumMonitors> found{};
        for (UINT adapterIndex = 0; adapterIndex <= 64; adapterIndex++)
        {
            ComPtr<IDXGIAdapter1> adapter;
            HRESULT result = factory.EnumAdapters1(adapterIndex, adapter.GetAddressOf());
            if (result == DXGI_ERROR_NOT_FOUND)
            {
                break;
            }
            if (FAILED(result))
            {
                return NativeError(ScreenRegionStage::OutputQuery, result);
            }
            if (adapterIndex == 64)
            {
                return ScreenRegionStatus::Failure(ScreenRegionErrorCode::ResourceLimit, ScreenRegionStage::OutputQuery);
            }
            for (UINT outputIndex = 0; outputIndex <= 64; outputIndex++)
            {
                ComPtr<IDXGIOutput> output;
                result = adapter->EnumOutputs(outputIndex, output.GetAddressOf());
                if (result == DXGI_ERROR_NOT_FOUND)
                {
                    break;
                }
                if (FAILED(result))
                {
                    return NativeError(ScreenRegionStage::OutputQuery, result);
                }
                if (outputIndex == 64)
                {
                    return ScreenRegionStatus::Failure(ScreenRegionErrorCode::ResourceLimit, ScreenRegionStage::OutputQuery);
                }
                DXGI_OUTPUT_DESC description{};
                result = output->GetDesc(&description);
                if (FAILED(result))
                {
                    return NativeError(ScreenRegionStage::OutputQuery, result);
                }
                if (!description.AttachedToDesktop)
                {
                    continue;
                }
                for (std::size_t index = 0; index < snapshot.count; index++)
                {
                    auto& monitor = snapshot.monitors[index];
                    if (monitor.monitor != description.Monitor)
                    {
                        continue;
                    }
                    if (found[index])
                    {
                        return ScreenRegionStatus::Failure(ScreenRegionErrorCode::AmbiguousMonitor, ScreenRegionStage::OutputQuery);
                    }
                    if (!EqualRect(description.DesktopCoordinates, monitor.physicalRect) ||
                        !std::equal(monitor.deviceName.begin(), monitor.deviceName.end(), description.DeviceName))
                    {
                        return ScreenRegionStatus::Failure(ScreenRegionErrorCode::DisplayChanged, ScreenRegionStage::OutputQuery);
                    }
                    found[index] = true;
                    monitor.rotation = description.Rotation;
                }
            }
        }
        for (std::size_t index = 0; index < snapshot.count; index++)
        {
            if (!found[index])
            {
                return ScreenRegionStatus::Failure(ScreenRegionErrorCode::MetadataUnavailable, ScreenRegionStage::OutputQuery, DXGI_ERROR_NOT_FOUND);
            }
        }
        return Fault(ScreenRegionStage::OutputQuery);
    }

    bool IsOwnWindow(const HWND window) const noexcept
    {
        return window != nullptr && std::find(windows_.begin(), windows_.end(), window) != windows_.end();
    }

    void Queue(const SelectionEvent event) noexcept
    {
        if (eventCount_ == events_.size())
        {
            pendingError_ = ScreenRegionStatus::Failure(ScreenRegionErrorCode::ResourceLimit, ScreenRegionStage::Input);
            return;
        }
        events_[(eventHead_ + eventCount_) % events_.size()] = event;
        eventCount_++;
    }

    void QueuePointer(const SelectionEventType type) noexcept
    {
        if (!pendingError_)
        {
            return;
        }
        POINT point{};
        if (!GetPhysicalCursorPos(&point))
        {
            pendingError_ = LastError(ScreenRegionStage::Cursor);
            return;
        }
        pendingError_ = Fault(ScreenRegionStage::Cursor);
        if (pendingError_)
        {
            Queue({type, point});
        }
    }

    void Paint(const HWND window) noexcept
    {
        const auto position = std::find(windows_.begin(), windows_.end(), window);
        if (position == windows_.end())
        {
            return;
        }
        const auto index = static_cast<std::size_t>(position - windows_.begin());
        PAINTSTRUCT paint{};
        const HDC context = BeginPaint(window, &paint);
        if (context == nullptr)
        {
            pendingError_ = LastError(ScreenRegionStage::Paint);
            return;
        }
        RECT client{};
        bool succeeded = GetClientRect(window, &client) != FALSE;
        succeeded = succeeded && FillRect(context, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH))) != 0;
        const HGDIOBJ oldFont = SelectObject(context, fonts_[index]);
        const HGDIOBJ oldPen = SelectObject(context, preview_.validation ? validPen_ : invalidPen_);
        const HGDIOBJ oldBrush = SelectObject(context, GetStockObject(NULL_BRUSH));
        succeeded = succeeded && oldFont != nullptr && oldFont != HGDI_ERROR && oldPen != nullptr && oldPen != HGDI_ERROR && oldBrush != nullptr &&
                    oldBrush != HGDI_ERROR;
        succeeded = succeeded && SetBkMode(context, TRANSPARENT) != 0 && SetTextColor(context, RGB(255, 255, 255)) != CLR_INVALID;
        RECT textRect = client;
        textRect.left = (std::min)(16L, client.right);
        textRect.top = (std::min)(16L, client.bottom);
        std::array<wchar_t, 384> text{};
        const wchar_t* const hint = preview_.validation ? L"" : L"\nInvalid region: keep the entire selection inside ONE monitor. Drag again.";
        const auto width = preview_.hasRectangle ? static_cast<std::int64_t>(preview_.physicalRect.right) - preview_.physicalRect.left : 0;
        const auto height = preview_.hasRectangle ? static_cast<std::int64_t>(preview_.physicalRect.bottom) - preview_.physicalRect.top : 0;
        const int length = swprintf_s(text.data(), text.size(), L"Drag to select physical pixels. Esc / right-click: cancel.\n%lld x %lld physical px%s",
                                      static_cast<long long>(width), static_cast<long long>(height), hint);
        if (succeeded && length > 0 && client.right > 16 && client.bottom > 16)
        {
            succeeded = DrawTextW(context, text.data(), length, &textRect, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX) != 0;
        }
        if (succeeded && preview_.hasRectangle)
        {
            const RECT origin = topology_.monitors[index].physicalRect;
            const RECT clipped{(std::max)(preview_.physicalRect.left, origin.left), (std::max)(preview_.physicalRect.top, origin.top),
                               (std::min)(preview_.physicalRect.right, origin.right), (std::min)(preview_.physicalRect.bottom, origin.bottom)};
            if (clipped.left < clipped.right && clipped.top < clipped.bottom)
            {
                // Intersection and validated monitor extents bound each signed subtraction.
                succeeded = Rectangle(context, static_cast<int>(static_cast<std::int64_t>(clipped.left) - origin.left),
                                      static_cast<int>(static_cast<std::int64_t>(clipped.top) - origin.top),
                                      static_cast<int>(static_cast<std::int64_t>(clipped.right) - origin.left),
                                      static_cast<int>(static_cast<std::int64_t>(clipped.bottom) - origin.top)) != FALSE;
            }
        }
        if (oldFont != nullptr && oldFont != HGDI_ERROR)
        {
            SelectObject(context, oldFont);
        }
        if (oldPen != nullptr && oldPen != HGDI_ERROR)
        {
            SelectObject(context, oldPen);
        }
        if (oldBrush != nullptr && oldBrush != HGDI_ERROR)
        {
            SelectObject(context, oldBrush);
        }
        const BOOL ended = EndPaint(window, &paint);
        if (!succeeded || !ended || length <= 0)
        {
            pendingError_ = NativeError(ScreenRegionStage::Paint, ERROR_GEN_FAILURE);
        }
    }

    static LRESULT CALLBACK WindowProcedure(const HWND window, const UINT message, const WPARAM word, const LPARAM parameter) noexcept
    {
        auto* backend = reinterpret_cast<NativeBackend*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            const auto* const creation = reinterpret_cast<const CREATESTRUCTW*>(parameter);
            backend = static_cast<NativeBackend*>(creation->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(backend));
        }
        if (backend != nullptr)
        {
            if (message == WM_PAINT)
            {
                backend->Paint(window);
                return 0;
            }
            if (message == WM_ERASEBKGND)
            {
                return 1;
            }
            if (message == WM_NCDESTROY)
            {
                SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            }
            if (backend->ready_)
            {
                switch (message)
                {
                case WM_LBUTTONDOWN:
                    backend->QueuePointer(SelectionEventType::PointerDown);
                    return 0;
                case WM_MOUSEMOVE:
                    backend->QueuePointer(SelectionEventType::PointerMove);
                    return 0;
                case WM_LBUTTONUP:
                    backend->QueuePointer(SelectionEventType::PointerUp);
                    return 0;
                case WM_RBUTTONDOWN:
                case WM_CLOSE:
                case WM_CANCELMODE:
                    backend->Queue({SelectionEventType::Cancel, {}});
                    return 0;
                case WM_KEYDOWN:
                    if (word == VK_ESCAPE)
                    {
                        backend->Queue({SelectionEventType::Cancel, {}});
                        return 0;
                    }
                    break;
                case WM_DPICHANGED:
                case WM_DISPLAYCHANGE:
                    backend->Queue({SelectionEventType::DisplayChanged, {}});
                    return 0;
                case WM_TIMER:
                    backend->Queue({SelectionEventType::CheckEnvironment, {}});
                    return 0;
                case WM_CAPTURECHANGED:
                    if (backend->ownsCapture_)
                    {
                        backend->ownsCapture_ = false;
                        backend->diagnostics_.ownsCapture = false;
                        backend->Queue({SelectionEventType::Cancel, {}});
                    }
                    break;
                case WM_ACTIVATE:
                    if (LOWORD(word) != WA_INACTIVE)
                    {
                        backend->activated_ = true;
                    }
                    else if (backend->activated_ && !backend->IsOwnWindow(reinterpret_cast<HWND>(parameter)))
                    {
                        backend->Queue({SelectionEventType::Cancel, {}});
                    }
                    break;
                default:
                    break;
                }
            }
        }
        return DefWindowProcW(window, message, word, parameter);
    }

    NativeBackendOptions options_;
    NativeDiagnostics diagnostics_;
    MonitorSnapshot topology_;
    std::array<HWND, maximumMonitors> windows_{};
    std::array<HFONT, maximumMonitors> fonts_{};
    std::array<wchar_t, 96> className_{};
    HINSTANCE instance_ = nullptr;
    HPEN validPen_ = nullptr;
    HPEN invalidPen_ = nullptr;
    HWND captureWindow_ = nullptr;
    bool classRegistered_ = false;
    bool ready_ = false;
    bool activated_ = false;
    bool ownsCapture_ = false;
    bool closed_ = false;
    bool repostQuit_ = false;
    int quitCode_ = 0;
    std::size_t faultVisits_ = 0;
    SelectionPreview preview_;
    std::array<SelectionEvent, 64> events_{};
    std::size_t eventHead_ = 0;
    std::size_t eventCount_ = 0;
    ScreenRegionStatus pendingError_;
    ScreenRegionStatus cleanupStatus_;
};

} // namespace

std::unique_ptr<ScreenRegionBackend> MakeNativeBackend(const NativeBackendOptions& options)
{
    return std::make_unique<NativeBackend>(options);
}

} // namespace pbscreenregion::detail
