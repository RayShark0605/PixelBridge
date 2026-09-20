# PixelBridge v1.0 — Decoder instructions

[简体中文](DECODER_GUIDE.md) | **English**

## Two important reminders

1. **Start Decoder receiving before starting Encoder.** Waiting for the sender's picture is normal and helps avoid missing initial control information.
2. **While receiving a large file, the received-size display can temporarily stop increasing. This can be normal during segmented recovery: do not pause because of it.** Read the current activity: repair collection, carousel waiting, segment verification, disk writes and whole-file verification require patience. Explicit errors, capture interruptions and persistent absence of useful blocks are not automatically normal.

## Installation and environment

- Fully extract into a new folder on the Windows x64 computer needing the file, then run `PixelBridgeDecoder.exe`. Do not copy only the EXE, mix DLL versions or overlay an older installation.
- Requires Windows 10 1903 or newer / Windows 11, working D3D11 graphics/desktop capture, and your existing remote view. Not every OS, driver, scale factor or remote provider has been separately qualified.
- Qt and MSVC runtimes are included; Python, developer tools and PBBridge are unnecessary. Windows supplies ICU, D3DCompiler_47 and OS APIs. Stripped systems missing these components are unsupported.
- Choose a writable output folder with enough free storage. A large-file-capable NTFS volume is recommended. The current **500 GiB** single-file admission limit is not a measured maximum; allow extra space for temporary/resume state.
- Payload is recovered only from actual pixels in the selected region: no hidden network, clipboard or shared-file transport. No network/remote-control setting changes are needed.

## Receiving steps

1. Select the output directory and the same visual mode as Encoder.
2. Select the monitor actually displaying the remote view, then whole-screen or a contained region covering the complete raster. Do not crop localization/calibration areas.
3. To use more RAM, enable memory-budget receiving in Advanced options; apply the host recommendation or set total/per-instance budgets. Settings lock while receiving.
4. **Click Start receiving first, then start the remote Encoder.**
5. Keep the full raster visible; close floating diagnostic overlays. Avoid sleep and resolution/DPI/display-mode changes. Occlusion, compression and scaling affect useful data.
6. Wait for Decoder's explicit successful completion before stopping Encoder and using the final file. Growing `.part`/`.resume`, a recovered segment or estimated progress is not final success.

After “Start receiving”, the GUI active elapsed time, average speed, and ETA remain unavailable/zero while waiting for the first `SessionDescriptor` accepted and validated for the current Profile. The active window continues through recovery, disk writes, whole-file digest, safe publication, and final reopen verification; on completion the average is original file bytes divided by the frozen active elapsed time. Formal `runStarted/runEnded` and verified raw goodput retain their full-run/performance evidence meanings.

## Modes, resolutions and one-monitor use

- **For 1080P non-local use, try PAM4 first, starting Encoder at 25 Hz**. This is a starting recommendation, not a speed guarantee across remote environments.
- PAM4 uses a fixed 1920×1080 source raster. PAM4 Wide uses 2560×1440 and needs a sufficiently large source desktop, such as suitable 1440P/1600P. Wide cannot run directly on a 1080P source desktop.
- Standard retains the color shape/chroma route. Gray Fast uses multilevel grayscale to avoid chroma distortion; it is not necessarily faster than PAM4. Endpoint modes must match.
- PAM4-family reception supports a selected whole monitor or contained region without a second protected monitor. On one display, the remote view may cover Decoder while receiving continues in the background; restore the complete view after checking progress.
- The program does not move/minimize windows or simulate input. A computer with only one physical monitor has not been independently qualified; selected-single-monitor entry was tested on a dual-monitor machine.
- Changed display identity requires re-selection instead of silently capturing another screen. Dimensions are physical pixels.

## Memory and waiting

Without budget mode, the legacy limit remains eight active decoders. Gray Fast/PAM4/Wide support budget mode, defaulting to 1024 MiB total and 512 MiB per instance. More RAM can retain more unfinished segments, but this budget is not the process's entire RAM cap. Excessive allocation can cause paging and reduce speed: prefer the host recommendation and leave headroom for Windows, remote video and other applications.

| Status | Action |
| --- | --- |
| Repair, carousel, verification, disk write, checkpoint | Normal recovery work: keep waiting, do not pause |
| Resource deferral | Wait; if frequent, inspect the budget before the next reception |
| No new useful blocks/verified segments for 30 seconds or more | Keep receiving while checking Encoder, occlusion and connection; retain logs; normality is not guaranteed |
| Capture interruption or explicit error | Investigate the message rather than merely waiting; preserve resume state and logs |

Received size combines verified bytes with in-progress estimates; it need not increase for every frame. Duplicates add no independent information, and an estimate may plateau before a segment becomes recoverable. Whole-file digest, safe publication and final reopen remain necessary. More RAM cannot fix video distortion, frame loss or occlusion.

## Stop, resume and logs

- Prefer normal Stop and preserve `.part`/`.resume` in the original output directory. These are recovery data, not disposable diagnostic logs.
- Resume the same Session with Encoder's original immutable source/cache. Decoder revalidates persisted state. Existing final files are not silently overwritten.
- Do not edit resume data or mix Sessions. A reduced budget can reject older state; restore a suitable budget before retrying.
- Advanced options open the log folder: `%LOCALAPPDATA%\PixelBridge\Logs\Decoder`. Files are `run-<id>.events.jsonl` and the terminal `.summary.json`.
- Logs normally sample every two seconds with size/time/retention bounds. Force-termination can leave no summary. Promptly save both endpoints' logs for the same Session, together with mode, Hz, file size and display environment.
- Logs contain no payload/screenshots but can include paths, filenames and machine information. Review privacy before sharing.

## License and verification

`LICENSE` contains MIT terms. Dependency terms are in `THIRD_PARTY_NOTICES.md`, `licenses/` and `SBOM.spdx.json`. Qt source/replacement instructions are in `QT_SOURCE.md` and `sources/`. Running does not require extracting source; retain it for redistribution.

The package is not publisher-code-signed. SHA-256/external seals establish integrity and consistency only. The optional PowerShell 7 `Test-PBUnifiedPortablePackage.ps1` is read-only and does not run packaged EXEs. Ordinary use requires neither PowerShell 7 nor running this verifier first.
