# Encoder 预期接收速度先验：可行性评估

**简体中文** | [English](SENDER_RATE_PRIOR_RESEARCH.en.md)

日期：2026-10-21。**状态：研究结论与候选验证建议，不是已批准的实现设计，不是新功能或提速验收。** 本次不修改产品代码、调度默认值、协议、Profile、FEC、Golden 或持久化格式；没有构建、运行应用或执行性能测试。

**后续状态：** 用户随后批准跳过现场实验前置、直接实现，并限定首版仅 PAM4。该后续工作的配置、算法和本地验证边界见[实现说明](SENDER_RATE_PRIOR.md)。本报告保留为此前研究截面；其中“本次不实现/不测试”不再代表后续实现任务的状态，研究引用的旧提速仍不能移作新功能成绩。

## 1. 项目目标、阶段与本次问题

PixelBridge 的目标是在 Windows x64 上，仅通过 Decoder 实际捕获的可见像素完成非本机、单向整文件传输。优化目标是正确文件通过 `WholeFileDigest → safe publish → final reopen` 的总完成时间，而不是减少提示次数、使进度条持续增长或提高 Encoder FPS。禁止隐藏 payload / ACK 旁路，内存、队列、重组、解码器与恢复状态必须有界。

项目主阶段仍是 **v1.0 稳定性、显示兼容性与交付维护**。2026-10-15 全量静态审计尚未完成；本次是用户新提出的窄调度路线评估，不恢复旧的大规模现场扫描，也不把旧候选测试移作当前工作树验收。参见[现状](PROJECT_STATUS.md)、[总体设计](PixelBridge_最终技术路线与总体设计.md)。

用户提出：在 Encoder 配置历史 GUI 平均接收速度区间，例如 `200–280 KB/s`，随发送时间将更多发送机会分配到预测的接收前沿，少向更早的数据分配机会，以减少已完成分段的额外发送和尾部等待。

用户原意是“少发送”而不是“不再发送”旧数据，这一点与保留修复机会相容。下文的硬前缀跳过只作为风险对照，不把它当作用户已经要求的行为；需要补充论证的是少发到什么程度、怎样回访，以及错误先验时如何避免旧段饥饿。

**评估结论：有条件可行，值得窄实验；不推荐直接把 `rMin × t` 之前的数据认作已完成。** 建议研究“弱速度先验 + 实际发送服务账本 + 有界活跃窗口 + 不饥饿修复回访”。它能改善某些链路中的调度浪费，但不能获得真实接收状态，也不能保证所有链路更快或完全没有停滞。

## 2. 输入证据与不能确定的事项

### 2.1 本次用户提供的事实

- 用户确认：Decoder 先开始接收，Encoder 随后发送；属于新接收，Encoder 为 **30 Hz**。
- 截图显示 PAM4、97.9%、已接收约 210.0 MB、已验证约 210.0 MB、活动耗时 `00:15:31`、平均约 `230.8 KB/s`、ETA 21 秒，以及“收到重复或已完成分段，正在等待轮播中的缺失修复块”和“大小已 20 秒未增长”。数值均为界面舍入值，MB/KB 按现有 GUI 的 1024 进制解释。
- 用户暂时无法提供该次双端日志，明确要求先完成路线评估。

**没有该次 Session、完整文件长度、双端精确构建身份、逐段记录或最终完成报告。** 因此不能确定缺的是哪个 Segment、重复类型占比、是否恰好在全局 wrap，也不能证明截图中的二进制与本次查阅的源码完全相同。源码用于解释可能机制，不替代该次运行证据。

### 2.2 阅读与复核范围

全文阅读了本次起点的 71 份第一方维护说明，以及 2 份受保护历史报告；后两份只读、不纳管、不修改。范围覆盖双语现状/设计/指南、模式、协议、调度、恢复、遥测、呈现/捕获、Golden、fuzz、发布与工具说明。

