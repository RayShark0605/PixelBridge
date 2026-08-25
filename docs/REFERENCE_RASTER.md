# PixelBridge Phase-0 CPU 参考 raster/demod（PB-ReferenceRaster-1）

本文档记录 `libs/PBModulation` 实现的 Phase-0 确定性参考调制/解调（设计文档
§16、§17.4、§38.2）的冻结参考 profile、字节级契约、Golden Vector 与第三方
版本基线（设计 §39.1 Phase-0 记录要求）。

## 1. 状态与定位

- **状态：Phase-0 参考候选（`PB-ReferenceRaster-1`），不是 certified
  profile。** 认证冻结 Gate（设计 §34.6）在后续阶段执行；本文档中的几何与
  星座参数在冻结前只作为参考实现与 Golden Vector 的锚点。
- 纯 CPU、无 GPU/Qt、无网络；不追求性能（设计 §3：先简单参考实现）。
- 全部整数运算；所有尺寸/偏移/容量算术使用 checked arithmetic；任何歧义、
  越界、冲突、算术溢出 fail-closed（设计 §3/§4.3：任一错误 → 整帧按 erasure
  拒绝，不输出 payload，不猜测）。
- 目标：建立 **protocol bytes → visual frame → protocol bytes** 的 bit-exact
  Golden Vector，并可完整承载一个 2025B Robust Inner-FEC codeword（端到端
  承载证明见 §8.4）。

## 2. 实现产物

| 路径 | 内容 |
| --- | --- |
| `libs/PBModulation/include/pbmodulation/reference_visual_profile.h` | 冻结常量、14 区域表、manifest 序列化/解析/校验 API |
| `libs/PBModulation/include/pbmodulation/reference_raster.h` | 帧编码/解调 API（含无分配 `DecodeReferenceFrameInto`） |
| `libs/PBModulation/include/pbmodulation/frame_io.h` | raw（PBRW v1）与 PNG 帧容器 I/O API |
| `libs/PBModulation/include/pbmodulation/modulation_result.h` | `ModulationStatus` / `ModulationResult<T>` / 错误码（沿用 PBProtocol/PBInnerFec 的 status/result 约定） |
| `libs/PBModulation/src/*` | 实现（私有 `raster_internal.h` 共享像素/灰码/位序 helper） |
| `tests/PBModulation/*` | Catch2 测试（40 个 test case），含独立 PNG 解码 oracle（`png_oracle.h`） |
| `fuzz/reference_raster_fuzz.cpp` | 确定性 mutation fuzz driver（Clang/libFuzzer 与 MSVC 双模式） |
| `fuzz/corpus/reference-raster/*` | 6 个小体积 corpus 回放种子 |

## 3. 冻结参考 profile（PB-ReferenceRaster-1）

### 3.1 Canvas 与星座

- 逻辑 canvas 固定 **1920×1080**，1:1 物理像素；像素格式 **BGRA**
  （每像素 4 字节：B,G,R,A），全帧 **A ≡ 255**（任何像素 A≠255 →
  `AlphaChannelViolation`）。
- **16 个 Luma level**：`L_i = 8 + 16*i`，i=0..15，即 `{8, 24, 40, ..., 248}`。
  数据承载像素一律 `(B,G,R) = (L_i, L_i, L_i)`（luma-only 契约：
  `B != G || G != R` → `ChromaChannelMismatch`）。
- 星座距范围两端各至少 7 步（guard band），保证 demod margin 内不会与
  0/255 端点混淆。

### 3.2 符号与位序

- **4-bit 符号** `s` 通过 Gray 编码映射到 level：`g = s ^ (s >> 1)`，像素值
  `L_g`。`s = 0`（`g = 0`，level 8）是 reserved 符号。
- **位流 LSB-first**：流 bit `b` 属于字节 `S[b/8]` 的 bit `b%8`（与
  PBInnerFec systematic bit order 一致）。
- 区域内第 k 个符号（**行主序**：左→右、上→下）承载流 bit `4k..4k+3`，符号内
  bit i（LSB 优先）= 流 bit `4k+i`。
- lane 打包：每字节装 2 个符号，偶数符号索引占低 nibble，奇数占高 nibble。

