# RemoteVisual Step 21：provider-generic 正式矩阵、失败归因与逐 run 封存

## 1. 状态、目标与完成真值

Step 21 的状态保持 `MANUAL-GATE`，直到预先冻结的验收范围被彼此独立的真实远控 run 完整覆盖并通过最终 verifier。canonical MatrixSpec 仍固定 41 个 cell；本次经用户明确授权的 `Dual2560x1440SingleExperimentMonitor` hardware scope 在任何正式 run 前仅按 Computer A 封存 monitor catalog 的单屏 ROI containment 机械选出 31 个可执行 cell，并明确排除 10 个 1.5× cell。工具、production Replay 绑定、scope 和契约测试通过只代表 readiness，不能替代 Computer B 可见像素 → 任意远控桌面/视频链路 → Computer A 实际 WGC/DXGI 捕获 → production demod/Receiver 的现场证据；31-cell PASS 也不声称 1.5× 已覆盖。

本步回答三个问题：

1. LF4 在 1/2/5 Hz、质量优先/自动/受限模式、且当前硬件能完整容纳的约 0.75/1.0/1.259 scale 下的可用域和 blocker 是什么；canonical 1.5× 列作为明确未覆盖限制保留；
2. WGC 主矩阵的结论能否由代表性的 DXGI 复核支持；
3. 在相同 mode/backend/FPS/strict-1:1 条件下，Direct、Shape、LF4 的成功/失败和权威 Decoder/Receiver 指标如何比较。

这里的 provider、可见产品模式和 `modeClass` 只属于 `NonDecodingOperatorMetadata`：它们用于把彼此独立的现场 run 分层、避免跨条件合并指标，并进入 report/Replay 证据身份。它们不是 Decoder 的信道输入，不选择 locator/demod 阈值，不改变 FEC、Receiver、WholeFileDigest 或 publish 分支，也不进入 wire format。生产 Decoder 仍只根据所选 PixelBridge profile、capture backend、实际 ROI/capture records 以及从屏幕捕获到的像素作出解码判断；同一批像素在 provider/mode 标签变化后必须得到相同的解码语义。

以下内容不能单独关闭 Step 21：截图主观“看起来更清楚”、Presenter/Encoder 正常退出、Replay 文件存在、某个 Transport block 被接受、Sender cycle 位置、离线模拟、WARP、单机闭环、单屏诊断，或把多个 run 的计数/时长/分母相加后得到的结果。

## 2. 预先冻结的 41-cell MatrixSpec 与 31-cell HardwareScope

先运行：

```powershell
pwsh -NoProfile -File .\tools\PBRemoteVisualEvidence\New-PBRemoteVisualStep21MatrixSpec.ps1 `
  -ComparisonModeClass QualityPriority `
  -ComparisonLogicalFps 5 `
  -OutputPath <new-matrix-root>\step21-matrix-spec.json
```

保存命令返回的 `matrixId`、`sha256` 和 MatrixSpec 文件。它必须在正式 run 前创建，且之后不可覆盖或编辑。`ComparisonModeClass` 与 `ComparisonLogicalFps` 只选择 Direct/Shape/LF4 的共同对照条件；它们不改变 LF4 主矩阵。

MatrixSpec 固定以下 41 个 cell：

- **LF4 + WGC 主矩阵：36 个**：3 个 mode class × 4 个 scale target × 3 个 logical FPS 的完整笛卡尔积；
- **LF4 + DXGI 代表性复核：3 个**：
  - `QualityPriority / 0.750 / 1 Hz`；
  - `Automatic / 1.259 / 2 Hz`；
  - `Restricted / 1.500 / 5 Hz`；
- **Profile baseline：2 个**：Direct 与 Shape 各一个，均为 WGC、strict 1:1，并使用命令指定的共同 mode class/FPS。

每个 cell 只允许一个独立 run。失败 run 不能删除后用成功 run 覆盖；如果需要第二次完整矩阵，应生成新的 MatrixSpec/matrixId，并保留第一份矩阵。该规则防止事后挑选 provider 状态最好的一次结果。

本次 canonical MatrixSpec 创建后、任何正式 run 开始前，使用 A 端真实 `PixelBridge.MonitorCatalog.1` 创建不可覆盖的 hardware scope：

```powershell
pwsh -NoProfile -File .\tools\PBRemoteVisualEvidence\New-PBRemoteVisualStep21HardwareScope.ps1 `
  -MatrixSpecPath <new-matrix-root>\step21-matrix-spec.json `
  -ExpectedMatrixSpecSha256 <frozen-spec-sha256> `
  -ComputerAMonitorCatalogPath <new-matrix-root>\computer-a-monitor-catalog.json `
  -ExpectedComputerAMonitorCatalogSha256 <catalog-sha256> `
  -ExperimentMonitorDeviceName \\.\DISPLAY2 `
  -OutputPath <new-matrix-root>\step21-hardware-scope.json
