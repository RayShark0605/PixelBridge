# PixelBridge RemoteVisual 低刷新率高密度传输技术路线

状态：**2026-09-02 Step 01..19 已全部完成，Step 20 保留真实双机文件级人工硬门禁。Step 02 已由 Windows Remote Desktop 上 Direct/Shape/LF4 各一组真实 receiver-only Replay 关闭；Step 07 选择的 `lf4-default/PiecewiseLookup` 已在 Step 08 与 LF4 raster/mapping/codebook/Bootstrap/4 个 accepted Transport 一起冻结为独立可重建 Golden；Step 09 已把 LF4 四-codeword raster 接入隐藏的 production Encoder candidate，并以 immutable D3D11 source、1..5 Hz 完整替换和显示许可驱动的重复 Present 关闭发送端 Gate。Step 09 后置 120 秒动态 RDP pilot 又证明 production EncoderRuntime raster 可经真实 AweSun/WGC 链路进入有界 Replay，并由 receiver-only Inspector 两次确定性解出 1428 个 Control copy 与 1496 个 Transport block，FEC/CRC/identity failure 全为 0。Step 10 增加独立 experimental D3D11 LF4 direct-texture demod后，Step 11 又在 WARP、AMD Radeon与当前枚举出的三个独立 NVIDIA RTX 5090 D LUID上关闭 accepted-Transport parity matrix；Step 12 把同一路径接入 `CaptureDemodulator`与WGC/DXGI共用的`CaptureNormalize` lifetime。Step 13 现已在GPU/Receiver之间关闭 `(CaptureEpoch, SessionTag, FrameSequence, canonical Bootstrap)` duplicate/reorder/gap与bounded refinement：32帧duplicate burst不增加GPU submission、FER、accepted Transport或队列HWM。Step 14 随后让temporally admitted四-codeword进入现有production Receiver/Outer；DirectRepeat与Wirehair V2均经过WholeFileDigest与安全发布恢复external byte-exact文件，raw 6 blocks中只有4个unique slots被admit。Step 15又保持Replay v2旧基础布局并在保留区增加presence-versioned production result，让6-frame LF4 receiver-only Replay在live/offline两次production Decoder中逐项一致、两次WholeFileDigest与安全发布external byte exact；Step 02真实Direct/Shape/LF4各8帧也由同一路径复现无Control descriptor时的Stopped/no-publish分类。Step 16又将冻结容量、Encoder generation、Present、capture/unique、FEC、temporal admission与最终verified goodput拆成权威分母，并让严格merger封印两端exact report后create-only生成JSON/Markdown/CSV。Step 17现已用共享profile catalog向GUI/CLI显式公开独立`remote-lf4`，以Protected/Experiment monitor双重authority、每秒topology watchdog、no-activate右屏Gate和实际WGC像素闭环关闭公开产品入口；最终闭环经过production Receiver、WholeFileDigest与safe publish恢复external byte-exact文件。Step 18又用同一Both-role便携包、完整依赖/SBOM/license清单、CSPRNG source set、共享RunId和双端environment/deployment manifest关闭可反查部署identity。Step 19随后以冻结8 MiB RAW执行WGC/DXGI × Direct/Shape同提交回归：四组WholeFileDigest/publish/external hash全部PASS，goodput相对基线分别提升10.86%、10.01%、19.46%和21.22%，完整Release CTest为200/200 PASS。Step 20期间又在Computer B单物理屏幕条件下按用户选择完成非Gate真实远控静态LF4诊断：首轮匹配sender时段内的8帧exact-ROI WGC Replay经边界舍入修复后确定性得到8/8 Bootstrap、8/8 modulation、8/8 Transport与32 accepted blocks，0 FEC/CRC/identity failure；同一不可变Replay在旧实现中稳定为8/8 Bootstrap、0/8 modulation。随后v2独立sender run实时回显READY，A端首次exact ROI捕获为7/8全链接受加1个MarkersNotFound安全擦除，相同参数bounded repeat为3/3全链接受；两轮共40 accepted blocks、0 FEC/CRC/identity failure，所有接受帧geometry均为exact (0,0,1,1)，不再依赖1 px margin。receiver-only falseAcceptedCodewords仍为Unavailable。该结果只关闭诊断像素链并修复、复验exact-canvas locator边界，不包含文件Receiver、WholeFileDigest或发布。复验后的deployment/pilot evidence contracts为2/2 PASS，最终Release build exit 0、完整CTest 201/201 PASS。Computer B单屏production safety gate仍正确阻止正式运行；provider matrix、真实双机goodput和file Gate仍待后续步骤。这不是Certified Profile，也不是Step 20双机文件传输field certification。**

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

当前实现保持严格分离的执行边界：CPU reference/simulator提供scale/freshness/FEC/Transport真值；Step 09的production Encoder-only candidate负责完整LF4 raster、immutable D3D11 source replacement和repeat Present；Step 09后置field pilot只用WGC diagnostic capture-only Replay与离线CPU Inspector验证动态远控像素链路；Step 10的D3D11 API从PB-owned texture按连续geometry生成compact metrics/freshness，并经现有FEC/Transport对齐CPU accepted blocks；Step 11让同一路径在WARP与全部当前可用AMD/NVIDIA adapter上运行统一corpus；Step 12再将它作为现有`CaptureDemodulator`的两阶段LF4分支接入WGC/DXGI共用的CaptureNormalize owner/lifetime，而没有建立另一套capture或解调实现；Step 13在该single path内完成temporal admission，Step 14仅把admitted indices接到原有`ReceiverPipeline`/`ReceiverIngress`/Outer/PBStorage，不创建LF4专用Receiver；Step 15再让Replay v2 live/offline共享这条production path，旧v2缺少detail时保持presence-aware兼容。Step 17现已让GUI与两端CLI共享同一个只读profile catalog并显式公开独立`remote-lf4`；既有`RemoteVisualResilient` enum/token/wire identity与默认选择保持不变。live Encoder/Decoder都要求Protected/Experiment monitor authority并周期性重验证，public WGC像素闭环已进入同一production Receiver/Outer/WholeFileDigest/publish路径；Step 18现已冻结Both-role便携包、source set、shared metadata、双端environment与deployment identity；Step 19又以同一tested-source包关闭冻结LocalDesktop四组合回归，保留用户左屏并发工作事实并验证最终文件、goodput、CPU/GPU与有界HWM。Step 20必须在真实Computer B/A远控像素链上执行，不能由本地闭环替代。

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

### 9.2 Step 01 当时明确未完成的项目

本小节保留初始 CPU/reference implementation checkpoint 的历史边界；当前权威状态以第 11 节步骤表和第 15～20 节完成证据为准。

- LF4 当时尚未进入 `VisualProfile` product enum/GUI；Step 09 后只新增了 enum 尾部的隐藏 Encoder-only candidate，GUI/CLI 仍未暴露且 Decoder validation 仍拒绝；
- 当时 D3D11 scaled Walsh demod shader 尚未实现；Step 10 后已有独立 experimental LF4 API，但当前 product `CaptureDemodulator`/Decoder 路径仍只使用既有 profiles，旧 1-bit strict-1:1 RemoteVisual shader未被重解释；
- 当时未运行真实远控双机、WGC/DXGI 或右侧屏幕 native field Gate；Step 02 后已有一组 Windows Remote Desktop receiver-only Replay，Step 09 后已有右屏 D3D11 sender native Gate，但仍没有 LF4 live production Decoder field Gate；
- 未证明 1/2/5 Hz 的真实 `VerifiedEncodedGoodput`；
- 当时未建立 LF4 Golden；Step 08 已冻结独立 manifest/raster/accepted Transport compatibility Gate，但仍不是 Certified Profile；
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

provider 名称和产品 UI 模式只是现场矩阵的 `NonDecodingOperatorMetadata`，用于区分外部链路条件；Decoder 不得根据这些标签选择阈值或改变任何接受语义。它只能从实际捕获的 ROI pixels、capture timing、固定 PixelBridge profile/backend 中恢复数据，最终仍由 FEC、Receiver、WholeFileDigest 和安全发布语义决定结果。

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
| 07 | Soft metric 标定、阈值选择与 false-confidence Gate | DONE | ★★★★★ | ★★★★★ | Validation-only selection、Holdout/External 改善、0 false accepted Transport/control/output |
| 08 | LF4 Golden/manifest 冻结与兼容性声明 | DONE | ★★★☆☆ | ★★★★★ | 独立 generator/check、canonical raster/hash、4 个 accepted-block manifest |
| 09 | LF4 D3D11 Encoder raster/immutable texture 接入 | DONE | ★★★★☆ | ★★★★☆ | CPU/GPU source BLAKE3、WARP/hardware immutable/repeat Gate、production carousel/dwell Gate |
| 10 | LF4 D3D11 scaled Walsh demod shader | DONE | ★★★★★ | ★★★★★ | direct PB-owned texture、261,040-byte compact readback、WARP exact/scale/blur/stale、FEC/Transport accepted-block parity |
| 11 | CPU/WARP/hardware GPU truth parity | DONE | ★★★★☆ | ★★★★★ | WARP + 4 hardware LUID、25 frame/100 block manifests、wrong-LUID/device-recreate/GPU timestamp Gate |
| 12 | CaptureNormalize、geometry、epoch 与 D3D lifetime 收口 | DONE | ★★★★★ | ★★★★★ | same-ROI staged completion、WGC/DXGI rotation、两epoch recreate、exception/second-continuation与lease/HWM归零 |
| 13 | duplicate/gap/reorder/stall 与 bounded queue 收口 | DONE | ★★★★☆ | ★★★★★ | temporal corpus、HWM、无重复 Outer admission |
| 14 | 四 codeword Transport/Outer/Receiver production admission | DONE | ★★★★☆ | ★★★★★ | 同帧绑定、unique/duplicate/conflict、digest |
| 15 | Replay v2 的 LF4 live/offline 一致性 | DONE | ★★★★☆ | ★★★★★ | receiver-only replay、live/offline metric consistency、三类真实Replay同分类 |
| 16 | LF4 telemetry、RunReport.2 与严格 merger | DONE | ★★★☆☆ | ★★★★★ | 单 owner authoritative denominators、双端 exact artifact seals、create-only JSON/Markdown/CSV |
| 17 | GUI/CLI/profile 暴露与屏幕安全 preflight | DONE | ★★★☆☆ | ★★★★☆ | shared catalog、89-case binding Gate、no-activate右屏Gate、WGC pixel-only full publish |
| 18 | 可复现便携包、source set 与环境 fingerprint | DONE | ★★☆☆☆ | ★★★★☆ | Both-role package/SBOM、source seal、双端 environment、RunId deployment manifest |
| 19 | 同提交 LocalDesktop 回归 | DONE | ★★★☆☆ | ★★★★★ | 四组合publish/hash PASS、goodput无回退、independent seal verifier、Release 200/200 |
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

