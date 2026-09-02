# PixelBridge RemoteVisual Step 13 Temporal Admission / Bounded Queue

状态：**DONE。`PB-RemoteVisual-LF4-X1` 现已在 `CaptureDemodulator` 内、GPU demod 与 production Receiver 之前建立以像素内 Bootstrap 为权威的 temporal admission。`(CaptureEpoch, SessionTag, FrameSequence, canonical Bootstrap)` 共同绑定当前视觉身份；reordered observation 不提交 GPU，已完整恢复的 duplicate 不提交 GPU，同序列失败 observation 只允许配置限定的有限 refinement，且只把此前未接受的 codeword slot 暴露给下游。gap/skipped、duplicate、reorder、retry/recovery/limit-drop、stale completion、queue HWM 和 stall 均有独立权威计数。32 帧连续 duplicate 不增加 GPU submission、accepted Transport、FER/verified frame 或队列 HWM；新唯一帧与新 epoch 随后均可继续接受。**

本步骤只关闭 temporal identity、duplicate refinement 和有界排队语义。它不把重复像素解释成新 payload，不改变既有 Transport/Outer wire format，也不表示 LF4 已对 GUI/CLI 开放。Receiver、WholeFileDigest 与发布闭环由 Step 14 单独证明；Replay v2 live/offline 一致性仍属于 Step 15。

## 1. 权威身份与共享 tracker

`PBModulation` 新增无分配、固定状态的 `VisualIdentityTracker`，并让 application 层复用同一实现，不再由 product runtime 和 capture integration 各自维护近似但可能漂移的 sequence 规则。

它的输入为：

```text
FrameSequence + CaptureEpoch + monotonic timestamp + optional stream identity
```

LF4 的 stream identity 是从当前 captured pixels 中解析出的 Bootstrap `SessionTag`，不是文件名、窗口状态、capture observation 序号或 caller 注入的隐藏身份。规则如下：

1. `CaptureEpoch == 0` 或负时间戳为 `Invalid`；
2. 首个 observation、epoch 变化或 `SessionTag` 变化建立新 baseline，并计为 `Unique`；
3. sequence 等于当前 maximum 为 `Duplicate`；
4. sequence 小于当前 maximum 为 `Reordered`，不回退 baseline；
5. sequence 大于 maximum 为 `Unique`；距离大于 1 时增加一个 gap event，并按 `distance - 1` 累加 skipped sequences；
6. unique FPS 只使用相邻 sequence 且时间单调递增的 interval；gap、duplicate、reorder 不污染该分母；
7. epoch reset 只清 baseline，不清累计 run counters，因而既能避免跨 epoch 合并，又保留整次运行的诊断证据。

所有计数使用 saturating arithmetic；tracker 不建队列、不保存历史帧，也不随运行时间增加内存。

## 2. CaptureDemodulator temporal state machine

### 2.1 固定状态与精确 identity

LF4 `CaptureDemodulator` 只保存一个当前 temporal frame：

- `CaptureEpoch`；
- Bootstrap `SessionTag`；
- `FrameSequence`；
- canonical 44-byte Bootstrap；
- 4-bit accepted Transport slot mask；
- 最多四个固定容量 accepted Transport blocks；
- 最多一个 accepted Remote Control block；
- bounded duplicate attempt count 与一个 `evaluationPending` bit。

因此内存为常量，不缓存 raster、metric history 或 duplicate observation。`SameTemporalIdentity` 要求上述 epoch/session/sequence/canonical Bootstrap 全部一致；仅仅“画面看起来相同”或 sequence 相同不足以合并。新 unique identity 会原子替换这一固定状态。

### 2.2 admission dispositions

每个 LF4 result 都明确携带以下 disposition：

| disposition | GPU work | Receiver-visible carrier | 语义 |
| --- | --- | --- | --- |
| `Unique` | 是 | 仅本次通过 FEC/CRC/identity 的 slots | 新 identity 的首次 observation |
| `DuplicateRefinement` | 有界地是 | 仅此前缺失、此次新恢复的 slots | 同 identity 的有限恢复机会 |
| `DuplicateSuppressed` | 否 | 无；`TelemetryOnly` | 已 terminal、已有 evaluation pending、identity 不完全一致或 retry 已耗尽 |
| `Reordered` | 否 | 无；`TelemetryOnly` | sequence 小于当前 maximum |
| `StaleCompletion` | work 已在旧 identity 下完成 | 无 | completion 时当前 identity 已改变 |
| `NotApplicable` | 保持旧行为 | strict profile 暴露全部 raw accepted indices | 非 LF4 兼容路径 |

