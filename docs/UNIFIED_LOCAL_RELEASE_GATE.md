# G20：本地 Release Gate 进度与回归修复

> **状态：G20 native 验收进行中。** 2026-09-04 经用户明确批准，在冻结提交 `e0729b2` 上追加的一次完整 Release CTest 已 **218/218 通过**，旧 206/215 失败记录保留，见第 12 节。此前 Replay、Golden/ProtocolDump、Capture、shader 编译开销、application 和 sampler 修复已纳入本次整套验证。CTest 层无失败/跳过，但 application 内一个依赖外部封存 Replay 的 case 因未设置数据根目录而跳过，不能声称全部内部 case 都执行。用户现已明确决定：先验收现有右屏 2560×1440 的可执行项目，全部通过后不验证 2.0x、关闭 G20 并进入 G21；2.0x 只能记录为用户豁免且未验证，不能标为通过。下文第 1–13 节保留各阶段当时的状态，不用新决定改写历史。

## 1. 前置与本轮边界

- 前置 G19 提交为 `8c7cab504668dcd7ae05e4564ba09730116f5666`。开始时已验证该提交是 HEAD、G19 report 的 2,561 Segment/publish/final reopen/全字节比较为真，12 个 source seals 与候选 worker SHA-256 匹配；没有重跑 20 GiB harness。
- 开始时唯一外部文件为 untracked `docs/PHASE1_GATE_REPORT.md`。其 SHA-256 为 `076ef4c9b9f89eabccd323dbe4bffc4dc125ddaf96e6ee437d2cf5b1b1cea306`，没有修改或暂存。
- 本轮仅 G20 的构建、回归诊断、最小修复及证据文档；不改 wire、公共接口、Golden bytes/pins、默认资源策略、GUI 交互或屏幕设置。
- 所有 displayful Gate cache 均 OFF。普通 Release 树关闭 G18/G19 私有终止构建，未运行 crash/20 GiB matrix；没有操作鼠标键盘或打开真实数据窗口/选区器。

## 2. 首次 Release 构建与完整 CTest（历史失败）

```powershell
cmake -S . -B build-unified-release -DPB_BUILD_PROCESS_RECOVERY_HARNESS=OFF
cmake --build build-unified-release --config Release --parallel 6
ctest --test-dir build-unified-release -C Release --output-on-failure --parallel 2 `
  --output-junit g20/ctest-full.xml
```

首次配置成功，构建在 `test_pbgolden_vector.cpp` 失败：旧测试将 `GetSerializedSize(SessionDescriptor)` 返回的 `ProtocolResult<size_t>` 直接传给 vector constructor。只改为先 REQUIRE 成功再 `.Value()`，保持原 Golden 字节比较断言；增量全构建成功。这只是解除编译阻塞，不代表旧 Golden 的 schema 适配已完成。

本节对应首次完整 CTest：215 项，206 passed、9 failed，exit 8，868.62 秒。当时没有重新启动整套测试；原始输出与 JUnit 保留。用户后来批准追加的一次整套运行单独记录于第 12 节，不覆盖本节历史结果。

| 失败项 | 原始结果 | 当前处理 |
| --- | --- | --- |
| PBApplicationTests | SIGSEGV / Stack overflow | 第 3/9 节关闭栈溢出与启动期限；第 10 节关闭另两项已知断言，新增预算/早完成回归；未重跑完整模块 |
| PBCapturePipelineTests | 两个 LF4 case 的 60 秒 Await 失败 | 第 8 节以 D3D debug error 定因并修复非法 structured UAV clear；4 cases / 2,705 assertions 通过，原期限不变 |
| PBDemodD3D11Tests | 180 秒外层 timeout | 第 9 节构建期预编译后 22 cases / 663,674 assertions 全过，8.110 秒；不增加时间上限 |
| PBRemoteVisualGpuParityTests | 300 秒外层 timeout | 第 8 节只排除另有独立注册的 Unified G12；legacy 1 case / 46,731 assertions、73.203 秒通过 |
| PBGoldenVectorTests | 旧 descriptor/control/fragment 期望冲突 | 第 7 节分层修复后定向通过，14 cases / 57,902 assertions |
| PBGoldenVectorCheck | 8 个旧 pin 与重算不符 | 第 7 节恢复历史重算后通过；没有修改文件或 pin |
| PBGoldenVectorCheckTests | 同上 8 项导致 summary 失败 | 第 7 节定向通过，7 cases / 120 assertions |
| PBProtocolDumpTests | 字段偏移、错误名与旧变异位置不符 | 第 7 节正式 schema 适配后通过，8 cases / 2,380 assertions |
| PBVectorGenIntegration | Golden 重算与旧 pin 不符 | 第 7 节定向通过，重新生成/重放/冲突拒绝均保留 |

正式 `PBProtocolTests`、`PBUnifiedVisualGpuParityTests`（159.08 秒）、`PBUnifiedVisualHardwareSmokeTests`（25.17 秒）及两项 Qt offscreen GUI smoke 已通过；不能把这些局部成功扩大为 full CTest 或 native Gate 通过。GUI smoke 已包含在 full CTest，不再重复。

### 历史 Golden 的明确处理边界

`PROTOCOL_1_DESCRIPTOR_SCHEMA.md` 第 9 节规定旧 37/110/142/65-byte descriptor 是 Phase-0 历史 fixture，正式 parser 只承诺 `UnsupportedDescriptorSchema`。本次旧 67-byte Control 的重算结果变为 115 bytes，BLAKE3 恰好等于 G01 manifest 的正式 accepted ControlSessionDescriptor；这是工具混用了两代合同，不是修改旧文件的理由。

第 7 节按此边界完成修复：保持旧 `.bin`/pin/函数签名不变，历史重算器使用明确的 private legacy byte source；正式成功解析/往返继续由正式 corpus 证明。Dump 的字段表与测试变异位置按正式 offset 更新，没有启用猜测式旧布局兼容解析。

## 3. 已定位的生产离线 Replay 栈溢出

### 原始动态证据

用同一个失败 Release EXE 在独立进程中只运行 `[remote-refinement]`，保持原 seed `178638649`、原 1 MiB PE stack reserve，不调大栈。CDB 捕获到 worker thread 的 first-chance **`0xC00000FD`**；dump 离线反汇编把上层地址对应到 `CompleteInternal` 的 `mov eax,64BE8h; call __chkstk`。Catch2 输出中的 423 行只是主线程轮询时的当前断言，不是故障位置。

- 原 EXE SHA-256：`cdf356a071b86d42b57b8893a8094cd35f8e99efb5d413a2ccfbbf37f8fec8c3`。
- CDB supervisor exit 0 表示成功抓取后退出 debugger，**不表示原失败测试通过**。
- Release 无 PDB；函数定位来自 dump 的真实 RVA/prologue 与现存 `.obj` 符号化反汇编相互核对，不冒充源码行级动态符号栈。

### 两轮修复的固定栈预留差分

| Release 嵌套函数 | 原始 bytes | 第一轮双缓冲 bytes | 第二轮 run owners bytes |
| --- | ---: | ---: | ---: |
| DecoderRuntime::Run | 189,456 | 189,456 | 6,240 |
| RunOfflineReplayDataset | 209,728 | 3,936 | 3,936 |
| RunReplayProductionDemod | 309,680 | 616 | 616 |
| CaptureDemodulator::CompleteInternal | 412,648 | 412,648 | 412,648（未改） |
| Demodulator::PollInternal | 209,392 | 209,392 | 209,392（未改） |
| 五层合计 | **1,330,904** | **816,048** | **632,832** |

这不是运行时最大栈的完整上界，尚不含调用框架、外部 driver 等；但修复前仅前四层就超过 1,048,576-byte reserve，动态异常确认了实际溢出。`CaptureDemodulatorResult` 已随 Unified 扩展到 103,024 bytes，旧 Profile 也承担该按值对象的完整大小。

第一轮 Release 已不再崩溃，但 ASan 更大的栈帧在 `PollInternal` 再次溢出：`CompleteInternal` 为 761,768 bytes，第一轮五层合计 1,178,328 bytes。第二轮将 ASan 的 `Run` 从 195,920 降至 13,040 bytes，五层合计降为 995,448 bytes；仍不是所有外部调用的最大栈证明，须以具体路径的运行结果验收。Release/ASan 均保持原 1 MiB PE reserve，未用 `/STACK` 掩盖问题。

### 最小修复

栈修复只改 `apps/common/local_desktop_runtime.cpp` 的私有实现：

- helper 不再按值返回大型结果；使用调用方的两个结果缓冲。
- `RunOfflineReplayDataset` 与 LF4 Replay probe 各在循环外一次分配固定双缓冲，逐帧复用；没有新增逐帧 allocation。
- `DecoderRuntime::Run` 的 `ReceiverPipeline` 改为 run-owned owner；声明顺序保证其在引用的 Receiver 之前析构。
- live result 改为循环外一次分配的 run-owned 缓冲；原来即使走提前返回的 offline 分支，也会为该大型栈变量保留整个函数的栈空间。
- 保留 0/多结果拒绝、两阶段 GPU retirement、capture epoch、temporal admission、Receiver/digest/publish 逻辑。
- 不修改公开 result struct、CaptureDemodulator/LDPC、栈 reserve、wire 或任何超时。allocation failure 仍进入既有异常/失败路径。

第一轮定向重建 `PBApplicationTests` 与两个 Qt EXE 成功。只重跑 `PBApplicationTests` 模块（不是 full CTest）：133.84 秒，103 cases，98 passed / 4 failed / 1 skipped，77,798 assertions 中 4 失败；该次 Release **不再 stack overflow**。原 duplicate-refinement 和 LF4 live/offline publish 用例不在失败项中，但模块仍不能声称全绿。

该次模块剩余结果：

1. `Production LF4 Receiver admits bounded partial codewords...` 的 probe final snapshot 非 authoritative。
2. neutral Replay 和 clean RemoteVisual confidence 两例超过各自原 **15 秒**等待；duplicate-refinement 原预算为 30 秒。
3. Direct/Shape legacy Replay fan-out 的 `ValidateDecoderConfig` 失败。
4. 既有 sealed real corpus 因未设置 `PB_REMOTE_VISUAL_REAL_REPLAY_ROOT` 显式 skip；没有伪造该外部资产或将 skip 当作真实数据集覆盖。

第二轮分别重建 Release 的应用测试/两个 Qt EXE 和 ASan 应用测试成功。原 Release `[remote-refinement] --rng-seed 178638649` 精确重放通过：1 case / 23,021 assertions，24.271 秒。ASan `[remote-refinement],[application][g16]` 为 5 passed / 1 failed；没有再报告栈溢出，但 Replay 超过原 30 秒，因此当时不能认定整个失败已解决。G16 Stop/fallback/restart/confirmation/mixed handoff 保持通过。

## 4. 定向 ASan

使用独立 `build-unified-asan`，只构建 Protocol/Receiver/Storage/Application 四个测试目标；不运行 full ASan CTest 或 fuzz workload。四个 EXE 均验证导入当前 MSVC 的 `clang_rt.asan_dynamic-x86_64.dll`，运行使用 `ASAN_OPTIONS=halt_on_error=1`。

```powershell
cmake -S . -B build-unified-asan -DPB_BUILD_PROCESS_RECOVERY_HARNESS=OFF
cmake --build build-unified-asan --config RelWithDebInfo --target `
  PBProtocolTests PBReceiverTests PBStorageTests PBApplicationTests --parallel 6
```

