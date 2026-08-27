# D3D11 Data Window / PBPresentTiming 验证记录

日期：2026-08-27。起点：干净的 `d5f27be`。

本记录只验收 **Certified candidate 呈现基础设施**。它不宣称完成 LocalDesktop
物理链路认证，也不把 Present 调用、生产端 DXGI 观察或 GPU readback 当作接收端捕获。
接口、恢复规则和指标公式见 [PRESENTATION.md](PRESENTATION.md)。

## 环境与执行边界

- Windows x64，Visual Studio 17 2022，MSVC `19.44.35228.0`，Windows SDK `10.0.26100.0`。
- CMake `3.31.6`、现有 vcpkg toolchain、Catch2 `3.15.0`；没有修改第三方版本或 Golden pins。
- 原生活跃输出：`\\.\DISPLAY1`、`\\.\DISPLAY2`，均为 `2560×1440@180Hz`、32 bpp。
  原点分别为 `(0,0)`、`(2560,0)`；方向、fixed output 和 display flags 均为 0；第一屏为主屏。
- 实测 GPU 为 `NVIDIA GeForce RTX 5090 D`。两个输出属于同一 adapter。
- GPU correctness Gate 显式打开 D3D11 debug layer / test-only readback；live Gate
  和独立 Release Encoder 运行不打开这两项。WARP 仅是显式软件正确性对照。
- 测试中的 mode switch 只使用 `CDS_TEST` 和动态 flags `0`，不写注册表、不改主屏、
  不启用 unsafe modes。模式监督进程之外没有其他模式修改者。

## 最终 Gate

最后一轮使用冻结后的源码：两种配置均先构建完整默认 target，再执行完整 CTest。
构建前、测试结束后逐项比对 35 个新增/修改的代码与构建文件 SHA-256，结果一致。
下表不是用早期二进制或补丁前的通过结果替代最终复验。

| Gate | 最终结果 |
| --- | --- |
| Release 完整默认构建 | PASS，exit 0 |
| Release 完整 CTest | **64/64 PASS**，0 failed、0 skipped；413.12 秒 |
| RelWithDebInfo / MSVC ASan 完整默认构建 | PASS，exit 0；新库本体、Encoder 和测试均插桩，runtime DLL 已复制 |
| ASan 完整 CTest | **175/175 PASS**，0 failed、0 skipped；536.50 秒 |
| 呈现专项 | 两种配置各 **8/8 PASS**：timing、owner lifecycle、非 PMv2 host 拒绝、WARP、hardware、live、mode supervisor、Encoder |
| 新增 Catch2 单元覆盖 | PBPresentTiming：18 cases / 37,802 assertions；PBRenderD3D：17 cases / 321 assertions（Release 最后一轮） |
| 既有回归 | Protocol/FEC/Receiver/Golden/工具/精确无参 smoke、静态独立链接、no-Qt、构建选项和 subproject Gate 全部通过 |
| cppcheck 2.21 | 5 个 production translation units：两个库的 4 个源文件和 Encoder CLI，exit 0，无诊断 |
| 格式 / diff | clang-format 19.1.5 对 16 个新 C++ 文件 dry-run 通过；`git diff --check` 通过 |

`PBPhase0GateLarge` 沿用原有注册策略只在 Release 运行；本轮没有删除、跳过或放宽它。
ASan 的 175 项还包括既有 deterministic mutation / corpus gates，不宣称 libFuzzer/UBSan/TSan 覆盖。

## 实机结果与证据路径

最后一轮 Release 和 ASan 均完成了以下实际操作：

1. 两种 flip effect × latency 1/2；WARP 与硬件分别验证完整 GPU 字节、padded row 和 resize 后恢复。
2. 真实 HWND minimize/restore、非 canonical resize、跨屏过渡暂停、两个输出间双向迁移。
3. `DISPLAY1` 从 `2560×1440@180Hz` 切到枚举且 CDS_TEST 通过的 `120Hz`，再恢复原模式。
   正常、失败 exit 19、骤停 exit 73、超时终止 exit 74 四条路径各有恢复验证，最后再验证一次。
   每次均回读全部保存输出的模式、位置、方向、bpp、fixed output、flags 和主屏标记。

完整路径索引：
`D:\MyProjects\PixelBridge\build-presentation-release\presentation-validation-index.json`。

