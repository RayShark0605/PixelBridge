# G21 执行交接（2026-09-06）

> 本文是关闭当前 Codex 对话前的事实交接，不替代
> [`UNIFIED_VISUAL_LARGE_FILE_IMPLEMENTATION_ROADMAP.md`](UNIFIED_VISUAL_LARGE_FILE_IMPLEMENTATION_ROADMAP.md) 的范围和退出标准，
> 也不把尚未通过的门禁写成已通过。后续任务必须先读仓库根 `AGENTS.md`、路线 G21、
> [`UNIFIED_REMOTE_GATE.md`](UNIFIED_REMOTE_GATE.md) 第 22 节，再使用本文 0.15 定位最终证据；第 15～21 节继续作为历史诊断与准备链。

## 0. 最新权威增量（最终远控结果与单次豁免见 0.15；三档 LocalDesktop 见 0.13；交付收尾见 0.14）

> **阅读规则：** 本节记录 `aec48c1` 交接之后实际完成的代码、测试和本机实屏证据，并取代本文后续章节中
> “416 次 resource rejection 尚未定位”“Base-only 仍缺”“代码基线为 `433bccf`”等已经过时的当前状态判断。
> 后续第 1～8 节继续保留，作为真实远控链历史、原始失败形状和操作背景，不得反向覆盖本节。
> **继续执行后的最新状态优先读 0.15，再读 0.14、0.13、0.12、0.11、0.10、0.9。** 0.1～0.8 的 `4a2463f` 是进入本次执行时的基线，不是新候选的实屏结果。

### 0.1 当前结论与 Git 边界

- 关闭本对话时的**代码基线**为 `4a2463f7e7ddc2179168b00d55bc4ca69f509d53`
  （`fix(unified): rotate striped capture phases`）。本次纯文档交接提交会使最终 HEAD 位于该代码基线之后；下一任务应
  用 `git log --oneline --decorate -20` 和 `git merge-base --is-ancestor 4a2463f HEAD` 核对，而不是要求 HEAD 仍精确等于
  `4a2463f`。
- 工作树只有用户所有、受保护的未跟踪文件 `docs/PHASE1_GATE_REPORT.md`；其 SHA-256 仍为
  `076EF4C9B9F89EABCCD323DBE4BFFC4DC125DDAF96E6EE437D2CF5B1B1CEA306`。绝对不要修改、删除、暂存或提交它。
- 关闭前没有 `PixelBridgeEncoder`、`PixelBridgeDecoder` 或 `PBUnifiedRemoteGate` 进程存活。
- G00..G20 保持完成；G21 已完成启动期 resource 根因修正和实际 capture 的 Base-only 证明，但**仍为 PARTIAL**。
  当前 `4a2463f` 的 64 MiB 本机实屏硬门通过；500 MiB 文件字节与发布门全部正确，却因
  `16,042.594 < 16,384 B/unique` 以性能硬门 exit 1；该候选未运行 1 GiB。不得把“文件正确”误写为 G21
  性能通过，也不得把旧 `786f466` 的 500 MiB 通过嫁接成 `4a2463f` 的通过。
- 用户已明确纠正执行边界：**从 G21 开始，不是永久停在 G21**。G21 全部退出标准真正满足并提交后，应继续执行 G22；
  若仍缺用户稍后安排的带远控因素现场复验，则保留该唯一边界并继续所有不依赖现场的工作，不能伪造远程结论。

### 0.2 本轮已经关闭的两个原始阻塞

#### 启动期 416 次 resource rejection

旧 `433bccf` 真实远程 64 MiB 虽正确发布，仍在 W=8 建立阶段累计 416 次 `outerResourceRejections`。本轮通过
生产调用顺序、ReceiverIngress orphan cache 和 Control/Transport 调度的窄链路证明：旧 sender 的
SegmentDescriptor 只出现在早期 Control，Transport-bearing mixed frame 未保证携带当前 Segment 的 Descriptor；在
descriptor 尚未到达时，其他 Segment 的 Transport 会先占满有界 orphan cache，形成启动期资源拒绝。

修正及证据：

- `634939c fix(unified): bind striped transport before admission`：每个带 Transport 的 mixed frame 都为当前 Segment
  保留其 SegmentDescriptor，且接收端继续先处理 Control、再处理 Transport；没有增大 orphan quota，没有允许未知
  Segment 触发大分配或 FEC codec 创建。
- `951543f test(unified): stabilize striped fallback fixture`：稳定真实调度顺序夹具。
- 定向 no-raster/真实顺序 fixture 在旧行为下可重复出现资源拒绝，修正后通过；后续 64 MiB、500 MiB 本机实际屏幕
  运行的 resource/conflict/deferred/quota/orphan 计数均为 0。
- 资源 telemetry 已细分为 protocol limit/exhaustion、Control reassembly quota、FEC OOM/decoder quota/extra
  insufficient、Receiver policy、orphan admit/drop/exhaustion/conflict 等字段。以后不能再只凭聚合计数猜根因。

#### Base Luma 独立恢复

`7b29c4b test(unified): prove Base recovery from actual captures` 已建立同一实际 capture 的离线
chroma-neutralized 派生闭环，不使用 sender source、理想 raster 或重渲染帧替代实际像素：

- 权威根：
  `<repo>\build-unified-release\g21-local-base-authoritative-0d4e5cda46e34e5082c0c5356778d852`
- 原始实际 capture：256 帧，`3,775,009,897` bytes；派生器只对实际捕获 BGRA 像素执行
  `B=G=R=luma`，共中和 943,718,400 pixels、改变 700,643,540 color components，并保留 alpha、row padding
  和 capture metadata。
- Base-only receiver 显式禁用 Fine Luma 与 Chroma admission，消费相同 256 帧后完整恢复 1 MiB 文件，
  WholeFileDigest、安全发布、final reopen 和外部 SHA-256/BLAKE3 全部通过；resource/conflict 均为 0。
- source/live/Base-only 的 BLAKE3 均为
  `e8d6c2fd806def6af3f04b48c031e3384b1f04f761b5e278dfe7d1903183e36f`；source SHA-256 为
  `d493c0e73f09d5a868f2b0c1225a7fe5eebbce257bc5e774be50bed020d40ecf`。
- 必读原始文件：`receiver-live-recorded/final.json`、`receiver-live-recorded/actual-capture-checks.json`、
  `receiver-live-recorded/actual-capture.pbrv2`、`chroma-neutralization-report.json`、`actual-capture-chroma-neutralized.pbrv2`、
  `receiver-base-luma-only/final.json`、`receiver-base-luma-only/base-luma-checks.json`、
  `base-luma-external-digest-audit.json`。

因此“Base-only 仍缺”已经过时。仍不可用的是**独立 live false-accepted codeword oracle**：Decoder 在真实场景不能
获得 sender truth；应继续报告 `null` 和 unavailable reason，不能拿最终 whole-file digest 或合成 fixture 冒充该 oracle。

### 0.3 新增本机右屏 15 Hz 阶梯基础设施

`tests/UnifiedRemoteGate/Run-G21LocalStaircase.ps1` 是本轮新增的有界、create-only supervisor：

- 精确档位：64 MiB=`67,108,864` bytes；500 MiB=`524,288,000` bytes；1 GiB=`1,073,741,824` bytes。
- 每轮先重新枚举并验证右屏 `\\.\DISPLAY2` 为 2560×1440；Decoder 先启动并捕获完整右屏，Encoder 再以
  `--single-monitor-fullscreen \\.\DISPLAY2`、Unified、15 Hz 启动。
- 使用流式 OS CSPRNG 生成 fresh、不可压缩 source；Session 期间持有只读 lease；每次要求 fresh GUID、fresh source、
  fresh run root，拒绝覆盖已有 evidence。
- Decoder 参数不含 source path、source digest、SessionId 或 sender report。只有 receiver 完成后，独立 audit 才读取
  source manifest 与 published file，复核 exact byte count、SHA-256 和 BLAKE3。
- 每秒采样两个进程的 working set/private bytes；检查 Segment、WholeFileDigest、安全发布、final reopen、残留文件、
  lane CRC/identity、resource/conflict/deferred/quota/orphan、16 KiB 指标、进程退出和 O(active Segment window) 证据。
- sender 通过附着同一 console 的 `Q` 正常停止，不把 deadline/强杀/部分恢复写成成功；失败会保存
  `failure-process-state.json` 和原始 receiver snapshot。
- 该脚本依赖交互式 Windows console；从 Codex `exec_command` 启动时必须使用 `tty:true`。无 PTY 的重定向 console
  会让 sender 的 `Q` 控制失效。建议有界期限：64 MiB 1800 秒、500 MiB 4200 秒、1 GiB 7200 秒。

关联提交：

- `05f8b4d test(unified): add bounded local screen staircase`
- `228a3ae fix(test): automate bounded local sender stop`
- `e4652ea test(unified): bound extended local staircase runs`
- `9903f58 test(unified): preserve staircase failure process state`
- `65584be test(unified): abort doomed staircase tiers early`
- `2f42277 test(unified): abort incomplete coverage runs early`

### 0.4 阶梯运行的权威结果

