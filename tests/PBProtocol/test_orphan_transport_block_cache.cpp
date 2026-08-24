#include "descriptor_test_helpers.h"

#include "pbprotocol/orphan_transport_block_cache.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <span>
#include <vector>

namespace {

using pbprotocol::OrphanTransportBlockCache;
using pbprotocol::ProtocolErrorCode;
using pbprotocol::ReceiverResourcePolicy;
using pbprotocol::SessionTag;

[[nodiscard]] std::vector<std::byte> MakePayload(
    const std::size_t byteCount,
    const std::uint8_t base)
{
    std::vector<std::byte> payload(byteCount);
    for (std::size_t byteIndex = 0; byteIndex < byteCount; byteIndex++)
    {
        payload[byteIndex] =
            static_cast<std::byte>(base + byteIndex % 26U);
    }
    return payload;
}

[[nodiscard]] ReceiverResourcePolicy MakeOrphanPolicy(
    const std::uint64_t orphanBytes,
    const std::uint64_t orphanBlocks)
{
    ReceiverResourcePolicy resourcePolicy =
        pbprotocol::test::MakeResourcePolicy();
    resourcePolicy.maxOrphanTransportBytes = orphanBytes;
    resourcePolicy.maxOrphanTransportBlocks = orphanBlocks;
    return resourcePolicy;
}

class FailOnAllocationMemoryResource final : public std::pmr::memory_resource
{
public:
    void FailAfterSuccessfulAllocations(
        const std::size_t successfulAllocations) noexcept
    {
        failedAllocationIndex_ = allocationCount_ + successfulAllocations;
    }

    [[nodiscard]] std::size_t OutstandingAllocations() const noexcept
    {
        return outstandingAllocations_;
    }

private:
    [[nodiscard]] void* do_allocate(
        const std::size_t bytes,
        const std::size_t alignment) override
    {
        const std::size_t allocationIndex = allocationCount_;
        allocationCount_++;
        if (allocationIndex == failedAllocationIndex_)
        {
            throw std::bad_alloc{};
        }

        void* const allocation = std::pmr::new_delete_resource()->allocate(
            bytes,
            alignment);
        outstandingAllocations_++;
        return allocation;
    }

    void do_deallocate(
        void* const allocation,
        const std::size_t bytes,
        const std::size_t alignment) override
    {
        std::pmr::new_delete_resource()->deallocate(allocation, bytes, alignment);
        outstandingAllocations_--;
    }

    [[nodiscard]] bool do_is_equal(
        const std::pmr::memory_resource& other) const noexcept override
    {
        return this == &other;
    }

    std::size_t failedAllocationIndex_ =
        std::numeric_limits<std::size_t>::max();
    std::size_t allocationCount_ = 0;
    std::size_t outstandingAllocations_ = 0;
};

} // namespace

TEST_CASE("Fresh orphan cache reports zero telemetry and empty drains",
          "[pbprotocol][orphan]")
{
    auto cacheResult = OrphanTransportBlockCache::Create(
        MakeOrphanPolicy(4096, 8));
    REQUIRE(cacheResult.HasValue());
    OrphanTransportBlockCache& cache = cacheResult.Value();

    REQUIRE(cache.GetAdmittedBlockCount() == 0);
    REQUIRE(cache.GetDroppedBlockCount() == 0);
    REQUIRE(cache.GetConflictedKeyCount() == 0);
    REQUIRE(cache.GetCachedBlockCount() == 0);
    REQUIRE(cache.GetCachedBytes() == 0);

    // Draining an unknown key is a normal case: empty success.
    const auto drainResult = cache.Drain(SessionTag{1}, 0);
    REQUIRE(drainResult.HasValue());
    REQUIRE(drainResult.Value().entries.empty());
    REQUIRE_FALSE(drainResult.Value().waitObservations.has_value());
}

