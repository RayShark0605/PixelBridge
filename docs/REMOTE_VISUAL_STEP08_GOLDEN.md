# PixelBridge RemoteVisual Step 08 LF4 Golden / Manifest Freeze

状态：**DONE。`PB-RemoteVisual-LF4-X1` 的固定 Bootstrap、物理 mapping、Walsh codebook、四平面 interleave、8100 字节 coded data、Step 07 selected metric calibration/量化契约、canonical BGRA raster identity 和 4 个 production-accepted Transport block 已冻结为独立可重建 Golden。该结论是实验 Golden 的 bit-exact compatibility gate，不是 production GPU deployment、Certified Profile 或双机文件传输 field certification。**

## 1. 本步骤关闭的边界

Step 08 只回答一个兼容性问题：当 LF4 的 profile/layout、物理 tile manifest、freshness、interleave、codebook、Bootstrap binding、Step 07 metric candidate、固定 int16 adapter 或 Transport 结果发生漂移时，能否在进入 D3D11 production 实现之前稳定地检测并非零失败。

本步骤没有改变以下权威边界：

- `VisualProfileId=0x504252564C463431`、`LayoutVersion=7`；
- `PB-Bootstrap-1`、Robust DVB-S2 Short QC-LDPC、Transport CRC/identity、Outer FEC、WholeFileDigest 与最终发布语义；
- production LF4 decoder 的 default admission policy；
- Step 07 selected model 仍是 `FrozenCandidateNotProductionDefault`，没有在本步骤静默接管 live decoder；
- Golden 只证明固定实验输入的 bit-exact 结果，不证明真实 provider matrix、GPU parity、goodput、soak 或最终文件发布。

## 2. 独立 oracle 与固定输入

新增 `tests/PBModulation/generate_remote_visual_lf4_golden.py`。它：

- 不读取 C++ header，不链接或调用任何 PixelBridge 可执行文件；
- 只复用既有独立 Python Bootstrap RS/CRC scaffold，以及旧 DesktopLevels Golden 已经独立抄录并验证的 Robust QC-LDPC Python oracle；
- 用独立 literal 实现重建七段 tile geometry、98 个 freshness region、eligibility、21456 项 role/data-ordinal mapping、SplitMix freshness bit、四平面 affine inverse mapping、16 个 Walsh mask 和 BGRA 绘制；
- 生成模式拒绝已存在 output directory，所有文件用 exclusive create；
- `--check` 严格只读，要求目录 inventory 和每个文件字节完全一致；
- `--check` 与 `--raster-output` 不能组合，避免把检查命令伪装成只读后仍写文件；
- `.gitattributes` 将 LF4 `.bin` 固定为 binary、manifest/BLAKE3 pin 固定为 LF，避免 Windows `core.autocrlf` 在 clean checkout 时改变 Golden 字节；
- 大 raster 不进入 Git，但可用同一固定 seed/input 重建为 PBRW。

固定输入为：

| 字段 | 值 |
| --- | --- |
| Profile / Layout | `0x504252564C463431` / `7` |
| SessionTag | `0x5354455030325244` |
| FrameSequence | `17` |
| ControlEpoch | `0` |
| diagnostic domain | `PB-RemoteVisual-LF4-X1-Data` |
| canvas / pixel format | `1920×1080` / `BGRA8` |
| raw BGRA bytes | `8294400` |
| canonical raw BGRA BLAKE3 | `28b09b66520b87f9cd9407b516530ad1225f84d390cc2b94d18d6e1b8d5e9bc4` |

该 raster identity 与 Step 02 Windows Remote Desktop evidence Presenter 封存值相同，因此 Step 08 没有重新选择一个更容易通过的新输入。

## 3. Golden inventory

提交目录为 `tests/golden/remote-visual/lf4/`。旧 `tests/golden/remote-visual/manifest.json` 没有修改；新 LF4 Golden 使用独立子目录和 `PixelBridge.RemoteVisualLowFpsGolden.1` schema。

