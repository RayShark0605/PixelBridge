# CP-A 多 Segment 无屏幕端到端检查点

> 状态：G04 已实现并通过 Release 定向验证（2026-09-03）
> 性质：正式 Descriptor/Transport、Outer FEC、Receiver、resume journal、storage/publish 的 application-runtime 检查点
> 证据边界：`visualChainCovered=false`；不包含 raster、Inner FEC、呈现、capture、GPU、GUI、物理链或吞吐认证

## 1. 检查点结论

CP-A 在不创建窗口、不编码像素、不采集屏幕的条件下，证明了以下窄闭环可以由当前生产 application runtime 对象完成：

```text
稳定源文件句柄
  -> durable Sender 预扫描与正式 descriptor bundle
  -> SenderFrameBuilder 的正式 Outer payload / TransportBlock serialization
  -> ReceiverPipeline / ReceiverIngress
  -> encoded digest / bounded decompress / raw digest
  -> Decoder resume journal
  -> PBStorage 乱序写入、flush、WholeFileDigest、rename 后复验
  -> authoritative final publish
  -> 独立重新打开 source/output 做长度、SHA-256、BLAKE3 和逐字节比较
```

`ApplicationRuntimeTestAccess::ProbeHeadlessMultiSegmentFile` 只是一个同步、无 raster 的定向入口；它没有实现第二套 Receiver、FEC 或发布状态机：

- Sender 使用 `PrepareDurableSender` 与生产 `SenderFrameBuilder`；`BuildTransportBlockForSlot` 同时被原 raster/Inner-FEC 路径和 CP-A 调用，因此 Outer block header、payload、CRC 与 serialization 只有一份实现。
- Control bytes 来自 durable Sender 创建的正式 Session/Segment/Manifest descriptor；Transport bytes 经 `SerializeTransportBlock` 后再由 Receiver 独立解析。
- Control 和 Transport 分别进入现有 `ReceiverPipeline::ProcessControlRecord` 与 `ReceiverPipeline::ProcessTransport`，后续继续使用现有 `ReceiverIngress`、`DecoderResumeStore` 与 `PBStorage`。
- CP-A 不伪造 capture epoch、Bootstrap 或视觉帧；capture-time goodput telemetry 在此模式中不产生。digest、journal、storage 与 publish 判定仍走生产路径。
- 测试结束条件不是“Outer decoder ready”，而是 Decoder snapshot 同时满足 `Completed`、`wholeFileDigestVerified`、`finalPublishSucceeded`、verified raw bytes 等于源长度，并且 authoritative output path 可重新打开。

当前 seam 绑定既有 `RemoteVisualLowFps` profile identity，仅用于满足正式 SessionDescriptor 的 application admission；没有调用该 profile 的 raster、modulation、Inner FEC 或 capture。最终 Unified visual profile 要到 G06 及后续目标实现，因此 CP-A 不能作为统一视觉链已经完成的证据。

## 2. 对抗性输入顺序

每个非空 fixture 都至少执行以下顺序；多 Segment fixture 扩展到全部 Segment：

1. 先提交正式 SessionDescriptor。
2. 在任何 SegmentDescriptor 之前提交 Segment 0 的一个有效 Transport，证明它只进入 bounded orphan cache；此时不创建未经 descriptor 约束的 durable payload 形状。
3. 按 ordinal 逆序提交全部 SegmentDescriptor。
4. 对 Segment 0 再提交一次 byte-identical descriptor，证明重复是幂等而不是同帧 `Ready` 重复完成。
5. 提交正式 FinalManifest。
6. 首轮给各 Segment 播种一个 equation；5-Segment fixture 先占满 4 个 active decoder，使第五个 Segment 精确返回一次 `DeferredResourceBusy`，不驱逐已有 decoder。
7. 对每个 Wirehair Segment 故意跳过 systematic block ID 1，再提交 repair equation，要求 Segment 只能经 repair 收敛。
8. 第一个 active Segment 完成并释放容量后，重新提交第五个 Segment 的原 Transport，要求精确成功一次。
9. 所有 Segment 分别完成 encoded/raw digest 验证与随机写入，最终执行 WholeFileDigest 和安全发布。

这组顺序同时覆盖 descriptor 乱序、byte-identical duplicate、pre-descriptor orphan drain、repair、4-active backpressure 和后续 Carousel retry。它没有注入 conflicting descriptor/payload；该 fail-closed 合同继续由 G01/G03 及对应单元测试覆盖。

