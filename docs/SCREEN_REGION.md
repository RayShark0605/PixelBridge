# PBScreenRegion：物理像素区域选择

对应总体设计 §21、§38.4 DPI/display matrix、§40.1。Windows-only 静态库
`PB::PBScreenRegion`，不依赖 Qt，不创建 capture/D3D device，不传递文件 payload。
只证明选区满足 **单显示器准入条件**，不代表 LocalDesktop 或 Capture 性能认证。

## 入口和坐标契约

- `SelectScreenCaptureRegion(ScreenCaptureRegion&)`：同步交互选择。
- `ResolveScreenCaptureRegion(const RECT&, ScreenCaptureRegion&)`：非交互解析已有物理 RECT。
- 所有失败和取消均保持调用方结果不变；选择成功前关闭所有 overlay、释放 capture 和自有 GDI 资源。
- 调用方在一个 UI owner thread 上同步调用，不并发／嵌套运行选择器。库不新建工作线程、不初始化 COM、不改变 DPI awareness。
- 应用必须在创建 HWND 前通过 manifest 声明 PMv2。Decoder 复用 Encoder 已有的
  `cmake/PixelBridge.PerMonitorV2.manifest`；库通过 `QueryActCtxSettingsW` 检查 manifest，
  并核对线程和实际 HWND。只通过 `SetThreadDpiAwarenessContext` 覆盖线程、没有 PMv2 manifest 的宿主也会被拒绝。
  本库不支持用运行时 DPI 设置替代宿主 manifest。

`ScreenCaptureRegion` 的字段：

| 字段 | 语义 |
| --- | --- |
| `monitor` | 借用的当前 `HMONITOR`；不是可持久化标识，不调用 `CloseHandle` |
| `physicalRect` | 有符号虚拟桌面 **physical pixels**，`[left,right) × [top,bottom)` |
| `monitorPhysicalRect` | 同一坐标系中的完整 monitor 边界，不是 work area |
| `dpiX` / `dpiY` | 对应 monitor 上 PMv2 HWND 的 effective DPI；Windows 返回的同一值分别记录，不是 EDID PPI |
| `rotation` | 对应 HMONITOR 的原始 `DXGI_MODE_ROTATION`，不是 `DEVMODE.dmDisplayOrientation` 的整数转换 |

两个 RECT **不再按 DPI 缩放，不按 rotation 旋转或交换宽高**。后续 capture 的 monitor-local
坐标需要有符号宽类型的 checked 相减：`desktop coordinate - monitor origin`。Desktop Duplication
surface 的旋转采样属于后续 capture normalization，不在本模块中实现。严禁把 `QPoint/QRect`、
`QScreen::geometry()` 等 Qt logical coordinates 直接传入物理 RECT 入口。

`RECT` 宽高必须为正且不大于 `INT_MAX`，先进行 64-bit 差值计算再窄化。
`HMONITOR`、DPI 和布局都可能失效，显示环境变化后调用方必须重新解析；结果不是对未来捕获的拓扑锁。

## 原生行为和错误

- 逻辑上一个 virtual-desktop overlay，实际每个 monitor 一个 topmost、tool-window、layered HWND。
  仅绘制 GDI 遮罩、边框和提示，不截屏、不分配虚拟桌面 bitmap。
- overlay 不接收文本输入；激活前用 `ImmAssociateContextEx` 解除**该 HWND**的输入法关联。
  不调用 `ImmDisableIME`，不修改线程默认输入法上下文、调用方窗口、键盘布局或系统输入法设置。
- 输入来自 `GetPhysicalCursorPos` 的 32-bit 有符号 physical point，不采用鼠标消息 LPARAM 的 16-bit 坐标。
  四向拖动都计入两个端点所在像素，再 checked `max + 1` 转为半开 RECT；纯点击不提交。
- 跨屏／离屏／空隙选区为 invalid，显示原始选区并允许再次拖动；不裁剪、不吸附、不选择 nearest monitor。
  多个显示器都完整包含同一 ROI 时返回 `AmbiguousMonitor`。
- Escape、右键、外部失焦、`WM_CANCELMODE`、异常 capture loss 取消。显示／DPI 通知或定期重新查询发现变化时失败，
  不自动迁移旧 ROI。`WM_QUIT` 在清理后重新投递给宿主消息循环。
- DPI 查询只使用目标 monitor 上的 PMv2 HWND；非交互路径使用不显示的短生命周期 HWND。
  元数据来自独立 Win32 monitor enumeration 与 DXGI output 匹配；核对 HMONITOR、显示名称、physical bounds、factory freshness。
  缺失／歧义的 output、DPI 为零、未知 rotation 都失败，不用 96／identity 代替。
