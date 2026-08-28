#include "duplication_source.h"
#include "d3d_roi_ring.h"
#include "native_support.h"

#include <WtsApi32.h>
#include <algorithm>
#include <cwchar>
#include <new>

namespace pbscreencapturedxgi::detail
{
using Microsoft::WRL::ComPtr;

namespace
{

[[nodiscard]] bool SameRectangle(const RECT& left, const RECT& right) noexcept
{
    return left.left == right.left && left.top == right.top && left.right == right.right && left.bottom == right.bottom;
}

[[nodiscard]] bool SameRegion(const pbscreenregion::ScreenCaptureRegion& left, const pbscreenregion::ScreenCaptureRegion& right) noexcept
{
    return left.monitor == right.monitor && SameRectangle(left.physicalRect, right.physicalRect) &&
           SameRectangle(left.monitorPhysicalRect, right.monitorPhysicalRect) && left.rotation == right.rotation &&
           left.dpiX == right.dpiX && left.dpiY == right.dpiY;
}

struct DesktopIdentity
{
    std::array<wchar_t, 256> name{};
    DWORD error = ERROR_SUCCESS;
    bool operator==(const DesktopIdentity&) const = default;
};

[[nodiscard]] DesktopIdentity ReadInputDesktop() noexcept
{
    DesktopIdentity identity;
    const HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (desktop == nullptr)
    {
        identity.error = GetLastError();
        return identity;
    }
    DWORD requiredBytes = 0;
    if (!GetUserObjectInformationW(desktop, UOI_NAME, identity.name.data(), static_cast<DWORD>(sizeof(identity.name)), &requiredBytes))
    {
        identity.name = {};
        identity.error = GetLastError();
    }
    CloseDesktop(desktop);
    return identity;
}

// A hidden, never-shown top-level window observes display/session broadcasts.
// Message-only windows do not receive WM_DISPLAYCHANGE. The WinEvent callback
// posts to its registering owner thread and never dereferences backend state.
class EnvironmentWatch
{
public:
    [[nodiscard]] CaptureStatus Initialize() noexcept
    {
        if (window_ != nullptr)
        {
            return {};
        }
        ownerThread_ = GetCurrentThreadId();
        if (!classRegistered_)
        {
            if (swprintf_s(className_.data(), className_.size(), L"PixelBridge.DxgiEnvironment.%lu.%p", ownerThread_, static_cast<void*>(this)) < 0)
            {
                return CaptureStatus::Failure(CaptureError::InternalError, CaptureStage::Region);
            }
            WNDCLASSEXW description{};
            description.cbSize = sizeof(description);
            description.lpfnWndProc = WindowProcedure;
            description.hInstance = GetModuleHandleW(nullptr);
            description.lpszClassName = className_.data();
            if (RegisterClassExW(&description) == 0)
            {
                const DWORD error = GetLastError();
                return FromHresult(error == ERROR_SUCCESS ? E_FAIL : HRESULT_FROM_WIN32(error), CaptureStage::Region);
            }
            classRegistered_ = true;
        }
        window_ = CreateWindowExW(0, className_.data(), L"PixelBridge capture environment observer", WS_POPUP,
                                  0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), this);
        if (window_ == nullptr)
        {
            const DWORD error = GetLastError();
            return FromHresult(error == ERROR_SUCCESS ? E_FAIL : HRESULT_FROM_WIN32(error), CaptureStage::Region);
        }
        sessionRegistered_ = WTSRegisterSessionNotification(window_, NOTIFY_FOR_THIS_SESSION) != FALSE;
        desktopHook_ = SetWinEventHook(EVENT_SYSTEM_DESKTOPSWITCH, EVENT_SYSTEM_DESKTOPSWITCH, nullptr, OnDesktopSwitch, 0, 0, WINEVENT_OUTOFCONTEXT);
        // Desktop identity polling remains available if WinEvent/WTS hooks are
        // unavailable (for example while the remote session service restarts).
        return {};
    }