### 3.3 区域表（14 个矩形，精确无重叠划分 1920×1080）

| # | 区域 | 类型 | x | y | w | h | 内容 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | GuardTop | Guard | 0 | 0 | 1920 | 8 | 全黑（level 0 像素值 0） |
| 2 | SyncTop | Sync | 0 | 8 | 1920 | 8 | 8×8 checker：`((x/8 + y/8) % 2 == 0) ? 0 : 255` |
| 3 | BootstrapA | BootstrapA | 0 | 16 | 1920 | 8 | 240 个 8×8 符号；符号 0..87 = 44B PB-Bootstrap-1 record，其余 reserved（符号 0） |
| 4 | Control | Control | 0 | 24 | 1920 | 16 | 480 个 8×8 符号 = 240B/帧 control 窗口 |
| 5 | PilotTop | Pilot | 0 | 40 | 1920 | 16 | 冻结标定 raster（§3.5） |
| 6 | GuardTopInner | Guard | 0 | 56 | 1920 | 8 | 全黑 |
| 7 | GuardLeft | Guard | 0 | 64 | 16 | 952 | 全黑 |
| 8 | DataGrid | DataGrid | 16 | 64 | 1888 | 952 | 472×238 个 4×4 tile = 56,168B/帧 |
| 9 | GuardRight | Guard | 1904 | 64 | 16 | 952 | 全黑 |
| 10 | GuardBottomInner | Guard | 0 | 1016 | 1920 | 8 | 全黑 |
| 11 | SyncBottom | Sync | 0 | 1024 | 1920 | 8 | 同 §3.3 规则（全局坐标） |
| 12 | BootstrapB | BootstrapB | 0 | 1032 | 1920 | 8 | 240 个 8×8 符号；符号 152..239（x 1216..1919）= 同一 44B record，与 A 空间分离 |
| 13 | PilotBottom | Pilot | 0 | 1040 | 1920 | 16 | 与 PilotTop 相同（局部坐标定义） |
| 14 | GuardBottom | Guard | 0 | 1056 | 1920 | 24 | 全黑 |

注：Sync checker 使用**全局坐标** `(x/8 + y/8)` 决定相位，因此 SyncTop（y/8=1）
与 SyncBottom（y/8=128）**相位相反**（顶部 x0 处为 255，底部 x0 处为 0）。
这是冻结行为，被 manifest 与 Golden Vector 锚定。

**容量**：Bootstrap 44B（A/B 双副本，计一次）、Control 240B/帧、Data
56,168B/帧；每帧协议字节 **56,452**（≥ 2025B Robust codeword，余量
54,143B 由发送方零填充，raster 层对内容不解释）。

### 3.4 区域内容契约（编码侧）

- Guard：全黑像素 `(0,0,0,255)`。
- Sync / Pilot：§3.3/§3.5 冻结 raster，逐像素确定。
- BootstrapA/B、Control：符号 lane（§3.2）；BootstrapA record 位于符号
  0..87，BootstrapB record 位于符号 152..239，其余 lane 位置一律 reserved
  符号 0（level 8）。
- DataGrid：4×4 tile 逐 tile 恒定 level（一个 tile 承载一个符号）。

### 3.5 Pilot 冻结 raster（上/下相同，局部坐标 localX = x - region.x）

| localX 范围 | 内容 |
| --- | --- |
| 0..255 | 16 个 16×16 ladder：`localX/16` 号 level（L_0..L_15），覆盖全星座 |
| 256..271 | 黑参考（0） |
| 272..287 | 白参考（255；margin 规则接受为 L_15，用于标定亮端点） |
| 288..303 | 中灰参考（128；恰为 L_7/L_8 中点，demod 判 AmbiguousLevel） |
| 304..335 | 8×8 checker：`(((localX-304)/8 + (y-region.y)/8) % 2 == 0) ? 0 : 255` |
| 336..1919 | reserved 填充 128（恰为 L_7/L_8 中点，任何 demod 尝试 fail-closed） |

