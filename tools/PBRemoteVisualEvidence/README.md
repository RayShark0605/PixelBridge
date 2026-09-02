# PBRemoteVisualEvidence

These create-only PowerShell tools support the P1.5 RemoteVisual evidence workflow without capturing screen pixels or using a non-visual payload path.

Formal Step 18/20 evidence commands must run under `pwsh` 7.0 or newer, not Windows PowerShell 5.1. The strict JSON path requires PowerShell 7 case-sensitive hashtables and `System.Text.Json`; all eight Step 20 entry scripts declare `#Requires -Version 7.0` so an incompatible host fails before it consumes or publishes formal evidence. Endpoint wrappers record the actual PowerShell edition/version and the final verifier requires all three endpoint results to come from PowerShell Core 7+. The currently validated host is PowerShell 7.6.4.

- `New-PBRemoteVisualPortablePackage.ps1 -Role Both` creates the single shared Computer A/B bundle used by formal runs. Schema 2 binds HEAD commit/tree, the exact tracked+untracked tested-source fingerprint, the unchanged `phase1-gate-pass` identity, MSVC/Windows SDK/CMake/Qt/vcpkg identities, every deployed EXE/DLL/resource, LF4 constants, SPDX 2.3 SBOM, and complete Qt/vcpkg notices. The only permitted source exclusion is the pre-existing unrelated `docs/PHASE1_GATE_REPORT.md`. Publication is create-only and rolled back unless `Test-PBRemoteVisualPortablePackage.ps1` independently verifies the directory, external seal, and every ZIP entry.
- `New-PBRemoteVisualRunPreset.ps1 -RemoteProvider <name>` generates one OS-CSPRNG 128-bit RunId and a create-only strict `PixelBridge.RemoteVisualRunMetadata.1` JSON preset. The provider and visible product mode are explicit `NonDecodingOperatorMetadata`: they stratify field evidence but never select locator/demod thresholds, FEC/Receiver acceptance, WholeFileDigest, publish behavior, or wire semantics. Optional target FPS is `null` unless explicitly supplied, and UI evidence accepts it only when the exact value was visibly observed; environment/deployment records preserve `null` rather than coercing unknown to zero. Copy the same preset to both endpoints; the GUI or `--remote-metadata` loads it, rejects evidence identity conflicts, and keeps it outside the wire/acceptance path.
- `New-PBRemoteVisualSourceSet.ps1` runs on Computer B and creates fixed 1 MiB/8 MiB incompressible CSPRNG inputs plus a ZIP containing one 4 MiB CSPRNG `payload.bin`. The ZIP intermediate is deleted before publication; PixelBridge Segment compression remains RAW/OFF. `Test-PBRemoteVisualSourceSet.ps1` independently checks the flat exact inventory, file/inner-ZIP hashes, sourceSetId/fingerprint, external seal, resource bounds, and create-only verification output.
- `Get-PBRemoteVisualEnvironment.ps1` collects read-only OS/GPU/display metadata and the PixelBridge physical monitor catalog. It first verifies the shared Both-role package and source set, requires the monitor probe to be the packaged Decoder, and binds the exact package/source/seal/metadata hashes and shared RunId in `PixelBridge.EndpointEnvironment.2`. It never captures pixels, takes screenshots, changes display settings, or calls a remote-provider private API.
- `New-PBRemoteVisualDeploymentManifest.ps1` joins the exact shared package, source set, metadata preset, and Encoder/Decoder environment artifacts under one RunId. `Test-PBRemoteVisualDeploymentManifest.ps1` independently verifies every referenced artifact, both endpoint self-fingerprints, package/source validators, and the canonical deployment fingerprint. This record is the reverse lookup from a formal RunId to one package, one source set, and exactly two endpoint environments.
- `analyze_remote_capture.py <existing-image>` performs a read-only, no-window diagnostic probe of one already supplied screenshot. It reports SHA-256, candidate canvas edges/scale and RGB channel spread as JSON. It accepts only regular files up to 64 MiB and decoded images up to 8192 pixels per axis / 8 MiPixels, and rejects an input that changes during analysis. Edge estimates are evidence candidates only, never profile acceptance or field certification. It requires the pinned workspace Python environment with Pillow and NumPy; run `D:\Python3.12.9\python.exe -m unittest -v test_analyze_remote_capture.py` from this directory for its resource/determinism checks.
- `seal_remote_visual_dataset.py --input-manifest INPUT --artifact-root ROOT --output-index NEW_JSON` creates `PixelBridge.RemoteVisualDatasetIndex.1` for bounded screenshots and Replay v2 artifacts. Paths must remain below the explicit root; symlinks/junctions, protected-monitor pixels, malformed enums, duplicate JSON keys, non-finite numbers, changing files and overwrite attempts are rejected. Screenshots run through the bounded analyzer. Replay files receive an envelope check, but they are not considered semantically valid until `ReplayV2Reader`/the offline decoder succeeds; the index is never an acceptance input.
- `build_codec_corpus.py` exports a fixed three-frame production LF4 raster from `PBRemoteVisualCodecProbe`, produces five actual libx264/libx265 bitstreams, requires exactly one inspected video stream, decodes Gray8, binds source/evaluation reports to the actual sequence and per-frame bytes, and sends every frame back through production LF4/QC-LDPC/Transport diagnostic truth. Tool paths are explicit and sealed; output is create-only and bounded.
- `build_step06_corpus.py` is the one-command Step 06 entry. It combines the 22-case transform matrix, production temporal/identity corpus and actual codec corpus into `PixelBridge.RemoteVisualStep06Corpus.2`, validates both BLAKE3 and SHA-256 identities, and writes a recursive `SHA256SUMS.txt`. See `docs/REMOTE_VISUAL_STEP06_CORPUS.md` for the exact command and evidence boundary.
- `build_step07_calibration.py` is the create-only Step 07 entry. It cross-validates one sealed Step 06 corpus, one sealed Step 02 dataset/completion pair and the actual LF4 Replay before invoking `PBRemoteVisualMetricCalibration`. Dataset/run split isolation, complete post-admission sender truth, the 24-candidate policy/model matrix, validation-only selection, holdout/external improvement, full ROC/reliability curves and zero false accepted Transport/control/output are independently checked before `PixelBridge.RemoteVisualStep07Calibration.1` is published. Input paths are bounded, stable and confined below their evidence roots; symlinks and NTFS junctions are rejected. See `docs/REMOTE_VISUAL_STEP07_CALIBRATION.md` for the exact command and truth boundary.
- `Invoke-PBRemoteVisualGpuParityGate.ps1` runs the hidden Step 11 LF4 truth gate on WARP and every DXGI hardware adapter that can create a D3D11 feature-level 11 device. It independently parses the bounded JSONL diagnostics, requires CPU/GPU accepted-Transport identity, complete GPU timestamps, wrong-LUID rejection, zero raw-pixel readback and device-recreate parity, then writes a create-only `gate.log`, validated `evidence.json` and `sha256s.txt`.
- `Invoke-PBLocalDesktopRegressionGate.ps1` runs the Step 19 WGC/DXGI × Direct/Shape same-source matrix on the Experiment monitor without activating windows or sending global input. It verifies the Step 18 package and frozen 8 MiB baseline identities, starts Decoder before Encoder with a sealed bounded warmup, keeps cold-start acquisition inside the goodput window, requires digest/publish/external-hash truth, records CPU/GPU/memory/queue HWM and journal admission history, and publishes a create-only 48-artifact seal. `Test-PBLocalDesktopRegressionEvidence.ps1` independently reparses every case/report/journal/output and rejects post-descriptor resource-rejection growth, conflicts, wrong identities, seal drift, or overwrite.
- `Capture-PBRemoteVisualExperimentMonitor.ps1` calls the packaged Decoder's live monitor catalog and captures the exact non-overlapping ExperimentMonitor physical rectangle without input, focus, window, or display mutation. `New/Test-PBRemoteVisualUiEvidence.ps1` bind that capture record, screenshot, bounded analyzer, visible provider/mode claims, metadata and RunId; no UI-invisible value may be inferred.
- `New-PBRemoteVisualPilotPlan.ps1` verifies the Step 18 deployment and UI evidence, resolves the actual A/B monitor catalogs, and freezes one 1/2/5 Hz strict-or-scaled run. Schema 1 remains the Step 20 LF4/WGC contract. Schema 2 adds Direct/Shape/LF4, explicit WGC/DXGI, a UI-visible QualityPriority/Automatic/Restricted mode class, exact monitor runtime contracts and per-run isolation; Direct/Shape reject any non-1:1 geometry. Replay sampling remains below 1250 frames and 16 GiB, including the scale-aware 1.5x policy. `PBRemoteVisualPilotCommon.psm1` independently reparses the frozen schema and policy on both endpoints.
- `Invoke-PBRemoteVisualPilotDecoder.ps1` starts the actual Computer A receiver before Computer B, records a sampled production Replay through WholeFileDigest/safe publish, and later performs receiver-only offline reproduction. PilotPlan.2 selects WGC or DXGI and performs a new live monitor-identity preflight; production Replay supports Direct, Shape and LF4 without throttling primary demod. A sampled live Replay intentionally records no live demod observations; the offline contract instead requires one production demod result per captured frame and proves Receiver/WholeFileDigest/publish identity against the source and live output. `Invoke-PBRemoteVisualPilotEncoder.ps1` validates the exact Computer B package/source/metadata, performs the same plan-bound monitor preflight, and preserves Enter/Q as the only graceful completion input. `Test-PBRemoteVisualPilotEvidence.ps1` recursively rejects exact duplicate JSON keys, validates endpoint/package/report path and timeline identities, verifies Replay input/frame/file identity, and requires Encoder `Broadcasting` at or beyond the Decoder evidence-ready timestamp plus the full frozen proof interval before accepting the strict Python combined report. A successful PilotPlan.2 also publishes a sealed `PixelBridge.RemoteVisualMatrixRunRecord.1`. See `docs/REMOTE_VISUAL_STEP20_REAL_REMOTE_PILOT.md`; local probes never close the field Gate.
- `New-PBRemoteVisualStep21MatrixSpec.ps1` freezes one OS-CSPRNG canonical matrix identity before field execution: the exact 36-cell LF4/WGC Cartesian matrix, three fixed LF4/DXGI representative checks, and one Direct plus one Shape strict-1:1 comparison cell. `New-PBRemoteVisualStep21HardwareScope.ps1` can then bind that unchanged 41-cell spec to the sealed two-by-2560x1440 Computer A catalog and mechanically produce the authorized 31 included / 10 excluded partition; the importer recomputes ROI containment and rejects post-generation cell selection. `Test-PBRemoteVisualFieldFailureEvidence.ps1` accepts only a non-published live+offline production-Replay failure whose selected geometry/signal/temporal/metric/scheduler classification is supported by report and per-frame Inspector evidence; retained `.part` output is sealed and final output is forbidden. `Test-PBRemoteVisualStep21Matrix.ps1` requires exactly one independently sealed success-or-failure record per full-spec or hardware-scoped included cell, forbids reused run artifacts, verifies a common package/source identity, emits one CSV row per run without aggregation, and requires a same-mode/WGC/FPS/1:1 Direct/Shape/LF4 cohort with shared authoritative Decoder/Receiver metrics. A 31-cell PASS explicitly retains ten untested 1.5x exclusions and is not full-41 coverage. See `docs/REMOTE_VISUAL_STEP21_PROVIDER_GENERIC_MATRIX.md`.

