# 文件反馈辅助传输 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** 在全部四种 GUI 模式实现默认关闭的 Decoder 写入/Encoder 只读反馈、按段减少无效发送、完成后世代安全自动停止和后台完成监听，交付双端独立候选包。

**Architecture:** 专用有界快照与纯 C++ 状态策略分离。可能阻塞的文件 I/O 位于随包 helper，运行时仅传递有界元数据；Decoder 从耐久提交/实际最终验证导出，Encoder 在原子提交边界应用，失效回到原调度。保留既有 publish journal 复用最终文件恢复验证。

**Tech Stack:** Windows x64、C++20、Win32 文件/进程/Job Object、现有 CRC32C/BLAKE3、Catch2、CMake/MSVC、Qt Widgets；不升级或下载依赖。

**Spec:** [已批准设计](../specs/2026-10-21-feedback-assisted-transfer-design.md)

**Status:** 用户已审阅并批准本计划，选择 Native（当前会话主 Agent 执行，最终独立审阅）。

## Global Constraints

- 全部四模式；一个指定 Decoder；关闭时无反馈 I/O、原行为与原先验保持。
- 正向载荷只来自捕获像素；helper/反馈只传元数据，不读取或交换源/恢复载荷。
- 不改视觉 wire/Profile/FEC/原 Golden 字节、PBJH/PBJR 格式、摘要/发布/资源规则。
- 单快照最多 64 KiB、最多 65,536 段；发布/读取约 1 秒，调度提示无新合格观察 15 秒失效。
- 最多现有六活动发送段；确定性回访，不把缺段全集加载成 codec。
- 完成后监听保留最终状态和冻结文件耗时；用户停止/关闭能安全退出。
- 自动停止在运行 owner 二次验证 Start token/实际 Submit 帧范围，不能从文件线程直接调用通用 Stop。
- 基础信任限于专用可信单写者位置、唯一 Sender、未克隆/回滚的 Session cache；不是认证协议。
- 不执行真实映射/远控/物理桌面实验。可执行本地测试、故障子进程和离屏 GUI。
- 保留 1,303 文件起点副本；不覆盖既有修改、受保护报告或 dist；不 stage/commit/push。
- C++ 变量 lowerCamelCase、函数 PascalCase、独立行大括号、常量局部 const、后置递增；核心不依赖 Qt。

## Review Focus

1. 文件已完成但 A 重启：B 必须重新观察本次帧并重新验证持久 final，不能放宽旧终态准入（Task 5/6）。
2. 用户快速 Stop/Start：晚到 helper 结果/完成通知不得影响新 run；空实际提交集合不能误算范围（Task 2/4/6）。
3. 映射永远不返回：主 UI/提交/关闭不得等待卡住的 helper，也不能不断增开进程（Task 4）。
4. 稀疏完成位图、空文件及小文件 barrier：四模式都必须真实改变服务且保留恢复，不只优化大 PAM4（Task 2/6）。
5. 最终反馈写失败或保留 journal：文件完成不降格，进度/计时不继续增长，重新开始需先终止旧监听（Task 5/7）。

---

## File Structure and Shared Interfaces

- Create apps/common/feedback_protocol.h/.cpp：数据结构、完整快照 codec/validation。
- Create apps/common/feedback_policy.h/.cpp：接纳、时效、确认集合、Start 范围、统计。
- Create apps/common/feedback_file_store.h/.cpp：安全路径/文件读写、单写者锁，仅 helper 调用。
- Create apps/common/feedback_io_process.h/.cpp：隔离 helper 的进程/管道/超时/最新槽管理。
- Create apps/PBFeedbackIo/CMakeLists.txt、main.cpp：Qt-free helper，只有 read/write 元数据操作。
- Create apps/common/feedback_options_qt.h/.cpp：双端可复用控件及偏好适配，不链接进 PBApplication。
- Modify apps/common/local_desktop_runtime.*、application_model.*、run_report.*：配置/运行时/状态/报告。
- Modify apps/PixelBridgeEncoder 的主/Shell GUI/controller 和 apps/PixelBridgeDecoder 的 GUI/controller；不另造传输入口。
- New tests/PBApplication/test_feedback_{protocol,policy,file_store,io_process,runtime,options,report}.cpp 及 test-only helper fixture；各目标按职责独立注册。
- Candidate producer/verifier、完整基线/执行日志和任务 diff 位于本轮 artifacts，正式工具门禁不绕过。

