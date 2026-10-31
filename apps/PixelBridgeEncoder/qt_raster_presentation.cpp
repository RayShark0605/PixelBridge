#include "qt_raster_presentation.h"
#include "qt_raster_paint.h"

#include <QApplication>
#include <QCloseEvent>
#include <QImage>
#include <QKeyEvent>
#include <QPaintEngine>
#include <QPaintEvent>
#include <QPainter>
#include <QThread>
#include <QTimer>
#include <QWidget>

#include <Windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <vector>

#ifdef PB_QT_RASTER_TESTS
#include "qt_raster_test_access.h"
#endif

namespace pbencoder
{
namespace
{
using namespace pbrenderd3d;

void Increment(std::uint64_t& value) noexcept
{
    if (value != (std::numeric_limits<std::uint64_t>::max)())
    {
        value++;
    }
}

struct RunState
{
    DataWindowConfig config;
    DataWindowSnapshot snapshot;
    std::array<std::vector<std::byte>, 2> frames;
    std::array<std::uint64_t, 2> sequences{};
    int currentSlot = -1;
    int pendingSlot = -1;
    bool creationRequested = false;
    bool ready = false;
    bool canceled = false;
    bool userStopRequested = false;
};

struct SharedState
{
    std::mutex mutex;
    std::condition_variable condition;
    std::shared_ptr<RunState> current;
    std::function<bool()> notify;
    bool dispatchQueued = false;
};

void DiscardFrames(RunState& run) noexcept
{
    if (run.pendingSlot >= 0)
    {
        Increment(run.snapshot.discardedEpochFrames);
    }
    if (run.currentSlot >= 0)
    {
        Increment(run.snapshot.invalidatedActiveFrames);
    }
    run.currentSlot = -1;
    run.pendingSlot = -1;
    run.snapshot.pendingFrame = false;
    run.snapshot.inFlightFrame = false;
    run.snapshot.activeFrame = false;
    run.snapshot.activeFrameSequence = 0;
    run.snapshot.activeFramePresentationEpoch = 0;
}

void FailLocked(SharedState& shared, RunState& run, const PresentationStatus error) noexcept
{
    run.canceled = true;
    run.snapshot.state = WindowState::Failed;
    run.snapshot.error = error;
    run.snapshot.candidateContractSatisfied = false;
    DiscardFrames(run);
    shared.condition.notify_all();
}

void StopLocked(SharedState& shared, RunState& run) noexcept
{
    run.canceled = true;
    run.snapshot.state = WindowState::Stopped;
    run.snapshot.candidateContractSatisfied = false;
    DiscardFrames(run);
    shared.condition.notify_all();
}

void QueueLocked(SharedState& shared, RunState& run) noexcept
{
    if (shared.current.get() != &run || shared.dispatchQueued || !shared.notify)
    {
        return;
    }
    try
    {
        shared.dispatchQueued = true;
        if (shared.notify())
        {
            return;
        }
    }
    catch (...)
    {
    }
    shared.dispatchQueued = false;
    FailLocked(shared, run, PresentationStatus::Failure(PresentationErrorCode::InternalError, PresentationStage::WakeEvent));
}

PresentationStatus ValidateQtConfig(const DataWindowConfig& config) noexcept
{
    if (config.presentationModel != PresentationModel::QtRaster || config.width == 0 || config.height == 0 ||
        config.width > 16384 || config.height > 16384 || config.maximumFrameBytes == 0 ||
        config.maximumFrameBytes == (std::numeric_limits<std::uint64_t>::max)() || config.waitTimeoutMilliseconds == 0 ||
        config.waitTimeoutMilliseconds > 60000 || (config.clientOrigin &&
        (config.clientOrigin->x == (std::numeric_limits<std::int32_t>::min)() ||
         config.clientOrigin->y == (std::numeric_limits<std::int32_t>::min)() ||
         static_cast<std::int64_t>(config.clientOrigin->x) + config.width > (std::numeric_limits<std::int32_t>::max)() ||
         static_cast<std::int64_t>(config.clientOrigin->y) + config.height > (std::numeric_limits<std::int32_t>::max)())))
    {
        return PresentationStatus::Failure(PresentationErrorCode::InvalidConfiguration, PresentationStage::Configuration);
    }
    // Dimensions above are bounded before multiplication.
    const auto byteCount = static_cast<std::uint64_t>(config.width) * config.height * 4;
    if (byteCount > config.maximumFrameBytes || byteCount > (std::numeric_limits<std::size_t>::max)() / 2)
    {
        return PresentationStatus::Failure(PresentationErrorCode::ResourceLimit, PresentationStage::Configuration);
    }
    return {};
}

class QtRasterAdapter final : public pbapp::EncoderPresentation
{
public:
    QtRasterAdapter(std::shared_ptr<SharedState> shared, std::shared_ptr<RunState> run) : shared_(std::move(shared)), run_(std::move(run))
    {
    }
    ~QtRasterAdapter() override
    {
        RequestStop();
    }
    DataWindowSnapshot GetSnapshot() const override
    {
        const std::lock_guard lock(shared_->mutex);
        return run_->snapshot;
    }
    PresentationStatus SubmitFrame(const CanonicalBgraFrameView& frame) override
    {
        const std::lock_guard lock(shared_->mutex);
        if (shared_->current != run_ || run_->canceled || !run_->ready)
        {
            return PresentationStatus::Failure(PresentationErrorCode::NotRunning, PresentationStage::Upload);
        }
        const auto validation = ValidateCanonicalBgraPixels(run_->config.width, run_->config.height, run_->config.maximumFrameBytes, frame);
        if (!validation)
        {
            return validation;
        }
        if (frame.presentationEpoch != run_->snapshot.timing.presentationEpoch)
        {
            return PresentationStatus::Failure(PresentationErrorCode::EpochMismatch, PresentationStage::Upload);
        }
        if (run_->snapshot.state != WindowState::Running)
        {
            return PresentationStatus::Failure(PresentationErrorCode::Paused, PresentationStage::Upload);
        }
        if (run_->pendingSlot >= 0)
        {
            Increment(run_->snapshot.replacedPendingFrames);
        }
        else
        {
            run_->pendingSlot = run_->currentSlot == 0 ? 1 : 0;
        }
        const auto slot = static_cast<std::size_t>(run_->pendingSlot);
        const auto rowBytes = static_cast<std::size_t>(frame.width) * 4;
        for (std::uint32_t row = 0; row < frame.height; row++)
        {
            std::memcpy(run_->frames[slot].data() + static_cast<std::size_t>(row) * rowBytes,
                frame.pixels.data() + static_cast<std::size_t>(row) * frame.rowPitch, rowBytes);
        }
        run_->sequences[slot] = frame.frameSequence;
        Increment(run_->snapshot.submittedFrames);
        run_->snapshot.pendingFrame = true;
        QueueLocked(*shared_, *run_);
        return run_->canceled ? run_->snapshot.error : PresentationStatus{};
    }
    void RequestStop() noexcept override
    {
        const std::lock_guard lock(shared_->mutex);
        StopLocked(*shared_, *run_);
        QueueLocked(*shared_, *run_);
    }
    void Stop() noexcept override
    {
        // Logical stop never waits for QWidget cleanup on the GUI thread.
        RequestStop();
    }

private:
    const std::shared_ptr<SharedState> shared_;
    const std::shared_ptr<RunState> run_;
};

class RasterWindow final : public QWidget
{
public:
    std::function<void(QPainter&, bool)> paint;
    std::function<void(PresentationStatus)> fail;
    std::function<void()> requestClose;
    std::uint64_t modeChangeSerial = 0;
    std::uint64_t dpiChangeSerial = 0;
    std::uint64_t maximumWindowFrameBytes = 0;

protected:
    void paintEvent(QPaintEvent* event) override
    {
        try
        {
            QPainter painter(this);
            paint(painter, QRegion(rect()).subtracted(event->region()).isEmpty());
        }
        catch (...)
        {
            fail(PresentationStatus::Failure(PresentationErrorCode::InternalError, PresentationStage::Present));
        }
    }
    void closeEvent(QCloseEvent* event) override
    {
        event->ignore();
        requestClose();
    }
    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->key() == Qt::Key_Escape)
        {
            requestClose();
            event->accept();
            return;
        }
        QWidget::keyPressEvent(event);
    }
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override
    {
        const auto* const nativeMessage = static_cast<const MSG*>(message);
        if (nativeMessage->message == WM_WINDOWPOSCHANGING && maximumWindowFrameBytes != 0)
        {
            auto* const position = reinterpret_cast<WINDOWPOS*>(nativeMessage->lParam);
            if ((position->flags & SWP_NOSIZE) == 0)
            {
                RECT borders{};
                const auto style = static_cast<DWORD>(GetWindowLongPtrW(nativeMessage->hwnd, GWL_STYLE));
                const auto extendedStyle = static_cast<DWORD>(GetWindowLongPtrW(nativeMessage->hwnd, GWL_EXSTYLE));
                const UINT dpi = GetDpiForWindow(nativeMessage->hwnd);
                if (dpi == 0 || !AdjustWindowRectExForDpi(&borders, style, FALSE, extendedStyle, dpi))
                {
                    position->flags |= SWP_NOSIZE;
                    fail(PresentationStatus::Failure(PresentationErrorCode::NativeFailure, PresentationStage::Window, GetLastError()));
                }
                else
                {
                    const auto width = static_cast<std::int64_t>(position->cx) - (static_cast<std::int64_t>(borders.right) - borders.left);
                    const auto height = static_cast<std::int64_t>(position->cy) - (static_cast<std::int64_t>(borders.bottom) - borders.top);
                    if (width > 16384 || height > 16384 || (width > 0 && height > 0 &&
                        static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * 4 > maximumWindowFrameBytes))
                    {
                        // Reject before Qt processes WM_SIZE and sizes its own
                        // backing store, not merely at our later paint check.
                        position->flags |= SWP_NOSIZE;
                        fail(PresentationStatus::Failure(PresentationErrorCode::ResourceLimit, PresentationStage::Window));
                    }
                }
            }
        }
        else if (nativeMessage->message == WM_DISPLAYCHANGE)
        {
            Increment(modeChangeSerial);
        }
        else if (nativeMessage->message == WM_DPICHANGED)
        {
            Increment(dpiChangeSerial);
        }
        return QWidget::nativeEvent(eventType, message, result);
    }
};

