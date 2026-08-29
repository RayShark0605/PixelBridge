#include "normalize_consumer.h"
#include "capture_runtime.h"
#include "pbprotocol/checked_integer.h"
#include "pbprotocol/session_random.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <new>
#include <wrl/client.h>

namespace pbcapturenormalize
{

const char* GetCaptureErasureName(const CaptureErasureReason reason) noexcept
{
    switch (reason)
    {
    case CaptureErasureReason::None: return "None";
    case CaptureErasureReason::InactiveDomain: return "InactiveDomain";
    case CaptureErasureReason::StaleObservation: return "StaleObservation";
    case CaptureErasureReason::Expired: return "Expired";
    case CaptureErasureReason::InvalidTimestamp: return "InvalidTimestamp";
    case CaptureErasureReason::CursorUnknown: return "CursorUnknown";
    case CaptureErasureReason::CursorPossiblyComposited: return "CursorPossiblyComposited";
    case CaptureErasureReason::InvalidMetadata: return "InvalidMetadata";
    case CaptureErasureReason::InvalidOwnedTexture: return "InvalidOwnedTexture";
    case CaptureErasureReason::Count: break;
    }
    return "Unknown";
}

namespace detail
{
namespace
{

bool SameRectangle(const RECT& first, const RECT& second) noexcept
{
    return first.left == second.left && first.top == second.top && first.right == second.right && first.bottom == second.bottom;
}

bool SameEnvironment(const CaptureEnvironment& first, const CaptureEnvironment& second) noexcept
{
    return first.region.monitor == second.region.monitor && SameRectangle(first.region.physicalRect, second.region.physicalRect) &&
           SameRectangle(first.region.monitorPhysicalRect, second.region.monitorPhysicalRect) && first.region.dpiX == second.region.dpiX &&
           first.region.dpiY == second.region.dpiY && first.region.rotation == second.region.rotation && first.contentSize == second.contentSize &&
           first.sourceSize == second.sourceSize && first.sourceRotation == second.sourceRotation && first.pixelFormat == second.pixelFormat &&
           first.backendKind == second.backendKind && first.adapterLuid.HighPart == second.adapterLuid.HighPart &&
           first.adapterLuid.LowPart == second.adapterLuid.LowPart && first.displayFrequency == second.displayFrequency &&
           first.bitsPerColor == second.bitsPerColor && first.outputColorSpace == second.outputColorSpace && first.hdr == second.hdr;
}

bool SameCompletion(const RawRoiFrameMetadata& raw, const ScreenCaptureFrameMetadata& pending) noexcept
{
    return raw.captureEpoch == pending.domain.captureEpoch && raw.arrivalOrdinal == pending.captureObservation &&
           raw.sourceGeneration == pending.sourceGeneration && raw.slotIndex == pending.slotIndex && raw.slotGeneration == pending.slotGeneration;
}

} // namespace

struct NormalizeConsumer::Implementation
{
    struct Pending
    {
        bool active = false;
        bool submitFailed = false;
        ScreenCaptureFrameMetadata metadata;
    };

    Implementation(const CaptureConfig& config, const CaptureBackendKind kind, std::shared_ptr<ScreenCaptureConsumer> receiver,
                   const std::array<std::byte, 16>& identity, const std::int64_t frequency, const std::uint64_t reserved)
        : runtimeConfig(config), backend(kind), consumer(std::move(receiver)), clockFrequency(frequency)
    {
        snapshot.enabled = true;
        snapshot.domain.sourceId = identity;
        snapshot.reservedConsumerBytes = reserved;
    }

    void Erase(const RawRoiFrameMetadata& raw, const CaptureErasureReason reason) noexcept
    {
        CaptureErasure erased;
        {
            const std::lock_guard lock(snapshotMutex);
            erased = {snapshot.domain, raw.arrivalOrdinal, reason};
            // Preserve the rejected observation's epoch; never relabel old work.
            erased.domain.captureEpoch = raw.captureEpoch;
            pbprotocol::SaturatingIncrementUnsigned(snapshot.erasedFrames);
            pbprotocol::SaturatingIncrementUnsigned(snapshot.erasures[static_cast<std::size_t>(reason)]);
            snapshot.lastErasure = reason;
        }
        consumer->Erased(erased);
    }

