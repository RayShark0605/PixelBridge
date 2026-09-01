# PixelBridge RemoteVisual 低刷新率高密度传输技术路线

状态：**2026-09-01 持续实施路线；Step 01..07 已全部完成。Step 02 已由 Windows Remote Desktop 上 Direct/Shape/LF4 各一组真实 receiver-only Replay 关闭；Step 07 已选择 `lf4-default/PiecewiseLookup` 作为仅供 Step 08 冻结的 soft-metric candidate，未修改 production default 或 admission。Step 06 的 production-truth hardening implementation commit 为 `1936c020cb2c017b9ce3064267f497d15f94d66d`。这不是 Certified Profile，也不是 Step 20 双机文件传输 field certification。**

本文件回答一个限定明确的问题：电脑 B 的编码窗口经过任意品牌、任意实现策略的远程操控/桌面视频链路，到达电脑 A 后，在**完整逻辑画面更新率不得超过 5 Hz**的约束下，如何尽可能提高可靠净载荷，同时保留 PixelBridge 已有的协议、FEC、文件完整性和发布语义。

---

## 1. 结论先行

采用两级路线，而不是把“Unicode 字符 + 颜色 + 任意角度”直接做成生产协议：

1. **现在实施的安全基线：`PB-RemoteVisual-LF4-X1`**
   - 独立实验 `VisualProfileId = 0x504252564C463431`，`LayoutVersion = 7`；
   - 逻辑画布仍为 1920×1080，复用现有四角角色标记、双份 Bootstrap、九个 timing patch、四组亮度 ladder、128×128 freshness region 和既有 data tile manifest；
   - 每个 8×8 data tile 不再是一块纯黑/纯白，而是一个固定 4×4 二值 Walsh 模板，每个逻辑 chip 放大为 2×2 像素；
   - 16 个模板表示 4 bit；每个模板恰好 8 个高亮 chip、8 个低亮 chip，平均亮度完全相同；任意两个模板的 Hamming distance 至少为 8；
   - 只使用中性亮度，不使用字体、Unicode、色度或连续角度；
   - 每帧承载 4 个未修改的 Robust QC-LDPC codeword，共 8100 coded bytes，恰好无 padding；
   - 解码以 locator 给出的连续 `originX/originY/scaleX/scaleY` 直接采样逻辑 chip，不把整张图先 resize 到 1920×1080；
   - stale/partial-update region 的四个 bit plane 同时置为 **zero soft metric**，交给现有 QC-LDPC 恢复，绝不把陈旧像素当成高置信 hard bit；
   - RemoteVisual 运行时逻辑刷新率现在强制为 **1..5 Hz**；`0`（presentation-driven）和 `>5` 均 fail closed。

2. **Gate 通过后的密度升级**
   - 先测 LF4 的真实 block survival、尺度残差、soft metric 分布和最终文件 goodput；
   - 只有 field replay 证明色度或更密 shape 在特定链路可稳定保留时，才另建 `LF5/LF6` 新 profile；不能在同一 Session 中改变 codebook；
   - GPU production path 必须直接在 PB-owned D3D11 texture 上按连续几何采样，不允许为了赶进度加入隐式 GPU→CPU→GPU fast-path round trip。

当前实现有意保持为 **CPU reference / simulator Gate 路径**，尚未加入 Encoder GUI profile 列表，也未接入当前严格 1920×1080 的 D3D11 production demod。这样可以先把物理符号、缩放、freshness、FEC 和 Transport 真值链证明清楚，再决定 GPU 接口；不会平行创造第二套协议或错误宣称生产可用。当前 CPU/reference 实现提交为 `9b8e8063a56251040cddb6b01e38c4343779571f`，对应 tree `0da0ea8a9e8a1260b3972098a3efdf87b0c096de`。

---

## 2. 用户截图的可复现证据

只读分析工具：

```powershell
D:\Python3.12.9\python.exe `
  tools\PBRemoteVisualEvidence\analyze_remote_capture.py `
  <existing-image-path>
```

本次输出保存在：

```text
build-p1_5-evidence/remote-low-fps/image-1-analysis.json
```

关键结果：

| 项目 | 结果 | 解释边界 |
| --- | ---: | --- |
| 输入 SHA-256 | `C7D373E2FA3AE2368DBD75B0FF8E7C002DB143B22E81EF37E289A904008CBCBD` | 固定本轮证据身份 |
| 截图尺寸 | 2560×1392 | 输入文件本身 |
| 候选数据画布 | `[71,32]..[2488,1391]`，2418×1360 | 由全局边缘梯度得到的诊断候选，不是协议 acceptance |
| 相对 1920×1080 尺度 | X=`1.259375`，Y=`1.259259259...` | 证明当前 strict 1:1 production policy 不适配该截图 |
| 画布 RGB 通道差均值 / P99 / 最大值 | `0.3855 / 3 / 4` code values | 该样本几乎为灰度，不能把色度位当作首版可靠容量 |

肉眼与数值共同显示：画面存在大块空间局部陈旧/拼接、边界污染和灰度化。最危险的不是独立像素噪声，而是**同一帧中不同 128×128 左右区域来自不同逻辑 FrameSequence**。因此首要原语必须是区域 freshness erasure，而不是增加更复杂的彩色字形后继续把所有采样都当成可信 hard decision。

该脚本不会打开窗口、不会捕获桌面、不会读取左侧屏幕，只分析用户已经提供的文件。

---

## 3. 资料研究与可迁移结论

### 3.1 远程桌面链路不是透明像素管道

Microsoft 的 Remote Desktop 图形编码文档说明，其实现会按内容/更新模式选择图形编码路径；频繁更新和视频区域可进入 AVC/H.264，默认视频色度通常为 4:2:0，而 4:4:4 需要更多带宽。MS-RDPEGFX 同样列出 progressive、H.264 4:2:0/4:4:4 等协商模式。这些资料只作为“远程桌面产品可能具有内容分类、块更新、视频编码、色度抽样”的代表性证据，**绝不假定用户使用 Microsoft RDP，更不假定任何特定品牌**。

- Microsoft：<https://learn.microsoft.com/en-us/azure/virtual-desktop/graphics-encoding>
- MS-RDPEGFX：<https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegfx/31c6e2b1-335b-4a75-9454-bb2309958c21>

工程结论：物理层必须对 block update、resize、blur、4:2:0、重复帧和局部 temporal mix 建模；“在 B 上画对了”不等于“在 A 上收到的是同一帧”。

### 3.2 QR / AprilTag 提供的是定位与几何思想，不是容量答案

QR 的 finder pattern、quiet zone、模块化采样和纠错证明了离散平面码的自定位可行；DENSO 资料也说明高版本 QR 的二进制容量仍只有数 KiB，并建议打印场景每 module 至少约 4 dots。AprilTag 进一步给出 line/quad、homography、角色明确的 fiducial 和 subpixel corner refinement。

- DENSO QR 基础与 finder：<https://www.qrcode.com/en/about/index.html>
- DENSO QR 版本/容量：<https://www.qrcode.com/en/about/version.html/index.html>
- DENSO module 尺寸：<https://www.qrcode.com/en/howto/cell.html/index.html>
- AprilTag 论文：<https://april.eecs.umich.edu/pdfs/olson2011a.pdf>

工程结论：PixelBridge 应复用自己已经实现的四角角色 marker、双 Bootstrap 和 timing corridor，并输出连续尺度模型；不应每帧嵌一张普通 QR 来替换现有协议，因为那会显著降低容量并制造第二套 framing/FEC。

### 3.3 彩色码与 glyph codebook 可增容，但必须服从实测信道

JAB Code、Microsoft HCCB 和 libcimbar 都证明“固定图形模板 + 颜色 + interleave + FEC/fountain”可以提高二维码容量。libcimbar 的公开实现尤其接近本项目：tile 同时承载 shape 和 color，配合 interleave、Reed-Solomon 和 Wirehair。

- JAB Code（ISO/IEC 23634:2022 实现）：<https://github.com/jabcode/jabcode>
- Microsoft HCCB：<https://www.microsoft.com/en-us/research/project/high-capacity-color-barcodes-hccb/>
- libcimbar：<https://github.com/sz3/libcimbar>
- libcimbar 设计细节：<https://github.com/sz3/libcimbar/blob/master/DETAILS.md>

工程结论：用户提出的“字符/颜色/角度”方向在抽象上正确，但可移植实现应是**版本固定的 bitmap codebook**，不是字体字符。颜色必须是另一个经过 field calibration 的 profile，而不是 LF4 的硬依赖。

### 3.4 低空间频率与分布式局部可靠性优于精细字形

PixNet 使用空间频率/OFDM 思路抵抗投影/拍摄链路的模糊和几何变化；RDCode 使用分布式 locator/palette 与局部独立 block，应对局部可见性和信道变化。两者共同支持：首版符号应采用低空间频率、等均值模板；定位和可靠性标签要分布在画布中，不能把全部同步信息压在单一角落。

- PixNet：<https://people.csail.mit.edu/nabeel/pixnet-mobicom10.pdf>
- RDCode：<https://mashuai-ms.github.io/pubs/mobicom2014.pdf>

---

## 4. 为什么不直接使用汉字、数字和多语言字符

### 4.1 “字符数”不是可靠符号数

Unicode code point 数量很大，但远控链路真正要区分的是渲染后的有限像素模板。以下变量会让同一字符不再具有 canonical raster：

- 字体是否安装、font fallback、字体版本；
- hinting、ClearType/subpixel AA、灰度 AA；
- locale、复杂文本 shaping、combining mark、双向文本；
- Qt/DirectWrite/浏览器的布局与缩放策略；
- 视频编码器对细笔画、锐边和 chroma 的不同处理。

如果把字体在发布前 rasterize 并冻结，最终得到的仍然只是 bitmap codebook。因此直接从 bitmap 开始更可测试、更跨机器、更容易建立最小距离和 Golden Vector。

### 4.2 角度不应是连续估计变量

把一个复杂 glyph 旋转 10°、20°、30°会引入插值、边界裁切和整体几何旋转混淆。LF4 把“方向差异”固化在 16 个离散模板中；整体画布方向由四个角色不同的 marker 决定。未来若增加 orientation 状态，也必须是 codebook 的离散成员，不能是无限精度角度。

### 4.3 色度是可选增益，不是基线

用户样本画布的 RGB 通道最大差仅 4/255；代表性远程桌面资料也明确存在 4:2:0 路径。首版若把 2~3 bit 放在颜色上，整个 plane 可能被系统性抹除，不是 QC-LDPC 擅长的少量随机误码。因此 LF4 采用中性灰；未来 LF-C profile 只有在 replay 证明各色 centroid、空间一致性和误判率后才成立。

---

## 5. LF4 符号与容量的精确定义

### 5.1 物理符号

- data tile：8×8 logical pixels；
- 内部：4×4 binary chips；每 chip 为 2×2 pixels；
- 亮度：low=`32`，high=`224`，BGRA 三通道相同且 alpha=`255`；
- 16 个 `uint16_t` mask：

```text
3333 00FF CC33 9999 F00F 6699 3CC3 9669
CCCC FF00 33CC 6666 0FF0 9966 C33C 6996
```

位序是 4×4 row-major、LSB-first。每个 mask `popcount=8`；任意两 mask 的 Hamming distance 为 8 或 16；后八个是前八个的 complement。因此：

- 所有符号平均亮度相同，DC/背景估计不会泄露 symbol；
- 最近模板至少需要 4 个 chip 越过决策边界才可能混淆；
- 模糊/resize 优先削弱 metric 幅度，而不是系统性把高平均亮度 symbol 偏向某些 label。

### 5.2 bit plane 与 interleave

每 tile 的 label 提供四个 bit。每个 plane 独立使用现有 `kRemoteVisualPermutation`，但 FrameSequence phase 分别加 `{0,4,8,12}`：

```text
logicalBit = plane * 16200
           + permutation.ToLogical(dataOrdinal, FrameSequence + phaseOffset[plane])
```

物理区域损坏因此不会在一个 codeword 中形成四倍连续 burst；它在四个 codeword 中各形成一个空间散布的 soft erasure 集合。

### 5.3 单帧与 1/2/5 Hz 容量

| 项目 | 当前 RemoteVisual 1-bit | LF4 |
| --- | ---: | ---: |
| bit/tile | 1 | 4 |
| coded bits/frame | 16,200 | 64,800 |
| Robust codewords/frame | 1 | 4 |
| coded bytes/frame | 2,025 | 8,100 |
| Transport payload ceiling/frame | 1,314 B | 5,256 B |
| padding | 0 | 0 |

LF4 的**理论 Transport payload ceiling**：

