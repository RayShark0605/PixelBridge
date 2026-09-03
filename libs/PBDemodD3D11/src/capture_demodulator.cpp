#include "pbdemodd3d11/capture_demodulator.h"

#include "pbmodulation/desktop_levels.h"
#include "pbmodulation/reference_raster.h"
#include "pbmodulation/remote_visual.h"
#include "pbmodulation/shape_chroma.h"
#include "pbprotocol/checked_integer.h"
#include "pbprotocol/control_fragment_codec.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
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

pbmodulation::LocalDesktopObservation MakeReferenceBootstrapObservation(
    const std::array<std::byte, pbprotocol::kBootstrapRecordBytes>& canonicalRecord) noexcept
{
    pbmodulation::LocalDesktopObservation observation;
    observation.erasure = pbmodulation::LocalDesktopErasureReason::None;
    observation.canonical44 = canonicalRecord;
    observation.geometry.scaleX = 1.0;
    observation.geometry.scaleY = 1.0;
    return observation;
}

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
inline constexpr std::uint32_t lowFpsAllTransportSlotsMask =
    (1U << pbmodulation::kRemoteVisualLowFpsCodewords) - 1U;
static_assert(pbmodulation::kRemoteVisualLowFpsCodewords < 32);

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

bool SupportedCaptureSourceFormat(const DXGI_FORMAT format) noexcept
{
    return format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_R10G10B10A2_UNORM ||
        format == DXGI_FORMAT_R16G16B16A16_FLOAT;
}

bool EqualLuid(const LUID& left, const LUID& right) noexcept
{
    return left.LowPart == right.LowPart && left.HighPart == right.HighPart;
}

CaptureStatus ValidateDeviceAdapter(ID3D11Device* const device, const LUID& expectedAdapterLuid) noexcept
{
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC description{};
    HRESULT status = device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (SUCCEEDED(status))
    {
        status = dxgiDevice->GetAdapter(&adapter);
    }
    if (SUCCEEDED(status))
    {
        status = adapter->GetDesc(&description);
    }
    if (FAILED(status))
    {
        return CaptureStatus::Failure(CaptureError::NativeFailure, CaptureStage::Adapter, status);
    }
    return EqualLuid(description.AdapterLuid, expectedAdapterLuid) ? CaptureStatus{} :
        CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Adapter);
}

bool ResolveBinding(const std::uint64_t visualProfileId, pbmodulation::LocalDesktopBootstrapBinding& output) noexcept
{
    if (visualProfileId == pbmodulation::kUnifiedVisualProfile.productProfile.visualProfileId)
    {
        output = {visualProfileId, pbmodulation::kUnifiedVisualProfile.productProfile.visualLayoutVersion};
        return true;
    }
    if (visualProfileId == pbmodulation::kRemoteVisualLowFpsProfileId)
    {
        output = {visualProfileId, pbmodulation::kRemoteVisualLowFpsLayoutVersion};
        return true;
    }
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
    if (visualProfileId == pbmodulation::kRemoteVisualProfileId)
    {
        output = {visualProfileId, pbmodulation::kRemoteVisualLayoutVersion};
        return true;
    }
    return false;
}

bool IsRemoteVisualLowFps(const std::uint64_t visualProfileId) noexcept
{
    return visualProfileId == pbmodulation::kRemoteVisualLowFpsProfileId;
}

bool IsUnifiedVisual(const std::uint64_t visualProfileId) noexcept
{
    return visualProfileId == pbmodulation::kUnifiedVisualProfile.productProfile.visualProfileId;
}

bool IsStagedVisual(const std::uint64_t visualProfileId) noexcept
{
    return IsRemoteVisualLowFps(visualProfileId) || IsUnifiedVisual(visualProfileId);
}

pbmodulation::LocalDesktopObservation DecodeRemoteVisualLowFpsBootstrap(const pbmodulation::LumaView& view,
    const pbmodulation::LocalDesktopBootstrapBinding& expectedBinding,
    const pbmodulation::RemoteVisualLowFpsDecodePolicy& policy) noexcept
{
    auto observation = pbmodulation::DecodeLocalDesktopBootstrap(view, expectedBinding, policy.locator);
    if (observation.IsAccepted())
    {
        const auto parsed = pbprotocol::ParseBootstrapRecord(observation.canonical44);
        if (!parsed || parsed.Value().visualProfileId != expectedBinding.visualProfileId ||
            parsed.Value().visualLayoutVersion != expectedBinding.visualLayoutVersion)
        {
            observation.erasure = pbmodulation::LocalDesktopErasureReason::UnsupportedRecord;
        }
        else
        {
            pbmodulation::LocalDesktopGeometry samplingGeometry;
            if (pbmodulation::ResolveRemoteVisualLowFpsSamplingGeometry(observation.geometry,
                view.width, view.height, samplingGeometry, policy) != pbmodulation::RemoteVisualLowFpsErasure::None)
            {
                observation.erasure = pbmodulation::LocalDesktopErasureReason::InvalidGeometry;
            }
        }
    }
    if (!observation.IsAccepted())
    {
        observation.canonical44.fill(std::byte{0});
        observation.quality = 0;
    }
    return observation;
}

pbmodulation::LocalDesktopObservation DecodeUnifiedBootstrap(const pbmodulation::LumaView& view,
    const pbmodulation::LocalDesktopBootstrapBinding& expectedBinding,
    const pbmodulation::UnifiedVisualDecodePolicy& policy) noexcept
{
    auto observation = pbmodulation::DecodeLocalDesktopBootstrap(view, expectedBinding, policy.locator);
    if (observation.IsAccepted())
    {
        const auto parsed = pbprotocol::ParseBootstrapRecord(observation.canonical44);
        pbmodulation::LocalDesktopGeometry samplingGeometry;
        if (!parsed || parsed.Value().visualProfileId != expectedBinding.visualProfileId ||
            parsed.Value().visualLayoutVersion != expectedBinding.visualLayoutVersion)
        {
            observation.erasure = pbmodulation::LocalDesktopErasureReason::UnsupportedRecord;
        }
        else if (!pbmodulation::ResolveUnifiedVisualSamplingGeometry(observation.geometry,
            view.width, view.height, policy, samplingGeometry))
        {
            observation.erasure = pbmodulation::LocalDesktopErasureReason::InvalidGeometry;
        }
    }
    if (!observation.IsAccepted())
    {
        observation.canonical44.fill(std::byte{0});
        observation.quality = 0;
    }
    return observation;
}

