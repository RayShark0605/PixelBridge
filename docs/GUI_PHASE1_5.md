# PixelBridge Windows GUI — Phase 1.5

## 1. 交付边界

`PixelBridgeEncoder.exe` 与 `PixelBridgeDecoder.exe` 无参数启动 Qt Widgets GUI。两者是薄
UI/controller，不是测试面板，也没有复制协议、FEC、raster、capture、demod、Receiver 或
Storage 实现。Qt 只存在于 `apps/PixelBridgeEncoder`、`apps/PixelBridgeDecoder`；
`apps/common/PBApplication` 与所有 `libs/` target 都不依赖 Qt。

当前 GUI 只公开已经有真实 public API binding 的 Phase 1.5 能力：

- Instant LocalDesktop；
- 单 Session、单 Segment、源文件 1 byte..8 MiB；
- 固定 1920×1080 physical-pixel Data Window/ROI；
- Direct-Level 2x2 与 Shape+Chroma，均标记为 Experimental；
- RAW，或现有 zstd/RAW fallback；
- 自动 `ChooseOuterFecMode` 后使用 DirectRepeat 或 Wirehair V2；
- 固定 robust DVB-S2 Short QC-LDPC；
- 显式 WGC 或 DXGI Desktop Duplication，不提供 Auto/fallback；
- PBStorage `.part`、WholeFileDigest 与 same-directory final publish。

未公开 Offline MP4、Direct-Level 4x4 文件路径、多 Segment、任意缩放、自动 capture fallback、
profile 自动探测、Receiver-to-Sender feedback 和 resume。完整逐项清单见
[`CURRENT_RUNTIME_OPTION_INVENTORY.md`](CURRENT_RUNTIME_OPTION_INVENTORY.md)。

## 2. 唯一正确的运行语义

```text
Encoder broadcasts; Decoder converges.
```

Encoder 状态为 `Idle -> Preparing -> Broadcasting -> Stopping -> Stopped`，失败进入
`Failed`。`Broadcasting` 没有自然 Completed；Carousel 到末尾只会增加 `CycleCount` 并从
下一个周期继续。Encoder 只显示自己可证明的广播运行时间、当前周期位置、Segment/
OuterBlock、FrameSequence、真实 Present 指标和 generated rate。周期进度条明确标注为
“当前轮播周期位置”，不是文件恢复进度。用户主动 Stop 后才停止 Data Window。

Decoder 状态为 `Idle -> WaitingForBootstrap -> ReceivingControl -> Receiving -> Recovering ->
Verifying -> Publishing -> Completed`，并支持显式 `Stopping/Stopped/Failed`。Completed 的
唯一接受条件是：

1. Receiver 已验证 EncodedDigest、bounded decompression 与 RawDigest；
2. 已验证 raw Segment 写入 PBStorage `.part`；
3. Receiver `PrepareFinalization` 返回 authoritative FinalManifest；
4. PBStorage 验证 WholeFileDigest；
5. same-directory final publish 成功且 final artifact 再次通过摘要验证。

即使 Decoder 已 Completed，Encoder 也可以并应继续广播，直到 Encoder 用户主动 Stop。

## 3. Encoder 控件与真实 binding

