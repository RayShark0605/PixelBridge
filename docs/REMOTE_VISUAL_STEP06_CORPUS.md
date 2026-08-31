# PixelBridge RemoteVisual Step 06 Impairment Corpus

状态：**Step 06 离线 impairment/adversarial corpus 已完成 production-truth hardening；implementation commit 为 `1936c020cb2c017b9ce3064267f497d15f94d66d`，tree 为 `a0f0fb2717ee2712ecc76bc52f037076c7ee2219`。这不是双机 field PASS，不是 `RemoteVisualSmokePass`，也不是 Certified RemoteVisual Profile。**

## 1. 结论与证据边界

本步骤把用户截图中出现的非整数缩放、letterbox、局部陈旧块、块预测污染、灰度/色度退化和 temporal blend 转为四条可重复的离线证据链：

1. **空间/颜色/块更新 transform matrix**：22 个 case，覆盖 area/bilinear/bicubic、fractional scale/phase、blur/sharpen、range/gamma、4:2:0 proxy、crop/letterbox、solid/alpha overlay、8/16/64 block replacement、Bootstrap mismatch、freshness low-confidence、stale replacement 和 temporal blend。
2. **实际 codec bitstream corpus**：6 个真实 libx264/libx265 Matroska bitstream，覆盖 H.264/HEVC、4:2:0、4:4:4、limited/full range、intra/inter 和不同 CRF；FFprobe 核验实际 codec、pixel format、BT.709 range、几何、frame/packet 和 keyframe。
3. **sequence/identity corpus**：11 个 event 直接复用 production `VisualIdentityTracker`、`RemoteDuplicateRefinementGate` 和 `ChannelStallTracker`，覆盖 duplicate、gap、reorder、CaptureEpoch、erasure 后 duplicate refinement、capture/visual stall，以及 CRC-valid conflicting SessionTag。
4. **Receiver/Outer/digest truth**：production `Transport` 模式接受的 block 才能进入现有 `ReceiverIngress`、DirectRepeat Outer、segment verify、commit 与 finalization。工具没有复制协议/FEC/Receiver，也不声明 `PBStorage` final publish。

所有入口均为 headless：不启动窗口、capture、WGC/DXGI 或 display API，不读取桌面，不接收 expected payload 作为 demod/FEC 输入，不改变 frozen wire/FEC/CRC/digest。固定 sender fixture 只用于生成普通 Control/Segment 描述和 **post-admission** truth scoring。

真实 Direct/Shape/LF4 receiver-only Replay 仍不存在；数据集索引明确为 `replayV2Count=0`。因此路线 Step 02 保持 `PARTIAL`，本 corpus 不能替代 Step 15/20 的 field Replay Gate。

## 2. Schema 与 truth boundary

本轮版本化输出为：

- `PixelBridge.RemoteVisualChannelMatrix.2`；
- `PixelBridge.RemoteVisualCodecFrameEvaluation.2`；
- `PixelBridge.RemoteVisualCodecCorpus.2`；
- `PixelBridge.RemoteVisualTemporalCorpus.2`；
- `PixelBridge.RemoteVisualStep06Corpus.2`。

每帧并行执行两个互不混淆的评估模式：

- `DiagnosticTruth`：允许 reference channel 使用 deterministic sender fixture 统计 coded-bit denominator、BER 和 CRC-valid nontruth candidate；
- `Transport`：只执行 production FEC/padding/CRC/Bootstrap SessionTag/Transport admission，不具有 sender expected bytes，也不把 `FrameSequence` 推导成 `SegmentOrdinal`。

两种模式必须产生完全一致的 Bootstrap 与 modulation observation；只允许 evaluation disposition 不同。production 已接受的 block 之后才与显式 source Segment fixture 比较。如果字节、slot、OuterBlockId、SegmentOrdinal 或 SessionTag 不符，计为 production false acceptance 并使 corpus 失败。