| 逻辑更新率 | dwell | payload ceiling | bit rate |
| ---: | ---: | ---: | ---: |
| 1 Hz | 1000 ms | 5,256 B/s（5.13 KiB/s） | 42.048 kbit/s |
| 2 Hz | 500 ms | 10,512 B/s（10.27 KiB/s） | 84.096 kbit/s |
| 5 Hz | 200 ms | 26,280 B/s（25.66 KiB/s） | 210.240 kbit/s |

这些数字不是最终文件 goodput。Control repetition、Carousel、Outer FEC repair、重复/丢帧、stale region、Receiver 去重和全文件 digest 都会降低 `VerifiedEncodedGoodput`。报告必须同时给出 ceiling、accepted Transport、unique Outer admission 和最终 digest 后 goodput，禁止把 raster capacity 当成文件传输成绩。

---

## 6. 自定位与任意适度缩放

### 6.1 复用现有 locator

现有 scaffold 已包含：

- 四角 role-distinct finder；
- 两份相距较远的 Bootstrap A/B；
- 九个分布式 timing patch；
- bounded marker candidate、geometry candidate 和 refinement budget；
- 连续几何：

```text
physicalX = originX + scaleX * logicalX
physicalY = originY + scaleY * logicalY
```

LF4 不建立第二套 locator。CPU reference 在 finder/Bootstrap/timing 全部通过后，直接使用连续几何对 ladder 和每个 chip 取样。

### 6.2 当前支持边界

- 轴对齐；`scaleX`、`scaleY` 可独立；
- CPU 实验 policy：每轴 `0.5..2.0`；
- 允许 fractional origin 与 fractional scale；
- 不做整帧 canonical resize；
- 不接受 perspective、旋转、画布被裁掉或落在 view 外；这些情况 fail closed；
- rotation 若以后必须支持，应升级几何模型和新 layout，而不是在现有 layout 中静默猜测。

已通过的独立 test-only resampler 矩阵包括：1.0、0.5 area、0.75×1.5 anisotropic area、用户证据的 1.259375×1.259259 area、2.0 bilinear。测试 resampler 不调用 production sampler/locator，避免同源算法自证。

### 6.3 采样与 max-log metric

每个 2×2 logical chip 取中心和四个轴向偏移样本；先用四个亮度 ladder 估计 low/high centroid，再执行：

1. `normalized = (sample - midpoint) / halfSeparation`；
2. 对一个 tile 的 16 个值减去 tile mean，消除局部 DC；
3. 计算 RMS；
4. 对 16 个模板计算 normalized mean-square residual；
5. 每个 label bit 输出 `0.5 * (minDistance(bit=1) - minDistance(bit=0))`；正数偏向 0，负数偏向 1；
6. RMS 太低、best residual 太高、best/second margin 太小或发生 clipping 时，四个 metric 全置零。

整个过程使用 bounded `LumaReader`，默认 data work budget 8,000,000；没有 image-sized normalized copy，也没有 decode-time codec creation。

---

## 7. 低刷新率下的时序策略

### 7.1 “低逻辑帧率”不等于停止 Present

逻辑 raster 仅以 1..5 Hz 变化；Data Window 仍可按现有 flip/VSync contract 重复 present 同一 PB-owned texture。这样远端编码器有多个捕获/编码机会，但重复 present 绝不生成新 fountain equation，也不计为 unique data frame。

### 7.2 每个逻辑帧必须稳定驻留

- 5 Hz：至少 200 ms；
- 2 Hz：至少 500 ms；
- 1 Hz：至少 1000 ms。

新 raster 只能在完整生成并提交后替换旧 raster；禁止逐 tile 原地更新可见纹理。Renderer 的 pending replacement 可以丢弃尚未呈现的旧候选，但不能让同一可见 texture 包含两个 FrameSequence。

### 7.3 freshness region 处理远控局部刷新

画布按 128×128 logical region 划分。每个 eligible region 有分布式 freshness tags，由 `(SessionTag, FrameSequence, physicalIndex, regionId)` 决定。接收端：

- tag 与当前 Bootstrap FrameSequence 一致：该 region 的 data metrics 可用；
- 任一 tag 低置信或错误：整个 region stale；
- stale region 的四个 plane 全部是 zero LLR；
- QC-LDPC/Transport/CRC/identity/digest 仍按既有规则判断；没有 latest-wins，也不跨 FrameSequence 拼 soft data。

当前测试已把前一 FrameSequence 的一个完整 region 复制到当前 raster：解调识别 `staleRegions=1`，四个 codeword 经同一 `ReferenceChannel::EvaluateCodewords` 恢复，4/4 Transport blocks 被接受。

### 7.4 去重与 frame admission

权威身份仍是 CaptureEpoch + canonical Bootstrap + FrameSequence：

- 同一 FrameSequence 重复捕获只增加 capture/duplicate 统计，不重复喂 Outer FEC；
- Bootstrap 混合、不一致或 CRC 失败是整帧 erasure；
- freshness 只决定同帧局部 soft erasure，不允许从上一帧补数据；
- 最终文件只有 WholeFileDigest 通过后才发布。

---

## 8. 与现有 PixelBridge 架构的连接

LF4 **只替换 Data Visual modulation/demodulation**：

```text
File
  -> Segment compression (RAW/zstd)
  -> DirectRepeat or canonical Wirehair V2
  -> Transport block + CRC/session identity
  -> existing Robust QC-LDPC x4
  -> LF4 plane interleave + Walsh raster
  -> remote desktop/video channel
  -> locator + LF4 soft metrics
  -> existing Robust QC-LDPC decoder
  -> existing Transport/Receiver/conflict/resource checks
  -> WholeFileDigest
  -> no-overwrite final publish
```

没有新增 socket、pipe、共享内存、clipboard、COM、window-message 或 temp-file payload bypass；Control/Bootstrap/Transport/Wirehair/Receiver 均没有另起一套实现。

---

## 9. 已完成的实现

### 9.1 新增/修改代码

本节代码已进入提交 `9b8e8063a56251040cddb6b01e38c4343779571f`。

- `libs/PBModulation/include/pbmodulation/remote_visual_low_fps.h`
  - profile/layout 常量、16-symbol codebook、policy、workspace、encode/decode/metric API；
- `libs/PBModulation/src/remote_visual_low_fps.cpp`
  - LF4 raster、scale-aware ladder/chip sampling、max-log metric、freshness erasure、bounded workspace；
- `libs/PBModulation/src/local_desktop_internal.h`
  - 新 binding 使用同一 scaffold；
- `libs/PBDesktopLevelsReference/include/pbdesktoplevels/reference_channel.h`
- `libs/PBDesktopLevelsReference/src/reference_channel.cpp`
  - 4-codeword diagnostic data 与共享 FEC/Transport truth boundary；
- `tests/PBModulation/test_remote_visual_low_fps.cpp`
  - codebook、bijection、尺度、alias/bounds、freshness、stale-region、QC-LDPC/Transport tests；
- `apps/common/local_desktop_runtime.cpp`
- `apps/PixelBridgeEncoder/encoder_runtime_cli.cpp`
- `apps/PixelBridgeEncoder/encoder_gui.cpp`
  - 当前可选 RemoteVisual profile 强制 1..5 Hz，GUI 动态限制，CLI/config 双层 fail closed；
- `tools/PBRemoteVisualEvidence/analyze_remote_capture.py`
  - 用户截图只读 JSON 证据分析。

### 9.2 当前明确未完成

- LF4 尚未进入 `VisualProfile` product enum/GUI，避免未经 Gate 就冒充 production profile；
- D3D11 scaled Walsh demod shader 未实现；当前 production RemoteVisual shader 仍是旧 1-bit strict-1:1 path；
- 未运行真实远控双机、WGC/DXGI 或右侧屏幕 native field Gate；
- 未证明 1/2/5 Hz 的真实 `VerifiedEncodedGoodput`；
- 未建立 LF4 Golden PNG/manifest 冻结，因此 ID/layout 仍是 experimental checkpoint，不是 wire certification；
- 未证明 perspective/rotation/crop；当前明确拒绝。

---

## 10. 后续实施顺序与 Gate

### Gate A：CPU/协议真值（本轮已建立首个通过基线）

必须持续满足：

- 16 masks 平衡、唯一、minimum distance ≥8；
- 四个 plane 的 16,200 logical positions 分别构成 bijection；
- 0.5..2.0 合同内多种独立 scale/filter 能 bit-exact 恢复；
- stale region 只产生 zero metrics，不产生 false accept；
- 4 个 QC-LDPC codeword、Transport CRC/identity 全部通过；
- invalid policy、scale、span alias、work budget、frame bounds fail closed；
- frozen Direct-Level/Shape/RemoteVisual 旧测试不回归。

### Gate B：Provider-generic channel simulator

以一个变量一组实验的方式覆盖：

1. area/bilinear/bicubic resize，X/Y 独立 scale 与 fractional phase；
2. Gaussian blur、sharpen/ringing；
3. H.264/HEVC 4:2:0 与 4:4:4、limited/full range；
4. 8×8/16×16/64×64 局部 block replacement；
5. old/new frame alpha blend；
6. crop、letterbox、overlay、cursor/occluder；
7. duplicate/drop/reorder 和 CaptureEpoch change；
8. brightness/gamma/contrast change；
9. 两份 Bootstrap 不一致、freshness mismatch、低置信 tag；
10. adversarial valid CRC/identity conflict。

输出必须包含 PreFecBER、region erasure 分布、FEC/CRC/identity、false accept、accepted Transport、Outer unique admission 和最终 digest。

### Gate C：D3D11 production integration

- 在 locator 产生 geometry 后，把 `(origin, scale)` 写入 bounded constant buffer；
- compute shader 直接从 PB-owned capture texture 取样 4×4 chips；
- GPU 输出固定 64,800 metrics + freshness summary，不回读原始 ROI；
- CaptureEpoch/mode/size/device change 时 drain stale slots 并 rebuild；
- CPU reference 与 WARP/D3D quantized metric 或 accepted Transport bytes 对齐；
- 旧 strict-1:1 RemoteVisual 不被静默重解释；LF4 使用独立 ID/layout。

### Gate D：真实双机、右侧实验屏幕

环境安全：

- Encoder Data Window 只能完全位于 `ExperimentMonitor`；
- 与 `ProtectedMonitor` 有任意相交立即拒绝；
- no-activate，不移动/关闭外部窗口，不修改分辨率、DPI、HDR、刷新率；
- 所有 native 操作只在用户允许的右侧屏幕执行。

测试矩阵（不绑定具体 provider 名称）：

- logical FPS：1 / 2 / 5；
- provider mode：质量优先 / 自动 / 受限带宽（若产品提供）；
- scale：约 0.75 / 1.0 / 用户样本约 1.259 / 1.5；
- 文件：1 MiB 随机、1 MiB 可压缩、8 MiB、ZIP；
- WGC 主矩阵，DXGI 代表性复核；
- 每组保存 receiver-only Replay v2、两端 report、环境 fingerprint、外部 size/SHA-256。

成功判据不是“画面看起来清楚”，而是：

```text
logical FPS <= 5
+ no protected-monitor violation
+ no false accepted Transport/control/conflict
+ final size and external SHA-256 exact
+ WholeFileDigest passed before publish
+ result reproducible from captured Replay
+ reported goodput uses authoritative denominator
```

### Gate E：是否升级密度

只有 LF4 在 Gate D 可复现后才评估：

- `LF5-Shape`：更大 balanced codebook；
- `LF4+C2`：四 shape bits + 两个经过校准的 chroma bits；
- 更高 canvas utilization / 更少 scaffold overhead；
- 自适应 profile selection 只允许在新 Session 开始前根据 probe 选择，Session 内固定。

每个候选必须以 `VerifiedEncodedGoodput` 超过 LF4 且 false-accept/恢复稳定性不退化为准；不能只按 raw bits/tile 晋级。

---

## 11. 完整分步骤实施 WBS

### 11.1 星级、状态与执行规则

难度和重要性分别独立评分，均采用五级：

| 星级 | 难度含义 | 重要性含义 |
| --- | --- | --- |
| ★☆☆☆☆ | 单文件或机械性工作，失败影响局部 | 可选增强，不阻塞主路径 |
| ★★☆☆☆ | 少量模块、接口稳定、可完全离线验证 | 有价值但可延后，不影响正确性边界 |
| ★★★☆☆ | 跨模块或需要新的测试夹具 | 主路径组成部分，缺失会降低可用性或证据质量 |
| ★★★★☆ | 跨线程/GPU/持久化/双端协作，错误代价高 | 正式 Smoke/Gate 的必要条件 |
| ★★★★★ | 跨整条 production pipeline、真实硬件或长时间状态 | 最终文件正确性、无 false accept 或认证的硬阻断项 |

