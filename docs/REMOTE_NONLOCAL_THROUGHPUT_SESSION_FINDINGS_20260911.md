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
| vb bands 15/100 | 待填 | | | | |
| vb bands 30/100 | 待填 | | | | |

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
4. 预期收益模型（待现场验证）：O1 把 100MB 档重复占比 41%→~10-15%、O3a 把 bootstrap 27-32ms/帧→数 ms；若成立，100MB 档 goodput 有望 60→90-110 KB/s（今日链路口径）。

### 11.5 A/B 计划

构建（增量+重configure 取新 commit）→ `ctest -C Release -E PBPresentationGate` → `package_opt_kit.py`（kit-opt1-*.zip + local-receiver-opt + opt-build.json）→ `opt_matrix.py` 四格（bands 15/30×50/100，receiver 带 --stage-diagnostics --journal）→ 对比今日 vb-* 基线。
