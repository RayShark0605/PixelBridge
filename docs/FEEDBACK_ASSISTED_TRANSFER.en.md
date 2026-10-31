# Optional file-feedback-assisted transfer

[简体中文](FEEDBACK_ASSISTED_TRANSFER.md) | **English**

Updated 2026-10-31. This default-off feature is part of v1.1.0; the maintainer reports a successful real-use test of the preceding candidate. See [release notes](RELEASE_V1.1.md). Earlier local test counts below belong to the 2026-10-21 candidate; they are not quantitative field-speedup or universal provider certification.

## Data and control boundaries

Every A-to-B payload byte must still be recovered from actually captured pixels. Only bounded receiver metadata travels B-to-A: identity digests, durable segment bitmap, validated visual observation and final verification facts. No source bytes, encoded payload or arbitrary commands enter the feedback file or helper pipe. Disabled mode launches no feedback helper/policy and preserves original scheduling, recovery cleanup and stopping behavior.

The first version assumes one designated Decoder, one Sender, and trusted, un-cloned/un-rolled-back Encoder Session cache. CRC and the writer lock are not authentication. Multiple or untrusted writers are outside this trust contract.

## Setup

1. On B, open **Decoder → Advanced → Optional feedback**, enable status publication and select a writable folder or dedicated file. This need not be the output folder.
2. Folder mode uses `PixelBridgeFeedback.pbfb`; file mode uses the selected name. Prefer a physically separate dedicated feedback subfolder, not output/recovery files. Unrelated files, directories, links and conflicting locks are not overwritten.
3. On A, enable read-only feedback in Encoder's main or Shell transfer entry. Select the corresponding mapped folder/file; pasted UNC paths are supported. A and B path strings can differ, but must designate the same status file and matching location kind.
4. Select matching Standard, Gray Fast, PAM4 or PAM4 Wide modes, then start the normal receiver-first transfer. Feedback does not auto-start a transfer or alter displays/mapping configuration.
5. After B says **Completed, feedback listening**, leave it listening until A reports receiver-verified completion and safely stops; then stop listening or close B. Reading/viewing remains available while listening, but stop listening before editing, moving or deleting the completed file. Stop releases the read-only lease.

Encoder main/Shell share preferences; Decoder preferences are separate. Preference loading and Apply perform lexical validation, not network probing. A missing/unavailable mapping can disable the auxiliary connection without making visual transfer depend on it.

B starts the writer only after the visual Session establishes output/recovery paths and lexical overlap checks pass. The feedback primary and `.tmp`/`.previous`/`.lock` cannot take the current final, part, journal or journal temporary pathname; conflicts disable only feedback. Checks normalize Windows case and lexical components, not a proof of arbitrary ancestor redirection, short-name/namespace aliases or every filesystem race. A genuinely separate feedback subfolder remains recommended. A may wait/read-retry before the status file appears.

## Trusted facts and scheduling

A bitmap bit follows the real durable `RecordCompletedSegment`, `CommitStoredSegment` and `storedSegments` updates, never FEC-ready, CRC, estimated percent or rate-times-time. Acceptance binds the complete Session/Profile/layout/dimensions, canonical SessionDescriptor/FinalManifest BLAKE3, writer/epoch/sequence and consistent bitmap to a frame **actually successfully submitted in the current Encoder Start**. Leases, built-only/failed frames, next sequence and previous Starts cannot authorize stopping.

The visual witness follows complete selected-profile/same-frame validation, not Bootstrap-only, erased-frame or cadence high-water statistics. Ordinary hints expire after 15 seconds without a newer qualified observation. Duplicate snapshots/heartbeats do not renew freshness. Final receipts have no short TTL but still require current-Start and complete identity binding.

At committed frame/spatial-bank boundaries, durable confirmations retire subsequent service while retaining at most six encoded segments and deterministic unconfirmed revisits. Encoded bytes/descriptors stay immutable, committed-service accounting remains exact, and unused leased repair IDs are retired, not reused. Expiry restores all-file coverage and the configured no-feedback/prior policy without persisting ACKs as Sender completion. All bits without a final receipt keep a real one-segment service visit and Control refresh; they neither stop nor fabricate an empty Session.

## Final publication and restart

