# resume.state v1 corpus (FastResume basic tier)

The documents are independently authored bytes and are not emitted by the
PixelBridge builder at test runtime. Every seed below is pinned to one exact
`LoadResumeState` outcome under the fuzz policy (`maxResumeBytes=1024`,
`maxOuterBlockBytes=32`; all other limits default). The v1 envelope is
unchanged: magic `PBRS`, version 1, reserved 0, little-endian payloadLength u64,
CRC-32C over `[magic .. last payload byte]` stored as the final four bytes.

The ctest corpus replays (PBProtocolResumeStateCorpus. tests) enforce each pinned outcome above exactly: a seed that drifts into another error class or becomes accidentally valid exits nonzero with CORPUS_REPLAY_EXPECTATION_MISMATCH.

Envelope-relative body layouts (little-endian): completed record tag(1) +
sessionId(16) + ordinal(8) + rawOffset(8) + rawSize(8) + rawDigest(32), 73
bytes total; Wirehair cache tag(1) + ordinal(8) + profile(32) + entryCount(4)
+ entries{outerBlockId(4), payloadLen(4), payload}; DirectRepeat tag(1) +
ordinal(8) + directBlockCount(4) + entryCount(4) +
entries{blockOrdinal(4), realPayloadBytes(4), paddedLen(4), padded}.

- `valid-single-completed.bin` (91 bytes): one CRC-valid completed record with
  session `00..0F`, ordinal 7, rawOffset 4096, rawSize 12345, digest bytes
  `10..31`. Loads to exactly one completed segment and no torn-tail flag.
- `valid-multi-record.bin` (525 bytes): four CRC-valid records covering all
  three record types: two completed segments (ordinals 7 and 30), a Wirehair
  cache for ordinal 31 with entries `(id,len) = (8,32), (17,8), (24,32),
  (31,2), (40,8)` including two short-tail payloads, and a DirectRepeat record
  for ordinal 32 with `directBlockCount=8` and six entries whose padded blocks
  mix zero-filled tails (`real<padded`) and exact-fit blocks
  (`real==padded`, down to one byte). Loads all four records.
- `malformed-bad-magic.bin` (91 bytes): the valid completed record with magic
  byte 2 flipped `B -> X`; header checks precede CRC, so the stale stored
  checksum is never reached. Fails closed with `InvalidMagic` at offset 0.
- `malformed-version.bin` (91 bytes): version byte changed from 1 to 7. Fails
  closed with `InvalidEnumValue` at offset 4 before any body interpretation.
- `malformed-reserved.bin` (91 bytes): reserved byte set to 1. Fails closed
  with `NonZeroReservedByte` at offset 5.
- `malformed-crc-payload.bin` (91 bytes): bit-7 flip of digest byte 28 at
  absolute offset 82 while the stored checksum is left untouched; all header
  fields validate, so parsing reaches the envelope CRC comparison and fails
  closed with `CrcMismatch` at offset 87.
- `malformed-body-trailing.bin` (92 bytes): payloadLength raised from 73 to 74
  with one extra body byte appended; the checksum is recomputed over the whole
  record so the failure comes strictly from the typed-body full-consumption
  check: `TrailingBytes` at offset 87.
- `malformed-unknown-tag.bin` (91 bytes): first body byte set to tag 0xFF with
  a repaired envelope checksum; header and CRC validate, then the unknown-type
  gate rejects it with `InvalidEnumValue` at offset 14 (body base). This is the
  fail-closed proof that new record types cannot be smuggled past old parsers.
- `semantic-cross-type-conflict.bin` (164 bytes): two CRC-valid records claiming
  the same SegmentOrdinal 5, one completed segment and one Wirehair cache. The
  cross-category ordinal collision fails closed with `ResumeRecordConflict` at
  offset 91 (second record start); no partial state is returned.
- `semantic-wirehair-id-conflict.bin` (83 bytes): one CRC-valid Wirehair cache
  whose two entries share outerBlockId 7 but carry different payloads
  (`0A 0B` vs `0C 0D`). The in-record id collision fails closed with
  `ResumeRecordConflict` at offset 14 (body base); identical-payload duplicates
  are deduplicated instead.
- `quota-document-over-budget.bin` (1112 bytes): a single CRC-valid Wirehair
  cache record (ordinal 50, 27 entries of 32-byte payloads plus one 1-byte tail)
  whose full size exceeds the 1024-byte fuzz budget. The document-level gate
  rejects it before any byte is interpreted: `ResourceLimitExceeded` at offset
  0 with no parse-class error code reachable.
- `torn-mid-envelope.bin` (80 bytes): the first 80 of the valid completed
  record's 91 bytes, cut inside its body so the declared length runs past EOF.
  Loads as a torn tail: success with an empty state and `hasTruncatedTail=1`.
- `torn-short-header.bin` (17 bytes): shorter than one 18-byte envelope. Same
  torn-tail semantics: success, empty state, flag set.

The corpus documents the FastResume basic tier only: no crash-safe flush
ordering, no .part commit-order proof, and no whole-file digest rescan are
claimed by these seeds (design doc section 31.5 remains out of scope).
