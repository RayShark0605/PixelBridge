# PixelBridge

Windows x64 / C++20：通过可见桌面/视频像素进行的高性能单向文件传输。
规范设计文档：`docs/PixelBridge_最终技术路线与总体设计.md`；工程规则见 `AGENTS.md`。

## 目录

| 路径 | 用途 |
| --- | --- |
| `apps/PixelBridgeEncoder`、`apps/PixelBridgeDecoder`、`apps/common` | 正式 Qt Widgets GUI、Qt-free application runtime/state/report 层，以及保留的显式诊断 CLI |
| `libs/PBCore`、`libs/PBProtocol`、`libs/PBCompression`、`libs/PBOuterFec`、`libs/PBReceiver`、`libs/PBStorage` | 核心静态库（禁止依赖 Qt）；PBStorage 负责 `.part`、WholeFileDigest 与 final publish |
| `libs/PBPresentTiming`、`libs/PBRenderD3D` | 有界 DXGI observation 计时与独立原生 D3D11 数据窗口；不依赖 Qt |
| `libs/PBScreenRegion`、`libs/PBScreenCaptureWgc` | 物理像素选区、Windows Graphics Capture 与 GPU 退休保护的 ROI texture ring；不依赖 Qt |
| `tools`、`fuzz`、`benchmarks` | 独立可选子图；protocol/compression/Outer FEC fuzz 与 protocol/Outer FEC benchmark 均有真实 target |
| `tests` | Catch2 v3 单元测试（CTest） |
| `docs` | 设计文档 |

## Phase-0 协议状态

当前 37/110/142/65-byte Session/Segment/Final payload 是 **Phase-0 provisional implementation slice**，不是完整正式 v1 wire 承诺。它仍缺少总体设计要求的 `SessionVisualProfileId` 等固定 Session 绑定，因而不得在后续 Data Plane/backend 接入时被误称为已冻结的正式 canonical v1 schema。当前状态、资源预算与升级前置条件见 [`docs/PHASE0_PROTOCOL_STATUS.md`](docs/PHASE0_PROTOCOL_STATUS.md)。

`PBProtocol` 已实现与 Data Profile 解耦的逻辑字节 envelope：固定 44-byte
`PB-Bootstrap-1`，以及 `26-byte prefix + payload + 4-byte CRC` 的
`PB-Control-1`。Control 的 `RecordBytes` 包含整条 record，最大 65,536 bytes；
低层 parser 零拷贝返回借用 payload，但生产接收入口统一使用
`ControlPlaneReceiver`，在状态变更前完成 record-type dispatch、Descriptor、资源策略、
SessionTag cross-check 和 immutable binding。`PB-Control-Fragment-1` 已冻结为
`20-byte prefix + non-empty payload + 4-byte CRC`，并提供有界 PMR 重组、乱序、幂等重复、
冲突 tombstone、observation-window 过期和 ControlEpoch reset。该逻辑字节协议步骤为
**GO**；整体 Phase-0 仍为 **NO-GO**，当前不包含视觉 raster/FEC、物理 Control Block
容量/映射、重复 cadence、正式 profile binding 或完整文件恢复。

`pbreceiver::ReceiverIngress` 是当前唯一的逻辑接收 façade。它按值持有一份已验证的
`ReceiverResourcePolicy` 和当前 profile 的固定 `OuterBlockBytes`，并独占一个
`ControlPlaneReceiver`、一个 bounded orphan cache、一个 receiver-wide Outer FEC
manager 和 active decoder 表。未知 Segment 的逻辑 Data Block 只能进入 orphan cache
或以明确 quota 状态被拒绝；在 Segment capability 绑定前不会创建 DirectRepeat/Wirehair
decoder、segment-sized buffer、zstd context 或 output reservation。这里的
`ReceivedTransportBlock` 表示上游已完成 Inner-FEC/Transport-CRC 检查的内存内值，**不是**
正式 Transport wire schema。Phase 1.5 application runtime 已通过现有 Transport parser、
PBReceiver 与 PBStorage 接入 `.part` 创建、WholeFileDigest 和 final publish，并接通
WGC/DXGI -> CaptureNormalize -> D3D11 demod；这不把 provisional descriptor 提升为正式 v1 wire。

