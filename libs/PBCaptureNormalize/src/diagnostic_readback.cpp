#include "pbcapturenormalize/diagnostic_readback.h"
#include "diagnostic_readback_internal.h"
#include "capture_runtime.h"
#include "pbprotocol/checked_integer.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <process.h>
#include <thread>
#include <utility>
#include <wrl/client.h>

namespace pbcapturenormalize
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr std::size_t noCpuBuffer = diagnosticCpuBufferCount;

bool IsGraphicsDeviceLoss(const CaptureStatus status) noexcept
{
    const auto nativeError = static_cast<HRESULT>(status.nativeError);
    return status.code == CaptureError::DeviceLost || (status.code == CaptureError::NativeFailure &&
           (nativeError == DXGI_ERROR_DEVICE_REMOVED || nativeError == DXGI_ERROR_DEVICE_RESET || nativeError == DXGI_ERROR_DEVICE_HUNG));
}

std::uint64_t PixelBytes(const DXGI_FORMAT format) noexcept
{
    switch (format)
    {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM: return 4;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
    default: return 0;
    }
}

bool ValidDomain(const ScreenCaptureDomain& domain) noexcept
{
    return domain.captureEpoch != 0 && std::ranges::any_of(domain.sourceId, [](const std::byte value) { return value != std::byte{0}; });
}

bool SameFrameKey(const ScreenCaptureFrameMetadata& left, const ScreenCaptureFrameMetadata& right) noexcept
{
    return left.domain == right.domain && left.sourceGeneration == right.sourceGeneration && left.captureObservation == right.captureObservation &&
           left.slotIndex == right.slotIndex && left.slotGeneration == right.slotGeneration;
}

bool SameRectangle(const RECT& left, const RECT& right) noexcept
{
    return left.left == right.left && left.top == right.top && left.right == right.right && left.bottom == right.bottom;
}

bool ReadNow100ns(const std::int64_t frequency, std::int64_t& output) noexcept
{
    LARGE_INTEGER counter{};
    return QueryPerformanceCounter(&counter) && ConvertQpcTo100ns(counter.QuadPart, frequency, output);
}

enum class CpuBufferState : std::uint8_t
{
    Free, Filling, Queued, Processing
};

struct CpuBuffer
{
    std::unique_ptr<std::byte[]> pixels;
    CpuBufferState state = CpuBufferState::Free;
    ScreenCaptureFrameMetadata metadata;
    std::size_t rowPitch = 0;
    std::size_t usedBytes = 0;
    std::uint64_t revision = 0;
};

// This is the worker's only self-retained object. It intentionally contains no
// device, context, texture, OS acquisition, capture owner, or readback pointer.
struct CpuState
{
    CpuState(const DiagnosticReadbackConfig& initialConfig, const DiagnosticReadbackBudget& budget,
             std::shared_ptr<CpuFrameProcessor> initialProcessor, const std::int64_t frequency)
        : config(initialConfig), processor(std::move(initialProcessor)), clockFrequency(frequency)
    {
        snapshot.reservation = budget;
        for (auto& buffer : buffers)
        {
            buffer.pixels = std::make_unique<std::byte[]>(static_cast<std::size_t>(budget.bytesPerCpuBuffer));
        }
    }

    void UpdateUsageLocked() noexcept
    {
        snapshot.queuedFrames = queuedIndex == noCpuBuffer ? 0u : 1u;
        snapshot.cpuBuffersInUse = static_cast<std::uint32_t>(std::ranges::count_if(buffers, [](const CpuBuffer& buffer) { return buffer.state != CpuBufferState::Free; }));
        snapshot.queueHighWater = std::max(snapshot.queueHighWater, snapshot.queuedFrames);
        snapshot.cpuBufferHighWater = std::max(snapshot.cpuBufferHighWater, snapshot.cpuBuffersInUse);
    }

    void DropLocked(const DiagnosticReadbackDropReason reason) noexcept
    {
        pbprotocol::SaturatingIncrementUnsigned(snapshot.dropEvents);
        pbprotocol::SaturatingIncrementUnsigned(snapshot.drops[static_cast<std::size_t>(reason)]);
        snapshot.lastDrop = reason;
    }

    void ClearQueuedLocked(const DiagnosticReadbackDropReason reason) noexcept
    {
        if (queuedIndex != noCpuBuffer)
        {
            buffers[queuedIndex].state = CpuBufferState::Free;
            queuedIndex = noCpuBuffer;
            DropLocked(reason);
        }
        UpdateUsageLocked();
    }

    void ChangeDomainLocked(std::optional<ScreenCaptureDomain> domain) noexcept
    {
        if (revision == std::numeric_limits<std::uint64_t>::max())
        {
            if (snapshot.error)
            {
                snapshot.error = CaptureStatus::Failure(CaptureError::ResourceLimit, CaptureStage::Consumer);
            }
            snapshot.stopRequested = true;
            domain.reset();
        }
        else
        {
            revision++;
        }
        activeDomain = std::move(domain);
        snapshot.active = activeDomain.has_value();
        if (activeDomain)
        {
            snapshot.domain = *activeDomain;
        }
        snapshot.resetPending = true;
        lastCommittedObservation = 0;
        ClearQueuedLocked(snapshot.stopRequested ? DiagnosticReadbackDropReason::Stopping : DiagnosticReadbackDropReason::InactiveDomain);
    }

