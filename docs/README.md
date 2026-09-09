# PixelBridge 文档索引

**2026-09-09 目录整理与文档状态记账：** 在用户停止非本机吞吐目标后，同一日按用户要求整理项目目录并校正文档状态。`4a36d0f` 之后由整理产生的提交只暂存显式路径，未移动、改名或删除 `artifacts/`、`build-*`、现场录像或封存证据包；工作树唯一未跟踪文件是按用户要求长期保留、始终不入库的受保护报告 `PHASE1_GATE_REPORT.md`。本轮属于纯文档与 Git 记账：未构建、未运行测试、未执行任何实屏或远程现场流程，因此不构成任何新的验证结论。全量文档状态矩阵见 [`DOCUMENT_STATUS_2026-09-09.md`](DOCUMENT_STATUS_2026-09-09.md)。

**2026-09-09 暂停交接（最新入口）：用户已要求停止本次任务。** 下次继续非本机吞吐优化先读[暂停交接与恢复入口](REMOTE_THROUGHPUT_RESUME_HANDOFF_2026-09-09.md)。第22节固定codec A/B已封存复验；第23节4/3显示适配仅只读准备，未实现/构建/运行。实际非本机提速仍未确立，不能按下方历史“下一步”自动继续。

**2026-09-09第22节：** 空白带唯一固定codec A/B已完成，实际码流属性与输入/检查器负例通过，但两组均NOT_RECOVERED。A有15帧定位失败/15帧CanvasClipped，B虽30帧Bootstrap通过，仍30帧CanvasClipped，未进入主FEC；不能据此判定主区无损或提速。局部60条补充带有21条硬判决码字精确相同，但任一低置信度cell即丢整条的规则仅3条通过；这不是RS实际解码或接纳证据。原始回归与控制晚到整文件guard通过，正式layout10/主控制未改。下一步先复用已验证4/3显示几何和本轮码流，不重跑同一失败入口、不降门限。见[夜间记录第22节](REMOTE_OVERNIGHT_THROUGHPUT_2026-09-09.md)。

**2026-09-09第21节：** 空白带已进入独立新身份layout11的完整原始画面参考验证：30帧×5条件（无补充、无遮挡、右上/左下/双遮挡）均恢复262144B，原ReceiverIngress/Storage摘要/安全发布/reopen及外部双摘要通过，150条主区记录一致。只证明CPU reference＋原恢复库，未跑原应用/GPU、codec或实屏；原layout10和主区控制未改，无远控提速结论。3次构建失败及资源原因保留，最终/O2在2GiB内通过。见[夜间记录第21节](REMOTE_OVERNIGHT_THROUGHPUT_2026-09-09.md)。

**2026-09-09第20节：** 空白带短控制原型已在独立608×64原始patch证明完整121/194/114B控制记录可单条恢复；61项分组检查、原ControlPlaneReceiver的重复/冲突/资源规则及Python独立像素/RS/CRC审计通过。超195B合法控制不截断而不使用补充区；两条独立失效。此为artifact-only原型，不是正式布局、整画面/codec/整文件或远控提速证据。原layout10及主区控制未改；失败fixture/source/build完整保留。见[夜间记录第20节](REMOTE_OVERNIGHT_THROUGHPUT_2026-09-09.md)。

**2026-09-09第19节：** 新定位＋新纠错组合已完成一次实际WGC两段整文件接收：8454144B、39.4210093s、UniqueVisualFPS14.9921，原摘要/安全发布/reopen和外部双摘要通过、双方正常退出。此为本机无codec、FEC迭代0的组合集成检查，不是非本机收益认证；旧现场包未替换。用户空白区/遮挡建议已进入只读容量核对，正式layout10未改。见[夜间记录第19节](REMOTE_OVERNIGHT_THROUGHPUT_2026-09-09.md)。以下为各时点历史增量。

**2026-09-09第18节增量：复用两份既有压缩视频，新locator的240次完整观察/工作量与原版一致。原CPU oracle固定75组软判决上，QC-LDPC仅复用每行最小值归一化，675对码字/状态/错误/迭代相同，阶段均值1.8984→1.4471ms（约23.8%下降，非远控吞吐百分比）。40个相关用例通过；新locator+FEC接回原GPU/Receiver后180条非处理时钟帧记录完全一致，全彩仍拒绝、中和同媒体PTS6000ms整文件摘要/发布/reopen通过。周期resumeGeneration快照差异186→159如实保留，不冒充断点恢复认证。没有新编码/实屏/远控，本节新增FEC还未经过实际WGC，未替换现场包。见[夜间记录第18节](REMOTE_OVERNIGHT_THROUGHPUT_2026-09-09.md)。**

**2026-09-09最新实现（第17节）：** 已定位并优化原Bootstrap全图扫描的重复地址/格式分支，不减少扫描、阈值或校验。两固定像素交替CPU对照中位耗时16.988→14.909ms、21.638→19.570ms，全部观察/资源计数一致；49个受影响用例及1070项零分配检查通过。新扫描对象已接回原WGC/DecoderRuntime/Receiver，4/3右屏两段8,454,144B在39.574339s完成原摘要/发布/reopen及外部双摘要，双方正常退出。仅是已验证本机CPU成本优化与集成通过，**没有新实际远控提速认证**；不改FPS/Profile/远控设置，不替换现场包。见[夜间记录第17节](REMOTE_OVERNIGHT_THROUGHPUT_2026-09-09.md)。

