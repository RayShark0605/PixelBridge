#include "pbprotocol/orphan_transport_block_cache.h"

#include "pmr_byte_buffer.h"

#include "pbprotocol/checked_integer.h"
#include "pbprotocol/descriptor_codec.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <memory_resource>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace pbprotocol {

namespace detail {

struct OrphanTransportBlockMemoryBudgetState
{
    std::size_t limitBytes = 0;
    std::size_t bytesInUse = 0;
};

class OrphanTransportBlockBudgetExceeded final : public std::bad_alloc
{
};

class BoundedOrphanTransportBlockMemoryResource final
    : public std::pmr::memory_resource
{
public:
    BoundedOrphanTransportBlockMemoryResource(
        std::shared_ptr<OrphanTransportBlockMemoryBudgetState> memoryBudgetState,
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
            throw OrphanTransportBlockBudgetExceeded{};
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

    std::shared_ptr<OrphanTransportBlockMemoryBudgetState> memoryBudgetState_;
    std::shared_ptr<std::pmr::memory_resource> upstreamMemoryResource_;
};

struct OrphanTransportBlockKey
{
    std::uint64_t sessionTagValue = 0;
    std::uint64_t segmentOrdinal = 0;

    bool operator<(const OrphanTransportBlockKey& other) const noexcept
    {
        if (sessionTagValue != other.sessionTagValue)
        {
            return sessionTagValue < other.sessionTagValue;
        }
        return segmentOrdinal < other.segmentOrdinal;
    }
};

struct OrphanTransportBlockEntryState
{
    explicit OrphanTransportBlockEntryState(
        std::pmr::memory_resource* memoryResource)
        : paddedPayload(memoryResource)
    {
    }

    OrphanTransportBlockEntryState(const OrphanTransportBlockEntryState&) =
        delete;
    OrphanTransportBlockEntryState& operator=(
        const OrphanTransportBlockEntryState&) = delete;
    OrphanTransportBlockEntryState(OrphanTransportBlockEntryState&&) noexcept =
        default;
    OrphanTransportBlockEntryState& operator=(
        OrphanTransportBlockEntryState&&) noexcept = default;

    std::uint32_t outerBlockId = 0;
    std::uint16_t declaredPayloadBytes = 0;
    PmrByteBuffer paddedPayload;
};

struct OrphanTransportBlockKeyState
{
    // Set only when the creating Admit supplied an observation ordinal. A key
    // created without one never backfills, so waitObservations stays nullopt
    // for that key instead of undercounting the true first observation.
    std::optional<std::uint64_t> firstSeenObservationOrdinal = std::nullopt;
    bool conflicted = false;
    std::vector<OrphanTransportBlockEntryState> entries;
};

struct OrphanTransportBlockCacheImplementation
{
    OrphanTransportBlockCacheImplementation(
        ReceiverResourcePolicy receiverResourcePolicy,
        const std::size_t orphanByteLimitBytes,
        std::shared_ptr<std::pmr::memory_resource> upstreamMemoryResource)
        : resourcePolicy(std::move(receiverResourcePolicy)),
          memoryBudgetState(
              std::make_shared<OrphanTransportBlockMemoryBudgetState>(
                  OrphanTransportBlockMemoryBudgetState{orphanByteLimitBytes, 0})),
          memoryResource(
              std::make_shared<BoundedOrphanTransportBlockMemoryResource>(
                  memoryBudgetState,
                  std::move(upstreamMemoryResource))),
          orphanByteLimit(orphanByteLimitBytes)
    {
    }

    ReceiverResourcePolicy resourcePolicy;
    std::shared_ptr<OrphanTransportBlockMemoryBudgetState> memoryBudgetState;
    std::shared_ptr<std::pmr::memory_resource> memoryResource;
    // Lazy: a fresh cache reports zero occupancy, mirroring the control
    // reassembly storage. Only payload buffers flow through the bounded PMR
    // resource so maxOrphanTransportBytes stays an exact ceiling on cached
    // payload bytes; structure overhead (one map node per key plus one vector
    // slot per entry) is separately bounded by maxOrphanTransportBlocks.
    std::optional<
        std::map<OrphanTransportBlockKey, OrphanTransportBlockKeyState>>
        keys;
    std::size_t orphanByteLimit = 0;

    // Cumulative event counters (telemetry).
    std::uint64_t admittedBlockCount = 0;
    std::uint64_t droppedBlockCount = 0;
    std::uint64_t resourceExhaustedCount = 0;
    // Current-state counters.
    std::uint64_t conflictedKeyCount = 0;
    std::size_t cachedBlocks = 0;
    std::size_t cachedBytes = 0;
};

} // namespace detail

namespace {

void ReleaseKeyCapacityIfEmpty(
    detail::OrphanTransportBlockCacheImplementation& implementation) noexcept
{
    if (!implementation.keys || !implementation.keys->empty())
    {
        return;
    }

    implementation.keys.reset();
}

// Allocation failures must not leave an empty key behind: a zero-entry key
// would suppress first-seen recording for later Admits of the same segment.
void RollbackEmptyKey(
    detail::OrphanTransportBlockCacheImplementation& implementation,
    const detail::OrphanTransportBlockKey& key) noexcept
{
    if (!implementation.keys)
    {
        return;
    }

    auto iterator = implementation.keys->find(key);
    if (iterator == implementation.keys->end() ||
        !iterator->second.entries.empty())
    {
        return;
    }

    implementation.keys->erase(iterator);
    ReleaseKeyCapacityIfEmpty(implementation);
}

} // namespace

OrphanTransportBlockCache::OrphanTransportBlockCache(
    std::unique_ptr<detail::OrphanTransportBlockCacheImplementation>
        implementation) noexcept
    : implementation_(std::move(implementation))
{
}

OrphanTransportBlockCache::~OrphanTransportBlockCache() = default;

OrphanTransportBlockCache::OrphanTransportBlockCache(
    OrphanTransportBlockCache&& other) noexcept = default;

OrphanTransportBlockCache& OrphanTransportBlockCache::operator=(
    OrphanTransportBlockCache&& other) noexcept = default;

ProtocolResult<OrphanTransportBlockCache> OrphanTransportBlockCache::Create(
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
        return ProtocolResult<OrphanTransportBlockCache>::Failure(
            ProtocolErrorCode::ResourceExhausted,
            0);
    }
}

