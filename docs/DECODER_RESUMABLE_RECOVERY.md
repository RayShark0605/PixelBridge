# Decoder 可恢复乱序写入、确认门与 publish commit

本文记录 G03 与 G05 的当前实现合同：正式 Descriptor 绑定后的 Outer block 持久化、乱序 Segment 写入、completed Segment 重启重验、大输出确认门，以及 WholeFileDigest 后可跨进程恢复的安全发布。权威代码入口是：

- `apps/common/decoder_resume_store.h/.cpp`；
- `apps/common/local_desktop_runtime.cpp`；
- `libs/PBReceiver/include/pbreceiver/receiver_ingress.h`；
- `libs/PBStorage/include/pbstorage/output_file.h`。

本文不改变 Protocol 1.0 wire，不把 journal 当作 Sender 数据，也不宣称完成 Qt 对话框、真实 capture、20 GiB 或真实进程终止注入。G05 使用小 fixture 对同一 durable marker 状态进行确定性析构/重启模拟；Qt 具体交互属于 G16。

## 1. 文件身份与本地命名

收到并通过 `ReceiverResourcePolicy` 校验的正式 `SessionDescriptor` 后，Decoder 才考虑建立本地输出状态。若 `OriginalFileSize > MaxOutputPreallocationBytesWithoutPrompt`（产品默认 4 GiB），Qt-free controller 先公开 `AwaitingLargeOutputConfirmation`、`RunGeneration` 与 `RequestId`。确认前不创建/扩展 `.part`、不创建 `.resume`，也不把 Segment Control 或 Outer payload 送入 Receiver；同一 Session 的重复 descriptor 只对应同一个请求。拒绝后进入带原因的 `Stopped`，接受后才继续以下流程：

```text
<selected-output-directory>/PixelBridge-<SessionTag-16-lower-hex>.part
<selected-output-directory>/PixelBridge-<SessionTag-16-lower-hex>.resume
```

`.part` 与最终目标位于同一目录和卷。`.part` 从不使用 `FileNameUtf8`；不可信文件名只在通过 basename、UTF-8、Windows 保留名、ADS、traversal、尾随 dot/space 和长度校验后用于最终候选名。

最终名称候选固定为：

1. 已验证的原始 basename；
2. `<stem> (PixelBridge-<SessionTag>).<extension>`。

首个候选已存在时选择第二个；第二个也存在时返回 `TargetExists/Reservation`，不覆盖、不删除，也不创建 `.part`。选中的 UTF-8 basename 先作为 `OutputReservation` flush 到 journal，随后 PBStorage 才创建 `.part`；重启必须重新验证它确实是该 Session 唯一合法的 primary/collision 候选，因此目录状态变化不会偷偷改名。publish 前再次执行存在性检查，随后使用不带 replace 的同卷 `MoveFileExW(..., MOVEFILE_WRITE_THROUGH)`，因此检查与 rename 之间的竞态也只能失败，不能覆盖。

创建/恢复输出时同时查询目标卷可用空间、`FILE_STANDARD_INFO.AllocationSize`、文件 sparse/compressed 属性以及卷的 sparse/compression capability。快照分别记录 logical/requested bytes 与 actual allocation bytes，`PreallocationFullyAllocated` 只由实际 allocation 比较得出，不从逻辑长度推断。

## 2. PBJH/PBJR journal schema

所有整数均为 little-endian。journal 是 Decoder 本地格式，不是视觉 wire。当前版本为 1。

### 2.1 文件头 `PBJH`

| Offset | Bytes | 字段 | 约束 |
| ---: | ---: | --- | --- |
| 0 | 4 | magic | ASCII `PBJH` |
| 4 | 2 | version | `1` |
| 6 | 2 | reserved | `0` |
| 8 | 8 | SessionTag | 必须等于当前像素 Session |
| 16 | 4 | SessionControlBytes | 非零、受 `MaxResumeBytes` 约束 |
| 20 | 4 | reserved | `0` |
| 24 | N | canonical SessionDescriptor ControlRecord | 必须完整解析、SessionId 派生 tag 必须一致 |
| 24+N | 4 | CRC32C | 覆盖此前整个 header，不含 CRC 字段 |

