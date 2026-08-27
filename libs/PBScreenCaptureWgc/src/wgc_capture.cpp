#include "capture_internal.h"
#include "pbprotocol/checked_integer.h"

#include <algorithm>
#include <limits>
#include <roapi.h>
#include <thread>

namespace pbscreencapturewgc
{
using namespace detail;

CaptureStatus ValidateWgcCaptureConfig(const WgcCaptureConfig& config) noexcept
{
    if (config.initialCaptureEpoch == 0 || config.initialCaptureEpoch == std::numeric_limits<std::uint64_t>::max() ||
        config.queuedFrameLimit == 0 || config.queuedFrameLimit > maximumQueuedFrames || config.roiTextureCount < 2 ||
        config.roiTextureCount > maximumRoiTextures || config.maximumDeviceRecoveries > 8 || config.gpuTimeoutMilliseconds == 0 ||
        config.gpuTimeoutMilliseconds > 60000 || config.maximumCaptureBytes == 0 || config.maximumRoiBytes == 0 ||
        config.maximumCaptureBytes == std::numeric_limits<std::uint64_t>::max() || config.maximumRoiBytes == std::numeric_limits<std::uint64_t>::max() ||
        (config.pixelFormat != DXGI_FORMAT_B8G8R8A8_UNORM && config.pixelFormat != DXGI_FORMAT_R16G16B16A16_FLOAT) ||
        (config.minUpdateInterval100ns && (*config.minUpdateInterval100ns < 0 || *config.minUpdateInterval100ns > 10000000)))
    {
        return CaptureStatus::Failure(CaptureError::InvalidConfiguration, CaptureStage::Configuration);
    }
    const auto width = static_cast<std::int64_t>(config.region.monitorPhysicalRect.right) - config.region.monitorPhysicalRect.left;
    const auto height = static_cast<std::int64_t>(config.region.monitorPhysicalRect.bottom) - config.region.monitorPhysicalRect.top;
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384)
    {
        return CaptureStatus::Failure(CaptureError::InvalidConfiguration, CaptureStage::Region);
    }
    CaptureEnvironment environment;
    environment.region = config.region;
    environment.contentSize = {static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)};
    CaptureLayout layout;
    return ValidateLayout(config, environment, layout);
}

const char* GetCaptureErrorName(const CaptureError error) noexcept
{
    switch (error)
    {
    case CaptureError::None: return "None";
    case CaptureError::InvalidConfiguration: return "InvalidConfiguration";
    case CaptureError::ResourceLimit: return "ResourceLimit";
    case CaptureError::Unsupported: return "Unsupported";
    case CaptureError::InvalidFrame: return "InvalidFrame";
    case CaptureError::RegionChanged: return "RegionChanged";
    case CaptureError::DeviceLost: return "DeviceLost";
    case CaptureError::AccessLost: return "AccessLost";
    case CaptureError::NativeFailure: return "NativeFailure";
    case CaptureError::ConsumerFailure: return "ConsumerFailure";
    case CaptureError::Timeout: return "Timeout";
    case CaptureError::WrongThread: return "WrongThread";
    case CaptureError::OutOfMemory: return "OutOfMemory";
    case CaptureError::InternalError: return "InternalError";
    case CaptureError::DpiAwarenessRequired: return "DpiAwarenessRequired";
    }
    return "Unknown";
}

struct WgcCapture::Implementation final : DeferredCleanup, std::enable_shared_from_this<Implementation>
{
    enum class SlotState : std::uint8_t
    {
        Free, Copying, Consuming
    };
    struct Slot
    {
        SlotState state = SlotState::Free;
        FrameLease source;
        RoiFrameMetadata metadata;
        std::chrono::steady_clock::time_point submittedAt;
    };

    Implementation(const WgcCaptureConfig& initialConfig, std::shared_ptr<RoiConsumer> initialConsumer, std::unique_ptr<CaptureBackend> initialBackend)
        : config(initialConfig), consumer(std::move(initialConsumer)), backend(std::move(initialBackend)), inbox(std::make_shared<FrameInbox>(config))
    {
        snapshot.captureEpoch = config.initialCaptureEpoch;
    }

    void Publish() noexcept
    {
        working.busyRoiTextures = static_cast<std::uint32_t>(std::ranges::count_if(slots, [](const Slot& slot)
        {
            return slot.state != SlotState::Free;
        }));
        working.environment = backend->GetEnvironment();
        working.capabilities = backend->GetCapabilities();
        const std::lock_guard lock(snapshotMutex);
        snapshot = working;
    }

