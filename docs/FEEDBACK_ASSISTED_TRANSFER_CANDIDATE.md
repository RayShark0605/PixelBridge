# 文件反馈辅助传输候选交付 / Feedback-assisted candidate delivery

> 历史候选记录 / Historical candidate record：2026-10-31 已转入 [v1.1.0 正式提交发布流程](RELEASE_V1.1.md)，不要把旧候选包名、dirty 身份或旧“未实测”状态当作当前版本。


日期：2026-10-21。项目阶段仍为 v1.0 可靠性、显示兼容性与交付维护；本轮是默认关闭的可选控制面，不是视觉 wire 升级、正式全绿发布或旧静态审计结案。

## 权威交付记录

本地维护证据入口：`artifacts/feedback-assisted-packages-20261021-run01/DELIVERY.md`。只有该记录实际列出的已通过包、ZIP SHA-256 和独立验证结果才可作为交付；文件名/目录存在本身不是通过。完整回归日志、初始失败、修复和字节基线在 `artifacts/feedback-assisted-implementation-20261021-run01/`，独立原审阅及主 Agent 修复分开记录。

预定独立包名：
- `PixelBridge-v1.0.0-Encoder-Feedback-Candidate-20261021-win64.zip`
- `PixelBridge-v1.0.0-Decoder-Feedback-Candidate-20261021-win64.zip`

完整解压到新目录，不覆盖旧包，不只复制 EXE。两包分别含自身的 `PBFeedbackIo.exe`、运行库、插件、许可与 Qt 对应源码。源码身份沿用 `PixelBridge.DiagnosticSourceInventory.1` 与标准 SHA-256 清单；未提交候选不能仅由 base commit 标识。Encoder 内嵌指纹与清单一致；Decoder/helper 以当前构建、原样身份和精确 EXE 哈希绑定，不伪造不存在的身份字段。

## 使用与停止

见[中文指南](FEEDBACK_ASSISTED_TRANSFER.md) / [English guide](FEEDBACK_ASSISTED_TRANSFER.en.md)。先 B 后 A，双端选同模式与对应反馈目录/专用文件。A 可粘贴 UNC，只读；B 写状态元数据。未启用时保持原行为。完成监听期间文件可读取查看，编辑/移动/删除前停止监听。反馈不可用会恢复视觉轮播；残留 .previous/.tmp 必须确认归属后处理，不自动删除陌生文件。

Encoder 候选启动器使用既有隔离 display-diagnostic 入口，不修改旧 Shell 注册；直接启动 EXE 则沿用正常设置/注册行为。Decoder 正常入口仍使用本端既有偏好，请核对模式/输出。独立包离屏检查不读取实际源文件、不进行屏幕捕获、注册或现场传输。

## 已知边界

- 历史 GrayFast spatial late-join 门槛 12505 < 12000 的失败仍保留，不放宽阈值；缺真实 Replay 的跳过也准确记录。
- 两项额外基线失败是测试仪器问题：MSVC 大 vector 过分配与假 GPU 完成条件中途翻转；原基线已复现，修复后重复回归，并有寿命错误的反向 mutation 检查。不是删/跳测试换通过。
- headless 真实 FEC/Receiver/存储、持久 ID 租约重启、CPU 像素 oracle、新 Start 停止、离屏 GUI/DPI 各层分列，不相互冒充。
- 真实映射的缓存/断线、客户端重定向提供者、长稳和端到端提速未执行，留给后续实机验证。没有启用真实显示 gates，没有干净 Windows VM 认证。

## English

Use only packages marked verified in the local DELIVERY.md, with matching external ZIP SHA-256 and independent fresh-extraction evidence. These are dirty-source test candidates, not a clean formal release or a field-performance certification. Extract each package in full into a new folder. Keep the sibling helper and dependencies. Disabled feedback preserves baseline behavior. Completed listening permits reading/viewing; Stop releases the protection before editing/moving/deleting the output. The retained GrayFast gate and missing Replay are explicit exceptions, while the two exposed baseline fixture defects have been reproduced and repaired. Real mapped-folder behavior and speedup remain NOT_RUN.