以下各 selector 使用 `--rng-seed 20092026 --durations yes`，全部 exit 0；后续 shader-only 变化没有重复这些不受影响的组：

| EXE / selector | cases / assertions | wall seconds |
| --- | ---: | ---: |
| PBProtocolTests `[pbprotocol][descriptor],[pbprotocol][bootstrap],[pbprotocol][control],[pbprotocol][transport],[pbprotocol][resume-state]` | 93 / 32,080 | 0.187 |
| PBReceiverTests `[resume-replay],[resume-state-file]` | 13 / 243 | 0.094 |
| PBStorageTests `[storage]` | 17 / 219 | 0.172 |
| PBApplicationTests `[application][decoder][resume],[application][encoder][durable-lease]` | 10 / 532 | 1.375 |

原始 ASan `summary.json` 的 `allPassed=false` 保持不变，因为其中还包括第一轮修复后仍发生 stack overflow 的 Replay。第二轮无溢出但 deadline 失败的结果也单独保留，不能用后续结果覆盖中间红灯。

### 4.1 独立的 shader 编译开销

第二轮失败后只观察原 ASan 二进制的阶段：`DecoderRuntime::Run → DomainStarted → D3DCompile`。在首帧 `RunReplayProductionDemod` 前，`Demodulator::Create` 无条件编译全部 12 个 shader；旧 RemoteVisual Replay 同样承担 Unified shader 的成本。CDB trace 中 `EvaluateUnifiedPhaseCS` 编译耗时 32.804 秒，首帧解调到约 50 秒才开始。这是调试器下的定位证据，不作为无插桩性能结果；最初 trace 因 ASan first-chance 异常停住而未到测试路径，该次尝试没有被当成复现通过。

随后使用当前系统 `d3dcompiler_47.dll` 做**无 debugger、无 GPU**的单变量实验，沿用生产 `cs_5_0`、入口与 flags `264192`。只将 phase 内层固定 16 次循环的 `[unroll]` 换成 `[loop]`：原版 17.578 秒、28,804-byte bytecode；候选版 0.360 秒、6,476-byte bytecode；HRESULT 均为 0，diagnostics 均为空。原始/派生 HLSL、CSO 与 compiler/source SHA-256 分开保留。

生产仅修改 `unified_visual_compute.hlsl` 的这一编译属性及解释性注释：循环仍有固定 512×16 上界，采样顺序、距离累加、布局、阈值、entry point、编译 flags 和 shader 列表不变，不增加 profile fallback 或跳过任何解码检查。编译实验本身不证明 GPU 语义，因此还需受影响 Replay 和 Unified parity 的无 debugger 复验。

### 4.2 最后一次受影响复验

定向重建 Release 的 `PBApplicationTests`、`PBDemodD3D11Tests`、两个 Qt EXE，以及 ASan 的 `PBApplicationTests` 均成功。没有再运行 full CTest 或完整 PBApplication 模块。

```powershell
# Release：恰好三个 offline D3D11 Replay case
& .\build-unified-release\tests\PBApplication\Release\PBApplicationTests.exe `
  '[application][replay][offline][d3d11]' --reporter console --durations yes --rng-seed 178638649

# ASan：相同三个 case，保留原 15 / 15 / 30 秒期限
$env:ASAN_OPTIONS = 'halt_on_error=1'
& .\build-unified-asan\tests\PBApplication\RelWithDebInfo\PBApplicationTests.exe `
  '[application][replay][offline][d3d11]' --reporter console --durations yes --rng-seed 20092026

# 仅复验改变过的 Unified shader 对应的现有两个 Gate case
& .\build-unified-release\tests\PBDemodD3D11\Release\PBDemodD3D11Tests.exe `
  '[.unified-g12]' --reporter console --durations yes --rng-seed 12122026
```

实际由 create-only Python supervisor 启动 hidden child，不改变父进程环境；每组先 `--list-tests` 核对 3/3/2 个 case。原 G12 固定文件名报告先备份到 `pre-phase-loop-reports/`，没有覆盖唯一原证据。

| 最终受影响结果 | cases / assertions | 结果与时间 |
| --- | ---: | --- |
| Release offline Replay | 3 / 23,036 | exit 0，总计 24.156 秒；refinement 7.781 秒，neutral 8.113 秒，confidence 8.216 秒 |
| ASan offline Replay | 1 passed / 2 failed；24,686 assertions 中 2 失败 | exit 42，总计 60.093 秒；**原 stack-overflow refinement 通过，20.147 秒**；另两例仍超过原 15 秒期限；无 ASan error 报告 |
| Release Unified parity + publish | 2 / 32,989 | exit 0，总计 61.719 秒；mandatory corpus 42.509 秒，final publish 19.178 秒 |

Unified mandatory corpus 的 WARP、一个当前 AMD、一个当前 NVIDIA adapter 各 18 个场景，最终 CPU/GPU accepted bytes 一致；false accepted、truth mismatch、conflict output 均为 0。两个 WARP OS-CSPRNG/RAW 文件仍经 whole-file digest、安全发布、final reopen 后才开放指标：Base-only `262,144 / 13 = 20,164.923 B/unique frame`，clean `262,144 / 7 = 37,449.143 B/unique frame`，分别满足 16 KiB 硬门槛和 32 KiB 工程目标。该复验由本轮 shader 变化触发，不是重复未改变的完整检查点，也不是 DISPLAY2 实屏证据。

原栈溢出用例已形成原 Release 动态红灯 → 当前 Release/ASan 原断言绿灯的闭环；不能据此宣称所有 worker 路径、整个 ASan 模块或 G20 已通过。两个 15 秒 ASan deadline 失败继续留在台账，未删除/跳过测试，也未放宽期限。`phase-loop-summary.json` 因 ASan 组失败而保持 `allPassed=false`；随后独立执行的 Unified parity 使用 `phase-loop-unified-parity.json` 记录，不能把这个独立绿灯写回覆盖原 summary。

## 5. 实屏硬约束：待用户决策

由普通 Decoder 的只读 `--list-monitors` 刚枚举到：

| identity | physical rect | physical resolution | DPI / refresh |
| --- | --- | --- | --- |
| `\\.\DISPLAY1` | `[0,0,2560,1440)` | 2560×1440 | 96 / 180 Hz |
| `\\.\DISPLAY2` | `[2560,0,5120,1440)` | 2560×1440 | 96 / 180 Hz |

固定 1920×1080 canonical raster 的 2.0x 需要至少 3840×2160，因此当前右屏无法在完整包含窗口/ROI 的硬约束下执行该项。已经向用户询问处理方式；未改变显示模式、跨越 DISPLAY1、用离屏结果替代该实屏要求或自行豁免。

native 1/15/60 Hz、0.75x/fractional/letterbox、2.0x、低于阈值暂停恢复均未执行。依赖屏幕决策的工作暂停，但可继续解决上述独立回归。G21、G22 尚未开始。

## 6. 本地证据与后续

`build-unified-release/g20/` 中保留：

- `preflight.json`、`monitors.json`：G19 前置/source seals/保护文件与本次 monitor identity。
- `configure.*`、`build.*`、`build-after-golden-fix.*`：首轮编译失败和最小编译修复后的 build。
- `ctest-plan.json`、`ctest-full.txt/.xml/.exitcode`、`ctest-full-summary.json`：唯一 full CTest、9 项失败。
- `prior-fixed-reports/`：full CTest 前备份的 G12 固定文件名报告，未用新结果覆盖唯一原证据。
- `stack-before.txt`、`cdb-stack-before.*`、`run_cdb_before.py`、`stack-before.dmp`、`stack-before-function-map.txt`：原二进制静态/dynamic 红灯；dump 为本测试进程，保持本地。
- `stack-after.txt`、`build-replay-scratch.*`、`ctest-application-after.*`：第一轮编译栈差分和模块结果。
- `stack-final-release.txt`、`stack-final-asan.txt`、`build-worker-owners.*`、`replay-final-*`：第二轮 run owners 的编译差分、Release 精确绿灯和 ASan deadline 红灯；文件名中的 final 不表示 G20 通过。
- `cdb-replay-timing*`、`run_cdb_replay_timing*.py`：第二轮失败后的只读阶段追踪，包含失败的首个诊断尝试。
- `compile_phase_pair.py`、`phase-compile-pair.json`、`phase-original-unroll.*`、`phase-candidate-loop.*`：离线编译器单变量实验，不混淆为像素验收。
- `build-phase-loop.*`、`verify_phase_loop.py`、`verify_phase_loop_parity.py`、`phase-loop-*.list.txt/.txt/.json`、`pre-phase-loop-reports/`：最终受影响构建、精确 selector、无 debugger 复验及覆盖固定报告文件前的备份。
- `final-evidence-audit.json`：最后的 source/executable seals、结果聚合、固定报告副本与保护文件核对；不是新增测试，也不把现有红灯改成绿灯。

`build-unified-asan/g20/` 保留 configure/build/imports、四组窄范围通过结果、第一轮 `replay-stack.txt` 的符号化 ASan stack-overflow、第二轮 `build-worker-owners.*` 和 shader 修改后的 `build-phase-loop.*`。

**下一目标仍是继续 G20，不是 G21**：先完成剩余回归的窄定位/修复和定向复验，并等待实屏 2.0x 决策。禁止重复 full CTest、静默更改旧 Golden 或放宽门禁来关闭目标。本节首轮 build 嵌入当时的父提交 `8c7cab5`，不声称提交后包身份复验。

## 7. 后续定向闭环：Golden / ProtocolDump（2026-09-04）

### 前置与原因确认

从栈/shader 修复提交 `e5f7d9d5e677cf25c514ff1f2d633e891401c592` 继续，重新检查 AGENTS、G20 前置/退出和 Git 状态。G19 仍为 ancestor，原 12 个 source seals 按 G19 提交内容复验，候选 worker 与报告 SHA-256 匹配。没有因后续 runtime 的合法修复而要求旧 G19 source seal 匹配当前 HEAD，也没有重跑大文件 harness。

修复前现有 `PBProtocolDump.exe` 的实际行为确认两处独立问题：

1. G01 的 85-byte 正式 Session 返回 `Success`，但仍把 offset 0/2 当 ProtocolMajor/Minor、offset 4 当 SessionId、offset 20 当 FileSize；例如打印 `FileSize=0f0e0d0c0b0a0908`，而真实文件大小是 offset 36 的 64。
2. 将同一 Session 的 `payload.bin` 改为 `a.bin` 并同步 length/CRC，得到合法的 79-byte context；同一个正式 DirectRepeat Segment 在旧 CLI 下 exit 2，错误要求 context 必须恰好 85 bytes。该尺寸只是常用 fixture，不是 schema 的变长上限。

原 EXE SHA-256 为 `26068530fca4f5208dbf7db08a4a756042d22ecf69d9275711f0b61821d15615`。旧 Golden 重算失败的原因仍是第 2 节已确认的新 serializer/旧 pin 混用，不是 fixture 损坏。

### 最小修改

- `PBGoldenVector` 仅给四个历史 payload generator 增加显式 LE byte source；原函数签名、37/110/142/65-byte 内容及全部 pin 保持不变，Control/Fragment envelope 继续复用原 serializer。生产 Descriptor codec 没有改动，也没有新增旧布局解析入口。
- Golden 测试分开证明历史 bytes/精确拒绝与正式值的 serialize/parse round-trip。正式 accepted/rejected corpus 和 manifest 不变，不以删除失败断言或 re-pin 达到绿灯。
- Dump 使用正式 schema offset 常量显示 prefix、fixed fields、filename/TLV、Wirehair profile、inner CRC。字段体限制在声明的 CRC 边界内；截断、非法 total 或尾随字节不会被当成 variable field/profile。失败仍来自权威 parser，不让观察到的 CRC 结果替代实际错误分支。
- CLI 在 allocation 前使用 1,300-byte context 上限，74-byte 最小 envelope 下限后再交正式 parser；不再强制 85 bytes。补齐五个已存在的错误枚举名称，未改变 CLI 参数、exit 0/1/2 分类或公共函数签名。
- 新增唯一 79-byte CLI fixture `tests/tools/fixtures/formal-session-short-name.bin`，由 G01 Session 仅修改 filename/两个 length/CRC 得到，SHA-256 `aa3cfd41fb5dbf91eabd21c8046316ae8151462e3cd00bd922bb5ca2d28931da`。它是工具回归输入，不是替换或重新 pin 正式 Golden。

### 最小验证与结果

```powershell
cmake -S . -B build-unified-release -DPB_BUILD_PROCESS_RECOVERY_HARNESS=OFF
cmake --build build-unified-release --config Release --target `
  PBGoldenVectorTests PBGoldenVectorCheck PBGoldenVectorCheckTests PBVectorGen `
  PBFrameInspector PBProtocolDumpTests PBProtocolDump PBProtocolTests --parallel 6
