# G19：20 GiB+ headless 大文件能力

> 状态：**G19 已通过最终验收**（2026-09-04）。20 GiB + 64 KiB、2,561 个 Segment 全部经真实 headless payload 链完成，一次进程终止后恢复，最终文件全字节及外部双 digest 一致。本轮没有进入 G20。
> 前置 G18 为 `2c1d74db693962fe48d8806c007f20a6213fc958`；已重新核对九个终止点、五负例、Encoder 定向 ASan 日志及修复源码封印，没有重跑 G18。

## 1. 范围与真实链路

本目标复用 opt-in `PBProcessRecoveryWorker` 的生产 Unified scheduler、Control/Transport serialization、Outer FEC、Receiver、journal、PBStorage 和 final reopen。新增 `large-file` 私有测试模式，普通产品不提供该入口；原 G18 三个模式的 256 MiB、RAW/Wirehair 和默认 600 秒超时合同保留。只执行专用 G19 harness，不调用 CTest、ASan、GUI、GPU、capture、native/remote Gate 或输入设备。

G19 通过原 `LargeOutputConfirmationController` 的 Request/Resolve/Apply 接受这次明确指定的大文件测试。每次新进程仍先进入 `AwaitingLargeOutputConfirmation`，记录 run/request 身份；没有把默认 4 GiB 免提示阈值调大，也没有绕过默认 500 GiB/4 active 的 `ReceiverResourcePolicy`。

生产文件中只增加 `PB_PROCESS_FAULT_TESTS` 条件编译的观测：

- `DescribeSource` 记录完整预扫描（流式读取、raw/encoded digest、压缩选择、descriptor/control 生成）的耗时；不把 session-state 查找/落盘耗时混入该值。
- journal 在成功 open/append/compact 后记录逻辑文件长度高水位，避免仅在 compaction 后采样而漏掉峰值。该数字不是 journal 与 compaction 临时文件合计的磁盘占用。
- 观测器只含测试子进程所属线程上的固定大小计数器，不传输 payload；普通 `PBApplication`/产品 EXE 没有这些计数器或终止点。

没有改变公共接口、wire/Profile/FEC、生产策略、持久化 schema 或存储错误处理。

## 2. 结构化稀疏 fixture

| 项目 | 合同 |
| --- | --- |
| 逻辑长度 | **21,474,902,016 bytes = 20 GiB + 64 KiB** |
| Segment target / count | 8 MiB / **2,561** |
| 最后 ordinal / raw offset | **2,560 / 21,474,836,480** |
| 最后一段长度 | 65,536 bytes，额外覆盖非整 Segment 尾部 |
| seed | `719ab40d3e68125cf94d072be138c66a918de74fbaa638529671d5fe1024ac83` |
| 数据生成 | SHAKE256(seed + LE64 ordinal + LE64 相对位置 + LE64 rawSize)，输出字节映射为 `(x % 255) + 1` |
| 常规 Segment 区域 | 起点 16 KiB、中点 16 KiB、末尾 32 KiB，其余 hole 为零 |
| 短尾 Segment 区域 | 整个 64 KiB |
| 首尾标记 | 首 64 bytes 为 `<8sQQQ32s>`：`PBG19V1\0`、ordinal、raw offset、raw size、seed；末 64 bytes 为两份 SHA-256(header) |

因此每个 Segment 都有确定性非零数据及唯一 ordinal/offset marker，不以全零高压缩文件代替真实 Segment 触达。fixture 创建后先完整顺序计算 SHA-256/BLAKE3，将规则、seed、长度、实际 sparse 属性和分配空间写入 create-only manifest。

发送端使用既有 level-3 Zstandard 自动选择/RAW fallback；所有 encoded bytes 都从源重新流式读取、编码并校验，然后经正式 Outer payload 路径传送。压缩降低传输量，但**接收端仍实际解压并写回全部 20 GiB+ 的原始字节**。只有 descriptor table 可以按 SegmentCount 增长，不能保留整文件 raw/encoded payload。

运行前要求目标卷至少 64 GiB 可用空间；运行目录必须不存在。源使用 `FSCTL_SET_SPARSE` 和 64-bit `SetFilePointerEx`/`SetEndOfFile` 扩展逻辑 EOF，只写规定的数据区域。所有源、输出、日志与失败尝试均留在 ignored build tree，不覆盖已有 artifacts。

## 3. 终止、恢复与独立验收

在 Receiver 完成 ordinal **1,024** 后写入进度并调用真实 `TerminateProcess(..., 218)`；此时 1,025 个 Segment 已 durable，已写过 8 GiB 边界，尚未发布。必须同时存在匹配 PID、点名和 exit 218 的 flushed marker。

