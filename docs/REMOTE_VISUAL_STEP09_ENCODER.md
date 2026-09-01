# PixelBridge RemoteVisual Step 09 LF4 D3D11 Encoder / Immutable Source

状态：**DONE。`PB-RemoteVisual-LF4-X1` 已接入 production-code `SenderFrameBuilder` 与 `EncoderRuntime` 的隐藏 Encoder-only candidate；完整 1920×1080 BGRA raster 以 1..5 Hz 原子替换 immutable D3D11 source，显示许可可对同一 source 重复 `Present`，但不推进 `FrameSequence`、Carousel 或 `OuterBlockId`。这不是 GUI/CLI exposure、production Decoder deployment、屏幕捕获证明、Certified Profile 或双机文件传输 field certification。**

## 1. 本步骤关闭的边界

Step 09 回答发送端的三个窄问题：

1. production sender 是否能够用冻结的 LF4 wire identity 生成真实 Carousel，包括四份相同的 Robust Control copy 与四个独立 Data codeword；
2. 完整 logical raster 是否先在不可变 source texture 中建立并验证，再由每次 flip 完整复制到 back buffer，而不是逐 tile 改写可见纹理；
3. logical source replacement 是否严格限制为 1..5 Hz，同时显示刷新许可可以持续重画同一 source，且 sender 不读取或推断 receiver completion。

本步骤特意不关闭以下边界：

- LF4 没有加入 Encoder GUI、公开 CLI token 或 Decoder product selection；
- 既有 `RemoteVisualResilient` 继续使用原 `kRemoteVisualProfileId`、原 layout、原一-bit/tile raster 与现有 Decoder 路径，没有被静默重解释；
- 没有 LF4 production D3D11 demod shader，没有从 WGC/DXGI capture texture 解调；
- GPU source staging readback 不是屏幕捕获，DXGI Present 统计也不是 receiver `UniqueVisualFPS`；
- test-local completion marker 没有进入 `EncoderRuntime`，只用于证明 sender 在外部观察之后仍继续 Carousel；它不是实际 Decoder completion；
- 没有 Outer convergence、WholeFileDigest、final publish、VerifiedEncodedGoodput、provider matrix 或 soak 结论。

## 2. Compatibility 与 exposure

`VisualProfile::RemoteVisualLowFps` 被追加在 enum 尾部，只作为内部 candidate：

- Encoder validation 可接受它，以便原生 Gate 调用真实 `EncoderRuntime`；
- Decoder validation 明确拒绝它；
- Qt profile combo 与 Encoder CLI 均不提供该值；
- 原三个 enum 值及其公开行为不变；
- `CURRENT_RUNTIME_OPTION_INVENTORY.md` 把 LF4 gate 列在 evidence-only/hidden boundary，而不是 GUI product option。

冻结 binding 为：

| 字段 | 值 |
| --- | --- |
| Visual Profile | `PB-RemoteVisual-LF4-X1` |
| `VisualProfileId` | `0x504252564C463431` |
| `LayoutVersion` | `7` |
| coded bytes / frame | `8100` |
| codewords / frame | `4 × 2025` |
| logical canvas | `1920×1080 BGRA8` |
| allowed logical FPS | `1..5`；`0` 与 `>5` fail closed |

Control frame 先把 PB-Control-1 bytes 放入 1350-byte information window，再编码一份 2025-byte Robust QC-LDPC codeword，并把该**完整 codeword**逐字节复制到四个 LF4 slot。Data frame 继续产生四个独立 Transport/QC-LDPC codeword。两类 frame 最后都调用同一个 production `EncodeRemoteVisualLowFpsFrame`。

## 3. DataWindow active-source state machine

`DataWindowConfig::repeatActiveFrame` 默认 `false`，所以旧调用方语义不变；只有隐藏 LF4 Encoder candidate 显式启用。状态机遵守：