**2026-09-09当前诊断（第16节）：** 原硬件WGC/Decoder在固定全彩15Hz、8MiB+64KiB两段源下，1:1与4/3实际像素均完成整文件摘要/发布/reopen，分别39.296s/40.123s、UniqueVisualFPS 14.992/14.644。4/3本身在此次无远控压缩条件下未复现现场约10–11Hz，但不能据此归因网络：现场非零FEC迭代、突发到达仍未被模拟。初始scaled实验适配器错误固化可重试EpochMismatch，失败保留；仅修正工具适配器、另建目录后通过。下一步拆分Bootstrap Map/定位/Unmap成本，不改协议、不继续低容量held-neutral提速晋级。见[夜间记录](REMOTE_OVERNIGHT_THROUGHPUT_2026-09-09.md)。

本目录同时保存当前产品路线、核心规范、模块说明、历史实现记录和人工 Gate 证据。阅读时必须区分“当前合同”“当前实现清单”和“历史证据”，不能因为某个历史 Step 已通过，就推断新的统一视觉产品已经完成。

**2026-09-09最新决策：** [夜间记录第15节](REMOTE_OVERNIGHT_THROUGHPUT_2026-09-09.md)完成原生未包装/中和保持两段整文件实屏，39.288218s/176.8616631s，两者原摘要/发布/reopen和外部双摘要全通过、正常退出。新增独立有界入口并未改变原runtime/Receiver。但对照正常B2现场190,186B/s及Chroma 11949/11955通过，held-neutral理想稳态容量仅98,550B/s，缺少提速余量；因此停止该候选的现场提速晋级，保留弱信道恢复参考，不继续40MB/codec扩跑或做提速包。后续回到保留有效色度与足够容量的时序/到达率分析；本地或模型成功不等于非本机优化完成。以下为历史增量。

**2026-09-09最新增量：** [夜间记录第14节](REMOTE_OVERNIGHT_THROUGHPUT_2026-09-09.md)把冻结压缩候选扩大到256KiB/K=200，固定8秒视频、前2秒仅媒体预运行、原Receiver接收其余6秒。两组raw均完整恢复；相同8M/4M-VBV条件下，中和版在媒体PTS6000ms完成摘要/发布/reopen和独立双摘要，彩色未建立Session、Outer接纳0。90次Bootstrap成功不等于建立Session，彩色125次正确数据观察被原规则WaitingForSession拒绝。新编码仅2次，不调参数、不重跑失败源；仍为固定模型证据，非实际远控速度认证。

**2026-09-09当前增量：** [夜间记录第13节](REMOTE_OVERNIGHT_THROUGHPUT_2026-09-09.md)已接通新实验Sender到原Decoder的右屏实际像素整文件链。固定64 KiB、同保持包装的彩色/色度中和两轮均通过原摘要/发布/reopen和停机后外部双摘要，分别1.2138502s/1.5310232s；本机无codec压力，中和并不更快。新无GUI Receiver工具只启动原库、不注入payload/Replay/服务。不是非本机速度认证，不改产品默认；第11–12节中的“实屏整文件未验证”已由第13节窄增量更新，其他限制保留。

**2026-09-09后续夜间进度：** [第11–12节](REMOTE_OVERNIGHT_THROUGHPUT_2026-09-09.md)更新：固定色度中和候选通过一个独立源/新Session的原GPU/Receiver整文件验证，彩色对照仍未恢复。工具专用实验发送入口完成C++像素一致性、4组定向检查和右屏双模式各5秒启动/退出；它尚未由实屏Decoder完成整文件，也没有新的非本机收益认证，不新增认证Profile、不改产品默认。以下条目是各时间点的历史增量。

**2026-09-09最新进度：** [夜间自主推进记录第5–7节](REMOTE_OVERNIGHT_THROUGHPUT_2026-09-09.md)记载了不改原GPU/Receiver准入的4/3回放，以及H2整画面保持两帧候选的首次固定codec整文件成功和干净复现。它尚受启动阶段偏差限制，不是持续吞吐或真实远控收益；不改产品默认。下面Step3/Step4各历史条目保留其原证据边界。

## 1. 新参与者的最短阅读路径

按以下顺序阅读即可建立当前上下文：

1. [`../AGENTS.md`](../AGENTS.md)：工程、安全、代码风格、测试和 Git 纪律；
2. [`../README.md`](../README.md)：项目入口、目录、构建方式和当前事实边界；
3. [`../CONTRIBUTING.md`](../CONTRIBUTING.md)：面向 GitHub 贡献者的范围、构建、测试和提交约定；
4. [`UNIFIED_VISUAL_LARGE_FILE_IMPLEMENTATION_ROADMAP.md`](UNIFIED_VISUAL_LARGE_FILE_IMPLEMENTATION_ROADMAP.md)：G00–G22 现有产品合同、实现历史与验收证据；
5. [`REMOTE_CHANNEL_THROUGHPUT_OPTIMIZATION_ROADMAP.md`](REMOTE_CHANNEL_THROUGHPUT_OPTIMIZATION_ROADMAP.md)：**G22之后的非本机吞吐优化入口**；Step1/Step2仍PREPARED/PARTIAL，Step3 PARTIAL/codec NOT_RECOVERED；Step4单候选的A1/B1/B2/A2四轮核心整文件/身份核验已完成，原组B更快、反序接收观察A更快，且A2进程残留后人工结束、退出/预算失败；无完全合规反序配对，不证明稳定增益、不晋级默认值。保留全部缺项和失败，Step4 整体 IN_PROGRESS；Step5 已进入未晋级结论（roadmap 第 20–22 节：layout 11 CPU 参考验证通过、空白带固定 codec 两组 NOT_RECOVERED），Step6–10 未启动；用户已于 2026-09-09 主动停止本目标，非技术阻塞；
6. [`UNIFIED_G21_EXECUTION_HANDOFF_2026-09-06.md`](UNIFIED_G21_EXECUTION_HANDOFF_2026-09-06.md)：先读第0节最新增量；0.15为最终远控结果与单次用户豁免，0.13/0.14保留本机三档和交付历史；
7. [`UNIFIED_G21_REMOTE_1GIB_RESULT_2026-09-07.md`](UNIFIED_G21_REMOTE_1GIB_RESULT_2026-09-07.md)：最终Windows远程桌面1 GiB的功能PASS、原始性能FAIL、跨机摘要与单次豁免边界；
8. [`PixelBridge_最终技术路线与总体设计.md`](PixelBridge_最终技术路线与总体设计.md)：完整总体架构与长期不变量；
9. [`CURRENT_RUNTIME_OPTION_INVENTORY.md`](CURRENT_RUNTIME_OPTION_INVENTORY.md)：G22产品入口与保留的旧诊断区别；旧表为历史记录；
10. 与当前目标直接相关的模块文档和测试。