下列接口在 owning task 中定义，后续任务按同名调用，不在接线处复制 codec 或策略：

- FeedbackStatus：显式成功/失败状态和 message，失败不得部分更新输出。
- FeedbackConfig：FeedbackLocationKind（Directory/File）和用户所选 filesystem path；EncoderConfig/DecoderConfig 用 optional 表示关闭。
- FeedbackIdentity：SessionId、profileId、layoutVersion、fileBytes、segmentCount、canonicalSessionDigest、canonicalManifestDigest。
- FeedbackSnapshot：identity、receiverId/receiverEpoch、snapshotSequence、durableCompleted 位图、observedFrameSequence/available、phase、wholeFileDigest、三层验收 flags、reportedCompletionMilliseconds。
- FeedbackTelemetry：requested/effective/status、接受/拒绝/过期/I/O计数、confirmedSegments、skippedService、terminalPublished/Accepted、listening、stopReason。
- FeedbackSenderPolicy：Reset、RecordSubmittedFrame、Accept、Expire、IsSegmentConfirmed、CanStop、GetTelemetry。
- FeedbackIoProcess：Start、QueueSnapshot、Poll、Stop；全部结果携带本次 Start token。

---

### Task 1: Bounded snapshot codec and isolated build baseline

**Files:** Create feedback_protocol.h/.cpp and test_feedback_protocol.cpp; modify apps/common/CMakeLists.txt、tests/PBApplication/CMakeLists.txt. Create local Configure-FeedbackBuild.ps1 under this task's artifacts.

**Interfaces:** Produce FeedbackConfig/Identity/Snapshot/Status/Telemetry and SerializeFeedbackSnapshot(const FeedbackSnapshot&, vector<byte>&) noexcept、ParseFeedbackSnapshot(span<const byte>, FeedbackSnapshot&) noexcept、ValidateFeedbackSnapshot(const FeedbackSnapshot&) noexcept。

- [x] Read current binary readers/writers, ComputeCrc32c and ComputeBlake3Digest; freeze explicit field offsets/known flags in a short source contract, never struct persistence.
- [x] Configure build-feedback-assisted-20261021 using existing build cache paths for toolchain, Qt and sealed vcpkg installation; PB_BUILD_APPS/TESTS ON, all real-display gates/tools/fuzz/bench OFF, manifest install OFF. Save discovered local paths only in ignored artifacts.
- [x] Write failing tests named Feedback snapshot round-trips exact identities and 65536-segment bitmap、Rejects truncated trailing unknown flags and CRC、Rejects inconsistent completion and unused tail bits. Assert exact 8,192-byte maximum bitmap and <=65,536 total bytes, empty output unchanged on failure.
- [x] RED: build PBFeedbackProtocolTests; record missing implementation/failed valid roundtrip, not an unrelated compiler error.
- [x] Implement explicit little-endian codec, checked field sizes, nonzero epoch/sequence, legal phases, exact bit count/tail, CRC and full terminal consistency. Catch allocation/length errors into FeedbackStatus.
- [x] GREEN: cmake --build build-feedback-assisted-20261021 --config Release --target PBFeedbackProtocolTests; ctest --test-dir build-feedback-assisted-20261021 -C Release -R "^PBFeedbackProtocolTests$" --output-on-failure.
- [x] Save task diff and checks; no Git commit.

### Task 2: Receiver-state acceptance and deterministic selection policy

**Files:** Create feedback_policy.h/.cpp and test_feedback_policy.cpp; register PBFeedbackPolicyTests.

**Interfaces:** Consume Task 1. Define FeedbackSenderPolicy::Reset(const FeedbackIdentity&, uint64_t runToken)、RecordSubmittedFrame(uint64_t)、Accept(const FeedbackSnapshot&, uint64_t nowNanoseconds)、Expire(uint64_t)、IsSegmentConfirmed(uint64_t) const、CanStop(uint64_t currentRunToken) const、GetTelemetry() const. Define bounded segment-selection helper using confirmed bitmap and a round-robin cursor.

