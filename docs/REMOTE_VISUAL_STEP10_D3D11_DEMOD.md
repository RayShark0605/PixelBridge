# PixelBridge RemoteVisual Step 10 LF4 D3D11 Scaled Walsh Demod

状态：**DONE。`PB-RemoteVisual-LF4-X1` 已有独立的 experimental Step-10 D3D11 demod submission path：它直接从同一 admitted observation 的 PB-owned BGRA8 ROI texture 按连续 geometry 采样，GPU 生成 64,800 个 logical soft metrics、16-entry calibration 与 99-entry freshness summary，并只回读这些 compact 结果。WARP exact/scale/blur/stale、NaN/out-of-bounds/work-budget、bounded ring 与 epoch cancellation 已通过；compact metrics 经冻结的 LF4 calibration candidate 和现有 QC-LDPC/Transport evaluator 得到与 CPU reference 相同的 accepted Transport blocks。该路径尚未接入 `CaptureDemodulator`、GUI/CLI 或 product Decoder admission，也不是 hardware truth parity、真实 capture lifetime closure、Receiver/WholeFileDigest、Certified Profile 或 field certification。**

## 1. 本步骤关闭的边界

Step 10 只回答以下窄问题：

1. GPU 是否能在不回读整张 ROI 的前提下，按 locator 已恢复的连续 `(originX, originY, scaleX, scaleY)` 直接采样 LF4 calibration ladder、freshness tag 和 Walsh data tile；
2. GPU 输出能否保持固定、有界、可审计，并在同一 frame/domain/Bootstrap binding 下进入现有 QC-LDPC、Transport CRC、identity 和 padding gate；
3. event-query completion、PB-owned source texture lifetime、readback slot/ring 与 capture epoch cancellation 是否保持原 `PBDemodD3D11` 的 one-owner/fail-closed 语义；
4. exact、连续尺度、独立 blur 与局部 stale fixture 的 production GPU accepted Transport bytes 是否与 CPU reference 一致。

本步骤特意不关闭以下边界：

- `SubmitRemoteVisualLowFps` 是独立 experimental API；普通 `Submit` 遇到 LF4 binding 会返回 `UnsupportedProfile`，旧 strict-1:1 RemoteVisual shader 不会静默解释 LF4 layout；
- `CaptureDemodulator` 与应用 runtime 尚未调用该 API；product Decoder validation、GUI combo 与公开 CLI token 仍不暴露 LF4；
- geometry 和 canonical Bootstrap 必须由调用方从**同一 admitted pixel observation/domain**恢复；Step 10 不平行实现第二套 locator 或 Bootstrap admission；
- WARP 是 D3D11 shader/reference runtime 证据，不是 NVIDIA/AMD hardware parity；Step 11 单独关闭 adapter/driver truth matrix；
- 对 borrowed PB-owned texture 的引用不授权调用方提前复用 capture slot；Step 12 才把 WGC/DXGI `Completed`、CaptureEpoch、geometry 与 lease retirement 接到 product pipeline；
- accepted Transport block 不是 Outer/Receiver/file convergence，最终文件仍必须通过 WholeFileDigest 后才能发布。

## 2. Compatibility 与 public boundary

LF4 继续使用 Step 08 冻结的独立 wire identity：

| 字段 | 值 |
| --- | --- |
| Visual Profile | `PB-RemoteVisual-LF4-X1` |
| `VisualProfileId` | `0x504252564C463431` |
| `LayoutVersion` | `7` |
| logical canvas | `1920×1080` |
| data tile | `8×8`，4×4 Walsh chips，每 chip `2×2` logical pixels |
| coded payload | `64,800` bits / `8,100` bytes |
| inner FEC | `4 × 2,025-byte` Robust QC-LDPC codeword |
| freshness summary | `98` region entries + `1` global invariant entry |

`ParseBinding` 识别 LF4 仅用于建立精确的 metric/data/codeword bounds。调用边界仍严格分离：

- `Submit(frame, ..., lf4Bootstrap, ...)`：拒绝，避免旧 shader 重解释；
- `SubmitUnbound(..., kRemoteVisualLowFpsProfileId, ...)`：拒绝，Step 10 不允许在没有同帧 geometry/Bootstrap authority 的情况下猜测 LF4；
- `SubmitRemoteVisualLowFps(frame, ..., lf4Bootstrap, geometry, policy, ...)`：唯一的 Step-10 入口；只在 frame、adapter、domain、texture、geometry、policy 和 work budget 全部通过后提交 GPU work。