发生冲突时，优先遵循当次用户要求、`AGENTS.md`、当前任务对应路线，再使用总体设计中未被该路线明确取代的内容。现有产品合同仍由 G00–G22 统一路线解释；新吞吐路线中的实验假设不是已冻结协议。Phase 0、Phase 1.5、RemoteVisual Step 文档不得反向覆盖新产品合同。

## Step2 本地诊断增量（2026-09-08）

- [Step2 诊断与 OfflinePixels 合同](REMOTE_STEP2_DIAGNOSTICS_AND_REPLAY_CONTRACT.md)
- [Step2 实施、录像损失分解和待确认事项](REMOTE_STEP2_EXECUTION_2026-09-08.md)
- 本地目标完成不等于非本机 PASS。新发现的几何准入数值边界尚未修改；不自动进入 Step3。

## Step3-A 本地确定性工具增量（2026-09-08，后续单独获准执行）

- [Step3-A 实施、两轮精确重复、小文件闭环与已知失败边界](REMOTE_STEP3A_EXECUTION_2026-09-08.md)
- [独立工具、固定样例、资源预算与复验命令](../tools/PBRemoteThroughputStep3A/README.md)
- 9 场景×2 轮仅证明软件 WARP/OfflinePixels 的本地诊断与小文件恢复；G1/G1B 几何门未修改，Step3-B／G2／Step4 未启动，非本机现场仍 NOT_RUN。

## 2. 当前权威文档

### 夜间自主非本机吞吐推进（2026-09-09）

- [新观察版包、实际4/3显示比例与固定codec差分](REMOTE_OVERNIGHT_THROUGHPUT_2026-09-09.md)：用户授权不提问、自主代码及本机双屏测试。双端观察包已通过本地构建、15项定向用例及10项解压启动检查；同一codec在4/3显示缩放后3张CPU样本接受15/5/0块，而1:1均为0。未改几何门，尚无新GPU整文件/现场提速证明；总目标active，继续闭合可用于吞吐筛选的完整恢复链。

### 非本机控制相位 A/B（2026-09-08，后续独立授权）

- 后续用户明确要求继续以非本机吞吐优化为目标：见[四轮归因与已有观察量补齐](REMOTE_THROUGHPUT_ATTRIBUTION_2026-09-08.md)。现场报告未写出生产快照已有的capture/demod/Outer计数；仅输出增量已通过5 cases/80 assertions，无新collector、无接纳/资源/wire更改。总目标保持active，尚无新包或提速证据，不转入退出修复主线。

- [周期控制相位候选、A/B包和单组现场结果（第9节）](REMOTE_CONTROL_PHASE_AB_2026-09-08.md)
- 原节奏与相位分散的定向 scheduler、测量模型、原始像素整文件和新解压启动检查通过；本轮 60/60 正常像素预算已用完。
- 用户确认当前为向日葵，不是Citrix；provider仅作环境元数据。当前PathFix1操作包的一组A/B整文件和精确迟加入关联均通过：A238.3155474s、B211.5549573s。只支持本组B更快，不支持稳定或因果收益；A延后审计及B本机辅助wrapper缺失如实记录。旧RunId/长路径失败保留，Step3 codec阻断未变，Step4单组子目标完成并停止，不自动扩展后续测试。
- 23:24后续独立授权的反序B2→A2已收束：B2为212.477132s且正常收尾；A2为205.0048921s，但Encoder窗口关闭后进程残留，用户人工结束后原Launcher记录exit1/999.1670919s/预算告警true。四轮核心文件核验通过不等于全部运行检查通过；无合规反序配对、不证明稳定收益。最终报告、失败边界和待确认退出修复计划见执行记录10.3及`artifacts/remote-field-reverse-20260908-run01/four-run-comparison01/REPORT.md`。本目标停止，不自动实施下一步或补跑。

### Receiver 接纳原因诊断补充（2026-09-08）

- [接纳原因、历史控制就绪归因与原始开/关对照](REMOTE_RECEIVER_DECISION_DIAGNOSTICS_2026-09-08.md)
- [独立工具、字段含义、预算及只读复验](../tools/PBReceiverDecisionProbe/README.md)
- 本地最小诊断完成：60/60 原始观察，两组整文件/摘要/发布/reopen 通过，旧非时钟 trace 与历史原始对照一致。没有 codec 重跑或发送节奏改动；Step3 仍 PARTIAL、现场 NOT_RUN、Step4 未启动。

