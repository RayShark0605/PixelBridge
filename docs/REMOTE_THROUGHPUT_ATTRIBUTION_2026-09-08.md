# 非本机吞吐主线：四轮归因与已有观察量补齐

> **状态（2026-09-09 记账）：** 本文属 G22 之后非本机吞吐优化线在 2026-09-08/09 的记录或合同文本，正文未改写。该目标已于 2026-09-09 由用户主动停止（未完成、非技术阻塞）；文中「目标进行中 / 下一步 / 待执行」等表述仅属当时时点，不构成继续执行或现场操作的授权。当前状态见 [暂停交接](REMOTE_THROUGHPUT_RESUME_HANDOFF_2026-09-09.md) 与 [文档状态矩阵](DOCUMENT_STATUS_2026-09-09.md)。

## 0. 目标保持进行中

用户明确要求回到非本机吞吐优化并继续目标模式。本增量为选择有效优化变量补齐证据，不把诊断工具完成当成非本机吞吐目标完成。不转入Encoder退出修复，不重复控制相位实验，不启动Step5–10或codec矩阵。原人工实屏窗口已结束，新的实屏仍须另行确认和核对右屏拓扑。

## 1. 四轮实际证据

来源：`<repo>\artifacts\remote-field-reverse-20260908-run01\four-run-comparison01\FOUR_RUN_COMPARISON.json`。重新核对四轮原seal/final身份和Receiver主计时，不覆盖旧记录。

| 观察 | A1 | B1 | B2 | A2 |
| --- | ---: | ---: | ---: | ---: |
| 主时长s | 238.3155474 | 211.5549573 | 212.4771320 | 205.0048921 |
| 首有用方程等待占主时长 | 2.9870% | 1.5672% | 0.5389% | 1.5306% |
| 最后段存储到最终重开s | 0.0211831 | 0.0217934 | 0.0225514 | 0.0197553 |
| UniqueVisualFPS | 9.942938 | 10.804767 | 10.799969 | 11.422610 |
| VerifiedEncodedBytesPerUniqueFrame | 17577.328 | 17952.144 | 17700.516 | 17516.375 |
| 上项/普通帧18396B载荷上限 | 95.5497% | 97.5872% | 96.2194% | 95.2184% |

最后一行只是格式容量参照，不是逐帧实际发送量、方程独立性、可恢复容量或收益保证。不能与未对齐Sender窗口相除构造远控丢包率。

已进入数据解调的Base/Fine slots没有FEC失败，Chroma仅5/6/6/11次失败；不覆盖捕获前丢失或Bootstrap拒绝画面。Sender全run提交率约14.985–14.991fps，窗口并非Receiver对齐窗口。

下一步优先查有效新画面到达/捕获/处理，而不是最终发布、盲目加FEC或继续细调控制相位。现在仍不能区分远控送达、capture背压和本机CPU/GPU成本；不能直接宣布网络、GPU或resume线性扫描是瓶颈。A2人工结束和预算失败、A1/B1辅助记录限制原样保留。

## 2. 缺口与最小实现

四轮现场final均没有diagnostics/capture流水计数。生产循环已经在DecoderSnapshot更新capture到达/拷贝/交付/丢弃/帧龄、Bootstrap接受/拒绝、CPU/GPU累计计时、Outer接纳及资源计数；但Unified报告原来只有显式diagnostics存在时才输出stageCounters，GUI测量默认没有此collector。历史数值不能事后补回。

本轮仅改产品文件`apps/common/run_report.cpp`：

1. Unified快照带measurement时也输出已有stageCounters；原显式diagnostics输出不变。
2. 同一测量路径增加版本化captureFlow，写出15个已有快照值；capture backend尚未观察到时数值为null、available=false。
3. 明示epoch/session可能重置，lossRate保持null，不制造累计或跨机对齐。drop类别可能重叠，不能直接相加。原累计计时可能嵌套/滞后，覆盖完整性未获证明，0不表示阶段没有工作。
4. 不创建StageDiagnostics，不增加热路径timer/collector、像素采集、readback、缓存或线程。不改接纳、发布、资源限制、wire/Profile/Golden、GUI设置或发送节奏。
5. 普通非测量Unified和legacy报告路径不增加captureFlow；新增字段不会升级goodput或发布状态。

新测试`tests/PBApplication/test_throughput_observation_report.cpp`登记到PBApplicationTests；仅报告和定向测试变更，不修退出逻辑。

## 3. 本地验证与身份边界

独立目录`<repo>\build-remote-throughput-attribution-20260908-run01`只编译当前application_model.cpp、run_measurement.cpp、run_report.cpp和新测试TU；不链接旧PBApplication，只读复用frozen B未改核心库。CL read log核对68项仓库实际输入，无全局vcpkg installed headers。这不是两端产品或现场包构建。

**5 cases / 80 assertions通过**：已有数值传播、剔除新增观察字段后的旧语义parity、没有新collector、capture未就绪null、epoch重置不虚构累计、UINT64文本完整、64KiB记录预算、legacy/显式diagnostics分离。

build-01因独立CMake遗漏PBScreenRegion传递include失败，补齐真实依赖后build-02通过；未弱化断言，原日志保留。最终构建16.125s/peak899,362,816B，测试0.031s/peak3,264,512B；监督上限180s/60s、2GiB Job、1MiB日志、编译并发1。Build identity初检同时匹配CompilerId和测试目标CL日志而拒绝，改为精确选择目标日志，记账错误记录保留。

**新像素观察0、实屏0、codec执行0、未运行完整CTest或压力测试。** 尚未构建Encoder/Decoder应用、未生成新包、未取得新的实际阶段时长或证明提速。

## 4. 后续仍围绕吞吐

状态：REPORT_OBSERVATION_INCREMENT_LOCAL_TESTED / NONLOCAL_THROUGHPUT_GOAL_ACTIVE。下一步继续准备同一路线的新身份诊断候选及受影响应用构建/包验证；未来获准现场观察后，按新画面速率限制发生的层级选择一个优化变量。不得把旧frozen A/B现场数值配给新代码，也不以本次serializer测试宣称总目标完成。

证据根：`<repo>\artifacts\remote-throughput-attribution-20260908-run01`。关键文件：analysis/FOUR_RUN_ATTRIBUTION.json、context/PREFLIGHT.json、context/SCOPED_DIFF.patch、context/BUILD_IDENTITY.json、logs/build-01.log、logs/build-02.log、logs/tests-01.log。

不覆盖/删除旧源码、包、构建、录像、实验原件；无Git暂存/提交/推送。受保护PHASE1_GATE_REPORT.md保持原hash。
