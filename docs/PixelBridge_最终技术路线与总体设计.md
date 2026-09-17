# PixelBridge 技术路线与总体设计

**简体中文** | [English](ARCHITECTURE.en.md)


> 产品界面统一显示“标准 / 灰阶高速 / PAM4 / PAM4 Wide”，不带“实验”字样。下文 `Experimental`、`experimental-*` 等标识是已冻结的内部/CLI兼容名称，保留它们不代表界面或产品版本仍为实验版；验证范围仍按真实证据说明。

> 当前维护的总体设计，2026-09-17。取代旧文档里按历史阶段追加的“当前状态”和未落地候选路线；协议、文件安全、资源和捕获生命周期的不变量继续有效。正式版本号不等于协议/Profile 的认证状态。v1.0 发布内容及验证边界见 [发布说明](RELEASE_V1.0.md)。

## 1. 目标、产品边界与完成定义

PixelBridge 面向 **Windows x64 非本机远控画面中的单向文件传输**。首要指标是最终正确文件的总完成时间，而不是发送帧率、理论 bits/cell、GPU 使用率或内存占用。

- Encoder 读取本端文件，显示数据码面；Decoder 只读取所选屏幕区域实际捕获的像素。
- 不通过 socket、pipe、共享内存、COM、剪贴板、窗口消息或临时文件交换文件内容、已解码块或接收 ACK。
- PBBridge 仅服务开发部署、进程编排、日志与摘要元数据；不能提供 payload、像素或接收决策输入。
- GUI 是 Qt Widgets；协议、FEC、调制、捕获、存储和应用运行时核心不依赖 Qt。
- 本次产品路线是即时桌面视觉流。离线 MP4、摄像头、任意 HDR/缩放、跨平台接收以及 CUDA 全流水线是非交付能力，不得把设计愿景当作实现。

**完成链：** 每段 EncodedDigest → 有界解压 → RawDigest → 持久写入 → WholeFileDigest → 安全发布 → 最终重开复验。只有最后一步完成才可报告完整成功。CRC 不等于文件摘要；随画面发送的摘要不提供发送者身份认证。

[现状与验证边界](PROJECT_STATUS.md) 与 [证据索引](EVIDENCE_INDEX.md) 单独维护，不在架构中滚动复制实验日志。

## 2. 实际数据通路

```text
Encoder 本地只读源文件
  → 流式预扫描、分段、RAW / Zstandard
  → 不可变 Session / Segment / FinalManifest
  → DirectRepeat 或 Wirehair V2（分段纠删恢复）
  → Transport + CRC32C → QC-LDPC（码字纠错）
  → 独立 Profile 光栅 → D3D11 数据窗口
  → 远控软件的视频编码、网络传送、解码、缩放与桌面合成
  → Decoder 所选屏幕的 WGC / DXGI 捕获
  → PB 自有纹理、epoch/帧龄检查 → 定位、可见校准、软解调
  → 内层 FEC / CRC / 身份准入 → typed Control / Receiver
  → 分段恢复、摘要、断点、最终发布与重开
```

远控软件是既有外部信道，不由 PixelBridge 调参。provider 名称只能进入操作员元数据，不能影响解码阈值、Profile、资源准入或校验分支。

## 3. 当前视觉模式与身份隔离

| 界面模式 | 精确身份 / layout | 默认与实现 |
| --- | --- | --- |
| 标准 | `PB-Unified-SC6-V3` / `0x5042554E49534333` / 10 | 现有正式目录唯一条目，默认 |
| 灰阶高速 | `UnifiedGrayFast` / layout 13 | 可选模式；保持旧身份和兼容性 |
| PAM4 | `PB-Experimental-Pam4-1` / `0x504250414D343031` / 15 | 可选模式，1080P 物理码面 |
| PAM4 Wide | `PB-Experimental-Pam4-Wide-1` / `0x504250414D345731` / 16 | 可选模式，固定 2560×1440 物理码面 |

双端必须选择匹配模式。Session 内 Data Profile 固定，切换不兼容模式必须建立新 Session。不能靠尝试多个身份/FEC、挑选过 CRC 的结果来静默回退。旧 layout 8/9、LF4 和 layout 14 研究不成为新的默认。

### 3.1 从形状/色度到四级灰阶

