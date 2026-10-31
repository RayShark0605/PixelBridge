# PAM4 expected reception-rate prior

[简体中文](SENDER_RATE_PRIOR.md) | **English**

Date: 2026-10-21. **Status: user-approved optional implementation; local build/regression and field performance are separate acceptance layers.** This is not a new protocol, FEC or acknowledgement mechanism. The main project phase remains v1.0 stability, display compatibility and delivery maintenance; previous audit and field limitations remain open.

## 1. Use and rollback

- Both the Encoder main window and right-click transfer dialog offer the expected reception-rate prior, **PAM4 only**. It is unchecked on a fresh installation.
- Select PAM4, enable it, then enter the historical Decoder raw-file average, e.g. `200–280 KB/s`, where `1 KB = 1024 B`. It is neither video bandwidth, encoded-FEC throughput nor a rate limiter.
- Both endpoints still select PAM4. **PAM4 Wide, Standard and Gray Fast are not supported in this first version.** Other modes disable the controls and do not apply the prior, while retaining the PAM4 preference.
- It takes effect only with **more than 12 segments**, on the existing graduation path. The small-file whole-window barrier is unchanged. With the default 7 MiB raw segmentation and no override, this means more than 84 MiB of original file bytes, irrespective of compression ratio.
- Bounds are integers in `1..1048576 KB/s`, inclusive, with minimum no greater than maximum. Equal endpoints are allowed. Invalid enabled input prevents Start; it is not silently swapped, clamped or replaced. Invalid persisted settings visibly disable the feature.
- Settings are shared between the two entries and locked during preparation, broadcast and shutdown. Stop and uncheck to restore the original scheduler. No CLI switches were added.
- Predicted EOF does not stop transmission. Stop after Decoder completion, required digest checks and final reopen. An explicitly configured existing timeout remains independent.

## 2. Implementation rationale

The user approved direct implementation with later manual field validation and chose PAM4-only scope. This follows the weak-prior direction in the [research assessment](SENDER_RATE_PRIOR_RESEARCH.en.md), without arbitrary segment jumps, additional active codecs, cross-segment coding or ACKs.

The predicted band is `[rMin × t, rMax × t]`, expressed in raw-file bytes and clipped to file length. Verified descriptor `rawOffset/rawSize` locate a segment; encoded block count `K` measures its service cost. A broad band does not load every intersecting segment into memory.

**Earlier segments are neither marked received nor skipped.** Segment order, the six-active-segment bound, rotation and control contracts remain unchanged. Adjusted per-visit budgets hand airtime back sooner from old segments to subsequent ones. Segments outside the active window cannot be jumped over. This is soft budget alignment, not exact tracking of a moving byte cursor.

## 3. Reproducible budget rules

These are additional submitted-equation targets for one window admission. Fractions round up, with a minimum of 16. They are not receiver demand or successful-delivery guarantees.

| Position | First visit with no committed service in this run | Later global revisit |
| --- | --- | --- |
| Entire segment before the lower frontier | `5K/4` | `K/16` |
| Touches/intersects the predicted band | `7K/4` | `K/2` |
| Entire segment beyond the upper frontier | `2K` | `K/8` |
| Deterministic all-file safety-repair sweep | `2K` | `5K/4` |

- Relative global passes are numbered 0, 1, 2, ... from this Start. **Passes 3, 7, 11, ... are safety sweeps.** Every segment is traversed without consulting receiver progress. Once the lower frontier reaches EOF, later visits use safety budgets rather than staying at the tail.
- The sender guarantee is nonzero service on every full sweep and a substantial safety sweep after at most three ordinary full sweeps. This is not a bound in seconds or recovery under arbitrary loss; source reads, submission cadence and channel losses still matter.
- Each admission anchors its target to this run's actual committed equation ledger, never an ID lease. A completed scheduler pass may lower the remaining visit target as the frontier moves, but cannot chase an ever-increasing target indefinitely.
- Passes finish transactionally. The first systematic-plus-repair pass is never truncated; repair overrides remain at least 16 and physical tail slots can be padding duplicates. Actual equations may exceed the table. The existing eight-pass-per-admission cap remains.
- DirectRepeat retains fixed `[0,K)` IDs and completes whole passes; it cannot fabricate fresh repair IDs or necessarily match the proportions for tiny segments.
- **The table is not used while disabled.** Original large-file cumulative `2K + approximately 25%K per wrap` targets, small-file behavior and other modes remain intact.

