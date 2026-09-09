# PixelBridge 文档状态矩阵（2026-09-09）

> **性质：** 本文件只做**文档索引与状态标注**，不是第二套真相。事实来源优先级仍是：当前 `git log` 与仓库内容 → 源码/测试/封存 artifact → 各文档顶部带日期的原始记录 → 本矩阵。冲突时以更早的原始记录为准，本矩阵负责指出冲突，不负责改写历史。
>
> **本轮边界（重要）：** 本文件与其所属提交属于**纯文档与 Git 记账**：未执行任何构建、测试、实屏、远程桌面/Citrix 或大文件现场流程，未修改任何源码字节、协议常量、Profile、Golden Vector 或封存证据。因此本矩阵**不构成任何新的验证、晋级或性能结论**，也不得被引用为“已复核通过”。

## 1. 记账时点的仓库事实

| 项 | 值 | 核对方式 |
| --- | --- | --- |
| 分析时 HEAD | `a39fe61cbac5c921601685007aaf262865f64fe6`（本矩阵所在提交是其后的文档提交） | `git rev-parse HEAD` |
| 工作树 | 干净；唯一未跟踪文件为受保护的 `docs/PHASE1_GATE_REPORT.md` | `git status --porcelain -uall` |
| 远端 | 仓库无 remote；全程未 push、未 reset、未 rebase、未重写历史 | `git remote -v` |
| 当前产品合同 | `PB-Unified-SC6-V3`，`VisualProfileId=0x5042554E49534333`，layout 10，1920×1080 规范画布 | 总体设计与 `unified_visual_profile.h` |
| 实验身份（未进正式 catalog） | `PB-Experimental-BlankControl-1`，`0x504242414E443031`，layout 11 | 吞吐路线图与夜间记录第 21 节 |
| 非本机吞吐目标 | **2026-09-09 由用户主动停止**（非技术阻塞、未完成） | 暂停交接文档 |
| 受保护文件 | `docs/PHASE1_GATE_REPORT.md`，SHA-256 `076ef4c9b9f89eabccd323dbe4bffc4dc125ddaf96e6ee437d2cf5b1b1cea306`，不 add、不改、不删 | 暂停交接文档第 2 节 |

### 1.1 基线 `4a36d0f` 之后的提交构成

| 提交 | 类型 | 实际内容 | 是否改变源码字节 |
| --- | --- | --- | --- |
| `6eb4563` | chore(git) | `.gitignore` 追加远端显示现场录像忽略项 | 否（仅仓库配置） |
| `8d60f77` | feat(core) | `PBCore` 仅观测用 stage timing + 用例 | 是 |
| `a3eae50` | perf(inner-fec) | QC-LDPC 每 check node 复用行最小值归一化 + 用例 | 是 |
| `9477617` | feat(modulation) | CPU oracle 逐 lane FEC/slot 计时 | 是 |
| `bf1697e` | perf(modulation) | Bootstrap 标记顺序 BGRA 扫描游标 + 用例 | 是 |
| `401b6d2` | feat(app) | Step1/Step2 测量、录像像素回放、发布计时观测 + 用例 | 是 |
| `75984df` | feat(gui) | 双端 GUI 暴露 Step1 测量入口 + 启动用例 | 是 |
| `46a072b` | feat(tools) | 2026-09 远端吞吐实验工具集与 Step1 runner | 是 |
| `8f74cd7` | docs | 吞吐路线图、14 篇 2026-09-08/09 记录、交接与索引首次入库 | 否 |
| `80c9e0e` | docs | 校正 Step4/Step5 状态与暂停交接的过期快照描述 | 否 |
| `6c059c6` | docs | 索引补登 11 篇未注册文档、修正“Step5–10 未启动”旧表述 | 否 |
| `a39fe61` | docs | 运行时清单/用户指南/首页补 Step1 测量入口与 G22 之后状态 | 否 |
| 本提交 | docs | 新增本状态矩阵；暂停交接补一条 HEAD 勘误 | 否 |

**关键推论：** `8d60f77`…`46a072b` 只是把暂停时工作树里已有的源码改动入库，入库动作本身不代表这些改动在本轮被重新构建或重新测试；它们的验证记录仍是相应 `REMOTE_*` 文档在当时的原文。文档类提交只移动记账位置，**不会升级任何证据身份**：旧交付包、录像、raw 像素与实验封存目录仍按封存时记录的 build 身份解释；`--measurement-build-identity` 的 `baseCommit` 与 `sourceFingerprintSha256` 均为编译期注入，只有重新构建后才反映新 HEAD。若要以当前源码身份作为新对照基线，必须重新 freeze、build、test、package、seal。

## 2. 图例

| 标记 | 含义 |
| --- | --- |
| 当前权威 | 今天仍约束产品合同、协议字节、工程纪律或验收口径 |
| 当前入口 | 导航/用途有效，但不是判定依据 |
| 冻结证据 | 某 Gate 或一次性实验的终态记录，只允许追加勘误，不得改写 |
| 历史（已取代） | 旧路线/旧 Profile/旧合同，可复用工具与思路，不得反向覆盖当前合同 |
| 诊断日志 | 按日/按节追加的运行记录，旧节结论不回写 |
| 未跟踪受保护 | 存在于工作树但按用户要求永不入库 |
## 3. A 组：入口与总架构（6 篇）