注：设计 §16.5 要求 Certified Profile 的 Pilot 还包含 active chroma
星座状态与 neutral chroma 参考；本 profile 是 luma-only Phase-0 参考
候选（数据承载不用 chroma 通道，冻结像素始终 B=G=R），因此 Pilot 不
含 chroma 训练符号，chroma pilot 随 certified profile 定义（见 §11）。

## 4. Manifest（PBVM，275B）

机器可读 canonical 序列化，显式 little-endian，尾部 CRC-32C：

| 偏移 | 大小 | 字段 |
| --- | --- | --- |
| 0 | 4 | magic `"PBVM"` |
| 4 | 1 | version = 1 |
| 5 | 1 | layout version = 1 |
| 6 | 2 | reserved = 0 |
| 8 | 4 | canvas width u32（1920） |
| 12 | 4 | canvas height u32（1080） |
| 16 | 1 | region count（14） |
| 17 | 1 | bits per symbol（4） |
| 18 | 1 | level count（16） |
| 19 | 1 | level base（8） |
| 20 | 1 | level step（16） |
| 21 | 1 | data tile width（4） |
| 22 | 1 | data tile height（4） |
| 23 | 1 | lane symbol width（8） |
| 24 | 1 | lane symbol height（8） |
| 25 | 8 | reserved = 0 |
| 33 | 14×17 | 区域表（§3.3 顺序）：type u8 + x u32 + y u32 + w u32 + h u32 |
| 271 | 4 | CRC-32C over 0..270 |

`ParseReferenceRegionManifest` 只接受规范 manifest：任何字段偏差（几何、参数、
顺序、reserved、CRC）fail-closed。`ValidateReferenceVisualProfile()` 额外以
运行时证明全部冻结不变量：每个矩形在 canvas 内（64-bit 边界算术防 32-bit
回卷）、两两无重叠、总面积恰为 canvas 面积（与边界检查一起证明精确划分）、
星座参数 8..248 step 16、lane/tile 对齐整除、容量等式。

## 5. Raster 编码/解调契约

### 5.1 编码（`EncodeReferenceFrame`）

- 输入：`ReferenceFrameInput{ array<byte,44> bootstrapRecord; span<240>
  controlWindow; span<56168> data }`；输出 `span<byte>` 必须恰为 8,294,400B
  （`kReferenceFrameBgraBytes`），否则 `OutputBufferTooSmall` /
  `InvalidInput`。
- 每帧从 payload + 固定区域**完整重建**（无帧间状态）：先整体清零，再按
  区域表逐区域写入，每个像素恰被写一次；同一输入永远产生同一字节序列
  （无时间戳、无分配器依赖内容、无持久状态）。
- raster 层对 Bootstrap/Control/Data 内容**不解释**（bootstrap 的结构性
  校验归 `pbprotocol::ParseBootstrapRecord`；control 的 record 分片/重组归
  控制面）。

### 5.2 解调（`DecodeReferenceFrame` / `DecodeReferenceFrameInto`）

三遍处理，任何失败 → 无输出（整帧 erasure，§4.3）：

1. **Pass 1 冻结区逐像素 byte-exact 校验**（Guard/Sync/Pilot 全部像素）：
   失配 → `FrozenRegionMismatch`，offset = 首个失配像素线性索引
   （`y*1920 + x`）。
2. **Pass 2** 在冻结表中定位四个数据承载区域（缺失 = 表被篡改 →
   `InternalInvariantViolation`）。
3. **Pass 3 符号解调**，每符号/块（8×8 符号 = 64 像素，4×4 tile = 16 像素）：
   - 逐像素先校验 A=255（offset = 首个违例像素）与 B=G=R（同上）；
   - 整数均值 `mean = lumaSum / N`（N = 块像素数：4×4 tile 为 16、8×8 符号为 64；截断除法，纯整数）；
   - 最近 level 判定（margin 严格小于半距 8，即距离 ≤ 7），**不猜测**：
     - `mean ∈ [8, 248]`：`diff = mean - 8`，`remainder = diff % 16`；
       `remainder == 8`（恰在两 level 正中）→ `AmbiguousLevel`；
       `remainder < 8` → 低 level，否则高 level。
     - `mean < 8`：`8 - mean > 7` → `OffConstellationLevel`；否则 level 0。
       注意**不对称**：`mean = 0`（距离 8）被拒绝，`mean = 1`（距离 7）被
       接受。
     - `mean > 248`：`mean - 248 ≤ 7` → level 15（`mean = 255` 被接受）；
       否则 `OffConstellationLevel`。
     - level 判定的错误 offset = 块原点像素。
   - **reserved lane 符号必须解调为 0**，否则 `NonZeroReservedByte`
     （offset = 该符号块原点）。
   - **Bootstrap A≠B**（双副本逐字节比较）→ `TornFrame`（设计 §16.4
     torn 检测），offset = B 副本首个 record 符号原点
     （`1032*1920 + 1216`）；整帧拒绝。
   - Bootstrap 成功输出 = A 副本（A/B 已验证一致）。