    void SetError(const CaptureStatus error) noexcept
    {
        if (!error && working.error)
        {
            working.error = error;
        }
    }

    [[nodiscard]] bool AdvanceEpoch() noexcept
    {
        if (working.captureEpoch == std::numeric_limits<std::uint64_t>::max())
        {
            SetError(CaptureStatus::Failure(CaptureError::ResourceLimit, CaptureStage::Recreate));
            return false;
        }
        working.captureEpoch++;
        return true;
    }

    [[nodiscard]] CaptureStatus StartEpoch() noexcept
    {
        const auto status = backend->NotifyEpoch(*consumer, working.captureEpoch);
        if (!status)
        {
            return status;
        }
        inbox->Resume(backend->GetEnvironment().contentSize, working.captureEpoch);
        return backend->Start(working.captureEpoch);
    }

    void ReleaseSlots() noexcept
    {
        for (auto& slot : slots)
        {
            slot.source.Reset();
            slot.state = SlotState::Free;
        }
    }

    [[nodiscard]] bool PollSlots(const bool deliver) noexcept
    {
        bool allFree = true;
        for (std::size_t index = 0; index < config.roiTextureCount; index++)
        {
            auto& slot = slots[index];
            if (slot.state == SlotState::Free)
            {
                continue;
            }
            const auto completion = backend->Poll(index);
            SetError(completion.status);
            if (!completion.complete)
            {
                allFree = false;
                if (std::chrono::steady_clock::now() - slot.submittedAt >= std::chrono::milliseconds(config.gpuTimeoutMilliseconds))
                {
                    SetError(CaptureStatus::Failure(CaptureError::Timeout, CaptureStage::Completion));
                }
                continue;
            }
            if (slot.state == SlotState::Copying)
            {
                // Poll proved the GPU no longer reads this WGC Surface.
                slot.source.Reset();
                SetError(FromHresult(inbox->counters->closeError.load(), CaptureStage::Completion));
                const auto input = inbox->GetSnapshot();
                if (deliver && working.error && input.error && !input.recreateRequested && !input.stopRequested)
                {
                    slot.state = SlotState::Consuming;
                    slot.submittedAt = std::chrono::steady_clock::now();
                    slot.metadata.capabilities = backend->GetCapabilities();
                    const auto status = backend->Consume(*consumer, slot.metadata, index);
                    SetError(status);
                    if (status)
                    {
                        pbprotocol::SaturatingIncrementUnsigned(working.deliveredFrames);
                    }
                    allFree = false;
                    continue;
                }
            }
            slot.state = SlotState::Free;
        }
        return allFree;
    }

    void SubmitNewest() noexcept
    {
        for (std::size_t index = 0; index < config.roiTextureCount; index++)
        {
            auto& slot = slots[index];
            if (slot.state != SlotState::Free)
            {
                continue;
            }
            FrameLease source;
            if (!inbox->TakeNewest(source))
            {
                return;
            }
            slot.metadata = {source.captureEpoch, source.arrivalOrdinal, source.timestamp100ns, backend->GetEnvironment(), backend->GetCapabilities()};
            bool submitted = false;
            const auto status = backend->Copy(source, index, submitted);
            // Even a failed Signal/End path may follow a successfully submitted copy.
            if (submitted)
            {
                slot.source = std::move(source);
                slot.state = SlotState::Copying;
                slot.submittedAt = std::chrono::steady_clock::now();
                pbprotocol::SaturatingIncrementUnsigned(working.copiedFrames);
            }
            SetError(status);
            return;
        }
    }

    // A bounded drain, never in FrameArrived. Failure to prove completion must
    // not turn into an unsafe Close; the native backend preallocates a GPU event.
    [[nodiscard]] bool Drain() noexcept
    {
        working.state = CaptureState::Draining;
        inbox->Pause();
        SetError(backend->Pause());
        Publish();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(config.gpuTimeoutMilliseconds);
        do
        {
            if (backend->DeviceRemoved())
            {
                working.lastDeviceLoss = CaptureStatus::Failure(CaptureError::DeviceLost, CaptureStage::Device, backend->DeviceRemovalReason());
                ReleaseSlots();
            }
            const bool gpuIdle = PollSlots(false);
            if (gpuIdle && backend->CallbacksIdle())
            {
                return true;
            }
            inbox->Wait();
        } while (std::chrono::steady_clock::now() < deadline);
        SetError(CaptureStatus::Failure(CaptureError::Timeout, CaptureStage::Shutdown));
        return false;
    }

