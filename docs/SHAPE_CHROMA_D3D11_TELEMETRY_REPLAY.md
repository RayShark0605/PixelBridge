# ShapeChroma、D3D11 Demod、Telemetry 与 Real Capture Replay baseline

本文记录本轮 Phase 1 A/B baseline 的已实现边界。它不是 Certified Profile 冻结，也不把离线/WARP 结果冒充真实显示链路认证。

## 1. PB-Mod-ShapeChroma-1

`PB-Mod-ShapeChroma-1` 是与 Direct-Level 共用 LocalDesktop Bootstrap、数据网格、`FrameSequence % 16` 空间 phase、QC-LDPC、Transport framing/CRC/identity gate 和 `AcceptedTransportBlock` 输出的 CPU reference 分支。

- `VisualProfileId = 0x5042534843503031`，`VisualLayoutVersion = 4`。
- 每个 4×4 tile 携带 4 shape bits 与 2 chroma bits，共 6 bits；16 个 literal shape template、4 个固定 chroma state。
- 86,688 个数据 tile，520,128 coded bits，65,016 data bytes，32 个 LDPC codeword，尾部 216 bytes 必须为 canonical zero padding。
- codebook 是可审计的初始 literal table；没有搜索优化、训练、learned model 或运行时自适应 codebook。
- CPU decoder 对每个 tile 分别计算 16 个 shape template SSE metric 与 4 个 chroma state SSE metric，共 20 个 metric，并从竞争标签的最小 metric 差产生 6 个 bit soft metric。hard bytes 和 soft metric 都先按同一 phase 逆交织，再进入现有 `ReferenceChannel::EvaluateCodewords`。
- A/B 判定的权威输出不是 float metric bit pattern，而是同一 CPU Transport/FEC gate 最终接受的 Transport Block bytes。

## 2. PBDemodD3D11 baseline

`PB::PBDemodD3D11` 的输入必须是同一 capture device/adapter 上的 PixelBridge-owned upright ROI texture。它只覆盖 Bootstrap/locate 已从同一像素观测恢复后的 Data demod 阶段：调用方传入的 canonical 44-byte Bootstrap 是该观测的已恢复绑定，不是隐藏 payload 身份通道。

路径固定为：

```text
PB-owned ROI texture
  -> HLSL Compute calibration/data metric
  -> compact staging metric buffer
  -> nonblocking event-query retirement
  -> CPU metric adaptation + existing QC-LDPC/Transport gate
```

约束：

- Create/Submit/Poll/InvalidateDomain/Shutdown 由同一 immediate-context owner thread 调用；context/device 不匹配直接拒绝。
- capture metadata 的 adapter LUID 必须与 D3D11 device adapter 一致。
- Submit 保留输入 texture 到 event query 完成；domain invalidation 只标记 cancelled，仍由 Poll 安全退休 GPU/source lifetime。
- 不创建 raw-pixel staging/readback；`rawPixelReadbackBytes` 恒为 0。CPU 只读取 compact calibration/metric buffer，之后直接进入 CPU FEC；不存在 GPU→CPU→GPU roundtrip。
- ShapeChroma、Direct-Level 4×4、Direct-Level 2×2 都用 CPU reference 作 oracle。float 中间值允许运算顺序差异；最终 `FrameEvaluation` 关键 gate 与每个 accepted Transport Block 必须一致。

## 3. PBTelemetry 口径

`PB::PBTelemetry` 是单 owner、按 CaptureEpoch 重新开始的聚合器。它消费各组件已经形成的权威观测，不自行猜测 PresentedVisualFPS、像素唯一性、Transport 接受或 verified bytes。

| 字段 | 口径 |
|---|---|
| PresentCallFPS / PresentedVisualFPS | 直接保留 `PBPresentTiming::TimingSnapshot`；不从 capture cadence 反推 |
| CaptureFPS | 至少两个 capture timestamp 后，按 `(capturedFrames-1)/elapsed` |
| UniqueVisualFPS | 只对提供像素 BLAKE3 identity 的帧计算；相邻相同 digest 是 duplicate，不用 timestamp 猜唯一 |
| duplicate/drop/jitter | duplicate 保留原始分子；drop 由 capture owner 显式上报；jitter 是相邻 capture interval 的 population standard deviation |
| ROI copy time | D3D11 disjoint/start/end timestamp query 覆盖 crop/copy/rotation；unsupported/disjoint 明确记 unavailable，不用 CPU submit/completion 时间替代 |
| BootstrapSuccessRate / scale / phase | 每次 committed capture 一个 Bootstrap denominator；成功样本保留最近 scale 与 fractional phase |
| PreFecBER | 独立 admitted FrameSequence 的 erroneous coded bits / compared coded bits |
| FER | post-FEC 未完整验证帧 / FEC evaluated frames；另输出 codeword failure rate，避免混淆分母 |
| CRC failure / Outer symbols | 原始计数；outer accepted/duplicate/conflict/rejected 由权威 receiver admission 显式上报 |
| VerifiedEncodedGoodput | 只有 storage/whole-file digest 权威 gate 后调用 `RecordVerifiedEncodedBytes` 才增加；不把 demod/CRC 中间成功算作 verified bytes |