| 控件/显示 | 真实 binding/定义 |
| --- | --- |
| 选择文件 | worker 用只读 Win32 handle 读取；Session 期间保持 handle，并检查 volume/file identity、大小和 last-write time；不截断 |
| 启用 Segment 压缩 | off 为 byte-identical RAW；on 调用 `pbcompression::CompressSegment`，无收益时只使用已有 RAW fallback |
| Compression level | `CompressionSettings::compressionLevel`，范围 1..22；仅在 compression on 时生效 |
| Visual Profile | `kDesktopLevels2ProfileId` / `kShapeChromaProfileId` 以及对应 production raster API |
| Outer FEC | 只读 automatic；`ChooseOuterFecMode` 后构造 DirectRepeat 或 Wirehair V2，不能人工拼非法组合 |
| Inner FEC | 只读 robust QC-LDPC；不能 override |
| 目标 monitor | PMv2 Win32 monitor inventory；固定 canvas 可放入 work area 时居中，否则在物理 monitor 内居中，再传入 `DataWindowConfig::clientOrigin`；monitor 小于固定 canvas 时 Start 禁用 |
| 开始/停止广播 | `EncoderRuntime` 的单 bounded worker；重复 Start 拒绝，Stop 幂等 |
| Cycle Position | `cyclePosition / cycleFrameCount`，到末尾回零并增加 CycleCount；绝不映射到接收进度 |
| Generated Payload Rate | Sender 已生成 outer payload bytes / 广播运行时间；不是 VerifiedGoodput |
| PresentedVisualFPS/PresentCallFPS | 原有 `PBRenderD3D` timing snapshot；不可用时显示 `—`，不从提交数推断 |

源文件扩展名只用于提示。`.7z/.rar/.zip/.gz/.zst` 以及多数视频/图片不会自动改动用户的
compression checkbox。

## 4. Decoder 控件与真实 binding

| 控件/显示 | 真实 binding/定义 |
| --- | --- |
| 输出目录 | 传给 PBStorage；UI 不使用 `QFile` 拼 payload，不 rename final |
| WGC/DXGI | 显式构造相应 production backend；Requested/Actual/Reason 可见；失败不 silent fallback |
| 选择/重新选择/清除 ROI | 原生 `PBScreenRegion` selector；返回 physical-pixel RECT |
| 使用整个显示器 | 对已知 monitor 的 `monitorPhysicalRect` 再调用 `ResolveScreenCaptureRegion`；若非固定 1920×1080 仍 fail closed |
| Visual Profile | 绑定 `CaptureDemodulatorConfig::visualProfileId`；必须与 Encoder 一致，因为当前 provisional wire 不传 profile |
| 文件恢复进度 | `verifiedRawBytes / OriginalFileSize`；Descriptor 未知时 indeterminate；当前单 Segment 路径只在完整 Segment 验证后从 0 跳到 100% |
| VerifiedEncodedGoodput | 真实 `PBTelemetry::RecordVerifiedEncodedBytes`；只有 Receiver 的 encoded/raw digest、PBStorage write、FinalManifest/WholeFileDigest 与 final publish 全部通过后才记录，单位 bit/s |
| Verified raw recovery rate | verified raw-byte mutation 的 EMA：首个 positive sample 取 instant，后续为 `0.25 * instant + 0.75 * previous`；这是 ETA basis，不冒充 encoded channel goodput |
| ETA | `remainingRawBytes / smoothedVerifiedRawGoodput`；少于两个 positive samples、speed 为 0、长 stall、Descriptor 未知或 CaptureEpoch reset 时不可用 |
| 完成 | 只有 WholeFileDigest PASS 与 final publish success 同时为 true 才使用绿色 Completed |

`VerifiedEncodedGoodput` 与 verified raw recovery rate 是两个不同分母。压缩启用时二者可以不同；
前者是 digest/publish-gated encoded bytes，后者是 raw output mutation，不能互换。当前单 Segment
产品边界通常在完成前没有足够的 positive raw samples，因此 ETA 保持“计算中/等待有效数据”
比伪造逐帧 ETA 更正确。

Advanced 使用真实 `PB::PBTelemetry`：每个由 D3D11 demod 完成的 bounded result 记录 Capture 与
Bootstrap；Bootstrap erasure 通过同一个固定容量 result ring 以 `TelemetryOnly` 事件上报，不携带
Transport payload。FEC 只对 CaptureEpoch 内去重后、已接纳的 FrameSequence 记录。生产 demod
没有 sender truth 时以 `comparedCodedBits=0, erroneousCodedBits=0` 明确撤回 PreFecBER coverage，
但保留真实 FER/CRC/FEC 计数；任何部分或矛盾 bit denominator 仍被拒绝。

