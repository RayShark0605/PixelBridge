# PixelBridge 文档整理与历史恢复记录

> **文档性质：** 本文件登记 2026-09-09 那次 `docs/` 结构性清理的**完整删除清单与逐字节恢复方式**，以及整理后仍在工作树的文档清单。它是"哪些文档没了、为什么没、怎么拿回来"的唯一入口。
>
> **不做的事：** 本文件不改写任何被删文档的结论、数值或失败事实。需要原文时用下面的取回命令，**不要凭记忆重述**。
>
> **本轮整理边界：** 纯文档与 Git 记账。整理过程中未构建、未测试、未实屏、未远程，未修改任何源码字节或封存证据，因此不构成任何验证、晋级或性能结论。现状叙述见 [`PROJECT_STATUS.md`](PROJECT_STATUS.md)，证据索引见 [`EVIDENCE_INDEX.md`](EVIDENCE_INDEX.md)。

## 1. 删除机制与取回方式

**机制：** 全部用 `git rm` 从工作树移除，**不建 `docs/archive/` 目录**。文件内容仍完整保存在 Git 对象库里，按 blob SHA-1 可逐字节还原，且与删除前完全一致（Git 对象不可变）。

**为什么不留 archive 目录：** 归档目录会让"当前规范"和"历史材料"再次混在同一层，正是本次要解决的问题；而 Git 历史本身就是不可变归档，检索成本更低。

### 取回命令

```powershell
# 1) 按 blob 直接取回内容（推荐：不碰工作树，重定向即可）
git cat-file blob <blob-sha1> > Restored.md

# 2) 按"清理前的最后一次提交 + 路径"取回
git show 7e88a69:docs/REMOTE_STEP1_EXECUTION_2026-09-08.md

# 3) 清理前 docs/ 全量清单（路径 / blob / 字节）
git ls-tree -r 7e88a69 --format="%(objectname) %(objectsize) %(path)" -- docs/

# 4) 某个文档的完整变更史
git log --follow --oneline -- docs/UNIFIED_REMOTE_GATE.md
```

`7e88a69` 是**删除发生之前**的 `docs/` 完整状态（最后一次改动源码的提交是 `46a072b`）。非 ASCII 文件名在 PowerShell 里请配合 `git -c core.quotepath=false` 查看。

**恢复建议：** 需要历史文档时，优先只取回内容到临时位置阅读；不要直接把它放回 `docs/`，除非同时决定它重新成为规范文档并更新索引。

## 2. 整理后仍在工作树的文档（15 篇）

这 15 篇是本次整理判定的**现行权威与规范**，全部保持 Git 跟踪状态。表内 `blob 字节` 是**整理前** `git ls-tree` 记录的 blob 大小（Git 存 LF，磁盘工作树因 CRLF 可能更大）。唯一例外是 `README.md`：本轮被重写为薄索引，因此它的 blob 会变（磁盘 8,258 B），表内保留整理前数值仅作基线。

