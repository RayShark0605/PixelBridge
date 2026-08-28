# PB-LocalDesktopBootstrap-X1

## 1. 范围与兼容性

本实验映射把现有 **PB-Bootstrap-1 的完整 44 字节 canonical record** 放到真实 LocalDesktop visual frame。它是 SDR / Luma-only 的 Bootstrap 捕获诊断基线，不是已认证 Data Plane Profile，不声明文件传输 goodput，也不把尚未实现的 Control/Data 区域当成可用载荷容量。

- `VisualProfileId = 0x50424C4442533031`，类型仍为协议既有 `uint64_t`。
- `VisualLayoutVersion = 2`。
- 名称 `PB-LocalDesktopBootstrap-X1`：专属实验标识，不复用 `PB-ReferenceRaster-1`。
- 不修改 PB-Bootstrap-1 的版本、字段布局、CRC、序列化或已有 Golden。
- 不修改 `PB-ReferenceRaster-1` 的像素、区域、API、Golden 或 Encoder 旧命令行默认行为。
- 设计依据限于总体设计 §8（Bootstrap）、§16（定位、双区一致性、Timing）、§17.4～17.6（Direct-Level/Signal）、§26（捕获归一化）以及 §34（候选与冻结边界）。本实验的可缩放 Bootstrap 不是 §34.4 的严格 1:1 高阶 Data Profile 认证。

公共编码入口是 `pbmodulation::EncodeLocalDesktopBootstrapFrame`，位于 `libs/PBModulation/include/pbmodulation/local_desktop_bootstrap.h`。定位/解码入口是 `DecodeLocalDesktopBootstrap`，位于同目录的 `local_desktop_decode.h`。两个入口都独立于 Qt、Windows 捕获后端和应用会话状态。

## 2. 44 字节逻辑记录没有变化

所有多字节字段 little-endian，不能持久化 C++ struct 的内存布局。

| Offset | Bytes | 字段 |
|---:|---:|---|
| 0 | 4 | `PBRG` |
| 4 | 1 | BootstrapVersion = 1 |
| 5 | 1 | ProtocolMajor = 1 |
| 6 | 1 | ProtocolMinor = 0 |
| 7 | 1 | VisualLayoutVersion = 2，仅限本实验映射 |
| 8 | 8 | VisualProfileId = `0x50424C4442533031` |
| 16 | 8 | SessionTag |
| 24 | 8 | FrameSequence |
| 32 | 4 | ControlEpoch |
| 36 | 4 | Flags，当前必须为零 |
| 40 | 4 | BootstrapCrc：CRC-32C over bytes `[0,40)` |

编码器必须先调用既有协议验证，再检查此映射的 ID/layout，全部成功后才能写像素。错误 offset 是逻辑 record 的字节 offset。错误路径不能部分改写输出；输入允许与输出重叠，编码器在输出写入前缓存完整 record、RS 和 Timing。

## 3. 冻结的逻辑画布与区域

画布固定为 **1920×1080 BGRA8**，row-major、无 row padding。所有像素 `B=G=R`、`A=255`：

- black = 32；
- white = 224；
- 其他 reserved diagnostic background = 128。

生产编码器每帧重画全部 8,294,400 字节，不依赖 swap-chain 后缓冲区旧内容。坐标采用左闭右开物理像素矩形，形式 `(x,y,width,height)`。

| 区域 | 矩形 |
|---|---|
| TL marker | `(16,16,64,64)` |
| TR marker | `(1840,16,64,64)` |
| BL marker | `(16,1000,64,64)` |
| BR marker | `(1840,1000,64,64)` |
| Bootstrap A | `(96,16,608,64)` |
| Bootstrap B | `(1216,1000,608,64)` |
| Timing 0 / 1 / 2 | `(96,160,128,128)` / `(896,160,128,128)` / `(1696,160,128,128)` |
| Timing 3 / 4 / 5 | `(96,476,128,128)` / `(896,476,128,128)` / `(1696,476,128,128)` |
| Timing 6 / 7 / 8 | `(96,792,128,128)` / `(896,792,128,128)` / `(1696,792,128,128)` |

这 15 个区域合计 241,664 像素，彼此不重叠并且完整位于画布内；生产代码的 `static_assert`、独立 Python painter、独立 C++ fixture painter 三处验证/固定该布局。其余区域只保留灰色背景，不承诺任意未采样处的 mixed-frame 检测能力。

### 3.1 四角 marker

