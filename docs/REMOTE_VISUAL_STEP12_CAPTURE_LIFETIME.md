# PixelBridge RemoteVisual Step 12 CaptureNormalize / Geometry / Epoch / D3D Lifetime

状态：**DONE。`PB-RemoteVisual-LF4-X1` 已接入现有 `CaptureDemodulator` 与 WGC/DXGI 共用的 `CaptureNormalize` owner-thread 路径。LF4 Bootstrap、continuous geometry 与 Step-10 direct-texture GPU demod 现在绑定到同一个 PB-owned upright ROI、同一个 `ScreenCaptureDomain`、同一个 `CaptureEpoch` 和同一个 capture observation。Capture runtime 允许 consumer 在首个 completion marker 后提交恰好一个有界 GPU continuation，并在第二个 marker 完成前继续持有原 ROI slot；取消回调不暴露 texture/context，异常按“可能已提交 GPU work”保守退休。WGC upright 与 DXGI ROTATE90 normalization 的完整 WARP 集成测试均跨 epoch recreate 得到每帧 4 个与 CPU reference byte-identical 的 accepted Transport blocks，停止时 lease、ROI、queue 和 pending demod 全部归零。**

该状态只关闭 Step 12 的 capture/geometry/epoch/lifetime 边界。它不表示 LF4 已加入 GUI/CLI profile 列表，不表示 Transport 已进入 production Receiver/Outer mutation，也不表示 WholeFileDigest、final publish、provider matrix、双机完整文件恢复、VerifiedEncodedGoodput 或 Certified Profile 已完成。

## 1. 本步骤关闭的问题

Step 10/11 已证明 LF4 D3D11 demod primitive 在 PB-owned texture 上的协议真值，但当时 caller 必须直接提供已经解析好的 Bootstrap 与 continuous geometry。Step 12 关闭以下缺口：

1. 从 WGC/DXGI acquire surface 到 upright PB-owned ROI 的 copy/rotation/retirement是否仍是唯一 capture 路径；
2. Bootstrap locator、profile identity、geometry 与 data demod 是否来自同一个 captured observation，而不是不同帧或 hidden metadata；
3. Bootstrap CPU readback结束后，是否能在不归还 ROI slot 的前提下从该 exact texture提交 LF4 GPU work；
4. completion callback发生 failure/exception、请求第二次 continuation、epoch invalidation、recreate、device loss或 shutdown时，GPU work和 source lease是否总能有界退休；
5. exact、scaled、letterboxed与 cropped/invalid geometry是否具有显式状态，未知或不完整 geometry是否只能产生 erasure/telemetry而不能产生 accepted payload；
6. LF4的最大ROI和全部预分配是否在 capture pool/ring创建前完成 checked resource admission；
7. optional diagnostic branch是否可以安全共存，而不会覆盖 primary consumer的协议真值或无限延长 ROI lease。

## 2. Staged completion contract

### 2.1 公开但向后兼容的 consumer contract

`PBCaptureNormalize` 新增：

```cpp
struct CaptureConsumerCompletion
{
    CaptureStatus status;
    bool gpuWorkSubmitted = false;
};
```

`RawRoiConsumer::CompleteStage` 与 `ScreenCaptureConsumer::CompleteStage` 默认调用原有 `Completed` 并返回 `gpuWorkSubmitted=false`，所以旧 consumer不会被迫改变语义。只有明确需要在第一个 retirement marker后继续使用同一个 ROI的 consumer才覆盖该方法。

`gpuWorkSubmitted=true` 是精确的 lifetime claim：callback已在传入的 one-owner immediate context上提交了额外 D3D11 work；capture runtime必须保留 PB-owned ROI slot并记录新的 query/fence marker。运行时只允许一次 non-cancelled continuation：

```text
copy / normalize / consumer Submit
  -> marker #1 retires Bootstrap staging copy
  -> CompleteStage(texture, context, cancelled=false)
       may submit LF4 GPU work and return gpuWorkSubmitted=true
  -> marker #2 retires LF4 GPU reads/copies
  -> CompleteStage(same texture, same context, cancelled=false)
       must return gpuWorkSubmitted=false
  -> release ROI slot and source lease
```

