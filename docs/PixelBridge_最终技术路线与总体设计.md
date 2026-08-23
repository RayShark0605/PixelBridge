# PixelBridge：高性能屏幕区域视觉流文件传输系统——最终技术路线与总体设计

> 项目名称：**PixelBridge**  
> 文档类型：项目愿景 + 协议规范草案 + 总体架构设计 + 工程实施路线  
> 目标平台：Windows x64 / C++20  
> 目标程序：`PixelBridgeEncoder.exe`、`PixelBridgeDecoder.exe`  
> 核心输入模型：Decoder 对用户指定的**屏幕矩形区域进行高速桌面捕获**，不使用摄像头  
> 支持模式：**即时视觉流模式**、**离线 MP4 模式**  
> 设计日期：2026-08-21  
> 文档状态：**总体架构可行，可进入 Phase 0～1 原型验证；协议语义可先冻结，LocalDesktop 物理层、Visual Profile 常量与性能承诺必须在 Pixel Round-Trip / Capture / Video Gate Benchmark 通过后冻结**

---

## 目录

1. [总体设计结论与关键架构约束](#1-总体设计结论与关键架构约束)
2. [项目愿景与最终目标](#2-项目愿景与最终目标)
3. [系统边界：PixelBridge 不是什么](#3-系统边界pixelbridge-不是什么)
4. [核心设计原则](#4-核心设计原则)
5. [总体分层与系统架构](#5-总体分层与系统架构)
6. [模块拆分与工程目录](#6-模块拆分与工程目录)
7. [协议设计总原则](#7-协议设计总原则)
8. [Bootstrap 与控制平面](#8-bootstrap-与控制平面)
9. [Session / File Manifest / Segment 协议](#9-session--file-manifest--segment-协议)
10. [任意大文件与 Segment 流式模型](#10-任意大文件与-segment-流式模型)
11. [即时模式的无反馈调度：Segment Carousel](#11-即时模式的无反馈调度segment-carousel)
12. [压缩层](#12-压缩层)
13. [外层纠删码：Wirehair V2](#13-外层纠删码wirehair-v2)
14. [内层纠错码：Soft-Decision QC-LDPC](#14-内层纠错码soft-decision-qc-ldpc)
15. [端到端完整性、安全边界与文件摘要](#15-端到端完整性安全边界与文件摘要)
16. [视觉帧总体结构](#16-视觉帧总体结构)
17. [视觉调制：Video Shape/Chroma 与 LocalDesktop Direct-Level](#17-视觉调制video-shapechroma-与-localdesktop-direct-level)
18. [Visual Symbol Codebook](#18-visual-symbol-codebook)
19. [空间交织、时域相位、扰码与局部坏区](#19-空间交织时域相位扰码与局部坏区)
20. [离线 MP4 技术路线](#20-离线-mp4-技术路线)
21. [Windows 屏幕区域选择与 DPI](#21-windows-屏幕区域选择与-dpi)
22. [Windows 屏幕捕获 Backend 与归一化](#22-windows-屏幕捕获-backend-与归一化)
23. [GPU Adapter 拓扑与 GPU Backend 选择](#23-gpu-adapter-拓扑与-gpu-backend-选择)
24. [D3D11 Compute 与 D3D11 → CUDA 高性能路径](#24-d3d11-compute-与-d3d11--cuda-高性能路径)
25. [HDR / SDR / 色彩管理](#25-hdr--sdr--色彩管理)
26. [Visual Frame 定位、尺度与相位恢复](#26-visual-frame-定位尺度与相位恢复)
27. [软解调、硬判决与 LLR 校准](#27-软解调硬判决与-llr-校准)
28. [桌面帧丢失、重复、混合帧与时序](#28-桌面帧丢失重复混合帧与时序)
29. [Decoder 完整流水线](#29-decoder-完整流水线)
30. [线程模型、队列、同步与内存](#30-线程模型队列同步与内存)
31. [断点恢复与状态持久化](#31-断点恢复与状态持久化)
32. [输入文件一致性与源文件变更防护](#32-输入文件一致性与源文件变更防护)
33. [输出路径与不可信文件名安全](#33-输出路径与不可信文件名安全)
34. [Visual Profile：认证配置与实验配置](#34-visual-profile认证配置与实验配置)
35. [1080p60 理论容量预算与性能目标](#35-1080p60-理论容量预算与性能目标)
36. [链路训练与能力探测](#36-链路训练与能力探测)
37. [日志、遥测与链路质量统计](#37-日志遥测与链路质量统计)
38. [仿真、Golden Vector 与 Benchmark 体系](#38-仿真golden-vector-与-benchmark-体系)
39. [第三方库与依赖](#39-第三方库与依赖)
40. [关键 C++ 接口建议](#40-关键-c-接口建议)
41. [必须重点注意的工程问题清单](#41-必须重点注意的工程问题清单)
42. [分阶段实施路线](#42-分阶段实施路线)
43. [最终验收标准](#43-最终验收标准)
44. [与 libcimbar 的继承关系与根本差异](#44-与-libcimbar-的继承关系与根本差异)
45. [最终系统摘要](#45-最终系统摘要)
46. [主要参考资料](#46-主要参考资料)

---

# 1. 总体设计结论与关键架构约束

## 1.1 总体结论

**PixelBridge 的总体技术路线可行。**

它与 camera-based 的 libcimbar 处在不同的物理信道：PixelBridge 的 Decoder 直接读取 Windows 桌面合成、播放器输出或远程视觉链路最终形成的桌面像素，不再经过镜头、对焦、曝光、白平衡、透视、rolling shutter、camera moiré 等光学退化。因此，在相同 1080p60 画布下，把端到端有效吞吐从 libcimbar 的百 KB/s 量级提升到 MiB/s 量级具有现实基础。

但 PixelBridge 不把所有信道强行归入同一种视觉调制。v1 从一开始就采用“**上层协议统一、物理层按 ChannelClass 分化**”的架构：

```text
Arbitrary-size File
→ Segment
→ Zstd / RAW
→ Outer FEC: Wirehair V2 / DirectRepeat
→ Transport Block + CRC32C
→ Inner FEC Profile
→ Frame Packing / Temporal-Spatial Interleave
→ Visual Modulation
→ D3D11 Presentation / H.264 MP4
→ Windows Desktop Pixels
→ WGC / Desktop Duplication
→ Geometry + Pilot Calibration
→ Soft/Hard Demodulation
→ Inner FEC Decode
→ Outer FEC Recovery
→ Segment Verify / Decompress
→ Whole-file Verify
```

其中：

- `PB-Channel-LocalVideo-1` 的主线候选是 **Luma Shape + Chroma + calibrated soft QC-LDPC**；
- `PB-Channel-LocalDesktop-1` 不预先认定 Shape 是最优物理层，优先让 `PB-Mod-DesktopLevels-X` 与 Shape 基线竞争；如果 Pixel Round-Trip 证明 1:1 桌面链路接近 byte-exact，则优先采用直接量化 level/constellation，必要时进一步评估 CRC + Fountain 的 erasure-only 路线；
- `PB-DesktopFast-1` 保留为 Shape+Chroma 的保守性能基线，不在 Direct-Level Gate 通过前作为 LocalDesktop 的最终 Certified 物理层；
- 每个 Data Frame 都必须有 Profile-defined 的 Sync / Bootstrap / Control / Pilot 保留区；其几何开销在 Profile 冻结前进入精确容量预算；
- 空间交织必须允许随 `FrameSequence` 变化的确定性 phase，以避免通知栏、播放器控件、鼠标残留或固定坏区持续打击同一 codeword；
- 即时模式的 Segment Carousel 支持有界的跨 Segment temporal striping，避免一次短时 capture stall 让单个 Segment 整个 burst 丢失后被迫等待完整 Carousel；
- CUDA 只是 Same-NVIDIA-Adapter 条件下的可选优化，跨厂商 GPU 基线是 D3D11 Compute；
- 所有吞吐数字在真实 Present / Capture / Player / GPU / DPI / HDR / Scale / Pilot-overhead 矩阵通过前都属于目标或参考上限，不是硬保证。

---

## 1.2 ChannelClass 是性能与可靠性承诺的边界

PixelBridge v1 至少区分：

```text
PB-Channel-LocalDesktop-1
PB-Channel-LocalVideo-1
PB-Channel-RemoteVisual-X
```

`LocalDesktop` 指本机 DWM/显示管线到本机 WGC/DXGI Capture；`LocalVideo` 指本地播放器解码 MP4 后再通过屏幕像素捕获；RDP、VDI、KVM、视频会议、云桌面、远程串流等额外经过编码/缩放/色彩转换的链路统一属于 `RemoteVisual`。

任何 LocalDesktop 的 benchmark 结果都不得直接外推到 RemoteVisual。若 PixelBridge 的最终产品目标是跨机器或跨安全域传输，那么 `RemoteVisual` 必须拥有独立的信道仿真、真实设备矩阵和 Certified Profile；LocalDesktop 只能证明 PixelBridge 协议与 Windows 像素链路本身成立。

同时，Encoder 与 Decoder 位于同一个 Windows Session 时，PixelBridge 是一种**人为限定“有效载荷只能经可见桌面像素传递”的软件协议**，而不是物理 air-gap，也不是 OS 强安全隔离机制。

---

## 1.3 Session 内固定 Data Visual Profile

PixelBridge v1 的 Data Plane 在一个 Session 内固定：

```text
ChannelClass
VisualProfileId
SignalProfileId
ModulationFamilyId
InnerFecProfileId
OuterFec policy
```

Bootstrap 每帧仍携带 `VisualProfileId` 用于自描述和一致性检查，但 v1 不允许发送端在同一 Session 中临时从 6 bit/cell 切成 4 bit/cell、改变 LDPC K、改变 Fountain block size 或切换调制族。

用户改变速度/可靠性档位时创建新的 `SessionId`。这样可避免“同一个 Segment 的 Wirehair packet size 与当前 LDPC 信息块容量不匹配”之类非常难调试的跨 Profile 状态污染。

---

## 1.4 任意大文件采用 Segment 化，不把整个文件放进一个 Fountain Message

默认 Source Segment 目标：

```text
8 MiB
```

允许范围：

```text
2 ~ 32 MiB
```

实际值由：

```text
Wirehair block count
RAM budget
codec create/recover time
CPU/disk throughput
```

共同约束。

总文件大小只影响 Segment 数量和物理传输时间，不使内存复杂度退化为 `O(total file size)`。

---

## 1.5 即时模式采用 Segment Carousel，并明确 Late-Join 语义

PixelBridge 是单向、无程序级 ACK 的链路，因此即时模式不能只发送一次 Segment 0..N 后永久释放。

调度采用：

```text
Initial Pass
    Segment 0 → ... → Segment N

Full Repair Pass 1
    Segment 0(new repair IDs) → ... → Segment N

Full Repair Pass 2
    ...
```

对 `WirehairV2`，后续 repair pass 使用新的 `OuterBlockId` / Wirehair block ID；对 `DirectRepeat`，重复少量直接块。

为了让“晚加入”具有可计算的上界，Certified `FullRepairPass` 必须定义每个 Segment 的 `RepairPassSymbolBudget`。如果一个所谓 repair pass 只发送很小比例的 K，则 Decoder 晚加入后可能需要等待很多个全文件周期，不能把这种调度宣传为“一轮可恢复”。

UI 必须显示：

```text
CarouselCycleTime
EstimatedWorstLateJoinTime
CurrentPass
CurrentSegment
```

---

## 1.6 Tiny / Highly-Compressible Segment 使用 DirectRepeat

Wirehair V2 的：

```text
K = ceil(messageBytes / blockBytes)
```

要求合法 block count。0 B、1 B、最后一个极小 Segment 或高度压缩后的 Segment 可能不适合创建 Fountain codec。

因此 `OuterFecMode` 固定支持：

```text
WirehairV2
DirectRepeat
```

小 Segment 直接切为少量 `OuterBlockId = DirectBlockOrdinal` 的 Transport Block，由内层 FEC + CRC 保护并周期重复，避免为了满足 Fountain 维度而无意义 padding。

---

## 1.7 Wirehair V2 必须使用 canonical descriptor，并保持同一 Segment 的方程身份稳定

PixelBridge v1 的正常 Segment 默认使用：

```text
WIREHAIR_V2_PROFILE_CERTIFIED_2026_07
ProfileId = 0x4b295bbb47f4f9c9
```

每个 `SegmentDescriptor` 保存 canonical 32-byte Wirehair V2 descriptor。后续 Carousel 重新创建 encoder 时，必须用**第一次保存的 descriptor**和完全一致的 Encoded Segment bytes 调用 profile-based create 路径，不重新“选择 CURRENT/profile/seed”。

必须验证：

```text
2 <= K <= 64000
serialized message_bytes == EncodedSize
serialized block_bytes == OuterBlockBytes
```

`WIREHAIR_V2_PROFILE_MIXED_2026_07` 与 `MIXED_MIX2_2026_07` 只作为实验候选，并且要求 `blockBytes` 为偶数。因此当前 1449 B / 1629 B 的 Balanced/Fast Fountain payload 不能原样用于 mixed profile；若测试 mixed profile，必须使用独立的、偶数字节 block budget，例如在保持其它 framing 不变时先评估 1448 B / 1628 B。

Wirehair 可以生成大量 repair IDs，但 Decoder 仍可能因内部额外行/accepted-ID 资源限制返回 `WirehairV2_ExtraInsufficient`。接收端必须把它当成显式 Segment-level failure/decoder-reset 事件记录和处理，而不是假设 Fountain Decoder 可无限接收新 ID。

---

## 1.8 内层 FEC 以 soft QC-LDPC 为主，但 LocalDesktop 允许早期验证 Erasure-Only 路线

Video/缩放信道的典型问题是 bit error，因此主线采用：

```text
soft metric
→ calibrated LLR
→ DVB-S2 Short QC-LDPC
```

LocalDesktop 1:1 像素链路可能比视频链路干净得多。`PB-DesktopNative-X` 应在 Phase 1～2 同时测试：

```text
QC-LDPC Fast/High-rate
vs
CRC + Wirehair erasure-only experimental path
```

只有真实 Post-Capture BER/FER 证明 bit error 已经低到“CRC 失败直接当 erasure”更划算时，才允许跳过 LDPC；VideoSafe/VideoBalanced 不采用这条捷径。

---

## 1.9 Control Plane 与 Data Plane 完全解耦

v1 固定：

```text
PB-Bootstrap-1
PB-Control-1
```

它们不依赖 Data Shape 数量、Chroma 状态、Data LDPC rate 或实验 codebook。

Control Record 采用有界长度、显式分片/重组、强 FEC 和 CRC。相同 Session/Segment 键出现两个内容不同但都通过 Control FEC/CRC 的 Descriptor 时，接收端必须进入 `DescriptorConflict`，不得采用“后到覆盖先到”的策略。

---

## 1.10 LocalDesktop 的发送端 Present 路径属于通信链路的一部分

Encoder 的 D3D11 数据窗口不是普通 UI 画布，而是“发送器物理层”。Certified LocalDesktop 路径要求：

```text
Flip-model swap chain
1:1 physical-pixel client area
no MSAA
no alpha blending
no texture filtering
point/texel-aligned rasterization
VSync-paced Present
no tearing in Certified profile
```

推荐使用 frame-latency waitable object 控制 present queue，并记录 Present statistics。`DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING` 只允许作为实验项，因为部分刷新/撕裂会让一张捕获图包含两个不同 `FrameSequence`。

Bootstrap 在至少两个空间分离位置重复 `FrameSequence`，二者不一致时直接把该 Capture Frame 判为 mixed/torn erasure。

---

## 1.11 Windows Capture 采用 Adapter-aware D3D11 Compute 基线

Capture 主线：

```text
WGC / Desktop Duplication
→ Capture Normalization
→ PixelBridge-owned ROI texture
→ D3D11 Compute
→ compact soft metric / LLR
```

CUDA 只有在 Capture Adapter 本身映射到 CUDA-compatible NVIDIA Device，并且 benchmark 明显优于 D3D11 Compute 时才启用。

WGC `Direct3D11CaptureFrame` 是 frame-pool lease。上层不得在 frame 归还后继续持有 `frame.Surface`。Capture callback 只创建有界 `FrameLease`；D3D submission owner 完成 ROI copy 后，通过 fence/query 确认源资源不再被 GPU 使用，再归还 frame。队列满时优先丢旧 Capture Frame，不允许用无限持有 WGC frame 的方式积累延迟。

---

## 1.12 SDR 信号空间必须规范化，而不能只写“Luma + Chroma”

v1 至少定义两个 Signal Profile：

```text
PB-Signal-DesktopSRGB-1
    RGB full-range / sRGB-like transfer / BT.709 primaries

PB-Signal-Video709Limited-1
    8-bit NV12 / BT.709 / studio range / fixed chroma siting
```

Visual Profile 必须冻结：

```text
DataGridOriginX/Y
CellPitch
signal code values / constellation
range
chroma siting
```

Video profile 的 DataGrid origin 与 CellPitch 都必须满足 4:2:0 对齐约束；constellation 需要保留 range guard band，避免使用接近 clipping 端点的极端 Y/Cb/Cr 值。

---

## 1.13 离线 MP4 是“数据视频”，评价标准是 BER/FER 和 Artifact Expansion

默认候选：

```text
MP4 + H.264 + 8-bit NV12 + CFR + SDR BT.709
All-Intra / B-frame=0 / CQP
```

但必须同时约束：

```text
MaxEncodedBitrate
PeakFrameBytes
DecoderCompatibility
OfflineArtifactExpansionRatio = MP4 bytes / encoded payload bytes
```

AQ、lookahead、psycho-visual 优化默认关闭。H.264 deblocking 不预设“开一定好”或“关一定好”，必须作为 benchmark 变量，最后在 Certified Offline Profile 中冻结。H.264 profile、level、VUI、range、chroma siting、IDR cadence、PTS/CFR 语义同样进入 bitstream inspector Gate。

MP4 循环只会再次播放相同 Visual Frame，不会自动产生新的 Fountain 方程。因此离线文件本身必须预先包含足够的 unique repair IDs。

---

## 1.14 完整性、安全与资源安全分层处理

数据正确性至少包含：

```text
Transport CRC32C
Encoded Segment Digest
Raw Segment Digest
WholeFileDigest = BLAKE3-256
```

WholeFileDigest 与数据来自同一未认证视觉链路时，只能证明强完整性，不证明发送者身份。需要身份真实性时再启用 Ed25519/MAC Secure Profile。

视觉输入视为不可信输入。Decoder 必须有统一 `ReceiverResourcePolicy`，限制：

```text
MaxAcceptedFileBytes
MaxSegmentCount
MaxConcurrentSessions
MaxActiveOuterFecDecoders
MaxOrphanBytes
MaxControlReassemblyBytes
MaxResumeBytes
MaxZstdWindowBytes
```

任何 Descriptor 都不得在通过 Bootstrap/Control 校验和资源策略之前触发大内存分配、巨大文件预分配或 codec 创建。

---

## 1.15 Verified Goodput 的上限必须按真实 payload budget 计算

对当前 Shape 候选 DataGrid，扣除每 codeword 36 B Transport framing 后：

| Profile | Fountain payload / frame | 60 FPS 候选 payload 上限 |
|---|---:|---:|
| PB-VideoSafe-1 | 13,140 B | **0.752 MiB/s** |
| PB-VideoBalanced-1 | 26,082 B | **1.492 MiB/s** |
| PB-DesktopFast-1 | 65,160 B | **3.728 MiB/s** |

这些数字的前提是最终 Sync / Bootstrap / Control / Pilot / Guard region map 能让当前 DataGrid 保持不变；真正的 `VerifiedEncodedGoodput` 还会损失于：

- Capture duplicate/drop；
- Inner-FEC failure；
- Fountain repair overhead；
- Carousel / temporal striping 调度；
- resource stall；
- Final verification latency 的用户端完成时间。

因此 `PB-DesktopFast-1 @ 1080p60` 不能把 4.0 MiB/s 写成可认证的 incompressible encoded-goodput 目标；3.728 MiB/s 只属于该 Shape 候选的 payload ceiling，并不是 LocalDesktop ChannelClass 的上限。

`PB-DesktopNative-X` 的 ceiling 必须在 Pixel Round-Trip Gate 后根据真实 `Tile × BitsPerTile × FEC rate` 重新计算。对高度可压缩文件，`VerifiedRawGoodput` 可以高于 encoded-goodput，但必须单独标记 compression gain。

---

## 1.16 Whole-file 验证必须定义 out-of-order Segment 的实现方式

Decoder 可以乱序恢复 Segment 并随机写入 `.part`，但普通 BLAKE3 streaming state 不能直接按任意 Segment 到达顺序得到“原文件顺序”的 whole-file digest。

PixelBridge v1 采用最简单可靠的策略：

```text
所有 Segment 完成并通过 RawDigest
→ 顺序扫描 output.part
→ 计算 WholeFileDigest
→ 与 FinalManifest 比较
→ Atomic Rename
```

这会增加一次顺序磁盘读取，因此必须单独记录 `FinalVerificationLatency`，并把 `Verified Complete` 的时间点放在最终 whole-file digest 通过之后。

---

## 1.17 LocalDesktop 的 Direct-Level 调制必须作为第一等候选提前做 Gate

`PB-Channel-LocalDesktop-1` 的核心信道不是摄像头，而是：

```text
D3D11 Sender Surface
→ DWM / Display Color Pipeline
→ WGC / Desktop Duplication
→ D3D11 Capture Texture
```

在 1:1 physical-pixel、SDR、无缩放、无覆盖的条件下，它可能接近一个确定性的数字像素变换。因此 LocalDesktop 的首要问题不是“能否设计足够好认的 Shape”，而是：

> **经过真实 Windows Present → DWM → Capture 后，发送的离散 level 到底保留了多少可重复、可分离的状态？**

Phase 1 必须同时实现最小可用的：

```text
PB-Mod-ShapeChroma-1
PB-Mod-DesktopLevels-X
```

并以同一显示器、同一 Capture Backend、同一 FPS、同一 FEC/CRC 预算比较：

```text
VerifiedEncodedGoodput
PreFecBER / RawSymbolErrorRate
FER
ByteExactRatio
P01/P001 constellation margin
GPU time
scale sensitivity
color-management sensitivity
capture backend consistency
```

如果 Direct-Level 显著胜出，则 LocalDesktop 的 Certified v1 物理层以它为主；Shape+Chroma 作为兼容/鲁棒 fallback，并继续承担 LocalVideo 主线。

---

## 1.18 LocalDesktop 必须先通过 Pixel Round-Trip Probe

在开始正式 Session 之前，Decoder 可执行固定的 `PB-LinkProbe-1`。Sender 以固定、低密度、协议无关的测试序列显示：

```text
RGB / Luma level ladder
neutral gray ladder
chroma constellation pilots
checker / phase patterns
1×1 / 2×2 / 4×4 tile probes
FrameSequence / timing pattern
```

Decoder 测量：

```text
ByteExactRatio
PerChannelTransferLut
LevelCentroid
LevelVariance
MinimumDecisionMargin
ScaleX / ScaleY
PhaseX / PhaseY
UniqueVisualFPS
Duplicate / mixed-frame ratio
HDR / SDR / range behavior
```

`PB-LinkProbe-1` 不承载文件数据，也不需要反向程序通道。Decoder 只给用户显示“推荐的 Encoder Profile”；用户选择后创建新的 Session。

LocalDesktop 的物理层选择按以下顺序：

```text
byte-exact / extremely-low BER
    → Direct-Level + high-rate FEC or erasure-only experiment

stable quantized transform but not byte-exact
    → coarser Direct-Level + calibrated LLR + QC-LDPC

scale/color transform too unstable
    → Shape+Chroma fallback
```

任何 `PB-DesktopNative-*` Certified Profile 都必须把对应 Probe Gate、显示/捕获矩阵和最低 decision margin 写进认证条件。

---

## 1.19 Frame Overhead、Pilot 与 Burst-Loss 必须进入系统级预算

PixelBridge 的数据网格之外必须显式定义：

```text
Sync Region
Bootstrap Region
Control Region
Pilot / Calibration Region
Guard Region
Data Grid
```

这些区域不得通过“理论上还有一些 margin”隐式假定存在。`VisualProfileId` 必须冻结它们的像素几何、重复策略和容量开销。

同时，即时模式不得把一个 Segment 的全部 K+R block 长时间连续发送。Certified Scheduler 支持：

```text
ActiveSegmentWindow
→ round-robin / striped outer blocks
→ bounded local repair
→ slide window
```

使 100 ms～数秒的 capture stall、窗口覆盖或 display glitch 更接近“多个 Segment 的稀疏 erasure”，而不是“某一个 Segment 的整段 burst 被抹掉”。

`ActiveSegmentWindowSize` 受 `MaxActiveOuterFecDecoders` 和内存预算约束，初始候选 2～8，最终由 benchmark 冻结。

---

# 2. 项目愿景与最终目标

PixelBridge 实现：

> **通过 Windows 屏幕上持续显示的高密度视觉数据流传输任意大小单文件，并由另一个 Windows C++ Decoder 对用户指定屏幕区域进行实时桌面捕获、软解调、FEC 恢复和文件重建的高性能单向视觉通信系统。**

系统由：

```text
PixelBridgeEncoder.exe
PixelBridgeDecoder.exe
```

组成。

两者不通过：

- Socket；
- Pipe；
- Shared Memory；
- COM；
- Clipboard；
- Windows Message；
- 文件共享；
- 隐式 IPC；

交换有效文件内容。

唯一承载文件数据的媒介是：

> **Decoder 实际捕获到的桌面像素。**

---

## 2.1 即时模式

```text
File
↓
Segment
↓
Zstd / Raw
↓
OuterFecMode: Wirehair V2 / DirectRepeat
↓
Transport Block
↓
Inner FEC Profile（QC-LDPC / eligible LocalDesktop Erasure-Only）
↓
Visual Modulation
↓
D3D11 Data Surface
↓
Desktop Pixels
↓
Screen Capture
↓
Decoder
```

特点：

- Encoder 与 Decoder 同时运行；
- 无程序级反向 ACK；
- Encoder 采用 Segment Carousel；
- 用户停止 Encoder 前可持续产生新 repair symbols。

---

## 2.2 离线 MP4 模式

```text
File
↓
Visual Frame Sequence
↓
NV12
↓
H.264 Encoder
↓
MP4
```

播放端可使用普通播放器。

Decoder 只：

```text
框选视频显示区域
→ 捕获屏幕像素
→ 解码
```

不读取 MP4 文件本身。

---

## 2.3 文件大小目标

PixelBridge 协议不把整个文件装入一个 Fountain Message。

```text
File Stream
→ Segment 0
→ Segment 1
→ ...
```

因此不会继承 libcimbar 约 33.55 MB 这类由单个 Fountain message / 小尺寸字段直接造成的限制。

PixelBridge v1 不使用“数学意义上的无限文件”表述。固定协议字段采用：

```text
OriginalFileSize : uint64
RawOffset        : uint64
SegmentOrdinal   : uint64
SegmentCount     : uint64
```

所以 v1 的形式协议上限是 `2^64 - 1` byte 量级；实际可接受大小还受：

- Windows 文件系统/卷限制；
- 本地磁盘空间；
- `ReceiverResourcePolicy`；
- 可接受的物理传输时间；

共同约束。

这已经远高于现实 Windows 单文件需求，并且总内存复杂度保持为 `O(active segment set)`，不随总文件大小线性增长。

如果未来确实需要超过 uint64 的逻辑对象，应通过新的 Protocol Major 定义更宽尺寸语义，而不是在 v1 中引入无必要的 arbitrary-precision 长度字段。

---

---

## 2.4 ChannelClass 与认证范围

PixelBridge 协议上层与视觉/捕获信道解耦。v1 至少定义：

```text
PB-Channel-LocalDesktop-1
PB-Channel-LocalVideo-1
PB-Channel-RemoteVisual-X
```

`ChannelClass` 进入 Visual Profile / benchmark 元数据，但不允许 Decoder 根据一个未知远程链路“猜测自己大概能解”。RemoteVisual 在通过独立的 RDP/VDI/KVM/streaming test matrix 前只能标为 Experimental。

---

# 3. 系统边界：PixelBridge 不是什么

## 3.1 不使用 Camera

Decoder 输入不是摄像头。

不存在：

```text
UVC
Media Foundation Camera
Lens
Focus
Exposure
White Balance
Camera Perspective
Rolling Shutter
Camera Moire
```

---

## 3.2 不保证绕过 Protected Content

DRM / protected video / secure content 可能：

- 无法被 WGC 捕获；
- 无法被 Desktop Duplication 正常返回；
- 得到黑屏/保护图像。

PixelBridge 不绕过任何内容保护机制。

---

## 3.3 默认协议不提供发送者身份认证

默认 PixelBridge v1：

- CRC32C：随机错误检测；
- BLAKE3/SHA-256：端到端内容完整性；

但不默认提供：

- 身份认证；
- 加密；
- 防恶意发送者。

Secure Profile 可作为后续扩展。

---

## 3.4 LocalDesktop 不是物理隔离边界

当 Encoder 与 Decoder 位于同一 Windows 登录 Session 时，“不使用 Socket/Pipe/Shared Memory”是 PixelBridge 自己施加的传输约束，而不是操作系统强制的安全隔离。

因此项目可以声称：

> payload 只通过 Decoder 实际捕获到的桌面像素进入协议解码链路。

但不能把它等同于：

> 物理 air-gap / 强安全域跨域认证。

若未来用于真正隔离域，必须重新定义物理边界、可信显示链路、接收设备与安全认证模型。

---

# 4. 核心设计原则

## 4.1 Goodput 优先

核心 KPI 分为三层：

```text
VerifiedRawGoodput
= 最终通过 WholeFileDigest 校验的原始文件字节 / 传输时间

VerifiedEncodedGoodput
= 已验证 Encoded Segment 字节 / 传输时间

VisualInformationRate
= 成功进入传输层的 LDPC information bytes / 传输时间
```

其中 `VerifiedRawGoodput` 是用户体验指标，`VerifiedEncodedGoodput` 才适合比较视觉信道和 FEC Profile。

不能只看：

```text
raw bits/cell
```

---

## 4.2 Bit Error 与 Erasure 分层处理

### Bit Error

来源：

- H.264 quantization；
- YUV420；
- resize；
- RGB/YUV conversion；
- HDR/SDR；
- player filters；
- subpixel phase。

处理：

```text
Soft Metric
→ LLR
→ QC-LDPC
```

### Erasure

来源：

- dropped Visual Frame；
- duplicate；
- LDPC codeword failure；
- capture interruption；
- player skip。

处理：

```text
Wirehair Fountain
```

---

## 4.3 任何层级都不“猜着往下传”

```text
Visual Confidence
↓
LDPC Syndrome
↓
Transport CRC32C
↓
Wirehair
↓
Encoded Segment Digest/CRC
↓
Decompression Bounds Check
↓
Raw Segment Digest/CRC
↓
Whole File Digest
```

失败的数据不得污染下一层状态。

---

## 4.4 协议必须显式、自描述、可版本化

禁止依赖：

> “Encoder 和 Decoder 恰好链接了同样版本的第三方库，因此应该能解。”

所有影响 wire compatibility 的参数必须：

- 序列化；
- 有 Profile ID；
- 有版本号；
- 有 Golden Vector。

---

# 5. 总体分层与系统架构

```text
L8  File / Final Manifest
L7  Segment / Resume
L6  Outer FEC: Wirehair V2 / DirectRepeat
L5  Transport Block + CRC
L4  Inner FEC Profile: Soft QC-LDPC / Experimental Erasure-Only
L3  Frame Packing / Spatial+Temporal Interleave
L2  Visual Modulation / Bootstrap / Control / Pilot Calibration
L1  Desktop Video Pixel Channel
```

Encoder：

```text
FileSource
  ↓
Segmenter
  ↓
Compression
  ↓
Outer FEC (Wirehair V2 / DirectRepeat)
  ↓
Transport Packetizer
  ↓
Inner FEC Encode（Profile dispatch）
  ↓
Spatial Interleave Phase
  ↓
Inter-Segment Temporal Scheduler
  ↓
Visual Modulator
  ↓
FrameBuilder
  ↓
┌────────────────────┬───────────────────┐
│ D3DRealtimeSink    │ Mp4VideoSink      │
└────────────────────┴───────────────────┘
```

Decoder：

```text
Region Selector
  ↓
WGC / Desktop Duplication
  ↓
Adapter-aware D3D11 ROI
  ↓
Bootstrap + Geometry
  ↓
Pilot Calibration / Reliability Map
  ↓
CUDA / D3D11 / CPU Demod
  ↓
Soft Metric / LLR or eligible hard decision
  ↓
Inner FEC Decode（Profile dispatch）
  ↓
CRC32C
  ↓
Outer FEC dispatch (Wirehair V2 / DirectRepeat)
  ↓
Segment Verify / Decompress
  ↓
Storage / Reassembly
  ↓
Whole File Digest
```

---

# 6. 模块拆分与工程目录

## 6.1 核心库

```text
PBCore
PBProtocol
PBCompression
PBOuterFec
PBInnerFec
PBCodebook
PBModulation
PBFrame
PBRenderD3D
PBVideo
PBMp4Mux
PBScreenRegion
PBScreenCapture
PBScreenCaptureWgc
PBScreenCaptureDxgi
PBCaptureNormalize
PBScreenCropD3D
PBDemodReference
PBDemodD3D11
PBDemodCuda
PBPresentTiming
PBLinkProbe
PBCalibration
PBControl
PBReceiver
PBStorage
PBTelemetry
```

---

## 6.2 应用

```text
apps/
├─ PixelBridgeEncoder/
└─ PixelBridgeDecoder/
```

Qt 只负责：

- UI；
- 配置；
- 生命周期；
- 状态展示。

核心库禁止依赖 Qt。

---

## 6.3 工具

```text
tools/
├─ PBCodebookOptimizer/
├─ PBVideoChannelSimulator/
├─ PBFrameGenerator/
├─ PBFrameInspector/
├─ PBProtocolDump/
├─ PBCaptureProbe/
├─ PBPixelRoundTripAnalyzer/
├─ PBPlayerCompatibilityTest/
└─ PBBenchmark/
```

---

## 6.4 推荐目录

```text
PixelBridge/
├─ apps/
├─ libs/
├─ tools/
├─ tests/
├─ fuzz/
├─ benchmarks/
├─ docs/
└─ third_party/
```

---

# 7. 协议设计总原则

PixelBridge v1 采用：

```text
Little Endian
UTF-8
Explicit Serialization
Bounds Checked Parsing
```

禁止：

```cpp
stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
```

直接持久化带 padding / ABI 依赖的 C++ struct。

---

## 7.1 版本维度

至少区分：

```text
ProtocolVersion
BootstrapVersion
VisualLayoutVersion
SignalProfileId
ModulationFamilyId
CodebookId / LevelConstellationId
CalibrationProfileId
InterleaveProfileId
InnerFecProfileId
OuterFecProfileId / Wirehair Descriptor
CompressionProfileId
DigestAlgorithmId
```

---

## 7.2 ID 宽度

推荐：

```text
SessionId      = 128-bit random
SessionTag     = 64-bit compact tag for hot-path blocks
FrameSequence  = uint64
SegmentOrdinal = uint64
OuterBlockId   = uint32   // Wirehair block ID / DirectRepeat ordinal
```

`SessionTag` 必须由 `SessionId` 确定性派生，例如：

```text
SessionTag = Trunc64(BLAKE3("PixelBridge SessionTag v1" || SessionId))
```

Decoder 只有在已经通过 `SessionDescriptor` 建立 `SessionTag → SessionId` 绑定后，才允许 hot-path Data Block 进入该 Session。若活跃 Session 出现 tag collision，必须拒绝歧义映射，而不是猜测。

禁止同一个 Segment 内发生 `OuterBlockId` wrap-around 后复用；达到实现的安全阈值前必须结束 Session 或重新建立新的 Segment generation / Session。

---

## 7.3 Wire Semantic 与 Implementation Tuning 分离

进入 wire compatibility 的只有会改变字节解释、码字或方程的参数。

例如：

```text
Wire semantic:
ProtocolVersion
VisualProfileId
CodebookId
LDPC matrix / bit ordering / puncturing
Wirehair serialized profile
Compression codec/container
Digest algorithm
```

而：

```text
Implementation tuning:
LDPC decoder algorithm
LLR quantization
max iterations
AVX/CUDA/HLSL backend
zstd compression level
thread count
queue depth
```

属于本地实现或 Encoder tuning，不因为它们变化就修改协议。

---

## 7.4 兼容性、Feature Flag 与 SessionId 生成规则

协议版本按 Major / Minor 解释：

```text
ProtocolMajor
ProtocolMinor
```

规则：

1. `ProtocolMajor` 不同：拒绝解析；
2. `ProtocolMajor` 相同、Minor 更高：仅在未知字段/Feature 被标记为 optional 时允许跳过；
3. 未知 mandatory feature：拒绝 Session；
4. 所有 reserved bits / reserved bytes 在当前版本必须为 0；
5. fixed-prefix 之后允许存在有界 length-delimited extension，但未知 extension 的跳过规则必须由 flags 明确说明。

`SessionId` 必须来自操作系统 CSPRNG。Windows 实现推荐：

```text
BCryptGenRandom(..., BCRYPT_USE_SYSTEM_PREFERRED_RNG)
```

禁止使用时间戳、进程 ID、`rand()` 或仅依赖 `std::random_device` 拼接 SessionId。

`SessionTag` 的截断与整数解释必须进入 Golden Vector，例如固定为：

```text
Digest = BLAKE3("PixelBridge SessionTag v1" || SessionId)
SessionTag = little_endian_uint64(Digest[0..7])
```

---

# 8. Bootstrap 与控制平面

Bootstrap 是 PixelBridge 最关键的“先有鸡还是先有蛋”问题。

Decoder 在知道 Data Profile 前，必须先知道：

- 当前是不是 PixelBridge Frame；
- ProtocolVersion；
- VisualProfile；
- FrameSequence；
- SessionTag；
- Layout；
- 控制 epoch。

因此 Bootstrap 不能依赖 Data Profile 自己。

---

## 8.1 固定 PB-Bootstrap-1

v1 必须冻结一个：

> **永远使用固定、低密度、单色、强纠错的 Bootstrap Profile。**

建议设计：

- 只使用 Luma，不使用 Chroma；
- Cell 明显大于 Data Cell；
- 固定同步图案；
- 固定 byte serialization；
- 固定强 FEC；
- 上下或多处重复；
- Bootstrap CRC。

Bootstrap FEC 可在 POC 阶段从成熟 RS/BCH 方案中选定，**冻结后写入协议并提供 Golden Vector**。

不要为了 Data Path 的理论先进性，把 Bootstrap 也做成复杂软 LDPC。

---

## 8.2 BootstrapRecord

逻辑字段至少：

```text
Magic = "PBRG"
BootstrapVersion
ProtocolMajor
ProtocolMinor
VisualLayoutVersion
VisualProfileId
SessionTag
FrameSequence
ControlEpoch
Flags
BootstrapCrc32c
```

---

## 8.3 Control Frame / Control Block

较大的：

- Session Descriptor；
- Segment Descriptor；
- Final Manifest；

不强塞进每帧 Bootstrap。

它们通过：

```text
BlockType = Control
```

进入强健的控制数据通道，并周期重复。

---

## 8.4 固定 `PB-Control-1`

PixelBridge v1 的 Control Plane 必须与 Data Profile 解耦。`PB-Control-1` 固定：

```text
ControlRecordMagic
ControlVersion
ControlRecordType
ControlSequence
RecordBytes
SessionTag
Payload
ControlCrc32c
```

并冻结：

- 最大 RecordBytes；
- 固定强 FEC；
- Luma-only / reserved lane 的视觉映射；
- repetition cadence；
- bit ordering；
- Golden Vector。

未知 `ControlRecordType` 必须按版本规则拒绝或显式跳过，不能让 Data Profile 的实验参数影响控制面的可读性。

---

## 8.5 Control Record 分片、重组与冲突规则

`PB-Control-1` 的单条逻辑 Record 允许跨多个 Control Block 发送，但分片格式必须固定、可界定资源上限。

推荐固定分片前缀：

```text
ControlRecordId    : uint64
FragmentIndex      : uint16
FragmentCount      : uint16
TotalRecordBytes   : uint32
FragmentBytes      : uint16
Flags              : uint16
FragmentPayload
FragmentCrc32c
```

v1 约束：

```text
MaxControlRecordBytes = 64 KiB
FragmentCount >= 1
FragmentIndex < FragmentCount
sum(fragment bytes) == TotalRecordBytes
```

重组器必须设置：

- 最大并发 Record 数；
- 最大总重组内存；
- 过期时间 / sequence window；
- 重复分片幂等；
- 同一 `(RecordId, FragmentIndex)` 内容冲突立即丢弃整条 Record。

Descriptor 是 immutable binding：

```text
(SessionId) -> exactly one SessionDescriptor
(SessionTag, SegmentOrdinal) -> exactly one SegmentDescriptor
(SessionId) -> exactly one FinalManifest content
```

相同键重复收到完全相同内容属于正常 repetition；若内容不同但都通过 `PB-Control-1` 的 FEC/CRC，Session 进入 `DescriptorConflict`，停止接收该 Session，禁止 latest-wins。

为了降低 Descriptor-before-data 的等待，每次开始一个 Segment 的数据 burst 前应先重复对应 `SegmentDescriptor`，而不是仅依赖低频全局广播。

---

# 9. Session / File Manifest / Segment 协议

## 9.1 SessionDescriptor

单文件项目中建议至少：

```text
ProtocolMajor
ProtocolMinor
SessionId[16]
FileNameUtf8
OriginalFileSize : uint64
SourceSegmentTargetBytes
SegmentCount : uint64
CompressionProfile
SessionVisualProfileId
DigestAlgorithm
FeatureFlags
DescriptorCrc32c
```

`OriginalFileSize` 对 PixelBridge 的单文件场景应当是**必填**，而不是 optional。

它只用于：

- UI；
- 进度；
- 输出预分配；
- 边界校验；

不会成为实际文件大小上限。

---

## 9.2 SegmentDescriptor

```text
SessionTag
SegmentOrdinal
RawOffset
RawSize
EncodedSize
CompressionCodec
OuterFecMode            // WirehairV2 / DirectRepeat
OuterBlockBytes         // Wirehair blockBytes / DirectRepeat payload capacity
RawDigest
EncodedDigest
WirehairV2SerializedProfile[32] // 仅 WirehairV2 时存在/有效
Flags
DescriptorCrc32c
```

注意：

> Wirehair V2 32-byte descriptor 本身只定义 Fountain 方程与 message dimensions，不承担 PixelBridge 文件真实性。

---

## 9.3 FinalManifest

Whole-file digest 可能直到第一轮完整读完源文件才知道。

字段如下：

```text
FinalManifest
{
    SessionId;
    OriginalFileSize;
    SegmentCount;
    WholeFileDigest;
    DigestAlgorithm;
    OptionalSignature;
}
```

即时模式：

- 第一轮末尾发布；
- 后续 Carousel Pass 开头和周期位置重复。

离线 MP4：

- 在序列中周期嵌入；
- 尾部重复；
- 后续 Repair Pass 继续重复。

---

## 9.4 Transport Block 固定头

Hot Path 不建议每块重复完整 128-bit SessionId。

推荐逻辑布局：

```text
BlockType          1 byte
ProtocolMinor      1 byte
Flags              2 byte
SessionTag         8 byte
SegmentOrdinal     8 byte
OuterBlockId       4 byte
PayloadBytes       2 byte
Reserved           2 byte
HeaderCrc32c       4 byte
Payload            N byte
PayloadCrc32c      4 byte
```

固定头约 32 byte，尾 CRC 4 byte。

实际字段大小在 POC 后冻结，但原则是：

- hot path 固定长度；
- 易 SIMD / 易 parser；
- 无 VarInt 恶意超长问题；
- 所有长度在读 payload 前先 bounds check。

在当前 Phase-0 provisional layout 中，`PayloadBytes` 已选为 `uint16`，因此所有
sender/receiver/本地 policy 入口必须共同约束：

```text
1 <= OuterBlockBytes <= 65535
```

放宽这个上限属于 wire layout 变更，不能只提高本地资源 policy。

每个 Transport Block 最终进入固定大小的 Inner-FEC information block。`PayloadBytes` 小于 `OuterBlockBytes` 时：

```text
Header
Payload[PayloadBytes]
PayloadCrc32c
ZeroPadding...
```

未使用 information bytes 必须编码为 0；`PayloadCrc32c` 只覆盖真实 payload，Decoder 同时要求 padding 为 0。这样 short systematic block / DirectRepeat tail 不存在“未初始化字节也参与码字”的实现差异。

`OuterBlockBytes` 必须由已经验证的 `SegmentDescriptor` 与当前固定 `SessionVisualProfileId` 共同确认，Data Block 本身不能要求 Decoder 扩大 buffer。

---

## 9.5 Descriptor-before-data 与 Orphan Block Policy

Data Block 到达时，Decoder 可能还没有收到对应的 `SegmentDescriptor`。此时禁止根据 Data Block 中的未验证字段直接创建大对象、Wirehair codec 或磁盘文件。

v1 推荐：

```text
Known verified SegmentDescriptor
    → accept Data Block

Unknown SegmentDescriptor
    → bounded orphan cache OR drop
```

默认更安全的实现是“小容量 orphan cache + 超限丢弃”，因为 Carousel 会再次给出新 repair IDs。必须记录：

```text
OrphanTransportBlocks
OrphanDropped
DescriptorWaitTime
```

所有 resource allocation 必须由经过验证的固定 Profile 与 SegmentDescriptor 上限共同约束。

---

## 9.6 Segment Map 与 Descriptor 一致性验证

在写入 `.part` 前必须建立并验证 Segment Map：

- `SegmentOrdinal` 唯一且在 `[0, SegmentCount)`；
- `RawOffset + RawSize` 使用 checked arithmetic；
- Segment 区间不得重叠；
- 完成时所有区间必须精确覆盖 `[0, OriginalFileSize)`，不得有 gap；
- `WirehairV2SerializedProfile.message_bytes == EncodedSize`；
- `WirehairV2SerializedProfile.block_bytes == OuterBlockBytes`；
- `DirectRepeat` 的 `DirectBlockCount = ceil(EncodedSize / OuterBlockBytes)` 可由 Descriptor 确定性推导；
- 同一 `SegmentOrdinal` 的 `RawDigest / EncodedDigest / size / offset / OuterFecMode` 不得变化。

`SegmentCount` 本身也受 `ReceiverResourcePolicy` 限制，禁止根据一个未经约束的 `uint64` 直接分配同等大小数组。

---

# 10. 任意大文件与 Segment 流式模型

默认：

```text
SourceSegmentTarget = 8 MiB
```

可配置：

```text
2 ~ 32 MiB
```

但不是用户随意输入后直接相信，而是根据：

```text
FountainBlockBytes
Wirehair K limit
RAM budget
CPU benchmark
```

自动 clamp。

---

## 10.1 Segment Pipeline

```text
Segment N
  → read
  → digest
  → compress/raw
  → Wirehair
  → transmit

Segment N+1
  → read/compress preparation
```

允许有限重叠，但必须控制 active segment count。

---

## 10.2 内存复杂度

总文件 5 GB 或 500 GB：

```text
RAM = O(active segment set)
```

而不是：

```text
O(total file size)
```

---

## 10.3 Wirehair V2 内存注意

V2 encoder 会复制 message。

因此实际内存预算至少包含：

```text
Raw Segment Buffer
Encoded Segment Buffer
Wirehair private copy / codec state
FEC buffers
Frame buffers
Capture buffers
```

不能把“8 MiB Segment”理解成“只占 8 MiB RAM”。

---

## 10.4 Tiny / Small Encoded Segment Policy

Segment 是否适合 Wirehair 必须在**压缩之后**判断。

```text
K = ceil(EncodedSize / FountainBlockBytes)
```

处理规则：

```text
EncodedSize == 0
    → no data segment payload

K <= frozen DirectRepeat efficiency threshold
    → OuterFecMode = DirectRepeat

2 <= K <= 64000
    → OuterFecMode = WirehairV2

K > 64000
    → split Segment / choose another valid block size
```

Phase-0 CPU reference 的保守 efficiency gate 冻结为：

```text
K = 0..2      → DirectRepeat
K = 3..64000  → WirehairV2
K > 64000     → split Segment / choose another valid block size
```

Certified Profile 可根据 `PBOuterFecDirectRepeatBenchmark` 的实测结果显式选择另一个
冻结阈值，但必须在 `SegmentDescriptor` 冻结前完成，并与该 Profile 的接收端工作量
配额协调；禁止在 Wirehair codec 创建失败后 silent fallback。接收端另外使用
`maxDirectRepeatBlockCount` 作为本地工作量配额，默认 64，避免极小 block size 仅凭
内存预算进入数千万 ordinal 的活跃状态。

`DirectRepeat` 将小 Encoded Segment 切成少量有 ordinal 的 Transport Block，由内层 LDPC + CRC32C 保护，并在不同 Visual Frame / Carousel pass 中重复。

这条路径必须覆盖：

- 0 B；
- 1 B；
- 高压缩率 Segment；
- 极小 tail；
- Wirehair K 很小、创建成本不划算的场景。

空文件采用确定性语义：

```text
OriginalFileSize = 0
SegmentCount = 0
WholeFileDigest = BLAKE3-256(empty)
no SegmentDescriptor
no Data Block
```

Decoder 仍必须收到并验证 `SessionDescriptor + FinalManifest` 后才创建最终 0-byte 文件。

---

# 11. 即时模式的无反馈调度：Segment Carousel

这是即时模式的核心协议机制。

## 11.1 Pass 0

对每 Segment：

```text
Systematic / initial repair
+ InitialRedundancy
```

例如：

```text
K source-equivalent symbols
+ R0 repair symbols
```

`R0` 不是写死百分比，而由 Certified Profile benchmark 冻结。

对 Wirehair V2，推荐固定 ID policy：

```text
Systematic IDs : 0 .. K-1
Repair IDs     : K .. monotonically increasing
```

每个 SegmentDescriptor/Encoder runtime 维护 `NextRepairOuterBlockId`；Carousel、resume 和 Encoder restart policy 都不得意外复用一个曾经用于不同方程语义的 repair ID。

---

## 11.2 Pass N > 0

后续轮次：

```text
Segment 0 → new repair ids
Segment 1 → new repair ids
...
```

只产生新的修复 Symbol，避免浪费带宽重复发送相同 Fountain 方程。

Certified `FullRepairPass` 对每个 Wirehair Segment 定义：

```text
RepairPassSymbolBudget = K + Rpass
```

其中全部使用新的 repair IDs。这样一个完全错过 Pass 0 的 Decoder，在理想无额外丢失时仍可仅凭一个 Full Repair Pass 获得约 K 个独立方程；`Rpass` 再吸收 capture/LDPC erasure 与 Wirehair 非 MDS 开销。

允许存在更短的 `MicroRepairPass` 以降低已在线 Receiver 的尾部等待，但只要 `RepairPassSymbolBudget < K`，UI/文档就不得把它计入“一轮 late-join 可恢复”保证。

---

## 11.3 为什么仍允许部分重复

控制信息需要重复：

- Bootstrap；
- Session Descriptor；
- Segment Descriptor；
- Final Manifest。

这些属于低开销、强健控制平面。

---

## 11.4 无 ACK 的停止条件

由于 Encoder 不知道 Decoder 是否完成：

- Encoder 默认持续 Carousel；
- 用户在 Decoder 显示 `Verified Complete` 后手工停止；
- 不通过隐藏 IPC 自动通知。

如果未来允许可选反向通道，可单独设计，不污染 v1 单向协议。

---

## 11.5 Carousel 周期与局部 Repair Burst

全局 Carousel 的恢复能力是以更长的 late-join latency 为代价的。

推荐每个 Segment 在离开当前工作集前先给出一个由 benchmark 冻结的 `LocalRepairBurst`，降低“只差少量 symbol 却必须等完整一圈”的概率；每次 Segment burst 开始前先重复对应 `SegmentDescriptor`，随后全局 pass 再继续产生新 repair IDs。

必须实时统计：

```text
CarouselCycleTime
EstimatedWorstLateJoinTime
CurrentSegment
CurrentPass
LocalRepairRatio
```

对于非常大的文件，UI 必须明确展示预计周期，不能仅以“支持任意大文件”掩盖实际需要数小时或数天的物理传输时间。

---

## 11.6 Inter-Segment Temporal Striping

Certified 即时调度器允许同时维护一个很小的 Active Segment Window：

```text
Segment i
Segment i+1
...
Segment i+W-1
```

在窗口内部按确定性 round-robin 发送 Outer Block，而不是把每个 Segment 的 K+R 长时间连续发完：

```text
S0:B0 → S1:B0 → S2:B0 → S3:B0
S0:B1 → S1:B1 → S2:B1 → S3:B1
...
```

这样一次连续的 capture interruption、窗口覆盖或 Present glitch 会被摊成多个 Segment 的少量 erasure，降低单个 Segment 因整段 burst 丢失而等待完整 Carousel 的概率。

约束：

- `ActiveSegmentWindowSize` 是 Scheduler/Profile tuning，不改变 Wirehair 方程；
- 默认候选范围为 2～8，不在 benchmark 前冻结；
- Decoder 的 `MaxActiveOuterFecDecoders`、RAM 和 resume cache 必须覆盖该窗口；
- Segment Descriptor 在进入窗口时重复，并在长窗口中按固定 cadence 再广播；
- Window sliding 前仍可发送有界 `LocalRepairBurst`；
- 对超大文件仍必须计算真实 `CarouselCycleTime` 与 late-join 上界。

Telemetry 增加：

```text
ActiveSegmentWindowSize
BurstErasureLengthP50/P95/P99
StripingRecoveryBenefit
OuterDecoderPeakActive
```

---

# 12. 压缩层

使用：

```text
Zstandard
```

每个 Source Segment 独立形成一个 zstd frame。

推荐 API：

```text
ZSTD_compressStream2
```

---

## 12.1 RAW Fallback

如果：

```text
compressedBytes + framingMargin >= rawBytes
```

则：

```text
CompressionCodec = RAW
```

避免对：

- ZIP；
- RAR；
- JPEG；
- MP4；
- 已压缩 EXE installer；

做负收益压缩。

---

## 12.2 即时与离线压缩等级

即时模式：

> 优先低延迟，不因追求最后几个百分点压缩率阻塞开始发送。

离线模式：

> 可以尝试更高 zstd level，因为压缩后少 10% 数据几乎直接减少 10% 视觉帧数量。

最终通过：

```text
CompressionTime
vs
BytesSaved
vs
VideoDurationSaved
```

选择。

---

# 13. 外层纠删码：Wirehair V2

PixelBridge v1 对正常大小 Segment 的主要外层 FEC：

```text
Wirehair V2
```

Tiny/Small Segment 可按 10.4 使用 `DirectRepeat`；本章其余约束只针对 `OuterFecMode = WirehairV2`。

---

## 13.1 必须 pin Profile，不使用含糊的 CURRENT 语义

协议中应记录 Wirehair V2 serialized descriptor，并在 PixelBridge 版本规范里明确：

```text
SupportedWirehairProfileId = 某个冻结 ID
```

PixelBridge v1 默认固定：

```text
WIREHAIR_V2_PROFILE_CERTIFIED_2026_07
ProfileId = 0x4b295bbb47f4f9c9
```

协议实现不得只序列化“CURRENT”这个概念；必须保存 canonical 32-byte V2 descriptor，并验证其中的具体 Profile ID。

`WIREHAIR_V2_PROFILE_MIXED_2026_07` / `MIXED_MIX2_2026_07` 仅作为实验候选，只有在 PixelBridge 自己的 block-size、loss pattern 和内存/CPU benchmark 通过后再形成新的 `PBOuterFecProfileId`。

同一 Segment 第一次创建 V2 encoder 后，必须持久保存它返回的 canonical 32-byte descriptor。后续 Carousel 重新载入该 Segment 时使用：

```text
exact Encoded Segment bytes
+ exact saved descriptor
→ wirehair_v2_encoder_create_profile(...)
```

而不是再次调用默认 profile 选择路径。这样 seed attempt、方程身份和历史 `OuterBlockId` 始终属于同一个 wire contract。

Mixed 两个 profile 要求 `blockBytes` 为正偶数。当前 payload budget 中：

```text
VideoSafe     1314 B  // even, 可进入 mixed A/B
VideoBalanced 1449 B  // odd
DesktopFast   1629 B  // odd
```

因此 Balanced/Fast 若测试 mixed profile，必须为该 `PBOuterFecProfileId` 定义新的偶数 `OuterBlockBytes`（例如先评估 1448 / 1628 B），不能复用 certified GF(256) profile 的奇数 block size。

同时，PixelBridge 的 release build 必须 pin 经过验证的 Wirehair source revision / vendored commit，并记录到 build metadata / SBOM。

> `ProfileId + 32-byte descriptor` 决定 wire 方程兼容性；**Git commit 不进入 wire protocol**。pin 源码版本的目的是锁定经过测试的实现、错误处理、性能和恢复行为。

---

## 13.2 block count 约束

对每个选择 `OuterFecMode = WirehairV2` 的 Segment，在创建 Fountain 前：

```text
K = ceil(EncodedSegmentBytes / FountainBlockBytes)
```

必须：

```text
2 <= K <= 64000
```

否则：

- 缩小 Segment；或
- 调整 Fountain block size。

---

## 13.3 Fountain block 尽量和一个 LDPC 信息块一一对应

最简单、最稳的映射是：

```text
1 Wirehair Packet
→ 1 Transport Block
→ 1 LDPC Information Block
```

好处：

- 一个 LDPC failure 只擦除一个 Fountain Packet；
- 不跨 codeword 拼接 packet；
- parser 简单；
- resume 简单；
- telemetry 清晰。

按照当前建议的 32-byte Transport Header + 4-byte Payload CRC，可得到初始 Fountain block budget：

| PixelBridge Profile | LDPC K | Information bytes | Transport overhead | FountainBlockBytes |
|---|---:|---:|---:|---:|
| PB-VideoSafe-1 | 10800 bit | 1350 B | 36 B | **1314 B** |
| PB-VideoBalanced-1 | 11880 bit | 1485 B | 36 B | **1449 B** |
| PB-DesktopFast-1 | 13320 bit | 1665 B | 36 B | **1629 B** |

以默认 8 MiB Encoded Segment 粗略估算，Wirehair source block count 分别约为：

```text
1314 B → K ≈ 6385
1449 B → K ≈ 5790
1629 B → K ≈ 5150
```

都明显小于 64000。即使把 Segment 上限放到 32 MiB，三档也约为 25537 / 23157 / 20599，仍在 V2 维度限制内；但真实峰值内存和 codec 创建/恢复耗时仍需 benchmark。

注意：这组表只描述正常大小 Segment；Tiny/Highly-compressible Segment 必须走 10.4 的动态策略，不能因为“默认 8 MiB”就假定 K 一定很大。

---

## 13.4 OuterBlockId

Transport 层统一使用 `OuterBlockId : uint32`。

- `OuterFecMode = WirehairV2`：`OuterBlockId` 就是 Wirehair block ID；
- `OuterFecMode = DirectRepeat`：`OuterBlockId` 是从 0 开始的 Direct Block ordinal。

Wirehair API 使用 32-bit block ID。

PixelBridge：

- 每 Segment 独立 ID 空间；
- 不发生 wrap；
- 重复 ID 只作为同一 Fountain 方程的再次观察；
- 新 Carousel Pass 应优先用新 ID；
- 同一 `(SessionTag, SegmentOrdinal, OuterBlockId)` 的**有效 payload 必须完全一致**。

Decoder 对已经接受过的 ID 保存一个有界 fingerprint，例如内部 BLAKE3-128：

```text
OuterBlockId
→ PayloadFingerprint
```

若后续重复 ID 的 Transport CRC 通过，但 payload fingerprint 与已接受值不同：

```text
OuterBlockConflict
→ stop accepting that Segment
→ fail closed / request decoder reset policy
```

禁止 latest-wins，也禁止把两个不同 payload 以同一 Wirehair equation ID 继续喂给 solver。

fingerprint 属于接收端本地一致性状态，不进入 wire framing；完成 Segment 后立即释放。

---

## 13.5 Decoder accepted-symbol 资源边界

“Encoder 可产生很多 repair block”不等于“一个 Decoder instance 可以无限接收 unique ID”。Wirehair V2 API 明确暴露：

```text
WirehairV2_ExtraInsufficient
```

表示额外行求解容量或 accepted-ID 资源耗尽。

PixelBridge 处理规则：

1. 只有通过 Inner FEC、Transport CRC、Descriptor binding 的 packet 才允许送入 Wirehair；
2. 对每个 Segment 统计 `AcceptedOuterSymbols` / `DuplicateOuterSymbols`；
3. `WirehairV2_ExtraInsufficient` 视为 Segment decoder failure，停止向该 codec 继续灌包；
4. 记录完整诊断并重新创建 decoder；重新开始时必须等待/收集一个足以独立恢复该 Segment 的完整新 repair window（Certified Full Repair Pass 至少按 `K + Rpass` 预算），不能只保留少量后续增量包却假设旧 decoder 状态仍存在；
5. 正常 Certified channel 中若频繁出现该状态，视为 Wirehair profile / packet policy / implementation bug，不把“无限增大 repair 数量”当修复手段。

---

# 14. 内层纠错码：Soft-Decision QC-LDPC

采用：

> DVB-S2 Short Frame 系列 QC-LDPC parity-check matrix 作为首选工程基础。

固定：

```text
N = 16,200 bit
```

但协议中不能只写：

```text
Rate ≈ 3/4
```

而必须写确切：

```text
InnerFecProfileId
N
K
MatrixId
MatrixDigest
SystematicBitOrder
PuncturingOrShorteningRule
InterleaveProfileId
```

---

## 14.1 推荐三个初始 FEC 档

### Robust

```text
N = 16200
K = 10800
K/N = 2/3
```

### Balanced

```text
N = 16200
K = 11880
K/N ≈ 0.7333
```

### Fast

```text
N = 16200
K = 13320
K/N ≈ 0.8222
```

这里按 **LDPC 信息位**计算；PixelBridge 不默认再叠加 DVB-S2 的 BCH 外码。

需要特别区分 DVB-S2 表中的“LDPC Code Identifier”和去掉 BCH 后真正使用的 `K/N`：

| PixelBridge 档 | DVB-S2 Short code identifier | N | K | 实际 LDPC rate |
|---|---:|---:|---:|---:|
| Robust | 2/3 | 16200 | 10800 | 2/3 |
| Balanced | 3/4 | 16200 | 11880 | **11/15 ≈ 0.7333** |
| Fast | 5/6 | 16200 | 13320 | **37/45 ≈ 0.8222** |

因此协议与代码中一律以 `InnerFecProfileId + exact N/K + MatrixId/MatrixDigest` 为准，不根据“3/4”“5/6”字符串反推 K。

LocalDesktop 的实验路线还可以 benchmark DVB-S2 Short `K=14400, N=16200`（effective rate 8/9），以及 `PB-InnerFEC-ErasureOnly-X`。它们不得用于 VideoSafe/VideoBalanced，除非真实 Post-Capture BER/FER Gate 证明可靠。

---

## 14.2 Decoder

首选：

```text
Layered Offset Min-Sum
```

或：

```text
Layered Normalized Min-Sum
```

输入：

```text
int8 / int16 quantized LLR
```

CPU reference：

- AVX2；
- AVX-512（运行时检测）；

GPU 是否值得做 LDPC，要以 benchmark 决定，不预设“GPU 一定更快”。

这些 Decoder 细节属于接收端本地能力；只要最终遵循同一个 `InnerFecProfileId` 所冻结的 parity-check matrix、systematic mapping 和 bit ordering，不需要为了更换 Min-Sum/SPA、LLR 量化或迭代次数升级 wire protocol。

---

## 14.3 Early Termination

每若干 iteration：

```text
check syndrome
```

通过即提前退出。

随后仍必须验证 Transport CRC32C。

---

# 15. 端到端完整性、安全边界与文件摘要

## 15.1 三层完整性

```text
Transport CRC32C
Segment Digest
Whole File Digest
```

---

## 15.2 默认摘要

推荐：

```text
DigestAlgorithm = BLAKE3-256
```

可通过协议扩展支持 SHA-256。

---

## 15.3 Whole File Digest 不要求预扫两遍

Encoder 第一次从头读文件时流式计算：

```text
WholeFileDigestState.Update(rawBytes)
```

到最后一个 Segment 才得到 digest。

之后发布 `FinalManifest`。

这样即时模式可以边读边传，无需“先完整 Hash 一遍再开始显示”。

---

## 15.4 摘要不是认证

如果攻击者同时伪造：

```text
Data + Digest
```

in-band digest 不提供身份认证。

如需安全身份：

```text
FinalManifest
→ Ed25519 Signature
```

或预共享密钥 MAC。

该功能作为可选 Secure Profile。

---

# 16. 视觉帧总体结构

Logical Canvas：

```text
1920 × 1080
```

但 **Data ROI 不对所有 Profile 共用一个近似尺寸**。每个 Profile 必须同时冻结 Data Grid 与所有非数据区域，保证：

- Grid width/height 为整数；
- codeword packing 可预测；
- Sync / Bootstrap / Control / Pilot 有明确物理位置；
- 不出现“剩余 margin 应该够用”或“尺寸不能整除却靠取整继续运行”的隐式行为；
- 容量计算能从 canonical frame layout 精确重现。

---

## 16.1 Frame Layout

```text
┌────────────────────────────────────────────────────┐
│ Sync / Bootstrap / Timing / Control / Pilot        │
│                                                    │
│       ┌────────────────────────────────────┐       │
│       │                                    │       │
│       │             Data Grid              │       │
│       │                                    │       │
│       └────────────────────────────────────┘       │
│                                                    │
│ Pilot / Calibration / Bootstrap / Timing / Guard  │
└────────────────────────────────────────────────────┘
```

整个 Canvas 被划分为互不重叠的：

```text
SyncRegion
BootstrapRegionA/B
ControlRegion
PilotRegion
GuardRegion
DataGrid
```

每个区域的 `x/y/width/height`、cell/tile geometry、重复 cadence 和 bit mapping 都由 `VisualLayoutVersion + VisualProfileId` 冻结。

---

## 16.2 Data Grid 与真实容量预算

```text
DataGridOriginX/Y
DataGridColumns
DataGridRows
CellPitch / TileWidth / TileHeight
BitsPerCellOrTile
CodewordsPerFrame
```

全部由 `VisualProfileId` 冻结。

Decoder 不做模糊猜测。

Profile 的理论容量必须从最终 canonical layout 计算。若 Bootstrap / Control / Pilot 完全位于 DataGrid 外，则它们不直接扣 Data Grid bit 数，但会限制 DataGrid 能使用的几何范围；若任何控制数据借用 DataGrid，则必须在 payload budget 中显式扣除。

因此在 Profile Freeze Gate 前，必须满足：

```text
CanonicalRaster
→ exact region map
→ exact data cells/tiles
→ exact FEC codeword packing
→ exact Transport payload bytes/frame
```

并将结果写进 Golden Vector。

任何基于“当前候选 DataGrid 尺寸”得到的 MiB/s 数字，在此 Gate 之前都只视为候选上界。

---

## 16.3 Encoder Presentation Contract

即时模式的 Data Surface 必须使用独立 D3D11 top-level data window；Qt 控制 UI 不参与数据画布合成。

Certified LocalDesktop 候选要求：

```text
SwapEffect = FLIP_DISCARD / FLIP_SEQUENTIAL
BufferCount >= 2
FRAME_LATENCY_WAITABLE_OBJECT = enabled when supported
MaximumFrameLatency = benchmark 1~2, default candidate 1
Present pacing = VSync-based
ALLOW_TEARING = off
MSAA = off
alpha blending = off
texture filtering = point/none
client pixels = physical 1:1
```

Direct-Level 的 Certified Sender 还要求：

```text
Swap-chain format = R8G8B8A8_UNORM or B8G8R8A8_UNORM
no *_SRGB render-target auto conversion
shader/compute writes exact UNORM code values
full data surface is deterministically redrawn every Visual Frame
```

尤其 `FLIP_DISCARD` 不能依赖 Present 之后 back buffer 内容仍被保留；DWM/flip presentation 允许前一 buffer 内容失效或被组合路径使用。PixelBridge 每一帧都必须从 `FrameSequence + payload + fixed regions` 完整重建 canonical raster，而不是以“上一帧内容 + 局部 patch”作为协议正确性的前提。

`PBPresentTiming` 记录：

```text
PresentCallFPS
PresentedVisualFPS
PresentRefreshCount
PresentQueueLatency
PresentGlitchCount
```

DXGI `PresentCount` 与实际呈现次数不保证等于应用调用 `Present` 的次数，因此性能统计以 present statistics / measured unique visual frames 为准。

如果发生 `DXGI_ERROR_FRAME_STATISTICS_DISJOINT`、显示模式切换、DPI/monitor 迁移或 swap-chain resize，暂停数据 Session 的 Certified timing 统计并重建 presentation epoch。

---

## 16.4 Torn / Mixed Frame 的空间一致性检测

`PB-Bootstrap-1` 至少在 Data Grid 的两个空间分离区域重复：

```text
SessionTag
FrameSequence
VisualProfileId
BootstrapCrc
```

两处 Bootstrap 解码结果不一致时，说明 Capture Frame 可能处于：

- tearing；
- DWM transition；
- player interpolation；
- resize/mixed composition；

整张 frame 直接标记为 erasure，不允许把上半帧和下半帧的 LLR 拼成一个 codeword。

---

## 16.5 Pilot / Calibration Region

Pilot 不是装饰性测试图案，而是每个 Certified Profile 的通信训练符号。

至少包含空间分离的：

```text
black / white / mid-gray references
luma level ladder
active chroma constellation states
neutral chroma reference
phase / checker references
```

Pilot 的目标是直接测量“发送 code value → 当前 capture space 中观测分布”的映射，而不是强行假设 DWM / player / HDR pipeline 可以由一个固定 gamma 或 3×3 矩阵精确描述。

对于 Direct-Level，优先维护：

```text
CapturedCentroid[level]
CapturedVariance[level]
DecisionBoundary[level]
```

对于 Shape+Chroma，维护：

```text
Luma normalization
Chroma constellation centroid/covariance
Calibration residual
```

如果 Pilot 出现 clipping、level order inversion、类间 margin 低于 Profile 门槛、颜色空间突变或与当前 `CaptureEpoch` 不一致，当前 frame 应降权或擦除；持续失败时终止该 Certified Session，而不是继续用错误模型硬解。

Pilot 参数可以做跨帧低通平滑，但 `CaptureEpoch`、Display/Monitor migration、HDR/SDR 切换、播放器 resize 后必须清空并重新收敛。

---

---

# 17. 视觉调制：Video Shape/Chroma 与 LocalDesktop Direct-Level

PixelBridge 采用统一协议、分化物理层：

```text
LocalVideo
    → PB-Mod-ShapeChroma-1

LocalDesktop
    → PB-Mod-DesktopLevels-X 作为第一等候选
    → PB-Mod-ShapeChroma-1 作为保守 fallback

RemoteVisual
    → 独立 Experimental/Certified profile
```

Shape + Color/Luma+Chroma 仍然继承 libcimbar 最有价值的思想之一，但它不被默认视为所有 Windows 像素链路的最高效调制。

---

## 17.1 LocalVideo：Luma Shape 是主数据通道

Shape 主要编码在 Y / luminance。

优点：

- H.264 4:2:0 不降低 luma 空间分辨率；
- resize 后更稳定；
- RGB/YUV conversion 更可控；
- 可用模板距离产生 Soft Metric；
- 对播放器/显示色差的依赖小于高阶颜色星座。

---

## 17.2 LocalVideo：Chroma 是增益通道

默认最多：

```text
4 chroma states
= 2 bit/cell
```

不以 8/16 色作为 v1 默认。

对于：

- HDR；
- 非整数缩放；
- 强 player post-processing；
- range / matrix 解释不一致；

Decoder 可通过 Pilot 测量 Chroma Reliability。

由于 Visual Profile 在 Session 内固定，**不能临时把某一帧从 6 bit/cell 改成 4 bit/cell**。因此任何包含 Chroma 的 Certified Profile 都必须在目标信道矩阵中证明其低分位数 decision margin 和 FER 达标。

---

## 17.3 YUV420：Even Cell Pitch 只是必要条件

对于 4:2:0：

```text
CellPitch % 2 == 0
DataGridOriginX % 2 == 0
DataGridOriginY % 2 == 0
```

是基础要求，但不是充分条件。

播放器如果把 1920×1080 缩成非整数比例，chroma sampling phase 仍会变化。因此必须做：

```text
Scale Sweep
Chroma Phase Sweep
Player Matrix
full/limited-range interpretation
hardware/software decode matrix
```

不能只因为 pitch 和 origin 为偶数就认为 chroma 一定安全。

---

## 17.4 LocalDesktop：Direct-Level / Quantized Raster

`PB-Mod-DesktopLevels-X` 不使用 Shape classifier。逻辑数据直接映射为离散 tile level/constellation：

```text
bits
→ Gray-labelled level/constellation
→ physical-pixel-aligned tile
→ D3D11 raster
→ DWM / Capture
→ Pilot-calibrated centroid distance
→ hard bits or LLR
```

初始实验维度：

```text
Tile = 4×4
Tile = 2×2
optional 1×N / N×1 stripe experiments

Luma-only levels
RGB/YCbCr-like joint constellations
```

不在设计阶段预设“16 levels 一定可靠”或“2×2 一定优于 4×4”。真正需要优化的是：

```text
VerifiedEncodedGoodput
= tiles/frame
× usable bits/tile
× FEC rate
× unique visual FPS
× completion efficiency
```

而不是单独最大化 bits/tile。

---

## 17.5 Direct-Level 的判决模型

Direct-Level Decoder 不强求把 Capture 像素还原成 Sender 的理论 RGB 值。

更稳健的做法是利用每帧/邻帧 Pilot 得到真实 capture-space 分布：

```text
sent level i
→ captured centroid μ_i
→ covariance / variance Σ_i
```

解调使用：

```text
distance(sample, μ_i, Σ_i)
```

并从候选 level 的 bit label 计算 Max-Log LLR。

如果链路在 Probe 中表现为 byte-exact，可直接使用整数比较/查表；如果存在稳定的确定性映射，则使用 centroid/LUT；只有误差仍明显时才依赖 soft QC-LDPC。

Level label 应尽量采用 Gray-like mapping，使最容易混淆的相邻 level 只差 1 bit。

---

## 17.6 Signal Profile：把像素变成可复现的数值协议

Visual Profile 必须引用 `SignalProfileId`。

v1 至少定义：

```text
PB-Signal-DesktopSRGB-1
    D3D11 8-bit SDR
    DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709
    exact sender code values
    Pilot calibration contract

PB-Signal-Video709Limited-1
    NV12 8-bit
    BT.709 primaries / transfer / matrix
    studio-range Y/Cb/Cr
    fixed chroma sample location
    Pilot calibration contract
```

Desktop Sender 在支持时先用 `CheckColorSpaceSupport` 检查，再通过 `SetColorSpace1` 显式声明 swap-chain SDR color space。Decoder 仍以真实 capture Pilot 为准，不把“API 设置成功”误认为端到端像素一定 byte-exact。

Offline Sender 直接写 NV12 Y/UV plane，避免依赖未知 RGB→YUV converter。

Constellation/code value 满足：

- 不使用接近合法 range 端点的极值；
- 保留 clipping/tone-map guard band；
- Chroma states 尽量保持近似恒定 luma；
- 映射后不发生严重 gamut clipping；
- Pilot 校准后的类间 margin 有明确 P01/P001 门槛。

---

## 17.7 当前 1080p Shape 基线几何

Shape 基线当前可采用：

```text
PB-VideoSafe-1      DataGrid 1800×960  → Origin = (60, 60)
PB-VideoBalanced-1  DataGrid 1800×972  → Origin = (60, 54)
PB-DesktopFast-1    DataGrid 1728×1000 → Origin = (96, 40)
```

它们都是候选 geometry。最终只有在 Sync / Bootstrap / Control / Pilot / Guard 的 canonical region map 能完整放入 1920×1080 且 Golden Raster 通过后才冻结。

`PB-DesktopNative-X` 使用独立 tile geometry，不要求复用 Shape profile 的 DataGrid。

---

## 17.8 LocalDesktop 物理层 Gate

Phase 1～2 的 A/B 必须至少比较：

```text
PB-Mod-ShapeChroma-1
PB-Mod-DesktopLevels-X
```

在相同：

```text
Canvas
UniqueVisualFPS
Capture Backend
Adapter
SDR/HDR state
CPU/GPU budget
FEC acceptance rule
file corpus
```

下报告：

```text
VerifiedEncodedGoodput
TimeToVerifiedComplete
RawSymbolErrorRate / PreFecBER
FER
ByteExactRatio
DecisionMargin P50/P01/P001
GPU demod time
CPU FEC time
scale sensitivity
color-management sensitivity
```

LocalDesktop 的最终 Certified Profile 只由这些结果决定。

---

---

# 18. Visual Symbol Codebook

Codebook 必须由：

```text
PBCodebookOptimizer
```

离线生成。

---

## 18.1 信道仿真

不模拟 camera lens。

模拟：

```text
YUV420
H.264 / HEVC / AV1 reference experiments
RGB ↔ YUV
BT.709
full / limited range
bilinear
bicubic
integer/non-integer resize
subpixel phase
HDR-like transfer/tone mapping
DWM composition
capture BGRA/FP16
sharpen
noise reduction
frame blending
```

---

## 18.2 Shape 约束

- 避免单像素孤岛；
- 避免最高 Nyquist 附近结构；
- luma duty 约平衡；
- 低通/缩放后仍可分；
- H.264 后低分位数类间距离仍大；
- 支持可靠 soft metric。

---

## 18.3 Label 优化

最容易混淆的两个 Shape，其 bit label 应尽量：

```text
Hamming Distance = 1
```

即 Gray-like mapping。

目标不是“模板几何上最好看”，而是：

> **真实信道下期望 bit cost 最低。**

---

# 19. 空间交织、时域相位、扰码与局部坏区

## 19.1 Spatial Interleave

一个 Inner-FEC codeword 的 bits/tiles 必须分布到 Data Grid 不同空间位置。

目的：

- OSD；
- 鼠标；
- 播放器控制栏；
- 通知弹窗；
- 局部缩放滤波异常；
- 某一固定显示区域的颜色/亮度异常；

不集中摧毁同一个 codeword。

---

## 19.2 Interleave Phase 随 FrameSequence 确定性变化

固定的空间 permutation 有一个隐藏问题：

> 如果屏幕上长期存在一个固定遮挡区域，它可能在每一帧都持续打击同一组 codeword bit positions。

因此 Certified Profile 定义一个有限的：

```text
InterleavePhaseCount
InterleavePhase = f(FrameSequence)
```

例如通过几组预定义的 row/column affine permutation、cyclic shift 或 tile-group permutation，在相邻 Visual Frame 间改变 codeword 到物理位置的映射。

要求：

- permutation 完全由 `InterleaveProfileId + FrameSequence` 确定；
- 不发送额外随机 seed；
- 不改变单帧 FEC codeword 本身；
- Phase 数量保持小而可测试；
- Pilot / Bootstrap 位置不随该 permutation 漂移；
- Video Profile 必须同时测量 MP4 bitrate，避免过度随机化损害编码效率。

---

## 19.3 Local Reliability Map

Decoder 不只输出全局 `CalibrationResidual`，还维护粗粒度空间可靠性：

```text
TileReliability[x, y]
PersistentBadRegionMask
LocalContrast
LocalPilotResidual
```

对于持续异常的区域：

- soft 路径降低 LLR magnitude；
- 明确不可用的 cell/tile 直接给近零 LLR，而不是强行选最近 symbol；
- 如果坏区超过 Profile 可接受比例，整帧或 Session 进入 degraded/failure 状态。

禁止把一个长期错误但“距离最近”的 symbol 赋予高置信度 LLR。

---

## 19.4 Scrambler 不应为“随机而随机”

压缩数据 + Fountain 本身已接近高熵。

全量 PRBS 可能进一步破坏视频编码器可利用的微弱相关性并增加 MP4 bitrate。

因此：

> Scrambler 的价值必须通过 BER/FER、Verified Goodput 与 MP4 bitrate 三指标验证。

若 Fountain 数据本身已经足够白化，可只保留必要的 bit permutation / DC balancing / whitening，而不是追求最大视觉随机性。

---

---

# 20. 离线 MP4 技术路线

默认兼容容器：

```text
MP4
H.264
8-bit
YUV420 / NV12
CFR
SDR
BT.709
```

---

## 20.1 默认 GOP

```text
All-Intra
GOP = 1
B-frame = 0
```

理由：

- Visual Frame 独立；
- loop/seek 简单；
- 避免 temporal prediction 把错误扩散到相邻数据帧；
- Decoder 不依赖解码顺序之外的复杂 temporal reference。

但这会显著提高码率，必须通过兼容性 Gate。

---

## 20.2 Rate Control

优先：

```text
CONSTQP / CQP
```

不以普通视频主观视觉质量为目标。

---

## 20.3 关闭面向“人眼感知”的编码优化

PixelBridge 的目标是 BER/FER，而不是主观观感。

默认建议禁用：

```text
Lookahead
Spatial AQ
Temporal AQ
Psycho-visual tuning
自动 B-frame 决策
```

原因：

- AQ 会根据“人眼重要性”重新分配 QP；
- 高空间细节 Data Grid 可能反而被分配更差质量；
- temporal AQ / lookahead 引入缓冲和帧间策略；
- 对数据视频没有明确收益。

任何重新启用都必须以 Post-Screen-Capture BER/FER 证明。

---

## 20.4 直接 NV12 Rasterization

推荐：

```text
Logical Cell
→ Exact Y / UV code values
→ NV12
→ NVENC
```

避免：

```text
RGB frame
→ 未知/不可控 RGB→YUV converter
→ NV12
```

并显式设置：

- color primaries；
- transfer characteristics；
- matrix coefficients；
- range；
- chroma siting。

NV12 的工程实现必须尊重真实资源布局：

```text
width  = even
height = even
Y plane  = R8-compatible
UV plane = R8G8-compatible, half width/height sampling
row pitch may be > logical width
```

CPU reference / staging path 不允许按 `width * height * 3 / 2` 假定每行无 padding；必须按实际 `rowPitch` 写入 Y 与 UV。

GPU fast path 可以使用 D3D11 planar view / plane slice 能力直接写 NV12，或写入独立 Y/UV intermediate resources 后进行**经 bit-exact Gate 验证**的 plane copy。禁止为了省事引入 VideoProcessor/RGB→NV12 转换后却不验证实际 Y/UV code value。

Phase 4 必须保留一个最简单的 canonical NV12 raster reference，使：

```text
logical visual frame
→ expected NV12 bytes/code values
```

可以生成 Golden Vector；NVENC 前的输入 surface 必须与该 reference 在有效 plane 上一致。

---

---

## 20.5 MP4 码率保护

必须同时设置：

```text
EncodedBitrateGate
PeakFrameBytesGate
DecoderCompatibilityGate
```

如果某个 QP/Profile 产生过高码率，Encoder 自动降级 Profile，而不是无限加码率。

---

## 20.6 H.264 / MP4 bitstream 语义必须冻结

Certified Offline Profile 至少固定并写入生成器测试：

```text
CFR + exact FrameRateNum/FrameRateDen
H.264 profile
H.264 level / level-selection rule
B-frame = 0
IDR cadence / closed GOP rule
8-bit NV12 / 4:2:0
BT.709 primaries / transfer / matrix
full or limited range flag
chroma sample location / siting policy
deblocking-filter policy
PTS monotonicity
no frame interpolation metadata
```

“GOP=1”不能只作为一个 Encoder UI 参数；最终生成的 bitstream 必须通过 inspector 验证实际 IDR/I-frame、VUI 和 timing semantics。

---

## 20.7 “任意播放器”的准确边界

PixelBridge 将播放器分为：

### Certified Player

经过自动测试矩阵验证的版本。

### Best-Effort Generic Player

满足：

- 1.0×；
- 标准 H.264 解码；
- 无帧插值；
- 无 AI 超分；
- 无强锐化/降噪；
- 无强制 HDR 重映射。

UI 不再宣传“任何播放器必定成功”，而是：

> **标准兼容播放器通常可工作；认证列表提供确定性支持范围。**

---

## 20.8 H.264 Deblocking 必须通过数据链路 Gate 冻结

H.264 in-loop deblocking 的目标是普通视频主观质量，它对 PixelBridge 可能同时存在两种相反影响：

- 减轻量化造成的 block boundary artifact；
- 抹平本来用于分类的 Cell/Shape 高频边缘。

因此 `disableDeblockingFilterIDC` 不采用经验主义固定值。PBBenchmark 至少比较：

```text
Default deblocking
vs
Disabled deblocking
```

评价指标只看：

```text
PostCapture PreFecBER
LDPC FER
VerifiedEncodedGoodput
MP4 bitrate
```

胜出的设置进入对应 Certified Offline Profile。

---

## 20.9 Offline Artifact Expansion

离线 MP4 本身是一个中间传输载体，必须统计其空间成本：

```text
OfflineArtifactExpansionRatio
= MP4FileBytes / EncodedPayloadBytes
```

以及：

```text
PayloadSecondsPerGiB
PeakMuxWriteRate
AverageEncodedBitrate
PeakFrameBytes
```

如果为了降低 BER 把 All-Intra/CQP 推到极端高码率，导致 MP4 比源数据膨胀数倍甚至数量级增长，应优先降低 Visual density、增大 Cell 或加强 FEC，而不是无限堆视频码率。

Media Foundation H.264 fallback 只有在能够固定并验证所需 GOP/range/VUI/quality 语义后才能进入 Certified Player/Generator 矩阵；否则属于 Functional/Best-Effort Offline Backend。NVENC 是 v1 的首选 Certified Offline Encoder。

---

## 20.10 Offline Temporal Schedule

MP4 是有限文件，循环播放不会产生新的 Fountain equation。因此一个 Certified Offline Artifact 在**单次完整播放**中就必须包含足够的 unique `OuterBlockId` 和 Repair Margin。

同时不能简单按：

```text
Segment 0 all blocks
→ Segment 1 all blocks
→ ...
```

长时间连续排列。推荐采用有限的 temporal striping：

```text
Active Segment Window
+ interleaved unique repair IDs
+ periodic Control/FinalManifest
```

使播放器短时掉帧、解码卡顿或 Capture interruption 不集中摧毁一个 Segment。

离线文件生成时必须计算：

```text
UniqueOuterSymbolsPerSegment
OfflineRepairMargin
SinglePassCompletionTarget
LongestBurstLossTolerated
```

Loop 只作为“再次观察相同方程”的容错手段，不计入 unique repair budget。

---

## 20.11 Playback Cadence 与 NVENC Capability Gate

CFR 必须冻结精确有理帧率：

```text
60/1
60000/1001
30/1
...
```

Profile 不能仅写“60 FPS”。目标播放器/显示器矩阵必须比较 60/1 与 60000/1001 等常见 cadence，并以实际 `UniqueVisualFPS` 和 frame repeat/drop 结果选择 Certified 值。

NVENC 初始化必须运行时查询：

```text
codec support
profile support
input format
max width/height
rate-control capability
lookahead/AQ capability
required driver/API compatibility
```

不能把某个开发机上的 preset GUID、rate-control mode 或 input format 当成所有 NVIDIA 驱动都永久可用。生成器 build metadata / benchmark log 至少记录：

```text
Video Codec SDK version
NVENC API version
NVIDIA driver version
GPU PCI/LUID identity
selected preset/tuning/rate-control
actual encoder caps
```

PixelBridge 只使用当前 SDK 支持的 preset/rate-control 路线；已被新版驱动移除的旧 NVENC preset/rate-control 枚举不得进入 Certified configuration。

---


## 20.12 MP4 Muxing 是独立模块

NVENC 只负责产生 H.264 elementary bitstream，并不自动得到一个可移植的 `.mp4` 文件。PixelBridge v1 把：

```text
PBVideoEncoderNvenc
PBMp4Mux
```

明确拆开。

Windows 原生首选 Mux 路线：

```text
NVENC H.264 Access Units
→ parse/validate SPS + PPS
→ Media Foundation MPEG-4 File Sink
→ MP4
```

`PBMp4Mux` 必须处理：

- 从 NVENC 序列头提取 SPS/PPS；
- 为 H.264 media type 正确设置 `MF_MT_MPEG_SEQUENCE_HEADER` 或通过已验证的 passthrough 路线提供参数集；
- MP4 output byte stream 必须 writable + seekable；
- 每个 compressed sample 设置精确 PTS / Duration；
- 保持 CFR rational timeline，无 timestamp accumulation drift；
- Finalize sink 后再把 artifact 视为完成；
- 用独立 bitstream/MP4 inspector 验证 `avcC/stsd`、SPS/PPS、sample count、PTS、duration、key-frame/IDR 与 VUI；
- 不把 NVENC Annex-B 输出“直接改扩展名为 .mp4”。

如果 Media Foundation MPEG-4 File Sink 在目标 NVENC output 组合上无法满足精确 sample/VUI/compatibility Gate，可替换为独立 MP4 muxer，但 muxer 仍只做容器封装，不进行第二次视频转码。

---



## 20.13 Large MP4 / Long-Duration Gate

“协议支持大文件”不自动等于“单个 MP4 muxer 对任意大 artifact 都安全”。

Offline 模式必须单独测试跨越典型容器边界的 artifact：

```text
> 4 GiB
> 16 GiB
long-duration 1 h / 6 h / 24 h
large sample-count
```

检查：

```text
mdat large-size handling
stco/co64 chunk offsets
movie/track/media duration
sample table size
seek/finalize memory
finalize latency
player seek/open time
```

如果首选 Media Foundation MPEG-4 File Sink 在某个规模上不能稳定生成/播放，则该规模不能继续宣传为 Certified single-MP4；必须切换经过验证的 muxer/fragmented MP4 方案，或明确 Offline Artifact 的 Certified 上限。这个上限属于视频容器实现，不影响即时模式和 PixelBridge 文件协议的 Segment 能力。

---


# 21. Windows 屏幕区域选择与 DPI

进程必须在任何顶层窗口创建前进入：

```text
Per-Monitor DPI Aware V2
```

优先通过应用 manifest / 进程启动阶段一次性声明，不在 Qt/Win32 HWND 已创建后动态切换 DPI awareness。

Region Selector：

```text
Virtual Desktop Overlay
→ Mouse drag
→ Signed Physical Pixel RECT
```

所有 Capture 坐标统一为物理像素。虚拟桌面坐标可能为负值，因此 monitor origin、mouse position、ROI offset 全程使用有符号坐标类型，不能把 `left/top` 存成无符号整数。

禁止直接把 Qt logical coordinates 当 DXGI/WGC coordinates。

Encoder Data Window 若要求 `1920×1080` canonical client area，必须保证的是：

```text
physical client pixels = 1920×1080
```

而不是 Qt logical size = 1920×1080。窗口跨 DPI monitor 迁移后必须重新验证 client physical size，必要时暂停 Session 并重建 presentation epoch。

---

## 21.1 优先单 Monitor ROI

高性能路径要求：

```text
ROI fully inside one HMONITOR
```

跨显示器支持属于兼容功能，不是性能基准场景。

---

## 21.2 Monitor Rotation

Desktop Duplication 在 90/180/270 度显示器上需要显式处理 rotation。

PixelBridge：

- 读取 output rotation；
- 将 ROI coordinate 映射到 duplication surface；
- 在 GPU Crop/采样阶段应用旋转；
- benchmark 中加入 portrait monitor。

不能假设 AcquireNextFrame surface 永远与用户视觉方向一致。

---

# 22. Windows 屏幕捕获 Backend 与归一化

实现两个 Backend：

```text
Windows.Graphics.Capture
DXGI Desktop Duplication
```

它们之上统一经过：

```text
PBCaptureNormalize
```

统一 cursor、rotation、pixel format、ContentSize、CaptureEpoch、timestamp 与 HDR/SDR 语义。

### OS 基线

建议 v1 明确两档：

```text
Minimum Functional Baseline: Windows 10 1903+（按 API capability 探测）
Certified Performance Baseline: Windows 11 24H2 / build 26100+
```

WGC 能力必须按实际 API contract 探测：

```text
CreateForMonitor         → Windows 10 1903+ baseline
IsCursorCaptureEnabled   → Windows 10 2004+ optional capability
IsBorderRequired         → newer contract + user consent/capability
MinUpdateInterval        → Windows 11 24H2/build 26100+ optional hint
```

尤其 `MinUpdateInterval` 不进入 Minimum Functional Baseline，也不进入协议正确性；当前 SDK 文档仍可能带预发布说明，因此只能把它当作运行时可选优化。

---

## 22.1 WGC

默认现代 Backend。

使用：

```text
IGraphicsCaptureItemInterop::CreateForMonitor
```

从 `HMONITOR` 创建 CaptureItem，再 GPU Crop ROI。

Capture D3D11 device 必须明确由目标 `IDXGIAdapter` 创建，并包装为 WGC 所需的 `IDirect3DDevice`。工程上至少遵守：

```text
hardware D3D11 device on capture adapter
D3D11_CREATE_DEVICE_BGRA_SUPPORT
do not use D3D11_CREATE_DEVICE_SINGLETHREADED unless all access is provably single-threaded
CreateDirect3D11DeviceFromDXGIDevice
```

Debug layer 只在开发/测试环境探测启用，不能因为目标机器未安装 SDK Layers 让 Release device creation 失败。

---

## 22.2 Frame Pool

优先：

```text
Direct3D11CaptureFramePool::CreateFreeThreaded
```

这样 FrameArrived 在内部 worker thread 触发，不依赖 UI DispatcherQueue。

Capture callback 只做：

```text
TryGetNextFrame
validate basic frame metadata
create bounded FrameLease token
timestamp / ContentSize
enqueue lease
return
```

不做 FEC / disk / heavy CUDA synchronization，也不把 `frame.Surface` 脱离 `Direct3D11CaptureFrame` 生命周期长期保存。

---

## 22.3 WGC Frame 生命周期

`TryGetNextFrame()` 取得的是 frame-pool lease。Frame 归还后，上层不得继续保存或访问其底层 `frame.Surface`。

推荐生命周期：

```text
FrameArrived
→ TryGetNextFrame
→ validate ContentSize / CaptureEpoch
→ create short-lived bounded FrameLease
→ enqueue to the single D3D submission owner
→ CopySubresourceRegion ROI → PixelBridge-owned texture
→ issue fence/event-query
→ move lease to very small GPU-retire queue
→ poll/retire completed copy
→ Close/Release Direct3D11CaptureFrame
```

这样既不在 frame 被 check-in 后继续使用底层 surface，也不要求 Capture callback 或 D3D owner **同步阻塞等待 GPU**。

Certified path 的原则是：

> 提交 copy 后可以异步继续，但在确认该 copy 不再读取 WGC surface 之前，不归还对应 `Direct3D11CaptureFrame`。

如果目标 D3D11 device 支持合适的 fence，则使用 fence；否则使用 D3D11 event query 等明确完成机制。不要为了“理论零等待”依赖 undocumented driver lifetime。

`FrameLease` + retire queue 必须很短；满时优先丢弃尚未提交的旧 Capture Frame。若 retire queue 长期占满，则说明 GPU/capture pipeline 已失速，应记录并降载，而不是继续持有越来越多 WGC frame。

如果 `frame.ContentSize` 与当前 frame pool / capture epoch 不一致：

```text
pause data submission
→ drain/release current frame
→ Recreate frame pool
→ recreate ROI texture ring if needed
→ reset geometry/calibration state
→ increment CaptureEpoch
```

不能拿旧尺寸 texture / geometry 继续解码新桌面。

---

## 22.4 Cursor 与 Decoder 自身窗口

WGC 支持时默认：

```text
IsCursorCaptureEnabled = false
```

Decoder 的 top-level 控制/统计窗口可在支持的系统上 best-effort 使用：

```text
SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)
```

用于减少控制 UI 污染捕获区；这不是安全边界，也不能替代“数据 ROI 不应被其他窗口覆盖”的基本要求。

Desktop Duplication 的 pointer 可能已合成进 surface，也可能以独立 pointer metadata 给出；`PBCaptureNormalize` 必须把上层语义统一为 `CursorExcluded=true`。

---

## 22.5 Borderless

`IsBorderRequired = false` 不是无条件保证，也不作为 PixelBridge 正确性的依赖。

在支持的系统上，关闭边框需要：

- 对应 API contract；
- 可声明 `graphicsCaptureWithoutBorder` 的应用打包/能力配置；
- `GraphicsCaptureAccess.RequestAccessAsync(Borderless)` 用户授权；

如果项目以传统 unpackaged Win32 方式发布，或用户拒绝权限，就必须接受系统 capture border 仍存在。

因此 Certified ROI 设计原则是：

> Data Grid 永远不要依赖 monitor 最外缘像素；即使 capture border 存在，也不能进入 Sync/Pilot/Data region。

是否采用 MSIX/package 以获取 Borderless capability 是产品部署决策，不是协议前置条件。

---

## 22.6 MinUpdateInterval

`GraphicsCaptureSession.MinUpdateInterval` 只在支持它的较新 Windows build 上探测使用。

把它视为：

> **可选 capture cadence hint，而不是“设置 1/144 秒就保证 144 FPS”的承诺。**

规则：

- Windows 10 Functional Baseline 不引用该属性；
- Win11 24H2+ 也必须先 capability probe；
- API 不存在、设置失败或实际 cadence 无提升时静默回退；
- 协议与 Goodput 不依赖它；
- 性能一律依据真实 `UniqueVisualFPS`。

```text
ConfiguredMinUpdateInterval
MeasuredCaptureFPS
MeasuredUniqueVisualFPS
```

三者必须分开记录。

---

## 22.7 Desktop Duplication

作为：

- fallback；
- performance comparison；
- 某些系统环境下替代路径。

必须处理：

- `DXGI_ERROR_ACCESS_LOST`；
- display mode change；
- desktop switch；
- rotation；
- pointer metadata；
- output recreate。

HDR / high-color 情况不能把 `DuplicateOutput` 与 WGC 当成等价输入。优先探测 `IDXGIOutput5::DuplicateOutput1` 并声明支持的 scan-out formats；若只能得到 BGRA8 且会丢失当前 Profile 所需的颜色动态范围，则必须降级到 SDR / Luma-only Profile 或切换 WGC。

---

# 23. GPU Adapter 拓扑与 GPU Backend 选择

模块：

```text
PBGpuTopology
```

职责：

```text
HMONITOR
→ IDXGIOutput
→ IDXGIAdapter
→ Adapter LUID
→ D3D11 Device
→ CUDA Device Mapping
```

---

## 23.1 GPU Backend 选择

首先在 Capture D3D11 Adapter 上探测并优先建立：

```text
PBDemodD3D11
```

它是跨 Intel / AMD / NVIDIA 的第一等 GPU baseline。

仅当：

```text
Capture D3D11 Adapter
== CUDA-compatible NVIDIA Adapter
```

且 benchmark 证明 CUDA 路径有明显收益时，才启用：

```text
D3D11 ROI Texture
→ cudaGraphicsD3D11RegisterResource
→ PBDemodCuda
```

---

## 23.2 Hybrid GPU

如果：

```text
Monitor → Intel iGPU
CUDA → NVIDIA dGPU
```

则：

```text
D3D11-CUDA Direct Interop = unavailable for that capture device
```

PixelBridge 必须自动选择：

1. D3D11 Compute demod；
2. CPU reference demod；
3. 经专门 benchmark/实现的 cross-adapter path。

禁止：

> 直接失败或 silently 做高代价 CPU roundtrip 而不告诉用户。

UI 应显示：

```text
Capture Adapter
CUDA Adapter
Interop Mode
Fallback Reason
```

---

# 24. D3D11 Compute 与 D3D11 → CUDA 高性能路径

## 24.1 D3D11 Compute Baseline

推荐最先实现的 GPU 路径：

```text
WGC / Duplication Texture
→ PixelBridge-owned ROI Texture
→ HLSL Compute Shader
→ Shape/Level/Chroma metrics
→ compact LLR buffer
→ CPU/GPU FEC stage
```

优点：

- 不跨 graphics API；
- 不要求 NVIDIA；
- 不存在 CUDA graphics map/unmap；
- 与 capture adapter 天然一致；
- 适合作为 CUDA 的 correctness/performance 对照。

---

## 24.2 CUDA Same-Adapter Fast Path

```text
WGC / Duplication Full Monitor Texture
↓
CopySubresourceRegion
↓
PixelBridge-owned ROI Texture Pool
↓
cudaGraphicsMapResources
↓
CUDA Array / Texture Object
↓
Demod Kernel
↓
cudaGraphicsUnmapResources
```

---

## 24.3 Persistent Registration

ROI textures 初始化时：

```text
cudaGraphicsD3D11RegisterResource
```

一次注册。

禁止每帧 register/unregister。

---

## 24.4 Map/Unmap 仍然有同步成本

“Persistent Registration”不等于：

```text
每帧完全无同步
```

每帧仍需要 map/unmap，并协调 D3D 与 CUDA 对同一资源的访问。

因此必须 benchmark：

- 2/3/4 texture ring；
- map/unmap latency；
- GPU bubble；
- CPU wait；
- D3D copy completion。

不要为追求理论零拷贝造成比一次小型 GPU copy 更严重的同步停顿。

---

## 24.5 “零 CPU 回读”的准确含义

PixelBridge 的 fast path 目标是：

> **桌面像素不经过 GPU→CPU→GPU 的 round-trip。**

CPU 仍负责：

- 调度；
- metadata；
- queue；
- FEC；
- control。

---


## 24.6 LLR Readback 也必须异步流水化

如果 Demod 在 GPU、LDPC 在 CPU，则真正需要回读的是 compact LLR/Transport candidate，而不是桌面原始像素。

D3D11 Compute 路径：

```text
GPU LLR Buffer
→ CopyResource/CopySubresourceRegion to staging/readback ring
→ event query/fence
→ Map only completed slot
→ CPU LDPC
```

CUDA 路径：

```text
CUDA LLR Buffer
→ cudaMemcpyAsync
→ page-locked host buffer ring
→ event completion
→ CPU LDPC
```

禁止每帧：

```text
dispatch
→ immediate blocking Map / cudaDeviceSynchronize
→ CPU decode
```

把整个 GPU pipeline 串行化。

如果未来实现 GPU LDPC，则允许 LLR 继续驻留 GPU，只在 Transport Block/CRC 结果层回 CPU；是否值得由 end-to-end benchmark 决定。

Telemetry：

```text
LlrReadbackBytesPerSec
LlrReadbackLatencyP50/P95/P99
GpuToCpuStallTime
LlrRingHighWater
```

---


# 25. HDR / SDR / 色彩管理

## 25.1 v1 认证基线：SDR

为了尽快得到稳定协议，建议 PixelBridge v1 的 Certified Profile 先以：

```text
SDR
BT.709
8-bit desktop path
```

作为认证基线。

---

## 25.2 HDR

Windows HD Color 开启时，WGC capture 可能需要：

```text
DXGI_FORMAT_R16G16B16A16_FLOAT
```

避免 BGRA8 overclipping。

因此 HDR 处理分两种：

### Certified v1

UI 提示用户将数据窗口置于 SDR 显示路径，或自动切换到 Luma-only robust profile。

### Experimental HDR

```text
FP16 WGC
→ defined HDR/SDR normalization
→ calibrated Y/chroma
```

通过完整 HDR test matrix 后再升级为 Certified。

---

# 26. Visual Frame 定位、尺度与相位恢复

没有 camera homography。

模型主要为：

```text
axis-aligned rect
+ scaleX
+ scaleY
+ phaseX
+ phaseY
+ optional rotation
```

---

## 26.1 Auto Refine

用户粗框：

```text
player/window neighborhood
```

Decoder 在 ROI 内通过：

- Sync Border；
- Corner Marker；
- timing pattern；

得到 `ExactVisualRect`。

---

## 26.2 不进行整帧 normalize resize

推荐直接：

```text
logical cell coordinate
→ inverse transform
→ CUDA texture sampling
```

避免多一次：

```text
Full ROI → 1920×1080 temporary image
```

---

# 27. 软解调、硬判决与 LLR 校准

## 27.1 Shape Metric

对每个 Cell 得到：

```text
distance[shapeId]
```

候选 metric：

- normalized SSD；
- NCC；
- weighted template distance；
- gradient-aware metric；
- learned small descriptor（仅作为后续实验）。

---

## 27.2 Shape Bit LLR

Max-Log 近似：

```text
LLR(bit k)
≈ min metric(symbols with bit=1)
 - min metric(symbols with bit=0)
```

符号正负约定在协议测试中固定。

---

## 27.3 Chroma LLR

Calibration 后在 capture-space constellation 中计算：

```text
distance to calibrated chroma centroids
```

得到对应 bit LLR。

对于 covariance 明显各向异性的 constellation，可使用归一化 Mahalanobis-like metric，而不是简单 RGB Euclidean distance。

---

## 27.4 Direct-Level Metric / LLR

`PB-Mod-DesktopLevels-X` 的每个 tile 使用 Pilot 得到：

```text
centroid[level]
variance/covariance[level]
```

若 Probe 证明是 byte-exact mapping：

```text
captured integer code value
→ exact lookup
→ hard label
```

否则：

```text
sample
→ distance to calibrated level centroids
→ candidate level metrics
→ Max-Log bit LLR
```

不需要先把 Capture 像素“逆变换”回理论 Sender RGB；直接在观测空间分类通常更稳健。

---

## 27.5 Reliability Scaling

LLR 还要根据：

- scale；
- local contrast；
- calibration residual；
- per-cell reliability map；
- chroma saturation；
- frame blend confidence；
- sync/border correlation；
- decision margin；
- clipping state；

进行可靠度缩放。

对于明确不可判断的 cell/tile，应输出接近 0 的 LLR，让 LDPC 把它当低置信度信息，而不是输出一个幅值很大的错误硬判决。

---

## 27.6 LLR Calibration Gate

“最小距离差”不天然等于统计意义上的 LLR。

PBBenchmark 必须通过真实 replay 数据校准：

```text
metric margin
→ empirical bit error probability
→ LLR scale / clipping
```

至少比较：

```text
uncalibrated raw metric
global scale
per-signal-profile scale
piecewise / lookup calibration
```

并用 held-out capture dataset 测量：

```text
BER vs |LLR|
LDPC FER
iterations P50/P95/P99
overconfidence error rate
```

核心目标：

> **LLR 不仅要排序正确，还必须避免“错误 bit 获得极高置信度”。**

LLR quantization、decoder algorithm 和 max iteration 属于 implementation tuning；Profile 冻结的是 bit mapping、matrix 和必要的 metric semantics，不要求不同 backend 浮点数组逐 bit 相同。

---

---

# 28. 桌面帧丢失、重复、混合帧与时序

Capture 不保证：

```text
Visual Frame == Capture Frame 1:1
```

可能：

```text
100
100
101
103
103
104
```

---

## 28.1 FrameSequence

每张 Visual Frame 的 Bootstrap 固定携带：

```text
FrameSequence : uint64
```

---

## 28.2 Duplicate

相同 FrameSequence：

- 若所有 codeword 已成功：忽略；
- 若上次部分 codeword 失败：允许再次评估失败 codeword；
- 若新观测与旧观测的像素/metric 基本相同：视为高度相关重复，忽略而不是重复累加置信度；
- 只有观测确实不同且满足可靠性 Gate 时，才允许 capped LLR combine 或选择更高置信度的一次。

LLR combine 前必须确认：

- Bootstrap 同帧；
- geometry compatible；
- color calibration compatible；
- 没有明显 blend；
- `captureEpoch` 相同；
- `DuplicateCombineCount` 未超过 Profile 的本地上限。

禁止对同一静态 FrameSequence 无限求和 LLR。重复观测通常是强相关样本，把它们当独立证据会制造虚假的超高置信度，并可能让错误 bit 更难被 LDPC 修正。

---

## 28.3 Frame Blending / Interpolation

检测特征：

- Bootstrap correlation 同时对两个 sequence 有响应；
- Data residual 异常；
- timing pattern 双影；

直接：

```text
Frame = Erasure
```

不要把 blend frame 的错误 bit 强送 LDPC。

---

# 29. Decoder 完整流水线

```text
Region Selector
↓
Capture Backend
↓
Capture Normalize / CaptureEpoch
↓
Adapter-aware D3D11 Texture
↓
GPU ROI Crop
↓
Bootstrap Decode
↓
FrameSequence / VisualProfile / InterleavePhase
↓
Geometry / Scale / Phase
↓
Pilot Decode / Capture-space Calibration
↓
PersistentBadRegion / Reliability Map
↓
Soft Demodulation or eligible hard decision
↓
Inverse Spatial Interleave
↓
Inner FEC Profile Dispatch
├─ QC-LDPC → Syndrome
└─ Eligible Erasure-Only → hard decision / CRC gate
↓
Transport CRC32C
↓
OuterFecMode dispatch
├─ Wirehair V2
└─ DirectRepeat reassembly
↓
Encoded Segment Verify
↓
Bounded Zstd Decompression / RAW
↓
Raw Segment Verify
↓
Random Write output.part
↓
Segment Map complete?
├─ No  → continue receiving
└─ Yes
    ↓
Sequential WholeFileDigest over output.part
    ↓
FinalManifest Match
    ↓
Same-volume Atomic Final Rename
```

---

---

# 30. 线程模型、队列、同步与内存

## 30.1 Encoder

```text
UI Thread
File I/O Thread
Compression Worker(s)
Wirehair / Packet Worker(s)
LDPC / Frame Worker(s)
Render Thread
或 NVENC Thread
```

---

## 30.2 Decoder

```text
UI
Capture callback
D3D ROI submit
Demod worker / CUDA stream
LDPC worker pool
Wirehair / Segment worker
Disk writer
Telemetry
```

---

## 30.3 Capture Queue

实时源优先“新鲜度”。

如果 backlog：

```text
drop oldest undecoded capture frame
```

禁止无限增长 latency。

---

## 30.4 Bounded Queue

每条队列：

```text
fixed capacity
explicit drop policy
high-water telemetry
```

不要依赖无界 `std::queue`。

---

## 30.5 D3D11 Immediate Context Ownership

不要让 UI、capture callback、demod worker 多线程随意同时调用同一个 D3D11 immediate context。

推荐：

```text
One D3D submission owner thread
+ bounded command/token queues
+ explicit texture-ring ownership
```

如果确实需要跨线程访问，必须显式评估 `ID3D11Multithread` 保护与同步成本。默认设计以“单 owner + GPU ring”减少隐式 driver lock。

---

## 30.6 Hot Path Allocation

初始化：

- ROI texture ring；
- CUDA registration；
- LLR buffer；
- LDPC workspace；
- transport buffers；
- frame descriptors；
- object pool。

热点路径目标：

```text
no general-purpose heap allocation
```

---

# 31. 断点恢复与状态持久化

Decoder：

```text
output.part
resume.state
```

---

## 31.1 Completed Segment

保存：

```text
SessionId
SegmentOrdinal
RawOffset
RawSize
RawDigest
```

---

## 31.2 Active Fountain

不直接序列化第三方 codec 私有结构。

保存：

```text
SegmentOrdinal
WirehairSerializedProfile
Received(OuterBlockId, Payload)
```

重启：

```text
create Wirehair decoder
→ replay validated packets
```

---

## 31.3 Active DirectRepeat

对于 `OuterFecMode = DirectRepeat`，保存：

```text
SegmentOrdinal
DirectBlockCount
Received(BlockOrdinal, Payload)
```

重启后只需要恢复经过 CRC/descriptor 验证的直接块集合。

---

## 31.4 防止 resume.state 无限膨胀

- 只保存未完成 Segment；
- 完成后立即删除该 Segment 的 packet cache；
- 设置 active incomplete segment 上限；
- 做磁盘配额；
- cache records 自身有 CRC / length bounds；
- 如果活跃 Wirehair Segment 的 packet cache 达到持久化配额，live decoder 可以继续工作，但必须明确标记 `ResumeDegraded`，不能静默假装重启后仍可完整恢复该 Segment。

---

## 31.5 Crash Consistency 与最终文件发布

`output.part` 必须创建在最终目标目录所在卷，确保最后 rename 不需要跨卷 copy。

v1 提供两档持久化语义：

```text
FastResume
CrashSafeResume
```

`CrashSafeResume` 中，一个 Segment 被写入 `completed` 状态前至少满足：

```text
random write segment bytes
→ flush required file range / file handle according to policy
→ atomically replace resume.state
```

`resume.state` 使用：

```text
resume.state.tmp
→ write + checksum
→ FlushFileBuffers
→ atomic replace/rename
```

如果进程异常退出后无法证明 `.part` 与 `resume.state` 的提交顺序，重启时重新计算相关已完成 Segment 的 `RawDigest`，而不是盲信状态文件。

所有 Segment 完成后，Decoder 按原始文件顺序**再次顺序扫描 `output.part`**，计算 FinalManifest 指定的 WholeFileDigest。只有 whole-file digest 成功后才执行同卷 atomic rename 并显示 `Verified Complete`。

因此需要单独统计：

```text
FinalVerificationLatency
FinalRenameLatency
```

---

# 32. 输入文件一致性与源文件变更防护

即时 Carousel 会多次重新访问旧 Segment。

如果源文件在传输期间被其他程序修改：

```text
Pass 0 Segment 5 = A
Pass 1 Segment 5 = B
```

会造成灾难性的“同 Session 混合内容”。

---

## 32.1 文件锁策略

Windows 打开源文件时建议：

```text
FILE_SHARE_READ
```

而不允许其他进程写入/删除。

同时记录：

- file size；
- file identity；
- last write information；

发现变化立即终止 Session。

---

## 32.2 不要把完整压缩 Segment 永久留在内存

Carousel 后续 Pass 有两种实现策略：

1. 使用受控磁盘 cache 保存该 Session 的 exact Encoded Segment bytes；
2. 从稳定源文件重新读取并重新编码整个 Segment。

第二种策略**不得一边重新压缩一边立即发 Fountain packet**。必须先完整得到 Encoded Segment，计算 `EncodedDigest`，确认与第一次 Descriptor 完全一致后，才允许用已保存的 Wirehair V2 descriptor 创建 encoder 并继续产生新的 `OuterBlockId`。

协议要求：

> 同一 SegmentOrdinal 的 EncodedDigest 必须完全一致。

Encoder 进程重启后，如果不能恢复**完全相同**的 SessionDescriptor、Segment 编码结果、Wirehair descriptor 与 next repair-id progression，则必须生成新的 `SessionId`，不能假装继续旧 Session。

zstd 的 compression level / worker 参数属于 Encoder tuning；Decoder 只依赖标准 zstd frame 与协议边界。不同 zstd 版本/worker 参数即使解压后得到相同 raw bytes，也可能产生不同 compressed bytes；同一 Session 不允许把这些不同 byte stream 混进同一个 Wirehair message。

---

# 33. 输出路径与不可信文件名安全

视觉输入本质上是外部输入。

`FileNameUtf8` 不能直接拼接成 Windows path。

必须：

- 只取 basename；
- 拒绝绝对路径；
- 拒绝 `..` traversal；
- 拒绝 NTFS ADS `:`；
- 过滤 NUL；
- 处理 `CON/PRN/AUX/NUL/COM1...`；
- 处理尾随 dot/space；
- 限制 UTF-8/UTF-16 长度；
- 不 silently overwrite；
- 最终输出目录由用户本地选择，发送端不能指定任意绝对路径。

---

## 33.1 解压边界

Segment Descriptor 必须在分配前验证：

```text
RawSize <= ConfiguredMaxSegmentBytes
EncodedSize <= ConfiguredMaxEncodedSegmentBytes
RawOffset + RawSize <= OriginalFileSize
```

Zstd：

- 限制输出 bytes；
- 限制 window/resource；
- 拒绝 decompression bomb；
- 任何 size mismatch 失败。

---

## 33.2 ReceiverResourcePolicy

视觉链路输入视为不可信网络输入。Decoder 启动时建立本地资源策略，任何来自 Sender 的 size/count 都只能在策略范围内使用。

建议至少包含：

```text
MaxAcceptedFileBytes
MaxSegmentCount
MaxRawSegmentBytes
MaxEncodedSegmentBytes
MaxConcurrentSessions
MaxActiveOuterFecDecoders
MaxOrphanTransportBytes
MaxControlRecordBytes
MaxControlReassemblyBytes
MaxResumeBytes
MaxZstdWindowBytes
MaxOutputPreallocationBytesWithoutPrompt
```

对于超过本地自动接受阈值的 `OriginalFileSize`，Decoder 先显示文件名、大小、Session 信息，由用户确认后再创建/扩展 `.part`。

所有配额失败必须作为可观测状态返回，例如：

```text
RejectedByResourcePolicy
ControlReassemblyQuotaExceeded
OuterFecDecoderQuotaExceeded
OutputReservationDenied
```

不得以 OOM、磁盘写满或异常退出作为正常拒绝机制。

---

## 33.3 输出文件预分配与发布规则

- `.part` 与最终文件位于同一目标目录/卷；
- 不使用 `SetFileValidData` 绕过正常零初始化语义；
- 大文件预分配受 `ReceiverResourcePolicy` 与磁盘剩余空间约束；
- 不覆盖已有目标文件；
- whole-file digest 未通过时绝不把 `.part` rename 成最终文件；
- rename 失败时保留已验证 `.part` 与状态，向用户报告可恢复错误。

---

# 34. Visual Profile：认证配置与实验配置

Profile 不是单纯“速度档”。

它冻结所有会影响 raster、判决或 wire interpretation 的参数：

```text
ChannelClass
Canvas
Sync/Bootstrap/Control/Pilot/Guard region map
DataGridColumns/Rows
DataGridOriginX/Y
CellPitch or TileWidth/TileHeight
SignalProfileId
PresentationProfileId
FrameHoldVBlanks / exact FrameRate semantics
ModulationFamilyId
ShapeCodebookId
ShapeBits
LevelConstellationId
ChromaConstellationId
ChromaBits
InnerFecProfileId
InterleaveProfileId / phase rule
FramePacking
CalibrationProfile
MinimumDecisionMargin policy
```

实现 tuning（线程数、LDPC iteration、GPU backend 等）不进入 `VisualProfileId`。

---

## 34.1 PB-VideoSafe-1（Certified 候选）

适合：

- MP4；
- resize；
- 普通播放器；
- 兼容优先。

```text
Canvas           = 1920×1080
DataGridOrigin   = (60, 60)
CellPitch        = 8
SignalProfile    = PB-Signal-Video709Limited-1
Presentation     = CFR video / exact fps from Offline Profile
Modulation       = PB-Mod-ShapeChroma-1
DataGrid         = 225×120 = 27,000 cells
Shape            = 16 / 4 bits
Chroma           = 4 / 2 bits
BitsPerCell      = 6
RawDataBits      = 162,000 bits
LDPC N           = 16,200
Codewords/Frame  = 10
LDPC K           = 10,800
```

每帧 LDPC information bytes：

```text
10 × 1350 = 13,500 bytes
```

减 Transport framing 后，候选 Fountain payload 约 13 KiB/frame。

该几何只有在 1920×1080 Canvas 中同时容纳最终 Bootstrap / Control / Pilot / Guard region 后才能冻结。

---

## 34.2 PB-VideoBalanced-1（Certified 候选）

```text
Canvas           = 1920×1080
DataGridOrigin   = (60, 54)
CellPitch        = 6
SignalProfile    = PB-Signal-Video709Limited-1
Presentation     = CFR video / exact fps from Offline Profile
Modulation       = PB-Mod-ShapeChroma-1
DataGrid         = 300×162 = 48,600 cells
Shape            = 16 / 4 bits
Chroma           = 4 / 2 bits
BitsPerCell      = 6
RawDataBits      = 291,600 bits
LDPC N           = 16,200
Codewords/Frame  = 18
LDPC K           = 11,880
```

每帧 LDPC information bytes：

```text
18 × 1485 = 26,730 bytes
```

这是 LocalVideo 的高吞吐候选，而不是在未验证前直接上 32 Shape。

---

## 34.3 PB-DesktopFast-1（Shape Reference / Fallback 候选）

该 Profile 用来建立 LocalDesktop 的 Shape+Chroma 可复现实验基线：

- 即时 D3D 输出；
- 1:1 或接近 1:1；
- SDR；
- 高质量桌面捕获。

```text
Canvas           = 1920×1080
DataGridOrigin   = (96, 40)
CellPitch        = 4
SignalProfile    = PB-Signal-DesktopSRGB-1
Presentation     = Flip/VSync, FrameHoldVBlanks=1 candidate
Modulation       = PB-Mod-ShapeChroma-1
DataGrid         = 432×250 = 108,000 cells
Shape            = 16 / 4 bits
Chroma           = 4 / 2 bits
BitsPerCell      = 6
RawDataBits      = 648,000 bits
LDPC N           = 16,200
Codewords/Frame  = 40
LDPC K           = 13,320
```

每帧 LDPC information bytes：

```text
40 × 1665 = 66,600 bytes
```

该 Profile 的价值是提供保守、可对照的 LocalDesktop baseline。它不在 Direct-Level Gate 之前获得“最终 Desktop Certified 物理层”的地位。

---

## 34.4 PB-DesktopNative-X（LocalDesktop 第一等候选）

```text
ChannelClass        = PB-Channel-LocalDesktop-1
SignalProfile       = PB-Signal-DesktopSRGB-1
PresentationProfile = PB-Present-LocalDesktop-1
ModulationFamily    = PB-Mod-DesktopLevels-X
Physical alignment  = required
Scale               = 1.0 / strict gate
Tile                 = 2×2 or 4×4 candidate
Luma/RGB levels      = PB-LinkProbe-1 + benchmark
Pilot calibration    = required
InterleavePhase      = required
Inner FEC            = QC-LDPC high-rate candidate
                       or PB-InnerFEC-ErasureOnly-X experiment
```

它不预设 bits/pixel。Phase 1 的 `PB-LinkProbe-1` 先回答：

```text
1×1 capture 是否 byte-exact？
多少个 level 能稳定分离？
2×2 与 4×4 的最低 margin 是多少？
WGC 与 Desktop Duplication 是否一致？
色彩管理/HDR/ICC 会造成怎样的变换？
```

然后才冻结：

```text
Tile geometry
LevelConstellationId
BitsPerTile
FEC rate
Pilot overhead
Payload bytes/frame
```

若 Direct-Level 在真实 Gate 中没有稳定优势，则 LocalDesktop 继续使用 `PB-DesktopFast-1` Shape baseline，不因设计偏好强行采用 Native。

---

## 34.5 PB-DesktopTurbo-X（Experimental）

Shape 路线仍可研究：

```text
CellPitch = 4
Shape     = 32
Chroma    = 4
Bits/Cell = 7
```

但它的优先级低于 `PB-DesktopNative-X` Gate。

只有当：

- CodebookOptimizer；
- 1:1 capture；
- scale sweep；
- real capture replay；
- calibrated LLR；
- Pilot margin；
- final VerifiedEncodedGoodput；

全部证明有稳定增益后才继续。

---

## 34.6 Profile Freeze Gate

任何 `*-1` Certified Profile 必须生成一份 machine-readable manifest，至少包含：

```text
VisualProfileId
CanonicalRasterHash
exact region map
exact signal values / constellation
exact data cell/tile count
exact Inner-FEC packing
Transport payload bytes/frame
InterleaveProfileId
Pilot layout
supported ChannelClass
tested scale/fps/HDR/player/backend matrix
minimum decision-margin gate
measured payload ceiling
```

设计文档中的候选数字只有在该 manifest 与 Golden Vector 一致后才成为协议常量。

---

---

# 35. 1080p60 理论容量预算与性能目标

容量计算分为：

```text
Canonical Data Grid / Tile Grid
→ RawVisualBits
→ Inner-FEC Information Bytes
→ Transport/Fountain Payload Bytes
→ VerifiedEncodedGoodput
→ VerifiedRawGoodput
```

其中 `Transport/Fountain Payload Bytes` 才是当前 Profile 在不考虑丢帧、Fountain repair 和调度损失时能够承载 Encoded Segment bytes 的候选上限。

Profile 比较视觉信道性能时以 `VerifiedEncodedGoodput` 为主；用户界面同时显示 `VerifiedRawGoodput`。认证 benchmark 必须包含 incompressible random corpus，避免把 zstd 压缩收益误算成物理层吞吐提升。

下列 Shape Profile 数字成立的前提是其当前候选 DataGrid 在最终 Sync / Bootstrap / Control / Pilot / Guard 布局中仍能原样保留。若 Profile Freeze Gate 调整网格，容量必须从 canonical layout 重新计算。

---

## 35.1 PB-VideoSafe-1

```text
LDPC codewords/frame       = 10
LDPC information bytes/cw  = 1350 B
Transport overhead/cw      = 36 B
Fountain payload/cw        = 1314 B
Fountain payload/frame     = 13,140 B
```

60 unique Visual FPS：

```text
13,140 × 60 = 788,400 B/s
≈ 769.9 KiB/s
≈ 0.752 MiB/s
```

这仍没有扣除：

- capture erasure；
- LDPC failure；
- Fountain overhead；
- control cadence 对时序的影响；
- final verification latency。

v1 目标：

```text
VerifiedEncodedGoodput ≥ 500 KiB/s
```

Stretch：

```text
≈ 650 ~ 700 KiB/s
```

---

## 35.2 PB-VideoBalanced-1

```text
LDPC codewords/frame       = 18
LDPC information bytes/cw  = 1485 B
Transport overhead/cw      = 36 B
Fountain payload/cw        = 1449 B
Fountain payload/frame     = 26,082 B
```

60 unique Visual FPS：

```text
26,082 × 60 = 1,564,920 B/s
≈ 1,528.2 KiB/s
≈ 1.492 MiB/s
```

v1 目标：

```text
VerifiedEncodedGoodput ≥ 1.0 MiB/s
```

Stretch：

```text
≈ 1.3 ~ 1.4 MiB/s
```

---

## 35.3 PB-DesktopFast-1 Shape Reference

```text
LDPC codewords/frame       = 40
LDPC information bytes/cw  = 1665 B
Transport overhead/cw      = 36 B
Fountain payload/cw        = 1629 B
Fountain payload/frame     = 65,160 B
```

60 unique Visual FPS：

```text
65,160 × 60 = 3,909,600 B/s
≈ 3,818.0 KiB/s
≈ 3.728 MiB/s
```

因此，在当前候选 Shape DataGrid/framing 不变、incompressible corpus、60 unique FPS 的条件下：

> **3.728 MiB/s 是 `PB-DesktopFast-1` 的候选 Fountain payload ceiling，不是 LocalDesktop 这个 ChannelClass 的总上限。**

实际 `VerifiedEncodedGoodput` 还要扣：

- failed LDPC；
- capture duplicate/drop；
- Wirehair repair overhead；
- Carousel / temporal striping scheduling；
- control/pilot导致的最终 layout 调整；
- resource stalls。

Shape baseline 的工程目标：

```text
VerifiedEncodedGoodput ≥ 2.0 MiB/s
```

参考 Stretch：

```text
≈ 3.0 ~ 3.4 MiB/s
```

---

## 35.4 PB-DesktopNative-X 不预设吞吐上限

Direct-Level 的容量必须在 `PB-LinkProbe-1` 和 Phase 1～2 Gate 后由真实 tile/constellation 决定：

```text
TilesPerFrame
× BitsPerTile
× InnerFecRate
- TransportOverhead
= CandidatePayloadPerFrame
```

再乘：

```text
MeasuredUniqueVisualFPS
```

得到 payload ceiling。

因此设计阶段不写一个臆测的 “5/10/20 MiB/s” 目标。Profile Freeze 后必须同时报告：

```text
RawVisualBits/frame
PayloadBytes/frame
ModeledPayloadCeiling
VerifiedEncodedGoodput
Efficiency = VerifiedEncodedGoodput / ModeledPayloadCeiling
```

LocalDesktop v1 的最低产品 Gate 是：

```text
selected Certified Desktop profile
→ VerifiedEncodedGoodput ≥ 2.0 MiB/s @ measured 60 unique FPS
```

若 Direct-Level 没有显著超过 Shape baseline，则不升级为 Certified 主线。

---

## 35.5 VerifiedRawGoodput 与压缩收益

对高压缩率源文件：

```text
VerifiedRawGoodput > VerifiedEncodedGoodput
```

完全可能。

但任何超过视觉 payload ceiling 的 `VerifiedRawGoodput` 都必须明确标记：

```text
CompressionGain = RawBytes / EncodedBytes
```

不能写成“视觉信道本身变快”。

---

## 35.6 120 / 144 FPS

容量只能按：

```text
Measured UniqueVisualFPS
```

线性外推。

不要用：

```text
Monitor refresh = 144 Hz
```

直接推导：

```text
Decoder unique data frames = 144 fps
```

高刷 Profile 必须分别通过：

- sender Present pacing；
- WGC/DXGI actual frame cadence；
- DWM composition；
- GPU demod budget；
- CPU/GPU LDPC budget；
- player decode cadence（Offline）；

之后才能建立认证 Goodput。

---

## 35.7 End-to-End Verified Goodput 的计时边界

`VerifiedRawGoodput` 的终点是：

```text
WholeFileDigest verified
```

而不是“最后一个 Segment 写入 `.part`”。

推荐同时记录：

```text
PreparationLatency
TransmissionElapsed
FinalVerificationLatency
TimeToVerifiedComplete
VerifiedEncodedGoodput
VerifiedRawGoodput
```

这样源文件准备时间、最后一次顺序 hash 的磁盘成本和实际传输时间都不会被混在一起。

---

---

# 36. 链路训练与能力探测

PixelBridge 在正式 Session 前允许运行固定的：

```text
PB-LinkProbe-1
```

它不是文件协议的一部分，不创建 `SessionId`，也不依赖当前 Data Visual Profile。

Probe 以低密度、强同步的确定性循环显示：

```text
FrameSequence / timing
neutral gray ladder
RGB/luma level ladder
active chroma candidates
2×2 / 4×4 tile patterns
phase/checker patterns
Shape reference patterns
```

Decoder 测量：

- sender presentation mode / refresh；
- capture backend；
- capture surface resolution；
- adapter；
- D3D11/CUDA interop；
- unique FPS；
- duplicate ratio；
- frame arrival jitter；
- SDR/HDR；
- scale / physical-pixel alignment；
- `ByteExactRatio`；
- per-channel transfer LUT；
- level centroid / variance；
- decision margin P50/P01/P001；
- SignalProfile / color calibration residual；
- Bootstrap success rate；
- present/capture phase stability；
- mixed/torn frame rate。

Probe 结果分为：

```text
CertifiedEligible
DegradedButUsable
Unsupported
```

并给出推荐 Profile，而不是只显示“成功/失败”。

---

## 36.1 LocalDesktop Profile Recommendation

推荐逻辑：

```text
1:1 + stable levels + very low raw error
    → PB-DesktopNative candidate

1:1 but level margin insufficient
    → PB-DesktopFast Shape baseline

non-1:1 / unstable color transform
    → robust Shape profile or reject Certified LocalDesktop

HDR / color mode unsupported
    → SDR requirement / compatible fallback
```

这些阈值属于 Certified implementation policy，必须由 real-capture benchmark 冻结，不能只凭经验写死。

---

## 36.2 无反向 ACK 的限制

Decoder 不能自动把结果发给 Encoder。

因此：

- Decoder 运行 `PB-LinkProbe-1` 后显示推荐 Profile；
- 用户在 Encoder 选择相同 Profile；
- Encoder 随后创建新的 Data Session；
- 或双方使用预先约定的默认 Certified Profile。

不要在协议文档中暗示存在自动速率反馈。

如果未来允许可选反向信道，应定义独立 Extension，不能让 v1 单向 wire semantics 隐式依赖它。

---

---

# 37. 日志、遥测与链路质量统计

## Presentation

```text
PresentCallFPS
PresentedVisualFPS
PresentRefreshCount
PresentGlitchCount
PresentQueueLatency
PresentationEpoch
DataWindowPhysicalSize
```

## Capture

```text
CaptureFPS
UniqueVisualFPS
FrameArrivalJitter
DuplicateFrameRatio
DroppedByDecoder
CaptureAdapter
Backend
InteropMode
ROI GPU Copy Time
FrameLeaseHighWater
FrameLeaseDropCount
CaptureEpoch
```

## Visual / Probe / Pilot

```text
BootstrapSuccessRate
FrameLocateConfidence
ScaleX/ScaleY
PhaseX/PhaseY
ByteExactRatio
CalibrationResidual
PilotClippingRatio
PilotLevelMarginP50/P01/P001
ShapeMetricMargin
ChromaMetricMargin
DirectLevelMetricMargin
PersistentBadRegionRatio
BlendFrameRatio
InterleavePhase
```

## FEC

```text
PreFecBerEstimate
LDPC FER
LDPC Iterations P50/P95/P99
CRC Failure
```

## Outer FEC / Scheduler

```text
UniqueOuterSymbols
DuplicateOuterSymbols
AcceptedOuterSymbols
WirehairExtraInsufficient
OuterBlockConflict
OuterDecoderResetCount
RepairRatio
SegmentRecoveryTime
ActiveSegmentWindowSize
OuterDecoderPeakActive
BurstErasureLengthP50/P95/P99
StripingRecoveryBenefit
```

## End-to-End

```text
VerifiedRawGoodput
VerifiedEncodedGoodput
VisualInformationRate
PreparationLatency
FinalVerificationLatency
TimeToVerifiedComplete
WholeFileDigestStatus
CarouselCycleTime
EstimatedWorstLateJoinTime
ResumeBytes
DiskWriteRate
RejectedByResourcePolicy
ControlReassemblyBytes
OrphanTransportBytes
```

## Offline Video

```text
MP4 Bitrate
Peak Frame Bytes
OfflineArtifactExpansionRatio
H264 Profile/Level
DeblockingMode
QP
Player Decode Drops
PostCapture FER
```

---

# 38. 仿真、Golden Vector 与 Benchmark 体系

## 38.1 PBVideoChannelSimulator

支持：

```text
YUV420
BT.709 full/limited
H.264 encode/decode
H.264 deblocking on/off
SignalProfile range / clipping
integer / fractional resize
bilinear / bicubic
letterbox
crop
frame blend
tearing / mixed-frame composition
gamma
HDR-like transfer
sharpen
noise reduction
chroma phase
```

---

## 38.2 Golden Vector

必须冻结：

- Protocol serialization；
- Bootstrap bytes；
- Wirehair serialized descriptor samples；
- LDPC codewords；
- interleave mapping + phase rule；
- Shape/Chroma mapping；
- Direct-Level constellation / bit labels；
- Pilot / calibration raster；
- complete Visual Frame PNG/raw raster；
- exact region map / capacity manifest；
- decoded Transport Blocks。

Wire bytes、LDPC codeword、interleave、canonical raster 与 Transport recovery 任何重构都跑 bit-exact regression。

GPU demodulator 内部允许使用不同浮点运算顺序，因此不要求 `float metric array` 逐 bit 相等。Backend Gate 分两层：

```text
canonical fixed-point/int8 LLR output -> exact when implementation chooses deterministic quantization
or
floating metric/LLR -> bounded numeric tolerance + identical accepted Transport Blocks
```

最终 `Transport Block bytes / Segment bytes / WholeFileDigest` 必须与 CPU reference 完全一致。

---

## 38.3 Real Capture Replay

真实 dataset 必须同时保存：

```text
Sender canonical raster / FrameSequence
Captured ROI frame
Capture Backend / pixel format
Display/adapter identity
DPI / scale / HDR state
Presentation timing
Capture timestamp / CaptureEpoch
```

这样既能做：

```text
Sender Pixel
→ Captured Pixel
```

的 Pixel Round-Trip 分析，也能在不重新播放的情况下回放：

```text
Locate / Pilot Calibration
→ Demod
→ LDPC
→ Packet recovery
```

数据集至少分为：

```text
LocalDesktop-ExactCandidate
LocalDesktop-Degraded
LocalVideo-CertifiedPlayer
LocalVideo-GenericPlayer
FailureCases
```

它将是最重要的算法回归数据集之一。

---

## 38.4 Benchmark Matrix

至少覆盖：

### Scale

```text
1.00
0.90
0.80
0.75
0.67
0.50
```

### FPS

```text
30
60
120
144
```

### DPI

```text
100%
125%
150%
175%
```

### Display / Capture Surface

- SDR；
- HDR；
- 0/90/180/270 rotation；
- 单屏；
- 多屏；
- 1920×1080 / 2560×1440 / 3840×2160 output；
- 60 / 120 / 144 refresh；
- Data ROI 1:1 与非 1:1。

### Presentation

- flip discard / flip sequential；
- waitable-object latency 1 / 2；
- VSync phase；
- tearing 仅 Experimental；
- windowed borderless / fullscreen-like data window；
- WM_DPICHANGED / monitor migration。

### GPU

- NVIDIA-only；
- Intel+iNVIDIA hybrid；
- AMD+NVIDIA；
- 无 CUDA。

### Player / H.264

- Certified player versions；
- software decode / hardware decode；
- resize；
- fullscreen/windowed；
- deblocking default/disabled；
- H.264 profile/level；
- full/limited range interpretation；
- 60/1 与需要时的 60000/1001 pacing experiment；
- AQ/lookahead disabled baseline。

### ChannelClass

```text
LocalDesktop
LocalVideo
RemoteVisual（仅 Experimental）
```

### Burst / Temporal

```text
capture stall: 1 / 2 / 4 / 8 / 16 / 30 / 60 / 120 frames
persistent rectangular occlusion
fixed bad stripe
random frame erasure
duplicate burst
mixed-frame burst
```

比较：

```text
single-segment burst scheduler
vs
inter-segment temporal striping
```

并记录 TimeToComplete 与 late-join penalty。

### Calibration / Constellation

- level ladder；
- Pilot clipping；
- ICC/color-management变化；
- SDR/HDR 切换；
- color-range mismatch；
- 2×2 / 4×4 tile；
- luma-only / joint constellation；
- LLR calibration held-out set。

### OS / API capability

- Windows 10 functional fallback；
- Windows 11 24H2 Certified baseline；
- WGC cursor off availability；
- Borderless permission denied/granted；
- ContentSize / mode-change / device-lost recreate。

---

## 38.5 Fuzz

必须 fuzz：

```text
Bootstrap parser
PB-Control fragment/reassembly parser
Protocol TLV/descriptor parser
Descriptor conflict handling
Segment map / size arithmetic
Wirehair descriptor wrapper
Transport blocks / zero padding
duplicate OuterBlockId payload conflict
Orphan cache quotas
ReceiverResourcePolicy boundaries
Resume state / crash recovery records
Filename/path sanitizer
Zstd boundary handling
```

所有 size calculation 使用 checked arithmetic。

---

# 39. 第三方库与依赖

## C++

```text
C++20
MSVC
CMake
vcpkg
```

## UI

```text
Qt 6
```

## Windows Graphics

```text
D3D11
DXGI
Windows.Graphics.Capture
```

## GPU

```text
D3D11 Compute Shader (cross-vendor baseline)
CUDA (optional optimized backend)
D3D11-CUDA Interop
```

## Video

```text
NVIDIA Video Codec SDK 13.1+ API baseline
NVENC（Certified Offline H.264 encoder 首选）
Media Foundation MPEG-4 File Sink（首选 MP4 mux baseline）
Media Foundation H.264 encoder（Best-Effort encode fallback；只有通过相同 bitstream semantic Gate 后才可进入 Certified）
```

NVENC 只使用运行时 capability query 后确认支持的现代 preset / tuning / rate-control API。不得把已被新驱动移除的 legacy preset GUID 或 legacy HQ rate-control mode 写入 Certified Profile。

## Compression

```text
zstd
```

## Outer FEC

```text
Wirehair V2
```

## Inner FEC Reference

```text
AFF3CT
```

建议：

> AFF3CT 作为 correctness oracle / matrix reference / benchmark 工具；生产 runtime 可逐步替换成 PixelBridge 自己的轻量高性能 QC-LDPC 实现。

## Digest

```text
BLAKE3 reference/portable implementation
```

或 Windows CNG SHA-256 fallback。

---

## 39.1 Third-party Baseline / SBOM / License Gate

正式 release 必须记录：

```text
Wirehair exact source revision
AFF3CT/reference matrix source revision
zstd version
BLAKE3 implementation revision
CUDA Toolkit version
Video Codec SDK / NVENC API version
NVIDIA driver version for Certified Offline benchmark
Windows SDK / target OS build
Qt version / license mode
vcpkg baseline
```

并生成 THIRD_PARTY_NOTICES / SBOM。第三方 source revision 是可复现构建和质量基线，不等价于 wire protocol version。

---

# 40. 关键 C++ 接口建议

## 40.1 Screen Region

```cpp
struct ScreenCaptureRegion
{
    HMONITOR monitor = nullptr;
    RECT physicalRect {};
    UINT dpiX = 96;
    UINT dpiY = 96;
    DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_UNSPECIFIED;
};
```

---

## 40.2 GPU Topology

```cpp
struct GpuInteropInfo
{
    LUID captureAdapterLuid {};
    int cudaDeviceIndex = -1;
    bool isCudaInteropAvailable = false;
};

class IGpuTopology
{
public:
    virtual ~IGpuTopology() = default;

    virtual bool ResolveForMonitor(
        HMONITOR monitor,
        GpuInteropInfo& interopInfo) = 0;
};
```

---

## 40.3 Capture Frame Token

不要把 WGC frame object 长期持有到后台。

```cpp
struct ScreenCaptureFrame
{
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    std::uint64_t captureSequence = 0;
    std::uint64_t captureEpoch = 0;
    std::int64_t qpcTimestamp = 0;
    RECT validRect {};
    DXGI_FORMAT pixelFormat = DXGI_FORMAT_UNKNOWN;
    bool isCursorExcluded = false;
};
```

上层 demod 只接受通过 `PBCaptureNormalize` 验证后的 frame；`captureEpoch` 变化时不得与旧帧做 LLR combine。

---

## 40.4 Capture Source

```cpp
class IScreenCaptureBackend
{
public:
    virtual ~IScreenCaptureBackend() = default;

    virtual bool Initialize(
        const ScreenCaptureRegion& region) = 0;

    virtual bool Start() = 0;

    virtual void Stop() = 0;

    virtual bool TryAcquireFrame(
        ScreenCaptureFrame& frame) = 0;
};
```

内部实现可由 FrameArrived 向 bounded queue 推送，再由 Receiver pull。

---

## 40.5 Demodulator

```cpp
class IVisualDemodulator
{
public:
    virtual ~IVisualDemodulator() = default;

    virtual bool ProcessFrame(
        const ScreenCaptureFrame& frame,
        VisualDemodResult& result) = 0;
};
```

---

## 40.6 Segment Policy

```cpp
struct SegmentPolicy
{
    std::uint64_t targetRawBytes = 8ULL * 1024ULL * 1024ULL;
    std::uint64_t minRawBytes = 2ULL * 1024ULL * 1024ULL;
    std::uint64_t maxRawBytes = 32ULL * 1024ULL * 1024ULL;
};
```

创建 Wirehair codec 前必须经过 policy validator，不能直接相信配置文件数值。

---

# 41. 必须重点注意的工程问题清单

1. **DPI 全程物理像素。**
2. **跨 Monitor ROI 必须拆分或明确降级。**
3. **Desktop Duplication rotation 必须处理。**
4. **WGC borderless 需要 capability + consent。**
5. **MinUpdateInterval 不能当 FPS 保证。**
6. **WGC Frame 必须及时释放，避免耗尽 frame pool。**
7. **D3D11 Adapter 与 CUDA Device 必须匹配。**
8. **Hybrid GPU 必须有 fallback。**
9. **Persistent CUDA registration 仍有 per-frame map/unmap sync。**
10. **HDR 不能简单按 BGRA8 处理。**
11. **Protected Content 不绕过。**
12. **UAC secure desktop / lock screen / desktop switch 可能中断 capture。**
13. **Capture device lost / access lost 自动重建。**
14. **Encoder Data Surface 和 Qt 控制 UI 分离。**
15. **数据窗口应避免被通知/其他窗口覆盖。**
16. **Cursor capture 尽量关闭；否则按局部误码处理。**
17. **Motion interpolation 必须关闭。**
18. **RTX Video Super Resolution / sharpening / AI enhancement 建议关闭。**
19. **YUV420 chroma alignment 不只看偶数 pitch，还要看缩放 phase。**
20. **All-Intra 高熵 H.264 可能达到异常高码率。**
21. **NVENC AQ/lookahead 默认关闭。**
22. **MP4 loop 不产生新的 Fountain IDs。**
23. **即时模式必须 Carousel，不可单遍 release 后永不回头。**
24. **Wirehair K 必须 ≤ 64000。**
25. **Wirehair V2 会复制 message，计算真实内存峰值。**
26. **不要使用模糊 `Wirehair CURRENT` 语义作为长期协议。**
27. **CRC32C 不是最终强完整性。**
28. **WholeFileDigest 必须最终验证。**
29. **摘要从同一未认证通道传来时不等于身份认证。**
30. **源文件在 Carousel 期间必须不可变。**
31. **文件名视为不可信输入。**
32. **防 path traversal / ADS / device name。**
33. **Zstd 解压必须 bounds checked。**
34. **所有整数加乘必须 overflow checked。**
35. **Visual DataGrid 尺寸必须按 Profile 精确整除，不使用近似网格。**
36. **Bootstrap 必须固定、独立于 Data Profile。**
37. **同帧 LLR combine 只能在同 FrameSequence 下进行。**
38. **capture backlog 丢旧帧，不积累延迟。**
39. **协议 parser / resume parser 必须 fuzz。**
40. **任何理论 Goodput 都不能在 benchmark 前写成硬承诺。**
41. **明确 ChannelClass；LocalDesktop 结果不能外推到 RDP/VDI/KVM。**
42. **0 B / 1 B / 高压缩 Segment 必须有 Tiny/DirectRepeat 路径，不能产生 Wirehair K=1。**
43. **Control Plane 必须冻结为独立 PB-Control-1。**
44. **未知 SegmentDescriptor 的 Data Block 不得触发无界资源分配。**
45. **LDPC decoder algorithm / LLR quantization 不进入 wire compatibility。**
46. **D3D11 Compute 是第一等跨厂商 GPU backend；CUDA 需实测胜出。**
47. **WGC / DXGI 必须经过 Capture Normalization。**
48. **ContentSize / mode change / device lost 必须建立 CaptureEpoch 并重建资源。**
49. **WGC cursor 能关则关闭；DXGI pointer 语义统一为 CursorExcluded。**
50. **Decoder 自身窗口可 best-effort WDA_EXCLUDEFROMCAPTURE，但不能当安全机制。**
51. **HDR 下优先 DuplicateOutput1；BGRA8 fallback 不得静默破坏 Chroma Profile。**
52. **D3D11 immediate context 建议单 owner，避免跨线程隐式 driver lock。**
53. **同一 Session 重生成 Segment 必须 EncodedDigest 完全一致；重启无法精确恢复则新建 SessionId。**
54. **OuterBlockId / Wirehair block ID 必须在 wrap 前结束 generation/session。**
55. **CarouselCycleTime / WorstLateJoinTime 必须可观测。**
56. **Profile benchmark 同时报告 RawGoodput 与 EncodedGoodput。**
57. **认证 benchmark 必须包含 incompressible random 数据。**
58. **DesktopNative direct-level modulation 必须与 Shape+Chroma A/B。**
59. **Offline H.264 必须验证实际 bitstream VUI / IDR / PTS，而不是只相信 encoder 配置。**
60. **第三方依赖必须 pin source baseline + SBOM + license notices。**
61. **Session 内 Data Visual Profile 固定；切档重新建立 SessionId。**
62. **Wirehair 后续 Carousel 必须复用首次保存的 32-byte descriptor。**
63. **Wirehair mixed profile 要求偶数 blockBytes；1449/1629 B 不能直接复用。**
64. **WirehairV2_ExtraInsufficient 必须显式处理，不能假设 Decoder 可无限收 repair IDs。**
65. **Full Repair Pass 必须定义 RepairPassSymbolBudget；不足 K 时不能承诺一轮 late join。**
66. **Transport 的 short payload 必须 canonical zero-padding。**
67. **OuterBlockId 同时覆盖 Wirehair ID 与 DirectRepeat ordinal。**
68. **同键 Descriptor 内容冲突必须 fail closed，禁止 latest-wins。**
69. **PB-Control-1 分片重组必须有 64 KiB 级有界 Record 上限和总内存配额。**
70. **SessionId 使用 BCryptGenRandom 等 OS CSPRNG。**
71. **Certified Sender 使用 flip model + VSync；tearing 只作为 Experimental。**
72. **Bootstrap 多位置 FrameSequence 不一致时整帧作为 erasure。**
73. **SignalProfile 必须冻结 range、color space、constellation 与 chroma siting。**
74. **YUV420 除 CellPitch 外，DataGridOriginX/Y 也必须偶数对齐。**
75. **WGC frame 归还后不得继续保存/访问底层 Surface；异步 GPU copy 用 bounded FrameLease + fence/query。**
76. **相同 FrameSequence 的重复观测高度相关，禁止无限 LLR 累加。**
77. **WholeFileDigest 对乱序 Segment 采用最终顺序扫描，FinalVerificationLatency 单独统计。**
78. **output.part 与最终文件必须同卷，WholeFileDigest 通过后才 atomic rename。**
79. **ReceiverResourcePolicy 必须限制 file/segment/session/orphan/control/zstd/resume 资源。**
80. **重新压缩 Segment 时必须先得到完整 EncodedDigest 一致结果，再发送新的 Fountain packet。**
81. **H.264 deblocking 是 benchmark 变量，不按主观视频经验直接固定。**
82. **OfflineArtifactExpansionRatio 必须纳入 MP4 Gate。**
83. **DesktopFast 60 FPS 当前 Fountain payload ceiling 约 3.728 MiB/s，4 MiB/s EncodedGoodput 不可作为该 Profile 目标。**
84. **LocalDesktop Direct-Level 调制必须在 Phase 1～2 早期与 Shape+Chroma A/B。**
85. **GPU backend 不要求浮点中间量逐 bit 相同；最终 Transport/Segment/WholeFileDigest 必须与 reference 一致。**
86. **Sync/Bootstrap/Control/Pilot/Guard 必须拥有明确 region map，不能默认“margin 应该够”。**
87. **Direct-Level 在冻结 bits/tile 前必须先跑 PB-LinkProbe-1，测量真实 Pixel Round-Trip。**
88. **Pilot 应直接建立 capture-space centroid/variance，不要盲信固定 gamma/3×3 矩阵能描述整个 Windows 色彩链路。**
89. **固定空间交织不足以对抗固定遮挡；Certified Interleave 应包含由 FrameSequence 决定的有限 phase。**
90. **长 Segment burst 对 capture stall 很脆弱；即时模式应 benchmark 有界 Active Segment Window temporal striping。**
91. **PersistentBadRegion 应降低 LLR 或擦除，不能把长期错误位置强判成高置信度 symbol。**
92. **MinUpdateInterval 只作为 Win11 24H2+ 可选 hint；Windows 10 基线和 Goodput 不依赖它。**
93. **WGC GPU copy 完成等待应异步 retire，禁止 Capture callback 或 D3D owner 每帧 CPU 阻塞。**
94. **Borderless capture 依赖 capability + 用户授权/打包条件，不能成为正确性前置条件。**
95. **NVENC Certified 配置必须 runtime query caps，并避免已被新驱动移除的 legacy preset/rate-control API。**
96. **Offline MP4 单次完整播放必须已经包含足够 unique repair IDs；loop 只重复观察，不增加方程。**
97. **Offline temporal schedule 应跨 Segment 条带化，避免播放器一次 burst drop 摧毁单一 Segment。**
98. **CFR 必须冻结精确 FrameRateNum/FrameRateDen；“60 FPS”不是足够精确的 bitstream 语义。**
99. **0-byte 文件必须有确定性 Session/FinalManifest 语义，不创建 K=0/K=1 Fountain。**
100. **任何 Certified Profile 都必须拥有 machine-readable Profile Manifest + CanonicalRasterHash + Golden Vector。**
101. **FLIP_DISCARD 不保证 Present 后 back buffer 内容可复用；每个 Visual Frame 必须完整重建 canonical raster。**
102. **Direct-Level Sender 避免 `*_SRGB` RTV 自动 gamma 转换，使用明确 UNORM code value + SignalProfile。**
103. **WGC 的 D3D11 device 应在 capture adapter 上创建，并满足 WinRT/DXGI interop 所需格式能力；Release 不依赖 Debug Layer。**
104. **不要错误设置 `D3D11_CREATE_DEVICE_SINGLETHREADED` 后又从多个线程访问 D3D11 接口。**
105. **NVENC 只产生 H.264 elementary bitstream；MP4 mux 必须单独处理 SPS/PPS、sample timing、finalize 和 container inspection。**
106. **任意大文件协议不等于任意大单 MP4；必须单独验证 >4 GiB、长 duration、sample table 与 mux finalize。**
107. **虚拟桌面物理坐标可能为负，ROI/monitor origin 不得使用无符号坐标。**
108. **Per-Monitor DPI Aware V2 应在创建 HWND 前确定，不能在 Qt/Win32 窗口已存在后随意切换。**
109. **GPU Demod + CPU LDPC 时 LLR readback 必须 ring-buffer + async completion，不能每帧 blocking Map/synchronize。**
110. **NV12 必须按真实 rowPitch/plane layout 写入，不能假定 `width*height*3/2` 连续无 padding。**
111. **任何 RGB→NV12/VideoProcessor 路径若参与 Certified Offline，都必须用 Golden NV12 code values 验证其没有偷偷改变 range/matrix。**
112. **同一 Segment 的重复 OuterBlockId 必须映射到完全相同 payload；不同内容必须触发 OuterBlockConflict，禁止 latest-wins。**

---

# 42. 分阶段实施路线

## Phase 0：协议、资源边界与 CPU Reference

目标：先证明 wire contract、Segment/FEC 与文件恢复完全正确，不引入 Windows Capture/GPU 复杂度。

实现：

- PBProtocol Major/Minor + explicit serialization；
- `PB-Bootstrap-1` / `PB-Control-1`；
- Control fragmentation / Descriptor conflict；
- `ReceiverResourcePolicy`；
- Session / Segment Map / FinalManifest；
- 0-byte file deterministic semantics；
- BLAKE3 / CRC32C；
- zstd / RAW；
- Wirehair V2 canonical descriptor；
- Tiny/DirectRepeat；
- CPU DVB-S2 Short LDPC；
- 简单 Luma raster + 简单 Direct-Level raster；
- fixed Pilot raster；
- Profile region map / Profile Manifest schema；
- PNG/raw-frame encode/decode；
- resume state 基础格式；
- Golden Vector；
- 0 B / 1 B / highly-compressible / boundary-size tests。

Gate：

```text
arbitrary file bit-exact round trip
+ descriptor conflict fail-closed
+ parser/resource fuzz no crash / no unbounded allocation
+ all Golden Vectors bit-exact
+ canonical region map produces exact payload budget
```

---

## Phase 1：Windows LocalDesktop Pixel Round-Trip 与 Present/Capture 基线

实现：

- 独立 D3D11 Data Window；
- flip-model / VSync Presentation Contract；
- present statistics / frame-latency waitable object；
- Region Selector / Per-Monitor DPI Aware V2；
- WGC；
- Desktop Duplication / DuplicateOutput1 probe；
- Capture Normalization / CaptureEpoch；
- bounded WGC FrameLease + asynchronous fence/query retire；
- cursor exclusion；
- Borderless capability/consent probe，但不依赖它；
- FrameSequence / multi-position Bootstrap consistency；
- duplicate/drop/blend handling；
- `PB-LinkProbe-1`；
- Pilot / calibration pipeline；
- CPU Demod；
- D3D11 Compute baseline；
- `PB-Mod-ShapeChroma-1` 最小实现；
- `PB-Mod-DesktopLevels-X` 最小实现；
- Real Pixel Round-Trip dataset recording。

Gate：

```text
1080p60 stable presentation + capture
+ no unbounded frame lease/backlog
+ accepted output always passes WholeFileDigest
+ sender raster ↔ captured raster statistics reproducible
+ 1:1 LocalDesktop Direct-Level and Shape paths both have comparable baseline data
```

这一阶段就回答 LocalDesktop 最重要的问题：

> “Windows 桌面像素链路到底能稳定保留多少离散状态？”

而不是等到 Shape classifier 完整优化后才测试 Direct-Level。

---

## Phase 2：Signal Profile、Interleave、Soft Demod 与 FEC/调度 Gate

实现：

- `PB-Signal-DesktopSRGB-1`；
- `PB-Signal-Video709Limited-1`；
- CodebookOptimizer；
- Direct-Level constellation search；
- Shape/Chroma soft metrics；
- Direct-Level centroid metric；
- calibrated LLR；
- PersistentBadRegion / reliability map；
- FrameSequence-driven InterleavePhase；
- Inter-Segment Temporal Striping；
- AVX2 LDPC；
- 可选 8/9 / erasure-only LocalDesktop experiment；
- Channel Simulator；
- Real Capture Replay Dataset；
- DirectLevels vs Shape+Chroma A/B Gate；
- burst-loss benchmark。

Gate：

```text
PB-VideoSafe-1 simulated/replay ≥ 500 KiB/s target
PB-VideoBalanced-1 simulated/replay ≥ 1 MiB/s target
LocalDesktop modulation family selected by VerifiedEncodedGoodput + stability
temporal striping does not violate resource budget and improves burst-loss recovery
LLR calibration does not create systematic overconfidence
```

---

## Phase 3：GPU Backend 竞争与 LocalDesktop Certified Candidate

实现：

- PBGpuTopology；
- optimized D3D11 Compute；
- same-adapter detection；
- persistent ROI texture pool；
- D3D11-CUDA interop；
- CUDA demod；
- hybrid GPU fallback；
- Phase 2 胜出的 LocalDesktop physical layer 优化；
- machine-readable Profile Manifest / CanonicalRasterHash；
- full Capture/Profile test matrix。

Backend correctness Gate：

```text
wire/protocol/raster Golden Vectors = bit-exact
floating metric/LLR = within defined tolerance OR canonical quantized LLR exact
accepted Transport Block bytes = CPU reference exact
WholeFileDigest = CPU reference exact
```

Performance Gate：

```text
CUDA becomes preferred only when it materially beats D3D11 Compute
selected Desktop profile ≥ 2.0 MiB/s VerifiedEncodedGoodput @ measured 60 unique FPS
Profile efficiency and modeled payload ceiling explicitly reported
```

只有此 Gate 通过后，才把胜出的 `PB-DesktopNative-X` 或 Shape baseline 固化为真正的 `*-1` Certified Profile。

---

## Phase 4：Offline MP4

实现：

- direct NV12 raster；
- NVENC capability query；
- modern preset/tuning/rate-control path；
- `PBMp4Mux` + Media Foundation MPEG-4 File Sink baseline；
- SPS/PPS / sequence-header extraction and validation；
- exact compressed-sample PTS / Duration / sink finalization；
- All-Intra CQP baseline；
- AQ/lookahead disabled baseline；
- deblocking A/B；
- exact H.264 profile/level/VUI/range/PTS inspection；
- exact rational playback cadence；
- finite unique Repair Pass；
- inter-segment offline temporal schedule；
- Certified player matrix；
- bitrate / peak-frame / expansion-ratio gates。

Gate：

```text
Certified player 1.0× playback
→ screen capture only
→ one complete artifact contains sufficient unique repair equations
→ verified full file recovery
```

并满足：

```text
PB-VideoSafe-1 ≥ 500 KiB/s VerifiedEncodedGoodput target
PB-VideoBalanced-1 ≥ 1 MiB/s VerifiedEncodedGoodput target
OfflineArtifactExpansionRatio recorded and bounded
driver/SDK/capability metadata recorded
```

---

## Phase 5：高刷、HDR、Turbo 与扩展信道

在前述 Certified baseline 稳定后评估：

- 32 Shape / 更激进 codebook；
- 1×N / 2×2 Direct-Level / 更高 constellation；
- high-rate LDPC / erasure-only LocalDesktop；
- 120/144 unique FPS；
- HDR/scRGB；
- mixed Wirehair profiles；
- advanced CUDA / cross-adapter experiments；
- HEVC / AV1 data-video experiments；
- `PB-Channel-RemoteVisual-X` 的 RDP/VDI/KVM/streaming 专门认证。

若项目的商业目标本身就是跨机器 RemoteVisual，则这一项不得长期停留在“未来实验”，而应在 LocalDesktop 基线跑通后单独建立正式里程碑、Simulator 和设备矩阵。

---

---

# 43. 最终验收标准

## 43.1 功能

必须支持：

- Windows x64 Encoder；
- Windows x64 Decoder；
- 任意屏幕区域选择；
- WGC；
- Desktop Duplication；
- CPU reference；
- D3D11 Compute GPU baseline；
- NVIDIA CUDA optional fast path；
- hybrid GPU fallback；
- 即时模式；
- 离线 MP4；
- arbitrary-size file；
- Tiny/DirectRepeat；
- PB-Bootstrap-1 / PB-Control-1 + bounded fragmentation；
- Segment Carousel / Full Repair Pass；
- `ReceiverResourcePolicy`；
- resume / crash-consistency policy；
- duplicate/drop/mixed-frame；
- `PB-LinkProbe-1`；
- fixed Pilot / calibration region；
- FrameSequence-driven InterleavePhase；
- Inter-Segment Temporal Striping；
- SignalProfile / PresentationProfile；
- machine-readable Certified Profile Manifest；
- WholeFileDigest + final sequential verification。

---

## 43.2 正确性

标准测试集：

- 0 B；
- 1 B；
- 边界长度；
- 1 KiB；
- 1 MiB；
- 100 MiB；
- 多 GB；
- 高压缩文本；
- incompressible random；
- ZIP/RAR/MP4；
- out-of-order Segment completion；
- descriptor conflict；
- malformed Control fragmentation；
- resource quota exceed；
- crash/restart resume；

最终：

```text
WholeFileDigest == Sender FinalManifest Digest
```

才显示：

```text
Verified Complete
```

“正确性”Gate 不使用 `99.999% accepted file correctness` 这种表述：**被接受的输出必须 100% 通过 digest**；统计指标应改成“在给定 channel/loss/time budget 下的 CompletionRate / TimeToComplete”。

---

## 43.3 内存

5 GB 与 500 GB 文件：

```text
peak working memory remains same order of magnitude
```

不得与总文件大小线性增长。

---

## 43.4 1080p60 Goodput

认证性能 corpus 必须包含 incompressible random；指标按 `VerifiedEncodedGoodput` 验收。

### PB-VideoSafe-1

```text
≥ 500 KiB/s
```

### PB-VideoBalanced-1

```text
≥ 1.0 MiB/s
```

### LocalDesktop Certified Profile

Phase 3 最终胜出的 LocalDesktop Profile：

```text
≥ 2.0 MiB/s @ measured 60 unique Visual FPS
```

如果胜出的是 `PB-DesktopFast-1` Shape baseline，其当前候选 60-FPS Fountain payload ceiling 约为 3.728 MiB/s，参考 Stretch 约 3.0～3.4 MiB/s；如果胜出的是 `PB-DesktopNative-*`，则必须按最终 tile/constellation/FEC/Profile Manifest 重新给出 payload ceiling，不能沿用 3.728 MiB/s。

所有 Certified Profile 同时报告：

```text
ModeledPayloadCeiling
VerifiedEncodedGoodput
GoodputEfficiency
VerifiedRawGoodput
CompressionGain
```

用户体验允许 `VerifiedRawGoodput > VerifiedEncodedGoodput`，但必须明确这是 compression gain。

---

## 43.5 Capture

Certified capture baseline 目标机器：现代 Windows 11 + 受支持的 Intel / AMD / NVIDIA D3D11 Adapter；CUDA 能力单独作为可选加速项验证：

```text
1080p ROI
60 unique Visual FPS target
no unbounded backlog
```

120/144 作为高刷可选能力，必须由 runtime measured unique FPS 证明。

Certified capture baseline 还必须验证：

- flip-model Present timing / glitch detection；
- ContentSize change / frame pool Recreate；
- display rotation；
- WGC / DXGI backend switch；
- bounded FrameLease / asynchronous fence/query retire；
- cursor exclusion；
- MinUpdateInterval absent/present fallback；
- Pilot recalibration after CaptureEpoch；
- capture access/device lost recovery；
- 1080p/1440p/4K capture-surface matrix；
- Windows 11 24H2+ baseline；
- hybrid GPU。

---

## 43.6 Offline MP4

认证播放器：

```text
1.0× playback
loop allowed
no external access to MP4 bytes by Decoder
```

Decoder 只通过屏幕 ROI 恢复完整文件。

同时必须：

- bitstream/MP4 inspector 验证实际 H.264 profile/level/VUI/range/IDR/PTS、`avcC/stsd`、SPS/PPS 与 sample count；
- Mux sample PTS/Duration 与 CFR rational timeline 一致；
- MP4 sink finalize 成功后才发布 artifact；
- 报告 `OfflineArtifactExpansionRatio`；
- deblocking/AQ/lookahead 与 Certified Profile 一致；
- 精确 FrameRateNum/FrameRateDen 与播放 cadence 通过验证；
- 单次完整 artifact 已包含足够 unique repair equations；
- temporal schedule 对 burst loss 通过 Gate；
- 循环播放不被计作新的 Fountain equation；
- 记录 NVENC API / driver / selected capability metadata。

---

## 43.7 Resume、资源与最终发布

必须验证：

```text
crash during segment write
crash during resume.state update
crash before/after final hash
out-of-space
resource quota rejection
```

任何情况下都不得把未通过 WholeFileDigest 的 `.part` 发布成最终文件。

---

## 43.8 ChannelClass 声明

产品/benchmark 报告必须明确标注：

```text
LocalDesktop
LocalVideo
RemoteVisual
```

LocalDesktop 的结果不得作为 RemoteVisual 或 air-gap 性能/安全声明。

---

# 44. 与 libcimbar 的继承关系与根本差异

PixelBridge 继承 libcimbar 中最值得保留的系统思想：

1. 用视觉调制把结构化数据映射到动态图像；
2. 在 Video Profile 中保留 Shape + Color/Luma+Chroma 的多维信息承载；
3. Fountain 处理丢帧、乱序与无 ACK 单向接收；
4. Interleave 处理空间成团错误；
5. 把视觉置信度继续传递为 soft information，而不是过早硬判决；
6. 把文件恢复、纠错、视觉解码和实际实时链路作为一个系统共同设计。

但 PixelBridge 的物理信道根本不同。

libcimbar：

```text
Display
→ optical channel
→ Camera
```

PixelBridge：

```text
Desktop / Video Pixels
→ Windows graphics pipeline
→ Screen Capture
```

因此 PixelBridge 不应照搬：

- perspective homography；
- camera anchor design；
- autofocus/exposure compensation；
- lens drift；

而应把主要优化资源放到：

- Pixel Round-Trip / Direct-Level 可分状态数；
- display/capture cadence；
- exact pixel phase；
- Pilot / calibration；
- persistent bad-region reliability；
- frame-sequence-driven interleave phase；
- burst-loss / inter-segment temporal striping；
- resize；
- YUV420；
- codec quantization；
- HDR/SDR；
- multi-GPU；
- SignalProfile / color-management；
- sender Present timing；
- soft LLR / erasure decision；
- Inner FEC profile；
- Segment scheduling / late join；
- resource-safe protocol parsing。

---

# 45. 最终系统摘要

```text
Arbitrary-size File
        ↓
Stable Read-only Source Session
        ↓
8 MiB Target Segment Stream (2~32 MiB bounded)
        ↓
Raw Segment Digest + Whole-file BLAKE3 state
        ↓
Segment Zstd / RAW
        ↓
Encoded Segment Digest
        ↓
Outer FEC
Wirehair V2 exact saved descriptor / DirectRepeat
        ↓
Transport Block + CRC32C + canonical zero padding
        ↓
Inner FEC Profile
QC-LDPC mainline / eligible LocalDesktop erasure-only experiment
        ↓
Frame Packing
+ FrameSequence-driven Spatial Interleave Phase
+ bounded Inter-Segment Temporal Striping
        ↓
┌────────────────────────────┬──────────────────────────────┐
│ LocalVideo                 │ LocalDesktop                  │
│ Luma Shape + Chroma        │ Direct Level first candidate │
│ calibrated soft metric     │ Shape fallback               │
└────────────────────────────┴──────────────────────────────┘
        ↓
SignalProfile
+ exact DataGrid/Tile geometry
+ Sync/Bootstrap/Control/Pilot/Guard region map
        ↓
Fixed Robust PB-Bootstrap-1 + PB-Control-1
        ↓
Pilot / Calibration symbols
        ↓
┌────────────────────────────┬──────────────────────────────┐
│ Instant                    │ Offline                      │
│ D3D11 Flip/VSync Surface   │ Direct NV12                  │
│ Segment Carousel           │ H.264 All-Intra CQP          │
│ Active Segment Window      │ Finite unique repair schedule│
└────────────────────────────┴──────────────────────────────┘
        ↓
Windows Desktop Pixels
        ↓
User Selected Physical-Pixel ROI
        ↓
WGC / DXGI Desktop Duplication
        ↓
Capture Normalization + CaptureEpoch
+ bounded FrameLease + async GPU retire
        ↓
Monitor → Adapter → D3D11 Device Topology Resolve
        ↓
┌────────────────────────────┬──────────────────────────────┐
│ Any capture adapter        │ Same NVIDIA Adapter          │
│ D3D11 Compute baseline     │ optional D3D11-CUDA          │
└────────────────────────────┴──────────────────────────────┘
        ↓
Multi-position Bootstrap / FrameSequence consistency
        ↓
Axis-aligned Locate + Scale + Phase + Rotation
        ↓
Pilot-calibrated capture-space model
+ PersistentBadRegion reliability map
        ↓
Soft Shape/Level/Chroma Metrics or eligible hard decision
        ↓
Calibrated LLR / erasure decision
        ↓
Inner FEC Decode
        ↓
Transport CRC
        ↓
OuterFecMode dispatch
Wirehair V2 / DirectRepeat
        ↓
Encoded Segment Verify
        ↓
Bounded Zstd / RAW
        ↓
Raw Segment Verify
        ↓
Random Write output.part
        ↓
All Segment Map complete
        ↓
Sequential WholeFileDigest verification over output.part
        ↓
FinalManifest match
        ↓
Same-volume Atomic Rename
        ↓
Original File (Verified)
```

PixelBridge 的核心技术价值集中在：

1. **无摄像头的 Data-over-Desktop-Pixels 高带宽单向视觉链路；**
2. **Segment + Wirehair V2/DirectRepeat + Full Repair Carousel，覆盖 Tiny 文件、晚加入、掉帧和任意大文件；**
3. **LocalDesktop 先通过 PB-LinkProbe-1 测量真实 Pixel Round-Trip，再决定 Direct-Level、Shape 与 FEC 强度，而不是把 camera-oriented 物理层先验带进数字桌面信道；**
4. **LocalVideo 采用 Shape/Chroma + calibrated soft QC-LDPC，并把 YUV420、播放器、H.264、色彩范围和 resize 当成真正信道模型；**
5. **Pilot、有限 Interleave Phase、PersistentBadRegion reliability 与跨 Segment temporal striping共同处理色彩变换、固定遮挡和 burst erasure；**
6. **把 Sender Present、SignalProfile、WGC/DXGI Capture、D3D11 Compute 与 optional CUDA 视为同一通信链路的一部分；**
7. **同一上层协议支持即时 D3D Visual Stream 与 H.264 MP4，同时严格区分 LocalDesktop / LocalVideo / RemoteVisual；**
8. **明确区分 raw visual capacity、Inner-FEC information、Fountain payload、VerifiedEncodedGoodput 和压缩后的 VerifiedRawGoodput；**
9. **通过固定 Bootstrap/Control、canonical Wirehair descriptor、Descriptor conflict policy、Digest、资源配额、Profile Manifest、Golden Vector 和 fuzz 把实现约束成可长期维护的协议。**

---

# 46. 主要参考资料

## libcimbar

- Repository  
  https://github.com/sz3/libcimbar
- PERFORMANCE  
  https://github.com/sz3/libcimbar/blob/master/PERFORMANCE.md
- DETAILS  
  https://github.com/sz3/libcimbar/blob/master/DETAILS.md
- TODO  
  https://github.com/sz3/libcimbar/blob/master/TODO.md

## Wirehair

- Repository  
  https://github.com/catid/wirehair
- V2 Wire Profile  
  https://github.com/catid/wirehair/blob/master/V2_WIRE_PROFILE.md
- Public API Header  
  https://github.com/catid/wirehair/blob/master/include/wirehair/wirehair.h

## Windows Graphics Capture

- Screen Capture  
  https://learn.microsoft.com/en-us/windows/apps/develop/media-authoring-processing/screen-capture
- CreateForMonitor  
  https://learn.microsoft.com/en-us/windows/win32/api/windows.graphics.capture.interop/nf-windows-graphics-capture-interop-igraphicscaptureiteminterop-createformonitor
- CreateFreeThreaded  
  https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.direct3d11captureframepool.createfreethreaded
- IsBorderRequired  
  https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession.isborderrequired
- MinUpdateInterval  
  https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession.minupdateinterval

## Desktop Duplication

- Desktop Duplication API  
  https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/desktop-dup-api
- DuplicateOutput1  
  https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_5/nf-dxgi1_5-idxgioutput5-duplicateoutput1

## DXGI Presentation / Color Space

- DXGI flip model  
  https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-flip-model
- Flip-model performance / frame-latency waitable object  
  https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/for-best-performance--use-dxgi-flip-model
- DXGI_FRAME_STATISTICS  
  https://learn.microsoft.com/en-us/windows/win32/api/dxgi/ns-dxgi-dxgi_frame_statistics
- IDXGISwapChain3::CheckColorSpaceSupport  
  https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiswapchain3-checkcolorspacesupport
- IDXGISwapChain3::SetColorSpace1  
  https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiswapchain3-setcolorspace1
- DXGI_COLOR_SPACE_TYPE  
  https://learn.microsoft.com/en-us/windows/win32/api/dxgicommon/ne-dxgicommon-dxgi_color_space_type

## CUDA / D3D11

- CUDA Graphics Interop  
  https://docs.nvidia.com/cuda/cuda-programming-guide/04-special-topics/graphics-interop.html
- CUDA D3D11 Runtime API  
  https://docs.nvidia.com/cuda/cuda-runtime-api/group__CUDART__D3D11.html

## NVIDIA Video Codec SDK

- SDK 13.1  
  https://docs.nvidia.com/video-technologies/video-codec-sdk/13.1/
- NVENC Programming Guide  
  https://docs.nvidia.com/video-technologies/video-codec-sdk/13.1/nvenc-video-encoder-api-prog-guide/index.html
- Video Codec SDK 13.1 Deprecation Notices  
  https://docs.nvidia.com/video-technologies/video-codec-sdk/13.1/deprecation-notices/index.html

## MP4 Mux / Media Foundation

- MPEG-4 File Sink  
  https://learn.microsoft.com/en-us/windows/win32/medfound/mpeg-4-file-sink
- MFCreateMPEG4MediaSink  
  https://learn.microsoft.com/en-us/windows/win32/api/mfidl/nf-mfidl-mfcreatempeg4mediasink

## Compression / FEC

- Zstandard  
  https://github.com/facebook/zstd
- AFF3CT  
  https://github.com/aff3ct/aff3ct
- AFF3CT DVB-S2 LDPC encoder documentation（支持不叠加 BCH 的 LDPC_DVBS2）  
  https://aff3ct.readthedocs.io/en/latest/user/simulation/parameters/codec/ldpc/encoder.html
- ETSI EN 302 307-1 V1.4.1（DVB-S2；Short FECFRAME Table 5b）  
  https://www.etsi.org/deliver/etsi_en/302300_302399/30230701/01.04.01_60/en_30230701v010401p.pdf

## Windows Capture 补充

- WGC cursor capture  
  https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession.iscursorcaptureenabled
- WDA_EXCLUDEFROMCAPTURE  
  https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowdisplayaffinity
- FramePool Recreate  
  https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.direct3d11captureframepool.recreate
- Desktop Duplication rotation / pointer semantics  
  https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/desktop-dup-api
- DuplicateOutput1 / high-color formats  
  https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_5/nf-dxgi1_5-idxgioutput5-duplicateoutput1

---

**设计日期：2026-08-21**  
**项目名称：PixelBridge**  
**实施起点：Phase 0 先冻结协议语义、Control/Tiny-Segment/资源边界、Canonical Region Map 与 CPU Reference；Phase 1 建立 Flip/VSync Present、WGC/DXGI Capture、PB-LinkProbe-1、Pilot Calibration 与 Shape/Direct-Level 两条可比较的 LocalDesktop 物理层基线；后续所有 Certified Profile 均以 Profile Manifest + Golden Vector + Real Capture Gate Benchmark 数据冻结。**
