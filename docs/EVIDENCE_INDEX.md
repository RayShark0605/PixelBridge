# PixelBridge 证据索引

> **文档性质：** 本文件是项目**唯一入口级证据索引**，回答三个问题：某个结论的证据在哪个路径、身份哈希是多少、当时的终态字段写了什么。本文件**只做索引与身份登记**，不重述实验过程、不新增任何结论。
>
> **哈希复核时点：** 2026-09-09，本机 `Get-FileHash -Algorithm SHA256` 重新计算。标注 ✅ 的条目为本轮独立重算并与封存清单一致；未标注的是封存清单内记录值（未在本轮逐一重算）。
>
> **本轮整理边界：** `docs/` 清理属**纯文档与 Git 记账**。整理过程中**未执行任何构建、单元测试、集成测试、实屏、远程桌面/Citrix 或大文件现场流程**，未修改任何源码字节、协议常量、Visual Profile、Golden Vector 或封存证据。因此本文件与同批提交**不构成任何新的验证、晋级或性能结论**。
>
> **入口关系：** 现状叙述见 [`PROJECT_STATUS.md`](PROJECT_STATUS.md)；被删文档的去处与逐字节取回见 [`DOC_HISTORY.md`](DOC_HISTORY.md)。

## 1. 三层身份与引用规范

引用任何 PixelBridge 结论时必须同时给出三层身份，缺一即视为不可核对：

| 层 | 含义 | 取法 |
| --- | --- | --- |
| 源码身份 | 结论所依据的代码版本 | `git log --oneline -- libs apps tests tools CMakeLists.txt`；或封存记录的 `gitCommit` |
| 包/构建身份 | 实际跑的二进制 | 封存包内 `SHA256SUMS.txt`、`.seal.json`、`package-manifest.json`、`PixelBridge.ApplicationBuildIdentity.1` |
| 会话身份 | 具体一次运行 | `SessionTag`、`SessionId`、Sender/Receiver `RunId`、`events.jsonl` |

**注意：** `--measurement-build-identity` 的 `baseCommit` 与 `sourceFingerprintSha256` 是**编译期注入**，只有重新构建后才反映新 HEAD。旧交付包、录像、raw 像素与实验封存目录一律按**封存时记录**的 build 身份解释；若要以当前源码身份作为新对照基线，必须重新 freeze、build、test、package、seal。

## 2. 产品合同与实验身份

| 身份 | `VisualProfileId` | layout | 画布 | 状态 |
| --- | --- | --- | --- | --- |
| `PB-Unified-SC6-V3` | `0x5042554E49534333`（= 5783278666223141683） | `10` | 1920x1080 物理像素 | **现行产品合同** |
| `PB-Experimental-BlankControl-1` | `0x504242414E443031` | `11` | 同主区 + 空白带补充控制 | 实验身份，**未进正式 catalog**，非产品合同；2026-09-11 起以显式 `--profile unified-bands` opt-in 进入产品二进制（见 §2.1） |

| 项 | 值 |
| --- | --- |
| 规范 Profile JSON SHA-256 | `312c7832854f8710719ee2d0e44e920e7270b48638044eaa4b6af9ee24b1cb8b` |

`0x5042554E49534333` 与 G21 豁免记录内的十进制值一致（本轮核对）。

## 2.1 实验身份 opt-in 集成与右屏实机冒烟（2026-09-11，Step9）

