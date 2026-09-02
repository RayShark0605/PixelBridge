# PixelBridge RemoteVisual Step 14 Four-Codeword Receiver Admission

状态：**DONE。LF4同一视觉帧的四个QC-LDPC codeword现在通过Step 13的admitted-index contract进入现有production `ReceiverPipeline`与`ReceiverIngress`，而不是建立LF4专用Outer/Receiver协议。每个codeword仍独立执行canonical Transport parse、SessionTag、segment/Outer identity、resource policy和conflict规则；同sequence duplicate refinement只提交此前缺失的slots，raw重复blocks不重复mutation。一个headless hidden production probe分别用DirectRepeat 1-byte文件和Wirehair V2 4096-byte文件，从production LF4 `SenderFrameBuilder` raster与reference Transport truth开始，经真实Receiver/Outer、PBStorage、WholeFileDigest和no-overwrite publish恢复逐字节一致文件。32个suppressed duplicate和一次partial-frame refinement均未增加Outer unique或verified bytes。**

本步骤没有把LF4加入公共Decoder配置、GUI或CLI；该暴露仍必须等待Step 15/16证据与Step 17 Gate。这里的完成结论是production Receiver admission和最终文件语义已闭环，不是实际远控双机field certification。

## 1. 单一production Receiver路径

`ReceiverPipeline::Process`现在区分两层事实：

1. `DemodFrameResult`报告本次visual/GPU observation里通过FEC、padding、CRC和identity的raw Transport/Control candidates；
2. `CaptureDemodulatorResult`的admitted index arrays报告temporal gate允许本次真正mutate Receiver的carrier。

LF4 result必须携带非`NotApplicable` temporal disposition；缺少Step 13 admission证明会立即失败，不能通过legacy逻辑绕过。具体规则为：

- `DuplicateSuppressed`、`Reordered`、`StaleCompletion`在Receiver前返回，零mutation；
- `DuplicateRefinement`只有存在新admitted Transport或一个admitted Control时才可继续；
- `Unique`是唯一增加frame-level FEC/FER与identity/false-accept evaluation分母的observation；
- `TelemetryOnly`必须同时具有零admitted Transport和零admitted Control；
- LF4 `ControlRecord`必须恰有一个admitted Control；
- strict旧profile继续把raw accepted entries全部映射为admitted entries，wire和行为保持兼容。

旧`RemoteVisualResilient`的application-level duplicate gate保留，但也被收紧为每sequence最多消费一次duplicate observation；LF4不依赖它，LF4的retry authority唯一位于CaptureDemodulator temporal gate。

## 2. Four-codeword Transport contract

### 2.1 独立candidate，不要求四个同时成功

`ProcessTransport`先独立检查两个上界：

- raw accepted count不得超过固定`acceptedTransportBlocks`容量；
- admitted count不得超过admitted index容量或raw accepted count。

随后用固定`processedIndices` bit set逐个验证admitted index：index必须落在raw count内且本result内不可重复。只有被索引的block进入下列现有production流程：

```text
accepted bytes
  -> ParseTransportBlock
  -> byteCount / minimum / payload bounds
  -> Bootstrap-bound SessionTag check
  -> zero-padded fixed ReceivedTransportBlock
  -> ReceiverIngress::ReceiveDataBlock
  -> DirectRepeat or Wirehair V2 decoder
  -> completed segment storage
  -> WholeFileDigest
  -> final publish
```

四个codeword彼此独立：0、1、2、3或4个均可通过；失败slot为erasure，不能要求“一帧四个全成才接受”。同时，它们必须来自同一captured observation的同一Bootstrap/FrameSequence binding；Step 12保证same texture/epoch，Step 13保证duplicate refinement只补缺失slot。

### 2.2 duplicate、conflict与resource semantics

admitted Transport counter只按admitted count增长，不按raw count增长。ReceiverIngress继续作为Outer identity的唯一authority：

- same `(SessionTag, SegmentOrdinal, OuterBlockId)`且payload相同为duplicate，不增加unique/progress；
- 同key但不同有效payload为conflict，fail closed并增加`outerConflictRejections`；
- descriptor/session未知、orphan cache超限、active decoder/bytes/segment资源超限均使用既有`ReceiverResourcePolicy`；
- resource/unknown-session类拒绝可观测但不把carrier提升为accepted output；其它protocol conflict终止run；
- completed segment只在Outer decoder返回完整、尺寸受限的encoded bytes后存储；
- `.part`不会在WholeFileDigest之前发布，existing final target不会被静默覆盖。

这意味着LF4只提高一帧内可供Receiver检查的candidate数量，不修改Outer key、不使用latest-wins，也不扩大Receiver缓存语义。

## 3. LF4 Control copy collapse

LF4一个Control raster在四个Robust codeword位置重复相同control window。底层`DemodFrameResult`固定容纳四个raw `AcceptedRemoteControlBlock`，避免旧单元素数组在production LF4路径中越界。temporal gate要求四份内容逐字段/逐字节相同，并只输出一个admitted Control index和一份`controlBytes`：

- 4个相同copy -> 1次production `ProcessControlRecord`；
- 任意copy冲突 -> completion fail closed；
- duplicate frame -> 0次Control mutation；
- Control与Transport混合 -> fail closed。

因此Control repetition提供信号冗余，不会使Session/Segment/FinalManifest被同一视觉帧重复处理四次。

## 4. Hidden production end-to-end probe

`DecoderRuntimeTestAccess::ProbeRemoteVisualLowFpsReceiver`是窄、headless、无GUI/CLI exposure的测试缝。它没有实现第二套Receiver；使用的组件均为product runtime实际对象：

