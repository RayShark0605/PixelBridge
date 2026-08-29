#include "capture_runtime.h"
#include "d3d_roi_ring.h"
#include "../PBScreenCaptureWgc/capture_test_support.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <vector>

using namespace pbcapturenormalize;
using namespace pbcapturenormalize::detail;

namespace
{

bool ReadNow100ns(std::int64_t& timestamp100ns) noexcept
{
    LARGE_INTEGER counter{};
    LARGE_INTEGER frequency{};
    return QueryPerformanceFrequency(&frequency) && QueryPerformanceCounter(&counter) &&
           ConvertQpcTo100ns(counter.QuadPart, frequency.QuadPart, timestamp100ns);
}

struct ReleaseFakeGpuOnExit
{
    std::shared_ptr<capturetest::Control> control;
    ~ReleaseFakeGpuOnExit()
    {
        control->copyComplete = true;
        control->consumeComplete = true;
        control->CompleteDeferred();
    }
};

class SequenceConsumer final : public RawRoiConsumer
{
public:
    CaptureStatus EpochStarted(std::uint64_t, const CaptureEnvironment&, ID3D11Device*) override
    {
        return {};
    }
    CaptureStatus Submit(const RawRoiFrameMetadata& metadata, ID3D11Texture2D*, ID3D11DeviceContext*) override
    {
        const std::lock_guard lock(mutex_);
        ordinals_.push_back(metadata.arrivalOrdinal);
        return {};
    }
    std::vector<std::uint64_t> Ordinals() const
    {
        const std::lock_guard lock(mutex_);
        return ordinals_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::uint64_t> ordinals_;
};

struct OrderingObservations
{
    std::atomic<std::size_t> secondSlot{maximumRoiTextures};
    std::atomic<std::size_t> thirdSlot{maximumRoiTextures};
};

// Only OS acquisition and GPU markers are simulated. The production owner,
// inbox ordering, source retirement and slot reuse execute without intervention.
class OrderingBackend final : public capturetest::Backend
{
public:
    OrderingBackend(std::shared_ptr<capturetest::Control> control, std::shared_ptr<OrderingObservations> observations)
        : capturetest::Backend(control), control_(std::move(control)), observations_(std::move(observations))
    {
    }
    CaptureStatus Acquire() noexcept override
    {
        if (emitted_ == 3)
        {
            return {};
        }
        try
        {
            std::int64_t now100ns = 0;
            if (!ReadNow100ns(now100ns))
            {
                return CaptureStatus::Failure(CaptureError::InternalError, CaptureStage::Callback);
            }
            emitted_++;
            auto frame = capturetest::MakeFrame(control_, emitted_);
            frame.timestamp100ns = now100ns;
            control_->inbox.load()->Push(std::move(frame));
            return {};
        }
        catch (...)
        {
            return CaptureStatus::Failure(CaptureError::OutOfMemory, CaptureStage::Callback);
        }
    }
    CaptureStatus Copy(const FrameLease& frame, const std::size_t slot, bool& submitted) noexcept override
    {
        const auto status = capturetest::Backend::Copy(frame, slot, submitted);
        if (submitted)
        {
            phases_[slot] = 1;
            ordinals_[slot] = frame.arrivalOrdinal;
            if (frame.arrivalOrdinal == 2)
            {
                observations_->secondSlot = slot;
            }
            if (frame.arrivalOrdinal == 3)
            {
                observations_->thirdSlot = slot;
                thirdSubmitted_ = true;
            }
        }
        return status;
    }
    CaptureStatus Consume(RawRoiConsumer& consumer, const RawRoiFrameMetadata& metadata, const std::size_t slot) noexcept override
    {
        phases_[slot] = 2;
        return capturetest::Backend::Consume(consumer, metadata, slot);
    }
    CompletionResult Poll(const std::size_t slot) noexcept override
    {
        const bool complete = phases_[slot] != 1 || ordinals_[slot] == 1 || thirdSubmitted_ || control_->copyComplete;
        if (complete && phases_[slot] != 0)
        {
            control_->Record(phases_[slot] == 1 ? "ordered-copy-done" : "ordered-consume-done", static_cast<std::int64_t>(ordinals_[slot]));
            phases_[slot] = 0;
        }
        return {{}, complete};
    }

private:
    std::shared_ptr<capturetest::Control> control_;
    std::shared_ptr<OrderingObservations> observations_;
    std::array<int, maximumRoiTextures> phases_{};
    std::array<std::uint64_t, maximumRoiTextures> ordinals_{};
    std::int64_t emitted_ = 0;
    bool thirdSubmitted_ = false;
};

enum class TimestampMode : std::uint8_t
{
    Current, Past, Future
};

struct TimestampObservations
{
    std::atomic<std::int64_t> timestamp100ns{-1};
};

class TimestampBackend final : public capturetest::Backend
{
public:
    TimestampBackend(std::shared_ptr<capturetest::Control> control, std::shared_ptr<TimestampObservations> observations,
                     const TimestampMode mode, const std::int64_t offset100ns)
        : capturetest::Backend(control), control_(std::move(control)), observations_(std::move(observations)), mode_(mode), offset100ns_(offset100ns)
    {
    }
    CaptureStatus Acquire() noexcept override
    {
        if (emitted_)
        {
            return {};
        }
        try
        {
            std::int64_t now100ns = 0;
            if (!ReadNow100ns(now100ns) || offset100ns_ < 0 ||
                (mode_ == TimestampMode::Past && now100ns < offset100ns_) ||
                (mode_ == TimestampMode::Future && now100ns > std::numeric_limits<std::int64_t>::max() - offset100ns_))
            {
                return CaptureStatus::Failure(CaptureError::InternalError, CaptureStage::Callback);
            }
            auto frame = capturetest::MakeFrame(control_, 1);
            frame.timestamp100ns = mode_ == TimestampMode::Past ? now100ns - offset100ns_ :
                                   mode_ == TimestampMode::Future ? now100ns + offset100ns_ : now100ns;
            observations_->timestamp100ns = frame.timestamp100ns;
            emitted_ = true;
            control_->inbox.load()->Push(std::move(frame));
            return {};
        }
        catch (...)
        {
            return CaptureStatus::Failure(CaptureError::OutOfMemory, CaptureStage::Callback);
        }
    }

private:
    std::shared_ptr<capturetest::Control> control_;
    std::shared_ptr<TimestampObservations> observations_;
    const TimestampMode mode_;
    const std::int64_t offset100ns_;
    bool emitted_ = false;
};

void RequireOneClosePerFrame(const std::shared_ptr<capturetest::Control>& control, const std::int64_t frameCount)
{
    const auto events = control->Events();
    for (std::int64_t id = 1; id <= frameCount; id++)
    {
        REQUIRE(std::ranges::count_if(events, [id](const capturetest::Event& event) { return event.operation == "close" && event.id == id; }) == 1);
    }
}

struct CompletionObservations
{
    std::atomic<bool> reportDeviceLoss{false};
    std::atomic<std::uint32_t> completions{0};
    std::atomic<std::uint32_t> invalidations{0};
    std::atomic<std::uint64_t> completionEpoch{0};
    std::atomic<bool> receivedContext{false};
    std::atomic<bool> cancelled{false};
    std::atomic<bool> invalidatedBeforeCompletion{false};
};

class CompletionBackend final : public capturetest::Backend
{
public:
    CompletionBackend(std::shared_ptr<capturetest::Control> control, std::shared_ptr<CompletionObservations> observations)
        : capturetest::Backend(control), control_(std::move(control)), observations_(std::move(observations))
    {
    }
    CaptureStatus Complete(RawRoiConsumer& consumer, const RawRoiFrameMetadata& metadata, std::size_t, const bool cancelled) noexcept override
    {
        // Exercise the production callback/exception/context-withholding wrapper;
        // only acquisition and GPU completion are supplied by the test backend.
        return completionRing_.Complete(consumer, metadata, cancelled);
    }
    CompletionResult Poll(const std::size_t slot) noexcept override
    {
        if (observations_->reportDeviceLoss.exchange(false))
        {
            control_->deviceRemoved = true;
            return {CaptureStatus::Failure(CaptureError::DeviceLost, CaptureStage::Completion, DXGI_ERROR_DEVICE_REMOVED), false};
        }
        return capturetest::Backend::Poll(slot);
    }

private:
    const std::shared_ptr<capturetest::Control> control_;
    const std::shared_ptr<CompletionObservations> observations_;
    D3dRoiRing completionRing_;
};

class FailingCompletionConsumer final : public capturetest::Consumer
{
public:
    FailingCompletionConsumer(std::shared_ptr<capturetest::Control> control, std::shared_ptr<CompletionObservations> observations, const bool throwOnComplete)
        : capturetest::Consumer(std::move(control)), observations_(std::move(observations)), throwOnComplete_(throwOnComplete)
    {
    }
    void EpochInvalidated(std::uint64_t) noexcept override
    {
        observations_->invalidations++;
    }
    CaptureStatus Completed(const RawRoiFrameMetadata& metadata, ID3D11DeviceContext* const context, const bool cancelled) override
    {
        observations_->completions++;
        observations_->completionEpoch = metadata.captureEpoch;
        observations_->receivedContext = context != nullptr;
        observations_->cancelled = cancelled;
        observations_->invalidatedBeforeCompletion = observations_->invalidations == 1;
        if (throwOnComplete_)
        {
            throw std::runtime_error("injected cancelled completion failure");
        }
        return CaptureStatus::Failure(CaptureError::ConsumerFailure, CaptureStage::Completion, E_FAIL);
    }

private:
    const std::shared_ptr<CompletionObservations> observations_;
    const bool throwOnComplete_;
};

class StoppingDrainConsumer final : public capturetest::Consumer
{
public:
    StoppingDrainConsumer(std::shared_ptr<capturetest::Control> control, std::shared_ptr<CompletionObservations> observations, const bool stopInInvalidation)
        : capturetest::Consumer(control), control_(std::move(control)), observations_(std::move(observations)), stopInInvalidation_(stopInInvalidation)
    {
    }
    void EpochInvalidated(std::uint64_t) noexcept override
    {
        observations_->invalidations++;
        if (stopInInvalidation_)
        {
            control_->inbox.load()->RequestStop();
        }
    }
    CaptureStatus Completed(const RawRoiFrameMetadata& metadata, ID3D11DeviceContext* const context, const bool cancelled) override
    {
        observations_->completions++;
        observations_->completionEpoch = metadata.captureEpoch;
        observations_->receivedContext = context != nullptr;
        observations_->cancelled = cancelled;
        observations_->invalidatedBeforeCompletion = observations_->invalidations == 1;
        if (!stopInInvalidation_)
        {
            control_->inbox.load()->RequestStop();
        }
        return {};
    }

private:
    const std::shared_ptr<capturetest::Control> control_;
    const std::shared_ptr<CompletionObservations> observations_;
    const bool stopInInvalidation_;
};

}

TEST_CASE("Capture frame age classification includes its exact finite boundary")
{
    constexpr std::uint32_t limitMilliseconds = 37;
    constexpr std::int64_t timestamp100ns = 123456789;
    constexpr std::uint64_t limit100ns = 370000;
    REQUIRE(ClassifyFrameAge(timestamp100ns, timestamp100ns, limitMilliseconds) == CaptureFrameAgeResult{CaptureFrameAgeDisposition::Current, 0});
    REQUIRE(ClassifyFrameAge(timestamp100ns + limit100ns - 1, timestamp100ns, limitMilliseconds) == CaptureFrameAgeResult{CaptureFrameAgeDisposition::Current, limit100ns - 1});
    REQUIRE(ClassifyFrameAge(timestamp100ns + limit100ns, timestamp100ns, limitMilliseconds) == CaptureFrameAgeResult{CaptureFrameAgeDisposition::Current, limit100ns});
    REQUIRE(ClassifyFrameAge(timestamp100ns + limit100ns + 1, timestamp100ns, limitMilliseconds) == CaptureFrameAgeResult{CaptureFrameAgeDisposition::Expired, limit100ns + 1});
    REQUIRE(ClassifyFrameAge(0, 0, limitMilliseconds) == CaptureFrameAgeResult{CaptureFrameAgeDisposition::Current, 0});
}

TEST_CASE("Capture frame age rejects invalid timestamps without unsigned wraparound")
{
    constexpr std::array<std::array<std::int64_t, 2>, 4> invalidTimestamps{{{-1, 0}, {0, -1}, {-1, -1}, {1, 2}}};
    for (const auto& timestamps : invalidTimestamps)
    {
        CAPTURE(timestamps[0], timestamps[1]);
        REQUIRE(ClassifyFrameAge(timestamps[0], timestamps[1], 1) == CaptureFrameAgeResult{CaptureFrameAgeDisposition::InvalidTimestamp, 0});
        REQUIRE(ClassifyFrameAge(timestamps[0], timestamps[1], 0) == CaptureFrameAgeResult{CaptureFrameAgeDisposition::Current, 0});
    }
    constexpr auto maximumTimestamp = std::numeric_limits<std::int64_t>::max();
    constexpr auto maximumLimit = std::numeric_limits<std::uint32_t>::max();
    constexpr std::uint64_t maximumLimit100ns = 42949672950000;
    REQUIRE(ClassifyFrameAge(maximumTimestamp, maximumTimestamp, maximumLimit) == CaptureFrameAgeResult{CaptureFrameAgeDisposition::Current, 0});
    REQUIRE(ClassifyFrameAge(maximumTimestamp, 0, maximumLimit) == CaptureFrameAgeResult{CaptureFrameAgeDisposition::Expired, static_cast<std::uint64_t>(maximumTimestamp)});
    REQUIRE(ClassifyFrameAge(maximumLimit100ns, 0, maximumLimit) == CaptureFrameAgeResult{CaptureFrameAgeDisposition::Current, maximumLimit100ns});
    REQUIRE(ClassifyFrameAge(maximumLimit100ns + 1, 0, maximumLimit) == CaptureFrameAgeResult{CaptureFrameAgeDisposition::Expired, maximumLimit100ns + 1});
    REQUIRE(ClassifyFrameAge(maximumTimestamp, 0, 0) == CaptureFrameAgeResult{CaptureFrameAgeDisposition::Current, 0});
}

TEST_CASE("Capture owner rejects an older completed slot after delivering a newer ordinal only in bounded mode")
{
    for (const std::uint32_t maximumAgeMilliseconds : {0u, 60000u})
    {
        CAPTURE(maximumAgeMilliseconds);
        const auto control = std::make_shared<capturetest::Control>();
        const auto observations = std::make_shared<OrderingObservations>();
        const auto consumer = std::make_shared<SequenceConsumer>();
        auto config = capturetest::MakeConfig();
        config.roiTextureCount = 2;
        config.maximumFrameAgeMilliseconds = maximumAgeMilliseconds;
        std::unique_ptr<CaptureRuntime> capture;
        const ReleaseFakeGpuOnExit releaseOnExit{control};
        REQUIRE(CaptureRuntime::Create(config, consumer, std::make_unique<OrderingBackend>(control, observations), capture));
        REQUIRE(capturetest::WaitFor([&]
        {
            const auto snapshot = capture->GetSnapshot();
            return snapshot.copiedFrames == 3 && snapshot.liveFrameLeases == 0 && snapshot.busyRoiTextures == 0;
        }));
        REQUIRE(capture->Stop());
        const auto snapshot = capture->GetSnapshot();
        const bool bounded = maximumAgeMilliseconds != 0;
        REQUIRE(consumer->Ordinals() == (bounded ? std::vector<std::uint64_t>{1, 3} : std::vector<std::uint64_t>{1, 3, 2}));
        REQUIRE(observations->secondSlot == 1);
        REQUIRE(observations->thirdSlot == 0);
        REQUIRE(snapshot.arrivedFrames == 3);
        REQUIRE(snapshot.copiedFrames == 3);
        REQUIRE(snapshot.deliveredFrames == (bounded ? 2 : 3));
        REQUIRE(snapshot.staleFrames == (bounded ? 1 : 0));
        REQUIRE(snapshot.expiredFrames == 0);
        REQUIRE(snapshot.shutdownComplete);
        REQUIRE_FALSE(snapshot.deferredCleanup);
        const auto events = control->Events();
        const auto thirdCompletion = capturetest::FindEvent(events, "ordered-copy-done", 3);
        const auto secondCompletion = capturetest::FindEvent(events, "ordered-copy-done", 2);
        REQUIRE(thirdCompletion < secondCompletion);
        REQUIRE(secondCompletion < events.size());
        RequireOneClosePerFrame(control, 3);
    }
}

TEST_CASE("Capture owner rejects expired claims and bounds ahead-of-time claims by the QPC arrival")
{
    for (const auto mode : {TimestampMode::Past, TimestampMode::Future})
    {
        CAPTURE(mode);
        const auto control = std::make_shared<capturetest::Control>();
        const auto observations = std::make_shared<TimestampObservations>();
        const auto consumer = std::make_shared<SequenceConsumer>();
        auto config = capturetest::MakeConfig();
        config.maximumFrameAgeMilliseconds = 250;
        control->copyComplete = true;
        // Past: a 251 ms claim under a 250 ms limit. Future: a claim six
        // seconds ahead of now, the WGC SystemRelativeTime failure mode; the
        // inbox samples a real QPC arrival at push that must supersede it.
        const std::int64_t offset100ns = mode == TimestampMode::Past ? 2510000 : 600000000;
        std::unique_ptr<CaptureRuntime> capture;
        const ReleaseFakeGpuOnExit releaseOnExit{control};
        REQUIRE(CaptureRuntime::Create(config, consumer, std::make_unique<TimestampBackend>(control, observations, mode, offset100ns), capture));
        REQUIRE(capturetest::WaitFor([&]
        {
            const auto snapshot = capture->GetSnapshot();
            const bool settled = mode == TimestampMode::Past ? snapshot.expiredFrames == 1 : snapshot.deliveredFrames == 1;
            return settled && snapshot.liveFrameLeases == 0;
        }));
        REQUIRE(capture->Stop());
        const auto snapshot = capture->GetSnapshot();
        if (mode == TimestampMode::Past)
        {
            REQUIRE(control->copies == 0);
            REQUIRE(control->consumes == 0);
            REQUIRE(snapshot.copiedFrames == 0);
            REQUIRE(snapshot.deliveredFrames == 0);
            REQUIRE(snapshot.expiredFrames == 1);
            REQUIRE(snapshot.frameAgeHighWater100ns >= 2510000);
            REQUIRE(consumer->Ordinals().empty());
        }
        else
        {
            // The impossible claim is not trusted; the measured arrival bounds
            // the age and the frame is admitted with a small age.
            REQUIRE(control->copies == 1);
            REQUIRE(control->consumes == 1);
            REQUIRE(snapshot.copiedFrames == 1);
            REQUIRE(snapshot.deliveredFrames == 1);
            REQUIRE(snapshot.expiredFrames == 0);
            REQUIRE(snapshot.frameAgeHighWater100ns < 2500000);
            REQUIRE(consumer->Ordinals() == std::vector<std::uint64_t>{1});
        }
        REQUIRE(snapshot.arrivedFrames == 1);
        REQUIRE(snapshot.staleFrames == 0);
        REQUIRE(snapshot.busyRoiTextures == 0);
        REQUIRE(snapshot.shutdownComplete);
        REQUIRE_FALSE(snapshot.deferredCleanup);
        RequireOneClosePerFrame(control, 1);
    }
}

TEST_CASE("Capture owner rechecks frame age after GPU copy and retires the expired source before erasure")
{
    const auto control = std::make_shared<capturetest::Control>();
    const auto observations = std::make_shared<TimestampObservations>();
    const auto consumer = std::make_shared<SequenceConsumer>();
    auto config = capturetest::MakeConfig();
    config.maximumFrameAgeMilliseconds = 250;
    config.gpuTimeoutMilliseconds = 3000;
    std::unique_ptr<CaptureRuntime> capture;
    const ReleaseFakeGpuOnExit releaseOnExit{control};
    REQUIRE(CaptureRuntime::Create(config, consumer, std::make_unique<TimestampBackend>(control, observations, TimestampMode::Current, 0), capture));
    REQUIRE(capturetest::WaitFor([&] { return capture->GetSnapshot().copiedFrames == 1; }));
    REQUIRE(capture->GetSnapshot().liveFrameLeases == 1);
    REQUIRE(capture->GetSnapshot().expiredFrames == 0);
    const auto pendingEvents = control->Events();
    REQUIRE(capturetest::FindEvent(pendingEvents, "close", 1) == pendingEvents.size());
    REQUIRE(capturetest::WaitFor([&]
    {
        std::int64_t now100ns = 0;
        return ReadNow100ns(now100ns) && ClassifyFrameAge(now100ns, observations->timestamp100ns.load(), config.maximumFrameAgeMilliseconds).disposition == CaptureFrameAgeDisposition::Expired;
    }));
    control->copyComplete = true;
    REQUIRE(capturetest::WaitFor([&]
    {
        const auto snapshot = capture->GetSnapshot();
        return snapshot.expiredFrames == 1 && snapshot.liveFrameLeases == 0 && snapshot.busyRoiTextures == 0;
    }));
    REQUIRE(capture->Stop());
    const auto snapshot = capture->GetSnapshot();
    REQUIRE(snapshot.copiedFrames == 1);
    REQUIRE(snapshot.deliveredFrames == 0);
    REQUIRE(snapshot.staleFrames == 0);
    REQUIRE(snapshot.frameAgeHighWater100ns > 2500000);
    REQUIRE(snapshot.shutdownComplete);
    REQUIRE_FALSE(snapshot.deferredCleanup);
    REQUIRE(control->consumes == 0);
    REQUIRE(consumer->Ordinals().empty());
    const auto events = control->Events();
    const auto copyCompletion = capturetest::FindEvent(events, "copy-done", observations->timestamp100ns.load());
    const auto sourceClose = capturetest::FindEvent(events, "close", 1);
    REQUIRE(copyCompletion < sourceClose);
    REQUIRE(sourceClose < events.size());
    RequireOneClosePerFrame(control, 1);
}

TEST_CASE("Capture owner zero age limit preserves legacy delivery of old timestamps")
{
    const auto control = std::make_shared<capturetest::Control>();
    const auto observations = std::make_shared<TimestampObservations>();
    const auto consumer = std::make_shared<SequenceConsumer>();
    const auto config = capturetest::MakeConfig();
    control->copyComplete = true;
    std::unique_ptr<CaptureRuntime> capture;
    const ReleaseFakeGpuOnExit releaseOnExit{control};
    REQUIRE(CaptureRuntime::Create(config, consumer, std::make_unique<TimestampBackend>(control, observations, TimestampMode::Past, 2510000), capture));
    REQUIRE(capturetest::WaitFor([&]
    {
        const auto snapshot = capture->GetSnapshot();
        return snapshot.deliveredFrames == 1 && snapshot.liveFrameLeases == 0 && snapshot.busyRoiTextures == 0;
    }));
    REQUIRE(capture->Stop());
    const auto snapshot = capture->GetSnapshot();
    REQUIRE(config.maximumFrameAgeMilliseconds == 0);
    REQUIRE(snapshot.copiedFrames == 1);
    REQUIRE(snapshot.deliveredFrames == 1);
    REQUIRE(snapshot.expiredFrames == 0);
    REQUIRE(snapshot.staleFrames == 0);
    REQUIRE(snapshot.frameAgeHighWater100ns == 0);
    REQUIRE(snapshot.shutdownComplete);
    REQUIRE_FALSE(snapshot.deferredCleanup);
    REQUIRE(consumer->Ordinals() == std::vector<std::uint64_t>{1});
    RequireOneClosePerFrame(control, 1);
}

TEST_CASE("Capture device recovery retires only the old device generation release error")
{
    for (const HRESULT releaseResult : {DXGI_ERROR_DEVICE_REMOVED, DXGI_ERROR_DEVICE_RESET, DXGI_ERROR_DEVICE_HUNG, E_FAIL})
    {
        CAPTURE(releaseResult);
        const auto control = std::make_shared<capturetest::Control>();
        const auto consumer = std::make_shared<SequenceConsumer>();
        std::unique_ptr<CaptureRuntime> capture;
        const ReleaseFakeGpuOnExit releaseOnExit{control};
        REQUIRE(CaptureRuntime::Create(capturetest::MakeConfig(), consumer, std::make_unique<capturetest::Backend>(control), capture));
        control->inbox.load()->Push(capturetest::MakeFrame(control, 1, {100, 80}, 1, releaseResult));
        REQUIRE(capturetest::WaitFor([&] { return control->copies == 1; }));
        control->deviceRemoved = true;
        if (releaseResult == E_FAIL)
        {
            REQUIRE(capturetest::WaitFor([&] { return capture->GetSnapshot().shutdownComplete; }));
            REQUIRE(capture->Stop().nativeError == E_FAIL);
            REQUIRE(control->inbox.load()->counters->closeError == E_FAIL);
            REQUIRE(control->initializes == 1);
            REQUIRE(capture->GetSnapshot().deviceRecoveries == 0);
        }
        else
        {
            REQUIRE(capturetest::WaitFor([&] { return control->initializes == 2; }));
            REQUIRE(capturetest::WaitFor([&] { return capture->GetSnapshot().state == CaptureState::Running; }));
            REQUIRE(control->inbox.load()->counters->closeError == S_OK);
            control->copyComplete = true;
            control->inbox.load()->Push(capturetest::MakeFrame(control, 2, {100, 80}, 2));
            REQUIRE(capturetest::WaitFor([&] { return control->consumes == 1; }));
            REQUIRE(capture->Stop());
            REQUIRE(capture->GetSnapshot().captureEpoch == 2);
            REQUIRE(capture->GetSnapshot().deviceRecoveries == 1);
        }
        REQUIRE(capture->GetSnapshot().lastDeviceLoss.nativeError == DXGI_ERROR_DEVICE_REMOVED);
        REQUIRE(capture->GetSnapshot().liveFrameLeases == 0);
        const auto events = control->Events();
        REQUIRE(std::ranges::count_if(events, [](const capturetest::Event& event) { return event.operation == "close" && event.id == 1; }) == 1);
    }
}

TEST_CASE("Capture device recovery never clears a failed cancelled consumer completion")
{
    for (const bool throwOnComplete : {false, true})
    {
        for (const bool removalAlreadyLatched : {false, true})
        {
            CAPTURE(throwOnComplete, removalAlreadyLatched);
            const auto control = std::make_shared<capturetest::Control>();
            const auto observations = std::make_shared<CompletionObservations>();
            const auto consumer = std::make_shared<FailingCompletionConsumer>(control, observations, throwOnComplete);
            control->copyComplete = true;
            control->consumeComplete = false;
            std::unique_ptr<CaptureRuntime> capture;
            const ReleaseFakeGpuOnExit releaseOnExit{control};
            REQUIRE(CaptureRuntime::Create(capturetest::MakeConfig(), consumer, std::make_unique<CompletionBackend>(control, observations), capture));
            control->inbox.load()->Push(capturetest::MakeFrame(control, 1));
            REQUIRE(capturetest::WaitFor([&] { return control->consumes == 1; }));
            REQUIRE(observations->completions == 0);
            if (removalAlreadyLatched)
            {
                observations->reportDeviceLoss = true;
            }
            else
            {
                control->deviceRemoved = true;
            }
            REQUIRE(capturetest::WaitFor([&] { return capture->GetSnapshot().shutdownComplete; }));
            const auto expected = CaptureStatus::Failure(CaptureError::ConsumerFailure, CaptureStage::Completion, throwOnComplete ? 0 : E_FAIL);
            REQUIRE(capture->Stop() == expected);
            const auto snapshot = capture->GetSnapshot();
            REQUIRE(snapshot.state == CaptureState::Failed);
            REQUIRE(snapshot.error == expected);
            REQUIRE(snapshot.lastDeviceLoss.nativeError == DXGI_ERROR_DEVICE_REMOVED);
            REQUIRE(snapshot.captureEpoch == 1);
            REQUIRE(snapshot.deviceRecoveries == 0);
            REQUIRE(snapshot.liveFrameLeases == 0);
            REQUIRE(snapshot.busyRoiTextures == 0);
            REQUIRE_FALSE(snapshot.deferredCleanup);
            REQUIRE(control->initializes == 1);
            REQUIRE(control->consumes == 1);
            REQUIRE(observations->completions == 1);
            REQUIRE(observations->invalidations == 1);
            REQUIRE(observations->completionEpoch == 1);
            REQUIRE(observations->cancelled);
            REQUIRE(observations->invalidatedBeforeCompletion);
            REQUIRE_FALSE(observations->receivedContext);
            RequireOneClosePerFrame(control, 1);
        }
    }
}

TEST_CASE("Capture source retirement promotes a later non-device Close failure without replacing the first non-device error")
{
    struct CloseResults
    {
        HRESULT first;
        HRESULT second;
        HRESULT expected;
        bool recovers;
    };
    constexpr std::array cases
    {
        CloseResults{DXGI_ERROR_DEVICE_REMOVED, E_FAIL, E_FAIL, false},
        CloseResults{DXGI_ERROR_DEVICE_RESET, E_ACCESSDENIED, E_ACCESSDENIED, false},
        CloseResults{DXGI_ERROR_DEVICE_HUNG, E_UNEXPECTED, E_UNEXPECTED, false},
        CloseResults{E_FAIL, DXGI_ERROR_DEVICE_REMOVED, E_FAIL, false},
        CloseResults{E_FAIL, E_ACCESSDENIED, E_FAIL, false},
        CloseResults{DXGI_ERROR_DEVICE_REMOVED, DXGI_ERROR_DEVICE_RESET, S_OK, true}
    };
    for (const auto& results : cases)
    {
        CAPTURE(results.first, results.second);
        const auto control = std::make_shared<capturetest::Control>();
        const auto consumer = std::make_shared<SequenceConsumer>();
        std::unique_ptr<CaptureRuntime> capture;
        const ReleaseFakeGpuOnExit releaseOnExit{control};
        REQUIRE(CaptureRuntime::Create(capturetest::MakeConfig(), consumer, std::make_unique<capturetest::Backend>(control), capture));
        control->inbox.load()->Push(capturetest::MakeFrame(control, 1, {100, 80}, 1, results.first));
        REQUIRE(capturetest::WaitFor([&] { return control->copies == 1; }));
        control->inbox.load()->Push(capturetest::MakeFrame(control, 2, {100, 80}, 1, results.second));
        REQUIRE(capturetest::WaitFor([&] { return control->copies == 2; }));
        REQUIRE(capture->GetSnapshot().liveFrameLeases == 2);
        REQUIRE(control->consumes == 0);
        control->deviceRemoved = true;
        if (results.recovers)
        {
            REQUIRE(capturetest::WaitFor([&] { return control->initializes == 2 && capture->GetSnapshot().state == CaptureState::Running; }));
            REQUIRE(control->inbox.load()->counters->closeError == S_OK);
            control->copyComplete = true;
            control->inbox.load()->Push(capturetest::MakeFrame(control, 3, {100, 80}, 2));
            REQUIRE(capturetest::WaitFor([&] { return control->consumes == 1; }));
            REQUIRE(capture->Stop());
            REQUIRE(capture->GetSnapshot().deviceRecoveries == 1);
            REQUIRE(capture->GetSnapshot().captureEpoch == 2);
        }
        else
        {
            REQUIRE(capturetest::WaitFor([&] { return capture->GetSnapshot().shutdownComplete; }));
            const auto expectedCode = results.expected == E_ACCESSDENIED ? CaptureError::AccessLost : CaptureError::NativeFailure;
            const auto expected = CaptureStatus::Failure(expectedCode, CaptureStage::Shutdown, results.expected);
            REQUIRE(capture->Stop() == expected);
            REQUIRE(control->inbox.load()->counters->closeError == results.expected);
            REQUIRE(control->initializes == 1);
            REQUIRE(capture->GetSnapshot().deviceRecoveries == 0);
            REQUIRE(capture->GetSnapshot().captureEpoch == 1);
            REQUIRE(consumer->Ordinals().empty());
        }
        const auto snapshot = capture->GetSnapshot();
        REQUIRE(snapshot.liveFrameLeases == 0);
        REQUIRE(snapshot.busyRoiTextures == 0);
        REQUIRE(snapshot.shutdownComplete);
        REQUIRE_FALSE(snapshot.deferredCleanup);
        RequireOneClosePerFrame(control, results.recovers ? 3 : 2);
        const auto events = control->Events();
        REQUIRE(capturetest::FindEvent(events, "close", 1) < capturetest::FindEvent(events, "close", 2));
    }
}

TEST_CASE("Capture device recovery honors RequestStop from invalidation or cancelled completion before starting another epoch")
{
    for (const bool stopInInvalidation : {false, true})
    {
        for (const bool removalAlreadyLatched : {false, true})
        {
            CAPTURE(stopInInvalidation, removalAlreadyLatched);
            const auto control = std::make_shared<capturetest::Control>();
            const auto observations = std::make_shared<CompletionObservations>();
            const auto consumer = std::make_shared<StoppingDrainConsumer>(control, observations, stopInInvalidation);
            control->copyComplete = !stopInInvalidation;
            control->consumeComplete = false;
            std::unique_ptr<CaptureRuntime> capture;
            const ReleaseFakeGpuOnExit releaseOnExit{control};
            REQUIRE(CaptureRuntime::Create(capturetest::MakeConfig(), consumer, std::make_unique<CompletionBackend>(control, observations), capture));
            control->inbox.load()->Push(capturetest::MakeFrame(control, 1));
            REQUIRE(capturetest::WaitFor([&] { return stopInInvalidation ? control->copies == 1 : control->consumes == 1; }));
            REQUIRE(observations->completions == 0);
            if (removalAlreadyLatched)
            {
                observations->reportDeviceLoss = true;
            }
            else
            {
                control->deviceRemoved = true;
            }
            REQUIRE(capturetest::WaitFor([&] { return capture->GetSnapshot().shutdownComplete; }));
            const auto expected = removalAlreadyLatched ? CaptureStatus::Failure(CaptureError::DeviceLost, CaptureStage::Completion, DXGI_ERROR_DEVICE_REMOVED) : CaptureStatus{};
            REQUIRE(capture->Stop() == expected);
            const auto snapshot = capture->GetSnapshot();
            REQUIRE(snapshot.error == expected);
            REQUIRE(snapshot.captureEpoch == 1);
            REQUIRE(snapshot.deviceRecoveries == 0);
            REQUIRE(snapshot.lastDeviceLoss.nativeError == DXGI_ERROR_DEVICE_REMOVED);
            REQUIRE(snapshot.liveFrameLeases == 0);
            REQUIRE(snapshot.busyRoiTextures == 0);
            REQUIRE(snapshot.shutdownComplete);
            REQUIRE_FALSE(snapshot.deferredCleanup);
            REQUIRE(control->initializes == 1);
            REQUIRE(control->inbox.load()->GetSnapshot().stopRequested);
            REQUIRE(observations->invalidations == 1);
            REQUIRE(observations->completions == (stopInInvalidation ? 0u : 1u));
            if (!stopInInvalidation)
            {
                REQUIRE(observations->completionEpoch == 1);
                REQUIRE(observations->cancelled);
                REQUIRE(observations->invalidatedBeforeCompletion);
                REQUIRE_FALSE(observations->receivedContext);
            }
            RequireOneClosePerFrame(control, 1);
            const auto events = control->Events();
            REQUIRE(std::ranges::count_if(events, [](const capturetest::Event& event) { return event.operation == "start"; }) == 1);
            REQUIRE(std::ranges::count_if(events, [](const capturetest::Event& event) { return event.operation == "epoch"; }) == 1);
        }
    }
}
