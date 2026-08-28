#include "native_support.h"

#include <roapi.h>

namespace pbcapturenormalize::detail
{
using Microsoft::WRL::ComPtr;

HRESULT ReadOutputState(IDXGIOutput6& output, CaptureEnvironment& environment, const DisplayModeReader readDisplayMode) noexcept
{
    if (readDisplayMode == nullptr)
    {
        return E_INVALIDARG;
    }
    DXGI_OUTPUT_DESC1 description{};
    const HRESULT result = output.GetDesc1(&description);
    if (FAILED(result))
    {
        return result;
    }
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!readDisplayMode(description.DeviceName, ENUM_CURRENT_SETTINGS, &mode, 0))
    {
        const DWORD error = GetLastError();
        return error == ERROR_SUCCESS ? E_FAIL : HRESULT_FROM_WIN32(error);
    }
    environment.outputColorSpace = static_cast<std::uint32_t>(description.ColorSpace);
    environment.bitsPerColor = description.BitsPerColor;
    environment.hdr = description.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
                      description.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
    environment.displayFrequency = mode.dmDisplayFrequency;
    return S_OK;
}

CaptureStatus FindCaptureAdapter(IDXGIFactory1* const factory, const pbscreenregion::ScreenCaptureRegion& region,
                                ComPtr<IDXGIAdapter1>& adapter, ComPtr<IDXGIOutput6>& output, CaptureEnvironment& environment,
                                const DisplayModeReader readDisplayMode) noexcept
{
    if (factory == nullptr || region.monitor == nullptr || readDisplayMode == nullptr)
    {
        return CaptureStatus::Failure(CaptureError::InvalidConfiguration, CaptureStage::Adapter);
    }
    ComPtr<IDXGIAdapter1> selectedAdapter;
    ComPtr<IDXGIOutput6> selectedOutput;
    CaptureEnvironment candidate = environment;
    // The 65th query is a sentinel, not another admitted adapter/output. Exactly
    // 64 entries remain valid only when the sentinel reports NOT_FOUND.
    for (UINT adapterIndex = 0; adapterIndex <= 64; adapterIndex++)
    {
        ComPtr<IDXGIAdapter1> currentAdapter;
        HRESULT result = factory->EnumAdapters1(adapterIndex, &currentAdapter);
        if (result == DXGI_ERROR_NOT_FOUND)
        {
            break;
        }
        if (FAILED(result))
        {
            return FromHresult(result, CaptureStage::Adapter);
        }
        if (adapterIndex == 64)
        {
            return CaptureStatus::Failure(CaptureError::ResourceLimit, CaptureStage::Adapter);
        }
        for (UINT outputIndex = 0; outputIndex <= 64; outputIndex++)
        {
            ComPtr<IDXGIOutput> currentOutput;
            result = currentAdapter->EnumOutputs(outputIndex, &currentOutput);
            if (result == DXGI_ERROR_NOT_FOUND)
            {
                break;
            }
            if (FAILED(result))
            {
                return FromHresult(result, CaptureStage::Adapter);
            }
            if (outputIndex == 64)
            {
                return CaptureStatus::Failure(CaptureError::ResourceLimit, CaptureStage::Adapter);
            }
            DXGI_OUTPUT_DESC description{};
            result = currentOutput->GetDesc(&description);
            if (FAILED(result))
            {
                return FromHresult(result, CaptureStage::Adapter);
            }
            if (description.Monitor != region.monitor || !description.AttachedToDesktop)
            {
                continue;
            }
            const auto& rectangle = region.monitorPhysicalRect;
            if (selectedOutput || description.DesktopCoordinates.left != rectangle.left || description.DesktopCoordinates.top != rectangle.top ||
                description.DesktopCoordinates.right != rectangle.right || description.DesktopCoordinates.bottom != rectangle.bottom ||
                description.Rotation != region.rotation)
            {
                return CaptureStatus::Failure(CaptureError::RegionChanged, CaptureStage::Adapter);
            }
            DXGI_ADAPTER_DESC1 adapterDescription{};
            result = currentAdapter->GetDesc1(&adapterDescription);
            if (FAILED(result))
            {
                return FromHresult(result, CaptureStage::Adapter);
            }
            if ((adapterDescription.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
            {
                return CaptureStatus::Failure(CaptureError::Unsupported, CaptureStage::Adapter);
            }
            result = currentOutput.As(&selectedOutput);
            if (FAILED(result))
            {
                return FromHresult(result, CaptureStage::Adapter);
            }
            result = ReadOutputState(*selectedOutput.Get(), candidate, readDisplayMode);
            if (FAILED(result))
            {
                return FromHresult(result, CaptureStage::Adapter);
            }
            candidate.adapterLuid = adapterDescription.AdapterLuid;
            selectedAdapter = currentAdapter;
        }
    }
    if (!selectedOutput)
    {
        return CaptureStatus::Failure(CaptureError::RegionChanged, CaptureStage::Adapter);
    }
    adapter = std::move(selectedAdapter);
    output = std::move(selectedOutput);
    environment = candidate;
    return {};
}

CaptureStatus CreateCaptureDevice(IDXGIAdapter* const adapter, const bool debugLayer, ComPtr<ID3D11Device>& device,
                                 ComPtr<ID3D11DeviceContext>& context) noexcept
{
    if (adapter == nullptr)
    {
        return CaptureStatus::Failure(CaptureError::InvalidConfiguration, CaptureStage::Adapter);
    }
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | (debugLayer ? D3D11_CREATE_DEVICE_DEBUG : 0u);
    const D3D_FEATURE_LEVEL requested[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL actual{};
    return FromHresult(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, requested, 2, D3D11_SDK_VERSION,
                                       &device, &actual, &context), CaptureStage::Device);
}

DeferredGpuRetirement::~DeferredGpuRetirement()
{
    Reset();
}

CaptureStatus DeferredGpuRetirement::Initialize(ID3D11Device* const device, ID3D11DeviceContext* const context) noexcept
{
    if (device == nullptr || context == nullptr || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
    {
        return CaptureStatus::Failure(CaptureError::InvalidConfiguration, CaptureStage::Device);
    }
    ComPtr<ID3D11Device> contextDevice;
    context->GetDevice(&contextDevice);
    if (contextDevice.Get() != device)
    {
        return CaptureStatus::Failure(CaptureError::InvalidConfiguration, CaptureStage::Device);
    }
    Reset();
    HRESULT result = device->QueryInterface(IID_PPV_ARGS(&device_));
    if (SUCCEEDED(result))
    {
        result = context->QueryInterface(IID_PPV_ARGS(&context_));
    }
    if (FAILED(result))
    {
        return FromHresult(result, CaptureStage::Device);
    }
    event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event_ == nullptr)
    {
        return FromHresult(HRESULT_FROM_WIN32(GetLastError()), CaptureStage::Device);
    }
    result = device_->RegisterDeviceRemovedEvent(event_, &removedCookie_);
    if (FAILED(result))
    {
        return FromHresult(result, CaptureStage::Device);
    }
    registered_ = true;
    wait_ = CreateThreadpoolWait(OnRetired, this, nullptr);
    return wait_ != nullptr ? CaptureStatus{} : FromHresult(HRESULT_FROM_WIN32(GetLastError()), CaptureStage::Device);
}

void DeferredGpuRetirement::Defer(std::shared_ptr<DeferredCleanup> owner, const HANDLE producerIdleEvent) noexcept
{
    owner_ = std::move(owner);
    producerIdleEvent_ = producerIdleEvent;
    context_->Flush1(D3D11_CONTEXT_TYPE_ALL, event_);
    SetThreadpoolWait(wait_, event_, nullptr);
}

void DeferredGpuRetirement::Reset() noexcept
{
    if (registered_)
    {
        device_->UnregisterDeviceRemoved(removedCookie_);
        registered_ = false;
    }
    if (wait_ != nullptr)
    {
        CloseThreadpoolWait(std::exchange(wait_, nullptr));
    }
    if (event_ != nullptr)
    {
        CloseHandle(std::exchange(event_, nullptr));
    }
    producerIdleEvent_ = nullptr;
    context_.Reset();
    device_.Reset();
}

void CALLBACK DeferredGpuRetirement::OnRetired(PTP_CALLBACK_INSTANCE, void* const context, PTP_WAIT, const TP_WAIT_RESULT result) noexcept
{
    auto* const retirement = static_cast<DeferredGpuRetirement*>(context);
    if (result != WAIT_OBJECT_0)
    {
        return;
    }
    if (retirement->producerIdleEvent_ != nullptr && WaitForSingleObject(retirement->producerIdleEvent_, 0) != WAIT_OBJECT_0)
    {
        SetThreadpoolWait(retirement->wait_, retirement->producerIdleEvent_, nullptr);
        return;
    }
    const HRESULT apartment = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(apartment))
    {
        return;
    }
    {
        const auto owner = std::move(retirement->owner_);
        owner->CompleteDeferredShutdown();
    }
    // CompleteDeferredShutdown can destroy retirement; no subsequent access.
    RoUninitialize();
}

} // namespace pbcapturenormalize::detail