ctest --test-dir build-unified-release -C Release `
  -R '^(PBGoldenVectorTests|PBGoldenVectorCheck|PBGoldenVectorCheckTests|PBVectorGenIntegration|PBProtocolDumpTests|PBProtocolDumpExit\..*)$' `
  --output-on-failure --parallel 1 --output-junit g20-tools/ctest-tools.xml
& .\build-unified-release\tests\PBProtocol\Release\PBProtocolTests.exe `
  '[pbprotocol][descriptor][corpus],[pbprotocol][descriptor][golden][manifest],[pbprotocol][descriptor][wire][formal][roundtrip]' `
  --reporter console --durations yes --rng-seed 20092026
```

构建成功，无 compiler warning/error；样式整理后仅增量重建 `PBProtocolDumpTests`，成功。实际由 hidden child supervisor 执行，运行前只做 CTest `-N` 和 Catch2 `--list-tests`，确认恰好 10 项 / 8 cases。首个 supervisor 将 Catch2 的 `8 matching test cases` 错认成 `8 test cases`，在任何测试执行前停止；保留该 preflight 失败记录，核对输出后只恢复尚未运行的测试，没有重跑已通过组。

| 检查 | 本轮结果 |
| --- | --- |
| 定向 CTest | **10/10 passed，exit 0，2.50 秒**；包含原五个失败项与五个 CLI 检查，不是 full CTest |
| PBGoldenVectorTests | 14 cases / 57,902 assertions，全通过 |
| PBGoldenVectorCheck | 25 个 file-backed + 5 个 frame pin，`total=30 failures=0` |
| PBGoldenVectorCheckTests | 7 cases / 120 assertions，全通过，保留负样本归因 |
| PBProtocolDumpTests | 8 cases / 2,380 assertions，全通过；覆盖正式 literal offsets、变长 name/TLV、所有 0..89 截断、CRC/total/name 错误、旧 schema 拒绝与 DirectRepeat 假尾部 |
| CLI | 原有成功/解析失败/usage 分类通过；79-byte context exit 0 且 `context=checked`；37-byte legacy context exit 2 |
| PBVectorGenIntegration | 相同输入重放 byte-identical，生成内容匹配原文件，冲突不覆盖；PBRW/PNG recovery 通过 |
| 正式 Protocol contract | **8 cases / 300 assertions，exit 0**；包括正式 manifest pin、独立 accepted/overflow/legacy corpus、正式 round-trip |

同输入再次运行两个修复前 CLI probe：正式 Session 现在准确输出 offset 36 的 `FileSize=0000000000000040` 与匹配的 inner CRC；79-byte context 由 exit 2 变为 exit 0 / `status=checked`。当前 Dump EXE SHA-256 为 `8451b577daf3b489eae11060785b20d031f70b7899abb41aa2389b63a75e4863`。

### 证据保护与剩余边界

`build-unified-release/g20-tools/` 保留 `preflight.json`、`dump-before.json` / `*-before.txt`、`build-tools-summary.json`、两次 selector 计划、`ctest-tools.xml` / `ctest-tools-last-test.log`、`formal-contract.stdout.log`、`verify-tools-resumed-summary.json`、`dump-after.json` / `*-after.txt` 与 `postflight.json`。后者固定 9 个代码/fixture source SHA-256、8 个 EXE SHA-256 和工作 diff；其 source base 是 `e5f7d9d` 加本轮修改，不冒充提交后的包身份认证。旧 generator integration scratch 在确认实际绝对路径位于本 build tree 后先备份到 `pre-tools-vector-gen-integration/`，再允许运行已有清理逻辑。

原 Golden、正式/历史 corpus、formal manifest 与 registry 共 **220 个受保护文件逐个 SHA-256 未变**。`docs/PHASE1_GATE_REPORT.md` 仍为原 SHA-256、untracked 且未暂存。唯一 full CTest 的 206/215 历史结果和各中间红灯不覆盖、不改写为全绿。

本轮没有 full CTest、ASan、GPU、Qt GUI、native、远程、20 GiB 或提交后安装包复验。五项工具失败已定向关闭；application/capture/demod 其他失败、两个 ASan 15 秒 deadline 和 DISPLAY2 2.0x 条件仍待处理。下一目标仍为 **继续 G20**。

## 8. Capture 首帧失败：非法 structured UAV clear（2026-09-04）

本轮从 `00045ed0130bcb5fe4dfa917e5642a73e3d0bf36` 继续，仅处理 G20 剩余本地回归。重新读取 AGENTS、G20 前置/退出与 Git 状态，复验 G19 ancestor、原报告、12 个提交级 source seals 和候选 worker。上轮是已有独立修复提交及通过证据的 progress，不是因实屏条件停滞的空转；G21/G22 未开始。

### 从综合 Await 失败到精确 D3D11 错误

只给 `PBCapturePipelineTests` 既有 LF4 首帧 60 秒 Await 增加失败后快照，不改变谓词、期限、frame age、GPU timeout 或生产错误处理。capture/control 计数先输出并 flush，再读取可能等待 demodulator mutex 的 consumer 快照，避免在每次轮询内引入该锁。

原 WGC 单 SECTION、原 seed `642631882` 再次失败：exit 42，67.422 秒。关键记录为：

```text
ready=0 state=Failed error=NativeFailure/Completion/0
arrived/copied/delivered=1/1/1 busy/live=0/0 expired=0
continuation=1/1/0 normalize=1/0/0 leases/closes=1/1
consumer submitted/completed/cancelled/expired=1/0/1/0
pending/queued/taken=0/0/0 bootstrap accepted=1 demod submitted=1
```

这排除了“没拿到源帧”“租约没退休”和“GPU 一直运行 60 秒”的解释：帧实际在错误后被安全取消，测试继续等一个不会入队的结果。随后用同一 EXE、相同 SECTION/seed，在 CDB 下仅捕获现有 debug output；没有设置断点、修改 GPU 行为或关闭检查。CDB/test 均 exit 42，74.453 秒，诊断日志明确给出：

```text
D3D11 ERROR #2097405: CLEARUNORDEREDACCESSVIEWFLOAT_INVALIDFORMAT
```

`D3dRoiRing::Mark → CheckDebug` 看到 ERROR 后返回 `NativeFailure/Completion/0`，随后 CaptureRuntime 走终端取消，和快照完全一致。该错误不是本轮新加的诊断导致；旧 EXE 的错误检查逻辑未变。两次均使用仅新增测试失败诊断、尚未修复生产代码的 EXE，SHA-256 为 `e178f152e5c0e80fbd4b80ca309c0fde279d38a8be6f76c975caf3fbbe279997`，不冒充首次 full CTest 的原二进制。

### 最小生产修复与独立过滤修复

G11 `1459c861` 在所有 Profile 共用的 calibration 路径增加了 `ClearUnorderedAccessViewFloat`，但该 UAV 是 36×float4、stride 16 的 structured buffer，格式为 `DXGI_FORMAT_UNKNOWN`。Float clear 只适用于 FLOAT/UNORM/SNORM，而 Uint clear 可以对 structured view 做无格式转换的按位写入。[Microsoft Float API](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-clearunorderedaccessviewfloat)、[Uint API](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-clearunorderedaccessviewuint)。

生产只把该调用及零数组类型改为 `ClearUnorderedAccessViewUint` / `UINT{0}`，使所有 float component 得到正零。buffer/UAV/SRV、容量、budget、HLSL、校准参数、dispatch、query/fence、wire 和 CPU/FEC 均不变；不关闭 debug layer，不吞掉 ERROR，也不调整 timeout。独立只读审查确认该范围和 bit pattern。

另将 `PBRemoteVisualGpuParityTests` selector 从 `[.gpu-parity]` 改为 `[.gpu-parity]~[unified]`。修复前后的 `--list-tests` 分别是 2 / 1 个 case，排除项正是已有独立注册的 Unified G12 corpus，不是删除测试或绕过失败。legacy 保留 300 秒预算，Unified 自己的 selector/预算不变。

### 只修复 clear 后的定向结果

- 受影响 `PBCapturePipelineTests`、`PBDemodD3D11Tests`、两个 Qt EXE build 成功，44.953 秒。
- Capture 原有全部 **4 cases / 2,705 assertions 通过，exit 0，34.250 秒**。包括 WGC upright、DXGI rotated LF4 Transport、epoch recreate、crop/lease retirement 和 continuation 拒绝；WGC/DXGI 对应 case 分别 16.786 / 16.725 秒。生产 debug 检查仍启用，内部 60 秒不变。
- 默认 demod 组仍在原 **180 秒**外层期限超时；只有 15 个 case 的完成时间行，不能据此声称 22 cases 全绿。这份红灯及 `verify-clear-fix-summary.json` 的 `allPassed=false` 原样保留。
- 随后独立运行 legacy selector：**1 case / 46,731 assertions，exit 0，73.203 秒**。WARP + 4 个可用硬件 adapter（NVIDIA/AMD），10 个初始/重建 run、25 个 scenario 结果，CPU/GPU accepted set 全部一致；false accepted、failed/cancelled frames 均为 0，shutdown 全部完成。没有重跑独立的 G12 corpus。

本阶段 Capture EXE SHA-256 为 `816f8f4941b3e9708347884cf660ed2d091d6b81edae49d1af610c2a6f34aa9e`，demod EXE 为 `04763eb14ddefee7026c75830d9bd1d65baf8ae4a63fb5250852b2dc781c04af`。全部命令、期限、CDB output、失败快照和结果保存在 `build-unified-release/g20-capture/`，包括 `capture-probe-summary.json`、`cdb-capture.log`、`verify-clear-fix-summary.json`、`verify-legacy-parity-summary.json` 和 `legacy-parity-analysis.json`。

## 9. 经用户批准的构建期 shader bytecode（2026-09-04）

### 测量、决策与不变项

clear 修复后，每个新 demodulator 仍依次编译全部 12 个入口。独立、无 GPU 的 `D3DCompile` 测量得到合计 **8.062 秒**，开销分散在 LF4/Shape/Unified 多个入口，不再是第 6 节单个 phase 循环的异常展开。使用系统 `d3dcompiler_47.dll`、`cs_5_0`、原 flags `264192`，所有 HRESULT=0；此测量不包含 CreateComputeShader、CPU workspace 或 ASan 额外开销，不冒充 ASan 阶段级 trace。

已向用户说明构建流程和编译错误时机将变化，并获得明确同意后才实施。此次 **三个 HLSL 的内容、原 source template、entry 顺序、source name、include handler、target、flags、GPU 调度、资源容量/预算、wire/FEC、public headers 和 timeout 均不变**。没有继续改循环、加入运行时缓存或按 Profile 延迟初始化。

### 私有实现与错误边界

- 新增 `PBDemodShaderCompiler` 私有 build target；不依赖 Qt、不随产品安装，且不受 `PB_BUILD_TOOLS` 开关控制，因为它是库的构建依赖。
- 私有 `demod_shader_entries.inc` 统一维护 12 个 source/entry/runtime-member 对应关系；compiler、runtime 绑定和测试共用该表。
- 继续用原始 `D3DCompile` 参数。全部入口编译成功后才写临时头文件并原子替换；失败返回非零并输出 entry/HRESULT/diagnostics，不以旧产物继续成功构建，不生成运行时 fallback。[Microsoft D3DCompile](https://learn.microsoft.com/en-us/windows/win32/api/d3dcompiler/nf-d3dcompiler-d3dcompile)。
- CMake 通过私有 custom target 和显式依赖生成 `generated/$<CONFIG>/demod_shader_bytecode.h`，不同配置隔离 writer；source/catalog/compiler 变化触发重建。遵循 target 与 file-level dependency 规则。[CMake add_custom_command](https://cmake.org/cmake/help/latest/command/add_custom_command.html)。
- `Demodulator::Create` 保留原 12 个 GPU shader 的创建顺序和原 NativeFailure/Shader 路径，但改为只调用 `CreateComputeShader`；从该库移除 d3dcompiler 链接依赖。公开 `ShaderCompileFailure` 枚举保留，避免 ABI/接口变更。其他库可能仍使用 D3DCompile，不声称整个应用已无编译器依赖。

### 最小验证与结果

```powershell
cmake --build build-unified-release --config Release --target PBDemodShaderBytecodeTests PBDemodD3D11Tests PBCapturePipelineTests PBApplicationTests PixelBridgeEncoder PixelBridgeDecoder --parallel 6
.\build-unified-release\tests\PBDemodD3D11\Release\PBDemodShaderBytecodeTests.exe --reporter console --durations yes --rng-seed 20092026
.\build-unified-release\tests\PBDemodD3D11\Release\PBDemodD3D11Tests.exe --reporter console --durations yes --rng-seed 20092026
.\build-unified-release\tests\PBCaptureNormalize\Release\PBCapturePipelineTests.exe --reporter console --durations yes --rng-seed 20092026
.\build-unified-release\tests\PBApplication\Release\PBApplicationTests.exe "[application][replay][offline][d3d11]" --reporter console --durations yes --rng-seed 20092026
cmake --build build-unified-asan --config RelWithDebInfo --target PBApplicationTests --parallel 6
.\build-unified-asan\tests\PBApplication\RelWithDebInfo\PBApplicationTests.exe "[application][replay][offline][d3d11]" --reporter console --durations yes --rng-seed 20092026
```

实际通过 create-only Python supervisor 运行上述命令，子进程均 `CREATE_NO_WINDOW`，没有干扰屏幕或输入。Release build exit 0 / 44.437 秒，ASan build exit 0 / 28.844 秒；只有 windeployqt 既有 `VCINSTALLDIR is not set` 警告，没有 shader/C++ 编译错误。

| 定向检查 | 结果 |
| --- | --- |
| 新增 bytecode compiler contract | 12 个入口逐字节比较，**1 case / 61 assertions，9.344 秒** |
| PBDemodD3D11Tests 原默认组 | **22 cases / 663,674 assertions，8.110 秒**；原外层 180 秒不变 |
| PBCapturePipelineTests | **4 cases / 2,705 assertions，1.703 秒**；内部 60 秒不变 |
| Release 三个 offline D3D11 Replay | **3 cases / 21,554 assertions，1.031 秒**；内部 15/15/30 秒不变 |
| ASan 同三个 Replay | **3 cases / 21,562 assertions，1.875 秒**；无 ASan error、skip 或栈溢出；原两项 deadline 红灯已定向关闭 |
| 增量 build contract | 无输入变化时 custom command 不执行，generated header 的 SHA-256 和 mtime 均不变 |
| 编译失败 contract | 仅在复制的隔离 fixture 插入 `#error G20_SHADER_FAILURE_SENTINEL`；真实 CMake dependency 自动 reconfigure，D3DCompile 报错、build exit 1，旧 header SHA-256 不变，未留下临时产物；这是预期负例，不是忽略编译失败 |

