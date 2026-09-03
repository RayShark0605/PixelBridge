# RemoteVisual Step 22：重复完整文件恢复验收

> **2026-09-03 历史状态：** 本 campaign 被统一视觉/大文件产品路线取代，保留为未执行的 LF4 readiness 证据，不再继续作为当前产品完成条件。`executedRunCount=0`、`successfulRunCount=0` 等原始状态不得改写。当前执行入口见 [`UNIFIED_VISUAL_LARGE_FILE_IMPLEMENTATION_ROADMAP.md`](UNIFIED_VISUAL_LARGE_FILE_IMPLEMENTATION_ROADMAP.md)。

## 1. 当前状态与真值边界

Step 22 保持 `MANUAL-GATE`。当前增量只冻结并验证六轮实机 campaign，尚未执行 Computer B Encoder、Computer A WGC capture、Receiver、WholeFileDigest 或安全发布，因此：

- `executedRunCount=0`；
- `successfulRunCount=0`；
- `RemoteVisualSmokePass=false`；
- `CertifiedRemoteVisualProfile=false`。

Step 21 只证明过一次新的 1 MiB 完整文件链。它不能重复计入 Step 22，也不能把同一 Replay、同一 RunId 或同一发布文件复制六份充数。

## 2. 冻结的六轮配额

每份 `PixelBridge.RemoteVisualStep22Campaign.1` 都必须精确包含下列顺序和彼此不同的 OS-CSPRNG 128-bit RunId：

| ordinal | slot | source | 必须成功次数 |
| ---: | --- | --- | ---: |
| 1 | `random-1mib-01` | `random-1MiB.bin` | 1/3 |
| 2 | `random-1mib-02` | `random-1MiB.bin` | 2/3 |
| 3 | `random-1mib-03` | `random-1MiB.bin` | 3/3 |
| 4 | `random-8mib-01` | `random-8MiB.bin` | 1/2 |
| 5 | `random-8mib-02` | `random-8MiB.bin` | 2/2 |
| 6 | `zip-4mib-payload-01` | `random-payload-4MiB.zip` | 1/1 |

三个 source 都来自同一 `PixelBridge.RemoteVisualSourceSet.2`。1 MiB 与 8 MiB 文件必须是固定的 OS-CSPRNG bytes；ZIP 必须仍只含一份恰好 4 MiB 的 CSPRNG `payload.bin`。PixelBridge 对 ZIP archive 本身传输，三者的 Segment compression 都固定为 `RAW/OFF`。

固定 campaign 的六个槽必须全部成功。任一失败目录、report、journal 或 partial artifact 都必须保留；不得删除失败后以同一 RunId 重跑，也不得只汇总成功项。若需要重新开展完整 campaign，必须创建新的 campaignId 和六个新的 RunId，并在最终报告中同时列出此前失败 campaign，而不是覆盖旧证据。

## 3. 复用的 production 配置

Step 21 被用户取消了跨品牌/跨模式矩阵，因此不存在可从 31-cell 比较中选出的“理论最佳”配置。Step 22 采用唯一已有真实完整文件成功证据的配置，避免在重复性 Gate 中同时改变信道变量：

- `PB-RemoteVisual-LF4-X1 (Experimental)`，ProfileId `5783275402097472561`，Layout 7；
- Computer B production Encoder single-monitor fullscreen `primary`；
- Computer A production Decoder WGC，运行时选择整块 ExperimentMonitor 作为 Locator search ROI；
- logical visual FPS 2，Control repetitions 12；
- Wirehair V2 + Robust DVB-S2 Short QC-LDPC；
- `--compression off` / `RAW/OFF`；
- Decoder 先启动，Receiver 完成后 Encoder 仍继续广播，由操作者按 Q/Enter 停止；
- 不允许 socket、pipe、clipboard、共享内存、临时文件交换或其他非像素 payload 路径。

Sender 的 `cycleCount`、`cyclePosition`、`cycleFrameCount` 或源文件读完都不是接收完成。唯一完成权威仍是 Computer A production Receiver 的 `Completed + WholeFileDigest + finalPublishSucceeded`，以及对发布文件重新计算的 external length/SHA-256。

