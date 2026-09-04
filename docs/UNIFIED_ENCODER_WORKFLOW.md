# Unified Encoder 产品工作流（G15）

> 状态：2026-09-04，G15 的定向 headless/controller/GUI smoke 验证通过。
> 边界：已接通 Encoder 产品发送路径；不代表 G16 Decoder GUI、G20 实屏、GPU/远程通道或吞吐门禁通过。

## 1. 产品入口与真实绑定

无参数运行 `PixelBridgeEncoder.exe` 打开 Qt Widgets 界面。基础区只有源文件、逻辑刷新率 `1..60 Hz`（默认 15）、开始/停止，以及本地准备/广播状态。

- `MakeUnifiedEncoderConfig` 是 Qt 与 CLI 共用的产品策略；固定 `PB-Unified-LC4-V1`、layout 8、自动 RAW/zstd(level 3)、Control repetitions=4。
- `EncoderApplicationController` 仅调用 Qt-free `EncoderRuntime`，不包含协议、压缩、FEC 或 raster 实现。
- 基础状态来自 `EncoderSnapshot`：文件大小、Segment 数、预扫描字节/段数/耗时/速度、Carousel pass/ordinal、实际逻辑帧的本次广播平均 FPS、源文件稳定性。预扫描百分比不是 Decoder 接收进度。
- 高级信息只读：Profile、Robust Inner FEC、当前 Segment 的 Outer FEC、整个文件 RAW/zstd 决策计数、Session/resume、whole-file BLAKE3 与 durable lease。
- 移除 Encoder 产品界面的 Profile、compression tuning、backend、monitor safety、provider 实验选项；不显示对端 ETA、接收百分比或“已发送完成”。
- `QSettings` 只保存窗口位置、源文件路径、`unifiedLogicalFps` 和 `unifiedAdvancedExpanded`；旧 compression/profile/FPS 实验键不会改变新产品策略。
- 显式 `direct/shape/remote/remote-lf4` CLI 诊断入口和 `EncoderConfig` 的历史默认保持兼容；产品 GUI 与默认 CLI 都显式绑定 Unified。旧 `GetVisualProfileOptions()` 列表暂只服务历史诊断选择器，避免在 G15 给尚未迁移的 Decoder GUI 加入假选项；`FindVisualProfileOption`/token parser 已识别 `unified`。

CLI 共用同一 runtime，例：

```powershell
# 这是实际显示广播命令；G15 验证未运行此命令。
.\PixelBridgeEncoder.exe --headless-broadcast --source D:\fixtures\sample.bin --logical-fps 15 --origin 2560 100 --seconds 30
```

默认 `--profile unified`，未指定 FPS 时为 15。`--origin` 对 Unified 可选，省略时由既有 DataWindow 默认居中策略放置。显式 legacy tuning 与 Unified 固定策略冲突时拒绝，而非静默忽略。

## 2. 发送链路与提交边界

1. 以只共享 read 的 Win32 源文件句柄完成整个文件的 8 MiB 分段预扫描；冻结正式 Session/Segment/Manifest、精确压缩字节摘要、FEC descriptor 和依赖身份。
2. 预扫描失败时不建立新 Session、不调用 presentation factory；已存在的恢复状态不被普通失败或停止删除。Session 成功持久化后立即发布其身份，即使后续 presenter 创建失败，也能解释和管理这个真实 Session。
3. 复用 `SenderFrameBuilder` 的 current/next 双 Segment 缓冲与原 durable lease；不建立平行发送管线。
4. Unified 分支调用 `SenderUnifiedCarouselScheduler`，把原 PB-Control-1 与 Transport 放入 31 个 mixed slots，再调用现有 `EncodeUnifiedVisualFrame`。Control 不分配 OuterBlockId，尾部 filler 只重复 systematic ID；0-byte 无 Transport equation。
5. `PrepareFrameAt(logicalTick, monotonicNanoseconds)` 以单调时间决定长 round 的约 10 秒 Control cadence，与运行中 FPS 修改/丢弃 ticks 解耦。历史 tick API 保留；同一 round 不允许混用时间基准。重试 pending frame 时冻结原 slot plan 与时间。
6. 只有完整 raster 被 `SubmitFrame` 成功接收后，才 commit scheduler/clock 并推进 FrameSequence、equation 和 Carousel。epoch mismatch/窗口太小时保留同一 pending raster，重复 Present 不推进发送状态。精确 round frame 总数随时间而变，因此 `cycleFrameCount=0` 表示不可提前确定，不伪报预计值。
7. `EnsureFrameSequenceLease` 成功后立即刷新快照中的 durable endpoint，不等第一次 Submit 成功才显示。

`EncoderPresentation` 是受控的应用层依赖注入边界。默认实现只是既有 `DataWindow` 的薄适配器；产品配置没有选择 mock 的开关。无窗口测试替换此边界，但使用真实的源文件、预扫描、session store、FEC、mixed-slot scheduler 和 raster 生成器。

