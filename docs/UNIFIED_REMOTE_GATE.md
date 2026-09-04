# G21 真实远程像素链验收

## 1. 当前状态与前置

**2026-09-05：PARTIAL / 两次真实远程 smoke 均未发布；用户随后提供的 Encoder 报告已核实配置 15 Hz、实际平均约 15.0023 Hz。** 该报告不能与此前两次接收完整关联。用户已批准同一远程源文件的 5 Hz 定向对照；附加脚本及本机最小检查完成，尚待远程原文件检查和右屏就绪，未运行 5 Hz 接收。初始准备和未运行边界保留在下文，第 6 节记录两次 live 结果，第 7 节记录附加包。

- 前置 G20：`e94da7f1d68fd3b410c180ac71201716c11bb9d7`，在途背压修复与本地功能收口已提交。原证据见 `UNIFIED_LOCAL_RELEASE_GATE.md` 第 16 节。
- G20 的 2.0x 是用户豁免、未验证；letterbox 的 15,420.2353 B/unique 不是性能通过。G21 的 16 KiB 硬门仍然有效。
- 用户确认两端由本项目提供：Encoder 由用户放到远程机、启动和摆放；Decoder 由本机控制，只捕获本机右屏。远程连接已经可用、具体类型未知。不自动连接远程、不操作用户输入。
- 用户回报远程 `00_Check.bat`：`PASS: Encoder loads; commit e94da7f1d68fd3b410c180ac71201716c11bb9d7`，`Script exit: 0`；未生成 source、未启动窗口或广播。这是**用户回报的远程加载证据**，不是已观察到的像素链。
- 用户随后明确同意：接收入口仅在专用测试构建启用，内部使用完整生产 `DecoderRuntime` 和 Auto 捕获；外层保护右屏，不扩展产品公共 CLI/Replay/monitor 配置。
- G21 的 1 MiB、64 MiB、chroma/Base Luma、外部摘要与远程性能门均未关闭。1 MiB 已有两次失败 live 记录，不能按准备检查通过收口。G22 未开始。

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

1. 在远程机保留原 `remote-encoder` 目录及 `runs`，把附加 ZIP 的五个文件复制到 `00_Check.bat` 同级，不覆盖原文件。
2. 运行 `03_Check_SameSource_5Hz.bat`；通过后运行 `04_Start_SameSource_5Hz.bat`。若旧 Encoder 尚在运行，先在其控制台按 Enter/Q 正常停止。不要重新运行生成源文件的 `01` / `02`。
3. 用户手动将完整画布和四个 finder 放在本机右屏可见范围，然后回复“5Hz 右屏已就绪”。本机届时重新进行只读 monitor preflight，以相同冻结 receiver、相同整块右屏 ROI、新建输出目录、180 秒接收 / 210 秒外层 watchdog 执行一次对照，不操作输入。
4. 收到停止指令后正常停止远程 Encoder，只提供**本次新 run** 的两个 JSON；原 `.bin` 留在远程。比较时单列 source/Session/运行时段关联，不能用未关联的 15 Hz 老记录推导严格配对性能结论。

单 owner、250 ms、质量/FEC/CRC/摘要/发布/重开门全部不变。此次仅判断降频能否改善可见模糊和接收行为；无论结果如何，都不把 5 Hz 诊断等同于默认 15 Hz、64 MiB、chroma/Base-only 或 16 KiB 性能门通过。G21 仍 PARTIAL，G22 未开始。
