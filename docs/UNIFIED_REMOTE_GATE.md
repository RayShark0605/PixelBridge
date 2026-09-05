# G21 真实远程像素链验收

## 1. 当前状态与前置

**2026-09-05：PARTIAL / SC6 V3 的真实远程 1 MiB 已正确发布并越过 16 KiB 硬门；首轮 64 MiB live 在 5/8 Segment 后触发双方有界时限，未发布。** `ceeef61` 的 receiver-first 运行以 55 个唯一帧发布 1 MiB，得到 19,065.018 B/unique frame。随后 `f93fd25` 的 64 MiB 运行在 1,800 秒 sender / 2,100 秒 receiver 硬时限内只完成 40 MiB；冻结 resume journal 证明 0..4 已完成，5..7 各保留数千个唯一方程。源码与运行状态共同定位到发送端逐 Segment 长突发、接收端最多 4 个在途 Outer decoder 的组合失配。当前修正把 Unified 冻结为 8-Segment 逐帧交织窗口，并把匹配的接收/恢复资源上限扩为 8；确定性 64 MiB 稀疏抽帧闭环已通过，但同提交 live 64 MiB 尚未执行，所以不能关闭 G21。完整历史见第 6–13 节，当前诊断与修正见第 14–15 节。

- 前置 G20：`e94da7f1d68fd3b410c180ac71201716c11bb9d7`，在途背压修复与本地功能收口已提交。原证据见 `UNIFIED_LOCAL_RELEASE_GATE.md` 第 16 节。
- G20 的 2.0x 是用户豁免、未验证；letterbox 的 15,420.2353 B/unique 不是性能通过。G21 的 16 KiB 硬门仍然有效。
- 用户确认两端由本项目提供：Encoder 由用户放到远程机、启动和摆放；Decoder 由本机控制，只捕获本机右屏。远程连接已经可用、具体类型未知。不自动连接远程、不操作用户输入。
- 用户回报远程 `00_Check.bat`：`PASS: Encoder loads; commit e94da7f1d68fd3b410c180ac71201716c11bb9d7`，`Script exit: 0`；未生成 source、未启动窗口或广播。这是**用户回报的远程加载证据**，不是已观察到的像素链。
- 用户随后明确同意：接收入口仅在专用测试构建启用，内部使用完整生产 `DecoderRuntime` 和 Auto 捕获；外层保护右屏，不扩展产品公共 CLI/Replay/monitor 配置。
- G21 的 SC6 真实 1 MiB、外部 SHA-256/BLAKE3、whole digest、安全发布和 16 KiB 性能门已有一次完整 PASS；真实 64 MiB、Base Luma 独立恢复及当前修正的同身份 live 仍未关闭。G22 未开始。

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

原来的 `01` / `02` 每次启动会生成新 source/Session；第 7 节新增同源诊断脚本，不替换这些脚本。不能把超时当成功，也不能在未知失败原因下反复新建 source 重试。**不得把 `.bin` 源文件交给 Decoder。** 用户仅在接收结束后提供 `source-manifest.json` 和 `encoder-report.json` 用于独立核验，源文件和其他日志保留在远程机。

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

**本节准备阶段未执行：** 真实远程 1 MiB/64 MiB、实际捕获或派生中和验证、全量 CTest、ASan、既有 GPU/native 矩阵、20 GiB、G22 包/SBOM 验收。后续仅追加第 6 节两次 1 MiB live；其他未执行项不变。历史 218/218 仍只属于 `e0729b2`，不是当前新增 worker 的全量回归。

## 5. 下一步的操作顺序

以下为默认 15 Hz 验收流程；当前批准的 5 Hz 诊断按第 7 节执行，不启动新的 `01` / `02` source。

1. 确认本机接收 worker 已提交、重新构建并封存；用户远程 Encoder 继续使用已检查通过的 `e94da7f` 包。两者 Git 身份不同必须如实记录；差异仅为 G21 测试入口/脚本/文档，生产库未改，不要求在远程机替换同一生产二进制。
2. 收到明确启动指令后，用户在**远程机**双击 `01_Start_Smoke_1MiB.bat`，等待动态编码画布出现。
3. 用户手动把远程连接画面完整放在本机右屏；保持整个编码画布可见、四角不裁切、不遮挡、不最小化。必要的远程窗口放大由用户完成，不能靠猜测放宽 scale/crop 质量门。
4. 用户回复“右屏画面已就绪”。本机随后重新枚举安全范围，用无窗口子进程启动该 worker，首次 smoke 上限 180 秒、外层 watchdog 210 秒。只读 `samples.jsonl` 汇报进展，不接收源文件或用源摘要辅助解码。
5. 收到停止指令后，用户切回远程脚本控制台按 Enter 或 Q，保留整个新 run 目录；提供 `source-manifest.json` 与 `encoder-report.json`。不要把 `.bin` 文件传回本机，不要先启动 64 MiB。
6. 发布后再独立流式计算最终文件的 SHA-256 和 BLAKE3，与 source manifest 和 Encoder 的 whole digest/Session identity 核对；检查唯一最终文件、准确大小、RAW/Segment 记录、真实 unique FPS/lane erasure/FER/control occupancy/goodput 和硬门。运行级文件真实性与 codeword truth 的证据粒度分开记录。
7. 只有 smoke 审核通过才安排 64 MiB。根据实际链路能否中和 chroma，补足 Base Luma 独立恢复证据；若采用同一实际 capture 的离线 neutralized 派生，必须另封存并标明离线证据，不能用其他截图、CPU 合成帧或 sender-side raster 冒充。

真实运行失败、低于 16 KiB 或证据不完整时保留原目录，先定向诊断，不自动重新启动源、调整门槛或越过 G21。

## 6. 首轮真实远程 smoke：遮挡修正与接收前准入阻塞

用户明确确认右屏画面就绪后启动真实接收，生产代码和测试入口均保持 `83bffb3a65361db7e59a54d581a376207efcdb43` 的冻结版本。实际 worker：

```text
build-unified-release/g21-receiver-1/receiver-worker/PBUnifiedRemoteGate.exe
SHA-256: 3f325779987618d0be8c84baf3bba0753052598f07f6205cbf300629134edbd4
```

两次均以无窗口子进程运行 `--receive NEW_ABSOLUTE_RUN_DIRECTORY 180`，由启动脚本仅向该子进程传入新输出目录与期限，外层 watchdog 210 秒。准确 argv、PID、起止时间分别记录在每个目录的 `launch.json` 和 `process-result.json`。没有 source、源摘要或 Session oracle 传入 Decoder；两次退出均为接收期限到达后的 exit 1，**没有 watchdog 强杀**。

| 项目 | 首次 `g21-remote-smoke-1` | 用户调整后的 `g21-remote-smoke-2` |
| --- | ---: | ---: |
| 实际进程耗时 | 180.344 s | 180.719 s |
| Backend / fallback / epoch | WGC / None / 1 | WGC / None / 1 |
| Capture arrived / delivered | 7,077 / 3,370 | 6,006 / 545 |
| Bootstrap accepted / rejected | 0 / 3,369 | 360 / 185 |
| 最终 admission drops | 0 | 359 |
| Copy 阶段 expired / pending high-water | 0 / 1 | 0 / 1 |
| Receiver accepted Transport | 0 | 0 |
| 正式 Session / 最终发布 | 未建立 / 否 | 未建立 / 否 |
| 输出目录文件数 | 0 | 0 |

证据根均在 `build-unified-release/` 下。两次完整 `samples.jsonl`、`final.json`、`failure-final.json`、`receiver-checks.json`、stderr 及失败审计保留，不用后一次覆盖前一次。两个 `recovered/` 均没有文件；此处不能把没有处理数据时的 conflict=0 或缺失的 false-accepted oracle 写成质量认证。正式 unique FPS、whole digest 和 per-unique hard metric 均不可用，不把不可用解释为“已测得低于 16 KiB”。

### 6.1 首次：明确的画布可见性问题

在重新核对 DISPLAY2 identity/rect 后，用单次有界 GDI `BitBlt` 只读取右屏 `(2560,0)–(5120,1440)`，保存 `right-placement.bgra/.png/.json`。该单帧显示底部两个 finder 被任务栏裁挡，右上 finder 有浮动叠层。它支持先修正摆放，但不能证明这是唯一原因。

用户手动调整窗口之后才启动第二次；第二次运行中用户又报告完成一次摆放调整，时间另存 `user-placement-update.json`。接收进程没有因此重启。第二份右屏位置图显示四角已可见，中段 Bootstrap 从 0 恢复到连续接受；但最终另有 185 个拒绝，**不能把整个 180 秒称为无遮挡/稳定通过**。