- `SubmitFrame` 只接受一份完整、row-pitch 已验证的 owned pending raster；pending capacity 固定为 1，后来的完整候选可替换尚未呈现的候选；
- frame-latency permit 到达后，pending raster 只上传一次并成为 active source；
- 后续 permit 在没有新 pending 时只重复呈现 active source，并沿用同一 `FrameSequence`；
- 新完整 pending 成功上传后才替换 active source；不向可见 back buffer 逐 tile 写入；
- presentation epoch、mode/size/device/statistics-disjoint 或 failure 会使 active identity 失效；旧 source 不跨 epoch 重播；
- shutdown 清除 pending/in-flight/active 状态并释放 HWND、D3D11 object 与 owned handle。

新增的发送端/Presentation telemetry 包括：

- `sourceTextureReplacements`、`repeatedPresentCalls`、`invalidatedActiveFrames`；
- `activeFrame`、`activeFrameSequence`、`activeFramePresentationEpoch`；
- `configuredLogicalDwellMilliseconds`、`minimumObservedLogicalDwellMilliseconds`、`logicalDwellViolationCount`；
- 冻结的实际 `visualProfileId`、layout、coded bytes 与 codeword count。

DataWindow JSON 记录 source/repeat/invalidation/active identity（含 active epoch）；Encoder snapshot、run report 和 bounded evidence journal 记录应用可见的 source/repeat/invalidation/active/dwell telemetry；实际 wire profile/layout/data/codeword binding 进入 Encoder snapshot 与 run report。Encoder report 仍不输出 receiver progress、receiver ETA 或 VerifiedEncodedGoodput。

## 4. Native D3D11 immutable source

native backend 对每个新 logical raster 执行：

1. 创建 `D3D11_USAGE_IMMUTABLE`、`DXGI_FORMAT_B8G8R8A8_UNORM`、`D3D11_BIND_SHADER_RESOURCE` 的完整 1920×1080 source texture；
2. 在 diagnostic verify 模式中把 source 复制到 staging texture，等待实际 completion 后逐 row/pitch 与 CPU bytes 比较；
3. 对 staging rows 计算 BLAKE3；只有 byte comparison、BLAKE3 provenance 与 D3D11 debug check 全部成功后才替换当前 source；
4. 每次 `Present` 前执行一次完整 `CopyResource(source -> swap-chain back buffer)`；flip-discard 不依赖旧 back-buffer contents；
5. reconfigure/resize/release 先 drain，随后使 source 失效并重建依赖资源。

开发 Gate 首次创建 immutable texture 时使用 `BindFlags=0`，运行时返回 `E_INVALIDARG`。修复为显式 `D3D11_BIND_SHADER_RESOURCE` 后，WARP 与硬件均通过；该失败没有用 fallback 或取消 immutable contract 来掩盖。首次有效 Present 又触发了可复现的 DXGI `statistics-disjoint` epoch，Gate 因此使用独立 sequence 16 warm-up，并验证旧 active source 被失效；canonical sequence 17 从新 epoch 重新提交，避免把 stale source 留在统计样本中。

提交前又把 cleanup assertion 从“graphics objects 为零”收紧到“graphics objects、owned handles、HWND 全部为零”。新 Gate 首次按预期失败，并定位到已经 `Stopped` 的 `DataWindow` 仍把 wake event 保留到 backend destructor；`Shutdown` 现在在 owner thread 离开全部 wait 后显式关闭 wake event，既有 partial-initialization/native GPU failure matrix 也同步要求 `liveOwnedHandles=0`。最终 WARP、hardware 与 LF4 Gate 全部在该更强 contract 下通过。

## 5. Logical cadence 与 sender independence

`EncoderRuntime` 使用 `ceil<steady_clock::duration>(1 / logicalVisualFps)` 生成调度间隔，避免向下截断造成早于 200 ms 的 5 Hz replacement。观测 dwell 以成功提交时刻之间的间隔计算；任何小于配置 dwell 的值都增加 `logicalDwellViolationCount`。

`generatedVisualFramesPerSecond` 使用 `(N - 1) / (lastLogicalFrameAt - firstLogicalFrameAt)`。初始 frame 不被错误地算作一个已经经过的完整 interval，因此 5 Hz Gate 不会因启动瞬间的第一帧产生虚假超限。`presentCallFps` 独立记录显示许可驱动的重复 Present，不能解释为新的 logical payload frame。