```

`PixelBridge.RemoteVisualStep21HardwareScope.1` 只接受两块不重叠、未旋转的 2560×1440 Computer A 显示器，且指定 ExperimentMonitor 必须恰好来自该 catalog。它按“目标 LF4 画布能否完整落入单块 ExperimentMonitor”而不是按 Decoder capture ROI 的宽高比，从 canonical 41-cell MatrixSpec 得到 31 个 included cell：27 个 LF4/WGC（3 mode × 3 可容纳 scale × 3 FPS）、2 个 LF4/DXGI（0.750/1 Hz 与 1.259/2 Hz）和 2 个 Direct/Shape baseline；另把 9 个 LF4/WGC 1.5× 与 1 个 LF4/DXGI 1.5× 封存为 `RequiredRoiExceedsSingleExperimentMonitor`。导入器会重新读取父 MatrixSpec 与 monitor catalog、重算 31/10 partition，拒绝人工删项、换项、移动身份或事后修改 catalog。excluded cell 不需要 run，但也永远不能计入覆盖。

在 HardwareScope 冻结后、任何 cell 开始前，再生成一份只读的操作者账本：

```powershell
pwsh -NoProfile -File .\tools\PBRemoteVisualEvidence\New-PBRemoteVisualStep21RunLedger.ps1 `
  -MatrixSpecPath <new-matrix-root>\step21-matrix-spec.json `
  -ExpectedMatrixSpecSha256 <frozen-spec-sha256> `
  -HardwareScopePath <new-matrix-root>\step21-hardware-scope.json `
  -ExpectedHardwareScopeSha256 <frozen-scope-sha256> `
  -OutputDirectory <new-matrix-root>\run-ledger
```

`PixelBridge.RemoteVisualStep21RunLedger.1` 和对应 CSV 逐项保存 31 个 included cell 的固定 ordinal、Profile、backend、mode、scale target、FPS、最小目标画布、`GeometryMode`、Decoder ROI 参数和独立目录名。LF4 的 Decoder ROI 固定为整块 A 端 ExperimentMonitor，purpose 明确为 `LocatorSearchNeighborhood`；这样 0.75/1.0/1.259 三种目标都可在同一个有界任意宽高比搜索区域内平移或 letterbox，ROI 本身不提供尺度证据。Direct/Shape 则使用该显示器中央的 exact 1920×1080 `ExactProfileCanvas`。导入器重算每一行并拒绝顺序、目标、ROI、目录名、状态或父 identity 篡改；seal 绑定 MatrixSpec、HardwareScope、A 端 catalog、JSON 与 CSV。

新账本的每行初始状态只能是 `PENDING`，`runId` 必须为 `null`，顶层只能是 `NOT_EXECUTED`、`executedCellCount=0`、`formalStep21Accepted=false`。RunId 仍然必须等到该 cell 真正开始时独立生成；账本不会预生成或共享 deployment/UI/plan/evidence，也不能填充任何矩阵覆盖率。Computer B monitor catalog 和逐 run provider UI evidence 仍是正式计划的未满足前提。完整账本目录是 create-only；seal 是最后的完成标记，已有目录不可覆盖或复用。

取得 Computer B 的 packaged `--list-monitors` 输出后，先把两端 monitor role 绑定为 readiness-only EndpointScope：

```powershell
pwsh -NoProfile -File .\tools\PBRemoteVisualEvidence\New-PBRemoteVisualStep21EndpointScope.ps1 `
  -RunLedgerPath <new-matrix-root>\run-ledger\step21-run-ledger.json `
  -ExpectedRunLedgerSha256 <ledger-sha256> `
  -RunLedgerSealPath <new-matrix-root>\run-ledger\step21-run-ledger.seal.json `
  -ExpectedRunLedgerSealSha256 <ledger-seal-sha256> `
  -ComputerBMonitorCatalogPath <computer-b-monitor-catalog.json> `
  -ExpectedComputerBMonitorCatalogSha256 <computer-b-catalog-sha256> `
  -ComputerBProtectedMonitorDeviceName <B-protected> `
  -ComputerBExperimentMonitorDeviceName <B-experiment> `
  -OutputDirectory <new-matrix-root>\endpoint-scope
