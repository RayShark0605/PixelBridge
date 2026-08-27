#include "d3d_roi_ring.h"

#include <algorithm>
#include <limits>

namespace pbscreencapturewgc::detail
{
using Microsoft::WRL::ComPtr;

CaptureStatus D3dRoiRing::Initialize(ID3D11Device* const device, ID3D11DeviceContext* const context, const WgcCaptureConfig& config,
                                    const CaptureEnvironment& environment, const bool forceQuery) noexcept
{
    if (device_ || context_ || device == nullptr || context == nullptr || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
    {
        return CaptureStatus::Failure(CaptureError::InvalidConfiguration, CaptureStage::Device);
    }
    ComPtr<ID3D11Device> contextDevice;
    context->GetDevice(&contextDevice);
    if (contextDevice.Get() != device)
    {
        return CaptureStatus::Failure(CaptureError::InvalidConfiguration, CaptureStage::Device);
    }
    device_ = device;
    context_ = context;
    static_cast<void>(device_->QueryInterface(IID_PPV_ARGS(&infoQueue_)));
    if (infoQueue_)
    {
        infoQueue_->SetMessageCountLimit(128);
    }
    if (!forceQuery)
    {
        ComPtr<ID3D11Device5> device5;
        if (SUCCEEDED(device_.As(&device5)) && SUCCEEDED(context_.As(&context4_)))
        {
            const HRESULT result = device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
            if (FAILED(result))
            {
                fence_.Reset();
                context4_.Reset();
            }
        }
    }
    return Recreate(config, environment);
}

CaptureStatus D3dRoiRing::Recreate(const WgcCaptureConfig& config, const CaptureEnvironment& environment) noexcept
{
    if (!device_ || environment.pixelFormat != config.pixelFormat)
    {
        return CaptureStatus::Failure(CaptureError::InvalidConfiguration, CaptureStage::TextureRing);
    }
    CaptureLayout layout;
    const auto validation = ValidateLayout(config, environment, layout);
    if (!validation)
    {
        return validation;
    }
    if (std::ranges::any_of(slots_, &Slot::pending))
    {
        return CaptureStatus::Failure(CaptureError::InternalError, CaptureStage::Recreate);
    }
    // Recreate is only allowed after drain. Do not allocate a second whole ring
    // beside the old one and silently exceed the configured resident quota.
    slots_ = {};
    slotCount_ = 0;
    std::array<Slot, maximumRoiTextures> candidate;
    D3D11_TEXTURE2D_DESC description{};
    description.Width = layout.roiWidth;
    description.Height = layout.roiHeight;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = config.pixelFormat;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_QUERY_DESC queryDescription{D3D11_QUERY_EVENT, 0};
    for (std::size_t index = 0; index < config.roiTextureCount; index++)
    {
        HRESULT result = device_->CreateTexture2D(&description, nullptr, &candidate[index].texture);
        if (SUCCEEDED(result))
        {
            result = device_->CreateQuery(&queryDescription, &candidate[index].query);
        }
        if (FAILED(result))
        {
            return FromHresult(result, CaptureStage::TextureRing);
        }
    }
    slots_.swap(candidate);
    environment_ = environment;
    layout_ = layout;
    slotCount_ = config.roiTextureCount;
    return CheckDebug();
}

CaptureStatus D3dRoiRing::Mark(const std::size_t slotIndex) noexcept
{
    auto& slot = slots_[slotIndex];
    slot.pending = true;
    slot.fenceValue = 0;
    context_->End(slot.query.Get());
    CaptureStatus status;
    if (fence_)
    {
        if (nextFenceValue_ >= std::numeric_limits<std::uint64_t>::max() - 1)
        {
            status = CaptureStatus::Failure(CaptureError::ResourceLimit, CaptureStage::Completion);
        }
        else
        {
            nextFenceValue_++;
            const HRESULT result = context4_->Signal(fence_.Get(), nextFenceValue_);
            status = FromHresult(result, CaptureStage::Completion);
            if (status)
            {
                slot.fenceValue = nextFenceValue_;
            }
        }
    }
    // No Present drives this device. Flush submits work; only Poll proves completion.
    context_->Flush();
    return status ? CheckDebug() : status;
}

CaptureStatus D3dRoiRing::Copy(const FrameLease& frame, const std::size_t slotIndex, bool& submitted) noexcept
{
    submitted = false;
    if (slotIndex >= slotCount_ || slots_[slotIndex].pending || frame.contentSize != environment_.contentSize)
    {
        return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Copy);
    }
    ComPtr<ID3D11Texture2D> source;
    const HRESULT result = frame.GetTexture(&source);
    if (FAILED(result) || !source)
    {
        return FromHresult(FAILED(result) ? result : E_UNEXPECTED, CaptureStage::Surface);
    }
    D3D11_TEXTURE2D_DESC description{};
    source->GetDesc(&description);
    ComPtr<ID3D11Device> sourceDevice;
    source->GetDevice(&sourceDevice);
    if (sourceDevice.Get() != device_.Get() || description.Width != static_cast<UINT>(environment_.contentSize.width) ||
        description.Height != static_cast<UINT>(environment_.contentSize.height) || description.Format != environment_.pixelFormat ||
        description.SampleDesc.Count != 1 || description.SampleDesc.Quality != 0 || description.MipLevels != 1 || description.ArraySize != 1)
    {
        return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Surface);
    }
    submitted = true;
    context_->CopySubresourceRegion(slots_[slotIndex].texture.Get(), 0, 0, 0, 0, source.Get(), 0, &layout_.sourceBox);
    // source is released before the frame lease, which the owner retains until Poll.
    return Mark(slotIndex);
}