| artifact | bytes | 内容 |
| --- | ---: | --- |
| `manifest.json` | 5942 | 完整 profile/raster/mapping/codebook/FEC/metric/Transport/truth-boundary manifest |
| `lf4-bootstrap.bin` | 44 | canonical PB-Bootstrap-1 record |
| `lf4-coded-data.bin` | 8100 | 4×2025-byte Robust coded data，无 padding |
| `lf4-codebook.bin` | 32 | 16×LE16 Walsh mask |
| `lf4-mapping.bin` | 429140 | header + 21456×20-byte physical mapping record |
| `lf4-metric-calibration.bin` | 532 | header + 16×32-byte selected calibration bin |
| `lf4-metric-probes.bin` | 1796 | header + 111×16-byte float/adapter boundary record |
| `lf4-raster.blake3` | 65 | raw BGRA raster BLAKE3 文本 pin |
| `lf4-accepted-transport-{0..3}.bin` | 4×1350 | production QC-LDPC/CRC/identity admission 后的 exact Transport bytes |

总提交体积为 451051 bytes；8294400-byte raw BGRA / 8294428-byte PBRW 没有进入 Git。manifest 自身 SHA-256 为 `fc43bb7849f6d00c063372b459338b1b88a324d5bda647b74d4329ac8c13cf79`。

### 3.1 Mapping record

`lf4-mapping.bin` 使用 `PBLF4M01`、version 1、20-byte LE record。每项显式记录：

- physical index；
- `x/y/width/height`；
- role：Unused/FreshnessTag/Data；
- freshness region ID；
- data ordinal，非 Data 固定为 sentinel `16723`；
- 两处 reserved 字段，必须为 0。

C++ gate 比较全部 21456 项，而不是只比较汇总计数。汇总必须为 2344 Unused、2389 FreshnessTag、16723 Data、77 eligible freshness regions。

### 3.2 Codebook 与 raster

`lf4-codebook.bin` 固定 16 个 16-bit mask。每个 mask 有 8 个 one chip，互补 symbol 相差 16 bit，任意两个不同 symbol 的最小 Hamming distance 为 8。C++ production encoder 用 Golden Bootstrap/coded data 重新绘制完整 BGRA，BLAKE3 必须等于独立 oracle pin；因此 codebook、四平面 offset、affine inverse、freshness、mapping、luma 或绘制顺序任一漂移都会使 gate 失败。

## 4. Step 07 metric calibration freeze

`lf4-metric-calibration.bin` 冻结 `lf4-default/PiecewiseLookup` 的：

- 16 个 `rawMagnitudeUpper` 的 binary64 identity；
- 每个 `calibratedMagnitude` 转换后的 exact IEEE-754 binary32 bits；
- Train sample/error 计数；
- terminal input magnitude `1000000`；
- Step 07 report SHA-256 `c17f17844c0b44cf029bb31ecbfc127139fa886588c941c2a3a0f34bf541acf8` 与 calibration core BLAKE3 `7a8ff14e09dc5f0b4417349f9ab3053f0e0dac05312be94806e55d40324f619f` provenance。

新增 `CalibrateRemoteVisualLowFpsMetric` 作为冻结候选的 reference mapping：

- 非有限值或 `abs(raw)>1000000` fail closed，且不修改 output；
- `+0/-0` exact 保留；
- 选择第一个满足 `abs(raw)<=rawMagnitudeUpper` 的 bin；
- calibrated magnitude 由固定 binary32 bits 构造，再复制原 sign；
- 本 API 没有被 live decoder 自动调用，因此没有把实验候选伪装成 production default。

`lf4-metric-probes.bin` 覆盖每个 bin boundary 的相邻 binary32、正负号、代表值、`+0/-0`、正负最小 subnormal、terminal 邻值、`±Inf` 和 qNaN。每条记录同时固定：

- mapping 是否有效；
- calibrated binary32 bits；
- `scale=4096`、clamp `[-32767,32767]`、`std::round` half-away-from-zero 后的 int16；
- `calibratedMetric <= float32(-0.5/4096)` hard-one 判定。

## 5. Production truth gate

`test_remote_visual_low_fps_golden.cpp` 没有复制 production decoder。它把独立文件与当前运行代码逐层交叉：

1. production Bootstrap serialization 必须等于 `lf4-bootstrap.bin`；
2. production constants、`GetRemoteVisualTile` 与 `GetRemoteVisualTileMapping` 必须逐项等于 codebook/mapping Golden；
3. production diagnostic Transport/QC-LDPC generation 必须逐字节等于 8100-byte coded data；
4. production LF4 encoder 的完整 raster BLAKE3 必须等于独立 pin；
5. raster 只通过 pixels 进入 production CPU decoder，hard output 必须等于 Golden coded data；
6. raw metrics 通过冻结 candidate 与既有 `AdaptSoftMetrics`，exact raster 的全部 int16 必须为 `±32767`；
7. calibrated metrics 进入既有 QC-LDPC、padding、Transport CRC 与 identity authority；
8. 4/4 codeword 必须 verified，accepted slot/byteCount/bytes 必须逐字节等于四份 Transport Golden。

