# 非本机周期控制相位 A/B：候选与单组现场结果

## 0. 当前状态与范围

2026-09-08 用户已要求直接在现有目标中推进下一步，并提供从 19:26:34 起四小时的人工远控协助窗口。本轮仅推进 **Step4 的一个发送节奏候选**，不执行整条路线。

用户进一步确认：当前为**远端向日葵会话运行 Encoder、本机 Decoder 捕获右屏**。远控软件名称只作 operator metadata，不进入 wire、Profile、几何阈值或任何 Decoder 分支。旧 Citrix 录像及结论保留原环境含义，不能与本轮向日葵样本混算。

**最新状态（22:32）：PathFix1 的一组人工 A→B 已完成原 Step1 双端关联及整文件复验。A=238.3155474s，B=211.5549573s；本组 B 节省26.7605901s，耗时下降11.2291%，有效吞吐提高12.6495%。** 这是单组观察，不是稳定或因果收益认证；两轮加入位置和实际 UniqueVisualFPS 不同，不能直接晋级或冻结产品默认值。A 使用真实延后源文件后审计且缺少原 Sender wrapper 结束记录；B 本机 Decoder wrapper 因用户提前关控制台而缺少结束记录，但两端原 GUI seal/完整文件均通过。这些缺项不重建，详见第9节。旧 RunId/长路径失败仍保留，不混入正式样本。Step3 仍 PARTIAL，原固定 codec 仍 NOT_RECOVERED；本轮未修几何门、未重跑 codec，不把原始像素筛选能力冒充完整信道平台就绪。

## 1. 假设与实际修改

前轮 Receiver 诊断确认：历史录像首次 Session 接纳前的 119 个不同可解帧没有进入数据接纳，但缺少相应 Sender trace，不能断言上游丢失的原因。

本轮核对当前生产调用链：`SenderFrameBuilder` 在同一活跃窗口创建每段 scheduler；相邻分段的首次控制在相邻逻辑帧发送，原 scheduler 从各自实际启动 burst 起点约每 10 秒刷新。每次周期刷新是一个 Session/Manifest/current-Segment triplet。因此它们存在时间集中结构，可测试分散相位是否改善迟加入后的信息利用率。

候选只做以下变化：

- 内部 scheduler config 增加 `periodicControlPhaseIndex` / `periodicControlPhaseCount`，默认 `0/1`；严格验证 `0 <= index < count <= 8`。
- 生产 builder 按当前有界活跃窗口的位置赋相位。
- 启动控制保持原四份；首次周期 deadline 为 `原启动时间 + 10s + floor(10s * index / count)`。后续仍从实际 burst 起点间隔 10 秒，不再次叠加相位。
- 普通非空帧仍有一个 SegmentDescriptor；周期仍为三控制 slots；15-slot plan、序列化、方程顺序/预算、repair ID lease、资源和发布门不变。
- 单段和零字节保持零相位；停顿不产生追补队列。重试仍冻结 prepared plan 和时间，溢出不部分提交状态。

这是**不增加每段周期重复预算的相位移动**，不是保证任意有限尾窗口内的实际控制总数恰好相等。长停顿和实际提交抖动可重新聚集相位；本轮不追加重锚定、反馈、调参矩阵或另一候选。有限现场 run 的实际控制占用也必须如实报告。

现有代码只改 4 文件：scheduler `.h/.cpp`、`local_desktop_runtime.cpp` 的 Sender 私有创建/接线、scheduler 测试。新增 `tools/PBRemoteControlPhase` 三个文件用于窄验证。**没有改 Receiver 分支或库、Decoder controller、wire/Profile/Golden、安全发布或资源政策。** `runtime-phase-only.normalized.diff` 给出相对冻结 A 的三个接线 hunk；另一个未归一化派生 diff 混入了换行差异，只保留诊断，不作为语义差异依据。

## 2. 本地验证结果及解释边界

| 检查 | 结果 | 不能据此声称 |
| --- | --- | --- |
| A 原调度器 | 10 cases，247,055 assertions 通过 | 现场吞吐基线已取得 |
| B 原测试 + 4 相位 cases | 14 cases，317,719 assertions 通过 | 任意停顿/丢失模型都改善 |
| GUI 测量模型 A/B | 各 3 cases、31 assertions 通过 | 双端现场运行正常 |
| 五段、15 Hz、40s 合成 schedule | 各 600 frames；各 15 periodic triplets、680 control slots、8,320 scheduled equations；稳态最大相邻周期控制间隔 A=150 ticks、B=35 ticks | 实际远控最大等待一定从 10s 降到 2.333s，或整文件已提速 |
| 既有生产 temporal-striping probe | A/B 相同：8×64 KiB、64 首轮逻辑帧、8 活跃段、524,288 B 编码驻留，无 busy | 小样例覆盖 10s 周期或整文件恢复 |
| 原始像素 A/B | 各 30 帧，合计 **60/60 正常观察已用完**；两组 64 KiB 整文件、摘要、发布、reopen 和外部逐字节/SHA256/BLAKE3 通过；全部旧非时钟 trace 与历史 raw-full 相等 | 新多段相位经过像素/远控验证 |
| 新解压包启动 | A/B 各两端 `--version`、两种 identity、Profile、offscreen GUI smoke 共 10 检查通过 | 真实屏幕或干净 Windows VM 认证 |
| PowerShell 5.1 操作准备 | A/B 两角色共 4 次只读预览通过；实际只读拓扑和双端 portable source audit 通过 | 用户已经执行现场 GUI |