    void StopLocked() noexcept
    {
        if (!snapshot.stopRequested)
        {
            snapshot.stopRequested = true;
            ChangeDomainLocked(std::nullopt);
        }
    }

    void FailLocked(const CaptureStatus status) noexcept
    {
        if (!status && snapshot.error)
        {
            snapshot.error = status;
        }
        StopLocked();
    }

    void GraphicsFailedLocked(const CaptureStatus status) noexcept
    {
        snapshot.lastGraphicsFailure = status;
        if (IsGraphicsDeviceLoss(status))
        {
            pbprotocol::SaturatingIncrementUnsigned(snapshot.deviceLossEvents);
            // Only the capture owner may decide to rebuild or exhaust its
            // recovery allowance. Retire this domain's CPU candidates without
            // poisoning the independently allocated worker or clearing Stop.
            if (!snapshot.stopRequested)
            {
                ChangeDomainLocked(std::nullopt);
            }
        }
        else
        {
            FailLocked(status);
        }
    }

    bool CurrentDomainLocked(const ScreenCaptureFrameMetadata& metadata, const std::uint64_t jobRevision) const noexcept
    {
        return !snapshot.stopRequested && activeDomain && *activeDomain == metadata.domain && revision == jobRevision;
    }

    bool FreshLocked(const std::int64_t timestamp100ns, const DiagnosticReadbackDropReason expiredReason, std::int64_t& now100ns) noexcept
    {
        if (!ReadNow100ns(clockFrequency, now100ns))
        {
            DropLocked(DiagnosticReadbackDropReason::InvalidTimestamp);
            return false;
        }
        const auto age = ClassifyFrameAge(now100ns, timestamp100ns, config.maximumFrameAgeMilliseconds);
        snapshot.frameAgeHighWater100ns = std::max(snapshot.frameAgeHighWater100ns, age.age100ns);
        if (age.disposition == CaptureFrameAgeDisposition::InvalidTimestamp)
        {
            DropLocked(DiagnosticReadbackDropReason::InvalidTimestamp);
            return false;
        }
        if (age.disposition == CaptureFrameAgeDisposition::Expired)
        {
            DropLocked(expiredReason);
            return false;
        }
        return true;
    }

    void Run() noexcept
    {
        for (;;)
        {
            std::unique_lock lock(mutex);
            wake.wait(lock, [&] { return snapshot.resetPending || snapshot.stopRequested || queuedIndex != noCpuBuffer; });
            if (snapshot.resetPending)
            {
                const auto resetRevision = revision;
                const auto resetDomain = activeDomain;
                lock.unlock();
                processor->Reset(resetDomain);
                lock.lock();
                pbprotocol::SaturatingIncrementUnsigned(snapshot.processorResets);
                if (revision == resetRevision && activeDomain == resetDomain)
                {
                    snapshot.resetPending = false;
                }
                continue;
            }
            if (snapshot.stopRequested)
            {
                snapshot.workerStopped = true;
                return;
            }
            const auto bufferIndex = std::exchange(queuedIndex, noCpuBuffer);
            auto& buffer = buffers[bufferIndex];
            buffer.state = CpuBufferState::Processing;
            UpdateUsageLocked();
            std::int64_t analyzeStart100ns = 0;
            if (!CurrentDomainLocked(buffer.metadata, buffer.revision))
            {
                DropLocked(DiagnosticReadbackDropReason::InactiveDomain);
                buffer.state = CpuBufferState::Free;
                UpdateUsageLocked();
                continue;
            }
            if (!FreshLocked(buffer.metadata.timestamp.monotonic100ns, DiagnosticReadbackDropReason::ExpiredBeforeAnalyze, analyzeStart100ns))
            {
                buffer.state = CpuBufferState::Free;
                UpdateUsageLocked();
                continue;
            }
            snapshot.workerBusy = true;
            pbprotocol::SaturatingIncrementUnsigned(snapshot.analyzedFrames);
            lock.unlock();
            CaptureStatus status;
            try
            {
                status = processor->Analyze(buffer.metadata, {buffer.pixels.get(), buffer.usedBytes}, buffer.rowPitch);
            }
            catch (...)
            {
                status = CaptureStatus::Failure(CaptureError::ConsumerFailure, CaptureStage::Consumer);
            }
            std::int64_t analyzeEnd100ns = 0;
            const bool endTimestampValid = ReadNow100ns(clockFrequency, analyzeEnd100ns);
            lock.lock();
            if (endTimestampValid && analyzeEnd100ns >= analyzeStart100ns)
            {
                snapshot.analysisLatencyHighWater100ns = std::max(snapshot.analysisLatencyHighWater100ns, static_cast<std::uint64_t>(analyzeEnd100ns - analyzeStart100ns));
            }
            bool commit = false;
            if (!CurrentDomainLocked(buffer.metadata, buffer.revision))
            {
                // A late failure is as stale as a late successful result. It
                // must not poison or stop the replacement capture domain.
                DropLocked(snapshot.stopRequested ? DiagnosticReadbackDropReason::Stopping : DiagnosticReadbackDropReason::InactiveDomain);
            }
            else if (!status)
            {
                DropLocked(DiagnosticReadbackDropReason::ProcessorFailure);
                FailLocked(status);
            }
            else if (buffer.metadata.captureObservation <= lastCommittedObservation)
            {
                DropLocked(DiagnosticReadbackDropReason::StaleObservation);
            }
            else
            {
                std::int64_t now100ns = 0;
                commit = FreshLocked(buffer.metadata.timestamp.monotonic100ns, DiagnosticReadbackDropReason::ExpiredBeforeCommit, now100ns);
            }
            if (commit)
            {
                // Domain invalidation takes this same short mutex. Analyze may
                // race a rebuild, but publishing its old candidate cannot.
                processor->Commit(buffer.metadata);
                lastCommittedObservation = buffer.metadata.captureObservation;
                pbprotocol::SaturatingIncrementUnsigned(snapshot.committedFrames);
            }
            else
            {
                lock.unlock();
                processor->Discard();
                lock.lock();
                pbprotocol::SaturatingIncrementUnsigned(snapshot.discardedCandidates);
            }
            snapshot.workerBusy = false;
            buffer.state = CpuBufferState::Free;
            UpdateUsageLocked();
        }
    }

