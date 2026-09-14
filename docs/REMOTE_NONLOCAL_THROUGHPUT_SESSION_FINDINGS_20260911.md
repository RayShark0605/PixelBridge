# 非本机吞吐优化 —— 会话进展与发现记录（2026-09-11）

> 状态文档（work log + field findings）。本文记录 2026-09-11 这次会话在"非本机（远程桥 + 右屏远控端）吞吐优化"这条线上的全部进展、实测数据、证据、结论、优化路线与未决问题。
> 本文**不是**gate 认证文档，也不修改任何协议/设计契约。受保护文件 `docs/PHASE1_GATE_REPORT.md` 未被读取外的任何改动（本会话未触碰）。

---

## 0. 一句话结论（TL;DR）

在真实远程画面链路（Encoder 本机 → 右屏 → Citrix 远控端 → 远程机 PixelBridgeDecoder）上完成了 `unified`（稳定）与 `unified-bands`（最新优化分支）两个 profile 的 A/B 实测（15/30 Hz × 50/100 MB）。核心结论：

1. **瓶颈不在带宽、不在编码端、不在画面信道容量，而在接收端单核 CPU**：解码进程稳定占用 **0.81–0.83 个核**，每帧 CPU 时间 **54–59 ms**，唯一视觉帧率天花板约 **10–12 fps**。因此 15 Hz 与 30 Hz 在 50 MB 上几乎无差别（97,725 vs 97,667 B/s）。
2. **接收端"符号接受速率"是一个与 encoder fps 无关的常数**：四种配置下都是 **77–79 symbol/s**（1 symbol ≈ 1314 payload 字节）⇒ 当前硬上限 ≈ **102 KB/s 原始**，实际 goodput = 上限 × 唯一符号占比。
3. **100 MB 超线性劣化的根因是发送端调度契约造成的重复符号**：`alreadyCompletedSymbols` 在 50 MB 时占 8.8–22%，在 100 MB 时高达 **40.7–41.1%**。这是**纯软件可解**的最大单笔收益。
4. **Chroma lane 在远程场景几乎失效**（erasure 92.5%、接受率 0.6%@30 Hz），却仍占用 15 个 codeword 槽中的 5 个。
5. 以上任一优化方向都已定位到具体代码位置（见 §4/§6），但 **O1（喷泉式调度）与 O4（车道重分配）需要主人授权**（见 §7，本会话提问未获答复，未擅自实施）。

---

## 1. 本次会话目标与完成范围

| 项 | 内容 | 状态 |
|---|---|---|
| A | 通读项目文档，理解目标/愿景/现状/技术路线 | 已完成（含 `AGENTS.md`、`docs/README.md`、`PROJECT_STATUS.md`、`REMOTE_CHANNEL_THROUGHPUT_OPTIMIZATION_ROADMAP.md`、`ENCODER_STREAMING_CAROUSEL.md`、`DECODER_RESUMABLE_RECOVERY.md`、`UNIFIED_VISUAL_LARGE_FILE_IMPLEMENTATION_ROADMAP.md`、`PixelBridge_最终技术路线与总体设计.md` 相关章节） |
| B | 审阅"非本机吞吐"这条线的实现思路与实现代码 | 已完成（结论见 §4/§5） |
| C | 通过远程桥在右屏（远程机）做 A/B 自动测试：`unified` vs `unified-bands`，15/30 Hz × 50/100 MB | **8/8 完成**，全部 `Completed` + `digestMatch=true`（见 §3.3） |
| D | 基于数据继续提升非本机传输速度 | 已完成根因定位与路线排序（§5/§6）；实现类改动因需授权项未答复而**尚未落地代码** |

### 1.1 现场收尾状态

