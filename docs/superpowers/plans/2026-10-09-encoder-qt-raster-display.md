# Encoder Qt Raster Display Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:executing-plans` to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. User instruction requires the primary agent to perform all work; do not dispatch implementers or reviewers and do not create commits.

**Goal:** 增加真正独立的 Qt 软件数据画面，以及主窗口和右键发送窗口中的显示方式切换按钮；每次新开窗口仍默认 D3D11，停止后才可切换。

**Architecture:** 保留纯 C++ `EncoderPresentation` 边界，在 Encoder 应用层实现 GUI-thread Qt raster host/adapter。编码 worker 使用有界、自有像素槽位，GUI 线程执行窗口操作和绘制；取消必须先解除创建/绘制等待，再 join worker。按实际后端验证合同和统计，原 D3D11/ComposedBlt 路径保持原约束。

**Tech Stack:** Windows x64、C++20、Qt Widgets 6.10.1 当前安装基线、Win32 物理几何、Catch2、CMake/VS2022 Release。无新增第三方依赖或版本升级。

**Spec:** [`../specs/2026-10-09-encoder-qt-raster-display-design.md`](../specs/2026-10-09-encoder-qt-raster-display-design.md)。用户已审阅并同意设计及本计划，已选择主 Agent 原位逐项实施。Task 1–5 已验证；本文为 Task 6 封存前的计划快照。后续源码封存、包验证和交付结果记录在候选证据目录的 `progress.md` / `final/HANDOFF.md`，不通过事后修改此源码清单成员来改变已封存身份。

## Global Constraints

- 每次新打开 Encoder 窗口均默认 D3D11；不恢复上次 Qt 选择，也不把 Qt 写成持久默认。
- 只允许停止后切换。准备、广播、停止、关闭清理期间不得切换。
- 主窗口和 Shell 发送窗口均覆盖标准、灰阶高速、PAM4、PAM4 Wide；原 Profile 尺寸/准入不放宽。
- Qt 是显式选择的兼容路径，不自动检测 Citrix，不自动回退。
- Qt 只在 Encoder 应用层实现；核心模块不引入 Qt 类型或链接。
- 协议、FEC、Profile 身份、Raster/Golden Vectors、Decoder、摘要/发布/重开验收不变。
- 不使用 QOpenGLWidget、Qt Quick GPU 路径或 Qt 内嵌原 D3D11 窗口冒充软件绘制。
- 原 `--gui-presentation-mode flip|composed-blt` 仍固定呈现方式；正式 measurement 不开放 Qt。
- 新隔离入口 `--gui-display-test` 严格 argc=2；包内 `display-diagnostic` INI/日志/缓存，不注册 Shell。
- 不覆盖现有候选/原包，不自动提交、打标签、推送，不更改全局 Git 身份。
- 不使用子智能体；不运行自动键鼠、捕获桌面、显示模式切换或无关全量 Gate。
- C++ 使用明确 lowerCamelCase 变量、PascalCase 自有函数、独立行大括号、不可变局部 const、`i++`；Qt 必须同名的 override 保持 Qt API 名称。

## Review Focus

1. 用户刚按开始就关闭窗口，Qt 创建事件尚未执行：停止必须唤醒 worker，不能 GUI join/worker 等 GUI 死锁。归属 Task 3。
2. GUI 事件积压、旧 generation 的创建/绘制事件在下一次开始后抵达：不能重新显示旧窗口或绘制旧帧。归属 Task 3。
3. 125%/150% DPI、负原点和非整倍数窗口尺寸：不得改变编码像素映射或偷偷裁切边缘。归属 Task 2/3。
4. 用户伪造持久设置、直接触发按钮信号或使用固定模式/measurement 入口：不能绕过默认与 active 锁定。归属 Task 4。
5. 像素 view 返回后立即失效、row pitch 带 padding、Qt 重绘同一帧：必须自持有数据，不错误推进帧序号或报告新方程。归属 Task 2/3/5。

## 基线与执行方式

当前分支为 `codex/citrix-composed-presentation`，HEAD 为 `4bd1934f8a16d8d8c7041abbaefdd8cb04024807`。工作区已有 ComposedBlt 的未提交修改及新设计文档；不能重置、checkout 覆盖或将它当干净基线。

保留当前修改所在 checkout 执行，新的构建目录为 `build-qt-raster-display-20261009`，证据目录为 `artifacts/encoder-qt-raster-display-20261009-run01`。不得清空或复用已封存候选的构建/交付目录。

