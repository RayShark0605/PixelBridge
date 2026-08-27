#include "capture_test_support.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <type_traits>

using namespace capturetest;

TEST_CASE("WGC configuration validates signed physical geometry and total resource budgets")
{
    const auto baseline = MakeConfig();
    REQUIRE(ValidateWgcCaptureConfig(baseline));
    CaptureEnvironment environment;
    environment.region = baseline.region;
    environment.contentSize = {100, 80};
    CaptureLayout layout;
    REQUIRE(ValidateLayout(baseline, environment, layout));
    REQUIRE(layout.sourceBox.left == 10);
    REQUIRE(layout.sourceBox.top == 5);
    REQUIRE(layout.sourceBox.right == 40);
    REQUIRE(layout.sourceBox.bottom == 25);
    REQUIRE(layout.roiWidth == 30);
    REQUIRE(layout.roiHeight == 20);
    REQUIRE(layout.poolBufferCount == 6);
    REQUIRE(layout.totalBytes == 100 * 80 * 4 * 6 + 30 * 20 * 4 * 3);
    for (const auto limit : {0u, 5u, std::numeric_limits<std::uint32_t>::max()})
    {
        auto config = baseline;
        config.queuedFrameLimit = limit;
        REQUIRE(ValidateWgcCaptureConfig(config).code == CaptureError::InvalidConfiguration);
    }
    for (const auto count : {0u, 1u, 9u, std::numeric_limits<std::uint32_t>::max()})
    {
        auto config = baseline;
        config.roiTextureCount = count;
        REQUIRE(ValidateWgcCaptureConfig(config).code == CaptureError::InvalidConfiguration);
    }
    auto config = baseline;
    config.maximumRoiBytes = 30 * 20 * 4 * 3 - 1;
    REQUIRE(ValidateWgcCaptureConfig(config).code == CaptureError::ResourceLimit);
    config.maximumRoiBytes++;
    config.maximumCaptureBytes = layout.totalBytes - 1;
    REQUIRE(ValidateWgcCaptureConfig(config).code == CaptureError::ResourceLimit);
    config.maximumCaptureBytes++;
    REQUIRE(ValidateWgcCaptureConfig(config));
    config = baseline;
    config.region.physicalRect.left = std::numeric_limits<LONG>::min();
    config.region.physicalRect.right = std::numeric_limits<LONG>::max();
    REQUIRE_FALSE(ValidateWgcCaptureConfig(config));
    config = baseline;
    config.region.rotation = DXGI_MODE_ROTATION_UNSPECIFIED;
    REQUIRE_FALSE(ValidateWgcCaptureConfig(config));
    config = baseline;
    config.pixelFormat = DXGI_FORMAT_R8_UNORM;
    REQUIRE_FALSE(ValidateWgcCaptureConfig(config));
    config = baseline;
    config.minUpdateInterval100ns = -1;
    REQUIRE_FALSE(ValidateWgcCaptureConfig(config));
    config.minUpdateInterval100ns = 10000001;
    REQUIRE_FALSE(ValidateWgcCaptureConfig(config));
    config.minUpdateInterval100ns = 0;
    REQUIRE(ValidateWgcCaptureConfig(config));
    config.initialCaptureEpoch = std::numeric_limits<std::uint64_t>::max();
    REQUIRE_FALSE(ValidateWgcCaptureConfig(config));
}

TEST_CASE("WGC recreate reconciles ContentSize and live monitor geometry without a cached item size")
{
    const auto config = MakeConfig();
    CaptureEnvironment previous;
    previous.region = config.region;
    previous.contentSize = {100, 80};
    auto current = config.region;
    current.monitorPhysicalRect.right = 20;
    current.monitorPhysicalRect.bottom = 110;
    CaptureSize output{7, 9};
    REQUIRE(ResolveRecreateContentSize({120, 90}, previous, current, output));
    REQUIRE(output == CaptureSize{120, 90});
    REQUIRE(ResolveRecreateContentSize(previous.contentSize, previous, current, output));
    REQUIRE(output == CaptureSize{120, 90});
    for (const CaptureSize size : {CaptureSize{0, 90}, {120, -1}, {16385, 90}, {119, 90}, {120, 89}})
    {
        output = {7, 9};
        REQUIRE(ResolveRecreateContentSize(size, previous, current, output) ==
                CaptureStatus::Failure(CaptureError::RegionChanged, CaptureStage::Recreate));
        REQUIRE(output == CaptureSize{7, 9});
    }
    current.monitorPhysicalRect.left = std::numeric_limits<LONG>::min();
    current.monitorPhysicalRect.right = std::numeric_limits<LONG>::max();
    REQUIRE_FALSE(ResolveRecreateContentSize(previous.contentSize, previous, current, output));
    REQUIRE(output == CaptureSize{7, 9});
    current = config.region;
    current.monitor = nullptr;
    REQUIRE_FALSE(ResolveRecreateContentSize(previous.contentSize, previous, current, output));
    REQUIRE(output == CaptureSize{7, 9});
}