    [[nodiscard]] CaptureErasureReason ValidateTime(const RawRoiFrameMetadata& raw) const noexcept
    {
        std::int64_t converted = 0;
        if (backend == CaptureBackendKind::Wgc)
        {
            if (raw.timestampDomain != CaptureTimestampDomain::WgcSystemRelative100ns || raw.rawFrequency != 10000000 || raw.rawTimestamp < 0)
            {
                return CaptureErasureReason::InvalidTimestamp;
            }
            converted = raw.rawTimestamp;
        }
        else if (raw.timestampDomain != CaptureTimestampDomain::DxgiQpcTicks || raw.rawFrequency != clockFrequency ||
                 !ConvertQpcTo100ns(raw.rawTimestamp, raw.rawFrequency, converted))
        {
            return CaptureErasureReason::InvalidTimestamp;
        }
        LARGE_INTEGER counter{};
        std::int64_t now100ns = 0;
        const auto effective = ResolveEffectiveCaptureTime100ns(converted, raw.arrivalQpc100ns);
        if (converted != raw.systemRelativeTime100ns || effective < 0 || !QueryPerformanceCounter(&counter) ||
            !ConvertQpcTo100ns(counter.QuadPart, clockFrequency, now100ns))
        {
            return CaptureErasureReason::InvalidTimestamp;
        }
        const auto age = ClassifyFrameAge(now100ns, effective, runtimeConfig.maximumFrameAgeMilliseconds);
        if (age.disposition == CaptureFrameAgeDisposition::InvalidTimestamp)
        {
            return CaptureErasureReason::InvalidTimestamp;
        }
        return age.disposition == CaptureFrameAgeDisposition::Expired ? CaptureErasureReason::Expired : CaptureErasureReason::None;
    }