总长度为 `28 + N`。当前 SessionDescriptor ControlRecord 与既有 journal 不逐字一致时拒绝恢复，不进行 latest-wins。

### 2.2 append record `PBJR`

| Offset | Bytes | 字段 | 约束 |
| ---: | ---: | --- | --- |
| 0 | 4 | magic | ASCII `PBJR` |
| 4 | 2 | version | `1` |
| 6 | 2 | RecordType | 见下表；未知类型拒绝 |
| 8 | 4 | TotalBytes | `36 + PayloadBytes` |
| 12 | 4 | PayloadBytes | 与 `TotalBytes` 精确一致 |
| 16 | 8 | Generation | 非零，按文件顺序严格递增 |
| 24 | 8 | reserved | `0` |
| 32 | N | payload | 按类型解析并受 descriptor/policy 约束 |
| 32+N | 4 | CRC32C | 覆盖 record header 与 payload，不含 CRC 字段 |

RecordType：

| 值 | 名称 | payload |
| ---: | --- | --- |
| 1 | SegmentControl | 完整 canonical `SegmentDescriptor` ControlRecord |
| 2 | ManifestControl | 完整 canonical `FinalManifest` ControlRecord |
| 3 | AcceptedBlock | 下述 20-byte header + fixed padded payload |
| 4 | CompletedSegment | 下述固定 72 bytes |
| 5 | OutputReservation | 已校验并绑定到该 Session 合法候选的 UTF-8 最终 basename |
| 6 | PublishIntent | 固定 32-byte WholeFileDigest；只能在 reservation、manifest 与全部 completed records 耐久后出现 |

`AcceptedBlock` payload：

| Offset | Bytes | 字段 |
| ---: | ---: | --- |
| 0 | 8 | SegmentOrdinal |
| 8 | 4 | OuterBlockId |
| 12 | 2 | DeclaredPayloadBytes |
| 14 | 2 | reserved=`0` |
| 16 | 4 | PaddedPayloadBytes |
| 20 | N | PaddedPayload |

加载时要求 SegmentDescriptor 已绑定、Segment 未 completed、`PaddedPayloadBytes == descriptor.OuterBlockBytes`，并同时满足 `MaxOuterBlockBytes`。相同 `(SegmentOrdinal, OuterBlockId)` 只有 declared length 与整个 padded payload 完全相同才幂等；不同内容拒绝整个恢复。

`CompletedSegment` payload：

| Offset | Bytes | 字段 |
| ---: | ---: | --- |
| 0 | 16 | SessionId |
| 16 | 8 | SegmentOrdinal |
| 24 | 8 | RawOffset |
| 32 | 8 | RawSize |
| 40 | 32 | RawDigest |

这些字段必须逐项等于已绑定 SegmentDescriptor。completed record 出现后，同 Segment 的 accepted cache 从快照中移除。

### 2.3 torn-tail 与内部损坏

只允许忽略最后一个“可证明仍可能成为合法 PBJR”的 EOF 截断前缀：已出现的 magic、version、type、length、generation、reserved 字节必须合法；完整 header 声明的 `TotalBytes` 必须大于实际剩余字节。随后立即截断旧尾并 compact。

以下情况不是 torn tail，必须拒绝整个 journal：

- 任意随机尾字节或错误 `PBJR` 前缀；
- 完整 header 的 type、length/payloadLength、generation 或 reserved 非法；
- 完整 record 的 CRC32C 不匹配；
- 文件中间的截断、未知 record、descriptor/payload 冲突；
- active Segment 数量超过本地 decoder 上限；
- record/payload/journal 总大小超过 `ReceiverResourcePolicy`。

## 3. checkpoint 与 compaction

### 3.1 accepted block

只有 PBReceiver 已完成 Transport shape/CRC 上游校验、descriptor 已绑定且 Outer decoder 返回 `Unique` 的 block 才进入 journal。descriptor 前的 block 仅进入有界 orphan cache；descriptor 绑定或后续 Carousel 在资源释放后 drain 时，PBReceiver 返回实际重放并唯一接纳的拥有型 block 列表，应用层再按 descriptor 记录它们。

accepted block 先进入内存 pending 列表，以下任一条件触发 checkpoint：

- 单 owner 接收循环达到 1 秒期限；
- Segment 即将写入 completed；
- 显式 checkpoint。

