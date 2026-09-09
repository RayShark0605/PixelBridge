# 非本机吞吐：夜间自主推进记录（2026-09-09）

> **最新停止状态：用户已要求收尾，本次不再新增实验。** 以[暂停交接](REMOTE_THROUGHPUT_RESUME_HANDOFF_2026-09-09.md)为下次入口；本文件各节保留历史事实，旧“下一步继续/目标active”不构成停止后继续执行的授权。最后完成第22节；第23节只读准备，未实现、构建或运行。

## 0. 目标与最新授权

用户睡前明确授权自主修改项目代码、按需使用本机两块屏幕测试，期间不提问、不依赖人工配合。路线图作为参考，最终目标仍是提高非本机条件下的整文件有效传输速度。保护报告、历史交付与实验原件不动，不自动提交或推送，不引入旁路或ACK。

本记录不是新的远控现场通过证明。当前只有本地准备及离线检查；既有A1/B1/B2/A2结果和A2退出/预算失败保持原分类。总目标active，不把诊断子项完成当作提速完成。

## 1. B-Observation1已完成本地准备

产品只增加上一轮已经验证的报告输出；与冻结B相比，非文档差异恰为run_report.cpp、新定向测试及其CMake登记。发送节奏、capture/Receiver/退出实现、wire/Profile/Golden均未修改，不代表将B晋级默认。

- 新双端产品、源审计工具及受影响测试目标构建通过：259.187s，峰值Job commit 7,069,716,480B；上限300s/8GiB、并发2、日志1MiB。编译预算不是产品资源限制的改变。
- 报告/计时/队列11 cases、262489 assertions；证据封存4 cases、157 assertions通过。大量断言来自已有有界测量队列检查，不是压力测试或新增大规模矩阵。
- 新解压目录、隔离PATH、双端版本/构建身份/Profile/offscreen smoke共10个入口检查通过。
- 准确统计：既有Decoder启动fixture使用2帧合成BGRA和CPU oracle，再经假capture/demod服务测试原runtime/storage。不得记为实屏，也不得宣称没有任何合成像素观察。前置预算表述修正在context/STARTUP_SCOPE_CLARIFICATION.md，原preflight保留。
- 27份实际CL read log核对1161项输入，其中248项仓库源文件与冻结快照一致；实际编译命令包含新指纹，无全局vcpkg installed头文件。依赖未安装或升级。
- source fingerprint：`008b4543da6fed6d7a71d03c878973430767c58b1e198065adff31a616f8d9a0`。
- package manifest：`c430fdc0473cf2d8db43823b5dc50114b543a506b94053cc9fd44b6c0eb4367b`。
- ZIP SHA-256：`736644c7f192ab5bfa785e60c8ac3d423a0003bfad4f82ed07dcd8beff0801ef`。

证据根：`<repo>\artifacts\remote-throughput-observation-20260909-run01`。以CANDIDATE_STATUS.json、context/BUILD_IDENTITY.json、package/package-result.json、startup/summary.json为准。该包没有发生新的远端运行。

## 2. 新证据：历史codec模型与现场显示比例不一致

原A2封存final.json中，accepted Bootstrap locator的最后scaleX/Y约为1.333330945/1.333332083，物理接收区域为2560×1440；历史Step3-B固定codec输出则直接以1920×1080的1:1画布进入离线工具。后者卡在原几何门，不等于实际4/3显示比例下也必然卡在同一位置。

因此只检验一个变量：对同一封存raw/codec解码像素，在Decoder之前模拟4/3显示缩放。没有重新编码、修改Session、引入payload真值或放宽几何门。固定选择half-pixel-center bilinear、BGRA整通道、边界钳位、整数权重及一次round-half-up；**实际向日葵缩放核未知，此模型只匹配观察到的显示比例，不声称复现了向日葵。**

工具独立放在`<repo>\artifacts\remote-display-scale-20260909-run01\source`，不注册产品构建。只链接本轮已核验的原PBModulation等库与原RecordingMedia；没有GPU、Receiver或文件发布调用。

## 3. 有界差分结果

固定ordinal0/1/15，raw和codec各读16帧前缀，未到EOF；每个选定帧比较1:1和4/3，共12次CPU oracle观察。

| 输入 | ordinal0接受块 | ordinal1接受块 | ordinal15接受块 |
| --- | ---: | ---: | ---: |
| 原始1:1 | 15 | 15 | 15 |
| 原始4/3 | 15 | 15 | 15 |
| 固定codec 1:1 | 0 | 0 | 0 |
| 同一codec解码后4/3 | 15 | 5 | 0 |

4/3的三张codec样本均到达frameAvailable，但不保证数据槽通过；ordinal15仍接受0块。所有已接受块包含Control和Transport，不能将20个总块一律视作20个新文件方程。

后审计确认：

- 原媒体解码输入像素hash与封存Step3-B相同。
- 原始缩放前后45块均与历史原始逐块记录一致。
- codec实际20个接受块逐slot/kind/length/BLAKE3与封存原始对应块一致；没有将真值回馈CPU oracle。
- 独立NumPy按32行有界分块重建3张原始4/3图，完整输出像素BLAKE3与C++整数实现一致。
- 3项相关guard（输出已存在、raw截断、模式非法）按预期exit1，原输出未变化，无额外oracle观察。
- 配置2.813s、构建6.172s；raw/codec观察0.640/0.860s，峰值约49.3MB；独立分析1.5s、峰值约818MB。各进程使用2GiB Job及时间/日志上限，未扩展参数矩阵。

结果：`DISPLAY_SCALE_CHANGES_CODEC_ADMISSION_WITH_UNCHANGED_GATES`。它证明这个显示条件差异会改变原CPU路径的codec准入，**不是几何门修复、GPU接收成功、整文件恢复或远控提速证明**。CPU与旧WARP的具体擦除枚举不可混作同一后端结果。

证据根：`<repo>\artifacts\remote-display-scale-20260909-run01`，关键文件为PLAN.json、raw-01.jsonl、codec-01.jsonl、RESULT.json、RUNTIME_IDENTITY.json、GUARDS.json及logs。

## 4. 下一步继续推进，不等待人工

1. 优先给工具专用离线回放增加明确且有界的2560×1440输入包络；保留原1:1失败，不改生产的几何/质量门，不将扩大工具输入尺寸等同于放松Receiver资源政策。
2. 对同一30帧raw和固定codec的4/3显示像素，走原GPU解调与Receiver，检验完整发布/reopen及独立双摘要。严格保留媒体PTS、EOF、已接受块真值后审计和失败记录。
3. 只有整文件筛选链可用后，才用它比较一个有证据支持的减少远控编码负担或增加独立信息利用率的候选。不能停留在让某帧过门，也不因CPU准入增加就宣称提速。
4. 需要本机实屏时按最新授权自行进行，测试前仍读取实际拓扑，优先保持有限时长和明确ROI。本机结果不升级为非本机证据，不要求睡眠中的用户协助重开远端Encoder。

本轮尚未发生新增实屏、远端传输、codec编码、整文件GPU回放或吞吐收益验证。文档记录在包冻结之后更新，不能把当前文档树冒充当时包内快照；产品源与该包一致。

## 5. 后续增量：原GPU/Receiver的4/3整文件回放

本节更新第4节之后的实际进度，不改写此前各阶段的预算和证据。工具输入尺寸扩展已经实施，生产入口没有改变：保留完整原1920×1080回放函数，另加一个工具专用2560×1440副本。副本与原函数仅相差函数名和五处固定尺寸；原几何、pilot、FEC、冲突、资源、摘要、发布和reopen判据不动。原ReceiverPipeline仍在相同runtime翻译单元中执行，MAP明确证明两回放函数和ReceiverPipeline::Process均来自runtime_fixed_display.obj。

使用原封存30帧，结果如下：

| 对照 | GPU观察 | 正确接受块观察数 | 原Receiver不同Outer块数 | 整文件 |
| --- | ---: | ---: | ---: | --- |
| 原始1:1 | 30 | 450 | 50 | 摘要/安全发布/reopen/外部双摘要通过，首次发布在第5次观察 |
| 原始4/3的3帧前缀 | 3 | 45 | 32 | 未恢复、未发布；正确保留prefix而非EOF |
| 原始4/3 | 30 | 450 | 50 | 与原始1:1相同文件，首次发布在第5次观察 |
| 固定codec解码后4/3 | 30 | 20 | 12 | NOT_RECOVERED；30帧Bootstrap均可接受，后续数据槽大量失败 |

codec的20块为8个Control观察加12个Transport观察，不是20个新方程。BaseLuma有252个LaneErasure、4个InnerFecFailure；FineLuma有29个LaneErasure；Chroma有145个InnerFecFailure。由此将这个模型的主要失败阶段定位到数据lane/纠错，**没有证明真实远控也有相同失败率**。

全部945个raw块观察和20个codec块观察逐slot/kind/length/BLAKE3匹配封存raw真值；原媒体像素hash、实际PTS/时基/duration及EOF全部核验。已在上一增量独立重建的CPU样本与对应GPU缩放像素hash及接受块集合一致。原始文件发布后工具继续解调至EOF，后续Receiver为AlreadyPublished；不能把450个解调块观察当成450个新接收块。

新增三个相关guard：已有输出拒绝且原结果不变、截断raw零GPU观察拒绝、非法模式拒绝。审计脚本前三次因null判据、MSVC源文件行格式、MAP子串误匹配而停止，均保留脚本与失败说明；修正的是审计解析，不是native结果或接收判据。成功审计未重跑93帧。

证据：`<repo>\artifacts\remote-scaled-gpu-replay-20260909-run01`；独立构建：`<repo>\build-remote-scaled-gpu-replay-20260909-run01`。关键文件为PLAN.json、SCALED_REPLAY_DIFF.patch、RUNTIME_IDENTITY.json、RESULT.json、GUARDS.json。3个编译源的CL实际输入共448项，无全局vcpkg installed头文件；新工具/WARP/原库链接检查通过。构建14.359s、峰值约904MB；codec回放3.328s、峰值约172MB。处理时间不是远控吞吐。

## 6. 单候选H2：相同视频预算下的首次完整codec恢复

依据上述失效位置，仅改变连续画面变化频率：30个视频帧中，视频ordinal n使用原封存逻辑画面floor(n/2)，每张完整画面连续保持两帧。第一批15张逻辑画面的像素、Session/帧序号、wire和方程保持原样，不生成新Session或重新扰码。原方案在两秒内有30张不同逻辑画面，H2只有15张；这项机会成本明确计入，重复像素不算新信息。

其他条件全部固定：15 fps视频、两秒、原libx264 High4.0、8 Mbit/s target/min/max、4 Mbit VBV、原颜色变换、GOP15/IDR0与15、原PTS、同一4/3显示变换、GPU与Receiver。没有新增ACK、旁路或产品节奏开关。

| 条件 | 整文件结果 | 首次发布观察/媒体PTS |
| --- | --- | --- |
| 原固定codec N1 | 未恢复 | null |
| H2原始像素 | 完整通过 | 第9次 / 8/15 s |
| H2固定codec，第一次 | 完整通过 | 第13次 / 800 ms |
| H2固定codec，干净重编码与新目录复现 | 完整通过且逐帧决策一致 | 第13次 / 800 ms |

