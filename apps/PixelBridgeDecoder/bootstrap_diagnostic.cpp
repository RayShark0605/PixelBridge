#include "bootstrap_diagnostic.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/checked_integer.h"

#include <algorithm>
#include <limits>

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

void BootstrapDiagnosticProcessor::Reset(std::optional<ScreenCaptureDomain> domain) noexcept
{
    const std::lock_guard lock(mutex_);
    if (pendingReady_)
    {
        pbprotocol::SaturatingIncrementUnsigned(snapshot_.discardedCandidates);
    }
    pendingReady_ = false;
    pending_ = {};
    snapshot_.domain = std::move(domain);
    pbprotocol::SaturatingIncrementUnsigned(snapshot_.resets);
    snapshot_.geometryGeneration = 0;
    snapshot_.calibrationGeneration = 0;
    snapshot_.trackedSessions = 0;
    snapshot_.retainedSequences = 0;
    snapshot_.diagnosticQueueDrops = pbprotocol::SaturatingAddUnsigned(snapshot_.diagnosticQueueDrops, static_cast<std::uint64_t>(eventCount_));
    snapshot_.queuedEvents = 0;
    history_ = {};
    sessions_ = {};
    nextHistory_ = 0;
    eventHead_ = 0;
    eventCount_ = 0;
    geometry_.reset();
    calibrated_ = false;
    blackLevel_ = 0;
    whiteLevel_ = 0;
    lastObservation_ = 0;
}

CaptureStatus BootstrapDiagnosticProcessor::Analyze(const ScreenCaptureFrameMetadata& metadata, const std::span<const std::byte> pixels, const std::size_t rowPitch)
{
    if (pendingReady_)
    {
        return CaptureStatus::Failure(CaptureError::ConsumerFailure, CaptureStage::Consumer);
    }
    pending_ = {};
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
    pending_.visual = pbmodulation::DecodeLocalDesktopBootstrap(view, policy_);
    if (!pending_.visual.IsAccepted())
    {
        pending_.disposition = BootstrapDisposition::VisualErasure;
        return {};
    }
    const auto record = pbprotocol::ParseBootstrapRecord(pending_.visual.canonical44);
    if (!record || record.Value().visualProfileId != pbmodulation::kLocalDesktopVisualProfileId ||
        record.Value().visualLayoutVersion != pbmodulation::kLocalDesktopLayoutVersion)
    {
        // The portable visual decoder already validates this. Fail closed if
        // its future implementation ever violates its accepted-result contract.
        pending_.disposition = BootstrapDisposition::InvalidMetadata;
        return {};
    }
    pending_.bootstrap = record.Value();
    pending_.pixelDigest = pbprotocol::ComputeBlake3Digest(pixels);
    pending_.disposition = BootstrapDisposition::Accepted;
    return {};
}

BootstrapDisposition BootstrapDiagnosticProcessor::AdmitLocked(BootstrapDiagnosticEvent& event) noexcept
{
    const auto tag = event.bootstrap.sessionTag.value;
    const auto sequence = event.bootstrap.frameSequence;
    HistoryEntry* found = nullptr;
    for (auto& entry : history_)
    {
        if (entry.active && entry.sessionTag == tag && entry.sequence == sequence)
        {
            found = &entry;
            break;
        }
    }
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
    if (found == nullptr && session != nullptr && sequence <= session->highestSequence)
    {
        // An evicted sequence is not magically a new independent observation.
        // Retain a bounded per-session high-water instead of accepting replays.
        return BootstrapDisposition::StaleSequence;
    }
    const bool geometryChanged = !geometry_ || !SameTransform(*geometry_, event.visual.geometry);
    const bool calibrationChanged = !calibrated_ || blackLevel_ != event.visual.blackLevel || whiteLevel_ != event.visual.whiteLevel;
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
        *session = {true, tag, sequence};
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
        return;
    }
    lastObservation_ = metadata.captureObservation;
    pending_.geometryGeneration = snapshot_.geometryGeneration;
    pending_.calibrationGeneration = snapshot_.calibrationGeneration;
    if (pending_.disposition == BootstrapDisposition::Accepted)
    {
        pending_.disposition = AdmitLocked(pending_);
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
}

void BootstrapDiagnosticProcessor::Discard() noexcept
{
    const std::lock_guard lock(mutex_);
    if (pendingReady_)
    {
        pbprotocol::SaturatingIncrementUnsigned(snapshot_.discardedCandidates);
    }
    pendingReady_ = false;
}

BootstrapDiagnosticSnapshot BootstrapDiagnosticProcessor::GetSnapshot() const noexcept
{
    const std::lock_guard lock(mutex_);
    return snapshot_;
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
    }
    return "Unknown";
}

bool IsTerminalDiagnosticFailure(const CaptureSnapshot& capture, const DiagnosticReadbackSnapshot& readback) noexcept
{
    return capture.state == CaptureState::Failed || capture.state == CaptureState::Stopped || !readback.error;
}

int GetBootstrapDiagnosticSuccessExitCode(const BootstrapDiagnosticSnapshot& snapshot) noexcept
{
    return snapshot.accepted != 0 ? 0 : 4;
}

} // namespace pbdecoder
