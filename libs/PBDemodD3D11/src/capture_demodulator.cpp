#include "pbdemodd3d11/capture_demodulator.h"

#include "pbmodulation/desktop_levels.h"
#include "pbmodulation/reference_raster.h"
#include "pbmodulation/shape_chroma.h"
#include "pbprotocol/checked_integer.h"
#include "pbprotocol/control_fragment_codec.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <new>
#include <ranges>
#include <utility>
#include <vector>

namespace pbdemodd3d11
{
namespace
{

using Microsoft::WRL::ComPtr;
using pbcapturenormalize::CaptureError;
using pbcapturenormalize::CaptureStage;
using pbcapturenormalize::CaptureStatus;
using pbcapturenormalize::ScreenCaptureDomain;
using pbcapturenormalize::ScreenCaptureFrame;
using pbcapturenormalize::ScreenCaptureFrameMetadata;

inline constexpr std::uint32_t canvasWidth = pbmodulation::kLocalDesktopCanvasWidth;
inline constexpr std::uint32_t canvasHeight = pbmodulation::kLocalDesktopCanvasHeight;
inline constexpr std::uint64_t canvasBytes = static_cast<std::uint64_t>(canvasWidth) * canvasHeight * 4;
inline constexpr std::uint64_t fixedOverheadBytes = 1024ULL * 1024;
inline constexpr std::size_t maximumDemodulatorSlots = 4;

bool NonzeroSourceId(const ScreenCaptureDomain& domain) noexcept
{
    return std::ranges::any_of(domain.sourceId, [](const std::byte value) { return value != std::byte{0}; });
}

bool SameComIdentity(IUnknown* const left, IUnknown* const right) noexcept
{
    if (left == nullptr || right == nullptr)
    {
        return false;
    }
    ComPtr<IUnknown> leftIdentity;
    ComPtr<IUnknown> rightIdentity;
    return SUCCEEDED(left->QueryInterface(IID_PPV_ARGS(&leftIdentity))) &&
        SUCCEEDED(right->QueryInterface(IID_PPV_ARGS(&rightIdentity))) && leftIdentity.Get() == rightIdentity.Get();
}

bool SameCompletion(const ScreenCaptureFrameMetadata& left, const ScreenCaptureFrameMetadata& right) noexcept
{
    return left.domain == right.domain && left.captureObservation == right.captureObservation &&
        left.sourceGeneration == right.sourceGeneration && left.slotIndex == right.slotIndex &&
        left.slotGeneration == right.slotGeneration;
}

bool ResolveBinding(const std::uint64_t visualProfileId, pbmodulation::LocalDesktopBootstrapBinding& output) noexcept
{
    if (visualProfileId == pbmodulation::kShapeChromaProfileId)
    {
        output = {visualProfileId, pbmodulation::kShapeChromaLayoutVersion};
        return true;
    }
    if (pbmodulation::GetDesktopLevelsProfile(visualProfileId) != nullptr)
    {
        output = {visualProfileId, pbmodulation::kDesktopLevelsLayoutVersion};
        return true;
    }
    return false;
}

CaptureStatus FromDemodStatus(const DemodStatus status, const CaptureStage stage) noexcept
{
    switch (status.code)
    {
    case DemodError::None: return {};
    case DemodError::WrongThread: return CaptureStatus::Failure(CaptureError::WrongThread, stage, status.nativeError);
    case DemodError::AdapterMismatch:
    case DemodError::WrongDevice: return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Adapter, status.nativeError);
    case DemodError::InvalidConfiguration: return CaptureStatus::Failure(CaptureError::InvalidConfiguration, stage, status.nativeError);
    case DemodError::UnsupportedProfile: return CaptureStatus::Failure(CaptureError::Unsupported, stage, status.nativeError);
    case DemodError::ResourceLimit:
    case DemodError::Busy: return CaptureStatus::Failure(CaptureError::ResourceLimit, stage, status.nativeError);
    case DemodError::DeviceLost: return CaptureStatus::Failure(CaptureError::DeviceLost, stage, status.nativeError);
    case DemodError::MapFailure:
    case DemodError::NativeFailure: return CaptureStatus::Failure(CaptureError::NativeFailure, stage, status.nativeError);
    default: return CaptureStatus::Failure(CaptureError::ConsumerFailure, stage, status.nativeError);
    }
}

std::optional<std::uint64_t> QpcElapsed100ns(const LARGE_INTEGER start, const LARGE_INTEGER end,
    const std::int64_t frequency) noexcept
{
    if (frequency <= 0 || end.QuadPart < start.QuadPart)
    {
        return std::nullopt;
    }
    std::int64_t converted = 0;
    if (!pbcapturenormalize::ConvertQpcTo100ns(end.QuadPart - start.QuadPart, frequency, converted) || converted < 0)
    {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(converted);
}

enum class ExtractedControlKind : std::uint8_t
{
    None, Record, Fragment, Invalid
};

struct ExtractedControl
{
    ExtractedControlKind kind = ExtractedControlKind::Invalid;
    std::uint32_t byteCount = 0;
};

ExtractedControl ExtractControlWindow(const std::span<const std::byte> window,
    const pbprotocol::SessionTag bootstrapSessionTag) noexcept
{
    if (window.size() != pbmodulation::kReferenceControlWindowBytes)
    {
        return {};
    }
    if (std::ranges::all_of(window, [](const std::byte value) { return value == std::byte{0}; }))
    {
        return {ExtractedControlKind::None, 0};
    }
    ExtractedControl accepted;
    std::uint32_t acceptedCandidates = 0;
    for (std::size_t byteCount = 1; byteCount <= window.size(); byteCount++)
    {
        if (!std::ranges::all_of(window.subspan(byteCount), [](const std::byte value) { return value == std::byte{0}; }))
        {
            continue;
        }
        const auto candidate = window.first(byteCount);
        const auto record = pbprotocol::ParseControlRecord(candidate);
        if (record && record.Value().sessionTag == bootstrapSessionTag)
        {
            accepted = {ExtractedControlKind::Record, static_cast<std::uint32_t>(byteCount)};
            acceptedCandidates++;
        }
        const auto fragment = pbprotocol::ParseControlFragment(candidate);
        if (fragment)
        {
            accepted = {ExtractedControlKind::Fragment, static_cast<std::uint32_t>(byteCount)};
            acceptedCandidates++;
        }
    }
    return acceptedCandidates == 1 ? accepted : ExtractedControl{};
}

} // namespace

struct CaptureDemodulator::Implementation
{
    struct Pending
    {
        bool active = false;
        bool hasDemodulation = false;
        ScreenCaptureFrameMetadata metadata;
        DemodSubmission submission;
    };

