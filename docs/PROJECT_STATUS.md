# PixelBridge 项目现状

> **文档性质：** 本文件是项目现状的唯一入口级描述，取代此前分散在各 Gate 记录里的"当前状态"表述。
>
> **事实来源优先级（不变）：** 当前 `git log` 与仓库内容 -> 源码/测试/封存 artifact -> 各文档顶部带日期的原始记录 -> 本文件。冲突时以更早的原始记录为准；本文件负责指出冲突，不改写历史。
>
> **本轮整理边界（2026-09-09）：** `docs/` 的大规模清理属于**纯文档与 Git 记账**。整理过程中**未执行任何构建、单元测试、集成测试、实屏、远程桌面/Citrix 或大文件现场流程**，未修改任何源码字节、协议常量、Visual Profile、Golden Vector 或封存证据。因此本文件与同批提交**不构成任何新的验证、晋级或性能结论**。
>
> **本次整理时点：** HEAD `7e88a69`；最后一次改动源码的提交 `46a072b`。判断源码身份一律用 `git log --oneline -- libs apps tests tools CMakeLists.txt`；判断证据身份用封存包/实验目录内的 build 记录。

## 1. 项目定义

PixelBridge 是 Windows x64 / C++20 项目，通过**可见桌面像素**做单向高吞吐文件传输。两个程序：

- `PixelBridgeEncoder.exe`：把文件切段、编码为视觉帧序列，并在指定屏幕区域呈现。
- `PixelBridgeDecoder.exe`：**只**从实际捕获的屏幕区域像素里恢复 payload。

硬约束（长期不变量，见 `../AGENTS.md` 与总体设计）：不得引入 socket、pipe、共享内存、COM、剪贴板、窗口消息、临时文件等任何第二条 payload 通道；未通过整文件摘要校验的输出不得算作已接受。

## 2. 当前产品合同（权威）

| 项 | 值 |
| --- | --- |
| 产品视觉合同 | `PB-Unified-SC6-V3` |
| `VisualProfileId` | `0x5042554E49534333` |
| layout | `10` |
| 规范画布 | 1920x1080 物理像素 |
| 实验身份（**未**进正式 catalog） | `PB-Experimental-BlankControl-1`，`0x504242414E443031`，layout `11` |

实验身份 `0x504242414E443031`/layout 11 只服务于吞吐优化线的空白带控制研究，**不是**产品 Profile，任何文档或代码都不得把它当作已冻结合同。

### 2.1 仍在工作树的权威与规范文档

| 文档 | 约束范围 |
| --- | --- |
| [`PixelBridge_最终技术路线与总体设计.md`](PixelBridge_最终技术路线与总体设计.md) | 长期总体设计：协议、FEC、视觉、GPU/capture、线程、存储、安全、验收 |
| [`UNIFIED_VISUAL_LARGE_FILE_IMPLEMENTATION_ROADMAP.md`](UNIFIED_VISUAL_LARGE_FILE_IMPLEMENTATION_ROADMAP.md) | G00-G22 产品合同与实现史（SC6-V3/layout 10 的落地契约） |
| [`PROTOCOL_1_DESCRIPTOR_SCHEMA.md`](PROTOCOL_1_DESCRIPTOR_SCHEMA.md) | `SessionDescriptor`/`SegmentDescriptor`/`FinalManifest` 字节合同、TLV、文件名与资源边界 |
| [`UNIFIED_TELEMETRY_REPORT.md`](UNIFIED_TELEMETRY_REPORT.md) | `PixelBridge.RunReport.3` 字段与 `VerifiedRawGoodput`/`VerifiedEncodedGoodput` 口径 |
| [`UNIFIED_G22_GUI_RELEASE.md`](UNIFIED_G22_GUI_RELEASE.md) | 双端 GUI 现行交互合同与 Windows 发布候选检查项 |
| [`CURRENT_RUNTIME_OPTION_INVENTORY.md`](CURRENT_RUNTIME_OPTION_INVENTORY.md) | 工作树里真实存在的 GUI/CLI/实验路径清单（区分产品路径与过渡路径） |
| [`UNIFIED_USER_GUIDE.md`](UNIFIED_USER_GUIDE.md) | 双端 GUI 日常操作与已知退出行为 |
| [`REFERENCE_RASTER.md`](REFERENCE_RASTER.md) | CPU 参考 raster/demod 与序列化参考路径（SIMD/GPU 对照基准） |
| [`PRESENTATION.md`](PRESENTATION.md) | Data Window 与 `PBPresentTiming` 呈现契约 |
| [`SCREEN_REGION.md`](SCREEN_REGION.md) | Per-Monitor DPI Aware V2 物理像素 ROI 接口与坐标语义 |
| [`PBScreenCaptureWgc.md`](PBScreenCaptureWgc.md) | WGC 帧池 lease 生命周期与 PB-owned texture 规则 |
| [`ENCODER_STREAMING_CAROUSEL.md`](ENCODER_STREAMING_CAROUSEL.md) | 预扫描、双 Segment Carousel、durable ID lease、状态 schema |
| [`DECODER_RESUMABLE_RECOVERY.md`](DECODER_RESUMABLE_RECOVERY.md) | journal、乱序 `.part`、重启重验、故障分类 |
| [`GOLDEN_VECTOR_HARNESS.md`](GOLDEN_VECTOR_HARNESS.md) | `PBGoldenVectorCheck`/`PBVectorGen` 工具边界 |