- **线程安全**：所有公开函数为无状态纯函数（仅读冻结常量表），noexcept
  入口；`DecodeReferenceFrame` 的唯一堆分配（56,168B data）失败时
  `bad_alloc` 被捕获并返回 `MemoryAllocationFailure`，不终止进程。失败时
  任何调用方输出缓冲（含 `outWidth/outHeight` 出参）都不被写入。

## 6. 帧容器 I/O

### 6.1 raw 容器（PBRW v1，28B 头）

| 偏移 | 大小 | 字段 |
| --- | --- | --- |
| 0 | 4 | magic `"PBRW"` |
| 4 | 1 | version = 1 |
| 5 | 3 | reserved = 0 |
| 8 | 4 | width u32 LE |
| 12 | 4 | height u32 LE |
| 16 | 4 | pixelBytes u32 LE（必须 = width×height×4） |
| 20 | 8 | reserved = 0 |
| 28 | … | BGRA 像素（行主序，无 padding） |

解码校验顺序（fail-closed，全部 checked arithmetic）：
size < 28 → `TruncatedInput(offset=size)`；magic → `InvalidMagic(0)`；
version → `UnsupportedVersion(4)`；reserved3 → `NonZeroReservedByte(5)`；
读 width/height/pixelBytes/reserved8（截断 → `TruncatedInput`）；
reserved8 → `NonZeroReservedByte(20)`；维度 ∈ [1, 16384]
（`kMaximumFrameDimension`，防恶意头驱动无界分配）且 checked 乘法
w×h×4；pixelBytes ≠ w×h×4 → `InvalidInput(16)`；
size < 28+w·h·4 → `TruncatedInput`；size > → `TrailingBytes`；
outBgra.size() ≠ w·h·4 → `OutputBufferTooSmall`。成功后才写 outBgra 与
outWidth/outHeight。

### 6.2 PNG（libpng 1.6.58 + zlib 1.3.2）

**编码器（Golden 固定参数）**：8-bit RGBA（BGRA→RGBA 仅在边界显式换序）、
non-interlaced、`PNG_COMPRESSION_TYPE_DEFAULT`、压缩级固定 6、
`png_set_filter(pngPtr, PNG_FILTER_TYPE_BASE, PNG_FILTER_NONE)`——注意
`PNG_FILTER_NONE`（0x08）是 libpng 的**滤波器选择位图**，语义为“只允许
filter 值 0”，即所有行不做行滤波。输出写入**预分配上界缓冲**，上界
`ComputePngOutputUpperBound`：

```
totalRaw       = height * (width*4 + 1)          // 每行 filter 字节 + RGBA 行
upperBound     = totalRaw
               + 5 * (totalRaw/65535 + 1)        // stored deflate 块头
               + totalRaw/32                     // 膨胀 margin（实测 0.0018）
               + 4096                            // 签名 + chunk 框架
```

越界即 `PngEncodeError`（longjmp，不扩容、不二次分配）；zlib 1.3.2 对冻结
canvas 的不可压缩内容实测最坏膨胀 0.0018，`totalRaw/32` 留有宽裕有限余量。

**解码器（严格流契约）**：
- 期望尺寸先过 `ValidateFrameDimensions`（checked 乘法先于任何分配）；
  outBgra 必须恰为 w·h·4（否则 `OutputBufferTooSmall`）；输入 < 8B →
  `TruncatedInput`。