- production `DescribeSource`与`SenderFrameBuilder`；
-冻结LF4 profile和完整production raster；
- `ReferenceChannel` Transport truth boundary；
- production `ReceiverPipeline`；
- `ReceiverIngress`与默认`ReceiverResourcePolicy`；
- DirectRepeat/Wirehair V2；
- PBStorage、WholeFileDigest与final publish。

probe输入限定为1..65536 bytes，duplicate burst限定为1..256，carousel最多4096帧；无效输入在改变output snapshot前拒绝。它执行以下故意对抗序列：

1. 正常处理Session Control；
2. 注入32个同Bootstrap/sequence的`DuplicateSuppressed/TelemetryOnly`结果；
3. 找到第一个LF4 data frame，将其第一次unique observation裁为raw/admitted slots `[0,1]`，并记录2个FEC failures；
4. 对同FrameSequence构造`DuplicateRefinement`，raw observation仍含4个blocks，但admitted仅为`[2,3]`；
5. 继续production carousel直到Receiver authoritative completion；
6. 要求WholeFileDigest verified、final publish succeeded，并从磁盘重新读取published file逐字节比较source。

该序列精确证明raw observation和Receiver mutation的差别：

```text
raw accepted Transport blocks       = 2 + 4 = 6
temporally admitted Transport blocks = 2 + 2 = 4
production accepted Transport blocks = 4
```

第一次unique是唯一计入data-frame evaluation的observation，因此`evaluatedDataFrames=1`、`postFecFailedFrames=1`；duplicate refinement恢复payload但不把FER分母改写为第二帧。

## 5. DirectRepeat与Wirehair V2 authoritative结果

| case | source | Outer mode | processed results | suppressed / refinement | raw / admitted Transport | Outer unique | final authority |
| --- | ---: | --- | ---: | ---: | ---: | ---: | --- |
| tiny | 1 byte | DirectRepeat | 37 | 32 / 1 | 6 / 4 | 1 | Completed；digest PASS；publish PASS；external bytes exact |
| normal | 4096 bytes | Wirehair V2 | 37 | 32 / 1 | 6 / 4 | 4 | Completed；digest PASS；publish PASS；external bytes exact |

两例共同断言：

- `duplicateFrameSequences=33`（32个suppressed + 1个refinement）；
- `endToEndUniqueFrameSequences=1`；
- `acceptedTransportBlocks=4`；
- `outerConflictRejections=0`、`outerResourceRejections=0`；
- `verifiedRawBytes == originalFileBytes`、`remainingRawBytes=0`；
- published path的实际文件内容逐字节等于输入。

DirectRepeat的Outer unique为1，Wirehair V2为4，证明四codeword可以向不同Outer IDs贡献进度，但duplicate burst和raw重复slots均没有膨胀unique count。

## 6. Negative与既有Receiver invariants

Step 14没有复制或弱化既有Receiver测试。完整`PBOuterFecTests`和`PBReceiverTests`继续覆盖：

- DirectRepeat与Wirehair V2 descriptor/dimension validation；
- duplicate block与same-ID conflicting payload；
- unknown session、descriptor-before-data与orphan cache policy；
- active segment/decoder count、encoded bytes、block count和Wirehair backend resource rejection；
- codec create/decode/recover error mapping与reservation exact release；
- short tail、padding、recovery尺寸与digest；
- resume persistent state的truncated/corrupt/resource/no-partial-mutation；
- completion、restart和cleanup后active decoder/cache reservation归零。

新增production LF4 probe还验证空source、零duplicate或越界参数在output不变的条件下失败。Receiver-facing admitted index的越界或重复会在任何`ReceiveDataBlock`调用前失败，防止malformed internal result被当成可信数组索引。

## 7. 验证结果

### 7.1 Targeted Release

```text
PBApplicationTests "[lf4][receiver]"   55 assertions / 1 case PASS
PBDemodD3D11Tests "[temporal]"       1,352 assertions / 1 case PASS
```

生产链路probe中的DirectRepeat和Wirehair V2两个SECTION均达到最终文件逐字节验证；`PBOuterFecTests`与`PBReceiverTests`完整套件同步通过。

### 7.2 Full affected Release / ASan

与Step 13共用的最终门禁：

- Release：`PBApplicationTests`、`PBRemoteVisualTests`、`PBOuterFecTests`、`PBReceiverTests`、`PBDemodD3D11Tests`、`PBTelemetryTests`，6/6 PASS，137.83秒；
- MSVC ASan/RelWithDebInfo：同6套，6/6 PASS，333.03秒；无ASan finding；
- `git diff --check`：PASS。

## 8. 完成出口与truth boundary

Step 14完成出口逐项闭合：

- 同frame四个codeword独立FEC/Transport candidates：PASS；
- 0..4 success矩阵与same-frame slot binding：PASS；
- admitted indices独立bounds/duplicate validation：PASS；
- duplicate refinement只补此前缺失slots：PASS；
- 四份Control copy一致性检查并折叠为一次mutation：PASS；
- DirectRepeat与Wirehair V2 production Receiver convergence：PASS；
- duplicate不增加Outer unique/progress/verified bytes：PASS；
- conflict、orphan、resource policy与codec cleanup regressions：PASS；
- WholeFileDigest、final publish与external byte comparison：PASS；
- invalid probe no-output-mutation、Release与ASan：PASS。

因此Step 14状态为`DONE`。最终文件truth仍严格是`Completed + WholeFileDigest verified + final publish succeeded + external bytes exact`，而不是中间provider、FEC或Outer成功。LF4公共profile选择仍被刻意拒绝；Step 15必须先让Replay v2用同一production decoder复现LF4 live result，Step 16再修正所有LF4 telemetry分母，Step 17之后才允许GUI/CLI exposure。
