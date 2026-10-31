# 延迟只读文件反馈辅助视觉传输：可行性评估

> 后续进展：本文保留研究阶段的范围和在线资料结论；用户之后另行批准设计和实现。当前实现、设置与证据边界见[文件反馈辅助传输](FEEDBACK_ASSISTED_TRANSFER.md)，不能把下文研究阶段的“未实现/未构建”误当最新状态。

**简体中文** | [English](FEEDBACK_ASSISTED_TRANSFER_RESEARCH.en.md)

日期：2026-10-21。**状态：研究评估与后续设计建议，不是已批准的实现规范，不是现有产品功能或提速验收。** 本次不修改产品代码、默认调度、视觉 wire、Profile、FEC、Golden 或恢复格式；没有构建、运行产品或执行传输测试。

## 1. 结论与已确认需求

**可行，值得作为显式可选的辅助能力继续设计。** 即使反馈延迟数秒，只要它最终可见且开销可控，也能让 Encoder 获知速度先验无法推断的具体接收缺口，减少向已完成段继续发送和等待全文件轮播的浪费。推荐方向是：

> 正向文件载荷仍完全通过可见像素；反向只传有界的接收状态。使用分段级确认、流水发送和失效回退，不逐帧停等，不凭百分比跳过文件前缀。

用户本轮明确的使用场景：

- Decoder 所在电脑可写一个本地目录/专用文件；Encoder 通过对应映射路径只读访问，更新可能立即可见，也可能晚几秒。
- 两端分别选择各自看到的对应位置，再按原来的先接收、后发送流程使用；未启用时保持当前行为。
- 经本轮交互确认：**一个 Encoder 只服务一个指定 Decoder**，不是多接收端广播。
- 经本轮交互确认：**Decoder 完整验证成功后，希望 Encoder 自动安全停止**，不只优化发送内容。
- 本轮请求是评估和维护文档，不是开始实现。下文的模块、字段、参数和分期都是建议；确认使用目标不等于批准实现规范。

无法从现有材料确认真实映射的延迟分布、缓存/替换/取消语义，也没有本功能实测。不能承诺固定提速比例、所有场景都更快或绝无停滞。

## 2. 项目目标、阶段与阅读范围

PixelBridge 的目标是 Windows x64 非本机远控环境中的正确整文件视觉传输，主指标是完成 WholeFileDigest、安全发布、最终重开复验的总耗时，而不是发送 FPS、进度平滑程度或理论容量。

项目当前处于 **v1.0 稳定性、显示兼容性与交付维护**。今天另有用户批准并已实施的 PAM4 接收速度先验；其本地构建/回归不证明真实链路提速。2026-10-15 的全量静态审计仍未完成，历史 Gray Fast late-join 性能门仍保留。参见[现状](PROJECT_STATUS.md)、[先验说明](SENDER_RATE_PRIOR.md)、[证据索引](EVIDENCE_INDEX.md)。

本轮逐份全文阅读起点的 78 份第一方维护文档，包含双语指南/架构/现状、模式、协议、调度、恢复、遥测、捕获/呈现、Golden、fuzz、发布、工具以及既有 Qt 设计/计划；另只读全文阅读 2 份受保护历史报告，并定向阅读静态审计停止交接和今日候选包交付说明。受保护报告不修改、不纳管、不复制到公开文档。

构建树、安装依赖、发布/源码备份副本，以及 artifacts 中全部原始实验记录没有逐份重读；没有把“读完当前维护文档”表述为全仓代码审计或全部历史实验复审。完整阅读台账、起点哈希、源码定位和本次文档差异留在本地 artifacts/feedback-assisted-evaluation-20261021/。

## 3. 这是怎样一种通道

~~~text
电脑 A：Encoder ── 可见数据码面/既有远控画面 ──> 电脑 B：Decoder
                文件内容只沿这条方向恢复

电脑 A：只读对应映射文件 <── 延迟可见的状态快照 ── 电脑 B：写本地反馈文件
                只反馈接收状态，不传文件内容或解码块
~~~