两份H2码流逐字节一致：2,188,333B，SHA-256 `4a96bb57b91aecf955043372f859d1cc6c49931567fd7d4c2cd290e2ac84fc14`。两次解码的原像素、缩放像素、所有逐帧Receiver决策/接受块均相同（只排除处理时钟）。实际码流属性、30帧、PTS/DTS、颜色、IDR/P结构和packet-byte VBV envelope通过原inspector；实际video packet均值8,749,904bit/s，并非声称短流严格等于8 Mbit/s或完整HRD认证。

每轮H2 codec共有132个正确块观察，其中108个Transport观察仅对应69个不同的已接受Transport payload hash，其余24个是Control观察。原Receiver在完成之前真实接纳50个不同Outer块；第12次观察还在同一FrameSequence的更清晰副本中补入新块，原去重逻辑没有将重复画面等同于必然无用。

三个H2已发布文件均为65,536B，外部重读SHA-256 `40f1859cccb1f6949e8ff0a0c157c97571759a020d7bc3c801bcdc457394276c`、BLAKE3 `d38ef0993b8f097aee9ad93267387bdef83c49e1f0c54e35be8c87d23974aebf`，与封存源一致。

**不能晋级的关键限制：** H2的前15个视频帧接受57个不同Transport payload hash，后15帧只有12个；该小文件在第一个GOP内完成。因此这只是固定开发模型中的启动阶段改善，尚未证明持续有效吞吐或迟加入，更不能据此将真实远控15Hz改成7.5Hz。真实远控最近几轮已可完成约40.5MB文件，受评估的Base/Fine FEC失败很少；本8 Mbit离线模型并不是已校准的向日葵替代品。

H2首个准备目录在生成像素前因通用包hash默认上限小于固定raw文件而停止；run01完整保留，run02显式使用事先固定的248,832,000B输入上限。没有改变产品资源政策。run02初始预算为60GPU观察/1次codec，正结果出现后先落盘REPRODUCTION_PLAN.json，再执行同条件额外30观察/1次重编码；最终H2合计90GPU观察/2次codec。不是三个hold因子的参数矩阵。

证据：`<repo>\artifacts\remote-temporal-hold2-20260909-run02`；重点是PLAN.json、INPUT.json、RAW_PROOF.json、RESULT.json、REPRODUCTION_PLAN.json、REPRODUCIBILITY.json及两份codec inspection。run_hold.py与reproduce.py提供原始命令与后审计。所有native子进程2GiB Job/进程上限，单编码60s、回放90s，实际最高约201MB；本轮未用屏幕、未启动远端、未改变产品默认、未自动提交。

## 7. 下一步判断

当前完整筛选链已出现可复现的正结果，但总目标仍active，非本机收益未确立。下一步仍只跟进H2这一候选的关键风险：让同一codec先经历固定的有状态前缀，再由全新Receiver从后续像素开始恢复，用一个短且有界的后启动对照排除初始VBV和前段清晰画面的影响。不能直接重放已经证明不足的原第二GOP、延长到碰巧成功、换hold因子或放宽门限。

若后启动结果不能支持持续收益，应淘汰H2默认晋级，保留它作为codec/Receiver正对照；下一候选需根据新的数据lane证据选择减少单位新信息的像素编码成本，而不是再做无依据的Hz矩阵。B-Observation1仍是下一次真实远端对照可用的诊断包，不要求睡眠中的用户协助，不把本机/离线结果升级为现场认证。

## 8. 后启动检查：H2没有通过，不晋级默认

按第7节计划，只把同一H2序列明确扩展为60个视频帧/4秒：原封存30张逻辑画面各保持两帧。前30个视频帧只经过同一RecordingMedia对象，Receiver/GPU尚未创建；随后才构造全新Receiver/GPU，接收视频ordinal30–59（原逻辑FrameSequence15–29）。保留原PTS2000–3933ms，不重开codec、不预先注入Session或descriptor，不把前两秒像素给Receiver。

工具只增加媒体前缀消费与60帧输入校验，两个回放函数、Receiver所在runtime翻译单元和所有GPU库与上一工具相同；CL实际输入及MAP函数归属再次通过。原始输入497,664,000B是流式磁盘fixture，不是单次全序列内存分配。编译14.172s、峰值904,261,632B；生产代码未变。

| 后加入条件 | 原始像素 | 固定codec彩色H2 |
| --- | --- | --- |
| 新Receiver首次状态 | 未绑定Session、未发布 | 未绑定Session、未发布 |
| 接收观察数 | 30 | 30 |
| Bootstrap接受 | 30 | 30 |
| 原Receiver不同Outer块 | 50 | 25 |
| 整文件 | 全部校验通过，第7次观察发布 | NOT_RECOVERED，未发布 |

完整新码流的前30个packet payload hash、前30张实际解码像素均与原两秒H2严格一致，排除了“新编码改变前缀内容”的解释。失败后缀首帧已获取Session，随后仍只有25个不同Outer块，未达恢复所需；没有偷偷延长接收。BaseLuma有216个LaneErasure、17个InnerFecFailure；Chroma的150个槽全部InnerFecFailure。

相关guard覆盖已有输出、错误30帧raw尺寸、合法30帧codec在新60帧边界提前EOF；三者均拒绝，0次额外GPU观察。最后一项只做30帧媒体解码，不冒充零媒体工作。实际码流60帧/4秒、四个IDR、颜色、PTS和同一packet-byte VBV envelope检查通过。

结论为`H2_COLD_SUCCESS_DID_NOT_SURVIVE_FIXED_POST_START_GATE`。拒绝H2默认晋级，不把这个结果扩大为“所有场景H2必失败”。证据根：`<repo>\artifacts\remote-warm-h2-20260909-run01`，关键文件PLAN.json、TOOL_DIFF.patch、RAW_PROOF.json、RESULT.json、AUDIT.json、RUNTIME_IDENTITY.json、GUARDS.json。该增量1次编码、60次GPU观察，无实屏/远端。

## 9. 同一后启动条件的色度成本消融：亮度通路完整恢复

当前总体设计第0节明确Base/Fine/Chroma三lane独立擦除；既有`Unified lane pilot failures erase only their own lane`测试有NeutralizeChroma对照。基于此，只检验一个新变量：**在视频编码前，对整个固定H2序列中和色度**，不是在Decoder之后修数据。

转换使用已有测试相同的BT709 luma规则：`round-half-up((722*B+7152*G+2126*R)/10000)`，B/G/R都设为此灰度，Alpha不变。全部124,416,000个像素同时与原double系数/正数lround规则独立比对，原本灰度的像素（含定位/亮度pilot等）不变；亮度仅有8bit取整精度，不声称实数luma毫无变化。整序列固定转换，不按解码结果或provider自适应，不改FrameSequence/slot/FEC/wire，也不将色度槽的位重新解释为亮度位。

这是**离线信道/编码成本消融**，不是新的Certified Profile，也不是把产品Sender静默改成少色模式。正式Profile、GUI、Sender、Receiver默认行为均未修改。若未来产品化，需要单独设计明确的实验身份和信息分配，不能直接以这份失真fixture取代既有颜色合同。

先验证同一后加入raw：每帧原来的10个亮度槽均正确，5个Chroma槽均按原规则LaneErasure，原文件经原Receiver完整恢复；再编码和接收。

| 同一固定后加入条件 | 彩色H2 | 色度中和H2 |
| --- | ---: | ---: |
| 正确接受块观察 | 39 | 184 |
| 不同Transport payload hash | 25 | 91 |
| 原Receiver完成前接纳的不同Outer块 | 25 | 50 |
| 整文件发布/reopen/独立双摘要 | 未恢复 | 通过 |
| 首次发布时间 | null | 第14次观察，原媒体PTS2867ms |
| Receiver加入点 | PTS2000ms | PTS2000ms |

相对加入点的867ms仅是开发模型PTS差，不是跨机时间，也不是实时goodput。色度中和后BaseLuma正确槽从37增至171，LaneErasure从216降至72；其5个Chroma槽在全部30次观察中仍严格擦除，没有产生错误payload。文件恢复完全由原亮度通路完成。

正结果后先冻结REPRODUCTION_PLAN.json，再做一次同参数重编码及全新输出目录复现。两份码流4,219,979B，SHA-256 `15b4a98c234443639769d2d1ddc92e659f830cf4c1b50139c4d3f14405f62315`，逐字节相同；包含预运行前缀的全部解码像素、后缀缩放像素、逐帧Receiver决策和接受块集合相同（排除处理时钟）。两次恢复文件65,536B，外部SHA-256/BLAKE3均为第6节原源摘要，安全发布和reopen通过。

**码率解释不能夸大：** 两者使用相同8 Mbit/s target/min/max、4 Mbit VBV、GOP与颜色参数，且通过同一packet envelope；但并非严格等字节。彩色/中和的整流video packet分别4,126,048B/4,218,815B，接收后缀分别1,938,572B/1,996,305B。中和版实际video packet均值8,437,630bit/s。不能据此声称“码流文件变小”或已认证完整HRD/真实网络公平性。

证据根：`<repo>\artifacts\remote-chroma-budget-20260909-run01`。关键文件PLAN.json、INPUT.json、RAW_PROOF.json、RESULT.json、REPRODUCIBILITY.json、COMPARISON.json和INSPECTOR_GUARDS.json。四项纯inspector负例（帧数、PTS、IDR、像素格式）均拒绝，没有额外codec/GPU调用。该消融最终2次编码、90次GPU观察；与第8节合计3次编码、150次GPU观察。未跑全回归、参数矩阵、压力、实屏或远端。

资源记录区分native与准备脚本：全部native构建/codec/回放/guard子进程有2GiB Job/进程上限；NumPy准备按单帧流式、确定的数组尺寸运行，但没有通过该Job runner，不声明其测得峰值或OS强制2GiB上限。完整说明保存在RESOURCE_SCOPE_NOTE.json。准备阶段未同时保留整序列内存，输出大小严格固定。

## 10. 下一步：先固定候选的独立验证，再谈接入

当前证据已从“停留两帧的启动小文件成功”推进到“减少色度成本后，经过编码前缀的新Receiver仍可完整恢复”，但非本机目标仍未完成。仍只有一个开发源/Session、一个codec条件和一个加入位置；实际向日葵带宽/缩放核未校准，不能把上述倍率当作真实提速幅度。

接下来只用一个事先冻结、独立的源文件和Session，对**完全相同的**彩色/色度中和条件做验证，保留同一预运行/后加入边界。不根据结果换seed、再调码率/颜色/保持时长；失败即保留并重新判断收益来源。验证通过才考虑具有明确实验身份、资源边界和相关安全负例的产品候选设计，不先改默认、不新增暗中反馈。历史包、源码、保护报告均不动，无自动提交。

## 11. 独立源和新 Session 的冻结验证通过

第10节计划已执行：只生成一次独立65,536 B源，生成后先冻结摘要，再用原生产EncoderRuntime生成一次新的Session及30张规范帧。没有更换失败源/Session，没有改codec、色度公式、H2、加入点或4/3缩放。