    [[nodiscard]] bool Poll() noexcept
    {
        bool changed = std::exchange(changed_, false);
        MSG message{};
        for (std::uint32_t count = 0; count < 32 && PeekMessageW(&message, window_, 0, 0, PM_REMOVE); count++)
        {
            DispatchMessageW(&message);
        }
        for (std::uint32_t count = 0; count < 32 && PeekMessageW(&message, nullptr, desktopSwitchMessage, desktopSwitchMessage, PM_REMOVE); count++)
        {
            changed = true;
        }
        const bool dispatchedChange = std::exchange(changed_, false);
        return changed || dispatchedChange;
    }

    [[nodiscard]] CaptureStatus Reset() noexcept
    {
        if (window_ == nullptr && desktopHook_ == nullptr && !classRegistered_)
        {
            return {};
        }
        if (ownerThread_ != GetCurrentThreadId())
        {
            return CaptureStatus::Failure(CaptureError::WrongThread, CaptureStage::Shutdown);
        }
        CaptureStatus status;
        if (desktopHook_ != nullptr)
        {
            if (UnhookWinEvent(desktopHook_))
            {
                desktopHook_ = nullptr;
            }
            else
            {
                const DWORD error = GetLastError();
                status = FromHresult(error == ERROR_SUCCESS ? E_FAIL : HRESULT_FROM_WIN32(error), CaptureStage::Shutdown);
            }
        }
        if (sessionRegistered_ && window_ != nullptr)
        {
            if (WTSUnRegisterSessionNotification(window_))
            {
                sessionRegistered_ = false;
            }
            else if (status)
            {
                const DWORD error = GetLastError();
                status = FromHresult(error == ERROR_SUCCESS ? E_FAIL : HRESULT_FROM_WIN32(error), CaptureStage::Shutdown);
            }
        }
        if (window_ != nullptr)
        {
            if (DestroyWindow(window_))
            {
                window_ = nullptr;
                sessionRegistered_ = false;
            }
            else if (status)
            {
                const DWORD error = GetLastError();
                status = FromHresult(error == ERROR_SUCCESS ? E_FAIL : HRESULT_FROM_WIN32(error), CaptureStage::Shutdown);
            }
        }
        if (classRegistered_ && window_ == nullptr)
        {
            if (UnregisterClassW(className_.data(), GetModuleHandleW(nullptr)))
            {
                classRegistered_ = false;
            }
            else if (status)
            {
                const DWORD error = GetLastError();
                status = FromHresult(error == ERROR_SUCCESS ? E_FAIL : HRESULT_FROM_WIN32(error), CaptureStage::Shutdown);
            }
        }
        MSG message{};
        for (std::uint32_t count = 0; count < 32 && PeekMessageW(&message, nullptr, desktopSwitchMessage, desktopSwitchMessage, PM_REMOVE); count++)
        {
        }
        return status;
    }

private:
    static constexpr UINT desktopSwitchMessage = WM_APP + 0x39b;
    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wordParameter, LPARAM longParameter) noexcept
    {
        auto* watch = reinterpret_cast<EnvironmentWatch*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            const auto* const creation = reinterpret_cast<const CREATESTRUCTW*>(longParameter);
            if (creation == nullptr || creation->lpCreateParams == nullptr)
            {
                return FALSE;
            }
            watch = static_cast<EnvironmentWatch*>(creation->lpCreateParams);
            SetLastError(ERROR_SUCCESS);
            if (SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(watch)) == 0 && GetLastError() != ERROR_SUCCESS)
            {
                return FALSE;
            }
        }
        if (watch != nullptr && (message == WM_DISPLAYCHANGE || message == WM_DEVICECHANGE || message == WM_SETTINGCHANGE || message == WM_WTSSESSION_CHANGE))
        {
            // Sent broadcasts are dispatched internally by PeekMessage, not
            // returned as MSGs. Record them here as well as posted messages.
            watch->changed_ = true;
        }
        if (message == WM_NCDESTROY)
        {
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        }
        return DefWindowProcW(window, message, wordParameter, longParameter);
    }
    static void CALLBACK OnDesktopSwitch(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) noexcept
    {
        // WINEVENT_OUTOFCONTEXT is dispatched on the registering thread.
        PostThreadMessageW(GetCurrentThreadId(), desktopSwitchMessage, 0, 0);
    }
    HWND window_ = nullptr;
    std::array<wchar_t, 96> className_{};
    HWINEVENTHOOK desktopHook_ = nullptr;
    DWORD ownerThread_ = 0;
    bool sessionRegistered_ = false;
    bool classRegistered_ = false;
    bool changed_ = false;
};