`DemodFrameResult` 新增 `remoteUnreliableSymbols`，其余已有 FEC、accepted Transport、freshness、metric summary、GPU timing 与 snapshot 字段继续复用。没有新增 socket、pipe、shared memory、clipboard、文件交换或其他像素外 payload 通道。

## 3. Shader 与共享常量

LF4 使用独立 `remote_visual_low_fps_compute.hlsl`，避免扩大或重解释旧 strict shader。三个 compute entry 按同一 immediate context 顺序执行：

1. `CalibrateRemoteVisualLowFpsCS`
   - 对四组冻结 ladder 的 0/3 endpoint 各采样 `8×8` 点；
   - 输出 mean、variance 与 clipping count；
   - CPU readback 后按 LF4 policy 验证每组 endpoint order、全局 separation、variance 与跨 ladder `max-min` spatial deviation。
2. `DemodRemoteVisualLowFpsFreshnessCS`
   - 每个 freshness tile 采样 16 个 chip，每 chip 使用中心与四个 `±0.45` offset，共 5 个双线性 sample；
   - 对 expected freshness bit 计算 mismatch/erasure，并以 region-scoped `InterlockedOr` 标记 stale；
   - 任何 sample failure 写入 global invariant entry，CPU 侧 fail closed。
3. `DemodRemoteVisualLowFpsCS`
   - 每个 data tile 使用相同 16×5 sample；
   - 与 16-mask codebook 计算 residual、RMS、best/second margin 和四个 bit-plane metric；
   - unreliable symbol 输出四个 zero metric；stale region 即使完成 diagnostic reliability 计数，最终仍把该 region 的全部有效 data metrics 置零；
   - logical output index 由当前 `FrameSequence` 的 frozen permutation/plane offset 在 CPU 建立，不在 HLSL 中复制另一套 interleave 实现。

codebook mask 与 21,456-entry tile mapping 都由 production CPU 常量/API建立为 immutable structured buffer；per-frame logical binding 由 `GetRemoteVisualLowFpsLogicalBit` 与 `GetRemoteVisualFreshnessBit` 建立为 bounded per-slot buffer。编译期 drift shields 固定并交叉检查：

- `4` bit/tile、`16` 个 symbol mask、`64,800` coded bits；
- `FreshnessTag=1`、`Data=2` 的 shader role binding；
- 四组 ladder 的 origin 与 `128×64` geometry。

任何这些 CPU 常量变化而 HLSL 未同步时，构建直接失败，不允许静默漂移。

## 4. 连续采样、format 与资源上界

Step-10 texture contract 为：

- `DXGI_FORMAT_B8G8R8A8_UNORM`、`D3D11_USAGE_DEFAULT`、单 mip、单 array slice、sample count 1；
- 必须带 `D3D11_BIND_SHADER_RESOURCE`，且 texture device identity 与 demodulator device 相同；
- metadata 仍必须证明 SDR RGB、非 HDR、cursor excluded、physical ROI/owned texture size 一致、adapter LUID 一致；
- LF4 允许经过 capture normalization 的 variable ROI dimensions，但每个 dimension 必须为正且不超过 D3D11 texture limit；旧 profiles 继续保持原 1920×1080 strict contract。

constant buffer 显式包含 origin、scale、`1/sourceWidth`、`1/sourceHeight`、source bounds、calibration/symbol/freshness policy 与 region count。HLSL 使用 linear-clamp sampler，logical sample 先转换为：

```text
physical = origin + scale * logical - 0.5
texture  = (physical + 0.5) / sourceSize
```

采样前仍检查 physical position 位于 `[0, sourceSize - 1]`；far corner、非有限 geometry、scale/alignment policy 与 texture dimensions 在 GPU submission 前已由 CPU fail closed。测试上传的 BGRA rows 带 `+17` bytes padding，证明 CPU upload pitch 不被假定为 tight；GPU texture sampling 使用实际 resource layout，不将 staging row pitch当作 logical width。

固定 compact readback 为：

| 内容 | bytes |
| --- | ---: |
| `64,800 × float` logical metrics | `259,200` |
| `16 × float4` calibration | `256` |
| `99 × uint4` freshness/global summary | `1,584` |
| 合计 | `261,040` |