- 源SHA-256：`bace08edb13ddfeda5dbfd533aec93cee4b61405674783b682c08b5da9c70910`；BLAKE3：`0f84625921b7b99d2236599b404840ab169f1e5ad7d05f1879a41de38498344c`。
- 新SessionId：`63270a40950e3a3d12f6f7ab30f084aa`，SessionTag：`4236202404198007166`。
- 原始规范参考和彩色/中和H2后缀的原始像素整文件门均通过。中和原始后缀固定10个亮度slots正确、5个色度slots擦除。
- codec彩色后缀只有7个Outer unique，未恢复；中和后缀取得50个Outer unique，203个正确接受块观察/99个不同Transport哈希，整文件摘要、安全发布和reopen通过。所有接受块事后与规范raw真值核对，未向Receiver提供源文件或预期payload。
- 中和首次发布后缀observation15，原媒体PTS2933ms，相对PTS2000ms加入点为933ms；仍只是模型时间，不是实时goodput。全部150个Chroma槽严格擦除。
- 两者实际video packet为4,172,961/4,190,546 B，中和流4,191,708 B、SHA-256 `89491ed51212f618b5db3aa13269e284f326e3c8b89aad4979db9ea2f77a87c1`。相同8 Mbit/s约束不等于严格等字节，不声称网络公平性或完整HRD认证。
- 本增量只有1个独立源/1个Session/2次codec/150次GPU观察。准备变换也通过原Job runner：15个子进程全部受2GiB限制，峰值为NumPy单帧推导906,289,152 B；无新实屏、远程、压力或完整回归。事后独立重新打开4个发布文件，均匹配源双摘要。

证据根：`<repo>\artifacts\remote-chroma-independent-20260909-run01`。`AUDIT.json`通过，`verify_saved.py`只读复验历史证据。原产品源、Profile/wire/Receiver/资源门及保护报告未修改，Git index为空。

**下一步从离线筛选转为有明确实验身份的发送接入。** 不能将GUI整数7或8Hz冒充离线15fps H2的7.5个新画面/秒，也不能将色度中和栅格标为已认证新Profile。优先隔离在工具专用presentation边界，保留规范Encoder和原Decoder；实验时序应明确记录本地提交/Present返回/屏幕可见性之间的区别，不虚构pending、present或对端确认。新远控整文件吞吐收益仍未建立，不晋级默认。

## 12. 工具专用实验发送入口已完成本机启动/退出接入

新增`<repo>\tools\PBExperimentalVisualSender`，复用冻结的原EncoderRuntime、DataWindow及原测量库，不改产品GUI、Profile/wire、Receiver或资源政策。两种工具模式分别保留原彩色及明确的后置色度中和处理；共享至少133,333,334ns的本地成功Present观察后保持。它是一个独立实验入口，不是新认证Profile，也不冒充离线CFR/H2。

持有一张真实队列，额外像素内存最多16MiB、8192条身份/时间记录。源限64MiB，初步实屏入口仅5..60秒，强制2GiB以内的外层Job/进程内存和kill-on-close，输出根create-only、最长96字符。原15Hz逻辑时钟仍运行，实测提交速率、native转交和Present分别记录；丢弃尾队列不伪装为转交或收到ACK。周期控制按原runtime时钟，不能把此入口时序宣称与离线重复帧完全等价。

### 12.1 已暴露并保留的接入问题

1. 初次CMake文件把字符串序列化为JSON；保留错误和原构建目录，改正新工具生成内容。
2. Win32显示器ASCII名称的显式窄化缺失，被`/WX`拒绝；经范围检查后显式转换，不关警告。
3. 第一次运行缺少原fullscreen要求的`experimentMonitorIdentity`，被原ValidateEncoderConfig在开窗前拒绝；按原GUI的绑定规则补齐，没有放宽校验。
4. 两次实际右屏启动在首张成功Present后严格epoch失败。补充原生快照确认是`statistics-disjoint`：native仍Running、同一2560×1440/180Hz右屏、mode/DPI serial均0、原活动源已被原PollStatistics失效。工具改为**只对已完全排空且物理环境不变的disjoint/recovered，最多两次丢弃自身旧epoch队列后接收原runtime下一帧**；没有重标/重放旧像素，没有把未观察到的Present补成功。其他epoch/资源/设备变化仍拒绝，并增加精确负例。
5. 首次正常双模式只有约4.14/4.26Hz，保存记录显示保持到期后仍额外等待16..382ms。原因是工具多等了`inFlightFrame=false`；原DataWindow已有独立pendingPixels，可在activePixels仍由GPU使用时同步复制。恢复原runtime的`!pendingFrame`转交条件，保留epoch恢复的完全排空要求；不是削弱GPU所有权。

### 12.2 最终验证和边界

最终源码及构建为`SOURCE_FROZEN08.json`、`<repo>\build-remote-experimental-held-sender-20260909-run08`。MSVC双目标构建19.797s、Job峰值881,192,960B；4组确定性检查通过。C++与独立冻结的30张整图逐字节一致，共62,208,000像素；没有新增codec/GPU解调/Receiver运行。

| 右屏5秒发送检查 | 彩色 | 色度中和 |
| --- | ---: | ---: |
| 原runtime提交帧 | 31 | 31 |
| native转交帧 | 30 | 30 |
| 观察到对应成功Present | 29 | 29 |
| 原测量提交速率 | 6.389731446Hz | 6.375333884Hz |
| 最小同epoch保持 | 133,460,000ns | 133,455,500ns |
| native成功Present调用（含重复） | 285 | 286 |
| 退出时丢弃真实尾队列 | 1 | 1 |
| 外层进程结束/耗时 | exit0 / 5.125s | exit0 / 5.125s |
| Job峰值 | 183,975,936B | 183,660,544B |

各有一次原生统计epoch重建，首张已转交但未观察的记录如实保留；全部同epoch保持检查、逐条SessionTag/FrameSequence关联、源前后ledger和拓扑前后检查通过。全流程没有操作鼠标/键盘或远控/网络设置。此次实际屏幕操作仅右侧DISPLAY2；新增实屏尝试为2次早期失败和4次5秒发送检查，另有1次开窗前配置拒绝。未将这些发送检查当作整文件接收或远控提速。

证据根：`<repo>\artifacts\remote-experimental-held-sender-20260909-run01`；最终独立运行根为`<repo>\artifacts\hl0909-color05`和`<repo>\artifacts\hl0909-neutral05`。`AUDIT.json`核对两个目标各342条实际CL输入、MAP中的原EncoderRuntime/DataWindow与新变换函数归属，以及外部运行文件。全部历史失败、旧构建和源/实验记录保留。原measurement.build指纹只覆盖复用的应用库，新工具必须另查其源码/二进制manifest。

**阶段结论：** 独立源的离线整文件候选成立，新实验Sender的本机生命周期已接通；但新入口的实屏Decoder整文件、真实远控吞吐、40MB/多Segment与长时统计epoch稳定性仍未验证。下一步用原Decoder完成一次有界实际像素整文件接入验证，再推进非本机对照，不继续盲扫codec参数，不改默认，不因本机提交速率改变而宣称远控增益。总目标保持active。

## 13. 新入口通过原 Decoder 的右屏实际像素整文件接入

本轮只实现第12节的窄接入步骤。新增独立 `tools/PBOriginalScreenReceiver` 无GUI launcher：默认构造原 `DecoderRuntime`、调用 `MakeUnifiedDecoderConfig`、固定产品 Auto capture，不注入 services、Replay、source、摘要、文件名或Session。只传新输出根、显式设备和5..60秒预算。没有新增捕获/解调/Receiver算法，也没有修改产品GUI/default、wire、Profile或资源阈值。

原静态库来自 `build-remote-throughput-observation-20260909-run01`，摘要与已封存基线一致。最终工具构建为 `build-remote-original-screen-receiver-20260909-run02`：6秒内构建完成，Job峰值833,908,736B，341条CL实际依赖未包含全局vcpkg；MAP精确确认 `DecoderRuntime::Start/Run` 和 `ReceiverPipeline::Process` 来自原 `PBApplication:local_desktop_runtime.obj`。发送端直接复用第12节run08二进制及DLL，未重编或修改。

### 13.1 启动前失败和修复保留

- 首版无GUI宿主未链接PMv2 manifest。即使进程DPI API成功，原 `ResolveScreenCaptureRegion` 仍按合同拒绝 `dpi-awareness-required`；尚未启动Sender窗口或捕获。只在新工具CMake中加入与产品Decoder相同的既有 `cmake/PixelBridge.PerMonitorV2.manifest`，不放宽原检查；新建run02构建，保留run01和全部负例/日志。
- 首次构建后的Python审计错误地假设 `step1.hash_file` 返回path；修复的是独立路径身份审计，native构建本身成功，无额外native运行。`finish_identity.py`核对原有有序哈希和冻结源副本。
- 首次direct-Job负例错误地假设宿主没有Job。实际宿主已有但不满足2GiB/kill-on-close合同，工具正确拒绝。初次stdout/stderr未保存，不能冒充已验证无Job分支；额外一次只读负例保留了实际不足Job拒绝。最终版本重核六种duration、已有根、缺失设备、继承不足Job共9项拒绝以及原只读DPI/ROI准入。没有借负例开始屏幕接收。

### 13.2 冻结的一组实屏条件

继续使用第11节冻结的65,536B独立源，不换seed。彩色和中和各生成新Session、新状态根和新接收输出；两者都使用相同保持包装，**彩色不是未包装的产品15Hz基线**。发送端固定运行20秒，接收器在脚本固定等待2秒后独立启动，最长接收15秒；即使接收提前完成，也不读其结果来终止或调节Sender。外层两个独立Job各2GiB、combined理论上限4GiB，cleanup分别30/25秒。实际都正常结束，不需要人工操作。

运行前后只读拓扑一致：DISPLAY1主屏`[0,0,2560,1440]`、DISPLAY2右屏`[2560,0,5120,1440]`、96DPI、180Hz。只在右屏开数据窗口并由原WGC捕获；没有键盘、鼠标、焦点或远控/网络配置操作。原定位器从实际像素观测到右屏ROI内原点`(320,180)`、scaleX/Y均1、marker residual为0，即规范1920×1080在2560×1440中居中显示；不是现场4/3显示模型。

| 原runtime与实际像素结果 | 彩色同保持对照 | 色度中和候选 |
| --- | ---: | ---: |
| SessionId | `83980788ce295699e697eadd24b5ae37` | `b4740b4759d23af03621a408638bf24c` |
| 原整文件主时长（startAccepted→finalReopenVerified） | 1.2138502s | 1.5310232s |
| 本机VerifiedRaw/EncodedGoodput | 53,990.1876 B/s | 42,805.3605 B/s |
| 接收观察/独立逻辑帧 | 12 / 4 | 30 / 8 |
| 原Receiver不同Outer块 | 50 | 50 |
| Base/FineLuma FEC接受 | 108 / 12 | 270 / 30 |
| Chroma接受/擦除 | 60 / 0 | 0 / 150 |
| 首次Bootstrap精确Sender身份 | Frame15，Carousel2，cycle1 | Frame14，Carousel2，cycle0 |
| 原WholeDigest/rename/reopen | 全通过 | 全通过 |
| 停机后外部SHA-256/BLAKE3 | 与原源一致 | 与原源一致 |
| Receiver Job峰值 | 374,169,600B | 373,305,344B |
| Sender Job峰值 | 184,172,544B | 184,147,968B |
| Sender实际进程时长/exit | 20.125s / 0 | 20.125s / 0 |
| 实际收尾的统计epoch重建数 | 1 | 2 |

两轮实际源/输出SHA-256均为 `bace08edb13ddfeda5dbfd533aec93cee4b61405674783b682c08b5da9c70910`，BLAKE3均为 `0f84625921b7b99d2236599b404840ab169f1e5ad7d05f1879a41de38498344c`。原报告 `resumeLoaded=false`、覆盖完整、measurement失败None，无摘要豁免。首次Bootstrap精确匹配本轮Sender提交记录及实际转交/Present观察，不跨机相减时钟、不推测丢帧率。