    const CaptureConfig runtimeConfig;
    const CaptureBackendKind backend;
    const std::shared_ptr<ScreenCaptureConsumer> consumer;
    const std::int64_t clockFrequency;
    mutable std::mutex snapshotMutex;
    CaptureNormalizeSnapshot snapshot;
    CaptureEnvironment environment;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    DWORD ownerThread = 0;
    std::uint64_t lastObservation = 0;
    std::uint64_t lastSourceGeneration = 0;
    std::uint64_t epochSourceGeneration = 0;
    std::array<std::uint64_t, maximumRoiTextures> lastSlotGenerations{};
    std::array<Pending, maximumRoiTextures> pending;
};

NormalizeConsumer::NormalizeConsumer(std::unique_ptr<Implementation> implementation) noexcept : implementation_(std::move(implementation))
{
}

NormalizeConsumer::~NormalizeConsumer() = default;

CaptureStatus NormalizeConsumer::Create(const CaptureNormalizeConfig& config, const CaptureBackendKind backend,
                                        std::shared_ptr<ScreenCaptureConsumer> consumer, std::shared_ptr<NormalizeConsumer>& output) noexcept
{
    if (!consumer || config.capture.maximumFrameAgeMilliseconds == 0 ||
        (backend != CaptureBackendKind::Wgc && backend != CaptureBackendKind::Dxgi) ||
        (backend == CaptureBackendKind::Wgc && config.capture.pixelFormat == DXGI_FORMAT_R10G10B10A2_UNORM))
    {
        return CaptureStatus::Failure(CaptureError::InvalidConfiguration, CaptureStage::Configuration);
    }
    const auto validation = ValidateCaptureConfig(config.capture, backend);
    if (!validation)
    {
        return validation;
    }
    const auto consumerValidation = consumer->ValidateConfiguration(config.capture);
    if (!consumerValidation)
    {
        return consumerValidation;
    }
    const auto reserved = consumer->ReservedBytes();
    if (reserved >= config.capture.maximumCaptureBytes)
    {
        return CaptureStatus::Failure(CaptureError::ResourceLimit, CaptureStage::Configuration);
    }
    auto runtimeConfig = config.capture;
    runtimeConfig.maximumCaptureBytes -= reserved;
    const auto budgetStatus = ValidateCaptureConfig(runtimeConfig, backend);
    if (!budgetStatus)
    {
        return budgetStatus;
    }
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
    {
        return CaptureStatus::Failure(CaptureError::Unsupported, CaptureStage::Configuration);
    }
    const auto identity = pbprotocol::GenerateRandomSessionId();
    if (!identity)
    {
        return CaptureStatus::Failure(CaptureError::NativeFailure, CaptureStage::Configuration);
    }
    try
    {
        auto implementation = std::make_unique<Implementation>(runtimeConfig, backend, std::move(consumer), identity.Value().bytes, frequency.QuadPart, reserved);
        auto created = std::shared_ptr<NormalizeConsumer>(new NormalizeConsumer(std::move(implementation)));
        output = std::move(created);
        return {};
    }
    catch (const std::bad_alloc&)
    {
        return CaptureStatus::Failure(CaptureError::OutOfMemory, CaptureStage::Configuration);
    }
    catch (...)
    {
        return CaptureStatus::Failure(CaptureError::InternalError, CaptureStage::Configuration);
    }
}

const CaptureConfig& NormalizeConsumer::GetRuntimeConfig() const noexcept
{
    return implementation_->runtimeConfig;
}

CaptureNormalizeSnapshot NormalizeConsumer::GetSnapshot() const noexcept
{
    const std::lock_guard lock(implementation_->snapshotMutex);
    return implementation_->snapshot;
}

CaptureStatus NormalizeConsumer::EpochStarted(const std::uint64_t epoch, const CaptureEnvironment& environment, ID3D11Device* device)
{
    auto& state = *implementation_;
    const auto previous = GetSnapshot();
    if (previous.active || epoch == 0 || epoch <= previous.domain.captureEpoch || device == nullptr ||
        (state.ownerThread != 0 && state.ownerThread != GetCurrentThreadId()) ||
        std::ranges::any_of(state.pending, [](const Implementation::Pending& slot) { return slot.active; }))
    {
        return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Recreate);
    }
    if (environment.backendKind != state.backend || environment.sourceSize.width <= 0 || environment.sourceSize.height <= 0 ||
        !SameRectangle(environment.region.physicalRect, state.runtimeConfig.region.physicalRect) ||
        environment.region.monitor != state.runtimeConfig.region.monitor ||
        (state.backend == CaptureBackendKind::Wgc && (environment.sourceRotation != DXGI_MODE_ROTATION_IDENTITY ||
         (environment.pixelFormat != DXGI_FORMAT_B8G8R8A8_UNORM && environment.pixelFormat != DXGI_FORMAT_R16G16B16A16_FLOAT))))
    {
        return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Recreate);
    }
    auto actualConfig = state.runtimeConfig;
    actualConfig.pixelFormat = environment.pixelFormat;
    CaptureLayout layout;
    const auto status = ValidateLayout(actualConfig, environment, layout);
    if (!status)
    {
        return status;
    }
    if (environment.hdr && environment.pixelFormat == DXGI_FORMAT_B8G8R8A8_UNORM)
    {
        return CaptureStatus::Failure(CaptureError::Unsupported, CaptureStage::Surface);
    }
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapterDescription{};
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
        FAILED(adapter->GetDesc(&adapterDescription)) || adapterDescription.AdapterLuid.HighPart != environment.adapterLuid.HighPart ||
        adapterDescription.AdapterLuid.LowPart != environment.adapterLuid.LowPart)
    {
        return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Adapter);
    }
    state.environment = environment;
    state.device = device;
    state.ownerThread = GetCurrentThreadId();
    state.epochSourceGeneration = 0;
    ScreenCaptureDomain domain = previous.domain;
    domain.captureEpoch = epoch;
    {
        const std::lock_guard lock(state.snapshotMutex);
        state.snapshot.domain = domain;
        state.snapshot.active = true;
        pbprotocol::SaturatingIncrementUnsigned(state.snapshot.epochStarts);
    }
    CaptureStatus notification;
    try
    {
        notification = state.consumer->DomainStarted(domain, environment, device);
    }
    catch (...)
    {
        notification = CaptureStatus::Failure(CaptureError::ConsumerFailure, CaptureStage::Consumer);
    }
    if (!notification)
    {
        EpochInvalidated(epoch);
    }
    return notification;
}

void NormalizeConsumer::EpochInvalidated(const std::uint64_t epoch) noexcept
{
    auto& state = *implementation_;
    ScreenCaptureDomain domain;
    {
        const std::lock_guard lock(state.snapshotMutex);
        if (!state.snapshot.active || state.snapshot.domain.captureEpoch != epoch)
        {
            return;
        }
        state.snapshot.active = false;
        domain = state.snapshot.domain;
        pbprotocol::SaturatingIncrementUnsigned(state.snapshot.invalidations);
    }
    state.consumer->DomainInvalidated(domain);
}

