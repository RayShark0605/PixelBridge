# Remote throughput Step1 — 本地测量合同与现场操作

## 1. 范围与权威

本合同只覆盖 [非本机吞吐路线](REMOTE_CHANNEL_THROUGHPUT_OPTIMIZATION_ROADMAP.md) Step1 的本地准备。
本地准备的通过不等于现场基线、录屏开／关对照或整条优化路线通过。执行结果与最终包身份见
[本次执行记录](REMOTE_STEP1_EXECUTION_2026-09-08.md)。本合同不授权实屏、远程机器或自动输入。

| 候选 | 身份与用途 | 不能宣称 |
| --- | --- | --- |
| B0 | 原始 `4a36d0f1b84c94c7388a58b8249a3f1b6ec0634a` 干净独立 clone，使用原 G22 打包／验证器 | 不能把旧 `df2bcfb` EXE 或当前文档 HEAD 当作该包的二进制证据 |
| M1 | 同 base 加只读测量，完整 source inventory＋diff＋源码 ZIP＋编译入 EXE 的 fingerprint，`InstrumentedExperiment` | 不是 clean G22 release；不宣称零开销、不宣称 Citrix 已验证 |

M1 是后续同口径完成时间的测量参照；B0 保留原始行为及测量开销对照价值。现场未验证前不能认为 M1 与 B0 的性能等价。
`--build-identity` 的原严格 schema 不变；M1 身份另用 `--measurement-build-identity`。没有有效 64 位十六进制 source fingerprint 的开发构建显示 `unsealed`，不得封为正式测量包。

保持当前 `PB-Unified-SC6-V3` / `0x5042554E49534333` / layout 10。wire、FEC、Profile、layout、Golden、生产准入、8 活跃段／1 GiB Outer 总预算及原报告中的 `UniqueVisualFPS`、每帧指标全部不变。
新 CLI、C++ 可选观测字段、PBStorage 可选进程内时间点不是 wire 或持久化 schema。

## 2. 固定输入及 encoded bytes

参考文件：`C:\Users/<user>\Desktop\PixelBridgeTest\.conan.zip`。

- raw bytes：`40,517,389`。
- SHA-256：`ff3c6ae419744396c3c78950ccf18571c929b3668c88640a76b5a39e1aa61a94`。
- BLAKE3：`ce370394b8f668692b72fcbe0e2297bd313366e2206910539f4759099df6d481`。
- 初次本地生产预扫描审计：encoded bytes **`40,410,277`**，减少 `107,112 B`；不是根据扩展名推测。

| Segment | raw offset | raw bytes | codec | encoded bytes |
| --- | ---: | ---: | --- | ---: |
| 0 | 0 | 8,388,608 | Raw | 8,388,608 |
| 1 | 8,388,608 | 8,388,608 | Zstandard | 8,370,083 |
| 2 | 16,777,216 | 8,388,608 | Raw | 8,388,608 |
| 3 | 25,165,824 | 8,388,608 | Zstandard | 8,369,619 |
| 4 | 33,554,432 | 6,962,957 | Zstandard | 6,893,359 |

压缩身份：`zstd-1.5.7;enabled=1;level=3;window-log=23;max-output=16777216;framing-margin=0;content-size=1;checksum=1;workers=0`。
每段 raw／encoded BLAKE3 保存在 `PixelBridge.Step1.SourceLedger.1`，不是只记总字节数。

`PBStep1SourceAudit.exe` 调用生产 `ReadSourceFile → DescribeSource → PrepareSegmentDescription / PrepareEncodedSegment`，
不复制 payload 输出、不建持久 Session、不创建呈现／捕获对象。上限 64 MiB，逐 8 MiB 段处理，内存 O(一个段)。
M1 实际 Encoder 完成同一生产预扫描后记录自己的 ledger；现场必须与固定 ledger 逐段比较。
零字节 ledger 为 0 段、raw/encoded 都为 0，不伪造空段。

源文件在审计和发送期间维持现有只读共享／变化检查。远端运行前后各做一次审计，文件 ID、大小、修改时间、双摘要及编码 ledger 必须一致。
运行前本机参考文件的检查不替代远端这两次检查。输出文件的外部双摘要在 Receiver 已停止后做，不进入 Decoder admission。

## 3. 计时合同

