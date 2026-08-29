# PB-Mod-DesktopLevels-X1：CPU/reference 与 LocalDesktop SDR 测量基线

## 定位与兼容边界

这是实验性、显式选择的 **诊断 Data Plane**。仅适用于完整可见的、物理像素 1:1 的 `1920×1080` LocalDesktop SDR。它不是 Certified Profile，也不代表最终系统完成。Encoder/Decoder 的诊断命令仍不是通用文件接收器；Phase 1 Gate 另以 tests-only 编排复用生产 Control/Transport/FEC/Receiver/Storage 和 GPU demod，证明一条窄的真实像素文件闭环，不把 Gate 编排变成第二套产品架构。HDR、视频、生产调度和最大 bits/pixel 优化不在本阶段。

实现复用现有 PBModulation 的四角定位、独立 A/B RS Bootstrap、九处 Timing、LumaView 像素读取，PBInterleave 的 affine arithmetic，Robust QC-LDPC、Transport framing，以及应用已有 DataWindow/VSync、WGC/DXGI、CaptureNormalize、readback worker。`PBDesktopLevelsReference` 只是两端工具共用的有界诊断编排/评分层，不另建捕获或 FEC 实现；所有核心库仍不依赖 Qt、Windows 或捕获后端。

旧 `PB-ReferenceRaster-1`、LocalDesktop Bootstrap/layout 2、旧 interleave 的入口绑定、行为与 Golden 不变。私有 scaffold 只能在明确的旧绑定与两个新绑定间选择；不能传任意 profile ID 绕过验证。

## 固定 profiles 与布局

| 字段 | 2×2 | 4×4 |
|---|---:|---:|
| VisualProfileId | `0xEBB15DCE41AB436E` | `0xF9B7490A9251F15C` |
| VisualLayoutVersion | 3 | 3 |
| Data tiles | 346,752 | 86,688 |
| Data bytes/frame | 86,688 | 21,672 |
| Robust codewords/frame | 42 | 10 |
| Coded bits/frame（BER 分母） | 680,400 | 162,000 |
| Canonical zero tail bytes | 1,638 | 1,422 |
| Raw data bits/pixel（tile 内） | 0.5 | 0.125 |

ID = UTF-8 `PixelBridge/VisualProfile/DesktopLevels-X1-Luma4-Tile{N}x{N}` 的 BLAKE3 前 8 字节，按 little-endian 解释。两者发送均为 BGRA8：`B=G=R`，`A=255`。

Luma code values 为 `32,96,160,224`；对应 **二进制数值** Gray labels `00,01,11,10`，每 tile 两位，序列化 **LSB-first**。例如数值 `01` 的首位是 1、第二位是 0。数据字节按低位开始，每四个 tile 一个字节。

- DataGrid：`(96,96,1728,888)`，排除九个既有 `128×128` Timing 矩形；剩余 tiles 按 row-major 编号。
- 四个 `128×64` ladders：`(736,16)`、`(1696,16)`、`(96,1000)`、`(1056,1000)`。各有四个从低到高的 `32×64` level patch。
- 两个 `128×64` phase pilots：`(896,16)`、`(896,1000)`。左侧 `64×64` 区按列、右侧按行，每像素交替 `32/224`；偶数索引为 32。
- 未占用区域是既有 scaffold 的中灰背景；marker、Bootstrap A/B、Timing 的位置与算法不变。
- `phase=FrameSequence%16`；`rowTiles=1728/tilePixels`；`physical=(logical*65537+phase*rowTiles)%tileCount`。两候选 inverse multiplier 都是 `29825`。hard bytes 与 soft samples 一同逆交织，API 输出即 logical 顺序。

## 接收顺序与 fail-closed gate

固定顺序：**输入视图/资源验证 → 当前帧定位、独立 A/B 与 Timing 验证 → 精确 profile/layout 识别 → 原始几何 gate → ladder/phase calibration → Data demod**。

Bootstrap 能定位 `[0.5,2.0]` 的缩放，这不意味着 Data 可以解调缩放。Data 默认只允许：

```
abs(scaleX-1)*1920 <= 0.125 physical pixel
abs(scaleY-1)*1080 <= 0.125 physical pixel
distance(originX, nearest integer) <= 0.125 pixel
distance(originY, nearest integer) <= 0.125 pixel
markerResidualPixels <= 0.125 pixel
```