TEST_CASE("Orphan blocks admit and drain in arrival order",
          "[pbprotocol][orphan]")
{
    auto cacheResult = OrphanTransportBlockCache::Create(
        MakeOrphanPolicy(4096, 8));
    REQUIRE(cacheResult.HasValue());
    OrphanTransportBlockCache& cache = cacheResult.Value();

    const SessionTag sessionTag{7};
    const std::vector<std::byte> payloadA = MakePayload(12, 0x41);
    const std::vector<std::byte> payloadB = MakePayload(8, 0x42);
    const std::vector<std::byte> payloadC = MakePayload(16, 0x43);

    REQUIRE(cache.Admit(sessionTag, 3, 5, payloadA));
    REQUIRE(cache.Admit(sessionTag, 3, 1, payloadB));
    REQUIRE(cache.Admit(sessionTag, 3, 3, payloadC));
    REQUIRE(cache.GetAdmittedBlockCount() == 3);
    REQUIRE(cache.GetCachedBlockCount() == 3);
    REQUIRE(cache.GetCachedBytes() ==
        payloadA.size() + payloadB.size() + payloadC.size());

    const auto drainResult = cache.Drain(sessionTag, 3);
    REQUIRE(drainResult.HasValue());
    const std::vector<pbprotocol::OrphanTransportBlockEntry>& entries =
        drainResult.Value().entries;
    REQUIRE(entries.size() == 3);
    REQUIRE(entries[0].outerBlockId == 5);
    REQUIRE(entries[0].payloadBytes == payloadA);
    REQUIRE(entries[1].outerBlockId == 1);
    REQUIRE(entries[1].payloadBytes == payloadB);
    REQUIRE(entries[2].outerBlockId == 3);
    REQUIRE(entries[2].payloadBytes == payloadC);

    // The key is cleared after drain: a second drain is an empty success.
    const auto secondDrain = cache.Drain(sessionTag, 3);
    REQUIRE(secondDrain.HasValue());
    REQUIRE(secondDrain.Value().entries.empty());
    REQUIRE(cache.GetCachedBlockCount() == 0);
    REQUIRE(cache.GetCachedBytes() == 0);
}

TEST_CASE("Orphan byte quota admits inclusively and drops only the incoming block",
          "[pbprotocol][orphan]")
{
    auto cacheResult = OrphanTransportBlockCache::Create(
        MakeOrphanPolicy(120, 64));
    REQUIRE(cacheResult.HasValue());
    OrphanTransportBlockCache& cache = cacheResult.Value();

    const SessionTag sessionTag{9};
    const std::vector<std::byte> payload = MakePayload(40, 0x50);
    for (std::uint32_t blockId = 0; blockId < 3; blockId++)
    {
        REQUIRE(cache.Admit(sessionTag, 1, blockId, payload));
    }
    REQUIRE(cache.GetCachedBytes() == 120);

    const pbprotocol::ProtocolStatus droppedStatus =
        cache.Admit(sessionTag, 1, 99, payload);
    REQUIRE_FALSE(droppedStatus);
    REQUIRE(droppedStatus.Error().code
        == ProtocolErrorCode::ResourceLimitExceeded);
    REQUIRE(cache.GetDroppedBlockCount() == 1);

    // Cached blocks are never evicted to make room.
    REQUIRE(cache.GetCachedBlockCount() == 3);
    REQUIRE(cache.GetCachedBytes() == 120);
    const auto drainResult = cache.Drain(sessionTag, 1);
    REQUIRE(drainResult.HasValue());
    REQUIRE(drainResult.Value().entries.size() == 3);
}

TEST_CASE("Orphan byte quota drops before exceeding the budget",
          "[pbprotocol][orphan]")
{
    auto cacheResult = OrphanTransportBlockCache::Create(
        MakeOrphanPolicy(100, 64));
    REQUIRE(cacheResult.HasValue());
    OrphanTransportBlockCache& cache = cacheResult.Value();

    const SessionTag sessionTag{9};
    const std::vector<std::byte> payload = MakePayload(40, 0x51);
    REQUIRE(cache.Admit(sessionTag, 2, 0, payload));
    REQUIRE(cache.Admit(sessionTag, 2, 1, payload));
    REQUIRE(cache.GetCachedBytes() == 80);

    const pbprotocol::ProtocolStatus droppedStatus =
        cache.Admit(sessionTag, 2, 2, payload);
    REQUIRE_FALSE(droppedStatus);
    REQUIRE(droppedStatus.Error().code
        == ProtocolErrorCode::ResourceLimitExceeded);
    REQUIRE(cache.GetDroppedBlockCount() == 1);
    REQUIRE(cache.GetCachedBytes() == 80);
}

