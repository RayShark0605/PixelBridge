# RemoteVisual LF4 Step 17：GUI/CLI 暴露与屏幕安全

状态：**DONE（2026-09-02）**。

本步骤只关闭 `PB-RemoteVisual-LF4-X1` 从隐藏 production seam 到公开产品绑定的差距，并证明启动前/运行期屏幕安全、公开 CLI、真实屏幕像素闭环与既有文件发布语义仍然一致。它不把 LF4 宣称为 Certified Profile，也不替代 Step 18 的便携包、Step 19 的 LocalDesktop 同提交回归或 Step 20 的真实双机 field Gate。

## 1. 完成出口

Step 17 的完成出口为：

1. GUI 与 CLI 使用同一个只读 profile catalog，公开独立 token `remote-lf4`；旧 `remote` 的 enum、token、wire identity 和默认选择不变。
2. LF4 只允许 RemoteVisual channel、1..5 Hz、独立 `ProtectedMonitor` 与 `ExperimentMonitor`，非法或不完整配置在启动前 fail closed。
3. Encoder 的固定 1920×1080 Data Window 必须完全位于 `ExperimentMonitor`；Decoder 的 live physical ROI 必须位于该显示器、identity rotation，且 X/Y 各自处于 0.5x..2.0x。精确 canvas/scale 仍由 captured-pixel locator 决定，不做静默 resize。
4. topology watchdog 每秒重新枚举并比对显示器 identity/geometry/rotation/refresh；任何漂移使 run 失败，report/journal 明确记录 `Pending/PASS/FAIL` 与重验证次数。
5. right-monitor native Gate 必须证明 no-activate、前台 PID 不变、Data Window containment 和 watchdog 运行。
6. 公开 Encoder/Decoder CLI 必须从实际 WGC 捕获像素完成一次最小本地闭环，并经过现有 Receiver、WholeFileDigest 与 safe publish 恢复 external byte-exact 文件。

上述出口现已全部关闭。

## 2. 公共 profile catalog 与兼容性

`apps/common/application_model.h/.cpp` 现在提供唯一的公共 catalog：

- `GetVisualProfileOptions()`；
- `FindVisualProfileOption()`；
- UTF-8/UTF-16 两种 `ParseVisualProfileToken()`；
- `IsRemoteVisualProfile()`。

四个公开选项按稳定顺序列出：

| token | profile | 默认 logical FPS | 默认 Control repetitions |
| --- | --- | ---: | ---: |
| `direct` | Direct-Level 2x2 | 0 | 4 |
| `shape` | Shape+Chroma | 0 | 4 |
| `remote` | 旧 RemoteVisual Resilient | 2 | 12 |
| `remote-lf4` | `PB-RemoteVisual-LF4-X1 (Experimental)` | 2 | 12 |

GUI 下拉框和两端 CLI parser 都从这个 catalog 取得 identity；因此不会再各自维护容易漂移的重复 token 表。`remote-lf4` 仍绑定独立 `VisualProfileId = 0x504252564C463431`、`LayoutVersion = 7`、64,800 coded bits/帧、四个未修改 Robust QC-LDPC codeword。代码没有把旧 `remote` 重解释为 LF4，也没有修改既有 Golden 或协议序列化。

QSettings 只保存本地 UI 偏好，例如最近输出目录、backend、ROI 和 `ui/lastProtectedMonitorDevice`；它不保存 VisualProfile、Session、descriptor、Receiver 或发布状态。用户必须在每次运行中显式选择 LF4，不会因为历史设置在不知情时改变 wire identity。

## 3. GUI 与 CLI 绑定

### 3.1 Encoder

GUI：

- profile 下拉框显式显示 `PB-RemoteVisual-LF4-X1 (Experimental)`；
- 选择 LF4 时自动切换到 RemoteVisual channel，并应用 catalog 的 2 Hz/12 repetitions 默认值；
- 只有 LF4 时启用 `ProtectedMonitor` 选择，且必须与 Data Window 所在的 `ExperimentMonitor` 不同；
- 既有目标显示器选择仍只决定 PixelBridge 自己的 Data Window，不移动、不关闭任何外部窗口，也不注入全局输入；
- metadata preset 中已有 monitor identity 时必须与实时选择完全一致，否则启动失败。

CLI：

```text
PixelBridgeEncoder --headless-broadcast --source PATH \
  --profile remote-lf4 --channel remote --remote-provider NAME \
  --compression off --origin X Y --seconds 1..600 --logical-fps 1..5 \
  --protected-monitor DEVICE --experiment-monitor DEVICE
```

缺失/空值/重复 monitor 参数、same monitor、`--channel local`、别名 `lf4`、0 Hz 或 >5 Hz 均拒绝。LF4 的 Data Window 始终为固定 1920×1080 physical pixels；启动前验证其完整包含于 `ExperimentMonitor` 且不与 `ProtectedMonitor` 相交。