新增字段位于 Unified `PixelBridge.RunReport.3.measurement`，独立 schema 为 `PixelBridge.Step1.Measurement.1`。
旧 diagnostic `.2` 不新增该字段，原 `.3` 字段不改名、不重定义。

**主计时：Decoder runtime 接受 Start 请求 → 最终路径重新打开并完成文件摘要复验。**
Start 的参数／生命周期检查通过、初始 snapshot 建立后，在创建 worker 前采样 `startAccepted`。包括随后 worker 初始化、capture 启动、控制等待、恢复尾部、整文件摘要、rename 和 final reopen；不把 UI 选文件／手动选区计入。

| milestone | 唯一观测点 |
| --- | --- |
| `startAccepted` | 上述 Start 边界；本进程 offset 为 0 |
| `captureReady` | `DecoderCaptureController::Start` 成功返回 |
| `firstVisualObservation` | 规范化 capture consumer 第一次 `Submit`；只是观察，不声称 Bootstrap／payload 已通过 |
| `firstAcceptedBootstrap` | 生产 Receiver 路径解析、Session 范围和 visual identity 检查后的首个 Bootstrap；同时记录 tag、sequence、epoch |
| `firstControlAccepted` | 生产 `ReceiveControlRecord` 成功，非恢复状态重放；在后续可能触发完整恢复之前记录 |
| `firstUsefulEquation` | 已绑定解码器的 Unique admission，或 descriptor 接纳后真正重放的 orphan；不把 orphan 缓存、重复、busy 当新有效方程 |
| `lastSegmentStored` | 最后一段通过解压／摘要、写入／flush／journal 及 Receiver stored commit 后 |
| `wholeDigestVerified` | PBStorage 对 `.part` 的整文件 hash 成功 |
| `finalRenameSucceeded` | 原同卷、不覆盖目标的 rename 成功 |
| `finalReopenVerified` | 原最终路径重新打开／hash 成功；主计时终点 |
| `terminal` | worker 正常收尾及 capture/demod 资源退休之后；异常／清理失败不能成为合格测速样本 |
| `preparationComplete` | Encoder 同一生产预扫描与 Session 准备结束；仅辅助观测 |

时钟采用每台机器各自 `steady_clock`，原点之后的**整数纳秒**。FrameSequence、SessionTag 等 64 位身份在 measurement/trace 中用十进制字符串，避免 JSON double 舍入。
capture 原始 `monotonic100ns` 独立保留为带单位字段，不与 `steady_clock` 或另一台机器做减法。

capture 可以在 Start 返回 ready 前同步送帧，所以 `firstVisualObservation < captureReady` 合法。
校验真实的偏序：观察→Bootstrap→Control→摘要→rename→reopen→terminal，ready→reopen，有 payload 时 useful→last stored→摘要。
不强造一条所有阶段严格递增的时间链。

主指标在上述完整条件及干净 terminal 下计算：

```text
elapsedNanoseconds = finalReopenVerified - startAccepted
VerifiedRawGoodput B/s = originalFileBytes * 1e9 / elapsedNanoseconds
VerifiedEncodedGoodput B/s = verifiedEncodedSegmentBytes * 1e9 / elapsedNanoseconds
```

辅助指标 `captureReadyToReopenNanoseconds` 单列，不能替代主时间。外部双摘要不加入时间分母，但必须通过才能取得正式样本资格。
未完成／不完整编码字节覆盖／resume／计时缺失、超限或顺序错误／测量 IO 失败／capture 清理警告均为 null＋具体原因，不记为 0，不事后剔除等待或尾部。
零字节的 useful/last stored 为 NotApplicable；0 B/s 与测量不可用不同。
`receiverTimingEligible=true` 只是接收端本地计时资格，`formalSampleEligibility` 仍要求事后跨端身份和双摘要核对。

## 4. 迟加入身份的精确定义

Encoder 在 **Submit 成功之后、`builder.Advance()` 和 `frameSequence++` 之前**记录：
SessionTag、实际 FrameSequence、Carousel pass、segment ordinal、cycle position、发送端本地提交 offset。
失败 Build／失败 Submit／重复 Present 不产生新 trace identity。

主场景保持 Encoder 已进入后续 Carousel，随后人工开始 Decoder。双端停止后：