另外按索引定向阅读了历史调度研究、短首轮实验记录及对应结果 JSON、Qt 候选最终交接、静态审计停止交接。构建树、安装依赖、打包副本及 `artifacts` 中大量重复文档/全部原始运行记录没有逐份重读；这不是全仓代码审计或全部历史实验复审。

本地只读取证与阅读台账保留于 `artifacts/speed-prior-evaluation-20261021/`：`document-reading-coverage.json`、`research-evidence.json`、`baseline/` 和 `sources/source-manifest.json`。这些是维护者本地材料，不是仓库下载资源。

## 3. 截图提示究竟说明什么

相关入口是 [DecoderActivityTracker::Observe](../apps/common/decoder_activity.cpp)、[活动文字](../apps/common/operational_log_qt.cpp)、[接收准入](../libs/PBReceiver/src/receiver_ingress.cpp) 和 [ReceiverPipeline](../apps/common/local_desktop_runtime.cpp)。

`WaitingForCarousel` 是短时计数变化形成的**观测性提示**，不是接收缺口清单。排除优先级更高的实际恢复操作、捕获/视觉停滞、最近资源等待等分支后，该提示要求：大小没有近期增长，没有最近 5 秒的新 unique/完成段进展，但最近 5 秒仍观察到 identical duplicate 或 already-completed 计数增长；连续 30 秒没有新有效进展会先进入另一提示。

两类计数必须分开：

| 类型 | 含义 | 对本题的影响 |
| --- | --- | --- |
| `outerIdenticalDuplicateSymbols` | 相同方程身份和相同内容再次到达 | 可能是重复观察、槽尾填充或 DirectRepeat；仅重排段的优先级不一定解决 |
| `outerAlreadyCompletedSymbols` | 数据指向 Receiver 已完成的 Segment | 即使 repair ID 全新，该段也不再需要它；调整段服务分配可能有价值 |

因此，“可能正在向已经恢复的段继续发送”有代码依据；“必然正在发送某个连续旧字节区间”没有依据。它也不是“重复检查代码导致停滞”：提示是在描述未增长的观测，不能靠删除提示或停止去重来增加独立信息。

GUI 的 `FormatDecoderProgress` 用本次活动耗时作分母，分子是 `max(estimatedReceivedRawBytes, verifiedRawBytes)`，并受文件总长约束。`EstimateReceivedRawBytes` 用 unique symbol 计数和已完成段的压缩比例估算在途原始字节；这不是逐段缺口的精确测量，也不是连续前缀。现有大传输/恢复估算问题仍按静态审计 SA-L03 保留，本次没有修复；不能把那个较大规模问题直接认作本次约 210 MB 新接收的原因。

ETA 只做“剩余估算字节 / 当前累计平均速度”，没有预测下一次有用 repair 的到达时间。因此 21 秒不是完成时限承诺。

## 4. 当前发送端已经做了什么

核对入口：[SenderFrameBuilder](../apps/common/local_desktop_runtime.cpp)、[scheduler](../apps/common/sender_carousel_scheduler.cpp)、[常量](../apps/common/sender_carousel_scheduler.h)、[GUI 模式配置](../apps/common/gui_visual_mode.h)。

| 当前源码分支 | 观察到的行为 |
| --- | --- |
| PAM4 GUI | 默认 7 MiB 原始分段；每个数据帧 10 个 Transport slots，每槽 payload 至多 1629 B；Session/Manifest 刷新帧的数据槽 inactive |
| 活跃发送窗口 | 通常至多 6 段；PAM4 按 sweep 旋转，减少固定采样相位饥饿 |
| 分段数不超过 12 | 保留小文件 whole-window barrier；不能用这个分支证明大文件策略 |
| 分段数超过 12 | 启用逐段 graduation；按本端累计实际提交的方程账本推进窗口，不是收到 ACK 后推进 |
| PAM4 大文件首访 | 常规累计目标约 `2K`；`K = ceil(EncodedSize / OuterBlockBytes)`，不是原始字节数。初始 scheduler pass 是 systematic 加约 10% repair 和 guard，必要的后续局部 repair pass 补到 graduation 目标 |
| 非空间交织后续全局 wrap | 在同一运行的累计目标上，每 wrap 再增加 `floor(K × 25%) + 1`；这是目标增量，不是把 ID 租约当作已播数据 |
| 帧率分档与 override | 通用 scheduler 有 ≤15 Hz / >15 Hz 的 later-pass 预算；大文件 builder 可以用“距毕业目标的剩余预算”覆盖它，不能只看 20%→40%→80% 的通用公式 |
| 崩溃恢复 | repair high-water / FrameSequence lease 防重用；运行内发送账本重新保守建立。租出的但未提交的 ID 不得记成功劳 |

