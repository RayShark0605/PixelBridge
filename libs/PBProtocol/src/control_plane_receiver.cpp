#include "pbprotocol/control_plane_receiver.h"

#include "pmr_byte_buffer.h"

#include "pbprotocol/checked_integer.h"
#include "pbprotocol/descriptor_codec.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <map>
#include <memory>
#include <memory_resource>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>

namespace pbprotocol {

namespace detail {

struct ControlReassemblyMemoryBudgetState
{
    std::size_t limitBytes = 0;
    std::size_t bytesInUse = 0;
};

class ControlReassemblyBudgetExceeded final : public std::bad_alloc
{
};

class BoundedControlReassemblyMemoryResource final
    : public std::pmr::memory_resource
{
public:
    BoundedControlReassemblyMemoryResource(
        std::shared_ptr<ControlReassemblyMemoryBudgetState> memoryBudgetState,
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
        if (memoryBudgetState_->bytesInUse >
                memoryBudgetState_->limitBytes ||
            bytes > memoryBudgetState_->limitBytes -
                memoryBudgetState_->bytesInUse)
        {
            throw ControlReassemblyBudgetExceeded{};
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

    std::shared_ptr<ControlReassemblyMemoryBudgetState> memoryBudgetState_;
    std::shared_ptr<std::pmr::memory_resource> upstreamMemoryResource_;
};

enum class ControlReassemblyState : std::uint8_t
{
    Collecting,
    PendingAdmission,
    Admitted,
    Conflict
};

struct ControlFragmentSlot
{
    explicit ControlFragmentSlot(std::pmr::memory_resource* memoryResource)
        : bytes(memoryResource)
    {
    }

    ControlFragmentSlot(const ControlFragmentSlot&) = delete;
    ControlFragmentSlot& operator=(const ControlFragmentSlot&) = delete;
    ControlFragmentSlot(ControlFragmentSlot&&) noexcept = default;
    ControlFragmentSlot& operator=(ControlFragmentSlot&&) noexcept = default;

    PmrByteBuffer bytes;
};

struct ControlReassemblyRecord
{
    ControlReassemblyRecord(
        const ControlFragmentView& fragment,
        const std::uint64_t observationOrdinal,
        std::pmr::memory_resource* memoryResource)
        : fragmentCount(fragment.fragmentCount),
          totalRecordBytes(fragment.totalRecordBytes),
          flags(fragment.flags),
          lastObservationOrdinal(observationOrdinal),
          fragmentsByIndex(std::in_place, memoryResource)
    {
    }

    ControlReassemblyRecord(const ControlReassemblyRecord&) = delete;
    ControlReassemblyRecord& operator=(const ControlReassemblyRecord&) = delete;
    ControlReassemblyRecord(ControlReassemblyRecord&&) noexcept = default;
    ControlReassemblyRecord& operator=(ControlReassemblyRecord&&) noexcept =
        default;

    std::uint16_t fragmentCount = 0;
    std::uint32_t totalRecordBytes = 0;
    std::uint16_t flags = 0;
    std::uint64_t lastObservationOrdinal = 0;
    std::size_t receivedFragmentCount = 0;
    std::size_t receivedBytes = 0;
    ControlReassemblyState state = ControlReassemblyState::Collecting;
    std::optional<std::pmr::map<std::uint16_t, ControlFragmentSlot>>
        fragmentsByIndex;
};

struct ControlReassemblyEntry
{
    ControlReassemblyEntry(
        const std::uint64_t recordIdentifier,
        const ControlFragmentView& fragment,
        const std::uint64_t observationOrdinal,
        std::pmr::memory_resource* memoryResource)
        : recordId(recordIdentifier),
          record(fragment, observationOrdinal, memoryResource)
    {
    }

    ControlReassemblyEntry(const ControlReassemblyEntry&) = delete;
    ControlReassemblyEntry& operator=(const ControlReassemblyEntry&) = delete;
    ControlReassemblyEntry(ControlReassemblyEntry&&) noexcept = default;
    ControlReassemblyEntry& operator=(ControlReassemblyEntry&&) noexcept =
        default;

    std::uint64_t recordId = 0;
    ControlReassemblyRecord record;
};

struct ControlReassemblyStorage
{
    ControlReassemblyStorage(
        const std::size_t limitBytes,
        std::shared_ptr<std::pmr::memory_resource> upstreamMemoryResource)
        : memoryBudgetState(
              std::make_shared<ControlReassemblyMemoryBudgetState>(
                  ControlReassemblyMemoryBudgetState{limitBytes, 0})),
          memoryResource(
              std::make_shared<BoundedControlReassemblyMemoryResource>(
                  memoryBudgetState,
                  std::move(upstreamMemoryResource)))
    {
    }

    std::shared_ptr<ControlReassemblyMemoryBudgetState> memoryBudgetState;
    std::shared_ptr<std::pmr::memory_resource> memoryResource;
    std::optional<std::pmr::deque<ControlReassemblyEntry>> records;
};

struct ControlPlaneReceiverImplementation
{
    ControlPlaneReceiverImplementation(
        ReceiverResourcePolicy receiverResourcePolicy,
        SessionRegistry sessionRegistry,
        const std::size_t controlReassemblyLimitBytes,
        std::shared_ptr<std::pmr::memory_resource> upstreamMemoryResource)
        : resourcePolicy(std::move(receiverResourcePolicy)),
          registry(std::move(sessionRegistry)),
          reassemblyStorage(
              controlReassemblyLimitBytes,
              std::move(upstreamMemoryResource))
    {
    }

    ReceiverResourcePolicy resourcePolicy;
    SessionRegistry registry;
    ControlReassemblyStorage reassemblyStorage;
    std::uint64_t currentObservationOrdinal = 0;
    bool hasObservationOrdinal = false;
    bool lastAdmissionFailureRetryable = false;
    std::optional<SessionTag> lastTerminalSessionTag;
    // Cumulative resource-policy rejection telemetry (see the public getter).
    std::uint64_t rejectedByResourcePolicyCount = 0;
};

} // namespace detail

namespace {

constexpr std::size_t kControlRecordTypeOffset = 5;
constexpr std::size_t kControlSessionTagOffset = 14;
constexpr std::size_t kControlRecordBytesOffset = 22;
constexpr std::size_t kControlPayloadOffset = kControlRecordPrefixBytes;
constexpr std::size_t kSessionDescriptorSessionIdOffset = 4;
constexpr std::size_t kFragmentIndexOffset = 8;
constexpr std::size_t kFragmentCountOffset = 10;
constexpr std::size_t kFragmentTotalRecordBytesOffset = 12;
constexpr std::size_t kFragmentBytesOffset = 16;
constexpr std::size_t kFragmentFlagsOffset = 18;

template <typename ValueType>
[[nodiscard]] ProtocolResult<ValueType> FailureFrom(
    const ProtocolError& error)
{
    return ProtocolResult<ValueType>::Failure(error.code, error.offset);
}

template <typename ValueType>
[[nodiscard]] ProtocolResult<ValueType> FailureFromPayload(
    const ProtocolError& error)
{
    const auto absoluteOffsetResult = CheckedAddSize(
        kControlPayloadOffset,
        error.offset);
    if (!absoluteOffsetResult)
    {
        return ProtocolResult<ValueType>::Failure(
            ProtocolErrorCode::InternalInvariantViolation,
            kControlPayloadOffset);
    }
    return ProtocolResult<ValueType>::Failure(
        error.code,
        absoluteOffsetResult.Value());
}

void ClearFragmentPayloads(
    detail::ControlReassemblyRecord& record) noexcept
{
    record.fragmentsByIndex.reset();
    record.receivedFragmentCount = 0;
    record.receivedBytes = 0;
}

void MarkConflict(
    detail::ControlReassemblyRecord& record) noexcept
{
    record.state = detail::ControlReassemblyState::Conflict;
    ClearFragmentPayloads(record);
}

void ReleaseRecordCapacityIfEmpty(
    detail::ControlReassemblyStorage& storage) noexcept
{
    if (!storage.records || !storage.records->empty())
    {
        return;
    }

    storage.records.reset();
}

[[nodiscard]] ProtocolResult<ControlFragmentReceiveResult> MakeProgressResult(
    const ControlFragmentReceiveDisposition disposition)
{
    return ProtocolResult<ControlFragmentReceiveResult>::Success(
        ControlFragmentReceiveResult{disposition, std::nullopt});
}

[[nodiscard]] ProtocolResult<ControlFragmentReceiveResult> MakeAdmissionResult(
    ControlRecordAdmission admission)
{
    const ControlFragmentReceiveDisposition disposition =
        admission.bindDisposition == DescriptorBindDisposition::Inserted
        ? ControlFragmentReceiveDisposition::DescriptorInserted
        : ControlFragmentReceiveDisposition::DescriptorRepeated;
    return ProtocolResult<ControlFragmentReceiveResult>::Success(
        ControlFragmentReceiveResult{
            disposition,
            std::move(admission)});
}

void EvictExpiredRecords(
    detail::ControlPlaneReceiverImplementation& implementation) noexcept
{
    if (!implementation.hasObservationOrdinal ||
        !implementation.reassemblyStorage.records)
    {
        return;
    }

    const std::uint64_t inactivityWindow = implementation.resourcePolicy.
        maxControlReassemblyInactivityObservations;
    auto& records = *implementation.reassemblyStorage.records;
    auto record = records.begin();
    while (record != records.end())
    {
        const std::uint64_t inactivityObservations =
            implementation.currentObservationOrdinal -
            record->record.lastObservationOrdinal;
        if (inactivityObservations > inactivityWindow)
        {
            record = records.erase(record);
        }
        else
        {
            record++;
        }
    }
    ReleaseRecordCapacityIfEmpty(implementation.reassemblyStorage);
}

// Resource-policy rejections are the observable quota-failure events of this
// component; protocol-level conflicts and malformed input are not counted.
[[nodiscard]] bool IsResourcePolicyRejection(
    const ProtocolErrorCode code) noexcept
{
    return code == ProtocolErrorCode::ResourceLimitExceeded ||
        code == ProtocolErrorCode::ResourceExhausted ||
        code == ProtocolErrorCode::ControlReassemblyQuotaExceeded;
}

} // namespace

ProtocolResult<ControlPlaneReceiver> ControlPlaneReceiver::Create(
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
            std::move(resourcePolicy),
            std::move(upstreamMemoryResource));
    }
    catch (const std::bad_alloc&)
    {
        return ProtocolResult<ControlPlaneReceiver>::Failure(
            ProtocolErrorCode::ResourceExhausted,
            0);
    }
}

ProtocolResult<ControlPlaneReceiver>
ControlPlaneReceiver::CreateWithMemoryResource(
    ReceiverResourcePolicy resourcePolicy,
    std::shared_ptr<std::pmr::memory_resource> upstreamMemoryResource)
{
    if (!upstreamMemoryResource)
    {
        return ProtocolResult<ControlPlaneReceiver>::Failure(
            ProtocolErrorCode::InvalidResourcePolicy,
            0);
    }

    const ProtocolStatus policyStatus = ValidateReceiverResourcePolicy(
        resourcePolicy);
    if (!policyStatus)
    {
        return FailureFrom<ControlPlaneReceiver>(policyStatus.Error());
    }
    const auto budgetSizeResult = CheckedUint64ToSize(
        resourcePolicy.maxControlReassemblyBytes);
    if (!budgetSizeResult)
    {
        return FailureFrom<ControlPlaneReceiver>(budgetSizeResult.Error());
    }

    auto registryResult = SessionRegistry::Create(resourcePolicy);
    if (!registryResult)
    {
        return FailureFrom<ControlPlaneReceiver>(registryResult.Error());
    }

    try
    {
        auto implementation =
            std::make_unique<detail::ControlPlaneReceiverImplementation>(
                std::move(resourcePolicy),
                std::move(registryResult).Value(),
                budgetSizeResult.Value(),
                std::move(upstreamMemoryResource));
        return ProtocolResult<ControlPlaneReceiver>::Success(
            ControlPlaneReceiver(std::move(implementation)));
    }
    catch (const std::bad_alloc&)
    {
        return ProtocolResult<ControlPlaneReceiver>::Failure(
            ProtocolErrorCode::ResourceExhausted,
            0);
    }
    catch (const std::length_error&)
    {
        return ProtocolResult<ControlPlaneReceiver>::Failure(
            ProtocolErrorCode::ResourceExhausted,
            0);
    }
}

ControlPlaneReceiver::ControlPlaneReceiver(
    std::unique_ptr<detail::ControlPlaneReceiverImplementation>
        implementation) noexcept
    : implementation_(std::move(implementation))
{
}

ControlPlaneReceiver::~ControlPlaneReceiver() = default;

ControlPlaneReceiver::ControlPlaneReceiver(
    ControlPlaneReceiver&& other) noexcept = default;

ControlPlaneReceiver& ControlPlaneReceiver::operator=(
    ControlPlaneReceiver&& other) noexcept = default;

ProtocolResult<ControlRecordAdmission> ControlPlaneReceiver::ReceiveControlRecord(
    const std::span<const std::byte> recordBytes)
{
    if (!implementation_)
    {
        return ProtocolResult<ControlRecordAdmission>::Failure(
            ProtocolErrorCode::InternalInvariantViolation,
            0);
    }

    implementation_->lastTerminalSessionTag.reset();
    auto admissionResult = ParseValidateAndBind(recordBytes);
    if (!admissionResult &&
        IsResourcePolicyRejection(admissionResult.Error().code))
    {
        SaturatingIncrementUnsigned(
            implementation_->rejectedByResourcePolicyCount);
    }
    return std::move(admissionResult);
}

ProtocolResult<ControlFragmentReceiveResult>
ControlPlaneReceiver::ReceiveControlFragment(
    const std::span<const std::byte> fragmentBytes,
    const std::uint64_t observationOrdinal)
{
    if (implementation_)
    {
        implementation_->lastTerminalSessionTag.reset();
    }
    auto receiveResult = ReceiveControlFragmentCore(
        fragmentBytes,
        observationOrdinal);
    if (!receiveResult &&
        IsResourcePolicyRejection(receiveResult.Error().code))
    {
        // Counted here (not inside AttemptAdmission) so the fragment path and
        // the record path each count exactly once per receive operation.
        SaturatingIncrementUnsigned(
            implementation_->rejectedByResourcePolicyCount);
    }
    return std::move(receiveResult);
}

ProtocolResult<ControlFragmentReceiveResult>
ControlPlaneReceiver::ReceiveControlFragmentCore(
    const std::span<const std::byte> fragmentBytes,
    const std::uint64_t observationOrdinal)
{
    if (!implementation_)
    {
        return ProtocolResult<ControlFragmentReceiveResult>::Failure(
            ProtocolErrorCode::InternalInvariantViolation,
            0);
    }

    const ProtocolStatus observationStatus = AdvanceObservationOrdinal(
        observationOrdinal);
    if (!observationStatus)
    {
        return FailureFrom<ControlFragmentReceiveResult>(
            observationStatus.Error());
    }

    const auto fragmentResult = ParseControlFragment(fragmentBytes);
    if (!fragmentResult)
    {
        return FailureFrom<ControlFragmentReceiveResult>(fragmentResult.Error());
    }
    const ControlFragmentView& fragment = fragmentResult.Value();
    if (fragment.totalRecordBytes >
        implementation_->resourcePolicy.maxControlRecordBytes)
    {
        return ProtocolResult<ControlFragmentReceiveResult>::Failure(
            ProtocolErrorCode::ResourceLimitExceeded,
            kFragmentTotalRecordBytesOffset);
    }
    if (fragment.fragmentCount >
        implementation_->resourcePolicy.maxControlFragmentsPerRecord)
    {
        return ProtocolResult<ControlFragmentReceiveResult>::Failure(
            ProtocolErrorCode::ResourceLimitExceeded,
            kFragmentCountOffset);
    }
    if (fragment.payload.size() >
        implementation_->resourcePolicy.maxControlRecordBytes)
    {
        return ProtocolResult<ControlFragmentReceiveResult>::Failure(
            ProtocolErrorCode::ResourceLimitExceeded,
            kFragmentBytesOffset);
    }

    try
    {
        detail::ControlReassemblyStorage& reassemblyStorage =
            implementation_->reassemblyStorage;
        if (!reassemblyStorage.records)
        {
            reassemblyStorage.records.emplace(
                reassemblyStorage.memoryResource.get());
        }
        auto& records = *reassemblyStorage.records;
        auto recordIterator = std::find_if(
            records.begin(),
            records.end(),
            [&fragment](const detail::ControlReassemblyEntry& entry) noexcept
            {
                return entry.recordId == fragment.controlRecordId;
            });
        bool inserted = false;

        if (recordIterator == records.end())
        {
            if (static_cast<std::uint64_t>(records.size()) >=
                implementation_->resourcePolicy.
                    maxConcurrentControlReassemblies)
            {
                return ProtocolResult<ControlFragmentReceiveResult>::Failure(
                    ProtocolErrorCode::ControlReassemblyQuotaExceeded,
                    0);
            }

            records.emplace_back(
                fragment.controlRecordId,
                fragment,
                observationOrdinal,
                implementation_->reassemblyStorage.memoryResource.get());
            recordIterator = records.end() - 1;
            inserted = true;
        }

        detail::ControlReassemblyRecord& record = recordIterator->record;
        record.lastObservationOrdinal = observationOrdinal;
        if (record.state == detail::ControlReassemblyState::Conflict)
        {
            return ProtocolResult<ControlFragmentReceiveResult>::Failure(
                ProtocolErrorCode::ControlFragmentConflict,
                0);
        }

        std::size_t metadataConflictOffset = 0;
        if (record.fragmentCount != fragment.fragmentCount)
        {
            metadataConflictOffset = kFragmentCountOffset;
        }
        else if (record.totalRecordBytes != fragment.totalRecordBytes)
        {
            metadataConflictOffset = kFragmentTotalRecordBytesOffset;
        }
        else if (record.flags != fragment.flags)
        {
            metadataConflictOffset = kFragmentFlagsOffset;
        }
        if (metadataConflictOffset != 0)
        {
            MarkConflict(record);
            return ProtocolResult<ControlFragmentReceiveResult>::Failure(
                ProtocolErrorCode::ControlFragmentConflict,
                metadataConflictOffset);
        }
        if (!record.fragmentsByIndex)
        {
            MarkConflict(record);
            return ProtocolResult<ControlFragmentReceiveResult>::Failure(
                ProtocolErrorCode::InternalInvariantViolation,
                kFragmentIndexOffset);
        }

        auto fragmentIterator = record.fragmentsByIndex->find(
            fragment.fragmentIndex);
        if (fragmentIterator != record.fragmentsByIndex->end())
        {
            const detail::ControlFragmentSlot& fragmentSlot =
                fragmentIterator->second;
            if (fragmentSlot.bytes.Size() != fragment.payload.size() ||
                !std::equal(
                    fragmentSlot.bytes.Bytes().begin(),
                    fragmentSlot.bytes.Bytes().end(),
                    fragment.payload.begin()))
            {
                MarkConflict(record);
                return ProtocolResult<ControlFragmentReceiveResult>::Failure(
                    ProtocolErrorCode::ControlFragmentConflict,
                    kFragmentBytesOffset);
            }

            if (record.state == detail::ControlReassemblyState::PendingAdmission)
            {
                return AttemptAdmission(*this, *implementation_, record);
            }
            return MakeProgressResult(
                ControlFragmentReceiveDisposition::Repeated);
        }

        if (record.state != detail::ControlReassemblyState::Collecting)
        {
            MarkConflict(record);
            return ProtocolResult<ControlFragmentReceiveResult>::Failure(
                ProtocolErrorCode::InternalInvariantViolation,
                kFragmentIndexOffset);
        }

        const auto receivedBytesResult = CheckedAddSize(
            record.receivedBytes,
            fragment.payload.size(),
            kFragmentTotalRecordBytesOffset);
        if (!receivedBytesResult ||
            receivedBytesResult.Value() > record.totalRecordBytes)
        {
            MarkConflict(record);
            return ProtocolResult<ControlFragmentReceiveResult>::Failure(
                ProtocolErrorCode::ControlFragmentConflict,
                kFragmentTotalRecordBytesOffset);
        }

        const auto receivedFragmentCountResult = CheckedAddSize(
            record.receivedFragmentCount,
            1U,
            kFragmentCountOffset);
        if (!receivedFragmentCountResult ||
            receivedFragmentCountResult.Value() > record.fragmentCount)
        {
            MarkConflict(record);
            return ProtocolResult<ControlFragmentReceiveResult>::Failure(
                ProtocolErrorCode::ControlFragmentConflict,
                kFragmentCountOffset);
        }
        const std::size_t remainingFragmentCount =
            static_cast<std::size_t>(record.fragmentCount) -
            receivedFragmentCountResult.Value();
        if (receivedBytesResult.Value() >
            static_cast<std::size_t>(record.totalRecordBytes) -
                remainingFragmentCount)
        {
            MarkConflict(record);
            return ProtocolResult<ControlFragmentReceiveResult>::Failure(
                ProtocolErrorCode::ControlFragmentConflict,
                kFragmentTotalRecordBytesOffset);
        }

        try
        {
            detail::ControlFragmentSlot fragmentSlot(
                implementation_->reassemblyStorage.memoryResource.get());
            fragmentSlot.bytes.Assign(fragment.payload);
            const auto fragmentInsertion = record.fragmentsByIndex->emplace(
                fragment.fragmentIndex,
                std::move(fragmentSlot));
            if (!fragmentInsertion.second)
            {
                MarkConflict(record);
                return ProtocolResult<ControlFragmentReceiveResult>::Failure(
                    ProtocolErrorCode::InternalInvariantViolation,
                    kFragmentIndexOffset);
            }
        }
        catch (...)
        {
            if (inserted)
            {
                records.pop_back();
                ReleaseRecordCapacityIfEmpty(
                    implementation_->reassemblyStorage);
            }
            throw;
        }
        record.receivedBytes = receivedBytesResult.Value();
        record.receivedFragmentCount = receivedFragmentCountResult.Value();

        if (record.receivedFragmentCount < record.fragmentCount)
        {
            return MakeProgressResult(
                ControlFragmentReceiveDisposition::Stored);
        }
        if (record.receivedFragmentCount != record.fragmentCount ||
            record.receivedBytes != record.totalRecordBytes)
        {
            MarkConflict(record);
            return ProtocolResult<ControlFragmentReceiveResult>::Failure(
                ProtocolErrorCode::ControlFragmentConflict,
                kFragmentTotalRecordBytesOffset);
        }

        return AttemptAdmission(*this, *implementation_, record);
    }
    catch (const detail::ControlReassemblyBudgetExceeded&)
    {
        ReleaseRecordCapacityIfEmpty(implementation_->reassemblyStorage);
        return ProtocolResult<ControlFragmentReceiveResult>::Failure(
            ProtocolErrorCode::ControlReassemblyQuotaExceeded,
            kFragmentBytesOffset);
    }
    catch (const std::bad_alloc&)
    {
        ReleaseRecordCapacityIfEmpty(implementation_->reassemblyStorage);
        return ProtocolResult<ControlFragmentReceiveResult>::Failure(
            ProtocolErrorCode::ResourceExhausted,
            kFragmentBytesOffset);
    }
    catch (const std::length_error&)
    {
        ReleaseRecordCapacityIfEmpty(implementation_->reassemblyStorage);
        return ProtocolResult<ControlFragmentReceiveResult>::Failure(
            ProtocolErrorCode::ResourceExhausted,
            kFragmentBytesOffset);
    }
}

ProtocolResult<ControlFragmentReceiveResult>
ControlPlaneReceiver::AttemptAdmission(
    ControlPlaneReceiver& receiver,
    detail::ControlPlaneReceiverImplementation& implementation,
    detail::ControlReassemblyRecord& record)
{
    record.state = detail::ControlReassemblyState::PendingAdmission;

    try
    {
        if (!record.fragmentsByIndex)
        {
            MarkConflict(record);
            return ProtocolResult<ControlFragmentReceiveResult>::Failure(
                ProtocolErrorCode::InternalInvariantViolation,
                kFragmentIndexOffset);
        }

        if (record.fragmentsByIndex->size() !=
            static_cast<std::size_t>(record.fragmentCount))
        {
            MarkConflict(record);
            return ProtocolResult<ControlFragmentReceiveResult>::Failure(
                ProtocolErrorCode::InternalInvariantViolation,
                kFragmentCountOffset);
        }

        detail::PmrByteBuffer recordBytes(
            implementation.reassemblyStorage.memoryResource.get());
        recordBytes.ResizeZeroed(record.totalRecordBytes);
        std::span<std::byte> mutableRecordBytes = recordBytes.MutableBytes();
        std::size_t writeOffset = 0;
        std::size_t expectedFragmentIndex = 0;
        for (const auto& [fragmentIndex, fragment] : *record.fragmentsByIndex)
        {
            if (static_cast<std::size_t>(fragmentIndex) !=
                    expectedFragmentIndex ||
                fragment.bytes.Size() == 0)
            {
                MarkConflict(record);
                return ProtocolResult<ControlFragmentReceiveResult>::Failure(
                    ProtocolErrorCode::InternalInvariantViolation,
                    kFragmentIndexOffset);
            }
            const auto nextWriteOffsetResult = CheckedAddSize(
                writeOffset,
                fragment.bytes.Size(),
                kFragmentTotalRecordBytesOffset);
            if (!nextWriteOffsetResult ||
                nextWriteOffsetResult.Value() > mutableRecordBytes.size())
            {
                MarkConflict(record);
                return ProtocolResult<ControlFragmentReceiveResult>::Failure(
                    ProtocolErrorCode::InternalInvariantViolation,
                    kFragmentTotalRecordBytesOffset);
            }
            std::copy(
                fragment.bytes.Bytes().begin(),
                fragment.bytes.Bytes().end(),
                mutableRecordBytes.begin() + writeOffset);
            writeOffset = nextWriteOffsetResult.Value();
            expectedFragmentIndex++;
        }
        if (expectedFragmentIndex !=
            static_cast<std::size_t>(record.fragmentCount))
        {
            MarkConflict(record);
            return ProtocolResult<ControlFragmentReceiveResult>::Failure(
                ProtocolErrorCode::InternalInvariantViolation,
                kFragmentIndexOffset);
        }
        if (writeOffset != recordBytes.Size())
        {
            MarkConflict(record);
            return ProtocolResult<ControlFragmentReceiveResult>::Failure(
                ProtocolErrorCode::ControlFragmentConflict,
                kFragmentTotalRecordBytesOffset);
        }

        auto admissionResult = receiver.ParseValidateAndBind(
            recordBytes.Bytes());
        if (!admissionResult)
        {
            if (!implementation.lastAdmissionFailureRetryable)
            {
                MarkConflict(record);
            }
            return FailureFrom<ControlFragmentReceiveResult>(
                admissionResult.Error());
        }

        record.state = detail::ControlReassemblyState::Admitted;
        return MakeAdmissionResult(std::move(admissionResult).Value());
    }
    catch (const detail::ControlReassemblyBudgetExceeded&)
    {
        return ProtocolResult<ControlFragmentReceiveResult>::Failure(
            ProtocolErrorCode::ControlReassemblyQuotaExceeded,
            kFragmentTotalRecordBytesOffset);
    }
    catch (const std::bad_alloc&)
    {
        return ProtocolResult<ControlFragmentReceiveResult>::Failure(
            ProtocolErrorCode::ResourceExhausted,
            kFragmentTotalRecordBytesOffset);
    }
    catch (const std::length_error&)
    {
        return ProtocolResult<ControlFragmentReceiveResult>::Failure(
            ProtocolErrorCode::ResourceExhausted,
            kFragmentTotalRecordBytesOffset);
    }
}

ProtocolStatus ControlPlaneReceiver::AdvanceObservationOrdinal(
    const std::uint64_t observationOrdinal) noexcept
{
    if (!implementation_)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InternalInvariantViolation,
            0);
    }
    if (implementation_->hasObservationOrdinal &&
        observationOrdinal < implementation_->currentObservationOrdinal)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InvalidObservationOrdinal,
            0);
    }

    implementation_->currentObservationOrdinal = observationOrdinal;
    implementation_->hasObservationOrdinal = true;
    EvictExpiredRecords(*implementation_);
    return ProtocolStatus::Success();
}