状态使用：`DONE` 已有提交和本地证据；`PARTIAL` 已有可复用实现但证据不足；`NEXT` 当前最先实施；`PENDING` 尚未开始；`MANUAL-GATE` 最终必须由双机人工环境提供外部状态。

执行顺序遵守以下规则：

1. 一个步骤只有在“完成出口”的权威证据全部存在时才能变为 `DONE`；编译成功不能替代 runtime、replay 或最终文件证据。
2. 每个步骤先增加失败测试或可复现 fixture，再改 production 代码；一次只改变一个信道变量。
3. CPU/reference 是数学真值，GPU 以 accepted Transport bytes、CRC、identity、FEC 和 digest 对齐，不要求浮点 metric bit-identical。
4. 新 profile 只能通过独立 ID/layout 引入；不得改写 Direct-Level、Shape+Chroma、旧 RemoteVisual 或历史 Golden。
5. 任何真实窗口、capture 或 ROI 实验只能位于 `ExperimentMonitor`；本地无界面步骤不得借机启动 native Gate。
6. 每个可独立审查的步骤使用显式路径暂存和非 amend 原子提交；不提交 build、replay、截图或大体积 evidence。

### 11.2 路线总表

| # | 步骤 | 状态 | 难度 | 重要性 | 主要完成证据 |
| ---: | --- | --- | --- | --- | --- |
| 01 | 冻结基线、tag 与证据边界 | DONE | ★★☆☆☆ | ★★★★★ | `PBPhase1GateTagIdentity` 自动复核 tag object/peeled commit，并断言 build/evidence 未被 git 跟踪 |
| 02 | 失真样本分析与 RemoteVisual 数据集入口 | DONE | ★★☆☆☆ | ★★★★★ | Direct/Shape/LF4 真实 receiver-only Replay、三项 dataset seal、同一 Inspector 双跑逐字节一致 |
| 03 | LF4 profile identity、codebook 与容量真值 | DONE | ★★★☆☆ | ★★★★★ | `9b8e806`、codebook/bijection tests |
| 04 | CPU 连续尺度 demod、freshness erasure、四 codeword 真值链 | DONE | ★★★★☆ | ★★★★★ | scale/stale/QC-LDPC/Transport tests |
| 05 | Provider-generic deterministic channel transform API | DONE | ★★★★☆ | ★★★★★ | 独立 transforms、seed manifest、resource bounds |
| 06 | 信道 impairment 矩阵与 adversarial corpus | DONE | ★★★★☆ | ★★★★★ | 22-case transforms、6 actual codecs、11-event temporal/identity、Receiver/Outer/digest truth |
| 07 | Soft metric 标定、阈值选择与 false-confidence Gate | PENDING | ★★★★★ | ★★★★★ | train/holdout split、BER/FER/false accept curves |
| 08 | LF4 Golden/manifest 冻结与兼容性声明 | PENDING | ★★★☆☆ | ★★★★★ | canonical raster/hash/accepted-block manifest |
| 09 | LF4 D3D11 Encoder raster/immutable texture 接入 | PENDING | ★★★★☆ | ★★★★☆ | CPU raster parity、stable dwell/present evidence |
| 10 | LF4 D3D11 scaled Walsh demod shader | PENDING | ★★★★★ | ★★★★★ | bounded metrics/freshness GPU output、无 ROI readback |
| 11 | CPU/WARP/hardware GPU truth parity | PENDING | ★★★★☆ | ★★★★★ | accepted Transport equivalence、adapter matrix |
| 12 | CaptureNormalize、geometry、epoch 与 D3D lifetime 收口 | PENDING | ★★★★★ | ★★★★★ | WGC/DXGI epoch/recreate/stale-drain tests |
| 13 | duplicate/gap/reorder/stall 与 bounded queue 收口 | PARTIAL | ★★★★☆ | ★★★★★ | temporal corpus、HWM、无重复 Outer admission |
| 14 | 四 codeword Transport/Outer/Receiver production admission | PARTIAL | ★★★★☆ | ★★★★★ | 同帧绑定、unique/duplicate/conflict、digest |
| 15 | Replay v2 的 LF4 live/offline 一致性 | PARTIAL | ★★★★☆ | ★★★★★ | receiver-only replay、live/offline metric consistency |
| 16 | LF4 telemetry、RunReport.2 与严格 merger | PARTIAL | ★★★☆☆ | ★★★★★ | authoritative denominators、双端 combined report |
| 17 | GUI/CLI/profile 暴露与屏幕安全 preflight | PARTIAL | ★★★☆☆ | ★★★★☆ | 1..5 Hz fail-closed、no-activate、right-monitor gate |
| 18 | 可复现便携包、source set 与环境 fingerprint | PARTIAL | ★★☆☆☆ | ★★★★☆ | package manifest、双端 EXE/DLL hashes |
| 19 | 同提交 LocalDesktop 回归 | PENDING | ★★★☆☆ | ★★★★★ | WGC/DXGI × Direct/Shape、goodput regression ≤10% |
| 20 | 真实双机 LF4 pilot | MANUAL-GATE | ★★★★☆ | ★★★★★ | 1 MiB、1/2/5 Hz、Replay、external SHA-256 |
| 21 | 正式 provider-generic RemoteVisual 矩阵 | MANUAL-GATE | ★★★★★ | ★★★★★ | mode/scale/FPS/backend matrix 与 failure classification |
| 22 | 重复完整文件恢复验收 | MANUAL-GATE | ★★★★☆ | ★★★★★ | 1 MiB 3/3、8 MiB 2/2、ZIP 1/1 |
| 23 | QoS 扰动与恢复后继续收敛 | MANUAL-GATE | ★★★★☆ | ★★★★★ | 限速/恢复时间线、无 false accept、最终 digest |
| 24 | 6 小时 bounded soak 与性能预算 | PENDING | ★★★★☆ | ★★★★☆ | memory/queue drift、CPU/GPU、shutdown drain |
| 25 | LF5/LF4+C2 密度升级决策 | PENDING | ★★★★★ | ★★★☆☆ | 实测 goodput 优于 LF4 且稳定性不退化 |
| 26 | production multi-segment/late-join scheduler | PENDING | ★★★★★ | ★★★★☆ | O(active segments)、持续 Carousel、任意文件规模 |
| 27 | Certified profile Gate 与最终报告 | PENDING | ★★★★★ | ★★★★★ | 全矩阵、sealed evidence、无 Critical/High |

### 11.3 Step 01：冻结基线、tag 与证据边界

- **状态**：`DONE`；**难度**：★★☆☆☆；**重要性**：★★★★★。
- **目标**：保证低帧率路线不会追溯重定义历史 Phase-1 Gate，并保留修改前 LocalDesktop 和真实 RemoteVisual 失败证据。
- **实施要点**：记录 base commit、annotated tag object、peeled commit、source hash、显示器物理矩形和两轮 raw baseline；大体积证据只放在 `build-p1_5-evidence`。
- **注意事项**：历史 tag 不 amend/move/recreate；旧报告不能用新测试回填；用户原有 `docs/PHASE1_GATE_REPORT.md` 不自动暂存。
- **验收证据**：`phase1-gate-pass` 仍为 `fde56c...`，peeled 为 `806998...`；四个 pre-change LocalDesktop 文件 hash 一致；Direct/Shape RemoteVisual 无 descriptor/Transport 的失败报告保留。
- **本次复核**：在 HEAD `6669cdf` 上 `git rev-parse phase1-gate-pass` = `fde56c4c4e7124e8ffe29a0dcb619f8236781ebb`，`phase1-gate-pass^{commit}` = `80699813b595bcf6db64047b50d31056872e33e1`；`build-p1_5-evidence/p1_5-prechange-20260830-193716-cd533fe/` 中的四轮 LocalDesktop reference 报告与 Direct/Shape `failure-classification.json`、`decoder-report.json` 均仍在原地，暂存区为空。
- **自动复核实现**：`cmake/PBVerifyPhase1GateTagIdentity.cmake` 内置冻结常量并禁止调用方覆盖（否则任何调用方都能传入 tag 当前指向的值来“通过”检查）；先校验 pin 自身是 40 位小写 hex，再断言 git work-tree 顶层目录等于请求根目录（git 会向上遍历，否则校验的是别的仓库）、`cat-file -t` 必须是 `tag`（拒绝同名轻量 tag）、`rev-parse <tag>` 与 `rev-parse <tag>^{commit}` 必须等于冻结 tag object 与 peeled commit，并用 `cat-file tag` 头部交叉验证 payload 的 `object`/`type`/`tag`/`tagger`；最后断言 `build/` 与 `build-p1_5-evidence/` 都没有被 git 跟踪。脚本注册为 CTest `PBPhase1GateTagIdentity`，`New-PBRemoteVisualPortablePackage.ps1` 也必须在写 manifest 前通过它，因此 tag 一旦移动，打包直接失败。
- **历史口径**：`build-p1_5-evidence/20260831-step06-production-truth-v2-validation-1936c02.json` 把 `phaseStatus.step01` 记为 `DONE`，而当时 §11.3 仍是 `PARTIAL`；差异来自该摘要只记录“tag identity 已人工核对”，未检查完成出口要求的自动复核。封存摘要保持原样不改写，状态口径以本节为准。
- **完成出口**：每次后续提交前后自动复核 tag identity，且 `git status` 中没有 evidence/build artifact 被暂存。`git ls-files` 实测 `build/` 与 `build-p1_5-evidence/` 均为 0 个被跟踪文件，暂存区为空。

### 11.4 Step 02：失真样本分析与数据集入口

- **状态**：`DONE`；**难度**：★★☆☆☆；**重要性**：★★★★★。
- **目标**：把截图、真实 ROI frame sequence 和 metadata 转为可重复、可校验、最小隐私范围的数据源。
- **实施要点**：使用 `analyze_remote_capture.py` 做 bounded/read-only 初筛；`seal_remote_visual_dataset.py` 对显式 artifact root 下的 screenshot/Replay v2 生成 create-only SHA-256+BLAKE3+size+analysis index，拒绝 path escape、symlink/junction、重复 JSON key、非有限数值、变化中的输入和 protected-monitor pixels。Replay v2 仍由现有 writer 保存 selected ROI、CaptureEpoch、timestamp、format、live observation，并且必须先经 `ReplayV2Reader`/offline decoder 才能成为语义证据。
- **注意事项**：edge detector 只是诊断候选，不能改变 acceptance；不得保存左屏或 ROI 外像素；输入在分析期间变化必须拒绝；不能根据 provider 名称选择阈值。
- **已有证据**：analyzer/sealer deterministic/resource/parser/no-overwrite tests；两张用户提供且明确只含 `ExperimentMonitor` 的真实 Sunlogin Direct/Shape 失真截图被封成同一 byte-identical index；该历史截图索引的 `acceptanceInput=false`、`containsProtectedMonitorPixels=false`、`replayV2Count=0` 口径保持不变。2026-09-01 又在 Windows Remote Desktop receiver-only 路径取得 Direct/Shape/LF4 各 8 帧的封口 Replay v2；三项 create-only dataset index 两次逐字节一致，SHA-256=`c3e81b56163f150be9a1b2cfa133c40f46b494390902c5a588e16d5658795b95`，`replayV2Count=3`、`realRemoteRenderCount=3`、`containsProtectedMonitorPixels=false`、`acceptanceInput=false`。
- **验收证据**：`PBRemoteVisualReplayInspector` 对每份 Replay 都先用 `ReplayV2Reader` 完整读到 footer，再调用现有 Bootstrap、profile reference decoder、QC-LDPC、padding、Transport CRC 与 Session identity 判决链；每份 Replay 用同一个可执行文件独立运行两次，完整 sealed JSON 逐字节一致。Direct/Shape/LF4 分别得到 8/8 Bootstrap、8/8 modulation、8/8 Transport accepted frames，accepted Transport blocks 分别为 336/256/32，FEC/CRC/identity failures 均为 0。完整证据见第 15 节。
- **完成出口**：已满足。该出口只证明真实 receiver-only ROI frame sequence 可重复分类；没有 sender truth 时 `falseAcceptedCodewords=null`，WholeFileDigest/final publish 固定为 `NotEvaluated`。Step 15 的 live/offline production parity 与 Step 20 的真实文件发布 Gate 仍是独立硬出口。

### 11.5 Step 03：LF4 identity、codebook 与容量真值

