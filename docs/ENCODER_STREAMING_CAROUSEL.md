# Encoder 流式预扫描、Carousel 与持久状态

> 状态：G02/G09 合同已接入 G15 Unified Encoder 产品 runtime，并通过定向非显示验证（2026-09-04）
> 性质：Encoder 本地实现/持久状态及 Unified scheduler 规范；G15 产品接线与安全删除见 [Unified Encoder 工作流](UNIFIED_ENCODER_WORKFLOW.md)，不是实屏或吞吐认证。
> 主要代码：`apps/common/local_desktop_runtime.cpp`、`apps/common/sender_carousel_scheduler.*`、`apps/common/encoder_session_store.*`、`libs/PBModulation/src/unified_visual.cpp`、`libs/PBProtocol/src/bootstrap_control_codec.cpp`

## 1. 已关闭的发送端合同

1. 源文件通过 `CreateFileW(..., GENERIC_READ, FILE_SHARE_READ, ...)` 打开；现有句柄不共享 write/delete。
2. 文件身份使用 `FILE_ID_INFO` 的 64-bit `VolumeSerialNumber`、128-bit `FileId`，并绑定 size 与 last-write time。
3. 预扫描按固定 8 MiB Segment 顺序读取，同时计算 whole-file BLAKE3、每段 `RawDigest`、精确 encoded bytes、`EncodedDigest` 和 Outer FEC descriptor。zstd 使用 level 3 测试基线；压缩无收益时按规则回退 RAW。
4. 预扫描结束后只保留 Session/Manifest/Segment descriptor/control table，不保留所有 Segment 的 encoded bytes。广播器常驻 current/next 两个 encoded Segment；重新读取和编码的临时工作区仍受单 Segment 上限约束。
5. 广播时先验证重新读取的 `RawDigest`，再重新编码并验证 codec、encoded size 和 `EncodedDigest`；全部一致后才调用 `WirehairV2Encoder::Recreate`。
6. Wirehair 每个 Segment round 精确调度 `K` 个 systematic equations 和 `max(16, ceil(K*20/100))` 个 repair equations。物理尾帧的空余 slot 复用 systematic ID，不增加 repair ID，也不推进 repair high-water。
7. 后续 round 从进程内的精确 repair high-water 继续；崩溃恢复则从已经持久化的 lease endpoint 继续，因此允许跳号但不允许回退或重用。
8. G02 的历史整帧调度器只认识 Control 类型、equation 顺序、每帧 slot 数与逻辑 FPS，不认识像素、lane 或最终 mixed-slot mapping。每个 Segment 开始有一组 Session/Manifest/Segment Control；长 round 以整个 Control burst 的起点计时。当 burst 短于 10 秒时，下一个 burst 起点精确间隔 `logicalFps * 10` 个逻辑帧；若 burst 自身已经占满该预算，则至少插入一个 Data frame 后再开始下一组，避免数据饥饿。G09 的产品 Unified 调度合同如下节所述，不再生成整帧 Control。

DirectRepeat 仍由 `ChooseOuterFecMode` 决定。它只调度固定 `[0,K)`，尾部物理 slot 同样只重复已有 systematic ID，不生成 repair ID。

### 1.1 G09 Unified mixed-slot 调度合同

