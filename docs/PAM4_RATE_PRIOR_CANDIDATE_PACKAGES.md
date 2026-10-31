# PAM4 速度先验候选：双端独立便携包

> 历史候选记录 / Historical candidate record：2026-10-31 已转入 [v1.1.0 正式提交发布流程](RELEASE_V1.1.md)，不要把旧候选包名、dirty 身份或旧“未实测”状态当作当前版本。


日期：2026-10-21。项目主阶段仍是 v1.0 稳定性、显示兼容性与交付维护。本页描述用户要求的候选分发，不是新的正式发布或现场性能认证。

## 包与入口

两份 ZIP 各自独立，不能把其中一个包的 EXE 拿到另一个包里混用：

- `PixelBridge-v1.0.0-Encoder-RatePrior-Candidate-20261021-win64.zip`：本次 PAM4 预期接收速度先验 Encoder。
- `PixelBridge-v1.0.0-Decoder-Current-Candidate-20261021-win64.zip`：同一源码截面的当前 Decoder。

新目录为维护者本地 `artifacts/pam4-rate-prior-packages-20261021-run01/final/`。正式 `dist/` 不作为输出目标，旧正式包保留。压缩包的哈希、内容身份、全新解压后的验证和实际执行结果，以该任务目录中的交付记录为准，不能仅凭文件名推定已通过检查。

1. 将两个 ZIP 分别完整解压到两端的新目录；不要覆盖旧程序或会话缓存，不要只复制 EXE。
2. 需要文件的电脑先运行 Decoder，选输出目录、接收区域及 **PAM4**，开始接收。
3. 发送端推荐运行 Encoder 包内 `01-Start-Encoder-Candidate.cmd`。它使用既有 `--gui-display-test` 入口，不注册或重定向旧右键菜单；模式、缓存与日志保存在本候选的 `display-diagnostic/` 下。解压目录须可写。
4. Encoder 选相同的 PAM4，保持原有帧率，例如用户以往的 30 Hz；勾选“预期接收速度先验”，填写 `200～280 KB/s`。超过 12 段时生效，关闭可回退原调度。
5. 以 Decoder 整文件摘要、安全发布与最终重开成功为完成依据，再停止 Encoder；预测 EOF 不会自动知道接收完成。

直接启动 Encoder EXE 或使用右键入口仍是原来的正常入口，会使用用户原有偏好，并按既有逻辑维护每用户 Shell 命令；不要与上述隔离候选入口的配置/日志混淆。Decoder 使用现有正常偏好；无效/缺失模式默认 PAM4，已保存模式不会被候选包强行清除，请手动核对。

## 依赖、许可与校验

每包包含本角色 EXE、匹配的 Qt Core/Gui/Widgets、Windows/offscreen/样式插件、BLAKE3/Zstd、app-local VC 运行库，以及项目和依赖许可、Qt Base 对应源码。无需安装 Qt、Visual Studio、Python 或 PowerShell 7 才能运行 EXE。仍依赖受支持 Windows x64 自带的图形/捕获 API、ICU 和 D3DCompiler；本地受限 PATH 启动不等于任意干净虚拟机/Windows 版本认证。

- `SHA256SUMS.txt` 是标准逐文件 SHA-256 清单；`Verify-Package.ps1` 只读验证，不执行应用。建议解压后、首次启动前验证；候选启动生成的可写状态不是原始分发内容。
- EXE 的真实 `--build-identity`、版本和固定默认 profile 输出随包保存，不改写为虚假的正式源码身份。
- `source-inventory.json` 沿用既有 `PixelBridge.DiagnosticSourceInventory.1` 封存约定；记录 base commit、未提交状态及准确物理文件哈希。Encoder 可使用已有诊断源码指纹构建选项；Decoder 不存在相同内嵌字段时，以实际 build identity、EXE 哈希及封存后构建记录共同追溯，不能把 base commit 当作全部 dirty 源码内容。
- 正式 producer 的 clean-commit 门禁保持不变。本次不提交代码来迁就该门禁，也不把普通候选改称 ProductRelease；不套用正式包 verifier 的通过标签。
- 不含个人日志、恢复文件、用户配置、测试数据、PDB 或未使用的 PDF/SVG/network 插件。Qt 对应源码无需解压即可运行，但重新分发时须与许可/声明一起保留。

## 验证边界

候选打包应从独立新目录检查 ZIP 和文件哈希、x64 GUI PE、依赖闭包、版本/build identity/profile，以及移除开发路径后的离屏启动。验证前后复核原正式包和正常偏好/Shell 状态。不会为打包执行真实传输、采集桌面、修改显示配置或人为制造远控冻结。

上一轮源码验证结果仍分开保留：18/18 定向 CTest 通过；完整 PBApplicationTests 有历史 Gray Fast late-join `12505 < 12000` 失败及缺真实 Replay 数据的跳过。候选包含任务起点已有的 Qt/静态审计修改，未完成此前全量静态审计，不是全仓全绿、签名发布或普遍提速承诺。若比较先验效果，用同一候选的开关对照，并使用新的 Sender 会话缓存和 Decoder 输出目录。

功能细节：[预期接收速度先验](SENDER_RATE_PRIOR.md)。当前状态：[项目现状](PROJECT_STATUS.md)。

## English summary

These are two self-contained Windows x64 **candidate** ZIPs, one per role, in a new output directory; existing formal packages are not replaced. Extract the whole ZIP, not just its EXE. Start Decoder first and select PAM4 at both ends. The Encoder launcher uses the existing isolated `--gui-display-test` entry, without changing the original Shell registration. Enable the optional rate prior (e.g. 200–280 KiB/s) only when wanted; it is effective with more than 12 segments and is not an ACK.

Runtime DLLs, app-local VC runtime, Qt plugins, notices/licenses and matching Qt Base source accompany each role. Windows system APIs remain OS prerequisites. Standard SHA-256 inventories, captured application identities and the existing diagnostic source-inventory format distinguish these dirty-source candidates from formal releases. Do not infer field speed, pristine-VM qualification, publisher authentication or all-green repository status from packaging/startup checks. Actual delivery hashes and results are retained outside Git with the candidate artifacts.
