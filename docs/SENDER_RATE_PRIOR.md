# PAM4 预期接收速度先验

**简体中文** | [English](SENDER_RATE_PRIOR.en.md)

日期：2026-10-21。**状态：用户批准的可选实现；本地构建/回归与真实链路效果分开验收。** 这不是新的协议、FEC 或接收确认机制。项目主阶段仍为 v1.0 稳定性、显示兼容性与交付维护，旧静态审计和现场限制没有因本功能关闭。

## 1. 使用与回退

- Encoder 主窗口和右键发送对话框均有“预期接收速度先验（仅 PAM4）”。新安装默认不勾选。
- 选 PAM4，勾选后填写历史 Decoder 平均接收速度的上下限，例如 `200～280 KB/s`；`1 KB = 1024 B`。这是原始文件字节平均，不是视频带宽、FEC 字节速度或限速器。
- 两端仍须选择相同的 PAM4；**PAM4 Wide、标准、灰阶高速首版不支持**。切换其它模式会禁用这些控件且不应用先验，保留 PAM4 设置，切回后可复用。
- 仅在 **超过 12 段**的既有 graduation 路径生效；不改变小文件 whole-window barrier。默认 7 MiB 分段且未覆盖分段大小时，原始文件超过 84 MiB 才超过 12 段，压缩率不改变分段个数。
- 合法区间为整数 `1..1048576 KB/s`，下限不能大于上限。上下限可以相等。无效输入拒绝开始，不交换、夹紧或暗中代入其它速度；无效的已保存设置会明确提示并关闭先验。
- 偏好在主/Shell 入口共用的设置中保存，准备/发送/停止收尾期间锁定。停止后取消勾选即可恢复原调度；未新增 CLI 开关。
- 不会因预测到达文件末尾而自动停止。仍以 Decoder 完成、摘要校验和最终重开为准，再人工停止 Encoder；既有显式超时设置仍独立生效。

## 2. 为什么这样实现

用户批准直接实现、后续自行做真实传输验证，并将首版范围确定为仅 PAM4。本版沿用[研究结论](SENDER_RATE_PRIOR_RESEARCH.md)中的弱先验路线，但不引入任意跳段、额外活跃 codec、跨段编码或 ACK。

前沿带是 `[rMin × t, rMax × t]`，按原始字节计算并裁到文件长度。通过已验证的 descriptor 的 `rawOffset/rawSize` 判定段与带的关系，按 encoded 工作量 `K` 计服务成本。带可以超过活跃窗口宽度，但不会把带内全部分段加载进内存。

**不把前沿之前的段标成已收到，也不跳过它们。** 分段顺序、最多 6 个活跃发送段、轮转与控制帧合同不变；通过访问预算，让旧段较快交还发送机会给后续段。窗口外的段不会因为预测而被越过。这是预算型软对齐，不是强制发送窗口精确追踪一个字节游标。

## 3. 可复核的预算规则

预算为本次入窗的新增发送方程目标，以下乘除均向上取整且至少为 16；不是接收端需求量或接收成功保证。

| 段的位置 | 本次运行尚无已提交服务的首访 | 后续全局回访 |
| --- | --- | --- |
| 整段在预测下界之前 | `5K/4` | `K/16` |
| 与预测带相交/接触 | `7K/4` | `K/2` |
| 完全在预测上界之后 | `2K` | `K/8` |
| 确定性全文件保底修复轮 | `2K` | `5K/4` |

- 相对本次 Start 的全局轮次为 0、1、2、…；**3、7、11、… 轮为保底修复轮**。所有段仍被遍历，不由接收进展选择。预测下界到达 EOF 后，后续访问均使用保底修复规则，而非永远停在尾段。
- 发送保证是：每段每轮都有非零访问预算，至多经过 3 个普通完整轮后进入一个足量的保底修复轮。不是固定秒数上界，也不保证任意丢帧下都能恢复；源读取、实际提交速率及链路损失仍影响等待。
- 入窗时将目标锚定到该段本次运行的真实提交账本，而非 repair-ID lease。每个 scheduler pass 完成后可按移动前沿下调当前访问目标，但不会向上追逐目标而无限驻留。
- scheduler pass 必须完整提交：首个 systematic+repair pass 不会被截断；后续 repair override 至少 16，物理尾槽仍可 padding duplicate。因此实际方程数可能高于表中目标。原有每次入窗最多 8 个 pass 的边界保留。
- DirectRepeat 保留固定 `[0,K)`，不能产生虚构 repair ID。每次访问至少完成其整 pass；这类小段不一定能精确达到上述比例。
- **关闭先验时不使用这张表。** 保留原来的大文件 `2K + 每轮约 25%K` 累计目标、原始小文件路径，以及其它模式的所有策略。

