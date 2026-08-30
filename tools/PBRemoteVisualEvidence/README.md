# PBRemoteVisualEvidence

These create-only PowerShell tools support the P1.5 RemoteVisual evidence workflow without capturing screen pixels or using a non-visual payload path.

- `New-PBRemoteVisualPortablePackage.ps1` seals one deployed Encoder or Decoder directory, the exact Git/source fingerprint, the unchanged `phase1-gate-pass` identity, and a SHA-256 archive seal. It explicitly excludes the pre-existing untracked `docs/PHASE1_GATE_REPORT.md` by default.
- `New-PBRemoteVisualRunPreset.ps1 -RemoteProvider <name>` generates one OS-CSPRNG 128-bit RunId and a create-only strict `PixelBridge.RemoteVisualRunMetadata.1` JSON preset. The provider is explicit and may name any remote-control product; it is evidence-only and never selects thresholds. Copy the same preset to both endpoints; the GUI or `--remote-metadata` loads it, rejects provider/RunId conflicts, and keeps it outside the wire/acceptance path.
- `New-PBRemoteVisualSourceSet.ps1` runs on Computer B and creates fixed 1 MiB/8 MiB incompressible CSPRNG inputs plus a ZIP containing a 4 MiB CSPRNG payload. PixelBridge Segment compression remains RAW/OFF.
- `Get-PBRemoteVisualEnvironment.ps1` collects read-only OS/GPU/display metadata and the PixelBridge physical monitor catalog. It never captures pixels, takes screenshots, changes display settings, or calls a remote-provider private API.
- `analyze_remote_capture.py <existing-image>` performs a read-only, no-window diagnostic probe of one already supplied screenshot. It reports SHA-256, candidate canvas edges/scale and RGB channel spread as JSON. It accepts only regular files up to 64 MiB and decoded images up to 8192 pixels per axis / 8 MiPixels, and rejects an input that changes during analysis. Edge estimates are evidence candidates only, never profile acceptance or field certification. It requires the pinned workspace Python environment with Pillow and NumPy; run `D:\Python3.12.9\python.exe -m unittest -v test_analyze_remote_capture.py` from this directory for its resource/determinism checks.

All outputs are create-only. Existing package, evidence, replay, and `.partial` files are never overwritten or deleted.
