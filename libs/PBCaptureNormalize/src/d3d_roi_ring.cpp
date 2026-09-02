#include "d3d_roi_ring.h"

#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace pbcapturenormalize::detail
{
using Microsoft::WRL::ComPtr;

namespace
{
constexpr char rotationShader[] = R"hlsl(
cbuffer RotationParameters : register(b0)
{
    uint roiWidth;
    uint roiHeight;
    uint sourceRotation;
    uint conversionMode;
};
Texture2D<float4> sourceTexture : register(t0);

float4 VertexMain(uint vertexIndex : SV_VertexID) : SV_Position
{
    const float2 corner = float2((vertexIndex << 1) & 2, vertexIndex & 2);
    return float4(corner * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 PixelMain(float4 position : SV_Position) : SV_Target
{
    const uint2 pixel = uint2(position.xy);
    uint2 sourcePixel = pixel;
    if (sourceRotation == 2)
    {
        sourcePixel = uint2(pixel.y, roiWidth - 1 - pixel.x);
    }
    else if (sourceRotation == 3)
    {
        sourcePixel = uint2(roiWidth - 1 - pixel.x, roiHeight - 1 - pixel.y);
    }
    else if (sourceRotation == 4)
    {
        sourcePixel = uint2(roiHeight - 1 - pixel.y, pixel.x);
    }
    float4 color = sourceTexture.Load(int3(sourcePixel, 0));
    if (conversionMode == 1)
    {
        const float3 linearColor = saturate(color.rgb);
        const float3 lower = linearColor * 12.92;
        const float inverseGamma = 1.0 / 2.4;
        const float3 powered = float3(pow(linearColor.r, inverseGamma), pow(linearColor.g, inverseGamma), pow(linearColor.b, inverseGamma));
        const float3 upper = 1.055 * powered - 0.055;
        color.rgb = lerp(upper, lower, step(linearColor, 0.0031308));
        color.a = saturate(color.a);
    }
    return color;
}
)hlsl";
}

CaptureStatus D3dRoiRing::Initialize(ID3D11Device* const device, ID3D11DeviceContext* const context, const CaptureConfig& config,
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

CaptureStatus D3dRoiRing::CreateTransformPipeline(const CaptureConfig& config, const CaptureEnvironment& environment,
                                                  const CaptureLayout& layout) noexcept
{
    if (device_->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0)
    {
        return CaptureStatus::Failure(CaptureError::Unsupported, CaptureStage::TextureRing);
    }
    UINT sourceSupport = 0;
    HRESULT result = device_->CheckFormatSupport(environment.pixelFormat, &sourceSupport);
    if (FAILED(result))
    {
        return FromHresult(result, CaptureStage::TextureRing);
    }
    UINT outputSupport = 0;
    result = device_->CheckFormatSupport(config.pixelFormat, &outputSupport);
    if (FAILED(result))
    {
        return FromHresult(result, CaptureStage::TextureRing);
    }
    constexpr UINT requiredSourceSupport = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_LOAD;
    constexpr UINT requiredOutputSupport = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_RENDER_TARGET;
    if ((sourceSupport & requiredSourceSupport) != requiredSourceSupport || (outputSupport & requiredOutputSupport) != requiredOutputSupport)
    {
        return CaptureStatus::Failure(CaptureError::Unsupported, CaptureStage::TextureRing);
    }
    ComPtr<ID3DBlob> shaderCode;
    ComPtr<ID3DBlob> diagnostics;
    constexpr UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    result = D3DCompile(rotationShader, sizeof(rotationShader) - 1, nullptr, nullptr, nullptr, "VertexMain", "vs_5_0", compileFlags, 0, &shaderCode, &diagnostics);
    if (SUCCEEDED(result))
    {
        result = device_->CreateVertexShader(shaderCode->GetBufferPointer(), shaderCode->GetBufferSize(), nullptr, &rotationVertexShader_);
    }
    if (FAILED(result))
    {
        return FromHresult(result, CaptureStage::TextureRing);
    }
    shaderCode.Reset();
    diagnostics.Reset();
    result = D3DCompile(rotationShader, sizeof(rotationShader) - 1, nullptr, nullptr, nullptr, "PixelMain", "ps_5_0", compileFlags, 0, &shaderCode, &diagnostics);
    if (SUCCEEDED(result))
    {
        result = device_->CreatePixelShader(shaderCode->GetBufferPointer(), shaderCode->GetBufferSize(), nullptr, &rotationPixelShader_);
    }
    if (FAILED(result))
    {
        return FromHresult(result, CaptureStage::TextureRing);
    }
    const std::uint32_t conversionMode = environment.pixelFormat == DXGI_FORMAT_R16G16B16A16_FLOAT &&
        config.pixelFormat == DXGI_FORMAT_B8G8R8A8_UNORM ? 1U : 0U;
    const std::array<std::uint32_t, 4> parameters{layout.roiWidth, layout.roiHeight,
        static_cast<std::uint32_t>(environment.sourceRotation), conversionMode};
    D3D11_BUFFER_DESC bufferDescription{};
    bufferDescription.ByteWidth = static_cast<UINT>(sizeof(parameters));
    bufferDescription.Usage = D3D11_USAGE_IMMUTABLE;
    bufferDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    const D3D11_SUBRESOURCE_DATA initialData{parameters.data(), 0, 0};
    result = device_->CreateBuffer(&bufferDescription, &initialData, &rotationConstants_);
    if (SUCCEEDED(result))
    {
        D3D11_RASTERIZER_DESC rasterizerDescription{};
        rasterizerDescription.FillMode = D3D11_FILL_SOLID;
        rasterizerDescription.CullMode = D3D11_CULL_NONE;
        rasterizerDescription.DepthClipEnable = TRUE;
        result = device_->CreateRasterizerState(&rasterizerDescription, &rotationRasterizer_);
    }
    return FromHresult(result, CaptureStage::TextureRing);
}

CaptureStatus D3dRoiRing::Recreate(const CaptureConfig& config, const CaptureEnvironment& environment) noexcept
{
    if (!device_)
    {
        return CaptureStatus::Failure(CaptureError::InvalidConfiguration, CaptureStage::TextureRing);
    }
    const bool converting = environment.pixelFormat != config.pixelFormat;
    if (converting && (config.pixelFormat != DXGI_FORMAT_B8G8R8A8_UNORM || environment.hdr || environment.outputColorSpace != 0 ||
        (environment.pixelFormat != DXGI_FORMAT_R10G10B10A2_UNORM && environment.pixelFormat != DXGI_FORMAT_R16G16B16A16_FLOAT)))
    {
        return CaptureStatus::Failure(CaptureError::Unsupported, CaptureStage::TextureRing);
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
    rotationVertexShader_.Reset();
    rotationPixelShader_.Reset();
    rotationConstants_.Reset();
    rotationRasterizer_.Reset();
    const bool transformRequired = environment.sourceRotation != DXGI_MODE_ROTATION_IDENTITY || converting;
    if (transformRequired)
    {
        const auto pipelineStatus = CreateTransformPipeline(config, environment, layout);
        if (!pipelineStatus)
        {
            return pipelineStatus;
        }
    }
    std::array<Slot, maximumRoiTextures> candidate;
    D3D11_TEXTURE2D_DESC description{};
    description.Width = layout.roiWidth;
    description.Height = layout.roiHeight;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = config.pixelFormat;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE | (transformRequired ? D3D11_BIND_RENDER_TARGET : 0u);
    const D3D11_QUERY_DESC queryDescription{D3D11_QUERY_EVENT, 0};
    const D3D11_QUERY_DESC disjointDescription{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    const D3D11_QUERY_DESC timestampDescription{D3D11_QUERY_TIMESTAMP, 0};
    for (std::size_t index = 0; index < config.roiTextureCount; index++)
    {
        HRESULT result = device_->CreateTexture2D(&description, nullptr, &candidate[index].texture);
        if (SUCCEEDED(result) && transformRequired)
        {
            result = device_->CreateRenderTargetView(candidate[index].texture.Get(), nullptr, &candidate[index].targetView);
            if (SUCCEEDED(result))
            {
                auto scratchDescription = description;
                scratchDescription.Width = layout.sourceBox.right - layout.sourceBox.left;
                scratchDescription.Height = layout.sourceBox.bottom - layout.sourceBox.top;
                scratchDescription.Format = environment.pixelFormat;
                scratchDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                result = device_->CreateTexture2D(&scratchDescription, nullptr, &candidate[index].scratch);
            }
            if (SUCCEEDED(result))
            {
                result = device_->CreateShaderResourceView(candidate[index].scratch.Get(), nullptr, &candidate[index].scratchView);
            }
        }
        if (SUCCEEDED(result))
        {
            result = device_->CreateQuery(&queryDescription, &candidate[index].query);
        }
        if (FAILED(result))
        {
            return FromHresult(result, CaptureStage::TextureRing);
        }
        HRESULT timingResult = device_->CreateQuery(&disjointDescription, &candidate[index].copyDisjoint);
        if (SUCCEEDED(timingResult))
        {
            timingResult = device_->CreateQuery(&timestampDescription, &candidate[index].copyStart);
        }
        if (SUCCEEDED(timingResult))
        {
            timingResult = device_->CreateQuery(&timestampDescription, &candidate[index].copyEnd);
        }
        if (FAILED(timingResult))
        {
            candidate[index].copyDisjoint.Reset();
            candidate[index].copyStart.Reset();
            candidate[index].copyEnd.Reset();
        }
    }
    slots_.swap(candidate);
    environment_ = environment;
    layout_ = layout;
    outputPixelFormat_ = config.pixelFormat;
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

void D3dRoiRing::Transform(const std::size_t slotIndex) noexcept
{
    const auto& slot = slots_[slotIndex];
    context_->ClearState();
    const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(layout_.roiWidth), static_cast<float>(layout_.roiHeight), 0, 1};
    context_->RSSetViewports(1, &viewport);
    context_->RSSetState(rotationRasterizer_.Get());
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(rotationVertexShader_.Get(), nullptr, 0);
    context_->PSSetShader(rotationPixelShader_.Get(), nullptr, 0);
    ID3D11Buffer* const constants = rotationConstants_.Get();
    context_->PSSetConstantBuffers(0, 1, &constants);
    ID3D11ShaderResourceView* const sourceView = slot.scratchView.Get();
    context_->PSSetShaderResources(0, 1, &sourceView);
    ID3D11RenderTargetView* const targetView = slot.targetView.Get();
    context_->OMSetRenderTargets(1, &targetView, nullptr);
    context_->Draw(3, 0);
    // Integer Load preserves exact texel positions. Same-format rotation does
    // no color transform; the only conversion is the explicit SDR FP16 linear
    // to BGRA8 transfer selected in the immutable constants above.
    context_->ClearState();
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
    const CaptureSize sourceSize = environment_.sourceSize == CaptureSize{} ? environment_.contentSize : environment_.sourceSize;
    if (sourceDevice.Get() != device_.Get() || description.Width != static_cast<UINT>(sourceSize.width) ||
        description.Height != static_cast<UINT>(sourceSize.height) || description.Format != environment_.pixelFormat ||
        description.SampleDesc.Count != 1 || description.SampleDesc.Quality != 0 || description.MipLevels != 1 || description.ArraySize != 1)
    {
        return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Surface);
    }
    submitted = true;
    auto& slot = slots_[slotIndex];
    if (slot.copyDisjoint)
    {
        context_->Begin(slot.copyDisjoint.Get());
        context_->End(slot.copyStart.Get());
    }
    if (environment_.sourceRotation == DXGI_MODE_ROTATION_IDENTITY && environment_.pixelFormat == outputPixelFormat_)
    {
        context_->CopySubresourceRegion(slot.texture.Get(), 0, 0, 0, 0, source.Get(), 0, &layout_.sourceBox);
    }
    else
    {
        context_->CopySubresourceRegion(slot.scratch.Get(), 0, 0, 0, 0, source.Get(), 0, &layout_.sourceBox);
        Transform(slotIndex);
    }
    if (slot.copyDisjoint)
    {
        context_->End(slot.copyEnd.Get());
        context_->End(slot.copyDisjoint.Get());
        slot.copyTimingPending = true;
    }
    // source is released before the frame lease, which the owner retains until Poll.
    return Mark(slotIndex);
}

CaptureStatus D3dRoiRing::Consume(RawRoiConsumer& consumer, const RawRoiFrameMetadata& metadata, const std::size_t slotIndex) noexcept
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

CaptureConsumerCompletion D3dRoiRing::Complete(RawRoiConsumer& consumer, const RawRoiFrameMetadata& metadata, const bool cancelled) noexcept
{
    if (!cancelled && (metadata.slotIndex >= slotCount_ || slots_[metadata.slotIndex].pending))
    {
        return {CaptureStatus::Failure(CaptureError::InternalError, CaptureStage::Completion), false};
    }
    try
    {
        // Cancelled retirement can run on the OS cleanup thread. Deliberately
        // withhold the context: cleanup must not Map or submit GPU work there.
        ID3D11Texture2D* const texture = cancelled || metadata.slotIndex >= slotCount_ ? nullptr : slots_[metadata.slotIndex].texture.Get();
        auto completion = consumer.CompleteStage(metadata, texture, cancelled ? nullptr : context_.Get(), cancelled);
        if (cancelled)
        {
            if (completion.gpuWorkSubmitted)
            {
                completion.status = CaptureStatus::Failure(CaptureError::ConsumerFailure, CaptureStage::Completion);
                completion.gpuWorkSubmitted = false;
            }
            return completion;
        }
        context_->ClearState();
        if (!completion.gpuWorkSubmitted)
        {
            return completion;
        }
        // Mark sets the emergency query pending before the optional fence
        // signal, so even a post-submission signal/debug failure remains safely
        // retireable through Poll.
        const auto retirement = Mark(metadata.slotIndex);
        if (completion.status && !retirement)
        {
            completion.status = retirement;
        }
        return completion;
    }
    catch (...)
    {
        if (cancelled)
        {
            return {CaptureStatus::Failure(CaptureError::ConsumerFailure, CaptureStage::Completion), false};
        }
        // The callback may have thrown after issuing immediate-context work.
        // Record a conservative marker and retain the slot even though the
        // callback could not report its lifetime claim.
        context_->ClearState();
        const auto retirement = Mark(metadata.slotIndex);
        const auto failure = CaptureStatus::Failure(CaptureError::ConsumerFailure, CaptureStage::Completion);
        return {retirement ? failure : retirement, true};
    }
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
        std::optional<std::uint64_t> copyTime100ns;
        bool timingUnavailable = false;
        if (slot.copyTimingPending)
        {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
            std::uint64_t start = 0;
            std::uint64_t end = 0;
            const HRESULT disjointStatus = context_->GetData(slot.copyDisjoint.Get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            const HRESULT startStatus = context_->GetData(slot.copyStart.Get(), &start, sizeof(start), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            const HRESULT endStatus = context_->GetData(slot.copyEnd.Get(), &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (FAILED(disjointStatus) || FAILED(startStatus) || FAILED(endStatus))
            {
                if (DeviceRemoved())
                {
                    return {CaptureStatus::Failure(CaptureError::DeviceLost, CaptureStage::Completion,
                        device_->GetDeviceRemovedReason()), false};
                }
                timingUnavailable = true;
            }
            else if (disjointStatus == S_FALSE || startStatus == S_FALSE || endStatus == S_FALSE)
            {
                return {{}, false};
            }
            else if (disjoint.Disjoint || disjoint.Frequency == 0 || end < start)
            {
                timingUnavailable = true;
            }
            else
            {
                const long double duration = static_cast<long double>(end - start) * 10000000.0L /
                    static_cast<long double>(disjoint.Frequency);
                if (!std::isfinite(duration) || duration < 0 || duration > static_cast<long double>(std::numeric_limits<std::uint64_t>::max()))
                {
                    timingUnavailable = true;
                }
                else
                {
                    copyTime100ns = static_cast<std::uint64_t>(duration + 0.5L);
                }
            }
            slot.copyTimingPending = false;
        }
        slot.pending = false;
        return {{}, true, copyTime100ns, timingUnavailable || !slot.copyDisjoint};
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
    rotationVertexShader_.Reset();
    rotationPixelShader_.Reset();
    rotationConstants_.Reset();
    rotationRasterizer_.Reset();
    context_.Reset();
    device_.Reset();
    outputPixelFormat_ = DXGI_FORMAT_UNKNOWN;
    slotCount_ = 0;
    nextFenceValue_ = 0;
}

}
