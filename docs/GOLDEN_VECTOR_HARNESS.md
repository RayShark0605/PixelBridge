# PixelBridge Phase-0 Golden Vector Harness

本文档记录 `PBGoldenVectorCheck`、`PBVectorGen` 与相关 test-only oracle 的覆盖范围、
证据来源、诊断约定和受控再生成流程。该 harness 是 Phase-0 工具链验证设施，不是
协议版本协商机制，也不会把暂定 wire layout 自动提升为正式 v1。

## 1. 成熟度与依赖边界

- PB-Bootstrap-1、PB-Control-1、Control Fragment、既有 descriptor Golden 与
  Wirehair V2 canonical descriptor 延续仓库已有字节契约。
- `TransportBlockHeader` 的 32-byte header、`36 + PayloadBytes` block layout 以及
  Phase-0 interleave 公式仍为 **provisional tooling/reference**。本任务没有修改
  `ReceivedTransportBlock` admission API，也没有创建尚未定义的
  `SessionVisualProfileId` / `InterleaveProfileId` wire binding。
- `PB-ReferenceRaster-1` 仍是 Phase-0 reference candidate，不是 certified profile。
  完整 G1-Transport frame 明确为 non-interleaved；Inspector 输出
  `interleave=not-bound`，不会猜测或隐式反交织。
- `PBGoldenVector` 是 Qt-free、非安装、非导出的内部 support target。仅在
  tools/tests/fuzz 需要时构建；core-only 与默认 `PB_BUILD_TOOLS=OFF` 配置不会
  无条件拉入该 target。

## 2. Coverage matrix

| 类别 | 文件/内存 pin | 权威校验与独立证据 |
| --- | --- | --- |
| Bootstrap | `bootstrap-record.bin`，44B | 既有 inline byte array、CRC-32C、authoritative parser、精确重序列化 |
| Control | 67B SessionDescriptor、30B empty、65,536B maximum | 既有 inline array；67B CRC 固定 `0xA13883C8`；长度/CRC/语义边界 |
| Fragment | 48/48/43B 三片 | 既有 arrays；CRC 固定 `0x9DCD5402 / 0xF3FF94D8 / 0x40106F66` |
| Descriptor | 37B Session、110B DirectRepeat、142B Wirehair Segment、65B FinalManifest | 与 `test_descriptor_codec.cpp` 的独立精确数组双向一致；Session 使用 fileSize=117、segmentCount=1 |
| Wirehair | 32B `WHV2` canonical descriptor | 独立手工 LE layout、profile/dimension validator；不序列化第三方 C++ 类型 |
| Transport | 36B minimum、1,350B canonical、65,571B maximum | test-only 手工 LE builder、bitwise CRC-32C、独立 SplitMix64 与生产 serializer 逐字节比较；header CRC 仅覆盖 `[0,28)` |
| LDPC | Robust/Balanced/Fast 各 2,025B | 三个 profile 各自从 `SplitMix64(0xC0FFEE)` bit 0 开始；既有独立 H·c=0/矩阵/pin 测试 |
| Interleave | 8,192B mapping、56,168B logical、phase-7 physical | test-only 公式 `(logical*65537 + phase*472) mod 112336` 与手工 nibble placement；inverse `108673` |
| Raster manifest | 275B PBVM file-backed Golden | 手工字段/区域测试、canonical parser、CRC 与 BLAKE3 pin |
| Decoded Transport | 1,350B one-CW decoded、2,700B two-CW information | Raster → Robust LDPC syndrome/decode/re-encode → canonical padding → Transport CRC/parse |
| Full frame | G0/G1/G1-Transport/G1-Transport-2CW/G2 的 PBRW/PNG digest | test-only literal-geometry oracle 构造全部 8,294,400 BGRA bytes，不调用 raster encoder；PBRW 全字节比较；PNG 解码像素比较后再检查 stream digest |

完整 8.3 MB PBRW/PNG 不提交 Git；小型 protocol/LDPC/interleave/raster/PBVM
vectors 位于 `tests/golden/<category>/`。当前 registry 为 25 个 file-backed vectors
和 5 个完整 frame pins，总计 30 项。

三个 LDPC codeword 的冻结 BLAKE3-256 为：

- Robust: `84d43e6e4fb748df1de5f903f1f2bd77d672001784b40254788a8e93209117a9`
- Balanced: `515edea20642ff96d7ece9dbcaf877ea135826ad9043736fa46a61ce78714594`
- Fast: `bf428f59e26c7e715f79ae2fae191304c8501b82ea2bf1c5060b36d14650a667`