PresentationStatus PositionWindow(RasterWindow& window, const DataWindowConfig& config)
{
    const HWND handle = reinterpret_cast<HWND>(window.winId());
    POINT point{};
    if (config.clientOrigin)
    {
        point = {config.clientOrigin->x, config.clientOrigin->y};
    }
    const HMONITOR monitor = MonitorFromPoint(point, config.clientOrigin ? MONITOR_DEFAULTTONEAREST : MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO monitorInfo{sizeof(MONITORINFO)};
    if (!GetMonitorInfoW(monitor, &monitorInfo))
    {
        return PresentationStatus::Failure(PresentationErrorCode::NativeFailure, PresentationStage::Environment, GetLastError());
    }
    const auto style = static_cast<DWORD>(GetWindowLongPtrW(handle, GWL_STYLE));
    const auto extendedStyle = static_cast<DWORD>(GetWindowLongPtrW(handle, GWL_EXSTYLE)) | WS_EX_NOACTIVATE;
    SetLastError(0);
    if (SetWindowLongPtrW(handle, GWL_EXSTYLE, extendedStyle) == 0 && GetLastError() != 0)
    {
        return PresentationStatus::Failure(PresentationErrorCode::NativeFailure, PresentationStage::Window, GetLastError());
    }
    RECT outer{0, 0, static_cast<LONG>(config.width), static_cast<LONG>(config.height)};
    const UINT dpi = GetDpiForWindow(handle);
    if (dpi == 0 || !AdjustWindowRectExForDpi(&outer, style, FALSE, extendedStyle, dpi))
    {
        return PresentationStatus::Failure(PresentationErrorCode::NativeFailure, PresentationStage::Window, GetLastError());
    }
    // Native WM_SIZE delivery may trail a hidden QWidget render. Establish
    // Qt's logical size before the authoritative physical SetWindowPos.
    const double ratio = window.devicePixelRatioF();
    if (!std::isfinite(ratio) || ratio <= 0)
    {
        return PresentationStatus::Failure(PresentationErrorCode::ContractViolation, PresentationStage::DpiAwareness);
    }
    window.resize(static_cast<int>(std::llround(config.width / ratio)), static_cast<int>(std::llround(config.height / ratio)));
    const std::int64_t clientX = config.clientOrigin ? point.x : static_cast<std::int64_t>(monitorInfo.rcMonitor.left) +
        (static_cast<std::int64_t>(monitorInfo.rcMonitor.right) - monitorInfo.rcMonitor.left - config.width) / 2;
    const std::int64_t clientY = config.clientOrigin ? point.y : static_cast<std::int64_t>(monitorInfo.rcMonitor.top) +
        (static_cast<std::int64_t>(monitorInfo.rcMonitor.bottom) - monitorInfo.rcMonitor.top - config.height) / 2;
    const auto left = clientX + outer.left;
    const auto top = clientY + outer.top;
    if (left < (std::numeric_limits<int>::min)() || left > (std::numeric_limits<int>::max)() ||
        top < (std::numeric_limits<int>::min)() || top > (std::numeric_limits<int>::max)())
    {
        return PresentationStatus::Failure(PresentationErrorCode::InvalidConfiguration, PresentationStage::Window);
    }
    if (!SetWindowPos(handle, config.topmost ? HWND_TOPMOST : HWND_NOTOPMOST, static_cast<int>(left), static_cast<int>(top),
        outer.right - outer.left, outer.bottom - outer.top, SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED))
    {
        return PresentationStatus::Failure(PresentationErrorCode::NativeFailure, PresentationStage::Window, GetLastError());
    }
    return {};
}

PresentationStatus ReadEnvironment(RasterWindow& window, WindowEnvironment& environment)
{
    const HWND handle = reinterpret_cast<HWND>(window.winId());
    RECT client{};
    POINT origin{};
    if (!GetClientRect(handle, &client) || !ClientToScreen(handle, &origin) || client.right < 0 || client.bottom < 0)
    {
        return PresentationStatus::Failure(PresentationErrorCode::NativeFailure, PresentationStage::Environment, GetLastError());
    }
    environment.clientOrigin = {origin.x, origin.y};
    environment.clientWidth = static_cast<std::uint32_t>(client.right);
    environment.clientHeight = static_cast<std::uint32_t>(client.bottom);
    environment.dpi = GetDpiForWindow(handle);
    environment.minimized = IsIconic(handle) != FALSE;
    environment.occluded = IsWindowVisible(handle) == FALSE;
    environment.modeChangeSerial = window.modeChangeSerial;
    environment.dpiChangeSerial = window.dpiChangeSerial;
    const HMONITOR monitor = MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    DEVMODEW displayMode{};
    displayMode.dmSize = sizeof(displayMode);
    if (!GetMonitorInfoW(monitor, &monitorInfo) ||
        !EnumDisplaySettingsExW(monitorInfo.szDevice, ENUM_CURRENT_SETTINGS, &displayMode, 0))
    {
        return PresentationStatus::Failure(PresentationErrorCode::NativeFailure, PresentationStage::Environment, GetLastError());
    }
    environment.monitorIdentity = reinterpret_cast<std::uintptr_t>(monitor);
    std::copy(std::begin(monitorInfo.szDevice), std::end(monitorInfo.szDevice), environment.displayName.begin());
    environment.modeWidth = displayMode.dmPelsWidth;
    environment.modeHeight = displayMode.dmPelsHeight;
    environment.modeFrequency = displayMode.dmDisplayFrequency;
    environment.modeOrientation = displayMode.dmDisplayOrientation;
    environment.singleMonitor = origin.x >= monitorInfo.rcMonitor.left && origin.y >= monitorInfo.rcMonitor.top &&
        static_cast<std::int64_t>(origin.x) + client.right <= monitorInfo.rcMonitor.right &&
        static_cast<std::int64_t>(origin.y) + client.bottom <= monitorInfo.rcMonitor.bottom;
    return {};
}
}

