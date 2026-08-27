# PixelBridge Phase-0 protocol implementation status

## Status decision

The descriptor codec currently implemented by `PBProtocol` is a **Phase-0
provisional implementation slice**. `ProtocolVersion{1, 0}` identifies the
current protocol baseline used by the prototype, but the exact descriptor
payloads below are not yet the complete formal v1 compatibility contract:

| Payload | Current exact size | Status |
| --- | ---: | --- |
| `SessionDescriptor` | 37 bytes | Provisional |
| `SegmentDescriptor` / DirectRepeat | 110 bytes | Provisional |
| `SegmentDescriptor` / Wirehair V2 | 142 bytes | Provisional PixelBridge wrapper; its embedded 32-byte Wirehair V2 profile remains canonical |
| `FinalManifest` | 65 bytes | Provisional |
| `TransportBlock` | 32-byte header + payload + 4-byte payload CRC | Provisional tooling/reference layout; exact total is `36 + PayloadBytes` |

The exact-byte tests are implementation regression vectors. They prevent
accidental changes inside this Phase-0 slice, but they must not be cited as
proof that the architecture-required v1 schema is complete.

## Fields intentionally not frozen yet

The current `SessionDescriptor` does not bind `SessionVisualProfileId`,
`FeatureFlags`, `FileNameUtf8`, `SourceSegmentTargetBytes`, or
`CompressionProfile`. The current `SegmentDescriptor` also omits its final
flags. Descriptor payload integrity is now provided when those opaque bytes are
carried by the fixed PB-Control-1 record envelope, but that does not fill in the
missing descriptor fields or promote these payloads to a formal v1 schema.

`SessionVisualProfileId` is a blocking field for formal wire freeze: the
receiver cannot validate `OuterBlockBytes` against the fixed Session visual
payload capacity until that binding and the machine-readable Profile Manifest
exist. CPU, D3D11, and CUDA data-plane backends must not independently infer or
silently choose this relationship.

Before a formal v1 descriptor Golden Vector is declared, the project must do
one of the following explicitly:

1. assign and serialize all mandatory Session/profile bindings before the v1
   layout is published; or
2. migrate the provisional layout through an explicit protocol version change
   if any current bytes have already become externally consumed.

Silent extension of the existing `1.0` payload bytes is not allowed.

## Provisional Transport/interleave tooling status

`PBProtocol` now exposes a strict Phase-0 Transport codec for tools, Golden and
fuzz verification. The fixed implementation slice is explicit little-endian:
type/minor/flags, SessionTag, SegmentOrdinal, OuterBlockId, uint16 PayloadBytes,
reserved, header CRC-32C over `[0,28)`, payload, and a separate payload CRC-32C.
The parser validates header presence and semantic fields, then header CRC before
trusting declared length, then exact checked total length and payload CRC.
Inner-FEC extraction additionally requires all unused information bytes to be
canonical zero padding. This codec is not wired into production
`ReceiverIngress::ReceivedTransportBlock` admission and is not a formal v1
freeze.

The Qt-free `PBInterleave` reference fixes 112,336 four-bit tiles, 16 phases,
forward mapping `(logical*65537 + phase*472) mod 112336` and inverse multiplier
`108673`. It is a local bit-exact candidate only. There is no serialized
`InterleaveProfileId` binding, so complete PB-ReferenceRaster-1 frames remain
non-interleaved and `PBFrameInspector` reports `interleave=not-bound` rather
than guessing from content.

The implementation slice is covered by `PBProtocolDump`, `PBFrameInspector`,
25 file-backed Golden vectors, five full-frame digest/oracle pins,
`PBVectorGen`, focused Transport/Interleave/LDPC parser drivers and the
`PBParserFuzzHarness` aggregate target. These artifacts prevent accidental
Phase-0 drift; they do not fill the missing formal Session/Visual/Interleave
profile fields described above.

## PB-Bootstrap-1 / PB-Control-1 byte protocol status: GO