TEST_CASE("WGC lease is move-only and closes exactly once, including failed Close")
{
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<FrameLease>);
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<FrameLease>);
    const auto control = std::make_shared<Control>();
    control->inbox.store(std::make_shared<FrameInbox>(MakeConfig()));
    {
        auto first = MakeFrame(control, 1);
        auto second = MakeFrame(control, 2, {100, 80}, 1, E_FAIL);
        second = std::move(first);
        REQUIRE_FALSE(first);
        REQUIRE(second);
        REQUIRE(control->inbox.load()->counters->live == 1);
        FrameLease third(std::move(second));
        third.Reset();
        third.Reset();
    }
    REQUIRE(control->inbox.load()->counters->live == 0);
    REQUIRE(control->inbox.load()->counters->highWater == 2);
    REQUIRE(control->inbox.load()->counters->closeError == E_FAIL);
    const auto events = control->Events();
    REQUIRE(events.size() == 2);
    REQUIRE(events[0].id == 2);
    REQUIRE(events[1].id == 1);
}

TEST_CASE("WGC bounded inbox drops stale frames and closes outside the queue lock")
{
    const auto control = std::make_shared<Control>();
    const auto inbox = std::make_shared<FrameInbox>(MakeConfig());
    control->inbox.store(inbox);
    inbox->Resume({100, 80}, 1);
    control->closeHook = [inbox]
    {
        static_cast<void>(inbox->GetSnapshot());
    };
    for (int id = 1; id <= 100; id++)
    {
        inbox->Push(MakeFrame(control, id));
        REQUIRE(inbox->GetSnapshot().queuedFrames <= 2);
    }
    REQUIRE(inbox->counters->live == 2);
    REQUIRE(inbox->counters->highWater == 3);
    FrameLease newest;
    REQUIRE(inbox->TakeNewest(newest));
    REQUIRE(newest.timestamp100ns == 100);
    REQUIRE(inbox->GetSnapshot().droppedFrames == 99);
    newest.Reset();
    REQUIRE(inbox->counters->live == 0);
    inbox->Push(MakeFrame(control, 101, {100, 80}, 0));
    REQUIRE(inbox->GetSnapshot().queuedFrames == 0);
    control->closeHook = {};
}

TEST_CASE("WGC out-of-lock stale retirement remains charged while a producer refills the inbox")
{
    const auto control = std::make_shared<Control>();
    const auto inbox = std::make_shared<FrameInbox>(MakeConfig());
    control->inbox.store(inbox);
    inbox->Resume({100, 80}, 1);
    auto activeFirst = MakeFrame(control, 1);
    auto activeSecond = MakeFrame(control, 2);
    inbox->Push(MakeFrame(control, 3));
    inbox->Push(MakeFrame(control, 4));
    std::atomic<bool> firstClose{true};
    std::atomic<bool> closeEntered{false};
    std::atomic<bool> allowClose{false};
    control->closeHook = [&]
    {
        if (firstClose.exchange(false))
        {
            closeEntered = true;
            static_cast<void>(WaitFor([&]
            {
                return allowClose.load();
            }));
        }
    };
    FrameLease newest;
    bool taken = false;
    std::jthread owner([&]
    {
        taken = inbox->TakeNewest(newest);
    });
    REQUIRE(WaitFor([&]
    {
        return closeEntered.load();
    }));
    for (int id = 5; id <= 104; id++)
    {
        inbox->Push(MakeFrame(control, id));
        REQUIRE(inbox->GetSnapshot().queuedFrames == 1);
    }
    REQUIRE(inbox->counters->highWater <= 6);
    allowClose = true;
    owner.join();
    REQUIRE(taken);
    REQUIRE(newest.timestamp100ns == 4);
    newest.Reset();
    activeFirst.Reset();
    activeSecond.Reset();
    inbox->Pause();
    REQUIRE(inbox->counters->live == 0);
    control->closeHook = {};
}

