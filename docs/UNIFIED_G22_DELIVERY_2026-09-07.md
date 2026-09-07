# G22 最终 GUI 本地候选交付 — 2026-09-07

后续 Citrix Encoder 显示兼容修复有独立的[候选与验证记录](UNIFIED_G22_ENCODER_DISPLAY_COMPAT_2026-09-07.md)；
不覆盖本文原双端冻结身份与原实屏证据。

## 1. 结果与冻结身份

**G22 = PASS_LOCAL_CANDIDATE**：用户要求的双端GUI已重新实现，独立Windows x64便携包、新解压双端GUI检查、
最小实际像素恢复、最终独立双摘要及包完整性复验全部通过；本轮目录整理已完成。
这是本地可用候选的完成状态，不代表公开发行、维护者选择项目LICENSE或新版大文件/远控性能认证。

- 权威构建：`3a840a202cf7d342a3ff2d95f2d69b3c788abcea`。
- Profile：`PB-Unified-SC6-V3` / `0x5042554E49534333` / layout 10，Protocol 1.0。
- 规范Profile JSON SHA-256：`312c7832854f8710719ee2d0e44e920e7270b48638044eaa4b6af9ee24b1cb8b`。
- 最后整理提交只更新文档和交付索引，不重打包，不将其HEAD当成上述二进制/实屏身份。
- G21继续保持`PASS_WITH_SINGLE_RUN_USER_WAIVER`及原始性能失败事实，本轮不重新解释其豁免或重跑性能矩阵。

## 2. 实际交付入口

总目录：`<repo>\artifacts\g22-delivery-2026-09-07`，先读`START_HERE.md`。

| 内容 | 相对总目录的路径 |
| --- | --- |
| Encoder | `final-package/PB-Unified-B-3a840a20-6a2a1131/Encoder/PixelBridgeEncoder.exe` |
| Decoder | `final-package/PB-Unified-B-3a840a20-6a2a1131/Decoder/PixelBridgeDecoder.exe` |
| 完整可复制ZIP | `final-package/PB-Unified-B-3a840a20-6a2a1131.zip` |
| 外部seal | `final-package/PB-Unified-B-3a840a20-6a2a1131.seal.json` |
| 用户操作指南 | `final-package/PB-Unified-B-3a840a20-6a2a1131/USER_GUIDE.md` |
| 独立验证器 | `final-package/PB-Unified-B-3a840a20-6a2a1131/Test-PBUnifiedPortablePackage.ps1` |
| 总checkpoint与证据哈希 | `release-checkpoint.json`、`SHA256SUMS.txt`、`evidence-index.json` |

ZIP为 **56,943,595 bytes（54.31 MB，1024进位）**；100个payload文件共132,309,001 bytes。
两个应用各有自己的Qt plugins/DLL目录，另含VC runtime、SPDX SBOM、Qt/vcpkg/MSVC notices、用户说明和编译Profile。
不要单独复制一个EXE并删掉其依赖。应用本身不要求安装PowerShell；独立verifier要求PowerShell7。

| 身份项 | SHA-256 |
| --- | --- |
| ZIP | `8cf089ed624bb9bb44c085493d32d6d99f1ce599729561ba569824db2c7269b3` |
| package-manifest.json | `3f35b8893a497f2a08bdeca78f30a782ca9e6d6dc63049dd4c84e5acba8751ef` |
| Encoder.exe | `2c8f797df8397016a733124e642eb4bc72a28430110908f14546f50cde5c6e34` |
| Decoder.exe | `a011b8b6759a9fd446e1d42265ee77dfd2218ceba314b2e41d1f733c65e3043d` |

Qt 6.10.1.0；随包VC runtime文件版本14.44.35211.0，来自明确记录的VS x64 redist目录。
MSVC工具链/SDK/CMake/vcpkg baseline与ABI等完整字段见manifest；第三方库基线不冒充wire版本。

## 3. 用户要求验收

