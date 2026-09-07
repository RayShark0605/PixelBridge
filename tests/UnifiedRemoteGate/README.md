# Unified G21 tools

本目录保存可审查的工具源码；构建、运行输出和原始 evidence 不写回源码目录。
当前产品合同和权威结果分别见 `docs/UNIFIED_VISUAL_LARGE_FILE_IMPLEMENTATION_ROADMAP.md` 与
`docs/UNIFIED_G21_EXECUTION_HANDOFF_2026-09-06.md` 第 0 节最新增量。

## 工具分层

| 入口 | 用途 / authority |
|---|---|
| `remote_decoder_gate.cpp` / `PBUnifiedRemoteGate` | 生产 Decoder 的受保护右屏实际捕获、发布和 G21 Gate；仅 `--self-test` 不捕获 |
| `Run-G21LocalStaircase.ps1` | 同机 actual-pixel 64 MiB → 500 MiB → 1 GiB；必须交互 console / Codex `tty:true`，不等于 remote field |
| `extract_g21_phase_loss.py` | 从历史 journal 提取有 provenance 的缺失方程集合，不是 live oracle |
| `run_g21_transition_probe.py` | 16-Segment Pass-0 transition / bounded W8；synthetic 定向证明，不是实屏吞吐 |
| `Start-G21Encoder.ps1` | 远端 fresh CSPRNG source、只读 lease、15 Hz / smoke 1 MiB 600 秒 / full 64 MiB 1800 秒 / 1 GiB 7200 秒边界档 / 1 GiB manual-only 无限 Carousel 完成档；停止后独立摘要 |
| `Start-G21Receiver.ps1` | 本机 Receiver 先启动、`--receive-eventual` 监视、bounded watchdog、1 秒内存及可恢复 busy 时间线与退出证据 |
| `Get-G21FileDigests.ps1` | 进程停止后独立读取 exact bytes/SHA256/BLAKE3；不向 Decoder 提供 source 或摘要 |
| `test_g21_wrappers.py` / `wrapper_lifecycle_fixture.cs` | 无窗口、无 capture、无 payload 通道的进程生命周期 fixture；不是 G21 live PASS |

`03/04_*5Hz` 与 `Start-G21*5Hz.ps1` 只属于保留的旧失败诊断，不是当前 15 Hz 入口。

## 远控复验包布局

脚本需要与冻结包中的 `expected-build.json`、`bin/` 及完整依赖共同使用，不应在此源码目录直接运行 live。
脚本和 EXE 各自有 hash/provenance：更新包装脚本不把历史实屏验证变成新二进制身份验证。
Windows PowerShell 5.1 / Windows x64；建议解压到较短的新绝对路径，保留原始包与全部运行证据。
CheckOnly 不生成 source、Session、capture 或窗口。Receiver 先启动；Sender 后启动；不得提供隐藏 payload IPC。
`03_Start_Full_1GiB.bat` 是用户明确批准跳过远程 smoke/64 MiB 后的直接 1 GiB 入口；精确大小为
`1,073,741,824` bytes，Receiver 与 Sender 都使用 7200 秒有界期限。该用户决策不回写或放宽协议、摘要、
coverage、真实 resource rejection/conflict 或 16 KiB/unique Gate。
首轮真实 RDP 运行在第二个 Segment window 已出现配对 busy，因而末尾 Segment 最早要等下一 FullRepair pass 的末尾；
仅由调度下界 `64,512 + 70,144` sender frames / 15 Hz 已得约 8,977 秒，证明 7200 秒档无法覆盖最早完成点。
用户随后明确选择 `04_Start_Full_1GiB_Manual.bat` / `1gibmanual`：同样精确 1 GiB 和 15 Hz，Encoder 省略
产品仅允许 1..7200 的 `--seconds`，改用产品已有的 `--manual-stop --loop`，无自动 sender deadline；Decoder
独立使用 `21,600` 秒有界 Gate，为多轮 Carousel 留余量。旧 `1gib` 仍保留为已执行的 7200 秒边界档，不静默改义。

Receiver 仅在 `observationAvailable=true` 后把 coverage=false 当成不可恢复失败，初始等待不误杀。
计数溢出、真实 resource-policy/FEC rejection、conflict 和 orphan 丢弃/耗尽仍立即保留 snapshot 并停止本档。
`DeferredResourceBusy` 仅在与 `OuterFecQuotaExceeded` 累计值严格配对、单调、历史峰值达到声明的有界
active-decoder window、当前/峰值计数和预留字节均未越界时归类为可恢复背压，继续等待后续 Carousel 重试；
任何缺字段、不配对、回退或越界仍 fail-closed。1 秒采样不要求恰好看到当前 window 仍为 8/8，避免把已经释放
容量的正常进展误判为失败。
最终 `receiver-checks.json` 同时保留 `eventualRecoveryPassed` 与 `strictPass0ZeroPressurePassed`：前者是本次
用户批准的远程完成口径，后者保持原始 Pass-0 零压力结论，绝不把非零 busy 伪写为零。原 `--receive` 严格模式
仍保持所有 busy/quota 计数必须为零。
强制停止仅表示 fail-fast，绝不算正常退出或 Gate 成功。内存样本最多 deadline+31 条，输出/退出等待有界。
Sender 先保留 `process-exit.json` 再运行摘要 helper，helper 失败不能丢失已发生的 Encoder 退出码。
最终 Gate 成功仍需独立外部审计；false-accepted live oracle 不可用时保持 null/unavailable。

## 最终 1 GiB 现场结果（2026-09-07）

v5 manual Sender 经 Windows 远程桌面在本机右屏完成唯一 Session
`daba04b1c7c8c22a31604ea68dd61f8f`。Receiver 128/128、精确 1 GiB、whole digest、安全发布、final reopen、
外部 SHA-256/BLAKE3 和清理全部通过；eventual recovery true，Sender 正常 Q/Enter exit 0。原始性能为
8,626.511 B/unique，低于16 KiB，Receiver Gate 因该后置检查 exit 1；原报告和检查保持不变。

用户在准确结果披露后只对该唯一 Run/Session 明确豁免，所以 G21 最终记为
`PASS_WITH_SINGLE_RUN_USER_WAIVER`。该决定不改变本夹具的 Gate 实现或未来 16 KiB 门。权威结果、证据哈希和复核
步骤见 `docs/UNIFIED_G21_REMOTE_1GIB_RESULT_2026-09-07.md`；不得用本节文字覆盖原始 JSON。

## 窄回归（不操作屏幕）

```powershell
& '<python>' -X utf8 '<repo>\tests\UnifiedRemoteGate\test_g21_wrappers.py' --new-run-root '<NEW_ABSOLUTE_SHORT_ROOT>'
```

可选 `--baseline-kit-root '<OLD_PRE-V2_KIT_ROOT>'` 只用于复现指定旧 Receiver 未 fail-fast、旧 Sender 摘要失败丢失 exit，并比较
未启动 Process 的 finally：本机 PS5.1 旧/新版本都保留原异常，新版额外保存 started=false / receiverExit=null。
不把未复现的旧 getter 异常写成已修复故障。每例在新目录复制显式 `g21-lifecycle-fixture` 身份的 .NET fixture，
不是产品 EXE。每个外层 watchdog 有界；清理前核对自己启动的 PID 仍指向该 fixture 的绝对 EXE 路径。
原始测试结果不覆盖，fixture 使用短目录组件以兼容 .NET Framework 的历史 MAX_PATH 限制。