## Target 与依赖边界

- `PBCore`、`PBProtocol` 是显式静态库，不受父工程 `BUILD_SHARED_LIBS` 影响。
- `PBCompression` 是显式静态库；外部消费者只链接 `PB::PBCompression`
  即可获得 PBProtocol 与 zstd 的完整静态链接闭包。
- `PBOuterFec` 是显式静态库；外部消费者只链接 `PB::PBOuterFec` 即可获得
  PBProtocol 与固定 Wirehair 静态库的完整链接闭包。公共头不暴露 Wirehair
  原生头或 host-native profile struct。
- `PBReceiver` 是显式静态库；外部消费者只链接 `PB::PBReceiver` 即可获得
  PBProtocol、PBCompression 与 PBOuterFec 的完整静态链接闭包。生产 decoder factory
  只接受由 Control admission 签发的 `BoundSegmentDescriptor`；普通
  `SegmentDescriptor` 的创建 seam 仅位于未安装的 test/benchmark 私有头中。
- `PBProtocol` 不依赖 `PBCore`；消费者只获得所链接 target 的公共头和链接闭包。
- `PBPresentTiming` 与 Windows-only `PBRenderD3D` 是显式静态库，公共接口不暴露 Qt、HWND 或 DXGI 类型；独立链接及 no-Qt Gate 同样覆盖它们。
- `PB::CompilerSettings` 仅供 PixelBridge 自有 target 私有使用，`/WX` 等策略不传播给外部消费者。
- Qt 只允许由应用以 `PRIVATE` 方式链接；`libs/` 下的核心库和公共头禁止依赖 Qt。
- PBOuterFec/PBFEC、协议与 CPU reference 模块保持平台无关。
- WGC、DXGI、Capture Normalize、D3D11 Demod 和 CUDA Demod 分别建立 target，不把平台 backend 塞入公共核心库。
- CUDA 选项只与真实 CUDA target 同时引入，默认关闭；显式启用后缺失依赖必须配置失败，不允许静默 fallback。

CMake 在配置期审计核心 target 的 Qt 依赖、公共 `src/` 路径和公共编译选项泄漏。未来模块必须继续满足这些门禁。

## Windows Qt GUI（Phase 1.5）

Release 构建后的以下程序无参数启动正式 GUI：

```powershell
.\build\apps\PixelBridgeEncoder\Release\PixelBridgeEncoder.exe
.\build\apps\PixelBridgeDecoder\Release\PixelBridgeDecoder.exe
```

GUI 是薄 presentation/controller 层，真实传输链仍为现有 Session/Segment、
Compression、Outer/Inner FEC、D3D11 Data Window、WGC/DXGI、CaptureNormalize、
D3D11 demod、ReceiverIngress、PBTelemetry 和 PBStorage。Qt 不进入 `libs/`，也不合成数据像素或发布文件。

当前产品边界为 Instant LocalDesktop、单 Segment、1 byte..8 MiB、固定 1920×1080
physical-pixel ROI，以及 Direct-Level 2x2 / Shape+Chroma 两个明确标为 Experimental 的路径。
原文件名和 Visual Profile 尚未写入 provisional wire，因此 Decoder 需要人工选择同一 profile，
并安全发布为 `PixelBridge-<SessionTag>.bin`。完整 Runtime Option Inventory、控件绑定、
状态机、进度定义和限制见 [`docs/GUI_PHASE1_5.md`](docs/GUI_PHASE1_5.md) 与
[`docs/CURRENT_RUNTIME_OPTION_INVENTORY.md`](docs/CURRENT_RUNTIME_OPTION_INVENTORY.md)。

关键语义始终是 **Encoder broadcasts; Decoder converges**：Encoder 完成一轮 Carousel
后继续广播，直到用户点击“停止广播”；它没有接收端恢复百分比或完成 ETA。Decoder 的
进度严格为 `verifiedRawBytes / OriginalFileSize`，当前单 Segment 路径以已验证 Segment
粒度推进；只有 WholeFileDigest PASS 且 PBStorage final publish 成功才显示 Completed。