1. 每个 `PB-Unified-LC4-V1` 逻辑帧固定含 31 个显式 slot：Base Luma 17、Fine Luma 4、Chroma 10。Control 只能占用 Base Luma 的前部连续 slots，最多 16 个，因此任何帧都至少保留 15 个 Transport slots；不存在整帧 Control 分支。
2. 一轮默认 Control burst 按优先级放置 4 份 SessionDescriptor、4 份 FinalManifest、4 份当前 SegmentDescriptor，即 cadence 帧为 12 Control + 19 Transport；长 Segment 从 burst 首帧起每 `logicalFps * 10` ticks 再发一轮。零字节 Session 不存在 SegmentDescriptor，只发送 4 份 Session 与 4 份 Manifest。
3. PB-Control-1 原始记录进入 1,350-byte Robust Inner-FEC information block 时只增加规范零尾部，不修改 Control envelope 或 wire CRC。FEC 成功后，Decoder 依据原有互斥规范前缀自分类：Control 从 `PBCR` 开始，Transport 从固定 BlockType 开始；因此不需要 sender slot plan、ACK 或其他隐藏信道，也没有压缩 1,314-byte Transport payload。
4. `PrepareFrame(logicalTick)` 冻结同一 tick 的完整 31-slot 计划且重复调用逐值一致；它不推进任何 Carousel 状态。调用方只有在整张规范 raster 已成功构建后才能调用 `CommitPreparedFrame()`，该提交才一次性推进帧计数、Control offset 与真正新调度的 equation IDs。Control 与尾帧 systematic duplicates 均不分配新 ID。
5. `SenderLogicalFrameClock` 只接受 1..60 Hz。一次 `Acquire(now)` 最多返回已经到期的最新 tick，并显式计数中间错过的 ticks；旧 tick 直接丢弃，不进入 catch-up queue。Scheduler 自身仅持有一个 prepared frame 和固定 31-slot 数组。
6. 零字节 Session 的剩余 Transport slots 使用显式 inactive disposition，并编码为确定性全零 information word。它们不能形成有效 Transport Block；Session/Manifest 仍走既有 `ControlPlaneReceiver`，未扩大 control record、reassembly 或其他 receiver resource policy。

G09 关闭 scheduler、protocol packing 与 CPU reference-raster 合同；G15 已把它接入 Encoder 生命周期/GUI/默认 CLI。生产路径使用 `PrepareFrameAt(logicalTick, monotonicNanoseconds)`：长 round 的 Control cadence 按 burst 起点约 10 秒调度，不因动态 FPS/丢弃 tick 改变。slot plan 在 pending retry 时保持冻结，只有完整 raster 成功 Submit 后才 commit。历史 tick API 保留用于原 fixture，不能在同一 round 混用两种时间基准。Decoder 产品自动接线和实屏/capture 仍由后续目标完成。

## 2. 恢复身份

恢复旧 Session 前必须同时满足：

| 身份 | 当前值或来源 |
| --- | --- |
| source object | 64-bit volume serial + 128-bit file ID |
| source snapshot | file size + last-write time |
| source content | whole-file/Segment raw digest 与 immutable descriptor bundle |
| build | `<product>-<version>;sender-streaming-carousel=1` |
| compression | zstd runtime version、enabled、level、window log、max output、framing margin、content-size flag、checksum、workers |
| Outer FEC | Wirehair `2.0.0`、revision `067ca7cdb66aed424ec23f97557429bf791c6f0c`、canonical profile ID、OuterBlockBytes |

同一文件对象仅 size 或 last-write 改变时返回“不匹配”，由调用方创建新 Session；这不是持久状态损坏。若 size/time 被外部恢复但内容发生变化，完整预扫描得到的 descriptor bundle 仍会不同，也会创建新 Session。source index 指向的 SessionId、source object identity、CRC 或 descriptor/runtime digest 自相矛盾时则 fail closed。

## 3. `descriptors.bin` schema v2

所有整数均为显式 little-endian。最终 4-byte CRC32C 覆盖它之前的整个文件。字符串必须是有效 UTF-8，每个 identity 字段最多 4,096 bytes；文件总长最多 32 MiB。

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 4 | magic `PBED` |
| 4 | 2 | store version = 2 |
| 6 | 2 | reserved = 0 |
| 8 | 8 | total file bytes，包含末尾 CRC |
| 16 | 16 | SessionId |
| 32 | 8 | volume serial number |
| 40 | 16 | 128-bit file ID，按 Win32 `FILE_ID_128.Identifier` 原始顺序 |
| 56 | 8 | source file bytes |
| 64 | 8 | last-write FILETIME value |
| 72 | 8 | SegmentCount |
| 80 | 4 | source path UTF-8 bytes |
| 84 | 4 | build identity bytes |
| 88 | 4 | compression identity bytes |
| 92 | 4 | Outer FEC identity bytes |
| 96 | 8 | immutable descriptor bundle bytes |
| 104 | variable | source path、build、compression、Outer FEC、descriptor bundle，依次连续存放 |
| end-4 | 4 | CRC32C |

