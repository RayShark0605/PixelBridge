#include "pbcapturenormalize/diagnostic_readback.h"
#include "pbdesktoplevels/reference_channel.h"
#include "pbdemodd3d11/capture_demodulator.h"
#include "pbmodulation/remote_visual_low_fps.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "../../libs/PBCaptureNormalize/src/d3d_roi_ring.h"
#include "../../libs/PBCaptureNormalize/src/native_support.h"
#include "../../libs/PBScreenCaptureWgc/src/capture_internal.h"
#include "../../libs/PBScreenCaptureDxgi/src/capture_internal.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

using namespace pbcapturenormalize;
using namespace pbcapturenormalize::detail;
using Microsoft::WRL::ComPtr;

namespace
{

constexpr CaptureSize monitorSize{32, 24};
constexpr CaptureSize roiSize{9, 6};
constexpr std::size_t roiBytes = 9 * 6 * 4;

template<typename Predicate>
bool Await(Predicate predicate, const std::chrono::milliseconds timeout = std::chrono::seconds(5))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
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

struct CpuResult
{
    ScreenCaptureFrameMetadata metadata;
    std::array<std::byte, roiBytes> pixels{};
    std::size_t rowPitch = 0;
};

struct CpuObservations
{
    std::array<CpuResult, 3> analyzed;
    std::array<CpuResult, 2> committed;
    std::uint32_t analyzes = 0;
    std::uint32_t commits = 0;
    std::uint32_t discards = 0;
    std::uint32_t resets = 0;
    std::optional<ScreenCaptureDomain> domain;
    DWORD threadId = 0;
    bool blocked = false;
    bool callbackViolation = false;
    bool inputChanged = false;
    bool waitExpired = false;
};

struct PipelineControl
{
    CpuObservations GetCpu() const
    {
        const std::lock_guard lock(mutex);
        return cpu;
    }
    void ReleaseCpu()
    {
        {
            const std::lock_guard lock(mutex);
            released = true;
        }
        wake.notify_all();
    }
    void CheckCpuThreadLocked()
    {
        const DWORD current = GetCurrentThreadId();
        if (cpu.threadId == 0)
        {
            cpu.threadId = current;
        }
        cpu.callbackViolation = cpu.callbackViolation || cpu.threadId != current || current == ownerThread.load();
    }

    std::atomic<std::uint32_t> requestedFrames{0};
    std::atomic<bool> injectOldEpoch{false};
    std::atomic<std::uint32_t> madeLeases{0};
    std::atomic<std::uint32_t> textureReads{0};
    std::array<std::atomic<std::uint32_t>, 4> closes{};
    std::atomic<std::uint32_t> copies{0};
    std::atomic<std::uint32_t> consumes{0};
    std::atomic<std::uint32_t> completions{0};
    std::atomic<std::uint32_t> polls{0};
    std::atomic<std::uint32_t> recreates{0};
    std::atomic<std::uint32_t> shutdowns{0};
    std::atomic<DWORD> ownerThread{0};
    std::atomic<bool> wrongOwner{false};
    mutable std::mutex mutex;
    std::condition_variable wake;
    CpuObservations cpu;
    bool released = false;
};

struct PipelineFrameSource
{
    CaptureSize contentSize;
    CaptureSize sourceSize;
    DXGI_MODE_ROTATION sourceRotation = DXGI_MODE_ROTATION_IDENTITY;
    std::vector<std::byte> pixels;
};

class PipelineProcessor final : public CpuFrameProcessor
{
public:
    explicit PipelineProcessor(std::shared_ptr<PipelineControl> control) : control_(std::move(control))
    {
    }
    void Reset(std::optional<ScreenCaptureDomain> domain) noexcept override
    {
        const std::lock_guard lock(control_->mutex);
        control_->CheckCpuThreadLocked();
        control_->cpu.domain = std::move(domain);
        control_->cpu.resets++;
        candidateValid_ = false;
    }
    CaptureStatus Analyze(const ScreenCaptureFrameMetadata& metadata, const std::span<const std::byte> pixels, const std::size_t rowPitch) override
    {
        if (pixels.size() != roiBytes || rowPitch != 9 * 4)
        {
            return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Consumer);
        }
        candidate_.metadata = metadata;
        candidate_.rowPitch = rowPitch;
        std::copy(pixels.begin(), pixels.end(), candidate_.pixels.begin());
        std::unique_lock lock(control_->mutex);
        control_->CheckCpuThreadLocked();
        if (control_->cpu.analyzes == control_->cpu.analyzed.size())
        {
            return CaptureStatus::Failure(CaptureError::ResourceLimit, CaptureStage::Consumer);
        }
        control_->cpu.analyzed[control_->cpu.analyzes] = candidate_;
        control_->cpu.analyzes++;
        if (metadata.domain.captureEpoch == 1 && metadata.captureObservation == 2)
        {
            control_->cpu.blocked = true;
            // A bounded, test-only slow CPU job; the GPU owner must still drain
            // and start epoch 2 while this old input buffer remains borrowed.
            if (!control_->wake.wait_for(lock, std::chrono::seconds(10), [&] { return control_->released; }))
            {
                control_->cpu.waitExpired = true;
                return CaptureStatus::Failure(CaptureError::Timeout, CaptureStage::Consumer);
            }
        }
        control_->cpu.inputChanged = control_->cpu.inputChanged || !std::equal(pixels.begin(), pixels.end(), candidate_.pixels.begin());
        candidateValid_ = true;
        return {};
    }
    void Commit(const ScreenCaptureFrameMetadata& metadata) noexcept override
    {
        const std::lock_guard lock(control_->mutex);
        control_->CheckCpuThreadLocked();
        auto& observed = control_->cpu;
        if (!candidateValid_ || observed.domain != metadata.domain || candidate_.metadata.domain != metadata.domain ||
            candidate_.metadata.captureObservation != metadata.captureObservation || observed.commits == observed.committed.size())
        {
            observed.callbackViolation = true;
            return;
        }
        observed.committed[observed.commits] = candidate_;
        observed.commits++;
        candidateValid_ = false;
    }
    void Discard() noexcept override
    {
        const std::lock_guard lock(control_->mutex);
        control_->CheckCpuThreadLocked();
        control_->cpu.discards++;
        candidateValid_ = false;
    }

private:
    const std::shared_ptr<PipelineControl> control_;
    CpuResult candidate_;
    bool candidateValid_ = false;
};

struct TextureLease
{
    ComPtr<ID3D11Texture2D> texture;
    std::shared_ptr<PipelineControl> control;
    std::uint32_t seed = 0;
};

HRESULT CloseTexture(void* const pointer) noexcept
{
    const std::unique_ptr<TextureLease> lease(static_cast<TextureLease*>(pointer));
    lease->control->closes[lease->seed]++;
    return S_OK;
}

HRESULT ReadTexture(void* const pointer, ID3D11Texture2D** const texture) noexcept
{
    auto& lease = *static_cast<TextureLease*>(pointer);
    lease.control->textureReads++;
    lease.control->wrongOwner = lease.control->wrongOwner || GetCurrentThreadId() != lease.control->ownerThread.load();
    return lease.texture.CopyTo(texture);
}