这些系数是保守且可隔离的首版启发式，不是由 200～280 KB/s 推算出的丢包率，也不是已经测得的最优系数。错误先验可能让实际需要的段更久才获得足量修复，导致变慢；应保留关闭对照。

## 4. 模型时钟与恢复

- 从本次第一个**成功提交的逻辑码面**开始计时，排除预扫描、codec 准备及此前等待。只构造/重试 pending frame 不推进模型，也不记方程服务。
- 使用成功提交时间的单调间隔；单次本端提交空隙最多记 1 秒。大于 1 秒的本端暂停不会按全部墙钟时长快进预测，但仍计入该 1 秒；不足 1 秒的等待不被剔除。该时钟不是 Decoder 活动计时，无法观察链路远端冻结。
- 停止/重启后模型从零、服务账本从零保守重建。持久化 repair high-water 仍独立防重用；既往未用租约不成为服务功劳。不会拿上一次运行的预测位置跳过早段。
- 先验不进入 Session/descriptor 或源准备身份，可在停止后为同一兼容 Session 改设置；不改变该段的精确 encoded bytes、descriptor、ID 到方程映射或 Decoder 恢复状态格式。

## 5. 日志和诊断边界

Encoder 的 `events.jsonl` 及导出/自动生成的运行报告新增 `senderRatePrior`：

- `requested`：输入的 `minimumKibPerSecond/maximumKibPerSecond`，关闭为 `null`；
- `effective` / `policy`：当前是否实际启用，未启用为 `original`；准备完成前或不超过 12 段时即使配置存在也为 false；
- `activeNanoseconds`、`clampedSenderGaps`：模型时间和本端长间隔裁剪计数；
- `predictedLowerRawBytes/predictedUpperRawBytes`：预测带，**不是 receiver progress**；
- `beforeFrontierEquations/nearFrontierEquations/aheadOfFrontierEquations/safetyRepairEquations`：成功提交时按当时预测关系分类的 scheduled equations；不包含控制槽、padding duplicates、失败 Submit 或重复 Present，不表示这些方程实际被 Decoder 接收。

`receiverAcknowledged` 始终 false，Encoder 的真实 `receiverProgress/receiverEta/verifiedGoodput` 仍为空。判断实际效果应看同一任务最终完成耗时、Decoder verified bytes、WholeFileDigest、安全发布和重开，而非仅看预测带或提示减少。

## 6. 实现与验证入口

- [纯策略与时钟](../apps/common/sender_rate_prior.h)、[实现](../apps/common/sender_rate_prior.cpp)；
- [生产发送器接入](../apps/common/local_desktop_runtime.cpp)；[两入口共用控件/设置](../apps/PixelBridgeEncoder/encoder_rate_prior_options.h)；
- [策略边界测试](../tests/PBApplication/test_sender_rate_prior.cpp)、[真实 FEC/接收/存储的无窗口恢复测试](../tests/PBApplication/test_pam4_rate_prior_runtime.cpp)、[离屏控件测试](../tests/PBApplication/test_encoder_rate_prior_options.cpp)、[JSON 报告测试](../tests/PBApplication/test_rate_prior_reporting.cpp)。

本地材料目录为 `artifacts/pam4-rate-prior-implementation-20261021/`；起点备份/哈希、RED/后续构建测试日志与任务差异分开保留。最终执行结果见[项目现状](PROJECT_STATUS.md)和[证据索引](EVIDENCE_INDEX.md)。没有真实远控传输、现场提速、长稳或无停滞验收，也没有以先验确定截图当次停滞根因。

后续具备条件时，先按平常任务尝试即可，无需现在补实验。若比较开关效果，使用**同一个候选构建**、相同文件/模式/帧率/链路，并为每次从头接收使用独立的新 Encoder 会话缓存与新 Decoder 输出目录，避免把恢复状态和首轮 systematic/repair 差异混为速度先验效果；仍然先开 Decoder。保留两端日志及最终完成结果。当前候选含本轮起点已有的 Qt/静态审计修改，不把旧正式包与新工作树的全部差异归功于先验。

## 可选文件反馈关系（2026-10-21）

[文件反馈辅助传输](FEEDBACK_ASSISTED_TRANSFER.md)支持全部四模式，默认关闭，不改变本合同的 wire/Profile/FEC/采样/摘要与发布规则。真实完成集合优先用于后续服务选择，速度先验仍只是未完成段的可选本端预算；失效后恢复原覆盖/先验。不要把反馈或先验字段混成 Sender 独立验证的接收进度，也不要把本地回归当真实远控提速成绩。