## 3. 仓库与工作区现状

| 项 | 现状 | 核对方式 |
| --- | --- | --- |
| 远端 | 仓库**无 remote**，全程未 push、未 reset、未 rebase、未重写历史 | `git remote -v` |
| 工作树 | 干净；唯一未跟踪的项目文件为受保护的 `docs/PHASE1_GATE_REPORT.md`（另可有本地会话目录 `.zcode/`，非项目内容） | `git status --porcelain -uall` |
| 受保护文件 | SHA-256 `076ef4c9b9f89eabccd323dbe4bffc4dc125ddaf96e6ee437d2cf5b1b1cea306`，永不 add/修改/移动/删除 | `Get-FileHash` |
| 源码最后改动 | `fca42e1 fix(tools)`（2026-09-10 新增 PBRemoteOpsBridge 远程操作桥及部署修复）；其后为文档提交 | `git log -- libs apps tests tools CMakeLists.txt` |
| 工作区遗留 | 仓库根 `display_probe.obj`（366,491 B，被 `*.obj` 规则忽略）**未删除**，需用户确认后才可清理 | `Test-Path display_probe.obj` |
| 证据区 | `artifacts/`（约 12.9 GiB）、119 个 `build-*` 目录、现场录像与 raw 像素全部保留，本次整理未触碰 | 目录存在性 |

**封存身份规则（沿用整理前的记账）：** `8d60f77`…`46a072b` 只是把暂停时工作树里已有的源码改动入库，入库动作本身不代表这些改动在后续任何一轮里被重新构建或重新测试。`--measurement-build-identity` 的 `baseCommit` 与 `sourceFingerprintSha256` 均为编译期注入，只有重新构建后才反映新 HEAD；旧交付包、录像、raw 像素与实验封存目录仍按**封存时记录**的 build 身份解释。若要以当前源码身份作为新对照基线，必须重新 freeze、build、test、package、seal。

## 4. 能力边界：已证明到哪一步

### 4.1 已有冻结证据、可作依据（但边界严格受限）