- **状态**：`DONE`；**难度**：★★★☆☆；**重要性**：★★★★★。
- **目标**：建立独立、可版本化、低空间频率的 4 bit/tile 物理候选，不依赖字体、Unicode 或色度。
- **实施要点**：16 个 balanced 4×4 Walsh masks、2×2 pixels/chip、四 plane interleave、独立 profile/layout、8100 coded bytes/frame。
- **注意事项**：profile ID 不能与旧 profile 复用；codebook 一旦进入 Golden 就不能原地更换；理论 5256 B/frame 不是 VerifiedGoodput。
- **验收证据**：16 masks 唯一、popcount=8、minimum Hamming distance≥8；每个 plane 16200 positions 为 bijection；encode 输入/输出 alias 和错误 Bootstrap fail closed。
- **完成出口**：提交 `9b8e8063a56251040cddb6b01e38c4343779571f` 及其 C++/ASan tests 保持通过。

### 11.6 Step 04：CPU 连续尺度 demod、freshness erasure 与四 codeword 真值链

- **状态**：`DONE`；**难度**：★★★★☆；**重要性**：★★★★★。
- **目标**：在不整帧 resize 的前提下，以 locator 的连续 origin/scale 采样 LF4，并把局部陈旧块变成 soft erasure。
- **实施要点**：0.5..2.0 axis-aligned scale；ladder calibration；chip multi-sample；max-log bit metrics；region freshness；共享 QC-LDPC/Transport reference path。
- **注意事项**：不支持 perspective/rotation/crop 时必须拒绝；不能跨 FrameSequence 拼 metrics；hard/soft output 只在整帧成功后写出。
- **验收证据**：1.0、0.5、0.75×1.5、1.259375×1.259259、2.0 独立 resampler；stale region=1 时四 codeword 仍通过；invalid scale/policy/bounds/alias fail closed。
- **完成出口**：CPU reference 对 scale 和 stale fixtures 恢复 bit-exact coded data，并产生 4/4 verified Transport blocks。

### 11.7 Step 05：Provider-generic deterministic channel transform API

- **状态**：`DONE`；**难度**：★★★★☆；**重要性**：★★★★★。
- **目标**：建立唯一的离线信道变换层，先把 production LF4 raster 经过可组合、可 seed、可审计的 resize/block-replacement 送回 production CPU decoder，并为 Step 06 的 blur/temporal/color transforms 提供同一扩展入口。
- **实施要点**：新模块只实现 channel transforms 和 manifest，不复制 Bootstrap/Demod/FEC/Receiver；每个 transform 明确输入格式、边界、seed、参数和输出 hash；使用 checked arithmetic 和 bounded allocation。
- **注意事项**：一次实验只改变一个变量；transform 顺序必须进入 manifest；不能把 decoder 预期值注入解码；simulator 成功不是 field success。
- **验收证据**：identity transform bit-exact；相同 seed/manifest 输出 hash 一致；invalid dimension/kernel/range/overflow/oversize 拒绝且不部分写出。
- **完成出口**：`PBRemoteVisualSimulator` API、单元测试、`PixelBridge.RemoteVisualChannelManifest.1` schema 和最小 resize/block-replacement pipeline 已进入独立提交；规范见 `REMOTE_VISUAL_CHANNEL_MANIFEST.md`。

### 11.8 Step 06：信道 impairment 矩阵与 adversarial corpus

- **状态**：`DONE`；**难度**：★★★★☆；**重要性**：★★★★★。
- **目标**：用可重复 corpus 覆盖真实远控可能产生的空间、颜色、时序和 parser/identity 失真。
- **实施要点**：逐项 sweep area/bilinear/bicubic、fractional scale/phase、blur/ringing、gamma/contrast、4:2:0/4:4:4、limited/full range、block replacement、alpha mix、crop/letterbox/overlay、duplicate/drop/reorder/epoch。
- **当前增量**：Manifest v2 已加入固定 3×3 blur/sharpen、gain/bias/gamma、integer 4:2:0 proxy、crop、solid/alpha overlay、reference blend、8/16/64 block replacement、Bootstrap mismatch、freshness low-confidence 和固定 Q16/Q20 Catmull-Rom bicubic；22-case matrix 得到 18 个 `Verified`、4 个 `ErasureNoFalseAccept`、0 production false acceptance、0 expectation mismatch。每个 case 同时运行 `DiagnosticTruth` 与不接触 sender expected bytes 的 `Transport` 模式；production-accepted blocks 再进入现有 `ReceiverIngress`/DirectRepeat Outer/segment verify/finalization。18 个完整 case 均得到 4 unique Outer symbols、synthetic one-segment WholeFileDigest PASS，4 个 erasure case 保持 `NotReady`，没有 publish 声明。`PBRemoteVisualCodecProbe` 的 6 个 H.264/HEVC case 覆盖 4:2:0 limited、4:4:4 limited/full range；18 帧仅 1 帧 Verified、17 帧 erasure、4 unique Outer symbols、6/6 case WholeFileDigest `NotReady`、0 production false acceptance。`PBRemoteVisualTemporalCorpus` 以 11 events 证明 duplicate/reorder 抑制、gap/epoch、一次性 duplicate refinement，以及不同 SessionTag 的 CRC-valid adversarial codeword 在 production Transport identity 上 0 admission；FrameSequence 与 SegmentOrdinal 不被错误绑定。
- **注意事项**：H.264/HEVC 应保存实际 bitstream 参数并用 inspector 验证；crop/letterbox 不得被 simulator 隐式纠正；valid CRC + wrong identity 必须单独测试。
- **验收证据**：每个 case 的 canonical input/output hash、truth manifest、Bootstrap、BER/FER、stale distribution、accepted Transport、false accept 和 digest 结果。
- **完成出口**：`build_step06_corpus.py` 可从空 scratch 一条命令重建 transform、actual codec 和 temporal corpus，并交叉核对 source/evaluation report 与实际 BGRA/Gray8 字节。implementation commit `1936c020cb2c017b9ce3064267f497d15f94d66d` 的两次 create-only rebuild `20260831-step06-production-truth-v2-a`/`-b` 各含 34 个相对文件，路径、size、SHA-256 全部一致；root `SHA256SUMS.txt` SHA-256=`2e3d269fe5b921aacea45b53441fe5b0d46e9563dc810547d6aaf142798e70a0`，`step06-corpus-index.json` SHA-256=`100b061fb275256c00f37642791cfafcd677d00c88b66f8ccfeecb69fa84d4aa`。完整规范与结果见 `REMOTE_VISUAL_STEP06_CORPUS.md`。

### 11.9 Step 07：Soft metric 标定与 false-confidence Gate

- **状态**：`DONE`；**难度**：★★★★★；**重要性**：★★★★★。
- **目标**：避免“高置信错误”穿过 FEC，选择可泛化而非针对单个 provider 的 metric scale、erasure 和 symbol-margin policy。
- **实施要点**：按 dataset/run 分离 train、validation、holdout；统计 bit-conditioned metric、reliability diagram、BER/FER、CRC/identity、symbol residual/margin；阈值只来自跨 dataset 结果。
- **注意事项**：不能用 holdout 调参；没有 truth coverage 时 falseAcceptedCodewords 必须为 unavailable；CRC failure 不能当 soft success；阈值变更需要新 profile/layout 或明确 local tuning 边界。
- **验收证据**：ROC/reliability/FER curves、parameter manifest、holdout external results、0 false accepted Transport/control/output。
- **完成出口**：已关闭。24 项 policy/model candidate 只用 Validation 选择 `lf4-default/PiecewiseLookup`；Holdout 与一组 Step 02 真实 RDP Replay 的 log loss/ECE 均严格优于 `lf4-default/Raw`，hard BER/FER/FEC/accepted Transport 无回退，false accepted Transport/control/output 均为 0，且 production default/admission 未变。两次最终 create-only seal 的 `metric-calibration.json` 逐字节一致，SHA-256=`c17f17844c0b44cf029bb31ecbfc127139fa886588c941c2a3a0f34bf541acf8`。完整边界与复现命令见 `REMOTE_VISUAL_STEP07_CALIBRATION.md` 和第 16 节。

### 11.10 Step 08：LF4 Golden/manifest 冻结

- **状态**：`PENDING`；**难度**：★★★☆☆；**重要性**：★★★★★。
- **目标**：把通过 simulator/metric Gate 的 LF4 raster、mapping、codebook、Bootstrap binding 和 accepted Transport 结果变成不可漂移的实验 Golden。
- **实施要点**：固定 seed/session/frame；记录 canonical raster BLAKE3、coded bytes、metric quantization contract、4 个 accepted Transport hashes；提供独立 generator `--check`。
- **注意事项**：当前 `9b8e806` 只是 checkpoint，不能在真实 Gate 前宣称 frozen/certified；Golden 不保存大图时至少保存 manifest 和可重建 seed。
- **验收证据**：clean scratch 独立重生成；CPU/Qt5/Qt6/ASan 一致；旧 32 fixtures 仍不变。
- **完成出口**：任何 codebook/mapping/profile drift 都导致 Golden check 非零退出。

### 11.11 Step 09：LF4 D3D11 Encoder raster 与 immutable texture

- **状态**：`PENDING`；**难度**：★★★★☆；**重要性**：★★★★☆。
- **目标**：让 production Encoder 生成完整 LF4 raster 并以 1..5 Hz 替换 PB-owned texture，同时持续 Present 同一稳定 raster。
- **实施要点**：把四 codeword 的 raster generation 接入现有 scheduler；完整生成后一次性提交；重复 Present 不增加 FrameSequence/OuterBlockId；记录 dwell、generated/present FPS。
- **注意事项**：禁止逐 tile 更新可见纹理；pending replacement 可以丢弃未呈现候选但不能 torn；Encoder 不显示 receiver progress/ETA；0 和 >5 fail closed。
- **验收证据**：CPU canonical raster 与 GPU-present source hash/diagnostic readback 对齐；logical updates≤5；Present 可保持显示刷新率；窗口 no-activate/right-monitor containment。
- **完成出口**：Encoder 在 headless/native test 中持续广播多个 Carousel cycles，Decoder 完成后 Encoder 仍在运行。

### 11.12 Step 10：LF4 D3D11 scaled Walsh demod shader

- **状态**：`PENDING`；**难度**：★★★★★；**重要性**：★★★★★。
- **目标**：直接从 PB-owned capture texture 按连续 geometry 解调 64,800 soft metrics 和 freshness summary，不回读整张 ROI。
- **实施要点**：constant buffer 包含 origin/scale/pitch/bounds/policy；one-owner immediate context；bounded slot/ring；GPU 输出仅 compact metrics/counters；shader 与 CPU 使用同一 codebook/mapping constants。
- **注意事项**：实际 texture pitch/format 必须显式处理；Flush 不是 completion；source lease 在 GPU copy 完成前不能归还；旧 strict-1:1 shader 不得静默解释 LF4 layout。
- **验收证据**：exact/scale/blur/stale fixtures 的 WARP tests；out-of-bounds/NaN/epoch stale fail closed；无 ROI-sized GPU→CPU readback。
- **完成出口**：同一 frame 的 GPU metrics 经现有 FEC/Transport 得到与 CPU reference 相同的 accepted blocks。

### 11.13 Step 11：CPU/WARP/hardware GPU truth parity

- **状态**：`PENDING`；**难度**：★★★★☆；**重要性**：★★★★★。
- **目标**：证明 LF4 production GPU path 在跨 adapter/driver 环境下保持协议接受结果一致。
- **实施要点**：比较 canonical Bootstrap、FrameSequence、FEC disposition、Transport bytes、CRC/identity/padding，而不是 raw float；覆盖 WARP、NVIDIA、AMD 可用 adapter。
- **注意事项**：硬件不可用只能标记 unavailable，不能由 WARP 代替；adapter LUID 不匹配时不得隐式跨 GPU copy；量化容差必须在 FEC truth boundary 验证。
- **验收证据**：adapter fingerprint、accepted block manifest、metric summary、GPU timestamps、device-recreate case。
- **完成出口**：所有可用 adapter 对 corpus 的 accepted Transport 集合完全一致，差异 case 必须 fail closed 而非降级接受。

### 11.14 Step 12：CaptureNormalize、geometry、epoch 与 lifetime

- **状态**：`PENDING`；**难度**：★★★★★；**重要性**：★★★★★。
- **目标**：把 WGC/DXGI ROI、format、ContentSize、monitor/DPI/rotation 和 CaptureEpoch 正确绑定到 LF4 geometry/demod work。
- **实施要点**：单显示器 physical ROI；WGC lease copy 到 PB texture；epoch change drain；topology fingerprint 变化停止 run；scale/letterbox/crop status 进入 telemetry。
- **注意事项**：不能访问归还后的 WGC surface；不能跨 epoch combine；未知 geometry 只能 diagnostic replay，不能 publish；不捕获 ProtectedMonitor。
- **验收证据**：resize/mode/device loss/recreate、epoch rollover、stale completion、DXGI/WGC normalization tests；lease/HWM 回到零。
- **完成出口**：任何 geometry/lifetime 变化要么安全 rebuild 新 epoch，要么明确终止，不出现 use-after-lease、跨 epoch admission 或无界 backlog。