void ControlPlaneReceiver::ResetControlReassembly() noexcept
{
    if (!implementation_)
    {
        return;
    }

    implementation_->reassemblyStorage.records.reset();
    implementation_->currentObservationOrdinal = 0;
    implementation_->hasObservationOrdinal = false;
}

std::size_t ControlPlaneReceiver::ActiveSessionCount() const noexcept
{
    return implementation_ ? implementation_->registry.ActiveSessionCount() : 0;
}

std::uint64_t ControlPlaneReceiver::ReservedDescriptorStateBytes() const noexcept
{
    return implementation_
        ? implementation_->registry.ReservedDescriptorStateBytes()
        : 0;
}

std::size_t ControlPlaneReceiver::ActiveControlReassemblyCount() const noexcept
{
    if (!implementation_ || !implementation_->reassemblyStorage.records)
    {
        return 0;
    }
    return implementation_->reassemblyStorage.records->size();
}

std::size_t ControlPlaneReceiver::ControlReassemblyBytesInUse() const noexcept
{
    return implementation_
        ? implementation_->reassemblyStorage.memoryBudgetState->bytesInUse
        : 0;
}

std::uint64_t ControlPlaneReceiver::GetRejectedByResourcePolicyCount() const noexcept
{
    return implementation_
        ? implementation_->rejectedByResourcePolicyCount
        : 0;
}