“Encoder 只读”并不妨碍 B→A 传递信息：信息由 B 修改内容产生，A 只需读到变化。反之，A 无法通过此机制写回 B，不能设计依赖 A 在映射目录确认、加锁、清理或发布命令的握手。

现行[总体设计](PixelBridge_最终技术路线与总体设计.md)不仅禁止隐藏载荷，也明确没有接收 ACK。因此这不是在现行“完全无反馈”声明下偷偷复用日志，而是需要公开命名的**可选反向状态控制面**。本轮只研究这个用户提出的架构例外；后续实现前，应同步审阅 AGENTS/总体设计、指南、遥测和兼容说明的相关边界。

PBBridge 继续仅作开发编排，不能复用其 listener、命令树、脚本执行或远程 stop。新功能也不读取另一端的 .part/.resume、恢复块或完整日志作为调度协议；Decoder 的文件字节仍只能来自其实际捕获像素。

## 4. 为什么它比速度先验更直接

本轮定向复核了当前 SenderFrameBuilder、scheduler、ReceiverIngress、存储提交与 EncoderSessionStore，而不是仅从注释猜测：

| 当前机制 | 对新路线的意义 |
| --- | --- |
| 常规有界发送窗口最多 6 段；超过 12 段时采用 graduation | 可以保留有限工作集，但不能把新反馈误当成已有接口 |
| 大文件默认目标基于本端成功提交方程的账本；普通 PAM4 首访约 2K | 已恢复段仍可能获得额外服务；发送达标不等于接收达标 |
| 后续 repair ID 可以全新，但接收端对应段已完成 | 新方程仍可能无用；应减少该段服务，而不只是消除同 ID 重复 |
| 现有先验根据原始字节速率×本端模型时间软调访问预算 | 它不知道真实缺段，无法提供 ACK；反馈是另一类信息 |
| Receiver 有权威的 storedSegments 集合及耐久完成顺序 | 有机会导出可靠段集合，不必从 GUI 百分比倒推 |
| 现有 ReceiverIngress 的公开资源遥测主要是聚合量 | 精确活动段/需求列表还需明确的新只读观测接口，不能声称现已具备 |

源码入口：[发送 runtime](../apps/common/local_desktop_runtime.cpp)、[scheduler](../apps/common/sender_carousel_scheduler.cpp)、[ReceiverIngress](../libs/PBReceiver/include/pbreceiver/receiver_ingress.h)、[Session store](../apps/common/encoder_session_store.cpp)。本地台账保留当次文件哈希和行号；这些是静态机制证据，不代表某个旧现场包执行了同一源码。

例如 100 段中已完成 99 段，真正缺的是第 8 段：精确反馈能把更多机会给第 8 段；“99%”或总接收字节无法区分它与“缺最后一段”。这是前次速度先验研究中非连续前缀反例的直接延伸，详见[先验评估](SENDER_RATE_PRIOR_RESEARCH.md)。

## 5. 数秒延迟为何仍可能有效

### 5.1 不采用逐帧或逐段停等

不应“发一帧/一段，等映射文件变化，再发下一帧/段”。应持续给有限的未确认段发送，在等待反馈时服务其它段；反馈到达只影响后续机会。

仅作量级算术：若一个段为 7 MiB、有效原始文件速率假设为 250 KiB/s，则服务一个段约需 28.672 秒。每段额外停等 5 秒会把这一理想模型的效率降到约 85.15%；流水处理可避免把每次 5 秒都串进关键路径。这个假设速率不是当前测量，也不等于 encoded/FEC 速率。

吸收反馈延迟需要额外约 R×D 的可服务工作量；同一假设下是 1,250 KiB。它说明几秒延迟并非天然不可用，**不证明现有 6 段窗口在所有损失、压缩、资源条件下都足够**。窗口还受当前段服务时间、活动 decoder 名额、存储和相关丢帧影响，不能仅按带宽时延积扩 RAM。

### 5.2 主要收益和无收益情形

最可能获益：较多已完成段仍在发送；窗口驻留过于保守；接收尾部只有少量洞；部分旧段占用 decoder，需要集中修复后释放名额。