- **状态**：`DONE`；**难度**：★★★☆☆；**重要性**：★★★★★。
- **目标**：把通过 simulator/metric Gate 的 LF4 raster、mapping、codebook、Bootstrap binding 和 accepted Transport 结果变成不可漂移的实验 Golden。
- **实施要点**：固定 seed/session/frame；记录 canonical raster BLAKE3、coded bytes、metric quantization contract、4 个 accepted Transport hashes；提供独立 generator `--check`。
- **注意事项**：当前 `9b8e806` 只是 checkpoint，不能在真实 Gate 前宣称 frozen/certified；Golden 不保存大图时至少保存 manifest 和可重建 seed。
- **验收证据**：clean scratch 独立重生成；CPU/Qt5/Qt6/ASan 一致；旧 32 fixtures 仍不变。
- **完成出口**：已关闭。`PixelBridge.RemoteVisualLowFpsGolden.1` 固定 Step 02 Presenter 的 SessionTag/FrameSequence、44-byte Bootstrap、8100-byte coded data、21456-entry mapping、16-mask codebook、Step 07 selected metric binary32/int16 quantization contract、canonical BGRA BLAKE3 和 4×1350-byte accepted Transport。独立 Python oracle 不读取 C++ header/工具，最终两套 clean scratch fixture 与 PBRW 逐字节一致；production C++ gate 在 CPU/Qt5/Qt6/ASan 中一致通过，profile/codebook/mapping drift 均 fail closed，旧 DesktopLevels 32 帧不变。完整格式、命令、hash、验证与非认证边界见 `REMOTE_VISUAL_STEP08_GOLDEN.md` 和第 17 节。

### 11.11 Step 09：LF4 D3D11 Encoder raster 与 immutable texture

- **状态**：`DONE`；**难度**：★★★★☆；**重要性**：★★★★☆。
- **目标**：让 production Encoder 生成完整 LF4 raster 并以 1..5 Hz 替换 PB-owned texture，同时持续 Present 同一稳定 raster。
- **实施要点**：把四 codeword 的 raster generation 接入现有 scheduler；完整生成后一次性提交；重复 Present 不增加 FrameSequence/OuterBlockId；记录 dwell、generated/present FPS。
- **注意事项**：禁止逐 tile 更新可见纹理；pending replacement 可以丢弃未呈现候选但不能 torn；Encoder 不显示 receiver progress/ETA；0 和 >5 fail closed。
- **验收证据**：CPU canonical raster 与 GPU-present source hash/diagnostic readback 对齐；logical updates≤5；Present 可保持显示刷新率；窗口 no-activate/right-monitor containment。
- **完成出口**：已关闭。production `SenderFrameBuilder` 的 LF4 hidden candidate 在 headless truth probe 中跨 2 个完整 Carousel cycle 生成 9 帧，7 个 Control frame 的四个 Robust copy 与 2 个 Data frame 的 8 个 Transport 全部通过 production admission；native `EncoderRuntime` 同样完成 2 个 cycle 后，在一个不传入 runtime 的 test-local Decoder completion marker 之后继续生成并呈现第 9 帧，最后只因显式 stop 停止。WARP 与 RTX 5090 D 均证明 Step 08 canonical CPU raster BLAKE3 等于 GPU immutable source diagnostic readback，repeat Present 不推进 FrameSequence。完整命令、hash、计数与限制见 `REMOTE_VISUAL_STEP09_ENCODER.md` 和第 18 节。

### 11.12 Step 10：LF4 D3D11 scaled Walsh demod shader

- **状态**：`DONE`；**难度**：★★★★★；**重要性**：★★★★★。
- **目标**：直接从 PB-owned capture texture 按连续 geometry 解调 64,800 soft metrics 和 freshness summary，不回读整张 ROI。
- **实施要点**：constant buffer 包含 origin/scale/pitch/bounds/policy；one-owner immediate context；bounded slot/ring；GPU 输出仅 compact metrics/counters；shader 与 CPU 使用同一 codebook/mapping constants。
- **注意事项**：实际 texture pitch/format 必须显式处理；Flush 不是 completion；source lease 在 GPU copy 完成前不能归还；旧 strict-1:1 shader 不得静默解释 LF4 layout。
- **验收证据**：exact/scale/blur/stale fixtures 的 WARP tests；out-of-bounds/NaN/epoch stale fail closed；无 ROI-sized GPU→CPU readback。
- **完成出口**：已关闭。新增独立 `SubmitRemoteVisualLowFps`，普通 `Submit` 明确拒绝 LF4，旧 strict shader 不重解释新 layout。三个 LF4 compute pass 直接采样 PB-owned BGRA8 texture；CPU-built immutable mapping/codebook 和 per-frame logical binding避免复制第二套 interleave；固定回读 64,800 metrics + 16 calibration + 99 freshness/global entries，共 261,040 bytes，`rawPixelReadbackBytes=0`。WARP exact、独立 Area scale、独立 Gaussian blur、previous-sequence stale region、NaN/OOB/policy/work-budget、2-slot ring和 epoch cancellation全部通过；compact metrics 经冻结 candidate 与现有 QC-LDPC/Transport得到与 CPU reference byte-identical 的 accepted blocks。Release、完整 demod regression、CPU `PBRemoteVisualTests` 与 ASan通过。完整实现、命令、资源数学和非认证边界见 `REMOTE_VISUAL_STEP10_D3D11_DEMOD.md` 与第 19 节。

### 11.13 Step 11：CPU/WARP/hardware GPU truth parity

- **状态**：`DONE`；**难度**：★★★★☆；**重要性**：★★★★★。
- **目标**：证明 LF4 production GPU path 在跨 adapter/driver 环境下保持协议接受结果一致。
- **实施要点**：比较 canonical Bootstrap、FrameSequence、FEC disposition、Transport bytes、CRC/identity/padding，而不是 raw float；覆盖 WARP、NVIDIA、AMD 可用 adapter。
- **注意事项**：硬件不可用只能标记 unavailable，不能由 WARP 代替；adapter LUID 不匹配时不得隐式跨 GPU copy；量化容差必须在 FEC truth boundary 验证。
- **验收证据**：adapter fingerprint、accepted block manifest、metric summary、GPU timestamps、device-recreate case。
- **完成出口**：已关闭。隐藏 `PBRemoteVisualGpuParityTests` 在 WARP、AMD Radeon(TM) Graphics与当前 DXGI枚举出的三个独立 RTX 5090 D LUID上运行 exact、independent Area scale、independent Gaussian blur与 stale-region四场景；每个 backend销毁并重建设备后再跑 exact。generation 0/recreate共25个 GPU frame、100个 accepted Transport block manifest entries全部与 CPU reference byte-identical，FEC/CRC/identity/false accept均为0；10次 wrong-LUID submission均在计数/输出变化前拒绝。25/25 GPU timestamp有效，每帧 compact readback=261,040 bytes、raw pixel readback=0。create-only wrapper独立校验42条 JSON记录并封存 hash；完整矩阵、命令与限制见 `REMOTE_VISUAL_STEP11_GPU_PARITY.md`和第20节。

### 11.14 Step 12：CaptureNormalize、geometry、epoch 与 lifetime

- **状态**：`DONE`；**难度**：★★★★★；**重要性**：★★★★★。
- **目标**：把 WGC/DXGI ROI、format、ContentSize、monitor/DPI/rotation 和 CaptureEpoch 正确绑定到 LF4 geometry/demod work。
- **实施要点**：单显示器 physical ROI；WGC lease copy 到 PB texture；epoch change drain；topology fingerprint 变化停止 run；scale/letterbox/crop status 进入 telemetry。
- **注意事项**：不能访问归还后的 WGC surface；不能跨 epoch combine；未知 geometry 只能 diagnostic replay，不能 publish；不捕获 ProtectedMonitor。
- **验收证据**：resize/mode/device loss/recreate、epoch rollover、stale completion、DXGI/WGC normalization tests；lease/HWM 回到零。
- **完成出口**：已关闭。`CaptureConsumerCompletion`允许首个completion callback在owner immediate context上提交恰好一个GPU continuation；runtime保留exact PB-owned ROI并记录第二个query/fence marker，取消始终隐藏texture/context，post-submit exception按可能已有GPU work保守退休，第二continuation有界拒绝并terminal cancel。LF4 `CaptureDemodulator`先从同一ROI staging解析bound Bootstrap/continuous geometry，再在原texture lease内提交Step-10 GPU demod；cropped/unknown geometry只生成`TelemetryOnly/Rejected`，wrong adapter在allocation/admission前拒绝。WGC upright与DXGI raw ROTATE90 facade均通过完整WARP pipeline、epoch 1→2 recreate与CPU accepted-Transport逐字节比对：每epoch 4/4，continuation 2/2/0，最终lease/ROI/pending/queue为0。strict profiles与optional diagnostic fanout保持向后兼容；Release与ASan affected 9-suite均9/9 PASS。完整contract、资源数学、测试和truth boundary见`REMOTE_VISUAL_STEP12_CAPTURE_LIFETIME.md`与第21节。

### 11.15 Step 13：duplicate/gap/reorder/stall 与 bounded queue

- **状态**：`DONE`；**难度**：★★★★☆；**重要性**：★★★★★。
- **目标**：使远控重复帧、variable cadence、局部停更和恢复跳帧只影响 goodput，不破坏身份和资源上界。
- **实施要点**：`(CaptureEpoch, FrameSequence)` 去重；reordered 不 admission；gap/skipped 记账；1 s capture/visual stall；capture/demod/result queues 丢 stale 而不积累延迟。
- **注意事项**：像素相同不一定等价于 Bootstrap identity；同 FrameSequence 失败 observation 是否重试要由 bounded policy 明确；duplicate 不累计 BER/FER/verified bytes。
- **验收证据**：deterministic temporal corpus；HWM≤配置容量；恢复后继续接受新唯一帧；shutdown drain 完成。
- **完成出口**：已关闭。共享fixed-state `VisualIdentityTracker`以epoch/session/sequence区分unique、duplicate、reorder与gap；LF4 `CaptureDemodulator`再绑定canonical Bootstrap，只保存一个4-slot temporal frame，默认允许一次、hard max四次duplicate refinement。terminal duplicate/reorder不提交GPU；refinement只admit此前缺失slot，旧slot冲突fail closed；duplicate不增加FER/verified frame。44-observation WARP corpus得到6 unique、37 duplicate、1 reorder、1 gap/2 skipped，仅8次GPU submission与20个admitted blocks；32帧burst的result queue HWM为1，恢复新unique和新epoch均成功。1秒capture/visual stall、queue hard bound、epoch stale-drop和shutdown drain保持闭合。完整状态机、资源与验证见`REMOTE_VISUAL_STEP13_TEMPORAL_ADMISSION.md`和第22节。

### 11.16 Step 14：四 codeword Transport/Outer/Receiver production admission

