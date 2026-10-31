# Delayed read-only file feedback for visual transfer: feasibility assessment

> Follow-up: this preserves the research-stage scope and online evidence. The user later approved a separate design and implementation; see [current implementation/setup/evidence limits](FEEDBACK_ASSISTED_TRANSFER.en.md). Research-stage no-implementation/no-build statements below are not current implementation status.

[简体中文](FEEDBACK_ASSISTED_TRANSFER_RESEARCH.md) | **English**

Date: 2026-10-21. **Research assessment and recommendations for a later design, not an approved implementation specification, an implemented feature, or a measured speedup.** This task changes no product code, scheduling default, visual wire format, Profile, FEC, Golden or resume format. No product build, execution or transfer test was performed.

## 1. Conclusion and confirmed requirements

**Feasible and worth developing as an explicitly optional auxiliary capability.** Even feedback delayed by several seconds can identify specific reception holes that a rate prior cannot infer, reducing service to completed segments and whole-file carousel waits, provided updates eventually become visible at acceptable cost.

> Keep all forward file payload in captured visible pixels. Return only bounded receiver state. Use segment-level acknowledgments, pipelined service and failure fallback, not per-frame stop-and-wait or percentage-based prefix skipping.

The user's requirements for this assessment are:

- Decoder can write a local directory or dedicated file; Encoder can read its corresponding mapped location but cannot write it. Changes may appear immediately or seconds later.
- Each endpoint selects its own view of that location, followed by the ordinary receiver-first workflow. Disabled operation remains unchanged.
- Confirmed during this task: **one Encoder serves one designated Decoder**, not a group of receivers.
- Confirmed during this task: **Encoder should stop safely after Decoder has fully verified the file**, rather than only optimizing scheduling.
- This task evaluates the idea and maintains documentation; it does not implement it. The modules, fields, parameters and stages below remain recommendations. Confirmation of intended behavior does not approve an implementation specification.

The actual mapping's visibility-delay distribution, cache/replacement/cancellation semantics and performance are unverified. No fixed speedup, universal improvement or stall-free guarantee follows.

## 2. Project objective, stage and reading coverage

PixelBridge targets correct whole-file visual transfer over non-local Windows x64 remote desktops. The primary objective is total time through WholeFileDigest, safe publication and final reopen verification, not Encoder FPS, smooth progress or theoretical capacity.

The project remains in **v1.0 reliability, display-compatibility and delivery maintenance**. Today's separately approved PAM4 expected-rate prior has been implemented, but its local builds/regressions establish no real-channel gain. The 2026-10-15 full static audit remains incomplete, with the historical Gray Fast late-join performance failure retained. See [status](PROJECT_STATUS.en.md), [prior implementation](SENDER_RATE_PRIOR.en.md) and [evidence](EVIDENCE_INDEX.md).

The task read all 78 first-party maintained documents present at its start, including bilingual guides/design/status, profiles, protocol, scheduling, recovery, telemetry, capture/presentation, Goldens, fuzz, releases, tools and the earlier Qt specification/plan. Two protected historical reports were also read in full, without modification or tracking; the static-audit stop handoff and today's candidate-package delivery record were read as targeted evidence.

Build/install trees, release/source-copy duplicates and every raw historical artifact were not reread individually. This is not a full code audit or a replay of all experiments. Reading coverage, baseline hashes, source locations and this task's document-only differences are retained locally under artifacts/feedback-assisted-evaluation-20261021/.

## 3. What channel this would create

~~~text
Computer A: Encoder -- visible rasters / existing remote desktop --> Computer B: Decoder
                         file payload travels only this way

Computer A: read mapped file <-- delayed state snapshots -- Computer B: write local feedback
                         receiver state only, no file bytes or decoded blocks
~~~

A read-only view does not prevent information flowing from B to A: B changes the state, and A observes it. A cannot use that path to acknowledge, lock, clean up or issue commands on B.

The [current architecture](ARCHITECTURE.en.md) excludes receiver ACKs as well as hidden payload transport. This therefore requires an openly named **optional reverse status-control plane**, not silently relabeling operational logs as diagnostics while using them for decisions. Researching that user-requested exception is in scope; implementing it later requires explicit review of the corresponding AGENTS/architecture, guide, telemetry and compatibility boundaries.