Decoder 主吞吐是 digest/publish-gated `VerifiedEncodedGoodput`；用于 ETA 的 verified raw EMA
单独命名。生产 D3D11 fast path 不做 raw-pixel readback/digest，因此 `UniqueVisualFPS` 严格显示
为不可用；FrameSequence cadence 与 duplicate/reordered/gap/skipped 只作为独立 admission 诊断，
不会冒充像素唯一帧率。

为可复现物理桌面 smoke 保留了调用同一 application runtime 的有界自动化入口；它们不建立
任何 payload IPC：

```powershell
PixelBridgeEncoder.exe --headless-broadcast --source input.bin --profile direct `
  --compression off --origin 0 0 --seconds 30 --report encoder.json
PixelBridgeDecoder.exe --headless-receive --output-dir output --backend wgc `
  --profile direct --roi 0 0 1920 1080 --timeout 60 --report decoder.json
```

`--version` 输出确定性版本横幅；旧 `--data-window`、`--select-region` 和 capture diagnostics
仍保持显式参数入口。`--gui-smoke` 仅用于 CTest 的窗口构造/事件循环/部署检查。

## 独立 D3D11 Data Window

`PixelBridgeEncoder --data-window --frames 120 --telemetry NEW_FILE.jsonl`
运行现有 BGRA reference raster 的呈现诊断，不是完整文件发送器。默认使用
flip-discard、双缓冲、frame-latency waitable object、MaximumFrameLatency 1、
`Present(1, 0)`、无 tearing/MSAA/alpha/filtering，以及 1920×1080 physical client。
窗口与 immediate context 由专用 owner thread 持有，Qt 不参与数据像素合成。

`--frames` 限制成功的 **CPU 帧提交**，不保证所有提交都到达显示器。遥测分别报告
`PresentCallFPS`、基于 DXGI/Present ID/FrameSequence 关联的 `PresentedVisualFPS`
及明确命名的观测下界；statistics 不可用或有关联缺口时，不拿调用数补齐视觉 FPS。
本功能是 **Certified candidate 基础设施**，不是 Capture round-trip 或 LocalDesktop 认证。

API、指标公式、异常恢复、真实显示/模式恢复 Gate 和限制见
[`docs/PRESENTATION.md`](docs/PRESENTATION.md)。默认测试不会弹出数据窗口或切换显示模式；
显式开启 `PB_BUILD_PRESENTATION_GATE=ON` 才运行真实显示 Gate，模式测试由独立监督进程恢复设置。

## 物理像素区域选择

`PixelBridgeDecoder --select-region` 启动覆盖虚拟桌面的原生拖选 overlay，只有整个 ROI
位于一个显示器内才接受。返回有符号 physical-pixel RECT、HMONITOR、effective DPI 和
DXGI rotation；跨屏／空隙选区不裁剪、不吸附，允许重选。Escape 或右键取消。
无参数 banner 保持不变；不依赖 Qt，也不把 logical coordinates 当作 WGC/DXGI 坐标。
这不是屏幕捕获或 LocalDesktop 认证。

接口及生命周期契约见 [`docs/SCREEN_REGION.md`](docs/SCREEN_REGION.md)。
默认模型测试不显示 overlay；`PB_BUILD_SCREEN_REGION_GATE=ON` 显式启用真实桌面测试，
会移动并恢复鼠标，不修改 DPI、分辨率、旋转或显示器布局。

## WGC 屏幕捕获与 GPU lease 退休

`PB::PBScreenCaptureWgc` 复用 `ScreenCaptureRegion`，使用 `CreateForMonitor` /
`CreateFreeThreaded` 捕获单显示器。callback 仅获取 frame lease、验证 metadata 并进入
有界队列；专用 D3D owner copy/crop 到自有 ROI ring，fence/event-query 确认 source 不再被
GPU 使用后才 Close frame，consumer 工作另有退休标记。积压丢旧帧，尺寸/环境变化先 drain
再 recreate 并递增 CaptureEpoch，错误/超时不提前归还 lease。