    void FinishShutdown() noexcept
    {
        ReleaseSlots();
        const auto status = backend->Shutdown();
        SetError(status);
        const HRESULT closeError = inbox->counters->closeError.load();
        SetError(FromHresult(closeError, CaptureStage::Shutdown));
        consumer.reset();
        working.shutdownComplete = true;
        working.deferredCleanup = false;
        working.state = working.error ? CaptureState::Stopped : CaptureState::Failed;
        Publish();
    }

    void CompleteDeferredShutdown() noexcept override
    {
        // The owner has handed off after its last GPU submission. No consumer or
        // backend access may race this callback; public snapshots use a mutex.
        FinishShutdown();
    }

    void End() noexcept
    {
        if (Drain())
        {
            FinishShutdown();
        }
        else
        {
            working.deferredCleanup = true;
            Publish();
            backend->DeferShutdown(shared_from_this());
            // No access to working/backend after handing off: completion can run now.
        }
    }

    void Run() noexcept
    {
        const HRESULT apartmentResult = RoInitialize(RO_INIT_MULTITHREADED);
        struct ApartmentGuard
        {
            bool initialized;
            ~ApartmentGuard()
            {
                if (initialized)
                {
                    RoUninitialize();
                }
            }
        };
        const ApartmentGuard apartment{SUCCEEDED(apartmentResult)};
        working.captureEpoch = config.initialCaptureEpoch;
        auto status = FromHresult(apartmentResult, CaptureStage::Apartment);
        if (status)
        {
            status = backend->Initialize(config, inbox);
        }
        if (status)
        {
            status = StartEpoch();
        }
        SetError(status);
        working.state = status ? CaptureState::Running : CaptureState::Failed;
        Publish();
        {
            const std::lock_guard lock(startMutex);
            startStatus = status;
            started = true;
        }
        startedCondition.notify_one();
        if (!status)
        {
            End();
            return;
        }
        auto nextEnvironmentCheck = std::chrono::steady_clock::now();
        for (;;)
        {
            auto input = inbox->GetSnapshot();
            SetError(input.error);
            SetError(FromHresult(inbox->counters->closeError.load(), CaptureStage::Callback));
            if (input.stopRequested)
            {
                break;
            }
            if (backend->DeviceRemoved())
            {
                const auto lost = CaptureStatus::Failure(CaptureError::DeviceLost, CaptureStage::Device, backend->DeviceRemovalReason());
                working.lastDeviceLoss = lost;
                if (working.deviceRecoveries == config.maximumDeviceRecoveries)
                {
                    SetError(lost);
                    static_cast<void>(AdvanceEpoch());
                    break;
                }
                if (!Drain())
                {
                    SetError(lost);
                    break;
                }
                status = backend->Shutdown();
                if (!status || !AdvanceEpoch())
                {
                    SetError(status);
                    break;
                }
                working.deviceRecoveries++;
                working.error = {};
                status = backend->Initialize(config, inbox);
                if (status)
                {
                    status = StartEpoch();
                }
                SetError(status);
                if (!status)
                {
                    break;
                }
                working.state = CaptureState::Running;
            }
            if (!working.error)
            {
                break;
            }
            if (std::chrono::steady_clock::now() >= nextEnvironmentCheck)
            {
                bool changed = false;
                status = backend->CheckEnvironment(changed);
                SetError(status);
                if (!status)
                {
                    break;
                }
                if (changed)
                {
                    inbox->RequestRecreate();
                }
                nextEnvironmentCheck = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
            }
            input = inbox->GetSnapshot();
            if (input.recreateRequested)
            {
                if (!Drain() || !working.error || inbox->GetSnapshot().stopRequested)
                {
                    break;
                }
                working.state = CaptureState::Recreating;
                Publish();
                status = backend->Recreate(input.requestedSize);
                if (!status || !AdvanceEpoch())
                {
                    SetError(status);
                    break;
                }
                pbprotocol::SaturatingIncrementUnsigned(working.recreates);
                status = StartEpoch();
                SetError(status);
                if (!status)
                {
                    break;
                }
                working.state = CaptureState::Running;
            }
            static_cast<void>(PollSlots(true));
            if (working.error && !inbox->GetSnapshot().stopRequested)
            {
                SubmitNewest();
            }
            Publish();
            inbox->Wait();
        }
        End();
    }

