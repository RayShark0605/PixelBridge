#include "pbprotocol/descriptor_binding.h"

#include "pbprotocol/checked_integer.h"

#include <cstddef>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <utility>

namespace pbprotocol {

namespace {

constexpr std::size_t kSessionIdOffset = 4;
constexpr std::size_t kSegmentOrdinalOffset = 8;
constexpr std::size_t kSegmentRawOffsetOffset = 16;
constexpr std::size_t kFinalSessionIdOffset = 0;
constexpr std::size_t kFinalWholeFileDigestOffset = 32;

template <typename ValueType>
[[nodiscard]] ProtocolResult<ValueType> FailureFrom(
    const ProtocolError& error)
{
    return ProtocolResult<ValueType>::Failure(error.code, error.offset);
}

} // namespace

ProtocolResult<DescriptorBindingState> DescriptorBindingState::Create(
    SessionDescriptor sessionDescriptor,
    ReceiverResourcePolicy resourcePolicy)
{
    const ProtocolStatus validationStatus = ValidateSessionDescriptor(
        sessionDescriptor,
        resourcePolicy);
    if (!validationStatus)
    {
        return FailureFrom<DescriptorBindingState>(validationStatus.Error());
    }

    const auto segmentCountResult = CheckedUint64ToSize(
        sessionDescriptor.segmentCount,
        kSegmentOrdinalOffset);
    if (!segmentCountResult)
    {
        return FailureFrom<DescriptorBindingState>(segmentCountResult.Error());
    }

    return ProtocolResult<DescriptorBindingState>::Success(
        DescriptorBindingState(
            std::move(sessionDescriptor),
            std::move(resourcePolicy),
            segmentCountResult.Value()));
}

DescriptorBindingState::DescriptorBindingState(
    SessionDescriptor sessionDescriptor,
    ReceiverResourcePolicy resourcePolicy,
    const std::size_t segmentCount) noexcept
    : sessionDescriptor_(std::move(sessionDescriptor)),
      resourcePolicy_(std::move(resourcePolicy)),
      segmentCount_(segmentCount)
{
}

ProtocolResult<DescriptorBindDisposition>
DescriptorBindingState::BindSessionDescriptor(
    const SessionDescriptor& descriptor)
{
    const ProtocolStatus terminalStatus = CheckTerminalState();
    if (!terminalStatus)
    {
        return FailureFrom<DescriptorBindDisposition>(terminalStatus.Error());
    }

    const ProtocolStatus validationStatus = ValidateSessionDescriptor(
        descriptor,
        resourcePolicy_);
    if (!validationStatus)
    {
        return FailureFrom<DescriptorBindDisposition>(validationStatus.Error());
    }

    if (descriptor.sessionId != sessionDescriptor_.sessionId)
    {
        return ProtocolResult<DescriptorBindDisposition>::Failure(
            ProtocolErrorCode::SessionMismatch,
            kSessionIdOffset);
    }
    if (descriptor == sessionDescriptor_)
    {
        return ProtocolResult<DescriptorBindDisposition>::Success(
            DescriptorBindDisposition::Repeated);
    }

    const ProtocolStatus conflictStatus = LatchTerminalError(
        ProtocolErrorCode::DescriptorConflict,
        kSessionIdOffset);
    return FailureFrom<DescriptorBindDisposition>(conflictStatus.Error());
}

ProtocolResult<DescriptorBindDisposition>
DescriptorBindingState::BindSegmentDescriptor(
    const SegmentDescriptor& descriptor)
{
    const ProtocolStatus terminalStatus = CheckTerminalState();
    if (!terminalStatus)
    {
        return FailureFrom<DescriptorBindDisposition>(terminalStatus.Error());
    }

    const ProtocolStatus validationStatus = ValidateSegmentDescriptor(
        descriptor,
        sessionDescriptor_,
        resourcePolicy_);
    if (!validationStatus)
    {
        return FailureFrom<DescriptorBindDisposition>(validationStatus.Error());
    }

    const auto existingOrdinal = segmentsByOrdinal_.find(
        descriptor.segmentOrdinal);
    if (existingOrdinal != segmentsByOrdinal_.end())
    {
        if (existingOrdinal->second == descriptor)
        {
            return ProtocolResult<DescriptorBindDisposition>::Success(
                DescriptorBindDisposition::Repeated);
        }

        const ProtocolStatus conflictStatus = LatchTerminalError(
            ProtocolErrorCode::DescriptorConflict,
            kSegmentOrdinalOffset);
        return FailureFrom<DescriptorBindDisposition>(conflictStatus.Error());
    }

    if (segmentsByOrdinal_.size() >= segmentCount_)
    {
        return ProtocolResult<DescriptorBindDisposition>::Failure(
            ProtocolErrorCode::ResourceLimitExceeded,
            kSegmentOrdinalOffset);
    }

    const auto rawEndResult = CheckedAddUint64(
        descriptor.rawOffset,
        descriptor.rawSize,
        kSegmentRawOffsetOffset);
    if (!rawEndResult)
    {
        return FailureFrom<DescriptorBindDisposition>(rawEndResult.Error());
    }
    const std::uint64_t rawEnd = rawEndResult.Value();

    const auto nextOffset = ordinalsByRawOffset_.lower_bound(
        descriptor.rawOffset);
    if (nextOffset != ordinalsByRawOffset_.end() &&
        nextOffset->first < rawEnd)
    {
        const ProtocolStatus overlapStatus = LatchTerminalError(
            ProtocolErrorCode::SegmentOverlap,
            kSegmentRawOffsetOffset);
        return FailureFrom<DescriptorBindDisposition>(overlapStatus.Error());
    }
    if (nextOffset != ordinalsByRawOffset_.begin())
    {
        auto previousOffset = nextOffset;
        previousOffset--;
        const auto previousSegment = segmentsByOrdinal_.find(
            previousOffset->second);
        if (previousSegment == segmentsByOrdinal_.end())
        {
            const ProtocolStatus invariantStatus = LatchTerminalError(
                ProtocolErrorCode::InternalDescriptorStateError,
                kSegmentRawOffsetOffset);
            return FailureFrom<DescriptorBindDisposition>(
                invariantStatus.Error());
        }

        const auto previousEndResult = CheckedAddUint64(
            previousSegment->second.rawOffset,
            previousSegment->second.rawSize,
            kSegmentRawOffsetOffset);
        if (!previousEndResult)
        {
            const ProtocolStatus invariantStatus = LatchTerminalError(
                ProtocolErrorCode::InternalDescriptorStateError,
                kSegmentRawOffsetOffset);
            return FailureFrom<DescriptorBindDisposition>(
                invariantStatus.Error());
        }
        if (previousEndResult.Value() > descriptor.rawOffset)
        {
            const ProtocolStatus overlapStatus = LatchTerminalError(
                ProtocolErrorCode::SegmentOverlap,
                kSegmentRawOffsetOffset);
            return FailureFrom<DescriptorBindDisposition>(overlapStatus.Error());
        }
    }

    try
    {
        const auto ordinalInsertion = segmentsByOrdinal_.emplace(
            descriptor.segmentOrdinal,
            descriptor);
        if (!ordinalInsertion.second)
        {
            const ProtocolStatus invariantStatus = LatchTerminalError(
                ProtocolErrorCode::InternalDescriptorStateError,
                kSegmentOrdinalOffset);
            return FailureFrom<DescriptorBindDisposition>(
                invariantStatus.Error());
        }

        try
        {
            const auto offsetInsertion = ordinalsByRawOffset_.emplace(
                descriptor.rawOffset,
                descriptor.segmentOrdinal);
            if (!offsetInsertion.second)
            {
                segmentsByOrdinal_.erase(ordinalInsertion.first);
                const ProtocolStatus overlapStatus = LatchTerminalError(
                    ProtocolErrorCode::SegmentOverlap,
                    kSegmentRawOffsetOffset);
                return FailureFrom<DescriptorBindDisposition>(
                    overlapStatus.Error());
            }
        }
        catch (...)
        {
            segmentsByOrdinal_.erase(ordinalInsertion.first);
            throw;
        }
    }
    catch (const std::bad_alloc&)
    {
        return ProtocolResult<DescriptorBindDisposition>::Failure(
            ProtocolErrorCode::ResourceExhausted,
            kSegmentOrdinalOffset);
    }
    catch (const std::length_error&)
    {
        return ProtocolResult<DescriptorBindDisposition>::Failure(
            ProtocolErrorCode::ResourceExhausted,
            kSegmentOrdinalOffset);
    }

    return ProtocolResult<DescriptorBindDisposition>::Success(
        DescriptorBindDisposition::Inserted);
}

ProtocolResult<DescriptorBindDisposition>
DescriptorBindingState::BindFinalManifest(
    const FinalManifest& finalManifest)
{
    const ProtocolStatus terminalStatus = CheckTerminalState();
    if (!terminalStatus)
    {
        return FailureFrom<DescriptorBindDisposition>(terminalStatus.Error());
    }

    const ProtocolStatus validationStatus = ValidateFinalManifest(
        finalManifest,
        sessionDescriptor_,
        resourcePolicy_);
    if (!validationStatus)
    {
        return FailureFrom<DescriptorBindDisposition>(validationStatus.Error());
    }

    if (!finalManifest_.has_value())
    {
        finalManifest_ = finalManifest;
        return ProtocolResult<DescriptorBindDisposition>::Success(
            DescriptorBindDisposition::Inserted);
    }
    if (*finalManifest_ == finalManifest)
    {
        return ProtocolResult<DescriptorBindDisposition>::Success(
            DescriptorBindDisposition::Repeated);
    }

    const ProtocolStatus conflictStatus = LatchTerminalError(
        ProtocolErrorCode::DescriptorConflict,
        kFinalSessionIdOffset);
    return FailureFrom<DescriptorBindDisposition>(conflictStatus.Error());
}

ProtocolStatus DescriptorBindingState::ValidateCompleteSegmentMap()
{
    const ProtocolStatus terminalStatus = CheckTerminalState();
    if (!terminalStatus)
    {
        return terminalStatus;
    }

    if (segmentsByOrdinal_.size() != segmentCount_)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::SegmentMapIncomplete,
            kSegmentOrdinalOffset);
    }
    if (ordinalsByRawOffset_.size() != segmentsByOrdinal_.size())
    {
        return LatchTerminalError(
            ProtocolErrorCode::InternalDescriptorStateError,
            kSegmentRawOffsetOffset);
    }

    std::uint64_t expectedOrdinal = 0;
    for (const auto& [segmentOrdinal, descriptor] : segmentsByOrdinal_)
    {
        static_cast<void>(descriptor);
        if (segmentOrdinal != expectedOrdinal)
        {
            return LatchTerminalError(
                ProtocolErrorCode::SegmentGap,
                kSegmentOrdinalOffset);
        }
        expectedOrdinal++;
    }

    std::uint64_t expectedRawOffset = 0;
    for (const auto& [rawOffset, segmentOrdinal] : ordinalsByRawOffset_)
    {
        const auto segment = segmentsByOrdinal_.find(segmentOrdinal);
        if (segment == segmentsByOrdinal_.end())
        {
            return LatchTerminalError(
                ProtocolErrorCode::InternalDescriptorStateError,
                kSegmentRawOffsetOffset);
        }

        if (rawOffset < expectedRawOffset)
        {
            return LatchTerminalError(
                ProtocolErrorCode::SegmentOverlap,
                kSegmentRawOffsetOffset);
        }
        if (rawOffset > expectedRawOffset)
        {
            return LatchTerminalError(
                ProtocolErrorCode::SegmentGap,
                kSegmentRawOffsetOffset);
        }

        const auto rawEndResult = CheckedAddUint64(
            rawOffset,
            segment->second.rawSize,
            kSegmentRawOffsetOffset);
        if (!rawEndResult)
        {
            return LatchTerminalError(
                rawEndResult.Error().code,
                rawEndResult.Error().offset);
        }
        expectedRawOffset = rawEndResult.Value();
    }

    if (expectedRawOffset != sessionDescriptor_.originalFileSize)
    {
        return LatchTerminalError(
            ProtocolErrorCode::SegmentGap,
            kSegmentRawOffsetOffset);
    }

    return ProtocolStatus::Success();
}