- [x] Write failing tests for full identity mismatch, same sequence/different content, duplicate no TTL renewal, 15-second expiry, same-Start membership, empty range, next/lease/future exclusion, stale token and all-complete-but-not-final.
- [x] Add review-focus cases: sparse early hole vs tail hole; 0/1/6/12/13/65,536 segments; deterministic fair visits, selected set <=6 and every selected ordinal valid.
- [x] RED: run PBFeedbackPolicyTests and retain failures.
- [x] Implement single immutable accepted snapshot, epoch replacement without bitmap OR, bounded retired-epoch tracking (maximum 8; further ambiguity disables feedback until new Start), overflow-safe actual Submit interval and separate terminal/short-lived freshness rules.
- [x] Implement selection only from unconfirmed complement; when complement empty but no valid final receipt, return an explicit legal-visual keepalive decision rather than a zero-segment forged Session or stop.
- [x] GREEN: build and ctest PBFeedbackPolicyTests; verify disabled/no snapshot returns no override and no invented progress.
- [x] Save exact interfaces/targeted test results in work log.

### Task 3: Safe local feedback file primitive

**Files:** Create feedback_file_store.h/.cpp and test_feedback_file_store.cpp; register PBFeedbackFileStoreTests.

**Interfaces:** Consume Task 1. Produce ResolveFeedbackFilePath(const FeedbackConfig&) without I/O, ReadFeedbackFile(const filesystem::path&, vector<byte>&) noexcept, and FeedbackFilePublisher::Open/Publish/Close for one program-owned writer.

- [x] Write failing tests using local scratch directories: directory/file path resolution, absent read creates nothing, read-only handles permit replacement, unowned existing content is never overwritten, hardlink/reparse/directory refusal, concurrent writer refusal and malformed/oversized file.
- [x] RED: run tests before the file primitive exists.
- [x] Implement A reads with GENERIC_READ/OPEN_EXISTING only, explicit share modes and handle-based bounded length/read. No mtime shortcut and no recursive scan.
- [x] Implement B exclusive owned lock, same-parent CREATE_NEW temporary file, full write/FlushFileBuffers, same-object retirement and no-replace publication/restoration; retain bounded .previous on conflict and clean only owned objects. Validate leaf/parent identity limitations explicitly.
- [x] GREEN: PBFeedbackFileStoreTests plus unchanged PBStorageTests; assert source/unrelated files and target content remain byte-identical on refused writes.
- [x] Record no real UNC/mapped target was accessed.

### Task 4: Bounded helper and process I/O isolation

**Files:** Create apps/PBFeedbackIo/{CMakeLists.txt,main.cpp}, feedback_io_process.h/.cpp, test_feedback_io_process.cpp and tests/PBApplication/feedback_io_fixture.cpp. Modify apps/CMakeLists.txt and product dependency targets.

**Interfaces:** Produce FeedbackIoProcess::Start(FeedbackIoRole, const FeedbackConfig&, uint64_t runToken)、QueueSnapshot(const FeedbackSnapshot&, uint64_t)、Poll(uint64_t nowNanoseconds)、Stop() noexcept. Poll returns bounded status/optional snapshot with token; no callbacks into runtime.

- [x] Write failing tests for protocol handshake/length/token rejection, child absent/version mismatch, clean read/write exchange, latest-wins slot bounded at one, child early exit, blocking test child, Stop/Start late result and quoted Unicode/space paths.
- [x] RED: run PBFeedbackIoProcessTests against the test-only helper fixture.
- [x] Implement hidden CreateProcess with fixed sibling helper name, explicit inherited-handle list, Job Object kill-on-close/one-child constraint and metadata-only framing. Do not use QProcess in the core or shell-built commands.
- [x] Poll only ready pipe bytes; bound every read/message, keep one pending operation/latest write. Five-second operation timeout invalidates result and terminates helper; do not spawn replacement before old child is confirmed terminal. Stop uses finite waits and reports teardown trouble without blocking owner indefinitely.
- [x] Implement helper read/write loop using Task 3, fixed allowed operations and strict parent token; no payload or arbitrary command operation.
- [x] GREEN: build helper and PBFeedbackIoProcessTests; prove blocked fixture cannot hold parent shutdown or grow process/thread count. Disabled setup launches zero processes.
- [x] Keep fixture fault behavior in test-only executable, not product flags or environment selectors.

### Task 5: Decoder durable export and terminal feedback lifecycle

**Files:** Modify local_desktop_runtime.h/.cpp、application_model.h; create test_feedback_runtime.cpp and runtime test-access seam as needed, without production fault selectors.

**Interfaces:** Consume Tasks 1/3/4. Add optional FeedbackConfig to DecoderConfig, FeedbackTelemetry to DecoderSnapshot and owner-generated immutable snapshots from ReceiverPipeline. Tests access the same production pipeline, not a fake completion counter.

