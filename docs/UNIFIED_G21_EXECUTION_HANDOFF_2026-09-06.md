# G21 执行交接（2026-09-06）

> 本文是关闭当前 Codex 对话前的事实交接，不替代
> [`UNIFIED_VISUAL_LARGE_FILE_IMPLEMENTATION_ROADMAP.md`](UNIFIED_VISUAL_LARGE_FILE_IMPLEMENTATION_ROADMAP.md) 的范围和退出标准，
> 也不把尚未通过的门禁写成已通过。后续任务必须先读仓库根 `AGENTS.md`、路线 G21、
> [`UNIFIED_REMOTE_GATE.md`](UNIFIED_REMOTE_GATE.md) 第 15～18 节，再使用本文定位证据。

## 1. 交接时结论

- G00..G20 已有独立提交并按路线记录完成；当前目标仍是 **G21**，G22 未开始。
- 当前唯一产品视觉合同是 `PB-Unified-SC6-V3`、VisualProfileId `0x5042554E49534333`、layout 10、
  1920×1080 canonical canvas、6×6 separated symbols。LC4、SC6 V2 与旧 LF4 仅为历史回归。
- 当前代码修复基线为 `433bccf42df29c54ae326f0e01296ab22a8258ef`
  （`fix(resume): retry transient decoder journal replacement`）。后续任务须以 `git rev-parse HEAD` 核对本文之后的
  交接文档提交，不得仅相信本文中的旧 HEAD。
- 真实未知远控链上的最终 64 MiB CSPRNG/RAW 已完成 **8/8 Segment、WholeFileDigest、安全 rename、final reopen、
  外部 SHA-256/BLAKE3 双摘要**；Decoder 没有拿到 source 或摘要 oracle。
- 该次 `PBUnifiedRemoteGate` 仍 exit 1：不是文件恢复或发布失败，而是
  `ReceiverChecksPassed()` 要求 `outerResourceRejections == 0`，现场启动阶段累计了 416 次后稳定不再增长。
  因而留下 `failure.txt`，G21 不能直接写成完成。
- `VerifiedEncodedBytesPerUniqueFrame = 17,796.039246884116`，已高于 16 KiB/unique 硬门；没有达到 32 KiB 工程目标。
  路线 G21 明确允许未达 32 KiB 的首版，但必须记录，不能将工程目标误说成 exit 1 的原因。
- Base Luma 独立恢复证据仍缺；独立 false-accepted codeword oracle 仍为 unavailable。当前最终文件字节正确、
  lane CRC/identity failure 与 Outer conflict 均为 0，但这些不能冒充独立 codeword oracle。

## 2. Git 与工作树安全状态

交接前最后核对：

```text
HEAD before handoff-doc commit: 433bccf42df29c54ae326f0e01296ab22a8258ef
git status --short: ?? docs/PHASE1_GATE_REPORT.md
docs/PHASE1_GATE_REPORT.md SHA-256:
076EF4C9B9F89EABCCD323DBE4BFFC4DC125DDAF96E6EE437D2CF5B1B1CEA306
```

`docs/PHASE1_GATE_REPORT.md` 是用户所有的未跟踪文件：不得覆盖、删除、暂存或顺手纳入提交。继续使用显式路径
`git add -- path...`；不得 broad-stage、amend、rebase、reset 或 push。

## 3. 最终真实远程 64 MiB 证据

### 3.1 同身份交付

完整 Encoder ZIP：

```text
C:\Users/<user>\Desktop\PixelBridge-G21-RemoteEncoder-433bccf-SC6V3-Final64MiB-Full.zip
ZIP SHA-256: 555068278780e4a9ad57abe6e22634541fbd0225400ed28262685f5f9cdec67e
Encoder SHA-256: 053b13ba545f315635117802e868fe9a8483eedd78e64723f0d23ce9783a06d1
Encoder/Decoder commit: 433bccf42df29c54ae326f0e01296ab22a8258ef
```

远端 `00_Check.bat` 回报通过；检查没有生成 source、run 目录、窗口或传输。用户随后只运行
`02_Start_Full_64MiB_15Hz.bat`。本机 Receiver 先启动并只捕捉 `\\.\DISPLAY2`。

### 3.2 权威运行根与配对结果

```text
<repo>\build-unified-release\g21-sc6-v3-live-final-64mib-1\
  full-64mib-15hz-a4ae1abe75b942e9be3517ddb73e70c8\
```

