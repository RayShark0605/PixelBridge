# Receiver 接纳原因最小诊断

> **维护范围 / Scope (2026-09-17):** 本文保留该模块的协议/工具/测试参考，不再作为项目当前路线或发布状态入口。当前模式、单屏接收、预算、日志与完整文件证据见 [中文文档](../../docs/README.md) / [English documentation](../../docs/README.en.md)。历史日期、Gate、现场坐标与阶段参数仅适用于当时记录；不应直接复制到新环境。
> This is a module/tool/test reference, not the current release roadmap. Use the bilingual index for current behavior and validation boundaries; historical gates/settings are not universal defaults.

此工具服务于**非本机整文件有效传输效率**的损失归因。它不是新 Decoder、payload 旁路、视频参数搜索器或现场测速工具。

当前实现入口：

- `apps/common/receiver_decision_trace.h`：固定 15-slot、无 payload 的输出结构与 JSON writer；结构小于 4 KiB。
- `apps/common/local_desktop_runtime.cpp`：原 ReceiverPipeline 分支的只读观测；正常产品调用不启用该结构。
- `apps/common/recorded_pixel_replay.h/.inc`：离线接口末尾新增默认关闭的 `receiverDecisionDiagnostics`；源码调用兼容，静态消费者需要重新编译/链接。
- `main.cpp`：只读既有 30 帧 BGRA，显式 WARP，经原解调/Receiver/存储恢复；无 codec、窗口或捕获输入。
- `test_decisions.cpp`：在测试 TU 中包含原 runtime，验证其同步接纳边界；不向产品公开测试 payload 输入。
- `run.py`：串行无窗口子进程、启动前加入 2 GiB Job、create-only 日志、失败停止。
- `analyze.py`：外部整文件及诊断开/关/历史原始 trace 核对；不回馈 Decoder。
- `analyze_history.py`：只读已有 R1 trace/journal，核验控制摘要和源码身份；不解码录像。
- `seal.py`：本轮源码增量、输入、二进制和证据封存/只读复验。

## 诊断合同

每帧新增 `receiverDecisions`，关闭时为 `null`，开启时 schema 为 `PixelBridge.ReceiverDecisions.1`。

`before/after` 只表示原状态对象：`sessionReady` 是 Session 对象已建立，不保证所有 Segment 或 Manifest 齐备；`pendingConfirmation`、`receiverSessionBound`、`published` 分别记录原确认、Receiver 绑定及发布状态。

最多 15 个 slot，按 slot 编号排列，只记录本次原处理路径检查的 retained-frame slot。`observedThisCall=false` 表示原有单帧缓存中的块，不是诊断新引入的缓存。前置帧拒绝或已经发布时可没有 slot；不能从空列表推断未解调出数据。

- `receiverCalled`：确实调用了原 Receiver ingress。
- `receiverReturned`：该调用返回了结果，包括失败结果；**不等于接纳成功**。
- `dataDisposition` / `outerSymbolAdmission`：仅成功的 Transport ingress 结果有值，未调用/失败/Control 为 `null`。
- `CachedOrphan` 的 Unique 仅说明原有受限缓存保留了数据，不是已绑定的新恢复方程，更不是 verified 文件字节。
- `AlreadyAdmitted` 表示原单帧去重跳过，不同于调用 Receiver 后返回 `IdenticalDuplicate`。
- `ControlAccepted` 表示原控制接纳及应用侧处理返回成功，不单独区分所有控制层内部重复子状态。
- `protocolError` 保留实际协议错误编号；`resourceRejected` / `conflictRejected` 为相应已知分支。其他异常以帧 `ProcessingFailed` 和原 replay error 报告为准。
- 原 `receiverDataDisposition` 保留旧值；新增 `receiverDataDispositionAuthority=LegacyDefaultDoNotUseForUnified`，禁止用旧默认值推断 Unified 接纳。

发生处理异常时记录该帧的可用诊断，随后重新抛出原异常；不读取下一帧、不重试恢复。JSON 仍受单记录 64 KiB / 总计 64 MiB 约束，诊断容量失败不会放宽接纳。

