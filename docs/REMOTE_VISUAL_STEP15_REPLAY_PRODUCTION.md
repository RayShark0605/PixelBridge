# RemoteVisual LF4 Step 15：Replay v2 production live/offline 一致性

日期：2026-09-02  
状态：`DONE`  
边界：本步骤关闭 Replay 可复现性，不公开 LF4 GUI/CLI，不声明 Certified Profile、真实双机文件发布、provider matrix、goodput 或 soak。

## 1. 完成出口

Step 15 要求 Replay 保存 Decoder 实际看到的 selected ROI，并让 offline mode 在没有远控软件时重放同一条 Bootstrap、GPU demod、FEC、Receiver、WholeFileDigest 与 publish 路径。本轮同时关闭了两类出口：

1. 无窗口 WARP 生成一个完整 LF4 receiver-only Replay；live 与 offline 两次独立进入同一 production `CaptureDemodulator` 和 `ReceiverPipeline`，均通过 WholeFileDigest 并安全发布同一外部字节。
2. Step 02 已封存的真实 RDP Direct、Shape、LF4 receiver-only Replay 各自用 production offline Decoder 重放，均复现原先的“Transport 可恢复、但缺少 Control descriptor，因此不得发布”分类。

Replay 从不向 Decoder 注入 sender raster、canonical Bootstrap、expected Transport 或文件字节。测试源只用于在 Replay 之外核对最终发布文件；Replay 内对应 presence 均为 absent。

## 2. Replay v2 向后兼容扩展

文件格式版本、file header、record header 和 footer 分别保持 `2`、256 bytes、512 bytes 和 96 bytes。原有 demod observation 基础字段及其偏移不变。基础 flags 的 bit 5 只表示保留区中存在 `production detail version 1`；该 bit 缺失时，138..455 必须全零，新 reader 按历史 v2 语义读取。

可选 detail 保存：

- Bootstrap 中实际恢复的 profile/layout 与 FrameSequence presence；
- 连续 geometry 的 status、origin 和 X/Y scale；
- result kind 与 temporal disposition；
- evaluation/padding、FEC/CRC/identity、iterations；
- raw accepted 与 temporally admitted Transport/Control 数量；
- LF4 metric/freshness/unreliable counters 与 compact readback bytes；
- GPU timing presence 和 live duration；
- Receiver carrier validation 与是否推进 receiver state。

`senderTruthAvailable` 是独立 presence。receiver-only production observation 必须使它为 false，并使 false-accepted、compared coded bits、erroneous coded bits 保持零；这些零不被报告为“测得 0 false accept”。writer 和 reader 共用 fail-closed 校验，拒绝 unknown enum/detail version、非有限 geometry/metric、无 presence 的非零字段、admitted 大于 raw、错误 bit 大于 compared bit、以及 Receiver flags 自相矛盾。

GPU duration 被保存在 live evidence 中，但不参加 live/offline bit-exact comparison；第二次 GPU 执行的耗时不是信道判决。layout、geometry、temporal、FEC/CRC/identity、metric/freshness、accepted/admitted counts 与 Receiver disposition 则必须精确一致。历史 v2 observation 没有 detail presence 时只比较原有基础字段。

## 3. 同一 production 路径

offline Replay 只把已验证的 BGRA8 selected ROI 重建为 PB-owned D3D11 texture，之后调用配置 profile 对应的同一个 `CaptureDemodulator` 和 `ReceiverPipeline`：

```text
ReplayV2Reader
  -> PB-owned BGRA8 texture on exact recorded adapter LUID
  -> CaptureDemodulator::Submit
  -> CompleteStage (Bootstrap)
  -> optional CompleteStage (LF4 metric/FEC)
  -> temporal admitted indices
  -> ReceiverPipeline / ReceiverIngress / Outer
  -> WholeFileDigest
  -> PBStorage no-overwrite publish
```

Direct、Shape、既有 RemoteVisual 仍是单阶段；LF4 是最多两阶段。每一阶段都用 `ID3D11DeviceContext3::Flush1` 的 OS event 证明 GPU retirement，不能用 `Flush` 冒充完成。第二阶段使用新的 one-shot completion/device-removal event，避免 reset 一个可能与 device removal 竞争的 event。超过两阶段会先外部退休意外 work，再 fail closed。

Replay 必须使用记录中的 exact adapter LUID。测试 WARP Replay 只在记录的 LUID 与当前 WARP LUID 完全相同时启用，不构成不同 adapter fallback。offline sealed frame 没有 live backlog，因此 frame-age 上限使用有界 60 s，以允许慢 WARP 重放；live capture 仍保持 250 ms stale policy。

## 4. Recorder 资源与发布语义

实际 `RemoteVisualReplayRecorder` 继续满足：

- fixed preallocated slots；默认 queue 2，hard maximum 16；
- queue 满只丢 Replay evidence sample，不阻塞 production demod/Receiver；
- 默认 256 frames / 2 GiB，hard maximum 2048 / 16 GiB；
- 只保存 selected ROI；sender canonical raster 与 canonical Bootstrap 不写入；
- worker 以 `.partial` create-new 写入，成功 footer/flush 后 no-overwrite rename；
- demod observation 以 `(CaptureEpoch, CaptureObservation)` 绑定已经写入的 capture；
- writer/reader 在分配前验证长度、frame count、CRC32C 与 whole-stream BLAKE3。