收益有限甚至变慢：映射延迟接近/超过整轮；反馈读写争用远控链路或磁盘；快照发布阻塞接收；瓶颈主要是坏像素、视频冻结、重复观察、CPU 解调、存储同步或最终摘要；反馈引入新段过快导致资源暂缓。

反馈不会提高同一 Profile 的 payload/frame 或修复不可读像素。可减少的是调度浪费与等待，不是凭空增加视频信道容量。

## 6. 路线比较

| 路线 | 评价 |
| --- | --- |
| A：只反馈百分比、总字节、速率和 ETA | 适合展示，不足以精确补洞；不推荐作为跳段/停止依据 |
| B：耐久完成段集合 + 可选有界活动/需求提示 + 原 FEC 流水发送 | **推荐**；直接减少无用段服务，保留现有恢复与文件安全链 |
| C：逐方程 ACK/选择重传，或换成反馈感知 LT/Raptor/RLNC | 首版不推荐；状态量、缓存一致性和 wire/FEC 变更远超所需 |

B 还可分层验证：先证明“完成段集合”确有收益，再增加“当前活动缺段、资源压力、粗粒度所需修复量”。后者不是 Wirehair 精确秩，也不能因为收到 K 个不同 ID 就宣布恢复成功。

此能力在原理上不依赖 PAM4；但四种模式、小文件 barrier、DirectRepeat 和 Gray Fast spatial bank 的接线/原子边界不同。建议先在主要 PAM4 路径证明，再按同一合同扩展；**本报告没有替用户批准具体首版模式矩阵，也不把旧 prior 的 PAM4-only、>12 段限制自动搬过来**。

## 7. 推荐反馈的语义，不冻结具体格式

建议使用专用、完整、有界、可重复读取的状态快照，而不是只能顺序消费的事件日志。漏掉中间几次更新后，下一份完整快照仍可恢复当前视图。字段名以下仅为说明，不是已冻结 schema。

| 内容 | 要解决的问题 |
| --- | --- |
| magic/schema/version、精确长度、完整性检查 | 拒绝错误文件、截断、撕裂和不支持格式；校验和不是认证 |
| 完整 SessionId、精确 Profile/layout、文件大小/段数、canonical Session/已绑定 FinalManifest 身份 | 防止跨文件、跨 Session、跨模式误用；仅文件名或 SessionTag 不足 |
| 无个人信息的 ReceiverId、随机 receiverEpoch、单调 snapshotSequence | 指定一个写者，区分重启/换输出/重新接收，拒绝回退与同序不同内容 |
| durableCompleted 段集合 | 只表示经验证、落盘和恢复提交的具体段，不是连续字节前缀 |
| 可选的有限活动段/优先修复提示、资源状态 | 辅助先补在途缺口，避免不断引入新段；不授权增加资源预算 |
| 从实际视觉帧观察到的 FrameSequence | 支持本次发送绑定和延迟诊断，不能从预测值/文件快照伪造观察 |
| 接收阶段与最终验证结果 | 区分恢复、落盘、全文件验证、最终完成和失败 |

不要求 Decoder 计算 Encoder 整个本地 descriptor bundle，也不传源路径、输出路径、文件内容、恢复块、脚本、任意命令或截图。身份摘要应来自双方实际可共同验证的 canonical 记录；未知或未绑定状态不能伪装为有效确认。

当前段数上限 65,536，完成位图本体至多 **8,192 B**；Base64 约 10.7 KiB，仍很小。相比“每段一个 JSON 对象”，位图或有严格展开上限的 ranges 更可控。最大文件、活动提示条数、字符串、嵌套深度、整数范围、重复键、尾随数据及位图未用尾位均须限制；不能先按外部声明分配再校验。可将单快照 64 KiB、约 1–2 秒合并发布/读取作为起始实验参数，**不是已验证最优默认**。

## 8. 完成事实应在什么时刻产生

### 8.1 分段确认

