#include "feedback_policy.h"

#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace pbapp
{
namespace
{

void Increment(std::uint64_t& value) noexcept
{
    if (value != (std::numeric_limits<std::uint64_t>::max)())
    {
        value++;
    }
}

}

FeedbackStatus FeedbackSenderPolicy::Reset(const FeedbackIdentity& identity, const std::uint64_t runToken) noexcept
{
    accepted_.reset();
    telemetry_ = {};
    telemetry_.requested = true;
    telemetry_.state = FeedbackConnectionState::Waiting;
    identity_ = identity;
    runToken_ = runToken;
    retiredEpochs_ = {};
    retiredEpochCount_ = 0;
    firstSubmittedSequence_ = 0;
    nextSubmittedSequence_ = 0;
    lastObservedSequence_ = 0;
    lastFreshObservationAt_ = 0;
    hasSubmitted_ = false;
    hasFreshObservation_ = false;
    conflictLatched_ = false;
    if (runToken == 0 || identity.profileId == 0 || identity.layoutVersion == 0 ||
        identity.segmentCount > maximumFeedbackSegmentCount || identity.fileBytes > maximumFeedbackFileBytes ||
        (identity.fileBytes == 0) != (identity.segmentCount == 0))
    {
        return Reject(FeedbackError::InconsistentState, "Feedback sender binding is invalid", true);
    }
    return {};
}

FeedbackStatus FeedbackSenderPolicy::RecordSubmittedFrame(const std::uint64_t frameSequence) noexcept
{
    if (runToken_ == 0 || frameSequence == (std::numeric_limits<std::uint64_t>::max)())
    {
        return Reject(FeedbackError::LimitExceeded, "Feedback actual submitted range is invalid", true);
    }
    if (!hasSubmitted_)
    {
        firstSubmittedSequence_ = frameSequence;
        nextSubmittedSequence_ = frameSequence + 1;
        hasSubmitted_ = true;
        return {};
    }
    if (frameSequence != nextSubmittedSequence_)
    {
        return Reject(FeedbackError::InconsistentState, "Feedback actual submitted range is not contiguous", true);
    }
    nextSubmittedSequence_++;
    return {};
}

FeedbackStatus FeedbackSenderPolicy::Reject(const FeedbackError error, const char* const message, const bool permanent) noexcept
{
    Increment(telemetry_.rejectedSnapshots);
    telemetry_.effective = false;
    telemetry_.terminalAccepted = false;
    telemetry_.receiverReportedCompletionMilliseconds.reset();
    telemetry_.confirmedSegments = 0;
    telemetry_.lastError = error;
    telemetry_.state = error == FeedbackError::Stale ? FeedbackConnectionState::Stale : FeedbackConnectionState::Rejected;
    conflictLatched_ = conflictLatched_ || permanent;
    return {error, message, 0};
}

bool FeedbackSenderPolicy::IsFresh(const std::uint64_t nowNanoseconds) const noexcept
{
    return hasFreshObservation_ && nowNanoseconds >= lastFreshObservationAt_ &&
        nowNanoseconds - lastFreshObservationAt_ < feedbackFreshnessNanoseconds;
}

void FeedbackSenderPolicy::ApplyAcceptedState(const std::uint64_t nowNanoseconds) noexcept
{
    if (accepted_ && accepted_->phase == FeedbackPhase::Failed)
    {
        telemetry_.terminalAccepted = false;
        telemetry_.receiverReportedCompletionMilliseconds.reset();
        telemetry_.effective = false;
        telemetry_.confirmedSegments = 0;
        telemetry_.state = FeedbackConnectionState::Unavailable;
        telemetry_.lastError = FeedbackError::Unavailable;
        telemetry_.observedFrameAvailable = true;
        telemetry_.observedFrameSequence = accepted_->observedFrameSequence;
        return;
    }
    const bool terminal = accepted_ && accepted_->phase == FeedbackPhase::Completed;
    telemetry_.receiverReportedCompletionMilliseconds = terminal ? std::optional<std::uint64_t>{accepted_->reportedCompletionMilliseconds} : std::nullopt;
    telemetry_.terminalAccepted = terminal;
    telemetry_.effective = terminal || IsFresh(nowNanoseconds);
    telemetry_.lastError = telemetry_.effective ? FeedbackError::None : FeedbackError::Stale;
    telemetry_.confirmedSegments = telemetry_.effective && accepted_ ? GetFeedbackCompletedSegmentCount(*accepted_) : 0;
    telemetry_.state = terminal ? FeedbackConnectionState::ReceiverReportedComplete :
        telemetry_.effective ? FeedbackConnectionState::Active : FeedbackConnectionState::Stale;
    telemetry_.observedFrameAvailable = accepted_.has_value();
    telemetry_.observedFrameSequence = accepted_ ? accepted_->observedFrameSequence : 0;
}

FeedbackStatus FeedbackSenderPolicy::Accept(const FeedbackSnapshot& snapshot, const std::uint64_t nowNanoseconds) noexcept
{
    if (conflictLatched_ || runToken_ == 0)
    {
        return Reject(FeedbackError::Conflict, "Feedback is quarantined until a new sender Start", true);
    }
    const FeedbackStatus valid = ValidateFeedbackSnapshot(snapshot);
    if (!valid)
    {
        return Reject(valid.error, valid.message);
    }
    if (!snapshot.identityBound || snapshot.phase == FeedbackPhase::Waiting)
    {
        const FeedbackStatus status = Reject(FeedbackError::Unavailable, "Receiver feedback is not ready");
        telemetry_.state = FeedbackConnectionState::Waiting;
        return status;
    }
    if (snapshot.identity != identity_)
    {
        return Reject(FeedbackError::InconsistentState, "Feedback transfer identity does not match");
    }
    if (!snapshot.observedFrameAvailable || !hasSubmitted_ || snapshot.observedFrameSequence < firstSubmittedSequence_ ||
        snapshot.observedFrameSequence >= nextSubmittedSequence_)
    {
        return Reject(FeedbackError::Stale, "Feedback visual witness was not submitted in this Start");
    }
    if (hasFreshObservation_ && nowNanoseconds < lastFreshObservationAt_)
    {
        return Reject(FeedbackError::Stale, "Feedback local observation clock regressed");
    }
    bool changeEpoch = false;
    if (accepted_)
    {
        if (snapshot.receiverId != accepted_->receiverId)
        {
            return Reject(FeedbackError::Conflict, "Feedback writer changed during the sender Start", true);
        }
        changeEpoch = snapshot.receiverEpoch != accepted_->receiverEpoch;
        if (changeEpoch)
        {
            for (std::size_t index = 0; index < retiredEpochCount_; index++)
            {
                if (snapshot.receiverEpoch == retiredEpochs_[index])
                {
                    return Reject(FeedbackError::Conflict, "Retired feedback epoch returned", true);
                }
            }
            if (retiredEpochCount_ == retiredEpochs_.size())
            {
                return Reject(FeedbackError::Conflict, "Feedback epoch history limit reached", true);
            }
        }
        else
        {
            if (snapshot.snapshotSequence < accepted_->snapshotSequence)
            {
                return Reject(FeedbackError::Stale, "Feedback snapshot sequence regressed");
            }
            if (snapshot.snapshotSequence == accepted_->snapshotSequence)
            {
                if (snapshot != *accepted_)
                {
                    return Reject(FeedbackError::Conflict, "Conflicting feedback with the same sequence", true);
                }
                Increment(telemetry_.duplicateSnapshots);
                ApplyAcceptedState(nowNanoseconds);
                return {};
            }
            if (snapshot.observedFrameSequence < accepted_->observedFrameSequence)
            {
                return Reject(FeedbackError::Stale, "Feedback visual witness regressed");
            }
            for (std::size_t index = 0; index < snapshot.durableCompleted.size(); index++)
            {
                if ((std::to_integer<unsigned int>(accepted_->durableCompleted[index]) &
                    ~std::to_integer<unsigned int>(snapshot.durableCompleted[index])) != 0)
                {
                    return Reject(FeedbackError::Conflict, "Durable completion regressed within one epoch", true);
                }
            }
            if (accepted_->phase == FeedbackPhase::Completed && snapshot.phase != FeedbackPhase::Completed)
            {
                return Reject(FeedbackError::Conflict, "Final completion regressed within one epoch", true);
            }
        }
    }
    try
    {
        FeedbackSnapshot next = snapshot;
        if (changeEpoch)
        {
            retiredEpochs_[retiredEpochCount_] = accepted_->receiverEpoch;
            retiredEpochCount_++;
        }
        if (!hasFreshObservation_ || snapshot.observedFrameSequence > lastObservedSequence_)
        {
            hasFreshObservation_ = true;
            lastObservedSequence_ = snapshot.observedFrameSequence;
            lastFreshObservationAt_ = nowNanoseconds;
        }
        accepted_ = std::move(next);
        Increment(telemetry_.acceptedSnapshots);
        ApplyAcceptedState(nowNanoseconds);
        return {};
    }
    catch (const std::bad_alloc&)
    {
        return Reject(FeedbackError::AllocationFailure, "Feedback accepted-state allocation failed");
    }
    catch (const std::length_error&)
    {
        return Reject(FeedbackError::AllocationFailure, "Feedback accepted-state length failed");
    }
}

void FeedbackSenderPolicy::MarkUnavailable(const FeedbackError error) noexcept
{
    telemetry_.receiverReportedCompletionMilliseconds.reset();
    telemetry_.effective = false;
    telemetry_.terminalAccepted = false;
    telemetry_.confirmedSegments = 0;
    telemetry_.lastError = error;
    telemetry_.state = FeedbackConnectionState::Unavailable;
}

bool FeedbackSenderPolicy::Expire(const std::uint64_t nowNanoseconds) noexcept
{
    if (!telemetry_.effective || telemetry_.terminalAccepted || IsFresh(nowNanoseconds))
    {
        return false;
    }
    telemetry_.effective = false;
    telemetry_.confirmedSegments = 0;
    telemetry_.state = FeedbackConnectionState::Stale;
    telemetry_.lastError = FeedbackError::Stale;
    Increment(telemetry_.staleEvents);
    return true;
}

bool FeedbackSenderPolicy::IsSegmentConfirmed(const std::uint64_t segmentOrdinal) const noexcept
{
    if (!telemetry_.effective || !accepted_ || segmentOrdinal >= identity_.segmentCount)
    {
        return false;
    }
    const std::size_t byteIndex = static_cast<std::size_t>(segmentOrdinal / 8U);
    const std::uint32_t bitIndex = static_cast<std::uint32_t>(segmentOrdinal % 8U);
    return (std::to_integer<std::uint32_t>(accepted_->durableCompleted[byteIndex]) & (1U << bitIndex)) != 0;
}

bool FeedbackSenderPolicy::CanStop(const std::uint64_t currentRunToken) const noexcept
{
    return currentRunToken != 0 && currentRunToken == runToken_ && telemetry_.terminalAccepted;
}

const FeedbackTelemetry& FeedbackSenderPolicy::GetTelemetry() const noexcept
{
    return telemetry_;
}

std::uint32_t FeedbackSenderPolicy::GetSegmentCount() const noexcept
{
    return identity_.segmentCount;
}

FeedbackSegmentSelection SelectFeedbackSegments(const FeedbackSenderPolicy& policy, std::uint64_t& cursor) noexcept
{
    FeedbackSegmentSelection selection;
    if (!policy.GetTelemetry().effective)
    {
        return selection;
    }
    selection.overrideSelection = true;
    const std::uint64_t segmentCount = policy.GetSegmentCount();
    if (segmentCount == 0)
    {
        cursor = 0;
        selection.keepalive = true;
        return selection;
    }
    cursor %= segmentCount;
    for (std::uint64_t inspected = 0; inspected < segmentCount && selection.count < selection.segmentOrdinals.size(); inspected++)
    {
        const std::uint64_t candidate = cursor;
        cursor = cursor + 1 == segmentCount ? 0 : cursor + 1;
        if (!policy.IsSegmentConfirmed(candidate))
        {
            selection.segmentOrdinals[selection.count] = candidate;
            selection.count++;
        }
    }
    selection.keepalive = selection.count == 0;
    return selection;
}

}
