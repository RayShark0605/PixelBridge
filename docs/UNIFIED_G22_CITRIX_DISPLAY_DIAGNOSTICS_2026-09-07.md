# G22 Citrix 启动故障：只读诊断交接

## 1. 当前事实与边界

2026-09-07 用户报告：另一台电脑中，通过 Citrix Receiver 打开的远程桌面内运行 Encoder，
点击“开始传输”出现“无法确认当前屏幕；请将 Encoder 窗口移到目标屏幕后重试”。

**当前状态：截图已确认 DXGI output 对应缺失；用户已同意 Encoder 专用硬件解耦修复，实施与候选见[后续记录](UNIFIED_G22_ENCODER_DISPLAY_COMPAT_2026-09-07.md)。Citrix 新版实测仍待返回。**
截图及后续最小修复建议见第6节；第2..5节保留发出诊断工具时的分析与交接背景。
本机普通双屏正常枚举不反证远端故障；G21 Windows 远程桌面证据和 G22 本地候选证据不等于 Citrix 兼容性认证。
现有 `3a840a2` 交付 Encoder/Decoder、封印 ZIP、Unified build 目录均未替换或重构建。

## 2. 原交付版本已证明的代码路径（修复前）

1. `apps/PixelBridgeEncoder/encoder_gui.cpp` 的 `ConfigureCurrentMonitor` 将 `MonitorFromWindow` 返回空
   与 `EnumerateMonitors` 失败合并为同一句提示，因此“移动窗口”不是可靠的通用补救。
2. `apps/common/monitor_catalog.cpp` 要求**所有**枚举的显示器都能取得 DPI、当前模式、非 `0/1` 刷新率、
   DXGI output 对应和有效 rotation；任意一项失败即终止整个 catalog。
3. Windows `DEVMODE.dmDisplayFrequency` 的 `0/1` 可以表示默认刷新率，不代表坏显示器或实际 0/1 Hz。
   当前的无条件拒绝存在兼容性缺口；不能将未知刷新率随便改写成 60 Hz。
   依据：[Microsoft DEVMODEA](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-devmodea)。
4. `libs/PBRenderD3D/src/native_backend.cpp` 的 `FindAdapter` / `CreateGraphics` 仍要求目标 HMONITOR
   有对应 DXGI output，正常路径拒绝 software adapter，且没有自动 WARP fallback。
   因此即使放开 catalog，也不能据此声称软件/虚拟显示端可以成功 Present。