12 个嵌入 bytecode 合计 **392,884 bytes**，也与第 8 节修复前保存的独立编译 CSO 全部 byte-identical。Release 与 ASan 生成的 header 相同，SHA-256 为 `bbf53e113b453ce43153c066986f0a1fe4f55c9369790e974cb84fc0fef1d470`。demodulator.obj 符号检查无 D3DCompile 引用；ASan 应用测试 PE 确认导入 `clang_rt.asan_dynamic-x86_64.dll`。以上耗时是本机这一轮定向结果，不是吞吐认证或跨机器性能保证。

### 证据与剩余 G20

`build-unified-release/g20-shader-bytecode/` 保留批准边界、构建日志、逐 bytecode seals、`verify-release.json`、`replay-asan.json`、`verify-build-contract.json`、各测试 stdout/stderr 和隔离失败 fixture。当前 demod/Release application/ASan application EXE SHA-256 分别为 `e3c080b8e454b49d51b6dde7bce794caa3f3fa6b40874cb05ff42af535ad75cd` / `8ad4c0d3c5204b7ed9050b61aa77c17631396a0f95af131a06e58fc4b65f95b0` / `5201bd5522b17016b3771c71731f071f1cec778b5817e2ab0eeac9281fe8a3a8`。测试源身份为 `00045ed` 加本轮明确 diff，不冒充提交后打包复验。

220 个原 Golden/corpus/manifest/registry 文件逐个 SHA-256 未变；保护文档仍为原 hash、untracked 且未暂存。没有重复 full CTest、已通过的 parser/resume/storage ASan、Qt GUI smoke、Unified 硬件矩阵、20 GiB、native/remote 或安装包复验。保留唯一 full CTest 的历史 **206/215** 和所有中间红灯，不将多次定向结果拼写成一次 full CTest 全绿。

下一目标仍为 **继续 G20**：独立处理 LF4 Receiver probe final-snapshot 断言、legacy Direct/Shape Replay validation 拒绝；DISPLAY2 2.0x 的容量问题仍待用户决定，未改分辨率或使用左屏。只读审查还提出 LF4 校准前可能继承 caller sampler state 的候选问题，尚无定向复现，未在本次 clear/bytecode 修复中顺手修改，也不将其声称为已确认根因。不能进入 G21。

## 10. Application 预算预检与发布后 probe 计数

从 `1b7a35ebc36d0366a017c7fd5fd16fddf8c80d0a` 继续。重新读取 AGENTS/G20 边界，复验 G19 ancestor、原 report、12 个提交级 source seals、候选 worker 以及上一轮的提交和测试证据。上一轮为 progress，不是仅等待屏幕条件。只处理两项已知 application 失败，没有进入 G21。

### 最小复现

只增加原测试的 status INFO 和既有 probe 最终失败消息的数值上下文，不改生产行为。`PBApplicationTests` 定向两个原失败 case 再现：exit 42，0.328 秒；诊断 EXE SHA-256 为 `c05867faaf5ed26481d1494545cc5d6bbb13cce5df744315a4ee1ae799d71b3c`。

1. Replay validation 的真实分支是 `leaves insufficient bounded native capture budget: ResourceLimit stage=1 native=0`，不是 G16 删除了旧入口。G16 工作流仍明确保留显式 CLI/Replay 诊断兼容。
2. LF4 probe 的 1-byte 输入已经 `state=8 (Completed), digest=1, published=1, verified=1`，但 `accepted=2 expected=4`。G05 的 `published_` guard 正确忽略后续 refinement，旧 probe 却把这次调用继续计入 processed/admitted；不是最终文件校验失败。

### 两个最小修复