production Gate 在两个完整 Carousel cycle 后记录一个 test-local marker，但不把该 marker 或任何 receiver 状态传入 runtime。随后必须看到第 9 个 logical frame，并且只有显式 `RequestStop`/`Stop` 才能终止广播。停止报告的状态文本明确记录 sender 没有推断 receiver completion。

## 6. Gate 与重放命令

构建与无窗口单元回归：

```powershell
cmake --build build-presentation-release --config Release --target `
  PBPresentationGate PBRenderD3DTests PBApplicationTests PixelBridgeEncoder PixelBridgeDecoder

ctest --test-dir build-presentation-release -C Release `
  -R "^(PBApplicationTests|PBRenderD3DTests)$" --output-on-failure
```

右侧显示器原生 Gate（每次 evidence root 必须不存在；窗口使用 no-activate，不发送鼠标或键盘输入）：

```powershell
build-presentation-release\tests\PresentationGate\Release\PBPresentationGate.exe `
  --lf4-encoder-warp <new-evidence-root>

build-presentation-release\tests\PresentationGate\Release\PBPresentationGate.exe `
  --lf4-encoder-hardware <new-evidence-root>

build-presentation-release\tests\PresentationGate\Release\PBPresentationGate.exe `
  --lf4-production-encoder <new-evidence-root>