- **状态**：`DONE`；**难度**：★★★★☆；**重要性**：★★★★★。
- **目标**：把 LF4 同一视觉帧的四个 codeword 作为四个独立、严格验证的 Transport candidates 送入现有 Receiver，而不建立新协议。
- **实施要点**：同 captured frame/Bootstrap/FrameSequence binding；逐 codeword FEC/padding/CRC/identity；Outer unique/duplicate/conflict；资源拒绝可观测。
- **注意事项**：四个 codeword 不要求同时成功，但不能来自不同 FrameSequence；same ID conflicting payload 必须 fail closed；duplicate 不增加 progress。
- **验收证据**：0..4 codeword success matrix、same-frame binding、conflict、orphan cache、resource limit、DirectRepeat/Wirehair tests。
- **完成出口**：已关闭。production `ReceiverPipeline`只遍历temporal admitted indices，独立复验每个Transport并调用现有`ReceiverIngress`；LF4四份相同Control Robust copy折叠为一次mutation，mixed/conflicting copy fail closed。headless hidden probe把首个data frame拆为Unique slots 0/1和同sequence refinement slots 2/3，并在Session Control后注入32个suppressed duplicate：两种Outer模式均只从raw 6 blocks admit 4 slots。DirectRepeat 1-byte与Wirehair V2 4096-byte文件分别以Outer unique 1/4达到Completed、WholeFileDigest PASS、publish PASS和external bytes exact；duplicate不增加Outer unique/verified bytes。完整contract、Receiver negatives和验证见`REMOTE_VISUAL_STEP14_RECEIVER_ADMISSION.md`和第23节。

### 11.17 Step 15：Replay v2 LF4 live/offline 一致性

- **状态**：`DONE`；**难度**：★★★★☆；**重要性**：★★★★★。
- **目标**：让每次真实 LF4 失败都能在没有远控软件的情况下重放 Bootstrap、GPU/CPU demod、FEC 和 Receiver。
- **实施要点**：presence flags 表示 sender truth 缺失；记录 profile/layout、geometry、CaptureEpoch、observation ID、ROI bytes、live result；offline mode 走同一 production decoder。
- **注意事项**：recorder queue 满只丢样本不阻塞 demod；默认 256 frames/2 GiB，硬上限 2048/16 GiB；`.partial` 和 no-overwrite；只保存 selected ROI。
- **验收证据**：v1 compatibility、v2 receiver-only、corrupt/truncated/oversize/checksum/no-overwrite、live/offline metric consistency。
- **完成出口**：已关闭。Replay v2保持旧基础layout与历史reader语义，在reserved header中用显式presence/detail-version记录production layout、continuous geometry、temporal/result、FEC/CRC/identity、raw/admitted carriers、metric/freshness、GPU timing presence和Receiver结果；sender truth缺失继续是unavailable而非0。实际bounded async recorder生成6-frame receiver-only LF4 Replay，2个duplicate suppressed、drop=0；live/offline同一`CaptureDemodulator`/`ReceiverPipeline`得到6/6 observation、0 mismatch、4 admitted Transport、Outer unique 1，并分别WholeFileDigest PASS、safe publish PASS、external 1-byte exact。Step 02真实Direct/Shape/LF4各8帧又分别得到8 Bootstrap、7 duplicate、42/32/4 admitted Transport、0 FEC/CRC/identity，并一致停在descriptor absent/no publish。v1/v2、corrupt/truncated/oversize/checksum/no-overwrite与queue bounds继续通过。完整contract、结果与命令见`REMOTE_VISUAL_STEP15_REPLAY_PRODUCTION.md`和第24节。

### 11.18 Step 16：LF4 telemetry、RunReport.2 与 merger

- **状态**：`DONE`；**难度**：★★★☆☆；**重要性**：★★★★★。
- **目标**：使 1..5 Hz 下的容量、重复 Present、unique visual、四 codeword 和 verified goodput 各有正确分母。
- **实施要点**：增加 LF4 symbol unreliable、fresh/stale region、erased metrics、per-codeword FEC、accepted Transport、Outer unique；sender receiver metrics 继续为 null；严格 merge profile/session/source/time。
- **注意事项**：0 不代表 unavailable；PreFecBER 没 truth 时必须 null；ceiling/generation/presentation/verified goodput 不得混用；journal failure 使 evidence invalid 但不改 acceptance。
- **验收证据**：unit tests 覆盖 denominator、null semantics、duplicate、time/provider/profile mismatch；combined JSON/Markdown/CSV seal。
- **完成出口**：已关闭。`PBTelemetry` 成为 LF4 signal/FEC 分母的单 owner：每个有效 metric observation 固定贡献 64,800 metrics、16,723 symbols 与 77 freshness regions，unique FEC frame 固定评估 4 个 codeword，raw FEC accepted 与 Receiver-bound temporal admission 分列；无 sender truth 的 PreFEC BER 为 `null`，非零分母上的零错误为数值 0。Encoder `capacityModel` 分列 64,800 raw bits、5,400 Inner-FEC bytes、5,256 Transport payload bytes，以及 nullable generation rate；Decoder report 冻结 profile identity 并记录 signal/FEC/temporal/Outer/verified truth。严格 merger 重新读取并封印两端 exact report artifacts，核对 Session/profile/source/provider/time 与全部 denominator，仅在 Completed、WholeFileDigest、publish、external hash/length、journals、identity、零 conflict 和两端 seals 同时成立时允许 success，并 create-only 发布单次 JSON/Markdown/CSV。Release/ASan telemetry/application Gate、真实 Step 02 Replay 条件 Gate及 23 项 merger tests 均通过；完整 schema、命令与限制见 `REMOTE_VISUAL_STEP16_TELEMETRY_REPORT.md` 和第 25 节。

### 11.19 Step 17：GUI/CLI/profile 暴露与屏幕安全

- **状态**：`DONE`；**难度**：★★★☆☆；**重要性**：★★★★☆。
- **目标**：只有 Gate A-C 通过后才把 LF4 加入 product enum/GUI/CLI，并让非法 FPS/geometry/monitor 配置在启动前失败。
- **实施要点**：独立 `remote-lf4` profile token；1..5 FPS；metadata preset；ROI physical rect/snap/scale status；no-activate Data Window；topology watchdog。
- **注意事项**：不能把旧 `remote` token 重解释成 LF4；不自动移动外部远控窗口；不使用全局输入；不显示 receiver progress 于 Encoder。
- **验收证据**：config/QSettings/CLI parser/GUI model tests；right-monitor containment；ProtectedMonitor intersection fail closed；focus PID 前后不变。
- **完成出口**：已关闭。GUI与两端CLI使用同一个四项只读profile catalog，独立token `remote-lf4`不重解释旧`remote`；QSettings不持久化profile。LF4严格要求RemoteVisual、1..5 Hz、显式且不同的Protected/Experiment monitor；Encoder固定1920×1080 Data Window与Decoder 0.5x..2.0x单屏ROI在启动前验证，worker启动后才报告PASS并每秒重验topology/identity/geometry。最终no-activate Gate保持前台PID `63784`不变并完成2个Carousel cycles；公开5 Hz CLI为0 dwell violation。首次实际WGC闭环暴露两阶段LF4 completion会使TelemetryOnly越过早期GPU结果，现以固定slot observation barrier与有界结果环最小序交付修复，未放松Telemetry；最终公开CLI pixel-only闭环以4个temporally admitted Transport、Outer unique 1完成WholeFileDigest/publish并恢复external byte exact文件。Release核心6/6、定向parser/model 89/89、report 24/24及MSVC ASan核心6/6通过。完整契约、失败证据、hash和truth boundary见`REMOTE_VISUAL_STEP17_GUI_CLI_SCREEN_SAFETY.md`与第26节。

### 11.20 Step 18：便携包、source set 与环境 fingerprint

- **状态**：`DONE`；**难度**：★★☆☆☆；**重要性**：★★★★☆。
- **目标**：确保 Computer A/B 实际运行相同 tested tree、依赖和 profile，而不是混用旧包。
- **实施要点**：create-only package；manifest 包含 commit/tree/compiler/Qt/vcpkg/EXE/DLL hashes；CSPRNG source set；共享 metadata/RunId；endpoint environment fingerprint。
- **注意事项**：Computer B source 在 Session 中不可变；包不能携带 payload 旁路；manifest hash 必须在复制前后核对；provider UI 信息只作为 metadata。
- **验收证据**：clean directory package rebuild、hash verify、tamper negative test、两端 packageManifest 完全匹配。
- **完成出口**：已关闭。正式包统一为同一Both-role archive，schema 2 manifest封印HEAD/tree、当前tested-source全清单与指纹、MSVC/SDK/CMake/Qt/vcpkg ABI、LF4常量、全部EXE/DLL/resource、SPDX 2.3 SBOM和完整license notices；独立verifier逐文件/ZIP-entry复算并拒绝tamper、额外路径、reparse和overwrite。CSPRNG source set另以sourceSetId、三个payload、ZIP inner payload及外部seal独立验证。两端environment只能调用verified package内Decoder做无窗口只读probe，并与shared metadata/RunId及package/source exact hashes绑定；deployment manifest要求Encoder/Decoder恰好各一份并独立复算全部artifact/self/deployment fingerprint。任何正式run现可由RunId反查唯一package、source和endpoint environments。完整规范、命令和truth boundary见`REMOTE_VISUAL_STEP18_DEPLOYMENT_REPRODUCIBILITY.md`。

### 11.21 Step 19：同提交 LocalDesktop 回归

- **状态**：`DONE`；**难度**：★★★☆☆；**重要性**：★★★★★。
- **目标**：证明 LF4、scheduler、telemetry 和 GPU 修改没有破坏 frozen LocalDesktop oracle。
- **实施要点**：相同 8 MiB RAW、同 ROI/显示器、WGC/DXGI × Direct/Shape；replay off；比较 goodput/FER/unique FPS/CPU/GPU/HWM。
- **注意事项**：必须用最终 tested commit；用户左屏有并发工作则记录，不伪装 clean benchmark；WholeFileDigest/false accept/unbounded queue 直接 BLOCK。
- **验收证据**：pre/post combined table、external hash、Encoder still broadcasting；goodput regression≤10% 或具有可复现解释。
- **完成出口**：已关闭。冻结同一8 MiB RAW、ROI/显示器和replay-off条件下，WGC/DXGI × Direct/Shape四组均完成WholeFileDigest、安全发布与external SHA-256；goodput相对基线无回退，Decoder完成时Encoder仍广播，journal证明descriptor建立后resource rejection不再增长且conflict为0。定向7/7与完整Release 200/200均PASS；完整证据见`REMOTE_VISUAL_STEP19_LOCALDESKTOP_REGRESSION.md`。

### 11.22 Step 20：真实双机 LF4 pilot

