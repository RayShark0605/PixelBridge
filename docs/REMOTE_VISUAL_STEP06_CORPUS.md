# PixelBridge RemoteVisual Step 06 Impairment Corpus

状态：**Step 06 离线 impairment/adversarial corpus 已实现并可从空目录一条命令重建；implementation commit 为 `f6e4769e8d6339acd0b274c09173199fd8aaf6b4`。这不是双机 field PASS，也不是 Certified RemoteVisual Profile。**

## 1. 结论与证据边界

本步骤把此前仅有的 deterministic proxy matrix 扩展为三条互相独立、最终汇总到同一个 seal 的证据链：

1. **空间/颜色/块更新 transform matrix**：15 个 case，包含 area、bilinear、固定 Q16 Catmull-Rom bicubic、fractional scale/phase、blur/ringing、gamma/range、4:2:0 proxy、crop/letterbox/overlay、block replacement 和 temporal blend。
2. **实际 codec bitstream corpus**：5 个真实 libx264/libx265 Matroska bitstream；ffprobe 对 codec、pixel format、BT.709 limited range、几何、帧/packet 和 keyframe 进行核验；解码 Gray8 再进入 production LF4→QC-LDPC→Transport diagnostic truth boundary。
3. **sequence/identity corpus**：直接复用 production `VisualIdentityTracker`、`RemoteDuplicateRefinementGate` 和 `ChannelStallTracker`，覆盖 duplicate、gap、reorder、CaptureEpoch、erasure 后 duplicate refinement，以及 CRC 有效但 identity 错误的 Transport。

三个入口都不启动窗口、capture 或 display API，不读取桌面，不接收 expected payload 作为 decoder 输入，不改变 frozen wire/FEC/CRC/digest。所有输出只属于离线实验与回归证据。

## 2. 一键重建

在 Release tools 已构建、FFmpeg/FFprobe 路径显式给出的前提下，从一个尚不存在的 output directory 运行：

```powershell
D:\Python3.12.9\python.exe `
  tools\PBRemoteVisualEvidence\build_step06_corpus.py `
  --channel-matrix build-p1_5-evidence\20260831-step06-headless-release-final\tools\Release\PBRemoteVisualChannelMatrix.exe `
  --temporal-corpus build-p1_5-evidence\20260831-step06-headless-release-final\tools\Release\PBRemoteVisualTemporalCorpus.exe `
  --codec-probe build-p1_5-evidence\20260831-step06-headless-release-final\tools\Release\PBRemoteVisualCodecProbe.exe `
  --ffmpeg D:\vcpkg\buildtrees\ffmpeg\x64-windows-rel\ffmpeg.exe `
  --ffprobe D:\vcpkg\buildtrees\ffmpeg\x64-windows-rel\ffprobe.exe `
  --output-dir <new-output-directory>