class NativeDuplicationApi final : public DuplicationApi
{
public:
    explicit NativeDuplicationApi(ComPtr<IDXGIOutputDuplication> duplication) noexcept : duplication_(std::move(duplication))
    {
    }
    HRESULT AcquireNextFrame(const std::uint32_t timeoutMilliseconds, DXGI_OUTDUPL_FRAME_INFO& info, ComPtr<IDXGIResource>& resource) noexcept override
    {
        return duplication_->AcquireNextFrame(timeoutMilliseconds, &info, resource.ReleaseAndGetAddressOf());
    }
    HRESULT GetFramePointerShape(const std::span<std::byte> buffer, std::uint32_t& requiredBytes, DXGI_OUTDUPL_POINTER_SHAPE_INFO& shape) noexcept override
    {
        return duplication_->GetFramePointerShape(static_cast<UINT>(buffer.size()), buffer.data(), &requiredBytes, &shape);
    }
    HRESULT ReleaseFrame() noexcept override
    {
        return duplication_->ReleaseFrame();
    }
private:
    ComPtr<IDXGIOutputDuplication> duplication_;
};

class NativeDxgiBackend final : public CaptureBackend
{
public:
    explicit NativeDxgiBackend(const NativeDxgiOptions& options) noexcept : options_(options)
    {
        environment_.backendKind = CaptureBackendKind::Dxgi;
    }
    ~NativeDxgiBackend() override
    {
        static_cast<void>(Shutdown());
    }

    CaptureBackendKind Kind() const noexcept override
    {
        return CaptureBackendKind::Dxgi;
    }