- **状态**：`MANUAL-GATE`；**难度**：★★★★☆；**重要性**：★★★★★。
- **目标**：以最小 1 MiB RAW 证明 LF4 从 Computer B Data Window 经真实远控视频到 Computer A Receiver 的第一条完整链。
- **实施要点**：分别跑 1/2/5 Hz；优先 strict 1:1，再跑可定位 scale；WGC；每轮保存 Replay v2、两端 report、remote UI metadata、external hash。
- **注意事项**：Decoder 成功即 run 成功，Encoder 仍广播后人工 Stop；最长窗口/无进展规则预先固定；不得使用文件传输、clipboard 或任何旁路。
- **验收证据**：WholeFileDigest、publish、external length/SHA-256、0 false output、Replay offline reproduce。
- **当前准备状态（非完成证据）**：已增加production Replay采样/资源预检、publish后2秒tail、offline footer完成态延迟、Encoder交互式人工Stop、Decoder固定no-progress；另以create-only工具冻结双端deployment/UI/plan、A/B endpoint启动和最终独立验证。5 Hz strict计划探针复算为1222帧、约9.91 GiB含预留，右屏only UI evidence与adversarial contract已通过。首轮单屏实机诊断证明静态LF4可经真实远控到A端actual WGC pixels并通过32个receiver-only Transport blocks，同时发现并修复exact ROI locator在`-1.45e-5 px`原点处过早`InvalidInput`的问题；随后v2独立B run实时回显READY，A端两轮exact ROI捕获合计11帧，其中10帧全链接受、1帧`MarkersNotFound`安全擦除，共40 accepted blocks且0 FEC/CRC/identity failure，所有接受帧geometry均为exact `(0,0,1,1)`。receiver-only `falseAcceptedCodewords`保持unavailable；同一Replay前后对照、v2 correlation和单屏限制见`REMOTE_VISUAL_STEP20_SINGLE_MONITOR_DIAGNOSTIC.md`。复验后deployment/pilot evidence contracts为2/2 PASS，最终Release build exit 0、完整CTest 201/201 PASS；这些诊断仍没有文件Receiver/WholeFileDigest/publish，不能替代正式双机文件Gate；正式人工步骤见`REMOTE_VISUAL_STEP20_REAL_REMOTE_PILOT.md`。
- **额外人工硬门禁**：Step 20结束前再次执行Computer B Encoder → 实际远控桌面/视频链路 → Computer A WGC Decoder → WholeFileDigest/safe publish/external SHA-256 →同一Replay offline reproduce完整工作流；Decoder完成后等待冻结的跨机时钟证明窗口再人工停止仍在广播的Encoder。文件传输、clipboard、本地闭环、offline或模拟结果均不能替代。
- **完成出口**：至少一个 FPS 配置完成一次端到端恢复；失败也必须能归入 geometry/signal/temporal/metric/scheduler 之一。当前单屏诊断已把首轮历史失败归为geometry boundary roundoff并在同一Replay上关闭；v2 的单帧失败又明确归为signal/Bootstrap `MarkersNotFound`安全擦除，相同参数复验3/3通过。但文件级出口尚未满足，状态保持`MANUAL-GATE`。

### 11.23 Step 21：正式 provider-generic RemoteVisual 矩阵

- **状态**：`MANUAL-GATE`；**难度**：★★★★★；**重要性**：★★★★★。
- **目标**：跨 remote mode、scale、FPS、backend 确定 LF4 的可用域与 blocker，不绑定具体远控品牌。
- **实施要点**：1/2/5 Hz；质量优先/自动/受限模式；约 0.75/1.0/1.259/1.5 scale；WGC 主矩阵、DXGI 代表性复核；每组固定 run window 和 cycle 条件。
- **注意事项**：模式是否真的启用必须来自 UI 可见证据；unknown chroma/latency 不猜值；不同 run 不能合并；非法 geometry 不 silent resample。
- **验收证据**：per-run endpoint/combined reports、CSV、Replay、environment/package seals、failure classification。
- **当前准备状态（非完成证据）**：production sampled Replay/live+offline Receiver入口已从LF4单点扩展到Direct/Shape/LF4三个公开profile，仍保持primary demod不受采样节流、Direct/Shape exact 1:1和LF4 locator/0.5x..2.0x边界。PilotPlan.2现冻结profile/backend/UI-visible mode/scale/FPS、双端live monitor identity及每run资源预算；成功与失败均生成统一MatrixRunRecord，失败只有在同一Replay的Inspector及live/offline report确实支持geometry/signal/temporal/metric/scheduler分类且无final output时才可封存。canonical create-only MatrixSpec仍在执行前固定36个LF4/WGC单元、3个DXGI代表点和2个strict-1:1 profile baseline；针对用户明确声明的Computer B两块1920×1080、Computer A两块2560×1440实验条件，又在任何正式run前新增由A端sealed monitor catalog机械派生且不可覆盖的`Dual2560x1440SingleExperimentMonitor` HardwareScope：31个included cell、10个因单屏无法容纳2880×1620 ROI而excluded的1.5× cell。scope importer会重读父41-cell spec与catalog并重算31/10 partition；最终verifier要求恰好31个独立RunId、同package/source、逐run CSV和同条件Direct/Shape/LF4权威指标对照，拒绝缺项、scope外替换、artifact复用、推断latency/chroma或跨run合并，并明确`excludedCellsDoNotCountAsCoverage=true`。原41-run与新31/10 adversarial contract fixture均已通过；完整field run尚未开始。规范见`REMOTE_VISUAL_STEP21_PROVIDER_GENERIC_MATRIX.md`。这些均为readiness，不是现场矩阵结果；31-cell PASS也不会声称1.5×或full-41 coverage。
- **完成出口**：当前双机硬件范围内31个included cell均有独立成功或受支持的失败证据，hardware-scoped verifier与CSV/summary/seal PASS，Direct/Shape/LF4均被覆盖，且LF4的提升或退化由同条件权威指标而非截图主观判断支持；最终报告同时保留10个未运行1.5× cell的限制。

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

Step 07 关闭的是 CPU/reference calibration candidate Gate，不是 production deployment。该 candidate 已由 Step 08 与现有 LF4 raster/mapping/codebook/Bootstrap/accepted Transport 结果一起冻结成可独立 `--check` 的 Golden，但仍未接管 production default；Step 09/10 以后才实现 D3D11 production raster/demod。当前只有一组 Windows Remote Desktop External，画质/chroma mode 为 Unknown；没有 provider matrix、VerifiedEncodedGoodput、`UniqueVisualFPS`、6 小时 soak、双机文件 WholeFileDigest/final publish 或 Certified Profile 结论。

---

## 17. Step 08 LF4 Golden / manifest 冻结完成证据

完整 artifact schema、二进制格式、重建命令、hash 与 truth boundary 见 `REMOTE_VISUAL_STEP08_GOLDEN.md`。

### 17.1 独立 oracle 与固定输入

- 新增 `generate_remote_visual_lf4_golden.py`，不读取 C++ header、不调用 PixelBridge executable；它只复用既有独立 Bootstrap RS/CRC scaffold 和旧 DesktopLevels 已验证的独立 Robust QC-LDPC Python oracle。
- 固定 `VisualProfileId=0x504252564C463431`、`LayoutVersion=7`、`SessionTag=0x5354455030325244`、`FrameSequence=17`、`ControlEpoch=0`、diagnostic domain `PB-RemoteVisual-LF4-X1-Data`。
- Python 独立 literal 实现七段 geometry、98-region freshness、21456-entry mapping、four-plane affine inverse、16-mask Walsh codebook 与 BGRA paint。完整 raw BGRA BLAKE3=`28b09b66520b87f9cd9407b516530ad1225f84d390cc2b94d18d6e1b8d5e9bc4`，与 Step 02 Presenter seal 相同。
- `--check` 严格只读并要求 exact inventory/bytes；generation 拒绝 existing output；`.gitattributes` 固定 LF4 binary/text 的 checkout 字节，阻断 Windows `core.autocrlf` 漂移。独立单元测试让 profile、codebook、mapping 各漂移一个 byte，三类均按预期非零失败。

### 17.2 Golden 与 production truth cross-check

- `tests/golden/remote-visual/lf4/` 共 12 个文件、451051 bytes，manifest schema 为 `PixelBridge.RemoteVisualLowFpsGolden.1`，manifest SHA-256=`fc43bb7849f6d00c063372b459338b1b88a324d5bda647b74d4329ac8c13cf79`。旧 `tests/golden/remote-visual/manifest.json` 未改。
- 固定 44-byte Bootstrap、8100-byte coded data、16×LE16 codebook、21456×20-byte mapping、16-bin metric calibration、111 个 float/adapter probes、raw raster BLAKE3 和 4×1350-byte accepted Transport。
- production gate 逐项比较全部 tile mapping 与 codebook，重建完整 raster 并计算 BLAKE3；raster 再只通过 pixels 进入 production CPU demod，hard bits 必须等于 coded Golden。
- calibrated metrics 使用既有 scale4096/int16 adapter，进入既有 QC-LDPC、padding、Transport CRC 与 identity authority；4/4 block 必须 verified，slot/byteCount/bytes 必须逐字节等于 independent Golden。sender expected bytes 不进入 demod/FEC。

### 17.3 Step 07 candidate freeze 边界

- `lf4-default/PiecewiseLookup` 的 16 个 binary64 upper、calibrated binary32 bits、Train sample/error 与 Step 07 provenance 全部固定。
- `CalibrateRemoteVisualLowFpsMetric` 仅暴露该 frozen reference candidate；非有限或超 terminal 输入 fail closed 且不修改 output，`+0/-0` 保留，bin 使用 inclusive upper。
- 111 个 probe 覆盖所有 bin boundary 的相邻 binary32、正负号、最小 subnormal、terminal、`±Inf`、qNaN，以及所有可产生 calibrated output 的 int16 rounding/clamp 与 hard-decision 两侧。
- production LF4 decoder default/admission 未改变；`FrozenCandidateNotProductionDefault` 不能解释为 deployment 或 Certified Profile。

### 17.4 可重复 seal 与验证

最终 create-only seal：

```text
build-p1_5-evidence/20260901-step08-lf4-golden-final-d
build-p1_5-evidence/20260901-step08-lf4-golden-final-e
build-p1_5-evidence/20260901-step08-lf4-raster-final-d.pbrw
build-p1_5-evidence/20260901-step08-lf4-raster-final-e.pbrw
```

两套 12-file fixture 对应文件逐字节一致；两份 8294428-byte PBRW 逐字节一致，SHA-256=`8bd270750b30f8705d772d14806aa88201fde80e6321b3074b761711eaf0052f`；final-d 再执行 `--check` 通过。

| 验证 | 结果 |
| --- | --- |
| LF4 independent Python tests | 4/4 OK |
| LF4 committed `--check` | PASS |
| 旧 DesktopLevels oracle | `files=100 frames=32` PASS |
| Release / Qt5 / Qt6 / MSVC ASan LF4 targeted gate | 各 1/1 PASS |
| Release `ALL_BUILD` / 无界面 CTest | PASS / 155/155 PASS，246.43 s |
| MSVC ASan `ALL_BUILD` / 无界面 CTest | PASS / 290/290 PASS，442.75 s |
| PBModulation cppcheck 2.21.0 | 10/10 translation units；34 项均精确匹配既有 review ledger，0 new/unreviewed finding |

无界面 CTest 排除了 `Native|GuiSmoke`；Qt build-tree 一致性、CPU 和 ASan 不是 hardware GPU 或 field certification。本节固定的 canonical raster 与 hash 随后成为 Step 09 immutable D3D11 source/readback Gate 的输入；Step 09 完成证据见下一节。

---

## 18. Step 09 LF4 D3D11 Encoder / immutable source 完成证据

完整 compatibility/exposure 边界、状态机、D3D11 lifecycle、命令、逐文件 hash 与限制见 `REMOTE_VISUAL_STEP09_ENCODER.md`。

### 18.1 Hidden production-code sender candidate