// Only OS acquisition is injected. Neither marker, crop/rotation, normalized
// admission, staging copy, Map, nor CPU dispatch is replaced by a fake result.
class PipelineBackend final : public CaptureBackend
{
public:
    PipelineBackend(std::shared_ptr<PipelineControl> control, const CaptureBackendKind kind,
        std::shared_ptr<const PipelineFrameSource> source = {})
        : control_(std::move(control)), kind_(kind), source_(std::move(source))
    {
    }
    CaptureBackendKind Kind() const noexcept override
    {
        return kind_;
    }
    CaptureStatus Initialize(const CaptureConfig& config, std::shared_ptr<FrameInbox> inbox) noexcept override
    {
        control_->ownerThread = GetCurrentThreadId();
        config_ = config;
        inbox_ = std::move(inbox);
        D3D_FEATURE_LEVEL feature{};
        auto status = FromHresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_DEBUG,
                                                  nullptr, 0, D3D11_SDK_VERSION, &device_, &feature, &context_), CaptureStage::Device);
        if (!status)
        {
            return status;
        }
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC description{};
        HRESULT result = device_.As(&dxgiDevice);
        if (SUCCEEDED(result))
        {
            result = dxgiDevice->GetAdapter(&adapter);
        }
        if (SUCCEEDED(result))
        {
            result = adapter->GetDesc(&description);
        }
        if (FAILED(result))
        {
            return FromHresult(result, CaptureStage::Adapter);
        }
        environment_.region = config.region;
        environment_.contentSize = source_ ? source_->contentSize : monitorSize;
        environment_.sourceSize = source_ ? source_->sourceSize :
            (kind_ == CaptureBackendKind::Dxgi ? CaptureSize{24, 32} : monitorSize);
        environment_.sourceRotation = source_ ? source_->sourceRotation :
            (kind_ == CaptureBackendKind::Dxgi ? DXGI_MODE_ROTATION_ROTATE90 : DXGI_MODE_ROTATION_IDENTITY);
        environment_.backendKind = kind_;
        environment_.pixelFormat = config.pixelFormat;
        environment_.adapterLuid = description.AdapterLuid;
        environment_.bitsPerColor = 8;
        if (environment_.contentSize.width <= 0 || environment_.contentSize.height <= 0 || environment_.sourceSize.width <= 0 ||
            environment_.sourceSize.height <= 0)
        {
            return CaptureStatus::Failure(CaptureError::InvalidConfiguration, CaptureStage::Surface);
        }
        const auto sourcePixels = static_cast<std::uint64_t>(environment_.sourceSize.width) *
            static_cast<std::uint64_t>(environment_.sourceSize.height);
        if (source_ && (sourcePixels > std::numeric_limits<std::size_t>::max() / 4 ||
            source_->pixels.size() != static_cast<std::size_t>(sourcePixels) * 4))
        {
            return CaptureStatus::Failure(CaptureError::InvalidConfiguration, CaptureStage::Surface);
        }
        status = retirement_.Initialize(device_.Get(), context_.Get());
        return status ? ring_.Initialize(device_.Get(), context_.Get(), config_, environment_, kind_ == CaptureBackendKind::Dxgi) : status;
    }
    CaptureStatus Start(const std::uint64_t epoch) noexcept override
    {
        epoch_ = epoch;
        active_ = true;
        return {};
    }
    CaptureStatus Pause() noexcept override
    {
        active_ = false;
        return {};
    }
    bool CallbacksIdle() const noexcept override
    {
        return true;
    }
    CaptureStatus CheckEnvironment(bool& changed) noexcept override
    {
        changed = false;
        return {};
    }
    CaptureStatus Recreate(const CaptureSize requestedSize) noexcept override
    {
        CheckOwner();
        if (active_ || requestedSize != environment_.contentSize || inbox_->counters->live != 0)
        {
            return CaptureStatus::Failure(CaptureError::InternalError, CaptureStage::Recreate);
        }
        control_->recreates++;
        return ring_.Recreate(config_, environment_);
    }
    CaptureEnvironment GetEnvironment() const noexcept override
    {
        return environment_;
    }
    CaptureCapabilities GetCapabilities() const noexcept override
    {
        CaptureCapabilities capabilities;
        capabilities.cursorExcluded = kind_ == CaptureBackendKind::Wgc;
        capabilities.fenceRetirement = ring_.UsesFence();
        return capabilities;
    }
    CaptureStatus NotifyEpoch(RawRoiConsumer& consumer, const std::uint64_t epoch) noexcept override
    {
        return consumer.EpochStarted(epoch, environment_, device_.Get());
    }
    CaptureStatus Acquire() noexcept override
    {
        CheckOwner();
        if (!active_)
        {
            return {};
        }
        if (control_->injectOldEpoch.exchange(false))
        {
            return QueueFrame(0, 1);
        }
        if (emitted_ >= control_->requestedFrames.load())
        {
            return {};
        }
        if (emitted_ >= 3)
        {
            return CaptureStatus::Failure(CaptureError::ResourceLimit, CaptureStage::Callback);
        }
        emitted_++;
        return QueueFrame(emitted_, epoch_);
    }
    CaptureStatus Copy(const FrameLease& frame, const std::size_t slot, bool& submitted) noexcept override
    {
        CheckOwner();
        control_->copies++;
        return ring_.Copy(frame, slot, submitted);
    }
    CaptureStatus Consume(RawRoiConsumer& consumer, const RawRoiFrameMetadata& metadata, const std::size_t slot) noexcept override
    {
        CheckOwner();
        control_->consumes++;
        return ring_.Consume(consumer, metadata, slot);
    }
    CaptureConsumerCompletion Complete(RawRoiConsumer& consumer, const RawRoiFrameMetadata& metadata, std::size_t, const bool cancelled) noexcept override
    {
        if (!cancelled)
        {
            CheckOwner();
        }
        control_->completions++;
        return ring_.Complete(consumer, metadata, cancelled);
    }
    CompletionResult Poll(const std::size_t slot) noexcept override
    {
        CheckOwner();
        control_->polls++;
        return ring_.Poll(slot);
    }
    bool DeviceRemoved() const noexcept override
    {
        return ring_.DeviceRemoved();
    }
    std::int32_t DeviceRemovalReason() const noexcept override
    {
        return device_ ? device_->GetDeviceRemovedReason() : S_OK;
    }
    CaptureStatus Shutdown() noexcept override
    {
        const auto status = ring_.CheckDebug();
        ring_.Reset();
        retirement_.Reset();
        context_.Reset();
        device_.Reset();
        control_->shutdowns++;
        return status;
    }
    void DeferShutdown(std::shared_ptr<DeferredCleanup> owner) noexcept override
    {
        // Unexpected WARP timeout must not turn a failed test into an early
        // release of the real source leases. No visible HWND or producer exists.
        retirement_.Defer(std::move(owner));
    }