提交动作不属于本计划；每个任务末尾使用工作区 diff 自审和测试记录代替 commit。若发现不属于本任务的新修改、保护文件变化或需要改变协议/默认/热切换方案，停止相应修改并询问用户。

## 文件边界与接口约定

| 文件 | 职责 |
|---|---|
| `libs/PBRenderD3D/include/pbrenderd3d/data_window.h`、`src/data_window.cpp` | 纯 C++ QtRaster 后端标识、NotApplicable swap effect、QtEventLoop pacing、通用像素校验与必要软件绘制计数；不链接 Qt |
| `libs/PBRenderD3D/src/presentation_json.cpp` | 真实后端合同与 DXGI 不适用/null 序列化 |
| `apps/common/local_desktop_runtime.h/.cpp` | Qt 配置准入、工厂边界、独立 Qt 合同与 snapshot 映射；原 native 约束保留 |
| `apps/common/application_model.h`、`run_report.cpp`、`evidence_journal.cpp` | Qt 本地绘制计数/后端身份，不把它们冒充 Present |
| 新 `apps/PixelBridgeEncoder/qt_raster_paint.h/.cpp` | 有界物理像素输出与 Qt raster 绘制共用 helper，可在 QImage 上离屏验证 |
| 新 `apps/PixelBridgeEncoder/qt_raster_presentation.h/.cpp` | GUI-thread host、内部 QWidget、worker adapter、有界帧槽位、generation/取消/清理 |
| 新 `apps/PixelBridgeEncoder/encoder_display_options.h` | 两种可选显示方式及切换准入的纯 C++ 小策略；不保存偏好 |
| `encoder_application_controller.h/.cpp` | GUI host 所有权、工厂路由、停止前取消与 join 后清理 |
| `encoder_gui.cpp`、`encoder_shell_dialog.cpp`、`main.cpp` | 两个按钮、模式写入配置、严格隔离入口与 smoke 参数 |
| `apps/PixelBridgeEncoder/CMakeLists.txt`、`tests/PBApplication/CMakeLists.txt`、`tests/CMakeLists.txt` | 应用层 Qt 源码、新测试/部署注册，不改协议依赖基线 |
| 新 `tests/PBApplication/test_qt_raster_paint.cpp`、`test_qt_raster_presentation.cpp`、`qt_raster_test_support.h/.cpp` | 像素、DPI、并发和隐藏窗口测试；测试入口不向产品配置暴露 fake/hidden 选项 |
| 现有 `test_encoder_presentation.cpp`、`test_run_report.cpp`、`test_operational_logging.cpp` 及 GUI smoke | 配置、报告、默认路径和 UI 集成回归 |

核心接口约定如下，后续任务保持名称/类型一致：

- `pbapp::CreateNativeEncoderPresentation(const pbrenderd3d::DataWindowConfig&) -> std::unique_ptr<pbapp::EncoderPresentation>`：复用原 native adapter，供应用路由使用。
- `pbrenderd3d::ValidateCanonicalBgraPixels(std::uint32_t width, std::uint32_t height, std::uint64_t maximumFrameBytes, const CanonicalBgraFrameView&) noexcept -> PresentationStatus`：独立于后端创建的安全像素校验。原 `ValidateCanonicalBgraFrame` 保留原配置校验，再调用该 helper。
- `pbencoder::PaintQtRasterFrame(QPainter&, const pbrenderd3d::DataWindowConfig&, const pbrenderd3d::CanonicalBgraFrameView&, std::uint32_t physicalWidth, std::uint32_t physicalHeight, double devicePixelRatio, QImage& physicalScratch) noexcept -> pbrenderd3d::PresentationStatus`：scratch 定额复用；输入 bytes 不被修改。
- `pbencoder::QtRasterPresentationHost(QObject* parent = nullptr)`；`BeginRun() -> void`、`CreatePresentation(const DataWindowConfig&) -> std::unique_ptr<EncoderPresentation>`、`RequestStop() noexcept -> void`、`FinalizeStop() noexcept -> void`、`HasPendingWork() const noexcept -> bool`。除 worker 调用的 CreatePresentation 与可取消共享状态外，窗口所有权/操作限定 GUI 线程；HasPendingWork 表示实际创建/运行/GUI 清理尚未完成，不是独立的 UI busy 猜测。
- `pbencoder::GetNextEncoderDisplayMode(PresentationModel) noexcept -> std::optional<PresentationModel>`：Flip/QtRaster 双向切换；ComposedBlt/未知值不属于按钮循环。
- `pbencoder::CanChangeEncoderDisplayMode(pbapp::EncoderState, bool closePending, bool fixedPresentation, bool measurement) noexcept -> bool`：纯状态准入，用于两个 UI 和直接调用回归。

