# PixelBridge 显示环境只读诊断

用于定位 Encoder 的“无法确认当前屏幕”错误，特别是 Citrix/虚拟/远程显示环境。
它是独立诊断工具，不是新版 Encoder，也不改变产品对显示器或渲染设备的准入规则。

## 给现场用户

1. 把 `PBDisplayDiagnostics.exe` 复制到**发生报错的同一个远程桌面会话**中的可写目录。
2. 双击一次；不弹窗口，不切换焦点。
3. 同目录出现新的 `display-diagnostics-*.json`，把它发回即可。

不需要 Decoder、Qt、管理员权限或额外 DLL；不要只在 Citrix Receiver 所在的本机运行。
工具不截图、不读取待传文件、不创建图形设备/数据窗口、不联网、不操作鼠标键盘、不修改驱动、注册表或显示设置。
唯一写入是显式指定或 EXE 同目录下的新增 JSON 文件，已有文件永不覆盖。
报告含显示器名称/矩形/DPI/模式、显示设备描述、DXGI adapter/output 与进程会话数字；不收集用户名、主机名、IP 或设备注册表路径。

无参数失败时可能没有报告（例如目录不可写）；此时在 PowerShell 指定 EXE 完整路径运行：

```powershell
$diagnosticExe = Read-Host '输入 PBDisplayDiagnostics.exe 的完整路径'
& $diagnosticExe --stdout 2>&1 | Out-String
"ExitCode=$LASTEXITCODE"
```

管道用于等待 GUI 子系统程序退出并接收输出；程序不会分配新控制台。
也支持 `--output NEW_JSON_PATH`，路径必须不存在且父目录已经存在。
退出码 `0` 只表示报告成功产生，**不表示 Encoder/渲染/捕获正常**。
退出码 `2` 为参数或报告输出失败；写入失败可能留下不完整文件，不应视为成功报告。

## 证据解释

- `productionMonitorCatalog` 直接调用未修改的 `apps/common/monitor_catalog.cpp`。
- `win32` 是独立、继续收集的元数据枚举：即使一个屏幕缺 DPI/模式/DXGI 映射，也保留其他信息。
- `defaultRefreshMarker=true` 表示成功返回的刷新率原值为 `0/1`；这不是实际的 0 Hz/1 Hz，不伪造 60 Hz。
- `dxgi` 保留 factory/adapter/output 各阶段 HRESULT；正常枚举结束的 `DXGI_ERROR_NOT_FOUND` 不是单独的故障证明。
- 只有成功的 output 描述才可用于对应 Win32 的 `deviceName`/`monitorToken`；枚举不完整时不能声称输出不存在。
- `remoteSessionHint=false` 不排除 Citrix；不能按 provider 名称选择修复分支。
- `probePerMonitorV2` 只证明诊断进程本身；本工具不查询 Encoder HWND 或证明其 Qt 线程 DPI 状态。
- 各接口顺序采样，不是原子拓扑快照；请在远程连接稳定、不调整屏幕布局时运行。
- 新增的独立元数据枚举最多 64 项（每 adapter 最多 64 output），达到上限显式标注；生产 catalog 保持原实现，其 DXGI 循环依原生枚举结束。
- 无 D3D11 device/swap chain/Present，因此不能证明 WARP、硬件渲染、Citrix 可见像素或文件恢复可用。

## 构建与测试

MSVC x64 / C++20，系统 Win32/DXGI/SHCore API，静态 MSVC runtime。
可独立配置，不触碰现有 Unified 发布构建目录：

```powershell
$repository = '<repo>'
$build = Join-Path $repository 'artifacts\g22-citrix-diagnostics-20260907\build'
cmake -S (Join-Path $repository 'tools\PBDisplayDiagnostics') -B $build -G 'Visual Studio 17 2022' -A x64
cmake --build $build --config Release --target PBDisplayDiagnostics --parallel 2
```

源文件/生产 catalog SHA-256 和配置时的 Git base commit 写入报告；有未提交改动时，base commit 不是完整二进制身份。
`test_display_diagnostics.py --executable <完整EXE路径> --evidence <新目录> [--decoder <已交付Decoder完整路径>]`
执行有界、无窗口的 CLI/JSON/Unicode/拒绝覆盖/单文件便携检查；`--self-test` 仅检查序列化，不调用显示 API。
不运行全套 CTest、GUI 交互、显示 Gate 或 payload 测试。