原始两组最终文件 SHA-256：`40f1859cccb1f6949e8ff0a0c157c97571759a020d7bc3c801bcdc457394276c`；BLAKE3：`d38ef0993b8f097aee9ad93267387bdef83c49e1f0c54e35be8c87d23974aebf`。

正常工具进程/job 保持 2 GiB；原始 A/B peak commit 110,428,160 / 109,871,104 B。全新应用构建需要 8 GiB 的独立构建 Job，峰值约 6.6 GiB；**该预算不影响产品或测试 Receiver**。最初误套 2 GiB 构建 Job 的失败和之后修正日志均保留。没有完整 CTest、压力、codec、录屏或额外像素矩阵。

PowerShell 5.1 首次预览受父 PowerShell 7 module path 影响，找不到 `Get-FileHash`；启动器改为显式加载当前 Windows PowerShell 自带 Utility module 后四次预览通过，未持久改变环境、执行策略或用户配置。整组 stageCounters 初次比较包含三项 CPU/GPU 时间而失败；核对后仅排除这三项时钟计数，全部非时钟计数和逐帧结果一致，不删除原始数据或改生产检查。

## 3. 初版 A/B 构建、包与固定输入（历史；现场已由 RunIdFix1 取代）

HEAD 始终为 `4a36d0f1b84c94c7388a58b8249a3f1b6ec0634a`。A 是本轮开始的实际工作树重新构建，而不是冒充 clean B0；A/B 都保留同样的 `M1 / InstrumentedExperiment` 测量方式，靠完整 fingerprint 区分。

| 身份 | A 原节奏 | B 分散相位 |
| --- | --- | --- |
| 完整源码 fingerprint | `2ffd3c4d0511762bb22f69c8a796fe664882cf335624b08ab38592ece62689f2` | `be1fcc7f52b32ed7b6843d7f67e15c5893b493dfe778514a4e93122afce36fba` |
| package manifest SHA256 | `be2159979130887ac26dd9322534ae0ffb85f26ec9da1ed868f3430de05e0d3c` | `0e72e7e4030d3a2c403517ac0b364d344bcb6092540665aa8d2195d035b6ffc7` |
| package ZIP SHA256 | `8de9c42843c878606489f1afe688cfdba15bc9670c55c6a8ca9a3638139207fa` | `578973e55ed81fc4ad564c28b863433bc2a1ae7eebdb7c7892c1f59254c6184d` |

A source archive 为 1,189 项，B 为 1,192 项；前轮 1,190 项身份清单多包含受保护报告的**只读 hash 项**。本轮两包按 Step1 既有规则排除此报告，全部 1,189 个共有文件与前轮起点一致，不是漏丢源码。新文档及索引状态随后补记，不冒充已编译源码。

复用明确的 Qt 6.10.1 与 Step2 已安装依赖，未安装/升级；新建 A/B 构建目录，不修改旧构建。`VCPKG_MANIFEST_INSTALL=OFF`、VS 全局 vcpkg 集成关闭，实际 dependency root 记录在 CMakeCache；为未修改的 Step1 packager 复制的 installed status 仅作溯源，SHA256 与原 B0 和实际依赖一致。

现场固定文件为 40,517,389 B 的 `.conan.zip`，SHA256 `ff3c6ae419744396c3c78950ccf18571c929b3668c88640a76b5a39e1aa61a94`，BLAKE3 `ce370394b8f668692b72fcbe0e2297bd313366e2206910539f4759099df6d481`；本轮 A/B portable audit 均确认实际 encoded bytes 为 40,410,277，五段编码 ledger 相同。

## 4. 当前现场操作与退出条件

**当前唯一现场操作包（RunIdFix1）：** `<repo>\artifacts\remote-control-phase-runidfix-20260908-run01\PixelBridge-Remote-ControlPhase-AB-20260908-RunIdFix1.zip`。初版目录及其 `results` 仅保留作历史证据，不再用于重新启动 A/B。

