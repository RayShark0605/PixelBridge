# PixelBridge RemoteVisual Step 09 后置动态 LF4 RDP Field Pilot

状态：**PASS。2026-09-01 已完成一轮 120 秒 `PB-RemoteVisual-LF4-X1` 动态 Windows Remote Desktop 真实捕获，并对封口后的 receiver-only Replay 独立离线解码两次；两次检查结果逐字节一致。该结果补足 Step 09 sender 之后的第一条动态像素链路证据，但不启动或关闭 Step 10，不代表 LF4 production Decoder admission、文件恢复、WholeFileDigest、VerifiedEncodedGoodput 或 Certified Profile。**

## 1. 本轮回答的窄问题

本轮只回答以下端到端问题：

1. Computer B 上 evidence-only presenter 调用 production `EncoderRuntime` 产生的动态 LF4 Control/Data raster，能否经过真实 Windows Remote Desktop 视频链路到达 Computer A；
2. Computer A 能否在不触碰受保护左屏的前提下，从右屏 AweSun 窗口对应的物理 ROI 通过 WGC 捕获并生成有界、可封口的 Replay v2；
3. 同一份封口 Replay 能否由 `PBRemoteVisualReplayInspector` 两次确定性地得到相同 Bootstrap、LF4 modulation、FEC、CRC、identity 与 accepted Transport/Control 结果；
4. 动态发送是否可由多个 `FrameSequence`、Control copy 与独立 Transport block 证明，而不是把静止 raster 的重复 Present 误报成新数据。

本轮特意不回答：

- product `PixelBridgeDecoder` 的 LF4 live admission；
- Outer recovery、文件 `.part`、WholeFileDigest 或 final publish；
- sender expected-byte oracle 与 false-accepted-codeword 统计；
- `UniqueVisualFPS`、VerifiedEncodedGoodput、provider/quality/chroma matrix、长时间 soak 或 Certified Profile；
- Step 10 scaled Walsh D3D11 demod shader、Step 11 GPU truth parity 或 Step 20 双机完整文件恢复。

## 2. 发送端、接收端与环境身份

### 2.1 Computer B sender

| 项目 | 值 |
| --- | --- |
| sender implementation | `PBRemoteVisualLf4DynamicPresenter broadcast`；内部调用 production `EncoderRuntime`，不平行实现第二套 encoder |
| sender commit | `efd61688f23a886b800910720e8f2ee358cf4169` |
| package | `build-p1_5-evidence/PixelBridge-LF4-Dynamic-Pilot-ComputerB-efd61688.zip` |
| package SHA-256 | `2c4240478b9f22a72c483ae6799ddaa88c9099d1659a10ea8482082b5f5487b4` |
| launcher | `Start-Lf4-Dynamic-5Hz.cmd` |
| source/profile | 1 MiB deterministic RAW fixture；production Wirehair V2；`PB-RemoteVisual-LF4-X1` |
| cadence | 5 logical FPS；64 repetitions per Control record；300 秒显式时限 |
| Computer B display | 1920×1080 physical pixels；100% scaling；96 DPI |

`READY` 只在 Data Window 已满足物理契约、存在 active frame 且至少完成一次 production immutable-source replacement 后打印。发送包身份来自 Computer A create-only package 与用户报告的启动，不是经像素通道完成的远端主机 attestation。

### 2.2 Computer A receiver

| 项目 | 值 |
| --- | --- |
| receiver commit | `9b199ca14d1437a282940d3126338c5eba0d927c` |
| `PixelBridgeDecoder.exe` SHA-256 | `be4f820ba59e2b425463d5a6ee5dc35a04b05b18dc5640d327be217ef47c9e22` |
| `PBRemoteVisualReplayInspector.exe` SHA-256 | `64a7ab81438e1b96f7b418399307d05de6641d36c8f42bb6a7b9313e91c027d3` |
| capture backend | WGC；diagnostic capture-only；Replay evidence profile `lf4` |
| protected monitor | `\\.\DISPLAY1`，`[0,0,2560,1440]` |
| experiment monitor | `\\.\DISPLAY2`，`[2560,0,5120,1440]` |
| AweSun window | `[2560,0,5120,1392]` |
| selected physical ROI | `[2623,24,5057,1400]`，2434×1376 BGRA8；四边 8-pixel guard |
| remote geometry | 1920×1080 canvas；estimated scale X/Y=`1.259375/1.2592592592592593` |
| provider metadata | AweSun；ScaledToFit；画质、chroma、网络带宽与时延均为 Unknown/NotProvided |