1. 验证各端 `entry.json`、run seal、最终报告、实际启动 EXE hash 与 M1 package manifest；
2. 用 Receiver 第一个有效 Bootstrap 的 `(SessionTag, FrameSequence)` 精确查找 Sender trace；
3. 记录 `firstObservedSenderPhase`，迟加入主样本要求该帧 `carouselPass > 0`；
4. `actualClickSenderPhase=null`；不插值序号，不跨机相减时间戳，不把最后一份快照中的“下一帧”当已提交帧。

找不到精确匹配、trace 截断、多个 Session、重复／倒退序号、跨端构建不一致，都不合格。
这是**首次观察到的发送阶段**，不是无法证明的远端点击瞬间阶段。冷启动单独标记 `cold-start`，不混入迟加入组。

## 5. 资源、默认行为及错误路径

- RunMeasurementRecorder 一实例只供一次 run，复用即失败；没有 worker/capture 文件 IO。
- Sender→GUI 使用单生产者／单消费者 4,096 记录环；每 run 最多 131,072 条提交身份。
- 测量新增缓冲预算不超过 16 MiB；recorder 有 `<1 MiB` 静态断言，报告／单条 JSON 最大 64 KiB；无纹理持有或额外 GPU stage。
- GUI 快照最多每秒一次；环可每 100 ms 有界排空。单 run 证据 64 MiB，预留 1 MiB 给 final／seal；时长最多 1,800 s。
- 大于 64 MiB 的文件不属于 Step1 测量范围；不改变产品允许的文件上限或恢复路径。
- 溢出／IO 失败只使测量证据无效；不阻塞 payload worker，不改变接纳，不自动停止普通 GUI 发送。
- `--gui-measurement --evidence-root <新本地目录>` 显式启用；默认 GUI 不自动导出、不变更原 QSettings。
- 实验 entry 使用独立 `settings.ini`，每次 Start 新建 `run-UUID/sessions` 和 `run-UUID/output`，不读取既有恢复索引。
- GUI 仍由人点击开始、选择屏幕／ROI及停止。测量启动器默认只验证并显示命令，必须显式 `-AllowGuiLaunch` 才打开交互 GUI。
- 停止后自动写 `final.json`、`source-ledger.json`（Sender）、`evidence-seal.json`。关闭 GUI 时 join 既有停止流程后再封存，避免 terminal snapshot 先于 cleanup 的竞态。
- 证据根只允许新建的固定本地磁盘目录，无 reparse 祖先；文件 create-only。冲突 final 日志、旧包、旧构建树和失败资料不覆盖、不删除。

## 6. 本地构建、最小验证及封存

工作目录 `<repo>`；Python 使用 `<python>`，不是系统旧 Python。
唯一开发证据根 `<repo>\artifacts\remote-step1-20260907-prep01`。
B0 构建树 `<repo>\build-remote-step1-b0-prep01`；M1 构建树 `<repo>\build-remote-step1-m1-prep01`。
新一轮复现必须换新的后缀，不能直接覆盖这些历史目录。

本次构建配置为 VS2022 x64 Release、C++20、warnings-as-errors、Qt `<qt>6.10.1/6.10.1/msvc2022_64`、
vcpkg `<vcpkg-root>/scripts/buildsystems/vcpkg.cmake`。所有 native Gate 与 process recovery harness 保持 OFF。

定向命令（需先完成对应构建；下面不是现场命令）：

```powershell
$repo = '<repo>'
$build = '<repo>\build-remote-step1-m1-prep01'
& 'C:\Program Files\CMake\bin\cmake.exe' --build $build --config Release --target PixelBridgeEncoder PixelBridgeDecoder PBStep1SourceAudit PBApplicationTests PBStep1GuiEvidenceTests PBStorageTests PBUnifiedSenderSchedulerTests PBGuiConsoleProbe PBConsoleParentProbe --parallel 4
& "$build\tests\PBApplication\Release\PBApplicationTests.exe" '[step1],[application][report]' --reporter compact
& "$build\tests\PBApplication\Release\PBStep1GuiEvidenceTests.exe" --reporter compact
& "$build\tests\PBStorage\Release\PBStorageTests.exe" --reporter compact
& "$build\tests\PBApplication\Release\PBUnifiedSenderSchedulerTests.exe" --reporter compact
& '<python>' -B -X utf8 "$repo\tests\tools\test_remote_step1.py" --evidence-directory '<新的绝对证据目录>'
```