每个 marker 的 64×64 区域由 **4 像素白 quiet border + 7×7 个 8×8 二值 module** 组成。基准图形：外周全黑、中央 3×3 全黑、二者之间白 ring。

角色仅覆盖以下八个 module，bit 0 到 bit 7 的精确顺序为：

```text
(0,0), (1,0), (5,0), (6,0), (0,6), (1,6), (5,6), (6,6)
```

TL/TR/BL/BR 的 role byte 分别是 `00 / 0F / 33 / 55`，LSB-first，0=black，1=white。任意两角色 Hamming distance 为 4。中央横线、竖线不受角色覆盖，仍为 `black:white:black:white:black = 1:1:3:1:1`，便于 bounded run-length 候选发现。

不能只凭全 49 个 module 的平均残差猜角色：4 个角色位的差异会被平均稀释。定位必须独立验证角色、四角相对关系和几何，含糊/多帧候选不能作为已接受单帧。

### 3.2 两份独立 RS Bootstrap

每份区域都是 **76 columns × 8 rows × 8×8 pixels/cell = 608 bits**，承载完整 76 字节 RS codeword。

```text
cellIndex = row * 76 + column
bit = (codeword[cellIndex / 8] >> (cellIndex % 8)) & 1
```

这是连续 bitstream 的 row-major 映射；一行 76 bits，不是“每列一个字节”，一部分字节会跨行。没有第二层跨区域 interleave。A 与 B 包含全部 SessionTag、FrameSequence、VisualProfileId、BootstrapCrc，且独立采样、独立 RS、独立 CRC/版本验证。

**禁止从 A 取部分符号、B 取另一些符号来拼一个 RS word；禁止把不同捕获帧或不同 sequence 的区域拼起来。** 两处都必须解码有效，并且 canonical 44 bytes 完全相同；任一失败或不一致均为整帧 erasure。不能只比较 Sequence 的低位、奇偶位或部分身份字段。

## 4. 固定 RS(76,44) reference

这是本次任务选定的自包含固定数组实现，不引入通用 FEC 依赖。与 PBInnerFec 的 Data Plane codec 分层独立。

| 参数 | 冻结值 |
|---|---|
| Field | GF(256) |
| Primitive polynomial | `0x11D` |
| Primitive element | α = 2 |
| Full code | RS(255,223)，32 parity symbols |
| Generator roots | α⁰ 至 α³¹ |
| Coefficient order | highest degree first |
| Shortening | 在 44 data bytes 前补 179 个已知零；编码后去掉这 179 字节 |
| Wire codeword | 原始 44 data bytes + 32 parity bytes |
| Decoder | errors-only，最多纠正 16 个 byte-symbol errors |

Generator 的 33 个系数（hex，highest degree first）：

```text
01744034ae367e10c2a221219db0c5e10c3b37fde4942fb3b9188afd148e37ac58
```

实现次序：32 syndromes → Berlekamp–Massey → **完整 255 positions 的 Chien scan** → 不允许 omitted prefix 中存在任何 error root → 固定最大 16×17 Vandermonde/Gauss–Jordan 求 magnitudes → 全部 32 syndromes 重新计算为零 → 最后提交 44 字节。

求 magnitudes 使用 `S_j = Σ e_i X_i^j`，避免 first-root=0 情况下 Forney 公式约定歧义。固定工作数组、只读 constexpr GF 表，无全局可变状态、热路径 heap 或可增长容器。输入和输出都必须精确长度；所有失败，包括 alias 情况，保留调用者输出。

必须检查缩短前缀：存在完整 RS(255,223) 合法、但 omitted prefix 非零的 word，其后 44 字节仍可能是 CRC 正确的 Bootstrap。只扫描 transmitted 76 positions、只校验 payload CRC 或允许修复省略区都不是合法 shortened decoder。

**纠错边界不是万能校验。** 超过 16 个错误可能被拒绝，也可能落到另一个合法 codeword 的 16-symbol 球内；不能承诺任意 17+ 错误必拒。后续 CRC、A/B 一致性、Timing、应用身份绑定仍必须执行。CRC 与无密钥 hash 不认证发送方。

## 5. 分布式 Timing 与 mixed-frame 基线

每个 Timing patch 有 16×16 个 8×8 binary cells。索引为前述位置的 row-major 0..8。精确 hash 输入：

```text
ASCII "PB-LDBS-X1-Pilot"（16 bytes，不包含 NUL）
|| canonicalBootstrap44
|| uint8(pilotIndex)
```

