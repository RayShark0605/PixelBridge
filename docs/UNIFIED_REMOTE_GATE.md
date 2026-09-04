# G21 真实远程像素链验收

## 1. 当前状态与前置

**2026-09-05：PARTIAL / 接收准备完成，尚未执行真实远程恢复。**

- 前置 G20：`e94da7f1d68fd3b410c180ac71201716c11bb9d7`，在途背压修复与本地功能收口已提交。原证据见 `UNIFIED_LOCAL_RELEASE_GATE.md` 第 16 节。
- G20 的 2.0x 是用户豁免、未验证；letterbox 的 15,420.2353 B/unique 不是性能通过。G21 的 16 KiB 硬门仍然有效。
- 用户确认两端由本项目提供：Encoder 由用户放到远程机、启动和摆放；Decoder 由本机控制，只捕获本机右屏。远程连接已经可用、具体类型未知。不自动连接远程、不操作用户输入。
- 用户回报远程 `00_Check.bat`：`PASS: Encoder loads; commit e94da7f1d68fd3b410c180ac71201716c11bb9d7`，`Script exit: 0`；未生成 source、未启动窗口或广播。这是**用户回报的远程加载证据**，不是已观察到的像素链。
- 用户随后明确同意：接收入口仅在专用测试构建启用，内部使用完整生产 `DecoderRuntime` 和 Auto 捕获；外层保护右屏，不扩展产品公共 CLI/Replay/monitor 配置。
- G21 的 1 MiB、64 MiB、chroma/Base Luma、外部摘要与远程性能门均未关闭。G22 未开始。

## 2. 已交付的远程 Encoder 不变

冻结目录：`build-unified-release/g21-handoff-e94da7f/`。

| 对象 | SHA-256 |
| --- | --- |
| `PixelBridge-G21-RemoteEncoder-e94da7f.zip` | `5702141ed013533d5f98b6c3a7b02b63ae37f9dfd3366a5dbe9c1984d1eee71a` |
| `remote-encoder/bin/PixelBridgeEncoder.exe` | `b4e76d5d51b7e8f866d89cec1f31316aafa4ae5fb551e7a570129feb59083cae` |

四个已交付脚本的源副本保存在 `tests/UnifiedRemoteGate/`；本次没有重新生成远程 ZIP 或改写其内容：

- `00_Check.bat`：仅核对 EXE hash、实际加载及内嵌 Git 身份；没有源文件或广播副作用。
- `01_Start_Smoke_1MiB.bat`：远程 OS CSPRNG 生成新的 1 MiB 源文件并广播。
- `02_Start_Full_64MiB.bat`：远程 OS CSPRNG 生成新的 64 MiB 源文件并广播；只在 smoke 审核通过且收到启动指令后执行。
- `Start-G21Encoder.ps1`：上述 batch 的实现，1 MiB 固定生成缓冲、全新 GUID run 目录、create-only 文件、15 Hz Unified、600 秒广播上限和用户手动停止。

这些是**内部测试交付脚本**，不是可从源码目录直接运行的发布包；部署目录还必须含 `expected-build.json`、`bin/PixelBridgeEncoder.exe` 及完整依赖。`expected-build.json` 由交付过程写入该包的准确 commit/EXE SHA-256，不能手工改 hash 绕过校验。Qt/DLL 加载与原交付审计见该冻结目录的 `deployment-audit.json`、`handoff-audit.json`。

每次启动会生成新 source/Session。不能把超时当成功，也不能在未知失败原因下反复新建 source 重试。**不得把 `.bin` 源文件交给 Decoder。** 用户仅在接收结束后提供 `source-manifest.json` 和 `encoder-report.json` 用于独立核验，源文件和其他日志保留在远程机。

## 3. 专用接收入口及边界

`PBUnifiedRemoteGate.exe` 位于 `tests/UnifiedRemoteGate/`，只有 `PB_BUILD_PRESENTATION_GATE=ON` 时构建；不注册到 CTest。两个产品 EXE 的入口和公共接口不变，原 G20 worker 不变。

运行方式：