后续纯 repair 可以帮助错过 systematic 的接收者，但仍必须给正确的 Segment 足够服务。新 repair ID 只意味着不同方程，不意味着它对已经完成的段仍有用。DirectRepeat 则继续使用其固定 systematic IDs，不改成虚构的 fresh repair。

### 4.1 与 30 Hz、230.8 KB/s 有关的量级检查

只在满载、RAW、分段数超过 12、普通 PAM4 首访、忽略控制开销/丢帧/尾窗等假设下：

```text
Transport payload 容量 = 30 × 10 × 1629 = 488700 B/s ≈ 477.25 KiB/s
约 2K 服务预算下的新原始数据前沿推进量级 ≈ 477.25 / 2 = 238.62 KiB/s
```

这说明：即使链路已经较早恢复某个窗口，发送端仍可能在其保守服务预算内继续停留。**238.62 与截图 230.8 的接近只是待验证线索，不是根因证明。** 它不是整文件 goodput 的严格上限；压缩、首尾边界、控制帧、实际提交率和捕获损失会改变结果，Decoder 也可能在发送端用满最后一个窗口预算之前完成。

不能反过来用 `230.8 / 477.25` 宣称实际丢失约一半：GUI 平均已经混合了调度浪费、压缩、等待与存储。当前数字不足以分离“信道未送达”与“送到了但段已完成”。

## 5. 为什么速度先验可能有帮助，又为什么不能直接裁掉历史

### 5.1 可获得的收益

历史速度与当前环境接近、从头接收、已完成段额外发送占比高时，较少服务已充分覆盖的早段、较早服务新的或近尾分段，可以提高实际到达信息的有效利用率。它不提高视频信道的物理容量，而是减少浪费。

若主要瓶颈是像素失真、重复视频帧、捕获/解调吞吐、准入预算或同步持久化，发送优先级无法直接修复这些问题。更激进地引入新段甚至可能占满未完成解码器，使旧缺口更难补齐。

### 5.2 平均接收量不等于连续前缀

反例：10 个等长段，接收状态 A 完成 0–8、缺 9；状态 B 完成 1–9、缺 0。两者接收总量与平均速度完全相同，急需的段却相反。Encoder 仅凭同一个历史速度区间不能区分它们。

所以 `rMin × t` 不是“最少已经完成的连续字节数”。既往表现“大部分时间处于区间”也不是有硬下界保证的服务曲线。永久不给旧段 repair 会把短暂停滞变成长期等待甚至无法完成。

### 5.3 时间和字节坐标都不能直接混用

- Encoder 的 Start 包含预扫描；Decoder 活动窗口从首次通过 Profile 准入的 SessionDescriptor 开始。两者起点不一致，不能用跨机时钟直接相减。
- 已知的本端准备/呈现暂停可以在候选模型中显式处理，但远控冻结、接收暂停和链路恶化不一定能被 Encoder 观察到。
- GUI 平均的分子是原始字节；FEC 服务成本由每段 `EncodedSize`、`K`、codec 与 repair 冗余决定。压缩率不同，等量原始字节不对应等量发送成本。
- 历史 GUI 平均包含旧调度的代价，不能直接等同于独立信道容量，否则会把原来的浪费再次拟合进新预算。

### 5.4 区间会随时间变宽，且有文件末尾问题

仅作算术说明，暂取模型时钟 `t=931 s`、先验 `200–280 KiB/s`：