- [x] 丢弃旧界面，两端均为简洁主体 + 独立高级选项Tab。
- [x] Encoder单文件选择，0..500 GB资源合同（1024进位），1..60 Hz默认15；准备/发送/停止期间锁定帧率和输入。
- [x] 点击开始后当前主窗口所在屏幕全屏，1920×1080规范画布1:1居中加matte；无限Carousel，不读取Receiver完成消息。
- [x] Encoder本窗口焦点Esc停止，不注册全局钩子；局部shortcut信号和停止/解锁路径通过测试，未注入OS按键。
- [x] Decoder目录、明确显示器与整屏/框选ROI、开始/停止；手填物理坐标在高级页，跨屏/取消/拓扑变化模型有定向测试。
- [x] 百分比、KB/s、ETA纯文本；仅已验证raw bytes，最终digest/publish/reopen前不能100%。
- [x] 接收完整自动停止、页面保留100%/完成，无完成弹窗和自动开目录；停止保留resume。
- [x] 所有支持文件大小无需确认；500 GB cap、磁盘空间、路径、冲突、不覆盖和摘要检查不削弱。
- [x] 双击只打开GUI；原CLI标准输出/错误/退出码与独立文件、管道、NUL重定向保留。
- [x] PowerShell包装脚本显式等待GUI子系统进程，避免提前释放源lease；历史实验命令仍保留。

## 4. 验证证据及边界

| 检查 | 实际结果 | 证据 |
| --- | --- | --- |
| 受影响双端定向Release构建 | exit0；重新configure后嵌入提交与候选一致 | `artifacts/g22-package-20260907/build-final-candidate.log` |
| GUI启动/CLI/重定向/非法native参数 | 18/18 PASS | `artifacts/g22-native-20260907/startup-01/summary.json` |
| GUI/Windows单位/选区 | 两端offscreen、QSettings/进度和selector定向检查通过 | `artifacts/g22-decoder-20260907/tests-no-size-confirmation.log`（4/4） |
| runtime边界/停止恢复/资源策略 | 6 cases、305 assertions PASS | `artifacts/g22-decoder-20260907/runtime-sc6-fixture.log` |
| 旧PS5.1包装脚本GUI-subsystem夹具 | 19/19 PASS，不使用产品实屏 | `artifacts/g22-cli-wait-20260907/summary.json` |
| 包verifier函数 | 28/28 PASS | `artifacts/g22-package-20260907/verifier-functions-01/summary.json` |
| 完整包结构与恶意变体 | 20/20 PASS，修复ZIP Unix-mode有符号比较问题 | `artifacts/g22-package-20260907/negative-03/summary.json` |
| 最终包新解压启动/metadata/GUI smoke/前后verifier | 10/10 PASS | `final-clean-check/summary.json` |
| 最终包右屏实际像素收发 | 双端exit0，完整恢复、保护通过、无强杀 | `final-native-check/summary.json` |
| 原生运行后的sealed包复验 | PASS；文件/ZIP/seal未变，verifier不运行EXE | `final-post-native-verification.json` |

最终实屏只使用`\\.\DISPLAY2`（`[2560,0,5120,1440]`），保护`\\.\DISPLAY1`。
实际WGC到达332帧；UI显示100%、268.3 KB/s和00:00:00。这个KB/s是该小文件的GUI恢复估计，不是新的吞吐认证。
Encoder默认15 Hz；Receiver完成后仍独立继续广播 **40,976 ms**，直到45秒诊断期限。
正常GUI没有该测试期限。双端`ownProcessBecameForeground`无失败记录，`safetyHeld=true`，没有鼠标或键盘自动化。

最终输出恰好1,048,576 bytes：

- SHA-256：`27545ef2457927d885356f474e4360c9c1bbc5913a071294b3cce7b3e4b76044`。
- BLAKE3：`905b6379385ee5cb953650b52e93e2391322ef54292bf81a02f66c8ba9268b2a`。
- SHA-256/BLAKE3独立读取最终路径，与CSPRNG源一致；WholeFileDigest、rename、safe publish、final reopen全部为true。
- Decoder命令只得到输出目录、显示器ROI与本地测试日志目录，不获得源文件、摘要或Sender cache。