| 能力 | 证据 | 边界 |
| --- | --- | --- |
| Descriptor/FEC/Receiver/journal/storage/publish 无屏幕闭环 | G04 CP-A，9 个 case | 原文 `visualChainCovered=false`，不得推出视觉链覆盖 |
| Encoder 产品工作流 | G15 | 数值不直接代表 SC6-V3/layout 10 现网 |
| Decoder 产品工作流 | G16 | 当时未跑实屏/远程/大文件 |
| 遥测与运行报告真值 | G17 `PixelBridge.RunReport.3` | Present/callback FPS 不是 goodput |
| 进程终止后恢复 | G18：256 MiB、9 个终止点 | 批准的无像素 headless，**不是**现场认证 |
| 大文件能力 | G19：20 GiB + 64 KiB、2,561 Segment、外部双摘要 | 能力边界，**不是**吞吐结论 |
| 本地发布门（历史） | G20：LC4/layout 8、0.75x、31-slot，218/218 Release CTest（提交 `e0729b2`） | **不可**用于任何 SC6-V3 声明；该物理层已被后续远控失真证伪 |
| 真实远程像素链 | G21：1 GiB 真像素恢复、跨机双摘要 | 终态 `PASS_WITH_SINGLE_RUN_USER_WAIVER`；**原始性能门 FAIL** |
| 双端 GUI | G22 `PASS_LOCAL_CANDIDATE`（构建 `3a840a2`） | 本地候选，非现场/远程认证 |
| Encoder 无 DXGI-output 兼容 | 本机 1 MiB digest/publish/reopen + clean-startup | `LOCAL_REGRESSION_PASS / CITRIX_FIELD_PENDING`；本机为 mapped hardware |

### 4.2 未证明、未认证、不得声称

1. **非本机整文件吞吐提升未确立。** 该线没有晋级任何默认值，没有修改主区控制、layout 10、摘要、安全发布或 reopen 规则。
2. **G21 的原始性能门是 FAIL**：`8,626.510998634209 B/unique` < 16 KiB 硬门；用户豁免**仅限该次会话**，不得扩展到其它会话或 Citrix 链路。
3. **Citrix 现场实屏未验证**：Encoder 显示兼容修复停在 `CITRIX_FIELD_PENDING`。
4. **Step1 完整现场矩阵、Step2/Step3 现场门均 `NOT_RUN`**；Step1/Step2 本地状态为 `PREPARED/PARTIAL`。
5. 任何本地、offscreen、artifact-only、录像、WARP、CPU reference、receiver-only 结果**都不是**现场认证。
6. RemoteVisual 历史路线：Step 20 仍 `MANUAL-GATE`、Step 21 扩展矩阵 `NOT_EXECUTED`（`formalStep21Accepted=false`）、Step 22 `executedRunCount=0`。

## 5. 两条研发线的状态

### 5.1 统一产品路线 G00-G22

已完成的 Gate 序列：G00-G22（协议/Descriptor/FEC/Carousel/恢复/遥测/工作流/大文件/本地门/远程门/GUI）。产品合同冻结在 `PB-Unified-SC6-V3` + layout 10，GUI 处于本地发布候选。逐 Gate 的终态、证据根与哈希见 [`EVIDENCE_INDEX.md`](EVIDENCE_INDEX.md)。

### 5.2 非本机吞吐优化线（G22 之后）——**已停止**

**2026-09-09 由用户主动停止**：非技术阻塞、非任务完成。恢复工作只在用户重新开启目标后进行。

- 该线共有 16 篇执行/诊断记录（Step1-Step5、四轮归因、几何准入、固定 codec 观测、夜间第 1-23 节）。
- 结论：**没有确立任何非本机吞吐收益**，未晋级任何默认值。
- 四轮归因 A1/B1/B2/A2：原组 B 更快、反序观察 A 更快，但 A2 主进程残留后由用户人工结束、exit1/预算告警，**不存在完全合规的反序配对**，因此"稳定增益已证明"不成立。
- 第 22 节空白带固定 codec A/B 两组均 `NOT_RECOVERED`（A：15 次 LocatorFailure + 15 次 CanvasClipped；B：30 次 CanvasClipped），主帧准入在 Outer FEC 之前被拒。
- CPU 侧的两项优化（QC-LDPC 行归一化复用 `1.8984 -> 1.4471 ms`，约 23.8%；Bootstrap 顺序 BGRA 扫描游标，中位 -12.2%/-9.6%）**只是对应 CPU 阶段耗时**，不是远控吞吐百分比。

## 6. 度量口径（不得混淆）