### 3.2 Decoder

GUI：

- 使用同一 profile catalog；选择 LF4 时切换到 RemoteVisual channel；
- ROI 状态明确区分 strict 1920×1080 与 LF4 bounded 0.5x..2.0x；
- LF4 live run 要求显式 ProtectedMonitor、已选择的单显示器 ROI 和与 ROI monitor 一致的 ExperimentMonitor；
- metadata preset monitor identity 冲突在构造 `DecoderConfig` 时失败；
- UI 不把“bounded ROI”写成“已定位成功”，continuous locator 仍是 exact canvas/scale 的 authority。

CLI：

```text
PixelBridgeDecoder --headless-receive --output-dir DIR --backend wgc|dxgi \
  --profile remote-lf4 --channel remote --remote-provider NAME \
  --roi LEFT TOP RIGHT BOTTOM --timeout 1..600 \
  --protected-monitor DEVICE --experiment-monitor DEVICE
```

同一 token 也可显式用于 `--headless-replay`，但 offline Replay 不伪造 live monitor safety。live LF4 可选择 bounded production Replay fanout；旧 `remote` 的 diagnostic capture-only 语义保持不变。

## 4. 屏幕安全 authority

Step 17 没有再实现第二套显示器判定。GUI、CLI、runtime 与 Gate 共用 `MonitorSafetySelection` 和以下现有公共检查：

- `ResolveMonitorSafetySelection()`：从当前枚举 catalog 按 exact device identity 解析两块显示器；
- `ValidateMonitorSafetyTarget()`：拒绝 same monitor、target 超出 ExperimentMonitor、与 ProtectedMonitor 相交或 HMONITOR 不匹配；
- `RevalidateMonitorSafetySelection()`：重新枚举并要求 identity、physical/work RECT、DPI、rotation、refresh 与初始快照一致。

Encoder 和 live Decoder 都在 worker 真正启动后才把 `monitorSafety.preflightPassed` 从 `false` 改为 `true`，初始状态是 `Pending` 而不是提前报告 PASS。运行期间每秒重验证；异常路径将状态改为 `FAIL`。Encoder 还会重新读取 Data Window 的 physical client RECT，确保窗口没有离开 ExperimentMonitor、改变尺寸或触碰 ProtectedMonitor。

`PixelBridge.RunReport.2` 和 evidence journal 现在共同记录：

```json
"monitorSafety": {
  "preflightPassed": true,
  "revalidationCount": 4,
  "status": "PASS"
}
```

这只是当前 endpoint 的实时安全事实；它不是远端 Computer B/A 的包一致性证明，也不等同于 provider certification。

## 5. 实际闭环发现并修复的结果排序问题

第一次公开 CLI 本地像素闭环保存于：

```text
build-p1_5-evidence/step17-public-local-pixel-loop-20260902-072501
```

Encoder 正常广播，但 Decoder 以 `PBTelemetry RecordCapture failed: ObservationOrder` fail closed，未验证 digest、未发布文件。证据显示 WGC 已捕获并成功解析 Bootstrap，因此问题不是 profile exposure 或屏幕安全。

根因是 LF4 的两阶段 completion：较早的 observation 仍在等待 GPU demod 时，较晚的同 sequence duplicate 可以只经过 Bootstrap 就立即形成 `TelemetryOnly`。旧结果环按“completion 时间”而非 `captureObservation` 交付，因此较晚结果可能先进入 `PBTelemetry`；Telemetry 的单调性检查正确地拒绝了它。

修复位于 `PBDemodD3D11::CaptureDemodulator`：

1. 每个 bounded slot 在提交时、在任何后续结果可见前发布非零 `pendingCaptureObservations` 排序屏障；
2. completion 在同一 mutex 临界区中清除 pending observation 并入队结果；
3. `TakeResult()` 从固定容量结果环选择最小 observation；若仍有更早 observation pending，则暂不交付；
4. cancel、expiry、epoch invalidation 与 error 会明确退休其 pending barrier；
5. 结果环容量、drop counter 与原有资源上界不变，没有加入无界 reorder buffer，也没有放松 Telemetry。

新增 WARP 回归用例故意让 observation 2 的 duplicate suppression 先完成，断言 observation 1 完成前取不到 observation 2，随后严格按 1、2 交付。完整 `PBDemodD3D11Tests`、Release 实屏闭环和 ASan 均通过。

## 6. 自动化验证

### 6.1 Release build

以下目标在 `build-presentation-release` 成功重建：

- `PixelBridgeEncoder`
- `PixelBridgeDecoder`
- `PBPresentationGate`
- `PBRemoteVisualLf4DynamicPresenter`
- `PBApplicationTests`
- `PBQSettingsTests`
- `PBDemodD3D11Tests`
- `PBTelemetryTests`
- `PBCaptureNormalizeTests`
- `PBRealCaptureReplayTests`

