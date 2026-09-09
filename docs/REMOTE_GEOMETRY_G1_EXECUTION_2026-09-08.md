# 几何专项 G1 实施与结果 — 2026-09-08

## 1. 最终结论

**G1 的夹具、单一工具候选、窄验证和证据交付已完成；候选结论为 `NOT_PROMOTED_NO_NEW_ADMISSION`，不是生产修复成功。**

- 主人已确认按最终可观察像素分类：逐字节相同的裁切回填归为等价样例；可观察裁切/位移、非法尺度和冲突仍遵守原规则。
- 未修改生产 Encoder/Decoder、公共 API、几何准入、阈值、profile/layout、wire/FEC 或恢复语义。
- 原 1126 个封存源文件不变；仅新增独立工具目录和本报告。无 Git 暂存/提交/推送。
- Step2 仍为 `PREPARED/PARTIAL`，`fieldStatus=NOT_RUN`；G2 和主路线 Step3 未开始。
- **不进入 G2，不尝试第二个候选，不调大容差，不自动继续本会话。**

## 2. 实际实现

源码：`<repo>\tools\PBUnifiedGeometryG1`。该目录故意不加入产品的 CMake 构建图。

| 文件 | 实际内容 |
| --- | --- |
| `CMakeLists.txt` | 独立 Windows/MSVC Release 构建，显式复用封存库和工具专用 FFmpeg，无依赖安装 |
| `fixtures.h/.cpp` | 生产 SC6 V3 raster、15 个 Transport blocks、固定真值正负夹具 |
| `candidate.h`、`candidate_impl.inc` | 单一 canonical 1:1 候选的 marker/边缘/Bootstrap 验证及有界诊断 |
| `prepare_reference.py` | 在新 build 下生成带 diff 和哈希的工具派生源，不改生产源 |
| `main.cpp` | 25 夹具、8 数值回归、11 候选合同检查、36 录像观察及逐块摘要 |
| `run_checks.py` | create-only、无窗口、120 秒外层超时的手动执行器 |
| `analyze_results.py` | 与封存基线比较，验证计数、摘要、测量可用性，不自动晋级 |
| `test_tool_guards.py` | 5 项工具错误输入/防覆盖/坏证据负例 |

候选只有一种：在 1920×1080 上重新检查四个 marker，并对每个 marker 的两轴各六条边缘执行已有 sharp adjacent-center 条件，共 48 条。沿用原 `1e-9` roundoff 和半像素 point support 区间，**没有新增有损噪声容差，也没有拿 `0.005 pixel` 收敛量当真实误差上限**。

通过上述支持条件后，工具尝试 `(originX, originY, scaleX, scaleY)=(0,0,1,1)`。原公共头和实现不变；实验方法只在生成的工具头/源中存在。原有函数完整保留，新增入口只替换实验的 Bootstrap/几何选择，继续使用同一校准、局部 freshness、phase、Inner FEC、CRC 和身份检查实现。生成 diff 位于最终 build 的 `generated` 目录。

Decoder/候选只拿到同一帧像素及既有 policy；期望 payload 仅由独立夹具检查器在解码后比对。没有生产 Receiver、文件发布、源文件旁路或 ACK。

资源边界：512 MiB 进程 Job、32 MiB FFmpeg 单分配上限、软件单线程解码、最多 36 录像观察、120 秒外层超时。诊断按固定条数/固定大小结构输出，执行器检查 stdout ≤1 MiB、stderr ≤64 KiB；分析器还有文件、记录数和单记录大小限制。未执行的边缘测量记 `null` 并携带测量次数。

## 3. 更小的可重放拒绝样例

新样例名：`one-marker-edge-plus-one`。

1. 使用生产 raster 生成 sequence 40 的规范 1920×1080 帧，包含 15 个合法测试 Transport blocks。
2. 只把左上 marker 的像素 **(20,47)** 的 B/G/R 各增加 1，alpha 不变。
3. 不改变尺寸、坐标、尺度、Bootstrap 内容或数据区，不做裁切、缩放或重采样。
4. 当前通用 Bootstrap 返回接受，但 Unified 几何门拒绝；CPU 原入口在细化阶段返回 `InvalidGeometry`、0 blocks。

