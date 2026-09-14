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

## 8. GrayFast8 oracle 结果与当前门禁

layout 14 artifact-only oracle 已建立。现有 64-mask + 两个亮度 bit 的直接背景-plane 假设在完整 256 符号域失败：mask index 32 为全零，`32/96` 与 `160/224` 发生不可观测碰撞；结果为 `mappingRoundTrip=true`、`selectedGoldenVectorsRoundTrip=true`，但 `rasterRoundTrip=false`、`fullByteRoundTrip=false`、`REJECTED_AMBIGUOUS_SYMBOLS`。16 项安全/兼容 gate 仍全部通过，但那只是状态机与资源/摘要模型证据，不代表视觉表达可恢复。

artifact-only mask-repair 屏蔽性试探把 index 32 替换为 `0x13B0C15`，得到 256 符号 exact round-trip、64 个唯一 mask，但最小 Hamming 距离降为 8，必须重新定义完整 mapping/Golden/Decoder 并验证远端鲁棒性，不能接入现有 layout 13。证据位于 `artifacts/nonlocal-stall-20260913-2111/nl0915-profile-offline-grayfast8-01/`。在 mask-repair 或另一种正交表达未通过新的离线鲁棒门前，不得启动远端测试；Medium/Big 继续冻结。


## 9. Mask-repair 屏蔽性筛选

为消除 index 32 全零 glyph 导致的 4 个符号碰撞，artifact-only 屏蔽性候选将该 mask 替换为 `0x13B0C15`。256-symbol exact round-trip 和 64-mask uniqueness 通过，最小 mask Hamming 距离为 8；在固定 36-cell、最近距离解码及 ±2/4/8/16 synthetic luma 扰动下 256×8 trials 均 exact decode。以上均不是远端/codec/缩放/BER 证据，不能启动 field test。若要进入 ≤25 MB 视觉验证，需要用户另行确认 field-test 授权，并先冻结新 mapping hash/Golden/Decoder 与完整安全门。

## 10. Mask-repair oracle 更新

用户已确认继续 artifact-only。独立 mask-repair oracle 将现有 index 32 替换为 `0x13B0C15` 后，在完整 256-symbol 域 exact round-trip 通过，13 个 Golden vectors（含原碰撞边界）通过，`ambiguousCanonicalSymbols=[]`，状态 `ACCEPTED_OFFLINE_EXACT_MASK_REPAIR`；mapping hash=`f3b713926de2915bbd4d21818fd1afb6dd9043f04babd75524064e41356bfd02`。但最小 mask Hamming 距离为 8，field/codec/缩放鲁棒性仍未知。239,260 候选 mask 的有界搜索未观察到距离 9 替换；synthetic ±2/4/8/16 luma 扰动仅作为模型筛选。

下一步仍是 artifact-only 几何/采样/codec proxy 与安全门复核。未经主人新的明确授权，不运行 ≤25 MB field test；Medium/Big 保持冻结。

## 11. Mask-repair geometry proxy 结果

mask-repair 在无变换下 256-symbol exact round-trip，但简单 1-cell box blur 仅 53/256 exact、2-cell blur 仅 17/256；对比度 75% 仅 192/256，50% 仅 96/256。该模型不是 Citrix/codec 实测，但已否定“直接进入 ≤25 MB field test”的证据链。当前不请求 field 授权、不启动远端；若继续必须重新设计对 blur/contrast/phase 稳健的视觉表达，再重复 mapping/Golden/安全门。

## 12. 低频码本研究边界

artifact-only 低频候选包括 3×3 二值宏单元偶校验码本与多种 tile geometry。偶校验候选在 blur-aware affine 模型下原始脚本曾把 1 个 tie 计作 exact，现已按 fail-closed 修正为 255/256 accepted + 1 ambiguous；随机扰动也为 255/2048。不同 tile 几何显示 10×10 以上才在简化 blur proxy 下接近全 mask 可分，但 8-plane 容量跌至 11 slots 或更低，无法满足当前单帧载荷目标。以上均未建模远端 codec/缩放/BER，不得 field test。

## 13. 结构化多-cell 码本候选

artifact-only 研究找到一个可继续审查的 6×6 方案：3×3 二值 macrocell、每格 2×2 像素，使用 9-bit 偶校验码本排除全零常量 pattern，再加入奇校验 pattern 1，保持 256 symbols / 8 bits per tile。blur-aware affine proxy 在 identity/contrast75/contrast50 与 ±2/4/8/16/24 随机 luma 扰动下 256/256 accepted、0 ambiguous。该 decoder 语义和 codebook 均不同于当前生产 GrayFast，且可能增加每 tile 拟合成本；无真实远端、codec、缩放、BER/FER 或端到端证据。候选仍不允许 field test，需先做资源/映射/Golden/复杂度审查并取得单独 field 授权。

## 14. 结构化 2×2 macrocell 四级 luma 候选（artifact-only，未获现场授权）

最新离线筛选候选位于 `artifacts/nonlocal-stall-20260913-2111/nl0915-profile-offline-grayfast8-01/structured-multicell-screen.json`，状态 `ARTIFACT_ONLY_NOT_ACCEPTED`。几何为现有 6×6 tile 内的 2×2 个 3×3 均匀 macrocell；每格四级 luma（主梯度 `[0,85,170,255]`），即每格 2 bit、每 tile 8 bit、256 symbols。decoder 仅对每个 3×3 macrocell 的均值做最近平方距离判决。