**预算预检：** 旧预检把 128 MiB processor cap 当作 reservation，而实际创建 readback 时传入 recorder 的具体预留。新增 application-private、无分配/无文件副作用的共用计算器，以 checked arithmetic 计算 `(queue + 1) × frame bytes`、slot/queue/state 元数据及预留 demod observations；Create 在分配和打开 writer 前使用同一计算。生产 preflight/runtime 共用几何与限额配置映射，保持原 128 MiB processor cap、capture/readback caps、queue size 和 file limits；运行时再次核对创建后的 reservation 与计算结果相等。没有通过调高上限使测试通过，没有把此 reservation 字段冒充整个进程的实测峰值。

**probe：** 保留生产 `ReceiverPipeline::Process` 的发布后拒收、FEC/选模、whole digest 和发布逻辑。对故意注入的 late refinement，明确验证返回未准入，Completed/digest/publish、输出路径、verified bytes、accepted/unique/duplicate/geometry 计数不变；不再把该调用计为实际处理的 observation。发布前的 Wirehair refinement 必须真的 `carrierAccepted && uniqueAdmission`，仍验证 4 个 admitted blocks、最终 digest 和重新打开文件后的全字节比较。计数同时覆盖后续普通 Data observation，公开 snapshot/DecoderConfig/运行接口未变。

测试保留原 1-byte DirectRepeat 和 4,096-byte Wirehair，另增加 2,048-byte/two-block DirectRepeat 的早完成边界。中间两份红灯保留：一份新断言误用了只由 headless 路径填充的 `hasDataAdmission`；另一份新增 fixture 错把默认策略的 2-block 输入标为 Wirehair。对照既有 `Process` 聚合返回值和 `ChooseOuterFecMode` 后修正测试侧预期，没有改变返回合同、选模策略、文件内容或错误处理来迁就断言。

### 定向验证与预算

```powershell
cmake --build build-unified-release --config Release --target PBApplicationTests PixelBridgeEncoder PixelBridgeDecoder --parallel 6
$probe = '[application][decoder][remote-visual][lf4][temporal][receiver][outer][publish]'
$affected = '[application][validation][remote-visual][production-replay],' + $probe + ',[application][replay][remote-visual][bounded],[application][replay][remote-visual][validation],[application][replay][remote-visual][recorder-budget]'
.\build-unified-release\tests\PBApplication\Release\PBApplicationTests.exe $affected --reporter console --durations yes --rng-seed 20092026
# 最后只改新增 fixture 的模式标签，因此 Release 只复测该一个 case：
.\build-unified-release\tests\PBApplication\Release\PBApplicationTests.exe $probe --reporter console --durations yes --rng-seed 20092026
cmake --build build-unified-asan --config RelWithDebInfo --target PBApplicationTests --parallel 6
.\build-unified-asan\tests\PBApplication\RelWithDebInfo\PBApplicationTests.exe $affected --reporter console --durations yes --rng-seed 20092026
```

- `--list-tests` 确认 affected selector 仅 5 个 case。所有运行由 create-only supervisor 以 `CREATE_NO_WINDOW` 启动，单次测试外层 60 秒，未变更任何产品/测试 deadline。
- Release 的 Replay validation、recorder 原有 2 例和新增 calculator 例先通过；最后只重跑 LF4 probe：**1 case / 119 assertions，exit 0，0.860 秒**。这是分次定向收口，不是重新执行完整模块。
- 最终 ASan：**5 cases / 219 assertions，exit 0，4.234 秒**，stderr 无 sanitizer 错误。覆盖 Direct/WGC、Shape/WGC/DXGI、低 file budget 拒绝、预检不落盘、计算值等于实际 recorder reservation、BGRA/float 字节宽度、observation 预留、精确预算/少 1 byte、零/越界尺寸、未知格式、queue 0/17、row-pitch/frame 乘法溢出、失败不修改输出，以及三个 LF4 fixture 的 digest/publish/重新读取全字节比较。
- 最后 Release application tests EXE SHA-256 为 `40ab81c42d11a372a6aed932e433a5dcc92f891e13a98d3e33c6031e24eb39da`；ASan 为 `4190575bf5e46f0bece022059de26b616c9b422527c22df7b52709d6880efe92`。这是 `1b7a35e` 加本轮明确 diff 的测试构建，不冒充提交后的包认证。
- 只构建受影响 application target 和两个应用，未重跑 GPU、Capture、shader compiler、Qt GUI smoke、完整 CTest、完整 ASan、20 GiB、native 或 remote。

全部原始失败、selector、构建/测试日志和 hashes 位于 `build-unified-release/g20-application/`：`preflight.json`、`probe-before.json`、`verify-release.json`、`verify-release-final.json`、`verify-final.json`。中间 `allPassed=false` 不覆盖，成功另存。最终 diff/源码/二进制 seals 与 220 个 Golden/corpus/manifest/registry 的保护检查单独归档；`docs/PHASE1_GATE_REPORT.md` 始终未改动、未暂存。

本轮关闭最后两项已知 application 断言，不等于新的 full CTest 全绿，也不是屏幕像素链验收。下一步仍为 **G20**：先定向判断 LF4 caller sampler state 候选是否确实可复现；DISPLAY2 2.0x 容纳问题继续等待用户决定，未豁免/改屏幕设置。最终完整回归证据如何在既定预算内收口也仍需明确，当前不关闭 G20。

## 11. LF4 校准不再依赖调用方遗留 sampler

从 `86b6f5cbb89c86c0544978bed7787888a595d5ee` 继续。重新核对 AGENTS、G20 前置/范围/退出标准、Git 状态；G19 ancestor、原 report SHA-256、12 个提交级 source seals、候选 worker，以及上一轮 application 源码/定向验证/提交后身份记录均通过复核。只改 demod 私有绑定顺序、同文件组的测试和两份 G20 文档；不进入后续目标。

### 单变量运行时证据

旧 `SubmitInternal` 先执行 LF4 calibration dispatch，后在 freshness/data dispatch 前才绑定自有 LINEAR sampler；但 calibration HLSL 同样使用 `SampleLevel(s0)`。公共调用合同没有要求 caller 先清空 s0，因此仅靠常见调用路径留下的状态不能保证正确性。

新增测试 `RemoteVisual LF4 calibration does not inherit caller sampler state` 使用独立 Area resampler，保持 1.0 scale、平移 `(11.25, 13.5)`；只在黑色 pilot 内加入交替 0/64 行。正常 LINEAR 采样在半像素纵向位置得到约 32 的非 clipping 端点，POINT 会直接读到零行。原始 record、Data、decode policy、几何和输入像素在两次 GPU 提交间不变；只改变 caller sampler 的 `Filter`，其余 sampler 字段相同，capture observation 编号按合同递增。

- CPU reference 验证通过，接受 4 个 Transport block。
- 旧生产代码下，caller 为 `MIN_MAG_MIP_LINEAR (21)` 时 GPU 接受同样 4 个 block，false accepted 为 0。
- caller 改为 `MIN_MAG_MIP_POINT (0)` 后 Submit 成功，Poll 返回 **`CalibrationFailure`、stage 10、native 0**。测试 exit 42，**1 case / 92 assertions，91 passed、1 failed，0.328 秒**。
- 这是 legacy LF4 直接调用边界的有效帧误拒绝，不是之前 Capture Float-clear 失败的新归因，也不是 Unified 现场失败或错误文件发布的证据。

### 最小修复与测试诊断副作用

将已有自有 sampler 的两行绑定提前到 LF4 calibration dispatch 之前，保留原对象/参数、末尾 s0 解绑以及所有校准拒绝分支。HLSL、嵌入 bytecode、Golden/pins、阈值、资源预算、公共接口和其他 Profile 分支均不变，没有加入状态缓存或运行时 shader 编译。

第一次修复后验证中，新 sampler 用例已通过两种 Filter，但相邻既有 geometry 用例发生栈溢出。原 EXE/obj、日志和 CDB minidump 保留：CDB 对同一 EXE 的单独 geometry case 捕获到 `0xC00000FD`；实际 RVA/unwind 与 `.obj` 反汇编对应到 `PollInternal` 入口栈探测，不把 Catch2 当前断言行当作实际故障位置。此 Release 无源码级 PDB，debugger exit 0 只表示成功抓取并退出，不表示测试通过。

本轮为显示 Poll 错误添加的两行临时 `INFO`，使测试 helper `PollUntilReady` 的编译后固定栈预留由 **102,336 增至 204,416 bytes**。仅撤销这两行临时日志，helper 恢复与父提交一致；保留全部 REQUIRE、用例、图像和生产修复，原 **1,048,576-byte PE stack reserve** 不变。之后同三个用例全部通过。没有通过删除失败断言、提高栈限额或改生产对象生命周期来掩盖这个诊断副作用；也不将局部栈差分声称为所有调用链的最大栈上界。

### 最终最小验证

```powershell
cmake --build build-unified-release --config Release --target PBDemodD3D11Tests --parallel 6
$cases = 'RemoteVisual LF4 calibration does not inherit caller sampler state,' +
         'RemoteVisual LF4 D3D11 continuous geometry survives independent scale and blur fixtures,' +
         'RemoteVisual LF4 D3D11 demod produces compact metrics and the same accepted Transport blocks as CPU'
.\build-unified-release\tests\PBDemodD3D11\Release\PBDemodD3D11Tests.exe $cases --rng-seed 20092026 --durations yes
```

通过 create-only Python supervisor 以 `CREATE_NO_WINDOW` 执行，build 外层 300 秒、测试外层 120 秒；原 Poll 60 秒不变。最终 build exit 0 / 7.391 秒，测试 **3 cases / 330 assertions，exit 0 / 0.968 秒**：

| 用例 | 验证范围 | Catch2 耗时 |
| --- | --- | --- |
| caller sampler 隔离 | 两种 Filter 均匹配 CPU 的 4 个完整 Transport block；false accepted 0；s0 正确解绑；两帧 retired、pending 0、raw pixel readback 0 | 0.318 秒 |
| 原 continuous geometry | 原独立 fractional scale 与 Gaussian blur fixture 的完整 accepted truth/counter 一致 | 0.405 秒 |
| 原 compact metrics | exact-canvas 边界 snap、FEC/Transport、compact readback 与 CPU 一致 | 0.198 秒 |

最终测试 EXE SHA-256 为 `44505a1a80a9c08cf3320aa8a4492f4b3740573ab517d43689ac498eea958443`；对应 `86b6f5c` 加本轮 diff。三份 HLSL 和 generated bytecode header 保持第 9 节的 hash，未重跑 compiler equivalence 或已通过的等价 GPU 矩阵。此 WARP 结论不是原生屏幕/远程像素链或整文件发布认证。

证据在 `build-unified-release/g20-sampler/`：`preflight.json`、`before.json`/原 calibration 红灯、`after.json`/诊断栈溢出、`cdb-stack.log`、`stack-before.dmp`、`stack-comparison.json`、最终 `without-poll-info.json`/stdout/stderr，以及各阶段 source/executable hashes 和 diff；不覆盖任一中间 `allPassed=false`。最终源码、Golden/保护文件和提交后的应用身份分别另存审查记录。