The logical byte records are implemented in `PBProtocol` independently from
any Data Visual Profile or raster backend:

- PB-Bootstrap-1 is exactly 44 bytes (`PBRG`, compact v1/version fields,
  layout/profile/session/frame metadata, zero-only v1 flags, CRC-32C);
- PB-Control-1 uses a 26-byte `PBCR` prefix, opaque payload, and 4-byte CRC-32C;
- `RecordBytes` includes the complete Control record and is capped at 65,536,
  leaving at most 65,506 payload bytes;
- Control types are `1=SessionDescriptor`, `2=SegmentDescriptor`, and
  `3=FinalManifest`; other v1 values fail closed;
- PB-Control-Fragment-1 is the frozen `20-byte prefix + non-empty payload +
  4-byte CRC-32C` logical fragmentation envelope. It carries a bounded
  `ControlRecordId`, index/count, complete-record length, exact fragment length,
  and zero-only v1 flags.

`ParseControlRecord()` remains a zero-copy envelope parser. Its returned payload
view borrows the input, and successful envelope parsing alone is not descriptor
admission. Production ingress uses `ControlPlaneReceiver`, which owns the
Session registry and bounded reassembly state and enforces one typed path:
envelope parse, record-type dispatch, receiver policy, descriptor parse,
envelope/payload SessionTag cross-check, then immutable binding. It never guesses
a payload type or silently falls back to another parser.

Fragment reassembly supports out-of-order delivery, exact-repeat idempotence,
terminal metadata/payload conflict tombstones, exact-length reconstruction,
pending retry after `UnknownSession` or temporary capacity failure, once-only
successful submission per RecordId, explicit ControlEpoch reset, and a caller-
supplied monotonic observation window. All fragment containers, payloads, and
temporary complete-record storage share one bounded PMR budget. The fixed three-
fragment Golden Vector has payload sizes `24/24/19` and CRC-32C values
`0x9DCD5402`, `0xF3FF94D8`, and `0x40106F66`.

Independent Golden/corpus bytes, exact boundary/state tests, CRC-repairing
structured mutation modes, deterministic semantic self-tests, and the raw
libFuzzer-compatible entry point cover Bootstrap, complete Control, fragments,
and authoritative admission. The structured fragment gate explicitly reaches
`FragmentCount=65535`, `TotalRecordBytes=65536`, `UINT32_MAX`, and a sparse
maximum index without allocating a dense slot table. This closes the logical
PB-Bootstrap-1 / PB-Control-1 byte-protocol step. Control/Bootstrap FEC, visual
mapping, physical Control Block capacity, repetition cadence, and formal
profile-registry acceptance remain separate work.

## Structural validation versus receiver policy

Descriptor structure and receiver-local policy are separate contracts:

- serializer/producer code may use the overloads without a
  `ReceiverResourcePolicy`; these validate structural and deterministic wire
  rules only;
- parser, Session admission, and receiver binding code must use the overloads
  with a validated local policy;
- receiver policy values never enter serialized bytes and are not protocol
  constants.

`GetDefaultReceiverResourcePolicy()` is a finite conservative Phase-0 local
baseline, not a certified performance profile. Its current budgets are:

| Budget | Default |
| --- | ---: |
| accepted file bytes | 500 GiB |
| segments per Session | 65,536 |
| raw bytes per Segment | 16 MiB |
| encoded bytes per Segment | 32 MiB |
| outer block bytes | 65,535 |
| dynamic descriptor state per Session | 64 MiB |
| active Sessions plus ambiguous SessionTag tombstones | 4 |
| aggregate reserved descriptor state | 256 MiB |
| DirectRepeat blocks per Segment | 64 |
| active Outer FEC decoders | 4 |
| admission charge per Outer FEC decoder | 512 MiB |
| aggregate Outer FEC decoder charge | 1 GiB |
| complete Control record bytes | 65,536 |
| concurrent Control reassembly IDs | 8 |
| aggregate Control reassembly PMR storage | 1 MiB |
| fragments per Control record | 4,096 |
| Control reassembly inactivity observations | 16,384 |
| orphan Transport payload bytes | 4 MiB |
| orphan Transport blocks | 64 |
| zstd decoder window bytes | 8 MiB |
| resume-state record bytes | 256 MiB |
| output preallocation without confirmation | 4 GiB |

