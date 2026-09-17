# 自动日志与性能/异常诊断

**简体中文** | [English](DIAGNOSTICS.en.md)

## 1. 三类文件不可混淆

| 文件 | 用途 | 是否参与文件恢复 |
| --- | --- | --- |
| `run-<id>.events.jsonl` | 默认应用诊断，逐行 JSON 时间序列 | 否 |
| `run-<id>.summary.json` / 手动导出 RunReport | 最终状态、环境、资源、校验和警告 | 否 |
| `.resume` / `.part` | 可验证接收断点与未发布文件 | 是；不能当可删除日志 |

自动日志位置：`%LOCALAPPDATA%\PixelBridge\Logs\Encoder`、`...\Decoder`。普通 GUI 和正常传输 CLI 默认开启；单独的研究工具/参数解析失败不保证进入相同日志链。GUI smoke 使用 Qt test-mode 目录，不清理真实用户的运行日志。

GUI 从高级选项打开目录；CLI 在 stderr 输出结束时日志位置/异常。显式 `--journal` / `--report`（实际选项以 `--help` 为准）保留旧 create-only 取证语义，自动日志不会删除它们。

## 2. 记录范围与开销边界

- 通常每 2 秒一次，状态变化额外记录；在构造/序列化详细记录前节流。
- 应用轮询层直接写入，不增加每帧日志、不在捕获/GPU/FEC 热路径 flush，也不增加日志队列。
- 使用本机单调 `elapsedMs` 和 UTC `unixMs`，包括版本、Git commit、run/session/profile、状态、错误、阶段累计数和进程 private/working-set 内存。
- 单次时间序列最多 64 MiB / 48 小时；达到上限后标明截断，仍尝试保存最终报告。
- 每角色启动时保留最近七个历史日志加当前一个。只识别应用生成的精确文件名，不递归删除，不跟随链接，不删除活跃写入者或显式取证路径；无法清理时提示，故并发/权限异常下不是全局磁盘硬上限。
- 最终报告最多 1 MiB。日志创建/写入/flush 失败不改变传输成功条件；日志失败状态与文件失败分开。
- 只有正常运行生命周期才保证尝试最终报告。断电/强杀可能留下最后一行截断或没有 summary；不要将其当作已完成。
- 采样可能错过短于两秒的活动阶段；不能把“没有采样到”解释为“没有发生”。累计计数/最终报告补充时间序列。

## 3. 两端分别看什么

### Encoder

`preparedSourceBytes` / `preparationMilliseconds` 区分预扫描与正式显示。`configuredLogicalVisualFps` 是设置；`generatedVisualFps`、`submittedLogicalFrames`、`successfulPresentCalls` 说明生成/呈现。`replacedPendingFrames`、`repeatedPresentCalls`、`pendingFrames` / high-water 说明被替换、重复显示和积压。

`segmentCount`、`currentSegmentOrdinal`、`cycleCount`、`currentOuterBlockId`、`frameSequence` 和 airtime/visit 参数帮助识别轮播/尾部成本。静态边缘尺寸和实际码面记录用于比较不同呈现合同。

### Decoder

- **捕获层**：arrived/delivered/drop/expired/stale、capture epoch、后端原因、队列/lease high-water、capture/visual stall。
- **识别层**：Bootstrap 尝试/成功、定位尺度/偏差、FEC/CRC 失败、重复/缺口身份。Sequence gap 也可能包含 durable lease 跳号，不直接等于网络丢包。
- **PAM4**：独立 `pam4*` 观察/擦除/slot 接纳计数及 CPU readback/decode 的 wall cost。不是 Unified lane 数据；观察数可能含重复捕获。
- **恢复层**：`outerUniqueSymbols`、identical duplicates、already-completed、ready events、verified segments/bytes。
- **资源层**：`outerDeferredResourceBusyCount`、quota/OOM/rejections、active/peak decoders、reserved/total/per-instance budget、resume state 大小和 generation。
- **最终层**：整文件 digest、publish、final reopen、errorDetail。文件正确恢复与清理无警告是不同维度。

## 4. 怎样定位一次“卡住”

1. 确认两份日志匹配同一个 `sessionIdHex` 与 Profile，而非只看文件名。run ID 是单端运行标识。
2. 在 Decoder 找 `noSizeGrowthMs` 增长区间和 `activity`；同时对比该区间前后累计计数的差值。
3. 有新 unique symbols、无大小增长：段内 repair / 估算平台，通常不是捕获停止。
4. 重复/已完成段增长，无新 unique symbols：轮播尾部或重复观察；如果持续超过观察阈值，不保证正常。
5. deferred/quota 增长、active/预算接近限制：准入瓶颈；只有这类证据支持尝试增大预算。
6. 捕获仍来但 Bootstrap/FEC 可用量很低：先检查画面质量/几何/遮挡，不把它归因于 RAM。
7. Encoder 生成新帧而 Decoder 长时间无新身份：问题可能在远控视频链、捕获或识别。仅凭两份日志无法直接区分远控黑盒内部所有原因，需要额外受控证据。
8. 已标记实际写盘/校验操作：等待完成，关联 CPU、内存与持久化计数；不能仅凭 `DecoderState::Verifying` 推断仍在写盘。
9. 最后读 summary：最终完整校验和 cleanup warning 不能被中途成功的块/画面掩盖。

## 5. 使用与解释限制

`PixelBridge.RunJournal.1` 的既有字段保留，新字段为追加；自动诊断额外标 `logKind=OperationalDiagnostic`、`elapsedMs`、`terminal`、`activity` 和停滞时长。它不是正式现场 Gate，也不是逐帧重放语料。

自动 summary 使用原 RunReport，并添加独立 `operationalLog` 状态，不把自动采样冒充正式 measurement。终态报告在工作线程收尾后封存，以保留 post-publish 警告。

不能跨机器直接相减 monotonic 时间；按 Session / frame identity 关联，或用各端自己的 elapsed 区间。主成绩仍是 Decoder 最终重开通过的完整文件耗时，不是 JSONL 行数、显示 FPS 或估算接收速度。

不记录 payload/截图，但报告可能含文件名、路径、Session 与机器/显示信息。不要公开原始日志前不检查隐私内容；日志不是签名审计轨迹，也不替代可信发送者认证。