private:
    void CheckOwner() noexcept
    {
        control_->wrongOwner = control_->wrongOwner || GetCurrentThreadId() != control_->ownerThread.load();
    }
    CaptureStatus QueueFrame(const std::uint32_t seed, const std::uint64_t epoch) noexcept
    {
        auto source = std::unique_ptr<TextureLease>(new (std::nothrow) TextureLease{{}, control_, seed});
        if (!source)
        {
            return CaptureStatus::Failure(CaptureError::OutOfMemory, CaptureStage::Surface);
        }
        const UINT width = static_cast<UINT>(environment_.sourceSize.width);
        const UINT height = static_cast<UINT>(environment_.sourceSize.height);
        std::array<std::byte, 32 * 24 * 4> generatedPixels{};
        if (!source_)
        {
            // Fixed full-surface bytes, with DXGI's raw axes rotated relative to
            // the physical desktop. The oracle below never reads the crop box or source.
            for (UINT sourceY = 0; sourceY < height; sourceY++)
            {
                for (UINT sourceX = 0; sourceX < width; sourceX++)
                {
                    const UINT visualX = kind_ == CaptureBackendKind::Dxgi ? 31u - sourceY : sourceX;
                    const UINT visualY = kind_ == CaptureBackendKind::Dxgi ? sourceX : sourceY;
                    for (UINT channel = 0; channel < 4; channel++)
                    {
                        const UINT visualByte = (visualY * 32u + visualX) * 4u + channel;
                        generatedPixels[(static_cast<std::size_t>(sourceY) * width + sourceX) * 4u + channel] =
                            static_cast<std::byte>((visualByte * 73u + seed * 19u) & 255u);
                    }
                }
            }
        }
        D3D11_TEXTURE2D_DESC description{};
        description.Width = width;
        description.Height = height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        const std::byte* const initialPixels = source_ ? source_->pixels.data() : generatedPixels.data();
        const D3D11_SUBRESOURCE_DATA initial{initialPixels, width * 4u, 0};
        const auto created = FromHresult(device_->CreateTexture2D(&description, &initial, &source->texture), CaptureStage::Surface);
        if (!created)
        {
            return created;
        }
        LARGE_INTEGER counter{};
        LARGE_INTEGER frequency{};
        std::int64_t now100ns = 0;
        if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&counter) ||
            !ConvertQpcTo100ns(counter.QuadPart, frequency.QuadPart, now100ns))
        {
            return CaptureStatus::Failure(CaptureError::InternalError, CaptureStage::Callback);
        }
        FrameLease frame(source.release(), CloseTexture, ReadTexture, inbox_->counters, environment_.contentSize, now100ns, epoch);
        frame.cursorState = kind_ == CaptureBackendKind::Wgc ? CursorState::Excluded : CursorState::SeparatePointer;
        frame.pointer.positionKnown = kind_ == CaptureBackendKind::Dxgi;
        frame.pointer.separateVisible = kind_ == CaptureBackendKind::Dxgi;
        frame.pointer.physicalLeft = -20;
        frame.pointer.physicalTop = -20;
        frame.timestampDomain = kind_ == CaptureBackendKind::Wgc ? CaptureTimestampDomain::WgcSystemRelative100ns : CaptureTimestampDomain::DxgiQpcTicks;
        frame.rawTimestamp = kind_ == CaptureBackendKind::Wgc ? now100ns : counter.QuadPart;
        frame.rawFrequency = kind_ == CaptureBackendKind::Wgc ? 10000000 : frequency.QuadPart;
        control_->madeLeases++;
        inbox_->Push(std::move(frame));
        return {};
    }

    const std::shared_ptr<PipelineControl> control_;
    const CaptureBackendKind kind_;
    const std::shared_ptr<const PipelineFrameSource> source_;
    CaptureConfig config_;
    CaptureEnvironment environment_;
    std::shared_ptr<FrameInbox> inbox_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    D3dRoiRing ring_;
    DeferredGpuRetirement retirement_;
    std::uint64_t epoch_ = 0;
    std::uint32_t emitted_ = 0;
    bool active_ = false;
};

std::array<std::byte, pbprotocol::kBootstrapRecordBytes> MakeLowFpsCaptureRecord()
{
    pbprotocol::BootstrapRecord record;
    record.protocolVersion = pbprotocol::GetProtocolVersion();
    record.visualLayoutVersion = pbmodulation::kRemoteVisualLowFpsLayoutVersion;
    record.visualProfileId = pbmodulation::kRemoteVisualLowFpsProfileId;
    record.sessionTag.value = 0xA91D3C786E425BF0ULL;
    record.frameSequence = 901;
    std::array<std::byte, pbprotocol::kBootstrapRecordBytes> serialized{};
    REQUIRE(pbprotocol::SerializeBootstrapRecord(record, serialized));
    return serialized;
}

std::shared_ptr<PipelineFrameSource> MakeLowFpsFrameSource(const CaptureBackendKind kind,
    const std::span<const std::byte> uprightPixels)
{
    constexpr CaptureSize contentSize{static_cast<std::int32_t>(pbmodulation::kLocalDesktopCanvasWidth),
        static_cast<std::int32_t>(pbmodulation::kLocalDesktopCanvasHeight)};
    REQUIRE(uprightPixels.size() == pbmodulation::kLocalDesktopFrameBgraBytes);
    const auto source = std::make_shared<PipelineFrameSource>();
    source->contentSize = contentSize;
    if (kind == CaptureBackendKind::Wgc)
    {
        source->sourceSize = contentSize;
        source->sourceRotation = DXGI_MODE_ROTATION_IDENTITY;
        source->pixels.assign(uprightPixels.begin(), uprightPixels.end());
        return source;
    }

    source->sourceSize = {contentSize.height, contentSize.width};
    source->sourceRotation = DXGI_MODE_ROTATION_ROTATE90;
    source->pixels.resize(uprightPixels.size());
    for (std::uint32_t sourceY = 0; sourceY < static_cast<std::uint32_t>(source->sourceSize.height); sourceY++)
    {
        for (std::uint32_t sourceX = 0; sourceX < static_cast<std::uint32_t>(source->sourceSize.width); sourceX++)
        {
            const std::uint32_t visualX = pbmodulation::kLocalDesktopCanvasWidth - 1 - sourceY;
            const std::uint32_t visualY = sourceX;
            const std::size_t sourceOffset = (static_cast<std::size_t>(sourceY) * source->sourceSize.width + sourceX) * 4;
            const std::size_t visualOffset = (static_cast<std::size_t>(visualY) * contentSize.width + visualX) * 4;
            std::copy_n(uprightPixels.begin() + visualOffset, 4, source->pixels.begin() + sourceOffset);
        }
    }
    return source;
}

struct StagedGpuObservations
{
    std::atomic<std::uint32_t> domainStarts{0};
    std::atomic<std::uint32_t> invalidations{0};
    std::atomic<std::uint32_t> submits{0};
    std::atomic<std::uint32_t> completionCallbacks{0};
    std::atomic<std::uint32_t> mappedStages{0};
    std::atomic<std::uint32_t> terminalCancellations{0};
    std::atomic<bool> wrongTexture{false};
    std::atomic<bool> cancelledWithGpuObjects{false};
    std::array<std::byte, roiBytes> firstPixels{};
    std::array<std::byte, roiBytes> secondPixels{};
};

class StagedGpuConsumer final : public ScreenCaptureConsumer
{
public:
    explicit StagedGpuConsumer(std::shared_ptr<StagedGpuObservations> observations, const bool requestSecondContinuation = false)
        : observations_(std::move(observations)), requestSecondContinuation_(requestSecondContinuation)
    {
    }

    std::uint64_t ReservedBytes() const noexcept override
    {
        return 2 * roiBytes;
    }