| 文档 | 状态 | 可用 / 不可用 |
| --- | --- | --- |
| [`README.md`](README.md) | 当前索引 | 可用：按日期排列的状态日志与分组路由。不可用：把顶部某一日志当最终结论；状态以本矩阵与各文档顶部原文交叉核对 |
| [`PixelBridge_最终技术路线与总体设计.md`](PixelBridge_最终技术路线与总体设计.md) | 当前权威（长期不变量） | 可用：协议、FEC、视觉、GPU/capture、线程、存储、安全、验收的总体判断。不可用：覆盖顶部 supersession note 或已冻结产品合同的旧段落 |
| [`DOCUMENT_STATUS_2026-09-09.md`](DOCUMENT_STATUS_2026-09-09.md)（本文） | 当前入口（仅索引/状态） | 可用：判断某篇文档现在还能不能作依据。不可用：作为验证证据或性能结论来源 |
| [`CURRENT_RUNTIME_OPTION_INVENTORY.md`](CURRENT_RUNTIME_OPTION_INVENTORY.md) | 当前入口（过渡实现清单） | 可用：区分工作树里真实存在的 GUI/CLI/实验路径，含 2026-09 的 `--measurement-build-identity`、`--gui-measurement --evidence-root`。不可用：当最终产品说明；其中 2026-09 条目是源码声明级核对，未构建未运行 |
| [`GITHUB_PUBLISH_CHECKLIST.md`](GITHUB_PUBLISH_CHECKLIST.md) | 当前入口（状态日期 2026-09-05） | 可用：首次推送前的源码/仓库检查。不可用：任何“已推送/已发布”表述——仓库当前无 remote |
| [`UNIFIED_USER_GUIDE.md`](UNIFIED_USER_GUIDE.md) | 当前入口（G22 本地发布候选界面） | 可用：双端 GUI 日常操作与已知退出行为。不可用：现场性能承诺；Step1 测量入口非日常操作路径 |

## 4. B 组：统一产品路线与 Gate 证据（G00–G22，16 篇）

| 文档 | 状态 | 可用 / 不可用 |
| --- | --- | --- |
| [`UNIFIED_VISUAL_LARGE_FILE_IMPLEMENTATION_ROADMAP.md`](UNIFIED_VISUAL_LARGE_FILE_IMPLEMENTATION_ROADMAP.md) | 当前权威（G00–G22 产品合同与实现史） | 可用：现有产品合同、验收史、顶部指向新吞吐路线。不可用：把 G22 之后的实验假设当已冻结协议 |
| [`UNIFIED_VISUAL_CP_A_HEADLESS.md`](UNIFIED_VISUAL_CP_A_HEADLESS.md) | 冻结证据（G04 CP-A，2026-09-03） | 可用：Descriptor/Outer/Receiver/journal/storage/publish 的 9-case 无屏幕闭环。不可用：视觉链覆盖结论（原文 `visualChainCovered=false`） |
| [`UNIFIED_ENCODER_WORKFLOW.md`](UNIFIED_ENCODER_WORKFLOW.md) | 冻结证据（G15，2026-09-04） | 可用：Encoder 产品工作流与 headless/controller/GUI smoke 史。不可用：当期数值直接代表 SC6-V3/layout 10 现网 |
| [`UNIFIED_DECODER_WORKFLOW.md`](UNIFIED_DECODER_WORKFLOW.md) | 冻结证据（G16，2026-09-04） | 可用：Decoder 工作流与 offscreen smoke 史。不可用：实屏/远程/大文件结论——当时未跑 |
| [`UNIFIED_TELEMETRY_REPORT.md`](UNIFIED_TELEMETRY_REPORT.md) | 当前权威（G17 报告口径） | 可用：`PixelBridge.RunReport.3` 字段、`VerifiedRawGoodput`/`VerifiedEncodedGoodput` 与唯一逻辑帧口径。不可用：把 Present/callback FPS 当 goodput |
| [`UNIFIED_PROCESS_RESTART_RECOVERY.md`](UNIFIED_PROCESS_RESTART_RECOVERY.md) | 冻结证据（G18，2026-09-04） | 可用：256 MiB 真实进程终止与 9 个终止点恢复。不可用：现场认证（批准的无像素 headless） |
| [`UNIFIED_LARGE_FILE_CAPABILITY.md`](UNIFIED_LARGE_FILE_CAPABILITY.md) | 冻结证据（G19 已通过，2026-09-04） | 可用：20 GiB+64 KiB、2,561 Segment、终止后恢复、外部双摘要能力边界。不可用：吞吐结论 |
| [`UNIFIED_LOCAL_RELEASE_GATE.md`](UNIFIED_LOCAL_RELEASE_GATE.md) | 冻结证据（G20） | 可用：LC4/layout 8、0.75x、31-slot 的冻结历史与 218/218 Release CTest（提交 `e0729b2`）。不可用：任何 SC6-V3 声明——该物理层已被后续远控失真证伪 |
| [`UNIFIED_REMOTE_GATE.md`](UNIFIED_REMOTE_GATE.md) | 冻结证据（G21=`PASS_WITH_SINGLE_RUN_USER_WAIVER`） | 可用：真实远程像素链验收过程。不可用：无保留的远程性能 PASS——原始性能门为 FAIL |
| [`UNIFIED_G21_EXECUTION_HANDOFF_2026-09-06.md`](UNIFIED_G21_EXECUTION_HANDOFF_2026-09-06.md) | 历史交接（先读第 0 节最新增量） | 可用：0.13/0.14/0.15 的时点事实。不可用：0.15 之后的自动续做授权 |
| [`UNIFIED_G21_DELIVERY_2026-09-07.md`](UNIFIED_G21_DELIVERY_2026-09-07.md) | 冻结交付索引 | 可用：冻结产品/工具构建身份、包 hash、证据位置。不可用：现场结果（顶部已标注 supersession） |
| [`UNIFIED_G21_REMOTE_1GIB_RESULT_2026-09-07.md`](UNIFIED_G21_REMOTE_1GIB_RESULT_2026-09-07.md) | 冻结证据（G21 终态） | 可用：1 GiB 真像素恢复、跨机双摘要、原始性能门 `8,626.510998634209 B/unique` < 16 KiB 硬门失败、用户单次豁免范围。不可用：把豁免扩展到其它会话或 Citrix 链路 |
| [`UNIFIED_G22_GUI_RELEASE.md`](UNIFIED_G22_GUI_RELEASE.md) | 当前权威（G22=`PASS_LOCAL_CANDIDATE`，构建 `3a840a2`） | 可用：双端 GUI 重建范围、Windows 发布候选与检查项。不可用：现场/远程认证 |
| [`UNIFIED_G22_DELIVERY_2026-09-07.md`](UNIFIED_G22_DELIVERY_2026-09-07.md) | 冻结交付索引 | 可用：本地候选交付目录与包身份。不可用：Citrix 兼容性——由后续兼容修复文档解释 |
| [`UNIFIED_G22_CITRIX_DISPLAY_DIAGNOSTICS_2026-09-07.md`](UNIFIED_G22_CITRIX_DISPLAY_DIAGNOSTICS_2026-09-07.md) | 冻结证据（只读诊断） | 可用：Citrix 会话下 DXGI output 对应缺失的现场故障事实。不可用：修复有效性——见兼容修复文档 |
| [`UNIFIED_G22_ENCODER_DISPLAY_COMPAT_2026-09-07.md`](UNIFIED_G22_ENCODER_DISPLAY_COMPAT_2026-09-07.md) | **`LOCAL_REGRESSION_PASS / CITRIX_FIELD_PENDING`** | 可用：Encoder-only 无 DXGI-output 修复、冻结构建、1 MiB 本机 digest/publish/reopen 与 clean-startup 检查。不可用：宣称 Citrix 现场已修复——本机为 mapped hardware，原 Citrix 会话实屏仍待人工执行 |