```text
PBUnifiedRemoteGate.exe --build-identity
PBUnifiedRemoteGate.exe --self-test
PBUnifiedRemoteGate.exe --preflight NEW_ABSOLUTE_RUN_DIRECTORY
PBUnifiedRemoteGate.exe --receive NEW_ABSOLUTE_RUN_DIRECTORY SECONDS
```

- `--self-test`：纯策略、参数和报告边界检查；不枚举真实 monitor、不捕获、不创建窗口或文件、不注入输入。
- `--preflight`：只枚举并验证当前监视器和物理 ROI，写入新目录；不启动 capture。不存在默认 live mode。
- `--receive`：`SECONDS` 必须为 `30..600`；新 run 目录的 `recovered/` 是新输出目录，不继承旧状态。入口不接受 source、源摘要、Session ID、provider、FPS、backend 或宽松阈值参数。
- 固定保护 `\\.\DISPLAY1`，实验目标必须是物理上位于其右侧的 `\\.\DISPLAY2=2560×1440`、identity rotation；ROI 为**整块实验屏的当前物理范围**，不沿用 G20 自有窗口的 `+64,+64` 坐标。
- 启动前解析 ROI 并核对 monitor handle、rect、DPI、rotation；接收期间每 200 ms 重新校验完整 monitor identity/containment。生产 WGC/DXGI 仍保留原有绑定监视器、环境变化与 epoch 规则。任何外层安全失败停止本次验收，不切到其他监视器。
- 不创建 UI、不移动或关闭远程连接等外部 HWND、不改变分辨率/DPI/刷新率/HDR、不注入鼠标键盘。
- 唯一接收配置是 `MakeUnifiedDecoderConfig`，`DecoderRuntime` 使用全部默认 factory。没有额外像素 readback、注入数据、FEC 替身或独立接收架构；保持单 owner、Unified 2 个物理槽/1 个逻辑在途、250 ms、质量/FEC/CRC/摘要/安全发布/重开验证。
- Unified 产品配置有意不接入旧 RemoteVisual monitor/Replay 实验接口。因此生产报告保留其默认配置 metadata；`preflight.json` 单独如实注明实际是用户操作的 opaque remote pixels，authority 为 `NonDecodingOperatorMetadata`。这个旁注不是解码输入，也不伪造生产配置。
- 样本每秒至多一次、每条小于 24 KiB、至多 602 条，`samples.jsonl` 总上限 16 MiB，成功退出前显式 flush/close。所有固定报告 create-only；已有 run 目录拒绝而不是覆盖。
- 宿主启动使用无窗口子进程；本次进程的外层 watchdog 为 `SECONDS + 30` 秒。若生产 Stop 无法完成，宿主只终止自己创建的这个 PID，记录超时失败，不把已发布文件冒充一次干净 Gate。

`final.json` 保留生产 `PixelBridge.RunReport.3`，添加只读 `remoteGate` 计数（age/admission、backlog、Bootstrap、CPU/GPU 时间、accepted Transport、conflict、安全重验次数）。`receiver-checks.json` 独立区分：

1. 当前 clean run 的 `Completed + whole digest + safe publish + final reopen`、无 conflict、无清理警告；
2. 正式 `EvaluatePublishedFrameMetric` 的 `>=16 KiB/unique` 硬门；
3. `>=32 KiB/unique` 工程目标；
4. 尚待外部源 SHA-256/BLAKE3 和 lane 证据审核。

退出 0 仅表示接收侧第 1、2 项成立，**不是 G21 已完成**。缺少外部 sender truth 时，独立 false-accepted-codeword oracle 记为 `null`，不能把默认计数 0 当作已经验证。生产若在发布后报告清理警告，真实文件发布事实保留在生产报告中，但本测试的 clean gate 拒绝。

## 4. 本次最小验证

证据根：`build-unified-release/g21-receiver-1/`。