PBBridge remains a development-orchestration tool. Do not reuse its listener, command tree, script execution or remote stop. Do not treat .part/.resume, recovered blocks or complete logs as the feedback protocol. Decoder still obtains file bytes exclusively from pixels it actually captures.

## 4. Why this is more direct than a rate prior

The current SenderFrameBuilder, scheduler, ReceiverIngress, durable storage commit and EncoderSessionStore were cross-checked rather than inferred from comments alone.

| Existing mechanism | Implication |
| --- | --- |
| Normally at most six resident sender segments; graduation above twelve segments | Preserve a bounded working set, but do not claim a feedback interface already exists |
| Graduation uses successfully submitted equations; ordinary PAM4 first visits target approximately 2K | A decoded segment may continue receiving service; sender service is not receiver completion |
| Later repair IDs can be new while their segment is already complete | Fresh equations can still be useless; eliminating identical-ID repeats is not sufficient |
| The prior uses raw-byte rates and a sender-local model clock to soften visit budgets | It predicts demand but cannot acknowledge actual reception |
| Receiver has an authoritative storedSegments set and durable completion ordering | Export specific confirmed segments instead of inferring them from a progress bar |
| Public ReceiverIngress resource telemetry is primarily aggregate | Exact active-segment/demand summaries need a deliberate read-only observation interface; they are not an existing production API |

Source entry points: [runtime](../apps/common/local_desktop_runtime.cpp), [scheduler](../apps/common/sender_carousel_scheduler.cpp), [ReceiverIngress](../libs/PBReceiver/include/pbreceiver/receiver_ingress.h) and [Session store](../apps/common/encoder_session_store.cpp). Local records retain exact hashes and line locations. These are static mechanism findings, not evidence that an older field binary executed these exact bytes.

If 99 of 100 segments are complete and segment 8 is missing, exact feedback can focus service on segment 8. A value of 99% cannot distinguish that state from a missing final segment. This extends the non-contiguous-prefix counterexample in the [prior assessment](SENDER_RATE_PRIOR_RESEARCH.en.md).

## 5. Why several seconds of delay need not defeat the idea

### 5.1 Avoid per-frame or per-segment stop-and-wait

Keep sending to a finite set of unconfirmed segments while waiting for feedback. An arriving snapshot changes later opportunities; it does not gate every frame.

For scale only, suppose a segment is 7 MiB and useful raw-file service is 250 KiB/s. A segment takes about 28.672 seconds. Adding a five-second wait after each segment would reduce this ideal model's efficiency to about 85.15%. Pipelining avoids serializing every feedback delay onto the critical path. The assumed rate is neither a current measurement nor an encoded/FEC rate.

Absorbing a feedback delay requires approximately R×D additional serviceable work: 1,250 KiB in that example. This shows that seconds of delay are not inherently prohibitive; **it does not prove six segments suffice under all loss, compression and resource conditions**. Current segment-service time, decoder capacity, storage and correlated frame loss also matter. A bandwidth-delay calculation alone must not increase memory budgets.

### 5.2 Benefit conditions and counterexamples

Likely beneficiaries include substantial service to completed segments, overly conservative residency, a small number of tail holes, and old partial segments that should be finished to release decoder slots.

Benefits may be small or negative if mapping delay approaches a whole carousel, feedback competes with video or disk I/O, publication blocks reception, or unreadable pixels, frozen video, repeated observations, CPU demodulation, synchronous storage or final hashing dominate. Admitting fresh segments too quickly can make resource deferral worse.

Feedback does not increase payload/frame for the selected Profile or repair unreadable pixels. It can reduce scheduling waste and waits, not create physical channel capacity.

## 6. Candidate approaches

| Approach | Assessment |
| --- | --- |
| A: return only percentage, total bytes, speed and ETA | Useful for display, insufficient for precise holes, skipping or termination |
| B: durable completed-segment set, optional bounded active/demand hints, existing FEC pipeline | **Recommended**: directly targets wasted airtime while preserving recovery and file-safety chains |
| C: per-equation ACK/selective repeat, or feedback-aware LT/Raptor/RLNC replacement | Not recommended initially: much larger state/cache and wire/FEC changes than necessary |