- 当前 ZIP 大小 130,706,699 B；SHA256 `357244d01031020db873892d26009ba75efc2fe49144f332bd875279bfbe4556`；235 members，展开 276,990,354 B。初版 ZIP 身份仍保留在原 `FIELD_BUNDLE_RESULT.json`，未覆盖。
- 两端各在新目录解压。按包内 `START_HERE.md`，先 A 后 B，只做 15 Hz、固定文件、无录屏的迟加入主场景。
- 本机已只读核对主屏 `(0,0)-(2560,1440)` 与右屏 `(2560,0)-(5120,1440)`，均 96 DPI；每次人工启动入口会重新核对本机拓扑并保存远端显示元数据。不移动窗口、鼠标、键盘，不操作左屏。
- Sender READY 提示只读本端 submitted trace 的后续 Carousel 记录，人工决定开始 Decoder；不向 Decoder 传 payload 或给 Sender 回传 ACK。正式迟加入仍需停后精确匹配两端 `(SessionTag, FrameSequence)`，不得用 READY 代替。
- 保留两端完整 entry/run/seal/source audit 和权威输出文件位置。源文件只由发送端使用，接收文件必须来自实际右屏像素。
- 主计时仍为本机 `finalReopenVerified - startAccepted`；整文件正确、发布/reopen、源文件前后审计、双端构建和时序覆盖完整才有 goodput。没有跨机时钟相减、插值、人工补值或排除恢复尾部。
- 若超过此次人工 600s 单组预算，人工停止并保留失败记录；不以工具旧 1,800s 上限为扩大测试的理由。一组 A/B 只给出初始实测，不证明稳定增益。

若 A/B 完整证据支持收益，报告这组结果与不确定性，等待下一步安排；无收益/失败则不晋级。若用户尚未提供证据，保持 `FIELD_PENDING`，不将目标或整个 Step4 标为完成。

## 5. 证据与窄复验

修复后证据根：`<repo>\artifacts\remote-control-phase-runidfix-20260908-run01`，主要记录为 `REPAIR_REPORT.md`、`REPAIR_RESULT.json`、`FINAL_SOURCE_AND_PACKAGE_PARITY.json`、`FIELD_BUNDLE_RESULT.json`。

以下初版证据根及复核命令保留作**历史只读核对**，不用于现场启动：`<repo>\artifacts\remote-control-phase-20260908-run01`。

主要记录：`context/SCOPE.json`、`context/SELECTED_DESIGN.json`、`context/A_TO_B_SOURCE_DELTA.json`、`source-A`、`source-B`、`logs`、`RAW_PARITY.json`、`package-A/package-result.json`、`package-B/package-result.json`、`FIELD_BUNDLE_RESULT.json` 和 `field/START_HERE.md`。

可只读复核包；**现场正在运行时不并行构建、WARP 回放或大文件核验**：

```powershell
$repository = '<repo>'
$evidence = Join-Path $repository 'artifacts\remote-control-phase-20260908-run01'
# 不启动 EXE、GUI、capture；只核对已封存文件。
& '<python>' -B -X utf8 (Join-Path $repository 'tools\PBRemoteThroughputStep1\step1.py') verify-package --package (Join-Path $evidence 'package-B\PB-Step1-M1-be1fcc7f52b3') --seal (Join-Path $evidence 'package-B\PB-Step1-M1-be1fcc7f52b3.seal.json') --expected-manifest '0e72e7e4030d3a2c403517ac0b364d344bcb6092540665aa8d2195d035b6ffc7'
```

保护报告 `<repo>\docs\PHASE1_GATE_REPORT.md` 不修改、不暂存、不提交，SHA256 保持 `076ef4c9b9f89eabccd323dbe4bffc4dc125ddaf96e6ee437d2cf5b1b1cea306`。未暂存、提交、推送、改写历史、覆盖或删除历史包/源码/构建/实验记录。

## 6. 20:05 现场启动拒绝与同源修复

用户截图确认 A Encoder 在开始发送前报 `RunId 必须为空或 128-bit lowercase hex（32 个字符）`，不是 codec/向日葵/Receiver 吞吐失败。源码核对发现双端均错误地把 `run-` 加带连字符 UUID 的证据目录 basename 赋给 `config.runId`。旧 GUI smoke 没有传入 evidence 对象，已有模型测试没有调用两端 Start validator，故未覆盖这个入口。

修复保持人类可读目录名不变；从同一个 UUID 独立生成 `Id128`，通过 `Step1GuiEvidence::RunId()` 绑定给两端。严格 RunId validator、Session CSPRNG、发布/重开与资源门均不变。补充格式正负例、重复 BeginRun/重新开始的身份测试，以及两端 offscreen 的实际测量 Start 路径（替换 OS 显示/采集服务，不发送键鼠，不读取桌面像素）。原普通 GUI smoke 保留，不以替换测试缩小覆盖。

重新 freeze/build/test/package/seal A/B，使用全新 `artifacts/remote-control-phase-runidfix-20260908-run01` 及独立构建目录。所有旧包、构建、源快照和失败截图保留；不得把旧包校验通过等同现场能启动。本节当前记录修复设计，验证结果以后续修复证据目录为准，尚未声称实屏通过。

