# Encoder Qt 软件显示后端与切换按钮设计

日期：2026-10-09

状态：用户已审阅并同意本设计与实现计划；实施进行中。

## 1. 目标与已确认交互

为 Encoder 增加一条真正不同于现有 D3D11 swap chain 的 Qt 软件绘制路径，用于远控图形兼容性对照。不能只将原 D3D11 HWND 嵌入 Qt 窗口后改名，也不采用 Qt OpenGL 绘制作为本次兼容路径。

用户已确认：

1. 当前 D3D11 显示方式保留为默认。
2. 增加显示方式切换按钮；主窗口和右键快捷发送窗口采用相同规则。
3. 只允许停止后切换。准备、广播、停止、关闭清理期间不得切换。
4. 每次新打开 Encoder 窗口均默认 D3D11；不恢复上次 Qt 选择，也不把 Qt 写成持久默认。
5. Qt 是显式选择的兼容路径，不自动检测 Citrix，不自动回退。

实现不能以重现原先的间歇性冻结为前置条件。正常态的像素、传输、启停回归与故障态的修复效果是不同证据。后者仍需实际远控现场验证，不能从本地测试推导。

## 2. 方案选择

| 方案 | 是否采用 | 理由 |
|---|---|---|
| QWidget + QImage + QPainter raster 绘制 | 采用 | 与应用显式管理的 D3D11 swap chain 形成较清晰的呈现对照，复用既有 CPU BGRA 码面 |
| QOpenGLWidget / Qt Quick GPU 绘制 | 不采用 | 又引入 GPU 呈现路径与图形后端选择，不能达到本轮最直接的软件绘制对照目的 |
| 在 Qt 内嵌原 D3D11 窗口 | 不采用 | 只改变窗口外壳，不改变导致兼容性疑问的应用绘制路径 |

Qt 软件绘制最终仍经过 Qt 平台插件、Windows 桌面与既有远控软件。不得宣称完全绕过 GPU、DWM 或 Citrix，也不得未经测试宣称冻结已消除。

## 3. 范围与不变项

### 本轮范围

- 普通 Encoder GUI 中的标准、灰阶高速、PAM4、PAM4 Wide 都经过同一显示方式选择逻辑；不把新按钮限制为只有一个传输模式可用。
- 保持各 Profile 的既定物理码面、窗口几何、映射与启动准入规则。不同模式本来具有的尺寸或显示器要求不会因 Qt 模式而消失。
- 主窗口与 Shell 发送窗口的按钮、状态锁定、错误处理和实际显示方式信息。
- Qt 后端、应用呈现工厂接线、后端真实合同/统计、回归测试与独立候选交付。
- 新增隔离 GUI 测试入口 `--gui-display-test`：严格只接受该单一参数，默认 D3D11，允许在 GUI 中切换 Qt；使用包内独立 `display-diagnostic` INI、日志和会话缓存，跳过 Shell 注册。

### 不变项

- 默认 native D3D11 flip 路径及已有 ComposedBlt 诊断行为保留。
- 原 `--gui-presentation-mode flip|composed-blt` 仍是固定模式诊断入口，不改变其参数语义或放宽配置组合。
- CLI/core 默认、协议、FEC、Profile 身份、Raster/Golden Vectors、Decoder 和最终文件校验不变。
- Qt 只在 Encoder 应用层实现。协议、FEC、调制、捕获、存储及应用运行时核心不引入 Qt 链接或 Qt 类型。
- 不改变显示器模式、DPI、Citrix/驱动设置，不注入鼠标键盘，不自动操作远端。
- 不创建 Git 提交、标签或推送；旧包与封存证据不覆盖。

## 4. UI 合同

### 按钮

显示当前选择，点击后在以下两项间切换：

- `显示方式：D3D11（默认）`
- `显示方式：Qt 软件绘制（兼容）`

按钮提示说明：仅改变发送端画面绘制，不改变接收端模式；选择 Qt 后仍需 Decoder 按原传输模式接收。

### 锁定与默认

- 使用现有 active/close-pending 状态源，不另建一个可能与 Runtime 不一致的 UI busy 标记。
- Preparing、Broadcasting、Stopping 和关闭未完成时按钮禁用；事件处理函数也重复检查状态，不能只依赖控件禁用。
- 初始化时选择 D3D11。切换选择只保存在该窗口的内存状态中。
- 停止完成后，同一窗口可再次选择并重新开始；创建新窗口则重新默认 D3D11。
- 固定 flip/blt 诊断入口显示其固定模式且不允许按钮改变模式。
- 原正式测量入口不开放 Qt；如果配置被绕过 UI 构造出来，仍须在启动准入中明确拒绝。不能让 Qt 冒充旧 D3D Gate。

### 可观察性

