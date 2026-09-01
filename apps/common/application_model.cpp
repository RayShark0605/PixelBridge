#include "application_model.h"

#include <algorithm>
#include <cmath>

namespace pbapp
{
namespace
{

[[nodiscard]] bool CanBeginEncoder(const EncoderState state) noexcept
{
    return state == EncoderState::Idle || state == EncoderState::Stopped || state == EncoderState::Failed;
}

[[nodiscard]] bool CanBeginDecoder(const DecoderState state) noexcept
{
    return state == DecoderState::Idle || state == DecoderState::Stopped || state == DecoderState::Completed ||
        state == DecoderState::Failed;
}

[[nodiscard]] bool IsDecoderActive(const DecoderState state) noexcept
{
    return state == DecoderState::WaitingForBootstrap || state == DecoderState::ReceivingControl ||
        state == DecoderState::Receiving || state == DecoderState::Recovering || state == DecoderState::Verifying ||
        state == DecoderState::Publishing;
}

[[nodiscard]] bool IsAllowedDecoderAdvance(const DecoderState currentState,
    const DecoderState nextState) noexcept
{
    switch (currentState)
    {
    case DecoderState::WaitingForBootstrap:
        return nextState == DecoderState::ReceivingControl;
    case DecoderState::ReceivingControl:
        return nextState == DecoderState::Receiving;
    case DecoderState::Receiving:
        return nextState == DecoderState::Recovering || nextState == DecoderState::Verifying;
    case DecoderState::Recovering:
        return nextState == DecoderState::Verifying;
    case DecoderState::Verifying:
        return nextState == DecoderState::Publishing;
    case DecoderState::Publishing:
        return nextState == DecoderState::Completed;
    default:
        return false;
    }
}

} // namespace

TransitionResult EncoderStateMachine::Start(const std::uint64_t runGeneration) noexcept
{
    if (runGeneration == 0 || !CanBeginEncoder(state_))
    {
        return TransitionResult::Rejected;
    }
    runGeneration_ = runGeneration;
    state_ = EncoderState::Preparing;
    return TransitionResult::Applied;
}

TransitionResult EncoderStateMachine::MarkBroadcasting(const std::uint64_t runGeneration) noexcept
{
    if (!IsCurrent(runGeneration))
    {
        return TransitionResult::Stale;
    }
    if (state_ == EncoderState::Broadcasting)
    {
        return TransitionResult::NoChange;
    }
    if (state_ != EncoderState::Preparing)
    {
        return TransitionResult::Rejected;
    }
    state_ = EncoderState::Broadcasting;
    return TransitionResult::Applied;
}

TransitionResult EncoderStateMachine::RequestStop(const std::uint64_t runGeneration) noexcept
{
    if (!IsCurrent(runGeneration))
    {
        return TransitionResult::Stale;
    }
    if (state_ == EncoderState::Stopping || state_ == EncoderState::Stopped)
    {
        return TransitionResult::NoChange;
    }
    if (state_ != EncoderState::Preparing && state_ != EncoderState::Broadcasting)
    {
        return TransitionResult::Rejected;
    }
    state_ = EncoderState::Stopping;
    return TransitionResult::Applied;
}

TransitionResult EncoderStateMachine::MarkStopped(const std::uint64_t runGeneration) noexcept
{
    if (!IsCurrent(runGeneration))
    {
        return TransitionResult::Stale;
    }
    if (state_ == EncoderState::Stopped)
    {
        return TransitionResult::NoChange;
    }
    if (state_ != EncoderState::Stopping)
    {
        return TransitionResult::Rejected;
    }
    state_ = EncoderState::Stopped;
    return TransitionResult::Applied;
}

TransitionResult EncoderStateMachine::Fail(const std::uint64_t runGeneration) noexcept
{
    if (!IsCurrent(runGeneration))
    {
        return TransitionResult::Stale;
    }
    if (state_ == EncoderState::Failed)
    {
        return TransitionResult::NoChange;
    }
    state_ = EncoderState::Failed;
    return TransitionResult::Applied;
}

EncoderState EncoderStateMachine::GetState() const noexcept
{
    return state_;
}

std::uint64_t EncoderStateMachine::GetRunGeneration() const noexcept
{
    return runGeneration_;
}

bool EncoderStateMachine::IsCurrent(const std::uint64_t runGeneration) const noexcept
{
    return runGeneration != 0 && runGeneration == runGeneration_;
}

TransitionResult DecoderStateMachine::Start(const std::uint64_t runGeneration) noexcept
{
    if (runGeneration == 0 || !CanBeginDecoder(state_))
    {
        return TransitionResult::Rejected;
    }
    runGeneration_ = runGeneration;
    state_ = DecoderState::WaitingForBootstrap;
    return TransitionResult::Applied;
}

TransitionResult DecoderStateMachine::Advance(const std::uint64_t runGeneration,
    const DecoderState nextState) noexcept
{
    if (!IsCurrent(runGeneration))
    {
        return TransitionResult::Stale;
    }
    if (state_ == nextState)
    {
        return TransitionResult::NoChange;
    }
    if (!IsAllowedDecoderAdvance(state_, nextState))
    {
        return TransitionResult::Rejected;
    }
    state_ = nextState;
    return TransitionResult::Applied;
}

TransitionResult DecoderStateMachine::RequestStop(const std::uint64_t runGeneration) noexcept
{
    if (!IsCurrent(runGeneration))
    {
        return TransitionResult::Stale;
    }
    if (state_ == DecoderState::Stopping || state_ == DecoderState::Stopped)
    {
        return TransitionResult::NoChange;
    }
    if (!IsDecoderActive(state_))
    {
        return TransitionResult::Rejected;
    }
    state_ = DecoderState::Stopping;
    return TransitionResult::Applied;
}

TransitionResult DecoderStateMachine::MarkStopped(const std::uint64_t runGeneration) noexcept
{
    if (!IsCurrent(runGeneration))
    {
        return TransitionResult::Stale;
    }
    if (state_ == DecoderState::Stopped)
    {
        return TransitionResult::NoChange;
    }
    if (state_ != DecoderState::Stopping)
    {
        return TransitionResult::Rejected;
    }
    state_ = DecoderState::Stopped;
    return TransitionResult::Applied;
}

TransitionResult DecoderStateMachine::Fail(const std::uint64_t runGeneration) noexcept
{
    if (!IsCurrent(runGeneration))
    {
        return TransitionResult::Stale;
    }
    if (state_ == DecoderState::Failed)
    {
        return TransitionResult::NoChange;
    }
    state_ = DecoderState::Failed;
    return TransitionResult::Applied;
}

DecoderState DecoderStateMachine::GetState() const noexcept
{
    return state_;
}

std::uint64_t DecoderStateMachine::GetRunGeneration() const noexcept
{
    return runGeneration_;
}

bool DecoderStateMachine::IsCurrent(const std::uint64_t runGeneration) const noexcept
{
    return runGeneration != 0 && runGeneration == runGeneration_;
}

bool CarouselCounter::Reset(const std::uint32_t cycleFrameCount) noexcept
{
    if (cycleFrameCount == 0)
    {
        return false;
    }
    snapshot_ = {0, 0, cycleFrameCount};
    return true;
}

bool CarouselCounter::Advance() noexcept
{
    if (snapshot_.cycleFrameCount == 0 || snapshot_.cycleCount == (std::numeric_limits<std::uint64_t>::max)())
    {
        return false;
    }
    snapshot_.cyclePosition++;
    if (snapshot_.cyclePosition == snapshot_.cycleFrameCount)
    {
        snapshot_.cyclePosition = 0;
        snapshot_.cycleCount++;
    }
    return true;
}

CarouselSnapshot CarouselCounter::GetSnapshot() const noexcept
{
    return snapshot_;
}

VisualIdentityDisposition VisualIdentityTracker::Observe(const std::uint64_t sequence, const std::uint64_t captureEpoch,
    const std::int64_t timestamp100ns, const std::optional<std::uint64_t> streamIdentity) noexcept
{
    if (captureEpoch == 0 || timestamp100ns < 0)
    {
        return VisualIdentityDisposition::Invalid;
    }
    if (!hasBaseline_ || captureEpoch != lastCaptureEpoch_ || streamIdentity != lastStreamIdentity_)
    {
        hasBaseline_ = true;
        maximumSequence_ = sequence;
        lastCaptureEpoch_ = captureEpoch;
        lastStreamIdentity_ = streamIdentity;
        lastTimestamp100ns_ = timestamp100ns;
        if (uniqueFrames_ != (std::numeric_limits<std::uint64_t>::max)())
        {
            uniqueFrames_++;
        }
        return VisualIdentityDisposition::Unique;
    }
    if (sequence == maximumSequence_)
    {
        if (duplicateFrames_ != (std::numeric_limits<std::uint64_t>::max)())
        {
            duplicateFrames_++;
        }
        return VisualIdentityDisposition::Duplicate;
    }
    if (sequence < maximumSequence_)
    {
        if (reorderedFrames_ != (std::numeric_limits<std::uint64_t>::max)())
        {
            reorderedFrames_++;
        }
        return VisualIdentityDisposition::Reordered;
    }

    const std::uint64_t distance = sequence - maximumSequence_;
    if (distance == 1 && timestamp100ns > lastTimestamp100ns_)
    {
        if (intervalCount_ != (std::numeric_limits<std::uint64_t>::max)())
        {
            intervalCount_++;
        }
        const std::uint64_t interval = static_cast<std::uint64_t>(timestamp100ns - lastTimestamp100ns_);
        intervalTime100ns_ = interval > (std::numeric_limits<std::uint64_t>::max)() - intervalTime100ns_ ?
            (std::numeric_limits<std::uint64_t>::max)() : intervalTime100ns_ + interval;
    }
    else if (distance > 1)
    {
        if (gapEvents_ != (std::numeric_limits<std::uint64_t>::max)())
        {
            gapEvents_++;
        }
        const std::uint64_t skipped = distance - 1;
        skippedSequences_ = skipped > (std::numeric_limits<std::uint64_t>::max)() - skippedSequences_ ?
            (std::numeric_limits<std::uint64_t>::max)() : skippedSequences_ + skipped;
    }
    maximumSequence_ = sequence;
    lastTimestamp100ns_ = timestamp100ns;
    if (uniqueFrames_ != (std::numeric_limits<std::uint64_t>::max)())
    {
        uniqueFrames_++;
    }
    return VisualIdentityDisposition::Unique;
}

VisualIdentitySnapshot VisualIdentityTracker::GetSnapshot() const noexcept
{
    VisualIdentitySnapshot snapshot;
    snapshot.uniqueFrames = uniqueFrames_;
    snapshot.duplicateFrames = duplicateFrames_;
    snapshot.reorderedFrames = reorderedFrames_;
    snapshot.gapEvents = gapEvents_;
    snapshot.skippedSequences = skippedSequences_;
    if (intervalCount_ != 0 && intervalTime100ns_ != 0)
    {
        snapshot.framesPerSecond = static_cast<double>(intervalCount_) * 10000000.0 /
            static_cast<double>(intervalTime100ns_);
    }
    return snapshot;
}

void ChannelStallTracker::StartInterval(StallIntervalSnapshot& interval, const std::uint64_t startedMilliseconds,
    std::uint64_t& storedStartedMilliseconds) noexcept
{
    if (interval.active)
    {
        return;
    }
    interval.active = true;
    interval.currentMilliseconds = 0;
    if (interval.count != (std::numeric_limits<std::uint64_t>::max)())
    {
        interval.count++;
    }
    storedStartedMilliseconds = startedMilliseconds;
}

void ChannelStallTracker::EndInterval(StallIntervalSnapshot& interval, const std::uint64_t endedMilliseconds,
    std::uint64_t& startedMilliseconds) noexcept
{
    if (!interval.active)
    {
        return;
    }
    const std::uint64_t duration = endedMilliseconds >= startedMilliseconds ? endedMilliseconds - startedMilliseconds : 0;
    interval.totalMilliseconds = duration > (std::numeric_limits<std::uint64_t>::max)() - interval.totalMilliseconds ?
        (std::numeric_limits<std::uint64_t>::max)() : interval.totalMilliseconds + duration;
    interval.maximumMilliseconds = std::max(interval.maximumMilliseconds, duration);
    interval.currentMilliseconds = 0;
    interval.active = false;
    startedMilliseconds = endedMilliseconds;
}

void ChannelStallTracker::Observe(const std::uint64_t monotonicMilliseconds,
    const std::uint64_t captureObservations, const std::uint64_t legalVisualObservations) noexcept
{
    if (!initialized_)
    {
        ResetDomain(monotonicMilliseconds, captureObservations, legalVisualObservations);
        return;
    }
    if (monotonicMilliseconds < lastObservationMilliseconds_)
    {
        return;
    }
    constexpr std::uint64_t stallThresholdMilliseconds = 1000;
    const bool captureChanged = captureObservations != lastCaptureObservations_;
    const bool visualChanged = legalVisualObservations != lastLegalVisualObservations_;
    if (captureChanged)
    {
        EndInterval(snapshot_.capture, monotonicMilliseconds, captureStallStartedMilliseconds_);
        lastCaptureChangeMilliseconds_ = monotonicMilliseconds;
        lastCaptureObservations_ = captureObservations;
    }
    else if (!snapshot_.capture.active && monotonicMilliseconds - lastCaptureChangeMilliseconds_ >= stallThresholdMilliseconds)
    {
        StartInterval(snapshot_.capture, lastCaptureChangeMilliseconds_, captureStallStartedMilliseconds_);
    }

    if (visualChanged)
    {
        EndInterval(snapshot_.visual, monotonicMilliseconds, visualStallStartedMilliseconds_);
        lastVisualChangeMilliseconds_ = monotonicMilliseconds;
        lastLegalVisualObservations_ = legalVisualObservations;
    }
    else if (captureChanged && !snapshot_.visual.active &&
        monotonicMilliseconds - lastVisualChangeMilliseconds_ >= stallThresholdMilliseconds)
    {
        StartInterval(snapshot_.visual, lastVisualChangeMilliseconds_, visualStallStartedMilliseconds_);
    }
    else if (snapshot_.capture.active)
    {
        // Once capture loss reaches the one-second classification threshold,
        // the last capture observation is the authoritative end of a visual
        // stall. Do not charge the capture-loss detection window to both
        // categories.
        EndInterval(snapshot_.visual, lastCaptureChangeMilliseconds_, visualStallStartedMilliseconds_);
    }

    if (snapshot_.capture.active)
    {
        snapshot_.capture.currentMilliseconds = monotonicMilliseconds - captureStallStartedMilliseconds_;
    }
    if (snapshot_.visual.active)
    {
        snapshot_.visual.currentMilliseconds = monotonicMilliseconds - visualStallStartedMilliseconds_;
    }
    lastObservationMilliseconds_ = monotonicMilliseconds;
}

void ChannelStallTracker::ResetDomain(const std::uint64_t monotonicMilliseconds,
    const std::uint64_t captureObservations, const std::uint64_t legalVisualObservations) noexcept
{
    if (initialized_ && monotonicMilliseconds >= lastObservationMilliseconds_)
    {
        EndInterval(snapshot_.capture, monotonicMilliseconds, captureStallStartedMilliseconds_);
        EndInterval(snapshot_.visual, monotonicMilliseconds, visualStallStartedMilliseconds_);
    }
    lastObservationMilliseconds_ = monotonicMilliseconds;
    lastCaptureChangeMilliseconds_ = monotonicMilliseconds;
    lastVisualChangeMilliseconds_ = monotonicMilliseconds;
    lastCaptureObservations_ = captureObservations;
    lastLegalVisualObservations_ = legalVisualObservations;
    initialized_ = true;
}

void ChannelStallTracker::Finish(const std::uint64_t monotonicMilliseconds) noexcept
{
    if (!initialized_ || monotonicMilliseconds < lastObservationMilliseconds_)
    {
        return;
    }
    EndInterval(snapshot_.capture, monotonicMilliseconds, captureStallStartedMilliseconds_);
    EndInterval(snapshot_.visual, monotonicMilliseconds, visualStallStartedMilliseconds_);
    lastObservationMilliseconds_ = monotonicMilliseconds;
}

ChannelStallSnapshot ChannelStallTracker::GetSnapshot() const noexcept
{
    return snapshot_;
}

void RemoteDuplicateRefinementGate::StartSequence(const std::uint64_t captureEpoch,
    const std::uint64_t frameSequence) noexcept
{
    if (captureEpoch == 0)
    {
        return;
    }
    currentCaptureEpoch_ = captureEpoch;
    currentFrameSequence_ = frameSequence;
    snapshot_.currentSequenceAdmitted = false;
    hasCurrentSequence_ = true;
}

bool RemoteDuplicateRefinementGate::ShouldAttemptDuplicate(const std::uint64_t captureEpoch,
    const std::uint64_t frameSequence, const bool hasAcceptedCarrier) noexcept
{
    if (!hasCurrentSequence_ || captureEpoch != currentCaptureEpoch_ || frameSequence != currentFrameSequence_ ||
        snapshot_.currentSequenceAdmitted)
    {
        return false;
    }
    if (snapshot_.attempts != (std::numeric_limits<std::uint64_t>::max)())
    {
        snapshot_.attempts++;
    }
    return hasAcceptedCarrier;
}

bool RemoteDuplicateRefinementGate::MarkAdmission(const std::uint64_t captureEpoch,
    const std::uint64_t frameSequence, const bool duplicateRefinement) noexcept
{
    if (!hasCurrentSequence_ || captureEpoch != currentCaptureEpoch_ || frameSequence != currentFrameSequence_ ||
        snapshot_.currentSequenceAdmitted)
    {
        return false;
    }
    snapshot_.currentSequenceAdmitted = true;
    if (duplicateRefinement && snapshot_.recoveries != (std::numeric_limits<std::uint64_t>::max)())
    {
        snapshot_.recoveries++;
    }
    return true;
}

void RemoteDuplicateRefinementGate::ResetEpoch() noexcept
{
    currentCaptureEpoch_ = 0;
    currentFrameSequence_ = 0;
    snapshot_.currentSequenceAdmitted = false;
    hasCurrentSequence_ = false;
}

RemoteDuplicateRefinementSnapshot RemoteDuplicateRefinementGate::GetSnapshot() const noexcept
{
    return snapshot_;
}

bool DecoderProgressTracker::BindDescriptor(const std::uint64_t totalRawBytes,
    const std::uint64_t monotonicMilliseconds) noexcept
{
    if (totalRawBytes == 0 || snapshot_.descriptorKnown)
    {
        return false;
    }
    snapshot_ = {};
    snapshot_.descriptorKnown = true;
    snapshot_.totalRawBytes = totalRawBytes;
    snapshot_.remainingRawBytes = totalRawBytes;
    snapshot_.progress = 0.0;
    startMilliseconds_ = monotonicMilliseconds;
    lastSampleMilliseconds_ = monotonicMilliseconds;
    lastPositiveSampleMilliseconds_ = monotonicMilliseconds;
    lastVerifiedRawBytes_ = 0;
    return true;
}

bool DecoderProgressTracker::ObserveVerifiedRawBytes(const std::uint64_t verifiedRawBytes,
    const std::uint64_t monotonicMilliseconds) noexcept
{
    if (!snapshot_.descriptorKnown || verifiedRawBytes < snapshot_.verifiedRawBytes ||
        verifiedRawBytes > snapshot_.totalRawBytes || monotonicMilliseconds < lastSampleMilliseconds_)
    {
        return false;
    }
    const std::uint64_t elapsedMilliseconds = monotonicMilliseconds - lastSampleMilliseconds_;
    const std::uint64_t verifiedDelta = verifiedRawBytes - lastVerifiedRawBytes_;
    if (verifiedDelta > 0 && elapsedMilliseconds > 0)
    {
        const double instant = static_cast<double>(verifiedDelta) * 1000.0 /
            static_cast<double>(elapsedMilliseconds);
        snapshot_.instantBytesPerSecond = instant;
        snapshot_.smoothedBytesPerSecond = snapshot_.positiveSampleCount == 0 ? instant :
            0.25 * instant + 0.75 * snapshot_.smoothedBytesPerSecond;
        if (snapshot_.positiveSampleCount != (std::numeric_limits<std::uint32_t>::max)())
        {
            snapshot_.positiveSampleCount++;
        }
        lastPositiveSampleMilliseconds_ = monotonicMilliseconds;
    }
    snapshot_.verifiedRawBytes = verifiedRawBytes;
    snapshot_.remainingRawBytes = snapshot_.totalRawBytes - verifiedRawBytes;
    snapshot_.progress = static_cast<double>(verifiedRawBytes) / static_cast<double>(snapshot_.totalRawBytes);
    const std::uint64_t totalElapsedMilliseconds = monotonicMilliseconds - startMilliseconds_;
    snapshot_.averageBytesPerSecond = verifiedRawBytes == 0 || totalElapsedMilliseconds == 0 ? 0 :
        static_cast<double>(verifiedRawBytes) * 1000.0 / static_cast<double>(totalElapsedMilliseconds);
    lastSampleMilliseconds_ = monotonicMilliseconds;
    lastVerifiedRawBytes_ = verifiedRawBytes;
    RecalculateEta();
    return true;
}

void DecoderProgressTracker::ResetForCaptureEpoch(const std::uint64_t monotonicMilliseconds) noexcept
{
    snapshot_ = {};
    startMilliseconds_ = monotonicMilliseconds;
    lastSampleMilliseconds_ = monotonicMilliseconds;
    lastPositiveSampleMilliseconds_ = monotonicMilliseconds;
    lastVerifiedRawBytes_ = 0;
}

void DecoderProgressTracker::ObserveStall(const std::uint64_t monotonicMilliseconds) noexcept
{
    if (!snapshot_.descriptorKnown || monotonicMilliseconds < lastPositiveSampleMilliseconds_)
    {
        return;
    }
    if (monotonicMilliseconds - lastPositiveSampleMilliseconds_ >= 5000)
    {
        snapshot_.instantBytesPerSecond = 0;
        snapshot_.smoothedBytesPerSecond = 0;
        snapshot_.etaMilliseconds.reset();
    }
}

ProgressSnapshot DecoderProgressTracker::GetSnapshot() const noexcept
{
    return snapshot_;
}

void DecoderProgressTracker::RecalculateEta() noexcept
{
    if (!snapshot_.descriptorKnown || snapshot_.remainingRawBytes == 0)
    {
        snapshot_.etaMilliseconds = snapshot_.descriptorKnown ? std::optional<std::uint64_t>(0) : std::nullopt;
        return;
    }
    if (snapshot_.positiveSampleCount < 2 || !(snapshot_.smoothedBytesPerSecond > 0) ||
        !std::isfinite(snapshot_.smoothedBytesPerSecond))
    {
        snapshot_.etaMilliseconds.reset();
        return;
    }
    const long double eta = static_cast<long double>(snapshot_.remainingRawBytes) * 1000.0L /
        static_cast<long double>(snapshot_.smoothedBytesPerSecond);
    if (!(eta >= 0) || eta > static_cast<long double>((std::numeric_limits<std::uint64_t>::max)()))
    {
        snapshot_.etaMilliseconds.reset();
        return;
    }
    snapshot_.etaMilliseconds = static_cast<std::uint64_t>(eta);
}

const char* GetEncoderStateName(const EncoderState state) noexcept
{
    switch (state)
    {
    case EncoderState::Idle: return "Idle";
    case EncoderState::Preparing: return "Preparing";
    case EncoderState::Broadcasting: return "Broadcasting";
    case EncoderState::Stopping: return "Stopping";
    case EncoderState::Stopped: return "Stopped";
    case EncoderState::Failed: return "Failed";
    }
    return "Unknown";
}

const char* GetDecoderStateName(const DecoderState state) noexcept
{
    switch (state)
    {
    case DecoderState::Idle: return "Idle";
    case DecoderState::WaitingForBootstrap: return "WaitingForBootstrap";
    case DecoderState::ReceivingControl: return "ReceivingControl";
    case DecoderState::Receiving: return "Receiving";
    case DecoderState::Recovering: return "Recovering";
    case DecoderState::Verifying: return "Verifying";
    case DecoderState::Publishing: return "Publishing";
    case DecoderState::Completed: return "Completed";
    case DecoderState::Failed: return "Failed";
    case DecoderState::Stopping: return "Stopping";
    case DecoderState::Stopped: return "Stopped";
    }
    return "Unknown";
}

const char* GetVisualProfileName(const VisualProfile profile) noexcept
{
    switch (profile)
    {
    case VisualProfile::DirectLevels2x2: return "Direct-Level 2x2 (Experimental)";
    case VisualProfile::ShapeChroma: return "Shape+Chroma (Experimental)";
    case VisualProfile::RemoteVisualResilient: return "RemoteVisual Resilient 8x8 Luma (Experimental)";
    case VisualProfile::RemoteVisualLowFps: return "PB-RemoteVisual-LF4-X1 (Hidden Encoder Candidate)";
    }
    return "Unknown";
}

const char* GetCaptureBackendName(const CaptureBackend backend) noexcept
{
    switch (backend)
    {
    case CaptureBackend::Wgc: return "WGC";
    case CaptureBackend::Dxgi: return "DXGI Desktop Duplication";
    }
    return "Unknown";
}

const char* GetCompressionCodecName(const pbprotocol::CompressionCodec codec) noexcept
{
    switch (codec)
    {
    case pbprotocol::CompressionCodec::Raw: return "RAW";
    case pbprotocol::CompressionCodec::Zstandard: return "zstd";
    }
    return "Unknown";
}

const char* GetOuterFecModeName(const pbprotocol::OuterFecMode mode) noexcept
{
    switch (mode)
    {
    case pbprotocol::OuterFecMode::DirectRepeat: return "DirectRepeat";
    case pbprotocol::OuterFecMode::WirehairV2: return "Wirehair V2";
    }
    return "Unknown";
}

const char* GetMetadataProvenanceName(const MetadataProvenance provenance) noexcept
{
    switch (provenance)
    {
    case MetadataProvenance::NotProvided: return "NotProvided";
    case MetadataProvenance::Manual: return "Manual";
    case MetadataProvenance::PixelBridgeObserved: return "PixelBridgeObserved";
    case MetadataProvenance::RemoteUiVisible: return "RemoteUiVisible";
    }
    return "Unknown";
}

RuntimeCapabilities GetRuntimeCapabilities() noexcept
{
    return {};
}

bool IsEncoderStateActive(const EncoderState state) noexcept
{
    return state == EncoderState::Preparing || state == EncoderState::Broadcasting ||
        state == EncoderState::Stopping;
}

bool IsDecoderStateActive(const DecoderState state) noexcept
{
    return IsDecoderActive(state) || state == DecoderState::Stopping;
}

WindowCloseAction GetWindowCloseAction(const bool active, const bool closeAlreadyPending) noexcept
{
    if (!active)
    {
        return WindowCloseAction::Accept;
    }
    return closeAlreadyPending ? WindowCloseAction::Defer : WindowCloseAction::RequestStopAndDefer;
}

} // namespace pbapp