只有依次完成 EncodedDigest、有界解压、RawDigest、.part 写入/flush/checkpoint、completed journal 耐久记录及 Receiver stored commit，才把对应位设为完成。当前 runtime 的 RecordCompletedSegment → CommitStoredSegment → storedSegments 更新是合适的候选观察点；不能在 FEC ready、unique symbol、GUI 估算或仅 Transport CRC 时确认。

重启后必须先按现有恢复流程重算已存段摘要，再发布新 epoch 的当前集合。不能跨 epoch 盲目 OR 旧位图；不允许反馈替代 .resume 验证。旧静态交接保留了大传输/恢复估算问题，更不能复用在途估算生成确认。

### 8.2 整文件完成与 Encoder 自动停止

用户要求的自动停止可以实现，但条件应是：

1. 反馈身份与当前发送及指定接收端匹配；
2. 整文件长度/摘要与 Encoder 的 FinalManifest 一致；
3. Decoder 报告 WholeFileDigest、安全发布、最终重开复验全部成功，而不是所有段齐全、PublishIntent、发送完一轮或 ETA 到零；
4. 该证据属于本次 Start，且停止动作仍投递给同一个正在运行的发送世代；
5. 通过本节以下的新鲜度、单写者/信任前提检查。

Encoder 只能显示“接收方报告已验证完成”，不能声称自己经映射读回最终文件独立验证过。停止复用本地安全生命周期，不调用 PBBridge、注入按键或强杀进程；不删源文件或恢复状态。反馈错误不让 Decoder 发布，也不把传输 CRC 升级为认证。

最终反馈应持久保留在有界状态文件中；B 不知道 A 何时读到它，不能发布一次就删掉、短 TTL 后清理或等待 A 写确认。发布失败应提示“文件已正确完成，但辅助反馈失败”，不能回滚正确 final，也不能让 A 在没有有效终态时误停。

## 9. 旧完成文件、世代和视觉新鲜度

只检查 SessionId 仍不够：Encoder 可恢复同一个不可变 Session，旧完成文件也可能仍在映射缓存中；snapshotSequence 变大也不能证明内容现在才生成。

一个值得验证、无需改视觉 wire 的候选锚点是**现有持久化防重用 FrameSequence**：

- 当前 live Encoder 从 Session store 的 lease endpoint 起播，使用前耐久租号，仅成功 Submit 后推进实际帧号。
- Decoder 在通过当前 Session/Profile 视觉身份检查后，反馈它真正看到的帧号。
- Encoder 检查该帧号属于本次 Start 实际成功 Submit 的范围，而不是旧运行、未用租约或未来帧号。
- 不直接使用 durableFrameSequenceLeaseEnd 作已发送上界；现有 EncoderSnapshot.frameSequence 通常是成功后自增的 next 值，不能误当 lastSubmittedInclusive。

这能排除**上次 Start 遗留**的诚实终态，且不需要 A 写 B 的目录或新增视觉字段；它不是对不可信写者的挑战认证，也不证明当前目录视图就是全局最新。

该候选还依赖**唯一 Sender、可信且未克隆/回滚的 Session cache**。现有目录 owner lock 不跨独立缓存副本，CRC 也不防旧备份回滚；复制旧状态可能重用同一 Session 的序号范围。若正式要求覆盖这些情况，应另行设计真正每次 Start 的随机视觉挑战及其兼容/恢复语义，不能继续承诺“绝无 wire 变更”。

视觉锚点必须来自当前 live capture 中、通过所选 Profile 现有准入且与完整 SessionDescriptor 绑定的帧，而不是 cadence tracker 的最大序号；现有路径中 Bootstrap-only/erased 观察也可能推进帧率遥测。锚点不能要求一定产生新的 Transport unique 方程，否则会排除空文件和合法恢复完成。当前尚没有满足全部条件的现成反馈 API。

还必须解决三个当前源码没有自动提供的能力：