### 6.1 修复后最终封存

A/B 各 4 cases / 157 assertions 及各 10 项新解压检查通过，两端实际测量 Start/Stop 均经过 offscreen 生产 controller/runtime。A/B 1,193 个源码项仅原四个相位文件不同，共同修复五文件一致。四入口 PS5.1 preview 与只读拓扑/固定输入审计通过。

新交付：`artifacts/remote-control-phase-runidfix-20260908-run01/PixelBridge-Remote-ControlPhase-AB-20260908-RunIdFix1.zip`，130,706,699 B；SHA256 `357244d01031020db873892d26009ba75efc2fe49144f332bd875279bfbe4556`。完整重放说明与失败预检边界见同目录 `REPAIR_REPORT.md`、`REPAIR_RESULT.json`；最终源码一致性见 `FINAL_SOURCE_AND_PACKAGE_PARITY.json`。所有旧包和失败构建保留。此状态文档为源码封存后的记账，不改包内已冻结源码身份。当前 LOCAL_REPAIR_VERIFIED / FIELD_RETEST_PENDING，未获得远控效率结论。

## 7. 右屏整文件诊断与 PathFix1 操作修复

2026-09-08 21:14，原 A Receiver 在本机右屏 WGC 实际像素下恢复 `.conan.zip` 40,517,389 B，`finalReopenVerified - startAccepted = 241.4358493s`；digest/publish/reopen、外部 SHA256/BLAKE3/逐字节比对通过。此为 artifact-only headless Receiver-only diagnostic，非人工 GUI A/B 正式样本，不证明候选收益。详细结果见 `artifacts/remote-rightscreen-diagnostic-20260908-run01/REPORT_AND_NEXT_PLAN.md`。

此前四次 GUI 接收失败原因已通过同库单变量 probe 复现：输出目录存在但 `.resume.tmp` 达 271 字符，原 `CreateFileW` 路径失败；短路径创建和重开成功。不是用 codec/几何更改绕过失败。

用户随后确认可以人工配合切换。本轮新交付 `artifacts/remote-field-shortpath-20260908-run01/PixelBridge-AB-PathFix1.zip`（120,814,835 B；SHA256 `43b70af59189be2e97d9fc63f8c0897ea3eab43fc2f85a6441547d0b39ffe772`）。仅改变根启动器/操作说明/identity，增加路径预检；sealed A/B 二进制、DLL、manifest/source identity 不变，无产品重编译。25 path guard cases、准备和新 ZIP 解压共 8 次只读入口预览、两次实际 Launch 早拒绝负例通过。

**后续现场只使用 PathFix1 新目录**；默认输出迁至各端 `%LOCALAPPDATA%\PBField` 新 run，本机 resume 临时路径为 162 字符；远端启动时重新按当地用户目录预检。旧 RunIdFix1 ZIP 和所有失败结果保留。完整操作及验证见新证据根 `HANDOFF.md` / `PB-AB-PathFix1/START_HERE.md`。当前 `PATH_GUARD_VERIFIED / MANUAL_PAIR_PENDING`；先新的 A，再 B，缺少正式双端证据仍不晋级。

## 8. 新人工 A 整文件通过，发送端退出后审计待补齐

2026-09-08 22:08 增量；以上段落的现场状态保留各自记录时点。本轮 PathFix1 新人工 A 已完成，用户确认先关闭 Decoder 再关闭远端 Encoder，且全程 15 Hz、无录屏、向日葵/屏幕设置不变、Decoder 只捕获右屏。用户已回传 A Encoder 完整现有目录，并确认启动黑色控制台已关闭。

