# D3D11 Data Window / PBPresentTiming

## 范围与契约

对应总体设计 §16.3、§21、§37 的呈现部分。本轮交付独立原生数据窗口、
有界计时状态机和 Encoder 呈现验证入口。**Certified candidate 不等于完整
LocalDesktop 物理链路认证。** 没有增加 Capture 后端、GPU 调制或 texture 输入接口；
不改 Bootstrap、Visual Profile、Transport/FEC、Golden pins 或 Decoder 文件恢复语义。

| 项目 | 默认值 / 检查 |
| --- | --- |
| 窗口 | 无 Qt 合成的 borderless top-level HWND；1920×1080 physical client |
| DPI | Encoder 和真实显示 Gate 的 manifest 声明 PMv2；库验证，不修改进程 awareness |
| Device | 目标 monitor 所在硬件 adapter；D3D11 feature level 11.0 或以上；不自动 WARP fallback |
| Swap effect | `DXGI_SWAP_EFFECT_FLIP_DISCARD` |
| Buffers | 2；公共配置允许显式选择 2..16 |
| 帧延迟 | waitable object；`SetMaximumFrameLatency(1)`，回读验证 |
| Present | 每次都是 `Present(1, 0)`；tearing 创建和 Present flags 均关闭 |
| 格式 | `B8G8R8A8_UNORM`，非 SRGB；sample count 1、quality 0；alpha ignore |
| 像素 | 完整 `UpdateSubresource` 上传；没有 sampler、blending、MSAA、缩放 shader；`DXGI_SCALING_NONE` |
| 对照项 | `FLIP_SEQUENTIAL`、MaximumFrameLatency 2 只作为显式配置，不自动退回 |

创建与重建后检查实际 descriptor、latency、physical client 和 DPI awareness。
不满足契约就报错或暂停，不通过 blt、非 waitable swap chain、拉伸/裁剪或 tearing 掩盖问题。
首次上传/Present 也必须获得 frame-latency permit。
[waitable-object 契约](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-getframelatencywaitableobject)

## 模块与调用方责任

- `PB::PBPresentTiming`：无 Qt/Windows 类型的 normalized event 状态机。单 owner；
  `BeginEpoch`、`RecordPresent`、`ObserveStatistics`、`BreakCadence`、`GetSnapshot`。
- `PB::PBRenderD3D`：`DataWindow::Create`、`SubmitFrame`、`GetSnapshot`、`RequestStop`、`Stop`。
  公共头不暴露 HWND、COM 或 DXGI 对象。只有测试私有 seam 能获得 native window token。
- `CanonicalBgraFrameView`：尺寸、row pitch、像素 span、`FrameSequence`、预期
  `PresentationEpoch`。`SubmitFrame` 返回前逐行复制有效像素，不保留调用者指针。
  调用方须在这次调用期间保持输入 span 稳定，返回后可立即复用。
- geometry 在实例生命周期内固定。改变 canonical geometry 必须由上层重新建立
  窗口及相应 Session/profile，不把普通 resize 解释为协议配置变更。
- 调用方必须保证 `FrameSequence` 与 raster 身份一致。相同序号表示同一视觉帧；
  renderer 不解析像素中的 Bootstrap，也不通过计算 hash 来认证这个生产端声明。
  序号回退会失效精确 observation coverage；相同序号不会重复增加 unique visual 计数。
- `PresentTiming` 本身不提供并发访问；`DataWindow` 同步 mailbox 与快照。
  提交、读取、多个 `Stop` 可并发；**对象析构不得与任何成员调用并发**。

资源上界：两个预分配 CPU 帧槽（active + latest-pending）、256 条固定 Present
关联记录。新 pending 替换旧 pending 时累计计数；没有数据时不重复 Present 来制造吞吐。
单帧默认预算 64 MiB，另受 D3D11 尺寸上限限制；帧尺寸、stride、最小 span 长度和
双槽总量先验证再分配。正常路径没有 GPU readback，也没有 GPU→CPU→GPU 回读中转。

HWND、immediate context、Present、resize/rebuild 和销毁属于同一专用线程。
WndProc 只登记状态，GPU 工作在消息分派外执行。停止事件可打断库自己的等待；
owner 退出等待后才关闭 frame-latency handle，wake handle 保留到线程 join 之后。
GPU drain 使用 event query、可取消的短等待和配置超时，不无限重试。
已取得但尚未用于 Present 的 permit 只可在同一 swap chain 内保留，不能跨 device/swap-chain 重建。