中和轮先后遇到 `statistics-disjoint` 和 `statistics-recovered`，原生均已无active/pending/inflight，环境/viewport/generation未改变；仍符合已冻结的最多两次drained statistics重建，不放宽条件。同epoch最短保持分别133,370,000ns/133,337,300ns。彩色121提交/120转交/119观察，灰度122/121/119；尾队列各丢弃1条，失效帧记录保留，不虚构Present或跨epoch重放。

### 13.3 结论、范围与复验

**这一轮证明新实验发送入口确实能由原Decoder从真实屏幕恢复完整文件；它没有证明非本机提速。** 本机没有codec/带宽压力，中和版该次还更慢，不能将其推广成通用提速。两轮新Session、首次加入位置和启动耗时不同，单次小文件时间不是严谨的因果性能估计；必须连同第11节相同固定压缩条件下的成功/失败对照解释。此次新增codec为0、实屏Sender/Receiver为2对，无完整回归、矩阵或压力测试。

证据根：`<repo>\artifacts\remote-held-screen-recovery-20260909-run01`。主要文件：`PLAN.json`、`MANIFEST_FIX_PLAN.json`、`SOURCE_FROZEN02.json`、`RUNTIME_IDENTITY02.json`、`GUARDS02.json`、`AUDIT.json`及两个variant的process/launch记录。新四个运行根为 `artifacts/hs0909-color-s01`、`hs0909-color-r01`、`hs0909-neutral-s01`、`hs0909-neutral-r01`。全部路径均为独立新增；原历史产物/构建保留，保护报告摘要不变，Git index空，无自动提交。

仅重新检查保存的证据/外部输出，不启动屏幕或codec：

```powershell
<python> -B -X utf8 <repo>\artifacts\remote-held-screen-recovery-20260909-run01\audit_saved.py <NEW_ABSOLUTE_AUDIT_JSON_PATH>
```

此脚本有意检查原非文档源码基线；今后若修改相关实现，应核对新来源而不是把哈希差异强行忽略。历史只读复验另见本根 `verify_delivery.py`，不宣称之后工作树等于历史工作树。

下一步继续非本机主线：保持当前候选身份，先确认冻结压缩条件的效果是否只限64KiB单段和特定加入点，依据原调度/资源限制确定一个有界扩大单段的验证，而非盲加Hz/码率/Profile矩阵或本机长压。真实远端必须有真实像素证据才可声称吞吐改善，不用本机对照填补；不要求睡眠中的用户配合，不把本次接入完成当作总目标完成。

## 14. 固定压缩候选扩大到 256 KiB，通过原 Receiver 整文件门

依第13节下一步，只固定一个更大的独立源，不再调整codec或变换参数。预注册 `PLAN.json` 后只生成一次 `os.urandom(262144)`，先冻结双摘要；原生产EncoderRuntime生成一次新Session与60张规范帧。K从50升为200（`ceil(262144/1314)`），仍为一个Raw Segment。只把短序列规模改为120视频帧/8秒，每张逻辑画面原样保持两帧；预运行仍是前30视频帧/2秒，Receiver只接收后90视频帧，即45张不同逻辑画面。最多450个亮度槽只是测试预算上界，未被当作恢复保证。

固定配置仍为libx264 High4.0、8bit yuv420p、limited BT709/left、8M target/min/max、4M VBV/init0.9、15fps、GOP15/noB/ref1、veryfast/zerolatency/singlethread/asm0。颜色中和公式、4/3 scaler及原Receiver不变，不因结果修改源、保持因子、码率或加入点，不延长接收窗口。实际两条编码命令仅输入/输出路径不同。

### 14.1 实现来源和资源

新增独立证据根 `<repo>\artifacts\remote-chroma-256k-20260909-run01`，派生工具源保存在其 `source` 子目录，未修改已封存工具。原Receiver runtime TU、两个回放函数、4/3 scaler及声明与已验证warm工具逐字节一致；只改新入口的固定source/frame/EOF/报告规模，并增加媒体-only的短码流负例入口。

新构建 `<repo>\build-remote-chroma-256k-20260909-run01` 有两个目标：`PB256kFixture.exe`直接链接原 `PBApplication` EncoderRuntime；`PB256kReplay.exe`复用与前次完全相同的原Receiver runtime TU/回放实现和原GPU库。MAP确认了函数归属，CL实际依赖未混入全局vcpkg，库摘要与封存基线一致。构建22.797秒，峰值947,740,672B。NumPy派生逐帧运行，60张完整图共124,416,000像素与原浮点/正数取整规则一致，峰值905,609,216B、6.406秒；没有整序列驻留内存。

规范raw为497,664,000B，两份H2派生各995,328,000B；总证据预留3GiB。所有native和NumPy准备均经2GiB process/Job、kill-on-close、create-only日志/输出的runner。正常GPU观察共360次，编码仅2次。四个source/root/raw尺寸/codec提前EOF负例通过；其中短codec只解码60媒体帧、不建Receiver/GPU。另有5个纯inspector负例和3个纯身份审计负例，不增加codec/GPU运行。无实屏、远端、完整回归、参数矩阵或压力运行。

### 14.2 真实结果，不用中间接纳量替代恢复

源SHA-256：`db7b4a708a31e631176094bead52fbce32d52b4283fbbaa96d12daaf4b0f2209`；BLAKE3：`8dff87818df79fd0d70acb71fe642adbab60893367ff072d037bffbab946ba39`。冻结Session为 `b074bfe9854ef288310898d63e9abd6c`。每个接收目录全新，Receiver启动前均未绑定、未确认、未发布，不传源/预期摘要或描述符给Receiver。

| 相同后加入条件 | 彩色原始像素 | 中和原始像素 | 彩色固定codec | 中和固定codec |
| --- | ---: | ---: | ---: | ---: |
| Receiver观察数 | 90 | 90 | 90 | 90 |
| Bootstrap正确 | 90 | 90 | 90 | 90 |
| 正确块观察（含控制与重复） | 1350 | 900 | 132 | 585 |
| 不同Transport payload hash | 596 | 375 | 98 | 329 |
| 原Receiver完成前不同Outer接纳 | 200 | 200 | 0 | 200 |
| 完整Session建立 | 是 | 是 | 否 | 是 |
| 整文件摘要/发布/reopen/外部双摘要 | 全通过 | 全通过 | 未恢复、未发布 | 全通过 |
| 首次发布时间 | video66，PTS66/15秒 | video86，PTS86/15秒 | null | video90，PTS6000ms |

中和codec的发布点相对Receiver加入点为4秒**模型媒体时长**，不是实时goodput。原summary的live/simulated goodput继续为null；不能把它与此前40MB现场秒数比较，也不由单段小样本宣称稳定吞吐。

关键新证据：彩色codec解出了125次正确数据观察和7次SegmentDescriptor，但没有成功进入SessionDescriptor接纳。所有数据被原 `WaitingForSession` 路径拒绝，控制返回 `ControlUnknownSession`，Outer对象/接纳为0。这不是Bootstrap定位失败，也不是单纯“FEC块数量不足”。中和codec在第9次观察、媒体PTS2533ms、逻辑Frame19建立Session；原Receiver最终接纳200个Outer块并完成文件。450个Chroma槽全部按原规则擦除，没有错误色度接纳。应把收益解释为控制信息和亮度数据同时更易通过固定压缩条件，而不能只归因于“数据块变多”。

初始Python后处理错误地要求失败的彩色Receiver也必须有Sender SessionId，导致codec阶段wrapper exit1；native已正常读完120媒体帧/90观察、未发布，不是native异常。保留原异常日志，仅在 `experiment_v2.py` 修正审计：空Session仅在所有快照未建立会话、Outer=0、无发布时可作为NOT_RECOVERED；任何已知但不匹配身份仍拒绝，并补充3项负例。没有重编码或重放彩色，随后只执行尚未运行的中和条件。未知身份不补成预期身份。

### 14.3 码流实测与下一步

两条码流均检查120帧、8个IDR（0/15/…/105）、112个P帧、逐帧PTS/DTS、颜色/格式、8M/4M HRD字段及packet-byte VBV envelope；不宣称完整HRD合规。彩色video packet总8,176,586B、中和8,133,394B，后90帧分别5,990,973B/5,908,885B。此处中和实际字节略少，但两条有限流不严格等字节，不把该差值当作真实远控带宽测量。

- 彩色码流8,178,374B，SHA-256 `12a2a7cc94fd439a0896cbe34ac8799adeaf45f2aea9727c05b16c191cbffbed`。
- 中和码流8,135,178B，SHA-256 `5ac2bcf48927ad1a76e1f2aa117673854c0478f214e996e26ac41477c5fed669`。

`AUDIT.json`重新核对原始/压缩四条完整观察链、逐slot有效payload与raw真值、原安全门、外部双摘要、模型时钟、源码/库及25个受控进程结果。所有源码/历史包/构建/录像均保留，非文档产品源码未修改，保护文件不变，Git index空，无提交/推送。

**阶段结论：** 固定压缩候选不只在64KiB/K=50成立，扩大到256KiB/K=200仍完成原恢复链。现有离线证据已足够进入实用对照准备，不继续无界放大样本或扫描codec参数。下一步冻结候选并核对其代表性文件运行时长、证据容量及退出预算，准备能比较真实非本机整文件时间的独立入口；在扩大原60秒工具运行上限前，必须有明确预算和受影响验证，不能把只跑过64KiB实屏的程序直接当40MB现场包交付。多Segment、代表性40MB、真实远控收益和较长生命周期仍未验证，总目标保持active。

## 15. 有界对照入口与两 Segment 整文件通过；容量筛选否决当前候选的现场提速晋级

本轮按第14节先核对预算并增加未包装对照，再完成唯一一组最小跨Segment实屏验证。**更重要的结论不是“可以继续给中和候选做包”，而是它的持续容量不足以优先改善已验证现场，停止该提速晋级方向。** 保留其弱信道恢复参考价值、全部模型成功和本轮完整恢复证据，不修改产品默认，也不因模型PASS宣布优化完成。

### 15.1 只改工具宿主，不改原 runtime/Receiver

- Sender新增 `--comparison-run original|neutral SOURCE NEW_SHORT_ROOT DEVICE SECONDS`，Receiver新增 `--comparison-run NEW_SHORT_ROOT DEVICE SECONDS`，显式5..900秒；旧 `--run` 仍5..60秒。共用严格无前导零ASCII十进制解析，超预算/非法输入拒绝。
- `original`以空presentation factory直接使用当前冻结原runtime的native路径，无画面中和或保持；**不是历史现场A版**。当前库仍含既有控制相位实现。`neutral`复用原冻结中和＋保持，两项合起来筛选，不伪装成仅改变色度的单变量比较。
- 新 `comparison_process_runner.py`独立派生自Step3B runner，仅私有函数名/timeout ceiling变化；公开入口只接受这两个comparison CLI，固定预算加30秒cleanup，至多930秒。先入Job再运行、2GiB/process+Job、kill-on-close、句柄归属、日志create-only和大小限额不变，不读Receiver来结束Sender。没有扩大旧共享runner上限。
- 原64MiB源、8个测量Segment、16MiB变换图像、8192个held记录、最多两次已排空统计epoch重建、4MiB单证据文件、原1800秒测量上限均不变。新增submitted硬限14000行，原serializer的6个uint64取最大值时每行含换行268B，总3,752,000B；held8192条最大宽度记录3,088,998B，另留三份固定native快照196,608B，均不扩4MiB。
- 新构建目录为 `build-remote-comparison-sender-20260909-run01` / `build-remote-comparison-receiver-20260909-run01`，分别19.735s/5.891s、Job峰值881,238,016B/834,199,552B。MAP确认原EncoderRuntime/DecoderRuntime/ReceiverPipeline均来自已封存原库，CL依赖无全局vcpkg混入。
- 6组无屏幕检查通过，包含新严格时长、实际serializer容量、6750帧虚拟时钟，以及原变换/队列/错误/统计epoch检查。8项native CLI拒绝和11项纯supervisor预算检查通过，只读PMv2/ROI通过。虚拟900秒量级容量和900秒入口被验证，**并未实际跑900秒实屏**。