struct QtRasterPresentationHost::Implementation
{
    explicit Implementation(QtRasterPresentationHost& owner) : host(owner)
    {
        shared->notify = [this]
        {
            return QMetaObject::invokeMethod(&host, [this] { Dispatch(); }, Qt::QueuedConnection);
        };
        timer.setInterval(100);
        timer.setTimerType(Qt::CoarseTimer);
        QObject::connect(&timer, &QTimer::timeout, &host, [this]
        {
            try
            {
                if (window && RefreshEnvironment())
                {
                    window->update();
                }
            }
            catch (...)
            {
                Fail(PresentationStatus::Failure(PresentationErrorCode::InternalError, PresentationStage::Environment));
            }
        });
    }

    QtRasterPresentationHost& host;
    const std::shared_ptr<SharedState> shared = std::make_shared<SharedState>();
    std::unique_ptr<RasterWindow> window;
    std::shared_ptr<RunState> windowRun;
    QImage physicalScratch;
    QTimer timer;
#ifdef PB_QT_RASTER_TESTS
    bool hiddenWindow = false;
    bool simulateNativeDpi = false;
    std::optional<WindowEnvironment> syntheticEnvironment;
#endif

    void Fail(const PresentationStatus error) noexcept
    {
        const std::lock_guard lock(shared->mutex);
        if (shared->current)
        {
            FailLocked(*shared, *shared->current, error);
        }
    }