sender expected bytes 不进入 raster demod 或 FEC；只在 production admission 后做 exact byte comparison。本 gate 不创建 Outer/Receiver file session，因此不能声称 WholeFileDigest 或 final publish 已完成。

## 6. 重建与检查

只读检查提交的 Golden：

```powershell
D:\Python3.12.9\python.exe -B `
  tests\PBModulation\generate_remote_visual_lf4_golden.py `
  --check
```

从两个不存在的目录独立重建，并可选保存大 raster：

```powershell
D:\Python3.12.9\python.exe -B `
  tests\PBModulation\generate_remote_visual_lf4_golden.py `
  --output-dir <new-fixture-a> `
  --raster-output <new-raster-a.pbrw>

D:\Python3.12.9\python.exe -B `
  tests\PBModulation\generate_remote_visual_lf4_golden.py `
  --output-dir <new-fixture-b> `
  --raster-output <new-raster-b.pbrw>
```

最终 seal 使用：

```text
build-p1_5-evidence/20260901-step08-lf4-golden-final-d
build-p1_5-evidence/20260901-step08-lf4-golden-final-e
build-p1_5-evidence/20260901-step08-lf4-raster-final-d.pbrw
build-p1_5-evidence/20260901-step08-lf4-raster-final-e.pbrw
```

两套 12-file fixture 对应路径逐字节一致；两份 8294428-byte PBRW 逐字节一致，SHA-256=`8bd270750b30f8705d772d14806aa88201fde80e6321b3074b761711eaf0052f`。对 final-d 再执行 `--check` 通过。

## 7. 验证结果

| 验证 | 结果 |
| --- | --- |
| 独立 LF4 generator 单元测试 | 4/4 OK；clean generation、read-only check、create-only refusal、missing/extra inventory、profile/codebook/mapping 三类 drift fail closed |
| 旧 DesktopLevels Golden oracle | `DESKTOP_LEVELS_GOLDEN_PASS files=100 frames=32`；旧 32 帧未改变 |
| Release LF4 C++ 定向 gate | `PBRemoteVisualTests` 1/1 PASS |
| Qt 5.14.2 Release LF4 C++ 定向 gate | 1/1 PASS |
| Qt 6.10.1 Release LF4 C++ 定向 gate | 1/1 PASS |
| MSVC ASan/RelWithDebInfo LF4 C++ 定向 gate | 1/1 PASS |
| Release `ALL_BUILD` | PASS |
| Release 无界面全量 CTest | 155/155 PASS，246.43 s |
| MSVC ASan/RelWithDebInfo `ALL_BUILD` | PASS |
| MSVC ASan 无界面全量 CTest | 290/290 PASS，442.75 s |
| PBModulation cppcheck 2.21.0 | 10/10 translation units；34 项均精确匹配既有 review ledger，0 new/unreviewed finding |
| `git diff --check` | PASS |

全量 CTest 使用 `-E "Native|GuiSmoke"`，没有启动窗口、读取当前屏幕或把 WARP/CPU 结果冒充硬件 field evidence。Qt5/Qt6 一致性是同一 C++ Golden gate 在两套 Qt build tree 中通过，不代表执行了 GUI 实机验收。

## 8. 后续边界

Step 08 关闭后，下一步是 Step 09：实现 LF4 D3D11 Encoder raster 与 immutable PB-owned texture，要求 canonical CPU raster 和 GPU-present source diagnostic readback 对齐，并保持 logical update 1..5 Hz、重复 Present 不推进 FrameSequence。Step 10 才实现 scaled Walsh demod shader；Step 11 才比较 WARP 与可用 hardware adapter 的 production accepted Transport 集合。

尚未完成：

- Step 07 candidate 接入 production decoder/GPU path；
- LF4 D3D11 raster、immutable replacement、GPU demod 与 adapter parity；
- continuous capture 的 epoch/lifetime/queue 完整闭环；
- field provider matrix、`UniqueVisualFPS`、VerifiedEncodedGoodput、6 小时 soak；
- 真实文件 Outer convergence、WholeFileDigest 与 final publish；
- Certified RemoteVisual Profile。