### Step3-B 后续独立获准增量（2026-09-08）

- [Step3-B 多帧原始对照、固定有状态码流、颜色失败修正及几何拒绝](REMOTE_STEP3B_EXECUTION_2026-09-08.md)
- [独立工具、冻结参数、资源边界与复验命令](../tools/PBRemoteThroughputStep3B/README.md)
- 64 KiB / 30帧 / 15fps / libx264 8Mbit/s / VBV4Mbit / GOP15，正常观察93/93；原始多帧闭环通过，两次codec精确一致但未恢复。保持原几何门、生产Decoder/Receiver、安全发布与资源限制；现场NOT_RUN，整个Step3仍PARTIAL。上面的Step3-A段落保留当时状态，不代表后续Step3-B未执行。

| 文档 | 作用 | 当前状态 |
| --- | --- | --- |
| [`REMOTE_CHANNEL_THROUGHPUT_OPTIMIZATION_ROADMAP.md`](REMOTE_CHANNEL_THROUGHPUT_OPTIMIZATION_ROADMAP.md) | 非本机固定环境下的迟加入吞吐优化；远控软件仅作元数据（本轮向日葵）；录像诊断、Step1–Step10、离线筛选及退出条件 | **Step1/Step2 PREPARED/PARTIAL；Step3 PARTIAL/codec NOT_RECOVERED；Step4单候选一组A/B现场核验完成，本组B更快但未认证稳定收益，子目标停止；Step5–10未启动** |
| [`REMOTE_STEP3B_EXECUTION_2026-09-08.md`](REMOTE_STEP3B_EXECUTION_2026-09-08.md) | 固定有状态codec、多帧闭环、属性/失败检查、运行身份与封存证据 | **本地最小诊断完成；codec恢复未通过；不修改几何门或晋级后续Step** |
| [`REMOTE_STEP1_MEASUREMENT_CONTRACT.md`](REMOTE_STEP1_MEASUREMENT_CONTRACT.md) | B0/M1 身份隔离、固定 encoded ledger、Start 到 final reopen 计时、迟加入精确关联及人工现场操作 | **Step1 本地测量合同；不授权实屏或远程** |
| [`REMOTE_STEP1_EXECUTION_2026-09-08.md`](REMOTE_STEP1_EXECUTION_2026-09-08.md) | Step1 实际修改、构建、负例、独立包、输入和执行证据 | **本地完成；最终身份和现场待验收项入口** |
| [`UNIFIED_G22_DELIVERY_2026-09-07.md`](UNIFIED_G22_DELIVERY_2026-09-07.md) | 最终双端EXE/ZIP、冻结身份、hash、实屏证据和旧目录清理 | **G22 PASS_LOCAL_CANDIDATE；3a840a2** |
| [`UNIFIED_G22_CITRIX_DISPLAY_DIAGNOSTICS_2026-09-07.md`](UNIFIED_G22_CITRIX_DISPLAY_DIAGNOSTICS_2026-09-07.md) | Citrix会话Encoder启动故障、独立元数据诊断工具与现场证据 | **截图确认DXGI output对应缺失；修复范围已获同意** |
| [`UNIFIED_G22_ENCODER_DISPLAY_COMPAT_2026-09-07.md`](UNIFIED_G22_ENCODER_DISPLAY_COMPAT_2026-09-07.md) | Encoder 屏幕/硬件身份解耦、独立候选与回归 | **df2bcfbd 本地实屏PASS；Encoder-only已交付，Citrix现场待验证** |
| [`UNIFIED_G22_GUI_RELEASE.md`](UNIFIED_G22_GUI_RELEASE.md) | 用户新授权的双端 GUI 重建、确认事项、最小验证和 Windows 发布候选 | **本地候选已交付；保留全部阶段/失败记录** |
| [`UNIFIED_USER_GUIDE.md`](UNIFIED_USER_GUIDE.md) | 新 GUI 的文件/帧率/全屏/Esc、目录/限定屏幕 ROI/接收进度及 CLI 日志说明 | G22 用户指南；包和实屏门禁见工作记录 |
| [`UNIFIED_VISUAL_LARGE_FILE_IMPLEMENTATION_ROADMAP.md`](UNIFIED_VISUAL_LARGE_FILE_IMPLEMENTATION_ROADMAP.md) | 冻结既有产品行为；记录 G00..G22 的实现、最小测试和退出条件 | **现有合同与历史证据；后续吞吐优化见新路线** |
| [`UNIFIED_G21_EXECUTION_HANDOFF_2026-09-06.md`](UNIFIED_G21_EXECUTION_HANDOFF_2026-09-06.md) | 最新0.15最终远控结果/单次豁免；0.13同候选三档、0.14交付及更早失败保持历史 | **G21 最终交接入口** |
| [`UNIFIED_G21_REMOTE_1GIB_RESULT_2026-09-07.md`](UNIFIED_G21_REMOTE_1GIB_RESULT_2026-09-07.md) | 最终远控1 GiB身份、像素权威、双摘要、原始Gate失败与用户单次豁免 | **G21=`PASS_WITH_SINGLE_RUN_USER_WAIVER`** |
| [`UNIFIED_G21_DELIVERY_2026-09-07.md`](UNIFIED_G21_DELIVERY_2026-09-07.md) | 统一交付目录、冻结产品与工具构建身份、包hash、测试命令和原始证据位置 | 历史交付索引；现场结果见最终结果文档 |
| [`PixelBridge_最终技术路线与总体设计.md`](PixelBridge_最终技术路线与总体设计.md) | 协议、FEC、视觉、GPU/capture、线程、存储、安全、验收的总架构 | 权威背景；顶部 supersession 规则优先 |
| [`CURRENT_RUNTIME_OPTION_INVENTORY.md`](CURRENT_RUNTIME_OPTION_INVENTORY.md) | 说明工作树中实际存在的 GUI/CLI/实验路径和未接通能力 | 过渡实现清单，不是最终产品说明 |
| [`GITHUB_PUBLISH_CHECKLIST.md`](GITHUB_PUBLISH_CHECKLIST.md) | 首次推送前、GitHub 仓库配置和源码发布检查 | 当前发布准备入口 |
| [`../README.md`](../README.md) | 仓库首页、快速构建与真实完成状态 | 对外入口 |