- 自定义 longjmp 错误处理器：任何畸形流（签名、chunk CRC、zlib 流、IHDR
  内容校验）→ `PngDecodeError`，**绝不** `PNG_ABORT` 终止进程。
- `png_read_info` 后 w/h 必须与期望一致（否则 `FrameGeometryMismatch`）。
- 变换链（libpng 内部应用顺序固定：EXPAND → 16_TO_8 → PACK → FILLER）：
  `png_set_interlace_handling`（**必须**在 `png_read_update_info` 前；
  非 interlaced 输入是 no-op 返回 1 pass）→ `expand_gray_1_2_4_to_8` →
  `packing` → `expand` → `tRNS_to_alpha` → `gray_to_rgb` →
  `png_set_add_alpha(0xFF, PNG_FILLER_AFTER)` → `strip_16`。
  **`png_set_add_alpha`（FILLER + ADD_ALPHA）是必需的，不是
  `png_set_filler`**：libpng 仅在 ADD_ALPHA 同置时才把变换后 color type
  升级为 RGBA（`png_read_transform_info`），plain filler 会留下“行数据 4B
  /像素但 color type 报 RGB”的不一致状态。
- `png_read_update_info` 后必须为 8-bit RGBA 且 `rowbytes == w*4`，否则
  `UnsupportedPngFormat`。
- **interlaced（Adam7）输入被接受**：`png_read_row` 每 pass 只填充该 pass
  的行，必须按 pass×height 调用；所有行累积进**私有 RGBA staging 缓冲**，
  全部行成功**且**流末端校验（IEND 存在、无 trailing
  bytes）通过后才**一次性** RGBA→BGRA 转写 outBgra。mid-stream 失败
  （截断、chunk 损坏、缺 IEND）→ `PngDecodeError`，IEND 后仍有剩余字节
  → `TrailingBytes`，两种情况均 **outBgra 零部分写入**（有专门回归测试）。
- `bad_alloc` → `MemoryAllocationFailure`；`png_read_end` 后（缺 IEND →
  libpng 报错 → `PngDecodeError`）若流仍有剩余字节 → `TrailingBytes`
  （offset = 已消费位置）。严格流：IEND 后任何字节都拒绝。

## 7. 错误码与 offset 语义

`ModulationErrorCode`（1..21）：InvalidInput、OutputBufferTooSmall、
NonZeroReservedByte、InvalidMagic、UnsupportedVersion、TruncatedInput、
TrailingBytes、CrcMismatch、FrameGeometryMismatch、OffConstellationLevel、
AmbiguousLevel、ChromaChannelMismatch、AlphaChannelViolation、TornFrame、
FrozenRegionMismatch、ManifestValidationFailed、UnsupportedPngFormat、
PngDecodeError、PngEncodeError、MemoryAllocationFailure、
InternalInvariantViolation。

`ModulationError.offset` 语义：帧级错误 = canvas 线性像素索引
`y*1920 + x`；容器/manifest 错误 = 输入字节偏移；不适用 = 0。
`ModulationStatus::Failure` / `ModulationResult::Failure` 传入
`None` 会被规范化为 `InternalInvariantViolation`（防御误用）。

## 8. Golden Vector

### 8.1 规范 payload（五个固定帧）

| 帧 | Bootstrap（44B） | Control（240B） | Data（56,168B） |
| --- | --- | --- | --- |
| G0 zero | 全 0 | 全 0 | 全 0（全 lane level L_0=8） |
| G1 canonical | §8.2 黄金 PB-Bootstrap-1 record | 67B 黄金 PB-Control-1 SessionDescriptor record + 173B 零填充 | 2025B Robust QC-LDPC golden codeword + 54,143B 零填充 |
| G1-Transport | 同 G1 | 同 G1 | 1 个 Robust codeword：1,350B provisional Transport information block → 2,025B codeword，之后零填充 |
| G1-Transport-2CW | 同 G1 | 同 G1 | 2 个连续 Robust codeword（ordinal 0/1），之后零填充 |
| G2 max | 全 0xFF | 全 0xFF | 全 0xFF（全符号 0xF → Gray 索引 8 → 全 tile level L_8=136） |

