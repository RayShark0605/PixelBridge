# PixelBridge architecture and technical route

[简体中文](PixelBridge_最终技术路线与总体设计.md) | **English**


> Product labels are Standard / Gray Fast / PAM4 / PAM4 Wide, without experiment badges. Frozen `Experimental` / `experimental-*` identifiers below remain for internal/CLI compatibility, not as product-version labels. Validation scope still follows actual evidence.

> Maintained design, 2026-09-17. This replaces stage-by-stage claims of current status and unimplemented candidate routes in the earlier design. Protocol, file safety, resource, and capture lifetime invariants remain binding. A product version does not broaden the environments covered by measurements. See the bilingual [release notes](RELEASE_V1.0.md) for v1.0 contents and validation boundaries.

## 1. Objective, boundaries, and completion

PixelBridge targets **one-way file transfer through non-local remote-desktop pixels on Windows x64**. The primary metric is time to a correctly completed file, not configured FPS, theoretical bits per cell, or hardware utilization.

- Encoder reads a local source and displays a pattern; Decoder only reads pixels actually captured from its selected screen region.
- Sockets, pipes, shared memory, COM, clipboard, window messages, and temporary-file exchange must not carry payload, decoded blocks, or reverse ACKs.
- PBBridge is for development deployment, process orchestration, logs, and digest metadata, never payload/pixels or receiver-decision input.
- Qt Widgets belongs to the application adapter; protocol, FEC, modulation, capture, storage, and runtime cores remain Qt-free.
- The delivery route is live desktop visual streaming. Offline MP4, cameras, arbitrary HDR/scaling, cross-platform reception, and a full CUDA pipeline are not validated delivery features.

**Acceptance chain:** per-segment EncodedDigest → bounded decompression → RawDigest → durable storage → WholeFileDigest → safe publication → final reopen verification. Only the last step establishes whole-file success. CRC is not a cryptographic file digest; in-band digests do not authenticate the sender.

[Current status](PROJECT_STATUS.en.md) and [evidence](EVIDENCE_INDEX.md) are maintained separately, rather than copied into successive design addenda.

## 2. Actual data path

```text
Encoder read-only source
 → Streaming prescan, segmentation, RAW / Zstandard
 → Immutable Session / Segment / FinalManifest
 → DirectRepeat or Wirehair V2 segment erasure coding
 → Transport + CRC32C → QC-LDPC codeword error correction
 → Independently identified raster → D3D11 data window
 → Existing remote video encoding, network, decoding, scaling, desktop composition
 → Decoder WGC / DXGI capture of the selected screen
 → PB-owned textures, epoch/frame-age checks → geometry, visible calibration, soft demodulation
 → Inner FEC / CRC / identity admission → typed Control / Receiver
 → Segment recovery, digests, resume state, publication, reopen
```

The remote application is an external channel, not something PixelBridge reconfigures. Provider names are operator metadata, never inputs to decoder thresholds, profiles, resource admission, or integrity decisions.

## 3. Visual modes and identity isolation

| GUI mode | Exact identity / layout | Status |
| --- | --- | --- |
| Standard | `PB-Unified-SC6-V3` / `0x5042554E49534333` / 10 | CLI/core default and sole formal catalog entry; GUI compatibility index 0 |
| Gray Fast | `UnifiedGrayFast` / layout 13 | Selectable mode; previous identity retained |
| PAM4 | `PB-Experimental-Pam4-1` / `0x504250414D343031` / 15 | Fresh/missing/invalid GUI preference default (index 2), physical 1080P raster |
| PAM4 Wide | `PB-Experimental-Pam4-Wide-1` / `0x504250414D345731` / 16 | Selectable mode, fixed physical 2560×1440 raster |

Both ends must match. Data Profile is immutable within a Session; an incompatible change requires a new Session. Trying several identities/FEC modes and choosing whichever passes CRC is not a permitted fallback. Historical layouts 8/9, LF4, and layout-14 research are not new defaults.

Valid saved GUI indices retain their meaning (0 Standard, 1 Gray Fast, 2 PAM4, 3 PAM4 Wide). The GUI default does not change CLI/core defaults, formal catalog membership, or any Profile identity.