新共享枚举追加 `PresentationModel::QtRaster`、`PresentationSwapEffect::NotApplicable`、`PresentationPacing::QtEventLoop`；不重排旧值。软件绘制计数使用 `softwarePaintCalls`（全部 paint 调用）和 `softwareFramesPainted`（首次成功绘制某个逻辑帧），不复用 DXGI 计数。增加 `PresentationContract::rasterPaintEngine` 说明真实 Qt raster 合同；原 WARP 专用 `softwareRasterizer` 字段不改语义。

## 构建与测试命令约定

从仓库根执行。以下命令是实施阶段操作，本计划阶段不执行构建。

配置前从 `build-citrix-presentation-20261007/CMakeCache.txt` 读取 `CMAKE_TOOLCHAIN_FILE`、`PB_QT_ROOT`、`VCPKG_INSTALLED_DIR` 三个现有值。任何缺失、路径不存在或基线版本不匹配都停止，不猜路径、不升级/下载依赖。记为 `$toolchainFile`、`$qtRoot`、`$installedDependencies` 后执行：

```powershell
cmake -S . -B build-qt-raster-display-20261009 -G "Visual Studio 17 2022" -A x64 "-DCMAKE_TOOLCHAIN_FILE=$toolchainFile" "-DPB_QT_ROOT=$qtRoot" "-DVCPKG_INSTALLED_DIR=$installedDependencies" -DVCPKG_TARGET_TRIPLET=x64-windows -DVCPKG_MANIFEST_INSTALL=OFF -DPB_BUILD_APPS=ON -DPB_BUILD_TESTS=ON -DPB_BUILD_TOOLS=OFF -DPB_BUILD_BENCHMARKS=OFF -DPB_BUILD_FUZZERS=OFF -DPB_BUILD_PRESENTATION_GATE=OFF -DPB_BUILD_WGC_GATE=OFF -DPB_BUILD_DXGI_GATE=OFF -DPB_BUILD_SCREEN_REGION_GATE=OFF -DPB_BUILD_LOCAL_DESKTOP_GATE=OFF -DPB_BUILD_DESKTOP_LEVELS_GATE=OFF -DPB_BUILD_PROCESS_RECOVERY_HARNESS=OFF
```

每个 RED/GREEN 运行保存完整退出码与输出；MSVC 构建在同一目录串行发起，不启动两个同时写该目录的 build。单次 build 可使用 `--parallel 4`。

## Task 1：纯 C++ 呈现合同、工厂与配置准入

**Files:** 上表共享呈现/runtime/model/report 文件；`tests/PBApplication/test_encoder_presentation.cpp`、`test_run_report.cpp`；已有 `tests/PBRenderD3D/test_data_window.cpp`。

**Interfaces:** 产生新枚举/计数、CreateNativeEncoderPresentation 和 ValidateCanonicalBgraPixels；不产生任何 Qt 链接。

- [x] **RED：添加明确配置和报告测试。** 名称 `Qt raster is explicit and native creation rejects it`、`Qt raster reports paints without DXGI statistics`；四个 GUI Profile 的 Qt 配置可在 GUI 工厂下启动，未知 Profile/measurement/默认 native 工厂的 Qt 启动在源文件准备前拒绝。保留旧 Flip/Blt 负向矩阵。

```cpp
CHECK(pbapp::MakeUnifiedEncoderConfig(L"sample.bin").presentationModel == pbrenderd3d::PresentationModel::Flip);
CHECK_FALSE(pbrenderd3d::ValidateDataWindowConfig(qtConfiguration));
CHECK(pbrenderd3d::GetPresentationModelName(pbrenderd3d::PresentationModel::QtRaster) == std::string_view("qt-raster"));
```

`qtConfiguration` 在该测试内声明为默认 `DataWindowConfig` 并显式设置 `presentationModel = QtRaster`；第二条断言证明 native 配置校验没有放宽为创建 Qt。