计数溢出时采用 saturating counter 并撤回 derived rate。Decoder Bootstrap 诊断已经把 committed capture、Bootstrap 和去重后的 DesktopLevels FEC 观测接到该聚合器；该诊断模式没有 file receiver，因此 Outer symbol 与 VerifiedEncodedGoodput 在此模式保持零/null。

## 4. PB Real Capture Replay v1

`PB::PBRealCaptureReplay` 提供 streaming Writer/Reader。Writer 同时要求 sender canonical raster 与 captured ROI；Reader 一次只分配/返回一帧，后续可直接把 `capturedRoi` 交给 CPU reference 或上传到同 adapter 的 D3D11 texture，无需再次 Present/捕获。

### 4.1 文件发布与 trust boundary

- Writer 以 `CREATE_NEW` 创建同目录 `<target>.partial`，不覆盖 final 或既有 partial。
- `Finalize` 只在精确写满声明 frame count 后写 footer、`FlushFileBuffers`、关闭 handle，再以不替换目标的 `MoveFileExW(..., MOVEFILE_WRITE_THROUGH)` 发布。
- 未 Finalize、任何中途 I/O failure 或 publish failure 都不会留下可被 Reader 误认的 final；Writer 析构删除自己的 incomplete partial。
- Reader 先用 `GetFileSizeEx` 对 `maximumFileBytes` fail-fast，再校验固定 header/footer、whole-stream CRC32C，之后才暴露 reader。
- 每帧在分配前验证 record length、dimension、format、tight row pitch、每帧/累计 raster quota；读入后再校验 record CRC32C、sender/captured 两个 BLAKE3-256、canonical Bootstrap/FrameSequence binding 和 capture metadata contract。失败时调用方 output 不变。
- CRC32C 用于 torn/corrupt format 检测；BLAKE3 是持久数据完整性，不是发送者认证。

### 4.2 固定 little-endian layout

File header 是 96 bytes：

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 8 | `PBRCR001` |
| 8 | 2 | format version = 1 |
| 10 | 2 | header bytes = 96 |
| 12 | 4 | endian sentinel `0x01020304` |
| 16 | 4 | dataset class |
| 20 | 4 | expected frame count |
| 24 | 16 | local dataset ID |
| 40 | 8 | first frame offset = 96 |
| 48 | 44 | reserved zero |
| 92 | 4 | CRC32C over `[0,92)` |

每个 frame record 是 `512-byte fixed header || displayIdentityUtf8 || senderRaster || capturedRoi || uint32 recordCrc32c`。512-byte header 固定包含：record length/ordinal、FrameSequence、两个 raster 的 width/height/tight row pitch/pixel format/byte count、canonical 44-byte Bootstrap、Capture source ID/Epoch/Observation、source/slot generation、backend、timestamp/signal/HDR/cursor、physical ROI/source extent、rotation、adapter LUID、bits/color space、raw/normalized/arrival timestamp、GPU ROI copy time、DPI/scale、Present epoch/QPC/sample/present ID、两个 raster BLAKE3 digest，以及 pointer metadata。header CRC32C 位于 offset 508，覆盖 `[0,508)`；record trailer CRC32C 覆盖整个 header 与三个 variable payload。

Footer 是 40 bytes：

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 8 | `PBRFT001` |
| 8 | 2 | format version = 1 |
| 10 | 2 | footer bytes = 40 |
| 12 | 4 | finalized frame count |
| 16 | 8 | bytes before footer |
| 24 | 4 | CRC32C over all bytes before footer |
| 28 | 8 | reserved zero |
| 36 | 4 | footer CRC32C over `[0,36)` |

dataset class v1 固定为 `LocalDesktop-ExactCandidate`、`LocalDesktop-Degraded`、`LocalVideo-CertifiedPlayer`、`LocalVideo-GenericPlayer`、`FailureCases` 五种。UTF-8 display identity 不能为空且必须是严格 well-formed UTF-8；v1 sender canonical raster 固定 BGRA8，captured ROI 支持当前 capture baseline 的 BGRA8、R10G10B10A2 与 FP16 scRGB，均以 tight rows 保存。

### 4.3 离线回放验证

`PBRealCaptureReplayTests` 生成 ShapeChroma 与 Direct-Level v1 dataset，关闭 writer 后重新 Open：

1. 独立解析并 CRC 校验 file header；
2. 从 captured ROI 运行 CPU locate/demod/QC-LDPC/Transport；
3. 把同一 replay frame 上传到 WARP texture，运行 PBDemodD3D11；
4. 比较 CPU/GPU 每一个 accepted Transport Block；
5. 验证 overwrite/incomplete cleanup、truncation、file/frame quota、whole-stream CRC、独立 record CRC 与 BLAKE3 failure，且失败不修改输出。

Release 示例：

```powershell
cmake --build build-desktop-levels-release --config Release --target PBRealCaptureReplayTests --parallel 8
ctest --test-dir build-desktop-levels-release --build-config Release --output-on-failure -R '^PBRealCaptureReplayTests$'
```

当前库提供受控 recorder/replay API，但本轮没有声称已经采集到新的真实显示器 dataset；真实 SDR/WGC/DXGI 证据仍必须由 native Gate 在可用显示环境下单独生成和封存。
