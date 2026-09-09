# Step1 本地准备执行记录 — 2026-09-07/08

> **状态（2026-09-09 记账）：** 本文属 G22 之后非本机吞吐优化线在 2026-09-08/09 的记录或合同文本，正文未改写。该目标已于 2026-09-09 由用户主动停止（未完成、非技术阻塞）；文中「目标进行中 / 下一步 / 待执行」等表述仅属当时时点，不构成继续执行或现场操作的授权。当前状态见 [暂停交接](REMOTE_THROUGHPUT_RESUME_HANDOFF_2026-09-09.md) 与 [文档状态矩阵](DOCUMENT_STATUS_2026-09-09.md)。

## 1. 范围与状态

用户确认 [Step1 本地准备计划](REMOTE_STEP1_MEASUREMENT_CONTRACT.md) 后执行。只做 B0/M1 身份隔离、
固定输入及 encoded bytes、只读计时、迟加入事后匹配、受影响测试和远程操作包。
不创建子智能体／定时任务，不启动实屏、远程、后续 Step，不提交或推送。

当前本地状态：**COMPLETE / PREPARED**。整个 Step1：**PREPARED/PARTIAL / FIELD_NOT_RUN**。
最终封存及验证见第 6 节起；第 4、5 节保留开发阶段及失败历史。本轮结束，不写 Step1 PASS，不自动启动 Step2。

证据根：`<repo>\artifacts\remote-step1-20260907-prep01`。
初始上下文与已有改动：`context/preflight.json`、`context/pre-existing-tracked.diff`、`context/roadmap-at-start.md`。
已有 README／旧统一路线索引改动保留；原 untracked 路线、录像与受保护报告保持其既有身份。
`docs/PHASE1_GATE_REPORT.md` 不修改、不暂存、不提交；未把它纳入 M1 source inventory 或包。

## 2. 起始身份与已取得证据

- HEAD：`4a36d0f1b84c94c7388a58b8249a3f1b6ec0634a`。
- tree：`74d38b9b9d96e488369c5367507ff535f67861fc`。
- B0 独立干净 clone：`<repo>\artifacts\remote-step1-20260907-prep01\source\B0`。
- B0 独立构建：`<repo>\build-remote-step1-b0-prep01`。
- M1 独立构建：`<repo>\build-remote-step1-m1-prep01`。
- 构建／测试日志统一保存在上述证据根 `build-logs`／`tests`；失败文件不覆盖。

### B0 原始对照包

`packages/B0/PB-Unified-B-4a36d0f1-cec2e546.zip`：

- ZIP SHA-256：`0906e142f5f3bb5e0b7a4ca066d8909e2d4141deadd12b6862fabbebc7fab0ba`。
- manifest SHA-256：`f3f9e72b7e4d5ce063c92ae00cb7c6c4b3df77729df0ace59b2b99cf746d00f5`。
- 原打包器 source fingerprint：`cec2e546e1956baef6ba4fc6c2478163eae5c10fd5bca807d742445642bcb800`。
- Encoder EXE SHA-256：`113648a14cdc7dcf04a144c831e1a10df12d968ed30798f17dd84d624c36b449`。
- Decoder EXE SHA-256：`d61a3fb48999f3d7a489715ae8af8df4939430929cc227dd73a3acfc71e425d0`。
- `packages/b0-package-result.json` 记录原 verifier PASS；`tests/B0-clean-startup/summary.json` 记录全新解压 offscreen 双端启动 PASS。
- 未运行 B0 实屏／Citrix，不把该包与旧本地 pixel evidence 重新配对。

### 固定输入初次编码审计

`input/source-ledger-initial.json` / `input/input-audit-initial.json`：
raw `40,517,389 B`、encoded `40,410,277 B`；5 段中 2 段 Raw、3 段 Zstandard。
每段 bytes／digest 和完整双摘要见测量合同及 JSON。封存 M1 后需再用最终工具做固定输入复验。
原临时录像 Bootstrap 诊断 summary／trace 已按显式路径复制到 `historical-diagnostics`，
继续只标记 `OfflineRecordingDiagnostic`，没有运行生产 payload FEC 录像回放。