标准 SC6 V3 的 1920×1080 码面由 Base Luma、Fine Luma、Chroma 等可独立擦除 lane 组成，利用局部放置和 codeword-local 置换控制局部跨帧污染。GrayFast 沿既有灰阶路线提高可用内层信息比例。

远控常用 4:2:0 色度抽样，彩色信息可能被削弱；压缩、缩放、局部刷新和相位混合也会损伤细小形状。新 PAM4 直接采用 `[0,85,170,255]` 四个中性灰阶，每 cell 两位；不依赖色度提供额外位。它仍需要足够空间分辨率、可见校准和严格 FEC，不能把“灰阶”理解为压缩无损。

### 3.2 PAM4 的明确参数

| 参数 | PAM4 | PAM4 Wide |
| --- | ---: | ---: |
| 逻辑画布 | 1920×1080 BGRA8 SDR | 1920×1080 BGRA8 SDR |
| 逻辑 cell | 4×4 px | 3×3 px |
| 固定源桌面码面 | 1920×1080 | 2560×1440 |
| 源桌面有效 cell | 4×4 px | 4×4 px |
| 可用 Data cell | 92,832 | 165,192 |
| 固定 Control slot | 1 | 1 |
| Transport slots | 10 | 19 |
| 总码字数 | 11 | 20 |
| 满载文件 payload 上界 | 16,290 B/frame | 30,951 B/frame |

Control slot 固定使用 Robust QC-LDPC `N=16200/K=10800`，1350 信息字节。Transport slots 使用 Fast `N=16200/K=13320`，1665 信息字节，其中文件 payload 上界 1629 字节。未使用槽、未映射位和 padding 必须遵守零填充合同。

这些是码面容量，不是有效文件速度。Control 刷新、冗余修复、视频丢失、重复观察、资源暂缓和最终验证都会减少净吞吐。

Wide 用固定 pixel-center point 采样将 1920×1080 映射到 2560×1440，逻辑 3px 精确变成物理 4px；不是让 3px 单元直接出现在 1080P 上，也不是任意插值缩放。非数据边缘保持静态以减轻远控编码压力，不强行铺满每个屏幕。

### 3.3 定位、校准、映射与接收

保留四角色定位 marker、两个 RS Bootstrap、九个 timing/freshness 区域、四个 calibration 区域和 PhaseChecker 等保留结构，不侵占 guard。每帧检查精确身份与 freshness；失败按擦除处理。

可见校准粗阶 `[8,64,160,232]`，held-out 细阶 `[56,112,168,248]` 和中性 128。灰阶间距、总对比、空间偏差与 held-out 残差有固定门限，不能利用已知文件内容反向调参。PAM4 固定软度量分母 127.5；Wide 使用所测相邻灰阶中心间距中位数的 1.5 倍。两者保持独立，不改写旧模式。

映射是只读冻结表，运行时不重新随机生成；独立 Golden 验证 raster、coded frame、映射表、FEC 和保留区。详细数字与完整 manifest 见 [PAM4](EXPERIMENTAL_PAM4_PROFILE.md)、[Wide](EXPERIMENTAL_PAM4_WIDE_PROFILE.md)。

**当前 PAM4 实际接收是 GPU 捕获/ROI 后的 CPU reference 解调与 FEC 路径**，包含显式回读，不宣称 GPU 全链或零 CPU 回读。日志独立报告 PAM4 观察数、擦除、槽接纳和回读解码耗时，不伪造 Unified lane 指标。

## 4. 无反馈调度与大文件尾部

发送端按固定 Session 预扫描文件，并逐段压缩；普通默认 8 MiB，当前 PAM4 GUI 使用 7 MiB。RAW fallback 避免压缩膨胀，微小/高压缩段走 DirectRepeat，适用尺寸走 Wirehair V2。

Carousel 初始轮发送系统块与修复预算，随后生成新的修复 ID。对不同文件段数及刷新率，既有实现有窗口、barrier / graduation 与 repair visit 等不同路径。不得用落在另一调度分支的小文件替代大文件结论。具体常量、lease、窗口与预算以 [发送持久状态合同](ENCODER_STREAMING_CAROUSEL.md) 和当前 scheduler 的测试为准。

PAM4 每个数据帧固定带当前 SegmentDescriptor；Session/Manifest 刷新帧的 Transport 槽置 inactive，不能用额外通道提前告知 Receiver 调度。