    const DiagnosticReadbackConfig config;
    std::shared_ptr<CpuFrameProcessor> processor;
    const std::int64_t clockFrequency;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::array<CpuBuffer, diagnosticCpuBufferCount> buffers;
    std::optional<ScreenCaptureDomain> activeDomain;
    std::size_t queuedIndex = noCpuBuffer;
    std::uint64_t revision = 0;
    std::uint64_t lastCommittedObservation = 0;
    DiagnosticReadbackSnapshot snapshot;
};

unsigned __stdcall RunCpuWorker(void* const parameter) noexcept
{
    const std::unique_ptr<std::shared_ptr<CpuState>> holder(static_cast<std::shared_ptr<CpuState>*>(parameter));
    const auto state = *holder;
    state->Run();
    return 0;
}

struct ThreadHandle
{
    ThreadHandle() = default;
    ThreadHandle(const ThreadHandle&) = delete;
    ThreadHandle& operator=(const ThreadHandle&) = delete;
    ThreadHandle(ThreadHandle&&) = delete;
    ThreadHandle& operator=(ThreadHandle&&) = delete;
    ~ThreadHandle()
    {
        if (value != nullptr)
        {
            CloseHandle(value);
        }
    }
    HANDLE value = nullptr;
};

enum class StagingPhase : std::uint8_t
{
    Idle, Skipped, Copied, Completing
};

struct StagingSlot
{
    ComPtr<ID3D11Texture2D> texture;
    ScreenCaptureFrameMetadata metadata;
    StagingPhase phase = StagingPhase::Idle;
    std::uint64_t lastGeneration = 0;
    std::uint64_t revision = 0;
    std::int64_t submitted100ns = 0;
};

struct MappedResource
{
    MappedResource(ID3D11DeviceContext* initialContext, ID3D11Texture2D* initialTexture) noexcept : context(initialContext), texture(initialTexture)
    {
    }
    MappedResource(const MappedResource&) = delete;
    MappedResource& operator=(const MappedResource&) = delete;
    ~MappedResource()
    {
        context->Unmap(texture, 0);
    }
    ID3D11DeviceContext* const context;
    ID3D11Texture2D* const texture;
};
}

CaptureStatus CalculateDiagnosticReadbackBudget(const DiagnosticReadbackConfig& config, DiagnosticReadbackBudget& output) noexcept
{
    if (config.maximumRoiSize.width <= 0 || config.maximumRoiSize.height <= 0 || config.maximumRoiSize.width > 16384 || config.maximumRoiSize.height > 16384 ||
        config.stagingTextureCount < 2 || config.stagingTextureCount > detail::maximumRoiTextures || config.maximumFrameAgeMilliseconds == 0 ||
        config.maximumFrameAgeMilliseconds > 60000 || config.maximumReadbackBytes == 0 || config.maximumReadbackBytes == std::numeric_limits<std::uint64_t>::max())
    {
        return CaptureStatus::Failure(CaptureError::InvalidConfiguration, CaptureStage::Configuration);
    }
    const auto pixels = pbprotocol::CheckedMultiplyUint64(static_cast<std::uint64_t>(config.maximumRoiSize.width), static_cast<std::uint64_t>(config.maximumRoiSize.height));
    const auto bytes = pixels ? pbprotocol::CheckedMultiplyUint64(pixels.Value(), 8) : pixels;
    const auto cpu = bytes ? pbprotocol::CheckedMultiplyUint64(bytes.Value(), diagnosticCpuBufferCount) : bytes;
    const auto staging = bytes ? pbprotocol::CheckedMultiplyUint64(bytes.Value(), config.stagingTextureCount) : bytes;
    const auto total = cpu && staging ? pbprotocol::CheckedAddUint64(cpu.Value(), staging.Value()) : cpu;
    if (!bytes || !cpu || !staging || !total || !pbprotocol::CheckedUint64ToSize(bytes.Value()) || total.Value() > config.maximumReadbackBytes)
    {
        return CaptureStatus::Failure(CaptureError::ResourceLimit, CaptureStage::Configuration);
    }
    output = {bytes.Value(), cpu.Value(), staging.Value(), total.Value()};
    return {};
}

struct DiagnosticCpuReadback::Implementation
{
    Implementation(std::shared_ptr<CpuState> initialCpu, std::shared_ptr<detail::DiagnosticReadbackGraphicsApi> initialGraphics) :
        cpu(std::move(initialCpu)), graphics(std::move(initialGraphics))
    {
    }