- `VisualProfile::RemoteVisualLowFps` 追加在 enum 尾部，只允许内部 Encoder Gate；Qt profile combo 与 CLI 均不提供该值，Decoder validation 明确拒绝。
- 既有 `RemoteVisualResilient` 仍绑定原 profile ID/layout/raster/Decoder；没有把旧 `remote` token 静默改成 LF4。
- LF4 Control 把同一份完整 Robust QC-LDPC codeword 复制到四个 slot；Data 继续生成四个独立 Transport codeword；二者都通过 production `EncodeRemoteVisualLowFpsFrame` 生成完整 raster。
- production builder probe 使用 1-byte source、`controlRepetitions=1`，跨 2 个完整 cycle 共生成 9 帧：7 个 Control frame 的 28 个 copy 全部逐字节等于 PB-Control-1，2 个 Data frame 的 8 个 Transport 全部通过 FEC/CRC/identity；一个不传给 builder 的外部 completion marker 后仍生成 1 帧。

### 18.2 Immutable source 与 repeat Present

- `repeatActiveFrame` 默认 false，仅隐藏 LF4 candidate 启用；完整 pending raster 的容量固定为 1。
- 新 logical frame 建立 `D3D11_USAGE_IMMUTABLE` BGRA source，在 staging readback 逐 row/pitch 验证后才替换 active；每次 `Present` 都完整 `CopyResource` 到 flip-discard back buffer。
- 没有 pending 时，frame-latency permit 重复呈现同一 active source，不推进 `FrameSequence`/Carousel/Outer ID；epoch/mode/size/device/failure 会使 active identity 失效，shutdown 清零并释放资源。
- 开发过程中 `BindFlags=0` 的 immutable texture 在运行时返回 `E_INVALIDARG`；修复为显式 `D3D11_BIND_SHADER_RESOURCE`。首次 DXGI `statistics-disjoint` 由 sequence 16 warm-up 吸收并验证旧 source 失效，canonical sequence 17 在新 epoch 重交，未用 fallback 掩盖失败。提交前 strengthened cleanup Gate 又发现 Stopped backend 的 wake event 仍保留到 destructor；现在 owner thread 离开全部 wait 后由 `Shutdown` 显式关闭，partial initialization 与正常 WARP/hardware/LF4 shutdown 均要求 graphics/owned handles/HWND 为零。

Step 08 canonical raw BGRA BLAKE3 在 WARP 与 RTX 5090 D hardware source staging readback 中均 exact：

```text
28b09b66520b87f9cd9407b516530ad1225f84d390cc2b94d18d6e1b8d5e9bc4
```

| backend | canonical seq 17 | replacement seq 18 | evidence |
| --- | --- | --- | --- |
| WARP | sources=2，repeat=24，present calls=26 | sources=3，repeat=35，present calls=38 | `gate.txt` SHA-256=`c1915ac2cfe6252cc00a87fa25ab0dd57b515c28fc771d75d598fbcc3dac1d38`；JSONL=`03901b91450748adc21aeade271377c17ea7485b5bc8e58d2c24332f0535bdc3` |
| RTX 5090 D hardware | sources=2，repeat=24，present calls=26 | sources=3，repeat=35，present calls=38 | `gate.txt` SHA-256=`07ab026a8103428554f5d7a93a252b20e72328397febc10d1f0698c2cf326d01`；JSONL=`d487329b96f0201886c3fd04aa6ca58dfa6d00f74a9f9017a789c0d34d22c27d` |

两类 Gate 都选择最右 `\\.\DISPLAY2`，实际 output 2560×1440@180 Hz，1920×1080 client origin `(2880,180)`；窗口 no-activate，没有发送鼠标/键盘输入或改变显示设置。WARP snapshot 的 adapter description 是关联 output 的名称，`softwareRasterizer=true` 才是 backend authority。

### 18.3 Production EncoderRuntime cadence / independence

- logical interval 使用 steady-clock duration 的向上取整；dwell 以成功提交间隔计数，5 Hz 配置为 200 ms，任何更短 observation 都增加 violation。
- generated logical FPS 使用 `(N-1)/(last-first)`，不把启动时第一帧错误计为一个已过去 interval；present-call FPS 单独记录 repeat display cadence。
- 真实 `EncoderRuntime` 到 frame 8 时完成 2 个 4-frame cycle；test-local marker 不传入 runtime，随后继续到 frame 9；最终只由显式 Stop 结束。
- frame 8 的 generated FPS=4.973131591、configured/min dwell=200/200.0132 ms、violations=0、sources/repeats=8/68；frame 9 为 4.973155837 fps、sources/repeats=9/79；停止后 pending=0、active=false。
- broadcasting/after-marker/stopped report SHA-256 分别为 `849c714ea99c33548d8288845173bfdc47cf3a44bde90d9e3644863eb7236133`、`7c42ecb106658b92ba06a03467e8759b5d631c77e8511a63213b928ec29fca8c`、`64af2bf0d9f00b7395b3097692f522f6cd68c86f6a16ca7f30d4b32043a5648b`；`gate.txt` 为 `19cd4bad562e16aaafc03a9ae70250c8be7837f6579856896b9f4a99decaad34`。
- Encoder report 的 receiver progress、ETA 与 VerifiedEncodedGoodput 仍为 null/不存在；marker 不是 actual Decoder completion。

最终 create-only evidence：

```text
build-p1_5-evidence/20260901-step09-final-warp-d/lf4-encoder-warp-57364-718624121037
build-p1_5-evidence/20260901-step09-final-hardware-d/lf4-encoder-hardware-46976-718627462559
build-p1_5-evidence/20260901-step09-final-production-d/lf4-production-encoder-59608-718636392197
```

### 18.4 提交前验证与 truth boundary

| 验证 | 结果 |
| --- | --- |
| Release `PBApplicationTests` + `PBRenderD3DTests` | 2/2 PASS，6.23 s |
| Release `ALL_BUILD` / 无窗口全量 CTest | PASS / 155/155 PASS，237.54 s |
| WARP / RTX 5090 D immutable-source native Gate | 2/2 PASS；CPU/GPU source BLAKE3 exact；完整 repeat-copy lifecycle 与 Stop 后 graphics/handles/HWND=0 PASS |
| 既有 WARP/hardware presentation Gate | PASS；partial initialization、swap-chain replacement、flip/latency matrix 与 strengthened handle cleanup contract 通过 |
| production EncoderRuntime native Gate | PASS；2 cycle、marker 后继续、explicit bounded stop |
| MSVC ASan/RelWithDebInfo `ALL_BUILD` / 无窗口全量 CTest | PASS / 290/290 PASS，488.73 s |
| Qt 5.14.2 / Qt 6.10.1 `PBApplicationTests` | 各 1/1 PASS，5.54/5.50 s；两套 Encoder target build PASS |
| cppcheck 2.21.0 | PBRenderD3D/PBPresentationGate 零发现；application/GUI 仅 exact-reviewed baseline，零 new/unreviewed finding |
| `git diff --check` | PASS |

GPU source readback 不是 capture；WARP 不是 hardware；source-to-back-buffer hash 不是 remote-provider survival。按 Step 09 完成时的边界，当时仍没有 LF4 live product Decoder、scaled GPU demod、CaptureEpoch/lease 闭环、Outer/file convergence、WholeFileDigest/final publish、provider matrix、VerifiedEncodedGoodput 或 Certified Profile 结论。后续 Step 10 已关闭独立 experimental scaled Walsh D3D11 demod primitive；这不改变本段对 Step 09 证据本身的限定，也未提前关闭其余边界。

### 18.5 Step 09 后置动态 Windows RDP receiver-only pilot

Step 09 关闭后，新增 evidence-only `PBRemoteVisualLf4DynamicPresenter`；它不实现第二套 encoder，而是以隐藏的 `RemoteVisualLowFps` candidate 调用 production `EncoderRuntime`。Computer B 的 sealed package 使用 1 MiB deterministic RAW source、Wirehair V2、5 logical FPS、64 次 Control repetition 和 300 秒有界时限。package SHA-256=`2c4240478b9f22a72c483ae6799ddaa88c9099d1659a10ea8482082b5f5487b4`。

第一次未采样的动态 capture 暴露了真实资源边界：2434×1376 BGRA8 每帧 13,396,736 bytes，而远控 WGC delivery 明显高于 5 FPS；60 秒 Replay 达 17,175,294,158 bytes 并触及 16 GiB 上限，因此 report 为 invalid，未被当作成功 Gate。后续增加只允许 diagnostic capture-only 使用的 10 Hz pre-readback sampler：WGC interval 只是 hint，`DiagnosticCpuReadback` 在 `CopyResource` 之前执行权威时间采样；`sampledOutFrames` 与 capture/readback/recorder drop 分开，capture domain 变化重置 sampler。

正式 120 秒 evidence：

```text
build-p1_5-evidence/step09-lf4-dynamic-formal-sampled-120s-20260901-225635
```

- capture 1085 arrived/1085 delivered；Replay 1026 written + 59 intentionally sampled + 0 dropped，精确闭合；capture/readback drop、stale、epoch reset 全为 0；Replay 13,745,594,661 bytes，valid/finalized；monitor-safety PASS；
- 两次离线 Inspector JSON 逐字节一致，SHA-256=`7c17433c2c8ee26d907893c3d0d99cf6e9362be9c298f3c5a071cb3bf0a75dee`；
- 731/1026 帧通过 Bootstrap、modulation 与 authority；357 个完整 Control frame 接受 1428 个 copy，374 个完整 Data frame接受 1496 个 Transport block；partial Data frame=0；
- 一个 SessionTag，FrameSequence 1..395 中有 394 个 distinct、337 个 duplicate observation、1 个显式 gap/skip、0 reorder；FEC/CRC/identity failure 均为 0；
- Replay SHA-256=`6dbc62308c18a155ac8ae4c1b2fe06db65c0ebadd8407debe10975efb6fd0748`；sealed summary SHA-256=`51bb47f5f40b5bc947fc31876b791e0606322fe47c59794ed013ba15083c1294`。

完整环境、命令、资源数学、验证矩阵、逐项 hash 与 truth boundary 见 `REMOTE_VISUAL_STEP09_DYNAMIC_RDP_PILOT.md`。该 pilot 证明真实动态远控像素、WGC capture、Replay 与离线 LF4/FEC/Transport primitive；它没有 sender expected-byte oracle，`falseAcceptedCodewords` 仍 unavailable；也没有 live product Decoder、Outer/file path、WholeFileDigest、final publish、VerifiedEncodedGoodput 或 Certified Profile。2026-09-01 的停止点确实保持 Step 10 `PENDING`；2026-09-02 的后续实现已按第 19 节将 Step 10 独立关闭。

---

## 19. Step 10 LF4 D3D11 scaled Walsh demod 完成证据

完整 implementation boundary、shader/resource contract、命令、验证矩阵与限制见 `REMOTE_VISUAL_STEP10_D3D11_DEMOD.md`。

### 19.1 Direct-texture compact pipeline