TEST_CASE("WGC metadata rejection pauses admission without submitting stale geometry")
{
    for (const CaptureSize size : {CaptureSize{0, 80}, {-1, 80}, {16385, 80}, {100, 0}, {100, 16385}, {101, 80}})
    {
        const auto control = std::make_shared<Control>();
        const auto inbox = std::make_shared<FrameInbox>(MakeConfig());
        control->inbox.store(inbox);
        inbox->Resume({100, 80}, 1);
        inbox->Push(MakeFrame(control, 1));
        inbox->Push(MakeFrame(control, 2, size));
        FrameLease output;
        REQUIRE_FALSE(inbox->TakeNewest(output));
        const auto snapshot = inbox->GetSnapshot();
        REQUIRE((snapshot.recreateRequested || !snapshot.error));
        if (size == CaptureSize{101, 80})
        {
            REQUIRE(snapshot.error);
            REQUIRE(snapshot.requestedSize == size);
        }
        else
        {
            REQUIRE(snapshot.error.code == CaptureError::InvalidFrame);
            REQUIRE(snapshot.error.stage == CaptureStage::Callback);
        }
        inbox->Pause();
        REQUIRE(inbox->counters->live == 0);
    }
}

TEST_CASE("WGC source lease survives delayed GPU copy, ROI ring survives delayed consumer")
{
    const auto control = std::make_shared<Control>();
    control->consumeComplete = false;
    const auto consumer = std::make_shared<Consumer>(control);
    std::unique_ptr<WgcCapture> capture;
    REQUIRE(WgcCaptureTestAccess::Create(MakeConfig(), consumer, std::make_unique<Backend>(control), capture));
    for (int id = 1; id <= 3; id++)
    {
        control->inbox.load()->Push(MakeFrame(control, id));
        REQUIRE(WaitFor([&]
        {
            return control->copies == id;
        }));
    }
    for (int id = 4; id <= 20; id++)
    {
        control->inbox.load()->Push(MakeFrame(control, id));
    }
    REQUIRE(control->consumes == 0);
    REQUIRE(capture->GetSnapshot().liveFrameLeases == 5);
    REQUIRE(capture->GetSnapshot().frameLeaseHighWater <= 6);
    auto events = control->Events();
    REQUIRE(FindEvent(events, "close", 1) == events.size());
    control->copyComplete = true;
    REQUIRE(WaitFor([&]
    {
        return control->consumes == 3;
    }));
    REQUIRE(control->copies == 3);
    REQUIRE(WaitFor([&]
    {
        return capture->GetSnapshot().liveFrameLeases == 2;
    }));
    control->consumeComplete = true;
    REQUIRE(WaitFor([&]
    {
        return control->consumes == 4;
    }));
    REQUIRE(capture->Stop());
    REQUIRE(capture->Stop());
    events = control->Events();
    for (const int id : {1, 2, 3, 20})
    {
        REQUIRE(FindEvent(events, "copy-done", id) < FindEvent(events, "close", id));
        REQUIRE(FindEvent(events, "close", id) < FindEvent(events, "consumer", id));
        REQUIRE(events[FindEvent(events, "copy", id)].thread != std::this_thread::get_id());
    }
    REQUIRE(capture->GetSnapshot().shutdownComplete);
    REQUIRE(capture->GetSnapshot().liveFrameLeases == 0);
}