本轮未运行完整 CTest、ASan、完整 WARP/硬件矩阵、Qt GUI smoke、20 GiB、native、remote 或安装包验收。开始时已向用户询问：是否在代码冻结后额外执行一次完整 Release CTest；未获答复前不自行追加。右屏 2.0x 的容量问题也仍待决定，未操作屏幕设置或用户输入。G20 仍为 **PARTIAL**，不能进入 G21；下一步先明确这两项验收条件，而不是继续扩展候选排查或重复已通过测试。

## 12. 用户批准的冻结代码完整 Release CTest

用户明确允许“在当前冻结代码上追加一次完整 Release CTest”。冻结提交为 **`e0729b292b9dddec49346df73ce72437dd6ac468`**；本次 configure/build/CTest 全程没有修改源码、测试、阈值、超时或注册条件。所有新证据写入 create-only 目录 `build-unified-release/g20-release-final-1/`；该追加预算已经执行一次，不代表以后可以自动重复完整套件。

### 准备与执行范围

- 复核 AGENTS/G20、Git 状态、G19 ancestor/report、上轮 source/executable seals，以及保护文档；未发现前置缺失。
- 七个 `PB_BUILD_*GATE` 开关均为 OFF，process-recovery harness 和 fuzz/ASan 也为 OFF。`--gui-smoke` 在创建 QApplication 前明确要求 offscreen plugin，缺失时直接拒绝；没有真实 DataWindow、ROI selector、屏幕捕获或输入自动化。
- 当前注册 **218** 项，原 **215** 项全部保留。新增的 3 项分别是已提交的 `PBDemodShaderBytecodeTests`、`PBProtocolDumpExit.VariableSessionContext`、`PBProtocolDumpExit.LegacySessionContext`。
- 预检脚本最初只计入新增 shader case，按 216 项检查而中止；当时尚未启动 CTest。对照 `00045ed` 和 `tests/tools/CMakeLists.txt` 确认另外两个既有工具 case 后，修正的是本地清单审计预期，不是生产代码或测试注册。原预检失败记录另存，没有重跑 build 来掩盖差异。
- 先备份会被测试重新生成的 G12 `.actual.jsonl` 和 CTest 临时日志；旧 full CTest 四份原始输出的 SHA-256 在新运行前后保持不变。

```powershell
cmake -S . -B build-unified-release
cmake --build build-unified-release --config Release --parallel 6
ctest --test-dir build-unified-release -C Release --output-on-failure --parallel 2 `
  --output-junit g20-release-final-1/ctest-full.xml