发送端不知道接收端还缺哪段。接收尾部可能遇到重复/已完成段，直到新一轮修复块到来；视频变化并不保证新增独立方程。降低不必要的视频编码负担、增加可用 payload/frame、避免接收准入丢失与同步持久化开销必须共同衡量。

## 5. 协议与文件安全

- 显式 little-endian 序列化，多字节字段、长度、offset 和乘加全部 bounds/overflow checked；不持久化 C++ struct 内存布局。
- Protocol 1.0 / Descriptor Schema 1；未知 mandatory feature、major/schema、非零 reserved/padding 或长度不符按明确定义拒绝。
- Session ID 使用 OS CSPRNG。文件源在整个 Session 中不可变，变更必须检测并停止；崩溃恢复沿用已冻结 descriptor/encoded bytes。
- 同 descriptor key 的不同有效内容、同 `(SessionTag,SegmentOrdinal,OuterBlockId)` 的不同有效 payload 都是冲突，禁止 latest-wins。
- Descriptor/resource 校验前的未知段只能进入有限 orphan policy，不可先创建大 FEC 对象。
- Wirehair 参数与 K 范围先校验；额外修复不足、quota、OOM 等返回值必须单独处理，不假设一个对象可无限收块。
- 名称仅接受安全 basename；拒绝路径穿越、绝对路径、ADS、保留设备名、NUL、非法尾点/空格和超长名称。
- 不静默覆盖最终文件；`.part` 未满足整文件验收前不能改名为最终目标。

逐字段规范见 [Descriptor Schema](PROTOCOL_1_DESCRIPTOR_SCHEMA.md)。

## 6. 有界内存与接收并发

活动解码器是保存不同分段的 FEC 状态，不等于同样多的 CPU 线程。默认最多 8 个，总预算 1024 MiB、单实例 512 MiB；性能模式取消固定 8 名额，仍按有限资源预算及分段上限准入。

用户可设置总预算、单实例上限，断点预算与总预算联动。启动时根据可用物理/commit 检查整体规划，保留捕获、断点副本和系统余量。配置后不立即分配全部预算；不足时暂缓，不允许无限创建。内存复杂度保持 `O(active segment set)` 而非 `O(file bytes)`。

所有队列、capture/demod staging、orphan、Control 重组、active cache、repair retry 和日志有上限。所有支持文件仍有 500 GiB 产品上限。更大可用内存只有在资源准入确实成为瓶颈时才可能提速。[完整预算规则](DECODER_MEMORY_BUDGET.md)

## 7. 持久化、崩溃恢复与最终发布

Encoder 冻结 descriptors/encoded bytes，使用 durable sequence/repair ID lease，恢复时不得重用已发身份。只保留有限活跃编码缓冲，不把全部压缩文件驻留 RAM。

Decoder 对 `.resume` 长度、CRC、generation、descriptor 和资源边界重新验证；已完成分段按磁盘内容复验。只对可证明是 torn tail 的末尾截断处理，不能容忍内部损坏或冲突。

已接纳块 checkpoint 以 64 KiB 有界批缓冲合并完整记录；合法最大单记录可达 65,591 B，这种记录单独写入而不扩大批缓冲。成功仍需 flush；短写、部分前缀、seek/write/flush 失败均不能冒充提交成功。大活动集合下，显式预算模式合并没有回收收益的 compact，保持 finite quota 与已完成段立即耐久；不取消持久化。

分段路径：恢复 → EncodedDigest → 有界解压 → RawDigest → `.part` 写入/flush/checkpoint → completed record → Receiver stored commit。最终路径：全部段与 Manifest 一致 → 整文件 BLAKE3 → publish intent → 同目录安全发布 → 重开复验。恢复 publish crash window 时同样不能仅凭文件名认定成功。[恢复合同](DECODER_RESUMABLE_RECOVERY.md)

## 8. Windows 捕获与显示生命周期