- 接收端权威路径：`C:\Users/<user>\AppData\Local\PBField\A-Decoder-20260908-213759-d0179029\entry\run-785798a6-7098-47f8-abbb-b66dd5415f7c`。全新接收，无 resume；40,517,389 B 原文件 / 40,410,277 B 编码字节；5/5 segments，digest、safe publish、final reopen、外部 SHA256/BLAKE3 均通过。
- 主计时为 **238.3155474 s**；VerifiedRawGoodput 为 **170,015.71841216768 B/s**，VerifiedEncodedGoodput 为 **169,566.26389202167 B/s**。此为新人工 A 的接收事实，不与第 7 节 headless diagnostic 混样，不是 B 提速结论。
- 回传 Sender：`<repo>\artifacts\fr0908-A\A-Encoder-20260908-213639-53a5dcef\entry\run-53010a72-b2aa-4800-b490-0ab8657215d0`。双端 GUI seal、frozen A 实际 EXE 身份、Session、before audit、runtime ledger 已匹配。6671 条提交帧覆盖完整；Receiver 首帧 `(6892749689818756988, 2800)` 精确命中 Sender `carouselPass=1 / segmentOrdinal=3 / cyclePosition=356`，不插值、不跨机减时钟。
- 回传 operator 根缺少 `source-after.json` 及 `operator-launch.json`。无法从“控制台已关闭”确定远端是否后来生成过这些文件；也不把缺少 wrapper 文件解释为传输失败。Sender 自身 start-to-terminal 为 445.4641246 s；原 launcher 整体 elapsed 当前不可得，不能伪造。
- 已准备 **仅限此 A 的延后源文件审计小工具**：`<repo>\artifacts\remote-field-correlation-20260908-run01\PixelBridge-A-PostAudit.zip`，5,076 B，SHA256 `90f6b0bb37eabe05c4ace073679cf2fd8069f508b215a98894ce55b1291fd1ef`。固定核对原 run/trace 和原审计脚本 hash，调用未改动的 package/source auditor；只在远端原 operator 根新建 `post-audit-*` 子目录，标记真实延后时点。若原后审计/launcher 文件在远端已存在，只另存原字节副本，不重建它们；缺失的旧 launcher 结束/时长保持 unavailable。
- 小工具 PS5.1 九项定向检查及新 ZIP 解压只读预览通过；本机原 portable audit 的固定输入验证仅是工具测试，**不能代替远端后审计**。首轮测试因中文系统错误文案与英文预期不符而失败，改为检查同一 OS HRESULT 后通过；原失败日志保留，未削弱 helper 的 create-new 拒绝。无产品/codec 修改，无实屏、输入、全回归或压力测试，sealed A/B 原包再次校验不变。

当前状态为 **A_RECEIVER_WHOLE_FILE_VERIFIED / A_POST_AUDIT_PENDING / B_NOT_RUN**。小工具执行成功仍须回传真实新证据，并以原 `step1.correlate` 完成核对；必须附带延后审计和旧 wrapper 缺失的限制，不能冒充全套原 wrapper 记录已恢复。补齐前不启动 B，不重跑 A。主要证据：`<repo>\artifacts\remote-field-correlation-20260908-run01\A_CORE_CORRELATION_PARTIAL.json`、`A_RECEIVER_PRECHECK.json`、`A_OPERATOR_CONDITIONS.json`、`A_DELAYED_POST_AUDIT_DELIVERY.json`。

### 8.1 延后审计实际回传与 A 核对完成

2026-09-08 22:14 增量：用户回传 `<repo>\artifacts\fr0908-A-post\post-audit-20260908-221032-5cf71201`。工具记录远端实际补查时点为 22:10:32–22:10:33；四文件 inventory/hash 核验通过，source before/after 的摘要、大小、文件 ID、device、mtime 和完整编码 ledger 全等。原后审计/launcher 文件在远端确实缺失，补查没有重建它们。

未修改的 `step1.correlate` 已在 `<repo>\artifacts\remote-field-correlation-20260908-run01\A-completed-post-audit01\A_POSTSTOP_CORE_CORRELATION.json` 输出 `sampleEligible=true`，再次直接重开 Receiver 报告中的原最终路径做双摘要。对应 `A_FIELD_RESULT_WITH_LIMITATIONS.json` 保留 **A_CORE_SAMPLE_VERIFIED_WITH_DELAYED_SOURCE_POST_AUDIT**：原 Sender wrapper 的 completion/elapsed/budget-warning 均为 null，不冒充正常收尾；实际 Sender start-to-terminal 445.4641246 s、Receiver terminal 238.347794 s 均在 600s 内，Decoder launcher 263.6446755 s、无预算告警。

精确复验入口为 `<repo>\artifacts\remote-field-correlation-20260908-run01\verify_A_delayed_post.py --output-directory <新的绝对目录>`；使用 `<python> -B -X utf8`，输出 create-only。该入口仅编排既有 Step1 验证器并核对新增审计 provenance，不修改原 checkers 或源证据。

B frozen package 再次只读验证通过；`B-display-preparation01.json` 仅枚举元数据，右屏仍为 `DISPLAY2 / [2560,0,5120,1440] / 96 DPI`，左屏保持原边界。`B_MANUAL_PREPARATION.json` 记录同一 PathFix1 B 启动路径、固定文件、15 Hz、无录屏、600s 和原 23:26:34 协助截止。启动时仍由原 launcher 再核对拓扑；未自行启动 GUI 或采集像素。现在可由用户人工进行唯一一轮 B，结束时先关 Decoder GUI、再关 Encoder GUI，**两端黑色控制台都必须等到 `Finished. Keep and return...` 后再关闭**。B 尚未运行；无非本机提速结论，不冻结产品默认值。

## 9. A→B 单组远控对照完成：本组 B 更快，尚不证明稳定收益