关键文件：

- `receiver/final.json`：权威终态，`state=Completed`；
- `receiver/failure-final.json`：Gate 后置检查抛出后再次封存的相同 Completed snapshot；
- `receiver/failure.txt`：仅记录 Gate 对 resource rejection 的后置失败；
- `remote-source-manifest.json`、`remote-encoder-report.json`、`remote-process-exit.json`：用户回传原件的只读封存副本；
- `paired-publication-audit.json`：严格身份、发布和外部摘要配对，SHA-256
  `9c0de74228ec5c39f5343a3136482e2ba9e1effa69d138ace7398b9c7d714810`。

严格配对身份：

```text
Sender RunId:   42618932829c435e8f1bb3ba98989dc3
Receiver RunId: 7f184af71b73f1f1f277fe66ff92ed00
SessionId:      cc55f8afc83400af586a792bcac02665
SessionTag:     8060370733226332123
source:         g21-sc6-v3-acceptance-full-64mib-15hz-42618932829c435e8f1bb3ba98989dc3.bin
bytes:          67,108,864
SHA-256:        4dd1b87cbaf6872d07a4fee05297468224661748dffcf5a72de791976ab39708
BLAKE3:         deb2bf110d067de24e457770089bc0ece89546cb6736e9639eee1f529d1ad494
```

Sender 提交 10,196 logical frames，实际平均 14.983399 Hz，用户在 Receiver 已停止后正常按键结束，exit 0。
Receiver 在约 503.6 秒后结束，结果为：

| 项目 | 结果 |
| --- | ---: |
| Verified Segment / raw bytes | 8 / 67,108,864 |
| Observations / unique logical frames | 3,840 / 3,771 |
| UniqueVisualFPS | 8.9218175573 |
| VerifiedEncodedBytesPerUniqueFrame | 17,796.039246884116 |
| Accepted Transport | 54,148 |
| Base/Fine/Chroma FEC failures | 0 / 0 / 18 |
| lane CRC / identity failures | 全部 0 / 全部 0 |
| Outer conflict / deferred busy / FEC quota | 0 / 0 / 0 |
| Capture admission drops | 47 |
| Outer resource rejections | 416 |
| Active decoder peak / reserved bytes peak | 8 / 457,201,696 |
| Whole digest / rename / final reopen / published | true / true / true / true |
| 残留 `.part` / `.resume` | 0 / 0 |

外部读取最终发布文件重新计算 SHA-256 与 BLAKE3，分别逐字节匹配 sender manifest 和 sender/receiver whole digest。
这关闭了先前 Decoder `.resume` 在第 6 个 Segment 后 `MoveFileExW(...)=win32 5` 的决定性故障：修正版成功越过
第 6、7、8 个 Segment，并清理 journal。

## 4. 为什么发布成功仍 exit 1

`tests/UnifiedRemoteGate/remote_decoder_gate.cpp::ReceiverChecksPassed()` 当前要求：Completed、whole digest、publish、
final reopen、无 resume/recovered-after-publish、`outerResourceRejections == 0`、无 conflict/deferred/quota、无 error。
本轮唯一不满足的字段是 `outerResourceRejections=416`。

`receiver/samples.jsonl` 的转换点提供了重要定位证据：

| outerResourceRejections | active decoders | unique frames | accepted Transport | verified Segment |
| ---: | ---: | ---: | ---: | ---: |
| 0 | 0 | 0 | 0 | 0 |
| 86 | 6 | 51 | 722 | 0 |
| 281 | 7 | 108 | 1,521 | 0 |
| 386 | 7 | 160 | 2,301 | 0 |
| 416 | 8 | 212 | 3,029 | 0 |

当第 8 个 decoder 建立后，计数在余下约 3,559 个 unique frames、全部 8 Segment 恢复和发布期间保持 416。
这强烈指向“Control/SegmentDescriptor 尚未把 W=8 全部绑定时，其他 Segment 的 Transport 挤满有界 orphan cache”，
而不是持续内存不足；但现有聚合字段没有保留每次错误码/Segment，因此仍需用定向 fixture 或细分 telemetry 证实，
不能直接删掉 `outerResourceRejections == 0` 来制造绿灯。

建议最短证明路径：

