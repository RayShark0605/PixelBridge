#include "descriptor_test_helpers.h"

#include "pbprotocol/orphan_transport_block_cache.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <optional>
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

[[nodiscard]] pbprotocol::ProtocolStatus AdmitFullPayload(
    OrphanTransportBlockCache& cache,
    const SessionTag sessionTag,
    const std::uint64_t segmentOrdinal,
    const std::uint32_t outerBlockId,
    const std::span<const std::byte> paddedPayload,
    const std::optional<std::uint64_t> observationOrdinal = std::nullopt)
{
    const auto declaredPayloadBytes = static_cast<std::uint16_t>(
        paddedPayload.size());
    return cache.Admit(
        sessionTag,
        segmentOrdinal,
        outerBlockId,
        declaredPayloadBytes,
        paddedPayload,
        observationOrdinal);
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
    REQUIRE(cache.GetResourceExhaustedCount() == 0);
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

    REQUIRE(AdmitFullPayload(cache, sessionTag, 3, 5, payloadA));
    REQUIRE(AdmitFullPayload(cache, sessionTag, 3, 1, payloadB));
    REQUIRE(AdmitFullPayload(cache, sessionTag, 3, 3, payloadC));
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
    REQUIRE(entries[0].declaredPayloadBytes == payloadA.size());
    REQUIRE(entries[0].paddedPayload == payloadA);
    REQUIRE(entries[1].outerBlockId == 1);
    REQUIRE(entries[1].declaredPayloadBytes == payloadB.size());
    REQUIRE(entries[1].paddedPayload == payloadB);
    REQUIRE(entries[2].outerBlockId == 3);
    REQUIRE(entries[2].declaredPayloadBytes == payloadC.size());
    REQUIRE(entries[2].paddedPayload == payloadC);

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
        REQUIRE(AdmitFullPayload(cache, sessionTag, 1, blockId, payload));
    }
    REQUIRE(cache.GetCachedBytes() == 120);

    const pbprotocol::ProtocolStatus droppedStatus =
        AdmitFullPayload(cache, sessionTag, 1, 99, payload);
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
    REQUIRE(AdmitFullPayload(cache, sessionTag, 2, 0, payload));
    REQUIRE(AdmitFullPayload(cache, sessionTag, 2, 1, payload));
    REQUIRE(cache.GetCachedBytes() == 80);

    const pbprotocol::ProtocolStatus droppedStatus =
        AdmitFullPayload(cache, sessionTag, 2, 2, payload);
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
    REQUIRE(AdmitFullPayload(cache, sessionTag, 4, 0, payload));
    REQUIRE(AdmitFullPayload(cache, sessionTag, 4, 1, payload));

    const pbprotocol::ProtocolStatus droppedStatus =
        AdmitFullPayload(cache, sessionTag, 4, 2, payload);
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
        AdmitFullPayload(cache, sessionTag, 5, 0, emptyPayload);
    REQUIRE_FALSE(emptyStatus);
    REQUIRE(emptyStatus.Error().code == ProtocolErrorCode::InvalidRecordSize);

    const std::vector<std::byte> oversizedPayload = MakePayload(9, 0x53);
    const pbprotocol::ProtocolStatus oversizedStatus = AdmitFullPayload(cache,
        sessionTag, 5, 1, std::span<const std::byte>(oversizedPayload));
    REQUIRE_FALSE(oversizedStatus);
    REQUIRE(oversizedStatus.Error().code
        == ProtocolErrorCode::ResourceLimitExceeded);
    REQUIRE(cache.GetDroppedBlockCount() == 1);

    const std::vector<std::byte> exactPayload = MakePayload(8, 0x54);
    REQUIRE(AdmitFullPayload(
        cache,
        sessionTag,
        5,
        2,
        std::span<const std::byte>(exactPayload)));
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
    REQUIRE(AdmitFullPayload(cache, sessionTag, 6, 3, payload));
    REQUIRE(AdmitFullPayload(cache, sessionTag, 6, 3, payload));

    REQUIRE(cache.GetAdmittedBlockCount() == 1);
    REQUIRE(cache.GetCachedBlockCount() == 1);
    const auto drainResult = cache.Drain(sessionTag, 6);
    REQUIRE(drainResult.HasValue());
    REQUIRE(drainResult.Value().entries.size() == 1);
}