The coefficients are isolated first-version heuristics, not an inferred loss probability or measured optimum. An incorrect prior may delay a genuinely missing segment and make completion slower; retain a disabled comparison.

## 4. Model clock and resume

- Time begins at the first **successfully submitted logical raster** in this run, excluding prescan, codec preparation and previous waiting. Preparing/retrying a pending frame earns neither time nor equation credit.
- The clock uses monotonic intervals between successful submissions, capping each local gap at one second. A longer local pause contributes that one second, not its entire wall-clock duration; subsecond waits remain included. This is not the Decoder activity clock and cannot observe remote freezes.
- Stop/restart conservatively resets model time and service credit. Durable repair high-water remains independent and prevents reuse; unused leases are not credited as shown equations. No historical predicted prefix is skipped.
- The prior is absent from Session/descriptor and source-preparation identity. It may change after Stop within a compatible Session without changing exact encoded bytes, descriptor, ID-to-equation mapping or receiver resume format.

## 5. Logging and interpretation

Encoder `events.jsonl` and exported/automatic run reports add `senderRatePrior`:

- `requested`: `minimumKibPerSecond/maximumKibPerSecond`, or `null` while disabled;
- `effective` / `policy`: actual policy, `original` when inactive; it remains false before preparation completes or with at most 12 segments even if configured;
- `activeNanoseconds` / `clampedSenderGaps`: model time and capped local gaps;
- `predictedLowerRawBytes/predictedUpperRawBytes`: predictions, **not receiver progress**;
- `beforeFrontierEquations/nearFrontierEquations/aheadOfFrontierEquations/safetyRepairEquations`: scheduled equations classified at successful submission time; excludes control slots, padding duplicates, failed Submit and repeated Present. They are not proof that the Decoder received them.

`receiverAcknowledged` remains false and true Encoder `receiverProgress/receiverEta/verifiedGoodput` remain null. Evaluate final completion time, Decoder verified bytes, WholeFileDigest, safe publication and reopen, not just fewer messages or a moving predicted band.

## 6. Source and verification

- [Pure policy/clock](../apps/common/sender_rate_prior.h), [implementation](../apps/common/sender_rate_prior.cpp);
- [Production sender](../apps/common/local_desktop_runtime.cpp), [shared UI/settings](../apps/PixelBridgeEncoder/encoder_rate_prior_options.h);
- [Policy boundaries](../tests/PBApplication/test_sender_rate_prior.cpp), [no-window real FEC/receiver/storage recovery](../tests/PBApplication/test_pam4_rate_prior_runtime.cpp), [offscreen controls](../tests/PBApplication/test_encoder_rate_prior_options.cpp), [JSON reports](../tests/PBApplication/test_rate_prior_reporting.cpp).

Local materials live under `artifacts/pam4-rate-prior-implementation-20261021/`, separating baseline copies/hashes, RED/build/test logs and task-only differences. Execution results belong in [current status](PROJECT_STATUS.en.md) and the [evidence index](EVIDENCE_INDEX.md). There is no real remote-transfer, field speedup, long-duration or stall-free acceptance, nor a confirmed root cause for the screenshot session.

No experiment is required now; try an ordinary transfer when conditions permit. For an enabled/disabled comparison, use the **same candidate build**, file, mode, FPS and channel, with a separate fresh Encoder session-cache directory and Decoder output directory for each fresh-reception run. This avoids conflating resume state or first systematic/repair differences with the prior. Start Decoder first and retain both logs and the final completion outcome. This candidate includes Qt/static-audit modifications already present at task start; do not attribute every difference from an old release to the prior.

## Optional file-feedback relationship (2026-10-21)

[File-feedback assistance](FEEDBACK_ASSISTED_TRANSFER.en.md) supports all four modes and defaults off, without changing this contract's wire/Profile/FEC/sampling/digest/publication rules. Real durable confirmations govern subsequent selection; the rate prior remains optional Sender-side budgeting for unconfirmed service, restored with all-file coverage on failure. Neither is independent Sender-side receiver verification or field speedup certification.
