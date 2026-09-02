# RemoteVisual Step 20：真实双机 LF4 pilot 的冻结执行与证据门禁

## 1. 当前状态与不可替代的完成条件

Step 20 当前仍为 `MANUAL-GATE`。本文件记录已经实现并自动验证的执行工具、资源边界和正式双机操作顺序；它本身以及任何本地闭环、WARP、离线 Replay 或右屏截图探针都不能把 Step 20 标记为 `DONE`。

正式运行必须同时满足：

1. Computer B 的 packaged `PixelBridgeEncoder.exe` 把固定 1 MiB RAW source 编码到 1920×1080 Data Window；
2. Data Window 像素经过正在使用的真实远程操控/桌面视频链路显示在 Computer A 的 ExperimentMonitor；
3. Computer A 的 packaged `PixelBridgeDecoder.exe` 只从实际 WGC ROI 像素进入 LF4 GPU demod、temporal admission、现有 Receiver/Outer、WholeFileDigest 和安全发布；
4. live Decoder 已 `Completed` 且 Replay footer 已封口时，Computer B Encoder 仍为 `Broadcasting`；等待冻结的跨机时钟证明窗口后，用户在 Encoder 控制台按 Enter 或 Q 人工停止；
5. 同一 live Replay v2 由 fresh output directory 中的 production offline Decoder receiver-only 重放，再次完成 WholeFileDigest 和安全发布；
6. 外部检查器确认 source、live published 和 offline published 的 length/SHA-256 完全相同，且没有 false output、Outer conflict 或证据截断；
7. 每轮保存两端 report/journal/process result、Replay、remote UI 可见证据、package/source/environment/deployment/plan identity 和严格 combined report。

活动传输窗口内禁止远控文件传输、clipboard、共享目录、socket、pipe、临时文件交换或任何非视觉 payload 旁路。便携包、source identity、计划和命令必须在 Decoder 启动前完成预部署；Decoder 成功后复制 Encoder 证据只属于事后证据汇总，不进入 Receiver。

## 2. Step 20 新增的 runtime 完整性约束

### 2.1 production Replay 的采样与资源预检

`remote-lf4` live Decoder 现在允许 `--replay-sample-fps`，但采样只作用于可选 Replay fan-out，不改变 primary LF4 capture/demod/Receiver admission。运行前同时预检：

- LF4 live demod resident ceiling：256 MiB；
- CaptureNormalize resident ceiling：普通 1 GiB，Replay 2 GiB；
- Replay readback ceiling：768 MiB；
- processor scratch ceiling：128 MiB；
- capture-owned staging slot count 与四槽 capture ring 一致；
- Replay writer queue 固定为 2，并同时检查 `maximumFrames`、ROI frame bytes、文件上限和非帧预留。

Decoder publish 后仍继续采集固定 2 秒，再关闭 writer 并写 footer。只有 `replay.evidenceValid=true`、`finalized=true`、非零 live written/offline capture frames 且实际 CLI 输出成功时，带 Replay 的 run 才允许返回 0。

当 production live Replay 的采样率非零时，runtime 有意不把 live demod observation 写入 Replay：采样后的画面序列与未采样 primary demod 序列不是逐帧同一序列，伪造 observation 对齐反而会制造错误的 parity 结论。因此正式门禁要求 live `writtenDemodObservations=0`、`droppedDemodObservations=0`；offline 必须满足 `offlineDemodResults=offlineCaptureFrames`、`offlineObservationComparisons=0`、零 mismatch。权威复现结论来自完整封口的 sampled Replay 经 production receiver-only offline Decoder 再次完成 Receiver、WholeFileDigest、安全发布，并与 source/live output 做外部 length/SHA-256 一致性验证，而不是逐帧 live/offline observation 比较。

### 2.2 publish 与 Replay footer 的竞态修复