The descriptor state uses a policy-bounded memory resource for both Segment
indexes. Each completion bit is stored in its already-admitted ordinal-map
node, so marking a recovered Segment complete performs no late allocation. A
per-Session budget refusal returns terminal `ResourceLimitExceeded`. An
upstream allocator failure returns terminal `ResourceExhausted`; the owner must
destroy that Session before attempting a new admission. Session-registry
admission failures occur before a Session is
published and may be retried after capacity is released.

Control reassembly policy refusal returns `ControlReassemblyQuotaExceeded`;
upstream allocation failure returns `ResourceExhausted`. Policy values do not
enter wire bytes. The exact inactivity boundary remains live and an entry is
evicted only after the caller's monotonic observation difference exceeds the
configured window. `ResetControlReassembly()` is required on ControlEpoch
change and clears active, pending, completed, and conflict IDs.

Outer FEC decoder admission is owned by one receiver-wide
`OuterFecDecoderResourceManager`; `WirehairV2DecoderResourceManager` remains a
source-compatible alias. DirectRepeat and Wirehair V2 consume the same active,
per-decoder, and aggregate caps, so neither mode can bypass receiver admission.
The DirectRepeat charge includes the exact encoded buffer, received bitmap, and
a fixed wrapper allowance. The Wirehair charge covers the complete
accepted-payload window, wrapper and pinned-backend ID tables, solver/work
allowance, per-row state, and fixed state. Admission occurs before any large
wrapper allocation or third-party codec creation. A quota refusal returns
`OuterFecDecoderQuotaExceeded`; a reservation is released by RAII after every
failed create, move/destruction, and concurrent shutdown path. These charges are
deliberately conservative and are not claimed as allocator-exact RSS telemetry.
The DirectRepeat block-count ceiling is an independent work/liveness budget:
it prevents a valid `OuterBlockBytes=1` descriptor from turning one 32 MiB
Segment into 33,554,432 decode operations while staying inside byte quotas.

One receiver owns exactly one manager and shares it with every decoder.
Concurrent admission and counter queries are supported while that manager
object remains stable. Receiver shutdown must stop new admission before moving
or destroying the manager; already-created decoders retain the shared state and
may release their reservations concurrently during worker teardown.

The Qt-free `pbreceiver::ReceiverIngress` is the authoritative logical Data
admission owner for the current implementation slice. It permanently owns one
validated policy, one profile-derived `expectedOuterBlockBytes`, one Control
receiver, one orphan cache, one Outer FEC manager, and one bounded active-decoder
table. `ReceivedTransportBlock` is an in-memory value after upstream
frame/Inner-FEC/Transport-CRC validation; it deliberately does not freeze the
still-provisional Transport wire header.

An unknown `(SessionTag, SegmentOrdinal)` block is structurally checked and can
only enter the orphan cache or return an explicit rejection. Cache identity is
`OuterBlockId + declared PayloadBytes + the complete fixed padded region`.
Canonical padding is checked before storage. Existing duplicate/conflict
recognition runs before new-block quota checks, so a full cache cannot hide a
valid conflict. Equal duplicates are idempotent and do not change occupancy or
event counters; different valid claims latch terminal
`OrphanPayloadConflict`. The cache stores declared length so DirectRepeat short
tails and Wirehair systematic short tails can be replayed without sender-side
metadata. It never creates a decoder, segment-sized buffer, decompressor, output
reservation, or file.