### 几何准入与固定 codec 归因（2026-09-08）

| 文档 | 作用 | 当前状态 |
| --- | --- | --- |
| [`REMOTE_GEOMETRY_G1_EXECUTION_2026-09-08.md`](REMOTE_GEOMETRY_G1_EXECUTION_2026-09-08.md) | 几何准入 G1 的实际执行、身份与封存证据 | **NOT_PROMOTED_NO_NEW_ADMISSION**；未开放新准入 |
| [`REMOTE_GEOMETRY_G1B_EXECUTION_2026-09-08.md`](REMOTE_GEOMETRY_G1B_EXECUTION_2026-09-08.md) | 围绕 G1 判据的有界补充论证与实验边界 | 有界研究结论；不构成晋级证据 |
| [`REMOTE_GEOMETRY_CODEC_ATTRIBUTION_2026-09-08.md`](REMOTE_GEOMETRY_CODEC_ATTRIBUTION_2026-09-08.md) | 固定 codec 场景下几何/码流损失的归因尝试 | **ATTRIBUTION_COMPLETE_CRITERION_NOT_ESTABLISHED** |
| [`REMOTE_FIXED_CODEC_OBSERVATION_CONTRACT_2026-09-08.md`](REMOTE_FIXED_CODEC_OBSERVATION_CONTRACT_2026-09-08.md) | 固定 codec 边缘观测合同、字段与观测边界 | 当前观测合同；第 22 节 A/B 依据此合同 |

### 统一产品合同与 Gate 证据（G15–G21）

| 文档 | 作用 | 当前状态 |
| --- | --- | --- |
| [`UNIFIED_ENCODER_WORKFLOW.md`](UNIFIED_ENCODER_WORKFLOW.md) | Unified Encoder 产品工作流（G15） | 历史验收证据（2026-09-04 headless/controller/GUI smoke）；产品绑定现为 `PB-Unified-SC6-V3`/layout 10，G15 数字属历史 |
| [`UNIFIED_DECODER_WORKFLOW.md`](UNIFIED_DECODER_WORKFLOW.md) | Unified Decoder 产品工作流（G16） | 历史验收证据；当时未跑实屏/远程/大文件门 |
| [`UNIFIED_TELEMETRY_REPORT.md`](UNIFIED_TELEMETRY_REPORT.md) | G17 统一遥测与 `PixelBridge.RunReport.3` 报告合同 | 当前遥测/报告口径来源；Bootstrap-only 缓存身份问题经批准做最小修复 |
| [`UNIFIED_PROCESS_RESTART_RECOVERY.md`](UNIFIED_PROCESS_RESTART_RECOVERY.md) | G18 256 MiB 真实进程终止与恢复 | 已通过批准的无像素 headless 验收；非现场认证 |
| [`UNIFIED_LARGE_FILE_CAPABILITY.md`](UNIFIED_LARGE_FILE_CAPABILITY.md) | G19 20 GiB+ headless 大文件能力 | 已通过（20 GiB+64 KiB、2561 Segment、终止后恢复、外部双摘要）；能力证据而非吞吐证据 |
| [`UNIFIED_LOCAL_RELEASE_GATE.md`](UNIFIED_LOCAL_RELEASE_GATE.md) | G20 本地 Release Gate | **冻结历史证据：LC4/layout 8、0.75x、31-slot 不改写；218/218 Release CTest 属冻结提交 `e0729b2`；不可用于 SC6-V3 声明** |
| [`UNIFIED_REMOTE_GATE.md`](UNIFIED_REMOTE_GATE.md) | G21 真实远程像素链验收 | 2026-09-07 `PASS_WITH_SINGLE_RUN_USER_WAIVER`；真像素 1 GiB 恢复，原始性能门 FAIL（8,626.511 B/unique < 16 KiB 硬门），豁免仅限该次会话 |

> `PHASE1_GATE_REPORT.md` 在 `docs/` 目录中存在，但按用户要求长期**不加入 Git**：它保持未跟踪、受保护，不得暂存、修改、移动或删除（SHA-256 见[暂停交接](REMOTE_THROUGHPUT_RESUME_HANDOFF_2026-09-09.md)）。它不是待清理的垃圾文件。

## 3. 协议、FEC、恢复与存储

