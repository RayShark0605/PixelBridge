# Step3-B codec 几何归因与修复判据设计审查（2026-09-08）

## 0. 结论与范围

**本轮完成：原几何阻断定点归因、最小补充观测、原候选去重核对及安全修复可行性审查。结论为 `ATTRIBUTION_COMPLETE_CRITERION_NOT_ESTABLISHED`。**

**未完成也未实施：生产几何修复、替代准入判据、codec整文件恢复、现场验收。准入候选数量0。** Step3保持PARTIAL，Step4未启动，现场NOT_RUN。

本轮先用现有trace重建分支；经用户确认，观测原始与codec的ordinal0/1/15。第15帧出现额外marker后再次提问，获准只追加该帧并使用必要的原Bootstrap RS/CRC。全程无数据FEC/Receiver/WARP调用，无编码、参数矩阵、实屏或输入操作；不改变原Decoder/Receiver、安全发布或资源策略。

## 1. 首个决定性拒绝分支

原调用链：

```text
CaptureDemodulator::DecodeUnifiedBootstrap
  → DecodeLocalDesktopBootstrap（通用locator默认minimumScale=0.5）
  → ResolveUnifiedVisualSamplingGeometry
  → ResolveUnifiedSamplingGeometryInternal（当前SC6 Profile最小尺度=1）
  → SnapAxisToFrame
  → post-snap minimumScale检查 → false
  → InvalidGeometry → 不提交数据解调
```

源码定位：

- `<repo>\libs\PBDemodD3D11\src\capture_demodulator.cpp`：189–214，通用Bootstrap成功后应用Unified几何门。
- `<repo>\libs\PBModulation\src\unified_visual.cpp`：333–393，尤其385–390的post-snap尺度检查。
- `<repo>\apps\common\local_desktop_runtime.cpp`：2653–2689，原OfflinePixels配置没有改写locator默认policy。

通用locator与Unified payload几何门是两层不同范围。不能把CPU数据oracle内部收紧后的locator入口与本轮实际WARP入口混为一谈。

两轮共60个既有codec观察的几何数值均：

1. 通过初始尺度范围（包括既有0.005/1080容差）；
2. 通过两个轴的既有外越界范围；
3. 在边界处理后得到 `scaleX < 1`，首先触发水平最小尺度检查。

第0帧的可重算过程：

```text
originX = 0.00019240504229856015
inputScaleX = 0.9999988062505326
clampedScaleX = 1
far = originX + 1920 = 1920.0001924050423
boundedOrigin = originX（正值被保留）
boundedFar = 1920
postSnapScaleX = (1920 - originX) / 1920
              = 0.9999998997890405 < 1
```

第15帧对应post-snap scaleX为 `0.9999980604679725`。原始样例则始终为origin0/scale1，门通过。

以上60条是**源码支持的分支重建**，不是新增60个runtime观察。旧trace没有保存marker residual，不能伪造其读数；该分支对任何已通过原refine的有限非负residual都具有同样结果。所选3个codec样本另取得真实residual及原native resolver拒绝结果。

## 2. 边缘→拟合：两种不同起点

### 2.1 第0、1帧：初始几何不变，光度crossing产生偏差

原始与codec四marker中心均为(48,48)、(1872,48)、(48,1032)、(1872,1032)，seed均为(0,0,1,1)。codec改变了原VerifyMarker读取的black/white水平及实际边缘采样，因此FindEdge的midpoint crossing不再精确位于规范边缘。

例：左上marker的black/white由约32/224变为34.60606060606061/221.859375。通过原FindEdge采样括区可精确重算crossing，再按原FitAxis逐项累加顺序重算OLS，与native值精确相同。

| 项目 | raw 0/1 | codec 0/1 |
| --- | --- | --- |
| refine轮数 | 1 | 2 |
| 最终最大marker残差 | 0 | 0.021021900050300246 px |
| 最后一轮crossing相对规范边缘范围 | 0..0 | -0.02307851900445712 .. 0.02168905817347877 px |
| 满足原exact-sharp endpoint条件 | 48/48 | 0/48 |
| 第1轮movement | 0 | 0.0051652203851553224 px，大于0.005，继续迭代 |
| 第2轮movement | 未执行 | 0.00003994144513796982 px，收敛 |
| 原native resolver | 接受 | 拒绝 |

这不是把一次浮点末位差异放大解释为失败：crossing的光度偏移真实存在，OLS与边界处理按现行代码产生拒绝。0.005是迭代收敛量，不是允许真实裁切或纠偏的阈值。

### 2.2 第15帧：marker搜索已出现额外候选，之后才是拟合偏差

codec15的右下角出现两个marker，中心分别为(1873,1032)和(1871,1032)，原始中心为(1872,1032)。不能宣称所有codec帧都从同一个canonical seed开始。

两个marker组合的seed为：

```text
A = (-0.01315789473684248, 0, 1.0002741228070176, 1, residual=0.5)
B = ( 0.01315789473684248, 0, 0.9997258771929824, 1, residual=0.5)
```