G1-Transport 两个 fixture 明确为 **non-interleaved Phase-0 reference**。当前 wire
schema 没有正式 `InterleaveProfileId` binding，因此完整 frame 不隐式应用
interleave；独立 `PBInterleave` Golden/fuzz 验证 bit-exact 映射能力。

### 8.2 G1 黄金 record（字节复用 PBProtocol 既有黄金，不重复定义）

- **Bootstrap（44B，设计 §8.2）**：magic "PBRG"、bootstrapVersion 1、
  protocolVersion 1、layout 1、profileId `0x0102030405060708`、tag
  `0x81DF204BD997BAD0`、frameSequence `0x1112131415161718`、controlEpoch
  `0x21222324`、flags 0、CRC-32C `0xD488E1EA`。与
  `tests/PBProtocol/test_bootstrap_control_codec.cpp` 的 `kBootstrapGolden`
  逐字节相同。
- **Control（67B）**：type 1（SessionDescriptor）、seq
  `0x0102030405060708`、tag 同上、37B SessionDescriptor payload（与
  `kSessionDescriptorGolden` 相同）、CRC-32C `0xA13883C8`。
- **Data codeword（2025B）**：`pbinnerfec::EncodeQcLdpcCodeword(Robust)`，
  信息位 = SplitMix64(0xC0FFEE) 逐位前 10800 bit（与 PBInnerFec Golden 同一
  模式），前 16B pinned hex `e6e5215a64cb2a5199ac6341bd74ad28`
  （`test_inner_fec_encoder_golden.cpp` 与 `modulation_test_helpers.h` 双
  pinned，防两套 suite 漂移）。

### 8.3 Digest pin（BLAKE3，小写 hex；`pbprotocol::ComputeBlake3Digest`）

raw 容器摘要（PBRW 28B 头 + 8,294,400B 像素）是**主 pin**（版本无关）；
PNG 摘要（完整 PNG 流）是**次 pin**（绑定 libpng 1.6.58 + zlib 1.3.2 +
§6.2 固定参数）：

| 帧 | raw（PBRW 容器）BLAKE3 | PNG 字节 BLAKE3 |
| --- | --- | --- |
| G0 | `29ed5c8725a5d939685249be82ef5e3717c5ceb3545001ab0bedddf7e2500b0b` | `20ea184adc00f164b1c8fe71b7c568f240b1e8428991e867e0e0b88829144002` |
| G1 | `54c23b3e558bf4805afadb6193217d9ef111bcb5975ee1336a633ad569bf0d57` | `f65997df4b612412cb2ce50de3c0b9f9fd542bc84842b9f084b76c5f6e290a89` |
| G1-Transport | `6005706aba9d6676c59501924f9bbeae592f12e29a36184d02f34e329a47ea45` | `e0bf733622977cd5f08e36bf2e60763945442499e0494a0b4fb2baad0580020c` |
| G1-Transport-2CW | `fac16d8024a863a21b693270590c03bc7ecf1ee2d18721ad8054d3faec57bd11` | `30ba615d40692273a410c18d7b4a0b1891e66fe3fadf996c3351591ed2e57357` |
| G2 | `ccd24926d8e578fc65dcc4aaeafbeed1da3ce19c43a4d5709471344ddc3ffdcc` | `919510014ecab9fc5b56246a8c71d47e01f8b9824c90debb4ade116d86d544cf` |
| manifest（275B） | `a7e31cbd7cfa6865f8bc029a78d58d065990e732f6386613329f2dd07602003b` | — |

pin 位置：既有 G0/G1/G2/manifest 继续由 `tests/PBModulation` 固定；统一五帧
registry 位于 `libs/PBGoldenVector`，由 `PBGoldenVectorTests` 与
`PBGoldenVectorCheck` 交叉验证。275B PBVM 同时作为正常 file-backed Golden
提交在 `tests/golden/raster/reference-raster-manifest.bin`。

### 8.4 交叉验证（独立于 raster 层）

G1 解调结果必须通过以下独立协议/FEC 校验（`test_reference_golden.cpp`
"G1 payload cross-validates..."）：

