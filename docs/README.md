# PixelBridge 文档索引

本目录同时保存当前产品路线、核心规范、模块说明、历史实现记录和人工 Gate 证据。阅读时必须区分“当前合同”“当前实现清单”和“历史证据”，不能因为某个历史 Step 已通过，就推断新的统一视觉产品已经完成。

## 1. 新参与者的最短阅读路径

按以下顺序阅读即可建立当前上下文：

1. [`../AGENTS.md`](../AGENTS.md)：工程、安全、代码风格、测试和 Git 纪律；
2. [`../README.md`](../README.md)：项目入口、目录、构建方式和当前事实边界；
3. [`../CONTRIBUTING.md`](../CONTRIBUTING.md)：面向 GitHub 贡献者的范围、构建、测试和提交约定；
4. [`UNIFIED_VISUAL_LARGE_FILE_IMPLEMENTATION_ROADMAP.md`](UNIFIED_VISUAL_LARGE_FILE_IMPLEMENTATION_ROADMAP.md)：**当前唯一 Goal/目标模式实施路线**；
5. [`PixelBridge_最终技术路线与总体设计.md`](PixelBridge_最终技术路线与总体设计.md)：完整总体架构与长期不变量；
6. [`CURRENT_RUNTIME_OPTION_INVENTORY.md`](CURRENT_RUNTIME_OPTION_INVENTORY.md)：当前工作树仍可到达的旧/过渡 UI、CLI 和 runtime 绑定；
7. 与当前目标直接相关的模块文档和测试。

发生冲突时，优先遵循当次用户要求、`AGENTS.md`、统一路线，再使用总体设计中未被新路线取代的内容。Phase 0、Phase 1.5、RemoteVisual Step 文档不得反向覆盖新产品合同。

## 2. 当前权威文档

| 文档 | 作用 | 当前状态 |
| --- | --- | --- |
| [`UNIFIED_VISUAL_LARGE_FILE_IMPLEMENTATION_ROADMAP.md`](UNIFIED_VISUAL_LARGE_FILE_IMPLEMENTATION_ROADMAP.md) | 冻结最终产品行为；将实现拆为 G00..G22；规定每步最小测试和退出条件 | **当前执行入口** |
| [`PixelBridge_最终技术路线与总体设计.md`](PixelBridge_最终技术路线与总体设计.md) | 协议、FEC、视觉、GPU/capture、线程、存储、安全、验收的总架构 | 权威背景；顶部 supersession 规则优先 |
| [`CURRENT_RUNTIME_OPTION_INVENTORY.md`](CURRENT_RUNTIME_OPTION_INVENTORY.md) | 说明工作树中实际存在的 GUI/CLI/实验路径和未接通能力 | 过渡实现清单，不是最终产品说明 |
| [`GITHUB_PUBLISH_CHECKLIST.md`](GITHUB_PUBLISH_CHECKLIST.md) | 首次推送前、GitHub 仓库配置和源码发布检查 | 当前发布准备入口 |
| [`../README.md`](../README.md) | 仓库首页、快速构建与真实完成状态 | 对外入口 |

## 3. 协议、FEC、恢复与存储

| 文档 | 内容 | 解释边界 |
| --- | --- | --- |
| [`PHASE0_PROTOCOL_STATUS.md`](PHASE0_PROTOCOL_STATUS.md) | Bootstrap/Control、历史 provisional descriptor、Transport、resource policy、Inner/Outer FEC、旧 resume 基础 | 历史实现报告；37-byte SessionDescriptor 已成为必须拒绝 fixture |
| [`PROTOCOL_1_DESCRIPTOR_SCHEMA.md`](PROTOCOL_1_DESCRIPTOR_SCHEMA.md) | 正式 Protocol 1.0 Descriptor Schema 1 字节表、TLV/文件名/资源边界与 Golden manifest | 当前正式 Descriptor 规范 |
| [`GOLDEN_VECTOR_HARNESS.md`](GOLDEN_VECTOR_HARNESS.md) | Golden 生成、验证与工具边界 | Golden 变更时阅读；不能无理由重生成 |
| [`PHASE0_GATE_REPORT.md`](PHASE0_GATE_REPORT.md) | Phase-0 Gate 的既有证据和限制 | 只支持报告中列出的 commit/路径，不等于新产品完成 |

正式 Descriptor、流式发送和 decoder journal 的当前代码入口：

