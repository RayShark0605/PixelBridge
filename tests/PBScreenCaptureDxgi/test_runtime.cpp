#include "test_support.h"

using namespace dxgitest;

TEST_CASE("DXGI shared owner retires the one source before reacquiring while consumer work remains bounded")
{
    const auto control = std::make_shared<Control>();
    control->Push(Image(100, 100, true));
    control->Push(Image(200, 200, false));
    control->consumeComplete = false;
    std::unique_ptr<DxgiCapture> capture;
    REQUIRE(Create(control, capture));
    REQUIRE(WaitFor([&] { return control->copies == 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(control->acquired == 1);
    REQUIRE(control->releases == 0);
    REQUIRE(control->consumes == 0);
    REQUIRE(capture->GetSnapshot().liveFrameLeases == 1);
    control->copyComplete = true;
    REQUIRE(WaitFor([&] { return control->consumes == 2; }));
    REQUIRE(control->releases == 2);
    REQUIRE_FALSE(control->unsafeAcquire);
    const auto delivered = control->Delivered();
    REQUIRE(delivered[0].cursorState == CursorState::SeparatePointer);
    REQUIRE(delivered[1].cursorState == CursorState::PossiblyComposited);
    REQUIRE(delivered[0].pointer.rawUpdateTimestamp == 100);
    REQUIRE(delivered[1].pointer.rawUpdateTimestamp == 200);
    REQUIRE(delivered[0].rawFrequency == 1000);
    REQUIRE_FALSE(delivered[0].capabilities.cursorExcluded);
    REQUIRE(capture->GetSnapshot().busyRoiTextures == 2);
    control->consumeComplete = true;
    REQUIRE(capture->Stop());
    REQUIRE(capture->Stop());
    REQUIRE(capture->GetSnapshot().shutdownComplete);
    REQUIRE(capture->GetSnapshot().liveFrameLeases == 0);
    REQUIRE(capture->GetSnapshot().frameLeaseHighWater == 1);
}

TEST_CASE("DXGI shared owner access-loss recovery advances epoch and never delivers the invalid old image")
{
    for (int failurePoint = 0; failurePoint < 3; failurePoint++)
    {
        const auto control = std::make_shared<Control>();
        auto acquisition = Image(100, 100, true);
        if (failurePoint == 0)
        {
            acquisition.acquireResult = DXGI_ERROR_ACCESS_LOST;
        }
        else if (failurePoint == 1)
        {
            acquisition.info.PointerShapeBufferSize = 64;
            acquisition.shapeResult = DXGI_ERROR_ACCESS_LOST;
        }
        else
        {
            acquisition.releaseResult = DXGI_ERROR_ACCESS_LOST;
        }
        control->Push(acquisition);
        control->Push(Image(200, 200, true));
        control->copyComplete = true;
        std::unique_ptr<DxgiCapture> capture;
        REQUIRE(Create(control, capture));
        REQUIRE(WaitFor([&] { return control->consumes == 1; }));
        REQUIRE(capture->Stop());
        REQUIRE(control->recreates == 1);
        REQUIRE_FALSE(control->unsafeRecreate);
        REQUIRE(control->Delivered().front().captureEpoch == 2);
        REQUIRE(control->Delivered().front().rawTimestamp == 200);
        REQUIRE(capture->GetSnapshot().accessLostEvents == 1);
        REQUIRE(control->releases == (failurePoint == 0 ? 1u : 2u));
        REQUIRE(control->releases == control->acquired);
    }
}

TEST_CASE("DXGI waiting remains stoppable and only notifies consumer when a real environment recovers")
{
    const auto control = std::make_shared<Control>();
    control->available = false;
    std::unique_ptr<DxgiCapture> capture;
    REQUIRE(Create(control, capture));
    REQUIRE(capture->GetSnapshot().state == CaptureState::WaitingForEnvironment);
    REQUIRE(control->Epochs().empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    REQUIRE(control->acquireCalls == 0);
    control->available = true;
    control->environmentChanged = true;
    control->copyComplete = true;
    control->Push(Image(100, 100, true));
    REQUIRE(WaitFor([&] { return control->consumes == 1; }));
    REQUIRE(control->Epochs() == std::vector<std::uint64_t>{1});
    REQUIRE(capture->Stop());
    const auto unavailable = std::make_shared<Control>();
    unavailable->available = false;
    REQUIRE(Create(unavailable, capture));
    const auto start = std::chrono::steady_clock::now();
    REQUIRE(capture->Stop());
    REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
    REQUIRE(capture->GetSnapshot().shutdownComplete);
}

TEST_CASE("DXGI full drain precedes recreate and copy errors preserve the submitted lifetime distinction")
{
    for (const bool submitted : {false, true})
    {
        const auto control = std::make_shared<Control>();
        control->Push(Image());
        control->copyFailsBefore = !submitted;
        control->copyFailsAfter = submitted;
        std::unique_ptr<DxgiCapture> capture;
        REQUIRE(Create(control, capture));
        if (submitted)
        {
            REQUIRE(WaitFor([&] { return control->copies == 1; }));
            REQUIRE(control->releases == 0);
            control->copyComplete = true;
        }
        REQUIRE(WaitFor([&] { return capture->GetSnapshot().shutdownComplete; }));
        REQUIRE(capture->Stop().code == CaptureError::NativeFailure);
        REQUIRE(control->releases == 1);
        REQUIRE(control->consumes == 0);
        REQUIRE(capture->GetSnapshot().liveFrameLeases == 0);
    }
    const auto control = std::make_shared<Control>();
    control->Push(Image());
    std::unique_ptr<DxgiCapture> capture;
    REQUIRE(Create(control, capture));
    REQUIRE(WaitFor([&] { return control->copies == 1; }));
    DxgiCaptureTestAccess::RequestRecreate(*capture);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(control->recreates == 0);
    REQUIRE(control->releases == 0);
    control->copyComplete = true;
    REQUIRE(WaitFor([&] { return control->recreates == 1; }));
    REQUIRE(capture->Stop());
    REQUIRE(control->consumes == 0);
    REQUIRE(control->releases == 1);
    REQUIRE_FALSE(control->unsafeRecreate);
}

TEST_CASE("DXGI GPU timeout retains the exact source lease until deferred completion")
{
    const auto control = std::make_shared<Control>();
    control->Push(Image());
    auto config = MakeConfig();
    config.gpuTimeoutMilliseconds = 20;
    std::unique_ptr<DxgiCapture> capture;
    REQUIRE(Create(control, capture, config));
    REQUIRE(WaitFor([&] { return control->copies == 1; }));
    capture->RequestStop();
    REQUIRE(capture->Stop().code == CaptureError::Timeout);
    REQUIRE(capture->GetSnapshot().deferredCleanup);
    REQUIRE(control->releases == 0);
    REQUIRE(capture->GetSnapshot().liveFrameLeases == 1);
    control->copyComplete = true;
    control->FinishDeferred();
    REQUIRE(capture->GetSnapshot().shutdownComplete);
    REQUIRE_FALSE(capture->GetSnapshot().deferredCleanup);
    REQUIRE(control->releases == 1);
    REQUIRE(control->consumes == 0);
}

TEST_CASE("DXGI expired acquired image is released before Copy and has no consumer observation")
{
    const auto control = std::make_shared<Control>();
    control->Push(Image(1, 1, true));
    auto config = MakeConfig();
    config.maximumFrameAgeMilliseconds = 1;
    std::unique_ptr<DxgiCapture> capture;
    REQUIRE(Create(control, capture, config));
    REQUIRE(WaitFor([&] { return capture->GetSnapshot().expiredFrames == 1; }));
    REQUIRE(capture->Stop());
    REQUIRE(control->releases == 1);
    REQUIRE(control->copies == 0);
    REQUIRE(control->consumes == 0);
    REQUIRE(capture->GetSnapshot().liveFrameLeases == 0);
}
