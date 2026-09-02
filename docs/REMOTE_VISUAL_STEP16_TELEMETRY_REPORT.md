# RemoteVisual LF4 Step 16：权威遥测、RunReport.2 与严格双端合并

日期：2026-09-02  
状态：`DONE`  
范围：`PB-RemoteVisual-LF4-X1` 的运行期指标、两端 `PixelBridge.RunReport.2`、证据日志与 `PBRemoteVisualReport`。本步骤不公开 LF4 GUI/CLI 入口，不执行真实双机传输，也不改变任何 wire layout、FEC、Receiver、WholeFileDigest 或发布语义。

## 1. 本步骤关闭的事实边界

低刷新率链路里至少存在五种不能互相替代的速率：

1. 冻结 profile 的理论容量；
2. Encoder 实际生成逻辑帧与 Outer-FEC payload 的速率；
3. Data Window 的 Present 调用和完整逻辑画面替换速率；
4. Decoder 实际捕获、识别为新视觉身份并进入 FEC/Receiver 的速率；
5. WholeFileDigest 与最终发布之后才成立的 verified goodput。

Step 16 不再让这些值共享一个含糊的“FPS/throughput”分母。Sender 仍然不能观察 Receiver progress、ETA 或 verified goodput；Encoder report 中这三个字段继续严格为 `null`。

## 2. Encoder 容量与实际生成速率

`EncoderSnapshot` 与 `RunReport.2.capacityModel` 明确记录冻结 LF4 常量：

| 指标 | 每个完整逻辑帧 | 说明 |
| --- | ---: | --- |
| raw visual coded bits | 64,800 bit | 8,100 coded bytes，不是 payload |
| Inner-FEC information | 5,400 bytes | 4 × 1,350 bytes |
| Transport payload ceiling | 5,256 bytes | 4 × 1,314 bytes |

因此 1/2/5 Hz 的 Transport payload **理论上限**分别是 5,256 / 10,512 / 26,280 bytes/s。该值不扣除 Control repetition、Carousel 稀释、重复发送、远控丢失、FEC/CRC/identity rejection、Receiver 去重、Segment 验证和最终发布，不能命名为 goodput。

实际值采用两个独立分母：

- `generatedVisualFramesPerSecond`：`(N - 1) / (lastLogicalReplacement - firstLogicalReplacement)`；初始帧不冒充一个已经过去的 interval。观察不足时为 `null`，不是 0。
- `generatedPayloadBytesPerSecond`：已经放进 Data frame 的实际 Outer-FEC payload bytes / broadcast runtime；Control frame 贡献 0。它仍不是 Receiver verified goodput。

`presentedVisualFps` 与 `presentCallFps` 继续来自 presentation owner，不从上述两个生成速率推断。证据 journal 同样使用 nullable 数值并记录容量常量，避免准备阶段把 unavailable 写成 0。

## 3. Decoder 的三个独立 admission 域

### 3.1 signal-metric observation

每个实际完成 compact metric readback、且通过 profile 形状校验的 LF4 observation 贡献一次 `RemoteMetricSample`：

```text
metricSamples       = remoteMetricFrames × 64,800
symbolSamples       = remoteMetricFrames × 16,723
freshnessRegions    = remoteMetricFrames × 77
freshRegions + staleRegions = freshnessRegions
```

它记录 zero-magnitude、minimum/mean absolute metric、unreliable symbol、fresh/stale region、freshness tag mismatch/erasure，以及 stale region 在进入 LDPC 前擦除的数据 metric。一次允许的 same-sequence duplicate refinement 可以提供新的独立 signal observation，所以它可以增加 metric 分母；terminal duplicate/reorder/stale completion 没有 metric sample。

### 3.2 unique FEC evaluation

只有 temporal state machine 判为 `Unique` 的 Transport frame 才调用 `RecordFec`：

```text
evaluatedCodewords = evaluatedDataFrames × 4
fecAcceptedTransportBlocks + fecFailures + crcFailures + identityFailures = evaluatedCodewords
```