| 文档 | blob SHA-1 | blob 字节 | 职责 |
| --- | --- | --- | --- |
| `docs/README.md` | `483333959f077a2b8c9d32a881292b461989b833` | 35228 | `docs/` 薄索引与阅读路径（本轮重写为 8258 B） |
| `docs/PixelBridge_最终技术路线与总体设计.md` | `6a1807d2d328a43f5fc76b29c3b24a94086d1c39` | 180510 | canonical 架构与协议语义（改协议前必读） |
| `docs/UNIFIED_VISUAL_LARGE_FILE_IMPLEMENTATION_ROADMAP.md` | `e2c5bdec54474816451218b3b0f66cc1e312c2fa` | 147829 | 统一大文件产品合同（`PB-Unified-SC6-V3`）与实现史 |
| `docs/PROTOCOL_1_DESCRIPTOR_SCHEMA.md` | `c574bf9a61f5847a99047e1a26996bb079c410b2` | 12237 | 协议 1 描述符字节级 schema |
| `docs/UNIFIED_TELEMETRY_REPORT.md` | `49b4c8dd1588c7788570a501b5ed7afccddae114` | 15922 | goodput/遥测真值口径与 RunReport schema |
| `docs/UNIFIED_G22_GUI_RELEASE.md` | `d335c38d7d77f5506befeeabc686cb93ce89bf7a` | 29147 | G22 GUI 交互合同与发布流程 |
| `docs/CURRENT_RUNTIME_OPTION_INVENTORY.md` | `8e4579e0bdf30f83a0be40691d8053fdda02113a` | 23938 | 运行时选项/参数清单与默认值 |
| `docs/UNIFIED_USER_GUIDE.md` | `6890817d595d56902d979d2131d889cc6ed1e06a` | 6858 | 面向操作者的使用流程 |
| `docs/REFERENCE_RASTER.md` | `dce7ce1d7661f34af86e4e787783ccdc3afda714` | 29480 | 光栅化参考实现与像素语义 |
| `docs/PRESENTATION.md` | `72feb647d63015d13f9682d794117c04501724c2` | 19889 | 呈现/D3D11 数据窗口与节拍合同 |
| `docs/SCREEN_REGION.md` | `94a2e8bb259bb2513ab378741323365b7f8d4873` | 8226 | 屏幕区域选择与物理像素/DPI 语义 |
| `docs/PBScreenCaptureWgc.md` | `faea502e8b090fb9eda37754cf427dcf7ad1b835` | 13156 | WGC 捕获生命周期与 lease 规则 |
| `docs/ENCODER_STREAMING_CAROUSEL.md` | `516b313164727a91289a809be6ff9d53fc8fd582` | 14427 | 发送端流式 Carousel 与有界内存 |
| `docs/DECODER_RESUMABLE_RECOVERY.md` | `fd61d98f813bce58581a79694022b4831da8b6de` | 16458 | 接收端可恢复恢复与描述符冲突规则 |
| `docs/GOLDEN_VECTOR_HARNESS.md` | `170954f14a2a4d1c706fc88f07c0ffd2c9c61edd` | 8805 | Golden Vector 工具链与 wire 兼容校验合同 |

另外三篇本轮新增文档不在上表（它们在整理前不存在，因此没有旧 blob）：`docs/PROJECT_STATUS.md`（现状唯一入口）、`docs/EVIDENCE_INDEX.md`（证据索引）、本文件。

未跟踪文件 `docs/PHASE1_GATE_REPORT.md` 不在上表：它是**保护文件**，永不加入 Git，见第 5 节维护规则。

## 3. 被删除的 64 篇文档（按原用途分组）

删除理由统一为：历史/冻结/一次性记录，或结论已被保留文档收编。分组只是为了让"哪一类材料没了"可检索，不代表质量判断。每篇的 blob SHA-1 都能按第 1 节命令逐字节还原原文。

### 3.1 A 组：与保留规范重叠的一次性验证记录（3 篇）

| 文档 | blob SHA-1 | blob 字节 | 原状态与删除理由 |
| --- | --- | --- | --- |
| `docs/PBScreenCaptureWgc_validation.md` | `e049c41eb7cfbb71589c58755d70740bdbd2c2b7` | 9582 | WGC 捕获一次性验证记录；捕获规范以 PBScreenCaptureWgc.md 为准 |
| `docs/PRESENTATION_VALIDATION.md` | `35d3b886d9c1df2bdea25887e702713b74847a39` | 12493 | D3D11 数据窗口与 PBPresentTiming 验证记录；呈现规范以 PRESENTATION.md 为准 |
| `docs/SCREEN_REGION_VALIDATION.md` | `ee5d820469fc3d8d9be657a300718f4b9df48773` | 14170 | PBScreenRegion 验证记录；区域语义以 SCREEN_REGION.md 为准 |