当前生产 D3D11 fast path 明确保持 `rawPixelReadbackBytes=0`，因此没有 collision-resistant ROI
pixel digest：`fingerprintedFrames=0`，`EndToEndUniqueVisualFPS=null`。UI 另行显示
`admittedFrameSequenceFps` 以及 duplicate/reordered/gap/skipped FrameSequence 诊断，并明确标注它们
**不是** `UniqueVisualFPS`；绝不以 FrameSequence、timestamp 或 sender cadence 猜测像素唯一性。

CaptureEpoch 变化会先调用 Receiver 的 authoritative reset，随后销毁未发布 `.part` 和所有
本地 descriptor/progress binding，再等待同一广播 Carousel 的描述符重新建立状态。旧 epoch
结果不能进入新 epoch；未认证进度不会被保留为可信进度。

## 5. 线程、生命周期与关闭

- Qt GUI thread 只做 input validation、snapshot presentation、报告/偏好 I/O；大文件、压缩、
  digest、FEC、D3D、capture、demod 与 payload storage 均在 application worker/现有 runtime。
- GUI 每 100 ms 读取 mutex-protected immutable snapshot copy；worker 不访问 QWidget。
- UI/controller 不持有 DataWindow、WGC、DXGI、D3D11 或 FEC third-party resource。
- 每次 run 有单调 `runGeneration`；所有 snapshot mutation 先比对 generation，旧 callback 不更新新 run。
- Start 两次被拒绝；Stop 两次幂等；Preparing 时 Stop 不进入 Broadcasting。
- 活跃窗口第一次 close 会请求 Stop 并 defer；重复 close 不重复发出 shutdown；terminal snapshot 后
  才真正关闭。controller 析构前停止 polling，并 join 已完成 drain 的 worker。
- Decoder capture Stop 后要求所有 frame lease、ROI texture、demod pending/result queue 都已退休；
  cleanup 不完整会 fail closed。

## 6. PBStorage

`PBStorage` 是 Qt-free Windows core target：

- `.part` 使用 `CREATE_NEW`；final 在 reservation 和 publish 前都必须不存在，且同目录 rename 不使用 replace-existing 标志，现有目标不会被覆盖；
- 在任何 payload write 前按 `maximumFileBytes` 验证并有界预分配；
- 当前接口只接受 contiguous、non-overlapping raw writes；
- 每次 write 后 flush；
- publish 前关闭 writer、重新读取 `.part` 并验证 BLAKE3 WholeFileDigest；
- 使用 `MoveFileExW(..., MOVEFILE_WRITE_THROUGH)` 做同目录 publish；
- final 再次验证失败会尝试删除本实例刚创建的 final；即使清理本身失败也会显式报错，绝不报告 Completed；
- 未发布对象析构时只清理自己创建的 `.part`，不会删除已发布 final。

当前 provisional wire 不携带原始文件名，因此 final 名称为
`PixelBridge-<16-hex SessionTag>.bin`。

## 7. QSettings 与运行报告

QSettings 只保存：window geometry、last input/output path、compression checkbox、显式 backend、
上次选择的 monitor device name 和 Advanced 展开状态。monitor 每次仍重新枚举并验证，不保存物理坐标。
它不保存 SessionId、Wirehair 私有状态、resume.state、FEC internal state
或 protocol state；profile 也不会作为隐藏的自动 wire override 恢复。

两端都能导出 `PixelBridge.RunReport.1` JSON 和复制不含 payload 的诊断文本。报告记录版本、
git commit、RunId、起止 Unix 时间、SessionTag、profile/FEC/compression、Data Window/ROI geometry、
backend、broadcast runtime/cycle 或 verified progress/goodput/ETA、FPS、FEC/CRC exact counters、
CaptureEpoch、queue/high-water、GPU/CPU timing、WholeFileDigest/publish 和 RemoteVisual metadata。
任何非有限浮点 metadata 写为 JSON `null`。Encoder 报告显式写出
`receiverProgress:null`、`receiverEta:null`、`verifiedGoodput:null`。

