# P0-15 Phase 0 Final Gate：范围、修复与封版规则

## 1. Final Verdict

**本文件随源码提交，记录验收定义、修复、审查与复跑方式；不是提交后结果的替代物。**
采用用户选定的“单次提交＋外部封版”：精确 `FINAL_PHASE0_COMMIT`、clean worktree、
Release/ASan 实测结果、最终报告和 `gate-decision.json` 写入
`build-phase0-gate-evidence/<完整 commit SHA>-<timestamp>/`，不把自身 commit SHA
回写本文件，也不把旧工作树日志改称 clean-commit 验收。

最终判定必须独立报告：

```text
Phase0Acceptance = PASS / FAIL
FinalSystemComplete = NO
OperationalTagBlockers = [...]
```

Phase 0 验证 **wire、Segment/FEC、资源边界、basic resume 和 CPU/reference 文件往返**。
后续产品未完成不能自动否决 Phase 0；反之，CPU/reference PASS 也不是完整产品 PASS。
最终封版必须包含 Final Commit、Worktree、in-scope Critical/High、TagCreated、TagTarget。

## 2. Scope Reconciliation

每个大项拆分 Phase 0 子集和未来子集，不将整个大项一并标为 blocker 或 deferred。

| Requirement | Phase0RequiredSubset | CurrentStatus | Classification | Reason | FutureOwner/Phase |
| --- | --- | --- | --- | --- | --- |
| Descriptor/profile binding | reference ID/layout、精确 N/K/矩阵/digest/位序/几何/packing、无 interleave、未知参数拒绝 | 固定组合与独立接收已实现，新增反例；须 clean-commit 复跑 | Phase0 Required | 接收解释必须唯一，不能依赖发送端 block 数或长度 | Protocol/reference Gate，Phase 0 |
| Descriptor/profile binding | — | 尚未冻结最终 Certified physical profiles | Deferred | 不把 provisional descriptor 偷换成完整 v1 schema | Physical profile/Capture，Phase 1+ |
| Physical Bootstrap/Control | logical bytes/CRC、fragment/reassembly/conflict/资源、确定性 reserved lanes | 原实现保留；Gate 固定完整 PB-Control-1 窗口，自描述长度且尾部为零 | Phase0 Required | 不发明 discriminator；独立 fragment 测试继续运行 | Protocol/reference Gate，Phase 0 |
| Physical Bootstrap/Control | — | 无 production physical FEC、同步、repetition cadence | Deferred | CPU reference 不证明真实显示捕获的抗噪声/抗撕裂 | Present/Capture，Phase 1+ |
| PBStorage | Verify → 精确写入/flush/close → Commit；权威 manifest、顺序 hash、final 字节一致、已有文件不覆盖 | 两阶段 Receiver 与真实 `.part` Gate；发布负例到达实际调用 | Phase0 Required | output acceptance 不能止于 outer Ready 或内存摘要 | Receiver/reference storage Gate，Phase 0 |
| PBStorage | — | 无生产 durable flush、事务替换、掉电顺序和空间 UX | Deferred | 证明进程内顺序与文件结果，不证明掉电耐久性 | PBStorage，Phase 1+ |
| Live resume | save/destroy/load/replay、completed metadata、两种 active FEC cache、资源与 raw 完整性 | RAW 保留；新增 Zstd completed 从 `.part` 恢复，不重传/重压缩 | Phase0 Required | encoded bytes 不是已完成 raw 文件区间 | Receiver/basic resume，Phase 0 |
| Live resume | — | 无实时 durable snapshot scheduling、ResumeDegraded、kill/power-loss 矩阵 | Deferred | PBRS 基础恢复不等于生产 crash-consistency | Receiver/storage persistence，Phase 1+ |

不引入 Capture、GPU、MP4、Carousel 调度、Certified Profile 或新 wire 语义。
仅测试调度使用确定性乱序/丢包/修复，不据此声称生产 Carousel 完成。

## 3. Changes Made

### 3.1 默认关闭、可重复执行的 Gate

`PB_BUILD_PHASE0_GATE` 默认 OFF；启用必须有 `BUILD_TESTING=ON` 和 `PB_BUILD_TESTS=ON`。
`PBPhase0Gate` 使用现有库，CTest 标签为 `phase0-gate`、`phase0-resume`、`phase0-large`。
100 MiB 仅 Release；成功清理自己创建的 scratch，失败保留；拒绝复用已有 case 目录。

