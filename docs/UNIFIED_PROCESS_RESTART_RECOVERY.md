# G18：256 MiB 真实进程终止与恢复

> 状态：实现与定向证据已整理，但 **G18 未通过最终验收，不得进入 G19**（2026-09-04）。
> 基线为 `805dd2a5a8b8f937f56287e4e030ff6f50860074`（G17）。实现与失败证据已提交为 `8965f3d`，定向诊断记录为 `33f6bfa`。用户追加要求多次复测：三次 encoder-prescan 均通过，但随后 lease-persisted 再现同一 Win32 5，累计四次自然失败；不能按“复测均未发生”的条件忽略。未实施生产修复。定向诊断见第 8 节，最新复测见第 9 节。
> 本目标的无像素 headless 验收与仅测试构建启用的最小插桩已由用户明确批准。

## 1. 范围与证据边界

- 新建测试专用 `PBG18Application` / `PBG18Storage` 静态库，复用原 target 的源码、依赖及编译设置，只有克隆库定义 `PB_PROCESS_FAULT_TESTS`。
- `PBApplication`、`PBStorage`、两个产品 EXE 和生产公共头不启用终止点；默认 `PB_BUILD_PROCESS_RECOVERY_HARNESS=OFF`。
- 测试入口在原 runtime 翻译单元中复用 `PrepareDurableSender`、`SenderFrameBuilder`、Unified scheduler、正式 Control/Transport serialization、`ReceiverPipeline`、`ReceiverIngress`、journal 和 `OutputFile`。没有另建 FEC、恢复或发布状态机。
- 每个测试子进程执行 sender/receiver 无像素闭环；独立 Python supervisor 只提供路径与终止点，读取元数据与最终文件，不传输 Control/Transport 或 payload。所有子进程使用 `CREATE_NO_WINDOW`，无屏幕、GPU、capture、GUI 或鼠标键盘输入。
- 进程终止使用 `TerminateProcess(GetCurrentProcess(), 218)`，不经过异常展开、对象析构或普通 Stop checkpoint。成功注入必须同时有 exit 218、匹配 PID/终止点的已 flush marker；仅有退出码不算成功。
- 当前为进程 crash consistency，**不是断电、磁盘控制器缓存丢失或文件系统损坏认证**。
- `visualChainCovered=false`。不生成视觉 FPS/goodput，不把 headless 执行秒数解释成视觉信道吞吐。
- 已核对 G17 提交、其 G12/G15/G16 祖先与 G17 telemetry/report/cache 定向日志；没有以文档状态代替提交或通过日志，也没有重跑 G17。

## 2. 注入点与重启后的独立判据

| 点 | 实际位置 | 已 durable completed Segment 期望 | 重启重点 |
| --- | --- | ---: | --- |
| `encoder-prescan` | `DescribeSource` 扫描两个 Segment 后，16 MiB，durable Session 创建前 | 0 | 不采用未完成预扫描；重新扫描 |
| `lease-persisted` | 真实 `EnsureFrameSequenceLease` 成功及身份台账记录后，首次 payload 交付前 | 0 | 同 Session；跳过已持久化 frame/repair lease |
| `encoder-mid-segment` | Segment 2 的 block 128 由真实 builder 序列化后、Receiver 交付前 | 2 | 原有 completed 不丢；未 durable 尾部可重收 |
| `decoder-active` | Segment 2 的 block 128 进入 active FEC 后 | 2 | 使用真实 1 秒 checkpoint，恢复时确有 active cache replay |
| `part-flushed` | Segment 2 的 `.part` flush/checkpoint 后、Completed record 前 | 2 | active equation 重放或 Carousel 重收；不盲信 `.part` |
| `completed-record` | Segment 2 的 Completed append + flush 后、内存更新与 compact 前 | 3 | 未 compact journal 的 Completed record 必须保留 |
| `whole-digest` | `OutputFile` 对 `.part` 顺序 BLAKE3 已读取 8 MiB 后 | 32 | 重新验证 whole digest，不提前发布 |
| `before-rename` | whole digest 成功、最终路径 absent 检查后，rename 前 | 32 | publish intent 和完整 `.part` 恢复 |
| `after-rename` | rename 成功后、final reopen 与 journal cleanup 前 | 32 | 使用原 G05 路径恢复最终文件，重新打开验证 |

每点使用独立、create-only 的 sender/output/evidence 目录。Source 为实际 256 MiB OS CSPRNG 文件，32 个 8 MiB Segment，压缩关闭，全部 RAW/Wirehair；每段故意丢 systematic block 1，要求 repair 参与恢复。

`frames.jsonl` 是保守的交付前身份台账：即使一个 frame 在交付前终止，其记录的身份也视为已经消耗。supervisor 检查同 run 和重启前后 FrameSequence、repair ID 不重复，且所有 ID 在已持久化 lease 内，重启起点不低于前次 lease end。systematic 原块的幂等重发不归入 repair ID 重用。