cursor、Borderless 和 MinUpdateInterval 按真实 interface/权限探测；不把优化 setter 成功
当作实际 FPS 或无边框保证。默认 BGRA，HDR 需显式使用 FP16；不静默做色调映射。
Phase 1.5 GUI application runtime 已将该 backend/normalization 边界接到现有 D3D11 demod、
ReceiverIngress 与 PBStorage；这仍不自动构成 Certified Profile 或任意硬件/RemoteVisual
性能认证。API、资源边界、验证命令与限制见
[`docs/PBScreenCaptureWgc.md`](docs/PBScreenCaptureWgc.md)。
真实桌面测试由 `PB_BUILD_WGC_GATE=ON` 显式启用；默认测试仅模型/COM mock/WARP，
不弹出捕获窗口或改动显示设置。

## Source Segment 压缩

`PBCompression` 将每个 Source Segment 独立编码为一个标准 zstd frame，
frame 带 32-bit checksum。压缩等级、encoder window 和线程策略只属于本地
Encoder tuning，不写入 PixelBridge wire descriptor，也不构成协议版本。若
`compressed bytes + framing margin >= raw bytes`，则该 Segment 使用 RAW；若
zstd frame 明确触及本地 encoded-segment budget，只有当 raw payload 本身仍在
该 budget 内时才允许 RAW fallback。其他 zstd、状态或 allocation 错误不会被
静默降级。

Decoder 同时执行三类本地边界：`EncodedSize/maxInputBytes`、
`RawSize/maxOutputBytes` 和 zstd frame window。streaming decoder 只暂存最多
18 bytes 的 frame header，已消费的 compressed input 不会累计保存；
`Finish()` 要求输入字节数严格等于 Descriptor `EncodedSize`、恰好完成一个
frame（包括 final block/checksum），且输出严格等于 Descriptor `RawSize`。

以 `SegmentDescriptor` 调用 canonical `DecompressSegment()` 前，调用方必须先用
同一份 `ReceiverResourcePolicy` 执行
`ValidateSegmentDescriptor(descriptor, sessionDescriptor, resourcePolicy)`；随后用
`MakeDecompressionLimits(resourcePolicy)` 构造本地解压边界。该
前置条件保证任何 zstd context 或输出 allocation 创建前，Descriptor 的 raw 和
encoded 配额已经验证。

## Outer FEC / DirectRepeat / Wirehair V2

`ChooseOuterFecMode()` 在 Wirehair 硬维度之外应用显式效率 gate：Phase-0
保守默认值令压缩后的 `K = ceil(EncodedSize / OuterBlockBytes)` 为 `0..2` 时选择
`DirectRepeat`，`3..64000` 选择 `WirehairV2`。Certified Profile 可以用 benchmark
结果显式传入另一个已冻结的 `OuterFecModeSelectionPolicy` 阈值，但必须在 Descriptor 冻结前
决定，禁止 Wirehair 创建失败后 silent fallback。大于 `64000` 仍要求上层拆分
Segment 或调整 block size。0-byte 的 block count 为 0；空文件路径不创建
`SegmentDescriptor`，也不发送 Data Block。

DirectRepeat 的 `OuterBlockId` 是从 0 开始的 ordinal。每个 block 携带真实
`PayloadBytes`，固定 `OuterBlockBytes` payload 区的 short tail 必须使用 canonical
zero padding。Decoder 支持乱序和相同 block 的幂等重复；同一 ordinal 的不同有效
payload 返回终止性的 `OuterBlockConflict`。仅当全部 ordinal 收齐且重组结果通过
BLAKE3 `EncodedDigest` 后，`Recover()` 才会返回 exact Encoded Segment bytes。
当前 provisional Transport 的 `PayloadBytes` 为 `uint16`，所以所有入口都强制
`1 <= OuterBlockBytes <= 65535`，custom receiver policy 也不能放宽这一 wire 上限。
`DirectRepeatDecoder::Create()` 还必须接收 Control admission 签发的
`BoundSegmentDescriptor` 和当前固定 Visual Profile 派生的 expected
`OuterBlockBytes`，并在任何 reservation/allocation 前与 Descriptor 精确比对；
`WirehairV2Decoder::Create()` 同样要求该 capability 和 expected block size。

`PBOuterFec` 只使用 Wirehair V2 canonical serialized-profile API。初次创建默认
显式选择 `WIREHAIR_V2_PROFILE_CERTIFIED_2026_07`（不使用 `CURRENT`），保存上游
原样返回的 32-byte descriptor；`OuterBlockId` 原值就是 Wirehair `blockId`，其中
`0..K-1` 为 systematic，`K..` 为 repair。