## 本轮已用预算与结果

证据根：`<repo>\artifacts\receiver-decision-diagnostics-20260908-run01`。

- 原始 64 KiB 文件、30 帧、15 fps、1920×1080 BGRA8，每次新的 Receiver/输出根。
- 诊断 off/on 各一次：**60/60 WARP 像素观察**，两次都完成摘要/发布/reopen，外部逐字节/SHA-256/BLAKE3 一致。
- 非时钟旧字段 off/on 完全一致，并与原 Step3-B 封存原始结果一致。
- 两次均有 50 个新 Outer 方程、1 个已验证 Segment、65,536 verified Raw/Encoded bytes；无资源拒绝、冲突或资源延后。
- 新诊断为 5 个 Processed 帧、25 个 AlreadyPublished 帧；50 个实际数据调用、15 个实际控制调用。
- 定向测试 6 类：5 个接纳场景各 off/on，共 10 个 admission-only 场景，另测 trace 容量/溢出/坏输出流。**这些不是像素观察**。
- 监督器采用更紧的单进程 300 s 上限（低于已批 600 s），2 GiB process/job；两次实际 peak commit 低于 111 MB。
- 没有 codec 编解码、全录像重放、实屏、完整回归、压力测试；没有性能改善声明。

## 不消耗新观察的复验

```powershell
$python = '<python>'
$tool = '<repo>\tools\PBReceiverDecisionProbe'
$root = '<repo>\artifacts\receiver-decision-diagnostics-20260908-run01'
& $python -B "$tool\analyze.py" --root $root
if ($LASTEXITCODE -ne 0) { throw 'Parity evidence verification failed' }
& $python -B "$tool\analyze_history.py"
if ($LASTEXITCODE -ne 0) { throw 'Historical attribution verification failed' }
& $python -B "$tool\seal.py" --root $root --verify
if ($LASTEXITCODE -ne 0) { throw 'Seal verification failed' }
```

这些命令只读现有文件并输出 stdout。`analyze.py` 外部读取源文件仅作事后验证，不启动 Decoder。

## 独立构建与再次运行

本轮实际构建根为 `<repo>\build-receiver-decision-20260908-run01`；当前应用库从源码重编译，核心依赖复用原 Step2 Release `.lib`。不链接旧 `PBApplication.lib`，不装依赖、不修改历史构建。显式关闭全局 vcpkg MSBuild 自动注入；最终 CL read logs 中没有全局 vcpkg headers。精确输入见 `identity/BUILD_INPUT_IDENTITY.json`。

新构建可使用全新目录执行：

```powershell
& 'C:\Program Files\CMake\bin\cmake.exe' `
  -S '<repo>\tools\PBReceiverDecisionProbe' `
  -B '<repo>\build-receiver-decision-reproduce-new01' `
  -G 'Visual Studio 17 2022' -A x64 `
  -DPB_DECISION_REPO='<repo>' `
  -DPB_DECISION_BASE_BUILD='<repo>\build-remote-step2-20260908-run01'
```

再构建 `PBReceiverDecisionProbe` 和 `PBReceiverDecisionTests`，必须保存该次命令、日志和新源码/二进制身份，不能冒用本轮 seal。

再次像素运行**需要另一份明确批准的 60 观察预算**。程序入口为 `--off <original-source.bgra> <new-root>` 和 `--on <original-source.bgra> <another-new-root>`；须用 `run.py` 的同一 Job supervisor 或等价有界监管，先保存输入/runtime 哈希及 create-only reservation。不能直接重跑本轮 `parity`；现有 `NORMAL_OBSERVATION_RESERVATION.json` 会拒绝重复执行。迁移证据包时只核验相对成员；原绝对路径复验命令要求原输入仍在原位置。

完整结果和后续候选见 `docs/REMOTE_RECEIVER_DECISION_DIAGNOSTICS_2026-09-08.md`。
