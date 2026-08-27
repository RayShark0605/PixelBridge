#include "pbprotocol/descriptor_binding.h"

#include "pbprotocol/checked_integer.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <utility>

namespace pbprotocol {

namespace detail {

struct DescriptorMemoryBudgetState
{
    std::size_t limitBytes = 0;
    std::size_t bytesInUse = 0;
};

class DescriptorStateBudgetExceeded final : public std::bad_alloc
{
};

class BoundedDescriptorMemoryResource final : public std::pmr::memory_resource
{
public:
    BoundedDescriptorMemoryResource(
        std::shared_ptr<DescriptorMemoryBudgetState> memoryBudgetState,
        std::shared_ptr<std::pmr::memory_resource> upstreamMemoryResource) noexcept
        : memoryBudgetState_(std::move(memoryBudgetState)),
          upstreamMemoryResource_(std::move(upstreamMemoryResource))
    {
    }

private:
    [[nodiscard]] void* do_allocate(
        const std::size_t bytes,
        const std::size_t alignment) override
    {
        if (bytes > memoryBudgetState_->limitBytes -
                memoryBudgetState_->bytesInUse)
        {
            throw DescriptorStateBudgetExceeded{};
        }

        void* const allocation = upstreamMemoryResource_->allocate(
            bytes,
            alignment);
        memoryBudgetState_->bytesInUse += bytes;
        return allocation;
    }

    void do_deallocate(
        void* const allocation,
        const std::size_t bytes,
        const std::size_t alignment) override
    {
        if (bytes > memoryBudgetState_->bytesInUse)
        {
            std::terminate();
        }

        upstreamMemoryResource_->deallocate(allocation, bytes, alignment);
        memoryBudgetState_->bytesInUse -= bytes;
    }

    [[nodiscard]] bool do_is_equal(
        const std::pmr::memory_resource& other) const noexcept override
    {
        return this == &other;
    }

    std::shared_ptr<DescriptorMemoryBudgetState> memoryBudgetState_;
    std::shared_ptr<std::pmr::memory_resource> upstreamMemoryResource_;
};

struct DescriptorBindingStorage
{
    struct BoundSegmentState
    {
        SegmentDescriptor descriptor;
        bool completed = false;
    };

    DescriptorBindingStorage(
        const std::size_t limitBytes,
        std::shared_ptr<std::pmr::memory_resource> upstreamMemoryResource)
        : memoryBudgetState(std::make_shared<DescriptorMemoryBudgetState>(
              DescriptorMemoryBudgetState{limitBytes, 0})),
          descriptorMemoryResource(
              std::make_shared<BoundedDescriptorMemoryResource>(
                  memoryBudgetState,
                  std::move(upstreamMemoryResource))),
          segmentsByOrdinal(descriptorMemoryResource.get()),
          ordinalsByRawOffset(descriptorMemoryResource.get())
    {
    }

