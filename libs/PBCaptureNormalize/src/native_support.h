#pragma once

#include "capture_runtime.h"

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

namespace pbcapturenormalize::detail
{

using DisplayModeReader = decltype(&EnumDisplaySettingsExW);

[[nodiscard]] HRESULT ReadOutputState(IDXGIOutput6& output, CaptureEnvironment& environment, DisplayModeReader readDisplayMode = EnumDisplaySettingsExW) noexcept;
[[nodiscard]] CaptureStatus FindCaptureAdapter(IDXGIFactory1* factory, const pbscreenregion::ScreenCaptureRegion& region,
                                               Microsoft::WRL::ComPtr<IDXGIAdapter1>& adapter, Microsoft::WRL::ComPtr<IDXGIOutput6>& output,
                                               CaptureEnvironment& environment, DisplayModeReader readDisplayMode = EnumDisplaySettingsExW) noexcept;
[[nodiscard]] CaptureStatus CreateCaptureDevice(IDXGIAdapter* adapter, bool debugLayer, Microsoft::WRL::ComPtr<ID3D11Device>& device,
                                                Microsoft::WRL::ComPtr<ID3D11DeviceContext>& context) noexcept;

// All handles are allocated before capture starts. The deferred owner retains
// this object and all GPU/source resources until the OS completion notification.
class DeferredGpuRetirement
{
public:
    DeferredGpuRetirement() = default;
    ~DeferredGpuRetirement();
    DeferredGpuRetirement(const DeferredGpuRetirement&) = delete;
    DeferredGpuRetirement& operator=(const DeferredGpuRetirement&) = delete;
    DeferredGpuRetirement(DeferredGpuRetirement&&) = delete;
    DeferredGpuRetirement& operator=(DeferredGpuRetirement&&) = delete;
    [[nodiscard]] CaptureStatus Initialize(ID3D11Device* device, ID3D11DeviceContext* context) noexcept;
    // One-shot after Initialize. owner retains this object and the disabled
    // producer's idle event until completion; no producer can become active again.
    void Defer(std::shared_ptr<DeferredCleanup> owner, HANDLE producerIdleEvent = nullptr) noexcept;
    // Only before Defer, or from its completion callback. Closing a wait does not
    // make external concurrent Reset safe against an already queued callback.
    void Reset() noexcept;
private:
    static void CALLBACK OnRetired(PTP_CALLBACK_INSTANCE instance, void* context, PTP_WAIT wait, TP_WAIT_RESULT result) noexcept;
    Microsoft::WRL::ComPtr<ID3D11Device4> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext3> context_;
    HANDLE event_ = nullptr;
    HANDLE producerIdleEvent_ = nullptr;
    PTP_WAIT wait_ = nullptr;
    DWORD removedCookie_ = 0;
    bool registered_ = false;
    std::shared_ptr<DeferredCleanup> owner_;
};

} // namespace pbcapturenormalize::detail
