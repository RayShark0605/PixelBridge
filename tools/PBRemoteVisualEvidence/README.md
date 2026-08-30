# PBRemoteVisualEvidence

These create-only PowerShell tools support the P1.5 RemoteVisual evidence workflow without capturing screen pixels or using a non-visual payload path.

- `New-PBRemoteVisualPortablePackage.ps1` seals one deployed Encoder or Decoder directory, the exact Git/source fingerprint, the unchanged `phase1-gate-pass` identity, and a SHA-256 archive seal. It explicitly excludes the pre-existing untracked `docs/PHASE1_GATE_REPORT.md` by default.
- `New-PBRemoteVisualRunPreset.ps1 -RemoteProvider <name>` generates one OS-CSPRNG 128-bit RunId and a create-only strict `PixelBridge.RemoteVisualRunMetadata.1` JSON preset. The provider is explicit and may name any remote-control product; it is evidence-only and never selects thresholds. Copy the same preset to both endpoints; the GUI or `--remote-metadata` loads it, rejects provider/RunId conflicts, and keeps it outside the wire/acceptance path.
- `New-PBRemoteVisualSourceSet.ps1` runs on Computer B and creates fixed 1 MiB/8 MiB incompressible CSPRNG inputs plus a ZIP containing a 4 MiB CSPRNG payload. PixelBridge Segment compression remains RAW/OFF.
- `Get-PBRemoteVisualEnvironment.ps1` collects read-only OS/GPU/display metadata and the PixelBridge physical monitor catalog. It never captures pixels, takes screenshots, changes display settings, or calls a remote-provider private API.

All outputs are create-only. Existing package, evidence, replay, and `.partial` files are never overwritten or deleted.
