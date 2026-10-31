# 当前运行时选项 / Current runtime options

[中文使用指南](UNIFIED_USER_GUIDE.md) · [English user guide](USER_GUIDE.en.md)

本表按当前 adapter 中的 option literal 核对，只列名字，不把研究选项推广成产品能力。CLI 的组合约束以 parser、Validate*Config 与帮助为准；未知/重复/冲突参数不得静默回退。

The names below are audited against current adapters. Presence is not product certification or permission to combine every option. Parser/config validation remains authoritative. GUI users should use the user guide.


## Encoder

Source: `apps/PixelBridgeEncoder/encoder_runtime_cli.cpp`

- `--channel`
- `--compression`
- `--compression-level`
- `--control-repetitions`
- `--experiment-monitor`
- `--fullscreen-native-size`
- `--fullscreen-raster-width`
- `--fullscreen-sampling`
- `--grayfast-extended-visits`
- `--grayfast-short-initial-airtime`
- `--grayfast-spatial-interleave`
- `--journal`
- `--logical-fps`
- `--loop`
- `--manual-stop`
- `--origin`
- `--profile`
- `--protected-monitor`
- `--remote-metadata`
- `--remote-provider`
- `--report`
- `--run-id`
- `--seconds`
- `--segment-target-mb`
- `--session-state-root`
- `--single-monitor-fullscreen`
- `--source`


## Decoder

Source: `apps/PixelBridgeDecoder/decoder_runtime_cli.cpp`

- `--backend`
- `--budget-bound-decoders`
- `--channel`
- `--decoder-instance-memory-mib`
- `--decoder-memory-mib`
- `--diagnostic-capture-only`
- `--experiment-monitor`
- `--headless-replay`
- `--journal`
- `--no-progress-seconds`
- `--output-dir`
- `--profile`
- `--protected-monitor`
- `--remote-metadata`
- `--remote-provider`
- `--replay-evidence-profile`
- `--replay-frames`
- `--replay-input`
- `--replay-max-mib`
- `--replay-output`
- `--replay-sample-fps`
- `--report`
- `--roi`
- `--run-id`
- `--single-monitor-capture`
- `--stage-diagnostics`
- `--timeout`


## GUI 与自动诊断 / GUI and automatic diagnostics

2026-10-21：Encoder 主/Shell GUI 新增默认关闭的 PAM4 预期接收速度先验，`EncoderConfig::expectedReceptionRate`；整数区间单位为 1024 B/s，首版仅 PAM4 且超过 12 段生效，不新增 CLI 开关。其它模式不应用，core 的非法组合明确拒绝。参数、模型时钟、确定性修复及 `senderRatePrior` 日志见 [中文](SENDER_RATE_PRIOR.md) / [English](SENDER_RATE_PRIOR.en.md)。本地实现/回归不等于真实链路提速验收。

四个面向用户的模式：标准 / 灰阶高速 / PAM4 / PAM4 Wide。内部 CLI token `experimental-pam4` 与 `experimental-pam4-wide` 保留兼容，不显示为产品模式标签。不同 Profile 不在同一 Session 中切换。

Product-facing mode labels have no experiment badges. Frozen CLI names retain compatibility. The GUI defaults to PAM4 (index 2) for fresh, missing, or invalid preferences; valid saved indices remain unchanged. Standard remains compatibility index 0, while CLI/core defaults stay separate; parameters do not weaken finite budgets or final verification.

- Encoder: source, persistent cache, mode, 1–60 Hz, non-negative `Maximum run duration (seconds)` (`0` = no automatic stop), start/stop; no peer-completion inference. A normal GUI start idempotently registers the per-user `PixelBridgeEncoder` static shell verb for `*` and `Directory`; the verb opens a small rate/mode/timeout dialog, and folder inputs are archived as ZIP ordinary-file sources before the existing runtime starts. This is GUI/Runtime-local policy; the existing CLI `--seconds 1..7200` contract is unchanged.
- Decoder: output, mode, target monitor/whole screen/ROI, refresh 100–2000 ms, finite memory budgets, normal stop/resume, verified output opening.
- Automatic logs: no enabling flag needed for normal application runs. Explicit `--journal`/`--report` remain independent create-only evidence. [Diagnostics](DIAGNOSTICS.en.md)
- `--single-monitor-capture` is distinct from legacy protected/experiment dual-monitor safety authority; no forged second display.
- Current PAM4 GUI chooses 7 MiB segments; CLI defaults/overrides must be read separately. Wide's physical 2560×1440 presentation is not optional.

## 可选文件反馈（2026-10-21 GUI/应用配置增量）

Encoder 主/Shell 和 Decoder 高级选项新增默认关闭的 FeedbackOptions，应用层为 optional FeedbackConfig（Directory/File + 绝对/UNC 路径）；未新增自动启用或通用文件执行命令。PBFeedbackIo 只支持带版本、nonce/run token 的元数据读/写启动和 --build-identity。后台约 1 秒轮询、5 秒 I/O watchdog、15 秒合格观察 TTL；不在 UI 配置阶段探测网络。精确行为见[文件反馈合同](FEEDBACK_ASSISTED_TRANSFER.md)，不能把 helper 当另一条 payload 通道。