实测拟合：

```text
originX = 0.00011337968305724644
originY = 0
scaleX  = 0.9999999385577532
scaleY  = 1
markerResidualPixels = 0.00119333219059925
```

改变后的像素 BLAKE3：`d47f15c94e68df29f70b8e9e48f3866de0d8ad948ec734fdeb1ea98e99acb6e2`。

候选也拒绝：第一条边缘的 integer crossing 偏差约 `0.0013054830287195784`，不满足原 sharp 条件。这个样例把“真实几何没变、单像素亮度扰动影响连续拟合、近最小比例拒绝”固化为更小的回归；**它没有被本候选修复**。

## 4. 夹具结果

25 个像素观察中，原路径 10 个观察有 blocks、共 148 blocks；候选 7 个观察有 blocks、共 103 blocks；**新增接受为 0**。

| 样例组 | 数量 | 原路径 | 候选 | 解释 |
| --- | ---: | --- | --- | --- |
| 精确 1×、全局 +1、四个像素等价回填 | 6 | 各 15 blocks | 各 15 blocks | 同槽、同长度、同 payload |
| 四边较小 ROI 裁切 | 4 | 0 | 0 | 几何拒绝保持 |
| 裁切后另一侧回填，形成 1 像素位移 | 4 | 0 | 0 | canonical origin 不受像素支持 |
| 0.75× point downscale | 1 | 0 | 0 | 非法尺度不被接纳 |
| 1× 对称模糊 | 2 | 各 15 | 0 | 原路径可恢复，候选过于严格；不能替换原路径 |
| 1.125× point upscale | 1 | 15 | 0 | 非本候选的规范尺寸范围 |
| 1.125× point 后的两轴半像素边界模糊 | 2 | 0 | 0 | 既有覆盖负例保持拒绝 |
| 单 marker 边缘像素 +1 | 1 | 0 | 0 | 上述更小拒绝复现，未解决 |
| 两份 Bootstrap 身份撕裂、Bootstrap 抹除 | 2 | 0 | 0 | 校验/冲突拒绝保持 |
| 单个局部 freshness patch 撕裂 | 1 | 13 | 13 | 相同局部擦除、相同逐块 payload |
| expected identity 冲突 | 1 | 0 | 0 | Bootstrap 可解仍不越过身份门 |

其中 14 个明确 must-reject 样例在两条路径都为 0 blocks。等价回填是已获确认的等价例，不被伪装成负例通过。每个已接受夹具 block 都独立核对了实际字节，不只是计数。

## 5. 原录像短窗口结果

输入：`<repo>\30Hz_Remote.mkv`。

- BLAKE3：`f321c148cc75444fb860e80d21a0083d0b3ef3a79311940558f426bdfba7b0a0`。
- FFmpeg 8.1.1，既有工具专用 pinned DLL，原媒体合同不变。
- 前 36 个观察，PTS 0..583、time base 1/1000；不是整段视频。
- 原路径仍接受 16 个观察、240 blocks；候选接受 0 个观察、0 blocks。
- 36 个观察全部止于 `NonIntegerSharpEdge`。第一观察偏差约 `0.0013321361281199984`；后续 endpoint/origin 测量未执行，最终 trace 为 `null`，不是零误差。
- 与封存 Step2 前缀的顺序、PTS、几何前四字段、接纳及 **240 个逐块摘要**全部一致；第五字段 marker residual 与独立几何审计逐观察一致。

结论：现有 sharp point support 不能直接用于这段有损录像的纠偏。**没有提速证据，没有整文件恢复/发布/reopen 结论，没有非本机 PASS；不能外推整段 2362 次拒绝的全部原因。**

## 6. 验证清单与过程记录

- 最终独立工具及 `PBLocalDesktopBootstrapTests` 构建成功。
- 8 数值边界样例、25 像素观察、11 项候选合同检查通过。
- `[fixed-canvas]`：1 case / 132 assertions PASS。
- `[point-downscale],[point-coverage],[remote-coverage]`：3 cases / 332 assertions PASS。
- 最终只读证据分析：21 项检查通过。
- 5 工具负例：错误 CLI、缺失录像、已有 evidence root、已有不同生成源、篡改 trace block 计数。均失败退出并保留证据；防覆盖用例哨兵不变；坏 trace 标为 `EVIDENCE_INVALID`。