监督进程检查 journal framing/CRC 与 completed ordinals 恰为 0..1,024，复制 Encoder runtime 与 Decoder journal 到 create-only `frozen-metadata/`，然后在同一传输状态上创建新进程：

1. 原完整预扫描重新验证源和 immutable descriptor bundle，同一 Session 恢复。
2. 原 Receiver 重读 `.part`，重新验证已 completed Segment 的 RawDigest；保留 1,025 个 durable completed。
3. FrameSequence 和 repair ID 跳过旧 durable lease；检查 run 内与跨进程不复用/不越界。
4. 汇合终止前/重启后的真实 completed 事件，要求所有 ordinal 0..2,560 均实际经过 Receiver；逐条验证 64-bit raw offset/size。
5. 原 PBStorage 完成 WholeFileDigest、安全 rename、final reopen 后才接受 `published`；旧 `.part`/`.resume` 必须清理。
6. 子进程结束后，监督进程按 sealed seed/规则逐 Segment 重新生成期望字节，**源、最终文件、重生成期望值三者逐字节比较**，同时计算最终 SHA-256/BLAKE3 并与 sealed fixture 一致；不只检查文件长度或少量标记。期望值计算独立于生产 worker，但与夹具创建复用同一个规则函数，不冒充第二套独立的 fixture 算法实现。

每个 child 有 1,800 秒上限，只有超时的自有 child 可被 supervisor 终止；失败立即退出，不试到通过。worker 的既有 100,000-frame/有限 Carousel pass 上限不放宽。记录 prescan MiB/s、恢复时间、Sender 双 Segment、Receiver active、resume payload、combined OS working set 和 journal 逻辑高水位。combined 是 receiver 保守上界，不冒充独立 Decoder EXE 测量或视觉吞吐。

## 4. 500 GiB：仅元数据边界

专用 worker 在不创建任何 500 GiB 数据文件的情况下：

- 计算 500 GiB / 8 MiB = 64,000 个 Segment；checked multiply/add 验证最后 ordinal 63,999 的 offset/end。
- 对真实 SessionDescriptor、最后 SegmentDescriptor、FinalManifest 使用正式 serializer/parser 往返验证；保留完整 64-bit 长度与 offset。
- 拒绝 500 GiB + 1 的 policy 越界，以及 `UINT64_MAX + 1`、`UINT64_MAX * 2` 的算术溢出。

这些只是格式/算术证明，不表示已传送 500 GiB，不证明对应 500 GiB payload 的 digest、性能或内存占用。证据写入 `boundary-500gib.json`，不会被提交到正常接收管线。

## 5. 构建、执行与第一次夹具失败

```powershell
cmake -S . -B build-unified-release -DPB_BUILD_PROCESS_RECOVERY_HARNESS=ON
cmake --build build-unified-release --config Release --target PBProcessRecoveryWorker --parallel 6
& <python> tests\PBApplication\run_large_file_recovery.py `
  --worker build-unified-release\g19-candidate\PBProcessRecoveryWorker.exe `
  --run-directory build-unified-release\g19-final-2
```

`g19-candidate/` 为 build 输出 EXE、blake3/zstd DLL 的 create-only 快照。worker SHA-256 为 `853908b8154a44ee34492dea76c6401f77f52324b9c6143a2448337ac286a669`，嵌入父提交 `2c1d74d`；实际源码由 `provenance.json` 逐文件封印，不冒充提交后 HEAD 构建。再次重放须更换为不存在的新 run directory。

第一次运行 `g19-final/` 在 fixture 分配空间检查中停止，**没有启动生产 worker**：虽然 sparse attribute 为真，flush/close 后实际 allocation 等于逻辑长度 21,474,902,016 bytes。没有放宽检查以求通过。

`g19-fixture-size-diagnosis/` 的受控对照保留原始结果：同样的 32 MiB 文件、相同稀疏标志与四段数据写入，仅改变扩展逻辑长度的方法，Python `truncate` 路径最终 allocation 为 33,554,432 bytes，Win32 SetEOF 路径为 786,432 bytes。活跃句柄期间查询的 allocation 不作为最终值，以上结果在 flush/close 后取得。生成器据此改用 SetEOF，不修改生产 PBStorage，也不覆盖失败文件；第二次 source 实际 allocation 为 **503,382,016 bytes（480.0625 MiB）**。

## 6. 最终结果与未执行门禁