1920×1080 BGRA8 ROI 为 `8,294,400` bytes；compact readback 约为其 `3.15%`，`rawPixelReadbackBytes` 保持 `0`。metric staging buffer 虽按模块最大 profile 预留，但每帧只用 `CopySubresourceRegion` 复制当前 binding 的 metric prefix。

resident budget 使用 checked multiply/add 覆盖每个 slot 的 metric/calibration/freshness default+staging pair、per-frame binding、constant buffer，以及固定 CPU/GPU mapping/codebook storage。`maximumDataWorkUnits` 继续遵循冻结 CPU-reference 的 source-pixel-read 定义：一次双线性 sample 最多访问 4 个 texel，因此 GPU submission 按所有 calibration/data/freshness samples 的 `×4` worst case 保守计费。开发审计曾发现按一次 `SampleLevel` 计 1 unit 会让低预算在 GPU 接受、CPU 拒绝；最终实现已修正，并以 `2,000,000` units 的负例防回归。

## 5. Completion、slot 与 source lifetime

`Demodulator` 沿用一个 immediate-context owner thread 和配置为 `2..4` 的固定 slot ring：

- 第三个提交在 2-slot ring 满时返回 `Busy`，不覆盖 pending work，submission output 保持不变；
- submission 对 source texture 持有引用，但 caller 仍必须保持 PB-owned capture slot lease，直到 `Poll` ready 或外部已证明完成的 retirement；
- calibration、freshness、data、compact copies、timestamp end 与 `D3D11_QUERY_EVENT` 按同一 context 顺序提交；
- `Poll(..., D3D11_ASYNC_GETDATA_DONOTFLUSH)` 只有 event ready 后才执行 `Map`；代码没有以 `Flush`、Present 或 CPU submission 冒充 GPU completion；
- `InvalidateDomain` 只把匹配 pending work 标记 cancelled；texture/readback slot 仍等 event completion 后才真正退休；
- wrong thread、wrong context/device、adapter mismatch、stale generation/observation/domain、invalid submission 与 device removal 都 fail closed，失败不修改 caller output。

Step-10 negative test 在同一 epoch 提交两个 LF4 frame、验证第三个 `Busy`，随后 invalidates domain；两个 slot 都必须在各自 event ready 后返回 `Cancelled`。最终 snapshot 为 submitted=2、completed=0、cancelled=2、failed=0、pending=0、highWater=2、metric/raw readback=0。

## 6. FEC 与 truth boundary

GPU raw metrics 先经过 Step 07/08 冻结但仍为 experimental candidate 的 `CalibrateRemoteVisualLowFpsMetric`，再调用原 `ReferenceChannel::EvaluateCodewords`。这没有修改：

- Robust QC-LDPC wire matrix/profile；
- Transport serialization、CRC、SessionTag identity 与 canonical padding；
- accepted Transport block representation；
- Receiver、Outer FEC、segment digest、WholeFileDigest 或 final publish 语义。

该 calibration 只存在于新的 experimental LF4 API；既有 product Decoder default/admission 未切换。CPU oracle 仍从同一实际 raster pixels 运行 production reference locate/bootstrap/demod/FEC；GPU Gate 比较 authoritative `FrameEvaluation` 与 accepted Transport bytes，不要求 raw float bit-identical，也不借用 sender expected bytes完成解调。

## 7. 验证矩阵

### 7.1 Shader 编译

三个 entry 使用 Windows SDK `fxc.exe` 的 strict/warnings-as-errors gate：

```powershell
$fxc = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.19041.0\x64\fxc.exe'
& $fxc /nologo /Ges /WX /T cs_5_0 /E CalibrateRemoteVisualLowFpsCS `
  libs\PBDemodD3D11\src\remote_visual_low_fps_compute.hlsl
& $fxc /nologo /Ges /WX /T cs_5_0 /E DemodRemoteVisualLowFpsFreshnessCS `
  libs\PBDemodD3D11\src\remote_visual_low_fps_compute.hlsl
& $fxc /nologo /Ges /WX /T cs_5_0 /E DemodRemoteVisualLowFpsCS `
  libs\PBDemodD3D11\src\remote_visual_low_fps_compute.hlsl
```

三个 entry 均 PASS。

### 7.2 WARP fixture

```powershell
cmake -S . -B build-presentation-release
cmake --build build-presentation-release --config Release --target PBDemodD3D11Tests PBRemoteVisualTests