## 3. 实现边界

- `apps/common/run_measurement.*`：有界只读记录器、计时偏序／unavailable 检查、独立测量 schema 和编译指纹。
- `apps/common/measurement_capture_consumer.h`：透明转发全部 capture lease／stage 方法，仅记录首个观察时间。
- `apps/common/local_desktop_runtime.*`：实际成功 Submit 前进之前的身份、绑定方程接纳、存储／发布里程碑，源审计共用生产预扫描。
- `libs/PBStorage/include/pbstorage/output_file.h`、`libs/PBStorage/src/output_file.cpp`：可选进程内时间点，不改变原摘要、rename、reopen、失败回滚或 resume 逻辑。
- `apps/common/run_report.cpp`、`application_model.h`：`.3` 可选 measurement；旧 `.2` 与既有 goodput 不变。
- `apps/common/step1_gui_evidence_qt.*` 与双端 GUI/main/controller：显式实验入口、独立 INI／新 run 根、只在 GUI 线程有界落盘，关闭 join 后最终封存。普通 GUI 默认行为不变。
- `tools/PBRemoteThroughputStep1`：审计、source seal、M1 独立包／校验、GUI 启动预览和双端停止后的精确匹配；无生产回传通路。
- `tests/PBApplication` 与 `tests/tools/test_remote_step1.py`：仅对应模块窄测与独立合成 fixture；其报告不是现场样本。

## 4. 开发验证（最终身份复验另列）

| 项目 | 开发证据 | 结果 |
| --- | --- | --- |
| B0 双端构建、原 packager、独立解压启动 | `build-logs/*b0*`、`tests/B0-clean-startup` | PASS |
| M1 第一轮相关 C++ 构建 | `build-logs/build-m1-01.log`、`build-m1-02.log` | PASS |
| `[step1],[application][report]` 第一轮 | `tests/application-step1-01.log` | 17 cases / 262,878 assertions PASS |
| PBStorage 受影响模块窄测 | `tests/storage-01.log` | 17 cases / 219 assertions PASS |
| Unified scheduler 不变性 | `tests/scheduler-01.log` | 10 cases / 247,055 assertions PASS |
| GUI 证据 writer / 冲突文件 / 新根负例 | `tests/gui-evidence-01.log` | 3 cases / 31 assertions PASS，无 UI |
| 工具合成负例和完整事后匹配 | `tests/tools-01/summary.json` | 12 cases PASS，无产品 EXE／像素 |
| 固定文件生产编码与独立双摘要 | `tests/source-audit-01.log`、`tests/input-audit-01.log` | PASS |

构建中 Qt 部署器提示未设置 `VCINSTALLDIR`，不是编译错误；B0/M1 包显式携带同工具链 VC runtime 并做解压装载验证。
不运行完整 CTest／ASan／WARP 矩阵／native Gate／大于 8 段或 1 GiB 长测／录像 payload 回放。

## 5. 冻结过程的历史检查点与现场边界

### 冻结前发现并保留的既有启动测试汇总错误

`tests/gui-startup-sealed01/summary.json` 保留了 18 项检查全部 passed、但 `allPassed=false` 的原结果，脚本当时仍退出 0。
核对 HEAD 版本发现 `tests/PBApplication/test_gui_startup.py` 的汇总仍写死 16 项，而实际为每应用 6 项、公共 stdio 6 项，共 18 项。
此次只校正显式预期总数，并使汇总不通过时退出非零；没有删减检查、改变断言或跳过失败项。
第一份 `source/M1-seal01` 及其构建、日志保留为未晋级冻结尝试；需使用包含该修正的下一份源码 seal 重构建和重新验证。

该检查点当时的本地待办（已由第 6–9 节收口）：新增 orphan／透明 capture 测试收口、最终 source seal 重构建／窄测、M1 独立包和新解压启动、
可迁移 PowerShell source audit 验证、完整命令和执行清单、最终状态文件。
现场仍需：另行明确授权、重核右屏拓扑、正式迟加入主文件样本、录屏开／关及 B0/M1 开销对照。
不将手机人工耗时升级为自动计时样本；不宣布提速，不自动进入 Step2。