原SameGeometry对origin差与canvas乘scale差逐项比较，得到：

```text
[0.02631578947368496, 0, 1.0526315789473983, 0] < 1.5
```

所以B在原候选顺序中被当作A的重复seed，**在EvaluateGeometry之前就被跳过**。原去重数值函数已以封存seed实测验证，未改1.5阈值。

A经过3轮refine得到与封存WARP完全一致的4字段几何；真实residual为0.07566422767183667 px。最后一轮crossing偏移范围约-0.06978243..0.09784002 px，48条边缘均不满足原exact-sharp证明。

两个组合分别独立调用原EvaluateGeometry时都能读出相同Bootstrap，但最终几何不同；两者各自调用原Unified resolver都拒绝。B的独立评估只是诊断性反事实，不是生产的第二次准入，不能据此声称生产存在AmbiguousGeometry漏洞，也不能改候选顺序来挑有利解。

## 3. 观测过程中的两次失败没有改写

- 初轮工具要求每角色只有一个marker，第15帧触发guard，exit1；3个raw与codec0/1已完成。
- 补充工具最初错误期待两个独立评估只接受一个；与原完整Bootstrap结果核对时exit1。复核原代码发现其先按seed去重，该工具预期不成立。
- 修正的是**工具的计数／一致性断言**，不是生产候选去重或Bootstrap规则。原失败trace、stderr、process记录、旧源码ZIP和build均保留。
- 最终构建仅跑4个纯数值guard，未追加像素回放。两个native批次都不能改称完整成功；最终分析明确保留其失败状态。最终EXE也不能被包装成已从头跑过成功闭环。

已完成归因的证据链：封存WARP trace → 新原媒体像素hash相同 → 原FindEdge/Refine函数结果 → 独立同序OLS → 原native resolver结果 → 原去重代码及数值guard。分析器对被去重组合、停止行和有效样本分别解释。

## 4. 安全修复判据审查

### 已排除，不能重新默认采用

1. **把post-snap scale重新钳到1或强制canonical**：规范尺寸ROI中，scale≥1与完整画布共同把几何压成唯一(0,0,1,1)。合法解的存在不证明像素支持这个解。
2. **增大epsilon或用0.005直接吸附**：混淆浮点舍入、迭代停止条件和实际测量误差。
3. **沿用1.25残差作为绝对纠偏预算**：G1B四个真实1像素位移反例都满足该不充分谓词，不能重启已证伪路线。
4. **从本次最大crossing偏移选一个新阈值**：这是同一编码条件、同一开发批次的观察，不是独立误差上界或Holdout。
5. **放松sharp条件、保留更小残差的marker或尝试第二seed到成功为止**：改变观测合同、候选选择或准入语义，缺乏安全依据。第15帧两个独立候选在原Unified门都失败，也不支持此方向能修复本例。

### 为什么本轮没有准入候选

现有证据已足以证明拒绝机制，却不足以给“未知有损边缘的真实位置”一个可用于安全覆盖判断的误差界。Bootstrap RS/CRC通过、marker残差小或已知开发样例确实未缩放，都不能向生产Decoder提供这个缺失的可观察保证。

因此本轮结论是**当前证据不足以提交可实施的安全准入候选**，不是证明所有估计器都不可能，也不是忽略性能问题。直接放宽门会把研究假设变成生产合同；本轮不这样做。

## 5. 等待确认的后续实施计划

后续交流中，用户已选择“固定 codec 开发模型”作为下一份设计的侧重点。这仅确定设计范围，不是新观测、估计器实现或生产修复的授权。本轮封存完成后，下一轮应先提交固定 codec 的观测合同；下述工具候选和12样本预算仍须另行确认，不能把自动目标续行视作批准。

### 目标

先确定一个明确、可检验的有损边缘观测合同，再判断是否能用它产生保守的边缘位置区间，并保留现有几何/冲突/资源/完整性拒绝。**不是让这3个codec帧通过的定向调参任务。**

### 前置决策（必须确认，不能由当前样本代填）

- 允许依赖的采样、滤波、光度与量化假设是什么；哪些条件能够仅从输入像素检查，哪些仅是合成实验元数据。
- 是否允许引入上述新观测合同。若不允许，则继续保留原门与已知失败，不提交吸附型修复。
- 研究范围是否仅为现有固定codec的开发模型。即便如此，也不能自动升级为Citrix通用误差界或认证产品Profile。

### 获确认后建议的有界顺序