B can be tested incrementally: first demonstrate the value of confirmed segment identities, then consider active holes, resource pressure and coarse repair-demand hints. Such hints are not exact Wirehair rank; receiving K distinct IDs alone does not establish successful recovery.

The principle is not PAM4-specific. However, the four modes, small-file barrier, DirectRepeat and Gray Fast spatial bank have different integration/atomic boundaries. Prove the main PAM4 path first, then extend against the same contract. **This report does not approve a first-release mode matrix or automatically inherit the prior's PAM4-only and >12-segment restrictions.**

## 7. Suggested feedback semantics, not a frozen format

Prefer a dedicated, complete, bounded, repeat-readable snapshot over an event log requiring uninterrupted sequential consumption. Missing intermediate updates need not prevent convergence on the next full snapshot. Names below are illustrative, not a published schema.

| Content | Purpose |
| --- | --- |
| Magic/schema/version, exact length and integrity check | Reject wrong files, truncation, torn writes and unsupported formats; a checksum is not authentication |
| Full SessionId, exact Profile/layout, file size/count and canonical Session/bound FinalManifest identity | Prevent cross-file/session/mode use; a filename or SessionTag is insufficient |
| Non-personal ReceiverId, random receiverEpoch and monotonic snapshotSequence | Designate one writer, separate restarts/output changes, reject rollback and conflicting same-sequence content |
| durableCompleted set | Identify specific verified, durably stored segments, not a contiguous raw-byte prefix |
| Optional bounded active-segment/repair-priority hints and resource state | Finish in-flight holes before admitting more work; never authorize larger budgets |
| FrameSequence actually observed through the visual channel | Support current-Start binding and delay diagnostics; never manufacture an observation from a prediction or file data |
| Recovery phase and final verification results | Distinguish recovery/storage/whole-file verification/final success/failure |

Decoder need not reproduce Encoder's complete local descriptor bundle. Do not include source/output paths, payload, recovered blocks, scripts, arbitrary commands or screenshots. Identity hashes must use canonical records both sides can actually validate. Unknown or unbound state is not a valid acknowledgment.

With the current 65,536-segment bound, a completion bitmap needs at most **8,192 bytes**, approximately 10.7 KiB in Base64. A bitmap or strictly expansion-bounded ranges are more predictable than one JSON object per segment. Bound the entire file, active hints, strings, nesting, integers, duplicate keys, trailing data and unused bitmap tail bits before allocating from external counts. A 64 KiB snapshot cap and roughly one-to-two-second coalesced publication/read intervals are reasonable experimental starting points, **not measured optimal defaults**.

## 8. Where completion facts must originate

### 8.1 Segment acknowledgment

Set a completed bit only after EncodedDigest, bounded decompression, RawDigest, .part write/flush/checkpoint, durable completed journal record and Receiver stored commit. The existing RecordCompletedSegment → CommitStoredSegment → storedSegments update is a candidate observation point. FEC readiness, unique symbols, estimated GUI progress and Transport CRC are not sufficient.

After restart, rehash stored segments through the existing recovery path before publishing a new epoch's set. Do not blindly OR old bitmaps across epochs or replace .resume validation with feedback. Retained static-audit notes include large-transfer/resume progress-estimation defects, making those estimates particularly unsuitable for acknowledgments.

### 8.2 Whole-file receipt and automatic Encoder stop

The user's requested automatic stop is feasible, but requires:

1. Matching transfer and designated-receiver identities.
2. Final length/digest matching Encoder's FinalManifest.
3. Decoder evidence of WholeFileDigest, safe publication and final reopen success, not merely all segments, PublishIntent, a completed carousel or zero ETA.
4. Evidence belonging to this Start, with the stop action still targeting that exact sending generation.
5. The freshness and writer/trust conditions below.

Encoder can state “receiver reports verified completion”; it cannot claim to have independently read and verified the final file over the mapping. Stop through the local safe lifecycle, not PBBridge, injected keys or process termination. Do not delete the source or resume state. Feedback never bypasses Decoder publication gates or turns CRC into authentication.

Retain the final receipt durably in bounded state. B cannot know when A consumed it: do not publish once and immediately delete it, expire it under a short TTL, or wait for A to write an ACK. If publication fails, report “file completed correctly; auxiliary feedback failed” without rolling back the verified final or permitting A to stop without a valid receipt.

