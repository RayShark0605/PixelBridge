# G22 — 双端 GUI 重建与 Windows 发布候选

## 1. 当前状态与本轮授权

- 状态：**IN PROGRESS**；2026-09-07 已完成双端新 GUI 接线、限定显示器选区和无大小确认的生产策略，
  定向无显示检查已通过；GUI-only 启动、CLI 重定向、封装负例和新 GUI 原生右屏恢复已验证；最终新解压包门尚待收尾。
- 起始提交：`b86702fafcde60b1666674bd5478649affa1c559`。
- G21 已以 `PASS_WITH_SINGLE_RUN_USER_WAIVER` 关闭，详见
  [最终远控结果](UNIFIED_G21_REMOTE_1GIB_RESULT_2026-09-07.md)。本轮不重跑 G21 性能调优，
  不改写其原始性能失败、运行身份或单次豁免边界。
- 用户明确要求扩大原 G22 的打包范围：**丢弃旧的双端界面，重新交付最终 GUI**，不是给旧控件换标题。
- 仅由当前任务亲自执行；不创建子智能体、不委派其他任务。
- 只可在明确限制到右侧实验屏幕时进行必要实屏检查；不得干扰左屏、抢占用户焦点或自动操作鼠标键盘。

## 2. 已确认的新产品要求

### 界面大小单位

用户补充确认：界面 `KB`、`MB`、`GB` 与 Windows 文件大小的含义一致，即 **1024 进位**。
`1 KB = 1024 B`、`1 MB = 1024 KB`、`1 GB = 1024 MB`，`KB/s` 同样以 1024 换算。
界面资源上限显示 `500 GB`，底层仍为原来的 `500 * 1024^3` bytes（技术文档的 `500 GiB`）；
不改变 wire、字节计数、资源限制或恢复算法。早期 Decoder 预览中的十进制速度已作废，不能作为最终界面。

### Encoder

- 主体仅保留单文件选择、1..60 Hz 刷新率、开始传输及必要状态。
- 默认 15 Hz；从开始准备到停止完成期间不能调整刷新率。
- 用户补充确认：**暂不考虑超过 500 GiB 的文件，保持现有资源上限**；包含 0-byte，
  不重新引入旧的 8 MiB/单 Segment UI 限制。
- 准备完成后在当前屏幕全屏呈现 Unified 数据流；无限 Carousel 广播，不按时间、轮次或对端完成自动结束。
- Esc 表示停止传输；源变更、硬件失效、资源或协议安全错误仍必须安全停止，不能将“永不停止”解释为忽略错误。
- 新增真正的高级选项 Tab，设置必须绑定实际 runtime，不能提供无效或绕过安全门的控件。

### Decoder

- 主体仅保留输出目录、指定屏幕的 ROI、开始接收及必要状态。
- 进度只用文本表达：百分比、恢复速度（KB/s）、剩余时间；详细协议/运行数据移到高级页。
- 没有有效画面、丢帧或暂时停滞时持续等待，保留已验证恢复状态；尽可能利用后续 Carousel 修复。
- 仍只从捕获到的 ROI 像素恢复，不读取 Encoder source、报告、Session 状态或任何隐藏 payload 旁路。
- WholeFileDigest、安全发布、最终重新打开复验全部成功后才能显示完成；不覆盖已有文件。
- 高级选项单独成 Tab，默认不要求用户手动调参。
- 用户进一步明确取消大文件确认：**现有 500 GiB 上限内，所有文件大小均无需确认**。Unified 生产策略将
  `maxOutputPreallocationBytesWithoutPrompt` 设为 `maxAcceptedFileBytes`，不是由 UI 代点确认或绕过 Receiver admission。
  磁盘空间检查、路径安全、不覆盖、摘要、安全发布与最终重新打开复验继续强制执行。通用协议的历史 4 GiB
  确认策略只保留用于旧诊断/显式小阈值回归，不再是 G22 产品要求。

### 全屏与 Esc 补充确认

- 用户已确认：以开始时 Encoder 主窗口所在屏幕为目标，沿用远控 1920×1080 画布
  1:1 居中加中性背景；Esc 只在 Encoder 持有焦点时生效，不注册全局键盘钩子。

### 待确认，不得自行落地

