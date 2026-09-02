#include "d3d_roi_ring.h"
#include "session_capabilities.h"
#include "../../PBCaptureNormalize/src/native_support.h"

#include <appmodel.h>
#include <dxgi1_6.h>
#include <roapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <type_traits>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>

namespace pbscreencapturewgc::detail
{
using Microsoft::WRL::ComPtr;
using namespace winrt::Windows::Graphics::Capture;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Security::Authorization::AppCapabilityAccess::AppCapabilityAccessStatus;

namespace
{

[[nodiscard]] bool SameRectangle(const RECT& left, const RECT& right) noexcept
{
    return left.left == right.left && left.top == right.top && left.right == right.right && left.bottom == right.bottom;
}

[[nodiscard]] CaptureStatus ExceptionStatus(const CaptureStage stage) noexcept
{
    return FromHresult(winrt::to_hresult(), stage);
}

[[nodiscard]] HRESULT CloseNativeFrame(void* const pointer) noexcept
{
    const Direct3D11CaptureFrame frame(pointer, winrt::take_ownership_from_abi);
    try
    {
        frame.Close();
        return S_OK;
    }
    catch (...)
    {
        return winrt::to_hresult();
    }
}

[[nodiscard]] HRESULT GetNativeTexture(void* const pointer, ID3D11Texture2D** const texture) noexcept
{
    try
    {
        Direct3D11CaptureFrame frame{nullptr};
        winrt::copy_from_abi(frame, pointer);
        const auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        return access->GetInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(texture));
    }
    catch (...)
    {
        return winrt::to_hresult();
    }
}

// Event handlers capture only this independently owned gate, never a raw backend
// or capture object. Disable is serialized with Enter, so an old event cannot
// acquire a frame after the owner has observed quiescence and called Recreate.
class CallbackGate
{
public:
    CallbackGate(std::shared_ptr<FrameInbox> input, const std::uint64_t epoch, const bool cursorExcluded)
        : inbox(std::move(input)), captureEpoch(epoch), cursorState(cursorExcluded ? pbcapturenormalize::CursorState::Excluded : pbcapturenormalize::CursorState::Unknown),
          idleEvent(CreateEventW(nullptr, TRUE, TRUE, nullptr))
    {
        winrt::check_bool(idleEvent != nullptr);
    }
    ~CallbackGate()
    {
        CloseHandle(idleEvent);
    }
    CallbackGate(const CallbackGate&) = delete;
    CallbackGate& operator=(const CallbackGate&) = delete;
    CallbackGate(CallbackGate&&) = delete;
    CallbackGate& operator=(CallbackGate&&) = delete;
    [[nodiscard]] bool Enter() noexcept
    {
        const std::lock_guard lock(mutex_);
        if (!enabled_ || active_)
        {
            return false;
        }
        active_ = true;
        ResetEvent(idleEvent);
        return true;
    }
    void Leave() noexcept
    {
        const std::lock_guard lock(mutex_);
        active_ = false;
        SetEvent(idleEvent);
    }
    void Disable() noexcept
    {
        const std::lock_guard lock(mutex_);
        enabled_ = false;
    }
    [[nodiscard]] bool Idle() const noexcept
    {
        const std::lock_guard lock(mutex_);
        return !active_;
    }
    std::shared_ptr<FrameInbox> inbox;
    const std::uint64_t captureEpoch;
    const pbcapturenormalize::CursorState cursorState;
    HANDLE idleEvent = nullptr;

private:
    mutable std::mutex mutex_;
    bool enabled_ = true;
    bool active_ = false;
};

static_assert(!std::is_copy_constructible_v<CallbackGate> && !std::is_copy_assignable_v<CallbackGate>);
static_assert(!std::is_move_constructible_v<CallbackGate> && !std::is_move_assignable_v<CallbackGate>);

void OnFrameArrived(const std::shared_ptr<CallbackGate>& gate, const Direct3D11CaptureFramePool& sender) noexcept
{
    if (!gate->Enter())
    {
        return;
    }
    struct Guard
    {
        CallbackGate& gate;
        ~Guard()
        {
            gate.Leave();
        }
    };
    const Guard guard{*gate};
    Direct3D11CaptureFrame frame{nullptr};
    try
    {
        frame = sender.TryGetNextFrame();
        if (!frame)
        {
            return;
        }
        const auto size = frame.ContentSize();
        const auto timestamp = frame.SystemRelativeTime().count();
        FrameLease lease(winrt::detach_abi(frame), CloseNativeFrame, GetNativeTexture, gate->inbox->counters,
                         {size.Width, size.Height}, timestamp, gate->captureEpoch);
        lease.cursorState = gate->cursorState;
        gate->inbox->Push(std::move(lease));
    }
    catch (...)
    {
        const auto status = ExceptionStatus(CaptureStage::Callback);
        if (frame)
        {
            static_cast<void>(CloseNativeFrame(winrt::detach_abi(frame)));
        }
        gate->inbox->ReportError(status, gate->captureEpoch);
    }
}

class NativeCaptureBackend final : public CaptureBackend
{
public:
    explicit NativeCaptureBackend(const NativeCaptureOptions& options) : options_(options)
    {
    }
    ~NativeCaptureBackend() override
    {
        static_cast<void>(Shutdown());
    }