TEST_CASE("Full orphan quotas cannot hide duplicate or conflict semantics",
          "[pbprotocol][orphan][quota][conflict]")
{
    const auto exerciseFullCache = [](
        const ReceiverResourcePolicy& resourcePolicy)
    {
        auto identicalCacheResult = OrphanTransportBlockCache::Create(
            resourcePolicy);
        REQUIRE(identicalCacheResult.HasValue());
        OrphanTransportBlockCache& identicalCache =
            identicalCacheResult.Value();

        const SessionTag sessionTag{16};
        const std::vector<std::byte> originalPayload =
            MakePayload(16, 0x60);
        REQUIRE(AdmitFullPayload(
            identicalCache,
            sessionTag,
            6,
            3,
            originalPayload));
        REQUIRE(AdmitFullPayload(
            identicalCache,
            sessionTag,
            6,
            3,
            originalPayload));
        REQUIRE(identicalCache.GetAdmittedBlockCount() == 1);
        REQUIRE(identicalCache.GetDroppedBlockCount() == 0);
        REQUIRE(identicalCache.GetConflictedKeyCount() == 0);
        REQUIRE(identicalCache.GetCachedBlockCount() == 1);

        auto conflictCacheResult = OrphanTransportBlockCache::Create(
            resourcePolicy);
        REQUIRE(conflictCacheResult.HasValue());
        OrphanTransportBlockCache& conflictCache = conflictCacheResult.Value();
        const std::vector<std::byte> conflictingPayload =
            MakePayload(16, 0x61);
        REQUIRE(AdmitFullPayload(
            conflictCache,
            sessionTag,
            6,
            3,
            originalPayload));
        const pbprotocol::ProtocolStatus conflictStatus = AdmitFullPayload(
            conflictCache,
            sessionTag,
            6,
            3,
            conflictingPayload);
        REQUIRE_FALSE(conflictStatus);
        REQUIRE(conflictStatus.Error().code ==
            ProtocolErrorCode::OrphanPayloadConflict);
        REQUIRE(conflictCache.GetDroppedBlockCount() == 0);
        REQUIRE(conflictCache.GetConflictedKeyCount() == 1);
        const auto drainResult = conflictCache.Drain(sessionTag, 6);
        REQUIRE_FALSE(drainResult);
        REQUIRE(drainResult.Error().code ==
            ProtocolErrorCode::OrphanPayloadConflict);
    };

    SECTION("block-count quota is full")
    {
        exerciseFullCache(MakeOrphanPolicy(4096, 1));
    }

    SECTION("byte quota is full")
    {
        exerciseFullCache(MakeOrphanPolicy(16, 8));
    }
}