    bool RefreshEnvironment() noexcept
    {
        try
        {
            WindowEnvironment environment;
            const auto status = ReadEnvironment(*window, environment);
#ifdef PB_QT_RASTER_TESTS
            if (syntheticEnvironment)
            {
                environment = *syntheticEnvironment;
            }
            else if (!status)
#else
            if (!status)
#endif
            {
                Fail(status);
                return false;
            }
#ifdef PB_QT_RASTER_TESTS
            if (hiddenWindow && !syntheticEnvironment)
            {
                // The native hidden test never exposes a desktop window.
                environment.occluded = false;
            }
            if (simulateNativeDpi)
            {
                environment.dpi = static_cast<std::uint32_t>(std::llround(window->devicePixelRatioF() * 96));
            }
#endif
            const std::lock_guard lock(shared->mutex);
            auto& run = *windowRun;
            if (shared->current != windowRun || run.canceled)
            {
                return false;
            }
            if (environment.clientWidth > 16384 || environment.clientHeight > 16384 ||
                static_cast<std::uint64_t>(environment.clientWidth) * environment.clientHeight * 4 > run.config.maximumFrameBytes)
            {
                FailLocked(*shared, run, PresentationStatus::Failure(PresentationErrorCode::ResourceLimit, PresentationStage::Environment));
                return false;
            }
            const auto ratio = window->devicePixelRatioF();
            // Win32 client pixels are authoritative. Qt rounds physical/DPR
            // into integer logical coordinates; reversing that rounding is
            // not an equality test (for example 322 physical pixels at 150%).
            bool geometryValid = std::isfinite(ratio) && ratio > 0 && environment.dpi != 0 &&
                std::abs(ratio - static_cast<double>(environment.dpi) / 96.0) < 0.000001 &&
                window->width() == std::llround(environment.clientWidth / ratio) &&
                window->height() == std::llround(environment.clientHeight / ratio);
#ifdef PB_QT_RASTER_TESTS
            if (syntheticEnvironment)
            {
                geometryValid = true;
            }
#endif
            if (!geometryValid && !environment.minimized)
            {
                FailLocked(*shared, run, PresentationStatus::Failure(PresentationErrorCode::ContractViolation, PresentationStage::DpiAwareness));
                return false;
            }
            const bool changed = run.snapshot.timing.presentationEpoch == 0 || run.snapshot.environment != environment;
            if (changed)
            {
                if (run.snapshot.timing.presentationEpoch == (std::numeric_limits<std::uint64_t>::max)())
                {
                    FailLocked(*shared, run, PresentationStatus::Failure(PresentationErrorCode::ResourceLimit, PresentationStage::Environment));
                    return false;
                }
                const auto& previous = run.snapshot.environment;
                auto reason = pbpresenttiming::EpochReason::Resize;
                if (run.snapshot.timing.presentationEpoch == 0)
                {
                    reason = pbpresenttiming::EpochReason::Initial;
                }
                else if (previous.dpi != environment.dpi || previous.dpiChangeSerial != environment.dpiChangeSerial)
                {
                    reason = pbpresenttiming::EpochReason::DpiChanged;
                }
                else if (previous.monitorIdentity != environment.monitorIdentity)
                {
                    reason = pbpresenttiming::EpochReason::MonitorChanged;
                }
                else if (previous.modeChangeSerial != environment.modeChangeSerial || previous.modeWidth != environment.modeWidth ||
                    previous.modeHeight != environment.modeHeight || previous.modeFrequency != environment.modeFrequency ||
                    previous.modeOrientation != environment.modeOrientation)
                {
                    reason = pbpresenttiming::EpochReason::DisplayModeChanged;
                }
                else if (environment.minimized || environment.occluded)
                {
                    reason = pbpresenttiming::EpochReason::Occluded;
                }
                else if (previous.minimized || previous.occluded)
                {
                    reason = pbpresenttiming::EpochReason::Restored;
                }
                DiscardFrames(run);
                run.snapshot.timing.presentationEpoch++;
                run.snapshot.timing.epochReason = reason;
                run.snapshot.environment = environment;
            }
            run.snapshot.viewport = ResolvePresentationViewport(run.config, environment.clientWidth, environment.clientHeight);
            run.snapshot.contract.bufferWidth = environment.clientWidth;
            run.snapshot.contract.bufferHeight = environment.clientHeight;
            const bool active = environment.singleMonitor && !environment.minimized && !environment.occluded && !environment.closed &&
                CanPresentData(run.snapshot.viewport.disposition);
            run.snapshot.state = active ? WindowState::Running : WindowState::Paused;
            run.snapshot.candidateContractSatisfied = active && run.snapshot.contract.rasterPaintEngine;
            return changed;
        }
        catch (...)
        {
            Fail(PresentationStatus::Failure(PresentationErrorCode::InternalError, PresentationStage::Environment));
            return false;
        }
    }