std::optional<SessionTag>
ControlPlaneReceiver::GetLastTerminalSessionTag() const noexcept
{
    return implementation_
        ? implementation_->lastTerminalSessionTag
        : std::nullopt;
}

bool ControlPlaneReceiver::IsTagAmbiguous(
    const SessionTag sessionTag) const noexcept
{
    return implementation_ &&
        implementation_->registry.IsTagAmbiguous(sessionTag);
}

ProtocolResult<SessionDescriptor> ControlPlaneReceiver::GetSessionDescriptor(
    const SessionTag sessionTag) const
{
    if (!implementation_)
    {
        return ProtocolResult<SessionDescriptor>::Failure(
            ProtocolErrorCode::InternalInvariantViolation,
            0);
    }
    const auto bindingStateResult =
        static_cast<const SessionRegistry&>(implementation_->registry).
            FindBindingState(sessionTag);
    if (!bindingStateResult)
    {
        return FailureFrom<SessionDescriptor>(bindingStateResult.Error());
    }
    return ProtocolResult<SessionDescriptor>::Success(
        bindingStateResult.Value()->GetSessionDescriptor());
}

ProtocolResult<std::size_t> ControlPlaneReceiver::BoundSegmentCount(
    const SessionTag sessionTag) const noexcept
{
    if (!implementation_)
    {
        return ProtocolResult<std::size_t>::Failure(
            ProtocolErrorCode::InternalInvariantViolation,
            0);
    }
    const auto bindingStateResult =
        static_cast<const SessionRegistry&>(implementation_->registry).
            FindBindingState(sessionTag);
    if (!bindingStateResult)
    {
        return FailureFrom<std::size_t>(bindingStateResult.Error());
    }
    return ProtocolResult<std::size_t>::Success(
        bindingStateResult.Value()->BoundSegmentCount());
}