ProtocolStatus DescriptorBindingState::ValidateReadyForFinalVerification()
{
    const ProtocolStatus mapStatus = ValidateCompleteSegmentMap();
    if (!mapStatus)
    {
        return mapStatus;
    }
    if (!finalManifest_.has_value())
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::MissingFinalManifest,
            kFinalSessionIdOffset);
    }

    return ProtocolStatus::Success();
}

ProtocolStatus DescriptorBindingState::VerifyWholeFileDigest(
    const WholeFileDigest& computedDigest)
{
    const ProtocolStatus readyStatus = ValidateReadyForFinalVerification();
    if (!readyStatus)
    {
        return readyStatus;
    }

    if (computedDigest != finalManifest_->wholeFileDigest)
    {
        return LatchTerminalError(
            ProtocolErrorCode::DigestMismatch,
            kFinalWholeFileDigestOffset);
    }

    return ProtocolStatus::Success();
}

const SessionDescriptor& DescriptorBindingState::GetSessionDescriptor() const noexcept
{
    return sessionDescriptor_;
}

std::size_t DescriptorBindingState::BoundSegmentCount() const noexcept
{
    return segmentsByOrdinal_.size();
}

bool DescriptorBindingState::HasFinalManifest() const noexcept
{
    return finalManifest_.has_value();
}

bool DescriptorBindingState::HasTerminalError() const noexcept
{
    return terminalError_.code != ProtocolErrorCode::None;
}

ProtocolErrorCode DescriptorBindingState::TerminalError() const noexcept
{
    return terminalError_.code;
}

ProtocolStatus DescriptorBindingState::CheckTerminalState() const noexcept
{
    if (!HasTerminalError())
    {
        return ProtocolStatus::Success();
    }
    return ProtocolStatus::Failure(
        terminalError_.code,
        terminalError_.offset);
}

ProtocolStatus DescriptorBindingState::LatchTerminalError(
    const ProtocolErrorCode errorCode,
    const std::size_t errorOffset) noexcept
{
    if (!HasTerminalError())
    {
        terminalError_ = ProtocolError{errorCode, errorOffset};
    }
    return ProtocolStatus::Failure(
        terminalError_.code,
        terminalError_.offset);
}

} // namespace pbprotocol
