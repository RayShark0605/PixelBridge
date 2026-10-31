# PixelBridge v1.1.0 — 文件反馈辅助传输 / File-feedback assistance

日期 / Date: 2026-10-31. Windows x64；Encoder / Decoder 独立分包。应用版本更新不改变既有视觉协议、Profile、Golden 或恢复格式。The application version changes, not the existing visual protocol, profiles, Golden bytes or recovery format.

## 新功能 / What's new

- **可选文件反馈，默认关闭。** Decoder 在本端专用目录/文件发布有界身份、耐久段位图、视觉观察和完成状态；Encoder 经对应映射只读获取。正向文件内容依然只从 Decoder 实际捕获的像素恢复。**Optional, default-off feedback:** only bounded receiver metadata uses the return mapping; all forward payload remains pixel-derived.
- 标准、灰阶高速、PAM4、PAM4 Wide 均可启用；Encoder 主/Shell 共用设置，Decoder 在高级选项配置。有效时优先服务未完成段，过期/断线/拒绝时回到原视觉轮播。Supported in all four modes, with bounded scheduling and safe no-feedback fallback.
- Encoder 仅在完整身份、当前 Start 实际展示帧和接收方最终验收都匹配后安全自动停止；所有段已确认但没有最终凭据时不停止。Automatic stopping requires a complete current-Start final receipt, not an estimate or segment count.
- Decoder 完成后保持“已完成，反馈监听中”，文件耗时冻结。监听期间完成文件可读取查看；编辑、移动或删除前先停止监听。Completed listening keeps the verified output read/view-only until Stop releases protection.
- 有界 `PBFeedbackIo.exe` 隔离可能阻塞的映射 I/O；两个包均包含同目录 helper，不需另行安装。Bounded sibling helpers isolate blocking mapping I/O without carrying file payload.
- 保留可选 PAM4 接收速度先验和显式 Qt 软件呈现选择；新增配置不改变默认关闭语义。包含配套的资源、解析、恢复、呈现寿命和测试夹具修复。Existing rate-prior and explicit Qt presentation choices remain optional; reliability fixes and regressions are included.

## 最短使用 / Quick start

1. 完整解压两个独立 ZIP，不只复制 EXE；保留 helper、运行库、插件、许可证和 `sources/`。Extract each complete ZIP, not the EXE alone.
2. 先启动 B 的 Decoder，在高级选项启用辅助反馈，选择一个与输出/恢复状态物理分离的专用反馈子目录。Start Decoder first; select a dedicated feedback subfolder separate from output/recovery state.
3. A 的 Encoder 启用只读反馈，选择 B 位置对应的目录/文件映射，可直接粘贴 UNC；然后按原流程传输，双端模式一致。Select the corresponding read-only mapping on Encoder and match visual modes.
4. B 获得视觉身份并排除输出路径重叠后开始写反馈。A 可以等待状态出现，不必先等反馈“有效”再发送。After visual initialization B starts publishing; A can transmit while waiting for feedback.
5. B 完成后保留监听，等待 A 接受完成凭据并停止，然后停止 B 监听。未启用或辅助失败时，仍可在 B 验收成功后人工停止 A。Keep B listening until A stops; without feedback stop A manually after verified completion.

详见[中文反馈指南](FEEDBACK_ASSISTED_TRANSFER.md) / [English feedback guide](FEEDBACK_ASSISTED_TRANSFER.en.md)、[Encoder](ENCODER_GUIDE.md)、[Decoder](DECODER_GUIDE.md)。

## 验证与边界 / Validation and limits

- **维护者于 2026-10-31 报告反馈候选实机亲测通过，并批准发布。** 未提供各模式/文件尺寸/映射延迟矩阵或测速数据，因此只登记功能通过反馈，不推导量化提速、全平台、长稳或全部提供商认证。**Maintainer-reported real-use functional success** is recorded separately from automated verification; no quantitative performance or universal qualification is inferred.
- 发布从准确提交树重新构建；`package-manifest.json` 记录 commit/tree、源码清单、应用/helper/依赖及文件哈希，外部 `.seal.json` 绑定 ZIP 与 manifest。机器路径以通用占位符代替；原始日志、用户载荷、恢复状态、个人配置和 PDB 不分发。Release artifacts bind an exact committed tree, not the previous dirty candidate.
- 既有 Gray Fast spatial late-join 性能门 `12505 < 12000` 仍为已知限制，本次不扩大到该性能优化；原测试和阈值保留，不声称全套全绿。缺真实 Replay 夹具的跳过也单列。The known Gray Fast performance gate is not relabeled as passing or weakened.
- 单指定 Decoder、唯一 Sender、可信反馈位置和未克隆/未回滚的 Session cache 是信任前提。CRC/目录锁不是认证；恶意共享写者不在此功能保证内。Trust is single-receiver/single-sender, not sender authentication.
- no-replace 发布允许短暂路径缺席并安全回退；不是无间隙 atomic replacement 或任意目录别名/祖先竞争的完整证明。残留 `.previous` / `.tmp` / 锁不自动删除，先停止双方并确认归属再处理。Transient absence is safe; unknown leftovers are never adopted/deleted automatically.
- 500 GiB 字节上限仍同时受 65,536 段与资源预算限制；PAM4/Wide 当前 GUI 默认组合上限 448 GiB。边界不是相应大小已实测声明。File-size admission limits are not tested-capacity claims.

## 许可与分发 / Licensing and publication

独立文件名为 `PixelBridge-v1.1.0-Encoder-win64.zip`、`PixelBridge-v1.1.0-Decoder-win64.zip`；保留对应 `.seal.json` 和 SHA-256 清单。本地生成及 Git push 不等于已经创建 GitHub Release；发布条目和附件上传由维护者完成。

PixelBridge 使用 MIT；Qt 和其它依赖保留自身条款，见 `LICENSE`、`THIRD_PARTY_NOTICES.md`、`licenses/`、`SBOM.spdx.json`、`QT_SOURCE.md` 和 Qt 对应源码。包未代码签名，哈希保证完整性，不是发布者认证。
