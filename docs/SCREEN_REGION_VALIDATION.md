# PBScreenRegion 验证记录

验证日期：2026-08-27。对应 `SCREEN_REGION.md` 和总体设计 §6、§21、§38.4、§40.1。
本记录只验证**物理像素区域选择和单显示器准入**，不是 WGC、Desktop Duplication、LocalDesktop 吞吐或完整捕获认证。

## 环境与边界

- Windows x64；VS 2022 / MSVC 19.44.35228.0，Windows SDK 10.0.26100.0，CMake/CTest 3.31.6。
- 使用仓库现有 vcpkg baseline、CompilerSettings、PMv2 manifest 和 sanitizer/runtime-copy 机制。
- 不新增 Qt、D3D device、GPU 拓扑管理器或文件 payload 通道；不修改协议、FEC、Golden pins、恢复语义或 Encoder 呈现代码。
- 开启应用、测试、工具、Phase 0、ScreenRegion Gate；`PB_BUILD_PRESENTATION_GATE=OFF`，不运行会修改显示模式的呈现实机 Gate。
  原有 `PBPresentTimingTests`、`PBRenderD3DTests`、呈现非 PMv2 宿主探针仍参与完整 CTest。
- Release 与 ASan 分别使用独立构建目录。RelWithDebInfo 开启 `PB_BUILD_FUZZERS`，复用 MSVC ASan 配置。
  已核对新库、Decoder、选区单元测试和实机 Gate 的 `/fsanitize=address`；Decoder 的实际 imports 包含 ASan runtime。

## 自动化与实机证据分开计数

| 层级 | 覆盖内容 |
| --- | --- |
| 模型与共享状态机 | 17 个 Catch2 case、18,282 个断言；固定 seed 的 6,000 个逐像素归属 oracle 和 512 条状态序列 |
| DPI/坐标矩阵 | 96/120/144/168 DPI；同一手势的固定物理 RECT；混合 DPI、负 X、负 Y、双负原点、错位、空隙、portrait；四种 DXGI rotation |
| 边界/拒绝 | 四向拖动、完整 monitor、末行末列、1px、空/反向 RECT、贴边、跨界 1px、离屏、包含歧义、32-bit 极值、checked 加一及宽高窄化、零 DPI、未知 rotation、64/65 monitor |
| 状态/清理 | 非法后重选、纯点击不提交、取消、重入取消、环境变化、所有后端边界失败、失败输出不变、清理先于提交 |
| 宿主与 CLI | unaware/system/PMv1、仅线程 PMv2、PMv2 进程降级线程拒绝；实际 HWND awareness；实际 Decoder 成功 JSON/取消；同一 Decoder 源码在非 PMv2 宿主中的精确 exit=1/stderr；无参 banner、参数错误和帮助 |
| 构建隔离 | 新库静态独立消费者、`BUILD_SHARED_LIBS=ON` 隔离、no-Qt 正/负 Gate、子工程默认关闭测试和交互 Gate、完整构建选项矩阵 |
| 当前实机 | 三个活动显示器逐屏正向/反向拖选、完整首屏、跨屏拒绝后重选；真实 Escape/右键、实际 focus/capture loss、`WM_CANCELMODE`、`WM_QUIT`、显示变化通知；部分初始化及阶段失败；所有 overlay 已关闭 |

真实鼠标输入使用单一 `SendInput` 队列的 virtual-desktop absolute event，逐步等待物理位置和生产状态机确认；
结果与独立 Win32/DXGI/DPI HWND 查询比较。输入设备的归一化只存在于测试驱动，绝不用于产品 RECT 或 capture 坐标。
native Gate 的 fault seam 在真实资源取得后注入阶段错误，覆盖 DPI/query、class、第一/第二窗口、style、font、input context、paint、message 和 cleanup；
共享状态机另覆盖输入/cursor/capture/revalidation 等失败。此处不声称通过破坏系统状态使每个 Win32 API 真实耗尽资源。
生产和 fake/native Gate 使用同一选择状态机，没有测试专用选择器。