Final status follows the existing whole-file digest, safe publication and final reopen checks. Encoder records `ReceiverVerified` separately from user/timeout stops; it is a receiver report, not independent Sender-side final-file verification.

Only enabled feedback retains the existing completed `.resume`, including Manifest, PublishIntent, bound reservation and completed records. Decoder restart revalidates final length/digest/reopen through the existing recovery chain; it does not invent a new rename observation. Missing/corrupt final output cannot generate a new terminal receipt. No bitmap is imported from feedback into recovery. Removing the retained journal loses automatic restart confirmation, not the completed file.

Post-completion listening admits only same-Session qualified visual observations: no more Outer FEC input, output writes or verified-byte growth. File-completion timing freezes; listener start/stop/elapsed are separate. Explicit stop/close/new-run lifecycle must retire the old listener without downgrading published output.

When feedback is enabled, the ordinary single-link handle used for final reopen/digest verification remains read-only, with only FILE_SHARE_READ, until listening stops. Verification and retention refer to the same object with no close/reopen gap. A new Start cannot renew a terminal receipt from unprotected historical booleans. Existing writable handles/mappings or unsupported providers disable new auxiliary terminal authorization, never weaken sharing protection or revoke an already verified file result.

## Bounded I/O and failure

Each standalone package needs sibling `PBFeedbackIo.exe`. Its local versioned pipe carries metadata only. Hidden creation, explicit inherited handles and a constrained Job Object isolate blocking redirector calls. Each runtime retains at most one unconfirmed child, one in-flight write and one latest pending snapshot, with no unbounded threads/retries.

Initial polling/publication is about once per second, with prompt durable/terminal coalescing. A five-second operation watchdog invalidates and terminates the helper; no replacement starts before exit is confirmed, and Stop waits finitely. Ordinary read errors can recover in the same live reader; a killed/timed-out helper requires explicit Stop/Start. These are conservative engineering settings, not measured field-optimal timing.

A opens read-only/existing objects and never probes by writing or cleans B's files. B writes/flushes its owned temporary file, retires the same validated old object by handle into fixed `.previous`, then publishes with **no-replace** semantics. A concurrently inserted unrelated target is never overwritten. Existing shared readers keep their complete old object, but a short absence is possible and safely disables hints; this is not gap-free atomic replacement or pathname CAS.

Failure attempts only no-replace restoration. A target conflict retains both objects and disables feedback. Occupied `.previous`/`.tmp`/lock names are not adopted or removed automatically. Stop both ends, establish ownership and move leftovers to a backup before selecting a fresh dedicated feedback location and starting again. Unsupported safe rename/cleanup fails the auxiliary connection closed.

## Reports and evidence limits

Reports/journals add `feedback` (`PixelBridge.FileFeedbackTelemetry.1`) with requested/effective state, acceptance/rejection/duplicate/timeout counters, available durable count, retired service visits, observation sequence, terminalPublished/terminalAccepted, explicitly receiver-reported completion time and separate listener timing. Disabled/unknown facts are false/null. Feedback paths, arbitrary feedback status strings and payload are not serialized.

Existing Encoder `receiverProgress`, `receiverEta` and `verifiedGoodput` remain unavailable; skipped visits are not saved bytes/time or measured speedup. Existing source/output diagnostics elsewhere can still contain private paths: the whole report is not automatically privacy-clean.

Local coverage includes malformed/boundary codec cases, file safety, actual helper timeout/restart, four-mode 0/1/6/12/13-segment real FEC/Receiver/storage, spatial banks, expiry fallback, terminal restart, actual new-Start pixels, offscreen GUI and simulated DPI. No-raster probes are not screen/remote certification. The full application suite retains the historical Gray Fast late-join `12505 < 12000` failure and missing-real-Replay skip; no all-green claim is made.

Real mapping visibility, cache/disconnect behavior, permissions/occupancy, long runs and end-to-end gain remain field checks. Compare feedback off/on in the same candidate using digest/publication/reopen-complete timing. See [research and online sources](FEEDBACK_ASSISTED_TRANSFER_RESEARCH.en.md), [design](superpowers/specs/2026-10-21-feedback-assisted-transfer-design.md) and [status](PROJECT_STATUS.en.md).