整个 Gate 使用 headless process 与物理坐标；没有发送鼠标/键盘输入，没有激活、移动或关闭外部窗口，也没有改变显示设置。左侧 `DISPLAY1` 由 monitor-safety preflight 明确保护。

## 3. 初轮资源失败与权威采样修复

### 3.1 不能把原始高频捕获等同于 5 Hz payload

远控端虽然只以 5 logical FPS 替换 LF4 raster，AweSun/WGC 仍可产生远高于 5 FPS 的捕获 delivery。2434×1376 BGRA8 每帧为：

```text
2434 × 1376 × 4 = 13,396,736 bytes = 12.776123046875 MiB
```

未采样的 60 秒诊断轮次写入 1282 帧、17,175,294,158 bytes，随后触及 16 GiB 上限；该 Replay 虽已由离线 Inspector 读出动态 LF4 primitive，但 capture report 为 `evidenceValid=false`，因此只作为资源边界的负向诊断，不能作为正式成功证据。

这证明 `logicalVisualFps=5`、WGC `MinUpdateInterval` 或显示刷新率都不能替代接收端自己的有界记录策略。若直接记录所有远控视频 delivery，2～5 分钟 field capture 无法满足现有 Replay 上限。

### 3.2 10 Hz pre-readback sampler

commit `9b199ca14d1437a282940d3126338c5eba0d927c` 增加了只允许 diagnostic capture-only 使用的显式策略：

```text
--replay-sample-fps 1..60
```

正式轮次使用 10 Hz。其约束为：

- WGC `MinUpdateInterval` 只是减少上游 delivery 的 hint；`DiagnosticCpuReadback` 中的时间采样才是 authority；
- freshness/domain/slot/observation 校验先执行，采样判断随后发生，并且严格早于 `CopyResource`、GPU readback、CPU row copy 与 Replay processing；
- 每个 capture domain 的第一帧允许进入；后续有效 timestamp 与上次提交相差不足 1,000,000 个 100 ns 单位时计为 `sampledOutFrames`；
- 非递增 timestamp 也只被采样掉，不做有符号减法溢出；domain 变化会重置 sampler；
- intentional sampling 与 capture/readback/recorder drop 严格分开，不能用 sampler 掩盖丢帧；
- 0 表示保持旧行为、记录每个 admitted delivery；非 capture-only、0 CLI 值或大于 60 FPS 都 fail closed。

正式资源合同为：

| 参数 | 值 |
| --- | --- |
| capture seconds | 120 |
| authoritative sample ceiling | 10 FPS；interval=`1,000,000 × 100 ns` |
| maximum expected sampled frames | `120 × 10 + 2 = 1202` |
| Replay frame cap | 1250 |
| Replay byte cap | 16,384 MiB |
| explicit reserve | 256 MiB |
| expected frames + reserve | 16,371,312,128 bytes，约 15.247 GiB |
| remaining fail-closed headroom | 771.10 MiB |

## 4. 正式复现步骤

正式 evidence root 必须不存在，tracked worktree 必须 clean；脚本还会检查 commit、sender package、receiver/inspector SHA-256 与双显示器拓扑：

```powershell
build-p1_5-evidence\step09-lf4-dynamic-pilot-tools\Invoke-Lf4DynamicRdpPilot.ps1 `
  -EvidenceRoot build-p1_5-evidence\step09-lf4-dynamic-formal-sampled-120s-20260901-225635 `
  -CaptureSeconds 120
```

Receiver 先启动并确认 `.partial` 正在增长；随后在 Computer B 双击：

```text
Start-Lf4-Dynamic-5Hz.cmd
```

Receiver 在 120 秒处显式停止并把 Replay 原子封口。脚本随后以相同的 1250-frame/16-GiB reader policy 调用 Inspector 两次，要求两个 exit code 都为 0 且输出 JSON SHA-256 完全相同。完整实际参数保存在 `capture-command.json`，因此不需要依赖本文手工重建命令行。

发送包中 launcher 的核心命令等价于：

```powershell
PBRemoteVisualLf4DynamicPresenter.exe broadcast `
  --source lf4-wirehair-pilot-1MiB.bin `
  --origin 0 0 --seconds 300 --logical-fps 5 --control-repetitions 64 `
  --report <new-sender-report.json>