build-presentation-release\tests\PBDemodD3D11\Release\PBDemodD3D11Tests.exe `
  '[low-fps]' --reporter console --durations yes

ctest --test-dir build-presentation-release -C Release `
  -R '^(PBDemodD3D11Tests|PBRemoteVisualTests)$' --output-on-failure
```

| case | 独立输入/变化 | authoritative result |
| --- | --- | --- |
| exact | canonical LF4 raster，BGRA padded upload pitch | 4/4 accepted Transport 与 CPU byte-identical；zero metrics=0；compact=261,040；raw ROI=0 |
| scale | independent Area resample，`1.259375 × 1.259259...`，fractional origin | CPU/GPU accepted Transport 与 freshness/reliability counters一致 |
| blur | independent fixed 3×3 Gaussian kernel | CPU/GPU accepted Transport 与 counters一致 |
| stale | 把 previous-sequence 的一个完整 freshness region复制到 current frame | 1 stale region；该 region metrics 全部 zero；FEC 仍恢复 4/4 accepted Transport |
| negative/lifetime | NaN、far-corner OOB、NaN policy、2,000,000 work units、2-slot saturation、epoch invalidation | 全部 exact fail closed；caller output不变；无 premature readback |

`PBRemoteVisualTests` 额外回归 CPU/reference、Golden、FEC 与 RemoteVisual contract，1/1 PASS。这里没有重跑或冒充 Step 01..09 的全部 field/corpus Gate。

### 7.3 ASan

ASan tree 需要显式 Qt CMake path，并把 MSVC ASan runtime 只加入当前测试进程的 `PATH`：

```powershell
cmake -S . -B build-presentation-asan `
  -DQT_DIR='D:/Qt6.10.1/6.10.1/msvc2022_64/lib/cmake/Qt6' `
  -DQt6_DIR='D:/Qt6.10.1/6.10.1/msvc2022_64/lib/cmake/Qt6'
cmake --build build-presentation-asan --config RelWithDebInfo --target PBDemodD3D11Tests

$asanDir = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64'
$env:PATH = "$asanDir;$env:PATH"
$env:ASAN_OPTIONS = 'halt_on_error=1:abort_on_error=1:detect_leaks=0'
build-presentation-asan\tests\PBDemodD3D11\RelWithDebInfo\PBDemodD3D11Tests.exe `
  '[low-fps]' --reporter console --durations yes
```

4/4 LF4 cases PASS；完整 13-case `PBDemodD3D11Tests` 也 PASS。ASan 验证 CPU memory/error paths；WARP runtime 与 D3D event/lifetime assertions是单独边界，二者不能互相替代。

## 8. Step 10 完成出口

完成出口逐项闭合：

- PB-owned texture direct sampling：PASS；没有 ROI-sized CPU normalization/readback；
- continuous geometry constant/policy/bounds：PASS；exact、scale、blur与 OOB/NaN negative覆盖；
- shared mapping/codebook/frame binding：PASS；CPU source of truth + compile-time drift shields；
- freshness/stale erasure：PASS；region-scoped summary与 stale zero metrics进入原 FEC gate；
- one-owner/bounded ring/event completion/source retention：PASS；没有 `Flush == completion`；
- unchanged FEC/Transport truth：PASS；同 frame CPU/GPU authoritative accepted Transport bytes一致；
- old profile compatibility：PASS；完整 13-case demod regression与 CPU `PBRemoteVisualTests` 通过，普通 `Submit` 不解释 LF4。

因此 Step 10 状态为 `DONE`。这只是 experimental D3D11 LF4 demod primitive；它不提升 Step 11/12/14/17/20/27 的状态。

## 9. 后续边界

Step 11 必须在 WARP 与所有当前可用 hardware adapter 上运行同一 corpus，记录 adapter description/vendor/device/LUID/driver、device-recreate、metric summary、GPU timing、FEC disposition 与 accepted Transport manifest。硬件不可用必须记为 unavailable，不能由 WARP 替代；比较终点是 accepted Transport bytes/CRC/identity/padding，不是 float bit equality。

Step 12 才能把 LF4 API 接入 CaptureNormalize consumer：locator/Bootstrap/geometry必须来自同一 observation；WGC/DXGI PB-owned texture lease必须在 demod event completion 后才 `Completed`；ContentSize/mode/monitor/device/CaptureEpoch变化必须 drain stale work并重建资源。GUI/CLI/profile exposure、Receiver/Outer/WholeFileDigest 与 field Gate仍按原路线后续步骤执行。