## 9. Old receipts, generations and visual freshness

SessionId alone is insufficient: Encoder may resume the same immutable Session while an old final receipt remains cached. An increasing snapshotSequence does not prove a snapshot was generated recently.

A promising candidate that needs no new visual field is the existing **durably non-reused FrameSequence**:

- Current live Encoder starts at its Session-store lease endpoint, reserves before use, and advances its actual sequence only after successful Submit.
- Decoder returns a sequence actually observed after the selected Session/Profile visual-identity checks.
- Encoder checks membership in this Start's actual successful Submit range, excluding previous runs, unused leases and future values.
- durableFrameSequenceLeaseEnd is not a submitted upper bound. Existing EncoderSnapshot.frameSequence generally contains the post-increment next value, not lastSubmittedInclusive.

This can reject an honest receipt left by a **previous Start**, without A writing B's directory or changing the visual wire. It is not a challenge-authentication scheme against an untrusted writer and does not prove the mapped view is globally latest.

The candidate also assumes **one Sender and trusted Session state that is neither cloned nor rolled back**. The existing directory-owner lock does not cover independent copies, and CRC does not prevent restoring an old backup. A copied state can reuse the same Session's sequence range. Covering those cases requires a genuinely per-Start random visual challenge with explicit compatibility/recovery semantics; do not promise “no wire change” unconditionally.

The witness must come from current live capture, admitted through the selected Profile's existing checks and bound to the complete SessionDescriptor, not a cadence tracker's maximum sequence. Bootstrap-only/erased observations can advance cadence telemetry. Do not require a new Transport unique equation, which would exclude empty files and valid resume completion. No ready-made feedback API currently satisfies all of these conditions.

Three missing capabilities require deliberate design:

1. **Late callbacks:** Current RequestStop has no expectedRunGeneration parameter. A file thread must not validate once and call it directly later. Deliver an event carrying the exact Start token to the run owner, and recheck at the lifecycle boundary so an old read cannot stop a new run.
2. **Restart after completion:** Decoder currently stops capture when complete, and its published pipeline does not process new frames. If A restarts after B has completed, the old receipt lacks the new Start witness. Covering this case requires an auxiliary terminal-confirmation lifecycle that revalidates the completion receipt/final file and obtains a new bounded visual witness. Otherwise require explicit manual confirmation; do not relax the old-receipt rejection. This recovery path should be part of automatic-stop acceptance, not delegated to a short TTL.
3. **Long final verification:** Hashing/publication/reopen may outlast a short visual freshness interval. Separate short-lived scheduling hints from terminal evidence: do not reject a valid final solely because its last observed frame is seconds old, but do not drop current-Start binding.

The time a snapshot is first read locally is not its true creation time. Rereading the same sequence must not renew freshness. Endpoint monotonic clocks cannot be subtracted. Local submit records and echoed visual observations may provide a conservative visual-round-trip age, but polling frequency does not establish an update-delay bound.

Terminal identity, bitmap, digest, verification results and visual witness must belong to **one immutable snapshot, one epoch/sequence and one authoritative state generation**. Never splice an old receipt with a new frame number. Random epochs are not chronologically ordered; require a B-side single-writer lock and explicit takeover rules rather than switching to any unfamiliar epoch. Retired-epoch metadata must also remain bounded.

The formal design must choose whether a receipt represents current receiver-generation state or durable evidence that this Start has already achieved delivery, and apply that meaning consistently during recovery. Starting another B task must not silently overwrite the only receipt before a delayed A reader can observe it; define finite retention/explicit supersession. Permanent mapping failure or B exiting before durable receipt publication makes guaranteed automatic stop impossible. Safe fallback is continued original transmission with manual stop available. Successful B publication does not prove A stopped.

## 10. Read-only mapped-file engineering

### 10.1 Proposed user entry

Prefer a **dedicated directory containing a program-managed status file**. Each endpoint selects its own path. A generic Encoder-side example is:

~~~text
\\Client\<mapped-drive>\<feedback-directory>\<status-file>
~~~