Once Control admission returns a by-value `BoundSegmentDescriptor` capability,
`ReceiverIngress` lazily creates at most one decoder for the Segment using its
single manager. If a bound Segment has orphans, decoder reservation/create must
succeed before Drain. A quota failure therefore preserves the bounded orphan
set for a later Carousel retry. Replay validates descriptor/profile length and
padding and never switches FEC mode after a failure. Session removal and
capture-epoch reset release active decoders first, clear orphan state, and then
remove/rebuild Control state. Cross-thread callers must externally serialize
the façade; decoder reservation destruction remains RAII-safe through shared
manager state.

`ReceiverResourceTelemetrySnapshot` preserves each explicit error status and
reports cumulative saturating counters for total resource-policy rejections,
Control, Outer FEC quota, orphan admit/drop/allocation/conflict, resume quota,
zstd input/output/window/allocation, and output-reservation
denied/confirmation/automatic decisions, plus current occupancy. A receive
operation is counted once by the façade; component counters remain available
for diagnostics. Capture-epoch reset clears occupancy but does not make
cumulative counters decrease.

DirectRepeat uses `DirectBlockCount = ceil(EncodedSize / OuterBlockBytes)` and
the `OuterBlockId` as its ordinal. A short final block carries its true
`PayloadBytes`; the rest of the fixed payload region must be canonical zero
padding. Equal duplicates are idempotent, while a structurally valid duplicate
with different real payload is terminal `OuterBlockConflict`. Recovery becomes
available only after every ordinal is present and the exact reassembled bytes
match the descriptor's BLAKE3 `EncodedDigest`. For an empty file the count is
zero and no Segment descriptor or Data Block exists; the existing descriptor
validator continues to reject zero-length Segment descriptors.

The Phase-0 sender efficiency gate defaults to DirectRepeat for `K=0..2` and
Wirehair V2 for `K=3..64000`; a profile may supply a different frozen,
benchmark-backed threshold through `OuterFecModeSelectionPolicy`. That sender
threshold must be coordinated with the profile's receiver work quota. The mode
is chosen before the descriptor is frozen and is never changed as a
codec-creation fallback. `K>64000` requires Segment splitting or a different
valid block size.

Wirehair backend readiness and recovery success are not sufficient integrity
claims. `WirehairV2Decoder::Recover` verifies the exact recovered bytes against
the bound BLAKE3 `EncodedDigest` and latches a mismatch terminally. The Receiver
façade also verifies the encoded digest before decompression and the `RawDigest`
before returning raw bytes.

The provisional Transport `PayloadBytes` field is `uint16`, so structural
validation, sender codec creation, and receiver policy all enforce
`OuterBlockBytes <= 65535`. DirectRepeat decoder creation additionally receives
the current Visual Profile's expected block size and compares it to the
descriptor before reservation or allocation. This explicit API check is the
Phase-0 bridge until `SessionVisualProfileId` is serialized in the formal v1
descriptor.

Every receiver-policy field must be non-zero, finite, and representable in the
implementation type used for its single allocation or container count.
Unbounded `uint64` sentinels are invalid. The two explicitly bounded protocol
ceilings are `OuterBlockBytes=65535` and
`DirectBlockCount=UINT32_MAX+1`; either exact ceiling remains structurally
representable, subject to the receiver's normally much smaller local quotas.
The per-Session, concurrent-Session, aggregate descriptor, and three Outer FEC
decoder limits remain independent caps; admission applies each aggregate cap
before publishing the corresponding state.

The policy also rejects a Session whose declared map cannot possibly cover its
file under `maxRawSegmentBytes`, including:

```text
ceil(OriginalFileSize / maxRawSegmentBytes) > SegmentCount
```

## SessionTag collision handling

`SessionRegistry` owns the active `SessionTag -> SessionId` binding. It permits
an exact repeated `SessionDescriptor`, but when a second different SessionId
derives the same active tag it removes the previous candidate, marks the tag
ambiguous, and rejects both candidates from the hot path for the lifetime of
that registry. Active bindings and ambiguous tombstones consume the same
`maxConcurrentSessions` routing-entry quota; once tombstones fill it, new
distinct Sessions fail with `ResourceLimitExceeded` rather than growing an
unbounded collision-history map. Tests inject a deterministic colliding tag
derivation; they do not attempt a birthday search against BLAKE3. The injection
entry is a private, friend-only test seam; production registry creation always
uses `DeriveSessionTag()`.