    void FinishStageLocked(const std::size_t slotIndex) noexcept
    {
        auto& slot = staging[slotIndex];
        slot.phase = StagingPhase::Idle;
        slot.metadata = {};
        slot.submitted100ns = 0;
        cpu->snapshot.pendingStagingFrames--;
    }

    CaptureStatus ValidateTextureLocked(const ScreenCaptureFrame& frame, ID3D11DeviceContext* const context) const noexcept
    {
        const auto& metadata = frame.metadata;
        if (std::this_thread::get_id() != ownerThread)
        {
            return CaptureStatus::Failure(CaptureError::WrongThread, CaptureStage::Consumer);
        }
        if (context == nullptr || frame.texture == nullptr || metadata.captureObservation == 0 || metadata.sourceGeneration == 0 || metadata.slotGeneration == 0 ||
            metadata.slotIndex >= cpu->config.stagingTextureCount || metadata.roiSize != roiSize || metadata.pixelFormat != environment.pixelFormat ||
            !SameRectangle(metadata.physicalRoi, environment.region.physicalRect) || !metadata.isCursorExcluded ||
            (sourceGeneration != 0 && metadata.sourceGeneration != sourceGeneration) || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
        {
            return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Consumer);
        }
        ComPtr<ID3D11Device> contextDevice;
        ComPtr<ID3D11Device> textureDevice;
        context->GetDevice(&contextDevice);
        frame.texture->GetDevice(&textureDevice);
        D3D11_TEXTURE2D_DESC description{};
        frame.texture->GetDesc(&description);
        if (contextDevice.Get() != device.Get() || textureDevice.Get() != device.Get() || description.Width != static_cast<UINT>(roiSize.width) ||
            description.Height != static_cast<UINT>(roiSize.height) || description.Format != environment.pixelFormat || description.MipLevels != 1 ||
            description.ArraySize != 1 || description.SampleDesc.Count != 1 || description.SampleDesc.Quality != 0 || description.Usage != D3D11_USAGE_DEFAULT || description.CPUAccessFlags != 0)
        {
            return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Consumer);
        }
        return {};
    }

    std::shared_ptr<CpuState> cpu;
    const std::shared_ptr<detail::DiagnosticReadbackGraphicsApi> graphics;
    ThreadHandle worker;
    unsigned workerId = 0;
    // Owner-only GPU calls; the CPU mutex also guards cancellation and slot
    // metadata. No GPU object is reachable from the worker's shared CpuState.
    ComPtr<ID3D11Device> device;
    std::array<StagingSlot, detail::maximumRoiTextures> staging;
    CaptureEnvironment environment;
    CaptureSize roiSize;
    std::thread::id ownerThread;
    std::uint64_t sourceGeneration = 0;
    std::uint64_t lastSubmittedObservation = 0;
    bool rebuilding = false;
};

DiagnosticCpuReadback::DiagnosticCpuReadback(std::unique_ptr<Implementation> implementation) noexcept : implementation_(std::move(implementation))
{
}

DiagnosticCpuReadback::~DiagnosticCpuReadback()
{
    RequestStop();
}

CaptureStatus DiagnosticCpuReadback::Create(const DiagnosticReadbackConfig& config, std::shared_ptr<CpuFrameProcessor> processor,
                                              std::shared_ptr<DiagnosticCpuReadback>& output) noexcept
{
    return detail::DiagnosticReadbackTestAccess::Create(config, std::move(processor), {}, output);
}