Path strings need not match; similarity does not prove correspondence. Permit pasting a mapped UNC path without requiring a drive letter. A read-only capability check must not attempt a test write. If single-file selection is offered, use a dedicated program-owned file, never overwrite an arbitrary existing document. Read only the agreed name; no recursive Desktop scan, discovery of other locations or reading neighboring user files.

### 10.2 Publication, reading and caches

- B writes a complete same-directory temporary snapshot, flushes/closes, then publishes with validated local same-volume replacement semantics. Keep the previous complete state on failure. Do not expose partially overwritten files or pretend a cross-volume move is atomic.
- A requests GENERIC_READ and OPEN_EXISTING, opens briefly, reads a bounded complete snapshot and closes. Appropriate FILE_SHARE_READ/WRITE/DELETE allows B's replacement; FILE_SHARE_DELETE grants sharing, not delete access to A. [CreateFileW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew)
- Validate the content obtained from one open handle. Do not stat one pathname object and read a different replacement. Reject torn, oversized, conflicting or mismatched snapshots without partially applying them.
- Local replacement does not imply immediate remote visibility. Notifications, directory entries, content and long-held handles can have different caches. Periodic reopen is a practical candidate, not proof of strong consistency.
- Microsoft documents unsupported redirectors and network buffer restrictions for ReadDirectoryChangesW. Notifications may accelerate a poll, not serve as the only correctness mechanism. [ReadDirectoryChangesW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-readdirectorychangesw)
- The official SMB client tuning page documents file-information, directory and negative-lookup caches. It cautions against treating unchanged mtime or “not found” as decisive, but describes SMB, not every client-drive redirector. Do not automatically modify global registry settings. [SMB client caches](https://learn.microsoft.com/en-us/windows-server/administration/performance-tuning/role/file-server/)

### 10.3 UNC is not necessarily SMB; cancellation is not a hard deadline

Citrix documents client-drive mapping and read-only access, supporting the practicality of the described capability. It does not guarantee update latency, notifications, replacement visibility or cancellation deadlines for a particular path. [Citrix Client Drive Mapping](https://docs.citrix.com/us/en/citrix-virtual-apps-desktops/2511/devices/client-drive-mapping.html)

Isolate reads from UI, the D3D owner and FEC/storage commits. Use bounded workers, a latest-snapshot mailbox and finite retries/backoff, not a fresh thread after every timeout. Windows CancelIoEx/CancelSynchronousIo depend on driver support; wrapping a blocking UNC read in a thread with a timed wait does not establish bounded shutdown. [Canceling Pending I/O](https://learn.microsoft.com/en-us/windows/win32/fileio/canceling-pending-i-o-operations)

Test the actual redirector's blocking, cancellation and close behavior. If process-exit bounds cannot be established, assess a bounded isolated reader process or equivalent containment, exchanging only auxiliary metadata, never payload. This report does not assert that an existing worker meets the requirement or approve a helper-process architecture.

## 11. Scheduling, fallback and trust

### 11.1 Scheduling recommendations

While fresh, correctly bound feedback is effective:

1. Prioritize incomplete segments already consuming receiver resources and supply meaningful repair service to old holes.
2. Confirmed durable segments can leave a visit early or receive much less service; retain descriptors/source and repair high-water state.
3. Admit fresh segments only within existing limits; do not instantiate codecs for every zero bit.
4. Preserve Session/Manifest/current Segment control refreshes.
5. Never mutate a pending raster. Change subsequent choices only across completed atomic frame or applicable bank boundaries. Early pass retirement needs correct committed-service accounting and retirement of unused reserved IDs; simply setting graduated violates existing assumptions.
6. Preserve deterministic, fair and sufficient fallback service for unconfirmed segments. When hints expire, segments not protected by trusted confirmation rejoin the original all-file coverage.

Do not change Wirehair descriptors, encoded bytes, ID-to-equation meaning, DirectRepeat, quota/OOM handling or receiver digests. Feedback changes service order/budget; it need not demand retransmission of a particular missing repair ID when legal new equations can serve the same segment.

### 11.2 States and failures

Possible descriptive states are Disabled, WaitingForMatchingFeedback, Active, Stale/Unavailable, Rejected/Incompatible and ReceiverReportedComplete. These are not existing product enums.

- **Disabled:** no feedback reader/writer, mapped access or automatic stop from residual files; original scheduling and manual/explicit-timeout stopping remain.
- **Missing/lost/stale/I/O failure:** no waiting for ACK; return to the user's configured no-feedback policy. If the prior was already enabled, that is the fallback baseline rather than silently clearing unrelated settings.
- **Wrong identity, malformed data, multiple writers or same-sequence conflict:** reject/suspend auxiliary feedback and explain why, without weakening pixel admission, discarding correct recovery or stopping a valid transfer.
- **Epoch change:** invalidate old feedback and late reads; await new binding and recovery revalidation. Never splice generations.
- **All segments but no final verification:** not permission to stop. Receiver errors and feedback failures are not terminal success.
- **Older opposite endpoint:** absence of supported feedback retains no-feedback operation; do not silently enable other channels or change the visual protocol version.

Actual feedback and the prior must not silently compete for weights. A reasonable design direction is verified specific facts first, with original policy still servicing unknown/invalid regions. Compare original, prior-only and feedback-only operation before deciding whether a combination has additional value.

### 11.3 Read-only does not authenticate a writer

Read-only restricts A's writes; it does not prove the file came from the designated Decoder. Checksums, public SessionId/FrameSequence/generation and digests do not authenticate content. The basic scenario depends on a program-owned single-writer location and trusted access control. Untrusted writers require independent pairing/MAC/signature authentication or disabling trust actions such as automatic stop and hard suppression.

A forged completed bitmap can cause starvation or early stop even though Decoder cannot publish an unverified file. Do not present fail-closed file integrity as immunity to availability attacks. Formal design must state the trust boundary rather than infer it from a mapped pathname.

## 12. Primary online references and applicability

Checked 2026-10-21. Only original papers, official standards/docs and upstream implementations support technical conclusions. No dependency or network-transfer protocol was installed.

| Source | Relevant lesson and limit |
| --- | --- |
| [RFC 5740 NORM](https://www.rfc-editor.org/rfc/rfc5740.html), §§2,4.3.1 | FEC repair can use an asymmetric return path; repair requests can identify items, ranges or block erasure counts. Borrow semantics, not IP multicast or multi-receiver suppression machinery |
| [RFC 6330 RaptorQ](https://www.rfc-editor.org/rfc/rfc6330.html), §4.4 | Sufficient symbols must reach the corresponding source block; no reason to replace Wirehair |
| [RFC 6726 FLUTE](https://www.rfc-editor.org/rfc/rfc6726.html) | A mature unidirectional reference; optional feedback does not remove the value of the original no-return-path capability |
| [Sun et al., 2014, limited feedback and rateless scheduling](https://arxiv.org/abs/1404.6683) | Sparse ACKs without exact CSI support the direction; cellular scheduling/power models do not predict a PixelBridge speedup |
| [Sørensen et al., 2012, Rateless Feedback Codes](https://www.merl.com/publications/TR2012-052), [paper](https://www.merl.com/publications/docs/TR2012-052.pdf) | Limited feedback can help particular code designs. Their LT/Raptor degree redesign is not a frozen Wirehair setting; keep initial changes outside equation generation |
| [Leith/Karzand, 2016, author publication list](https://www.scss.tcd.ie/Doug.Leith/publications.php?name=karzand) | Directly relevant delayed-feedback throughput/delay literature. The author PDF could not be fetched, so no specific formula or algorithm was inferred from unread full text |
| [QRTransfer](https://github.com/tony-xlh/QRTransfer) | An upstream visual-transfer example explicitly lets receiver reports suppress scanned content; not a Windows large-file FEC/durable-storage implementation or remote-desktop benchmark |
| [qr-file-transfer](https://github.com/thealper2/qr-file-transfer) | Separate ACKs, session/packet binding and final-digest FINISHED illustrate correctness; per-packet stop-and-wait is unsuitable for blindly copying into seconds-delayed feedback |
| [TXQR](https://github.com/divan/txqr) | Animated QR plus fountain provides a visual-transfer reference, not proof of this file-feedback capability |
| [Pinned libcimbar source](https://github.com/sz3/libcimbar/blob/156fbb9c675ca9641f6559922b7025a0f2ee8125/src/lib/fountain/fountain_encoder_stream.h) | Continued fountain-block generation is relevant, but this path retains the entire input stream and cannot be copied as the large-file working-set model |
| [Pinned Wirehair V2 contract](https://github.com/catid/wirehair/blob/067ca7cdb66aed424ec23f97557429bf791c6f0c/V2_WIRE_PROFILE.md) | Canonical descriptors and IDs fix equation meaning, not authentication; scheduler changes must preserve that meaning |
| [Citrix mapping](https://docs.citrix.com/us/en/citrix-virtual-apps-desktops/2511/devices/client-drive-mapping.html), [notifications](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-readdirectorychangesw), [read-only open](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew), [I/O cancellation](https://learn.microsoft.com/en-us/windows/win32/fileio/canceling-pending-i-o-operations) | Support scenario and engineering-risk analysis, not the latency, deadline or speed of the user's actual mapping |

## 13. Proposed validation order and stop conditions

None of the following experiments was executed in this task.

1. **Review the written design first:** mode scope, program-owned path/trust, terminal restart lifecycle, snapshot limits, I/O isolation and prior precedence; explicitly update optional-control-plane boundaries. Implementation design should resolve API details rather than ask the user to choose low-level mechanisms.
2. **Small mapping capability probe:** after separate authorization, B writes only program-owned metadata, A only reads. Test updates, reopen/replacement/cache, disconnect/reconnect and stop; measure sequence visibility/read duration without changing global caches or mappings. No payload.
3. **Parser/state cases:** repeated/reordered/conflicting sequences, truncation/oversize/overflow/duplicate keys, wrong full SessionId with same Tag, wrong Profile/Manifest, 0/1/65,536-segment maps, epochs, lost snapshots, replacement races, unowned files and parent-directory substitution.
4. **Real FEC/Receiver/storage regressions:** 0/1/6/12/13 segments and multiple windows, RAW/compression/DirectRepeat, burst/periodic losses, early/tail holes, late join, resource pressure, disk/flush failure, revalidation and independent endpoint restarts. An ideal K-threshold simulation does not replace Wirehair or final files.
5. **Automatic-stop cases:** residual Completed, a previous Start of the same Session, unused leases/next-sequence boundaries, old epochs, reads crossing Stop/Start, long hashing, missing/unwritable final receipts, A restart after B completion and B restart/output changes. Prove no early stop; without valid terminal evidence continue and explain the state.
6. **Disabled equivalence:** identical inputs/clocks/loss models retain the old scheduling trace, submission/equation identity and zero feedback I/O, not just successful recovery of a small file.
7. **Actual pixel comparisons:** same candidate/file/Profile/FPS/presentation/capture/budgets/remote settings, fresh and resume runs separately. A 91 MiB file with 7 MiB segments covers >12 segments while staying below 100 MB for screening; full large-file tests remain separate. Compare original, prior-only and feedback-only without changing density/FPS/budgets simultaneously.

Primary metrics are Decoder's full run through final reopen and verified goodput. Also report tail, service wasted on completed segments, longest unique/verified stalls, feedback acceptance/rejection/staleness, observed visual-witness age, file-I/O/resource costs and safe-stop latency. Report Encoder receipt time separately: return-path delay is not part of the time at which Decoder completed the file. Never subtract cross-host monotonic timestamps.

**Do not promote** a candidate with digest/conflict/resume/resource regressions, early stops, starving old holes, mapped I/O blocking shutdown, disabled-path I/O, or gains dependent on favorable-only cases, relaxed tests or mixed package identities. Invalid feedback must not produce accepted unverified files; persistent malicious-writer DoS needs an explicit separate limitation.

## 14. Delivery boundary and recommendation

This task delivered document reading, narrow static source tracing, online research and conditional recommendations, plus bilingual reports and index/status/evidence maintenance. It implemented no feedback, accessed no actual user mapping, changed/packaged no product, measured no local or field speedup, closed no prior audit/failure and created no Git commit.

**Proceed to an explicit optional segment-feedback design.** It targets waste more directly than a rate-based prediction. The real engineering work is durable semantics, unreliable mapped-file timing, bounded/fair scheduling and the requested generation-safe automatic stop, not merely writing a status file. Define and validate those boundaries before considering rate adaptation or equation-level feedback.
