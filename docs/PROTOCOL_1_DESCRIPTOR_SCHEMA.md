# PixelBridge Protocol 1.0 Descriptor Schema 1

本文冻结 `SessionDescriptor`、`SegmentDescriptor` 和 `FinalManifest` 的正式字节合同。它是
`docs/PixelBridge_最终技术路线与总体设计.md` 第 7..10、15、33 节的机器核对补充，不取代
其中的资源、安全、不可变绑定和最终发布约束。

实现与核对入口：

- 公共 wire 常量：`libs/PBProtocol/include/pbprotocol/descriptor_codec.h`
- serializer/parser：`libs/PBProtocol/src/descriptor_codec.cpp`
- offset/width/CRC/Control exact-length 核对：`tests/PBProtocol/test_descriptor_schema.cpp`
- 正式 Golden manifest：`tests/PBProtocol/formal_descriptor_golden_manifest.txt`
- 正式与历史 corpus：`fuzz/corpus/descriptor-resource/`、`fuzz/corpus/bootstrap-control/`

## 1. 共用规则

- 所有多字节整数都是 unsigned Little Endian。
- `SessionId` 和摘要按原始 byte 顺序存放，不进行整数重解释。
- 字段之间没有 ABI padding；reserved bytes 必须为零。
- `DescriptorSchemaVersion=1`。其他值稳定返回 `UnsupportedDescriptorSchema`。
- `DescriptorHeaderBytes` 必须与 record type 的固定值完全相等，不用于猜测兼容布局。
- `DescriptorTotalBytes` 必须等于 parser 收到的完整 input bytes。
- 最后 4 bytes 是 Little-Endian CRC-32C，覆盖 `[0, DescriptorTotalBytes-4)`。
- serializer 只接受尺寸完全相等的输出 span，并通过 scratch buffer 保证失败时不部分改写输出。

三个 record 共用以下 8-byte prefix：

| Offset | Bytes | Encoding | Field | Schema 1 rule |
| ---: | ---: | --- | --- | --- |
| 0 | 2 | LE `uint16` | `DescriptorSchemaVersion` | 必须为 `1` |
| 2 | 2 | LE `uint16` | `DescriptorHeaderBytes` | Session=`70`，Segment=`128`，Manifest=`80` |
| 4 | 4 | LE `uint32` | `DescriptorTotalBytes` | 必须与当前 Descriptor 所占 Control payload bytes 完全相等 |

公共常量 `kDescriptorSchemaVersionOffset`、`kDescriptorHeaderBytesOffset`、
`kDescriptorTotalBytesOffset` 及对应 `*Bytes` 常量由编译期连续性断言核对。CRC offset 统一由
`DescriptorTotalBytes-kDescriptorCrcBytes` 得出，不存在 host struct 尺寸依赖。

## 2. SessionDescriptor

`DescriptorHeaderBytes=70`。总长度为：

```text
70 + FileNameUtf8Bytes + OptionalExtensionsBytes + 4
```

最小值为 74，最大值 `kMaximumDescriptorPayloadBytes=1300`。当前正式 Golden 使用
`payload.bin`，因此为 85 bytes；85 不是固定 wire record 长度。

