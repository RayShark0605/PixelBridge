#include "pbcapturenormalize/diagnostic_readback.h"
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
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <thread>
#include <utility>

using namespace pbcapturenormalize;
using namespace pbcapturenormalize::detail;
using Microsoft::WRL::ComPtr;

namespace
{

constexpr CaptureSize monitorSize{32, 24};
constexpr CaptureSize roiSize{9, 6};
constexpr std::size_t roiBytes = 9 * 6 * 4;

template<typename Predicate>
bool Await(Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
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
    PipelineBackend(std::shared_ptr<PipelineControl> control, const CaptureBackendKind kind) : control_(std::move(control)), kind_(kind)
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
        environment_.contentSize = monitorSize;
        environment_.sourceSize = kind_ == CaptureBackendKind::Dxgi ? CaptureSize{24, 32} : monitorSize;
        environment_.sourceRotation = kind_ == CaptureBackendKind::Dxgi ? DXGI_MODE_ROTATION_ROTATE90 : DXGI_MODE_ROTATION_IDENTITY;
        environment_.backendKind = kind_;
        environment_.pixelFormat = config.pixelFormat;
        environment_.adapterLuid = description.AdapterLuid;
        environment_.bitsPerColor = 8;
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
        if (active_ || requestedSize != monitorSize || inbox_->counters->live != 0)
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
    CaptureStatus Complete(RawRoiConsumer& consumer, const RawRoiFrameMetadata& metadata, std::size_t, const bool cancelled) noexcept override
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
        // Fixed full-surface bytes, with DXGI's raw axes rotated relative to the
        // physical desktop. The oracle below never reads the crop box or source.
        std::array<std::byte, 32 * 24 * 4> pixels{};
        const UINT width = static_cast<UINT>(environment_.sourceSize.width);
        const UINT height = static_cast<UINT>(environment_.sourceSize.height);
        for (UINT sourceY = 0; sourceY < height; sourceY++)
        {
            for (UINT sourceX = 0; sourceX < width; sourceX++)
            {
                const UINT visualX = kind_ == CaptureBackendKind::Dxgi ? 31u - sourceY : sourceX;
                const UINT visualY = kind_ == CaptureBackendKind::Dxgi ? sourceX : sourceY;
                for (UINT channel = 0; channel < 4; channel++)
                {
                    const UINT visualByte = (visualY * 32u + visualX) * 4u + channel;
                    pixels[(static_cast<std::size_t>(sourceY) * width + sourceX) * 4u + channel] = static_cast<std::byte>((visualByte * 73u + seed * 19u) & 255u);
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
        const D3D11_SUBRESOURCE_DATA initial{pixels.data(), width * 4u, 0};
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
        FrameLease frame(source.release(), CloseTexture, ReadTexture, inbox_->counters, monitorSize, now100ns, epoch);
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