计算 BLAKE3-256，取前 16 bytes、每 byte LSB-first，得到 128 bits；每 bit `b` 映射为两个 cells `[b, b XOR 1]`，共 256 cells。Manchester pair 保持各 patch 黑白平衡，辅助 contrast/soft residual 判定。

Timing 与**全部 44 字节**绑定，而非低位 sequence toggle。它覆盖同奇偶 sequence、仅高位 sequence 改变、仅 SessionTag/ControlEpoch 改变的异帧。必须以同一帧独立一致的 A/B 结果生成期望 Timing，逐 patch 验证，不能从旧帧获得“更好”的 Bootstrap 替换当前结果。

基本混帧检测包括：

1. A/B 解码结果不一致：`BootstrapMismatch`，整帧 erasure。
2. A/B 相同但中间 Timing 来自另一帧：`TimingMismatch`，整帧 erasure。
3. 比如 50:50 blend 的二值 cell 落入中灰/产生过高 soft residual：`DoubleImage` 或质量门限 erasure。
4. 多个相互竞争的完整几何、不能唯一定位：`AmbiguousGeometry`，不能选一个“最像”的帧混合接受。

Bootstrap cell 与 timing cell 保留同一批五个 core samples（中心、左、右、上、下），不只保留它们的平均值。`midGrayFraction` 是原有 cell 均值统计；`sampleMidGrayFraction` 是平均前五点的统计。每份 A/B、每个 timing patch **分别**使用既有 `midGrayBoundary=.18`、`maximumMidGrayFraction=.06` 判定，两种统计任一种越限即 `DoubleImage`；不能跨副本或跨 patch 稀释。整体 telemetry 报各区域最大值。这个检查不增加像素读取或采样预算，也不替代原有 residual、FEC、CRC 和 timing 检查。

**可检测范围：** 有限采样点不可能保证发现完全避开 marker/Bootstrap/Timing 的任意小块替换；低于门限的微弱 blend 也不能承诺全部拒绝。X1 背景不携带 Data codeword，因此不把这些未覆盖像素称作已验证载荷。后续 Data Plane 必须有自身完整码字、CRC/摘要与帧身份约束。测试明确区分已覆盖 torn/mixed cases 与这个观测边界。

## 6. 缩放、归一化和事务边界

捕获层负责 physical ROI、显示 rotation、owned texture、ContentSize、timestamp、CaptureEpoch、adapter identity 和严格 cursor erasure。portable visual decoder 不再旋转 OS surface，也不隐式扩大 ROI。

本诊断定位支持 **axis-aligned、各轴独立 `[0.5,2.0]` scale、亚像素 phase**，不是 perspective 或任意画面旋转估计：

```text
captureEdgeX = originX + scaleX * logicalX
captureEdgeY = originY + scaleY * logicalY
```

8×8 logical cells 在最低尺度仍有 4×4 capture pixels。定位在实际 row-pitched pixels 上做 bounded marker 搜索、有限候选/几何 refine，然后按此变换直接采样 cell core；不把整幅图 resize 到 canonical canvas，不在应用端创建隐藏 full-luma image。

`LumaView` 可表达 Gray8、BGRA8、R10G10B10A2、FP16 linear SDR。实际捕获信号必须先由 normalize/app 元数据判断：HDR 或 Unknown signal 本轮是 `UnsupportedSignal` erasure，**不定义或偷偷进行 HDR tone-map**。非 HDR 的 FP16 保持 linear SDR 语义；采样的 NaN、Inf、非法范围必须 fail closed，不能先量化成 BGRA8 掩盖异常。

work/candidate/contrast/residual 是 receiver policy，不是新增 wire 字段；具体默认值集中在 `LocalDesktopDecodePolicy`。每次 decode 的返回值都含 erasure、work/candidate、geometry/quality、A/B 纠错/CRC 状态，不能只有一个不可解释的 success bit。

Marker 搜索使用有限的 `128 / 64 / 192` 三个 luma midpoint proposals，横纵 cross-check 使用同一个 proposal。三次扫描共享最多 64 个 marker、32 个 geometry 和 24M work 的总预算；不是每个 proposal 各有一份预算。即使早先 proposal 找齐四角，也继续有界扫描，以拒绝另一 luma 区间中同时有效的第二个位置。最终 contrast96、marker/Bootstrap/timing residual 等验收门槛不随 proposal 放宽。独立 fixtures 固定 black/white=0/96 与159/255恰好通过、contrast95拒绝，以及不同 luma 区间的两个完整帧必须报歧义。

应用的 CPU processor 由 readback worker 独占调用：