CaptureDemodulatorGeometryStatus ClassifyGeometry(const pbmodulation::LocalDesktopObservation& bootstrap,
    const std::uint32_t roiWidth, const std::uint32_t roiHeight) noexcept
{
    if (!bootstrap.IsAccepted())
    {
        return CaptureDemodulatorGeometryStatus::Rejected;
    }
    const auto& geometry = bootstrap.geometry;
    const double right = geometry.originX + geometry.scaleX * canvasWidth;
    const double bottom = geometry.originY + geometry.scaleY * canvasHeight;
    constexpr double edgeTolerance = 0.5;
    const bool hasMargin = geometry.originX > edgeTolerance || geometry.originY > edgeTolerance ||
        right < static_cast<double>(roiWidth) - edgeTolerance || bottom < static_cast<double>(roiHeight) - edgeTolerance;
    if (hasMargin)
    {
        return CaptureDemodulatorGeometryStatus::Letterboxed;
    }
    const bool exact = std::abs(geometry.originX) <= edgeTolerance && std::abs(geometry.originY) <= edgeTolerance &&
        std::abs(geometry.scaleX - 1.0) <= 1e-6 && std::abs(geometry.scaleY - 1.0) <= 1e-6 &&
        roiWidth == canvasWidth && roiHeight == canvasHeight;
    return exact ? CaptureDemodulatorGeometryStatus::ExactCanvas : CaptureDemodulatorGeometryStatus::Scaled;
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
    struct TemporalFrame
    {
        bool active = false;
        bool evaluationPending = false;
        std::uint64_t captureEpoch = 0;
        std::uint64_t sessionTag = 0;
        std::uint64_t frameSequence = 0;
        std::array<std::byte, pbprotocol::kBootstrapRecordBytes> canonicalBootstrap{};
        std::uint32_t acceptedTransportSlotMask = 0;
        std::uint32_t duplicateAttempts = 0;
        std::array<pbdesktoplevels::AcceptedTransportBlock, pbmodulation::kRemoteVisualLowFpsCodewords>
            acceptedTransportBlocks{};
        bool acceptedRemoteControl = false;
        pbdesktoplevels::AcceptedRemoteControlBlock acceptedRemoteControlBlock;
    };

    struct Pending
    {
        bool active = false;
        bool hasDemodulation = false;
        bool bootstrapReady = false;
        bool bootstrapAcceptedCounted = false;
        ScreenCaptureFrameMetadata metadata;
        pbmodulation::LocalDesktopObservation bootstrap;
        ID3D11Texture2D* borrowedTexture = nullptr;
        DemodSubmission submission;
        CaptureDemodulatorTemporalDisposition temporalDisposition =
            CaptureDemodulatorTemporalDisposition::NotApplicable;
        std::uint64_t temporalCaptureEpoch = 0;
        std::uint64_t temporalSessionTag = 0;
        std::uint64_t temporalFrameSequence = 0;
        bool temporalAttemptActive = false;
    };

    Implementation(const CaptureDemodulatorConfig& configured, const CaptureDemodulatorBudget& reserved,
        const pbmodulation::LocalDesktopBootstrapBinding expectedBinding, const std::int64_t frequency)
        : config(configured), binding(expectedBinding), qpcFrequency(frequency),
          referenceScratch(IsStagedVisual(configured.visualProfileId) ? 0 : pbmodulation::kReferenceFrameBgraBytes),
          results(configured.resultQueueCapacity)
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

    [[nodiscard]] bool SameTemporalIdentity(const Pending& value) const noexcept
    {
        return temporalFrame.active && value.temporalCaptureEpoch == temporalFrame.captureEpoch &&
            value.temporalSessionTag == temporalFrame.sessionTag &&
            value.temporalFrameSequence == temporalFrame.frameSequence &&
            value.bootstrap.canonical44 == temporalFrame.canonicalBootstrap;
    }

    void SyncTemporalIdentitySnapshot() noexcept
    {
        const auto identitySnapshot = temporalIdentity.GetSnapshot();
        const std::lock_guard lock(mutex);
        snapshot.temporalUniqueFrames = identitySnapshot.uniqueFrames;
        snapshot.temporalDuplicateFrames = identitySnapshot.duplicateFrames;
        snapshot.temporalReorderedFrames = identitySnapshot.reorderedFrames;
        snapshot.temporalGapEvents = identitySnapshot.gapEvents;
        snapshot.temporalSkippedSequences = identitySnapshot.skippedSequences;
    }

    [[nodiscard]] CaptureStatus BeginTemporalAttempt(Pending& value, const std::int64_t effectiveTimestamp,
        bool& shouldSubmit) noexcept
    {
        shouldSubmit = false;
        const auto parsed = pbprotocol::ParseBootstrapRecord(value.bootstrap.canonical44);
        if (!parsed)
        {
            return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Completion);
        }
        const auto& record = parsed.Value();
        const auto disposition = temporalIdentity.Observe(record.frameSequence, value.metadata.domain.captureEpoch,
            effectiveTimestamp, record.sessionTag.value);
        SyncTemporalIdentitySnapshot();
        value.temporalCaptureEpoch = value.metadata.domain.captureEpoch;
        value.temporalSessionTag = record.sessionTag.value;
        value.temporalFrameSequence = record.frameSequence;
        if (disposition == pbmodulation::VisualIdentityDisposition::Invalid)
        {
            return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Completion);
        }
        if (disposition == pbmodulation::VisualIdentityDisposition::Reordered)
        {
            value.temporalDisposition = CaptureDemodulatorTemporalDisposition::Reordered;
            const std::lock_guard lock(mutex);
            pbprotocol::SaturatingIncrementUnsigned(snapshot.temporalSuppressedFrames);
            return {};
        }
        if (disposition == pbmodulation::VisualIdentityDisposition::Unique)
        {
            temporalFrame = {};
            temporalFrame.active = true;
            temporalFrame.evaluationPending = true;
            temporalFrame.captureEpoch = value.temporalCaptureEpoch;
            temporalFrame.sessionTag = value.temporalSessionTag;
            temporalFrame.frameSequence = value.temporalFrameSequence;
            temporalFrame.canonicalBootstrap = value.bootstrap.canonical44;
            value.temporalDisposition = CaptureDemodulatorTemporalDisposition::Unique;
            value.temporalAttemptActive = true;
            shouldSubmit = true;
            return {};
        }

        const bool terminal = temporalFrame.acceptedRemoteControl ||
            temporalFrame.acceptedTransportSlotMask == lowFpsAllTransportSlotsMask;
        if (!SameTemporalIdentity(value) || temporalFrame.evaluationPending || terminal)
        {
            value.temporalDisposition = CaptureDemodulatorTemporalDisposition::DuplicateSuppressed;
            const std::lock_guard lock(mutex);
            pbprotocol::SaturatingIncrementUnsigned(snapshot.temporalSuppressedFrames);
            return {};
        }
        if (temporalFrame.duplicateAttempts >= config.maximumDuplicateRefinementAttempts)
        {
            value.temporalDisposition = CaptureDemodulatorTemporalDisposition::DuplicateSuppressed;
            const std::lock_guard lock(mutex);
            pbprotocol::SaturatingIncrementUnsigned(snapshot.duplicateRefinementLimitDrops);
            pbprotocol::SaturatingIncrementUnsigned(snapshot.temporalSuppressedFrames);
            return {};
        }
        temporalFrame.duplicateAttempts++;
        temporalFrame.evaluationPending = true;
        value.temporalDisposition = CaptureDemodulatorTemporalDisposition::DuplicateRefinement;
        value.temporalAttemptActive = true;
        shouldSubmit = true;
        {
            const std::lock_guard lock(mutex);
            pbprotocol::SaturatingIncrementUnsigned(snapshot.duplicateRefinementAttempts);
        }
        return {};
    }

    void AbandonTemporalAttempt(Pending& value) noexcept
    {
        if (!value.temporalAttemptActive)
        {
            return;
        }
        if (SameTemporalIdentity(value))
        {
            temporalFrame.evaluationPending = false;
        }
        value.temporalAttemptActive = false;
    }

    [[nodiscard]] CaptureStatus CompleteTemporalAttempt(Pending& value, const DemodFrameResult& demodulation,
        CaptureDemodulatorResult& result) noexcept
    {
        result.temporalDisposition = value.temporalDisposition;
        if (!value.temporalAttemptActive)
        {
            return {};
        }
        value.temporalAttemptActive = false;
        if (!SameTemporalIdentity(value))
        {
            result.temporalDisposition = CaptureDemodulatorTemporalDisposition::StaleCompletion;
            const std::lock_guard lock(mutex);
            pbprotocol::SaturatingIncrementUnsigned(snapshot.temporalStaleCompletionDrops);
            pbprotocol::SaturatingIncrementUnsigned(snapshot.temporalSuppressedFrames);
            return {};
        }
        temporalFrame.evaluationPending = false;
        TemporalFrame candidate = temporalFrame;
        if (demodulation.acceptedTransportBlockCount > demodulation.acceptedTransportBlocks.size() ||
            demodulation.acceptedRemoteControlBlockCount > demodulation.acceptedRemoteControlBlocks.size())
        {
            return CaptureStatus::Failure(CaptureError::ConsumerFailure, CaptureStage::Completion);
        }

        bool recoveredNewData = false;
        for (std::uint32_t index = 0; index < demodulation.acceptedTransportBlockCount; index++)
        {
            const auto& block = demodulation.acceptedTransportBlocks[index];
            if (block.slot >= pbmodulation::kRemoteVisualLowFpsCodewords || block.byteCount > block.bytes.size() ||
                candidate.acceptedRemoteControl)
            {
                return CaptureStatus::Failure(CaptureError::ConsumerFailure, CaptureStage::Completion);
            }
            const std::uint32_t slotMask = 1U << block.slot;
            if ((candidate.acceptedTransportSlotMask & slotMask) != 0)
            {
                if (candidate.acceptedTransportBlocks[block.slot] != block)
                {
                    return CaptureStatus::Failure(CaptureError::ConsumerFailure, CaptureStage::Completion);
                }
                continue;
            }
            candidate.acceptedTransportBlocks[block.slot] = block;
            candidate.acceptedTransportSlotMask |= slotMask;
            result.admittedTransportBlockIndices[result.admittedTransportBlockCount++] = index;
            recoveredNewData = true;
        }
        for (std::uint32_t index = 0; index < demodulation.acceptedRemoteControlBlockCount; index++)
        {
            const auto& block = demodulation.acceptedRemoteControlBlocks[index];
            if (block.byteCount > block.bytes.size() || candidate.acceptedTransportSlotMask != 0)
            {
                return CaptureStatus::Failure(CaptureError::ConsumerFailure, CaptureStage::Completion);
            }
            if (candidate.acceptedRemoteControl)
            {
                if (candidate.acceptedRemoteControlBlock != block)
                {
                    return CaptureStatus::Failure(CaptureError::ConsumerFailure, CaptureStage::Completion);
                }
                continue;
            }
            candidate.acceptedRemoteControl = true;
            candidate.acceptedRemoteControlBlock = block;
            result.admittedRemoteControlBlockIndices[result.admittedRemoteControlBlockCount++] = index;
            recoveredNewData = true;
        }
        temporalFrame = std::move(candidate);
        {
            const std::lock_guard lock(mutex);
            snapshot.temporallyAdmittedTransportBlocks = pbprotocol::SaturatingAddUnsigned(
                snapshot.temporallyAdmittedTransportBlocks,
                static_cast<std::uint64_t>(result.admittedTransportBlockCount));
            if (value.temporalDisposition == CaptureDemodulatorTemporalDisposition::DuplicateRefinement)
            {
                if (recoveredNewData)
                {
                    pbprotocol::SaturatingIncrementUnsigned(snapshot.duplicateRefinementRecoveries);
                }
                else
                {
                    pbprotocol::SaturatingIncrementUnsigned(snapshot.temporalSuppressedFrames);
                }
            }
        }
        return {};
    }

    void QueueResultLocked(const CaptureDemodulatorResult& result) noexcept
    {
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

    void FinishPending(const std::uint32_t slotIndex) noexcept
    {
        AbandonTemporalAttempt(pending[slotIndex]);
        pending[slotIndex] = {};
        const std::lock_guard lock(mutex);
        pendingCaptureObservations[slotIndex] = 0;
        if (snapshot.pendingFrames != 0)
        {
            snapshot.pendingFrames--;
        }
    }

    void FinishPendingWithResult(const std::uint32_t slotIndex, const CaptureDemodulatorResult& result) noexcept
    {
        AbandonTemporalAttempt(pending[slotIndex]);
        pending[slotIndex] = {};
        const std::lock_guard lock(mutex);
        pendingCaptureObservations[slotIndex] = 0;
        if (snapshot.pendingFrames != 0)
        {
            snapshot.pendingFrames--;
        }
        QueueResultLocked(result);
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

    CaptureDemodulatorGeometryStatus RecordGeometry(const pbmodulation::LocalDesktopObservation& bootstrap) noexcept
    {
        const auto status = ClassifyGeometry(bootstrap, roiWidth, roiHeight);
        const std::lock_guard lock(mutex);
        snapshot.lastGeometryStatus = status;
        snapshot.lastGeometry = bootstrap.geometry;
        switch (status)
        {
        case CaptureDemodulatorGeometryStatus::ExactCanvas:
            pbprotocol::SaturatingIncrementUnsigned(snapshot.exactGeometryFrames);
            break;
        case CaptureDemodulatorGeometryStatus::Scaled:
            pbprotocol::SaturatingIncrementUnsigned(snapshot.scaledGeometryFrames);
            break;
        case CaptureDemodulatorGeometryStatus::Letterboxed:
            pbprotocol::SaturatingIncrementUnsigned(snapshot.letterboxedGeometryFrames);
            break;
        case CaptureDemodulatorGeometryStatus::Rejected:
            pbprotocol::SaturatingIncrementUnsigned(snapshot.rejectedGeometryFrames);
            break;
        case CaptureDemodulatorGeometryStatus::NotApplicable:
            break;
        }
        return status;
    }

    const CaptureDemodulatorConfig config;
    const pbmodulation::LocalDesktopBootstrapBinding binding;
    const std::int64_t qpcFrequency;
    DWORD ownerThread = 0;
    bool active = false;
    ScreenCaptureDomain domain;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    LUID adapterLuid{};
    std::array<ComPtr<ID3D11Texture2D>, maximumDemodulatorSlots> bootstrapStaging;
    std::uint32_t roiWidth = 0;
    std::uint32_t roiHeight = 0;
    std::array<Pending, maximumDemodulatorSlots> pending;
    pbmodulation::VisualIdentityTracker temporalIdentity;
    TemporalFrame temporalFrame;
    mutable std::mutex demodulatorMutex;
    std::unique_ptr<Demodulator> demodulator;
    std::vector<std::byte> referenceScratch;
    std::array<std::byte, pbmodulation::kReferenceBootstrapRecordBytes> referenceBootstrap{};
    std::array<std::byte, pbmodulation::kReferenceControlWindowBytes> referenceControl{};
    std::array<std::byte, pbmodulation::kReferenceDataRegionBytes> referenceData{};
    mutable std::mutex mutex;
    CaptureDemodulatorSnapshot snapshot;
    std::vector<CaptureDemodulatorResult> results;
    std::array<std::uint64_t, maximumDemodulatorSlots> pendingCaptureObservations{};
    std::size_t resultHead = 0;
    std::size_t resultSize = 0;
};

CaptureStatus CalculateCaptureDemodulatorBudget(const CaptureDemodulatorConfig& config,
    CaptureDemodulatorBudget& output) noexcept
{
    pbmodulation::LocalDesktopBootstrapBinding binding;
    const bool remoteVisualLowFps = IsRemoteVisualLowFps(config.visualProfileId);
    const bool unifiedVisual = IsUnifiedVisual(config.visualProfileId);
    const bool stagedVisual = remoteVisualLowFps || unifiedVisual;
    const pbmodulation::LocalDesktopGeometry policyProbe{0, 0, 1, 1, 0};
    if (!ResolveBinding(config.visualProfileId, binding) || config.slotCount < 2 ||
        config.slotCount > maximumDemodulatorSlots || config.maximumFrameAgeMilliseconds == 0 ||
        config.maximumFrameAgeMilliseconds > 60000 || config.resultQueueCapacity == 0 ||
        config.resultQueueCapacity > maximumCaptureDemodResultQueue || config.maximumRoiWidth == 0 ||
        config.maximumRoiHeight == 0 || config.maximumRoiWidth > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        config.maximumRoiHeight > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        (remoteVisualLowFps && config.maximumDuplicateRefinementAttempts > maximumCaptureDemodDuplicateRefinementAttempts) ||
        (remoteVisualLowFps && pbmodulation::ValidateRemoteVisualLowFpsGeometry(policyProbe, config.remoteVisualLowFpsPolicy) ==
            pbmodulation::RemoteVisualLowFpsErasure::InvalidPolicy) ||
        (unifiedVisual && !pbmodulation::ValidateUnifiedVisualDecodePolicy(config.unifiedVisualPolicy)) ||
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
    const auto maximumPixels = stagedVisual ? pbprotocol::CheckedMultiplyUint64(config.maximumRoiWidth, config.maximumRoiHeight) :
        pbprotocol::ProtocolResult<std::uint64_t>::Success(static_cast<std::uint64_t>(canvasWidth) * canvasHeight);
    const auto maximumFrameBytes = maximumPixels ? pbprotocol::CheckedMultiplyUint64(maximumPixels.Value(), 4) : maximumPixels;
    const auto staging = maximumFrameBytes ? pbprotocol::CheckedMultiplyUint64(maximumFrameBytes.Value(), config.slotCount) : maximumFrameBytes;
    const auto queue = pbprotocol::CheckedMultiplyUint64(sizeof(CaptureDemodulatorResult), config.resultQueueCapacity);
    const auto first = staging && queue ? pbprotocol::CheckedAddUint64(budget.demodulatorBytes, staging.Value()) :
        pbprotocol::ProtocolResult<std::uint64_t>::Failure(pbprotocol::ProtocolErrorCode::LengthOverflow, 0);
    const std::uint64_t referenceScratchBytes = stagedVisual ? 0 : canvasBytes;
    const auto second = first ? pbprotocol::CheckedAddUint64(first.Value(), referenceScratchBytes) : first;
    const auto third = second ? pbprotocol::CheckedAddUint64(second.Value(), queue.Value()) : second;
    const auto total = third ? pbprotocol::CheckedAddUint64(third.Value(), fixedOverheadBytes) : third;
    if (!maximumPixels || !maximumFrameBytes || !staging || !queue || !total || total.Value() > config.maximumResidentBytes)
    {
        return CaptureStatus::Failure(CaptureError::ResourceLimit, CaptureStage::Configuration);
    }
    budget.bootstrapStagingBytes = staging.Value();
    budget.referenceScratchBytes = referenceScratchBytes;
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
    const bool stagedVisual = IsStagedVisual(implementation_->config.visualProfileId);
    const bool validSize = stagedVisual ? width > 0 && height > 0 &&
        width <= implementation_->config.maximumRoiWidth && height <= implementation_->config.maximumRoiHeight :
        width == canvasWidth && height == canvasHeight;
    if (!validSize || config.pixelFormat != DXGI_FORMAT_B8G8R8A8_UNORM ||
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
    const bool stagedVisual = IsStagedVisual(state.config.visualProfileId);
    const bool validSize = stagedVisual ? width > 0 && height > 0 &&
        width <= state.config.maximumRoiWidth && height <= state.config.maximumRoiHeight :
        width == canvasWidth && height == canvasHeight;
    if (device == nullptr || !NonzeroSourceId(domain) || domain.captureEpoch == 0 || state.active ||
        (state.ownerThread != 0 && state.ownerThread != GetCurrentThreadId()) ||
        std::ranges::any_of(state.pending, [](const Implementation::Pending& pending) { return pending.active; }) ||
        !validSize || environment.pixelFormat != DXGI_FORMAT_B8G8R8A8_UNORM ||
        environment.hdr || environment.outputColorSpace != 0)
    {
        return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Recreate);
    }
    const auto adapterStatus = ValidateDeviceAdapter(device, environment.adapterLuid);
    if (!adapterStatus)
    {
        return adapterStatus;
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
    state.roiWidth = 0;
    state.roiHeight = 0;
    state.context.Reset();
    state.device.Reset();
    state.adapterLuid = {};

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
    description.Width = static_cast<UINT>(width);
    description.Height = static_cast<UINT>(height);
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
    state.adapterLuid = environment.adapterLuid;
    state.bootstrapStaging = std::move(staging);
    state.roiWidth = static_cast<std::uint32_t>(width);
    state.roiHeight = static_cast<std::uint32_t>(height);
    {
        const std::lock_guard lock(state.demodulatorMutex);
        state.demodulator = std::move(demodulator);
    }
    state.domain = domain;
    state.temporalIdentity.ResetBaseline();
    state.temporalFrame = {};
    state.active = true;
    {
        const std::lock_guard lock(state.mutex);
        state.pendingCaptureObservations.fill(0);
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
    state.temporalFrame = {};
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
    const auto& metadata = frame.metadata;
    const auto physicalWidth = static_cast<std::int64_t>(metadata.physicalRoi.right) - metadata.physicalRoi.left;
    const auto physicalHeight = static_cast<std::int64_t>(metadata.physicalRoi.bottom) - metadata.physicalRoi.top;
    const bool cursorProvenAbsent = metadata.sourceCursorState == pbcapturenormalize::CursorState::Excluded ||
        metadata.sourceCursorState == pbcapturenormalize::CursorState::SeparatePointer ||
        metadata.sourceCursorState == pbcapturenormalize::CursorState::KnownAbsent;
    if (!state.active || metadata.domain != state.domain || metadata.slotIndex >= state.config.slotCount ||
        state.pending[frame.metadata.slotIndex].active || !SameComIdentity(context, state.context.Get()) ||
        !state.bootstrapStaging[frame.metadata.slotIndex] || frame.texture == nullptr || metadata.captureObservation == 0 ||
        metadata.sourceGeneration == 0 || metadata.slotGeneration == 0 || metadata.roiSize.width <= 0 ||
        metadata.roiSize.height <= 0 || static_cast<std::uint32_t>(metadata.roiSize.width) != state.roiWidth ||
        static_cast<std::uint32_t>(metadata.roiSize.height) != state.roiHeight || metadata.pixelFormat != DXGI_FORMAT_B8G8R8A8_UNORM ||
        metadata.signalEncoding != pbcapturenormalize::CaptureSignalEncoding::SdrRgb || metadata.hdr ||
        !metadata.isCursorExcluded || !cursorProvenAbsent || physicalWidth != metadata.roiSize.width ||
        physicalHeight != metadata.roiSize.height || metadata.sourceContentSize.width <= 0 ||
        metadata.sourceContentSize.height <= 0 || metadata.sourceExtent.width <= 0 || metadata.sourceExtent.height <= 0 ||
        metadata.displayRotation < DXGI_MODE_ROTATION_IDENTITY || metadata.displayRotation > DXGI_MODE_ROTATION_ROTATE270 ||
        metadata.sourceTransform < DXGI_MODE_ROTATION_IDENTITY || metadata.sourceTransform > DXGI_MODE_ROTATION_ROTATE270 ||
        !SupportedCaptureSourceFormat(metadata.sourcePixelFormat) || metadata.timestamp.rawFrequency <= 0 ||
        metadata.timestamp.monotonic100ns < 0 || metadata.timestamp.arrivalQpc100ns < -1 ||
        !EqualLuid(metadata.adapterLuid, state.adapterLuid))
    {
        return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Consumer);
    }
    D3D11_TEXTURE2D_DESC description{};
    ComPtr<ID3D11Device> textureDevice;
    frame.texture->GetDesc(&description);
    frame.texture->GetDevice(&textureDevice);
    if (!SameComIdentity(textureDevice.Get(), state.device.Get()) || description.Width != state.roiWidth ||
        description.Height != state.roiHeight || description.MipLevels != 1 || description.ArraySize != 1 ||
        description.Format != DXGI_FORMAT_B8G8R8A8_UNORM || description.SampleDesc.Count != 1 ||
        description.SampleDesc.Quality != 0 || description.Usage != D3D11_USAGE_DEFAULT || description.CPUAccessFlags != 0 ||
        (description.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0)
    {
        return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Consumer);
    }
    auto& pending = state.pending[frame.metadata.slotIndex];
    DemodSubmission submission;
    const bool stagedVisual = IsStagedVisual(state.config.visualProfileId);
    if (!stagedVisual)
    {
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
    }
    // Publish the observation-order barrier before this submission can coexist with
    // a completed later slot. TakeResult may run on a different thread.
    {
        const std::lock_guard lock(state.mutex);
        state.pendingCaptureObservations[frame.metadata.slotIndex] = frame.metadata.captureObservation;
        pbprotocol::SaturatingIncrementUnsigned(state.snapshot.submittedFrames);
        state.snapshot.pendingFrames++;
        state.snapshot.pendingHighWater = std::max(state.snapshot.pendingHighWater, state.snapshot.pendingFrames);
    }
    pending.active = true;
    pending.hasDemodulation = !stagedVisual;
    pending.metadata = frame.metadata;
    pending.borrowedTexture = frame.texture;
    pending.submission = submission;
    context->CopyResource(state.bootstrapStaging[frame.metadata.slotIndex].Get(), frame.texture);
    return {};
}

pbcapturenormalize::CaptureConsumerCompletion CaptureDemodulator::CompleteStage(const ScreenCaptureFrameMetadata& metadata,
    ID3D11Texture2D* const texture, ID3D11DeviceContext* const context, const bool cancelled)
{
    bool gpuWorkSubmitted = false;
    const auto status = CompleteInternal(metadata, texture, context, cancelled, true, gpuWorkSubmitted);
    return {status, gpuWorkSubmitted};
}

CaptureStatus CaptureDemodulator::Completed(const ScreenCaptureFrameMetadata& metadata,
    ID3D11DeviceContext* const context, const bool cancelled)
{
    bool gpuWorkSubmitted = false;
    const auto status = CompleteInternal(metadata, nullptr, context, cancelled, false, gpuWorkSubmitted);
    return gpuWorkSubmitted ? CaptureStatus::Failure(CaptureError::InternalError, CaptureStage::Completion) : status;
}

CaptureStatus CaptureDemodulator::CompleteInternal(const ScreenCaptureFrameMetadata& metadata, ID3D11Texture2D* const texture,
    ID3D11DeviceContext* const context, const bool cancelled, const bool allowContinuation, bool& gpuWorkSubmitted)
{
    gpuWorkSubmitted = false;
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
    const bool remoteVisualLowFps = IsRemoteVisualLowFps(state.config.visualProfileId);
    const bool unifiedVisual = IsUnifiedVisual(state.config.visualProfileId);
    const bool stagedVisual = remoteVisualLowFps || unifiedVisual;
    if (stagedVisual && pending.hasDemodulation)
    {
        const std::lock_guard lock(state.mutex);
        pbprotocol::SaturatingIncrementUnsigned(state.snapshot.stagedGpuCompletions);
    }
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
        !SameComIdentity(context, state.context.Get()) ||
        (stagedVisual && allowContinuation && (texture == nullptr || texture != pending.borrowedTexture)))
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

    auto bootstrap = pending.bootstrap;
    if (!pending.bootstrapReady)
    {
        LARGE_INTEGER bootstrapStart{};
        LARGE_INTEGER bootstrapEnd{};
        const bool bootstrapStartValid = QueryPerformanceCounter(&bootstrapStart) != FALSE;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        // D3dRoiRing invokes this completion stage only after the query/fence recorded after
        // this staging CopyResource has completed. DO_NOT_WAIT can still report
        // DXGI_ERROR_WAS_STILL_DRAWING for a staging read on some drivers despite
        // that external completion proof. Match the demodulator staging reads and
        // allow Map to finish the already-proven-complete transition; Flush is not
        // used as a substitute for the ring's query/fence.
        const HRESULT mapResult = context->Map(state.bootstrapStaging[metadata.slotIndex].Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(mapResult) || mapped.pData == nullptr || mapped.RowPitch < state.roiWidth * 4)
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
        const auto mappedBytesResult = pbprotocol::CheckedMultiplyUint64(mapped.RowPitch, state.roiHeight);
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
        const pbmodulation::LumaView view{mappedPixels, state.roiWidth, state.roiHeight, mapped.RowPitch,
            pbmodulation::LumaPixelFormat::Bgra8};
        bootstrap = remoteVisualLowFps ? DecodeRemoteVisualLowFpsBootstrap(view, state.binding,
            state.config.remoteVisualLowFpsPolicy) : unifiedVisual ? DecodeUnifiedBootstrap(view, state.binding,
                state.config.unifiedVisualPolicy) : pbmodulation::DecodeLocalDesktopFixedCanvasBootstrap(view, state.binding);
        bool referenceCandidate = false;
        bool referenceAccepted = false;
        ExtractedControl extractedControl;
        if (!stagedVisual && !bootstrap.IsAccepted() && mappedPixels.size() >= 4 && mappedPixels[0] == std::byte{0} &&
            mappedPixels[1] == std::byte{0} && mappedPixels[2] == std::byte{0} && mappedPixels[3] == std::byte{255})
        {
            referenceCandidate = true;
            std::span<const std::byte> referencePixels;
            if (mapped.RowPitch == state.roiWidth * 4)
            {
                referencePixels = mappedPixels.first(pbmodulation::kReferenceFrameBgraBytes);
            }
            else
            {
                for (std::uint32_t row = 0; row < state.roiHeight; row++)
                {
                    std::copy_n(mappedPixels.begin() + static_cast<std::size_t>(row) * mapped.RowPitch,
                        static_cast<std::size_t>(state.roiWidth) * 4,
                        state.referenceScratch.begin() + static_cast<std::size_t>(row) * state.roiWidth * 4);
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
            const std::uint64_t readbackBytes = static_cast<std::uint64_t>(state.roiWidth) * state.roiHeight * 4;
            state.snapshot.bootstrapReadbackBytes = pbprotocol::SaturatingAddUnsigned(state.snapshot.bootstrapReadbackBytes, readbackBytes);
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
        const auto observedGeometryStatus = stagedVisual ? state.RecordGeometry(bootstrap) :
            CaptureDemodulatorGeometryStatus::NotApplicable;
        if (!bootstrap.IsAccepted())
        {
            const auto retired = RetireExternally();
            state.SetDemodStatus(retired);
            if (referenceAccepted)
            {
                if (extractedControl.kind != ExtractedControlKind::None)
                {
                    CaptureDemodulatorResult result;
                    result.kind = extractedControl.kind == ExtractedControlKind::Record ?
                        CaptureDemodulatorResultKind::ControlRecord : CaptureDemodulatorResultKind::ControlFragment;
                    result.metadata = metadata;
                    result.bootstrapRecord = state.referenceBootstrap;
                    result.bootstrap = MakeReferenceBootstrapObservation(state.referenceBootstrap);
                    result.controlByteCount = extractedControl.byteCount;
                    std::copy_n(state.referenceControl.begin(), result.controlByteCount, result.controlBytes.begin());
                    state.FinishPendingWithResult(metadata.slotIndex, result);
                }
                else
                {
                    CaptureDemodulatorResult result;
                    result.kind = CaptureDemodulatorResultKind::TelemetryOnly;
                    result.metadata = metadata;
                    result.bootstrapRecord = state.referenceBootstrap;
                    result.bootstrap = MakeReferenceBootstrapObservation(state.referenceBootstrap);
                    state.FinishPendingWithResult(metadata.slotIndex, result);
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
                pbprotocol::SaturatingIncrementUnsigned(state.snapshot.completedFrames);
            }
            CaptureDemodulatorResult result;
            result.kind = CaptureDemodulatorResultKind::TelemetryOnly;
            result.metadata = metadata;
            result.bootstrap = bootstrap;
            result.geometryStatus = observedGeometryStatus;
            state.FinishPendingWithResult(metadata.slotIndex, result);
            return FromDemodStatus(retired, CaptureStage::Completion);
        }
        pending.bootstrap = bootstrap;
        pending.bootstrapReady = true;
    }

    if (stagedVisual && !pending.hasDemodulation)
    {
        if (!allowContinuation || texture == nullptr || texture != pending.borrowedTexture)
        {
            state.FinishPending(metadata.slotIndex);
            const auto failure = CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Completion);
            state.SetError(failure);
            return failure;
        }
        bool shouldSubmit = unifiedVisual;
        const auto temporalStatus = remoteVisualLowFps ?
            state.BeginTemporalAttempt(pending, effectiveTimestamp, shouldSubmit) : CaptureStatus{};
        if (!temporalStatus)
        {
            if (remoteVisualLowFps)
            {
                state.temporalFrame = {};
            }
            state.FinishPending(metadata.slotIndex);
            state.SetError(temporalStatus);
            return temporalStatus;
        }
        if (!shouldSubmit)
        {
            CaptureDemodulatorResult result;
            result.kind = CaptureDemodulatorResultKind::TelemetryOnly;
            result.metadata = metadata;
            result.bootstrapRecord = pending.bootstrap.canonical44;
            result.bootstrap = pending.bootstrap;
            result.geometryStatus = ClassifyGeometry(pending.bootstrap, state.roiWidth, state.roiHeight);
            result.temporalDisposition = pending.temporalDisposition;
            state.FinishPendingWithResult(metadata.slotIndex, result);
            {
                const std::lock_guard lock(state.mutex);
                pbprotocol::SaturatingIncrementUnsigned(state.snapshot.bootstrapAcceptedFrames);
                pbprotocol::SaturatingIncrementUnsigned(state.snapshot.completedFrames);
            }
            return {};
        }
        const ScreenCaptureFrame frame{metadata, texture};
        DemodSubmission submission;
        DemodStatus submitStatus;
        {
            const std::lock_guard lock(state.demodulatorMutex);
            submitStatus = state.demodulator ? (remoteVisualLowFps ?
                state.demodulator->SubmitRemoteVisualLowFps(frame, context, pending.bootstrap.canonical44,
                    pending.bootstrap.geometry, state.config.remoteVisualLowFpsPolicy, submission) :
                state.demodulator->SubmitUnifiedVisual(frame, context, pending.bootstrap,
                    state.config.unifiedVisualPolicy, submission)) :
                    DemodStatus::Failure(DemodError::InvalidConfiguration, DemodStage::Submission);
        }
        if (!submitStatus)
        {
            state.SetDemodStatus(submitStatus);
            if (remoteVisualLowFps)
            {
                state.temporalFrame = {};
            }
            state.FinishPending(metadata.slotIndex);
            const auto failure = FromDemodStatus(submitStatus, CaptureStage::Completion);
            state.SetError(failure);
            return failure;
        }
        pending.hasDemodulation = true;
        pending.bootstrapAcceptedCounted = true;
        pending.submission = submission;
        {
            const std::lock_guard lock(state.mutex);
            pbprotocol::SaturatingIncrementUnsigned(state.snapshot.bootstrapAcceptedFrames);
            pbprotocol::SaturatingIncrementUnsigned(state.snapshot.stagedGpuSubmissions);
        }
        gpuWorkSubmitted = true;
        return {};
    }

    LARGE_INTEGER demodulationStart{};
    LARGE_INTEGER demodulationEnd{};
    const bool demodulationStartValid = QueryPerformanceCounter(&demodulationStart) != FALSE;
    DemodFrameResult demodulation;
    DemodPollResult poll;
    {
        const std::lock_guard lock(state.demodulatorMutex);
        poll = state.demodulator ? (stagedVisual ? state.demodulator->Poll(context, pending.submission, demodulation) :
            state.demodulator->PollUnbound(context, pending.submission, bootstrap.canonical44, demodulation)) :
            DemodPollResult{DemodStatus::Failure(DemodError::InvalidConfiguration, DemodStage::Completion), true};
    }
    const bool demodulationEndValid = QueryPerformanceCounter(&demodulationEnd) != FALSE;
    const auto demodulationTime = demodulationStartValid && demodulationEndValid ?
        QpcElapsed100ns(demodulationStart, demodulationEnd, state.qpcFrequency) : std::nullopt;
    if (!poll.ready)
    {
        const auto retired = RetireExternally();
        state.SetDemodStatus(retired);
        if (remoteVisualLowFps)
        {
            state.temporalFrame = {};
        }
        state.FinishPending(metadata.slotIndex);
        const auto failure = CaptureStatus::Failure(CaptureError::InternalError, CaptureStage::Completion);
        state.SetError(failure);
        return failure;
    }
    const bool bootstrapAcceptedCounted = pending.bootstrapAcceptedCounted;
    {
        const std::lock_guard lock(state.mutex);
        if (!bootstrapAcceptedCounted)
        {
            pbprotocol::SaturatingIncrementUnsigned(state.snapshot.bootstrapAcceptedFrames);
        }
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
            poll.status.code == DemodError::CalibrationFailure || (stagedVisual && poll.status.code == DemodError::InvalidFrame);
        if (visualErasure)
        {
            CaptureDemodulatorResult result;
            result.kind = CaptureDemodulatorResultKind::TelemetryOnly;
            result.metadata = metadata;
            result.bootstrapRecord = bootstrap.canonical44;
            result.bootstrap = bootstrap;
            result.geometryStatus = stagedVisual ? ClassifyGeometry(bootstrap, state.roiWidth, state.roiHeight) :
                CaptureDemodulatorGeometryStatus::NotApplicable;
            const auto temporalStatus = remoteVisualLowFps ? state.CompleteTemporalAttempt(pending, demodulation, result) :
                CaptureStatus{};
            if (!temporalStatus)
            {
                state.FinishPending(metadata.slotIndex);
                state.temporalFrame = {};
                state.SetError(temporalStatus);
                return temporalStatus;
            }
            state.FinishPendingWithResult(metadata.slotIndex, result);
            {
                const std::lock_guard lock(state.mutex);
                pbprotocol::SaturatingIncrementUnsigned(state.snapshot.completedFrames);
            }
            return {};
        }
        if (remoteVisualLowFps)
        {
            state.temporalFrame = {};
        }
        state.FinishPending(metadata.slotIndex);
        const auto failure = FromDemodStatus(poll.status, CaptureStage::Completion);
        state.SetError(failure);
        return failure;
    }

    CaptureDemodulatorResult result;
    result.metadata = metadata;
    result.bootstrapRecord = bootstrap.canonical44;
    result.bootstrap = bootstrap;
    result.geometryStatus = stagedVisual ? ClassifyGeometry(bootstrap, state.roiWidth, state.roiHeight) :
        CaptureDemodulatorGeometryStatus::NotApplicable;
    result.demodulation = demodulation;
    CaptureStatus temporalStatus;
    if (remoteVisualLowFps)
    {
        temporalStatus = state.CompleteTemporalAttempt(pending, demodulation, result);
    }
    else if (!unifiedVisual)
    {
        result.admittedTransportBlockCount = demodulation.acceptedTransportBlockCount;
        for (std::uint32_t index = 0; index < result.admittedTransportBlockCount; index++)
        {
            result.admittedTransportBlockIndices[index] = index;
        }
        result.admittedRemoteControlBlockCount = demodulation.acceptedRemoteControlBlockCount;
        for (std::uint32_t index = 0; index < result.admittedRemoteControlBlockCount; index++)
        {
            result.admittedRemoteControlBlockIndices[index] = index;
        }
    }
    if (!temporalStatus)
    {
        state.temporalFrame = {};
        state.FinishPending(metadata.slotIndex);
        state.SetError(temporalStatus);
        return temporalStatus;
    }
    const bool remoteControl = !unifiedVisual && result.admittedRemoteControlBlockCount == 1;
    result.kind = unifiedVisual ? CaptureDemodulatorResultKind::UnifiedFrame :
        remoteControl ? CaptureDemodulatorResultKind::ControlRecord :
            (!remoteVisualLowFps || result.temporalDisposition == CaptureDemodulatorTemporalDisposition::Unique ||
                result.admittedTransportBlockCount != 0 ? CaptureDemodulatorResultKind::Transport :
                    CaptureDemodulatorResultKind::TelemetryOnly);
    if (remoteControl)
    {
        const auto controlIndex = result.admittedRemoteControlBlockIndices[0];
        result.controlByteCount = demodulation.acceptedRemoteControlBlocks[controlIndex].byteCount;
        std::copy_n(demodulation.acceptedRemoteControlBlocks[controlIndex].bytes.begin(), result.controlByteCount,
            result.controlBytes.begin());
    }
    state.FinishPendingWithResult(metadata.slotIndex, result);
    {
        const std::lock_guard lock(state.mutex);
        pbprotocol::SaturatingIncrementUnsigned(state.snapshot.completedFrames);
        state.snapshot.acceptedTransportBlocks = pbprotocol::SaturatingAddUnsigned(state.snapshot.acceptedTransportBlocks,
            static_cast<std::uint64_t>(result.admittedTransportBlockCount));
        state.snapshot.acceptedUnifiedBlocks = pbprotocol::SaturatingAddUnsigned(state.snapshot.acceptedUnifiedBlocks,
            static_cast<std::uint64_t>(demodulation.acceptedUnifiedBlockCount));
        if (unifiedVisual)
        {
            if (demodulation.unifiedObservation.IsFrameAvailable() &&
                demodulation.unifiedObservation.acceptedBlocks == pbmodulation::kUnifiedCodewordCount)
            {
                pbprotocol::SaturatingIncrementUnsigned(state.snapshot.verifiedFrames);
            }
            else
            {
                pbprotocol::SaturatingIncrementUnsigned(state.snapshot.postFecFailedFrames);
            }
        }
        else if (remoteControl)
        {
            pbprotocol::SaturatingIncrementUnsigned(state.snapshot.controlFrames);
        }
        else if ((!remoteVisualLowFps || result.temporalDisposition == CaptureDemodulatorTemporalDisposition::Unique) &&
            demodulation.evaluation.IsVerified())
        {
            pbprotocol::SaturatingIncrementUnsigned(state.snapshot.verifiedFrames);
        }
        else if (!remoteVisualLowFps || result.temporalDisposition == CaptureDemodulatorTemporalDisposition::Unique)
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
    // LF4 Bootstrap-only suppression can finish a later slot while an earlier slot
    // still has GPU work pending. Preserve capture-observation order without adding
    // an unbounded reorder buffer or weakening the telemetry monotonicity contract.
    std::size_t selectedOffset = 0;
    std::uint64_t selectedObservation = state.results[state.resultHead].metadata.captureObservation;
    for (std::size_t offset = 1; offset < state.resultSize; offset++)
    {
        const std::size_t index = (state.resultHead + offset) % state.results.size();
        const std::uint64_t observation = state.results[index].metadata.captureObservation;
        if (observation < selectedObservation)
        {
            selectedOffset = offset;
            selectedObservation = observation;
        }
    }
    const bool earlierCompletionPending = std::ranges::any_of(state.pendingCaptureObservations,
        [selectedObservation](const std::uint64_t observation)
        {
            return observation != 0 && observation < selectedObservation;
        });
    if (earlierCompletionPending)
    {
        return false;
    }
    const std::size_t selectedIndex = (state.resultHead + selectedOffset) % state.results.size();
    output = state.results[selectedIndex];
    for (std::size_t offset = selectedOffset; offset > 0; offset--)
    {
        const std::size_t destination = (state.resultHead + offset) % state.results.size();
        const std::size_t source = (state.resultHead + offset - 1) % state.results.size();
        state.results[destination] = std::move(state.results[source]);
    }
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