两份位置图仅用于布局诊断，**不是 Decoder 消费的那张 WGC 帧**，也没有执行任何 offline decode，因此不能充当 G21 要求的 same-capture neutralized/Base Luma 独立恢复证据。没有读取左屏、创建显示窗口、改变显示设置或注入输入；所有外部窗口调整由用户执行。

### 6.2 第二次：先定位结果准入，不放宽 250 ms

第二次 Bootstrap accepted=360，但 Receiver 始终没有绑定 Session；admissionDrops=359。累计 `postGpuFecCpuTotal100ns=1,395,205,443`，按 360 个已接受 Bootstrap 粗略折算约 **387.557 ms/帧**，明显提示后处理时延压力；capture 阶段 age 高水为 151.9582 ms，不能用它冒充结果取出时的年龄。

当前默认 worker 没有逐结果 take-QPC/有效捕获时间日志，因此聚合计数**尚不足以独立证明每次 CanAdmit 拒绝都来自 age，而不是其其他条件**。不把这个推断写成已经完成的精确根因定位，也不把“Bootstrap 成功”写成文件恢复成功。单 owner、250 ms、质量/FEC/CRC/摘要/发布门均未调整，没有拆 FEC 线程，也没有实施生产优化。

### 6.3 用户要求固定约 15 Hz：先核对实际发送行为

用户目测 Encoder 刷新过快、远控画面模糊，要求调整为固定约 15 Hz。现已再次核实：

- 已交付且未修改的 `Start-G21Encoder.ps1` 第 49 行传入 `--logical-fps 15`；第 54 行的 source manifest 也记录 `configuredLogicalFps=15`。
- CLI 将该值传递给 `EncoderConfig.logicalVisualFps`；Unified 未显式指定时默认也是 15，不回退到旧 profile。
- **这些是脚本/源码证据，不是远程实际帧率证据。** 不能把 WGC arrival rate、Present rate 或显示器刷新率当作 Encoder 独立逻辑帧率。
- 已请用户停止本轮并提供 `encoder-report.json` 与 `source-manifest.json`，不提供 `.bin`。用户随后将这两个指定文件放到本机桌面；仅从 Windows Desktop 已知目录读取这两个文件，原样封存到 `build-unified-release/g21-sender-report-1/`，未枚举其他用户目录或读取源文件。

收到的两个报告相互一致：RunId `8795d05374344818931dd77bf36aa96c`、SessionId `73465b8c3755a792df15269418c024eb`、1,048,576 bytes、Encoder `e94da7f` 及交付 EXE SHA-256 均匹配。实际 argv 传入 15；`configuredLogicalFps=15`、`observedSubmittedLogicalFps=15.002331006513039`、`submittedLogicalFrames=1553`，报告起止跨度 103.633 秒，正常手动停止且 `errorDetail` 为空。因此**这份报告对应的运行确实约为 15 Hz**，不能把它称为配置未限频；平均数仍不是每张 raster 的独立 dwell 证据。

不能将其冒充前两次完整发送记录：两个 Receiver 均未绑定 Session，缺少 Bootstrap identity 关联，发送报告约 103.6 秒也不覆盖任一完整 180 秒接收；没有远程/本机时钟同步证据，不能直接按 wall clock 强行拼接。sender 声明的 SHA-256/BLAKE3 仅封存为元数据，没有本机发布文件可供独立比对。报告 SHA-256 为 `f9b4b826a1a4b1ac9e4d38a26d4ccd256ba3d2b492534adc1e714b92e62cf3e8`；source manifest SHA-256 为 `93469483eb4903abfb101fc2a19fd8a9acd9273d8427cc4b9a73e54aac5733d7`，完整字段和关联限制见 `sender-audit.json`。

用户随后批准：复用该报告对应的同一个远程 1 MiB 源文件，临时降为 5 Hz 做一次短对照，Decoder 规则全部保持；此对照不能关闭 G21 的 15 Hz 验收。**附加包已准备，尚未启动 5 Hz 远程广播或接收，也没有再启动同参数的第三次盲目 smoke。** 未运行全量 CTest、ASan、GPU/native 回归、64 MiB、20 GiB 或 G22。下一目标仍为 G21，在已核实的 cadence 证据上进行定向诊断，而不是降低安全或质量门。

## 7. 已批准的 5 Hz 同源诊断附加包

证据及交付根：`build-unified-release/g21-5hz-handoff-1/`。新增源文件仅为 `tests/UnifiedRemoteGate/03_Check_SameSource_5Hz.bat`、`04_Start_SameSource_5Hz.bat`、`Start-G21SameSource5Hz.ps1`；未改 C++、公共接口或构建配置。Encoder `e94da7f` 和 receiver `83bffb3` 保持第 2、6 节的冻结 EXE，不重新构建或冒充本次脚本文档提交身份。

附加 ZIP：`PixelBridge-G21-5Hz-SameSource-addon.zip`，5,105 bytes，SHA-256 **`bc7672c3d99bde5e89f0cf431d11afe5da8c635becdd254886558ab304ff45b3`**。仅包含三个新增脚本、`expected-source-5hz.json` 和 `README_5Hz_SameSource.txt`，无 EXE、DLL 或 payload；每个 ZIP entry 与封存附加目录逐字节一致，脚本与源副本逐字节一致。原 ZIP、原脚本、两端 EXE 和 `PHASE1_GATE_REPORT.md` 的 SHA-256 均未变。

仅允许复用远程原包中以下文件，不重新生成或用其他文件代替：

```text
runs\smoke-8795d05374344818931dd77bf36aa96c\g21-smoke-8795d05374344818931dd77bf36aa96c.bin
bytes: 1048576
SHA-256: da27c9dd9f13d9683d1719c7d1a5741ba97cfe9d9282fd3ecaeee2363cd8d61a
```

预期值来自用户指定的远程 manifest，不是本机已读取源文件的证明。新增脚本检查有界 metadata、原文件名/大小/SHA-256、原 EXE hash 和加载身份；从哈希开始直到 Encoder 退出持续持有只读租约，禁止源文件写入/删除。`03_Check` 不创建 run、不生成源文件或启动广播；任何检查失败均停止，不能修改预期 hash 绕过。`04_Start` 要求用户先手动停止旧 Encoder；只把目标逻辑 FPS 改为 5，原 Unified profile、600 秒 sender 上限及手动停止方式保留。使用新的 create-only `runs/diagnostic-5hz-<RunId>` 保存报告，不覆盖旧证据。Session/恢复状态由原 Encoder 决定，以新报告为准，不清理旧状态，也不宣称两次发送具有相同 FrameSequence 或逐帧固定 200 ms。

### 7.1 最小验证及边界

- Windows PowerShell **5.1.26100.9278** 内置 AST parser：零语法错误，见 `syntax.log` / `syntax-result.json`。
- 五个定向脚本检查：非法 run ID、缺失源文件、错误大小、错误 SHA-256 均拒绝；**合成的 1 MiB 全零 policy fixture** 上，真实冻结 Encoder 的 `--build-identity` 加载及 `-CheckOnly` 通过。各次 fixture 内容和文件列表均未变，无广播、捕获或 Decoder 运行。
- 第一轮三个早期拒绝通过；后两项被本机继承的 PowerShell 7 `PSModulePath` 遮蔽 Windows PowerShell 5.1 的 `Get-FileHash` 而阻断。只在验证子进程删除该继承变量、使用 Windows PowerShell 默认模块路径后，原封不动的交付脚本对应两项通过；未改系统环境，未重跑已通过三项。原始失败和只读定位日志全部保留，不冒充首轮五项全绿。
- **本机合成 fixture 通过不是远程原文件通过。** 实际原文件仍须用户执行 `03_Check`，本机未读取、传回或向 Decoder 提供远程 `.bin`。未启动 5 Hz live；实际频率、模糊变化、Bootstrap/admission 与最终恢复情况尚无新结果。

精确执行参数在各 `results.json`；最小复核脚本保存在证据根，可在其下指定新目录重放，不覆盖现有结果：

```powershell
& <python> .\build-unified-release\g21-5hz-handoff-1\validate_addon.py --out .\build-unified-release\g21-5hz-handoff-1\checks-1
& <python> .\build-unified-release\g21-5hz-handoff-1\validate_addon.py --out .\build-unified-release\g21-5hz-handoff-1\checks-2 --cases wrong-hash synthetic-check-only
```

第一条为已保留的初次执行记录；保存的验证脚本已包含子进程环境修正，再次执行须使用新目录。`approval.json`、`handoff-audit.json`、两轮 `results.json` 记录批准、版本、hash、失败原因与未执行项。

### 7.2 用户操作和接收安排