TEST_CASE("Orphan blocks preserve declared payload length and canonical padding",
          "[pbprotocol][orphan][payload-length]")
{
    auto cacheResult = OrphanTransportBlockCache::Create(
        MakeOrphanPolicy(4096, 8));
    REQUIRE(cacheResult.HasValue());
    OrphanTransportBlockCache& cache = cacheResult.Value();

    const SessionTag sessionTag{17};
    std::vector<std::byte> paddedPayload = MakePayload(4, 0x62);
    paddedPayload.resize(8, std::byte{0});
    REQUIRE(cache.Admit(sessionTag, 7, 1, 4, paddedPayload));
    REQUIRE(cache.Admit(sessionTag, 7, 1, 4, paddedPayload));

    const auto drainResult = cache.Drain(sessionTag, 7);
    REQUIRE(drainResult.HasValue());
    REQUIRE(drainResult.Value().entries.size() == 1);
    REQUIRE(drainResult.Value().entries[0].declaredPayloadBytes == 4);
    REQUIRE(drainResult.Value().entries[0].paddedPayload == paddedPayload);

    auto conflictCacheResult = OrphanTransportBlockCache::Create(
        MakeOrphanPolicy(4096, 8));
    REQUIRE(conflictCacheResult.HasValue());
    OrphanTransportBlockCache& conflictCache = conflictCacheResult.Value();
    REQUIRE(conflictCache.Admit(sessionTag, 8, 1, 4, paddedPayload));
    const pbprotocol::ProtocolStatus lengthConflict = conflictCache.Admit(
        sessionTag,
        8,
        1,
        5,
        paddedPayload);
    REQUIRE_FALSE(lengthConflict);
    REQUIRE(lengthConflict.Error().code ==
        ProtocolErrorCode::OrphanPayloadConflict);

    std::vector<std::byte> nonCanonicalPayload = paddedPayload;
    nonCanonicalPayload[4] = std::byte{1};
    const pbprotocol::ProtocolStatus paddingStatus = cache.Admit(
        sessionTag,
        9,
        1,
        4,
        nonCanonicalPayload);
    REQUIRE_FALSE(paddingStatus);
    REQUIRE(paddingStatus.Error().code ==
        ProtocolErrorCode::NonCanonicalPadding);
    REQUIRE(paddingStatus.Error().offset == 4);

    const pbprotocol::ProtocolStatus zeroLengthStatus = cache.Admit(
        sessionTag,
        10,
        1,
        0,
        paddedPayload);
    REQUIRE_FALSE(zeroLengthStatus);
    REQUIRE(zeroLengthStatus.Error().code ==
        ProtocolErrorCode::InvalidRecordSize);

    const pbprotocol::ProtocolStatus oversizedLengthStatus = cache.Admit(
        sessionTag,
        10,
        1,
        9,
        paddedPayload);
    REQUIRE_FALSE(oversizedLengthStatus);
    REQUIRE(oversizedLengthStatus.Error().code ==
        ProtocolErrorCode::InvalidRecordSize);
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

    REQUIRE(AdmitFullPayload(cache, sessionA, 7, 1, payloadX));
    const pbprotocol::ProtocolStatus conflictStatus =
        AdmitFullPayload(cache, sessionA, 7, 1, payloadY);
    REQUIRE_FALSE(conflictStatus);
    REQUIRE(conflictStatus.Error().code
        == ProtocolErrorCode::OrphanPayloadConflict);
    REQUIRE(cache.GetConflictedKeyCount() == 1);

    // The latch rejects further Admits and Drains for the key.
    const std::vector<std::byte> payloadZ = MakePayload(8, 0x58);
    const pbprotocol::ProtocolStatus latchedAdmit =
        AdmitFullPayload(cache, sessionA, 7, 2, payloadZ);
    REQUIRE_FALSE(latchedAdmit);
    REQUIRE(latchedAdmit.Error().code
        == ProtocolErrorCode::OrphanPayloadConflict);
    const auto latchedDrain = cache.Drain(sessionA, 7);
    REQUIRE_FALSE(latchedDrain);
    REQUIRE(latchedDrain.Error().code
        == ProtocolErrorCode::OrphanPayloadConflict);

    // Other keys are unaffected.
    REQUIRE(AdmitFullPayload(cache, sessionB, 8, 1, payloadX));
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
    REQUIRE(AdmitFullPayload(cache, SessionTag{21}, 1, 0, payload, 5));
    const auto timedDrain = cache.Drain(SessionTag{21}, 1, 9);
    REQUIRE(timedDrain.HasValue());
    REQUIRE(timedDrain.Value().waitObservations.has_value());
    REQUIRE(*timedDrain.Value().waitObservations == 4);

    // A key created without an ordinal never backfills first-seen.
    REQUIRE(AdmitFullPayload(cache, SessionTag{22}, 1, 0, payload));
    REQUIRE(AdmitFullPayload(cache, SessionTag{22}, 1, 1, payload, 5));
    const auto unbackfilledDrain = cache.Drain(SessionTag{22}, 1, 9);
    REQUIRE(unbackfilledDrain.HasValue());
    REQUIRE_FALSE(
        unbackfilledDrain.Value().waitObservations.has_value());

    // A drain ordinal earlier than first-seen fails without wrapping.
    REQUIRE(AdmitFullPayload(cache, SessionTag{23}, 1, 0, payload, 10));
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

    REQUIRE(AdmitFullPayload(cache, sessionA, 1, 0, payloadA1));
    REQUIRE(AdmitFullPayload(cache, sessionA, 1, 1, payloadA2));
    REQUIRE(AdmitFullPayload(cache, sessionB, 1, 0, payloadB1));
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

TEST_CASE("Orphan ClearAll releases occupancy but preserves event telemetry",
          "[pbprotocol][orphan][cleanup]")
{
    auto cacheResult = OrphanTransportBlockCache::Create(
        MakeOrphanPolicy(4096, 8));
    REQUIRE(cacheResult.HasValue());
    OrphanTransportBlockCache& cache = cacheResult.Value();

    const std::vector<std::byte> payload = MakePayload(16, 0x5D);
    std::vector<std::byte> conflict = payload;
    conflict[0] ^= std::byte{1};
    REQUIRE(AdmitFullPayload(cache, SessionTag{27}, 1, 0, payload));
    REQUIRE(AdmitFullPayload(cache, SessionTag{28}, 1, 0, payload));
    REQUIRE_FALSE(AdmitFullPayload(
        cache,
        SessionTag{27},
        1,
        0,
        conflict));
    REQUIRE(cache.GetCachedBlockCount() == 2);
    REQUIRE(cache.GetConflictedKeyCount() == 1);

    cache.ClearAll();
    REQUIRE(cache.GetCachedBlockCount() == 0);
    REQUIRE(cache.GetCachedBytes() == 0);
    REQUIRE(cache.GetConflictedKeyCount() == 0);
    REQUIRE(cache.GetAdmittedBlockCount() == 2);
    REQUIRE(cache.GetDroppedBlockCount() == 0);
    REQUIRE(cache.GetResourceExhaustedCount() == 0);
    REQUIRE(cache.Drain(SessionTag{27}, 1));
    REQUIRE(AdmitFullPayload(cache, SessionTag{27}, 1, 0, payload));
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
        AdmitFullPayload(cache, SessionTag{29}, 2, 1, payload);
    REQUIRE_FALSE(failedAdmit);
    REQUIRE(failedAdmit.Error().code == ProtocolErrorCode::ResourceExhausted);
    REQUIRE(cache.GetCachedBlockCount() == 0);
    REQUIRE(cache.GetCachedBytes() == 0);
    REQUIRE(cache.GetAdmittedBlockCount() == 0);
    REQUIRE(cache.GetResourceExhaustedCount() == 1);
    REQUIRE(faultMemoryResource->OutstandingAllocations() == 0);

    // The failed attempt left no empty key behind, so the next Admit records
    // first-seen normally and succeeds.
    REQUIRE(AdmitFullPayload(cache, SessionTag{29}, 2, 1, payload, 3));
    REQUIRE(cache.GetCachedBlockCount() == 1);
    const auto drainResult = cache.Drain(SessionTag{29}, 2, 4);
    REQUIRE(drainResult.HasValue());
    REQUIRE(*drainResult.Value().waitObservations == 1);
}