用户完成 B 并回传远端证据，确认两轮源文件、15 Hz、关闭录屏、向日葵/屏幕设置和右屏 ROI 全程不变。本机 B Decoder 绑定句柄在22:24:40确认退出；终态 report、GUI evidence seal、source/binary identity、完整提交覆盖、digest/safe publish/final reopen 和原最终路径外部 SHA256/BLAKE3 全部通过。B 的 first Bootstrap 精确匹配 `SessionTag=17202764380963327350, FrameSequence=2737, Carousel=1, Segment=0, cyclePosition=294`；7149条 Sender提交覆盖完整。A 的对应值为 `6892749689818756988 / 2800 / Carousel=1 / Segment=3 / cyclePosition=356`，6671条覆盖完整。不跨机减时钟，不插值。

| 指标 | A 原节奏 | B 周期相位分散 |
| --- | ---: | ---: |
| 原文件 / 编码字节 | 40,517,389 / 40,410,277 B | 相同 |
| 主时长：startAccepted 到 finalReopenVerified | 238.3155474 s | 211.5549573 s |
| VerifiedRawGoodput | 170,015.718412 B/s | 191,521.813136 B/s |
| VerifiedEncodedGoodput | 169,566.263892 B/s | 191,015.504982 B/s |
| 首次控制接纳 | 7.0956824 s | 3.2932456 s |
| 首个有用方程 | 7.1184374 s | 3.3154499 s |
| UniqueVisualFPS | 9.942937766 | 10.804767181 |
| 已观察唯一逻辑帧 | 2299 | 2251 |
| Sender全run控制占用（非Receiver对齐窗口） | 7.2223055% | 7.1982095% |

主时长节省26.7605901s，其中“启动到首个有用方程”差值为3.8029875s；其余差异出现在后续恢复过程。不能把所有收益归因于首次控制等待，更不能仅凭本次 UniqueVisualFPS 差异断言是向日葵或候选造成。当前只有固定顺序 A→B 一组；两轮都迟加入但具体相位不同，未随机化/反序复验，无统计稳定性结论。

### 9.1 缺项和权威边界

- A Sender 原 wrapper 缺失；22:10实际延后审计与 before 完全一致，核心关联通过；旧 helper elapsed/结束/预算告警仍 null。
- 用户明确说明 B 本机 Decoder 控制台关闭过快。该轮 GUI 已完整封存，句柄确认退出，文件再次独立复验通过；只有辅助 `operator-launch.json` 缺失，不伪造或要求重跑接收。B 远端 source-before/source-after/launcher齐全，launcher519.927351s、无预算告警。
- 实际 Sender start-to-terminal A445.4641246s/B477.5085684s，Receiver terminal A238.347794s/B211.5783326s，均未超过本轮600s。辅助 wrapper时长缺失不拿来补主计时，也不声称所有 wrapper正常收尾。
- 原 verifier 未修改，两个 `sampleEligible=true` 只表明整文件、身份、计时和迟加入关联满足既有检查；不是三组正式基线、B0/M1开销矩阵、稳定收益或全部Step4完成。
- 旧 RunId/长路径失败、成功的 headless Receiver-only diagnostic、旧编译/测试失败、原始像素与codec失败全部保留。没有额外参数矩阵、codec复跑、压力或第二组实屏。

### 9.2 复核入口、交付与停止点

完整单组结果：`<repo>\artifacts\remote-field-correlation-20260908-run01\AB-comparison01\AB_COMPARISON.json`；同目录 `REPORT.md`、`COMPLETION_AUDIT.json`、`A_CORE_CORRELATION_REVERIFIED.json`、`B_CORE_CORRELATION_REVERIFIED.json` 保存结论、限制、验收逐项和原工具复核。

精确重放（只读原证据，仅创建新的结果目录，不运行GUI/实屏/codec）：

```powershell
& '<python>' -B -X utf8 '<repo>\artifacts\remote-field-correlation-20260908-run01\finalize_AB_evidence.py' --output-directory '<repo>\artifacts\remote-field-correlation-20260908-run01\AB-reverification-new01'
```

该工具重新核验两端原始 seal、实际 EXE/package、源文件前后、整文件原最终路径及完整 trace；A/B各1193项源快照经 ZIP逐项验证，仅原四个相位文件不同；当前产品源码仍等于frozen B，差异只有封存后状态文档。禁止把此命令用于覆盖已存在输出目录。

**本轮独立授权的单候选、一组A/B子目标完成并停止。** B保留为候选，未认证稳定默认值。建议下一次只做一组反序B→A最小确认以降低顺序/加入位置影响；此建议未执行，不在剩余协助时间内自动追加。整个Step4仍未完成，Step3仍PARTIAL/codec NOT_RECOVERED，Step5–10未开始。未暂存/提交/推送/改写历史；受保护报告SHA256保持原值。

## 10. 后续明确获准的一组反序 B2→A2（准备完成、尚待人工运行）

