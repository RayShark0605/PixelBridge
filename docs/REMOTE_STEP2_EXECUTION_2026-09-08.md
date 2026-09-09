# Step2 本地实施与录像诊断结果 — 2026-09-08

## 1. 状态

- **本地实施目标：COMPLETE**；逐阶段诊断、独立流式回放、小输入完整恢复、定向负例和现有录像分析均有证据。
- **整个 Step2：PREPARED/PARTIAL，fieldStatus=NOT_RUN**。没有实屏/非本机测试，也没有实测提速或产品发布结论。
- **Step3：NOT_STARTED**。几何准入问题留作单独计划，不在本轮改质量门或算法。
- 起点 HEAD：`4a36d0f1b84c94c7388a58b8249a3f1b6ec0634a`；存在用户既有 Step1 工作树修改。本轮不自动提交或暂存。

合同：[REMOTE_STEP2_DIAGNOSTICS_AND_REPLAY_CONTRACT.md](REMOTE_STEP2_DIAGNOSTICS_AND_REPLAY_CONTRACT.md)。

## 2. 产物及身份

证据根：`<repo>\artifacts\remote-step2-20260908-run01`。

构建根：`<repo>\build-remote-step2-20260908-run01`。

最终工具：`<repo>\build-remote-step2-20260908-run01\tools\PBUnifiedRecordingReplay\Release\PBUnifiedRecordingReplay.exe`。

主要证据：

| 相对于证据根的路径 | 内容 |
| --- | --- |
| `context/preflight.json` / `pre-existing-tracked.diff` | 修改前源码哈希、HEAD、保护文件哈希及既有 diff |
| `build-logs/ffmpeg-install-01.log` | 独立 pinned FFmpeg 构建记录 |
| `build-logs/final-precision-build-04.log` | 最终受影响应用/工具链接记录 |
| `tests/affected-tests-final.json` | 最终窄测试命令与退出码 |
| `tests/fixture-final-01` | 生产 Sender 小输入、FFV1 录像、Sender 诊断 |
| `tests/integration-final-02/checks.json` | 最终干净输出根的小输入恢复与负例 |
| `runs/recording-01/summary.json` / `frames.jsonl` | 原录像整段生产回放结果 |
| `runs/recording-01-loss-analysis.json` | 逐 lane、重复、Receiver 与耗时分解 |
| `runs/recording-prefix-02` | 最终工具前 36 帧、max_digits10 几何拒绝诊断 |
| `runs/recording-prefix-02-geometry-analysis.json` | 对保存的双精度几何值按既有函数进行只读数值分析 |
| `identity/recording-01-*` | 整段运行 R1 的二进制、源码覆盖包、源码与二进制身份 |
| `identity/FINAL_SOURCE_IDENTITY.json` / `FINAL_RUNTIME_IDENTITY.json` | 最终源文件与工具/应用/DLL 身份 |
| `FINAL_STATUS.json` | 最终状态、测试与范围保护检查 |

**身份边界：**整段 `recording-01` 使用中间诊断候选 R1，源码指纹为 `8a35896e0eb0f6f05292060b7de9e23c5cebe6b41aad94ebb0b122bf5e555763`。原二进制和源码覆盖包已保存，未覆盖。

之后仅作代码格式整理、夹具错误清理、满额 EOF 边界处理、Debug DLL 选择、前缀入口和更精确的只读 Bootstrap 诊断。最终版本重新执行小夹具、定向测试和 36 帧诊断；**没有将 R1 整段运行冒称为最终二进制的整段验证**。未因这些诊断改动重复整段录像或做性能 A/B。源码覆盖包应覆盖于对应 HEAD 的独立新 checkout，不应覆盖当前用户工作区。

## 3. 最小完整恢复

生产 `EncoderRuntime/SenderFrameBuilder` 生成一个独立 Unified 画面，夹具将它存成 3 个相同像素观察。Receiver 只获得解码后的 BGRA 与录像时间；源文件仅由外部验证脚本读取。

结果：

- 512 raw bytes，实际 encoded Segment 为 280 bytes；不是将压缩收益冒作链路性能。
- 3/3 观察进入生产解调；同一个 Bootstrap 身份没有在工具输入端去重。
- WholeFileDigest、安全发布、最终文件 reopen 全部通过。
- 外部 SHA-256：`8d35978b98877da24dc6904f1a27aaf031f9103991d3ed868ae7dc938a6a00fd`，源文件与恢复文件一致。
- 诊断 on/off 的逐帧非计时 trace 完全一致，包含每个已接纳 payload 的 BLAKE3。
- 错误 header、首帧截断、尾部截断、缺失输入均失败退出；既有 evidence root 被拒绝且文件哈希未变化。

尾部截断可能发生在小文件已通过所有发布验证之后：此时 `publishedAndReopened=true` 和媒体错误可以同时存在。这不是把截断媒体标成成功；报告保留 error 且 `reachedEof=false`，已正确恢复的文件不需要伪造撤销。

## 4. 现有录像的整段结果

输入 `30Hz_Remote.mkv`：708,677,341 bytes；SHA-256 `7245faaf569ffc999b19e3fbd96a4b5b6eb8e83dee4c7d634fb8251186f33f40`。

本次读取实际确认：H.264、1920×1080、YUV420P limited BT.709/left，原始 PTS time base 为 1/1000；非视频 packet 跳过 2,830 个。