ProtocolResult<BoundSegmentDescriptor>
ControlPlaneReceiver::GetBoundSegmentDescriptor(
    const SessionTag sessionTag,
    const std::uint64_t segmentOrdinal) const
{
    if (!implementation_)
    {
        return ProtocolResult<BoundSegmentDescriptor>::Failure(
            ProtocolErrorCode::InternalInvariantViolation,
            0);
    }
    const auto bindingStateResult =
        static_cast<const SessionRegistry&>(implementation_->registry).
            FindBindingState(sessionTag);
    if (!bindingStateResult)
    {
        return FailureFrom<BoundSegmentDescriptor>(
            bindingStateResult.Error());
    }
    return bindingStateResult.Value()->GetBoundSegmentDescriptor(
        segmentOrdinal);
}

ProtocolResult<bool> ControlPlaneReceiver::IsSegmentCompleted(
    const SessionTag sessionTag,
    const std::uint64_t segmentOrdinal) const
{
    if (!implementation_)
    {
        return ProtocolResult<bool>::Failure(
            ProtocolErrorCode::InternalInvariantViolation,
            0);
    }
    const auto bindingStateResult =
        static_cast<const SessionRegistry&>(implementation_->registry).
            FindBindingState(sessionTag);
    if (!bindingStateResult)
    {
        return FailureFrom<bool>(bindingStateResult.Error());
    }
    return bindingStateResult.Value()->IsSegmentCompleted(segmentOrdinal);
}

