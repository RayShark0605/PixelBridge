# PBRemoteVisualLf4DynamicPresenter

`PBRemoteVisualLf4DynamicPresenter` is an evidence-only Windows sender for
dynamic LF4 field captures. It is deliberately separate from
`PixelBridgeEncoder` so that the Step 09 hidden LF4 candidate is not exposed
through the product GUI or headless CLI before the later admission Gate.

The executable does not implement another encoder. `broadcast` constructs an
`pbapp::EncoderConfig` for `VisualProfile::RemoteVisualLowFps` and invokes the
production `pbapp::EncoderRuntime`. The production `SenderFrameBuilder`,
Control records, DirectRepeat/Wirehair selection, Transport framing, QC-LDPC,
LF4 rasterizer, immutable source replacement, stable repeated Present, source
immutability check, and bounded shutdown therefore remain authoritative.

## Commands

```text
PBRemoteVisualLf4DynamicPresenter describe

PBRemoteVisualLf4DynamicPresenter broadcast \
  --source PATH \
  --origin X Y \
  --seconds 2..600 \
  [--logical-fps 1..5] \
  [--control-repetitions 1..64] \
  [--report NEW_PATH]
```

The defaults are 5 logical frames per second and one repetition of each of the
three Control records. `--report` is create-only and never overwrites an
existing file.

`READY` is printed only after the native Data Window satisfies its physical
contract, has an active frame, and has performed at least one production
immutable source replacement. `PROGRESS` reports the authoritative runtime
FrameSequence, Carousel position, source replacements, repeated Presents, and
generated logical FPS once per second. A successful `COMPLETE` additionally
requires at least one dynamic frame advance after `READY`, a stable source,
zero logical-dwell violations, explicit duration-based stop, and an empty
runtime error.

For the first receiver-only dual-computer pilot, use a 1 MiB RAW fixture at
5 Hz with `--control-repetitions 64`. The size forces production Wirehair V2;
the long Control prefix gives the receiver time to start after the sender has
reported `READY`. This pilot is evidence for dynamic pixel-channel survival.
It does not expose LF4 in the product Decoder, publish a recovered file, prove
WholeFileDigest, or close the Step 20 two-computer file-transfer Gate.

The first formal 120-second Windows Remote Desktop receiver-only pilot has now
passed with a bounded 10 Hz pre-readback Replay sampler and byte-identical
double offline inspection. Its exact environment, identities, commands,
counters, hashes, and non-certification boundary are recorded in
`docs/REMOTE_VISUAL_STEP09_DYNAMIC_RDP_PILOT.md`. This does not expose the
presenter or LF4 candidate through the product GUI/CLI.