## 6. 最终 M1 封存与固定输入复验

最终候选不是 clean G22 release，而是 `InstrumentedExperiment`：

| 项目 | 最终身份 |
| --- | --- |
| base | `4a36d0f1b84c94c7388a58b8249a3f1b6ec0634a` |
| 完整源码 fingerprint | `9a0278779859ed87790ccb19777e7a7d5f0914b709075a3562a2d9e12b482024` |
| source ZIP SHA256 | `3d113821f37b243b3c06b54cbf25e123889b625ef5a164a94e0ad6700f672ca3` |
| tracked diff SHA256 | `49076832f6546a34673a47d780fe36dddb9c1cea371cf43e75d21d55f7ef7d30` |
| M1 package manifest SHA256 | `9a14936404f5270a5b62fcaa7a605ea1269ea3adae154ae8c28ef962666d3095` |
| M1 ZIP SHA256 | `5b136f28fd6f5b397842b0e8dcf38404467dc2f75e3b41a2524f42b110d7263b` |
| M1 ZIP bytes | `64,945,763` |
| Encoder SHA256 / bytes | `d0fb1321aab8ee634715e2ef1e6aabd50ef77089e9d5db87678537e2717ee4a0` / `1,328,128` |
| Decoder SHA256 / bytes | `a23fa6ac7e379dc3b1a64ac69d7ea29329e09719e51adb1c8831807b6b6dfb0e` / `2,707,456` |
| Profile JSON SHA256 | `312c7832854f8710719ee2d0e44e920e7270b48638044eaa4b6af9ee24b1cb8b` |

完整路径：

- `<repo>\artifacts\remote-step1-20260907-prep01\source\M1-seal02\source-seal.json`
- `<repo>\artifacts\remote-step1-20260907-prep01\source\M1-seal02\source-snapshot.zip`
- `<repo>\artifacts\remote-step1-20260907-prep01\packages\M1\package-result.json`
- `<repo>\artifacts\remote-step1-20260907-prep01\packages\M1\PB-Step1-M1-9a0278779859.zip`
- `<repo>\artifacts\remote-step1-20260907-prep01\packages\M1\PB-Step1-M1-9a0278779859.seal.json`
- `<repo>\artifacts\remote-step1-20260907-prep01\packages\M1\PB-Step1-M1-9a0278779859\Encoder\PixelBridgeEncoder.exe`
- `<repo>\artifacts\remote-step1-20260907-prep01\packages\M1\PB-Step1-M1-9a0278779859\Decoder\PixelBridgeDecoder.exe`

M1 使用独立 manifest/source seal，不给原 G22 打包器增加 dirty-source 豁免。DLL／许可证复用 B0 的逐项 hash；
两构建的 vcpkg status SHA256 同为 `7230473147017f6158eacdd96e1d524475dd956935aea3009cc04715f8127c8e`。
Qt 6.10.1、MSVC 14.44、VC runtime、blake3 1.8.5、zstd 1.5.7、wirehair 2.0.0、libpng 1.6.58 等细项在包内依赖清单。
原 SPDX 标为 dependency-reference，只证明依赖溯源，不能冒充 M1 应用或现场认证。

最终 C++ 工具重新审计得到的 source ledger 与 initial **逐字节一致**，SHA256 均为
`d4bfa21125372935eb7310386a76fc8839ae9b8440f3138f5cb2178e0a2aad5d`。
`<repo>\artifacts\remote-step1-20260907-prep01\input\input-audit-final.json` 保存独立 Python SHA256/BLAKE3 与逐段 raw digest。
`portable-source-before.json` / `portable-source-after.json` 是**本地脚本资格验证**，不是远程运行证据；两份完全一致，
其双摘要、5 段 encoded ledger 与最终 Python 审计一致。正式远端运行仍须重新取得远端 before/after。

## 7. 最终受影响验证及实际命令

最终配置、构建、测试和封包的逐条 argv、绝对路径、UTC 起止、退出码、日志位于：