Step 18 formal artifact order (all destinations and result files are create-only):

```powershell
$packageResult = .\New-PBRemoteVisualPortablePackage.ps1 `
  -Role Both -Label final-hardening `
  -BuildDirectory <clean-release-build> -OutputRoot <package-output>

.\New-PBRemoteVisualSourceSet.ps1 `
  -OutputDirectory <source-output>\remote-visual-source-set

.\New-PBRemoteVisualRunPreset.ps1 `
  -OutputPath <run-output>\remote-metadata.json `
  -RemoteProvider <visible-provider-name>

# Run once on each endpoint against its extracted copy of the same Both-role package.
.\Get-PBRemoteVisualEnvironment.ps1 `
  -EndpointRole Encoder `
  -PixelBridgeDecoderPath <package>\Decoder\PixelBridgeDecoder.exe `
  -OutputPath <run-output>\encoder-environment.json `
  -PackageManifestPath <package>\package-manifest.json `
  -PackageSealPath <package-seal> `
  -SourceManifestPath <source-set>\source-manifest.json `
  -SourceSealPath <source-seal> `
  -RemoteMetadataPath <run-output>\remote-metadata.json

.\New-PBRemoteVisualDeploymentManifest.ps1 `
  -PackageManifestPath <package>\package-manifest.json `
  -PackageSealPath <package-seal> `
  -SourceManifestPath <source-set>\source-manifest.json `
  -SourceSealPath <source-seal> `
  -RemoteMetadataPath <run-output>\remote-metadata.json `
  -EncoderEnvironmentPath <run-output>\encoder-environment.json `
  -DecoderEnvironmentPath <run-output>\decoder-environment.json `
  -OutputPath <run-output>\deployment-manifest.json