专用运行 `g19-final-2` **exit 0**。第一次只有夹具失败，没有运行失败的生产 transfer 再重试到通过；修复夹具后的完整终止/恢复链一次通过。

| 项目 | 结果 |
| --- | --- |
| 源与最终长度 | 21,474,902,016 bytes |
| 实际 completed / 逐字节比较 Segment 数 | 2,561 / 2,561 |
| 最大 ordinal / raw offset | 2,560 / 21,474,836,480 |
| 终止进程 | PID 61688，exit 218，104.484 秒；1,025 个 durable completed |
| 恢复进程 | PID 31088，exit 0，同 Session；1,025 个 completed 重新验证后保留 |
| 重启到权威发布 | 242.047 秒，worker 内部 total 242.009 秒 |
| `.part` 已完成数据恢复重验 | 3.294 秒 |
| 第一次 / 重启预扫描 | 26.216 / 25.561 秒；781.205 / 801.223 MiB/s |
| 外部全字节与双 digest 复验 | 32.406 秒 |
| combined 最大观测 working set | 60,805,120 bytes，**57.99 MiB** |
| Sender 常驻 encoded 高水位 | 2 个 Segment / 131,787 bytes |
| Receiver active / reserved decoder 高水位 | 1 个 / 4,707,364 bytes；未提高默认 cap=4 |
| resume payload 高水位 | 134,028 bytes |
| journal 逻辑长度高水位 | **935,526 bytes**，含 append-before-compact 观测 |
| 编码选择 | 2,560 个 Zstandard Segment + 1 个 RAW 短尾，encoded 合计 168,749,661 bytes |
| FrameSequence 身份 | 终止前 3,075 帧、重启后 4,611 帧，跨进程无复用；重启跳过 lease end 4,096 |
| repair 身份台账 | 终止前 16,400、重启后 24,592 个预留 ID，run 内/跨进程无复用、不越界 |

预扫描速率按逻辑原始字节（包含 sparse holes）计量，包含压缩/摘要/描述符准备，不是物理磁盘读取速率，更不是视觉信道吞吐。重启进程实际交付 repair 为 24,577；台账的其余 15 个属于完成前已预留但未交付的身份，仍视为已消耗，不混用两种计数口径。

最终文件经过原 PBStorage WholeFileDigest、安全 publish、final reopen。外部复验比较了全部字节，并得到与 sealed fixture 相同的：

- SHA-256：`c09a18ae3cf85ea8690da225ef4ae3a79761171c4d42f1d2f36673e5f9b1eb8c`
- BLAKE3：`5e1cb7a2f50cd948fd283ec736cabeede48c211bb94d070acfd9bdca22ad755b`

成功后无 active `.part`/`.resume` 残留；未记录到 Encoder 原子替换 retry/failure 事件。500 GiB 元数据边界的最后 offset 为 536,862,523,392，三种 descriptor/manifest 往返及越界/溢出拒绝均通过，没有分配 500 GiB 数据。

主要本地证据：

- `build-unified-release/g19-final-2/report.json`：完整通过报告、fixture、provenance、resource/恢复/外部比较。
- 同目录 `audit-summary.json`：只读复核当前源码/worker hashes、PID/exit、长度、恢复点、最终 artifacts 和保护文件 hash；不重跑 20 GiB transfer。
- 同目录 `fixture-manifest.json`、`transfer/{terminated,restarted}/`、`transfer/frozen-metadata/`：规则与原始过程证据。
- `build-unified-release/g19-final-2.txt` / `.exitcode`：完整监督日志与 exit 0；`g19-configure.txt`、`g19-build.txt` / `.exitcode`：定向构建成功。
- `build-unified-release/g19-final/`、`g19-fixture-size-diagnosis/`：未覆盖的首轮夹具失败与受控定位证据。

**退出：已满足 G19。** 全部 Segment 被实际触达并完整发布，64-bit 末段 offset 正确，至少一次真实终止恢复，最终 byte-exact，应用 payload 内存受双 Segment/4 active 上限约束，500 GiB 仅做元数据边界。此结论不包括 20 GiB CSPRNG/RAW 吞吐、独立 Decoder EXE 内存或真实像素链认证。

本轮没有运行 full CTest、ASan、GPU/GUI/native/remote、断电、安装包或提交后嵌入身份复验；没有操作任一屏幕或鼠标键盘。`docs/PHASE1_GATE_REPORT.md` 内容和未暂存状态保持不变。下一目标为 **G20：CP-C 最终 Release、定向 ASan 与 DISPLAY2 native Gate**，必须先读取其门禁范围并重新核实显示器身份/边界。