    CaptureStatus Initialize(const CaptureConfig& config, std::shared_ptr<FrameInbox> inbox) noexcept override
    {
        config_ = config;
        inbox_ = std::move(inbox);
        active_ = false;
        waiting_ = false;
        waitingNativeError_ = 0;
        observedFactoryCurrent_ = false;
        capabilities_ = {};
        retryBudget_.RecordAttempt(EnvironmentRetryBudget::Clock::now());
        auto status = ValidateDxgiCaptureConfig(config_);
        if (!status)
        {
            return status;
        }
        pbscreenregion::ScreenCaptureRegion resolved;
        const auto regionStatus = pbscreenregion::ResolveScreenCaptureRegion(config_.region.physicalRect, resolved);
        if (regionStatus.code == pbscreenregion::ScreenRegionErrorCode::DpiAwarenessRequired)
        {
            return CaptureStatus::Failure(CaptureError::DpiAwarenessRequired, CaptureStage::Region, regionStatus.nativeError);
        }
        status = watch_.Initialize();
        if (!status)
        {
            return status;
        }
        observedDesktop_ = ReadInputDesktop();
        observedRegionAvailable_ = static_cast<bool>(regionStatus);
        observedRegion_ = resolved;
        if (!regionStatus || resolved.monitor != config_.region.monitor)
        {
            return WaitForEnvironment(regionStatus.nativeError == 0 ? DXGI_ERROR_NOT_CURRENTLY_AVAILABLE : regionStatus.nativeError);
        }
        if (!initializedBefore_ && !SameRegion(resolved, config_.region))
        {
            return CaptureStatus::Failure(CaptureError::RegionChanged, CaptureStage::Region);
        }
        environment_.region = resolved;
        environment_.backendKind = CaptureBackendKind::Dxgi;
        environment_.contentSize = {static_cast<std::int32_t>(static_cast<std::int64_t>(resolved.monitorPhysicalRect.right) - resolved.monitorPhysicalRect.left),
                                    static_cast<std::int32_t>(static_cast<std::int64_t>(resolved.monitorPhysicalRect.bottom) - resolved.monitorPhysicalRect.top)};
        environment_.pixelFormat = config_.pixelFormat;
        // A same-monitor mode/rotation change can outgrow the original valid
        // selection. Recheck current geometry before creating any GPU surface.
        status = ValidateDuplicationPreflight(config_, resolved);
        if (!status)
        {
            return status;
        }
        HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory_));
        if (FAILED(result))
        {
            return FromHresult(result, CaptureStage::Adapter);
        }
        observedFactoryCurrent_ = factory_->IsCurrent() != FALSE;
        status = FindCaptureAdapter(factory_.Get(), resolved, adapter_, output_, environment_);
        if (!status)
        {
            return IsEnvironmentUnavailable(status.nativeError) || status.code == CaptureError::RegionChanged ? WaitForEnvironment(status.nativeError) : status;
        }
        result = ReadOutputState(*output_.Get(), environment_);
        if (FAILED(result))
        {
            return IsEnvironmentUnavailable(result) ? WaitForEnvironment(result) : FromHresult(result, CaptureStage::Adapter);
        }
        observedOutput_ = environment_;
        status = Fault(CaptureStage::Adapter);
        if (!status)
        {
            return status;
        }
        status = CreateCaptureDevice(adapter_.Get(), options_.debugLayer, device_, context_);
        if (!status)
        {
            return status;
        }
        status = retirement_.Initialize(device_.Get(), context_.Get());
        if (!status)
        {
            return status;
        }
        status = Fault(CaptureStage::Device);
        if (!status)
        {
            return status;
        }
        DuplicationFormatPlan formatPlan;
        status = BuildDuplicationFormatPlan(config_.pixelFormat, environment_.hdr, formatPlan);
        if (!status)
        {
            return status;
        }
        std::array<UINT, 3> formatSupport{};
        for (std::size_t index = 0; index < formatPlan.formatCount; index++)
        {
            result = device_->CheckFormatSupport(formatPlan.formats[index], &formatSupport[index]);
            if (FAILED(result))
            {
                return FromHresult(result, CaptureStage::CaptureItem);
            }
        }
        status = FilterDuplicationFormatPlan(formatPlan, formatSupport, formatPlan);
        if (!status)
        {
            return status;
        }
        formatPlan.allowLegacy = formatPlan.allowLegacy && environment_.bitsPerColor > 0 && environment_.bitsPerColor <= 8;
        ComPtr<IDXGIOutput5> output5;
        ComPtr<IDXGIOutputDuplication> duplication;
        result = output_.As(&output5);
        bool modernDuplication = SUCCEEDED(result);
        if (modernDuplication)
        {
            result = output5->DuplicateOutput1(device_.Get(), 0, formatPlan.formatCount, formatPlan.formats.data(), &duplication);
        }
        if (FAILED(result) && MayUseLegacyDuplication(formatPlan, result))
        {
            modernDuplication = false;
            result = output_->DuplicateOutput(device_.Get(), &duplication);
        }
        if (FAILED(result))
        {
            if (IsEnvironmentUnavailable(result))
            {
                return WaitForEnvironment(result);
            }
            return result == E_NOINTERFACE || result == E_NOTIMPL ? CaptureStatus::Failure(CaptureError::Unsupported, CaptureStage::CaptureItem, result) :
                FromHresult(result, CaptureStage::CaptureItem);
        }
        DXGI_OUTDUPL_DESC description{};
        duplication->GetDesc(&description);
        if (std::find(formatPlan.formats.begin(), formatPlan.formats.begin() + formatPlan.formatCount, description.ModeDesc.Format) ==
            formatPlan.formats.begin() + formatPlan.formatCount)
        {
            return CaptureStatus::Failure(CaptureError::Unsupported, CaptureStage::CaptureItem);
        }
        status = ResolveDuplicationEnvironment(config_, description, modernDuplication, environment_);
        if (!status)
        {
            return status.code == CaptureError::RegionChanged ? WaitForEnvironment(DXGI_ERROR_ACCESS_LOST) : status;
        }
        LARGE_INTEGER frequency{};
        if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
        {
            return CaptureStatus::Failure(CaptureError::NativeFailure, CaptureStage::Callback, HRESULT_FROM_WIN32(GetLastError()));
        }
        try
        {
            status = source_.Initialize(std::make_unique<NativeDuplicationApi>(std::move(duplication)), inbox_, environment_, frequency.QuadPart);
        }
        catch (const std::bad_alloc&)
        {
            return CaptureStatus::Failure(CaptureError::OutOfMemory, CaptureStage::CaptureItem);
        }
        catch (...)
        {
            return CaptureStatus::Failure(CaptureError::InternalError, CaptureStage::CaptureItem);
        }
        if (!status)
        {
            return status;
        }
        status = Fault(CaptureStage::CaptureItem);
        if (!status)
        {
            return status;
        }
        auto actualConfig = config_;
        actualConfig.pixelFormat = environment_.pixelFormat;
        status = ring_.Initialize(device_.Get(), context_.Get(), actualConfig, environment_, options_.forceQuery);
        if (!status)
        {
            return status;
        }
        capabilities_.fenceRetirement = ring_.UsesFence();
        status = Fault(CaptureStage::TextureRing);
        if (!status)
        {
            return status;
        }
        initializedBefore_ = true;
        return {};
    }

    CaptureStatus Start(const std::uint64_t epoch) noexcept override
    {
        source_.Start(epoch);
        active_ = !waiting_;
        return Fault(CaptureStage::Session);
    }

    CaptureStatus Acquire() noexcept override
    {
        if (!active_ || waiting_)
        {
            return {};
        }
        const auto status = source_.Acquire();
        if (source_.AccessLost())
        {
            lastRebuildReason_ = CaptureRebuildReason::AccessLost;
            waitingNativeError_ = DXGI_ERROR_ACCESS_LOST;
        }
        return status;
    }

    CaptureStatus Pause() noexcept override
    {
        active_ = false;
        // HWND/hook cleanup happens on the owner before a potential deferred
        // GPU shutdown hands resource ownership to a threadpool callback.
        return watch_.Reset();
    }

    bool CallbacksIdle() const noexcept override
    {
        // Acquire is synchronous/nonblocking on the shared owner. WinEvent
        // callbacks do not acquire frames or access any resource in this object.
        return true;
    }

    bool WaitingForEnvironment() const noexcept override
    {
        return waiting_;
    }

    CaptureStatus CheckEnvironment(bool& changed) noexcept override
    {
        changed = false;
        if (source_.AccessLost())
        {
            // Do not let a secondary stale-output query hide an authoritative
            // Acquire/metadata/Release ACCESS_LOST notification.
            lastRebuildReason_ = CaptureRebuildReason::AccessLost;
            waitingNativeError_ = DXGI_ERROR_ACCESS_LOST;
            changed = true;
            return {};
        }
        bool newEnvironment = watch_.Poll();
        const auto desktop = ReadInputDesktop();
        const bool desktopChanged = desktop != observedDesktop_;
        newEnvironment = newEnvironment || desktop != observedDesktop_;
        observedDesktop_ = desktop;
        pbscreenregion::ScreenCaptureRegion current;
        const auto regionStatus = pbscreenregion::ResolveScreenCaptureRegion(config_.region.physicalRect, current);
        const bool available = static_cast<bool>(regionStatus);
        newEnvironment = newEnvironment || available != observedRegionAvailable_ || (available && !SameRegion(current, observedRegion_));
        observedRegionAvailable_ = available;
        observedRegion_ = current;
        const bool factoryCurrent = factory_ && factory_->IsCurrent() != FALSE;
        newEnvironment = newEnvironment || factoryCurrent != observedFactoryCurrent_;
        observedFactoryCurrent_ = factoryCurrent;
        if (output_ && factoryCurrent && available && current.monitor == environment_.region.monitor)
        {
            auto currentOutput = environment_;
            const HRESULT result = ReadOutputState(*output_.Get(), currentOutput);
            if (SUCCEEDED(result))
            {
                newEnvironment = newEnvironment || currentOutput.hdr != observedOutput_.hdr ||
                    currentOutput.outputColorSpace != observedOutput_.outputColorSpace || currentOutput.bitsPerColor != observedOutput_.bitsPerColor ||
                    currentOutput.displayFrequency != observedOutput_.displayFrequency;
                observedOutput_ = currentOutput;
            }
            else
            {
                const auto disposition = ClassifyOutputQueryResult(result, newEnvironment, waiting_);
                if (disposition == OutputQueryDisposition::Failure)
                {
                    return FromHresult(result, CaptureStage::Region);
                }
                newEnvironment = newEnvironment || disposition == OutputQueryDisposition::Rebuild;
            }
        }
        if (newEnvironment)
        {
            retryBudget_.Reset();
            lastRebuildReason_ = desktopChanged ? CaptureRebuildReason::DesktopChanged : CaptureRebuildReason::DisplayChanged;
        }
        if (waiting_)
        {
            changed = retryBudget_.CanRetry(EnvironmentRetryBudget::Clock::now());
        }
        else
        {
            changed = newEnvironment || !available || current.monitor != environment_.region.monitor || !factoryCurrent;
        }
        return {};
    }

    CaptureStatus Recreate(CaptureSize) noexcept override
    {
        if (source_.Outstanding())
        {
            return CaptureStatus::Failure(CaptureError::InternalError, CaptureStage::Recreate);
        }
        if (source_.AccessLost())
        {
            lastRebuildReason_ = CaptureRebuildReason::AccessLost;
            waitingNativeError_ = DXGI_ERROR_ACCESS_LOST;
        }
        const auto faultStatus = Fault(CaptureStage::Recreate);
        if (!faultStatus)
        {
            return faultStatus;
        }
        const auto status = Shutdown();
        if (!status)
        {
            return status;
        }
        if (!retryBudget_.CanRetry(EnvironmentRetryBudget::Clock::now()))
        {
            const auto watchStatus = watch_.Initialize();
            if (!watchStatus)
            {
                return watchStatus;
            }
            observedFactoryCurrent_ = false;
            observedDesktop_ = ReadInputDesktop();
            return WaitForEnvironment(waitingNativeError_ == 0 ? DXGI_ERROR_NOT_CURRENTLY_AVAILABLE : waitingNativeError_);
        }
        return Initialize(config_, inbox_);
    }

    CaptureEnvironment GetEnvironment() const noexcept override
    {
        return environment_;
    }

    CaptureCapabilities GetCapabilities() const noexcept override
    {
        // A separate-pointer frame is safe to leave undrawn. That is not a
        // session-wide guarantee and must never upgrade an earlier raw frame.
        return capabilities_;
    }

    void UpdateSnapshot(CaptureSnapshot& snapshot) const noexcept override
    {
        source_.UpdateSnapshot(snapshot);
        snapshot.lastRebuildReason = source_.AccessLost() ? CaptureRebuildReason::AccessLost : lastRebuildReason_;
        snapshot.waitingNativeError = waiting_ ? waitingNativeError_ : 0;
        snapshot.environmentAttempts = retryBudget_.Attempts();
    }

    CaptureStatus NotifyEpoch(RawRoiConsumer& consumer, const std::uint64_t epoch) noexcept override
    {
        if (waiting_)
        {
            return {};
        }
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
        submitted = false;
        ComPtr<ID3D11Texture2D> texture;
        const HRESULT result = frame.GetTexture(&texture);
        if (FAILED(result))
        {
            return FromHresult(result, CaptureStage::Surface);
        }
        if (!texture)
        {
            return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Surface);
        }
        ComPtr<ID3D11Device> sourceDevice;
        texture->GetDevice(&sourceDevice);
        if (!sourceDevice || sourceDevice.Get() != device_.Get())
        {
            return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Surface);
        }
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        const bool validSurface = description.Width > 0 && description.Width <= 16384 && description.Height > 0 && description.Height <= 16384 &&
            description.ArraySize == 1 && description.MipLevels == 1 && description.SampleDesc.Count == 1 && description.SampleDesc.Quality == 0 &&
            (description.Format == DXGI_FORMAT_B8G8R8A8_UNORM || description.Format == DXGI_FORMAT_R10G10B10A2_UNORM || description.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
        if (validSurface && (description.Width != static_cast<UINT>(environment_.sourceSize.width) ||
            description.Height != static_cast<UINT>(environment_.sourceSize.height) || description.Format != environment_.pixelFormat))
        {
            lastRebuildReason_ = CaptureRebuildReason::SourceDescriptionChanged;
            inbox_->RequestRecreate();
            return {};
        }
        const auto status = ring_.Copy(frame, slot, submitted);
        if (status && submitted)
        {
            // A newly created duplication that immediately loses access, or
            // repeatedly supplies inconsistent descriptors, cannot reset retry limits.
            retryBudget_.Reset();
        }
        return status;
    }

    CaptureStatus Consume(RawRoiConsumer& consumer, const RawRoiFrameMetadata& metadata, const std::size_t slot) noexcept override
    {
        return ring_.Consume(consumer, metadata, slot);
    }

    CaptureStatus Complete(RawRoiConsumer& consumer, const RawRoiFrameMetadata& metadata, std::size_t, const bool cancelled) noexcept override
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
        return device_ && FAILED(device_->GetDeviceRemovedReason());
    }

    std::int32_t DeviceRemovalReason() const noexcept override
    {
        return device_ ? device_->GetDeviceRemovedReason() : S_OK;
    }

    CaptureStatus Shutdown() noexcept override
    {
        auto status = Pause();
        if (DeviceRemoved())
        {
            lastRebuildReason_ = CaptureRebuildReason::DeviceLost;
        }
        if (source_.Outstanding())
        {
            return CaptureStatus::Failure(CaptureError::InternalError, CaptureStage::Shutdown);
        }
        const auto sourceStatus = source_.Reset();
        if (status)
        {
            status = sourceStatus;
        }
        if (context_)
        {
            context_->ClearState();
        }
        const auto debugStatus = ring_.CheckDebug();
        if (status)
        {
            status = debugStatus;
        }
        ring_.Reset();
        retirement_.Reset();
        context_.Reset();
        device_.Reset();
        output_.Reset();
        adapter_.Reset();
        factory_.Reset();
        return status;
    }

    void DeferShutdown(std::shared_ptr<DeferredCleanup> owner) noexcept override
    {
        retirement_.Defer(std::move(owner));
    }