## 状态和 PresentationEpoch

`Running` 只表示当前物理几何/呈现契约有效；不隐含 statistics 可用。
每个环境变化 epoch 清除旧关联、pending 数据与测量基线。

| 事件 | 行为 |
| --- | --- |
| 非 canonical resize | 暂停数据 Present；在预算内重建 back buffer，不拉伸为有效数据 |
| minimize / zero client | 暂停；不对零尺寸调用 `ResizeBuffers` |
| 精确尺寸恢复 | 重验契约；等待新的完整 raster；重新暖机 |
| 跨屏过渡 | `singleMonitor=false` 时暂停 admission，不抢先做跨 adapter 分配 |
| 完全进入新 monitor | 查询 monitor、adapter、DPI、client；新 epoch；不同 adapter 时 drain、释放并重建 |
| `WM_DPICHANGED` | 记录事件，owner 使用建议位置并保持 canonical physical size，重新验证 |
| `WM_DISPLAYCHANGE` / 实际模式变化 | 记录 serial/实际模式，清掉旧基线；即使通知前后模式恰好相同也有 epoch |
| statistics disjoint | 只换 timing epoch，**不重建 swap chain** |
| 不支持 / 查询失败 / 不推进的 refresh clock | 显示继续，相关 timing 不可用；计时恢复后重新建立 epoch/基线 |
| occluded | 与失败/成功 Present 分开计数；恢复探测使用 `DXGI_PRESENT_TEST`，不计入数据调用数 |
| device loss / wait / rebuild 失败 | 终止性 Failed，保留阶段、native status、可获得的 device removed reason；不无限重试 |

`ResizeBuffers` 前先 drain、`ClearState`、释放 staging/back-buffer 直接和间接引用并 Flush。
重建完成才重新发布有效契约。
[ResizeBuffers 引用释放要求](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-resizebuffers)

真正替换 flip swap chain 时，在释放旧 chain/back-buffer 引用之后、创建新 chain
之前 Flush immediate context，处理 D3D11 deferred destruction，避免同一个 HWND
仍被旧 flip chain 占用。原生 Gate 在同一真实 adapter 上强制这条重建路径并重复字节验证；
这不代替不同物理 adapter 间的实测迁移。
[Flip chain deferred destruction](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-flush)

S_OK 不保证 refresh counter 有效。真实窗口路径可能出现 PresentCount 递增、
PresentRefreshCount 和 SyncRefreshCount 不推进（包括持续为零）。此时不能每帧
重置 epoch，否则会不断丢弃 pending raster；实现保留显示，报告 `refresh-clock-unavailable`。
32-bit 正常回绕仍按模差分识别，不把回绕经过零误判为不支持。

## 指标口径

所有指标均是**生产端 DXGI observation**，不是接收端 `UniqueVisualFPS`，更不是光学测量。
不得声称 `PresentCallFPS == PresentedVisualFPS`。

| 指标 | 计算与有效性 |
| --- | --- |
| `PresentCallFPS` | epoch 内实际数据 Present 尝试次数 / `(sampleQpc - epochStartQpc) / frequency`；成功、失败、occluded 分列；不含 Present TEST |
| `PresentedVisualFPS` | 仅对当前 epoch 的成功 Present ID 与 FrameSequence 关联；至少两个连续有效时钟样本、完整 ID coverage；`(uniqueObserved - 1) / elapsedDisplayedSeconds` |
| `ObservedVisualFPSLowerBound` | 已确认序号集合的保守下界率；有关联缺口时不补算丢失 ID 对应的视觉帧；时钟无效/陈旧时同样不可用 |
| `PresentRefreshCountExtended` | epoch 内 32-bit refresh counter 的 checked 延展；正常 wrap 可接受；半区间歧义、倒退、加法溢出重新建立基线 |
| `PresentQueueLatency` | 有效连续时基及关联上的估计：`SyncQPC - PresentBeginQPC + signedModulo(PresentRefreshCount - SyncRefreshCount) * period`；单位 ms；负值/无时基/缺口/陈旧为 null |
| `PresentGlitchCount` | 在连续 producer cadence 内，实际 VBlank 晚于相对预期 VBlank 的**已观测 ID 数**；重复 statistics 不重复计数；不推断未观测 glitch 总数 |