TEST_CASE("WGC mismatch drains source and consumer markers before Recreate and epoch increment")
{
    const auto control = std::make_shared<Control>();
    const auto consumer = std::make_shared<Consumer>(control);
    std::unique_ptr<WgcCapture> capture;
    REQUIRE(WgcCaptureTestAccess::Create(MakeConfig(), consumer, std::make_unique<Backend>(control), capture));
    control->inbox.load()->Push(MakeFrame(control, 1));
    REQUIRE(WaitFor([&]
    {
        return control->copies == 1;
    }));
    control->callbacksIdle = false;
    control->inbox.load()->Push(MakeFrame(control, 2, {120, 90}));
    REQUIRE(WaitFor([&]
    {
        return capture->GetSnapshot().state == CaptureState::Draining;
    }));
    REQUIRE(control->recreates == 0);
    REQUIRE(capture->GetSnapshot().liveFrameLeases == 1);
    control->copyComplete = true;
    REQUIRE(WaitFor([&]
    {
        return capture->GetSnapshot().liveFrameLeases == 0;
    }));
    REQUIRE(control->recreates == 0);
    control->callbacksIdle = true;
    REQUIRE(WaitFor([&]
    {
        return capture->GetSnapshot().captureEpoch == 2 && capture->GetSnapshot().state == CaptureState::Running;
    }));
    REQUIRE(control->recreates == 1);
    REQUIRE_FALSE(control->unsafeRecreate);
    REQUIRE(control->consumes == 0);
    control->inbox.load()->Push(MakeFrame(control, 3, {100, 80}, 1));
    control->inbox.load()->Push(MakeFrame(control, 4, {120, 90}, 2));
    REQUIRE(WaitFor([&]
    {
        return control->consumes == 1;
    }));
    REQUIRE(capture->Stop());
    const auto events = control->Events();
    REQUIRE(FindEvent(events, "copy", 3) == events.size());
    REQUIRE(events[FindEvent(events, "consumer", 4)].epoch == 2);
}

TEST_CASE("WGC resize also waits for downstream GPU use after the source lease was closed")
{
    const auto control = std::make_shared<Control>();
    control->copyComplete = true;
    control->consumeComplete = false;
    std::unique_ptr<WgcCapture> capture;
    REQUIRE(WgcCaptureTestAccess::Create(MakeConfig(), std::make_shared<Consumer>(control), std::make_unique<Backend>(control), capture));
    control->inbox.load()->Push(MakeFrame(control, 1));
    REQUIRE(WaitFor([&]
    {
        return control->consumes == 1;
    }));
    REQUIRE(capture->GetSnapshot().liveFrameLeases == 0);
    control->inbox.load()->Push(MakeFrame(control, 2, {120, 90}));
    REQUIRE(WaitFor([&]
    {
        return capture->GetSnapshot().state == CaptureState::Draining;
    }));
    REQUIRE(control->recreates == 0);
    REQUIRE(capture->GetSnapshot().captureEpoch == 1);
    control->consumeComplete = true;
    REQUIRE(WaitFor([&]
    {
        return capture->GetSnapshot().captureEpoch == 2 && capture->GetSnapshot().state == CaptureState::Running;
    }));
    REQUIRE(capture->Stop());
    const auto events = control->Events();
    REQUIRE(FindEvent(events, "consume-done", 1) < FindEvent(events, "recreate", 0));
}

TEST_CASE("WGC frame Close failure fails closed before consumer delivery")
{
    const auto control = std::make_shared<Control>();
    control->copyComplete = true;
    std::unique_ptr<WgcCapture> capture;
    REQUIRE(WgcCaptureTestAccess::Create(MakeConfig(), std::make_shared<Consumer>(control), std::make_unique<Backend>(control), capture));
    control->inbox.load()->Push(MakeFrame(control, 1, {100, 80}, 1, E_FAIL));
    REQUIRE(WaitFor([&]
    {
        return capture->GetSnapshot().shutdownComplete;
    }));
    REQUIRE(capture->Stop() == CaptureStatus::Failure(CaptureError::NativeFailure, CaptureStage::Completion, E_FAIL));
    REQUIRE(control->consumes == 0);
    REQUIRE(capture->GetSnapshot().liveFrameLeases == 0);
}

TEST_CASE("WGC partial submission failure still retires the source before teardown")
{
    for (const bool before : {false, true})
    {
        const auto control = std::make_shared<Control>();
        control->failCopyBefore = before;
        control->failCopyAfter = !before;
        std::unique_ptr<WgcCapture> capture;
        REQUIRE(WgcCaptureTestAccess::Create(MakeConfig(), std::make_shared<Consumer>(control), std::make_unique<Backend>(control), capture));
        control->inbox.load()->Push(MakeFrame(control, 1));
        REQUIRE(WaitFor([&]
        {
            return !capture->GetSnapshot().error;
        }));
        if (!before)
        {
            REQUIRE(capture->GetSnapshot().liveFrameLeases == 1);
        }
        control->copyComplete = true;
        REQUIRE_FALSE(capture->Stop());
        const auto snapshot = capture->GetSnapshot();
        REQUIRE(snapshot.error.code == CaptureError::NativeFailure);
        REQUIRE(snapshot.error.stage == CaptureStage::Copy);
        REQUIRE(snapshot.liveFrameLeases == 0);
        REQUIRE(snapshot.shutdownComplete);
        REQUIRE(control->consumes == 0);
    }
}