| 代码/候选 | 档位 | 最终状态 | elapsed | unique / UniqueVisualFPS | B/unique | 决定性结论 |
| --- | ---: | --- | ---: | ---: | ---: | --- |
| `786f466`，固定 capture phase，12.5%+32 repair | 64 MiB | PASS，8/8，正确发布 | 280,616 ms | 3,839 / 13.817856 | 17,480.819 | 所有硬门、外部摘要、错误计数通过 |
| `786f466`，同上 | 500 MiB | PASS，63/63，正确发布 | 2,240,926 ms | 31,321 / 14.002865 | 16,739.185 | 同一旧候选的硬门通过，不代表当前 HEAD |
| `786f466`，同上 | 1 GiB | FAIL，31/128，未发布 | 约 1,130 s 后提前中止 | 15,905 / 13.7571 | 不可用 | Segment 27 仅 6,375/6,385，short 10；deferred/FEC quota=235 |
| `4a2463f`，phase hold=128、step=1 | 64 MiB | PASS，8/8，正确发布 | 262,960 ms | 3,831 / 14.727552 | 17,517.323 | 当前 HEAD 的 64 MiB 通过 |
| `4a2463f`，同上 | 500 MiB | **性能 FAIL**；63/63 且文件正确发布 | 2,238,745 ms | 32,681 / 14.626057 | **16,042.594** | 比 16 KiB 硬门少 341.406 B/unique（约 2.08%）；receiver exit 1 |
| `4a2463f`，同上 | 1 GiB | 未运行 | — | — | — | 按 fail-fast 原则在 500 MiB 后停止 |

当前 HEAD 的 64 MiB 权威根：

```text
<repo>\build-unified-release\g21-phase128-staircase-64mib-7e80057f17c345399a7faf2755b36b51
```

该轮 source/published SHA-256 均为
`bba12e04d6cfeacab4d0db9bd05980a237fa558255d9bd81bc31050d8aae340f`，BLAKE3 均为
`8cf95fd7798d7ef962f65097deb73be0b079a3c4c67635d129f6bc5da090f50b`。Receiver/Encoder peak working set 分别为
404,197,376 / 258,486,272 bytes，peak private 分别为 626,536,448 / 302,841,856 bytes；进程 exit 0/0。

当前 HEAD 的 500 MiB 权威失败根：

```text
<repo>\build-unified-release\g21-phase128-staircase-500mib-a4ee996b04fe438eb10adb1907df0659
```

`receiver/final.json` 的权威业务状态确实是 `Completed`：63/63、524,288,000 bytes、WholeFileDigest、安全发布、
final reopen、frame coverage 均为 true，resource/conflict/deferred/quota/orphan 均为 0；但
`receiver/failure.txt` 精确记录 `published file does not meet the G21 16 KiB/unique hard gate`，所以进程 exit 1。
`postfailure-external-digest-audit.json` 独立证明 source/published 的 exact bytes、SHA-256
`db517f94f1f9400efdae7b3e23f9d3cca804bb905ef822c1645b4cb3eecfc4aa` 和 BLAKE3
`7f8bb00f15083a71d9ebccd668b209669667b175f2acee81d0b10cc24296d81c` 全部相等。Receiver/Encoder peak working set
分别为 406,577,152 / 277,237,760 bytes，peak private 分别为 678,330,368 / 332,394,496 bytes。

旧 `786f466` 的三个权威根和 1 GiB 分析：

```text
<repo>\build-unified-release\g21-final-guard32-staircase-64mib-ccc7a3681f8e47e28c57256340c926d7
<repo>\build-unified-release\g21-final-guard32-staircase-500mib-0c2c8e5b7b944f9bbab8d6c872e9a6e4
<repo>\build-unified-release\g21-final-guard32-staircase-1gib-c63e2113a8f94e4588aef5056567297c
<repo>\build-unified-release\g21-final-guard32-1gib-failure-analysis-77ebac6297204a7d9d777a37a8fb40d7\analysis.json
```

1 GiB 失败不是全局内存耗尽：同一窗口其他 Segment 完成，Segment 27 的 K=6,385、accepted=6,375、scheduled=7,216，
仅短 10 个方程；固定 capture phase 把周期性捕获遗漏相关地集中到某些 Segment。不能通过扩大 W、忽略 quota 或伪造
完成来绕过。

### 0.5 已尝试的修正、参数证据与当前根因判断

repair/window 相关提交按时间保留了真实失败，而不是覆盖证据：

- `3fb3004` 将初始 repair 降为 5%，但 500 MiB 未可靠完成；
- `d061573` 使用 10% margin；历史 1 GiB 固定 phase 运行首窗口只有 7/8，Segment 4 的 K=6,385、
  accepted=6,325，short 60；
- `bcc88c7` 使用 12.5% repair；无 guard 的 500 MiB 曾出现 Segment 31 short 1；
- `786f466` 在 12.5% 之外加入固定 32-block window-transition guard，使固定-phase 64/500 MiB 通过，但 1 GiB
  仍因相关相位不平衡在后续窗口失败；
- `4a2463f` 每隔 128 sweeps（W=8 时为 1,024 logical frames）将 striped capture phase 旋转一步，修复固定 phase 长期偏置，且当前 64 MiB
  通过、500 MiB 正确发布；但预排 repair/control 总开销在接近满速捕获时使 500 MiB 指标低于硬门。

`573cef8 fix(telemetry): count bounded out-of-order frames exactly` 同时将 unique-frame 计数修成有界 exact FIFO 4096
加 max-evicted frontier，避免 out-of-order 观测被近似算法错误重复计数。当前 500 MiB 的 32,681 unique 是有效硬门
分母，不能修改 telemetry 或丢弃捕获较好的帧制造绿灯。

两个 parameter-sweep 根只用于方向判断，不是 live 通过：

```text
<repo>\build-unified-release\g21-phase-hold-sweep-0843147e2e974fb4a1ce93d8aee84b83
<repo>\build-unified-release\g21-phase-hold-sweep-fine-594599bd34b74517a9708f829d83881d
```

同一 synthetic sparse/burst probe 下，hold=272 得到最高的 16,789.808 B/unique，但它只跨两个 Pass-0 phases；
128 为 16,508.946，208/224/240/256、352、384/400/416 也曾越门，192/288/304/320/336/368/432/448/480/512
失败。这组数据不是单调函数，也没有覆盖 `>8 Segment` 的首次 window slide，因此**不得把 272 直接写进产品后就盲跑
1 GiB**。临时 sweep 对常量的修改已经恢复；当前源码/二进制仍是提交的 hold=128。

当前最重要的判断：

1. 500 MiB phase128 与旧固定 phase 的 sender submitted frames 几乎相同（33,510 vs 33,551），所以失败不是简单的
   “phase 旋转让 sender 多跑很久”。新轮捕获更完整（32,681 unique / 14.626 Hz；旧轮 31,321 / 14.003 Hz），反而让
   相同预排开销暴露为较低 B/unique。硬门必须在更好的捕获下也通过，不能依赖 WGC 丢帧。
2. Sender 是单向链路，没有 ACK，无法知道 Receiver 已提前恢复；Pass 0 repair 会对每个 W=8 window 支付。
   `>8 Segment` 文件又要求旧窗口全部完成后才能 slide，否则 Receiver 的 bounded decoder quota 必然拒绝新窗口。
3. 12.5%+32 在 phase-balanced schedule 下较可靠，但固定开销不足以过当前 500 MiB 硬门；更低 repair 有指标空间，
   但历史固定 phase 曾留下 straggler。较合理的解空间是**相位均衡调度 + 更小的 Pass-0 repair**，而不是只调 hold，
   更不是放宽 16 KiB 门、修改分母或增大 Receiver quota。
4. 现有 `RunUnifiedLargeWindowRecoveryProbe` 只有恰好 8 个 Segment，而且允许在后续 Carousel pass 修复，无法证明
   第一个窗口在发出下一窗口 Transport 前已经安全释放。

### 0.6 下一任务的最短可靠推进顺序

1. **先补确定性 `>8 Segment` Pass-0 window-transition probe。** 建议 9 或 16 Segment、Receiver max=8，并从现有
   1 GiB 失败抽取 phase-biased loss。必须断言：第一个窗口所有 Segment 在第一条下一窗口 Transport 之前已 digest-complete
   并释放 decoder；deferred/quota/resource/conflict=0；同时保留 16 KiB/unique 预算。夹具不得靠第二个 Carousel pass
   回头补洞才变绿。
2. 在该夹具下每次只改一个变量，比较保守的 `1/N` 初始 repair（例如 1/9、1/10、1/11）和 phase hold。当前
   `CalculateEquationCounts` 的比例实现假定 denominator 可整除 numerator；若改成任意 numerator/denominator，必须用
   checked multiplication/ceil 并补 overflow/boundary tests，不能静默泛化。
3. 候选必须先跑 scheduler、`[application][g21]`、新 window-transition probe 和 RemoteGate self-test；不要无理由重跑
   full CTest。任何 source/build-identity 变更后都要重新 configure/build，并核对 Encoder/Gate 嵌入 commit 等于 HEAD。
4. 在**同一最终候选/同一 HEAD/同一参数**下，使用全新 root 依次跑 64 MiB → 500 MiB → 1 GiB。64/500 任一档
   出现 digest、publish、resource、conflict、deferred/quota、硬门、内存或进程失败就先修，不要盲跑更大档。
5. 每轮完成后验证 `WholeFileDigest + safe publish + final reopen + complete current-run frame coverage`，再接受
   `VerifiedEncodedBytesPerUniqueFrame`；独立 external audit 必须在运行后读取 source 与 published file。
6. 三档本机真实像素链通过后，更新本文、`UNIFIED_REMOTE_GATE.md` 和路线 G21，并保留精确 artifact root。它仍不能冒充
   用户稍后安排的带远控因素真实复验。
7. 用户安排的最终远控复验满足 G21 live publish、错误文件为 0、resource/conflict 为 0、16 KiB 硬门、Base-only、
   外部摘要及 oracle 边界后，正式关闭 G21并创建独立提交；**随后继续 G22**，不要一直停留在 G21。

### 0.7 操作、安全与证据注意事项