### 3.2 B 组：Phase 0 与早期调制/GUI 实验史（9 篇）

| 文档 | blob SHA-1 | blob 字节 | 原状态与删除理由 |
| --- | --- | --- | --- |
| `docs/CAPTURE_BOOTSTRAP_IMPLEMENTATION.md` | `6e1a4045288f6026af4e8b70344777ecdb003caa` | 44119 | Phase 0：DXGI/CaptureNormalize/LocalDesktop bootstrap 实现记录 |
| `docs/DESKTOP_LEVELS_REFERENCE.md` | `0497f3625e9885464f7e88ced252a33924d9dbb5` | 27471 | PB-Mod-DesktopLevels-X1 CPU/reference 与 LocalDesktop SDR 测量基线（旧调制实验） |
| `docs/GUI_PHASE1_5.md` | `1ce6633058008b775e85c9a0ea2ccecd3cd9ef25` | 14572 | Windows GUI Phase 1.5 记录；GUI 合同已在 UNIFIED_G22_GUI_RELEASE.md |
| `docs/LOCAL_DESKTOP_BOOTSTRAP.md` | `e5356310eef3be7019e458f19a5333d95ab9129c` | 19720 | PB-LocalDesktopBootstrap-X1 bootstrap 实验记录 |
| `docs/P1_5_REMOTEVISUAL_HARDENING_CHECKPOINT.md` | `7751f8207a88b2ee64cd1afed7c8cad9861ccfb2` | 11450 | P1.5 RemoteVisual 加固检查点（旧里程碑快照） |
| `docs/PHASE0_GATE_REPORT.md` | `30af00d8d2c87c8006ab60efce3b74afea81f3a2` | 19998 | P0-15 Phase 0 终版 Gate 报告；build-phase0-gate-evidence/ 内 2 处链接仍指向它（冻结证据快照，故不修链接） |
| `docs/PHASE0_PROTOCOL_STATUS.md` | `2c14443fc139a59f97373159659ec97b072d7e71` | 33322 | Phase-0 协议实现状态；已被统一大文件路线取代 |
| `docs/SHAPE_CHROMA_D3D11_TELEMETRY_REPLAY.md` | `4b14ffc0b5f5c9e762c8f6955b7082d737ad59a2` | 9510 | ShapeChroma/D3D11 Demod/Telemetry/真实捕获回放 baseline（旧实验） |
| `docs/UNIFIED_VISUAL_CP_A_HEADLESS.md` | `716b824f6f275acd4e199f66db458f97056878b3` | 11266 | CP-A 多 Segment 无屏幕端到端检查点（统一路线早期） |

### 3.3 C 组：Unified G15-G20 阶段记录（5 篇）

| 文档 | blob SHA-1 | blob 字节 | 原状态与删除理由 |
| --- | --- | --- | --- |
| `docs/UNIFIED_DECODER_WORKFLOW.md` | `acba918898229002bcebb0fde598cc29223d2d70` | 11322 | G16 Decoder 产品工作流记录；操作流程以 UNIFIED_USER_GUIDE.md 为准 |
| `docs/UNIFIED_ENCODER_WORKFLOW.md` | `21773fe6958c28b6d358382552a54268531fd8f2` | 9141 | G15 Encoder 产品工作流记录；操作流程以 UNIFIED_USER_GUIDE.md 为准 |
| `docs/UNIFIED_LARGE_FILE_CAPABILITY.md` | `ac3e870c4ecd6ef929f38a6f01f9cf8732a441e3` | 12019 | G19 20 GiB+ headless 大文件能力记录；结论已归并进 UNIFIED_VISUAL_LARGE_FILE_IMPLEMENTATION_ROADMAP.md |
| `docs/UNIFIED_LOCAL_RELEASE_GATE.md` | `5912d571170b893e1b7a5657b1c997ee198fc1ab` | 97825 | G20 本地 Release Gate 进度与回归修复记录 |
| `docs/UNIFIED_PROCESS_RESTART_RECOVERY.md` | `ea4a06a00f9dad69ac107964c8651b0f37ef3d38` | 45774 | G18 256 MiB 真实进程终止与恢复记录；恢复机制以 DECODER_RESUMABLE_RECOVERY.md 为准 |