```

实际由 Python supervisor 以绝对路径及 `CREATE_NO_WINDOW` 执行；无 `-R/-E`、`--rerun-failed` 或 `--repeat`。configure **exit 0 / 6.250 秒**，完整 Release build **exit 0 / 20.484 秒**。构建保留已有 windeployqt `VCINSTALLDIR is not set` 警告，没有编译错误；不以此声明安装包验收通过。

### 整套结果与原九项红灯

**218/218 CTest passed，0 failed、0 CTest skipped，exit 0；CTest real time 217.27 秒，外层 supervisor 217.344 秒。** 注册集合与 JUnit case 集合逐名相同，所有测试启动 EXE 的 seals、冻结源码及 220 个 Golden/corpus/manifest/registry pins 在运行前后均一致。

| 原失败项 | 本次结果 | 秒 |
| --- | --- | ---: |
| PBApplicationTests | 通过；103 内部 cases passed、1 skipped，77,241 assertions passed，跳过原因见下文 | 17.039 |
| PBCapturePipelineTests | 4 cases / 2,705 assertions 通过 | 1.743 |
| PBDemodD3D11Tests | 23 cases / 663,818 assertions 通过，含两种 caller sampler 状态 | 8.719 |
| PBRemoteVisualGpuParityTests | 通过 | 2.978 |
| PBGoldenVectorTests | 14 cases / 57,902 assertions 通过 | 0.273 |
| PBGoldenVectorCheck | 通过，未改历史 Golden bytes/pins | 0.262 |
| PBGoldenVectorCheckTests | 7 cases / 120 assertions 通过 | 0.497 |
| PBProtocolDumpTests | 8 cases / 2,380 assertions 通过 | 0.016 |
| PBVectorGenIntegration | 通过 | 0.952 |

shader bytecode contract 为 **1 case / 61 assertions，10.400 秒**。两个实际应用 offscreen GUI smoke 分别 **0.193 / 0.873 秒**通过。统一 GPU parity 在该整套运行内通过，当前生成报告核实 WARP/一个 AMD/一个 NVIDIA 各 18 场景，共 **54** 条，false accepted、truth mismatch、conflict output 均为 0；两个离屏发布 fixture 的 whole digest、safe publish、final reopen 和硬门槛均为真。没有把这些 GPU/离屏发布结果写成 DISPLAY2 实屏链或 G21 远程验收。

**内部 skip 的精确边界：** `PBApplicationTests` 中 `Sealed real Direct Shape and LF4 receiver-only datasets preserve production failure classification` 因 **`PB_REMOTE_VISUAL_REAL_REPLAY_ROOT is not set`**，按既有测试逻辑跳过。没有新增 skip、删除断言或为全绿改测试。它不是本次已通过的 `PBRealCaptureReplayTests` 所覆盖的同一个 case，不能用后者替代；本次没有执行该外部封存数据集 case，也未临时设置路径追加重跑。因此结论是“完整注册的 CTest 套件通过”，不是“全部内部 case 无条件执行”。

### 封存、提交边界与下一步

- `preflight.json`：用户批准边界、冻结源码与旧 full-run/pins seals。
- `plan.stdout.log`、`initial-plan-audit.json`、`plan-audit.json`：原 215 项保留、新增三项来源及清单审计。
- `ctest-started.json`、`ctest-full.stdout.log`、`ctest-full.stderr.log`、`ctest-full.xml`、`ctest-summary.json`：唯一追加进程、完整命令/期限、原始输出和 218 项结果。
- `test-executable-seals.json`、`full-result-audit.json`：测试 EXE、逐项结果、内部 skip、旧九项红灯现状及独立解析的新 G12 报告。
- `prior-generated-artifacts/` 和 `completed-artifacts/`：旧/新 CTest 临时日志与 G12 固定文件名报告分开保存；另保留本次实际测试的两个 `e0729b2` 应用 EXE，避免后续文档提交的身份重构建覆盖唯一候选。

本轮提交只更新两份 G20 文档；运行时代码保持冻结。提交后应用身份检查若执行，单独归档，不重复完整 CTest，也不把新文档提交冒充上述测试的源码身份。`docs/PHASE1_GATE_REPORT.md` 始终未修改、未暂存。

**当前 G20：** 完整 Release CTest 与 offscreen GUI smoke 已取得本次整套通过证据；此前定向 ASan 的范围和阶段结果见第 4/9/10 节，本轮没有追加 ASan。20 GiB/process-crash harness、真实 native、remote 和安装包验收未执行。右屏仍需满足 2.0x 的 3840×2160 完整包含条件，目前已确认的 2560×1440 配置不足；仍等待用户决定，未改显示模式或用离屏代替该要求。下一目标是 **G20 native 验收安排**，不能关闭 G20 或进入 G21。


## 13. 现有右屏 native 验收、0.75x 修复与剩余阻塞（2026-09-04）

### 用户决定、前置与范围

- 用户明确要求先验收现有右屏 **2560×1440** 能完成的项目，其余项通过后不再验证 2.0x。2.0x 始终标为“用户条件性豁免 / 未验证”，不是通过；本节发现的其他失败不在豁免范围内。
- 用户允许接收端采样/phase 修复，随后明确“**不用考虑兼容性**”。本次没有保留旧错误解码行为的要求，但不改 `PB-Unified-LC4-V1` wire/Golden、point-sampled 展示合同、0.75x 下限、质量阈值或最终发布安全门。
- 起点 `bd58d5e99c4c81e21a4556a01ee9ea4c9159fabb`。重新读取 AGENTS、G20/G21、Git status；G19 `8c7cab5` 为 ancestor，12 个 G19 source seals（按该提交 LF/CRLF 规范化复核）、worker/report seals 以及 220 个 pins 均通过。前置审计：`build-presentation-release/g20-native-1/prerequisite-audit.json`。
- 本节 **不重复完整 CTest**。第 12 节的 218/218 仅对应冻结 `e0729b2`，不冒充后续采样修复代码的 full-suite 结果；正常 Release 两应用仍是该冻结版本。本轮增量构建并运行的是专用 worker 和受影响单元目标。
- 本地证据根：`<repo>\build-presentation-release\g20-native-1`，以下目录均相对此根。

### 专用 Gate 与真实边界

新增 `tests/PresentationGate/unified_native_gate.cpp`、`run_unified_native_gate.py`，仅在既有 `PB_BUILD_PRESENTATION_GATE=ON` 的独立 build tree 编译 `PBUnifiedNativeGate`，**不注册自动实屏 CTest**。配置时关掉该树原有 `PB_BUILD_PHASE0_GATE`，不运行旧 PresentationGate 的显示模式/输入操作模式。

worker 两个独立进程调用默认生产 `EncoderRuntime`、`DecoderRuntime`。Decoder 只获得 output dir 与真实 ROI；不接收 source path、hash、Session oracle、编码数据或临时文件 payload。数据路径为 native D3D11 窗口→真实桌面 WGC（Auto，无 fallback）→GPU→Receiver/storage。supervisor 仅在最终发布后作独立 SHA-256/BLAKE3 和逐字节校验。这里验证的是生产 runtime，不是两个 Qt main EXE 的 native GUI 操作；后者只有此前明确记录的 offscreen smoke。

每次运行前后重新枚举 monitor，worker 每 200 ms 重验 topology、ROI 和自有 HWND。当前实测 DISPLAY1 `[0,0,2560,1440)`，DISPLAY2 `[2560,0,5120,1440)`，两者 96 DPI / 180 Hz，adapter LUID `(0,94961)`；这些是本次观测，不是未来运行可硬编码的假定。仅创建/缩放自己进程的 `WS_EX_NOACTIVATE` HWND，展示前检查含 window chrome 的完整矩形，始终 `SWP_NOACTIVATE`，不操纵外部窗口、鼠标/键盘或显示设置。所有 Gate 的前后 catalog 一致；成功 sender 正常退出并释放窗口。诊断截图仅 BitBlt 自有右屏 client，原始 BGRA 不传入 live Decoder。

### 已运行 native 结果（每项独立新 Session / 新输出目录）

| Case / client | 配置 Hz | 实际 sender Hz | observed unique Hz | 最终发布 | Encoded B / unique frame |
| --- | ---: | ---: | ---: | --- | ---: |
| `native-15-1x` / 1920×1080 | 15 | 15.0449 | 1.2766 | 262,144 B，SHA/BLAKE3/bytes 一致 | 26,214.4 |
| `native-1-1x` / 1920×1080 | 1 | 1.0053 | 0.4989 | 同上 | 37,449.1429 |
| `native-60-1x` / 1920×1080 | 60 | **44.8953** | 2.4546 | 同上；不代表实测持续 60 Hz | 26,214.4 |
| `native-15-075x` / 1440×810，修复前 | 15 | 约 15.008 | 未绑定 Session | 22 秒 receiver 期限内未发布 | 不可用 |
| `native-15-075x-fixed` / 1440×810，修复后 | 15 | 15.0046 | 0.6241 | 262,144 B，全部发布门和外部比较通过 | **32,768** |
| `native-15-1125x` / 2160×1215 | 15 | 有发送 | 未绑定 Session | 未发布；见几何诊断 | 不可用 |
| `native-15-letterbox` / 2240×1120 | 15 | 有发送 | 0.2862（仅 2 unique） | 本次期限内未发布 | 不可用 |
| `native-pause-resume` / 1920×1080→1344×756→1920×1080 | 15 | 成功暂停及恢复 | 1.3249（8 unique） | 本次期限内未发布 | 不可用 |

前三项使用当时冻结 sender/receiver，后四项中的 fixed/letterbox/pause 使用 `frozen-worker-point-model/PBUnifiedNativeGate.exe`。第一次 15 Hz 运行的原 `summary.json` 因 supervisor 错把历史 minimum-submit-dwell 计数当成 G09 合同而标 false；**原失败文件未覆盖、未重跑实屏掩盖它**。G09 源码/测试证明使用绝对逻辑 tick deadlines，慢帧之后的快帧允许短于名义间隔。新增 clock 纯单测，按 `elapsed × fps`、最多 1 pending、单调 FrameSequence 对同一份日志重审，`corrected-contract-audit.json` 为通过（25,199 ms / 378 帧 / 上界 380）；原 legacy counter=191 保留，不作为 Unified 通过条件。first worker hash 保留在 provenance，原 EXE 未在重编译前另存，不能声称已封存该 binary。1 Hz/60 Hz 的实际 worker/DLL 另存 `frozen-worker-v2/`。

### 0.75x：原因、最小修复与对抗检查

1. 修复前 WGC 能接受 Bootstrap（原 gate 120 次），但无法绑定 Session。`diagnose-075-gpu-final/first-result.json` 直接观测 Unified frame：Base erasure=5、Fine=6、Chroma=0、10 accepted blocks；`demod-final.json` 为 134 completed、134 postFecFailed、1,340 accepted Unified blocks、demodRejected=0。这是 Base/Fine phase 拒绝，不是 GPU device/timeout/calibration API 错误；Control 位于 Base，故不能靠 Chroma 进入最终业务状态。
2. `diagnose-075/actual-right-roi.bgra`：真实右屏 1440×810、sequence=4，SHA-256 `ebd30a1beb3e9e5cdac1af636167750bb98df153390dc62f24c8254e779570f7`。四级 calibration 精确 32/80/176/224，九个 freshness 全 current；独立 phase 运算得到正确 phase 的归一化残差 **0.1875 > 0.125**。真实每 tile 行/列保留 `[0,1,3]`；简单数学 point 构造取 `[0,2,3]`。原 mandatory 0.75x corpus 是 Area transform，不能替代实际 point 展示。
3. “只丢弃不可观测 chip”的初始原型只通过构造样本，原截图仍失败，**已撤换该原型**；失败/通过输出和原型备份均保留。最终方案是对当前 captured sample 预测其实际来源的 canonical chip。原 integrated-sample reference 能通过时仍使用 reference；否则最多四种独立横/纵 point tie 模型，由本帧 Base/Fine phase pilot 各自选择。所有竞争 phase 都在相同候选集合中比较，残差阈值不变；数据 tile 只使用该 lane 当前帧选定的模型，不根据载荷、provider 或 GPU 品牌猜测。
4. CPU / HLSL 同步；私有 `PhaseOutput.w` 携带 0..4 model，readback 验证整数范围。仍为 16 个 float4 phase entries，不增加 raw ROI readback，不改变 slot/ring/public API。无法预测的相邻 tile 不填造 bits，以零 metric 擦除，Chroma 保持独立。
5. 新独立整数 fixture 覆盖四个取样组合，clean 为 31/31 且逐字节 truth 相等；将 Base pilot 换成上一 sequence 后，Base 拒绝、Control=0，Fine/Chroma 仍可用。CPU 与 WARP 均通过。原真实截图以默认 CPU oracle 重解得到 **31/31**、三 lane 无 erasure（`cpu-oracle-point-model.json`）。这只是单帧诊断，不单独构成文件恢复证据。
6. 在全新 `native-15-075x-fixed/` 实屏会话，Encoder/Decoder exit 0，8 unique frames 后独立 whole-file 校验/安全发布/final reopen 通过，三个 lane FEC/CRC/identity failures 均为 0。最终 SHA-256 `bb713c1b00f71780bf2d358591d3db11dc33ff3621f4a4c213d8b2e18a59cc6a`，BLAKE3 `3c66a8482ac78083b99745e48b289b9f98ba4da26bf5f1e3577978704cc4d961`。不把 configured 15 Hz 或 encoder rate 当作 receiver unique FPS。

shader 扩展首次触发 MSVC 单字符串字面量长度限制 `C2026`，未降低 shader 功能或运行期再编译。构建期将 Unified HLSL 分成最多 8 KiB 的相邻 raw literals，保留原首行换行和精确内容；最终 shader bytecode contract 61 assertions 通过。旧生成结果未用失败编译覆盖。

### 剩余失败：不得越级或忽略

- **1.125x（已定位到几何边界）**：`diagnose-1125-owned/actual-right-roi.bgra` SHA-256 `bde186b8f7ea0dc5b5d0bcf549212f0762073d5eae55b9446dc5d171d2a80938`。独立默认 CPU 解得 Bootstrap `None` erasure / sequence=4，scale=(1.125,1.125)、origin=(0.5,0.5)、markerResidual=0；但完整画布 far edge=(2160.5,1215.5)，被当前 0.005 px refinement 边界检查明确判 `CanvasClipped`。该 half-pixel 量化与 0.75x 的 phase 故障不是同一分支。**尚未修改几何/裁剪判定**；区分 point raster 的亚像素量化不确定性与真实 crop 会影响接收覆盖合同，需要明确该边界后再实现，不能直接增大 tolerance 让测试通过。
- **letterbox（未定位最终未发布原因）**：本次 live 仅 2 个 unique frames，接收结果三 lane FEC/CRC failures=0，但未最终发布，不能写通过。独立 `diagnose-letterbox-owned/` 实际截图默认 CPU 可接受 31/31，origin 约 (124.3841,0.05729)、scale 约 (1.03710,1.03693)；这反驳“必然同于 1.125x 整帧裁剪”的猜测，不代替最终文件闭环。没有为它更换易过的宽高比或盲目拉长 deadline。
- **pause/resume（发送端通过，文件门失败）**：稳定暂停 4.096 秒期间 FrameSequence=50、source texture replacements=49、submitted counter 和 Session 不变；九个真实右屏 GDI matte samples 为 128，声明仅为九点检查。恢复到 1920×1080 后同 Session 的 FrameSequence=290、presentation epoch=5；但 Decoder 仅取得 8 个 unique、三个 lane FEC/CRC failures=0，期限内没有 whole-file publish。不得用 sender 恢复成功当成完整产品链成功；后续仍须从已捕获帧→descriptor/ingress→最终 storage 状态定位。
- 诊断截图 helper 第一次按 PID 选 HWND 时因同进程多个窗口而 fail closed，立即仅终止自有子进程；没有捕获左屏。改为 PID + `PixelBridge Data Window` 精确选择后保存到新 `*-owned` 目录。原失败目录不覆盖。单帧 capture replay helper 保留在本地 `capture_owned_roi.py`；它不进入产品或正式 payload 路径。

### 定向测试、命令与冻结证据

最终只运行受影响目标；所有 child 用 `CREATE_NO_WINDOW`、有界 timeout、分离 stdout/stderr，未跑全量回归或新增显示模式 Gate。

```powershell
cmake --build build-presentation-release --config Release --target PBUnifiedVisualCpuTests PBDemodD3D11Tests PBUnifiedNativeGate --parallel 6
& .\build-presentation-release\tests\PBModulation\Release\PBUnifiedVisualCpuTests.exe
& .\build-presentation-release\tests\PBDemodD3D11\Release\PBDemodD3D11Tests.exe "Unified layout-8 D3D11 demod hands*,Unified layout-8 D3D11 demod preserves*,[point-downscale]"
& .\build-presentation-release\tests\PBDemodD3D11\Release\PBDemodShaderBytecodeTests.exe
& .\build-presentation-release\tests\PresentationGate\Release\PBUnifiedNativeGate.exe --self-test
```

| 最小检查 | 结果 | 秒 |
| --- | --- | ---: |
| 最终 affected build | exit 0 | 26.375 |
| Unified CPU 单元目标 | **11 cases / 1,022,877 assertions，exit 0** | 5.407 |
| Unified WARP 新 point + 邻接 compact/geometry | **3 cases / 1,454 assertions，exit 0** | 1.750 |
| shader bytecode contract | **1 case / 61 assertions，exit 0** | 11.812 |
| native worker headless policy | PASS；无 HWND/capture/input | 0.047 |
| 原 18 场景 CPU Golden corpus（此前同轮，仅执行一次） | **1 case / 9,042,053 assertions；report 逐字节一致** | 4.172 |

新 CPU point 首次修复验证为 1 case / 605 assertions / 1.000 秒；新 WARP 为 1 case / 790 assertions / 0.812 秒。最终邻接计数包含异步轮询，不要求与前次相同。过程中旧 observable-chip 原型、build C2026、helper 编码/窗口选择错误和三项 native 文件失败均保留，不用最终绿色日志覆盖。

native 独立复验方式（**只有明确执行以下命令才会显示右屏窗口**；每次必须新 run directory）：

```powershell
& <python> tests/PresentationGate/run_unified_native_gate.py `
  --worker build-presentation-release/g20-native-1/frozen-worker-point-model/PBUnifiedNativeGate.exe `
  --catalog-executable build-unified-release/apps/PixelBridgeDecoder/Release/PixelBridgeDecoder.exe `
  --run-directory build-presentation-release/g20-native-replay-new `
  --case native-15-075x
```

不打开屏幕的原截图复验：

```powershell
& .\build-presentation-release\g20-native-1\frozen-worker-point-model\PBUnifiedNativeGate.exe --inspect-roi `
  build-presentation-release/g20-native-1/diagnose-075/actual-right-roi.bgra `
  build-presentation-release/g20-native-1/cpu-inspect-new.json 1440 810