```

`PixelBridge.RemoteVisualStep21EndpointScope.1` 要求 B catalog 恰有两块不重叠、未旋转的 exact 1920×1080 monitor，并拒绝 Duplicate/重叠、同名/同角色、错误尺寸、错误 hash 或非整数 rectangle。A 端角色来自已经封存的 HardwareScope：其余那块 monitor 自动成为 ProtectedMonitor；B 端角色必须由操作者显式选择。Encoder Data Window 机械等于 B ExperimentMonitor 的完整 physical rect，因此允许合法的负数桌面 origin，而不会把 `(0,0)` 写死。EndpointScope seal 递归绑定 RunLedger seal（含 CSV）、MatrixSpec、HardwareScope、A/B catalog 与 topology。

EndpointScope 仍只能是 `READINESS_ONLY`：它证明静态 catalog 可形成符合要求的双端 Extended Desktop 布局，但不是运行开始时的 topology。每个 cell 的 endpoint wrapper 仍必须重新调用 packaged Decoder `--list-monitors`，并让 live catalog 与 plan 中的完整 device/rect/DPI/refresh/rotation/LUID/primary contract 一致；provider 模式也仍须新的 UI-visible evidence。EndpointScope 保持 `runIdsAllocated=0`、`executedCellCount=0`、`formalStep21Accepted=false`，不能拿来填一个 cell。

### 2.1 Computer B 单次解压交付包

为了让 Computer B 不再单独复制 BAT、也不再需要先解压外层文件后继续寻找第二层 `RuntimePackage`，可在当前 source/package identity 完全一致时创建一个单次解压包：

```powershell
$kit = pwsh -NoProfile -File .\tools\PBRemoteVisualEvidence\New-PBRemoteVisualStep21ComputerBKit.ps1 `
  -PackageDirectory <verified-current-head-both-package> `
  -PackageSealPath <both-package-seal.json> `
  -PackageArchivePath <both-package.zip> `
  -ExpectedPackageManifestSha256 <package-manifest-sha256> `
  -SourceSetDirectory <sealed-source-set> `
  -SourceSetSealPath <source-set-seal.json> `
  -ExpectedSourceManifestSha256 <source-manifest-sha256> `
  -ExpectedHeadCommit <40-hex-current-head> `
  -ComputerBProtectedMonitorDeviceName <B-protected> `
  -ComputerBExperimentMonitorDeviceName <B-experiment> `
  -OutputRoot <new-kit-output-root> | ConvertFrom-Json

pwsh -NoProfile -File .\tools\PBRemoteVisualEvidence\Test-PBRemoteVisualStep21ComputerBKit.ps1 `
  -KitDirectory $kit.kitDirectory `
  -KitSealPath $kit.sealPath `
  -ArchivePath $kit.archivePath `
  -ExpectedManifestSha256 $kit.manifestSha256
```

生成器只有在以下条件全部满足后才发布目录、外层 ZIP 和外部 seal：Both-role package 的目录/seal/原始 ZIP 已由权威 package verifier 验证，package HEAD/tree/tested-source fingerprint 与当前 checkout 一致，source set 及其 1 MiB CSPRNG 文件已由权威 source verifier 验证，所有输出与输入目录分离，且展开目录和外层 ZIP 的每一个不可变文件都重新通过清单验证。文件数、单文件、总 payload、manifest、seal 和 ZIP 都有上限；额外文件、重复/不安全路径、reparse point、哈希漂移、archive tamper 和 launcher 语义漂移均 fail closed。只有 `Working/` 是明确可变区。

把 `$kit.archivePath` 复制到 Computer B 后只需解压一次。根目录内有：

1. `VERIFY-COMPUTER-B-KIT.bat`：调用内嵌 PowerShell 7 verifier，递归验证展开的 production package、source set 和全部不可变文件；
2. `Capture-ComputerBMonitorCatalog.bat`：只运行 packaged Decoder `--list-monitors`，create-only 写入 `Working\computer-b-monitor-catalog.json`；已有 final 或 `.partial` 都会拒绝，不会静默覆盖/删除；
3. `Start-PBRemoteVisualExperimentMonitorDemo.bat`：先确认 exact packaged `Encoder\PixelBridgeEncoder.exe` 存在，并用 `%SystemRoot%\System32\certutil.exe` 对 sealed `random-1MiB.bin` 做 SHA-256 校验，然后以 2 Hz/12 Control repetitions 在已选 B ExperimentMonitor 上执行 `remote-lf4 --single-monitor-fullscreen --manual-stop --loop`。它持续循环，直到 A 端已经完成 Receiver、WholeFileDigest、safe publish 和 external byte-exact 比较后，由操作者按 Q 或 Enter 正常停止。