    const WgcCaptureConfig config;
    std::shared_ptr<RoiConsumer> consumer;
    std::unique_ptr<CaptureBackend> backend;
    std::shared_ptr<FrameInbox> inbox;
    std::array<Slot, maximumRoiTextures> slots;
    WgcCaptureSnapshot working;
    mutable std::mutex snapshotMutex;
    WgcCaptureSnapshot snapshot;
    std::thread owner;
    std::thread::id ownerId;
    std::mutex stopMutex;
    std::mutex startMutex;
    std::condition_variable startedCondition;
    bool started = false;
    CaptureStatus startStatus;
};

WgcCapture::WgcCapture(std::shared_ptr<Implementation> implementation) noexcept : implementation_(std::move(implementation))
{
}

WgcCapture::~WgcCapture()
{
    static_cast<void>(Stop());
}

CaptureStatus WgcCapture::Create(const WgcCaptureConfig& config, std::shared_ptr<RoiConsumer> consumer, std::unique_ptr<WgcCapture>& output) noexcept
{
    try
    {
        return WgcCaptureTestAccess::Create(config, std::move(consumer), MakeNativeCaptureBackend(), output);
    }
    catch (const std::bad_alloc&)
    {
        return CaptureStatus::Failure(CaptureError::OutOfMemory, CaptureStage::Configuration);
    }
    catch (...)
    {
        return CaptureStatus::Failure(CaptureError::InternalError, CaptureStage::Configuration);
    }
}

WgcCaptureSnapshot WgcCapture::GetSnapshot() const noexcept
{
    WgcCaptureSnapshot result;
    {
        const std::lock_guard lock(implementation_->snapshotMutex);
        result = implementation_->snapshot;
    }
    const auto input = implementation_->inbox->GetSnapshot();
    result.arrivedFrames = input.arrivedFrames;
    result.droppedFrames = input.droppedFrames;
    result.queuedFrames = input.queuedFrames;
    result.liveFrameLeases = implementation_->inbox->counters->live.load();
    result.frameLeaseHighWater = implementation_->inbox->counters->highWater.load();
    return result;
}

void WgcCapture::RequestStop() noexcept
{
    implementation_->inbox->RequestStop();
}

CaptureStatus WgcCapture::Stop() noexcept
{
    RequestStop();
    if (implementation_->ownerId == std::this_thread::get_id())
    {
        return CaptureStatus::Failure(CaptureError::WrongThread, CaptureStage::Shutdown);
    }
    const std::lock_guard lock(implementation_->stopMutex);
    if (implementation_->owner.joinable())
    {
        implementation_->owner.join();
    }
    return GetSnapshot().error;
}

CaptureStatus WgcCaptureTestAccess::Create(const WgcCaptureConfig& config, std::shared_ptr<RoiConsumer> consumer,
                                         std::unique_ptr<CaptureBackend> backend, std::unique_ptr<WgcCapture>& output) noexcept
{
    const auto validation = ValidateWgcCaptureConfig(config);
    if (!validation)
    {
        return validation;
    }
    if (!consumer || !backend)
    {
        return CaptureStatus::Failure(CaptureError::InvalidConfiguration, CaptureStage::Configuration);
    }
    try
    {
        const auto implementation = std::make_shared<WgcCapture::Implementation>(config, std::move(consumer), std::move(backend));
        auto capture = std::unique_ptr<WgcCapture>(new WgcCapture(implementation));
        implementation->owner = std::thread([implementation]
        {
            implementation->Run();
        });
        implementation->ownerId = implementation->owner.get_id();
        CaptureStatus status;
        {
            std::unique_lock lock(implementation->startMutex);
            implementation->startedCondition.wait(lock, [&]
            {
                return implementation->started;
            });
            status = implementation->startStatus;
        }
        if (!status)
        {
            return status;
        }
        output = std::move(capture);
        return {};
    }
    catch (const std::bad_alloc&)
    {
        return CaptureStatus::Failure(CaptureError::OutOfMemory, CaptureStage::Configuration);
    }
    catch (...)
    {
        return CaptureStatus::Failure(CaptureError::InternalError, CaptureStage::Configuration);
    }
}

void WgcCaptureTestAccess::RequestRecreate(WgcCapture& capture) noexcept
{
    capture.implementation_->inbox->RequestRecreate();
}

} // namespace pbscreencapturewgc