必须先检查未 round 的几何，再选整数像素。随后完整画布必须在输入视图内。`DesktopLevelsDecodePolicy` 只能收紧上述 gate（以及 pilot gate），不能扩大缩放许可。超限明确返回 `ScaleOutOfRange`、`AlignmentOutOfRange` 等 erasure；不 resize、不 silent fallback、不自动切 candidate。

输入复用 `LumaView` 的 Gray8、BGRA8、R10G10B10A2、显式 FP16 linear SDR。应用还验证真实 capture signal metadata：拒绝 HDR/Unknown，允许 10-bit **SDR** 输出。FP16 不是 HDR 通行证；每分量须有限且在 `[0,1]` 内。新数据采样对整数格式用等价的精确整数权重后除法，避免中性 BGRA 整数在严格 midpoint/stddev 边界被浮点乘法误差偏移；旧 locator/sample API 保留原算术。

### Pilot calibration

每个 patch 读取所有 2,048 像素，独立计算均值/方差；不从诊断 payload 估计任何参数。每处 ladder 保持 level 顺序，邻级间距至少 32，patch 标准差不超过 8。同级四处均值的 **max−min** 不超过 8。四处均值平均得到各 capture-space centroid；最小邻级间距用于 soft normalization。

每个 phase pilot 分别验证两轴 4,096 个像素；residual 是 `mean(abs(pixel-expectedCentroid)/(centroid3-centroid0))`。四个 residual 的最大值不超过 `0.125`。这提供定位后的逐像素相位验证，不允许以 Bootstrap 能读出为由跳过。

任一 Pilot 非有限、clipping、level collapse/inversion、过大方差、空间差异或 phase residual 都擦除整帧。Clipping 保守地检查任一 **Luma/RGB 数据分量** 到达格式端点：Gray/BGRA 0/255、R10 0/1023、FP16 RGB 0/1；不将不透明 alpha 视为 clipping。JSON 保留四处各 level 的 centroid/variance、四个 phase residual 和聚合结果；失败时未完成的 calibration 字段不是有效拟合值。

### Direct constellation distance 与 soft metric

对 tile 内真实像素均值 `s` 与校准 centroid `c_i` 直接计算 `d_i=(s-c_i)^2`。每位在对应 Gray label 的两组点中取最小距离：

```
metric(bit) = (minDistance(bit=1)-minDistance(bit=0)) / minimumAdjacentGap^2
```

正号表示 bit 0，负号表示 bit 1，零表示歧义且 hard bit 取 0。无“先估算 level 再做阈值补偿”或已知数据纠正。tile 标准差大于最小邻级间距的 1/4，或任一像素 clipping 时，该 tile 两位 soft metric 都置零；hard bits 仍是 direct-distance 判决。不能将低置信度 tile 制造为强信息。

每 tile margin 为 `(secondDistance-bestDistance)/(secondDistance+bestDistance)`，在 `[0,1]`。统计所有 Data tiles（包括 canonical tail，但不包括 pilots/Bootstrap/Timing），输出 exact min 与 P50/P01/P001。分位数用固定 4,096 bins，`bin=floor(margin*4095)`，nearest-rank 分位点报告该 bin 的下界，绝对量化误差小于 `1/4095≈0.0002442002442`；JSON 明示 `quantileResolution`。

`AdaptSoftMetrics` 使用固定 `round(metric*4096)`、clip `[-32767,32767]` 生成 QC-LDPC int16 输入，非有限输入在写输出前拒绝。本轮 **uncalibrated-max-log-distance** 不是经信道统计校准的 LLR。

## 诊断 payload、FEC 与评分隔离

统一已有 Robust QC-LDPC `N=16200,K=10800`：information 1,350 bytes，coded 2,025 bytes。先对 **adapted soft metric 的 hard decision** 用已有完整 QC-LDPC syndrome 检查；初始 syndrome 为零时不做无意义的迭代，但仍完整检查 CRC、identity 与真值。否则调用已有 normalized min-sum，最多 48 iterations，scale 3/4、offset 0。不能以已知 payload 或另一路 hard bytes 替换 soft erasure。`iterations` 统计实际迭代次数，初始有效 codeword 为 0；旧 FEC API 的迭代语义不变。每个 information block 恰好包含 36-byte Transport framing 和 1,314-byte payload；`SegmentOrdinal=FrameSequence`，`OuterBlockId=slot`，Transport SessionTag 必须与从像素恢复的 Bootstrap 一致。