第三个 BAT 只用于快速重现用户要求的文件恢复演示，不依赖视频播放器或远控窗口的品牌、宽高比、位置或缩放策略；A 端仍只从实际捕获像素中的 Locator 得到 origin/X/Y scale。它不带 formal `--protected-monitor/--experiment-monitor` 双屏参数，也没有 deployment/UI evidence/PilotPlan.3，因此明确不能填 RunLedger。正式 cell 仍必须先在 A 端创建独立 RunId、environment/deployment/UI evidence 和 plan，先启动 Decoder，再在 B 端调用 `Invoke-PBRemoteVisualPilotEncoder.ps1`。

## 3. 每个 cell 的冻结输入与几何

每个 included cell 都必须重新生成一个 OS-CSPRNG RunId，并生成与该 RunId 绑定的 metadata、双端 environment、deployment manifest、remote UI evidence 和 `PilotPlan.3`。package manifest 与 1 MiB RAW/OFF source identity 必须在 31 个 run 中完全相同；其余 run-bound artifact 不得复用。历史 `PilotPlan.2` 仍可只读导入和验证，但不能再生成当前 `MatrixRunRecord.2`，以免把旧的 ROI-derived scale 误当作现场几何真值。

LF4 scale target 对应的**最小目标画布包围尺寸**为：

| target | 最小目标画布尺寸 | 目标 X/Y scale |
|---|---:|---:|
| 0.750 | 1440 × 810 | 0.75 / 0.75 |
| 1.000 | 1920 × 1080 | 1.0 / 1.0 |
| 1.259 | 2417 × 1360 | 1.258854… / 1.259259… |
| 1.500 | 2880 × 1620 | 1.5 / 1.5 |

这里的尺寸**不是要求外层远控画面、播放器窗口或 Decoder ROI 必须是 16:9**。Decoder ROI 只是 locator 扫描的物理像素搜索邻域：LF4 可使用 960..3840 × 540..2160 范围内、能容纳该目标画布、位于同一 ExperimentMonitor 内的任意宽高比矩形；画面可以在其中平移、缩放或带 letterbox。计划保存 `captureRoiWidth/Height` 与 `captureRoiScaleX/Y` 只用于资源和边界审计，并显式声明 `scaleAuthority=AcceptedBootstrapLocatorPixels`，不得把这些 ROI 比值写入矩阵尺度结论。

成功 run 的 Matrix verifier 从 live 与 offline `RunReport.2.observedLocatorGeometry` 读取实际接受 Bootstrap locator 的 origin、X/Y scale、marker residual、完整 min/max 和样本数。只有 last/min/max scale 均映射到预冻结 target、与 target 的 X/Y 偏差均不超过 0.015、且 X/Y 各向异性不超过 0.015，才接受该成功记录；零样本成功记录必定失败。失败记录允许零 accepted Locator 样本，但必须保留同一权威对象的 `null` 语义并由 Replay/report 支持失败分类。Direct/Shape 仍只接受 exact 1920×1080、`Strict1To1` 和实际 1.0/1.0；任何 scaled Direct/Shape 计划都会在运行前失败，绝不 silent resample。

每个 Step 21 plan 的创建形式为：

```powershell
pwsh -NoProfile -File .\tools\PBRemoteVisualEvidence\New-PBRemoteVisualPilotPlan.ps1 `
  -DeploymentManifestPath <this-run>\deployment-manifest.json `
  -UiEvidencePath <this-run>\remote-ui-evidence.json `
  -PythonPath D:\Python3.12.9\python.exe `
  -OutputPath <this-run>\pilot-plan.json `
  -ProfileToken remote-lf4 `
  -CaptureBackend wgc `
  -MatrixModeClass QualityPriority `
  -MatrixScaleTarget 0.750 `
  -LogicalFps 1 `
  -GeometryMode LocatorScaled `
  -EncoderProtectedMonitorDeviceName <B-protected> `
  -EncoderExperimentMonitorDeviceName <B-experiment> `
  -EncoderOriginX <B-data-left> -EncoderOriginY <B-data-top> `
  -DecoderProtectedMonitorDeviceName <A-protected> `
  -DecoderExperimentMonitorDeviceName <A-experiment> `
  -DecoderRoiLeft <left> -DecoderRoiTop <top> `
  -DecoderRoiRight <right> -DecoderRoiBottom <bottom>