- 用户已确认 Decoder：先选择显示器，再选整屏或框选；框选只覆盖所选显示器，精确坐标在高级页；
  开始/停止使用同一个按钮，停止保留断点；完整验证并落盘后自动停止，页面保留 100%/已完成，不自动弹窗或打开目录。
- 项目自身 LICENSE、公开分发或签名选择不得擅定；本轮本地候选不等于已公开发布或已选择开源许可证。
- 用户已确认：改为双击只打开 GUI，保留 CLI 诊断与日志重定向能力。实现及 Windows shell 等待语义见第 8 节。

## 3. 源码核对与复用边界

| 现场入口 | 当前行为 | G22 处理 |
| --- | --- | --- |
| `apps/PixelBridgeEncoder/encoder_gui.cpp` | G15 大量状态行、只读高级折叠区、运行中可调 FPS | 重做界面，改为主页面/高级 Tab，锁定运行期 FPS |
| `apps/PixelBridgeDecoder/decoder_gui.cpp` | G16 进度条与多行协议指标、旧全桌面选区入口 | 重做简洁界面，明确显示器/ROI 选择与纯文本三项进度 |
| `apps/common/local_desktop_runtime.*` | 实际 Unified 发送/捕获/解调/恢复状态机，已有单屏全屏发送模式 | 沿用生产收发路径，仅按新 UI 合同作必要适配 |
| `libs/PBRenderD3D` | 独立 D3D11 owner；既有全屏模式为 `WS_EX_NOACTIVATE` | 保持数据绘制与 Qt 分离；不能靠全局 Esc 监听干扰其他程序 |
| `libs/PBScreenRegion` | PMv2、物理像素、单显示器 ROI 验证 | 复用坐标与边界规则；不得把 Qt 逻辑像素直接交给捕获 |
| `apps/common/application_model.*` | 已验证字节、平滑恢复速度、可空 ETA | 使用真实 snapshot，不用发送 FPS、FEC 收包量或未验证 bytes 伪造完成 |
| `apps/common/encoder_session_store.*` / `decoder_resume_store.*` | 双端持久恢复与有界状态 | 不因重做 UI 丢弃协议恢复能力 |

当前真实 Profile 为 `PB-Unified-SC6-V3`、`VisualProfileId=0x5042554E49534333`、layout 10。
保留正式 wire、8 MiB Segment、FEC、Golden、捕获 lease/epoch、500 GiB policy 和文件发布不变量。
历史枚举名中的 `UnifiedLc4` 不表示正在使用旧 LC4 wire，不能只根据名称改 Profile。

## 4. 分段交付与验证预算

1. 记录现场、明确新合同与待决事项，保留旧版基线证据。
2. 重建 Encoder：简洁 Tab UI、真实高级设置、固定运行期 FPS、全屏开始/Esc 生命周期。
3. 重建 Decoder：显示器/ROI、真实高级设置、开始/停止和真实百分比/KB/s/ETA。
4. 对受影响 controller/model/GUI 和必要的选区边界做定向测试，保存 offscreen 图像供界面检查。
5. 生成独立 Windows 候选包、manifest、verifier、SBOM/notices、用户说明；从新解压包验证身份、GUI smoke。
6. 受保护右屏执行 G22 最小 1 MiB 实际像素恢复，独立读回最终文件并核对摘要；不重跑全量 CTest、
   20 GiB、64 MiB/500 MiB/1 GiB 阶梯或远控性能矩阵。
7. 显式路径暂存并创建独立提交；提交后重配/重建，区分代码身份、构建身份与实屏证据身份。

每个阶段的缺失门禁保留为未执行；不能以 GUI smoke 或旧 G21 的像素证据代替新版 GUI 完整收发。

## 5. 首次现场与无显示基线证据

证据根：`artifacts/g22-gui-baseline-20260907-154250/`（本地文件，不提交 artifact）。

- `baseline.json`：起始 commit/tree、Git 状态、保护文件哈希、G21 权威与屏幕/输入边界。
- `baseline-build.log` / `baseline-build-result.json`：Release 构建双端成功，exit 0。
- `baseline-gui-smoke.log` / `baseline-gui-smoke-result.json`：两个旧 GUI 的 offscreen smoke **2/2 PASS**，1.49 s。
- `baseline-binary-hashes.json`：此次基线双端 EXE 的 SHA-256。
- 后续 `--build-identity` 查明：基线 EXE 仍嵌入 `959678340d1946fed4fec01b4410a250b533daa3`，
  不是 checkout `b86702f`；此次基线仅执行了增量 build。两份 `PixelBridge*-build-identity.json` 保留实际身份，
  不把这轮旧二进制 smoke 算作 G22 新代码验证。Encoder 新代码已先显式重新配置再构建。
