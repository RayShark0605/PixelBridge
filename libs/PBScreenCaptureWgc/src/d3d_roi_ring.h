#pragma once

#include "capture_internal.h"

#include <d3d11_4.h>
#include <d3d11sdklayers.h>
#include <wrl/client.h>

namespace pbscreencapturewgc::detail
{

class D3dRoiRing
{
public:
    [[nodiscard]] CaptureStatus Initialize(ID3D11Device* device, ID3D11DeviceContext* context, const WgcCaptureConfig& config,
                                           const CaptureEnvironment& environment, bool forceQuery) noexcept;
    [[nodiscard]] CaptureStatus Recreate(const WgcCaptureConfig& config, const CaptureEnvironment& environment) noexcept;
    [[nodiscard]] CaptureStatus Copy(const FrameLease& frame, std::size_t slot, bool& submitted) noexcept;
    [[nodiscard]] CaptureStatus Consume(RoiConsumer& consumer, const RoiFrameMetadata& metadata, std::size_t slot) noexcept;
    [[nodiscard]] CompletionResult Poll(std::size_t slot) noexcept;
    [[nodiscard]] bool DeviceRemoved() const noexcept;
    [[nodiscard]] bool UsesFence() const noexcept;
    [[nodiscard]] CaptureStatus CheckDebug() noexcept;
    void Reset() noexcept;

private:
    struct Slot
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        // Also the emergency retirement proof if a fence Signal fails AFTER copy.
        Microsoft::WRL::ComPtr<ID3D11Query> query;
        std::uint64_t fenceValue = 0;
        bool pending = false;
    };
    [[nodiscard]] CaptureStatus Mark(std::size_t slot) noexcept;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context4_;
    Microsoft::WRL::ComPtr<ID3D11Fence> fence_;
    Microsoft::WRL::ComPtr<ID3D11InfoQueue> infoQueue_;
    std::array<Slot, maximumRoiTextures> slots_;
    CaptureEnvironment environment_;
    CaptureLayout layout_;
    std::size_t slotCount_ = 0;
    std::uint64_t nextFenceValue_ = 0;
};

}
