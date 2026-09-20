# Automatic logs and performance/failure diagnosis

[简体中文](DIAGNOSTICS.md) | **English**

## 1. Different files, different responsibilities

| File | Purpose | Recovery input? |
| --- | --- | --- |
| `run-<id>.events.jsonl` | Default application diagnostic timeline | No |
| `run-<id>.summary.json` / exported RunReport | Final state, environment, resources, verification, warnings | No |
| `.resume` / `.part` | Verifiable resume state and unpublished file | Yes; never treat as disposable logs |

Defaults: `%LOCALAPPDATA%\PixelBridge\Logs\Encoder` and `...\Decoder`. Normal GUI and transfer-CLI runs enable logging automatically. Standalone research tools and argument-parser failures are not guaranteed to use this path. GUI smoke uses Qt test-mode storage, not real users' operational logs.

Open the directory from advanced options; transfer CLI prints final log status/location to stderr. Explicit journal/report options retain their create-only evidence behavior (consult the actual CLI help); automatic retention does not delete them.

## 2. Scope and overhead bounds

- Normally one snapshot every two seconds, plus state changes. Throttling happens before expensive serialization.
- Writes occur on the application polling side, with no per-frame log, capture/GPU/FEC-path flush, or unbounded log queue.
- Records include local monotonic `elapsedMs`, UTC `unixMs`, version/commit, run/session/profile, state/errors, cumulative counters, and process private/working-set memory.
- Timeline cap: 64 MiB or 48 hours per run. Truncation is reported; final summary is still attempted.
- On startup, retain seven old runs plus the current run per role. Prune only exact application-generated names, without recursion or following links. Active/unremovable files remain with a warning; concurrent/permission cases are not a strict global disk cap.
- Summary cap: 1 MiB. Log creation/write/flush failures do not change file acceptance; diagnostic failure and transfer failure are distinct.
- Power loss/forced termination may leave a partial last line and no summary; absence of a terminal report is not success.
- Sampling can miss sub-two-second operations. Absence of a sampled stage is not proof it never ran; use cumulative/final values too.

## 3. Encoder and Decoder fields

### Encoder

`preparedSourceBytes`/`preparationMilliseconds` distinguish prescan from display. `configuredLogicalVisualFps` is a setting; `generatedVisualFps`, submitted logical frames, and successful Present counts observe different stages. Pending replacement, repeated Present, and queue high-water values expose local pressure.

Segment ordinal/count, cycle count, outer block ID, frame sequence, airtime/visit parameters, and physical raster dimensions describe carousel and presentation behavior. None proves receiver completion.

### Decoder

- **Capture:** arrived/delivered/drop/expired/stale counters, epoch/backend reasons, queue/lease high-water, capture and visual stalls.
- **Recognition:** Bootstrap attempts/success, measured geometry, FEC/CRC failures, duplicate/gap identities. Gaps can include durable-ID lease skips; they are not directly network packet loss.
- **PAM4:** independent observation/erasure/slot counters and CPU readback/decode wall cost. Observations may contain duplicates and are not Unified lane metrics.
- **Recovery:** unique equations, identical duplicates, already-completed segments, readiness, verified segments and bytes.
- **Resources:** deferred/quota/OOM/rejections, active/peak decoder counts, reserved/total/per-instance budgets, resume bytes and generations.
- **Final acceptance:** digest, publication, final reopen and errorDetail. Correct file recovery and warning-free cleanup are distinct.
- **GUI active timing:** `activeReceptionTiming` starts at the first `SessionDescriptor` accepted and validated for the current Profile; it excludes the earlier wait but includes recovery, disk writes, digest, publication and final reopen. The completed GUI average is original file bytes divided by the frozen active elapsed time; it does not replace formal `runStarted/runEnded` or verified raw goodput.

## 4. Investigating a plateau

1. Match `sessionIdHex` and Profile on both ends, not just a filename. A run ID identifies a local run.
2. Find increasing `noSizeGrowthMs` and its `activity`; compare cumulative-counter deltas across the interval.
3. New unique symbols but flat size suggest segment repair or a saturated estimate, not necessarily capture interruption.
4. Increasing duplicate/completed symbols without new equations suggest carousel tail or repeated observations; prolonged lack of progress is not guaranteed normal.
5. Growing deferral/quota counts with occupied budgets support an admission bottleneck and a controlled memory-budget change.
6. Captured frames with low Bootstrap/FEC success suggest picture/geometry/visibility issues before RAM.
7. Fresh Encoder generation but no fresh Decoder identities can arise in the remote-video, capture, or recognition path. Two logs cannot uniquely identify every internal black-box cause.
8. An actual storage/verification operation marker explains a synchronous wait. A lingering `Verifying` state alone does not.
9. Inspect the final summary for acceptance and post-publish cleanup warnings; intermediate block success is insufficient.

## 5. Interpretation and privacy

Existing `PixelBridge.RunJournal.1` fields remain; new counters are additive. Automatic diagnostics add `logKind=OperationalDiagnostic`, monotonic time, terminal marker, activity, and plateau ages. This is neither a formal field Gate nor frame-replay data.

Automatic summaries retain RunReport and append an independent `operationalLog` status. Logging does not create formal measurement eligibility. The authoritative terminal report is sealed after worker cleanup to retain post-publish warnings.

Never subtract monotonic clocks across computers. Correlate Session/frame identities or compare each endpoint's own elapsed intervals. File-completion time through verified reopen remains the performance truth, not log-line count, display FPS, or estimated reception rate.

Decoder `activeReceptionTiming.startedUnixMilliseconds` is a correlatable UTC start only; `elapsedMilliseconds` is accumulated from the Decoder worker's local monotonic clock and frozen on stop, failure or completion. GUI active time and formal full-run time are therefore separate conventions. Older logs without this object do not provide active timing and must not be treated as if `runStarted` had already excluded the wait.

No payload/screenshots are recorded, but filenames, local paths, Session IDs, and machine/display details may appear. Review logs before sharing. These files are not a signed audit trail or sender authentication.
