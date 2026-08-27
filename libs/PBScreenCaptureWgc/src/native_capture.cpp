#include "d3d_roi_ring.h"
#include "session_capabilities.h"

#include <appmodel.h>
#include <dxgi1_6.h>
#include <roapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
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

void ReadOutputState(IDXGIOutput6& output, CaptureEnvironment& environment)
{
    DXGI_OUTPUT_DESC1 description{};
    winrt::check_hresult(output.GetDesc1(&description));
    environment.outputColorSpace = static_cast<std::uint32_t>(description.ColorSpace);
    environment.bitsPerColor = description.BitsPerColor;
    environment.hdr = description.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
                      description.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    winrt::check_bool(EnumDisplaySettingsExW(description.DeviceName, ENUM_CURRENT_SETTINGS, &mode, 0));
    environment.displayFrequency = mode.dmDisplayFrequency;
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
    CallbackGate(std::shared_ptr<FrameInbox> input, const std::uint64_t epoch)
        : inbox(std::move(input)), captureEpoch(epoch), idleEvent(CreateEventW(nullptr, TRUE, TRUE, nullptr))
    {
        winrt::check_bool(idleEvent != nullptr);
    }
    ~CallbackGate()
    {
        CloseHandle(idleEvent);
    }
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
    HANDLE idleEvent = nullptr;

private:
    mutable std::mutex mutex_;
    bool enabled_ = true;
    bool active_ = false;
};

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
            UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
            if (options_.debugLayer)
            {
                flags |= D3D11_CREATE_DEVICE_DEBUG;
            }
            const D3D_FEATURE_LEVEL requested[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
            D3D_FEATURE_LEVEL actual{};
            winrt::check_hresult(D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, requested, 2, D3D11_SDK_VERSION,
                                                  &device_, &actual, &context_));
            // Windows 10 baseline completion events make shutdown timeout safe even
            // when a driver cannot supply fences. Probe before any frame is submitted.
            winrt::check_hresult(device_.As(&device4_));
            winrt::check_hresult(context_.As(&context3_));
            retirementEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            winrt::check_bool(retirementEvent_ != nullptr);
            winrt::check_hresult(device4_->RegisterDeviceRemovedEvent(retirementEvent_, &removedCookie_));
            removedRegistered_ = true;
            retirementWait_ = CreateThreadpoolWait(OnRetired, this, nullptr);
            winrt::check_bool(retirementWait_ != nullptr);
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
            gate_ = std::make_shared<CallbackGate>(inbox_, epoch);
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
            ReadOutputState(*output_.Get(), currentOutput);
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
            const winrt::Windows::Graphics::SizeInt32 size{environment.contentSize.width, environment.contentSize.height};
            ReadOutputState(*output_.Get(), environment);
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
        if (removedRegistered_)
        {
            device4_->UnregisterDeviceRemoved(removedCookie_);
            removedRegistered_ = false;
        }
        if (retirementWait_ != nullptr)
        {
            CloseThreadpoolWait(retirementWait_);
            retirementWait_ = nullptr;
        }
        if (retirementEvent_ != nullptr)
        {
            CloseHandle(retirementEvent_);
            retirementEvent_ = nullptr;
        }
        context3_.Reset();
        device4_.Reset();
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
        deferredOwner_ = std::move(owner);
        // Flush1's event (or the registered device-removed event) is a retirement
        // proof, unlike Flush alone. Everything was allocated before StartCapture.
        context3_->Flush1(D3D11_CONTEXT_TYPE_ALL, retirementEvent_);
        SetThreadpoolWait(retirementWait_, retirementEvent_, nullptr);
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
        for (UINT adapterIndex = 0; adapterIndex < 64; adapterIndex++)
        {
            ComPtr<IDXGIAdapter1> adapter;
            const HRESULT result = factory_->EnumAdapters1(adapterIndex, &adapter);
            if (result == DXGI_ERROR_NOT_FOUND)
            {
                break;
            }
            winrt::check_hresult(result);
            for (UINT outputIndex = 0; outputIndex < 64; outputIndex++)
            {
                ComPtr<IDXGIOutput> output;
                const HRESULT outputResult = adapter->EnumOutputs(outputIndex, &output);
                if (outputResult == DXGI_ERROR_NOT_FOUND)
                {
                    break;
                }
                winrt::check_hresult(outputResult);
                DXGI_OUTPUT_DESC description{};
                winrt::check_hresult(output->GetDesc(&description));
                if (description.Monitor != environment_.region.monitor || !description.AttachedToDesktop)
                {
                    continue;
                }
                DXGI_ADAPTER_DESC1 adapterDescription{};
                winrt::check_hresult(adapter->GetDesc1(&adapterDescription));
                if ((adapterDescription.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
                {
                    return CaptureStatus::Failure(CaptureError::Unsupported, CaptureStage::Adapter);
                }
                winrt::check_hresult(output.As(&output_));
                ReadOutputState(*output_.Get(), environment_);
                if (environment_.hdr && config_.pixelFormat != DXGI_FORMAT_R16G16B16A16_FLOAT)
                {
                    return CaptureStatus::Failure(CaptureError::Unsupported, CaptureStage::Adapter);
                }
                environment_.adapterLuid = adapterDescription.AdapterLuid;
                adapter_ = adapter;
                return {};
            }
        }
        return CaptureStatus::Failure(CaptureError::RegionChanged, CaptureStage::Adapter);
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

    static void CALLBACK OnRetired(PTP_CALLBACK_INSTANCE, void* context, PTP_WAIT, const TP_WAIT_RESULT result) noexcept
    {
        auto* const backend = static_cast<NativeCaptureBackend*>(context);
        if (result != WAIT_OBJECT_0)
        {
            return;
        }
        if (backend->gate_ && !backend->gate_->Idle())
        {
            // The GPU is retired, but a pre-pause acquisition can still be in
            // WinRT. The disabled gate can become idle only once. Rearm the
            // preallocated wait instead of blocking a thread-pool worker.
            SetThreadpoolWait(backend->retirementWait_, backend->gate_->idleEvent, nullptr);
            return;
        }
        const HRESULT apartment = RoInitialize(RO_INIT_MULTITHREADED);
        if (FAILED(apartment))
        {
            // Preserve the bounded owner on an OS cleanup failure; never report
            // completion or destroy WinRT objects on an uninitialized apartment.
            return;
        }
        {
            const auto owner = std::move(backend->deferredOwner_);
            owner->CompleteDeferredShutdown();
        }
        RoUninitialize();
    }

    const NativeCaptureOptions options_;
    WgcCaptureConfig config_;
    CaptureEnvironment environment_;
    CaptureCapabilities capabilities_;
    std::shared_ptr<FrameInbox> inbox_;
    std::shared_ptr<CallbackGate> gate_;
    std::shared_ptr<DeferredCleanup> deferredOwner_;
    ComPtr<IDXGIFactory1> factory_;
    ComPtr<IDXGIAdapter1> adapter_;
    ComPtr<IDXGIOutput6> output_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11Device4> device4_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11DeviceContext3> context3_;
    IDirect3DDevice directDevice_{nullptr};
    GraphicsCaptureItem item_{nullptr};
    Direct3D11CaptureFramePool pool_{nullptr};
    GraphicsCaptureSession session_{nullptr};
    winrt::Windows::Foundation::IAsyncOperation<AppCapabilityAccessStatus> borderlessRequest_{nullptr};
    D3dRoiRing ring_;
    winrt::event_token frameToken_{};
    winrt::event_token closedToken_{};
    bool frameRegistered_ = false;
    bool closedRegistered_ = false;
    bool initializedBefore_ = false;
    bool removedRegistered_ = false;
    DWORD removedCookie_ = 0;
    HANDLE retirementEvent_ = nullptr;
    PTP_WAIT retirementWait_ = nullptr;
    CO_MTA_USAGE_COOKIE mtaUsage_ = nullptr;
};

}

std::unique_ptr<CaptureBackend> MakeNativeCaptureBackend(const NativeCaptureOptions& options)
{
    return std::make_unique<NativeCaptureBackend>(options);
}

} // namespace pbscreencapturewgc::detail