以下安排在远程原目录和原 source 存在时才成立。用户随后明确报告两者均已删除，因此**不再执行本小节**，也不能从 SHA-256/BLAKE3 摘要恢复 CSPRNG 原文或伪造同源文件。改用第 8 节全新完整包。

1. 在远程机保留原 `remote-encoder` 目录及 `runs`，把附加 ZIP 的五个文件复制到 `00_Check.bat` 同级，不覆盖原文件。
2. 运行 `03_Check_SameSource_5Hz.bat`；通过后运行 `04_Start_SameSource_5Hz.bat`。若旧 Encoder 尚在运行，先在其控制台按 Enter/Q 正常停止。不要重新运行生成源文件的 `01` / `02`。
3. 用户手动将完整画布和四个 finder 放在本机右屏可见范围，然后回复“5Hz 右屏已就绪”。本机届时重新进行只读 monitor preflight，以相同冻结 receiver、相同整块右屏 ROI、新建输出目录、180 秒接收 / 210 秒外层 watchdog 执行一次对照，不操作输入。
4. 收到停止指令后正常停止远程 Encoder，只提供**本次新 run** 的两个 JSON；原 `.bin` 留在远程。比较时单列 source/Session/运行时段关联，不能用未关联的 15 Hz 老记录推导严格配对性能结论。

单 owner、250 ms、质量/FEC/CRC/摘要/发布/重开门全部不变。此次仅判断降频能否改善可见模糊和接收行为；无论结果如何，都不把 5 Hz 诊断等同于默认 15 Hz、64 MiB、chroma/Base-only 或 16 KiB 性能门通过。G21 仍 PARTIAL，G22 未开始。

## 8. 远程原目录删除后的全新 5 Hz 完整包

用户明确说明远程原 `remote-encoder` 整个目录已删除，而不是只删除本机提供的附加 ZIP。因此第 7 节要求复用的 CSPRNG source 也已丢失；现有 sender 报告中的摘要只能验证将来找到的候选内容，不能反推出 1 MiB 随机原文。没有把其他文件命名成旧 source 或修改预期 hash 绕过检查。

新增的全新安装包不依赖任何旧目录，解压到任意新目录后即可先做加载检查。它复用冻结 Encoder `e94da7f` 及原封不动的 42 个 `bin/` 文件，另含 `expected-build.json`、`PACKAGE_MANIFEST.json`、说明文件和三个 fresh-source 脚本：

```text
build-unified-release/g21-5hz-fresh-full-2/PixelBridge-G21-RemoteEncoder-e94da7f-Fresh5Hz-Full.zip
bytes: 26714458
SHA-256: 93123440c769031ecc1ebe73755d087a4a9c939ce1bb71ee51c82583d4982e2c
ZIP entries: 48
uncompressed bytes: 61461871
```

ZIP 根目录直接包含 `Fresh5Hz_00_Check.bat` 和 `Fresh5Hz_01_Start_1MiB.bat`，不会再要求把 add-on 合并到旧目录。启动脚本仅在确认没有运行中的 `PixelBridgeEncoder` 后新建 `runs/diagnostic-fresh-5hz-<RunId>`，用远程 OS CSPRNG 和有界 1 MiB buffer 生成全新的 1 MiB source，durable flush 后核对 regular-file/size；从 SHA-256 到 Encoder 退出持续持有拒绝写入和删除的只读租约。Encoder 参数固定为 Unified、`--logical-fps 5`、600 秒安全上限及手动停止。manifest 明确记录 `originalFixtureAvailable=false`、`strictSameSourceComparison=false`、`G21AcceptanceRun=false`，不将新 source 冒充旧 15 Hz 的逐字节控制变量。

### 8.1 最小验证

- Windows PowerShell 5.1 AST：零错误；静态核对只有一个 `--logical-fps 5` command 和一个 1 MiB CSPRNG buffer。
- 在组装目录执行真实 `Fresh5Hz_00_Check.bat`：exit 0，加载的应用为 Encoder `e94da7f`；package 文件逐字节未变，未生成 `runs`、source 或画面。
- ZIP CRC/entry/path/逐字节检查通过；解压到全新目录后再次从 batch 入口检查：exit 0，解压文件逐字节未变，未生成 `runs`、source 或广播。这是部署/加载证据，不是正常 sender、实屏或恢复成功。
- 首次组装目录 `g21-5hz-fresh-full-1/` 没有 ZIP。其 batch 检查在刻意缩窄且漏掉 Windows PowerShell 的验证 `PATH` 下返回 9009；没有 source/run/广播。问题确认后，两个新 batch 改为 `%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe` 绝对路径，并从全新 `full-2` 目录重做上述检查；原失败日志与 `FAILED_ASSEMBLY.json` 保留，不覆盖、不冒充首轮通过。
- 原 Encoder ZIP/EXE、冻结 receiver 及 `PHASE1_GATE_REPORT.md` 的 SHA-256 均未变；未重新构建产品，未运行 full CTest、ASan、GPU/native 回归、64 MiB、20 GiB 或 G22。

完整证据见 `build-unified-release/g21-5hz-fresh-full-2/handoff-audit.json`、`PACKAGE_MANIFEST.json`、两份 batch log 和 `syntax.log`。

### 8.2 新操作顺序

1. 用户把完整 ZIP 复制到远程机，完整解压到任意**新目录**；不能直接在 ZIP 预览中运行。
2. 双击 `Fresh5Hz_00_Check.bat` 并提供完整输出。该步骤不创建 source/run 或画面。
3. 检查通过后，按明确启动指令双击 `Fresh5Hz_01_Start_1MiB.bat`，再把完整画布和四个 finder 放到本机右屏，回复“5Hz 右屏已就绪”。
4. 本机届时仍使用冻结 `83bffb3` receiver、新建输出目录、整块右屏 ROI、180 秒接收 / 210 秒 watchdog；不操作输入或左屏，不把 sender source/摘要传入 Decoder。
5. 收到停止指令后正常停止远程 Encoder，只提供新 run 的 `source-manifest.json` 与 `encoder-report.json`，不提供 `.bin`。

fresh-source 诊断可以观察 5 Hz 下的画面及接收行为，但失去严格 same-source 差分能力。它仍不能替代 G21 默认 15 Hz、64 MiB、Base-only/chroma、外部发布摘要或 16 KiB 硬门；G21 继续 PARTIAL，G22 未开始。

## 9. 远控失真根因与 SC6 替代方案

### 9.1 5 Hz live 排除了“只需降频”

用户在全新完整包检查通过后启动 5 Hz 画面，本机以冻结 `83bffb3` receiver 对整块右屏执行 180 秒接收。用户指定的两个 sender JSON 在事后原样封存，sender 区间完整包含 receiver 区间：

| 项目 | 结果 |
| --- | ---: |
| Sender configured / observed | 5 Hz / 5.0014566 Hz |
| Sender submitted frames / duration | 1,273 / 254.617 s |
| Receiver WGC duration | 180.528 s |
| Capture arrived / copied / delivered | 5,890 / 198 / 198 |
| Bootstrap accepted / rejected | 198 / 0 |
| Capture admission drops | 197 |
| Post-GPU/FEC CPU 平均 | 836.25 ms / accepted Bootstrap |
| Accepted Transport / Session / publish | 0 / 未绑定 / 否 |

receiver 正常期限退出 1，没有 watchdog 强杀、输出文件或错误发布。严格配对审计位于 `build-unified-release/g21-remote-smoke-5hz-fresh-1/paired-run-audit.json`。该运行把“原 sender 实际远高于设置”排除掉：15 Hz 报告约 15.0023 Hz，5 Hz 报告约 5.0015 Hz，二者都符合配置；降低到 5 Hz 仍无法让 LC4 建立 Session，因此 cadence 不是充分修复。

### 9.2 真实截图的可证事实

用户提供的四张 2560×1440 LC4 运行截图原样封存于忽略目录 `build-unified-release/g21-user-distortion-samples-1/`。四角 locator、Bootstrap 条带和较大的 freshness patches 仍可辨认，但大面积连续 4×4 Data 微纹理被远控链变成方向相关、相邻相关且随帧变化的灰色碎片；这与“Bootstrap 198/0 而 Transport 0”的 live 计数一致。截图本身不是同一 WGC frame 的 sender truth，不能用于统计 false acceptance 或反推原始 payload。

用户提供的四张 libcimbar 网页端参考截图封存于 `build-unified-release/g21-libcimbar-reference-screenshots-1/`。它们显示数据单元之间存在稳定黑色间隔、符号轮廓与高饱和色差，视觉上没有 LC4 那种跨单元连续灰色微纹理；这里只把它用作设计方向，不把截图当成 PixelBridge 性能证据。