| 指标 | 结果 |
| --- | ---: |
| 解码观察 | 3,622 |
| 生产 Bootstrap 接受 / 拒绝 | 1,260 / 2,362 |
| 接受的不同 Bootstrap 身份 | 234 |
| 接受观察中的重复次数 | 1,026 |
| 不同 `(identity, slot, kind, size, digest)` | 3,510 |
| Base/Fine/Chroma slot 观察 | 11,340 / 1,260 / 6,300 |
| 上述已进入数据解调的 slot FEC/CRC | 全部通过 |
| freshness current | 11,340 / 11,340；只覆盖 Bootstrap 已准入观察 |
| Receiver 新方程 | 1,598 |
| Receiver 重复方程 / 冲突 / 资源拒绝 / 资源延后 | 本次均为 0 |
| Outer decoder 峰值数量 / reservation | 5 / 276,251,496 bytes |
| Demod pending / result queue high water | 1 / 1 |
| 首个 `receiverCarrierAccepted` | observation 2230，PTS 37.150 秒 |
| verified Segments / verified raw bytes | 0 / 0 |
| 整文件发布与重开 | 未发生 |
| 媒体 EOF / 工具 error | true / 空 |
| 本机处理时长 | 169,451 ms；不是链路传输时长 |

Receiver 从视觉控制记录中获得了 40,517,389 bytes 的文件声明。仍未完整恢复任何 Segment，因此不能报告 VerifiedRawGoodput、VerifiedEncodedGoodput 或现场 PASS。

### 4.1 耗时解释

R1 inclusive CPU timers：媒体解码/转换约 83.621 秒，完整 replay demod 约 82.742 秒；其中既有 Bootstrap CPU 总量约 50.292 秒，demod GPU 总量约 13.047 秒，post-GPU CPU 约 11.106 秒。Receiver Process 约 0.501 秒。

这些量有嵌套关系，不能相加推算总耗时。运行初段与受影响目标构建存在 CPU 竞争，不适合建立性能基准；没有据此推断真实 GPU 或 Citrix 性能。

## 5. 最重要的发现：微小几何内缩被现有最小比例门拒绝

历史轻量诊断只验证 Bootstrap 字节一致性，不等于当前生产入口的全部几何准入。不能把历史 3,614 次轻量 Bootstrap 一致直接用于生产接受计数。

最终工具只复查前 36 帧：16 接受，20 返回 `LocalDesktopErasureReason::InvalidGeometry`。20 次拒绝的两份 Bootstrap 都已通过 FEC、CRC、record 验证，timing bit error 为 0。

例如 observation 1 保存的精确拟合值：

```text
originX = 0.0000018894090771937044
originY = 0.00002867913622139895
scaleX  = 1.0000000375203795
scaleY  = 0.9999995452371536
```

`ResolveUnifiedSamplingGeometryInternal` 先在收敛容差内允许接近 1 的 scale，并 clamp，然后按边界重新计算 scale，最后再次要求 `scale >= 1`。

按现有算式，以上拟合经过 Snap 后为 `0.9999999990159327 / 0.9999999734452443`，因此被最后的严格下界拒绝。36 帧只读数值分析与实际接受/拒绝结果完全一致。

**结论范围：**这个小窗口存在可重现的几何数值边界拒绝，值得优先审计；尚未统计整段 2,362 次拒绝的逐原因分布，也不能证明所有拒绝都由此产生。未修改这个分支、quality policy、scale 门、freshness 或 FEC。

## 6. 瓶颈/验证优先级与待确认计划

1. **首先单独审计几何准入的数值边界。** 固定实际像素、拟合值与当前判定；区分合法 1:1 估计误差和真实缩放/裁切。先提出不放宽安全质量合同的方案及负例，再请主人确认；不得直接减小最小 scale 或抬高容差。
2. **随后查明控制面等待。** 本次首个 Receiver 接纳已到 PTS 37.150 秒；早期可解的 payload 并不等于已具备合法 Session/Segment binding。现有源码解释存在这种前置条件，但本次 trace 尚不能独立给出所有控制记录类型的等待归因，不能把差额统称为 FEC 损失。
3. **再讨论计算优化。** 本机媒体和 Bootstrap/解调耗时可指导离线工具效率，但不代替现场 Sender pacing、Present 或捕获唯一帧率测量。
4. 本次样本不支持优先放宽 lane FEC/CRC 或扩大 Receiver 资源；相应已观察路径没有显示损失或资源压力。拒绝观察的数据区尚未解调，不能宣称整个视频的数据区无错。

以上是**待主人确认的后续计划，不自动执行**，也不自动进入 Step3。非本机测试暂不可协助的限制持续生效。

## 7. 测试和保护检查

最终定向验证：

- PBCore：3 cases / 19 assertions。
- Unified CPU：3 cases / 412 assertions，覆盖诊断无效/采样开关不改变接纳、pack/raster 一致性。
- Application：12 cases / 262,614 assertions，包含 Step2 输入合同/PTS、受影响 Step1 计时与发布边界、报告兼容性。大量 assertion 来自已有有界计数测试，不是长压力运行。
- Render：24 cases / 407 assertions，使用 mock backend，不创建实屏窗口。
- 最终集成脚本：7 项检查；新输出根重新通过完整恢复和诊断开关一致性。
- 最终受影响的 Encoder、Decoder、工具均构建成功。未运行 GUI。

`docs/PHASE1_GATE_REPORT.md` 始终未修改、未暂存、未提交，SHA-256 保持 `076ef4c9b9f89eabccd323dbe4bffc4dc125ddaf96e6ee437d2cf5b1b1cea306`。Git HEAD 与 index 不变；历史录像、交付、构建、失败记录均保留。首次夹具 packet 单次分配不足和 mock shutdown 修正、增量链接失败等过程日志也未覆盖或删除。