`tests/Phase0Gate/reference_frame_receiver.{h,cpp}`：接收仅根据 raster + 固定 binding
决定 Bootstrap、Control 和 Data 解析。契约见 `REFERENCE_RASTER.md` §13。
`reference_frame_tests.cpp`：独立 literal/PBVM/矩阵 pin、0/1/27 codeword、17 种错误
binding、CRC-valid profile/layout、Control 长度/尾部、Data 零窗口边界、重编码后的
Transport CRC/padding、packing，另有 128 次固定 seed mutation。

### 3.2 Receiver 完成语义与压缩段 resume

`libs/PBReceiver/include/pbreceiver/receiver_ingress.h`：

- `VerifyRecoveredSegment`：当前权威 binding、encoded size/digest、有界解压、RawDigest；
  返回私有构造、move-only、只读 raw bytes 的 `ReceiverVerifiedSegment`。
- `VerifyResumedStoredSegment`：完整 SessionId、ordinal、bound descriptor、offset/size/RawDigest、
  policy 与 terminal 检查，再核对实际 raw 长度和 digest。不重压缩，不请求原 encoded bytes。
  调用方先比对 metadata，然后只按权威 descriptor 有界读取 `.part`。
- `CommitStoredSegment`：实际精确写入并 flush/close 后提交；重复幂等。
- `PrepareFinalization`：必须有权威 FinalManifest、完整 Segment Map、全部存储提交。
- Ready decoder 的 accepted-ID 指纹和 reservation 保留到 commit/terminal/reset 清理。

两种 capability 来源不混称：新恢复走 encoded/decompress/raw；completed resume 走 bound
metadata/raw；最终均要求实际 `.part` WholeFileDigest。capability 证明字节，**不是磁盘写入
证明**；single-owner storage 调用者必须履约。仅 append 本地 `SegmentRecoveryIncomplete=52`，
保留既有枚举、wire/PBRS/Golden 布局。

### 3.3 OOM、instrumentation、evidence

非安装 `libs/PBOuterFec/src/decoder_test_access.h` 通过私有 factory 参数，在 reservation
成功后、真实分配前进入生产 catch 路径抛 `std::bad_alloc`。无全局开关，不再实际分配
约 `0xffff00000027` bytes；保留极值算术、quota−1/exact quota、错误映射和 RAII 回滚。

`fuzz/CMakeLists.txt` 将 PBReceiver 纳入 ASan；raster driver 增加统一 completion marker。
`InvokeMutationGate.ps1` 保留 11-driver/830,000-iteration 固定 seed 预算。
`InvokeReferenceReplay.ps1` 两轮独立空 scratch，只排除 `elapsed_ms`。
`InvokeFinalGate.ps1` 只接受精确 clean HEAD，新建 commit 专属 Release/ASan 构建树，
保存命令/退出码、JUnit、inventory、JSONL、依赖、source/binary/config hashes，阶段前后核对身份。
驱动不自行 commit/tag；最终静态诊断由封版报告分类，非零 cppcheck 不冒称零告警。

## 4. H01/H02/H03 Revalidation 与审查 ledger

### 4.1 Findings（Critical / High / Medium / Low）

先理解机制，再修改；保留旧失败证据。下列 fixed 不能替代 clean-commit 最终复跑。

