# PixelBridge 项目现状

> **2026-09-14 v0.6收尾：** 用户要求停止当前研究，封装今天最新实现、详细交接及续接prompt。P2正常源码修复Capture灰阶入口；用户确认的完整无裁切全屏已实现。最新全屏100MB同Q/P2封存二进制，首轮65%→100%后由782615ms降到525968ms（快32.79%），完整摘要/发布/重开及独立SHA通过，活动峰16→4；有效帧仍约7.51，未突破视觉链路上限。没有R的完整Medium/Big成绩；O旧居中完整合计4312469ms仍是历史最好，不得记到v0.6名下。正式默认名额8、实验预算约束名额及所有安全门不变。发布版本0.6.0的实际commit、exe哈希、包清单和验收结果以发布目录为准，旧候选实传不是新版本二进制实传。详细现状见[本次交接](SESSION_HANDOFF_20260914_V0.6.md)、[版本说明](RELEASE_V0.6.md)和会话findings §21.33–§21.37；[续接prompt](NEXT_TASK_PROMPT_V0.6.md)用于下一任务。

> **文档性质：** 本文件是项目现状的唯一入口级描述，取代此前分散在各 Gate 记录里的"当前状态"表述。
>
> **事实来源优先级（不变）：** 当前 `git log` 与仓库内容 -> 源码/测试/封存 artifact -> 各文档顶部带日期的原始记录 -> 本文件。冲突时以更早的原始记录为准；本文件负责指出冲突，不改写历史。
>
> **本轮整理边界（2026-09-09）：** `docs/` 的大规模清理属于**纯文档与 Git 记账**。整理过程中**未执行任何构建、单元测试、集成测试、实屏、远程桌面/Citrix 或大文件现场流程**，未修改任何源码字节、协议常量、Visual Profile、Golden Vector 或封存证据。因此本文件与同批提交**不构成任何新的验证、晋级或性能结论**。
>
> **本次整理时点：** HEAD `7e88a69`；最后一次改动源码的提交 `46a072b`。判断源码身份一律用 `git log --oneline -- libs apps tests tools CMakeLists.txt`；判断证据身份用封存包/实验目录内的 build 记录。