### 15.2 固定两段实际屏幕结果

一次生成并先冻结 `8,454,144B = 8MiB + 64KiB` 独立源。原预扫描确认两个Raw Segment；不是根据`.bin`扩展名假设无压缩。SHA-256 `694f8a5ef0c983e40c890477e8bcf0091094d493d1c9445afcfa6d4d1f70c52c`，BLAKE3 `b59b7ae78973f1f2a3df9f2bccd7b47021331e0f7230241293f7a272a9e49e7a`。

每个variant固定发送240秒，固定2秒后启动原Receiver，接收预算230秒；提前接收完成也不反馈、不改变Sender时长。只使用重新核验的右屏DISPLAY2，物理`[2560,0,5120,1440]`、96DPI、180Hz；原WGC捕获同一ROI，1920×1080 raster在其中1:1居中、原点(320,180)。不是远控4/3模型。没有鼠标、键盘、焦点、远控或网络配置操作。

| 实际像素/原Receiver | 原生未包装 | 色度中和＋保持 |
| --- | ---: | ---: |
| Session | `0a15a8f5d480bc4d84a1373a22467f96` | `56eee400260f6d55ef1c541a1110fa64` |
| 整文件startAccepted→finalReopenVerified | 39.288218s | 176.8616631s |
| 本机VerifiedRaw/EncodedGoodput | 215,182.6789B/s | 47,800.8849B/s |
| 首控制/首有用方程 | 8.3324061s / 8.3535494s | 8.7344527s / 8.7570429s |
| 接收观察/独立逻辑帧 | 610 / 464 | 3312 / 1010 |
| 原UniqueVisualFPS | 14.9921166 | 6.0026688 |
| 原Outer不同块/已验证Segment | 6435 / 2 | 6435 / 2 |
| Chroma接受/擦除（含重复评估） | 3050 / 0 | 0 / 16560 |
| Sender提交/提交证据字节 | 3597 / 672700B | 1442 / 268986B |
| Sender实际进程时长/exit | 240.172s / 0 | 240.156s / 0 |
| Receiver实际进程时长/exit | 39.438s / 0 | 177.000s / 0 |
| Receiver Job峰值 | 406,958,080B | 407,973,888B |

两端全为新Session/状态/输出，无resume；原整文件摘要、安全rename、reopen及停机后独立SHA-256/BLAKE3全通过，源前后ledger与拓扑一致。首次Bootstrap精确对应Sender Frame30/Frame13，**均为Carousel0**；这是跨段接入检查，不冒充later-Carousel现场样本。中和运行转交1440、观察1439次成功Present，1次有界drained统计epoch，最小同epoch保持133,340,800ns；失效/未转交尾记录保留，不制造成功Present。

新增实屏只有预注册的两对，未延长、补跑、重编码或做压力矩阵。四个native进程正常退出，无残留。虽然本机中和明显更慢，也不将该单次时间比当远控因果性能估计。

### 15.3 回到实际非本机目标：停止低容量候选的提速晋级

只读重核历史**正常收尾**B2：原final.json摘要匹配原核心correlation，再重新打开其40,517,389B最终文件核对双摘要；编码40,410,277B、212.477132s，编码有效吞吐190,186.4762B/s（185.729KiB/s）。两端原wrapper均exit0、无预算告警。B2并不因此变成“稳定胜过A”，A/B因果限制照旧。

原B2进入FEC的Chroma槽11949/11955成功（约99.95%），Base与Fine全部成功。这是**条件于实际FEC评估**的结果，不包括未捕获/未准入帧，不宣称全信道FER接近零；但已有证据显示Chroma在现场是有用容量，并非可以无代价丢掉的lane。

按当前1314B外层块、9 Base＋1 Fine槽、至少133,333,334ns保持，held-neutral理想稳态生产上限为98,549.9995B/s（约96.24KiB/s），还未扣控制、纠错、重复、丢帧和本机Present开销；40,410,277编码字节对应理想生产时间约410.05s。即使假设保持后仍可用全15槽，理想稳态上限也只有147,824.9993B/s，低于已验证B2观察值。实际本轮约6Hz而非7.5Hz，进一步减少余量。

**这是容量排除依据，不是测得的远控goodput，也不是所有可能播放/缓冲模型下的Receiver时长下界。** 推导只针对稳定交付、没有大量预存视频加速排空的路径，忽略有限启动/两次epoch重建瞬态；不跨主机相减时钟。实际远控codec、带宽和编码策略仍未知，固定8M压力模型没有被校准成当前现场的等价物。不能因该模型彩色失败、灰度成功，就忽略真实现场已经正常利用色度并达到更高有效容量。

据此生成 `DECISION_UPDATE.json`：完成原定有界跨段检查，但不继续为held-neutral做当前现场提速包、40MB/900秒扩跑或codec矩阵。保留它作为弱信道恢复参考，以及新原生无GUI测量入口作为后续工具。下一步优先保留有效Chroma与足够发送容量，回到真实唯一画面到达率/时序及有效数据利用的损失分析，再确定一个有证据和容量余量的单候选；不盲选新的FPS，不因模型成功降低主目标。

### 15.4 交付与复验

证据根：`<repo>\artifacts\remote-bounded-comparison-20260909-run01`。运行根为 `artifacts/bc0909-original-s01`、`bc0909-original-r01`、`bc0909-neutral-s01`、`bc0909-neutral-r01`。`PLAN.json`保留初始预算，`CAPACITY_SCREENING.json`保存原现场final身份/重开摘要及容量推导，`AUDIT.json`保存两段完整恢复链，`RUNTIME_IDENTITY.json`保存工具与原库/实际CL/MAP身份。只读复验见本根`REPLAY.md`与`verify_delivery.py`，不会启动屏幕、codec或新接收。

本轮只改两个实验工具与相关文档，原应用/库实现保持不变；`docs/PHASE1_GATE_REPORT.md`摘要不变、index空、无提交/推送，历史目录/录像/构建均保留。没有新的实际非本机提速认证，目标保持active。

## 16. 全彩4/3原硬件捕获成本：两段整文件通过，缩放单独未复现现场到达率

### 16.1 范围与失败保留

复用第15节8,454,144B两Raw Segment源，冻结15Hz、原EncoderRuntime、原硬件WGC/Decoder/Receiver。只在新artifact的Sender适配器中裁出居中1920×1080画布，并用第14节逐字节相同的固定双线性算法放大4/3至2560×1440。另一条件传递原画面/stride不变。同步变换、无保持/中和/队列/ACK，不能称为实际远控的未知缩放器。

两条件初定Sender75秒、Receiver70秒、固定延迟2秒，单Job/进程2GiB，屏幕前后只读核验DISPLAY2物理[2560,0,5120,1440]、96DPI、180Hz。本轮GPU LUID 95279，仅记录当前身份。native组通过；首次scaled在Frame1出现Native EpochMismatch后，实验适配器错误将可重试状态固化，只转交Frame0。Receiver按70秒预算停止，goodput与主时长为null、无发布；Sender工具exit1。源不变，原始失败运行、`.part`、resume、日志均保留，未延时或改成PASS。

原runtime明确允许EpochMismatch/Paused/NotRunning后重新取得native快照重试。新`source02`只修正实验适配器：跟踪最后成功转交序号；原样返回这三类短暂拒绝；记录每次attempt和最多8个拒绝后快照，第9次拒绝升级ResourceLimit；不自行改epoch/重放/排队。用确定性Frame0成功、Frame1旧epoch拒绝、新epoch重试、成功重复拒绝及三类状态预算检查验证。固定像素变换不改；另建build/run根只重跑scaled一对，原对照不重跑。实际仅一次统计epoch1→2拒绝后恢复，原runtime重新提交成功。

### 16.2 结果（本机诊断，非实际远控速度）

| 原硬件捕获/Receiver | 1:1居中 | 4/3放大，修正适配器 |
| --- | ---: | ---: |
| startAccepted→finalReopenVerified | 39.2957546s | 40.1229359s |
| 本机VerifiedEncoded/RawGoodput | 215,141.4087B/s | 210,706.0167B/s |
| Sender成功提交逻辑FPS | 15.0138206 | 15.0028932 |
| 原Receiver UniqueVisualFPS | 14.9921286 | 14.6436513 |
| 观察/unique | 610/464 | 531/463 |
| Sender成功/attempt数 | 1122/1122 | 1121/1122 |
| 平均同步像素变换 | 0.000033ms | 29.283298ms |
| 接收进程时长/exit | 39.468s/0 | 40.282s/0 |
| 发送进程时长/exit | 75.171s/0 | 75.172s/0 |
| Receiver Job峰值 | 408,096,768B | 408,375,296B |

两组均原whole-file digest→安全rename→reopen成功，停机后再次独立读取SHA-256/BLAKE3匹配源。均2段、6435个Outer不同块；首Bootstrap精确关联Sender Frame30/Frame29，均Carousel0，不冒充later-Carousel现场。原定位实测native为origin(320,180)、scale1；scaled为origin约(0,0)、scale约1.333333、marker residual0.033521px，几何门未放宽。

Base/Fine/Chroma全部已评估槽通过，且FEC迭代均0；真实B2是非零迭代，因此本机结果不排除压缩后纠错成本。CaptureFlow native arrived2344/copied772/dropped1572，scaled2394/677/1717；这些包含刷新重复，不将原始callback丢弃比等同有效payload丢失。两组无readback/admission/epochreset事件；所有成功测试进程正常退出，无残留。

`bootstrapCpuTimeTotal100ns`除以Bootstrap尝试数为21.287ms/28.012ms，GPU累计摊销4.026ms/4.235ms，post-FEC9.653ms/10.562ms；分母是Bootstrap尝试，不是各阶段样本数，且阶段耗时不应相加当端到端时长。代码确认Bootstrap QPC包住Map、CPU定位/解码和Unmap，因此名称不能证明它是纯CPU成本。

**决策：** 此次无远控压缩的4/3未单独把原捕获降至现场约10–11个独立画面/秒。不能据此宣称瓶颈是网络或远控，亦不能排除真实输入噪声/FEC/突发到达。下一步优先拆清Bootstrap成本、寻找保持全色度及原正确性门的接收端低风险改进，不盲调发送FPS、不继续codec矩阵。非本机实际提速仍未确立。

### 16.3 证据与复验

证据根 `<repo>\artifacts\remote-scale-capture-cost-20260909-run01`，`AUDIT.json`核对源码/库/工具身份、所有6个运行根及两份完整输出；`REPLAY.md`、`verify_delivery.py`提供只读复验。`source`与`source02`分别保留初版/修正版，`adapter-retry-delta.patch`记录差异；build分别`build-remote-scale-capture-20260909-run01/02`。2组/3组定向单元检查通过，4项native输入拒绝通过。新增屏幕共3对（其中1对失败保留），新codec/远控运行0。产品及现有工具源码、原库不改，只新增artifact和3个文档增量；保护文件摘要不变、Git index空，无提交/推送。目标保持active。