- `libs/PBProtocol/include/pbprotocol/descriptor_codec.h`
- `libs/PBProtocol/include/pbprotocol/protocol_types.h`
- `apps/common/encoder_session_store.h`
- `apps/common/decoder_resume_store.h`
- `libs/PBReceiver/include/pbreceiver/receiver_ingress.h`
- `libs/PBStorage/include/pbstorage/output_file.h`

这些接口在 `1445f9b` 建立了实现基础；完整大文件、故障注入和统一视觉产品接线仍按路线 G01..G05、G18..G20 关闭。

## 4. 视觉编码、呈现、选区与捕获

### 4.1 通用基础

| 文档 | 内容 |
| --- | --- |
| [`REFERENCE_RASTER.md`](REFERENCE_RASTER.md) | 规范 raster、序列化和参考路径 |
| [`DESKTOP_LEVELS_REFERENCE.md`](DESKTOP_LEVELS_REFERENCE.md) | 历史 Direct-Level CPU reference 和边界 |
| [`PRESENTATION.md`](PRESENTATION.md) | D3D11 Data Window 架构、线程与 Present 语义 |
| [`PRESENTATION_VALIDATION.md`](PRESENTATION_VALIDATION.md) | 呈现验证与真实显示 Gate 边界 |
| [`SCREEN_REGION.md`](SCREEN_REGION.md) | PMv2 物理像素 ROI 选择接口 |
| [`SCREEN_REGION_VALIDATION.md`](SCREEN_REGION_VALIDATION.md) | ROI/多显示器验证和人工 Gate |
| [`PBScreenCaptureWgc.md`](PBScreenCaptureWgc.md) | WGC capture 生命周期和 PB-owned texture |
| [`PBScreenCaptureWgc_validation.md`](PBScreenCaptureWgc_validation.md) | WGC 验证边界 |
| [`CAPTURE_BOOTSTRAP_IMPLEMENTATION.md`](CAPTURE_BOOTSTRAP_IMPLEMENTATION.md) | capture→bootstrap 参考流水线 |
| [`LOCAL_DESKTOP_BOOTSTRAP.md`](LOCAL_DESKTOP_BOOTSTRAP.md) | LocalDesktop Bootstrap 几何与诊断 |
| [`SHAPE_CHROMA_D3D11_TELEMETRY_REPLAY.md`](SHAPE_CHROMA_D3D11_TELEMETRY_REPLAY.md) | 历史 Shape+Chroma/D3D11/telemetry/replay 接线 |

### 4.2 统一产品与历史 Profile 的关系

正式产品目标是 `PB-Unified-LC4-V1`、layout 8、Base/Fine/Chroma 独立 lane、0.75x..2.0x 和 mixed Control/Transport slots。它尚未由现有某一份旧 LF4 文档自动满足。

旧 Direct、Shape+Chroma、RemoteVisual 8x8 和 `PB-RemoteVisual-LF4-X1`：

- 可以复用码本、locator、freshness、transform、GPU lifetime 和证据工具；
- 保留为内部 A/B、历史 Golden 或回归 fixture；
- 不再作为最终 GUI 的用户可选 Profile；
- 不能把旧 LF4 的 `1..5 Hz`、0.5x 下限或四 codeword 容量迁移成 Unified 合同；
- 不能按远控 provider 名称选择阈值或解码分支。

## 5. Phase 1.5 GUI 与过渡 runtime

| 文档 | 内容 | 状态 |
| --- | --- | --- |
| [`GUI_PHASE1_5.md`](GUI_PHASE1_5.md) | 旧 GUI controller/presentation 架构与控件绑定 | 历史/过渡实现 |
| [`CURRENT_RUNTIME_OPTION_INVENTORY.md`](CURRENT_RUNTIME_OPTION_INVENTORY.md) | 当前实际可达入口、隐藏能力和限制 | 当前事实清单 |
| [`P1_5_REMOTEVISUAL_HARDENING_CHECKPOINT.md`](P1_5_REMOTEVISUAL_HARDENING_CHECKPOINT.md) | 旧 RemoteVisual 加固检查点 | 历史证据 |

最终 GUI 收敛见统一路线 G15/G16。Qt 只能负责 presentation/controller；协议、FEC、modulation、capture、storage 和 telemetry 核心不得依赖 Qt。

## 6. RemoteVisual 历史路线与证据

[`REMOTE_VISUAL_LOW_FPS_TECHNICAL_ROUTE.md`](REMOTE_VISUAL_LOW_FPS_TECHNICAL_ROUTE.md) 及 Step 06..22 文档记录 2026-09-03 以前的低刷新 LF4 研究、Golden、GPU、Replay、field/package 工具和证据边界。它们很有复用价值，但已由统一路线取代为产品实施入口。

