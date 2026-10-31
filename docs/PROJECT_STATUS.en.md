# Project status

[简体中文](PROJECT_STATUS.md) | **English**

Updated 2026-10-31. **Current phase: v1.1.0 release preparation**, focused on visual-transfer reliability, display compatibility and delivery. This includes optional feedback, the PAM4 rate prior, explicit Qt software presentation and existing reliability fixes; it does not reopen the unfinished historical static audit or unrelated performance research.

The maintainer reports that the previous feedback candidate passed a real-use test and authorized repository cleanup, commit, push and v1.1.0 delivery. This is maintainer-supplied functional feedback, not a complete profile/provider/size/stability matrix or quantitative speedup result. Release packages are rebuilt from the exact committed tree; see [current release notes](RELEASE_V1.1.md).

[Feedback](FEEDBACK_ASSISTED_TRANSFER.en.md) remains default-off, supports all four modes and falls back safely. Completed B keeps listening with read/view-only output until Stop. All forward payload remains pixel-derived. The separate [PAM4 prior](SENDER_RATE_PRIOR.en.md) is a soft scheduling hint, not a final receipt.

The 2026-10-21 candidate's local complete CTest was 208/209; the historical GrayFast 12505 <12000 gate and missing Replay skip remain explicit. Current committed-source build/regression results are reported separately, not inferred from old totals. Earlier candidates and research remain [historical records](FEEDBACK_ASSISTED_TRANSFER_CANDIDATE.md), not instructions to reopen them.

## Implemented capabilities

- Windows x64 GUI applications; fresh, missing or invalid mode preferences default to PAM4 (index 2); Standard/Gray Fast compatibility retained and Wide remains selectable.
- PAM4's 1080P source raster; Wide's fixed 2560×1440 raster on a suitable 1440P/1600P source desktop.
- Selected-monitor whole-screen/contained-ROI capture without a protected second screen or reserved UI area; startup/runtime display-identity revalidation.
- Finite performance budgets, total/per-instance settings and host recommendation; batched resume checkpoints and bounded compaction.
- Visual-only one-way payload, segment/whole-file digests, safe publication and final reopen.
- Current closeout adds automatic logs, evidence-based activity text, and distinct icons. Targeted validation is recorded separately.
- Decoder GUI active reception timing starts at the first valid same-Profile `SessionDescriptor`, excluding the earlier wait; the completed GUI average and frozen active elapsed time use the same window. Formal full-run time and verified goodput meanings are unchanged.
- Encoder GUI/Runtime now support local **Maximum run duration (seconds)**: `0` means manual stop, while a non-zero `uint64_t` value starts when Start is accepted and follows the safe-stop path; it is not a protocol field or receiver-completion signal.
- A normal Encoder GUI start idempotently keeps a per-user HKCU Shell static verb for both `*` and `Directory`. The verb opens a small sender-rate/mode/timeout dialog; folder inputs are bounded locally into a standard ZIP and then reuse the existing ordinary-file visual transfer, digest, safe-publication and final-reopen chain. Wire/Profile, Decoder and ACK semantics remain unchanged.
- The worktree retains the 2026-10-09 explicit Qt software-presentation candidate: both main/Shell windows allow stopped-only selection, default to D3D11 on each new window, never persist the selection, and never automatically fall back. Its sealed local checks are separate from current source changes; this audit supplies no Citrix freeze-improvement field acceptance.
- The common file-byte cap is 500 GiB with at most 65,536 segments; the current PAM4/Wide GUI's default 7 MiB segments jointly cap it at 448 GiB. See [budgets and file size](DECODER_MEMORY_BUDGET.en.md).

## Evidence boundaries