## 17. 保留全部正确性门的 Bootstrap 扫描成本优化及实际捕获集成

### 17.1 先分清CPU成本，不把Map差值当定论

新增两个artifact-only短探针：`remote-bootstrap-cost-20260909-run01`、`remote-bootstrap-scan-cursor-20260909-run01`。取第14节已封存60帧全彩raw的Frame30（先校验整份source.bgra身份），一份1:1居中到2560×1440，另一份用同一冻结4/3变换；不编码、不捕获、不进入Receiver。每种输入两次warmup及24次交替对照，像素变换和序列化均在计时外。

第一探针只在独立重命名的源码副本上给LocateMarkers/EvaluateGeometry计时，原库作为对照。原中位Bootstrap17.193ms/21.853ms，扫描约16.699ms/21.299ms，几何/Bootstrap校验约0.499ms/0.505ms；扫描占副本总耗时97.06%/97.66%。52次含warmup的全部观察字段（包括两个副本、canonical44、几何/质量/残差/候选/工作量）相同，另4个预算/坏视图结果一致。此为温热普通CPU内存，不是mapped staging；与第16节耗时相减不能直接推出Map耗时。

### 17.2 唯一候选：顺序BGRA游标，不缩扫描范围

候选只替换全宽行扫描读取：先完整验证LumaView，在每行建立有界顺序指针，逐像素原样Charge并按同一浮点算式读BGRA，减少重复地址乘法和格式/整数亮度模式分支。没有预读、整图/行缓存、前帧几何缓存、候选提前退出或阈值减少；阈值仍128→64→192，扫描顺序、垂直cross/验证交错顺序及所有失败上限不变。其他像素格式走原ScanPixel。构造该reader的路径仍明确使用非整数亮度模式，不改变DesktopLevels算术。

| 同进程交替CPU对照，各24次 | 1:1居中 | 4/3 |
| --- | ---: | ---: |
| 原Bootstrap中位 | 16.98835ms | 21.63750ms |
| cursor含局部计时的中位 | 14.90935ms | 19.57030ms |
| 中位成本下降 | 12.24% | 9.55% |
| 原/cursor均值 | 17.06707/14.91484ms | 21.65853/19.64895ms |

52次完整观察/资源计数一致；13项差分guard包括17-token预算、准确完成预算/少1、坏footprint、Gray/R10/FP16回退、非有限FP16和两帧歧义。初次构建后的MAP审计脚本仍误匹配旧`timed_...obj`名字，**发生在native探针启动前**；保留原失败脚本/说明，只修正审计名字并复用已成功构建，native对照没有补跑。

CPU阶段下降不是远控goodput提升百分比。只据此接受这个低风险成本候选，不调发送速率、不少发色度、不改wire或资源门。

### 17.3 实现和受影响检查

源码只改`libs/PBModulation/src/local_desktop_decode.cpp`；新增`tests/PBModulation/test_local_desktop_scan_cursor.cpp`并挂到原Bootstrap测试目标。新增用例覆盖1923奇数宽度、13字节行padding、非对齐首地址、末行精确footprint、少1字节拒绝、准确工作预算/少1，以及第二张画面只在threshold64可见时仍必须AmbiguousGeometry。不是放宽测试或Golden。

`build-remote-bootstrap-cursor-integration-20260909-run01`只编译新locator object及受影响的原Bootstrap/零分配探针，其他依赖库冻结、不覆盖。MAP确认旧PBModulation的local_desktop_decode.obj未进入新测试二进制，新对象是实际执行者。

- 构建13.160s，Job峰值319,467,520B。
- 原Bootstrap受影响用例（过滤`~[matrix]`，隐藏耗时benchmark不运行）：**49 cases / 12,610,092 assertions PASS**，3.530s；包含原穷举RGB读取检查，不新增参数扫描。
- 原零分配探针：**1070 checks PASS，sutAllocationAttempts=0**，0.140s。
- 未运行完整CTest、几何矩阵、压力测试、ASan或新codec。

### 17.4 新对象接回原捕获/恢复链

随后独立新建`build-remote-cursor-screen-20260909-run01`，宿主main与当前`PBOriginalScreenReceiver`逐字节一致；只显式链接已验证的新locator object，原PBApplication DecoderRuntime/ReceiverPipeline、WGC/GPU/FEC/文件恢复库不改。MAP同时核对原runtime/Receiver归属和新locator归属，不能把它称为“完全未修改的Decoder”。PMv2 manifest保持。

仅一对实际右屏：固定15Hz全彩4/3、复用第16节修正版Sender和8,454,144B两Raw Segment源。Sender固定75秒、2秒后启动Receiver/70秒预算，完成不反向结束Sender。前后核对右屏[2560,0,5120,1440]、96DPI180Hz，无鼠标/键盘/焦点/设置操作。

- 原startAccepted→finalReopenVerified：**39.574339s**；本机VerifiedEncoded/RawGoodput **213,626.9162B/s**。
- Sender提交15.0015056逻辑FPS；原Receiver UniqueVisualFPS **14.8475612**。
- 原两段摘要、安全rename、reopen和停机后独立SHA-256/BLAKE3全通过；源前后ledger不变，Session/首Bootstrap与Sender精确关联，无resume、冲突或资源拒绝。
- Bootstrap累计耗时按尝试数摊销24.348486ms，GPU4.240568ms、post-FEC10.984773ms；不相加为端到端时长。
- Sender75.171s、Receiver39.875s，双方exit0；Job峰值227,700,736B/408,391,680B。最终无相关native残留。

该结果证明新扫描对象在真实WGC/GPU/Receiver全链路没有破坏本次整文件恢复；与第16节不同时间、不同Session的40.122936s不能当严格配对性能收益。没有新远控运行，未制作/替换现场包，未改变默认FPS/Profile或远控/网络配置。

### 17.5 交付和下一步

主证据根：`<repo>\artifacts\remote-cursor-screen-20260909-run01`，包含`AUDIT.json`、`README.md`、只读`verify_delivery.py`及汇总清单，关联前述两个CPU根与`remote-bootstrap-cursor-integration-20260909-run01`。实际运行根`artifacts/cu0909-s01`、`artifacts/cu0909-r01`。产品差异和原始文件保存在集成根`PRODUCT_DELTA.patch`、`before`、`frozen`，历史源/构建/证据不覆盖。

**当前状态：LOCAL_CPU_COST_REDUCTION_AND_ACTUAL_CAPTURE_INTEGRATION_PASS；REMOTE_GAIN_NOT_ESTABLISHED。** 后续继续围绕非本机有效吞吐：优先复用已有压缩像素/录像证据验证新locator的一致性及其余接收处理成本，尤其真实输入的FEC迭代和突发到达，不再盲扩codec参数或低容量保持候选。只在有新证据时推进一个有足够容量的改进。保护文件摘要不变、Git index空、无自动提交/推送，目标保持active。

## 18. 压缩像素一致性与 QC-LDPC 行归一化复用：降低计算成本，不放宽纠错

### 18.1 固定既有码流，不扩模型

复用第14节全彩/中和两份固定8M/4M-VBV、120帧码流；不重新编码，不修改PTS、像素变换或解码器参数。冻结FFmpeg解码依赖，用原媒体入口逐帧读取，随后采用同一4/3缩放。240张解码像素与180张历史预运行后缩放像素摘要一致。原locator与第17节cursor按帧交替调用，完整Bootstrap观察字段及资源计数240/240一致。全彩中位19.1832→17.0788ms、中和19.8024→17.7054ms，仅为同进程CPU定位耗时。

只在预定视频序号30、60、90调用原CPU oracle，导出已通过原frame/lane擦除门的75组16200个int16软判决；显式little-endian存储，每组32400B，共2430000B，未导出被擦除槽的陈旧metric。60组原FEC成功、15组失败，迭代为1/2/12，没有硬判决syndrome直接成功的0迭代样本。CPU oracle与GPU的槽准入可不同，因此该语料不是现场或GPU分布，不能套用其成功率到远控。

最初工具误用不存在的IsFrameAccepted API，编译失败发生在native运行之前；保留source/build01和错误记录，仅在新source02/build02改为实际IsFrameAvailable。生成manifest前的profileId也按原产品值固定为36bc661265e826c3。没有重跑成功native样本来挑选结果。

### 18.2 唯一纠错候选及精确差分

原QC-LDPC分层min-sum行处理中，每个target会对同一最小值或次小值重复做整数scale除法、offset和saturation。候选只把这两个计算移到target循环外：最小值每行一次；仅唯一最小值时计算次小值，否则复用最小值结果，避免归一化未使用的INT64_MAX sentinel。target的读取、符号、同值判定、消息更新顺序、饱和、syndrome检查间隔和迭代上限全部不改。

原int32总量与消息差的绝对值不超过2^32−1，scale不超过4096，乘积仍在int64范围内；三个固定矩阵的行度均至少2（实际Robust9–10、Balanced9–13、Fast15–19）。不新增缓存、分配或公共接口，不改矩阵/Golden/Profile、12次产品迭代上限或失败输出规则。

对上述75组固定metric，各做1次warmup与8次测量，原/候选交替调用，包含未变的原硬判决syndrome入口。**675对输出整码字、成功/失败、错误码/详情和迭代数完全一致**，原结果同时匹配采集时CPU oracle。每组入口均值1.8983978→1.4471265ms，下降**23.7712%**。这是固定语料FEC阶段CPU成本，不是23.8%的非本机吞吐增益。

native探针一次正常退出后，初版汇总脚本对空的0迭代子组除零；保留原日志和错误说明，只离线修正汇总，samples=0、耗时/降幅=null、StageNotObserved，未补跑native或制造样本。

### 18.3 产品落地与受影响验证

本节产品/测试只改：
- libs/PBInnerFec/src/qc_ldpc_codec.cpp：上述行内不变量复用；
- tests/PBInnerFec/test_inner_fec_row_normalization.cpp：新增固定矩阵行度、同值/零metric sentinel回归；
- tests/PBInnerFec/CMakeLists.txt：挂入原测试目标。

独立构建新FEC object，原依赖库不覆盖；MAP确认旧qc_ldpc_codec.obj没有进入测试/回放二进制。原InnerFec和Unified slot相关检查合计**40 cases / 47298 assertions PASS**，0.828s。构建24.531s，Job峰值942010368B；测试峰值3579904B。没有完整CTest、参数矩阵、压力或实屏扩跑。

### 18.4 两份既有码流接回原GPU/Receiver

新FEC与第17节locator一起链接到独立回放宿主，其余原GPU/Receiver实现及所有准入/资源/完整性规则不变。媒体前30帧只预运行，后90帧进入新原Receiver；两条件都严格到EOF，不延长彩色失败样本。**180条接收记录除processingQpc100ns外逐字段一致**，包括Bootstrap、FEC迭代/拒绝、payload摘要以及Receiver前后状态、槽决策、冲突/资源结果；4份像素审计文件逐字节一致。