```

`ProfileToken`、`CaptureBackend`、`MatrixModeClass`、`MatrixScaleTarget`、`LogicalFps` 和 target geometry 必须与 MatrixSpec cell 完全一致。`GeometryMode` 现在约束预期 locator target：target `1.000` 使用 `Strict1To1`，其余 LF4 target 使用 `LocatorScaled`；它不再声称 capture ROI 本身就是编码画布。provider 品牌或实现策略不进入 profile 阈值；真实模式必须来自该 run 的 UI-visible screenshot/capture record，并且只作为非解码操作者元数据用于矩阵分组。当前 UI 证据没有 latency claim，所以 `observedLatencyMilliseconds` 必须保持 `null`；chroma 只有在 UI 中确实可见并被 `visibleFields` 封存时才允许不是 `Unknown`。

## 4. 生产 Replay 已对三 profile 使用同一边界

RemoteVisual live/offline production Replay 现支持 `direct`、`shape`、`remote-lf4` 三个 public profile token：

- live Decoder 的 primary D3D11 demod 不受 evidence sampler 节流；
- Replay 只采样实际捕获 ROI pixels，保持 16 GiB、最大帧数、队列 high-water 和零 dropped-frame 硬门禁；
- offline Decoder 对 Replay 中每个 capture frame 运行 production demod 和 Receiver；
- Direct/Shape offline Replay 仍执行 exact 1920×1080/1:1 production geometry 检查；
- LF4 offline Replay 仍执行 locator、0.5..2.0 scale、temporal admission、FEC、Transport、Receiver 和 WholeFileDigest 语义；
- diagnostic capture-only 仍然是独立的非 Receiver 入口，不能作为 MatrixRunRecord。

Direct/Shape Encoder CLI 既有公共契约不接受 LF4 专用的 monitor-safety 参数。Step 21 对它们采用 plan/environment containment + 启动前 packaged Decoder live-monitor preflight；Decoder 对三个 profile 都继续执行 production monitor preflight/periodic revalidation。LF4 Encoder 继续执行 production monitor-safety preflight/periodic revalidation。该差异会在 plan 的 `runtimeEnforcement` 中显式记录，不能伪装为相同的运行时实现。

## 5. 单个真实 run 的执行顺序

每个 cell 都沿用 Step 20 的双端顺序，但 profile/backend 来自该 cell：

1. 两端验证同一个 Both-role package、source set、metadata 和 plan SHA-256；
2. A 端启动 `Invoke-PBRemoteVisualPilotDecoder.ps1 -Mode Live`；
3. 看到 A 端 ready 后，在冻结的 30 秒 warmup 内启动 B 端 `Invoke-PBRemoteVisualPilotEncoder.ps1`；
4. 成功时，A 端完成 WholeFileDigest/safe publish/Replay footer 后，等待计划要求的跨机证明窗口，再人工在 B 端按 Enter/Q；
5. 失败时，等待 A 端 frozen timeout/no-progress 终止并保留 Replay，然后仍人工正常停止 B，不能杀进程后只留半份证据；
6. A 端用 exact live Replay 运行 `Invoke-PBRemoteVisualPilotDecoder.ps1 -Mode Offline`；
7. 成功 run 使用 `Test-PBRemoteVisualPilotEvidence.ps1`；失败 run 使用下节的分类 verifier。

每次 endpoint wrapper 启动前都会调用 verified package 内 Decoder 的 `--list-monitors`，重新核对 device name、physical rect、DPI、refresh、rotation、adapter LUID 和 primary 标志。任何 topology/identity 漂移都在像素运行前 fail closed；这种 preflight-blocked 目录不是已执行 matrix cell，不能拿来填覆盖率。

## 6. 失败 run 的五类归因和封存

失败 run 不能只写一句人工判断。运行：

```powershell
pwsh -NoProfile -File .\tools\PBRemoteVisualEvidence\Test-PBRemoteVisualFieldFailureEvidence.ps1 `
  -PlanPath <this-run>\pilot-plan.json `
  -ExpectedPlanSha256 <exact-plan-sha256> `
  -EncoderEvidenceDirectory <this-run>\encoder `
  -LiveDecoderEvidenceDirectory <this-run>\decoder-live `
  -OfflineDecoderEvidenceDirectory <this-run>\decoder-offline `
  -SourcePath <source-set>\random-1MiB.bin `
  -PythonPath D:\Python3.12.9\python.exe `
  -ReplayInspectorPath <tested-build>\tools\Release\PBRemoteVisualReplayInspector.exe `
  -FailureClassification signal `
  -OutputDirectory <this-run>\classified-failure