```

对已经封口的正式 Replay 进行新的只读复核时，输出路径必须不存在：

```powershell
build-desktop-levels-release\tools\Release\PBRemoteVisualReplayInspector.exe `
  --input build-p1_5-evidence\step09-lf4-dynamic-formal-sampled-120s-20260901-225635\lf4-dynamic-receiver-only-120s.pbrv2 `
  --output <new-inspection.json> --max-frames 1250 --max-mib 16384
```

## 5. 正式捕获结果

Evidence root：

```text
build-p1_5-evidence/step09-lf4-dynamic-formal-sampled-120s-20260901-225635
```

时间与身份：

| 项目 | 值 |
| --- | --- |
| UTC start/end | `2026-09-01T14:56:35.8686594Z` / `2026-09-01T14:58:36.5397881Z` |
| process elapsed | 120.6703487 s |
| runtime | 120,584 ms |
| run ID | `1f05290a59fcffcf10608fc45c4d15e7` |
| state/backend | `Stopped` / `WGC` |
| monitor-safety | `PASS` |

Capture/Replay 权威记账：

| 计数 | 值 |
| --- | ---: |
| capture arrived / delivered | 1085 / 1085 |
| capture drop / readback drop | 0 / 0 |
| capture stale / epoch reset | 0 / 0 |
| Replay written | 1026 |
| intentionally sampled out | 59 |
| Replay dropped | 0 |
| exact delivery accounting | `1026 + 59 + 0 = 1085`，PASS |
| reported capture FPS | 9.038653782072643 |
| Replay evidence/finalization | `evidenceValid=true` / `finalized=true` |
| Replay bytes | 13,745,594,661，约 12.802 GiB |

正式轮次没有触及 frame/byte limit，没有 `.partial` 遗留，stderr 为空。`sampledOutFrames=59` 是权威策略有意跳过的输入，不是 drop；capture delivery、readback 与 recorder 的真实 drop 都为 0。

## 6. 两次 receiver-only 离线解码

`inspection-run1.json` 与 `inspection-run2.json` 都有 1,458,615 bytes，SHA-256 均为：

```text
7c17433c2c8ee26d907893c3d0d99cf6e9362be9c298f3c5a071cb3bf0a75dee
```

两次结果逐字节一致，reader `complete=true`，1026 个 capture frame 与 Replay report 的 written count 精确一致。离线结果为：

| 指标 | 值 |
| --- | ---: |
| Bootstrap accepted frames | 731 / 1026 = 0.7124756335282652 |
| modulation accepted frames | 731 / 1026 = 0.7124756335282652 |
| authority accepted frames | 731 / 1026 = 0.7124756335282652 |
| Control frames / accepted copies | 357 / 1428 |
| full Data frames / accepted Transport blocks | 374 / 1496 |
| partial Data frames | 0 |
| distinct SessionTags | 1：`8c12c61a263f4ea9` |
| FrameSequence range | 1..395 |
| distinct / duplicate FrameSequence observations | 394 / 337 |
| reordered observations | 0 |
| gap events / skipped sequences | 1 / 1 |
| distinct captured raster hashes | 940 |
| FEC / CRC / identity failures | 0 / 0 / 0 |
| stale regions / erased data metrics | 121 / 101106 |

每个 accepted Control frame 都贡献完整四份 Control copy；每个 accepted Data frame 都贡献完整四个独立 Transport block，所以：

```text
357 × 4 = 1428 accepted Control copies
374 × 4 = 1496 accepted Transport blocks
```

没有 partial Data acceptance、混合 Control/Data authority、identity failure 或 reorder。337 个 duplicate sequence observation 是远控视频/采样对同一 logical frame 的重复观察，只增加接收机会，不增加 sender sequence 或 Outer truth；唯一一次 gap 明确记为 skipped sequence，没有被重排或 latest-wins 掩盖。

121 个 stale region 与 101106 个 erased data metric 保留为 freshness soft-erasure 证据；它们没有被解释为 CRC/FEC success。731 个最终接受帧仍须通过现有 Bootstrap、modulation、FEC、Transport CRC、identity 与 padding 边界。

## 7. Evidence seals

| artifact | bytes | SHA-256 |
| --- | ---: | --- |
| `lf4-dynamic-receiver-only-120s.pbrv2` | 13,745,594,661 | `6dbc62308c18a155ac8ae4c1b2fe06db65c0ebadd8407debe10975efb6fd0748` |
| `decoder-report.json` | 7,255 | `9e1380838a382968d95e26eafc38081e68f5ccc6a07b912ea8beea9dc12f8824` |
| `decoder-journal.jsonl` | 158,305 | `0a747eb792b1c7c032edbfcb9f4350621a42db75693476728db4927eeba8d54a` |
| `capture-command.json` | 2,214 | `8974a55302a60221f2b299cda7eaf09aba82e7545b7900c3efe024ec661877cb` |
| `inspection-run1.json` | 1,458,615 | `7c17433c2c8ee26d907893c3d0d99cf6e9362be9c298f3c5a071cb3bf0a75dee` |
| `inspection-run2.json` | 1,458,615 | `7c17433c2c8ee26d907893c3d0d99cf6e9362be9c298f3c5a071cb3bf0a75dee` |
| `pilot-summary.json` | generated summary | `51bb47f5f40b5bc947fc31876b791e0606322fe47c59794ed013ba15083c1294` |

`pilot-summary.sha256` 对 summary 作 create-only seal；summary 内的 15-entry artifact inventory 记录正式 Replay、两个 inspection、capture command/process、monitor/metadata、report/journal 与 stdout/stderr 的 bytes 和 SHA-256。正式 pilot script SHA-256 为 `09a51f0d80fb8c0ce7e551d4d556a987620cc667398f10147aa9936ecb7dba63`。

## 8. 采样实现验证矩阵

| 验证 | 结果 |
| --- | --- |
| sampler native smoke | 15.244 s；143 delivered=`132 written + 11 sampled + 0 dropped`；capture/readback/epoch 全 0；Replay valid/finalized；两次 Inspector JSON byte-identical |
| Release targeted | `PBApplicationTests`、`PBCaptureNormalizeTests`、`PixelBridgeDecoderHelp` 3/3 PASS |
| Release full headless | 174/174 PASS，517.78 s；排除 `Native|GuiSmoke` |
| MSVC ASan/RelWithDebInfo full headless | 309/309 PASS，1013.97 s；含 fuzz/corpus；排除 `Native|GuiSmoke` |
| decoder invalid CLI matrix | 25/25 PASS；缺值、0、61 等 sampler 参数 fail closed |
| cppcheck 2.21 | 18 个 finding 全部 exact-match existing review ledger；0 new/unreviewed |
| static review/Qt keyword guard | 2/2 PASS |
| post-commit rebuild/preflight | receiver 内嵌 full commit exact；binary/package/topology/resource budget preflight PASS |
| `git diff --check` | PASS |

主要本地验证根：

```text
build-p1_5-evidence/replay-sampler-native-smoke-20260901-222100
build-p1_5-evidence/replay-sampler-static-review-20260901-222500
build-p1_5-evidence/replay-sampler-precommit-gates-20260901-223000
```

Release/ASan full-headless logs SHA-256 分别为：

```text
862fae4192077b701d9d7e83970862483ea69e03b67d28c9209999a071be6551
58962aaf7b46e98ebdcadd1874cba35270329232de1b9547912035375aac8ee5
```

## 9. 结论与停止边界

本轮正式证明了以下动态 primitive：

```text
production EncoderRuntime on Computer B
  -> complete LF4 Control/Data raster
  -> Windows Remote Desktop / AweSun
  -> Computer A right-screen WGC capture
  -> bounded and finalized Replay v2
  -> receiver-only offline LF4 modulation/FEC/Transport inspection
```

它比 Step 09 的 immutable-source readback 更进一步，证明了真实远控视频与真实 capture 后仍能得到大量动态、身份一致、零 FEC/CRC/identity failure 的 accepted Control/Transport；同时，它仍停在 diagnostic receiver-only 边界。因为没有独立 sender expected-byte manifest 进入 Inspector，`falseAcceptedCodewords` 必须保持 unavailable；因为没有 product Decoder/Outer/file path，`finalFileDisposition=NotEvaluated`。

按本次停止指令，Step 10 保持 `PENDING`，不进行 scaled Walsh D3D11 shader 实现。下一次若继续，应从 Step 10 的 compact-metrics GPU demod 开始；不得把本轮 CPU/offline Inspector success 作为旧 strict-1:1 shader、live product Decoder、Step 20 或 Certified Profile 的替代证据。