reordered 与可提前判定的 suppressed duplicate 在 Bootstrap stage 后立即结束，不进入 LF4 GPU data stage。这样重复 Present 或远控链路重复 delivery 只增加轻量 observation/counter，不占用 demod slot 的第二阶段，也不把延迟堆进 result queue。

### 2.3 bounded duplicate refinement

默认 `maximumDuplicateRefinementAttempts=1`，合法范围为 0..4；超过 hard maximum 在配置/预算阶段拒绝。一个 duplicate attempt 一旦被允许就会消耗额度，不因本次仍未恢复 carrier 而重新获得无限次机会。

允许 refinement 必须同时满足：

- epoch、SessionTag、FrameSequence 与 canonical Bootstrap 和当前 frame 完全一致；
- 当前没有另一个 evaluation pending；
- 当前 frame 尚未接受 Control，也尚未集齐四个 Transport slots；
- duplicate attempt 尚未达到配置上限。

完成时先复制当前 temporal frame 到固定大小 candidate，在 candidate 上验证全部 raw accepted blocks，再一次性提交状态。对每个 Transport block：

- slot 必须在 0..3；
- byte count 必须在固定 block capacity 内；
- Control 与 Transport 不得在同一 temporal identity 混合；
- 旧 slot 的逐字段/逐字节内容若相同则忽略，若不同则 `ConsumerFailure/Completion` fail closed；
- 只有此前 mask 中缺失的 slot 才写入 `admittedTransportBlockIndices`。

Remote Control 的 LF4 raster会产生四个相同 Robust copy；固定 cache要求它们完全一致并折叠成一个 admitted Control。任一 copy 冲突，或 Control 与已接受 Transport 混合，均在下游 mutation 前失败。fatal completion/submit/poll error 清空当前 temporal frame，避免错误后的半状态被后续 duplicate 延续。

### 2.4 raw observation 与 admitted mutation 分离

`DemodFrameResult` 仍报告本次 GPU observation 中全部 raw accepted blocks；`CaptureDemodulatorResult` 新增固定容量 admitted index arrays。下游必须根据 admitted indices 读取 carrier，不得从 raw count 推断 temporal mutation。

这一分离允许以下合法情况：

```text
Unique observation:             raw slots [0,1], admitted [0,1]
DuplicateRefinement observation: raw slots [0,1,2,3], admitted [2,3]
```

第二次 observation 的 slots 0/1 仍是正确 raw 诊断证据，但不能再次进入 Receiver。LF4 `verifiedFrames`、`postFecFailedFrames` 与 FER evaluation 只由 `Unique` observation 增加；duplicate refinement 即使恢复新 carrier，也不会被误计为又一独立完整 data frame。

## 3. Queue、stale-drop、stall 与 shutdown

本步骤复用而不是平行重建现有有界 capture pipeline：

- capture acquire queue、PB-owned ROI ring、demod pending slots和 result ring均有配置容量与 HWM；
- `resultQueueCapacity` 合法范围为 1..256，其完整 `CaptureDemodulatorResult` reservation 在启动前计入 resident budget；
- result ring满时按既有 stale/drop policy 丢 observation，不阻塞 capture owner；
- epoch invalidation丢弃旧 result ring内容并计入 `staleResultDrops`，随后 drain demod work；
- pending slot HWM不得超过 configured slot count；
- `DomainStarted`/`DomainInvalidated`重置 temporal baseline和单帧cache，但保留run counters；
- shutdown 后 capture/demod pending、result queue与GPU work必须全部归零。

application 层已有的 `ChannelStallTracker` 继续作为 1,000 ms authority。它把“capture observation 停止”与“capture仍到达但 unique visual不前进”分成 capture stall 和 visual stall：持续 duplicate会触发 visual stall而不是伪装为新数据；capture停止则按最后一次 observation关闭/归类已有visual stall。domain reset会关闭活动区间而不丢累计证据。

## 4. Deterministic temporal corpus

