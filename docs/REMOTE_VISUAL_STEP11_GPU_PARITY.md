# PixelBridge RemoteVisual Step 11 CPU/WARP/Hardware GPU Truth Parity

状态：**DONE。`PB-RemoteVisual-LF4-X1` 的 Step-10 experimental D3D11 demod path 已在同一台 Windows 主机当前可枚举的 WARP、AMD 与 NVIDIA D3D11 adapter 上运行同一组 exact/independent-area-scale/independent-gaussian-blur/stale-region corpus。5 个 backend identity（1 个 WARP LUID、1 个 AMD LUID、3 个 NVIDIA LUID）在 generation 0 的四个场景以及 device recreate 后的 exact 场景都产生与 CPU reference 完全相同的 FEC/CRC/identity/padding disposition 和 accepted Transport bytes。错误 adapter LUID 在 GPU submission 前 fail closed，全部 GPU timestamp 有效，compact readback 固定为每帧 261,040 bytes，raw ROI readback 为 0。该结论只关闭当前可用 adapter/driver 的协议真值矩阵；LF4 仍未接入 product `CaptureDemodulator`、WGC/DXGI lease/epoch、GUI/CLI、Receiver/WholeFileDigest 或安全发布。**

## 1. 本步骤关闭的边界

Step 11 只回答以下问题：

1. Step 10 的同一组 LF4 shader、CPU-built mapping/codebook/frame binding 与既有 FEC/Transport evaluator，在 WARP、NVIDIA 和 AMD 当前可用 D3D11 adapter 上是否给出相同的**协议接受真值**；
2. 比较终点是否是 canonical Bootstrap、FrameSequence、FEC/CRC/identity/padding disposition 和 accepted Transport bytes，而不是不可移植的 raw float bit equality；
3. adapter LUID 不一致时是否在提交前拒绝，而不是隐式 cross-adapter copy、CPU fallback 或降级接受；
4. 销毁并重建 D3D11 device、context 和 demodulator 后，canonical exact case 是否仍得到相同 accepted Transport set；
5. adapter fingerprint、metric summary、GPU timestamp、compact/raw readback 和 shutdown/device-removal 状态是否形成可解析、可封存证据。

本步骤不声称：

- 同名 adapter description 等价于同一 adapter。当前系统枚举出三个名称相同但 LUID 不同的 RTX 5090 D adapter identity，Gate 按 LUID 分别创建设备并分别执行；
- WARP 可以替代缺失硬件。枚举或设备创建失败的 hardware adapter 只能写为 `available=false`；Gate 不为它运行 WARP 替身；
- 当前通过可以覆盖未来未运行的 GPU、driver、操作系统或 D3D runtime；新 adapter/driver 进入支持矩阵时必须重跑同一 Gate；
- GPU parity 等于真实 capture lifetime、live Decoder、Receiver convergence、WholeFileDigest、final publish、VerifiedEncodedGoodput 或 Certified Profile。

## 2. 单一路径 Gate，而不是第二套实现

Step 11 复用 `PBDemodD3D11Tests` 已有的 Step-10 production-code test harness，并只新增一个隐藏 Catch2 case：

```text
[.gpu-parity]
```

默认 `PBDemodD3D11Tests` 不选择隐藏 case，因此原 WARP 回归的执行时间和选择语义不变。CMake 另注册串行、300 秒超时的 `PBRemoteVisualGpuParityTests`，显式选择该 tag。Gate 调用的仍是公开 experimental API `Demodulator::SubmitRemoteVisualLowFps`，没有复制 shader、FEC、Transport parser 或 accepted-block evaluator。

CPU oracle 从每个实际 fixture 的 BGRA pixels 运行现有 `ReferenceChannel::DecodeRemoteVisualLowFps`。GPU 结果再通过现有 `FrameEvaluation` 和 `AcceptedTransportBlock` 结构比较。两边都从像素恢复数据；测试没有把 sender expected bytes 传给 GPU demodulator，也没有通过文件、IPC 或其它隐藏通道提供 payload。

## 3. Adapter 枚举与身份

硬件枚举流程为：

1. `CreateDXGIFactory1`；
2. 有界枚举最多 64 个 `IDXGIAdapter1`；
3. 排除 `DXGI_ADAPTER_FLAG_SOFTWARE`；
4. 对每个候选用 `D3D_DRIVER_TYPE_UNKNOWN` 尝试 feature level 11.1，旧 runtime 返回 `E_INVALIDARG` 时再只尝试 11.0；
5. 设备创建失败仍写 adapter fingerprint、HRESULT 和 `available=false`，但不把它算作已验证硬件；
6. WARP 用 `D3D_DRIVER_TYPE_WARP` 单独创建设备，并以 `softwareRasterizer=true` 作为 authority，不能根据 description 文本误报为硬件；
7. 对每个可用 backend 从实际 device 反查 `IDXGIAdapter1`，再次核对 vendor/device/subsystem/revision/LUID。