| ID / severity | File / function | 机制、触发与旧覆盖缺口 | 修复与回归 |
| --- | --- | --- | --- |
| H01 / High / fixed | `PBReceiver/src/receiver_ingress.cpp`，outer recovery/completion | Ready 提前 MarkSegmentCompleted；解压/raw/write/flush 失败仍被当成完成；旧局部链未走到发布 | Verify/Store/Commit；Ready/Verify 不能 final；错误后可重收；0-byte 合法 final |
| H02 / High / fixed | `tests/PBOuterFec/test_direct_repeat.cpp`，allocation failure；`direct_repeat.cpp` 私有 factory | 巨额真实分配让 ASan allocation-size-too-big abort，未进预期 bad_alloc catch | 每调用注入 OOM；quota−1/exact quota/极值算术/reservation 回滚保留 |
| H03 / High / fixed | `PBReceiver/src/receiver_ingress.cpp`，DecodeBlock/ProcessBoundDataBlock/Commit | 第一版两阶段实现过早丢掉 Ready decoder，pending capability 的同 accepted-ID 冲突失去历史；手工 constructed recovered data 测试未覆盖 | 保留指纹/reservation；两种 FEC 均以重算 CRC/LDPC/raster 的冲突验证 commit/final terminal 拒绝 |
| H04 / High / fixed | Gate 旧 ProcessFrame | 接收使用发送方 Control 长度/block 数，Bootstrap 不拒绝未知 profile/layout；验收依赖 test side-channel | 独立 ReferenceFrameDecoder、固定 binding、像素自描述范围；独立 oracle、0/1/27、malformed/mutation |
| H05 / High / fixed | Gate RunResumeGate / Receiver 新 API | completed `.part` raw 被作为 encoded 输入，RAW 掩盖问题；Zstd 复现 EncodedSizeMismatch | 先存红证据；新增 metadata/raw 验证；8MiB Zstd completed + 8MiB Wirehair partial + DirectRepeat tail，首段不重传 |
| H06 / High / fixed | Gate RasterTransportPipeline::ReceiveFrame | 同帧 systematic recovery Ready 后再次出现相同 accepted block，Receiver 返回同一恢复结果，聚合层误报多个 Segment；既有跨帧重复未覆盖同帧 Ready 边界 | 仅对精确 binding 和 encoded bytes 相同的结果合并，冲突仍先由 Receiver 拒绝；1B DirectRepeat 与2629B Wirehair 均经真实 raster 验证 Ready 不提前提交、一次存储、提交后整帧重复幂等及最终文件一致 |
| M01 / Medium / fixed | `fuzz/CMakeLists.txt` | ASan 链接不等于 Receiver 本体插桩 | 生成 vcxproj `/fsanitize=address` 检查及全套 ASan |
| M02 / Medium / fixed | Gate RunPublicationNegativeChecks | 只测 publish predicate 不能证明实际发布调用不修改文件 | 用权威 manifest 调用 VerifyAndPublish；hash/长度/已有目标不变 |
| M03 / Medium / fixed | Gate CreateFreshCaseDirectory | 启动删除已有 scratch 会毁掉失败证据/无关文件 | 拒绝已有目录/链接；ScratchSafety sentinel |
| M04 / Medium / fixed | Gate MetricsJson / resume | 用 stored commits 代替验证来源，无法表达 restored raw 额外验证 | fresh encoded/raw、resumed raw、restored commits 分计数 |
| L01 / Low / fixed | `fuzz/reference_raster_fuzz.cpp` | 缺统一结束 marker，预算无法统一核对 | FUZZ_COMPLETED + exit/sanitizer 检查 |

未发现需要新产品架构才能修复的 in-scope Critical/High；最终仍须复核 runtime 和全部静态诊断。

### 4.2 八个独立视角（同一执行者，非虚构八名评审人）

| 视角 | 审查路径/不变量 | 对抗或独立证据 | 剩余边界 |
| --- | --- | --- | --- |
| 文件流式/算术 | DescribeFile/ReadFileRange/checked offsets；两遍、tail/RawOffset、source 重读 digest | 0/1/1313/1314/1315/2628/2629、8MiB±1、100MiB13段；尺寸/digest 变化拒绝 | 不证明生产实时文件锁策略 |
| digest/压缩 | CompressSegment、bounded decode、VerifyRecovered/VerifyResumed；encoded/raw 分离 | RAW random、实际 Zstd、坏 encoded/raw/解压、input/output/window quota | in-band digest 不认证发送者 |
| Outer FEC | canonical Recreate、DirectRepeat tail、Ready 指纹、extra/error/RAII | systematic/repair/repair-only、乱序/重复/丢包、profile/payload 冲突、ExtraInsufficient、OOM/quota | 无无限 repair/decoder 假设 |
| Transport/LDPC/raster | PBVM/matrix/LSB 固定 binding；整帧预验证；CRC/zero padding | literal/pins、0/1/27、未知 profile/layout、重编码坏 CRC、零窗口后非零 | 不证明物理抗噪同步；无 interleave |
| Receiver commit | terminal-before-verify、权威 binding、move/只读 capability、唯一完成点 | H01/H03/H06、pending conflict、0-byte、失败不完成、同帧/跨帧 Ready 重复幂等 | capability 非磁盘 receipt |
| 文件发布 | 权威 manifest、顺序 hash、final 不存在、同目录 rename、byte compare | 缺段/未提交、错 whole digest/长度、已有 final；实际调用后文件不变 | 掉电/并发发布 race 属生产 storage |
| quota/resume | admission-before-allocation、stat-first、metadata-before-read、全 SessionId | checksum/length/profile/count/padding/conflict/quota；RAW/Zstd completed + 两种 cache | 允许有效前缀＋hasTruncatedTail；不接纳未完成 record |
| Golden/fuzz/证据 | pins 不变、结束 marker、fresh build/scratch、HEAD/二进制身份 | Golden/corpus/structured/parser、830000+128 mutation、VectorGen byte compare、双轮 JSONL | MSVC mutation 非 libFuzzer/UBSan；cppcheck 局限分类 |

