#pragma once

#include "pbprotocol/protocol_result.h"
#include "pbprotocol/protocol_types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <vector>

namespace pbprotocol {

namespace detail {

struct OrphanTransportBlockCacheImplementation;

} // namespace detail

// One cached logical transport block. declaredPayloadBytes is the provisional
// Transport PayloadBytes field. paddedPayload is the fixed payload region
// (real payload plus canonical zero padding). Both values must survive orphan
// caching so descriptor-bound replay can validate the exact transport claim.
struct OrphanTransportBlockEntry
{
    std::uint32_t outerBlockId = 0;
    std::uint16_t declaredPayloadBytes = 0;
    std::vector<std::byte> paddedPayload{};

    bool operator==(const OrphanTransportBlockEntry&) const = default;
};

// Drain result for one (SessionTag, SegmentOrdinal) key. Entries are in
// arrival order. waitObservations is the DescriptorWaitTime metric of design
// section 9.5: drain observation ordinal minus first-seen observation
// ordinal, both caller-supplied monotonic sequence numbers (no wall clock).
struct OrphanTransportBlockDrain
{
    std::vector<OrphanTransportBlockEntry> entries{};
    std::optional<std::uint64_t> waitObservations = std::nullopt;

    bool operator==(const OrphanTransportBlockDrain&) const = default;
};

// Bounded cache for Data Blocks whose SegmentDescriptor is not bound yet.
// Invariants (design section 9.5, AGENTS.md resource safety):
//   * A block may only enter this cache or be dropped. It must never trigger
//     Wirehair/codec creation, large buffers, or file preallocation by itself.
//   * The byte budget is an exact ceiling on cached payload bytes: every
//     payload buffer allocates through a PMR resource capped at
//     maxOrphanTransportBytes, and quota checks run before any allocation so
//     an admitted block always fits. Structure overhead (one map node per key,
//     one vector slot per entry) is separately bounded by the block-count
//     policy field.
//   * Quota overflow drops only the incoming block; cached blocks are never
//     evicted. Carousel replay is the recovery path for dropped data.
//   * The same (key, outerBlockId) with different valid payloads latches the
//     key as a terminal conflict: later Admit and Drain calls for that key
//     always fail with OrphanPayloadConflict until ClearSession. Latest-wins
//     is forbidden by protocol invariant.
// Single owner thread; move-only. Telemetry counters are per-component state,
// not global counters: admitted/dropped/resource-exhausted counts are
// cumulative events, while conflicted keys and cached block/byte counts are
// current occupancy.
class OrphanTransportBlockCache
{
public:
    [[nodiscard]] static ProtocolResult<OrphanTransportBlockCache> Create(
        ReceiverResourcePolicy resourcePolicy);

    // The bounded reassembly-style PMR resource remains authoritative over the
    // upstream one, so a custom arena cannot bypass maxOrphanTransportBytes.
    // This overload is also the allocator fault-injection seam used by
    // resource-path tests.
    [[nodiscard]] static ProtocolResult<OrphanTransportBlockCache>
    CreateWithMemoryResource(
        ReceiverResourcePolicy resourcePolicy,
        std::shared_ptr<std::pmr::memory_resource> upstreamMemoryResource);

    OrphanTransportBlockCache(const OrphanTransportBlockCache&) = delete;
    OrphanTransportBlockCache& operator=(const OrphanTransportBlockCache&) =
        delete;
    ~OrphanTransportBlockCache();
    OrphanTransportBlockCache(OrphanTransportBlockCache&&) noexcept;
    OrphanTransportBlockCache& operator=(OrphanTransportBlockCache&&) noexcept;

    // Admits one orphan block. declaredPayloadBytes must be non-zero, no larger
    // than paddedPayload, and every byte after it must be canonical zero
    // padding. Empty/structurally invalid payloads fail closed. Regions above
    // maxOuterBlockBytes or byte/block quota are dropped with
    // ResourceLimitExceeded. A repeated (key, outerBlockId) is idempotent only
    // when both the declared length and padded region are identical; otherwise
    // it latches a terminal conflict. Existing duplicate/conflict recognition
    // runs before quota checks so a full cache cannot hide a protocol conflict.
    // observationOrdinal is the caller's monotonic sequence number recorded as
    // first-seen when the key does not exist yet.
    [[nodiscard]] ProtocolStatus Admit(
        SessionTag sessionTag,
        std::uint64_t segmentOrdinal,
        std::uint32_t outerBlockId,
        std::uint16_t declaredPayloadBytes,
        std::span<const std::byte> paddedPayload,
        std::optional<std::uint64_t> observationOrdinal = std::nullopt);

    // Returns all cached entries of one key in arrival order and clears the
    // key. An unknown key is a normal case and yields an empty success. A
    // conflicted key always fails with OrphanPayloadConflict and keeps its
    // latch until ClearSession. When both first-seen and drain ordinals are
    // present, waitObservations is attached; a drain ordinal earlier than the
    // first-seen ordinal fails with InvalidObservationOrdinal (no wrapped
    // subtraction). Allocation failure while copying out fails closed with
    // ResourceExhausted and leaves the cache unchanged.
    [[nodiscard]] ProtocolResult<OrphanTransportBlockDrain> Drain(
        SessionTag sessionTag,
        std::uint64_t segmentOrdinal,
        std::optional<std::uint64_t> observationOrdinal = std::nullopt);

    // Releases every key of one session during teardown so cached bytes do
    // not outlive the session. Cumulative event counters are preserved;
    // current occupancy counters are adjusted.
    void ClearSession(SessionTag sessionTag) noexcept;

    // Releases every cached/conflicted key while preserving cumulative event
    // counters. Receiver-wide collision/shutdown cleanup uses this when the
    // offending SessionTag cannot be recovered from a fragmented record.
    void ClearAll() noexcept;

    [[nodiscard]] std::uint64_t GetAdmittedBlockCount() const noexcept;
    [[nodiscard]] std::uint64_t GetDroppedBlockCount() const noexcept;
    [[nodiscard]] std::uint64_t GetResourceExhaustedCount() const noexcept;
    [[nodiscard]] std::uint64_t GetConflictedKeyCount() const noexcept;
    [[nodiscard]] bool HasCachedKey(
        SessionTag sessionTag,
        std::uint64_t segmentOrdinal) const noexcept;
    // Current occupancy. Values never exceed the validated policy limits, so
    // they are always representable as size_t on this platform.
    [[nodiscard]] std::size_t GetCachedBlockCount() const noexcept;
    [[nodiscard]] std::size_t GetCachedBytes() const noexcept;

private:
    explicit OrphanTransportBlockCache(
        std::unique_ptr<detail::OrphanTransportBlockCacheImplementation>
            implementation) noexcept;

    std::unique_ptr<detail::OrphanTransportBlockCacheImplementation>
        implementation_;
};

} // namespace pbprotocol
