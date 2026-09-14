# 2026-09-15 吞吐研究增量交接（T2 / T3 / O3b / T4）

## 1. 首先核对

继续之前读取 `AGENTS.md`、`docs/README.md`、本交接、`SESSION_HANDOFF_20260914_V0.6.md`、`PROJECT_STATUS.md` 与 findings §21.38–21.49，核对 live HEAD/status。当前用户明确要求多智能体并行，取代旧交接的禁止子智能体；子任务必须有独立、具体范围，临时脚本放在 create-only artifacts 目录。

目标仍是尽可能缩短远端完整 `TestMediumFile.bin` 和 `TestBigFile.bin` 的摘要、安全发布、重开总时间。纯视觉单向payload、不增加ACK/旁路、不改Citrix/网络/显示、不干扰左屏或输入、不削弱安全/资源门。频繁测试单次≤100000000B，证据tag不复用；不盲扫FPS、不马上重跑Big。当前目标**未完成**，本轮没有新的完整双文件成绩。

授权：采样opt-in、O1窄验证和O3b本地评估已确认；高影响新profile/layout/Golden方案须再次问用户。既有O1喷泉调度、接收端O3b并行FEC和历史O4灰阶profile均已存在，不要再重复提出“尚未实现”。

## 2. 源码与保护文件

- `e8c9c5b`：neutral linear fullscreen compose位等价快速路径；point默认不变。
- `fb6a849`：DataWindow紧凑BGRA一次有界memcpy，stride fallback保留。
- `4854af3`、`05ba6b3`及本交接后续docs commit：结果/勘误；以live git log取最终hash。
- `.zcode/`、`docs/PHASE1_GATE_REPORT.md`、`docs/V3_GRAY_STAGE_C_HANDOFF.md`为用户保护的未跟踪文件，不纳入提交、不修改。
- build：`<repo>\build-nonlocal-stall-20260913-2111`；只构建受影响target，不默认全套测试。最近全PBApplication已知M late-join 12505<12000失败未删改。

## 3. 有效远端证据（ROOT）

ROOT=`<repo>\artifacts\nonlocal-stall-20260913-2111`。

| tag | bytes | sampling / FPS | native ms | 结论 |
|---|---:|---|---:|---|
| nl0914-t2-spatial-point25 | 25000000 | point / 30 | 126906 | 四门+第二SHA通过 |
| nl0914-t2-spatial-linear25 | 25000000 | linear / 30 | 120974 | 小样本快4.67%，不推广 |
| nl0914-t2-spatial-linear100 | 100000000 | linear / 30 | 660758 | 较旧point100慢25.3% |
| nl0914-t3-submit-point25 | 25000000 | point / 30 | 126897 | 与T2 point差9ms，不证明memcpy因果 |
| nl0914-t4-linear-f22-100m | 100000000 | linear / 22 | 529685 | 比T2 linear30快19.84%，未胜旧point526930 |

这些均 receiver-first、6MiB目标段、spatial、首轮100%、budget-bound。各自全新Session、无resume、源稳定、whole digest/rename/reopen/published/第二SHA通过；远端原2560×1600@240前后不变，owned process已退出。T4 foreground端点不同，capture dropped708、staleResultDrops1、orphanAdmitted33均保留，不能写成“全计数0”或“全程焦点不变”。只可称Outer资源拒绝/冲突/延期/orphan quota/conflict为0。

## 4. 身份与错误证据的边界

- T2 package=`candidate-t2-compose-fast-release`，Encoder SHA256=`f510ba36f9f5b91157ec22d2a350db8692e0dea09693b256e4aac8a74bdb60bf`；Decoder冻结v0.6=`151e46e5e3f8dd2c9e927e5aa43690ca53a60692b734299b671f8bd8f536967d`。
- T4完全复用T2两端，不使用T3 memcpy包；唯一更改是configured FPS30→22。选择依据事前保存于 `nl0914-t4-linear-f22-100m-preregistration.json`。
- T3 Encoder=`837b5a69db5756efe5d804084efbc92c9a2032d673477e7c7b9c5c0de2be6f3c`。原T3 identity继承T2部分source字段；不改旧artifact，使用 `candidate-t3-submit-copy-provenance-correction.json` 与同名source patch补充来源。
- EXE内嵌gitCommit仍是configure时25a08aa；不可将源码commit填入actualBuildIdentity冒充正常内嵌值。这些是private candidates，不是新release。
- `nl0914-fss-fast-point-25m-f30-s6`、`nl0914-fss-fast-linear-25m-f30-s6`、`nl0914-fss-fast-linear-100m-f30-s6`遗漏spatial参数，不能拿来与spatial基线作性能比较；原证据保留，见findings §21.44。