```

输出包含：

- `channel-matrix.json`；
- `temporal-corpus.json`；
- `actual-codec-corpus/` 下的 source、5 个 bitstream、Gray8、ffprobe、evaluation 和嵌套 seal；
- `step06-corpus-index.json`；
- 根级 `SHA256SUMS.txt`。

脚本严格校验 duplicate JSON member、非有限数值、schema/version、canonical member order、payload BLAKE3、固定 summary contract、source/evaluation report 与实际 BGRA/Gray8 字节的逐序列及逐帧 BLAKE3 绑定、唯一视频流及 frame/packet inspector contract、工具运行前后 SHA-256 identity、进程 timeout、stdout/stderr/file size 和 create-only output。它不会自动发现 provider，也不会按 provider 或机器选择 threshold。

## 3. Bicubic 精确定义

`PBRemoteVisualSimulator` 的新增 filter 名称为 `bicubic-catmull-rom-q16`，只要出现该 filter，整份 channel manifest 使用 v2。数学定义见 `REMOTE_VISUAL_CHANNEL_MANIFEST.md`；关键不变量是：

- Catmull-Rom `a=-1/2`；
- fractional phase Q16；
- kernel weights Q20 且权重和严格为 `1<<20`；
- 4×4 / 16 taps per output pixel；
- signed half-away rounding；
- extent 外使用显式 border，extent 内 edge-clamp；
- identity transform 仍逐字节保持 active BGRA；
- area/bilinear v1 manifest 和数学语义不变。

单元测试使用硬编码 1-D truth vector `[0,10,42,93,163,214,245,255]`，并覆盖 B/G/R/alpha、identity、manifest v2 和 16-tap work limit。

## 4. 实际 codec corpus

固定 source 是 3 个 production LF4 BGRA frame：1920×1080、2 FPS、FrameSequence 900..902、同一固定 SessionTag。每个 bitstream 都显式关闭音频/字幕/data、metadata、非确定线程行为与 B-frame，并固定 BT.709 limited-range metadata。FFprobe inspector 证明实际产生的 codec/pixel format，而不是信任命令行意图。

本轮固定结果：

| case | inspector | keyframes | Verified | Erasure | accepted Transport | diagnostic nontruth |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| `h264-420-crf18-intra` | H.264 / yuv420p | 3 | 1/3 | 2/3 | 4 | 0 |
| `h264-420-crf35-inter` | H.264 / yuv420p | 1 | 0/3 | 3/3 | 0 | 0 |
| `h264-444-crf28-inter` | H.264 / yuv444p | 1 | 0/3 | 3/3 | 0 | 0 |
| `hevc-420-crf28-inter` | HEVC / yuv420p | 1 | 0/3 | 3/3 | 0 | 0 |
| `hevc-420-crf40-inter` | HEVC / yuv420p | 1 | 0/3 | 3/3 | 0 | 0 |

结果说明当前 LF4 reference raster 对真实有损编码仍很敏感；即使 `h264-444-crf28-inter` 也没有完整恢复一帧。它是强烈的 **Signal-layer blocker 输入**，但不能外推为向日葵必然失败：这里没有远控自己的缩放、块更新、码控、帧丢弃或后处理，也没有 field calibration。

所有 15 个实际 codec frame 都遵守 production truth boundary；普通 codec corpus 的 `falseAcceptedCodewords=0`。唯一成功帧产生 4 个完整 Transport block，其余 14 帧为 erasure，无部分 publish 或伪 success。

## 5. Temporal / identity adversarial corpus

固定 11-event corpus 的权威 summary：

| 项目 | 结果 |
| --- | ---: |
| Transport blocks passed by visual-identity gate（按 CaptureEpoch 独立计数） | 16 |
| suppressed duplicate events | 3 |
| suppressed reordered events | 1 |
| gap events / skipped sequences | 1 / 1 |
| CaptureEpoch reset | 新 epoch 建立新 visual baseline |
| erasure 后 duplicate refinement | 1 次恢复，成功后后续 duplicate 被抑制 |
| wrong-identity accepted Transport blocks | 0 |
| wrong-identity diagnostic nontruth candidates | 4 |
| capture stall | 1 次 / 1400 ms |
| visual stall | 1 次 / 1300 ms |

“diagnostic nontruth candidate=4”不是生产 Receiver 的四次误接收。该 adversarial frame 内有四个 CRC 有效的 Transport codeword，但其 Session/Frame/slot identity 与 Bootstrap truth 不一致；production identity gate 将四个全部拒绝，因此 accepted Transport 为 0。Reference truth oracle 有意把所有 CRC-valid nontruth codeword 计入危险候选，以保证此 case 不会被错误写成普通 `falseAcceptedCodewords=0`。

## 6. 本轮可重复证据

最终两次独立 create-only rebuild：

```text
build-p1_5-evidence/20260831-f6e4769-step06-final-a
build-p1_5-evidence/20260831-f6e4769-step06-final-b
```

两目录相对路径集合、size 和 SHA-256 共 30 个文件逐项一致；FFmpeg/FFprobe `-version`/build configuration 也进入 artifact seal。目录名中的 `f6e4769` 对应上文 implementation commit；`-a` 目录的关键 seal：

| artifact | bytes | SHA-256 |
| --- | ---: | --- |
| `channel-matrix.json` | 36186 | `0BF260614D78EC4A43A188E20FD77677AD584A3403C4D651C29E987E5032E13C` |
| `temporal-corpus.json` | 6007 | `ABC7F35333A8B08C02061B4BDC7219C2D3B41F52969A96972D9ED0D7B39FFF7B` |
| `actual-codec-corpus/codec-corpus-manifest.json` | 14378 | `02357FCD8FC4E58BA7688ABF08DB094C49F4C71034C78BAE98767683E00A274E` |
| `step06-corpus-index.json` | 2079 | `A73A5DD2BE75E8158E55DDC20ABFF9B0DBCB7A3E782FB118D0FFDF4044A0BF47` |
| `SHA256SUMS.txt` | 3311 | `307D207A85944D343568FA07FAFC6550527C871DB5AB417246FDFE3F95BF99DC` |

汇总为：15 个 transform cases（12 Verified / 3 erasure）、5 个 actual codec cases（1/15 frame Verified / 14 erasure / 0 rejected）、11 个 temporal events、0 普通 codec false candidate、0 wrong-identity production admission、所有 expectation matched。

## 7. 验证记录

下列验证均针对与 implementation commit 完全一致的实现文件；大体积 build/test 输出保留在 `build-p1_5-evidence/`，未提交到 Git：

- fresh headless Release build：`20260831-step06-headless-release-final`，完整 CTest **142/142 PASS**；
- fresh headless MSVC ASan/fuzz build：`20260831-step06-headless-asan-final`，完整 CTest **277/277 PASS**；
- fresh apps-off/tools-only configure/build：`20260831-step06-tools-clean-final`，相关 CTest **10/10 PASS**，并证明 application-only probe 不再污染 tools-only build boundary；
- `D:\Python3.12.9\python.exe -B` evidence tests：**16/16 PASS**；
- 独立 Golden checks：LocalDesktop **32 files PASS**，DesktopLevels **100 files / 32 frames PASS**；
- `PBRemoteVisualReport` Python regression：**20/20 PASS**；
- Step 06 corpus 从两个空目录独立重建并完成 30/30 文件 identity comparison；
- diff 的 bounds/lifetime/queue/file-publication/identity/false-accept 审查未发现未解决的 Critical/High。

上述 build 明确关闭 presentation/screen-region/WGC/DXGI/LocalDesktop/DesktopLevels 原生交互 Gate；本步骤没有运行桌面 capture 或真实双机接收实验。

## 8. 不宣称的内容

- 不是 LF4 production D3D11 Encoder/Demod 完成；
- 不是真实向日葵 LF4 Replay；
- 不是完整文件 WholeFileDigest/publish；
- 不是 RemoteVisualSmokePass；
- `CertifiedRemoteVisualProfile=false`。

这些边界分别保留给路线 Step 09..20 和后续 field Gate；不得用本 corpus 的离线成功或失败替代。
