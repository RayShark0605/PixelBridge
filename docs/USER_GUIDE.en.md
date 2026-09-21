# PixelBridge user guide

[简体中文](UNIFIED_USER_GUIDE.md) | **English**

## 1. Preparation

Run Encoder on the remote Windows x64 computer containing the file and Decoder on the Windows x64 computer that needs it. Extract the **entire application directory** from the delivery package; do not copy only the EXE, mix old Qt DLLs, or overlay incompatible versions. v1.0 delivers Encoder and Decoder separately, each with standalone Chinese and English instructions.

Use your existing remote connection. PixelBridge neither requires changes to remote/network/driver settings nor creates a hidden file channel. When Encoder starts normally it checks and keeps a per-user `PixelBridgeEncoder` context-menu verb for files and folders; Windows 11 may expose the legacy static verb under “Show more options.” The verb passes only the selected path to Encoder as a launch argument. Prepare a writable output directory and enough disk space. Do not change the source or delete Encoder's session cache or Decoder's `.part` / `.resume` files during a transfer.

## 2. Mode selection

Choose the same Standard, Gray Fast, PAM4, or PAM4 Wide mode on both ends. Mode names have no research-stage labels; choose one suitable for the physical source raster.

On a fresh install, or when `g22/carrier` is missing or invalid, both GUI selectors default to PAM4 (index 2). Valid saved indices remain unchanged (0=Standard, 1=Gray Fast, 2=PAM4, 3=PAM4 Wide). This is only the GUI's initial choice; CLI/core defaults and Profile/Wire identities are unchanged.

- A 1080P sender can use PAM4's fixed 1920×1080 source raster.
- A 1440P/1600P sender can fit Wide's fixed 2560×1440 source raster.
- Wide cannot simply be shrunk to 1080P. Fullscreen windows may retain static borders; speed/correctness takes priority over filling the display.
- Requirements refer to physical pixels, not DPI-scaled logical window dimensions.
- The current PAM4-family GUI requires an unrotated sender display no larger than 3840×2160; unsupported geometry is not hidden by silently shrinking the raster.
- Existing Standard/Gray sessions retain their compatible modes; they cannot be mixed with PAM4 sessions.

Sender cadence is configurable from 1–60 Hz and locked during a run. The retained ToDesk large-file result used Wide at 25 Hz; that is not a universal optimum. 60 Hz is not necessarily faster. Incompatible identity changes require a new Session.

Encoder's **Maximum run duration (seconds)** is local sender control: `0` means no timeout and the sender continues until a human stops it; a non-zero non-negative integer starts when this Start is accepted, covers preparation and broadcasting, and ends through the normal safe-stop path. The only limit is the `uint64_t` representation range; there is no 7200-second ceiling. It is not part of the visual raster or Session and does not mean Decoder completed.

## 3. Start Decoder first

1. Select Decoder's output directory.
2. Select the monitor displaying the remote desktop, and whole-screen or a region containing the complete pattern. Do not crop markers/calibration regions.
3. Configure optional memory budgets before starting.
4. **Start Decoder receiving first.** Waiting before Encoder shows its pattern is expected.
5. On the remote computer, select the file in Encoder, verify the target display, and start sending.
6. Keep the complete pattern visible. Close remote diagnostic panels, floating tools, and other obstructions.
7. After Decoder explicitly reports completion, stop Encoder manually.

You can also right-click a file or folder in File Explorer and choose **PixelBridgeEncoder**. The small dialog asks only for sender rate, mode, and timeout. A selected folder is first archived locally as a standard ZIP and then follows the existing visual transfer, digest, safe-publication, and final-reopen flow as an ordinary file. Decoder does not extract it automatically; extract the verified ZIP yourself.

The Decoder GUI's active elapsed time starts when it first accepts a `SessionDescriptor` validated for the current Profile. Waiting after clicking “Start receiving” but before the Encoder shows a valid coded picture is excluded. The active window includes recovery, disk writes, whole-file digest, safe publication, and final reopen verification. On completion, the GUI average is original file bytes divided by the frozen active elapsed time; formal `runStarted/runEnded` and verified raw goodput retain their full-run/performance evidence meanings.