1. **晚到回调：** 当前 RequestStop 没有 expectedRunGeneration。文件线程不得校验后直接调用它；应把带本次 Start token 的候选事件交给运行 owner，使用前再次比对世代，避免旧读取停掉新运行。
2. **完成后重启：** 当前 Decoder 完成后停止捕获，published pipeline 也不再处理新帧。若 A 在 B 已完成后重启，旧终态没有新 Start 锚点，不能自动通过。要覆盖此恢复场景，辅助模式需设计“重验完成凭据/最终文件 + 有界地重新观察本次视觉锚点”的终态确认生命周期；否则只能明确提示需要人工确认，不能悄悄接受旧终态。推荐后续设计把此恢复路径纳入自动停止验收，不把问题留给短 TTL。
3. **长验证阶段：** 最终摘要/重开可能耗时，最后观察帧不一定很新。调度提示的短时效与最终完成证据应分开，不能用数秒视觉 TTL 误拒合法完成，也不能因此完全取消当前 Start 绑定。

新快照被首次读到的本地时刻不等于其真实生成时刻；重复读到同一序号不能续期。两端 monotonic 时钟不能直接相减。可用本端提交记录与回显观察帧建立保守视觉往返年龄观测，但不能从轮询周期推导映射延迟上界。

终态的身份、位图、摘要、三层验收结果和视觉锚点必须来自**同一不可变快照、同一 epoch/sequence、同一次权威状态生成**，不能把旧终态与新帧号拼接。随机 epoch 本身没有可比较的新旧顺序；需要 B 侧单写者锁和明确接管规则，而不是看到任意陌生 epoch 就自动切换。元数据也不能靠无限保存 retired epochs 维持安全。

终态究竟表示“当前接收世代状态”，还是“本次 Start 已经达成交付的持久凭据”，应在正式设计中选择并统一恢复语义。B 启动下一任务时也不能悄悄覆盖延迟读者尚未看到的唯一终态；需有限的终态保留/显式替代策略。若映射永久不可见，或 B 在发布完成凭据前退出，任何只读反馈协议都不能保证 A 自动停止；安全退化是继续原发送并允许人工停止。B 只能证明已发布反馈，不能仅凭写成功声称 A 已停。

## 10. 只读映射的文件系统工程

### 10.1 建议的用户入口

优先选择**专用目录，由程序管理一个约定反馈文件**；两端各自填写本地看到的路径，例如 Encoder 侧为通用形式：

~~~text
\\Client\<mapped-drive>\<feedback-directory>\<status-file>
~~~

两边的文本路径不要求相同，也不凭字符串相似认定对应。Encoder 支持直接粘贴映射路径，不要求映射盘符；只读检查不得以写测试文件验证权限。若支持指定单文件，也应是专用程序所有文件，不能覆盖任意已有用户文件。只访问约定名称，不递归扫描桌面、不自动发现其它目录、不读取相邻用户文件。

### 10.2 发布、读取与缓存