| 口径 | 定义位置 | 常见误用 |
| --- | --- | --- |
| `VerifiedRawGoodput` | [`UNIFIED_TELEMETRY_REPORT.md`](UNIFIED_TELEMETRY_REPORT.md) | 用理论 bits/cell 或画布容量替代 |
| `VerifiedEncodedGoodput` | 同上 | 用发送端编码字节速率替代"已验证"字节 |
| `UniqueVisualFPS` | 同上；`MinUpdateInterval`/刷新率/Present 调用率都不是证据 | 把 Present 或 callback 频率当唯一帧率 |
| 最终真值 | **最终发布并 reopen 的整文件时间**除以唯一逻辑帧数 | 用中间态、首 GOP、单段或完成点媒体 PTS 替代 |

规则补充：

1. 只有**最终发布并 reopen 的整文件**时间，加上 `VerifiedRawGoodput`/`VerifiedEncodedGoodput`，才是吞吐真值。
2. Step1 的主时长定义为 `finalReopenVerified - startAccepted`。晚加入场景必须以 `(SessionTag, FrameSequence)` 精确关联 Sender `events.jsonl`，**不得跨主机直接相减时间**，也不得插值。
3. `RunReport.3.measurement` 中失败或缺失的计时、覆盖、身份必须写为 `null`/failed，**不得制造样本**；测量环、报告、证据和时长都有上限。
4. 理论容量、离线 artifact 展开率、录像身份率、CPU reference 阶段耗时，都不能替代 `VerifiedEncodedGoodput`。

## 7. 未关闭事项（现状登记，不是执行授权）

| 事项 | 现状 | 关闭所需 |
| --- | --- | --- |
| 非本机整文件吞吐提升 | 未确立，用户主动停止 | 用户重新开启目标，从恢复入口续做 |
| 4/3 显示采样适配（夜间记录第 23 节） | 仅只读核对，未实现/构建/运行 | 新授权 + 新 root/build/output + 有界验证 |
| 空白带补充控制（layout 11） | CPU 参考通过；真实 codec 未恢复；控制槽未释放 | 独立显示采样变量 + 明确擦除预算，不降门限 |
| 以当前源码身份建立对照基线 | 未完成（`baseCommit`/`sourceFingerprintSha256` 仍是旧 build 注入值） | 重新 freeze、build、test、package、seal |
| Citrix Encoder 显示兼容 | `CITRIX_FIELD_PENDING` | 在原 Citrix 会话完整解压包、观察数据帧并人工按 Esc |
| Step1 完整现场矩阵 / Step2-Step3 现场门 | `NOT_RUN` | 现场窗口与人工操作（不使用输入自动化，不动左屏） |
| 工作区遗留 `display_probe.obj` | 未删除 | 需用户确认 |

## 8. 文档地图

### 8.1 `docs/`（2026-09-09 整理后 18 篇，另有后续新增）

| 类别 | 文档 |
| --- | --- |
| 现状入口 | `PROJECT_STATUS.md`（本文）、`README.md`（薄索引） |
| 证据索引 | `EVIDENCE_INDEX.md` |
| 历史与恢复 | `DOC_HISTORY.md` |
| 总体设计 | `PixelBridge_最终技术路线与总体设计.md` |
| 产品合同与实现史 | `UNIFIED_VISUAL_LARGE_FILE_IMPLEMENTATION_ROADMAP.md` |
| 协议字节 | `PROTOCOL_1_DESCRIPTOR_SCHEMA.md` |
| 遥测真值 | `UNIFIED_TELEMETRY_REPORT.md` |
| GUI 与运行时 | `UNIFIED_G22_GUI_RELEASE.md`、`CURRENT_RUNTIME_OPTION_INVENTORY.md`、`UNIFIED_USER_GUIDE.md` |
| 视觉/呈现/捕获 | `REFERENCE_RASTER.md`、`PRESENTATION.md`、`SCREEN_REGION.md`、`PBScreenCaptureWgc.md` |
| 流式与恢复 | `ENCODER_STREAMING_CAROUSEL.md`、`DECODER_RESUMABLE_RECOVERY.md` |
| 工具合同 | `GOLDEN_VECTOR_HARNESS.md` |
| 远程实验通道 | `REMOTE_OPS_BRIDGE.md`（2026-09-10 新增；SMB 文件协议操作桥的机制与用法入口） |

