# PixelBridge

[简体中文](README.md) | **English**

<p>
  <img src="apps/PixelBridgeEncoder/resources/logo.png" width="88" alt="PixelBridge Encoder">
  <img src="apps/PixelBridgeDecoder/resources/logo.png" width="88" alt="PixelBridge Decoder">
</p>

### Another way to bring a file back when you can see the remote desktop

PixelBridge is a Windows file-transfer tool. It encodes a file into a changing visual pattern and reconstructs it on another computer from the **actual screen pixels** displayed by a remote-desktop connection.

No shared directory, clipboard transfer, drive mapping, or remote-control file-transfer feature is required. Without feedback the sender displays pixels and the receiver captures, reconstructs and verifies. An available B-to-A read-only mapping can optionally carry status metadata, never forward payload.

> **PixelBridge v1.1.0** provides two separate applications, Encoder and Decoder, with Standard, Gray Fast, PAM4, and PAM4 Wide transfer modes, resumable reception, integrity verification, optional feedback assistance, and diagnostic logs on both ends.

> **Segmented streaming for large files.** The entire file does not need to fit in memory at once. The common file-byte cap is **500 GiB**, subject also to segment-count, disk and receiver budgets. **The current PAM4 / PAM4 Wide GUI defaults are limited to 448 GiB** (7 MiB × 65,536 segments). These are source-derived admission bounds, not tested maximum-file claims; see [file size and budgets](docs/DECODER_MEMORY_BUDGET.en.md).