## 5. O3b实际结论与下一瓶颈

接收端 `UnifiedVisualCpuOracle` 已有18槽并行池、私有Robust/Fast decoder、槽序合并、资源计费；diagnostics时有意串行。当前现场harness未开逐槽diagnostics。`postGpuFecCpuTimeTotal100ns`计时整个Poll后段，不是纯FEC；当前约15–16ms/observation、进程约0.4CPU core，不证明CPU饱和。

artifact-only `nl0914-o3b-encode-bench01/` 测的是发送端编码：3profile×128帧×18码字，串行/复用3参与者池逐字节一致，双路径输入20736000B。Fast p50=1.063→0.3675ms；仅本机节省约0.7ms，不据此添加生产线程。该微基准不是remote/file-transfer证据；详见result.jsonl、identity.json、NOTES.md。

源码 `CalculateUnifiedGraduationTarget` 按配置FPS扩大base和margin；T2 linear配置30、实际21.655导致多余airtime。T4将配置22与实际21.146接近，outerAlreadyCompleted24514→7349，而outerUnique保持61400，支持该失配假设。但配置FPS同时改变节奏，非独立机制因果证明。结果仍未比point好，不继续盲扫。

条件容量推导：现18×1629B/帧，即使全是有效Transport，在7.8 unique FPS也最多228711.6 encoded B/s。若保持同encoded bytes，匹配历史双文件O的4312.469秒至少需10.4879 unique FPS（全18槽乐观）或11.1048（1Control槽）。这不是不可达证明，也不是整文件成绩；见 `nl0914-o3b-conditional-capacity-bound.json`。

## 6. 重放及下一步

Python固定用 `<python>`，不要依赖PATH的python（本轮曾不含CREATE_NO_WINDOW）。实屏前读取 `docs/REMOTE_OPS_BRIDGE.md` 和 `tools/PBRemoteOpsBridge/README.md`；桥只编排包/脚本/启动/停止/元数据，绝不递送payload给Decoder。先复核live进程和display，不能仅信旧的stage文件。

T4仅在确有新证据需要复跑且获准的情况下，用**全新tag**：

```powershell
& '<python>' -X utf8 '<repo>\artifacts\nonlocal-stall-20260913-2111\run_t4_fixed22_guarded.py' --tag '<全新未使用tag>' --sampling linear --bytes 100000000 --seconds 1200 --fps 22
```

此入口强制22fps/linear/100MB；同T2身份、前后display、ROI、资源拒绝/冲突、所有最终门与第二SHA都有检查。不要原样重跑已用tag。现有报告已足够判定“不晋级”，不建议无新假设反复复测。

下阶段应先明确一个能够提高实际有效帧payload率的新窄假设；需要新visual profile/layout时先询问，不把CPU单项小优化或兼容性修复等同于总目标。完整Medium成功并有充分性能依据后才考虑Big，仍保留既有timeout和资源门。

历史指定文件成绩仅属于O：Medium273806498B/860828ms，Big1059917774B/3451641ms，合计4312469ms；不得拼接或改名给当前候选。

## 7. 2026-09-15 授权后的 GrayFast8 离线候选

用户已明确回复“可以”，授权仅限提高单帧净载荷的新 visual Profile/layout 离线可行性对照，不含部署或远端文件运行。artifact-only 候选 `PB-Experimental-GrayFast8-Offline-1` 使用新 identity `0x5042475246383031`、layout 14：在 layout 13 的 6×6/41,872 tiles/7 planes/18 Fast slots 基础上，假设增加第 8 个独立中性亮度 plane，容量模型为 20 slots、32,580 B/unique frame（+11.111111%），保守 19 slots 为 30,951 B（+5.555556%）。确定性 128 帧 packing benchmark 显示新增 plane 增加本机工作量；不含 FEC、GPU、capture、远端、摘要、发布、reopen，不能视为吞吐证据。

产物：`artifacts/nonlocal-stall-20260913-2111/nl0915-profile-offline-grayfast8-01/`。后续若继续，先做 canonical raster/mapping/Decoder oracle/Golden 与错误 identity/layout、资源和冲突门，再以 ≤25 MB 完整摘要/发布/reopen 验证；未完成这些门前不得改默认、生产 Decoder 或启动 Medium/Big。
