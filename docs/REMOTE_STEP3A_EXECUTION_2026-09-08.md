# Remote Step3-A 本地实施记录 — 2026-09-08

## 0. 结论与授权范围

**已完成本次确认的 Step3-A 小型确定性离线工具与验证。整个 Step3 仍为 PARTIAL，非本机现场为 NOT_RUN。**

本轮只新增独立工具与文档状态增量；不修改产品源码、几何准入、SC6-V3/layout 10、FEC/CRC、digest、安全发布、reopen、冲突拒绝或资源策略。没有硬件 GPU、实屏、远控、录屏、Citrix／网络改动或输入自动化，也没有自动提交、暂存、推送或启动后续 Step。

前置证据为 Step2 已可复现的 OfflinePixels 回放和 G1/G1B 的几何失败结论。用户当前不能配合非本机场景，故不把现场缺口用 WARP 或合成像素替代。

入口：[吞吐路线](REMOTE_CHANNEL_THROUGHPUT_OPTIMIZATION_ROADMAP.md)、[工具及完整复验命令](../tools/PBRemoteThroughputStep3A/README.md)。

## 1. 实际修改和接线

新目录 `tools/PBRemoteThroughputStep3A`：

- `support.*`：固定 512 字节源；生产 `EncoderRuntime` + 无窗口 presentation；封存 4 张实际生产 raster。Session 继续使用 OS CSPRNG，不注入固定 Session ID。
- `scenario_source.*`：固定源 ordinal、合成 PTS、9 个既有 Simulator 变换／时序清单；只向 `RecordedPixelSource` 提供 BGRA8 与必要时序。
- `main.cpp`：复用原 `RunRecordedPixelReplay`、软件 D3D11 WARP、生产 `CaptureDemodulator` 与 `ReceiverPipeline`，不实现第二套恢复路径。
- `run_suite.py`：串行固定两轮，独立文件验证、完整非时钟结果比较、阶段归因、哈希与资源约束。
- `test_guards.py`：create-only 的受影响负例，所有失败输入、日志和结果保留。
- standalone `CMakeLists.txt` 与 `README.md`：不注册产品目标，不新增依赖，不创建窗口。

已有文件仅更新本路线及文档索引的当前状态。工作开始的已有改动、未跟踪源码和历史交付不被收编成此次新增实现。

构建根：`<repo>\build-remote-step3a-20260908-run01`。

证据根：`<repo>\artifacts\remote-step3a-20260908-run01`。

只读链接依赖来自现存 `build-remote-step2-20260908-run01`；本次记录具体链接库哈希与新 EXE 身份，不把工具当成重建后的产品双端或 B0/M1 现场候选。

## 2. 最小闭环与确定性边界

固定源大小 512 B；生产压缩后的 verified encoded bytes 为 280 B。4 张 1920×1080、BGRA8、rowPitch=7680 输入封存一次，合计 33,177,600 B。

正确输出：

```text
SHA-256 8d35978b98877da24dc6904f1a27aaf031f9103991d3ed868ae7dc938a6a00fd
BLAKE3  a10d2ee50c9a4366be4f7b75c4ae2a4bce3560f8981a1e1a2c9ff0d49799242f
```

先用 `clean-proof` 的 4 个观察证明完整链路：源 → 生产像素 → 原 Decoder/Receiver → whole digest → safe publish → final reopen → 进程外 512 B 逐字节比对。随后仅执行固定 9 场景×2 轮，共 76 个观察。**本轮总计 80 个解调观察；不是长测或参数搜索。**

两轮从同一封存输入开始，不重建随机 Session；每次使用新输出目录、全新 Receiver 状态，不载入上一轮恢复状态。比较全部非时钟逐帧数据、完整变换清单、像素哈希、accepted payload digest、FEC 迭代、Receiver 非时间计数及阶段调用数，9/9 场景均精确一致。所有接受的 payload 均与同 ordinal 的 clean 解码一致。

## 3. 实测结果

下表为**每一轮**结果；accepted blocks 是全部解调观察的累加，包含重复，不是唯一有效帧或 goodput。

| 场景 | 观察数 | accepted blocks | 全文件发布／重开／独立字节验证 | 关键证据 |
| --- | ---: | ---: | --- | --- |
| clean | 4 | 60 | PASS | 四个实际不同帧身份，各 15 块 |
| repeat-each | 8 | 120 | PASS | 源映射 `0,0,1,1,2,2,3,3` |
| burst-loss | 2 | 30 | PASS | 仅 ordinal `0,3`，PTS 空隙保留 |
| neutral-chroma | 4 | 40 | PASS | Chroma 全部 lane erasure；Base/Fine 每帧共 10 块 |
| quantize-6 | 4 | 60 | PASS | 固定 6-bit BGR 量化下，此样例仍每帧 15 块 |
| bootstrap-conflict | 4 | 0 | NOT_RECOVERED，未发布 | 两份有效 Bootstrap 冲突，最终 erasure=18 |
| local-freshness-mix | 4 | 52 | PASS | freshness[0] 均失效；每帧 Base slot 0、Chroma slot 10 FEC 拒绝，其他 13 块通过 |
| marker-plus-one | 4 | 0 | **已知失败复现，未发布** | 原 Unified 几何门拒绝，未调整任何容差 |
| shift-right-one | 4 | 0 | NOT_RECOVERED，未发布 | origin=(1,0)，原严格几何门拒绝 |

`marker-plus-one` 的四次几何结果均为：

```text
originX = 0.00011337968305724644
originY = 0
scaleX  = 0.9999999385577532
scaleY  = 1
最终生产 BootstrapAccepted = false
BootstrapErasure = 12 (InvalidGeometry)
GeometryStatus = 4 (Rejected)
acceptedBlocks = 0
```