The tag is routing metadata, not sender authentication.

## CRC32C, BLAKE3, checked arithmetic, and SessionId status

The Phase-0 primitive slice for CRC32C, allocation-free streaming BLAKE3-256,
checked add/multiply/range/narrowing, deterministic SessionTag derivation, and
Windows CSPRNG SessionId generation is implemented and covered by independent
known-answer and boundary tests. The public BLAKE3 wrapper keeps the pinned
third-party header and link dependency private, and protocol-critical digest
paths do not allocate.

These primitives have deliberately separate trust meanings:

- CRC32C detects transport corruption and is not cryptographic integrity or
  authentication;
- unkeyed BLAKE3 provides strong integrity, but an in-band WholeFileDigest does
  not authenticate the sender;
- SessionTag is deterministic routing metadata and not an identity proof;
- SessionId generation uses `BCryptGenRandom` with
  `BCRYPT_USE_SYSTEM_PREFERRED_RNG`, fails closed on unsuccessful NTSTATUS, and
  has no weaker fallback.

Ordinary `WholeFileDigest` equality is an integrity comparison, not a
constant-time authentication verifier. Any future MAC or signature must use a
different type and a dedicated verification API rather than reusing this
public digest type.

`DescriptorBindingState` binds immutable expected FinalManifest metadata but
does not expose a whole-file verification or publication decision. That
decision requires a later recovery finalizer to sequentially read the actual
`output.part`, compute the digest, compare it with the bound manifest, and only
then perform the same-volume atomic rename.

## PBInnerFec reference slice (DVB-S2 Short QC-LDPC, N=16200)

`libs/PBInnerFec` (namespace `pbinnerfec`) implements the Phase-0
correctness reference for the design document section 14 inner FEC: a
deterministic encoder, a reference soft decoder (layered offset min-sum,
saturating int32 messages, one fixed per-instance workspace), the
structured syndrome check with early termination, and the frozen profile
identity.

Frozen profiles (protocol identity is
`InnerFecProfileId + exact N/K + InnerFecMatrixId/MatrixDigest +
canonical systematic bit order`; rate labels are never parsed back):

| Profile | K | N-K | Q | lines | profileId |
| --- | ---: | ---: | ---: | ---: | --- |
| Robust | 10800 | 5400 | 15 | 30 | `0x36BC661265E826C3` |
| Balanced | 11880 | 4320 | 12 | 33 | `0xF01CACD38B344350` |
| Fast | 13320 | 2880 | 8 | 37 | `0x24B794EB5A445D58` |

The profile/matrix IDs are the first 8 bytes (little-endian) of
BLAKE3-256 over `PixelBridge/InnerFecProfile/DVB-S2-Short-N16200-K<exact K>`
and `PixelBridge/InnerFecMatrix/DVB-S2-Short-N16200-K<exact K>`; every other
K (including the experimental K=14400), profileId, or matrixId is rejected
fail-closed. Puncturing/shortening is `None` (exact K only). All three
public entry points (encoder, decoder creation, syndrome check) run the
same live MatrixDigest gate on the embedded tables, and the encoder
rejects overlapping information/codeword spans with `InvalidInput`
(fail-closed contract enforcement, both pinned by tests).

Conventions frozen by the public headers and pinned by tests:

- canonical systematic bit order: codeword = [K information bits][N-K
  parity bits] in natural DVB-S2 Short index order, packed LSB-first
  (bit i = byte[i/8] bit i%8); information bit i sits in matrix line
  i/360, within-line index i%360; parity bit j connects check rows j and
  j+1 (sub-diagonal staircase, the ETSI/AFF3CT reference structure; row
  contributions followed by the prefix-XOR chain P[j] = A[j] XOR P[j-1]).