## 3. 负例

- 从真实 `decoder-active` crash 的状态复制只读基线，再派生三个独立目录：明确的 torn final prefix、内部完整 record CRC 损坏、完整 header 的伪长度。派生文件不覆盖原始 crash 证据。
- torn final tail 必须识别并修复，最终文件仍通过外部长度/SHA-256/BLAKE3；内部 CRC 与伪长度必须拒绝，且没有最终文件发布。
- 专用 writer 在**不改变默认 cap=4** 的条件下写入四个 active cache、五份 canonical Segment descriptor，然后真实终止。supervisor 只在派生副本中追加第 5 个 cache，使用相应 RAW 源块、正确 framing/单调 generation/CRC。真实接收管线必须在 `.part` 预分配前因 active limit 拒绝。该派生 journal 是不可信输入负例，不是正常传输通道。
- 在独立源文件副本中改变一个字节，并保留 file identity、长度、mtime。预扫描必须识别内容变化，拒绝复用原 Session，创建新 Session；不能只验证 mtime 改变的浅层路径。

## 4. 内存与时间的含义

- `PROCESS_MEMORY_COUNTERS` 的 working set 与 peak 在终止 marker、恢复过程和最终 worker report 中记录。
- 普通闭环 worker 同时包含 sender 与 receiver，因此其整个进程峰值是 receiver 的保守上界，**不是独立 Decoder EXE 的实测 working set**。独立 sender-only worker 在 source-change 用例中记录发送端测量。
- 同时保留 sender current/next encoded bytes、Receiver active/reserved decoder、resume active+pending payload 的实际高水位；不能将 reservation 当成 OS 实际 working set。
- 重启到发布耗时由 supervisor 单调时钟计时；worker 单独记录预扫描、resume verification 与整体耗时。

## 5. 构建与运行

使用路线中的 `build-unified-release` 配置，仅额外启用专用测试 target：

```powershell
cmake -S . -B build-unified-release -DPB_BUILD_PROCESS_RECOVERY_HARNESS=ON
cmake --build build-unified-release --config Release --target PBProcessRecoveryWorker --parallel 6
& <python> tests\PBApplication\run_process_recovery.py `
  --worker build-unified-release\tests\PBApplication\Release\PBProcessRecoveryWorker.exe `
  --run-directory build-unified-release\g18-new-clean-run --negative-checks
```

`--run-directory` 必须不存在。可以使用 `--fixture-manifest <已存在清单>` 复用已封印且重新计算哈希确认一致的 CSPRNG fixture；仍为每个点创建全新的状态目录。`--points` 支持单点诊断，`--repeat 2` 支持显式重复；不会加入默认 CTest。未请求的重复矩阵不自动运行。

负例出现夹具错误后，只重跑受影响子集，避免重跑已通过的 256 MiB 闭环。示例为本次实际通过的命令；再次执行时必须改用新的 run directory：

```powershell
& <python> tests\PBApplication\run_process_recovery.py `
  --worker build-unified-release\tests\PBApplication\Release\PBProcessRecoveryWorker.exe `
  --run-directory build-unified-release\g18-quota-source-final `
  --fixture-manifest build-unified-release\g18-first-flow\fixture-manifest.json `
  --negative-checks --negative-baseline build-unified-release\g18-campaign-3\immutable-active-baseline `
  --negative-cases over-active-quota changed-source
```

`--negative-baseline` 只接受两个 completed 加 Segment 2 active 的既有状态副本，并与 `--points` 互斥；不会在原始状态上修改 journal。各 negative 和 termination case 都立即写入 create-only `verified.json`；整组中途失败不抹去已完成证据，也不生成整组通过声明。

Python 3.12.9 与既有 `blake3 1.0.9` 用于独立外部复验；不新增生产依赖。`provenance.json` 封印 worker SHA-256/BLAKE3、相关源文件 SHA-256、环境、命令和初始磁盘可用量。所有日志、源 fixture、`.part`、journal、最终输出均留在 ignored build tree，不进入源码提交。

## 6. 定向 ASan

ASan 使用独立 `build-unified-asan`、MSVC 14.44.35207、RelWithDebInfo。既有 CMake 通过 `PB_BUILD_FUZZERS=ON` 给共享库启用 `/fsanitize=address`；这里只构建指定测试目标，不构建/运行 fuzz workload，也不启用任何 native Gate。STL container annotations 延续 pinned Wirehair 的既有一致性配置；普通 ASan 内存检测启用，不声称第三方预编译库获得插桩。

```powershell
cmake -S . -B build-unified-asan -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake `
  -DPB_QT_ROOT=<qt>6.10.1/6.10.1/msvc2022_64 `
  -DPB_BUILD_APPS=ON -DPB_BUILD_TESTS=ON -DPB_BUILD_TOOLS=OFF `
  -DPB_BUILD_FUZZERS=ON -DPB_BUILD_BENCHMARKS=OFF -DPB_BUILD_PROCESS_RECOVERY_HARNESS=OFF
cmake --build build-unified-asan --config RelWithDebInfo `
  --target PBProtocolTests PBReceiverTests PBStorageTests PBApplicationTests --parallel 6
