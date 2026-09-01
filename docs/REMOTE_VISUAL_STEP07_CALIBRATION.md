# PixelBridge RemoteVisual Step 07 Soft-Metric Calibration Gate

状态：**Step 07 的 CPU/reference soft-metric 标定与 false-confidence Gate 已完成。Gate 选择 `lf4-default/PiecewiseLookup` 作为 Step 08 待冻结候选，但没有修改 production default、VisualProfile、LayoutVersion、FEC、CRC、Receiver admission 或文件发布语义。这不是 LF4 production deployment、双机 field PASS、`RemoteVisualSmokePass` 或 Certified RemoteVisual Profile。**

## 1. 目标与结论

本步骤回答一个窄问题：在不放宽 LF4 modulation/Transport admission、也不让 sender truth 进入解调或 FEC 的前提下，能否用彼此隔离的 train/validation/holdout dataset/run 选择比当前 raw soft metric 更可信、并且能泛化到真实 Step 02 RDP Replay 的 metric mapping。

新增 `PBRemoteVisualMetricCalibration` headless 工具及 `PixelBridge.RemoteVisualMetricCalibration.1` / `PixelBridge.RemoteVisualMetricCalibrationEvidence.1` 两层 create-only seal。它直接复用 production LF4 demodulator、QC-LDPC、canonical padding、Transport CRC 和 Session identity reference channel，不复制第二套协议或 acceptance 实现。

权威 Gate 结果：

- baseline：`lf4-default/Raw`；
- selected：`lf4-default/PiecewiseLookup`；
- admission policy 仍是 LF4 default：`minimumSymbolMargin=0.08`、`minimumSymbolRms=0.35`、`maximumSymbolResidual=0.7`；
- selected model 是由 Train truth 拟合的 16-bin 单调 piecewise error-probability lookup；完整 bin upper、training sample/error 和 calibrated magnitude 均在 sealed parameter manifest 中；
- candidate 只由 Validation 排序；Holdout 和 External 在 selection 完成后才对 baseline/selected 两项打开；
- Holdout 与真实 RDP External 的 calibration loss/ECE 均严格改善，hard decision、BER、FER、FEC/CRC/identity 和 accepted Transport 没有回退；
- false accepted Transport/control/output 全部为 0；
- disposition 为 `CandidateOnlyRequiresStep08Freeze`，production defaults 保持不变。

## 2. 输入、split 与 truth boundary

### 2.1 固定输入矩阵

总输入为 25 个互异 `(datasetId, runId)` group、44 个 frame、6 个 admission policy 下共 264 个 immutable observation：

| split | dataset/run groups | frames | 来源 |
| --- | ---: | ---: | --- |
| Train | 8 | 12 | 6 个 mild simulator case；`h264-420-crf18-intra` 与 `h264-444-crf28-inter` 各 3 帧实际 codec Gray8 |
| Validation | 8 | 12 | 6 个 moderate simulator case；`h264-420-crf35-inter` 与 `hevc-420-crf28-inter` 各 3 帧实际 codec Gray8 |
| Holdout | 8 | 12 | 6 个 severe simulator case；`h264-444-full-crf28-inter` 与 `hevc-420-crf40-inter` 各 3 帧实际 codec Gray8 |
| External | 1 | 8 | Step 02 Windows Remote Desktop LF4 receiver-only Replay 的实际 captured ROI |

Split unit 固定为完整 `datasetId/runId`，同一 group 不允许跨 split；signal profile、truth availability、frame coverage 或 policy coverage 冲突均 fail closed。模型只在 Train 拟合，只在 Validation 选择。代码不会为某个远控 provider 名称选择阈值；External 的 signal profile 诚实记录为 `PB-Signal-RdpUnknown-1`。

### 2.2 真实 Replay 的 sender truth 恢复方式

Step 02 Replay 自身仍然是 receiver-only：其中没有 expected sender bytes，单独检查它时 `falseAcceptedCodewords` 必须继续是 `UnavailableReceiverOnly`。Step 07 只有在同时满足以下条件后，才把 External frame 标为 `DiagnosticSenderFixture`：

1. Python builder 交叉核对 Step 02 dataset index、completion summary、LF4 inspection 和 Replay 的 SHA-256/BLAKE3、collection dataset、Replay descriptor dataset 和 RunId；
2. C++ pipeline 用 `ReplayV2Reader` 完整验证 capture-only Replay、resource limits、footer 和 descriptor profile；
3. C++ pipeline 从与 Step 02 Presenter 相同的固定 Bootstrap/Transport fixture 重新生成完整 raster，并要求 BLAKE3 精确等于已封存的 Presenter LF4 raster `28b09b66520b87f9cd9407b516530ad1225f84d390cc2b94d18d6e1b8d5e9bc4`；
4. captured pixels 独立进入 production LF4 demodulator；sender truth 不进入 modulation 或 FEC；
5. 只有 production `QC-LDPC+padding+TransportCRC+SessionIdentity` 已接受的 block 才在 admission 之后与 sender fixture 的 exact slot/byteCount/bytes 比较。