开始前显示所选方式，运行信息和报告显示实际方式。初始化失败清楚标明后端与阶段，不将未成功创建的 Qt 窗口报告为运行成功。

## 5. 架构与数据流

现有 `EncoderPresentation` / `EncoderPresentationFactory` 是接入边界。保留 Native adapter；在 Encoder 应用层增加 Qt adapter 与 GUI-thread host。控制器为实际 GUI 配置选择正确实现，测试仍可显式注入测试工厂。

```text
既有编码/光栅生成（不变）
    -> CanonicalBgraFrameView
    -> EncoderPresentation
         -> 默认 Native D3D11 DataWindow
         -> 显式 Qt raster adapter
              -> 进程内有界、拥有数据的帧缓冲
              -> GUI 线程 QWidget paintEvent
              -> QImage/QPainter raster
    -> 实际可见像素 -> 原远控链路 -> 原 Decoder
```

共享的纯 C++ 呈现类型可增加 Qt 后端标识及其不适用的 swap-chain/计时语义，但不能将 Qt 对象带入这些类型。Native `DataWindow::Create` 不能接受 Qt 配置并假装创建了 Qt 后端；Qt 由应用层工厂显式提供。

合同验证按后端分支：

- 原 D3D11 的实际 swap effect、waitable、latency、尺寸、资源规则保持原断言。
- Qt 验证真实的物理 client、raster 绘制方式、像素/几何、线程与队列合同，不伪造 DXGI swap chain 参数来满足旧断言。
- 未知后端、请求/实际后端不匹配和初始化不完整均拒绝。

## 6. Qt 生命周期与并发

### 线程归属

- QWidget 的创建、显示、paintEvent、隐藏、销毁全部在 Qt GUI 线程。
- 编码 worker 不直接访问 QWidget，不在 worker 中绘制 QWidget。
- worker 只接触纯 C++ 的共享状态和拥有像素所有权的有界槽位。QImage/像素存储在被绘制期间不得被另一线程修改。
- 不使用 `BlockingQueuedConnection` 完成跨线程创建/销毁；GUI 线程 join worker 时不能让 worker 反向等待 GUI 执行清理。

### 初始化与停止

- Host 在控制器生命周期内由 GUI 线程持有。每次启动使用独立 generation，旧运行事件不能创建或激活新运行窗口。
- worker 发起的创建请求通过一次有界控制通知交给 GUI；创建握手等待必须同时响应取消并遵守 `waitTimeoutMilliseconds`（现有默认 5000 ms）。这是本方同步等待的上限，不宣称能强行中断 Windows/Qt/驱动内部的同步调用。
- GUI 控制器进入停止或销毁时，先使 Qt 创建/提交等待可取消，再 join worker，最后在 GUI 线程完成窗口释放。不能依赖 join 期间继续处理 GUI 消息才能退出。
- 停止请求幂等；丢弃待显示帧，禁止新提交。GUI 阶段清理完成后才向用户发布最终可再次启动/关闭状态。
- 关闭窗口、Esc、超时、准备期间停止、初始化失败和应用退出均走同一取消/清理合同。

### 帧背压

- 产品持有的 canonical 帧最多包含一个当前帧和一个待绘制帧；绘制所需物理输出缓冲单独按配置尺寸定额分配，不随帧数增长。
- 当前帧在被 paintEvent 使用时不可变。输入 view 在 SubmitFrame 返回后可失效，因此不得借用调用方内存跨帧保存。
- 待帧槽位与既有 Runtime 背压接线；不建立逐帧 Qt signal/事件队列。
- 唤醒消息只表示“有状态变化需要处理”，同一运行最多一个待处理绘制唤醒；不在事件对象中无限排队图片。
- GUI 忙时保留有界状态，不通过忙循环或递归 repaint 追赶历史帧。
- Qt 随新帧及系统重绘请求执行绘制，不模拟 DXGI 每次刷新上的重复 Present。重复绘制同一当前帧不递增逻辑 FrameSequence，也不生成新方程；该后端差异明确记录为 Qt 呈现合同。
- 保持现有单向 FrameSequence/Carousel 提交语义，不引入接收 ACK，也不以 paint 次数创建新的 FEC 方程。

## 7. 像素与窗口几何