TEST_CASE("Orphan block count quota drops with byte budget remaining",
          "[pbprotocol][orphan]")
{
    auto cacheResult = OrphanTransportBlockCache::Create(
        MakeOrphanPolicy(4096, 2));
    REQUIRE(cacheResult.HasValue());
    OrphanTransportBlockCache& cache = cacheResult.Value();

    const SessionTag sessionTag{11};
    const std::vector<std::byte> payload = MakePayload(8, 0x52);
    REQUIRE(cache.Admit(sessionTag, 4, 0, payload));
    REQUIRE(cache.Admit(sessionTag, 4, 1, payload));

    const pbprotocol::ProtocolStatus droppedStatus =
        cache.Admit(sessionTag, 4, 2, payload);
    REQUIRE_FALSE(droppedStatus);
    REQUIRE(droppedStatus.Error().code
        == ProtocolErrorCode::ResourceLimitExceeded);
    REQUIRE(cache.GetDroppedBlockCount() == 1);
    REQUIRE(cache.GetCachedBytes() == 16);
}

TEST_CASE("Orphan payloads outside the transport width fail closed",
          "[pbprotocol][orphan]")
{
    ReceiverResourcePolicy resourcePolicy = MakeOrphanPolicy(4096, 8);
    resourcePolicy.maxOuterBlockBytes = 8;
    auto cacheResult = OrphanTransportBlockCache::Create(resourcePolicy);
    REQUIRE(cacheResult.HasValue());
    OrphanTransportBlockCache& cache = cacheResult.Value();

    const SessionTag sessionTag{13};
    const std::span<const std::byte> emptyPayload{};
    const pbprotocol::ProtocolStatus emptyStatus =
        cache.Admit(sessionTag, 5, 0, emptyPayload);
    REQUIRE_FALSE(emptyStatus);
    REQUIRE(emptyStatus.Error().code == ProtocolErrorCode::InvalidRecordSize);

    const std::vector<std::byte> oversizedPayload = MakePayload(9, 0x53);
    const pbprotocol::ProtocolStatus oversizedStatus = cache.Admit(
        sessionTag, 5, 1, std::span<const std::byte>(oversizedPayload));
    REQUIRE_FALSE(oversizedStatus);
    REQUIRE(oversizedStatus.Error().code
        == ProtocolErrorCode::ResourceLimitExceeded);

    const std::vector<std::byte> exactPayload = MakePayload(8, 0x54);
    REQUIRE(cache.Admit(sessionTag, 5, 2, std::span<const std::byte>(exactPayload)));
    REQUIRE(cache.GetCachedBlockCount() == 1);
}

TEST_CASE("Repeated identical orphan block is an idempotent no-op",
          "[pbprotocol][orphan]")
{
    auto cacheResult = OrphanTransportBlockCache::Create(
        MakeOrphanPolicy(4096, 8));
    REQUIRE(cacheResult.HasValue());
    OrphanTransportBlockCache& cache = cacheResult.Value();

    const SessionTag sessionTag{15};
    const std::vector<std::byte> payload = MakePayload(24, 0x55);
    REQUIRE(cache.Admit(sessionTag, 6, 3, payload));
    REQUIRE(cache.Admit(sessionTag, 6, 3, payload));

    REQUIRE(cache.GetAdmittedBlockCount() == 1);
    REQUIRE(cache.GetCachedBlockCount() == 1);
    const auto drainResult = cache.Drain(sessionTag, 6);
    REQUIRE(drainResult.HasValue());
    REQUIRE(drainResult.Value().entries.size() == 1);
}