- Qt 部署提示 `VCINSTALLDIR` 未设置；当前构建通过，但这不能作为 VC runtime 已独立部署的证据，打包阶段须核对。

复验命令（无真实窗口/捕获）：

```powershell
cmake --build build-unified-release --config Release --target PixelBridgeEncoder PixelBridgeDecoder --parallel 4
ctest --test-dir build-unified-release -C Release -R '^PixelBridge(Encoder|Decoder)GuiSmoke$' --output-on-failure
```

## 6. Encoder 新界面第一阶段

- 整体替换 `encoder_gui.cpp`：主体只显示文件、1..60 Hz（默认 15）、开始与必要状态；真正的两个 Tab。
- 高级页：实际 `sessionStateRoot` 设置、明确确认后删除当前 Session、create-only 导出诊断报告和只读运行详情。
  RAW/zstd、FEC、Profile 与资源合同保持固定，不提供无效调参选项。
- 当前主窗口的 Win32 monitor identity 绑定既有 `singleMonitorFullscreen` runtime；实际 1:1 中性背景组合仍在 Qt-free runtime 中。
- 源文件预扫描/传输/停止期间禁用文件、FPS 和缓存设置，controller 也拒绝活跃运行的 FPS 修改；保留核心 runtime 的历史诊断能力。
- Esc 使用 `Qt::WindowShortcut` 且关闭 auto-repeat，不注册全局快捷键或键盘钩子，数据窗口维持 no-activate。
- 新偏好使用独立 `g22/` keys，不继承旧 Profile、240 Hz 或窗口位置；文件名和状态 QLabel 强制 PlainText。
- 无显示 smoke 使用生产 controller/预扫描/persistence，替换的只有 OS monitor 选择与 presentation owner，
  不创建实际数据窗口。覆盖默认/越界偏好、Tabs、缓存生效、准备期锁定、程序化修改 disabled 控件不影响当次 FPS、
  局部 Esc 连接、停止保留、取消删除/确认删除和设置落盘。

证据根：`artifacts/g22-encoder-20260907/`；`configure.log`、`build-encoder.log`、`build-encoder-font.log`、
`encoder-smoke-01.log`、`encoder-smoke-02.log`。最后一轮 1/1 PASS，0.28 s（尚非实屏证据）。
`preview-01/` 保留 offscreen 未发现系统字体时的方框问题；`preview-02/` 通过显式加载本机 CJK 字体恢复可读预览。
字体文件不复制进包，不增加生产字体依赖。截图只渲染自己的 offscreen widget，不读取桌面像素。

本阶段结束时尚未验证：真实全屏/Esc 用户操作、左/右屏 native containment、真实端到端、Decoder 新版、独立 package。

## 7. Decoder 新界面与用户补充要求

- 重建主页面与高级 Tab：输出目录、明确选择显示器、整屏/框选、同一个开始/停止按钮；
  只以文本显示百分比、1024 进位的 KB/s、剩余时间。没有有效画面时持续等待，停止后保留断点。
- 完成判断要求 `Completed + WholeFileDigest + finalPublishSucceeded + finalReopenVerified + outputPath`；
  在最终复验前最多显示 99.9%，0-byte 同样不能绕过完成检查。完成不弹窗、不自动打开目录。
- 高级页的真实设置为 UI 状态刷新间隔 100..2000 ms（默认 250 ms）与精确物理 ROI；
  刷新间隔不改变 capture cadence。手动打开完成目录与 create-only 诊断报告导出仅由用户显式操作触发。
- 新增 `SelectScreenCaptureRegionOnMonitor`，只为所选显示器创建 overlay；依然复验完整拓扑。
  拒绝跨屏拖动、失效 monitor scope、拓扑/DPI 变化；取消保持原选区。选区期间禁用配置重入，关闭请求延期处理。