ProtocolStatus ControlPlaneReceiver::MarkSegmentCompleted(
    const SessionTag sessionTag,
    const std::uint64_t segmentOrdinal)
{
    if (!implementation_)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InternalInvariantViolation,
            0);
    }
    const auto bindingStateResult = implementation_->registry.FindBindingState(
        sessionTag);
    if (!bindingStateResult)
    {
        return ProtocolStatus::Failure(
            bindingStateResult.Error().code,
            bindingStateResult.Error().offset);
    }
    return bindingStateResult.Value()->MarkSegmentCompleted(segmentOrdinal);
}

ProtocolResult<bool> ControlPlaneReceiver::HasFinalManifest(
    const SessionTag sessionTag) const noexcept
{
    if (!implementation_)
    {
        return ProtocolResult<bool>::Failure(
            ProtocolErrorCode::InternalInvariantViolation,
            0);
    }
    const auto bindingStateResult =
        static_cast<const SessionRegistry&>(implementation_->registry).
            FindBindingState(sessionTag);
    if (!bindingStateResult)
    {
        return FailureFrom<bool>(bindingStateResult.Error());
    }
    return ProtocolResult<bool>::Success(
        bindingStateResult.Value()->HasFinalManifest());
}

ProtocolStatus ControlPlaneReceiver::ValidateCompleteSegmentMap(
    const SessionTag sessionTag)
{
    if (!implementation_)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InternalInvariantViolation,
            0);
    }
    return implementation_->registry.ValidateCompleteSegmentMap(sessionTag);
}