    CaptureStatus DomainStarted(const ScreenCaptureDomain& domain, const CaptureEnvironment& environment, ID3D11Device* device) override
    {
        const auto& rectangle = environment.region.physicalRect;
        if (active_ || phase_ != 0 || device == nullptr || rectangle.right - rectangle.left != roiSize.width ||
            rectangle.bottom - rectangle.top != roiSize.height || environment.pixelFormat != DXGI_FORMAT_B8G8R8A8_UNORM)
        {
            return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Consumer);
        }
        D3D11_TEXTURE2D_DESC description{};
        description.Width = static_cast<UINT>(roiSize.width);
        description.Height = static_cast<UINT>(roiSize.height);
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_STAGING;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        std::array<ComPtr<ID3D11Texture2D>, 2> staging;
        HRESULT result = device->CreateTexture2D(&description, nullptr, &staging[0]);
        if (SUCCEEDED(result))
        {
            result = device->CreateTexture2D(&description, nullptr, &staging[1]);
        }
        if (FAILED(result))
        {
            return FromHresult(result, CaptureStage::Consumer);
        }
        staging_ = std::move(staging);
        domain_ = domain;
        active_ = true;
        observations_->domainStarts++;
        return {};
    }

    void DomainInvalidated(const ScreenCaptureDomain& domain) noexcept override
    {
        if (active_ && domain == domain_)
        {
            active_ = false;
            observations_->invalidations++;
        }
    }

    CaptureStatus Submit(const ScreenCaptureFrame& frame, ID3D11DeviceContext* context) override
    {
        if (!active_ || phase_ != 0 || frame.metadata.domain != domain_ || frame.texture == nullptr || context == nullptr)
        {
            return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Consumer);
        }
        metadata_ = frame.metadata;
        borrowedTexture_ = frame.texture;
        context->CopyResource(staging_[0].Get(), frame.texture);
        phase_ = 1;
        observations_->submits++;
        return {};
    }

    CaptureConsumerCompletion CompleteStage(const ScreenCaptureFrameMetadata& metadata, ID3D11Texture2D* texture,
                                              ID3D11DeviceContext* context, const bool cancelled) override
    {
        observations_->completionCallbacks++;
        if (cancelled)
        {
            observations_->cancelledWithGpuObjects = texture != nullptr || context != nullptr;
            observations_->terminalCancellations++;
            phase_ = 0;
            borrowedTexture_ = nullptr;
            return {};
        }
        if (!active_ || phase_ == 0 || context == nullptr || texture == nullptr || texture != borrowedTexture_ || !SameFrame(metadata))
        {
            observations_->wrongTexture = texture != borrowedTexture_;
            return {CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Completion), false};
        }
        auto& output = phase_ == 1 ? observations_->firstPixels : observations_->secondPixels;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT result = context->Map(staging_[phase_ - 1].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (FAILED(result))
        {
            return {FromHresult(result, CaptureStage::Completion), false};
        }
        for (std::size_t row = 0; row < static_cast<std::size_t>(roiSize.height); row++)
        {
            const auto* const source = static_cast<const std::byte*>(mapped.pData) + row * mapped.RowPitch;
            std::copy_n(source, static_cast<std::size_t>(roiSize.width) * 4, output.data() + row * roiSize.width * 4);
        }
        context->Unmap(staging_[phase_ - 1].Get(), 0);
        observations_->mappedStages++;
        if (phase_ == 1)
        {
            context->CopyResource(staging_[1].Get(), texture);
            phase_ = 2;
            return {{}, true};
        }
        if (requestSecondContinuation_)
        {
            context->CopyResource(staging_[0].Get(), texture);
            phase_ = 3;
            return {{}, true};
        }
        phase_ = 0;
        borrowedTexture_ = nullptr;
        return {};
    }

private:
    bool SameFrame(const ScreenCaptureFrameMetadata& metadata) const noexcept
    {
        return metadata.domain == metadata_.domain && metadata.captureObservation == metadata_.captureObservation &&
               metadata.sourceGeneration == metadata_.sourceGeneration && metadata.slotIndex == metadata_.slotIndex &&
               metadata.slotGeneration == metadata_.slotGeneration;
    }

    const std::shared_ptr<StagedGpuObservations> observations_;
    std::array<ComPtr<ID3D11Texture2D>, 2> staging_;
    ScreenCaptureDomain domain_;
    ScreenCaptureFrameMetadata metadata_;
    ID3D11Texture2D* borrowedTexture_ = nullptr;
    std::uint8_t phase_ = 0;
    bool active_ = false;
    const bool requestSecondContinuation_;
};

struct ReleaseCpuOnExit
{
    std::shared_ptr<PipelineControl> control;
    ~ReleaseCpuOnExit()
    {
        control->ReleaseCpu();
    }
};

void RequirePixels(const CpuResult& result, const CaptureBackendKind kind, const std::uint32_t seed)
{
    REQUIRE(result.rowPitch == 36);
    REQUIRE(result.metadata.roiSize == roiSize);
    REQUIRE(result.metadata.physicalRoi.left == -43);
    REQUIRE(result.metadata.physicalRoi.top == -35);
    REQUIRE(result.metadata.physicalRoi.right == -34);
    REQUIRE(result.metadata.physicalRoi.bottom == -29);
    REQUIRE(result.metadata.sourceContentSize == monitorSize);
    REQUIRE(result.metadata.sourceExtent == (kind == CaptureBackendKind::Dxgi ? CaptureSize{24, 32} : monitorSize));
    REQUIRE(result.metadata.sourceTransform == (kind == CaptureBackendKind::Dxgi ? DXGI_MODE_ROTATION_ROTATE90 : DXGI_MODE_ROTATION_IDENTITY));
    REQUIRE(result.metadata.displayRotation == DXGI_MODE_ROTATION_ROTATE90);
    REQUIRE(result.metadata.backend == kind);
    REQUIRE(result.metadata.pixelFormat == DXGI_FORMAT_B8G8R8A8_UNORM);
    REQUIRE(result.metadata.sourcePixelFormat == DXGI_FORMAT_B8G8R8A8_UNORM);
    REQUIRE(result.metadata.bitsPerColor == 8);
    REQUIRE(result.metadata.signalEncoding == CaptureSignalEncoding::SdrRgb);
    REQUIRE_FALSE(result.metadata.hdr);
    REQUIRE(result.metadata.isCursorExcluded);
    REQUIRE(result.metadata.sourceCursorState == (kind == CaptureBackendKind::Dxgi ? CursorState::SeparatePointer : CursorState::Excluded));
    REQUIRE(result.metadata.timestamp.monotonic100ns > 0);
    std::int64_t converted = 0;
    if (kind == CaptureBackendKind::Dxgi)
    {
        REQUIRE(result.metadata.timestamp.domain == CaptureTimestampDomain::DxgiQpcTicks);
        REQUIRE(ConvertQpcTo100ns(result.metadata.timestamp.rawValue, result.metadata.timestamp.rawFrequency, converted));
    }
    else
    {
        REQUIRE(result.metadata.timestamp.domain == CaptureTimestampDomain::WgcSystemRelative100ns);
        REQUIRE(result.metadata.timestamp.rawFrequency == 10000000);
        converted = result.metadata.timestamp.rawValue;
    }
    REQUIRE(converted == result.metadata.timestamp.monotonic100ns);
    for (std::size_t index = 0; index < roiBytes; index++)
    {
        // Independent physical ROI oracle: top-left (7,5) on a 32-pixel-wide
        // monitor, not a readback of the source or the implementation's D3D box.
        const std::size_t expectedByte = ((index / 36 + 5) * 32 + index % 36 / 4 + 7) * 4 + index % 4;
        const auto expected = static_cast<std::byte>((expectedByte * 73 + seed * 19) & 255u);
        CAPTURE(index, seed);
        REQUIRE(result.pixels[index] == expected);
    }
}