Payload 可独立重建：连续 BLAKE3-256 digest 的输入为 ASCII `PB-DesktopLevels-X1-Data`（无 NUL）+ canonical Bootstrap 44 bytes + slot LE32 + chunk LE32，chunk 从 0 起；拼接并截到 1,314 bytes。完整 codewords 后必须是 canonical zero tail。

**接收端只在 hard/soft demod 与所有 codeword 的 FEC、Transport CRC、身份检查之后生成期望数据用于评分。** 期望数据不是 decoder 参数，也不能进入定位、calibration、FEC、补全或纠错。工具/应用共用 `ReferenceChannel::Decode/EvaluateCodewords`；独立 Python oracle 不调用生产 renderer/demod/FEC，也不读取生产 C++ 矩阵生成 Expected。

- `PreFecBER = erroneousCodedBits / comparedCodedBits`，仅比较完整 codewords，不使用 padding 稀释。
- `PreFecFER = preFecFailedFrames / frames`，一帧任一 coded bit 错误即失败。
- `PostFecFER = postFecFailedFrames / frames`，任一 FEC、CRC、身份或 canonical padding 失败即失败。
- CRC-valid 但与独立规则生成的真值不符记为 `falseAcceptedCodewords`，不作为已验证 payload 接受，并使 Gate 失败。
- FEC 失败样本仍计入适用分母；Geometry/Pilot erasure、未识别 Bootstrap、重复、capture/readback drops 单列。空分母输出 JSON `null`，不伪造 0。
- JSON 同时保留原始分子/分母、样本数、observed/verified 16-bit phase masks、margin、逐帧 calibration、几何、FEC iterations，以及实际 backend/capture metadata。64-bit identity 序列化为字符串，避免 JSON 消费者丢失精度。

## 生命周期、资源与应用入口

`DesktopLevelsWorkspace` 与 `ReferenceChannel` 是 move-only、每实例单 owner、启动时分配；独立实例可并发。不保留 Analyze 像素借用，不在帧热路径分配 C++ heap 或创建 codec。完整处理 reservation 固定 **16 MiB**，含 modulation scratch、hard/soft buffers、现有 LDPC workspace、候选/统计及有界事件状态；计入现有 readback 总 reservation，未充足预留则启动拒绝。栈上的发送 scratch 也固定有界。

应用 processor 使用已有 **Analyze → Commit/Discard → Reset**：Analyze 不更新已接受统计或身份；Commit 线性化后才去重/计分。FEC 失败的有效身份也消费去重记录，重复捕获不增加独立分母、不跨帧合并 soft metrics。同一 SessionTag 不能切换 candidate，即使 sequence 新增也拒绝；更换不兼容 candidate 必须创建新 Session。

CaptureEpoch/完整 domain 改变时清空 live calibration、history、候选。DesktopLevels 模式保留有界、已经 Commit 的不可变 audit events（原 domain），直到 telemetry 消费，避免 shutdown/reset 抹掉最后一个失败样本的原始证据；它们不能重新提交，也不是当前 epoch 的 calibration。旧 Bootstrap-only 的事件处理不变。真实 worker 测试覆盖旧候选 stale commit、Discard、recreate 和停止清理。

在仓库根目录运行（路径中的 ROI 是当前机器示例，应先验证实际物理坐标）：

```powershell
Set-Location D:\MyProjects\PixelBridge
build-desktop-levels-release\apps\PixelBridgeEncoder\Release\PixelBridgeEncoder.exe --visual desktop-levels-2x2 --frames 80 --telemetry build-desktop-levels-release\encoder-new.jsonl
build-desktop-levels-release\apps\PixelBridgeDecoder\Release\PixelBridgeDecoder.exe --capture-desktop-levels --backend wgc --seconds 30 --roi 320 180 2240 1260 --telemetry build-desktop-levels-release\decoder-new.jsonl
build-desktop-levels-release\tools\Release\PBDesktopLevelsBaseline.exe --baseline
build-desktop-levels-release\tools\Release\PBDesktopLevelsBaseline.exe --input frame.pbrw
```

