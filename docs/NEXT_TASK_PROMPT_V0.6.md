# 下一次任务可直接粘贴的Prompt

请继续<repo>项目的非本机纯视觉单向传输效率优化。请先读取AGENTS.md、docs/README.md、docs/PROJECT_STATUS.md、docs/SESSION_HANDOFF_20260914_V0.6.md、docs/RELEASE_V0.6.md、docs/REMOTE_NONLOCAL_THROUGHPUT_SESSION_FINDINGS_20260911.md的§21.33–21.37，以及artifacts/release-v0.6-20260914内的DELIVERY_VERIFICATION.json、FINAL_HANDOFF_RECEIPT.md和发布manifest。核对live git status/HEAD、最新实际源码、二进制哈希、旧diff与保护文件，再决定下一步；不要只信本prompt或旧记忆。

最终目标：让远端桌面TestMediumFile.bin（273806498B）与TestBigFile.bin（1059917774B）的完整传输总耗时尽可能短。最终成功必须经过全文件摘要、安全发布、重开验证及独立SHA。之前954MB传至75.9MB停止增长的问题不能只靠UI活动或更多名额宣称解决。

最近已交付v0.6（应用版本0.6.0），上一任务已按我的要求停止新研究，并获准只提交本任务文件，不推送。当前发布与实传证据范围必须从receipt确认，不把旧0.5候选的成绩改名成0.6二进制实测。

关键结论：
1. 完整全屏是确定需求。1080p、2K、4K都应完整内容铺满，不回到中间小码面+外围灰边；16:10允许横纵独立缩放，不裁切。当前有界实现/验证范围详见交接，不能无证据外推任意分辨率。
2. P2修复真实Capture入口漏用已验证Bootstrap的灰阶尺度准入，已正常源码整合CPU/GPU灰阶前向采样。SC6正式尺度、安全校验不放宽。
3. Q全屏使实际UniqueVisualFPS约13.5→7.5。25MB同包15/30FPS都约7.5，不能继续盲扫FPS。25MB仅4段，不触发>12段首轮策略。
4. R在相同Q Encoder/P2 Decoder、全屏、30FPS、6MiB段、spatial、预算约束名额下，取消实验首轮65%开关、保持首轮100%，100MB由782615ms降到525968ms（快32.79%）。完成后重复块37277→4784、活动峰16→4，摘要/发布/重开及两次SHA全过，资源拒绝0。有效帧率仍约7.51，所以主要是减少修复/重复等待，还未突破视觉刷新上限。
5. O旧居中候选的完整Medium+Big最佳历史合计4312469ms=1小时11分52.469秒；Q/R/v0.6没有同身份的完整双文件成绩，不可混用。M late-join12505未达12000等失败边界必须保留。
6. 原resume保留在C:\Users/<user>\Desktop\PixelBridge-5e4e83e66150e401.resume。75.9MiB对应48883×1629已接受方程字节，0完成段、8未完成活动段；没有原现场runtime日志，不能说已唯一证明根因。

工作约束：
- 不要创建任何子智能体，由你亲自完成。
- 纯视觉单向payload，不增加网络/文件/IPC旁路或隐式ACK。远程桥仅编排、部署、停止、收集日志和元数据，不能向Decoder送payload或图片。
- 不修改Citrix、网络或显示模式；不削弱摘要、安全发布、重开、冲突拒绝、源稳定性、恢复校验或资源限制。
- 不干扰左屏、焦点、鼠标、键盘。当前右屏显示远控；本机DISPLAY2 ROI=[2560,0,5120,1440]，远端原2560×1600@240，必须先live核对。桥为<ShareRoot>、SENDER-LAPTOP；先读桥文档，不做全局停止或输入自动化。
- 正式默认解码器名额保持8；显式--budget-bound-decoders才启用原1GiB等预算约束的有限动态名额。不是无限名额/无限内存。
- 不覆盖/删除.zcode/、docs/PHASE1_GATE_REPORT.md、docs/V3_GRAY_STAGE_C_HANDOFF.md以及旧证据；Git用显式路径、原子非amend提交，不自行push/reset/rebase。
- 有高影响授权/需求不确定时及时向我提问并等待答复；可逆技术细节基于代码/测试决定。及时告知进展、困难、实验及下一步。使用中文，称呼我主人。
- 频繁测试≤100MB。先本地窄验证，明确新证据后才实屏；不要测试与构建/重负载同时跑。不要马上重跑整份Big。

建议起点：先确认v0.6确切发布来源、当前运行状态与R复跑边界。随后研究保持完整全屏的更友好呈现采样：只先做test-only point与线性/面积参考对照，覆盖canonical→远端全屏→远控缩放的两阶段链、分数相位、满载随机transport块、CPU及真实Capture/GPU逐字节验证。没有通过前不改正式过滤器、不放宽阈值；通过后显式候选封存，先≤25MB非本机实传，与R首轮100%对照。该方向只是待验证假设，不把官方文档的通用机制当本会话唯一根因。

每个重要结果记录具体commit/exe hash、源hash、参数、Session、clean/resume状态、最终摘要/发布/重开、同主机计时、资源/冲突和停止结果。持续面向最终两个大文件的耗时改善，不把兼容性、理论容量或一次100MB成功当作总目标完成。