TEST_CASE("WGC shutdown timeout keeps a bounded lease alive, even after capture destruction")
{
    const auto control = std::make_shared<Control>();
    auto config = MakeConfig();
    config.gpuTimeoutMilliseconds = 20;
    std::unique_ptr<WgcCapture> capture;
    REQUIRE(WgcCaptureTestAccess::Create(config, std::make_shared<Consumer>(control), std::make_unique<Backend>(control), capture));
    control->inbox.load()->Push(MakeFrame(control, 1));
    REQUIRE(WaitFor([&]
    {
        return control->copies == 1;
    }));
    REQUIRE(capture->Stop().code == CaptureError::Timeout);
    REQUIRE(capture->GetSnapshot().deferredCleanup);
    REQUIRE_FALSE(capture->GetSnapshot().shutdownComplete);
    REQUIRE(control->HasDeferred());
    capture.reset();
    REQUIRE(control->inbox.load()->counters->live == 1);
    REQUIRE(control->shutdowns == 0);
    control->copyComplete = true;
    control->CompleteDeferred();
    REQUIRE(control->inbox.load()->counters->live == 0);
    REQUIRE(control->shutdowns == 1);
}

TEST_CASE("WGC device removal is authoritative, recovery is bounded and advances epochs")
{
    const auto control = std::make_shared<Control>();
    std::unique_ptr<WgcCapture> capture;
    REQUIRE(WgcCaptureTestAccess::Create(MakeConfig(), std::make_shared<Consumer>(control), std::make_unique<Backend>(control), capture));
    control->inbox.load()->Push(MakeFrame(control, 1));
    REQUIRE(WaitFor([&]
    {
        return control->copies == 1;
    }));
    control->deviceRemoved = true;
    REQUIRE(WaitFor([&]
    {
        return capture->GetSnapshot().deviceRecoveries == 1 && capture->GetSnapshot().state == CaptureState::Running;
    }));
    REQUIRE(capture->GetSnapshot().captureEpoch == 2);
    REQUIRE(capture->GetSnapshot().lastDeviceLoss.nativeError == DXGI_ERROR_DEVICE_REMOVED);
    REQUIRE(capture->GetSnapshot().liveFrameLeases == 0);
    control->deviceRemoved = true;
    REQUIRE(WaitFor([&]
    {
        return capture->GetSnapshot().shutdownComplete;
    }));
    REQUIRE(capture->Stop().code == CaptureError::DeviceLost);
    REQUIRE(control->initializes == 2);
    REQUIRE(capture->GetSnapshot().captureEpoch == 3);
}