```text
预测前沿带约为 [181.84, 254.57] MiB
宽度约 72.73 MiB；1 小时后宽度为 281.25 MiB
```

宽度已经超过一个常规 6×7 MiB 活跃发送窗口。先验带只能是优先级依据，不能要求把其中全部段同时编码/解码驻留。预测带越过文件末尾时也不能永久夹在最后一段；必须继续服务早期可能遗留的缺口。只给每段一个非零随机概率还不足以保证可解释的最大等待，修复回访需要明确的确定性服务边界。

## 6. 项目中已有正反实验，不必从零猜测

本轮重新读取了本地历史记录，而非重新执行实验：

- `artifacts/nonlocal-stall-20260913-2111/WORK_LOG.md` §21.25、§21.29；
- 同目录 `candidate-o-original-gpu-100-comparison.json`；
- 当前回归源码 [test_unified_encoder_workflow.cpp](../tests/PBApplication/test_unified_encoder_workflow.cpp) 中 `Short initial airtime retains full repair recovery without receiver feedback`。

| 历史对照 | 结果与边界 |
| --- | --- |
| 显式 spatial GrayFast，100000000 B，首访预算 100%→65% | 单次真实链路从 464746 ms 到 327833 ms，耗时减少约 29.46%；采样 repeat-only 等待从 184480 ms 到 48190 ms，最终摘要/发布/重开通过 |
| 同类真实 FEC/Receiver/存储探针，每 4 帧保留 1 帧，从头加入 | 100% 为 12349 帧，65% 为 20173 帧，反而慢约 63.36% |
| 同一 quarter 模型，15000 帧晚加入 | 27505→27645 总帧，无收益；不能隐藏相关 orphan/资源等待 |

这些实验说明“减少过量首访服务”确实可能有效，同时存在非常实在的反例。**不是预期速度区间功能已经验证，也不是本次 PAM4 的提速比例。** GrayFast 的空间交织、预算和较早候选身份必须保留，不能直接把 65% 或 29.46% 迁移给 PAM4。原有 GrayFast late-join `<12000` 门未达的事实不变，本轮没有重跑或关闭它。

## 7. 在线资料给出的启示与适用边界

检索/核对日期：2026-10-21。下表只把一手标准、原始论文、上游源码作为结论依据；搜索聚合页不作技术证据。均未将外部实现安装进项目或替换依赖。