### 6.2 Release tests

```powershell
ctest --test-dir build-presentation-release -C Release --output-on-failure `
  -R "^(PBApplicationTests|PBQSettingsTests|PBDemodD3D11Tests|PBTelemetryTests|PBCaptureNormalizeTests|PBRealCaptureReplayTests)$" -j 1
```

结果：`6/6 PASS`，`181.02 s`。其中完整 `PBDemodD3D11Tests` 为 `142.88 s`。

公开参数/profile/dynamic presenter 定向集合：`87/87 PASS`，`5.84 s`；与 Application/QSettings 合计为 Step 17 parser/model 集合 `89/89 PASS`。

`PBRemoteVisualReport`：`24/24 PASS`，并通过 Python 3.12 `py_compile`。

### 6.3 MSVC ASan

同六个核心测试在 `build-presentation-asan` / `RelWithDebInfo` 串行执行：`6/6 PASS`，`433.19 s`。完整 `PBDemodD3D11Tests` 为 `346.83 s`；未见 sanitizer finding。

## 7. 最终原生屏幕安全 Gate

最终 Release 原生证据：

```text
build-p1_5-evidence/step17-final-native-20260902-080514/
  lf4-production-encoder-41176-1170484902017/
```

结果：

- `ProtectedMonitor = \\.\DISPLAY1`；
- `ExperimentMonitor = \\.\DISPLAY2`；
- 固定 Data Window 位于右侧显示器；
- 两个完整 Carousel cycles，9 个 source replacements，79 个 repeat Presents；
- watchdog report 为 PASS；
- 前台 PID 在 Gate 前后均为 `63784`；
- `gate.txt` SHA-256：`D3500040ED1DD6ECED7AF5C98FFD03A94C4C6A456DB560713BF6074497828FEF`。

公开 Encoder CLI 另以 `--profile remote-lf4` 运行 2 秒：exit 0、11 个 source replacements、5 Hz、0 dwell violation、monitor safety PASS/4 次 revalidation。证据：

```text
build-p1_5-evidence/step17-final-public-cli-20260902-080529
```

## 8. 最终实际捕获像素闭环

最终树对应证据：

```text
build-p1_5-evidence/step17-final-local-pixel-loop-20260902-080450
```

执行边界是：右侧 Data Window → Windows compositor → WGC selected ROI → PB-owned texture → LF4 GPU demod → temporal admission → production Receiver/Outer → WholeFileDigest → safe publish。Encoder 与 Decoder 没有 socket、pipe、shared memory、clipboard、临时文件交换或其它 payload IPC；两端唯一数据联系是 Decoder 实际捕获的像素。

关键结果：

| 项目 | 结果 |
| --- | --- |
| Encoder / Decoder exit | 0 / 0 |
| Decoder state | `Completed` |
| WholeFileDigest / final publish | `true` / `true` |
| source / published length | 1 / 1 byte |
| source / published SHA-256 | `6922E93E3827642CE4B883C756B31ABF80036649D3614BF5FCB3ADDA43B8EA32` / 相同 |
| WGC arrived / copied / delivered | 128 / 66 / 66 |
| Telemetry capture / Bootstrap success | 63 / 63 |
| raw FEC accepted / temporal admitted | 4 / 4 |
| Outer unique symbols | 1 |
| duplicate / reordered sequences | 52 / 0 |
| result queue HWM | 2 |
| Encoder/Decoder monitor revalidation | 10 / 4，均 `PASS` |
| foreground PID before / after | 63784 / 63784 |

artifact seals：

- `encoder-report.json`：`CD4981A74F40F09FDB35F519D5796897D2B857BFC70847AD87B8254B090D3858`
- `decoder-report.json`：`836196C28BF2BF642CF952E50B4B92A91C56FE943C7A0AE0CF345B2C79E7FA60`
- `verification-summary.json`：`BC51C5AE447DAFF2F9F6D4A915E5DA4911D18322E1A51AC076756B2BF0EF2359`

这条 1-byte 本机实屏链证明公开产品绑定已经连接到正确的 production Receiver 与发布语义；它不是跨 Computer B/A 的 provider field certification，也不能作为真实链路 goodput 数据。

## 9. 保留边界与下一步

Step 17 仍不证明：

- Computer A/B 使用同一个 tested package、DLL set、source set 与环境 fingerprint；
- 真实双机远控链上的 1/2/5 Hz goodput、scale/provider/backend matrix；
- 同提交 LocalDesktop WGC/DXGI × Direct/Shape 无性能回归；
- 6 小时 bounded soak；
- LF4、LF5 或任何 RemoteVisual Certified Profile。

因此下一步是 Step 18：生成 create-only 便携包，冻结 package/source/environment manifest，并用 tamper negative 与 clean-directory smoke 证明两端复制前后 identity 完全一致。