1. 用 headless/no-raster fixture 重放 W=8 的真实 Control/Transport 顺序，复现 active 6→7→8 时 orphan quota drop；
2. 将 protocol `ResourceLimitExceeded`、`ResourceExhausted`、Control quota、Outer FEC quota 分开计数，避免聚合字段抹掉根因；
3. 比较两个最小方案：第一批 Transport 前提供覆盖当前 W=8 的 descriptor prelude，或使每个 Control-bearing frame 都包含
   当前窗口的完整 descriptor 集；只有证据证明无法通过调度消除时才评估 orphan quota；
4. 保持 bounded memory、ActiveSegmentWindow=8、单 owner、250 ms freshness、质量/FEC/CRC/digest/publish 门不变；
5. 保留“未知 Segment 不得触发大 allocation/codec”的硬边界。

## 5. 已经踩过的坑与可复用经验

1. **构建身份必须来自实际 EXE。** 每次提交后须 reconfigure/rebuild，再运行 `--build-identity`；package HEAD 与陈旧二进制
   commit 不一致必须 fail closed。
2. **安装包必须完整、自包含、在全新解压目录复验。** 曾因只给增量包、缺 `expected-build.json`，以及用户删除整个旧目录而失败。
   不得依赖“原 remote-encoder 目录”。
3. **纯灰不是 payload。** 旧带边框窗口/DPI/任务栏使 client area 低于最小尺度后，Encoder 按合同显示 neutral matte。
   受约束的单显示器无边框全屏修复后才获得有效画面。
4. **远控失真是核心产品条件。** LC4 与 SC6 V2 均在缩放、低通、4:2:0、量化和局部旧帧混合下暴露结构性问题；
   当前 SC6 V3 使用 region-local placement、codeword-local permutation、6×6 separated symbols。不要退回只在无损本机好看的设计。
5. **configured/Present/capture callback FPS 不是 unique FPS。** 最终只用 SessionTag/FrameSequence 去重后的
   `UniqueVisualFPS`；本轮 sender 14.98 Hz、receiver unique 8.92 Hz 都必须同时记录。
6. **Control 抽帧别名会比 BER 更致命。** 按 record kind 分组的两帧 burst 曾让远控长期漏掉第二帧 SegmentDescriptor；
   后改为 Session/Manifest/Segment 交织，但 W=8 启动仍有 descriptor 覆盖窗口，正是 416 次拒绝的当前方向。
7. **后续 Carousel 不能重复 systematic IDs。** `5cedd15` 后 Pass N>0 使用 durable fresh repair IDs；不能用重复帧数冒充新方程。
8. **64 MiB 必须 W=8 条带调度。** 逐 Segment 长突发与 Receiver 活动 decoder 数不匹配会在期限内只完成部分 Segment。
9. **point sampling 的半像素量化不是裁剪。** `caba104` 只吸收严格小于半个 point sample 的边缘量化；恰好半像素、
   整像素裁剪和原质量门仍拒绝。不要简单扩大容差。
10. **Capture 必须在 acquisition 前背压。** Unified live 只允许 1 个全链在途 work，避免正确 GPU 结果因排队超过 250 ms；
    不要以增大时效门掩盖延迟。
11. **Windows 原子替换存在短暂 target-reader 竞争。** Encoder `runtime.state` 和 Decoder `.resume` 都只对同一个已 flush
    candidate 的 rename 做 25 ms、最多 11 次、250 ms 有界重试；不重写、不删 durable target、不 copy fallback、不递增 generation。
12. **成功发布和 Gate 进程 exit 必须分别解释。** 本轮 `final.json` 是 Completed，但后置 policy 产生 `failure.txt` 和 exit 1。
    不得只看退出码否认字节正确，也不得只看发布成功忽略尚未关闭的 resource/Base-only 门。
13. **source 不得进入 Decoder。** 外部摘要只在接收结束后由审计器读取 sender manifest 与最终文件；不要为方便本地测试把
    source path、digest 或 payload 通过 IPC 提供给 Decoder。
14. **证据必须 create-only。** 每次 source、run root、report、journal、外部 audit 使用新 GUID/目录；失败证据不得覆盖。

## 6. 后续 G21 工作清单

按依赖顺序推进，不得先做 G22：

1. 复现并解释 416 次启动期 resource rejection，优先修 Control/descriptor 调度根因，而不是放宽 Gate；
2. 为修正增加 parser/resource/conflict/bounded-memory 邻接测试和最小 Release 验证；
3. 建立 Base Luma 独立恢复证据。路线允许“同一实际 capture 的离线 chroma-neutralized 派生”；当前最终 remote run 没有保存
   可重放的完整像素序列，因此应在下一次本机实屏 run 中用专用测试构建有界保存实际 captured frames，再离线只中和 Chroma，
   严禁用 sender source 或重新渲染的理想 raster 冒充 actual capture；