### 11.15 Step 13：duplicate/gap/reorder/stall 与 bounded queue

- **状态**：`PARTIAL`；**难度**：★★★★☆；**重要性**：★★★★★。
- **目标**：使远控重复帧、variable cadence、局部停更和恢复跳帧只影响 goodput，不破坏身份和资源上界。
- **实施要点**：`(CaptureEpoch, FrameSequence)` 去重；reordered 不 admission；gap/skipped 记账；1 s capture/visual stall；capture/demod/result queues 丢 stale 而不积累延迟。
- **注意事项**：像素相同不一定等价于 Bootstrap identity；同 FrameSequence 失败 observation 是否重试要由 bounded policy 明确；duplicate 不累计 BER/FER/verified bytes。
- **验收证据**：deterministic temporal corpus；HWM≤配置容量；恢复后继续接受新唯一帧；shutdown drain 完成。
- **完成出口**：长时间 duplicate/stall 不增加 Outer unique count或内存，恢复后 Receiver 可继续收敛。

### 11.16 Step 14：四 codeword Transport/Outer/Receiver production admission

- **状态**：`PARTIAL`；**难度**：★★★★☆；**重要性**：★★★★★。
- **目标**：把 LF4 同一视觉帧的四个 codeword 作为四个独立、严格验证的 Transport candidates 送入现有 Receiver，而不建立新协议。
- **实施要点**：同 captured frame/Bootstrap/FrameSequence binding；逐 codeword FEC/padding/CRC/identity；Outer unique/duplicate/conflict；资源拒绝可观测。
- **注意事项**：四个 codeword 不要求同时成功，但不能来自不同 FrameSequence；same ID conflicting payload 必须 fail closed；duplicate 不增加 progress。
- **验收证据**：0..4 codeword success matrix、same-frame binding、conflict、orphan cache、resource limit、DirectRepeat/Wirehair tests。
- **完成出口**：production Receiver 的 verified bytes 只由唯一且完整验证的 encoded mutation 增加，最终仍需 WholeFileDigest。

### 11.17 Step 15：Replay v2 LF4 live/offline 一致性

- **状态**：`PARTIAL`；**难度**：★★★★☆；**重要性**：★★★★★。
- **目标**：让每次真实 LF4 失败都能在没有远控软件的情况下重放 Bootstrap、GPU/CPU demod、FEC 和 Receiver。
- **实施要点**：presence flags 表示 sender truth 缺失；记录 profile/layout、geometry、CaptureEpoch、observation ID、ROI bytes、live result；offline mode 走同一 production decoder。
- **注意事项**：recorder queue 满只丢样本不阻塞 demod；默认 256 frames/2 GiB，硬上限 2048/16 GiB；`.partial` 和 no-overwrite；只保存 selected ROI。
- **验收证据**：v1 compatibility、v2 receiver-only、corrupt/truncated/oversize/checksum/no-overwrite、live/offline metric consistency。
- **完成出口**：至少 Direct/Shape/LF4 各一个真实 dataset 可离线复现其成功或相同失败分类。

### 11.18 Step 16：LF4 telemetry、RunReport.2 与 merger

- **状态**：`PARTIAL`；**难度**：★★★☆☆；**重要性**：★★★★★。
- **目标**：使 1..5 Hz 下的容量、重复 Present、unique visual、四 codeword 和 verified goodput 各有正确分母。
- **实施要点**：增加 LF4 symbol unreliable、fresh/stale region、erased metrics、per-codeword FEC、accepted Transport、Outer unique；sender receiver metrics 继续为 null；严格 merge profile/session/source/time。
- **注意事项**：0 不代表 unavailable；PreFecBER 没 truth 时必须 null；ceiling/generation/presentation/verified goodput 不得混用；journal failure 使 evidence invalid 但不改 acceptance。
- **验收证据**：unit tests 覆盖 denominator、null semantics、duplicate、time/provider/profile mismatch；combined JSON/Markdown/CSV seal。
- **完成出口**：成功报告必须同时证明 Decoder Completed、WholeFileDigest、publish、external length/SHA-256，并能追到两端 artifacts。

### 11.19 Step 17：GUI/CLI/profile 暴露与屏幕安全

- **状态**：`PARTIAL`；**难度**：★★★☆☆；**重要性**：★★★★☆。
- **目标**：只有 Gate A-C 通过后才把 LF4 加入 product enum/GUI/CLI，并让非法 FPS/geometry/monitor 配置在启动前失败。
- **实施要点**：独立 `remote-lf4` profile token；1..5 FPS；metadata preset；ROI physical rect/snap/scale status；no-activate Data Window；topology watchdog。
- **注意事项**：不能把旧 `remote` token 重解释成 LF4；不自动移动外部远控窗口；不使用全局输入；不显示 receiver progress 于 Encoder。
- **验收证据**：config/QSettings/CLI parser/GUI model tests；right-monitor containment；ProtectedMonitor intersection fail closed；focus PID 前后不变。
- **完成出口**：用户可以显式选择 LF4，任何默认值都不会在不知情时改变既有 profile 或 wire identity。

### 11.20 Step 18：便携包、source set 与环境 fingerprint

- **状态**：`PARTIAL`；**难度**：★★☆☆☆；**重要性**：★★★★☆。
- **目标**：确保 Computer A/B 实际运行相同 tested tree、依赖和 profile，而不是混用旧包。
- **实施要点**：create-only package；manifest 包含 commit/tree/compiler/Qt/vcpkg/EXE/DLL hashes；CSPRNG source set；共享 metadata/RunId；endpoint environment fingerprint。
- **注意事项**：Computer B source 在 Session 中不可变；包不能携带 payload 旁路；manifest hash 必须在复制前后核对；provider UI 信息只作为 metadata。
- **验收证据**：clean directory package rebuild、hash verify、tamper negative test、两端 packageManifest 完全匹配。
- **完成出口**：任何正式 run 都能由 RunId 反查唯一 package、source 和 endpoint environment。

### 11.21 Step 19：同提交 LocalDesktop 回归

- **状态**：`PENDING`；**难度**：★★★☆☆；**重要性**：★★★★★。
- **目标**：证明 LF4、scheduler、telemetry 和 GPU 修改没有破坏 frozen LocalDesktop oracle。
- **实施要点**：相同 8 MiB RAW、同 ROI/显示器、WGC/DXGI × Direct/Shape；replay off；比较 goodput/FER/unique FPS/CPU/GPU/HWM。
- **注意事项**：必须用最终 tested commit；用户左屏有并发工作则记录，不伪装 clean benchmark；WholeFileDigest/false accept/unbounded queue 直接 BLOCK。
- **验收证据**：pre/post combined table、external hash、Encoder still broadcasting；goodput regression≤10% 或具有可复现解释。
- **完成出口**：四组合全部 publish/hash PASS，旧 Golden/Transport 不变，无未解释性能或资源退化。

### 11.22 Step 20：真实双机 LF4 pilot

- **状态**：`MANUAL-GATE`；**难度**：★★★★☆；**重要性**：★★★★★。
- **目标**：以最小 1 MiB RAW 证明 LF4 从 Computer B Data Window 经真实远控视频到 Computer A Receiver 的第一条完整链。
- **实施要点**：分别跑 1/2/5 Hz；优先 strict 1:1，再跑可定位 scale；WGC；每轮保存 Replay v2、两端 report、remote UI metadata、external hash。
- **注意事项**：Decoder 成功即 run 成功，Encoder 仍广播后人工 Stop；最长窗口/无进展规则预先固定；不得使用文件传输、clipboard 或任何旁路。
- **验收证据**：WholeFileDigest、publish、external length/SHA-256、0 false output、Replay offline reproduce。
- **完成出口**：至少一个 FPS 配置完成一次端到端恢复；失败也必须能归入 geometry/signal/temporal/metric/scheduler 之一。

### 11.23 Step 21：正式 provider-generic RemoteVisual 矩阵

- **状态**：`MANUAL-GATE`；**难度**：★★★★★；**重要性**：★★★★★。
- **目标**：跨 remote mode、scale、FPS、backend 确定 LF4 的可用域与 blocker，不绑定具体远控品牌。
- **实施要点**：1/2/5 Hz；质量优先/自动/受限模式；约 0.75/1.0/1.259/1.5 scale；WGC 主矩阵、DXGI 代表性复核；每组固定 run window 和 cycle 条件。
- **注意事项**：模式是否真的启用必须来自 UI 可见证据；unknown chroma/latency 不猜值；不同 run 不能合并；非法 geometry 不 silent resample。
- **验收证据**：per-run endpoint/combined reports、CSV、Replay、environment/package seals、failure classification。
- **完成出口**：Direct/Shape/LF4 均有成功或失败证据，且 LF4 的提升或退化由权威指标而非截图主观判断支持。

### 11.24 Step 22：重复完整文件恢复验收

- **状态**：`MANUAL-GATE`；**难度**：★★★★☆；**重要性**：★★★★★。
- **目标**：把偶然一次恢复提升为可重复文件级 Smoke evidence。
- **实施要点**：最佳配置下 1 MiB random 3/3、8 MiB random 2/2、含 4 MiB random payload 的 ZIP 1/1，全部 Segment compression RAW/OFF。
- **注意事项**：每轮使用唯一 RunId；source hash 固定但输出路径 no-overwrite；失败不能删掉重跑只保留成功；Sender cycle position 不是 completion。
- **验收证据**：每轮 WholeFileDigest、publish、external size/SHA-256；无 false output；统计 completion time/VerifiedGoodput/FER/stale/duplicates。
- **完成出口**：满足上述 6/6 文件级 run，才允许 `RemoteVisualSmokePass=true`；`CertifiedRemoteVisualProfile` 仍为 false。

### 11.25 Step 23：QoS 扰动与恢复后继续收敛

- **状态**：`MANUAL-GATE`；**难度**：★★★★☆；**重要性**：★★★★★。
- **目标**：验证远控自适应造成 FPS/画质/重复/停更时，持续广播能够在网络恢复后继续 Receiver convergence。
- **实施要点**：稳定期 60 s；约中位带宽 60% cap 120 s；第 90 s 启 Decoder；第 120 s 恢复；记录完整时间线。
- **注意事项**：QoS 必须由用户/环境人工确认恢复；不能自动操作路由器；无法测量 baseline 或恢复状态则 run blocked，不猜数值。
- **验收证据**：remote FPS/bandwidth/latency、stalls/gaps/duplicates、Bootstrap/FER/goodput、恢复后的 unique admission 和最终 digest。
- **完成出口**：至少一次扰动 run 在恢复后完成，且扰动期间没有 false accepted Transport/output 或无界 queue。

### 11.26 Step 24：6 小时 bounded soak 与性能预算

- **状态**：`PENDING`；**难度**：★★★★☆；**重要性**：★★★★☆。
- **目标**：证明低帧率持续广播、capture、replay-off receive 和失败重试不会造成内存/句柄/queue 漂移。
- **实施要点**：成功和长期失败场景各一轮；1 Hz journal 上限；采样 working/private bytes、handles、CPU/GPU、HWM、epoch/recreate、stale drops；正常 shutdown drain。
- **注意事项**：build/test 进程不升 High/Realtime priority；并发用户 workload 要进 metadata；达到 journal 上限只停止采样并标 truncated。
- **验收证据**：rolling median drift、high-water、handle count、queue bounds、shutdown latency、no crash/ASan issue。
- **完成出口**：6 小时内资源增长有界且可解释，Stop 后 lease/demod/result/replay writer 全部清空。

### 11.27 Step 25：LF5/LF4+C2 密度升级决策

- **状态**：`PENDING`；**难度**：★★★★★；**重要性**：★★★☆☆。
- **目标**：只在 LF4 已稳定的前提下判断更多 shape bits 或 chroma bits 是否提高最终 VerifiedGoodput。
- **实施要点**：用真实 replay 训练独立 codebook optimizer；评估 minimum distance、confusion、4:2:0 survival、FEC/Outer overhead；每个候选使用新 profile ID/layout。
- **注意事项**：不在 Session 中 adaptive switch；颜色 plane 系统性丢失时必须回退为新 Session 的 LF4，而不是把全零 metric 伪装正常；raw bits/tile 不作为晋级指标。
- **验收证据**：相同 dataset/CPU/GPU/file matrix A/B；VerifiedGoodput、completion probability、false acceptance、资源成本。
- **完成出口**：候选跨 holdout 和真实双机显著优于 LF4且稳定性不退化，否则明确保留 LF4并关闭升级。