TEST_CASE("Conflicting orphan payloads latch the key terminally",
          "[pbprotocol][orphan]")
{
    auto cacheResult = OrphanTransportBlockCache::Create(
        MakeOrphanPolicy(4096, 8));
    REQUIRE(cacheResult.HasValue());
    OrphanTransportBlockCache& cache = cacheResult.Value();

    const SessionTag sessionA{17};
    const SessionTag sessionB{18};
    const std::vector<std::byte> payloadX = MakePayload(16, 0x56);
    const std::vector<std::byte> payloadY = MakePayload(16, 0x57);

    REQUIRE(cache.Admit(sessionA, 7, 1, payloadX));
    const pbprotocol::ProtocolStatus conflictStatus =
        cache.Admit(sessionA, 7, 1, payloadY);
    REQUIRE_FALSE(conflictStatus);
    REQUIRE(conflictStatus.Error().code
        == ProtocolErrorCode::OrphanPayloadConflict);
    REQUIRE(cache.GetConflictedKeyCount() == 1);

    // The latch rejects further Admits and Drains for the key.
    const std::vector<std::byte> payloadZ = MakePayload(8, 0x58);
    const pbprotocol::ProtocolStatus latchedAdmit =
        cache.Admit(sessionA, 7, 2, payloadZ);
    REQUIRE_FALSE(latchedAdmit);
    REQUIRE(latchedAdmit.Error().code
        == ProtocolErrorCode::OrphanPayloadConflict);
    const auto latchedDrain = cache.Drain(sessionA, 7);
    REQUIRE_FALSE(latchedDrain);
    REQUIRE(latchedDrain.Error().code
        == ProtocolErrorCode::OrphanPayloadConflict);

    // Other keys are unaffected.
    REQUIRE(cache.Admit(sessionB, 8, 1, payloadX));
    REQUIRE(cache.GetAdmittedBlockCount() == 2);

    // ClearSession releases the latch and the key's memory.
    cache.ClearSession(sessionA);
    REQUIRE(cache.GetConflictedKeyCount() == 0);
    REQUIRE(cache.GetCachedBlockCount() == 1);
    const auto clearedDrain = cache.Drain(sessionA, 7);
    REQUIRE(clearedDrain.HasValue());
    REQUIRE(clearedDrain.Value().entries.empty());
}

TEST_CASE("Orphan wait observations measure first-seen to drain",
          "[pbprotocol][orphan]")
{
    auto cacheResult = OrphanTransportBlockCache::Create(
        MakeOrphanPolicy(4096, 8));
    REQUIRE(cacheResult.HasValue());
    OrphanTransportBlockCache& cache = cacheResult.Value();

    const std::vector<std::byte> payload = MakePayload(12, 0x59);

    // Both ordinals present: waitObservations is the difference.
    REQUIRE(cache.Admit(SessionTag{21}, 1, 0, payload, 5));
    const auto timedDrain = cache.Drain(SessionTag{21}, 1, 9);
    REQUIRE(timedDrain.HasValue());
    REQUIRE(timedDrain.Value().waitObservations.has_value());
    REQUIRE(*timedDrain.Value().waitObservations == 4);

    // A key created without an ordinal never backfills first-seen.
    REQUIRE(cache.Admit(SessionTag{22}, 1, 0, payload));
    REQUIRE(cache.Admit(SessionTag{22}, 1, 1, payload, 5));
    const auto unbackfilledDrain = cache.Drain(SessionTag{22}, 1, 9);
    REQUIRE(unbackfilledDrain.HasValue());
    REQUIRE_FALSE(
        unbackfilledDrain.Value().waitObservations.has_value());

    // A drain ordinal earlier than first-seen fails without wrapping.
    REQUIRE(cache.Admit(SessionTag{23}, 1, 0, payload, 10));
    const auto wrappedDrain = cache.Drain(SessionTag{23}, 1, 7);
    REQUIRE_FALSE(wrappedDrain);
    REQUIRE(wrappedDrain.Error().code
        == ProtocolErrorCode::InvalidObservationOrdinal);
    // The key survives the failed drain.
    const auto validDrain = cache.Drain(SessionTag{23}, 1, 12);
    REQUIRE(validDrain.HasValue());
    REQUIRE(*validDrain.Value().waitObservations == 2);
}

