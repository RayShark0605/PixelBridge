# PixelBridge 文档索引

> 本文件是 `docs/` 的**薄索引**，只负责"先读哪份、每份管什么"。它**不叙述现状、不记录数值、不做状态判定**——那些只写在一处：[`PROJECT_STATUS.md`](PROJECT_STATUS.md) 与 [`EVIDENCE_INDEX.md`](EVIDENCE_INDEX.md)。
>
> 2026-09-09 结构性整理：`docs/` 从 79 篇 tracked 文档裁到 18 篇（15 篇保留 + 3 篇新增）。被删文档的逐篇 blob 清单与取回方式见 [`DOC_HISTORY.md`](DOC_HISTORY.md)。

## 1. 最短阅读路径

1. [`PROJECT_STATUS.md`](PROJECT_STATUS.md) — 项目定义、产品合同、能力边界、两条研发线的终态、未关闭事项、续做入口。
2. [`EVIDENCE_INDEX.md`](EVIDENCE_INDEX.md) — 三层证据身份规范、G00-G22 Gate 索引、G21/G22 会话与交付包字段、吞吐线归因数字与强制引用限制、Citrix 现场诊断。
3. [`UNIFIED_USER_GUIDE.md`](UNIFIED_USER_GUIDE.md) — 操作员日常怎么跑。
4. [`../AGENTS.md`](../AGENTS.md) — 工程约束、协议不变量、风格与验收标准（改动前必读）。
5. 需要协议/模块细节时再按下面第 3 节挑对应的规范文档。
6. 需要做远程机实验时先读 [`REMOTE_OPS_BRIDGE.md`](REMOTE_OPS_BRIDGE.md) — 远程实验操作桥的机制与用法入口。

任何"当前是什么状态"的表述，若与 `PROJECT_STATUS.md` 冲突，以 `PROJECT_STATUS.md` 为准；若 `PROJECT_STATUS.md` 本身与 live 运行行为、Git 或 artifact 冲突，以那些一手证据为准并回写修正。

## 2. 三份现状文档的分工

| 文档 | 回答什么问题 | 不写什么 |
| --- | --- | --- |
| `PROJECT_STATUS.md` | 现在能做到什么、不能做到什么、下一步从哪续 | 不复制 Gate 原始数值字段 |
| `EVIDENCE_INDEX.md` | 某个结论的证据在哪、字段是什么、引用时的限制 | 不做能力判断 |
| `DOC_HISTORY.md` | 哪些文档没了、为什么、怎么逐字节取回 | 不改写被删文档的结论 |

## 3. 规范与操作文档（2026-09-09 整理保留 14 篇，另有后续新增）

| 文档 | 管什么 |
| --- | --- |
| [`PixelBridge_最终技术路线与总体设计.md`](PixelBridge_最终技术路线与总体设计.md) | `AGENTS.md` 指定的 canonical 总体架构与技术路线 |
| [`PROTOCOL_1_DESCRIPTOR_SCHEMA.md`](PROTOCOL_1_DESCRIPTOR_SCHEMA.md) | Protocol v1 描述符 wire schema（显式 little-endian、边界检查） |
| [`REFERENCE_RASTER.md`](REFERENCE_RASTER.md) | 参考光栅化与逐像素定义（正确性基准） |
| [`PRESENTATION.md`](PRESENTATION.md) | 呈现层：独立 Data Window、swap chain、Present 语义 |
| [`SCREEN_REGION.md`](SCREEN_REGION.md) | ROI 选取、物理像素、Per-Monitor DPI V2 |
| [`PBScreenCaptureWgc.md`](PBScreenCaptureWgc.md) | WGC 捕获后端与帧池租约生命周期 |
| [`ENCODER_STREAMING_CAROUSEL.md`](ENCODER_STREAMING_CAROUSEL.md) | Encoder 流式 Carousel 发送与内存有界模型 |
| [`DECODER_RESUMABLE_RECOVERY.md`](DECODER_RESUMABLE_RECOVERY.md) | Decoder resume 状态机与持久化语义 |
| [`UNIFIED_TELEMETRY_REPORT.md`](UNIFIED_TELEMETRY_REPORT.md) | Unified 遥测与 `RunReport.3` 度量真值契约 |
| [`GOLDEN_VECTOR_HARNESS.md`](GOLDEN_VECTOR_HARNESS.md) | Golden Vector 装置与回归口径 |
| [`CURRENT_RUNTIME_OPTION_INVENTORY.md`](CURRENT_RUNTIME_OPTION_INVENTORY.md) | 当前运行时选项与配置项清单 |
| [`UNIFIED_USER_GUIDE.md`](UNIFIED_USER_GUIDE.md) | 统一用户手册 |
| [`UNIFIED_G22_GUI_RELEASE.md`](UNIFIED_G22_GUI_RELEASE.md) | G22 GUI 发布内容与交互合同（右屏、Esc、不干扰左屏） |
| [`UNIFIED_VISUAL_LARGE_FILE_IMPLEMENTATION_ROADMAP.md`](UNIFIED_VISUAL_LARGE_FILE_IMPLEMENTATION_ROADMAP.md) | G00-G22 统一路线的目标定义、决策与验收口径（历史权威） |
| [`REMOTE_OPS_BRIDGE.md`](REMOTE_OPS_BRIDGE.md)（2026-09-10 新增） | 远程实验操作桥（PBRemoteOpsBridge）：SMB 文件协议机制、远程 Encoder 实验编排用法、部署与故障排查入口 |