- [x] **验证 RED。** 构建/运行 `PBEncoderPresentationTests` 与 `PBRenderD3DTests`；首次新类型尚不存在导致编译失败可记录为接口 RED，随后必须继续取得行为断言 RED，不将缺文件当完成。
- [x] **实现共享合同。** 抽取安全像素校验，原 native Validate/Create 仍拒绝 Qt；独立校验 Qt 配置的尺寸/预算/超时和四个 GUI Profile。默认 Runtime 无 GUI factory 时拒绝 Qt；显式注入后仍验证实际 Qt 合同，不把“注入了 factory”当成正确性证明。
- [x] **实现工厂与报告。** 抽出原 native adapter 的创建函数。Runtime 的 Qt 合同分支不要求不存在的 DXGI buffer/latency/adapter；原 D3D 分支断言不删。Qt 报告 `QtRasterHasNoDxgiStatistics`，DXGI 相关 rate/id/latency 为 null，本地绘制计数独立输出；不能用 `bufferCount=2` 冒充 swap chain 才让 snapshot 被发布。
- [x] **GREEN 与自审。** `cmake --build build-qt-raster-display-20261009 --config Release --target PBEncoderPresentationTests PBRenderD3DTests PBApplicationTests --parallel 4`；运行上述两个 CTest 和 `PBApplicationTests "[report],[validation]"`。检查 core targets 没有新增 Qt 依赖。

## Task 2：像素/DPI helper 和离屏 oracle

**Files:** 新 `qt_raster_paint.h/.cpp`、`test_qt_raster_paint.cpp`、测试支持文件与应用/测试 CMake。

**Interfaces:** 消费 Task 1 的安全像素校验与既有 `ResolvePresentationViewport`；产生 PaintQtRasterFrame。

- [x] **RED：新增 `PBQtRasterPresentationTests` 测试目标。** 测试进程只创建一个 QApplication；默认 CTest 使用 offscreen 平台，部署该平台插件。先写 `Qt raster preserves canonical pixels at fractional DPR`、`Qt raster validates padded rows and resource bounds`。
- [x] **写清 oracle。** 使用独立的像素中心 point-sampling 算法逐像素计算期望值，不用 Qt 的 resize/draw 结果生成期望。矩阵 DPR=`1.0,1.25,1.5,2.0`，scale=`1.0,1.5,2.0`、小于最小 client、静态边缘、非整倍数 client、padded row、零/超限尺寸、pitch/length 溢出；成功输出的可见 RGB 和不透明 alpha 必须匹配，输入 bytes 不改变。
- [x] **验证 RED。** `cmake --build build-qt-raster-display-20261009 --config Release --target PBQtRasterPresentationTests --parallel 4` 后 `ctest --test-dir build-qt-raster-display-20261009 -C Release -R '^PBQtRasterPresentationTests$' --output-on-failure`；保存未正确处理 DPR/像素映射时的断言失败。
- [x] **实现最小软件绘制。** 使用物理输出 scratch 和既有 viewport 规则，复用定额内存；禁止平滑插值/透明混合/额外色彩变换。DPR 只用于将已确定的物理输出准确交给 Qt，不能改 Profile raster。QPainter/Qt 异常转换为明确失败。
- [x] **GREEN。** 运行像素矩阵，并加入四个现有 GUI Profile 的代表性 raster 和旧 WARP/reference 输出对照。断言整幅 bytes，不只抽样中心像素；不重写 Golden。

## Task 3：Qt host、worker adapter 与有界生命周期

**Files:** 新 `qt_raster_presentation.h/.cpp`、`test_qt_raster_presentation.cpp`、`qt_raster_test_support.h/.cpp`。

**Interfaces:** 消费 PaintQtRasterFrame；产生 QtRasterPresentationHost 及 EncoderPresentation 实现。测试专用 friend/access seam 可控制隐藏显示和环境事件，不通过产品配置开放 fake 或可见桌面自动化。

