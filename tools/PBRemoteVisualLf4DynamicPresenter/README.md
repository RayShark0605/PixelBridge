# PBRemoteVisualLf4DynamicPresenter

`PBRemoteVisualLf4DynamicPresenter` is an evidence-only Windows sender for
bounded dynamic LF4 field captures. Step 17 now also exposes `remote-lf4`
through `PixelBridgeEncoder`; this tool remains as a reproducible, duration-
bounded evidence harness rather than a second product encoder.

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
  --protected-monitor DEVICE \
  --experiment-monitor DEVICE \
  --seconds 2..600 \
  [--logical-fps 1..5] \
  [--control-repetitions 1..64] \
  [--report NEW_PATH]
```

The defaults are 5 logical frames per second and one repetition of each of the
three Control records. `--report` is create-only and never overwrites an
existing file. Both monitor identities are mandatory, must resolve to distinct
non-overlapping active displays, and are revalidated before startup and once
per second. The complete 1920x1080 Data Window must remain inside the selected
`ExperimentMonitor` and outside the selected `ProtectedMonitor`; there is no
automatic monitor choice, window movement, or input automation.

`READY` is printed only after the native Data Window satisfies its physical
contract, has an active frame, and has performed at least one production
immutable source replacement. `PROGRESS` reports the authoritative runtime
FrameSequence, Carousel position, source replacements, repeated Presents, and
generated logical FPS once per second. A successful `COMPLETE` additionally
requires at least one dynamic frame advance after `READY`, a stable source,
zero logical-dwell violations, explicit duration-based stop, and an empty
runtime error.

For a receiver-only dual-computer pilot, use a 1 MiB RAW fixture at
5 Hz with `--control-repetitions 64`. The size forces production Wirehair V2;
the long Control prefix gives the receiver time to start after the sender has
reported `READY`. This pilot is evidence for dynamic pixel-channel survival.
This presenter alone does not publish a recovered file, prove WholeFileDigest,
or close the Step 20 two-computer file-transfer Gate.

The first formal 120-second Windows Remote Desktop receiver-only pilot has now
passed with a bounded 10 Hz pre-readback Replay sampler and byte-identical
double offline inspection. Its exact environment, identities, commands,
counters, hashes, and non-certification boundary are recorded in
`docs/REMOTE_VISUAL_STEP09_DYNAMIC_RDP_PILOT.md`. That sealed pilot used the
then-current hidden-profile binary and remains historical evidence; it is not
silently reinterpreted as a Step 17 public-profile run.
