#pragma once

#include "pbreceiver/receiver_result.h"

#include "pbouterfec/direct_repeat.h"
#include "pbouterfec/wirehair_v2.h"
#include "pbprotocol/resume_state.h"

#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>

namespace pbreceiver {

// Result of re-injecting cached validated blocks into a freshly created
// decoder. replayedEntryCount is the number of entries successfully fed to
// DecodeBlock before the result; decoderReady reports whether any entry's
// disposition was Ready (sticky once reached).
struct ResumeReplayOutcome
{
    std::size_t replayedEntryCount = 0;
    bool decoderReady = false;

    bool operator==(const ResumeReplayOutcome&) const = default;
};

// Design doc section 31.2 restart path: create the WirehairV2Decoder from the
// bound SegmentDescriptor plus the session's expected OuterBlockBytes (the
// receiver already knows both at restart), then feed this cache record into it.
// A replay against an empty (moved-from) decoder fails with InvalidState before
// any metadata comparison, because its zero-filled sentinel profile must never
// be treated as a match for stale state. Otherwise the persisted profile
// snapshot is untrusted input and is cross-checked against the decoder's
// descriptor-bound profile before the first block: a mismatch fails closed with
// UnsupportedProfile without consuming the decoder. Encoded-digest verification
// is performed by decoder Recover, not here; replay re-injects exactly the
// stored payload spans in stored order. A failing entry stops the replay and
// surfaces its exact OuterFecError unchanged; the decoder's own terminal latch
// stays authoritative for all subsequent operations. A single call is owned by
// one thread.
[[nodiscard]] ReceiverResult<ResumeReplayOutcome> ReplayActiveWirehairCache(
    const pbprotocol::ResumeActiveWirehairCacheRecord& cacheRecord,
    pbouterfec::WirehairV2Decoder& decoder);

// Design doc section 31.3 restart path for the DirectRepeat mode: a replay
// against an empty (moved-from) decoder fails with InvalidState before any
// metadata comparison. Otherwise the persisted block count is cross-checked
// against the decoder's descriptor-derived bound count (mismatch fails closed
// with InvalidInput, detail = persisted count), then each stored
// (blockOrdinal, realPayloadBytes, padded region) entry is re-injected in
// stored order. The decoder revalidates realPayloadBytes against its own
// descriptor-derived expectation and canonical zero padding before accepting
// an entry, so corrupt state content fails closed during replay instead of at
// recovery time. Error propagation matches ReplayActiveWirehairCache.
[[nodiscard]] ReceiverResult<ResumeReplayOutcome> ReplayDirectRepeatBlocks(
    const pbprotocol::ResumeActiveDirectRepeatRecord& directRepeatRecord,
    pbouterfec::DirectRepeatDecoder& decoder);

// Bounded resume.state file IO (FastResume phase: plain read/write; no
// FlushFileBuffers or atomic replace/rename ordering - design doc section 31.5
// crash-safe semantics layer on top of this and are intentionally out of scope
// here). ReadResumeStateFile stats the size first and fails with
// ResourceLimitExceeded before any allocation or read when the file exceeds
// maxResumeBytes; it then reads exactly that many bytes in binary mode (short
// read -> TruncatedInput, non-regular file or open failure -> ResumeStateIoFailure).
[[nodiscard]] ReceiverResult<std::vector<std::byte>> ReadResumeStateFile(
    const std::filesystem::path& filePath,
    const pbprotocol::ReceiverResourcePolicy& resourcePolicy);

// WriteResumeStateFile performs a plain binary write of document bytes that the
// caller already validated through ResumeStateBuilder/LoadResumeState and
// returns the byte count written. Success is reported only after the data is
// flushed and the stream is closed, so a torn write never masquerades as
// persisted state. Open/flush/close failure -> ResumeStateIoFailure.
[[nodiscard]] ReceiverResult<std::size_t> WriteResumeStateFile(
    const std::filesystem::path& filePath,
    std::span<const std::byte> documentBytes);

} // namespace pbreceiver