    Implementation(const CaptureDemodulatorConfig& configured, const CaptureDemodulatorBudget& reserved,
        const pbmodulation::LocalDesktopBootstrapBinding expectedBinding, const std::int64_t frequency)
        : config(configured), binding(expectedBinding), qpcFrequency(frequency),
          referenceScratch(pbmodulation::kReferenceFrameBgraBytes), results(configured.resultQueueCapacity)
    {
        snapshot.reservation = reserved;
    }

    void SetError(const CaptureStatus status) noexcept
    {
        if (!status)
        {
            const std::lock_guard lock(mutex);
            if (snapshot.error)
            {
                snapshot.error = status;
            }
        }
    }

    void SetDemodStatus(const DemodStatus status) noexcept
    {
        if (!status)
        {
            const std::lock_guard lock(mutex);
            snapshot.lastDemodStatus = status;
        }
    }

    void FinishPending(const std::uint32_t slotIndex) noexcept
    {
        pending[slotIndex] = {};
        const std::lock_guard lock(mutex);
        if (snapshot.pendingFrames != 0)
        {
            snapshot.pendingFrames--;
        }
    }

    void TryExternalShutdown() noexcept
    {
        DemodStatus status;
        bool attempted = false;
        {
            const std::lock_guard lock(demodulatorMutex);
            if (!active && demodulator && demodulator->GetSnapshot().pendingFrames == 0)
            {
                status = demodulator->ShutdownAfterExternalCompletion();
                attempted = true;
            }
        }
        if (attempted)
        {
            SetDemodStatus(status);
            SetError(FromDemodStatus(status, CaptureStage::Shutdown));
        }
    }

    void PushResult(const CaptureDemodulatorResult& result) noexcept
    {
        const std::lock_guard lock(mutex);
        if (resultSize == results.size())
        {
            resultHead = (resultHead + 1) % results.size();
            resultSize--;
            pbprotocol::SaturatingIncrementUnsigned(snapshot.resultQueueDrops);
        }
        const std::size_t index = (resultHead + resultSize) % results.size();
        results[index] = result;
        resultSize++;
        snapshot.queuedResults = static_cast<std::uint32_t>(resultSize);
        snapshot.resultQueueHighWater = std::max(snapshot.resultQueueHighWater, snapshot.queuedResults);
    }