    void Paint(QPainter& painter, const bool wholeClient) noexcept
    {
        const bool environmentChanged = RefreshEnvironment();
        try
        {
            const std::lock_guard lock(shared->mutex);
            auto& run = *windowRun;
            if (shared->current != windowRun || run.canceled)
            {
                return;
            }
            if (!painter.isActive() || !painter.paintEngine() || painter.paintEngine()->type() != QPaintEngine::Raster)
            {
                FailLocked(*shared, run, PresentationStatus::Failure(PresentationErrorCode::ContractViolation, PresentationStage::Present));
                return;
            }
            Increment(run.snapshot.softwarePaintCalls);
            run.snapshot.contract.rasterPaintEngine = true;
            run.snapshot.candidateContractSatisfied = run.snapshot.state == WindowState::Running;
            // A system exposure can paint only part of the widget before our
            // full update arrives. Never promote a pending frame into such a
            // partial damage region, leaving a mixed old/new backing store.
            const bool newFrame = wholeClient && run.pendingSlot >= 0;
            if (newFrame)
            {
                run.currentSlot = run.pendingSlot;
                run.pendingSlot = -1;
                run.snapshot.pendingFrame = false;
            }
            if (run.currentSlot >= 0 && run.snapshot.state == WindowState::Running)
            {
                const auto slot = static_cast<std::size_t>(run.currentSlot);
                const CanonicalBgraFrameView frame{run.frames[slot], run.config.width, run.config.height,
                    static_cast<std::size_t>(run.config.width) * 4, run.sequences[slot], run.snapshot.timing.presentationEpoch};
                run.snapshot.inFlightFrame = true;
                // This lock leases current pixels until drawing finishes.
                // Cancellation cannot free them underneath a QPainter read.
                const auto status = PaintQtRasterFrame(painter, run.config, frame, run.snapshot.environment.clientWidth,
                    run.snapshot.environment.clientHeight, window->devicePixelRatioF(), physicalScratch);
                run.snapshot.inFlightFrame = false;
                if (!status)
                {
                    FailLocked(*shared, run, status);
                    return;
                }
                if (newFrame && (!run.snapshot.activeFrame || run.snapshot.activeFrameSequence != frame.frameSequence ||
                    run.snapshot.activeFramePresentationEpoch != frame.presentationEpoch))
                {
                    Increment(run.snapshot.softwareFramesPainted);
                }
                run.snapshot.activeFrame = true;
                run.snapshot.activeFrameSequence = frame.frameSequence;
                run.snapshot.activeFramePresentationEpoch = frame.presentationEpoch;
            }
            else
            {
                painter.setCompositionMode(QPainter::CompositionMode_Source);
                painter.fillRect(window->rect(), QColor(128, 128, 128));
            }
            if (wholeClient)
            {
                run.ready = true;
                shared->condition.notify_all();
            }
            else if (run.pendingSlot >= 0 || environmentChanged || !run.ready)
            {
                // Defer update() to Dispatch rather than reenter paintEvent.
                QueueLocked(*shared, run);
            }
        }
        catch (...)
        {
            Fail(PresentationStatus::Failure(PresentationErrorCode::InternalError, PresentationStage::Present));
        }
    }

