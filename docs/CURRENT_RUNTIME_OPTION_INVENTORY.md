# PixelBridge Phase 1.5 Current Runtime Option Inventory

Status: implementation inventory for the first Windows GUI. This document records only options that are reachable through the current worktree's real public bindings, layered on pre-GUI baseline HEAD `80699813b595bcf6db64047b50d31056872e33e1`. The frozen annotated `phase1-gate-pass` tag object remains `fde56c4c4e7124e8ffe29a0dcb619f8236781ebb` and still peels to that same baseline commit. This is not a profile certification statement.

## Product boundary used by the GUI

- Instant LocalDesktop only: the Encoder broadcasts visual frames until the user stops it; the Decoder converges independently.
- One Segment per Session, with a source size of 1 byte through 8 MiB. The current production multi-Segment scheduler does not exist, so larger and empty files are rejected rather than truncated or silently rerouted.
- Fixed 1920 x 1080 physical-pixel Data Window and ROI. Scaling, resampling, and multi-monitor ROI are rejected.
- The wire currently does not carry the original file name or Visual Profile. The Decoder therefore requires the user to select the same profile and publishes a safe generated name, `PixelBridge-<SessionTag>.bin`.
- The current descriptor wire is Phase-0 provisional. The GUI does not change it and does not add GUI metadata to protocol records.

## Encoder options with real bindings

| UI option | Availability | Public binding | Runtime effect and constraints |
| --- | --- | --- | --- |
| Source file | Enabled | application controller opens the source with Win32 bounded file I/O; protocol descriptors use `pbprotocol::SessionDescriptor`, `SegmentDescriptor`, and `FinalManifest` | The controller holds the source handle for the Session, verifies identity/size/last-write stability periodically and at stop, and rejects files outside 1 byte..8 MiB. |
| Segment compression | Enabled, default off | off: `pbcompression::EncodedSegment` with `CompressionCodec::Raw`; on: `pbcompression::CompressSegment` | On uses the existing zstd frame and RAW fallback rule. Off is byte-identical RAW. No new wire codec is introduced. |
| Compression level | Advanced, enabled only when compression is on | `pbcompression::CompressionSettings::compressionLevel` | Range 1..22 for the pinned zstd baseline. It is local preparation tuning and is not a wire field. |
| Visual Profile: Direct-Level 2x2 | Enabled, Experimental | `pbmodulation::kDesktopLevels2ProfileId`, `EncodeDesktopLevelsFrame` | Current Phase-1 physical file Gate path. Fixed 1920 x 1080, strict 1:1 physical pixels. |
| Visual Profile: Shape+Chroma | Enabled, Experimental | `pbmodulation::kShapeChromaProfileId`, `EncodeShapeChromaFrame` | Current experimental A/B Gate path. Not a Certified Profile. |
| Visual Profile: RemoteVisual Resilient 8x8 Luma | Enabled, Experimental | `pbmodulation::kRemoteVisualProfileId`, `EncodeRemoteVisualFrame` | Current one-bit/tile production experiment. It remains strict 1920×1080/1:1 on the D3D11 receive path and is not the new LF4 profile. |
| Outer FEC | Read-only automatic | `pbouterfec::ChooseOuterFecMode`, `DirectRepeatEncoder`, `WirehairV2Encoder` | DirectRepeat is selected for the existing tiny-segment threshold; Wirehair V2 is selected otherwise. The UI cannot construct an illegal override. |
| Inner FEC | Read-only | `pbinnerfec::kInnerFecProfileIdRobust`, `EncodeQcLdpcCodeword` | Fixed robust DVB-S2 Short QC-LDPC profile. No override is exposed. |
| Target monitor | Enabled | `pbrenderd3d::DataWindowConfig::clientOrigin` after application-layer Win32 monitor enumeration | Selects the existing D3D11 Data Window. A fixed 1920 x 1080 client canvas is centered in the monitor work area when it fits, otherwise centered in the physical monitor; Qt never paints the payload. |
| Logical Visual FPS / stable dwell | Advanced | application scheduler | Public Local profiles allow 0 (presentation-driven) or 1..240. Public RemoteVisual requires 1..5, defaults to 2 (500 ms), and rejects both 0 and >5. The hidden LF4 Encoder candidate additionally keeps one immutable active source and repeats display Presents without creating logical data. |
| Frame hold / Present candidate | Not exposed | `PBRenderD3D::DataWindowConfig::repeatActiveFrame` is an internal opt-in used only by the hidden LF4 Gate | There is no GUI/CLI arbitrary hold override. Default false preserves old profile behavior; LF4 Gate mode presents the same complete active source on display permits and invalidates it at an epoch change. |