2026-09-08 22:43，用户明确同意“按照你说的，在目标模式下，开始下一步”。这是一项新的有界目标：仅用不变的frozen A/B和PathFix1操作包做一组反序B2→A2，保留上一组A1→B1；不把上节已结束的目标追溯改成无界持续测试。

当前 `PREPARED_B2_PENDING`。两个包、当前产品源码、受保护报告和空Git index重新核对；产品源码与frozen B一致，只有封存后文档差异。只读显示元数据确认右屏仍为DISPLAY2/[2560,0,5120,1440]/96DPI；未启动GUI或捕获像素，没有重新构建/产品测试。B2/A2必须各建新run，不用旧resume；比较新反序对照并同时报告四次观察，不挑选最快样本。

新证据根 `<repo>\artifacts\remote-field-reverse-20260908-run01`：`SCOPE_AND_BINDINGS.json`、`PREPARATION_RESULT.json`、`OPERATOR_STEPS.md`。远端新B2/A2证据分别回传到已创建的 `<repo>\artifacts\fr0908-B2`、`<repo>\artifacts\fr0908-A2`，不覆盖旧目录。

仍保持同一固定文件、15Hz、无录屏、相同右屏ROI及远控设置，600s/轮；旧协助窗口23:26:34不被默认为延长，为保留完整600s，23:16:34后不再开新轮。先B2，核对后再A2；两端黑色控制台分别等`Finished`后再关闭。故障/超时保留原记录，不自动补跑、改变参数或弱化验证。两个顺序对照仍不能替代正式稳定性/因果认证。

### 10.1 B2 已完整核验，A2 等待人工启动

2026-09-08 23:03 增量：用户完成 B2 并回传新 Sender，确认同一源文件、15Hz、关闭录屏、同一右屏ROI及向日葵/网络/屏幕设置全程不变。原始 `step1.py` SHA256 未变；未改动的 `correlate` 完成双端封存/EXE身份、源文件前后完整identity/ledger、原最终路径外部双摘要、完整提交覆盖和精确迟加入关联，`sampleEligible=true`。

- Receiver：`C:\Users/<user>\AppData\Local\PBField\B-Decoder-20260908-224928-7381de53\entry\run-36539ffe-41e9-47c8-b5c8-70e8f656945c`；Sender：`<repo>\artifacts\fr0908-B2\B-Encoder-20260908-224606-aa5c145a\entry\run-15db2627-9eb2-4087-a357-a220b80252ca`。新run/session、无resume。
- 主时长 **212.477132s**，VerifiedRawGoodput **190,690.586882 B/s**，VerifiedEncodedGoodput **190,186.476162 B/s**；40,517,389B最终文件SHA256/BLAKE3与原固定输入相同。UniqueVisualFPS **10.799968591**，2283个唯一逻辑帧。
- 首次接受Bootstrap精确命中 `SessionTag=7983696376992260819 / FrameSequence=2832 / Carousel=1 / Segment=1 / cyclePosition=389`；不插值、不跨机减时钟。
- 两端原 `operator-launch.json` 齐全，GUI exit=0、预算告警=false；Encoder helper448.8409752s，Decoder helper241.5052479s，均在600s预算内。与上一组缺项不同，B2不需要补造或延后wrapper记录。主性能计时仍只来自Receiver，而非helper时长。
- 核验证据：`<repo>\artifacts\remote-field-reverse-20260908-run01\B2-audit01\CORE_CORRELATION.json`、同目录 `RUN_AUDIT.json`；编排脚本 `verify_reverse_run.py` 只读原始证据，结果create-only，helper受120s/2GiB/1MiB日志预算限制，本次0.453s、退出0。

已向用户提供现有 `Start-A-Encoder.cmd` / `Start-A-Decoder.cmd` 的 A2 人工步骤；此时 **B2_CORE_PASS / A2_MANUAL_START_PENDING**，尚不能形成反序配对结论或完成当前目标。下一轮仍用相同包、固定参数和原时间窗口；不追加第五轮、不改产品、codec或默认配置。

### 10.2 A2 接收通过，但远端收尾未完成且触发预算告警

后续 A2 人工运行：本机 Receiver 为 `C:\Users/<user>\AppData\Local\PBField\A-Decoder-20260908-230740-558a6931\entry\run-5da8da8a-aa73-470f-94ce-084cb7ea9c52`，205.0048921s 完成整文件，外部SHA256/BLAKE3、发布/重开和GUI seal通过。绑定PID46760及creation FILETIME134333536646541320的只读句柄于23:11:33确认退出；本机wrapper正常收尾，223.4693646s、exit0、无预算告警。