checkpoint 逐条 append，最后调用 `FlushFileBuffers`，成功后才清空 pending。journal 达到 16 MiB 时执行 compact。

### 3.2 compact snapshot

Segment completed 或 journal 达到 16 MiB 时，构建仅含当前 authoritative state 的 snapshot：

```text
PBJH
+ output reservation（存在时）
+ unique SegmentControl records
+ optional FinalManifest
+ completed Segment records
+ only incomplete active accepted blocks
+ publish intent（存在时，必须为最后的 authoritative 状态）
```

snapshot 写入同目录 `.resume.tmp`，完成 write、`FlushFileBuffers`、close 后，使用 `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH` 原子替换 journal；generation 不回退。旧 journal 在替换成功前仍是 authoritative state。

## 4. Segment 完成状态机

固定顺序如下，任何一步失败都不得提前释放 active decoder：

| 阶段 | authoritative operation | 失败后状态 |
| --- | --- | --- |
| OuterReady | PBReceiver recover exact `EncodedSize` | decoder 与 accepted-ID conflict history 保留 |
| EncodedVerified | BLAKE3 对比 `EncodedDigest` | Session fail closed |
| BoundedDecoded | RAW exact copy 或受 input/output/window 限制的 zstd 解压 | 不写 `.part` |
| RawVerified | BLAKE3 对比 `RawDigest` | 不写 `.part` |
| ActiveCheckpointed | flush 当前 Segment 的 accepted-block pending | 后续 `.part` 写入失败时仍可从 durable equations 重试 |
| PartWritten | `WriteVerifiedSegment(RawOffset, raw)`，拒绝越界/重叠 | pending range 不算 verified |
| PartFlushed | `FlushFileBuffers(.part)`，PBStorage 才登记 verified range | completed journal 尚未承诺 |
| JournalCompleted | 先 checkpoint accepted blocks，再 flush completed record，再 compact | `.part` 可由重启重验 |
| ReceiverCommitted | `CommitStoredSegment` 标记完成并释放 decoder | 该 Segment 可进入最终计数 |

PBStorage 支持 Segment 任意顺序写入，但 verified ranges 不得重叠；所有 offset/addition 使用 checked arithmetic。`FlushVerifiedSegment` 在 flush 后更新区间表时捕获 allocation/length failure，不允许异常穿越 `noexcept`。

### 4.1 crash-window 判定

| 中断位置 | 重启 authoritative state | 处理 |
| --- | --- | --- |
| Unique block 已进入内存、尚未到 1 秒 checkpoint | journal 中没有该 block | 最多损失一个 checkpoint 周期的 equation；由后续 Carousel 重送 |
| accepted-block checkpoint 后、`.part` write 前或 write 中 | active block records | 重建 decoder 并重放；未 completed 的 `.part` 范围不 adopt，可安全重写 |
| `.part` flush 后、completed record 前 | active block records + 未承诺的磁盘 bytes | 不盲信该范围；重放恢复后重新验证/写入 |
| completed record flush 后、`CommitStoredSegment` 前 | completed record + flushed `.part` | 读取精确范围并重算 RawDigest，通过后 adopt/commit |
| compact temp flush 后、atomic replace 前 | 旧 `.resume` | 旧 journal 仍有效；`.tmp` 不是 authoritative state |
| publish intent flush 后、final rename 前 | intent + 完整 `.part` | 重启复验 completed `.part`，重复 intent 幂等，再次执行安全 publish |
| final rename 后或 final reopen 前、journal 删除前 | intent + 仅 final | 以绑定路径独占读取，重算长度/WholeFileDigest；一致则 Completed 并删除 journal |
| intent 状态下 `.part` 与 final 同时存在、两者都不存在或 final 摘要不符 | 冲突状态 | fail closed；不覆盖、不删除、不重建为可信输出 |

## 5. 重启恢复状态机

```text
validate SessionDescriptor
→ bounded load/validate journal
→ validate/reuse durable OutputReservation
→ if PublishIntent + final only:
     lock/read final and verify exact length + WholeFileDigest
     mark recovered publish, then remove journal
→ else CreateOrResume exact-size internal .part
→ replay stored SegmentDescriptor/FinalManifest controls
→ for each completed record:
     bounded ReadRange(.part)
     verify metadata against bound descriptor
     recompute RawDigest
     AdoptVerifiedSegment
     CommitStoredSegment
→ replay accepted blocks in journal order
→ create at most MaxActiveOuterFecDecoders (product default 4)
→ continue from later Carousel
```