### 11.28 Step 26：production multi-segment/late-join scheduler

- **状态**：`PENDING`；**难度**：★★★★★；**重要性**：★★★★☆。
- **目标**：把当前≤8 MiB 单 Segment reference path 扩展为最终项目需要的任意文件、late join 和长期 Carousel。
- **实施要点**：ActiveSegmentWindow；O(active segments) memory；descriptor/control repetition；systematic/repair scheduling；stable segment bytes/profile；resume/late join；Encoder 无限广播。
- **注意事项**：不改变 Outer identity/conflict；source mutation 中止 Session；同 Segment 的 Wirehair descriptor/encoded bytes 跨 cycles 固定；不能让 sender 假完成。
- **验收证据**：100 MiB/1 GiB sparse/incompressible/compressed、late join、restart/resume、duplicate/conflict/resource/fuzz、long soak。
- **完成出口**：Receiver 从任意 Carousel 位置加入仍可完成，内存与活动窗口而非文件总大小成正比，最终 digest/publish 正确。

### 11.29 Step 27：Certified profile Gate 与最终报告

- **状态**：`PENDING`；**难度**：★★★★★；**重要性**：★★★★★。
- **目标**：用 sealed、可重放、跨硬件和真实双机证据决定是否把某一 LF profile 标记为 Certified。
- **实施要点**：汇总 simulator、Golden、CPU/GPU、WGC/DXGI、field matrix、QoS、soak、LocalDesktop regression、package/SBOM；独立 Critical/High review；生成最终 Reality/Gate report。
- **注意事项**：SmokePass 不等于 Certified；任何 digest failure、false accept、accepted Transport inconsistency、unbounded queue 或 unexplained >10% LocalDesktop regression 都 BLOCK。
- **验收证据**：从 clean tested commit/package 复现；所有 artifact hashes/links 有效；review 无未关闭 Critical/High；tag 只在用户明确要求时创建。
- **完成出口**：正式 profile contract、适用 scale/FPS/chroma/mode/hardware 范围和不支持边界全部明确，最终文件恢复可由外部 SHA-256 独立验证。

## 12. 本轮无界面验证命令

```powershell
cmake --build build-gui-qt5 --config Release --target `
  PBRemoteVisualTests PBApplicationTests PixelBridgeEncoder --parallel 8

ctest --test-dir build-gui-qt5 -C Release --output-on-failure `
  -R "^(PBApplicationTests|PBLocalDesktopBootstrapTests|PBRemoteVisualTests)$"
```

提交 `9b8e8063a56251040cddb6b01e38c4343779571f` 前只运行不会创建/移动 native 窗口的 build、CPU/reference tests 和截图文件分析；没有启动 Encoder/Decoder GUI，没有执行 capture/selector/native display Gate，未触碰左侧屏幕内容。验证覆盖 Qt5 相关 7/7 CTest、MSVC ASan 2/2、Qt6 相关 3/3、Python analyzer 3/3，以及 32/32 独立 Golden fixture 重生成。

机器可读验证摘要保存在 `build-p1_5-evidence/remote-low-fps/validation-summary.json`。该文件是提交前工作树快照，因此其中的 `workingTreeUncommitted=true` 是历史采集状态；其实现内容已经由上述 commit/tree 固化，但 `certified=false`、未运行 native/capture field Gate 和不能替代最终 sealed evidence 的边界保持不变。

## 13. Step 06 production-truth hardening 验证补记

提交 `1936c020cb2c017b9ce3064267f497d15f94d66d`（tree `a0f0fb2717ee2712ecc76bc52f037076c7ee2219`）把离线 corpus 的证据终点从“diagnostic codeword 看起来正确”延伸到 production Transport admission、现有 `ReceiverIngress`、DirectRepeat Outer、segment digest 与 WholeFileDigest disposition，同时明确分离两种事实：

- `DiagnosticTruth` 可以使用 deterministic sender fixture 计算 coded-bit BER 和危险候选，但不能冒充 production admission；
- `Transport` 模式只按 Bootstrap SessionTag、padding、FEC、CRC 与 Transport identity 接受，不把 `FrameSequence` 错误映射为 `SegmentOrdinal`；
- 只有 production 已接受的 block 才进入 Receiver/Outer；之后再用固定 sender fixture 做 post-admission mismatch scoring；
- actual codec corpus 未完整恢复 3-segment fixture 时 WholeFileDigest 必须保持 `NotReady`，不能写成 PASS、warning 或 publish。

本次 fresh headless 验证为 Release CTest `143/143`、MSVC ASan/RelWithDebInfo CTest `278/278`、RemoteVisual evidence Python `16/16`、`PBRemoteVisualReport` Python `20/20`、LocalDesktop Golden `32 files PASS`、DesktopLevels Golden `100 files / 32 frames PASS`。两次 Step 06 create-only rebuild 各 34 个文件逐字节一致。机器可读摘要保存在：

```text
build-p1_5-evidence/20260831-step06-production-truth-v2-validation-1936c02.json
```

该摘要为 1905 bytes，SHA-256=`376f0a93563ede824912b213e02c52ebe90d5e33b4918ad52a23dcd08c4bce80`。本轮没有启动 Encoder/Decoder GUI、capture、ROI selector 或 display API，没有读取或保存左屏像素，也没有输入自动化。真实 Direct/Shape/LF4 receiver-only Replay 仍缺失，因此 Step 02 继续是明确 blocker；这里的离线证据不能转写为 `RemoteVisualSmokePass`。
## 14. Step 01-06 独立复审第二轮补记

本节只追加，不改写 11 到 13 节的任何历史结论或旧数字。它记录在 HEAD `6669cdf26505ec64fbeabe3b02e15c5ca5b566ba` 的工作树上对 Step 01-06 的第二轮独立复审：修复了哪些缺陷、给自动复核增加了什么、本轮真正跑了什么、以及仍然没有证据的边界。当前改动全部处于未提交、未暂存状态。

### 14.1 计数口径

`ctest` 省略 `-C <Config>` 时不会执行以 `CONFIGURATIONS Release` 注册的用例（例如 `tests/Phase0Gate/CMakeLists.txt` 中的 `PBPhase0GateLarge`），因此计数必须与构建目录和配置一起记录，否则 143 会被误读成 149 的回退：

| 构建目录 | 配置与过滤 | 注册 | 实跑 | 结果 |
| --- | --- | --- | --- | --- |
| `build-p1_5-evidence/20260831-step06-headless-release-final` | `-C Release -E "Native|GuiSmoke"`（13 节记录） | 143 | 143 | 143 通过 |
| `build-desktop-levels-release` | `-C Release -E "Native|GuiSmoke"` | 165 | 149 | 149 通过 |
| `build-desktop-levels-asan` | `-C RelWithDebInfo -E "Native|GuiSmoke"` | 300 | 284 | 284 通过 |

差额全部来自新增用例；两份日志的 `Skipped` 与 `Disabled` 计数均为 0，即全部真实执行。native 与 GuiSmoke 共 16 项按既定边界继续排除，本轮没有创建任何窗口，也没有读取左屏。

### 14.2 本轮修复（以当前 diff 为准）

1. `apps/common/remote_visual_replay_recorder.cpp`：槽位容器 `slots` 更名为 `captureSlots`。cppcheck 对 `apps/*` 使用 `-Dslots=` 等 shim 代替 moc，`state.slots.resize(n)` 会被预处理成 `state..resize(n)`，于是该翻译单元以语法错误退出静态审查，静态覆盖率静默归零，而 `--error-exitcode=2` 只会显示一条无关的语法错误。
2. `apps/common/evidence_journal.cpp`、`libs/PBRealCaptureReplay/src/replay_v2.cpp`：三个持有裸 `HANDLE` 的 `Implementation` 显式删除拷贝与移动，消除默认拷贝造成的重复 `CloseHandle`。
3. `apps/PixelBridgeEncoder/encoder_runtime_cli.cpp`、`apps/PixelBridgeDecoder/decoder_runtime_cli.cpp`：终局不再采用 `AppendTerminal()` 的返回值，统一以 `Finish()` 返回的权威快照为准（该快照已经带上 terminal append 记录的失效或截断），并删除已死的 `journalCreateAttempted` 二次赋值。
4. `apps/common/local_desktop_runtime.cpp`：删除被上游 1 秒采样门限蕴含的 `monotonicMilliseconds <= lastWallMilliseconds_` 死条件，并注明 interval 非零的不变量；语义不变，但原条件会让读者误以为除数可能为 0。
5. `libs/PBModulation/src/remote_visual_low_fps.cpp`：远角落界改用 `kLocalDesktopCanvasWidth`/`kLocalDesktopCanvasHeight` 常量替代 1920/1080 字面量；`ResolveRemoteVisualLowFpsPhysicalMetrics` 失败时不再提前 `return`，而是落到统一的 work/pixel 记账，使失败路径同样产出 `dataWorkUnits` 与 `pixelError`，同时 `hardBits`、`softMetrics`、`dataBytes` 与 `histogramValid` 保持不变。
6. `tools/PBRemoteVisualChannelMatrix/channel_matrix_core.cpp`：`FindDifferingDataBlockTransform` 同时校验被比较的 tile 矩形与写出的 blockSize 矩形，并改用 64 位加法；原代码只检查 blockSize，且 `region.x + blockSize` 在 `uint32` 下可回绕。
7. `channel_matrix_core.cpp` 与 `tools/PBRemoteVisualCodecProbe/codec_probe_core.cpp`：验收阈值与 `reserve()` 里的裸字面量 4 改为 `pbmodulation::kRemoteVisualLowFpsCodewords`，让 codeword 数量只有一个真值来源。
8. 新增回归测试：tile 与 block 双矩形在 8/16/64 下的画布内界（`tests/tools/test_remote_visual_channel_matrix.cpp`）；输出 span 为 null 或过小时必须擦除、且同一 workspace 之后仍能正常解码（`tests/PBModulation/test_remote_visual_low_fps.cpp`）；telemetry 快照的取值语义用 `static_assert` 加运行期不变性钉住（`tests/PBApplication/test_application_model.cpp`）；sealer 的 symlink、NTFS junction、非有限 JSON、枚举与成员校验、Replay 扩展名与截断（`tools/PBRemoteVisualEvidence/test_seal_remote_visual_dataset.py`，16 到 20 个用例）。
9. `tests/DesktopLevelsGate/cppcheck_review.json` 重新钉值：69 条 pin 对当前文件的规范化 SHA-256 逐条复算，0 条失配、0 条缺失。

### 14.3 自动复核增量

- Step 01 的 tag 身份从人工执行 git 改为 `cmake/PBVerifyPhase1GateTagIdentity.cmake`，注册为 CTest `PBPhase1GateTagIdentity`。冻结常量内置于脚本，调用方传入 `PB_PHASE1_GATE_TAG_NAME`、`PB_PHASE1_GATE_TAG_OBJECT` 或 `PB_PHASE1_GATE_COMMIT` 会直接 `FATAL_ERROR`，否则任何人都能拿 tag 当前指向的值来自证通过。脚本先校验 pin 自身是 40 位小写 hex，再断言 work-tree 顶层等于请求根（git 会向上遍历，否则会去校验别的仓库）、`cat-file -t` 必须是 `tag`（拒绝同名轻量 tag）、`rev-parse` 结果与 `cat-file tag` 头部 payload 的 object/type/tag/tagger 三重一致，最后断言 `build/` 与 `build-p1_5-evidence/` 未被 git 跟踪。
- `New-PBRemoteVisualPortablePackage.ps1` 在写 manifest 之前必须先通过该脚本。本轮首次端到端验证：真实仓库退出码 0，manifest 记录 `fde56c4c4e7124e8ffe29a0dcb619f8236781ebb` 与 `80699813b595bcf6db64047b50d31056872e33e1`，source set 为 841 个文件；另建一个把 `phase1-gate-pass` 指向别的提交的 scratch 仓库，打包脚本退出码 1 并报 `Phase-1 Gate tag identity check failed (exit 1)`，输出目录里没有任何 zip 或 seal，create-only 语义未被破坏。
- `tests/DesktopLevelsGate/VerifyQtKeywordCollisions.ps1`（CTest `PBDesktopLevelsQtKeywordGuard`）守住上面第 1 条那类静默失效。本轮把它的作用域从 `apps/*` 扩到 apps 翻译单元实际会包含的公共头闭包 `libs/*/include/**`：新增必填参数 `-HeaderClosureRoot`、一个只扫 include 目录的路径过滤器，以及一个证明该过滤器既不是空转也不是全量扫描的 scratch 自测。当前结果为 `apps-files=36 header-files=63 violations=0`。变异验证：在 `libs/PBTelemetry/include/pbtelemetry/` 放一个含 `std::vector<int> slots;` 的头文件，守卫退出码 1 并指名该文件与行号，探针随后删除。
- `tests/DesktopLevelsGate/InvokeFinalGate.ps1` 的 cppcheck 清单补入 Step 05/06 的 `PBRemoteVisualSimulator` 与 7 个证据工具；23 个条目的 `.vcxproj` 在对应构建目录中逐一存在（缺失会让 cppcheck 以非 0/2 退出而使 Gate 失败，因此这一步必须在合入前逐个确认）。