- 新 LF4 API只接受同一 admitted observation/domain 的 canonical Bootstrap与连续 geometry；普通 `Submit`/`SubmitUnbound` 不会让旧 strict shader解释 LF4。
- calibration、freshness与 data 三个 compute pass在一个 owner immediate context顺序提交；event query位于 compact copies之后，`Poll(DONOTFLUSH)` ready前不 `Map`，source texture与 caller lease不得提前复用。
- CPU production constants建立 21,456-entry immutable tile mapping、16-mask codebook与 per-frame logical/freshness binding；compile-time drift shields固定 HLSL role、coded bits、bits/tile与 ladder geometry。
- GPU回读 `259,200` metric bytes + `256` calibration bytes + `1,584` freshness/global bytes = `261,040` bytes；1920×1080 BGRA8下约为 ROI的 `3.15%`，raw pixel readback为 0。
- `maximumDataWorkUnits`按 CPU-reference source Pixel read语义保守计入每次双线性 sample最多 4个 texel；2,000,000-unit负例证明 GPU不会用较宽松的“一个 SampleLevel=一个 unit”边界接受 CPU会拒绝的工作。

### 19.2 Authoritative fixtures

| case | 输入 | 结果 |
| --- | --- | --- |
| exact | canonical LF4 raster + padded BGRA upload pitch | 4/4 accepted Transport与 CPU byte-identical；zero metrics=0；compact/readback contract PASS |
| scale | independent Area `1.259375×1.259259...` + fractional origin | CPU/GPU accepted blocks和 freshness/reliability counters一致 |
| blur | independent fixed 3×3 Gaussian | CPU/GPU accepted blocks和 counters一致 |
| stale | previous-sequence完整 region替换 current region | exactly 1 stale region；相关 metrics全 zero；FEC仍恢复4/4 Transport |
| negative/ring/epoch | NaN、OOB、invalid policy、work limit、2-slot saturation、domain invalidation | exact fail closed；2个 pending event完成后 Cancelled；无 premature readback |

Release LF4 4/4、完整 `PBDemodD3D11Tests` 13/13、CPU/reference `PBRemoteVisualTests` 1/1与 ASan LF4/完整 demod suite均通过。三个 HLSL entry以 `fxc /Ges /WX /T cs_5_0`独立编译通过，`git diff --check`通过。

### 19.3 当前 truth boundary

Step 10 的 authoritative endpoint是同 frame CPU/WARP `FrameEvaluation`、FEC disposition和 accepted Transport bytes，而不是 raw float bit equality。Step 10完成时 WARP不是 hardware且没有关闭 adapter matrix；后续 Step 11已按第20节在当前可用 AMD/NVIDIA hardware上关闭 accepted-block parity和 device-recreate，Step 12又按第21节把同一API接入`CaptureDemodulator`与WGC/DXGI共用lifetime。该历史Step-10证据本身仍不包含GUI/CLI exposure、Receiver/Outer/WholeFileDigest/final publish、provider matrix、VerifiedEncodedGoodput、soak或Certified Profile结论。

---

## 20. Step 11 CPU/WARP/hardware GPU truth parity 完成证据

完整 adapter枚举、identity contract、corpus、accepted manifest、GPU timestamp、create-only wrapper、命令、hash与限制见 `REMOTE_VISUAL_STEP11_GPU_PARITY.md`。

### 20.1 当前 adapter matrix

- WARP：Microsoft Basic Render Driver，LUID `00000000:0001895a`；
- AMD：AMD Radeon(TM) Graphics，LUID `00000000:000189ad`；
- NVIDIA：三个名称相同但 LUID分别为 `00000000:000172f1`、`00000000:00023612`、`00000000:0002223a`的 RTX 5090 D adapter identity；
- 四个 hardware candidate全部能创建 D3D11 feature-level 11.1 device并实际运行；NVIDIA=`pass`、AMD=`pass`，没有 hardware unavailable或 WARP替代项。

### 20.2 Protocol truth 与 failure boundary

每个 backend运行 generation-0 exact/scale/blur/stale和 device-recreate exact。Gate逐字段比较 `FrameEvaluation`中的 padding、FEC、CRC、identity、false accept、accepted counts和 coded-bit诊断，并逐字节比较4个 accepted Transport blocks；raw float只进入 metric summary。exact/scale/blur及所有 recreate exact的 accepted-set BLAKE3均为 `1f7fd593d668849ee8d5ac134b2581bb8a2cb20c05bcb1f0fcebe1e18465ae24`；stale set为 `bab7226cc3d954f97c3585aac160db1ee6bdfa9d1b40348de4f7ae7644f94f4a`。任何跨 adapter digest差异使 test失败，不会改用 CPU结果、raw-float容差或部分集合降级接受。

10次 wrong-LUID negative全部返回 `AdapterMismatch`，caller submission和 submitted count不变；25个成功 frame全部有非零 GPU timestamp、261,040-byte compact readback和0 raw-pixel readback。每次 run的 pending/failed/cancelled为0、device removed reason=`S_OK`、shutdown完成。

### 20.3 Sealed evidence 与 truth boundary

Release create-only Gate为1/1 PASS，本次观察为46,733 assertions、50.810秒；ASan专用 adapter matrix为1/1 PASS、121.16秒；Release默认 `PBRemoteVisualTests` + 非隐藏 `PBDemodD3D11Tests`为2/2 PASS。evidence目录为 `build-p1_5-evidence/20260902-011943-step11-gpu-parity-final`：

```text
0c56409d47f2cc4dd730f40f786dfc7e72fa24488581a839a63afefb8aa23fed  gate.log
168bbcf20b59eb85ab4f52fc2c1b054fd9a9f66ac5e6bbeb56bc634870d0aadc  evidence.json
```

封存包含6条 availability、25条 result、10条 run和1条 summary，共42条结构化记录。覆盖写入负测在执行 GPU Gate前拒绝，已有artifact hash不变。

Step 11证明当前 adapter/driver矩阵上的 experimental LF4 GPU protocol truth，不证明未来未测试硬件。后续Step 12已按第21节关闭同observation locator/Bootstrap/geometry、PB-owned WGC/DXGI normalized ROI lease、CaptureEpoch drain/recreate和bounded staged completion；Step 13/14又分别按第22/23节关闭temporal admission与hidden production Receiver/Outer/file publish。这些后续结果不追溯扩大Step-11 adapter evidence本身，也仍不证明GUI/CLI exposure、Replay v2 production一致性、VerifiedEncodedGoodput或Certified Profile。

---

## 21. Step 12 CaptureNormalize / geometry / epoch / lifetime 完成证据

完整staged-completion contract、same-observation LF4 pipeline、resource math、WGC/DXGI integration、exception/fanout semantics、命令与限制见`REMOTE_VISUAL_STEP12_CAPTURE_LIFETIME.md`。

### 21.1 单一CaptureNormalize路径与有界continuation

- `RawRoiConsumer`与`ScreenCaptureConsumer`新增向后兼容的`CompleteStage`；legacy `Completed`默认返回无GPU continuation，既有consumer无需改变；
- 首个marker退休Bootstrap staging后，LF4 consumer可在同一owner context、同一PB-owned ROI texture上提交一次GPU data stage；runtime记录第二marker并继续持有slot/source lease；
- cancelled callback只有metadata，texture/context均为null；第二continuation在其work安全退休后fail closed并terminal cancel，不形成无界链；
- callback抛异常时无法证明尚未提交GPU work，因此D3dRoiRing保守`ClearState`并记录marker，避免post-submit use-after-lease；
- snapshot以submission/completion/rejection三个saturating counters记录continuation authority。

### 21.2 Bound Bootstrap、geometry 与resource admission

- `LocalDesktopBootstrapBinding` overload只解析冻结LF4 identity，并复用同一continuous locator；错误/未知binding产生`UnsupportedRecord` erasure；
- LF4 `CaptureDemodulator`先读同一observation的Bootstrap staging，再以得到的canonical record与continuous geometry提交现有`SubmitRemoteVisualLowFps`；adapter LUID/device/context/texture/domain/epoch/slot/generation均逐项绑定；
- `ExactCanvas`、`Scaled`、`Letterboxed`、`Rejected`进入result和snapshot；1800x1080 crop只产生`TelemetryOnly/Rejected`，data demod submission与accepted Transport均为0；
- LF4 hard ROI默认3840x2160，Bootstrap staging按`maximumRoiWidth * maximumRoiHeight * 4 * slotCount`在capture allocation前预留；0维、D3D上限、NaN policy、budget少1 byte、actual ROI越界与wrong adapter均原子拒绝；
- data demod仍只回读261,040 bytes、raw-pixel readback=0；Bootstrap locator当前明确进行一次完整ROI CPU staging readback并单独计数，不能把两者混为“整个pipeline零全帧readback”。

### 21.3 WGC/DXGI、epoch 与truth endpoint

完整integration使用现有WGC/DXGI facade、production CaptureRuntime/NormalizeConsumer/D3dRoiRing和真实WARP device；测试backend只注入immutable source surface。WGC upright 1920x1080与DXGI raw 1080x1920 ROTATE90均归一化为同一upright ROI。每个backend在epoch 1得到4个与CPU reference byte-identical的accepted Transport，随后`RequestRecreate`使旧domain drain/invalidate并进入epoch 2，再得到同一4-block truth。

两epoch结束时每backend continuation submissions/completions/rejections为2/2/0，`frameLeaseHighWater=1`；stop后`busyRoiTextures=0`、`liveFrameLeases=0`、`queuedFrames=0`，capture/demod/result pending均为0且无deferred cleanup。额外negative覆盖wrong texture/context/device identity、canceled non-null objects、post-GPU-submit exception、second continuation、stale epoch与legacy completion。

### 21.4 Optional diagnostic fanout 与验证

product runtime原有optional diagnostic fanout被抽为可测试内部组件；primary仍是唯一capture/protocol authority。diagnostic branch完整计费且最多8个pending identity，只能延长一次ROI marker；其domain/submit/completion failure或exception只进入diagnostic status与一次null-object cancellation，不能覆盖primary truth或发布payload。

Targeted Release结果：staged normalize 73 assertions/2 cases、post-submit exception 22/1、fanout 126/3、LF4 capture demod 89,334/4、full pipeline staged cases 1,077/3，全部PASS。affected Release 9-suite为9/9 PASS、173.34秒；同一ASan 9-suite为9/9 PASS、421.45秒，其中完整17-case WARP `PBDemodD3D11Tests`为300.47秒且无sanitizer finding。instrumented tree的CTest timeout按实测有界设为420秒，Release仍为180秒，断言与失败条件没有放宽。

### 21.5 Truth boundary 与下一步

Step 12证明library-level LF4 capture lifetime与same-observation accepted-Transport truth；后续Step 13已按第22节收口duplicate/gap/reorder/stall与bounded queue，Step 14再按第23节让admitted blocks进入production Receiver/Outer并闭合WholeFileDigest/final publish，Step 15/16又分别关闭Replay一致性与权威report，Step 17关闭公开GUI/CLI exposure和屏幕安全。它们仍未证明真实provider双机文件恢复、goodput或Certified Profile。

---

## 22. Step 13 temporal admission / bounded queue 完成证据

完整identity state machine、bounded retry、queue/stall contract、deterministic corpus、命令与限制见`REMOTE_VISUAL_STEP13_TEMPORAL_ADMISSION.md`。

### 22.1 Fixed-state visual identity