## 3. Fixture 与结果

专用 `PBHeadlessMultiSegmentCheckpoint` 共执行 9 个 fixture。以下 SHA-256 由测试进程使用 Windows CNG 重新打开文件计算，并又由 Python 3.12.9 `hashlib` 独立复验；每个 source/output 的完整 BLAKE3 对也记录在 JSON report 中并由 Python `blake3 1.0.9` 复验。

| fixture | 源字节 | Segment | 编码 / Outer FEC | 注入与结果 | source = output SHA-256 |
| --- | ---: | ---: | --- | --- | --- |
| `empty-0b` | 0 | 0 | 无 Segment | 空 manifest 直接完成；发布 0-byte 文件 | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `single-1b` | 1 | 1 | RAW / DirectRepeat | orphan + exact descriptor duplicate；byte-exact | `3f79bb7b435b05321651daefd374cdc681dc06faa65e374e38337b88ca046dea` |
| `boundary-8m-minus-1` | 8,388,607 | 1 | RAW / WirehairV2 | 跳过 1 systematic，1 repair；byte-exact | `f14c625f4823b6fd638ff9019b4bd8ffbee0420f033c45bd6a5d5c17c0eb8c8a` |
| `boundary-8m` | 8,388,608 | 1 | RAW / WirehairV2 | 跳过 1 systematic，1 repair；byte-exact | `bf45ad44de0dc8c1f9e650fbd5a9c1e0f03975fa5792e1c8915f7e0782452109` |
| `boundary-8m-plus-1` | 8,388,609 | 2 | RAW / WirehairV2 + DirectRepeat | descriptor 全逆序；大段 repair；byte-exact | `0a1ddc7649d3b078c7009010c1aa0db250817583c676afaea096ea79edf3ff73` |
| `large-csprng-24m` | 25,165,824 | 3 | compression enabled，三段均回退 RAW / WirehairV2 | `BCryptGenRandom`；每段跳过 1 systematic 并由 repair 恢复；byte-exact | `871f6e61c147cd237de97ad6f7e219fcf990d4415dc4c61834c680f5d0969047` |
| `small-compressible` | 65,536 | 1 | zstd 24 bytes / DirectRepeat | 强可压缩输入确实选择 zstd；byte-exact | `156c38442089c1323d3e3ba549a6ac24341c47e8b6367bec4740c9b8c865826e` |
| `small-incompressible` | 65,536 | 1 | compression enabled，回退 RAW / WirehairV2 | `BCryptGenRandom`；跳过 1 systematic，1 repair；byte-exact | `c0bf86db1b28171b0d1cc41929ae41072a3214f06715e45ca11bb1db313d3b26` |
| `five-segment-busy-retry` | 33,554,433 | 5 | 前四段 RAW / WirehairV2，尾段 RAW / DirectRepeat | 4 active；1 busy；释放后 1 成功重试；四个大段均走 repair；byte-exact | `7970f312e65498b20a15364f0c724e30f59c46c624cab59fb9b47537393988e4` |

外部检查不是只比较 runtime snapshot：测试会关闭 application 闭环后重新打开 source 与最终路径，分别流式计算长度、CNG SHA-256、BLAKE3，并做完整逐字节比较。随后单独的 Python verifier 再次读取相同文件，核对 JSON 中 source/output 的两个摘要。最终 JSON 顶层结果为：

```json
{
  "schema": "PixelBridge.UnifiedVisualCpAHeadless.1",
  "caseCount": 9,
  "visualChainCovered": false,
  "allPublishedByteExact": true,
  "workingSetWithinLimits": true
}
```

## 4. Working-set 高水位

高水位是在每次 production admission/journal mutation 后读取实际 Sender/Receiver/resume 状态得到，不是依据文件总长推算。

| 资源 | 观测高水位 | 上限 | 判定 |
| --- | ---: | ---: | --- |
| Sender resident encoded Segment count | 2 | 2 | PASS |
| Sender resident encoded Segment bytes | 16,777,216 | 16,777,216 | PASS |
| Receiver active Outer FEC decoder count | 4 | 4 | PASS |
| Receiver reserved Outer FEC decoder bytes | 228,600,848 | 1,073,741,824 | PASS |
| Receiver orphan cached payload bytes | 1,314 | default policy 4,194,304 | PASS |
| resume active payload copies | 8,393,832 | 268,435,456 | PASS |
| resume pending payload copies | 8,393,832 | 268,435,456 | PASS |
| resume active + pending resident payload copies | 16,787,664 | 536,870,912 | PASS |