离线 Replay 可能在读到决定性数据帧时已经完成文件发布，但 reader 尚未读到 footer。此前外部观察到 `Completed` 后立即调用 `Stop()` 会使 footer 消费具有时序依赖。现在 offline pipeline 在发布后暂留 `Publishing`，完整读取 Replay footer、完成 cleanup 并确认 evidence 后才公开 `Completed`；若其间停止，则明确为 `Stopped`，不得把已发布的中间事实当作完整 Replay 证据。

live post-publish Replay finalization 失败也不再让 CLI 无限等待；runtime 会设置 evidence invalid/error，CLI fail closed。

### 2.3 人工停止与无进展规则

Encoder 的 `--manual-stop` 要求附着的交互式 Windows console，只接受 Enter 或 Q；每次轮询最多读取 32 个 console events。`--seconds` 始终是硬上限，只有确实收到人工停止输入且 report/journal 合同成立时 formal wrapper 才接受 exit 0。

Decoder 的 `--no-progress-seconds` 以 descriptor transition、temporally admitted blocks 和 verified raw bytes 为权威进展量。三者在冻结窗口内都不变化时，runtime 请求有界停止并输出明确诊断，而不是无限等待。offline Replay 同样受 600 秒总窗口和 120 秒无进展上限约束。

## 3. 冻结的 1/2/5 Hz 执行策略

每轮由 create-only `PixelBridge.RemoteVisualPilotPlan.1` 固定参数；不能在运行中延长窗口或改变 Replay 采样率：

| Logical FPS | Encoder 硬上限 | live Decoder timeout | live no-progress | Replay sample FPS | 最坏 sampled frames（含 2 秒 tail + 2 帧余量） |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 600 s | 540 s | 120 s | 2 | 1086 |
| 2 | 360 s | 300 s | 90 s | 4 | 1210 |
| 5 | 180 s | 120 s | 60 s | 10 | 1222 |

所有配置还固定：

- WGC、`remote-lf4`、Segment compression `off/RAW`、Control repetitions 12；
- Decoder 必须先启动，Encoder 最迟在 30 秒内启动；
- Replay `maximumFrames=1250`、`maximumMiB=16384`、非帧预留 256 MiB；
- offline timeout/no-progress 为 600/120 秒；
- live Decoder 公开完成后，人工停止 Encoder 前等待 `ceil(clockUncertaintyMs/1000)+2` 秒；默认时钟不确定度 5000 ms，因此等待 7 秒；
- strict 1:1 的 Decoder ROI 必须恰好 1920×1080；locator-scaled 轮必须至少一轴不是 1.0，且两轴都在 0.5..2.0；
- exact-canvas locator fit 在 refinement convergence `0.005 px` 内越过零点或远端 frame boundary 时，CPU/GPU 共用 resolver 只把采样 geometry 向 frame 内侧收敛；真实负原点、超过该阈值的裁切和非法 scale 仍 fail closed；
- 计划生成器使用实际 ROI 复算 `width × height × 4 × 1250 + 256 MiB`，超过 16 GiB 直接拒绝。

表中的 sampled-frame 计算为 `sampleFps × (decoderTimeout + 2) + 2`；它是资源上界，不是假定实际 run 一定持续到 timeout。

## 4. remote UI 与左右屏隔离证据

`Capture-PBRemoteVisualExperimentMonitor.ps1` 每次重新调用 packaged Decoder `--list-monitors`，要求显式且不重叠的 ProtectedMonitor/ExperimentMonitor，然后只通过 `CopyFromScreen` 读取 ExperimentMonitor 的 exact physical rectangle。它不发送输入、不改变焦点、不移动窗口、不修改显示设置，也不捕获 ProtectedMonitor。

截图不能单独成为 provider-mode 证据。`New-PBRemoteVisualUiEvidence.ps1` 还必须收到同一次采集生成的 `PixelBridge.ExperimentMonitorScreenshotCapture.1` record，并交叉绑定：

- screenshot path/size/SHA-256；
- ExperimentMonitor physical rectangle 与图像尺寸；
- protected/experiment device identity；
- shared metadata/RunId；
- UI 中确实可见的 provider、mode，以及仅在确实可见时记录的 version/resolution/FPS/chroma。