- LLR: int16, positive means bit 0 is more likely; hard decision
  `llr[i] < 0 ? 1 : 0` (zero decides 0).
- decode options (maxIterations, syndromeCheckInterval, offset,
  scaleNum/scaleDen) are receiver-local capability, never wire fields
  (design 14.2); the transport CRC32C of the recovered information bytes
  remains the caller's final gate after early termination (design 14.3)
  and is pinned by the CRC integration test.

Matrix provenance: the embedded tables are the DVB-S2 Short FECFRAME
matrices of ETSI EN 302 307-1 Table 5b, extracted from AFF3CT
(aff3ct/aff3ct develop, commit `e8a65c5047262d97a15563b9edc961f69b2792cc`,
file `include/Tools/Code/LDPC/Standard/DVBS2/DVBS2_constants_16200.hpp`,
raw file SHA256
`AF378CA17CA2F400B5ECECEC81BEC69BAE1BCD83788E0856D9C8BC106406F2C3`, BSD
license), cross-checked value-for-value against an independent open-source
transcription of Table 5b (freecores/dvb_s2_ldpc_decoder,
`mti/dvbs2_hdef.txt` labels 2_3s / 11_15s / 37_45s) at extraction time and
against a fresh fetch of the pinned upstream file (150/141/158 values per
profile, zero delta). No AFF3CT code is linked and no third-party private
structure is serialized.

Golden vectors (pinned hex in the test suite): the three canonical
matrix-serialization digests
`c6d8eabe...e34dd3` / `2a491986...5342da3` / `720993e2...e77e76c`
(full values in `test_inner_fec_profile.cpp`), the zero codeword, the
all-ones codeword/parity digests (with the residue-class parity structure
pin), the deterministic pattern codeword, nine seeded random codeword
digests, and the unit-information parity digests. Verification evidence
per profile: 200+ random codewords pass an independent explicit
parity-check matrix H-c = 0; 1000 seeded vectors pass both the structured
and the brute-force syndrome path; the decoder pins clean-channel one-pass
recovery, single-bit correction in one pass at the error magnitude,
deterministic small-flip correction (1/3/5/10/20/50 flips), over-capacity
failure with the output untouched, the all-inverted and low-confidence
(|LLR| = 1/5) boundaries, and the "a different valid codeword decodes as
itself" gate that keeps the CRC decision with the caller.

Verification boundary (declared): this machine has no AFF3CT runtime
oracle, so no cross-implementation decode benchmark runs here; the matrix
identity is instead pinned by the three-source value-for-value check above
and the golden digests. The reference decoder is a scalar CPU
implementation (design 14.2 lists AVX2/AVX-512 as later work), and the
slice intentionally excludes the DVB-S2 BCH outer code, the DVB-S2 bit
interleaver (`InterleaveProfileId`), and the wire serialization of
`InnerFecProfileId` inside the formal v1 SessionDescriptor (separate
task). A future environment with AFF3CT installed should add an oracle
regression comparing this encoder against the AFF3CT DVB-S2 encoder on
identical information bits.

### Independent re-verification (2026-08-25 review)

The review re-fetched the pinned upstream file at commit
`e8a65c5047262d97a15563b9edc961f69b2792cc` (SHA256
`AF378CA17CA2F400B5ECECEC81BEC69BAE1BCD83788E0856D9C8BC106406F2C3`,
matching the provenance record) and re-verified it value-for-value
against the embedded tables (150/141/158 values, zero delta on every
value and on all structural parameters N/K/M/Q/N_LINES). A throwaway
independent spec-based encoder (separate code path, upstream flat data
layout) reproduced the library codewords byte-for-byte on 2003 vectors
per profile (zero, all-ones, the 0xC0FFEE pattern, and 2000 seeded
random vectors); an explicit-row H*c=0 oracle built from the upstream
data accepted every library codeword and rejected 50 single-bit
corruptions per profile. The documented profile/matrix ID derivation
(first 8 bytes, little-endian, of BLAKE3-256 over the fixed UTF-8
strings) was recomputed and matches all six frozen constants, and the
derivation is now pinned by a committed test.