| 文档 | 历史主题 |
| --- | --- |
| [`REMOTE_VISUAL_CHANNEL_MANIFEST.md`](REMOTE_VISUAL_CHANNEL_MANIFEST.md) | provider-generic channel metadata 与矩阵身份 |
| [`REMOTE_VISUAL_STEP06_CORPUS.md`](REMOTE_VISUAL_STEP06_CORPUS.md) | transform corpus |
| [`REMOTE_VISUAL_STEP07_CALIBRATION.md`](REMOTE_VISUAL_STEP07_CALIBRATION.md) | metric calibration |
| [`REMOTE_VISUAL_STEP08_GOLDEN.md`](REMOTE_VISUAL_STEP08_GOLDEN.md) | LF4 Golden freeze |
| [`REMOTE_VISUAL_STEP09_ENCODER.md`](REMOTE_VISUAL_STEP09_ENCODER.md) | immutable LF4 Encoder raster |
| [`REMOTE_VISUAL_STEP09_DYNAMIC_RDP_PILOT.md`](REMOTE_VISUAL_STEP09_DYNAMIC_RDP_PILOT.md) | 动态真实像素 pilot |
| [`REMOTE_VISUAL_STEP10_D3D11_DEMOD.md`](REMOTE_VISUAL_STEP10_D3D11_DEMOD.md) | D3D11 Compute demod |
| [`REMOTE_VISUAL_STEP11_GPU_PARITY.md`](REMOTE_VISUAL_STEP11_GPU_PARITY.md) | 历史 accepted-byte GPU parity |
| [`REMOTE_VISUAL_STEP12_CAPTURE_LIFETIME.md`](REMOTE_VISUAL_STEP12_CAPTURE_LIFETIME.md) | CaptureEpoch、lease、adapter lifetime |
| [`REMOTE_VISUAL_STEP13_TEMPORAL_ADMISSION.md`](REMOTE_VISUAL_STEP13_TEMPORAL_ADMISSION.md) | duplicate/reorder/stale admission |
| [`REMOTE_VISUAL_STEP14_RECEIVER_ADMISSION.md`](REMOTE_VISUAL_STEP14_RECEIVER_ADMISSION.md) | Receiver/Outer 接线 |
| [`REMOTE_VISUAL_STEP15_REPLAY_PRODUCTION.md`](REMOTE_VISUAL_STEP15_REPLAY_PRODUCTION.md) | Replay production path |
| [`REMOTE_VISUAL_STEP16_TELEMETRY_REPORT.md`](REMOTE_VISUAL_STEP16_TELEMETRY_REPORT.md) | 指标真实性和 report merger |
| [`REMOTE_VISUAL_STEP17_GUI_CLI_SCREEN_SAFETY.md`](REMOTE_VISUAL_STEP17_GUI_CLI_SCREEN_SAFETY.md) | GUI/CLI 与双屏安全 |
| [`REMOTE_VISUAL_STEP18_DEPLOYMENT_REPRODUCIBILITY.md`](REMOTE_VISUAL_STEP18_DEPLOYMENT_REPRODUCIBILITY.md) | package/SBOM/deployment identity |
| [`REMOTE_VISUAL_STEP19_LOCALDESKTOP_REGRESSION.md`](REMOTE_VISUAL_STEP19_LOCALDESKTOP_REGRESSION.md) | LocalDesktop 回归 |
| [`REMOTE_VISUAL_STEP20_REAL_REMOTE_PILOT.md`](REMOTE_VISUAL_STEP20_REAL_REMOTE_PILOT.md) | 真实远程 pilot 总体证据 |
| [`REMOTE_VISUAL_STEP20_SINGLE_MONITOR_DIAGNOSTIC.md`](REMOTE_VISUAL_STEP20_SINGLE_MONITOR_DIAGNOSTIC.md) | 单屏诊断边界 |
| [`REMOTE_VISUAL_STEP20_SINGLE_MONITOR_FILE_PILOT.md`](REMOTE_VISUAL_STEP20_SINGLE_MONITOR_FILE_PILOT.md) | 单屏完整文件 pilot |
| [`REMOTE_VISUAL_STEP21_PROVIDER_GENERIC_MATRIX.md`](REMOTE_VISUAL_STEP21_PROVIDER_GENERIC_MATRIX.md) | 未执行/缩减矩阵与单次成功边界 |
| [`REMOTE_VISUAL_STEP22_REPEAT_RECOVERY.md`](REMOTE_VISUAL_STEP22_REPEAT_RECOVERY.md) | 旧重复恢复 readiness；不是已完成的新路线 Gate |