发送端 Direct-Level 与 ShapeChroma 诊断模式每次成功提交后等待 500 ms 再提交新 sequence；`--sequence-interval-ms 1..60000` 可覆盖该间隔（默认 500，30 秒 native 诊断 Gate 使用 640），仅影响物理层诊断呈现 pacing；真实文件 Gate 使用独立的无额外 dwell sender，由 flip/vsync 和 frame-latency contract 限速并以 receiver `UniqueVisualFPS` 验证。旧命令默认行为不变。输出 telemetry create-only，文件已存在即报错。接收命令成功需至少一帧完整 FEC/CRC/identity/exact-truth 验证，不能仅依赖 Bootstrap；专门 Gate 的样本要求更强。离线 `--input` 仅评分本诊断规则的 PBRW，不冒充通用文件恢复或任意 payload decoder。

## Phase 1 真实文件闭环

`PBPhase1FileGate` 是 tests-only sender/receiver，不新增隐藏 IPC，也不复制生产协议组件。sender 创建固定 seed 的 8 MiB 高熵源文件并保持源 handle 打开以验证 identity/size/mtime 不变；RAW Segment 使用现有 Wirehair V2、1,314-byte Outer block、Transport CRC、Robust QC-LDPC，Control 使用现有 `PB-ReferenceRaster-1` 承载真实 SessionDescriptor/SegmentDescriptor/FinalManifest。data frame 分别使用 `desktop-levels-2x2` 和 `shape-chroma`。receiver 走 WGC/DXGI → CaptureNormalize bounded ROI ring → 同帧固定 A/B Bootstrap → D3D11 demod → QC-LDPC/Transport → ReceiverIngress → Outer FEC → encoded/raw digest → `.part` → WholeFileDigest → 同目录 write-through rename；rename 后再次顺序 digest。最终文件还由父脚本独立 SHA-256/长度比较，`.part` 必须消失。

CaptureNormalize 把 **采集源格式** 与 **归一化输出格式** 分开建模。当前真实 SDR DXGI 桌面可能由 duplication 交付 `DXGI_FORMAT_R16G16B16A16_FLOAT` 或 `DXGI_FORMAT_R10G10B10A2_UNORM`，而 Gate 的 owned ROI/Demod 输入固定为 `DXGI_FORMAT_B8G8R8A8_UNORM`；资源预算分别计入实际 source pool、固定 BGRA8 output ring，以及仅在旋转或格式转换时存在的 per-slot source-format scratch。SDR 的 R10/FP16→BGRA8 与旋转在同一次 bounded GPU transform 中完成，不引入 GPU→CPU→GPU 往返；FP16 scRGB 使用标准 sRGB OETF。未知格式、HDR/非 SDR colorspace、非 BGRA8 目标或任一 checked-size/quota 失败均 fail closed。metadata 同时记录 `sourcePixelFormat` 与归一化 `pixelFormat`，后端原始元数据按 source domain 校验，交给 consumer 的 owned texture 按 output domain 校验；因此 GPU ROI cost 包含实际发生的格式转换，而不会把高位深源错误记成 BGRA8 copy。

每个 132-frame carousel 固定插入 4 个 mixed/torn frame：A 区来自 sequence N，B 区来自 N+1，数据不前移。Gate 要求 `BootstrapMismatch` 计数非零且这些帧不能进入任何 Transport/Control 分母；Control 解码失败、result queue overflow 或 reordered accepted identity 都是失败。文件 Gate 只拥有一个由已验证 `SessionDescriptor` 绑定的输出 Session；此后即使某个像素帧的 Bootstrap 与 Control 各自 CRC 有效，只要其 `SessionTag` 不是该权威 Session，也必须在进入 ReceiverIngress、FER 和 `UniqueVisualFPS` 前作为 bounded erasure 丢弃并单列 `foreignSessionErasedFrames`。CaptureEpoch reset 后、重复 SessionDescriptor 重新绑定 ReceiverIngress 前，合法同 Session 的 Segment/Manifest `UnknownSession` 只允许作为 bounded retryable erasure 单列；重新绑定后同类错误仍是致命状态不一致。初始权威 Session 尚未建立的视觉帧也不计入 `UniqueVisualFPS`，并由 `unboundSessionVisualFrames` 限界。三类 erasure 的固定上限分别为 16、16、128，任何越界都使 Gate 失败，不能借此隐藏持续串帧或接收器失配。CaptureEpoch invalidation 可丢弃尚未被父线程取走的旧 epoch result，但必须单列 `staleResultDrops` 且不超过固定 128-entry ring，不能跨 epoch 接受。数据帧允许记录真实 Post-FEC 失败，但错误 Transport 不能进入 Receiver，最终发布只能在 WholeFileDigest 完全一致后发生。

