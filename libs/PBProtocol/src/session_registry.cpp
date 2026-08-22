#include "pbprotocol/session_registry.h"

#include "pbprotocol/checked_integer.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>

namespace pbprotocol {

namespace {

constexpr std::size_t kSessionIdOffset = 4;
constexpr std::size_t kSegmentSessionTagOffset = 0;

template <typename ValueType>
[[nodiscard]] ProtocolResult<ValueType> FailureFrom(
    const ProtocolError& error)
{
    return ProtocolResult<ValueType>::Failure(error.code, error.offset);
}

} // namespace

ProtocolResult<SessionRegistry> SessionRegistry::Create(
    ReceiverResourcePolicy resourcePolicy)
{
    return CreateWithSessionTagDeriverForTesting(
        std::move(resourcePolicy),
        &DeriveSessionTag);
}

ProtocolResult<SessionRegistry>
SessionRegistry::CreateWithSessionTagDeriverForTesting(
    ReceiverResourcePolicy resourcePolicy,
    const SessionTagDeriver sessionTagDeriver)
{
    const ProtocolStatus policyStatus = ValidateReceiverResourcePolicy(
        resourcePolicy);
    if (!policyStatus)
    {
        return FailureFrom<SessionRegistry>(policyStatus.Error());
    }
    if (sessionTagDeriver == nullptr)
    {
        return ProtocolResult<SessionRegistry>::Failure(
            ProtocolErrorCode::InvalidResourcePolicy,
            0);
    }

    return ProtocolResult<SessionRegistry>::Success(
        SessionRegistry(std::move(resourcePolicy), sessionTagDeriver));
}

SessionRegistry::SessionRegistry(
    ReceiverResourcePolicy resourcePolicy,
    const SessionTagDeriver sessionTagDeriver) noexcept
    : resourcePolicy_(std::move(resourcePolicy)),
      sessionTagDeriver_(sessionTagDeriver)
{
}

ProtocolResult<DescriptorBindDisposition>
SessionRegistry::BindSessionDescriptor(const SessionDescriptor& descriptor)
{
    const ProtocolStatus validationStatus = ValidateSessionDescriptor(
        descriptor,
        resourcePolicy_);
    if (!validationStatus)
    {
        return FailureFrom<DescriptorBindDisposition>(validationStatus.Error());
    }
    if (!HasConsistentRegistryState())
    {
        return ProtocolResult<DescriptorBindDisposition>::Failure(
            ProtocolErrorCode::InternalDescriptorStateError,
            kSessionIdOffset);
    }

    const SessionTag sessionTag = sessionTagDeriver_(descriptor.sessionId);
    auto tagBinding = bindingsByTag_.find(sessionTag.value);
    if (tagBinding != bindingsByTag_.end())
    {
        if (tagBinding->second.ambiguous)
        {
            return ProtocolResult<DescriptorBindDisposition>::Failure(
                ProtocolErrorCode::SessionTagCollision,
                kSessionIdOffset);
        }

        if (tagBinding->second.sessionId == descriptor.sessionId)
        {
            const auto sessionEntry = sessionsById_.find(
                descriptor.sessionId.bytes);
            if (sessionEntry == sessionsById_.end() ||
                !sessionEntry->second.bindingState)
            {
                return ProtocolResult<DescriptorBindDisposition>::Failure(
                    ProtocolErrorCode::InternalDescriptorStateError,
                    kSessionIdOffset);
            }
            return sessionEntry->second.bindingState->BindSessionDescriptor(
                descriptor);
        }

        const ProtocolStatus removalStatus = RemoveUniqueSessionForCollision(
            sessionTag,
            tagBinding->second);
        if (!removalStatus)
        {
            return FailureFrom<DescriptorBindDisposition>(removalStatus.Error());
        }
        tagBinding->second.sessionId = SessionId{};
        tagBinding->second.ambiguous = true;
        return ProtocolResult<DescriptorBindDisposition>::Failure(
            ProtocolErrorCode::SessionTagCollision,
            kSessionIdOffset);
    }

    if (sessionsById_.find(descriptor.sessionId.bytes) != sessionsById_.end())
    {
        return ProtocolResult<DescriptorBindDisposition>::Failure(
            ProtocolErrorCode::InternalDescriptorStateError,
            kSessionIdOffset);
    }
    if (static_cast<std::uint64_t>(sessionsById_.size()) >=
        resourcePolicy_.maxConcurrentSessions)
    {
        return ProtocolResult<DescriptorBindDisposition>::Failure(
            ProtocolErrorCode::ResourceLimitExceeded,
            kSessionIdOffset);
    }

    const auto reservationResult = CheckedAddUint64(
        reservedDescriptorStateBytes_,
        resourcePolicy_.maxDescriptorStateBytes,
        kSessionIdOffset);
    if (!reservationResult ||
        reservationResult.Value() >
            resourcePolicy_.maxTotalDescriptorStateBytes)
    {
        return ProtocolResult<DescriptorBindDisposition>::Failure(
            ProtocolErrorCode::ResourceLimitExceeded,
            kSessionIdOffset);
    }

    auto stateResult = DescriptorBindingState::Create(
        descriptor,
        resourcePolicy_);
    if (!stateResult)
    {
        return FailureFrom<DescriptorBindDisposition>(stateResult.Error());
    }

    try
    {
        auto bindingState = std::make_unique<DescriptorBindingState>(
            std::move(stateResult).Value());
        const auto sessionInsertion = sessionsById_.emplace(
            descriptor.sessionId.bytes,
            SessionEntry{sessionTag, std::move(bindingState)});
        if (!sessionInsertion.second)
        {
            return ProtocolResult<DescriptorBindDisposition>::Failure(
                ProtocolErrorCode::InternalDescriptorStateError,
                kSessionIdOffset);
        }

        try
        {
            const auto tagInsertion = bindingsByTag_.emplace(
                sessionTag.value,
                TagBinding{descriptor.sessionId, false});
            if (!tagInsertion.second)
            {
                sessionsById_.erase(sessionInsertion.first);
                return ProtocolResult<DescriptorBindDisposition>::Failure(
                    ProtocolErrorCode::InternalDescriptorStateError,
                    kSessionIdOffset);
            }
        }
        catch (...)
        {
            sessionsById_.erase(sessionInsertion.first);
            throw;
        }
    }
    catch (const std::bad_alloc&)
    {
        return ProtocolResult<DescriptorBindDisposition>::Failure(
            ProtocolErrorCode::ResourceExhausted,
            kSessionIdOffset);
    }
    catch (const std::length_error&)
    {
        return ProtocolResult<DescriptorBindDisposition>::Failure(
            ProtocolErrorCode::ResourceExhausted,
            kSessionIdOffset);
    }

    reservedDescriptorStateBytes_ = reservationResult.Value();
    return ProtocolResult<DescriptorBindDisposition>::Success(
        DescriptorBindDisposition::Inserted);
}

ProtocolResult<DescriptorBindDisposition>
SessionRegistry::BindSegmentDescriptor(const SegmentDescriptor& descriptor)
{
    auto bindingStateResult = FindBindingState(descriptor.sessionTag);
    if (!bindingStateResult)
    {
        return FailureFrom<DescriptorBindDisposition>(
            bindingStateResult.Error());
    }
    return bindingStateResult.Value()->BindSegmentDescriptor(descriptor);
}

ProtocolResult<DescriptorBindDisposition> SessionRegistry::BindFinalManifest(
    const FinalManifest& finalManifest)
{
    const SessionTag sessionTag = sessionTagDeriver_(finalManifest.sessionId);
    auto bindingStateResult = FindBindingState(sessionTag);
    if (!bindingStateResult)
    {
        return FailureFrom<DescriptorBindDisposition>(
            bindingStateResult.Error());
    }
    return bindingStateResult.Value()->BindFinalManifest(finalManifest);
}

ProtocolStatus SessionRegistry::ValidateCompleteSegmentMap(
    const SessionTag sessionTag)
{
    auto bindingStateResult = FindBindingState(sessionTag);
    if (!bindingStateResult)
    {
        return ProtocolStatus::Failure(
            bindingStateResult.Error().code,
            bindingStateResult.Error().offset);
    }
    return bindingStateResult.Value()->ValidateCompleteSegmentMap();
}

ProtocolStatus SessionRegistry::ValidateReadyForFinalVerification(
    const SessionTag sessionTag)
{
    auto bindingStateResult = FindBindingState(sessionTag);
    if (!bindingStateResult)
    {
        return ProtocolStatus::Failure(
            bindingStateResult.Error().code,
            bindingStateResult.Error().offset);
    }
    return bindingStateResult.Value()->ValidateReadyForFinalVerification();
}

ProtocolStatus SessionRegistry::VerifyWholeFileDigest(
    const SessionTag sessionTag,
    const WholeFileDigest& computedDigest)
{
    auto bindingStateResult = FindBindingState(sessionTag);
    if (!bindingStateResult)
    {
        return ProtocolStatus::Failure(
            bindingStateResult.Error().code,
            bindingStateResult.Error().offset);
    }
    return bindingStateResult.Value()->VerifyWholeFileDigest(computedDigest);
}

ProtocolResult<bool> SessionRegistry::RemoveSession(
    const SessionId& sessionId) noexcept
{
    if (!HasConsistentRegistryState())
    {
        return ProtocolResult<bool>::Failure(
            ProtocolErrorCode::InternalDescriptorStateError,
            kSessionIdOffset);
    }

    const auto sessionEntry = sessionsById_.find(sessionId.bytes);
    if (sessionEntry == sessionsById_.end())
    {
        return ProtocolResult<bool>::Success(false);
    }

    const auto tagBinding = bindingsByTag_.find(
        sessionEntry->second.sessionTag.value);
    if (tagBinding == bindingsByTag_.end() ||
        tagBinding->second.ambiguous ||
        tagBinding->second.sessionId != sessionId)
    {
        return ProtocolResult<bool>::Failure(
            ProtocolErrorCode::InternalDescriptorStateError,
            kSessionIdOffset);
    }

    bindingsByTag_.erase(tagBinding);
    sessionsById_.erase(sessionEntry);
    reservedDescriptorStateBytes_ -= resourcePolicy_.maxDescriptorStateBytes;
    return ProtocolResult<bool>::Success(true);
}

std::size_t SessionRegistry::ActiveSessionCount() const noexcept
{
    return sessionsById_.size();
}

std::uint64_t SessionRegistry::ReservedDescriptorStateBytes() const noexcept
{
    return reservedDescriptorStateBytes_;
}

bool SessionRegistry::IsTagAmbiguous(const SessionTag sessionTag) const noexcept
{
    const auto tagBinding = bindingsByTag_.find(sessionTag.value);
    return tagBinding != bindingsByTag_.end() && tagBinding->second.ambiguous;
}

ProtocolResult<DescriptorBindingState*> SessionRegistry::FindBindingState(
    const SessionTag sessionTag) noexcept
{
    const auto tagBinding = bindingsByTag_.find(sessionTag.value);
    if (tagBinding == bindingsByTag_.end())
    {
        return ProtocolResult<DescriptorBindingState*>::Failure(
            ProtocolErrorCode::UnknownSession,
            kSegmentSessionTagOffset);
    }
    if (tagBinding->second.ambiguous)
    {
        return ProtocolResult<DescriptorBindingState*>::Failure(
            ProtocolErrorCode::SessionTagCollision,
            kSegmentSessionTagOffset);
    }

    const auto sessionEntry = sessionsById_.find(
        tagBinding->second.sessionId.bytes);
    if (sessionEntry == sessionsById_.end() ||
        !sessionEntry->second.bindingState ||
        sessionEntry->second.sessionTag != sessionTag)
    {
        return ProtocolResult<DescriptorBindingState*>::Failure(
            ProtocolErrorCode::InternalDescriptorStateError,
            kSegmentSessionTagOffset);
    }

    return ProtocolResult<DescriptorBindingState*>::Success(
        sessionEntry->second.bindingState.get());
}

ProtocolStatus SessionRegistry::RemoveUniqueSessionForCollision(
    const SessionTag sessionTag,
    TagBinding& tagBinding) noexcept
{
    const auto sessionEntry = sessionsById_.find(tagBinding.sessionId.bytes);
    if (!HasConsistentRegistryState() ||
        sessionEntry == sessionsById_.end() ||
        !sessionEntry->second.bindingState ||
        sessionEntry->second.sessionTag != sessionTag)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InternalDescriptorStateError,
            kSessionIdOffset);
    }

    sessionsById_.erase(sessionEntry);
    reservedDescriptorStateBytes_ -= resourcePolicy_.maxDescriptorStateBytes;
    return ProtocolStatus::Success();
}