- `PBModulation::VisualIdentityTracker`以`CaptureEpoch + optional SessionTag + FrameSequence`区分unique、duplicate、reordered与gap，不分配、不保存历史帧；epoch/session变化建立新baseline但保留累计run counters；
- LF4 `CaptureDemodulator`进一步绑定canonical 44-byte Bootstrap，并只保留一个当前temporal frame、4-bit accepted slot mask、最多四个fixed Transport blocks和一个Control block；
- reordered、terminal duplicate、identity不完全一致或evaluation already pending都在Bootstrap后变成`TelemetryOnly`，不提交GPU；
- unique FPS只由相邻sequence和正向时间interval组成，gap按distance精确累计skipped，duplicate/reorder不污染分母。

### 22.2 Bounded refinement 与admitted-index contract

默认每sequence只允许一次duplicate refinement，配置0..4；attempt无论是否恢复carrier都会消费额度。completion在固定candidate副本上事务验证：旧slot相同则幂等忽略，旧slot冲突、Control/Transport混合、Control copy冲突或越界均fail closed；只有此前缺失slot写入admitted indices。raw observation仍保留完整诊断结果，但LF4 verified/post-FEC frame只由Unique计数。

44-observation WARP corpus包含6 unique、37 duplicate、1 reorder、1 gap/2 skipped，只触发8次GPU submission；admitted Transport=20、verified/post-FEC=4/2。32帧同sequence burst全部suppressed，pending HWM≤2、result HWM=1、drop=0；新unique和epoch recreate继续接受。0..4 codeword matrix和四Control-copy collapse同步通过。

### 22.3 Stall、资源与验证

现有1,000 ms `ChannelStallTracker`把capture停止与capture持续但unique visual停止分开；domain reset关闭活动interval而不清累计证据。capture/ROI/demod/result均保持固定容量，epoch invalidation把旧结果计入stale drop并drain，shutdown后pending/queue/GPU work归零。

Targeted Release的stall、production LF4 Receiver、CPU temporal与WARP temporal用例均通过。最终affected Release 6-suite为6/6 PASS、137.83秒；同一MSVC ASan 6-suite为6/6 PASS、333.03秒，无sanitizer finding。ASan初次发现的测试栈占用已通过把大result改为显式bounded heap ownership修复，生产语义与断言未放宽。

Step 13只决定“哪些carrier可进入Receiver”，不把中间Transport成功提升为文件成功。Step 14继续以WholeFileDigest和final publish为最终authority。

---

## 23. Step 14 four-codeword Receiver / Outer / publish 完成证据

完整admitted-index consumer contract、Control collapse、DirectRepeat/Wirehair production probe、Receiver negatives、命令与truth boundary见`REMOTE_VISUAL_STEP14_RECEIVER_ADMISSION.md`。

### 23.1 Existing production Receiver only

- `ReceiverPipeline`要求LF4 result携带temporal disposition；suppressed/reordered/stale结果零mutation，duplicate refinement只有新admitted carrier才继续；
- `ProcessTransport`分别验证raw count、admitted count与每个index的范围/唯一性，只对admitted blocks重新执行canonical Transport parse、SessionTag与payload bounds，再调用原有`ReceiverIngress::ReceiveDataBlock`；
- strict旧profile仍把全部raw accepted entries映射为admitted entries，wire identity和旧行为不变；
- 四个LF4 Control Robust copy在底层固定数组中完整承载并要求byte-identical，随后折叠成一次production Control mutation；冲突/mixed carrier fail closed。

### 23.2 Authoritative file convergence

hidden headless production probe从`SenderFrameBuilder`完整LF4 raster开始，经ReferenceChannel truth、production ReceiverPipeline/ReceiverIngress、DirectRepeat或Wirehair V2、PBStorage、WholeFileDigest和publish。它在Session Control后注入32个suppressed duplicate，并把首个data frame拆成Unique admitted slots 0/1与同sequence refinement admitted slots 2/3；第二次raw observation含4 blocks，但只有2个新slot进入Receiver。

| mode | source | processed | raw/admitted Transport | Outer unique | final result |
| --- | ---: | ---: | ---: | ---: | --- |
| DirectRepeat | 1 byte | 37 | 6 / 4 | 1 | Completed + digest + publish + external byte exact |
| Wirehair V2 | 4096 bytes | 37 | 6 / 4 | 4 | Completed + digest + publish + external byte exact |

两例均为33个duplicate sequence observations、1个end-to-end unique data sequence、4个production accepted Transport；Outer conflict/resource rejection为0。唯一data observation记录2个FEC failure；refinement恢复其余slots但不增加FER分母。

### 23.3 Failure boundary 与下一步

完整`PBOuterFecTests`/`PBReceiverTests`继续覆盖same-ID conflict、duplicate、orphan、unknown session、resource limits、Wirehair errors/reservation、resume corruption与cleanup。无效probe输入保持output不变；invalid/repeated admitted index在Receiver mutation前拒绝。最终affected Release/ASan 6-suite分别6/6 PASS。

Step 14证明hidden production Receiver/file semantics，不表示公开Decoder已支持LF4，也不是双机field run。后续Step 15已按第24节让Replay v2 LF4 live/offline进入同一production decoder，并保留v1 compatibility、presence/null truth、recorder bounds、corrupt/truncated/oversize/checksum/no-overwrite语义。

---

## 24. Step 15 Replay v2 production live/offline 完成证据

完整schema、presence/null truth、exact-adapter/two-stage lifetime、async recorder资源、测试矩阵和truth boundary见`REMOTE_VISUAL_STEP15_REPLAY_PRODUCTION.md`。

### 24.1 Versioned production observation

Replay file/version/header/footer大小保持不变；历史observation缺少detail presence时reserved bytes仍必须全0。detail version 1只占用既有demod record reserved区，保存实际恢复的layout、连续geometry、temporal/result、FEC/CRC/identity、raw/admitted blocks、metric/freshness、GPU timing presence与Receiver disposition。receiver-only record的sender raster、canonical Bootstrap与sender truth全部absent；false-accepted/compared/error bits不会因默认0而被冒充为测量结果。

offline adapter只把reader验证后的selected ROI建成PB-owned BGRA8 texture，再走原有production `CaptureDemodulator`/`ReceiverPipeline`/`ReceiverIngress`/Outer/PBStorage。Direct/Shape/旧RemoteVisual保持单阶段；LF4用最多两次`CompleteStage`。每阶段使用新的one-shot `Flush1` event并同时注册device removal，避免reset event与移除信号竞争。记录LUID不存在时只允许当前WARP exact LUID相等，不允许different-adapter fallback。

### 24.2 LF4 authoritative publish replay

headless Gate用实际fixed-slot async recorder生成6-frame receiver-only Replay：Session、Manifest、Segment、Data四个unique视觉帧，加两个Session duplicate。capture/demod observation均为6，queue HWM≤2、drop=0；四个unique observation携带LF4 metric summary，两个duplicate为terminal suppressed。live与offline结果如下：

| 项目 | live | offline |
| --- | ---: | ---: |
| observations | 6 | 6 |
| comparison mismatch | N/A | 0 |
| duplicate suppressed | 2 | 2 |
| admitted Transport | 4 | 4 |
| Outer unique | 1 | 1 |
| WholeFileDigest / publish | PASS / PASS | PASS / PASS |
| external output | 1 byte exact | 1 byte exact |

GPU execution duration作为live evidence保留但不要求二次执行数值相同；layout、geometry、temporal、metric/freshness、FEC/CRC/identity、carrier与Receiver result全部精确比较。

### 24.3 Real receiver-only classification

显式只读`PB_REMOTE_VISUAL_REAL_REPLAY_ROOT`指向Step 02 sealed RDP corpus时，production offline Decoder得到：Direct/Shape/LF4均8/8 Bootstrap、7 duplicate、分别42/32/4 admitted Transport，FEC/CRC/identity全0。三者都因capture-only corpus没有Session/Manifest/Segment Control而结束为`Stopped`、descriptor unknown、WholeFileDigest not evaluated、no publish；这与原Inspector分类一致，并且没有把中间Transport成功提升为文件成功。

Release `PBRealCaptureReplayTests`、Step 15 LF4 full-publish Gate与real-corpus Gate均通过；同三项MSVC ASan Gate也无sanitizer finding。最终affected Release 7-suite为7/7 PASS、188.94秒。ASan组合首轮除`PBApplicationTests`外6-suite全部通过，后者只是新增Gate超过历史CTest 60秒外层上限；把保留内部deadline的外层timeout调整为600秒并重新配置Release/ASan后，`PBApplicationTests`在107.73秒PASS，因此7项均有ASan通过证据且没有放宽测试。后续Step 16已按第25节关闭权威telemetry、RunReport.2与strict merger，Step 17又按第26节关闭公开GUI/CLI exposure与屏幕安全。

---

## 25. Step 16 authoritative telemetry、RunReport.2 与严格 merger 完成证据

完整字段定义、分母公式、`null`/0/epoch 语义、双端 artifact seal、验证命令与未认证边界见 `REMOTE_VISUAL_STEP16_TELEMETRY_REPORT.md`。

### 25.1 五类速率不再混用

Encoder 明确记录 LF4 每完整逻辑帧 64,800 raw coded bits、5,400 Inner-FEC information bytes 与 5,256 Transport payload ceiling bytes；1/2/5 Hz 的 5,256/10,512/26,280 bytes/s 只叫理论 ceiling。实际 generation rate、Present rate、Decoder capture/unique rate、FEC/codeword rate和最终 verified goodput 均使用各自的权威分母；Sender 不能观察的 Receiver progress、ETA 与 verified goodput 继续为 `null`。

Decoder 的每个有效 LF4 compact-metric observation 固定贡献 64,800 metrics、16,723 symbols 与 77 freshness regions。只有 temporal `Unique` data frame 增加一次 FEC frame denominator，并固定评估 4 个 codeword；same-sequence refinement 可以增加 signal observation和此前缺失slot的temporal admission，但不重复增加 unique FER/FEC denominator。raw FEC accepted、Receiver-bound temporal admitted 与 Outer unique/duplicate/conflict 分列保存，历史 `acceptedTransportBlocks` 仅作为 temporal admission alias。

没有 sender coded-bit truth 时 `preFecBerEstimate=null`；有非零 comparison denominator 且错误为零时才写数值 0。CaptureEpoch 切换会同时清除 signal、FEC、Bootstrap、capture 与 derived-rate 状态；同序/倒序 observation、非法非有限 metric、分母越界和 counter saturation 均 fail closed。

### 25.2 双端成功证据必须指向 exact artifacts

`PixelBridge.RunReport.2` 分别保存 Encoder capacity/generation/presentation 和 Decoder frozen profile/signal/FEC/temporal/Outer/verified truth。严格 merger 对 LF4 常量、128-bit SessionId、SessionTag、source length/digest、provider metadata、时间窗与全部 numerator/denominator 做独立校验，并重新严格读取、封印两端原始 report 文件，确认传入对象与磁盘 artifact 完全一致。