## 5. End-to-End Evidence

```text
File → 8MiB Segment → streaming RawDigest/WholeFileDigest
 → CompressSegment (Zstd/RAW) → DirectRepeat/Wirehair V2 canonical Recreate
 → Transport (32-byte header, payload<=1314, header/payload CRC, canonical padding)
 → Robust LDPC (1350-byte info / 2025-byte codeword)
 → 最多27 codeword + zero tail → reference raster encode
 → independent demod / fixed binding / Control RecordBytes / Data zero-window scan
 → syndrome / hard LLR / LDPC decode → Transport padding / CRC
 → ReceiverIngress / outer recovery → encoded digest → bounded decompress → raw digest
 → RawOffset write output.part / flush / close → CommitStoredSegment
 → authoritative FinalManifest → final sequential WholeFileDigest
 → 同目录 rename → final sequential digest + source/final byte equality
```

12 个 fast 文件用例＋100MiB large，断言真实 codec/FEC。相同 block/frame 重复，Segment 与
block 双层逆序，丢 systematic 0 后 DirectRepeat 下一轮或 Wirehair 新 repair 恢复。
1MiB random 必须 RAW/Wirehair；1MiB compressible 为实际 Zstd（预检792B）/DirectRepeat。
另有两个同帧 Ready 重复回归：将 byte-identical accepted block 放在最后一个必要 systematic
block 后，不依赖帧间调用才能完成存储；精确合并后仍保留 reservation，直到 Verify/Store/Commit。

两个 resume 用例均为16,779,844B：先存1个 completed 和1个 Wirehair/1个 DirectRepeat validated
entry，销毁 Receiver/decoder，加载 PBRS/control，重读 `.part` completed raw；其余数据继续经
reference raster，最终顺序 hash/发布。混合用例首段 Zstd 8,388,608B→281B（预检），恢复时不
重新解压、不重传。新恢复与已存储恢复的验证来源在 JSONL 分开计数。

红证据：`build-phase0-gate-evidence/precommit-p015-20260827-133943/resume-red.*`，
工作树快照 `resume-red-tracked.patch`，失败现场 `resume-red-scratch` 保留。
旧24文件任务快照及 `561d298-20260827-104127` 证据继续保留，仅作 baseline。

H06 红证据：首次 clean commit `39f946de92bd369797cd736b27715fa1acf6f410` 的
`build-phase0-gate-evidence/39f946de92bd369797cd736b27715fa1acf6f410-20260827-142303/late-review-ready-duplicate-probe/`。
独立可构建探针对1B/2629B 均得到 `one frame produced multiple recovered Segments`；
该 SHA 的必跑 Release/ASan/Golden/mutation 当时已通过，但晚期审查证明覆盖缺口，因此
主动停止未完成的双轮重放并记录 FAIL，不能继承其 PASS。保留全部旧日志和探针；本修复
采用非 amend 追加提交，新 clean SHA 必须重新完整执行 Final Gate。

## 6. Build/Test Evidence

提交前局部验证仅作 preflight：Receiver completion/resume 最终聚焦回归7 cases/731 assertions，
H02 为3 cases/46 assertions；Release/ASan 完整 default build 后分别45/45、156/156，
包含独立 frame 与压缩 completed resume；最后补充的旧路径精确错误类型断言另行重跑 Receiver。
最终必须重新执行，不继承预检结论。

```powershell
# 原子提交后的 clean HEAD；工具已存在，不安装 LLVM。
$finalCommit = (git rev-parse HEAD).Trim()
& .\tests\Phase0Gate\InvokeFinalGate.ps1 -ExpectedCommit $finalCommit `
    -VcpkgRoot D:\vcpkg -QtRoot D:\Qt6.10.1\6.10.1\msvc2022_64 `
    -CppcheckExecutable <installed-cppcheck.exe> -Parallel 4
```

驱动每次新建独立证据/构建树，不复用旧日志；default target 在 CTest 前。执行全部 applicable
CTest、Golden30、corpus/structured/parser、H01/H02/H03、11-driver830000 mutation、VectorGen
同 seed 两轮逐文件 bytes、Release fast/resume/large 两轮、ASan fast/resume 两轮、跨配置比较、
cppcheck；均绑定同一 clean commit。静态诊断仍须人工分类，驱动成功不自动授权 tag。