因此 External 的 “0 false accept” 来自独立封存的 Presenter truth 与真实 captured pixels 的重新结合，不是把 receiver-only CRC-valid 结果直接改写成 sender-truth 结论。

本工具不创建 Outer/Receiver file session，不评估 WholeFileDigest，不发布文件。机器可读 truth boundary 固定记录 `wholeFileOutputEvaluated=false`、`acceptedOutputObjects=0` 和 `falseAcceptedOutputObjects=0`，不能把 Transport block 结果外推成文件发布成功。

## 3. Candidate matrix 与防泄漏规则

固定矩阵为 6 个 policy × 4 个 metric model，共 24 项：

- policy：LF4 default、margin 0.04、margin 0.12、residual 0.55、residual 0.90、RMS 0.45；
- model：Raw、GlobalScale、PerSignalScale、PiecewiseLookup。

margin 0.04 与 residual 0.90 相对 default 放宽 admission，报告仍保留它们供审计，但 `policyRelaxesDefaultAdmission=true`，永远没有 selection 资格。PerSignalScale 需要已知 signal-profile binding，`deployableWithoutSignalProfileBinding=false`，因此不能作为 provider-generic 候选。最终候选必须同时满足：

- complete Train/Validation truth coverage；
- production acceptance safety；
- Validation 排序优于 baseline；
- 不放宽 default admission；
- 不需要 External provider-specific signal binding；
- selection 过程未读取 Holdout/External summary。

只有确定 baseline 与 selected 之后，才分别计算两者的 Holdout/External。其余 22 项的这两个 split 保持零 frame/空 curve，Python sealer 会拒绝“提前打开”或“选完后缺失”状态。

## 4. 权威结果

### 4.1 Validation（唯一 selection surface）

| 指标 | `lf4-default/Raw` | `lf4-default/PiecewiseLookup` |
| --- | ---: | ---: |
| frames / modulation erasures / verified | 12 / 7 / 5 | 12 / 7 / 5 |
| BER | 0.05884567901234568 | 0.05884567901234568 |
| accepted Transport / false accepted | 20 / 0 | 20 / 0 |
| FEC failures / iterations total | 0 / 50 | 0 / 49 |
| average log loss | 0.4180466693827719 | 0.05354665741491579 |
| expected calibration error | 0.27831021588882454 | 0.020988875567698764 |

### 4.2 Holdout（selection 后首次打开）

| 指标 | baseline | selected |
| --- | ---: | ---: |
| frames / truth frames | 12 / 12 | 12 / 12 |
| modulation erasures / verified | 7 / 3 | 7 / 3 |
| BER / FER | 0.11609567901234567 / 0.75 | 0.11609567901234567 / 0.75 |
| accepted Transport / false accepted | 12 / 0 | 12 / 0 |
| FEC / CRC / identity failures | 8 / 0 / 0 | 8 / 0 / 0 |
| iterations total | 384 | 384 |
| false-confidence errors | 0 | 0 |
| average log loss | 0.43106895732822903 | 0.1223487155739084 |
| Brier score | 0.1261610113724054 | 0.04399907276718941 |
| expected calibration error | 0.2272827570759902 | 0.028538512183229617 |

### 4.3 External（Step 02 真实 RDP LF4 Replay）

| 指标 | baseline | selected |
| --- | ---: | ---: |
| frames / truth frames / verified | 8 / 8 / 8 | 8 / 8 / 8 |
| compared bits / BER | 518400 / 0 | 518400 / 0 |
| accepted Transport / false accepted | 32 / 0 | 32 / 0 |
| FEC / CRC / identity failures | 0 / 0 / 0 | 0 / 0 / 0 |
| false-confidence errors | 0 | 0 |
| average log loss | 0.31321503283560503 | 0.0003354882554752294 |
| Brier score | 0.07231140836463415 | 1.1251461695497329e-7 |
| expected calibration error | 0.2689070269798241 | 0.00033543198558441223 |

必须按正确边界解读这些结果：selected 改善的是 soft confidence 的概率标定；Holdout/External 的 hard bit、erasure、FER、accepted Transport 和 FEC 结果没有变化。本步骤不声称提高了实际恢复帧数或文件 goodput。它证明候选没有借助 admission 放宽获得表面收益，并为后续冻结/production integration 提供更诚实的 metric scale。

## 5. 一键重建

在 Release tool 已构建，且 Step 02/06 gitignored evidence 仍可用时，从一个尚不存在的 output directory 运行：

```powershell
D:\Python3.12.9\python.exe -B `
  tools\PBRemoteVisualEvidence\build_step07_calibration.py `
  --calibration-exe build-desktop-levels-release\tools\Release\PBRemoteVisualMetricCalibration.exe `
  --step06-index build-p1_5-evidence\20260831-step06-production-truth-v2-a\step06-corpus-index.json `
  --step02-index build-p1_5-evidence\step02-rdp-20260901\step02-dataset-index-a.json `
  --step02-completion build-p1_5-evidence\step02-rdp-20260901\step02-completion-summary-a.json `
  --output-dir <new-output-directory>
```

输出只有三个文件：

