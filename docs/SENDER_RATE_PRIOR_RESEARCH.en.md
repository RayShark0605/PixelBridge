# Encoder expected-reception-rate prior: feasibility assessment

[简体中文](SENDER_RATE_PRIOR_RESEARCH.md) | **English**

Date: 2026-10-21. **Research findings and proposed validation, not an approved implementation design, shipped feature, or measured speedup.** No product code, scheduler default, protocol, Profile, FEC, Golden, or persistence format changed. No product build, application run, or performance test was executed.

**Subsequent status:** The user then approved direct implementation without a prerequisite field experiment and chose PAM4-only scope. See the [implementation guide](SENDER_RATE_PRIOR.en.md) for that later task's configuration, algorithm and local-verification boundaries. This report retains the earlier research snapshot; its no-implementation/no-test statements do not describe the subsequent implementation task, and historical speedups remain inapplicable as new-feature results.

## 1. Objective, phase, and question

PixelBridge targets Windows x64, non-local, one-way whole-file transfer exclusively through pixels actually captured by Decoder. The objective is total time through `WholeFileDigest → safe publish → final reopen`, not fewer status messages, smooth progress, or higher sending FPS. Hidden payload/ACK channels are prohibited; memory, queues, reassembly, decoders, and recovery state remain bounded.

The main project phase remains **v1.0 reliability, display compatibility, and delivery maintenance**. The 2026-10-15 full static audit remains incomplete. This newly requested, narrow scheduling assessment neither resumes a broad field campaign nor transfers old candidate test results to the current worktree. See [status](PROJECT_STATUS.en.md) and [architecture](ARCHITECTURE.en.md).

The proposal is an optional Encoder input representing historical GUI average reception speed, for example `200–280 KB/s`. As broadcast time advances, concentrate more transmission opportunities near the predicted reception frontier and fewer on earlier data, reducing service spent on already completed segments and carousel tails.

The user proposed fewer opportunities for old data, not zero opportunities. That is compatible with retaining repairs. Hard prefix skipping below is a risk comparison, not an assertion about the request; the unresolved engineering questions are how much to reduce service, how to revisit, and how to prevent starvation under a wrong prior.

**Conclusion: conditionally feasible and worth a narrow experiment, but do not treat bytes before `rMin × t` as completed.** Investigate a weak rate prior combined with an actual sender-service ledger, bounded active windows, and non-starving repair revisits. This can improve scheduling efficiency in some environments; it cannot reveal the real receiver state or guarantee faster, stall-free delivery everywhere.

## 2. Inputs and unresolved incident evidence

### 2.1 User-provided observations

- The user confirmed a fresh reception with Decoder started first and Encoder sending at **30 Hz**.
- The screenshot shows PAM4, 97.9%, approximately 210.0 MB received and verified, active elapsed time `00:15:31`, approximately `230.8 KB/s`, an estimated 21 seconds remaining, a repeated/already-completed-segment carousel message, and 20 seconds without size growth. Values are rounded GUI observations; current GUI MB/KB use powers of 1024.
- Paired incident logs are temporarily unavailable. The user asked to finish the route assessment without waiting for them.

The incident's Session, exact file length, endpoint build identities, per-segment state, and final outcome were not supplied. We cannot identify the missing segment, classify the repetition proportions, establish a global wrap, or prove that the screenshot binaries implement precisely the source inspected here. Source explains possible mechanisms; it is not incident-runtime proof.

### 2.2 Reading and cross-check scope

All 71 first-party maintained narrative documents at task start were read, together with two protected historical reports. The latter were read-only, not modified or brought into Git. Scope includes bilingual design/status/guides, profiles, protocol, scheduling, recovery, telemetry, presentation/capture, Goldens, fuzzing, release, and tool documentation.

Relevant archived scheduling research, short-initial-visit records and result JSON, the final Qt candidate handoff, and the static-audit stop handoff were read selectively through the maintained index. Build/install/package mirrors and every archived raw run/document were not individually re-read. This is not a full source audit or a re-audit of all historical experiments.

Local evidence is retained under `artifacts/speed-prior-evaluation-20261021/`: `document-reading-coverage.json`, `research-evidence.json`, `baseline/`, and `sources/source-manifest.json`. These are maintainer-local records, not public repository downloads.