ProtocolResult<OrphanTransportBlockCache>
OrphanTransportBlockCache::CreateWithMemoryResource(
    ReceiverResourcePolicy resourcePolicy,
    std::shared_ptr<std::pmr::memory_resource> upstreamMemoryResource)
{
    if (!upstreamMemoryResource)
    {
        return ProtocolResult<OrphanTransportBlockCache>::Failure(
            ProtocolErrorCode::InvalidResourcePolicy,
            0);
    }

    const ProtocolStatus policyStatus = ValidateReceiverResourcePolicy(
        resourcePolicy);
    if (!policyStatus)
    {
        return ProtocolResult<OrphanTransportBlockCache>::Failure(
            policyStatus.Error().code,
            policyStatus.Error().offset);
    }
    const auto budgetSizeResult = CheckedUint64ToSize(
        resourcePolicy.maxOrphanTransportBytes);
    if (!budgetSizeResult)
    {
        return ProtocolResult<OrphanTransportBlockCache>::Failure(
            budgetSizeResult.Error().code,
            budgetSizeResult.Error().offset);
    }

    try
    {
        auto implementation =
            std::make_unique<detail::OrphanTransportBlockCacheImplementation>(
                std::move(resourcePolicy),
                budgetSizeResult.Value(),
                std::move(upstreamMemoryResource));
        return ProtocolResult<OrphanTransportBlockCache>::Success(
            OrphanTransportBlockCache(std::move(implementation)));
    }
    catch (const std::bad_alloc&)
    {
        return ProtocolResult<OrphanTransportBlockCache>::Failure(
            ProtocolErrorCode::ResourceExhausted,
            0);
    }
    catch (const std::length_error&)
    {
        return ProtocolResult<OrphanTransportBlockCache>::Failure(
            ProtocolErrorCode::ResourceExhausted,
            0);
    }
}