| Offset | Bytes | Encoding | Field | Schema 1 rule |
| ---: | ---: | --- | --- | --- |
| 0 | 2 | LE `uint16` | `DescriptorSchemaVersion` | `1` |
| 2 | 2 | LE `uint16` | `DescriptorHeaderBytes` | `70` |
| 4 | 4 | LE `uint32` | `DescriptorTotalBytes` | `70 + name + extensions + 4`，且等于 input bytes |
| 8 | 2 | LE `uint16` | `ProtocolMajor` | 当前必须为 `1` |
| 10 | 2 | LE `uint16` | `ProtocolMinor` | 当前必须为 `0` |
| 12 | 16 | bytes | `SessionId` | OS CSPRNG 生成；wire parser 不接受另一长度 |
| 28 | 8 | LE `uint64` | `SessionVisualProfileId` | PBProtocol 只要求非零 |
| 36 | 8 | LE `uint64` | `OriginalFileSize` | 受本地 `ReceiverResourcePolicy` 限制 |
| 44 | 4 | LE `uint32` | `SourceSegmentTargetBytes` | PBProtocol 要求非零；产品 admission 冻结为 8 MiB |
| 48 | 8 | LE `uint64` | `SegmentCount` | 空文件必须为 0；非空必须为 `1..OriginalFileSize`，并受本地 policy 限制 |
| 56 | 1 | `uint8` | `CompressionPolicy` | `1=AutomaticZstandardLevel3RawFallback` |
| 57 | 1 | `uint8` | `DigestAlgorithm` | `1=BLAKE3-256` |
| 58 | 2 | zero bytes | `Reserved` | 必须全零 |
| 60 | 8 | LE `uint64` | `FeatureFlags` | 低 32 bits 为 mandatory；高 32 bits 为 optional |
| 68 | 2 | LE `uint16` | `FileNameUtf8Bytes` | `1..1020` |
| 70 | N | UTF-8 bytes | `FileNameUtf8` | 精确 N bytes；规则见第 6 节 |
| `70+N` | M | canonical TLVs | `OptionalExtensions` | M 可为 0；规则见第 3 节 |
| `DescriptorTotalBytes-4` | 4 | LE `uint32` | `DescriptorCrc32c` | CRC-32C over `[0, DescriptorTotalBytes-4)` |

`FeatureFlags & 0x00000000FFFFFFFF != 0` 表示当前实现遇到未知 mandatory feature，必须返回
`UnknownMandatoryFeature`。高 32 bits 的未知 optional flags 可以保留和跳过，但不能改变当前
Schema 1 字节的解释。

PBProtocol 故意不把 `SessionVisualProfileId` 限死为 Unified ID。协议层只验证它非零；
`PB-Unified-LC4-V1` 是产品 application admission 规则，历史内部 A/B 工具仍可生成正式 schema，
但不能因此成为最终产品可选项。

## 3. Session optional TLV

TLV offset 相对于 `OptionalExtensions` 起点：

| Relative offset | Bytes | Encoding | Field | Schema 1 rule |
| ---: | ---: | --- | --- | --- |
| 0 | 2 | LE `uint16` | `Type` | 非零；各 TLV 按 Type 严格递增，因此禁止重复 |
| 2 | 2 | LE `uint16` | `Flags` | bit 0=`Optional`；其他 bits 必须为零 |
| 4 | 4 | LE `uint32` | `ValueBytes` | 后续 Value 的精确长度 |
| 8 | N | bytes | `Value` | N 可为 0，但 header 和 value 必须完整存在 |

Schema 1 当前没有已知 TLV。因而：

- `Flags.Optional=0` 的任何 TLV 都是未知 mandatory TLV，返回 `UnknownMandatoryFeature`；
- `Flags.Optional=1` 的未知 TLV 原样保存在 `SessionDescriptor::optionalExtensions`，使 immutable
  binding 能发现同 key 不同内容；
- type 0、乱序、重复、未知 flags、截断 header/value 均 fail closed；
- TLV bytes 与文件名共同受 1300-byte Descriptor 上限约束，parser 在该上限之外不分配字符串或
  extension storage。

## 4. SegmentDescriptor

`DescriptorHeaderBytes=128`。DirectRepeat 总长固定 132 bytes；Wirehair V2 总长固定 164 bytes。