固定 evidence SessionId 为 tool-only deterministic fixture，不进入 production sender，也不改变任何 VisualProfile/Wire contract。它派生的 SessionTag 为 `5751a0fe3cc27908`。

## 3. 一键重建

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

输出包括：

- `channel-matrix.json`；
- `temporal-corpus.json`；
- `actual-codec-corpus/` 下的 source、6 个 bitstream、Gray8、ffprobe、evaluation 和嵌套 seal；
- `step06-corpus-index.json`；
- 根级 `SHA256SUMS.txt`。

脚本拒绝 existing output directory、duplicate JSON member、非有限数值、非 canonical member order、schema/version mismatch、payload BLAKE3 mismatch、source/evaluation 与实际 BGRA/Gray8 字节不一致、工具运行中变化、进程 timeout、超限 stdout/stderr/file、错误 frame/packet/range/pixel-format 和 partial publication。所有输出 create-only；失败清理未发布的 partial artifact。

## 4. 22-case deterministic matrix

按 canonical 顺序固定为：

1. `identity`
2. `full-range-444-identity`
3. `area-upscale`
4. `bilinear-fractional-phase`
5. `bicubic-fractional-scale`
6. `letterbox-075`
7. `box-blur-1`
8. `gaussian-blur-1`
9. `sharpen-1`
10. `limited-range`
11. `gamma-115`
12. `chroma-420`
13. `illegal-crop-8px`
14. `codec-block-overlay`
15. `alpha-overlay-128`
16. `reference-block-8x8`
17. `reference-block-16x16`
18. `reference-block-64x64`
19. `bootstrap-a-mismatch`
20. `freshness-low-confidence`
21. `stale-region-replacement`
22. `temporal-blend-96`

权威 summary：

| 指标 | 结果 |
| --- | ---: |
| case count | 22 |
| `Verified` | 18 |
| `ErasureNoFalseAccept` | 4 |
| production false accepted codewords | 0 |
| expectation mismatch | 0 |
| 完整 case 的 Receiver unique Outer symbols | 每 case 4 |
| 完整 case 的 synthetic one-segment WholeFileDigest | 18 × PASS |
| erasure case 的 WholeFileDigest | 4 × `NotReady` |
| Receiver/Outer conflict/resource rejection | 0 |

4 个 erasure case 为 `sharpen-1`、`illegal-crop-8px`、`bootstrap-a-mismatch` 和 `temporal-blend-96`。`freshness-low-confidence` 至少擦除一个 freshness tag，但 QC-LDPC 仍恢复 4/4 Transport，证明“局部低置信置零”可以恢复而不是硬判错。

`full-range-444-identity` 是无色度损失的 matrix control；实际 4:4:4 limited/full encode/decode 证据由下一节的 codec corpus 提供，不能把 identity control 单独解释成 codec 生存率。

## 5. 实际 codec corpus

固定 source 是 3 个 production LF4 BGRA frame：1920×1080、2 FPS、FrameSequence 0..2、同一 evidence SessionTag。每个 bitstream 都显式关闭音频/字幕/data、metadata、B-frame 和不确定线程行为，并固定 BT.709 metadata；full-range H.264 同时要求 encoder `range=full`，inspector 必须看到 `pc/jpeg` 与 `yuvj444p`。

| case | inspector | range | keyframes | Verified | Erasure | production Transport | Outer unique | WholeFileDigest |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | --- |
| `h264-420-crf18-intra` | H.264 / yuv420p | limited | 3 | 1/3 | 2/3 | 4 | 4 | `NotReady` |
| `h264-420-crf35-inter` | H.264 / yuv420p | limited | 1 | 0/3 | 3/3 | 0 | 0 | `NotReady` |
| `h264-444-crf28-inter` | H.264 / yuv444p | limited | 1 | 0/3 | 3/3 | 0 | 0 | `NotReady` |
| `h264-444-full-crf28-inter` | H.264 / yuvj444p | full | 1 | 0/3 | 3/3 | 0 | 0 | `NotReady` |
| `hevc-420-crf28-inter` | HEVC / yuv420p | limited | 1 | 0/3 | 3/3 | 0 | 0 | `NotReady` |
| `hevc-420-crf40-inter` | HEVC / yuv420p | limited | 1 | 0/3 | 3/3 | 0 | 0 | `NotReady` |

