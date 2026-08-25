# PixelBridge Phase-0 command-line tooling

本目录包含 Qt-free 的协议/帧检查和确定性向量生成工具。它们复用现有
`PBProtocol`、`PBInnerFec`、`PBModulation` parser/codec，不创建第二套生产协议
架构。Transport/interleave 仍为 Phase-0 tooling/reference，详见
`docs/GOLDEN_VECTOR_HARNESS.md`。

## PBProtocolDump

```text
PBProtocolDump [--type <auto|bootstrap|control|fragment|session-descriptor|segment-descriptor|final-manifest|transport|wirehair-descriptor|pbvm-manifest>]
               [--session-descriptor <file>]
               <record-file>
```

- `auto` 只按 `PBRG/PBCR/WHV2/PBVM` magic 选择 parser；无 magic 只严格尝试
  Transport，不猜测 Session/Segment/Manifest payload 类型。
- 输出稳定字段表，包含 byte offset、size、raw/interpreted value、stored 与
  recomputed CRC 以及 authoritative parser 状态。
- Segment/Manifest 语义检查需要精确 37-byte SessionDescriptor context。缺少
  context 是 `skipped/missing-context`（exit 1）；context I/O/size 错误为 exit 2。
- exit code：0 有效，1 已评估但无效/缺上下文，2 参数、I/O 或输入上限错误。

## PBFrameInspector

```text
PBFrameInspector [--recovery] <frame.pbrw|frame.png>
```

Inspector 在分配 8,294,400-byte canvas 前校验 PBRW/PNG header、reserved、固定
1920×1080 geometry、pixel byte count 与 checked 总长。处理顺序为容器 → raster
解调 → authoritative Bootstrap → Control diagnostics → data digest/zero tail → 可选
Robust LDPC recovery → canonical padding → Transport CRC/parse → SessionTag cross-check。

- Bootstrap 无效会使整体 frame 失败且禁止 recovery；
- Control 是 diagnostic-only，坏 Control 会显示 parser error 但不单独否决有效
  frame；
- recovery 对每个 2025-byte window 做 syndrome、bounded perfect-LLR decode、
  decode/re-encode 一致性和 1,350-byte info extraction；完整零窗口仅在其后全部
  为 canonical zero padding 时终止扫描，非零残缺尾部失败；
- 完整 raster 不隐式交织/反交织，报告固定显示 `interleave=not-bound`；
- exit code：0 frame authoritative valid（且请求的 recovery 成功），1 内容无效，
  2 参数/I/O 错误。

## PBVectorGen

```text
PBVectorGen write-golden <out-dir> [--only <protocol|ldpc|interleave|raster>]
PBVectorGen write-corpus <out-dir> --category <transport|interleave|ldpc> [--seed <u64>]
PBVectorGen write-frame <out-file> --vector <g0|g1|g1-transport|g1-transport-2cw|g2> --format <pbrw|png>
PBVectorGen dump-manifest [--skip-frames]
```

解析器拒绝重复/未知选项、多 positional root、缺值、负数或溢出的 seed 和多余参数。
写入后重新读取验证；目标已存在时仅 byte-identical 才算幂等成功，任何不同内容
均拒绝覆盖。`write-golden`/`write-frame` 在写文件前必须匹配冻结 digest pin，
没有自动 re-pin 路径。

## PBGoldenVectorCheck

测试配置额外构建：

```text
PBGoldenVectorCheck <tests/golden>
```

它验证 25 个 file-backed vectors、5 个完整 frame pins、独立 raster oracle 与
file/recompute/pin 三方状态。退出码为 0/1/2（全匹配/验证失败/用法错误）。

## 诊断约定

所有可归因到 byte 的失败包含：

```text
space=<record|context|raw-pbrw|decoded-png-bgra> byte_offset=<n> expected=<...> actual=<...>
```

Recovery 的嵌套错误还包含 `windowByteOffset`、`innerByteOffset`、
`dataRegionByteOffset`。PNG 仅压缩 stream digest 漂移而 decoded BGRA 相同时使用
`byte_offset=not-applicable`。