1. 44B bootstrap 过 `pbprotocol::ParseBootstrapRecord`，字段全等 §8.2 黄金；
2. control 窗口前 67B 过 `ParseControlRecord`，payload 全等 37B 黄金，
   尾部 173B 全零；
3. data 前 2025B 过 `pbinnerfec::ComputeQcLdpcSyndrome(Robust)` 且 syndrome
   全零（**Inner-FEC codeword 端到端承载证明**），前 16B 与 pinned hex
   一致，尾部 54,143B 全零；
4. raw 与 PNG 双通道解码回同一 raster，且 demod 回同一 payload（bit-exact
   round-trip）。
5. Golden harness 另以 test-only literal-geometry oracle 手工构造全部
   8,294,400 BGRA bytes（不调用 `EncodeReferenceFrame`），与生产 PBRW 完整
   比较；PNG 解码像素与同一 oracle 比较。raw/pixel drift 报首个 byte offset、
   expected/actual；仅压缩 stream 漂移则报告两端 digest 和
   `byte_offset=not-applicable`。
6. `PBFrameInspector --recovery` 对 G1-Transport one/two-codeword fixture 验证
   syndrome、perfect-LLR decode、re-encode、info-block 零填充、Transport CRC
   及 Bootstrap SessionTag。

### 8.5 再生成方法

摘要值经 §8.4 独立交叉验证后冻结。日常命令使用 `PBGoldenVectorCheck` 和
`PBVectorGen dump-manifest`；`write-frame` 只有在当前输出已经匹配冻结 pin 时
才写文件。工具没有 “接受当前输出” 或自动 re-pin 旁路。合法协议/profile 变更
必须先更新独立 oracle、人工审查首个 byte diff，再以单独协议变更提交更新 pin；
详见 `docs/GOLDEN_VECTOR_HARNESS.md`。

## 9. 依赖与版本基线（设计 §39.1 Phase-0 记录）

| 项 | 版本 | 备注 |
| --- | --- | --- |
| vcpkg builtin-baseline | `75672db6bd812b060482b0f00b5a16b18a0c0f07` | 本地 `D:/vcpkg` 恰在该 commit（tag `2026.06.01-68-g75672db6bd`） |
| libpng | 1.6.58 | manifest 声明 `version>= 1.6.58` + baseline 钉 registry 快照，解析结果恰为 1.6.58（已用 `vcpkg_installed/vcpkg/status` 验证） |
| zlib | 1.3.2 | libpng 传递依赖，status 验证 |
| MSVC | 14.44.35207（VS2022 17.14.51 x64） | Release，/W4 /WX |
| CMake | 3.31.6 | 生成器 Visual Studio 17 2022 |
| Catch2 | 3.15.0 | 测试框架（tests feature） |

**libpng pin 方式说明**：计划原稿要求精确 pin 字段，但本 vcpkg 工具（即
baseline 提交）的 manifest schema 只接受顺序约束字段（`version>` /
`version>=`；`version`、`version=`、`version<` 均被拒绝，已实测）。可复现性
由 `builtin-baseline` 保证：baseline 钉死 registry 快照，`>= 1.6.58` 在该
快照下只能解析为 1.6.58。**若 baseline 被升级，PNG 次 pin 摘要必须重新
生成并重新 pin；raw 主 pin 不受 libpng/zlib 版本影响。**

## 10. Fuzz 与 corpus

`fuzz/reference_raster_fuzz.cpp`（`PBModulationReferenceRasterFuzz`）：
- Clang/libFuzzer（`PB_USE_LIBFUZZER=1`）：仅暴露 `LLVMFuzzerTestOneInput`。
- 其他后端（本环境为 MSVC）：确定性 mutation runner，CLI
  `[iterations] [seed]`（默认 1000 / `0x50424D4F44550001`）或
  `--input <corpus-file>`；mutation 序列 SplitMix64 播种，CTest 回放完全
  可复现。
- 输入分发：275B → manifest 解析；8,294,400B → 整帧 demod；其余 →
  raw 容器解码（恶意头不驱动分配：头字段全过 checked 校验才按声明尺寸
  分配，否则 4B probe span 让解码器走拒绝路径）+ PNG 解码（期望尺寸从
  前 2 字节派生，上界 64）+ raw 编码边界探针。