`period` 来自连续 `SyncQPCTime` / `SyncRefreshCount` 差值。先做整数相对时间差再
转浮点，避免大绝对 QPC 值导致短间隔精度消失；预计显示时刻在 epoch 外或未来时不可用。
Glitch 比较相邻已观测记录的 refresh delta 与成功提交 ordinal delta，迟到后重新锚定，
不会把一次迟到永久累计成后续每帧都迟到。producer 进入无 pending 的 idle、失败或
occluded 时中断 cadence；不通过 Present(0)、tearing 或强行丢未确认队列“修正”计数。

重复 statistics 不续 freshness；超过 1 秒没有新确认观察就标记 stale。
关联 ring 有界，溢出、跳过 ID、未匹配 ID、序号回退会使精确 coverage 失效。
第一次确认前的数据不属于 measured interval；之后的缺口保持到下个 epoch。
64-bit 诊断计数饱和时显式标记 `counterSaturated` 并关闭 rate/latency，不给出溢出后的伪精确值。

快照/JSON 包含 epoch、原因、physical size、monitor/adapter、模式、native status、
原始 statistics、最近一次 Present 的 begin/end QPC、ID、FrameSequence、各类计数和可用性。
不存在的指标输出 `null`，不是 0。JSON 只含诊断元数据，不输出 payload；文件生命周期由调用方管理。
快照不是无损事件流：低频轮询无法恢复没有采样到的每条事件。

DXGI statistics 本身有多显示器等可靠性限制；`valid-dxgi-observation` 只是本模块的
关联/算术有效，不是显示器真实输出认证。没有在热路径悄悄添加 DwmFlush 来制造另一套性能语义。
[Microsoft statistics 限制](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-getframestatistics)

## Encoder 诊断入口

```powershell
D:\MyProjects\PixelBridge\build-presentation-release\apps\PixelBridgeEncoder\Release\PixelBridgeEncoder.exe --data-window --frames 120 --telemetry D:\MyProjects\PixelBridge\build-presentation-release\encoder-NEW.jsonl
```

复用 `GenerateRandomSessionId`、现有 reference profile binding、`SerializeBootstrapRecord`
和 `EncodeReferenceFrame`；逐帧递增 Bootstrap FrameSequence。Data/Control 为诊断内容，
不是完整 Session 控制平面或可恢复文件发送流程。无参数的原精确横幅不变；`--help` 给用法。

`--frames N` 是成功 **CPU SubmitFrame** 上限，范围 1..1,000,000，不等于 Present 成功数，
更不等于视觉显示帧数。epoch 变化或 latest-pending 替换可能使计数不同。
省略 `--frames` 时用窗口中的 Escape 结束；有限运行 10 秒无进展则报错。
遥测使用 `CREATE_NEW`，拒绝覆盖；最多 16 MiB，达到上限明确终止，不无限写日志。

## 构建和验证

普通默认 CTest 运行 PBPresentTiming、共用 owner loop 的 fake-backend 生命周期测试、
没有 PMv2 manifest 的负向 host probe，以及原有回归/Golden；不会弹窗或切模式。

```powershell
cmake -S D:\MyProjects\PixelBridge -B D:\MyProjects\PixelBridge\build-presentation-release -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake -DPB_BUILD_APPS=ON -DPB_BUILD_TOOLS=ON -DPB_BUILD_PHASE0_GATE=ON -DPB_BUILD_PRESENTATION_GATE=ON
cmake --build D:\MyProjects\PixelBridge\build-presentation-release --config Release --parallel
ctest --test-dir D:\MyProjects\PixelBridge\build-presentation-release --build-config Release --output-on-failure

cmake -S D:\MyProjects\PixelBridge -B D:\MyProjects\PixelBridge\build-presentation-asan -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake -DPB_BUILD_APPS=ON -DPB_BUILD_TOOLS=ON -DPB_BUILD_FUZZERS=ON -DPB_BUILD_PHASE0_GATE=ON -DPB_BUILD_PRESENTATION_GATE=ON
cmake --build D:\MyProjects\PixelBridge\build-presentation-asan --config RelWithDebInfo --parallel
ctest --test-dir D:\MyProjects\PixelBridge\build-presentation-asan --build-config RelWithDebInfo --output-on-failure
```

