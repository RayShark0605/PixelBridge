#include "capture_runtime.h"
#include "pbprotocol/checked_integer.h"

#include <algorithm>
#include <limits>
#include <roapi.h>
#include <thread>

namespace pbcapturenormalize
{
using namespace detail;

CaptureStatus ValidateCaptureConfig(const CaptureConfig& config, const CaptureBackendKind backendKind) noexcept
{
    if (config.initialCaptureEpoch == 0 || config.initialCaptureEpoch == std::numeric_limits<std::uint64_t>::max() ||
        config.queuedFrameLimit == 0 || config.queuedFrameLimit > maximumQueuedFrames || config.roiTextureCount < 2 ||
        config.roiTextureCount > maximumRoiTextures || config.maximumDeviceRecoveries > 8 || config.maximumFrameAgeMilliseconds > 60000 || config.gpuTimeoutMilliseconds == 0 ||
        config.gpuTimeoutMilliseconds > 60000 || config.maximumCaptureBytes == 0 || config.maximumRoiBytes == 0 ||
        config.maximumCaptureBytes == std::numeric_limits<std::uint64_t>::max() || config.maximumRoiBytes == std::numeric_limits<std::uint64_t>::max() ||
        (config.pixelFormat != DXGI_FORMAT_B8G8R8A8_UNORM && config.pixelFormat != DXGI_FORMAT_R16G16B16A16_FLOAT && config.pixelFormat != DXGI_FORMAT_R10G10B10A2_UNORM) ||
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
    environment.backendKind = backendKind;
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

} // namespace pbcapturenormalize

namespace pbcapturenormalize::detail
{
struct CaptureRuntime::Implementation final : DeferredCleanup, std::enable_shared_from_this<Implementation>
{
    enum class SlotState : std::uint8_t
    {
        Free, Copying, Consuming
    };
    struct Slot
    {
        SlotState state = SlotState::Free;
        FrameLease source;
        RawRoiFrameMetadata metadata;
        std::uint64_t generation = 0;
        std::chrono::steady_clock::time_point submittedAt;
    };

    Implementation(const CaptureConfig& initialConfig, std::shared_ptr<RawRoiConsumer> initialConsumer, std::unique_ptr<CaptureBackend> initialBackend)
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
        backend->UpdateSnapshot(working);
        const std::lock_guard lock(snapshotMutex);
        snapshot = working;
    }

    [[nodiscard]] static bool IsDeviceLoss(const CaptureStatus status) noexcept
    {
        const auto nativeError = static_cast<HRESULT>(status.nativeError);
        return status.code == CaptureError::DeviceLost ||
               (status.code == CaptureError::NativeFailure &&
                (nativeError == DXGI_ERROR_DEVICE_REMOVED || nativeError == DXGI_ERROR_DEVICE_RESET || nativeError == DXGI_ERROR_DEVICE_HUNG));
    }

    void SetError(const CaptureStatus error) noexcept
    {
        // Recovery can replace a lost device, not undo failed consumer cleanup.
        // Preserve the first non-device error even if a removal was observed first.
        if (!error && (working.error || (IsDeviceLoss(working.error) && !IsDeviceLoss(error))))
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
        if (!backend->WaitingForEnvironment())
        {
            if (sourceGeneration == std::numeric_limits<std::uint64_t>::max())
            {
                return CaptureStatus::Failure(CaptureError::ResourceLimit, CaptureStage::Recreate);
            }
            sourceGeneration++;
            notifiedEpoch = working.captureEpoch;
            const auto status = backend->NotifyEpoch(*consumer, working.captureEpoch);
            if (!status)
            {
                return status;
            }
        }
        inbox->Resume(backend->GetEnvironment().contentSize, working.captureEpoch);
        return backend->Start(working.captureEpoch);
    }

    [[nodiscard]] bool Expired(const std::int64_t timestamp100ns) noexcept
    {
        if (config.maximumFrameAgeMilliseconds == 0)
        {
            return false;
        }
        LARGE_INTEGER now{};
        std::int64_t now100ns = 0;
        if (!QueryPerformanceCounter(&now) || !ConvertQpcTo100ns(now.QuadPart, clockFrequency, now100ns))
        {
            pbprotocol::SaturatingIncrementUnsigned(working.expiredFrames);
            return true;
        }
        const auto classification = ClassifyFrameAge(now100ns, timestamp100ns, config.maximumFrameAgeMilliseconds);
        working.frameAgeHighWater100ns = std::max(working.frameAgeHighWater100ns, classification.age100ns);
        if (classification.disposition != CaptureFrameAgeDisposition::Current)
        {
            pbprotocol::SaturatingIncrementUnsigned(working.expiredFrames);
            return true;
        }
        return false;
    }

    [[nodiscard]] bool Stale(const std::uint64_t arrivalOrdinal) noexcept
    {
        if (config.maximumFrameAgeMilliseconds != 0 && arrivalOrdinal <= lastDeliveredOrdinal)
        {
            pbprotocol::SaturatingIncrementUnsigned(working.staleFrames);
            return true;
        }
        return false;
    }

    void ReleaseSlots() noexcept
    {
        for (std::size_t index = 0; index < slots.size(); index++)
        {
            auto& slot = slots[index];
            if (slot.state == SlotState::Consuming)
            {
                SetError(backend->Complete(*consumer, slot.metadata, index, true));
            }
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
                // Poll proved the GPU no longer reads this OS capture surface.
                slot.source.Reset();
                SetError(FromHresult(inbox->counters->closeError.load(), CaptureStage::Completion));
                const auto input = inbox->GetSnapshot();
                if (deliver && working.error && input.error && !input.recreateRequested && !input.stopRequested &&
                    !Expired(slot.metadata.systemRelativeTime100ns) && !Stale(slot.metadata.arrivalOrdinal))
                {
                    // Slot reuse is not capture ordering: a newer copy in a
                    // low-numbered slot can retire before an older high slot.
                    lastDeliveredOrdinal = slot.metadata.arrivalOrdinal;
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
            if (slot.state == SlotState::Consuming)
            {
                const auto input = inbox->GetSnapshot();
                const bool cancelled = !deliver || !working.error || !input.error || input.recreateRequested || input.stopRequested ||
                                       slot.metadata.captureEpoch != working.captureEpoch;
                SetError(backend->Complete(*consumer, slot.metadata, index, cancelled));
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
            if (Expired(source.timestamp100ns))
            {
                return;
            }
            if (slot.generation == std::numeric_limits<std::uint64_t>::max())
            {
                SetError(CaptureStatus::Failure(CaptureError::ResourceLimit, CaptureStage::TextureRing));
                return;
            }
            slot.generation++;
            slot.metadata = {source.captureEpoch, source.arrivalOrdinal, source.timestamp100ns, backend->GetEnvironment(), backend->GetCapabilities()};
            slot.metadata.cursorState = source.cursorState;
            slot.metadata.pointer = source.pointer;
            slot.metadata.sourceGeneration = sourceGeneration;
            slot.metadata.slotGeneration = slot.generation;
            slot.metadata.slotIndex = static_cast<std::uint32_t>(index);
            slot.metadata.timestampDomain = source.timestampDomain;
            slot.metadata.rawTimestamp = source.rawTimestamp;
            slot.metadata.rawFrequency = source.rawFrequency;
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
        if (notifiedEpoch != 0)
        {
            // Invalidates already-running CPU work, not only the capture queue.
            // This precedes waiting for GPU retirement or a replacement device.
            consumer->EpochInvalidated(notifiedEpoch);
            notifiedEpoch = 0;
        }
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
        LARGE_INTEGER frequency{};
        if (status && config.maximumFrameAgeMilliseconds != 0 && (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0))
        {
            status = CaptureStatus::Failure(CaptureError::Unsupported, CaptureStage::Configuration);
        }
        clockFrequency = frequency.QuadPart;
        if (status)
        {
            status = backend->Initialize(config, inbox);
        }
        if (status)
        {
            status = StartEpoch();
        }
        SetError(status);
        working.state = status ? (backend->WaitingForEnvironment() ? CaptureState::WaitingForEnvironment : CaptureState::Running) : CaptureState::Failed;
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
                SetError(status);
                const HRESULT closeError = inbox->counters->closeError.load();
                SetError(FromHresult(closeError, CaptureStage::Shutdown));
                if (!status || (!working.error && !IsDeviceLoss(working.error)) || inbox->GetSnapshot().stopRequested || !AdvanceEpoch())
                {
                    break;
                }
                working.deviceRecoveries++;
                // Every old producer/source is quiescent here. Device-loss
                // release errors belong to the retired device generation, not
                // to the replacement. Unrelated Close failures remain latched.
                if (closeError == DXGI_ERROR_DEVICE_REMOVED || closeError == DXGI_ERROR_DEVICE_RESET || closeError == DXGI_ERROR_DEVICE_HUNG)
                {
                    inbox->counters->closeError.store(S_OK);
                }
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
                working.state = backend->WaitingForEnvironment() ? CaptureState::WaitingForEnvironment : CaptureState::Running;
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
                const bool wasWaiting = backend->WaitingForEnvironment();
                status = backend->Recreate(input.requestedSize);
                // Reserve a new epoch when invalidating live resources, not on
                // every environment retry while the desktop remains unavailable.
                if (!status || (!wasWaiting && !AdvanceEpoch()))
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
                working.state = backend->WaitingForEnvironment() ? CaptureState::WaitingForEnvironment : CaptureState::Running;
            }
            static_cast<void>(PollSlots(!backend->WaitingForEnvironment()));
            input = inbox->GetSnapshot();
            if (working.error && !input.stopRequested && !input.recreateRequested &&
                std::ranges::any_of(slots.begin(), slots.begin() + config.roiTextureCount, [](const Slot& slot) { return slot.state == SlotState::Free; }))
            {
                SetError(backend->Acquire());
                input = inbox->GetSnapshot();
                if (working.error && !backend->WaitingForEnvironment() && !input.stopRequested && !input.recreateRequested)
                {
                    SubmitNewest();
                }
            }
            Publish();
            inbox->Wait();
        }
        End();
    }

    const CaptureConfig config;
    std::shared_ptr<RawRoiConsumer> consumer;
    std::unique_ptr<CaptureBackend> backend;
    std::shared_ptr<FrameInbox> inbox;
    std::array<Slot, maximumRoiTextures> slots;
    CaptureSnapshot working;
    mutable std::mutex snapshotMutex;
    CaptureSnapshot snapshot;
    std::thread owner;
    std::thread::id ownerId;
    std::mutex stopMutex;
    std::mutex startMutex;
    std::condition_variable startedCondition;
    bool started = false;
    CaptureStatus startStatus;
    std::int64_t clockFrequency = 0;
    std::uint64_t lastDeliveredOrdinal = 0;
    std::uint64_t sourceGeneration = 0;
    std::uint64_t notifiedEpoch = 0;
};

CaptureRuntime::CaptureRuntime(std::shared_ptr<Implementation> implementation) noexcept : implementation_(std::move(implementation))
{
}

CaptureRuntime::~CaptureRuntime()
{
    static_cast<void>(Stop());
}

CaptureSnapshot CaptureRuntime::GetSnapshot() const noexcept
{
    CaptureSnapshot result;
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

void CaptureRuntime::RequestStop() noexcept
{
    implementation_->inbox->RequestStop();
}

CaptureStatus CaptureRuntime::Stop() noexcept
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

CaptureStatus CaptureRuntime::Create(const CaptureConfig& config, std::shared_ptr<RawRoiConsumer> consumer,
                                         std::unique_ptr<CaptureBackend> backend, std::unique_ptr<CaptureRuntime>& output) noexcept
{
    const auto validation = ValidateCaptureConfig(config, backend ? backend->Kind() : CaptureBackendKind::Wgc);
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
        const auto implementation = std::make_shared<CaptureRuntime::Implementation>(config, std::move(consumer), std::move(backend));
        auto capture = std::unique_ptr<CaptureRuntime>(new CaptureRuntime(implementation));
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

void CaptureRuntime::RequestRecreate() noexcept
{
    implementation_->inbox->RequestRecreate();
}

} // namespace pbcapturenormalize::detail