CaptureStatus D3dRoiRing::Consume(RoiConsumer& consumer, const RoiFrameMetadata& metadata, const std::size_t slotIndex) noexcept
{
    if (slotIndex >= slotCount_ || slots_[slotIndex].pending)
    {
        return CaptureStatus::Failure(CaptureError::InternalError, CaptureStage::Consumer);
    }
    CaptureStatus status;
    try
    {
        status = consumer.Submit(metadata, slots_[slotIndex].texture.Get(), context_.Get());
    }
    catch (...)
    {
        status = CaptureStatus::Failure(CaptureError::ConsumerFailure, CaptureStage::Consumer);
    }
    context_->ClearState();
    const auto retirement = Mark(slotIndex);
    return status ? retirement : status;
}

CompletionResult D3dRoiRing::Poll(const std::size_t slotIndex) noexcept
{
    if (slotIndex >= slotCount_)
    {
        return {CaptureStatus::Failure(CaptureError::InternalError, CaptureStage::Completion), false};
    }
    auto& slot = slots_[slotIndex];
    if (!slot.pending)
    {
        return {{}, true};
    }
    if (DeviceRemoved())
    {
        return {CaptureStatus::Failure(CaptureError::DeviceLost, CaptureStage::Completion, device_->GetDeviceRemovedReason()), false};
    }
    bool complete = false;
    if (slot.fenceValue != 0)
    {
        const auto completedValue = fence_->GetCompletedValue();
        if (completedValue == std::numeric_limits<std::uint64_t>::max())
        {
            return {CaptureStatus::Failure(CaptureError::DeviceLost, CaptureStage::Completion), false};
        }
        complete = completedValue >= slot.fenceValue;
    }
    else
    {
        BOOL completed = FALSE;
        const HRESULT result = context_->GetData(slot.query.Get(), &completed, sizeof(completed), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (FAILED(result))
        {
            return {FromHresult(result, CaptureStage::Completion), false};
        }
        complete = result == S_OK && completed;
    }
    if (complete)
    {
        slot.pending = false;
    }
    return {{}, complete};
}

bool D3dRoiRing::DeviceRemoved() const noexcept
{
    return device_ && FAILED(device_->GetDeviceRemovedReason());
}

bool D3dRoiRing::UsesFence() const noexcept
{
    return fence_ != nullptr;
}

CaptureStatus D3dRoiRing::CheckDebug() noexcept
{
    if (!infoQueue_)
    {
        return {};
    }
    bool error = infoQueue_->GetNumMessagesDiscardedByMessageCountLimit() != 0;
    const auto count = infoQueue_->GetNumStoredMessages();
    for (UINT64 index = 0; index < count; index++)
    {
        alignas(D3D11_MESSAGE) std::array<std::byte, 4096> buffer{};
        SIZE_T size = buffer.size();
        auto* const message = reinterpret_cast<D3D11_MESSAGE*>(buffer.data());
        const HRESULT result = infoQueue_->GetMessage(index, message, &size);
        error = error || FAILED(result) || message->Severity == D3D11_MESSAGE_SEVERITY_ERROR || message->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION;
    }
    infoQueue_->ClearStoredMessages();
    return error ? CaptureStatus::Failure(CaptureError::NativeFailure, CaptureStage::Completion) : CaptureStatus{};
}

void D3dRoiRing::Reset() noexcept
{
    slots_ = {};
    fence_.Reset();
    context4_.Reset();
    infoQueue_.Reset();
    context_.Reset();
    device_.Reset();
    slotCount_ = 0;
    nextFenceValue_ = 0;
}

}