```

`frozen-worker-point-model/seal.json` 封存实际 native worker SHA-256 `39a8963a3cb9ec33ae187ede7156d4035494279a942e12d172119e8a81cc31bb` 及 DLL；`source/` 与 `source-seal.json` 保留其对应 dirty 源码和 base HEAD。native 之后的最终小修复仅把“样本可读但所选模型预测跨相邻 tile”的 CPU 零 metric 归类对齐 GPU 的 decision-margin erasure；最终三组定向单测覆盖，未重复已完成的 native 成功项，不能把两个 worker 字节身份混写。最终 mutable build worker 与源码另在最终审计封印。`point-final-checks.json`、`point-model-native-summary.json`、`remaining-native-summary.json` 和各 case 原始 reports 是精确命令/结果入口。

**本节未执行：** 追加 full CTest、追加 ASan、完整 G12 多适配器矩阵、20 GiB/process-crash、Qt main EXE 的新 native UI 流程、2.0x native、G21 remote、G22 package。未修改或暂存 `docs/PHASE1_GATE_REPORT.md`，未修改 220 个既有 pins。**下一目标仍为 G20；G21/G22 未启动。**

## 14. G20：point 半像素几何与完整画布覆盖（2026-09-04）

**结论：1.125x 已修复并完成全新 native 文件闭环；G20 仍 OPEN。** 本节更新第 13 节的 1.125x 待决/失败状态，不覆盖原失败证据。用户明确同意修正几何估计与覆盖判定，区分 point 半像素量化和实际裁剪，保持整像素裁剪拒绝及质量、摘要、发布门；不是直接扩大 tolerance。实施基线 `690acf89dd6ec1e5f4929d40b94f64ef1eda2168`，G18/G19 仍为祖先，G19 报告及原 seals 已核对。本轮仅修改一个生产实现文件、三个定向测试文件和两份 G20 文档；不修改公共接口、wire、HLSL、资源上限或展示合同。

### 根因与实现边界

- 原真实 2160×1215 ROI 的 marker 边缘全都落在 point sampling 的半像素 tie 上。连续边缘最小二乘把像素跳变的中点当作精确边缘，得到 `origin=(0.5,0.5), scale=(1.125,1.125), markerResidual=0`，随后 `CanvasClipped`。新独立整数 9/8 point fixture 在未修复生产代码上复现同样结果（`point-coverage/red-test.log`，exit 42）；不是仅根据合成推测现场。
- 在 `local_desktop_decode.cpp` 的 **Unified-only** locator refinement 中，保持已拟合 scale 不变。只有当前画布不完整包含于 ROI 时，才在受影响轴重新检查四个 marker 的 24 条已知边缘。边缘必须为整数像素边界，且相邻像素中心分别匹配该 marker 的黑/白端点；只允许原有 `1e-9` 数值 roundoff，不把灰化/模糊边缘当作 point 证据。
- 对每条边缘，由固定 scale 和跳变位置 E 推得 origin 的闭区间 `[E - scale × logical - 0.5, E - scale × logical + 0.5]`，求 24 条边缘与完整画布可容纳范围 `[0, ROI - scale × canvas]` 的交集。交集为空、拟合画布比 ROI 大、任一边缘无可靠证据或 reader/work-budget 出错时不放行；不收缩 scale 来隐藏 crop。
- 两轴候选原子应用，任一轴失败不能留下半份修正。调整后的实测 marker residual 重新计入原质量门，本次为 **0.5 px**，而不是继续报告 0。`ResolveUnifiedVisualSamplingGeometry` 的 **0.005 px** 边界检查完全未改；Bootstrap 双副本/FEC/CRC/identity、lane/pilot/freshness、WholeFileDigest、安全发布/final reopen 全部保留。没有增加载荷旁路或跨帧猜测。

### 最小验证与结果

证据根目录：`build-presentation-release/g20-native-1/point-coverage/`。先重新 configure 专用 presentation Release tree，再仅增量构建 `PBUnifiedVisualCpuTests`、`PBUnifiedNativeGate`、`PBUnifiedTransformCorpusTests`、`PBDemodD3D11Tests`，均 exit 0；没有运行新的整套 CTest。

| 检查 | 结果 | 秒 |
| --- | --- | ---: |
| Unified CPU 受影响单元目标 | **12 cases / 1,023,233 assertions，exit 0** | 7.672 |
| 新 Unified WARP coverage 单例 | **1 case / 580 assertions，exit 0** | 0.610 |
| 原 18 场景 CPU transform corpus | **1 case / 9,042,053 assertions，report 与原 manifest 逐字节一致** | 4.094 |
| 原真实 1.125x 截图离线重解 | **31/31 blocks；三 lane 可用、九 freshness current；scale 不变** | 非吞吐测量 |
| 新 1.125x native 文件闭环 | **两个 child exit 0，全部最终发布门和外部 bytes/digests 一致** | 25.625 |

新 CPU 单例覆盖四个横/纵 tie 组合、四边各裁 1 px 的 **16 个拒绝负例**、更严 `maximumGeometryResidualPixels=0.25` 的四个拒绝负例，以及横/纵对称 blur 的两个拒绝负例。blur 保留整数跳变中点但破坏锐利相邻中心证据，不能触发修正；纵轴拒绝同时验证横轴候选不会单独发布。原 1 px crop/冲突/后 FEC 验真用例仍在 CPU 目标中通过。

WARP 覆盖四种 tie 的 31/31 accepted-byte truth，以及四边整数裁剪：Bootstrap 仍可解析，但 GPU submission 在 Binding 阶段以 `InvalidFrame / CanvasClipped` 拒绝，pending 不增加；raw pixel readback=0。新增测试首次运行因复用 upload helper 要求完整末行 pitch，而 cropped span 仅保留逻辑 footprint，未能进入 GPU 裁剪断言；改为测试侧逐行紧密拷贝，不修改 helper/生产边界。第二次是新断言把已存在的 `InvalidFrame` 写成 `InvalidBinding`，按生产的 code/stage/CanvasClipped 三项精确断言修正。两份开发失败日志原样保留，最终生产修复没有为它们改门槛。

```powershell
cmake -S . -B build-presentation-release
cmake --build build-presentation-release --config Release --target PBUnifiedVisualCpuTests PBUnifiedNativeGate PBUnifiedTransformCorpusTests PBDemodD3D11Tests --parallel 4
& .\build-presentation-release\tests\PBModulation\Release\PBUnifiedVisualCpuTests.exe
& .\build-presentation-release\tests\PBDemodD3D11\Release\PBDemodD3D11Tests.exe '[point-coverage]'
& .\build-presentation-release\tests\PBModulation\Release\PBUnifiedTransformCorpusTests.exe
```

### 实际闭环与可复验工件

`native-15-1125x-fixed/summary.json` 为独立新 Session / CSPRNG 262,144 B RAW 文件，默认生产 Encoder/Decoder runtime，没有启用 diagnostic demod wrapper。`DISPLAY2` 仍为 2560×1440，窗口 client 为 2160×1215，前后 monitor catalog 相同；只控制自有 no-activate HWND，未发送输入或更改显示设置。

- WGC accepted geometry 25 次均为 `origin=(0,0), scale=(1.125,1.125), markerResidual=0.5`。
- 配置 15 Hz，实际 sender **15.0319 Hz**；16 个 distinct received logical frames，observed unique **5.8443 Hz**，最终 **16,384 encoded B / unique frame**；不把 sender rate 当作 receiver rate。
- 三 lane 的 FEC/CRC/identity failures 均 0；whole digest、rename、final reopen、published 全 true。外部 source/final **SHA-256 `57e6ad0c4e735a807afda5f1e7eeccc81576d98cd9353fa83cfc5c09c682564e`**，**BLAKE3 `8d98ea9c445d92b9f2793a69ce226fe8e9ef5856c001cf82882dea991ae7761b`**，逐字节相等。
- `frozen-worker-point-coverage/` 另存实际运行 EXE、DLL、九份 source 快照及 `source.diff`；worker SHA-256 **`02296215659851b5a4a7fd9f901012aa59eda711f684fe9bc6cba4decac0f12a`**。嵌入基线 HEAD 为 `690acf8`，修复由 source seals 标识，不能冒充提交后重新构建的包。普通 Release 两应用仍是此前 `e0729b2` 的冻结产物，本轮未改写其身份。

不打开屏幕的原 ROI 复验（report 路径必须不存在）：

```powershell
& .\build-presentation-release\g20-native-1\frozen-worker-point-coverage\PBUnifiedNativeGate.exe --inspect-roi `
  build-presentation-release/g20-native-1/diagnose-1125-owned/actual-right-roi.bgra `
  build-presentation-release/g20-native-1/point-coverage-replay-new.json 2160 1215
```

以下命令会显示一个自有右屏窗口，必须使用新的 run directory，脚本先重新枚举并验证屏幕：

```powershell
& <python> tests/PresentationGate/run_unified_native_gate.py `
  --worker build-presentation-release/g20-native-1/frozen-worker-point-coverage/PBUnifiedNativeGate.exe `
  --catalog-executable build-unified-release/apps/PixelBridgeDecoder/Release/PixelBridgeDecoder.exe `
  --run-directory build-presentation-release/g20-native-1125-replay-new `
  --case native-15-1125x
```

`validation-results.json` / `validation-results-2.json` 保留前两次开发运行，`validation-results-3.json` 为最终 WARP/corpus；`actual-1125-fixed.json` 是同一旧截图修复后结果。`final-audit.json` 核对源码/实际 worker seals、G19 报告、220 个 pins、保护文件、native 最终状态以及原 Golden report。

### 尚未关闭的边界

本轮只读解析旧 letterbox/pause 的 `.resume` journal，PBJH/PBJR CRC32C 全通过，分别只有 **31 / 97 个不同 AcceptedBlock**（`journal-passive-audit.json`），不足 256 KiB RAW / 1,314 B block 的 K=200；这是未完成状态的证据，**不是丢失原因已定位**。未改 journal、恢复状态、发送期限、接收期限或用更小 source 规避失败；两项完整文件门仍未通过，下一步应从实际 Bootstrap 拒绝、geometry、capture/queue 到最终入库定位缺口。

**未执行：** 新 full CTest/ASan、重复已过的 native 1/15/60 Hz 与 0.75x、完整 GPU/硬件矩阵、20 GiB/crash harness、新 Qt main EXE native UI、2.0x、G21 remote、G22 package/提交后 binary 身份复验。2.0x 仍为其余 native 均通过之后的用户条件性豁免，未验证。`docs/PHASE1_GATE_REPORT.md` 未修改、未暂存。**下一目标仍为 G20 的 letterbox/pause 完整闭环，不进入 G21。**