当前实机前后快照（均为半开 **physical-desktop** RECT）：

| 显示器顺序 | RECT | effective DPI X/Y | DXGI rotation |
| --- | --- | --- | --- |
| 1 | `[0,0,2560,1440)` | 96 / 96 | identity (1) |
| 2 | `[2560,0,5120,1440)` | 96 / 96 | identity (1) |
| 3 | `[5120,0,6072,600)` | 96 / 96 | identity (1) |

没有修改 Windows 分辨率、缩放、旋转或显示器排列。父进程监督 native worker，并独立验证鼠标位置恢复；
worker 非零退出/超时仍是失败，不会 skip。调用方隐藏窗口的 IME context 和键盘布局在重复选择前后保持一致。

**未实测**：真实 125/150/175%、混合 DPI、负坐标、portrait/旋转显示器、实际热插拔/模式切换、无交互桌面、其他 Windows/驱动/输入法组合。
模型测试不能代替这些实机组合。显示变化实机测试只发送通知，不更改显示设置；具体元数据变化由模型测试覆盖。

## 复审发现与回归

1. **提交前取消竞态**：鼠标松开后的绘制回调可同步收到 `WM_CANCELMODE`，若只排队而不检查待处理终止事件，原实现会发布成功结果。
   `LateCancel` 回归先复现失败，再修复 native revalidation/cleanup 中的终止事件检查；同时覆盖 `PeekMessage` 自身可能派发 sent messages 的情况。
   复现日志：`build-screen-region-release/cancel-regression-before-fix-reproduced.log`；修复验证：`cancel-regression-after-fix.log`。
   完整实机 Gate 持续保留此回归，不通过跳过取消路径来通过测试。
2. **非文本窗口的输入法激活**：早期实机测试发生原生异常；CDB 捕获了 `PalmInputTSF` 中的 AV，调用栈经 `ImmSetActiveContext` 到 overlay 前台激活。
   记录在 `build-screen-region-release/native-debugger-review.log`。现在只对自有非文本 HWND 调用 `ImmAssociateContextEx`，
   不禁用线程/系统输入法，不修改第三方模块。实机检查无 IME 关联且调用方 context 未变；父进程另防护 worker 崩溃时的鼠标恢复。
   这不是对所有第三方输入法/全局 hook 的兼容性认证。
3. **线程覆盖不能证明宿主初始化**：本机实测同进程的 process-DPI 查询可反映调用线程覆盖值。
   `thread-only-v2` 拒绝探针先失败，随后改为核验宿主 PMv2 manifest、实际线程和 HWND；库不调用 DPI setter。
   证据：`dpi-context-diagnostic2.log`、`screen-region-input-context.log`、`screen-region-manifest-guard.log`。
4. **测试身份的可移植性**：扩展 cppcheck 到测试代码后，去掉状态机 fixture 中整数形式的假 monitor 指针，使用独立不可变对象地址作 identity；保留全部断言。
5. **实机输入顺序**：一次压力重复中，旧测试驱动把 `SetPhysicalCursorPos` 直接 warp 与队列中的 relative move 混用，出现一次“目标物理点不可达”；
   父进程按设计清理并使 Gate 失败。该次证据不能唯一归因到库或外部输入，因此没有放宽 RECT/preview 校验。
   测试驱动改为 Windows 规定的 `MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK`，用一个 input stream 排序 move/button，
   关闭 move 合并并等待 `GetPhysicalCursorPos` 精确确认后才继续；不精确可表示的桌面明确失败。
   原失败保存在 `repeat-pointer-failure-before-diagnostic.log`，新的 10 轮 Release 重复见 `ordered-input-repeat.log`。
   后续压力测试又捕获到 child 已完成全部功能/资源检查，但立即读取恢复位置不一致；parent 独立恢复成功，Gate 仍失败，
   记录在 `repeat-restore-failure-before-barrier.log`。恢复路径现在也只定位一次、有限时间读取并确认**精确原位置**，不放宽容差、不反复 warp。
   128 次单独 Set/Get 探针未重现该时序现象，不能据此排除外部输入因素；保留 `set-physical-cursor-timing.log`。
   加入完整输入/恢复确认后，Release 与 ASan 的 native/Decoder Gate 各连续 20 次通过，见各目录 `input-barriers-repeat.log`。