- 新任务拥有项目绝对修改权限，可修改架构、协议、视觉合同、测试工具和 UI；不需要维持旧视觉/协议兼容性。但任何
  改动仍须保持“payload 只从 Decoder 实际捕获的可见像素进入”，禁止 socket、pipe、共享内存、剪贴板、临时文件交换、
  source/digest oracle 或伪造 telemetry。
- 本机有两块屏幕；右屏可自由用于窗口放置、Encoder/Decoder 和鼠标键盘自动化。默认尽量不要扰动左屏；开始每轮前
  仍须现场枚举 monitor，不能永久假设 `DISPLAY2` identity/geometry 不变。
- 每轮使用 fresh GUID/root；原 evidence 永不覆盖、重命名或删除。失败与成功同等保留。跑完必须清理自己启动的进程，
  但不得清理原始 evidence。
- PowerShell 用显式 `Set-Location -LiteralPath`/`-LiteralPath` 和数组计数；需要 Python 时使用
  `<python>`，不要使用系统 Python 3.6。
- Git 只显式暂存目标文件；不 `git add -A`、不 amend/rebase/reset/push。提交敏感构建身份变化后，必须重构建再验证
  packaged binary embedded identity 等于 HEAD。
- 本机 64/500/1 GiB 是 LocalDesktop actual-pixel 证据；synthetic/WARP/no-raster/receiver-only 证据只能解释相应层级；
  用户稍后安排的真实远控链才是 remote authority。任何报告都要分别标明 authority。
- 32 KiB/unique 是工程目标，首版未达可如实记录；16 KiB/unique 是硬门，低于它不得关闭。更好的 capture 产生更多
  unique frames 不是 telemetry bug，也不能人为丢弃来优化指标。
- 只有 whole digest、安全发布、final reopen 且本次 frame coverage 完整时，`VerifiedEncodedBytesPerUniqueFrame` 才有值；
  resume 跨生命周期覆盖不足时必须为 `null` 并给 unavailable reason。

### 0.8 本轮定向验证与未执行边界

- phase-rotation 最终定向结果：`PBUnifiedSenderSchedulerTests` 8 cases / 247,128 assertions；
  `PBApplicationTests '[application][g21]'` 6 cases / 353 assertions；当前 Encoder/Gate embedded identity 为
  `4a2463f`，RemoteGate self-test 通过。
- 本轮没有重跑 full CTest、完整 ASan、全 GPU/GUI/native 矩阵、20 GiB 或新的真实远控复验。
- 当前没有已知错误发布文件；当前 500 MiB 发布文件经独立摘要证明正确，但性能 hard gate 明确失败。
- 本次收尾只更新文档，不改变产品代码，因此不应为文档提交重复大规模测试。

### 0.9 16-Segment Pass-0 证明与新候选（2026-09-06 21:35 CST）

本段与候选代码在同一个独立提交中。进入时 HEAD=`23764d2ae17182b7079e7c6fea17c152cbf8e23c`，
`4a2463f` ancestor 检查通过；保护文件 hash 与 0.1 完全一致。当前尚未运行本候选的实屏阶梯，不拼接旧结果。

**逐变量修正：** 实际 phase hold 的单位是 sweeps，不是 logical frames；W=8 的旧 128 意味着 1,024 frames，
一个完整 Pass-0 window 只覆盖约四个 phases。新候选改为 hold=32 sweeps（256 frames）、step=1；Pass-0 repair
为 `max(16, ceil(K/10)) + 32`，后续 FullRepairPass 的 K+20%、W=8、Receiver quota、wire/SC6 V3、telemetry
不变。启动 Control 四份交织不变；周期 10 秒 refresh 缩为同帧一个 Session/Manifest/current-Segment triplet，
每个其他 transport-bearing frame 仍带当前 Descriptor。总体设计 11.3 同步记录该调度变化。

新增 `PBUnifiedWindowTransitionProbe` 使用 16×8 MiB RAW、实际 production SenderFrameBuilder/ReceiverIngress/
ReceiverPipeline/EncoderSessionStore，不经过 raster。第一条 next-window Transport 之前必须首窗口 8/8 经 Segment
digest 和持久存储完成、active decoder=0；禁止进入第二个 Carousel pass。检查 repair lease 先于使用、持久 window
checkpoint、source immutable、16/16、WholeFileDigest、安全发布、final reopen、外部流式 BLAKE3 和无 part/resume。
内部 deadline=210 秒，独立 supervisor=240 秒/例、最多 11,000 sender frames，不进入默认全量 CTest。

权威开发证据根（开发 dirty source，不冒充正式二进制身份）：

```text
<repo>\build-unified-release\g21-pass0-transition-d3d6e5ba640d4305b161a2e467b70896
```

- `phase-loss-projection.json` 从旧 Segment-27 `.resume` 校验全部 9,095 条 CRC 后提取：K=6,385、scheduled=7,216、
  accepted=6,375。任意未接受 ID 所在 14-equation band 被保守投影为擦除，共 107/516 bands；这不是捕获时间戳重建。
  其他 phases 的每 16 bands 擦除一个是明确的 synthetic sensitivity，不冒充实际 trace。
- `baseline-128-r8-phase3/result.json`：首窗口只有 7/8，Segment 6 short 20，阻止 slide；clean 对照虽正确恢复，
  首窗口 4,296 帧预算仅 15,621.244 B/frame。`hold32-r8-phase3/result.json` 单独改 phase 后 16/16，但预算仍失败。
- `hold32-r10-original-control/summary.json`：再单独减 repair 后首窗口安全，但 4,192 帧仅 16,008.794 B/frame；
  最后窗口 15/16、short 9，禁止靠 pass 1 补洞。仅继续降 repair 不足以同时保证预算和安全 margin。
- 最后单独改周期 Control：`hold32-r10-triplet-phase-check/summary.json` 的 clean/phase3，及
  `hold32-r10-triplet-remaining-phases/summary.json` 的其余 7 phases **全部通过**。首窗口统一 4,072 sender
  frames，8/8、active=0、**全捕获预算 16,480.566 B/frame**；16/16 全部 accepted=6,385，peak active=8，peak
  reserved decoder=457,201,696 bytes，resource/conflict/deferred/quota/orphan=0。
- clean 最终 7,760/7,760 sender/observed frames；偏置八相最终 8,101..8,106 / 7,450..7,455。这些是 no-raster
  计数，不称为 `UniqueVisualFPS` 或 live `VerifiedEncodedBytesPerUniqueFrame`。

定向开发验证：`PBUnifiedSenderSchedulerTests --rng-seed 21092026 --reporter console` 为 10 cases / 247,055
assertions；`PBApplicationTests '[application][g21]' --rng-seed 21092026 --reporter console` 为 6 / 353；
`'[.g21-large-window]'` 为 1 / 25；streaming、resume journal、durable-lease 相邻六例为 6 / 407；
`PBUnifiedRemoteGate --self-test` 通过。日志为根目录下 `scheduler-final-development.txt`、
`application-g21-periodic-triplet.txt`、`legacy-sparse-window-final-development.txt`、
`durable-adjacent-final-development.txt`、`gate-self-test-periodic-triplet.txt`。
旧 fixed-phase 800 lost equations 的算术反例明确保留：10% repair 单独无法承受它，新 16-Segment probe 才是相位
重新分配后的 transition 判据；不是把历史丢失改小或删除恢复门。

Base replay 的实际原始路径已重新核对为 `receiver-live-recorded/actual-capture.pbrv2`（3,775,009,897 bytes），
根目录直接放置的同名文件不存在；本次只核对已有 JSON、长度和首尾，不声称重新完整 hash 3.8 GB。

**下一步：** 提交本候选后重新 configure/build，并核对 Encoder/Decoder/Gate embedded identity 等于该提交；正式身份
复核定向测试，再用 fresh GUID 按 64 MiB → 500 MiB → 1 GiB 跑右屏 15 Hz，失败即停后续档位。当前 G21 仍
PARTIAL，以上不是 actual-pixel 或 remote pass；独立 live false-accepted oracle 仍为 null/unavailable。

### 0.10 bf52b24 实屏 64 MiB 与 compaction 放大修正（2026-09-06 22:00 CST）

0.9 的正式代码提交为 `bf52b24f2b3c95368b8de382fe50f59215e387db`。提交后重新 configure/build Encoder、Decoder、
Gate、Application、scheduler、transition probe，三个 EXE embedded identity 均匹配；formal narrow tests 全通过，
clean/phase3 transition 再次通过。formal evidence 在 0.9 根下 `formal-bf52b24/`（含 commands、binary hashes、
source snapshot、summary）；本次未跑 full CTest/ASan/GPU matrix/20 GiB。

该身份的右屏 LocalDesktop actual-pixel 64 MiB 权威根：

```text
<repo>\build-unified-release\g21-bf52b24-staircase-64mib-3ea04901a15e4fc696494872bbbdd1c2
```

重新枚举右屏 DISPLAY2，RECT `[2560,0,5120,1440]`，2560×1440、96 DPI；receiver-first、WGC、fresh CSPRNG、
immutable source lease、15 Hz。8/8、67,108,864 bytes、elapsed=291,492 ms、3,822 unique、UniqueVisualFPS=
13.244329484265329、17,558.572475143905 B/unique。WholeFileDigest、安全发布、final reopen、frame coverage、无
part/resume 和 lane CRC/identity 门全通过；resource/conflict/deferred/quota/orphan=0，进程正常退出 0/0。
Sender 4,334 submitted frames，configured=15、submitted=14.985983623005827 Hz。Receiver peak working set/private=
402,628,608 / 608,464,896 bytes；Encoder=276,795,392 / 301,895,680 bytes；decoder-owned peak=8 / 457,201,696 bytes。
独立再次读取 source/published exact bytes、SHA-256 `b712664112bbaec4ae87021f05288d097dfb21c7c57597bbcb53b364c67ff458`、
BLAKE3 `dd310e611c4ee7f48728592f92dac904a2f7707df5d2ba87cfd9679680556b98` 全相等，详见
`independent-postrun-audit.json`。独立 live codeword oracle 仍 null/unavailable。