> **2026-09-14 非本机吞吐持续工作补记（尚未发布）：** 上述整理快照与下方旧“干净工作树”不是当前live状态。当前HEAD为`110fce3`，有未提交的GrayFast大文件调度、分段CLI、测量报告和定向回归改动；仍以纯视觉单向通路及全部恢复/资源安全门为约束。用户提供的954MB原始resume证明75.9MiB是已接受方程字节而非已完成文件段，相关勘误和候选A/B/C/D的失败/成功边界见[当前会话第21节](REMOTE_NONLOCAL_THROUGHPUT_SESSION_FINDINGS_20260911.md)。D已通过100MB/96个1MiB段的实际远控压力样本及100MB/6MiB段对照，但未据此宣布原始事故与任意大文件全部解决；原分辨率适配仍在隔离诊断，候选D的两个完整指定文件现已通过一轮验证，按同一本机接收进程时间线合计1小时55分26.594秒，最新候选E仅调整GrayFast为按新帧提交驱动Present，100MB/6MiB段实屏已完成517.754秒，比同参数D单次缩短2.82%，尚未重测E的两个完整指定文件；候选F的显式帧内多段交织通过36项定向回归、15项调度器测试及同参数100MB远控，耗时517.942秒，与E基本相同，未测出整文件提速；E仍为默认路径；大K门禁保留F/G失败后，H以四段常规窗口及有界尾窗通过38项定向回归，100MB同参数实屏493.751秒，比E单次缩短4.64%；H-clean以独立Sender状态根避免旧会话污染，完整Medium已验证21分37.610秒，相比D缩短7.23%；完整Big已验证1小时28分0.672秒，两个文件合计1小时49分38.282秒，比D缩短5分48.312秒（5.03%）；Big原生同Session恢复及全部摘要/发布/重开通过，但最终报告纠正了监控遗漏：41次有界orphan缓存拒绝，FEC配额拒绝/延期仍0，未放宽限制；详见§21.21。原分辨率Fast/GrayStates私有CPU诊断各全部16相位288/288码字恢复，随后原2560×1600上1MiB真实文件通过同Session、摘要/发布/重开及独立SHA256；该私有GDI/CPU参考证明后，隔离GPU移植通过128项hardware/WARP/原画布/实屏像素相位检查；原分辨率1MiB真实WGC/GPU文件也完成全部验证，仍是混合来源实验包，不是任意分辨率或正式发布认证。原分辨率100MB真实WGC/GPU现已验证463.633秒，比此前H/1080p组合单次减少30.118秒（6.10%），所有资源拒绝/冲突为0；仍有36次FEC擦除、有界丢帧，以及直接观测到合计184.15秒的已完成窗口重复等待。扩展缩放64项49项全恢复，另一个单pilot拟合CPU候选有退化已拒绝，尚未宣称任意分辨率完成（§21.24–§21.25）。后续I/J/K/L晚加入候选失败均保留；M缩小后续修复窗消除了已复现的名额延期链，但晚加入12505帧仍未达新增12000帧门。用户随后批准N预算约束动态名额实验，正式默认仍8、1GiB等预算不变；同一M Sender的五分之一保留帧样本37116→24481帧（-34.04%），活动峰13、预留552.53MiB、FEC/orphan拒绝0；四分之一晚加入无改善。N真实分配边界及恢复journal定向通过，显式CLI已接通并通过原分辨率1MiB及100MB真实WGC/GPU验证；100MB为464.746秒，与原463.633秒基本相同（+0.24%），实际峰值仅4个codec，且仍有184.48秒重复窗口等待。正式默认仍8，M旧晚加入等待门失败仍保留，不是可发布版本（§21.26–§21.28）。O把显式spatial首轮预算缩至65%、后续完整修复不缩，默认不开启；原分辨率同100MB实传327.833秒，比N减少29.46%，重复等待184.48→48.19秒，所有最终校验通过且资源拒绝0。但quarter保留率native样本更慢，不能推广成普遍改善或替换默认。O完整Medium现已验证860.828秒，比H-clean的1297.610秒减少33.66%；18段、全部最终校验及两次独立SHA256通过，资源/延期/冲突0。O完整Big也已验证3451.641秒=57分31.641秒，比H-clean缩短34.64%，单Receiver完成无需重开；全部摘要/安全发布/重开及两次独立SHA256通过，资源/延期/冲突0。O两个完整文件同候选合计4312.469秒=1小时11分52.469秒，比H-clean的6578.282秒减少2265.813秒（34.44%），成为当前完整实测最好组合；这是原分辨率/私有GPU/N/短首轮整组比较，不能归因于动态名额或推广到所有链路。Big前台句柄两端样本不同，不能宣称全程不变，本轮未使用焦点或输入操作（§21.29–§21.31）；已测H-clean包不变。本补记不改变正式Profile目录或Gate认证结论，详细结果以带运行身份的后续证据条目为准。v0.6（0.6.0，提交25a08aa）交付后的新会话已按交接完成test-only两阶段采样参考对照（canonical→全屏point/linear/area→独立分数远控缩放）：三模式在全部配置下精确恢复、CPU/真实Capture(WARP)逐字节一致、生产合成==point夹具绑定及全部相邻回归通过；area平均软度量最优但受8192/对比度²归一化混淆，有界噪声代理下三模式均保持满接收且area衰减最慢。用户批准继续后落地显式opt-in开关`--fullscreen-sampling point|linear|area`（默认point=历史逐字节行为；限灰阶家族单屏全屏；Encoder报告新增fullscreenSamplingMode），并完成25MB与100MB两级实传单变量A/B。25MB：point 127312 / area 124627（-2.11%）/ linear 123695ms（-2.84%），全部四门+同源双SHA通过；100MB（17段graduation路径）：point 526930（与R复现差+0.18%）/ linear 752227（-42.76%）/ area 878081ms（-66.64%），反转定论——采样合成成本压低Sender提交fps至18.5/15.6后每有效帧新信息量从25.3KB坍缩至17.2/14.9KB，接收有效帧率7.49-7.77与发送fps无关。候选不晋级：开关保持opt-in默认point、产品路径字节不变；继续该方向的前置是合成压至约≤10ms/帧或先做journal级novelty坍缩归因（findings §21.38–21.41）。随后按证据外推的point 60fps 100MB单变量测试同样证伪（914215ms，慢73.5%；Sender交付封顶约29.7fps形成抖动提交，每有效帧新信息坍缩到14.6KB）：30fps point 526930ms仍为100MB已知最好配置；今晚七组实验一致表明信道~7.5有效帧/秒为内容率上限，吞吐差异来自每有效帧新信息量，在交付变慢或抖动时坍缩（findings §21.42）。

