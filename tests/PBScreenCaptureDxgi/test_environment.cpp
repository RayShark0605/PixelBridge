#include "capture_internal.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <string_view>
#include <thread>

using namespace pbcapturenormalize;
using namespace pbscreencapturedxgi;
using namespace pbscreencapturedxgi::detail;

namespace
{
using Clock = std::chrono::steady_clock;
constexpr std::wstring_view observerTitle = L"PixelBridge capture environment observer";
constexpr std::uint64_t initialEpoch = 17;

class WaitingConsumer final : public RawRoiConsumer
{
public:
    CaptureStatus EpochStarted(std::uint64_t, const CaptureEnvironment&, ID3D11Device*) override
    {
        epochCalls++;
        return CaptureStatus::Failure(CaptureError::ConsumerFailure, CaptureStage::Consumer);
    }
    CaptureStatus Submit(const RawRoiFrameMetadata&, ID3D11Texture2D*, ID3D11DeviceContext*) override
    {
        frameCalls++;
        return CaptureStatus::Failure(CaptureError::ConsumerFailure, CaptureStage::Consumer);
    }
    std::atomic<std::uint32_t> epochCalls{0};
    std::atomic<std::uint32_t> frameCalls{0};
};

struct ObserverWindows
{
    std::array<HWND, 16> handles{};
    std::size_t count = 0;
    bool overflow = false;
};

BOOL CALLBACK CollectObserver(HWND const window, const LPARAM parameter) noexcept
{
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != GetCurrentProcessId())
    {
        return TRUE;
    }
    std::array<wchar_t, 96> title{};
    const int length = GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
    if (length <= 0 || std::wstring_view(title.data(), static_cast<std::size_t>(length)) != observerTitle)
    {
        return TRUE;
    }
    auto& result = *reinterpret_cast<ObserverWindows*>(parameter);
    if (result.count == result.handles.size())
    {
        result.overflow = true;
        return FALSE;
    }
    result.handles[result.count++] = window;
    return TRUE;
}

ObserverWindows ReadObservers(const DWORD ownerThread = 0)
{
    ObserverWindows result;
    if (ownerThread == 0)
    {
        REQUIRE(EnumWindows(CollectObserver, reinterpret_cast<LPARAM>(&result)));
    }
    else
    {
        // EnumThreadWindows returns FALSE when the thread has no windows, too.
        // A stopped owner therefore needs an empty-result check, not a TRUE check.
        static_cast<void>(EnumThreadWindows(ownerThread, CollectObserver, reinterpret_cast<LPARAM>(&result)));
    }
    REQUIRE_FALSE(result.overflow);
    return result;
}

bool Contains(const ObserverWindows& windows, const HWND window)
{
    return std::find(windows.handles.begin(), windows.handles.begin() + windows.count, window) != windows.handles.begin() + windows.count;
}

DWORD FindCreatedOwner(const ObserverWindows& before)
{
    const auto after = ReadObservers();
    HWND created = nullptr;
    for (std::size_t index = 0; index < after.count; index++)
    {
        if (!Contains(before, after.handles[index]))
        {
            REQUIRE(created == nullptr);
            created = after.handles[index];
        }
    }
    REQUIRE(created != nullptr);
    REQUIRE_FALSE(IsWindowVisible(created));
    DWORD processId = 0;
    const DWORD ownerThread = GetWindowThreadProcessId(created, &processId);
    REQUIRE(processId == GetCurrentProcessId());
    REQUIRE(ownerThread != 0);
    REQUIRE(ownerThread != GetCurrentThreadId());
    return ownerThread;
}

HWND ReadOwnedObserver(const DWORD ownerThread)
{
    const auto observers = ReadObservers(ownerThread);
    REQUIRE(observers.count == 1);
    const HWND window = observers.handles[0];
    REQUIRE_FALSE(IsWindowVisible(window));
    DWORD processId = 0;
    REQUIRE(GetWindowThreadProcessId(window, &processId) == ownerThread);
    REQUIRE(processId == GetCurrentProcessId());
    REQUIRE(window != HWND_BROADCAST);
    return window;
}