复审清单：有符号坐标、半开端点、加一/差值溢出、DPI 来源、rotation 仅元数据、显示快照一致性、正常/异常 capture 释放、
同步重入消息、`WM_QUIT` 保留、部分 HWND/GDI 创建、COM RAII、输出原子提交、单线程所有权、严格参数和 JSON 契约、协议隔离。

## 验收命令

以下命令从仓库根目录运行；安装路径按本机实际环境记录。两个配置的桌面测试不并行运行。

```powershell
Set-Location D:\MyProjects\PixelBridge
$ErrorActionPreference = 'Stop'
$common = @(
    '-G', 'Visual Studio 17 2022', '-A', 'x64',
    '-DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake',
    '-DPB_BUILD_APPS=ON', '-DPB_BUILD_TESTS=ON', '-DBUILD_TESTING=ON',
    '-DPB_BUILD_TOOLS=ON', '-DPB_BUILD_PHASE0_GATE=ON',
    '-DPB_BUILD_PRESENTATION_GATE=OFF', '-DPB_BUILD_SCREEN_REGION_GATE=ON'
)
cmake -S . -B build-screen-region-release @common -DPB_BUILD_FUZZERS=OFF
cmake -S . -B build-screen-region-asan @common -DPB_BUILD_FUZZERS=ON
cmake --build build-screen-region-release --config Release --parallel 4
cmake --build build-screen-region-asan --config RelWithDebInfo --parallel 4
ctest --test-dir build-screen-region-release --build-config Release --parallel 4 --output-on-failure --output-junit ctest-verified.xml
ctest --test-dir build-screen-region-asan --build-config RelWithDebInfo --parallel 4 --output-on-failure --output-junit ctest-verified.xml
ctest --test-dir build-screen-region-release --build-config Release -R '^PBScreenRegionGate\.' --repeat until-fail:20 --output-on-failure --output-junit input-barriers-repeat.xml
ctest --test-dir build-screen-region-asan --build-config RelWithDebInfo -R '^PBScreenRegionGate\.' --repeat until-fail:20 --output-on-failure --output-junit input-barriers-repeat.xml
& .\build-screen-region-release\tests\PBScreenRegion\Release\PBScreenRegionTests.exe --reporter compact --rng-seed 1324215877
```

静态与格式检查：

```powershell
$cppFiles = @(
    Get-ChildItem libs/PBScreenRegion,tests/PBScreenRegion,tests/ScreenRegionGate -Recurse -Filter *.cpp |
        ForEach-Object { $_.FullName }
) + @('apps/PixelBridgeDecoder/main.cpp', 'apps/PixelBridgeDecoder/screen_region_cli.cpp',
      'tests/CMake/StaticLibraryIsolation/screen_region_consumer.cpp')
cppcheck --enable=warning,performance,portability --std=c++20 --language=c++ --platform=win64 --library=windows --inline-suppr --suppress=missingIncludeSystem --error-exitcode=1 -I libs/PBScreenRegion/include -I libs/PBScreenRegion/src -I tests/ScreenRegionGate -I libs/PBCore/include -I libs/PBProtocol/include $cppFiles
$formatFiles = @(Get-ChildItem libs/PBScreenRegion,tests/PBScreenRegion,tests/ScreenRegionGate -Recurse -File |
    Where-Object { $_.Extension -in '.cpp','.h' } | ForEach-Object { $_.FullName }) +
    @('apps/PixelBridgeDecoder/screen_region_cli.cpp', 'tests/CMake/StaticLibraryIsolation/screen_region_consumer.cpp')
$style = '{BasedOnStyle: Microsoft, ColumnLimit: 160, BreakBeforeBraces: Allman, AllowShortFunctionsOnASingleLine: None, AllowShortIfStatementsOnASingleLine: Never, AllowShortLoopsOnASingleLine: false, AllowShortLambdasOnASingleLine: None, AllowShortBlocksOnASingleLine: Never, PointerAlignment: Left, ReferenceAlignment: Pointer, IndentWidth: 4, NamespaceIndentation: None, AccessModifierOffset: -4}'
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\bin\clang-format.exe' --dry-run --Werror "-style=$style" $formatFiles
git diff --check
```

