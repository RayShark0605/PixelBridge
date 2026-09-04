# G18：256 MiB 真实进程终止与恢复

> 状态：实现与定向证据已整理，但 **G18 未通过最终验收，不得进入 G19**（2026-09-04）。
> 基线为 `805dd2a5a8b8f937f56287e4e030ff6f50860074`（G17）。最终候选复核再次出现 Encoder `runtime.state` 原子替换 `win32=5`；原因未确认，待用户同意扩大定向诊断范围。
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

已经向用户询问是否允许扩展为原子替换失败的定向诊断，并在原因确认后再确定最小修复；答复前停止依赖该决策的实现、进一步故障 campaign 和 G19。

### 7.3 夹具修正与证据封印

- 最初 quota writer 尝试将 cap 设为 5，被 `DecoderResumeStore::Open` 的现有配置校验提前拒绝。修正为默认 cap 下的四缓存种子和派生第五缓存，**没有放宽生产校验**。原失败日志 `g18-final-negatives.txt` 保留。
- `after-rename` 子进程实际已成功发布，旧 supervisor 却错误要求普通 `RestoreResumeState` 的 `restore.json`。既有 G05 `CompleteRecoveredPublish` 直接校验完整 intent/manifest 和已经 rename 的文件，不走 active replay。修正 verifier 要求 32 个 durable completed、32 个 restored verified、final reopen、零 frame/Transport replay 和外部三个文件判据；不改生产分支。使用已完成进程证据重新核对，`g18-campaign-3/failure.json` 仍保留原 assertion 失败。
- worker 初始构建的 wchar narrowing、Restart Manager const pointer-array 两个编译问题均已修正，最终 `g18-quota-fixture-build.txt` / `.exitcode` 为成功；没有降低 warnings 或测试判据。
- `g18-worker-matrix-snapshot/` 保留追加 I/O 观察后、修正 quota 前的 worker 与其非系统 DLL；worker SHA-256 为 `dd8987603ca10682629ed7850cd56d03045817e767887b3643b239f55a0bec5d`。
- `g18-worker-final-snapshot/` 保留最终 worker 与非系统 DLL；worker SHA-256 为 `3d7d269fe726281422c7cc2d6d7e7a9265d619e7b7241f11cfb2314940639682`。两个阶段之间 C++ 差异只在 quota fixture 分支，Python 另有 verifier/负例子集选项修正；仍明确分别记录 producer，不伪称同一二进制通过完整矩阵。
- `g18-incomplete-checkpoint.json` 汇总九个历史点、五个负例、三次 I/O 失败及 producer hashes，明确 `g18ExitCriteriaMet=false`。生成时重新读取各 PID/exit/marker、身份台账、restore 状态并流式复核全部成功输出 digest；`g18_collect_checkpoint.py` 和 `g18-collect-checkpoint.txt` 保留该只读核对过程。
- 生成的普通 `PBApplication` / `PBStorage` 项目中 `PB_PROCESS_FAULT_TESTS` 出现次数为 0，两个克隆库中非零；最终 worker 不依赖 Qt 或 D3D DLL。普通产品不会读取 G18 环境变量或主动终止。

未执行 full CTest、GPU/GUI/native/remote、20 GiB、独立产品 EXE 实屏恢复、断电或 release package 验证。这里的 base Git commit 是提交前构建基线；不能冒充提交后重建的产品嵌入身份。当前独立提交用于保存 G18 的实现与未关闭证据，不构成验收完成。