source index 文件名为 `<16-hex-volume>-<32-hex-file-id>.txt`，内容严格为 32 个 lowercase SessionId hex 字符和一个 LF。v1 使用较窄 identity key；v2 不会误选旧 key，也不会把旧状态当成可恢复的新 Session。

### 3.1 Immutable descriptor bundle v1

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 4 | magic `PBDB` |
| 4 | 2 | bundle version = 1 |
| 6 | 2 | reserved = 0 |
| 8 | 8 | SegmentCount |
| 16 | variable | `uint32 length + ControlRecord bytes`：Session、FinalManifest、随后按 ordinal 排列的每个 SegmentDescriptor |

该 bundle 的 BLAKE3 写入 mutable runtime state。恢复时既比较 exact bundle bytes，也比较 runtime 中的 bundle digest。

## 4. `runtime.state` schema v2

所有整数均为显式 little-endian。最终 CRC32C 覆盖它之前的整个文件；总长最多 2 MiB。

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 4 | magic `PBER` |
| 4 | 2 | store version = 2 |
| 6 | 2 | reserved = 0 |
| 8 | 8 | total file bytes，包含末尾 CRC |
| 16 | 8 | monotonically increasing generation，0 非法 |
| 24 | 16 | SessionId |
| 40 | 32 | immutable descriptor bundle BLAKE3 |
| 72 | 8 | completed/current Carousel pass |
| 80 | 8 | current Segment ordinal |
| 88 | 8 | FrameSequence lease exclusive endpoint |
| 96 | 8 | repair lease entry count，必须等于 SegmentCount |
| 104 | `4 * SegmentCount` | 每段 repair ID lease exclusive endpoint |
| end-4 | 4 | CRC32C |

每次改变 lease endpoint 或 Carousel position 都生成完整新文件：同目录 `.tmp` 独占创建、完整写入、`FlushFileBuffers`，随后 `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` 原子替换。持久化失败会恢复内存中的旧 endpoint/position 并停止调用路径。

## 5. 4,096-ID lease crash window

FrameSequence 和每段 repair ID 使用相同的 4,096 对齐 exclusive endpoint 规则。

| 崩溃/失败窗口 | 已允许使用 ID？ | 重启行为 | 安全结果 |
| --- | --- | --- | --- |
| 请求 lease 之前 | 否 | 从旧 endpoint/初值继续 | 没有提前使用 |
| `.tmp` 写入或 flush 失败 | 否 | 旧正式文件仍权威；内存回滚 | 调用失败，不生成对应帧/equation |
| 原子替换成功、首个 ID 使用之前 | 否 | 从新 endpoint 开始 | 整个未用 lease 被跳过，但不重用 |
| lease 内已有部分 ID 被使用 | 是 | 从 lease endpoint 开始 | 未用尾部被跳过，所有已用 ID 均小于恢复起点 |
| Segment/pass 已推进、position 尚未 checkpoint | 是 | 可能重播旧 Segment/pass，但从新 ID lease 开始 | Control/systematic 可重复；repair/FrameSequence 不回退 |
| runtime CRC/长度/digest 冲突 | 不确定 | 不恢复 | fail closed，不猜测最新状态 |

该表覆盖进程终止与可观察持久状态顺序；本目标没有执行断电/文件系统硬件故障认证。

## 6. 无屏幕验证报告

### 6.1 G02 流式预扫描与持久状态

定向命令：

```powershell
cmake --build build-unified-release --config Release --target PBApplicationTests --parallel 2
& 'D:\MyProjects\PixelBridge\build-unified-release\tests\PBApplication\Release\PBApplicationTests.exe' '[sender]' --durations yes
```

测试生成本地 JSON：`build-unified-release/tests/PBApplication/g02-sender-headless-report.json`；最终控制台记录保存在 `build-unified-release/tests/PBApplication/g02-sender-tests.txt`。这些路径是可再生的本地证据，不提交 build artifact。固定 deterministic source 的本次结果为：