只合并两个内存对象仍可生成诊断结果，但不能把 `successfulRun` 置为 true。正式成功还必须同时满足 Decoder `Completed`、WholeFileDigest PASS、safe publish PASS、external SHA-256/length exact、两端 journal complete、identity complete、Outer conflict=0 和两端 report seals present。每次 CLI invocation create-only 生成独立 combined JSON、Markdown 与 per-run CSV；已有目标不会被覆盖，跨 run summary CSV 不能代替单次运行 artifact。

### 25.3 自动验证与保留边界

Release `PBTelemetryTests`/`PBApplicationTests` 为 2/2 PASS；最后一次 RunReport basis 字段调整后又单独重建并运行 `PBApplicationTests`，1/1 PASS、27.54秒。显式 Step 02真实Replay条件用例为84 assertions PASS。MSVC ASan对应两套测试2/2 PASS、67.97秒，真实Replay条件用例同为84 assertions PASS且无sanitizer finding。merger单元测试23/23 PASS，`py_compile`和`git diff --check`通过。

上述是Step 16当时的headless验证边界；后续Step 17已在右侧ExperimentMonitor运行no-activate `PBPresentationGate`和public WGC pixel-only full-publish闭环，Step 18又关闭create-only package/source/environment/deployment identity，但仍不证明真实双机goodput、provider matrix、LocalDesktop同提交物理回归、6小时soak或Certified Profile。`certifiedRemoteVisualProfile`继续为`false`；后续Step 19已关闭同提交LocalDesktop回归，Step 20真实双机门禁仍未执行。


---

## 26. Step 17 实现与验证：公开 profile、屏幕安全与 pixel-only 闭环

Step 17 的完整实现、参数契约、QSettings边界、显示器authority、首次闭环失败分析、结果排序修复、自动化命令和artifact seals见`REMOTE_VISUAL_STEP17_GUI_CLI_SCREEN_SAFETY.md`。

### 26.1 GUI/CLI 共享同一 identity

`application_model`现在提供唯一四项profile catalog，GUI与UTF-8/UTF-16 CLI parser都从中取得token、display name和默认值。新增`remote-lf4`只映射`PB-RemoteVisual-LF4-X1`的独立ID/layout；旧`remote`、默认Direct选择和全部wire identities不变。profile不写入QSettings，只有本地目录/backend/ROI/最近ProtectedMonitor等UI偏好允许持久化。

### 26.2 启动前与运行期都 fail closed

LF4 Encoder/Decoder要求RemoteVisual、1..5 Hz和显式不同的Protected/Experiment monitor。Encoder固定1920×1080 client RECT必须完全位于ExperimentMonitor；Decoder live ROI必须为同一identity-rotation显示器且X/Y各在0.5x..2.0x，exact geometry仍由captured-pixel locator决定。worker真正重验成功前report状态保持`Pending`；其后每秒重新枚举identity/physical/work RECT/DPI/rotation/refresh，任何漂移或metadata冲突转为`FAIL`并停止，不移动外部窗口、不使用全局输入。

### 26.3 实际屏幕像素闭环发现并修复异步排序缺陷

第一次public本地闭环在Bootstrap成功后被`PBTelemetry`以`ObservationOrder`正确拒绝且没有publish。根因是较晚duplicate的Bootstrap-only `TelemetryOnly`可早于较早observation的GPU demod完成。`CaptureDemodulator`现在用固定slot `pendingCaptureObservations` barrier，在同一mutex内原子退休barrier并入队结果；`TakeResult`只交付当前最小且不存在更早pending的observation。资源容量和drop语义不变，也没有削弱Telemetry。独立WARP回归明确构造2先完成、1后完成并验证最终交付1→2。

最终Release证据`build-p1_5-evidence/step17-final-local-pixel-loop-20260902-080450`从右屏Data Window经WGC实际捕获像素进入production GPU demod/temporal/Receiver/Outer，得到4个temporal admitted Transport、Outer unique 1、WholeFileDigest PASS、safe publish PASS与external SHA-256/length exact；52个duplicate没有造成reorder，result queue HWM为2。前台PID前后均为63784。

### 26.4 Gate 与保留边界

最终no-activate原生Gate位于`build-p1_5-evidence/step17-final-native-20260902-080514`：两个Carousel cycles、9个source replacements、79个repeat Presents，Protected=`\\.\DISPLAY1`、Experiment=`\\.\DISPLAY2`且foreground PID不变。Release核心6/6、定向parser/model 89/89、merger 24/24和MSVC ASan核心6/6均通过。

这仍只是同机屏幕像素与产品绑定证明；后续Step 18已关闭create-only package/source/environment/deployment identity，但仍没有真实Computer B/A provider链、正式goodput matrix、LocalDesktop同提交物理回归、soak或Certified Profile结论。后续Step 19已关闭同提交LocalDesktop回归；Step 20真实双机门禁仍未执行。

---

## 27. Step 18 实现与验证：package/source/environment/deployment identity

完整schema、工具参数、资源边界、复制流程和truth boundary见`REMOTE_VISUAL_STEP18_DEPLOYMENT_REPRODUCIBILITY.md`。

### 27.1 两端共享一个 Both-role 包

`New-PBRemoteVisualPortablePackage.ps1 -Role Both`不再为A/B分别构造可能漂移的包，而是在同一manifest中封印Encoder/Decoder、全部Qt plugin/DLL/resource、HEAD/tree、tested-source全清单、Phase-1 tag、MSVC/SDK/CMake/Qt/vcpkg ABI和LF4 identity。SPDX 2.3 SBOM与THIRD_PARTY_NOTICES覆盖Qt和全部installed vcpkg package。独立verifier复算目录和ZIP中每个entry，拒绝额外/重复/越界/reparse路径、manifest/seal漂移和tamper；validation完成前不保留正式输出。

### 27.2 source set 与共享 RunId

source generator用Windows OS CSPRNG生成1 MiB、8 MiB RAW和内含单一4 MiB `payload.bin`的ZIP，ZIP中间原始文件在发布前删除。manifest/seal绑定128-bit sourceSetId、outer file hashes、inner payload hash和canonical source fingerprint。另一个CSPRNG 128-bit RunId只存在shared RemoteVisual metadata/evidence，不进入wire或acceptance。

### 27.3 双端 environment 和 deployment 反查

environment collector先验证package/source，并强制`--version`/`--list-monitors`来自shared package内Decoder；随后只读收集bounded OS/CPU/GPU/display/monitor metadata。Encoder/Decoder两份schema 2 artifact分别自封印完整持久化JSON，并都绑定相同package/source/metadata SHA。deployment manifest再要求两角色恰好各一份，以logical fingerprint和全部磁盘artifact hash建立RunId→package/source/metadata/endpoints唯一反查。

### 27.4 adversarial Gate 与保留边界

Windows CTest覆盖八脚本parser closure、source generator/verifier、ZIP inner identity、single-byte tamper、错误expected manifest hash、create-only source/metadata/verification和partial cleanup。正式clean Release Gate另外从同一ZIP解压两个干净endpoint目录，要求manifest SHA逐字节一致、packaged CLI只读探测成功，并以DLL single-byte mutation验证package verifier非零且不发布verification artifact；exact输出封存在ignored `build-p1_5-evidence/step18-*`目录。

Step 18没有捕获或发送像素，也不产生Receiver、WholeFileDigest、safe publish、goodput或field结论。本机双endpoint证据只关闭工具与identity链；真实Computer A/B身份和远控像素链仍必须由Step 20正式pilot记录。后续Step 19已用同一tested-source包关闭LocalDesktop四组合回归；Step 20仍必须记录真实Computer A/B身份和远控像素链。

---

## 28. Step 19 同提交 LocalDesktop 回归完成证据

完整输入身份、harness契约、失败轮诊断、四组合结果、资源口径、独立验证命令与artifact hashes见`REMOTE_VISUAL_STEP19_LOCALDESKTOP_REGRESSION.md`。

### 28.1 同一 oracle、同一 tested-source

正式矩阵以Step 18 package manifest SHA-256=`ab1f511be7f46a0b2be590fc618f7085e79d925bf4a2b7bb11bafc501ded2e49`反查同一Both-role package和tested-source fingerprint=`1c0b2ff91ed98b0b1781788a8e6285891aaa224f6d9de7b2d8645150628753bc`。输入继续使用冻结8 MiB RAW，SHA-256=`e02206c8813b212705ce995f49498bfacdd49030f097492a6eb2777ea66a4fe2`；Protected=`\\.\DISPLAY1`、Experiment=`\\.\DISPLAY2`、ROI=`[2880,180]-[4800,1260]`，Replay明确关闭。左屏并发工作记录为`Present`，矩阵前后foreground PID均为63784。

### 28.2 首轮BLOCK揭示的是harness相位偏差

首轮四组均恢复exact文件，但旧harness在Encoder启动3秒后才启动Decoder，使不同profile从任意Carousel中段进入；WGC/Shape与DXGI/Shape因此表观回退10.25%和19.32%，该轮保持`BLOCKED`。正式策略改为Decoder先启动、固定250 ms后启动Encoder，并把冷启动/采集时间留在权威goodput窗口内。每组journal同时证明descriptor已知后的resource rejection增量为0；非零的predescriptor bounded-orphan拒绝没有被隐藏或误判为冲突。

### 28.3 四组合文件与性能出口

| Capture/Profile | Baseline Mbit/s | Post Mbit/s | Change | FER | UniqueVisualFPS | 文件真值 |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| WGC / Direct | 9.467 | 10.495 | +10.86% | 0 | 58.978 | digest/publish/hash PASS |
| DXGI / Direct | 9.633 | 10.597 | +10.01% | 0 | 60.003 | digest/publish/hash PASS |
| WGC / Shape | 13.665 | 16.324 | +19.46% | 0 | 59.162 | digest/publish/hash PASS |
| DXGI / Shape | 14.635 | 17.740 | +21.22% | 0 | 60.005 | digest/publish/hash PASS |

四组Decoder完成时Encoder仍广播；Outer conflict均为0；frame lease、demod pending与result queue HWM均在固定容量内。历史baseline没有UniqueVisualFPS、GPU或memory HWM，比较表保持`N/A`而不从其他指标重构。Post GPU/memory、CPU equivalent cores、stale-drop解释和全部资源数值保存在正式summary及专项文档。

### 28.4 Seal、独立验证与保留边界

正式证据根为`build-p1_5-evidence/step19-final-localdesktop-20260902-095359`；`post-summary.json` SHA-256=`0f61eb9d8607f5ba9f408b42f48daf9068678f4e8492c8996f6a90cd552ac07b`，48-artifact `evidence-seal.json` SHA-256=`4588177999c5cf01794bd280cd21e3404ffe7cfa50a01570f660c94b00b653aa`，seal外独立verification SHA-256=`c204b940580a60840bd9c7eddfdc486af64a63aad4df28d6dc1260cc3d703bc1`。定向Golden/Transport/Receiver/证据契约为7/7 PASS，重新configure后的完整Release headless CTest为200/200 PASS、0失败。

这仍是同机LocalDesktop回归，不证明远控provider传输了任何像素。Step 20结束前必须再次执行真实Computer B Encoder → 实际远控桌面/视频链路 → Computer A WGC Decoder → WholeFileDigest/safe publish/external SHA-256/Replay offline reproduce完整工作流；本地、offline或模拟结果不能替代。