### 3.4 D 组：Unified G21 交付与真实远控验收（4 篇）

| 文档 | blob SHA-1 | blob 字节 | 原状态与删除理由 |
| --- | --- | --- | --- |
| `docs/UNIFIED_G21_DELIVERY_2026-09-07.md` | `6bcab42b825df0fcca091f374de677879e2dfd25` | 10283 | G21 交付与目录索引；交付包哈希见 EVIDENCE_INDEX.md 第 4 节 |
| `docs/UNIFIED_G21_EXECUTION_HANDOFF_2026-09-06.md` | `ce0444c63b51e36e3ba1cf64a0a7b3dadc0b4ae0` | 67561 | G21 执行交接（2026-09-06），一次性交接文档 |
| `docs/UNIFIED_G21_REMOTE_1GIB_RESULT_2026-09-07.md` | `6a2a4d2a500b86d5f728ef0ab3b3c53643014529` | 8505 | G21 真实远控 15 Hz / 1 GiB 终态与单次显式豁免；原始性能门未过的事实保留在 EVIDENCE_INDEX.md 第 4/9 节 |
| `docs/UNIFIED_REMOTE_GATE.md` | `0d134b09214b5d2c00b5f6ab2a25f385476995da` | 110584 | G21 真实远程像素链验收记录；终态见 EVIDENCE_INDEX.md 第 4 节 |

### 3.5 E 组：G22 交付与 Citrix 诊断（3 篇）

| 文档 | blob SHA-1 | blob 字节 | 原状态与删除理由 |
| --- | --- | --- | --- |
| `docs/UNIFIED_G22_CITRIX_DISPLAY_DIAGNOSTICS_2026-09-07.md` | `5783c6c203d328cac19a8f8949a199f6b30932b0` | 9806 | G22 Citrix 启动故障只读诊断交接；现场事实保留在 EVIDENCE_INDEX.md 第 8 节 |
| `docs/UNIFIED_G22_DELIVERY_2026-09-07.md` | `2f643e12e1e1358e0f0b5975b4cd29290eb45b9f` | 10354 | G22 最终 GUI 本地候选交付记录；交付身份见 EVIDENCE_INDEX.md 第 5 节 |
| `docs/UNIFIED_G22_ENCODER_DISPLAY_COMPAT_2026-09-07.md` | `b68d45ad90e5b4e8898113d4dbe406b3f5d4d01d` | 9863 | G22 Encoder 无 DXGI 输出兼容修复记录；LOCAL_REGRESSION_PASS / CITRIX_FIELD_PENDING 见 EVIDENCE_INDEX.md 第 5 节 |

### 3.6 F 组：RemoteVisual（LF4）通道史（22 篇）