另外运行已存在的 `tests/PBApplication/test_gui_startup.py`，B0 的 `tests/tools/verify_unified_package_startup.py`，
以及 M1 `step1.py startup`。它们使用非显示探针／offscreen，不能解释为实际右屏验证或干净 Windows VM 认证。
本地负例覆盖清单／ZIP／路径／身份篡改、计时缺失／单位／偏序、缓存 orphan 与真实有用方程区别、记录上限、
默认测量关闭、相同输入测量开／关恢复一致、零字节、RAW／zstd／短尾、输出冲突、摘要失败、post-rename resume 不伪造旧时间、清理失败不具测速资格。

封存顺序：完成实现和窄测 → `step1.py freeze-source` → 以该 fingerprint 重新配置／构建双端 → 重跑受影响窄测 →
`step1.py package` → Python 完整 package/ZIP verifier＋PowerShell 可迁移目录 verifier＋全新解压 offscreen startup。
原 G22 verifier 不新增 dirty-source 豁免。M1 内包含 full source ZIP、tracked diff、source inventory、CMakeCache、vcpkg status、
独立实验 manifest/seal、双端 EXE、逐项 DLL hash、许可证和标明仅用于依赖溯源的原 SPDX。
后续只追加执行记录的文档改动不冒充已编译源码；精确编译身份始终以包内 source seal 为准。

## 7. 后续现场操作包（当前未授权运行）

### 7.1 开始前要由用户确认

- 可用远程窗口及本批 15–30 分钟预算；超额顺延，不能开启无人值守长测。
- 保护左屏和实际右屏拓扑、物理像素尺寸、DPI、ROI、Encoder 所处屏幕已重新核对；不复用历史 DISPLAY2 坐标假设。
- 人工移动／启动远程 GUI、人工选择 Decoder ROI、人工控制录屏开／关；工具不抢焦点、移动鼠标、发按键或改变显示设置。
- Citrix／网络配置不变；硬件／编码模式／provider counter 不可取得时记录 null 和原因，不凭品牌补值。
- 将完整 M1 包及独立 seal 按包内结构保留；传输输入源文件只放 Sender，不能传给生产 Decoder。包内项目源码 ZIP 仅用于构建溯源，不是传输输入文件。
- Receiver 侧事后审计需要 Python 3.12＋`blake3`；Sender 的前后审计也可使用包内 PowerShell 5.1 wrapper，无需安装 Python。

预期一条主文件正式样本需等待 Sender 实际进入后续 Carousel，再给接收恢复及停后核对留出数分钟。
这不是基于手机 210/227 s 的性能保证；不要用固定 sleep 代替 phase 证据。30 分钟证据上限不是自动停止发送功能。

### 7.2 运行前：两端包验证，Sender 输入前审计

以下变量必须使用执行记录／`START_HERE.md` 中实际封存的路径与 manifest hash；不提供通配符找“最新包”。
包移动到远端后，只修改操作者本地变量中的路径，不修改包内容。

```powershell
$package = '<完整 M1 解压目录的绝对路径>'
$seal = '<独立 M1 seal.json 的绝对路径>'
$manifest = '<START_HERE 中固定的 manifest SHA256>'
& "$package\Test-PBStep1Package.ps1" -PackageDirectory $package -SealPath $seal -ExpectedManifestSha256 $manifest

# 仅 Sender：文件来自本机预先准备的固定输入；audit 路径必须全新。
& "$package\New-Step1InputAudit.ps1" -PackageDirectory $package -SealPath $seal -ExpectedManifestSha256 $manifest -SourcePath '<Sender源文件绝对路径>' -OutputPath '<新的source-before.json绝对路径>'
```

### 7.3 迟加入主样本

1. 设置本批 operator record：候选、configured Hz、`startupScenario=late-join`、`recording=on/off`、批次／顺序、右屏拓扑与人工确认。首次仅做固定条件基线，不搜索 Hz／调制／FEC。
2. Sender 由人运行下列命令并手动选择固定源文件／帧率；把窗口放在获准测试屏，手动开始。
3. Sender 已持续循环后，人启动 Decoder GUI、选择正确屏幕和 ROI、点击开始接收。不能为了更快改成 Receiver-first。
4. Decoder 完整恢复自动结束接收；人确认显示完成后，在 Sender 本地按既有 GUI 停止动作。不会把 Decoder 完成状态送给 Sender。
5. 等待两端 `evidence-seal.json` 落盘且为 complete；不要关闭后立刻删除 Session／日志。