TEST_CASE("Orphan ClearSession releases only the named session",
          "[pbprotocol][orphan]")
{
    auto cacheResult = OrphanTransportBlockCache::Create(
        MakeOrphanPolicy(4096, 8));
    REQUIRE(cacheResult.HasValue());
    OrphanTransportBlockCache& cache = cacheResult.Value();

    const SessionTag sessionA{25};
    const SessionTag sessionB{26};
    const std::vector<std::byte> payloadA1 = MakePayload(10, 0x5A);
    const std::vector<std::byte> payloadA2 = MakePayload(14, 0x5B);
    const std::vector<std::byte> payloadB1 = MakePayload(6, 0x5C);

    REQUIRE(cache.Admit(sessionA, 1, 0, payloadA1));
    REQUIRE(cache.Admit(sessionA, 1, 1, payloadA2));
    REQUIRE(cache.Admit(sessionB, 1, 0, payloadB1));
    REQUIRE(cache.GetCachedBlockCount() == 3);

    cache.ClearSession(sessionA);
    REQUIRE(cache.GetCachedBlockCount() == 1);
    REQUIRE(cache.GetCachedBytes() == payloadB1.size());

    const auto drainA = cache.Drain(sessionA, 1);
    REQUIRE(drainA.HasValue());
    REQUIRE(drainA.Value().entries.empty());
    const auto drainB = cache.Drain(sessionB, 1);
    REQUIRE(drainB.HasValue());
    REQUIRE(drainB.Value().entries.size() == 1);
}

TEST_CASE("Orphan cache creation validates policy and upstream resource",
          "[pbprotocol][orphan]")
{
    const ReceiverResourcePolicy zeroedPolicy{};
    const auto invalidPolicyResult = OrphanTransportBlockCache::Create(
        zeroedPolicy);
    REQUIRE_FALSE(invalidPolicyResult.HasValue());
    REQUIRE(invalidPolicyResult.Error().code
        == ProtocolErrorCode::InvalidResourcePolicy);

    const std::shared_ptr<std::pmr::memory_resource> nullResource{};
    const auto nullResourceResult = OrphanTransportBlockCache::
        CreateWithMemoryResource(MakeOrphanPolicy(4096, 8), nullResource);
    REQUIRE_FALSE(nullResourceResult.HasValue());
    REQUIRE(nullResourceResult.Error().code
        == ProtocolErrorCode::InvalidResourcePolicy);
}

TEST_CASE("Orphan cache allocation failure fails closed without leaking",
          "[pbprotocol][orphan]")
{
    auto faultMemoryResource =
        std::make_shared<FailOnAllocationMemoryResource>();
    faultMemoryResource->FailAfterSuccessfulAllocations(0);
    auto cacheResult = OrphanTransportBlockCache::CreateWithMemoryResource(
        MakeOrphanPolicy(4096, 8),
        faultMemoryResource);
    REQUIRE(cacheResult.HasValue());
    OrphanTransportBlockCache& cache = cacheResult.Value();

    const std::vector<std::byte> payload = MakePayload(16, 0x5D);
    const pbprotocol::ProtocolStatus failedAdmit =
        cache.Admit(SessionTag{29}, 2, 1, payload);
    REQUIRE_FALSE(failedAdmit);
    REQUIRE(failedAdmit.Error().code == ProtocolErrorCode::ResourceExhausted);
    REQUIRE(cache.GetCachedBlockCount() == 0);
    REQUIRE(cache.GetCachedBytes() == 0);
    REQUIRE(cache.GetAdmittedBlockCount() == 0);
    REQUIRE(faultMemoryResource->OutstandingAllocations() == 0);

    // The failed attempt left no empty key behind, so the next Admit records
    // first-seen normally and succeeds.
    REQUIRE(cache.Admit(SessionTag{29}, 2, 1, payload, 3));
    REQUIRE(cache.GetCachedBlockCount() == 1);
    const auto drainResult = cache.Drain(SessionTag{29}, 2, 4);
    REQUIRE(drainResult.HasValue());
    REQUIRE(*drainResult.Value().waitObservations == 1);
}
