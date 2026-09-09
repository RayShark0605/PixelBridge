# 非本机效率：Receiver 接纳原因诊断与原始对照

> **状态（2026-09-09 记账）：** 本文属 G22 之后非本机吞吐优化线在 2026-09-08/09 的记录或合同文本，正文未改写。该目标已于 2026-09-09 由用户主动停止（未完成、非技术阻塞）；文中「目标进行中 / 下一步 / 待执行」等表述仅属当时时点，不构成继续执行或现场操作的授权。当前状态见 [暂停交接](REMOTE_THROUGHPUT_RESUME_HANDOFF_2026-09-09.md) 与 [文档状态矩阵](DOCUMENT_STATUS_2026-09-09.md)。

## 0. 状态与目标

本项用户批准实施“最小接纳原因诊断 + 原始诊断开/关对照”。**本地诊断目标完成，不是非本机提速验收。** 主线仍是更快完成非本机整文件摘要、安全发布和重开，而不是提高配置 FPS、局部接受数或本机处理速度。

此项是 Step2 损失归因的窄补充。Step3 固定 codec 仍未恢复，整个 Step3 仍 PARTIAL，现场 NOT_RUN，Step4–Step10 未启动。此前固定 codec/几何失败全部保留，没有修改几何门或再次编码。

## 1. 已确认的历史损失

只读 R1 逐帧 trace、恢复日志中的原控制记录和对应源码，得到：

| 阶段 | 不同的已解码画面 | 控制记录 | 不同 Transport 字节摘要 | Receiver 利用 |
| --- | ---: | --- | ---: | --- |
| 首次接纳 Session 前 | 119 | 119 个 SegmentDescriptor slots，没有已接纳的 SessionDescriptor | 1,666 | 未进入数据接纳路径 |
| 首次接纳 Session 起 | 115 | 6 Session、6 Manifest、115 Segment slots | 1,598 | 与原新方程计数 1,598 一致 |

首次接纳 Session：observation 2230，FrameSequence 70542，PTS 37150，time base 1/1000。Session 记录 115 B、Manifest 114 B、5 个 Segment 记录各 194 B，其 BLAKE3 与逐帧记录精确对应。恢复日志头和全部记录 CRC、长度和 generation 单调性均通过元数据检查；这不是启动生产 resume 恢复。

源码解释：原 Unified 路径按 Session → 其他 Control → Transport 三遍处理；未建立 Session 时不调用 Transport ingress；非 Session 控制的 UnknownSession 返回可重试未接纳。单帧缓存、冲突拒绝和 resource policy 不变。

**边界：**1,666 是不同 Transport 序列化字节摘要，不直接等于独立可恢复方程或 verified bytes；37.150 是录像位置，不是可承诺节省时间。仅靠 Receiver 记录无法区分此前关键控制在 Sender、链路、几何准入哪一层损失；被拒帧的数据区未解调，不能宣称整段数据无错。

另确认旧 `receiverDataDisposition` 在 Unified 汇总路径未传播实际逐块结果，全部 3,622 行均为默认值 1；新增明确权威标记，保留旧记录不改写。

## 2. 改动与非干预合同

既有代码仅修改：

- `apps/common/local_desktop_runtime.cpp`：输出型诊断指针、状态快照及实际分支/返回值记录；不修改 PBReceiver、几何/FEC、调度、资源常量或发布条件。
- `apps/common/recorded_pixel_replay.h/.inc`：离线诊断开关默认关闭；追加可空诊断；异常记录后仍原样失败退出。
- 新增 `apps/common/receiver_decision_trace.h` 与独立 `tools/PBReceiverDecisionProbe`。

固定 15 slots，无 payload、无新缓存/队列、无反向通信。诊断字段只被输出代码读取，不决定任何接纳。`receiverCalled` / `receiverReturned` 不等于成功；Transport 的可空 disposition/Outer classification 才说明实际成功结果，CachedOrphan 不冒充已绑定方程或 goodput。完整字段解释见工具 README。

GUI/CLI 产品未增加可选设置，不修改 Citrix/网络，不操作任何屏幕、焦点或输入。工具只读生产 Sender 先前生成的 BGRA，外部源文件核对发生在子进程结束后，不进入 Decoder 输入。

## 3. 构建、负例与整文件结果

独立构建重编译当前应用库，复用原 Step2 核心 `.lib`；不链接旧 PBApplication。冻结 83 项本地实际编译输入/header、24 项基线库身份以及本轮 EXE/DLL/map。最终构建禁用全局 vcpkg 自动注入，CL read logs 未出现全局 vcpkg headers。不是全项目干净构建或正式产品发布。

### 3.1 定向状态机测试

5 个接纳场景各开/关诊断，共 10 个 admission-only 场景：

1. 缺失 Session：Segment 控制返回 UnknownSession，Transport 未调用，无 Outer decoder/输出文件分配。
2. 同帧控制顺序与两类重复：compact handoff 顺序不影响先绑定 Session；同帧缓存跳过区别于新帧调用 Receiver 后的 IdenticalDuplicate。
3. 同 Session 键、不同有效控制内容：原冲突拒绝，失败帧原因保留，不发布。
4. 原有受限 orphan 路径：首次 CachedOrphan，随后配额拒绝，无虚构 dataDisposition。
5. 几何擦除和 slot 255 非法 handoff：原拒绝，未创建输出状态。