```

当前机器的 Gate 要求至少两个 active monitor，选择最右输出并把 1920×1080 client 居中。本轮实际输出为 `\\.\DISPLAY2`，物理模式 2560×1440@180 Hz，client origin `(2880,180)`。窗口不激活、不取得 foreground；Gate 不关闭或移动用户窗口，也不改变显示设置。

## 7. 最终证据

### 7.1 Canonical source / repeat Present

Step 08 canonical raw BGRA BLAKE3：

```text
28b09b66520b87f9cd9407b516530ad1225f84d390cc2b94d18d6e1b8d5e9bc4
```

CPU raster 与 GPU immutable-source staging readback 在 WARP 和 RTX 5090 D hardware 两种 backend 中都逐字节一致；每个 Present 都有一份完整 source-to-back-buffer copy：

| Gate | canonical seq 17 | replacement seq 18 | evidence SHA-256 |
| --- | --- | --- | --- |
| WARP | sources=2，repeat=24，present calls=26 | sources=3，repeat=35，present calls=38 | `gate.txt`=`c1915ac2cfe6252cc00a87fa25ab0dd57b515c28fc771d75d598fbcc3dac1d38`；`snapshots.jsonl`=`03901b91450748adc21aeade271377c17ea7485b5bc8e58d2c24332f0535bdc3` |
| hardware / RTX 5090 D | sources=2，repeat=24，present calls=26 | sources=3，repeat=35，present calls=38 | `gate.txt`=`07ab026a8103428554f5d7a93a252b20e72328397febc10d1f0698c2cf326d01`；`snapshots.jsonl`=`d487329b96f0201886c3fd04aa6ca58dfa6d00f74a9f9017a789c0d34d22c27d` |

最终 create-only evidence：

```text
build-p1_5-evidence/20260901-step09-final-warp-d/lf4-encoder-warp-57364-718624121037
build-p1_5-evidence/20260901-step09-final-hardware-d/lf4-encoder-hardware-46976-718627462559
```

WARP snapshot 中的 adapter description 是关联 output 的 RTX 名称；`softwareRasterizer=true` 才是 backend authority，不能把该文本误报成硬件执行。

### 7.2 Production EncoderRuntime

最终 create-only evidence：

```text
build-p1_5-evidence/20260901-step09-final-production-d/lf4-production-encoder-59608-718636392197
```

| 状态 | frame / cycle | cadence | immutable/repeat | artifact SHA-256 |
| --- | --- | --- | --- | --- |
| Broadcasting | frame 8；2 cycles；4 frames/cycle | generated 4.973131591 fps；configured/min dwell 200/200.0132 ms；violations=0 | sources=8；repeats=68；pending=0；active=true | `849c714ea99c33548d8288845173bfdc47cf3a44bde90d9e3644863eb7236133` |
| after test-local marker | frame 9；2 cycles | generated 4.973155837 fps；min dwell 200.0132 ms；violations=0 | sources=9；repeats=79；pending=0；active=true | `7c42ecb106658b92ba06a03467e8759b5d631c77e8511a63213b928ec29fca8c` |
| explicit stop | frame 9；Stopped | cadence identity retained | sources=9；repeats=79；pending=0；active=false | `64af2bf0d9f00b7395b3097692f522f6cd68c86f6a16ca7f30d4b32043a5648b` |

`gate.txt` SHA-256=`19cd4bad562e16aaafc03a9ae70250c8be7837f6579856896b9f4a99decaad34`；1-byte create-only source SHA-256=`8de0b3c47f112c59745f717a626932264c422a7563954872e237b223af4ad643`。三份 report 的 receiver progress、ETA 与 VerifiedEncodedGoodput 均为 `null`/不存在，不把 sender cadence 冒充 transfer convergence。

## 8. 提交前验证矩阵

| 验证 | 结果 |
| --- | --- |
| Release `PBApplicationTests` + `PBRenderD3DTests` | 2/2 PASS，6.23 s |
| Release `ALL_BUILD` / 无窗口全量 CTest | PASS / 155/155 PASS，237.54 s；排除 `Native|GuiSmoke` |
| WARP immutable/repeat native Gate | PASS；canonical CPU/GPU source hash exact；Stop 后 graphics/handles/HWND 为零 |
| RTX 5090 D immutable/repeat native Gate | PASS；canonical CPU/GPU source hash exact；Stop 后 graphics/handles/HWND 为零 |
| 既有 WARP/hardware presentation Gate | PASS；partial initialization、swap-chain replacement、4 组 flip/latency matrix 与 strengthened handle cleanup contract 通过 |
| production EncoderRuntime native Gate | PASS；2 cycles + marker 后继续 + explicit stop |
| MSVC ASan/RelWithDebInfo `ALL_BUILD` / 无窗口全量 CTest | PASS / 290/290 PASS，488.73 s；`halt_on_error=1:abort_on_error=1:detect_leaks=0`；D3D/handle cleanup 由原生 Gate 单独验证 |
| Qt 5.14.2 Release `PBApplicationTests` | 1/1 PASS，5.54 s |
| Qt 6.10.1 Release `PBApplicationTests` | 1/1 PASS，5.50 s |
| Qt5/Qt6 Encoder build | PASS；只有已知 `VCINSTALLDIR is not set` deploy warning，target 成功 |
| cppcheck 2.21.0 | PBRenderD3D 与 PBPresentationGate 零发现；PBApplication/PixelBridgeEncoder 仅精确匹配既有 review ledger，零 new/unreviewed finding |
| `git diff --check` | PASS |

WARP、ASan、Qt build-tree 与 GPU source readback 分别验证不同边界；它们不能互相替代，也都不能冒充实际 remote capture/receiver certification。

## 9. 后续边界

Step 10 必须实现 LF4 D3D11 scaled Walsh demod shader：直接从 PB-owned capture texture 按连续 `(originX, originY, scaleX, scaleY)` 采样，输出固定 64,800 soft metrics 与 freshness summary，只回读 compact results，不做 ROI-sized GPU→CPU→GPU round trip。它还必须覆盖 exact/scale/blur/stale、NaN/out-of-bounds、epoch stale completion 与 bounded slot/ring。

Step 11 才能在 CPU、WARP 与实际 hardware adapter 之间比较同一 corpus 的 Bootstrap/FEC disposition/accepted Transport bytes；Step 12 以后才闭合 WGC/DXGI lease、CaptureEpoch 与 geometry。LF4 的 GUI/CLI exposure 和 production Decoder admission 仍需等后续 Gate，不因 Step 09 完成而提前启用。