## 3. Meaning of the screenshot message

Relevant sources: [DecoderActivityTracker::Observe](../apps/common/decoder_activity.cpp), [activity text](../apps/common/operational_log_qt.cpp), [receiver ingress](../libs/PBReceiver/src/receiver_ingress.cpp), and [ReceiverPipeline](../apps/common/local_desktop_runtime.cpp).

`WaitingForCarousel` is an observational heuristic based on recent counter changes, not a missing-range list. After higher-priority recovery, capture/visual stall, and recent resource-wait branches, it requires no recent size growth, no new unique-symbol/completed-segment progress within five seconds, but a repeated/already-completed counter increase within five seconds. Thirty seconds without useful progress selects a different message first.

| Counter | Meaning | Consequence |
| --- | --- | --- |
| `outerIdenticalDuplicateSymbols` | The same equation identity and content arrives again | May involve repeated observations, tail-slot padding, or DirectRepeat; changing segment priority alone may not solve it |
| `outerAlreadyCompletedSymbols` | Transport addresses a segment Receiver has completed | Even a fresh repair ID has no remaining value for that segment; reallocating service may help |

Continued service to recovered segments is therefore plausible. A specific contiguous old byte range is not established. The duplicate-check code is not itself the cause of the wait; removing the message or deduplication creates no new information.

`FormatDecoderProgress` divides `max(estimatedReceivedRawBytes, verifiedRawBytes)`, bounded by total length, by active reception time. `EstimateReceivedRawBytes` estimates in-flight raw bytes from unique-symbol counters and completed-segment compression ratios. Neither exposes an exact per-segment deficit or contiguous verified prefix. Static-audit issue SA-L03 concerning large/resumed progress estimation remains unchanged; it must not be asserted as the cause of this approximately 210 MB fresh reception.

ETA is remaining estimated bytes divided by the cumulative average. It does not forecast the next useful repair visit, so 21 seconds is not a completion deadline.

## 4. Existing sender behavior

Cross-checked sources: [SenderFrameBuilder](../apps/common/local_desktop_runtime.cpp), [scheduler](../apps/common/sender_carousel_scheduler.cpp), [constants](../apps/common/sender_carousel_scheduler.h), and [GUI configuration](../apps/common/gui_visual_mode.h).

| Current source path | Behavior |
| --- | --- |
| PAM4 GUI | Default 7 MiB raw segments; ten Transport slots of at most 1629 payload bytes per data frame; inactive data slots during Session/Manifest refresh |
| Active sender set | Normally at most six segments, with PAM4 sweep rotation to reduce fixed sampling-phase starvation |
| At most 12 segments | Whole-window barrier path; a test here does not establish large-file behavior |
| More than 12 segments | Per-segment graduation driven by locally committed equation counts, not receiver acknowledgments |
| First ordinary PAM4 large-file visit | Approximately `2K` cumulative target, where `K = ceil(EncodedSize / OuterBlockBytes)`. Initial scheduler pass is systematic plus approximately 10% repair and a guard; subsequent local repair fills the graduation distance |
| Subsequent non-spatial global wraps | Add `floor(K × 25%) + 1` to the run-local cumulative target per wrap; this is a target increment, not a credit for leased IDs |
| Cadence classes and override | The generic scheduler distinguishes ≤15 Hz and >15 Hz repair budgets, but the large-file builder may override them with the remaining graduation distance |
| Restart | Durable repair/sequence high-water prevents reuse, while the run-local service ledger is conservatively rebuilt. Unused leases are not transmitted service |

Repair-only passes can recover a receiver that missed systematic data only if enough service reaches the appropriate segment. Fresh IDs identify different equations, not receiver need. DirectRepeat retains its fixed systematic-ID contract; it does not acquire invented fresh repairs.

### 4.1 A 30 Hz scale check

For full payload, RAW, more than 12 segments, ordinary PAM4 first visits, and ignoring control/loss/tail-window effects:

```text
Transport payload capacity = 30 × 10 × 1629 = 488700 B/s ≈ 477.25 KiB/s
Fresh-raw frontier scale with about 2K service ≈ 477.25 / 2 = 238.62 KiB/s
```

