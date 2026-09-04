# G17 — Unified 遥测与报告合同

**状态（2026-09-04）：G17 实现与指定最小验证已完成。**

第 6 节的 Bootstrap-only / 首次完整数据接收缓存身份问题曾因超出只读观测范围而停止实施；用户明确批准后已完成最小修复与定向回归，原失败场景及 8 秒 deadline 保留。用户要求本次任务在 G17 完成并独立提交后结束，不继续 G18。

## 1. 前置与批准范围

已核对以下提交为当前基线 `c7331e1ba1c5d64b35dffdca22473a95ba0ad38e` 的祖先或基线自身，并读取既有证据；没有重跑这些前置目标：

| 前置 | 提交 | 既有证据 |
|---|---|---|
| G12 | `3044f2330655f157ec40ebcd0007c2ec2b5902b8` | `build-unified-release/tests/PBDemodD3D11/g12-unified-publish-metrics.actual.jsonl`；Base 20,164.923、clean 37,449.143 encoded bytes / unique frame，whole digest、publish、final reopen 均成功 |
| G15 | `c5dc9da4963a362444fd46046170a459654acb25` | `build-unified-release/tests/PBApplication/g15-workflow-reviewed.txt`：11 cases / 563 assertions |
| G16 | `c7331e1ba1c5d64b35dffdca22473a95ba0ad38e` | `build-unified-release/tests/PBApplication/g16-workflow-final.txt`：5 cases / 170 assertions |

用户已同意的 G17 三项边界：

1. 除 PBTelemetry、run_report、状态适配与报告测试外，允许扩展既有 application model/runtime、Unified 解调观察值及 PBStorage 的**只读观测接口**。不改变解码决策、shader、wire/FEC、发布顺序或持久化格式。
2. Unified 产品报告使用 `PixelBridge.RunReport.3`；历史诊断 Profile 保持 `.2`，不批量迁移历史验收脚本或 Golden。
3. resumed run 缺少文件完整生命周期的帧覆盖证据时，`VerifiedEncodedBytesPerUniqueFrame` 为 `null`，带明确 unavailable reason；不猜测历史帧数。

后续明确批准的例外仅为第 6 节的单帧缓存身份推进：已通过原有身份验证的较新帧首次有可用数据时，可以建立对应缓存；不把普通同帧 duplicate 当成新缓存，不允许旧帧回退，也不绕过同帧冲突及边界检查。

核心库不依赖 Qt。只有 application report tests 使用 Qt Core 做实际 JSON 解析，并按既有 `windeployqt` 方式部署测试所需 Qt Core / ICU 运行库。

## 2. 实际观测与计数

实现入口：

- `libs/PBModulation/include/pbmodulation/unified_visual.h`、`src/unified_visual.cpp`：共享 FEC gate 前的只读 metric 汇总。
- `libs/PBTelemetry/include/pbtelemetry/unified_telemetry.h`、`src/unified_telemetry.cpp`：有界、单 owner 的 run / Session 统计。
- `apps/common/application_model.h`、`local_desktop_runtime.cpp`：生产状态来源与既有 GUI 快照接线。

| 字段 | 来源、分子 / 分母及 unavailable 规则 |
|---|---|
| `lanes[].metrics` | Base/Fine/Chroma 实际量化并局部擦除后的 soft metrics；记录 samples、zero、erased、absoluteSum、minimumAbsolute。不是 GPU 原始浮点值，也不是 BER。没有 metric 观察时整个对象为 `null`。 |
| `lanes[].fec` | 实际逐 slot gate 结果：evaluatedSlots、attempts、failures、crcFailures、identityFailures、accepted、erased、iterations。重复 capture 导致的实际再次评估也计数；这些不是唯一帧分母。没有 slot 评估时对象为 `null`，已测得的零失败保留为 0。 |
| `classifiedControlSlots` / `acceptedControlSlots` | 经过 FEC 且识别为 Control 的 slot / 其中通过 framing、CRC、identity 的 slot。FEC 失败或无法确认的 Transport information 记入 `unclassifiedSlots`，不猜测其为有效载荷。 |
| `observations` / `frameErasedObservations` | 已绑定 Session 的有效 Bootstrap 对应观察次数 / 没有可用整帧数据解调结果的次数。Bootstrap-only 仍可识别实际看到的逻辑帧，但不能合成 lane/FEC 计数。 |
| `uniqueLogicalFrames` / `duplicateObservations` | 正式或 pending Session 绑定后的实际 `(SessionTag, FrameSequence)`；首个建立 Session 的完整帧在其他 Control/Transport 导致发布前计入。重复 capture 不增加唯一帧数，capture epoch / backend 变化不重置该身份。 |
| `uniqueVisualFps` | `(实际唯一帧数 - 1) / (末次唯一帧时间 - 首次唯一帧时间)`，时间为 capture monotonic 100 ns。少于 2 帧、时间无跨度或覆盖不可靠时为 `null`。FrameSequence 跳号只计实际看到的新帧，不外推被跳过的帧数。 |