**不能据此升级大档：** `encoder-evidence.jsonl` 第一条 pass1 为 frameSequence=4,087/cyclePosition=15，
而同轮接收快照在该边界前仍 **0/8 completed、active=8**；后续 pass1 才补完。时钟关联方法和原始快照保存在
`pass0-safety-postrun-analysis.json`。因此 64 MiB 本身发布/硬门 PASS，但不满足跨窗口安全，**500 MiB 与 1 GiB
没有启动**；不能把它们写成运行失败，也不消耗长跑重现已经暴露的问题。

新定位的独立可复现 primitive 在 `DecoderResumeStore::Checkpoint`：旧逻辑只要 journal 总长超过 16 MiB，
每秒 checkpoint 就对仍存活的大窗口快照重写，即使没有新增 block。实屏 3,198 unique / 44,371 accepted Transport
已产生 generation=4,100,234，最终约 614 万。新增合法 Wirehair journal 回归在空 checkpoint 复现 generation
2,054→3,081；首个无效 DirectRepeat fixture 的失败也保留，随后用 canonical Wirehair fixture 定位真实分支。

本段同提交的最小修正只把 periodic compact 改为 **validated open / 成功 compact 后新增 16 MiB**，不变更
scheduler/phase/repair/SC6 V3/Receiver quota。1 秒 append+flush、torn-tail compact、completed Segment 立即 compact、
CRC/generation、atomic replace/final reopen 与 maxResumeBytes 不变。新增回归覆盖 idle、少量 append、第二个 16 MiB
growth 仍触发 compact、restart、completed 后释放。G21+journal=10 cases / 2,630 assertions；所有 Decoder resume
相关例=8 / 2,503，含现有 native rename reader、corruption、quota、final-reopen crash-window，相应日志在 0.9 根的
`compaction-growth-valid-before.txt`、`compaction-growth-final-g21-resume.txt`、`compaction-decoder-resume-adjacent.txt`。

这是消除重复持久化开销的证据，不声称它已解释或消除全部 capture loss。下一步提交此候选、重构建身份、复核窄测和
16-Segment probe，再重新开始 64/500 MiB/1 GiB 同身份阶梯；若 64 MiB 仍靠 pass1，先继续修正，不盲跑下一档。
保护文件 SHA-256 未变；测试结束没有自己启动的 Encoder/Decoder/Gate 遗留。G21 仍 PARTIAL、G22 未开始。

### 0.11 21b3591 实屏复核与位精确 Bootstrap 热路径（2026-09-06 22:20 CST）

0.10 的正式提交为 `21b35913f0c3a6a3a3342f2a33715b36f5d263fd`，提交后完整重构建三 EXE 身份与全部 formal
窄测通过，证据在 0.9 根下 `formal-21b3591/`。clean/phase3 transition 仍通过。

本候选实际右屏 64 MiB root：
`<repo>\build-unified-release\g21-21b3591-staircase-64mib-8985c739d1964bd182501ec3c20d7eff`。
8/8、67,108,864 bytes、279,568 ms、3,748 unique、13.547948201896428 Hz、17,905.24653148346 B/unique，
whole/publish/reopen/coverage/外部 exact bytes+SHA256+BLAKE3 均通过；lane CRC/identity、resource/conflict/deferred/
quota/orphan=0，exit0/0。Sender=4,161 frames @14.983885481307 Hz（配置15）。Receiver peak working set/private=
396,500,992 / 520,880,128 bytes；Encoder=276,525,056 / 303,112,192；decoder-owned 8 / 457,201,696 bytes。
source/published SHA256=`754db279513b1f8ac207229604813fa755127a02a00832d39bc01d2f7de011c9`，
BLAKE3=`a5ea89d16a395cc2e01cb31c7120749415be92181d94a4dd0a620367a293882d`。

`pass0-safety-postrun-analysis.json` 仍显示 pass1 前 0/8、active8，后续才完成；因此 500 MiB/1 GiB 仍未启动。
与前轮相比，最终 generation 降至 350,489、capture admission drops 由76降到2，证明重复 compact 已消除；但这不是
全部 capture loss 的解释。当前每帧 Bootstrap CPU（含 staging Map）约36 ms，是下一个可优化热点。

本段同提交只优化 `LumaReader::ScanPixel` 的 BGRA full-view 热路径：把不请求 clipping 的扫描读取从含 FP16/pow
的通用函数中分离，原 `Pixel` reference/其他格式 fallback 保留。**所有三轮阈值扫描、所有像素、marker/geometry
顺序、ambiguity/contrast/timing/CRC/work budget 均不变**；没有 cached-ROI 快捷接受、没有丢弃 unique frames，也无
image-sized scratch 或 allocation。两种 luma 算法分别穷举全部 16,777,216 RGB 代码，double bits 与 reference 完全
相同，并检查 row padding、work charging、输出不变与 bounds/budget failure。

相同32帧 2560×1440 离线 Golden full-view probe：原扫描21.078 ms/帧，新扫描10.2191 ms/帧；total work units 均为
185,997,856。这里只是同夹具 CPU 成本，不冒充 live 时延或 goodput。证据在 0.9 根的
`bootstrap-scan-reference.txt`、`bootstrap-scan-optimized.txt`：2 cases/2,503 assertions；
`bootstrap-scan-all-local-gates.txt`：17 cases/3,900；`bootstrap-scan-no-allocation.txt`：1,070 checks、0 allocations。
现有不同格式、torn/mixed frame、threshold、ambiguity 和 budget tests 全通过，未放宽任何门。

新候选仍需独立提交、重构建、正式 identity/窄测复核，并重新从64 MiB开始同身份三档。保护文件 hash 未变；没有残留
产品/Gate进程。G21 PARTIAL，所有本机结果明确为 LocalDesktop，仍不是 remote field pass。

### 0.12 2861beb 两档 Pass-0 通过；1 GiB coverage 失效与首次失败诊断（23:35 CST）

正式候选 `2861beb4affe1a37ea386dafdc52e27d7a1d6498` 已重构建并核对 Encoder/Decoder/Gate embedded identity。
0.9 开发根下 `formal-2861beb/summary.json` 保存 scheduler、Application G21、原 sparse window、durable/resume、
Bootstrap/no-allocation、Gate self-test 与 clean/phase3 16-Segment probe 全部 PASS。以下三档没有夹入其他提交，
同一 HEAD、相同 hold32 sweeps/repair1/10+32/Control triplet、右屏全物理 ROI、Unified 15 Hz；每档 fresh CSPRNG。

| 档位 | bytes / Segments | elapsed ms | unique / UniqueVisualFPS | B/unique | Sender frames / submitted FPS / pass |
|---|---|---:|---|---:|---|
| 64 MiB | 67,108,864 / 8/8 | 254,198 | 3,720 / 14.807471230450075 | 18,040.017204301075 | 3,777 / 15.003861944793432 / 0 |
| 500 MiB | 524,288,000 / 63/63 | 2,121,518 | 31,590 / 14.924181227410648 | 16,596.64450775562 | 31,740 / 14.990317266475468 / 0 |
| 1 GiB FAIL | 1,073,741,824 / 8/128 | 首个失败 sample 约 254 秒；无正常 final | 3,714 / null | null / NotPublished | 3,856 / 15.00383556 / 0 |

权威根依次为：
- `<repo>\build-unified-release\g21-2861beb-staircase-64mib-810d78174c9a4200a7a80298e7dba985`
- `<repo>\build-unified-release\g21-2861beb-staircase-500mib-655711d69c3d4e588eab6c5c1455df04`
- `<repo>\build-unified-release\g21-2861beb-staircase-1gib-90ca448d3b274fd5b0bde650c0c36bc0`

前两档 whole/safe publish/final reopen/complete current-run frame coverage、外部 exact bytes/SHA256/BLAKE3、
lane CRC/identity=0、resource/conflict/deferred/quota/orphan=0、无 .part/.resume、exit0/0 全通过；均在 Pass0 发布。
`independent-postrun-audit.json` 与 `pass0-completion-check.json` 保存逐项检查，不把 32 KiB 工程目标写成已达到。
64 MiB SHA256=`bc4dcb4f62340b28fe459d5d94828595a62832f7ee091b2f7fece80ab6b822be`，
BLAKE3=`763236bb1c6aa717ba5f87a6b245e93a2126df1e6dac40216bca8362e45c7df1`；
500 MiB SHA256=`dfc7a0fd30a0ff75ae6e93f8ded928328539b9017fda55cea6d3a87df7e0cffd`，
BLAKE3=`ed5229aea731cdfb9ffc1f15987713b2f7cf67511898a7b048fb9018ff0ee9ee`。

内存峰值（working set / private bytes）：
- 64 MiB Receiver 399,753,216 / 614,559,744；Encoder 258,596,864 / 302,145,536。
- 500 MiB Receiver 402,165,760 / 567,963,648；Encoder 277,041,152 / 319,688,704。
- 1 GiB 失败前 Receiver 400,310,272 / 561,696,768；Encoder 260,386,816 / 319,963,136。
三档 decoder peak active=8，reserved=457,201,696 bytes；500 MiB 未表现为 O(total file) 的 resident 增长，
但 1 GiB 未完成，不据此宣称全部大文件内存认证。

