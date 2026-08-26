#pragma once

#include "pbprotocol/protocol_result.h"
#include "pbprotocol/protocol_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace pbprotocol {

// Receiver-local persistent envelope for resume.state records. This format is
// deliberately NOT part of the wire protocol: it only protects state that a
// receiver writes and later re-reads, so its evolution stays independent from
// bootstrap/control/data plane versions. Layout (little-endian):
//   magic 'PBRS' (4 bytes) | version u8 (=1) | reserved u8 (=0) |
//   payloadLength u64 | payload | CRC32C u32 over [magic .. payload]
constexpr std::size_t kResumeRecordEnvelopeBytes = 18;

// A resume.state document is a sequence of envelope records whose payloads are
// typed bodies starting with one ResumeRecordType tag byte. Unknown tags are
// rejected fail-closed so older readers can never misinterpret future record
// types as valid state (design doc section 31). Format evolution rules: the
// v1 envelope stays untouched; a new field or record type must remain
// rejectable by existing parsers through their unknown-tag / non-zero-reserved
// gates.

enum class ResumeRecordType : std::uint8_t
{
    CompletedSegment = 1,
    ActiveWirehairCache = 2,
    DirectRepeatReceivedBlocks = 3
};

// Design doc section 31.1: metadata of a Segment whose raw bytes are already
// in output.part. The RawDigest is integrity metadata only; when crash-safe
// flush ordering has not been established the caller may recompute digests
// from .part instead of blindly trusting these records (section 31.5).
struct ResumeCompletedSegmentRecord
{
    SessionId sessionId{};
    std::uint64_t segmentOrdinal = 0;
    std::uint64_t rawOffset = 0;
    std::uint64_t rawSize = 0;
    RawDigest rawDigest{};

    bool operator==(const ResumeCompletedSegmentRecord&) const = default;
};

// One validated outer block captured for the active Wirehair segment. payload
// is exactly the byte span handed to WirehairV2Decoder::DecodeBlock (the final
// systematic block may be shorter than OuterBlockBytes). Replay re-injects it
// into a freshly created decoder, so no codec-private structure persists.
struct ResumeWirehairCacheEntry
{
    std::uint32_t outerBlockId = 0;
    std::vector<std::byte> payload;

    bool operator==(const ResumeWirehairCacheEntry&) const = default;
};

// Design doc section 31.2: the replayable cache of one active Wirehair
// segment. wirehairProfile is the canonical serialized profile snapshot, not a
// codec handle. Duplicate outerBlockId entries with identical payload bytes are
// deduplicated on load; conflicting payloads fail the whole document.
struct ResumeActiveWirehairCacheRecord
{
    std::uint64_t segmentOrdinal = 0;
    WirehairV2SerializedProfile wirehairProfile{};
    std::vector<ResumeWirehairCacheEntry> entries;

    bool operator==(const ResumeActiveWirehairCacheRecord&) const = default;
};

// Design doc section 31.3: one validated DirectRepeat block. paddedPayload is
// the canonical zero-padded OuterBlockBytes region and realPayloadBytes is the
// exact payload-length argument that replay must re-pass to
// DirectRepeatDecoder::DecodeBlock, which revalidates it against its own
// descriptor-derived expectation before accepting the entry.
struct ResumeDirectRepeatEntry
{
    std::uint32_t blockOrdinal = 0;
    std::uint32_t realPayloadBytes = 0;
    std::vector<std::byte> paddedPayload;

    bool operator==(const ResumeDirectRepeatEntry&) const = default;
};

// The replayable received-block set of one active DirectRepeat segment.
// directBlockCount is the descriptor-derived total block count of the segment,
// not the number of received entries (entries stay a bounded subset).
struct ResumeActiveDirectRepeatRecord
{
    std::uint64_t segmentOrdinal = 0;
    std::uint32_t directBlockCount = 0;
    std::vector<ResumeDirectRepeatEntry> entries;

    bool operator==(const ResumeActiveDirectRepeatRecord&) const = default;
};

// In-memory resume.state document after validation. Records are deduplicated:
// a byte-identical duplicate record is kept once, and any same-key
// different-content conflict fails the whole load with ResumeRecordConflict.
// hasTruncatedTail reports that the final region of the input could not hold
// one complete structurally-valid envelope (fewer than 18 bytes remaining, a
// declared total record length that overflows, or a declared length running
// past EOF) and was dropped as a torn tail write; the
// validated prefix is still usable, but callers needing crash-safe guarantees
// must reverify completed segments per design doc section 31.5. A record whose
// envelope is fully present but malformed fails the entire load instead.
struct LoadedResumeState
{
    std::vector<ResumeCompletedSegmentRecord> completedSegments;
    std::vector<ResumeActiveWirehairCacheRecord> activeWirehairCaches;
    std::vector<ResumeActiveDirectRepeatRecord> activeDirectRepeatRecords;
    bool hasTruncatedTail = false;

    bool operator==(const LoadedResumeState&) const = default;
};

