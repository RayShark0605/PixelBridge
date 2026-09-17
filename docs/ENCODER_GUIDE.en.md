# PixelBridge v1.0 — Encoder instructions

[简体中文](ENCODER_GUIDE.md) | **English**

## Read first

1. **Start Decoder receiving before starting Encoder.**
2. **During a large-file transfer, Decoder's received-size display can temporarily stop increasing. Repair symbols, carousel waiting, disk writes, and verification can cause normal waits: do not pause.** Explicit errors, capture interruptions, or persistent absence of useful data require investigation; not every stall is normal.
3. Encoder receives no completion ACK. Stop it manually after Decoder confirms successful completion.

## Installation and environment

- Run this package on the remote Windows x64 computer holding the source file. Extract the entire archive into a new folder and run `PixelBridgeEncoder.exe`. Do not copy only the EXE or overlay an older release.
- Requires Windows 10 1903 or newer / Windows 11, a working D3D11 graphics environment and desktop session. The Qt platform baseline is not a claim that every OS/driver combination was qualified by this project.
- Qt and MSVC runtimes are included. Developer tools, Python and PBBridge are not required. ICU, D3DCompiler_47 and Windows APIs come from Windows; stripped systems missing these components are unsupported. Do not obtain replacement DLLs from unofficial download sites.
- Use your existing remote video connection. No network or remote-control settings need to change; file content does not travel through clipboard, shared directories, or hidden network channels.
- Current single-file admission limit: **500 GiB**. Segmented streaming does not require whole-file RAM residency, but sufficient cache/output storage is necessary. This limit is not a measured maximum-file claim.

## Choosing a mode

Both endpoints must use the same mode.

| Mode | Characteristics | Recommendation |
| --- | --- | --- |
| Standard | Retained color shape/chroma route | Environments already qualified for it, or compatible workflows |
| Gray Fast | Multilevel grayscale avoids chroma distortion; its name is not a universal speed guarantee | Comparison/compatibility option, not automatically faster than PAM4 |
| PAM4 | Four luminance levels and structured cells; fixed 1920×1080 source raster | **First choice to try for 1080P non-local transfers** |
| PAM4 Wide | Separate wider PAM4 layout; fixed 2560×1440 source raster | Source desktops that fit it, including suitable 1440P/1600P displays |

Wide cannot simply be shrunk to 1080P and treated as the same mode. PAM4-family sending uses an unrotated target monitor no larger than 3840×2160. Static margins are allowed: prioritize data quality and stability rather than filling every pixel. Dimensions are physical pixels, not DPI-scaled logical coordinates.

**25 Hz** is a starting point for a new environment. The available 1–60 Hz range does not mean higher is faster: remote scaling, encoding and refresh cadence can reduce useful throughput at higher sending rates. A retained full-file result reached approximately **279 KB/s** in a particular environment (1 KB = 1024 B), not a guarantee for every mode, resolution or computer.

## Steps

1. Fully extract Decoder on the computer needing the file; select its output folder, receiving screen/region and mode, then **start receiving first**.
2. Open Encoder, select the source file, matching mode and target monitor, and set sending frequency.
3. Ensure the remote view contains the complete raster, then start sending. Initial source preparation/verification precedes stable transmission.
4. Keep the source immutable, the application running and the computer awake. Preserve the session cache. Do not change resolution/DPI/display mode or cover the raster.
5. Stop Encoder only after Decoder reports success. Carousel passes, display FPS and network traffic do not prove file completion.

## Interruptions and diagnostics

- Prefer normal Stop over force-termination. Resume a Session using the original immutable file and cache; do not substitute a modified file or another Session.
- Advanced options can open logs. Default location: `%LOCALAPPDATA%\PixelBridge\Logs\Encoder`.
- `run-<id>.events.jsonl` is the time series, usually sampled every two seconds. Normal termination attempts a matching `.summary.json`; power loss or force-termination may leave no final summary.
- For slow transfers, keep logs from both endpoints for the same Session and record file size, mode, Hz, resolutions and remote environment. Logs are bounded and rotated, so save needed evidence elsewhere promptly.
- Logs omit payload/screenshots but may include filenames, local paths and machine information; review before sharing.

## Licensing and integrity

PixelBridge-owned code is MIT-licensed; see the package's `LICENSE`. Dependencies retain their own terms: `THIRD_PARTY_NOTICES.md`, `licenses/`, `SBOM.spdx.json` and `QT_SOURCE.md`. You need not extract `sources/` again to run the application, but retain corresponding Qt source when redistributing.

The release is not publisher-code-signed. SHA-256 manifests and the external seal establish integrity and version consistency, not publisher authentication. `Test-PBUnifiedPortablePackage.ps1` is an optional read-only PowerShell 7 verifier, not a launch requirement. Do not execute binaries from untrusted sources.