ProtocolStatus OrphanTransportBlockCache::Admit(
    const SessionTag sessionTag,
    const std::uint64_t segmentOrdinal,
    const std::uint32_t outerBlockId,
    const std::uint16_t declaredPayloadBytes,
    const std::span<const std::byte> paddedPayload,
    const std::optional<std::uint64_t> observationOrdinal)
{
    if (!implementation_)
    {
        return ProtocolStatus::Failure(
            ProtocolErrorCode::InternalInvariantViolation,
            0);
    }

    const ReceiverResourcePolicy& resourcePolicy =
        implementation_->resourcePolicy;
    if (paddedPayload.empty() || declaredPayloadBytes == 0 ||
        declaredPayloadBytes > paddedPayload.size())
    {
        return ProtocolStatus::Failure(ProtocolErrorCode::InvalidRecordSize, 0);
    }
    if (paddedPayload.size() > resourcePolicy.maxOuterBlockBytes)
    {
        SaturatingIncrementUnsigned(implementation_->droppedBlockCount);
        return ProtocolStatus::Failure(
            ProtocolErrorCode::ResourceLimitExceeded,
            0);
    }

    const std::size_t declaredPayloadSize = declaredPayloadBytes;
    for (std::size_t paddingIndex = declaredPayloadSize;
         paddingIndex < paddedPayload.size();
         paddingIndex++)
    {
        if (paddedPayload[paddingIndex] != std::byte{0})
        {
            return ProtocolStatus::Failure(
                ProtocolErrorCode::NonCanonicalPadding,
                paddingIndex);
        }
    }

    const detail::OrphanTransportBlockKey key{sessionTag.value, segmentOrdinal};
    if (implementation_->keys)
    {
        auto& keys = *implementation_->keys;
        const auto iterator = keys.find(key);
        if (iterator != keys.end())
        {
            detail::OrphanTransportBlockKeyState& keyState = iterator->second;
            if (keyState.conflicted)
            {
                return ProtocolStatus::Failure(
                    ProtocolErrorCode::OrphanPayloadConflict,
                    0);
            }

            for (const auto& entry : keyState.entries)
            {
                if (entry.outerBlockId != outerBlockId)
                {
                    continue;
                }
                if (entry.declaredPayloadBytes == declaredPayloadBytes &&
                    entry.paddedPayload.Size() == paddedPayload.size() &&
                    std::equal(
                        paddedPayload.begin(),
                        paddedPayload.end(),
                        entry.paddedPayload.Bytes().begin()))
                {
                    return ProtocolStatus::Success();
                }

                keyState.conflicted = true;
                implementation_->conflictedKeyCount++;
                return ProtocolStatus::Failure(
                    ProtocolErrorCode::OrphanPayloadConflict,
                    0);
            }
        }
    }

    // Only a new unique block consumes quota. Cached blocks are never evicted;
    // Carousel replay is the recovery path for a quota-dropped block.
    if (implementation_->cachedBlocks >=
            resourcePolicy.maxOrphanTransportBlocks ||
        !CheckedAddWithinLimit(
            implementation_->cachedBytes,
            paddedPayload.size(),
            implementation_->orphanByteLimit))
    {
        SaturatingIncrementUnsigned(implementation_->droppedBlockCount);
        return ProtocolStatus::Failure(
            ProtocolErrorCode::ResourceLimitExceeded,
            0);
    }

    bool keyCreated = false;
    try
    {
        if (!implementation_->keys)
        {
            implementation_->keys.emplace();
        }
        auto& keys = *implementation_->keys;
        auto iterator = keys.find(key);
        if (iterator == keys.end())
        {
            const auto insertion = keys.emplace(
                key,
                detail::OrphanTransportBlockKeyState{});
            iterator = insertion.first;
            keyCreated = true;
        }

        detail::OrphanTransportBlockKeyState& keyState = iterator->second;
        detail::OrphanTransportBlockEntryState entryState(
            implementation_->memoryResource.get());
        entryState.outerBlockId = outerBlockId;
        entryState.declaredPayloadBytes = declaredPayloadBytes;
        entryState.paddedPayload.Assign(paddedPayload);
        keyState.entries.push_back(std::move(entryState));
        if (keyCreated && observationOrdinal)
        {
            keyState.firstSeenObservationOrdinal = *observationOrdinal;
        }
    }
    catch (const detail::OrphanTransportBlockBudgetExceeded&)
    {
        RollbackEmptyKey(*implementation_, key);
        SaturatingIncrementUnsigned(implementation_->droppedBlockCount);
        return ProtocolStatus::Failure(
            ProtocolErrorCode::ResourceLimitExceeded,
            0);
    }
    catch (const std::bad_alloc&)
    {
        RollbackEmptyKey(*implementation_, key);
        SaturatingIncrementUnsigned(implementation_->resourceExhaustedCount);
        return ProtocolStatus::Failure(ProtocolErrorCode::ResourceExhausted, 0);
    }
    catch (const std::length_error&)
    {
        RollbackEmptyKey(*implementation_, key);
        SaturatingIncrementUnsigned(implementation_->resourceExhaustedCount);
        return ProtocolStatus::Failure(ProtocolErrorCode::ResourceExhausted, 0);
    }

    // Bookkeeping updates only after the allocation succeeded, so a failed
    // Admit never moves the counters.
    implementation_->cachedBlocks++;
    implementation_->cachedBytes += paddedPayload.size();
    SaturatingIncrementUnsigned(implementation_->admittedBlockCount);
    return ProtocolStatus::Success();
}