第二个 callback再请求 continuation时，runtime先让已经声明的 GPU work通过已记录 marker安全退休，然后以 `ConsumerFailure/Completion` fail closed，并进行一次不暴露 GPU objects的 terminal cancellation；不会形成第三个 marker或无界 callback chain。

### 2.2 取消、异常与 GPU object ownership

- non-cancelled completion只获得当前 slot的 exact PB-owned ROI texture与 owner immediate context；
- `NormalizeConsumer`核对 retained pointer identity、device、context、domain、epoch、observation、slot与generation，不允许替换 texture或跨 device使用；
- cancelled completion始终收到 `texture=nullptr`、`context=nullptr`，不得 `Map`、提交 GPU work或申请 continuation；
- callback如果在可能提交 GPU work后抛异常，`D3dRoiRing`会 `ClearState`、记录保守 retirement marker并返回 `gpuWorkSubmitted=true`；slot不会因异常提前复用；
- marker/fence signal或 debug validation在提交后失败时，emergency query已先被置为 pending，后续仍可通过 `Poll`有界退休；
- `Flush`从未被当作完成证明。

`CaptureSnapshot`新增 `consumerContinuationSubmissions`、`consumerContinuationCompletions` 与 `consumerContinuationRejections`，因此每个额外 lifetime stage都有权威计数，而不是从 callback次数猜测。

## 3. LF4 same-observation geometry pipeline

### 3.1 Bound Bootstrap API

`PBModulation`新增接受冻结 `LocalDesktopBootstrapBinding` 的 continuous Bootstrap overload。它只解析绑定中允许的 profile/layout identity，并复用原有有界 locator；错误已知 binding、未知 profile或不匹配 layout都产生 `UnsupportedRecord` erasure，不会根据像素内容猜测另一个 profile。

`DecodeRemoteVisualLowFpsFrame`也改为调用这一单一入口，避免 CPU reference与 capture integration各自维护一套 locator/profile选择逻辑。

### 3.2 两阶段 LF4 CaptureDemodulator

strict 1:1 profiles保持原路径：`Submit`同时排队 Bootstrap staging与data demod。LF4则采用以下顺序：

1. `DomainStarted`先核对实际 D3D11 device的 DXGI adapter LUID与 `CaptureEnvironment`；不一致在 `CaptureStage::Adapter`、任何大分配或 frame admission之前拒绝；
2. `Submit`验证 active domain、epoch、observation、format、ROI尺寸、adapter、cursor语义和 exact PB-owned texture，然后只把同一 ROI复制到 Bootstrap staging；
3. marker #1完成后，首个 `CompleteStage`对 staging执行有界 CPU Bootstrap locator，得到 canonical 44-byte record与 continuous `originX/originY/scaleX/scaleY`；
4. 只有 Bootstrap/profile/layout/geometry全部可接受时，才调用现有 `Demodulator::SubmitRemoteVisualLowFps`，输入仍是该 observation的 exact borrowed ROI texture；callback返回 `gpuWorkSubmitted=true`；
5. marker #2完成后，第二个 `CompleteStage`调用现有 compact `Poll`，运行 QC-LDPC/Transport evaluation，将 result放入固定容量 ring，然后释放 pending state和 ROI lease。

由此，Bootstrap与四个 codeword不可能来自两个 observation，且 epoch replacement不会把旧 geometry用于新 texture。

### 3.3 Geometry状态与 fail-closed边界

`CaptureDemodulatorGeometryStatus`固定为：

- `ExactCanvas`：logical 1920x1080画布与ROI精确一致；
- `Scaled`：画布连续缩放后填满ROI；
- `Letterboxed`：完整逻辑画布位于更大的ROI中，允许fractional origin与非整数scale；
- `Rejected`：locator/profile/画布边界不完整，或存在crop/OOB；
- `NotApplicable`：非LF4 profile。