## Decoder options with real bindings

| UI option | Availability | Public binding | Runtime effect and constraints |
| --- | --- | --- | --- |
| Output directory | Enabled | `PBStorage` bounded `.part` reservation and same-directory write-through final publish | UI never assembles or renames the file. Existing targets and `.part` files are not overwritten. |
| Capture backend: WGC | Enabled | `pbscreencapturewgc::WgcCapture::CreateNormalized` | Explicit backend; no silent fallback. Requested and actual values remain WGC. |
| Capture backend: DXGI Desktop Duplication | Enabled | `pbscreencapturedxgi::DxgiCapture::Create` | Explicit backend; no silent fallback. Requested and actual values remain DXGI. |
| Capture backend: Auto | Not exposed | none | No public, proven WGC-to-DXGI policy currently exists. |
| ROI selector | Enabled | `pbscreenregion::SelectScreenCaptureRegion` | Native PMv2 selector returns a physical-pixel rectangle on one monitor. |
| Use entire monitor | Enabled after an ROI/monitor is known | `pbscreenregion::ResolveScreenCaptureRegion` with `monitorPhysicalRect` | Still requires exactly 1920 x 1080 for the current profile and fails closed otherwise. |
| Clear/reselect ROI | Enabled | controller-local preference plus the two Region APIs above | Clearing stops admission; it does not create a default or scaled ROI. |
| Visual Profile | Enabled, Experimental | `pbdemodd3d11::CaptureDemodulatorConfig::visualProfileId` | Must match the Encoder because the provisional wire does not bind the profile. Mismatch is an erasure/failure, not auto-detection. |
| Capture age/queue/ring budgets | Read-only current defaults | `CaptureNormalizeConfig`, `CaptureDemodulatorConfig` | Fixed bounded production values are shown in Advanced telemetry; arbitrary overrides are not exposed. |
| PBTelemetry | Read-only live/report output | `pbtelemetry::TelemetryAccumulator` fed by bounded `CaptureDemodulatorResult` events | Capture/Bootstrap are recorded for accepted and erasure frames; FEC is FrameSequence-deduplicated; VerifiedEncodedGoodput is added only after WholeFileDigest and final publish. PreFecBER and UniqueVisualFPS remain unavailable when their required truth/pixel-digest coverage is absent. |
| FrameSequence cadence | Read-only diagnostic | application `VisualIdentityTracker` | Reports admitted FrameSequence FPS plus duplicate/reordered/gap/skipped counters. It is intentionally not named or used as pixel `UniqueVisualFPS`. |
| Resume | Not exposed | receiver core has resume primitives, but the current GUI product path has no authoritative end-to-end resume/storage binding | QSettings never stores protocol or decoder state. |

## Local preferences and run metadata

The following are application-only and have no protocol or acceptance effect:

- window geometry, last input/output directory, compression checkbox, selected explicit backend, selected monitor device name, and Advanced expansion through `QSettings`;
- `ChannelType`, remote provider version/mode, target/observed FPS, chroma mode, remote resolution/window scale, network note, observed bandwidth, and latency;
- `RunId` used to correlate independently exported Encoder and Decoder JSON reports.

They never change CRC, FEC, digest, descriptor, or final-publish acceptance.

## Evidence-only headless bindings

The following are real public CLI/tool bindings, but are intentionally not GUI product options:

