# PixelBridge P1.5 RemoteVisual Hardening Checkpoint

## 1. 文档状态

这是用户结束本轮任务时保存的**实现检查点**，不是 `P1.5 RemoteVisual Reality Check` 的最终验收报告，也不替代历史 `P1-11 Phase 1 LocalDesktop Gate`。

- `ImplementationCommit`: `72a210eae2a857c930f0621d451654587bf9779a`
- `ImplementationTree`: `b61f9e62014830d4f7c36cf618f3c32512e8aebe`
- `BaseCommit`: `cd533feceaaf28b925e4b6878e589747ea159c67`
- `phase1-gate-pass` annotated tag object: `fde56c4c4e7124e8ffe29a0dcb619f8236781ebb`
- `phase1-gate-pass^{}`: `80699813b595bcf6db64047b50d31056872e33e1`
- `RemoteVisualSmokePass`: `NotEvaluated`
- `CertifiedRemoteVisualProfile`: `false`

没有生成 `docs/P1_5_SUNLOGIN_REALITY_REPORT.md`，因为新实现尚未完成正式双机矩阵、真实 Replay、QoS 恢复验证和同提交 LocalDesktop 回归；把本检查点伪装成最终验收报告会越过证据边界。

## 2. 已取得的真实基线证据

### 2.1 修改前 LocalDesktop reference

修改前基线使用 `cd533feceaaf28b925e4b6878e589747ea159c67`、相同 8 MiB RAW 随机源和右侧实验屏幕。四个组合均完成 WholeFileDigest、final publish 和外部 SHA-256/length 验证；Encoder 在 Decoder 完成时仍在广播。

| Capture/Profile | VerifiedEncodedGoodput | FER | Bootstrap | 结果 |
|---|---:|---:|---:|---|
| WGC / Direct-Level | 9.467 Mbit/s | 0 | 100% | PASS |
| DXGI / Direct-Level | 9.633 Mbit/s | 0 | 100% | PASS |
| WGC / Shape+Chroma | 13.665 Mbit/s | 0 | 100% | PASS |
| DXGI / Shape+Chroma | 14.635 Mbit/s | 0 | 100% | PASS |

证据：

- `build-p1_5-evidence/p1_5-prechange-20260830-193716-cd533fe/localdesktop-reference-summary.json`
- size `10116`，SHA-256 `750D85F7F77413584553C221E4B550635C1F2AFBAD9D5C991BE0772755D1CD7D`

### 2.2 修改前真实 RemoteVisual raw baseline

两轮均为 WGC、严格 1920×1080 ROI、10 分钟窗口、RAW。它们是 legacy raw evidence：两端当时没有共享 RunId，因此不能计为正式 merged run。

| Profile | Bootstrap | Descriptor | Transport admission | Verified bytes | Publish |
|---|---:|---|---:|---:|---|
| Direct-Level | 1311 / 20799 = 6.303% | 未观测 | 0 | 0 | 无 |
| Shape+Chroma | 10701 / 19081 = 56.082% | 未观测 | 0 | 0 | 无 |

两轮均没有 false output，且没有放宽 CRC、FEC、WholeFileDigest 或 publish acceptance。

证据：

- Direct：`build-p1_5-evidence/p1_5-prechange-20260830-193716-cd533fe/remote-raw/raw-A-direct-wgc-1to1-20260830-201550/failure-classification.json`，size `2289`，SHA-256 `191D1D457D8F76E83EA6CE2E0A1CC3C703F709504028C3A32931775827BA83F7`
- Shape：`build-p1_5-evidence/p1_5-prechange-20260830-193716-cd533fe/remote-raw/raw-B-shape-wgc-1to1-20260830-203312/failure-classification.json`，size `2069`，SHA-256 `57F9CAF6F49EDB635FE148DC43DDB655B3D8EDB5002EC043F713EC002B59604E`

### 2.3 用户提供失真图的结论

- 图 1 的有效画布约为 2418×1360，对 1920×1080 的估计 scale 为 X=1.259375、Y=1.259259；必须由 strict geometry gate 拒绝，不能 silent resize。
- 图 2 的有效画布为 1920×1080，可作为 strict 1:1 候选，但仍存在细粒度 codec/chroma 污染和时间上的局部陈旧块；geometry 正确并不足以恢复数据。
- 观测到的主要机制是：非整数缩放、预测/局部刷新形成的矩形陈旧区域、codec/chroma 污染及 temporal composition 叠加。