过程差异全部保留：

1. 初次分析把旧 Step2 的四字段 `geometryFit` 与本工具含 residual 的五字段直接比较，正确触发失败。随后按已知 schema 对齐前四字段，并额外与旧审计的五字段比较；没有放宽数值比较。原失败 JSON 和脚本副本保留。
2. 初版候选在未执行到的测量上输出初始零值。最终版本改为次数 + `null`，在全新 build03 保留 build02 后重新跑同一窄集；接纳、payload、拒绝原因和 workUnits 均与初版一致。没有尝试第二种几何候选。

没有完整 CTest、完整录像、长时间压力、GUI、WARP/native、实屏或非本机测试。原屏幕、焦点和输入不受本工具操作。

## 7. 产物、身份与复现

最终构建：`<repo>\build-geometry-g1-20260908-run03`。

最终工具：`<repo>\build-geometry-g1-20260908-run03\Release\PBUnifiedGeometryG1.exe`。

工具 SHA-256：`0ce0156e59cb2da4081c61369dda0a51fffd2ea6652c90ea6038cdb76c8bd15c`。

证据根：`<repo>\artifacts\geometry-g1-20260908-run02`。

| 证据根下的条目 | 内容 |
| --- | --- |
| `context/start.json` | 开始时源码、授权分类与范围 |
| `logs/build-final-02.log` | 最终编译记录 |
| `runs/fixtures-final-02` | 最终夹具与现有窄测试 |
| `runs/recording-final-02` | 最终 36 观察、媒体身份与逐块摘要 |
| `runs/guards-final-01/checks.json` | 工具防覆盖和坏证据负例 |
| `RESULT_FINAL_CHECKED.json` | 最终 21 项分析与未晋级结论 |
| `source-candidate-r2.zip` | 初版候选源码保留 |
| `SOURCE_FINAL.zip`、`FINAL_MANIFEST.json`、`FINAL_STATUS.json` | 最终源码覆盖包、身份和保护校验 |

未来获得复查授权时，在新输出根运行，不能重用历史目录：

```powershell
$repo = '<repo>'
$build = '<repo>\build-geometry-g1-20260908-run03'
$newRoot = Join-Path $repo ('artifacts\geometry-g1-replay-' + [Guid]::NewGuid().ToString('N'))
& '<python>' "$repo\tools\PBUnifiedGeometryG1\run_checks.py" --repo $repo --build $build --output-new "$newRoot\fixtures"
if ($LASTEXITCODE -ne 0) { throw 'Fixture run failed; evidence retained' }
& '<python>' "$repo\tools\PBUnifiedGeometryG1\run_checks.py" --repo $repo --build $build --output-new "$newRoot\recording" --recording "$repo\30Hz_Remote.mkv"
if ($LASTEXITCODE -ne 0) { throw 'Prefix run failed; evidence retained' }
```

这会复现候选未晋级，不会调用屏幕或发布文件。独立新 build 的准确配置命令见工具 README；派生源依赖封存 source/lib 身份，身份不同须先停下核对。

`runs/guards-final-01/altered-trace` 是人为篡改的负例，禁止混入正常录像/夹具统计。

## 8. 后续边界

本轮已回答“现有 sharp proof 的 canonical 候选是否足够”：对本样本不够，因此不应进入生产 G2。当前几何拒绝仍未修复。

若后续继续，需另行确认一个新的小范围研究，针对有损边缘建立能够区分估计误差与真实位移/裁切的证据判据，再决定是否尝试候选。不能只调高 roundoff，不能为了让单样例通过而删检查。该研究、本机实屏和非本机验收均未获本轮自动授权。

保护文件 `<repo>\docs\PHASE1_GATE_REPORT.md` 不改、不暂存、不提交；历史 build01/build02、Step2 构建、录像、源包、失败记录全部保留。原纯视觉单向通路、工具独有 OfflinePixels/live 光标排除边界、摘要、安全发布、重开验证、冲突拒绝和资源限制继续生效。