    void QueueUserStop() noexcept
    {
        const std::lock_guard lock(shared->mutex);
        if (shared->current && !shared->current->canceled)
        {
            shared->current->userStopRequested = true;
            QueueLocked(*shared, *shared->current);
        }
    }

    void Dispatch() noexcept
    {
        try
        {
            std::shared_ptr<RunState> run;
            bool userStop = false;
            bool canceled = false;
            {
                const std::lock_guard lock(shared->mutex);
                shared->dispatchQueued = false;
                run = shared->current;
                if (!run || !run->creationRequested)
                {
                    return;
                }
                userStop = run->userStopRequested;
                run->userStopRequested = false;
                canceled = run->canceled;
            }
            if (userStop)
            {
                // Dispatch outside the QWidget close/key event stack, because
                // the controller is allowed to join and destroy that window.
                emit host.StopRequested();
                const std::lock_guard lock(shared->mutex);
                StopLocked(*shared, *run);
                QueueLocked(*shared, *run);
                return;
            }
            if (canceled)
            {
                if (window)
                {
                    window->hide();
                }
                return;
            }
            if (!window)
            {
                if (!AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
                {
                    Fail(PresentationStatus::Failure(PresentationErrorCode::DpiAwarenessRequired, PresentationStage::DpiAwareness));
                    return;
                }
                windowRun = run;
                window = std::make_unique<RasterWindow>();
                window->setWindowTitle(QStringLiteral("PixelBridge — Qt 软件绘制"));
                auto flags = Qt::Window | Qt::WindowDoesNotAcceptFocus;
                if (run->config.topmost)
                {
                    flags |= Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint;
                }
                window->setWindowFlags(flags);
                window->setAttribute(Qt::WA_OpaquePaintEvent);
                window->setAttribute(Qt::WA_NoSystemBackground);
                window->setAttribute(Qt::WA_ShowWithoutActivating);
                window->paint = [this](QPainter& painter, const bool wholeClient) { Paint(painter, wholeClient); };
                window->fail = [this](const PresentationStatus error) { Fail(error); };
                window->requestClose = [this] { QueueUserStop(); };
                PresentationStatus positionStatus;
#ifdef PB_QT_RASTER_TESTS
                if (syntheticEnvironment)
                {
                    window->resize(static_cast<int>(syntheticEnvironment->clientWidth), static_cast<int>(syntheticEnvironment->clientHeight));
                    static_cast<void>(window->winId());
                }
                else
#endif
                {
                    positionStatus = PositionWindow(*window, run->config);
                }
                if (!positionStatus)
                {
                    Fail(positionStatus);
                    return;
                }
                window->maximumWindowFrameBytes = run->config.maximumFrameBytes;
                {
                    const std::lock_guard lock(shared->mutex);
                    if (shared->current != run || run->canceled)
                    {
                        return;
                    }
                    auto& contract = run->snapshot.contract;
                    contract.presentationModel = PresentationModel::QtRaster;
                    contract.swapEffect = PresentationSwapEffect::NotApplicable;
                    contract.pacing = PresentationPacing::QtEventLoop;
                    contract.bgraUnorm = true;
                    contract.noMsaa = true;
                    contract.alphaIgnored = true;
                    contract.perMonitorV2 = true;
                    contract.resizableChrome = !run->config.topmost;
                    contract.immutableCanonicalSource = true;
                    contract.pointSampled = true;
                    contract.centeredLetterbox = true;
                    contract.neutralMatteBelowMinimum = true;
                    run->snapshot.timing.state = pbpresenttiming::TimingState::Unavailable;
                    run->snapshot.timing.issue = pbpresenttiming::TimingIssue::UnsupportedStatistics;
                }
#ifdef PB_QT_RASTER_TESTS
                if (hiddenWindow)
                {
                    static_cast<void>(RenderHidden());
                }
                else
#endif
                {
                    window->show();
                }
                timer.start();
                const std::lock_guard lock(shared->mutex);
                canceled = run->canceled;
            }
            if (canceled)
            {
                window->hide();
            }
            else
            {
                window->update();
            }
        }
        catch (const std::bad_alloc&)
        {
            Fail(PresentationStatus::Failure(PresentationErrorCode::OutOfMemory, PresentationStage::Window));
        }
        catch (...)
        {
            Fail(PresentationStatus::Failure(PresentationErrorCode::InternalError, PresentationStage::Window));
        }
    }

#ifdef PB_QT_RASTER_TESTS
    QImage RenderHidden(const QRegion& region = {})
    {
        if (!window)
        {
            return {};
        }
        const auto ratio = window->devicePixelRatioF();
        QImage output(static_cast<int>(std::llround(window->width() * ratio)),
            static_cast<int>(std::llround(window->height() * ratio)), QImage::Format_RGB32);
        output.setDevicePixelRatio(ratio);
        output.fill(Qt::black);
        window->render(&output, QPoint(0, 0), region);
        return output;
    }
#endif
};

QtRasterPresentationHost::QtRasterPresentationHost(QObject* parent) : QObject(parent), implementation_(std::make_unique<Implementation>(*this))
{
    if (!qApp || QThread::currentThread() != qApp->thread())
    {
        throw std::logic_error("Qt raster host requires the GUI thread");
    }
}

QtRasterPresentationHost::~QtRasterPresentationHost()
{
    RequestStop();
    {
        const std::lock_guard lock(implementation_->shared->mutex);
        implementation_->shared->notify = {};
    }
    FinalizeStop();
}

void QtRasterPresentationHost::BeginRun()
{
    if (QThread::currentThread() != thread())
    {
        throw std::logic_error("Qt raster BeginRun requires the GUI thread");
    }
    const std::lock_guard lock(implementation_->shared->mutex);
    if (implementation_->shared->current)
    {
        throw std::logic_error("Qt raster cleanup must finish before restarting");
    }
    // Distinct shared ownership identities isolate generations even if old
    // adapters survive; queued notifications carry neither a run nor pixels.
    implementation_->shared->current = std::make_shared<RunState>();
}

std::unique_ptr<pbapp::EncoderPresentation> QtRasterPresentationHost::CreatePresentation(const DataWindowConfig& config)
{
    const auto shared = implementation_->shared;
    std::unique_lock lock(shared->mutex);
    const auto run = shared->current;
    if (!run || QThread::currentThread() == thread() || run->creationRequested)
    {
        const auto rejected = std::make_shared<RunState>();
        FailLocked(*shared, *rejected, PresentationStatus::Failure(PresentationErrorCode::InvalidConfiguration, PresentationStage::Thread));
        return std::make_unique<QtRasterAdapter>(shared, rejected);
    }
    if (run->canceled)
    {
        return std::make_unique<QtRasterAdapter>(shared, run);
    }
    run->config = config;
    const auto validation = ValidateQtConfig(config);
    if (!validation)
    {
        FailLocked(*shared, *run, validation);
        return std::make_unique<QtRasterAdapter>(shared, run);
    }
    try
    {
        const auto bytes = static_cast<std::size_t>(config.width) * config.height * 4;
        for (auto& frame : run->frames)
        {
            frame.resize(bytes);
        }
    }
    catch (const std::bad_alloc&)
    {
        FailLocked(*shared, *run, PresentationStatus::Failure(PresentationErrorCode::OutOfMemory, PresentationStage::Upload));
        return std::make_unique<QtRasterAdapter>(shared, run);
    }
    run->creationRequested = true;
    QueueLocked(*shared, *run);
    if (!shared->condition.wait_for(lock, std::chrono::milliseconds(config.waitTimeoutMilliseconds), [&] { return run->ready || run->canceled; }))
    {
        FailLocked(*shared, *run, PresentationStatus::Failure(PresentationErrorCode::Timeout, PresentationStage::Window));
    }
    return std::make_unique<QtRasterAdapter>(shared, run);
}

void QtRasterPresentationHost::RequestStop() noexcept
{
    const auto shared = implementation_->shared;
    const std::lock_guard lock(shared->mutex);
    if (shared->current)
    {
        StopLocked(*shared, *shared->current);
        QueueLocked(*shared, *shared->current);
    }
}

void QtRasterPresentationHost::FinalizeStop() noexcept
{
    Q_ASSERT(QThread::currentThread() == thread());
    RequestStop();
    implementation_->timer.stop();
    implementation_->window.reset();
    implementation_->windowRun.reset();
    implementation_->physicalScratch = {};
    const auto shared = implementation_->shared;
    const std::lock_guard lock(shared->mutex);
    if (shared->current)
    {
        for (auto& frame : shared->current->frames)
        {
            std::vector<std::byte>().swap(frame);
        }
        shared->current.reset();
    }
}

bool QtRasterPresentationHost::HasPendingWork() const noexcept
{
    const std::lock_guard lock(implementation_->shared->mutex);
    return static_cast<bool>(implementation_->shared->current);
}

#ifdef PB_QT_RASTER_TESTS
void QtRasterPresentationTestAccess::UseHiddenWindow(QtRasterPresentationHost& host)
{
    host.implementation_->hiddenWindow = true;
}
void QtRasterPresentationTestAccess::UseNativeDpiScaleSimulation(QtRasterPresentationHost& host)
{
    UseHiddenWindow(host);
    host.implementation_->simulateNativeDpi = true;
}
void QtRasterPresentationTestAccess::SetEnvironment(QtRasterPresentationHost& host, const WindowEnvironment& environment)
{
    auto& implementation = *host.implementation_;
    implementation.hiddenWindow = true;
    implementation.syntheticEnvironment = environment;
    if (implementation.window)
    {
        implementation.window->resize(static_cast<int>(environment.clientWidth), static_cast<int>(environment.clientHeight));
    }
}
bool QtRasterPresentationTestAccess::CreationQueued(const QtRasterPresentationHost& host)
{
    const auto shared = host.implementation_->shared;
    const std::lock_guard lock(shared->mutex);
    return shared->current && shared->current->creationRequested;
}
std::size_t QtRasterPresentationTestAccess::PendingWakeups(const QtRasterPresentationHost& host)
{
    const std::lock_guard lock(host.implementation_->shared->mutex);
    return host.implementation_->shared->dispatchQueued ? 1 : 0;
}
std::size_t QtRasterPresentationTestAccess::OwnedFrameBytes(const QtRasterPresentationHost& host)
{
    const auto shared = host.implementation_->shared;
    const std::lock_guard lock(shared->mutex);
    return shared->current ? shared->current->frames[0].size() + shared->current->frames[1].size() : 0;
}
std::uintptr_t QtRasterPresentationTestAccess::WindowHandle(const QtRasterPresentationHost& host)
{
    return host.implementation_->window ? static_cast<std::uintptr_t>(host.implementation_->window->winId()) : 0;
}
QString QtRasterPresentationTestAccess::DescribeGeometry(const QtRasterPresentationHost& host)
{
    const auto* const window = host.implementation_->window.get();
    if (!window)
    {
        return QStringLiteral("No window");
    }
    const HWND handle = reinterpret_cast<HWND>(window->winId());
    RECT client{};
    static_cast<void>(GetClientRect(handle, &client));
    return QStringLiteral("logical=%1x%2 DPR=%3 client=%4x%5 DPI=%6").arg(window->width()).arg(window->height())
        .arg(window->devicePixelRatioF()).arg(client.right).arg(client.bottom).arg(GetDpiForWindow(handle));
}
QImage QtRasterPresentationTestAccess::Paint(QtRasterPresentationHost& host)
{
    return host.implementation_->RenderHidden();
}
QImage QtRasterPresentationTestAccess::PaintRegion(QtRasterPresentationHost& host, const QRect& region)
{
    return host.implementation_->RenderHidden(QRegion(region));
}
void QtRasterPresentationTestAccess::CloseWindow(QtRasterPresentationHost& host)
{
    host.implementation_->window->close();
}
#endif

}