去重历史为固定 4,096 项，不随文件或 Session 帧总数增长。无法区分“新出现的旧帧”和“已逐出的重复帧”时，不估算新帧数，而是撤回完整覆盖声明与派生性能。乱序、时间倒退、计数溢出、非法 metric 尺寸或 slot 统计冲突同样撤回相关证据。

统计先验证后原子提交，失败不留下部分 lane 计数。上述新遥测的失败只影响证据健康度，不能替代 Receiver 的 payload admission；实际回归已证明破坏只读 metric 统计仍能正确发布文件，而每帧性能变为 unavailable。

## 3. 调度、恢复与发布证据

### Encoder

- preparation 使用真实 source prescan 的 verified bytes、Segment 数、耗时和 source stability；尚未观测到的耗时/速率为 `null`。
- resumed preparation 时间覆盖 source prescan 与已有持久化描述符核验，不声称仅为某一个低层磁盘操作耗时。
- Carousel pass / ordinal、state generation、FrameSequence / repair lease 取既有 scheduler/store 快照；尚未取得的 lease 为 `null`。
- `submittedControlSlots` 取完整 raster 的实际调度计划，且只在 `SubmitFrame` 成功后累加。
- `controlSlotOccupancy = submittedControlSlots / (31 * submittedLogicalFrames)`；dwell 中重复 Present 不增加计数。未提交任何完整 raster 或计数溢出时为 `null`。
- `configuredLogicalFps` 与 `observedSubmittedLogicalFps` 分开。它们都不是 Decoder 的 `UniqueVisualFPS` 或端到端 goodput。
- `receiverProgress`、`receiverEta`、`verifiedGoodput` 始终为 `null`。

### Decoder / PBStorage

- verified raw / encoded bytes 和 Segment 数取 Receiver、恢复核验及成功落盘状态，不取截图面积或 FEC 中间输出估算。
- resume 时间覆盖当前初始化中的 journal/storage open 和既有 completed-Segment / pending-payload 核验。只有到达可观测的恢复分支后才给出耗时与成功/失败；更早的打开失败不伪造完成的测量。
- 进度 goodput 明确标为当前 run 的 verified raw-byte progress，可能包括恢复时的磁盘重新核验，**不是整文件信道性能**。
- `wholeDigestVerified`、`renameSucceeded`、`finalReopenVerified` 独立记录。`null` 是当前对象未观测到该阶段；`false` 是该阶段尝试失败；不会将尚未执行的阶段填成成功或数值 0。
- rename 后恢复路径能重新打开并验证最终文件，但不能反推本进程实际执行过前次 rename，因此 `renameSucceeded` 保持 `null`。
- 只读状态赋值沿用原有 hash、非覆盖 rename、reopen 和失败回滚顺序，不改变 PBStorage 持久化合同。

## 4. 报告兼容性与最终每帧指标

Unified Encoder / Decoder 导出 `PixelBridge.RunReport.3`，Decoder 内嵌 `PixelBridge.UnifiedTelemetry.1`。历史显式诊断 Profile 的 `.2` 分支保留；现有 journal schema 不在本次迁移范围。JSON 使用 classic locale、正确转义及 finite 检查；64-bit 身份/计数按整数序列化，消费者不能先转成低精度浮点再比较身份。

`verifiedEncodedBytesPerUniqueFrame`：