fingerprint 记录：backend、DXGI index、description、software flag、VendorId、DeviceId、SubSysId、Revision、三类 memory、LUID、`CheckInterfaceSupport(IDXGIDevice)` 返回的 raw driver version，以及实际 feature level。

本轮当前主机矩阵如下；`45312 = 0xB100 = D3D_FEATURE_LEVEL_11_1`：

| backend | DXGI index | description | vendor/device | LUID | driverVersionRaw | feature level | result |
| --- | ---: | --- | --- | --- | ---: | --- | --- |
| WARP | n/a | Microsoft Basic Render Driver | `0x1414/0x008C` | `00000000:0001895a` | `2814751477605438` | 11.1 | PASS |
| hardware | 0 | NVIDIA GeForce RTX 5090 D | `0x10DE/0x2B87` | `00000000:000172f1` | `9007199255791224` | 11.1 | PASS |
| hardware | 1 | AMD Radeon(TM) Graphics | `0x1002/0x13C0` | `00000000:000189ad` | `9007200633820041` | 11.1 | PASS |
| hardware | 2 | NVIDIA GeForce RTX 5090 D | `0x10DE/0x2B87` | `00000000:00023612` | `9007199255791224` | 11.1 | PASS |
| hardware | 3 | NVIDIA GeForce RTX 5090 D | `0x10DE/0x2B87` | `00000000:0002223a` | `9007199255791224` | 11.1 | PASS |

本轮 `hardwareAdaptersEnumerated=4`、`hardwareAdaptersAvailable=4`，所以不存在被 WARP 替代或静默跳过的硬件；summary 中 NVIDIA 与 AMD 均为 `pass`。

## 4. Authoritative corpus

generation 0 在每个 backend 上执行相同四场景：

| scenario | 唯一变化 | canonical identity | authoritative result |
| --- | --- | --- | --- |
| `exact` | canonical LF4 BGRA raster | SessionTag `0xa74c35e29180db6f`，FrameSequence 401 | 4/4 Transport accepted；0 FEC/CRC/identity/false accept |
| `scale-area` | independent Area resample，`1.259375 × 1.259259...`，fractional origin | 与 exact 相同 | 4/4 accepted；同一 accepted set |
| `blur-gaussian-3x3` | independent fixed 3×3 Gaussian | 与 exact 相同 | 4/4 accepted；同一 accepted set |
| `stale-region` | previous sequence 510 的一个 freshness region 覆盖 current sequence 511 | SessionTag `0xbc913e4075a26d8f`，FrameSequence 511 | exactly 1 stale region；869 data metrics erased；FEC 仍恢复 4/4 Transport |

每个场景逐字段比较：

- `evaluated`、`paddingValid`、codeword count；
- FEC、CRC、identity、false-accept disposition；
- accepted Transport/control counts；
- compared/erroneous coded-bit diagnostics；
- freshness region、stale/mismatch/erasure 和 unreliable-symbol counters；
- 每个 accepted block 的 slot、byteCount 和全部 bytes。

`iterationsTotal/Maximum` 与 metric magnitude进入诊断证据，但跨 adapter Gate 不以 raw float bit equality 为前提。所有当前 backend 实际也给出相同的 metric summary与 FEC iteration disposition：exact/scale/blur 为零 bit error，stale 为 454 erroneous coded bits、5 total / 2 maximum iterations并仍成功恢复。

## 5. Accepted Transport manifest

set digest 不是简单拼接不定长对象，而是按输出顺序对每个 block 的以下 bytes 做 BLAKE3：

```text
LE32(slot) || LE32(byteCount) || exact serialized Transport bytes
```

exact、scale 与 blur 在全部 generation-0 backend，以及所有 generation-1 recreate exact run 上只有一个 set digest：

```text
1f7fd593d668849ee8d5ac134b2581bb8a2cb20c05bcb1f0fcebe1e18465ae24
```

四个 1,350-byte block 的 BLAKE3 为：

```text
slot 0  42fb59e3b986303e12ce576942cd9fc9280fa93ccdedf96362a6f01d124cc04b
slot 1  83121c1a823deb9defc195c1e6ea9128db48cf04f23207f2356525dbc98d7cfc
slot 2  32969dd75bc8ab08f242a6180a7b99b4a0d9d84f9c63b3d2e014081d0a1e11a7
slot 3  c885453381820ca62010bfde0cd5139d686ab11431d543ab36027f254d8ce272
```