### 9.3 对本地 libcimbar 源码的只读核对

审计对象为用户提供的 `D:\libcimbar` 和 `D:\libcimbar\cimbar_js.html`；没有修改或构建该仓库。关键运行事实：

- `src/lib/cimb_translator/GridConf.h` 的 `Conf5x5` 使用 `cell_size=5`、`cell_spacing_x/y=6`，即 5×5 symbol 外有 1 px 间隔；
- `src/lib/cimb_translator/CimbReader.cpp` 在需要时对灰度 symbol grid 使用 3×3 十字锐化核，再做局部 adaptive threshold；
- `src/lib/image_hash/average_hash.h` 与 `CimbDecoder.cpp` 从含边界的局部窗口生成 fuzzy average hashes，并按中心/邻位 drift 候选选择最小 Hamming distance；
- `src/lib/cimbar_js/cimbar_recv_js.cpp` 的网页接收入口在 extractor 请求时启用上述 preprocess，再进入 fountain decode；
- `GridConf.h` 同时给每帧块配置显式 ECC 和 fountain chunks。因此其鲁棒性来自物理间隔、形状判决、预处理、局部漂移和两层冗余的组合，而不是某个网页 CSS 或动画频率。

审计摘要保存在 `build-unified-release/g21-libcimbar-source-audit-1/`。PixelBridge 没有引入 OpenCV、libcimbar wire 或源代码依赖；这里只采用通用信号处理结论，并继续使用既有 QC-LDPC/Wirehair/Receiver/Storage 安全链。

### 9.4 首版离线 Profile：`PB-Unified-SC6-V2`（历史，已由第 10 节取代）

用户明确允许修改整个项目并声明不要求兼容后，LC4/layout 8 从产品 admission 移除，历史源码生成器和 Golden 仅保留为内部回归。下表是当时 SC6 V2/layout 9 的冻结合同，不是当前产品 identity：

| 项目 | 当前值 |
| --- | ---: |
| VisualProfileId / layout | `0x5042554E49534332` / 9 |
| Canvas / pixel format | 1920×1080 / BGRA8 SDR |
| Data 单元 | 6×6；5×5 glyph + 第六行/列暗隔离带 |
| Data tiles | 41,872 |
| 每 tile | 4 shape/luma bits + 2 chroma bits |
| Base / Fine / Chroma codewords | 9 / 1 / 5 |
| 全帧 codewords / payload | 15 / 19,710 B |
| 最大 Control / 最小 Transport slots | 8 / 7 |
| 当前 mapping digest | `8718eb1c1b43764160f8a79b437cec0ac5cee46072aeb7d08ef0930fbe450fbc` |
| 可解码尺度 | 1.0x..2.0x；低于 1.0x neutral matte pause |

16 个 5×5 glyph 为互补对，前景数 8..17、pairwise Hamming distance `>=9` 且无孤立同值像素。Base/Fine/Chroma 使用不同置换且 codeword 不跨 lane。独立 point-sampling 夹具证明 0.75x 的 SC6 采样点会跨过物理单元边界；因此最小尺度改为 1.0x，而不是扩大误差容忍、让错误像素进入 FEC。右屏 2560×1440 的 4/3 等比呈现是 1.3333x，不受下限变化影响。

CPU 与 HLSL 在 shape luma 模板打分前应用同一个十字反卷积：`clamp(5*center-left-right-top-bottom, 0, 255)`。核系数和为 1，平坦 calibration levels 不变；它只抵消远控低通，chroma 判决仍使用未锐化 RGB，避免放大色噪声。算法及阈值没有 provider 名称、品牌或模式分支。

QC-LDPC layered min-sum 的 check-node 更新原来为每个目标 edge 重扫本行其他 edges，严重帧约 241..243 ms。当前实现先为整行计算 sign parity、第一/第二绝对最小值及其唯一索引，再为每条 edge 排除自身，数学结果相同而复杂度由 O(degree²) 降为 O(degree)。Inner-FEC 全部旧/新断言通过；严重场景稳定态约 50..64 ms，三种 GPU backend 的关键失真帧约 29..31 ms。AMD/NVIDIA 首个 dispatch 分别出现约 631/408 ms 冷启动尖峰，故仍保留单在途与 250 ms fail-closed admission，不声称所有帧均实时通过，也没有拆分 FEC owner。

### 9.5 最小验证和完整文件离线证据

本轮遵守 G21 定向预算，没有运行 full CTest、ASan、真实 UI/屏幕或 64 MiB。核心命令：

```powershell
<python> tests\PBModulation\generate_unified_sc6_mapping_golden.py --check
<python> tests\PBModulation\generate_unified_sc6_cpu_golden.py --check
cmake --build build-presentation-release --config Release --target PBUnifiedTransformCorpusTests PBDemodD3D11Tests PBDemodShaderBytecodeTests PBInnerFecTests --parallel 4
.\build-presentation-release\tests\PBModulation\Release\PBUnifiedTransformCorpusTests.exe --rng-seed 21092026
.\build-presentation-release\tests\PBDemodD3D11\Release\PBDemodD3D11Tests.exe '[.unified-g12]' --rng-seed 12122026
.\build-presentation-release\tests\PBDemodD3D11\Release\PBDemodD3D11Tests.exe '[.unified-g21-offline]' --rng-seed 21092026
.\build-presentation-release\tests\PBDemodD3D11\Release\PBDemodShaderBytecodeTests.exe
.\build-presentation-release\tests\PBInnerFec\Release\PBInnerFecTests.exe
```

定向结果：SC6 transform corpus 1 case / 5,104,511 assertions；WARP+AMD+NVIDIA parity 21 scenarios / 16,729 assertions；shader bytecode 61 assertions；Inner FEC 37 cases / 47,202 assertions；全部通过，false accepted/truth mismatch/conflict output 为 0。Profile、mapping 和 CPU oracle 分别为 8 cases / 823、3 / 19,683,249、14 / 493,327 assertions；RenderD3D 为 20 cases / 376，WARP offscreen 为 1 / 14；scheduler 5 / 186,681，G17 telemetry 6 / 4,202，G15/G16/G17 application 分别为 11 / 505、5 / 212、6 / 272；均通过。

新增隐藏 G21 Gate 使用真实 1 MiB OS-CSPRNG RAW、正式 Session/Manifest/Segment Control、Wirehair V2、GPU WARP 解调、ReceiverIngress、PBStorage、whole-file BLAKE3、安全发布和 final reopen。失真链固定为 `2560×1440 bilinear + centered 4:2:0 phase(1,1) + one box blur + 5-bit BGR quantization`。五次独立 source/run 均在 54 个唯一逻辑帧后发布逐字节正确文件：

```text
1,048,576 / 54 = 19,418.074074 B/unique logical frame
hard 16 KiB: PASS
engineering 32 KiB: MISS
false accepted / truth mismatch / conflict output: 0 / 0 / 0
whole digest / safe publish / final reopen: true / true / true
```

五次分别为 12,725、12,715、12,735、12,737、12,735 assertions，报告位于 `build-unified-release/g21-sc6-offline-distortion-1/run-{1,2,3,4,5}.jsonl`；每份都明确写入 `authority="headless synthetic WARP; not live remote capture"`。最强单帧在锐化前只接受 8/15（Base/Fine/Chroma hard errors 15/3/0），当前 CPU 与三个 GPU backend 均接受 15/15、hard errors 0/0/0。

### 9.6 当前退出边界

该方案已经用独立 Golden、CPU/GPU parity、严重失真和五次完整文件发布证明“值得进入 live”，但 G21 仍不能关闭。尚未执行：

1. 用提交后同身份的完整 SC6 Encoder 包重新跑真实远程 1 MiB；
2. 真实 64 MiB RAW；
3. live 外部 SHA-256/BLAKE3、错误文件数与正式 Session identity 核对；
4. live Base Luma 独立恢复或同一实际 capture 的 neutralized 派生；
5. live `>=16 KiB/unique logical frame`、稳定态 250 ms admission 和 cold-start 丢帧后的收敛证明。

在用户恢复可配合前不启动屏幕/capture、不操作输入。提交后可生成自包含的新远端 Encoder ZIP 和本机 receiver worker，但它们的加载检查仍不是 live 成功。下一目标仍是 G21，不进入 G22。

## 10. SC6 V3：跨帧远控失真的定向修复

### 10.1 从截图到可重放的时域失真模型

用户追加的四张真实运行截图证明 LC4 Data 区不是稳定的均匀模糊：碎片方向和局部清晰度会随截图变化，符合远控视频链在缩放、低通、色度抽样和帧间预测/插值共同作用下产生的内容相关时域污染。截图没有 sender truth，不能直接计算 BER；因此把它转成确定、可复核且比第 9 节更强的模拟链：