| 证据 | 本轮位置 |
| --- | --- |
| Release 完整日志 / JUnit | `D:\MyProjects\PixelBridge\build-presentation-release\ctest-release.log` / `release-ctest.xml` |
| ASan 完整日志 / JUnit | `D:\MyProjects\PixelBridge\build-presentation-asan\ctest-asan.log` / `asan-ctest.xml` |
| 构建日志 / 源码指纹 | 各构建根目录的 `build-release.log` 或 `build-asan.log`、`validated-source-manifest.json` |
| Release mode 恢复 | `build-presentation-release/tests/PresentationGate/Release/evidence/mode-supervisor-13196-1225085752072/gate.txt` |
| ASan mode 恢复 | `build-presentation-asan/tests/PresentationGate/RelWithDebInfo/evidence/mode-supervisor-9728-1229775230752/gate.txt` |
| Release live 原始统计 | `build-presentation-release/tests/PresentationGate/Release/evidence/live-37224-1225061021244/snapshots.jsonl` |
| QPC 负值回归的先失败 / 后通过 | `build-presentation-release/qpc-regression-before-fix.log` / `qpc-regression-after-fix.log` |
| 最终静态检查 | `build-presentation-release/cppcheck-presentation.txt`，为空表示无诊断 |

表中的相对路径均以 `D:\MyProjects\PixelBridge` 为根；其余 GPU / child / Encoder Gate
目录可从上述 JSON 索引定位。`gate.txt` 描述动作，`snapshots.jsonl` 保存对应原始快照。

最终还在全部 CTest 结束后单独执行了没有 ASan/debug/readback 的 Release Encoder：

```powershell
D:\MyProjects\PixelBridge\build-presentation-release\apps\PixelBridgeEncoder\Release\PixelBridgeEncoder.exe --data-window --frames 120 --telemetry D:\MyProjects\PixelBridge\build-presentation-release\encoder-final-b44c12828456431794d4ebc6621ee96d\telemetry.jsonl
```

结果：exit 0；120 次接受的 CPU 提交、119 次成功 Present 调用、1 个 epoch-invalidated
pending frame、0 次 latest-pending 替换。最终 epoch 为 2，显示模式仍为 `2560×1440@180Hz`。
这 **不意味着显示器实际呈现了 119 个 unique visual frames**。

该次运行原始 `PresentCount=119`，但 `PresentRefreshCount=0`、`SyncRefreshCount=0`，
因此 timing 为 `unavailable / refresh-clock-unavailable`，`PresentedVisualFPS`、
`ObservedVisualFPSLowerBound`、`PresentQueueLatency` 均为 `null`。显示契约继续有效，
不把调用率或 PresentCount 跳变解释为视觉 FPS。完整 stdout / stderr / JSONL 位于上述独立目录。

终审未留下已知 Critical/High 问题；以下明确列出的实机覆盖边界仍然保留。

## 可复现命令

```powershell
cmake -S D:\MyProjects\PixelBridge -B D:\MyProjects\PixelBridge\build-presentation-release -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake -DPB_BUILD_APPS=ON -DPB_BUILD_TOOLS=ON -DPB_BUILD_PHASE0_GATE=ON -DPB_BUILD_PRESENTATION_GATE=ON
cmake --build D:\MyProjects\PixelBridge\build-presentation-release --config Release --parallel
ctest --test-dir D:\MyProjects\PixelBridge\build-presentation-release --build-config Release --output-on-failure --output-junit release-ctest.xml

cmake -S D:\MyProjects\PixelBridge -B D:\MyProjects\PixelBridge\build-presentation-asan -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake -DPB_BUILD_APPS=ON -DPB_BUILD_TOOLS=ON -DPB_BUILD_FUZZERS=ON -DPB_BUILD_PHASE0_GATE=ON -DPB_BUILD_PRESENTATION_GATE=ON
cmake --build D:\MyProjects\PixelBridge\build-presentation-asan --config RelWithDebInfo --parallel
ctest --test-dir D:\MyProjects\PixelBridge\build-presentation-asan --build-config RelWithDebInfo --output-on-failure --output-junit asan-ctest.xml

cppcheck --enable=warning,performance,portability --std=c++20 --language=c++ --platform=win64 --library=windows --inline-suppr --suppress=missingIncludeSystem --error-exitcode=1 -I libs/PBPresentTiming/include -I libs/PBRenderD3D/include -I libs/PBRenderD3D/src -I libs/PBProtocol/include -I libs/PBModulation/include libs/PBPresentTiming/src libs/PBRenderD3D/src apps/PixelBridgeEncoder/presentation_cli.cpp
git diff --check
```

cppcheck 和 Git 命令的工作目录为 `D:\MyProjects\PixelBridge`。真实显示 Gate 必须
运行在交互桌面；模式或输出前提不符会非零退出，不会 skip 成功。

## 验证覆盖与自审收敛