1 GiB 在第一窗口完成附近触发 fail-fast：`receiver-gate-failure-snapshot.json` 为 Verifying、8/128、
verifiedRawBytes=67,108,864、active=0，所有 lane/resource/conflict/deferred/quota/orphan=0，但
`frameCoverageComplete=false`。Receiver 因 supervisor 失败退出 -1/forced=true，Encoder Q 正常退出0；
原 .resume/.part 保留为失败证据，不算残留清理成功。无 published final，无外部最终文件摘要审计。
`coverage-failure-boundary-analysis.json` 冻结原始文件 hash：samples[20] coverage=true、3,546 unique、0/128；
12,135 ms 后 samples[21] coverage=false、3,714 unique、8/128。counterOverflow=false，尚未达到4096身份窗口。
旧二进制不导出首次失败原因，不能从这个采样间隔直接断言时序倒退的具体来源。

本节同提交先加入固定大小 `firstCoverageFailure`（原因、失效前 observation/unique 数、FrameSequence、epoch、
当前/此前 timestamp），只记录首次；**不改任何计数、覆盖规则、接收时序或硬门**。窄测 telemetry 9/8,345、
Application G17+G21 13/2,744 通过，含 malformed sample 原因、旧帧拒绝原因、timestamp regression 与首次证据不可覆盖。
下一步正式提交重构建后做有界实屏诊断，取得首因再修复；修正后必须重新从64 MiB跑同身份三档，不能拼接旧档。
G21 PARTIAL；LocalDesktop 不是 remote field，独立 live false-accepted oracle 仍为 null/unavailable。

### 0.13 同一最终候选三档 LocalDesktop 通过与远控复验准备（2026-09-07 01:55 CST）

**当前正式实验代码为 `6e9064319a51bedcd403d74d47dd45c067928013`。今晚优先的 64 MiB→500 MiB→1 GiB
15 Hz 右屏 actual-pixel 阶梯已全部通过，同一代码/二进制/调度参数，且全部在 Pass0 发布。G21 仍为 PARTIAL：
没有执行该候选的带远控因素现场复验，不能把 LocalDesktop 成功写成 remote field pass，G22 前置尚未满足。**

本节是纯文档收口：它之后 HEAD 可以前移，但冻结的 Encoder/Decoder/Gate identity 仍应为 `6e90643`，不是代码落后。
任何后续产品代码/测试候选仍需独立提交、configure/build、身份核对，不能与本节三档 evidence 拼接成同身份通过。
旧 `2861beb` 的一次 coverage=false 保留在0.12；`6e90643` **只加诊断，没有修改 coverage/计数/接受规则**。
本次三档 `firstCoverageFailure=null`、coverage=true，只能说原异常未复现，不能说根因已经修复。

#### 三档最终权威值

| 档位 | 精确 bytes | Segment | Receiver elapsed ms | unique frames | UniqueVisualFPS | VerifiedEncodedBytesPerUniqueFrame |
|---|---:|---|---:|---:|---:|---:|
| 64 MiB | 67,108,864 | 8/8 | 250,754 | 3,700 | 14.921582942802349 | 18,137.53081081081 |
| 500 MiB | 524,288,000 | 63/63 | 2,121,922 | 31,630 | 14.936724736545933 | 16,575.6560227632 |
| 1 GiB | 1,073,741,824 | 128/128 | 4,328,785 | 64,577 | 14.938359707725137 | 16,627.31040463323 |

三档共同成立：WholeFileDigest=true、safe publish=true、final reopen=true、frameCoverageComplete=true、
全部 Segment 完成、外部 exact bytes/SHA256/BLAKE3 相等、lane CRC/identity failures=0、
resource/conflict/deferred/quota/orphan 全0、无 .part/.resume、无 receiver error、Receiver/Encoder exit=0/0，
没有 deadline/强杀；峰值 active decoder=8、reserved decoder bytes=457,201,696。
16,384 B/unique 硬门全过；32,768 B/unique 工程目标均未达到，不把工程目标改成通过。

Sender configured=15 Hz；三档 submitted frames / submitted FPS 分别是：
`3,739 / 15.003677845770426`、`31,766 / 14.99078004284591`、`64,816 / 14.990193138810495`。
它们不是 Receiver UniqueVisualFPS。固定参数仍为 SC6 V3/layout10、W8、hold32 sweeps（256 logical frames）、
step1、Pass0 `max(16,ceil(K/10))+32`、启动四份交织、10秒周期一个 Session/Manifest/current Segment triplet。
未扩大任何资源策略，未按 provider 分支，未主动删有效 unique 或改性能分母。

内存峰值单位 bytes，来自1秒采样；每档 Receiver / Encoder 的 working set 与 private bytes 分别是：

| 档位 | Receiver WS | Receiver private | Encoder WS | Encoder private |
|---|---:|---:|---:|---:|
| 64 MiB | 399,527,936 | 551,387,136 | 258,568,192 | 319,152,128 |
| 500 MiB | 400,359,424 | 611,368,960 | 277,176,320 | 320,421,888 |
| 1 GiB | 400,695,296 | 691,380,224 | 277,323,776 | 320,622,592 |

源码定向核对 `StoreCompleted`→durable journal→`CommitStoredSegment`→codec destruction，已完成 payload 被释放，
只保留 descriptor/完成 bitmap 等 metadata；decoder-owned 峰值恒为8窗口。总文件增长16倍时 working set基本稳定，
没有发现持有全部 completed payload 的路径。但 private high-water 跨 fresh runs有所增长；现有 peak-only采样
不是完整 allocator 时间线，不能宣称 private曲线完全平坦，也不能替代>=20 GiB认证。审计边界见开发根
`memory-ownership-review-6e90643.json`，不把 OS private peak 直接等同于 decoder-owned reservation。

#### 原始 evidence 与双摘要

根目录依次为：
- `<repo>\build-unified-release\g21-6e90643-staircase-64mib-8feb223231a84e828dab5c4e73e0c954`
- `<repo>\build-unified-release\g21-6e90643-staircase-500mib-c13ee15e64c84925b896402deef3c3cb`
- `<repo>\build-unified-release\g21-6e90643-staircase-1gib-4b966e20a4d748d39c8816387065ebbf`

每根保留 `source-manifest.json`、`encoder-report.json`、`encoder-evidence.jsonl`、`receiver/final.json`、
`receiver/receiver-checks.json`、`receiver/samples.jsonl`、`external-digest-audit.json`、
`independent-postrun-audit.json`、`staircase-verification.json`、`process-exits.json`、`pass0-completion-check.json`。
三档 source/published 的 SHA256、BLAKE3：

- 64 MiB SHA256 `1570ae31701f1484e6f22ac7792110ed07b3a2f0553f9d9b851b06001ab185e9`；
  BLAKE3 `f7c84a668e894ceadc69c46b2d556d86802c76e877dd9eab3cf6fd466f013977`。
- 500 MiB SHA256 `6516bfda2cf60b00db5dda13e7fd2198cee164be6dd8bb942b2743b52220d64d`；
  BLAKE3 `a54fb4e5d83bb800a48e412c2ca9aa1b12606eacb2ad08bce69456816b2cedad`。
- 1 GiB SHA256 `0c9d4154c0391a8d88c0fea4c24c9badf1acd1f8b7c51b3d8c06582b2dd45dc0`；
  BLAKE3 `f07dfdd2ee42e5df77d123a88baee4ed25b8205058885adc21e9fbcddc610fe5`。

所有 run 重新枚举并证明 DISPLAY2=`[2560,0,5120,1440]`/2560×1440，DISPLAY1受保护；下次仍须现场枚举。
Receiver先启动，命令只含 `--receive <fresh receiver root> <deadline>`，无source路径/摘要/SessionId/sender oracle。
source通过OS CSPRNG流式生成，FileShare.Read lease从Receiver启动前保持至两端退出；payload仅来自右屏可见像素。
独立post-run audit在完成后重读两份文件，不使用sender摘要替代实际文件hash。

完整汇总、每份 evidence hash与三 EXE hash不变核对：
`<repo>\build-unified-release\g21-pass0-transition-d3d6e5ba640d4305b161a2e467b70896\same-candidate-6e90643-staircase-audit.json`。
冻结 Encoder SHA256=`f63f7773b8de829da6536664e4467abbf69670b8dc4a56ea53452cc4d49caaf0`；
Decoder=`8321da971527ebc9939bc31441636aaea9a56263e7f96df378c3bd37591c549c`；
Gate=`96183eb7a50446572d3227c6e491ced3d5c89cd4f0bfdae3d06741cea78fe6d4`。

#### 精确验证入口与预算

`formal-6e90643/summary.json` 记录全部命令、deadline、退出码、stdout/stderr：

```powershell
$build = '<repo>\build-unified-release'
& "$build\tests\PBApplication\Release\PBUnifiedSenderSchedulerTests.exe" --rng-seed 21092026
& "$build\tests\PBApplication\Release\PBApplicationTests.exe" '[application][g21],[application][report][g17]' --rng-seed 21092026
& "$build\tests\PBTelemetry\Release\PBTelemetryTests.exe" '[telemetry][g17],[telemetry][g21]' --rng-seed 21092026
& "$build\tests\UnifiedRemoteGate\Release\PBUnifiedRemoteGate.exe" --self-test
& '<python>' '<repo>\tests\UnifiedRemoteGate\run_g21_transition_probe.py' '<NEW_ABSOLUTE_PROBE_ROOT>' --modes clean 3
# 必须从交互 console / Codex tty:true 启动，三个 root 各为新 GUID，依序且前档全过后才继续：
& '<repo>\tests\UnifiedRemoteGate\Run-G21LocalStaircase.ps1' -Tier 64MiB -NewRunRoot '<NEW_64MIB_ROOT>' -MaximumSeconds 1800
& '<repo>\tests\UnifiedRemoteGate\Run-G21LocalStaircase.ps1' -Tier 500MiB -NewRunRoot '<NEW_500MIB_ROOT>' -MaximumSeconds 4200
& '<repo>\tests\UnifiedRemoteGate\Run-G21LocalStaircase.ps1' -Tier 1GiB -NewRunRoot '<NEW_1GIB_ROOT>' -MaximumSeconds 7200
```