TEST_CASE("WGC startup, Recreate, callback and consumer failure paths terminate cleanly")
{
    SECTION("startup is transactional")
    {
        const auto control = std::make_shared<Control>();
        control->startError = CaptureStatus::Failure(CaptureError::AccessLost, CaptureStage::Session, E_ACCESSDENIED);
        std::unique_ptr<WgcCapture> capture;
        const auto status = WgcCaptureTestAccess::Create(MakeConfig(), std::make_shared<Consumer>(control), std::make_unique<Backend>(control), capture);
        REQUIRE(status == control->startError);
        REQUIRE_FALSE(capture);
        REQUIRE(control->shutdowns == 1);
    }
    SECTION("Recreate failure")
    {
        const auto control = std::make_shared<Control>();
        control->failRecreate = true;
        std::unique_ptr<WgcCapture> capture;
        REQUIRE(WgcCaptureTestAccess::Create(MakeConfig(), std::make_shared<Consumer>(control), std::make_unique<Backend>(control), capture));
        control->inbox.load()->Push(MakeFrame(control, 1, {120, 90}));
        REQUIRE(WaitFor([&]
        {
            return capture->GetSnapshot().shutdownComplete;
        }));
        REQUIRE(capture->Stop().stage == CaptureStage::Recreate);
        REQUIRE(capture->GetSnapshot().captureEpoch == 1);
    }
    SECTION("failed Create leaves an existing output instance untouched")
    {
        const auto originalControl = std::make_shared<Control>();
        std::unique_ptr<WgcCapture> capture;
        REQUIRE(WgcCaptureTestAccess::Create(MakeConfig(), std::make_shared<Consumer>(originalControl), std::make_unique<Backend>(originalControl), capture));
        const auto* const original = capture.get();
        const auto failedControl = std::make_shared<Control>();
        failedControl->initializeError = CaptureStatus::Failure(CaptureError::OutOfMemory, CaptureStage::Device, E_OUTOFMEMORY);
        REQUIRE(WgcCaptureTestAccess::Create(MakeConfig(), std::make_shared<Consumer>(failedControl), std::make_unique<Backend>(failedControl), capture) ==
                failedControl->initializeError);
        REQUIRE(capture.get() == original);
        REQUIRE(capture->GetSnapshot().state == CaptureState::Running);
        REQUIRE(failedControl->shutdowns == 1);
        REQUIRE(capture->Stop());
    }
    SECTION("EpochStarted failure does not start acquisition")
    {
        const auto control = std::make_shared<Control>();
        const auto consumer = std::make_shared<Consumer>(control);
        consumer->epochError = CaptureStatus::Failure(CaptureError::ConsumerFailure, CaptureStage::Consumer, E_FAIL);
        std::unique_ptr<WgcCapture> capture;
        REQUIRE(WgcCaptureTestAccess::Create(MakeConfig(), consumer, std::make_unique<Backend>(control), capture) == consumer->epochError);
        REQUIRE_FALSE(capture);
        REQUIRE(control->shutdowns == 1);
        const auto events = control->Events();
        REQUIRE(FindEvent(events, "start", 0) == events.size());
    }
    SECTION("shutdown failure preserves its original native error")
    {
        const auto control = std::make_shared<Control>();
        control->shutdownError = CaptureStatus::Failure(CaptureError::NativeFailure, CaptureStage::Shutdown, E_FAIL);
        std::unique_ptr<WgcCapture> capture;
        REQUIRE(WgcCaptureTestAccess::Create(MakeConfig(), std::make_shared<Consumer>(control), std::make_unique<Backend>(control), capture));
        REQUIRE(capture->Stop() == control->shutdownError);
        REQUIRE(capture->GetSnapshot().shutdownComplete);
        REQUIRE(capture->GetSnapshot().liveFrameLeases == 0);
    }
    SECTION("late old-epoch callback error cannot poison a new epoch")
    {
        const auto control = std::make_shared<Control>();
        std::unique_ptr<WgcCapture> capture;
        REQUIRE(WgcCaptureTestAccess::Create(MakeConfig(), std::make_shared<Consumer>(control), std::make_unique<Backend>(control), capture));
        WgcCaptureTestAccess::RequestRecreate(*capture);
        REQUIRE(WaitFor([&]
        {
            return capture->GetSnapshot().captureEpoch == 2;
        }));
        control->inbox.load()->ReportError(CaptureStatus::Failure(CaptureError::AccessLost, CaptureStage::Callback), 1);
        REQUIRE(capture->GetSnapshot().error);
        control->inbox.load()->ReportError(CaptureStatus::Failure(CaptureError::AccessLost, CaptureStage::Callback), 2);
        REQUIRE(WaitFor([&]
        {
            return capture->GetSnapshot().shutdownComplete;
        }));
        REQUIRE(capture->Stop().code == CaptureError::AccessLost);
    }
    SECTION("consumer exceptions still insert and drain a marker")
    {
        const auto control = std::make_shared<Control>();
        control->copyComplete = true;
        control->consumeComplete = false;
        const auto consumer = std::make_shared<Consumer>(control);
        consumer->throwOnSubmit = true;
        std::unique_ptr<WgcCapture> capture;
        REQUIRE(WgcCaptureTestAccess::Create(MakeConfig(), consumer, std::make_unique<Backend>(control), capture));
        control->inbox.load()->Push(MakeFrame(control, 1));
        REQUIRE(WaitFor([&]
        {
            return capture->GetSnapshot().error.code == CaptureError::ConsumerFailure;
        }));
        REQUIRE(capture->GetSnapshot().busyRoiTextures == 1);
        control->consumeComplete = true;
        REQUIRE(capture->Stop().code == CaptureError::ConsumerFailure);
        REQUIRE(capture->GetSnapshot().shutdownComplete);
    }
}