stale-region 使用不同的 current Bootstrap/data，因此 set digest 为：

```text
bab7226cc3d954f97c3585aac160db1ee6bdfa9d1b40348de4f7ae7644f94f4a
```

其四个 block digest 为：

```text
slot 0  54d645b9630e7a05666649a7f8142f977d0c28897aa3acf5375561750bd873db
slot 1  e66fb10238877d3f3ca7ddf3571643729d59bfcd86a5132d835d9b7af6bf5b31
slot 2  9c6857319f72c1eb67218162b5efc72b7b7214b6eba411254469d06580c87848
slot 3  55841bb3ab2c4c431f9c68f580ddda1ae050e149dc81cd0149ca435ec0b1c123
```

每条 result 同时记录 CPU set digest 与 GPU set digest；evidence validator要求二者大小写敏感完全相同，并按 scenario要求所有 adapter 的 digest只有一个唯一值。任何差异使 Gate 非零退出，不会改用 raw-float容差、WARP替代、CPU demod结果或部分 accepted set继续通过。

## 6. Wrong-LUID 与 device recreate

每次创建 demodulator 后，Gate先上传同一 fixture texture，再只改变 metadata中的 adapter LUID最低位：

- `SubmitRemoteVisualLowFps` 必须返回 `AdapterMismatch` / `Submission`；
- caller的非默认 `DemodSubmission` sentinel必须逐字段保持不变；
- snapshot `submittedFrames` 必须仍为 0；
- 只有该 negative通过后才运行 corpus。

generation 0 完成四场景后必须满足 submitted=completed=4、pending/failed/cancelled=0、raw pixel readback=0、device removed reason=`S_OK`、shutdown完成。随后销毁 device/context/demodulator并针对同一 adapter重新创建 generation 1；recreate exact run 必须再次通过 wrong-LUID negative，且 accepted set与 generation 0/WARP baseline相同。

因此总计：

- 5 个 backend × 2 次 wrong-LUID negative = 10 次明确拒绝；
- 5 个 backend ×（4 个 generation-0场景 + 1 个 recreate场景）= 25 个成功 GPU frame；
- 25 × 4 = 100 个 accepted Transport manifest entries；
- 0 failed、0 cancelled、0 device removal、0 raw-pixel readback。

## 7. Metric/readback/timestamp evidence

每条 result 都记录：

- 64,800 metric samples；
- zero/minimum/mean absolute metric；
- unreliable/freshness/stale/mismatch/erasure counters；
- 261,040 compact readback bytes；
- `gpuTimingValid` 与 disjoint-query换算后的 `gpuTime100ns`。

本轮 25/25 GPU timestamps 全部有效且非零。按 backend合并 generation 0 + recreate exact 后的观察范围为：

| backend identity | `gpuTime100ns` min..max | 约合 ms | 说明 |
| --- | ---: | ---: | --- |
| WARP `0001895a` | 13,180..560,506 | 1.3180..56.0506 | software rasterizer |
| NVIDIA `000172f1` | 863..5,696 | 0.0863..0.5696 | hardware |
| AMD `000189ad` | 21,221..43,133 | 2.1221..4.3133 | hardware |
| NVIDIA `00023612` | 788..4,947 | 0.0788..0.4947 | hardware |
| NVIDIA `0002223a` | 784..27,580 | 0.0784..2.7580 | hardware |

这些是单次 GPU query 的诊断时长，不是端到端 capture latency、5 Hz goodput或性能认证。Gate只要求 timestamp存在且资源/协议真值一致，不按某个厂商的时长设置 admission阈值。

## 8. Create-only evidence tool

`tools/PBRemoteVisualEvidence/Invoke-PBRemoteVisualGpuParityGate.ps1`：

1. 要求 evidence目录事先不存在；
2. 直接运行隐藏 Catch2 case并先封存完整 `gate.log`；
3. 只接受四个冻结 JSON schema：AdapterAvailability、Result、Run、Summary；
4. 独立检查 adapter/run/result数量、四场景集合、recreate集合、CPU/GPU digest、FEC/CRC/identity/padding、wrong-LUID、timestamp、readback、device removal与shutdown；
5. 记录测试 EXE和 validator脚本自身的 size/SHA-256、CMake source root、Git HEAD和工作树状态；DXGI raw driver version以十进制字符串写入，避免超过 JSON 53-bit安全整数后被消费者舍入；
6. create-only写出 `evidence.json` 与 `sha256s.txt`。

