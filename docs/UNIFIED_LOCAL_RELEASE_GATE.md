# G20：本地 Release Gate 进度与回归修复

> **状态：进行中，不能关闭 G20，也不能进入 G21。** 2026-09-04 已完成一次完整 Release CTest；后续定向修复 Replay worker 栈溢出、五项 Golden/ProtocolDump 工具失败、Capture 非法 UAV clear 和 demod 重复启动编译。经用户同意改用构建期嵌入 bytecode 后，demod 22 cases 及原两个 ASan 15 秒 deadline 用例均通过。application 的另两项既有断言失败和实屏验收条件仍待处理。本文不把部分通过写成最终产品认证。

## 1. 前置与本轮边界

- 前置 G19 提交为 `8c7cab504668dcd7ae05e4564ba09730116f5666`。开始时已验证该提交是 HEAD、G19 report 的 2,561 Segment/publish/final reopen/全字节比较为真，12 个 source seals 与候选 worker SHA-256 匹配；没有重跑 20 GiB harness。
- 开始时唯一外部文件为 untracked `docs/PHASE1_GATE_REPORT.md`。其 SHA-256 为 `076ef4c9b9f89eabccd323dbe4bffc4dc125ddaf96e6ee437d2cf5b1b1cea306`，没有修改或暂存。
- 本轮仅 G20 的构建、回归诊断、最小修复及证据文档；不改 wire、公共接口、Golden bytes/pins、默认资源策略、GUI 交互或屏幕设置。
- 所有 displayful Gate cache 均 OFF。普通 Release 树关闭 G18/G19 私有终止构建，未运行 crash/20 GiB matrix；没有操作鼠标键盘或打开真实数据窗口/选区器。

## 2. Release 构建与唯一完整 CTest

```powershell
cmake -S . -B build-unified-release -DPB_BUILD_PROCESS_RECOVERY_HARNESS=OFF
cmake --build build-unified-release --config Release --parallel 6
ctest --test-dir build-unified-release -C Release --output-on-failure --parallel 2 `
  --output-junit g20/ctest-full.xml
```

首次配置成功，构建在 `test_pbgolden_vector.cpp` 失败：旧测试将 `GetSerializedSize(SessionDescriptor)` 返回的 `ProtocolResult<size_t>` 直接传给 vector constructor。只改为先 REQUIRE 成功再 `.Value()`，保持原 Golden 字节比较断言；增量全构建成功。这只是解除编译阻塞，不代表旧 Golden 的 schema 适配已完成。

完整 CTest 只运行 **一次**：215 项，206 passed、9 failed，exit 8，868.62 秒。没有重新启动整套测试；原始输出与 JUnit 保留。

| 失败项 | 原始结果 | 当前处理 |
| --- | --- | --- |
| PBApplicationTests | SIGSEGV / Stack overflow | 第 3 节 run-owned 缓冲修复；模块重跑暴露其他断言失败，随后只做受影响用例复验 |
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