真实恢复序列包含三条互不替代的证据：

1. 父脚本只对测试自有 `WS_POPUP` DataWindow 做 `1920×1080 → 1600×900 → 1920×1080`，要求稳定 flip/vsync contract 恢复、PresentationEpoch/swap-chain/buffer generation 前进，并在恢复后重新证明 1:1 可见性；缩小时不允许偷偷缩放/接受数据。
2. 每个 backend 在同一 native capture 内请求一次 bounded recreate，CaptureEpoch 前进后 ReceiverIngress fail-closed reset；随后完整 Stop/零 lease/零 busy texture/零 pending GPU work，再创建第二个 native capture，第二次 CaptureEpoch reset 后继续同一个文件闭环。
3. WGC/DXGI/CaptureNormalize 单元和 native integration 保留 mode/ContentSize、access-lost、device-lost、epoch overflow、stale completion、恢复上限及 source GPU retirement 故障注入。Gate 不自动热拔真实显示器或改变用户显示模式；跨实际 adapter/HDR/hot-plug、SDR duplication source-format/bit-depth 变化的硬件矩阵列为 Phase 2 必修，不把确定性恢复测试冒充那一矩阵。

固定 Gate 资源上限为 native queued frame 4、ROI textures/GPU slots 4、WGC FrameLease high-water 6（DXGI 更低）、CaptureDemod pending 4、result ring 128、frame age 250 ms。每个 capture/domain 停止后必须 `liveFrameLeases=0`、`busyRoiTextures=0`、无 deferred cleanup、consumer/domain inactive、D3D demod shutdown。`captureDelivered/captureArrivals` 必须至少 90%，并从原始计数重新核算；drop 与 expired 终态计数之和不能超过 arrivals。GPU cost 分开报告 ROI copy/transform 和 demod timestamp query 平均值及 unavailable 样本；CPU cost 分开报告完整 Bootstrap 检查和 post-GPU/FEC 平均值，父脚本另以进程 CPU-time/wall-time 报告 sender/receiver equivalent cores。GPU timing unavailable 比例不得超过 2%，CPU timing 不可缺失。

WGC 与 DXGI 共用同一个 CaptureRuntime、ROI ring 和 consumer contract，但按生产模型采用不同的有界调度。WGC 的 callback producer 独立入队，因此 owner 先轮询完整 GPU/consumer ring、释放可复用 slot，再只取 callback queue 的最新帧；DXGI 是 owner 同步 acquisition，`AcquireNextFrame` 使用 8 ms 非零且有界的 OS wait，owner 先退休所有 Copying slot 并释放 duplication source，再尝试取得/提交最新帧，最后才执行可能包含 CPU demod/FEC 的 Consuming completion。活动 DXGI 不再叠加通用 condition-variable 定时等待，只在 GPU slot 周转时 yield；等待 environment 时仍使用通用等待。这样既不延长 source lease，也避免 Windows 粗粒度 timer 形成“每次成功 acquire 后再空等一次”的约 30 FPS 假上限，同时保留单 D3D11 immediate-context owner 与停止/重建最多额外 8 ms 的响应界限。

`UniqueVisualFPS` 是 receiver 观察到的稳态显示 cadence：只累计同一 CaptureEpoch 内、相邻且严格递增的 accepted `FrameSequence`，分母使用对应 native capture monotonic timestamp；duplicate、reordered、Bootstrap erasure、sequence gap 与 CaptureEpoch 恢复暂停均不能伪造或拖慢稳态 cadence。Gate 至少要求 60 个 cadence interval 且 `55.0 <= UniqueVisualFPS <= 65.0`。WGC Gate 设置可选的 10 ms `MinUpdateInterval` cadence hint；它快于 60 Hz 源，不节流也不生成新视觉帧，且不能替代 `UniqueVisualFPS` 实测。每个非 long-soak 文件 Gate 在最终文件发布后仍让完整 capture/demod/FEC/Transport 链路运行至少 15 秒，并在 final telemetry 中同时记录要求值与实测 observation；55 FPS、90% delivery、FER、队列边界和恢复计数均对包含启动与两次 CaptureEpoch 恢复的整段运行重新核算，不能删去坏窗口。另报 `EndToEndUniqueVisualFPS=(strictly increasing accepted FrameSequence count-1)/(first..last receiver QPC)`，它保留 resize/recreate/restart/drop 的实际停顿，必须有限、正值且不超过 65 FPS；gap、skipped sequence 与 CaptureEpoch boundary 分别计数并与 receiver reset 交叉验证。`VerifiedEncodedGoodput=verified encoded Segment bytes/(首个已接收 Control 到 encoded/raw Segment 验证完成)`；FER 是所有有合法同帧 Bootstrap、进入物理解调的 data frame 中 Post-FEC failed/evaluated，不能用 capture erasure 缩小分母后再伪称 capture FER。四个 backend/profile 组合都保留原始分子、分母、timing 和错误模式。