### 3.1 Why four-level grayscale?

Standard SC6 V3 uses independently erasable Base Luma, Fine Luma, and Chroma lanes on a 1920×1080 raster. Region-local placement and codeword-local permutations limit correlated contamination. Gray Fast extends the existing grayscale route with a higher inner-code information ratio.

Remote video often subsamples chroma at 4:2:0. Compression, scaling, partial updates, and temporal mixing also damage fine shapes. PAM4 uses neutral levels `[0,85,170,255]`, two bits per cell, without relying on chroma for extra bits. Grayscale still requires adequate spatial resolution, visible calibration, and strict FEC; it does not make the video channel lossless.

### 3.2 Fixed PAM4 parameters

| Parameter | PAM4 | PAM4 Wide |
| --- | ---: | ---: |
| Logical canvas | 1920×1080 BGRA8 SDR | 1920×1080 BGRA8 SDR |
| Logical cell | 4×4 px | 3×3 px |
| Fixed sender raster | 1920×1080 | 2560×1440 |
| Effective sender cell | 4×4 px | 4×4 px |
| Eligible Data cells | 92,832 | 165,192 |
| Fixed Control slots | 1 | 1 |
| Transport slots | 10 | 19 |
| Total codewords | 11 | 20 |
| Full-frame file-payload ceiling | 16,290 B/frame | 30,951 B/frame |

Control always uses Robust QC-LDPC `N=16200/K=10800`, 1350 information bytes. Transport uses Fast `N=16200/K=13320`, 1665 information bytes and at most 1629 file-payload bytes per slot. Inactive slots, unmapped bits, and padding obey their canonical zero rules.

These are raster capacities, not file goodput. Control refresh, repair overhead, video loss, duplicate observations, admission deferrals, and final verification all reduce net throughput.

Wide uses fixed pixel-center point sampling from 1920×1080 to 2560×1440, turning each logical 3px cell into exactly 4 physical pixels. It is not a physical 3px-at-1080P experiment or arbitrary interpolation. Static borders reduce unnecessary video-encoding work rather than forcing the pattern to fill every screen.

### 3.3 Geometry, calibration, mappings, and decoding

Four role markers, two RS Bootstrap records, nine timing/freshness regions, four calibration regions, PhaseChecker, and guards remain reserved. Exact identity and freshness are checked for every frame; failures produce erasure.

Visible coarse levels are `[8,64,160,232]`; held-out fine levels `[56,112,168,248]` and neutral 128 validate the fit. Spacing, contrast, spatial deviation, and residual limits are fixed; known source bytes cannot tune them. PAM4 retains the fixed 127.5 soft-metric denominator. Wide uses 1.5 times the median observed adjacent centroid spacing. These remain independent contracts.

Mappings are frozen read-only tables, not regenerated at runtime. Independent Goldens validate raster, coded frame, mapping, FEC, and reserved regions. Exact parameters and machine-readable manifests: [PAM4](EXPERIMENTAL_PAM4_PROFILE.md), [Wide](EXPERIMENTAL_PAM4_WIDE_PROFILE.md).

**The actual PAM4 receive implementation uses CPU-reference demodulation/FEC after GPU capture and ROI readback.** It is not an all-GPU or zero-readback path. Logs independently report PAM4 observations, erasures, accepted slots, and readback/decode cost instead of inventing Unified lane metrics.

## 4. Feedback-free scheduling and large-file tails

Encoder prescans an immutable Session and compresses segments. The ordinary default segment is 8 MiB; the current PAM4 GUI chooses 7 MiB. RAW fallback avoids expansion; small/highly compressible segments use DirectRepeat, suitable dimensions use Wirehair V2.

Carousel pass zero sends systematic blocks and repair budget; later passes generate fresh repair IDs. Existing paths differ by segment count and cadence, including windows, barriers/graduation, and repair visits. A small test on a different scheduling branch cannot establish large-file behavior. Constants, leases, windows, and budgets are defined by the [sender persistence reference](ENCODER_STREAMING_CAROUSEL.md) and scheduler tests.