bool SessionRegistry::HasConsistentRegistryState() const noexcept
{
    const auto activeSessionCountResult =
        CheckedNarrowUnsigned<std::uint64_t>(sessionsById_.size());
    if (!activeSessionCountResult)
    {
        return false;
    }

    const auto expectedReservationResult = CheckedMultiplyUint64(
        activeSessionCountResult.Value(),
        resourcePolicy_.maxDescriptorStateBytes);
    if (!expectedReservationResult ||
        expectedReservationResult.Value() != reservedDescriptorStateBytes_)
    {
        return false;
    }

    for (const auto& [sessionIdBytes, sessionEntry] : sessionsById_)
    {
        if (!sessionEntry.bindingState)
        {
            return false;
        }

        const auto tagBinding = bindingsByTag_.find(
            sessionEntry.sessionTag.value);
        if (tagBinding == bindingsByTag_.end() ||
            tagBinding->second.ambiguous ||
            tagBinding->second.sessionId.bytes != sessionIdBytes)
        {
            return false;
        }
    }

    std::size_t uniqueTagBindingCount = 0;
    for (const auto& [sessionTagValue, tagBinding] : bindingsByTag_)
    {
        if (tagBinding.ambiguous)
        {
            if (tagBinding.sessionId != SessionId{})
            {
                return false;
            }
            continue;
        }

        const auto sessionEntry = sessionsById_.find(
            tagBinding.sessionId.bytes);
        if (sessionEntry == sessionsById_.end() ||
            !sessionEntry->second.bindingState ||
            sessionEntry->second.sessionTag.value != sessionTagValue)
        {
            return false;
        }
        uniqueTagBindingCount++;
    }

    return uniqueTagBindingCount == sessionsById_.size();
}

} // namespace pbprotocol
