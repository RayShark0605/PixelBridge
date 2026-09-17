# 非本机周期控制相位单变量候选

> **维护范围 / Scope (2026-09-17):** 本文保留该模块的协议/工具/测试参考，不再作为项目当前路线或发布状态入口。当前模式、单屏接收、预算、日志与完整文件证据见 [中文文档](../../docs/README.md) / [English documentation](../../docs/README.en.md)。历史日期、Gate、现场坐标与阶段参数仅适用于当时记录；不应直接复制到新环境。
> This is a module/tool/test reference, not the current release roadmap. Use the bilingual index for current behavior and validation boundaries; historical gates/settings are not universal defaults.

本工具仅做已冻结 A/B 应用库的窄验证，不启动 GUI、屏幕捕获、网络或 codec；不得作为非本机吞吐证明。

候选在当前活跃 Segment 窗口内分配 `phaseIndex / phaseCount`，只将首次周期控制 deadline 延后 `floor(10s * phaseIndex / phaseCount)`。启动的四份控制不变，以后仍从实际控制 burst 起点间隔 10 秒，每次一个 Session/Manifest/Segment triplet，普通数据帧仍携带当前 SegmentDescriptor。不追补错过的周期，不改变 slot/wire/FEC/Receiver/资源和发布门。单段和零字节的零相位保持旧行为。

这保持每段的周期重复预算与上限，而不是承诺任意有限尾窗口内的实际总控制数相等。停顿会使不同相位再次靠近；本轮不增加重锚定或动态反馈以追逐更漂亮的模拟结果。

## 本地最小检查

- `PBUnifiedSenderSchedulerTests`：既有十项测试及四项相位测试。五段、15 Hz、40 秒合成 schedule 对照；验证完整窗口控制 slots/方程总量，拒绝非法相位、溢出保留 pending、重试时间冻结、停顿不补发。模拟时间不是实际吞吐。
- `PBControlPhaseStriping`：调用已有生产 SenderFrameBuilder 的 8×64 KiB 有界 temporal-striping probe，核对首轮方程和持久 repair lease，之后仅观察每段第一条后续 pass 方程。此小样例不覆盖 10 秒周期，不宣称完整文件验证。
- `PBControlPhaseRawReplay --off <原始 source.bgra> <新的输出根>`：原有 30 帧像素入口链接当前 A 或 B 应用库，接回原 WARP demod/Receiver，整文件摘要、发布、reopen 后独立双摘要比对。A/B 各 30 帧，总 60；不重跑 codec，不扩展像素矩阵。这也不是新多段节奏的现场验证。

每个工具进程/job 最多 2 GiB、300 秒；原始像素每次只保留一帧，原始 BGRA 文件来自历史封存且只读，不复制或覆盖。构建独立目录，引用明确的 A/B 应用构建与已安装依赖；应用库身份和实际链接输入必须留在本轮证据。

## 现场边界

首轮只做同一 40,517,389 B 固定文件、15 Hz、无录屏的 A/B 迟加入对照。两端均用对应同一封存包，人工运行；Decoder 只捕获右屏像素。非本机软件仅作 operator metadata，不进入解码分支。

须先核对两端拓扑/实际画面，保持远控与网络设置不变；不自动移动窗口或注入鼠标键盘。主计时保持 `finalReopenVerified - startAccepted`，首次有效 Bootstrap 必须精确关联 Sender `events.jsonl` 且 `carouselPass > 0`。每个测量 run 操作预算 600 秒；超时人工停止并保留不完整证据。工具原有 1,800 秒证据上限并不替代此次人工预算，也不自动停止发送。

只有两端封存、源文件前后审计、整文件独立摘要、发布/reopen 和完整计时均合格才纳入对照。一组 A/B 不证明稳定提速，不将更早控制就绪替代完成时间；未获益则不晋级。