| 文档 | blob SHA-1 | blob 字节 | 原状态与删除理由 |
| --- | --- | --- | --- |
| `docs/REMOTE_VISUAL_CHANNEL_MANIFEST.md` | `8fa2c1564b651982452773d3d5ae6d44f8b1902e` | 10058 | RemoteVisual Channel Manifest v1/v2（LF4 时代通道定义；已被统一 SC6 合同取代） |
| `docs/REMOTE_VISUAL_LOW_FPS_TECHNICAL_ROUTE.md` | `c284b3ee26ee15b160dcff03369c388c6ccbc906` | 166717 | RemoteVisual 低刷新率高密度传输技术路线（LF4 历史路线，非当前非本机吞吐路线） |
| `docs/REMOTE_VISUAL_STEP06_CORPUS.md` | `664628039acbc814e603e4d728f123bf58fb5a83` | 13556 | RemoteVisual Step 06 Impairment Corpus 记录 |
| `docs/REMOTE_VISUAL_STEP07_CALIBRATION.md` | `85368111b098704e2c3d43ef5dfb05638f0e2db2` | 13283 | RemoteVisual Step 07 Soft-Metric Calibration Gate 记录 |
| `docs/REMOTE_VISUAL_STEP08_GOLDEN.md` | `e16f0bca33549b7b2f465eaa46d4da07fa3258ee` | 11069 | RemoteVisual Step 08 LF4 Golden / Manifest Freeze 记录 |
| `docs/REMOTE_VISUAL_STEP09_DYNAMIC_RDP_PILOT.md` | `85e717a280cb58e7d5c6834fb4af04b5945f5df9` | 14679 | RemoteVisual Step 09 后置动态 LF4 RDP 现场 pilot 记录 |
| `docs/REMOTE_VISUAL_STEP09_ENCODER.md` | `548d565cc033476aa2c26d2240758643f2146b3c` | 14996 | RemoteVisual Step 09 LF4 D3D11 Encoder / Immutable Source 记录 |
| `docs/REMOTE_VISUAL_STEP10_D3D11_DEMOD.md` | `eb41c8d16446b7768e36856cee9434406618a3ed` | 15426 | RemoteVisual Step 10 LF4 D3D11 Scaled Walsh Demod 记录 |
| `docs/REMOTE_VISUAL_STEP11_GPU_PARITY.md` | `8297bfbe0e9823d831ad9a7d5dc97c84298dc044` | 15394 | RemoteVisual Step 11 CPU/WARP/Hardware GPU 真值一致性记录 |
| `docs/REMOTE_VISUAL_STEP12_CAPTURE_LIFETIME.md` | `ce9624f9fb24622943b52d2bfd9c6938cc702f04` | 15043 | RemoteVisual Step 12 CaptureNormalize/Geometry/Epoch/D3D 生命周期记录 |
| `docs/REMOTE_VISUAL_STEP13_TEMPORAL_ADMISSION.md` | `fb18f86720ad4a55f93adc3da75f3092f35d9d96` | 11964 | RemoteVisual Step 13 Temporal Admission / 有界队列记录 |
| `docs/REMOTE_VISUAL_STEP14_RECEIVER_ADMISSION.md` | `e0072d866f1a225c758850fa97dab7758c0572b0` | 10895 | RemoteVisual Step 14 四码字接收接纳记录 |
| `docs/REMOTE_VISUAL_STEP15_REPLAY_PRODUCTION.md` | `978fc1bba503351a5cd39d5430d82d6c61e44350` | 9137 | RemoteVisual Step 15 Replay v2 production live/offline 一致性记录 |
| `docs/REMOTE_VISUAL_STEP16_TELEMETRY_REPORT.md` | `1fdbbddcb8c1f7719740dbfaf6fc79a46c23d630` | 10847 | RemoteVisual Step 16 权威遥测、RunReport.2 与严格双端合并记录；现行 schema 见 UNIFIED_TELEMETRY_REPORT.md |
| `docs/REMOTE_VISUAL_STEP17_GUI_CLI_SCREEN_SAFETY.md` | `042e0fd3293546920c271136223ccae58cb6ece1` | 12402 | RemoteVisual Step 17 GUI/CLI 暴露与屏幕安全记录 |
| `docs/REMOTE_VISUAL_STEP18_DEPLOYMENT_REPRODUCIBILITY.md` | `5752257f306fb8d1e64eaeb5919c83faac6024de` | 10040 | RemoteVisual Step 18 便携包/source set/双端环境/RunId 部署闭环记录 |
| `docs/REMOTE_VISUAL_STEP19_LOCALDESKTOP_REGRESSION.md` | `636991667bf335b963aeb699f754878a0b500bd5` | 11961 | RemoteVisual Step 19 同提交 LocalDesktop 回归记录 |
| `docs/REMOTE_VISUAL_STEP20_REAL_REMOTE_PILOT.md` | `300d7bb59cc2221e73a3eece9a69b58434dfad52` | 17880 | RemoteVisual Step 20 真实双机 LF4 pilot 冻结执行与证据门禁记录；结论 MANUAL-GATE 见 EVIDENCE_INDEX.md 第 7 节 |
| `docs/REMOTE_VISUAL_STEP20_SINGLE_MONITOR_DIAGNOSTIC.md` | `e5728c31772e03c504afe95eef76fd4be448f8ca` | 15437 | RemoteVisual Step 20 单屏真实远控 LF4 诊断像素链证据 |
| `docs/REMOTE_VISUAL_STEP20_SINGLE_MONITOR_FILE_PILOT.md` | `a39b42dba13878539f4288a6e0ae85dca1b1eb09` | 12338 | RemoteVisual Step 20 单屏全屏发送端真实文件级 pilot 记录 |
| `docs/REMOTE_VISUAL_STEP21_PROVIDER_GENERIC_MATRIX.md` | `9795af37afc5710a54c75934bc2acfebcf9da23a` | 36629 | RemoteVisual Step 21 用户授权单次实机关闭记录；formalStep21Accepted=false 见 EVIDENCE_INDEX.md 第 7 节 |
| `docs/REMOTE_VISUAL_STEP22_REPEAT_RECOVERY.md` | `bb05b84eedbc4b34ebff113f2a89d01b96f5acf5` | 7027 | RemoteVisual Step 22 重复完整文件恢复验收记录；executedRunCount=0 见 EVIDENCE_INDEX.md 第 7 节 |