Each PAM4 data frame carries its SegmentDescriptor. Session/Manifest refresh frames leave Transport slots inactive. No separate channel announces the schedule to the receiver.

Without ACKs, Encoder does not know the last missing segment. The tail may receive repeated/completed segments until useful repairs appear. A changing video image is not proof of new independent equations. Video cost, payload per frame, admission losses, and synchronous persistence overhead must be evaluated together.

## 5. Protocol and file safety

- Explicit little-endian serialization, checked lengths/offsets/multiplication/addition; no persisted C++ memory layouts.
- Protocol 1.0 / Descriptor Schema 1. Unknown mandatory features, incompatible major/schema versions, invalid reserved/padding bits, and exact-length failures are rejected.
- OS-CSPRNG Session IDs. Source immutability is enforced for the Session; source mutation aborts. Resume preserves descriptors and encoded bytes.
- Conflicting valid descriptors or `(SessionTag, SegmentOrdinal, OuterBlockId)` payloads fail closed, never latest-wins.
- Unknown-segment data is bounded by orphan policy before descriptor/resource validation; no premature large decoder creation.
- Validate Wirehair dimensions/K before creation, and handle quota, OOM, and extra-insufficient results explicitly.
- Accept only safe basenames; reject traversal, absolute paths, ADS, reserved devices, NUL, invalid trailing dots/spaces, and excessive length.
- Never silently overwrite final files or rename `.part` before required verification.

See the exact [Descriptor Schema field tables](PROTOCOL_1_DESCRIPTOR_SCHEMA.md).

## 6. Bounded memory and active decoders

An active decoder retains FEC state for a segment; it is not a CPU thread. The default limit is 8 instances, 1024 MiB total, 512 MiB per instance. Explicit performance mode removes the fixed eight-slot constraint, not finite resource admission or segment-count bounds.

Users can configure total/per-instance budgets; resume capacity is derived from the total. Startup checks available physical/commit headroom including recovery copies and capture overhead. Configuration does not allocate the whole budget immediately. Insufficient resources cause deferral, not unbounded creation. Memory remains `O(active segment set)`, not `O(file bytes)`.

Queues, staging, orphan/control reassembly, caches, repair attempts, and logs are bounded. Supported files retain a 500 GiB product cap. More RAM helps only when admission is actually limiting useful reception. [Complete budget rules](DECODER_MEMORY_BUDGET.en.md)

## 7. Persistence, crash recovery, and publication

Encoder freezes descriptors/encoded bytes and leases durable sequence/repair-ID ranges to prevent reuse after crashes. Encoding buffers remain bounded.

Decoder revalidates `.resume` length, CRC, generations, descriptors, and current resource policy, then rechecks completed segments on disk. Only a provable torn tail is recoverable by truncation; internal corruption/conflicts are not tolerated.

Accepted-block checkpoints combine complete records in a bounded 64 KiB batch buffer. A maximum-size valid record can reach 65,591 B; it is written alone without enlarging that buffer. Success still requires flush. Short writes, partially written prefixes, or seek/write/flush errors cannot count as commits. Explicit budget mode coalesces unproductive compaction for large active sets, retaining finite quota and immediate durability for completed segments.

Segment chain: recovery → EncodedDigest → bounded decompression → RawDigest → `.part` write/flush/checkpoint → completed record → Receiver stored commit. Final chain: all segments and Manifest agree → whole-file BLAKE3 → publish intent → safe same-directory publication → reopen verification. A matching filename alone does not resolve a publish crash window. [Persistence reference](DECODER_RESUMABLE_RECOVERY.md)

## 8. Windows capture and presentation lifetime

- All coordinates are physical pixels; Per-Monitor DPI Aware V2, preferably one-monitor ROI.
- WGC first; Desktop Duplication fallback is limited to defined initialization/device-recovery failures, not arbitrary bad pictures.
- WGC frames are frame-pool leases. Copy into bounded PB-owned textures and retire only after GPU source use completes. No reading after lease return.
- ContentSize, ROI, mode, device, monitor, or epoch changes invalidate/drain stale work and rebuild dependencies; old-domain results never enter a new domain.
- Drop expired frames instead of accumulating latency. One owner submits immediate-context work. Respect actual pitch and format.
- PAM4 selected-monitor authority is separate from the legacy protected/experiment dual-monitor field gate. Revalidate device, geometry, DPI, rotation, refresh, and adapter at startup and during capture; never invent a protected monitor.
- No automatic window moving/minimizing, focus stealing, or injected input. Single-monitor users make the remote pattern visible themselves; Decoder can run behind it.