- mutation 分布：50% 全帧随机破坏（1-4 段 × 1-64B）、30% 随机小 buffer
  （0..16,384B）、15% 随机 275B manifest、5% 截断帧前缀。base 帧 = 零
  payload 合成帧。
- **8.3MB 帧不进 corpus**（repo 体积决策，与其他 corpus 全部 ≤64KB 的
  约定一致）；smoke 在内存中合成全帧。corpus 仅 6 个小种子：
  valid-manifest（275B）、corrupted-manifest（275B，CRC 末字节翻转）、
  valid-raw-small（1,052B，16×16）、valid-png-small（406B，10×17）、
  truncated-png（100B）、invalid-raw-magic（32B）。
- CTest：`PBModulationReferenceRasterFuzzSmoke`（MSVC：1000 次播种迭代，
  TIMEOUT 600；Clang：`-runs=2000` 从 corpus 目录起步）+ 6 个
  `PBModulationReferenceRasterCorpus.*` 回放（TIMEOUT 30）。
- MSVC 构建树中该 driver 与 `PBModulation`/`PBProtocol` 库同为
  `/Zi /fsanitize=address` 仪器化 + `_DISABLE_STRING/VECTOR_ANNOTATION`
  （与既有 fuzz 目标同一注解一致性约束），ASan runtime dll 经 POST_BUILD
  复制。
- 该 driver 与新 Transport/Interleave/LDPC focused drivers 共同属于
  `PBParserFuzzHarness`，对应 smoke/corpus 统一带 `parser-harness` label。

## 11. 限制与后续工作（显式声明）

1. **Bootstrap/Control lane 无强 FEC**：仅靠 A/B 双副本 torn 检测 + record
   自带 CRC-32C，与状态文档 "Control/Bootstrap visual FEC 为 separate work"
   一致。
2. **无 soft/LLR 解调路径**（Phase 2 工作）：参考 demod 是 byte-exact 严格
   路径（margin ≤7、冻结区 byte-exact），低置信度输出整帧 erasure 而非
   猜测。
3. **PNG 摘要绑定 libpng/zlib 版本**：故 raw BGRA 摘要为主 pin（§9）。
4. **Data lane 尾部零填充**由发送方产生，raster 层对内容不解释（与 §9.4
   零 padding 约定一致）；56,168B 容量承载多 codeword/多帧拼接的协议解释
   归数据面后续工作。
5. **Control 每帧窗口固定 240B**：跨帧 record 分片/重组属控制面后续工作；
   raster 层只搬运 240B 窗口字节。
6. **Sync 上下相位相反**（§3.3 注）：全局坐标定义的冻结行为。
7. **margin 不对称**：mean 0 拒绝 / mean 255 接受（§5.2）：半距严格小于 8
   的整数边界使然，非 bug。
8. 参考路径**不做性能优化**，无吞吐指标承诺。
9. 认证（§34.6 Freeze Gate）、GPU/SIMD 路径、视频/离线通道（§7）均不在
   本 profile 范围内。
10. 完整 raster 没有 interleave profile signal；Inspector 明确输出
    `interleave=not-bound`，不会按内容猜测。Phase-0 interleave reference 只在
    独立 Golden/unit/fuzz 中使用。

## 12. 复现验证（本环境）

```
# Release harness
cmake --build build-phase0-tooling-release --config Release --parallel
ctest --test-dir build-phase0-tooling-release --build-config Release --output-on-failure
build-phase0-tooling-release\tests\golden\Release\PBGoldenVectorCheck.exe tests\golden

# 生成可由 Inspector 恢复的完整 fixture（写入 build/output 目录，不提交 frame）
build-phase0-tooling-release\tools\Release\PBVectorGen.exe write-frame `
  build-phase0-tooling-release\g1-transport.pbrw --vector g1-transport --format pbrw
build-phase0-tooling-release\tools\Release\PBFrameInspector.exe --recovery `
  build-phase0-tooling-release\g1-transport.pbrw
```

全量测试中与本任务无关的既有失败（若有）单独报告，不代改。