- B 在同目录写完整临时快照，flush/close 后以经过验证的本地同卷替换语义发布；失败保留上一份完整状态。不要原地覆盖一半便被 A 读走，也不要跨卷 move 假装原子替换。
- A 仅用 GENERIC_READ + OPEN_EXISTING；短时打开、读完整有界内容、关闭。适当使用 FILE_SHARE_READ/WRITE/DELETE 允许 B 替换；FILE_SHARE_DELETE 是共享许可，不是赋予 A 删除权限。[CreateFileW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew)
- 对一次打开得到的完整快照做长度/内容/身份校验，不能在路径上先查长度，再从另一份被替换对象读内容。拒绝半写、同序不同内容、越界或错身份，不把错误快照“部分应用”。
- 本地同卷替换不意味着映射端立即可见；通知、目录项、文件内容和长持有句柄可能有不同缓存。周期性重开是实用候选，不是强一致性证明。
- Windows 官方明确某些 redirector 不支持 ReadDirectoryChangesW，网络通知也有缓冲限制。因此通知只能辅助唤醒，不能是唯一正确性来源。[ReadDirectoryChangesW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-readdirectorychangesw)
- Microsoft 的 SMB 调优说明列有文件信息、目录和 negative lookup 缓存；它提醒“文件不存在/mtime未变”不等于没有更新。但该说明是 SMB，不可套用为所有 client-drive redirector 的准确缓存模型，也不建议本功能自动改全局注册表。[SMB client cache parameters](https://learn.microsoft.com/en-us/windows-server/administration/performance-tuning/role/file-server/)

### 10.3 UNC 不等于普通 SMB；不能承诺强制取消

Citrix 官方支持 client-drive mapping 和只读 client-drive access，说明用户描述的能力在产品机制上是现实的；它没有承诺任意映射的更新延迟、目录通知、替换可见性或取消时限。[Citrix Client Drive Mapping](https://docs.citrix.com/us/en/citrix-virtual-apps-desktops/2511/devices/client-drive-mapping.html)

读取必须与 UI、D3D owner、FEC/存储提交隔离，使用有限 worker、单槽最新快照和有界重试/退避，不能每次超时再启动一个线程。Windows 的 CancelIoEx/CancelSynchronousIo 不是所有驱动上的硬超时保证；把同步 UNC read 放进线程再设置 wait timeout，不能证明安全停止有界。[Canceling Pending I/O](https://learn.microsoft.com/en-us/windows/win32/fileio/canceling-pending-i-o-operations)

后续必须验证实际 redirector 的阻塞/取消/关闭行为；若不能给主程序退出建立边界，需评估有界隔离读取进程等方案，且只交换辅助元数据、不传 payload。这里没有承诺现有 worker 就能满足，也没有批准新增 helper 架构。

## 11. 调度、回退与信任

### 11.1 调度建议

新鲜、正确绑定的反馈生效时：

1. 优先服务真实未完成且已经占用接收资源的段，给旧洞足够修复量；
2. 已耐久确认段可提前退出本次服务或明显减少服务，不永久删除描述符/源及 repair high-water；
3. 有余量才引入新段；不把缺段位图中的全部段同时加载为 codec；
4. 保留 Session/Manifest/当前 Segment 控制刷新，不因“对方已知道”省略 wire 所需控制；
5. 不修改 pending raster；只在已完成的原子帧/相应 bank 边界更改下一次选择。若提前结束一个 pass，必须核算已提交服务并安全退休已租未用 ID，不能直接把 graduated 设 true 破坏当前假设；
6. 为未确认段保留确定性、公平且足量的兜底服务；不受可信确认永久保护的段，在提示失效时重新进入原有全文件覆盖。

Wirehair descriptor、encoded bytes、同 ID 的方程含义、DirectRepeat、资源/OOM/配额结果和接收摘要不变。反馈只改变服务顺序/预算，不要求某一缺失 repair ID 必须重传；优先使用该段合法的后续新修复方程。

### 11.2 状态与失败

建议状态可区分 Disabled、WaitingForMatchingFeedback、Active、Stale/Unavailable、Rejected/Incompatible、ReceiverReportedComplete。这不是已存在枚举。

- **关闭：** 不建立反馈 reader/writer，不访问映射，不因残留文件自动停止；原调度和原人工/显式超时停止不变。
- **未收到、丢失、过期或读取错误：** 不停等，回到用户本来配置的无反馈策略；若原已启用速度先验，基线就是该先验，而不是暗中清除其它偏好。
- **错 Session/Profile、畸形、多个写者或同序冲突：** 拒绝该反馈/暂停辅助能力并显示原因；不放宽像素准入、不清空正确恢复状态、不停止有效传输。
- **epoch 变化：** 使旧反馈/晚到读结果失效，等待新绑定和恢复重验；不能把两代状态拼接。
- **全段齐但最终验证未结束：** 不能当作完成停止；接收错误和反馈不可用也不能伪装终态成功。
- **旧版本另一端：** 没有有效反馈便继续原无反馈路径，不自动开启任何旁路或改变视觉协议版本。

真实反馈与既有速度先验不得无说明地同时争夺权重。建议按“经过验证的具体接收事实优先，未知/失效部分仍有原策略服务”设计组合，并先分别比较原调度、仅先验、仅反馈；组合是否值得加入由额外证据决定。

### 11.3 只读不是认证

只读限制 A 的写能力，不保证文件来自正确 Decoder。校验和、随机 SessionId、FrameSequence、generation 和公开摘要都不认证写者。基础场景必须明确依赖选定专用位置的单写者与可信访问控制；存在不可信写者时，需要独立配对/MAC/签名等认证，或禁用可触发自动停止/硬跳段的信任动作。

恶意完成位图可能造成饥饿或早停，虽然不能让 Decoder 绕过整文件校验。不能把“文件正确性仍 fail closed”宣传为“错误反馈不会影响可用性”。安全边界须在正式设计中说明，不能从映射路径名称自动推出。

## 12. 在线一手资料及可借鉴边界

核对日期：2026-10-21。仅使用原始论文、官方标准/文档和上游实现支持技术判断；未安装依赖、没有引入网络传输协议。

| 来源 | 可借鉴结论与边界 |
| --- | --- |
| [RFC 5740 NORM](https://www.rfc-editor.org/rfc/rfc5740.html)，§2、§4.3.1 | FEC 修复可以配合非对称返回路径；缺口可表达为项、范围或块擦除量。借鉴控制语义，不把 IP multicast/NORM 接入产品，也不照搬多接收者抑制 |
| [RFC 6330 RaptorQ](https://www.rfc-editor.org/rfc/rfc6330.html)，§4.4 | 方程必须作用于对应源块；足量接收总数不等于每块恢复。本项目无需因此更换 Wirehair |
| [RFC 6726 FLUTE](https://www.rfc-editor.org/rfc/rfc6726.html) | 提供成熟的无返回路径对照；原无反馈能力仍有独立价值，不被辅助功能取代 |
| [Sun 等，2014，Limited Feedback + Rateless](https://arxiv.org/abs/1404.6683) | 研究稀疏 ACK 与无精确 CSI 的调度，支持“不必逐包反馈”的方向；蜂窝调度/功率模型和收益不能迁移为本项目百分比 |
| [Sørensen 等，2012，Rateless Feedback Codes](https://www.merl.com/publications/TR2012-052) / [原论文](https://www.merl.com/publications/docs/TR2012-052.pdf) | 少量反馈可以服务于特定编码方案优化；其 LT/Raptor degree 重设计不是本项目已冻结 Wirehair 的配置项，首版只改外部调度 |
| [Leith、Karzand，2016，作者出版目录](https://www.scss.tcd.ie/Doug.Leith/publications.php?name=karzand) | 直接相关的延迟反馈/吞吐与时延研究线索；本轮作者 PDF 获取失败，未据未阅读全文推导具体公式或算法 |
| [QRTransfer](https://github.com/tony-xlh/QRTransfer) | 上游明确有“接收端告知已扫描内容，发送端跳过”的视觉传输示例。它不是大文件远控实测，也不是可直接移植的 Windows FEC/持久化实现 |
| [qr-file-transfer](https://github.com/thealper2/qr-file-transfer) | 展示独立 ACK、Session/packet 绑定、最终摘要后 FINISHED 和逐包停等；适合作为正确性/反例参照，不适合直接照搬到数秒延迟反馈 |
| [TXQR](https://github.com/divan/txqr) | 动画 QR + fountain 是无反馈视觉传输对照；不能据此声称已有本次文件反馈 |
| [libcimbar 固定源码](https://github.com/sz3/libcimbar/blob/156fbb9c675ca9641f6559922b7025a0f2ee8125/src/lib/fountain/fountain_encoder_stream.h) | 继续新 fountain block 的参考；该路径读取整个流驻留内存，不可照搬为本项目大文件工作集 |
| [固定 Wirehair V2 合同](https://github.com/catid/wirehair/blob/067ca7cdb66aed424ec23f97557429bf791c6f0c/V2_WIRE_PROFILE.md) | canonical descriptor 与 packet ID 固定方程，且本身不认证内容；调度优化不得改变同 ID 的含义 |
| [Citrix 映射](https://docs.citrix.com/us/en/citrix-virtual-apps-desktops/2511/devices/client-drive-mapping.html)、[Win32 通知](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-readdirectorychangesw)、[只读打开](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew)、[取消 I/O](https://learn.microsoft.com/en-us/windows/win32/fileio/canceling-pending-i-o-operations) | 支持场景真实性和文件 I/O 风险分析；不证明特定现场路径的时效、取消边界或速度 |

## 13. 后续验证顺序与停止条件

本轮没有执行以下实验；它们是建议的后续验收，而不是既成结果。

1. **先冻结正式设计：** 明确模式范围、专用路径所有权/信任、终态恢复生命周期、快照边界、超时隔离及 prior 优先级；更新可选控制面的架构声明。技术选择由实现设计论证，不要求用户选择底层 API。
2. **映射能力小探针：** 经后续授权，在 B 仅写程序所有的小状态文件，A 仅只读；分别验证发布/内容更新/重开/替换/缓存/断连/重连/停止。测序号可见延迟和读操作耗时，不改全局缓存或映射设置。先证明通道，不传 payload。
3. **解析/状态模型：** 重复/回退/同序冲突、截断/超大/溢出/重复键、错完整 SessionId/相同 Tag、错 Profile/Manifest、0/1/65,536 段位图、随机 epoch、乱序/丢快照、并发替换、非程序所有文件和目录置换。
4. **真实 FEC/Receiver/存储回归：** 0/1/6/12/13 段及多窗口、RAW/压缩/DirectRepeat、不同损失/突发/周期采样、早期洞/尾洞/晚加入、资源满/磁盘/flush失败、恢复重验、双方分别重启。模拟“理想 K 到齐”不能代替 Wirehair 和最终文件。
5. **自动停止专项：** 残留 Completed、当前 Session 上次 Start、未用 lease/next帧号、旧 epoch、旧文件线程跨 Stop/Start、最终摘要耗时、最终反馈丢失/不可写、B 完成后 A 重启、B 重启/换输出。必须证明不会早停，读不到有效终态则继续发送并明确状态。
6. **关闭等价：** 同一输入、时钟与损失模型下，关闭功能的调度轨迹/提交/方程身份与旧分支一致，且反馈 I/O 为零；不是仅“传完小文件”。
7. **实际像素对照：** 固定同候选、文件、Profile、FPS、呈现/捕获、预算、远控设置；分别新接收和恢复。可用 91 MiB/7 MiB 分段覆盖 >12 段且仍小于 100 MB 的筛选约束，再独立测完整大文件。原行为、仅先验、仅反馈分开，不同时改密度/FPS/预算。

主指标：Decoder 完整运行至 final reopen 的总耗时与 verified goodput。另记录尾部、已完成段浪费、最长无 unique/verified 进展、反馈被接受/拒绝/过期、所见视觉锚点年龄、读写成本、资源与安全停止耗时。Encoder 收到终态的时间另外报告，不能把网络反馈延迟算进 Decoder 的文件已完成时刻。跨机 monotonic 不相减。

**不晋级条件：** 任一摘要/冲突/恢复/资源回退；早停；旧缺口饥饿；映射 I/O 拖住退出；关闭仍访问反馈路径；只能在挑选有利模型、放宽原测试或混用旧包下得到收益。坏反馈应使辅助能力失效或只降低效率，不能使未校验文件被接受；对于恶意写者的持续 DoS 风险须单独说明。

## 14. 本轮交付与后续边界

本轮完成文档精读、当前源码窄流程交叉核对、在线资料研究，以及条件性推荐；新增本报告的中英文版并更新索引/现状/证据入口。没有新增反馈功能、没有接触用户实际映射目录、没有修改或打包产品、没有本地/现场提速成绩、没有关闭旧静态审计和历史失败，也没有创建 Git 提交。

最终建议：**继续推进显式可选的分段反馈辅助设计。** 它比仅凭速度猜位置更有针对性；工程重点不是“写一个状态文件”，而是耐久完成语义、只读映射的不可靠时序、有限调度、公平回退，以及用户已要求的跨世代安全自动停止。先把这些边界定义并验证，再考虑更复杂的速率自适应或方程级反馈。