## 3. 三方判定模型

每个 file-backed vector 同时计算三种状态：

1. `file`：当前 `tests/golden` 文件及其 digest；
2. `recomputed`：当前实现重新生成的 bytes/digest；
3. `pinned`：registry 中冻结的 size 与 BLAKE3。

归因规则：

- file 匹配 pin、recomputed 不匹配：`VectorMismatch`，file 是 expected，
  recomputed 是 actual；
- recomputed 匹配 pin、file 不匹配：`FileDigestMismatch`，recomputed 是 expected，
  file 是 actual；
- 二者都不匹配 pin：`FileAndVectorMismatch`，输出三方 digest；由于没有任何
  unpinned byte stream 可称为权威 expected，诊断使用
  `byte_offset=not-authoritative`；
- generator/decoder 内部失败：`RecomputeFailed`，typed detail 上送，不调用
  `abort()`，不以空 vector 冒充期望内容。

size mismatch 的首个差异 offset 是两端较短长度，缺失端显示 `<eof>`。
`pinnedDigest` 始终填入报告，不允许以全零占位。

## 4. 完整 frame 的两层诊断

每个 frame 先生成独立 oracle BGRA，再运行生产 encoder：

1. 手工构造 oracle PBRW header + 完整 BGRA，与生产 PBRW 逐字节比较。若漂移，
   输出 `space=raw-pbrw byte_offset=<n> expected=<byte|eof> actual=<byte|eof>`；
2. 生产 PNG 解码回固定 1920×1080 BGRA，与同一 oracle 比较。像素漂移输出
   `space=decoded-png-bgra` 和 BGRA byte offset；
3. 只有像素完全一致后才判断 PNG stream digest。若压缩 stream 因受控依赖版本
   漂移而像素未变，输出 expected/actual BLAKE3，并明确
   `byte_offset=not-applicable`。

PBRW/raw pin 是版本无关的主要证据；PNG pin 绑定仓库记录的 libpng/zlib 基线，
是次级容器证据。

## 5. 统一 byte 诊断

工具可归因到输入字节的失败统一使用：

```text
space=<record|context|raw-pbrw|decoded-png-bgra> byte_offset=<n> expected=<...> actual=<...>
```

- CRC mismatch：expected 为 recomputed CRC，actual 为 stored CRC；
- 截断/尾随：offset 指首个缺失/额外 byte，缺失端用 `<eof>`；
- nested recovery 同时报告 `windowByteOffset`、`innerByteOffset` 与
  `dataRegionByteOffset = windowByteOffset + innerByteOffset`；
- digest-only drift 没有可信 byte oracle 时使用 `byte_offset=not-applicable` 或
  `not-authoritative`，不伪造 byte expected。

## 6. 校验与受控生成命令

```powershell
& .\build-phase0-tooling-release\tests\golden\Release\PBGoldenVectorCheck.exe `
  .\tests\golden

& .\build-phase0-tooling-release\tools\Release\PBVectorGen.exe `
  dump-manifest

& .\build-phase0-tooling-release\tools\Release\PBVectorGen.exe `
  write-golden .\out\golden

& .\build-phase0-tooling-release\tools\Release\PBVectorGen.exe `
  write-corpus .\out\corpus --category transport --seed 12648430

& .\build-phase0-tooling-release\tools\Release\PBVectorGen.exe `
  write-frame .\out\g1-transport.pbrw --vector g1-transport --format pbrw
```

`PBVectorGenIntegration` 在 build tree 中生成两份 Golden、两份相同 seed corpus，
与提交文件逐文件比较 size/digest，验证已有相同内容的幂等行为、冲突拒绝，且把
生成的 PBRW/PNG 交给 `PBFrameInspector --recovery`。

## 7. 禁止自动 re-pin

正常修复流程不得提供 “accept current output”、自动修改 registry digest 或覆盖
冲突目标的选项。若协议/参考 profile 的合法变更确需更新 pin，必须单独完成：

1. 先审查设计/成熟度和 wire compatibility；
2. 更新独立 oracle 或已有权威 array，而不是只改生产 serializer；
3. 证明旧 pin 为什么需要替换，并记录首个 byte 差异；
4. 在临时目录运行 `write-golden` / `write-frame`；
5. 人工审查 bytes、size、CRC、digest 与完整测试；
6. 以明确的协议变更提交更新 pin。

当前 generator 在 recomputed digest 不等于冻结 pin 时拒绝写入，在已有目标内容不
同的时候拒绝覆盖；因此不能被用作绕过 Golden Gate 的 re-pin 工具。