用户回传 `<repo>\artifacts\fr0908-A2\A-Encoder-20260908-230417-0eea0877\entry\run-7b40d88b-38ec-4e65-8d5e-c8629bd0d8e2`，确认固定现场条件不变。Sender GUI seal、frozen A身份、Session、source-before/ledger与Receiver一致；首Bootstrap精确命中 `11376614554329835845 / 2862 / Carousel1 / Segment0 / cycle418`。Sender运行start-to-terminal474.5094348s，但这不等于整个Encoder进程或启动脚本已退出。

**不能将这一轮登记为全部通过：** 回传目录和远端原目录均缺source-after.json及operator-launch.json。用户说明Encoder主窗口已关闭、黑色控制台仍开着；两张截图明确显示10分钟操作预算告警、没有Finished。当前只能确认远端wrapper预算告警为true，elapsed未知；不能把告警改成false，也不能把接收205秒代替wrapper时长。原correlate未在缺少真实after audit时调用，formal eligibility保持未确定，预算合规现场样本为false。

证据见反序根 `A2_RECEIVER_PRECHECK.json`、`A2_SENDER_PRECHECK.json`、`A2_COLLECTION_EXCEPTION.json` 及原截图副本。已请求用户提供远端只读Encoder进程信息以区分进程未退出与脚本后续卡住；未杀进程、未补造记录、未自动重传或修改代码。接收端观察A2比B2快7.4722399s仅为当前不完整配对的描述，尚非完整反序性能认证。

### 10.3 最终收束：四轮核心文件核验通过，A2退出/预算失败不豁免

用户在远端只读CIM查询到仍存活的PID22820，creation23:04:26，EXE路径及evidence-root精确对应A2。结合用户主窗口已关闭的报告和原Launcher `WaitForExit` 后才执行source-after的顺序，可确认本次“窗口关闭但进程未退出，脚本等待未结束”；具体C++退出卡点没有线程栈/阶段探针，尚未证实。不能由此追溯推断A1缺项的原因。

用户明确同意后，人工通过任务管理器结束该残留进程，并把真实收尾后的完整目录另存到 `<repo>\artifacts\fr0908-A2-closeout\A-Encoder-20260908-230417-0eea0877`。原已回传目录未覆盖。新副本中seal/final/events/ledger/entry/before与原件逐项哈希相同；原Launcher产生的source-after与before完整identity/ledger全等。真实operator-launch为 **exitCode=1、helperElapsedSeconds=999.1670919、operationBudgetWarning=true**，不得改为正常退出或无告警。

`verify_A2_exception.py`独立记录异常，不修改正常600s验证器；原`step1.correlate`在真实after就绪后通过。`finalize_four_runs_with_A2_exception.py`随后重新核验全部四轮原核心关联及最终文件，四组Session/八个RunId互异，均无resume；A/B各1193项源快照仅原四个相位文件不同，当前产品源码未改、只有封存后文档差异。

| 轮次（实际顺序） | 主时长s | VerifiedRawGoodput B/s | VerifiedEncodedGoodput B/s | 收尾边界 |
| --- | ---: | ---: | ---: | --- |
| A1 | 238.3155474 | 170015.718412 | 169566.263892 | 原Sender wrapper缺失；实际延后source-after审计 |
| B1 | 211.5549573 | 191521.813136 | 191015.504982 | 本机wrapper缺失；其余核心证据完整 |
| B2 | 212.4771320 | 190690.586882 | 190186.476162 | 两端正常收尾、600s内、无预算告警 |
| A2 | 205.0048921 | 197641.083512 | 197118.598420 | 进程残留后人工结束；远端退出/预算失败 |

原组B吞吐+12.6495%；反序接收观察中B吞吐-3.5167%，比A多用7.4722399s。四次全纳入的总字节/总时间描述性汇总为B+4.5488%，**但不能包装成稳定增益，也没有得到完全合规的反序配对**。不同加入相位/UniqueVisualFPS、固定ABBA顺序、样本极少、A2退出/预算异常和旧wrapper缺项均保留。

最终权威结果：`<repo>\artifacts\remote-field-reverse-20260908-run01\four-run-comparison01\REPORT.md`、`FOUR_RUN_COMPARISON.json`、`COMPLETION_AUDIT.json`及四个`*_CORE_CORRELATION.json`；A2异常另见`A2-exception-audit01\RUN_AUDIT.json`。正常收尾预案`finalize_four_runs.py`未执行，不是本次权威复验入口；应使用`finalize_four_runs_with_A2_exception.py --output <新的绝对目录>`。

**当前有界目标以“反序尝试及失败证据已审计、四次结果完整交付”完成并停止，不是性能PASS。** 不晋级B为稳定默认值，不回滚或改写现有候选；整个Step4仍IN_PROGRESS，Step3仍PARTIAL/codec NOT_RECOVERED，Step5–10未开始。下一步仅提交待确认的Encoder退出/证据收尾窄修复计划，之后再由现存吞吐证据选择单变量候选；当前未实现、未构建、未补跑。无Git操作、受保护报告不变、历史源/包/构建/录像/记录全部保留。