`fecFrameErrorRate`、`fecCodewordFailureRate` 与 `fecAcceptedTransportBlockRate` 分别使用 frame、codeword 和 codeword 分母。same-sequence refinement 不重复增加 FER/FEC 分母。

production receive 没有 sender coded-bit truth 时，`comparedCodedBits=0`、`erroneousCodedBits=0` 且 `preFecBerEstimate=null`。只有显式存在非零 comparison denominator 时，0 BER 才写作数值 0。

### 3.3 Receiver-bound temporal admission

`fecAcceptedTransportBlocks` 表示 unique FEC observation 中原始通过的 codeword；`temporallyAdmittedTransportBlocks` 表示经 slot 去重后真正交给既有 Receiver 的 carrier。历史字段 `acceptedTransportBlocks` 保留兼容，但明确是后者的 alias。一次 refinement 可以让 temporal admission 大于 unique-FEC observation 内的 accepted 数量，这是受支持的语义，而不是计数错误。

Outer unique/duplicate/conflict 继续来自 `ReceiverIngress` 的权威 disposition。`verifiedEncodedGoodputBitsPerSecond` 只在 WholeFileDigest 和最终 publish 已成功后调用 `RecordVerifiedEncodedBytes`，不会从中间 Transport acceptance 推导。

## 4. `null`、0 与 epoch 语义

- denominator 为 0：对应 rate/mean/minimum 为 `null`；
- denominator 非 0 且 numerator 为 0：rate 必须是数值 0；
- CaptureEpoch 切换：signal、FEC、Bootstrap、capture 与 derived rate 一起清零，lifetime epoch count 保留；
- 相同或倒序 observation：`ObservationOrder`，不增加任何已提交计数；
- NaN、负 metric、`minimum > mean`、count 超 denominator：`InvalidSample`；
- 计数饱和：显式 `CounterOverflow/counterSaturated`，不继续把 derived rate 当作可靠证据。

`PBTelemetry` 是这些分母的单 owner；`ReceiverPipeline` 只把已通过 profile 和 temporal contract 的 observation 输入该 owner，不在应用层维护第二套 metric 聚合器。

## 5. RunReport.2 与严格 merger

Decoder report 新增并区分：

- frozen profile ID/layout/data bytes/codewords；
- evaluated frame/codeword、FEC/CRC/identity disposition 与 rate；
- raw FEC accepted 和 Receiver-bound temporal admitted；
- LF4 metric、symbol unreliable、fresh/stale、erased metric 的 numerator/denominator/rate；
- unavailable reason 与 production truth-oracle 边界。

Step 21 后续以不改变 `RunReport.2` decisive Receiver/WholeFileDigest 语义的方式追加了 `observedLocatorGeometry`：每个成功 Bootstrap 从实际捕获像素保存 locator origin、X/Y scale、marker residual、last/min/max、最大 scale anisotropy 和样本数，authority 固定为 `AcceptedBootstrapLocatorPixels`。样本数必须逐值等于 `telemetryBootstrapSuccesses`；零样本时全部测量值必须为 `null`，CaptureEpoch 切换时与 Bootstrap 分母一起清零。该对象用于阻止 evidence 工具把任意宽高比 capture ROI 或 metadata `estimatedScaleX/Y` 冒充实际视频几何，不改变 FEC、temporal admission、Receiver、WholeFileDigest 或 publish 分支。

`tools/PBRemoteVisualReport/pb_remote_visual_report.py` 对 LF4 fail closed：

1. 固定 `VisualProfileId=0x504252564C463431`、`LayoutVersion=7`、8,100 bytes、4 codewords 和全部容量常量；
2. descriptor known 时比较两端 128-bit `SessionId`、`SessionTag`、source length 与 WholeFileDigest；
3. 独立校验每个 FEC、metric、symbol、freshness numerator/denominator 和 `null` 语义；
4. 比较 provider/version/mode/target/chroma/resolution 与有界时间窗；
5. 对 source 与 published output 做稳定文件 SHA-256/length；
6. 重新严格读取并封印两端原始 report 文件，确认文件内容就是传入的 endpoint object；
7. 只有 Decoder `Completed`、WholeFileDigest PASS、publish PASS、external match、journals complete、identity complete、Outer conflict 为 0，且两端 report seals 都存在时，才允许 `successfulRun=true`。