CaptureStatus detail::DiagnosticReadbackTestAccess::Create(const DiagnosticReadbackConfig& config, std::shared_ptr<CpuFrameProcessor> processor,
                                                          std::shared_ptr<DiagnosticReadbackGraphicsApi> graphics,
                                                          std::shared_ptr<DiagnosticCpuReadback>& output) noexcept
{
    DiagnosticReadbackBudget budget;
    const auto status = CalculateDiagnosticReadbackBudget(config, budget);
    if (!status)
    {
        return status;
    }
    if (!processor)
    {
        return CaptureStatus::Failure(CaptureError::InvalidConfiguration, CaptureStage::Configuration);
    }
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
    {
        return CaptureStatus::Failure(CaptureError::Unsupported, CaptureStage::Configuration);
    }
    try
    {
        if (!graphics)
        {
            graphics = std::make_shared<detail::NativeReadbackGraphicsApi>();
        }
        const auto cpu = std::make_shared<CpuState>(config, budget, std::move(processor), frequency.QuadPart);
        auto candidate = std::shared_ptr<DiagnosticCpuReadback>(new DiagnosticCpuReadback(std::make_unique<DiagnosticCpuReadback::Implementation>(cpu, std::move(graphics))));
        auto holder = std::make_unique<std::shared_ptr<CpuState>>(cpu);
        const auto handle = _beginthreadex(nullptr, 0, RunCpuWorker, holder.get(), 0, &candidate->implementation_->workerId);
        if (handle == 0)
        {
            return CaptureStatus::Failure(errno == ENOMEM || errno == EAGAIN ? CaptureError::OutOfMemory : CaptureError::NativeFailure, CaptureStage::Configuration, errno);
        }
        static_cast<void>(holder.release());
        candidate->implementation_->worker.value = reinterpret_cast<HANDLE>(handle);
        output = std::move(candidate);
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

std::uint64_t DiagnosticCpuReadback::ReservedBytes() const noexcept
{
    return implementation_->cpu->snapshot.reservation.totalBytes;
}

CaptureStatus DiagnosticCpuReadback::ValidateConfiguration(const CaptureConfig& config) const noexcept
{
    const auto& readback = implementation_->cpu->config;
    const auto width = static_cast<std::int64_t>(config.region.physicalRect.right) - config.region.physicalRect.left;
    const auto height = static_cast<std::int64_t>(config.region.physicalRect.bottom) - config.region.physicalRect.top;
    if (config.roiTextureCount < 2 || config.roiTextureCount > readback.stagingTextureCount || width <= 0 || height <= 0 ||
        width > readback.maximumRoiSize.width || height > readback.maximumRoiSize.height || config.maximumFrameAgeMilliseconds == 0 ||
        readback.maximumFrameAgeMilliseconds > config.maximumFrameAgeMilliseconds)
    {
        return CaptureStatus::Failure(CaptureError::InvalidConfiguration, CaptureStage::Configuration);
    }
    return {};
}

CaptureStatus DiagnosticCpuReadback::DomainStarted(const ScreenCaptureDomain& domain, const CaptureEnvironment& environment, ID3D11Device* const device)
{
    const auto width = static_cast<std::int64_t>(environment.region.physicalRect.right) - environment.region.physicalRect.left;
    const auto height = static_cast<std::int64_t>(environment.region.physicalRect.bottom) - environment.region.physicalRect.top;
    auto& implementation = *implementation_;
    const auto& cpu = implementation.cpu;
    const auto pixelBytes = PixelBytes(environment.pixelFormat);
    if (!ValidDomain(domain) || device == nullptr || width <= 0 || height <= 0 || width > cpu->config.maximumRoiSize.width || height > cpu->config.maximumRoiSize.height || pixelBytes == 0)
    {
        return CaptureStatus::Failure(CaptureError::InvalidConfiguration, CaptureStage::Consumer);
    }
    const auto pixels = pbprotocol::CheckedMultiplyUint64(static_cast<std::uint64_t>(width), static_cast<std::uint64_t>(height));
    const auto textureBytes = pixels ? pbprotocol::CheckedMultiplyUint64(pixels.Value(), pixelBytes) : pixels;
    const auto residentBytes = textureBytes ? pbprotocol::CheckedMultiplyUint64(textureBytes.Value(), cpu->config.stagingTextureCount) : textureBytes;
    if (!residentBytes || residentBytes.Value() > cpu->snapshot.reservation.stagingBytes)
    {
        return CaptureStatus::Failure(CaptureError::ResourceLimit, CaptureStage::Consumer);
    }
    std::array<StagingSlot, detail::maximumRoiTextures> replacement;
    {
        const std::lock_guard lock(cpu->mutex);
        if (cpu->snapshot.stopRequested || !cpu->snapshot.error)
        {
            return cpu->snapshot.error ? CaptureStatus::Failure(CaptureError::InvalidConfiguration, CaptureStage::Consumer) : cpu->snapshot.error;
        }
        if (implementation.rebuilding || cpu->activeDomain || cpu->snapshot.pendingStagingFrames != 0)
        {
            return CaptureStatus::Failure(CaptureError::InvalidConfiguration, CaptureStage::Consumer);
        }
        implementation.rebuilding = true;
        replacement = std::move(implementation.staging);
        implementation.device.Reset();
        cpu->snapshot.residentStagingBytes = 0;
    }
    // Release the entire old generation BEFORE allocating its replacement.
    // An old CPU Analyze holds only a distinct fixed CPU buffer, never staging.
    replacement = {};
    D3D11_TEXTURE2D_DESC description{};
    description.Width = static_cast<UINT>(width);
    description.Height = static_cast<UINT>(height);
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = environment.pixelFormat;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CaptureStatus status;
    for (std::size_t index = 0; index < cpu->config.stagingTextureCount; index++)
    {
        status = detail::FromHresult(implementation.graphics->CreateStaging(device, description, &replacement[index].texture), CaptureStage::Consumer);
        if (status && !replacement[index].texture)
        {
            status = CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Consumer);
        }
        if (!status)
        {
            break;
        }
    }
    {
        const std::lock_guard lock(cpu->mutex);
        implementation.rebuilding = false;
        if (!status)
        {
            cpu->GraphicsFailedLocked(status);
        }
        else if (cpu->snapshot.stopRequested)
        {
            status = CaptureStatus::Failure(CaptureError::InvalidConfiguration, CaptureStage::Consumer);
        }
        else
        {
            implementation.staging = std::move(replacement);
            implementation.device = device;
            implementation.environment = environment;
            implementation.roiSize = {static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)};
            implementation.ownerThread = std::this_thread::get_id();
            implementation.sourceGeneration = 0;
            implementation.lastSubmittedObservation = 0;
            cpu->snapshot.residentStagingBytes = residentBytes.Value();
            pbprotocol::SaturatingIncrementUnsigned(cpu->snapshot.domainStarts);
            cpu->ChangeDomainLocked(domain);
            status = cpu->snapshot.error;
        }
    }
    cpu->wake.notify_one();
    return status;
}

