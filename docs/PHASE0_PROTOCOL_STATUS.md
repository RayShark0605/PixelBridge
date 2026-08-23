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
| concurrent Sessions | 4 |
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

The descriptor state uses a policy-bounded memory resource for both Segment
maps. A per-Session budget refusal returns terminal
`ResourceLimitExceeded`. An upstream allocator failure returns terminal
`ResourceExhausted`; the owner must destroy that Session before attempting a
new admission. Session-registry admission failures occur before a Session is
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
that registry. Tests inject a deterministic colliding tag derivation; they do
not attempt a birthday search against BLAKE3. The injection entry is a private,
friend-only test seam; production registry creation always uses
`DeriveSessionTag()`.

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

## Remaining Phase-0 Gate scope

This status decision closes the ambiguity around the current descriptor bytes
and marks the logical Bootstrap/Control byte protocol, bounded Control
fragmentation, and authoritative admission boundary GO. It does not declare the
overall Phase-0 architecture Gate complete. Formal profile binding,
Control/Bootstrap visual FEC and raster mapping, physical repetition cadence,
complete file recovery, the remaining Golden Vectors, and later CPU/GPU backend
consistency gates remain separate work.