void RequireLowFpsPipelineResult(const pbdemodd3d11::CaptureDemodulatorResult& result,
    const std::array<std::byte, pbprotocol::kBootstrapRecordBytes>& bootstrapRecord,
    const std::span<const pbdesktoplevels::AcceptedTransportBlock> expectedBlocks,
    const CaptureBackendKind kind, const std::uint64_t captureEpoch)
{
    constexpr CaptureSize contentSize{static_cast<std::int32_t>(pbmodulation::kLocalDesktopCanvasWidth),
        static_cast<std::int32_t>(pbmodulation::kLocalDesktopCanvasHeight)};
    REQUIRE(result.kind == pbdemodd3d11::CaptureDemodulatorResultKind::Transport);
    REQUIRE(result.metadata.domain.captureEpoch == captureEpoch);
    REQUIRE(result.metadata.captureObservation > 0);
    REQUIRE(result.metadata.sourceGeneration > 0);
    REQUIRE(result.metadata.slotGeneration > 0);
    REQUIRE(result.metadata.physicalRoi.left == 0);
    REQUIRE(result.metadata.physicalRoi.top == 0);
    REQUIRE(result.metadata.physicalRoi.right == contentSize.width);
    REQUIRE(result.metadata.physicalRoi.bottom == contentSize.height);
    REQUIRE(result.metadata.roiSize == contentSize);
    REQUIRE(result.metadata.sourceContentSize == contentSize);
    REQUIRE(result.metadata.sourceExtent == (kind == CaptureBackendKind::Dxgi ?
        CaptureSize{contentSize.height, contentSize.width} : contentSize));
    REQUIRE(result.metadata.sourceTransform == (kind == CaptureBackendKind::Dxgi ?
        DXGI_MODE_ROTATION_ROTATE90 : DXGI_MODE_ROTATION_IDENTITY));
    REQUIRE(result.metadata.displayRotation == DXGI_MODE_ROTATION_ROTATE90);
    REQUIRE(result.metadata.backend == kind);
    REQUIRE(result.metadata.pixelFormat == DXGI_FORMAT_B8G8R8A8_UNORM);
    REQUIRE(result.metadata.sourcePixelFormat == DXGI_FORMAT_B8G8R8A8_UNORM);
    REQUIRE(result.metadata.signalEncoding == CaptureSignalEncoding::SdrRgb);
    REQUIRE_FALSE(result.metadata.hdr);
    REQUIRE(result.metadata.isCursorExcluded);
    REQUIRE(result.bootstrapRecord == bootstrapRecord);
    REQUIRE(result.bootstrap.IsAccepted());
    REQUIRE(result.bootstrap.canonical44 == bootstrapRecord);
    REQUIRE(result.geometryStatus == pbdemodd3d11::CaptureDemodulatorGeometryStatus::ExactCanvas);
    REQUIRE(result.demodulation.visualProfileId == pbmodulation::kRemoteVisualLowFpsProfileId);
    REQUIRE(result.demodulation.metadata.domain == result.metadata.domain);
    REQUIRE(result.demodulation.metadata.captureObservation == result.metadata.captureObservation);
    REQUIRE(result.demodulation.evaluation.IsVerified());
    REQUIRE(result.demodulation.evaluation.falseAcceptedCodewords == 0);
    REQUIRE(result.demodulation.evaluation.fecFailures == 0);
    REQUIRE(result.demodulation.evaluation.crcFailures == 0);
    REQUIRE(result.demodulation.evaluation.identityFailures == 0);
    REQUIRE(result.demodulation.evaluation.comparedCodedBits == 0);
    REQUIRE(result.demodulation.acceptedTransportBlockCount == expectedBlocks.size());
    for (std::size_t index = 0; index < expectedBlocks.size(); index++)
    {
        REQUIRE(result.demodulation.acceptedTransportBlocks[index] == expectedBlocks[index]);
    }
    REQUIRE(result.demodulation.remoteMetricSummaryAvailable);
    REQUIRE(result.demodulation.remoteMetricSamples == pbmodulation::kRemoteVisualLowFpsCodedBits);
    REQUIRE(result.demodulation.metricReadbackBytes > 0);
}