```

Step 19 producer and independent verifier (the verifier output must remain outside the sealed evidence root):

```powershell
.\Invoke-PBLocalDesktopRegressionGate.ps1 `
  -EncoderPath <verified-package>\Encoder\PixelBridgeEncoder.exe `
  -DecoderPath <verified-package>\Decoder\PixelBridgeDecoder.exe `
  -PackageManifestPath <verified-package>\package-manifest.json `
  -ExpectedPackageManifestSha256 <64-lowercase-hex> `
  -BaselineSummaryPath <frozen-baseline>\localdesktop-reference-summary.json `
  -ExpectedBaselineSummarySha256 <64-lowercase-hex> `
  -SourcePath <frozen-baseline>\sources\random-8MiB.bin `
  -OutputRoot <new-step19-evidence-root> `
  -ProtectedMonitorConcurrentWork Present

.\Test-PBLocalDesktopRegressionEvidence.ps1 `
  -EvidenceRoot <step19-evidence-root> `
  -BaselineSummaryPath <frozen-baseline>\localdesktop-reference-summary.json `
  -ExpectedBaselineSummarySha256 <64-lowercase-hex> `
  -PackageManifestPath <verified-package>\package-manifest.json `
  -ExpectedPackageManifestSha256 <64-lowercase-hex> `
  -OutputPath <new-verification-json-outside-evidence-root>
```