| 文档 | 内容 | 解释边界 |
| --- | --- | --- |
| [`PHASE0_PROTOCOL_STATUS.md`](PHASE0_PROTOCOL_STATUS.md) | Bootstrap/Control、历史 provisional descriptor、Transport、resource policy、Inner/Outer FEC、旧 resume 基础 | 历史实现报告；37-byte SessionDescriptor 已成为必须拒绝 fixture |
| [`PROTOCOL_1_DESCRIPTOR_SCHEMA.md`](PROTOCOL_1_DESCRIPTOR_SCHEMA.md) | 正式 Protocol 1.0 Descriptor Schema 1 字节表、TLV/文件名/资源边界与 Golden manifest | 当前正式 Descriptor 规范 |
| [`ENCODER_STREAMING_CAROUSEL.md`](ENCODER_STREAMING_CAROUSEL.md) | Encoder 预扫描、双 Segment Carousel、durable ID lease、状态 schema 与 headless report | G02 当前实现规范；不等于视觉/接收 Gate |
| [`DECODER_RESUMABLE_RECOVERY.md`](DECODER_RESUMABLE_RECOVERY.md) | Decoder journal、乱序 `.part`、重启重验、状态转换与故障分类 | G03 当前实现规范；post-rename crash 留给 G05 |
| [`UNIFIED_VISUAL_CP_A_HEADLESS.md`](UNIFIED_VISUAL_CP_A_HEADLESS.md) | 正式 Descriptor/Outer/Receiver/journal/storage/publish 的 9-case 多 Segment 无屏幕闭环与 working-set 证据 | G04 CP-A；明确 `visualChainCovered=false` |
| [`GOLDEN_VECTOR_HARNESS.md`](GOLDEN_VECTOR_HARNESS.md) | Golden 生成、验证与工具边界 | Golden 变更时阅读；不能无理由重生成 |
| [`PHASE0_GATE_REPORT.md`](PHASE0_GATE_REPORT.md) | Phase-0 Gate 的既有证据和限制 | 只支持报告中列出的 commit/路径，不等于新产品完成 |

正式 Descriptor、流式发送和 decoder journal 的当前代码入口：

- `libs/PBProtocol/include/pbprotocol/descriptor_codec.h`
- `libs/PBProtocol/include/pbprotocol/protocol_types.h`
- `apps/common/encoder_session_store.h`
- `apps/common/sender_carousel_scheduler.h`
- `apps/common/decoder_resume_store.h`
- `libs/PBReceiver/include/pbreceiver/receiver_ingress.h`
- `libs/PBStorage/include/pbstorage/output_file.h`

这些接口在 `1445f9b` 建立了实现基础；完整大文件、故障注入和统一视觉产品接线仍按路线 G01..G05、G18..G20 关闭。

## 4. 视觉编码、呈现、选区与捕获

### 4.1 通用基础

| 文档 | 内容 |
| --- | --- |
| [`REFERENCE_RASTER.md`](REFERENCE_RASTER.md) | 规范 raster、序列化和参考路径 |
| [`DESKTOP_LEVELS_REFERENCE.md`](DESKTOP_LEVELS_REFERENCE.md) | 历史 Direct-Level CPU reference 和边界 |
| [`PRESENTATION.md`](PRESENTATION.md) | D3D11 Data Window 架构、线程与 Present 语义 |
| [`PRESENTATION_VALIDATION.md`](PRESENTATION_VALIDATION.md) | 呈现验证与真实显示 Gate 边界 |
| [`SCREEN_REGION.md`](SCREEN_REGION.md) | PMv2 物理像素 ROI 选择接口 |
| [`SCREEN_REGION_VALIDATION.md`](SCREEN_REGION_VALIDATION.md) | ROI/多显示器验证和人工 Gate |
| [`PBScreenCaptureWgc.md`](PBScreenCaptureWgc.md) | WGC capture 生命周期和 PB-owned texture |
| [`PBScreenCaptureWgc_validation.md`](PBScreenCaptureWgc_validation.md) | WGC 验证边界 |
| [`CAPTURE_BOOTSTRAP_IMPLEMENTATION.md`](CAPTURE_BOOTSTRAP_IMPLEMENTATION.md) | capture→bootstrap 参考流水线 |
| [`LOCAL_DESKTOP_BOOTSTRAP.md`](LOCAL_DESKTOP_BOOTSTRAP.md) | LocalDesktop Bootstrap 几何与诊断 |
| [`SHAPE_CHROMA_D3D11_TELEMETRY_REPLAY.md`](SHAPE_CHROMA_D3D11_TELEMETRY_REPLAY.md) | 历史 Shape+Chroma/D3D11/telemetry/replay 接线 |

### 4.2 统一产品与历史 Profile 的关系

当前唯一产品 Profile 是 `PB-Unified-SC6-V3`、layout 10：6×6 分隔单元、Base/Fine/Chroma 独立 lane、region-local placement、codeword-local sequence permutation、1.0x..2.0x 和 mixed Control/Transport slots。SC6 V2/layout 9、LC4/layout 8 与旧 LF4 只保留为历史 Golden/回归，不能覆盖当前合同；SC6 V3 已完成最终远控1 GiB功能恢复，G21以用户单次明确性能豁免关闭，原始16 KiB失败仍保留。

旧 Direct、Shape+Chroma、RemoteVisual 8x8 和 `PB-RemoteVisual-LF4-X1`：

- 可以复用码本、locator、freshness、transform、GPU lifetime 和证据工具；
- 保留为内部 A/B、历史 Golden 或回归 fixture；
- 不再作为最终 GUI 的用户可选 Profile；
- 不能把旧 LF4 的 `1..5 Hz`、0.5x 下限或四 codeword 容量迁移成 Unified 合同；
- 不能按远控 provider 名称选择阈值或解码分支。

## 5. Phase 1.5 GUI 与过渡 runtime