DxgiCaptureConfig MakeUnavailableConfig()
{
    REQUIRE(AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
    const HMONITOR actualMonitor = MonitorFromPoint(POINT{}, MONITOR_DEFAULTTOPRIMARY);
    REQUIRE(actualMonitor != nullptr);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    REQUIRE(GetMonitorInfoW(actualMonitor, &monitor));
    const auto width = static_cast<std::int64_t>(monitor.rcMonitor.right) - monitor.rcMonitor.left;
    const auto height = static_cast<std::int64_t>(monitor.rcMonitor.bottom) - monitor.rcMonitor.top;
    REQUIRE(width > 0);
    REQUIRE(height > 0);
    REQUIRE(width <= 16384);
    REQUIRE(height <= 16384);
    const RECT roi{monitor.rcMonitor.left, monitor.rcMonitor.top,
                   static_cast<LONG>(static_cast<std::int64_t>(monitor.rcMonitor.left) + (std::min)(width, std::int64_t{32})),
                   static_cast<LONG>(static_cast<std::int64_t>(monitor.rcMonitor.top) + (std::min)(height, std::int64_t{32}))};
    DxgiCaptureConfig config;
    REQUIRE(pbscreenregion::ResolveScreenCaptureRegion(roi, config.region));
    REQUIRE(config.region.monitor == actualMonitor);
    const auto firstInvalidMonitor = reinterpret_cast<HMONITOR>(std::uintptr_t{1});
    config.region.monitor = actualMonitor == firstInvalidMonitor ? reinterpret_cast<HMONITOR>(std::uintptr_t{2}) : firstInvalidMonitor;
    REQUIRE(config.region.monitor != nullptr);
    REQUIRE(config.region.monitor != actualMonitor);
    config.initialCaptureEpoch = initialEpoch;
    config.queuedFrameLimit = 1;
    config.roiTextureCount = 2;
    // Dimensions are capped above, so this arithmetic fits uint64_t. The budget
    // covers the largest negotiable (FP16) surface plus small ROI/pointer
    // storage even on a large screen, matching conservative DXGI admission;
    // the intentional monitor mismatch prevents allocation or acquisition.
    config.maximumCaptureBytes = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * 8 + 1024 * 1024;
    config.gpuTimeoutMilliseconds = 200;
    REQUIRE(ValidateDxgiCaptureConfig(config));
    return config;
}

void RequireNoCapturedWork(const CaptureSnapshot& snapshot, const WaitingConsumer& consumer)
{
    REQUIRE(snapshot.error);
    REQUIRE(snapshot.captureEpoch == initialEpoch);
    REQUIRE(snapshot.deviceRecoveries == 0);
    REQUIRE(snapshot.arrivedFrames == 0);
    REQUIRE(snapshot.droppedFrames == 0);
    REQUIRE(snapshot.queuedFrames == 0);
    REQUIRE(snapshot.copiedFrames == 0);
    REQUIRE(snapshot.deliveredFrames == 0);
    REQUIRE(snapshot.liveFrameLeases == 0);
    REQUIRE(snapshot.frameLeaseHighWater == 0);
    REQUIRE(snapshot.busyRoiTextures == 0);
    REQUIRE(snapshot.acquireTimeouts == 0);
    REQUIRE(snapshot.pointerOnlyFrames == 0);
    REQUIRE(snapshot.accumulatedFrames == 0);
    REQUIRE(snapshot.accessLostEvents == 0);
    REQUIRE_FALSE(snapshot.deferredCleanup);
    REQUIRE(consumer.epochCalls == 0);
    REQUIRE(consumer.frameCalls == 0);
}

void PrintWaiting(const std::string_view phase, const CaptureSnapshot& snapshot)
{
    std::cout << "DXGI environment phase=" << phase << " attempts=" << snapshot.environmentAttempts
              << " recreates=" << snapshot.recreates << " epoch=" << snapshot.captureEpoch
              << " arrived=" << snapshot.arrivedFrames << " leases=" << snapshot.liveFrameLeases
              << " copied=" << snapshot.copiedFrames << " error=" << GetCaptureErrorName(snapshot.error.code)
              << " waitingNativeError=" << snapshot.waitingNativeError << std::endl;
}

CaptureSnapshot AwaitWaiting(DxgiCapture& capture, const WaitingConsumer& consumer, const std::uint64_t expectedRecreates)
{
    const auto deadline = Clock::now() + std::chrono::seconds(8);
    for (;;)
    {
        const auto snapshot = capture.GetSnapshot();
        RequireNoCapturedWork(snapshot, consumer);
        REQUIRE(snapshot.environmentAttempts <= 3);
        REQUIRE(snapshot.recreates <= expectedRecreates);
        if (snapshot.state == CaptureState::WaitingForEnvironment && snapshot.environmentAttempts == 3 && snapshot.recreates == expectedRecreates)
        {
            REQUIRE(snapshot.waitingNativeError == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE);
            return snapshot;
        }
        if (Clock::now() >= deadline)
        {
            PrintWaiting("timeout", snapshot);
            FAIL("Native waiting did not reach its exact bounded attempt/recreate count");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void RequireStableWaiting(DxgiCapture& capture, const WaitingConsumer& consumer, const std::uint64_t expectedRecreates)
{
    // Longer than the 1000 ms retry backoff plus the 200 ms environment cadence:
    // an exhausted budget cannot silently start another initialization cycle.
    const auto deadline = Clock::now() + std::chrono::milliseconds(1400);
    do
    {
        const auto snapshot = capture.GetSnapshot();
        RequireNoCapturedWork(snapshot, consumer);
        REQUIRE(snapshot.state == CaptureState::WaitingForEnvironment);
        REQUIRE(snapshot.environmentAttempts == 3);
        REQUIRE(snapshot.recreates == expectedRecreates);
        REQUIRE(snapshot.waitingNativeError == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE);
        REQUIRE_FALSE(snapshot.shutdownComplete);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (Clock::now() < deadline);
}

}

TEST_CASE("DXGI native unavailable environment exhausts retries and sent display notification opens one new bounded series", "[dxgi-environment]")
{
    const auto config = MakeUnavailableConfig();
    const auto before = ReadObservers();
    const auto consumer = std::make_shared<WaitingConsumer>();
    std::unique_ptr<DxgiCapture> capture;
    const auto firstSeriesStarted = Clock::now();
    REQUIRE(DxgiCaptureTestAccess::CreateWithBackend(config, consumer, MakeNativeDxgiBackend(), capture));
    REQUIRE(capture);
    const auto exhausted = AwaitWaiting(*capture, *consumer, 2);
    REQUIRE(Clock::now() - firstSeriesStarted >= std::chrono::milliseconds(1250));
    // Discover the observer only after retries stop replacing its HWND. This
    // keeps window identity checks independent of test-thread scheduling delays.
    const DWORD ownerThread = FindCreatedOwner(before);
    PrintWaiting("initial-exhausted", exhausted);
    RequireStableWaiting(*capture, *consumer, 2);
    static_cast<void>(ReadOwnedObserver(ownerThread));

    // A caller's explicit resource rebuild is not a newly observed environment.
    // It may replace the watch, but must not reopen the exhausted attempt budget.
    DxgiCaptureTestAccess::RequestRecreate(*capture);
    PrintWaiting("explicit-recreate-exhausted", AwaitWaiting(*capture, *consumer, 3));
    RequireStableWaiting(*capture, *consumer, 3);
    const HWND observer = ReadOwnedObserver(ownerThread);

    // Send to this test's sole hidden observer only; never broadcast and never
    // change the actual monitor mode. A sent (not posted) same-mode notification
    // must be observed by the real WndProc/Poll path, even with unchanged geometry.
    DWORD_PTR messageResult = 0;
    const auto secondSeriesStarted = Clock::now();
    SetLastError(ERROR_SUCCESS);
    const LRESULT sent = SendMessageTimeoutW(observer, WM_DISPLAYCHANGE, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT, 2000, &messageResult);
    INFO("SendMessageTimeout lastError=" << GetLastError());
    REQUIRE(sent != 0);
    const auto reopened = AwaitWaiting(*capture, *consumer, 6);
    REQUIRE(Clock::now() - secondSeriesStarted >= std::chrono::milliseconds(1250));
    REQUIRE(reopened.lastRebuildReason == CaptureRebuildReason::DisplayChanged);
    PrintWaiting("notification-exhausted", reopened);
    RequireStableWaiting(*capture, *consumer, 6);

    const HWND finalObserver = ReadOwnedObserver(ownerThread);
    std::array<wchar_t, 128> observerClass{};
    REQUIRE(GetClassNameW(finalObserver, observerClass.data(), static_cast<int>(observerClass.size())) > 0);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    REQUIRE(GetClassInfoExW(GetModuleHandleW(nullptr), observerClass.data(), &windowClass));
    const auto stopStarted = Clock::now();
    REQUIRE(capture->Stop());
    REQUIRE(Clock::now() - stopStarted < std::chrono::seconds(2));
    REQUIRE(capture->Stop());
    const auto stopped = capture->GetSnapshot();
    RequireNoCapturedWork(stopped, *consumer);
    REQUIRE(stopped.state == CaptureState::Stopped);
    REQUIRE(stopped.shutdownComplete);
    REQUIRE(stopped.environmentAttempts == 3);
    REQUIRE(stopped.recreates == 6);
    REQUIRE(ReadObservers(ownerThread).count == 0);
    REQUIRE_FALSE(IsWindow(finalObserver));
    REQUIRE_FALSE(GetClassInfoExW(GetModuleHandleW(nullptr), observerClass.data(), &windowClass));
    PrintWaiting("stopped", stopped);
}