`WirehairV2Encoder::Recreate()` 先验证 `EncodedSize`、`OuterBlockBytes`、
`2 <= K <= 64000` 与 BLAKE3 `EncodedDigest`，随后直接使用 saved descriptor 与
exact Encoded Segment bytes 调用 `wirehair_v2_encoder_create_profile()`；Carousel
路径不会调用 profile-ID selector，也不会用新 profile、seed 或 attempt 替换保存状态。
saved descriptor 是否为该 Segment 首次绑定的 exact 32 bytes，必须由上层 descriptor
conflict state 保证，不能靠 profile ID 或重新选择来“认证”。

Decoder 将 `NeedMore` 作为正常增量状态；`ExtraInsufficient`、OOM、unsupported、
bad seed、invalid input 与未知 codec 结果均显式 fail closed。accepted block-ID
冲突检测使用 BLAKE3-128 fingerprint、独立的 per-decoder OS-CSPRNG hash salt 和
固定 64-probe 上限；该 wrapper table 不复制 Wirehair 的私有 bucket placement，
两者是独立防线。实现不假设一个 decoder 能无限接收新的 repair IDs；
`ExtraInsufficient` 后必须销毁该实例并由上层用完整新 repair window 重建。
Wirehair backend 的 `recover` 成功不是发布条件：wrapper 还会对 exact recovered
bytes 验证 descriptor 的 BLAKE3 `EncodedDigest`，不匹配时终止 decoder。
`ReceiverIngress::DecompressSegment()` 在解压前再次验证 encoded digest，并在返回
raw bytes 前验证 `RawDigest`。

`DirectRepeatDecoder::Create()` 与 `WirehairV2Decoder::Create()` 必须接收同一个
receiver-wide `OuterFecDecoderResourceManager`；旧名称
`WirehairV2DecoderResourceManager` 是源码兼容别名。manager 在任何 DirectRepeat
encoded buffer/bitmap、Wirehair wrapper 大分配或 codec 创建前，原子执行
active-decoder、per-decoder admission charge 与 aggregate charge 三重配额，并以
RAII 在创建失败、move、析构和并发 shutdown 路径精确回收。默认
`ReceiverResourcePolicy` 为最多 64 个 DirectRepeat ordinal、4 个 active Outer FEC
decoder、每个 512 MiB charge、aggregate 1 GiB；这些是本地保守 admission 值，
不进入 wire，也不是精确 RSS 计量。policy 是非 aggregate 类型，调用方应从
`GetDefaultReceiverResourcePolicy()` 开始按字段收紧，避免新增 quota 被旧 positional
initializer 静默置零。
同一 receiver 必须只建立并共享一个 manager；`Create()`/计数查询可并发调用，但
manager 的 move/析构必须在停止新 admission 后由 owner 排序，已存在 decoder 的并发
析构仍由共享 reservation state 安全回收。

Profile ID 只选择方程兼容性，不认证发送者。canonical descriptor、CRC、
`EncodedDigest` 以及任何 in-band whole-file digest 也不能单独提供发送者认证；
recovered bytes 仍必须经过设计书要求的 Segment、解压与 whole-file 验证流程。

## 构建（MSVC x64）

依赖：Visual Studio 2022（C++ 工作负载）、CMake >= 3.24、vcpkg（本机 `D:\vcpkg`），
以及 Qt Widgets。当前验证 Qt 6.10.1 与 Qt 5.14.2；正式构建通过 `PB_QT_ROOT` 指向
包含 `lib/cmake/Qt6` 或 `lib/cmake/Qt5` 的 Qt host prefix。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DPB_QT_ROOT=D:/Qt6.10.1/6.10.1/msvc2022_64
cmake --build build --config Release --parallel
ctest --test-dir build --build-config Release --output-on-failure
```

默认顶层构建启用应用和 PixelBridge 测试。Catch2 位于 vcpkg manifest 的非默认 `tests` feature；仅当 PixelBridge 是顶层工程且 `BUILD_TESTING=ON`、`PB_BUILD_TESTS=ON` 时，CMake 才会在加载 vcpkg toolchain 前启用该 feature。

### Production / Core-only

只构建核心库，不配置应用、工具、fuzz、benchmark，也不安装 Catch2：

```powershell
cmake -S . -B build-core -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DBUILD_TESTING=OFF `
  -DPB_BUILD_TESTS=OFF `
  -DPB_BUILD_APPS=OFF