ASan 沿用现有 fuzz 图的 `/Zi /fsanitize=address` 和 runtime-copy；新库本体、Encoder
和新测试均插桩。与固定 Wirehair 图保持一致，MSVC STL container annotations 关闭，
地址插桩仍开启。MSVC ASan 不等于 UBSan、TSan 或 coverage-guided libFuzzer。
性能/时序观察只引用没有 ASan 和没有测试 readback 的 Release 运行。
WARP 只通过未安装的 test seam 显式选择，快照以 `softwareRasterizer=true` 标记；
WARP 的 descriptor/字节正确性通过不能冒充硬件认证。生产路径拒绝软件 adapter，不做隐式降级。

真实 Gate 串行并使用 `PixelBridgeDesktop` resource lock：

| CTest | 内容 |
| --- | --- |
| `PBPresentationGate.gpu-warp` | 显式测试 WARP；8 个 native 创建阶段失败清理；两种 flip effect × latency 1/2；独立彩色棋盘格/padded row oracle、完整连续覆盖、resize 恢复、canonical BGRA；D3D debug layer |
| `PBPresentationGate.gpu-hardware` | 相同字节 oracle、配置矩阵和 debug layer，真实硬件 adapter；没有 WARP fallback |
| `PBPresentationGate.live` | 无 readback/debug instrumentation 的真实 Present；PMv2/descriptor/client 回读；minimize、非 canonical resize、真实跨屏过渡暂停及双向迁移 |
| `PBPresentationGate.mode-supervisor` | 保存全部活动输出；CDS_TEST；2560×1440@180 → 120 → 原模式；子进程 epoch/旧样本失效/重新暖机；正常、失败、崩溃、超时四条恢复路径 |
| `PBPresentationGate.encoder` | apps 开启时验证 Encoder 有限提交、实际 Present、默认契约、Unicode 遥测路径、拒绝覆盖且已有日志不变；不比较两种 FPS 是否相等 |

Evidence 每次创建独立目录，位于 `build-presentation-*/tests/PresentationGate/<Config>/evidence/`；
`gate.txt` 记录断言步骤/保存和回读的模式，`snapshots.jsonl` 保存原始 observation。
模式监督进程不渲染或传递 payload；子进程自行生成 raster，命名 event 仅用于测试控制同步，
从未连接 Decoder 或形成第二条文件数据通道。

模式切换只用 flags=0 的动态更改，不写注册表、不变主屏、不启用 unsafe modes。
父进程在子进程非零退出、强制终止或超时后均按保存的当前 DEVMODE 恢复，随后验证全部输出。
预检不满足时以 BLOCKED/非零退出报告，**不 skip 或把缺少实测视为 PASS**。
[ChangeDisplaySettingsEx 语义](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-changedisplaysettingsexw)

## 明确限制

本轮实际命令、最终 Gate 结果、显示模式恢复和原始遥测位置见
[PRESENTATION_VALIDATION.md](PRESENTATION_VALIDATION.md)。

- GPU byte readback 证明上传字节与独立/现有 CPU raster 一致，不证明屏幕捕获、DWM
  色彩变换、HDR、视频编码或光学链路正确。没有 Capture round-trip。
- 双输出真实迁移与 180/120Hz 模式 Gate 不代替真实 DPI 比例矩阵、不同 adapter 的实际迁移、
  HDR/VRR、显示方向变更、热拔插矩阵或设备 TDR 的实机认证。对应状态机分支有 fault/synthetic 测试，不能冒充这些实测。
- 单监视器窗口契约并不证明无遮挡、无光标覆盖、无显示器缩放/色彩处理；最终接收端必须从实际捕获像素验收。
- 库内 wait/drain 可取消且有超时，但操作系统/显示驱动内部阻塞的创建、Present 或销毁调用
  无法由 C++ 超时强行抢占。实际 driver hang 的容错需要上层进程监督。
- 模式恢复保证覆盖监督子进程的失败、崩溃和超时，不声称能在整机断电或监督进程本身被强杀后执行恢复代码。
- 现阶段 Encoder 是诊断入口，不是 Qt 控制 UI、文件发送器或通过接收端验证的 Certified profile。