Decoder adversarial sweep (same throwaway harness, no repo changes):
int16 extreme LLR boundaries, the scaleNum=0 and max-offset
hard-decision regimes, failed-then-clean instance reuse, and the
interval=maxIterations semantics all behave per the pinned contracts,
and these behaviors are now pinned by committed regression tests in
`tests/PBInnerFec`. A 117-case deterministic flip sweep (13 flip counts
x 3 seeds x 3 profiles at magnitude 8192) decodes 36/39 (Robust), 30/39
(Balanced), and 24/39 (Fast) codewords, with the correction capability
ordered by rate and no successful decode emitting a wrong information
block. A focused bounded LDPC codeword fuzz driver now covers syndrome and
clean-channel decode/re-encode; performance/throughput benchmarking remains
a separate gate.

## resume.state FastResume basic tier status (design doc section 31)

The receiver-local `resume.state` document now implements the FastResume
basic tier. The v1 envelope is unchanged and remains independently parsed by
`ParseResumeRecord`: magic `PBRS`, version 1, reserved byte, little-endian
payloadLength u64, and CRC-32C over `[magic .. last payload byte]`. New typed
records are appended inside the envelope payload with a first-byte tag; unknown
tags fail closed through `InvalidEnumValue`, so a newer writer cannot smuggle
record types past an older parser.

Three record types persist exactly the design-doc sections 31.1 to 31.3
minimal fields and no third-party codec private structure:

- Completed Segment metadata (73-byte body): SessionId, SegmentOrdinal,
  rawOffset, rawSize, and the RawDigest integrity bytes;
- Active Wirehair cache: SegmentOrdinal plus the canonical serialized 32-byte
  Wirehair V2 profile snapshot and every validated outer-block payload exactly
  as handed to `WirehairV2Decoder::DecodeBlock` (the final systematic block may
  be shorter than OuterBlockBytes);
- DirectRepeat received blocks: descriptor-derived directBlockCount plus each
  validated entry's canonical zero-padded full block and the exact realPayload
  length replay must re-pass.

`LoadResumeState` applies the receiver resource policy first, then bounds the
whole document by `maxResumeBytes` before any byte is interpreted. Records are
parsed sequentially with single-record budget checks; typed bodies must be
fully consumed. A trailing region that cannot hold one complete structurally
valid record is dropped as a torn tail and reported via
`LoadedResumeState::hasTruncatedTail`; any malformed record whose envelope is
fully present fails the entire load fail-closed with no partial result. Key
conflicts (same completed key or same active-cache ordinal, including across
record types) and in-record payload conflicts reject the document with the new
append-only diagnostic code `ResumeRecordConflict`; identical-content
duplicates are deduplicated on load while the strict-writer
`ResumeStateBuilder` rejects them. All size arithmetic uses checked helpers;
no hash tables are introduced for uniqueness detection.
Accepted per-segment records (one record per distinct segment ordinal across all
categories) are additionally bounded by `maxSegmentCount`, implementing the design doc section 31.4 active incomplete
segment limit: a further distinct record fails with `ResourceLimitExceeded` at that record start,
while identical duplicates never consume quota (the builder mirrors this gate so it can
never emit a document the loader would reject).

