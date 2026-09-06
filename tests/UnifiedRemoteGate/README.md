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
| `Start-G21Encoder.ps1` | 远端 fresh CSPRNG source、只读 lease、15 Hz / smoke 600 秒 / full 1800 秒；停止后独立摘要 |
| `Start-G21Receiver.ps1` | 本机 Receiver 先启动、监视 coverage/resource、bounded watchdog、1 秒内存时间线与退出证据 |
| `Get-G21FileDigests.ps1` | 进程停止后独立读取 exact bytes/SHA256/BLAKE3；不向 Decoder 提供 source 或摘要 |
| `test_g21_wrappers.py` / `wrapper_lifecycle_fixture.cs` | 无窗口、无 capture、无 payload 通道的进程生命周期 fixture；不是 G21 live PASS |

`03/04_*5Hz` 与 `Start-G21*5Hz.ps1` 只属于保留的旧失败诊断，不是当前 15 Hz 入口。

## 远控复验包布局

脚本需要与冻结包中的 `expected-build.json`、`bin/` 及完整依赖共同使用，不应在此源码目录直接运行 live。
脚本和 EXE 各自有 hash/provenance：更新包装脚本不把历史实屏验证变成新二进制身份验证。
Windows PowerShell 5.1 / Windows x64；建议解压到较短的新绝对路径，保留原始包与全部运行证据。
CheckOnly 不生成 source、Session、capture 或窗口。Receiver 先启动；Sender 后启动；不得提供隐藏 payload IPC。

Receiver 仅在 `observationAvailable=true` 后把 coverage=false 当成不可恢复失败，初始等待不误杀。
计数溢出及九个 resource/conflict/deferred/quota/orphan 计数非零立即保留 snapshot 并停止本档。
强制停止仅表示 fail-fast，绝不算正常退出或 Gate 成功。内存样本最多 deadline+31 条，输出/退出等待有界。
Sender 先保留 `process-exit.json` 再运行摘要 helper，helper 失败不能丢失已发生的 Encoder 退出码。
最终 Gate 成功仍需独立外部审计；false-accepted live oracle 不可用时保持 null/unavailable。

## 窄回归（不操作屏幕）

```powershell
& '<python>' -X utf8 '<repo>\tests\UnifiedRemoteGate\test_g21_wrappers.py' --new-run-root '<NEW_ABSOLUTE_SHORT_ROOT>'
```

可选 `--baseline-kit-root '<OLD_KIT_ROOT>'` 复现旧 Receiver 未 fail-fast、旧 Sender 摘要失败丢失 exit，并比较
未启动 Process 的 finally：本机 PS5.1 旧/新版本都保留原异常，新版额外保存 started=false / receiverExit=null。
不把未复现的旧 getter 异常写成已修复故障。每例在新目录复制显式 `g21-lifecycle-fixture` 身份的 .NET fixture，
不是产品 EXE。每个外层 watchdog 有界；清理前核对自己启动的 PID 仍指向该 fixture 的绝对 EXE 路径。
原始测试结果不覆盖，fixture 使用短目录组件以兼容 .NET Framework 的历史 MAX_PATH 限制。