```text
current canonical frame + previous canonical frame * 30/255
-> 2560x1440 bilinear (4/3)
-> centered 4:2:0, phase (1,1)
-> one 3x3 box blur
-> 5-bit BGR quantization
```

reference blend 明确使用每个测试帧自己的上一逻辑帧，而不是固定噪声图；三条 mandatory case 分别覆盖 sequence `41<-40`、`51<-50`、`64<-63`。这不是声称未知远控 provider 正好使用 30/255，而是把此前缺失的“上一帧残留”维度纳入产品回归。provider 名称仍不进入 wire、Profile、阈值或分支。

### 10.2 V2 全局交织为什么失败

V2 在 30/255 blend 下可出现 6 个 current、3 个 stale freshness regions；每个 region 的 hard timing bits 仍可能全对，但 analog residual 不同。全局 affine interleave 把三个 stale regions 的 zero metrics 均匀撒到全部 15 个 QC-LDPC codeword，结果不是只损失局部容量，而是所有 codeword 同时失去足够信息。临时静态 region mapping 能恢复 9 个 Base blocks / 11,826 B，却让既有“当前 Bootstrap + 完整上一帧 Data”负例错误接受 15 blocks，因此没有保留该不安全方案。

当前唯一产品身份原子替换为：

| 项目 | 当前值 |
| --- | ---: |
| Name | `PB-Unified-SC6-V3` |
| VisualProfileId / layout | `0x5042554E49534333` / 10 |
| Mapping version / sequence period | 3 / 16 |
| Base / Fine / Chroma codewords | 9 / 1 / 5 |
| Mapping digest | `4b06ec15c338a4f18502b49a7d5947bd33b4e79fd1a7f73c53c682dfa2283639` |
| Freshness residual / phase residual | `0.075` / `0.125` |
| Minimum decision metric | 64 |

V3 先把 Base codeword 0..8 分别放入对应 freshness region；边角 region 容量不足的部分只溢出到确定的 luma surplus，故每个 Base codeword 至少 93% 位于本 region。Chroma 按冻结 region 顺序串接，使每个 codeword 最多跨三个 regions。随后只在**各自 16,200-bit codeword 内**按 `FrameSequence mod 16` 置换；不会把一个 region 的擦除扩散到所有 codeword，同时完整旧帧仍因错相位、QC-LDPC、CRC 和 identity fail closed。Golden generator 独立重建 tile ownership、region ordering、置换及 160-byte contract，不调用生产 mapping。

freshness analog residual 从 Bootstrap timing 判断中拆为独立 `maximumFreshnessResidual=0.075`；Bootstrap timing、phase residual、decision metric、QC-LDPC、Transport/Control CRC、identity 和 250 ms admission 均未降低。Data glyph 与 phase checker 都可比较原始样本和同一和为 1 的结构化锐化模型：

```text
clamp(5 * center - left - right - top - bottom, 0, 255)
```

phase checker 仍必须让预期 phase 严格优于其余七个 phase，且 normalized residual 仍不超过 `0.125`。这修复了“Data 已按低通模型恢复、Fine phase pilot 却因仍看原始模糊样本而整 lane 擦除”的观测模型不一致，不是扩大容差。错误序列负例继续要求 0 accepted blocks；新增的 `40 -> 48` 相位别名负例在 phase pilot 仍有效时替换全部 Data，直接证明 16-phase codeword-local permutation 不会退化为可拼接的静态 region mapping。

### 10.3 冻结验证

本轮只执行 G21 受影响的最小验证，没有运行 full CTest、ASan、真实屏幕、live remote 或 64 MiB：

| 验证 | 结果 |
| --- | --- |
| 独立 mapping Golden `--check` | PASS；5 files；digest `4b06ec...3639` |
| 独立 CPU raster Golden `--check` | PASS；6 files |
| SC6 mapping | PASS；4 cases / 19,910,103 assertions |
| SC6 profile / product profile | PASS；8 / 823，3 / 16 assertions |
| CPU oracle（含 policy 和完整错序负例） | PASS；15 cases / 494,588 assertions |
| mandatory transform corpus | PASS；24 cases encoded in 1 corpus / 5,833,845 assertions |
| shader bytecode | PASS；1 / 61 assertions |
| WARP + AMD + NVIDIA semantic parity | PASS；24 mandatory + 2 semantic mutation scenarios / 19,395 assertions |
| Application 产品身份 | PASS；1 / 23 assertions |

三条时域 mandatory records 均为 `falseAcceptance=0`、`freshnessBitErrors=[0,0,0,0,0,0,0,0,0]`；accepted payload 分别为 19,710、15,768、17,082 B，其中 Base 分别为 11,826、10,512、10,512 B。WARP、AMD Radeon 和 NVIDIA 对每条记录的 accepted block count 与 accepted-set BLAKE3 完全相同。corpus Golden 为 `PixelBridge.UnifiedTransformCorpus.3`，SHA-256 `fa2ce4d94279b71eb18d111ccf0efcfbe59ab266a81e4a4f697d4a4299a75b87`。

### 10.4 三次时域完整文件发布

隐藏 Gate 使用每帧真实上一 canonical frame 做 30/255 blend，并继续走正式 1 MiB OS-CSPRNG RAW、Session/Segment/FinalManifest Control、Wirehair V2、WARP demod、ReceiverIngress、PBStorage、whole-file BLAKE3、安全发布、final reopen 和逐字节比较。三次独立 source/run 结果：

| Run | Unique logical frames | Verified encoded B/unique frame | 16 KiB | 32 KiB |
| --- | ---: | ---: | --- | --- |
| 01 | 60 | 17,476.2667 | PASS | MISS |
| 02 | 63 | 16,644.0635 | PASS | MISS |
| 03 | 59 | 17,772.4746 | PASS | MISS |

三次的 whole digest / safe publish / final reopen / byte equality 均为 true，false accepted / truth mismatch / conflict output 均为 0。报告和 stdout 位于 `build-unified-release/g21-sc6-v3-temporal-distortion-1/run-01..03/`；每份 authority 都是 `headless synthetic WARP; not live remote capture`。首次在 phase-pilot 修复前的运行虽完成发布但在 16 KiB 计算处失败，修复后一次早期成功未作为三次冻结组混入；原失败不删除、不改写。

### 10.5 当前边界和下一步

V3 已证明能在显式跨帧残留与原最强空间/色度链叠加时完整恢复文件，并且三次均超过 G21 的 16 KiB 硬门；这解决的是可重放的根因和实现缺口，不等于未知 live 链已经通过。仍待用户恢复配合后执行：

1. 用提交后相同 V3 identity 的完整 Encoder 包跑真实远程 1 MiB / 默认 15 Hz；
2. 审核 live whole digest、安全发布、final reopen、外部 SHA-256/BLAKE3、错误文件数和实际 unique FPS；
3. 同链 64 MiB RAW；
4. live chroma 可用和 Base-only（或同一 actual capture 的 neutralized 派生）；
5. 核对 250 ms admission、cold-start 丢帧后的收敛以及 `>=16 KiB/unique logical frame`。

在用户明确恢复配合前不启动 capture、不显示窗口、不操作输入。上述 live 项缺一不可，故 **G21 仍为 PARTIAL，G22 未开始**。

## 11. SC6 V3 远端包纯灰暂停与全屏呈现修正

### 11.1 纯灰不是数据栅格

用户报告 `PixelBridge-G21-RemoteEncoder-bba268e-SC6V3-Full.zip` 的
`01_Start_Smoke_1MiB_15Hz.bat` 启动后画面为纯灰。对 ZIP、启动脚本和冻结二进制的只读核对确认：

- ZIP SHA-256 仍为 `64cbbefa03d8d3f3b25d33c5a4904f28597fd9ed7eed98f0fa91d3abbce7b4fa`，脚本确实启动
  `bba268e` 的 SC6 V3 Encoder，并非残缺包或错误 EXE；
- 旧脚本没有传入 `--single-monitor-fullscreen`，因此创建的是带边框的可缩放窗口；
- Data Window 的最低显示比例仍为 1.0。只要远控桌面、任务栏、窗口边框或 DPI 使实际 client area 的宽或高低于
  `1920x1080`，`ResolvePresentationViewport` 就返回 `PausedBelowMinimumScale`；
- 该状态有意不提交数据 raster，而由 `PresentNeutralMatte` 清成 RGB code value 128，窗口标题同时变为
  `窗口过小，广播已暂停`。因此纯灰是 fail-closed 暂停面，不是 SC6 payload、shader 编译失败或全灰编码结果。