结果：scheduler10 cases/247,055 assertions；Application13/2,744；telemetry9/8,345；Gate self-test PASS；
16-Segment clean/phase3均PASS，未靠第二Carousel pass。旧阶段八phase、crash-safe/durable/compaction/Bootstrap
位精确与无分配邻接证据分别保留在0.9～0.11；不把旧身份测试冒称为本次重跑。没有额外full CTest、ASan、GPU
矩阵、20 GiB或新真实远程运行。现有Base-only实际capture闭环沿用其独立authority，没有在本轮重哈希/重放3.8GB。
独立live false-accepted oracle继续`null`，reason=`No independent sender truth supplied to receiver`。

#### 最小远控复验包（prepared，不是 remote PASS/G22 release）

准备根：
`<repo>\build-unified-release\g21-6e90643-remote-ready-9fcfe8767919473badec2859e9154af1`。

- `PixelBridge-G21-RemoteEncoder-6e90643-15Hz.zip`：26,166,009 bytes，SHA256
  `b1a0835a8ffe5b24c5b2621b605a7e66f73746d07acac4aebd986831b696b998`，39 entries。
- `PixelBridge-G21-LocalReceiver-6e90643-15Hz.zip`：1,022,995 bytes，SHA256
  `53bff51163e1c39b5ec972d2734570fe7922463e7390da1e9b4c7a1f1faa1655`，8 entries。
- `FIELD_RECHECK_README.md` 给出最小顺序：远程完整解压`00_Check.bat`；右屏连接/完整摆放远控窗口；
  本机先`Start-G21Receiver.ps1 -Stage smoke`；远程再`01_Start_Smoke_1MiB.bat`；全部Gate/外部审计通过后同序full。
  full为64 MiB/15Hz/1800秒，smoke1 MiB/600秒；Q/Enter正常停sender，超时绝不是成功。
- sender入口基于本提交tracked脚本派生，差异保存在`sender-entry.diff`；延长full期限、持有source只读lease、
  停止后独立读取source双摘要。`Get-G21FileDigests.ps1`使用锁定文件流、1MiB buffer、.NET SHA256和hash/版本/ABI
  固定的BLAKE3 C API，不复制sender-report摘要，不需要远程Python；不得给运行中的Receiver传source或审计JSON。
- 两包逐entry bytes/hash/无重复与越界路径核对、新目录解压后PowerShell5.1 CheckOnly/identity通过，无run/source/window。
  helper的empty/abc/2,056,443-byte跨buffer fixture与Python双摘要一致，也重读了已结束的1GiB published file一致；
  错DLLhash、既有输出、篡改Encoder均拒绝且没有新source。结果见`preparation-verification.json`、`checks/`。
- 新wrapper只验证了CheckOnly和独立digest helper，没有在真实远程端执行live orchestration；不得宣称目标机、网络、
  远控窗口或G22 SBOM/LICENSE/发行门已验证。主产品源码/二进制没有为组包而修改。

当前没有本任务启动的Encoder/Decoder/Gate残留；所有旧失败与新成功artifact都保留。受保护文件SHA256仍为
`076EF4C9B9F89EABCCD323DBE4BFFC4DC125DDAF96E6EE437D2CF5B1B1CEA306`，只它未跟踪、从未修改/暂存。
下一步是用户安排真实远控场景后，以冻结候选执行smoke/full与独立外部审计；若coverage失效，先读首次失败原因，
不能以本次未复现抹掉旧失败。G21真正满足退出标准并独立提交后继续G22，而不是因为本机阶梯完成就提前关G21。

### 0.14 用户要求交付收尾；包装脚本纳入版本管理（2026-09-07）

用户在本机三档完成后明确要求完成交付、提交代码、整理目录并结束本次任务；不再开启新的长时或现场运行。
G21 保持 PARTIAL，真实远控复验与 G22 留待后续单独安排，不能用任务收尾代替 Gate 完成。

收尾代码只涉及 `tests/UnifiedRemoteGate` 包装层，不修改产品调度、协议、接收、telemetry 或性能门：

- 将此前包内的 Receiver 入口与独立双摘要 helper 纳入源码；Sender 持有只读 source lease，full 期限1800秒。
- Receiver 在 observed coverage=false、counter overflow 或九项 resource/conflict/deferred/quota/orphan 非零时，
  保留失败 snapshot 并有界停止；初始 observationAvailable=false 不误杀。增加有界1秒内存 JSONL。
- Sender 在运行 post-stop digest helper **之前**持久化 Encoder exit；helper 失败不丢退出证据。
- Receiver finally 显式区分未启动 Process，并记录 started=false / receiverExit=null；输出 drain/cleanup 有界。
  本机 PS5.1 AST 实测旧/新 finally 都保留原始启动异常；旧 getter 掩盖异常的推测未复现，不称为已修复旧故障。
- 新增 .NET 进程 fixture 与 Python 定向回归，身份固定 `g21-lifecycle-fixture`，不捕获、不呈现、不传输 payload。
  正式收尾20项已覆盖正常/等待/partial JSON/exit7、coverage、overflow、九项资源计数及旧失败复现，全部PASS。

V1 包与所有原始 evidence 保持原位。新的 V2 准备根为
`<repo>\build-unified-release\g21-6e90643-remote-ready-v2-31405053c7db4979ac6a3dc85de05204`。
冻结的三档 authority 仍只属于6e90643；后续仅工具提交后的新构建身份不冒充重跑实屏。
最终交付路径、包hash、正式定向结果与目录索引见下方收尾补记，不以夹具测试宣称remote field PASS。

首轮 lifecycle-formal 保留了一项测试预期错误：误以为旧 PowerShell property getter 会掩盖异常，实际并未发生。
已按实测修正断言，继续同时检验新旧异常保留以及仅新版产生 nullable exit 记录，不删除任何失败路径覆盖。

#### 最终交付补记

- 工具/夹具提交为 `9fcbed27827d0c55bb3776bad81802b4ce202ed7` 和 `959678340d1946fed4fec01b4410a250b533daa3`。
- 在9596783后重新configure/build Encoder、Decoder、Gate，三EXE embedded identity全部等于9596783，Gate self-test PASS。
  **当前build树已不是6e90643；没有重跑新identity的实屏。** 三档通过的6e90643 Encoder/Gate冻结在V2包，
  原Decoder完整runtime冻结在V2 `validated-local-decoder`。不能拿当前build树冒充旧候选现场证据。
- 正式无屏幕fixture `checks\lifecycle-9596783\summary.json` 为20/20 PASS；先前路径过长和错误预期的开发失败保留。
  ZIP逐entry、新目录解压CheckOnly、独立helper三种长度/双摘要、错DLLhash/既有输出/篡改Gate负例全部PASS。
- 统一交付目录：`<repo>\artifacts\g21-delivery-2026-09-07`；跟踪索引为
  `<repo>\docs\UNIFIED_G21_DELIVERY_2026-09-07.md`，包含精确命令、峰值、authority与原始root。
- V2 Sender ZIP：26,166,428 bytes /39 entries，SHA256
  `018abb6c7ccd8dd83c42dcfb31dcb2da7f5ca354d01ea9b5bce04ca1210d1451`。
- V2 Receiver ZIP：1,024,293 bytes /8 entries，SHA256
  `fc5fbd863b586036925d7326e9edfafb22fbb4181b7374523c435b58c6ab5760`。
- 源码工具集中在 `tests/UnifiedRemoteGate`，新增README分清actual-pixel/synthetic/remote准备边界；更新项目/文档索引中
  过时的500MiB、Base-only缺项描述。旧build/cache/原始evidence未移动或删除，历史对象文件不擅自认领。
- 受保护文件仍保持原SHA256、只它未跟踪；收尾无本任务产品进程残留。不新增后台监控、远控场景或长时测试。

本次按用户要求交付后结束；G21仍PARTIAL、G22未开始。最终仓库状态另封存在开发根 `task-closeout-final.json`，
不修改已经封存的三档报告或交付manifest来追写新HEAD。

### 0.15 最终 Windows 远程桌面 1 GiB 恢复与单次用户豁免（2026-09-07）

本节取代 0.14 中“远控仍待现场、G21仍PARTIAL”的**当前状态**，但不改写其历史事实。用户使用 v5 包完成
`00_Check`，本机 Receiver 先启动并只捕获完整 `\\.\DISPLAY2`，远程 Encoder 随后以
`--logical-fps 15 --manual-stop --loop`、无自动 deadline 呈现精确 1 GiB。Windows 远程桌面自身 FPS 未知，
不使用远程显示器 refresh rate 冒充 RDP FPS。

权威身份和结果：

```text
Product source commit: 6e9064319a51bedcd403d74d47dd45c067928013
Gate/tool embedded commit: 959678340d1946fed4fec01b4410a250b533daa3
Sender RunId: 132e1a54d2134d2e828593dbeff210da
Receiver RunId: 2c4f578663c0b3762bb0396acb2021ab
SessionId: daba04b1c7c8c22a31604ea68dd61f8f
SessionTag: 15447616161310190557
bytes / Segment: 1,073,741,824 / 128 of 128
SHA256: e6ec3a7f5643f7b04ca5b90ce9510fb8388410b562b329fb70fe4cb837a0d323
BLAKE3: db460e2c8a260f885f3a8a1b0d4d47a5d04f9d74e648c8ce4a44626b03185063
```