总计 18 帧中仅 1 帧 `Verified`，17 帧为 erasure，0 帧为 production false acceptance。唯一成功帧使一个 Segment 的 4 个 DirectRepeat symbols 完成并通过 segment digest；其余两个 Segment 未完成，因此 6/6 case 的整文件 disposition 都必须是 `NotReady`。没有 WholeFileDigest PASS、没有 final publish、没有 soft success。

这个结果是明确的 **Signal-layer blocker**：当前 LF4 default 对真实有损 codec 仍过于敏感，即使 4:4:4 full range 也未获得完整帧恢复。它不能外推成特定远控产品必然失败，但足以阻止在没有真实 Replay/标定前宣称 current profile 可用。后续输入应进入 Step 07 的 metric/reliability Gate，而不是在 P1.5 放宽 CRC、FEC 或 digest。

## 6. Temporal / identity adversarial corpus

固定 11-event corpus 的权威 summary：

| 项目 | 结果 |
| --- | ---: |
| Transport blocks passed by visual-identity/refinement gate | 16 |
| suppressed duplicate events | 3 |
| suppressed reordered events | 1 |
| gap events / skipped sequences | 1 / 1 |
| CaptureEpoch reset | 新 epoch 建立新 visual baseline |
| erasure 后 duplicate refinement | 1 次恢复，成功后后续 duplicate 被抑制 |
| wrong-identity production Transport | 0 |
| wrong-identity diagnostic nontruth candidates | 4 |
| capture stall | 1 次 / 1400 ms |
| visual stall | 1 次 / 1300 ms |

wrong-identity fixture 使用不同 SessionTag，而不是把不同 `FrameSequence` 错误解释成 production Transport identity。四个 codeword 均能通过 FEC/CRC，因此 DiagnosticTruth 将它们计为 4 个危险 candidate；production `Transport` 模式按 Bootstrap SessionTag 全部拒绝，accepted Transport=0。`FrameSequence` 只用于 visual cadence/epoch identity，`SegmentOrdinal` 只来自 Transport header，两者没有新增 wire mapping。

## 7. 可重复证据与 seal

两次独立 create-only rebuild：

```text
build-p1_5-evidence/20260831-step06-production-truth-v2-a
build-p1_5-evidence/20260831-step06-production-truth-v2-b
```

两目录各 34 个文件；相对路径集合、size 和全部文件 SHA-256 逐项一致。复现清单：

```text
build-p1_5-evidence/20260831-step06-production-truth-v2-reproducibility.json
```

该清单 SHA-256=`438b21430403a2db9853aef2d860109d6a41cb5c39f317a93cbc3c7b82559052`，并记录：

- `allRelativeFilesByteIdentical=true`；
- `rootSha256SumsA/B=2e3d269fe5b921aacea45b53441fe5b0d46e9563dc810547d6aaf142798e70a0`；
- `step06IndexA/B=100b061fb275256c00f37642791cfafcd677d00c88b66f8ccfeecb69fa84d4aa`。

A 目录关键 seal：