### 14.4 本轮实测（全部为本轮重新执行）

| 验证 | 命令或套件 | 结果 |
| --- | --- | --- |
| Release 全量 | `ctest --test-dir build-desktop-levels-release -C Release -E "Native|GuiSmoke"` | 149/149 通过，551.66 s |
| MSVC ASan 全量 | `ctest --test-dir build-desktop-levels-asan -C RelWithDebInfo -E "Native|GuiSmoke"` | 284/284 通过，986.96 s |
| RemoteVisual evidence Python | `test_analyze_remote_capture.py test_seal_remote_visual_dataset.py test_build_codec_corpus.py test_build_step06_corpus.py` | 20/20 OK |
| RemoteVisual report Python | `test_pb_remote_visual_report.py` | 20/20 OK |
| cppcheck pin 一致性 | 69 条 pin 规范化 SHA-256 逐条复算 | 0 失配、0 缺失 |
| 打包 tag 前置 | 真实仓库正例与移动 tag 反例 | 正例 0 并记录冻结身份；反例 1 且零产出 |
| Qt 关键字守卫 | `apps` 加 `libs/*/include` 全量扫描与公共头变异反例 | 违规 0；公共头反例退出 1 |
| Gate 静态审查清单 | 23 个条目的 `.vcxproj` 存在性 | 全部存在 |

### 14.5 一个必须记住的流程事实

CTest 脚本用例的参数只有在对应构建目录重新 configure 之后才会更新。本轮就撞上过一次：`tests/DesktopLevelsGate/CMakeLists.txt` 增加 `-HeaderClosureRoot` 之后，尚未重新 configure 的 ASan 树仍然带着旧命令行去调用该脚本。因此任何给 CTest 或 Gate 脚本增加必填参数的改动，都必须对每一个在用的构建目录重新 configure，并重新跑一次全量，否则得到的不是新定义下的结果。

### 14.6 未执行与不执行的验证

- Step 02 的真实 receiver-only Replay 仍然缺失：Direct/Shape/LF4 三类 ROI frame sequence 无法在沙箱内产生，Step 02 继续为 `PARTIAL`，Step 15 与 Step 20 的硬出口不变。
- 双机 field、`UniqueVisualFPS`、真实 provider 矩阵、6 小时 bounded soak、Certified profile Gate 全部未执行。本轮全部结论仍停留在 CPU/reference 与离线 corpus 级别，不能转写为 `RemoteVisualSmokePass`。
- 封存 corpus 的逐字节复现（`build_step06_corpus.py` 的 create-only 重建与 `SHA256SUMS.txt` 交叉核对）本轮未重复执行，原因是它与 ASan 全量回归争用 CPU 与磁盘，而其生产端与消费端都在本轮 CTest 覆盖范围内。残余风险：若某台机器上的 ffmpeg 或插件行为漂移，只有重新执行 create-only 重建才会暴露。
- cppcheck 只重跑了 pin 的一致性，没有重跑全部 23 个工程的发现集合。pin 是按文件内容哈希绑定的，任何源码改动都会使旧 pin 失配而失败，因此残余风险限于「cppcheck 自身版本或默认检查集变化」，而版本变更另有 `Cppcheck` 字段守卫。
- Python 证据套件仍然没有接入 CMake 与 CTest。项目现行约定是由文档指定 pinned interpreter 手工执行（`tests/PBApplication/InvokeEncoderGuiLocalDesktopSmoke.ps1` 就以 `D:\Python3.12.9\python.exe` 作为默认值），而把它们变成 CTest 需要给所有构建树引入 Python 3.12 硬依赖（本机 `python` 是 3.6，套件用到的 `Path.is_junction` 需要 3.12）。这属于影响构建架构的决定，本轮只如实记录风险，不擅自更改。残余风险：sealer 的路径穿越、symlink、受保护像素等守卫一旦回归，只有手工运行才会发现。

### 14.7 停止点与续办清单

- 审查轮次：R1-R11 已完成。R6、R8、R9 为无代码修改轮；R11 修复 1 个 Medium（cppcheck 关键字守卫只扫 `apps/*`，未覆盖同一预处理单元内的 `libs/*/include/**`）后按规则 cleanRounds 归零，用户在此处指示停止，因此"连续 5 轮无新问题"的收敛条件未达成。
- 最高优先级的未覆盖缺口：Step 05/06 主体模块的逐行审计尚未执行（此前只审过 diff 与调用点），包括 `libs/PBRemoteVisualSimulator/`（resize/block-replacement/blur/gain-bias-gamma/4:2:0 proxy/crop/solid-alpha overlay/reference blend 与 `PixelBridge.RemoteVisualChannelManifest.1/2` schema）、`tools/PBRemoteVisualReceiverEvidence*`、`tools/PBRemoteVisualTemporalCorpus*`。检查重点：长度/offset/count 溢出、bounds、bounded allocation 与资源配额、未初始化 padding、第三方 API（zstd/wirehair/blake3/ffmpeg）使用方式、manifest 顺序与 seed 可复现、erasure 不得被记为 Verified。
- 交付状态：21 个修改文件与 2 个新增脚本（`cmake/PBVerifyPhase1GateTagIdentity.cmake`、`tests/DesktopLevelsGate/VerifyQtKeywordCollisions.ps1`）保持未提交、未暂存，提交时机由用户决定；`docs/PHASE1_GATE_REPORT.md` 属用户文件，本轮未做任何操作。
- 本次工作树的最终验证基线：Release 全量 149/149、ASan 全量 284/284、定向 `RemoteVisual|QtKeyword|Phase1GateTagIdentity` 11/11、Python 证据套件 20/20 与报告套件 20/20。
- 清理：删除 `tests/PBModulation/__pycache__`；源码树无其它临时或探针产物；可复用校验脚本与日志保留在 `build-desktop-levels-release/_review/`（gitignored，可随构建目录一并删除）。

## 15. Step 02 真实 receiver-only Replay 完成证据

本节是 2026-09-01 在第 14 节历史快照之后取得的新证据。它更新当前 Step 02 状态，但不改写第 14.6 节在当时“真实 Replay 仍缺失”的事实。证据根为 gitignored 的 `build-p1_5-evidence/step02-rdp-20260901/`；机器可读完成摘要 `step02-completion-summary-a.json` 与独立重建的 `-b.json` 均为 8718 bytes，逐字节一致，SHA-256=`dd721e599800737e4fb9584d53d678ee4fae3871392231412cbf85a929e9c987`，payload BLAKE3=`d42c7b0455fcd4560826093862e4745150a514bccd7e181ba6ee33b6f718b567`。

### 15.1 环境、屏幕与发送边界

- Computer B 原生面板为 2560×1600、原生缩放 150%；当前 RDP session 实际可见 canvas 为 2560×1440。Computer A 的 RDP 主窗口完整位于右侧 `\\.\DISPLAY2` 的 `[2560,0,5120,1440]`，受保护左屏为 `\\.\DISPLAY1`。
- 实验 ROI 固定为 Computer A 物理像素 `[2880,180,4800,1260]`，即 RDP session 内 `[320,180,2240,1260]`；PMv2 捕获报告为 1920×1080、96 DPI、scale 1.0。只保存右屏截图与 selected-ROI Replay；dataset seal 明确记录 `containsProtectedMonitorPixels=false`。
- `PBRemoteVisualEvidencePresenter` 只构造一个确定性的有效 Bootstrap/Transport raster，并在成功 Present 后报告精确 window geometry。三种 profile 都重复同一 immutable raster，Present cadence 不高于 5 Hz；重复 Present 不被解释为新数据或 `UniqueVisualFPS`。
- Computer B 便携包 `build-p1_5-evidence/PixelBridge-Step02-ComputerB-Presenter.zip` 为 350280 bytes，SHA-256=`1ba9ce9e8a142e85c115204a2f2058a01bb8ddc504b2186656a279f4fabeb6a0`。Direct/Shape/LF4 的 descriptor hash 分别为 `2920e56fcb5967a144a46cd8cc441386a361b4307f2e423a5d000325a59c214c`、`a907e99f2c50aa56b148e6fd23d1809441d790561f42c5e368c4b4dcf02d1266`、`28b09b66520b87f9cd9407b516530ad1225f84d390cc2b94d18d6e1b8d5e9bc4`。
- RDP 的具体画质模式与 chroma mode 无法从当前 UI 可靠确认，metadata 保持 `Unknown`；代码与判决没有根据 provider 名称选择阈值。

### 15.2 接收、封口与离线双跑

`PixelBridgeDecoder --headless-receive --diagnostic-capture-only` 使用显式 WGC、右屏 monitor-safety preflight/revalidation、固定 8 帧和 128 MiB Replay 上限。`--replay-evidence-profile direct|shape|lf4` 只给 capture-only Replay descriptor 标记真实 raster identity，不选择 LF4 产品 demodulator，也不运行 Bootstrap/demod/Receiver/publish。三轮 report 均为 WGC 实际后端、Replay `evidenceValid=true`、`finalized=true`、written frames=8、dropped frames=0、monitor safety `PASS`。

| Raster | RunId | Replay bytes | Replay SHA-256 | Reader / Bootstrap / modulation / Transport | accepted blocks | 双跑 inspection SHA-256 |
| --- | --- | ---: | --- | --- | ---: | --- |
| Direct | `0ce5e41d6099d4968bc726d184750887` | 66361133 | `6d391140ef32081424e371d812eccbad38c298bd1af140e10058cf242e8a2da4` | complete / 8 / 8 / 8 | 336 | `09ef7c7dac6c7b9b1f56be1c90a70a8e220b6101745921beced66623f393b929` |
| Shape | `82db9eb2147d938958022e662513c780` | 66361068 | `7404dbbf61a0b05a48e03fc00a7a309834f044ab45312e73f691f4d1df89a17c` | complete / 8 / 8 / 8 | 256 | `e4a5ef679851da0fa8a80b87d15efbabd36342348cf99bcfda503b0dea994586` |
| LF4 | `dd6d2bc7082afe306686284b36a463ab` | 66361066 | `fe534a2b3778de5ee565b66b017149ffd6ddcc4321d507a54abad914d41b6500` | complete / 8 / 8 / 8 | 32 | `5ccebecd857c9b55ec2cc1a09c299e3a1fe579daeec529b4615638b4bbeeb2a9` |

每个 inspection hash 同时适用于 run 1 与 run 2，因为对应文件逐字节一致；这比只比较汇总计数更强，完整覆盖每帧 Bootstrap canonical hash、连续 geometry、soft/hard metric 统计、FEC iterations、CRC/identity disposition 和 accepted-block digest。三类的 FEC/CRC/identity failures 都是 0；LF4 stale regions 为 0。dataset input 中只列三份相对路径 Replay，`step02-dataset-index-a.json` 与 `-b.json` 逐字节一致，SHA-256=`c3e81b56163f150be9a1b2cfa133c40f46b494390902c5a588e16d5658795b95`。

首次 Direct 探针在零帧时被 `InvalidConfiguration` 正确拒绝；失败 report 被保留，未复用旧输出路径。根因是 capture 配置需要 4 个 ROI texture，而 diagnostic readback 只配置了 3 个 staging texture。修复后 staging 数量从同一个 `CaptureConfig::roiTextureCount` 派生，避免两个常量再次漂移；随后三轮真实捕获均成功封口。

### 15.3 真实性边界

- Receiver-only Replay 没有独立 sender expected-byte oracle，因此 `senderTruthAvailable=false`、`falseAcceptedCodewords=null`/`UnavailableReceiverOnly` 是唯一诚实口径；不能把 CRC-valid block 数量转换为“0 false accept”统计。
- Inspector 的 acceptance authority 是 `QC-LDPC+padding+TransportCRC+SessionIdentity`；它不创建 Outer/Receiver 会话，不做 WholeFileDigest，也不发布文件，`finalFileDisposition=NotEvaluated`。
- 本节关闭 Step 02 的真实数据入口与重复分类出口，但不关闭 Step 07 holdout false-confidence Gate、Step 15 LF4 live/offline production consistency 或 Step 20 真实双机文件传输。它也不产生 `RemoteVisualSmokePass`、Certified Profile、真实 provider matrix、带宽/时延、`UniqueVisualFPS` 或 6 小时 soak 结论。

