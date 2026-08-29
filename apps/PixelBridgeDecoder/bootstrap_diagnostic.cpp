#include "bootstrap_diagnostic.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/checked_integer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

namespace pbdecoder
{
using namespace pbcapturenormalize;

namespace
{
bool SameObservation(const ScreenCaptureFrameMetadata& first, const ScreenCaptureFrameMetadata& second) noexcept
{
    return first.domain == second.domain && first.captureObservation == second.captureObservation && first.sourceGeneration == second.sourceGeneration &&
           first.slotIndex == second.slotIndex && first.slotGeneration == second.slotGeneration;
}

bool SameTransform(const pbmodulation::LocalDesktopGeometry& first, const pbmodulation::LocalDesktopGeometry& second) noexcept
{
    return first.originX == second.originX && first.originY == second.originY && first.scaleX == second.scaleX && first.scaleY == second.scaleY;
}
} // namespace

BootstrapDiagnosticProcessor::BootstrapDiagnosticProcessor(const pbmodulation::LocalDesktopDecodePolicy& policy) noexcept : policy_(policy)
{
}

struct BootstrapDiagnosticProcessor::PhysicalLayerState
{
    enum class Mode : std::uint8_t
    {
        DesktopLevels,
        ShapeChroma
    };

    explicit PhysicalLayerState(const Mode selectedMode) noexcept : mode(selectedMode)
    {
    }

    [[nodiscard]] bool AddShapeChroma(const pbdesktoplevels::ShapeChromaReferenceObservation& observation,
        const std::uint64_t sequence) noexcept
    {
        const auto shapeHistogram = channel.GetShapeMarginHistogram();
        const auto chroma = channel.GetChromaMarginHistogram();
        if (shapeHistogram.size() != pbmodulation::kShapeChromaMetricBins || chroma.size() != chromaHistogram.size() ||
            shapeHistogram.data() == nullptr || chroma.data() == nullptr || !std::isfinite(observation.modulation.chromaMargin.minimum) ||
            observation.modulation.chromaMargin.minimum < 0 || observation.modulation.chromaMargin.minimum > 1)
        {
            return false;
        }
        const auto chromaSummary = pbmodulation::SummarizeShapeChromaMargin(chroma, observation.modulation.chromaMargin.minimum);
        const auto nextSamples = pbprotocol::CheckedAddUnsigned(chromaSamples, chromaSummary.samples);
        if (chromaSummary.samples != pbmodulation::kShapeChromaTileCount || !nextSamples)
        {
            return false;
        }
        for (std::size_t index = 0; index < chromaHistogram.size(); index++)
        {
            if (chroma[index] > std::numeric_limits<std::uint64_t>::max() - chromaHistogram[index])
            {
                return false;
            }
        }
        if (!shapeStatistics.Add(observation.evaluation, sequence, shapeHistogram, observation.modulation.shapeMargin.minimum))
        {
            return false;
        }
        for (std::size_t index = 0; index < chromaHistogram.size(); index++)
        {
            chromaHistogram[index] += chroma[index];
        }
        chromaMinimum = chromaSamples == 0 ? observation.modulation.chromaMargin.minimum :
            std::min(chromaMinimum, observation.modulation.chromaMargin.minimum);
        chromaSamples = nextSamples.Value();
        return true;
    }

    [[nodiscard]] pbmodulation::ShapeChromaMargin GetChromaMargin() const noexcept
    {
        return pbmodulation::SummarizeShapeChromaMargin(chromaHistogram, chromaSamples == 0 ? 0 : chromaMinimum);
    }