A receiver may finish a window early while the sender continues its conservative service budget. **The proximity of 238.62 and the screenshot's 230.8 is only a hypothesis-generating observation.** It is not a strict whole-file goodput ceiling: compression, startup/tail effects, control, actual submission cadence, and capture losses matter, and Decoder may finish before the sender exhausts its final-window budget.

In particular, `230.8 / 477.25` does not measure a roughly half-loss channel. The GUI average already includes scheduling waste, compression, waiting, and storage. The available figures do not separate undelivered data from delivered data targeting completed segments.

## 5. Potential value and failure modes

### 5.1 When it can help

With a representative prior, fresh reception, and substantial service to completed segments, earlier service to fresh/near-tail segments and less excess service to well-covered earlier segments can improve useful utilization of arriving information. This does not increase the physical video-channel capacity.

If distortion, repeated video pictures, capture/demodulation capacity, admission budgets, or synchronous persistence dominate, prioritization does not directly fix them. Advancing faster can instead fill the receiver with unfinished decoders and delay older repairs.

### 5.2 Total received bytes are not a prefix

Consider ten equal segments. Receiver A completes 0–8 and needs 9; B completes 1–9 and needs 0. Both have identical totals and averages but opposite urgent needs. The same historical rate range cannot distinguish them.

Thus `rMin × t` is not a guaranteed completed-prefix lower bound. An empirical range observed most of the time is not a guaranteed service curve. Removing old repair opportunities can turn a short plateau into very long or incomplete delivery.

### 5.3 Clock and byte-domain mismatches

- Encoder Start includes prescan; Decoder active time starts at the first Profile-validated SessionDescriptor. Their origins differ, and cross-host monotonic clocks cannot be directly subtracted.
- A model may explicitly handle known local preparation/presentation pauses, but Encoder cannot necessarily observe remote freezes, receiver pauses, or channel degradation.
- GUI bytes are raw bytes. Service cost depends on each segment's encoded size, K, codec, and redundancy. Equal raw offsets do not imply equal airtime.
- Historical GUI averages include the old scheduler's inefficiencies. Treating them as independent channel capacity risks fitting those inefficiencies back into the new budget.

### 5.4 Uncertainty growth and EOF

As arithmetic only, assume model time `t=931 s` and `200–280 KiB/s`:

```text
Predicted frontier interval ≈ [181.84, 254.57] MiB
Width ≈ 72.73 MiB; after one hour, width = 281.25 MiB
```

This already exceeds a normal 6×7 MiB active sender window. The interval can guide priority, not require all enclosed segments to remain encoded/decoded in memory. Once it passes EOF, permanently pinning transmission to the final segment would neglect older holes. A nonzero random selection probability alone is also weaker than an explicit deterministic service/revisit bound.

## 6. Existing project evidence: both benefit and regression

These were re-read, not re-executed:

- `artifacts/nonlocal-stall-20260913-2111/WORK_LOG.md`, sections 21.25 and 21.29;
- `candidate-o-original-gpu-100-comparison.json` in that directory;
- current [regression source](../tests/PBApplication/test_unified_encoder_workflow.cpp), `Short initial airtime retains full repair recovery without receiver feedback`.

| Historical comparison | Outcome and boundary |
| --- | --- |
| Explicit spatial GrayFast, 100000000 B, initial budget 100%→65% | One real-channel run changed 464746 ms to 327833 ms, approximately 29.46% shorter; sampled repeat-only time changed 184480 ms to 48190 ms, with digest/publication/reopen success |
| Actual FEC/Receiver/storage probe, retaining one in four frames, joining at zero | 12349 frames at 100%, 20173 at 65%: approximately 63.36% slower |
| Same quarter model, joining at frame 15000 | Total frames 27505→27645, no gain; orphan/resource waits must remain visible |

Reducing excess first-visit service can work, but has concrete counterexamples. **These do not validate a rate-range feature or quantify a current PAM4 gain.** Spatial interleaving, GrayFast budgets, and historical candidate identity matter; neither 65% nor 29.46% transfers directly to PAM4. The old GrayFast `<12000` late-join performance failure remains; it was not rerun or closed here.

## 7. Online primary references and limits

Checked on 2026-10-21. Technical conclusions rely on primary standards, original papers, and upstream source, not search aggregators. No external implementation was installed or substituted into the product.

