# RemoteVisual Step 19：同提交 LocalDesktop 回归

## 1. 结论与真实性边界

Step 19 已完成。正式矩阵使用 Step 18 的同一 tested-source 便携包、冻结的同一份 8 MiB RAW 输入、同一组显示器与 ROI，运行 WGC/DXGI × Direct/Shape 四个 LocalDesktop 组合。四组均满足：

- Decoder 状态为 `Completed`；
- `WholeFileDigest` 与安全发布均为 PASS；
- 发布文件长度与外部 SHA-256 均与源文件完全一致；
- Decoder 完成时 Encoder 仍在广播，随后才由 harness 停止；
- Replay 明确关闭；
- FEC frame error rate 为 0，Outer conflict 为 0；
- 队列和 lease high-water mark 均在冻结容量内；
- 相对冻结基线没有 goodput 回退，四组分别提高 10.86%、10.01%、19.46% 和 21.22%；
- 用户左屏并发工作被明确记录为 `Present`，前台进程在四组矩阵前后保持不变，未把本轮伪装成 clean benchmark。

这证明当前 LF4、capture lifetime、temporal admission、Receiver admission、Replay、telemetry 和 GUI/CLI 修改没有破坏冻结的 LocalDesktop 文件恢复 oracle。它不是远程链路证据，不关闭 Computer B → 远控视频 → Computer A 的 Step 20，也不产生 `RemoteVisualSmokePass` 或 Certified Profile 结论。

## 2. 冻结输入与运行身份

| 项目 | 值 |
| --- | --- |
| 冻结基线根目录 | `build-p1_5-evidence/p1_5-prechange-20260830-193716-cd533fe` |
| 基线 commit | `cd533fe` |
| 基线 summary SHA-256 | `750d85f7f77413584553c221e4b550635c1f2afbad9d5c991be0772755d1cd7d` |
| Step 18 package manifest SHA-256 | `ab1f511be7f46a0b2be590fc618f7085e79d925bf4a2b7bb11bafc501ded2e49` |
| tested-source fingerprint | `1c0b2ff91ed98b0b1781788a8e6285891aaa224f6d9de7b2d8645150628753bc` |
| 输入 | `random-8MiB.bin`，8,388,608 bytes，RAW/OFF |
| 输入 SHA-256 | `e02206c8813b212705ce995f49498bfacdd49030f097492a6eb2777ea66a4fe2` |
| Protected monitor | `\\.\DISPLAY1`，EDID serial `T2LMTF085477` |
| Experiment monitor | `\\.\DISPLAY2`，EDID serial `T2LMTF085470` |
| Data Window / capture ROI | `[2880,180]-[4800,1260]`，1920×1080 physical pixels |
| concurrent protected-monitor work | `Present` |
| Replay | `OFF` |

`Invoke-PBLocalDesktopRegressionGate.ps1` 在启动任何产品进程前独立读取并固定上述身份；package manifest、基线 summary 或输入 hash 任一不匹配都会 fail closed。Data Window 全程必须位于 Experiment monitor，Decoder ROI 必须与它完全一致；harness 不移动鼠标、不发送按键、不激活窗口，也不调整显示设置。

## 3. 启动相位偏差的发现与修正

第一次完整开发矩阵保留在：

`build-p1_5-evidence/step19-localdesktop-dev-20260902-0938`

该轮四组文件级真值全部通过，但 WGC/Shape 与 DXGI/Shape 的 goodput 分别比历史基线低 10.25% 和 19.32%，因此工具正确给出 `BLOCKED`，没有用 digest 成功掩盖性能门禁。进一步逐项对比发现，旧 harness 固定在 Encoder 启动 3 秒后才启动 Decoder，导致 Decoder 在不同运行中从任意 Carousel 中段进入；Direct/Shape 的控制窗口、描述符建立与数据窗口长度不同，所以这个固定延迟不是公平的同源对比。

正式策略改为：

1. 先启动 Decoder；
2. 等待固定 250 ms；
3. 再启动 Encoder；
4. 250 ms 冷启动/采集等待不从 telemetry 中扣除，仍留在权威 goodput 时间窗内；
5. 两端启动 UTC 与实测 Decoder lead 写入每个 case summary；
6. Decoder journal 证明 descriptor 建立前后的 admission 状态。

这一修改只消除任意的 mid-Carousel 起点，不修改产品协议、FEC、Receiver、telemetry 公式或报告结果。开发轮 `step19-localdesktop-aligned-dev-20260902-094558` 随即恢复四组正向 goodput；该轮仍因 verifier 曾错误要求最终 resource rejection 必须为 0 而保持 `BLOCKED`。这个要求随后被纠正为准确的 Receiver 语义：descriptor 出现前，未知 Segment 数据只能进入有界 orphan 路径并可能被资源策略拒绝；关键门禁是 descriptor 已知后不再增长、conflict 为 0、最终 digest/publish 成功，而不是隐藏或清零历史计数。

