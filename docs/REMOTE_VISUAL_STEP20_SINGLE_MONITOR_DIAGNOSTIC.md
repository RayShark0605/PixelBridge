# RemoteVisual Step 20：单屏真实远控 LF4 诊断像素链证据

## 1. 状态与真值边界

本次运行是用户明确选择的单屏实机诊断，不是 Step 20 的正式文件级 Gate。Computer B 只有一块物理显示器，production `remote-lf4` Encoder 对 ProtectedMonitor/ExperimentMonitor 不同且不重叠的要求继续正确 fail closed；没有为运行诊断而放松该安全约束。

诊断只回答一个更窄的问题：一张由真实 LF4 Encoder 逻辑生成、具有有效 Bootstrap、QC-LDPC、padding、Transport CRC 与 Session identity 的确定性 1920×1080 栅格，能否经过真实远程操控画面进入 Computer A 实际捕获到的 WGC 像素，并由 receiver-only Inspector 接受。它没有编码文件、建立完整 Receiver 文件会话、验证 WholeFileDigest 或发布文件，因此：

- 诊断结论：`DiagnosticPixelLinkPass`；
- Step 20 状态：仍为 `MANUAL-GATE`；
- `step20GateAuthority=false`；
- `RemoteVisualSmokePass` 与 `CertifiedRemoteVisualProfile` 均不得由本次运行推出。

## 2. 实际链路与操作约束

实际执行链为：

```text
Computer B PBRemoteVisualEvidencePresenter
  -> Computer B 单物理屏幕上的可见 1920x1080 LF4 Data Window
  -> 正在使用的真实远程操控桌面视频链路
  -> Computer A 右侧 ExperimentMonitor 上的可见远程桌面
  -> Computer A WGC selected ROI
  -> sealed Replay v2
  -> receiver-only PBRemoteVisualReplayInspector
  -> QC-LDPC + padding + TransportCRC + SessionIdentity
```

运行期间没有控制鼠标或键盘，没有移动、覆盖、最小化、缩放或关闭 Data Window。Computer A 的截图工具重新枚举显示器后只读取 `\\.\DISPLAY2` 的物理矩形 `[2560,0,5120,1440]`，capture record 明确记录 `containsProtectedMonitorPixels=false`。B 端 evidence kit 在活动像素运行前部署；`runs` 目录在 Presenter 自然结束后才由用户返回，不进入 Decoder 或 Receiver。

首轮临时 launcher 存在一个不影响像素的可观察性缺陷：native Presenter stdout 被 `Tee-Object` 写入 `presenter-console.txt` 后又被外层数组赋值截获，因此控制台在运行中只显示“Wait for the Presenter READY line”，没有实时显示实际已经写入文件的 `READY`/`COMPLETE`。这一缺陷只属于一次性单屏诊断 launcher；正式 Step 20 endpoint wrapper 不使用它。首轮就绪事实由 Data Window 的右屏被动截图、B 端最终 console artifact 和相互重叠的时间区间共同确认。后续 v2 包将 Presenter 输出改为逐行收集、写盘并 `Write-Host`，用户已在 B 端实际观察到实时 `READY`，最终 `COMPLETE` 仍与返回 artifact 交叉核对；v2 的像素栅格继续有意保持静态。

## 3. 主证据：同一 B 端运行、精确 1:1 ROI

### 3.1 Computer B sender

| 字段 | 值 |
| --- | --- |
| Run ID | `97816de397ce49d68b8363cbd4491d99` |
| Monitor | `\\.\DISPLAY49`, `[0,0,2560,1440]`, 单物理屏幕 |
| Data Window | origin `(320,180)`, `1920×1080`, BGRA8_UNORM |
| Profile | `PB-RemoteVisual-LF4-X1`, Profile ID `0x504252564c463431`, Layout 7 |
| Logical frame | sequence 17，重复 immutable raster |
| Frozen raster BLAKE3 | `28b09b66520b87f9cd9407b516530ad1225f84d390cc2b94d18d6e1b8d5e9bc4` |
| UTC interval | `2026-09-02T05:35:23.7628147Z` 至 `2026-09-02T05:40:23.8785469Z` |
| Presenter result | exit 0，READY/COMPLETE 均观察到 |
| Present counts | submitted 1478，successful 1473；300 秒内低于 5 Hz 上界 |

B 端五个 artifact 已从用户返回目录逐文件哈希核对后复制到：

`build-p1_5-evidence/step20-single-monitor-remote-diagnostic-20260902-124354/computer-b-run-97816de397ce49d68b8363cbd4491d99`

### 3.2 Computer A capture