    std::shared_ptr<DescriptorMemoryBudgetState> memoryBudgetState;
    std::shared_ptr<std::pmr::memory_resource> descriptorMemoryResource;
    std::pmr::map<std::uint64_t, BoundSegmentState> segmentsByOrdinal;
    std::pmr::map<std::uint64_t, std::uint64_t> ordinalsByRawOffset;
};

} // namespace detail

namespace {

constexpr std::size_t kSessionIdOffset = 4;
constexpr std::size_t kSegmentOrdinalOffset = 8;
constexpr std::size_t kSegmentRawOffsetOffset = 16;
constexpr std::size_t kFinalSessionIdOffset = 0;

template <typename ValueType>
[[nodiscard]] ProtocolResult<ValueType> FailureFrom(
    const ProtocolError& error)
{
    return ProtocolResult<ValueType>::Failure(error.code, error.offset);
}

} // namespace

BoundSegmentDescriptor::BoundSegmentDescriptor(
    SegmentDescriptor descriptor) noexcept
    : descriptor_(std::move(descriptor))
{
}

const SegmentDescriptor& BoundSegmentDescriptor::GetDescriptor() const noexcept
{
    return descriptor_;
}

ProtocolResult<DescriptorBindingState> DescriptorBindingState::Create(
    SessionDescriptor sessionDescriptor,
    ReceiverResourcePolicy resourcePolicy)
{
    try
    {
        auto upstreamMemoryResource =
            std::shared_ptr<std::pmr::memory_resource>(
                std::pmr::new_delete_resource(),
                [](std::pmr::memory_resource*) noexcept
                {
                });
        return CreateWithMemoryResource(
            std::move(sessionDescriptor),
            std::move(resourcePolicy),
            std::move(upstreamMemoryResource));
    }
    catch (const detail::DescriptorStateBudgetExceeded&)
    {
        return ProtocolResult<DescriptorBindingState>::Failure(
            ProtocolErrorCode::ResourceLimitExceeded,
            kSegmentOrdinalOffset);
    }
    catch (const std::bad_alloc&)
    {
        return ProtocolResult<DescriptorBindingState>::Failure(
            ProtocolErrorCode::ResourceExhausted,
            kSegmentOrdinalOffset);
    }
}

ProtocolResult<DescriptorBindingState>
DescriptorBindingState::CreateWithMemoryResource(
    SessionDescriptor sessionDescriptor,
    ReceiverResourcePolicy resourcePolicy,
    std::shared_ptr<std::pmr::memory_resource> upstreamMemoryResource)
{
    if (!upstreamMemoryResource)
    {
        return ProtocolResult<DescriptorBindingState>::Failure(
            ProtocolErrorCode::InvalidResourcePolicy,
            0);
    }

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

    const auto descriptorBudgetSizeResult = CheckedUint64ToSize(
        resourcePolicy.maxDescriptorStateBytes,
        kSegmentOrdinalOffset);
    if (!descriptorBudgetSizeResult)
    {
        return FailureFrom<DescriptorBindingState>(
            descriptorBudgetSizeResult.Error());
    }

    try
    {
        auto descriptorStorage =
            std::make_unique<detail::DescriptorBindingStorage>(
                descriptorBudgetSizeResult.Value(),
                std::move(upstreamMemoryResource));

        return ProtocolResult<DescriptorBindingState>::Success(
            DescriptorBindingState(
                std::move(sessionDescriptor),
                std::move(resourcePolicy),
                segmentCountResult.Value(),
                std::move(descriptorStorage)));
    }
    catch (const detail::DescriptorStateBudgetExceeded&)
    {
        return ProtocolResult<DescriptorBindingState>::Failure(
            ProtocolErrorCode::ResourceLimitExceeded,
            kSegmentOrdinalOffset);
    }
    catch (const std::bad_alloc&)
    {
        return ProtocolResult<DescriptorBindingState>::Failure(
            ProtocolErrorCode::ResourceExhausted,
            kSegmentOrdinalOffset);
    }
}

DescriptorBindingState::DescriptorBindingState(
    SessionDescriptor sessionDescriptor,
    ReceiverResourcePolicy resourcePolicy,
    const std::size_t segmentCount,
    std::unique_ptr<detail::DescriptorBindingStorage> descriptorStorage) noexcept
    : sessionDescriptor_(std::move(sessionDescriptor)),
      resourcePolicy_(std::move(resourcePolicy)),
      segmentCount_(segmentCount),
      descriptorStorage_(std::move(descriptorStorage))
{
}

DescriptorBindingState::~DescriptorBindingState() = default;

DescriptorBindingState::DescriptorBindingState(
    DescriptorBindingState&& other) noexcept = default;

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

    const auto existingOrdinal = descriptorStorage_->segmentsByOrdinal.find(
        descriptor.segmentOrdinal);
    if (existingOrdinal != descriptorStorage_->segmentsByOrdinal.end())
    {
        if (existingOrdinal->second.descriptor == descriptor)
        {
            return ProtocolResult<DescriptorBindDisposition>::Success(
                DescriptorBindDisposition::Repeated);
        }

        const ProtocolStatus conflictStatus = LatchTerminalError(
            ProtocolErrorCode::DescriptorConflict,
            kSegmentOrdinalOffset);
        return FailureFrom<DescriptorBindDisposition>(conflictStatus.Error());
    }

    if (descriptorStorage_->segmentsByOrdinal.size() >= segmentCount_)
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