### 8.2 `docs/` 之外

| 文档 | 状态 |
| --- | --- |
| [`../AGENTS.md`](../AGENTS.md) | 当前权威（工程纪律：协议不变量、资源安全、C++ 风格、Git 纪律） |
| [`../README.md`](../README.md) | 当前入口（含吞吐线停止状态与未关闭项摘要） |
| [`../CONTRIBUTING.md`](../CONTRIBUTING.md) | 当前入口（贡献范围/构建/测试约定） |
| `../tools/**/README.md` | 各工具说明；2026-09 实验工具多为独立 `project()`，仅少数经 `tools/CMakeLists.txt` 接入主构建。工具不得成为第二条 payload 通道 |
| `../tests/UnifiedRemoteGate/README.md`、`FIELD_1GIB_README.md` | G21 现场与 1 GiB 豁免流程说明；豁免仅限该次会话 |
| `../benchmarks/README.md`、`../fuzz/**` | 基准与 fuzz 入口；resume 状态仍按不可信持久输入处理 |
| `../third_party/README.md`、`WIREHAIR_BASELINE.md` | 第三方基线与 SBOM 依据 |

### 8.3 本次 `docs/` 整理

2026-09-09 对 `docs/` 做了结构性清理：`git rm` 64 篇历史/冻结/诊断类记录，保留 14 篇现行权威与规范，新增本文件、`EVIDENCE_INDEX.md`、`DOC_HISTORY.md` 三篇。**所有被删除文件都可按 `DOC_HISTORY.md` 记录的 blob SHA-1 逐字节取回**，删除清单、原状态与取回命令见 [`DOC_HISTORY.md`](DOC_HISTORY.md)。

## 9. 续做与恢复入口

1. **先读** 本文件第 2/4/7 节，确认产品合同与"未证明"边界。
2. **查证据** ：[`EVIDENCE_INDEX.md`](EVIDENCE_INDEX.md) 给出每个 Gate 与实验的终态、证据根路径、关键哈希。
3. **取历史** ：需要旧 Gate 全文时按 [`DOC_HISTORY.md`](DOC_HISTORY.md) 的 blob 取回，不要凭记忆重述。
4. **改协议/Profile 前** ：先读总体设计与 `PROTOCOL_1_DESCRIPTOR_SCHEMA.md`，并遵守 wire 兼容 + Golden Vector 要求。
5. **Git 纪律** ：显式路径、原子、非 amend；禁止 `git add -A` / `git add .`；不 reset/rebase/重写历史；提交前后执行 `git diff --cached --check`。
6. **保护文件** ：`docs/PHASE1_GATE_REPORT.md` 永不 add/commit/修改/移动/删除，始终保持唯一未跟踪。
7. **现场动作由人执行** ：不干扰左屏、鼠标或键盘；只用右侧测试屏；不做输入自动化。

## 10. 文档维护规则

在 `../README.md`/`../AGENTS.md` 既有纪律之上：

1. **状态只有一个入口。** 现状写在本文件，证据结论写在 `EVIDENCE_INDEX.md`，历史文档的去处写在 `DOC_HISTORY.md`；其他文档不得再复制一份"当前状态"。
2. **纯文档提交不追记 HEAD 哈希。** 判断源码身份用 `git log --oneline -- libs apps tests tools CMakeLists.txt`；判断证据身份用封存包/实验目录内的 build 记录。
3. **冻结证据只允许追加勘误**，不得改写当时的结论、数值或失败事实。
4. **失败与豁免必须保留。** 任何整理、重写或精简都不得抹掉失败门限、豁免范围与未认证边界。
5. **新建或停用文档时同步更新** `docs/README.md` 索引与本文件第 8.1 节。
6. **删除文档一律用 `git rm`** 并在 `DOC_HISTORY.md` 登记 blob SHA-1，保证逐字节可恢复；不建 `docs/archive/` 目录。