Receiver `final.json` 为 Completed，WholeFileDigest、安全 rename、final reopen、published、完整 current-run coverage
全部成功，`.part/.resume` 清零；所有 lane FEC/CRC/identity、真实 resource rejection、conflict、orphan drop/exhaustion
均为 0。配对且有界的 `DeferredResourceBusy/OuterFecQuotaExceeded` 为 `970,220/970,220`，8-slot 峰值和
457,201,696 bytes 预留从未越界；后续 Carousel 完成全部 Segment，因此
`eventualRecoveryPassed=true`、`strictPass0ZeroPressurePassed=false`。Sender 在 Receiver 结束后由用户按 Q/Enter
正常停止，Encoder exit 0；post-stop source 双摘要与本机重新完整读取的 published file 精确相等。source 未复制给
Decoder，独立 live codeword oracle 仍为 null/unavailable。

原始 Gate 不能改写：Receiver 观察 124,470 unique frames @ 13.753410 Hz，得到
`8,626.510998634209 B/unique`，低于 16,384 硬门；`failure.txt` 只记录该性能失败，Receiver Gate exit 1。
跨机审计如实为 `functionalRemoteOneGiBPassed=true`、`hard16KiBFrameMetricPassed=false`、
`documentedG21GatePassed=false`。

在这些数值、退出码和既有标准全部向用户披露后，用户明确选择“**仅本次明确豁免**”。因此 G21 最终状态为
`PASS_WITH_SINGLE_RUN_USER_WAIVER`，G22 前置已解除但尚未开始。该豁免只绑定上述 Run/Session：不改原始 evidence、
不把 exit 1 写成 exit 0、不改分母/协议/产品代码/provider 分支，且未来运行的 16 KiB/unique 硬门继续有效。

完整结果、复核步骤和边界见
[`UNIFIED_G21_REMOTE_1GIB_RESULT_2026-09-07.md`](UNIFIED_G21_REMOTE_1GIB_RESULT_2026-09-07.md)。机器可读证据：

```text
<repo>\artifacts\g21-remote-1gib-2026-09-07\remote-run-cross-host-audit-132e1a54d2134d2e828593dbeff210da.json
SHA256: 23d45347a42e923148ab3975ebef5277057d4895bd731a083209d8df24a423eb

<repo>\artifacts\g21-remote-1gib-2026-09-07\g21-single-run-waiver-132e1a54d2134d2e828593dbeff210da.json
SHA256: d45e9b4ecebc50fdee65e782835a7edb6aabe609c2581e463ea4e1fde01c78da
```

## 1. 交接时结论

- G00..G20 已有独立提交并按路线记录完成；当前目标仍是 **G21**，G22 未开始。
- 当前唯一产品视觉合同是 `PB-Unified-SC6-V3`、VisualProfileId `0x5042554E49534333`、layout 10、
  1920×1080 canonical canvas、6×6 separated symbols。LC4、SC6 V2 与旧 LF4 仅为历史回归。
- 当前代码修复基线为 `433bccf42df29c54ae326f0e01296ab22a8258ef`
  （`fix(resume): retry transient decoder journal replacement`）。后续任务须以 `git rev-parse HEAD` 核对本文之后的
  交接文档提交，不得仅相信本文中的旧 HEAD。
- 真实未知远控链上的最终 64 MiB CSPRNG/RAW 已完成 **8/8 Segment、WholeFileDigest、安全 rename、final reopen、
  外部 SHA-256/BLAKE3 双摘要**；Decoder 没有拿到 source 或摘要 oracle。
- 该次 `PBUnifiedRemoteGate` 仍 exit 1：不是文件恢复或发布失败，而是
  `ReceiverChecksPassed()` 要求 `outerResourceRejections == 0`，现场启动阶段累计了 416 次后稳定不再增长。
  因而留下 `failure.txt`，G21 不能直接写成完成。
- `VerifiedEncodedBytesPerUniqueFrame = 17,796.039246884116`，已高于 16 KiB/unique 硬门；没有达到 32 KiB 工程目标。
  路线 G21 明确允许未达 32 KiB 的首版，但必须记录，不能将工程目标误说成 exit 1 的原因。
- Base Luma 独立恢复证据仍缺；独立 false-accepted codeword oracle 仍为 unavailable。当前最终文件字节正确、
  lane CRC/identity failure 与 Outer conflict 均为 0，但这些不能冒充独立 codeword oracle。

## 2. Git 与工作树安全状态

交接前最后核对：

```text
HEAD before handoff-doc commit: 433bccf42df29c54ae326f0e01296ab22a8258ef
git status --short: ?? docs/PHASE1_GATE_REPORT.md
docs/PHASE1_GATE_REPORT.md SHA-256:
076EF4C9B9F89EABCCD323DBE4BFFC4DC125DDAF96E6EE437D2CF5B1B1CEA306
```

`docs/PHASE1_GATE_REPORT.md` 是用户所有的未跟踪文件：不得覆盖、删除、暂存或顺手纳入提交。继续使用显式路径
`git add -- path...`；不得 broad-stage、amend、rebase、reset 或 push。

## 3. 最终真实远程 64 MiB 证据

### 3.1 同身份交付

完整 Encoder ZIP：

```text
C:\Users/<user>\Desktop\PixelBridge-G21-RemoteEncoder-433bccf-SC6V3-Final64MiB-Full.zip
ZIP SHA-256: 555068278780e4a9ad57abe6e22634541fbd0225400ed28262685f5f9cdec67e
Encoder SHA-256: 053b13ba545f315635117802e868fe9a8483eedd78e64723f0d23ce9783a06d1
Encoder/Decoder commit: 433bccf42df29c54ae326f0e01296ab22a8258ef
```

远端 `00_Check.bat` 回报通过；检查没有生成 source、run 目录、窗口或传输。用户随后只运行
`02_Start_Full_64MiB_15Hz.bat`。本机 Receiver 先启动并只捕捉 `\\.\DISPLAY2`。

### 3.2 权威运行根与配对结果

```text
<repo>\build-unified-release\g21-sc6-v3-live-final-64mib-1\
  full-64mib-15hz-a4ae1abe75b942e9be3517ddb73e70c8\
```

关键文件：

- `receiver/final.json`：权威终态，`state=Completed`；
- `receiver/failure-final.json`：Gate 后置检查抛出后再次封存的相同 Completed snapshot；
- `receiver/failure.txt`：仅记录 Gate 对 resource rejection 的后置失败；
- `remote-source-manifest.json`、`remote-encoder-report.json`、`remote-process-exit.json`：用户回传原件的只读封存副本；
- `paired-publication-audit.json`：严格身份、发布和外部摘要配对，SHA-256
  `9c0de74228ec5c39f5343a3136482e2ba9e1effa69d138ace7398b9c7d714810`。

严格配对身份：

```text
Sender RunId:   42618932829c435e8f1bb3ba98989dc3
Receiver RunId: 7f184af71b73f1f1f277fe66ff92ed00
SessionId:      cc55f8afc83400af586a792bcac02665
SessionTag:     8060370733226332123
source:         g21-sc6-v3-acceptance-full-64mib-15hz-42618932829c435e8f1bb3ba98989dc3.bin
bytes:          67,108,864
SHA-256:        4dd1b87cbaf6872d07a4fee05297468224661748dffcf5a72de791976ab39708
BLAKE3:         deb2bf110d067de24e457770089bc0ece89546cb6736e9639eee1f529d1ad494
```

Sender 提交 10,196 logical frames，实际平均 14.983399 Hz，用户在 Receiver 已停止后正常按键结束，exit 0。
Receiver 在约 503.6 秒后结束，结果为：

| 项目 | 结果 |
| --- | ---: |
| Verified Segment / raw bytes | 8 / 67,108,864 |
| Observations / unique logical frames | 3,840 / 3,771 |
| UniqueVisualFPS | 8.9218175573 |
| VerifiedEncodedBytesPerUniqueFrame | 17,796.039246884116 |
| Accepted Transport | 54,148 |
| Base/Fine/Chroma FEC failures | 0 / 0 / 18 |
| lane CRC / identity failures | 全部 0 / 全部 0 |
| Outer conflict / deferred busy / FEC quota | 0 / 0 / 0 |
| Capture admission drops | 47 |
| Outer resource rejections | 416 |
| Active decoder peak / reserved bytes peak | 8 / 457,201,696 |
| Whole digest / rename / final reopen / published | true / true / true / true |
| 残留 `.part` / `.resume` | 0 / 0 |

外部读取最终发布文件重新计算 SHA-256 与 BLAKE3，分别逐字节匹配 sender manifest 和 sender/receiver whole digest。
这关闭了先前 Decoder `.resume` 在第 6 个 Segment 后 `MoveFileExW(...)=win32 5` 的决定性故障：修正版成功越过
第 6、7、8 个 Segment，并清理 journal。

## 4. 为什么发布成功仍 exit 1

`tests/UnifiedRemoteGate/remote_decoder_gate.cpp::ReceiverChecksPassed()` 当前要求：Completed、whole digest、publish、
final reopen、无 resume/recovered-after-publish、`outerResourceRejections == 0`、无 conflict/deferred/quota、无 error。
本轮唯一不满足的字段是 `outerResourceRejections=416`。

`receiver/samples.jsonl` 的转换点提供了重要定位证据：