- 坐标是物理像素；进程 Per-Monitor DPI Aware V2，单 monitor ROI 是优先路径。
- WGC 优先，只有明确后端初始化/设备恢复类失败才允许按策略转向 Desktop Duplication；画面无效不是任意切换理由。
- WGC frame 是帧池租约。GPU 使用源 surface 完成前不得归还后继续读；先复制到 PB-owned bounded textures，按 GPU 完成退休。
- ContentSize、ROI、设备、显示模式、monitor 或 epoch 变化时，丢弃/排空旧工作并重建依赖，不把旧域结果送给新域。
- 过期帧丢弃，不堆积延迟；D3D11 immediate context 有明确 owner；实际 row pitch 和 alpha/色彩格式不能靠宽度推断。
- PAM4 单屏授权与旧双屏现场 gate 分开。所选设备、几何、DPI、rotation、refresh、adapter 在启动前及运行期复核，不伪造一块保护屏。
- 程序不自动移动/最小化窗口、不抢焦点、不注入鼠标键盘。单屏使用者自行让码面可见，Decoder 可在后台接收。

参见 [WGC](PBScreenCaptureWgc.md)、[屏幕区域](SCREEN_REGION.md)、[呈现](PRESENTATION.md)。分辨率支持不等于任意远控缩放都能成功。

## 9. 自动日志与用户等待提示

普通双端 GUI / 传输 CLI 的应用轮询层默认记录每 2 秒的时间序列，状态切换额外采样，停止/失败/完成保存最终报告。记录当前构建、Session、Profile、阶段、累计计数、内存及错误；不记录文件内容/像素，不参与接收接受判定。

日志每 run 至多 64 MiB / 48 小时，超限明确标记截断，仍尝试最终报告；每角色启动时保留最近 7 份旧日志加当前一份，活跃或不可删除项保留并提示。不会自动删用户数据或显式 evidence 文件。

活动原因使用单调时间与实际同步恢复操作，不把残留 `Verifying` 状态误当持续写盘。正常修复/轮播/校验等待提示“不要暂停”；捕获失联、资源暂缓、持续缺乏新有效数据和错误区别显示。提示不虚构进度、不提升文件完成条件，也不保证停滞一定会自行恢复。[诊断说明](DIAGNOSTICS.md)

## 10. 性能真值与证据层级

主指标是 `finalReopenVerified - startAccepted` 对应的同机完整运行时间和 verified goodput，包含等待控制信息、恢复尾部、磁盘、摘要、发布和复验。跨机的 monotonic timestamps 不能直接相减；CPU/GPU/fps 仅用于定位。

证据分开：Golden/CPU reference → 编码器/失真代理 → 离屏 GUI/应用正确性 → 真实远控小文件 → 同环境完整大文件。前一层不能代替后一层；包和源码身份改变后必须明确哪些成绩属于旧候选。

当前大文件保留结果约 70 分 28 秒；一小时和全程无停滞仍未达成。不能据此证明已达理论上限或全局最快。

## 11. 当前交付方向与后续愿景

当前只做稳定性、日志、清晰交互、多分辨率边界、文档和可复现交付，不新增密度/Profile/FPS 扫描。v1.0 采用双端独立打包；wire 兼容身份和默认模式选择不随产品版本号自动改变。

未来若重新进入性能研究，应先从完整日志定位有效数据率与尾部占比，针对受影响场景设计窄对照，频繁文件不超过 100 MB，并验证最终文件。更积极的调度、解码并行化或GPU路径需要各自成本与正确性证据；不能通过隐藏反向通道获得速度。

## 12. 代码地图与定向检查

| 部分 | 入口 |
| --- | --- |
| 双端界面与自动日志 | `apps/PixelBridgeEncoder`、`apps/PixelBridgeDecoder`、`apps/common/operational_log_qt.*` |
| 接收活动原因 | `apps/common/decoder_activity.*`、运行时真实 operation 标记 |
| 应用调度/恢复 | `apps/common/local_desktop_runtime.cpp`、`sender_carousel_scheduler.*`、`decoder_resume_store.*` |
| 协议与 FEC | `libs/PBProtocol`、`PBOuterFec`、`PBInnerFec` |
| PAM4 | `libs/PBModulation/experimental_pam4*` 相关 public/src 文件与独立 Golden |
| 实际捕获/解调 | `PBCaptureNormalize`、`PBScreenCaptureWgc`、`PBScreenCaptureDxgi`、`PBDemodD3D11` |
| 文件验收 | `PBReceiver`、`PBStorage` |

构建、测试与禁止隐式实屏操作见 [开发指南](../CONTRIBUTING.md)。