cmake --build build-core --config Release --parallel
```

### Tests-only

```powershell
cmake -S . -B build-tests -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DPB_BUILD_APPS=OFF `
  -DBUILD_TESTING=ON `
  -DPB_BUILD_TESTS=ON
cmake --build build-tests --config Release --parallel
ctest --test-dir build-tests --build-config Release --output-on-failure
```

### 作为子工程使用

通过 `add_subdirectory()` 引入时，`PB_BUILD_APPS` 和 `PB_BUILD_TESTS` 默认均为 `OFF`。父工程可以保持自己的 `BUILD_TESTING=ON`，PixelBridge 不会因此查找 Catch2 或创建自身测试 target。若父工程显式启用 `PB_BUILD_TESTS=ON`，还必须启用全局 `BUILD_TESTING`、在顶层建立 CTest 测试树，并提供可发现的 Catch2 v3。

### 可选子图

下列选项默认关闭。fuzz 与 benchmark 选项会创建下表列出的真实可执行 target；`tools/` 仍不会创建假 target：

| 选项 | 默认值 | 子图 |
| --- | --- | --- |
| `PB_BUILD_APPS` | 顶层 `ON`，作为子工程时 `OFF` | `apps/` |
| `PB_QT_ROOT` | 空（由 CMake 常规搜索）；构建 Windows apps 时必须可发现 Qt | Qt 5/6 host prefix |
| `PB_BUILD_TOOLS` | `OFF` | `tools/` |
| `PB_BUILD_FUZZERS` | `OFF` | `fuzz/`：`PBProtocolDescriptorResourceFuzz`、`PBProtocolBootstrapControlFuzz`、`PBProtocolBootstrapControlStructuredSelfTest`、`PBProtocolOrphanResourceFuzz`、`PBCompressionZstdBoundaryFuzz`、`PBOuterFecWirehairV2Fuzz`、`PBOuterFecDirectRepeatFuzz` |
| `PB_BUILD_BENCHMARKS` | `OFF` | `benchmarks/`：`PBProtocolDescriptorStateBenchmark`、`PBOuterFecWirehairV2Benchmark`、`PBOuterFecDirectRepeatBenchmark` |
| `PB_BUILD_PRESENTATION_GATE` | `OFF` | Windows-only：真实 GPU/HWND/双屏/受监督模式切换；要求两个 tests 开关均开启 |
| `PB_BUILD_SCREEN_REGION_GATE` | `OFF` | Windows-only：真实 overlay／物理鼠标拖选／Decoder；恢复鼠标，不修改显示设置；要求两个 tests 开关均开启 |
| `BUILD_TESTING` | 顶层 `ON`，子工程由父工程管理 | 全局 CTest 开关 |
| `PB_BUILD_TESTS` | 顶层 `ON`，作为子工程时 `OFF` | PixelBridge 的 `tests/`；顶层同时控制 vcpkg `tests` feature |

fuzz 与 benchmark 共用核心库，但 fuzz 构建会对 `PBProtocol`、
`PBCompression` 和 `PBOuterFec` 静态库本身启用 AddressSanitizer，而不是只插桩 driver，
因此两者必须使用不同 build directory。Clang target 使用 libFuzzer +
ASan/UBSan；MSVC target 使用确定性 mutation runner + ASan。fuzz 配置与运行示例：

```powershell
cmake -S . -B build-fuzz-msvc -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DBUILD_TESTING=ON `
  -DPB_BUILD_TESTS=OFF `
  -DPB_BUILD_APPS=OFF `
  -DPB_BUILD_TOOLS=OFF `
  -DPB_BUILD_FUZZERS=ON `
  -DPB_BUILD_BENCHMARKS=OFF
cmake --build build-fuzz-msvc --config RelWithDebInfo --parallel
ctest --test-dir build-fuzz-msvc --build-config RelWithDebInfo `
  --output-on-failure -L fuzz