ProtocolResult<bool> ControlPlaneReceiver::RemoveSession(
    const SessionId& sessionId) noexcept
{
    if (!implementation_)
    {
        return ProtocolResult<bool>::Failure(
            ProtocolErrorCode::InternalInvariantViolation,
            0);
    }
    return implementation_->registry.RemoveSession(sessionId);
}

ProtocolResult<ControlRecordAdmission>
ControlPlaneReceiver::ParseValidateAndBind(
    const std::span<const std::byte> recordBytes)
{
    implementation_->lastAdmissionFailureRetryable = false;
    if (recordBytes.size() > implementation_->resourcePolicy.maxControlRecordBytes)
    {
        return ProtocolResult<ControlRecordAdmission>::Failure(
            ProtocolErrorCode::ResourceLimitExceeded,
            kControlRecordBytesOffset);
    }

    const auto controlRecordResult = ParseControlRecord(recordBytes);
    if (!controlRecordResult)
    {
        return FailureFrom<ControlRecordAdmission>(controlRecordResult.Error());
    }
    const ControlRecordView& controlRecord = controlRecordResult.Value();

    switch (controlRecord.recordType)
    {
    case ControlRecordType::SessionDescriptor:
    {
        const auto descriptorResult = ParseSessionDescriptor(
            controlRecord.payload,
            implementation_->resourcePolicy);
        if (!descriptorResult)
        {
            return FailureFromPayload<ControlRecordAdmission>(
                descriptorResult.Error());
        }
        const SessionTag derivedSessionTag = DeriveSessionTag(
            descriptorResult.Value().sessionId);
        if (derivedSessionTag != controlRecord.sessionTag)
        {
            return ProtocolResult<ControlRecordAdmission>::Failure(
                ProtocolErrorCode::SessionTagMismatch,
                kControlSessionTagOffset);
        }

        const auto bindResult = implementation_->registry.BindSessionDescriptor(
            descriptorResult.Value());
        if (!bindResult)
        {
            const auto bindingStateResult = implementation_->registry.
                FindBindingState(controlRecord.sessionTag);
            if (bindingStateResult &&
                bindingStateResult.Value()->HasTerminalError())
            {
                implementation_->lastTerminalSessionTag =
                    controlRecord.sessionTag;
            }
            implementation_->lastAdmissionFailureRetryable =
                bindResult.Error().code == ProtocolErrorCode::ResourceExhausted ||
                (bindResult.Error().code ==
                     ProtocolErrorCode::ResourceLimitExceeded &&
                 bindResult.Error().offset ==
                     kSessionDescriptorSessionIdOffset);
            return FailureFromPayload<ControlRecordAdmission>(
                bindResult.Error());
        }
        return ProtocolResult<ControlRecordAdmission>::Success(
            ControlRecordAdmission{
                controlRecord.recordType,
                controlRecord.controlSequence,
                controlRecord.sessionTag,
                bindResult.Value()});
    }
    case ControlRecordType::SegmentDescriptor:
    {
        const auto bindingStateResult =
            implementation_->registry.FindBindingState(
                controlRecord.sessionTag);
        if (!bindingStateResult)
        {
            implementation_->lastAdmissionFailureRetryable =
                bindingStateResult.Error().code ==
                ProtocolErrorCode::UnknownSession;
            return ProtocolResult<ControlRecordAdmission>::Failure(
                bindingStateResult.Error().code,
                kControlSessionTagOffset);
        }

        const auto descriptorResult = ParseSegmentDescriptor(
            controlRecord.payload,
            bindingStateResult.Value()->GetSessionDescriptor(),
            implementation_->resourcePolicy);
        if (!descriptorResult)
        {
            return FailureFromPayload<ControlRecordAdmission>(
                descriptorResult.Error());
        }
        if (descriptorResult.Value().sessionTag != controlRecord.sessionTag)
        {
            return ProtocolResult<ControlRecordAdmission>::Failure(
                ProtocolErrorCode::SessionTagMismatch,
                kControlSessionTagOffset);
        }

        const auto bindResult = implementation_->registry.BindSegmentDescriptor(
            descriptorResult.Value());
        if (!bindResult)
        {
            if (bindingStateResult.Value()->HasTerminalError())
            {
                implementation_->lastTerminalSessionTag =
                    controlRecord.sessionTag;
            }
            return FailureFromPayload<ControlRecordAdmission>(
                bindResult.Error());
        }
        const auto boundDescriptorResult =
            bindingStateResult.Value()->GetBoundSegmentDescriptor(
                descriptorResult.Value().segmentOrdinal);
        if (!boundDescriptorResult)
        {
            return FailureFromPayload<ControlRecordAdmission>(
                boundDescriptorResult.Error());
        }
        return ProtocolResult<ControlRecordAdmission>::Success(
            ControlRecordAdmission{
                controlRecord.recordType,
                controlRecord.controlSequence,
                controlRecord.sessionTag,
                bindResult.Value(),
                std::move(boundDescriptorResult).Value()});
    }
    case ControlRecordType::FinalManifest:
    {
        const auto bindingStateResult =
            implementation_->registry.FindBindingState(
                controlRecord.sessionTag);
        if (!bindingStateResult)
        {
            implementation_->lastAdmissionFailureRetryable =
                bindingStateResult.Error().code ==
                ProtocolErrorCode::UnknownSession;
            return ProtocolResult<ControlRecordAdmission>::Failure(
                bindingStateResult.Error().code,
                kControlSessionTagOffset);
        }

        const auto manifestResult = ParseFinalManifest(
            controlRecord.payload,
            bindingStateResult.Value()->GetSessionDescriptor(),
            implementation_->resourcePolicy);
        if (!manifestResult)
        {
            return FailureFromPayload<ControlRecordAdmission>(
                manifestResult.Error());
        }
        if (DeriveSessionTag(manifestResult.Value().sessionId) !=
            controlRecord.sessionTag)
        {
            return ProtocolResult<ControlRecordAdmission>::Failure(
                ProtocolErrorCode::SessionTagMismatch,
                kControlSessionTagOffset);
        }

        const auto bindResult = implementation_->registry.BindFinalManifest(
            manifestResult.Value());
        if (!bindResult)
        {
            if (bindingStateResult.Value()->HasTerminalError())
            {
                implementation_->lastTerminalSessionTag =
                    controlRecord.sessionTag;
            }
            return FailureFromPayload<ControlRecordAdmission>(
                bindResult.Error());
        }
        return ProtocolResult<ControlRecordAdmission>::Success(
            ControlRecordAdmission{
                controlRecord.recordType,
                controlRecord.controlSequence,
                controlRecord.sessionTag,
                bindResult.Value()});
    }
    default:
        return ProtocolResult<ControlRecordAdmission>::Failure(
            ProtocolErrorCode::InvalidEnumValue,
            kControlRecordTypeOffset);
    }
}

} // namespace pbprotocol