$asanRuntime = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\clang_rt.asan_dynamic-x86_64.dll'
foreach ($target in @('PBProtocol', 'PBReceiver', 'PBStorage', 'PBApplication')) {
  Copy-Item -LiteralPath $asanRuntime -Destination "build-unified-asan\tests\$target\RelWithDebInfo"
}
$env:ASAN_OPTIONS = 'halt_on_error=1'
.\build-unified-asan\tests\PBProtocol\RelWithDebInfo\PBProtocolTests.exe '[pbprotocol][resume-state]' --rng-seed 18092026 --durations yes
.\build-unified-asan\tests\PBReceiver\RelWithDebInfo\PBReceiverTests.exe '[resume-replay],[resume-state-file]' --rng-seed 18092026 --durations yes
.\build-unified-asan\tests\PBStorage\RelWithDebInfo\PBStorageTests.exe '[storage][resume],[storage][publish],[storage][random-write]' --rng-seed 18092026 --durations yes
.\build-unified-asan\tests\PBApplication\RelWithDebInfo\PBApplicationTests.exe '[application][decoder][resume],[application][encoder][durable-lease]' --rng-seed 18092026 --durations yes
```

已通过的实现前基线：22 / 2,261、13 / 243、9 / 132、6 / 348 cases/assertions。每个 EXE 的 `dumpbin /imports` 证明导入 ASan runtime。最初使用当前 Catch2 不支持的 `--list-test-names-only`，在运行测试前改为 `--list-tests`；这不是 sanitizer 或测试失败。

实现后只重建受条件编译改动影响的 `PBStorageTests`、`PBApplicationTests`，仅重跑上面两个对应 selector，分别 **9 cases / 132 assertions、6 / 348**，exit 0，无 sanitizer 报告；没有再次运行未改动的 Protocol/Receiver 组。所有 native Gate cache 开关均为 OFF。最终日志：

- `build-unified-asan/g18-affected-final-build.txt`、同名 `.exitcode`（0）；
- `build-unified-asan/g18-storage-final.txt`、`g18-application-final.txt`，对应 `.exitcode` 均为 0；
- 实现前基线 `build-unified-asan/g18-{protocol,receiver,storage,application}-baseline.txt`；
- `windeployqt` 报 `VCINSTALLDIR is not set` 环境警告，但部署/链接成功，以上最终测试实际加载运行成功，不把警告当作 sanitizer 失败或隐藏它。

## 7. 当前结果与未覆盖项

### 7.1 已取得的局部成功证据

源文件 `build-unified-release/g18-first-flow/fixture.bin` 为 268,435,456 bytes，32 个 8 MiB RAW Segment：

- SHA-256：`b3372cd7dbfe2e72a7c0ad59f975786b28f69bcc137b6ea0b37dcca207860627`
- BLAKE3：`861a0c2d079c206d31d87aa79c22fb7ab5b26ae554aa8f22fe56caf1c4046056`

以下各点均有一次真实终止、重启、32 段发布、外部长度与两个 digest 一致的记录。它们来自不同阶段的 worker，**不是同一最终候选整组通过**：

| 点 | durable completed | 重启到发布 / 秒 | 重启进程峰值 / MiB | `build-unified-release/` 下证据目录 |
| --- | ---: | ---: | ---: | --- |
| encoder-prescan | 0 | 50.157 | 82.50 | `g18-first-flow/r1-encoder-prescan` |
| lease-persisted | 0 | 52.766 | 85.54 | `g18-observed-lease/r1-lease-persisted` |
| encoder-mid-segment | 2 | 54.859 | 85.51 | `g18-campaign-3/r1-encoder-mid-segment` |
| decoder-active | 2 | 54.297 | 85.03 | `g18-campaign-3/r1-decoder-active` |
| part-flushed | 2 | 47.469 | 85.06 | `g18-campaign-3/r1-part-flushed` |
| completed-record | 3 | 49.453 | 99.11 | `g18-campaign-3/r1-completed-record` |
| whole-digest | 32 | 0.875 | 50.22 | `g18-campaign-3/r1-whole-digest` |
| before-rename | 32 | 0.875 | 50.25 | `g18-campaign-3/r1-before-rename` |
| after-rename | 32 | 0.703 | 43.17 | `g18-campaign-3/r1-after-rename` |

`decoder-active` 重启确有 active blocks replay；所有相关 completed 均保留。身份台账未发现 FrameSequence/repair ID 在 run 内或跨重启复用。以上终止/重启进程的最大观测峰值为 **99.11 MiB**；sender-only 的 source-change run 峰值为 52,674,560 bytes（50.23 MiB），resident encoded bytes 为 16 MiB。这些是本 fixture 的测量，不是独立 Decoder EXE 或所有输入的内存认证。

五个负例均通过：`g18-final-negatives/{torn-final-tail,internal-crc,forged-length}` 和 `g18-quota-source-final/{over-active-quota,changed-source}`。前组三个进程的既有日志已重新核对，未因后续 quota 夹具问题而重复传输。torn tail 恢复保留 completed/active 状态并通过外部文件验证；另外两种 journal 损坏给出对应 parser 原因且没有错误发布。

### 7.2 不能关闭 G18 的阻塞

| 失败 run | 失败时已 durable completed | 行为 |
| --- | ---: | --- |
| `g18-campaign-1/r1-encoder-prescan/restarted` | 27 | Carousel position 原子替换返回 `win32=5`，进程 exit 1，未发布 |
| `g18-campaign-2/r1-lease-persisted/restarted` | 28 | 同类原子替换失败，exit 1，未发布 |
| `g18-final-prescan/r1-encoder-prescan/restarted` | 29 | 最终候选同类失败，exit 1，未发布 |

最终候选的 `atomic-replace-failure.json` 记录：MoveFileEx error 5、target/tmp attributes 均 32、失败后的 DELETE-access 探测均成功、Restart Manager 返回成功但 `lockingPids=[]`。这不能证明失败时没有瞬时占用，也不能把原因归咎于防病毒、系统、权限或项目代码中的任何一方。

观察器只在原有失败返回前读取两个 fixture 文件的属性、DELETE-access 和有限的占用 PID；不会删除/重命名文件、重试原子替换、停止其他进程、调用输入设备或改变返回值。原有失败清理继续执行。**生产持久化行为没有修改，失败没有被吞掉。**

第一个 I/O 失败状态另做过一次窄恢复：27 个 durable completed 保留，8.125 秒完成，frame/repair 无复用，最终长度/SHA-256/BLAKE3 一致。记录为 `g18-campaign-1/r1-encoder-prescan/io-error-recovery-verified.json`。这是额外 I/O 错误恢复证据，不会把原来 exit 1 的测试改写成通过，也不能证明三次错误的原因已经解决。

最终候选的失败重放命令如下；**当前只是失败证据，不是验收通过命令**，再次运行须新建不同目录：

```powershell
& <python> tests\PBApplication\run_process_recovery.py `
  --worker build-unified-release\tests\PBApplication\Release\PBProcessRecoveryWorker.exe `
  --run-directory build-unified-release\g18-final-prescan `
  --fixture-manifest build-unified-release\g18-first-flow\fixture-manifest.json `
  --points encoder-prescan