```powershell
# 两端分别运行，各用新 EvidenceRoot；去掉 -AllowGuiLaunch 只校验和预览，不打开窗口。
& "$package\Start-Step1Gui.ps1" -Role Encoder -PackageDirectory $package -SealPath $seal -ExpectedManifestSha256 $manifest -EvidenceRoot '<Sender新的entry绝对路径>' -AllowGuiLaunch
& "$package\Start-Step1Gui.ps1" -Role Decoder -PackageDirectory $package -SealPath $seal -ExpectedManifestSha256 $manifest -EvidenceRoot '<Receiver新的entry绝对路径>' -AllowGuiLaunch
```

录屏开／关另分组，其他条件相同、交错运行；B0 原始包对照与 M1 仪器开销检查另记，不能混成同一统计口径。
冷启动另建 `cold-start` 批次，不能代替迟加入正式基线。至少三组的正式对照若一晚放不下，分批安排，不降低必需样本数。

### 7.4 双端停止后核对

Sender 再运行一次 `New-Step1InputAudit.ps1`，写入全新的 `source-after.json`。
此后人工回传 Sender 的 `entry.json`、`run-UUID` 完整证据目录及 before/after audit；这是事后报告交换，不是生产 payload/ACK 通路。
必须保留 `entry.json` 与 `run-UUID` 的父子关系。Receiver 的实际发布文件留在报告中的原最终路径，避免把源文件当发布文件审计。

```powershell
& '<python>' -B -X utf8 '<M1包绝对路径>\step1.py' correlate `
  --package-result '<本机已封存package-result.json>' `
  --sender-run '<回传的Sender entry\run-UUID>' --receiver-run '<Receiver entry\run-UUID>' `
  --input-audit '<repo>\artifacts\remote-step1-20260907-prep01\input\input-audit-final.json' `
  --sender-source-before '<回传的source-before.json>' --sender-source-after '<回传的source-after.json>' `
  --operator-record '<本次operator.json>' --published-file '<Receiver报告中的原finalPath>' `
  --output '<新的post-stop-correlation.json>'
```

operator record 必需字段示例（现场由人填写，不自动确认）：

```json
{
  "schema": "PixelBridge.Step1.OperatorRecord.1",
  "startupScenario": "late-join",
  "configuredHz": 15,
  "recording": "off",
  "manualOperation": true,
  "topologyRechecked": false,
  "protectedScreenUntouched": false,
  "batchId": "待填写",
  "runOrder": null,
  "displayTopologyEvidence": null,
  "citrixCodecMode": null,
  "citrixCodecUnavailableReason": "未采集"
}
```

初始 false/null 不能生成合格样本；只能由实际操作和证据更新。脚本不会操作任何显示器、网络、剪贴板或远程界面。

### 7.5 采集文件及现场门

- 完整候选 package-result／manifest／seal，双端实际启动 `entry.json`；
- `events.jsonl`、`final.json`、Sender `source-ledger.json`、`evidence-seal.json`；
- 固定本机 input audit、Sender source-before/source-after 及各自 ledger；
- Receiver 原最终路径的事后双摘要与完整 correlation；
- operator record、重核的拓扑证据、录屏状态；开录屏时附原始录像路径／hash，关录屏组明确无录像；
- 所有失败／超时／资源计数与未运行项，不能从结果表移除。

现场基线、录屏开／关、B0/M1 测量开销对照尚未完成时，整个 Step1 只能是 **PREPARED/PARTIAL**。
本地完成后停止；不得自动开始 Step2 的生产录像回放、Step3 信道平台、节奏调优、新布局、新 FEC 或长文件窗口测试。

## 8. 本次封存后的操作补充

正式 M1 的精确源码身份见 source seal；本节及执行记录的收尾文字是封包后的文档补充，不冒充已编译代码。
本机 Windows PowerShell 5.1 的默认策略为 Restricted，测试宿主继承的 PowerShell 7 模块路径也不适用于它。
本地验证仅给测试子进程使用 `-ExecutionPolicy RemoteSigned` 和原生 WindowsPowerShell 模块路径；未执行 `Set-ExecutionPolicy`，未修改用户／机器持久设置。
现场必须先确认当地允许的脚本运行方式；若有组策略限制，停在该前置条件交由操作者处理，启动器不会改变策略。
完整精确命令、策略／模块路径失败原始记录及成功复验见本次执行记录与 `field-kit/START_HERE.md`。