交互复现：运行 `build-screen-region-release/apps/PixelBridgeDecoder/Release/PixelBridgeDecoder.exe --select-region`；
在任一显示器内拖选，或先跨屏拖选看到 invalid 提示再重选。Escape/右键取消。stdout 只在成功时产生一条带 `physical-desktop` 的 JSON。

## 结果与证据索引

最终源码复验结果如下；两套完整 CTest 的 JUnit 均为 0 failures、0 skipped、0 disabled。

| 检查 | 结果 |
| --- | --- |
| Release 完整默认 target 构建 | 通过 |
| Release 完整 CTest | **74 / 74 通过**，242.42 s，包含既有 `PBPhase0GateLarge` |
| RelWithDebInfo / MSVC ASan 完整默认 target 构建 | 通过 |
| ASan 完整 CTest | **185 / 185 通过**，209.79 s，包含 112 项 fuzz/corpus/structured 检查 |
| 选区单元测试独立运行 | **17 cases / 18,282 assertions 通过**，seed `1324215877` |
| Release 实机压力重复 | native 20 次 + Decoder 20 次，**40 / 40 通过** |
| ASan 实机压力重复 | native 20 次 + Decoder 20 次，**40 / 40 通过** |
| cppcheck 2.21.0 | 12 个任务范围 C++ translation units，0 findings |
| clang-format 19.1.5 只读检查 | 14 个新增 C++/header 文件通过；未重排既有入口 banner |
| 差异和源码冻结核对 | `git diff --check` 通过；29 个构建/测试代码文件 SHA-256 与完整构建前一致 |

`PBPhase0GateLarge` 按仓库既有规则只注册于 Release，未为本任务修改注册条件。
ASan 数量是 RelWithDebInfo 配置实际注册的完整测试集合，不是删除或跳过失败用例后的子集。
复审后没有已知未解决的 Critical / High 问题；上述结果不扩大前文列出的硬件、驱动和输入法覆盖范围。

- 两套目录中的 `configure-final.log`、`build-verified.log`、`ctest-verified.log`、`ctest-verified.xml`。
- 两套目录中的 `input-barriers-repeat.log` / `input-barriers-repeat.xml`；`until-fail:20` 遇失败即停止该项重复，不是失败后重试到通过。
- `build-screen-region-release/unit-detail.log`、`cppcheck-task.log`、`cppcheck-task-stdout.log`、`format-check.log`、`diff-check.log`。
- `build-screen-region-release/task-source-before-build.sha256.json`：构建/测试代码冻结快照，完成时再次比对。
- `build-screen-region-asan/decoder-imports.log`：实际 Decoder binary 的 ASan runtime、physical cursor、DPI、IME、manifest 查询 imports。
- Release 证据根：`build-screen-region-release/tests/ScreenRegionGate/Release/evidence/`。
- ASan 证据根：`build-screen-region-asan/tests/ScreenRegionGate/RelWithDebInfo/evidence/`。
  每次 native/CLI 运行使用独立 PID/QPC 目录，保留 `gate.txt`、`regions.jsonl` 和实际 `decoder-stdout.json`；失败记录也保留。

日志和实机证据属于忽略的构建产物，不加入源码提交。无协议/Golden 变更；本轮未进行 WGC/DD 抓取、旋转采样、跨屏拼接、吞吐测试或 TSan/UBSan 验证。

测试输入归一化依据：[MOUSEINPUT](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-mouseinput)、
[SendInput](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput)。