| 项目 | 结果 |
| --- | ---: |
| source bytes / SegmentCount | 25,169,920 / 4 |
| completed Carousel passes | 2 |
| scheduled / Control / Data frames | 12,250 / 744 / 11,506 |
| scheduled systematic / repair equations | 38,318 / 7,694 |
| padding duplicate slots | 12 |
| descriptor-resident encoded bytes | 0 |
| peak resident encoded Segments / bytes | 2 / 16,777,216 |
| FrameSequence used | 0..12,249 |
| persisted/restarted FrameSequence | 12,288 / 12,288 |
| source write/delete sharing | both denied |
| restart exact Session / equation | resumed / reproduced |

三个完整 8 MiB Segment 均为 RAW、`K=6,385`、每轮 `R=1,277`；pass 0 首个 repair ID 为 6,385，pass 1 为 7,662，落盘/restart 起点为 12,288。最后 4 KiB Segment 为 RAW、`K=4`、`R=16`，两轮首个 repair ID 为 4/20，落盘/restart 起点为 4,096。

测试使用现有四-slot carrier 参数只为覆盖生产 `SenderFrameBuilder` 接线，但从未调用 raster `Build()`。它不能证明最终 `PB-Unified-LC4-V1` mixed-slot mapping、Data Window、真实显示、接收端恢复、20 GiB/500 GiB 长运行、吞吐或断电一致性；这些仍分别属于后续 G04、G05、G06..G14、G18..G22。

### 6.2 G09 Unified mixed-slot scheduler/reference raster

定向构建与执行：

```powershell
cmake --build build-unified-release --config Release --target PBUnifiedSenderSchedulerTests PBUnifiedVisualCpuTests PBProtocolTests --parallel 2
& 'D:\MyProjects\PixelBridge\build-unified-release\tests\PBApplication\Release\PBUnifiedSenderSchedulerTests.exe' --rng-seed 9092026 --durations yes
& 'D:\MyProjects\PixelBridge\build-unified-release\tests\PBModulation\Release\PBUnifiedVisualCpuTests.exe' '[g09]' --rng-seed 9092026 --durations yes
& 'D:\MyProjects\PixelBridge\build-unified-release\tests\PBModulation\Release\PBUnifiedVisualCpuTests.exe' '[unified][cpu][admission]' --rng-seed 9092026 --durations yes
& 'D:\MyProjects\PixelBridge\build-unified-release\tests\PBProtocol\Release\PBProtocolTests.exe' '[g09]' --rng-seed 9092026 --durations yes
```

| 子集 | 本次结果 |
| --- | ---: |
| Unified scheduler/clock/zero-byte raster | 4 cases / 332,630 assertions |
| Unified explicit mixed frame input | 2 cases / 366 assertions |
| 既有 Unified admission 回归 | 2 cases / 383 assertions |
| PB-Control fixed-info framing | 1 case / 28 assertions |

30 秒模拟在 1、15、60 Hz 均观察到 tick 0、10 秒、20 秒的三轮 Control burst；默认 cadence 帧为 12 Control + 19 Transport，其他帧为 31 Transport。60 Hz 的 5 秒停顿只生成 tick 300 的一帧并丢弃 299 ticks。零字节 raster 在不传 sender slot plan 的条件下接受 8 个 Control、0 个 Transport，ControlPlaneReceiver 插入 Session/Manifest 各一份并将另外六份判为相同重复，完成空 Segment map 与 FinalManifest；资源重组计数与字节保持 0。

本地可再生日志为 `build-unified-release/tests/PBApplication/g09-unified-scheduler-tests.txt`、`build-unified-release/tests/PBModulation/g09-unified-frame-input-tests.txt`、`build-unified-release/tests/PBModulation/g09-unified-admission-regression.txt` 与 `build-unified-release/tests/PBProtocol/g09-control-framing-tests.txt`，均不提交。此验证不包含完整 CTest、产品 GUI、GPU、capture、实屏、长文件吞吐或后续产品生命周期接线。
