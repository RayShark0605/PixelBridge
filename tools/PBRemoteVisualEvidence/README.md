# PBRemoteVisualEvidence

These create-only PowerShell tools support the P1.5 RemoteVisual evidence workflow without capturing screen pixels or using a non-visual payload path.

- `New-PBRemoteVisualPortablePackage.ps1` seals one deployed Encoder or Decoder directory, the exact Git/source fingerprint, the unchanged `phase1-gate-pass` identity, and a SHA-256 archive seal. It explicitly excludes the pre-existing untracked `docs/PHASE1_GATE_REPORT.md` by default.
- `New-PBRemoteVisualRunPreset.ps1 -RemoteProvider <name>` generates one OS-CSPRNG 128-bit RunId and a create-only strict `PixelBridge.RemoteVisualRunMetadata.1` JSON preset. The provider is explicit and may name any remote-control product; it is evidence-only and never selects thresholds. Copy the same preset to both endpoints; the GUI or `--remote-metadata` loads it, rejects provider/RunId conflicts, and keeps it outside the wire/acceptance path.
- `New-PBRemoteVisualSourceSet.ps1` runs on Computer B and creates fixed 1 MiB/8 MiB incompressible CSPRNG inputs plus a ZIP containing a 4 MiB CSPRNG payload. PixelBridge Segment compression remains RAW/OFF.
- `Get-PBRemoteVisualEnvironment.ps1` collects read-only OS/GPU/display metadata and the PixelBridge physical monitor catalog. It never captures pixels, takes screenshots, changes display settings, or calls a remote-provider private API.
- `analyze_remote_capture.py <existing-image>` performs a read-only, no-window diagnostic probe of one already supplied screenshot. It reports SHA-256, candidate canvas edges/scale and RGB channel spread as JSON. It accepts only regular files up to 64 MiB and decoded images up to 8192 pixels per axis / 8 MiPixels, and rejects an input that changes during analysis. Edge estimates are evidence candidates only, never profile acceptance or field certification. It requires the pinned workspace Python environment with Pillow and NumPy; run `D:\Python3.12.9\python.exe -m unittest -v test_analyze_remote_capture.py` from this directory for its resource/determinism checks.
- `seal_remote_visual_dataset.py --input-manifest INPUT --artifact-root ROOT --output-index NEW_JSON` creates `PixelBridge.RemoteVisualDatasetIndex.1` for bounded screenshots and Replay v2 artifacts. Paths must remain below the explicit root; symlinks/junctions, protected-monitor pixels, malformed enums, duplicate JSON keys, non-finite numbers, changing files and overwrite attempts are rejected. Screenshots run through the bounded analyzer. Replay files receive an envelope check, but they are not considered semantically valid until `ReplayV2Reader`/the offline decoder succeeds; the index is never an acceptance input.
- `build_codec_corpus.py` exports a fixed three-frame production LF4 raster from `PBRemoteVisualCodecProbe`, produces five actual libx264/libx265 bitstreams, requires exactly one inspected video stream, decodes Gray8, binds source/evaluation reports to the actual sequence and per-frame bytes, and sends every frame back through production LF4/QC-LDPC/Transport diagnostic truth. Tool paths are explicit and sealed; output is create-only and bounded.
- `build_step06_corpus.py` is the one-command Step 06 entry. It combines the 22-case transform matrix, production temporal/identity corpus and actual codec corpus into `PixelBridge.RemoteVisualStep06Corpus.2`, validates both BLAKE3 and SHA-256 identities, and writes a recursive `SHA256SUMS.txt`. See `docs/REMOTE_VISUAL_STEP06_CORPUS.md` for the exact command and evidence boundary.
- `build_step07_calibration.py` is the create-only Step 07 entry. It cross-validates one sealed Step 06 corpus, one sealed Step 02 dataset/completion pair and the actual LF4 Replay before invoking `PBRemoteVisualMetricCalibration`. Dataset/run split isolation, complete post-admission sender truth, the 24-candidate policy/model matrix, validation-only selection, holdout/external improvement, full ROC/reliability curves and zero false accepted Transport/control/output are independently checked before `PixelBridge.RemoteVisualStep07Calibration.1` is published. Input paths are bounded, stable and confined below their evidence roots; symlinks and NTFS junctions are rejected. See `docs/REMOTE_VISUAL_STEP07_CALIBRATION.md` for the exact command and truth boundary.

Python regression command (run from this directory):

```powershell
D:\Python3.12.9\python.exe -B -m unittest -v `
  test_analyze_remote_capture.py `
  test_seal_remote_visual_dataset.py `
  test_build_codec_corpus.py `
  test_build_step06_corpus.py `
  test_build_step07_calibration.py
```

All outputs are create-only. Existing package, evidence, replay, and `.partial` files are never overwritten or deleted.