- [x] **RED：先写停止/创建竞态测试。** `Stop before queued Qt creation never blocks GUI join`：让 worker 发出创建请求但暂不处理 GUI 事件，GUI RequestStop 后 worker 必须在测试 deadline 内结束；随后处理旧事件也不能出现窗口。另测创建超时（配置 100 ms）、重复停止、失败后重新开始。
- [x] **RED：队列与所有权。** `Qt presentation owns submitted pixels and bounds wakeups`：Submit 后覆盖调用方源内存，后续 paint 仍得到原图；GUI 暂不处理事件时持续提交只能保留一个待帧和一个待唤醒，不随提交次数增长；反复重绘不增加逻辑帧序号。Catch 断言在测试主线程执行，worker 仅返回结果。
- [x] **实现 host/adapter。** QWidget 只在 GUI 线程创建/绘制/销毁；worker 使用自有双槽和纯 C++ 状态。创建握手响应 RequestStop 和配置 timeout；generation 隔离旧事件；不使用 BlockingQueuedConnection，不在 paintEvent 里 processEvents/repaint 形成重入。
- [x] **实现环境/epoch。** 用 Win32 实际 client/monitor 几何复核 Qt DPR。创建无效、跨屏/模式/DPI变化、client 过小、最小化/恢复和关闭时按合同暂停/失效或失败；新 epoch 不得消费旧帧。不要将 rasterPaintEngine 与 WARP 标识混用。
- [x] **实现有界停止。** RequestStop 取消初始化/提交，逻辑丢弃 pending；正在绘制的数据必须活到绘制退出。FinalizeStop 在 GUI 线程、worker join 之后释放窗口和存储，重复调用安全。暴露 terminal 前核对 pending/in-flight 均清空。
- [x] **GREEN 与隐藏窗口验证。** 同一测试程序另注册 `PBQtRasterHiddenWindowTests`，仅用 Windows 平台隐藏窗口验证真实 HWND/物理 client/raster engine/重复创建清理；测试专用隐藏模式绝不 Show/activate。独立测试超时，不冒充实屏或 Citrix。

## Task 4：控制器、两处切换按钮和严格隔离入口

**Files:** 新 `encoder_display_options.h`；`encoder_application_controller.h/.cpp`、`encoder_gui.cpp`、`encoder_shell_dialog.cpp`、`main.cpp`；扩展 GUI smoke 与 `tests/CMakeLists.txt`。

**Interfaces:** 消费 QtRasterPresentationHost 和 Task 1 native factory；产生两个纯策略函数及按钮配置接线。

- [x] **RED：策略与默认。** 对 Idle/Stopped/Failed 可编辑状态、Preparing/Broadcasting/Stopping、close-pending、fixed diagnostic、measurement 做完整矩阵。Flip/Qt 双向切换，Blt/未知值返回 nullopt。

```cpp
CHECK(pbencoder::GetNextEncoderDisplayMode(pbrenderd3d::PresentationModel::Flip) == pbrenderd3d::PresentationModel::QtRaster);
CHECK_FALSE(pbencoder::GetNextEncoderDisplayMode(pbrenderd3d::PresentationModel::ComposedBlt));
CHECK_FALSE(pbencoder::CanChangeEncoderDisplayMode(pbapp::EncoderState::Preparing, false, false, false));
```

- [x] **RED：GUI 接线。** 两个窗口初始均 native；点击改变 BuildConfiguration 的实际 model；新窗口重新 native；在 INI 中人为写入 Qt 相关值也不会恢复选择。active 时直接触发 clicked 信号不能改变配置，固定诊断/measurement 入口同样不能绕过。另测 Runtime 已公布 Stopped、GUI 尚未 FinalizeStop 的间隙，按钮和重复 Start 仍须被拒绝。
- [x] **实现控制器路由与 stop 顺序。** 默认 GUI controller 持有 host，工厂仅在明确 Qt 模式时交给 host；native 交给原 factory。外部测试 factory 保持显式测试语义。Start 准入/失败、RequestStop、StopAndGetSnapshot、终态 PollSnapshot、析构全部覆盖“先取消 host 等待、再 Runtime join、最后 GUI FinalizeStop”。Controller 的 IsActive/Start 准入同时检查 Runtime 活动状态和 host 的 HasPendingWork，保证清理间隙不能开始或切换；它不改变核心 Runtime 的协议状态机。
- [x] **实现按钮。** 使用已确认两条标签，只保存窗口内状态。Start 时固定该次选择；不得在源预检/文件夹归档后重新读取一个可能改变的模式。Shell 为新增控件调整固定高度/布局，保持原文件夹 ZIP 与取消语义，不开展无关归档重构。
- [x] **实现隔离入口。** 在 QApplication 创建前严格解析 `--gui-display-test`，拒绝附加参数和冲突。解析 INI 成功后才写缺省值，隔离日志/缓存并跳过 Shell 注册；旧两个固定模式命令保持不变。新增 `--gui-qt-display-smoke` 供隐藏/离屏集成测试，不执行真实传输/注册。
- [x] **GREEN。** 运行 `PixelBridgeEncoderGuiSmoke`、新 `PixelBridgeEncoderQtDisplaySmoke`、`PBEncoderShellTests`、`PBQSettingsTests`、`PBEncoderPresentationTests`。另以离屏渲染核对主/Shell 控件在 100%/125%/150%/200% 下标签和按钮不截断；不自动操作可见桌面。