- `<repo>\artifacts\remote-step1-20260907-prep01\local-sealed02-commands.json`
- 对应实际执行脚本 `<repo>\artifacts\remote-step1-20260907-prep01\run-local-sealed02.ps1`（原路径已有证据，不可直接重跑覆盖）。

| 验证 | 实际结果 | 最终证据根下位置 |
| --- | --- | --- |
| M1 sealed02 VS2022 x64 Release 相关目标 | exit 0，warnings-as-errors | `build-logs/configure-m1-sealed02.log`、`build-m1-sealed02.log` |
| PBApplication Step1 + report | **19 cases / 262,907 assertions PASS** | `tests/application-step1-sealed02.log` |
| Qt evidence writer，无 UI | **3 cases / 31 assertions PASS** | `tests/gui-evidence-sealed02.log` |
| PBStorage 受影响模块 | **17 cases / 219 assertions PASS** | `tests/storage-sealed02.log` |
| Unified sender scheduler 参考不变性 | **10 cases / 247,055 assertions PASS** | `tests/scheduler-sealed02.log` |
| Python 工具／身份／关联合成负例 | **13 cases PASS** | `tests/tools-sealed02/summary.json` |
| 既有 GUI PE/CLI/stdio 拒绝路径 | **18 checks、allPassed=true、exit 0** | `tests/gui-startup-sealed02/summary.json` |
| B0 新解压离线启动 | PASS | `tests/B0-clean-startup/summary.json` |
| M1 完整清单、source ZIP、package ZIP | PASS | `build-logs/package-m1-sealed02.log`、`tests/portable-sealed02/python-package-zip-verify.stdout.txt` |
| M1 新解压、隔离 PATH、offscreen 双端 | **10 checks PASS** | `tests/M1-clean-startup/summary.json` |
| Windows PowerShell 5.1 工具和额外 CLI 负例 | **10 checks PASS**，包含预期 exit 1/2 的拒绝 | `tests/portable-sealed02-attempt03/summary.json` |

最小核心命令（本次已运行；复现时换新目录后按顺序执行）：

```powershell
$repo = '<repo>'
$build = '<repo>\build-remote-step1-m1-prep01'
$root = '<repo>\artifacts\remote-step1-20260907-prep01'
$python = '<python>'
$cmake = 'C:\Program Files\CMake\bin\cmake.exe'
& $cmake -S $repo -B $build -G 'Visual Studio 17 2022' -A x64 '-DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake' '-DPB_QT_ROOT=<qt>6.10.1/6.10.1/msvc2022_64' '-DPB_BUILD_APPS=ON' '-DPB_BUILD_TESTS=ON' '-DPB_BUILD_TOOLS=ON' '-DPB_TREAT_WARNINGS_AS_ERRORS=ON' '-DPB_STEP1_SOURCE_FINGERPRINT=9a0278779859ed87790ccb19777e7a7d5f0914b709075a3562a2d9e12b482024'
& $cmake --build $build --config Release --target PixelBridgeEncoder PixelBridgeDecoder PBStep1SourceAudit PBApplicationTests PBStep1GuiEvidenceTests PBStorageTests PBUnifiedSenderSchedulerTests PBGuiConsoleProbe PBConsoleParentProbe --parallel 4
& "$build\tests\PBApplication\Release\PBApplicationTests.exe" '[step1],[application][report]' --reporter compact
& "$build\tests\PBApplication\Release\PBStep1GuiEvidenceTests.exe" --reporter compact
& "$build\tests\PBStorage\Release\PBStorageTests.exe" --reporter compact
& "$build\tests\PBApplication\Release\PBUnifiedSenderSchedulerTests.exe" --reporter compact
& $python -B -X utf8 "$repo\tests\tools\test_remote_step1.py" --evidence-directory "$root\tests\tools-sealed02"
& $python -B -X utf8 "$repo\tests\PBApplication\test_gui_startup.py" --build-directory $build --evidence-directory "$root\tests\gui-startup-sealed02" --powershell 'C:\Users/<user>\.cache\codex-runtimes\codex-primary-runtime\dependencies\native\powershell\pwsh.exe'
& "$build\tools\Release\PBStep1SourceAudit.exe" --source 'C:\Users/<user>\Desktop\PixelBridgeTest\.conan.zip' --output "$root\input\source-ledger-final.json"
& $python -B -X utf8 "$repo\tools\PBRemoteThroughputStep1\step1.py" audit-input --source 'C:\Users/<user>\Desktop\PixelBridgeTest\.conan.zip' --ledger "$root\input\source-ledger-final.json" --output "$root\input\input-audit-final.json"
```