template<typename CaptureType, typename TestAccess>
void ExerciseLowFpsPipeline(const CaptureBackendKind kind)
{
    const auto record = MakeLowFpsCaptureRecord();
    std::vector<std::byte> data(pbmodulation::kRemoteVisualLowFpsDataBytes);
    REQUIRE(pbdesktoplevels::GenerateDiagnosticData(record, data));
    std::vector<std::byte> raster(pbmodulation::kLocalDesktopFrameBgraBytes);
    REQUIRE(pbmodulation::EncodeRemoteVisualLowFpsFrame(record, data, raster));
    auto referenceCreated = pbdesktoplevels::ReferenceChannel::Create(pbdesktoplevels::kProcessingReservationBytes);
    REQUIRE(referenceCreated);
    auto reference = std::move(referenceCreated.Value());
    const pbmodulation::LumaView view{raster, pbmodulation::kLocalDesktopCanvasWidth,
        pbmodulation::kLocalDesktopCanvasHeight, pbmodulation::kLocalDesktopCanvasWidth * 4,
        pbmodulation::LumaPixelFormat::Bgra8};
    const auto referenceObservation = reference.DecodeRemoteVisualLowFps(view);
    REQUIRE(referenceObservation.modulation.IsAccepted());
    REQUIRE(referenceObservation.evaluation.IsVerified());
    const auto referenceBlocks = reference.GetAcceptedTransportBlocks();
    const std::vector<pbdesktoplevels::AcceptedTransportBlock> expectedBlocks(referenceBlocks.begin(), referenceBlocks.end());
    REQUIRE(expectedBlocks.size() == pbmodulation::kRemoteVisualLowFpsCodewords);

    const auto control = std::make_shared<PipelineControl>();
    const auto source = MakeLowFpsFrameSource(kind, raster);
    pbdemodd3d11::CaptureDemodulatorConfig consumerConfig;
    consumerConfig.visualProfileId = pbmodulation::kRemoteVisualLowFpsProfileId;
    consumerConfig.slotCount = 2;
    consumerConfig.maximumFrameAgeMilliseconds = 60000;
    consumerConfig.resultQueueCapacity = 2;
    consumerConfig.maximumRoiWidth = pbmodulation::kLocalDesktopCanvasWidth;
    consumerConfig.maximumRoiHeight = pbmodulation::kLocalDesktopCanvasHeight;
    pbdemodd3d11::CaptureDemodulatorBudget consumerBudget;
    REQUIRE(pbdemodd3d11::CalculateCaptureDemodulatorBudget(consumerConfig, consumerBudget));
    consumerConfig.maximumResidentBytes = consumerBudget.totalBytes;
    std::shared_ptr<pbdemodd3d11::CaptureDemodulator> consumer;
    REQUIRE(pbdemodd3d11::CaptureDemodulator::Create(consumerConfig, consumer));
    REQUIRE(consumer->ReservedBytes() == consumerBudget.totalBytes);

    CaptureNormalizeConfig config;
    config.capture.region = {reinterpret_cast<HMONITOR>(std::uintptr_t{1}),
        {0, 0, static_cast<LONG>(pbmodulation::kLocalDesktopCanvasWidth), static_cast<LONG>(pbmodulation::kLocalDesktopCanvasHeight)},
        {0, 0, static_cast<LONG>(pbmodulation::kLocalDesktopCanvasWidth), static_cast<LONG>(pbmodulation::kLocalDesktopCanvasHeight)},
        96, 96, DXGI_MODE_ROTATION_ROTATE90};
    config.capture.queuedFrameLimit = 1;
    config.capture.roiTextureCount = consumerConfig.slotCount;
    config.capture.gpuTimeoutMilliseconds = 60000;
    config.capture.maximumFrameAgeMilliseconds = consumerConfig.maximumFrameAgeMilliseconds;
    std::unique_ptr<CaptureType> capture;
    const auto created = TestAccess::CreateNormalized(config, consumer,
        std::make_unique<PipelineBackend>(control, kind, source), capture);
    CAPTURE(created.code, created.stage, created.nativeError, kind);
    REQUIRE(created);
    REQUIRE(capture);

    control->requestedFrames = 1;
    pbdemodd3d11::CaptureDemodulatorResult firstResult;
    bool firstResultReady = false;
    REQUIRE(Await([&]
    {
        firstResultReady = firstResultReady || consumer->TakeResult(firstResult);
        const auto snapshot = capture->GetSnapshot();
        return firstResultReady && snapshot.busyRoiTextures == 0 && snapshot.liveFrameLeases == 0;
    }, std::chrono::seconds(60)));
    RequireLowFpsPipelineResult(firstResult, record, expectedBlocks, kind, 1);
    const auto firstDomain = firstResult.metadata.domain;
    auto captureSnapshot = capture->GetSnapshot();
    REQUIRE(captureSnapshot.state == CaptureState::Running);
    REQUIRE(captureSnapshot.copiedFrames == 1);
    REQUIRE(captureSnapshot.deliveredFrames == 1);
    REQUIRE(captureSnapshot.consumerContinuationSubmissions == 1);
    REQUIRE(captureSnapshot.consumerContinuationCompletions == 1);
    REQUIRE(captureSnapshot.consumerContinuationRejections == 0);
    REQUIRE(captureSnapshot.frameLeaseHighWater == 1);
    REQUIRE(captureSnapshot.busyRoiTextures == 0);
    REQUIRE(captureSnapshot.liveFrameLeases == 0);
    auto consumerSnapshot = consumer->GetSnapshot();
    REQUIRE(consumerSnapshot.domainStarts == 1);
    REQUIRE(consumerSnapshot.completedFrames == 1);
    REQUIRE(consumerSnapshot.stagedGpuSubmissions == 1);
    REQUIRE(consumerSnapshot.stagedGpuCompletions == 1);
    REQUIRE(consumerSnapshot.exactGeometryFrames == 1);
    REQUIRE(consumerSnapshot.pendingFrames == 0);
    REQUIRE(consumerSnapshot.demodulator.pendingFrames == 0);
    REQUIRE(control->madeLeases == 1);
    REQUIRE(control->closes[1] == 1);
    REQUIRE(control->copies == 1);
    REQUIRE(control->consumes == 1);
    REQUIRE(control->completions == 2);

    TestAccess::RequestRecreate(*capture);
    REQUIRE(Await([&]
    {
        const auto captureState = capture->GetSnapshot();
        const auto consumerState = consumer->GetSnapshot();
        return captureState.captureEpoch == 2 && captureState.state == CaptureState::Running &&
            consumerState.domainStarts == 2 && consumerState.invalidations == 1;
    }, std::chrono::seconds(60)));
    REQUIRE(capture->GetSnapshot().busyRoiTextures == 0);
    REQUIRE(capture->GetSnapshot().liveFrameLeases == 0);
    control->requestedFrames = 2;
    pbdemodd3d11::CaptureDemodulatorResult secondResult;
    bool secondResultReady = false;
    REQUIRE(Await([&]
    {
        secondResultReady = secondResultReady || consumer->TakeResult(secondResult);
        const auto snapshot = capture->GetSnapshot();
        return secondResultReady && snapshot.busyRoiTextures == 0 && snapshot.liveFrameLeases == 0;
    }, std::chrono::seconds(60)));
    RequireLowFpsPipelineResult(secondResult, record, expectedBlocks, kind, 2);
    REQUIRE(secondResult.metadata.domain.sourceId == firstDomain.sourceId);
    REQUIRE(secondResult.metadata.domain != firstDomain);
    captureSnapshot = capture->GetSnapshot();
    REQUIRE(captureSnapshot.consumerContinuationSubmissions == 2);
    REQUIRE(captureSnapshot.consumerContinuationCompletions == 2);
    REQUIRE(captureSnapshot.consumerContinuationRejections == 0);
    REQUIRE(captureSnapshot.busyRoiTextures == 0);
    REQUIRE(captureSnapshot.liveFrameLeases == 0);
    const auto normalizeSnapshot = capture->GetNormalizationSnapshot();
    REQUIRE(normalizeSnapshot.active);
    REQUIRE(normalizeSnapshot.epochStarts == 2);
    REQUIRE(normalizeSnapshot.invalidations == 1);
    REQUIRE(normalizeSnapshot.acceptedFrames == 2);
    REQUIRE(normalizeSnapshot.erasedFrames == 0);
    REQUIRE(normalizeSnapshot.reservedConsumerBytes == consumer->ReservedBytes());
    consumerSnapshot = consumer->GetSnapshot();
    REQUIRE(consumerSnapshot.domainStarts == 2);
    REQUIRE(consumerSnapshot.invalidations == 1);
    REQUIRE(consumerSnapshot.completedFrames == 2);
    REQUIRE(consumerSnapshot.bootstrapAcceptedFrames == 2);
    REQUIRE(consumerSnapshot.bootstrapRejectedFrames == 0);
    REQUIRE(consumerSnapshot.stagedGpuSubmissions == 2);
    REQUIRE(consumerSnapshot.stagedGpuCompletions == 2);
    REQUIRE(consumerSnapshot.exactGeometryFrames == 2);
    REQUIRE(consumerSnapshot.rejectedGeometryFrames == 0);
    REQUIRE(consumerSnapshot.pendingFrames == 0);
    REQUIRE(consumerSnapshot.acceptedTransportBlocks == expectedBlocks.size() * 2);
    REQUIRE(consumerSnapshot.demodulator.pendingFrames == 0);
    REQUIRE(control->madeLeases == 2);
    REQUIRE(control->closes[2] == 1);
    REQUIRE(control->copies == 2);
    REQUIRE(control->consumes == 2);
    REQUIRE(control->completions == 4);

    REQUIRE(capture->Stop());
    const auto stopped = capture->GetSnapshot();
    REQUIRE(stopped.state == CaptureState::Stopped);
    REQUIRE(stopped.shutdownComplete);
    REQUIRE_FALSE(stopped.deferredCleanup);
    REQUIRE(stopped.busyRoiTextures == 0);
    REQUIRE(stopped.liveFrameLeases == 0);
    REQUIRE(stopped.queuedFrames == 0);
    REQUIRE(stopped.frameLeaseHighWater == 1);
    REQUIRE(control->closes[1] == 1);
    REQUIRE(control->closes[2] == 1);
    REQUIRE(control->shutdowns == 1);
    REQUIRE_FALSE(control->wrongOwner);
    consumerSnapshot = consumer->GetSnapshot();
    REQUIRE_FALSE(consumerSnapshot.active);
    REQUIRE(consumerSnapshot.invalidations == 2);
    REQUIRE(consumerSnapshot.pendingFrames == 0);
    REQUIRE(consumerSnapshot.demodulator.pendingFrames == 0);
    REQUIRE(consumerSnapshot.demodulator.shutdown);
    pbdemodd3d11::CaptureDemodulatorResult unexpected;
    REQUIRE_FALSE(consumer->TakeResult(unexpected));
}