## 5. C 组：协议、FEC、存储与 Golden（7 篇）

| 文档 | 状态 | 可用 / 不可用 |
| --- | --- | --- |
| [`PROTOCOL_1_DESCRIPTOR_SCHEMA.md`](PROTOCOL_1_DESCRIPTOR_SCHEMA.md) | 当前权威（Descriptor Schema 1） | 可用：`SessionDescriptor`/`SegmentDescriptor`/`FinalManifest` 字节合同、TLV/文件名/资源边界。不可用：任何 provisional 变体 |
| [`PHASE0_PROTOCOL_STATUS.md`](PHASE0_PROTOCOL_STATUS.md) | 历史（2026-09-03 已标注非当前规范） | 可用：Phase-0 切片实现史与当时 Gate。不可用：正式 Descriptor 依据；37-byte SessionDescriptor 现为必须拒绝 fixture |
| [`PHASE0_GATE_REPORT.md`](PHASE0_GATE_REPORT.md) | 冻结证据 | 可用：报告中列出的 commit/路径范围。不可用：新产品完成度 |
| [`PHASE1_GATE_REPORT.md`](PHASE1_GATE_REPORT.md) | **未跟踪受保护（不入库）** | 可用：本地阅读。不可用：任何 `git add`/修改/移动/删除；该链接仅指向工作树文件，仓库内不存在其 blob |
| [`ENCODER_STREAMING_CAROUSEL.md`](ENCODER_STREAMING_CAROUSEL.md) | 当前实现规范（G02/G09 已接入 G15） | 可用：预扫描、双 Segment Carousel、durable ID lease、状态 schema。不可用：视觉/接收 Gate 结论 |
| [`DECODER_RESUMABLE_RECOVERY.md`](DECODER_RESUMABLE_RECOVERY.md) | 当前实现规范（G03，已复用至产品 runtime） | 可用：journal、乱序 `.part`、重启重验、故障分类。不可用：post-rename crash 语义（留给 G05） |
| [`GOLDEN_VECTOR_HARNESS.md`](GOLDEN_VECTOR_HARNESS.md) | 当前工具合同 | 可用：`PBGoldenVectorCheck`/`PBVectorGen` 边界。不可用：无理由重生成 Golden；Golden 只证确定性，不证真实链路 |
## 6. D 组：视觉编码、呈现、选区、捕获与 Phase 1.5（13 篇）