状态进入每个 `CaptureDemodulatorResult`和snapshot counters。1800x1080 cropped LF4负例只产生 `TelemetryOnly/Rejected`：`stagedGpuSubmissions=0`、底层demod submitted=0、accepted Transport=0。LF4 signal-layer `InvalidFrame`同样按可观测erasure处理，不会把不完整像素提升成 fatal product payload。

## 4. 资源与readback真值

LF4允许实际ROI小于或等于配置hard bounds，默认上限为3840x2160；0维、超过D3D11 texture limit、实际ROI超过hard bound、NaN policy及算术/预算溢出均在分配前拒绝。

Bootstrap staging按最坏ROI显式预留：

```text
maximumRoiWidth * maximumRoiHeight * 4 * slotCount
```

LF4不再另配strict-profile的1920x1080 reference scratch；result ring、demod slots与fixed overhead仍逐项计入 `CaptureDemodulatorBudget`。预算差1 byte拒绝，exact budget接受，failure保持output不变。

必须区分两种readback：

- LF4 **data demod**继续满足Step 10：每帧只回读261,040-byte compact metrics/freshness/calibration，`rawPixelReadbackBytes=0`；
- 当前Bootstrap locator仍需从同一ROI的staging进行一次**完整ROI CPU readback**，并由 `bootstrapReadbackBytes`精确记账。这是Step 12显式保留的reference/correctness路径，不得把Step 10“data plane无raw ROI readback”误报为整个capture pipeline零全帧readback。

后续若优化Bootstrap定位，只能在保持same-observation、continuous geometry与fail-closed真值后进行，不能为了性能删掉当前可靠reference path。

## 5. Epoch、recreate、rotation 与 drain

完整集成测试使用现有 WGC/DXGI facade、production `CaptureRuntime`、`NormalizeConsumer`和真实 WARP `D3dRoiRing`；测试backend只注入immutable source surface，不创建第二套capture runtime：

| backend facade | source语义 | normalization | authoritative endpoint |
| --- | --- | --- | --- |
| WGC | upright 1920x1080 BGRA8 | copy/crop到PB-owned upright ROI | epoch 1和epoch 2各4个accepted Transport，与CPU逐字节一致 |
| DXGI | raw 1080x1920、`ROTATE90` | 现有rotation shader恢复upright 1920x1080 ROI | 与WGC、CPU得到同一4-block set |

每个backend在epoch 1完成后执行 `RequestRecreate`：旧domain先invalidated并完全drain，新domain使用同一随机source identity但`captureEpoch=2`。第二帧必须从新epoch重新定位Bootstrap并重新提交LF4 GPU work。关键断言：

- 两个epoch共 `consumerContinuationSubmissions=2`、`consumerContinuationCompletions=2`、rejections=0；
- `CaptureDemodulator` staged submissions/completions均为2，accepted Transport共8；
- `frameLeaseHighWater=1`，每轮结束及stop后 `busyRoiTextures=0`、`liveFrameLeases=0`、`queuedFrames=0`；
- pending capture/demod/result状态归零，shutdown不是deferred cleanup；
- stale old-epoch observation不会进入new-domain output。

既有 capture regression同时覆盖ContentSize/source description变化、access/device loss、device recovery上限、stale completion、rotation resource recreate、deferred cleanup与shutdown drain。Step 12没有把测试facade误称为一次真实远控field capture；真实provider链路仍由后续manual Gates负责。

## 6. Optional diagnostic fanout

原本位于 `local_desktop_runtime.cpp`匿名作用域的optional diagnostic fanout被抽成可测试的 `pbapp::detail::OptionalDiagnosticFanout`，仍由现有product runtime使用，没有增加平行架构。

其规则为：

- primary consumer始终是capture/protocol authority；diagnostic failure只记录到独立状态；
- 两分支的reservation都在capture allocation前计费，fixed pending table最多8项；
- 只有某分支在首个completion明确请求continuation时，第二阶段才回调该分支；
- diagnostic-only GPU work可以延长一次ROI lease，但不能产生或替换primary truth；
- diagnostic `DomainStarted`失败只禁用diagnostic branch；Submit failure/throw在marker退休后只接收一次null-object cancellation；
- branch callback throw按可能已经提交GPU work保守延长一次marker；cancelled callback若非法请求continuation会被强制拒绝；
- primary failure/throw保留authoritative failure与terminal cancellation路由。