| Primary reference | Relevant lesson | Invalid extrapolation |
| --- | --- | --- |
| [RFC 3453, sections 1 and 1.1](https://www.rfc-editor.org/rfc/rfc3453.html#section-1) | Feedback-free carousels incur cycle waits and repetition; expandable FEC reduces identical-equation retransmission | Fresh equations for a completed independent segment remain useful |
| [RFC 5775 ALC](https://datatracker.ietf.org/doc/html/rfc5775#section-1.2), [RFC 6726 FLUTE](https://www.rfc-editor.org/rfc/rfc6726.html) | Feedback-free object/file delivery is an established problem domain | PixelBridge may import network channels, multicast, out-of-band descriptors, or ACKs |
| [RFC 6330, sections 4.4.1–4.4.2](https://www.rfc-editor.org/rfc/rfc6330.html#section-4.4) | Objects use independently encoded source blocks; enough symbols must address the right block | A sufficient whole-file aggregate completes every segment, or RaptorQ replacement is needed |
| [RFC 8680](https://datatracker.ietf.org/doc/html/rfc8680), [RFC 8681, section 3 and Appendix C](https://www.rfc-editor.org/rfc/rfc8681.html#section-3) | Encoding windows, redundancy, latency, and decoder working set must be dimensioned together | Cross-symbol linear sliding-window coding is the same as segment-service scheduling, or real-time expiry is acceptable for exact file recovery |
| [Bulut et al., 2016](https://link.springer.com/article/10.1186/s13634-016-0316-4) | Slow independent-block recovery affects whole-file time; interleaving and client-rate limits matter | Wi-Fi performance numbers transfer to remote video, or full-file interleaving can ignore decoder slots and persistence |
| [Jones, Chatzigeorgiou and Tassi, 2014](https://andreatassi.github.io/publications/Performance_Assessment_FC.pdf) | Progressive and complete-message recovery are different objectives; sliding step size trades them off | Smoother progress guarantees faster completion, or their encoder is an in-place Wirehair option |
| [Pinned libcimbar snapshot](https://github.com/sz3/libcimbar/blob/156fbb9c675ca9641f6559922b7025a0f2ee8125/src/lib/fountain/fountain_encoder_stream.h) and `FountainEncoder` | The examined path reads/retains a complete stream and generates successive block IDs | This whole-stream residency meets PixelBridge's large-file active-set memory requirement, or implements this proposed prior |
| [Project-pinned Wirehair V2 contract](https://github.com/catid/wirehair/blob/067ca7cdb66aed424ec23f97557429bf791c6f0c/V2_WIRE_PROFILE.md) | Exact descriptor/packet ID binds equation identity; scheduling stays separate | Reprioritization may change a payload under the same ID, relax dimensions/resources, or omit digests |

Also consulted: [SDMB reliable transport](https://new.eurasip.org/Proceedings/Ext/IST05/papers/541.pdf) for carousel/FEC resource trade-offs, and [Lin et al., 2012](https://people.bu.edu/staro/CISS_2012.pdf) for rateless dissemination scheduling. Their satellite/multi-cell interference models are not used to calculate a PixelBridge speedup.

## 8. Alternatives and recommendation

These are research candidates, not approved product behavior.

| Alternative | Assessment |
| --- | --- |
| A: Hard time-based prefix skipping; transmit only inside the predicted band | Not recommended: old holes, inaccurate priors, late join/interruption, and EOF are unsafe |
| B: Prior affects bounded first-visit/graduation service only; complete repair route remains | Cheapest isolated mechanism experiment; reuse the ledger, but do not turn GUI average directly into loss rate |
| C: Weak frontier weights, bounded service budgets, deterministic repair revisits | Recommended research direction; more robust than A, but must prove working-set and tail behavior rather than merely add an input field |
| D: Cross-segment sliding RLNC/new whole-file fountain | Not selected now: protocol/FEC/recovery/resource redesign, not the minimum variable |

Start with B to isolate the mechanism, then require evidence that C's time-frontier information adds value beyond sensible service budgeting alone. A viable C should satisfy:

1. **Prior, not receipt.** Optional historical raw-byte average, not a bandwidth guarantee, contiguous ACK, or completed position. Missing/invalid inputs must not silently select another policy.
2. **Actual service first.** Count truly committed systematic/fresh repair equations and last-service age per segment, charging encoded work. Leases, failed Submit, and repeated Present do not earn new-service credit.
3. **Soft frontier.** Use bounded weights and front/back uncertainty margins; choose primarily from a bounded active set rather than allocating codecs in proportion to rate×time.
4. **Explicit old-hole service.** Deterministic bounded revisits and tested sufficient visits; occasional tiny repairs may leave many decoders occupied. A sender-service bound is not a guarantee against arbitrary erasures.
5. **EOF is not completion.** Use only a finite tail preference, continuing whole-file repair coverage; do not remain pinned to EOF or stop at a predicted ETA.
6. **Correctness remains independent.** Preserve descriptors/encoded bytes, conflict handling, Wirehair dimensions/error handling, DirectRepeat, resources, digests, publication, and reopen. Receiver need not know the prior.
7. **Observable and reversible.** A future implementation should start explicit opt-in and report requested/effective prior, policy, estimated frontier/uncertainty, service, and revisit age, never predicted receiver progress. Restart/local-pause clock and setting lifecycle belong in its later approved design.

No untested 80/20 allocation, new default rate, or UI interaction is specified here. Allocation shares, revisit intervals, and risk margins must come from counterexamples and cost measurements, not arbitrary constants.

## 9. Proposed validation sequence and stopping conditions

None of these experiments was executed in this assessment.

### 9.1 Diagnose the mechanism separately

If incident logs become available, match Session/Profile/build, then compare interval deltas for unique/identical/already-completed symbols and verified bytes/segments; active/peak decoders, deferral/resource/orphan pressure, checkpoint activity; sender segment/cycle/new submissions and receiver unique identities, FEC/CRC, and capture/visual stalls.

Low-frequency logs may not preserve every missing segment or transmitted frame. Admit insufficient observability if necessary; a later bounded payload-free trace may help, but must not be described as a complete replay.

### 9.2 Headless comparisons using the real recovery chain

Freeze current source/dependency/test identities, use independent outputs, compare baseline, B, and C, changing one scheduling variable at a time. Sender must not read test receiver state; test truth is only for post-run assessment.

Cover zero/one/six/twelve/thirteen and many-window segment counts; RAW/highly compressed/mixed/DirectRepeat; 30 Hz PAM4 and independently Wide; different burst/periodic sampling at equal average rate; an early stranded segment; late joins, short/long outages, substantially high/low/worsening priors; sender/receiver restart; eight-slot defaults and explicit finite-budget pressure. Preserve all existing failures and worst cases, not only favorable receiver-first samples.

Time-only/ideal-K simulations can reject poor candidates cheaply, not prove Wirehair rank, admission, persistence, digests/publication/reopen. Reuse actual FEC/Receiver/storage probes and add corresponding PAM4 large-file-branch coverage. GrayFast probes are not a substitute.

### 9.3 Matched actual-pixel validation last

Under separately determined field scope, fix package, file, Profile, 30 Hz, raster/ROI, presentation backend, memory budget, and remote settings, alternating baseline/candidate order. Frequent screening files remain no larger than 100 MB and must actually exercise `>12` segments. Small-file results do not establish large-file/tail performance. Every accepted output needs length, whole-file digest, safe publication, final reopen, and an independent digest; retain failures, timeouts, and cleanup warnings.

Primary results are full-run final-reopen completion time and verified goodput. Secondary results include tail, maximum/cumulative no-new-equation and no-size-growth intervals, completed-segment waste, control overhead, and active/memory/disk cost. Offline multi-seed P50/P95 may characterize a model; small field samples should report individual results/ranges, not invented confidence.

**Do not promote** a candidate with correctness/conflict/resource/resume regression, old-segment starvation, unexplained severe mismatched-prior/late-join tails, improvements confounded by file/Profile/FPS/budget changes, or no demonstrated incremental benefit of C over B. No fixed speedup is promised before measurement.

## 10. Delivery boundary

This assessment delivers document/source cross-checks, primary-source research, conditional conclusions, and a validation proposal. **It does not implement expected-rate scheduling, establish the incident's unique root cause, close the old static audit/late-join failure, or produce a new measured PAM4 speedup.**

Retain the user's direction of less old-data service and more service to predicted need, adding explicit, bounded and sufficiently large repair visits without turning a prior into a completion assertion. First isolate conservative airtime waste, then establish the incremental value of the time frontier.