## 4. `docs/` 之外的文档

| 路径 | 责任 |
| --- | --- |
| `../AGENTS.md` | 工程约束与协议不变量（优先级最高的项目级规则） |
| `README.md`（仓库根） | 面向外部的一句话项目说明与构建入口 |
| `tests/UnifiedRemoteGate/README.md` | opt-in native Gate 的运行方式与安全前提 |
| `tests/UnifiedRemoteGate/FIELD_1GIB_README.md` | 1 GiB 现场流程（人工执行） |
| `tools/README.md` 及各工具目录 | 诊断/取证/打包工具的局部说明；工具不得成为隐藏 payload 通道 |

## 5. 代码目录与文档责任

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

公共 wire、Profile、持久状态或报告 schema 改变时，必须在同一改动里同步其规范文档、Golden 与 compatibility note；普通内部重构不需要把实现细节复制进总体设计。

## 6. 证据解释边界

- 默认 CTest 不应弹窗、移动鼠标或改变显示设置；Presentation/ScreenRegion/WGC/DXGI/LocalDesktop 的 native Gate 全是 opt-in，且需显式目标显示器坐标。
- 性能结论必须使用最终验证/发布/重开后的 verified bytes 与明确时间分母；理论容量、Present FPS、callback FPS、录像身份率都不是 goodput。
- Golden 只证明给定输入下的确定性语义，不证明真实远程链路。
- WARP 只证明 D3D11 参考/兼容路径，不替代目标硬件 adapter。
- Build/GUI smoke 只证明装载与基本生命周期，不证明视觉恢复。
- offscreen、静态校验、CPU reference、receiver-only、录像诊断都不构成 runtime-field certification。
- 完整 Gate 记录与豁免/失败事实只能在 `EVIDENCE_INDEX.md` 里查；**失败与豁免不得被后续整理抹掉**。

## 7. 文档维护规则

1. 产品决策变更：先改统一路线顶部与冻结合同，再补总体设计的 supersession note。
2. 目标推进：更新状态、提交哈希与证据路径；不删未通过/未执行的历史。
3. 历史文档：原则上只追加勘误或 supersession note，不改写旧结论的上下文。
4. 新文档：新增时必须同时在 `PROJECT_STATUS.md` 的文档地图与本索引登记，并说明它与既有文档的取代关系。
5. 删除：一律 `git rm`（不建 `docs/archive/`），并在 [`DOC_HISTORY.md`](DOC_HISTORY.md) 登记路径、blob SHA-1、blob 字节与状态摘要。
6. 链接：只用仓库相对路径，且必须是真可解析的 Markdown 链接；证据 artifact 不得依赖开发机临时 build 路径。
7. 未经明确批准，本地临时 Gate 报告、运行日志与用户文件不加入 Git。
8. `docs/PHASE1_GATE_REPORT.md` 由用户长期保留但**不入库**：整理、暂存、提交时不得暂存、修改、移动或删除它。

## 8. 发布入口

首次推送前执行发布检查清单：原文已删除，取回 `git cat-file blob 9b554bda9c8009886cdaa474e7dcecf3bbec7668`（记录日期 2026-09-05；当前源代码仓库不应包含 build tree、vcpkg 安装目录、`.part`、resume journal、日志、压缩发布包或真实传输数据）。项目 LICENSE、GitHub 可见性、远程 URL 与默认分支由维护者明确决定，仓库当前**无 remote**。