- 全彩仍未建立Session，Outer unique=0，无发布，摘要/reopen为null；原失败被保留，不放宽准入。
- 中和仍以200个Outer不同块恢复262144B，在同一媒体PTS6000ms通过原整文件摘要、安全rename、reopen。停机后独立SHA-256/BLAKE3与原源相同：SHA-256 db7b4a708a31e631176094bead52fbce32d52b4283fbbaa96d12daaf4b0f2209，BLAKE3 8dff87818df79fd0d70acb71fe642adbab60893367ff072d037bffbab946ba39。
- 两回放进程10.875s/9.516s正常退出，Job峰值171261952B/171712512B，无相关残留。
- 新/旧Base、Fine、Chroma阶段累计耗时下降，但这是不同历史run，不是现场配对因果证据；中和ReceiverProcess含磁盘/发布的累计耗时反而75.6282→116.6995ms，如实保留，不加总重叠阶段当主时长。
- liveChannelGoodput、simulatedVerifiedGoodput、originalCaptureClock均为null；媒体PTS不是远控完成秒数。

初版审计读取旧SOURCE.json时误把嵌套digests当顶层，审计失败保留；仅离线修正。另保留中和recovery.resumeGeneration快照186→159：原runtime按steady_clock每秒checkpoint并刷新快照，处理速度不同会改变最后一次周期采样点；它不是帧语义计数或最终持久journal generation。除该字段外recovery相同；不称整个RunReport逐字节相同，也不将此验证升级为崩溃/断点恢复认证。未重跑native迎合审计。

### 18.5 封存与当前结论

主根 <repo>\artifacts\remote-fec-row-integration-20260909-run01，关联remote-compressed-cursor-fec-20260909-run01、remote-fec-row-cost-20260909-run01。PRODUCT_DELTA.patch、before/frozen保留确切产品差异，RUNTIME_IDENTITY记录实际CL/MAP和冻结库；AUDIT.json已经完成结果核验。早晨汇报时本节交付尚待整理；后续续跑增加README、只读verify_delivery.py、统一manifest及文档快照，封存是否通过以CLOSEOUT.json与VERIFY_DELIVERY_01.json为准。此整理不重跑native。历史失败脚本/构建/证据全部保留，第17节已封存交付不受影响。

**状态：LOCAL_FEC_COST_REDUCTION_AND_ORIGINAL_GPU_RECEIVER_EQUIVALENCE_PASS；REMOTE_GAIN_NOT_ESTABLISHED。** 第17节实际WGC整文件通过只覆盖locator；本节新增FEC尚无实际WGC/远控整文件运行。中和仍仅为已知正对照，不能推翻第15节低容量候选停止晋级的结论。接下来优先整理保留全彩容量、只改接收成本的独立候选，而不是继续扩codec样例。保护文件摘要不变、index空、不提交/推送、不替换旧现场包，总目标保持active。

## 19. 新定位＋纠错组合的实际WGC整文件检查

第18节已封存54个主文件、132个关联文件，保留1次初始编译失败；只读VERIFY_DELIVERY_01.json通过，不重跑native。随后独立构建PBReceiverCostScreen，只链接第17节locator和第18节FEC两个新对象，其余原DecoderRuntime/Receiver/WGC/GPU依赖冻结。宿主仅更新报告标签，MAP确认两个新对象及原runtime/Receiver确为执行者；没有替换GUI现场包。

只新增一对右屏实际像素运行：固定全彩15Hz、4/3、同一8454144B两Raw Segment源；Sender75秒、Receiver70秒预算、固定延迟2秒，无完成反馈。前后核对DISPLAY2物理[2560,0,5120,1440]、96DPI、180Hz。原整文件摘要、安全rename、reopen及停机后外部SHA-256/BLAKE3均通过。

- startAccepted→finalReopenVerified为39.4210093s，本机VerifiedEncoded/RawGoodput214457.8272B/s。
- Sender成功提交15.0026845逻辑FPS；Receiver UniqueVisualFPS14.9921237，619次观察/464个unique，Outer不同块6435。
- 首Bootstrap精确关联Sender Frame34、Carousel0；这不是远控迟加入认证。
- 原Base/Fine/Chroma的9285个已评估槽均通过，FEC迭代均0。因此本次证明组合实际捕获集成正确，不补充非零迭代现场性能证据。
- Bootstrap/GPU/post-FEC按尝试数摊销19.7221/4.6565/7.8887ms，非独立可相加阶段；不与历史不同Session作因果比较。
- Sender75.172s/226086912B峰值、Receiver39.594s/407453696B峰值，双方exit0；一次Sender可重试epoch拒绝由原runtime恢复，未改变错误规则。

一次离线审计启动早于固定75秒Sender结束，因sender-report.json尚不存在而停止。保持同一个exec session/PID等待终态后，再核对已落盘证据；没有重启/延长发送或根据Receiver完成结束Sender。EARLY_AUDIT_NOTE.json保存这一操作时序错误。

根目录<repo>\artifacts\remote-receiver-cost-screen-20260909-run01，实际运行根artifacts/fc0909-s01、fc0909-r01；AUDIT.json与RUNTIME_IDENTITY.json为本轮结果/执行身份。只增加独立artifact和3个文档增量，无产品代码改动；保护文件摘要不变、index空、无提交/推送，无新codec或远控运行。新组合状态ACTUAL_WGC_INTEGRATION_PASS，非本机提速仍未确立。

用户随后重新提出右上/左下空白区利用及右上可能遮挡的需求。只读初查见artifacts/remote-blank-region-feasibility-20260909-read01：两块保守608×64与现有33区域不重叠，约3.75%画布；直接延长原独立carrier不足新增完整码字，优先核对短控制包容量和独立擦除边界。不直接修改layout10，不删基础控制，不把面积或7.14%理想普通帧槽位增益当实测远控收益。

## 20. 空白带独立短控制原型：194字节完整分段记录无需跨区分片

### 20.1 容量选择和适用范围

当前PB-Control-1包裹开销30B；原WirehairV2 SegmentDescriptor为164B，完整分段控制194B。原Bootstrap的608×64/8×8二值格只能传76B码字、44B信息，不能照搬承载完整记录。故本轮只做独立的608×64原始BGRA patch原型，不生成正式整帧、不修改layout10、GUI、协议库或产品控制调度。

唯一候选采用4×4黑白单元，黑32/白224/底128；码字从patch(8,4)开始，148列、14行，使用2040bit，剩32cell不承载数据；周围保留水平8/垂直4像素底色。原型只读每格中心2×2像素，阈值128，距阈值不足32时保守拒绝整条。没有宣称它通过codec、完整画面定位/校准或实际远控。

短包实验标识PBB1/version1：magic4、version1、flags1、lengthLE2、SessionTagLE8、FrameSequenceLE8、CRC32CLE4，共28B；之后放完整原PB-Control-1，canonical零填充到223B，再加32B RS校验得到255B。CRC覆盖CRC字段清零后的全部223B，不替代原Control/Descriptor CRC、类型校验、冲突或整文件摘要。上限195B，足够当前194B分段记录；这是实验短包身份，不是已冻结的新完整视觉Profile。

RS采用原Bootstrap算法的独立全长参考副本，GF(256)/0x11d/首根0/32 parity，修改的是本原型的223信息字节/255码字和0 shortening；原产品RS(76,44)不动。全长前179个信息字节置零时，输出后76B逐字节等于原库短码；MAP核对两实现和原ControlPlaneReceiver的实际归属。它是最多16个byte error的错误纠正，不是带遮挡位置的erasure decoder。

### 20.2 实际完成的窄验证

固定262144B原源，新SessionId来自原OS CSPRNG；原serializer生成Session/Segment/Final完整记录121/194/114B。每条patch独立保存完整记录，两条不跨区组合。Decoder仅从patch像素恢复字节；本单元边界传入SessionTag/FrameSequence作为未来已验证父Bootstrap元数据的占位，不传入源字节或描述符内容。此处没有实际父Bootstrap采集，不宣称整帧新鲜度已经集成。

- 最终61项分组检查通过，6次独立patch恢复的控制记录经原ControlPlaneReceiver进入Session/Segment/Final绑定与重复处理。
- 每类记录固定1/16/17处byte扰动：1和16处全部准确修复；本次17处均拒绝，不能外推超预算错误永远能被RS检测，仍需要完整校验链。
- 右上全遮、左下全遮、两者全遮及一条宽200像素灰色部分遮挡按预期只使对应patch不可用；未验证任意遮挡形状/覆盖旧定位校准时的行为。首版是一条一个独立码字，灰遮挡可保守丢整条，不声称剩余小块都能继续利用。
- 非法版本/flags/length、异SessionTag、旧FrameSequence、非零padding、Control损坏、外层CRC错误、少1像素字节均拒绝；失败不改输出message。
- 校验有效但内容冲突的Segment，经像素解码后仍被原入口DescriptorConflict拒绝；不能当普通补充区擦除忽略。
- 合法100字节ASCII文件名产生204B Session控制，超195B短包上限，Pack拒绝且不改输出；不截断文件名/字段、不降低原正式最大长度。未来主区周期控制必须继续覆盖这类记录。
- 更严格但内部一致的测试资源策略拒绝过大Session且不创建Session；产品资源配置没有修改。

Python离线审计独立重建3份像素排列、用逐bit GF乘法重算RS parity，并校验短包、Control、Descriptor三层CRC及Segment源摘要。审计没有启动native。最终native0.047s、Job峰值3362816B；构建3.422s、峰值287129600B。只证明原始patch→完整Control→原控制状态，不涉及Outer文件恢复/发布或吞吐。

### 20.3 失败保留、交付和下一步

第一版fixture未显式设置ProtocolVersion，原FinalManifest校验在任何RS/像素操作前正确拒绝；新source02补GetProtocolVersion，并把RAW冲突fixture的raw/encoded摘要一起改变以保持结构合法。第二版在前面像素检查通过后，严格资源fixture把文件上限降到原prompt阈值以下，原构造器正确拒绝；新source03同时降低fixture prompt阈值，并分开断言合法构造与真实ResourceLimitExceeded。没有改原验证器或产品政策。复制第三版launcher时误改历史依赖身份文件名，发生在build/native前；仅修正那个引用。三套source/build、两次native失败、空/部分output和所有说明均保留。

主根<repo>\artifacts\remote-blank-control-primitive-20260909-run01；权威为RUNTIME_IDENTITY03.json、output03/result.json、AUDIT.json。build_and_run_v3_fixed.py是最后实际命令，历史脚本create-only不可原地重跑。最终只读复验不依赖未来当前源码树保持不变。

**RAW_SHORT_CONTROL_PRIMITIVE_PASS；FULL_FRAME_AND_CODEC_NOT_RUN；REMOTE_GAIN_NOT_ESTABLISHED。** 本轮没有把patch涂入正式帧、没有改主区控制、没有新增整文件/codec/实屏/远控运行；3个文档之外产品/工具工作树不变。下一步必须先给完整实验信号定义明确身份和父Bootstrap/局部混帧绑定，再做单组整画面主区不受损与固定压缩筛选；短包放得下不意味着可以立即撤去原控制槽。目标保持active。


## 21. 空白带完整实验身份、原始整帧遮挡与整文件参考链

### 21.1 明确新身份，不伪装旧格式

采用独立 `PB-Experimental-BlankControl-1`，线上ProfileId `0x504242414E443031`、layout11，SessionDescriptor与两个Bootstrap均在编码前写入同一新身份。没有把捕获后的记录改回旧ID；正式product catalog仍只有layout10并拒绝此新身份。本轮仅在artifact内派生32个PBModulation源/头到独立pbblankvisual命名空间，冻结主区算法/几何/FEC，新增两条可选PBB1控制带。原产品/GUI/工具工作树代码不改。完整合同见artifact的CONTRACT.md。