CaptureStatus NormalizeConsumer::Submit(const RawRoiFrameMetadata& rawMetadata, ID3D11Texture2D* texture, ID3D11DeviceContext* context)
{
    auto& state = *implementation_;
    const auto current = GetSnapshot();
    if (state.ownerThread != GetCurrentThreadId())
    {
        return CaptureStatus::Failure(CaptureError::WrongThread, CaptureStage::Consumer);
    }
    CaptureErasureReason reason = CaptureErasureReason::None;
    if (!current.active || rawMetadata.captureEpoch != current.domain.captureEpoch)
    {
        reason = CaptureErasureReason::InactiveDomain;
    }
    else if (rawMetadata.arrivalOrdinal == 0 || rawMetadata.arrivalOrdinal <= state.lastObservation)
    {
        reason = CaptureErasureReason::StaleObservation;
    }
    else if (!SameEnvironment(rawMetadata.environment, state.environment) || rawMetadata.slotIndex >= state.runtimeConfig.roiTextureCount ||
             rawMetadata.slotGeneration == 0 || rawMetadata.sourceGeneration == 0 ||
             (state.epochSourceGeneration != 0 && rawMetadata.sourceGeneration != state.epochSourceGeneration) ||
             (state.epochSourceGeneration == 0 && rawMetadata.sourceGeneration <= state.lastSourceGeneration))
    {
        reason = CaptureErasureReason::InvalidMetadata;
    }
    else if (state.pending[rawMetadata.slotIndex].active || rawMetadata.slotGeneration <= state.lastSlotGenerations[rawMetadata.slotIndex])
    {
        reason = CaptureErasureReason::InvalidMetadata;
    }
    if (reason != CaptureErasureReason::None)
    {
        state.Erase(rawMetadata, reason);
        return {};
    }
    state.lastObservation = rawMetadata.arrivalOrdinal;
    state.lastSlotGenerations[rawMetadata.slotIndex] = rawMetadata.slotGeneration;
    state.epochSourceGeneration = rawMetadata.sourceGeneration;
    state.lastSourceGeneration = rawMetadata.sourceGeneration;
    reason = state.ValidateTime(rawMetadata);
    // KnownAbsent is per-frame proof from the source that the pointer is not
    // in the frame's pixels; it needs no backend capability and applies to
    // both backends. The other branches keep their capability/position proofs.
    const bool cursorKnownAbsent = rawMetadata.cursorState == CursorState::KnownAbsent;
    const bool cursorExcluded = cursorKnownAbsent ||
                                (state.backend == CaptureBackendKind::Wgc ?
                                 rawMetadata.cursorState == CursorState::Excluded && rawMetadata.capabilities.cursorExcluded :
                                 rawMetadata.cursorState == CursorState::SeparatePointer && rawMetadata.pointer.separateVisible && rawMetadata.pointer.positionKnown);
    if (reason == CaptureErasureReason::None && !cursorExcluded)
    {
        reason = rawMetadata.cursorState == CursorState::PossiblyComposited ? CaptureErasureReason::CursorPossiblyComposited : CaptureErasureReason::CursorUnknown;
    }
    if (reason != CaptureErasureReason::None)
    {
        state.Erase(rawMetadata, reason);
        return {};
    }
    D3D11_TEXTURE2D_DESC description{};
    Microsoft::WRL::ComPtr<ID3D11Device> textureDevice;
    Microsoft::WRL::ComPtr<ID3D11Device> contextDevice;
    if (texture != nullptr)
    {
        texture->GetDesc(&description);
        texture->GetDevice(&textureDevice);
    }
    if (context != nullptr)
    {
        context->GetDevice(&contextDevice);
    }
    const auto& rectangle = state.environment.region.physicalRect;
    const auto width = static_cast<UINT>(static_cast<std::int64_t>(rectangle.right) - rectangle.left);
    const auto height = static_cast<UINT>(static_cast<std::int64_t>(rectangle.bottom) - rectangle.top);
    if (textureDevice.Get() != state.device.Get() || contextDevice.Get() != state.device.Get() || context == nullptr ||
        context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE || description.Width != width || description.Height != height ||
        description.Format != state.environment.pixelFormat || description.MipLevels != 1 || description.ArraySize != 1 ||
        description.SampleDesc.Count != 1 || description.SampleDesc.Quality != 0 || description.Usage != D3D11_USAGE_DEFAULT || description.CPUAccessFlags != 0)
    {
        state.Erase(rawMetadata, CaptureErasureReason::InvalidOwnedTexture);
        return {};
    }
    ScreenCaptureFrame frame;
    auto& metadata = frame.metadata;
    metadata.domain = current.domain;
    metadata.backend = state.backend;
    metadata.captureObservation = rawMetadata.arrivalOrdinal;
    metadata.sourceGeneration = rawMetadata.sourceGeneration;
    metadata.slotIndex = rawMetadata.slotIndex;
    metadata.slotGeneration = rawMetadata.slotGeneration;
    metadata.physicalRoi = rectangle;
    metadata.sourceContentSize = state.environment.contentSize;
    metadata.sourceExtent = state.environment.sourceSize;
    metadata.roiSize = {static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)};
    metadata.displayRotation = state.environment.region.rotation;
    metadata.sourceTransform = state.environment.sourceRotation;
    metadata.sourcePixelFormat = state.environment.pixelFormat;
    metadata.pixelFormat = state.environment.pixelFormat;
    metadata.adapterLuid = state.environment.adapterLuid;
    metadata.bitsPerColor = state.environment.bitsPerColor;
    metadata.outputColorSpace = state.environment.outputColorSpace;
    metadata.hdr = state.environment.hdr;
    // WGC FP16 carries a documented linear-scRGB capture contract. The DXGI
    // duplication backend converts the negotiated SDR scan-out format into the
    // same FP16 ROI without tone mapping, so identical SDR G22 metadata must
    // classify identically across backends (unified normalized frame contract).
    // An HDR scan-out descriptor alone does not establish a linear conversion;
    // such frames stay Unknown and are erased downstream by the authoritative
    // hdr flag. No tone-map is invented here.
    metadata.signalEncoding = metadata.pixelFormat == DXGI_FORMAT_R16G16B16A16_FLOAT &&
                              (state.backend == CaptureBackendKind::Wgc || (!metadata.hdr && metadata.outputColorSpace == 0))
        ? CaptureSignalEncoding::LinearScRgb
        : !metadata.hdr && metadata.outputColorSpace == 0 && metadata.pixelFormat != DXGI_FORMAT_R16G16B16A16_FLOAT
        ? CaptureSignalEncoding::SdrRgb
        : CaptureSignalEncoding::Unknown;
    metadata.timestamp = {rawMetadata.timestampDomain, rawMetadata.rawTimestamp, rawMetadata.rawFrequency,
                          rawMetadata.systemRelativeTime100ns, rawMetadata.arrivalQpc100ns};
    metadata.roiCopyTime100ns = rawMetadata.roiCopyTime100ns;
    metadata.isCursorExcluded = true;
    metadata.sourceCursorState = rawMetadata.cursorState;
    metadata.pointer = rawMetadata.pointer;
    frame.texture = texture;
    auto& pending = state.pending[rawMetadata.slotIndex];
    pending = {true, true, metadata};
    CaptureStatus status;
    try
    {
        status = state.consumer->Submit(frame, context);
    }
    catch (...)
    {
        status = CaptureStatus::Failure(CaptureError::ConsumerFailure, CaptureStage::Consumer);
    }
    pending.submitFailed = !status;
    if (status)
    {
        const std::lock_guard lock(state.snapshotMutex);
        pbprotocol::SaturatingIncrementUnsigned(state.snapshot.acceptedFrames);
    }
    return status;
}

CaptureStatus NormalizeConsumer::Completed(const RawRoiFrameMetadata& rawMetadata, ID3D11DeviceContext* context, const bool cancelled)
{
    auto& state = *implementation_;
    if (!cancelled && state.ownerThread != GetCurrentThreadId())
    {
        return CaptureStatus::Failure(CaptureError::WrongThread, CaptureStage::Completion);
    }
    if (rawMetadata.slotIndex >= state.pending.size())
    {
        return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Completion);
    }
    auto& pending = state.pending[rawMetadata.slotIndex];
    if (!pending.active)
    {
        return {};
    }
    if (!SameCompletion(rawMetadata, pending.metadata))
    {
        return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Completion);
    }
    const auto current = GetSnapshot();
    const bool discard = cancelled || pending.submitFailed || !current.active || pending.metadata.domain != current.domain;
    CaptureStatus status;
    try
    {
        status = state.consumer->Completed(pending.metadata, discard ? nullptr : context, discard);
    }
    catch (...)
    {
        status = CaptureStatus::Failure(CaptureError::ConsumerFailure, CaptureStage::Completion);
    }
    pending = {};
    return status;
}

} // namespace detail
} // namespace pbcapturenormalize