- 保持 Per-Monitor DPI Aware V2，以 Win32 物理 client/monitor 几何作为最终校验面。
- 区分 Qt device-independent geometry 与实际 backing-store 像素；不能把 QScreen 的逻辑尺寸直接当作编码宽高。
- 不使用 QLabel 的自动缩放，也不启用 `SmoothPixmapTransform` 或对数据图案进行抗锯齿。
- 原输入 RGB 值、每个 Profile 的 point 映射、居中静态边缘与 matte 行为保持一致；不额外做 gamma、色彩空间转换或透明混合。
- 显示时保持不透明，alpha 处理与原路径的 alpha-ignored 可见结果一致；不允许透明窗口将桌面内容混入码面。
- PAM4/Wide 复用 Runtime 已构成的码面与静态边缘，不重新定义其缩放规则。
- 对窗口模式使用既有 viewport/point-sampling 规则；用独立像素 oracle 验证 Qt 路径，不用 Qt 输出本身生成期望值。
- DPR、client 尺寸、monitor、模式或窗口可见/关闭状态变化时，失效旧 epoch/旧帧。无法满足当前配置几何时暂停或明确失败，不静默拉伸或裁掉码面。
- 创建/分配前检查尺寸、row pitch、乘加溢出与 `maximumFrameBytes`；有界物理输出缓冲也单独检查。
- 不嵌入 OpenGL/RHI 子控件。若实际绘制引擎不符合所声明 raster 路径，明确失败，不修改全局环境变量掩盖不匹配。

## 8. 统计与错误语义

- 新增实际 Qt 绘制/接纳/丢弃等必要计数；明确区分提交到应用、执行 paintEvent、远端 Decoder 接收。
- Qt 不拥有 DXGI Present 统计：相关 rate/latency/Present ID 在报告中为不适用/null，并提供明确原因，不能把 paint 次数填入 DXGI 成功计数。
- 软件路径不伪装硬件 adapter，也不复用只代表 WARP 测试的字段去证明真实软件后端已受支持。
- 计数有界/饱和，日志沿用低频与体积上限。日志不记录像素或文件内容。
- 任何 Qt 回调内的异常都转换为可观察的失败状态，不能逃逸 Qt event loop。
- 已选择 Qt 时失败直接报告；不会退回 native 后端。默认 Native 失败也不会自行转 Qt。

## 9. 回归与验收矩阵

### 纯配置和 GUI

- 每个新主窗口和 Shell 窗口默认 D3D11，切换不写入持久默认。
- active/Stopping/close-pending 禁用按钮，直接调用处理函数也不能热切换。
- 四个 GUI 传输模式保持原参数；非法显示方式、固定诊断入口冲突、正式测量+Qt 组合明确拒绝。
- 旧 flip/blt 诊断启动参数、独立偏好和 Shell 注册边界不变。

### 软件像素与 DPI

- BGRA 格式、padded row pitch、输入所有权、alpha/不透明、整幅输出像素验证。
- 合成 DPR 1.0、1.25、1.5、2.0，负 monitor origin、窗口边界、有效/过小 client 和几何变更的 oracle 验证。
- 四种 GUI Profile 的代表性码面与现有 reference/Golden 字节比较，Golden 不重写。
- Qt raster 绘制与 native/WARP reference 的可比输出验证；离屏结果不冒充实屏 DPI 或 Citrix 验证。

### 生命周期

- 创建成功/失败、创建尚未处理即停止、准备期间取消、GUI 繁忙/无新绘制时的有界等待。
- 当前帧绘制中提交新帧、待帧上限、旧 epoch/generation 消息拒绝。
- 停止、重复停止、反复启动、窗口关闭、控制器析构及延迟事件不发生 UAF/死锁。
- 默认 D3D11 合同与已有 ComposedBlt 定向测试继续通过，不削弱失败测试。

### 交付验证

- 构建受影响的 Encoder、应用与呈现测试目标，执行定向回归和现有相关 Golden 检查。
- 先使用离屏/隐藏窗口测试；不主动运行会改变桌面模式或注入输入的 Gate。
- 新的独立包使用精确源码指纹与文件摘要；从新解压目录验证依赖和严格参数入口。新隔离启动入口不改正常偏好或 Shell 注册。
- 原包、原候选和受保护报告保持不变；无自动 Git 提交。
- 现场先可进行正常态 Qt 显示、启停和小文件完整性检查。只有实际远控证据支持后，才评价间歇性冻结的改善；最终仍要求原链路整文件摘要、安全发布、最终重开与独立长度/SHA-256。

## 10. 参考资料

- [Qt：Painting in Threads](https://doc.qt.io/qt-6/threads-modules.html)：QPainter 可用于线程中的 QImage，但 QWidget 绘制受 GUI 线程约束。
- [Qt：High DPI](https://doc.qt.io/qt-6/highdpi.html)：Qt 控件的逻辑坐标与 QImage 原始像素不同，必须显式处理 DPR。
- [Qt：QWidget paintEvent/update](https://doc.qt.io/qt-6/qwidget.html)：update 可合并绘制请求，paintEvent 不应递归调用 update/repaint。

相关仓库合同：`AGENTS.md`、`docs/PixelBridge_最终技术路线与总体设计.md`、`docs/PRESENTATION.md`、`docs/EXPERIMENTAL_PAM4_PROFILE.md`、`docs/EXPERIMENTAL_PAM4_WIDE_PROFILE.md`。本设计不改变协议或 Profile 规范。