## 4. 正式四组合结果

正式证据根目录：

`build-p1_5-evidence/step19-final-localdesktop-20260902-095359`

| Capture/Profile | Baseline Mbit/s | Post Mbit/s | Change | FER | Post UniqueVisualFPS | Digest / publish / external hash | Encoder still broadcasting |
| --- | ---: | ---: | ---: | ---: | ---: | --- | --- |
| WGC / Direct | 9.467 | 10.495 | +10.86% | 0 | 58.978 | PASS / PASS / PASS | PASS |
| DXGI / Direct | 9.633 | 10.597 | +10.01% | 0 | 60.003 | PASS / PASS / PASS | PASS |
| WGC / Shape | 13.665 | 16.324 | +19.46% | 0 | 59.162 | PASS / PASS / PASS | PASS |
| DXGI / Shape | 14.635 | 17.740 | +21.22% | 0 | 60.005 | PASS / PASS / PASS | PASS |

历史 baseline 没有 `uniqueVisualFps` 字段，正式比较保持 `N/A`，没有用 `captureFps` 或 admitted FrameSequence rate 追溯伪造该指标。Post 值的 basis 是 distinct legal `(CaptureEpoch, SessionTag, FrameSequence)`；production D3D11 fast path 不做整幅 ROI CPU pixel digest，因此 `roiPixelDigestUniqueVisualFps` 仍诚实为 `null`。

`VerifiedEncodedGoodput` 使用 `PBTelemetry` 的“第一条 captured observation 到 authoritative verified encoded-byte completion”窗口。它不是理论 ceiling，也没有减去 cold start、locator、descriptor acquisition 或 Receiver convergence 时间。

## 5. CPU、GPU、资源与有界队列

### 5.1 CPU 与队列基线对比

CPU 数值为进程 CPU seconds / wall seconds 的 equivalent cores。HWM 格式为 `frame lease / demod pending / result queue`。

| Case | Encoder CPU baseline→post | Decoder CPU baseline→post | HWM baseline→post | stale result drops baseline→post |
| --- | ---: | ---: | --- | ---: |
| WGC / Direct | 1.164→1.115 | 0.665→0.707 | 3/2/2→4/2/2 | 0→2 |
| DXGI / Direct | 1.156→1.122 | 0.807→0.724 | 1/2/3→1/1/2 | 0→2 |
| WGC / Shape | 1.010→0.999 | 0.549→0.822 | 3/2/3→3/2/2 | 0→2 |
| DXGI / Shape | 1.004→0.985 | 0.703→0.714 | 1/1/3→1/1/3 | 0→3 |

WGC/Shape Decoder CPU 的单轮 equivalent-core 数值较历史样本高，但 wall time 更短、goodput 提高 19.46%、所有 HWM 有界且文件级真值通过；没有持续增长或最终状态退化。新增的 2/3 个 stale drops 是 Step 13/17 明确的“晚到旧结果丢弃而不排队/乱序交付”语义，HWM 没有随之增长，不能改写成丢失 accepted Transport。

### 5.2 Post-run 资源观测

历史 summary 没有保存 GPU 与 memory HWM，所以这两项保持 baseline `N/A`，不从其他计数推导。正式 post-run 观测如下：

| Case | Encoder GPU avg/peak | Decoder GPU avg/peak | Encoder private/workset MiB | Decoder private/workset MiB | resource rejection / post-descriptor increase / conflict |
| --- | ---: | ---: | ---: | ---: | --- |
| WGC / Direct | 3.611% / 3.737% | 2.263% / 3.694% | 142.16 / 101.16 | 194.49 / 152.82 | 8420 / 0 / 0 |
| DXGI / Direct | 3.297% / 3.327% | 2.621% / 3.971% | 142.18 / 101.28 | 197.12 / 145.47 | 9092 / 0 / 0 |
| WGC / Shape | 3.298% / 3.373% | 1.664% / 3.327% | 141.50 / 101.24 | 233.28 / 152.02 | 960 / 0 / 0 |
| DXGI / Shape | 3.265% / 3.275% | 1.785% / 3.569% | 141.32 / 101.05 | 233.07 / 146.12 | 512 / 0 / 0 |

每个 journal 都满足：counter 单调、至少一次 `descriptorKnown=true`、第一次 descriptor-known sample 的 resource rejection 已等于 terminal 值、此后增量为 0、conflict 为 0、terminal 为 `Completed` 且 digest/publish 为 PASS。非零 pre-descriptor rejection 被保留为可审计事实，没有被工具抹除。

## 6. 自动化、独立验证与可重放命令

新增工具：

- `tools/PBRemoteVisualEvidence/Invoke-PBLocalDesktopRegressionGate.ps1`：执行 preflight、四组合、资源/窗口采样、外部 hash、比较表与 48-artifact create-only seal；
- `tools/PBRemoteVisualEvidence/Test-PBLocalDesktopRegressionEvidence.ps1`：不信任 producer summary，重新读取 baseline、package manifest、case/report/journal/output 和 seal，独立复算完成条件；
- `tests/tools/VerifyLocalDesktopRegressionEvidence.ps1`：以合成的 valid/tamper corpus 覆盖 wrong RunId、post-descriptor rejection 增长、conflict、启动顺序、禁止删除/动态执行等契约。