| 文档 | 状态 | 可用 / 不可用 |
| --- | --- | --- |
| [`REFERENCE_RASTER.md`](REFERENCE_RASTER.md) | 当前权威（CPU 参考 raster/demod） | 可用：规范 raster 与序列化参考路径、SIMD/GPU 对照基准。不可用：性能结论 |
| [`DESKTOP_LEVELS_REFERENCE.md`](DESKTOP_LEVELS_REFERENCE.md) | 历史（Direct-Level CPU reference/测量基线） | 可用：历史量化方案与边界。不可用：当前产品 Profile 依据 |
| [`PRESENTATION.md`](PRESENTATION.md) | 当前权威（Data Window / `PBPresentTiming` 契约） | 可用：呈现线程、Present 语义与 epoch。不可用：把 Present 调用率当唯一帧率 |
| [`PRESENTATION_VALIDATION.md`](PRESENTATION_VALIDATION.md) | 冻结证据（2026-08-27，起点 `d5f27be`） | 可用：呈现验证范围与真实显示 Gate 边界。不可用：当前构建的呈现结论 |
| [`SCREEN_REGION.md`](SCREEN_REGION.md) | 当前权威（PMv2 物理像素 ROI 接口） | 可用：ROI 选择与坐标语义。不可用：逻辑像素/缩放坐标解释 |
| [`SCREEN_REGION_VALIDATION.md`](SCREEN_REGION_VALIDATION.md) | 冻结证据（人工 Gate 记录） | 可用：多显示器验证边界与人工检查条件。不可用：自动认证 |
| [`PBScreenCaptureWgc.md`](PBScreenCaptureWgc.md) | 当前权威（WGC lease 生命周期） | 可用：有界 lease、PB-owned texture、异步 ROI 退休。不可用：绕过 lease 生命周期 |
| [`PBScreenCaptureWgc_validation.md`](PBScreenCaptureWgc_validation.md) | 冻结证据 | 可用：当时验证范围。不可用：当前 capture 认证 |
| [`CAPTURE_BOOTSTRAP_IMPLEMENTATION.md`](CAPTURE_BOOTSTRAP_IMPLEMENTATION.md) | 历史实现记录（DXGI/CaptureNormalize/LocalDesktop Bootstrap） | 可用：capture→bootstrap 参考流水线。不可用：当前 RemoteVisual/Unified 视觉链认证 |
| [`LOCAL_DESKTOP_BOOTSTRAP.md`](LOCAL_DESKTOP_BOOTSTRAP.md) | 历史（Bootstrap 几何与诊断） | 可用：历史几何推导。不可用：当前 layout 10 几何 |
| [`SHAPE_CHROMA_D3D11_TELEMETRY_REPLAY.md`](SHAPE_CHROMA_D3D11_TELEMETRY_REPLAY.md) | 历史（Phase 1 A/B baseline） | 可用：ShapeChroma/D3D11/telemetry/replay 接线史。不可用：Certified Profile；原文已声明不把离线/WARP 冒充真实显示认证 |
| [`GUI_PHASE1_5.md`](GUI_PHASE1_5.md) | 历史（Phase 1.5 GUI；顶部已注 G16 收敛） | 可用：早期 GUI 状态机沿革。不可用：当前界面契约（见用户指南与 G22） |
| [`P1_5_REMOTEVISUAL_HARDENING_CHECKPOINT.md`](P1_5_REMOTEVISUAL_HARDENING_CHECKPOINT.md) | 冻结证据（Phase 1 检查点） | 可用：tag `phase1-gate-pass` 身份与 `RemoteVisualSmokePass=NotEvaluated`。不可用：RemoteVisual 已通过冒烟的暗示 |

## 7. E 组：RemoteVisual / LF4 历史路线（22 篇，已被统一产品路线取代）

**整组判定：** `PB-RemoteVisual-LF4-X1` 与 8×8 RemoteVisual 只保留为历史研究与可复用工具来源。[`REMOTE_VISUAL_LOW_FPS_TECHNICAL_ROUTE.md`](REMOTE_VISUAL_LOW_FPS_TECHNICAL_ROUTE.md) 顶部 2026-09-05 supersession note 优先于本组任何 STEP 文档；这些文档不得反向覆盖 `PB-Unified-SC6-V3`/layout 10 合同。