## Task 5：真实 Runtime 接线、日志及四模式回归

**Files:** 应用层集成测试、`test_run_report.cpp`、`test_operational_logging.cpp`；必要的 GUI 实际后端状态文本。

**Interfaces:** 使用前四任务的生产接口；只在测试目标注入合成显示环境/离屏渲染，不为产品增加第二条像素通道。

- [x] **RED：真实 worker 到 Qt paint。** 对四种 GUI Profile 使用小的不可变测试源，由真实 Runtime 生成 BGRA，经 Qt adapter/paint 后与独立 oracle 比较；验证不是只测按钮文字或 fake factory。校验创建/绘制后 Requested=Actual Qt，原 native 开始默认不变。
- [x] **RED：状态与统计。** 即使 Qt paint 增长，DXGI Present/rate/latency 仍不适用/null；系统重绘不生成新的方程/FrameSequence。停止/失败最终日志包含实际后端和完整清理结果，paint 计数不作为文件成功条件。
- [x] **验证失败并修复接线。** 尤其检查原 snapshot 发布以 `bufferCount != 0` 为条件的假设、核心稳定合同排斥软件路径的旧条件、active 状态与 GUI 清理完成次序；用后端分支修正，不删除 native 约束。
- [x] **GREEN：受影响目标。** 构建 Encoder、PBQtRasterPresentationTests、PBEncoderPresentationTests、PBApplicationTests、PBOperationalLoggingTests、PBQSettingsTests、PBEncoderShellTests、PBGuiVisualModeTests、PBRenderD3DTests 及现有 PAM4/广域相关测试。执行精确 CTest 列表，不运行无关完整套件或可见 Gate。
- [x] **保留证据边界。** 这证明本地 worker/Qt/像素与逻辑正确性；真实 WGC/Citrix 端到端文件验收仍是现场事项。若 native/reference 或既有 Golden 回归失败，先修复，不排除失败用例。

## Task 6：文档、封存与独立候选

**Files:** `docs/PRESENTATION.md`、总体设计的实际数据路径/Qt应用层边界说明；新证据目录下的构建/启动/封存脚本和 README。受保护报告不修改。

- [x] **更新文档。** 清楚区分默认 native、固定 ComposedBlt 诊断、显式 Qt 软件路径；按钮/每次默认/停止后切换/Qt统计不适用/故障与正常回归边界一致，不改变 Profile 合同。
- [x] **自审整个 diff。** 按 use-after-free、GUI/worker 等待环、overflow、队列/Qt事件增长、旧 epoch、尺寸/DPR/alpha、错误路径和隐式回退逐项检查。由主 Agent 自审，不伪称独立审查。
- [ ] **封存源码身份。** 重新生成精确文件清单/源码指纹，包含本轮新文件，不能继续把 `06adff37b261` 当作新候选身份。以新指纹重建 Encoder，保存新 EXE/依赖/ZIP 摘要和完整未提交 diff。
- [ ] **独立打包。** 新 ZIP、新目录，启动器调用 `--gui-display-test`。在新解压目录验证清单、严格参数、`--version`/`--build-identity`/原 wire profile、offscreen smoke、初始化默认 D3D 与 Qt 切换；证明旧偏好和 Shell 注册未变。正式 release 的 clean-tree 限制不修改。
- [ ] **复核保存。** HEAD/暂存区、受保护文件摘要、原 `06adff37b261` ZIP 摘要及旧包均不变。报告实际测试、已知限制和新的源/包身份；不能因打包通过宣布 Citrix 冻结修复。
- [ ] **交付用户。** 给新包和详细说明；当前无需刻意复现冻结，可先做 Qt 正常态启停/小文件回归。原始远控链路的 digest → safe publish → reopen 和独立长度/SHA-256 未得到现场证据前保持未验收。

## 计划自审结果

- 设计第 1/4 节交互由 Task 4 覆盖；第 3/5 节范围与核心边界由 Task 1/4/5 覆盖。
- 第 6 节创建、队列、取消和销毁由 Task 3/4 覆盖；第 7 节像素/DPI/资源由 Task 2/3 覆盖。
- 第 8 节统计/错误由 Task 1/5 覆盖；第 9 节各回归与封存由 Task 2–6 覆盖。
- Review Focus 的五项均有对应 RED/边界测试；类型/函数名称在接口表和任务间一致。
- 无需等待原冻结复现才实现。无自动系统设置修改、无 Git 提交、无子智能体、无新 payload 通道。