| Offset | Bytes | Encoding | Field | Schema 1 rule |
| ---: | ---: | --- | --- | --- |
| 0 | 2 | LE `uint16` | `DescriptorSchemaVersion` | `1` |
| 2 | 2 | LE `uint16` | `DescriptorHeaderBytes` | `128` |
| 4 | 4 | LE `uint32` | `DescriptorTotalBytes` | DirectRepeat=`132`；Wirehair V2=`164`；必须等于 input bytes |
| 8 | 8 | LE `uint64` | `SessionTag` | 必须等于从已验证 SessionId 派生的 tag |
| 16 | 8 | LE `uint64` | `SegmentOrdinal` | 必须 `< SessionDescriptor.SegmentCount` |
| 24 | 8 | LE `uint64` | `RawOffset` | 与 RawSize 使用 checked addition |
| 32 | 8 | LE `uint64` | `RawSize` | 非零且受 policy 限制 |
| 40 | 8 | LE `uint64` | `EncodedSize` | 非零且受 policy 限制 |
| 48 | 1 | `uint8` | `CompressionCodec` | `1=RAW`，`2=Zstandard` |
| 49 | 1 | `uint8` | `OuterFecMode` | `1=WirehairV2`，`2=DirectRepeat` |
| 50 | 2 | zero bytes | `Reserved` | 必须全零 |
| 52 | 4 | LE `uint32` | `OuterBlockBytes` | `1..65535`，并受 receiver policy 限制 |
| 56 | 32 | bytes | `RawDigest` | BLAKE3-256 |
| 88 | 32 | bytes | `EncodedDigest` | BLAKE3-256；RAW 时必须等于 RawDigest |
| 120 | 8 | LE `uint64` | `Flags` | Schema 1 必须为 0；非零为未知 mandatory feature |
| 128 | 0 or 32 | bytes | `WirehairV2SerializedProfile` | DirectRepeat 时不存在；Wirehair V2 时必须精确 32 bytes |
| `DescriptorTotalBytes-4` | 4 | LE `uint32` | `DescriptorCrc32c` | CRC-32C over `[0, DescriptorTotalBytes-4)` |

DirectRepeat 的 CRC 位于 offset 128；Wirehair V2 profile 位于 `[128,160)`，CRC 位于 offset 160。
mode 与总长必须相互一致，不能把 164-byte payload 按 DirectRepeat 部分解析，也不能缺少 Wirehair
profile 后仍返回成功。

## 5. FinalManifest

`DescriptorHeaderBytes=80`，`DescriptorTotalBytes=84`。

| Offset | Bytes | Encoding | Field | Schema 1 rule |
| ---: | ---: | --- | --- | --- |
| 0 | 2 | LE `uint16` | `DescriptorSchemaVersion` | `1` |
| 2 | 2 | LE `uint16` | `DescriptorHeaderBytes` | `80` |
| 4 | 4 | LE `uint32` | `DescriptorTotalBytes` | `84`，且等于 input bytes |
| 8 | 16 | bytes | `SessionId` | 必须等于 SessionDescriptor.SessionId |
| 24 | 8 | LE `uint64` | `OriginalFileSize` | 必须等于 SessionDescriptor.OriginalFileSize |
| 32 | 8 | LE `uint64` | `SegmentCount` | 必须等于 SessionDescriptor.SegmentCount |
| 40 | 32 | bytes | `WholeFileDigest` | BLAKE3-256；空文件必须是 BLAKE3-256(empty) |
| 72 | 1 | `uint8` | `DigestAlgorithm` | 必须等于 SessionDescriptor.DigestAlgorithm，当前为 1 |
| 73 | 7 | zero bytes | `Reserved` | 必须全零 |
| 80 | 4 | LE `uint32` | `DescriptorCrc32c` | CRC-32C over `[0,80)` |

## 6. UTF-8 basename

`FileNameUtf8` 必须是严格有效 UTF-8，并且只表示 basename。拒绝：

- embedded NUL 和所有 ASCII control bytes `<0x20`；
- `/`、`\`，因此绝对路径、UNC、device namespace 和 traversal 都不能进入；
- `:`，因此盘符路径和 NTFS ADS 都不能进入；
- Windows 非法字符 `< > " | ? *`；
- `.`、`..`、尾随 dot 或 space；
- 不区分 ASCII 大小写的 `CON`、`PRN`、`AUX`、`NUL`、`CONIN$`、`CONOUT$`，包括带扩展名形式；
- `COM1..COM9`、`LPT1..LPT9`，包括带扩展名形式；
- 空名称或超过 1020 UTF-8 bytes 的名称。