冻结包内 `bba268e` 原 EXE 在不切换用户桌面的私有 Windows desktop 上用新的 1 MiB source 启动，正常生成并提交了
一个真实 SC6 V3/layout 10 logical frame 后因不可见 desktop 被 DWM occlude；报告位于
`build-unified-release/g21-gray-diagnosis-1/`。这排除了包内 raster 生成链损坏，但私有 desktop 结果不属于可见屏幕或
live remote 证据。

### 11.2 最小产品修正

Unified 产品入口现在显式接受 `--single-monitor-fullscreen primary|DEVICE`。启动时把选定显示器的物理 RECT、
`HMONITOR`、未旋转状态和 identity 原子写入配置；只接受能容纳 `1920x1080` 且不超过 `3840x2160` 的单块显示器。
窗口改为精确覆盖该显示器的无边框 topmost popup；1920x1080 canonical SC6 raster 以 1:1 居中拷入同尺寸 framebuffer，
余区才使用中性灰。因此任务栏或 non-client chrome 不再把数据 client area 压到 1.0 以下，也不会通过缩小、裁剪或插值
来换取显示成功。

启动前及运行期间仍复核同一 monitor identity、RECT 和拓扑；不支持的尺寸、旋转、identity 冲突或拓扑改变均直接失败，
不会静默回到普通窗口。原 1.0 最低比例、SC6 V3 wire/layout、质量阈值、250 ms admission、FEC、摘要和安全发布门均未放宽。
三个 G21 远端启动脚本统一声明 `RemoteVisual / UnknownRemoteLink` operator metadata，并使用 primary fullscreen；provider
标签仍不进入解码或阈值分支。

### 11.3 定向验证和边界

只执行受影响最小验证：

```powershell
cmake --build build-unified-release --config Release --target PBApplicationTests PixelBridgeEncoder --parallel 4
.\build-unified-release\tests\PBApplication\Release\PBApplicationTests.exe '[single-monitor][fullscreen]' --rng-seed 21092026
```

结果为 3 cases / 35 assertions 全部通过，覆盖 Unified 正例，以及 dual authority、ProtectedMonitor、identity/origin、旋转、
过小显示器和错误 profile 的拒绝；三个 PowerShell 5.1 AST 均为零错误，并分别只有一组 fullscreen/remote/provider 参数。
工作树产品 EXE 随后在不可见私有 desktop 上以
`Unified + RemoteVisual + UnknownRemoteLink + primary fullscreen + 15 Hz` 运行 2 秒，exit 0、stderr 为空；journal 记录
`singleMonitorFullscreen=true` 和 4 次 topology revalidation，report 记录 `2560x1440` / `\\.\DISPLAY1`。证据位于
`build-unified-release/g21-gray-diagnosis-3/`。该 probe 使用未重新配置的测试 build tree，嵌入旧 build identity，故只作为
工作树启动路径证据；提交后的交付 EXE 必须重新配置、重建并单独核对其 identity。

没有切换用户桌面、显示窗口、执行 capture 或操作鼠标键盘；没有运行 full CTest、ASan、native/remote live、64 MiB 或 G22。
新包仍需真实远端显示和 Decoder 恢复验证；本节只关闭“旧包为什么纯灰”和产品全屏入口缺失，**G21 继续 PARTIAL，G22 未开始**。

## 12. 两次真实 1 MiB 发布与 FullRepairPass 方程修正

### 12.1 两轮 live 证据

`6addde627bfbc8457ad748fc4a39fe8eafe295ae` 的同身份 Encoder/Decoder 在未知远控链上完成两次独立的 1 MiB OS-CSPRNG RAW 运行。Decoder 始终只读取本机右侧 `DISPLAY2` 的真实 WGC 像素；sender source 未传给 Decoder，用户事后只复制 `encoder-report.json` 和 `source-manifest.json` 用于配对与外部摘要核验。

| 项目 | Run 1：sender-first / late join | Run 2：receiver-first |
| --- | ---: | ---: |
| Sender 相对 Receiver 启动 | 提前 165.677 s | 延后 49.694 s |
| Configured / observed sender FPS | 15 / 15.000343 | 15 / 15.000575 |
| Receiver unique visual FPS | 1.573692 | 9.012836 |
| Unique logical frames | 116 | 72 |
| Verified encoded B/unique | 9,039.4483 | 14,563.5556 |
| 1 MiB 硬门允许的最大帧数 | 64 | 64 |
| Capture admission drops | 0 | 0 |
| Accepted Control / Transport | 20 / 1,726 | 28 / 1,055 |
| Bootstrap accepted / rejected | 142 / 887 | 76 / 708 |
| Whole digest / rename / final reopen | true / true / true | true / true / true |
| 16 KiB hard gate | FAIL | FAIL（多 8 unique frames） |

两轮的 commit、profile `PB-Unified-SC6-V3`、layout 10、SessionId、SessionTag、source filename 和 1,048,576 bytes 均逐项匹配。三条 lane 的 QC-LDPC failures、Transport CRC failures 和 identity failures 均为 0，Outer conflict rejections 为 0；每个输出目录只产生一个与本轮 source 对应的正确最终文件，没有错误发布。

Run 1 的 source/published SHA-256 均为 `2d8cf3a2ca8a54f6f5f4266b353918a44b5cb23516a62beaf3115f9c7c0c0226`，sender/receiver/published BLAKE3 均为 `4263910735b9e103183685251efdb08a9284b261856d0d9807d72948f3b19e36`。Run 2 的 SHA-256 均为 `cae80bc40d28467760ea1b35ca6ad522e57c43e53efd3b694dc269fb94b46ee1`，BLAKE3 均为 `d2f3a23883b7ca452d40d240ed4386b797140323e3e43fde1cd1e093963fc4d4`。

证据分别封存于：

- `build-unified-release/g21-sc6-v3-live-1/smoke-1mib-15hz-ecbbfc084f734b22b57c8b103477bd20/`
- `build-unified-release/g21-sc6-v3-live-1/smoke-1mib-15hz-240984e5f7294b5aad7cf11cbb4b7357/`

首轮 `paired-run-audit.json` 曾把 RunReport 根级性能字段误从 `unifiedTelemetry` 提取，导致两个辅助计数字段写成 0；原 receiver/sender 报告、9,039.4483 指标和 Gate 判定未受影响。该文件原样保留，修正值另存于 `paired-run-audit-corrected.json`，没有覆盖失败证据。第二轮 `paired-run-audit.json` 已直接使用正确根级字段。

### 12.2 已确认根因

receiver-first 将 116 unique frames 降到 72，证明启动顺序会影响 Control 获取和有效采样，但仍无法解释最后 8 帧开销。1 MiB RAW 的正式 Outer payload 为 1,314 B/symbol，Wirehair `K=ceil(1,048,576/1,314)=799`，20% 初始冗余 `R=160`，所以每个 full round 恰为 959 个方程，相当于 64 个全 Transport frame-equivalents；实际 mixed scheduler 的初始 Control 占用使完整一轮跨 65 个 logical frames，不能把 64 个 frame-equivalents 冒充完整调度轮长度。

总体设计 11.1/11.2 节要求：Pass 0 发送 `K systematic + R0 repair`；Pass N>0 的 `K+Rpass` 预算必须**全部**使用从未用过的新 repair IDs。实现审计发现 `SenderUnifiedCarouselScheduler` 过去每轮都报告 `systematicEquationCount=K`，`SenderFrameBuilder::MapEquationToOuterBlockId` 又把每轮局部索引 `0..K-1` 固定映射回 systematic IDs。远控链只保留约 9 个 accepted unique frames/s 时，跨轮采样因此反复收到相同方程；Run 2 虽接收 1,055 个合法 Transport blocks，仍需要 72 帧才积累足够独立 Wirehair 方程。

这不是 SC6、QC-LDPC 或远控图像判决失败：两轮全部 lane FEC/CRC/identity failures 为 0，且最终字节严格正确。根因位于无反馈 Carousel 的方程新鲜度。

### 12.3 最小修正

