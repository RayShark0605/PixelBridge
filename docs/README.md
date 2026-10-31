# PixelBridge 文档

> 当前版本 / Current release: [v1.1.0](RELEASE_V1.1.md)。反馈候选由维护者报告实机通过；以新提交、正式包身份及其验包记录为准。


> 新增：[可选文件反馈设置、控制面合同和证据边界](FEEDBACK_ASSISTED_TRANSFER.md) / [English](FEEDBACK_ASSISTED_TRANSFER.en.md)。Decoder 在高级选项设置；Encoder 主/Shell 共用只读设置，四模式均支持，默认关闭。

**简体中文** | [English](README.en.md)

## 面向使用者

1. [项目介绍](../README.md)：使用场景与最短入门。
2. [使用指南](UNIFIED_USER_GUIDE.md)：先接收后发送、整屏/单屏、模式、停止与恢复。
3. [内存预算](DECODER_MEMORY_BUDGET.md)：总/单实例设置、建议值及边界。
4. [日志与排查](DIAGNOSTICS.md)：等待原因、双端计数、异常与日志文件。
5. [支持与项目现状](PROJECT_STATUS.md)：已验证和未验证范围；v1.1 发布边界。
6. [2026-09-22 历史审计记录](AUDIT_2026-09-22.md)：当时的代码/文档审阅、修复、验证结果和保留限制；不是后续工作树的重新验收。
7. [PAM4 预期接收速度先验](SENDER_RATE_PRIOR.md)：可选配置、预算规则、回退和本地/现场证据边界。
8. [先验候选双端便携包](PAM4_RATE_PRIOR_CANDIDATE_PACKAGES.md)：独立 ZIP、隔离发送入口、依赖与完整性检查。

## 面向开发者

- [技术路线与总体设计](PixelBridge_最终技术路线与总体设计.md)：当前架构、PAM4/Wide、FEC/Carousel、恢复、安全与捕获生命周期。其英文版是完整配套文档，不是摘要替代。
- [PAM4 模式合同](EXPERIMENTAL_PAM4_PROFILE.md) / [Wide 模式合同](EXPERIMENTAL_PAM4_WIDE_PROFILE.md)：精确数值、只读映射、校准/FEC、Golden 与兼容身份。
- [当前选项清单](CURRENT_RUNTIME_OPTION_INVENTORY.md)：从当前 CLI adapter 核对的选项名。
- [构建与贡献](../CONTRIBUTING.md)、[AGENTS](../AGENTS.md)：工具链、检查与工程不变量。
- [PBBridge](REMOTE_OPS_BRIDGE.md)：仅开发编排，不是 payload/ACK 旁路。

## 深入参考与历史

以下模块规范/逐字段表保留，不再按历史阶段宣称“当前产品尚未实现”；当前行为优先看上方设计和对应源代码/测试。

| 参考 | 内容 |
| --- | --- |
| [Descriptor Schema](PROTOCOL_1_DESCRIPTOR_SCHEMA.md) | 协议字节、长度、reserved、CRC 与冲突 |
| [Reference raster](REFERENCE_RASTER.md) | 参考光栅与逐像素合同 |
| [Sender persistence](ENCODER_STREAMING_CAROUSEL.md) | 发送状态/lease 与历史调度证据 |
| [预期接收速度先验评估](SENDER_RATE_PRIOR_RESEARCH.md) | 2026-10-21 窄调度研究；收益条件、反例与验证建议，未实现或实测晋级 |
| [延迟只读反馈辅助评估](FEEDBACK_ASSISTED_TRANSFER_RESEARCH.md) | 单个指定 Decoder 的可选分段反馈、流水调度与验证完成后安全自动停止；仅研究，未实现/实测 |
| [Receiver persistence](DECODER_RESUMABLE_RECOVERY.md) | PBJH/PBJR、checkpoint、compact、发布崩溃窗口 |
| [Presentation](PRESENTATION.md) | 数据窗口、采样与 Present 生命周期 |
| [Screen region](SCREEN_REGION.md) / [WGC](PBScreenCaptureWgc.md) | 物理坐标、DPI、纹理/租约 |
| [Telemetry](UNIFIED_TELEMETRY_REPORT.md) | RunReport 与 verified goodput 契约 |
| [Golden harness](GOLDEN_VECTOR_HARNESS.md) | 确定性回归与解释边界 |
| [Evidence](EVIDENCE_INDEX.md) | 当前可引用结果及未抹去的失败/waiver |
| [Document history](DOC_HISTORY.md) | 删除记录、原文 SHA/备份与旧 Git blob 取回 |
| [Tools](../tools/README.md) / [Fuzz](../fuzz/README.md) | 局部研究工具及测试语料，不是默认产品路线 |

保留的局部工具/历史文档不全部逐句重译；**当前用户指南、架构、模式、预算、诊断、现状均有配套英文版**，机器字段表与 Golden 共用一份，不复制出相互分歧的 wire 定义。维护时先改两种语言的当前文档，再核对共享表/测试。

## 维护规则

- README 服务用户，不堆阶段日志；当前结论、证据索引与历史恢复分开。
- 产品模式标签统一“标准 / 灰阶高速 / PAM4 / PAM4 Wide”；内部 `experimental-*` token 为兼容保留。
- 单次速度结果必须带源码/包/会话身份与环境，不把旧包成绩移给新源码。
- 不把原始文件、日志、屏幕录像、私有地址/凭据或 build tree 加入 Git。
- 受保护本地文档 `PHASE1_GATE_REPORT.md`、`V3_GRAY_STAGE_C_HANDOFF.md` 不修改/删除/纳管；`AGENTS.md` 与 `.zcode` 不参与本轮清理。
- v1.0 采用 MIT 许可证，发布流程、拆包内容及验证边界见 [发布说明](RELEASE_V1.0.md)。


根目录 README 为常青产品文档：保持当前状态、直接更新正文，禁止修订记录/日期更新段；同步中英文。历史和版本变更只放专门文档。Root READMEs must stay evergreen and bilingual; keep change history outside them.
