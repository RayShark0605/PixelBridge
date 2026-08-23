# PixelBridge

Windows x64 / C++20：通过可见桌面/视频像素进行的高性能单向文件传输。
规范设计文档：`docs/PixelBridge_最终技术路线与总体设计.md`；工程规则见 `AGENTS.md`。

## 目录

| 路径 | 用途 |
| --- | --- |
| `apps/PixelBridgeEncoder`、`apps/PixelBridgeDecoder` | 应用（当前为控制台空壳，Qt 6 UI 在后续里程碑接入） |
| `libs/PBCore`、`libs/PBProtocol`、`libs/PBCompression`、`libs/PBOuterFec` | 核心静态库（禁止依赖 Qt） |
| `tools`、`fuzz`、`benchmarks` | 独立可选子图；fuzz 与 descriptor-state benchmark 已有真实 target |
| `tests` | Catch2 v3 单元测试（CTest） |
| `docs` | 设计文档 |

## Phase-0 协议状态

当前 37/110/142/65-byte Session/Segment/Final payload 是 **Phase-0 provisional implementation slice**，不是完整正式 v1 wire 承诺。它仍缺少总体设计要求的 `SessionVisualProfileId` 等固定 Session 绑定，因而不得在后续 Data Plane/backend 接入时被误称为已冻结的正式 canonical v1 schema。当前状态、资源预算与升级前置条件见 [`docs/PHASE0_PROTOCOL_STATUS.md`](docs/PHASE0_PROTOCOL_STATUS.md)。

## Target 与依赖边界

- `PBCore`、`PBProtocol` 是显式静态库，不受父工程 `BUILD_SHARED_LIBS` 影响。
- `PBCompression` 是显式静态库；外部消费者只链接 `PB::PBCompression`
  即可获得 PBProtocol 与 zstd 的完整静态链接闭包。
- `PBOuterFec` 是显式静态库；外部消费者只链接 `PB::PBOuterFec` 即可获得
  PBProtocol 与固定 Wirehair 静态库的完整链接闭包。公共头不暴露 Wirehair
  原生头或 host-native profile struct。
- `PBProtocol` 不依赖 `PBCore`；消费者只获得所链接 target 的公共头和链接闭包。
- `PB::CompilerSettings` 仅供 PixelBridge 自有 target 私有使用，`/WX` 等策略不传播给外部消费者。
- Qt 只允许由应用以 `PRIVATE` 方式链接；`libs/` 下的核心库和公共头禁止依赖 Qt。
- PBOuterFec/PBFEC、协议与 CPU reference 模块保持平台无关。
- WGC、DXGI、Capture Normalize、D3D11 Demod 和 CUDA Demod 分别建立 target，不把平台 backend 塞入公共核心库。
- CUDA 选项只与真实 CUDA target 同时引入，默认关闭；显式启用后缺失依赖必须配置失败，不允许静默 fallback。

CMake 在配置期审计核心 target 的 Qt 依赖、公共 `src/` 路径和公共编译选项泄漏。未来模块必须继续满足这些门禁。

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
`MakeDecompressionLimits(resourcePolicy, maxWindowLog)` 构造本地解压边界。该
前置条件保证任何 zstd context 或输出 allocation 创建前，Descriptor 的 raw 和
encoded 配额已经验证。

## Outer FEC / Wirehair V2

`PBOuterFec` 只使用 Wirehair V2 canonical serialized-profile API。初次创建默认
显式选择 `WIREHAIR_V2_PROFILE_CERTIFIED_2026_07`（不使用 `CURRENT`），保存上游
原样返回的 32-byte descriptor；`OuterBlockId` 原值就是 Wirehair `blockId`，其中
`0..K-1` 为 systematic，`K..` 为 repair。

`WirehairV2Encoder::Recreate()` 先验证 `EncodedSize`、`OuterBlockBytes`、
`2 <= K <= 64000` 与 BLAKE3 `EncodedDigest`，再确认 exact bytes 对应的 canonical
selection 与 saved descriptor 逐字节一致，最后使用 saved descriptor 与 exact
Encoded Segment bytes 调用 `wirehair_v2_encoder_create_profile()`。不会用新 profile、
seed 或 attempt 替换保存状态。

Decoder 将 `NeedMore` 作为正常增量状态；`ExtraInsufficient`、OOM、unsupported、
bad seed、invalid input 与未知 codec 结果均显式 fail closed。accepted block-ID
冲突检测有固定上限，不假设一个 decoder 能无限接收新的 repair IDs；
`ExtraInsufficient` 后必须销毁该实例并由上层用完整新 repair window 重建。

Profile ID 只选择方程兼容性，不认证发送者。canonical descriptor、CRC、
`EncodedDigest` 以及任何 in-band whole-file digest 也不能单独提供发送者认证；
recovered bytes 仍必须经过设计书要求的 Segment、解压与 whole-file 验证流程。

## 构建（MSVC x64）

依赖：Visual Studio 2022（C++ 工作负载）、CMake >= 3.24、vcpkg（本机 `D:\vcpkg`）。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake
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
| `PB_BUILD_TOOLS` | `OFF` | `tools/` |
| `PB_BUILD_FUZZERS` | `OFF` | `fuzz/`：`PBProtocolDescriptorResourceFuzz`、`PBCompressionZstdBoundaryFuzz` |
| `PB_BUILD_BENCHMARKS` | `OFF` | `benchmarks/`：`PBProtocolDescriptorStateBenchmark` |
| `BUILD_TESTING` | 顶层 `ON`，子工程由父工程管理 | 全局 CTest 开关 |
| `PB_BUILD_TESTS` | 顶层 `ON`，作为子工程时 `OFF` | PixelBridge 的 `tests/`；顶层同时控制 vcpkg `tests` feature |

fuzz 与 benchmark 共用 `PBProtocol`，但 fuzz 构建会对 `PBProtocol` 和
`PBCompression` 静态库本身启用 AddressSanitizer，而不是只插桩 driver，
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
.\build-fuzz-msvc\fuzz\RelWithDebInfo\PBCompressionZstdBoundaryFuzz.exe `
  2000 13856851484949778996

# Replay one pinned PBCompression corpus input.
.\build-fuzz-msvc\fuzz\RelWithDebInfo\PBCompressionZstdBoundaryFuzz.exe `
  --input .\fuzz\corpus\compression-zstd\wide-window.bin
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