Sender 高水位只统计 current/next 两个已编码 Segment，不把预扫描 descriptor table 误记为 encoded payload。resume resident 数值明确把 active 与 pending journal payload copy 相加；两类各自受 `maxResumeBytes` 约束，因此总 resident copy ceiling 记为两倍该值。表中不包含第三方 codec 对象的进程 allocator overhead；Outer decoder 自身使用 Receiver 的 reservation telemetry 单独计入 228,600,848 bytes。

## 5. 可复现验证

从配置后的 `build-unified-release` 执行路线图规定的五目标构建和精确 CTest 过滤：

```powershell
cmake --build build-unified-release --config Release `
  --target PBProtocolTests PBOuterFecTests PBReceiverTests PBStorageTests PBApplicationTests -- /m
ctest --test-dir build-unified-release -C Release `
  -R '^(PBProtocolTests|PBOuterFecTests|PBReceiverTests|PBStorageTests|PBApplicationTests)$' `
  --output-on-failure
```

结果为 5/5 PASS，0 failed，31.94 秒。随后只构建并运行专用检查点：

```powershell
cmake --build build-unified-release --config Release `
  --target PBHeadlessMultiSegmentCheckpoint -- /m
ctest --test-dir build-unified-release -C Release `
  -R '^PBHeadlessMultiSegmentCheckpoint$' --output-on-failure -V
```

结果为 1/1 PASS，0 failed，16.98 秒，内部 9/9 fixture PASS。另以 `D:\Python3.12.9\python.exe`、`hashlib` 和 `blake3 1.0.9` 对 9 组 source/output 与 JSON report 做只读独立复验，9/9 PASS。

本次没有运行 full CTest，也没有创建窗口或运行 raster、Inner FEC、modulation、D3D11、GPU、WGC/DXGI capture、GUI、native/remote display 或物理链。

## 6. 本地证据产物

以下是本次验证后的本地生成物；它们位于 ignored build tree，不进入源码提交。SHA-256 用于识别本轮具体证据快照：

| 产物 | SHA-256 |
| --- | --- |
| `build-unified-release/g04-five-target-build.txt` | `d696c0a8864257bfe260e854c570b503d40959f24ff3397830fbf011dbfbb5fb` |
| `build-unified-release/g04-five-target-ctest.txt` | `fe620d468291ae5b2b0d56e92f347c954fe9fe92a6ca87f3e0e7683577fb04dd` |
| `build-unified-release/g04-headless-target-build.txt` | `5cfb84912040d28cdfe90de9654d535144f6ff8c23d9aa9a3c8379ad6c21965f` |
| `build-unified-release/tests/PBApplication/g04-cp-a-gate.txt` | `9b36ee22e45318bd77ef6f79832d8fdab6741a0db8c15015d23348a8e37240bf` |
| `build-unified-release/tests/PBApplication/g04-cp-a-headless-report.json` | `600a95ec211335deef4b608e777a826a380e37f54c1ef158d6adb621e0738de2` |
| `build-unified-release/tests/PBApplication/g04-external-python-verification.txt` | `e0c7402748b66bf7cb035a479180d00fe649dc56038b804dc9061a0055f30f46` |

`g04-cp-a-scratch/` 只包含此测试创建的 source、sender state、`.part`/resume 中间状态和发布文件；每个 case 开始前仅清理由该 fixture 自己拥有的子目录。

## 7. 未覆盖项与后续门槛

CP-A 只关闭 G04，不前移后续 Goal：

- G05 仍需实现并验证大输出确认、卷空间/allocation 语义，以及 rename 后崩溃恢复。
- G06..G17 仍需实现 Unified visual profile、mapping、CPU/GPU、Data Window、capture、Qt 与 telemetry 产品接线。
- G18..G20 仍负责 20 GiB 长运行、系统化 fault injection、真实 native/remote 显示与发布级验收。
- 本检查点的 24 MiB CSPRNG 和 32 MiB+1 busy fixture 证明多 Segment 闭环与 active-budget 行为，不是 20 GiB/500 GiB 可用性或 working-set 认证。
- 这里的 16.98 秒是本机 headless 测试耗时，不能换算成视觉信道吞吐、`UniqueVisualFPS` 或 `VerifiedEncodedGoodput`。