```

允许的分类及其最低支持证据为：

- `geometry`：Replay 中出现 `ScaleOutOfRange`、`AlignmentOutOfRange`、`FrameOutOfBounds`、`InvalidGeometry` 或 `AmbiguousGeometry`；
- `signal`：Bootstrap marker/contrast/FEC/CRC/timing erasure、LF4 pilot erasure、pixel read failure，或 Transport FEC/CRC/identity rejection；
- `temporal`：stale/freshness mismatch/erasure、duplicate/reorder/gap/skipped sequence 等 production temporal evidence；
- `metric`：unreliable symbol、rejected metric frame、soft-metric 下的 FEC/CRC/identity rejection；
- `scheduler`：零 capture、capture/readback/result drop、capture/visual stall，或 frozen timeout/no-progress window 在 Receiver convergence 前结束。

不同类别可能由同一失败同时支持；操作者选择主分类，但 verifier 会保存五类全部支持计数和逐帧说明，并拒绝一个没有任何对应证据的选择。它还必须证明：

- Encoder 正常人工 Stop，source immutable，journal 完成；
- live/offline Decoder 应用进程均正常退出并完成 report/journal/Replay 封存，但均未 `Completed`、未 WholeFileDigest PASS、未 publish final file；应用崩溃或 wrapper/preflight 故障不能伪装成远控信道失败；
- `falseAcceptedCodewords` 为 0 或 receiver-only unavailable，Outer conflict 为 0；
- live Replay finalized、非空、零 drop，offline 对全部 sampled frames 跑完 production demod；
- exact Replay identity 被 offline process result、report、Inspector 和最终 seal 一致引用；
- `published` 目录至多保留一个 `.part`，绝不出现 final file；partial 也会被封存，不能删除后假装没有输出。

输出包括 `failure-evidence-verification.json`、`matrix-run-record.json`、逐帧 `replay-inspection.json`、strict combined JSON/CSV/Markdown 和 `failure-evidence-seal.json`。成功 verifier 对 `PilotPlan.3` 同样新增 `PixelBridge.RemoteVisualMatrixRunRecord.2` 并纳入 success seal，因此成功/失败均进入同一个矩阵入口。记录中不再存在 `estimatedScaleX/Y`；`matrix.observedLocatorGeometry` 必须逐值等于 combined report 的权威 Decoder 几何对象。

## 7. 最终矩阵校验和 CSV

收齐 HardwareScope 中恰好 31 个 evidence directory 后运行：

```powershell
$runEvidenceDirectories = @(
  <cell-01-success-or-classified-failure>,
  <cell-02-success-or-classified-failure>
  # ...exactly 31 independent roots from includedCells...
)

pwsh -NoProfile -File .\tools\PBRemoteVisualEvidence\Test-PBRemoteVisualStep21Matrix.ps1 `
  -MatrixSpecPath <new-matrix-root>\step21-matrix-spec.json `
  -ExpectedMatrixSpecSha256 <frozen-spec-sha256> `
  -HardwareScopePath <new-matrix-root>\step21-hardware-scope.json `
  -ExpectedHardwareScopeSha256 <frozen-scope-sha256> `
  -RunEvidenceDirectory $runEvidenceDirectories `
  -OutputDirectory <new-matrix-root>\verified-matrix