| Item | Status |
| --- | --- |
| Real non-local full file | Earlier sealed candidate: ToDesk, Wide 25 Hz, 1,206,113,941 B in 4,227,996 ms, correct output |
| One-hour / no-plateau objective | **Unmet**; longest retained joint no-progress interval 217.439 s |
| 1080P / 1440P / 1600P | Remote-source dimensions tested at different stages, not certification of both endpoints at 1080P or arbitrary scaling/providers |
| Later private-package small files | Stage47 PAM4 at remote1080P, Stage48 Wide at remote1600P, 1 MiB each; correct final files |
| A computer with only one physical monitor | **Not independently qualified**; unavailable hardware. Selected-monitor authority tested on a dual-monitor computer; feature retained |
| New logging/UI source | Separate build/targeted/offscreen checks; older throughput identities do not transfer to it |
| Formal v1.0 version/commit/packages | Version 1.0.0, MIT, separate endpoint packages; exact identity/results are recorded in release manifests/evidence |
| Historical v1.0 closeout non-interactive release regression | 172/173 CTest entries pass; the historical Gray Fast spatial late-join `< 12,000` frame gate remains unmet at 12,505 frames. Final file is correct; test and threshold are unchanged |
| Full Release CTest from the 2026-09-22 audit build | 233 of 234 entries pass; the only failure is the same Gray Fast spatial late-join (12,505 post-join frames). `PBApplicationTests` contains one skipped case because the real Replay root is unset. This is that build's record, not a current-worktree result |
| Sealed Qt presentation candidate, 2026-10-09 | Retained handoff records 75/75 targeted CTest and 40 report/configuration cases / 1026 assertions, source fingerprint `bf0015ed4206`. Local candidate evidence only, not a Citrix fix or verification of this audit's edits |
| Static-audit worktree, 2026-10-15 | Code/document review remains in progress; no build, application launch, test execution, or field validation. Added regression-test source is not a passing test result |
| Encoder Shell entry | Local unit/offscreen GUI checks, shell-argument parsing, HKCU registration readback, and Windows `tar -caf` ZIP archive checks are targeted-verified; this is not claimed as real non-local throughput qualification |

## Targeted closeout checks

Authoritative local records: `artifacts/v1-closeout-20260917/build08/` and `test09/`.

- Both Release applications build; five targeted groups pass 79 cases and 13,912 assertions.
- Both offscreen GUI checks pass, including PAM4/Wide application/storage recovery and final reopen, Decoder default/minimum layout, and normal/abnormal activity explanations.
- Seven automatic log pairs from real GUI controller paths were checked for terminal states, sampling metadata, and PAM4/Wide verified completion.
- Both original logos remain byte-identical. All seven native icon sizes match their ICO data; rebuilding the ICOs reproduces identical bytes.
- The document inventory, pre-retirement backup, bilingual current docs and local-link check are recorded. Protected reports remain unchanged.

These are local correctness/offscreen checks, **not new physical-screen remote or main.rar throughput measurements, nor qualification on a single-physical-monitor computer**; this task also did not use PBBridge for free non-local testing. Those records describe the closeout baseline; new v1.0 build/unpacked verification records are separate in `artifacts/v1-release-20260917/`.

## Remaining limitations

Remote internal scaling/bitrate/update cadence limits delivered information. Sending Hz is not receiving Hz. PAM4 currently uses CPU-reference readback. No ACK means carousel tails; more RAM is not a universal speedup. Logs support layered investigation but cannot identify every black-box cause uniquely.

README review is complete. The maintainer selected MIT and authorized step 5: explicit commits, version 1.0.0 and separate local product packages. No remote push is included. See the bilingual [v1.0 release notes](RELEASE_V1.0.md) for instructions, corresponding source and validation rules.

[Architecture](ARCHITECTURE.en.md) · [User guide](USER_GUIDE.en.md) · [Diagnostics](DIAGNOSTICS.en.md) · [Evidence](EVIDENCE_INDEX.md) · [Document history](DOC_HISTORY.md)

**Candidate packaging scope, 2026-10-21:** The user requested separate portable ZIPs for this Encoder and the current Decoder from the same source snapshot, in a new candidate directory without replacing dist releases. See the [candidate guide](PAM4_RATE_PRIOR_CANDIDATE_PACKAGES.md); actual delivery results live under artifacts/pam4-rate-prior-packages-20261021-run01/. Formal clean-commit release gates remain intact; no source commit is implied.