`Test-PBRemoteVisualUiEvidence.ps1` 会独立重算上述全部 identity，并再次运行 bounded image analyzer。未知或 UI 中不可见的 chroma、latency、bandwidth 等字段保持 `Unknown`/空值，不能猜测。
同样，preset 中没有经过 UI/环境实际观察的 `targetFps` 必须保持 JSON `null`，不得由默认值或整数零伪装成现场事实；只有非空且有限的 1..240 值才能进入 UI claim 的 exact parity 检查。

## 5. 正式运行工具与顺序

以下 Step 18/20 evidence tools 必须由 `pwsh` 7.0 或更高版本执行；不要用 Windows PowerShell 5.1。严格 JSON 路径依赖 PowerShell 7 的 case-sensitive hashtable 与 `System.Text.Json`，八个 Step 20 入口都以 `#Requires -Version 7.0` 在读取或写入任何正式证据前 fail closed。当前已验证的工具宿主为 PowerShell 7.6.4；A/B 正式运行前都必须先执行 `pwsh --version`，endpoint wrapper 还会把实际 edition/version 自动写入 process-result，最终 verifier 要求三端记录均为 PowerShell Core 7+。

### 5.1 先生成同一 tested-source 包和真实双端 deployment

使用 Step 18 工具生成新的 `-Role Both` package、1 MiB source set、`RemoteUiVisible` metadata。把相同 package/archive/seal、source set/manifest/seal、metadata 和 Step 20 tools 在活动传输前预部署到 A/B。分别在真实 A/B 上运行 `Get-PBRemoteVisualEnvironment.ps1`，把 B 的 environment artifact 汇总到 A，再生成并独立验证 deployment manifest。

本机伪造两份 endpoint snapshot 不算真实双机 environment，旧 Step 18 包也不能用于新的 Step 20 runtime。

### 5.2 在 A 的右侧 ExperimentMonitor 显示真实 provider 设置 UI

用户手动打开当前远控软件的质量/分辨率/缩放设置面板，并确保它完全位于右侧 ExperimentMonitor。随后在 Computer A 执行：

```powershell
.\Capture-PBRemoteVisualExperimentMonitor.ps1 `
  -PixelBridgeDecoderPath <verified-package>\Decoder\PixelBridgeDecoder.exe `
  -ProtectedMonitorDeviceName <A-protected-device> `
  -ExperimentMonitorDeviceName <A-experiment-device> `
  -OutputPath <new-run>\remote-ui.png `
  > <new-run>\remote-ui-capture-record.json

.\New-PBRemoteVisualUiEvidence.ps1 `
  -MetadataPath <new-run>\remote-metadata.json `
  -ScreenshotPath <new-run>\remote-ui.png `
  -CaptureRecordPath <new-run>\remote-ui-capture-record.json `
  -OutputPath <new-run>\remote-ui-evidence.json `
  -PythonPath D:\Python3.12.9\python.exe `
  -VisibleProvider <exact-visible-provider> `
  -VisibleMode <exact-visible-mode>
```

只传入 UI 中确实可见的 optional claims。关闭设置面板后再开始数据运行。

### 5.3 为每个 FPS/geometry 单独生成 plan

```powershell
.\New-PBRemoteVisualPilotPlan.ps1 `
  -DeploymentManifestPath <new-run>\deployment-manifest.json `
  -UiEvidencePath <new-run>\remote-ui-evidence.json `
  -PythonPath D:\Python3.12.9\python.exe `
  -OutputPath <new-run>\pilot-plan.json `
  -LogicalFps 5 `
  -GeometryMode Strict1To1 `
  -EncoderProtectedMonitorDeviceName <B-protected-device> `
  -EncoderExperimentMonitorDeviceName <B-experiment-device> `
  -EncoderOriginX <B-data-window-left> -EncoderOriginY <B-data-window-top> `
  -DecoderProtectedMonitorDeviceName <A-protected-device> `
  -DecoderExperimentMonitorDeviceName <A-experiment-device> `
  -DecoderRoiLeft <left> -DecoderRoiTop <top> -DecoderRoiRight <right> -DecoderRoiBottom <bottom>
```