- `metric-calibration.json`：C++ raw-payload BLAKE3 sealed report，包含 input inventory、truth boundary、24 项 candidate 的 parameter manifest 与完整 Train/Validation/选后 Holdout/External curves；
- `step07-calibration-index.json`：Python 独立交叉核对后的 source/tool/report identity、split manifest、选择摘要和约束；
- `SHA256SUMS.txt`：前两项的 SHA-256。

builder 拒绝 existing output directory、duplicate JSON member、非有限数值、schema/member/curve/cardinality 漂移、文件变化、digest mismatch、路径越界、symlink/NTFS junction、工具变化、timeout、超限 file/stdout/stderr、holdout leakage、provider-specific selection、admission 放宽、false acceptance 和不完整 truth coverage。输出 create-only；已存在文件不会覆盖。

## 6. Seal 与可重复性

最终 Gate/validator 收紧后的两次 create-only rebuild：

```text
build-p1_5-evidence/20260901-step07-calibration-final-g
build-p1_5-evidence/20260901-step07-calibration-final-h
```

`-g` 与 `-h` 分别使用 Step 02 的独立 dataset/completion seal，并在 FEC/CRC/identity/iteration/Brier/ECE 非回退条件、Validation-winner 独立复算及 curve/model/inventory/path 守卫全部加固后执行。两目录对应文件逐字节一致；较早的 `sealed-a/-b/-c` 与 `final-d/-e/-f` 是收紧过程中的开发证据，不作为最终 validator-complete seal：

| artifact | bytes | SHA-256 |
| --- | ---: | --- |
| `metric-calibration.json` | 375118 | `c17f17844c0b44cf029bb31ecbfc127139fa886588c941c2a3a0f34bf541acf8` |
| `step07-calibration-index.json` | 27877 | `d972a5fd30317c186ce0fb59279cd6ab9c7b86bfb748c05bc7e4ca9df444443e` |
| `SHA256SUMS.txt` | 186 | `2fbca82ab5f0bae957582b681091b3d19ab7215ee8e2c8160575eaafb41e8c9b` |

关键 BLAKE3：

- report file：`7425a07e4d7abdb69a24cab8eddd7956c06edcc697114e7f40a68b4348a7b660`；
- outer raw payload：`2fdf2c5dbfbc52d7a73d878243f96ae60b095b6f4456bff73f226a085288287f`；
- calibration core raw payload：`7a8ff14e09dc5f0b4417349f9ab3053f0e0dac05312be94806e55d40324f619f`；
- immutable observation input：`06eeacf328e8c8855803e346315b67ca366f1aefd2f25c2778e786596b31d139`；
- index canonical payload：`b282d4a5729c2d4a446cbf763dd7ece4249d37d876a0f4c78585450691450e34`。

本轮最终 evidence 所绑定的 Release `PBRemoteVisualMetricCalibration.exe` 为 718336 bytes，SHA-256=`588e96c7830e146cdf1dd14515594f985424ffb6761d751dcb8b1176c82a2c69`，BLAKE3=`f51fcc871ace922344f7c5854d6f5326c527e9ed3221e09ca4fe275932a3133e`。

## 7. 验证与剩余边界

提交前验证覆盖：

- C++ deterministic selection、dataset/run 泄漏、receiver-only truth、CRC-valid nontruth、不可选择的 admission-relax policy 和 split preflight；
- Python raw C++ payload seal、candidate matrix/curve/cardinality、holdout leakage、false accept、digest/duplicate member/path escape、file/dir symlink 和 NTFS junction；
- 与 Final Gate 相同的 cppcheck 2.21.0 参数扫描 calibration core 与 CLI，并把两项加入静态 Gate project inventory；
- Release 与 MSVC ASan/RelWithDebInfo 的 targeted 及全量无界面 CTest；
- 完整 RemoteVisual evidence Python suite；
- 两次最终 create-only evidence rebuild 的逐字节一致性。

具体最终计数记录在 `REMOTE_VISUAL_LOW_FPS_TECHNICAL_ROUTE.md` 第 16 节，避免本文在后续新增测试时被误读为整个仓库的永久总数。

Step 07 没有关闭以下边界：

- Step 08 尚未把 selected metric quantization/model、LF4 raster/mapping/codebook/Bootstrap/accepted Transport 结果冻结为 drift-detecting Golden；
- selected model 尚未接入 production Decoder fast path；
- LF4 D3D11 Encoder/Demod、连续 capture、QoS/late join、真实文件 Outer/WholeFileDigest/final publish 尚未完成；
- 当前真实 External 只有一组 Windows Remote Desktop、画质与 chroma mode 为 Unknown；这满足路线要求的“至少一组真实 replay”，不构成 provider matrix；
- 没有 `UniqueVisualFPS`、VerifiedEncodedGoodput、6 小时 soak、双机文件恢复或 Certified Profile 结论。

下一步是 Step 08：只冻结本步骤通过 Gate 的 candidate 与既有 LF4 wire/raster 语义；任何 codebook、mapping、profile、metric quantization 或 accepted Transport drift 必须使独立 generator `--check` 非零退出。