- `Analyze` 仅暂存候选，不更新已提交 geometry/calibration、seen sequence、accepted counters。
- worker 核对 domain/revision、observation 顺序和 freshness 后才 `Commit`。
- `Discard` 清候选，不能把旧帧的 sequence/几何带入状态。
- `Reset(nullopt)` 即使无下一帧也执行；新 CaptureEpoch/domain 清空 temporal state、旧队列以及重复 sequence 状态。
- visual erasure 是一次成功分析得到的诊断候选，可提交原因 telemetry；不是把它误报成 GPU/readback fatal error。
- duplicate FrameSequence 检测必须发生在真实 Commit 路径，不能用纯 stateless decoder 或未提交的 Analyze 候选推进去重。

重复捕获帧不是新的独立观测，不能当成额外 FEC equation；CaptureEpoch 改变之后禁止与旧帧 LLR combine。本实验不提供跨帧 LLR combine。

应用实际准入保留 64 条完整 Bootstrap identity、8 个 Session 与16个有界诊断事件；history 中同tag/sequence不同完整44B形成 conflict tombstone，已淘汰的sequence由每Session high-water拒绝为stale。geometry（包括phase/scale）或black/white校准改变时分别更新generation；同identity的新像素观测只分类为duplicate，不重复累积置信度。事件队列可以丢旧日志，不能决定一次诊断是否曾成功；CLI成功码来自已Commit的累计accepted计数。

## 7. 独立 Golden 与测试

新 Golden 位于 `tests/golden/local-desktop-bootstrap/`，不覆盖旧目录。生产代码不会生成 expected bytes。

`tests/PBModulation/generate_local_desktop_golden.py` 是独立 Python oracle：

- GF 使用 bitwise polynomial multiply，不使用生产 log/exp 表；
- generator 用 ascending coefficient multiplication 后反转；
- parity 用 LFSR，生产用 polynomial long division；
- syndromes 用显式 powers，而非生产 Horner evaluator；
- CRC 使用独立 bitwise CRC-32C；
- BLAKE3 使用 Python `blake3` 1.0.9 基线，并先验证 empty-message known answer；
- 画布使用 literal region painter，不读取 C++ header、不运行生产 Encode；
- pin 所有 small fixture 的 byte count/BLAKE3，以及完整 raw BGRA/PBRW 的 hash；不依赖 PNG 编码器版本。

fixture 分组：

| Stem / 文件 | 含义 |
|---|---|
| `a` | 基准 SessionTag 与 sequence |
| `b` | 同 Session、sequence +1 |
| `c` | 同 sequence、SessionTag +1 |
| `d` | 同 Session、sequence +2，保持奇偶 |
| `e` | 同 Session、sequence XOR `(1<<48)` |
| `f` | Session/sequence 不变，只 ControlEpoch +1 |
| `badcrc` | a 的 byte40 XOR 1，不修 CRC，但独立重新 RS 编码 |
| `unsupported` | layout3、独立重算 CRC 与 RS |
| `protocol-rs76.bin` | 原协议示例的 opaque RS word，不代表此 visual profile |
| `shortening-prefix-first/last.bin` | 完整 RS word 的省略区 position0/178 非零，payload CRC 仍合法 |
| `uncorrectable-17.bin` | 17 个 parity errors，前16 syndromes全零，不能存在 ≤16-error 解 |
| `rootless-locator.bin` | 保留合法44B/CRC，仅改变32个parity；最小locator degree2但全GF无根，精确拒绝为LocatorRootCount |

独立 C++ test painter `tests/PBModulation/local_desktop_test_fixtures.h` 只读取这些小文件，再按第二份 literal region map 构造捕获测试输入；不调用生产 RS/Marker/Timing/Encode。

基准 a 的 raw BGRA BLAKE3：

```text
248b075d46be224f982633732960cd13e80be8eea4f7329554179216d5ef293f
```

### 7.1 可重放命令

在仓库根目录，当前环境的独立 oracle Python 路径为 `D:\Python3.12.9\python.exe`：

```powershell
& 'D:\Python3.12.9\python.exe' tests/PBModulation/generate_local_desktop_golden.py --check

# 可选：独立生成可检查的原始 PBRW，不通过生产 renderer。
# 输出目录中的同名文件已存在时会拒绝覆盖。
& 'D:\Python3.12.9\python.exe' tests/PBModulation/generate_local_desktop_golden.py --check `
    --raster-output build-wgc-release/independent-local-desktop-pbrw