| artifact | bytes | SHA-256 |
| --- | ---: | --- |
| `channel-matrix.json` | 74551 | `9a1fefb83d9fd93d8abf35d2a4d7a27e9629135fd96d13f9bddd163d27d986d6` |
| `temporal-corpus.json` | 6369 | `e242cf908ebb203252fd6e26c14fcd362475ddf4c7914b70fef31db58600c5f5` |
| `actual-codec-corpus/codec-corpus-manifest.json` | 21071 | `f7bc481f96e5ccd05063d6b72e13bddce7c5c200269deeaf0eeb4ca81be3d6ea` |
| `step06-corpus-index.json` | 2179 | `100b061fb275256c00f37642791cfafcd677d00c88b66f8ccfeecb69fa84d4aa` |
| `SHA256SUMS.txt` | 3813 | `2e3d269fe5b921aacea45b53441fe5b0d46e9563dc810547d6aaf142798e70a0` |

三个 PixelBridge tool 的当前 Release EXE SHA-256 与 index 中 identity 完全一致：

- `PBRemoteVisualChannelMatrix.exe`: `67511d93aff3d1d2fe3d1c205b918e559ca6cefb8cf0776bd203972a028742f1`；
- `PBRemoteVisualCodecProbe.exe`: `1403829971ed650d15fb3b936fc963ac0978720bb164db5c914527fa366b2df0`；
- `PBRemoteVisualTemporalCorpus.exe`: `99ec012444988fb810fff80e15e6cf839b542280d34717f4410353842726d052`。

## 8. 验证与独立审查

针对与 implementation commit 内容一致的 headless build：

- Release build 后完整 CTest：**143/143 PASS**；`LastTest.log` 246213 bytes，SHA-256=`6148a0d824795e811e5c388312c78fabc6acdf9c697c5e0011989bf8b0e84e34`；
- MSVC ASan/RelWithDebInfo build 后完整 CTest：**278/278 PASS**；`LastTest.log` 379162 bytes，SHA-256=`de7b4d4530002fa7caf7ed8945380a7cb3e1ae14ba9ab7611e60ebcc383754ea`；
- `D:\Python3.12.9\python.exe` RemoteVisual evidence tests：**16/16 PASS**；
- `PBRemoteVisualReport` Python regression：**20/20 PASS**；
- 独立 Golden checks：LocalDesktop **32 files PASS**；DesktopLevels **100 files / 32 frames PASS**；
- `git diff --check` 无 whitespace error；frozen protocol/modulation/FEC/Golden/CRC/digest 文件无变更；
- bounds/overflow、Receiver/Outer conflict、resource rejection、digest state、diagnostic/production identity、canonical JSON、create-only publication 和 shutdown-independent headless scope 审查：**0 open Critical / 0 open High**。

机器可读摘要：

```text
build-p1_5-evidence/20260831-step06-production-truth-v2-validation-1936c02.json
```

其大小 1905 bytes，SHA-256=`376f0a93563ede824912b213e02c52ebe90d5e33b4918ad52a23dcd08c4bce80`。

本轮没有启动 Encoder/Decoder GUI、capture、selector 或 native display Gate，没有操作鼠标键盘，没有读取或保存左侧屏幕像素。`phase1-gate-pass` annotated tag object 和 peeled commit 均未改变。

## 9. 不宣称的内容与后续 blocker

本步骤不证明：

- LF4 production D3D11 Encoder/Demod 已完成；
- 真实向日葵 LF4 Replay 或端到端恢复；
- PBStorage final publish 或 external file SHA-256；
- QoS 恢复后收敛、LocalDesktop 最终 performance regression、6 小时 soak；
- `RemoteVisualSmokePass=true`；
- `CertifiedRemoteVisualProfile=true`。

当前结论是：Step 06 离线 corpus 和 production truth boundary 已完整可重建；Step 02 因 `replayV2Count=0` 保持 `PARTIAL`；actual codec 结果将当前方案分类为 **C. Signal-layer blocker**，同时真实链路仍可能附加 **D. Temporal/channel blocker**，大文件/late-join 仍是 **E. Scheduler blocker**。这些边界必须由后续真实 Replay、Step 07 metric Gate、Step 15/20 field pilot 与最终 LocalDesktop regression 关闭。