| 项 | 值 |
| --- | --- |
| 源码身份 | 提交 `da70009`/`4c977cc`/`619455d`/`1fbddb6`/`ee1b25d`（`ee1b25d` 为实机证据之后补录的身份修复链：`ResolveLocalDesktopBinding`、GPU `ParseBinding` 等实验对分支；`163/163` 非交互 CTest） |
| 实机构建身份 | 远程 Encoder `gitCommit=619455d`（编码端报告）；本机 Decoder 同工作树构建（未含当时未提交的修复链） |
| 证据根 | 本机会话目录 `<PBLine root>\`（`session-log.md` §13、`bands-smoke\run7`、`evidence\bands06-encoder\`、`evidence\bands03-local\`）；未封存为 artifacts |
| 链路 | SENDER-LAPTOP Encoder `--profile unified-bands` → UnknownThirdParty 远控查看器（letterbox scale≈1.2604/1.2611）→ RECEIVER-DESKTOP 产品 Decoder CLI `--profile unified-bands --roi 2560 0 5120 1440` |
| 终态 | 1 MiB：`Completed`、exit 0、三方 SHA-256 `37f99176…` 一致（源 manifest=发送端 poststop=本地发布文件）、wire BLAKE3 `e338ad6d…` 双端一致、Bootstrap 118 接纳/0 拒、补充带 236 试/188 接纳（48 拒全为 AmbiguousCell）、801 传输块、9.14s |
| 不得越界 | 冒烟单样本、正确性验证；**不构成吞吐、晋级、认证或产品合同变更结论**；实验身份仍未进 catalog |

## 3. Gate 证据索引（G00-G22）

G00-G22 的**逐项验收条款与实现史**保留在 [`UNIFIED_VISUAL_LARGE_FILE_IMPLEMENTATION_ROADMAP.md`](UNIFIED_VISUAL_LARGE_FILE_IMPLEMENTATION_ROADMAP.md) §5（G00 起于该文件第 289 行，G22 起于第 922 行）。本索引只登记**有独立证据根或独立终态字段**的 Gate；其余 Gate 不在此重述，避免复制两份可能漂移的状态。

| Gate | 记录所在 | 关键事实 | 不得越界 |
| --- | --- | --- | --- |
| G00-G03、G05-G14 | roadmap §5 对应小节 | 协议/Descriptor/流式/journal/CPU oracle/D3D11 demod/Data Window/WGC 收敛 | 逐 Gate 结论只在 roadmap 内；**本索引未在本轮重新核实** |
| G04 | CP-A headless 检查点（2026-09-03） | 9 个 case 覆盖 Descriptor/FEC/Receiver/journal/storage/publish 无屏幕闭环 | 原文 `visualChainCovered=false`，**不得**推出视觉链已覆盖 |
| G15 / G16 | Encoder / Decoder Qt 产品收敛（2026-09-04） | 双端产品工作流 | 数值不代表 SC6-V3/layout 10 现网；当时未跑实屏/远程/大文件 |
| G17 | 遥测与运行报告 | `PixelBridge.RunReport.3` 字段与 goodput 口径冻结，见 [`UNIFIED_TELEMETRY_REPORT.md`](UNIFIED_TELEMETRY_REPORT.md) | Present/callback/配置 FPS **不是** goodput |
| G18 | 进程级故障注入 | 256 MiB、9 个终止点（批准的无像素 headless） | **不是**现场认证 |
| G19 | 大文件能力 | 20 GiB + 64 KiB、2,561 Segment、外部双摘要 | 能力边界，**不是**吞吐结论 |
| G20 | 历史本地发布门（提交 `e0729b2`） | LC4/layout 8、0.75x、31-slot，218/218 Release CTest | 该物理层已被后续远控失真证伪，**不可**用于任何 SC6-V3 声明 |
| G21 | 见 §4 | 1 GiB 真像素恢复 + 跨机双摘要；**原始性能门 FAIL** | 见 §4.3 |
| G22 | 见 §5.1 | `PASS_LOCAL_CANDIDATE`，构建 `3a840a2` | 本地候选，非现场/远程认证 |
| G22 追加：Encoder 无 DXGI-output 兼容 | 见 §5.2 | `LOCAL_REGRESSION_PASS / CITRIX_FIELD_PENDING` | 本机是 mapped hardware，**不等于** Citrix 现场通过 |

## 4. G21 真实远程像素链（1 GiB）

### 4.1 会话与结果身份

| 项 | 值 |
| --- | --- |
| 证据根 | `artifacts/g21-remote-1gib-2026-09-07/` |
| 豁免记录 | `g21-single-run-waiver-132e1a54d2134d2e828593dbeff210da.json`（2,167 B）✅ `d45e9b4ecebc50fdee65e782835a7edb6aabe609c2581e463ea4e1fde01c78da` |
| 跨主机审计 | `remote-run-cross-host-audit-132e1a54d2134d2e828593dbeff210da.json`（16,395 B）✅ `23d45347a42e923148ab3975ebef5277057d4895bd731a083209d8df24a423eb` |
| 终态快照 | `g21-final-status-*.json` `e7ec3b0851e8861207b768c627028d51340082131819504aab1158fc154eb824` |
| 其他封存件 | `README.md`、`PACKAGE_VERIFICATION.json`、`PREPARATION_MANIFEST.json`、5 个 v3/v4/v5 zip |
| Sender RunId | `132e1a54d2134d2e828593dbeff210da` |
| Receiver RunId | `2c4f578663c0b3762bb0396acb2021ab` |
| SessionId / SessionTag | `daba04b1c7c8c22a31604ea68dd61f8f` / `15447616161310190557` |
| 文件字节 | `1073741824`（1 GiB） |
| 源文件 SHA-256 / BLAKE3 | `e6ec3a7f5643f7b04ca5b90ce9510fb8388410b562b329fb70fe4cb837a0d323` / `db460e2c8a260f885f3a8a1b0d4d47a5d04f9d74e648c8ce4a44626b03185063` |

### 4.2 交付包身份（`artifacts/g21-delivery-2026-09-07/`）

| 包 | 字节 | SHA-256 |
| --- | --- | --- |
| `PixelBridge-G21-RemoteEncoder-6e90643-15Hz-v2.zip` | 26,166,428 | ✅ `018abb6c7ccd8dd83c42dcfb31dcb2da7f5ca354d01ea9b5bce04ca1210d1451` |
| `PixelBridge-G21-LocalReceiver-6e90643-15Hz-v2.zip` | 1,024,293 | ✅ `fc5fbd863b586036925d7326e9edfafb22fbb4181b7374523c435b58c6ab5760` |

构建标识 `6e90643`，15 Hz 配置。

### 4.3 必须随结论一起引用的字段（失败与豁免）

| 字段 | 值 | 含义 |
| --- | --- | --- |
| `decision` | `SingleRunExplicitWaiver` | 用户对该次会话的显式豁免 |
| `selectedOption` | 仅本次明确豁免 | 不扩展、不建立先例 |
| `functionalRemoteOneGiBPassed` | `true` | 功能层：1 GiB 真像素恢复通过 |
| `eventualRecoveryPassed` | `true` | 最终恢复成功 |
| `strictPass0ZeroPressurePassed` | `false` | 严格 Pass-0 零压力**未**通过 |
| `verifiedEncodedBytesPerUniqueFrame` | `8626.510998634209` | 实测每唯一帧有效字节 |
| `hardThreshold` | `16384.0` | 16 KiB 硬门限 |
| `hard16KiBFrameMetricPassed` | `false` | **原始性能门 FAIL** |
| `rawReceiverGateExit` | `1` | 原始接收门非零退出 |
| 终态 | `PASS_WITH_SINGLE_RUN_USER_WAIVER` | 只在功能层成立 |

豁免记录内的 `nonPrecedent` 明确写入：`rawHardGateResultRemainsFailed`、`future16KiBHardGateRemainsInForce`、`doesNotCertifyG22`。任何引用 G21 的表述都必须同时给出上面这组字段。

## 5. G22 双端 GUI 与 Encoder 显示兼容

### 5.1 G22 发布候选包（`artifacts/g22-delivery-2026-09-07/SHA256SUMS.txt`）

| 路径 | SHA-256 |
| --- | --- |
| `final-package/PB-Unified-B-3a840a20-6a2a1131.zip` | `8cf089ed624bb9bb44c085493d32d6d99f1ce599729561ba569824db2c7269b3` |
| `final-package/PB-Unified-B-3a840a20-6a2a1131.seal.json` | `9979a3a0dba1d9330401c6117a1f4fb733d52907985767dfda001351dc8b61a6` |
| `final-package/PB-Unified-B-3a840a20-6a2a1131/package-manifest.json` | `3f35b8893a497f2a08bdeca78f30a782ca9e6d6dc63049dd4c84e5acba8751ef` |
| `.../Encoder/PixelBridgeEncoder.exe` | `2c8f797df8397016a733124e642eb4bc72a28430110908f14546f50cde5c6e34` |
| `.../Decoder/PixelBridgeDecoder.exe` | `a011b8b6759a9fd446e1d42265ee77dfd2218ceba314b2e41d1f733c65e3043d` |

构建号 `3a840a2`；终态 `PASS_LOCAL_CANDIDATE`；GUI 交互合同见 [`UNIFIED_G22_GUI_RELEASE.md`](UNIFIED_G22_GUI_RELEASE.md)。

### 5.2 Encoder 无 DXGI-output 兼容候选（`artifacts/g22-encoder-compat-20260907/SHA256SUMS.txt`）

| 路径 | SHA-256 |
| --- | --- |
| `package/PB-Unified-E-df2bcfbd-47403e4f.zip` | `cead75ef6d14d07c33da580fbd35db2af3fb2ff1262404ecb06ffec2d9e53d29` |
| `package/PB-Unified-E-df2bcfbd-47403e4f.seal.json` | `06bc3074967ba45f8abbd32215af2fbf970e974457542e741553a7b88b7bf103` |
| `package/PB-Unified-E-df2bcfbd-47403e4f/package-manifest.json` | `dcc7ca9ccb6ca4b5c83cdc81f8d48e228fc98abe38d827274350e4f66d666966` |
| `package/PB-Unified-E-df2bcfbd-47403e4f/PixelBridgeEncoder.exe`（1,285,120 B） | ✅ `377cc672f9e9d2dfde2c5ee9b1ddb45c9cc97440d8fad03b731a90ffc4f5490b` |
| `native-frozen/source-1MB.bin` | ✅ `0257ab7fb5cb4a8b32052cc6f835a8af77b13e168d40fa760fa9bea7c056cff7` |
| `native-frozen/output/source-1MB.bin` | ✅ `0257ab7fb5cb4a8b32052cc6f835a8af77b13e168d40fa760fa9bea7c056cff7`（与输入同摘要） |

`native-frozen/summary.json`（`PixelBridge.GuiNativeEndToEnd.1`，`passed=true`，BLAKE3 `4efef186a52ee38600a9df5b6517597ccaf046f62693c2f1761923506c5c8585` ✅）内记：`payloadChannel="Actual right-monitor pixels only"`、`automaticInputEvents=false`、`gitCommit=df2bcfbd1dfbcc648a2d44f75bf1037c40c9dccd`、双端 exit code 0。

**边界（该 summary 自证）：** `physicalEscKeyTested=false`、`physicalRoiDragTested=false`；本机为 mapped hardware。整体状态停在 `LOCAL_REGRESSION_PASS / CITRIX_FIELD_PENDING`，关闭条件是**在原 Citrix 会话**完整解压包、观察数据帧并由人工按 Esc。

## 6. 非本机吞吐优化线（2026-09-08 至 2026-09-09）——**已停止**

**2026-09-09 由用户主动停止**：非技术阻塞、非任务完成。恢复只在用户重新开启目标后进行。该线的 16 篇执行/诊断记录已 `git rm`，逐篇 blob 见 [`DOC_HISTORY.md`](DOC_HISTORY.md)；本节只登记**已核实数字与其证据根**，供后续引用时不致丢失口径。

### 6.1 四轮归因（A1/B1/B2/A2）

主时长定义为 `finalReopenVerified - startAccepted`。数值取自停止前的归因记录（`REMOTE_THROUGHPUT_ATTRIBUTION_2026-09-08.md`、`REMOTE_CONTROL_PHASE_AB_2026-09-08.md` §表，本轮逐值复核）：

| 组 | 主时长 s | VerifiedRawGoodput B/s | VerifiedEncodedGoodput B/s | 记录注记 |
| --- | --- | --- | --- | --- |
| A1 | 238.3155474 | 170015.718412 | 169566.263892 | 原 Sender wrapper 缺失；实际延后 source-after 审计 |
| B1 | 211.5549573 | 191521.813136 | 191015.504982 | 本机 wrapper 缺失；其余核心证据完整 |
| B2 | 212.4771320 | 190690.586882 | 190186.476162 | 两端正常收尾、600 s 内、无预算告警 |
| A2 | 205.0048921 | 197641.083512 | 197118.598420 | **进程残留后人工结束；远端退出/预算失败** |

| 派生口径 | 值 | 引用限制 |
| --- | --- | --- |
| 原组 A→B 单组 | 省 26.7605901 s，耗时降 11.2291%，有效吞吐 **+12.6495%** | 单组观察，不证明稳定或因果 |
| 反序接收组 | B 吞吐 **−3.5167%**（比 A 多用 7.4722399 s） | 与原组方向相反 |
| 四组全纳入描述性汇总 | B **+4.5488%** | 总字节/总时间的描述性汇总，**不得**包装成稳定增益 |

**结论字段：** 不存在完全合规的反序配对（A2 人工结束 + exit1/预算告警），因此"稳定增益已证明"不成立；该线终态为归因完成但**判据未确立**。

### 6.2 夜间记录中被引用最多的几节

| 节 | 事实 | 证据根 |
| --- | --- | --- |
| §17 Bootstrap 扫描游标 | 保留全部正确性门的前提下，顺序 BGRA 游标中位成本下降 **12.24% / 9.55%**（均值 17.06707→14.91484 ms、21.65853→19.64895 ms） | `artifacts/remote-cursor-screen-20260909-run01` |
| §18 QC-LDPC 行归一化复用 | 每组入口均值 `1.8983978 → 1.4471265 ms`，降 **23.7712%**；675 对输出整码字、成功/失败、错误码/详情、迭代数**完全一致** | 见该节原文（文档已删，按 blob 取回） |
| §19 新定位 + 纠错组合的实际 WGC 整文件 | 8,454,144 B（8 MiB + 64 KiB）；`startAccepted→finalReopenVerified` = **39.4210093 s**；本机 VerifiedEncoded/RawGoodput **214457.8272 B/s**；Sender 15.0026845 逻辑 FPS；Receiver `UniqueVisualFPS` **14.9921237**（619 观察 / 464 unique，Outer 不同块 6435）；9285 个已评估槽全通过、**FEC 迭代均为 0**；终态 `ACTUAL_WGC_INTEGRATION_PASS` | `artifacts/remote-receiver-cost-screen-20260909-run01`；运行根 `artifacts/fc0909-s01`、`artifacts/fc0909-r01` |
| §21 layout 11 原始参考链 | 实验身份 `0x504242414E443031`/layout 11；同主区 30 帧 × 5 条件经原 ReceiverIngress/Storage 完成 262144 B 整文件、外部双摘要、150 条主区记录一致 | `artifacts/remote-blank-fullframe-20260909-run01` |
| §22 空白带固定 codec A/B | 两组均 `NOT_RECOVERED`；`LocatorFailure`/`CanvasClipped`：A **15/15**、B **0/30**；补充区 60 patch 独立硬判决 21 条码字精确、最大 6 byte 错误，但原整条置信度规则仅 3 条通过 | `artifacts/remote-blank-codec-20260909-run01` |
| §23 收尾 | 用户停止时 4/3 适配仅**只读准备**，未实现、未构建、未运行 | — |

§19 的源文件 SHA-256 为 `694f8a5ef0c983e40c890477e8bcf0091094d493d1c9445afcfa6d4d1f70c52c`；该目录内另有 `EARLY_AUDIT_NOTE.json` 记录了当时的操作时序错误。

### 6.3 引用这些数字时的强制限制

1. §18 的 23.7712% 是**固定语料 FEC 阶段 CPU 成本**，原文明写"不是 23.8% 的非本机吞吐增益"。
2. §17 的 12.24%/9.55% 是 Bootstrap 扫描阶段耗时，同样不是端到端吞吐百分比。
3. §19 是**本机** WGC 组合通过，原文注明非本机提速仍未确立。
4. §21 **不是**原 DecoderRuntime/GPU/codec/实屏或远控提速证明；控制槽未释放。
5. §22 终态 `FIXED_CODEC_AB_DIAGNOSTIC_COMPLETE；BOTH_NOT_RECOVERED；REMOTE_GAIN_NOT_ESTABLISHED`；`CanvasClipped` 来自 `ResolveUnifiedSamplingGeometryInternal` 的拒绝，**不得**扩写为真实 ROI 被裁剪。
6. 停止时路线图状态：Step5 `IN_PROGRESS／未晋级`，Step6/Step9 `NOT_STARTED`；未晋级任何默认值，未改主区控制、layout 10、摘要、安全发布或 reopen 规则。

## 7. RemoteVisual 历史路线（已被统一产品路线取代）

| 项 | 终态 | 备注 |
| --- | --- | --- |
| Step 20 真实远控试点 | `MANUAL-GATE` | **从未 DONE** |
| Step 20 单屏诊断 | `DiagnosticPixelLinkPass` | 诊断级，非产品认证 |
| Step 21 provider-generic 矩阵 | `DONE`，但扩展矩阵 `NOT_EXECUTED` | `formalStep21Accepted=false` |
| Step 22 重复恢复 | `executedRunCount=0` | 无执行样本 |
| Step 06 corpus | 语料基线 `1936c02` | 历史 |
| Step 09 DYNAMIC_RDP_PILOT | PASS（120 s 动态 RDP + 两次离线 Replay） | 历史 |
| `P1_5_REMOTEVISUAL_HARDENING_CHECKPOINT` | tag `phase1-gate-pass`，`RemoteVisualSmokePass=NotEvaluated` | 历史 |
| Step 16 遥测口径 | `RunReport.2` | 已被 `RunReport.3` 取代 |

## 8. Citrix 现场诊断（G22 追加，仍属现场未通过）

| 项 | 值 |
| --- | --- |
| 证据根 | `artifacts/g22-citrix-field-report-20260907/` |
| 截图 | `user-report-screenshot.png` `370a52b6ce1619901e108375408b5582ea482b9ed24afa6bf29da6d55c759081` |
| 转录字段 | `selected-fields-transcribed.json` `5379f3270dd1ad1ca6725b422e003ae7ff0db91b41d73ec126bf2139a8fc7642` |
| 证据种类 | `user-supplied-screenshot`，`extraction="manual transcription of selected visible fields; not the original JSON"`，`originalJsonFileAvailable=false` |
| 诊断基线 | `ddb2fa2fc366bad6ca3165cf4f28d869b50f2201`；收口提交 `75eccbc493a9a7956e720da12c094bff42c8672e`（`scope="documentation and screenshot-derived evidence only"`，`productSourceModified=false`，`productRebuilt=false`，`displayOrInputAutomation=false`，`repairScopeAwaitingUserConfirmation=true`） |

关键观测（转录值）：`productionMonitorCatalog.succeeded=false`、`monitorCount=0`、`nativeError=1168`（`ERROR_NOT_FOUND`）；Win32 枚举成功，两个监视器描述均为 `Citrix Display Only Adapter`（`\\.\DISPLAY2`、`\\.\DISPLAY3`）；`dxgiFactoryHresult=0`，但 `NVIDIA GRID K220Q` 与 `Microsoft Basic Render Driver` 两个 adapter 的 `outputCount` 均为 `0`，`outputEnumerationEndHresult=-2005270526`（`0x887A0002` = `DXGI_ERROR_NOT_FOUND`）。

原文 `conclusion`：Win32 监视器存在，但 catalog 需要 DXGI output 映射，而两个 adapter 都不暴露任何 output；**Device/swap-chain/Present 能力未测试**。另有 `artifacts/g22-citrix-diagnostics-20260907/` 为本机侧诊断目录。

**边界：** 这条证据是**截图人工转录**，不是原始 JSON；可用于解释故障机制，不可用于任何"已修复/已验证"表述。§5.2 的兼容候选正停在 `CITRIX_FIELD_PENDING`。

## 9. 不得被本次整理抹掉的事实

以下每一条在本次文档清理后仍然成立，任何后续文档、报告或提交信息都不得弱化或删除：

1. **G21 原始性能门 FAIL**：`verifiedEncodedBytesPerUniqueFrame=8626.510998634209` < `hardThreshold=16384.0`，`strictPass0ZeroPressurePassed=false`，`rawReceiverGateExit=1`；用户豁免**仅限该次会话**，`nonPrecedent` 三项（`rawHardGateResultRemainsFailed`、`future16KiBHardGateRemainsInForce`、`doesNotCertifyG22`）。
2. **非本机整文件吞吐提升未确立**，该线未晋级任何默认值，未改主区控制、layout 10、摘要、安全发布或 reopen 规则；2026-09-09 由用户主动停止。
3. **Citrix 现场实屏未验证**：Encoder 兼容停在 `LOCAL_REGRESSION_PASS / CITRIX_FIELD_PENDING`；本机 summary 记 `physicalEscKeyTested=false`、`physicalRoiDragTested=false`。
4. **Step1 完整现场矩阵、Step2/Step3 现场门均 `NOT_RUN`**；Step1/Step2 本地状态 `PREPARED/PARTIAL`（`fieldStatus=NOT_RUN`）。
5. **四轮归因不存在完全合规的反序配对**：`+12.6495%` 只是原组单组观察，反序组为 `−3.5167%`，四组描述性汇总 `+4.5488%`。
6. §18 的 **23.7712%** 与 §17 的 **12.24% / 9.55%** 是 CPU/Bootstrap 阶段耗时，**不是**远控吞吐百分比。
7. §22 空白带固定 codec A/B **两组均 `NOT_RECOVERED`**；`CanvasClipped` 是 `ResolveUnifiedSamplingGeometryInternal` 的拒绝，不是真实 ROI 被裁剪。
8. **G20 的 LC4/layout 8、0.75x、31-slot、218/218 Release CTest 不可用于任何 SC6-V3 声明**（该物理层已被后续远控失真证伪）。
9. **RemoteVisual 历史路线未收口**：Step 20 从未 DONE、Step 21 `formalStep21Accepted=false`、Step 22 `executedRunCount=0`。
10. **任何本地、offscreen、artifact-only、录像、WARP、CPU reference、receiver-only 结果都不是现场认证。**
11. Phase-0 时代的 37-byte `SessionDescriptor` 现在是**必须拒绝**的 fixture，不能作为"旧格式仍被接受"的依据。
12. **build identity 为编译期注入**：`baseCommit`/`sourceFingerprintSha256` 不随提交自动更新，旧包与旧封存目录不代表当前源码。
13. **G04 CP-A 的 `visualChainCovered=false`**：无屏幕闭环证据不得推出视觉链覆盖。
14. `docs/PHASE1_GATE_REPORT.md` 始终保持**未跟踪且字节不变**（SHA-256 `076ef4c9b9f89eabccd323dbe4bffc4dc125ddaf96e6ee437d2cf5b1b1cea306`），从未被 add/commit/修改/删除。

## 10. 未关闭事项（登记，不是执行授权）

| 事项 | 现状 | 关闭所需 |
| --- | --- | --- |
| 非本机整文件吞吐提升 | 未确立；用户主动停止 | 用户重新开启目标，从恢复入口续做 |
| Citrix Encoder 显示兼容 | `CITRIX_FIELD_PENDING` | 原 Citrix 会话完整解压包、观察数据帧、人工按 Esc |
| Step1 完整现场矩阵 / Step2-Step3 现场门 | `NOT_RUN` | 现场窗口 + 人工操作（右屏，不用输入自动化） |
| 4/3 显示采样适配（夜间 §23） | 仅只读核对 | 新授权 + 新 root/build/output + 有界验证 |
| 空白带补充控制（layout 11） | CPU 参考通过；真实 codec 未恢复；控制槽未释放 | 独立显示采样变量 + 明确擦除预算，不降门限 |
| 以当前源码身份建立对照基线 | 未完成 | 重新 freeze、build、test、package、seal |
| 仓库根 `display_probe.obj`（366,491 B，被 `*.obj` 忽略） | 未删除 | 需用户确认 |

## 11. 复核命令

```powershell
# 受保护文件字节未变
Get-FileHash docs/PHASE1_GATE_REPORT.md -Algorithm SHA256

# 封存包身份
Get-FileHash artifacts/g21-delivery-2026-09-07/*.zip -Algorithm SHA256
Get-Content artifacts/g22-delivery-2026-09-07/SHA256SUMS.txt
Get-Content artifacts/g22-encoder-compat-20260907/SHA256SUMS.txt

# G21 豁免与审计原文
Get-Content artifacts/g21-remote-1gib-2026-09-07/g21-single-run-waiver-132e1a54d2134d2e828593dbeff210da.json

# 源码身份（判断"最后一次改代码"用这条，而不是 HEAD）
git log --oneline -5 -- libs apps tests tools CMakeLists.txt

# 取回被删除文档的逐字节原文（两种方式等价）
git cat-file blob <blob-sha1>
git show 7e88a69:docs/<FILE>.md
```

被删文档的路径、原状态与 blob SHA-1 全表见 [`DOC_HISTORY.md`](DOC_HISTORY.md)。