| 一手来源 | 对本题的启示 | 不能据此推出 |
| --- | --- | --- |
| [RFC 3453 §1、§1.1](https://www.rfc-editor.org/rfc/rfc3453.html#section-1) | 无反馈 carousel 存在整轮等待和重复开销；扩展型 FEC 可减少相同方程的重发 | 独立分段中，给已完成段的新方程仍有价值 |
| [RFC 5775 ALC](https://datatracker.ietf.org/doc/html/rfc5775#section-1.2)、[RFC 6726 FLUTE](https://www.rfc-editor.org/rfc/rfc6726.html) | 无逐接收者反馈的对象/文件分发是成熟问题域 | 可以给 PixelBridge 引入网络、组播或带外描述符/ACK |
| [RFC 6330 §4.4.1–4.4.2](https://www.rfc-editor.org/rfc/rfc6330.html#section-4.4) | 对象分源块独立编码；足量符号要落在对应源块 | 全文件累计字节或累计方程足量即可完成每段；或本项目应改用 RaptorQ |
| [RFC 8680](https://datatracker.ietf.org/doc/html/rfc8680)、[RFC 8681 §3、Appendix C](https://www.rfc-editor.org/rfc/rfc8681.html#section-3) | 编码滑窗、冗余、时延和解码工作集要联合约束 | 其跨符号线性滑窗等同于本项目的“分段服务窗口”，或实时流的过期丢弃适用于必须恢复每个字节的文件 |
| [Bulut 等，2016，Raptor-enabled data carousel](https://link.springer.com/article/10.1186/s13634-016-0316-4) | 独立源块的最慢恢复会影响完整文件时长；交织与客户端速率限制都重要 | Wi-Fi 数字可迁移到远控视频；全文件交织可忽略接收活动名额与持久化成本 |
| [Jones、Chatzigeorgiou、Tassi，2014](https://andreatassi.github.io/publications/Performance_Assessment_FC.pdf) | 分别评估渐进恢复与全消息恢复；滑动步长会产生二者取舍 | 进度更平滑就代表总完成更快。论文研究的编码器不是 Wirehair 原位配置项 |
| [libcimbar 固定快照](https://github.com/sz3/libcimbar/blob/156fbb9c675ca9641f6559922b7025a0f2ee8125/src/lib/fountain/fountain_encoder_stream.h) 及其 `FountainEncoder` | 核对该实现读取完整流、保留数据并持续生成 block ID 的路径，可参考持续新方程思路 | 此全流驻留方式可直接满足 PixelBridge 大文件 `O(active segment set)` 的内存要求，或它已经实现本次速度先验 |
| [项目固定 Wirehair revision 的 V2 合同](https://github.com/catid/wirehair/blob/067ca7cdb66aed424ec23f97557429bf791c6f0c/V2_WIRE_PROFILE.md) | equation 身份绑定精确 descriptor 与 packet ID；调度与方程语义应分离 | 可以为了预测窗口改变同一 ID 的 payload、放宽 K/内存上限或省略摘要 |

另参考了 [SDMB 可靠传输论文](https://new.eurasip.org/Proceedings/Ext/IST05/papers/541.pdf) 的 carousel/FEC 资源取舍，以及 [Lin 等，2012](https://people.bu.edu/staro/CISS_2012.pdf) 的 rateless 分发调度问题。它们的卫星/多小区干扰模型不用于计算 PixelBridge 提速百分比。

## 8. 候选路线比较与建议

以下都是研究候选，不是用户已批准的产品行为。

| 路线 | 评价 |
| --- | --- |
| A：按时间硬跳过旧前缀，只发预测区间 | 不推荐；未完成早段、错误先验、晚加入/中断和文件末尾风险最大 |
| B：先验只调有界首访/毕业服务预算，保留完整修复路线 | 最低成本的机制对照候选；可复用现有发送账本，但不能将 GUI 速度直接换算成丢失率 |
| C：弱前沿权重 + 有界服务预算 + 确定性修复回访 | 推荐的研究方向；比 A 稳健，但须证明工作集与尾部服务没有回退，不能只加一个区间输入框 |
| D：跨段滑窗 RLNC / 新全文件喷泉编码 | 暂不选择；会触及协议、FEC、恢复和资源架构，不是本题所需的最小变量 |

建议先用 B 作为可隔离的机制试验，再判断增加 C 的时间前沿是否确实比“仅合理调整服务预算”有额外收益。C 至少应满足：

1. **先验只影响概率性优先级。** 输入为可选的原始字节平均速率经验，不表述为带宽保证、连续 ACK 或已完成位置；缺失/非法先验不能暗中改变既有策略。
2. **以实际服务账本为主。** 记录每段真正提交的新 systematic/repair 方程和最近服务时间；按该段 encoded 工作量计成本，ID lease、失败 Submit、重复 Present 不计新服务。
3. **前沿是软约束。** 对预测附近段增加权重，但保留前后缓冲、压缩/时钟不确定性；优先从有界活跃集合选段，不因带宽×时间增加活动 codec 数。
4. **旧缺口有明确回访。** 分配确定性、有限等待的修复服务，并测试最小足量访问；只有偶尔抽到旧段或每次给极少 repair 可能长期占住解码器。该边界约束发送服务，不承诺任意丢失下接收成功。
5. **预测结束不等于文件完成。** 模型越过文件末尾后采用有限尾段偏置并继续覆盖全文件修复，不永久钉在 EOF，也不因预测 ETA 自动停止。
6. **错误先验不能破坏正确性。** 不改变 descriptor、encoded bytes、同 key 冲突拒绝、Wirehair K/错误结果处理、DirectRepeat、资源预算、摘要/发布/重开；现有 Receiver 不需要知道先验。
7. **保持可诊断、可回退。** 后续若获准实现，先显式 opt-in，报告 requested/effective prior、实际调度策略、前沿估计/不确定性、服务账本和回访年龄；不把预测值写成 receiver progress。恢复/本端暂停的模型时钟与设置生命周期需在实现设计中明确。

这里没有规定未经验证的“80%主窗口/20%补发”、新的默认速率或 UI 方案；份额、回访间隔和风险预算应从定向反例与成本测量选出，不把任意数字当成果。

## 9. 后续验证顺序与停止条件

本次只制定验证建议，没有执行下列实验。

### 9.1 先区分症状与机制

如后续获得截图对应的双端日志，先核对 Session / Profile / build，再对停滞区间比较：

- `outerUniqueSymbols`、`outerIdenticalDuplicateSymbols`、`outerAlreadyCompletedSymbols`、verified segments/bytes 的增量；
- active/peak decoder、deferred/resource/orphan quota、resume/checkpoint 活动；
- Encoder 当前段、cycle、提交新帧，以及 Decoder 实际唯一身份、FEC/CRC 和捕获/视觉停滞。

现有低频日志不一定保存每个缺口或逐帧发送计划。若不能定位缺段，应承认观测不足；后续只增加有界、无 payload 的定向诊断，不伪造完整重放。

### 9.2 无屏幕、真实恢复链的窄对照

冻结当前源码/依赖/测试身份，独立新输出，分别比较基线、B、C；每一步只变一个调度因素。发送端不能读测试接收者状态；真值只供离线评估使用。

覆盖至少：0/1/6/12/13 段及多个窗口、RAW/高压缩/混合压缩/DirectRepeat、30 Hz PAM4 与独立 Wide、同平均率不同突发/周期采样、首段遗留缺口、晚加入、短长中断、先验明显过高/过低/随时间恶化、Sender/Decoder 重启、默认 8 个活动 decoder 与显式有限预算压力。保留现有所有失败门和最差模型，不只报告有利的 receiver-first 样本。

时间模型/理想 K 阈值模拟可先淘汰明显劣解，但不能证明 Wirehair rank、资源准入、checkpoint、摘要、安全发布及最终重开。必须复用真实 FEC/Receiver/存储探针，并补 PAM4 对应的大文件调度分支回归；现有 GrayFast 探针结果不能代替。

### 9.3 最后才做匹配环境的实际像素对照

后续另行确定现场范围后，固定包身份、文件、Profile、30 Hz、码面/ROI、呈现后端、预算和远控设置，交错基线/候选顺序；频繁筛选文件不超过 100 MB，且须真正覆盖 `>12` 段分支。小文件证据不能直接代替完整大文件和长尾。每个可接受输出均验证长度、整文件摘要、安全发布、最终重开与独立摘要；失败/超时/清理告警保留。

主指标是完整运行到 final reopen 的总耗时及 verified goodput；辅助报告 tail、最长/累计无新方程与无大小增长时长、已完成段浪费、控制开销、活跃/内存/磁盘成本。多随机种子的 P50/P95 可用于离线分布；真实少样本只能报告逐次值/范围，不能伪装统计置信度。

**停止/不晋级条件：** 任何正确性/冲突/资源/恢复回退；旧段服务饥饿；错误先验或晚加入出现未解释的大幅长尾；改善只能由删测试、换文件、改 Profile/FPS/预算等混杂因素得到；或者 C 没有优于 B 的额外证据。没有数据前不承诺固定提升比例。

## 10. 本次交付边界

本次完成的是文档与源码机制交叉核对、在线一手资料研究、条件性结论和验证建议。**没有新增“预期速度窗口”功能，没有证明这次截图停滞的唯一根因，没有关闭旧静态审计或历史 late-join 性能失败，也没有新的 PAM4 实测提速成绩。**

最终建议：保留用户“少发旧数据、更多服务预计需求”的方向，补上持续、可解释且有足量服务的旧段修复约束，不将先验误用成已完成断言。优先检验当前保守 airtime 的浪费，再证明时间前沿先验的增量价值。