    CaptureStatus Initialize(const WgcCaptureConfig& config, std::shared_ptr<FrameInbox> inbox) noexcept override
    {
        config_ = config;
        inbox_ = std::move(inbox);
        capabilities_ = {};
        CaptureStage stage = CaptureStage::Apartment;
        try
        {
            // Deferred GPU retirement can outlive the original owner thread's
            // apartment. Keep the MTA alive until every WinRT lease is returned.
            winrt::check_hresult(CoIncrementMTAUsage(&mtaUsage_));
            stage = CaptureStage::Region;
            pbscreenregion::ScreenCaptureRegion resolved;
            const auto resolvedStatus = pbscreenregion::ResolveScreenCaptureRegion(config.region.physicalRect, resolved);
            if (!resolvedStatus)
            {
                const auto error = resolvedStatus.code == pbscreenregion::ScreenRegionErrorCode::DpiAwarenessRequired
                                       ? CaptureError::DpiAwarenessRequired : CaptureError::RegionChanged;
                return CaptureStatus::Failure(error, stage, resolvedStatus.nativeError);
            }
            if (resolved.monitor != config.region.monitor ||
                (!initializedBefore_ && (!SameRectangle(resolved.monitorPhysicalRect, config.region.monitorPhysicalRect) ||
                 resolved.rotation != config.region.rotation || resolved.dpiX != config.region.dpiX || resolved.dpiY != config.region.dpiY)))
            {
                return CaptureStatus::Failure(CaptureError::RegionChanged, stage);
            }
            environment_.region = resolved;
            environment_.pixelFormat = config.pixelFormat;
            stage = CaptureStage::Adapter;
            winrt::check_hresult(CreateDXGIFactory1(IID_PPV_ARGS(&factory_)));
            const auto adapterStatus = FindAdapter();
            if (!adapterStatus)
            {
                return adapterStatus;
            }
            Fault(stage);
            stage = CaptureStage::Device;
            const auto deviceStatus = CreateCaptureDevice(adapter_.Get(), options_.debugLayer, device_, context_);
            if (!deviceStatus)
            {
                return deviceStatus;
            }
            const auto retirementStatus = retirement_.Initialize(device_.Get(), context_.Get());
            if (!retirementStatus)
            {
                return retirementStatus;
            }
            ComPtr<IDXGIDevice> dxgiDevice;
            winrt::check_hresult(device_.As(&dxgiDevice));
            winrt::com_ptr<::IInspectable> inspectable;
            winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put()));
            directDevice_ = inspectable.as<IDirect3DDevice>();
            Fault(stage);
            stage = CaptureStage::CaptureItem;
            // Do not use the projection's process-wide static factory cache: a
            // short-lived owner MTA can be the last apartment and unload WinRT
            // DLLs between captures. Keep activation factories local to this MTA.
            const auto sessionFactory = winrt::try_get_activation_factory<GraphicsCaptureSession, IGraphicsCaptureSessionStatics>();
            if (!sessionFactory || !sessionFactory.IsSupported())
            {
                return CaptureStatus::Failure(CaptureError::Unsupported, stage);
            }
            const auto interop = winrt::try_get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
            if (!interop)
            {
                return CaptureStatus::Failure(CaptureError::Unsupported, stage);
            }
            winrt::check_hresult(interop->CreateForMonitor(resolved.monitor, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item_)));
            const auto itemSize = item_.Size();
            environment_.contentSize = {itemSize.Width, itemSize.Height};
            environment_.sourceSize = environment_.contentSize;
            environment_.sourceRotation = DXGI_MODE_ROTATION_IDENTITY;
            CaptureLayout layout;
            const auto layoutStatus = ValidateLayout(config_, environment_, layout);
            if (!layoutStatus)
            {
                return layoutStatus;
            }
            Fault(stage);
            stage = CaptureStage::TextureRing;
            const auto ringStatus = ring_.Initialize(device_.Get(), context_.Get(), config_, environment_, options_.forceQuery);
            if (!ringStatus)
            {
                return ringStatus;
            }
            capabilities_.fenceRetirement = ring_.UsesFence();
            Fault(stage);
            stage = CaptureStage::FramePool;
            const auto poolFactory = winrt::try_get_activation_factory<Direct3D11CaptureFramePool, IDirect3D11CaptureFramePoolStatics2>();
            if (!poolFactory)
            {
                return CaptureStatus::Failure(CaptureError::Unsupported, stage);
            }
            pool_ = poolFactory.CreateFreeThreaded(directDevice_, static_cast<DirectXPixelFormat>(config_.pixelFormat),
                                                   static_cast<std::int32_t>(layout.poolBufferCount), itemSize);
            poolBufferCount_ = layout.poolBufferCount;
            Fault(stage);
            initializedBefore_ = true;
            return {};
        }
        catch (...)
        {
            return ExceptionStatus(stage);
        }
    }

    CaptureStatus Start(const std::uint64_t epoch) noexcept override
    {
        try
        {
            const bool newSession = !session_;
            if (newSession)
            {
                session_ = pool_.CreateCaptureSession(item_);
                ProbeCapabilities();
            }
            gate_ = std::make_shared<CallbackGate>(inbox_, epoch, capabilities_.cursorExcluded);
            const auto gate = gate_;
            frameToken_ = pool_.FrameArrived([gate](const auto& sender, const auto&)
            {
                OnFrameArrived(gate, sender);
            });
            frameRegistered_ = true;
            closedToken_ = item_.Closed([gate](const auto&, const auto&)
            {
                gate->inbox->ReportError(CaptureStatus::Failure(CaptureError::AccessLost, CaptureStage::CaptureItem), gate->captureEpoch);
            });
            closedRegistered_ = true;
            Fault(CaptureStage::Session);
            if (newSession)
            {
                session_.StartCapture();
            }
            else
            {
                // A continuously presenting source can fill the recreated pool
                // while Pause has revoked the old FrameArrived handler. WinRT
                // does not guarantee a new event merely because a handler is
                // attached to an already-nonempty pool. Drain at most the fixed
                // pool capacity through the new epoch gate so the queue reaches
                // an event-producing state again. FrameInbox remains bounded
                // and drops older arrivals if more than its configured limit
                // are recovered here.
                for (std::uint32_t index = 0; index < poolBufferCount_; index++)
                {
                    OnFrameArrived(gate, pool_);
                }
            }
            return {};
        }
        catch (...)
        {
            return ExceptionStatus(CaptureStage::Session);
        }
    }

    CaptureStatus Pause() noexcept override
    {
        if (gate_)
        {
            gate_->Disable();
        }
        CaptureStatus status;
        try
        {
            if (frameRegistered_)
            {
                pool_.FrameArrived(frameToken_);
                frameRegistered_ = false;
            }
        }
        catch (...)
        {
            status = ExceptionStatus(CaptureStage::Shutdown);
        }
        try
        {
            if (closedRegistered_)
            {
                item_.Closed(closedToken_);
                closedRegistered_ = false;
            }
        }
        catch (...)
        {
            if (status)
            {
                status = ExceptionStatus(CaptureStage::Shutdown);
            }
        }
        // Do not Close a session/pool while a FrameArrived callback is active.
        return status;
    }

    bool CallbacksIdle() const noexcept override
    {
        return !gate_ || gate_->Idle();
    }

    CaptureStatus CheckEnvironment(bool& changed) noexcept override
    {
        changed = false;
        try
        {
            PollBorderless();
            pbscreenregion::ScreenCaptureRegion current;
            const auto status = pbscreenregion::ResolveScreenCaptureRegion(config_.region.physicalRect, current);
            if (!status || current.monitor != environment_.region.monitor)
            {
                return CaptureStatus::Failure(CaptureError::RegionChanged, CaptureStage::Region, status.nativeError);
            }
            auto currentOutput = environment_;
            winrt::check_hresult(ReadOutputState(*output_.Get(), currentOutput));
            changed = !factory_->IsCurrent() || !SameRectangle(current.monitorPhysicalRect, environment_.region.monitorPhysicalRect) ||
                      current.dpiX != environment_.region.dpiX || current.dpiY != environment_.region.dpiY ||
                      current.rotation != environment_.region.rotation || currentOutput.hdr != environment_.hdr ||
                      currentOutput.displayFrequency != environment_.displayFrequency || currentOutput.bitsPerColor != environment_.bitsPerColor ||
                      currentOutput.outputColorSpace != environment_.outputColorSpace;
            return {};
        }
        catch (...)
        {
            return ExceptionStatus(CaptureStage::Region);
        }
    }

    CaptureStatus Recreate(const CaptureSize requestedSize) noexcept override
    {
        try
        {
            if (!CallbacksIdle())
            {
                return CaptureStatus::Failure(CaptureError::InternalError, CaptureStage::Recreate);
            }
            Fault(CaptureStage::Recreate);
            if (!factory_->IsCurrent())
            {
                const auto savedInbox = inbox_;
                const auto status = Shutdown();
                return status ? Initialize(config_, savedInbox) : status;
            }
            // Keep the session alive across frame-pool Recreate. Closing a WGC
            // session is terminal on this pool; creating another can fail with
            // E_UNEXPECTED. Pause means revoke admission, not Close the session.
            pbscreenregion::ScreenCaptureRegion current;
            const auto resolved = pbscreenregion::ResolveScreenCaptureRegion(config_.region.physicalRect, current);
            if (!resolved || current.monitor != config_.region.monitor)
            {
                return CaptureStatus::Failure(CaptureError::RegionChanged, CaptureStage::Recreate, resolved.nativeError);
            }
            auto environment = environment_;
            environment.region = current;
            const auto sizeStatus = ResolveRecreateContentSize(requestedSize, environment_, current, environment.contentSize);
            if (!sizeStatus)
            {
                return sizeStatus;
            }
            environment.sourceSize = environment.contentSize;
            environment.sourceRotation = DXGI_MODE_ROTATION_IDENTITY;
            const winrt::Windows::Graphics::SizeInt32 size{environment.contentSize.width, environment.contentSize.height};
            winrt::check_hresult(ReadOutputState(*output_.Get(), environment));
            if (environment.hdr && config_.pixelFormat != DXGI_FORMAT_R16G16B16A16_FLOAT)
            {
                return CaptureStatus::Failure(CaptureError::Unsupported, CaptureStage::Recreate);
            }
            CaptureLayout layout;
            auto status = ValidateLayout(config_, environment, layout);
            if (!status)
            {
                return status;
            }
            // All acquired frames are closed and all submitted work is retired.
            pool_.Recreate(directDevice_, static_cast<DirectXPixelFormat>(config_.pixelFormat),
                           static_cast<std::int32_t>(layout.poolBufferCount), size);
            poolBufferCount_ = layout.poolBufferCount;
            status = ring_.Recreate(config_, environment);
            if (status)
            {
                environment_ = environment;
            }
            return status;
        }
        catch (...)
        {
            return ExceptionStatus(CaptureStage::Recreate);
        }
    }

    CaptureEnvironment GetEnvironment() const noexcept override
    {
        return environment_;
    }
    CaptureCapabilities GetCapabilities() const noexcept override
    {
        return capabilities_;
    }
    CaptureStatus NotifyEpoch(RoiConsumer& consumer, const std::uint64_t epoch) noexcept override
    {
        try
        {
            return consumer.EpochStarted(epoch, environment_, device_.Get());
        }
        catch (...)
        {
            return CaptureStatus::Failure(CaptureError::ConsumerFailure, CaptureStage::Consumer);
        }
    }
    CaptureStatus Copy(const FrameLease& frame, const std::size_t slot, bool& submitted) noexcept override
    {
        return ring_.Copy(frame, slot, submitted);
    }
    CaptureStatus Consume(RoiConsumer& consumer, const RoiFrameMetadata& metadata, const std::size_t slot) noexcept override
    {
        return ring_.Consume(consumer, metadata, slot);
    }
    CaptureConsumerCompletion Complete(RoiConsumer& consumer, const RoiFrameMetadata& metadata, std::size_t, const bool cancelled) noexcept override
    {
        return ring_.Complete(consumer, metadata, cancelled);
    }
    CompletionResult Poll(const std::size_t slot) noexcept override
    {
        if (options_.holdCompletionPolling && !DeviceRemoved())
        {
            return {{}, false};
        }
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
        CaptureStatus status = Pause();
        try
        {
            CloseSession();
        }
        catch (...)
        {
            status = status ? ExceptionStatus(CaptureStage::Shutdown) : status;
        }
        try
        {
            if (pool_)
            {
                pool_.Close();
            }
        }
        catch (...)
        {
            status = status ? ExceptionStatus(CaptureStage::Shutdown) : status;
        }
        pool_ = nullptr;
        poolBufferCount_ = 0;
        item_ = nullptr;
        frameRegistered_ = false;
        closedRegistered_ = false;
        gate_.reset();
        if (context_)
        {
            context_->ClearState();
        }
        const auto debug = ring_.CheckDebug();
        if (status)
        {
            status = debug;
        }
        ring_.Reset();
        directDevice_ = nullptr;
        retirement_.Reset();
        context_.Reset();
        device_.Reset();
        output_.Reset();
        adapter_.Reset();
        factory_.Reset();
        if (mtaUsage_ != nullptr)
        {
            const HRESULT result = CoDecrementMTAUsage(std::exchange(mtaUsage_, nullptr));
            if (status)
            {
                status = FromHresult(result, CaptureStage::Apartment);
            }
        }
        return status;
    }

    void DeferShutdown(std::shared_ptr<DeferredCleanup> owner) noexcept override
    {
        retirement_.Defer(std::move(owner), gate_ ? gate_->idleEvent : nullptr);
    }