- ExperimentMonitor 截图时刻：`2026-09-02T05:39:34.5626597Z`，位于上述 sender interval 内；
- Decoder capture command 时刻：`2026-09-02T05:39:34.5844351Z`，同样位于 sender interval 内；
- A 端精确 ROI：`[2880,180,4800,1260]`，即远程 2560×1440 画布上的 `(320,180)` 映射为右屏物理坐标后的 1920×1080 原尺寸区域；
- WGC Replay：8 帧，`complete=true`，66,360,746 bytes；
- Replay SHA-256：`68adba6a71294560337e410898883475ddc74c6052e112981a8a1ec774424d0d`；
- 右屏 screenshot SHA-256：`78ee2f432fd939c18ecd601f95614bd5991e034d1e24ed16553b9b8574027599`；
- 8 帧 captured-raster BLAKE3 完全相同：`c5137a0d3732100086e0fa680ad509e2ec3780f19f851c2307fcf1d7b8c5920d`。

A 端主证据位于：

`build-p1_5-evidence/step20-single-monitor-remote-diagnostic-20260902-124354/computer-a-run-97816de397ce49d68b8363cbd4491d99`

## 4. 实机发现与单变量归因

### 4.1 修复前结果

修复前 Inspector SHA-256 为 `c7765baf0b4934e19f9e6af46bddeeaa3b23248dcc62e51d2c6b293d0093297a`。同一封口 Replay 的确定性双跑结果均为：

- Bootstrap accepted：8/8；
- modulation accepted：0/8；
- Transport accepted：0/8；
- modulation erasure：`InvalidInput`；
- data work units：0。

每帧定位结果相同：

```text
originX = -0.000000740242398933333
originY = -0.0000145399249049660
scaleX  =  1.0000000007710859
scaleY  =  1.0000002254867735
markerResidualPixels = 0.0027374946912459563
```

LF4 原实现把任意负原点立即判为 `InvalidInput`。精确 ROI 的真实边界实际上是 `(0,0)`，但 locator 的连续拟合在自身收敛误差内落到了极小负值，导致数据像素尚未采样就被拒绝。8/8 Bootstrap 与 0 data work units 把失败点收窄到了 geometry precondition，而不是 FEC、CRC、identity 或远控压缩。

### 4.2 1 px margin 对照

在不改变 Presenter 栅格、远控链路、Decoder、Inspector、采样数量和速率的前提下，仅把 A 端 ROI 四边各扩 1 个物理像素至 `[2879,179,4801,1261]`。定位结果变为精确 `(originX,originY,scaleX,scaleY)=(1,1,1,1)`，随即得到 8/8 Bootstrap、8/8 modulation、8/8 Transport、32 accepted blocks、0 FEC/CRC/identity failure，双跑逐字节一致。

该对照发生在随后一个 B 端静态栅格运行 `0e67d17fa1f245c1a8b36864761d5213` 的时间区间内，而 A 端临时脚本的 metadata 仍沿用了第一个 Run ID。因此它只作为“边界数值问题”的单变量诊断，不作为主证据的严格 Run-ID 绑定。主结论改由第一个 Run 的原始 Replay 在修复后重放得到。

## 5. 修复与 fail-closed 边界

修复没有改变 Visual Profile、Layout、wire bytes、Walsh codebook、FEC、Transport、Receiver 或发布规则：

1. 公开 locator 的 `0.005 px` refinement convergence 常量，并让 LF4 boundary tolerance 引用同一常量，避免两个阈值漂移；
2. 新增 `ResolveRemoteVisualLowFpsSamplingGeometry`：仅当拟合的近端/远端边界位于该收敛阈值内时，把采样几何向实际 frame 内侧收敛；
3. 非有限值、真实负原点、超过阈值的裁切、无效 scale/policy、marker residual 超限和 frame 越界仍明确 erasure，且失败时输出参数不改变；
4. CPU reference/Replay Inspector、production CaptureDemodulator bootstrap gate 与 D3D11 direct-texture shader constants 共用该 resolver；
5. WARP 回归直接使用本次实机捕获的负微量 geometry，要求 compact metrics 与 CPU accepted Transport truth 一致。

### 5.1 同一不可变 Replay 的修复后结果

最终修复版 Inspector SHA-256 为 `7bdd27c3f04dc291b43a235369af55e3a5d756406dd2efae4f82c7811671a0ad`。对原始 Replay 双跑：

| 指标 | 修复前 | 修复后 |
| --- | ---: | ---: |
| Replay SHA-256 | `68adba6a...424d0d` | `68adba6a...424d0d` |
| Bootstrap accepted | 8/8 | 8/8 |
| Modulation accepted | 0/8 | 8/8 |
| Transport accepted | 0/8 | 8/8 |
| Accepted blocks | 0 | 32 |
| FEC / CRC / identity failures | 0 / 0 / 0 | 0 / 0 / 0 |
| Inspector output SHA-256 | `984259e8...1078cb` | `ae861f3f...42bb90` |