本轮结构化记录：

| schema | count |
| --- | ---: |
| AdapterAvailability.1 | 6（WARP generation 0/recreate各一条 + 4 hardware） |
| Result.1 | 25 |
| Run.1 | 10 |
| Summary.1 | 1 |
| 合计 | 42 |

对同一 evidence目录执行第二次 wrapper会在运行测试前立即拒绝。负测已验证拒绝后 `gate.log`、`evidence.json`、`sha256s.txt` 三个文件哈希均不变。

## 9. 构建与重放命令

Release构建及 CTest：

```powershell
cmake --build build-presentation-release --config Release --target PBDemodD3D11Tests -- /m:1

ctest --test-dir build-presentation-release -C Release `
  -R '^PBRemoteVisualGpuParityTests$' --output-on-failure
```

生成新的 create-only evidence：

```powershell
tools\PBRemoteVisualEvidence\Invoke-PBRemoteVisualGpuParityGate.ps1 `
  -BuildDirectory build-presentation-release `
  -Configuration Release `
  -EvidenceDirectory build-p1_5-evidence\<new-step11-directory>
```

原有默认回归仍单独运行：

```powershell
ctest --test-dir build-presentation-release -C Release `
  -R '^(PBDemodD3D11Tests|PBRemoteVisualTests)$' --output-on-failure
```

ASan专用 adapter matrix使用 `RelWithDebInfo`与当前 MSVC ASan runtime PATH；`ASAN_OPTIONS=halt_on_error=1:abort_on_error=1:detect_leaks=0`。最终同源矩阵1/1 PASS，121.16秒。Release默认 regression为2/2 PASS：`PBRemoteVisualTests` 1.21秒、`PBDemodD3D11Tests` 99.57秒。Release create-only Gate为1/1 PASS，本次观察为46,733 assertions、50.810秒；poll-loop assertion数量与实际 completion轮次有关，不作为协议接受条件。

## 10. 最终证据与 seal

create-only evidence目录：

```text
build-p1_5-evidence/20260902-011943-step11-gpu-parity-final
```

测试 executable：

```text
size    991744
SHA-256 7d11d1d75ac1d4cbda737b4b00553655634db0fe38db8f3124f7c765f3b8038e
```

artifact seals：

```text
0c56409d47f2cc4dd730f40f786dfc7e72fa24488581a839a63afefb8aa23fed  gate.log
168bbcf20b59eb85ab4f52fc2c1b054fd9a9f66ac5e6bbeb56bc634870d0aadc  evidence.json
```

validator脚本为13,389 bytes，SHA-256=`5bf923f248310097c44873688c248214c5eb3f6693b28b8eb44bc2f9112804ac`。`sha256s.txt` 已由独立命令重新计算并确认两项均匹配。证据采集时 source HEAD为 `bb24da82100beede06348fe6005ecb2e7be79e28`，`evidence.json`同时记录工作树未提交状态、exact test executable hash和 validator hash；最终停止任务并提交前仍需按用户要求执行最终回归和创建提交，不能把当前 evidence误称为 clean-commit artifact。

## 11. Step 11 完成出口与后续边界

完成出口逐项闭合：

- WARP + NVIDIA + AMD当前可用 adapter：PASS；4/4 hardware candidates全部真实运行，没有替代或跳过；
- canonical Bootstrap/FrameSequence：PASS；每条 result有 Bootstrap BLAKE3、SessionTag、FrameSequence和 geometry；
- FEC/CRC/identity/padding：PASS；全部 adapter逐字段等于 CPU reference；
- accepted Transport set：PASS；所有 scenario跨 adapter只有一个唯一 set digest，100个 block manifest entries完整；
- wrong adapter LUID：PASS；10/10在提交前拒绝且不修改输出/计数；
- device recreate：PASS；5/5新 device/context/demodulator exact结果等于 generation 0；
- compact resource/timing：PASS；25/25 timestamp有效，261,040 bytes/frame，raw pixel readback=0；
- create-only evidence：PASS；42条记录独立校验并封存，overwrite负测不改变artifact。

因此 Step 11 状态为 `DONE`。Step 12 是当前下一步：必须把 locator/Bootstrap/geometry与同一 WGC/DXGI observation绑定，把 PB-owned texture lease保留到 event completion，并在 ContentSize/mode/monitor/device/CaptureEpoch变化时 drain stale work或终止。Step 11没有提前把 experimental API接入 product Decoder，也没有提升 Step 12/14/17/20/27 的状态。