private:
    [[nodiscard]] CaptureStatus WaitForEnvironment(const HRESULT result) noexcept
    {
        waiting_ = true;
        active_ = false;
        waitingNativeError_ = result;
        if (lastRebuildReason_ == CaptureRebuildReason::None)
        {
            lastRebuildReason_ = CaptureRebuildReason::UnavailableEnvironment;
        }
        return {};
    }
    [[nodiscard]] CaptureStatus Fault(const CaptureStage stage) const noexcept
    {
        return options_.failAfterStage == stage ? CaptureStatus::Failure(CaptureError::NativeFailure, stage, E_FAIL) : CaptureStatus{};
    }
    const NativeDxgiOptions options_;
    CaptureConfig config_;
    CaptureEnvironment environment_;
    CaptureEnvironment observedOutput_;
    CaptureCapabilities capabilities_;
    pbscreenregion::ScreenCaptureRegion observedRegion_;
    DesktopIdentity observedDesktop_;
    std::shared_ptr<FrameInbox> inbox_;
    ComPtr<IDXGIFactory1> factory_;
    ComPtr<IDXGIAdapter1> adapter_;
    ComPtr<IDXGIOutput6> output_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    D3dRoiRing ring_;
    DeferredGpuRetirement retirement_;
    DuplicationFrameSource source_;
    EnvironmentWatch watch_;
    EnvironmentRetryBudget retryBudget_;
    bool active_ = false;
    bool waiting_ = false;
    bool initializedBefore_ = false;
    bool observedRegionAvailable_ = false;
    bool observedFactoryCurrent_ = false;
    CaptureRebuildReason lastRebuildReason_ = CaptureRebuildReason::None;
    HRESULT waitingNativeError_ = S_OK;
};

} // namespace

std::unique_ptr<CaptureBackend> MakeNativeDxgiBackend(const NativeDxgiOptions& options)
{
    return std::make_unique<NativeDxgiBackend>(options);
}

} // namespace pbscreencapturedxgi::detail