completed record 从不让 Decoder 盲信 `.part`。长度、offset、descriptor identity 或 RawDigest 任一不符都在 adopt/commit 前拒绝，`verifiedBytes` 保持不增长。

第 5 个同时活跃的有效 Segment 返回 `DeferredResourceBusy`：不为它创建 codec、不驱逐既有 decoder、不使 Session 终止。其 block 可保留在有界 orphan cache，等已完成 Segment 在 durable commit 后释放资源，再由重复 descriptor 或后续 Carousel Data 重试。

## 6. WholeFileDigest 与 publish

全部 Segment durable commit 且 FinalManifest 已绑定后：

1. PBReceiver `PrepareFinalization` 再确认所有 Segment completed；
2. journal append+flush `PublishIntent(WholeFileDigest)`；该记录要求 OutputReservation、FinalManifest 与全部 completed records 已存在；
3. PBStorage flush/close `.part`；
4. 以 1 MiB bounded buffer 顺序扫描整个 `.part`，计算 BLAKE3 并比较文件长度与 WholeFileDigest；
5. publish 前再次确认持久绑定的最终候选不存在；
6. 同卷安全 rename；
7. 重新打开最终文件，再次验证长度与 WholeFileDigest；
8. 此时先把内存 completion 设为 published，再删除 `.resume`；删除失败只形成 post-publish cleanup warning，不把已经验证的 final 降格或覆盖。

当前进程中 final reopen 失败或摘要不一致时尝试把最终文件移回内部 `.part`；任何 cleanup 失败以独立错误返回。跨进程重启若发现 durable intent、`.part` 已消失且绑定 final 的长度/摘要一致，则把它视为 rename 已成功并安全清理 journal。重复 `Publish` 只有摘要完全相同时幂等成功；不同摘要、错误候选、双文件并存和 final 摘要不一致均保留现场并 fail closed。

## 7. 故障分类

| 分类 | 例子 | 行为 |
| --- | --- | --- |
| 可由 Carousel 重试 | `DeferredResourceBusy`、orphan quota 对新块的 drop | 不分配第五个 decoder、不终止 Session；等待重复 descriptor/Data |
| 可由重启恢复 | 合法 PBJR 截断尾、`.part` 已 flush 且 completed record durable、intent 后仅存在匹配 final | 修复尾部并逐 Segment 重算，或复验 final 后完成 publish commit |
| 当前进程停止、持久数据保留 | disk/flush/compact/publish native failure、WholeFileDigest mismatch | 不提前 commit、不覆盖最终文件；保留可诊断状态 |
| Session fail closed | descriptor conflict、同 OuterBlockId 不同 payload、encoded/raw digest mismatch | 不 latest-wins，不发布 |
| journal fail closed | header/record CRC、length、generation、unknown type、内部截断、cache 超限 | 不采用任何部分解析结果 |
| 本地配置/命名拒绝 | resource policy、非法 basename、两个候选都存在 | 不创建不受控输出，不覆盖现有文件 |

## 8. G03 定向验证

从仓库根目录运行，不创建窗口、不使用真实 capture/GPU、不修改显示器：

```powershell
cmake --build build-unified-release --config Release `
  --target PBReceiverTests PBStorageTests PBApplicationTests --parallel 2

build-unified-release/tests/PBReceiver/Release/PBReceiverTests.exe --durations yes
build-unified-release/tests/PBStorage/Release/PBStorageTests.exe --durations yes
build-unified-release/tests/PBApplication/Release/PBApplicationTests.exe '[resume]' --durations yes
```

覆盖点包括 DirectRepeat/Wirehair orphan replay、decoder 资源满后 Carousel Data 重试、same-ID conflict、乱序 verified ranges、确定性第二名称冲突、合法 torn tail、随机尾/length/CRC 损坏、restart active cache 上限，以及 completed `.part` 的成功/损坏 RawDigest 重验。最终执行结果与本地日志摘要记录在统一路线 G03 状态中。