- 原生资源上界：64 个 active monitor／overlay，64 个 adapter、每 adapter 64 个 output，64 个内部事件。
  每窗口一个字体、共享两支画笔；字体像素高度最多 512。上限之外显式 `ResourceLimit`，不截断结果。
  manifest 的 `dpiAwareness` 设置缓冲区为 256 个 wchar（含终止符），超长设置失败而非截断；按首个受支持项解析优先级列表。
  显示器坐标不能被 `CW_USEDEFAULT` 解释。普通 DPI 不采用 100/125/150/175 白名单。
- `SetForegroundWindow` 是尽力的 UI 焦点请求；Windows 拒绝时用户仍可点击 overlay 激活，不绕过系统前台策略。
  无交互桌面或必要 Win32/DXGI 操作失败不会成为成功选区。

`ScreenRegionStatus` 保留 error code、stage 和 Win32/HRESULT。失败清理继续释放剩余资源；若已有主错误，返回主错误；
否则清理失败阻止提交结果。`OutOfMemory` 不跨公共 `noexcept` 边界传播。

参考：[PMv2](https://learn.microsoft.com/en-us/windows/win32/hidpi/high-dpi-desktop-application-development-on-windows)、
[GetDpiForWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getdpiforwindow)、
[GetPhysicalCursorPos](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getphysicalcursorpos)、
[MonitorFromRect](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-monitorfromrect)、
[DXGI_OUTPUT_DESC](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/ns-dxgi-dxgi_output_desc)。
窗口输入上下文使用 [ImmAssociateContextEx](https://learn.microsoft.com/en-us/windows/win32/api/imm/nf-imm-immassociatecontextex)，
manifest 查询使用 [QueryActCtxSettingsW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-queryactctxsettingsw)。

## Decoder 命令行

```text
PixelBridgeDecoder --select-region
PixelBridgeDecoder --help
```

无参数 banner 保持不变。选区成功后 stdout 为一条 JSON，包含 `coordinateSpace="physical-desktop"`、
十六进制字符串 HMONITOR、两个 RECT、DPI、原始 DXGI rotation。
rotation 数值为 identity=1、90=2、180=3、270=4；0/unknown 不能作为成功结果。
提示／错误走 stderr。退出码：0 成功或帮助，1 运行失败，2 参数错误，3 取消。
JSON 是诊断信息，不是持久化格式、文件 payload、capture surface 或完整传输接口。

## 测试与运行边界

默认测试只运行模型、状态机和非 PMv2 宿主拒绝测试，不显示 overlay。
私有 OS seam 只替换查询／输入／失败来源，fake/native 执行同一个生产选择状态机。
模型矩阵覆盖 96/120/144/168 DPI、混合 DPI、负 X/Y、多屏空隙、portrait、四种 rotation，
并覆盖 32-bit 极值、末行末列、四向拖动、失败输出不变和独立逐像素 membership oracle。

显式 `PB_BUILD_SCREEN_REGION_GATE=ON` 才注册真实桌面 Gate；要求 Windows、`BUILD_TESTING=ON`、`PB_BUILD_TESTS=ON`。
真实测试持有 `PixelBridgeDesktop` CTest resource lock、串行且有 timeout，只在当前布局运行，不修改显示设置。
完整 native Gate 要求至少两个活动显示器以及可验证的 DXGI output；条件不足非零失败，不 skip 成功。
测试鼠标使用单一 `SendInput` virtual-desktop absolute 队列，按下／移动／松开分步进行，等待精确物理位置和生产端确认；
光标恢复只发送一次定位，再有界等待精确原位置，不使用坐标容差。该测试输入设备每轴只有 65536 个归一化位置，
更大虚拟桌面会明确使实机 Gate 失败；这不是产品 RECT 的 16-bit 限制。native Gate 由父进程限时监督，
worker 异常退出或超时时仍回收 worker 并恢复物理光标，异常不会被转为成功。显示变化通知测试不修改实际显示设置。

开启 sanitizer 时，库、Decoder、单元及实机 Gate 均随现有 fuzz 配置插桩；MSVC ASan 不等于 UBSan／TSan 或 libFuzzer。
实机当前布局的通过记录，不能替代真实 125/150/175%、混合 DPI、负坐标或旋转显示器的覆盖。
本轮精确命令、结果与剩余限制见 `SCREEN_REGION_VALIDATION.md`。