第 6 类验证固定结构 <4 KiB、15 slots 序列化 <12 KiB、null 语义、64 KiB/64 MiB 边界、UINT64 溢出输入及坏输出流。未运行像素、Bootstrap FEC 或 GPU。

过程中的测试辅助 API 拼写编译失败、测试 Bootstrap 缺协议版本、测试几何 scale 默认 0 三项均失败停止，之后依据原合同修正测试夹具。日志保留为 build-01、tests-01、tests-02；未弱化断言或生产校验。最终 tests-03 全部通过。

### 3.2 原始像素 off/on 对照

同一冻结 64 KiB 文件、30 帧 BGRA、1920×1080、15 fps；两个全新 Receiver/输出根。关闭诊断 30 帧，开启诊断 30 帧，**60/60 观察上限已耗尽**。

- 两组 EOF、30/30 Bootstrap、摘要、安全 rename、final reopen、外部逐字节/SHA-256/BLAKE3 全通过。
- 两组与原 Step3-B 原始对照的全部旧非时钟逐帧字段相同，包括每个 accepted payload 摘要、geometry/FEC/Receiver 标志。
- 各恢复 1 Segment，Raw/Encoded 均 65,536 B，50 个新方程，资源/冲突/延后/孤立数据丢弃计数均 0。
- 新诊断：5 帧 Processed、25 帧 AlreadyPublished；15 个 ControlAccepted、50 个实际 Transport 返回，另 10 个 retained slots 在同帧发布后跳过。
- 全部 450 个解调接受 slots 不是 450 个有用方程；恢复完成后其余观察不再贡献 verified bytes。

源文件及两组最终文件 SHA-256：

`40f1859cccb1f6949e8ff0a0c157c97571759a020d7bc3c801bcdc457394276c`

BLAKE3：

`d38ef0993b8f097aee9ad93267387bdef83c49e1f0c54e35be8c87d23974aebf`

每进程/job 2 GiB、监督最长 300 s（比已批 600 s 更紧）。关闭/开启诊断 peak commit 分别为 109,391,872 / 110,153,728 B。处理耗时只作资源记录，不比较成速度收益。

## 4. 后续只保留一个候选方向，尚未实施

**候选：必要周期控制信息的时间分散。** 优先评估把活跃分段调度器的周期 Session/Manifest 重复错开，而不是增加总体重复量或建立反向 ACK。

下一项设计应先核对当前 Sender 调度器的实际相位与提交规则；若确有集中发送，候选只改变周期控制的相位分布，保持原初始化控制、固定周期/总控制预算、普通帧 SegmentDescriptor、调制/FEC/资源与安全门。比较迟加入后控制就绪、可用方程、控制占用和最终整文件完成时间；不能只选择首次接纳更早的一项结果。

这是基于当前损失的**可检验假设**，不是已证明集中发送导致本次等待，也不是已证明分散后提速。若源码/有界调度证据不支持它，记录未晋级，不自动轮换参数矩阵。Step3 的 codec/几何阻断仍保留，不能用 admission-only 成功冒充完整离线信道就绪。

任何发送节奏实现、额外像素预算或 Step4 推进需下一项范围确认；一般技术细节由执行者判断。非本机实屏验证另批，先核对右屏拓扑；不干扰左屏或输入。

## 5. 证据与复验

证据根：`<repo>\artifacts\receiver-decision-diagnostics-20260908-run01`。

- `context/`：原 1,180 项源码身份、既有 diff、改前文件、输入身份、范围预算及本次 runtime 增量。
- `identity/`、`runtime/`：实际编译输入、依赖、EXE/DLL/map。
- `logs/build-01..04*`、`logs/tests-01..03*`：保留全部失败/修正/最终成功记录。
- `HISTORICAL_ATTRIBUTION.json`：历史控制摘要关联及计数，不增加回放。
- `runs/off`、`runs/on`、`PARITY_ANALYSIS.json`：本轮 60 帧与整文件真值。
- `SOURCE_IDENTITY_FINAL.json`、`FINAL_STATUS.json`、`FINAL_MANIFEST.json`、`RECEIVER_DECISION_EVIDENCE.zip`：本次增量及封存。

工具 README 提供只读验证命令。证据含 ZIP 总预算 128 MiB，不复制历史视频/raw/构建。旧封存源码集合将因本次批准的源码改动而不同；使用本次差异清单解释，不修改旧 checker，也不将旧完整工作区 seal 宣称仍与当前完全相同。

保护文件 `<repo>\docs\PHASE1_GATE_REPORT.md` SHA-256 保持 `076ef4c9b9f89eabccd323dbe4bffc4dc125ddaf96e6ee437d2cf5b1b1cea306`。未暂存、提交、推送、重写 Git 历史或覆盖历史交付；没有新 codec、完整回归、压力、实屏或后台任务。
