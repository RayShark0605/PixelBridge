#pragma once

#include "pbcapturenormalize/diagnostic_readback.h"

#include <d3d11.h>
#include <memory>
#include <wrl/client.h>

namespace pbcapturenormalize::detail
{
using Microsoft::WRL::ComPtr;


// Private, per-instance OS-call boundary. No public fault switches, global
// replacement table, worker-thread GPU reference, or substitute capture path.
// Production calls the exact D3D11 operations through the default implementation.
class DiagnosticReadbackGraphicsApi
{
public:
    virtual ~DiagnosticReadbackGraphicsApi() = default;
    [[nodiscard]] virtual HRESULT CreateStaging(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& description,
                                               ID3D11Texture2D** output) noexcept = 0;
    // Maps a staging texture whose GPU work the caller already proved complete
    // (by fence, before calling). Prefers the non-blocking fast path.
    [[nodiscard]] virtual HRESULT MapStaging(ID3D11DeviceContext* context, ID3D11Texture2D* texture,
                                            D3D11_MAPPED_SUBRESOURCE& output, bool& usedBlockingRetry) noexcept = 0;
};

// Default implementation: the exact production D3D11 calls.
//
// Some D3D11 runtimes never clear the DO_NOT_WAIT fast-path not-ready state
// even after fence completion (observed on a hardware adapter: DO_NOT_WAIT
// returned DXGI_ERROR_WAS_STILL_DRAWING for every frame while a blocking Map
// on the same resource, immediately after fence completion, succeeded). The
// caller's fence proof bounds the GPU work, so exactly one blocking retry is
// bounded and safe. It runs only while the device has not been removed (never
// block on a dead device) and is reported through usedBlockingRetry so the
// caller counts it in telemetry instead of changing latency behavior silently.
class NativeReadbackGraphicsApi final : public DiagnosticReadbackGraphicsApi
{
public:
    HRESULT CreateStaging(ID3D11Device* const device, const D3D11_TEXTURE2D_DESC& description, ID3D11Texture2D** const output) noexcept override
    {
        return device->CreateTexture2D(&description, nullptr, output);
    }

    HRESULT MapStaging(ID3D11DeviceContext* const context, ID3D11Texture2D* const texture, D3D11_MAPPED_SUBRESOURCE& output,
                       bool& usedBlockingRetry) noexcept override
    {
        usedBlockingRetry = false;
        const HRESULT fastResult = context->Map(texture, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &output);
        if (fastResult != DXGI_ERROR_WAS_STILL_DRAWING)
        {
            return fastResult;
        }
        // ID3D11DeviceContext::GetDevice returns void (no failure HRESULT); the
        // owning device of a live context is always ID3D11Device, but still guard
        // a null result before touching it.
        ComPtr<ID3D11Device> device;
        context->GetDevice(&device);
        if (device.Get() == nullptr || FAILED(device->GetDeviceRemovedReason()))
        {
            return fastResult;
        }
        usedBlockingRetry = true;
        return context->Map(texture, 0, D3D11_MAP_READ, 0, &output);
    }
};

struct DiagnosticReadbackTestAccess
{
    // A null graphics boundary selects the real D3D11 implementation. The
    // normal public factory always uses that path; tests inject only HRESULTs
    // or malformed native return values, not normalized/decoded frame results.
    [[nodiscard]] static CaptureStatus Create(const DiagnosticReadbackConfig& config, std::shared_ptr<CpuFrameProcessor> processor,
                                              std::shared_ptr<DiagnosticReadbackGraphicsApi> graphics,
                                              std::shared_ptr<DiagnosticCpuReadback>& output) noexcept;
};

} // namespace pbcapturenormalize::detail