fresh startup 使用 `--gui-smoke` 的 offscreen 测试路径；native-smoke 仅执行缺参数／非法时长／不存在监视器的前置拒绝，不打开数据窗口。
没有运行实屏、远程、录屏开／关、完整 CTest、ASan、GPU/WARP 矩阵、进程崩溃矩阵、1 GiB 长测或生产录像 payload 回放。

### PowerShell 5.1 的两份环境失败证据

1. `tests/portable-sealed02`：Python 完整包验证和 M1 fresh startup 已通过，随后 Windows PowerShell 默认 Restricted 拒绝运行脚本。
2. `tests/portable-sealed02-attempt02`：只给子进程显式 RemoteSigned 后，继承的 PowerShell 7 模块路径导致 Get-FileHash 不可加载。
3. `tests/powershell51-environment-inherited.*` 与 `powershell51-environment-native-module-path.*` 的单变量探针证明原生模块路径可正确加载系统命令。
4. `tests/portable-sealed02-attempt03`：仅测试子进程使用 RemoteSigned 和原生模块发现，包验证、两端不启动预览、本地输入两次审计、错误 pin、64 MiB+1 拒绝、CREATE_NEW 冲突保护、两端缺失测量参数拒绝全部通过。

实际命令及预期 exit 0/1/2 均在该目录 `summary.json`；脚本为 `verify-local-package01.py`、`02.py`、`03.py`，历史不覆盖。
未执行 `Set-ExecutionPolicy`，未修改持久用户／机器策略或环境变量。远端若受策略限制仍须操作者处理，不授权自动更改。

## 8. 精确复现与证据身份规则

1. 直接验证既有交付：使用 `packages/M1/package-result.json` 的绝对路径及独立 pinned manifest，调用包内 `step1.py verify-package` 并指定 `--archive`。本次 `verify-local-package01.py` 记录了完整 argv。
2. 需要重新构建时，新建独立 `4a36d0f` clone；在**该全新 clone** 中还原 `source/M1-seal02/source-snapshot.zip` 的完整文件，不在现有工作区覆盖。保留 `.git` 的 base 身份，以 source inventory 逐文件核对后构建。
3. 原源码快照、source fingerprint、compiled measurement identity、EXE hash 是不同层级。新重建不保证 MSVC EXE 逐字节一致；必须重新封包、重新记录 EXE/ZIP/seal，不能复用本次旧 seal。
4. 本次最终索引、执行记录和合同澄清为封存后的**仅文档差异**。`post-seal-documentation.diff` 与 `final-scope-audit.json` 分开记录；不把当前工作树说成与封包源码逐字节相同。
5. 测量开始前 SourceAudit 与 Encoder 都走生产预扫描。运行中的 Decoder 不读取任何输入审计、期望编码字节、Sender trace 或外部 digest；事后工具仅在双方已停止后读它们进行核验。
6. 主时间不切换为 capture-ready 起点，不剔除控制等待或恢复尾部。未完成、resume、计时/身份/coverage 缺口、cleanup 警告和 IO 超限均保持 null/失败，不能提升为样本通过。

## 9. 交付记录与仍需人工的现场验收

- 本地操作包入口：`<repo>\artifacts\remote-step1-20260907-prep01\field-kit\START_HERE.md`。
- 配套 Sender/Receiver Checklist、未确认 operator template、完整候选 identity 和 field-kit seal 均在同目录。
- 最终机器可读状态：`<repo>\artifacts\remote-step1-20260907-prep01\PREPARATION_FINAL_STATUS.json`。
- 最终范围与文件身份审计：`<repo>\artifacts\remote-step1-20260907-prep01\final-scope-audit.json`、`delivery-evidence-index.json`。
- 原 `PREPARATION_STATUS.json` 保留为启动时历史，不覆盖；最终状态以 `PREPARATION_FINAL_STATUS.json` 为准。