4. 完成下述本机 15 Hz 大小阶梯；这些可验证稳定性和大文件流式行为，但不能冒充带远控因素的最终现场复验；
5. 更新 `UNIFIED_REMOTE_GATE.md`、路线 G21 状态，审查 diff，显式路径提交一次；
6. 只有 G21 exit 全部满足后才进入 G22。

## 7. 睡眠期间的本机实屏阶梯建议

用户明确允许新的目标任务在其睡眠期间使用两块本机屏幕、鼠标键盘和自动化；无需保护左屏，也无需等待人工点击。
这项临时许可仅适用于用户再次明确恢复正常交互之前。仍须避免隐藏 payload IPC，并保持证据可复现。

建议依次执行：

1. `64 MiB = 67,108,864 bytes`；
2. `500 MiB = 524,288,000 bytes`；
3. `1 GiB = 1,073,741,824 bytes`。

这里把用户口语 `500MB/1GB` 按项目一贯的二进制 MiB/GiB 记账；报告必须写出精确 bytes，不能只写缩写。

实现/运行边界：

- 不直接复用远程 ZIP 的 launcher：它固定 `primary` 且只生成 64 MiB。应先创建专用、测试范围内的本机阶梯 supervisor；
- 运行时先枚举并核对右屏仍为 `\\.\DISPLAY2`、2560×1440；Encoder 使用
  `--single-monitor-fullscreen \\.\DISPLAY2`，Receiver Gate 捕捉同一完整物理 monitor；
- 每轮先启动 Decoder、确认 run root/PID，再启动 Encoder；固定 Unified 15 Hz，source 为流式 OS CSPRNG、RAW/不可压缩，
  source lease 在 Session 全程保持不可变；
- Decoder 参数中不得出现 source、摘要、SessionId 或 sender report；完成后再由独立审计计算 SHA-256/BLAKE3/bytes；
- 每轮要求 8/8 或全部 Segment、WholeFileDigest、safe publish、final reopen、无 `.part/.resume` 残留、CRC/identity/conflict=0；
- 记录 sender submitted FPS、receiver unique FPS、每 lane FEC/erasure、resource 细分、active/reserved peak、goodput 和期限；
- 500 MiB/1 GiB 开始前检查目标卷可用空间和预计时长；只检查项目/证据目标，不枚举无关用户目录；
- 当前 Encoder 与 Receiver CLI 的硬上限均为 3,600 秒。若前一档实测投影证明下一档不能在期限内完成，先用测试证明并做
  有界上限调整；不得把 deadline、强杀或部分 Segment 写成成功；
- 一档出现 digest、发布、resource/conflict、内存增长或稳定性问题时，保留失败并先定位，不盲跑更大档；
- 本机大文件通过之后，等待用户醒来安排新的带远控因素真实复验；本机结果不得替代该复验。

## 8. 已执行和未执行的验证边界

`433bccf` 修复提交后已执行的最小 Release 验证：

```powershell
build-unified-release\tests\PBApplication\Release\PBApplicationTests.exe "[application][decoder][resume][atomic-replace][g21]" --rng-seed 21092026 --reporter console
build-unified-release\tests\PBApplication\Release\PBApplicationTests.exe "[application][encoder][atomic-replace]" --rng-seed 21092026 --reporter console
build-unified-release\tests\PBApplication\Release\PBApplicationTests.exe "[application][decoder][resume][journal]" --rng-seed 21092026 --reporter console
ctest --test-dir build-unified-release -C Release -R "^PBHeadlessMultiSegmentCheckpoint$" --output-on-failure
```

结果依次为 1 case / 23 assertions、4 / 184、4 / 243、CTest 1/1，全部通过。完整 Encoder package 原目录与 fresh
extract 的 `00_Check` 均通过；真实 remote 64 MiB 的发布/外部摘要结果见第 3 节。

本轮没有新增 full CTest、ASan、完整 GPU/corpus、Base-only、500 MiB/1 GiB 实屏、20 GiB 重跑或 G22。历史完整 Release
CTest 218/218 只属于路线记录的 `e0729b2`，不能把当前定向结果重新表述为当前 HEAD full CTest 全绿。