Encoder receives no completion ACK. Multiple carousel passes, CRC success, or a growing `.resume` file are not proof of a completed file.

## 4. One physical monitor

PAM4-family GUI reception supports the selected whole monitor or a valid region without a second protected monitor or reserved Decoder area. Start receiving, then put the visible remote pattern over the Decoder window; capture can continue in the background.

PixelBridge does not move/minimize windows, steal focus, or inject input. You arrange visibility. If checking progress temporarily obstructs the pattern, restore it afterward. Independent single-physical-monitor hardware has not been available for qualification; selected-screen operation on a dual-monitor computer has been tested.

Display/device/DPI changes invalidate the current capture authority and may require stopping and reselecting. The program does not silently switch to another monitor.

## 5. When received size stops increasing

The received-size display combines verified bytes and an in-flight estimate. **Not every captured frame increases it.** Duplicate blocks add no independent information. A segment may need further repair equations after its estimate is nearly full. Reaching 100% also requires whole-file verification and reopen.

| Activity | Meaning | Action |
| --- | --- | --- |
| Collecting repairs | New useful equations are arriving; segment incomplete | Expected wait; do not pause |
| Waiting for carousel | Repeated/completed segments observed; missing repair pending | Short waits are expected; keep receiving |
| Segment verification, writing, checkpoint | Actual synchronous recovery/storage work | Do not pause or force termination |
| Whole-file verification/publication/reopen | Final acceptance checks | Wait before using the result |
| Resource backpressure | Active state occupies the configured budget | Wait; inspect budget if frequent |
| Capture interruption/no fresh pattern | New usable capture/visual data unavailable | Check visibility, connection, mode, and Encoder |
| At least 30 s without a new useful block/verified segment | Evidence insufficient to call the wait normal | Keep receiving, investigate, save both logs |
| Explicit error | Reception cannot proceed normally | Follow the error; do not dismiss it as a normal wait |

**Do not pause during expected waits.** Repeated stop/restart cycles can add verification cost and miss upcoming patterns. Preserve logs and resume files before troubleshooting abnormal behavior.

## 6. Memory budgets

Gray Fast/PAM4/Wide offer memory-budget-based reception in advanced options. Disabled: eight active decoders, 1024 MiB total and 512 MiB per-instance limits.

The recommendation uses currently available physical and commit memory. A budget is not a hard process-RAM cap. More memory can retain more unfinished segments, but paging can reduce speed. [Budget details](DECODER_MEMORY_BUDGET.en.md)

## 7. Stop and resume

- Prefer normal stop over forced termination so verifiable recovery state can be preserved.
- Resume Encoder with the original unchanged source and session cache; resume Decoder in the original output directory.
- Do not edit `.resume`, move `.part` arbitrarily, or treat another Session as the old one.
- Resume revalidates state and completed segments; this can take time without retransferring those bytes.
- A smaller budget may reject an older larger resume state. The old files are preserved, not silently discarded; restore a suitable budget.
- Accept only a file whose digest, safe publication, and final reopen checks pass. Existing final files are not silently overwritten.

## 8. Logs

Use **Open log directory** in advanced options. Automatic files are `run-<id>.events.jsonl` plus a matching `.summary.json` on normal stop/failure/completion. Snapshots normally arrive every two seconds and include activity plus useful/duplicate/resource/capture counters. [Diagnostic guide](DIAGNOSTICS.en.md)

For investigation, collect both roles' logs, matching Session ID, size, mode, cadence, and environment description. Review filenames/paths before sharing. Source/recovered files or screenshots are not substitutes for diagnostics.

## 9. Precautions

- **Decoder first, Encoder second.**
- **Large-file received size may temporarily plateau; do not pause expected waits.**
- Do not obscure/crop the pattern or change display modes mid-transfer.
- Screen FPS and remote-network traffic are not file goodput.
- Small files may not exercise large-file scheduling/tails; speed claims require full-file results in the relevant environment.
- This delivery does not promise arbitrary providers, resolutions, scaling, HDR, or the historical offline-MP4 design.