References: [WGC](PBScreenCaptureWgc.md), [screen regions](SCREEN_REGION.md), [presentation](PRESENTATION.md). Resolution support does not certify every remote scaling chain.

## 9. Automatic diagnostics and wait explanations

Normal GUI/transfer-CLI polling writes a snapshot every two seconds, extra state-transition samples, and a final report on stop/failure/completion. Records include build, Session, Profile, phase, cumulative counters, memory, and errors; never payload/pixels or receive-admission input.

Each run has a 64 MiB / 48-hour timeline limit. Truncation is visible and the final report is still attempted. Startup retains seven recent old logs plus the current run per role; active/unremovable files are preserved with a warning. Explicit evidence/user files are not pruned.

Activity derives from monotonic time and actual synchronous recovery operations, not a lingering `Verifying` state. Expected repair/carousel/verification waits advise against pausing. Capture interruption, resource backpressure, sustained lack of useful data, and errors are distinct. The UI neither invents progress nor weakens acceptance or promises that all stalls self-resolve. [Diagnostics](DIAGNOSTICS.en.md)

Decoder GUI active reception timing starts at the first `SessionDescriptor` accepted and validated for the current Profile, so the wait after clicking Start and before a valid coded picture appears is excluded from GUI elapsed time, average speed, and ETA. Recovery, disk writes, digest, publication and final reopen remain inside that window; the completed GUI average is strictly original file bytes divided by the frozen active elapsed time. This is a UI convention and does not change formal `runStarted/runEnded`, verified raw progress, or verified-goodput full-run evidence.

## 10. Performance truth and evidence tiers

Use same-host complete-run time through `finalReopenVerified` from `startAccepted`, and verified goodput, including control waiting, recovery tail, storage, digests, publication, and reopen. Never subtract monotonic clocks across hosts. CPU/GPU/FPS are explanatory metrics only.

Separate Golden/CPU-reference, codec/distortion proxy, offscreen GUI/application correctness, real-remote small-file, and full-file same-environment results. One tier cannot stand in for the next. Preserve source/package identity and mark old-candidate measurements as historical after code changes.

The retained large-file result is about 70m28s. One-hour and stall-free goals remain unmet; neither a theoretical limit nor global optimality is proven.

## 11. Delivery direction and longer-term vision

Current work is stability, logging, clear interaction, resolution boundaries, documentation, and reproducible delivery—not new profile/density/FPS sweeps. v1.0 delivers separate application packages. Wire identifiers and default-mode choices remain independent of the product version number.

Future performance work should start from complete logs, isolate useful-data rate and tail cost, use narrow comparisons with frequent files no larger than 100 MB, and verify final files. Scheduling, parallel decoding, and GPU work each need correctness/cost evidence; a hidden reverse channel is never an optimization option.

## 12. Source map and tests

| Area | Entry |
| --- | --- |
| GUI and automatic logging | `apps/PixelBridgeEncoder`, `apps/PixelBridgeDecoder`, `apps/common/operational_log_qt.*` |
| Activity explanation | `apps/common/decoder_activity.*`, actual runtime operation markers |
| Scheduling/recovery | `local_desktop_runtime.cpp`, `sender_carousel_scheduler.*`, `decoder_resume_store.*` |
| Protocol/FEC | `PBProtocol`, `PBOuterFec`, `PBInnerFec` |
| PAM4 | `PBModulation` independent PAM4/PAM4 Wide headers, sources, mappings and Goldens |
| Capture/demod | `PBCaptureNormalize`, `PBScreenCaptureWgc`, `PBScreenCaptureDxgi`, `PBDemodD3D11` |
| File acceptance | `PBReceiver`, `PBStorage` |

Build/test rules and explicit opt-in requirements for real-screen work: [development guide](../CONTRIBUTING.md).