派生分析：

- `build-p1_5-evidence/remote-screenshot-analysis-20260831-023146-cd533fe/analysis.json`
- size `2848`，SHA-256 `50A9926D28B42ED7574BE847332E263F5DD3AB8193B084F911C6DDC9E446E538`
- 分析文件只保存输入图 hash、派生几何和定性结论；没有复制额外屏幕区域。

## 3. 本检查点实现

### 3.1 独立的失真鲁棒实验 profile

新增 `RemoteVisual Resilient 8x8 Luma`：

- 独立 profile ID `0xED05C2CC0397572A` 和 layout version `6`；
- 1920×1080、8×8 tile、中心 4×4 采样；
- luma endpoints 32/224，降低 chroma 依赖；
- 98 个 128×128 freshness regions，对陈旧/混合区域先置零 LLR，再进入原有 Robust QC-LDPC；
- 每帧仍为一个未改变数学定义的 Robust QC-LDPC codeword；
- 默认 2 logical visual FPS / 500 ms dwell，用持续 Carousel 广播换取远控编码器刷新机会；
- CPU reference、D3D11 compute、WARP parity 和独立 Golden manifest 已加入。

这是为失真证据建立的**新实验 profile 候选**，不是对 frozen Direct-Level/Shape+Chroma 的暗改。Bootstrap、Control、Transport、Inner/Outer FEC、CRC、digest 和 publish acceptance 均未放宽。它尚未通过真实双机 field gate。

### 3.2 非 wire hardening

- `PixelBridge.RemoteVisualRunMetadata.1`、共享 128-bit lowercase CSPRNG RunId 和 metadata preset；
- `PixelBridge.RunReport.2`、1 Hz/6 h/32 MiB bounded journal、endpoint artifact sealing；
- provider-generic `PBRemoteVisualReport` 严格 merger、combined JSON/Markdown/CSV/replay index；
- 右侧实验屏幕 containment、physical ROI、DPI/rotation/topology strict gate 和 no-activate window 行为；
- `(CaptureEpoch, FrameSequence)` duplicate/reordered/gap/stale accounting；
- unique/duplicate Outer admission telemetry，不改变冲突 fail-closed 语义；
- capture/visual stall、queue HWM、goodput/ETA unavailable semantics；
- Replay v2 receiver-only streaming container，CRC32C+BLAKE3、`.partial`、no-overwrite、严格 bounds 和 bounded recorder fan-out；
- headless offline replay 复用 production Bootstrap/Demod/FEC/Receiver；
- source-set、environment fingerprint、portable-package 和 run-preset 工具。

Encoder 仍然是持续 Broadcasting：Carousel/repair 完成一轮后继续下一轮；Encoder report 中 receiver progress、ETA 和 VerifiedGoodput 固定为 `null`。

## 4. 本地验证

以下均在实现提交对应的 tested tree 上以无界面方式完成：

- Qt 5 Release 全量 build：PASS。
- Qt 5 关键 CTest：24/24 PASS；`PBApplicationTests` 为 41 cases / 86574 assertions，`PBQSettingsTests` 为 7 cases / 86 assertions。
- Qt 6 Release 全量 build：PASS。
- Qt 6 关键 CTest：7/7 PASS（Application、QSettings、RemoteVisual、Receiver、D3D11 Demod、Replay、Golden check）。
- MSVC ASan 定向 CTest：6/6 PASS（Application、QSettings、RemoteVisual、Receiver、D3D11 Demod、Replay），73.24 s。
- fixture 泛化后的 ASan 复核：2/2 PASS（Application、Replay）。
- `D:\Python3.12.9\python.exe -m unittest -v test_pb_remote_visual_report.py`：20/20 PASS。
- Golden generator check：32 个已有 fixture 独立重生成一致；旧 Golden 未修改。
- PowerShell metadata preset：合法 CSPRNG RunId/preset PASS，单字段超过 1024 UTF-8 bytes 时 fail closed 且不创建文件。
- `git diff --cached --check`：PASS。
- 新增行/新文件检查：没有 `BlockInput`、`ClipCursor`、全局 input hook、`SendInput`、持续置顶、显示模式修改、High/Realtime priority 或非视觉 IPC payload bypass。