| 文档 | 状态 | 边界要点 |
| --- | --- | --- |
| [`REMOTE_VISUAL_LOW_FPS_TECHNICAL_ROUTE.md`](REMOTE_VISUAL_LOW_FPS_TECHNICAL_ROUTE.md) | 历史（2026-09-05 已 superseded） | 可用：LF4 研究史、码本/locator/freshness/信道建模思路。不可用：当前产品实施路线 |
| [`REMOTE_VISUAL_CHANNEL_MANIFEST.md`](REMOTE_VISUAL_CHANNEL_MANIFEST.md) | 历史（离线信道变换记录格式 v1/v2） | 可用：provider-generic 离线信道实验格式。不可用：PixelBridge wire、Session descriptor 或接收合同 |
| [`REMOTE_VISUAL_STEP06_CORPUS.md`](REMOTE_VISUAL_STEP06_CORPUS.md) | 冻结证据（corpus hardening，`1936c02`） | 可用：离线 impairment/adversarial corpus。不可用：真实远控结论 |
| [`REMOTE_VISUAL_STEP07_CALIBRATION.md`](REMOTE_VISUAL_STEP07_CALIBRATION.md) | 冻结证据（CPU/reference 软度量标定） | 可用：`lf4-default/PiecewiseLookup` 选择与 false-confidence Gate。不可用：GPU/现场结论 |
| [`REMOTE_VISUAL_STEP08_GOLDEN.md`](REMOTE_VISUAL_STEP08_GOLDEN.md) | 冻结证据（实验 Golden 冻结） | 可用：bit-exact 兼容 Gate。不可用：production GPU 部署或 Certified Profile |
| [`REMOTE_VISUAL_STEP09_ENCODER.md`](REMOTE_VISUAL_STEP09_ENCODER.md) | 冻结证据（DONE，Encoder-only candidate） | 可用：LF4 接入 `SenderFrameBuilder`/`EncoderRuntime` 的隐藏候选实现。不可用：生产部署声明 |
| [`REMOTE_VISUAL_STEP09_DYNAMIC_RDP_PILOT.md`](REMOTE_VISUAL_STEP09_DYNAMIC_RDP_PILOT.md) | 冻结证据（PASS，2026-09-01 120 s 动态 RDP 捕获 + 两次离线 Replay） | 可用：一次动态真实捕获事实。不可用：文件级传输或持续吞吐认证 |
| [`REMOTE_VISUAL_STEP10_D3D11_DEMOD.md`](REMOTE_VISUAL_STEP10_D3D11_DEMOD.md) | 冻结证据（DONE，实验 demod 路径） | 可用：PB-owned BGRA 直读的 D3D11 解调路径。不可用：production 部署结论 |
| [`REMOTE_VISUAL_STEP11_GPU_PARITY.md`](REMOTE_VISUAL_STEP11_GPU_PARITY.md) | 冻结证据（DONE，WARP/AMD/NVIDIA 同机 parity） | 可用：CPU/WARP/硬件真值一致性。不可用：跨机/跨环境认证 |
| [`REMOTE_VISUAL_STEP12_CAPTURE_LIFETIME.md`](REMOTE_VISUAL_STEP12_CAPTURE_LIFETIME.md) | 冻结证据（DONE，CaptureNormalize/epoch/D3D lifetime） | 可用：生命周期不变量的实现史。不可用：当前认证 |
| [`REMOTE_VISUAL_STEP13_TEMPORAL_ADMISSION.md`](REMOTE_VISUAL_STEP13_TEMPORAL_ADMISSION.md) | 冻结证据（DONE，有界队列/时序接纳） | 可用：以像素内 Bootstrap 为权威的接纳设计。不可用：产品默认路径声明 |
| [`REMOTE_VISUAL_STEP14_RECEIVER_ADMISSION.md`](REMOTE_VISUAL_STEP14_RECEIVER_ADMISSION.md) | 冻结证据（DONE，四码字进入 production Receiver） | 可用：admitted-index contract。不可用：LF4 文件传输完成 |
| [`REMOTE_VISUAL_STEP15_REPLAY_PRODUCTION.md`](REMOTE_VISUAL_STEP15_REPLAY_PRODUCTION.md) | 冻结证据（DONE，Replay v2 一致性） | 可用：live/offline 一致性与 GPU retirement 证明手法。不可用：把 receiver-only observation 当 sender truth |
| [`REMOTE_VISUAL_STEP16_TELEMETRY_REPORT.md`](REMOTE_VISUAL_STEP16_TELEMETRY_REPORT.md) | 历史遥测口径（RunReport.2） | 可用：FER/FEC 分母恒等式推导。不可用：当前报告口径（现行为 `RunReport.3`，见 G17） |
| [`REMOTE_VISUAL_STEP17_GUI_CLI_SCREEN_SAFETY.md`](REMOTE_VISUAL_STEP17_GUI_CLI_SCREEN_SAFETY.md) | 冻结证据（DONE，2026-09-02） | 可用：屏幕安全与显示器隔离约束。不可用：当前 GUI 契约 |
| [`REMOTE_VISUAL_STEP18_DEPLOYMENT_REPRODUCIBILITY.md`](REMOTE_VISUAL_STEP18_DEPLOYMENT_REPRODUCIBILITY.md) | 冻结证据（便携包/source set/RunId 闭环） | 可用：fail-closed 打包校验脚本思路。不可用：发布完成声明 |
| [`REMOTE_VISUAL_STEP19_LOCALDESKTOP_REGRESSION.md`](REMOTE_VISUAL_STEP19_LOCALDESKTOP_REGRESSION.md) | 冻结证据（同提交 LocalDesktop 回归未破坏 oracle） | 可用：回归未破坏冻结 oracle 的事实。不可用：远程链路证据（原文明确否定） |
| [`REMOTE_VISUAL_STEP20_REAL_REMOTE_PILOT.md`](REMOTE_VISUAL_STEP20_REAL_REMOTE_PILOT.md) | **`MANUAL-GATE`（从未标记 DONE）** | 可用：正式双机操作顺序、资源边界、fail-closed 执行工具。不可用：任何本地闭环/WARP/右屏截图把它当完成 |
| [`REMOTE_VISUAL_STEP20_SINGLE_MONITOR_DIAGNOSTIC.md`](REMOTE_VISUAL_STEP20_SINGLE_MONITOR_DIAGNOSTIC.md) | 冻结证据（`DiagnosticPixelLinkPass`；Step 20 仍 `MANUAL-GATE`） | 可用：单屏诊断事实与 ProtectedMonitor fail-closed 说明。不可用：正式文件级 Gate |
| [`REMOTE_VISUAL_STEP20_SINGLE_MONITOR_FILE_PILOT.md`](REMOTE_VISUAL_STEP20_SINGLE_MONITOR_FILE_PILOT.md) | 冻结证据（用户授权单屏全屏发送 pilot；`monitorSafety.preflightPassed=false` 如实记录） | 可用：单屏 ROI `[2560,0]-[5120,1440]` 的文件级事实。不可用：冒充双屏保护通过 |
| [`REMOTE_VISUAL_STEP21_PROVIDER_GENERIC_MATRIX.md`](REMOTE_VISUAL_STEP21_PROVIDER_GENERIC_MATRIX.md) | **`DONE`（用户取消跨品牌矩阵；扩展账本 `NOT_EXECUTED`、`formalStep21Accepted=false`）** | 可用：出口重冻结的定义与账本 fail-closed 设计。不可用：provider-generic 覆盖度结论 |
| [`REMOTE_VISUAL_STEP22_REPEAT_RECOVERY.md`](REMOTE_VISUAL_STEP22_REPEAT_RECOVERY.md) | 历史（被统一路线取代；`executedRunCount=0`） | 可用：未执行的 readiness 记录。不可用：重复恢复能力证明 |
## 8. F 组：G22 之后的非本机吞吐优化线（16 篇，2026-09-09 由用户停止）