这与 G1/G1B 的已知数值脆弱性一致；**不是已修复，不是负例豁免，更不是产品 geometry PASS**。

6 个成功场景每轮均通过四项生产发布门与独立文件比对。所有固定场景未观察到新的 Outer conflict/resource rejection 或 stale-result drop；成功场景 Outer peak active decoder=1、reserved bytes=4384。它们只是本次小样例资源观察，不证明极限资源容量。

## 4. 分析器首轮失败及保留处理

`logs/analysis-01.log` 保留一次真实分析失败：工具最初误将 G1 中的“通用 Bootstrap 接受”写成最终生产回放也应 `bootstrapAccepted=true`。

返回当前源码和实际 trace 核对后，`DecodeUnifiedBootstrap` 在 `ResolveUnifiedVisualSamplingGeometry` 拒绝时，将 Bootstrap erasure 改为 `InvalidGeometry`。因此最终入口必须是 false。修正范围仅为工具断言，并增加专门回归；不修改生产几何、像素、已有 trace 或通过标准。

修正前源码：`context/tool-source-before-analysis-correction.zip`；解释：`context/analysis-01-failure.json`。随后对现存回放完成 `analysis-02`，再增加清单参数／链路核对得到 `analysis-03`；收口时补齐证据扫描目录项／深度／文件增长界限后生成最终 `analysis-04`。所有旧分析和日志保留；没有因此重新执行 Decoder。

## 5. 实际测试与未执行项目

- 新 standalone Release 构建通过：MSVC C++20 `/W4 /WX`，记录于 `logs/configure-01.log`、`logs/build-01.log`。
- C++ 固定自检 12 项通过：PTS 顺序／timebase／120 s 界、duplicate/burst 映射、越界 ordinal、pitch、Simulator 资源界、缺失 reference、非法量化、identity 确定性。
- 首条端到端闭环通过；固定两轮回放 76 个观察，非时钟精确重复 9/9。
- 最终 guard 31/31 通过，结果为 `guards-03/RESULT.json`：JSON 重复键／NaN／体积、清单 schema／count／hash／路径、truncated raster、观察数／行长、PTS／payload／发布门篡改、几何层级、未到达阶段 null、禁止 live rate、固定参数／清单链路、目录深度／空目录项界限、既有目录拒绝与输入不变。guard 未增加解调观察。
- 受影响源码／文档与 Git diff、保护文件、历史 manifest、源／运行文件身份复核由 `FINAL_STATUS.json` 和 `POST_SEAL_VERIFY.json` 记录。
- **未执行**完整 CTest、硬件 GPU、GUI/native screen Gate、原 MKV 重放、新录屏、Citrix、非本机、迟加入、大文件、压力测试或 codec 矩阵。

## 6. 必须保留的限制

1. tiny source 的 DirectRepeat 在第一张可用帧即完成，Receiver telemetry 在完成处收口，后续帧仍解调但不能用来宣称 Receiver 去重或多帧 Outer 恢复能力。
2. 合成 PTS 不等于采到的生产帧时钟。`UniqueVisualFPS`、live channel goodput、simulated verified goodput 和 pre-FEC BER 均不生成有效性能样本。原报告的 encoded bytes/unique frame 也不得外推远控吞吐。
3. Bootstrap/geometry 早拒绝后的 freshness、FEC、CRC、Outer 未执行项为 null；不能把默认计数当作“零错误”。仅显式 CRC 决定进入相应分母。
4. 软件 WARP + 合成像素仅证明本地链路与确定性诊断；新旧协议、真实录制批次划分、受码率约束的 codec、现场预测能力仍未验证。
5. G1B 结论保持 `CRITERION_INSUFFICIENT_NO_ADMISSION_CANDIDATE`；G2 和 Step4 未启动，Step1/Step2 现场缺口仍为 NOT_RUN。

## 7. 封存、保护与待确认的下一步

开始时 HEAD 为 `4a36d0f1b84c94c7388a58b8249a3f1b6ec0634a`，index 为空。更新两份当前状态文档前，1148 项已有源码哈希全部一致；既有 Step2 runtime manifest 70 项、G1 manifest 124 项、G1B manifest 159 项全部复核一致。

保护文件 `docs/PHASE1_GATE_REPORT.md` 未修改、暂存或提交，SHA-256 保持：

```text
076ef4c9b9f89eabccd323dbe4bffc4dc125ddaf96e6ee437d2cf5b1b1cea306
```

最终 source、runtime、证据清单与 create-only zip 均在本轮证据根。历史包、构建、录像、失败分析和实验记录全部保留。交付后停止，不后台等待或自行安排现场。

**下一步建议先确认 Step3-B 的小范围设计，不直接开始编码矩阵：**

1. 只读核对现有 FFmpeg/codec 能力与 Step2 录像元数据，选一个可复现的有状态 codec 方案；不能把录像元数据当成 Citrix 内部编码参数。
2. 提交固定 codec、像素格式／颜色、码率／VBV、GOP、PTS 和短序列规模；这些参数及是否纳入多帧恢复样例，先由用户确认。
3. 获准实现后，用单独的有界 encoder/decoder 子进程产生码流、inspect 实际属性、把解码像素接回本工具的原 Receiver；仍不输入外部 payload 真值。
4. 只安排 clean 与一组固定码率约束对照、codec 失败负例、清单与整文件复核，不默认搜索矩阵、扩大压力测试或需要用户实机配合。
5. codec 支持、资源预算或观察结果有高影响不确定性时停止并提问。无论本地结果如何，现场仍为 NOT_RUN，实屏另行确认并先核对右屏拓扑。

上述是等待确认的后续计划，**本轮没有开始其中任何一项**。