早期`native-check/`曾在Encoder保护检查上中断，接收文件仍正确。该失败原样保存；只有最终`final-*`是交付权威。
后续只增补了诊断原因，未降低保护条件；没有把未复现当成已证明原因或生产修复。

## 5. 重放与独立校验

在新目录解压ZIP，保持包目录名。以下校验只读、不运行包内程序：

```powershell
pwsh -NoProfile -File <package>\Test-PBUnifiedPortablePackage.ps1 `
  -PackageDirectory <package> -PackageSealPath <seal.json> -ArchivePath <zip> `
  -ExpectedManifestSha256 3f35b8893a497f2a08bdeca78f30a782ca9e6d6dc63049dd4c84e5acba8751ef
```

使用仓库工具从新解压目录重放最小检查（create-only输出，不能复用本轮目录）：

```powershell
& <python> -X utf8 tests/tools/verify_unified_package_startup.py `
  --package-result artifacts/g22-delivery-2026-09-07/final-package-result.json `
  --evidence-directory artifacts/g22-new-clean-check --powershell (Get-Command pwsh).Source

# 仅在明确允许使用右屏时执行；不发送鼠标键盘事件。
& <python> -X utf8 tests/PBApplication/run_gui_native_smoke.py `
  --encoder <new-package>/Encoder/PixelBridgeEncoder.exe --decoder <new-package>/Decoder/PixelBridgeDecoder.exe `
  --experiment-monitor '\\.\DISPLAY2' --protected-monitor '\\.\DISPLAY1' --evidence-directory artifacts/g22-new-native-check
```

## 6. 目录整理与最后提交

按用户“如果它们没有用了就删除”的授权，审计根目录92个`build-*`目录：先核对CMake来源、文档/脚本引用、
文件种类、reparse及当前进程占用，再删除 **35个**仅含可生成产物的旧缓存，共 **20,085 files / 4,315,567,815 bytes（4.02 GB）**。
先将 **954份**配置/日志等文本放入ZIP并逐项读回比对SHA-256；删除采用本机PowerShell `Remove-Item -LiteralPath`，
最终再次确认每个绝对目标是本工作区的直接`build-*`子目录。不使用跨shell删除或广域清理。

清理证据：`artifacts/g22-directory-cleanup-20260907/` 下的scope/reviewed inventory、preserved-text-manifest、
`retired-build-text-provenance.zip`、`cleanup-result.json`与精确脚本。保留当前Unified构建、被引用的历史目录、
源码/索引快照、原始Golden/实屏/故障证据；中间G22失败及通过证据路径全部不变，只通过交付索引区分用途。
旧`display_probe.obj`已有历史记录，来源用途不能可靠判定，未擅自删除。

原有未跟踪`docs/PHASE1_GATE_REPORT.md`从未改动/暂存；SHA-256一直为
`076ef4c9b9f89eabccd323dbe4bffc4dc125ddaf96e6ee437d2cf5b1b1cea306`。
最终整理采用显式路径独立提交，不提交payload、运行状态、二进制、ZIP或旧report，不amend/rewrite/push。

## 7. 明确保留的未执行事项

- 后续用户报告 Citrix Receiver 远程会话内 Encoder“无法确认当前屏幕”；返回截图已确认Win32有屏幕、DXGI有硬件adapter但无output，导致catalog返回1168。现有包尚未修复，不宣称Citrix兼容；证据与待确认修复范围见[Citrix显示诊断](UNIFIED_G22_CITRIX_DISPLAY_DIAGNOSTICS_2026-09-07.md)，不是新远控PASS。
- 未人工双击EXE、按真实键盘Esc或拖动真实ROI；no-console由PE/CRT/进程探针证明，GUI和本地shortcut路径由真实控件测试证明。
- 未在无开发环境的纯净Windows VM认证；已在新解压目录限制子进程PATH为Windows/System32，清除Qt/plugin覆盖后通过。
- 未新跑4/500 GB实际文件、G21大容量阶梯、新版远控性能、全量CTest或ASan；500 GB相关是策略边界测试。
- 项目自身LICENSE、数字签名、公开分发未决定；SBOM保留NOASSERTION。无签名哈希/seal不是发布者身份认证。