主区33区域之外增加(1056,16,608,64)/(256,1000,608,64)；两条分别承载完整当帧控制，均可中灰缺省。严格1:1、1920×1080 BGRA8 SDR，实际父Bootstrap从输入像素解出，PBB1的SessionTag/FrameSequence必须与之相等。固定30帧、PTS0..29/timebase1/15只是合成序号，不是现场时钟。主区每帧仍保留1个完整控制槽、14个数据槽，没有释放容量。

### 21.2 原ReceiverIngress与Storage，但不是原应用/GPU链

一个生成进程只读既有262144B源，使用OS CSPRNG Session、新身份、原canonical Control/Transport和WirehairV2(K200/1314B)。原始对照和补充区候选共享同一实验身份与全部主区像素；区别只有两条带的内容。两文件各248832000B，独立逐帧审计确认允许区域以外逐字节相同，连原控制、定位和校准均未改变。

五个新接收进程只接收像素文件、固定遮挡模式和新输出路径，不接收源路径/摘要/描述符sidecar/发送端状态。CPU reference提取的控制和数据进入未修改的ReceiverIngress；原编码/原始摘要验证、OutputFile写入/flush、commit、整文件digest、安全发布和reopen均保留。参考宿主的文件/免确认阈值收紧到262144B、并发Session收紧为1，其余原策略不改。没有运行原DecoderRuntime、GPU、capture、resume或GUI，不能称为原Decoder全链实屏证明。

| 原始像素条件 | 30帧接受补充带次数 | 主区观察 | 整文件结果 |
| --- | ---: | --- | --- |
| 同实验格式、不使用补充带 | 0 | 每帧15块 | PASS |
| 两条无遮挡 | 60 | 每帧15块 | PASS |
| 右上整条遮挡 | 30 | 每帧15块 | PASS |
| 左下整条遮挡 | 30 | 每帧15块 | PASS |
| 两条同时遮挡 | 0 | 每帧15块 | PASS |

五组均首次于合成ordinal14完成发布，150条主区block摘要/发布状态记录完全一致。停机后独立SHA-256/BLAKE3再次读取五个最终文件，与原源一致：SHA-256 db7b4a708a31e631176094bead52fbce32d52b4283fbbaa96d12daaf4b0f2209；BLAKE3 8dff87818df79fd0d70acb71fe642adbab60893367ff072d037bffbab946ba39。此ordinal不是耗时或goodput，所有goodput为null。

另有6项分组检查：旧产品Bootstrap入口拒绝新身份、有效CRC/RS但异Session/Frame的补充带拒绝且不改输出、200像素灰色局部遮挡隔离、两份父Bootstrap混帧拒绝两条补充带、243000个metric元素默认值一致。前一节已验证的短控制CRC/冲突/资源负例保持不变，未宣称任意遮挡、缩放或codec适用。

### 21.3 构建失败的真实归因与边界

初始构建触及2GiB Job限制并报MSBuild StackOverflow/Access violation。分别拆分为单源object目标、对unified_visual单源关闭优化后仍失败；这些假设被否定、3组source/build/log完整保留。随后绕开MSBuild单独调用相同cl，在同一2GiB限制下取得C1060定位：unified_visual.cpp第23行的243000项metrics{}聚合初始化。只把该数组改为默认初始化（每个元素的每个字段本来就有显式默认值），同/Od直接编译降至768614400B并通过；最终正式实验构建恢复/O2 /Ob2，并用全部243000项值检查验证初始化语义。

最终build04为23.500s、峰值1660243968B，预算未提高；生成0.610s/12230656B，五个接收进程约2.14–2.31s，最高30932992B，全部exit0。上述进程耗时不能当传输性能。原失败和直接编译探针均保留；该初始化改动只在独立参考副本，不是产品提速声明。

### 21.4 状态与下一步

证据根：<repo>\artifacts\remote-blank-fullframe-20260909-run01。实际成功入口build_and_run_v4.py，运行身份RUNTIME_IDENTITY04.json，独立审计AUDIT.json，复验REPLAY.md/verify_delivery.py。新原始生成1次、接收5次；新codec/实屏/远控0。保护文件摘要不变、index空，没有提交/推送或历史删除。

**RAW_FULL_FRAME_REFERENCE_MASKED_WHOLE_FILE_PASS；ORIGINAL_APPLICATION_GPU_AND_CODEC_NOT_RUN；REMOTE_GAIN_NOT_ESTABLISHED。** Step5当前仅为新结构的原始参考验证IN_PROGRESS，路线中旧NOT_STARTED表保留其初始时点含义，不把本节升级为整个Step5通过。下一步为单组固定codec A/B及失败恢复诊断，检查新增纹理是否拖累主区、补充控制在压缩后是否仍可用；不减少主区控制、不扩矩阵、不将这个参考宿主包装成原GUI。目标保持active。

## 22. 空白带固定codec A/B：主几何门和补充带置信度成为两个独立限制

### 22.1 本轮实际工作与回归

复用第21节同Session、同主区的30帧raw对照，不重新生成源/方程/raster。链接原14个pbblankvisual object，新宿主直接使用原RecordingMedia和原ReceiverIngress/Storage。未修改主区算法、正式layout10、主控制槽、产品或现场包；不是原DecoderRuntime/GPU/WGC。

参考宿主仅补齐压缩失败时的正常等待：非Session控制的UnknownSession按原应用规则等待；Session建立前数据不进入orphan缓存；验证分段写入/flush/commit后，必须等到实际接纳FinalManifest才执行原安全发布/reopen，其他错误不忽略。两个raw回归的30帧trace和result与第21节逐字节一致。独立guard仅从冻结raw读出控制和数据，验证未建Session时无输出/缓存，分段已commit但Final未到时不发布，释放所持实际像素解出的Final后原整文件通过。三份262144B输出外部SHA-256/BLAKE3与原源一致。

### 22.2 唯一固定条件及真实结果

1920×1080/15fps/30帧/2秒，原libx264 High4.0 veryfast zerolatency，8Mbit target/min/max、VBV4Mbit/init0.9，GOP15/IDR0与15/无B/ref1/单线程，原显式BGRA-full→BT709-limited/yuv420p/left。没有保持、色度中和或缩放；完整实际argv和codec运行身份封存。

| 条件 | 无补充带A | 有补充带B |
| --- | ---: | ---: |
| 实际码流属性/PTS/包字节VBV envelope | PASS | PASS |
| 视频packet字节 | 2206187 | 2197404 |
| 容器字节 | 2207048 | 2198265 |
| packet平均bit/s | 8824748 | 8789616 |
| 峰值packet字节 | 254673 | 289511 |
| Bootstrap可接受 | 15/30 | 30/30 |
| LocatorFailure / CanvasClipped | 15 / 15 | 0 / 30 |
| 主帧可用 / 主FEC迭代 / Outer接纳 | 0 / 0 / 0 | 0 / 0 / 0 |
| 补充带实际接纳 | 0 | 0 |
| Session / 最终文件 | 未建立 / 无 | 未建立 / 无 |
| 整文件 | NOT_RECOVERED | NOT_RECOVERED |

短流平均码率高于8M但通过既有带4M初始缓存余量的工程包字节包络，不声称完整Annex C HRD认证或两文件同字节预算。Bootstrap数量变化不能代表有效吞吐；两组在主FEC前被原几何准入拒绝，因此没有主数据样本可用于判定新增纹理是否拖累主区。accepted-byte真值检查在0接纳时是空样本，不能作为无误接受的额外证明。

CanvasClipped状态来自原ResolveUnifiedSamplingGeometryInternal拒绝；本轮没有记录具体几何浮点值，不能把这个枚举扩写为真实ROI被裁剪的证明。它与此前1:1固定模型的准入限制一致，不应继续沿此入口加帧或调码率碰运气。

### 22.3 保存局部像素，定位补充带本身的限制

两组每帧各保存两个608×64解码后patch，共120个；只作诊断，不越过父帧准入向Receiver传送内容。Python独立按原4×4单元/中心2×2/整数luma重新采样，与同帧冻结raw对照。

候选60条中21条硬判决255B码字与原始完全相同，每条硬判决错误最多6byte。可是原规则遇到任一96<luma<160的单元即拒绝整条：57/60条被拒，仅(ordinal0,右上)、(0,左下)、(15,右上)三条通过置信度条件。共2046个模糊cell、单条0..60个；这说明短控制的整条低置信度策略可能放弃可用信息，是独立于父帧几何的研究线索。

此审计没有执行RS纠正，没有任意遮挡或真实远控输入，不能把“最多6byte错误、小于16纠错预算”当实际接收成功。不调门限、不猜值、不把源真值送入Receiver。

### 22.4 负例、资源、封存与下一步

新codec编码只有2次、codec接收2次；原始回归2次和admission guard1次；截断媒体/短raw两个输入负例正确exit1且无发布；range、PTS、HRD缺失、IDR标志和缺帧五个离线inspector负例均拒绝。包括configure/build/检查器共17个子进程，所有owned Job/进程2GiB，build90s、encode60s、receive90s未扩限。实际build9.750s/峰值938844160B，两个encode约2.08/2.17s、峰值约201MB；CPU耗时不是非本机速度。

证据根：<repo>\artifacts\remote-blank-codec-20260909-run01。RAW_PROOF.json、CODEC_RESULT.json、PATCH_DIAGNOSTIC.json、AUDIT.json、RUNTIME_IDENTITY.json及CONTRACT.md/REPLAY.md为主入口。87个实际本地编译输入冻结副本、75个link输入hash、14个既有实验object和媒体依赖身份保留；只读复验使用冻结helper，不依赖未来当前工作树不变。没有屏幕、远控或网络设置变化，保护文件不变、index空、未提交/推送。

**FIXED_CODEC_AB_DIAGNOSTIC_COMPLETE；BOTH_NOT_RECOVERED；REMOTE_GAIN_NOT_ESTABLISHED。** 总目标保持active。下一步先复用第5节已验证的4/3显示变换和本轮已有两份码流，按原准入后的geometry定义补充带采样；不放宽原几何门、不再重跑同一1:1失败输入。显示适配和短带纠错不得同时变更；若代表性几何下仍有上述置信度限制，才设计有界字节擦除纠错，而不是降低原门限。主区完整控制继续保留，离线失败定位不是正式布局晋级或实际提速证明。

## 23. 用户停止时的收尾：4/3适配只读准备，尚未实施

收到目标继续指令后，只读核对了当前Git、第21/22节合同、已有ScaledSource/ScaleFourThirds，以及原ReadSample/ResolveUnifiedVisualSamplingGeometry。随后用户明确要求尽快收尾，已停止后续实现。此节没有新实验源码、build、编码、接收、实屏或远端运行，没有待续的native句柄。

新增`REMOTE_THROUGHPUT_RESUME_HANDOFF_2026-09-09.md`，集中记录目标、当前未提交工作、已验证CPU成本改进、空白带失败、准确恢复入口、不可重复路径及资源/安全边界，并在文档索引和本路线顶部链接。用户下次主动恢复后再从4/3显示采样开始，不执行旧的重复codec或中和保持计划。

暂停收尾重新只读复验第22节封存：308文件、26918995B通过；无native/codec/实屏新增。复验结果写到新的`<repo>\artifacts\remote-throughput-pause-20260909-closeout01`，旧交付未改。原保护文件SHA-256不变、index空，历史包/源/录像/build/记录原位保留，未提交/推送。总体目标仍未达成；此次为用户主动停止，不标记完成或技术blocked。