Decoder 报告分别写出 `verifiedEncodedGoodputBitsPerSecond`（WholeFileDigest + final publish gate）、
`*VerifiedRawGoodputBytesPerSecond`（ETA basis）、真实 PBTelemetry `captureFps`/
`bootstrapSuccessRate`/FER，以及 `uniqueVisualFpsBasis` 与 `fingerprintedFrames`。生产快路径没有 pixel
identity 时，`uniqueVisualFps` 必须为 JSON `null`；`admittedFrameSequenceFps` 使用独立字段名。

## 8. 构建与启动

Qt 6.10.1：

```powershell
cmake -S . -B build-gui-qt6 -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DPB_QT_ROOT=D:/Qt6.10.1/6.10.1/msvc2022_64 `
  -DPB_BUILD_APPS=ON -DPB_BUILD_TESTS=ON -DBUILD_TESTING=ON
cmake --build build-gui-qt6 --config Release --parallel
ctest --test-dir build-gui-qt6 -C Release --output-on-failure
```

Qt 5.14.2 可将 `PB_QT_ROOT` 改为 `D:/Qt5.14.2/5.14.2/msvc2017_64`。两种配置都会在
app/test 构建后调用对应 `windeployqt`；构建目录包含可直接运行的 Qt DLL/plugins。

```powershell
.\build-gui-qt6\apps\PixelBridgeEncoder\Release\PixelBridgeEncoder.exe
.\build-gui-qt6\apps\PixelBridgeDecoder\Release\PixelBridgeDecoder.exe
```

## 9. 正式 GUI LocalDesktop smoke

`InvokeEncoderGuiLocalDesktopSmoke.ps1` 启动正式 Encoder GUI，通过 Windows UI Automation 读取并
调用正式控件，不移动鼠标、不发送全局键盘输入。脚本临时写入并在 finally 恢复 QSettings
UI 偏好；monitor 仍由当前 Win32 catalog 重新枚举。只有当 GUI 窗口、monitor 详情和 D3D11
Data Window client rect 全部位于指定屏幕时才启动 Decoder。当前 GUI smoke 固定覆盖
Direct-Level；Shape+Chroma 由不经 UI 自动设置的完整 application matrix 另行覆盖。

以当前右屏 `\\.\DISPLAY2` 为例（物理屏 `[2560,0,5120,1440]`，work-area 居中
1920×1080 原点为 `[2880,156]`）：

```powershell
$stamp = Get-Date -Format yyyyMMdd-HHmmss
pwsh -NoProfile -File tests\PBApplication\InvokeEncoderGuiLocalDesktopSmoke.ps1 `
  -BuildRoot build-gui-qt6 `
  -EvidenceRoot "build-gui-qt6\evidence\encoder-gui-right-$stamp" `
  -Source "build-gui-qt6\evidence\application-matrix\sources\random-1MiB.bin" `
  -MonitorDeviceName '\\.\DISPLAY2' `
  -MonitorLeft 2560 -MonitorTop 0 -MonitorRight 5120 -MonitorBottom 1440 `
  -MonitorOriginX 2880 -MonitorOriginY 156 `
  -Backend wgc -Profile direct -Compression off `
  -PythonExecutable D:\Python3.12.9\python.exe
```

成功条件包括：Decoder `Completed` + WholeFileDigest + final publish；Decoder 完成时 Encoder GUI
仍为 `Broadcasting`；随后通过 GUI Stop 到达 `Stopped`；源/输出长度、外部 SHA-256、独立
BLAKE3 以及 Encoder GUI/Decoder digest 全部一致；无 `.part`、无强制清理。

## 10. 认证边界

GUI 可运行和一次 LocalDesktop 文件摘要一致，不等于任意显示器、RemoteVisual、HDR、缩放、
色度采样或 Certified Profile 已认证。UI 会把不同于 Phase-1 1920×1080/60 reference 的显示
环境标为 Experimental。当前仓库的 CertifiedProfile/FinalSystemComplete 状态不因本 GUI 自动
改变；物理证据必须继续由相应 Gate、确切 backend/profile/显示环境与 source/output digest 支撑。