ProtocolResult<OrphanTransportBlockDrain> OrphanTransportBlockCache::Drain(
    const SessionTag sessionTag,
    const std::uint64_t segmentOrdinal,
    const std::optional<std::uint64_t> observationOrdinal)
{
    if (!implementation_)
    {
        return ProtocolResult<OrphanTransportBlockDrain>::Failure(
            ProtocolErrorCode::InternalInvariantViolation,
            0);
    }

    OrphanTransportBlockDrain drain;
    const detail::OrphanTransportBlockKey key{sessionTag.value, segmentOrdinal};
    if (implementation_->keys)
    {
        auto& keys = *implementation_->keys;
        auto iterator = keys.find(key);
        if (iterator != keys.end())
        {
            const detail::OrphanTransportBlockKeyState& keyState =
                iterator->second;
            if (keyState.conflicted)
            {
                return ProtocolResult<OrphanTransportBlockDrain>::Failure(
                    ProtocolErrorCode::OrphanPayloadConflict,
                    0);
            }

            std::optional<std::uint64_t> waitObservations = std::nullopt;
            if (observationOrdinal && keyState.firstSeenObservationOrdinal)
            {
                const std::uint64_t drainOrdinal = *observationOrdinal;
                const std::uint64_t firstSeenOrdinal =
                    *keyState.firstSeenObservationOrdinal;
                if (drainOrdinal < firstSeenOrdinal)
                {
                    return ProtocolResult<OrphanTransportBlockDrain>::Failure(
                        ProtocolErrorCode::InvalidObservationOrdinal,
                        0);
                }
                waitObservations = drainOrdinal - firstSeenOrdinal;
            }

            const std::size_t drainedBlockCount = keyState.entries.size();
            std::size_t drainedBytes = 0;
            try
            {
                drain.entries.reserve(keyState.entries.size());
                for (const auto& entry : keyState.entries)
                {
                    OrphanTransportBlockEntry copiedEntry;
                    copiedEntry.outerBlockId = entry.outerBlockId;
                    copiedEntry.declaredPayloadBytes =
                        entry.declaredPayloadBytes;
                    copiedEntry.paddedPayload.assign(
                        entry.paddedPayload.Bytes().begin(),
                        entry.paddedPayload.Bytes().end());
                    drainedBytes += entry.paddedPayload.Size();
                    drain.entries.push_back(std::move(copiedEntry));
                }
            }
            catch (const std::bad_alloc&)
            {
                SaturatingIncrementUnsigned(
                    implementation_->resourceExhaustedCount);
                return ProtocolResult<OrphanTransportBlockDrain>::Failure(
                    ProtocolErrorCode::ResourceExhausted,
                    0);
            }
            catch (const std::length_error&)
            {
                SaturatingIncrementUnsigned(
                    implementation_->resourceExhaustedCount);
                return ProtocolResult<OrphanTransportBlockDrain>::Failure(
                    ProtocolErrorCode::ResourceExhausted,
                    0);
            }

            // Erase the whole key: entries plus first-seen and conflict state.
            keys.erase(iterator);
            ReleaseKeyCapacityIfEmpty(*implementation_);
            implementation_->cachedBlocks -= drainedBlockCount;
            implementation_->cachedBytes -= drainedBytes;
            drain.waitObservations = waitObservations;
        }
    }

    return ProtocolResult<OrphanTransportBlockDrain>::Success(std::move(drain));
}