    const CaptureDemodulatorConfig config;
    const pbmodulation::LocalDesktopBootstrapBinding binding;
    const std::int64_t qpcFrequency;
    DWORD ownerThread = 0;
    bool active = false;
    ScreenCaptureDomain domain;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    std::array<ComPtr<ID3D11Texture2D>, maximumDemodulatorSlots> bootstrapStaging;
    std::array<Pending, maximumDemodulatorSlots> pending;
    mutable std::mutex demodulatorMutex;
    std::unique_ptr<Demodulator> demodulator;
    std::vector<std::byte> referenceScratch;
    std::array<std::byte, pbmodulation::kReferenceBootstrapRecordBytes> referenceBootstrap{};
    std::array<std::byte, pbmodulation::kReferenceControlWindowBytes> referenceControl{};
    std::array<std::byte, pbmodulation::kReferenceDataRegionBytes> referenceData{};
    mutable std::mutex mutex;
    CaptureDemodulatorSnapshot snapshot;
    std::vector<CaptureDemodulatorResult> results;
    std::size_t resultHead = 0;
    std::size_t resultSize = 0;
};

CaptureStatus CalculateCaptureDemodulatorBudget(const CaptureDemodulatorConfig& config,
    CaptureDemodulatorBudget& output) noexcept
{
    pbmodulation::LocalDesktopBootstrapBinding binding;
    if (!ResolveBinding(config.visualProfileId, binding) || config.slotCount < 2 ||
        config.slotCount > maximumDemodulatorSlots || config.maximumFrameAgeMilliseconds == 0 ||
        config.maximumFrameAgeMilliseconds > 60000 || config.resultQueueCapacity == 0 ||
        config.resultQueueCapacity > maximumCaptureDemodResultQueue ||
        (config.evaluationMode != pbdesktoplevels::EvaluationMode::DiagnosticTruth &&
         config.evaluationMode != pbdesktoplevels::EvaluationMode::Transport))
    {
        return CaptureStatus::Failure(CaptureError::InvalidConfiguration, CaptureStage::Configuration);
    }
    DemodConfig demodConfig;
    demodConfig.readbackSlotCount = config.slotCount;
    demodConfig.maximumResidentBytes = config.maximumResidentBytes;
    demodConfig.evaluationMode = config.evaluationMode;
    CaptureDemodulatorBudget budget;
    const auto demodStatus = CalculateDemodulatorResidentBytes(demodConfig, budget.demodulatorBytes);
    if (!demodStatus)
    {
        return FromDemodStatus(demodStatus, CaptureStage::Configuration);
    }
    const auto staging = pbprotocol::CheckedMultiplyUint64(canvasBytes, config.slotCount);
    const auto queue = pbprotocol::CheckedMultiplyUint64(sizeof(CaptureDemodulatorResult), config.resultQueueCapacity);
    const auto first = staging && queue ? pbprotocol::CheckedAddUint64(budget.demodulatorBytes, staging.Value()) :
        pbprotocol::ProtocolResult<std::uint64_t>::Failure(pbprotocol::ProtocolErrorCode::LengthOverflow, 0);
    const auto second = first ? pbprotocol::CheckedAddUint64(first.Value(), canvasBytes) : first;
    const auto third = second ? pbprotocol::CheckedAddUint64(second.Value(), queue.Value()) : second;
    const auto total = third ? pbprotocol::CheckedAddUint64(third.Value(), fixedOverheadBytes) : third;
    if (!staging || !queue || !total || total.Value() > config.maximumResidentBytes)
    {
        return CaptureStatus::Failure(CaptureError::ResourceLimit, CaptureStage::Configuration);
    }
    budget.bootstrapStagingBytes = staging.Value();
    budget.referenceScratchBytes = canvasBytes;
    budget.resultQueueBytes = queue.Value();
    budget.fixedOverheadBytes = fixedOverheadBytes;
    budget.totalBytes = total.Value();
    output = budget;
    return {};
}

CaptureDemodulator::CaptureDemodulator(std::unique_ptr<Implementation> implementation) noexcept
    : implementation_(std::move(implementation))
{
}

CaptureDemodulator::~CaptureDemodulator() = default;

CaptureStatus CaptureDemodulator::Create(const CaptureDemodulatorConfig& config,
    std::shared_ptr<CaptureDemodulator>& output) noexcept
{
    CaptureDemodulatorBudget budget;
    const auto budgetStatus = CalculateCaptureDemodulatorBudget(config, budget);
    if (!budgetStatus)
    {
        return budgetStatus;
    }
    pbmodulation::LocalDesktopBootstrapBinding binding;
    if (!ResolveBinding(config.visualProfileId, binding))
    {
        return CaptureStatus::Failure(CaptureError::Unsupported, CaptureStage::Configuration);
    }
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
    {
        return CaptureStatus::Failure(CaptureError::Unsupported, CaptureStage::Configuration);
    }
    try
    {
        auto implementation = std::make_unique<Implementation>(config, budget, binding, frequency.QuadPart);
        auto candidate = std::shared_ptr<CaptureDemodulator>(new CaptureDemodulator(std::move(implementation)));
        output = std::move(candidate);
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

std::uint64_t CaptureDemodulator::ReservedBytes() const noexcept
{
    return implementation_ ? implementation_->snapshot.reservation.totalBytes : 0;
}

CaptureStatus CaptureDemodulator::ValidateConfiguration(const pbcapturenormalize::CaptureConfig& config) const noexcept
{
    if (!implementation_)
    {
        return CaptureStatus::Failure(CaptureError::InvalidConfiguration, CaptureStage::Configuration);
    }
    const auto& rectangle = config.region.physicalRect;
    const auto width = static_cast<std::int64_t>(rectangle.right) - rectangle.left;
    const auto height = static_cast<std::int64_t>(rectangle.bottom) - rectangle.top;
    if (width != canvasWidth || height != canvasHeight || config.pixelFormat != DXGI_FORMAT_B8G8R8A8_UNORM ||
        config.roiTextureCount != implementation_->config.slotCount ||
        config.maximumFrameAgeMilliseconds != implementation_->config.maximumFrameAgeMilliseconds)
    {
        return CaptureStatus::Failure(CaptureError::InvalidConfiguration, CaptureStage::Configuration);
    }
    return {};
}

CaptureStatus CaptureDemodulator::DomainStarted(const ScreenCaptureDomain& domain,
    const pbcapturenormalize::CaptureEnvironment& environment, ID3D11Device* const device)
{
    auto& state = *implementation_;
    const auto& rectangle = environment.region.physicalRect;
    const auto width = static_cast<std::int64_t>(rectangle.right) - rectangle.left;
    const auto height = static_cast<std::int64_t>(rectangle.bottom) - rectangle.top;
    if (device == nullptr || !NonzeroSourceId(domain) || domain.captureEpoch == 0 || state.active ||
        (state.ownerThread != 0 && state.ownerThread != GetCurrentThreadId()) ||
        std::ranges::any_of(state.pending, [](const Implementation::Pending& pending) { return pending.active; }) ||
        width != canvasWidth || height != canvasHeight || environment.pixelFormat != DXGI_FORMAT_B8G8R8A8_UNORM ||
        environment.hdr || environment.outputColorSpace != 0)
    {
        return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Recreate);
    }
    DemodStatus previousShutdown;
    bool previousDemodulator = false;
    {
        const std::lock_guard lock(state.demodulatorMutex);
        if (state.demodulator)
        {
            previousShutdown = state.demodulator->ShutdownAfterExternalCompletion();
            previousDemodulator = true;
        }
    }
    if (previousDemodulator && !previousShutdown)
    {
        state.SetDemodStatus(previousShutdown);
        return FromDemodStatus(previousShutdown, CaptureStage::Recreate);
    }

    // The fixed reservation covers one active GPU generation. Once shutdown
    // has proved that the old generation has no pending commands, release it
    // before allocating the replacement rather than transiently holding two
    // complete demodulators and two full-canvas staging rings.
    {
        const std::lock_guard lock(state.demodulatorMutex);
        state.demodulator.reset();
    }
    state.bootstrapStaging = {};
    state.context.Reset();
    state.device.Reset();

    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (!context)
    {
        return CaptureStatus::Failure(CaptureError::NativeFailure, CaptureStage::Device);
    }
    DemodConfig demodConfig;
    demodConfig.readbackSlotCount = state.config.slotCount;
    demodConfig.maximumResidentBytes = state.snapshot.reservation.demodulatorBytes;
    demodConfig.evaluationMode = state.config.evaluationMode;
    std::unique_ptr<Demodulator> demodulator;
    const auto demodStatus = Demodulator::Create(device, demodConfig, demodulator);
    if (!demodStatus)
    {
        state.SetDemodStatus(demodStatus);
        return FromDemodStatus(demodStatus, CaptureStage::Recreate);
    }
    std::array<ComPtr<ID3D11Texture2D>, maximumDemodulatorSlots> staging;
    D3D11_TEXTURE2D_DESC description{};
    description.Width = canvasWidth;
    description.Height = canvasHeight;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (std::uint32_t index = 0; index < state.config.slotCount; index++)
    {
        const HRESULT result = device->CreateTexture2D(&description, nullptr, &staging[index]);
        if (FAILED(result))
        {
            static_cast<void>(demodulator->Shutdown(context.Get()));
            return CaptureStatus::Failure(CaptureError::NativeFailure, CaptureStage::Surface, result);
        }
    }
    state.ownerThread = GetCurrentThreadId();
    state.device = device;
    state.context = std::move(context);
    state.bootstrapStaging = std::move(staging);
    {
        const std::lock_guard lock(state.demodulatorMutex);
        state.demodulator = std::move(demodulator);
    }
    state.domain = domain;
    state.active = true;
    {
        const std::lock_guard lock(state.mutex);
        state.snapshot.active = true;
        state.snapshot.domain = domain;
        state.snapshot.error = {};
        state.snapshot.lastDemodStatus = {};
        pbprotocol::SaturatingIncrementUnsigned(state.snapshot.domainStarts);
    }
    return {};
}

void CaptureDemodulator::DomainInvalidated(const ScreenCaptureDomain& domain) noexcept
{
    auto& state = *implementation_;
    if (!state.active || state.domain != domain)
    {
        return;
    }
    state.active = false;
    DemodStatus demodStatus;
    bool invalidated = false;
    {
        const std::lock_guard lock(state.demodulatorMutex);
        if (state.demodulator)
        {
            demodStatus = state.demodulator->InvalidateDomain(domain);
            invalidated = true;
        }
    }
    if (invalidated)
    {
        state.SetDemodStatus(demodStatus);
        state.SetError(FromDemodStatus(demodStatus, CaptureStage::Consumer));
    }
    {
        const std::lock_guard lock(state.mutex);
        state.snapshot.active = false;
        pbprotocol::SaturatingIncrementUnsigned(state.snapshot.invalidations);
        state.snapshot.staleResultDrops = pbprotocol::SaturatingAddUnsigned(state.snapshot.staleResultDrops,
            static_cast<std::uint64_t>(state.resultSize));
        state.resultHead = 0;
        state.resultSize = 0;
        state.snapshot.queuedResults = 0;
    }
    state.TryExternalShutdown();
}

CaptureStatus CaptureDemodulator::Submit(const ScreenCaptureFrame& frame, ID3D11DeviceContext* const context)
{
    auto& state = *implementation_;
    if (GetCurrentThreadId() != state.ownerThread)
    {
        return CaptureStatus::Failure(CaptureError::WrongThread, CaptureStage::Consumer);
    }
    if (!state.active || frame.metadata.domain != state.domain || frame.metadata.slotIndex >= state.config.slotCount ||
        state.pending[frame.metadata.slotIndex].active || !SameComIdentity(context, state.context.Get()) ||
        !state.bootstrapStaging[frame.metadata.slotIndex])
    {
        return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Consumer);
    }
    auto& pending = state.pending[frame.metadata.slotIndex];
    DemodSubmission submission;
    DemodStatus status;
    {
        const std::lock_guard lock(state.demodulatorMutex);
        status = state.demodulator ? state.demodulator->SubmitUnbound(
            frame, context, state.config.visualProfileId, submission) :
            DemodStatus::Failure(DemodError::InvalidConfiguration, DemodStage::Submission);
    }
    if (!status)
    {
        state.SetDemodStatus(status);
        const auto converted = FromDemodStatus(status, CaptureStage::Consumer);
        state.SetError(converted);
        return converted;
    }
    pending.active = true;
    pending.hasDemodulation = true;
    pending.metadata = frame.metadata;
    pending.submission = submission;
    context->CopyResource(state.bootstrapStaging[frame.metadata.slotIndex].Get(), frame.texture);
    {
        const std::lock_guard lock(state.mutex);
        pbprotocol::SaturatingIncrementUnsigned(state.snapshot.submittedFrames);
        state.snapshot.pendingFrames++;
        state.snapshot.pendingHighWater = std::max(state.snapshot.pendingHighWater, state.snapshot.pendingFrames);
    }
    return {};
}

CaptureStatus CaptureDemodulator::Completed(const ScreenCaptureFrameMetadata& metadata,
    ID3D11DeviceContext* const context, const bool cancelled)
{
    auto& state = *implementation_;
    if (metadata.slotIndex >= state.config.slotCount)
    {
        return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Completion);
    }
    auto& pending = state.pending[metadata.slotIndex];
    if (!pending.active || !SameCompletion(metadata, pending.metadata))
    {
        return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Completion);
    }
    const auto RetireExternally = [&state, &pending]() noexcept
    {
        if (!pending.hasDemodulation)
        {
            return DemodStatus{};
        }
        const std::lock_guard lock(state.demodulatorMutex);
        return state.demodulator ? state.demodulator->RetireAfterExternalCompletion(pending.submission) :
            DemodStatus::Failure(DemodError::InvalidConfiguration, DemodStage::Completion);
    };
    if (cancelled)
    {
        const auto retired = RetireExternally();
        state.SetDemodStatus(retired);
        state.FinishPending(metadata.slotIndex);
        {
            const std::lock_guard lock(state.mutex);
            pbprotocol::SaturatingIncrementUnsigned(state.snapshot.cancelledFrames);
        }
        state.TryExternalShutdown();
        return FromDemodStatus(retired, CaptureStage::Completion);
    }
    if (GetCurrentThreadId() != state.ownerThread || !state.active || metadata.domain != state.domain ||
        !SameComIdentity(context, state.context.Get()))
    {
        const auto retired = RetireExternally();
        state.SetDemodStatus(retired);
        state.FinishPending(metadata.slotIndex);
        const auto failure = CaptureStatus::Failure(GetCurrentThreadId() != state.ownerThread ? CaptureError::WrongThread : CaptureError::InvalidFrame,
            CaptureStage::Completion);
        state.SetError(failure);
        state.TryExternalShutdown();
        return failure;
    }

    LARGE_INTEGER now{};
    std::int64_t now100ns = 0;
    const std::int64_t effectiveTimestamp = pbcapturenormalize::ResolveEffectiveCaptureTime100ns(
        metadata.timestamp.monotonic100ns, metadata.timestamp.arrivalQpc100ns);
    if (effectiveTimestamp < 0 || !QueryPerformanceCounter(&now) ||
        !pbcapturenormalize::ConvertQpcTo100ns(now.QuadPart, state.qpcFrequency, now100ns) ||
        pbcapturenormalize::ClassifyFrameAge(now100ns, effectiveTimestamp, state.config.maximumFrameAgeMilliseconds).disposition !=
            pbcapturenormalize::CaptureFrameAgeDisposition::Current)
    {
        const auto retired = RetireExternally();
        state.SetDemodStatus(retired);
        state.FinishPending(metadata.slotIndex);
        {
            const std::lock_guard lock(state.mutex);
            pbprotocol::SaturatingIncrementUnsigned(state.snapshot.expiredFrames);
        }
        return FromDemodStatus(retired, CaptureStage::Completion);
    }

    LARGE_INTEGER bootstrapStart{};
    LARGE_INTEGER bootstrapEnd{};
    const bool bootstrapStartValid = QueryPerformanceCounter(&bootstrapStart) != FALSE;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    // D3dRoiRing invokes Completed only after the query/fence recorded after
    // this staging CopyResource has completed. DO_NOT_WAIT can still report
    // DXGI_ERROR_WAS_STILL_DRAWING for a staging read on some drivers despite
    // that external completion proof. Match the demodulator staging reads and
    // allow Map to finish the already-proven-complete transition; Flush is not
    // used as a substitute for the ring's query/fence.
    const HRESULT mapResult = context->Map(state.bootstrapStaging[metadata.slotIndex].Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(mapResult) || mapped.pData == nullptr || mapped.RowPitch < canvasWidth * 4)
    {
        const auto retired = RetireExternally();
        state.SetDemodStatus(retired);
        state.FinishPending(metadata.slotIndex);
        const HRESULT removed = state.device->GetDeviceRemovedReason();
        const bool deviceLost = mapResult == DXGI_ERROR_DEVICE_REMOVED || mapResult == DXGI_ERROR_DEVICE_RESET || FAILED(removed);
        const auto failure = CaptureStatus::Failure(deviceLost ? CaptureError::DeviceLost : CaptureError::NativeFailure,
            CaptureStage::Completion, deviceLost && FAILED(removed) ? removed : mapResult);
        state.SetError(failure);
        return failure;
    }
    const auto mappedBytesResult = pbprotocol::CheckedMultiplyUint64(mapped.RowPitch, canvasHeight);
    if (!mappedBytesResult || mappedBytesResult.Value() > std::numeric_limits<std::size_t>::max())
    {
        context->Unmap(state.bootstrapStaging[metadata.slotIndex].Get(), 0);
        const auto retired = RetireExternally();
        state.SetDemodStatus(retired);
        state.FinishPending(metadata.slotIndex);
        const auto failure = CaptureStatus::Failure(CaptureError::ResourceLimit, CaptureStage::Completion);
        state.SetError(failure);
        return failure;
    }
    const auto mappedPixels = std::span(static_cast<const std::byte*>(mapped.pData),
        static_cast<std::size_t>(mappedBytesResult.Value()));
    const pbmodulation::LumaView view{mappedPixels, canvasWidth, canvasHeight, mapped.RowPitch,
        pbmodulation::LumaPixelFormat::Bgra8};
    const auto bootstrap = pbmodulation::DecodeLocalDesktopFixedCanvasBootstrap(view, state.binding);
    bool referenceCandidate = false;
    bool referenceAccepted = false;
    ExtractedControl extractedControl;
    if (!bootstrap.IsAccepted() && mappedPixels.size() >= 4 && mappedPixels[0] == std::byte{0} &&
        mappedPixels[1] == std::byte{0} && mappedPixels[2] == std::byte{0} && mappedPixels[3] == std::byte{255})
    {
        referenceCandidate = true;
        std::span<const std::byte> referencePixels;
        if (mapped.RowPitch == canvasWidth * 4)
        {
            referencePixels = mappedPixels.first(pbmodulation::kReferenceFrameBgraBytes);
        }
        else
        {
            for (std::uint32_t row = 0; row < canvasHeight; row++)
            {
                std::copy_n(mappedPixels.begin() + static_cast<std::size_t>(row) * mapped.RowPitch,
                    static_cast<std::size_t>(canvasWidth) * 4,
                    state.referenceScratch.begin() + static_cast<std::size_t>(row) * canvasWidth * 4);
            }
            referencePixels = state.referenceScratch;
        }
        const auto referenceStatus = pbmodulation::DecodeReferenceFrameInto(referencePixels,
            state.referenceBootstrap, state.referenceControl, state.referenceData);
        if (referenceStatus)
        {
            const auto parsedReference = pbprotocol::ParseBootstrapRecord(state.referenceBootstrap);
            if (parsedReference && parsedReference.Value().visualProfileId == state.binding.visualProfileId &&
                parsedReference.Value().visualLayoutVersion == state.binding.visualLayoutVersion)
            {
                extractedControl = ExtractControlWindow(state.referenceControl, parsedReference.Value().sessionTag);
                referenceAccepted = extractedControl.kind != ExtractedControlKind::Invalid;
            }
        }
    }
    context->Unmap(state.bootstrapStaging[metadata.slotIndex].Get(), 0);
    const bool bootstrapEndValid = QueryPerformanceCounter(&bootstrapEnd) != FALSE;
    const auto bootstrapTime = bootstrapStartValid && bootstrapEndValid ?
        QpcElapsed100ns(bootstrapStart, bootstrapEnd, state.qpcFrequency) : std::nullopt;
    {
        const std::lock_guard lock(state.mutex);
        pbprotocol::SaturatingIncrementUnsigned(state.snapshot.bootstrapMapCalls);
        state.snapshot.bootstrapReadbackBytes = pbprotocol::SaturatingAddUnsigned(state.snapshot.bootstrapReadbackBytes, canvasBytes);
        if (bootstrapTime)
        {
            state.snapshot.bootstrapCpuTimeTotal100ns = pbprotocol::SaturatingAddUnsigned(
                state.snapshot.bootstrapCpuTimeTotal100ns, *bootstrapTime);
            state.snapshot.bootstrapCpuTimeHighWater100ns = std::max(state.snapshot.bootstrapCpuTimeHighWater100ns, *bootstrapTime);
            pbprotocol::SaturatingIncrementUnsigned(state.snapshot.bootstrapCpuTimingSamples);
        }
        else
        {
            pbprotocol::SaturatingIncrementUnsigned(state.snapshot.cpuTimingUnavailable);
        }
    }
    if (!bootstrap.IsAccepted())
    {
        const auto retired = RetireExternally();
        state.SetDemodStatus(retired);
        state.FinishPending(metadata.slotIndex);
        if (referenceAccepted)
        {
            if (extractedControl.kind != ExtractedControlKind::None)
            {
                CaptureDemodulatorResult result;
                result.kind = extractedControl.kind == ExtractedControlKind::Record ?
                    CaptureDemodulatorResultKind::ControlRecord : CaptureDemodulatorResultKind::ControlFragment;
                result.metadata = metadata;
                result.bootstrapRecord = state.referenceBootstrap;
                result.controlByteCount = extractedControl.byteCount;
                std::copy_n(state.referenceControl.begin(), result.controlByteCount, result.controlBytes.begin());
                state.PushResult(result);
            }
            {
                const std::lock_guard lock(state.mutex);
                pbprotocol::SaturatingIncrementUnsigned(state.snapshot.controlFrames);
                pbprotocol::SaturatingIncrementUnsigned(state.snapshot.completedFrames);
            }
            return FromDemodStatus(retired, CaptureStage::Completion);
        }
        {
            const std::lock_guard lock(state.mutex);
            pbprotocol::SaturatingIncrementUnsigned(state.snapshot.bootstrapRejectedFrames);
            if (referenceCandidate)
            {
                pbprotocol::SaturatingIncrementUnsigned(state.snapshot.controlFrameFailures);
            }
            const auto erasureIndex = static_cast<std::size_t>(bootstrap.erasure);
            if (erasureIndex < state.snapshot.bootstrapErasures.size())
            {
                pbprotocol::SaturatingIncrementUnsigned(state.snapshot.bootstrapErasures[erasureIndex]);
            }
        }
        return FromDemodStatus(retired, CaptureStage::Completion);
    }

    LARGE_INTEGER demodulationStart{};
    LARGE_INTEGER demodulationEnd{};
    const bool demodulationStartValid = QueryPerformanceCounter(&demodulationStart) != FALSE;
    DemodFrameResult demodulation;
    DemodPollResult poll;
    {
        const std::lock_guard lock(state.demodulatorMutex);
        poll = state.demodulator ? state.demodulator->PollUnbound(
            context, pending.submission, bootstrap.canonical44, demodulation) :
            DemodPollResult{DemodStatus::Failure(DemodError::InvalidConfiguration, DemodStage::Completion), true};
    }
    const bool demodulationEndValid = QueryPerformanceCounter(&demodulationEnd) != FALSE;
    const auto demodulationTime = demodulationStartValid && demodulationEndValid ?
        QpcElapsed100ns(demodulationStart, demodulationEnd, state.qpcFrequency) : std::nullopt;
    if (!poll.ready)
    {
        const auto retired = RetireExternally();
        state.SetDemodStatus(retired);
        state.FinishPending(metadata.slotIndex);
        const auto failure = CaptureStatus::Failure(CaptureError::InternalError, CaptureStage::Completion);
        state.SetError(failure);
        return failure;
    }
    state.FinishPending(metadata.slotIndex);
    {
        const std::lock_guard lock(state.mutex);
        pbprotocol::SaturatingIncrementUnsigned(state.snapshot.bootstrapAcceptedFrames);
        if (demodulationTime)
        {
            state.snapshot.demodulationCpuTimeTotal100ns = pbprotocol::SaturatingAddUnsigned(
                state.snapshot.demodulationCpuTimeTotal100ns, *demodulationTime);
            state.snapshot.demodulationCpuTimeHighWater100ns = std::max(
                state.snapshot.demodulationCpuTimeHighWater100ns, *demodulationTime);
            pbprotocol::SaturatingIncrementUnsigned(state.snapshot.demodulationCpuTimingSamples);
        }
        else
        {
            pbprotocol::SaturatingIncrementUnsigned(state.snapshot.cpuTimingUnavailable);
        }
    }
    if (!poll.status)
    {
        state.SetDemodStatus(poll.status);
        {
            const std::lock_guard lock(state.mutex);
            pbprotocol::SaturatingIncrementUnsigned(state.snapshot.demodulationRejectedFrames);
        }
        const bool visualErasure = poll.status.code == DemodError::InvalidBinding || poll.status.code == DemodError::NonFiniteMetric ||
            poll.status.code == DemodError::CalibrationFailure;
        if (visualErasure)
        {
            return {};
        }
        const auto failure = FromDemodStatus(poll.status, CaptureStage::Completion);
        state.SetError(failure);
        return failure;
    }

    CaptureDemodulatorResult result;
    result.kind = CaptureDemodulatorResultKind::Transport;
    result.metadata = metadata;
    result.bootstrapRecord = bootstrap.canonical44;
    result.bootstrap = bootstrap;
    result.demodulation = demodulation;
    state.PushResult(result);
    {
        const std::lock_guard lock(state.mutex);
        pbprotocol::SaturatingIncrementUnsigned(state.snapshot.completedFrames);
        state.snapshot.acceptedTransportBlocks = pbprotocol::SaturatingAddUnsigned(state.snapshot.acceptedTransportBlocks,
            static_cast<std::uint64_t>(demodulation.acceptedTransportBlockCount));
        if (demodulation.evaluation.IsVerified())
        {
            pbprotocol::SaturatingIncrementUnsigned(state.snapshot.verifiedFrames);
        }
        else
        {
            pbprotocol::SaturatingIncrementUnsigned(state.snapshot.postFecFailedFrames);
        }
    }
    return {};
}