- numerator：当前文件已验证并落盘的 encoded Segment bytes；
- denominator：本 run 已绑定 Session 的实际 observed unique logical frames；
- gate：`WholeFileDigest + safe publish + final reopen + complete current-run frame coverage`；
- resumed run：`null` / `ResumeHasNoLifetimeFrameCoverage`；
- 未发布或 digest/reopen 未成功：`null` / `NotPublished`；
- 帧覆盖不完整：`null` / `IncompleteObservedFrameCoverage`；
- fresh published run 缺少 encoded-byte 证据：`null` / `EncodedByteCoverageUnavailable`；
- 0-byte 文件在全部 gate 满足时允许实测值 0，这不同于 unavailable。

没有独立 sender truth 时，`preFecBerEstimate` 为 `null` / `No independent sender truth`。provider、mode、version 等只出现在 `NonDecodingOperatorMetadata` 中；测试改变这些字段后逐项比较实际 Unified demodulator 配置、locator/metric/FEC 阈值、输出 bytes/digest 和 lane 遥测，不能通过 provider 标签选择解码参数。

## 5. 已运行的最小验证与最终结果

以下命令在仓库根目录执行，均为 Release；未运行真实窗口、ROI selector 或 capture。测试只替换 presentation / capture / GPU 边界，payload 由实际 Encoder raster 经 CPU oracle 进入既有 Decoder runtime / Receiver / PBStorage。

```powershell
cmake --build build-unified-release --config Release --target PBTelemetryTests PBApplicationTests PixelBridgeEncoder PixelBridgeDecoder --parallel 6
.\build-unified-release\tests\PBTelemetry\Release\PBTelemetryTests.exe --rng-seed 17092026 --durations yes
.\build-unified-release\tests\PBApplication\Release\PBApplicationTests.exe '[application][report]' --rng-seed 17092026 --durations yes
.\build-unified-release\tests\PBApplication\Release\PBApplicationTests.exe '[application][g16][model],[application][g16][negative]' --rng-seed 17092026 --durations yes
```

- 构建成功：`build-unified-release/g17-build-cache-fix.txt` 包含两个测试目标及两个 Qt 应用；最终 fixture 修正后的三个受影响目标增量构建为 `g17-build-final-complete.txt`，最后测试 `const` 风格复核构建为 `g17-build-final-review.txt`。Qt 部署仍提示 `VCINSTALLDIR` 未设置，不能据此声明安装包已验证。
- PBTelemetry：12 cases / 4,381 assertions 通过；`build-unified-release/tests/PBTelemetry/g17-telemetry-reviewed.txt`。
- application report：11 cases / 478 assertions 通过；`build-unified-release/tests/PBApplication/g17-report-complete.txt`。包含 Unified/legacy schema、实际像素、provider 参数隔离、同 run fallback/Bootstrap-only 后最终发布、Stop/resume、遥测异常不改变 payload admission、发布各阶段与冲突边界。
- 受影响的 G16 model / negative 窄兼容检查：2 cases / 55 assertions 通过；`g17-cache-compatibility.txt`。包括 accepted 数量越界、重复 slot、foreign profile、同帧有效 payload 冲突和新帧同 Outer identity 冲突。新增 observation 大小下，3840×2160 demod reservation 为 **216,641,048 bytes**，小于 256 MiB cap（268,435,456 bytes）。
- 最后仅修改测试局部只读 span 为 `const`，重建测试目标并只复核受影响的缓存身份 test case：1 case / 29 assertions 通过；`g17-cache-final-review.txt`。没有因此重跑其它模块。

收尾时核对了实际日志分支和导出的 JSON，而不只读取绿色总数：

- 0-byte 首帧实际为 8 个 Control，20,000-byte RAW/Wirehair 为 12；测试原先固定 12 的假设已修正，并与实际 pixel 解调得到的 Control 数量交叉检查。
- 移除了 test SECTION 内提前返回造成的后续 SECTION 未发现问题；最终日志明确同时包含同 run 发布和 Stop/resume 两条分支，未删除或弱化完成断言。
- pixel collector 测试 seam 在 Stop 后原先仍报告 `pendingFrame=true`，导致 Encoder 正确报告 bounded-shutdown 失败。已仅修正 seam 的停止状态，并新增 Encoder 必须为 `Stopped` 且无 error 的断言；没有修改生产关闭逻辑。
- 初始失败日志 `g17-report-final.txt` 和 `g17-bootstrap-only-stall.txt` 保留为修复前证据，不作为最终通过证据。