private:
    void Fault(const CaptureStage stage) const
    {
        if (options_.failAfterStage == stage)
        {
            winrt::throw_hresult(E_FAIL);
        }
    }

    [[nodiscard]] CaptureStatus FindAdapter()
    {
        const auto status = FindCaptureAdapter(factory_.Get(), environment_.region, adapter_, output_, environment_);
        if (!status)
        {
            return status;
        }
        return environment_.hdr && config_.pixelFormat != DXGI_FORMAT_R16G16B16A16_FLOAT
                   ? CaptureStatus::Failure(CaptureError::Unsupported, CaptureStage::Adapter) : CaptureStatus{};
    }

    void ProbeCapabilities()
    {
        const bool fence = capabilities_.fenceRetirement;
        capabilities_ = {};
        capabilities_.fenceRetirement = fence;
        ProbeCursorAndCadence(session_, config_, capabilities_);
        if (config_.requestBorderless && capabilities_.borderlessAvailable)
        {
            try
            {
                UINT32 length = 0;
                const LONG packageResult = GetCurrentPackageFullName(&length, nullptr);
                if (packageResult == APPMODEL_ERROR_NO_PACKAGE)
                {
                    capabilities_.borderlessNativeError = HRESULT_FROM_WIN32(packageResult);
                }
                else
                {
                    winrt::check_hresult(packageResult == ERROR_INSUFFICIENT_BUFFER ? S_OK : HRESULT_FROM_WIN32(packageResult));
                    const auto factory = winrt::try_get_activation_factory<GraphicsCaptureAccess, IGraphicsCaptureAccessStatics>();
                    if (factory)
                    {
                        borderlessRequest_ = factory.RequestAccessAsync(GraphicsCaptureAccessKind::Borderless);
                        capabilities_.borderlessAccessRequested = true;
                    }
                }
            }
            catch (...)
            {
                capabilities_.borderlessNativeError = winrt::to_hresult();
            }
        }
    }

    void PollBorderless() noexcept
    {
        try
        {
            if (!borderlessRequest_ || borderlessRequest_.Status() == AsyncStatus::Started)
            {
                return;
            }
            if (borderlessRequest_.Status() == AsyncStatus::Completed)
            {
                ApplyBorderlessPermission(session_, borderlessRequest_.GetResults() == AppCapabilityAccessStatus::Allowed, capabilities_);
            }
            else
            {
                capabilities_.borderlessNativeError = borderlessRequest_.ErrorCode();
            }
            borderlessRequest_ = nullptr;
        }
        catch (...)
        {
            capabilities_.borderlessNativeError = winrt::to_hresult();
            borderlessRequest_ = nullptr;
        }
    }

    void CloseSession()
    {
        const auto request = std::exchange(borderlessRequest_, nullptr);
        const auto session = std::exchange(session_, nullptr);
        HRESULT error = S_OK;
        try
        {
            if (request)
            {
                request.Cancel();
            }
        }
        catch (...)
        {
            error = winrt::to_hresult();
        }
        try
        {
            if (session)
            {
                session.Close();
            }
        }
        catch (...)
        {
            if (SUCCEEDED(error))
            {
                error = winrt::to_hresult();
            }
        }
        winrt::check_hresult(error);
    }

    const NativeCaptureOptions options_;
    WgcCaptureConfig config_;
    CaptureEnvironment environment_;
    CaptureCapabilities capabilities_;
    std::shared_ptr<FrameInbox> inbox_;
    std::shared_ptr<CallbackGate> gate_;
    ComPtr<IDXGIFactory1> factory_;
    ComPtr<IDXGIAdapter1> adapter_;
    ComPtr<IDXGIOutput6> output_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    IDirect3DDevice directDevice_{nullptr};
    GraphicsCaptureItem item_{nullptr};
    Direct3D11CaptureFramePool pool_{nullptr};
    std::uint32_t poolBufferCount_ = 0;
    GraphicsCaptureSession session_{nullptr};
    winrt::Windows::Foundation::IAsyncOperation<AppCapabilityAccessStatus> borderlessRequest_{nullptr};
    D3dRoiRing ring_;
    DeferredGpuRetirement retirement_;
    winrt::event_token frameToken_{};
    winrt::event_token closedToken_{};
    bool frameRegistered_ = false;
    bool closedRegistered_ = false;
    bool initializedBefore_ = false;
    CO_MTA_USAGE_COOKIE mtaUsage_ = nullptr;
};

}

std::unique_ptr<CaptureBackend> MakeNativeCaptureBackend(const NativeCaptureOptions& options)
{
    return std::make_unique<NativeCaptureBackend>(options);
}

} // namespace pbscreencapturewgc::detail