Step 20 runs are deliberately not collapsed into a one-machine orchestrator. Pre-stage both endpoints, create and hash one frozen plan, start the Computer A live Decoder first, start Computer B within 30 seconds, wait the full plan-defined interval after the Decoder endpoint reports evidence-ready before manually stopping B, then run A offline and the final verifier. Exact commands and the 1/2/5 Hz policies are in `docs/REMOTE_VISUAL_STEP20_REAL_REMOTE_PILOT.md`.

Step 21 begins by creating and hashing the canonical 41-cell MatrixSpec before any run. For the explicitly authorized dual-2560x1440 Computer A environment, a create-only HardwareScope additionally freezes the 31 included cells and ten 1.5x exclusions before execution. Each included cell then uses a new RunId/deployment/UI evidence/PilotPlan.2 and ends in either the success verifier or the classified-failure verifier; preflight-blocked directories do not fill a cell. Only after all 31 sealed included roots exist may the hardware-scoped matrix verifier produce its CSV/summary/seal, which preserves the parent MatrixSpec, scope, catalog and exclusions. The exact cell schedule, commands, failure predicates and dual-monitor prerequisites are in `docs/REMOTE_VISUAL_STEP21_PROVIDER_GENERIC_MATRIX.md`.

Python regression command (run from this directory):

```powershell
D:\Python3.12.9\python.exe -B -m unittest -v `
  test_analyze_remote_capture.py `
  test_seal_remote_visual_dataset.py `
  test_build_codec_corpus.py `
  test_build_step06_corpus.py `
  test_build_step07_calibration.py
```

All outputs are create-only. Existing package, evidence, replay, and `.partial` files are never overwritten or deleted. The Step 20 endpoint wrappers and final verifier create their final directories before runtime output begins so persisted absolute paths never become stale; the endpoint `*-process-result.json` or final verification seal is written last as the completion marker. Interrupted or failed directories are retained for diagnosis and cannot be reused.