### 3.7 G 组：非本机吞吐优化线 2026-09-08/09（16 篇）

| 文档 | blob SHA-1 | blob 字节 | 原状态与删除理由 |
| --- | --- | --- | --- |
| `docs/REMOTE_CHANNEL_THROUGHPUT_OPTIMIZATION_ROADMAP.md` | `fccf346e48299e5a6c00a874d0b9e1ebfebee26d` | 56657 | 非本机（Citrix 类）吞吐优化路线图 Step1-Step10；该线已由用户主动停止，路线与停止状态见 PROJECT_STATUS.md 第 5.2 节 |
| `docs/REMOTE_CONTROL_PHASE_AB_2026-09-08.md` | `43c363f8f7b2f52064235b0b6b76d833ed7c4c7b` | 34869 | 周期控制相位 A/B 候选与单组现场结果 |
| `docs/REMOTE_FIXED_CODEC_OBSERVATION_CONTRACT_2026-09-08.md` | `ef20a0a5b9cb0b124ed28e4e6738265c266b3a79` | 16357 | 固定 codec 边缘观测合同设计审查与吞吐路线收束 |
| `docs/REMOTE_GEOMETRY_CODEC_ATTRIBUTION_2026-09-08.md` | `fc5cfd1734f3f098df62fe2389d2ed706c8359ab` | 14743 | Step3-B codec 几何归因与修复判据设计审查 |
| `docs/REMOTE_GEOMETRY_G1_EXECUTION_2026-09-08.md` | `0f904ca81a3ff9245df98e5e1fe3bc7407f1bf7d` | 11425 | 几何专项 G1 实施与结果；工具说明仍在 ../tools/PBUnifiedGeometryG1/README.md |
| `docs/REMOTE_GEOMETRY_G1B_EXECUTION_2026-09-08.md` | `b352888579253b532303e00fe2b99b9552206326` | 11451 | 几何专项 G1B 判据论证与离线结果；工具说明仍在 ../tools/PBUnifiedGeometryG1B/README.md |
| `docs/REMOTE_OVERNIGHT_THROUGHPUT_2026-09-09.md` | `5b516a65140b23929a7ddabc5eb1c5b200862ff0` | 86722 | 夜间自主推进记录（第 17-23 节）；各节终态与哈希见 EVIDENCE_INDEX.md 第 6.2 节 |
| `docs/REMOTE_RECEIVER_DECISION_DIAGNOSTICS_2026-09-08.md` | `2beba6f794260fbda0f951ea5f2e3059389ba52c` | 9219 | Receiver 接纳原因诊断与原始对照 |
| `docs/REMOTE_STEP1_EXECUTION_2026-09-08.md` | `182d6b7b35bf0498f5f7ef1923c2c49d1043e68a` | 21344 | Step1 本地准备执行记录（PREPARED/PARTIAL、fieldStatus=NOT_RUN） |
| `docs/REMOTE_STEP1_MEASUREMENT_CONTRACT.md` | `04d1cf0d4a0766126808da4965c533edd6259a06` | 20352 | Step1 本地测量合同：主时长定义、晚加入按 (SessionTag, FrameSequence) 关联、RunReport.3.measurement 不得制造样本等规则已收进 PROJECT_STATUS.md 第 6 节 |
| `docs/REMOTE_STEP2_DIAGNOSTICS_AND_REPLAY_CONTRACT.md` | `c2fff20c58635844579ac7e3e44a571e5c4bc80f` | 9565 | Step2 逐阶段诊断与 OfflinePixels 回放合同 |
| `docs/REMOTE_STEP2_EXECUTION_2026-09-08.md` | `9784622b568879e40377b058f0d76c8fe51e3370` | 10174 | Step2 本地实施与录像诊断结果（OfflineRecordingDiagnostic；不构成远程吞吐认证） |
| `docs/REMOTE_STEP3A_EXECUTION_2026-09-08.md` | `f20a56f7bb68b705fac7454546dc787eeef59cf7` | 10630 | Step3-A 本地实施记录；工具说明仍在 ../tools/PBRemoteThroughputStep3A/README.md |
| `docs/REMOTE_STEP3B_EXECUTION_2026-09-08.md` | `110d61c66243cdbb7ccedc13e1e4d311d892b22c` | 13323 | Step3-B 固定有状态码流对照执行记录 |
| `docs/REMOTE_THROUGHPUT_ATTRIBUTION_2026-09-08.md` | `b3a0acdf34b4314b3de0c19f4fb2be352f9de90d` | 5998 | 四轮归因 A1/B1/B2/A2 与观察量补齐；缺少合规反序配对的事实保留在 EVIDENCE_INDEX.md 第 6.1 节 |
| `docs/REMOTE_THROUGHPUT_RESUME_HANDOFF_2026-09-09.md` | `e6bd455adf1c96c306017984770bb4a6b0ac9891` | 15280 | 吞吐线暂停交接与恢复入口；未关闭项与恢复前置条件见 PROJECT_STATUS.md 第 7 节 |