复用历史证据时必须同时记录其 commit、ProfileId、layout、尺度、捕获 backend、是否经过 Receiver/WholeFileDigest/publish，以及未覆盖项。仅有 receiver-only Replay 或 GPU source readback 不能证明最终文件恢复。

## 7. 代码目录与文档责任

| 路径 | 责任 |
| --- | --- |
| `apps/PixelBridgeEncoder` | Encoder Qt、CLI adapter、controller 入口 |
| `apps/PixelBridgeDecoder` | Decoder Qt、CLI adapter、controller 入口 |
| `apps/common` | Qt-free application model/runtime、Session persistence、resume、report |
| `libs/PBProtocol` | Bootstrap/Control/Transport/Descriptor、CRC、摘要、资源校验 |
| `libs/PBCompression` | bounded zstd/RAW Segment 处理 |
| `libs/PBOuterFec` | DirectRepeat/Wirehair V2 |
| `libs/PBInnerFec` | Robust QC-LDPC reference |
| `libs/PBModulation` | CPU raster/oracle、profile、pilots、metrics |
| `libs/PBDemodD3D11` | D3D11 Compute 解调 |
| `libs/PBCaptureNormalize` | WGC/DXGI 共用的 PB-owned texture、epoch、队列 |
| `libs/PBScreenCaptureWgc` | WGC backend |
| `libs/PBScreenCaptureDxgi` | DXGI Desktop Duplication backend |
| `libs/PBRenderD3D` | 独立 Data Window 和 presentation epoch |
| `libs/PBReceiver` | control/data admission、active Outer decoder、conflict/resource 状态 |
| `libs/PBStorage` | `.part`、random verified writes、whole digest、safe publish |
| `libs/PBTelemetry` | 指标、计数和报告 schema |
| `tests` | Catch2/CTest、Golden、静态和 opt-in native Gate |
| `fuzz` | parser/compression/FEC/resume fuzz target 与最小 corpus |
| `tools` | 诊断、Golden、evidence、package 工具；不能成为隐藏 payload 路径 |
| `third_party` | overlay port、固定基线与 license 说明；不提交 installed binaries |

如果公共 wire、Profile、持久状态或报告 schema 改变，必须在同一目标中同步其规范/Golden/compatibility note。普通内部重构不需要把每个实现细节复制到总体设计。

## 8. 测试文档与证据解释

- 默认 CTest 测试不应弹窗、移动鼠标或改变显示设置。
- Presentation/ScreenRegion/WGC/DXGI/LocalDesktop 的 native Gate 都是 opt-in，并需要显式目标显示器坐标。
- 统一路线只在 G20 运行一次完整 Release CTest 和真实右屏 Gate。
- 性能结论必须使用最终发布后的 verified bytes 与唯一逻辑帧；理论容量、Present FPS 和 callback FPS 不是 goodput。
- Golden 只证明给定输入下的确定性语义；不能证明真实远程链路。
- WARP 只证明 D3D11 参考/兼容路径；不能替代目标硬件 adapter。
- Build/GUI smoke 只证明装载和基本生命周期；不能证明视觉恢复。

## 9. 文档维护规则

1. 当前产品决策变更：先更新统一路线顶部和冻结合同，再更新总体设计 supersession note。
2. Goal 完成：更新路线中目标状态、提交和证据；不要删掉未通过/未执行历史。
3. 历史 Step 文档：原则上只追加勘误或 supersession note，不改写旧结论的上下文。
4. 新模块文档：文件名使用稳定的英文大写或明确模块名；中文总体设计文件保留原名。
5. 文档中的命令应从仓库根目录可执行，并明确是否会打开窗口、占用显示器或产生大文件。
6. 所有链接使用仓库相对路径；证据 artifact 不应依赖开发机的临时 build 路径。
7. 未经明确批准，本地临时 Gate 报告、运行日志和用户文件不加入 Git。

## 10. GitHub 发布入口

首次推送前执行 [`GITHUB_PUBLISH_CHECKLIST.md`](GITHUB_PUBLISH_CHECKLIST.md)。当前源代码仓库不应包含 build tree、vcpkg 安装目录、`.part`、resume journal、日志、压缩发布包或真实传输数据。项目自身 LICENSE、GitHub 可见性、远程 URL 和默认分支由维护者明确决定。