WARP integration corpus在真实两阶段 `CaptureDemodulator` 上逐 observation 驱动：

1. 首批44个 observation包含6个 unique、37个 duplicate、1个 reorder；从sequence 100跳到103，精确产生1个gap与2个skipped；
2. 两个失败unique分别验证“第一次duplicate恢复4 slots”和“第一次duplicate仍失败、第二次被limit suppress”；
3. 末尾32个同sequence duplicate burst全部在Bootstrap后抑制；
4. 总共仅8次GPU submission/completion，admitted Transport共20，而不是按44个 observation重复处理；
5. `verifiedFrames=4`、`postFecFailedFrames=2`，duplicate不增加两者；
6. pending HWM不超过2，result queue HWM为1、drop为0，结束时pending/queue为0；
7. domain invalidation后新epoch把相同sequence当成新baseline，再次得到4个admitted Transport；
8. 独立0..4 codeword矩阵用正确与wrong-SessionTag Transport混合，逐项证明admitted slots和identity failures；
9. LF4 Control frame的4个相同raw Robust copies只产生1个admitted Control，紧随duplicate被抑制。

application production Receiver corpus又在Session Control后注入32个 suppressed duplicate，随后将首个data frame拆为`Unique [0,1]`和同sequence `DuplicateRefinement [2,3]`。DirectRepeat与Wirehair V2两条路径的Outer unique count和最终publish均不受duplicate burst影响；详细闭环见Step 14文档。

## 5. 验证结果

### 5.1 Targeted Release

```text
PBApplicationTests "[stall]"                  30 assertions / 3 cases  PASS
PBApplicationTests "[lf4][receiver]"          55 assertions / 1 case   PASS
PBRemoteVisualTests "[temporal]"          257,633 assertions / 2 cases PASS
PBDemodD3D11Tests "[temporal]"             1,352 assertions / 1 case   PASS
```

MSVC ASan初次运行暴露了新增WARP测试把较大的`CaptureDemodulatorResult`置于测试函数栈帧、经ASan redzone放大后触发Windows stack overflow。修复只把该测试对象改为显式`std::unique_ptr`固定所有权，没有改变生产实现、断言或门禁；targeted ASan随后以1,370 assertions通过。

### 5.2 Full affected gates

```powershell
ctest --test-dir build-presentation-release -C Release --output-on-failure -j 2 `
  -R '^(PBApplicationTests|PBRemoteVisualTests|PBOuterFecTests|PBReceiverTests|PBDemodD3D11Tests|PBTelemetryTests)$'

ctest --test-dir build-presentation-asan -C RelWithDebInfo --output-on-failure -j 2 `
  -R '^(PBApplicationTests|PBRemoteVisualTests|PBOuterFecTests|PBReceiverTests|PBDemodD3D11Tests|PBTelemetryTests)$'
```

最终结果：

- Release：6/6 PASS，0失败，总时长137.83秒；完整`PBDemodD3D11Tests`为137.80秒；
- MSVC ASan/RelWithDebInfo：6/6 PASS，0失败，总时长333.03秒；完整`PBDemodD3D11Tests`为332.98秒，无sanitizer finding；
- `git diff --check`：无whitespace error。

## 6. 完成出口与限制

Step 13完成出口逐项闭合：

- `(CaptureEpoch, SessionTag, FrameSequence, canonical Bootstrap)` identity：PASS；
- duplicate/reorder/gap/skipped权威计数：PASS；
- reordered和terminal duplicate零GPU/零Receiver admission：PASS；
- failed same-sequence observation只允许bounded refinement：PASS；
- previously accepted slot相同则幂等、冲突则fail closed：PASS；
- duplicate不增加FER、verified frame、Outer unique或verified bytes：PASS；
- 32帧duplicate burst不增加内存/HWM并能恢复接受新unique：PASS；
- 1秒capture/visual stall分类与domain reset：PASS；
- queue/pending hard bounds、epoch stale-drop与shutdown drain：PASS；
- strict profile兼容、Release与ASan regression：PASS。

因此Step 13状态为`DONE`。本步骤的truth endpoint是“哪些已验证carrier可以进入下一层”，不是最终文件成功；WholeFileDigest和publish仍是最终authority。Step 14在这个admission contract上证明四codeword进入现有Receiver/Outer后仍保持唯一mutation与安全发布。