void DiagnosticCpuReadback::DomainInvalidated(const ScreenCaptureDomain& domain) noexcept
{
    const auto& cpu = implementation_->cpu;
    {
        const std::lock_guard lock(cpu->mutex);
        if (!cpu->activeDomain || *cpu->activeDomain != domain)
        {
            return;
        }
        pbprotocol::SaturatingIncrementUnsigned(cpu->snapshot.invalidations);
        cpu->ChangeDomainLocked(std::nullopt);
    }
    cpu->wake.notify_one();
}

CaptureStatus DiagnosticCpuReadback::Submit(const ScreenCaptureFrame& frame, ID3D11DeviceContext* const context)
{
    auto& implementation = *implementation_;
    const auto& cpu = implementation.cpu;
    const std::lock_guard lock(cpu->mutex);
    if (!cpu->snapshot.error)
    {
        return cpu->snapshot.error;
    }
    if (cpu->snapshot.stopRequested)
    {
        cpu->DropLocked(DiagnosticReadbackDropReason::Stopping);
        // Diagnostic cancellation is not a capture failure. Register a skipped
        // callback token without querying the caller's texture or context, so
        // the normal consumer marker can still complete and retire this slot.
        if (frame.metadata.slotIndex < cpu->config.stagingTextureCount)
        {
            auto& slot = implementation.staging[frame.metadata.slotIndex];
            if (slot.phase == StagingPhase::Idle)
            {
                slot.metadata = frame.metadata;
                slot.revision = cpu->revision;
                slot.phase = StagingPhase::Skipped;
                cpu->snapshot.pendingStagingFrames++;
                cpu->snapshot.stagingHighWater = std::max(cpu->snapshot.stagingHighWater, cpu->snapshot.pendingStagingFrames);
            }
        }
        return {};
    }
    if (!cpu->activeDomain || *cpu->activeDomain != frame.metadata.domain)
    {
        cpu->DropLocked(DiagnosticReadbackDropReason::InactiveDomain);
        return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Consumer);
    }
    const auto validation = implementation.ValidateTextureLocked(frame, context);
    if (!validation)
    {
        return validation;
    }
    auto& slot = implementation.staging[frame.metadata.slotIndex];
    if (slot.phase != StagingPhase::Idle || frame.metadata.slotGeneration <= slot.lastGeneration)
    {
        return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Consumer);
    }
    slot.metadata = frame.metadata;
    slot.revision = cpu->revision;
    slot.lastGeneration = frame.metadata.slotGeneration;
    slot.phase = StagingPhase::Skipped;
    cpu->snapshot.pendingStagingFrames++;
    cpu->snapshot.stagingHighWater = std::max(cpu->snapshot.stagingHighWater, cpu->snapshot.pendingStagingFrames);
    implementation.sourceGeneration = frame.metadata.sourceGeneration;
    if (frame.metadata.captureObservation <= implementation.lastSubmittedObservation)
    {
        cpu->DropLocked(DiagnosticReadbackDropReason::StaleObservation);
        return {};
    }
    implementation.lastSubmittedObservation = frame.metadata.captureObservation;
    if (!cpu->FreshLocked(frame.metadata.timestamp.monotonic100ns, DiagnosticReadbackDropReason::ExpiredBeforeReadback, slot.submitted100ns))
    {
        return {};
    }
    slot.phase = StagingPhase::Copied;
    // Record the lease before the first GPU command. Even a caller-side marker
    // failure must later cancel this exact token without mapping or slot reuse.
    context->CopyResource(slot.texture.Get(), frame.texture);
    pbprotocol::SaturatingIncrementUnsigned(cpu->snapshot.submittedCopies);
    return {};
}