只传两个内存 dictionary 可以生成诊断 combined report，但不能成为成功证据。每次 CLI 调用预检并 create-only 发布：

```text
remote-run-<RunId>-combined.json
remote-run-<RunId>.md
remote-run-<RunId>.csv
```

可选 `--summary-csv` 仍是跨 run append-only 索引，不能代替单次运行 CSV。Markdown/CSV 均把 ceiling、generation、presentation、capture/unique、FEC、temporal admission 和 verified goodput 分列显示。

## 6. 验证结果

所有验证均为纯命令行/headless；没有启动 GUI、native capture 或 Data Window，没有访问或改变任何显示器内容。

### Release

```powershell
cmake --build build-presentation-release --config Release --target PBTelemetryTests PBApplicationTests --parallel 4
ctest --test-dir build-presentation-release -C Release -R '^(PBTelemetryTests|PBApplicationTests)$' --output-on-failure
```

结果：2/2 PASS，27.51 s。`PBApplicationTests` 包含 6-frame LF4 live/offline production Replay，两侧 metric/FEC/Receiver/WholeFileDigest/publish 逐项一致。

```powershell
$env:PB_REMOTE_VISUAL_REAL_REPLAY_ROOT = (Resolve-Path 'build-p1_5-evidence\step02-rdp-20260901').Path
build-presentation-release\tests\PBApplication\Release\PBApplicationTests.exe '[real-replay][step15]'
```

结果：84 assertions PASS。Direct/Shape/LF4 三份真实 receiver-only Replay 保持原 failure classification；LF4 新增的 aggregate denominator 为 1 × 64,800 metrics、1 × 16,723 symbols、1 × 77 freshness regions，且没有把 descriptor-absent Transport success 提升为文件成功。

Release `PixelBridgeEncoder`、`PixelBridgeDecoder`、`PBRemoteVisualLf4DynamicPresenter` 与 `PBPresentationGate` 均重新编译成功。`PBPresentationGate` 只做编译验证，没有在用户无人值守期间运行需要真实显示器的 native Gate。

### MSVC ASan

```powershell
cmake --build build-presentation-asan --config RelWithDebInfo --target PBTelemetryTests PBApplicationTests --parallel 4
ctest --test-dir build-presentation-asan -C RelWithDebInfo -R '^(PBTelemetryTests|PBApplicationTests)$' --output-on-failure
```

结果：2/2 PASS，67.97 s；同一真实 Replay 条件用例另行运行，84 assertions PASS，无 sanitizer finding。`PBPresentationGate` ASan target 也编译成功但未运行 native display Gate。

### Merger 与静态检查

```powershell
D:\Python3.12.9\python.exe -m unittest -v test_pb_remote_visual_report.py
D:\Python3.12.9\python.exe -m py_compile pb_remote_visual_report.py test_pb_remote_visual_report.py
git diff --check
```

结果：23/23 Python tests PASS；覆盖双端 seal、SessionId、provider/time/profile mismatch、LF4 frozen constant、denominator 差 1、0/null、external mismatch、create-only 三格式输出与重复发布拒绝。`py_compile` 和 `git diff --check` PASS。

## 7. 未被本步骤证明的内容

- LF4 仍未加入 product GUI/CLI；Step 17 才能改变 exposure。
- 本步骤没有新的真实双机完整文件 run；真实 provider goodput 仍待 Step 20/21。
- WARP/Replay/单元测试不等于 field certification。
- 没有 6 小时 soak、LocalDesktop 同提交物理回归或 Certified Profile 结论。
- `highConfidenceWrongCodewords` 在 production run 中继续为 `null`；只有带独立 truth 的 sealed Replay 才能进行该分类。

因此 Step 16 的工程出口已关闭，但 `certifiedRemoteVisualProfile` 必须继续为 `false`。