- `SenderUnifiedCarouselSchedulerConfig` 新增默认值为 0 的 `carouselPass`；所有旧调用保持 Pass 0 行为，调度器只在 Wirehair 且 `carouselPass>0` 时进入 repair-only。
- Pass 0 仍调度 799 systematic + 160 repair；Pass N>0 保持相同的 959-equation FullRepairPass 物理预算，但 snapshot 为 0 systematic + 959 repair。
- 每个 transport slot 显式携带 `repairEquation` 和 `repairEquationOffset`。运行时直接用 durable `NextRepairOuterBlockId + repairEquationOffset` 分配 ID，不再用 `equationIndex < K` 猜测方程类型。
- 初始轮使用 IDs `0..958`；下一轮从 durable high-water 959 开始，使用 `959..1917`。每轮完成后按实际 repair equation count 推进 high-water；崩溃/重启继续从已持久化 lease end 分配，不复用可能已经展示过的 repair ID。
- 最后一物理帧的 padding 不获取新 ID，只重复本轮已经调度的方程：Pass 0 重复已有 systematic，FullRepairPass 重复本轮开头的 repair。
- DirectRepeat、零字节 Session、Control cadence、15 Hz、SC6 V3 identity/layout、slot 数、QC-LDPC、Wirehair 参数、质量阈值、250 ms admission、摘要和安全发布门均未改变；没有 provider-specific 分支。

### 12.4 定向验证与退出边界

只执行 G21 受影响的最小 Release 验证：

```powershell
cmake --build build-unified-release --config Release --target PBUnifiedSenderSchedulerTests --parallel 2
cmake --build build-unified-release --config Release --target PBApplicationTests PixelBridgeEncoder --parallel 2
build-unified-release\tests\PBApplication\Release\PBUnifiedSenderSchedulerTests.exe --rng-seed 21092026 --reporter console
build-unified-release\tests\PBApplication\Release\PBApplicationTests.exe "[application][g15][scheduler]" --rng-seed 21092026 --reporter console
build-unified-release\tests\PBApplication\Release\PBApplicationTests.exe "[application][g15][pixels][runtime]" --rng-seed 21092026 --reporter console
```

结果分别为 6 cases / 239,188 assertions、1 / 37、1 / 128，全部通过。新增 G21 单例自身为 1 case / 17,506 assertions，逐 slot 验证首轮与后续轮的 equation kind/offset、full-round 数量、padding，以及 `0..958` 与 `959..1917` 两个不相交 ID 区间；DirectRepeat 和零字节 Session 即使位于后续 Carousel pass 也保持各自原有语义。Release Encoder 编译通过。

没有运行 full CTest、ASan、GPU/CPU corpus、native 屏幕、额外 capture、64 MiB、20 GiB 或 G22。本节两次 live 均属于修正前 `6addde6`；它们权威证明远控失真下 whole-file 正确发布，也权威证明旧调度没有达到 16 KiB 硬门。修正提交后必须重建同身份完整 Encoder/Decoder，再执行 receiver-first 的 1 MiB / 15 Hz live 复验；通过后仍需 64 MiB RAW 与 chroma/Base-only 证据。因此 **G21 继续 PARTIAL，G22 未开始**。

## 13. FullRepair live 与 Control burst 抽帧别名

### 13.1 第三轮同身份 live 结果

`5cedd15649582d8fd73f84fc34b8578646c07c5f` 的完整 Encoder 包通过无副作用 `00_Check.bat` 后，先启动本机
DISPLAY2-only Decoder，再由用户启动远端 1 MiB / 15 Hz Encoder。Receiver 比 sender 早启动 93.677 秒；sender
保持 15.000659 Hz，Decoder 从真实 WGC 像素观察到 3.575722 unique Hz。运行最终状态为 `Completed`，但 Gate
因性能硬门返回 1，而不是崩溃或超时：

| 项目 | 结果 |
| --- | ---: |
| Unique logical frames | 192 |
| Verified encoded B/unique | 5,461.3333 |
| 16 KiB hard gate | FAIL |
| Accepted Control / Transport | 52 / 2,833 |
| Capture admission drops / expired | 0 / 0 |
| Demod pending / result queue high-water | 1 / 1 |
| Whole digest / rename / final reopen | true / true / true |
| Base/Fine/Chroma FEC, CRC, identity failures | 0 / 0 / 0（每条 lane） |
| Outer conflict rejections | 0 |

画布为稳定约 1.33333x，marker residual 为 0.069..0.352 px；没有裁切、积压或 250 ms admission 失败证据。
source 与 published 文件均为 1,048,576 bytes，外部 SHA-256 均为
`8f846bc1a98ed6c23af74ca3a2ff7c8b663dbeb2a8cb4f083dc83b5e3de75812`；sender/receiver/published BLAKE3
均为 `b1c5872d55af8edabbda061cf01f2ae28719dc3e4289a81932c933e6fe9fbb4d`。提交、profile/layout、SessionId、
SessionTag、source filename 与大小全部严格配对。证据封存于：

```text
build-unified-release/g21-sc6-v3-live-full-repair-1/
  smoke-1mib-15hz-19de4398558a4dfb9a79c74dbb1b8fd0/
```

其中 `sender-evidence/` 是桌面报告的哈希一致副本，`paired-run-audit.json` 同时记录外部双摘要、身份、时序和
Control 状态变化；原 `failure-final.json` / `failure.txt` 未覆盖。

### 13.2 现场时序定位出的根因

FullRepair 修正后的新方程可以收敛并正确发布，但没有缩短到 64 unique frames。进一步核对 scheduler 与每秒
receiver samples 后得到可复现的窄链路：

1. 产品 `controlRepetitions=4`，非空 Session 每个 burst 共 12 slots；
2. 旧顺序按 record kind 分组为 `Session×4 → Manifest×4 → Segment×4`；
3. mixed Control 上限为 8，因此第一帧恰好只有 4 Session + 4 Manifest，第二帧才有 4 Segment；
4. live 在 8/16/24 accepted Control slots 时仍为 `ReceivingControl`；首次采样到 `Recovering` 时为 36 slots，
   与多次只抓到 8-slot 首帧、最终才抓到一次 4-slot Segment 帧完全一致；
5. 进入 `Recovering` 时 unique frames 已达 147、Transport blocks 已达 2,184。没有 SegmentDescriptor 时不能创建
   正式 Wirehair decoder，早到数据只能进入有界 orphan 路径，不能把合法 block 数直接等同于可用于恢复的独立方程数。

所以第三轮的决定性瓶颈是 Control burst 与远控周期抽帧发生别名，导致当前 SegmentDescriptor 长期不可见；不是
FullRepair ID 再次重复，也不是 SC6、QC-LDPC、CRC、identity、几何或在途背压失败。

### 13.3 最小修正与定向验证

总 Control 数仍为 12，只把次序改为按 record kind 交织：

```text
frame 0: Session, Manifest, Segment, Session, Manifest, Segment, Session, Manifest
frame 1: Segment, Session, Manifest, Segment
```

因此两个 Control-bearing frames 的任意一个都同时含 Session、Manifest、Segment；远控只保留任意一帧即可建立
完整控制状态。零字节 Session 同理只交织 Session/Manifest。下列项目均未改变：Control repetitions=4、每轮 Control
slots=12、10 秒 cadence、15 codewords、Transport 方程顺序和总预算、FullRepair 新 ID、SC6 V3 identity/layout、
QC-LDPC/Wirehair 参数、质量门、单 owner/250 ms、摘要和发布门；没有 provider-specific 分支或隐藏 IPC。

只执行受影响的最小 Release 验证：

```powershell
cmake --build build-unified-release --config Release --target PBUnifiedSenderSchedulerTests --parallel 2
build-unified-release\tests\PBApplication\Release\PBUnifiedSenderSchedulerTests.exe "[g21]" --rng-seed 21092026 --reporter console
build-unified-release\tests\PBApplication\Release\PBUnifiedSenderSchedulerTests.exe --rng-seed 21092026 --reporter console
cmake --build build-unified-release --config Release --target PBApplicationTests PixelBridgeEncoder --parallel 2
build-unified-release\tests\PBApplication\Release\PBApplicationTests.exe "[application][g15][scheduler]" --rng-seed 21092026 --reporter console
build-unified-release\tests\PBApplication\Release\PBApplicationTests.exe "[application][g15][pixels][runtime]" --rng-seed 21092026 --reporter console
```

结果分别为 2 cases / 17,667 assertions、7 / 239,349、1 / 37、1 / 128，全部通过；Release Encoder 构建通过。
新增测试显式验证两个 burst frames 各自包含全部三类 descriptor，合计仍严格为每类 4 次、12 slots。第一次构建
只因新增测试把 `uint32_t` 与有符号字面量比较而被 `/WX` 拒绝；改用 `8U/4U` 后通过，失败日志原样保留在
`build-unified-release/g21-control-interleave-1/build-scheduler.log`。

### 13.4 当前退出边界

没有运行 full CTest、ASan、GPU/CPU corpus、额外实屏、64 MiB、Base-only、20 GiB 或 G22。第三轮 live 权威证明
`5cedd15` FullRepair 可以在真实失真链发布正确文件，也权威证明旧 Control 排列低于硬门；调度修正目前只有定向
Release 证据，必须在提交后重建同身份 Encoder/Decoder 并再跑 1 MiB / 15 Hz live。通过后仍需 64 MiB RAW 与
chroma/Base-only 证据。因此 **G21 继续 PARTIAL，G22 未开始**。