> **Under specific conditions, average reception speed can reach approximately 279 KB/s.** This is a retained result from a complete large-file transfer using PAM4 Wide over an actual non-local remote connection, including final file verification—not an instantaneous peak. See [transfer speed](#what-transfer-speed-should-i-expect) below for the environment and measurement convention.

## Demo

![PixelBridge data pattern demo](docs/media/pixelbridge-demo.gif)

This is the data pattern actually shown by the sender: Encoder encodes the file into a continuously changing grayscale pattern and repeats it, while Decoder reconstructs and verifies the file from the captured screen pixels. The animation is recorded from a real sending session.

## Inspiration and improvements

This project was inspired by [libcimbar](https://github.com/sz3/libcimbar), which demonstrates file transfer through animated colored patterns on a screen read by a smartphone camera. PixelBridge focuses that visual-transfer idea on **Windows non-local remote-desktop workflows**, with improvements tailored to this use case:

- **Direct remote-desktop reception:** capture the actual screen pixels displayed by the remote window, without a phone or camera, to bring a file from the remote computer to the local one.
- **Picture encoding designed for video compression:** Gray Fast, PAM4, and PAM4 Wide reduce reliance on a color data channel, with optimization judged by the reception speed of a correctly reconstructed final file.
- **An integrated large-file workflow:** segmented streaming, bounded memory budgets, resumable recovery, whole-file verification, graphical applications on both ends, and diagnostic logs support long transfers and troubleshooting.

These advantages target remote-desktop and large-file reception needs. The projects have different typical use cases; speed figures from different environments are not treated as a direct performance comparison.

## When is it useful?

- You can view a remote desktop, but do not have a convenient file-transfer channel.
- Your remote-desktop or virtual-desktop (VDI) workflow needs a way to bring a file back through the existing visible desktop.
- You need resumable reception and a verified final file, with diagnostics for larger transfers.
- Your receiving computer has one monitor: start reception, then make the remote data pattern visible over the selected screen. No reserved area for the Decoder window is required.
- When Encoder starts it keeps a per-user `PixelBridgeEncoder` context-menu verb; a selected folder is first archived as a standard ZIP and then follows the same visual transfer and final-verification chain as an ordinary file.

PixelBridge is not a remote-control application. It neither establishes the remote connection nor changes remote-control or network settings. It does not use a camera. If direct file copying is available, ordinary file transfer is usually faster and easier.

## Two applications, two roles

| Application | Computer | Responsibility |
| --- | --- | --- |
| **PixelBridgeEncoder** | Remote computer containing the file | Select the file and display a repeating visual stream |
| **PixelBridgeDecoder** | Computer that needs the file | Capture a selected screen/region and save the reconstructed file |

The file travels from remote computer **A (Encoder)** to receiving computer **B (Decoder)**. By default there is no return channel: **wait for Decoder to report completion, then stop Encoder manually**. If A can read a folder on B, [optional feedback](#optional-file-feedback-assistance) can guide missing-segment service and authorize safe stopping after final verification.

The Encoder GUI also supports a maximum run duration in seconds: `0` runs until a human stops it; a non-zero value safely stops at its limit. Timeout is not proof of successful reception.

## Download and run

Download `PixelBridge-v1.1.0-Encoder-win64.zip` and `PixelBridge-v1.1.0-Decoder-win64.zip` from the project's Releases. Extract each complete archive into a new folder on its computer, then run `PixelBridgeEncoder.exe` or `PixelBridgeDecoder.exe`.

- Windows x64 with working D3D11 graphics and desktop capture is required. Runtimes are included; Qt, Visual Studio and Python installations are not needed.
- Do not copy just the EXE or mix old DLLs. Keep sibling `PBFeedbackIo.exe`, Qt plugins and runtime libraries at both endpoints.
- Retain licenses, dependency inventories and the matching Qt source in `sources/` when redistributing. SHA-256 and external seals establish integrity, not code signing.

## Quick start

1. Start the appropriate application on each computer and choose **the same mode** on both ends.
2. In Decoder, select an output directory, monitor, and whole-screen or region capture. If using feedback, configure both ends as described below.
3. **Start Decoder receiving first.**
4. Select the source file in the remote Encoder, or right-click a file/folder in File Explorer and choose `PixelBridgeEncoder`; confirm the sender rate, mode, and timeout in the small dialog. Keep the complete pattern visible; avoid panels, overlapping windows, and pointer obstruction.
5. For a folder selection, Decoder receives the verified ZIP file; extract it yourself after final reopen succeeds.
6. Without feedback, stop Encoder after Decoder confirms completion. With feedback enabled, leave B listening until A accepts the verified final receipt and stops, then stop B's listener.

After Decoder is started, its GUI active elapsed time, average speed, and ETA exclude the wait before the first accepted same-profile `SessionDescriptor`. The active window includes recovery, disk writes, final publication, and final reopen verification. On completion, the GUI average is original file bytes divided by the frozen active elapsed time. Formal `runStarted/runEnded` and verified raw goodput retain their existing evidence meanings.

**The received-size display may temporarily stop increasing during a large transfer.** When the UI reports segment repair, verification, writing, or carousel waiting, this is an expected wait: **keep receiving; do not pause**. An explicit capture interruption, lack-of-progress warning, or error must instead be investigated, not dismissed as normal.

See the [user guide](docs/USER_GUIDE.en.md) for operation, resume, and precautions.

## Optional file-feedback assistance

**Feedback is not required for normal transfer.** If A can read a folder on receiving computer B, even with several seconds of visibility delay, optional metadata feedback can reduce redundant service to completed segments and safely stop Encoder after receiver verification.

1. **B: Decoder → Advanced → Optional feedback.** Select a dedicated feedback subfolder/file separate from output and recovery state. Folder mode uses `PixelBridgeFeedback.pbfb`.
2. **A: Encoder main window or Shell sending dialog.** Enable read-only feedback and select the corresponding mapped folder/file; UNC text such as `\\Client\C$\…` can be pasted directly. Path strings may differ but must identify the same feedback location.
3. Match visual modes and **start Decoder before Encoder**. B publishes after visual identity and output paths are known; A need not wait for active feedback before sending.
4. When B says “Completed, feedback listening”, keep it listening until A accepts the final receipt and stops, then stop B's listener. **The output can be read/viewed while listening; stop listening before editing, moving or deleting it.**

All four modes support this default-off feature. A only reads metadata: no source bytes, encoded payload or pixels enter the return file. All forward payload still travels through visible pictures. Unavailable, expired or rejected feedback restores ordinary visual service; an old completion message cannot simply stop a new Start. Use a trusted location managed by the designated Decoder, not one arbitrary untrusted writers may modify.

Benefits depend on the mapping and transfer environment. See the [feedback guide](docs/FEEDBACK_ASSISTED_TRANSFER.en.md) for setup, trust and failure handling.

## Resolution and mode selection

**For non-local transfer with a 1080P display, try PAM4 first, select it on both ends, and start with a sending rate of 25 Hz.** On a fresh install, or when `g22/carrier` is missing or invalid, both GUI selectors default to PAM4 (index 2); valid saved indices are restored unchanged. Standard index 0 remains the compatibility mode. CLI/core defaults are separate from the GUI default.

### How do Standard, Gray Fast, and PAM4 differ?

They are three different ways to encode data into a picture, not three picture-quality settings for the same mode:

| Mode | How the picture carries data | When to use it and the trade-off |
| --- | --- | --- |
| **Standard** | Small patterns carry data through **shape and color** | Compatibility-preserved index 0; existing settings remain usable. Remote-video compression can damage the color channel, so it is not the first recommendation for 1080P remote transfer. |
| **Gray Fast** | Grayscale patterns carry data through **shape and brightness**, without a color data channel | Offers a higher per-frame payload, but still depends on fine pattern detail surviving the video path. Use an already reliable setup, or compare it as an alternative when PAM4 performs poorly. |
| **PAM4** | Regular blocks use four brightness levels: **black, dark gray, light gray, and white** | **The first recommendation for non-local 1080P transfer.** Uniform grayscale data cells reduce reliance on fine pattern and color detail, aiming to deliver more usable data after remote-video compression. |

**The name “Gray Fast” does not mean it is faster than PAM4 on every remote connection.** Packing more data into a frame can increase total reception time if more of that data is lost during video compression. This recommendation follows the current implementation and retained tests; it is not a universal speed ranking. If PAM4 performs poorly, compare Gray Fast manually using the same small file and environment, judging by verified whole-file completion time. All three modes retain final file-integrity verification rather than trading it away for speed.

### PAM4 Wide and resolution

**PAM4 Wide is a larger-raster version of PAM4, not a higher setting intended for 1080P.** It uses the same four-level grayscale principle and a larger display area to carry more data:

| Mode | Fixed sender raster | Suitable sender desktop |
| --- | --- | --- |
| PAM4 | 1920×1080 | 1080P, or centered on a larger desktop |
| PAM4 Wide | 2560×1440 | A desktop that fits the complete raster, such as 1440P / 1600P |

These are the **physical desktop dimensions of the computer containing the file**, not the Decoder window size. **For a 1080P sender, choose PAM4, not Wide; do not shrink Wide to fit 1080P.** Standard and Gray Fast also use a 1920×1080 base raster. Decoder supports whole-screen or region capture, but remote scaling, quality, and delivered pixels affect reception; display resolution alone cannot guarantee speed. A rate of 25 Hz is a starting point, not a universal optimum, and sending faster is not necessarily better.

The current PAM4-family GUI also requires an unrotated sender display no larger than 3840×2160. Supported dimensions have explicit lower and upper bounds; arbitrary resolutions are not implied.

Single-monitor reception is implemented. An independent computer with only one physical monitor has not been available for qualification; testing one selected monitor on a dual-monitor computer is not that qualification. See [support and validation scope](docs/PROJECT_STATUS.en.md).

## Can a computer with more RAM help?

Decoder's advanced options offer **memory-budget-based reception** for Gray Fast, PAM4, and PAM4 Wide. Set the total active-decoder budget and per-instance limit, or request a recommendation based on currently available memory.

This can reduce admission delays caused by occupied decoder slots. Filling all RAM is not the goal: the OS, remote application, capture buffers, and resume data need headroom. Paging can make reception slower. See [memory budgets](docs/DECODER_MEMORY_BUDGET.en.md).

## What transfer speed should I expect?

Speed depends on the **entire remote-video path**, not one universal throughput figure.

Retained non-local tests achieved the following **average verified whole-file reception speeds**:

| Mode and sender resolution | Average reception speed | Test scope |
| --- | ---: | --- |
| **PAM4 Wide · 2560×1600** | **About 279 KB/s** | Complete large file, approximately 1.12 GiB |
| **PAM4 · 1920×1080** | **About 190 KB/s** | A 91 MiB small file; not a large-file speed guarantee |

Both records used ToDesk Professional at HD quality and a 25 Hz sending rate, with a 2560×1440 local receiving display. **The retained records use 1 KB = 1024 bytes.** Each average divides original file size by full receiver runtime, including startup waiting, repair, disk writes, whole-file verification, and final reopen verification. They are neither peak rates nor theoretical bandwidth, and use a different time window from GUI active-reception statistics.

These measurements describe the listed test conditions, not a comparison using the same resolution and file for both modes. **The approximately 279 KB/s Wide result is not a speed promise for 1080P PAM4.** Neither these speeds nor continuously increasing progress are guaranteed on arbitrary remote connections. See [evidence and limitations](docs/EVIDENCE_INDEX.md#readme-speed-reference) for versions, original values, and verification details.

## Troubleshooting

Normal GUI and transfer CLI runs automatically write low-frequency diagnostic logs. Use **Open log directory** in the advanced tab, or export a report manually.

- Default location: `%LOCALAPPDATA%\PixelBridge\Logs\Encoder` or `Decoder`.
- Logs describe sending, capture, decoding, resource waits, and final verification. They do not store screenshots or file contents.
- Final reports can contain filenames, local paths, session identifiers, and environment information. Review them before sharing.

See [diagnostics](docs/DIAGNOSTICS.en.md).

---

## How it works

```text
Remote file → Segmentation/compression → Erasure/error coding → Visible pattern
                                                                   ↓ Remote video
Local file  ← Integrity verification ← Segment recovery ← Captured screen pixels
```

- **Visual-only forward payload:** recovered content comes only from captured pixels, without a hidden file channel. There is no ACK by default; optional file feedback carries receiver metadata only.
- **Tolerates missing pictures:** carousel scheduling and fountain repair let the receiver continue collecting useful data after frame loss.
- **Segmented, bounded, resumable:** memory need not scale with the full file; verified segments are written to disk and recovery state is validated on resume.
- **Correctness first:** transport CRC, segment digests, and whole-file BLAKE3 are separate checks. Completion requires verified publication and final reopen verification.

### Main engineering challenges

1. **Remote desktops transport video, not original pixels.** Chroma subsampling, lossy coding, scaling, duplicate frames, and partial updates distort a data pattern. PAM4 uses separately identified four-level grayscale encoding with visible geometry, timing, and calibration regions.
2. **Faster sending is not necessarily faster receiving.** Higher frequency or a larger changing area can increase video-encoding pressure and reduce useful delivered data.
3. **Feedback depends on the environment.** Without it the transfer tail may wait for another repair pass. A read-only return mapping permits durable segment hints, but delay, stale state and fallback still need handling.
4. **Keeping more decoders active has costs.** It can improve admission but increases memory, checkpoint, and compaction work. Resource bounds and crash consistency must remain intact.

Read the [architecture and technical route](docs/ARCHITECTURE.en.md) for parameters, protocol invariants, capture lifetime, persistence, and measurement boundaries.

## Building and development

Windows x64, C++20, MSVC, Qt Widgets, CMake, and vcpkg. See [development instructions](CONTRIBUTING.md) and the [documentation index](docs/README.en.md).

Research tools and PBBridge are for development orchestration and evidence, not extra product payload channels. Offline MP4, camera reception, arbitrary scaling/HDR, and cross-platform reception are outside the supported feature scope.

## License

See the [release notes](docs/RELEASE_V1.1.md) for current package contents, validation scope and known limits.

This project uses the **[MIT License](LICENSE)**. Use, modification, and redistribution, including commercial use, are permitted subject to preserving the copyright and license notices. The software is provided “as is” under the license text.

Third-party components such as Qt retain their own licenses. Release packages include original notices, dependency inventories, and corresponding Qt source. See [third-party components and licensing](THIRD_PARTY_NOTICES.md).

## Acknowledgements

Special thanks to **GPT-5.6 Sol, GPT-6 Astra, GLM-5.3, and Qwen 3.8 Flash Next**, and the teams behind them, for their assistance and inspiration during this project's exploration and development! Thanks also to libcimbar, the contributors to our open-source dependencies, and the users who tested the software and provided feedback. See the full [acknowledgements](ACKNOWLEDGEMENTS.md).