Step 15 headless Gate 使用真实异步 recorder，而不是直接拼装文件。6 个 capture 与 6 个 production detail observation 全部写入，queue HWM 不超过 2，capture/demod drop 均为 0。

## 5. 完整 LF4 live/offline Gate

输入为 1 byte `0x5A`，outer mode 为 DirectRepeat。sender carousel 是 Session、Manifest、Segment、Data 四个唯一视觉帧；Session 后额外插入两个相同 FrameSequence 的 receiver observations。结果：

| 项目 | live production | offline production |
| --- | ---: | ---: |
| selected-ROI captures | 6 | 6 |
| demod results | 6 | 6 |
| duplicate suppressed | 2 | 2 |
| LF4 metric-bearing observations | 4 | 4 |
| temporally admitted Transport | 4 | 4 |
| Outer unique symbols | 1 | 1 |
| observation comparisons / mismatch | N/A | 6 / 0 |
| WholeFileDigest | PASS | PASS |
| safe publish | PASS | PASS |
| external bytes | exact `0x5A` | exact `0x5A` |
| sender truth in Replay | absent | absent |

两个 final path 位于独立目录，均由 PBStorage create-new 发布；测试独立读取并比较最终字节，而不是只相信内部 Completed 状态。

Release 定向结果：`[production][step15]` 的完整发布用例全通过。

## 6. 三份真实 RDP dataset

只读输入位于 gitignored evidence root `build-p1_5-evidence/step02-rdp-20260901/`。测试通过显式环境变量 `PB_REMOTE_VISUAL_REAL_REPLAY_ROOT` 注入根目录；普通开发机没有这些证据时用例明确 SKIP，不伪造实机输入。

| dataset | captures / Bootstrap | duplicate sequence | production admitted Transport | FEC / CRC / identity | 最终分类 |
| --- | ---: | ---: | ---: | ---: | --- |
| Direct | 8 / 8 | 7 | 42 | 0 / 0 / 0 | `Stopped`, descriptor absent, no publish |
| Shape | 8 / 8 | 7 | 32 | 0 / 0 / 0 | `Stopped`, descriptor absent, no publish |
| LF4 | 8 / 8 | 7 | 4 | 0 / 0 / 0 | `Stopped`, descriptor absent, no publish |

三份 Replay 都是 capture-only receiver evidence，没有 Session/Manifest/Segment Control。production Decoder 因此只能验证首个 unique raster 的 Transport，不能建立文件会话；结束为 no-publish 是正确且与原始 Inspector 一致的失败分类。Release `[real-replay]` 用例全通过。

## 7. 验证矩阵

主要命令：

```powershell
cmake --build build-presentation-release --config Release --target PBRealCaptureReplayTests PBApplicationTests --parallel 4
& build-presentation-release/tests/PBRealCaptureReplay/Release/PBRealCaptureReplayTests.exe
& build-presentation-release/tests/PBApplication/Release/PBApplicationTests.exe '[step15]'
$env:PB_REMOTE_VISUAL_REAL_REPLAY_ROOT = (Resolve-Path build-p1_5-evidence/step02-rdp-20260901).Path
& build-presentation-release/tests/PBApplication/Release/PBApplicationTests.exe '[real-replay]'
```

覆盖项包括：v1 reader compatibility；历史 v2 receiver-only observation；production detail roundtrip/presence；corrupt/truncated/oversize/checksum；capture/observation linkage；frame/file/raster bounds；`.partial` cleanup；no-overwrite；queue-full evidence-only drop；exact adapter identity；LF4 staged completion；live/offline semantic consistency；Direct/Shape/LF4 real dataset classification；WholeFileDigest 与 final bytes。

最终 affected Release 7-suite 为 7/7 PASS、188.94 s。MSVC ASan 首轮中另外 6 个套件全部通过，但新增 Replay Gate 使 `PBApplicationTests` 超过历史 CTest 60 s 外层上限；没有 sanitizer failure。测试自身仍有更紧的有界 deadline，因此将该目标外层 timeout 调整为 600 s并重新配置两棵build tree后，ASan `PBApplicationTests` 在107.73 s通过。由此7个affected ASan套件全部有通过结果，未删除、skip或放宽断言。

## 8. 剩余边界

- 本步骤不把 LF4 放入公开 live Decoder profile inventory；公开 exposure 仍属于 Step 17。
- aggregate LF4 telemetry 与 RunReport.2 的 authoritative denominator/null semantics 尚未完成；这是 Step 16。
- 三份真实 RDP Replay 是无 Control 的 capture-only corpus，不能证明真实双机 WholeFileDigest/publish；Step 20 仍需 1/2/5 Hz 文件传输 Gate。
- WARP 完整发布证明 production 语义和 Replay 一致性，不替代真实 provider/hardware performance certification。