# MSVC deterministic mutation runners can also be invoked directly.
.\build-fuzz-msvc\fuzz\RelWithDebInfo\PBProtocolDescriptorResourceFuzz.exe `
  2000 13464654573299691533
.\build-fuzz-msvc\fuzz\RelWithDebInfo\PBProtocolBootstrapControlFuzz.exe `
  2000 5783258900934164481
.\build-fuzz-msvc\fuzz\RelWithDebInfo\PBProtocolBootstrapControlStructuredSelfTest.exe `
  --self-test
.\build-fuzz-msvc\fuzz\RelWithDebInfo\PBProtocolOrphanResourceFuzz.exe `
  2000 7263948150273648113
.\build-fuzz-msvc\fuzz\RelWithDebInfo\PBCompressionZstdBoundaryFuzz.exe `
  2000 13856851484949778996
.\build-fuzz-msvc\fuzz\RelWithDebInfo\PBOuterFecWirehairV2Fuzz.exe `
  1000 6289371488644456784
.\build-fuzz-msvc\fuzz\RelWithDebInfo\PBOuterFecDirectRepeatFuzz.exe `
  2000 4923072552113298010

# Replay one pinned PBCompression corpus input.
.\build-fuzz-msvc\fuzz\RelWithDebInfo\PBCompressionZstdBoundaryFuzz.exe `
  --input .\fuzz\corpus\compression-zstd\wide-window.bin

# Replay the canonical PB-Bootstrap-1 corpus input.
.\build-fuzz-msvc\fuzz\RelWithDebInfo\PBProtocolBootstrapControlFuzz.exe `
  --input .\fuzz\corpus\bootstrap-control\valid-bootstrap.bin
```

benchmark 配置示例：

```powershell
cmake -S . -B build-bench -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DBUILD_TESTING=OFF `
  -DPB_BUILD_TESTS=OFF `
  -DPB_BUILD_APPS=OFF `
  -DPB_BUILD_TOOLS=ON `
  -DPB_BUILD_FUZZERS=OFF `
  -DPB_BUILD_BENCHMARKS=ON
cmake --build build-bench --config Release --parallel
.\build-bench\benchmarks\Release\PBOuterFecWirehairV2Benchmark.exe 5
.\build-bench\benchmarks\Release\PBOuterFecDirectRepeatBenchmark.exe 100
```

若在同一个 build directory 中同时启用两个选项，CMake 会以
`mutually exclusive` 诊断拒绝配置。core-only 依赖隔离由标准 CTest
fixture 独立验证；`tools/` 仍只是扩展入口，不创建假 target。

输出 target 依赖图：

```powershell
cmake -S . -B build --graphviz=target-dependency-graph.dot
Move-Item target-dependency-graph.dot build\ -Force
```

Ninja 替代（单配置）：

```powershell
cmake -S . -B build-ninja -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake
```

## 告警策略

所有 PixelBridge 自有目标私有使用 `/utf-8 /W4 /permissive- /EHsc /Zc:__cplusplus /Zc:preprocessor`，默认附加 `/WX`（可用 `-DPB_TREAT_WARNINGS_AS_ERRORS=OFF` 关闭）。这些选项不属于库的公共消费接口。
自有头文件使用引号包含，第三方头文件（如 Catch2）使用尖括号包含，并通过 `/external:anglebrackets /external:W0` 豁免第三方头文件的告警。

## 第三方依赖

依赖由 vcpkg manifest `vcpkg.json` 管理。BLAKE3 1.8.5 是 `PBProtocol` 的
生产依赖；zstd 1.5.7 是 `PBCompression` 的生产依赖；Wirehair 2.0.0 是
`PBOuterFec` 的生产依赖，并通过仓库 overlay 固定到 commit
`067ca7cdb66aed424ec23f97557429bf791c6f0c`。Catch2 仅存在于
非默认 `tests` feature，版本下限为 3.15.0。端口注册表基线由 manifest 的
`builtin-baseline` 固定，安装产物位于各构建目录的 `vcpkg_installed/`，不入库。
Wirehair 的源码 SHA-512、license、关闭的实验/工具选项和 canonical 文档记录见
[`third_party/WIREHAIR_BASELINE.md`](third_party/WIREHAIR_BASELINE.md)。