TEST_CASE("WGC producer races and reentrant stop do not block on the owner join mutex")
{
    for (int round = 0; round < 20; round++)
    {
        const auto control = std::make_shared<Control>();
        control->copyComplete = true;
        const auto consumer = std::make_shared<Consumer>(control);
        std::unique_ptr<WgcCapture> capture;
        REQUIRE(WgcCaptureTestAccess::Create(MakeConfig(), consumer, std::make_unique<Backend>(control), capture));
        std::atomic<int> stopCode{-1};
        consumer->submitHook = [&]
        {
            stopCode = static_cast<int>(capture->Stop().code);
        };
        std::thread producer([control]
        {
            for (int id = 1; id <= 200; id++)
            {
                control->inbox.load()->Push(MakeFrame(control, id));
            }
        });
        producer.join();
        REQUIRE(WaitFor([&]
        {
            return stopCode != -1;
        }));
        REQUIRE(capture->Stop());
        REQUIRE(stopCode == static_cast<int>(CaptureError::WrongThread));
        REQUIRE(capture->GetSnapshot().liveFrameLeases == 0);
        REQUIRE(capture->GetSnapshot().frameLeaseHighWater <= 6);
    }
}

TEST_CASE("WGC a query error without device removal is not permission to release a GPU source")
{
    const auto control = std::make_shared<Control>();
    control->failPoll = true;
    std::unique_ptr<WgcCapture> capture;
    REQUIRE(WgcCaptureTestAccess::Create(MakeConfig(), std::make_shared<Consumer>(control), std::make_unique<Backend>(control), capture));
    control->inbox.load()->Push(MakeFrame(control, 1));
    REQUIRE(WaitFor([&]
    {
        return capture->GetSnapshot().error.code == CaptureError::NativeFailure;
    }));
    REQUIRE(capture->GetSnapshot().liveFrameLeases == 1);
    REQUIRE(control->shutdowns == 0);
    REQUIRE(control->initializes == 1);
    control->failPoll = false;
    control->copyComplete = true;
    REQUIRE(capture->Stop().stage == CaptureStage::Completion);
    REQUIRE(capture->GetSnapshot().liveFrameLeases == 0);
    REQUIRE(capture->GetSnapshot().shutdownComplete);
}

TEST_CASE("WGC epoch overflow and failed device recovery fail closed without retry loops")
{
    SECTION("epoch exhaustion never wraps to zero")
    {
        const auto control = std::make_shared<Control>();
        auto config = MakeConfig();
        config.initialCaptureEpoch = std::numeric_limits<std::uint64_t>::max() - 1;
        std::unique_ptr<WgcCapture> capture;
        REQUIRE(WgcCaptureTestAccess::Create(config, std::make_shared<Consumer>(control), std::make_unique<Backend>(control), capture));
        WgcCaptureTestAccess::RequestRecreate(*capture);
        REQUIRE(WaitFor([&]
        {
            return capture->GetSnapshot().captureEpoch == std::numeric_limits<std::uint64_t>::max();
        }));
        WgcCaptureTestAccess::RequestRecreate(*capture);
        REQUIRE(WaitFor([&]
        {
            return capture->GetSnapshot().shutdownComplete;
        }));
        REQUIRE(capture->Stop().code == CaptureError::ResourceLimit);
        REQUIRE(capture->GetSnapshot().captureEpoch == std::numeric_limits<std::uint64_t>::max());
    }
    SECTION("one failed recreation does not spin")
    {
        const auto control = std::make_shared<Control>();
        control->failRecovery = true;
        std::unique_ptr<WgcCapture> capture;
        REQUIRE(WgcCaptureTestAccess::Create(MakeConfig(), std::make_shared<Consumer>(control), std::make_unique<Backend>(control), capture));
        control->deviceRemoved = true;
        REQUIRE(WaitFor([&]
        {
            return capture->GetSnapshot().shutdownComplete;
        }));
        REQUIRE(capture->Stop().code == CaptureError::NativeFailure);
        REQUIRE(control->initializes == 2);
    }
}
