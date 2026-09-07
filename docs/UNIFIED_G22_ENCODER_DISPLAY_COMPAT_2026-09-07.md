# G22 Encoder 显示兼容修复 — 2026-09-07

## 1. 范围与状态

用户对[Citrix 现场截图诊断](UNIFIED_G22_CITRIX_DISPLAY_DIAGNOSTICS_2026-09-07.md)后的最小修复方案回复“同意”。
本次仅修复 Unified Encoder 的 Win32 屏幕身份 / DXGI 渲染设备绑定耦合，不把虚拟屏直接等同于不支持。
原 G22 双端交付 `3a840a202cf7d342a3ff2d95f2d69b3c788abcea` 与 `build-unified-release` 保留，
新增构建树为 `<repo>\build-g22-encoder-compat`；最终只打 Encoder 包。

**代码和定向单测已完成；冻结构建、独立包和同候选右屏结果随后追加。Citrix 新版真实呈现尚未验证。**
本机正常 DXGI 映射路径通过不代表 Citrix 无 output 路径已实测；不升级为远控/大文件性能认证。

## 2. 实现与不变量

- `apps/common/encoder_monitor_catalog.*`：有界 Win32 屏幕枚举与纯元数据验证；保留实际设备名、HMONITOR、
  负坐标矩形、DPI、模式旋转和刷新率。无 DXGI output 查询；明确标记 `dxgiOutputIdentityAvailable=false`，
  不伪造 adapter LUID。现场1920×1080、top=-1、96 DPI、64 Hz元数据已有定向测试。
- `apps/PixelBridgeEncoder/encoder_gui.cpp` / `encoder_runtime_cli.cpp`：Unified fullscreen 使用 Encoder 专用 catalog。
  GUI 区分未找到当前窗口屏幕与 catalog 元数据失败；后者带 code/native，不再统一提示移动窗口。
- `apps/common/local_desktop_runtime.cpp`：同源 catalog 重验所选屏幕，继续要求 Identity / 单屏 / 物理矩形、
  规范画布与资源限制；只给 Unified 显式 fullscreen 开启 `allowUnmappedHardwareAdapter`。
- `libs/PBRenderD3D/src/native_backend.cpp`：优先有 HMONITOR output 对应的硬件；无对应时，必须完整、正常结束
  adapter/output 枚举，才选择首个非 software adapter。各维度64项上限；查询错误、超限、软件独占均失败。
  硬件 device / windowed swap chain 使用原参数；设备创建前重验 LUID/binding；没有自动 WARP/Qt/GDI 回退。
- `WindowEnvironment.adapterBoundToMonitorOutput` 区分映射和独立硬件；其变化与 LUID 变化一样失效 epoch/旧帧。
  新版 Unified Encoder `presentation.adapter` 是最后可用渲染环境观察，未取得时为 null+原因；
  `successfulPresentCalls` 仅为生产端计数。错误仍保留 adapter/device/swap-chain/Present 阶段与 native code。
- Decoder 捕获目录、backend、ROI准入、恢复/摘要/发布语义不变。原严格 `monitor_catalog.cpp` 未修改。
  Protocol/Profile/Golden/FEC/资源上限/单位/GUI控件、锁定帧率、无限循环和 Esc 语义均不变。
- `tests/tools/verify_unified_package_startup.py` 改为读取已通过受信校验的 manifest 应用路径，
  支持既有打包器的 Encoder-only 根目录布局，保留 Both 双端子目录布局；未删除任何验证项目。

本次不处理默认刷新率0/1（现场不是该条件），不增加旋转屏支持或软件兼容后端，不修改 Citrix 驱动/策略。
若该会话仍限制设备创建或 Present，须按新错误阶段再判断，不宣称本修复已经覆盖。

## 3. 定向验证与原始证据

证据根：`<repo>\artifacts\g22-encoder-compat-20260907`。

| 检查 | 结果与路径 |
| --- | --- |
| 独立配置与应用/受影响测试构建 | MSVC x64 Release，C++20，warnings-as-errors；`configure.log`、`build-01.log`、`build-03.log`通过 |
| 渲染模块全部非显示单测 | 24 cases / 407 assertions，`render-tests-01.log` |
| Encoder 元数据、原严格 monitor 和新增报告 | 10 cases / 126 assertions，`application-tests-final.log` |
| 原 Unified/legacy 报告边界 | 1 case / 30 assertions，`report-boundary-final.log` |
| 本机只读显示预检 | `local-display-before-native.json`；DISPLAY1左侧主屏、DISPLAY2右侧，均2560×1440，DPI96，严格catalog成功 |

元数据边界包括缺 DPI、无 orientation 字段、非法方向、零尺寸、无效矩形、空/未终止名称；失败不污染输出。
硬件策略覆盖关闭策略、不完整枚举、无候选、software-only、software-first、正常映射优先及明确软件映射拒绝。
epoch测试验证相同 LUID 但 binding 变化时旧帧拒绝，随后新 epoch 可继续；报告测试区分 null / false / true。
`build-02.log` 保留补报告时计数器成员层级写错的编译失败；已改为 `DataWindowSnapshot.totalSuccessfulPresents`，
未削弱测试。Qt部署器仍报告环境未设置 VCINSTALLDIR；正式包会显式附带已安装工具链的 VC runtime 并做解压启动检查。

本段单测构建的 Git base 是 `75eccbc` 加未提交修复，不将 base 当作最终产品身份。
未做全量CTest、ASan、大文件阶梯、鼠标/键盘自动化或 Citrix 真实 Present；后续冻结包证据须另记完整身份。

## 4. 冻结后验证与现场交接

冻结后二进制、ZIP/hash、同候选右屏结果与复现步骤待追加；交付入口将为证据根的 `START_HERE.md`。
原双端包、原始截图/人工转录、只读诊断工具、失败日志与本次可重现构建保留，不混为一个版本。
`docs/PHASE1_GATE_REPORT.md` 保持未跟踪、未暂存；SHA-256=`076ef4c9b9f89eabccd323dbe4bffc4dc125ddaf96e6ee437d2cf5b1b1cea306`。
