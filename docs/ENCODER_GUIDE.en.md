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
- When Encoder starts normally it idempotently checks and registers a `PixelBridgeEncoder` context-menu verb for files and folders in the current user's Windows Shell. This is a per-user persistent registration and does not require administrator rights. Windows 11 may place a legacy static verb under “Show more options.”
- Current single-file admission limit: **500 GiB**. Segmented streaming does not require whole-file RAM residency, but sufficient cache/output storage is necessary. This limit is not a measured maximum-file claim.

## Choosing a mode

Both endpoints must use the same mode.

On a fresh install, or when the `g22/carrier` preference is missing or invalid, the Encoder GUI defaults to PAM4 (index 2); valid saved indices are restored unchanged. Standard remains compatibility index 0, and CLI/core defaults are separate from the GUI default. The Decoder must select the same mode and Profile identity.

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
2. Open Encoder. You can select a file in the normal Encoder window, or right-click any file or folder in File Explorer and choose **PixelBridgeEncoder**; the shell entry opens a small dialog for sender rate, mode, and timeout.
3. When a folder is selected, Encoder first creates a standard ZIP archive locally and then sends that ZIP as an ordinary file. After Decoder completes its digest, safe publication and reopen verification, extract the ZIP yourself. The original folder is never sent through a side channel or shared directory.
4. Choose the matching Decoder mode and target monitor, and set sending frequency. If desired, enter a non-negative integer in **Maximum run duration (seconds)** for an automatic safe stop.
5. Ensure the remote view contains the complete raster, then start sending. Initial source preparation/verification precedes stable transmission.
6. Keep the source immutable, the application running and the computer awake. Preserve the session cache. Do not change resolution/DPI/display mode or cover the raster.
7. Stop Encoder only after Decoder reports success. Carousel passes, display FPS and network traffic do not prove file completion.

### Automatic stop timeout

- **Maximum run duration (seconds)** is local Encoder policy for this run. It is not written into the visual raster, SessionDescriptor, FEC, or any other protocol field.
- `0` disables the timeout and keeps the sender running until a human stops it or an existing error occurs. A non-zero value starts when this Start request is accepted and includes source preparation as well as broadcasting.
- The GUI accepts non-negative integer seconds in the `uint64_t` range; there is no 7200-second or other product-specific ceiling. Empty, negative, non-numeric, and overflowing values reject Start.
- When the limit is reached, Encoder follows the normal safe-stop path and keeps the resumable Session. **This does not mean Decoder completed**; wait for Decoder's whole-file digest, safe publication, and final reopen verification.
- To continue, use the original unchanged source and Session cache. Do not treat a timeout stop as receiver completion.

## Interruptions and diagnostics

- Prefer normal Stop over force-termination. Resume a Session using the original immutable file and cache; do not substitute a modified file or another Session.
- Advanced options can open logs. Default location: `%LOCALAPPDATA%\PixelBridge\Logs\Encoder`.
- `run-<id>.events.jsonl` is the time series, usually sampled every two seconds. Normal termination attempts a matching `.summary.json`; power loss or force-termination may leave no final summary.
- For slow transfers, keep logs from both endpoints for the same Session and record file size, mode, Hz, resolutions and remote environment. Logs are bounded and rotated, so save needed evidence elsewhere promptly.
- Logs omit payload/screenshots but may include filenames, local paths and machine information; review before sharing.

## Licensing and integrity

PixelBridge-owned code is MIT-licensed; see the package's `LICENSE`. Dependencies retain their own terms: `THIRD_PARTY_NOTICES.md`, `licenses/`, `SBOM.spdx.json` and `QT_SOURCE.md`. You need not extract `sources/` again to run the application, but retain corresponding Qt source when redistributing.

The release is not publisher-code-signed. SHA-256 manifests and the external seal establish integrity and version consistency, not publisher authentication. `Test-PBUnifiedPortablePackage.ps1` is an optional read-only PowerShell 7 verifier, not a launch requirement. Do not execute binaries from untrusted sources.