CaptureStatus DiagnosticCpuReadback::Completed(const ScreenCaptureFrameMetadata& metadata, ID3D11DeviceContext* const context, const bool cancelled)
{
    auto& implementation = *implementation_;
    const auto& cpu = implementation.cpu;
    ComPtr<ID3D11Texture2D> stagingTexture;
    std::size_t bufferIndex = noCpuBuffer;
    std::uint64_t completionRevision = 0;
    std::int64_t submitted100ns = 0;
    ScreenCaptureFrameMetadata storedMetadata;
    std::size_t rowBytes = 0;
    std::size_t usedBytes = 0;
    {
        const std::lock_guard lock(cpu->mutex);
        if (metadata.slotIndex >= cpu->config.stagingTextureCount || implementation.staging[metadata.slotIndex].phase == StagingPhase::Idle ||
            !SameFrameKey(implementation.staging[metadata.slotIndex].metadata, metadata))
        {
            cpu->DropLocked(DiagnosticReadbackDropReason::StaleCompletion);
            return {};
        }
        auto& slot = implementation.staging[metadata.slotIndex];
        if (slot.phase == StagingPhase::Completing)
        {
            return CaptureStatus::Failure(CaptureError::InternalError, CaptureStage::Consumer);
        }
        // This path may run on OS deferred cleanup. It does not even inspect
        // context, device, texture descriptions, or pixel memory.
        if (cancelled || slot.phase == StagingPhase::Skipped)
        {
            if (cancelled && slot.phase != StagingPhase::Skipped)
            {
                cpu->DropLocked(DiagnosticReadbackDropReason::CancelledCompletion);
            }
            implementation.FinishStageLocked(metadata.slotIndex);
            return {};
        }
        if (!cpu->CurrentDomainLocked(slot.metadata, slot.revision))
        {
            cpu->DropLocked(cpu->snapshot.stopRequested ? DiagnosticReadbackDropReason::Stopping : DiagnosticReadbackDropReason::InactiveDomain);
            implementation.FinishStageLocked(metadata.slotIndex);
            return {};
        }
        if (std::this_thread::get_id() != implementation.ownerThread || context == nullptr)
        {
            implementation.FinishStageLocked(metadata.slotIndex);
            return CaptureStatus::Failure(CaptureError::WrongThread, CaptureStage::Consumer);
        }
        ComPtr<ID3D11Device> contextDevice;
        context->GetDevice(&contextDevice);
        if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE || contextDevice.Get() != implementation.device.Get())
        {
            implementation.FinishStageLocked(metadata.slotIndex);
            return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Consumer);
        }
        std::int64_t now100ns = 0;
        if (!cpu->FreshLocked(slot.metadata.timestamp.monotonic100ns, DiagnosticReadbackDropReason::ExpiredBeforeReadback, now100ns))
        {
            implementation.FinishStageLocked(metadata.slotIndex);
            return {};
        }
        for (std::size_t index = 0; index < cpu->buffers.size(); index++)
        {
            if (cpu->buffers[index].state == CpuBufferState::Free)
            {
                bufferIndex = index;
                break;
            }
        }
        if (bufferIndex == noCpuBuffer)
        {
            cpu->DropLocked(DiagnosticReadbackDropReason::NoCpuBuffer);
            implementation.FinishStageLocked(metadata.slotIndex);
            return {};
        }
        const auto rowByteCount = pbprotocol::CheckedMultiplyUint64(static_cast<std::uint64_t>(slot.metadata.roiSize.width), PixelBytes(slot.metadata.pixelFormat));
        const auto byteCount = rowByteCount ? pbprotocol::CheckedMultiplyUint64(rowByteCount.Value(), static_cast<std::uint64_t>(slot.metadata.roiSize.height)) : rowByteCount;
        if (!rowByteCount || !byteCount || byteCount.Value() > cpu->snapshot.reservation.bytesPerCpuBuffer || !pbprotocol::CheckedUint64ToSize(byteCount.Value()))
        {
            implementation.FinishStageLocked(metadata.slotIndex);
            return CaptureStatus::Failure(CaptureError::ResourceLimit, CaptureStage::Consumer);
        }
        rowBytes = static_cast<std::size_t>(rowByteCount.Value());
        usedBytes = static_cast<std::size_t>(byteCount.Value());
        cpu->buffers[bufferIndex].state = CpuBufferState::Filling;
        cpu->UpdateUsageLocked();
        completionRevision = slot.revision;
        submitted100ns = slot.submitted100ns;
        storedMetadata = slot.metadata;
        stagingTexture = slot.texture;
        slot.phase = StagingPhase::Completing;
        pbprotocol::SaturatingIncrementUnsigned(cpu->snapshot.mapCalls);
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    bool mapUsedBlockingRetry = false;
    const HRESULT mapResult = implementation.graphics->MapStaging(context, stagingTexture.Get(), mapped, mapUsedBlockingRetry);
    CaptureStatus status;
    if (SUCCEEDED(mapResult))
    {
        const MappedResource unmap{context, stagingTexture.Get()};
        const auto lastRow = pbprotocol::CheckedMultiplyUint64(mapped.RowPitch, static_cast<std::uint64_t>(storedMetadata.roiSize.height - 1));
        const auto extent = lastRow ? pbprotocol::CheckedAddUint64(lastRow.Value(), rowBytes) : lastRow;
        if (mapped.pData == nullptr || mapped.RowPitch < rowBytes || !extent || !pbprotocol::CheckedUint64ToSize(extent.Value()) ||
            extent.Value() > std::numeric_limits<std::uintptr_t>::max() - reinterpret_cast<std::uintptr_t>(mapped.pData))
        {
            status = CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Consumer);
        }
        else
        {
            auto* const destination = cpu->buffers[bufferIndex].pixels.get();
            const auto* const source = static_cast<const std::byte*>(mapped.pData);
            for (std::size_t row = 0; row < static_cast<std::size_t>(storedMetadata.roiSize.height); row++)
            {
                std::memcpy(destination + row * rowBytes, source + row * mapped.RowPitch, rowBytes);
            }
        }
    }
    else if (mapResult != DXGI_ERROR_WAS_STILL_DRAWING)
    {
        status = detail::FromHresult(mapResult, CaptureStage::Consumer);
    }

    {
        const std::lock_guard lock(cpu->mutex);
        implementation.FinishStageLocked(metadata.slotIndex);
        auto& buffer = cpu->buffers[bufferIndex];
        if (!status || mapResult == DXGI_ERROR_WAS_STILL_DRAWING)
        {
            buffer.state = CpuBufferState::Free;
            cpu->DropLocked(status ? DiagnosticReadbackDropReason::MapNotReady : DiagnosticReadbackDropReason::MapFailure);
            if (!status)
            {
                cpu->GraphicsFailedLocked(status);
            }
        }
        else
        {
            pbprotocol::SaturatingIncrementUnsigned(cpu->snapshot.mappedFrames);
            if (mapUsedBlockingRetry)
            {
                pbprotocol::SaturatingIncrementUnsigned(cpu->snapshot.mapBlockingRetries);
            }
            cpu->snapshot.readbackBytes = pbprotocol::SaturatingAddUnsigned(cpu->snapshot.readbackBytes, static_cast<std::uint64_t>(usedBytes));
            cpu->snapshot.lastMappedRowPitch = mapped.RowPitch;
            std::int64_t now100ns = 0;
            const bool fresh = cpu->FreshLocked(storedMetadata.timestamp.monotonic100ns, DiagnosticReadbackDropReason::ExpiredAfterReadback, now100ns);
            if (now100ns >= submitted100ns)
            {
                cpu->snapshot.readbackLatencyHighWater100ns = std::max(cpu->snapshot.readbackLatencyHighWater100ns, static_cast<std::uint64_t>(now100ns - submitted100ns));
            }
            if (!cpu->CurrentDomainLocked(storedMetadata, completionRevision))
            {
                cpu->DropLocked(cpu->snapshot.stopRequested ? DiagnosticReadbackDropReason::Stopping : DiagnosticReadbackDropReason::InactiveDomain);
                buffer.state = CpuBufferState::Free;
            }
            else if (!fresh)
            {
                buffer.state = CpuBufferState::Free;
            }
            else
            {
                cpu->ClearQueuedLocked(DiagnosticReadbackDropReason::QueueReplaced);
                buffer.metadata = storedMetadata;
                buffer.rowPitch = rowBytes;
                buffer.usedBytes = usedBytes;
                buffer.revision = completionRevision;
                buffer.state = CpuBufferState::Queued;
                cpu->queuedIndex = bufferIndex;
            }
        }
        cpu->UpdateUsageLocked();
    }
    cpu->wake.notify_one();
    return status;
}