- [x] Write failing tests: no bit before durable stored commit, completion only after all actual final flags, feedback I/O failure preserves file result, old resumed segments rehashed before export, retained completed journal reopens/validates final.
- [x] Add terminal lifecycle tests: B completion then A restart obtains new visual witness; B restart revalidates retained journal/final; corrupt/missing final never produces terminal; Stop/close freezes completion and invalidates old generation.
- [x] RED: run PBFeedbackRuntimeTests filtered to Decoder cases.
- [x] Export at actual storedSegments update and successful Publish/CompleteRecoveredPublish. Include canonical record hashes, same-generation bitmap/flags and a frame admitted for the selected Profile/Session, not cadence-only counters.
- [x] Skip RemoveAfterPublish only when feedback is enabled; preserve exact original cleanup otherwise. On recovered publish set completed metadata from the validated final chain, not stale progress estimates.
- [x] Keep live capture in explicit post-completion listening, process only valid identity/observation feedback and no Outer-FEC data, preserve published file and frozen timing. Stop/new Start/close safely ends listening; Replay/diagnostic behavior unchanged and not auto-enabled.
- [x] GREEN: PBFeedbackRuntimeTests Decoder cases plus resume/session/storage regressions and disabled cleanup equivalence.

### Task 6: Four-mode sender feedback and safe stop

**Files:** Modify SenderFrameBuilder and live Encoder worker in local_desktop_runtime.cpp, EncoderConfig/Snapshot; extend test_feedback_runtime.cpp and existing scheduler workflow tests.

**Interfaces:** Consume Tasks 1/2/4/5. Add next-boundary feedback selection to SenderFrameBuilder, retaining original branch when absent. Encoder owner records actual successful Submit sequence before accepting candidate terminal, using its exact Start token.

- [x] Write failing true FEC/Receiver/storage tests for all four profiles with 0/1/6/12/13 segments, mixed RAW/compression/DirectRepeat, early/tail holes, delayed/dropped/expired feedback and independent restarts.
- [x] Assert acknowledged segment service decreases, unconfirmed segments still recover, active encoded set <=6, no duplicate repair identity and same-frame retries frozen. Test spatial bank and small-file barrier separately.
- [x] RED: run sender cases, including stale token attempted stop after new Start and completed-but-no-final receipt continuing legal visuals.
- [x] Implement opt-in selection/early retirement at committed atomic frame/bank boundary, update exact committed-service ledger and retire unused reserved IDs without reuse. Preserve all Control refresh and immutable bytes/descriptor behavior.
- [x] Apply fresh confirmations before prior weights; restore original configured no-feedback/optional-prior service on expiry without creating new Session or clearing high-water.
- [x] Accept terminal only in current worker with full identity/receipt/actual-Submit membership; set typed ReceiverVerified stop reason and use original safe shutdown, never asynchronous generic RequestStop from file thread.
- [x] GREEN: PBFeedbackRuntimeTests + PBUnifiedSenderSchedulerTests + relevant existing PAM4/Wide/Gray/standard workflows/Golden. Compare disabled traces to original branch, not merely final file equality.

### Task 7: Main/Shell/Decoder settings, status and timing

**Files:** Create feedback_options_qt.h/.cpp and test_feedback_options.cpp; modify Encoder main/Shell/controllers/CMake and Decoder GUI/controller/CMake.

**Interfaces:** Qt FeedbackOptions::Load/Save/SetEditable/Apply for optional FeedbackConfig; shared Encoder use in both entry points, role-specific copy. No Qt dependency added to PBApplication.

- [x] Write failing offscreen tests for default disabled, valid folder/file, UNC pasted text, damaged persisted setting, empty enabled path refusal, disabled invalid path ignored, shared main/Shell persistence and all four modes.
- [x] Add lifecycle/layout tests for minimum/resized/DPI windows, running locks, “已完成，反馈监听中”, explicit stop listener, new Start joining old run safely, open verified file remaining available and frozen elapsed time.
- [x] RED: PBFeedbackOptionsTests and targeted GUI smoke.
- [x] Implement small explicit UI region, role/read-only help, path-kind selection/browse and visible requested/effective/error states. Reuse existing scrolling/resizable patterns.
- [x] Update controllers/button state so completed-listening is busy for Start/close ownership but retains verified completion semantics; do not overwrite completion with “stopped before publish”.
- [x] GREEN: offscreen tests and both normal/Shell GUI startup checks with isolated preferences, without registering or changing unrelated real user settings.