| 视角 | 检查 / 发现 / 修复 | 对应证据 |
| --- | --- | --- |
| 架构 / 协议 | 两个静态库无 Qt；公共头不暴露 HWND/DXGI；复用现有 BGRA、Bootstrap 和 reference binding；无 Decoder 或 wire 变更 | 独立链接消费者、no-Qt / subproject / build-option Gate、既有 Golden 和 Phase 0 |
| 提交与所有权 | 2 个 CPU 槽、copy-before-return、latest-pending 替换；同一 owner 负责 HWND/context/resize/Present/销毁；停止可并发、析构不得与成员调用并发 | fake backend 使用生产 owner loop；并发 submit/snapshot/stop、failure immutability、exact cleanup 断言 |
| permit / 重建 | 旧 epoch 的 active raster 不得 Present；同 chain 未消费 permit 保留，跨 chain 必须重新等待；swap/buffer generation checked | 上传中环境变化、跨 adapter 元数据、首次等待、普通消息不能伪造 permit 的回归 |
| flip chain 生命周期 | 同一 HWND 替换旧 flip chain 时，必须在释放 chain/back-buffer 引用后 Flush deferred destruction | WARP 和 hardware 均在同一真实 HWND 连续重建 3 次并重新等待、上传、逐字节验证；不是物理跨 adapter 认证 |
| 大小与坐标 | checked frame/stride/span/双槽长度；最后一行不要求尾部 padding；拒绝 `INT32_MIN/CW_USEDEFAULT` 伪装为显式坐标；拒绝矩形上界溢出 | 精确尺寸/预算边界、溢出/短 span、极值和负坐标单元测试 |
| 计时真实性 | 只关联已成功 Present ID / FrameSequence；不补算跳过 ID；重复帧不增 unique；不把 duration 当 queue latency | 独立 QPC/VBlank oracle、调用率与 visual rate 不同、重复/gap/glitch/idle、wrap/half-range、ring 上界 |
| 时钟异常 | 大绝对 QPC 先整数相减；预期显示时刻在未来/epoch 外则失效；S_OK 但 refresh clock 不推进时不能不断换 epoch 丢 pending | `INT64_MAX` 附近、未来/旧时刻、连续 600 个 stalled-clock 样本、10,000 步固定种子 adversarial stream |
| QPC 错误 | 持续 `-1` 的故障注入曾使帧等待把错误值相减当成 0；改为负起点直接失败，避免无期限等待和负起点减法风险 | 新回归先失败（Catch2 exit 42），修复后 8 个断言通过；没有放宽 deadline / 删除断言 |
| HWND / 模式 | 非 canonical/minimized/跨屏过渡暂停；mode/DPI/monitor 新 epoch；旧统计不可复用；disjoint 不重建 chain | 实机 resize/minimize/straddling/双向迁移；四条监督模式恢复路径；fake DPI/device loss/wait/rebuild failure |
| 诊断输出 | 全 raw statistics、QPC、epoch/reason、coverage、native status；不可用指标为 null；JSON 不受调用者 locale/hex 影响 | formatting/escaping 测试、真实 JSONL、无调用率补算 visual rate 的测试 |
| CLI / 文件 | 有限 CPU 提交不是显示帧保证；Unicode 遥测 CREATE_NEW；文件 RAII 不可复制、仅在构造时打开；写入/flush 错误可见 | 精确无参 smoke、11 个参数拒绝测试、Encoder 正向实际 Present、已存在日志 SHA-256 不变；cppcheck 无新增抑制 |

QPC 错误是**故障注入**发现，不表示实机观察到 Windows QPC 故障。cppcheck 指出的
可复制/可重复打开的文件所有权隐患通过删除复制和构造期打开消除；没有添加测试专用生产分支。
测试 backend 的 fault seam 不安装、不属于公共 API；真实 owner loop 并未被第二套调度实现替代。

## 剩余边界

- 本轮没有 Capture、GPU modulation、GPU texture producer API、Qt 控制 UI 或完整文件发送流程。
- GPU 字节 oracle 不证明显示器输出、DWM/HDR 色彩处理、光标/遮挡或屏幕捕获正确；没有测得接收端 `UniqueVisualFPS`。
- 双屏均属于同一 adapter、实测 DPI 为 96；不同物理 adapter / DPI 比例矩阵、HDR/VRR、热拔插和真实 TDR 仍未实测。
- 数据窗口显示可继续，而 DXGI statistics 不支持、停滞或 observation 有缺口时，有效 timing 仍不可用。这不是隐藏失败或 FPS=0。
- MSVC ASan 不等于 UBSan、TSan 或 coverage-guided libFuzzer。测试中的多读写/停止并发也不等于完成形式化 race 证明。
- 库自身 wait/drain 有取消和 timeout，但无法强行抢占驱动内部阻塞 Present/Create/Destroy。
- 监督对测试子进程的正常退出、失败、崩溃、超时终止执行模式恢复；不承诺整机断电或监督进程本身被强杀后仍能执行恢复代码。

构建日志、JUnit、原始遥测和显示模式证据保留独立的 ignored build 目录；不混入源码提交。