Release 最终 Gate 还对 WGC/DXGI × Direct-Level 2×2/ShapeChroma 四组各执行 300 秒 publication 后 soak。每秒采样两个 test-owned 进程；publication 后 30 秒 warmup，比较随后 30 秒与最后 30 秒的 private bytes/handle median，并检查 warmup 后 high-water。median private 增长上限 32 MiB、high-water 增量 64 MiB、median handle 增长 4、high-water 增量 16；任一窗口少于 20 个样本或 post-warmup 少于 200 个样本均失败。该阈值是泄漏 Gate，不是性能预算。

快速单组重放示例（ROI 由脚本从当前物理环境读取，输出 create-only）：

```powershell
pwsh -NoProfile -File tests\DesktopLevelsGate\InvokePhase1FileLoop.ps1 `
  -GateExecutable build-desktop-levels-release\tests\DesktopLevelsGate\Release\PBPhase1FileGate.exe `
  -Support build-desktop-levels-release\tests\DesktopLevelsGate\Release\PBDesktopLevelsNativeSupport.exe `
  -Backend wgc -Profile desktop-levels-2x2 `
  -EvidenceRoot build-desktop-levels-release\tests\DesktopLevelsGate\Release\phase1-file-evidence
```

`-SoakSeconds` 只能是 0 或 300..1680，防止把几秒运行命名为 long soak。脚本只关闭/resize 自己创建的窗口和进程；任何外部遮挡、非 SDR、ROI 不完整或 DXGI pointer guard 失败都会保留 evidence 并返回失败。

## 测试与可重放 Gate

新独立 Golden：两种 candidate × 16 phases 的 record/RS/Timing pins、全 BGRA/data BLAKE3、容量/padding，以及独立 Python Transport/LDPC oracle 的 coded bytes。旧 pins 不覆盖、不 re-pin。生成器 create-only，`--check` 只验证。

CPU 测试覆盖全部 tiles/16 permutations、人工 distances/midpoints 及邻接可表示数、各 strict gate 边界、非线性 calibration、clipping/空间漂移/噪声、quarter phase、各轴 0.5/0.75/1.25/2.0 缩放、row padding、整数平移/ROI 边界、Gray/BGRA/R10/FP16、混帧、输出失败不变、alias/溢出/budget、allocation failure 与热路径无分配、并发实例、可恢复错误与合法错误 codeword。独立 CMake JSON parser 检查真实 serializers 与手工可推导的分子/分母。

`PBDesktopLevelsMutation` 是固定 seed、有界 1..512 次的 **deterministic mutation**；384 次覆盖 12 类 mutation × 两 candidate × 16 phases，另有 8 个 corpus seeds。MSVC ASan 运行不等同 coverage-guided libFuzzer 或 UBSan。

`PB_BUILD_DESKTOP_LEVELS_GATE` 默认 OFF。总 Gate 使用 VS2022 x64 和当前 vcpkg baseline，不新增依赖；Python oracle 需要可用的 `blake3` Python 模块。执行：

```powershell
pwsh -NoProfile -File tests\DesktopLevelsGate\InvokeFinalGate.ps1 `
  -PythonExecutable D:\Python3.12.9\python.exe `
  -CppcheckExecutable <本机cppcheck.exe的完整路径> -Parallel 4
```