// Serializes one resume record into output. Requires at least
// kResumeRecordEnvelopeBytes + payload.size() bytes of capacity; extra
// trailing capacity is left untouched. The CRC covers the header and payload,
// never itself. An empty payload produces a structurally complete 18-byte
// envelope; a document record still needs its record-type tag byte, so
// LoadResumeState rejects such a record with InvalidRecordSize.
[[nodiscard]] ProtocolStatus SerializeResumeRecord(
    std::span<const std::byte> payload,
    std::span<std::byte> output);

// Parses and validates one resume record against the receiver resource policy.
// Resume state is untrusted persistent input: full policy validation runs
// first (InvalidResourcePolicy), then magic/version/reserved/length/CRC checks
// fail closed with their specific codes. Both the supplied input length and
// the declared total record length must fit maxResumeBytes
// (ResourceLimitExceeded). The returned vector owns a copy of the payload;
// allocation failure fails closed with ResourceExhausted.
[[nodiscard]] ProtocolResult<std::vector<std::byte>> ParseResumeRecord(
    std::span<const std::byte> input,
    const ReceiverResourcePolicy& resourcePolicy);

// Whole-file quota gate to run before loading or extending resume.state on
// disk. totalBytes is the full record size (envelope plus payload). Zero is
// allowed; anything above maxResumeBytes fails with ResourceLimitExceeded.
[[nodiscard]] ProtocolStatus ValidateResumeStateBudget(
    std::uint64_t totalBytes,
    const ReceiverResourcePolicy& resourcePolicy) noexcept;

// Parses a complete resume.state document: the receiver resource policy gate
// runs first, then the whole size is bounded by maxResumeBytes before any byte
// is interpreted. Records are parsed sequentially with ParseResumeRecord and
// their typed bodies must be fully consumed. A trailing region that cannot
// hold one complete structurally-valid envelope is dropped as a torn tail and
// reported via LoadedResumeState::hasTruncatedTail; any malformed record whose
// envelope is fully present (bad magic/version/reserved/CRC, unknown tag, body
// mismatch, field bounds) fails the entire load fail-closed. Error offsets are
// relative to the start of the document span. Accepted per-segment records (one
// record per distinct segment ordinal across all categories) are bounded by
// maxSegmentCount: a further distinct record fails with ResourceLimitExceeded at
// that record start, while identical duplicates never consume quota (design doc
// section 31.4).
[[nodiscard]] ProtocolResult<LoadedResumeState> LoadResumeState(
    std::span<const std::byte> document,
    const ReceiverResourcePolicy& resourcePolicy);

// In-memory resume.state document builder with the same structural validation
// as LoadResumeState plus a strict-writer key rule: appending any record whose
// key (SessionId + SegmentOrdinal for completed records, SegmentOrdinal per
// active cache type) was already appended is rejected, even when the content
// is identical. Every append that fails validation or budget leaves the
// accumulated document untouched; appends are bounded by maxResumeBytes with
// checked arithmetic before any allocation. The accumulated per-segment records are
// additionally bounded by maxSegmentCount across all categories, mirroring
// LoadResumeState (design doc section 31.4). Per-block incremental persistence
// and crash-safe flush ordering are out of scope for this class (design doc
// section 31.5). A single instance has one owner and must not be called
// concurrently.
class ResumeStateBuilder
{
public:
    [[nodiscard]] static ProtocolResult<ResumeStateBuilder> Create(
        const ReceiverResourcePolicy& resourcePolicy);

    ResumeStateBuilder(const ResumeStateBuilder&) = delete;
    ResumeStateBuilder& operator=(const ResumeStateBuilder&) = delete;
    ResumeStateBuilder(ResumeStateBuilder&&) noexcept;
    ResumeStateBuilder& operator=(ResumeStateBuilder&& other) noexcept;

    [[nodiscard]] ProtocolStatus AppendCompletedSegment(
        const ResumeCompletedSegmentRecord& record);
    [[nodiscard]] ProtocolStatus AppendActiveWirehairCache(
        const ResumeActiveWirehairCacheRecord& record);
    [[nodiscard]] ProtocolStatus AppendDirectRepeatReceivedBlocks(
        const ResumeActiveDirectRepeatRecord& record);

    // The accumulated document bytes. The returned span is invalidated by any
    // subsequent append call: a failed append may resize (and reallocate) the
    // document before rolling back.
    std::span<const std::byte> GetDocument() const noexcept;
    std::size_t GetByteCount() const noexcept;

private:
    explicit ResumeStateBuilder(
        const ReceiverResourcePolicy& resourcePolicy) noexcept;

    [[nodiscard]] ProtocolStatus AppendRecordBytes(
        std::span<const std::byte> bodyBytes);

    // True when the ordinal is claimed by any already-appended record of a
    // different category (completed vs either active cache type).
    bool IsOrdinalClaimedByOtherCategory(std::uint64_t segmentOrdinal) const;

    ReceiverResourcePolicy resourcePolicy_;
    std::vector<std::byte> document_;
    // Strict-writer key sets. Duplicate appends are rejected before any byte
    // or allocation changes, so the appended record's content is never kept.
    std::vector<std::pair<std::array<std::byte, kSessionIdBytes>,
                          std::uint64_t>> completedKeys_;
    std::vector<std::uint64_t> wirehairCacheOrdinals_;
    std::vector<std::uint64_t> directRepeatOrdinals_;
};

} // namespace pbprotocol