void CaptureDemodulator::Erased(const pbcapturenormalize::CaptureErasure& erasure) noexcept
{
    static_cast<void>(erasure);
    const std::lock_guard lock(implementation_->mutex);
    pbprotocol::SaturatingIncrementUnsigned(implementation_->snapshot.captureErasures);
}

bool CaptureDemodulator::TakeResult(CaptureDemodulatorResult& output) noexcept
{
    auto& state = *implementation_;
    const std::lock_guard lock(state.mutex);
    if (state.resultSize == 0)
    {
        return false;
    }
    output = state.results[state.resultHead];
    state.resultHead = (state.resultHead + 1) % state.results.size();
    state.resultSize--;
    state.snapshot.queuedResults = static_cast<std::uint32_t>(state.resultSize);
    pbprotocol::SaturatingIncrementUnsigned(state.snapshot.resultsTaken);
    return true;
}

CaptureDemodulatorSnapshot CaptureDemodulator::GetSnapshot() const noexcept
{
    auto& state = *implementation_;
    DemodSnapshot demodulatorSnapshot;
    {
        const std::lock_guard lock(state.demodulatorMutex);
        demodulatorSnapshot = state.demodulator ? state.demodulator->GetSnapshot() : DemodSnapshot{};
    }
    const std::lock_guard lock(state.mutex);
    auto result = state.snapshot;
    result.demodulator = demodulatorSnapshot;
    return result;
}

} // namespace pbdemodd3d11
