# G20：本地 Release Gate 进度与回归修复

> **状态：进行中，不能关闭 G20，也不能进入 G21。** 2026-09-04 已完成一次完整 Release CTest，定位离线 Replay 的 worker 栈溢出和独立的 shader 初始化超时，并进行最小修复；仍有历史工具适配、capture/demod 失败和实屏验收条件待处理。本文不把部分通过写成最终产品认证。

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
| PBCapturePipelineTests | 两个 LF4 case 的 60 秒 Await 失败 | 未定因；不能仅凭组合条件推断是没结果还是 lease 未退休 |
| PBDemodD3D11Tests | 180 秒外层 timeout | 待分阶段诊断，不增加时间上限 |
| PBRemoteVisualGpuParityTests | 300 秒外层 timeout | 另发现旧 `[.gpu-parity]` 过滤命中 Unified G12 用例，重复运行已独立注册的 corpus；尚未修正 |
| PBGoldenVectorTests | 旧 descriptor/control/fragment 期望冲突 | 历史 fixture 重算器与正式 Schema 1 未分层，待修复 |
| PBGoldenVectorCheck | 8 个旧 pin 与重算不符 | 文件与旧 pin 一致，重算却调用新正式 serializer；禁止重新 pin |
| PBGoldenVectorCheckTests | 同上 8 项导致 summary 失败 | 与 Golden 根因同组 |
| PBProtocolDumpTests | 字段偏移、错误名与旧变异位置不符 | 工具仍使用旧 descriptor offsets；待适配正式 schema |
| PBVectorGenIntegration | Golden 重算与旧 pin 不符 | 与 Golden 根因同组 |

正式 `PBProtocolTests`、`PBUnifiedVisualGpuParityTests`（159.08 秒）、`PBUnifiedVisualHardwareSmokeTests`（25.17 秒）及两项 Qt offscreen GUI smoke 已通过；不能把这些局部成功扩大为 full CTest 或 native Gate 通过。GUI smoke 已包含在 full CTest，不再重复。

### 历史 Golden 的明确处理边界

`PROTOCOL_1_DESCRIPTOR_SCHEMA.md` 第 9 节规定旧 37/110/142/65-byte descriptor 是 Phase-0 历史 fixture，正式 parser 只承诺 `UnsupportedDescriptorSchema`。本次旧 67-byte Control 的重算结果变为 115 bytes，BLAKE3 恰好等于 G01 manifest 的正式 accepted ControlSessionDescriptor；这是工具混用了两代合同，不是修改旧文件的理由。

下一步应保持旧 `.bin`/pin/函数签名不变，给历史重算器使用明确的 private legacy byte source；正式成功解析/往返继续由正式 corpus 证明。Dump 的字段表与测试变异位置需按正式 offset 更新，不允许启用猜测式旧布局兼容解析。

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

**下一目标仍是继续 G20，不是 G21**：先完成剩余回归的窄定位/修复和定向复验，并等待实屏 2.0x 决策。禁止重复 full CTest、静默更改旧 Golden 或放宽门禁来关闭目标。当前 build 嵌入父提交 `8c7cab5`，不声称提交后包身份复验。