同 blur reference 的理想化代理结果：radius1/radius2 均 `256/256 exact`；固定 radius1 后，对每个 macrocell 均值注入 ±1、2、4、8、16 的独立合成噪声，各 `4096/4096 exact`。但是无逐帧对比度校准时，主梯度在 75% 对比度仅 `52/256 exact`（1 ambiguous、203 wrong），50% 仅 `15/256 exact`（16 ambiguous、225 wrong）；更贴近当前灰阶的 `[64,120,184,232]` 在该代理下为 75% `2/256 exact`、50% `1/256 exact`。这否定了“直接以四级 ladder 进入现场”的证据链，后续必须先完成 pilot/校准、gamma/clip/phase/codec proxy，以及新的 raster/mapping/Golden/Decoder/安全资源门。

若不考虑上述恢复风险且理想保留 Fast 码字效率，20 槽的代数上限为 32,580 B/frame，相对当前 18 槽的 29,322 B/frame 为 +11.111111%；没有远端吞吐、完整摘要/发布/reopen 或 Medium/Big 证据。候选未改生产代码、默认 Profile 或 catalog；当前**没有新的 field-test 授权**，不得启动 ≤25 MB 或更大远端运行。

## 15. 结构化多-cell 的 pilot/affine 校准代理（仅 artifact）

为针对 §14 的对比度失败边界，新增 `structured_multicell_calibration_proxy.py` 与 `structured-multicell-calibration-proxy.json`。假设每帧两个已知 0/255 pilot macrocell，估计全局 gain+bias 后归一化数据 macrocell；在 contrast 1/0.75/0.5、offset 0/+12/-12 及 ±8 合成 macrocell 噪声下，9 组均 `256/256 exact`、无 ambiguous/wrong。该结果只支持理想 affine 模型下“校准可恢复”的窄假设，不覆盖 gamma/clip/缩放/codec/4:2:0/WGC/Citrix/BER/FER。

若两个 pilot 占用 20 槽中的 2 槽，净有效槽仅 18，Transport 上限回到 `29,322 B/frame`，相对现有为 0%；将 pilot 放入现有 Control 或跨帧复用需要新的 wire/调度/资源合同，不能作为免费收益。候选继续 `ARTIFACT_ONLY_NOT_ACCEPTED`，没有 field-test 授权，不得启动 ≤25 MB、100 MB、Medium 或 Big。

## 16. 现有空间 pilot 合同的再利用边界（artifact-only）

静态源码证据显示 layout 10 已有 4 个 `CalibrationReferences` 空间区域与 2 个 `PhaseChecker` 区域，校准区不消耗 Data codeword slot；Control 仍按 BaseLuma mixed-slot 合同计费。详见 `nl0915-profile-offline-grayfast8-01/calibration-pilot-inventory.json`。

因此，若未来新 layout 14 保留该空间 pilot 几何，20 槽候选不必自动因 pilot 变成 18 槽；但不能把现有校准值、raster 或 Decoder 语义直接复用为新四级 macrocell 的证明。仍需完成新 profile identity/layout、canonical raster、mapping/Golden、资源/安全门及 codec/缩放/phase/gamma proxy，之后再取得单独 ≤25 MB field 授权。当前没有 field 授权，不运行任何远端文件测试。

## 17. Canonical oracle 与安全前置门结果

已在 artifact-only 范围内建立结构化 2×2 macrocell 候选的 canonical raster/mapping/Golden oracle：256 symbols、base-4 LSB-first row-major，mapping hash=`d74ad6e0aac3c1b5d63159bd0708932997ad364b99f95b17e12775d2773977c9`，Golden hash=`7ba1ca8e1e7c4c7052395d738b77e1b97b5e0ad645b08d4d4c3d0420f9639259`。整数 1:1 采样下 identity/radius1/radius2 以及 gain 0.5/0.75/1.25 的 affine 校准代理均 256/256 exact；tie 和 invalid pilot 全部 fail-closed。fractional phase、rescale、chroma siting、gamma、codec/4:2:0、WGC/Citrix 未覆盖。

`structured-multicell-safety-gates.json` 的 15 项模型安全门全部通过，但候选仍未进入生产 catalog/wire。完成 field 前仍需新 profile 审查、采样/codec 代理、构建封存，并由主人授权全新 tag 的 ≤25 MB 远端摘要/安全发布/reopen 运行；Medium/Big 不因该授权自动启动。

## 18. 生产接入前的高影响身份冻结门

只读审计确认结构化四级 macrocell 不是局部常量替换：需要独立 layout 14 manifest/mapping、20-slot 缓冲与 FEC 维度、CPU/GPU metric parity、应用层 opt-in、package/run-report 和定向测试；正式 catalog/default 保持不变。现有 canonical oracle 与 15 项安全模型门只能证明候选 artifact 前置条件。

在修改生产源码前必须冻结一个新的 profile ID，不能复用已有 GrayFast8 artifact identity（其语义假设不同）。同时必须完成 clipping/gamma/phase/codec、CPU/GPU parity、wire/catalog 和资源门；未完成前不运行远端。该 ID 是 wire 兼容性和证据身份的高影响选择，需主人明确确认。