| 文档 | 内容 | 状态 |
| --- | --- | --- |
| [`GUI_PHASE1_5.md`](GUI_PHASE1_5.md) | 旧 GUI controller/presentation 架构与控件绑定 | 历史/过渡实现 |
| [`CURRENT_RUNTIME_OPTION_INVENTORY.md`](CURRENT_RUNTIME_OPTION_INVENTORY.md) | 当前实际可达入口、隐藏能力和限制 | 当前事实清单 |
| [`P1_5_REMOTEVISUAL_HARDENING_CHECKPOINT.md`](P1_5_REMOTEVISUAL_HARDENING_CHECKPOINT.md) | 旧 RemoteVisual 加固检查点 | 历史证据 |

当前最终 GUI 以 G22 用户确认后的重建和交付文档为准，G15/G16 是历史收敛阶段。Qt 只能负责 presentation/controller；协议、FEC、modulation、capture、storage 和 telemetry 核心不得依赖 Qt。

## 6. RemoteVisual 历史路线与证据

[`REMOTE_VISUAL_LOW_FPS_TECHNICAL_ROUTE.md`](REMOTE_VISUAL_LOW_FPS_TECHNICAL_ROUTE.md) 及 Step 06..22 文档记录 2026-09-03 以前的低刷新 LF4 研究、Golden、GPU、Replay、field/package 工具和证据边界。它们很有复用价值，但已由统一路线取代为产品实施入口。

| 文档 | 历史主题 |
| --- | --- |
| [`REMOTE_VISUAL_CHANNEL_MANIFEST.md`](REMOTE_VISUAL_CHANNEL_MANIFEST.md) | provider-generic channel metadata 与矩阵身份 |
| [`REMOTE_VISUAL_STEP06_CORPUS.md`](REMOTE_VISUAL_STEP06_CORPUS.md) | transform corpus |
| [`REMOTE_VISUAL_STEP07_CALIBRATION.md`](REMOTE_VISUAL_STEP07_CALIBRATION.md) | metric calibration |
| [`REMOTE_VISUAL_STEP08_GOLDEN.md`](REMOTE_VISUAL_STEP08_GOLDEN.md) | LF4 Golden freeze |
| [`REMOTE_VISUAL_STEP09_ENCODER.md`](REMOTE_VISUAL_STEP09_ENCODER.md) | immutable LF4 Encoder raster |
| [`REMOTE_VISUAL_STEP09_DYNAMIC_RDP_PILOT.md`](REMOTE_VISUAL_STEP09_DYNAMIC_RDP_PILOT.md) | 动态真实像素 pilot |
| [`REMOTE_VISUAL_STEP10_D3D11_DEMOD.md`](REMOTE_VISUAL_STEP10_D3D11_DEMOD.md) | D3D11 Compute demod |
| [`REMOTE_VISUAL_STEP11_GPU_PARITY.md`](REMOTE_VISUAL_STEP11_GPU_PARITY.md) | 历史 accepted-byte GPU parity |
| [`REMOTE_VISUAL_STEP12_CAPTURE_LIFETIME.md`](REMOTE_VISUAL_STEP12_CAPTURE_LIFETIME.md) | CaptureEpoch、lease、adapter lifetime |
| [`REMOTE_VISUAL_STEP13_TEMPORAL_ADMISSION.md`](REMOTE_VISUAL_STEP13_TEMPORAL_ADMISSION.md) | duplicate/reorder/stale admission |
| [`REMOTE_VISUAL_STEP14_RECEIVER_ADMISSION.md`](REMOTE_VISUAL_STEP14_RECEIVER_ADMISSION.md) | Receiver/Outer 接线 |
| [`REMOTE_VISUAL_STEP15_REPLAY_PRODUCTION.md`](REMOTE_VISUAL_STEP15_REPLAY_PRODUCTION.md) | Replay production path |
| [`REMOTE_VISUAL_STEP16_TELEMETRY_REPORT.md`](REMOTE_VISUAL_STEP16_TELEMETRY_REPORT.md) | 指标真实性和 report merger |
| [`REMOTE_VISUAL_STEP17_GUI_CLI_SCREEN_SAFETY.md`](REMOTE_VISUAL_STEP17_GUI_CLI_SCREEN_SAFETY.md) | GUI/CLI 与双屏安全 |
| [`REMOTE_VISUAL_STEP18_DEPLOYMENT_REPRODUCIBILITY.md`](REMOTE_VISUAL_STEP18_DEPLOYMENT_REPRODUCIBILITY.md) | package/SBOM/deployment identity |
| [`REMOTE_VISUAL_STEP19_LOCALDESKTOP_REGRESSION.md`](REMOTE_VISUAL_STEP19_LOCALDESKTOP_REGRESSION.md) | LocalDesktop 回归 |
| [`REMOTE_VISUAL_STEP20_REAL_REMOTE_PILOT.md`](REMOTE_VISUAL_STEP20_REAL_REMOTE_PILOT.md) | 真实远程 pilot 总体证据 |
| [`REMOTE_VISUAL_STEP20_SINGLE_MONITOR_DIAGNOSTIC.md`](REMOTE_VISUAL_STEP20_SINGLE_MONITOR_DIAGNOSTIC.md) | 单屏诊断边界 |
| [`REMOTE_VISUAL_STEP20_SINGLE_MONITOR_FILE_PILOT.md`](REMOTE_VISUAL_STEP20_SINGLE_MONITOR_FILE_PILOT.md) | 单屏完整文件 pilot |
| [`REMOTE_VISUAL_STEP21_PROVIDER_GENERIC_MATRIX.md`](REMOTE_VISUAL_STEP21_PROVIDER_GENERIC_MATRIX.md) | 未执行/缩减矩阵与单次成功边界 |
| [`REMOTE_VISUAL_STEP22_REPEAT_RECOVERY.md`](REMOTE_VISUAL_STEP22_REPEAT_RECOVERY.md) | 旧重复恢复 readiness；不是已完成的新路线 Gate |