## 3. 普通停止与显式删除

- 普通停止和关闭窗口只停止/join worker 与 presentation owner，保留 Session；重新开始完整校验源文件、内容、descriptor 与压缩/FEC 依赖后才恢复，ID 从持久 lease endpoint 继续。
- 在停止后，通过“会话 → 结束并删除会话…”执行删除，默认取消，确认框显示实际 SessionId 和对应源文件；源文件从不删除。
- Qt 只调用 controller/runtime。`EndAndDeleteSession(expectedRunGeneration)` 拒绝活动 run 和过期确认，join 已到终态的 worker 后调用 store API，避免终态快照与 owner 释放之间的尾部竞态。
- Store 使用临时 SourceIndex 操作锁和整个 Session 生命周期的独占 owner 锁。删除拒绝活跃 owner、非精确 ID、reparse 目录/文件、额外未知文件、损坏的 descriptor/index/end marker；路径父目录在文件删除期间被 Win32 handle 固定。
- 只删除该 Session 的已知状态文件；只有 SourceIndex 仍指向该 Session 时才移除对应项。索引已指向更新 Session 时保留它。
- 先持久化含校验 descriptor 的 `ended.descriptors`，再移除 index/state；中断后不可恢复这个已结束 Session，可以从合法 marker 继续显式清理。损坏 marker 仍 fail closed。
- 新 Session 的 index publish 失败时清理本次创建的已知文件与目录，旧 index/Session 不变。不可恢复的底层清理失败/内存耗尽可能保留未索引的残留，不能作为可恢复 Session 或成功删除报告；不递归清理未知文件。

## 4. 最小验证与证据

在仓库根目录执行：

```powershell
cmake --build build-unified-release --config Release --target PixelBridgeEncoder PBApplicationTests PBUnifiedSenderSchedulerTests --parallel 6
.\build-unified-release\tests\PBApplication\Release\PBApplicationTests.exe '[application][g15]' --rng-seed 15092026 --durations yes
.\build-unified-release\tests\PBApplication\Release\PBApplicationTests.exe '[application][encoder][sender][session][persistence]' --rng-seed 15092026
.\build-unified-release\tests\PBApplication\Release\PBUnifiedSenderSchedulerTests.exe --rng-seed 15092026
ctest --test-dir build-unified-release -C Release -R '^PixelBridgeEncoderGuiSmoke$' --output-on-failure -V
```

结果：G15 11 cases / 563 assertions；既有 store 2 / 164；mixed scheduler/clock 5 / 332658；GUI smoke 1/1（最终复核 0.17 s）。

G15 小 fixture 为 0-byte、20,000-byte RAW/Wirehair、8 MiB+1-byte（zstd + RAW 两段）。从实际 runtime 提交的 raster 经独立 CPU oracle 自分类、Receiver 校验、PBStorage Publish 后重新打开并比对完整字节和 BLAKE3。额外验证 FPS pending 更新、epoch retry raster/ID 不变、源写锁、Session 恢复/lease 跳号/删除后新 Session、索引替换失败和删除负例；这不是进程崩溃或硬件显示测试。

GUI smoke 使用 `offscreen` Qt platform、临时 QSettings/源文件/Session root、真实 widget action 和真实 controller；只替换 presenter 为不显示的等待 owner。覆盖基础控件与只读高级区、旧设置不影响 Unified、准备完成后才能创建 presenter、动态 FPS、普通停止保留、取消删除、确认删除以及源文件保留。

- `build-unified-release/g15-build-reviewed.txt`（最终审查后的 Encoder/application 定向构建；scheduler 构建见 `g15-build-final.txt`）
- `build-unified-release/tests/PBApplication/g15-workflow-reviewed.txt`
- `build-unified-release/tests/PBApplication/g15-store-compatibility.txt`
- `build-unified-release/tests/PBApplication/g15-scheduler-compatibility.txt`
- `build-unified-release/g15-gui-smoke-reviewed.txt`

首次构建遇到 C++20 `u8path` deprecation-as-error，已改用 `u8string` path 构造。首轮 G15 测试发现 pending lease 快照滞后，并发现 100-byte 重复尾段实际也会压缩；已修正实现并使用 1-byte 尾段覆盖 RAW。首次 GUI smoke 因未部署 `qoffscreen.dll` 超时；CMake 现显式部署该插件，smoke 在 QApplication 前检查缺失插件并直接报错，最终 offscreen 重跑通过。初始日志保留，未通过延长超时、跳过断言或放宽退出条件规避失败。

未执行：全量 CTest、native/实屏/双屏窗口 Gate、真实 ROI/capture、GPU parity、远程 provider、20 GiB、进程故障注入及安装包复验。Qt 部署仍提示 `VCINSTALLDIR` 未设置，但构建返回 0。下一目标为 G16；G17 再统一报告/telemetry 字段，G20 再做实屏验收。