正式 producer 命令的等价重放形式：

```powershell
$package = 'build-p1_5-evidence\step18-final-deployment-20260902-084615\package-origin\PixelBridge-RemoteVisual-final-hardening-Both-bb24da82-1c0b2ff91ed9'
$baseline = 'build-p1_5-evidence\p1_5-prechange-20260830-193716-cd533fe\localdesktop-reference-summary.json'
$source = 'build-p1_5-evidence\p1_5-prechange-20260830-193716-cd533fe\sources\random-8MiB.bin'

pwsh -NoProfile -NonInteractive -File .\tools\PBRemoteVisualEvidence\Invoke-PBLocalDesktopRegressionGate.ps1 `
  -EncoderPath "$package\Encoder\PixelBridgeEncoder.exe" `
  -DecoderPath "$package\Decoder\PixelBridgeDecoder.exe" `
  -PackageManifestPath "$package\package-manifest.json" `
  -ExpectedPackageManifestSha256 ab1f511be7f46a0b2be590fc618f7085e79d925bf4a2b7bb11bafc501ded2e49 `
  -BaselineSummaryPath $baseline `
  -ExpectedBaselineSummarySha256 750d85f7f77413584553c221e4b550635c1f2afbad9d5c991be0772755d1cd7d `
  -SourcePath $source `
  -OutputRoot build-p1_5-evidence\step19-final-localdesktop-20260902-095359 `
  -ProtectedMonitorConcurrentWork Present
```

独立 verifier 使用一个 seal 外的新输出文件，防止 verifier 自己改变被验证集合：

```powershell
pwsh -NoProfile -NonInteractive -File .\tools\PBRemoteVisualEvidence\Test-PBLocalDesktopRegressionEvidence.ps1 `
  -EvidenceRoot build-p1_5-evidence\step19-final-localdesktop-20260902-095359 `
  -BaselineSummaryPath $baseline `
  -ExpectedBaselineSummarySha256 750d85f7f77413584553c221e4b550635c1f2afbad9d5c991be0772755d1cd7d `
  -PackageManifestPath "$package\package-manifest.json" `
  -ExpectedPackageManifestSha256 ab1f511be7f46a0b2be590fc618f7085e79d925bf4a2b7bb11bafc501ded2e49 `
  -OutputPath build-p1_5-evidence\step19-final-localdesktop-20260902-095359-verification.json
```

关键身份：

| Artifact | SHA-256 |
| --- | --- |
| `post-summary.json` | `0f61eb9d8607f5ba9f408b42f48daf9068678f4e8492c8996f6a90cd552ac07b` |
| `comparison.csv` | `ed7a77f69ef0da08d5047bc7ec3d1a1d497a8ea0dbfcc136ff86d17026996a6f` |
| `comparison.md` | `0780ba608094f5596ec65df894bff57f84fb534d88990946191668443ca7d54f` |
| `evidence-seal.json` | `4588177999c5cf01794bd280cd21e3404ffe7cfa50a01570f660c94b00b653aa` |
| seal 外 independent verification | `c204b940580a60840bd9c7eddfdc486af64a63aad4df28d6dc1260cc3d703bc1` |

## 7. 回归门禁

重新 configure `build-step18-final-release-20260902-084615` 后，定向七项均通过：

- `PBOuterFecTests`
- `PBReceiverTests`
- `PBGoldenVectorTests`
- `PBGoldenVectorCheck`
- `PBGoldenVectorCheckTests`
- `PBRemoteVisualReceiverEvidenceTests`
- `PBLocalDesktopRegressionEvidenceContracts`

同一 Release tree 的完整 headless CTest 为 **200/200 PASS，0 failed**，总用时 438.13 秒。日志：

`build-p1_5-evidence/step19-release-headless-198.log`

日志文件名中的 `198` 是启动命令创建时的旧预计数量；重新 configure 后权威 CTest 实际枚举为 200 项。日志 SHA-256 为 `33bdcdcf3672a1d39bece32947837ef37966e5b62269f53722d8521aed59d8ee`。PowerShell 三个脚本均通过 AST parse，最终还需在停止前的统一收尾轮执行 `git diff --check`。

## 8. Step 19 完成出口

四组合全部 publish/hash PASS，旧 Golden/Transport/Receiver 定向门禁与 200 项全量回归保持通过；goodput 无回退；CPU/HWM 的差异有明确、可重放的运行语义；GPU/memory 仅按实际可用的 post-run 观测报告；不存在未解释的性能或资源退化。因此 Step 19 可以标记为 `DONE`，下一步是 Step 20 的真实双机 LF4 pilot。