本地 JSON 产物位于 `build-unified-release/tests/PBApplication/`：`g17-encoder.json`、`g17-unified-published.json`、`g17-provider-variant.json`、`g17-fallback-published.json`、`g17-resumed-published.json`，以及保留的修复前失败快照 `g17-bootstrap-only-stall.json`。这些是明确使用 `fixture-commit` context 的测试输出，不是提交后打包二进制身份或真实 provider 认证证据。20,000-byte RAW fixture 最终 numerator=20,000、denominator=1；可压缩双 Segment fixture 为 8,388,609 raw bytes、282 encoded bytes、2 个唯一帧，故当前 run 最终值为 141 encoded bytes/frame，不能把 raw 文件大小当成该指标分子。相同内容恢复发布的 metric 为 `null` / `ResumeHasNoLifetimeFrameCoverage`。

## 6. 已获批并修复：Bootstrap-only 推进身份后，首次完整帧未进入 Receiver

**影响位置：** `apps/common/local_desktop_runtime.cpp`，`ReceiverPipeline::Process` / `ProcessUnifiedFrame`。

**修复前最小复现：** 8 MiB+1-byte 双 Segment fixture，真实 Encoder 生成两个完整 Unified raster。先恢复第一 Segment，使用测试 capture seam 触发 AccessLost→DXGI，再送第一帧重复观察；第二帧先交付 Bootstrap-only `TelemetryOnly`，随后交付该同序列的完整有效结果。修复前观测为 `Receiving`、verifiedSegments=1、observations=4、uniqueFrames=2、duplicates=2、error 为空；8 秒内未完成。

**原因：** 通用 `visualRate_` 已在 Bootstrap-only 时观察到新 FrameSequence；完整结果随后被判为 duplicate。单帧缓存只在无缓存或 `Unique` 时切换，因此仍保留上一帧，并因 FrameSequence 不同提前返回。该条件在基线 `c7331e1` 已存在；G17 最初的只读遥测阶段未修改这个 admission 条件，随后另行获得下述最小修复授权。

```powershell
.\build-unified-release\tests\PBApplication\Release\PBApplicationTests.exe 'G17 fallback duplicates and resumed completion retain honest frame coverage' -c 'same-run completion counts Bootstrap-only erasure without synthetic lane samples' --rng-seed 17092026 --durations yes
```

修复前证据：`build-unified-release/tests/PBApplication/g17-bootstrap-only-stall.txt`、`g17-bootstrap-only-stall.json`；当时 exit code 42。保留原来的完成断言和 8 秒 deadline 后，最终报告组中的同一场景通过，完成两个 Segment 的落盘、whole digest、safe publish 和 final reopen；4 次观察只有 2 个唯一帧，1 次 data erasure 不合成 lane/FEC 样本。

**用户明确批准后的最小修复：** 在原有 `Unique` / 无缓存条件之外，仅当当前 disposition 为 `Duplicate`、SessionTag 与缓存一致且 FrameSequence **严格大于**缓存身份时，允许已通过原校验的可用数据建立对应单帧缓存。普通同帧 duplicate 不清空冲突状态，旧帧仍在原 reordered 分支拒绝。没有增加缓存、跨帧像素组合或改变 wire、FEC、持久化、UI。

新增的 100,000-byte RAW 双帧回归先证明较新帧实际增加 Receiver 的 Outer unique symbols，再交付带有效 transport CRC 的旧帧差异 payload：旧帧被拒绝，symbols 不增加。随后对当前帧交付冲突 payload，明确命中原单帧缓存冲突检查，运行 Failed 且未发布。该 fixture 不依赖 final publish 才发现冲突，原 G16 越界/Profile/Outer identity 负例也通过。

## 7. 未执行与收尾约束

未执行 full CTest、G12 GPU/WARP parity 重跑、GUI smoke、真实 ROI/capture、native/双屏/远控 Gate、进程故障注入、256 MiB / 20 GiB、安装包或提交后嵌入身份复验。此处的像素证据没有经过显示器、视频链路或 GPU capture，不构成实屏或性能认证。

G17 的实现、已批准缓存修复与上述最小验证已完成。`docs/PHASE1_GATE_REPORT.md` 保持未动、未暂存；不 broad-stage，不 amend/rebase/reset/push。最终审查后以显式路径创建一次独立提交，然后结束本次任务；G18 未开始。测试与构建是提交前证据，不声称已完成提交后嵌入身份或安装包复验。