1. **合同论证，不写准入代码**：只审查一种候选思路——局部边缘光度归一化／模板拟合能否给出可检查的位置区间。具体模板／滤波假设和区间构造须先写成数学及输入合同；不自行选新容差。
2. **工具内验证，最多一个候选**：只在合同能成立时实现工具专有估计器；输出区间／不可用原因，不先接入生产。不能把payload/ledger作为拟合oracle。
3. **固定最小验证集**：保留本轮ordinal0/1/15为开发样例、G1光度+1正例及四个必须拒绝的真实位移反例，并保留既有非法尺度/裁切和Bootstrap冲突门。不能将开发样例同时称作Holdout。
4. **先确认新预算再运行**：建议首轮上限12个CPU几何样本、单进程512MiB、累计120秒、证据128MiB；最多一个候选、无参数矩阵、无WARP/数据FEC/Receiver/实屏。由于模型尚未确认，这些是待确认预算，不是执行授权。
5. **再单独决定生产接线**：工具判据通过不等于几何修复或codec整文件恢复通过。只有覆盖依据、反例拒绝和接口影响明确后，才提交生产改动清单、受影响测试及一次原恢复链路验证计划。

### 停止条件

合同无法仅由允许输入支持；出现无法解释的多解；必须拒绝反例进入候选；与原函数/封存像素不一致；需要扩大模型、阈值、样例或预算——均停止，保留未通过结论，先向用户提问。

拟影响范围仅为工具专有诊断文件及对应文档；本轮没有批准修改 `<repo>\libs\PBModulation\src\local_desktop_decode.cpp`、`unified_visual.cpp` 或 `libs\PBDemodD3D11\src\capture_demodulator.cpp`。生产文件的具体改动必须待新判据成立后另行确定。

## 6. 验证、资源与交付

- 原始/codec各3个不同像素，共6个unique样本、7次尝试；两次媒体前缀各16帧，总32帧，绝不声称媒体EOF。
- 新增WARP、数据FEC、Receiver调用0。初轮Bootstrap FEC调用0；补充Bootstrap候选评估上界4、RS copy上界8，经确认允许；不以此代替payload恢复。
- 3个新Release构建，C++20 `/W4 /WX`，原源码与依赖不修改；最后4个纯数值guard及5个分析器篡改拒绝检查通过。没有完整CTest或额外像素重跑。
- 封存审计发现三个构建的MSBuild全局vcpkg集成优先提供了38个FFmpeg头文件。以各构建的`CL.read`记录为依据，这38个文件与封存Step2对应文件逐字节一致；因此不能宣称编译路径完全隔离。保留编译/链接记录、实际头文件身份与等价核对，不改全局配置、不重编译；未来干净环境重建还须核对这项环境依赖。
- 配置/构建/观测/数值guard累计37.938秒，低于共同120秒预算。native观测峰值commit40,218,624 B；native自身512MiB，编译与外层监管Job上限2GiB，两者明确分开。
- 所有输入、trace及逐项证据均有界。128MiB预算包括本轮最终包；原237.30MiB raw与旧交付包仅按hash引用，不复制入本轮证据。

证据根：`<repo>\artifacts\geometry-codec-attribution-20260908-run01`。

| 文件 | 用途 |
| --- | --- |
| `ATTRIBUTION-01.json` | 完整只读归因、6样本、60条旧trace分支重建、原失败状态 |
| `run-01/samples.jsonl` / `ambiguity-01/samples.jsonl` | 原始观测及反事实候选，不能混作生产多候选结果 |
| `context/ambiguity-decision.json` / `seed-dedup-finding.json` | 用户确认的新边界及工具预期错误说明 |
| `SEED_GUARDS.json` | 原SameGeometry纯数值验证，无像素输入 |
| `SOURCE_IDENTITY_FINAL.json` / `RUNTIME_IDENTITY_FINAL.json` | 旧源/依赖保护、3个运行身份分别归属 |
| `EXTERNAL_INPUT_IDENTITY.json` / `STATIC_AUDIT.json` | 外部封存输入、既有反例引用、头文件路径影响及定向静态审计 |
| `FINAL_STATUS.json` / `FINAL_MANIFEST.json` / `POST_SEAL_VERIFY.json` | 逐项目标审计、负结论与封存读回验证 |
| `GEOMETRY_CODEC_ATTRIBUTION_EVIDENCE.zip` | 本轮诊断/设计证据包，不是生产修复发布包 |

为满足128MiB总证据预算，三个构建相同的44个app-local DLL在包中只保存于`runtime/common`，三个EXE分别保存在`runtime/initial`、`runtime/supplement`、`runtime/final`；逐阶段映射见运行身份清单。DLL清单表示原EXE同目录可用文件，不表示每个DLL实际加载或每项codec能力都被使用。不得直接把分开放置的EXE称作可独立启动的发布目录；如以后获准重播，须先在新的目录组合对应EXE与公共DLL，原目录保持不变。系统DLL、编译工具链及旧Step3-B大输入不包含在本包中。

工具及只读复验命令：`<repo>\tools\PBUnifiedGeometryCodecProbe\README.md`。原Step3-B全部输入与历史包、G1/G1B记录、构建目录均保留。HEAD为4a36d0f1b84c94c7388a58b8249a3f1b6ec0634a，index为空；保护文件SHA-256仍为076ef4c9b9f89eabccd323dbe4bffc4dc125ddaf96e6ee437d2cf5b1b1cea306。本轮不更新历史执行记录来改写其结论，不提交或推送Git。