`PBReceiver` adds the replay API and bounded file IO. Both replay functions
first reject a replay into an empty (moved-from) decoder with `InvalidState`
before any metadata comparison: an empty decoder's zero-filled sentinel
profile / zero bound block count is never treated as a match for stale state.
`ReplayActiveWirehairCache` then cross-checks the persisted profile snapshot
against the decoder's descriptor-bound profile (mismatch fails closed with
`UnsupportedProfile` without consuming the decoder) and then re-injects cached
entries into a freshly created `WirehairV2Decoder` in stored order;
`ReplayDirectRepeatBlocks` first cross-checks the persisted block count against
the descriptor-derived bound count (mismatch fails closed with `InvalidInput`,
detail = persisted count) and then re-passes each entry to a
`DirectRepeatDecoder`, which revalidates lengths against its own
descriptor-derived expectation before accepting. File IO is stat-first:
the on-disk size is checked against `maxResumeBytes` before any read or
allocation, then the document is read as one fixed-length binary image;
`WriteResumeStateFile` reports success only after the data is flushed and the
stream is closed, so a torn write never masquerades as persisted state. This
tier has no crash-safe flush ordering, no `.part` commit-order proof, and no
live ingress write path; section 31.5 crash-safety semantics remain later work,
so callers may recompute `.part` digests instead of blindly trusting completed
records (the header comments state this explicitly). Replay enforces the
metadata trust boundary at replay time (profile, bound block count,
per-entry codec validation); content trust is enforced at `Recover`. Entry
ids are not range-checked at the replay layer: the Wirehair decoder
deliberately accepts an elastic repair-equation window far beyond the
profile's planned repair count, so a well-sized but wrong-content payload
is accepted by the codec, and a replay that reaches Ready on such content
fails closed at the `Recover` encoded-digest gate (probed:
`EncodedDigestMismatch`). Probed crafted records (out-of-window ids,
wrong-content payloads, entry counts beyond the finite admission window)
all either fail the replay with the decoder's exact terminal
`OuterFecError` or reach that digest gate; none publishes unverified
content.

Coverage: exact-error-code unit matrices with independent byte-level Golden
pins, torn-tail exhaustive per-byte sweeps, builder failure-immutability and
budget-exhaustion tests, replay end-to-end against the real Wirehair V2 codec
(bit-exact Recover plus BLAKE3 cross-checks), a crash-restart integration test
that rebuilds a second receiver purely from saved control bytes plus
`resume.state` and verifies whole-file digests, bounded file IO tests with the
stat-first budget gate, and a structured dual-mode fuzz harness with a pinned
13-seed corpus under `fuzz/corpus/resume-state/`.

## Phase-0 acceptance versus later product requirements (P0-15)

Phase 0 proves logical wire bytes, bounded Segment/FEC recovery, resource
limits, basic save/destroy/load/replay resume, and CPU/reference file round trips.
It does not require completing the future PixelBridge v1 product. The logical
Bootstrap/Control bytes and bounded fragment reassembly remain unchanged.

The opt-in P0-15 Gate now composes actual raster demodulation with independent
fixed-profile/Control/Data framing checks and ReceiverIngress admission. The
reference binding and its no-interleave interpretation are documented in
`REFERENCE_RASTER.md` section 13. Receiver completion requires verified bytes
followed by explicit stored commit. Finalization returns the bound authoritative
manifest only for a complete stored Segment map; the Gate sequentially hashes
the actual `.part` and compares final file bytes. Completed resume metadata is
revalidated against the full SessionId and bound descriptor, then actual stored
raw bytes are rehashed, including Zstandard Segments, without re-compression.

Future Certified profiles, physical Bootstrap/Control FEC and repetition cadence,
Present/Capture robustness, production storage free-space UX/preallocation and
durable crash ordering, live ingress snapshot scheduling/ResumeDegraded, GPU,
and MP4 remain **deferred product requirements**, not blanket Phase-0 blockers.
Their Phase-0 subsets above must still pass independently; a deferred label
does not excuse ambiguous reference parsing or missing basic resume.

`PHASE0_GATE_REPORT.md` records the P0-15 scope, fixes and eight-view review.
The exact clean-commit result and tag decision are sealed externally under
`build-phase0-gate-evidence/<full-commit>-<timestamp>/`; this status document
does not itself assert a final Gate PASS or v1 wire/certified-profile freeze.