    Mode mode;
    pbdesktoplevels::ReferenceChannel channel;
    std::array<pbdesktoplevels::ReferenceStatistics, 2> statistics;
    pbdesktoplevels::ReferenceStatistics shapeStatistics;
    pbmodulation::DesktopLevelsCalibration levelsCalibration;
    pbmodulation::ShapeChromaCalibration shapeCalibration;
    std::array<std::uint64_t, pbmodulation::kShapeChromaMetricBins> chromaHistogram{};
    std::uint64_t chromaSamples = 0;
    double chromaMinimum = 0;
};

BootstrapDiagnosticProcessor::~BootstrapDiagnosticProcessor() = default;

std::uint64_t BootstrapDiagnosticProcessor::ProcessingReservedBytes() const noexcept
{
    return physical_ ? pbdesktoplevels::kProcessingReservationBytes : 0;
}

CaptureStatus BootstrapDiagnosticProcessor::CreateDesktopLevels(std::shared_ptr<BootstrapDiagnosticProcessor>& output) noexcept
{
    // ReferenceChannel's 16 MiB reservation includes 1 MiB for this bounded
    // application state, including events/history and shared_ptr bookkeeping.
    static_assert(sizeof(BootstrapDiagnosticProcessor) + sizeof(PhysicalLayerState) + 4096 <= 1024 * 1024);
    try
    {
        auto channel = pbdesktoplevels::ReferenceChannel::Create(pbdesktoplevels::kProcessingReservationBytes);
        if (!channel)
        {
            return CaptureStatus::Failure(CaptureError::OutOfMemory, CaptureStage::Configuration);
        }
        auto processor = std::make_shared<BootstrapDiagnosticProcessor>();
        processor->physical_ = std::make_unique<PhysicalLayerState>(PhysicalLayerState::Mode::DesktopLevels);
        processor->physical_->channel = std::move(channel).Value();
        processor->snapshot_.desktopLevels = true;
        output = std::move(processor);
        return {};
    }
    catch (const std::bad_alloc&)
    {
        return CaptureStatus::Failure(CaptureError::OutOfMemory, CaptureStage::Configuration);
    }
}

CaptureStatus BootstrapDiagnosticProcessor::CreateShapeChroma(std::shared_ptr<BootstrapDiagnosticProcessor>& output) noexcept
{
    static_assert(sizeof(BootstrapDiagnosticProcessor) + sizeof(PhysicalLayerState) + 4096 <= 1024 * 1024);
    try
    {
        auto channel = pbdesktoplevels::ReferenceChannel::Create(pbdesktoplevels::kProcessingReservationBytes);
        if (!channel)
        {
            return CaptureStatus::Failure(CaptureError::OutOfMemory, CaptureStage::Configuration);
        }
        auto processor = std::make_shared<BootstrapDiagnosticProcessor>();
        processor->physical_ = std::make_unique<PhysicalLayerState>(PhysicalLayerState::Mode::ShapeChroma);
        processor->physical_->channel = std::move(channel).Value();
        processor->snapshot_.shapeChroma = true;
        output = std::move(processor);
        return {};
    }
    catch (const std::bad_alloc&)
    {
        return CaptureStatus::Failure(CaptureError::OutOfMemory, CaptureStage::Configuration);
    }
}

void BootstrapDiagnosticProcessor::Reset(std::optional<ScreenCaptureDomain> domain) noexcept
{
    const std::lock_guard lock(mutex_);
    if (pendingReady_)
    {
        pbprotocol::SaturatingIncrementUnsigned(snapshot_.discardedCandidates);
    }
    pendingReady_ = false;
    pendingPixelDigestValid_ = false;
    pendingBootstrapRecovered_ = false;
    pending_ = {};
    snapshot_.domain = std::move(domain);
    pbprotocol::SaturatingIncrementUnsigned(snapshot_.resets);
    snapshot_.geometryGeneration = 0;
    snapshot_.calibrationGeneration = 0;
    snapshot_.trackedSessions = 0;
    snapshot_.retainedSequences = 0;
    if (!physical_)
    {
        snapshot_.diagnosticQueueDrops = pbprotocol::SaturatingAddUnsigned(snapshot_.diagnosticQueueDrops, static_cast<std::uint64_t>(eventCount_));
        snapshot_.queuedEvents = 0;
        eventHead_ = 0;
        eventCount_ = 0;
    }
    // Physical-layer modes keep immutable, already-committed audit events with their
    // original domains until drained. This is NOT live calibration/history and
    // cannot be re-committed. In particular shutdown must not erase the last
    // admitted failure from the raw denominator evidence. Queue remains bounded.
    history_ = {};
    sessions_ = {};
    nextHistory_ = 0;
    geometry_.reset();
    calibrated_ = false;
    blackLevel_ = 0;
    whiteLevel_ = 0;
    lastObservation_ = 0;
    if (snapshot_.domain)
    {
        if (!telemetry_.BeginCaptureEpoch(*snapshot_.domain, 0))
        {
            pbprotocol::SaturatingIncrementUnsigned(snapshot_.telemetryFailures);
        }
    }
    else
    {
        telemetry_.EndCaptureEpoch();
    }
    if (physical_)
    {
        physical_->levelsCalibration = {};
        physical_->shapeCalibration = {};
    }
}

CaptureStatus BootstrapDiagnosticProcessor::Analyze(const ScreenCaptureFrameMetadata& metadata, const std::span<const std::byte> pixels, const std::size_t rowPitch)
{
    if (pendingReady_)
    {
        return CaptureStatus::Failure(CaptureError::ConsumerFailure, CaptureStage::Consumer);
    }
    pending_ = {};
    pendingPixelDigestValid_ = false;
    pendingBootstrapRecovered_ = false;
    pending_.desktopLevels = physical_ && physical_->mode == PhysicalLayerState::Mode::DesktopLevels;
    pending_.shapeChroma = physical_ && physical_->mode == PhysicalLayerState::Mode::ShapeChroma;
    pending_.capture = metadata;
    pendingReady_ = true;
    pending_.disposition = BootstrapDisposition::InvalidMetadata;
    if (metadata.roiSize.width <= 0 || metadata.roiSize.height <= 0 || metadata.roiSize.width > 16384 || metadata.roiSize.height > 16384 ||
        !metadata.isCursorExcluded || metadata.captureObservation == 0 || metadata.sourceGeneration == 0 || metadata.slotGeneration == 0)
    {
        return {};
    }
    const auto physicalWidth = static_cast<std::int64_t>(metadata.physicalRoi.right) - metadata.physicalRoi.left;
    const auto physicalHeight = static_cast<std::int64_t>(metadata.physicalRoi.bottom) - metadata.physicalRoi.top;
    if (physicalWidth != metadata.roiSize.width || physicalHeight != metadata.roiSize.height)
    {
        return {};
    }
    pending_.disposition = BootstrapDisposition::UnsupportedSignal;
    if (metadata.hdr || metadata.signalEncoding == CaptureSignalEncoding::Unknown)
    {
        return {};
    }
    pbmodulation::LumaPixelFormat format;
    std::uint64_t pixelBytes = 4;
    if (metadata.pixelFormat == DXGI_FORMAT_B8G8R8A8_UNORM && metadata.signalEncoding == CaptureSignalEncoding::SdrRgb)
    {
        format = pbmodulation::LumaPixelFormat::Bgra8;
    }
    else if (metadata.pixelFormat == DXGI_FORMAT_R10G10B10A2_UNORM && metadata.signalEncoding == CaptureSignalEncoding::SdrRgb)
    {
        format = pbmodulation::LumaPixelFormat::R10G10B10A2;
    }
    else if (metadata.pixelFormat == DXGI_FORMAT_R16G16B16A16_FLOAT && metadata.signalEncoding == CaptureSignalEncoding::LinearScRgb)
    {
        format = pbmodulation::LumaPixelFormat::Fp16LinearSdr;
        pixelBytes = 8;
    }
    else
    {
        return {};
    }
    const auto rowBytes = pbprotocol::CheckedMultiplyUnsigned(static_cast<std::uint64_t>(metadata.roiSize.width), pixelBytes);
    const auto byteCount = rowBytes ? pbprotocol::CheckedMultiplyUnsigned(rowBytes.Value(), static_cast<std::uint64_t>(metadata.roiSize.height)) : rowBytes;
    if (!rowBytes || !byteCount || rowBytes.Value() != rowPitch || byteCount.Value() != pixels.size())
    {
        pending_.disposition = BootstrapDisposition::InvalidMetadata;
        return {};
    }
    const pbmodulation::LumaView view{pixels, static_cast<std::uint32_t>(metadata.roiSize.width), static_cast<std::uint32_t>(metadata.roiSize.height), rowPitch, format};
    pending_.pixelDigest = pbprotocol::ComputeBlake3Digest(pixels);
    pendingPixelDigestValid_ = true;
    if (pending_.desktopLevels)
    {
        pending_.levels = physical_->channel.Decode(view);
        pending_.visual = pending_.levels.modulation.bootstrap;
    }
    else if (pending_.shapeChroma)
    {
        pending_.shape = physical_->channel.DecodeShapeChroma(view);
        pending_.visual = pending_.shape.modulation.bootstrap;
    }
    else
    {
        pending_.visual = pbmodulation::DecodeLocalDesktopBootstrap(view, policy_);
    }
    if (!pending_.visual.IsAccepted())
    {
        pending_.disposition = BootstrapDisposition::VisualErasure;
        return {};
    }
    const auto record = pbprotocol::ParseBootstrapRecord(pending_.visual.canonical44);
    const bool bindingValid = record && (pending_.desktopLevels ?
        pbmodulation::GetDesktopLevelsProfile(record.Value().visualProfileId) != nullptr && record.Value().visualLayoutVersion == pbmodulation::kDesktopLevelsLayoutVersion :
        pending_.shapeChroma ? record.Value().visualProfileId == pbmodulation::kShapeChromaProfileId &&
            record.Value().visualLayoutVersion == pbmodulation::kShapeChromaLayoutVersion :
        record.Value().visualProfileId == pbmodulation::kLocalDesktopVisualProfileId && record.Value().visualLayoutVersion == pbmodulation::kLocalDesktopLayoutVersion);
    if (!bindingValid)
    {
        // The portable visual decoder already validates this. Fail closed if
        // its future implementation ever violates its accepted-result contract.
        pending_.disposition = BootstrapDisposition::InvalidMetadata;
        return {};
    }
    pending_.bootstrap = record.Value();
    pendingBootstrapRecovered_ = true;
    if ((pending_.desktopLevels && (!pending_.levels.modulation.IsAccepted() || !pending_.levels.evaluation.evaluated)) ||
        (pending_.shapeChroma && (!pending_.shape.modulation.IsAccepted() || !pending_.shape.evaluation.evaluated)))
    {
        pending_.disposition = BootstrapDisposition::VisualErasure;
        return {};
    }
    pending_.disposition = BootstrapDisposition::Accepted;
    return {};
}

BootstrapDisposition BootstrapDiagnosticProcessor::AdmitLocked(BootstrapDiagnosticEvent& event) noexcept
{
    const auto tag = event.bootstrap.sessionTag.value;
    const auto sequence = event.bootstrap.frameSequence;
    const auto foundIterator = std::ranges::find_if(history_, [tag, sequence](const HistoryEntry& entry)
    {
        return entry.active && entry.sessionTag == tag && entry.sequence == sequence;
    });
    HistoryEntry* const found = foundIterator == history_.end() ? nullptr : &*foundIterator;
    if (found != nullptr && (found->conflict || found->canonical44 != event.visual.canonical44))
    {
        found->conflict = true;
        return BootstrapDisposition::IdentityConflict;
    }
    SessionEntry* session = nullptr;
    SessionEntry* emptySession = nullptr;
    for (auto& entry : sessions_)
    {
        if (entry.active && entry.tag == tag)
        {
            session = &entry;
        }
        if (!entry.active && emptySession == nullptr)
        {
            emptySession = &entry;
        }
    }
    if (session == nullptr && emptySession == nullptr)
    {
        return BootstrapDisposition::SessionLimit;
    }
    if (session != nullptr && session->profileId != event.bootstrap.visualProfileId)
    {
        return BootstrapDisposition::IdentityConflict;
    }
    if (found == nullptr && session != nullptr && sequence <= session->highestSequence)
    {
        // An evicted sequence is not magically a new independent observation.
        // Retain a bounded per-session high-water instead of accepting replays.
        return BootstrapDisposition::StaleSequence;
    }
    const bool geometryChanged = !geometry_ || !SameTransform(*geometry_, event.visual.geometry);
    const bool calibrationChanged = !calibrated_ || blackLevel_ != event.visual.blackLevel || whiteLevel_ != event.visual.whiteLevel ||
        (event.desktopLevels && physical_->levelsCalibration != event.levels.modulation.calibration) ||
        (event.shapeChroma && physical_->shapeCalibration != event.shape.modulation.calibration);
    if ((geometryChanged && snapshot_.geometryGeneration == std::numeric_limits<std::uint64_t>::max()) ||
        (calibrationChanged && snapshot_.calibrationGeneration == std::numeric_limits<std::uint64_t>::max()))
    {
        return BootstrapDisposition::GenerationExhausted;
    }
    if (geometryChanged)
    {
        snapshot_.geometryGeneration++;
        geometry_ = event.visual.geometry;
    }
    if (calibrationChanged)
    {
        snapshot_.calibrationGeneration++;
        calibrated_ = true;
        blackLevel_ = event.visual.blackLevel;
        whiteLevel_ = event.visual.whiteLevel;
        if (event.desktopLevels)
        {
            physical_->levelsCalibration = event.levels.modulation.calibration;
        }
        else if (event.shapeChroma)
        {
            physical_->shapeCalibration = event.shape.modulation.calibration;
        }
    }
    event.geometryGeneration = snapshot_.geometryGeneration;
    event.calibrationGeneration = snapshot_.calibrationGeneration;
    if (found != nullptr)
    {
        const auto disposition = found->geometryGeneration != event.geometryGeneration ? BootstrapDisposition::DuplicateGeometryChanged :
                                 found->calibrationGeneration != event.calibrationGeneration ? BootstrapDisposition::DuplicateCalibrationChanged :
                                 found->pixelDigest == event.pixelDigest ? BootstrapDisposition::DuplicatePixels : BootstrapDisposition::DuplicateObservation;
        // Only replace the diagnostic fingerprint/domain-compatible generation.
        // There is no confidence accumulation, retry codeword merge or LLR sum.
        found->pixelDigest = event.pixelDigest;
        found->geometryGeneration = event.geometryGeneration;
        found->calibrationGeneration = event.calibrationGeneration;
        return disposition;
    }
    if (session == nullptr)
    {
        session = emptySession;
        *session = {true, tag, sequence, event.bootstrap.visualProfileId};
        snapshot_.trackedSessions++;
    }
    else
    {
        session->highestSequence = sequence;
    }
    history_[nextHistory_] = {true, false, tag, sequence, event.visual.canonical44, event.pixelDigest, event.geometryGeneration, event.calibrationGeneration};
    nextHistory_ = (nextHistory_ + 1) % history_.size();
    snapshot_.retainedSequences = std::min<std::uint32_t>(snapshot_.retainedSequences + 1, static_cast<std::uint32_t>(historyCapacity));
    return BootstrapDisposition::Accepted;
}

void BootstrapDiagnosticProcessor::PublishLocked(const BootstrapDiagnosticEvent& event) noexcept
{
    if (eventCount_ == eventCapacity)
    {
        eventHead_ = (eventHead_ + 1) % eventCapacity;
        eventCount_--;
        pbprotocol::SaturatingIncrementUnsigned(snapshot_.diagnosticQueueDrops);
    }
    events_[(eventHead_ + eventCount_) % eventCapacity] = event;
    eventCount_++;
    snapshot_.queuedEvents = static_cast<std::uint32_t>(eventCount_);
    snapshot_.lastDisposition = event.disposition;
}

void BootstrapDiagnosticProcessor::Commit(const ScreenCaptureFrameMetadata& metadata) noexcept
{
    const std::lock_guard lock(mutex_);
    if (!pendingReady_ || !SameObservation(pending_.capture, metadata) || !snapshot_.domain || metadata.domain != *snapshot_.domain ||
        metadata.captureObservation <= lastObservation_)
    {
        pbprotocol::SaturatingIncrementUnsigned(snapshot_.staleCommits);
        if (pendingReady_)
        {
            pbprotocol::SaturatingIncrementUnsigned(snapshot_.discardedCandidates);
        }
        pendingReady_ = false;
        pendingPixelDigestValid_ = false;
        pendingBootstrapRecovered_ = false;
        return;
    }
    lastObservation_ = metadata.captureObservation;
    pbtelemetry::CaptureSample captureSample{metadata.domain, metadata.captureObservation, metadata.timestamp.monotonic100ns,
        metadata.roiCopyTime100ns, std::nullopt};
    if (pendingPixelDigestValid_)
    {
        captureSample.pixelDigest = pending_.pixelDigest;
    }
    const auto captureTelemetryStatus = telemetry_.RecordCapture(captureSample);
    if (!captureTelemetryStatus)
    {
        pbprotocol::SaturatingIncrementUnsigned(snapshot_.telemetryFailures);
    }
    else
    {
        pbtelemetry::BootstrapSample bootstrapSample{metadata.domain, metadata.captureObservation, pendingBootstrapRecovered_};
        if (pendingBootstrapRecovered_)
        {
            bootstrapSample.scaleX = pending_.visual.geometry.scaleX;
            bootstrapSample.scaleY = pending_.visual.geometry.scaleY;
            bootstrapSample.phaseX = pending_.visual.geometry.originX - std::round(pending_.visual.geometry.originX);
            bootstrapSample.phaseY = pending_.visual.geometry.originY - std::round(pending_.visual.geometry.originY);
        }
        if (!telemetry_.RecordBootstrap(bootstrapSample))
        {
            pbprotocol::SaturatingIncrementUnsigned(snapshot_.telemetryFailures);
        }
    }
    pending_.geometryGeneration = snapshot_.geometryGeneration;
    pending_.calibrationGeneration = snapshot_.calibrationGeneration;
    if (pending_.disposition == BootstrapDisposition::Accepted)
    {
        pending_.disposition = AdmitLocked(pending_);
    }
    const auto* const physicalEvaluation = pending_.desktopLevels ? &pending_.levels.evaluation :
        pending_.shapeChroma ? &pending_.shape.evaluation : nullptr;
    if (captureTelemetryStatus && physicalEvaluation != nullptr && pending_.disposition == BootstrapDisposition::Accepted && physicalEvaluation->evaluated &&
        !telemetry_.RecordFec({metadata.domain, metadata.captureObservation, *physicalEvaluation}))
    {
        pbprotocol::SaturatingIncrementUnsigned(snapshot_.telemetryFailures);
    }
    if (pending_.desktopLevels)
    {
        const auto& observation = pending_.levels.modulation;
        const bool knownProfile = pbmodulation::GetDesktopLevelsProfile(observation.profileId) != nullptr;
        const std::size_t candidate = observation.profileId == pbmodulation::kDesktopLevels2ProfileId ? 0 : 1;
        if (pending_.disposition == BootstrapDisposition::Accepted)
        {
            if (!physical_->statistics[candidate].Add(pending_.levels.evaluation, pending_.bootstrap.frameSequence,
                    physical_->channel.GetMarginHistogram(), observation.margin.minimum))
            {
                pending_.disposition = BootstrapDisposition::StatisticsFailure;
                pbprotocol::SaturatingIncrementUnsigned(snapshot_.statisticsFailures);
            }
            else if (!pending_.levels.evaluation.IsVerified())
            {
                // Admission and deduplication already happened: capturing this
                // failed identity again cannot improve a denominator or retry it.
                pending_.disposition = BootstrapDisposition::PostFecFailure;
            }
        }
        if (pending_.disposition == BootstrapDisposition::VisualErasure)
        {
            if (!observation.bootstrap.IsAccepted())
            {
                pbprotocol::SaturatingIncrementUnsigned(snapshot_.unrecognizedBootstrap);
            }
            else if (knownProfile)
            {
                using Erasure = pbmodulation::DesktopLevelsErasure;
                if (observation.erasure == Erasure::ScaleOutOfRange || observation.erasure == Erasure::AlignmentOutOfRange || observation.erasure == Erasure::FrameOutOfBounds)
                {
                    pbprotocol::SaturatingIncrementUnsigned(snapshot_.candidates[candidate].geometryErasures);
                }
                else if (observation.erasure == Erasure::PilotClipping || observation.erasure == Erasure::PilotOrder || observation.erasure == Erasure::PilotVariance ||
                    observation.erasure == Erasure::PilotSpatialMismatch || observation.erasure == Erasure::PhasePilotMismatch)
                {
                    pbprotocol::SaturatingIncrementUnsigned(snapshot_.candidates[candidate].pilotErasures);
                }
                else
                {
                    pbprotocol::SaturatingIncrementUnsigned(snapshot_.candidates[candidate].otherErasures);
                }
            }
        }
        if (knownProfile && (pending_.disposition == BootstrapDisposition::DuplicatePixels || pending_.disposition == BootstrapDisposition::DuplicateObservation ||
            pending_.disposition == BootstrapDisposition::DuplicateGeometryChanged || pending_.disposition == BootstrapDisposition::DuplicateCalibrationChanged))
        {
            pbprotocol::SaturatingIncrementUnsigned(snapshot_.candidates[candidate].duplicates);
        }
    }
    else if (pending_.shapeChroma)
    {
        const auto& observation = pending_.shape.modulation;
        if (pending_.disposition == BootstrapDisposition::Accepted)
        {
            if (!physical_->AddShapeChroma(pending_.shape, pending_.bootstrap.frameSequence))
            {
                pending_.disposition = BootstrapDisposition::StatisticsFailure;
                pbprotocol::SaturatingIncrementUnsigned(snapshot_.statisticsFailures);
            }
            else if (!pending_.shape.evaluation.IsVerified())
            {
                pending_.disposition = BootstrapDisposition::PostFecFailure;
            }
        }
        if (pending_.disposition == BootstrapDisposition::VisualErasure)
        {
            if (!observation.bootstrap.IsAccepted())
            {
                pbprotocol::SaturatingIncrementUnsigned(snapshot_.unrecognizedBootstrap);
            }
            else
            {
                using Erasure = pbmodulation::ShapeChromaErasure;
                if (observation.erasure == Erasure::ScaleOutOfRange || observation.erasure == Erasure::AlignmentOutOfRange ||
                    observation.erasure == Erasure::FrameOutOfBounds)
                {
                    pbprotocol::SaturatingIncrementUnsigned(snapshot_.shape.geometryErasures);
                }
                else if (observation.erasure == Erasure::ChromaPilotClipping || observation.erasure == Erasure::ChromaPilotVariance ||
                    observation.erasure == Erasure::ChromaPilotSeparation || observation.erasure == Erasure::ChromaPilotSpatialMismatch)
                {
                    pbprotocol::SaturatingIncrementUnsigned(snapshot_.shape.pilotErasures);
                }
                else
                {
                    pbprotocol::SaturatingIncrementUnsigned(snapshot_.shape.otherErasures);
                }
            }
        }
        if (pending_.disposition == BootstrapDisposition::DuplicatePixels || pending_.disposition == BootstrapDisposition::DuplicateObservation ||
            pending_.disposition == BootstrapDisposition::DuplicateGeometryChanged || pending_.disposition == BootstrapDisposition::DuplicateCalibrationChanged)
        {
            pbprotocol::SaturatingIncrementUnsigned(snapshot_.shape.duplicates);
        }
    }
    pbprotocol::SaturatingIncrementUnsigned(snapshot_.observations);
    switch (pending_.disposition)
    {
    case BootstrapDisposition::Accepted: pbprotocol::SaturatingIncrementUnsigned(snapshot_.accepted); break;
    case BootstrapDisposition::DuplicatePixels:
    case BootstrapDisposition::DuplicateObservation:
    case BootstrapDisposition::DuplicateGeometryChanged:
    case BootstrapDisposition::DuplicateCalibrationChanged: pbprotocol::SaturatingIncrementUnsigned(snapshot_.duplicates); break;
    case BootstrapDisposition::IdentityConflict:
        pbprotocol::SaturatingIncrementUnsigned(snapshot_.identityConflicts);
        [[fallthrough]];
    default: pbprotocol::SaturatingIncrementUnsigned(snapshot_.erasures); break;
    }
    PublishLocked(pending_);
    pendingReady_ = false;
    pendingPixelDigestValid_ = false;
    pendingBootstrapRecovered_ = false;
}

void BootstrapDiagnosticProcessor::Discard() noexcept
{
    const std::lock_guard lock(mutex_);
    if (pendingReady_)
    {
        pbprotocol::SaturatingIncrementUnsigned(snapshot_.discardedCandidates);
    }
    pendingReady_ = false;
    pendingPixelDigestValid_ = false;
    pendingBootstrapRecovered_ = false;
}

BootstrapDiagnosticSnapshot BootstrapDiagnosticProcessor::GetSnapshot() const noexcept
{
    const std::lock_guard lock(mutex_);
    auto snapshot = snapshot_;
    if (physical_ && physical_->mode == PhysicalLayerState::Mode::DesktopLevels)
    {
        for (std::size_t index = 0; index < snapshot.candidates.size(); index++)
        {
            snapshot.candidates[index].statistics = physical_->statistics[index].GetSummary();
        }
    }
    else if (physical_ && physical_->mode == PhysicalLayerState::Mode::ShapeChroma)
    {
        snapshot.shape.statistics = physical_->shapeStatistics.GetSummary();
        snapshot.shape.chromaMargin = physical_->GetChromaMargin();
    }
    snapshot.telemetry = telemetry_.GetSnapshot();
    return snapshot;
}

bool BootstrapDiagnosticProcessor::TakeEvent(BootstrapDiagnosticEvent& output) noexcept
{
    const std::lock_guard lock(mutex_);
    if (eventCount_ == 0)
    {
        return false;
    }
    output = events_[eventHead_];
    eventHead_ = (eventHead_ + 1) % eventCapacity;
    eventCount_--;
    snapshot_.queuedEvents = static_cast<std::uint32_t>(eventCount_);
    return true;
}

const char* GetBootstrapDispositionName(const BootstrapDisposition disposition) noexcept
{
    switch (disposition)
    {
    case BootstrapDisposition::None: return "None";
    case BootstrapDisposition::Accepted: return "Accepted";
    case BootstrapDisposition::DuplicatePixels: return "DuplicatePixels";
    case BootstrapDisposition::DuplicateObservation: return "DuplicateObservation";
    case BootstrapDisposition::DuplicateGeometryChanged: return "DuplicateGeometryChanged";
    case BootstrapDisposition::DuplicateCalibrationChanged: return "DuplicateCalibrationChanged";
    case BootstrapDisposition::VisualErasure: return "VisualErasure";
    case BootstrapDisposition::UnsupportedSignal: return "UnsupportedSignal";
    case BootstrapDisposition::InvalidMetadata: return "InvalidMetadata";
    case BootstrapDisposition::IdentityConflict: return "IdentityConflict";
    case BootstrapDisposition::StaleSequence: return "StaleSequence";
    case BootstrapDisposition::SessionLimit: return "SessionLimit";
    case BootstrapDisposition::GenerationExhausted: return "GenerationExhausted";
    case BootstrapDisposition::PostFecFailure: return "PostFecFailure";
    case BootstrapDisposition::StatisticsFailure: return "StatisticsFailure";
    }
    return "Unknown";
}

bool IsTerminalDiagnosticFailure(const CaptureSnapshot& capture, const DiagnosticReadbackSnapshot& readback) noexcept
{
    return capture.state == CaptureState::Failed || capture.state == CaptureState::Stopped || !readback.error;
}

int GetBootstrapDiagnosticSuccessExitCode(const BootstrapDiagnosticSnapshot& snapshot) noexcept
{
    if (snapshot.desktopLevels)
    {
        if (snapshot.statisticsFailures != 0 || snapshot.identityConflicts != 0 || snapshot.candidates[0].statistics.falseAcceptedCodewords != 0 ||
            snapshot.candidates[1].statistics.falseAcceptedCodewords != 0)
        {
            return 1;
        }
        return snapshot.candidates[0].statistics.verifiedFrames != 0 || snapshot.candidates[1].statistics.verifiedFrames != 0 ? 0 : 4;
    }
    if (snapshot.shapeChroma)
    {
        if (snapshot.statisticsFailures != 0 || snapshot.identityConflicts != 0 || snapshot.shape.statistics.falseAcceptedCodewords != 0)
        {
            return 1;
        }
        return snapshot.shape.statistics.verifiedFrames != 0 ? 0 : 4;
    }
    return snapshot.accepted != 0 ? 0 : 4;
}

} // namespace pbdecoder