### 3.8 H 组：文档与发布元材料（2 篇）

| 文档 | blob SHA-1 | blob 字节 | 原状态与删除理由 |
| --- | --- | --- | --- |
| `docs/DOCUMENT_STATUS_2026-09-09.md` | `451c1d074fbeab83f94bd888e6064886c4a636f3` | 33860 | 本轮整理之前的文档状态矩阵（一次性自证材料），已被本文件与 PROJECT_STATUS.md 第 8 节取代 |
| `docs/GITHUB_PUBLISH_CHECKLIST.md` | `9b554bda9c8009886cdaa474e7dcecf3bbec7668` | 12430 | GitHub 首次发布检查清单；仓库当前无 remote，需要发布时按本表 blob 取回核对 |

分组合计：A=3，B=9，C=5，D=4，E=3，F=22，G=16，H=2，共 64 篇。

## 4. 本轮自查结果

| 检查项 | 结果 |
| --- | --- |
| 整理前 `docs/` 跟踪文件数 | 79（`git ls-tree -r 7e88a69 -- docs/`） |
| 本次删除 / 保留 | 64 / 15，合计 79，无遗漏 |
| 第 3 节分组数 | 3 + 9 + 5 + 4 + 3 + 22 + 16 + 2 = 64 |
| 删除路径与登记条目比对 | 实际 `git rm` 的 64 条路径与第 3 节 64 条逐一对齐：缺失 0、多余 0 |
| 是否建 `docs/archive/` | 否（Git 对象库即归档） |
| 整理后 `docs/` 跟踪文件数 | 18 = 15 保留 + 3 新增（`PROJECT_STATUS.md`、`EVIDENCE_INDEX.md`、本文件） |
| 保护文件 `docs/PHASE1_GATE_REPORT.md` | 始终未跟踪，SHA-256 保持 `076ef4c9b9f89eabccd323dbe4bffc4dc125ddaf96e6ee437d2cf5b1b1cea306` |
| 全仓库真 Markdown 链接 | `FILES=47 CHECKED=118 DANGLING=0` |

