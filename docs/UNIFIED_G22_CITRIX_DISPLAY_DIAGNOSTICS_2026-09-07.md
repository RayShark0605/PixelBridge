# G22 Citrix 启动故障：只读诊断交接

## 1. 当前事实与边界

2026-09-07 用户报告：另一台电脑中，通过 Citrix Receiver 打开的远程桌面内运行 Encoder，
点击“开始传输”出现“无法确认当前屏幕；请将 Encoder 窗口移到目标屏幕后重试”。

**状态：现场故障已报告，具体首个失败 API 尚未确认；诊断工具已准备。不是已修复。**
本机普通双屏正常枚举不反证远端故障；G21 Windows 远程桌面证据和 G22 本地候选证据不等于 Citrix 兼容性认证。
现有 `3a840a2` 交付 Encoder/Decoder、封印 ZIP、Unified build 目录均未替换或重构建。

## 2. 已证明的代码路径

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

## 5. 下一步决策

等待用户返回远端 JSON；先定位 catalog 是 DPI、模式、默认刷新率还是 DXGI 对应失败。
如果需要软件渲染/新后端或改变显示元数据准入，应先向用户说明影响并确认，不把安全检查整体移除，
不伪造 rotation/LUID/刷新率，不修改用户 Citrix 驱动/策略来绕开尚未定位的问题。
修复后必须在该 Citrix 会话复现“开始传输→可见数据帧”；全文件完成仍以本机实际捕获像素恢复、摘要、发布和重开为准。