**整组判定：** 该线**没有确立任何非本机吞吐收益**，没有晋级任何默认值，没有修改主区控制、layout 10、摘要、安全发布或 reopen 规则。停止是用户主动决策，不是技术阻塞，也不是任务完成。恢复工作只在用户重新开启目标后进行，且必须先读[暂停交接](REMOTE_THROUGHPUT_RESUME_HANDOFF_2026-09-09.md)。

| 文档 | 状态 | 可用 / 不可用 |
| --- | --- | --- |
| [`REMOTE_CHANNEL_THROUGHPUT_OPTIMIZATION_ROADMAP.md`](REMOTE_CHANNEL_THROUGHPUT_OPTIMIZATION_ROADMAP.md) | 停止时点的路线入口（顶部已注停止） | 可用：Step1–Step10 依赖与当前状态表（Step1/Step2 `PREPARED/PARTIAL`、Step3 `PARTIAL`、Step4 `IN_PROGRESS`、Step5 `IN_PROGRESS`/未晋级、Step6–10 `NOT_STARTED`）。不可用：页中历史“目标 active/下一步”当继续执行授权 |
| [`REMOTE_THROUGHPUT_RESUME_HANDOFF_2026-09-09.md`](REMOTE_THROUGHPUT_RESUME_HANDOFF_2026-09-09.md) | 当前唯一恢复导航 | 可用：30 秒现状、保护边界、可复用资产、不要重走的路径、恢复提示词。不可用：自动恢复执行授权；其中 HEAD 哈希是记账时点值，判定源码身份以 `git log -- libs apps tests tools` 为准 |
| [`REMOTE_OVERNIGHT_THROUGHPUT_2026-09-09.md`](REMOTE_OVERNIGHT_THROUGHPUT_2026-09-09.md) | 诊断日志（第 1–23 节，append-only） | 可用：各节时点事实、失败与构建 OOM 记录。不可用：把某一节的本机 WGC/右屏结果当远控收益；旧“继续/目标 active”已被停止状态覆盖 |
| [`REMOTE_STEP1_MEASUREMENT_CONTRACT.md`](REMOTE_STEP1_MEASUREMENT_CONTRACT.md) | 当前测量合同（尚未现场执行完整矩阵） | 可用：身份、计时分母、晚加入关联与现场操作顺序。不可用：任何“已完成现场测量”的推断 |
| [`REMOTE_STEP1_EXECUTION_2026-09-08.md`](REMOTE_STEP1_EXECUTION_2026-09-08.md) | `PREPARED/PARTIAL`，`fieldStatus=NOT_RUN` | 可用：本地测量身份/计时/晚加入关联与现场包准备。不可用：把 `30Hz_Remote.mkv` 或本地验证升级为远程吞吐认证 |
| [`REMOTE_STEP2_DIAGNOSTICS_AND_REPLAY_CONTRACT.md`](REMOTE_STEP2_DIAGNOSTICS_AND_REPLAY_CONTRACT.md) | 当前合同（仅本地范围） | 可用：逐阶段诊断与 OfflinePixels 回放字段。不可用：授权 Step3、实屏或非本机测试（原文明确否定） |
| [`REMOTE_STEP2_EXECUTION_2026-09-08.md`](REMOTE_STEP2_EXECUTION_2026-09-08.md) | 本地 `PREPARED/PARTIAL`；现场 `NOT_RUN` | 可用：录像损失分解。不可用：非本机 PASS；几何门数值边界未改 |
| [`REMOTE_STEP3A_EXECUTION_2026-09-08.md`](REMOTE_STEP3A_EXECUTION_2026-09-08.md) | `PARTIAL`（9 场景×2 轮 WARP/OfflinePixels 本地诊断 + 小文件闭环） | 可用：本地确定性与已知失败边界。不可用：目标硬件或现场信道结论 |
| [`REMOTE_STEP3B_EXECUTION_2026-09-08.md`](REMOTE_STEP3B_EXECUTION_2026-09-08.md) | `PARTIAL`（原始多帧 PASS，固定 codec `NOT_RECOVERED`） | 可用：最小固定 codec 对照与封存身份。不可用：把原始未压缩结果当 codec 链路可用 |
| [`REMOTE_GEOMETRY_G1_EXECUTION_2026-09-08.md`](REMOTE_GEOMETRY_G1_EXECUTION_2026-09-08.md) | **`NOT_PROMOTED_NO_NEW_ADMISSION`** | 可用：几何准入 G1 的执行与封存证据。不可用：开放新几何准入 |
| [`REMOTE_GEOMETRY_G1B_EXECUTION_2026-09-08.md`](REMOTE_GEOMETRY_G1B_EXECUTION_2026-09-08.md) | 有界研究结论 | 可用：判据论证与离线结果。不可用：晋级证据 |
| [`REMOTE_GEOMETRY_CODEC_ATTRIBUTION_2026-09-08.md`](REMOTE_GEOMETRY_CODEC_ATTRIBUTION_2026-09-08.md) | **`ATTRIBUTION_COMPLETE_CRITERION_NOT_ESTABLISHED`** | 可用：归因尝试与其判据缺口。不可用：几何/码流损失已被解释的结论 |
| [`REMOTE_FIXED_CODEC_OBSERVATION_CONTRACT_2026-09-08.md`](REMOTE_FIXED_CODEC_OBSERVATION_CONTRACT_2026-09-08.md) | 当前观测合同（第 22 节 A/B 依据） | 可用：固定 codec 边缘观测字段与边界。不可用：观测口径本身即收益证明 |
| [`REMOTE_RECEIVER_DECISION_DIAGNOSTICS_2026-09-08.md`](REMOTE_RECEIVER_DECISION_DIAGNOSTICS_2026-09-08.md) | 诊断补充（未改接纳规则） | 可用：接纳原因分布与原始对照。不可用：任何接纳规则已放宽的暗示 |
| [`REMOTE_CONTROL_PHASE_AB_2026-09-08.md`](REMOTE_CONTROL_PHASE_AB_2026-09-08.md) | 冻结单组现场结果（候选未晋级） | 可用：控制相位候选与一次现场事实。不可用：H2 首 GOP 小文件成功推及持续吞吐；后启动检查已失败，不得再降采样率或重复 40 MB/900 s 中和实验 |
| [`REMOTE_THROUGHPUT_ATTRIBUTION_2026-09-08.md`](REMOTE_THROUGHPUT_ATTRIBUTION_2026-09-08.md) | 冻结四轮归因（A1/B1/B2/A2） | 可用：原组 B 更快、反序观察 A 更快、A2 进程残留后人工结束与退出/预算失败。不可用：“稳定增益已证明”——不存在完全合规的反序配对；文内“目标保持进行中”仅属 2026-09-08 时点表述 |