- 删除 GUI 的大文件确认窗口、请求回调及自动弹窗调度。实际 Unified 生产 policy 自动接受 0..500 GiB，
  超上限仍拒绝。没有通过调高测试阈值、代点确认、改 wire 或放宽存储安全实现需求。
- 所有 GUI 大小单位统一按 Windows 的 1024 进位：源大小、资源上限和恢复速度都使用 B/KB/MB/GB 标签。

### 7.1 定向构建与 evidence

证据根：`artifacts/g22-decoder-20260907/`；限定屏幕 selector 早期证据另在 `artifacts/g22-roi-20260907/`。

- `build-no-size-confirmation.log`：Release 双端、PBApplicationTests、PBQSettingsTests、PBScreenRegionTests 构建 exit 0。
- `tests-no-size-confirmation.log`：双端 offscreen GUI smoke、PBQSettingsTests、PBScreenRegionTests **4/4 PASS，0.90 s**。
- `preview-05-no-size-confirmation/`：双端主/高级页面的 idle/stopped/completed 共 8 张自身 widget 渲染图；
  字体、Unicode、Windows 单位和取消确认文案已检查。未截取桌面或操作真实输入。
- `runtime-sc6-fixture.log`：PBApplicationTests `[g22],[g16]` **6 cases / 305 assertions PASS**。
  包含 0、1、4 GiB、4 GiB+1、500 GiB 自动接收策略，500 GiB+1/UINT64_MAX 拒绝；
  RAW/空文件实际 runtime + Receiver/Storage、确认旧诊断隔离、故障切换、停止/恢复与负例。
- 大尺寸只是资源策略边界测试，没有创建或传输 4/500 GiB 文件。上述像素来自生产 Encoder + CPU oracle、
  OS/capture/GPU 边界采用测试替身；不是新版真实屏幕、远程性能或全文件大容量认证。

### 7.2 发现并修复旧 G16 fixture 的 SC6 时序假设

`runtime-no-size-confirmation.log` 保留第一次 5/6 PASS、1 case / 2 assertions FAIL 的原始结果。
旧用例先排入帧 0 和帧 1，却假定它们只恢复第一个 Segment；实测 SC6-V3 的这两帧分别携带
SegmentOrdinal 0/1（各 7 个有效 Transport），第二帧可能在故障注入前完成整个文件。
因此不是无确认策略导致捕获退化，而是历史测试将正常提前完成当作 fallback/stall 失败。

修复只调整 fixture 的输入时序：先给帧 0、保留故障注入/停滞/重复帧/断点所有断言，重启后再给后续帧；
新增逐 Transport parse 与 SegmentOrdinal 断言证明输入关系。`build-sc6-fixture.log` 和
`runtime-sc6-fixture.log` 保存修复构建及通过证据，没有放宽生产条件或跳过失败用例。

### 7.3 重放命令

```powershell
cmake --build build-unified-release --config Release --target PixelBridgeEncoder PixelBridgeDecoder PBApplicationTests PBQSettingsTests PBScreenRegionTests --parallel 4
ctest --test-dir build-unified-release -C Release -R '^(PixelBridge(Encoder|Decoder)GuiSmoke|PBQSettingsTests|PBScreenRegionTests)$' --output-on-failure
& .\build-unified-release\tests\PBApplication\Release\PBApplicationTests.exe '[g22],[g16]'
```

`reviewed-evidence-summary.json` 记录父提交、源文件与 EXE hashes、证据边界；
`reviewed-working-tree.patch` 和新增源码副本保存该工作树。后续提交必须重新 configure/build 才能将 EXE 的
`--build-identity` 绑定到新提交；本轮工作树构建的 embedded parent 不等于 clean committed candidate。

本阶段已提交为 `879520a401d317c15a5d9d4d2a3385b83822bff4`。随后重新 configure/build 双端，
`postcommit-build-identity.json` 确认两个 EXE 的嵌入 commit 均一致；该重建本身不是实屏证据。

## 8. GUI-only 启动与 CLI 保留

- 用户确认后，两个 MSVC EXE 改为 `WIN32_EXECUTABLE` / Windows GUI subsystem。
  使用标准 `wmainCRTStartup` 保留 CRT 初始化与原有 Unicode 参数解析，不改成自定义 raw 入口；
  禁用 Qt 的额外 entrypoint adapter，保持原 `wmain` 命令路由。