旧 `45/156/30/98/3/86` 是 CTest/registry 数量基线，不能无解释缩减。新增测试进入已有
PBReceiverTests 和 PBPhase0GateFast/Resume，未必增加 CTest 条目；resume JSONL 增加第二个
文件，fast 增加独立 boundary 记录；新增字段说明验证来源。最终 inventory 差异必须解释。
H06 在原 fast 入口内增加两个明确记录，不减少其他文件、反例或 mutation 预算，也不增加
CTest registry 项；新增的 transient recovered buffer 计入 tracked working bytes（不是 RSS）。

## 7. 100 MiB Metrics

要求104,857,600B、13段（12×8MiB＋4MiB）、全 RAW/Wirehair、sender/receiver 各最多1 active。
第一遍只保留 descriptors/manifest，第二遍确定性乱序按 canonical profile 重建 encoder，
不得创建100MiB payload vector。

JSONL 记录 frames、codewords、Transport blocks、duplicates、duplicate frames、drop、repair、
stored commits、active Segment/decoder、decoder reservation、major buffers、whole digest、
final byte equality。`max_tracked_working_bytes` **不是 process RSS**，不含所有第三方私有
workspace/allocator bookkeeping；reservation 单列，不简单相加宣称 RSS。新独立 decoder 返回
帧缓冲纳入计数，比旧计数增加36,560B；不是文件级内存增长。

不填旧 run 数值冒充最终值；最终 SHA 的 `release-replay/round-*-large.jsonl` 为权威。
100MiB 在 ASan 下明确为 **Not run under ASan by policy**。

## 8. Deferred Requirements

Certified physical profiles、Signal/Pilot calibration、实际链路调优；物理 Control FEC/
repetition/synchronization；生产 PBStorage durable flush/atomic replacement/crash ordering/
free-space UX；live resume snapshot、ResumeDegraded、掉电矩阵；Windows Capture/Present、
GPU、MP4 等 Phase1+ 产品工作未实现，不掩盖，也不作为本任务自动 blocker。

## 9. Limitations

- CPU/reference 像素组合不证明真实 Capture、显示吞吐、BER/FER 或物理认证。
- MSVC ASan + 固定 seed deterministic mutation；Clang/libFuzzer/UBSan 不可用时明确
  NOT RUN，不安装 LLVM，也不单独据此否决 Phase0。
- `/W4 /WX` 为 owned source 编译策略；ASan 可能出现既有 LNK4075：`/INCREMENTAL`
  被 `/INFERASANLIBS` 忽略，不伪称整个构建零信息/零 warning。
- prebuilt vcpkg 依赖不全部带 ASan；不扩大 sanitizer 覆盖声明。
- cppcheck 使用 `--language=c++`/实际 vcxproj；Catch2 SECTION/moved-from 负例和 C++20
  parser limitation 逐项分类，不删除断言或压低 policy 让工具通过。
- deterministic fixture SessionId 为可重放测试值；生产会话仍必须 OS CSPRNG。

## 10. Git Evidence

基线 `561d298796ccf2fe35ac2d379f8d111ef76004c8`，当前分支 `master`。逐路径审计和暂存任务
文件，不纳入 ignored build/evidence 或无关修改。预检后非 amend 原子提交：

```text
P0-15: complete Phase 0 end-to-end reference gate
```

提交后 `git status --short` 必须为空。driver 阶段前后检查 HEAD/worktree/config/binary，保存
hashes/inventory/raw logs；封版 index 关联全部证据到同一 SHA。若必须再改源码/测试/config，
追加非 amend 提交并在新 clean HEAD 完整复验，不继承前 SHA 的 PASS；无关改动导致不能 clean
时登记 OperationalTagBlocker，不覆盖、不 stash。

## 11. Tag Decision

仅在 Phase0Acceptance=PASS、必跑项全过、无 in-scope Critical/High、无操作性 blocker、
worktree clean、证据/HEAD/target 同 SHA 时，创建本地 **annotated** `phase0-gate-pass`。
消息说明 CPU/reference、Protocol/Segment/FEC/reference visual、Golden、WholeFileDigest 通过，
Phase1+ physical/product 仍 deferred。验证对象类型 `tag` 和 peeled target；同名 tag 存在则
不覆盖、不强制移动。

驱动不提前宣布 tag 许可；封版 `gate-decision.json` 和最终11节报告给出实际结果。
禁止 push、创建 branch、amend、rebase、`reset --hard`、改 Golden pins。
