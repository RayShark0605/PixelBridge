#pragma once

#include "capture_runtime.h"

#include <d3d11_4.h>
#include <d3d11sdklayers.h>
#include <wrl/client.h>

namespace pbcapturenormalize::detail
{

class D3dRoiRing
{
public:
    [[nodiscard]] CaptureStatus Initialize(ID3D11Device* device, ID3D11DeviceContext* context, const CaptureConfig& config,
                                           const CaptureEnvironment& environment, bool forceQuery) noexcept;
    [[nodiscard]] CaptureStatus Recreate(const CaptureConfig& config, const CaptureEnvironment& environment) noexcept;
    [[nodiscard]] CaptureStatus Copy(const FrameLease& frame, std::size_t slotIndex, bool& submitted) noexcept;
    [[nodiscard]] CaptureStatus Consume(RawRoiConsumer& consumer, const RawRoiFrameMetadata& metadata, std::size_t slotIndex) noexcept;
    [[nodiscard]] CaptureConsumerCompletion Complete(RawRoiConsumer& consumer, const RawRoiFrameMetadata& metadata, bool cancelled) noexcept;
    [[nodiscard]] CompletionResult Poll(std::size_t slotIndex) noexcept;
    [[nodiscard]] bool DeviceRemoved() const noexcept;
    [[nodiscard]] bool UsesFence() const noexcept;
    [[nodiscard]] CaptureStatus CheckDebug() noexcept;
    void Reset() noexcept;

private:
    struct Slot
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> targetView;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> scratch;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> scratchView;
        // Also the emergency retirement proof if a fence Signal fails AFTER copy.
        Microsoft::WRL::ComPtr<ID3D11Query> query;
        Microsoft::WRL::ComPtr<ID3D11Query> copyDisjoint;
        Microsoft::WRL::ComPtr<ID3D11Query> copyStart;
        Microsoft::WRL::ComPtr<ID3D11Query> copyEnd;
        std::uint64_t fenceValue = 0;
        bool pending = false;
        bool copyTimingPending = false;
    };
    [[nodiscard]] CaptureStatus CreateTransformPipeline(const CaptureConfig& config, const CaptureEnvironment& environment,
                                                        const CaptureLayout& layout) noexcept;
    void Transform(std::size_t slotIndex) noexcept;
    [[nodiscard]] CaptureStatus Mark(std::size_t slotIndex) noexcept;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context4_;
    Microsoft::WRL::ComPtr<ID3D11Fence> fence_;
    Microsoft::WRL::ComPtr<ID3D11InfoQueue> infoQueue_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> rotationVertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> rotationPixelShader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> rotationConstants_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rotationRasterizer_;
    std::array<Slot, maximumRoiTextures> slots_;
    CaptureEnvironment environment_;
    CaptureLayout layout_;
    DXGI_FORMAT outputPixelFormat_ = DXGI_FORMAT_UNKNOWN;
    std::size_t slotCount_ = 0;
    std::uint64_t nextFenceValue_ = 0;
};

}