## 14. Control 交织后的真实 1 MiB 硬门通过

`ceeef61fb5fc32d834d919b35780b1c0e548f8ed` 的同身份 Encoder/Decoder 在 receiver-first 顺序下完成第四次
SC6 V3 真实远程 1 MiB / 15 Hz 运行。Receiver 比 sender 早启动 68.901 秒；sender 实际提交速率为
15.000604 Hz，Decoder 从 DISPLAY2 的 WGC 像素观察到 3.202689 unique Hz。最终结果如下：

| 项目 | 结果 |
| --- | ---: |
| Unique logical frames | 55 |
| Verified encoded B/unique | 19,065.0182 |
| 16 KiB hard gate | **PASS** |
| Accepted Control / Transport | 24 / 799 |
| Whole digest / rename / final reopen | true / true / true |
| Base/Fine/Chroma FEC failures | 0 / 0 / 0 |
| CRC / identity / Outer conflict | 0 / 0 / 0 |

source 与 published 文件均为 1,048,576 bytes，外部 SHA-256 均为
`dc51c3202eeabb93ed22f5408ff936dcb7eaf9e55073b3d08fdcafabb7b2898f`；sender/receiver/published BLAKE3 均为
`c1266b3497c20f293d640de6c6f06947037cd0d7a8b2bd930afc8e51c6d851d8`。提交、Profile/layout、SessionId、
SessionTag、source filename、大小及时段全部严格配对，source 未提供给 Decoder。证据封存于：

```text
build-unified-release/g21-sc6-v3-live-control-interleave-1/
  smoke-1mib-15hz-7ddc54fb7897403b88320a6420358760/
```

`sender-evidence/pairing-audit.json` 结果为 `PASS`。这轮权威关闭了当前 SC6 V3 的 live 1 MiB、外部双摘要、
正确发布及 16 KiB 硬门；它没有覆盖多 Segment 资源行为，也不替代 64 MiB 或 Base-only。

## 15. 64 MiB 有界超时、ActiveSegmentWindow 根因与修正

### 15.1 冻结 live 结果不是崩溃

为避免先前 180 秒短门把大文件正常收敛误判为故障，`0ae506d` 只把 remote Encoder 的受约束 hard maximum
扩为 1,800 秒，`f93fd25` 只把专用 Receiver Gate 的允许区间扩为 2,100 秒；没有改变视觉、FEC、质量、250 ms、
摘要或发布门。`f93fd257b64ab7efe7687fce73c06d64f0380a51` 的同身份 64 MiB CSPRNG/RAW 运行随后得到：

| 项目 | 结果 |
| --- | ---: |
| Sender submitted logical frames | 27,001 @ 15.000156 Hz |
| Receiver unique logical frames | 6,232 @ 3.587830 Hz |
| Verified Segments / raw bytes | 5 / 8；41,943,040 B |
| Sender / Receiver elapsed | 1,800.404 s / 2,100.471 s |
| Watchdog 强杀 | false |
| Whole digest / publish / final reopen | unavailable / false / unavailable |
| Base/Fine/Chroma FEC failures | 0 / 0 / 1 |
| CRC / identity / Outer conflict | 0 / 0 / 0 |

Encoder 到配置硬时限后自动停止，因没有 receiver ACK 仍按 fail-closed 返回 1；Receiver 到自身硬时限后返回 1，
没有发布错误文件，也没有删除可恢复状态。`.part` 精确预分配 67,108,864 bytes，`.resume` 为 20,236,376 bytes。
`VerifiedEncodedBytesPerUniqueFrame` 正确保持 `null / NotPublished`，不能用 40 MiB 中间进度冒充 G21 性能。
冻结证据位于：

```text
build-unified-release/g21-sc6-v3-live-long-deadline-1/
  full-64mib-15hz-ba1f09ba84d047c88834cfda5e4bffa7/
```

### 15.2 决定性运行状态与根因

对原 `.resume` 进行只读、逐 record CRC-32C 校验后确认：完成 ordinal 为 `0..4`；仍活动的 5/6/7 分别
持有 5,491 / 5,533 / 3,745 个互不重复的 OuterBlockId。解析结果、原 journal/report 哈希和证据边界写入
`scheduler-diagnosis.json`；没有重开或改写原 resume。这个状态排除了“最后三个 Segment 从未到达”，并表明它们
已经接收大量有效方程但尚未达到恢复阈值。

当时 sender 对每个 8 MiB Segment 连续发送一整个 round，再切到下一 Segment；而默认 Receiver 只允许 4 个活动
Outer decoder。真实链只观察约 24% sender 帧，一个 Segment 在单轮结束前通常不能完成，于是最先看到的四个
Segment 占满 decoder，第五个及以后只能在后续空位出现时重试。长突发还让同一时段失真集中损伤单个 Segment。
这正是总体设计要求 `ActiveSegmentWindow` 逐帧 temporal striping、且 Receiver 资源必须覆盖 W 的未实现部分。

### 15.3 最小产品修正

- Unified sender 的 `ActiveSegmentWindowSize` 冻结为 8；每个逻辑帧只推进窗口内一个 Segment，顺序为
  `0,1,...,7,0,1,...`。每个 Segment 保留独立 Wirehair/DirectRepeat encoder、scheduler 和 repair-ID high-water；
  一个窗口全部完成 round 后才滑到下一窗口或推进 Carousel pass。
- Unified Receiver 的 `maxActiveOuterFecDecoders` 同步从 4 提升到 8；旧视觉 Profile 的默认值仍为 4。
  默认 1 GiB 总 decoder 预算不变，本次 8×8 MiB 实测保留 457,201,696 bytes，仍在原预算内。
- 崩溃恢复只在 window/pass 边界持久化 Carousel 位置，避免每帧原子替换；每个 Segment 的 repair lease 仍在任何
  可能展示前单独持久化，重启不会复用可能已经显示过的 repair ID。
- Decoder resume journal 允许并保留 8 个活动 Segment；第 9 个仍 fail closed。超过产品支持的 policy 同样拒绝。
- Decoder report/journal 新增 active limit/current/peak、reserved bytes current/peak、`DeferredResourceBusy` 与
  Outer-FEC quota 的真实计数。专用 G21 Gate 对任一资源延期/配额事件保持 fail closed，避免再次只表现为超时。
- Wire format、SC6 V3 identity/layout、15 Hz、Control 交织、FullRepair、Wirehair 参数、单 owner、250 ms、质量、
  whole digest、安全发布和 final reopen 门均未改变；没有 provider-specific 分支或隐藏 IPC。

### 15.4 稀疏抽帧 64 MiB 定向闭环

新增隐藏的 no-raster G21 probe 复用产品 `SenderFrameBuilder`、真实 Wirehair V2 和 `ReceiverIngress`，构造 8×8 MiB
确定性 RAW source。它让约 75% sender 帧完全不可观察（不进入 Decoder unique-frame 分母），并在可观察帧中继续
注入 4-frame burst Data erasure（Bootstrap 仍计入分母），强制经历多个 fresh-repair pass。冻结结果为：

| 项目 | 结果 |
| --- | ---: |
| Sender / observed unique frames | 15,795 / 3,870 |
| Unobserved / accepted-but-Data-erased | 11,925 / 118 |
| Completed Carousel passes | 3 |
| Unique Outer symbols | 51,081 |
| Verified Segments / exact bytes | 8 / 67,108,864 |
| Receiver active decoder peak | 8 |
| Receiver reserved decoder peak | 457,201,696 B |
| DeferredResourceBusy / quota exceeded | 0 / 0 |
| Verified encoded B/observed unique frame | **17,340.7917** |

所有 8 个 Segment 的 encoded/raw digest 和 source byte equality 均通过；15,795 sender frames 也低于现场 1,800 秒
hard maximum 可提交的约 27,000 帧。其余最小 Release 验证为：8-Segment 调度 90 assertions、resume 81、Decoder
report 112、单 Segment scheduler/raster 邻接 37/128，以及 `PBUnifiedRemoteGate --self-test`；全部通过。

该 probe 没有编码/显示/捕获屏幕像素，所以只关闭调度、Outer FEC、资源和 digest 的确定性回归，不冒充 live。
本轮没有运行 full CTest、ASan、GPU/corpus、实际屏幕或 G22。提交后必须重建同身份 Encoder/Decoder，再跑真实
64 MiB；还需 Base Luma 独立恢复（或同一 actual capture 的 neutralized 派生）。因此 **G21 继续 PARTIAL，G22 未开始**。