```

verifier 会：

- 对每个 run 重新验证 plan、success/failure verification、combined report、Replay、三端 report 和 seal identity；
- 拒绝复用 RunId、plan、deployment、UI evidence、endpoint environment、Replay、combined report 或 evidence directory；
- 要求 31 个 run 使用同一个 package manifest 和同一个 1 MiB RAW source identity；
- 将每个 run 恰好映射到一个预冻结 cell，拒绝缺失、重复或 spec 外 run；
- 要求 Direct/Shape/LF4 存在一个 exact mode/WGC/FPS/1:1 对照 cohort；
- 只从 `PixelBridge.RemoteVisualCombinedReport.1` 的 Decoder/Receiver 字段提取每行指标；
- 把 accepted Locator origin/scale/residual 的样本数、last、min/max 和最大各向异性逐 run 写入 CSV，并断言 capture ROI 不是 scale evidence；
- 至少要求对照 cohort 共有一项非空权威指标：VerifiedEncodedGoodput、Bootstrap success、FER/codeword failure rate、UniqueVisualFPS 或 EndToEndUniqueVisualFPS；
- 输出一行一个 run 的 `step21-provider-generic-matrix.csv`，不求和、不平均、不拼接分母；
- 输出 `PixelBridge.RemoteVisualStep21MatrixEvidence.3`、CSV 和最终 `PixelBridge.RemoteVisualStep21MatrixSeal.3`；summary/seal 同时保存父 MatrixSpec、HardwareScope、A 端 catalog、31 included 和 10 excluded 的身份与 truth boundary，并写明 `captureRoiIsSearchNeighborhoodNotScaleEvidence=true` 与 `AcceptedBootstrapLocatorPixels` authority。

对照对象中会原样保存三条 run 的成功/失败、分类、WholeFileDigest/publish 和指标值。“LF4 提升/退化”必须由这些同条件的独立权威数值支持；不能用截图观感替代，也不能把 LF4 的一个 run 与 Direct/Shape 的另一个模式或 FPS 混比。

## 8. 当前硬件范围与正式关闭条件

用户声明 Computer B 现有两块 1920×1080 显示器，Computer A 有两块 2560×1440 显示器；B 端声明仍须由 packaged `--list-monitors` catalog 封存后才能生成正式 run。正式 plan 要求两端同时存在互不重叠的 `ProtectedMonitor` 与 `ExperimentMonitor`，B 的 1920×1080 Data Window 完全位于 ExperimentMonitor，并在整个 run 中周期重验；A 的任意宽高比搜索 ROI 同样必须完全位于一块独立 ExperimentMonitor，并足以容纳目标 locator 画布。A 的单屏上限排除了 2880×1620 的 1.5×目标画布，但不再阻塞经授权的 31-cell hardware scope。

开始正式矩阵前必须满足：

- B 端两块 1920×1080 显示器必须被 Windows/PixelBridge catalog 独立、非重叠地枚举，且为 Extend 而非 Duplicate；
- A 端两块 2560×1440 catalog identity 与 HardwareScope 完全一致，ProtectedMonitor 与 ExperimentMonitor 不重叠；
- 实际远控 UI 能逐 run 显示并证明质量优先/自动/受限模式；
- 生成包含最终 Step 21 代码的新的 verified Both-role package；
- 每个 run 的 UI capture、environment、deployment、plan 和 RunId 都重新封存。

只有 hardware-scoped 31-cell verifier PASS、CSV/summary/seal 完整、Direct/Shape/LF4 均有成功或受支持的失败记录、并且同条件 LF4 比较来自权威指标时，本次 Step 21 才可标记为“当前双机硬件范围内 `DONE`”。最终报告必须同时列出 10 个未运行的 1.5× cell，不能写成 full-41 或 1.5× coverage。该结果仍不设置 `CertifiedRemoteVisualProfile`，也不提前声明 Step 22 的重复文件恢复 smoke 通过。

## 9. 2026-09-03 实现与 readiness 验证（不是现场 cell）

本轮首先用 FINAL4 已封口的真实屏幕像素 Replay 复核尺度 authority。`build-p1_5-evidence/step21-final4-observed-locator-replay-20260903-retry1/observed-locator-verification.json` 为 `PASS`，SHA-256 为 `126b9f09d03a711c9fab7d34f8c68808c5fd1b598a7cb8939f1dab2fd83d3587`。当前 production Decoder 对 906 个 Replay capture 全部完成 demod，514 个成功 Bootstrap 的 `observedLocatorGeometry` 与独立 Inspector 逐值一致：last scale 约 `0.849999/0.849984`，全程 X/Y scale 分别位于 `0.849977..0.850010` 与 `0.849951..0.850003`，最大各向异性约 `0.0000515`；origin 约为 `(464.87,253.88)`。Receiver 再次完成 WholeFileDigest 与 safe publish，1 MiB 输出 SHA-256 为 `93f85aa63ed348ef4d565cd0bb942b2417ba4bb6e5ffafd223239ddfd83d3587`，与此前 live 输出相同。该证据只验证当前代码读取同一真实 Replay 的几何/文件语义，不是新 provider run，也不填任何 Step 21 cell。

实现收口后的自动验证如下：

- 完整 Release build 成功，`PixelBridgeEncoder.exe` 与 `PixelBridgeDecoder.exe` 均重新链接；`windeployqt` 仅报告当前 shell 未设置 `VCINSTALLDIR` 的既有 warning，部署完成且 build exit 0；
- 完整 Release CTest（未排除 Native 或 GUI）为 **223/223 PASS**，包括 GUI smoke、WGC/DXGI native capture、4 个物理 WholeFileDigest 文件 Gate、GPU parity、PilotPlan 和 Step 21 matrix contracts；最终 case-sensitive schema 加固后又单独复跑 `PBRemoteVisualPilotEvidenceContracts`，仍为 PASS；
- MSVC ASan 的 `PBTelemetryTests` 与 `PBApplicationTests` 为 **2/2 PASS**；Python strict report merger 为 **25/25 PASS**；
- 对抗性 evidence fixture 明确拒绝 Plan.3 中遗留的 ROI-derived `estimatedScaleX/Y`、分数 ROI 尺寸、字符串型/非有限 scale、case-tampered schema、成功 run 的零 Locator 样本、样本数与 Bootstrap successes 不一致、全程最大各向异性超过 0.015，以及 matrix record 内的 geometry 篡改；
- create-only run-ledger fixture 机械重算 31 个 included cell，证明 LF4 整屏搜索 ROI 与 1.0×目标画布彼此独立、Direct/Shape 仍为居中 exact 1920×1080，并拒绝 ROI 篡改、父 scope hash 错误、重复输出目录和任何 `estimatedScaleX/Y` 遗留；
- run-ledger seal importer 逐列、逐行核对 JSON/CSV，在解析前执行逐类 artifact 尺寸上限，并拒绝修改状态后重新计算 hash 的语义篡改或超限 CSV identity；EndpointScope fixture 又验证了 A 端 2560×1440 与 B 端带负 Y origin 的双 1920×1080 monitor role 绑定，拒绝重叠/Duplicate、重复 device name、尺寸不一致、origin 篡改和输出复用；
- single-extraction Computer B kit 合同用真实 Release Encoder/Decoder 构造 Both-role portable package 和 sealed source set，随后完成展开目录、外部 seal 与外层 ZIP 的逐 entry 流式复核；BAT preflight 找到 exact packaged Encoder 与 1 MiB source，输入/输出目录重叠、错误 expected manifest、archive 单字节篡改、不可变区额外文件、source 单字节篡改和 immutable-root verification output 均被拒绝，而 `Working/` 中的 create-only 运行/验证产物被允许；
- `git diff --check` 通过，未修改或纳入用户拥有的 `docs/PHASE1_GATE_REPORT.md`。

当前 create-only campaign 位于 `build-p1_5-evidence/step21-matrix-freeze-6309b44`。MatrixSpec / A catalog / HardwareScope SHA-256 分别为 `fa1f269016a1ab782e4e78516564e32caa1616e53021e76b8f29b892267db8d0`、`d225bebdc3d0a23d4dd7ece6292343578167e09d64219a3a98d463e7c368ba98`、`b7f52190f214a5c4283ca6f81e239d8c29fe154902982e07e0738ff13ee452cc`。`run-ledger` 的 JSON / CSV / seal SHA-256 为 `f0c847feb947e75953ef9f582a4aa3ac08ee02b0b9dcf575e85dc4af58f6a7c0`、`1977d6e1dd6ff6fec30bfa6f0116c768cff9df4c5ed2af08926ce4e72e0be2c1`、`4cb06ff21e3b54d48d6c4521cc90e6f966adfd05af161f80bcdf28d9efb2f4b6`。B 端回传并规范化的 catalog SHA-256 为 `19b6ccf010eea2f7ca9aca9222ecde54cd5225e1cf35ae3228242afa4340711d`；其 raw UTF-16LE identity 与 normalization provenance 仍保存在原始 field-kit。`endpoint-scope` JSON / seal SHA-256 为 `5a94208e8eba1bfd48f888b31e805664f17d66d954aec3909559a83ea847b93e` 与 `db7932c100209465333228a2ad7647ffb5335c58ddd355e7c0d592316f5fdc2a`。这些 hash 只固定 readiness 输入，不改变零现场 cell 的事实。

因此，production telemetry、Plan.3、Record.2、Evidence/Seal.3 和验证工具已达到执行正式矩阵的代码 readiness；Step 21 状态仍为 `MANUAL-GATE`，唯一完成出口仍是第 8 节规定的 31 个彼此独立的真实双机 included-cell 证据。