    const auto nextOffset = descriptorStorage_->ordinalsByRawOffset.lower_bound(
        descriptor.rawOffset);
    if (nextOffset != descriptorStorage_->ordinalsByRawOffset.end() &&
        nextOffset->first < rawEnd)
    {
        const ProtocolStatus overlapStatus = LatchTerminalError(
            ProtocolErrorCode::SegmentOverlap,
            kSegmentRawOffsetOffset);
        return FailureFrom<DescriptorBindDisposition>(overlapStatus.Error());
    }
    if (nextOffset != descriptorStorage_->ordinalsByRawOffset.begin())
    {
        auto previousOffset = nextOffset;
        previousOffset--;
        const auto previousSegment = descriptorStorage_->segmentsByOrdinal.find(
            previousOffset->second);
        if (previousSegment == descriptorStorage_->segmentsByOrdinal.end())
        {
            const ProtocolStatus invariantStatus = LatchTerminalError(
                ProtocolErrorCode::InternalDescriptorStateError,
                kSegmentRawOffsetOffset);
            return FailureFrom<DescriptorBindDisposition>(
                invariantStatus.Error());
        }

        const auto previousEndResult = CheckedAddUint64(
            previousSegment->second.descriptor.rawOffset,
            previousSegment->second.descriptor.rawSize,
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
        const auto ordinalInsertion =
            descriptorStorage_->segmentsByOrdinal.emplace(
                descriptor.segmentOrdinal,
                detail::DescriptorBindingStorage::BoundSegmentState{
                    descriptor,
                    false});
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
            const auto offsetInsertion =
                descriptorStorage_->ordinalsByRawOffset.emplace(
                    descriptor.rawOffset,
                    descriptor.segmentOrdinal);
            if (!offsetInsertion.second)
            {
                descriptorStorage_->segmentsByOrdinal.erase(
                    ordinalInsertion.first);
                const ProtocolStatus overlapStatus = LatchTerminalError(
                    ProtocolErrorCode::SegmentOverlap,
                    kSegmentRawOffsetOffset);
                return FailureFrom<DescriptorBindDisposition>(
                    overlapStatus.Error());
            }
        }
        catch (...)
        {
            descriptorStorage_->segmentsByOrdinal.erase(
                ordinalInsertion.first);
            throw;
        }
    }
    catch (const detail::DescriptorStateBudgetExceeded&)
    {
        const ProtocolStatus resourceStatus = LatchTerminalError(
            ProtocolErrorCode::ResourceLimitExceeded,
            kSegmentOrdinalOffset);
        return FailureFrom<DescriptorBindDisposition>(resourceStatus.Error());
    }
    catch (const std::bad_alloc&)
    {
        const ProtocolStatus resourceStatus = LatchTerminalError(
            ProtocolErrorCode::ResourceExhausted,
            kSegmentOrdinalOffset);
        return FailureFrom<DescriptorBindDisposition>(resourceStatus.Error());
    }
    catch (const std::length_error&)
    {
        const ProtocolStatus resourceStatus = LatchTerminalError(
            ProtocolErrorCode::ResourceExhausted,
            kSegmentOrdinalOffset);
        return FailureFrom<DescriptorBindDisposition>(resourceStatus.Error());
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

    if (descriptorStorage_->segmentsByOrdinal.size() != segmentCount_)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::SegmentMapIncomplete,
            kSegmentOrdinalOffset);
    }
    if (descriptorStorage_->ordinalsByRawOffset.size() !=
        descriptorStorage_->segmentsByOrdinal.size())
    {
        return LatchTerminalError(
            ProtocolErrorCode::InternalDescriptorStateError,
            kSegmentRawOffsetOffset);
    }

    std::uint64_t expectedOrdinal = 0;
    for (const auto& [segmentOrdinal, segmentState] :
         descriptorStorage_->segmentsByOrdinal)
    {
        static_cast<void>(segmentState);
        if (segmentOrdinal != expectedOrdinal)
        {
            return LatchTerminalError(
                ProtocolErrorCode::InternalDescriptorStateError,
                kSegmentOrdinalOffset);
        }
        expectedOrdinal++;
    }

    std::uint64_t expectedRawOffset = 0;
    for (const auto& [rawOffset, segmentOrdinal] :
         descriptorStorage_->ordinalsByRawOffset)
    {
        const auto segment = descriptorStorage_->segmentsByOrdinal.find(
            segmentOrdinal);
        if (segment == descriptorStorage_->segmentsByOrdinal.end())
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
            segment->second.descriptor.rawSize,
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

const SessionDescriptor& DescriptorBindingState::GetSessionDescriptor() const noexcept
{
    return sessionDescriptor_;
}

ProtocolResult<BoundSegmentDescriptor>
DescriptorBindingState::GetBoundSegmentDescriptor(
    const std::uint64_t segmentOrdinal) const
{
    const ProtocolStatus terminalStatus = CheckTerminalState();
    if (!terminalStatus)
    {
        return FailureFrom<BoundSegmentDescriptor>(terminalStatus.Error());
    }
    if (segmentOrdinal >= sessionDescriptor_.segmentCount)
    {
        return ProtocolResult<BoundSegmentDescriptor>::Failure(
            ProtocolErrorCode::SegmentOrdinalOutOfRange,
            kSegmentOrdinalOffset);
    }

    const auto iterator = descriptorStorage_->segmentsByOrdinal.find(
        segmentOrdinal);
    if (iterator == descriptorStorage_->segmentsByOrdinal.end())
    {
        return ProtocolResult<BoundSegmentDescriptor>::Failure(
            ProtocolErrorCode::UnknownSegment,
            kSegmentOrdinalOffset);
    }

    return ProtocolResult<BoundSegmentDescriptor>::Success(
        BoundSegmentDescriptor(iterator->second.descriptor));
}

ProtocolResult<bool> DescriptorBindingState::IsSegmentCompleted(
    const std::uint64_t segmentOrdinal) const
{
    const ProtocolStatus terminalStatus = CheckTerminalState();
    if (!terminalStatus)
    {
        return FailureFrom<bool>(terminalStatus.Error());
    }
    if (segmentOrdinal >= sessionDescriptor_.segmentCount)
    {
        return ProtocolResult<bool>::Failure(
            ProtocolErrorCode::SegmentOrdinalOutOfRange,
            kSegmentOrdinalOffset);
    }

    const auto iterator = descriptorStorage_->segmentsByOrdinal.find(
        segmentOrdinal);
    if (iterator == descriptorStorage_->segmentsByOrdinal.end())
    {
        return ProtocolResult<bool>::Failure(
            ProtocolErrorCode::UnknownSegment,
            kSegmentOrdinalOffset);
    }
    return ProtocolResult<bool>::Success(iterator->second.completed);
}

ProtocolStatus DescriptorBindingState::MarkSegmentCompleted(
    const std::uint64_t segmentOrdinal)
{
    const ProtocolStatus terminalStatus = CheckTerminalState();
    if (!terminalStatus)
    {
        return terminalStatus;
    }
    if (segmentOrdinal >= sessionDescriptor_.segmentCount)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::SegmentOrdinalOutOfRange,
            kSegmentOrdinalOffset);
    }

    const auto iterator = descriptorStorage_->segmentsByOrdinal.find(
        segmentOrdinal);
    if (iterator == descriptorStorage_->segmentsByOrdinal.end())
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::UnknownSegment,
            kSegmentOrdinalOffset);
    }

    iterator->second.completed = true;
    return ProtocolStatus::Success();
}