最终输出仍只能位于 Decoder 用户选择的本地目录；通过 wire basename 校验不等于允许覆盖已有文件。

## 7. Control payload exactness

PB-Control-1 的 payload 是 opaque bytes，但 typed admission 固定为：

```text
PB-Control-1 exact RecordBytes
  -> exact payload span
  -> record-type descriptor parser
  -> DescriptorTotalBytes == payload.size()
  -> descriptor CRC/resource/session validation
  -> immutable binding
```

Control envelope 的 `RecordBytes` 必须等于完整 Control input；Descriptor 的
`DescriptorTotalBytes` 又必须等于完整 Control payload。即使重新计算两层 CRC，给 Descriptor payload
增加 trailing byte 或删除最后一个 byte 也只能得到 `InvalidRecordSize`，不得部分解析成功。

## 8. 大小与 checked-arithmetic 基线

以产品固定 8 MiB source segment target 计算的 canonical 边界如下。这里的 SegmentCount 用
`size == 0 ? 0 : 1 + (size - 1) / 8 MiB`，避免 `size + target - 1` 溢出。

| OriginalFileSize | Canonical SegmentCount | G01 expectation |
| ---: | ---: | --- |
| 0 | 0 | 接受空 Session shape |
| 1 | 1 | 接受 |
| 8 MiB - 1 | 1 | 接受 |
| 8 MiB | 1 | 接受 |
| 8 MiB + 1 | 2 | 接受 |
| 20,000,000,000 | 2,385 | 接受 |
| 20 GiB | 2,560 | 接受 |
| 500 GiB | 64,000 | 在默认 500 GiB policy 上限内接受 |
| 500 GiB + 1 | 64,001 | 默认 policy 返回 `ResourceLimitExceeded` |

PBProtocol 的 Session shape validation 不替代后续 Segment Map：每个 Segment 的
`RawOffset+RawSize` 独立 checked，完整 map 必须无 overlap、无 gap 并精确覆盖
`[0, OriginalFileSize)`。未经验证的 count/size 不得直接驱动同规模分配。

## 9. Golden 与历史 fixture 边界

`tests/PBProtocol/formal_descriptor_golden_manifest.txt` 使用固定 schema、顺序、长度、BLAKE3-256 和
预期 disposition，绑定以下资产：

- 4 个正式 accepted Descriptor；
- 1 个正式 checked-overflow SegmentDescriptor；
- 1 个包含正式 SessionDescriptor 的 PB-Control-1 record；
- 4 个旧 provisional Descriptor；
- 1 个包含旧 provisional SessionDescriptor 的 PB-Control-1 record。

`PBProtocolTests` 固定 manifest 自身的 BLAKE3，逐项复验每个 fixture 的长度和 BLAKE3；已有独立
corpus tests 再验证正式 bytes 可解析、重序列化 byte-identical，overflow 返回精确算术错误，而所有旧
provisional Descriptor 只承诺 `UnsupportedDescriptorSchema`。

`tests/golden/protocol/` 和 `docs/GOLDEN_VECTOR_HARNESS.md` 仍属于历史 Phase-0 tooling harness，其中
37/110/142/65-byte Descriptor 不得重新解释为正式 Schema 1，也不在 G01 中静默 re-pin。正式产品
Descriptor Golden 以本节列出的 G01 manifest/corpus 为准。

## 10. G01 最小验证

```powershell
cmake --build build-unified-release --config Release --target PBProtocolTests -- /m
ctest --test-dir build-unified-release -C Release -R '^PBProtocolTests$' --output-on-failure
```

该验证只证明 Protocol 1.0 Descriptor Schema 1 的 parser、resource、conflict、Golden 和相关 PBProtocol
语义，不代表 sender、storage、Unified visual、GPU、GUI、实屏、大文件 I/O 或真实远程链通过。