修复后每帧 `dataBytes=8100`、`dataWorkUnits=6116352`、77/77 freshness regions fresh、0 stale/mismatch/erasure、64,800 个 primary margin samples 的 minimum/p50/p01/p001 均为 1。两次最终输出 SHA-256 完全相同，归档后的修复前/修复后 Inspector 也能分别逐字节复现各自原结果。

## 6. 验证

已在 Release 配置执行：

- `PBModulationTests`；
- `PBDemodD3D11Tests`，包括 WARP continuous geometry、compact readback、FEC/Transport parity 和 invalid-boundary negative cases；
- `PBRemoteVisualReplayInspectorTests`；
- 原始真实 Replay 的修复前归档 Inspector 重放；
- 同一 Replay 的最终修复版归档 Inspector 双重放。

持久化 build/CTest 日志与首轮最终 correlation manifest 位于本诊断 evidence root 的 `boundary-fix-validation/` 和 `diagnostic-correlation.json`。manifest 递归验证 sender/A capture/Replay/Inspector/source identities、严格 JSON、时间区间、前后结果和真值边界后，以 create-only 方式生成：

- `diagnostic-correlation.json` SHA-256：`ff0cd65622a972bbda54b931938eacda0156c13e1f81d0ab830151440be688da`；
- `diagnostic-correlation.seal.json` SHA-256：`0443f4c95a6bdecd33419a56fe13b47f5daa803387a1fa81336862b4fc12b82c`。

## 7. v2 单屏真实远控复验

### 7.1 v2 package 与 B 端权威运行

为了消除首轮 launcher 的实时可观察性问题，另行生成、不覆盖 v1 的 v2 包：

`build-p1_5-evidence/step20-single-monitor-diagnostic-v2-kit-20260902-143140/ComputerB-LF4-SingleMonitor-Diagnostic-v2.zip`

ZIP SHA-256 为 `b1123c35fec14ac8bdedbe4f306c3707e7c144df29adb810d590f2e987f33157`。包内 Presenter 仍为同一冻结二进制，SHA-256 为 `0fb0f30471fdb4997b20ba8edd5608a8be580e2bf4e4bfd2b8d2b8d7ce29b885`；只改变 launcher 的 stdout streaming 与说明文字，没有改变 raster 或 Presenter。Windows PowerShell 5.1 parser、12-file `SHA256SUMS.txt` 和 ZIP 内逐文件 hash 均由独立 sealer 再次验证。

用户在 B 端观察到实际 `READY` 后返回的权威 run 为：

| 字段 | 值 |
| --- | --- |
| Run ID | `40f1456b9ce5493a97eb8a4e4057abac` |
| UTC interval | `2026-09-02T06:33:53.3470648Z` 至 `2026-09-02T06:38:53.5152781Z` |
| Monitor / origin | `\\.\DISPLAY49`，2560×1440；`(320,180)` |
| Raster | 1920×1080，`PB-RemoteVisual-LF4-X1`，Layout 7，sequence 17 |
| Presenter result | exit 0，READY/COMPLETE 均观察到 |
| Present counts | submitted 1477，successful 1472；attempted cadence 约 4.92 Hz |

B 端返回目录恰好包含 5 个预期 artifact，复制前后逐文件 SHA-256 相同；桌面原始 `runs2` 未被修改。

### 7.2 A 端两次不改参数的精确 ROI 捕获

两轮都使用 A 端右屏 exact ROI `[2880,180,4800,1260]`、WGC、最多 8 帧、128 MiB、2 sample FPS、45 秒有界窗口，并使用最终修复版 Inspector `7bdd27c...1a0ad`。截图、command、Decoder 起止与 summary 时间全部位于上述 B sender interval 内；两个 `decoder-output` 目录均为空。

| 指标 | 首次 8-frame capture | 相同参数 bounded repeat |
| --- | ---: | ---: |
| Replay complete | true | true |
| captured frames | 8 | 3 |
| Bootstrap accepted | 7 | 3 |
| LF4 modulation accepted | 7 | 3 |
| Transport accepted | 7 | 3 |
| accepted Transport blocks | 28 | 12 |
| FEC / CRC / identity failures | 0 / 0 / 0 | 0 / 0 / 0 |
| Inspector double-run deterministic | true | true |
| diagnostic outcome | `DiagnosticPixelLinkNotProven` | `DiagnosticPixelLinkPass` |