cmake --build build-wgc-release --config Release
ctest --test-dir build-wgc-release --build-config Release `
    -R 'PBLocalDesktopBootstrapTests|PBModulationTests' --output-on-failure
& './build-wgc-release/tests/PBModulation/Release/PBLocalDesktopBootstrapTests.exe' '[local-desktop]'
```

默认生成是 create-only；`--check` 只读比较。维护时的 `--extend` 只允许新增 fixture/manifest entry，必须验证既有文件、既有 frame entry 和所有既有 digest pin 完全不变；不是通用 re-pin 开关。若生产结果与 oracle 不同，先调查协议/布局/实现差异，不能运行生产结果覆盖 Golden。

### 7.2 验证矩阵

`test_bootstrap_rs.cpp`：65536 GF 乘法组合；每个76位置×每种255非零单错；0..16个随机独立 symbol errors；全零 opaque input；shortening两端根与额外transmitted错误；可证明不可纠17错；长度/别名/失败输出不变；随机恶意 received word 只有在独立重编码距离≤16时才能提交；四线程独立 scratch。

root-count 拒绝样本使用独立构造而非试探生产 decoder：取 `δ=0x20`，验证 `Tr(δ)=1`；`Λ(x)=1+x+δx²` 在全部256个域元素上无根。固定 `S0=1,S1=0`，按 `S[j]=S[j-1] XOR δ*S[j-2]` 生成32个syndrome，`S2≠0` 排除degree0/1。独立解32×32 parity Vandermonde系统，原44B与CRC不变。C++测试复核全部syndrome、degree证明与256点无根，精确检查 `LocatorRootCount/errorPosition=0/correctedSymbols=0`，以及独立输出和4种别名布局整128B不变。该76B固定样本BLAKE3为 `2e60801855d17a127f8362e85cdb93486e4f5ad3d7d97e4d4011565b969819bf`；追加后独立 `--check` 共32文件通过，既有fixture字节、digest pin和原frame entry均保持不变，manifest仅追加新file entry。

`test_local_desktop_bootstrap.cpp`：三幅独立 raw/PBRW hashes 与整幅逐 byte 比较；完整44 A/B重复；ID/layout/geometry冻结；marker roles/Hamming/cross；所有 Timing oracle/Manchester；CRC/版本/flags/长度非法；完整 redraw/输入重叠/输出 guard；uint64 sequence/session 与 uint32 ControlEpoch 最大值。

定位/应用/真实捕获 Gate 还分别覆盖 anisotropic scale、subpixel phase、padded pitch、多格式、边界/资源上限、A/B冲突、不同方向 torn、同奇偶/高位sequence、中部Timing替换、blend、duplicate/Commit/Discard/epoch清理以及 WGC/DXGI 的真实像素入口。测试是否通过以当前构建日志为准；环境 SDR、窗口可见、抓屏 API 权限和桌面会影响 native Gate，不能把前置条件不满足改成 skip 或伪造像素通过。

`test_local_desktop_matrix.cpp` 的独立矩阵包含各轴 `{0.5,0.75,1,1.5,2}`、极端轴组合的 `{0,.25,.5,.75}²` phase、全部14个非平凡四象限 cut、中央 timing 的四个已观测局部替换、四个位移×25/50/75%双影，并覆盖 Area/Bilinear 与两种 mix/scale 顺序。合计298个矩阵观测及18个组成图的独立正向检查。所有 erasure 路径断言有效顶层44B输出全零、quality为0。

`test_local_desktop_core_samples.cpp` 独立改变每个 cell 的两个物理字节，只影响五点中的右点，锁定每patch76/1280通过、77/1280拒绝；每copy182/3040通过、183/3040拒绝，以及原始变化69/70跨过.18分类边界。新增拒绝不是依靠提高对比度要求、降低原residual阈值或损坏正向fixture。独立 `PBLocalDesktopNoAllocationProbe` 还覆盖首次RS调用、失败不可变/别名、实际定位和Luma路径的C++分配禁用检查。

## 8. 未实现/不声明的能力

- Certified desktop payload modulation、Control/Data raster、文件恢复和吞吐认证不属于 X1。
- 没有通用 CV 依赖、perspective/任意角度旋转恢复、任意小 scale、HDR tone-map。
- CPU readback 是显式、有界诊断路径，不声称 production GPU compact-luma/LLR fast path 性能。
- 有限 Timing/marker 覆盖不等于逐像素、任意 adversarial mixed-frame 检测。
- Reed–Solomon、CRC、BLAKE3 一致性不提供发送方认证；真实会话身份与最终文件摘要规则仍由既有协议/应用负责。