| Binding | Runtime effect and boundary |
| --- | --- |
| `PixelBridgeDecoder --headless-receive ... --replay-output NEW_PATH --diagnostic-capture-only --replay-evidence-profile direct\|shape\|lf4` | Records a bounded selected-ROI Replay v2 through the explicitly selected WGC/DXGI backend. The evidence-profile option only writes the raster identity into the capture-only descriptor; it cannot select an LF4 product demodulator and is rejected outside capture-only mode. Bootstrap/demod/FEC/Receiver/publish remain disabled. Output, journal and report are create-only. |
| `PBRemoteVisualEvidencePresenter describe\|present --profile direct\|shape\|lf4` | Step-02 evidence presenter. It renders one deterministic valid Bootstrap/Transport raster in the existing PMv2 1920×1080 `DataWindow`, verifies exact single-monitor geometry before reporting `READY`, and repeatedly presents the same immutable texture at no more than 5 Hz. It has no file-transfer lifecycle and does not expose LF4 in GUI/CLI product selection. |
| `PBRemoteVisualReplayInspector --input PATH [--output NEW_PATH]` | Bounded receiver-only Replay v2 semantic inspector. It consumes the complete file with `ReplayV2Reader`, then applies the existing Bootstrap/profile reference decoder/QC-LDPC/padding/Transport CRC/Session identity chain and emits a sealed deterministic per-frame JSON classification. With no sender truth it reports false-accepted codewords as unavailable and never claims Outer/WholeFileDigest/final publish. |
| `PBPresentationGate --lf4-encoder-warp\|--lf4-encoder-hardware NEW_EVIDENCE_ROOT` | Step-09 right-monitor/no-activate native Gate. It binds Step-08 Golden Bootstrap/coded bytes, proves CPU raster BLAKE3 equals immutable GPU-source diagnostic readback, and requires a complete source-to-back-buffer copy on every repeated Present. GPU source readback is not screen capture. |
| `PBPresentationGate --lf4-production-encoder NEW_EVIDENCE_ROOT` | Starts the real hidden LF4 `EncoderRuntime` at 5 Hz with a create-only 1-byte source, proves two complete Carousel cycles, continuation after a test-local marker that is never passed into the runtime, correct dwell/repeat telemetry, and explicit bounded shutdown. It cannot be used as a product LF4 Decoder or file-transfer completion claim. |

## Hidden future capabilities

- `PB-RemoteVisual-LF4-X1` now has an experimental CPU/reference encoder/decoder, four-codeword QC-LDPC/Transport truth path, frozen Step-08 Golden, and a Step-09 production-code **Encoder-only** hidden enum candidate with immutable D3D11 source/repeat Present. It remains absent from GUI/CLI and is rejected by Decoder validation until the scaled GPU demod, capture, admission and field Gates pass; see `REMOTE_VISUAL_LOW_FPS_TECHNICAL_ROUTE.md` and `REMOTE_VISUAL_STEP09_ENCODER.md`.

- Offline MP4/NVENC generation and playback are hidden; Phase 4 is not implemented.
- `PBRealCaptureReplay` remains an existing bounded library/Gate artifact. The evidence-only headless capture and inspector bindings above are now real, but this first GUI still has no live record/replay controller binding; no replay button or fake setting is exposed.
- Direct-Level 4x4 is hidden from the file-transfer GUI. A physical-layer candidate exists, but it is not in the Phase-1 full-file Gate matrix.
- Multi-Segment files and arbitrary-size files are hidden/rejected.
- Receiver-to-Sender feedback, sender-side receiver progress, transfer-completion ETA, and automatic sender completion do not exist.
- Certified Profile labels are not shown. Both selectable current paths remain explicitly Experimental.
- Automatic capture-backend fallback, production arbitrary resize/resampling, tunable RemoteVisual policy thresholds, adaptive profile switching, and protocol-state persistence are not available. LF4 CPU reference accepts bounded continuous scale 0.5..2.0, but this is not yet a production GPU capability.
- Application-generated Control records must fit the current fixed Control window. A completed fragmented Control record is rejected fail-closed because this product path has no separate application binding for it.