## 1. 项目定义

PixelBridge 是 Windows x64 / C++20 项目，通过**可见桌面像素**做单向高吞吐文件传输。两个程序：

- `PixelBridgeEncoder.exe`：把文件切段、编码为视觉帧序列，并在指定屏幕区域呈现。
- `PixelBridgeDecoder.exe`：**只**从实际捕获的屏幕区域像素里恢复 payload。

硬约束（长期不变量，见 `../AGENTS.md` 与总体设计）：不得引入 socket、pipe、共享内存、COM、剪贴板、窗口消息、临时文件等任何第二条 payload 通道；未通过整文件摘要校验的输出不得算作已接受。

## 2. 当前产品合同（权威）

| 项 | 值 |
| --- | --- |
| 产品视觉合同 | `PB-Unified-SC6-V3` |
| `VisualProfileId` | `0x5042554E49534333` |
| layout | `10` |
| 规范画布 | 1920x1080 物理像素 |
| 实验身份（**未**进正式 catalog） | `PB-Experimental-BlankControl-1`，`0x504242414E443031`，layout `11` |

实验身份 `0x504242414E443031`/layout 11 只服务于吞吐优化线的空白带控制研究，**不是**产品 Profile，任何文档或代码都不得把它当作已冻结合同。

**2026-09-11 记账更新（提交 `da70009`…`ee1b25d`）：** 该实验身份已作为**显式 opt-in CLI 入口**进入产品二进制：双端 `--profile unified-bands`（GUI、默认值、产品 catalog、Golden Vector 均零改动）。带语义与冻结实验逐字节一致：RS(255,223) 全长码字、4×4 cell、中心 2×2 硬判决、96<luma<160 歧义拒整条、两带 {1056,16,608,64}/{256,1000,608,64}、内容=镜像当前帧控制槽记录；解码全程 fail-closed，每帧 ≤2 条记录、预分配 scratch。右屏实机冒烟通过（1 MiB：远程 Encoder → 第三方远控查看器 → 本机产品 Decoder CLI，三方 SHA-256 一致、Bootstrap 118 接纳/0 拒、letterbox scale≈1.2604/1.2611、补充带 236 试/188 接纳、exit 0）。证据根 `<PBLine root>\`（`session-log.md` §13、`bands-smoke\run7`、`evidence\bands06-encoder`）；实机证据基于 `619455d`/`1fbddb6` 工作树构建，当时未提交的身份修复链现已全部收录于 `ee1b25d`。本条目为状态登记，**不构成吞吐、晋级或产品合同变更结论**。

### 2.1 仍在工作树的权威与规范文档

| 文档 | 约束范围 |
| --- | --- |
| [`PixelBridge_最终技术路线与总体设计.md`](PixelBridge_最终技术路线与总体设计.md) | 长期总体设计：协议、FEC、视觉、GPU/capture、线程、存储、安全、验收 |
| [`UNIFIED_VISUAL_LARGE_FILE_IMPLEMENTATION_ROADMAP.md`](UNIFIED_VISUAL_LARGE_FILE_IMPLEMENTATION_ROADMAP.md) | G00-G22 产品合同与实现史（SC6-V3/layout 10 的落地契约） |
| [`PROTOCOL_1_DESCRIPTOR_SCHEMA.md`](PROTOCOL_1_DESCRIPTOR_SCHEMA.md) | `SessionDescriptor`/`SegmentDescriptor`/`FinalManifest` 字节合同、TLV、文件名与资源边界 |
| [`UNIFIED_TELEMETRY_REPORT.md`](UNIFIED_TELEMETRY_REPORT.md) | `PixelBridge.RunReport.3` 字段与 `VerifiedRawGoodput`/`VerifiedEncodedGoodput` 口径 |
| [`UNIFIED_G22_GUI_RELEASE.md`](UNIFIED_G22_GUI_RELEASE.md) | 双端 GUI 现行交互合同与 Windows 发布候选检查项 |
| [`CURRENT_RUNTIME_OPTION_INVENTORY.md`](CURRENT_RUNTIME_OPTION_INVENTORY.md) | 工作树里真实存在的 GUI/CLI/实验路径清单（区分产品路径与过渡路径） |
| [`UNIFIED_USER_GUIDE.md`](UNIFIED_USER_GUIDE.md) | 双端 GUI 日常操作与已知退出行为 |
| [`REFERENCE_RASTER.md`](REFERENCE_RASTER.md) | CPU 参考 raster/demod 与序列化参考路径（SIMD/GPU 对照基准） |
| [`PRESENTATION.md`](PRESENTATION.md) | Data Window 与 `PBPresentTiming` 呈现契约 |
| [`SCREEN_REGION.md`](SCREEN_REGION.md) | Per-Monitor DPI Aware V2 物理像素 ROI 接口与坐标语义 |
| [`PBScreenCaptureWgc.md`](PBScreenCaptureWgc.md) | WGC 帧池 lease 生命周期与 PB-owned texture 规则 |
| [`ENCODER_STREAMING_CAROUSEL.md`](ENCODER_STREAMING_CAROUSEL.md) | 预扫描、双 Segment Carousel、durable ID lease、状态 schema |
| [`DECODER_RESUMABLE_RECOVERY.md`](DECODER_RESUMABLE_RECOVERY.md) | journal、乱序 `.part`、重启重验、故障分类 |
| [`GOLDEN_VECTOR_HARNESS.md`](GOLDEN_VECTOR_HARNESS.md) | `PBGoldenVectorCheck`/`PBVectorGen` 工具边界 |

## 3. 仓库与工作区现状

| 项 | 现状 | 核对方式 |
| --- | --- | --- |
| 远端 | 仓库**无 remote**，全程未 push、未 reset、未 rebase、未重写历史 | `git remote -v` |
| 工作树 | 干净；唯一未跟踪的项目文件为受保护的 `docs/PHASE1_GATE_REPORT.md`（另可有本地会话目录 `.zcode/`，非项目内容） | `git status --porcelain -uall` |
| 受保护文件 | SHA-256 `076ef4c9b9f89eabccd323dbe4bffc4dc125ddaf96e6ee437d2cf5b1b1cea306`，永不 add/修改/移动/删除 | `Get-FileHash` |
| 源码最后改动 | `ee1b25d fix(app)`（2026-09-11 空白带实验身份以 opt-in 进入产品二进制，`da70009`…`ee1b25d` 五提交；此前最后源码改动为 `fca42e1 fix(tools)` 2026-09-10） | `git log -- libs apps tests tools CMakeLists.txt` |
| 工作区遗留 | 仓库根 `display_probe.obj`（366,491 B，被 `*.obj` 规则忽略）**未删除**，需用户确认后才可清理 | `Test-Path display_probe.obj` |
| 证据区 | `artifacts/`（约 12.9 GiB）、119 个 `build-*` 目录、现场录像与 raw 像素全部保留，本次整理未触碰 | 目录存在性 |

**封存身份规则（沿用整理前的记账）：** `8d60f77`…`46a072b` 只是把暂停时工作树里已有的源码改动入库，入库动作本身不代表这些改动在后续任何一轮里被重新构建或重新测试。`--measurement-build-identity` 的 `baseCommit` 与 `sourceFingerprintSha256` 均为编译期注入，只有重新构建后才反映新 HEAD；旧交付包、录像、raw 像素与实验封存目录仍按**封存时记录**的 build 身份解释。若要以当前源码身份作为新对照基线，必须重新 freeze、build、test、package、seal。

## 4. 能力边界：已证明到哪一步

### 4.1 已有冻结证据、可作依据（但边界严格受限）

| 能力 | 证据 | 边界 |
| --- | --- | --- |
| Descriptor/FEC/Receiver/journal/storage/publish 无屏幕闭环 | G04 CP-A，9 个 case | 原文 `visualChainCovered=false`，不得推出视觉链覆盖 |
| Encoder 产品工作流 | G15 | 数值不直接代表 SC6-V3/layout 10 现网 |
| Decoder 产品工作流 | G16 | 当时未跑实屏/远程/大文件 |
| 遥测与运行报告真值 | G17 `PixelBridge.RunReport.3` | Present/callback FPS 不是 goodput |
| 进程终止后恢复 | G18：256 MiB、9 个终止点 | 批准的无像素 headless，**不是**现场认证 |
| 大文件能力 | G19：20 GiB + 64 KiB、2,561 Segment、外部双摘要 | 能力边界，**不是**吞吐结论 |
| 本地发布门（历史） | G20：LC4/layout 8、0.75x、31-slot，218/218 Release CTest（提交 `e0729b2`） | **不可**用于任何 SC6-V3 声明；该物理层已被后续远控失真证伪 |
| 真实远程像素链 | G21：1 GiB 真像素恢复、跨机双摘要 | 终态 `PASS_WITH_SINGLE_RUN_USER_WAIVER`；**原始性能门 FAIL** |
| 双端 GUI | G22 `PASS_LOCAL_CANDIDATE`（构建 `3a840a2`） | 本地候选，非现场/远程认证 |
| Encoder 无 DXGI-output 兼容 | 本机 1 MiB digest/publish/reopen + clean-startup | `LOCAL_REGRESSION_PASS / CITRIX_FIELD_PENDING`；本机为 mapped hardware |

### 4.2 未证明、未认证、不得声称

1. **非本机整文件吞吐提升未确立。** 该线没有晋级任何默认值，没有修改主区控制、layout 10、摘要、安全发布或 reopen 规则。
2. **G21 的原始性能门是 FAIL**：`8,626.510998634209 B/unique` < 16 KiB 硬门；用户豁免**仅限该次会话**，不得扩展到其它会话或 Citrix 链路。
3. **Citrix 现场实屏未验证**：Encoder 显示兼容修复停在 `CITRIX_FIELD_PENDING`。
4. **Step1 完整现场矩阵、Step2/Step3 现场门均 `NOT_RUN`**；Step1/Step2 本地状态为 `PREPARED/PARTIAL`。
5. 任何本地、offscreen、artifact-only、录像、WARP、CPU reference、receiver-only 结果**都不是**现场认证。
6. RemoteVisual 历史路线：Step 20 仍 `MANUAL-GATE`、Step 21 扩展矩阵 `NOT_EXECUTED`（`formalStep21Accepted=false`）、Step 22 `executedRunCount=0`。

## 5. 两条研发线的状态

### 5.1 统一产品路线 G00-G22

已完成的 Gate 序列：G00-G22（协议/Descriptor/FEC/Carousel/恢复/遥测/工作流/大文件/本地门/远程门/GUI）。产品合同冻结在 `PB-Unified-SC6-V3` + layout 10，GUI 处于本地发布候选。逐 Gate 的终态、证据根与哈希见 [`EVIDENCE_INDEX.md`](EVIDENCE_INDEX.md)。

### 5.2 非本机吞吐优化线（G22 之后）——**进行中（2026-09-10 用户重开，2026-09-12 第二期收官）**

> 以下 2026-09-09 的"已停止"记录为历史事实，原样保留；重开后的状态见本节末尾的追加记录。

**2026-09-09 由用户主动停止**：非技术阻塞、非任务完成。恢复工作只在用户重新开启目标后进行。

- 该线共有 16 篇执行/诊断记录（Step1-Step5、四轮归因、几何准入、固定 codec 观测、夜间第 1-23 节）。
- 结论：**没有确立任何非本机吞吐收益**，未晋级任何默认值。
- 四轮归因 A1/B1/B2/A2：原组 B 更快、反序观察 A 更快，但 A2 主进程残留后由用户人工结束、exit1/预算告警，**不存在完全合规的反序配对**，因此"稳定增益已证明"不成立。
- 第 22 节空白带固定 codec A/B 两组均 `NOT_RECOVERED`（A：15 次 LocatorFailure + 15 次 CanvasClipped；B：30 次 CanvasClipped），主帧准入在 Outer FEC 之前被拒。
- CPU 侧的两项优化（QC-LDPC 行归一化复用 `1.8984 -> 1.4471 ms`，约 23.8%；Bootstrap 顺序 BGRA 扫描游标，中位 -12.2%/-9.6%）**只是对应 CPU 阶段耗时**，不是远控吞吐百分比。

**2026-09-10 至 2026-09-12 追加记录（第二期，用户重开该线并逐项授权）：**

- **机制模型确立**（真实远程画面链路实测，RunReport 口径）：`goodput = 符号接受率 × 唯一符号占比 × 1314 B`，在链路条件差异显著的两个不同日期均以 **<0.1% 误差**复现；接收端单核 CPU、符号接受率常数、重复符号致超线性劣化三条结论全部当日复验确认。
- **四项优化已入库**（用户批准的默认行为变更，`ctest -C Release -E PBPresentationGate` 全绿）：①增量喷泉 repair 调度（pass≥1 不再整段 K+20%，且按逻辑帧率分档：≤15 Hz 沿用 K+20%、>15 Hz 用 20%→40%→80%→160% 翻倍预算，`docs/ENCODER_STREAMING_CAROUSEL.md` §1.1 第 7 条）；②发送端活跃段窗口 8→6、接收端解码器配额解耦保持 8（`senderUnifiedReceiverActiveDecoderLimit`）；③锁定几何窗口化 Bootstrap（32.3→2.7 ms/帧，实机 10 万+帧 fast-path 命中率 >99.8%，含漂移守卫与全扫描回退）；④15 码字并行 Qc-LDPC FEC（私有解码车道，与串行逐位一致有专项测试与全量语料背书；`--stage-diagnostics` 运行自动回退串行）。另：headless Decoder CLI 常驻 RunMeasurementRecorder（RunReport.3 具备 stageCounters/captureFlow/processCpu 归因）+ `--stage-diagnostics` 开关。
- **现场单样本数字**（第三方面板远控链路、50/100 MiB、15/30 Hz、`--profile unified-bands`、同日同链路基线对比，全部 `digestMatch=true`）：30 Hz/50 MiB **94,803 → 196,212 B/s（2.07×）**；15 Hz/100 MiB 60,089 → 77,593 B/s（+29%）。两因子模型对每格预测误差 <0.1%。
- **边界**：以上为现场单样本工作口径，**不构成认证吞吐、晋级 Gate 或产品合同变更结论**；每格单次采样系用户批准的实验口径。
- **未竟**：O4 车道重分配（Chroma 5 槽改判 Luma，用户已授权，实施设计与五步顺序见 `docs/REMOTE_NONLOCAL_THROUGHPUT_SESSION_FINDINGS_20260911.md` §12.2）；15 Hz 档瓶颈已转移至发送端空口重复占比，O4 是唯一正交杠杆。
- 证据根：`<PBLine root>\`、`<PBLine root>\`（vb-/opt-/o2v-/o3v-/o4v-/o5v-/o6v- 系列 runs 与 evidence 日志）；完整过程记录于 `docs/REMOTE_NONLOCAL_THROUGHPUT_SESSION_FINDINGS_20260911.md` §1-§12。

## 6. 度量口径（不得混淆）

| 口径 | 定义位置 | 常见误用 |
| --- | --- | --- |
| `VerifiedRawGoodput` | [`UNIFIED_TELEMETRY_REPORT.md`](UNIFIED_TELEMETRY_REPORT.md) | 用理论 bits/cell 或画布容量替代 |
| `VerifiedEncodedGoodput` | 同上 | 用发送端编码字节速率替代"已验证"字节 |
| `UniqueVisualFPS` | 同上；`MinUpdateInterval`/刷新率/Present 调用率都不是证据 | 把 Present 或 callback 频率当唯一帧率 |
| 最终真值 | **最终发布并 reopen 的整文件时间**除以唯一逻辑帧数 | 用中间态、首 GOP、单段或完成点媒体 PTS 替代 |

规则补充：

1. 只有**最终发布并 reopen 的整文件**时间，加上 `VerifiedRawGoodput`/`VerifiedEncodedGoodput`，才是吞吐真值。
2. Step1 的主时长定义为 `finalReopenVerified - startAccepted`。晚加入场景必须以 `(SessionTag, FrameSequence)` 精确关联 Sender `events.jsonl`，**不得跨主机直接相减时间**，也不得插值。
3. `RunReport.3.measurement` 中失败或缺失的计时、覆盖、身份必须写为 `null`/failed，**不得制造样本**；测量环、报告、证据和时长都有上限。
4. 理论容量、离线 artifact 展开率、录像身份率、CPU reference 阶段耗时，都不能替代 `VerifiedEncodedGoodput`。

## 7. 未关闭事项（现状登记，不是执行授权）

| 事项 | 现状 | 关闭所需 |
| --- | --- | --- |
| 非本机整文件吞吐提升 | 2026-09-10 用户重开；第二期四项优化入库（提交 d37d98f…03a79a9），现场单样本 30 Hz/50 MiB 达基线 2.07×（196,212 B/s）；**未做认证级多样本矩阵** | O4 车道重分配实施（设计见 findings §12.2）+ 认证口径多样本矩阵 |
| 4/3 显示采样适配（夜间记录第 23 节） | 仅只读核对，未实现/构建/运行 | 新授权 + 新 root/build/output + 有界验证 |
| 空白带补充控制（layout 11） | CPU 参考通过；真实 codec 未恢复；控制槽未释放 | 独立显示采样变量 + 明确擦除预算，不降门限 |
| 以当前源码身份建立对照基线 | 未完成（`baseCommit`/`sourceFingerprintSha256` 仍是旧 build 注入值） | 重新 freeze、build、test、package、seal |
| Citrix Encoder 显示兼容 | `CITRIX_FIELD_PENDING` | 在原 Citrix 会话完整解压包、观察数据帧并人工按 Esc |
| Step1 完整现场矩阵 / Step2-Step3 现场门 | `NOT_RUN` | 现场窗口与人工操作（不使用输入自动化，不动左屏） |
| 工作区遗留 `display_probe.obj` | 未删除 | 需用户确认 |

## 8. 文档地图

### 8.1 `docs/`（2026-09-09 整理后 18 篇，另有后续新增）

| 类别 | 文档 |
| --- | --- |
| 现状入口 | `PROJECT_STATUS.md`（本文）、`README.md`（薄索引） |
| 证据索引 | `EVIDENCE_INDEX.md` |
| 历史与恢复 | `DOC_HISTORY.md` |
| 总体设计 | `PixelBridge_最终技术路线与总体设计.md` |
| 产品合同与实现史 | `UNIFIED_VISUAL_LARGE_FILE_IMPLEMENTATION_ROADMAP.md` |
| 协议字节 | `PROTOCOL_1_DESCRIPTOR_SCHEMA.md` |
| 遥测真值 | `UNIFIED_TELEMETRY_REPORT.md` |
| GUI 与运行时 | `UNIFIED_G22_GUI_RELEASE.md`、`CURRENT_RUNTIME_OPTION_INVENTORY.md`、`UNIFIED_USER_GUIDE.md` |
| 视觉/呈现/捕获 | `REFERENCE_RASTER.md`、`PRESENTATION.md`、`SCREEN_REGION.md`、`PBScreenCaptureWgc.md` |
| 流式与恢复 | `ENCODER_STREAMING_CAROUSEL.md`、`DECODER_RESUMABLE_RECOVERY.md` |
| 工具合同 | `GOLDEN_VECTOR_HARNESS.md` |
| 远程实验通道 | `REMOTE_OPS_BRIDGE.md`（2026-09-10 新增；SMB 文件协议操作桥的机制与用法入口） |
| 非本机吞吐线第二期 | `REMOTE_NONLOCAL_THROUGHPUT_SESSION_FINDINGS_20260911.md`（2026-09-11 新增；两因子模型、四项优化机制与实测、O4 实施设计。会话工作日志性质：现状判定以本文件与 `EVIDENCE_INDEX.md` 为准） |

### 8.2 `docs/` 之外

| 文档 | 状态 |
| --- | --- |
| [`../AGENTS.md`](../AGENTS.md) | 当前权威（工程纪律：协议不变量、资源安全、C++ 风格、Git 纪律） |
| [`../README.md`](../README.md) | 当前入口（含吞吐线停止状态与未关闭项摘要） |
| [`../CONTRIBUTING.md`](../CONTRIBUTING.md) | 当前入口（贡献范围/构建/测试约定） |
| `../tools/**/README.md` | 各工具说明；2026-09 实验工具多为独立 `project()`，仅少数经 `tools/CMakeLists.txt` 接入主构建。工具不得成为第二条 payload 通道 |
| `../tests/UnifiedRemoteGate/README.md`、`FIELD_1GIB_README.md` | G21 现场与 1 GiB 豁免流程说明；豁免仅限该次会话 |
| `../benchmarks/README.md`、`../fuzz/**` | 基准与 fuzz 入口；resume 状态仍按不可信持久输入处理 |
| `../third_party/README.md`、`WIREHAIR_BASELINE.md` | 第三方基线与 SBOM 依据 |

### 8.3 本次 `docs/` 整理

2026-09-09 对 `docs/` 做了结构性清理：`git rm` 64 篇历史/冻结/诊断类记录，保留 14 篇现行权威与规范，新增本文件、`EVIDENCE_INDEX.md`、`DOC_HISTORY.md` 三篇。**所有被删除文件都可按 `DOC_HISTORY.md` 记录的 blob SHA-1 逐字节取回**，删除清单、原状态与取回命令见 [`DOC_HISTORY.md`](DOC_HISTORY.md)。

## 9. 续做与恢复入口

1. **先读** 本文件第 2/4/7 节，确认产品合同与"未证明"边界。
2. **查证据** ：[`EVIDENCE_INDEX.md`](EVIDENCE_INDEX.md) 给出每个 Gate 与实验的终态、证据根路径、关键哈希。
3. **取历史** ：需要旧 Gate 全文时按 [`DOC_HISTORY.md`](DOC_HISTORY.md) 的 blob 取回，不要凭记忆重述。
4. **改协议/Profile 前** ：先读总体设计与 `PROTOCOL_1_DESCRIPTOR_SCHEMA.md`，并遵守 wire 兼容 + Golden Vector 要求。
5. **Git 纪律** ：显式路径、原子、非 amend；禁止 `git add -A` / `git add .`；不 reset/rebase/重写历史；提交前后执行 `git diff --cached --check`。
6. **保护文件** ：`docs/PHASE1_GATE_REPORT.md` 永不 add/commit/修改/移动/删除，始终保持唯一未跟踪。
7. **现场动作由人执行** ：不干扰左屏、鼠标或键盘；只用右侧测试屏；不做输入自动化。

## 10. 文档维护规则

在 `../README.md`/`../AGENTS.md` 既有纪律之上：

1. **状态只有一个入口。** 现状写在本文件，证据结论写在 `EVIDENCE_INDEX.md`，历史文档的去处写在 `DOC_HISTORY.md`；其他文档不得再复制一份"当前状态"。
2. **纯文档提交不追记 HEAD 哈希。** 判断源码身份用 `git log --oneline -- libs apps tests tools CMakeLists.txt`；判断证据身份用封存包/实验目录内的 build 记录。
3. **冻结证据只允许追加勘误**，不得改写当时的结论、数值或失败事实。
4. **失败与豁免必须保留。** 任何整理、重写或精简都不得抹掉失败门限、豁免范围与未认证边界。
5. **新建或停用文档时同步更新** `docs/README.md` 索引与本文件第 8.1 节。
6. **删除文档一律用 `git rm`** 并在 `DOC_HISTORY.md` 登记 blob SHA-1，保证逐字节可恢复；不建 `docs/archive/` 目录。