把输出的 plan SHA-256 作为两端命令的 `ExpectedPlanSha256`，不能只按文件名信任 plan。

### 5.4 Decoder 先启动，随后 Encoder

在 Computer A 启动 live wrapper：

```powershell
.\Invoke-PBRemoteVisualPilotDecoder.ps1 `
  -Mode Live `
  -PlanPath <pilot-plan.json> -ExpectedPlanSha256 <plan-sha256> `
  -PackageDirectory <A-package> -PackageSealPath <A-package-seal> `
  -RemoteMetadataPath <A-metadata> `
  -OutputDirectory <new-A-live-evidence>
```

30 秒内，在 Computer B 已预先打开的 console 启动 Encoder wrapper：

```powershell
.\Invoke-PBRemoteVisualPilotEncoder.ps1 `
  -PlanPath <pilot-plan.json> -ExpectedPlanSha256 <plan-sha256> `
  -PackageDirectory <B-package> -PackageSealPath <B-package-seal> `
  -SourcePath <B-source-set>\random-1MiB.bin `
  -RemoteMetadataPath <B-metadata> `
  -OutputDirectory <new-B-encoder-evidence>
```

当 A 明确输出 `DECODER_COMPLETED_AND_REPLAY_FINALIZED` 时继续保持 Encoder 广播；等待 plan 指定的 proof seconds 后，用户在 B Encoder console 按 Enter 或 Q。不要把 Decoder 的完成信号通过程序 IPC 发给 Encoder。

Encoder/Decoder wrapper 从一开始就直接创建最终的 create-only evidence directory，runtime report、journal、Replay 和 published output 都写入其最终绝对路径；成功时最后写入对应 `*-process-result.json` 作为 endpoint 完成标记。工具禁止先写 `<final>.partial` 再移动目录，因为 report 中已经持久化的绝对路径在移动后会失效。中断或失败目录会原样保留用于诊断，而且由于 create-only 规则不得复用。

### 5.5 事后汇总、offline reproduce 和最终验证

Encoder 已停止后，才把 B endpoint evidence 复制到 A 的证据目录。然后运行：

```powershell
.\Invoke-PBRemoteVisualPilotDecoder.ps1 `
  -Mode Offline `
  -PlanPath <pilot-plan.json> -ExpectedPlanSha256 <plan-sha256> `
  -PackageDirectory <A-package> -PackageSealPath <A-package-seal> `
  -RemoteMetadataPath <A-metadata> `
  -ReplayInputPath <A-live-evidence>\live-capture.pbrv2 `
  -OutputDirectory <new-A-offline-evidence>

.\Test-PBRemoteVisualPilotEvidence.ps1 `
  -PlanPath <pilot-plan.json> -ExpectedPlanSha256 <plan-sha256> `
  -EncoderEvidenceDirectory <copied-B-encoder-evidence> `
  -LiveDecoderEvidenceDirectory <A-live-evidence> `
  -OfflineDecoderEvidenceDirectory <A-offline-evidence> `
  -SourcePath <A-verified-source-copy>\random-1MiB.bin `
  -PythonPath D:\Python3.12.9\python.exe `
  -OutputDirectory <new-final-verification>