template<typename CaptureType, typename TestAccess>
void ExercisePipeline(const CaptureBackendKind kind)
{
    const auto control = std::make_shared<PipelineControl>();
    CaptureNormalizeConfig config;
    config.capture.region = {reinterpret_cast<HMONITOR>(std::uintptr_t{1}), {-43, -35, -34, -29}, {-50, -40, -18, -16},
                             144, 192, DXGI_MODE_ROTATION_ROTATE90};
    config.capture.queuedFrameLimit = 1;
    config.capture.roiTextureCount = 2;
    config.capture.gpuTimeoutMilliseconds = 5000;
    // Keep freshness enabled, but isolate epoch invalidation from an unrelated
    // age timeout during the intentionally held CPU job and debug WARP startup.
    config.capture.maximumFrameAgeMilliseconds = 60000;
    config.capture.maximumCaptureBytes = 8 * 1024 * 1024;
    config.capture.maximumRoiBytes = 4096;
    DiagnosticReadbackConfig readbackConfig;
    readbackConfig.maximumRoiSize = roiSize;
    readbackConfig.stagingTextureCount = 2;
    readbackConfig.maximumFrameAgeMilliseconds = 60000;
    readbackConfig.maximumReadbackBytes = 4096;
    std::shared_ptr<DiagnosticCpuReadback> readback;
    REQUIRE(DiagnosticCpuReadback::Create(readbackConfig, std::make_shared<PipelineProcessor>(control), readback));
    std::unique_ptr<CaptureType> capture;
    // Release the CPU barrier before capture/readback destructors on any failed
    // REQUIRE. GPU cleanup itself remains production marker/deferred retirement.
    const ReleaseCpuOnExit release{control};
    const auto created = TestAccess::CreateNormalized(config, readback, std::make_unique<PipelineBackend>(control, kind), capture);
    CAPTURE(created.code, created.stage, created.nativeError);
    REQUIRE(created);
    REQUIRE(capture);
    const auto environment = capture->GetSnapshot().environment;

    control->requestedFrames = 1;
    REQUIRE(Await([&] { return control->GetCpu().commits == 1 && capture->GetSnapshot().busyRoiTextures == 0; }));
    REQUIRE(control->madeLeases == 1);
    REQUIRE(control->closes[1] == 1);
    REQUIRE(control->copies == 1);
    REQUIRE(control->consumes == 1);
    REQUIRE(control->completions == 1);
    REQUIRE(control->polls >= 2);
    REQUIRE(capture->GetSnapshot().liveFrameLeases == 0);
    REQUIRE(readback->GetSnapshot().mappedFrames == 1);
    REQUIRE(readback->GetSnapshot().pendingStagingFrames == 0);
    const auto first = control->GetCpu().committed[0];
    RequirePixels(first, kind, 1);
    REQUIRE(first.metadata.domain.captureEpoch == 1);
    REQUIRE(first.metadata.captureObservation == 1);
    REQUIRE(first.metadata.sourceGeneration == 1);
    REQUIRE(first.metadata.slotIndex == 0);
    REQUIRE(first.metadata.slotGeneration == 1);
    REQUIRE(first.metadata.adapterLuid.LowPart == environment.adapterLuid.LowPart);
    REQUIRE(first.metadata.adapterLuid.HighPart == environment.adapterLuid.HighPart);
    REQUIRE(std::ranges::any_of(first.metadata.domain.sourceId, [](const std::byte value) { return value != std::byte{}; }));
    // No second acquisition, Submit, manual Completed, or test-side GPU Flush
    // was needed to make this last/static frame reach the CPU processor.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(control->madeLeases == 1);
    REQUIRE(control->GetCpu().commits == 1);

    control->requestedFrames = 2;
    REQUIRE(Await([&] { return control->GetCpu().blocked; }));
    REQUIRE(control->GetCpu().commits == 1);
    TestAccess::RequestRecreate(*capture);
    REQUIRE(Await([&]
    {
        const auto state = capture->GetSnapshot();
        return state.captureEpoch == 2 && state.state == CaptureState::Running && readback->GetSnapshot().domainStarts == 2;
    }));
    REQUIRE(control->recreates == 1);
    REQUIRE(readback->GetSnapshot().invalidations == 1);
    REQUIRE(capture->GetSnapshot().liveFrameLeases == 0);
    REQUIRE(control->GetCpu().commits == 1);

    control->injectOldEpoch = true;
    REQUIRE(Await([&]
    {
        const auto state = capture->GetSnapshot();
        return control->closes[0] == 1 && state.arrivedFrames == 3 && state.busyRoiTextures == 0 && state.liveFrameLeases == 0;
    }));
    // Observe the stale lease by itself before allowing a new one. Otherwise
    // newest-only queue replacement could conceal a broken epoch rejection.
    REQUIRE(control->copies == 2);
    REQUIRE(control->textureReads == 2);
    REQUIRE(capture->GetSnapshot().droppedFrames == 1);
    REQUIRE(readback->GetSnapshot().mappedFrames == 2);
    control->requestedFrames = 3;
    REQUIRE(Await([&] { return readback->GetSnapshot().mappedFrames == 3 && capture->GetSnapshot().busyRoiTextures == 0; }));
    REQUIRE(control->madeLeases == 4);
    REQUIRE(control->closes[0] == 1);
    REQUIRE(control->textureReads == 3);
    REQUIRE(control->GetCpu().commits == 1);
    control->ReleaseCpu();
    REQUIRE(Await([&] { return control->GetCpu().commits == 2 && readback->GetSnapshot().discardedCandidates == 1; }));
    const auto observed = control->GetCpu();
    REQUIRE(observed.analyzes == 3);
    REQUIRE(observed.discards == 1);
    REQUIRE_FALSE(observed.callbackViolation);
    REQUIRE_FALSE(observed.inputChanged);
    REQUIRE_FALSE(observed.waitExpired);
    RequirePixels(observed.analyzed[1], kind, 2);
    RequirePixels(observed.committed[1], kind, 3);
    const auto& second = observed.analyzed[1].metadata;
    REQUIRE(second.domain == first.metadata.domain);
    REQUIRE(second.captureObservation == 2);
    REQUIRE(second.sourceGeneration == 1);
    REQUIRE(second.slotIndex == 0);
    REQUIRE(second.slotGeneration == 2);
    const auto& replacement = observed.committed[1].metadata;
    REQUIRE(replacement.domain.sourceId == first.metadata.domain.sourceId);
    REQUIRE(replacement.domain.captureEpoch == 2);
    REQUIRE(replacement.captureObservation == 4);
    REQUIRE(replacement.sourceGeneration == 2);
    REQUIRE(replacement.slotIndex == 0);
    REQUIRE(replacement.slotGeneration == 3);
    REQUIRE(observed.domain == replacement.domain);
    const auto normalized = capture->GetNormalizationSnapshot();
    REQUIRE(normalized.active);
    REQUIRE(normalized.epochStarts == 2);
    REQUIRE(normalized.invalidations == 1);
    REQUIRE(normalized.acceptedFrames == 3);
    REQUIRE(normalized.erasedFrames == 0);
    REQUIRE(normalized.reservedConsumerBytes == readback->ReservedBytes());
    REQUIRE(readback->GetSnapshot().drops[static_cast<std::size_t>(DiagnosticReadbackDropReason::InactiveDomain)] == 1);

    REQUIRE(capture->Stop());
    REQUIRE(capture->Stop());
    const auto stopped = capture->GetSnapshot();
    REQUIRE(stopped.state == CaptureState::Stopped);
    REQUIRE(stopped.shutdownComplete);
    REQUIRE_FALSE(stopped.deferredCleanup);
    REQUIRE(stopped.liveFrameLeases == 0);
    REQUIRE(stopped.queuedFrames == 0);
    REQUIRE(stopped.busyRoiTextures == 0);
    REQUIRE(stopped.arrivedFrames == 4);
    REQUIRE(stopped.droppedFrames == 1);
    REQUIRE(stopped.copiedFrames == 3);
    REQUIRE(stopped.deliveredFrames == 3);
    REQUIRE(stopped.roiCopyTimingSamples == stopped.copiedFrames);
    REQUIRE(stopped.roiCopyTimingUnavailable == 0);
    REQUIRE(stopped.roiCopyTimeTotal100ns >= stopped.roiCopyTimeHighWater100ns);
    REQUIRE(control->copies == 3);
    REQUIRE(control->consumes == 3);
    REQUIRE(control->completions == 3);
    REQUIRE(control->shutdowns == 1);
    REQUIRE_FALSE(control->wrongOwner);
    for (const auto& closes : control->closes)
    {
        REQUIRE(closes.load() == 1);
    }
    REQUIRE(readback->Stop(5000));
    const auto finalReadback = readback->GetSnapshot();
    REQUIRE(finalReadback.workerStopped);
    REQUIRE(finalReadback.cpuBuffersInUse == 0);
    REQUIRE(finalReadback.queuedFrames == 0);
    REQUIRE(finalReadback.pendingStagingFrames == 0);
    REQUIRE(finalReadback.committedFrames == 2);
    REQUIRE(finalReadback.analyzedFrames == 3);
    REQUIRE(finalReadback.invalidations == 2);
    REQUIRE(finalReadback.readbackBytes == 3 * roiBytes);
    REQUIRE(finalReadback.error);
    REQUIRE_FALSE(control->GetCpu().domain.has_value());
}