复用历史证据时必须同时记录其 commit、ProfileId、layout、尺度、捕获 backend、是否经过 Receiver/WholeFileDigest/publish，以及未覆盖项。仅有 receiver-only Replay 或 GPU source readback 不能证明最终文件恢复。

## 7. 代码目录与文档责任

| 路径 | 责任 |
| --- | --- |
| `apps/PixelBridgeEncoder` | Encoder Qt、CLI adapter、controller 入口 |
| `apps/PixelBridgeDecoder` | Decoder Qt、CLI adapter、controller 入口 |
| `apps/common` | Qt-free application model/runtime、Session persistence、resume、report |
| `libs/PBProtocol` | Bootstrap/Control/Transport/Descriptor、CRC、摘要、资源校验 |
| `libs/PBCompression` | bounded zstd/RAW Segment 处理 |
| `libs/PBOuterFec` | DirectRepeat/Wirehair V2 |
| `libs/PBInnerFec` | Robust QC-LDPC reference |
| `libs/PBModulation` | CPU raster/oracle、profile、pilots、metrics |
| `libs/PBDemodD3D11` | D3D11 Compute 解调 |
| `libs/PBCaptureNormalize` | WGC/DXGI 共用的 PB-owned texture、epoch、队列 |
| `libs/PBScreenCaptureWgc` | WGC backend |
| `libs/PBScreenCaptureDxgi` | DXGI Desktop Duplication backend |
| `libs/PBRenderD3D` | 独立 Data Window 和 presentation epoch |
| `libs/PBReceiver` | control/data admission、active Outer decoder、conflict/resource 状态 |
| `libs/PBStorage` | `.part`、random verified writes、whole digest、safe publish |
| `libs/PBTelemetry` | 指标、计数和报告 schema |
| `tests` | Catch2/CTest、Golden、静态和 opt-in native Gate |
| `fuzz` | parser/compression/FEC/resume fuzz target 与最小 corpus |
| `tools` | 诊断、Golden、evidence、package 工具；不能成为隐藏 payload 路径 |
| `third_party` | overlay port、固定基线与 license 说明；不提交 installed binaries |

如果公共 wire、Profile、持久状态或报告 schema 改变，必须在同一目标中同步其规范/Golden/compatibility note。普通内部重构不需要把每个实现细节复制到总体设计。

## 8. 测试文档与证据解释

- 默认 CTest 测试不应弹窗、移动鼠标或改变显示设置。
- Presentation/ScreenRegion/WGC/DXGI/LocalDesktop 的 native Gate 都是 opt-in，并需要显式目标显示器坐标。
- G00–G22 统一路线的完整 Release CTest 和真实右屏检查点为 G20；新吞吐路线仅在其最终集成检查点安排完整回归，普通 Step 保持定向验证。
- 性能结论必须使用最终验证／发布／重开后的 verified bytes 与明确的时间分母；每帧指标另用实际唯一逻辑帧。新分区协议不能改写旧 `UniqueVisualFPS`，理论容量、Present FPS 和 callback FPS 不是 goodput。
- Golden 只证明给定输入下的确定性语义；不能证明真实远程链路。
- WARP 只证明 D3D11 参考/兼容路径；不能替代目标硬件 adapter。
- Build/GUI smoke 只证明装载和基本生命周期；不能证明视觉恢复。

## 9. 文档维护规则

1. 当前产品决策变更：先更新统一路线顶部和冻结合同，再更新总体设计 supersession note。
2. Goal 完成：更新路线中目标状态、提交和证据；不要删掉未通过/未执行历史。
3. 历史 Step 文档：原则上只追加勘误或 supersession note，不改写旧结论的上下文。
4. 新模块文档：文件名使用稳定的英文大写或明确模块名；中文总体设计文件保留原名。
5. 文档中的命令应从仓库根目录可执行，并明确是否会打开窗口、占用显示器或产生大文件。
6. 所有链接使用仓库相对路径；证据 artifact 不应依赖开发机的临时 build 路径。
7. 未经明确批准，本地临时 Gate 报告、运行日志和用户文件不加入 Git。
8. 顶部带日期的状态日志与下方状态表冲突时，以最新日期条目为准；但整理文档时必须把最新结论回写状态表，任何一次性结论、失败记录或用户豁免都不得被静默抹掉。
9. `docs/PHASE1_GATE_REPORT.md` 由用户长期保留但不入库：整理、暂存或提交时不得暂存、修改、移动或删除它。全量文档状态视图见 [`DOCUMENT_STATUS_2026-09-09.md`](DOCUMENT_STATUS_2026-09-09.md)，该矩阵只做索引与状态标注，不构成第二套真相；事实来源仍是 `git log` 与各文档顶部带日期的原始记录。

## 10. GitHub 发布入口

首次推送前执行 [`GITHUB_PUBLISH_CHECKLIST.md`](GITHUB_PUBLISH_CHECKLIST.md)。当前源代码仓库不应包含 build tree、vcpkg 安装目录、`.part`、resume journal、日志、压缩发布包或真实传输数据。项目自身 LICENSE、GitHub 可见性、远程 URL 和默认分支由维护者明确决定。