```

此前已就扩大原子替换失败的定向诊断范围向用户提问并暂停。用户随后批准“先定向诊断，并在原因确认后再确定最小修复”；该范围审批已经解除，不代表根因已确认或自动批准重试/持久化语义变更。诊断结果见第 8 节，G19 仍未开始。

### 7.3 夹具修正与证据封印

- 最初 quota writer 尝试将 cap 设为 5，被 `DecoderResumeStore::Open` 的现有配置校验提前拒绝。修正为默认 cap 下的四缓存种子和派生第五缓存，**没有放宽生产校验**。原失败日志 `g18-final-negatives.txt` 保留。
- `after-rename` 子进程实际已成功发布，旧 supervisor 却错误要求普通 `RestoreResumeState` 的 `restore.json`。既有 G05 `CompleteRecoveredPublish` 直接校验完整 intent/manifest 和已经 rename 的文件，不走 active replay。修正 verifier 要求 32 个 durable completed、32 个 restored verified、final reopen、零 frame/Transport replay 和外部三个文件判据；不改生产分支。使用已完成进程证据重新核对，`g18-campaign-3/failure.json` 仍保留原 assertion 失败。
- worker 初始构建的 wchar narrowing、Restart Manager const pointer-array 两个编译问题均已修正，最终 `g18-quota-fixture-build.txt` / `.exitcode` 为成功；没有降低 warnings 或测试判据。
- `g18-worker-matrix-snapshot/` 保留追加 I/O 观察后、修正 quota 前的 worker 与其非系统 DLL；worker SHA-256 为 `dd8987603ca10682629ed7850cd56d03045817e767887b3643b239f55a0bec5d`。
- `g18-worker-final-snapshot/` 保留最终 worker 与非系统 DLL；worker SHA-256 为 `3d7d269fe726281422c7cc2d6d7e7a9265d619e7b7241f11cfb2314940639682`。两个阶段之间 C++ 差异只在 quota fixture 分支，Python 另有 verifier/负例子集选项修正；仍明确分别记录 producer，不伪称同一二进制通过完整矩阵。
- `g18-incomplete-checkpoint.json` 汇总九个历史点、五个负例、三次 I/O 失败及 producer hashes，明确 `g18ExitCriteriaMet=false`。生成时重新读取各 PID/exit/marker、身份台账、restore 状态并流式复核全部成功输出 digest；`g18_collect_checkpoint.py` 和 `g18-collect-checkpoint.txt` 保留该只读核对过程。
- 生成的普通 `PBApplication` / `PBStorage` 项目中 `PB_PROCESS_FAULT_TESTS` 出现次数为 0，两个克隆库中非零；最终 worker 不依赖 Qt 或 D3D DLL。普通产品不会读取 G18 环境变量或主动终止。

未执行 full CTest、GPU/GUI/native/remote、20 GiB、独立产品 EXE 实屏恢复、断电或 release package 验证。这里的 base Git commit 是提交前构建基线；不能冒充提交后重建的产品嵌入身份。当前独立提交用于保存 G18 的实现与未关闭证据，不构成验收完成。

## 8. 原子替换失败的定向诊断（2026-09-04）

### 8.1 前置、范围与已确认的失败位置

重新读取 `AGENTS.md`、路线 G18/G19、设计中恢复/源不可变性约束，并检查 Git。G17 `805dd2a` 是当前诊断基线 `8965f3d` 的祖先；其 telemetry 12/4,381、report 11/478、cache 1/29 通过日志仍存在，没有重跑前置目标。进入本轮时仅有用户所有的 untracked `docs/PHASE1_GATE_REPORT.md`，本轮不读取其正文、不改写、不暂存。

生产调用顺序与 headless harness 相同：

```text
SenderFrameBuilder::Advance
  -> ActivateNextSegment / InitializeCurrentSegment / InitializeUnifiedScheduler
  -> EnsureRepairIdLease(next segment) -> PersistRuntimeState -> AtomicWriteFile  [已成功]