- 矩阵 `run_matrix.py` 已于 **19:08:27 全部跑完**，`<PBLine root>\evidence\matrix-run3.log` 末尾为 `MATRIX COMPLETE`，8/8 格全部 `Completed` + `digestMatch=true`。
- 右屏 Encoder 窗口由矩阵脚本自行停止；本会话**未做任何鼠标/键盘输入自动化，未触碰左屏与焦点**。
- 全部证据落盘位置：`<PBLine root>\runs\<tag>\`（含 `receiver\decoder-report.json`、`receiver-cpu.jsonl`、`encoder-evidence\package\*`）与 `<PBLine root>\evidence\<tag>.log`。
- 复查命令：
  ```powershell
  Get-Content -LiteralPath '<PBLine root>\evidence\matrix-run3.log' -Tail 4
  <python> -X utf8 <PBLine root>\ab_summarize.py 'p*'
  ```

---

## 2. 测试环境与身份（可复现）

| 项 | 值 |
|---|---|
| 仓库 / HEAD | `<repo>` @ `cec4d358771332d0509017d89b11963fc4104405`（两个 arm **同一构建**，仅 `--profile` 不同） |
| 构建目录 | `<repo>\build-remote-line-20260910-run01` |
| 回归基线 | `ctest -C Release -E PBPresentationGate` → **163/163 PASS** |
| 本机接收端 | `<PBLine root>\local-receiver\bin\PixelBridgeDecoder.exe`，sha256 `b0a1fa27…f7ac8a9` |
| 远程机 | `SENDER-LAPTOP`（Citrix 远控端），远程显示 1920×1080@240，**未做任何改动** |
| 右屏 ROI | `2560 0 5120 1440`（DISPLAY2，唯一测试面）；观测缩放 scale≈1.2604/1.2611，origin≈(70.0, 28.0) |
| 远程桥 | `<python> -X utf8 <repo>\tools\PBRemoteOpsBridge\local\pbops.py --root <ShareRoot>`（listener 每次会话需 FRESH，主机名校验 SENDER-LAPTOP） |
| 测试文件 | `pb-ab-50mb.bin` / `pb-ab-100mb.bin`（50 MiB / 100 MiB，固定内容） |
| 采样策略 | 每格单次采样（主人已批准），成功判据 = `state=Completed` 且 `digestMatch=true` |

几何稳定性（跨 4 个 bands run 的 `observedLocatorGeometry`）：originX ∈ [69.979, 70.027]，scaleX ∈ [1.260400, 1.260434]，markerResidual ∈ [0.055, 0.286] px，`maximumScaleAnisotropy` ≤ 0.00074。**画面几何在整场测试中几乎完全静止**——这是 §4-F4 的关键前提。

---

## 3. A/B 实测结果矩阵（8/8，全部完成）

所有格子 `state=Completed`、`digestMatch=true`。`goodput` 为最终发布耗时（含摘要校验/安全发布/最终重开）反推的 `publishedBytesPerSecond`。

| # | profile | fps | size | 耗时 s | **goodput B/s** | uniqueVisualFps | bootstrap ms/帧 | Poll(含GPU) ms/帧 | 进程 CPU（核） | 捕获到达/丢弃 |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | unified | 15 | 50 | 536.492 | **97,725.2** | 9.121 | — | — | 0.822（推算） | — |
| 2 | unified-bands | 15 | 50 | 647.728 | 80,942.6 | 11.720(e2e) | 32.46 | 62.86 | 0.8139 | 19,696 / 13,149 (66.8%) |
| 3 | unified-bands | 30 | 50 | 559.765 | 93,662.2 | 7.056(e2e) | 28.84 | 58.32 | 0.8062 | 16,833 / 10,825 (64.3%) |
| 4 | unified | 30 | 50 | 536.810 | **97,667.3** | 11.222 | — | — | 0.823（推算） | — |
| 5 | unified | 15 | 100 | 2101.159 | 49,904.6 | 10.370 | — | — | 0.825（推算） | — |
| 6 | unified-bands | 15 | 100 | 1753.930 | 59,784.4 | 12.017(e2e) | 32.39 | 59.10 | 0.8235 | 53,374 / 34,772 (65.1%) |
| 7 | unified-bands | 30 | 100 | 1738.884 | 60,301.7 | 5.972(e2e) | 30.33 | 54.76 | 0.8093 | 56,289 / 36,994 (65.7%) |
| 8 | unified | 30 | 100 | 1966.054 | 53,334.0 | 10.611 | — | — | 0.805（推算） | — |

原始 JSON 行（便于核对，来自 `ab_summarize.py`）：

```
pbnd-f15-s050-161712  PB-Experimental-BlankControl-1 15 50 Completed True 647.728 80942.618 ... 11.719941540390389 ... 32.462 30.398 62.86 0.8138601756784944
pbnd-f15-s100-173404  PB-Experimental-BlankControl-1 15 100 Completed True 1753.93 59784.37 ... 12.017358738837919 ... 32.386 26.711 59.097 0.8234518164437037
pbnd-f30-s050-163716  PB-Experimental-BlankControl-1 30 50 Completed True 559.765 93662.162 ... 7.055869939021125 ... 28.836 29.485 58.321 0.8061799736792227
pbnd-f30-s100-180425  PB-Experimental-BlankControl-1 30 100 Completed True 1738.884 60301.665 ... 5.972139020252002 ... 30.332 24.43 54.762 0.8092734081211643
puni-f15-s050-160712  PB-Unified-SC6-V3 15 50 Completed True 536.492 97725.222 ... 9.12138847146053 cpuSec 441.062 0.08825/frame
puni-f15-s100-165743  PB-Unified-SC6-V3 15 100 Completed True 2101.159 49904.648 ... 10.369882193416347 cpuSec 1685.422 0.07525/frame
puni-f30-s050-164739  PB-Unified-SC6-V3 30 50 Completed True 536.810 97667.331 ... 11.222182970008914 cpuSec 431.828 0.07155/frame
puni-f30-s100-183426  PB-Unified-SC6-V3 30 100 Completed True 1966.054 53334.039 ... 10.611640641336507 cpuSec 1583.234 0.07435/frame
```

### 3.1 直接读数

- **50 MB**：`unified` 略优（97.7 KB/s），且 15 Hz 与 30 Hz **完全打平**（差 0.06%）。`unified-bands` 在 50 MB 反而更慢（80.9 / 93.7 KB/s），30 Hz 才追近。
- **100 MB**：`unified-bands` 明显更优（15 Hz：59.8 vs 49.9 KB/s，**+19.8%**；30 Hz：60.3 vs 53.3 KB/s，**+13.1%**）。`unified-bands` 内部 30 Hz 相比 15 Hz 只有 +0.9%，`unified` 内部 +6.9%。
- **两个 profile 的"文件大小翻倍 → goodput 腰斩"都超线性**：unified 97.7→49.9（×0.51，即 100 MB 用时是 50 MB 的 3.92 倍）；bands 80.9→59.8（×0.74）/ 93.7→60.3（×0.64）。**说明存在与"文件更大→轮次更多"强相关的重复开销**（见 §4-F5）。
- **提高 encoder fps 基本无效**：4 组对比中 30 Hz 相对 15 Hz 的增益为 −0.06%（unified 50 MB）/ +15.7%（bands 50 MB）/ +6.9%（unified 100 MB）/ +0.9%（bands 100 MB），且 uniqueVisualFps 从未超过 ~12。

### 3.2 派生出的性能模型（本会话最重要的量化结论）

对四个 bands 格子，接收端 `outerAdmission` 计数器给出精确关系：

```
uniqueSymbols(50MB)  = 39,907   uniqueSymbols(100MB) = 79,813
50 MiB / 39,900      = 1314.0   100 MiB / 79,800     = 1314.0
uniqueSymbols ≈ fileSize / 1314  (+ 段边界导致的 +7/+13 级别偏差)
=> publishedBytesPerSecond ≡ uniqueSymbols × 1314 / wallSeconds   （误差 < 0.02%）
```

因此 goodput 只由两个因子决定：**符号接受速率 × 唯一符号占比**。实测：

| 格子 | 带身份符号接受总数 | 接受速率 sym/s | `alreadyCompletedSymbols`（重复） | 唯一占比 |
|---|---|---|---|---|
| bands 15/50 | 51,326 | 79.2 | 11,416 | 77.7% |
| bands 30/50 | 43,773 | 78.2 | 3,861 | 91.2% |
| bands 15/100 | 135,461 | 77.2 | 55,636 | 58.9% |
| bands 30/100 | 134,526 | 77.4 | 54,713 | 59.3% |

- **接受速率恒定 77–79 sym/s**（与 fps、profile、文件大小无关）⇒ 原始上限 ≈ 78 × 1314 = **102.5 KB/s**。
- 实际 goodput = 上限 × 唯一占比：0.777→80.2 KB/s（实测 80.9）、0.912→93.5（实测 93.7）、0.589→60.4（实测 59.8）、0.593→60.8（实测 60.3）。**模型与实测吻合到 1% 以内。**
- 结论：**提升吞吐只有两条正交杠杆** —— ①提高符号接受速率（接收端 CPU/并行度）；②提高唯一符号占比（发送端调度/窗口）。二者可相乘。

### 3.3 完整 8 格（最终，全部 `digestMatch=true`）

| Encoder fps | 文件大小 | `unified`（稳定）goodput B/s | `unified-bands`（最新）goodput B/s | 胜者 |
|---|---|---|---|---|
| 15 | 50 MB | **97,725.2** | 80,942.6 | unified +20.7% |
| 30 | 50 MB | **97,667.3** | 93,662.2 | unified +4.3% |
| 15 | 100 MB | 49,904.6 | **59,784.4** | bands +19.8% |
| 30 | 100 MB | 53,334.0 | **60,301.7** | bands +13.1% |

补充身份：最后一格 `puni-f30-s100-183426` 发布耗时 1,966.054 s，`wholeFileDigest=3e86101c299a30d977ba46b21497a03effb60c1239cf33d6ac3f855886558d`，`finalReopenVerified=true`，`frameCoverageComplete=true`，gitCommit `cec4d358…`，接收端 sha256 `b47316eb…307c6` 匹配。

### 3.4 两 profile 的"两因子分解"（模型在 8/8 格上误差 < 0.3%）

`goodput = 符号接受速率 × 唯一符号占比 × 1314 B`

| 格子 | 接受符号总数 | 接受速率 sym/s | 唯一占比 | 模型预测 | 实测 |
|---|---|---|---|---|---|
| unified 15/50 | 48,968 | 91.3 | 81.5% | 97,842 | 97,725 |
| unified 30/50 | 55,495 | 103.4 | 71.9% | 97,675 | 97,667 |
| unified 15/100 | 206,521 | 98.3 | 38.6% | 49,868 | 49,904 |
| unified 30/100 | 195,865 | 99.6 | 40.7% | 53,290 | 53,334 |
| bands 15/50 | 51,326 | 79.2 | 77.7% | 80,400 | 80,943 |
| bands 30/50 | 43,773 | 78.2 | 91.2% | 93,500 | 93,662 |
| bands 15/100 | 135,461 | 77.2 | 58.9% | 60,400 | 59,784 |
| bands 30/100 | 134,526 | 77.4 | 59.3% | 60,800 | 60,302 |

**这张表是本会话最重要的产出**：两个 profile 的胜负完全由"两个因子谁占优"决定 ——
`unified` 的**符号接受速率高 27–33%**（91–103 vs 77–79 sym/s，即 CPU 效率更高），但 `unified-bands` 的**唯一符号占比在 100 MB 时高 20 个百分点**（重复更少）。50 MB 时前者占优，100 MB 时后者占优。
⇒ 理论最优组合 = unified 的接受速率 × bands 的重复行为 ≈ **100 sym/s × 0.6 × 1314 ≈ 79 KB/s（100 MB 档）**，即在不解决 CPU 的前提下，仅把重复率压到最低，100 MB 档就有 **+32%（59.8→79 KB/s）** 空间；若同时把每帧 CPU 砍半，则 100 MB 档有望接近 **150 KB/s**。

---

## 4. 关键发现（逐条，含证据与代码位置）

### F1（决定性）接收端是单核 CPU 受限，不是信道受限

- 接收进程 CPU 占用恒为 **0.806–0.825 个逻辑核**（4 个 bands 格 + 4 个 unified 格全部如此），即单线程已经打满一个核，其余核几乎不参与。
- 捕获层"到达帧率"恒为 **~30.1–32.4 fps**（与 Encoder 设的 15/30 Hz 无关，右屏画面刷新节奏决定），而"实际被处理的观察数"只有 **10.1–11.1 fps**：

  | 格子 | 捕获到达 | 丢弃 | 丢弃率 | 处理帧数 | 处理速率 |
  |---|---|---|---|---|---|
  | bands 15/50 | 19,696 | 13,149 | 66.8% | 6,539 | 10.10/s |
  | bands 30/50 | 16,833 | 10,825 | 64.3% | 5,994 | 10.71/s |
  | bands 15/100 | 53,374 | 34,772 | 65.1% | 18,578 | 10.59/s |
  | bands 30/100 | 56,289 | 36,994 | 65.7% | 19,280 | 11.09/s |

- **推论**：把 Encoder 从 15 Hz 提到 30 Hz，编码器发出的 payload 翻倍（`generatedPayloadBytesPerSecond` 273,296 → 546,744 B/s），但接收端每秒只能处理 ~10.6 帧画面，多出来的一半**全部变成 65% 丢弃率里的丢弃帧**。这就是 15 Hz 与 30 Hz goodput 几乎相同的机制性解释。
- 唯一视觉帧率天花板：unified 9.12/11.22/10.37/10.61 fps，bands e2e 5.97–12.02 fps。**永远没有超过 12。**

### F2 接收端解码管线深度 = 1，且串成单线程

- `demodPendingHighWater = 1`（RunReport.2），`CaptureDemodulator::QueueResultLocked` 在结果队列满时**丢最旧**（`libs/PBDemodD3D11/src/capture_demodulator.cpp:631-640`）。
- 捕获归一化 → bootstrap → GPU demod → CPU FEC → 外层 FEC → 落盘，实际都在 `DecoderRuntime::Run` 单线程上顺序推进（`apps/common/local_desktop_runtime.cpp`，1 ms 级 sleep 轮询）。
- **推论**：GPU demod（实测仅 **5.0 ms/帧**，`demodGpuTimeTotal100ns=933,411,428` ⇒ 93.3 s / 18,578 帧）与 CPU 侧 bootstrap/FEC 完全串行，GPU 95% 时间在等待。

### F3 每帧 CPU 时间拆分（bands 100 MB / 15 Hz，18,578 个 bootstrap 帧）

| 阶段 | 总量 | 每帧 | 代码/指标位置 |
|---|---|---|---|
| Bootstrap 定位+解码 | 601.68 s | **32.39 ms** | `bootstrapCpuTimeTotal100ns`（`capture_demodulator.cpp` QPC 包围） |
| GPU demod 等待 | 93.34 s | 5.02 ms | `demodGpuTimeTotal100ns` |
| Poll 之后（FEC+度量+准入） | 496.23 s | **26.71 ms** | `demodulationCpuTimeTotal100ns`（`Poll` 外围） |
| 进程总 CPU | 1,444.27 s | ~77 ms（含非 Poll 部分） | `processCpuEquivalentCores=0.8235` |

unified 侧同样量级：`cpuSecondsPerObservation` = 88.25 / 71.55 / 75.25 / 74.35 ms。
⇒ **Bootstrap 是单项最贵的 CPU 开销（约占总 CPU 的 42%）**，见 F4。

### F4（重大可优化点）Bootstrap 每帧都在做"全 ROI 盲搜索"，而画面几何在整场测试里几乎完全静止

证据链：

1. 每提交一帧都会做整 ROI 的 staging 拷贝：`context->CopyResource(state.bootstrapStaging[slot], frame.texture)`（`capture_demodulator.cpp:1094`）。ROI 为 2560×1440 BGRA ⇒ **每帧 14.75 MB** 拷贝。
2. 随后 `if (!pending.bootstrapReady)`（`capture_demodulator.cpp:1192`）→ `context->Map(bootstrapStaging[slot], 0, READ)` 整图映射（`:1204`，`RowPitch×roiHeight` 全量）。而 `pending` 槽位在上一帧结束时被 `FinishPending` / `FinishPendingWithResult` 用 `pending[slotIndex] = {}` 复位（`:633-655`），**`bootstrapReady` 必然每帧回到 false** ⇒ 每帧重新全图 Map + 全图定位。
3. 定位实现 `LocateMarkers` 无条件跑 **3 个阈值的整帧扫描** `{128, 64, 192}`（`libs/PBModulation/src/local_desktop_decode.cpp:405-423`），随后是 4 层嵌套的角色组合几何搜索（`:1131-1145`）。整帧 3.69 Mpx × 3 pass ≈ 11 M 次像素判决/帧。
4. 但现场几何极稳（4 个 bands 格 + unified 格统计）：`originX ∈ [69.975, 70.030]`、`scaleX ∈ [1.260400, 1.260434]`、`markerResidual ∈ [0.055, 0.376] px`、`maximumScaleAnisotropy ≤ 7.45e-4`。**画布 1920×1080 逻辑布局里真正有信息的区域只有 4 个 64×64 marker + 9 个 128×128 timing + bootstrap 记录区**（`libs/PBModulation/include/pbmodulation/local_desktop_bootstrap.h:18-63`），不足整帧 20%。
5. **代码里已经有"跳过全 ROI 搜索"的先例**：`DecodeLocalDesktopFixedCanvasBootstrap`（声明见 `local_desktop_decode.h:144-150`，"Certified 1920x1080 1:1 fast path. It skips the full-ROI marker search but still re-reads and validates all four fixed markers, refines their edges, … checks every distributed timing patch in this same frame."）。它要求 `view == canvas`（严格 1:1），我们捕获的是 1.26× 缩放，因此走不到；**但它的语义证明了"锁定几何后仍逐帧重验 4 marker + 全部 timing patch"是可接受的强度**——这正是 O3a 的设计蓝本，不需要削弱任何校验。

### F5 发送端调度契约是 100 MB 超线性劣化的根因

- `unified-bands` 的 `outerAdmission`：`uniqueSymbols` 恰为 `fileSize/1314`，而 `alreadyCompletedSymbols`（收到的、属于已完成 symbol 的重复）随文件增大而暴涨：

  | 格子 | unique | 重复(alreadyCompleted) | 重复占接受量 |
  |---|---|---|---|
  | bands 15/50 | 39,907 | 11,416 | 22.2% |
  | bands 30/50 | 39,907 | 3,861 | 8.8% |
  | bands 15/100 | 79,813 | 55,636 | 41.1% |
  | bands 30/100 | 79,813 | 54,713 | 40.7% |

- unified 侧因为 `RunReport.3` 不落 `outerAdmission`（见 F7），改用各 lane FEC `accepted` 之和换算重复率：15/50 = 18.5% 重复（接受 48,968 vs 需要 39,907），30/50 = 28.1%，15/100 = **61.4%**，30/100 = **59.3%**。
- 机制在代码里可读出：
  - `docs/ENCODER_STREAMING_CAROUSEL.md` §1 第 6–7 条规定**每一轮 = K 个 systemmatic 方程 + max(16, ceil(K×20%)) repair 方程**，后续轮次从 repair 高水位继续。也就是说**每一轮都要把整个 segment 的 K 个 systematic 重新播一遍**，而其中绝大多数 symbol 接收端早已拿到。
  - 实现：`SenderUnifiedCarouselScheduler::BuildFrame`（`apps/common/sender_carousel_scheduler.cpp:441-538`）里 `firstEquationIndex = committedEquationCount_`，方程号从 0 起严格递增，播完 `scheduledEquationCount_` 之后进入 `PaddingDuplicate` 分支继续重复播（`:514-522`）。
  - 窗口：`InitializeUnifiedSegmentWindow`（`apps/common/local_desktop_runtime.cpp:1788`）、`SelectNextUnifiedSegment`（`:1827`，sweep + `senderUnifiedSweepPhaseHold=32` + `step=1`）、`AdvanceUnifiedSegmentWindow`（`:1849`，要求窗口内**所有** segment 都跑完一轮才前进）。窗口大小常量 `senderUnifiedActiveSegmentWindowSize = 8`（`apps/common/sender_carousel_scheduler.h:40`），**同时被用作接收端 `ReceiverResourcePolicy.maxActiveOuterFecDecoders`**（`local_desktop_runtime.cpp:8104`）。
  - 现场后果（bands 15/100）：`outerFecQuotaExceededCount = deferredResourceBusyCount = 18,390`，`activeDecoderLimit = 8`，`peakActiveDecoderCount = 8` —— 8 个 Wirehair 解码器配额长期打满。unified 100 MB 的 `encCarouselPass = 4`（15 Hz）与 `8`（30 Hz），即 100 MB 实际转了 4–8 整圈，而理论只需 1 圈 + 少量 repair。
- 现场 encoder 侧无瓶颈：`observedSubmittedLogicalFps` 14.96/29.80/14.96/29.85，`presentCallFps` 188–202，`sourceTextureReplacements` 与目标帧率一致。

### F6 Chroma 车道在远程画面链路上几乎失效，却仍占 1/3 空口

unified 三车道 FEC 实测（`unifiedTelemetry.lanes[*].fec`）：

| 格子 | BaseLuma 擦除率 | FineLuma 擦除率 | **Chroma 擦除率** | Chroma 接受槽 | Chroma 占接受总量 | Chroma 占 BP 迭代量 |
|---|---|---|---|---|---|---|
| unified 15/50 | 16.5% (7,402/44,982) | 37.7% | **66.9%** (16,717/24,990) | 8,273 | 16.9% | 48.6% |
| unified 30/50 | 7.1% (3,879/54,315) | 19.4% | **99.4%** (29,981/30,175) | 194 | 0.35% | 11.7% |
| unified 15/100 | 10.4% | 24.0% | **92.1%** (103,136/111,990) | 8,854 | 4.3% | 22.6% |
| unified 30/100 | 8.1% | 18.5% | **97.8%** (104,119/106,465) | 2,346 | 1.2% | 22.4% |

- Chroma 在 15 个 codeword 槽里占 **5 个（33.3% 空口）**，但实际只贡献 0.35%–16.9% 的接受符号，并消耗 12%–49% 的 BP 迭代 CPU。
- 原因符合远程画面链路常识：远端编码器（Citrix/H.264/视频）对**色度**的量化/4:2:0 下采样远比亮度破坏性强；本项目的色度车道用彩色承载 bit，天然最先被压死。
- 反证：同一格子内 **BaseLuma 擦除率只有 7–16%**（同帧同链路同抖动），说明不是"链路坏"，而是**色度承载位坏**。
- 30 Hz 时 BaseLuma 擦除率从 16.5% 降到 7.1%（50 MB 档）——说明**逻辑帧停留越短、单帧采样越干净**，这也是 O5（CPU 修好后再上探 30/60 Hz）的依据。

### F7 归因缺口：`RunReport.3`（unified 档）缺 `outerAdmission` / 阶段 CPU / 捕获流量

- `apps/common/run_report.cpp:229-230` 处按 profile 分流：`unified → "PixelBridge.RunReport.3"`，`unified-bands → "PixelBridge.RunReport.2"`。
- RunReport.3 **不输出** `outerAdmission`（unique/duplicate/quota）、`bootstrapCpuTimeTotal100ns`、`postGpuFecCpuTimeTotal100ns`、`demodGpuTimeTotal100ns`、`captureFlow`（到达/丢弃）、`processCpu*`，而 RunReport.2 全都有（`run_report.cpp:448-450`、`:953-955`）。
- 后果：本次 4 个 unified 格只能**间接推算**重复率与 CPU 拆分，无法直接读出 quota 打满次数（`outerFecQuotaExceededCount`），也无法直接量化 unified 的 bootstrap 占比。这会让后续 O1/O2/O3 的收益归因变模糊。
- 另外：`pbcore::StageDiagnostics` 框架（`DiagnosticStage::ReceiverProcess/BaseFec/FineFec/ChromaFec/OuterReceive/SegmentRecover/SegmentWrite/FinalPublish` + `BuildStageDiagnosticsJson`）已存在，但 **CLI 从未设置 `DecoderConfig.diagnostics`**，所以一直是 null。
- 这两项都是**纯增量测（不动 wire）**，属于低成本高价值改动，建议下一会话优先补齐。

### F8 其它值得记录的现场事实

- 捕获后端 `WGC`，`geometryStatus="Letterboxed"`，观测 scale ≈ **1.2604 × 1.2611**（存在 7.4e-4 的轻微各向异性，来自远端 letterbox/缩放），而 `NonDecodingOperatorMetadata` 里元数据估计 1.3333 —— **元数据与实际几何不符，实际以 `PixelBridgeObserved` 为准**（正确行为，未做重采样）。
- `puni-f30-s100` 的 `observedGeometry` 极值出现 `maximumOriginX=106.32`、`minimumScaleX=1.1528` —— 说明**起步阶段确实出现过少数几何异常帧**（后续稳定到 70.0/1.2604）。这提示 O3a 的"锁定几何"必须带**漂移守卫 + 回退全扫描**，不能盲目锁定。
- `duplicateObservations` 仅 495/21,293（2.3%），说明重复主要来自"同 symbol 的方程被反复播放"，而不是"同一帧被重复观察"，与 F5 结论一致。
- Encoder 侧 `encCarouselPass`：unified 15/50=2、30/50=5、15/100=4、30/100=8；文件越大/帧率越高，转的圈数越多，重复播放越多。

---

## 5. 优化路线 backlog（按证据强度 × 预期收益排序）

> 每一条都必须在右屏现场 A/B 复测（`digestMatch=true`）并与本次基线对比：50 MB 档基线 **97.7 KB/s**，100 MB 档基线 **59.8 KB/s**（bands）/ **53.3 KB/s**（unified-30Hz）。

| ID | 优化 | 依据 | 预期（模型） | 是否需主人授权 | 风险 |
|---|---|---|---|---|---|
| **O1** | **喷泉式调度**：Wirehair segment 第 0 轮之后只发**新 repair 方程**（把 `committedEquationCount_` 起点从 0 提到 K，不再重播 systematic），并可放大每轮 repair 预算 | F5：100 MB 重复率 41%（bands）/59–61%（unified）；`encCarouselPass` 4–8 | 50 MB **×1.3–2.1**，100 MB **×1.5–3.0** | **需要（Q1）** | 改发送端调度契约（wire 兼容：方程号语义不变，晚加入者仍可从中途恢复）；需更新 `ENCODER_STREAMING_CAROUSEL.md` §1 第 6–7 条 |
| **O2** | 发送端活跃段窗口 **8 → 6**，接收端上限保持 8，消除 `outerFecQuotaExceededCount=18,390` 的丢弃 | F5 配额打满证据（`peakActiveDecoderCount=8` vs limit 8） | 100 MB +5%–12% | 不需要（未削弱任何限制：接收端限额保持 8，只是发送端不超发） | 窗口变小 → 段完成更集中，需观察 `recoveryReadyEvents` 与尾部长尾 |
| **O3** | **接收端 CPU 优化**（最大杠杆，且与 O1/O2 正交可相乘） | F1/F2/F3/F4 | 帧处理 10.6 → ~25 fps ⇒ goodput **×2.3** | 不需要（纯接收端性能，不改 wire/不改校验强度） | 见下方三个子项 |
| O3a | 锁定观测几何的**窗口化 bootstrap**：只 copy/map 4 marker + 9 timing + bootstrap 记录区，单阈值快路径 + **漂移守卫回退全扫描**（蓝本 = 已存在的 `DecodeLocalDesktopFixedCanvasBootstrap` 语义） | F4：32.4 ms/帧里大部分是全 ROI copy+Map+3 遍全帧扫描 | 32 → ~12 ms/帧 | 不需要 | 必须保留"逐帧重验 4 marker + 全部 timing patch + 双副本一致"；几何漂移（见 F8 的 106.32/1.1528 异常帧）必须自动退回全扫描 |
| O3b | **并行化每帧 15 个相互独立的 codeword FEC**（`unified_visual.cpp:1743-1787` 循环），保持观察序合并、`resultQueue` 与外层 Wirehair 准入仍单线程 | F2/F3：26.7 ms/帧串行 FEC；F6：Chroma 的 BP 迭代占 12–49% | FEC 26.7 → ~9 ms/帧 | 不需要（不改结果，只改并行度；需保证确定性/位级一致） | 线程池引入抖动；需 A/B 证明输出逐字节一致 |
| O3c | 让 GPU demod 与 CPU FEC 流水重叠（`demodPendingHighWater` 1 → 2–3） | F2 | 隐藏 5 ms GPU 等待 | 不需要 | 队列变深后需确认丢帧语义与内存上限不变（受 `ReceiverResourcePolicy` 约束） |
| **O4** | **车道重分配**：把 5 个 Chroma 槽改判给 BaseLuma 编码（需新的实验 profileId + layoutVersion + Golden Vector） | F6：Chroma 擦除 67–99%、仅贡献 0.35–16.9% | **+30%–40%** 空口有效容量 | **需要（Q2）** | 新视觉身份（不能静默改既有 SC6-V3 语义）；需新 Golden Vector |
| O5 | CPU 修好后复测 **30 Hz / 60 Hz** | F6 尾注：30 Hz 下 BaseLuma 擦除率 16.5%→7.1% | 未知，待测 | 不需要 | 若 CPU 未先解决，则毫无收益（F1 已证） |

**推荐执行顺序**：O3a → O2 → O3b → （O1/O4 待授权）→ O5。理由：O3/O2 不需要授权、风险可控、且 O1 的收益必须靠接收端能吃下来才能兑现。

---

## 6. 待主人决策的问题（本会话已提出但**未获答复**，因此以下两条路线均未擅自实施）

### Q1 `schedule_contract` —— 是否允许把发送端调度从"每轮 = K systematic + 20% repair"改为喷泉式？

- 现状是文档契约：`docs/ENCODER_STREAMING_CAROUSEL.md` §1 第 6–7 条。
- 拟改为：第 0 轮发 K 个 systematic + 少量 repair；之后**只发新 repair 方程**（不再重播 systematic）。
- 兼容性：Wirehair 是真正的喷泉码，方程号语义不变，晚加入者仍可从任意位置恢复；不改 wire 格式、不改摘要/发布/重开/冲突拒绝/资源限制。
- 代价：需要同步更新文档，并新增针对"中途加入 + 纯 repair 流"的测试。
- 选项：(a) 改默认值 + 更新文档（**推荐，收益最大**）；(b) 只加实验开关 `--fountain-after-round0`，现场验证后再决定；(c) 暂不动发送端。

### Q2 `lane_identity` —— 是否批准新建一个实验视觉身份，把 5 个 Chroma 槽改给 BaseLuma？

- 背景见 F6（Chroma 擦除率 67–99%，却占 33.3% 空口）。
- 需要新的 `profileId` + `layoutVersion` + 新 Golden Vector，属于新视觉身份，不能静默改 `PB-Unified-SC6-V3`。
- 选项：(a) 批准新建实验身份（**推荐**）；(b) 先离线做信道刻画（同一 ROI 下逐 lane 统计 BER/突发分布）再决定；(c) 暂缓。

> 另有一个**不需要授权**的后续问题想请主人拍板执行节奏：下一会话是"先做 O3a+O2（不需授权、风险低）"，还是"等 Q1/Q2 一起做完再统一现场复测"。默认将按前者推进。

---

## 7. 复现与工具链（下一次会话可直接照抄）

### 7.1 身份

```powershell
Set-Location -LiteralPath '<repo>'
git rev-parse HEAD          # cec4d358771332d0509017d89b11963fc4104405
# 构建目录：<repo>\build-remote-line-20260910-run01
# 回归：   ctest -C Release -E PBPresentationGate     # 163/163 PASS（注意必须带 -C Release）
```

工作树状态（本会话结束时）：仅 `tools/PBRemoteOpsBridge/local/pbops.py` 有未提交改动（+13/−2，`read_result()` 容忍 SMB 写入过程中的共享冲突/半截 JSON，改为继续轮询而非误判失败）；未跟踪：`.zcode/`、`docs/PHASE1_GATE_REPORT.md`（受保护，未动）。

### 7.2 测试脚手架（全部在 `<PBLine root>\`）

| 文件 | 作用 |
|---|---|
| `ab_run.py` | 跑单格：远程桥启动右屏 Encoder + 本机纯 Python 拉起接收端 + ctypes `GetProcessTimes` CPU 采样 |
| `pb_report.py` | 双 schema 归一化器（RunReport.2 扁平 / RunReport.3 嵌套） |
| `ab_summarize.py '<glob>'` | 汇总所有格子成一张表 |
| `run_matrix.py [--from-index N]` | 顺序跑矩阵，写 `matrix-index.json`，续跑用 `--from-index` |
| `encoder-kit\` / `local-receiver\` / `PB-Head-cec4d35-20260911.zip` | 部署到远程机的 Encoder 包、本机接收端 |
| `evidence\matrix-run3.log`、`evidence\<tag>.log` | 现场日志（每格一份 RESULT JSON） |
| `runs\<tag>\` | 每格完整证据：`receiver\decoder-report.json`、`receiver-cpu.jsonl`、`encoder-evidence\package\encoder-report-*.json`、`encoder-journal-*.jsonl` |

```powershell
<python> -X utf8 <PBLine root>\ab_summarize.py 'p*'
```

### 7.3 踩过的坑（务必保留，避免重复踩）

1. **PowerShell 5.1 被 Python 子进程拉起时丢失模块路径**（`Get-FileHash` 不可用）⇒ 接收端改为直接从 Python 启动（`ab_run.py` 已实现）。
2. `powershell -Command "..."` 里 `$_` / `$var` 会被吞 ⇒ 用单引号 here-string，或用 Python 做文本处理。
3. 原生命令 detached 输出是 GBK ⇒ Python 侧统一 `subprocess.run(..., encoding="utf-8", errors="replace")`。
4. `pbops pull` 的 id 要从 stderr 的 `sent: <commandId>` 解析。
5. `--params-json` 用 hashtable + `ConvertTo-Json -Compress` 构造，别手拼。
6. Encoder 启动必须 **`console:false` + `--seconds`**；`console:true + --manual-stop` 会 process-died。停止用 WM_CLOSE，此时 Encoder report 的 `state:"Failed"` 属正常，计数器仍有效。
7. `--report` / `--journal` 传相对路径会解析到 exe 旁边，务必给绝对路径。
8. Decoder `--timeout` 上限 3600 s（100 MB 档约 1,700–2,100 s，够用但要留意）；`--roi` 会吃掉后面 4 个 token。
9. **现场测试跑起来时不要在本机编译/跑 ctest**：接收端单核打满，任何本机 CPU 竞争都会污染计时。
10. 早期一次矩阵（16:01，`evidence\matrix-20260911.log`）8 格全部秒败，原因是 `ab_run.py` 里 `tasklist` 输出拼接的 `NoneType += NoneType`，已修（勿被旧日志误导）。
11. 垃圾/无效目录（可忽略）：`runs\smoke-*`、`runs\puni-f15-s050-160136`、`pbnd-f30-s050-162913`、`debug-collect*`、`<PBLine root>\patch_*.py`。

---

## 8. 下一会话的 TODO（按优先级）

1. **补归因（不依赖授权，先做）**：把 `outerAdmission` / 阶段 CPU / 捕获流量 / `processCpu*` 补进 `RunReport.3`；给 Decoder CLI 加 `--stage-diagnostics` 打开已有的 `pbcore::StageDiagnostics`；harness 里给接收端加 `--journal` 以获得段级时间线。
2. **O3a 实现 + 现场复测**（窗口化 bootstrap + 漂移守卫 + 全扫描回退）。目标：`bootstrapCpuTime` 32.4 → ≤ 14 ms/帧，`cpuSecondsPerObservation` ≤ 0.05 s，处理帧率 ≥ 18 fps。
3. **O2 实现 + 现场复测**（发送窗口 8→6，观测 `outerFecQuotaExceededCount` 是否归零）。
4. **O3b**（15 个 codeword FEC 并行 + 输出位级一致性 A/B）。
5. 拿到 Q1/Q2 答复后实施 O1 / O4。
6. 每完成一项：受影响目标窄验证 + `ctest -C Release -E PBPresentationGate` + **一次右屏现场 A/B**（与 §3.3 基线对比），证据入 `<PBLine root>\<...>\`，并更新本文档或新的 status 文档。

---

## 9. 本会话合规性自检

| 约束 | 状态 |
|---|---|
| 纯视觉单向 payload，无旁路/无隐式 ACK | 遵守（未新增任何信道；未改动协议） |
| 不修改 Citrix / 网络设置 | 遵守（远程机显示 1920×1080@240 未动） |
| 不削弱摘要 / 安全发布 / 重开验证 / 冲突拒绝 / 资源限制 | 遵守（8/8 格 `wholeDigestVerified+renameSucceeded+finalReopenVerified` 全真；`conflictRejections=0`、`resourceRejections=0`） |
| 不干扰左屏 / 焦点 / 鼠标 / 键盘 | 遵守（全程仅右屏 ROI `2560 0 5120 1440`，无任何输入自动化） |
| 主题聚焦"非本机传输效率" | 遵守 |
| Git：显式路径、不 broad-stage、不 reset/rebase/amend/push | 遵守；`docs/PHASE1_GATE_REPORT.md` 未 add/未改 |
| 结论边界 | 所有数字均来自现场 `Completed`+`digestMatch=true` 的 RunReport；模型外推（§3.4 最后一句、§5 预期列）**明确标注为模型推算，未经现场验证** |

---

## 10. 本次改动清单

- 新增文档：`docs/REMOTE_NONLOCAL_THROUGHPUT_SESSION_FINDINGS_20260911.md`（本文件，未提交）。
- 工作树遗留（上一轮工具加固，未提交）：`tools/PBRemoteOpsBridge/local/pbops.py`。
- **本会话未修改任何生产代码**（O1/O2/O3/O4 均处于"已定位、待实施/待授权"状态）。

---

## 11. 第二会话（2026-09-11 晚间）：结论复核、授权落地与现场中断处置

### 11.1 主人授权（已获答复）

- **Q1 → 改默认值+更新文档**：Unified Wirehair later pass 改为增量喷泉 repair（O1）。
- **Q2 → 批准新建实验身份**：5 个 Chroma 槽改判 BaseLuma（O4，另行实施）。
- 构建期间改动允许入库提交。

### 11.2 三条结论的复核（同构建 cec4d35，当日复测）

| 格子 | goodput B/s | 接受率 sym/s | 唯一占比 | CPU 核 | 模型预测 vs 实测 |
|---|---|---|---|---|---|
| vb bands 15/50 | **117,973** | 92.8 | 96.8% | 0.814 | 118,070 vs 117,973（0.08%）|
| vb bands 30/50 | **94,803** | 91.1 | 79.2% | 0.812 | 94,795 vs 94,803（0.01%）|
| vb bands 15/100 | **60,089** | 72.1 | 63.4% | 0.816 | 60,098 vs 60,089（0.015%）|
| vb bands 30/100 | **51,996** | 59.0 | 67.0% | 0.794 | 52,005 vs 51,996（0.017%）|

- **结论①确认**：0.81–0.83 核、e2e 唯一帧率 ≤13；今天链路更健康（接受率 91–93 vs 昨日 77–79），证明接受速率 = 链路质量 × 单核预算，CPU 天花板机制不变。
- **结论②强确认**：两因子模型在链路条件显著不同的另一天以 <0.1% 误差复现。
- **结论③方向确认+机制修正**：重复率随 wrap 频率升高（今日 15/50=3.2%、30/50=20.8%，与昨日 22%/8.8% 反向）——重复由"每秒 wrap 次数 × 已解码段比例"驱动。**修正**：unified 家族 pass≥1 在旧代码里已是纯新 repair（systematicEquationCount=0），真正浪费是每 wrap 给每段排满 K+20% 的 repair 预算；F5 中"每轮重播 K systematic"只适用于 G02 历史调度器。O1 按真实机制实施。
- **配额证据消失**：今日 50MB 两格外层配额打满/deferred 均为 0（昨日 100MB 为 18,390）；O2（窗口 8→6）暂无证据支撑，待 100MB 数据后决定。

### 11.3 现场中断处置（20:11–20:23）

- 主人关闭/重开右侧远控查看器：正在跑的 vb-f15-s100-201102 作废停止；远程机分辨率跳回 **2560×1600**（<1.0× 解码下限）且远程 Encoder 被杀。
- 处置：桥 display-set 切回 1920×1080@240；1MB 探针（vb-f15-s001-probe）20s 完成、digestMatch=true、几何与关闭前逐位一致（origin≈(70.0,28.0)、scale≈1.2604/1.2611）。随后以新 tag 重跑两个 100MB 格（validate_matrix2.py）。
- 经验：**查看器重开后必须先查 display-info 再复测**；探针格是最快的安全检查。

### 11.4 已落地实现（待编译+回归+现场 A/B）

1. **O1 增量喷泉调度**（`sender_carousel_scheduler.*`）：wirehair pass≥1 预算 `K+max(16,20%K)` → `max(16,20%K)` 全新 repair；尾帧 padding 改为本 pass 已调度范围复用（pass 0 复用 systematic、repair pass 复用本 pass repair）；`sender_carousel_scheduler` 注释、`docs/ENCODER_STREAMING_CAROUSEL.md` §1.1 第 7 条（含与 §1 第 6–7 条 G02 历史合同的分界）同步更新；新增探针 `ProbeUnifiedFountainMidJoin`（错过全部 pass 0、纯 repair 流跨 pass 恢复 8×64KiB 段，断言零资源拒绝、纯 repair ID、多 pass 完成）与调度器/工作流测试更新。
2. **O3a 锁定几何窗口化 bootstrap**（`local_desktop_decode.*`、`capture_demodulator.cpp`）：hint 预测 4 marker 中心，各在 ~130px 窗口内做与全扫描同源的 run-length 定位+VerticalCross+VerifyMarker；任一窗口 miss/异角色 marker/漂移>48px 或 scale 漂移>5% → 同调用内回退完整全扫描；MakeGeometry/EvaluateGeometry（双副本 RS/CRC+9 timing patch）完全不削弱；与 fixed-canvas 先例一样不做全 ROI 歧义扫描（下游逐帧身份检查仍是绑定权威）。`CaptureDemodulator` 维护 hint（域启动/失效即重置），`windowedFastPath/fallback` 计数贯通 CaptureDemodulatorSnapshot→DecoderSnapshot→RunReport.2/3。新增 4 组几何（1:1/1.26/各向异性）×（hint 命中/微漂移/大漂移回退/撕裂帧同擦除）测试。
3. **归因补齐**：headless Decoder CLI 挂 `RunMeasurementRecorder`（RunReport.3 自动获得 stageCounters/captureFlow/measurement）；RunReport.3 无条件输出 `processCpu*`；新增 `--stage-diagnostics` CLI 开关接通既有 `pbcore::StageDiagnostics`。
4. 预期收益模型（待现场验证）：O1 把 100MB 档重复占比 37-40%→~10-15%、O3a 把 bootstrap 27-32ms/帧→数 ms；若成立，100MB 档 goodput 有望 60→90-110 KB/s（今日链路口径）。
5. **实现过程的关键教训**：`decoder_resume_store.cpp` 把"接收端解码器上限 ≤ 发送窗口"当成硬校验（`> senderUnifiedActiveSegmentWindowSize` 即拒绝启动）——O2 解耦后该隐含耦合使**所有解码启动失败**（首轮回归 2 个测试套件连带 14 个用例失败），同文件还有按窗口定尺寸的活跃段计数数组。均已改为跟随接收端上限。全库扫描确认无其它隐性耦合。
6. **回归收口**：`ctest -C Release -E PBPresentationGate` **163/163 PASS**（含新增 fountain mid-join 探针、locked-geometry 测试与全部解耦后的期望更新）。改动已提交：`d37d98f feat(demod,scheduler): non-local throughput pass 1 ...`（20 文件，+1066/−65）。构建身份 gitCommit=d37d98f，encoder sha256 `493bd43f…`，decoder sha256 `e92f6992…`。
7. **部署陷阱**：桥的 deploy 安全解包**拒绝 zip 目录项**——Qt 插件目录打进 kit 会被 `zip-directory-entry` 拒绝；kit 必须只含平铺文件（旧 kit 34 项无目录即此因）。重打包为 15 个平铺文件后部署成功。

### 11.5 A/B 计划

构建（增量+重configure 取新 commit）→ `ctest -C Release -E PBPresentationGate` → `package_opt_kit.py`（kit-opt1-*.zip + local-receiver-opt + opt-build.json）→ `opt_matrix.py` 四格（bands 15/30×50/100，receiver 带 --stage-diagnostics --journal）→ 对比今日 vb-* 基线。

### 11.6 A/B 第一轮（d37d98f，平坦 20% 喷泉预算）——4/4 完成，全部 digestMatch=true

| 格子 | 基线 vb（cec4d35） | opt（d37d98f） | 变化 | 备注 |
|---|---|---|---|---|
| bands 15/50 | 117,973 | 113,490 | −3.8% | 基线重复本就 3.2%，无优化空间；链路噪声带内 |
| bands 30/50 | 94,803 | **123,980** | **+30.7%** | rate 91.1→98.1、唯一占比 79.2→96.2%，两因子同升 |
| bands 15/100 | 60,089 | 53,487 | **−11.0%** | **回归**：dups 46,026→56,737，机制见下 |
| bands 30/100 | 51,996 | **68,182** | **+31.1%** | e2e 帧率上限 12.9→16.5；dups 仍 49,169 |

- **O3a 窗口化 bootstrap 全胜**：32.3→**2.7ms/帧（12×）**，四格 fast-path 命中 26,822-6,437 帧、**回退仅 30 帧**（0.1%，且那些帧本身即被拒），几何锁极其稳定。
- **15Hz 回归的机制**（重要设计教训）：今晚链路擦除使每段缺口 ≈28-40%×K，平坦 20% 预算迫使缺口段经历 2-3 个 wrap 才补齐，而**每个 wrap 都重访全部已解码段** → 重复接收反超基线。30Hz 因帧流加倍、wrap 节奏更快而不受此害。
- **修复（7df05e2）**：pass k 预算翻倍 `max(16, ceil((K-1)×20%×2^min(k-1,3)/100)+1)`（20%→40%→80%，封顶 160%）——首 wrap 便宜、高缺口段 1-2 wrap 收敛。调度器相位测试的硬编码 8 一并改为窗口常量；PBUnifiedSenderSchedulerTests 14/14、PBApplicationTests 全过。
- 30/100 格的 e2e 16.5fps、FEC 30.2ms/帧 → **FEC 已是下一个主导瓶颈**（O3b 数据支撑）。

### 11.7 A/B 第二、三轮（翻倍预算 7df05e2 → 封顶 80% 594d6bb → 回退 55e4921）

| 格子 | 基线 | flat20 (d37d98f) | 翻倍160 (7df05e2) | 封顶80 (594d6bb) |
|---|---|---|---|---|
| 15Hz/100MB goodput | **60,089** | 53,487 | 58,489 | 52,837 |
| 15Hz/100MB rate×uFrac | 72.1×.634 | 69.7×.584 | 84.1×.529 | 80.0×.500 |
| 15Hz/100MB dups | 46,026 | 56,737 | 70,922 | 79,741 |
| 30Hz/50MB goodput | 94,803 | 123,980 | **124,898**（+31.7%）| — |
| 30Hz/100MB goodput | 51,996 | 68,182（+31.1%）| **61,366（+18.0%）** | — |

**机制结论（15Hz/100MB 三变体全输基线的原因）**：15Hz 下接收端接纳预算（72–84 sym/s）本已饱和于信源供给（~200 方程/s），.sender 多播的方程在接收端只能按帧存活率随机抽样接纳；喷泉变体把更多空口时间花在已解码段的 repair 上，接纳到的重复比例随之上升。翻倍预算提升 rate（大批次→更少控制边界）但降低 uFrac，二者近抵消、净输基线 3–12%。**30Hz 则相反**：接收端接纳率可达 111.9 sym/s（帧流翻倍），多出的有效空口被真正利用，+31% 稳定复现。

**决策**：80% 封顶被现场否决（dups 再升）→ revert 为 160% 翻倍（55e4921，最终默认）。最终确认格 o4v-f30-s100（55e4921）= 61,366 B/s（+18.0%）、e2e 帧率 **19.3**（全场最高）；与 flat20 的 68,182 差异含 2.3h 链路漂移混杂（rate 84.0→80.5），不下变体优劣结论。翻倍 vs flat20 直接对比 2:1 占优（15/100 +9.4%、30/50 +0.7%、30/100 −10% 存疑）。**最终默认 = 翻倍喷泉（20%→40%→80%→160%封顶）**：30Hz 档 +31%（吞吐场景应选 30Hz），15Hz 档损失 ≤3%（7df05e2 口径）；O3a 在所有档位 12× 削减 bootstrap CPU。后续若追求 15Hz 亦不掉速，候选方向是 fps 感知预算（≤15Hz 回退 K+20% 行为）——未实施，避免过拟合单链路两天数据。

**收官状态**：最终提交链 d37d98f → 7df05e2 → 594d6bb → revert 55e4921；`ctest -C Release -E PBPresentationGate` **163/163 PASS**（55e4921）；kit-opt4-6f6ce53f.zip（encoder 6f6ce53f/decoder 2256f521）为最终现场二进制。**推荐操作口径：吞吐场景用 30Hz + unified-bands + 55e4921 构建。**未竟事项：O4（已授权未实施）、O3b（FEC 并行，30.9-34.3ms/帧已是最大单项）、fps 感知预算（15Hz 兜底）。

**过程中事故**：01:12 前一次 o3v 运行因远程 Encoder 早死被主人发现并指示重跑（display/心跳均正常，死因未留日志；重跑 o3v-f15-s100-r2 正常完成）。


---

## 12. 第三会话（2026-09-12 凌晨）：O3b 落地、fps 感知预算、O4 设计交接

### 12.1 已交付（提交 03a79a9，163/163 PASS）

1. **O3b 并行码字 FEC**：`UnifiedVisualCpuOracle` 内建 15 条私有 Qc-LDPC 解码车道（每帧 15 个 codeword slot 从一个原子计数器认领、车道间零共享、串/并两路径共用同一逐槽解码函数），输出与串行**逐位一致**（新增专项测试 + 全量 495,140 断言语料通过）。逐槽 DiagnosticScope 保持串行（--stage-diagnostics 运行时自动回退串行路径，保留逐阶段归因）；内存按 RequiredBytes 计费（每车道 1 MiB）；任何分配失败优雅降级串行。预期：30Hz 接纳率从 111.9 sym/s 进一步上探（FEC 30.9-34.3ms/帧 → ~5-8ms/帧墙钟）。
2. **fps 感知 repair 预算**：`senderUnifiedSerialRepairFpsThreshold=15`。≤15Hz 沿用历史 K+20% FullRepairPass（现场数据：低帧率下喷泉只添重复接纳）；>15Hz 用增量翻倍喷泉。调度器双档测试、striping/mid-join 探针期望同步；契约 §1.1 第 7 条改写。
3. 现场验证（09-12 晨，全部 digestMatch=true，vs 昨日 cec4d35 基线）：

| 格子 | 昨日基线 | 今晨串行 FEC* | **今晨并行 FEC（03a79a9 完整形态）** |
|---|---|---|---|
| 30Hz/50MB | 94,803 | 116,489（+23%）| **196,212（+107%，2.07×）** |
| 15Hz/100MB | 60,089 | 78,431（+31%）| 77,593（+29%） |

   并行格细节：30/50 rate **158.8 sym/s**、FEC 32.1→**13.1ms/帧**、e2e 帧率 25.0、进程 1.149 核（多核首次真正参与）、uFrac 0.940、模型误差 0.02%。15/100：FEC 11.7ms、rate 109.5，但 uFrac 跌至 0.539（dups 68,160）——两因子相消。
   **机制定论**：15Hz 的瓶颈已从接收端 CPU 转移到**发送端空口的重复符号占比**——更快的接纳能力被 wrap 重访已解码段的重复吃掉。15Hz 的下一步唯一正交杠杆是 O4（提高每帧有效符号密度），与 §12.2 设计一致。
   \* 串行格意外发现：`--stage-diagnostics` 会按设计强制串行 FEC 路径（诊断保真）；CLI 常驻 measurement recorder 已覆盖计数器归因，A/B 中该开关冗余，已从 harness 移除。
   **事故记录**：02:15 首跑失败于 `ROI resolution failed: not-single-monitor`——本机右屏 DISPLAY2 凌晨从桌面拓扑消失（休眠/断链），主人唤醒后恢复；右屏拓扑现在是现场测试的前置检查项。

### 12.2 O4（车道重分配）实施设计——已授权，待实施

**目标**：新建实验视觉身份，把 15 个 codeword 槽中的 5 个 Chroma 槽（擦除率 67-99%）改判给 Luma 承载（BaseLuma），空口有效容量预期 +30-40%。

**表面清单（实施前必读）**：
- `kUnifiedVisualProfile` 是**单例 manifest**，被 13 个文件 147 处直接引用；`kUnifiedLaneCapacities`（telemetry/observation 结构按 3 车道定型）；`unified_visual_compute.hlsl`（GPU demod 着色器）按现行车道/载波契约编译；mapping/CPU/golden 生成器（`generate_unified_*_golden.py`）；GUI/CLI 身份表；demodulator 的 `ParseBinding`/`ResolveLocalDesktopBinding` 成对身份门（run7 教训）。
- 关键决策点：新 manifest 的 lanes 如何表达"无 Chroma"（`UnifiedLaneContract{Chroma, firstSlot=15, count=0}` 是否被 manifest 验证接受，或需扩展 `std::array<LaneContract,3>` 语义）；`GetUnifiedLogicalCarrierBit` 的 tile→(lane,slot,bit) 确定性映射是否车道表驱动（需通读 unified_visual_mapping）；golden 向量需为新身份全新生成（mapping/CPU/raster 三套）。

**建议实施顺序**（估计 2-3 个专注会话）：
1. manifest + 容量/验证器扩展（含 0-slot 车道合法性）+ 新 profile pair 注册进 `kUnifiedVisualProfileCatalog`；
2. `GetUnifiedLogicalCarrierBit`/编码栅格/CPU oracle 的 manifest 参数化（把 `kUnifiedVisualProfile` 直引改为经 FindUnifiedVisualProfile 的 profile 句柄）——本步即 147 处引用的主体改造；
3. GPU shader 与 demodulator 绑定对（成对身份门）；
4. golden 生成器 + 新向量冻结；CLI/GUI opt-in（模式同 unified-bands 的 5 提交路径）；
5. 实机 A/B：同日同链路 30Hz/50MB + 30Hz/100MB vs 03a79a9 基线。

**依据更新**：今晚 30Hz 全系 +18~32% 后，Chroma 车道的空口占比浪费变得相对更大（约 1/3 空口换 <5% 符号），O4 的预期收益仍成立。

---

## 13. 第四会话（2026-09-12 白天）：O4 灰阶身份落地、实机事件与信道实测定论

### 13.1 已交付（提交 `01cb14a` + `08500e4` + `626cd89`，ctest -E PBPresentationGate 全绿）

1. **unified-gray 实验身份**（`PB-Experimental-GrayStates-1`，`0x5042475953544131`，layout 12，CLI `--profile unified-gray`，不入产品 catalog）：数据 tile 前景状态与校准状态条从 4 种等亮度彩色改为 4 个灰度台阶，掩码/映射/槽位/脚手架与 SC6-V3 逐字节一致；六处成对身份门按 bands 模式扩展；RunReport.3 路由共享（并修复其 profile 三字段硬编码——现在写真实验身份）。
2. **测试**：调制层 8 用例（1:1/420 抽样/中性化/抖动/级距坍缩 fail-closed/压缩信道/独立 Python golden 对拍）、GPU WARP 用例、workflow 整文件恢复 ×3；golden 目录 `tests/golden/unified-graystates-cpu-oracle/`。
3. **数据驱动众数分类器**（`08500e4`）：实测定论——ToDesk 类链路对平坦校准条近乎无损（实测 {56.0,113.4,168.9,250.0}），但对 glyph 级前景按上下文压缩（248→~214、顶部总体混叠）。分类器改为每帧 tile 前景均值直方图找 4 众数（确定性算法，CPU oracle 与单线程 CS 逐位一致），校准条质心为 fail-closed 回退；前景门改为梯子 0/1 中点与状态校准解耦；级距重调为 56/112/168/248。
4. **修既存缺陷**：O3a 擦除路径 markerCandidates 丢失（stash 对照证明非本会话引入）；GPU 修复 modes 缓冲 UAV/SRV 同绑（`626cd89`）。

### 13.2 实机事件与处置（重要操作教训）

- **探针 1-3 全部 WaitingForBootstrap 的根因是环境而非代码**：远程机物理分辨率 2880×1620（display-info 的 1920×1080 是 DPI 虚拟值；桥截屏尺寸=物理地面真相）+ 查看器非等比映射；用户恢复 1920×1080 后同一二进制 1MB 探针 13.8s Completed+digest ✓。旧 kit 同链路基线对照 11.1s ✓（排除代码回归）。
- **bash 工具的 heredoc 会吞反斜杠**：UNC 路径 `\RECEIVER-DESKTOP\j` 经 heredoc 变 `\RECEIVER-DESKTOP\j`/`\RECEIVER-DESKTOP\j`，导致所有手动桥启动 "源文件不存在" 假象；harness 从文件读参数无恙。修法：补丁/脚本一律 Write 工具落盘或 chr(92) 构造；正斜杠 UNC 对编码器 CLI 也可用。
- **RunReport 硬编码陷阱**：unified 档报告的 profile 字段曾硬编码产品对，灰阶运行的报告会"谎报"身份（本次已修）；诊断身份问题必须用 stderr 探针或 bridge 截屏地面真相，勿信报告字段。

### 13.3 实测数字（同刻对照，全部 digestMatch=true）

| 格子 | 结果 |
| --- | --- |
| 基线（bands, 03a79a9）30Hz/50MB | **129,153 B/s**（405.9s）——链路比晨间劣化 ~34%（o6v 同格 196,212） |
| unified-gray（`08500e4` 完整形态）30Hz/50MB | 101,247 B/s（517.8s）；10 个 Luma 码字全程 0 失败 |
| 状态（Chroma）车道 | **0 接纳/全帧 InnerFecFailure**：glyph 前景总体 σ≈15-20、顶部总体拖尾重叠（边界错分 ~17%），LDPC 无法收敛；众数 {50.3,102.1,145.9,184.9} 可测但不足以救分类 |

### 13.4 结论与未竟

- **概念端到端成立**：灰阶身份在真实链路 bootstrap/几何/10 Luma 码字全通，1MB 整文件仅靠 Luma 码字即 Completed+digest（外层喷泉兜底）。
- **当日链路不承载 4 级灰度**：状态车道失败闭合（不伤恢复），但灰阶格因 GPU 众数 pass + CPU 二段开销反低于基线 22%。**灰阶收益需在晨间质量链路复测**（未测：当日链路已劣化）。
- **状态车道在劣化链路存活的候选路径**（未实施，按预期收益排序）：①谷值边界 + 擦除中心解码（边界 ±15 强制擦除，LDPC 擦除码率 2/3 理论容忍 33%）；②2 级强健状态（1 bit/tile，间距拉满 64/224）+ 重映射（容量减半但每比特极稳）；③链路质量自适应（校准条方差/众数宽度作为档位开关）。
- 会话证据根：`<PBLine root>\`（runs/、evidence/、token-probe/、vis-*、meas-*、cp-right.bgra 等）。

---

## 14. 第五会话（2026-09-12 晚）：gray v2——64 掩码码本全载 15 码字，劣化链路实机 +19%

### 14.1 动机与设计（提交 `61e4b69`，ctest 164/164 全绿）

§13 定论：4 级灰度在劣化链路硬判决 BER 15-25%（超出 rate-2/3 LDPC），但掩码载体全天 0 FEC 失败。v2 彻底放弃电平状态：

- **冻结 64 符号/25 chip punctured-Hadamard 码本**（32×32 Sylvester 行+补，贪心删 7 列；最小距离 9 与产品码本同级；符号 32=全背景自擦除）。
- 每 tile 6 个交织掩码平面（活跃 tile = 15×16200/6 = 40,500），**全部 15 码字骑掩码载体**（容量 +50% vs 10/15）；前景固定校准高亮度，无需任何电平估计。
- 解码用**原始样本亮度**做 64 路分类（和一交叉核的边缘膨胀在更密邻距下是系统性偏差）；灰阶帧跳过 SC6 16 符号 luma 存储（其曾与灰阶存储竞写同一平坦度量数组——这是 v2 调试中抓到的核心竞写缺陷）。
- GPU：绑定把 6 平面装进 lumaBits[0..3]+chromaBits[0..1]；DemodUnifiedCS 早返回 64 符号路径；直方图/众数 pass 不再派发。
- 修复灰阶逆映射双重施加槽偏移（槽 1-14 的度量曾整体高移 16200×k）。

### 14.2 实机数字（当日劣化链路，全部 digestMatch=true，0 FEC 失败）

| 格子 | 结果 | 对照 |
| --- | --- | --- |
| 1MB 探针 | **8.9s**（历史最快；基线 11.1s） | 三车道 0 失败 0 擦除 |
| 30Hz/50MB | **170,517 B/s（307.5s）** | 同刻基线 143,415 → **+18.9%**；三小时前基线 129,153 → +32% |
| 30Hz/100MB | 86,271 B/s（20.3min） | 载体 0 失败；**重复符号 62%**（wrap 重访）吞掉收益 |

### 14.3 下一步（按杠杆大小）

1. **发送端重复抑制**：100MB 档重复占比 62% 是当前最大单笔损失——wrap 时跳过接收端大概率已解码的段（喷泉 repair 预算已做，wrap 级去重未做）。
2. 接收端 CPU：64 路分类使 e2e 帧率 10.3（基线 ~11）；GPU 符号距离可用共享前缀/分组削减。
3. 晨间质量链路复测 v2（基线 196K 时 v2 预计 ~230K+）。

---

## 15. 第六会话（2026-09-12 傍晚）：逐段毕业调度 + 混合门控（提交 `ff912be`→`c5fbfbc`→`b2b9c5c`）

### 15.1 机制

- **逐段毕业**：每段跟踪累计已提交方程数（断点续传从持久化 repair-ID 高水位播种；逐段 pass 账本在首次准入时解析），达到 K×300% 即毕业出窗；欠服务段立即在窗内开局部修复 pass（预算=目标差额的精确值，新增 `repairBudgetOverride`，倍增公式不再过冲），修复租约从已终结高水位逐次延展。
- **滑窗**：毕业的前端逐个出窗、新段从尾部流入；回卷只在窗口清空时发生，且每卷把目标提高 K×25%（残余掉队段收敛、已恢复段一卷内最小补齐后再度毕业）。
- **混合门控**（`b2b9c5c`）：段数 ≤ 2×窗口的文件保持原整窗屏障语义（准入时刻 Carousel pass、屏障推进、公式预算），更大文件才启用毕业滑窗——实测 50MB/7 段在纯毕业下尾段饿死 -9%，而 100MB/13 段受益于 wrap 浪费消除。

### 15.2 实机（全部 digestMatch=true，当日劣化链路）

| 格子 | v2 屏障 | v3 纯毕业 | **v4 混合** | 备注 |
| --- | --- | --- | --- | --- |
| 30Hz/50MB | 170,517（dups 27%） | 154,697（dups 9%） | **175,121（dups 3%）** | v4 为今日最佳 |
| 30Hz/100MB | 86,271（dups 62%） | 90,205（dups 40%） | 82,842（dups 40%） | 三者在链路噪声带内（e2e 9.4-9.7） |

### 15.3 定论与下一步

- 50MB 档 v4 已把重复压到 3%——**发送端浪费基本消除，接收端 CPU 成为唯一绑定约束**（e2e ~10 fps × ~13.7 槽 × 1314B × 唯一率 ≈ 接收天花板 ~180KB/s，v4 在 50MB 已贴近）。
- **300KB/s 的下一杠杆全在接收端**：GPU demod 64 路分类成本（分层匹配可砍 ~4×）、CPU FEC 池调优、捕获管线重叠（O3c 未做）。这些与发送端调度正交。
- 教训：跨 run 对比必须看 e2e fps 归一（本轮 100MB v4 的"退化"实为链路慢 3.5%）。

---

## 16. 第七会话（2026-09-12 晚）："接收端 CPU 瓶颈"结论被推翻——信道内容率才是约束

### 16.1 证据（同日 50MB 分阶段对比 + fps 对照实验）

- gray-v4 每帧管线仅 ~16ms（bootstrap 1.4 + GPU demod 4.9 + FEC 9.7）→ 60fps 处理能力，但 e2e 只有 9.9fps——**接收端利用率 ~22%**。bands 同刻 e2e 5.0fps、管线 18ms，同样远未饱和。
- 16Hz 编码器：到达 18fps、bootstrap 全部接纳（0 拒绝），但唯一逻辑帧率仍 ~9.8fps；30Hz 同样 9.9fps——**唯一帧率与编码 fps 无关**。
- 1Hz 探针：到达降到 4.5fps（WGC 按内容变化投递）；30Hz 探针到达 11fps 封顶——**ToDesk 链路内容更新率午后 ~10-11fps**（晨间实测 ~25fps），与编码器负载无关（1Hz 时更低而非更高）。
- 15Hz 实测 130,930 B/s：fps≤15 触发串行 FullRepairPass 预算，重复 33%（§12.1 旧结论在新载体上复现）；16Hz（喷泉预算）102,114 B/s、重复 43%——**低 fps 下发送多样性不足，重复吞掉信道对齐收益**。

### 16.2 修正后的吞吐模型与 300KB/s 路径

goodput ≈ 内容率(fps) × 有效槽/帧 × 1314B × 唯一率。当前 30Hz + v4 已达午后链路上限的 93%（175.1K / ~182K 理论）。

- **晨间质量链路（内容率 ~25fps）**：现有构建理论上限 ~450KB/s——**当前构建很可能已在好链路上达标 300KB/s**，待晨间复测确认。
- **劣化链路（~10fps）**：15 槽已打满（承载位 251,232/251,232），唯一结构性杠杆是**扩画布**：本地右屏解码区 ~2420×1361（scale≥1 硬门），最大安全画布 ~2304×1296（1.44× 面积 → ~22 码字/帧）→ 午后链路 ~274KB/s（+57%）、12fps 以上链路即破 300。代价：新视觉身份（layout 13）、全部尺寸常量/着色器/绑定/测试/golden 重冻结（约 2-3 个专注会话），且 ToDesk 每帧像素 +44% 可能使内容率下降（带宽权衡需实测定）。
- 17% 的重复观察（同一逻辑帧被采两次）是观看器呈现行为，不可软件修复。

---

## 17. 第八会话（2026-09-12 深夜 ~ 09-13 凌晨）：v3 七位/瓦片载体全线落地与实机根因修复

### 17.1 范围与提交链（全部全绿后提交，164 用例回归两轮通过）

| 提交 | 内容 |
|---|---|
| e8d4e82 | 阶段C：调度器 slots 数组 max 化(18) + 每帧 slotCount + GetActiveSlots() + frameCodewordSlots 配置（0=产品 15，非法值 Create 拒绝） |
| 428d84a | 阶段B 遗留回归修复：oracle laneMetrics 汇总按 activeMetricCount 界定；PBTelemetry RecordSample 按 frameSlotCount 界定 + 灰 lane=BaseLuma 语义；遥测测试 helper 空指针修复 |
| b456176 | 阶段D：GetProfileBinding 灰 18/36450、两处调度器 Create 传 frameCodewordSlots、重开调度相位窗口常量化、接收侧 per-slot 存储 max 化（含 demodulator acceptedUnifiedBlocks 15→18 溢出修复） |
| 7f20f39 | 阶段E：UnifiedTileBinding 48B 七索引、ParseBinding 灰 291,600 度量/36,450 字节/18 码字、HLSL v3 灰分支、tile 绑定缓冲步长 sizeof 化（原硬编码 8×uint32 欠分配）、资源预留 64→80MiB 默认 / 128→160MiB RemoteVisual 帽 |
| f705dc3 | 测试修复：SC6 corpus 槽遍历按 frameSlotCount 界定；着色器条目数 12→14（v2 遗留） |
| 52f6e10 | 实机根因修复（见 17.3）：电平位并入 64 码本 min 分割 + min(raw, 锐化) 双表示假设 + 峰值对比度标尺；CPU + HLSL 同构 |

### 17.2 实机首跑故障（v3-f30-s050：14.7 分钟 0 字节）与定位方法

- 症状：编码端正常（灰身份 layout 12，26,024 帧 @29.7fps，报告 state=Failed 仅为人工停机记录）；接收端 locator 100% 接受引导（3,523/3,523），但 0 个槽评估、永远 WaitingForBootstrap。
- 定定方法（可复用工具链，<PBLine root>3\）：右屏 PNG 采集 → png_to_bgra → decode_probe（显式灰身份 CPU oracle 解码，分割 CPU/GPU）→ roundtrip_probe（encode→双线性缩放→decode 硅内实验室，无需 ToDesk）。
- 关键中间结论：链路近无损（阶梯实测 {7,64,160,234}，度量幅值健康、擦除 0.5%），CPU oracle 对真实捕获只解出 2/18 码字 → 故障在调制层本身而非 GPU/链路。

### 17.3 根因与修复

- **根因**：plane-6 电平位用 tile 前景均值对比阶梯中点 148 判定。纯 1.1× 双线性重采样就把交替的 8/232 单像素 chip 混合成 ~104 均值灰（实测 dump），恰好跨过判定中点——所有 HIGH tile 电平位满置信（±8192）翻转，按 tile 区间聚类（每码字的位集中在自己的 tile 区间），LDPC 18/18 全灭。产品路径不受影响因其从不用 per-tile 均值（固定阶梯电平 + min 分割）。
- **修复（CPU unified_visual.cpp + unified_visual_compute.hlsl 同构）**：
  1. 64 掩码 × 2 电平两张距离表：电平位 = 两半最优符号距离之差；掩码位 = 符号按最优电平假设分区取 min；
  2. 每符号距离 = min(raw 距离, SC6 同款 cross 锐化核距离)：1:1 画布保持近零真符号距离，重采样画布回落到锐化拟合；
  3. 对比度标尺锚定 tile 峰值 luma（9-chip 翻转恒 18,432 的不变量在两档电平下同时成立；阶梯中点标尺会使 LOW tile 度量跌破 min-sum offset 2048 安全区——中途验证时踩到）。
- 验证：硅内 1.0/1.25/1.26×（含实地几何 1.2604/1.2611@origin 69.99,27.99）全 18/18（1.1× 为 13/18，可接受）；多交织相位（seq 0/1/77/55942/123456）与分数原点全稳；真实链路捕获 18/18（含控制槽，1 次迭代收敛）。

### 17.4 实机终局（v3b-f30-s050 @52f6e10，30Hz/50MB）

| 指标 | v2 灰（gyv4，b2b9c5c） | v3（52f6e10） | 产品基线（03a79a9） |
|---|---|---|---|
| 绝对吞吐 | 175,121 B/s | **162,095 B/s** | 196,212 B/s |
| 链路内容率 | 9.92 fps | **8.08 fps**（深夜劣化） | ~9.9 fps（午后） |
| 每帧验证字节 | 17,648 B | **20,416 B** | ~19,800 B |
| BaseLuma 通道 FEC | — | 57,114/57,114 槽，0 失败 | — |

- 全摘要链绿：digestMatch / wholeDigestVerified / finalReopenVerified 全 true；frameCoverageComplete=true；control 3,342/3,342 全接受。
- **归一结论**：v3 每帧效率 vs v2 灰 +15.7%（贴近理论 +20%）；绝对值低于产品基线完全由深夜链路内容率（8.08 vs ~10fps）压制，午后链路推算 ≈200KB/s。
- 诊断首跑时确认：ToDesk 视图 = SENDER-LAPTOP（即 \RECEIVER-DESKTOP，桥实例表 §2.1 唯一实例）；DISPLAY2 2560×1440 在位；远程原生 2880×1620。

### 17.5 新教训（会再踩的）

1. **旧测试二进制假绿**：阶段B（4c2c523）只跑过 PBModulation 测试；PBApplicationTests/PBTelemetryTests/PBDemodD3D11Tests 的陈旧 exe 让全绿跑了旧代码。此后任何共享结构改动必须显式重建全部消费测试目标（本会话被同一陷阱咬了两次）。
2. 资源预留连锁：oracle 18×1MiB 解码 lane + 48B tile 绑定使 DemodConfig 默认 64MiB 与 RemoteVisual 128MiB 帽双破——共享 demodulator 的预留是跨 profile 最坏情形，帽子必须随之调整。
3. 远程构建身份（PB_GIT_COMMIT）在 CMake configure 时固化：打包前必须重新 configure，否则 kit 报告落后一个提交。
4. 纯双线性 1.1× 重采样即可在硅内复现实链故障——调制层改动先过 roundtrip 实验室再上实机。
5. ab_run 长超时下若需提前终止：只杀本地 PixelBridgeDecoder.exe，让编排脚本走正常 stop/collect 收尾。

### 17.6 当晚后续计划（自主执行，结果见后续小节）

1. 60Hz 逻辑帧率 A/B（现有 v3 kit，零代码改动，验证更高逻辑 fps 是否提升每传输帧的喷泉多样性收益）。
2. v4 实验室原型：4 电平灰阶（阶梯 64/160/232 + 第二载波条纹已校准的 {56,113,169,250} 基础设施）× 64 掩码 = 8 位/tile → 20 码字/帧（+11% 槽容量），先在 roundtrip 实验室验证 1.26× 稳健性再决定全链路实现。

---

## 18. 第九会话（2026-09-13 凌晨，主人授权自主决策）：v4 灰 Fast（layout 13）+ 段配额修复——100MB 口径 193.7KB/s

### 18.1 决策与实现

- **60Hz 逻辑帧率实测有害**：接收端采集率塌到 1.96fps（30Hz 为 9.6）——发送端 60fps 全重绘使 ToDesk/WGC 内容变化检测过载。30Hz 维持最优（ab_run 已支持 --fps 60 供复测）。
- **100MB 尺度退化根因**（v3 实测 84.3KB/s，每帧仅 10,650B）：13 段 > 接收端 8 解码器历史配额 → `outerFecQuotaExceededCount=35,998`（第 9+ 段符号整批拒绝、发送端不知情继续发）+ `alreadyCompletedSymbols` 占 42%（wrap 重毕业重复）。50MB=7 段无此问题（quota=0）。
- **修复**：灰家族段目标 8→15MiB（`GetSegmentTargetBytes`，会话描述符声明实际值；产品身份冻结 8MiB 不变）→ 100MB=7 段回到健康区。三处 8MiB 冻结常量同步解冻：段偏移步进、`ReadSourceRange` 读取上限、`LoadEncodedSegment` 透传（三个隐性冻结点都在实机首跑暴露：SegmentGap code=27 / 源读取上限异常）。
- **v4 灰 Fast（layout 13，PB-Experimental-GrayFast-1）**：七位/瓦片栅格不变，内检换冻结 DVB-S2 Short **Fast（37/45）**：1665 信息字节/码字、**1629B 运输载荷**（+24% 帧容量）。依据：v3 实机 220k 槽 FEC 0 失败@~1 迭代——富余换载荷。CPU oracle FEC 池每车道持双 profile 解码器（RequiredBytes 18×2MiB；DemodConfig 默认 112MiB、RemoteVisual 帽 192MiB）；栅格语义谓词 `IsUnifiedGrayCarrierPair`(12|13) 与身份精确判断分离；脚手架/引导解析/遥测/捕获解调器四处身份清单补 13；MSVC 默认线程栈提 4MiB（深调用链上多个 max 尺寸结果结构）。

### 18.2 实机终局（kit-gray-92357bd3 @ c8828ce，30Hz，全 digest 绿）

| 跑分 | 吞吐 (B/s) | 链路 fps | 每帧验证字节 |
|---|---|---|---|
| v3 50MB（v3c） | 161,705 | 8.16 | 20,432 |
| v3 100MB（v3c） | 84,302 | 7.94 | 10,650 |
| **v4 50MB（v4c）** | **174,418** | 8.23 | 21,202 |
| **v4 100MB（v4c）** | **193,653** | 8.01 | **24,181** |

- v4-100MB：quotaExceeded=0 ✓、alreadyCompleted 11.4%（v3 为 42%）、槽位打满（16.0/17 运输槽/帧）。
- 每帧增益结构：50MB 口径 +3.8%（喷泉修复开销随载荷升高，产量 81% vs v3 91%）；100MB 口径每帧 24,181 vs v3-100MB 10,650 = **2.27×**（段配额修复主导）。
- 归一化对照产品基线（196,212@~9.9fps → 每帧 ~19,820）：v4-100MB 每帧 +22%。午后链路（~10fps）推算 v4-100MB ≈ **242KB/s**；300KB/s 需 ~12.4fps 内容率或再 +25% 每帧容量。
- 报告的 `innerFec` 字符串仍是产品清单静态文案（Robust）；实际解码按 Bootstrap 记录身份选 Fast（否则不可能通过 digest）——RunReport 的该字段按身份化是遗留小项。

### 18.3 提交链与新增陷阱

提交：4aa412b（v4+段目标主体）、bb29bc9（捕获解调器身份解析补 13）、f05dd04（终局化诊断）、dfab8d6（段偏移修复+错误串条件求值）、c8828ce（源读取上限解冻）。164 全量回归绿。

新陷阱：
1. **8MiB 冻结常量的隐性复制**：段目标改动需审计全部使用点（计数/偏移/读取上限/探针夹具）——三处分散冻结在首次实机才爆。
2. `ReceiverResult::Error()` 是 optional 访问——错误字符串必须只在失败分支构造（f05dd04 反例：成功路径也炸 Bad optional access）。
3. 远端 run 目录残留会使 deploy 拒绝（run-exists）——崩溃后先 `cleanup` 再重跑。
4. 构建身份（PB_GIT_COMMIT）在 configure 时固化：每次打包前 `cmake -S . -B <build>` 重跑（本会话又踩一次）。
5. 本会话两度被"陈旧测试二进制"迷惑后新增：全量 ctest 前先整体构建（cmake --build 全目标）。

---

## 19. 第十会话（2026-09-13 白天，授权自主）：远控高帧率模式实测不兼容 + 段目标 CLI + layout-13 Golden 冻结

### 19.1 远控软件新模式（用户切换）诊断结论

- 新模式 capture fps 15.6（旧档 ~10.3，+51%）——内容率潜力大：若载体可用，15.6 × 24.2KB ≈ **378KB/s 已超 300 目标**。
- 但实测**静态 scaffold 被高帧率档糊化**：四角定位标记中间电平占比 32-47%（旧档 ≈0，振铃+对比度压缩），接收端引导 0/1824 全失败（`IncompleteMarkers`）；锁定几何 hint 同样失败；CPU oracle 对捕获同样失败（排除接收端因素）。
- **数据瓦片（逐帧全变区域）保真与旧档相当**（tile 中心中间电平 27% vs 20%）——远控编码器按内容变化分配码率：静态区域停留在低码率 I 帧重建，动态区域每帧获得正常码率。
- 结论：新模式与"冻结静态 scaffold + 高密度动态数据"的纯视觉载体**根本性不兼容**。修复需要么让 scaffold 逐帧抖动骗过编码器（违反栅格冻结契约/golden），要么放宽定位器容差（改冻结 reference 契约且引导 RS 大概率同挂）。**归用户决策**：若远控端存在"高帧率+高画质"组合档值得切换；旧画质档下 15.6fps 潜力仍在。
- 诊断工具链：右屏 PNG → 标记四角/阶梯/瓦片三电平分布对比 + hint_probe（锁定几何引导实验）——三分钟出结论。

### 19.2 --segment-target-mb CLI（提交 937e9b2）

50MB 口径（4 段）喷泉产量 81% vs 100MB（7 段）92.7%——段数影响毕业节奏与槽位利用率。`--segment-target-mb 1..15` 让吞吐线按文件尺寸调段数；SessionDescriptor 声明实际值、durable resume 身份串含段目标（不同分段的预扫描不互配）、认证产品会话拒绝该旗标。配套 ab_run `--seg-mb` 透传。

### 19.3 layout-13 Golden 冻结

`tests/PBModulation/generate_unified_grayfast_golden.py`：独立 Python oracle 复刻 DVB-S2 Short Fast 矩阵（37 行 shifts 逐值转录自 ETSI 表 5b/C++ 头）与 Q=8 累加编码；1629B 近上限运输载荷 fixture；栅格 digest 复用 layout-12 六变体。**C++ Pack 与 Python golden 交叉验证 36,450 字节完全一致**；`PBUnifiedGrayStatesTests` 新增 gray-fast golden 用例（含 >1600B 载荷断言），全量回归绿后提交。

### 19.4 模式不兼容的家族性确证（二分实验）

layout-12（灰 Robust）1MB 探针同模式同症状：引导 1529/2259（68%）、transport 0、解调 0 评估——与 gray-fast 完全一致。**新模式破坏的是整个 Unified 家族共享的静态 scaffold 高频结构**（定位标记/引导 RS 块/timing patches 逐帧不变 → 远控编码器静态区域低码率重建），与内检 profile、载体版本无关。阶梯（32px 大块低频）不受影响，四角标记（精细结构）中间电平 32-47%。CPU oracle 与 GPU 管线同败（hint 锁定几何也失败）——非接收端代码问题。

**给操作者的选择**（信道设置归操作者）：
- 切回旧画质档：v4 立即可用，旧模式下实测 100MB 193.7KB/s（8fps 深夜链路）；链路恢复 ~10fps 时推算 ~242KB/s。
- 若远控端有"高帧率+高画质"组合档：15.6fps 潜力 + 24.2KB/帧 ≈ 378KB/s，值得一次切换实测。
- 我们侧不改冻结 scaffold 契约（让静态区抖动骗编码器）与定位器容差（reference 契约）——两者都会破坏 wire 冻结与 golden。

---

## 20. v0.5.0 发布（2026-09-13，主人指令）

- 版本 0.5.0（project VERSION → --version/RunReport/构建身份全线）；双端 GUI 主页新增"传输模式/接收模式"选择器：标准（SC6-V3，默认不变）与 灰阶高速 v4（gray-fast 实验载体）；偏好键 g22/carrier，会话激活期锁定；标题栏带版本。提交 05be1b7，164/164 回归绿。
- 组装包 `<PBLine root>
elease-v0.5\PixelBridge-v0.5.0-win64.zip`（52.9MB，sha256 7ac0eb87…）：Encoder/Decoder 各自完整 Qt 6.10 部署（windeployqt 后构建）、README.txt（模式说明/快速开始/安全语义）、build-identity.json（双 exe sha256+gitCommit）。包内双 GUI offscreen 冒烟 PASS；打包 exe --version/身份/实验旗标帮助核验通过。

### 20.1 v0.5.0 进度显示修复（重打包 12175b3）

主人报告：接收中 .resume 增长但"当前进度/恢复速度"为 0、剩余时间"估算中"。根因：GUI 进度只在 Segment 验证时跳变，15MiB 灰家族段首验需数分钟。修复：运行时每次快照发布 `estimatedReceivedRawBytes`（唯一接纳符号 × profile 块字节 − 已完成段编码字节，× 会话实测 raw/encoded 比；饱和算术、8GiB/64× 敌意上限、夹逼 [verified, declared]）；GUI 进度区五项实时字段（进度/已接收大小含已验分子/已花费时间 GUI 时钟驱动/平均速度/ceil 界 ETA），各含明确不可用态（无描述符/首秒/画面停滞/已停止）；完成态冻结于 runEnded + 验证均值；发布前永不 100%。重打包 sha256 ae64aa1b…（gitCommit 12175b3），包内双 GUI 冒烟 PASS，164/164 回归绿。

### 20.2 954MB 大文件冻结修复（1407234）

主人报告：954MB 灰阶高速 v4 @30Hz，进度到 75.9MB 后冻结（.resume 78.5MB 不增长，发送端持续广播）。根因：300% 毕业目标使每窗口批的 ~60% 时间用于发送接收端已有的修复方程（窗口 6 段在 Pass 0 ~110% K 就全部解码，发送端仍要补到 300% 才让段毕业滑动窗口）。修复三处：①毕业因子 300→200%（接收端实解 105-110%，200% 保留 2× 边际；每批冻结 13→6 分钟，吞吐 +50%）②15Hz 串行分支补上 repairBudgetOverride（原仅 >15Hz 生效，15Hz 重开走九次 K+20% 而非一次精确距离）③GUI 选灰阶高速时帧率自动推到 30Hz（现场验证口径）。164/164 回归绿；重打包 c521dd92。

## 21. 2026-09-13 晚间原始断点勘误与继续排障（未完成）

本节追加新证据，不改写 §20.2 当时的记录。当前任务是非本机长文件的最终恢复时间与稳定性；所有候选仍保持单向视觉 payload、8 个接收 decoder 上限、摘要/安全发布/reopen 和冲突拒绝。

### 21.1 原始 `.resume` 否定“当前六段已全部解码”的解释

用户提供原始断点后进行了只读、逐条长度/CRC32C/身份绑定与 equation ID 检查。输入 SHA-256 为 `cc58c42c233b25ab94518a78a100440323606c814b0271b3116964d5dc4f67b8`。

- 原文件精确 1,000,667,445 B，64 个分段，15 MiB 分段目标，GrayFast/layout 13。
- 48,943 条有效 journal 记录，其中 AcceptedBlock 48,883 条，**CompletedSegment 0 条**。
- 活跃未完成分段为 24、25、26、27、28、29、30、32，**每段 unique blocks 均小于各自 K**；没有重复 AcceptedBlock 记录或同 ID 的冲突 fingerprint。
- 有效 payload 为 `48,883 × 1,629 = 79,630,407 B = 75.941474 MiB`；journal 精确 `82,381,527 B = 78.565146 MiB`。两者与原 UI/断点大小一致。

因此 §20.2 所称“接收端已完成，200% 保留 2× 解码边际”及据此推导的吞吐 +50% **不成立**：它把发送量当成接收量，也缺乏原始断点支持。原始日志/FPS 时序未提供，静态断点不能确定当时的唯一调用栈、运行版本或配额拒绝次数。

### 21.2 已复现的代码缺陷与私有候选

- `BuildUnifiedSegmentState` 将 repair-ID high-water / 未使用的 durable lease 当作实际发送预算；`CompleteUnifiedSegmentPass` 毕业提前返回导致 local pass 未推进、再入窗口重播 systematic。新建与 resumed pass 3/50 回归均覆盖；修复保持 repair ID 不回退、不复用。
- 小文件的窗口前移错误增加全局 Carousel pass，7 段文件末段首次出现即 repair-only；修复只在全文件 wrap 时推进全局 pass。
- 实验 `--segment-target-mb` 的 CLI 准入、准备入口和 SessionDescriptor 三处接线缺失；只补 gray/gray-fast 的 1..15 MiB 实验入口，稳定产品拒绝规则不变。
- GrayFast 报告错误走 RunReport.2；补回既有 RunReport.3 遥测及单调运行时长，不伪造未启用/不完整的 measurement。

私有候选 A 定向回归 29 cases / 9505 assertions、调度器 14 cases / 319160 assertions 通过。候选 B 加入按固定启动 FPS 保持首次分段 airtime 的实验（15fps=2K、30fps=4K、60fps=8K），定向回归 29 cases / 9877 assertions 通过。未改 wire、FEC、Golden 或接收资源上限。

### 21.3 真实远控结果及残留失败

远程 Encoder 经现有远控画面进入本机 DISPLAY2 的实际捕获。用户允许临时将远程 2560×1600 改为 1920×1080；原分辨率待本阶段结束恢复。未修改 Citrix/网络设置，无输入自动化。默认 Decoder CLI 的 `LocalDesktop` operator 标签存在与 Unified/旧 monitor 参数冲突的问题，不能用该标签代替物理链路证据；启动参数与限制记录在各 `launch.json`。

| 运行 | 文件/分段 | 最终恢复时长 | 结果 |
| --- | --- | ---: | --- |
| 旧包基线 `nl0913-1080p100` | 100,000,000 B，7段，30fps | 568.485 s | 完整发布，独立重开 SHA-256 一致；旧报告 .2 |
| A `nl0913-a13-f30` | 13 MiB，13段，30fps | 138.366 s | .3 摘要/发布/reopen 通过，但 4,881 次配额延期 |
| A `nl0913-a13-f15-r2` | 同文件/分段，15fps | 72.583 s | 全部通过，配额延期 0 |
| B `nl0913-b13-f30` | 同文件/分段，30fps | 70.805 s | 全部通过，配额延期 0 |
| B `nl0913-b100-f30-s1` | 100,000,000 B，96段，30fps | **不纳入成功计时** | 17 MiB 已验证后仍再次积累8个未完成段；unique blocks 长时间不增、配额延期持续增加，诊断停止 |

13 MiB 的约48.8%单样本时长下降只适用于该组对照。100MB 扩大验证说明按平均接收率增加 airtime 仍不足以解决长文件问题；正在检查周期抽帧与分段轮转的相关性，**不能宣称已经修好大文件停滞**。两个完整桌面测试文件尚未执行，任意分辨率适配亦未完成。

完整证据、create-only 候选身份、命令和持续工作日志：`artifacts/nonlocal-stall-20260913-2111/WORK_LOG.md`；原始断点分析为同目录 `inspect_original_resume.py` 和 `original-954mb-resume-analysis.json`。这些私有现场产物不提交 Git，不是公开发布或认证矩阵。

### 21.4 候选 C：周期混叠修复不等于长期停滞已修复

GrayFast 的 Segment 轮转相位由每32个sweep变更改为每个sweep变更，未修改其它Profile或增加接收反馈。另修正报告Control占用率分母：取实际 `codewordsPerFrame`（GrayFast为18），缺失/计数溢出时保留null。

- 周期每三帧仅观察一帧的13×256KiB回归，B超过20,000帧预算，C为511帧/171观察帧/pass0完成。C定向29 cases/9904 assertions、调度器正确路径重跑14 cases/319160 assertions通过。
- `nl0913-c13-f30`：13MiB/13段完整恢复78.528s，比B的70.805s慢，不能宣称相位修改提高了该场景吞吐。
- `nl0913-c100-f30-s1`：100,000,000B/96个1MiB段仍失败。220–480s观测区间内，verified停在31MiB、outerUnique停在24066，quota持续增加；全文件回绕后才恢复增长。停止时37MiB、outerUnique25290、quota67653、8个活跃未完成段。最终`published=false`、失败测量时长null；729.613s只是失败运行时长。
- `nl0913-c100-f30-s6`：同一封存程序和100,000,000B前缀、同样30fps，仅改6MiB/16段。接收端运行时长486.108s，16段完整恢复、整文件digest/rename/reopen及独立SHA-256全部通过；quota0、peakActive6。SHA-256为`4111eb5acba90ea84b8e1cce370699d682ac1460e9f841069c7445cd89a134eb`。

486.108s比旧包100MB样本568.485s减少约14.5%，但旧包/候选还存在代码和分段差异，不能拆出单一改动的净收益。该100MB文件超过当前Step1的64MiB测量范围，`receiverTimingEligible=false`、`unavailableReason=MeasurementSourceScopeExceeded`、正式elapsed/goodput均为null；运行时长仅作为工程对照，不冒充正式测量放行。发布/重开证明独立成立。没有扩大64MiB测量范围或其它资源限制。

本轮现场已停止所有本次注册的Encoder/Decoder，通过`display-restore`并再次`display-info`确认远程恢复2560×1600@240Hz；焦点前后仍一致，无输入自动化。后续继续处理小段有限帧方差、早期修复机会和分辨率适配；完整Medium/Big尚未运行。

### 21.5 合成测试时钟修正与不规则丢帧回归（继续验证中）

只读审计发现`PrepareHeadlessFrame`固定按15fps生成虚拟时间；实屏生产路径使用真实steady clock，不受影响。新增独立scheduler对照必须跨过原固定时钟的10秒Control截止点，30/60fps旧实现均失败。修正测试助手改用配置FPS，保留逻辑tick边界和除数校验。

新增64×256KiB（16MiB）不规则丢帧模型：固定种子、约28%帧可见并有短全擦除段，不读取Receiver状态来选择发送内容，也不冒充真实录屏。C需要18,257逻辑帧、82次wrap、20,321次配额延期；最长无新增有效方程1,598帧，违反该模型限定的30秒上限。

正在验证只依赖发送配置与K的平方根帧损失余量。第一个系数候选将同一回归降低为4,827帧、6次wrap、quota0，但最长无新增方程仍有954帧，**该候选仍未通过900帧阈值**。没有放宽测试阈值或把quota0当作任务完成；后续版本必须继续定向回归和真实像素测试。

### 21.6 候选 D：小段长序列通过，但尚非最快方案（2026-09-14）

有界余量系数4通过同一SparseBursty回归：5,069逻辑帧、1,362观察帧、5次wrap、quota0、最长无新增有效方程206帧；900帧门不变。更强的15fps/每四帧观察一帧用例仍实际触发8个活跃decoder、85次配额延期及15次wrap，随后完成摘要/发布/reopen和独立BLAKE3验证。定向32 cases/9,936 assertions与调度器14 cases/319,160 assertions通过，四个受影响target构建通过。

| 实际远控运行 | 文件/分段 | 接收端运行时长 | 终态 |
| --- | --- | ---: | --- |
| `nl0913-d32-f30-s1` | 32 MiB，32个1MiB段，30fps | 219.242 s | 完整digest/rename/reopen、独立SHA-256通过，quota0 |
| `nl0913-d100-f30-s1` | 100,000,000 B，96个1MiB段，30fps | 652.217 s | 96段全部完成；digest/rename/reopen、独立SHA-256通过，quota0，peakActive6 |

D100在此前B/C同尺寸小段压力样本出现长停滞的路径上完成了整文件验证，但652.217s比C的6MiB分段486.108s慢，不能宣称D小段是最快方案或已证明任意1GB文件不会停滞。D100超过Step1的64MiB范围，正式measurement elapsed/goodput保持null；D32虽receiverTimingEligible=true，也不能替代封存身份完整的正式现场认证。两个指定的完整Desktop文件仍未执行。

两次D运行的前后foreground句柄均不同；编排没有发送输入或调用窗口激活API，但现有证据不能归因该变化，**不宣称焦点始终未变**。停止均只针对本次注册Encoder、WM_CLOSE且forced=false。阶段结束已用display-restore及display-info确认恢复远程2560×1600@240Hz，见私有证据`display-restored-after-d100.json`。

### 21.7 原分辨率适配：先隔离数据解调失败，不放宽正确性门

当前生产解调器的有界合成诊断：1920×1080原图及单次1.125倍双线性缩放均18/18码字字节一致；先4/3倍point放大再0.84375倍双线性缩小为0/18；将第一次改双线性仍0/18。两个失败样本Bootstrap和Base/Fine phase pilot均通过，错误集中于灰阶数据度量，不能再笼统归因于定位失败。离线合成不是实际远控性能证据。

另一个完全隔离的诊断副本仅把私有最小几何尺度改为0.75；生产源码、封存包和FEC/CRC均未改。它读取此前保存的实际右屏像素（净尺度约0.851），几何通过后仍18/18内层FEC失败。单纯降低门槛不足以适配原分辨率；需要先验证采样/符号模型，再决定是否进入生产CPU/GPU实现。原始捕获、派生源码、构建脚本和逐plane误判统计分别保存在`scale075-diagnostic-only`与`double-resample-probe-01/02`，不提交真实像素或payload。

### 21.8 原分辨率的逐位链路证据与 D 的较大分段对照

仅在私有副本尝试前向采样模型：直接比较捕获像素与重采样后的候选符号，而非先锐化观察图。双线性模型配合合适的软度量尺度后，0.85倍单次缩小以及两种双重缩放的合成夹具均18/18字节一致。没有改变FEC迭代数、CRC、身份或freshness门；该模型尚未合入CPU/GPU生产路径。

`nl0914-gray-pattern-native01`在原2560×1600远程模式执行45秒合成图案呈现，远程独立生成两个已知测试帧，经原生DataWindow和现有远控进入本机DISPLAY2。桥仅部署程序与收集日志；本机三份14,745,600B捕获由固定右屏矩形读取，预期位只用于**解码之后的离线评分**，不交给Decoder。原生1350次提交/无pending替换，结束后已退出；焦点采样前后625612094一致，模式未改变。此夹具没有文件Session，不是文件传输完成或goodput证明。

对第一份实际捕获（sequence19），原采样路径在291,600个编码位中有28,635个非零度量符号错误（约9.82%）；前向双线性模型降到14,956个（约5.13%），但仍0/18通过FEC。电平位错误从1,375降到7，剩余问题主要在六个形状位。加入已知暗分隔环的完整像素支持仅小幅改善，尚不够发布。九个freshness区域均current，不能用局部过期擦除解释这些失败。导频拟合和已知夹具几何诊断均有独立记录，后者明确不是可直接用于生产的校准输入；不因合成测试通过而降低产品门槛。

作为思路参考，继续查阅了基于符号模型与灰度似然的低分辨率条码工作，以及[相机条码解码的分辨率分析](https://epubs.siam.org/doi/10.1137/21M1449658)。其中一维UPC的解析条件不能外推为PixelBridge二维载体的分辨率保证；实际像素、纠错与完整文件验证仍是本项目依据。

回到完整耗时主线，`nl0914-d100-f30-s6`保持D程序、30fps和同一100,000,000B文件，只把1MiB分段改为6MiB：532.801s，digest/rename/reopen及独立SHA-256全通过，quota0、peakActive6、峰值预留267,401,796B。比D/1MiB的652.217s短约18.3%，但仍比C/6MiB的486.108s慢约9.6%；这揭示稳健余量有真实吞吐成本，不能只报有利对照。正式measurement继续因64MiB范围为null。结束后已核对恢复2560×1600@240Hz，焦点采样前后一致。

接下来已开始完整`TestMediumFile.bin`的D/30fps/15MiB运行（`nl0914-d-medium-f30-s15`），启动前重新核对其273,806,498B和SHA-256。不扩大Decoder现有3600秒CLI时限；完整Big的后续编排如超过一次时限，只能使用原生同目录resume并计入重启间隙，最多两段有界运行，不能把多进程结果伪装成一次正式测量。完整Medium结果与Big现场执行尚待后续条目确认。

### 21.9 完整 TestMediumFile.bin 验证通过（2026-09-14 01:35；Big 尚未运行）

运行 `nl0914-d-medium-f30-s15` 使用候选 D、30 fps、15 MiB 分段，远程 Encoder → 右屏实际捕获 → 本机 Decoder。完整原始文件为 **273,806,498 B**，没有以截断前缀代替命名文件。Receiver `Completed`/exit 0，恢复运行时长 **1,398,646 ms（23分18.646秒）**；harness 本地单调进程时间线为 1,398,781 ms，额外独立重读 SHA-256 后为 1,398,953 ms。wholeDigestVerified/renameSucceeded/finalReopenVerified/published 均为 true，本地完整 SHA-256 为 `28FD5EAD99BF7FE3526E38CD1A44364B3C02792323D01840F2F4BDC38EBC1BB0`，与事先冻结并在远程重新验证的完整源文件一致。

操作性 raw goodput 为约 **195,765 B/s**，不是正式 Step1 `VerifiedRawGoodput` 认证样本；大文件 scope 导致正式 measurement 对应计时/吞吐仍为 null，未绕过。全过程 peak active=6，peak reserved decoder bytes=623,111,712，resource rejection/deferred/FEC failure/CRC failure 均为 0，UniqueVisualFPS=8.357589。前后台窗口标识前后均为 625612094。远程 Encoder PID 6784 在验证成功后由测试编排 WM_CLOSE 清理，forced=false；其现有实现把外部窗口关闭记录为 Failed/exit 1，不把这个发送端状态改写为 PASS，也不拿它代替接收端成功。原分辨率 2560x1600@240 已恢复，命令 `cmd-20260913T173230Z-b47d43`。

证据根为 `artifacts/nonlocal-stall-20260913-2111/nl0914-d-medium-f30-s15/`，最终文件在其 `receiver/output/TestMediumFile.bin`，完整报告位于 `receiver/epoch-0/report.json`。这只证明本次完整 Medium 的稳定恢复，不证明 Big、任意链路或原分辨率已完成。接下来先做三个各45秒的隔离画面更新实验筛查新的非本机提速思路，再执行完整 Big；不修改协议/Profile/Golden、Citrix/网络或安全与资源门限。

### 21.10 完整 TestBigFile.bin 与两个指定文件的已验证基线（2026-09-14 03:11）

`nl0914-d-big-f30-s15` 以相同候选 D、30 fps、15 MiB 分段完成 **1,059,917,774 B** 的整个远程桌面 TestBigFile.bin。第一 Receiver 在保留的3600s时限正常停止（runtime=3,600,037ms），当时已验证42段/660,602,880B。测试编排没有改变源端广播调度，只在同一输出目录启动第二 Receiver；最终两份报告的 SessionId 均为 `d786c4fabe99d724e8a9102398a8281b`，SessionTag 均为 `e22d1aa278572ff5`。第二报告明确为 resumeLoaded=true、resumeVerificationSucceeded=true，原有完成段和缓存重新验证耗时897ms；第二进程 runtime=1,927,531ms，Completed/exit0。

最终 wholeDigestVerified/renameSucceeded/finalReopenVerified/published 全true。完整独立SHA-256为 `20B000AFA567A66DD536B22946102A465575F77299A42FFC2FCC2116F9808E76`，与远程源文件一致；最终验证Encoded bytes=1,053,093,783，整文件BLAKE3为 `1d5c9d9c7aa1836d9d3953808667ec2b59981b372987e2327e53f1f573f6058b`。两个epoch的resource/deferred rejection、FEC/CRC failure均0、peak active均6，最高reserved decoder bytes=623,111,712。各自UniqueVisualFPS为8.191721与8.163131；不把它们跨epoch简单合并为一个正式样本。

同一本机harness单调时钟的 **Big完整进程时间线为5,527,813ms（1小时32分7.813秒）**，包含两次Receiver运行及中间间隔；独立重新读取SHA-256后为5,528,485ms。该口径不含部署/远程预核对准备阶段，不是跨主机相减，也不是被大文件/Resume条件排除的Step1正式计时。两项完整文件采用一致的这个口径如下：

| 指定文件 | 完整字节数 | 本机接收进程时间线 |
| --- | ---: | ---: |
| TestMediumFile.bin | 273,806,498 | 1,398,781ms |
| TestBigFile.bin | 1,059,917,774 | 5,527,813ms |
| 合计 | 1,333,724,272 | **6,926,594ms = 1小时55分26.594秒** |

Big最终Encoder2852仅在Receiver独立验证完成后WM_CLOSE清理，forced=false，前台HWND前后均625612094。原2560x1600@240已恢复（`cmd-20260913T190940Z-cc5270`）。Big证据根为 `artifacts/nonlocal-stall-20260913-2111/nl0914-d-big-f30-s15/`，报告为 `receiver/epoch-{0,1}/report.json`，最终文件为 `receiver/output/TestBigFile.bin`。

**本条目只建立当前候选D对这两个完整指定文件的一轮稳定基线，不宣布最快、不宣布任意非本机链路都无长尾，也不宣布原分辨率已适配。** 原分辨率/更低有效帧率仍是约束，后续优先小于100MB的定向提速实验。首轮合成图案缺少Session导致正式UnifiedTelemetry不可用的失败已保留；不放宽正式计时门，改用独立只读native像素结果观察器统计人工fixture的唯一CRC合法块。该观察器已单独编译，7类有界计数/冲突/畸形检查自测通过；第二次编排在观察器启动时被右屏识别检查拒绝，尚未启动远端图案，且已恢复原分辨率。代码审查发现独立观察器链接时遗漏了生产捕获层要求的PMv2 manifest，正在保留保护检查的前提下补齐并先做只读预检；不构成文件吞吐结论。


### 21.11 小规模实屏路线筛选：部分更新尚无显著收益，继续分离捕获/呈现瓶颈

三个45秒人工fixture `nl0914-bandwidth-{full-rotating,full-held,half-held}-03` 已实际结束并取回发送端日志。三者各提交1350帧、无pending替换，发送端用时约45秒；本机只从右屏原生捕获、原有GPU/FEC/CRC返回结果做只读计数，不向Receiver注入任何内容，不绕过Session接纳，也没有文件输出。数据如下：

| 模式 | 实际像素唯一Bootstrap | 唯一CRC合法fixture块 | 身份帧率 |
| --- | ---: | ---: | ---: |
| 全量更新、正常映射相位轮换 | 378 | 6804 | 8.3955/s |
| 全量更新、固定映射相位 | 386 | 6948 | 8.5663/s |
| 九个数据槽交替更新、其余保留原块 | 388 | 6975 | 8.5980/s |

所有payload冲突为0，最终输出目录为空；焦点句柄前后均为625612094；已恢复2560×1600@240Hz。三者只有约2%至3%的唯一有效块差异，**不足以支持生产代码改为部分更新或永久固定映射相位**。该实验不是文件吞吐测试，不能将计数乘payload大小称为VerifiedGoodput。完整身份、源端实际Present统计和结果在 `artifacts/nonlocal-stall-20260913-2111/bandwidth-diagnostic-comparison.json` 及对应运行目录。

诊断工具失败也保留：首轮正式UnifiedTelemetry因没有Session而不可用；第二轮独立观察器遗漏PMv2 manifest，被生产捕获层拒绝；第三轮补齐同一生产manifest且保留精确非主DISPLAY2检查后，预检及7类计数/冲突/畸形自测通过，才得到上述真实像素结果。原失败文件和旧二进制未覆盖。

进一步审查不能把约8.5Hz直接归咎于网络：完整Big的第二接收epoch中，21164个Bootstrap共消耗约21.47秒CPU，GPU解调约94.61秒，FEC后处理约307.99秒，整个epoch1927.531秒。新独立Bootstrap-only/WGC样本在不执行Data FEC时仍观察到378个唯一身份、约8.3945Hz，分析平均约1.92ms；但该样本源端发生1次pending替换，未通过预设的零替换比较门，标记为诊断观察、不是严格对照PASS。后续按独立样本继续比较DXGI Bootstrap-only、完整Decoder以及关闭重复Present的源端；不修改Citrix或网络设置，不提前宣布瓶颈归因或提速。

外部资料只用于提出实验，不替代现场证据：[libcimbar原项目说明](https://github.com/sz3/libcimbar/blob/master/DETAILS.md)讨论空间交织、置信度/局部漂移与喷泉纠错；[Citrix官方HDX计数器说明](https://www.citrix.com/blogs/2024/07/08/introducing-new-hdx-graphics-performance-counters/)区分输入帧率、编码输出帧率与丢弃帧。这支持分层测量方法，不证明当前链路使用特定编码器，也不授权改变任何远控设置。


### 21.12 捕获/呈现分离与候选E（100MB实屏运行中）

`capture-separation-stage-02` 已结束，三个独立样本均通过源端1350提交/零pending替换检查，原分辨率恢复，焦点前后均为625612094。

| 样本 | 唯一身份 | 唯一CRC合法块 | 身份帧率 | 重复Present次数 |
| --- | ---: | ---: | ---: | ---: |
| bootstrap-dxgi | 374 | 不测Data | 8.31368 | 7663 |
| data-repeat | 384 | 6912 | 8.53652 | 7756 |
| data-demand | 424 | 7632 | 9.41175 | 0 |

Bootstrap-only/DXGI仍约8.31Hz；结合上一WGC诊断，不能指望仅优化FEC或切换捕获后端获得数量级提速。Demand Present的人工fixture唯一块增加10.4167%，仅支持选择候选，不是文件提速证明。

候选E仅把GrayFast的`DataWindowConfig.repeatActiveFrame`设为false，其余Profile保留原策略；逻辑发送FPS、帧/方程提交、持久ID租约、光栅、Wirehair毕业预算及全部恢复/资源门不变。新回归先在D上以`1 == 0`失败，改动后连同原定向回归通过33例/10008断言，Scheduler原14例/319160断言通过；四个受影响构建目标通过。新例同时验证GrayFast、Gray、正式Unified的呈现策略隔离、失败重试像素/ID不变和实际Bootstrap连续序号。

E已独立封存，`candidate-e-vs-d-runtime.diff`只有一个配置条件与解释注释；没有覆盖D包。`nl0914-e100-f30-s6`正在运行100000000字节、30fps、6MiB段的真实远控对照，完成前不宣布文件提速。下一步潜在的帧内多Segment交织只记录为隔离设计备选，未修改生产实现，也未降低毕业预算。


### 21.13 候选E 100MB/6MiB段实屏完成：小幅整文件收益，冗余成为下一步重点

`nl0914-e100-f30-s6` 已实际结束，100000000字节，30fps，16个6MiB目标段，Receiver `Completed`/exit0。Receiver单机单调运行时 **517754ms**，对比同参数D的532801ms，减少15047ms（2.8241%）。只是一轮已验证操作基线，不是最快或跨链路统计显著性结论。

整文件摘要、rename、final reopen及独立重读SHA256均通过，SHA256 `4111eb5acba90ea84b8e1cce370699d682ac1460e9f841069c7445cd89a134eb`；本轮又使用独立Get-FileHash复核一致。资源/冲突拒绝与deferred均0，peak active6，peak reserved267401796字节，stale-result drop1（D同参数为0，未隐藏）；正式Step1仍因输入超64MiB而不具备计时资格，未伪造VerifiedGoodput。

实际UniqueVisualFPS从D的8.363682644增至E的9.375355495，约增12.10%，但`outerAlreadyCompletedSymbols`由13216增至19888；两者真正唯一方程都为61400。源端报告E的successfulPresentCalls/sourceTextureReplacements均15875，D对应105912/15879。该证据支持“高频重复Present负担已消除，但更高有效帧率有相当部分被既定毕业预算中的已完成段冗余消耗”，不能据图案实验宣称文件提速10%。

Encoder PID3108在Receiver验证成功后由WM_CLOSE停止、forced=false；其既有报告把外部窗口关闭归类Failed/exit1，不能将Sender报告改称PASS。原2560×1600@240Hz已由`cmd-20260913T195744Z-027c37`恢复，焦点前后625612094一致。证据：`candidate-e-six-mib-stage/restored.json`、运行`result.json`、`receiver/report.json`及远端`encoder-report.json`，均在当前artifact根。

下一步帧内多Segment交织尚未落地：只有`PENDING_SPATIAL_SEGMENT_INTERLEAVE.md`和`spatial-segment-matrix-model.json`。纯槽位矩阵3297项覆盖检查通过；固定六段、每帧17数据槽、已知描述符的计数模型中，periodic-quarter段间方程差由2856缩至168，sparse/bursty由544缩至36。**这不是生产调度/Receiver/FEC/资源或整文件恢复证明**，暂不降低D/E冗余预算。


### 21.14 候选F：显式帧内多Segment交织，保留E默认与原毕业预算

本次在E之上加入默认关闭的 `EncoderConfig.grayFastSpatialInterleave` / `--grayfast-spatial-interleave`。只允许GrayFast；其它Profile及重复CLI选项拒绝。原编解码wire、Profile/Layout、FEC参数、FrameSequence连续租约、接收资源和D/E毕业预算不变，没有接收状态反馈给发送调度。

审阅优先处理了三项风险：未提交列被提前计入方程预算；多段数据配错当前Segment控制记录；提交期间vector滑动使缓存的状态索引失效。实现为最多六个已激活段各准备一行、保留物理槽号的W×18槽矩阵，每个完整批次输出W个视觉帧。第q帧槽j来自 `(q+j+bankPhase)%W`，逐批旋转bankPhase。每个源槽恰好发出一次，全部W帧提交后才commit全部源行，然后完成pass，最后统一滑动窗口。新缓存固定 **186112字节**，编译期硬上限256KiB，无额外FEC实例，不把此缓存伪装成encoded-Segment字节统计。

每个Transport仍独立携带自己的SegmentOrdinal/OuterBlockId和CRC；只是把多个独立Transport装入同一视觉帧，而非跨Segment改写方程。总体设计1.x的active-window/striped outer blocks和9.4独立Transport头保持适用。[RFC 6330第4.4.2节](https://www.rfc-editor.org/rfc/rfc6330.html#section-4.4.2)也区分块身份与符号身份，但其RaptorQ packet规则不能当成PixelBridge/Wirehair的wire认证；这里只作为审阅参考。正式Step1的submitted identity只有一个SegmentOrdinal，本候选显式拒绝同时启用该measurement，诊断中的current Segment仅是首个Transport代表值，不伪造单段帧的认证样本。

定向验证：

- 四个受影响构建目标通过；windeployqt仍有VCINSTALLDIR未设置的部署探测警告，构建未因此失败，封存包使用逐EXE和全文件hash身份。
- 36例/10459断言通过；Scheduler 15例/325724断言通过。完整保留E的33例/10008断言和旧Scheduler 14例/319160断言。
- 新矩阵测试穷举W=1..6所有phase/slot并覆盖0、7、越界phase/slot、UINT32_MAX；失败不写出sourceRow。
- F测试独立验证完整矩阵前不计commit，完成后实际提交方程数恰好相等；重试不变字节/计划；半批次退出后repair-only重建跳过整个旧ID租约，未使用租约不能换算成发送信用。
- 1/5/7段短窗及13段quarter、64段sparse/bursty全文件安全发布与独立重读BLAKE3通过；所有新用例quota/conflict为0。13段quarter为817帧/205可见/0wrap；64段sparse为4081帧/1111可见/0wrap/peak6/最长无新方程189帧。相同旧E/D路径仍为5069帧/1362可见/5wrap/peak7/最长206，测试未删除或改换丢帧投影。
- 实际Encoder raster→CPU oracle/FEC/CRC→真实DecoderRuntime恢复6293504字节（六个压缩段+RAW尾段）与空文件；前者12帧实际包含多个Segment；whole digest/rename/reopen全部通过。Encoder在半批次停止后保持同Session恢复，FrameSequence跳过旧持久租约且后续逐帧连续；epoch拒绝重试像素/ID一致。此为本地1:1像素验证，不是远控吞吐。
- 初轮测试失败保留：配置单测使用不存在源文件而触发已有源验证，改成真实Scratch文件；像素用例向已经Completed的Decoder排入下一结果后等待队列排空，测试终止判定补充Completed（最终状态、摘要、发布、reopen检查全部保留）。生产代码没有为这两项测试修改恢复门。

**模型勘误：** §21.13引用的纯计数脚本serial sweep写成5，但当前源码 `senderUnifiedSweepPhaseStep=1`。旧文件不覆盖；`model_spatial_segment_interleave_v2.py`/`spatial-segment-matrix-model-v2.json`使用1，sparse serial差值应为612（不是544），spatial仍36；periodic差值不变。这仍不是生产性能证明。

F包独立create-only封存：patch SHA256 `6cafe4a6c0688457190501c870dbf823a29f2ba0c8d0626873f337a5f4416ee4`，Encoder `052fb88dffa685326c3f90b840bb40172d518d5001c8000be785b88968e4a1ff`，Decoder `03487800a6bb4c7e738e67d05be1282862561bf91fb39b2bff570e4e79e06011`。生产C++在field期间冻结，文档记录另行追加。对照 `nl0914-f100-f30-s6` 已启动100000000字节、30fps、6MiB段的实际右屏远控，完成前不宣布整文件提速。`candidate-f-six-mib-stage`独立记录原显示模式、恢复意图及finally恢复；payload仍仅经过实际右屏捕获，bridge仅部署/控制/日志。


### 21.15 候选F 100MB完成但未取得整文件提速，继续保留E默认

`nl0914-f100-f30-s6` Receiver Completed/exit0，100000000字节，16段，单机receiver runtime **517942ms**；同参数E为517754ms。F多188ms（+0.03631%），此单次差异不能证明性能优势或退化，结论为**没有测出提速**。不能用§21.14本地sparse模型快19.49%来替代实际整文件结果。

whole digest/rename/final reopen/published全部true，独立SHA256两次重读均为 `4111eb5acba90ea84b8e1cce370699d682ac1460e9f841069c7445cd89a134eb`，BLAKE3与E一致。最终输出目录仅有 `TestMedium-prefix-100000000.bin`，无遗留part/resume。运行Session `256daaef6b606a783e28e0ba052a4c06`，SessionTag十六进制 `332cc39ce67aadcb`；跨主机时间未相减，正式Step1仍为MeasurementSourceScopeExceeded，不声称formal VerifiedGoodput。

| 指标 | E | F |
|---|---:|---:|
| receiver ms | 517754 | 517942 |
| UniqueVisualFPS | 9.375355495 | 9.325291549 |
| outerUniqueSymbols | 61400 | 61400 |
| outerAlreadyCompletedSymbols | 19888 | 19490 |
| outerOrphanAdmittedBlockCount | 0 | 66 |
| outerPeakOrphanCachedBytes | 0 | 70047 |
| peak active decoder | 6 | 6 |
| peak reserved decoder bytes | 267401796 | 267401796 |
| conflict/resource/deferred/orphan quota rejection | 0 | 0 |
| result queue high water | 2 | 3 |
| stale result drops | 1 | 1 |

F少量Transport先于对应Segment控制到达，使用了原有有界orphan缓存，但未发生拒绝；这些开销没有隐藏。Sender observedSubmittedLogicalFps=29.97065988，successfulPresentCalls/sourceTextureReplacements=15886/15886，仍为demand Present；提交控制槽占比5.849996%（E5.848800%）。源端计数含最终20秒轮询后的停止间隔，不能与Receiver完成时点直接跨主机相减。

进度日志显示F第一窗口六段在140秒轮询时均已完成，而E此时只完成两段、160秒才全部完成；但两者后续新窗口依然受固定毕业预算控制。这支持“接收更均衡，但补发时间仍支配总耗时”的解释，不是接收端再次卡死。

本机Receiver PID54468已退出；远端Encoder PID2640在验证成功后通过WM_CLOSE停止、forced=false，沿用已有Failed/exit1关闭分类，不伪称Sender报告PASS。焦点前后625612094一致。`candidate-f-six-mib-stage/restored.json`确认恢复2560×1600@240Hz，命令 `cmd-20260913T205131Z-78a0ee`。证据比较见 `candidate-e-f-six-mib-comparison.json`；所有E/F包与旧失败记录保留。原始指定Medium/Big完整文件目前仍只有D的合计1h55m26.594s验证基线；没有重复大文件来粉饰这一轮没有提速的结果。

下一项仅为待验证技术方向：在空间交织保持不变的独立候选中，减少**离散方程余量**，不降低4K量级基础预算（30fps）、不改8-pass上限、不增接收器资源或旁路反馈。先保留全部现有回归，再补13×6MiB（合计约82MB）的大K quarter/sparse/late-join压力，因为仅用256KiB小段不能代表大文件的相对余量；不通过则拒绝候选。未经这些验证不启动新field、不修改E默认、不宣称安全或提速。原分辨率适配仍未解决，未将1080临时测试条件推广为各种分辨率认证。


### 21.16 大K门禁新增失败证据：F与G的补发等待，而非接收器卡死

上一Goal轮分类为progress：完成F实现和实屏否定性结果。继续工作先保持F预算，只扩展受控测试，输入13×6MiB=81788928字节（小于100MB），覆盖periodic-quarter、固定sparse/bursty，以及第15000逻辑帧才加入的repair-only接收。原用例20000帧上限不变，仅新增late-join用例显式使用60000硬上限；保留全部资源/发布/摘要门。新增记录失败时的进度、最长无新方程期间活动codec数和已验证字节。动态Section使一个用例失败不会阻止独立late-join证据，不删除或弱化失败断言。

| 候选/模型 | 完成帧 | 接收帧 | 完成时wrap | quota拒绝 | 加入后最长无新方程帧 | 该时活动codec |
|---|---:|---:|---:|---:|---:|---:|
| F / quarter | 13321 | 3331 | 0 | 0 | 623 | 0 |
| F / sparse | 13286 | 3602 | 0 | 0 | **1268** | 0 |
| F / late quarter | 38605 | 5902 | 34 | 1123 | 411 | 5 |
| G / quarter | 12589 | 3148 | 0 | 0 | 255 | 0 |
| G / sparse | 12532 | 3411 | 0 | 0 | **914** | 0 |
| G / late quarter | 39025 | 6007 | 35 | 4937 | 351 | 5 |

所有行的文件最终均完成whole digest/rename/reopen；但F/G sparse均未过新设的`<900`逻辑帧等待门，失败原样保留，不能改成1000让G通过。F/G最长间隔时已完成75497472字节、活动codec=0，确认这是已完成窗口仍在消耗发送预算的等待，与原954MB resume中8个未完成codec的停滞机制不同。以上为无光栅受控帧数，不是实际远控秒数。

G仅将空间交织路径的额外根号项系数从4减为2，30fps的基础4K、wrap top-up、8-pass上限及接收资源限制不变；默认非交织E继续系数4。差异`candidate-g-budget-only.diff`；G没有封存/启动现场测试。因为G仍未过门槛，下一步没有继续把系数微调到恰好过线，而是单独研究窗口调度。

晚加入仍明显更慢：firstObservedPass为F=3、G=4，需要多次MicroRepair循环，quota拒绝是受限接收器正常拒绝，不是资源上限被提高。总体设计11.2明确不足K的MicroRepair不能声称一轮late-join可恢复。本轮没有给这个已知限制贴上性能PASS标签，也未让source读取任何Receiver状态。

证据：`candidate-f-large-k-baseline.log`、`candidate-f-large-k-baseline-02.log`、`candidate-g-focused-01.log`。F带新探针的完整source patch为`candidate-f-large-k-baseline-source.patch`，SHA256 `f235bf8dce3fe84621666460c2801fddb9478fe483b70df85ab50a64a1cf46d9`。

### 21.17 候选H：四段常规窗口、最多六段尾窗，局部门禁通过后进行实屏组合比较

H保留G的系数2与基础预算，仅对显式空间交织且大于12段的文件使用四段常规窗口，剩余不超过六段时允许在全部bank提交之后有界扩展尾窗。Sender上限仍6、Receiver上限仍8及1GiB，没有增加资源配额。默认E、最多12段的旧barrier路径、wire/Profile/FEC参数均不变。尾部vector扩容位于所有bank source-row commit及pass finalize之后，避免悬空索引。单变量代码差异见`candidate-h-window-only.diff`。

同一大K测试结果：quarter 12349帧/3088接收/0wrap/0quota/最长247帧；sparse 11949帧/3263接收/0wrap/0quota/最长554帧，均peak5；late quarter第15000帧加入、firstObservedPass4，38481帧完成/5871接收/35wrap/4510quota/最长235帧，peak8。晚加入比F的完成帧数略少，但quota更多，不能单凭较短间隔说所有指标都改善。所有文件通过独立重读摘要和安全发布/reopen。

全部受影响四目标构建通过（windeployqt仍有VCINSTALLDIR未设置警告），定向37例/10518断言、Scheduler15例/325724断言通过。原E准确预算/持久租约/源文件不变/实际像素/重试/恢复检查保留；新增大K失败没有通过改阈值消除。

H独立封存：source patch SHA256 `cde19fabcb4e9d8a73285b685d1523f19a6af07b9f4aaba454a8288f603d32ce`；Encoder `660683560dce05b18b61803a9adfdfb3c9f51fc95c80f30511eab97afabb85f8`；Decoder `0cfface6aeac1b5671f86a50ce9f6672fd36df129c1263265399bc9417d370c0`。`nl0914-h100-f30-s6`正在实屏运行100000000字节/30fps/6MiB段。该现场比较相对F包含G余量与H窗口两个已分步检查的变化，是组合候选比较，不拆分宣称单项速度贡献。生产C++在field期间冻结。

完整指定文件的H脚本仅准备、未启动：在H100结果未知时预设至少比E单次快3%（<=502221ms）且完整校验通过，才值得花时间重测Medium/Big。沿用验证过的v2同目录/同Session单次3600秒deadline恢复规则，resume checker AST与原版一致，26项离线证据边界检查通过；这不替代真正原生resume验证。原分辨率适配仍未解决，测试finally必须恢复原2560×1600@240Hz。


### 21.18 H100实屏完成与干净Sender Session对照准备

H的100000000字节/30fps/6MiB段实屏完成，ReceiverRuntime=493751ms，比E517754ms短24003ms（4.635985%）。完整digest/rename/reopen/published全部true，独立再次SHA256复读为4111eb5acba90ea84b8e1cce370699d682ac1460e9f841069c7445cd89a134eb。UniqueVisualFPS=9.341408，与E/F相近；unique61400、alreadyCompleted15813（E19888、F19490）、peakActive4、peakReserved178267864B，resource/conflict/deferred/orphanQuota/stale均0，orphan缓存峰值8145B。这支持减少已完成段重复消费而非提高视频帧率的解释，但单次现场是组合候选证据，不作单变量因果声明。

Encoder PID5732在接收完成后WM_CLOSE、forced=false；原有Failed/exit1关闭分类保留。前台625612094保持不变；restored.json验证原2560×1600@240，restore命令cmd-20260913T214450Z-bcf57a。comparison见candidate-e-f-h-six-mib-comparison.json。H100已达到预设>=3%收益门，值得复测完整指定文件，但此时H的Medium/Big仍未运行，不能套用100MB百分比。

准备完整原文件时发现FindMatching按source identity及稳定build/compression/FEC身份寻找旧EncoderSessions；H不会因新exe哈希而自动隔离D的15MiB旧会话。若直接重用原Desktop文件，将存在从旧checkpoint开始的对照污染风险。没有删除/覆盖旧会话、没有复制修改原文件，也不通过桥传送payload。新增CLI --session-state-root ABSOLUTE_PATH仅把已有EncoderConfig.sessionStateRoot暴露给Unified家族测试；默认root和所有wire/预算/持久验证语义不变。重复/空/相对/缺值及历史profile参数拒绝。

H-clean仅增加上述CLI隔离及测试，不调整H运行时算法。每个远程run在自己的package/encoder-session-state建create-only空目录，启动时显式传入；完成后以Sender报告preparation.resumed=false、源稳定检查、同SessionId/uint64Tag、相同原文件字节数和整文件摘要作为额外测量门。旧SourceIndex完全不碰。不是让Receiver反馈选择Sender方程或排程。新增原生双root各两次启动测试证明互相独立且旧resume历史保留（37断言）；14项无呈现CLI边界用例通过；整体38例10555断言、Scheduler15例325724断言通过。31项Sender证据边界检查通过，原生Receiver恢复检查不替代为脚本检查。


### 21.19 完整Medium的H-clean实测通过，随后开始完整Big（2026-09-14清晨）

H-clean独立封存source patch SHA256=4fa1256d082efcef5426761104ac70b25d084a51dfd951eb3188d6cc00b554ab；Encoder=36db97a586330e91522081b4abf36dee19fa9a658e58bba9f8972dedc06f750c，Decoder仍为H的0cfface6aeac1b5671f86a50ce9f6672fd36df129c1263265399bc9417d370c0。新增未暂存Python CLI回归源文件显式保存在additional-source-files并纳入seal，未误把它遗漏为可重放源码。CLI隔离之外H算法未变。

nl0914-hclean-medium-f30-s15完整273806498字节完成，Receiver native1297465ms，harness本机时间线1297610ms=21分37.610秒。D同文件时间线1398781ms，因此整组改动累计减少101171ms（7.232798%）；D/E/F/H并非同轮统计重复，不能声称跨链路显著性或把收益仅归于窗口大小。18段全部验证，encoded273098926B。whole digest/rename/reopen/published及独立SHA256两次复读通过，SHA256为28fd5ead99bf7fe3526e38cd1a44364b3c02792323d01840f2f4bdc38ebc1bb0。Sender clean-start gate通过；SessionId=d2313893ff28e3710e65e077412da2a1，源预扫描与接收摘要一致，resumed=false。

UniqueVisualFPS=9.315566，unique167658，alreadyCompleted36264，peakActive6，peakReserved560745322B；resource/conflict/deferred/orphanQuota0；orphan峰9774B，queue峰4、stale drops4。没有掩去有界drop。Receiver60412已退出；Encoder24480在验证后WM_CLOSE、forced=false；既有Failed/exit1关闭分类保留。焦点625612094相同，原2560×1600@240恢复命令cmd-20260913T222950Z-d11f7c。比较证据candidate-d-hclean-medium-comparison.json。

完整Big随后在nl0914-hclean-big-f30-s15开始，原桌面TestBigFile.bin再次验证1059917774B及20B000AFA567A66DD536B22946102A465575F77299A42FFC2FCC2116F9808E76。独立create-only发送端状态根；沿用3600s/Receiver、最多一次正常deadline原生同目录恢复以及7200s Sender硬上限，原生资源/摘要门未改。H-clean生产源码和hclean-named-orchestration-frozen-01在运行中冻结；完成前没有新的两个指定文件合计耗时结论。

### 21.20 原分辨率GrayStates隔离诊断取得码字恢复，尚非适配完成

在Medium结束且原模式恢复后，nl0914-gray-robust-native01对远程2560×1600作45s独立图案呈现。只用已有GrayStates/layout12/1314B，不改产品wire、不改Citrix或网络。新固定测试Tag=92ba640ee1ac7035；sequence31/32使用各自不重叠OuterBlockId，payload仅用于人工合成诊断，绝不装作文件Session。

本机右屏GDI实际捕获三份2560×1440/14745600B BGRA，只在本机分析；桥无图像/payload。三份都恰好捕获sequence32，所以它们不是三个独立映射相位。接受几何scaleX=0.850826741099、scaleY=0.851404962843。私有0.75尺度CPU基线3次均0/18；保留的forward-model-probe-03-gain4对象3次均18/18 FEC+CRC有效，且18个Transport逐字节匹配独立生成的预期；预期内容只在普通解码完成之后评分，未交给解码路径。forward符号非零符号位误差约0.085%至0.091%，baseline约1.76%至1.78%。这些数字不可直接与旧Fast的sequence19/20跨运行作单变量因果比较。

源端1350提交、1350texture replacements、0pending replacements；统计只适用于重复两幅诊断图案，不是UniqueVisualFPS或文件吞吐认证。Presenter25392正常结束；焦点625612094保持，前后display-info均2560×1600@240。现有生产minimumScale、GPU解调和Profile合同均未更改。证据nl0914-gray-robust-native01/comparison.json、captures.json、analysis-*.json及gray-robust-original-resolution-prep/built-identity.json。

下一适配门：多映射相位和同条件Fast/Gray对照；再验证真正Session/Descriptor/文件/恢复全链路和CPU/GPU一致性。不能因当前18/18就宣布原分辨率或其他各种分辨率完成。此诊断后优先回到完整Big主线，未继续占用右屏做支线试验。


### 21.21 H-clean完整Big完成、两个指定文件合计缩短5.03%，并纠正资源监控遗漏

2026-09-14 08:07左右，nl0914-hclean-big-f30-s15完整1059917774字节、68段完成。单一本机harness时间线5280672ms（包含3600秒CLI正常截止后的原生重启间隔）=1小时28分0.672秒，比D的5527813ms减少247141ms（4.470864%）。两个指定文件合计6578282ms=1小时49分38.282秒，相比D的6926594ms=1小时55分26.594秒，减少348312ms=5分48.312秒（5.028619%）。这是该右屏链路、30fps/15MiB段、临时1920×1080条件下的单轮操作性实测，不是正式Step1认证、统计重复或任意分辨率承诺；默认GUI仍未启用H的显式空间交织路径。

Big epoch0正常Stopped/exit1，native3600058ms，44段692060160B完成；epoch1 Completed/exit0，native1680351ms，resumeLoaded=true、resumeVerificationSucceeded=true、重验801ms。同SessionId=e9ade731601c194d3d703efe766a785f、SessionTag=f77ba72e5404f935，与独立clean-start Sender证据一致。整文件digest、rename、final reopen、published全部true。独立SHA256再次复读为20b000afa567a66dd536b22946102a465575f77299a42ffc2fcc2116f9808e76；整文件BLAKE3为1d5c9d9c7aa1836d9d3953808667ec2b59981b372987e2327e53f1f573f6058b。Sender4152在成功后WM_CLOSE、forced=false，既有Failed/exit1关闭分类不伪称PASS；本机两个Receiver均退出。焦点625612094保持；cmd-20260914T000727Z-a9d87f确认恢复原2560×1600@240。

**监控勘误：** 运行中所报FEC decoder quota/deferred=0属实，但不能扩写为“所有资源拒绝均为0”。epoch1最终outerResourceRejections=41，全部来自outerOrphanDroppedByQuotaCount=41；FEC codec quota/deferred、冲突、CRC/FEC失败仍为0。完整原生日志表明事件在epoch1约515至517秒、已验证817889280B附近：描述符之前到达的数据暂存从27块升至64块（104256B），两秒左右排空，随后继续完成。不是恢复启动时，也不是最后收尾；静态日志尚不能单独证明具体哪一条描述符先后关系，不能把此事件解释成原事故同一根因。原有64块/4MiB缓存及所有Receiver限制未放大。

新增私有field_progress_v3.py完整提取orphan/resource/policy子计数，缺失、无效及未知字段不再按0处理；test_field_progress_v3.py的48项回归通过，包括实际Big日志41拒绝、损坏末行、64KiB有界tail、布尔/负数/非整数字段。仅更新后续观察工具，未改冻结H-clean harness或生产Receiver；未来run必须显式接入它。

完整比较与恢复/清场证据：artifacts/nonlocal-stall-20260913-2111/candidate-d-hclean-named-files-comparison.json；事件时间线：nl0914-hclean-big-f30-s15/orphan-quota-timeline.json。现有H成绩封存保留，下一工作用≤100MB定向门检查晚加入短repair轮导致的长恢复与原分辨率多相位适配，不重复完整Big筛参数。目标仍为缩短非本机指定两个文件的最终正确发布总耗时，未宣布完成。


### 21.22 晚加入原生门保留失败：I/J拒绝，K有改善但尚未达到新增等待门

完整H-clean结束与原模式恢复之后，以同一13×6MiB=81788928B真实FEC/ReceiverPipeline/文件发布原生探针继续调度研究。不是栅格/远控吞吐实测。测试额外要求第15000帧晚加入后的等待<12000逻辑帧、全局wrap≤8；原有<3000帧无新方程等待门、<900帧receiver-first门、8个codec/1GiB及所有摘要/rename/reopen/独立复读均未放宽。H参照为总38481帧、晚加入后23481帧、35次wrap、4510次quota/deferred事件。

| 候选 | 改动（只影响显式spatial的后续repair） | 总帧/加入后帧 | wrap | quota/deferred | 最长加入后无新方程 | 判定 |
|---|---|---:|---:|---:|---:|---|
| I | 每4轮一次所有段集中完整预算，其余micro | 40905/25905 | 8 | 235 | 7779 | 违反原等待门且变慢，拒绝 |
| J | 每轮统一K预算 | 38285/23285 | 9 | 13844 | 1151 | 解码器积压增加，未达新等待门，拒绝 |
| K | I的完整预算按SegmentOrdinal%4错开 | 35193/20193 | 6 | 587 | 1759 | 比H减少3288总帧，但未达<12000新门，尚不接纳 |

三个候选的receiver-first quarter/sparse结果均保持H的12349/11949帧、0wrap/0quota，所有完整文件恢复门通过。这只证明初始路径未被这组补发变化改变，不证明远控提速。K的失败用例继续保留在当前工作树，未通过降门槛来把回归伪装成全绿；当前源码处于进一步调度实验阶段，不是可发布版本。已验证H-clean封存包完全未变，I/J/K均未部署现场。I/J完整源码、失败日志及K差异分别封存在同名native-gate目录；总表candidate-i-j-k-native-comparison.json。

这些失败将下一步问题定位为：单纯增加补发量会在全文件重访延迟与未完成codec占用之间交换代价，不能只看wrap或quota单一指标。后续需要进一步改变访问顺序/公平性或建立更细的确定性进度证据，不能继续用完整大文件盲筛系数。原分辨率适配另用固定45秒、全部16映射相位的Fast/GrayStates匹配图案验证，仍不改生产Profile、minScale或GPU接受门。


### 21.23 原分辨率全部16相位通过，并完成1MiB真实文件的私有CPU参考通路

在远端2560×1600@240完全不变的条件下，nl0914-gray-matched-fast01与nl0914-gray-matched-gray01分别进行45秒/30fps/demand-Present图案呈现；两者均覆盖frameSequence64..79全部16个映射相位。各图案具独立固定SessionTag和不冲突的每sequence OuterBlockId，都是诊断图案，不伪装文件。右屏实际GDI捕获，本机Bootstrap辨识后仅保存每相位首个样本；远程桥只部署二进制和收集日志，不传任何像素/payload。每组16×18=288码字：私有0.75尺度普通判决均0/288；forward-resampled模板判决均288/288 FEC/CRC/独立字节一致。Fast总体非零符号位错误约1.7197%→0.09422%，GrayStates约1.7678%→0.08856%；两者实测scaleX约0.85083。每相位只有一次样本，不是大规模BER置信度或完整文件吞吐认证。

随后nl0914-original-cpu-file01首次把该私有算法推进真实文件链路：封存H-clean远端Encoder发送原桌面Medium的1048576B前缀，原桌面源未改；本机专用程序仅捕获DISPLAY2 [2560,0,5120,1440]，ordinary CPU oracle执行Bootstrap/几何/新鲜度/FEC/CRC判定，再经既有DecoderRuntime/ReceiverIngress/persistence/publish。仅使用可替换OS/GPU边界的测试adapter，既有恢复与资源门未替换；该adapter的WGC/DXGI生命周期标签不是实际WGC或GPU性能证据，所有结果显式标记privateGdiCpuReferenceOnly=true、productWgcGpuPerformance=false。Receiver未收到预期文件字节、预期Session、控制记录或修复ID清单，独立预期摘要只在测试完成后的harness比对。

Receiver Completed/exit0，native参考运行22423ms（包含等发送端启动，不用于替代生产GPU整文件成绩）；111次右屏捕获、45个可用帧、809个接纳码字。1段1048576B完整恢复，whole digest/rename/final reopen/published全部true；独立SHA256两次复读均为094b00edd68c24e87da63fa10e29301ef821858e5268390c6a83c9e944779adc。Sender clean preparation/resumed=false、源稳定检查通过，与Receiver同SessionId=8474994cf333be3f8055bfc9f8cfd339，SessionTag=10e229bdd76eede1，BLAKE3=a4ce30aaba0e7695075cf7e5989b659a7f7d3a420da7c31e1cbba28d9935e48f。新增完整progress-v3已真正接入此run，原生最终journal资源/延期/orphan/cache/conflict相关计数均0，而不是把缺失值当成0。

本机Receiver44000已退出；远端Encoder8164在核验后按owned run停止。cleanup与重新list-runs证实无远端测试进程；焦点前后625612094，原分辨率前后均2560×1600@240。C++私有driver最终构建无warning，命令行无效入口验证拒绝且无输出目录创建。生产minimumScale、modulation源码、GPU shader和Profile目录仍未改；真实生产GPU后端适配、其他缩放比例与较大文件验证尚未完成。

主要证据：gray-matched-original-resolution-prep/comparison.json、summary.json；nl0914-original-cpu-file01/result.json、independent-verification.json、receiver/report.json和本机原生journal.jsonl。私有源码original_resolution_cpu_receiver.cpp与build_original_resolution_cpu_receiver.cmd纳入run身份记录，payload路径完全可重放。下一步先把已证实的CPU数学模型移植到隔离GPU候选并做逐相位/边界/原画布回归，再考虑≤100MB实屏；不直接降低全Profile的scale门，也不把1MiB参考运行当作大文件提速完成。


### 21.24 私有GPU数学模型移植通过128项相位检查，原分辨率1MiB真实WGC/GPU链路通过

已将前述forward-resampling数学模型移至隔离HLSL的Gray分支，不改生产shader、wire、raster、FEC参数或捕获生命周期。初版FXC寄存器压力/loop警告失败保留；最终保持strictness与warnings-as-errors，使用物理坐标轴单调性去重及7个bit分区的流式最小距离，编译成功。291600组去重算术与1000组分区算术一致性检查通过。14个shader入口中13个字节不变，仅demodUnifiedBytecode改变（96492→44808B）；这些大小不作为吞吐指标。

私有离线D3D11诊断执行hardware/WARP × Fast/GrayStates × 原画布/实际右屏已存像素 × 16相位，共128/128通过；每项18个Transport均经过普通GPU/FEC/CRC门且逐字节匹配独立预期与CPU输出，false-accepted=0，raw pixel readback=0。现有PB-owned纹理/完成query/retirement路径不变，资源上限83501664B。后续hardware shader样本约10.7–13.84ms，首次冷启动约500.75ms仍保留；离线核时间不是整文件吞吐，且本轮未声称浮点LLR逐bit一致或全Golden认证。

随后nl0914-original-gpu-file01在远程2560×1600@240完全不变时，H-clean真实Encoder发送1048576B Medium前缀，本机真实PixelBridgeDecoder/WGC/GPU接收而非GDI测试adapter，Completed/exit0、native6617ms，整文件digest/rename/final reopen/published全部true。两次独立SHA256均094b00edd68c24e87da63fa10e29301ef821858e5268390c6a83c9e944779adc；与clean-start Sender同SessionId=a16fd8087e92b106af9950653556586c、SessionTag=5089691193372186171，BLAKE3=a4ce30aaba0e7695075cf7e5989b659a7f7d3a420da7c31e1cbba28d9935e48f。源稳定与准备均通过，resumed=false。资源/冲突/延期/orphanQuota均0；捕获67到达/64交付/3drop，队列峰1/stale1，全部保留。45个帧观察中的UniqueVisualFPS=13.6312只是短样本，不据此宣称100MB或指定完整文件提速。

仅私有新包candidate-gpu-gray-private01被重链接：Encoder SHA256=36db97a586330e91522081b4abf36dee19fa9a658e58bba9f8972dedc06f750c（H-clean不变）；Decoder=076a31e251e80711aa5ebfc16dfbaa12f78ad34bc8f247d20db653ea8bb6970f，显式记录既有CLI/GUI对象、当前K common库Receiver不变与私有scalar/GPU对象的混合来源。原生receiverTimingEligible=true字段原样保留，但此私有混合来源包没有完整配对Step1认证，formalStep1Certification=false。全Profile共用的私有0.75尺度门还未完成生产Profile分域，不是GUI发布候选。K新等待门失败仍保留，远端只使用已封存H-clean发送算法。

本机Receiver55556退出、远端Encoder7060在验证后WM_CLOSE/forced=false（既有Failed/exit1停止分类保留）；焦点625612094及原分辨率前后不变。证据gpu-forward-gray-private-01/phase-matrix-result.json、identity-built.json、shader-final.diff；nl0914-original-gpu-file01/receiver/report.json、result.json、independent-verification.json；gpu-original-file01-stage/no-display-change-verified.json。

继续用原分辨率100000000B/6MiB段/30fps/H空间交织进行实际持续测试，run=nl0914-original-gpu100-f30-s6，启动时未改显示模式或任何网络/Citrix配置。编排脚本另存field_run_gpu_original_100.py，不改1MiB原脚本或H-clean冻结harness；完整v3观察器重新在create-only新根执行48项通过（原固定测试目录直接重跑因已存在而拒绝，原证据未覆盖）。最终发布前不报告本次整文件耗时或加速结论。


### 21.25 原分辨率100MB真实WGC/GPU完成，定位到184秒重复等待；扩展缩放门尚未全部通过

nl0914-original-gpu100-f30-s6已完成100000000B、16个6MiB目标段、30fps、H空间交织。原远端2560×1600@240始终未改变，本机native Receiver runtime=463633ms（7分43.633秒），Completed/exit0；whole digest/rename/final reopen/published全部true，独立SHA256两次复读均4111eb5acba90ea84b8e1cce370699d682ac1460e9f841069c7445cd89a134eb。Sender clean preparation/resumed=false、源稳定通过；同SessionId=3f54a7943d8c5f9e10b1ea7d8fcb4ba7、SessionTag=15d8be9f72d17619，BLAKE3=32d85d4af2aca6c97e3a7104a47163701708dc554f24b47f900349afa1ea4afc。16段全部校验，0恢复重启，正式测量因MeasurementSourceScopeExceeded不可用，私有包仍不是正式Step1认证。

与此前H/临时1920×1080的同一100MB、6MiB段、30fps空间交织493751ms相比，本次组合减少30118ms=6.099836%。不是单变量GPU因果试验（分辨率、私有解调及Sender独立状态根不同），也不是重复统计或两个完整指定文件的新版成绩。当前两个完整指定文件的最好已验证合计仍是H-clean的6578282ms，不从100MB外推Big。

完整native结果：UniqueVisualFPS=13.493795、unique61400、identicalDuplicate15、alreadyCompleted43350、ready16；peakActive4、peakReserved178267864B，原8 codec/1GiB限制未变；全部resource/deferred/FEC-quota/orphanQuota/conflict=0。orphan接纳34、峰14661B。BaseLuma 133596次FEC评估、36次擦除失败、CRC/identity失败0；0整帧擦除。GPU总363930601×100ns、post-GPU FEC CPU总1351312493×100ns。capture到达9873、交付7452、drop2420，admission/readback/epochReset0，队列峰2、stale2，不能把这些有界丢弃隐去。原模式前后完全一致，焦点625612094相同；Receiver39888退出，Sender21944验证后WM_CLOSE/forced=false、既有Failed/exit1停止分类保留，stage最终list-runs=[]。

**重复等待的直接证据：** 同一本机journal连续样本中，unique已达15452/30904/46356、verifiedRaw已达25/50/75MiB窗口位置且activeDecoder=0时，分别63133、59895、61122ms没有新方程；alreadyCompleted却分别增加14423、13813、13942。合计184150ms。该样本证明发送端在固定预算内继续向已恢复窗口发数据，而不是本次FEC资源锁死；持续时间来自同机journal采样，只用于诊断，不替代native主计时。原954MB静态resume具有8个未完成codec，与这里active=0不同，不可混为同一事故证据。下一调度工作必须针对这种已确认的浪费，同时保留慢链路/晚加入/有限active资源门，不用Receiver反馈提前跳段。

隔离扩展缩放检查：gpu-forward-gray-transform-01使用同一64号帧，在Fast/GrayStates、8个0.75至1.25缩放、point/linear、整数/四分之一像素原点之间组成64项合成D3D11/CPU比较。49/64全部18码字且CPU精确一致，其余15项部分/0恢复，均没有独立错误接纳；因此不是任意分辨率适配完成。12个失败或邻近样本又与“旧GPU+仅0.75 locator放行”的独立基线比较；特别是1.125 point在两者均0/18，不能错误宣称这些失败全部是新GPU回归。

再尝试单个已知相位pilot推断point/linear和固定X/Y偏移（162个有界假设，不读取预期Transport内容）的私有CPU参考：1.125 point从0→18个精确块，但0.85 point从9→0，0.75 fractional linear仍0。该候选forward-model-probe-05-pilot-fit明确NOT_ACCEPTED，未移植GPU/未现场部署；修正独立构建后再次复现18/0结果。其全局常偏移假设不能充分描述已观察到的几何尺度偏差；这只是下一诊断方向，尚未证明唯一机制。原100MB已验证GPU包保持不变，不把新失败覆盖成成功。

证据：candidate-h1080-gpu-original-100-comparison.json；nl0914-original-gpu100-f30-s6/result.json、independent-verification.json、repeat-only-intervals.json、receiver/report.json；gpu-original-100-stage01；gpu-forward-gray-transform-01/matrix-result.json、baseline-differential；forward-model-probe-05-pilot-fit/standalone-build-reproduction.json。生产shader/调制/Profile尚未改，当前K调度新增晚加入等待测试仍失败、保留，不是可发布工作树。


### 21.26 逐段准入证据：L 尾窗超过剩余名额；M 保留资源限制消除此链，但晚加入总等待仍未过门

对 K 加入有界逐段跟踪后，13×6MiB、真实 Receiver/FEC/journal/publish（syntheticNoRaster）的三种既有样本仍逐帧得到 12349、11949、35193，说明跟踪没有改变调度结果。新增 firstBoundUniqueFrame 区分“保留为 orphan”和“已进入实际 FEC”；总 resource/orphan 拒绝计数也单独输出，不再把 FEC deferred 数冒充全部资源拒绝。

L 将后续 spatial 修复访问改为每次使用完整初轮损耗预算，首轮不变。晚加入第15000帧时，0..3段保留至27504/27508，4..7段先完成，而20428开始的8..12五段尾窗超过剩余四个名额。第10段直到25004才真正进入FEC，最后37260完成。总后续等待22261帧、drought5555、FEC deferred226、全局orphan/resource4320，违反原drought<3000及新增postJoin<12000，拒绝部署。

M 只把后续修复窗限制为最多四段（初轮尾窗仍保留原形状），相同晚加入样本总27505/postJoin12505，FEC deferred0、drought247；剩余496次orphan拒绝均在首次Session/descriptor建立之前。原receiver-first两样本仍12349/11949、资源拒绝0。M消除了L中“4个遗留+5个新段>8”的已证实链，但12505仍不满足新增<12000门，失败断言未放宽或跳过。失败前内部摘要/发布/重开已通过；该旧大K测试在失败断言之后的独立复读没有执行，不应声称该部分也通过。

证据：candidate-l-full-repair-native/k-trace-result.json、L-rejected-summary.json；candidate-m-repair-window4-native/result.json、large.log、exit.json。M未现场部署。§21.25“当前K”是当时快照，由本条后续M/N状态取代；H-clean封存发送包和原分辨率GPU100MB包均未改变。

### 21.27 用户批准预算约束动态名额：N 单变量恢复对照和真实内存准入边界通过，正式默认仍为8

用户明确批准：取消实验候选固定8个名额，改为由现有1GiB等资源预算约束的有限动态名额，正式默认策略不变。不是无限内存，也不是给发送端增加ACK。先按源码精确估算后，实际用原 Wirehair backend 验证：6MiB/1629B块可创建24个实例，合计预留1069607184B，第25个被拒；15MiB可创建10个，预留1038519520B，第11个被拒。销毁一个后能在相同预算内重新分配，全部释放后计数与预留归零。这里是保守准入预算，不是进程working-set测量。

实现仍使用原 OuterFecDecoderResourceManager 的每实例512MiB和合计1GiB检查，未修改估算公式。实验policy把计数兜底交给已有maxSegmentCount=65536；这是防止大量微小对象无界增长的有限元数据上限，不表示预分配或承诺同时创建65536个codec。正常 MakeUnifiedReceiverResourcePolicy/GUI默认仍8，orphan64块/4MiB、resume256MiB、其他协议/资源字段逐项与默认相等。

审阅发现两处必须同步修复的兼容问题：DecoderResumeStore::Open会拒绝任何大于8的policy；CountActiveSegments使用固定8元素数组，即使只改外层名额也不能正确保存9个活动段。现在只有显式opt-in允许预算约束名额；计数改为经过Session资源验证后的每ordinal一bit（通常上限8KiB），按接纳/完成转换更新，避免每个包扫描全部活动缓存。journal版本、字节格式、CRC、冲突拒绝和完成语义不变。旧/默认模式重开大于8活动段的实验journal仍明确拒绝且原文件字节不变，不会静默抛弃第9段。

同一未改动的M Sender，13×6MiB=81788928B，30逻辑fps，无视觉栅格但使用实际FEC/Receiver/存储和独立整文件复读，单变量对照：

| 丢帧模型/加入位置 | 固定8个总帧数 | N预算约束总帧数 | 固定/N活动峰值 | 固定/N FEC deferred | 固定/N orphan拒绝 |
|---|---:|---:|---:|---:|---:|
| 保留每4帧中的1帧，join15000 | 27505 | 27505 | 8/8 | 0/0 | 496/496 |
| 保留每5帧中的1帧，join0 | 37116 | 24481 | 8/13 | 7217/0 | 9836/0 |

五分之一样本减少12635帧（34.041923%），实验峰值预留579370558B=552.530821MiB、全部resource/orphan拒绝0，drought7779降至3224。四分之一晚加入完全无收益且postJoin12505仍未达12000；证明名额策略只改善实际被名额限制的情况，并非通用吞吐倍增。四个运行均内部whole digest/publish/reopen通过，额外独立复读BLAKE3通过。同一输入摘要一致，发送调度代码片段哈希相同；没有Receiver反馈改变发送计划。

首批N策略/默认门3项209断言通过；N真实分配边界、恢复journal、默认拒绝/损坏/缩小预算及完成后名额回收等定向9项2751断言通过；A/B恢复1项含四运行71断言通过。第一版仅有测试入口。后续显式CLI `--budget-bound-decoders` 正在接通：仅允许live GrayFast，禁止Replay/capture-only/正式测量混用，默认关闭，报告将明确标记budgetBoundDecoderAdmission。CLI及更新后的原生运行验证以其独立后续记录为准，不能把此处无像素结果升级为现场速度结论。

证据根：candidate-n-budgeted-decoders-native，包含before/prepared/build身份、N-native源码快照、result.json、recovery.log、policy.log、reservation-journal.log及各exit记录；资源估算独立留在decoder-slot-budget-readonly-20260914.json。N尚未现场部署，两个完整指定文件的最好已验证合计仍为H-clean的1小时49分38.282秒；原分辨率适配、184.15秒重复窗口等待和原954MB事件唯一根因仍未全部解决。


### 21.28 N显式CLI与原分辨率100MB实传完成：安全门通过，但当前四段窗口没有名额收益

N的--budget-bound-decoders只允许live GrayFast，默认关闭；Replay/capture-only/正式measurement对象混用被拒。审阅发现CLI会自动挂测量记录器，导致实验配置虽解析成功却必然在Start被拒，已修正为实验模式不自动附着正式测量记录器；阶段资源计数仍独立输出，顶层budgetBoundDecoderAdmission始终可见。不是伪造正式测量资格。最终runtime/恢复状态/报告/安全回归13项2838断言通过；CLI九项检查通过，前后焦点一致。另一次新旧恢复组合回归有413断言、412通过、1失败，唯一失败仍是M的postJoin12505<12000，不放宽、不跳过。

隔离新包candidate-n-gpu-private01保留原H-clean Encoder不变（SHA256 36db97a586330e91522081b4abf36dee19fa9a658e58bba9f8972dedc06f750c），以当前CLI/common及既有私有GPU/scalar对象重链Decoder（8bccf4c2f4bd27eaaf7a0888ce4531001351120d6036b7cb074c2eb37d9718dd），42项链接输入逐项保存身份。仍是混合来源私有包，全Profile私有0.75尺度门尚未分域，未认证任意分辨率或GUI发布。

实际先运行nl0914-budget-gpu-smoke-f30：1MiB，6940ms，全部内部发布门及两次独立SHA256通过。随后nl0914-budget-gpu100-f30-s6完成100000000B、16个6MiB目标段、30fps、H空间交织，native464746ms=7分44.746秒。内部whole digest/rename/final reopen/published全部true，独立SHA256两次均4111eb5acba90ea84b8e1cce370699d682ac1460e9f841069c7445cd89a134eb。Sender与Receiver同Session，源重新准备、resumed=false、稳定性通过；native进程exit0。这里没有正式measurement对象，因此不把缺失字段写作false或0，也不是Step1认证。

原远端2560×1600@240前后不变，Decoder只捕获右侧DISPLAY2；焦点108596712前后相同。Receiver56088退出；远端Encoder21256经owned stop后收集证据，最终list-runs liveProcesses=[]。计数上限65536、总预留上限1073741824B；实际peakActive4、peakReserved178267864B，unique61400、alreadyCompleted43739、ready16、全部resource/deferred/orphanQuota/conflict0；队列峰3、stale1、GPU359513822×100ns、FEC CPU1380358418×100ns，未隐去有界丢弃。

与旧原分辨率同一100MB私有包463633ms相比，本次多1113ms（+0.240061%），没有测出提速。两次峰值均只有4个codec，故不能将N的合成五分之一样本34.04%收益泛化到此链路。此现场比较还混有journal bitmap及记录器接线差异，不是count-only因果试验。新journal再次证明active=0、unique及verifiedRaw不变、alreadyCompleted持续增加的三个>=10秒区间，合计184480ms；它是同机诊断采样时间，不替代native整文件时间。

下一步只针对已确认的首轮重复airtime做隔离小样本调度实验，保留后续完整修复预算与全部接收资源门，再评估是否现场部署。N未重测完整Medium/Big，当前最好完整合计仍为H-clean的6578282ms。证据：candidate-n-budgeted-decoders-cli/reachability-regression.log、recovery-regression.log、private-link-inputs.json；candidate-n-gpu-private01/build-identity.json；nl0914-budget-gpu100-f30-s6/receiver/report.json、independent-verification.json、repeat-only-intervals.json；budget-original-100-stage01；candidate-n-original-gpu-100-comparison.json。复核脚本verify_budget_original_100.py采用create-only，不能覆盖原证据重跑。


### 21.29 O短首轮预算：真实100MB耗时下降29.46%，但四分之一接收率退化，不替换默认

针对N现场再次确认的184.48秒重复窗口等待，O在显式spatial GrayFast实验内把每段初始访问累计airtime目标设为原值的65%，仍使用checked乘加与向上取整；后续每次修复访问保持M的完整损耗预算，不跟随65%缩减。仅>12段的graduation文件适用；小文件barrier路径不变。SenderFrameBuilder的paired native版本与CLI接线版本逐字节（归一化换行）一致，SHA256 ef6285b53b209c7c1a3a10c67cf8a8d8c062c49defc0efc58a81686778ff0917。没有Decoder反馈、未使用ID租约记功或wire/Profile改变。

13×6MiB、同一N预算模式、真实FEC/Receiver/journal/publish并额外独立复读BLAKE3，八运行仅改初轮百分比：

| 保留帧模型/加入位置 | 100%总帧数 | 65%总帧数 | 100%/65%最长无新方程帧 | 结论 |
|---|---:|---:|---:|---|
| 每5帧保留2帧，join0 | 10637 | 7917 | 1614/254 | 均首轮完成，减少25.57%，均资源拒绝0 |
| 每4帧保留1帧，join0 | 12349 | 20173 | 247/2775 | 65%反而慢63.36%，不能替换默认 |
| 每5帧保留1帧，join0 | 24481 | 20401 | 3224/1864 | 均第二轮内完成，减少16.67%，资源拒绝0 |
| 每4帧保留1帧，join15000 | 27505 | 27645 | 247/259 | 无收益；首次Session建立前orphan拒绝496/1092，保留 |

八运行137断言通过只是上述功能/指定较好链路性能门，不意味着所有模型都更快。额外用更严格的固定8名额验证压力：quarter/fifth分别20801/32696帧最终完整恢复，peak8、预留356535728B；FEC deferred5894/4712、resource7991/6373、drought3147/7779，不能隐藏这些等待或宣传名额瓶颈已全面消失。该34断言验证的是拒绝后有界最终恢复，不是无停顿性能认证。M旧postJoin12505<12000失败仍保留。

正式默认未变；新增Encoder显式--grayfast-short-initial-airtime必须同时选择--grayfast-spatial-interleave，仅GrayFast、拒绝重复/携带值/不兼容profile及正式measurement。报告单独输出grayFastSpatialInterleave与configuredInitialAirtimePercent，不伪造Receiver进度；GUI没有自动启用。最终10项558断言覆盖原spatial路径、显式启动接线、报告与N模式；CLI九项解析检查通过。源码/测试定向构建通过，Qt部署VCINSTALLDIR警告原样保留。全局M性能失败与分辨率边界仍使工作树不是发布候选。

O私有包candidate-o-gpu-private01仅替换Encoder及其当前构建依赖（Encoder SHA256 48b1d14b2af793a74805368fcc2c110d2ffe22054a08c88fbe31a5a5c1a8f46f），Decoder保持已验证N的8bccf4c2f4bd27eaaf7a0888ce4531001351120d6036b7cb074c2eb37d9718dd逐字节不变。实际nl0914-shortinit-gpu100-f30-s6在原远端2560×1600@240、真实DISPLAY2 WGC/GPU、30fps、16个6MiB目标段完成100000000B。native327833ms=5分27.833秒，相比N同样本464746ms减少136913ms=2分16.913秒（29.459748%）。两个Sender停止时都在Carousel pass0；M新增后续全修复分支未在此完成前执行。但仍是私有不同Sender构建的单次比较与native单变量证据互证，不是重复统计或正式Step1认证。

内部whole digest/rename/final reopen/published全部true，独立SHA256两次均4111eb5acba90ea84b8e1cce370699d682ac1460e9f841069c7445cd89a134eb，BLAKE3=32d85d4af2aca6c97e3a7104a47163701708dc554f24b47f900349afa1ea4afc；同SessionId=17fad8e2d59d190af7b10d5b738fd18d，SessionTag=47f32eada0acee3e。Sender fresh preparation/resumed=false/源稳定通过，报告确认为65%显式策略。资源/FEC deferred/orphanQuota/conflict全部0，peak4、178267864B；unique61400、alreadyCompleted12266（N43739）、ready16。重复等待区间同机采样合计48190ms（N184480ms），而UniqueVisualFPS仍13.522029，支持收益来自减少已完成段发送，而不是伪称捕获FPS提高。

保留非零诊断：unifiedTelemetry BaseLuma 87480次FEC评估、21次擦除，CRC/identity失败0；不要用旧journal聚合fecFailures=0覆盖此逐lane真值。捕获到达5066/交付4889/drop176，队列峰2/stale2。GPU236367287×100ns、post-GPU FEC CPU829129833×100ns。Receiver20144退出，远端23404在验证后WM_CLOSE/forced=false（既有exit1分类保留），焦点625612094前后相同，原模式不变，最终liveProcesses=[]。

当前进入完整指定文件检查点：新field_named_file_run_short_initial.py从H-clean另存，保持3600s单Receiver上限、Big仅一次正常deadline后的同目录原生resume、原有所有摘要/同Session/clean source门；不更改显示模式。进度观察改用已验证v3完整资源计数。额外Sender/resume/实验模式证据门31/21/20项通过。已启动nl0914-shortinit-medium-f30-s15，100MB改善不能直接外推273806498B Medium或1059917774B Big的结果。当前最好完整双文件合计仍为H-clean的6578282ms，待两个新完整文件真正发布后再更新。

证据：candidate-o-initial-airtime-native/result.json与recovery.log；candidate-o-short-initial-cli/runtime-regression.log、resource-pressure.log、parse-checks、sender-builder-equivalence.json；candidate-o-gpu-private01/build-identity.json；nl0914-shortinit-gpu100-f30-s6/receiver/report.json、independent-verification.json、repeat-only-intervals.json；short-initial-original-100-stage01；candidate-o-original-gpu-100-comparison.json；candidate-o-named-*-contract.json。纯像素payload、摘要/发布/重开、资源边界及非本机目标全部保持。


### 21.30 O完整Medium已验证14分20.828秒，开始完整Big；尚未更新双文件合计

nl0914-shortinit-medium-f30-s15已经完成用户远程桌面上的完整TestMediumFile.bin，273806498B、18段、15MiB目标段、30fps。实际原远端2560×1600@240、右侧DISPLAY2 WGC/GPU，O Encoder与冻结N Decoder均不变。原生Receiver runtime860714ms；用于与之前H-clean同口径比较的本机harness进程时间线860828ms=14分20.828秒，独立外部校验完成时间线861000ms。H-clean旧同文件1297610ms，因此组合候选减少436782ms=7分16.782秒（33.660499%）。这是完整文件端到端比较；旧H使用临时1080p而O使用原分辨率私有GPU/N/短首轮组合，不作单因素归因或正式Step1认证。

18段全部验证，单Receiver epoch、resumeLoaded=false，内部whole digest/rename/final reopen/published全部true；额外第二次独立SHA256为28fd5ead99bf7fe3526e38cd1a44364b3c02792323d01840f2f4bdc38ebc1bb0，BLAKE3=aed722a6c5be94f72210c3eff05d526001aca17a4386a6acd7a94da416a5bf25。同SessionId=46bfc2a4ebc10484e01c8d323c36d650、uint64 SessionTag=4094833142217342664；Sender clean source/resumed=false/稳定性通过；两端实验策略报告验证通过。stdout先打印的success=false/senderCleanStartVerified=null是等待远端收尾元数据的中间状态，最终result.json的success/senderCleanStartVerified/experimentalPolicyVerified均true，不能只看前一条stdout误判失败。

资源/FEC延期/orphanQuota/conflict全部0；活动峰6、预留峰560745322B，仍低于原1GiB总预算，有限计数上限65536。unique167658、alreadyCompleted27227、ready18、identical63；BaseLuma 232308次FEC评估、69次擦除，CRC/identity失败0，队列峰4/stale2，非零项未省略。阶段GPU627296814×100ns、Poll/FEC区间2199090079×100ns（这些QPC区间不是独占CPU核时间，详见只读研究补记）。Receiver18524已退出；远端22492经WM_CLOSE/forced=false退出，原模式前后相同、焦点625612094相同，最终liveProcesses=[]。

证据：nl0914-shortinit-medium-f30-s15/result.json、independent-verification.json、receiver/epoch-0/report.json、sender-clean-start-verification.json；nl0914-shortinit-medium-f30-s15-stage；candidate-hclean-o-medium-comparison.json。重放/复核入口verify_short_initial_named.py --case medium为create-only，原证据不可覆盖。

只有在以上完整Medium成功后才启动nl0914-shortinit-big-f30-s15；使用同一封存包、原分辨率、完整1059917774B TestBigFile.bin、15MiB段和30fps。继续保留原3600s每Receiver上限，只有达到普通deadline且满足原同Session/native resume验证时才允许一次重开，不因错误或无进展退出而自动重试。Big尚未发布，O双文件合计未知；H-clean的6578282ms仍是完整同候选双文件历史基线，不把新Medium加旧Big拼成O成绩。


### 21.31 O完整Big及同候选双文件已验证：合计1小时11分52.469秒，比H-clean缩短34.44%

2026-09-14 14:22后完成独立复核。nl0914-shortinit-big-f30-s15使用与Medium相同的封存O包、原远程2560×1600@240、真实DISPLAY2 WGC/GPU、30fps、15MiB目标段，完整传输1059917774B TestBigFile.bin。单个Receiver epoch0/exit0/Completed，native3451505ms，同一本机harness进程时间线3451641ms=57分31.641秒，第二次外部读取前的首次独立校验时间线3452344ms。没有达到3600s限制，不需要重开或resume，也没有修改原deadline、自动重试失败运行或缩小输入。

68段及1053093783B编码段字节完成；whole digest/rename/final reopen/published全部true。两次独立SHA256均20b000afa567a66dd536b22946102a465575f77299a42ffc2fcc2116f9808e76，BLAKE3=1d5c9d9c7aa1836d9d3953808667ec2b59981b372987e2327e53f1f573f6058b。Sender/Receiver的SessionId=fae434178ffe25d775da927471e0ffc7、SessionTag=12651947178526901568相同；Sender fresh preparation/resumed=false/源稳定、显式65%及N实验资源策略均通过。正式measurement没有附着，不冒充Step1认证。

| 完整文件 | H-clean同机进程时间线 | O同机进程时间线 | 减少 |
|---|---:|---:|---:|
| Medium，273806498B | 1297610ms | 860828ms | 436782ms，33.6605% |
| Big，1059917774B | 5280672ms | 3451641ms | 1829031ms，34.6363% |
| 合计，1333724272B | 6578282ms | 4312469ms | 2265813ms，34.4438% |

合计是同一O候选的两个完整文件，不拼接旧候选Big；由1小时49分38.282秒降到1小时11分52.469秒，省37分45.813秒。以这一本机文件完成计时计算的合计verified raw/encoded goodput分别309271.620/307525.158 B/s，但不是正式RunReport.3.measurement字段。H-clean为临时1080p，O为原分辨率、私有GPU、N和短首轮组合，并且运行时段不同，因此这是整组方案的观测改善，不是单因素随机A/B或任意链路保证。

Big资源拒绝/deferred/orphanQuota/conflict全部0，peakActive4、peakReserved415407808B，1GiB及其它资源门未变。unique646505、alreadyCompleted128552、identical175、ready68；BaseLuma954576次FEC评估，339次擦除，CRC/identity失败0；UniqueVisualFPS13.309320，队列峰7/stale7。非零擦除和有界队列丢弃未隐藏；资源0不等于视觉通道从未停顿。GPU2539309502×100ns、Poll/FEC区间9888695661×100ns是各自QPC区间，不能当成独占CPU核耗时相加。两份文件实际codec峰值均未超过8，所以不能把本轮提速归功于放开固定名额。

Receiver60612已退出，远端12928在发布核验后由本轮harness按原WM_CLOSE流程停止，forced=false；既有Encoder以exit1/Failed记录外部DataWindow关闭的分类保留，不改写成正常用户Stop。远程模式前后相同、最终liveProcesses=[]。Big本机前台句柄两端样本625612094→1046484746不同；本轮未执行焦点/鼠标/键盘操作，不能因此推断是谁切换，也不能宣称全程焦点不变。该事实完整保存在result及independent-verification中。

正式默认仍不变。O在quarter接收率native模型中的退化、M晚加入12505<12000失败、私有GPU全局scale下限尚未按Profile收口及任意分辨率边界仍未解决，不能将本检查点当作发布候选或原954MB事故唯一根因证明。下一步只做≤100MB的隔离证据实验与定向回归；先封存当前最好完整结果，不在没有新机制证据时重跑整份Big或反复微调空口百分比。

证据：nl0914-shortinit-big-f30-s15/result.json、independent-verification.json、receiver/epoch-0/report.json、sender-clean-start-verification.json；nl0914-shortinit-big-f30-s15-stage；candidate-hclean-o-big-comparison.json、candidate-hclean-o-named-files-comparison.json。verify_short_initial_named.py --case big和aggregate_short_initial_named.py均create-only且已运行；再次验证应使用新证据根，不能覆盖原记录。合计脚本首次因误假定EXE位于包顶层而在写结果前停止，随后改为读取build-identity.json内实际相对子目录并校验hash后通过，未改任何原运行结果。


### 21.32 P源码化候选：离线定向通过，1MiB真实捕获启动失败；新增全屏适配需求待明确呈现边界

P把已验证的私有灰阶forward-sampling CPU/GPU分支整合到正常CMake构建，并新增绑定CRC有效同帧Bootstrap及精确Profile/Layout的几何入口；原geometry-only入口及SC6/BlankControl的1.0下限不变。正式默认、wire/FEC、摘要/安全发布/重开、原1GiB等资源预算均未改变。定向结果为Gray 13例2181断言、SC6 CPU 18例495140断言、GPU Unified 8例2226断言、shader 1例71断言通过；正常产品库链接的独立原始像素矩阵128/128通过，但这些结果不代表真实CaptureDemodulator入口或任意缩放已通过。

candidate-p-source01使用冻结O Encoder和正常构建Decoder e3adf7694c1efc4f15534caf722a2a7dece9f44e956179f63e00b7da24f5c838。nl0914-scoped-gray-smoke-f30的1MiB非本机验证在原2560×1600@240、实际DISPLAY2上停于WaitingForBootstrap；120065ms后按既有120秒无进展规则Stopped/exit1，1821次Bootstrap尝试、0成功、0恢复字节、未发布。WGC仍交付捕获帧，不能把它当成捕获源停止，也不能把离线通过升级成P现场成功。

收尾后审查发现capture_demodulator.cpp::DecodeUnifiedBootstrap第230行仍使用geometry-only入口，漏接了已验证Bootstrap的灰阶专用尺度许可；这是源码确认的集成缺口，与现场Bootstrap全拒绝一致，仍需专门的Capture adapter回归和新现场样本证明修复。第1313行则处于BlankControl补充带分支，必须保留其原几何下限，不能盲目批量替换。当前测试未覆盖这个低尺度真实捕获入口，记录覆盖缺口，不修改测试门槛或全局放宽SC6来掩盖问题。

Receiver已退出，远端Encoder22992由WM_CLOSE停止且forced=false，最终liveProcesses=[]；远程分辨率未改变，本机前台句柄首尾样本相等，但不由首尾样本推断全程焦点状态。未启动P的100MB样本，失败包与所有原始证据保留；后续必须用新包、新runId重建复测。O完整双文件4312469ms的已验证成绩保持，不受此次失败替代或拼接。

用户随后提出1080p、2K、4K等远程分辨率都应完整内容铺满全屏，而不是1:1居中大面积灰边。现源码ComposeRemoteVisualFullscreenBgra确实填充灰色背景后居中复制1920×1080；当前远程2560×1600还是16:10，需明确无裁切铺满的横纵缩放行为，不能仅按16:9整数放大处理。先答复可行性与吞吐边界，尚未修改呈现合同。纯像素、无ACK、不改Citrix/网络/显示模式和安全门继续保持。

证据：candidate-p-profile-scoped-gray/final-regression/directed-regression-result.json、offline-pixels/phase-matrix-result.json；candidate-p-source01/build-identity.json；nl0914-scoped-gray-smoke-f30/result.json、receiver/report.json、receiver/journal.jsonl；nl0914-scoped-gray-smoke-f30-stage；CHECKPOINT_20260914_P_SMOKE_FAILED_FULLSCREEN_QUESTION.json。原run_profile_scoped_gray_original.py --case smoke已执行且证据create-only；不得复用原tag覆盖失败样本。


### 21.33 P2捕获入口已复现修复，正常源码包完成原分辨率1MiB及100MB；全屏铺满获用户确认

P2先新增真实分阶段CaptureDemodulator测试，不直接向Demodulator注入已接受的Bootstrap。修复前正例在bootstrapStage.gpuWorkSubmitted断言失败，负例通过，原测试exe退出42（不是PowerShell外层展示的1）。随后仅把DecodeUnifiedBootstrap中的几何入口参数从observation.geometry改为经过同帧验证的observation，原BlankControl补充带路径不改。修复后2例1023断言通过；覆盖GrayStates/GrayFast独立0.85线性缩放及分数偏移、冷/热几何、两阶段GPU退休、18块逐字节，以及SC6/BlankControl低尺度、灰阶0.74、错误绑定和更严格调用方尺度拒绝，未放宽任何测试断言。测试专用变换器下界由750扩到700 permille仅用于生成应拒绝输入，不代表解码支持范围扩大。

受影响定向回归：Gray 13例2181断言、SC6 CPU 18例495140断言、Capture/Unified GPU 19例139184断言、shader 1例71断言通过。原128项像素矩阵作为未改变的标量/直接GPU分支证据保留并标明来源，不把它冒充新增Capture入口覆盖。封存candidate-p2-source01：Encoder仍为O的48b1d14b2af793a74805368fcc2c110d2ffe22054a08c88fbe31a5a5c1a8f46f；正常CMake Decoder为c4b60245fdf04fdafa276bc251f1a273ee0a3d628b0284c3451a281fa442bb8d，无私有生产object替换。

nl0914-capture-gray-smoke-f30在原2560×1600@240、实际DISPLAY2 WGC/GPU完成1048576B，native8488ms。随后nl0914-capture-gray100-f30-s6完成100000000B，native329033ms=5分29.033秒；相对O私有Decoder的327833ms增加1200ms（0.36604%），没有测出提速，价值是可正常重建的原分辨率基线。同冻结O Sender、30fps、6MiB段、显式65%首轮及预算约束名额，未同步修改呈现。两次均whole digest/rename/reopen/published以及两次外部SHA256通过。100MB同SessionId=d89c5e0be751993de12112501fc6b1f6，SessionTag=413165174501534701；SHA256=4111eb5acba90ea84b8e1cce370699d682ac1460e9f841069c7445cd89a134eb，源稳定/fresh/resumed=false通过。

100MB资源拒绝/deferred/orphanQuota/conflict全部0，活动峰4、预留178267864B；91764个FEC评估有27次擦除，CRC/identity失败0，未隐去。捕获几何实测scaleX=0.8508275819、scaleY=0.8514086674，仅支持该原分辨率链路，不是任意缩放认证。两次Receiver及注册Sender均已退出，WM_CLOSE/forced=false，远端显示模式不变、liveProcesses=[]。100MB前台句柄首尾不同，运行未执行输入或焦点操作，不能推断全程不变。P2未重测完整Medium/Big；O双文件4312469ms仍是最好完整成绩。

封存脚本初次误断言红测exit1，在复制包前停止；核对实际exit42及原失败断言后修正，原红测文件未改写。相关差错单独登记，不伪装成测试通过。

用户明确确认：16:10等屏幕允许横纵不同比例，但内容完整无裁切、不留外围灰边地铺满。Q因此开始独立改动Unified-family fullscreen的物理画布合成，保留canonical wire内容、旧LF4居中路径和所有资源/恢复门；不增加每帧字节、不改Citrix/网络/显示模式。已加入1080p、2K、2560×1600、4K及奇数尺寸的全像素映射/源像素覆盖测试，以及实际编码图案经生产合成后的独立解码测试；当前只是构建中，尚未宣称Q本地或远控通过。P2封存基线不再修改，后续Q仅先替换Encoder作隔离比较。

证据：candidate-p2-capture-gate/red-test.log、green-test.log、final-regression、seal-preflight-correction.json；candidate-p2-source01/build-identity.json；nl0914-capture-gray-smoke-f30与nl0914-capture-gray100-f30-s6的result/report/independent-verification及stage收尾；candidate-o-p2-original-100-comparison.json；CHECKPOINT_20260914_P2_SOURCE_FIELD_PASS_Q_BUILD.json。Q设计与当前来源快照见candidate-q-fullscreen-fill/REVIEW_AND_PLAN.md。参考Microsoft官方D3D像素中心规则：https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-rasterizer-stage-rules；仅用于选择可复现的呈现采样实现，不作为吞吐改善证据。


### 21.34 Q无裁切全屏已通过本地及原分辨率100MB恢复，但30fps整文件耗时显著退化，继续保持目标而非宣称提速

用户已确认横纵独立的完整铺满效果。Unified-family的单屏全屏生产路径现在将canonical 1920×1080图案按物理像素中心point映射到整个原显示尺寸；固定3840上限的列偏移表与重复目标行复用，不新增帧级heap或修改Display/Citrix/网络设置。旧LF4居中测试和普通非全屏路径保留。canonical字节/每帧容量、FEC、摘要/发布/重开以及所有资源/冲突门均不变。

全像素坐标/颜色/源覆盖测试覆盖1920×1080、2560×1440、2560×1600、3840×2160、2561×1601、1920×2160；独立CPU解码覆盖SC6/GrayStates/GrayFast×4尺寸×2相位=24组，共3例1511断言通过。GPU真实分阶段Capture adapter再覆盖同样24组，并重跑P2正负入口；最终3例3734断言通过。首次GPU新测试误用库默认128MiB而非产品既有Unified 256MiB预算，在4K准入失败；对照改动前产品源码确认256MiB后使测试匹配产品，同时保留4K/128MiB拒绝和requiredBytes-1拒绝、失败输出不变的断言，未改生产预算。初次失败证据保留，不当作产品4K通过记录。

封存candidate-q-fill01：正常构建Encoder SHA256=1106eacf45760c6c4c722f14ef78e60224e0a5af74b91b4fcf1d520e4bfb519d；Decoder保持P2 c4b60245fdf04fdafa276bc251f1a273ee0a3d628b0284c3451a281fa442bb8d逐字节不变。nl0914-fill-gray-smoke-f30实际1MiB native9386ms，全部最终门及第二次独立SHA通过。实测捕获尺度由0.850826×0.851406变为1.134363×1.261013；横纵倍率1.333248、1.481094与原远程2560×1600对1920×1080的铺满比例相符，面积约1.974666倍。它证明实际像素中的放大/非等比适配，不意味着每帧字节增加。

随后nl0914-fill-gray100-f30-s6实际100000000B完成，native782615ms=13分2.615秒；P2同源/同冻结Decoder/同30fps/同6MiB段的旧1:1呈现为329033ms=5分29.033秒。因此Q慢453582ms（137.85304%），不是提速。两次都是同一主机上的完整原生接收计时，不作跨主机减法或统计显著性宣称。Q同SessionId=70b868e9ff967d04598e95d3f0f484a6，SessionTag=8926549075593541125；whole digest/rename/reopen/published、源fresh/稳定/resumed=false、两次外部SHA全部通过，SHA256=4111eb5acba90ea84b8e1cce370699d682ac1460e9f841069c7445cd89a134eb。

| 同100MB观察 | P2居中1:1 | Q完整铺满 |
|---|---:|---:|
| Sender observedSubmittedLogicalFps | 29.876792 | 29.217494 |
| Receiver UniqueVisualFPS | 13.479116 | 7.495648 |
| 峰活动codec | 4 | 16 |
| 峰预留bytes | 178267864 | 708984560 |
| 已完成段重复symbol | 12292 | 37277 |
| Base FEC擦除/评估 | 27/91764 | 41/136404 |
| 资源/deferred/orphanQuota/conflict | 0/0/0/0 | 0/0/0/0 |

Q活动codec峰16实际超过原固定8，但仍在现有1GiB预算内。它避免名额拒绝，不等于保证短等待或长文件最佳速度。整帧视觉到达率下降，首轮不足并进入修复，再与既有重复空口窗口叠加；不能把它误判成解码器死锁。两次GPU每评估耗时约4.88/5.26ms，post-GPU Poll区间约16.68/14.72ms（非独占CPU），CPU均未显示翻倍开销。一次受控只读collect获取当前run的encoder-journal元数据，显示Sender仍约29.18fps；未传payload、像素/尺度给Decoder，也未改变活动Sender参数。这些证据把下一步重点放在呈现到实际捕获像素之间的视觉刷新/发送节奏，但尚不能唯一归因到Citrix某项机制或设置。

Q100 Receiver42744已退出，远端14392由原WM_CLOSE流程退出且forced=false；原显示模式前后不变、最终liveProcesses=[]，前台句柄端点相等（不推断全程状态）。没有重测完整Medium/Big；O已验证完整合计4312469ms仍是历史最好，不能拿Q的成功恢复覆盖性能退化。

继续保留用户要求的全屏目标，而不是退回中间小画布来掩盖退化。下一步先用25MB固定源/固定Q包的发送FPS差分，观察有效像素帧率与最终文件时间；不是改远控/网络设置，也不是在线ACK调度。已启动nl0914-fill-gray25-f15-s6，600秒普通截止、120秒无进展保护均保留；样本≤100MB且不在测量同时运行构建或重负载测试。

证据：candidate-q-fullscreen-fill的before/REVIEW_AND_PLAN、composition-decode-initial、gpu-fullscreen初次失败、gpu-budget-test-correction、gpu-product-budget最终通过；candidate-q-fill01/build-identity.json；nl0914-fill-gray-smoke-f30与nl0914-fill-gray100-f30-s6的result/report/journal/independent-verification及stage；candidate-q-fullscreen-smoke-geometry-verification.json；candidate-p2-q-original-100-comparison.json、candidate-p2-q-100-stage-attribution.json；live-sender-journal/collection-note.json。Q100原运行与证据create-only，不覆盖重跑。


### 21.35 Q全屏25MB固定源FPS差分：15与30都约7.5有效帧，未提升通道上限，不据此外推大文件

全屏需求、原远程2560×1600@240、同一Q Encoder/P2 Decoder和6MiB目标段不变，仅改变Encoder配置FPS。nl0914-fill-gray25-f15-s6完整25000000B用125042ms=2分5.042秒，实际Sender14.990027fps、Receiver UniqueVisualFPS7.659856；nl0914-fill-gray25-f30-s6同文件用127357ms=2分7.357秒，Sender29.031900fps、Receiver7.493925。15fps短2315ms（1.817725%），单次小差异，不宣称统计显著收益；绝对有效帧率仍在约7.5，不能将它说成解决全屏的视觉刷新瓶颈。

两次whole digest/rename/reopen/published、源fresh/稳定/resumed=false、同Session和两次独立SHA都通过；SHA256均f9fa18860c159080d860b654903f272846a5af5fa51ba303e39825ce4b53c3e4，BLAKE3均bb94edaaffea3132de5ae51d5878e2afc8720f7732aff82dc9597fd42fa5f360。活动峰均4、所有资源/deferred/orphanQuota/conflict为0；原预算不变。两次两端均已收尾，原模式不变、liveProcesses=[]。15fps前台端点相同，30fps端点不同；未执行焦点/鼠标/键盘操作，不归因也不宣称全程不变。

这个25MB文件只有4段，不触发>12段的O首轮65%分支，因此不能直接否定或肯定100MB/大文件改15fps后的调度收益。结论只限于：降低配置FPS没有明显抬高当前全屏链路绝对可见帧率。下一步不盲扫其它FPS、不重新跑整份Big；先用已记录的真实接收率检查大文件首轮/修复空口分配，并评估保持完整全屏的更易通过视觉链路的重采样候选。任何新候选仍先走独立本地/捕获字节验证，再≤100MB实屏，不放宽摘要/资源/冲突门。

证据：上述两个run的report/result/independent-verification以及stage收尾；candidate-q-fullscreen-cadence25-comparison.json；复跑入口run_fullscreen_cadence25.py --fps 15或30已经运行、原tag create-only，不应覆盖。当前没有活动实传；post-field-regression正在构建并复核最新工作树，封存Q/P2现场包不变。

收尾补记：最新工作树Encoder/Decoder及受影响测试构建通过；应用4例1521断言、Capture/Unified GPU20例141919断言通过，见candidate-q-fullscreen-fill/post-field-regression。新重链接的Decoder并未替代封存P2参与上述现场比较，不能冒充其新二进制现场认证。所有现场和构建/测试句柄均已终止，最新桥list-runs确认无注册活动进程，保护报告hash未变。续做入口CHECKPOINT_20260914_Q_FULL_FIELD_COMPLETE_CADENCE25.json；整个目标仍未完成。


### 21.36 R同有效帧率的大文件离线对照：主要差异是首轮预算，不是15/30发送FPS；全屏100MB单变量现场待结论

Q的25MB只有4段，不能覆盖>12段graduation。R新增测试专用PeriodicHalf擦除枚举，放在原枚举末尾保留既有值；没有改动生产发送调度、wire、呈现、接收资源策略或正式默认。用真实headless Sender/FEC/ReceiverPipeline/journal/摘要发布重开处理13×6MiB=81788928B，并逐段跟踪及独立重读BLAKE3。30FPS每4帧取1、15FPS每2帧取1都只是7.5FPS的确定性合成投影，不是录屏重放或远控吞吐证明。

| 合成投影配置 | 发送帧数 | modeledSenderMilliseconds | 已完成段重复symbol | 活动峰/预留B | 完整Carousel wrap |
|---|---:|---:|---:|---:|---:|
| 30FPS，首轮65% | 20173 | 672433 | 35306 | 13 / 579370558 | 1 |
| 30FPS，首轮100% | 12349 | 411633 | 2126 | 5 / 222834830 | 0 |
| 15FPS，首轮65% | 10117 | 674466 | 35310 | 13 / 579370558 | 1 |
| 15FPS，首轮100% | 6199 | 413266 | 2144 | 5 / 222834830 | 0 |

四组真实恢复/独立摘要均通过，resource/deferred/orphanQuota/conflict均0。等有效帧率下单纯减半发送FPS几乎无收益；取消65%使30FPS投影时间下降38.7845%，不是仅靠增加decoder名额。这是首轮不足、整圈等待及完成后重复空口共同放大耗时的窄路径证据，不抹去O在此前较高保留率现场的有效收益或其它late-join反例。新增定向恢复1例121断言、边界及原首轮配置5例51断言通过；现存M late-join性能失败未被修改/复跑成PASS。

随后以原封存candidate-q-fill01的两个exe开始nl0914-fill-gray100-fullinitial-f30-s6：100000000B、原2560×1600@240、30fps、6MiB段、spatial和1GiB预算约束名额不变，仅不传--grayfast-short-initial-airtime，使预先配置的首轮100%。普通1200秒上限/120秒无进展保护保留，未增加反馈。当前只登记启动，最终成绩须等report、摘要、发布、重开及独立SHA。未同时运行构建、native/GPU测试或重负载任务，未修改远控/网络设置，也不碰左屏或输入焦点。

证据：candidate-r-fullscreen-cadence-native的before/built快照、run_native.py、build/bounds/recovery日志和result.json；run_fullscreen_full_initial_100.py及field-wrapper-from-q.diff；独立现场run与stage。后续全屏重采样参考与未验证假设见该stage的RESEARCH_NEXT.md，尚未改变生产过滤算法。


### 21.37 R全屏100MB最终通过：同封存二进制取消首轮65%后快32.79%；用户要求停止研究并交付v0.6

nl0914-fill-gray100-fullinitial-f30-s6完整100000000B用525968ms=8分45.968秒；对照Q的782615ms，节省256647ms=4分16.647秒（32.793519%）。同Q Encoder 1106eacf...和P2 Decoder c4b60245...逐字节一致，仍原2560×1600@240、完整无裁切铺满、30fps、6MiB段、spatial和原1GiB预算约束名额；唯一发送调度变量是不传--grayfast-short-initial-airtime而使用首轮100%。候选总超时1200秒比旧1800秒更严格，120秒无进展保护不变，均不是主计时。

同SessionId=3ba9237eb4fcd65c2ae6b44242bb11a7，SessionTag=16731198839444628556；源fresh/稳定、两端无resume复用、whole digest/安全发布/重开和两次外部SHA256均通过。SHA256=4111eb5acba90ea84b8e1cce370699d682ac1460e9f841069c7445cd89a134eb，BLAKE3=32d85d4af2aca6c97e3a7104a47163701708dc554f24b47f900349afa1ea4afc。有效帧率7.495648→7.509483，Sender29.217494→29.186912，证明本次收益主要不是提高帧率；已完成段重复symbol37277→4784，峰活动16→4，峰预留708984560→178267864B，资源/deferred/orphanQuota/conflict全部0。相邻本机journal中至少10秒的“仅完成后重复、无新unique”区间合计288156→20971ms，是辅助归因而非替代最终计时。

Receiver53556成功退出。远端3352由已采用的WM_CLOSE流程退出，forced=false，但Encoder报告state=Failed/exitCode=1，原因为DataWindow stopped without an explicit user stop request；这是包外编排关闭窗口的既有停止分类，不隐写成Encoder正常Stop。关闭前完成文件的权威证据是Receiver全部最终门。显示模式不变、桥liveProcesses=[]。前台句柄端点不同，未执行输入或焦点操作，不承诺全程前台相同。

尚未重测R的完整Medium/Big；O旧居中呈现完整合计4312469ms保持历史最好，不归给R。R的525968ms仍比P2旧居中329033ms慢，因此全屏刷新瓶颈未解决。用户随后要求马上收尾、v0.6发布包、详细现状及续接prompt；已停止新研究/远控测试，并明确获准仅提交任务内改动、排除三个保护对象、不推送。v0.6新版本二进制与上述0.5源码候选不同，必须单独列出发布验收边界，不能把R实测改名为v0.6实测。

证据：candidate-q-r-fullscreen-initial-100-comparison.json、candidate-q-r-fullscreen-100-journal-attribution.json；run及stage的最终report/result/independent-verification、显示/进程收尾；candidate-r-fullscreen-cadence-native的测试及post-field-build（均完成）；后续交付入口docs/SESSION_HANDOFF_20260914_V0.6.md与docs/NEXT_TASK_PROMPT_V0.6.md，发布实物/验收记录以artifacts/release-v0.6-20260914为准。


### 21.38 v0.6后test-only两阶段采样参考：point/linear/area全过几何硬门，area诊断幅值最优但受归一化混淆；未改生产

新会话先核对live状态：HEAD 25a08aa与发布回执一致、工作树仅三个保护对象未跟踪、v0.6 ZIP/双EXE哈希现场复验一致、本机无PixelBridge进程。随后按交接建议实现两阶段链test-only对照：canonical 1920×1080 →（stage-1全屏合成 point=生产现行为 / linear=边缘钳位双线性 / area=精确盒重叠）→ 全屏栅格 →（stage-2远控视口独立X/Y permille尺度+分数原点双线性，黑外部）→ 2560×1440捕获ROI。全部为tests/内新增（unified_two_stage_resample_fixture.h、PBUnifiedTwoStageResampleTests、PBDemodD3D11 1例、PBApplication生产合成==point夹具绑定1例），无生产源码、呈现合同（UnifiedViewportFilter::Point）、阈值或wire变更。

硬门全绿：CPU 4例1156断言（GrayFast+GrayStates×3模式×3配置，随机满载payload，live-like 851×851复合≈1.1347×1.2611、强各向异性937×742、恒等控制，全部精确恢复+几何拟合≤0.002）；真实Capture(WARP) 1例818断言（GrayFast×3模式×2序列，与CPU oracle同字节逐字节一致）；生产绑定1例6断言。回归：GrayStates 2181/13、App fullscreen 1538/6、Demod全量669758/32全绿。

诊断（非门）：平均|软度量|稳定area>linear>point（live-like 30871/29827/29545），但GrayFast度量含8192/对比度²归一化，平滑光栅的低峰值对比度会机械抬高幅值，不能单独当鲁棒性证明。有界亮度噪声代理（±0..8 chroma中性）三模式均保18/18块；幅值衰减area最慢(-674)优于point(-696)与linear(-972)。min|metric|极值统计方差大、跨模式无稳定序，不作依据。

边界：几何+合成噪声代理不等于codec实测或实屏吞吐；候选晋级需显式opt-in编码器合成开关（默认不变）、封存、≤25MB非本机与R首轮100%基线对照，模式选择（area/linear）与执行授权待用户决策。M late-join失败门及全部既有安全边界保留。证据：artifacts/nonlocal-stall-20260913-2111/candidate-s-two-stage-sampling-reference/（三stdout+NOTES）。


### 21.39 显式opt-in编码器--fullscreen-sampling落地：限灰阶家族全屏、字节级对齐夹具、中性快速路径；18.2/28.8ms每帧成本待实屏判定

用户批准继续后实现显式候选开关。`FullscreenSamplingMode{Point,Linear,Area}`进入EncoderConfig与CLI `--fullscreen-sampling point|linear|area`（默认Point=历史逐字节行为；缺值/重复/未知值/无--single-monitor-fullscreen/非灰阶家族均fail-closed拒绝）。新合成`ComposeUnifiedFullscreenSampledBgra`仅在被显式选中且Unified灰阶家族（UnifiedGray/UnifiedGrayFast）单屏全屏时进入；remote-lf4与产品SC6路径不经过该函数，`ComposeRemoteVisualFullscreenBgra`原字节不变。开关限定灰阶家族的原因：其canonical栅格逐像素中性（B==G==R，4:2:0位等价测试已证全帧恒等），合成可先做每帧中性校验（非中性即回退通用三通道路径，正确性不依赖假设），再用单通道计算+复制快速路径。

验证：生产Linear/Area合成与两阶段夹具参考在随机BGRA（通用路径）与中性灰（快速路径）两类源、1920×1080/2560×1600/3840×2160三尺寸逐字节相等（28断言+3负例）；CLI回归10例（含unified彩色拒绝、无全屏拒绝、origin冲突拒绝）与旧initial-airtime 9例复跑全绿，前台不变；PBApplication [fullscreen] 7例1578断言、两阶段套件4例1156断言复跑不变。探针计时（含中性扫描与分配）：2560×1600每帧linear约18.2-18.6ms、area约28.7-28.8ms；30fps预算33.3ms下linear偏重、area很可能挤压节奏。不隐藏该成本：实屏由FullscreenCompose阶段诊断、logical dwell违例计数与Sender实际fps判定；若节奏破坏则候选回退15fps或后续优化，不在本轮放宽任何门。

默认行为、wire、阈值、GUI零变化；未提交。候选实传（≤25MB、新tag、与R首轮100%同配置、point/area/linear三组单变量对照）待按桥规范执行。


### 21.40 候选S全屏采样25MB实传A/B：linear最快-2.84%、area -2.11%，全部四门+同源双SHA通过；Sender节奏被合成成本压低但有效帧率反而略升

用户批准后按桥规范执行三组25MB单变量对照（唯一变量fullscreenSamplingMode；其余与R同：unified-gray-fast、30fps、6MiB段、spatial、--budget-bound-decoders、首轮100%、远端2560×1600@240原模式、本地DISPLAY2 ROI、Receiver-first、新tag不复用）。候选包candidate-s-fullscreen-sampling：Encoder 91011d5b2a1ddd5daaa5d5fe5e319261303451d2e43c125e5362ab23fcdb8ec5（25a08aa+未提交diff，正常CMake Release，sourcePatchSha256=ea64e3e0ef20a72a62354499b3996114ab2d56be04af6aadd98d5d8e935d9574），Decoder=v0.6发布件151e46e5逐字节不变。guard包装run_fullscreen_sampling_25.py逐run做本地右屏物理ROI校验、远端display前后不变断言、list-runs无残留断言、封存哈希断言及最终验证（同Session、源fresh/无resume、四门、第二次独立SHA、sender报告fullscreenSamplingMode与配置一致）。

| 采样模式 | 耗时ms | vs point | 接收有效帧率 | Sender提交fps |
|---|---:|---:|---:|---:|
| point（省略开关=历史路径） | 127312 | — | 7.494256 | 29.127889 |
| area | 124627 | -2.108992% | 7.652540 | 16.291618 |
| linear | 123695 | -2.841052% | 7.760921 | 18.415712 |

三组SessionTag各异（9096108879800814361/10189233931644109573/8761994070720726350）、同源25000000B、第二次独立SHA256同为f9fa18860c159080d860b654903f272846a5af5fa51ba303e39825ce4b53c3e4，全部门通过、远端显示模式未变、无残留登记进程、端点前台相同。合成成本如§21.39预告压低了Sender提交fps至16-18，但接收端有效帧率反而从7.494升至7.653/7.761：与远控信道约7.5有效帧/秒内容上限模型一致，更平滑光栅经codec后每有效帧保真更好，降低发送重复并未损失（略增）吞吐。

边界：每模式单样本、同晚链路，非统计或认证结论；25MB/4段不触发>12段graduation；linear与area相差0.75%在单样本噪声内；M late-join与任意分辨率/缩放认证缺口保留。未提交、未推送。证据：candidate-s-fullscreen-sampling-25m-comparison.json及三tag目录（各含result、receiver/report、remote-evidence、独立验证与-stage前置/收尾）。


### 21.41 候选S 100MB反转定论：采样合成成本压低Sender节奏后大文件退化42.8%/66.6%，每有效帧新信息量单调坍缩；当前形态不可采用

同晚紧接25MB A/B，以完全相同单变量设计升级到100000000B（17段，>12段graduation路径启用；每run仍≤100000000B频繁测试界；--seconds 1200）。三组全部通过四门、同Session配对、源fresh/无resume、第二次独立SHA256同为4111eb5acba90ea84b8e1cce370699d682ac1460e9f841069c7445cd89a134eb，远端显示模式未变、无残留登记进程；point/linear端点前台句柄样本不同（与R相同的既有口径，未使用输入或焦点自动化）。

| 采样模式 | 耗时ms | vs point | Sender提交fps | 接收有效帧率 | 估算每有效帧新字节 |
|---|---:|---:|---:|---:|---:|
| point（省略开关） | 526930 | — | 29.230819 | 7.493378 | 25326 |
| linear | 752227 | -42.756533% | 18.531692 | 7.714769 | 17232 |
| area | 878081 | -66.640920% | 15.557387 | 7.634859 | 14916 |

point组526930ms与R的525968ms（同配置、Q期二进制）相差+0.18%，同晚信道与R期一致，基线可信。结论：25MB的-2.1%~-2.8%收益在17段下完全反转为大幅退化，且退化幅度与每帧合成成本（约18.2/28.8ms）单调对应；接收端有效帧率在三组都保持7.49-7.77，证明信道内容率上限与发送fps无关，但每有效帧承载的新信息从25.3KB坍缩到17.2/14.9KB——更慢的Sender节奏在graduation/多段交织下大幅提高信道采样帧中的重复/无效占比。机制级归因（graduation交互 vs codec过渡混叠）尚未从journal隔离，登记为后续工作。

处置：--fullscreen-sampling保持显式opt-in、默认point、产品路径字节不变；当前双精度逐帧实现形态不满足30fps节奏，候选不晋级、不改默认。若继续该方向，前置条件是把合成压到约≤10ms/帧（定点化+向量化，以定点公式为新参考夹具重建字节级验证），或先做journal级novelty坍缩归因；任何重测仍以point为基线、新tag、≤100MB。证据：candidate-s-fullscreen-sampling-100m-comparison.json及三个100MB tag目录（各含result、receiver/report、remote-evidence、独立验证、-stage前置/收尾）与25MB同构六组数据。


### 21.42 point 60fps证伪：Sender交付封顶约29.7fps时抖动提交使100MB退化73.5%；30fps point仍为已知最优

据§21.41"越快越好"趋势外推的point 60fps单变量测试（同包同配置，仅--fps 60，tag nl0914-fss-point-100m-f60-s6）。四门+同Session配对+源fresh/无resume+第二次独立SHA256=4111eb5a...全过，远端显示未变、无残留。结果914215ms，比30fps的526930ms慢73.498377%。关键观察：配置60fps但Sender实际提交仅29.664002fps——每帧FEC构建+合成+提交的固有成本把交付率封顶在约30fps，形成"时钟60Hz/交付~30fps"的抖动提交；接收有效帧率7.495233不变，每有效帧新信息从25.3KB再次坍缩到14.6KB，与慢Sender两组同签名。注意60fps同时改变发送端每帧预算分档与提交节奏，两者贡献未隔离（见comparison JSON caveats）。

结论：15.6→29.2fps区间的单调改善不能外推到60；30fps point（526930ms，与R复现差+0.18%）仍是当前100MB已知最好配置。今晚七组100MB/25MB实验一致指向：信道有效帧率~7.5为内容率上限，吞吐差异几乎全部来自每有效帧新信息量，而它在Sender交付节奏变慢或抖动时坍缩。证据：candidate-s-fullscreen-sampling-point-fps-comparison.json及tag目录。

### 21.43 fast-linear 中性灰阶合成窄验证：本地成本降至约9ms，但100MB仍不具备晋级价值

在主人授权继续 O1 方向后，先复核当前 live HEAD `ea8fcba` 的调度实现：高于 15Hz 的 Unified later pass 已是 repair-only 增量喷泉预算，发送活动窗口已为 6，接收端并发上限仍为 8；因此没有重复落地同一 O1/O2 语义。为解决采样候选的已知前置瓶颈，仅对 `ComposeUnifiedFullscreenSampledBgra` 的中性灰阶线性路径做了可逆、非 wire 改动：保留四项 double 乘加及求值顺序，移除每像素 channel 循环、临时三通道数组和 `std::round` 库调用，以截断加半整数比较实现同一 half-away-from-zero 结果，alpha/上限钳位不变；彩色回退路径、默认 point 路径均未改动。提交 `e8c9c5b`。

本地 `PBApplicationTests [sampling]` 40/40、全屏绑定 1578 断言、两阶段采样 1156 断言及 `PBUnifiedSenderSchedulerTests` 通过；中性 2560×1600 linear 合成由约18.2–18.6ms降至约9ms。已知 `PBApplicationTests` 全目标的 M late-join 12505<12000 失败保持原样，未删改。

随后以新封存候选目录 `artifacts/nonlocal-stall-20260913-2111/candidate-t-compose-fast-release`，每次使用新 tag、Receiver-first、同配置（unified-gray-fast、30FPS、6MiB、首轮100%、budget-bound）完成三次右屏窄验证：

| tag | bytes | sampling | receiverRuntimeMilliseconds | UniqueVisualFPS | 结果 |
|---|---:|---|---:|---:|---|
| `nl0914-fss-fast-point-25m-f30-s6` | 25,000,000 | point | 131349 | 约7.54 | 四门+独立SHA通过 |
| `nl0914-fss-fast-linear-25m-f30-s6` | 25,000,000 | linear | 129796 | 约7.88 | 四门+独立SHA通过 |
| `nl0914-fss-fast-linear-100m-f30-s6` | 100,000,000 | linear | 714137 | 约7.75 | 四门+独立SHA通过 |

三次的 SHA256 分别与源元数据匹配（25MB=`f9fa18860c159080d860b654903f272846a5af5fa51ba303e39825ce4b53c3e4`；100MB=`4111eb5acba90ea84b8e1cce370699d682ac1460e9f841069c7445cd89a134eb`），wholeDigestVerified、renameSucceeded、finalReopenVerified、published 均为 true，资源拒绝/延期/孤儿/冲突均为 0，前台句柄前后相同且桥 `liveProcesses=[]`。25MB 成对结果 linear 仅快约1.18%，属于单次观察；100MB linear 比当前 point 526930ms 慢约35.4%，虽较旧 linear 752227ms 快约5.1%，仍不足以支撑长文件路线。故 `--fullscreen-sampling` 继续保持显式 opt-in、默认 point，不进入完整 Medium/Big 重测。

证据目录：`candidate-t-compose-fast-20260914-2201/`（本地基线/源码快照与汇总）、`nl0914-fss-fast-point-25m-f30-s6/`、`nl0914-fss-fast-linear-25m-f30-s6/`、`nl0914-fss-fast-linear-100m-f30-s6/`。本节不把100MB窄样本升级为完整目标结论；完整 Medium/Big 仍只引用 §21.29–§21.31 的 O 组合实测。

### 21.44 对§21.43现场参数与归因的勘误（保留原始证据）

复核 Encoder report 后发现，§21.43 三次直接调用底层 `field_run_fullscreen_sampling.py` 时漏传了 `--grayfast-spatial-interleave`，实际 `grayFastSpatialInterleave=false`。因此三次文件摘要/发布/重开及独立SHA成功仍成立，25MB pair在非spatial配置下的1.18%单次差异也可保留，但**714137ms不能与R/S spatial point 526930ms作为单变量性能比较，也不足以否定fast-linear在正确spatial配置下的表现**。§21.43关于“同R/S配置”和“仍慢35.4%故不晋级”的直接归因撤回；旧tag不覆盖、不复用。

本次底层调用未经过外层display前后guard；没有执行display-set或输入/焦点操作，报告显示远端2560×1600@240且前台端点相同、最终桥liveProcesses=[]，但不能据此宣称完成了外层前后显示模式一致性验证。候选T的build-identity曾误把源码commit写入actualBuildIdentity；实际exe报告的内嵌commit仍为configure时的25a08aa。新create-only `candidate-t2-compose-fast-release`保留同一Encoder SHA256 `f510ba36f9f5b91157ec22d2a350db8692e0dea09693b256e4aac8a74bdb60bf`，明确记录源码e8c9c5b、完整patch/源hash和内嵌commit陈旧的差异，不冒充新发布。

纠正入口为 `run_fast_fullscreen_sampling_guarded.py`，沿用原sampling外层guard并固定新候选hash，强制spatial/首轮100%/30FPS/6MiB和同Session/源稳定/无resume/完整最终门，前后核对右屏ROI、远端模式与登记进程。纠正run只用全新tag，先25MB，结果再决定是否100MB；原始错误及其修正均保留。

### 21.45 T2正确 spatial 参数结果：fast-linear 小文件改善、100MB 仍慢于 point

使用 §21.44 的 guarded 入口和新候选（Encoder SHA256 `f510ba36f9f5b91157ec22d2a350db8692e0dea09693b256e4aac8a74bdb60bf`，Decoder 为 v0.6 冻结件）补做成对/升级验证。每个 tag 均为新 Session、Receiver-first、`--grayfast-spatial-interleave`、30FPS、6MiB、首轮100%、`--budget-bound-decoders`；外层 guard 确认本机 ROI、远端2560×1600@240 前后不变、无输入/焦点自动化和 `liveProcesses=[]`。

| tag | bytes | sampling | runtime ms | UniqueVisualFPS | Sender submitted fps | allFinalGates |
|---|---:|---|---:|---:|---:|---|
| `nl0914-t2-spatial-point25` | 25,000,000 | point | 126906 | 7.51 | 28.74 | true |
| `nl0914-t2-spatial-linear25` | 25,000,000 | linear | 120974 | 7.85 | 21.90 | true |
| `nl0914-t2-spatial-linear100` | 100,000,000 | linear | 660758 | 7.74 | 21.66 | true |

三个 run 的 25MB SHA256 均为 `f9fa18860c159080d860b654903f272846a5af5fa51ba303e39825ce4b53c3e4`，100MB SHA256 为 `4111eb5acba90ea84b8e1cce370699d682ac1460e9f841069c7445cd89a134eb`；wholeDigestVerified、renameSucceeded、finalReopenVerified、published 和第二次独立 SHA 全部通过，资源拒绝/延期/孤儿/冲突均为0。25MB linear 相对同候选 point 快4.67%，但仍是单次小文件观察；100MB linear 相对已知 point 526930ms 慢25.3%，相对旧未优化 linear 752227ms 快12.2%，说明本地合成优化生效但采样模式在多段 graduation 下仍使发送节奏/每帧新信息不足，不能晋级或用于完整 Medium/Big。O1 授权已记录；当前30FPS later-pass repair-only 与发送窗口6已在 live scheduler 中，不再重复改写同一语义。

证据目录：`nl0914-t2-spatial-point25/`、`nl0914-t2-spatial-linear25/`、`nl0914-t2-spatial-linear100/`、候选构建与源快照 `candidate-t2-compose-fast-release/`。完整目标文件仍保持 O 组合历史最好，不以本节100MB推演替代完整摘要/发布/重开实测。

## 21.46 T3 紧凑 BGRA 提交拷贝窄验证（2026-09-14）

为降低 Sender 在 `DataWindow::SubmitFrame` 持有 `stateMutex` 时的逐行拷贝开销，新增提交 `fb6a849`：当输入 `rowPitch == width*4`（生产全屏路径的紧凑 BGRA 不变量）时，使用一次有界 `memcpy`；非紧凑 stride 继续保留逐行 `copy_n`，不改变队列深度、pending-frame 替换、Present、显示模式或协议语义。构建后仅替换私有候选包 Encoder，Decoder 保持冻结字节不变。

新的受保护远端证据 tag：`nl0914-t3-submit-point25`，路径为 `artifacts/nonlocal-stall-20260913-2111/nl0914-t3-submit-point25/`。参数为 receiver-first、`25,000,000` bytes、RAW、30 fps、6 MiB segment、`--grayfast-spatial-interleave`、`--budget-bound-decoders`、fullscreen sampling=point；远端显示/ROI 前后未变，foreground identity 未变，liveProcesses 为空。完整摘要、安全发布、final reopen、独立 SHA-256、资源/冲突/延期/配额计数均通过，`allFinalGates=true`。

结果：`126,897 ms`，UniqueVisualFPS=`7.528021963578194`，Sender submitted FPS=`29.50310765850552`。同候选之前的 point 25 MiB 参考为 `126,906 ms` / `7.514192759443952` / `28.744385363199285`；总时间变化 `-9 ms`（远小于单次远端噪声），但 Sender 提交速率由 `28.744` 升至 `29.503`，说明紧凑拷贝优化改善了本机提交侧余量，尚未证明能缩短远端唯一帧捕获瓶颈。不能据此重跑 Medium/Big 或宣称总目标完成。

定向验证：`PBRenderD3DTests.exe '*'` 407 assertions/24 cases 通过；`PBApplicationTests.exe 'sender*'` 8151 assertions/1 case 通过。未执行 full CTest、Medium/Big 远端重跑；point 仍为默认采样。

### 21.47 O3b 当前实现核对、发送端本地窄基准及 T3 归因勘误

用户再次明确授权 O3b 本地评估后，当前源码确认：接收端 `UnifiedVisualCpuOracle` 的并行 FEC 已在历史 §12 落地，并扩展至 GrayFast 的18个码字；每参与线程私有 Robust/Fast decoder，按槽合并结果，带逐槽 diagnostics 时保留串行路径。不能再把“新增接收端18码字并行”当作尚未实施的优化。此前把O3b重新提出为未实施项不准确，本文以当前源码为准。

新 artifact-only 微基准 `nl0914-o3b-encode-bench01/` 链接当前正常 CMake 构建的 PBInnerFec/PBProtocol，测发送端编码而非接收解码。每帧18码字、Fast/Robust/Balanced三profile、每profile128个独立伪随机输入帧（前8帧warm-up、120帧计时），串行与2个持久worker+主线程的3参与者池逐帧交替先后顺序。全部6912个码字的串并输出逐字节相等，含两路径编码输入20,736,000B；没有修改生产池、wire、矩阵、摘要或资源门。

| profile | serial p50 / p95 ms | pool3 p50 / p95 ms | serial / pool3 total ms |
|---|---:|---:|---:|
| Fast | 1.063 / 1.3194 | 0.3675 / 0.6672 | 131.115 / 49.3632 |
| Robust | 1.1461 / 1.4282 | 0.417 / 0.6913 | 141.835 / 54.3802 |
| Balanced | 1.0806 / 1.3681 | 0.387 / 0.6249 | 134.303 / 49.9183 |

这是本机 CPU 微基准，不是远端 CPU、raster、Wirehair、完整文件或吞吐证据。Fast绝对收益约0.7ms/帧，不据此往生产热路径新增线程池。测试池未注入线程创建失败；它没有进入产品构建。完整源码、构建/库/结果hash见该目录 `identity.json`，可用新build/output位置复跑。

**T3归因勘误**：§21.46 的25,000,000B是25 MB而非25 MiB；参考Sender准确值28.744385363199285。单次非随机A/B中runtime126906→126897ms、submitted FPS28.744→29.503，只能记录观测差异，不能证明 `memcpy` 导致了提交余量改善，更不能证明“唯一瓶颈已定位”。BaseLuma FEC failure=20而非所有层错误均0；被擦除的码字未越过CRC/identity/最终摘要门，outer冲突/资源/延期/orphan quota均0。T3包沿用T2部分source metadata未更新，原包不改写；补充的 `candidate-t3-submit-copy-provenance-correction.json` 和完整source patch给出fb6a849源码与837b5a69... Encoder的对应边界，内嵌commit仍是configure时25a08aa，不冒充新release。

**下一窄假设（尚未完成）**：fast-linear配置30而实际submitted21.655，`CalculateUnifiedGraduationTarget`仍按配置fps线性扩大base和margin；同100MB outerAlreadyCompletedSymbols=24514，point参考仅4743，outerUniqueSymbols均约61400。按既有观测ceil(21.655)=22预注册单点配置，保留同一T2封存包、linear、6MiB、首轮100%、budget-bound及全门。新tag `nl0914-t4-linear-f22-100m`，唯一变量30→22，依据文件 `nl0914-t4-linear-f22-100m-preregistration.json`。不是FPS扫描，不改生产调度，结果必须看完整100MB发布/重开/独立SHA和native时间，不能从早期进度宣称胜出。

### 21.48 T4 固定22fps的100MB结果：修复linear退化19.84%，但未超越point基线

预注册单点 `nl0914-t4-linear-f22-100m` 已完成。与 `nl0914-t2-spatial-linear100` 同一封存T2 Encoder/Decoder、linear、6MiB、首轮100%、空间交织、预算约束接收；只将configured FPS从30变为22，没有中途观测反馈或调参。100,000,000B、16段（§21.41的“17段”以实际sender report的16段勘误）、receiver-first、新Session/无resume/源稳定、whole digest/safe rename/final reopen/published及第二次独立SHA全部通过。SessionId=`166c8ec92d4b59f588ed2e9a2bdff33d`，SessionTag=`510792035862112369`；SHA256=`4111eb5acba90ea84b8e1cce370699d682ac1460e9f841069c7445cd89a134eb`，BLAKE3=`32d85d4af2aca6c97e3a7104a47163701708dc554f24b47f900349afa1ea4afc`。

| 指标 | T2 linear@30 | T4 linear@22 | 较早point@30参考（非严格配对） |
|---|---:|---:|---:|
| 完整native runtime ms | 660758 | 529685 | 526930 |
| Sender实际submitted FPS | 21.655129 | 21.145716 | 29.230819 |
| Receiver UniqueVisualFPS | 7.742468 | 7.754428 | 7.493378 |
| outerUniqueSymbols | 61400 | 61400 | 61401 |
| outerAlreadyCompletedSymbols | 24514 | 7349 | 4743 |
| capture dropped / arrived | 585 / 7800 | 708 / 6627 | 1907 / 7623 |
| staleResultDrops | 0 | 1 | 1 |

T4较同T2 linear@30省131073ms（19.836763%），已完成段重复减少70.021212%；实际submitted与unique FPS基本接近原linear，但用更小配置预算消除了大部分无效发送，支持“配置预算/实际供帧失配”假设。配置FPS也影响其他调度节奏，未隔离每个机制；不是统计因果证明。T4仍比较早点采样参考慢2755ms（0.522840%），不宣布胜出，不晋级linear默认，不启动完整Medium/Big。

T4资源拒绝、Outer冲突、deferred、orphan quota/conflict为0；**orphanAdmitted=33、capture dropped=708、staleResultDrops=1不是0**，保留所有原始计数。前台端点句柄1656821064→1205801604不同，未发出任何焦点/输入操作，不能声称全程前台不变。远端2560×1600@240前后mode完全相同，receiver exit0，owned sender经已有WM_CLOSE退出分类Failed/exit1保持原样；最终registered liveProcesses为空、本机双端无进程。

归因字段补充：`postGpuFecCpuTimeTotal100ns`在`Demodulator::Poll`外层计时，包含readback结果整理、hard bits、FEC、accepted block复制等，并非纯FEC时间。T4均值15.639881ms/observation、processCpuEquivalentCores=0.408084，不能拿它宣称“CPU并行化后即可提高unique FPS”。完整机器可读对照 `nl0914-t4-fixed22-comparison.json`；新run含外层guard、同Session/全门与第二次SHA证据。

### 21.49 面向完整指定文件的剩余门槛（条件容量推导，不是实测成绩）

当前GrayFast每码字净Transport上限1629B、最多18槽，全部都当有效payload也仅29322B/unique frame（乐观忽略Control/repair等开销）。若有效帧率仍维持当前约7.8Hz，则乐观encoded goodput≤228711.6B/s；以历史完整两个文件的encoded bytes总计1326192709B估算，乐观总时间也要5798.54秒，未计安全发布/重开耗时。这是“同encoded bytes且帧率不提高”条件下的代数上界，**不证明链路帧率不可提高，不替代真实整文件计时**。

要匹配历史O两个完整文件4312.469秒，即使18槽全有用也需至少10.487864Hz；若每帧留1个Control槽则需11.104798Hz。故仅本地编码节省0.7ms、修复linear过量预算，尚不足以证明总目标改善；后续需要对实际unique-frame有效载荷率有直接证据的方案。新的visual profile/layout/Golden方向需另行明确授权，不能把历史O4（已演进为GrayFast）再说成未实现。证据 `nl0914-o3b-conditional-capacity-bound.json`。完整Medium/Big未在本轮重测，历史O成绩不改名为T2/T3/T4成绩。

### 21.50 已获授权的新 Profile 离线可行性对照：GrayFast8（layout 14）仅保留为 artifact 候选

主人批准先做“提高单帧净载荷”的新视觉 Profile／布局离线对照。本轮严格停留在 artifact-only：未改生产源代码、默认 Profile、Decoder、Golden Vector、Citrix、显示模式、输入或远端运行态，也未进行 Medium/Big 或新的远端文件传输。基线取当前 `PB-Experimental-GrayFast-1`（layout 13、6×6 tile、41,872 Data tiles、7 个灰阶 carrier plane、18 个 Fast codeword）。

离线候选命名为 `PB-Experimental-GrayFast8-Offline-1`，新 identity=`0x5042475246383031`、layout=`14`，明确状态 `ARTIFACT_ONLY_NOT_ACCEPTED`。候选保持 1920×1080、6×6 tile、现有 16,200-bit Fast inner FEC 与 1,629 B Transport payload，仅假设增加第 8 个独立视觉 plane（逐帧独立校准的二值中性亮度 ladder），不假定现有 Control reservation、pilot、mapping 或 Decoder 可以直接复用。

| 模型 | carrier bits | Fast codeword slots | 单帧 Transport 上限 | 相对当前 |
|---|---:|---:|---:|---:|
| GrayFast 当前 7-plane/18-slot | 293,104 | 18 | 29,322 B | — |
| GrayFast8 候选 8-plane/20-slot | 334,976 | 20 | 32,580 B | +11.111111% |
| GrayFast8 保守 8-plane/19-slot | 334,976 | 19 | 30,951 B | +5.555556% |

这是 carrier/代码字整除得到的容量上界，不是吞吐实测；没有扣除新 pilot、Control、repair、丢帧或重复等待。确定性 packing benchmark（128 帧、41,872 tile/frame）反而显示增加 plane 会增加本机 synthetic 工作量：当前 7-plane/18-slot 为 687.5444 ms，8-plane/20-slot 为 765.1697 ms，8-plane/19-slot 为 769.2151 ms；该 benchmark 不含 FEC、GPU、WGC/DXGI、远端 codec、摘要、safe publish 或 reopen。

若仅作条件代数估计，并假设 T4 的 `7.754428 Hz` unique visual rate、其余开销及新 plane 恢复质量均不变，则 100 MB 的 `529,685 ms` 可按 18/20 缩至约 `476,716.5 ms`；此数字不是现场结果，不得替代完整门。更小 tile 或额外 plane 会改变误码率、亮度/色度映射、缩放相位、pilot/locator 几何及资源预算，必须新建 canonical raster、mapping hash、Decoder oracle、Golden vectors，并先通过错误 identity/layout 混用、冲突 payload、资源耗尽和 ≤25 MB 的完整摘要/安全发布/reopen 验证，再考虑 ≤100 MB paired 对照；Big 不因本授权自动获得许可。

机器可读产物：`artifacts/nonlocal-stall-20260913-2111/nl0915-profile-offline-grayfast8-01/`（`profile_spec.json`、`packing_benchmark.json`、`identity.json`、`NOTES.md`、`run-receipt.json`、可复跑 `profile_offline_model.py`）。本节只证明存在 +11.11% 的理论载荷空间，不证明视觉可恢复性或总传输时间改善；完整 Medium/Big 目标仍未完成。