首次 capture 的 ordinal 1 是唯一擦除帧：其 raster BLAKE3 与其余帧不同，Bootstrap 明确给出 `MarkersNotFound`，modulation 给出 `BootstrapErasure`，`dataWorkUnits=0`，Transport 没有被评估，也没有 accepted block。其余 7 帧与复验 3 帧的实际 captured-raster BLAKE3 均为 `4d9971560c8f29b8e11e371bfbfb7d8880cfc064fddb16d712005754003931e9`，geometry 均精确为 `(0,0,1,1)`，每帧 4 个 Transport blocks、64,800 margin samples 全为 1。复验只得到 3 帧，是静态画面在 45 秒窗口内只交付了 3 个 capture update；没有复制帧或伪造达到 8 帧，3 帧全部通过。

两轮合计观察 11 帧，其中 10 帧完整进入 Transport、共接受 40 个 blocks，1 帧在 Bootstrap 安全擦除。receiver-only Inspector 没有独立 sender truth，因此 `falseAcceptedCodewords=null`、availability=`UnavailableReceiverOnly`，不能把这些统计写成“0 false accept”；能确认的是没有 FEC/CRC/identity failure、没有 Receiver/file publish，也没有错误输出文件。

### 7.3 v2 create-only correlation

v2 独立 sealer 重新验证 B 包/ZIP、B run exact inventory、UTF-16 console、两轮 A exact inventory、严格 JSON/JSONL、Replay/Inspector payload seal、屏幕隔离、双跑确定性、二进制 identity、时间区间、首轮 boundary-fix correlation 以及三项 Release 回归后生成：

- `diagnostic-v2-correlation.json` SHA-256：`4dc250272ae0f75ebad3bbe9b5674533dc408e3febd2a9536e6b55c427389843`；
- `diagnostic-v2-correlation.seal.json` SHA-256：`ee1798bc53f695d26b88a70d576698c965037445c94af267eaa298201b4c18b5`。

该复验直接关闭了“修复后 exact 1920×1080 ROI 是否仍需 1 px margin”的疑问：不需要；真实远控 capture 的接受帧全部以 exact `(0,0,1,1)` geometry 通过。但该结论仍只属于静态、单逻辑 frame 的 receiver-only 像素链。

### 7.4 复验后的最终回归

v2 correlation 完成后又在当前 working source 上执行：

- `PBRemoteVisualDeploymentEvidenceContracts` 与 `PBRemoteVisualPilotEvidenceContracts`：Release 2/2 PASS；日志 SHA-256 `d3a2bcefd25f0e2aa2638cb098372745f36bcf385d2dff3dfa36ab8af7f21249`；
- 完整 Release build：exit 0；日志 SHA-256 `d14a24e30a091942c0343ceeb1944f336b6be55ef1fb6cc9ef24b94e3673ad5c`；
- 完整 Release CTest：201/201 PASS、0 失败、851.91 秒；日志 SHA-256 `dea2d029317fce59f5f60adb7e56264f9ce0e264014b9a9437e7ece0cc19cf5f`。

持久化目录分别为 `build-p1_5-evidence/step20-post-v2-contracts-20260902-145422` 与 `build-p1_5-evidence/step20-post-v2-full-release-20260902-145458`。完整轮包含 D3D11 WARP/hardware、GPU parity、Presentation Gate、RemoteVisual、Replay、Receiver、Golden、文件安全、package/plan/evidence contracts；它不把静态诊断提升为文件级 field pass。

独立 post-v2 validation sealer 又重新核对三组日志、result JSON、v2 diagnostic seal、最终 Decoder/Inspector identity、scoped source diff 和 `git diff --check` 后，以 create-only 方式生成：

- `post-v2-validation.json` SHA-256：`279e85051606a3d3325090495f74396059f176bdc5875f1b48fc1b19ae0ce801`；
- `post-v2-validation.seal.json` SHA-256：`51068911baa26ada8140fdc422299d75b16b4178064750902201785be262fd71`。

## 8. 仍未完成的 Step 20 出口

这次运行没有也不能证明：

- production Encoder 的完整文件 carousel；
- LF4 GPU demod 后进入 production Receiver/Outer 的文件会话汇聚；
- WholeFileDigest；
- `.part` 到 final 的安全发布；
- live published 与 source 的 external length/SHA-256；
- 同一 Replay 经 production offline Decoder 再次发布同一文件；
- Decoder 完成后 Encoder 在冻结跨机时钟窗口内仍广播；
- 1/2/5 Hz 与 locator-scaled 的正式矩阵。

要关闭 Step 20，仍需 Computer B 具备两个互不重叠的物理 monitor authority，或者未来另有经过设计审查且不削弱 ProtectedMonitor 隔离语义的正式环境。当前单屏环境下保持 `MANUAL-GATE` 是预期且正确的结论。