template<typename CaptureType, typename TestAccess>
void ExerciseStagedGpuPipeline(const CaptureBackendKind kind, const bool requestSecondContinuation = false)
{
    const auto control = std::make_shared<PipelineControl>();
    const auto observations = std::make_shared<StagedGpuObservations>();
    CaptureNormalizeConfig config;
    config.capture.region = {reinterpret_cast<HMONITOR>(std::uintptr_t{1}), {-43, -35, -34, -29}, {-50, -40, -18, -16},
                             144, 192, DXGI_MODE_ROTATION_ROTATE90};
    config.capture.queuedFrameLimit = 1;
    config.capture.roiTextureCount = 2;
    config.capture.gpuTimeoutMilliseconds = 5000;
    config.capture.maximumFrameAgeMilliseconds = 60000;
    config.capture.maximumCaptureBytes = 8 * 1024 * 1024;
    config.capture.maximumRoiBytes = 4096;
    std::unique_ptr<CaptureType> capture;
    const auto created = TestAccess::CreateNormalized(config, std::make_shared<StagedGpuConsumer>(observations, requestSecondContinuation),
                                                       std::make_unique<PipelineBackend>(control, kind), capture);
    CAPTURE(created.code, created.stage, created.nativeError);
    REQUIRE(created);
    REQUIRE(capture);
    control->requestedFrames = 1;
    REQUIRE(Await([&]
    {
        const auto snapshot = capture->GetSnapshot();
        return observations->mappedStages == 2 && snapshot.busyRoiTextures == 0 &&
               (!requestSecondContinuation || snapshot.shutdownComplete);
    }));
    const auto running = capture->GetSnapshot();
    const std::uint64_t expectedContinuations = requestSecondContinuation ? 2 : 1;
    const std::uint32_t expectedCallbacks = requestSecondContinuation ? 3 : 2;
    CHECK(static_cast<bool>(running.error) != requestSecondContinuation);
    CHECK(running.copiedFrames == 1);
    CHECK(running.deliveredFrames == 1);
    CHECK(running.consumerContinuationSubmissions == expectedContinuations);
    CHECK(running.consumerContinuationCompletions == expectedContinuations);
    CHECK(running.consumerContinuationRejections == static_cast<std::uint64_t>(requestSecondContinuation));
    CHECK(running.busyRoiTextures == 0);
    CHECK(running.liveFrameLeases == 0);
    CHECK(running.frameLeaseHighWater == 1);
    CHECK(observations->domainStarts == 1);
    CHECK(observations->submits == 1);
    CHECK(observations->completionCallbacks == expectedCallbacks);
    CHECK(observations->mappedStages == 2);
    CHECK_FALSE(observations->wrongTexture);
    CHECK_FALSE(observations->cancelledWithGpuObjects);
    CHECK(observations->firstPixels == observations->secondPixels);
    for (std::size_t index = 0; index < roiBytes; index++)
    {
        const std::size_t expectedByte = ((index / 36 + 5) * 32 + index % 36 / 4 + 7) * 4 + index % 4;
        const auto expected = static_cast<std::byte>((expectedByte * 73 + 19) & 255u);
        CAPTURE(index, kind);
        CHECK(observations->firstPixels[index] == expected);
    }
    const auto stopStatus = capture->Stop();
    CHECK(static_cast<bool>(stopStatus) != requestSecondContinuation);
    if (requestSecondContinuation)
    {
        CHECK(stopStatus.code == CaptureError::ConsumerFailure);
    }
    const auto stopped = capture->GetSnapshot();
    CHECK(stopped.shutdownComplete);
    CHECK(stopped.busyRoiTextures == 0);
    CHECK(stopped.liveFrameLeases == 0);
    CHECK(observations->invalidations == 1);
    CHECK(observations->terminalCancellations == static_cast<std::uint32_t>(requestSecondContinuation));
    CHECK(control->copies == 1);
    CHECK(control->consumes == 1);
    CHECK(control->completions == expectedCallbacks);
    CHECK(control->shutdowns == 1);
    CHECK_FALSE(control->wrongOwner);
}

} // namespace

TEST_CASE("Normalized capture facades drive real WARP crop retirement readback and epoch admission", "[capture-pipeline][warp][integration]")
{
    SECTION("WGC facade keeps an upright surface despite a rotated display")
    {
        ExercisePipeline<pbscreencapturewgc::WgcCapture, pbscreencapturewgc::WgcCaptureTestAccess>(CaptureBackendKind::Wgc);
    }
    SECTION("DXGI facade rotates raw pixels before the identical normalized CPU contract")
    {
        ExercisePipeline<pbscreencapturedxgi::DxgiCapture, pbscreencapturedxgi::DxgiCaptureTestAccess>(CaptureBackendKind::Dxgi);
    }
}

TEST_CASE("Normalized capture retains one ROI slot across a bounded staged GPU continuation", "[capture-pipeline][warp][staged-completion]")
{
    SECTION("WGC facade records a second retirement marker")
    {
        ExerciseStagedGpuPipeline<pbscreencapturewgc::WgcCapture, pbscreencapturewgc::WgcCaptureTestAccess>(CaptureBackendKind::Wgc);
    }
    SECTION("DXGI facade records the same second retirement marker")
    {
        ExerciseStagedGpuPipeline<pbscreencapturedxgi::DxgiCapture, pbscreencapturedxgi::DxgiCaptureTestAccess>(CaptureBackendKind::Dxgi);
    }
}

TEST_CASE("Normalized WGC and DXGI pipelines preserve one LF4 frame through Bootstrap GPU continuation and epoch recreate",
    "[capture-pipeline][warp][staged-completion][remote-visual][low-fps][epoch]")
{
    SECTION("WGC upright source reaches LF4 Transport truth")
    {
        ExerciseLowFpsPipeline<pbscreencapturewgc::WgcCapture, pbscreencapturewgc::WgcCaptureTestAccess>(CaptureBackendKind::Wgc);
    }
    SECTION("DXGI rotated source normalizes before the same LF4 Transport truth")
    {
        ExerciseLowFpsPipeline<pbscreencapturedxgi::DxgiCapture, pbscreencapturedxgi::DxgiCaptureTestAccess>(CaptureBackendKind::Dxgi);
    }
}

TEST_CASE("Capture runtime retires and rejects a second GPU continuation without an unbounded callback chain", "[capture-pipeline][warp][staged-completion][negative]")
{
    ExerciseStagedGpuPipeline<pbscreencapturewgc::WgcCapture, pbscreencapturewgc::WgcCaptureTestAccess>(CaptureBackendKind::Wgc, true);
}