```

最终 verifier 同样从一开始创建最终 create-only output directory，并在所有检查成功后最后写入 seal 作为完成标记；失败目录保留且不可复用。它不接受“Decoder report 看起来成功”作为充分条件，还要求：所有 JSON 和 JSONL 递归拒绝 exact-name duplicate keys、非有限数值和 trailing input；endpoint process identity、package manifest、executable、metadata、source、HEAD commit 与 frozen plan 精确一致；report 时间区间落在 wrapper process 时间区间内；exact process-result artifact inventory 和三份 journal 完整结束；source/live/offline SHA-256、SessionId/SessionTag/WholeFileDigest 一致；live Replay 与 offline input 的 path/size/hash、frame/demod/file identity 一致；采样模式遵守上述 observation 合同；bounded queue HWM；Decoder-before-Encoder warmup；以及 strict Python merger 的 `successfulRun=true`。

跨机仍在广播证明以 live Decoder wrapper 完成 report/Replay/publish 校验和大文件 identity 计算之后、即将原子写入 process-result 完成标记时记录的 `evidenceReadyUnixMilliseconds` 为证据就绪时刻；它不得早于 runtime 的 `endedUnixMilliseconds`。verifier 先用 frozen clock offset 把它换算到 Encoder 时钟，再加上完整的 `postDecoderBroadcastProofSeconds`，要求 Encoder journal 在该阈值时刻或之后仍有 `Broadcasting` sample，并记录实际超出时长；所有换算都使用 checked Int64 arithmetic。只在“adjusted Decoder completion + clock uncertainty”之后看到一次 sample 不再构成充分证明。

## 6. 已完成的 readiness 验证与当前真值边界

在正式双机运行之前已完成以下非 field-gate probes：

- sampled production Replay + 2 秒 publish tail 的 local WGC production Decoder/Receiver/offline reproduction 闭环；
- interactive console 的真实 Q 人工停止、Stopped report 和 finished journal；
- offline publish-before-footer 竞态的定向复现、修复后 5 次重复通过；
- 右侧 2560×1440 exact ExperimentMonitor 截图、capture-record/UI evidence 交叉绑定、create-only 和 claim-conflict negative tests；
- 5 Hz strict plan 的本机双 endpoint identity probe，固定资源上界通过；
- `PBRemoteVisualPilotEvidenceContracts` 覆盖 PowerShell parser closure、UI fixture、独立 verifier、overwrite/claim/hash/resource/geometry tamper rejection、递归 duplicate-key/非有限 JSON rejection、proof-window checked arithmetic/overflow，以及三个持久证据 producer 禁止 post-write directory move 的源级合同。
- Computer B 单物理屏幕环境按用户选择执行了非 Gate 的真实远控静态 LF4 像素链诊断：首轮匹配的 B sender run 与 A exact WGC ROI capture 时间重叠，8 帧 sealed Replay 在修复 exact-canvas locator 的 `-1.45e-5 px` 边界舍入问题后，由归档 Inspector 确定性双跑得到 8/8 Bootstrap、8/8 modulation、8/8 Transport、32 accepted blocks 和 0 FEC/CRC/identity failure；相同 Replay 在旧 Inspector 中稳定得到 8/8 Bootstrap、0/8 modulation，构成单变量前后对照。随后使用实时回显 READY 的 v2 包执行独立 B run：A 端首次 exact ROI capture 为 7/8 全链接受、1 个 `MarkersNotFound` 安全擦除，相同参数 bounded repeat 为 3/3 全链接受；两轮共 40 accepted blocks、0 FEC/CRC/identity failure，所有接受帧 geometry 均为 exact `(0,0,1,1)`，不再依赖 1 px margin。receiver-only `falseAcceptedCodewords` 仍为 unavailable，详细 evidence、代码边界和未完成项见 `REMOTE_VISUAL_STEP20_SINGLE_MONITOR_DIAGNOSTIC.md`。
- v2 复验后在当前 working source 上重新执行 deployment/pilot evidence contracts 2/2 PASS，并完成 Release build exit 0 与完整 CTest 201/201 PASS；持久日志分别位于 `build-p1_5-evidence/step20-post-v2-contracts-20260902-145422` 和 `build-p1_5-evidence/step20-post-v2-full-release-20260902-145458`。

这些只证明 Step 20 工具和 runtime 能进入真实门禁，以及当前真实远控链在静态 LF4 receiver-only Transport 边界上可通过；不证明 1 MiB 文件、WholeFileDigest、安全发布或 Replay offline 文件复现已经通过。Computer B 的单屏 production safety preflight 仍正确阻止正式 Encoder。Step 20 在真实 A/B 完成至少一轮端到端文件恢复前必须保持 `MANUAL-GATE`；路线要求仍计划执行 1/2/5 Hz strict 1:1，并在成功基线后再执行 locator-scaled 轮。