5. Citrix 会话可使用 DOD、IDD 或 GPU vendor adapter；仅知道客户端叫 Citrix Receiver，不能确定服务器侧
   具体显示适配器能力。依据：[Citrix CTX237608](https://support.citrix.com/s/article/CTX237608-display-adapter-priority-and-monitor-creation-in-citrix-sessions)。

以上是实现/接口证据，不是用户那台电脑的运行时首因证明。没有按 Citrix 品牌改协议、阈值或 Decoder admission。

## 3. 最小诊断交付

- 源码：`tools/PBDisplayDiagnostics/`；独立 CMake 项目，也注册在 Windows/MSVC 的可选 tools 树中。
- 交付目录：`<repo>\artifacts\g22-citrix-diagnostics-20260907\release`。
- 工具：`PBDisplayDiagnostics.exe`；复制到报错的**同一个 Citrix 会话**的可写目录并双击。
- 输出：EXE 同目录新增 `display-diagnostics-*.json`；不弹窗口，不覆盖已有文件。
- CLI：`--stdout`、`--output NEW_JSON_PATH`、`--help`；精确用法见工具 README。
- 权威诊断工具哈希、配置 Git base、源文件哈希见同目录 `diagnostic-manifest.json`；此身份不是 G22 产品二进制身份。

工具重用未修改的生产 catalog，另外独立记录 Win32/GDI 与 DXGI 元数据、错误码和原值。
它只读显示信息，不截图、不开数据窗口、不创建 D3D11 设备、不联网、不读 payload 或凭据、不改显示配置、
不操作鼠标键盘；唯一写入是新增诊断 JSON。新增独立枚举有 64 项上界，生产 catalog 保持原实现。
PMv2/会话信息只属于诊断进程，不能代替 Encoder HWND/Qt 线程诊断；各接口顺序采样，不宣称原子拓扑快照。

## 4. 本机验证

证据根：`<repo>\artifacts\g22-citrix-diagnostics-20260907`。

- 单独构建 MSVC x64 / C++20 / `/W4 /WX` / 静态 CRT；未拉入 Qt 或 core 实现库。
- 首次独立构建暴露 application model 的传递头依赖缺失，补齐准确 include 集合后通过；保留初始失败日志。
- `local-check-01/summary.json`：10项通过，包括6种序列化边界、合法 JSON/PMv2、编译源 hash、
  中文空格路径、拒绝覆盖、缺父目录/非法参数、帮助、无参数单EXE便携入口和与交付 Decoder 的同机 catalog 对照。
- 本机实际 catalog：`DISPLAY1 [0,0,2560,1440]`、`DISPLAY2 [2560,0,5120,1440]`，均180 Hz，catalog code/native为0。
- `pe-inspection.txt`：x64 / GUI subsystem，仅 USER32、SHCore API-set、DXGI、KERNEL32 系统导入。
- 原交付 Encoder SHA-256仍为 `2c8f797df8397016a733124e642eb4bc72a28430110908f14546f50cde5c6e34`。
- `docs/PHASE1_GATE_REPORT.md`未修改/暂存，原 hash保持不变。

**未做**：Citrix现场、真实双击鼠标操作、D3D11/WARP设备/Present、像素收发、大文件、全量CTest或ASan。
无参数子进程入口和PE检查不冒充人工双击；所有本机子进程使用无控制台方式，不激活窗口。

## 5. 发出诊断工具时的下一步决策（历史）

等待用户返回远端 JSON；先定位 catalog 是 DPI、模式、默认刷新率还是 DXGI 对应失败。
如果需要软件渲染/新后端或改变显示元数据准入，应先向用户说明影响并确认，不把安全检查整体移除，
不伪造 rotation/LUID/刷新率，不修改用户 Citrix 驱动/策略来绕开尚未定位的问题。
修复后必须在该 Citrix 会话复现“开始传输→可见数据帧”；全文件完成仍以本机实际捕获像素恢复、摘要、发布和重开为准。

## 6. 用户返回截图：已确认 DXGI output 对应缺失

用户提供的是可读的 JSON **截图**，不是原始 JSON 文件。已将原始 PNG 字节复制到
`<repo>\artifacts\g22-citrix-field-report-20260907\user-report-screenshot.png`，
并将关键可见字段人工转录到 `selected-fields-transcribed.json`，明确标注 derived/非原始JSON；`SHA256SUMS.txt`保存两者hash。
诊断 base commit为`ddb2fa2fc366bad6ca3165cf4f28d869b50f2201`，对应本次工具。

| 截图现场字段 | 结论 |
| --- | --- |
| `productionMonitorCatalog.succeeded=false, code=2, nativeError=1168, monitorCount=0` | 生产 catalog 为`MetadataUnavailable/ERROR_NOT_FOUND`；count0是拒绝发布catalog，不是Windows没有屏幕 |
| `win32.enumerationSucceeded=true`，DISPLAY2/3均`getMonitorInfoSucceeded=true` | Win32实际识别到两块屏幕 |
| DISPLAY2=`[1920,-1,3840,1079]`；DISPLAY3=`[0,0,1920,1080]`且primary | 两块均1920×1080；负的top坐标不是本次catalog失败原因 |
| 两块`dpiHresult=0, dpiX=dpiY=96, currentModeSucceeded=true` | 诊断进程中的DPI和模式读取成功 |
| 两块`refreshRateRaw=64, defaultRefreshMarker=false, modeOrientationRaw=0` | 本次不是默认刷新率0/1拒绝问题；64只是驱动报告值，不是UniqueVisualFPS证明 |
| DISPLAY2/3的description=`Citrix Display Only Adapter` | 已有Citrix显示端现场证据，不再只是品牌推断 |
| DXGI factory/adapter description均成功；`NVIDIA GRID K220Q, software=false` | 硬件适配器可以枚举；不能说没有GPU，也不能推断设备创建一定成功 |
| NVIDIA与Microsoft Basic Render Driver的`outputs=[]`，结束值`-2005270526`，limit=false | `0x887A0002 / DXGI_ERROR_NOT_FOUND`为正常枚举结束，未发现任何output；不是收集上限截断或factory创建失败 |
| `remoteSessionHint=false` | 该系统指标不能作为否认Citrix/虚拟屏的条件 |

生产 `GetDxgiOutputIdentity` 要求每个Win32显示器按`DeviceName`匹配DXGI output；现场所有output列表为空，
所以该函数返回false，`EnumerateMonitor`返回`MetadataUnavailable/1168`，GUI将catalog失败显示为“移动窗口”。
**这个catalog阻塞已定位；不是Windows不能枚举屏幕，也不是缺少显卡的证据。**
截图不包含Encoder HWND检测、D3D11设备创建、swap chain或Present，因此不能宣称修复后的像素呈现已获证明。

### 最小修复建议（用户随后已同意，实施见后续记录）

1. 仅对Encoder呈现解耦“Win32窗口/屏幕身份”和“实际渲染设备身份”；屏幕定位、物理矩形、DPI和拓扑重验仍严格执行，
   未提供的DXGI output/LUID不能伪造；不全局放宽Decoder捕获所需的元数据合同。
2. 普通有DXGI映射的显示器保留原路径；无output映射但存在硬件adapter时，建立显式可诊断的D3D11硬件候选路径，
   不按Citrix/NVIDIA品牌分支，不预设其能成功渲染。
3. 使用现有窗口化swap chain/无边框全屏与相同canonical raster；保留bounded queue、device loss、epoch和窗口安全处理。
   Microsoft接口分别接收adapter与HWND，`pFullscreenDesc=nullptr`创建窗口化swap chain，
   `pRestrictToOutput`可为空；这支持该解耦设计，但不是Citrix现场成功保证。
   参考：[D3D11CreateDevice](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-d3d11createdevice)、
   [CreateSwapChainForHwnd](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-createswapchainforhwnd)。
4. 本次先不新增自动WARP/Qt/GDI回退，不改profile、wire、FEC或Decoder；若硬件设备/窗口呈现仍被会话限制，
   记录具体失败阶段，再就软件路径及性能影响向用户确认。
5. 改进错误提示，区分窗口所属屏幕、catalog、渲染设备和swap chain/Present失败；不得继续用“移动窗口”概括所有情况。

`75eccbc` 诊断记录提交仅保存现场证据和更新文档，没有改动/重构建产品或运行新的显示/输入自动化。
其后用户同意上述范围，Encoder 修复、测试及候选身份单独记录，不回填为原交付版本已通过。