## 4. 有界长运行策略

按 Step 21 的真实 2 Hz 结果，8 MiB Wirehair run 可能超过旧 Decoder CLI 的 600 秒总 deadline。Step 22 把 production `--headless-receive/--headless-replay --timeout` 上限有界扩展到 3600 秒；`--no-progress-seconds` 上限仍为 600 秒，campaign 固定使用 180 秒。总 deadline 防止无限等待，no-progress deadline 则在 descriptor、temporal admission 和 verified-byte 都不前进时提前 fail closed。

Step 22 的目标是六次 **live** 文件恢复，而 Step 15/20/21 已分别验证过 production Replay/offline 路径。对 2560×1440 BGRA ROI，长达十几分钟的 full-frame Replay 很容易逼近或超过 16 GiB；本 campaign 因此冻结 `replayPolicy=DisabledForBoundedLiveFileSmoke`。每轮仍必须由实际 WGC pixels 进入相同 production demod/temporal/Receiver 链，并保留完整 bounded report/journal。这里不声称该轮有 offline Replay；也绝不把省略 Replay 改写成“无需屏幕像素”。

## 5. 每轮必须封存的证据

后续 A/B launcher 与最终 verifier 必须为每个预分配 RunId create-only 保存并交叉核对：

1. 同一 runtime commit/tree、package manifest/payload fingerprint 和 source-set identity；
2. B 端 Encoder report、journal、exit code、source stable、2 Hz dwell、manual stop 状态；
3. A 端 live Decoder report、journal、exit code、WGC backend、monitor-safety PASS 和 accepted Locator geometry；
4. B/A 完全一致的 SessionId、SessionTag、WholeFileDigest、profile/layout/FEC；
5. Receiver `Completed`、全部 raw bytes verified、WholeFileDigest PASS、safe publish PASS；
6. source 与 published output 的 external size/SHA-256 完全一致，且 output directory 恰好一个 final file、没有 `.part` 或额外 false output；
7. completion time、VerifiedRawGoodput、VerifiedEncodedGoodput、FER、FEC/CRC/identity failure、stale drops、capture drops、sequence gaps/duplicates/reorder 和 Outer duplicate/conflict/resource counters；
8. journal 未截断且以预期 terminal state 结束。

六轮 SessionId 与 SessionTag 也必须彼此唯一。相同 source 的 WholeFileDigest 可以相同，这是文件身份，不是 Session 重用。

## 6. 已实现工具与后续出口

- `PBRemoteVisualStep22Common.psm1`：共享的严格 schema importer 和固定 3/2/1 schedule authority；
- `New-PBRemoteVisualStep22Campaign.ps1`：先递归验证 package/source set，再生成 campaignId 与六个唯一 RunId；临时 artifact 通过独立 verifier 后才原子发布；
- `Test-PBRemoteVisualStep22Campaign.ps1`：重读 package/source verifiers、application binaries、source bytes、inner ZIP binding 和 campaign exact schema，可选 create-only 输出 verification artifact。

当前正向验证已经证明一份 campaign 精确生成 6 个唯一 RunId 和 3/2/1 配额；负向验证覆盖 duplicate RunId、字符串数值和 run/source SHA 漂移，均在产生执行证据前拒绝。该结果仍只是 readiness。

2026-09-03 用户进一步明确最终产品目标包含约 20 GB 单文件、Encoder 1..60 Hz 手工 cadence 和 Decoder cadence 自动适应；现有低帧率路线不足以覆盖该目标。因此本增量在 campaign readiness 边界停止，Computer B 六轮 `.bat`、Computer A runner、最终 6-run verifier 和实机 6/6 均未实现；后续实施必须先重新基线整体路线。只有将来最终 verifier 对固定 campaign 的六个槽全部给出文件级 PASS，才允许输出 `RemoteVisualSmokePass=true`；即便如此，`CertifiedRemoteVisualProfile` 仍保持 false。