ProtocolResult<FinalManifest> DescriptorBindingState::PrepareFinalization()
{
    const ProtocolStatus terminalStatus = CheckTerminalState();
    if (!terminalStatus)
    {
        return FailureFrom<FinalManifest>(terminalStatus.Error());
    }
    if (!finalManifest_.has_value())
    {
        return ProtocolResult<FinalManifest>::Failure(
            ProtocolErrorCode::MissingFinalManifest,
            kFinalSessionIdOffset);
    }

    const ProtocolStatus segmentMapStatus = ValidateCompleteSegmentMap();
    if (!segmentMapStatus)
    {
        return FailureFrom<FinalManifest>(segmentMapStatus.Error());
    }

    for (const auto& [segmentOrdinal, segmentState] :
         descriptorStorage_->segmentsByOrdinal)
    {
        if (!segmentState.completed)
        {
            return ProtocolResult<FinalManifest>::Failure(
                ProtocolErrorCode::SegmentRecoveryIncomplete,
                kSegmentOrdinalOffset);
        }
        static_cast<void>(segmentOrdinal);
    }

    return ProtocolResult<FinalManifest>::Success(*finalManifest_);
}

std::size_t DescriptorBindingState::BoundSegmentCount() const noexcept
{
    return descriptorStorage_->segmentsByOrdinal.size();
}

std::uint64_t DescriptorBindingState::DescriptorStateBytesInUse() const noexcept
{
    return static_cast<std::uint64_t>(
        descriptorStorage_->memoryBudgetState->bytesInUse);
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