链接检查口径（可复现）：文件集来自 `git ls-files -co --exclude-standard -- '*.md'`；链接用正则 `\[[^\]]*\]\(([^)\s]+)\)` 提取；跳过 `http:`/`https:`/`mailto:`/纯 `#anchor`/`data:`；去掉 `#anchor` 与 `?query` 后按所在文件目录解析并 `Test-Path`。

**有意保留、未修复的失效引用（不是遗漏）：**

1. `build-phase0-gate-evidence/` 内 2 处指向 `docs/PHASE0_GATE_REPORT.md` 的链接：该目录被 `.gitignore:3` 的 `build-*/` 忽略，属于**已封存证据快照**，不改写。
2. `tests/UnifiedRemoteGate/README.md`、`tests/UnifiedRemoteGate/FIELD_1GIB_README.md`、`tools/README.md` 以及若干 `tools/**/README.md` 中，以反引号散文形式提到已删文档的地方：它们不是可点击链接，属于当时的历史叙述，保留原文；需要原文时按本文件 blob 取回。

**未做与边界：** 本轮只动文档与 Git 记账，未构建、未测试、不实屏、不远程、未修改任何源码字节或封存证据，因此不构成任何验证、晋级或性能结论。

**遗留待用户确认：** 仓库根 `display_probe.obj`（366,491 B）被 `*.obj` 规则忽略、不属于本轮文档整理范围，未删除。

## 5. 后续文档维护规则

1. **双登记。** 新建文档必须同时登记到 `docs/README.md` 的索引与 `docs/PROJECT_STATUS.md` 第 8.1 节的文档地图，否则视为未交付。
2. **删除只用 `git rm` + 本文件登记。** 删除时在本文件第 3 节追加一行：路径、blob SHA-1、blob 字节、原状态与删除理由。不建 `docs/archive/` 目录。
3. **职责分离。** 现状只写在 `PROJECT_STATUS.md`；证据身份、包哈希、终态与不得抹掉的事实只写在 `EVIDENCE_INDEX.md`；本文件只登记文档的去向与恢复方式，不重述结论数值。
4. **冻结证据只追加勘误。** 已归档 Gate 的当时的结论、数值、失败门限、豁免范围不得被改写、精简或"优化掉"。
5. **保护文件。** `docs/PHASE1_GATE_REPORT.md` 永不 `git add`、不提交、不修改、不移动、不删除，始终保持唯一未跟踪状态。
6. **Git 纪律。** 所有暂存用显式路径，禁止 `git add -A` / `git add .`；非 amend、不 reset、不 rebase、不重写历史；提交前后执行 `git diff --cached --check` 与 `git diff HEAD^ HEAD --check`。
7. **取回方式。** 需要历史文档时先取回内容到临时位置阅读；只有在决定让它重新成为规范文档时才放回 `docs/`，并同时更新索引与本文件。