### 15.4 实现与验证闭环

- 新增 `PBRemoteVisualReplayInspector`：以 `ReplayV2Reader` 为唯一录制输入边界，覆盖 Replay v2 完整性、Direct/Shape/LF4 descriptor 路由、Bootstrap、连续几何、modulation metric、QC-LDPC、canonical padding、Transport CRC 和 Session identity；以 create-only 方式写出确定性 JSON。不支持的 profile、截断/冲突 Replay 或不完整 Transport 都 fail closed，且不修改已存在输出。
- 新增 `PBRemoteVisualEvidencePresenter`：它是 Step 02 的证据发送器，不是第二套产品 runtime。`describe` 输出确定性 raster 描述，`present` 仅在 PMv2、单 monitor、1920×1080 实体像素区域内 Present，且 READY 只在首次成功 Present 后输出。
- `--replay-evidence-profile direct|shape|lf4` 只允许与 `--diagnostic-capture-only` 共用，只标记 Replay descriptor 中已知的发送 raster；它不会把 LF4 注册为产品 GUI/profile，也不会让在线 Decoder 跳过任何权威校验。
- WGC 启动修复将 diagnostic staging texture 数量从同一个 `CaptureConfig::roiTextureCount` 派生；修复前的零帧 `InvalidConfiguration` 记录保留，修复后才重新使用新 RunId/新输出路径采集三类证据。

本轮在证据采集后重新 configure/build 两个构建树，再执行新定义下的全量无界面回归：

| 验证 | 命令或套件 | 结果 |
| --- | --- | --- |
| Release 构建 | `cmake --build build-desktop-levels-release --config Release --target ALL_BUILD` | 通过 |
| Release 无界面全量 | `ctest --test-dir build-desktop-levels-release -C Release -E "Native|GuiSmoke" --output-on-failure -j 2` | 154/154 通过，253.22 s |
| MSVC ASan/RelWithDebInfo 构建 | `cmake --build build-desktop-levels-asan --config RelWithDebInfo --target ALL_BUILD` | 通过 |
| MSVC ASan 无界面全量 | `ctest --test-dir build-desktop-levels-asan -C RelWithDebInfo -E "Native|GuiSmoke" --output-on-failure -j 2` | 289/289 通过，558.40 s |
| RemoteVisual evidence Python | `test_analyze_remote_capture.py test_seal_remote_visual_dataset.py test_build_codec_corpus.py test_build_step06_corpus.py` | 20/20 OK |
| RemoteVisual report Python | `test_pb_remote_visual_report.py` | 20/20 OK |
| Step 02 工具 cppcheck | 与 Final Gate 相同的 cppcheck 2.21.0 exhaustive/inconclusive/warning/style/performance/portability 参数 | Inspector Core、Inspector CLI、Presenter 3/3 零发现，且已纳入 Gate 的 26 项目清单 |
| 真实 Replay 重复分类 | Direct、Shape、LF4 各用最终 Release Inspector 独立执行两次 | 每类两份 JSON 逐字节一致 |
| dataset seal 重建 | 相同三份 Replay 独立 create-only 封口两次 | 两份 index 逐字节一致 |

提交前最后一次定向重建后，Release `PBRemoteVisualReplayInspector.exe` SHA-256=`a59bf320fc0bc6fbe7b53d6b4b466943494ac1e2808b8701d7a2cfbb4b2a7e61`。该最终可执行文件对三份真实 Replay 再各执行两次 create-only 分类，输出 SHA-256 仍与第 15.2 节分别记录的 Direct/Shape/LF4 hash 完全一致。

以上 CTest 明确排除会创建窗口或读取屏幕的 `Native|GuiSmoke` 用例；它们不是对第 15.1/15.2 节 RDP 实机证据的替代，而是对相同读取器、解调、Transport authority、错误路径和封口工具的回归闭环。

## 16. Step 07 Soft metric 标定与 false-confidence Gate 完成证据

本节是第 15 节真实 LF4 Replay 之上的新闭环。完整 schema、候选矩阵、复现命令、结果表和限制见 `REMOTE_VISUAL_STEP07_CALIBRATION.md`。

### 16.1 实现与不可越过的 truth boundary

- 新增 `PBRemoteVisualMetricCalibrationCore`、`PBRemoteVisualMetricCalibration` 与测试。pipeline 直接复用 production LF4 解调、QC-LDPC、canonical padding、Transport CRC 和 Session identity；sender fixture 只在 modulation/production admission 之后用于 exact byte truth scoring，不进入解调或 FEC。
- 输入固定为 25 个互异 dataset/run group、44 帧、6 个 policy 共 264 个 observation：Train/Validation/Holdout 各 8 group/12 帧，External 为 Step 02 的 1 个真实 RDP group/8 帧。完整 dataset/run 是最小 split unit，同一 group 跨 split、policy frame coverage 不一致或 truth 不完整都会 fail closed。
- Step 02 Replay 仍保持 receiver-only 原始语义；Step 07 通过其 sealed dataset/completion/inspection/Replay identity 和独立 sealed Presenter raster 重新建立 External sender oracle。C++ 先用 `ReplayV2Reader` 验证 capture-only Replay，再重建 Presenter raster 并核对 BLAKE3；只有 production 已接受 block 才比较 truth。
- truth boundary 明确记录 `senderTruthEntersDemodulation=false`、`senderTruthEntersFec=false`、`postAdmissionTruthScoring=true`、`wholeFileOutputEvaluated=false`、accepted/false-accepted output objects 均为 0。没有把 Transport 结果改写成 Receiver/Outer/WholeFileDigest/final publish 结论。
- 新增 `build_step07_calibration.py`，独立验证 raw C++ payload seal、schema/member/cardinality、24 项 candidate matrix、parameter manifest、ROC/reliability/confidence curves、selection/holdout 隔离、输入 digest/稳定性/resource policy、路径根约束与 create-only publication；路径中的 symlink 和 NTFS junction 逐级拒绝。

### 16.2 选择规则与结果

固定 candidate matrix 为 6 个 admission policy × Raw/GlobalScale/PerSignalScale/PiecewiseLookup 4 个 model。margin 0.04 与 residual 0.90 虽保留在报告中，但显式标记为放宽 default admission、没有 selection 资格；PerSignalScale 不能对未知 provider signal binding 部署。最终只用 Validation 选出 `lf4-default/PiecewiseLookup`，随后才打开 baseline/selected 的 Holdout 与 External：

| split | hard/Transport 结果 | baseline → selected 的 soft-calibration 结果 |
| --- | --- | --- |
| Validation | 12 帧、7 erasure、5 verified、20 accepted Transport、0 false accepted；BER 不变 | iterations 50→49；log loss 0.4180466693827719→0.05354665741491579；ECE 0.27831021588882454→0.020988875567698764 |
| Holdout | 12 帧、7 erasure、3 verified、FER 0.75、BER 0.11609567901234567、12 accepted Transport、8 FEC failure、0 false accepted；全部不变 | log loss 0.43106895732822903→0.1223487155739084；ECE 0.2272827570759902→0.028538512183229617 |
| External real RDP | 8/8 verified、BER 0、32 accepted Transport、0 FEC/CRC/identity/false accepted；全部不变 | log loss 0.31321503283560503→0.0003354882554752294；ECE 0.2689070269798241→0.00033543198558441223 |

结论只限于 soft confidence 标定改善；本步骤没有提高 Holdout/External 的 hard BER、FER 或恢复帧数。selected policy 仍是 LF4 default，`productionDefaultsChanged=false`、`acceptanceAuthorityChanged=false`、`admissionRelaxed=false`，部署 disposition 为 `CandidateOnlyRequiresStep08Freeze`。

### 16.3 可重复 seal

最终 Gate/validator 收紧后的两次 create-only rebuild `20260901-step07-calibration-final-g`、`-h` 的三个输出文件分别逐字节一致。两次使用 Step 02 的两套独立 seal，并都包含 symlink/junction、完整 curve/model/inventory/denominator、Validation-winner 独立复算和 FEC/CRC/identity/iteration/Brier/ECE 非回退守卫；较早的 `sealed-a/-b/-c` 与 `final-d/-e/-f` 是收紧过程中的开发证据，不作为最终 validator-complete seal：

| artifact | bytes | SHA-256 |
| --- | ---: | --- |
| `metric-calibration.json` | 375118 | `c17f17844c0b44cf029bb31ecbfc127139fa886588c941c2a3a0f34bf541acf8` |
| `step07-calibration-index.json` | 27877 | `d972a5fd30317c186ce0fb59279cd6ab9c7b86bfb748c05bc7e4ca9df444443e` |
| `SHA256SUMS.txt` | 186 | `2fbca82ab5f0bae957582b681091b3d19ab7215ee8e2c8160575eaafb41e8c9b` |

report 的 outer/core/input payload BLAKE3 分别为 `2fdf2c5dbfbc52d7a73d878243f96ae60b095b6f4456bff73f226a085288287f`、`7a8ff14e09dc5f0b4417349f9ab3053f0e0dac05312be94806e55d40324f619f`、`06eeacf328e8c8855803e346315b67ca366f1aefd2f25c2778e786596b31d139`。Gate 总结为 split isolation/holdout/external improvement 全部 true，selection 未使用 Holdout，false accepted Transport/control/output 全部 0。

### 16.4 提交前验证

| 验证 | 命令或套件 | 结果 |
| --- | --- | --- |
| Release 构建 | `cmake --build build-desktop-levels-release --config Release --target ALL_BUILD` | 通过 |
| Release 定向 CTest | `PBRemoteVisualMetricCalibrationTests` | 1/1 通过 |
| Release 无界面全量 | `ctest --test-dir build-desktop-levels-release -C Release -E "Native|GuiSmoke" --output-on-failure -j 2` | 155/155 通过，277.87 s |
| MSVC ASan/RelWithDebInfo 构建 | `cmake --build build-desktop-levels-asan --config RelWithDebInfo --target ALL_BUILD` | 通过 |
| MSVC ASan 定向 CTest | `PBRemoteVisualMetricCalibrationTests` | 1/1 通过，0.81 s |
| MSVC ASan 无界面全量 | `ctest --test-dir build-desktop-levels-asan -C RelWithDebInfo -E "Native|GuiSmoke" --output-on-failure -j 2` | 290/290 通过，438.91 s |
| RemoteVisual evidence Python | `test_analyze_remote_capture.py test_seal_remote_visual_dataset.py test_build_codec_corpus.py test_build_step06_corpus.py test_build_step07_calibration.py` | 27/27 OK |
| RemoteVisual report Python | `test_pb_remote_visual_report.py` | 20/20 OK |
| Step 07 工具 cppcheck | 与 Final Gate 相同的 cppcheck 2.21.0 exhaustive/inconclusive/warning/style/performance/portability 参数 | Core、CLI 2/2 零发现；两份 XML SHA-256 均为 `ed984569894251e9d98099b15e48f22767aa126e27ba9480999ee3e4653abee4`，且已纳入 Gate 的 28 项目清单 |
| C++ create-only 覆盖拒绝 | 用最终 Release CLI 将 `--output` 指向 `final-g/metric-calibration.json` | 按预期退出 1；文件 SHA-256 前后均为 `c17f17844c0b44cf029bb31ecbfc127139fa886588c941c2a3a0f34bf541acf8`；无 `.partial` 遗留 |
| 最终 seal 重建 | Step 02 两套独立 seal 分别构建 `final-g`/`final-h` | 三个对应文件分别逐字节一致 |

以上 CTest 明确排除会创建窗口或读取屏幕的 `Native|GuiSmoke` 用例；External 结论使用第 15 节已封存的真实 RDP LF4 Replay，而不是把无界面回归冒充新的实机采集。cppcheck 清单中的 28 个 `.vcxproj` 在对应 Release/ASan 构建树中均存在。

### 16.5 后续边界

Step 07 关闭的是 CPU/reference calibration candidate Gate，不是 production deployment。Step 08 必须把 selected metric quantization/model 与现有 LF4 raster/mapping/codebook/Bootstrap/accepted Transport 结果冻结成可独立 `--check` 的 Golden；Step 09/10 以后才实现 D3D11 production raster/demod。当前只有一组 Windows Remote Desktop External，画质/chroma mode 为 Unknown；没有 provider matrix、VerifiedEncodedGoodput、`UniqueVisualFPS`、6 小时 soak、双机文件 WholeFileDigest/final publish 或 Certified Profile 结论。