```powershell
cmake -S . -B build-presentation-release
cmake --build build-presentation-release --config Release --target PBUnifiedRemoteGate --parallel 4
& .\build-presentation-release\tests\UnifiedRemoteGate\Release\PBUnifiedRemoteGate.exe --self-test
& .\build-presentation-release\tests\UnifiedRemoteGate\Release\PBUnifiedRemoteGate.exe --build-identity
& .\build-presentation-release\tests\UnifiedRemoteGate\Release\PBUnifiedRemoteGate.exe --preflight <repo>\build-unified-release\g21-receiver-1\monitor-preflight
```

结果：configure/build exit 0；最终策略自检和 identity exit 0；只读真实 monitor preflight exit 0。实际右屏为 `(2560,0)–(5120,1440)`、96 DPI、identity rotation，保护屏为 `(0,0)–(2560,1440)`；仍将在正式捕获前重验，不把本次坐标当永久配置。

最终子进程检查 `readiness-tests.json` 共 6 项均符合预期：策略、身份、低于/高于时间边界、拒绝 source 参数、拒绝复用 preflight 目录。四个负例 exit 1 是预期拒绝；无源文件读取、无 capture，原 preflight 内容未变，非法 run 目录未创建。`--self-test` 同时覆盖整像素左屏相交拒绝、证据预算、恰好/低于 16 KiB、摘要/重开/冲突/清理警告拒绝、resume 和不完整 frame coverage 不产出有效硬门。

`Start-G21Encoder.ps1` 的 PowerShell AST 语法检查通过，四个脚本和已交付副本逐字节一致；没有执行 source 生成或远程广播。最终预提交 worker 内嵌 `e94da7f`，SHA-256 `ad22d39b0fa10db0b640041747e05dd50907ee1bbc1fb8590168c74048338807`，需与本提交的源代码 seals 一起解释，不能写成已嵌入新提交。提交后的重新 configure/build 身份和封存记录单列到同目录 `post-commit-audit.json`，不覆盖预提交测试证据。

**未执行：** 真实远程 1 MiB/64 MiB、实际捕获或派生中和验证、全量 CTest、ASan、既有 GPU/native 矩阵、20 GiB、G22 包/SBOM 验收。历史 218/218 仍只属于 `e0729b2`，不是当前新增 worker 的全量回归。

## 5. 下一步的操作顺序

1. 确认本机接收 worker 已提交、重新构建并封存；用户远程 Encoder 继续使用已检查通过的 `e94da7f` 包。两者 Git 身份不同必须如实记录；差异仅为 G21 测试入口/脚本/文档，生产库未改，不要求在远程机替换同一生产二进制。
2. 收到明确启动指令后，用户在**远程机**双击 `01_Start_Smoke_1MiB.bat`，等待动态编码画布出现。
3. 用户手动把远程连接画面完整放在本机右屏；保持整个编码画布可见、四角不裁切、不遮挡、不最小化。必要的远程窗口放大由用户完成，不能靠猜测放宽 scale/crop 质量门。
4. 用户回复“右屏画面已就绪”。本机随后重新枚举安全范围，用无窗口子进程启动该 worker，首次 smoke 上限 180 秒、外层 watchdog 210 秒。只读 `samples.jsonl` 汇报进展，不接收源文件或用源摘要辅助解码。
5. 收到停止指令后，用户切回远程脚本控制台按 Enter 或 Q，保留整个新 run 目录；提供 `source-manifest.json` 与 `encoder-report.json`。不要把 `.bin` 文件传回本机，不要先启动 64 MiB。
6. 发布后再独立流式计算最终文件的 SHA-256 和 BLAKE3，与 source manifest 和 Encoder 的 whole digest/Session identity 核对；检查唯一最终文件、准确大小、RAW/Segment 记录、真实 unique FPS/lane erasure/FER/control occupancy/goodput 和硬门。运行级文件真实性与 codeword truth 的证据粒度分开记录。
7. 只有 smoke 审核通过才安排 64 MiB。根据实际链路能否中和 chroma，补足 Base Luma 独立恢复证据；若采用同一实际 capture 的离线 neutralized 派生，必须另封存并标明离线证据，不能用其他截图、CPU 合成帧或 sender-side raster 冒充。

真实运行失败、低于 16 KiB 或证据不完整时保留原目录，先定向诊断，不自动重新启动源、调整门槛或越过 G21。
