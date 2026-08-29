#pragma once

#include "duplication_source.h"

#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <atomic>
#include <deque>
#include <functional>
#include <thread>

namespace dxgitest
{
using namespace pbcapturenormalize;
using namespace pbcapturenormalize::detail;
using namespace pbscreencapturedxgi;
using namespace pbscreencapturedxgi::detail;
using Microsoft::WRL::ComPtr;

inline CaptureConfig MakeConfig()
{
    CaptureConfig config;
    config.region = {reinterpret_cast<HMONITOR>(std::uintptr_t{1}), {-90, 25, -60, 45}, {-100, 20, 0, 100}, 96, 96, DXGI_MODE_ROTATION_IDENTITY};
    config.gpuTimeoutMilliseconds = 1000;
    return config;
}

inline CaptureEnvironment MakeEnvironment()
{
    CaptureEnvironment environment;
    environment.region = MakeConfig().region;
    environment.contentSize = {100, 80};
    environment.sourceSize = {100, 80};
    environment.pixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    environment.backendKind = CaptureBackendKind::Dxgi;
    environment.bitsPerColor = 8;
    return environment;
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

// No D3D device or desktop is needed to exercise ownership. QueryInterface for
// a texture deliberately fails; the native GPU path has separate integration gates.
class Resource final : public IDXGIResource
{
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID identity, void** output) override
    {
        if (output == nullptr)
        {
            return E_POINTER;
        }
        *output = nullptr;
        if (identity == __uuidof(IUnknown) || identity == __uuidof(IDXGIObject) || identity == __uuidof(IDXGIDeviceSubObject) || identity == __uuidof(IDXGIResource))
        {
            *output = static_cast<IDXGIResource*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return references_.fetch_add(1) + 1;
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const auto remaining = references_.fetch_sub(1) - 1;
        if (remaining == 0)
        {
            delete this;
        }
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID, void**) override
    {
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void**) override
    {
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetSharedHandle(HANDLE*) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetUsage(DXGI_USAGE*) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetEvictionPriority(UINT) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetEvictionPriority(UINT*) override
    {
        return E_NOTIMPL;
    }
private:
    std::atomic<ULONG> references_{1};
};

struct Acquisition
{
    DXGI_OUTDUPL_FRAME_INFO info{};
    HRESULT acquireResult = S_OK;
    HRESULT shapeResult = S_OK;
    HRESULT releaseResult = S_OK;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO shape{DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR, 4, 4, 16, {1, 2}};
    std::uint32_t shapeBytes = 64;
    bool resourcePresent = true;
};

inline Acquisition Image(const std::int64_t timestamp = 100, const std::int64_t mouseTimestamp = 0, const bool visible = false)
{
    Acquisition acquisition;
    acquisition.info.LastPresentTime.QuadPart = timestamp;
    acquisition.info.AccumulatedFrames = 1;
    acquisition.info.LastMouseUpdateTime.QuadPart = mouseTimestamp;
    acquisition.info.PointerPosition.Visible = visible;
    acquisition.info.PointerPosition.Position = {7, 11};
    return acquisition;
}

struct Control
{
    std::mutex mutex;
    std::deque<Acquisition> acquisitions;
    std::vector<RawRoiFrameMetadata> delivered;
    std::vector<std::uint64_t> epochs;
    std::shared_ptr<DeferredCleanup> deferred;
    std::atomic<std::uint32_t> acquireCalls{0};
    std::atomic<std::uint32_t> acquired{0};
    std::atomic<std::uint32_t> releases{0};
    std::atomic<std::uint32_t> shapes{0};
    std::atomic<std::uint32_t> copies{0};
    std::atomic<std::uint32_t> consumes{0};
    std::atomic<std::uint32_t> recreates{0};
    std::atomic<std::uint32_t> shutdowns{0};
    std::atomic<bool> unsafeAcquire{false};
    std::atomic<bool> nonzeroTimeout{false};
    std::atomic<std::uint32_t> acquireTimeoutMilliseconds{0};
    std::atomic<bool> unsafeRecreate{false};
    std::atomic<bool> copyComplete{false};
    std::atomic<bool> consumeComplete{true};
    std::atomic<bool> available{true};
    std::atomic<bool> environmentChanged{false};
    std::atomic<bool> copyFailsBefore{false};
    std::atomic<bool> copyFailsAfter{false};
    void Push(const Acquisition& acquisition)
    {
        const std::lock_guard lock(mutex);
        acquisitions.push_back(acquisition);
    }
    std::vector<RawRoiFrameMetadata> Delivered()
    {
        const std::lock_guard lock(mutex);
        return delivered;
    }
    std::vector<std::uint64_t> Epochs()
    {
        const std::lock_guard lock(mutex);
        return epochs;
    }
    void FinishDeferred()
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

class ScriptedDuplication final : public DuplicationApi
{
public:
    explicit ScriptedDuplication(std::shared_ptr<Control> control) : control_(std::move(control))
    {
        resource_.Attach(new Resource);
    }
    HRESULT AcquireNextFrame(const std::uint32_t milliseconds, DXGI_OUTDUPL_FRAME_INFO& info, ComPtr<IDXGIResource>& resource) noexcept override
    {
        control_->acquireCalls++;
        control_->nonzeroTimeout = control_->nonzeroTimeout || milliseconds != 0;
        control_->acquireTimeoutMilliseconds = milliseconds;
        if (outstanding_)
        {
            control_->unsafeAcquire = true;
            return DXGI_ERROR_INVALID_CALL;
        }
        const std::lock_guard lock(control_->mutex);
        if (control_->acquisitions.empty())
        {
            return DXGI_ERROR_WAIT_TIMEOUT;
        }
        current_ = control_->acquisitions.front();
        control_->acquisitions.pop_front();
        if (FAILED(current_.acquireResult))
        {
            return current_.acquireResult;
        }
        info = current_.info;
        resource = current_.resourcePresent ? resource_ : nullptr;
        outstanding_ = true;
        control_->acquired++;
        return current_.acquireResult;
    }
    HRESULT GetFramePointerShape(const std::span<std::byte> buffer, std::uint32_t& bytes, DXGI_OUTDUPL_POINTER_SHAPE_INFO& shape) noexcept override
    {
        control_->shapes++;
        bytes = current_.shapeBytes;
        shape = current_.shape;
        if (SUCCEEDED(current_.shapeResult) && bytes <= buffer.size())
        {
            std::fill_n(buffer.data(), bytes, std::byte{0xa5});
        }
        return current_.shapeResult;
    }
    HRESULT ReleaseFrame() noexcept override
    {
        control_->releases++;
        if (!outstanding_)
        {
            control_->unsafeAcquire = true;
            return DXGI_ERROR_INVALID_CALL;
        }
        outstanding_ = false;
        return current_.releaseResult;
    }
private:
    const std::shared_ptr<Control> control_;
    ComPtr<IDXGIResource> resource_;
    Acquisition current_;
    bool outstanding_ = false;
};

struct SourceFixture
{
    std::shared_ptr<Control> control = std::make_shared<Control>();
    std::shared_ptr<FrameInbox> inbox = std::make_shared<FrameInbox>(MakeConfig());
    DuplicationFrameSource source;
    SourceFixture()
    {
        REQUIRE(source.Initialize(std::make_unique<ScriptedDuplication>(control), inbox, MakeEnvironment(), 1000));
        inbox->Resume({100, 80}, 7);
        source.Start(7);
    }
    ~SourceFixture()
    {
        inbox->Pause();
        static_cast<void>(source.Reset());
    }
};

class Consumer final : public RawRoiConsumer
{
public:
    explicit Consumer(std::shared_ptr<Control> control) : control_(std::move(control))
    {
    }
    CaptureStatus EpochStarted(const std::uint64_t epoch, const CaptureEnvironment&, ID3D11Device*) override
    {
        const std::lock_guard lock(control_->mutex);
        control_->epochs.push_back(epoch);
        return {};
    }
    CaptureStatus Submit(const RawRoiFrameMetadata& metadata, ID3D11Texture2D*, ID3D11DeviceContext*) override
    {
        const std::lock_guard lock(control_->mutex);
        control_->delivered.push_back(metadata);
        control_->consumes++;
        return {};
    }
private:
    std::shared_ptr<Control> control_;
};

// Replace only D3D/desktop effects. All tests below still run CaptureRuntime,
// FrameInbox and DuplicationFrameSource's production source-retirement logic.
class Backend final : public CaptureBackend
{
public:
    explicit Backend(std::shared_ptr<Control> control) : control_(std::move(control))
    {
    }
    CaptureBackendKind Kind() const noexcept override
    {
        return CaptureBackendKind::Dxgi;
    }
    CaptureStatus Initialize(const CaptureConfig&, std::shared_ptr<FrameInbox> inbox) noexcept override
    {
        inbox_ = std::move(inbox);
        waiting_ = !control_->available;
        return waiting_ ? CaptureStatus{} : source_.Initialize(std::make_unique<ScriptedDuplication>(control_), inbox_, MakeEnvironment(), 1000);
    }
    CaptureStatus Start(const std::uint64_t epoch) noexcept override
    {
        source_.Start(epoch);
        return {};
    }
    CaptureStatus Pause() noexcept override
    {
        return {};
    }
    bool CallbacksIdle() const noexcept override
    {
        return true;
    }
    bool WaitingForEnvironment() const noexcept override
    {
        return waiting_;
    }
    CaptureStatus Acquire() noexcept override
    {
        return waiting_ ? CaptureStatus{} : source_.Acquire();
    }
    CaptureStatus CheckEnvironment(bool& changed) noexcept override
    {
        changed = control_->environmentChanged.exchange(false);
        return {};
    }
    CaptureStatus Recreate(CaptureSize) noexcept override
    {
        control_->recreates++;
        control_->unsafeRecreate = control_->unsafeRecreate || source_.Outstanding();
        const auto status = source_.Reset();
        return status ? Initialize(MakeConfig(), inbox_) : status;
    }
    CaptureEnvironment GetEnvironment() const noexcept override
    {
        return MakeEnvironment();
    }
    CaptureCapabilities GetCapabilities() const noexcept override
    {
        return {};
    }
    CaptureStatus NotifyEpoch(RawRoiConsumer& consumer, const std::uint64_t epoch) noexcept override
    {
        return consumer.EpochStarted(epoch, MakeEnvironment(), nullptr);
    }
    CaptureStatus Copy(const FrameLease&, const std::size_t slot, bool& submitted) noexcept override
    {
        submitted = !control_->copyFailsBefore;
        if (submitted)
        {
            phases_[slot] = 1;
            control_->copies++;
        }
        return control_->copyFailsBefore || control_->copyFailsAfter ? CaptureStatus::Failure(CaptureError::NativeFailure, CaptureStage::Copy, E_FAIL) : CaptureStatus{};
    }
    CaptureStatus Consume(RawRoiConsumer& consumer, const RawRoiFrameMetadata& metadata, const std::size_t slot) noexcept override
    {
        phases_[slot] = 2;
        return consumer.Submit(metadata, nullptr, nullptr);
    }
    CompletionResult Poll(const std::size_t slot) noexcept override
    {
        return {{}, phases_[slot] == 1 ? control_->copyComplete.load() : control_->consumeComplete.load()};
    }
    bool DeviceRemoved() const noexcept override
    {
        return false;
    }
    std::int32_t DeviceRemovalReason() const noexcept override
    {
        return S_OK;
    }
    CaptureStatus Shutdown() noexcept override
    {
        control_->shutdowns++;
        return source_.Reset();
    }
    void DeferShutdown(std::shared_ptr<DeferredCleanup> owner) noexcept override
    {
        const std::lock_guard lock(control_->mutex);
        control_->deferred = std::move(owner);
    }
    void UpdateSnapshot(CaptureSnapshot& snapshot) const noexcept override
    {
        source_.UpdateSnapshot(snapshot);
    }
private:
    const std::shared_ptr<Control> control_;
    std::shared_ptr<FrameInbox> inbox_;
    DuplicationFrameSource source_;
    std::array<std::uint8_t, maximumRoiTextures> phases_{};
    bool waiting_ = false;
};

inline CaptureStatus Create(const std::shared_ptr<Control>& control, std::unique_ptr<DxgiCapture>& capture, const CaptureConfig& config = MakeConfig())
{
    return DxgiCaptureTestAccess::CreateWithBackend(config, std::make_shared<Consumer>(control), std::make_unique<Backend>(control), capture);
}

} // namespace dxgitest