## 9. G 组：仓库根与其他目录文档（非 `docs/`）

| 文档 | 状态 | 说明 |
| --- | --- | --- |
| [`../AGENTS.md`](../AGENTS.md) | 当前权威（工程纪律） | 未被本轮改动；协议不变量、资源安全、风格与 Git 纪律仍以它为准 |
| [`../README.md`](../README.md) | 当前入口（2026-09-09 已补 G22 之后状态段） | 含吞吐线停止状态与未关闭项摘要，并链接路线图与本矩阵 |
| [`../CONTRIBUTING.md`](../CONTRIBUTING.md) | 当前入口（2026-09-03） | 贡献者范围/构建/测试约定；其中历史 Gate 数字受 G20 冻结边界约束 |
| [`../tools/README.md`](../tools/README.md) 与各工具 README | 当前工具说明 | 2026-09 实验工具（Step1/Step2/Step3A/Step3B、Geometry G1/G1B、GeometryCodecProbe、ReceiverDecisionProbe、ExperimentalVisualSender、OriginalScreenReceiver、RemoteControlPhase、UnifiedRecordingReplay 等）大多为独立 `project()`，仅少数经 `tools/CMakeLists.txt` 接入主构建；本轮未构建未运行，说明文字属源码声明级核对。工具不得成为第二条 payload 通道 |
| [`../tests/UnifiedRemoteGate/README.md`](../tests/UnifiedRemoteGate/README.md)、[`FIELD_1GIB_README.md`](../tests/UnifiedRemoteGate/FIELD_1GIB_README.md) | 冻结现场流程说明 | G21 现场与 1 GiB 豁免流程说明；豁免仅限该次会话 |
| [`../benchmarks/README.md`](../benchmarks/README.md) | 当前工具说明 | 基准入口；理论容量与 bits/cell 不是 `VerifiedEncodedGoodput` |
| [`../fuzz/README.md`](../fuzz/README.md)、[`corpus/bootstrap-control.md`](../fuzz/corpus/bootstrap-control.md)、[`corpus/compression-zstd.md`](../fuzz/corpus/compression-zstd.md)、[`corpus/resume-state.md`](../fuzz/corpus/resume-state.md) | 当前工具说明 | fuzz target 与最小 corpus 说明；resume 状态仍按不可信持久输入处理 |
## 10. 本轮已修的状态冲突（保留原始记录，不改写旧结论）

| 冲突 | 处理 | 提交 |
| --- | --- | --- |
| 路线图 0.2 表把 Step5 记为 `NOT_STARTED`，与页首第 20–22 节已有结论矛盾 | Step4/Step5 状态回写为 `IN_PROGRESS`（Step5 明确“未晋级”），并注明旧值只保留路线落盘时点含义 | `80c9e0e` |
| 暂停交接仍描述清理前快照（HEAD `4a36d0f`、脏工作树、空 index） | 更正为清理后事实：HEAD、干净工作树、唯一未跟踪受保护文件、两项 CPU 优化已入库 | `80c9e0e` |
| `docs/README.md` 索引缺 11 篇（4 篇 2026-09-08 几何/固定 codec 记录 + 7 篇 G15–G21 证据） | 补登两张状态表并修正“Step5–10 未启动”旧表述 | `6c059c6` |
| Step1 的两个测量 CLI 入口、2026-09 实验工具的构建接入、G22 之后吞吐线结局未见于运行时清单/用户指南/首页 | 在 `CURRENT_RUNTIME_OPTION_INVENTORY.md`、`UNIFIED_USER_GUIDE.md`、`../README.md` 补齐，并标注为源码声明级核对 | `a39fe61` |
| 3 处指向本矩阵的相对链接悬空（仓库首页、`docs/README.md`、吞吐路线图） | 创建本文件后全部解析 | 本提交 |
| F 组 13 篇 2026-09-08 记录顶部仍写“目标保持进行中/下一步”，与 2026-09-09 停止状态冲突 | 只在 H1 之后追加一条停止状态横幅，正文一字未改，历史时点表述保留 | 本提交 |
| 暂停交接记录的 HEAD `8f74cd7` 已落后 | 追加勘误：纯文档提交不再逐个追记 HEAD 哈希，源码身份判定改用 `git log -- libs apps tests tools` | 本提交 |