脚本分别配置/完整构建 `build-desktop-levels-release`、`build-desktop-levels-asan`，执行相关及全量 CTest、旧/新 Golden oracle、离线 baseline、ASan mutation/corpus 与 cppcheck。全量 CTest 包含每个配置下 **WGC × DXGI × 2×2/4×4/ShapeChroma** 的六个 30 秒真实诊断入口，以及 **WGC × DXGI × Direct-Level 2×2/ShapeChroma** 的四个真实文件闭环；Release 快速矩阵全部通过后，最终脚本再串行执行四个 300 秒 Release soak。Release 诊断要求至少 16 个完整验证帧且覆盖全部 16 phases；ASan 配置逐帧解码慢于 640 ms sequence 节奏，保持同一 30 秒窗口下的文档化 baseline 要求（至少 8 个完整验证帧、至少 8 个不同 phase）。诊断发送端以 80 帧 × 640 ms（约 51.2 s，长于 receiver 的 45 s 有界 deadline 与启动余量）提交，并在 receiver 完成后自然跑完，以保留成功退出状态和完整 sender telemetry。每个 phase 有多个副本，同 phase 副本间隔至少 10.24 s，单次 readback 停顿不能消灭任一 phase。任何错误接受、原始 JSONL 与汇总分母不一致、文件/digest/恢复/soak/timing/样本不足或捕获契约失败均为失败。真实缩小显示案例还要求 Bootstrap 能读、Data 明确 scale erasure；无法完整显示的放大案例由 CPU 矩阵承担。Gate 在启动前 fail-closed 检查系统稳定性：存在待重启标志（`PendingFileRenameOperations`、WindowsUpdate 或 CBS `RebootRequired`）或系统启动不足 600 s 即拒绝启动，避免计划内系统重启（如 Windows Update）在长时 native 采集中途杀死进程树并使证据不完整；若运行期间仍发生重启，Gate 以明确的 fatal error 终止并保留证据，不产出部分 PASS。

所有 native 测试共享 `PixelBridgeDesktop` resource lock。只定位/管理测试自有 DataWindow，不自动改变 HDR、分辨率、DPI、显示模式、光标或其他应用窗口。必须有真实 SDR、完整物理画布和 cursor-excluded capture contract，前提失败是失败，不 skip、不降门槛。帧龄 gate 以 backend 声明时间与 inbox 入队时 QPC 实测 arrival 中最早的有效值为准：声明早于自身 arrival 的超前时间戳（WGC SystemRelativeTime 实测可超前数毫秒）被 arrival 取代，两者均无效则拒绝该帧，不猜测帧龄。Gate 仅在 DXGI 捕获路径对 pointerInsideRoi 施加 fail-closed 环境检查（WGC 以 `IsCursorCaptureEnabled(false)` 显式禁用光标捕获，帧不受指针位置影响；DXGI 路径实测交付无指针像素，仍按保守契约拒绝指针位于 ROI 内的运行，防止操作员活动污染 30 s 证据），并披露 TopmostRaisedApplied。

证据放在忽略的 `build-desktop-levels-evidence/` 及两个 build tree 内：源 HEAD、完整 tracked/untracked 源文件 fingerprint（允许提交前工作区，但 Gate 期间不得变化）、编译配置/依赖、命令及 stdout/stderr、JUnit、原始 JSONL、显示/ROI 可见性、resize、逐秒资源样本、WholeFileDigest/SHA-256、两物理层 VerifiedEncodedGoodput/FER/GPU/CPU cost、主要错误模式、四组 long soak 和测试二进制 SHA256。Gate 不自动 commit；`phase1-comparison.json` 只有在 12 个诊断组、8 个 Release/ASan 快速文件组和 4 个 Release long-soak 组全部 PASS 后才给出 `TagAllowedAfterDiffReview=true`。仍须无未解决 Critical/High、完成最终 scoped diff/static review 和原子 commit，才可创建非 amend 的 `phase1-gate-pass` tag。`ExecutionGate=PASS` 仍不等于 Certified Profile 或最终系统完成。

静态检查完整保留 cppcheck XML，不做 blanket warning suppression。`tests/DesktopLevelsGate/cppcheck_review.json` 逐条记录本次审查的既有 style 建议/分析器误报、理由、行号、消息和源文件 SHA256；例如不能删除解锁后 revision/domain 重验，也不能删除先释放旧 COM generation 的语句。工具版本、消息、位置或文件 hash 改变，或出现任何新诊断，Gate 必须重新审查并拒绝自动放行。新 DesktopLevels 核心、共享 evaluator、baseline tool、mutation runner 不在该既有诊断清单中。