没有运行完整 113 项 CTest，因为其中包含会创建原生窗口、置顶或注入输入的历史 native gate；在用户左侧工作区保护约束下，不能把这些项目混入本轮无界面收口。不能把上述本地测试解释为真实 RemoteVisual field certification。

## 5. Critical/High 审查结果

提交前已修复：

1. Qt 5 overloaded signal connect 的编译歧义；
2. ScreenRegion host probe 缺少 `PB::PBApplication` 依赖；
3. endpoint sealing 在 Windows 上比较不稳定 ctime 导致的误拒绝，并保留 path identity/size/mtime/fstat 的稳定性检查；
4. merger 中 provider/version/mode/FPS/chroma/resolution 未交叉核对的问题；
5. metadata 单字段/总 UTF-8 budget 与 NUL 校验；
6. 代码和测试中的具体 provider、机器 serial 和固定机器阈值；
7. replay shutdown 等待、stall overlap 和 locale 非确定性。

在静态审查与已执行测试覆盖内，没有剩余已知 Critical/High。真实视频信道可用性、长时间广播稳定性与 QoS 恢复仍属于未执行 field evidence，不因本地测试而降级为低风险。

## 6. 尚未完成的验收项

以下项目必须在后续任务中从本检查点继续，不能回填为本轮成功：

1. 用 implementation commit 构建并部署两端便携包；
2. A–H 正式双机矩阵和两种 profile 的真实 receiver-only Replay；
3. 新 RemoteVisual profile 的 1 MiB 3/3、8 MiB 2/2、ZIP 1/1；
4. DXGI 代表性复核；
5. 用户人工 QoS 限速、恢复确认及恢复后继续收敛；
6. final tested commit 上的 WGC/DXGI × Direct/Shape LocalDesktop 四组合回归；
7. LocalDesktop goodput 是否存在超过 10% 的未解释回归；
8. 正式 combined reports、CSV summary、replay index 和最终 `P1_5_SUNLOGIN_REALITY_REPORT.md`。

当前 blocker 分类仅依据修改前 raw baseline 和用户图，暂定为：

- `C: Signal-layer blocker`；
- `D: Temporal/channel blocker`；
- `E: Scheduler/large-file blocker`（production path 仍为单 Segment，最大 8 MiB）。

新 profile 是否把 C/D 改善到可重复恢复，必须由真实双机 replay 和 external SHA-256/length 证明。

## 7. 环境与屏幕约束记录

- Decoder 主机：Windows 11 Pro 25H2 build 26200.9278；Ryzen 9 9950X；NVIDIA GeForce RTX 5090 D。
- `ProtectedMonitor`: `\\.\DISPLAY1`，physical rect `[0,0]-[2560,1440]`。
- `ExperimentMonitor`: `\\.\DISPLAY2`，physical rect `[2560,0]-[5120,1440]`。
- environment fingerprint：`build-p1_5-evidence/environment-tool-smoke-20260831-012749.json`，size `4531`，SHA-256 `D4E17EA87B2EF6DBCF41D5E2750D4C2DC2EB83138A35BF4EA77EE6A4DBD88499`。
- 本轮已有 native baseline 的 foreground PID 前后相同，记录为无 focus violation；最终提交收口没有启动任何 GUI、capture 或 native display gate。
- 未修改主显示器、排列、分辨率、刷新率、DPI、HDR 或颜色配置；未限制键盘和鼠标；未捕获或保存左侧屏幕像素。

## 8. 保留与提交边界

- 大体积 evidence、screenshots、replay、build outputs 和 portable packages 仅留在 `build-p1_5-evidence/` 或 build 目录，没有提交 Git。
- 用户原有未跟踪文件 `docs/PHASE1_GATE_REPORT.md` 未修改、未移动、未暂存。
- 没有 amend、reset、rebase、push 或新建 tag。
- 历史 `phase1-gate-pass` 对象与 peeled commit 保持不变。