## 11. 明确保留、不得被“整理”抹掉的事实

1. G21 真实远控 1 GiB 的原始性能门失败（`8,626.510998634209 B/unique` < 16 KiB 硬门）与 `PASS_WITH_SINGLE_RUN_USER_WAIVER` 的单次范围。
2. G20 的 `218/218 Release CTest` 属于 LC4/layout 8 冻结提交 `e0729b2`，不可用于 SC6-V3 声明。
3. G22 Encoder 显示兼容修复停留在 `LOCAL_REGRESSION_PASS / CITRIX_FIELD_PENDING`，Citrix 现场实屏仍待人工执行。
4. Step4 四轮归因中 A2 进程残留后人工结束、退出/预算失败，且不存在完全合规的反序配对。
5. 第 22 节空白带固定 codec A/B 两组均 `NOT_RECOVERED`（A：15 次 LocatorFailure + 15 次 CanvasClipped；B：30 次 CanvasClipped），主帧准入在 Outer FEC 之前被拒。
6. 第 21 节 layout 11 的 30 帧×5 条件恢复只证明 CPU reference + 原恢复库，不是原应用/GPU/codec/实屏证明。
7. 第 18 节 QC-LDPC 行归一化复用带来的 `1.8984 → 1.4471 ms` 是 CPU 解码阶段耗时（约 23.8%），**不是远控吞吐百分比**；Bootstrap 扫描游标同理。
8. Step1/Step2 现场 `NOT_RUN`、Step3 现场 `NOT_RUN`，任何本地/offscreen/artifact-only/录像/receiver-only 结果都不是现场认证。
9. RemoteVisual 历史：Step 20 仍为 `MANUAL-GATE`、Step 21 扩展矩阵 `NOT_EXECUTED`、Step 22 `executedRunCount=0`。
10. `docs/PHASE1_GATE_REPORT.md` 永久未跟踪、受保护；SHA-256 `076ef4c9b9f89eabccd323dbe4bffc4dc125ddaf96e6ee437d2cf5b1b1cea306`。

## 12. 停止时仍未关闭的事项（现状登记，不是执行授权）

| 事项 | 现状 | 关闭所需 |
| --- | --- | --- |
| 非本机整文件吞吐提升 | 未确立，用户主动停止 | 用户重新开启目标并从暂停交接第 5 节续做 |
| 4/3 显示采样适配（第 23 节） | 仅只读核对，未实现/构建/运行 | 新授权 + 新 root/build/output + 有界验证 |
| 空白带补充控制（layout 11） | CPU 参考通过、真实 codec 未恢复；控制槽未释放 | 独立显示采样变量 + 明确擦除预算，不降门限 |
| 以当前源码身份建立对照基线 | 未完成（`baseCommit`/`sourceFingerprintSha256` 仍是旧 build 注入值） | 重新 freeze、build、test、package、seal |
| Citrix Encoder 显示兼容 | `CITRIX_FIELD_PENDING` | 在原 Citrix 会话完整解压包、观察数据帧并人工按 Esc |
| Step1 完整现场矩阵 / Step2–Step3 现场门 | `NOT_RUN` | 现场窗口与人工操作（不使用输入自动化，不动左屏） |
| 工作树遗留 `display_probe.obj`（366,491 B，被 `*.obj` 忽略） | 未删除 | 需用户确认后才可清理 |

## 13. 维护规则增量

在 [`README.md`](README.md) 第 9 节规则之上追加：

10. **纯文档提交不追记 HEAD 哈希。** 状态文档记录 HEAD 只是记账时点；判断源码身份一律用 `git log --oneline -- libs apps tests tools CMakeLists.txt`，判断证据身份用封存包/实验目录内的 build 记录。
11. **新建或停用文档时，同时更新 [`README.md`](README.md) 索引与本矩阵**；本矩阵中每篇文档只出现一次，状态列必须能在该文档顶部原文找到依据。
12. **停止/暂停状态的权威位置是暂停交接文档**，其他文档只允许追加一条指向它的状态横幅，不得在多处复制停止理由与范围。

## 14. 覆盖与自检

- 本矩阵覆盖 `docs/` 下全部 80 篇 Markdown（含本文）：A 组 6、B 组 16、C 组 7、D 组 13、E 组 22、F 组 16，另有 G 组登记 `docs/` 之外文档。
- 相对链接完整性用脚本对 `docs/*.md`、仓库根 `README.md`、`CONTRIBUTING.md` 全量扫描：本矩阵创建前存在 3 处悬空链接（均指向本文），创建后已无悬空链接。
- 本轮未执行构建、测试、实屏、远程或大文件流程；未修改任何协议常量、Profile、Golden Vector、源码或封存 artifact。
- 复核方式：`git log --oneline -14`、`git status --porcelain -uall`、`git show --stat <hash>`，以及各文档顶部带日期的原文。