- 无参数 GUI 启动不 attach/create console。仅 CLI 分支尝试 `AttachConsole(ATTACH_PARENT_PROCESS)`，
  从不 `AllocConsole`、激活窗口或生成键盘事件。
- `application_console.h` 独立保存 stdin/stdout/stderr 的重定向，再修复 GUI CRT 的未绑定标准流；
  对已有管道、文件和 NUL 不进行统一 `freopen(CONOUT$)` 覆盖。真正需要绑定时使用 owned duplicate，
  不关闭父进程给出的原始重定向句柄。
- API 依据：[Microsoft AttachConsole](https://learn.microsoft.com/en-us/windows/console/attachconsole)、
  [GetStdHandle 的 GUI/附着/继承规则](https://learn.microsoft.com/en-us/windows/console/getstdhandle)、
  [标准 CRT 入口选项](https://learn.microsoft.com/en-us/cpp/build/reference/entry-entry-point-symbol?view=msvc-170)。
- **Shell 行为边界：** 进程自身退出码不变，但交互式 PowerShell 对裸调用的 GUI EXE 不保证同步等待。
  使用 `& .\PixelBridgeEncoder.exe --version | Out-Host; $LASTEXITCODE`、捕获/重定向管道，
  或 `Start-Process -Wait -PassThru` 获取退出码。不是修改协议或静默丢失 stderr；已作为实测行为写入用户指南。

证据根：`artifacts/g22-startup-20260907/`。

- `build-gui-subsystem.log`、`build-probes.log`、`build-mixed-probes.log`：双端及独立 stdio probes 构建 exit 0。
- `test-03-reviewed.log` / `test-03-reviewed/summary.json`：**12/12 PASS**，覆盖双端 PE GUI/x64、
  `--version`/`--build-identity`/错误 stderr 与 exit 2、PowerShell 合流及退出码、独立 pipe/file/NUL、
  无参数无 console、附着自有隐藏父 console 后的三标准句柄修复，以及 stdout=file、stderr/stdin=console 的混合情况。
- `gui-smoke-reviewed.log`：新子系统下双端 offscreen GUI **2/2 PASS，0.65 s**。
- `test-01.log` 保留首次测试错误：旧 presentation 未知命令的帮助输出在 stdout，不能用该路由证明 stderr 丢失。
  测试改用真实 runtime 的非法参数 stderr/exit 2 路径；未改产品错误输出约定。`test-02.log` 为随后 11/11 通过，
  第三轮增加混合重定向场景。
- 隐藏 console 只属于测试启动的 parent/probe，不显示产品窗口、不访问鼠标键盘或发送 input event；
  这不是人工双击/Esc 或 native 数据窗口验证。

重放命令：

```powershell
cmake --build build-unified-release --config Release --target PixelBridgeEncoder PixelBridgeDecoder PBGuiConsoleProbe PBConsoleParentProbe --parallel 4
& <python> -X utf8 tests/PBApplication/test_gui_startup.py --build-directory build-unified-release --evidence-directory artifacts/g22-startup-fresh --powershell (Get-Command pwsh).Source
ctest --test-dir build-unified-release -C Release -R '^PixelBridge(Encoder|Decoder)GuiSmoke$' --output-on-failure
```

## 9. 发布前 Profile 身份与旧 CLI 包装脚本兼容

启动改造已独立提交为 `bd0dbad64faf615ab8a92dfe15f7863ddde6d402`。

- 实测仅 `@(& GUI.exe ...)` 或单独 `2>&1` 仍不足以保证同步等待，必须进入实际管道或显式等待进程。
  因此给现有工具的 metadata/monitor 查询补上 `Out-String -Stream`，需要终端输出的 Sender 包装调用补上
  `Out-Host`，已有 stdout/stderr 文件重定向后补上 `Out-Null`。保留原编码/重定向、参数、异常和退出码。
  该兼容修正避免包装脚本在 Encoder 仍运行时提前释放源只读 lease；不改 G21 流程或已封存 artifacts。
- `test_g21_wrappers.py --gui-subsystem-fixture` 将现有纯生命周期 C# fixture 编译为 GUI subsystem，
  验证 Windows PowerShell 5.1 的真正等待路径；不使用产品 EXE、真实屏幕、输入事件或非视觉 payload 通道。
- 新增只读 `--unified-profile`。双端从实际编译的 `kUnifiedVisualProfile` 输出完整结构化 manifest，
  包含身份、画布/tiles、regions、lanes、carriers、mixed slots、FEC 与 presentation 参数。
  这是包身份诊断，不是新 wire descriptor，也不能作为 Decoder 的 payload 输入。
- 两个实际 EXE 输出的规范 JSON 相同；当前 SHA-256 为
  `312c7832854f8710719ee2d0e44e920e7270b48638044eaa4b6af9ee24b1cb8b`。
  新 Unified 包必须封印这一实际输出，不能继续把旧 `remote-lf4`/layout 7 manifest 当作当前产品。

证据：`artifacts/g22-package-20260907/` 内的 `build-profile.log`、双端 `*-unified-profile.json`、
`profile-and-cli-tests.log` / `profile-and-cli-tests/summary.json`（**16/16 PASS**，增加显式文件重定向和 Profile 对等检查）、
`powershell-syntax.json`；纯包装脚本回归在 `artifacts/g22-cli-wait-20260907/summary.json`，
日志在 `artifacts/g22-startup-20260907/gui-wrapper-compatibility.log`。

以上仍是源码/CLI/无产品窗口证据，独立包、从包启动、右屏真实恢复及人工 Esc 验证尚待完成。

本阶段提交为 `1c472d111564911b4b8995d8b6cf93317096d849`；GUI subsystem 包装 fixture **19/19 PASS**。

## 10. 独立 Unified package 工具初版

`tools/PBUnifiedRelease/` 提供单独的生产器和只读 verifier，复用保留的历史 inventory/SBOM/seal 思路，
但采用明确的 Unified schema 和 SC6-V3 完整编译 Profile digest，不改变旧 LF4 schema。
生产器要求 clean committed source、实际 EXE commit 一致、显式 VC runtime/notices 来源；
包内包含用户指南、Qt/vcpkg/MSVC 依赖资料和单文件独立 verifier。项目 LICENSE 保留 NOASSERTION，范围只到本地候选。

Verifier 不启动包内程序；静态检查 GUI/x64 PE、大小/路径/哈希、Profile/工具链/SBOM/notices/seal，
递归拒绝重复/大小写歧义 JSON key、非整数 size、reparse 与 ZIP symlink，并对 ZIP 实际解压读出设置封印长度边界。
生产失败保留 create-only artifacts，不递归删除未核实路径。

第一阶段证据 `artifacts/g22-package-20260907/verifier-functions-01.log`：
**28/28** 有界函数检查通过，覆盖路径、严格 JSON、整数类型、SHA-256 和解压长度 mismatch。
完整包生成、独立验证负例、新解压 GUI smoke 尚待执行；这个工具初版提交不代表 G22 交付完成。

保护文件 `docs/PHASE1_GATE_REPORT.md` 起始 SHA-256：
`076ef4c9b9f89eabccd323dbe4bffc4dc125ddaf96e6ee437d2cf5b1b1cea306`。
该文件保持原有未跟踪状态，不修改、不暂存、不提交。

## 11. 首包实证与 ZIP 符号链接修复

首包 `c41c8c174ea8fb0f88af668d1392c88f2217e054` 已成功生成，100 个 payload 文件共 132277484 bytes，
manifest SHA-256 `dd72d98722ad526dddf1e94d9d1682507c2ac1562a457e6cd50a0564fa6836f6`。
证据为 `artifacts/g22-package-20260907/package-01.log` 和 `package-01/`；这是工具初版候选，不是最终交付包。

- `negative-01/` 真实派生 ZIP 的 Unix symlink 条目未被拒绝：PowerShell 将 `0xA0000000` 解释成
  有符号 Int32，而规范化属性为 UInt32，比较不相等。只调整 verifier 为先右移取得 16-bit Unix mode 再比较。
- `negative-02/` 已证明 symlink 拒绝生效；随后 Python fixture 因 `writestr` 修改共享 ZipInfo 的 offset 导致
  自身 CRC 读取失败，改为复制 ZipInfo，不改变 verifier 的判断。
- `negative-03/summary.json`：**20/20 PASS**。完整 seal+ZIP 基线、缺失/篡改/额外文件与空目录、重复/不安全
  路径、字符串/小数/bool/负 size、错误 Profile、重复 JSON key、owned junction、ZIP symlink/duplicate/traversal、
  恢复后完整复验均有独立日志。验证过程不运行包内 EXE，原包/ZIP/seal hashes 保持不变。
- 新增 `tests/tools/test_unified_package.py` 可在 create-only 派生目录重放；每个修改恢复后再进行下一项，
  原始候选始终只读。原首包内 verifier 仍为旧版本，故必须重新生成包才可最终交付。

## 12. 新 GUI 原生路径的受保护诊断入口

两端新增显式 `--gui-native-smoke EXPERIMENT_MONITOR PROTECTED_MONITOR SOURCE_OR_OUTPUT NEW_EVIDENCE_DIR SECONDS`。
这是有界原生检查入口，正常无参数 GUI 不进入此分支，不增加主页面选项，也不改变持续传输合同。

- 强制不同显示器且实验屏完整位于保护屏右侧；先将隐藏的本应用窗口放到实验屏再 no-activate 显示，
  反复验证完整显示器身份、窗口物理范围、进程未取得前台焦点。身份/范围失败时停止自身流程，不恢复或抢夺用户焦点。
- 通过真实控件信号启动默认 Controller/Runtime/D3D/WGC 路径，不替换解调器/像素来源。
  Decoder 只获得输出目录及明确的监视器 ROI，不接收源路径、摘要、Session cache 或任何 payload 旁路。
- 只接受 create-only evidence 和 5..180 s 明确期限；Encoder fixture 限定 1 MB，Decoder fixture 只用全新空输出目录。
  QSettings 隔离在 evidence 中，不污染用户配置；ready 文件仅供外部测试排序，不由另一产品程序读取。
- `run_gui_native_smoke.py` 使用 CSPRNG 产生恰好 1,048,576 bytes；Receiver capture-ready 后启动 Encoder，
  两个期限独立，最终文件必须经过原 Runtime 的 whole digest/publish/reopen，再由 Python 独立 SHA-256/BLAKE3 重读验证。
- Encoder 期限后只在本进程调用已有 Qt shortcut signal，不合成 OS input；证据明确记为 `physicalEscKeyTested=false`。
  同样没有用鼠标实际拖动 ROI；该行为的算法/取消/跨屏边界仍由定向 selector 单元测试证明，不能冒充人工交互。

新增入口已定向构建；`artifacts/g22-native-20260907/startup-01.log` **18/18 PASS**（包含两端参数、期限、无效显示器
在任何窗口/evidence 创建前拒绝），`offscreen-01.log` **2/2 PASS**。本节此时只记录入口检查，原生运行结果待下一阶段。

## 13. 冻结新 GUI 原生1 MB路径通过

`ef626185a53db05ec807cb497c5e14b802644937` 重新 configure/build 后，双端 EXE 嵌入身份一致。
`artifacts/g22-native-20260907/run-01/summary.json` 记录两进程 exit0、无 watchdog 强杀、无输入事件和完整外部验证。

- 保护屏 `\\.\DISPLAY1`，实验屏 `\\.\DISPLAY2`；发送数据窗口精确覆盖右屏 `[2560,0,5120,1440]`。
  双端重复检查窗口包含关系、显示器身份和前台进程，`safetyHeld=true`；不移动鼠标、注入按键或激活窗口。
- GUI Start 默认15 Hz，参数锁定；Decoder从327个实际WGC到达帧中恢复完整1,048,576 bytes，
  whole digest/rename/publish/final reopen均为true，页面100%、269.3 KB/s、00:00:00，自动接收结束。
- 最终独立重读 SHA-256 `af479c6abf8ea4b537641714b7eb637c267b40ecdce4d686cce87c515ceee29e`；
  BLAKE3 `9eac3ff6fd05c8e427e27b912fe3ce19ca40d66cf298109ca67656c674f9458c`，与源及in-band whole digest一致。
- Encoder未接受接收完成信号，仍广播40,975 ms，直至自身45秒诊断期限；本地Qt shortcut连接和停止后解锁通过。
- 原生evidence是新GUI+真实像素，不是人工按下Esc、人工ROI拖动、全文件容量/远控效率/干净无开发环境机器认证。
  最终交付仍要从新解压的独立包运行metadata、offscreen和同类最小原生路径，不能只用此build目录证据代替。