| outerResourceRejections | active decoders | unique frames | accepted Transport | verified Segment |
| ---: | ---: | ---: | ---: | ---: |
| 0 | 0 | 0 | 0 | 0 |
| 86 | 6 | 51 | 722 | 0 |
| 281 | 7 | 108 | 1,521 | 0 |
| 386 | 7 | 160 | 2,301 | 0 |
| 416 | 8 | 212 | 3,029 | 0 |

当第 8 个 decoder 建立后，计数在余下约 3,559 个 unique frames、全部 8 Segment 恢复和发布期间保持 416。
这强烈指向“Control/SegmentDescriptor 尚未把 W=8 全部绑定时，其他 Segment 的 Transport 挤满有界 orphan cache”，
而不是持续内存不足；但现有聚合字段没有保留每次错误码/Segment，因此仍需用定向 fixture 或细分 telemetry 证实，
不能直接删掉 `outerResourceRejections == 0` 来制造绿灯。

建议最短证明路径：

1. 用 headless/no-raster fixture 重放 W=8 的真实 Control/Transport 顺序，复现 active 6→7→8 时 orphan quota drop；
2. 将 protocol `ResourceLimitExceeded`、`ResourceExhausted`、Control quota、Outer FEC quota 分开计数，避免聚合字段抹掉根因；
3. 比较两个最小方案：第一批 Transport 前提供覆盖当前 W=8 的 descriptor prelude，或使每个 Control-bearing frame 都包含
   当前窗口的完整 descriptor 集；只有证据证明无法通过调度消除时才评估 orphan quota；
4. 保持 bounded memory、ActiveSegmentWindow=8、单 owner、250 ms freshness、质量/FEC/CRC/digest/publish 门不变；
5. 保留“未知 Segment 不得触发大 allocation/codec”的硬边界。

## 5. 已经踩过的坑与可复用经验

1. **构建身份必须来自实际 EXE。** 每次提交后须 reconfigure/rebuild，再运行 `--build-identity`；package HEAD 与陈旧二进制
   commit 不一致必须 fail closed。
2. **安装包必须完整、自包含、在全新解压目录复验。** 曾因只给增量包、缺 `expected-build.json`，以及用户删除整个旧目录而失败。
   不得依赖“原 remote-encoder 目录”。
3. **纯灰不是 payload。** 旧带边框窗口/DPI/任务栏使 client area 低于最小尺度后，Encoder 按合同显示 neutral matte。
   受约束的单显示器无边框全屏修复后才获得有效画面。
4. **远控失真是核心产品条件。** LC4 与 SC6 V2 均在缩放、低通、4:2:0、量化和局部旧帧混合下暴露结构性问题；
   当前 SC6 V3 使用 region-local placement、codeword-local permutation、6×6 separated symbols。不要退回只在无损本机好看的设计。
5. **configured/Present/capture callback FPS 不是 unique FPS。** 最终只用 SessionTag/FrameSequence 去重后的
   `UniqueVisualFPS`；本轮 sender 14.98 Hz、receiver unique 8.92 Hz 都必须同时记录。
6. **Control 抽帧别名会比 BER 更致命。** 按 record kind 分组的两帧 burst 曾让远控长期漏掉第二帧 SegmentDescriptor；
   后改为 Session/Manifest/Segment 交织，但 W=8 启动仍有 descriptor 覆盖窗口，正是 416 次拒绝的当前方向。
7. **后续 Carousel 不能重复 systematic IDs。** `5cedd15` 后 Pass N>0 使用 durable fresh repair IDs；不能用重复帧数冒充新方程。
8. **64 MiB 必须 W=8 条带调度。** 逐 Segment 长突发与 Receiver 活动 decoder 数不匹配会在期限内只完成部分 Segment。
9. **point sampling 的半像素量化不是裁剪。** `caba104` 只吸收严格小于半个 point sample 的边缘量化；恰好半像素、
   整像素裁剪和原质量门仍拒绝。不要简单扩大容差。
10. **Capture 必须在 acquisition 前背压。** Unified live 只允许 1 个全链在途 work，避免正确 GPU 结果因排队超过 250 ms；
    不要以增大时效门掩盖延迟。
11. **Windows 原子替换存在短暂 target-reader 竞争。** Encoder `runtime.state` 和 Decoder `.resume` 都只对同一个已 flush
    candidate 的 rename 做 25 ms、最多 11 次、250 ms 有界重试；不重写、不删 durable target、不 copy fallback、不递增 generation。
12. **成功发布和 Gate 进程 exit 必须分别解释。** 本轮 `final.json` 是 Completed，但后置 policy 产生 `failure.txt` 和 exit 1。
    不得只看退出码否认字节正确，也不得只看发布成功忽略尚未关闭的 resource/Base-only 门。
13. **source 不得进入 Decoder。** 外部摘要只在接收结束后由审计器读取 sender manifest 与最终文件；不要为方便本地测试把
    source path、digest 或 payload 通过 IPC 提供给 Decoder。
14. **证据必须 create-only。** 每次 source、run root、report、journal、外部 audit 使用新 GUID/目录；失败证据不得覆盖。

## 6. 后续 G21 工作清单

按依赖顺序推进，不得先做 G22：

1. 复现并解释 416 次启动期 resource rejection，优先修 Control/descriptor 调度根因，而不是放宽 Gate；
2. 为修正增加 parser/resource/conflict/bounded-memory 邻接测试和最小 Release 验证；
3. 建立 Base Luma 独立恢复证据。路线允许“同一实际 capture 的离线 chroma-neutralized 派生”；当前最终 remote run 没有保存
   可重放的完整像素序列，因此应在下一次本机实屏 run 中用专用测试构建有界保存实际 captured frames，再离线只中和 Chroma，
   严禁用 sender source 或重新渲染的理想 raster 冒充 actual capture；
4. 完成下述本机 15 Hz 大小阶梯；这些可验证稳定性和大文件流式行为，但不能冒充带远控因素的最终现场复验；
5. 更新 `UNIFIED_REMOTE_GATE.md`、路线 G21 状态，审查 diff，显式路径提交一次；
6. 只有 G21 exit 全部满足后才进入 G22。

## 7. 睡眠期间的本机实屏阶梯建议

用户明确允许新的目标任务在其睡眠期间使用两块本机屏幕、鼠标键盘和自动化；无需保护左屏，也无需等待人工点击。
这项临时许可仅适用于用户再次明确恢复正常交互之前。仍须避免隐藏 payload IPC，并保持证据可复现。

建议依次执行：

1. `64 MiB = 67,108,864 bytes`；
2. `500 MiB = 524,288,000 bytes`；
3. `1 GiB = 1,073,741,824 bytes`。

这里把用户口语 `500MB/1GB` 按项目一贯的二进制 MiB/GiB 记账；报告必须写出精确 bytes，不能只写缩写。

实现/运行边界：

- 不直接复用远程 ZIP 的 launcher：它固定 `primary` 且只生成 64 MiB。应先创建专用、测试范围内的本机阶梯 supervisor；
- 运行时先枚举并核对右屏仍为 `\\.\DISPLAY2`、2560×1440；Encoder 使用
  `--single-monitor-fullscreen \\.\DISPLAY2`，Receiver Gate 捕捉同一完整物理 monitor；
- 每轮先启动 Decoder、确认 run root/PID，再启动 Encoder；固定 Unified 15 Hz，source 为流式 OS CSPRNG、RAW/不可压缩，
  source lease 在 Session 全程保持不可变；
- Decoder 参数中不得出现 source、摘要、SessionId 或 sender report；完成后再由独立审计计算 SHA-256/BLAKE3/bytes；
- 每轮要求 8/8 或全部 Segment、WholeFileDigest、safe publish、final reopen、无 `.part/.resume` 残留、CRC/identity/conflict=0；
- 记录 sender submitted FPS、receiver unique FPS、每 lane FEC/erasure、resource 细分、active/reserved peak、goodput 和期限；
- 500 MiB/1 GiB 开始前检查目标卷可用空间和预计时长；只检查项目/证据目标，不枚举无关用户目录；
- 当前 Encoder 与 Receiver CLI 的硬上限均为 3,600 秒。若前一档实测投影证明下一档不能在期限内完成，先用测试证明并做
  有界上限调整；不得把 deadline、强杀或部分 Segment 写成成功；
- 一档出现 digest、发布、resource/conflict、内存增长或稳定性问题时，保留失败并先定位，不盲跑更大档；
- 本机大文件通过之后，等待用户醒来安排新的带远控因素真实复验；本机结果不得替代该复验。

## 8. 已执行和未执行的验证边界

`433bccf` 修复提交后已执行的最小 Release 验证：

```powershell
build-unified-release\tests\PBApplication\Release\PBApplicationTests.exe "[application][decoder][resume][atomic-replace][g21]" --rng-seed 21092026 --reporter console
build-unified-release\tests\PBApplication\Release\PBApplicationTests.exe "[application][encoder][atomic-replace]" --rng-seed 21092026 --reporter console
build-unified-release\tests\PBApplication\Release\PBApplicationTests.exe "[application][decoder][resume][journal]" --rng-seed 21092026 --reporter console
ctest --test-dir build-unified-release -C Release -R "^PBHeadlessMultiSegmentCheckpoint$" --output-on-failure
```

结果依次为 1 case / 23 assertions、4 / 184、4 / 243、CTest 1/1，全部通过。完整 Encoder package 原目录与 fresh
extract 的 `00_Check` 均通过；真实 remote 64 MiB 的发布/外部摘要结果见第 3 节。

本轮没有新增 full CTest、ASan、完整 GPU/corpus、Base-only、500 MiB/1 GiB 实屏、20 GiB 重跑或 G22。历史完整 Release
CTest 218/218 只属于路线记录的 `e0729b2`，不能把当前定向结果重新表述为当前 HEAD full CTest 全绿。