### Task 8: Reporting and current bilingual contracts

**Files:** Modify run_report.h/.cpp, operational status text where needed; create test_feedback_report.cpp; update architecture, guides, status, index and relevant scheduler/recovery documents in Chinese/English.

**Interfaces:** Consume FeedbackTelemetry. Reports add a dedicated feedback object with explicit reported values; old unavailable receiver truth fields are not silently redefined.

- [x] Write failing report tests for requested vs effective, waiting/stale/conflict, disabled null/false semantics, no path/payload exposure, terminal published vs accepted, safe stop reason and separate listener/file-completion timing.
- [x] RED then implement bounded counters/serialization and run PBFeedbackReportTests, existing report/decoder-text/prior regressions.
- [x] Explain optional control-plane exception, setup, stale fallback, trusted single writer, retained .resume, completion listening and deferred field validation. Mark earlier research as followed by implementation without rewriting historical results.
- [x] Check bilingual/local links, exact task diff, protected hashes and privacy of additions. No actual logs or local build paths in maintained source/docs.

### Task 9: Integrated verification and independent review

**Files:** Tests and implementation from Tasks 1–8; local run logs and scope manifest under this task's artifacts.

- [ ] Build both applications/helper and all new targets in the isolated build tree; record compile/toolchain/source identity and failures before repairs.
- [ ] Run all new targets, relevant protocol/Outer/Inner-FEC/storage/resume/scheduler/Golden/GUI regression targets; failures in changed behavior block delivery.
- [ ] Run complete non-interactive CTest, retaining original Gray Fast late-join failure and missing-real-Replay skip if still present; do not run opt-in real-display gates or real mappings.
- [ ] Review actual diffs for overflow/resource/lifetime, helper/path races, epoch/Start ordering, disabled equivalence and full four-mode scope; obtain fresh whole-change review using the approved execution method.
- [ ] Repair confirmed new issues with regression tests, repeat affected/full verification as necessary. Unknown Critical/High or unexplained new failures remain incomplete.
- [ ] Recheck the 1,303-file baseline, task-only changes and protected reports. Preserve unknown concurrent drift; do not reset to make verification clean.

### Task 10: Separate reproducible candidate packages and final audit

**Files:** Candidate build/verify scripts and manifests under artifacts/feedback-assisted-packages-20261021-run01; maintained usage/candidate document when actual paths are known.

**Interfaces:** Consume verified Encoder/Decoder/helper outputs and sealed existing dependencies/licensing/source bundle. Read pixelbridge-encoder-shell-release before production of packages.

- [ ] Inspect current prior candidate producer/verifier and existing portable-package gates; reuse dirty-candidate separation, never weaken ProductRelease clean-commit verification.
- [ ] Produce new Encoder/Decoder ZIPs with each endpoint's own helper/dependencies, dependency/standard-file hashes, matching source content identity, licenses and current usage instructions. No personal settings, .part/.resume, logs or raw environment snapshots.
- [ ] Independently extract each ZIP into a fresh directory, verify content/hash/dependency closure, helper protocol/build identity and isolated GUI startup under restricted PATH; verify ZIP hashes unchanged afterward.
- [ ] Add useful negative package checks: missing helper, tampered helper/app/manifest and missing dependency must be rejected. Candidate paths never overwrite dist.
- [ ] Final requirement audit maps every spec item to current source/test/package evidence and explicitly lists field/mapping/performance NOT_RUN. Report known historical failures separately.
- [ ] Only when all requested implementation and both independent package deliverables are verified, mark the goal complete and provide paths, SHA-256, minimal setup/stop/recovery instructions and remaining real-machine limits.

## Self-review and execution handoff

- Spec sections 1–12 map to Tasks 1–10; all four modes, post-completion restart/listening, disabled behavior and final packaging have explicit tests.
- Five review-focus input classes are assigned to owning test tasks.
- Shared types/signatures are defined in their owning tasks; no task invents a parallel feedback parser or completion authority.
- No Git commit step is authorized; save local evidence/diffs at each task boundary.
- 推荐 Native：由主 Agent 在当前会话逐项实现，最后进行独立全量审阅；涉及 runtime/GUI/生命周期的接口耦合较强，便于统一维护当前 dirty 基线。若用户选择分工执行，再使用对应工作流，不默默派生实现者。
- 用户审阅计划并选择执行方法前，不开始产品编码。
