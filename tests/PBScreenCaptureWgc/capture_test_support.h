#pragma once

#include "capture_internal.h"

#include <algorithm>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace capturetest
{
using namespace pbscreencapturewgc;
using namespace pbscreencapturewgc::detail;

inline WgcCaptureConfig MakeConfig()
{
    WgcCaptureConfig config;
    config.region = {reinterpret_cast<HMONITOR>(std::uintptr_t{1}), {-90, 25, -60, 45}, {-100, 20, 0, 100}, 96, 96, DXGI_MODE_ROTATION_IDENTITY};
    config.gpuTimeoutMilliseconds = 1000;
    return config;
}

inline bool WaitFor(const std::function<bool()>& predicate, const std::uint32_t milliseconds = 3000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    do
    {
        if (predicate())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    return predicate();
}

struct Event
{
    std::string operation;
    std::int64_t id = 0;
    std::uint64_t epoch = 0;
    std::thread::id thread{};
};

struct Control
{
    std::atomic<std::shared_ptr<FrameInbox>> inbox;
    std::atomic<std::uint64_t> epoch{1};
    std::atomic<bool> copyComplete{false};
    std::atomic<bool> consumeComplete{true};
    std::atomic<bool> deviceRemoved{false};
    std::atomic<bool> callbacksIdle{true};
    std::atomic<bool> failCopyBefore{false};
    std::atomic<bool> failCopyAfter{false};
    std::atomic<bool> failPoll{false};
    std::atomic<bool> failRecreate{false};
    std::atomic<bool> failRecovery{false};
    std::atomic<bool> environmentChanged{false};
    std::atomic<int> initializes{0};
    std::atomic<int> shutdowns{0};
    std::atomic<int> recreates{0};
    std::atomic<int> copies{0};
    std::atomic<int> consumes{0};
    std::atomic<bool> unsafeRecreate{false};
    std::mutex mutex;
    std::vector<Event> events;
    std::shared_ptr<DeferredCleanup> deferred;
    CaptureCapabilities capabilities;
    CaptureStatus initializeError;
    CaptureStatus startError;
    CaptureStatus shutdownError;
    std::function<void()> closeHook;

    void Record(const std::string& operation, const std::int64_t id = 0, const std::uint64_t frameEpoch = 0)
    {
        const std::lock_guard lock(mutex);
        events.push_back({operation, id, frameEpoch, std::this_thread::get_id()});
    }
    std::vector<Event> Events()
    {
        const std::lock_guard lock(mutex);
        return events;
    }
    bool HasDeferred()
    {
        const std::lock_guard lock(mutex);
        return static_cast<bool>(deferred);
    }
    void CompleteDeferred()
    {
        std::shared_ptr<DeferredCleanup> owner;
        {
            const std::lock_guard lock(mutex);
            owner = std::move(deferred);
        }
        if (owner)
        {
            owner->CompleteDeferredShutdown();
        }
    }
};

struct FakeFrame
{
    std::shared_ptr<Control> control;
    std::int64_t id = 0;
    HRESULT closeResult = S_OK;
};

inline HRESULT CloseFrame(void* pointer) noexcept
{
    const std::unique_ptr<FakeFrame> frame(static_cast<FakeFrame*>(pointer));
    frame->control->Record("close", frame->id);
    if (frame->control->closeHook)
    {
        frame->control->closeHook();
    }
    return frame->closeResult;
}

inline HRESULT NoTexture(void*, ID3D11Texture2D**) noexcept
{
    return E_NOTIMPL;
}

inline FrameLease MakeFrame(const std::shared_ptr<Control>& control, const std::int64_t id, const CaptureSize size = {100, 80},
                            const std::uint64_t epoch = 1, const HRESULT closeResult = S_OK)
{
    auto* const frame = new FakeFrame{control, id, closeResult};
    return FrameLease(frame, CloseFrame, NoTexture, control->inbox.load()->counters, size, id, epoch);
}

class Consumer : public RoiConsumer
{
public:
    explicit Consumer(std::shared_ptr<Control> control) : control_(std::move(control))
    {
    }
    CaptureStatus EpochStarted(const std::uint64_t epoch, const CaptureEnvironment&, ID3D11Device*) override
    {
        control_->Record("epoch", 0, epoch);
        return epochError;
    }
    CaptureStatus Submit(const RoiFrameMetadata& metadata, ID3D11Texture2D*, ID3D11DeviceContext*) override
    {
        control_->Record("consumer", metadata.systemRelativeTime100ns, metadata.captureEpoch);
        if (submitHook)
        {
            submitHook();
        }
        if (throwOnSubmit)
        {
            throw std::runtime_error("injected consumer failure");
        }
        return submitError;
    }
    CaptureStatus epochError;
    CaptureStatus submitError;
    bool throwOnSubmit = false;
    std::function<void()> submitHook;

private:
    std::shared_ptr<Control> control_;
};

class Backend : public CaptureBackend
{
public:
    explicit Backend(std::shared_ptr<Control> control) : control_(std::move(control))
    {
    }
    CaptureStatus Initialize(const WgcCaptureConfig& config, std::shared_ptr<FrameInbox> inbox) noexcept override
    {
        const int count = control_->initializes.fetch_add(1) + 1;
        control_->inbox.store(std::move(inbox));
        control_->deviceRemoved = false;
        environment_.region = config.region;
        environment_.contentSize = {100, 80};
        environment_.pixelFormat = config.pixelFormat;
        control_->Record("initialize");
        if (count > 1 && control_->failRecovery)
        {
            return CaptureStatus::Failure(CaptureError::NativeFailure, CaptureStage::Device);
        }
        return control_->initializeError;
    }
    CaptureStatus Start(const std::uint64_t epoch) noexcept override
    {
        control_->epoch = epoch;
        control_->Record("start", 0, epoch);
        return control_->startError;
    }
    CaptureStatus Pause() noexcept override
    {
        control_->Record("pause");
        return {};
    }
    bool CallbacksIdle() const noexcept override
    {
        return control_->callbacksIdle;
    }
    CaptureStatus CheckEnvironment(bool& changed) noexcept override
    {
        changed = control_->environmentChanged.exchange(false);
        return {};
    }
    CaptureStatus Recreate(const CaptureSize size) noexcept override
    {
        control_->unsafeRecreate = control_->inbox.load()->counters->live != 0 || !control_->callbacksIdle;
        control_->recreates++;
        control_->Record("recreate");
        if (control_->failRecreate)
        {
            return CaptureStatus::Failure(CaptureError::NativeFailure, CaptureStage::Recreate);
        }
        environment_.contentSize = size;
        return {};
    }
    CaptureEnvironment GetEnvironment() const noexcept override
    {
        return environment_;
    }
    CaptureCapabilities GetCapabilities() const noexcept override
    {
        return control_->capabilities;
    }
    CaptureStatus NotifyEpoch(RoiConsumer& consumer, const std::uint64_t epoch) noexcept override
    {
        return consumer.EpochStarted(epoch, environment_, nullptr);
    }
    CaptureStatus Copy(const FrameLease& frame, const std::size_t slot, bool& submitted) noexcept override
    {
        submitted = !control_->failCopyBefore;
        if (submitted)
        {
            phases_[slot] = 1;
            ids_[slot] = frame.timestamp100ns;
            control_->Record("copy", frame.timestamp100ns, frame.captureEpoch);
            control_->copies++;
        }
        return control_->failCopyBefore || control_->failCopyAfter
                   ? CaptureStatus::Failure(CaptureError::NativeFailure, CaptureStage::Copy) : CaptureStatus{};
    }
    CaptureStatus Consume(RoiConsumer& consumer, const RoiFrameMetadata& metadata, const std::size_t slot) noexcept override
    {
        phases_[slot] = 2;
        control_->consumes++;
        try
        {
            return consumer.Submit(metadata, nullptr, nullptr);
        }
        catch (...)
        {
            return CaptureStatus::Failure(CaptureError::ConsumerFailure, CaptureStage::Consumer);
        }
    }
    CompletionResult Poll(const std::size_t slot) noexcept override
    {
        if (control_->failPoll)
        {
            return {CaptureStatus::Failure(CaptureError::NativeFailure, CaptureStage::Completion), false};
        }
        const bool complete = phases_[slot] == 0 || (phases_[slot] == 1 && control_->copyComplete) || (phases_[slot] == 2 && control_->consumeComplete);
        if (complete && phases_[slot] != 0)
        {
            control_->Record(phases_[slot] == 1 ? "copy-done" : "consume-done", ids_[slot]);
            phases_[slot] = 0;
        }
        return {{}, complete};
    }
    bool DeviceRemoved() const noexcept override
    {
        return control_->deviceRemoved;
    }
    std::int32_t DeviceRemovalReason() const noexcept override
    {
        return control_->deviceRemoved ? DXGI_ERROR_DEVICE_REMOVED : S_OK;
    }
    CaptureStatus Shutdown() noexcept override
    {
        control_->shutdowns++;
        control_->Record("shutdown");
        phases_ = {};
        return control_->shutdownError;
    }
    void DeferShutdown(std::shared_ptr<DeferredCleanup> owner) noexcept override
    {
        const std::lock_guard lock(control_->mutex);
        control_->deferred = std::move(owner);
    }

private:
    std::shared_ptr<Control> control_;
    CaptureEnvironment environment_;
    std::array<int, maximumRoiTextures> phases_{};
    std::array<std::int64_t, maximumRoiTextures> ids_{};
};

inline std::size_t FindEvent(const std::vector<Event>& events, const std::string& operation, const std::int64_t id)
{
    for (std::size_t index = 0; index < events.size(); index++)
    {
        if (events[index].operation == operation && events[index].id == id)
        {
            return index;
        }
    }
    return events.size();
}

}