返回调用方
  -> UpdateCarouselPosition(next segment) -> PersistRuntimeState -> AtomicWriteFile  [Win32 5]
```

- 对应 `tests/PBApplication/process_recovery_runtime.inc` 的 `builder.Advance()` 后位置保存，以及 `apps/common/local_desktop_runtime.cpp` 中正式 Encoder 的同名调用；两次保存之间均没有 FPS 等待。headless 整体更快不是该双写次序的唯一来源。
- 两份尚未做后续恢复的 Encoder 状态重新验证了长度、PBER v2、CRC32C、SessionId：

| 原失败 | durable generation | durable ordinal | 已保存的下一段 repair lease | frame lease end | runtime SHA-256 |
| --- | ---: | ---: | ---: | ---: | --- |
| campaign-2 / lease-persisted | 61 | 27 | segment 28 = 8,192 | 12,288 | `a69952237ba55a072b70377f02e4bf6358b0fdd93709a84e641eb828192e6513` |
| final-prescan / encoder-prescan | 61 | 28 | segment 29 = 8,192 | 8,192 | `6c759e6cfc43a68991cc0c41bcbffd7236629e3b6a152c14ea221a7b915b0a18` |

因此失败不是本次下一段 repair lease 未落盘，而是随后 Carousel position 的替换。第一份 campaign-1 Encoder 状态早已被第 7.2 节额外恢复推进，不能把它当前的 generation 当成原故障快照，也不能把 Decoder journal 摘要冒充 Encoder 状态。

三份失败的 `frames.jsonl` 分别与既有成功用例的前 6,696 / 6,944 / 7,192 行完全相同。这只是身份/lease 元数据前缀，不代表所有运行时输入或环境相同。campaign-1 与 first-flow 的 worker SHA-256 相同（`333f4e23b8099e761562ccc4f8c3d27adbacfdc8d959a27909995300dc52efb8`），且均早于 I/O 失败观察器；没有证据支持“观察器新增后才引入故障”或“固定 generation 必然失败”。

源码与历史审计未发现该同步路径中持有 `runtime.state` 的自有读句柄或第二个 writer：startup 读取句柄已关闭，tmp 的 write/flush/close 均先于 rename，owner lock 是另一文件。原子替换与失败回滚源自 G01 `1445f9b`，整轮 repair lease 预留源自 G02 `766d210`；G15 `c5dc9da` 增加安全 tmp/owner 检查，G18 观察器只在失败后运行。这些是排查证据，不是对未捕获瞬态句柄/过滤器的排除证明。

### 8.2 低干扰跟踪与小探针结果

全部新增产物位于 ignored `build-unified-release/g18-atomic-diagnosis/`。CDB 只启动自己的 worker，使用 `CREATE_NO_WINDOW`、`-hd`、本地空符号路径及有界超时，不附加其他进程、不控制输入、不打开实屏。没有安装工具、修改 ACL/安全软件、增加重试或改动生产源码。

先检查当前系统实际服务的 `ntdll`/`KernelBase` 指令，再设置原生返回/错误分支断点。当前 `KernelBase.dll` 为 `10.0.26100.9278`，SHA-256 `86d70eb7d0f1f997bd965616405f166afcb56c20ac468e297b8b60f51cbb0193`；这些偏移不是跨系统版本 ABI。小探针 runner 在指纹不符时拒绝运行。其 `NtSetInformationFile` 返回值在系统库保存后被错误分支读取，避免将 HeapFree 后的 EAX 或普通 API 合成错误误当成原生 rename 返回值。

| 执行 | 结果 | 证据边界 |
| --- | --- | --- |
| `native-trace-1` | CDB 脚本语法错误，在 worker main 前退出 | MASM 的按位 `|` 被误写为 `||`；保留原日志，不计产品失败/运行通过 |
| `native-trace-2`，syscall 返回处跟踪 | 102 次 rename 全成功；worker 96,312 ms | 明显扰动时序；不是问题消失证明 |
| `native-trace-3`，仅系统库错误分支跟踪 | 无错误分支命中；worker 54,958 ms | 同一最终 worker 快照，未复现自然故障 |
| `probe-baseline`，无占用，1 组双写 | 成功，generation 5，10 ms | 小范围对照 |
| `probe-share-all`，持有目标读句柄，允许 read/write/delete sharing | 原生 `0xc0000022`，Win32 5；内存回滚和重新打开均正确 | **受控占用**，不是自然故障 |
| `probe-no-delete`，仅取消上述 delete sharing | 同样原生 `0xc0000022`，Win32 5；内存回滚和重新打开均正确 | 单变量差分；事后 DELETE-access probe 变为 error 32 |
| `probe-paired-burst`，无占用，错误分支跟踪 | 128 组双写全成功，generation 259，1,405 ms | 不是 FEC/256 MiB 闭环或验收 |
| `probe-paired-no-debugger`，仅去掉 CDB | 128 组双写全成功，exit 0，generation 259，1,552 ms | 未证明必须有 debugger 才能通过 |

两次成功的 256 MiB 诊断输出另做流式长度/SHA-256/BLAKE3 核对，与第 7.1 节源完全一致；见 `diagnostic-output-verification.json`。它们没有真实终止/重启，不替换九点验收记录，表中时长也不是吞吐认证。

小探针源码 `probe-source/encoder_atomic_replace_probe.cpp` 与独立 `CMakeLists.txt` 只在该 ignored 诊断目录创建；链接已存在的 `PBG18Application.lib`、`PBProtocol.lib`，**没有复制或重写 AtomicWriteFile 实现**。MSVC `/W4 /WX` Release 构建成功；最多 128 组，32 项修复租约元数据，create-only 目录，无控制/传输/FEC 数据流。用于构建 state-store 的 descriptor bundle 明确是 non-transfer 夹具，不能作为正式描述符或大文件证据。probe binary SHA-256 为 `748eb9fe8d3d835b8b11a291cea92fa75d4de05edc36093c06c2603dead614f6`。

受控 share-all 案例在**同一线程**记录：native rename status `0xc0000022`、错误映射输入 `0xc0000022`、已核对的 `MoveFileExW` 错误分支 EAX=5、应用 `UpdateCarouselPosition ... win32=5`。同时原观察器仍显示 target/tmp attributes 32、DELETE-access 0/0；区别是本次占用保持到观察结束，Restart Manager 能看到探针自身 PID 29452，历史自然故障观察时 PID 列表为空。因此 **DELETE-access 成功不能排除已有读取句柄，Restart Manager 的事后空列表也不能证明失败瞬间无占用**。该受控机制与 Windows 对替换已打开目标的限制一致；参见 [MS-FSA FileRenameInformation](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fsa/87f86c9b-6c2a-4803-84b7-131a74a434fa)。

原日志中的 `!gle` 因缺少 `ntdll!_TEB` 类型给出不可用的零值；不能引用它来否认应用 error 5。有效证据是保存的真实 NTSTATUS、上述已反汇编核对的 EAX、应用保存的 GetLastError。`!handle` 没有返回源句柄对象名称，不能伪称取得了完整源路径；目标路径来自长度受限的 `UNICODE_STRING`，未读取已释放的 rename buffer。

### 8.3 重放、封印与下一步

以下为本机诊断重放入口，必须使用新的 case 名，不能在原失败状态上继续推进。probe 来源和其依赖 static-library/EXE/script/DLL hashes 见每个 `.trace-run.json`；这些本地产物未进入源码提交，也不是可移植工具发布。系统 DLL 或工作库发生变化后须先重新核对，不能盲用旧偏移/身份：

```powershell
$diagnosis = '<repo>\build-unified-release\g18-atomic-diagnosis'
cmake -S "$diagnosis\probe-source" -B "$diagnosis\probe-build" -G 'Visual Studio 17 2022' -A x64
cmake --build "$diagnosis\probe-build" --config Release --target PBEncoderAtomicReplaceProbe --parallel 2
& <python> "$diagnosis\run_probe_trace.py" probe-new-share-all share-all 1
& <python> "$diagnosis\run_probe_trace.py" probe-new-no-delete no-delete 1
& <python> "$diagnosis\run_probe_trace.py" probe-new-paired-burst none 128
& "$diagnosis\probe-build\Release\PBEncoderAtomicReplaceProbe.exe" none "$diagnosis\probe-new-no-debugger" 128
```

本轮两次有效完整 worker 诊断分别由 `run_native_trace.py native-trace-2 trace-ret-only-v2.cdb` 和 `run_native_trace.py native-trace-3 trace-errors-only.cdb` 执行。该旧 runner 没有 DLL 指纹前置断言，不应在不同系统上直接重用。`collect_diagnosis.py` 仅核对既有状态、元数据前缀和受控日志并封印 SHA-256；汇总 `diagnosis.json` 明确记录：

```text
naturalFailureRootCauseConfirmed = false
productionFixApplied = false
g18ExitCriteriaMet = false
```

**当前最值得验证的假设：** rename 目标在失败瞬间被短暂打开或仍有文件系统/过滤器引用。受控复现证明该机制可产生已有的 error/probe 特征，但尚未将它与三次历史自然失败连接；不能认定是安全软件、索引器、系统缺陷或自有句柄错误。另一尚未排除的类别是过滤器直接拒绝 rename，而非某个普通可枚举用户态句柄。

**下一证明步骤，而非修复：** 在一次自然失败附近取得同一 `runtime.state`/`.tmp` 的 CreateFile、Cleanup/CloseFile、SetRenameInformationFile 时间线、share/access、PID 与调用栈。推荐用户配合一次最多 90 秒、仅该诊断夹具路径的管理员文件 I/O 跟踪；[Process Monitor](https://learn.microsoft.com/en-us/sysinternals/downloads/procmon) 支持文件操作明细、过滤和调用栈。不能只按 worker PID 过滤，否则会丢掉其他进程的占用事件；不能只保留 error，否则会丢掉成功的 open/close。启动/配置该权限更高的跟踪前须取得用户配合，不自动安装驱动、接受 EULA、提升权限或改变安全软件。

当前会话不是管理员，`fltmc filters` 一次返回 access denied；PATH 未发现 Procmon 命令，这不代表整台机器未安装。未启动全局 ETW/Procmon，也未干扰左右屏幕或输入。普通权限下的双写探针已有带/不带 debugger 的有界对照，不继续无证据重复完整 campaign。等待捕获自然失败的必要证据后再确定最小修复；不采用未经归因的 sleep/retry、合并持久化事务或更换 rename API。

本轮只构建并执行上述诊断探针、两次有效诊断闭环及一次 main 前脚本失败，复核既有输出/状态；没有重跑 ASan、full CTest、GPU/GUI/native Gate、remote、20 GiB 或发布包验证。生产代码、公共接口、wire/FEC、持久化格式和 fail-closed 行为保持 `8965f3d` 不变。下一目标仍是 **关闭 G18 的自然原子替换故障及最终候选验收**，不是 G19。

## 9. 用户要求的追加复测：三次通过后再次自然失败（2026-09-04）

用户明确要求再测几次，如果这几次均不再发生，则忽略该失败。本轮先执行三次独立的 `encoder-prescan` 终止/重启，再补齐 G18 既定的其他终止点。前三次成功后，后续第一项 `lease-persisted` 就再次发生相同错误；因此本轮并非全部未复现，不能把该自然失败改判为通过，也不应用条件性忽略。

### 9.1 固定 producer 与结果

- 已重新读取 AGENTS/G18 范围，核对 G17 祖先提交及 telemetry/report/cache 通过日志；不重跑 G17。
- 四次均使用原最终失败快照 `g18-worker-final-snapshot/PBProcessRecoveryWorker.exe`，SHA-256 `3d7d269fe726281422c7cc2d6d7e7a9265d619e7b7241f11cfb2314940639682`。
- `original-candidate-comparison.json` 重新核对了原失败 provenance 中十个源码/脚本文件的 SHA-256，均与当前文件一致；没有重建成另一 worker 后再声称复现或消失。
- 同一 256 MiB CSPRNG fixture，每次独立 create-only 状态目录；串行、无 debugger、无人为文件占用、无额外 sleep/retry、不使用管理员采集。实际终止进程均有 exit 218 和匹配的 durable marker，随后创建新的恢复进程。

以下路径均相对于 `build-unified-release/g18-user-recheck-20260904/`：

| 路径 / 测试 | 重启 PID | 重启结果 | 重启到发布或失败 / 秒 | combined 峰值 / MiB |
| --- | ---: | --- | ---: | ---: |
| `attempt-1/r1-encoder-prescan` | 14240 | exit 0，32 段发布与 final reopen | 55.079 | 85.35 |
| `attempt-2/r1-encoder-prescan` | 46124 | exit 0，32 段发布与 final reopen | 57.859 | 85.41 |
| `attempt-3/r1-encoder-prescan` | 55092 | exit 0，32 段发布与 final reopen | 59.531 | 84.99 |
| `final-other-points/r1-lease-persisted` | 42612 | **exit 1，atomic replace Win32 5** | 20.406 | 85.37（截至失败前的观测值） |

前三次输出均为 268,435,456 bytes，外部 SHA-256/BLAKE3 与第 7.1 节源一致。预扫描终止时尚无 durable Session，重启未误采用未完成的预扫描；每次恢复后的 7,895 个 FrameSequence、39,595 个预留 repair ID 均独立检查唯一性及 lease 边界。这里 39,595 是交付前台账，实际交付 repair count 为 39,588：最后一帧预留 8 个、交付 1 个后整文件完成，余下 7 个没有交付，仍视为已消耗的身份，不能混用两个计数口径。

### 9.2 本次失败现场

```text
G18 Carousel position persistence failed: session state atomic replace failed; win32=5
```

- 故障发生在真实 `lease-persisted` 终止后的恢复阶段，恢复进程 `selectedCrashPoint` 为空，不是计划中的 exit 218 注入。
- Decoder journal 验证长度、framing 和 CRC 后，确认已 durable completed 的 ordinal 为 0..11，共 12 段；没有最终 `fixture.bin`、没有成功 `result.json`、没有整组 PASS 报告。`.part` / `.resume` 保留，未尝试额外恢复。
- Encoder `runtime.state` 为有效 PBER v2、236 bytes、generation **28**、pass 0、ordinal **11**、frame lease end 8,192；下一段 ordinal 12 的 repair lease 8,192 已保存，ordinal 13..31 仍为 0。仍是 repair lease 成功后的位置保存失败。
- runtime SHA-256：`a47893392ad6b3fad7cd36ac5a09a08bbbed03324767216941db7bde9b4609e0`。与先前约 47–51 秒、27–29 个 durable Segment 后的失败相比，这次在 20.406 秒、12 段、generation 28 就发生，不能再将问题限定于接近文件末尾或 generation 61。
- 事后观察器仍是 `moveError=5`、target/tmp attributes 32、DELETE-access 0/0、Restart Manager 成功但 PID 列表为空；根因/瞬时占用者仍未确认，不能凭空归因给安全软件。
- 失败前已经生成的部分身份台账通过检查：同一 Session，重启跳过 frame lease 4,096；重启前 1 帧、重启后 2,976 帧，FrameSequence 和 repair ID 没有交叉复用。这不是最终文件成功证据。

原始状态保持不变，另外用 create-only 文件封印 `frozen-failure-metadata/encoder-runtime.state`、`decoder.resume` 和 journal 检查摘要，避免将来继续恢复后丢失原故障快照。旧的三次失败、三次新增成功和本次失败分别保留，不覆盖旧汇总。

### 9.3 命令、核对与停止边界

前三次使用现有 harness 默认单次运行，分别传入 `attempt-1`、`attempt-2`、`attempt-3`，没有修改仅允许 `--repeat 1/2` 的脚本：

```powershell
$root = '<repo>\build-unified-release\g18-user-recheck-20260904'
foreach ($attempt in 1..3) {
    & <python> tests\PBApplication\run_process_recovery.py `
      --worker build-unified-release\g18-worker-final-snapshot\PBProcessRecoveryWorker.exe `
      --run-directory "$root\attempt-$attempt" `
      --fixture-manifest build-unified-release\g18-first-flow\fixture-manifest.json --points encoder-prescan
    if ($LASTEXITCODE -ne 0) { break }
}
```

随后实际执行的命令为 `--run-directory "$root\final-other-points" --points lease-persisted encoder-mid-segment decoder-active part-flushed completed-record whole-digest before-rename after-rename --negative-checks`，worker/fixture 与上面相同。**首项失败后立即退出，另外 7 个终止点及 5 项负例均未开始**。若再次复现，应新建目录并只指定 `--points lease-persisted`，而不是继续推进本次原始失败状态。

`collect_recheck.py` 只复核已结束进程的日志、三份最终输出、身份台账和失败状态，并生成 `recheck-summary.json`。首次离线汇总错误地要求“预留 repair ID 数 = 实际交付数”，已依据先写整帧台账、最终帧内完成即停止的实际代码和 7 个未交付 ID 修正口径；原 collector 和断言记录另存，未修改生产代码、harness 或任何测试结果，也没有为此再跑 worker。

汇总明确 `naturalFailureReproduced=true`、`allRechecksPassed=false`、`ignoreFailureConditionMetForWholeRecheck=false`、`g18ExitCriteriaMet=false`、`productionFixApplied=false`。本轮结论是**故障仍会发生，但不是每次发生；具体根因仍未知**，不是从这四次不同注入点的样本估计发生概率。

没有继续试到通过，没有实现重试/延时/API 替换，没有启动 Procmon/ETW，也没有运行 ASan、full CTest、GPU/GUI/native/remote、20 GiB 或发布包验证。本轮源码提交仅更新 G18 文档与状态，`docs/PHASE1_GATE_REPORT.md` 未改动、未暂存。下一目标仍是 G18 的自然拒绝来源诊断与最小修复决策，G19 未开始。