本轮 Goal 只包含本地准备，必需实现、相关构建、定向验证、输入审计、B0/M1 独立交付和现场操作材料已完成。
整个 Step1 仍是 **PREPARED/PARTIAL**：现场正式迟加入基线、录屏 on/off、B0/M1 测量开销／性能等价均未取得。
本地 CPU 参考/透明转发测试不替代真实 capture lifetime/吞吐、干净 Windows VM 或 Citrix 现场证据。

后续需用户另行确认一批 15–30 分钟窗口、实际右屏拓扑与 ROI、人工双端 GUI 操作和录屏分组；具体步骤与采集文件见合同第 7 节。
指标波动、远端脚本策略、远控诊断计数器不可取得、M1 仪器开销均仍是现场限制；缺失项记录 null 和原因。
本轮不自动提交、推送、开始下一 Step 或创建后台监视器。

## 10. 本轮实际修改文件完整路径

以下共 38 项。README 和新路线包含进入任务前已有内容，本轮仅增量更新；旧统一路线索引已有改动原样保留，不列作本轮实现。

- `<repo>\apps\PixelBridgeDecoder\CMakeLists.txt`
- `<repo>\apps\PixelBridgeDecoder\decoder_application_controller.cpp`
- `<repo>\apps\PixelBridgeDecoder\decoder_application_controller.h`
- `<repo>\apps\PixelBridgeDecoder\decoder_gui.cpp`
- `<repo>\apps\PixelBridgeDecoder\main.cpp`
- `<repo>\apps\PixelBridgeEncoder\CMakeLists.txt`
- `<repo>\apps\PixelBridgeEncoder\encoder_application_controller.cpp`
- `<repo>\apps\PixelBridgeEncoder\encoder_application_controller.h`
- `<repo>\apps\PixelBridgeEncoder\encoder_gui.cpp`
- `<repo>\apps\PixelBridgeEncoder\main.cpp`
- `<repo>\apps\common\CMakeLists.txt`
- `<repo>\apps\common\application_model.h`
- `<repo>\apps\common\local_desktop_runtime.cpp`
- `<repo>\apps\common\local_desktop_runtime.h`
- `<repo>\apps\common\measurement_capture_consumer.h`
- `<repo>\apps\common\run_measurement.cpp`
- `<repo>\apps\common\run_measurement.h`
- `<repo>\apps\common\run_report.cpp`
- `<repo>\apps\common\step1_gui_evidence_qt.cpp`
- `<repo>\apps\common\step1_gui_evidence_qt.h`
- `<repo>\docs\README.md`
- `<repo>\docs\REMOTE_CHANNEL_THROUGHPUT_OPTIMIZATION_ROADMAP.md`
- `<repo>\docs\REMOTE_STEP1_EXECUTION_2026-09-08.md`
- `<repo>\docs\REMOTE_STEP1_MEASUREMENT_CONTRACT.md`
- `<repo>\libs\PBStorage\include\pbstorage\output_file.h`
- `<repo>\libs\PBStorage\src\output_file.cpp`
- `<repo>\tests\PBApplication\CMakeLists.txt`
- `<repo>\tests\PBApplication\test_gui_startup.py`
- `<repo>\tests\PBApplication\test_step1_gui_evidence.cpp`
- `<repo>\tests\PBApplication\test_step1_measurement.cpp`
- `<repo>\tests\PBApplication\unified_decoder_test_support.h`
- `<repo>\tests\tools\test_remote_step1.py`
- `<repo>\tools\CMakeLists.txt`
- `<repo>\tools\PBRemoteThroughputStep1\New-Step1InputAudit.ps1`
- `<repo>\tools\PBRemoteThroughputStep1\Start-Step1Gui.ps1`
- `<repo>\tools\PBRemoteThroughputStep1\Test-PBStep1Package.ps1`
- `<repo>\tools\PBRemoteThroughputStep1\source_audit.cpp`
- `<repo>\tools\PBRemoteThroughputStep1\step1.py`