测试覆盖primary/diagnostic continuation组合、diagnostic domain/submit/completion failure和exception、wrong completion identity、第二continuation cleanup、legacy no-GPU completion及saturating resource accounting。

## 7. 验证矩阵

### 7.1 Targeted Release

```text
PBCaptureNormalizeTests "[staged-completion]"             73 assertions / 2 cases  PASS
PBCaptureRotationTests "[exception]"                     22 assertions / 1 case   PASS
PBApplicationTests "[fanout]"                           126 assertions / 3 cases  PASS
PBDemodD3D11Tests "[capture][low-fps]"               89,334 assertions / 4 cases  PASS
PBCapturePipelineTests "[staged-completion]"          1,077 assertions / 3 cases  PASS
```

这些用例分别证明contract identity/cancellation、post-submit exception retirement、product fanout、geometry/resource/adapter negatives，以及WGC/DXGI两epoch end-to-end Transport truth。

### 7.2 Full affected Release gate

```powershell
ctest --test-dir build-presentation-release -C Release `
  -R '^(PBApplicationTests|PBModulationTests|PBRemoteVisualTests|PBScreenCaptureWgcTests|PBCaptureRotationTests|PBCaptureNormalizeTests|PBCapturePipelineTests|PBDemodD3D11Tests|PBScreenCaptureDxgiTests)$' `
  --output-on-failure
```

结果：9/9 PASS，0失败，总时长173.34秒；其中完整 `PBDemodD3D11Tests` 125.02秒，完整 capture pipeline 21.08秒。

### 7.3 Full affected ASan gate

instrumented tree只构建targeted tests时，先显式构建/运行 `PBTestAsanRuntimeCopy`，确保每个test目录都有同一toolchain的 `clang_rt.asan_dynamic-x86_64.dll`。缺失该runtime表现为Windows loader `0xc0000135`，不是测试或sanitizer finding。

同一9-suite在 `RelWithDebInfo`、`ASAN_OPTIONS=halt_on_error=1:abort_on_error=1:detect_leaks=0`下最终结果：9/9 PASS，0失败，总时长421.45秒；完整 `PBDemodD3D11Tests` 300.47秒，无ASan finding。由于该完整17-case WARP矩阵实测约300秒，instrumented `PB_BUILD_FUZZERS`配置的CTest timeout由180秒有界调整为420秒；Release仍保持180秒，assertion与失败条件均未放宽。

两棵已测试build tree都在CTest timeout变更后重新configure；`ctest --show-only=json-v1`确认只有instrumented配置使用420秒。

## 8. 完成出口

Step 12完成出口逐项闭合：

- same observation Bootstrap + geometry + GPU data：PASS；
- exact PB-owned ROI在两次marker之间持续有效：PASS；
- WGC upright / DXGI ROTATE90 normalization：PASS；
- epoch invalidation、drain、recreate与new-domain admission：PASS；
- cancel不暴露GPU objects，exception不premature reuse：PASS；
- 第二continuation有界拒绝并terminal cancel：PASS；
- crop/unknown geometry只telemetry、不data admission：PASS；
- adapter LUID在allocation/admission前校验：PASS；
- hard ROI、policy、resident budget checked admission：PASS；
- lease/ROI/pending/queue HWM与shutdown归零：PASS；
- strict 1:1 profile、WGC、DXGI、application fanout、CPU/reference regression：PASS；
- Release与ASan affected suites：PASS。

因此Step 12状态为 `DONE`。下一步是Step 13：在不改变本步骤lifetime contract的前提下，明确 `(CaptureEpoch, FrameSequence)` duplicate/reorder/gap/stall admission与所有queue的bounded stale-drop语义。Step 12没有提前把LF4 result写入Receiver/Outer，也没有改变WholeFileDigest与安全发布的最终authority。