void DiagnosticCpuReadback::Erased(const CaptureErasure&) noexcept
{
    const auto& cpu = implementation_->cpu;
    const std::lock_guard lock(cpu->mutex);
    pbprotocol::SaturatingIncrementUnsigned(cpu->snapshot.captureErasures);
}

DiagnosticReadbackSnapshot DiagnosticCpuReadback::GetSnapshot() const noexcept
{
    const auto& cpu = implementation_->cpu;
    const std::lock_guard lock(cpu->mutex);
    return cpu->snapshot;
}

void DiagnosticCpuReadback::RequestStop() noexcept
{
    const auto& cpu = implementation_->cpu;
    {
        const std::lock_guard lock(cpu->mutex);
        cpu->StopLocked();
    }
    cpu->wake.notify_one();
}

CaptureStatus DiagnosticCpuReadback::Stop(const std::uint32_t maximumWaitMilliseconds) noexcept
{
    if (maximumWaitMilliseconds > 60000)
    {
        return CaptureStatus::Failure(CaptureError::InvalidConfiguration, CaptureStage::Shutdown);
    }
    RequestStop();
    const HANDLE worker = implementation_->worker.value;
    if (worker == nullptr)
    {
        return GetSnapshot().error;
    }
    if (GetCurrentThreadId() == implementation_->workerId && WaitForSingleObject(worker, 0) != WAIT_OBJECT_0)
    {
        return CaptureStatus::Failure(CaptureError::WrongThread, CaptureStage::Shutdown);
    }
    const DWORD result = WaitForSingleObject(worker, maximumWaitMilliseconds);
    if (result == WAIT_TIMEOUT)
    {
        return CaptureStatus::Failure(CaptureError::Timeout, CaptureStage::Shutdown);
    }
    if (result != WAIT_OBJECT_0)
    {
        return CaptureStatus::Failure(CaptureError::NativeFailure, CaptureStage::Shutdown, static_cast<std::int32_t>(GetLastError()));
    }
    return GetSnapshot().error;
}

const char* GetDiagnosticReadbackDropName(const DiagnosticReadbackDropReason reason) noexcept
{
    switch (reason)
    {
    case DiagnosticReadbackDropReason::None: return "None";
    case DiagnosticReadbackDropReason::InactiveDomain: return "InactiveDomain";
    case DiagnosticReadbackDropReason::StaleObservation: return "StaleObservation";
    case DiagnosticReadbackDropReason::ExpiredBeforeReadback: return "ExpiredBeforeReadback";
    case DiagnosticReadbackDropReason::ExpiredAfterReadback: return "ExpiredAfterReadback";
    case DiagnosticReadbackDropReason::ExpiredBeforeAnalyze: return "ExpiredBeforeAnalyze";
    case DiagnosticReadbackDropReason::ExpiredBeforeCommit: return "ExpiredBeforeCommit";
    case DiagnosticReadbackDropReason::InvalidTimestamp: return "InvalidTimestamp";
    case DiagnosticReadbackDropReason::NoCpuBuffer: return "NoCpuBuffer";
    case DiagnosticReadbackDropReason::QueueReplaced: return "QueueReplaced";
    case DiagnosticReadbackDropReason::CancelledCompletion: return "CancelledCompletion";
    case DiagnosticReadbackDropReason::StaleCompletion: return "StaleCompletion";
    case DiagnosticReadbackDropReason::MapNotReady: return "MapNotReady";
    case DiagnosticReadbackDropReason::MapFailure: return "MapFailure";
    case DiagnosticReadbackDropReason::ProcessorFailure: return "ProcessorFailure";
    case DiagnosticReadbackDropReason::Stopping: return "Stopping";
    case DiagnosticReadbackDropReason::Count: break;
    }
    return "Unknown";
}

} // namespace pbcapturenormalize