void OrphanTransportBlockCache::ClearSession(
    const SessionTag sessionTag) noexcept
{
    if (!implementation_ || !implementation_->keys)
    {
        return;
    }

    auto& keys = *implementation_->keys;
    for (auto iterator = keys.begin(); iterator != keys.end();)
    {
        if (iterator->first.sessionTagValue != sessionTag.value)
        {
            iterator++;
            continue;
        }

        std::size_t clearedBlockCount = 0;
        std::size_t clearedBytes = 0;
        for (const auto& entry : iterator->second.entries)
        {
            clearedBlockCount++;
            clearedBytes += entry.paddedPayload.Size();
        }
        if (iterator->second.conflicted)
        {
            implementation_->conflictedKeyCount--;
        }
        iterator = keys.erase(iterator);
        implementation_->cachedBlocks -= clearedBlockCount;
        implementation_->cachedBytes -= clearedBytes;
    }
    ReleaseKeyCapacityIfEmpty(*implementation_);
}

void OrphanTransportBlockCache::ClearAll() noexcept
{
    if (!implementation_)
    {
        return;
    }

    implementation_->keys.reset();
    implementation_->conflictedKeyCount = 0;
    implementation_->cachedBlocks = 0;
    implementation_->cachedBytes = 0;
}

std::uint64_t OrphanTransportBlockCache::GetAdmittedBlockCount() const noexcept
{
    return implementation_ ? implementation_->admittedBlockCount : 0;
}

std::uint64_t OrphanTransportBlockCache::GetDroppedBlockCount() const noexcept
{
    return implementation_ ? implementation_->droppedBlockCount : 0;
}

std::uint64_t OrphanTransportBlockCache::GetResourceExhaustedCount() const noexcept
{
    return implementation_ ? implementation_->resourceExhaustedCount : 0;
}

std::uint64_t OrphanTransportBlockCache::GetConflictedKeyCount() const noexcept
{
    return implementation_ ? implementation_->conflictedKeyCount : 0;
}

bool OrphanTransportBlockCache::HasCachedKey(
    const SessionTag sessionTag,
    const std::uint64_t segmentOrdinal) const noexcept
{
    if (!implementation_ || !implementation_->keys)
    {
        return false;
    }
    const detail::OrphanTransportBlockKey key{
        sessionTag.value,
        segmentOrdinal};
    return implementation_->keys->contains(key);
}

std::size_t OrphanTransportBlockCache::GetCachedBlockCount() const noexcept
{
    return implementation_ ? implementation_->cachedBlocks : 0;
}

std::size_t OrphanTransportBlockCache::GetCachedBytes() const noexcept
{
    return implementation_ ? implementation_->cachedBytes : 0;
}

} // namespace pbprotocol
