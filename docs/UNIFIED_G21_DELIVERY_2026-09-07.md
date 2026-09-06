# Unified G21 本次交付与目录索引（2026-09-07）

## 1. 交付结论与身份

按用户最新要求，完成交付、代码提交和目录整理后结束本次任务，不继续长时测试或后台执行。
**本机右屏 15 Hz、64 MiB → 500 MiB → 1 GiB 三档 actual-pixel 已通过；G21 仍 PARTIAL，真实远控复验未执行，G22 未开始。**

必须区分三个身份：

| 对象 | 身份 / 验证范围 |
|---|---|
| 三档实屏 authority、交付 ZIP 内的 Encoder/Gate、单独冻结的 Decoder | `6e9064319a51bedcd403d74d47dd45c067928013`；三档在该正式候选下依序通过 |
| 最后工具/测试提交及当前 `build-unified-release` 三 EXE | `959678340d1946fed4fec01b4410a250b533daa3`；提交后 configure/build、三 EXE identity 和 Gate self-test 通过，**没有重跑实屏** |
| 本文之后的最终 HEAD | 纯文档交付提交；不会把旧实屏验证升级成新构建身份验证 |

`git diff 6e90643 9596783 --name-only` 只有文档及 `tests/UnifiedRemoteGate` 的 PowerShell/Python/C# fixture/README，
没有任何产品 C++、CMake、协议、调度或 telemetry 修改。即使产品源码内容相同，也不冒称新 identity 已实屏验证。

主要独立提交：

| 提交 | 内容 |
|---|---|
| `bf52b24f2b3c95368b8de382fe50f59215e387db` | 相位均衡、1/10 Pass-0 repair、周期 Control triplet、16-Segment transition probe |
| `21b35913f0c3a6a3a3342f2a33715b36f5d263fd` | live journal 按新增字节触发 compaction，保留持久/崩溃语义 |
| `2861beb4affe1a37ea386dafdc52e27d7a1d6498` | BGRA marker scan 位精确热路径优化 |
| `6e9064319a51bedcd403d74d47dd45c067928013` | 首次 coverage 失败诊断，不改分母、admission 或 Gate |
| `2092ec72ace547eae0cdc833c4e517c8b136b54d` | 同候选三档 LocalDesktop 结果封存 |
| `9fcbed27827d0c55bb3776bad81802b4ce202ed7` | 包装层 fail-fast、退出证据、独立双摘要和进程夹具入库 |
| `959678340d1946fed4fec01b4410a250b533daa3` | 按 PS5.1 实测校正旧 finally 行为断言，保留全部失败覆盖 |

## 2. 统一交付目录

`<repo>\artifacts\g21-delivery-2026-09-07`

| 文件 | 用途 |
|---|---|
| `README.md` | 本次交付入口 |
| `FIELD_RECHECK_README.md` | 后续真实远控复验的最小步骤；本次无需用户操作 |
| `PixelBridge-G21-RemoteEncoder-6e90643-15Hz-v2.zip` | 远端发送机完整依赖、fresh source 与只读 lease、停止后双摘要 |
| `PixelBridge-G21-LocalReceiver-6e90643-15Hz-v2.zip` | 本机右屏 Receiver/Gate、bounded watchdog、失败 snapshot 和内存时间线 |
| `local-staircase-audit.json` | 三档数字、外部摘要、原始 artifact 路径及封印 |
| `product-targeted-tests-6e90643.json` | 正式实屏候选的窄测试命令、期限、退出码 |
| `wrapper-lifecycle-tests.json` | 20项无屏幕生命周期测试，显式 fixture authority |
| `package-verification.json` | ZIP逐entry/hash、新解压CheckOnly、双摘要和负例结果 |
| `directory-organization.json` | 项目目录清单、原始 evidence 保留位置及保护文件指纹 |
| `DELIVERY_MANIFEST.json` | 上述文件的 exact bytes/SHA256（不包含自身） |

ZIP 封印：

- Sender：26,166,428 bytes、39 entries；SHA256 `018abb6c7ccd8dd83c42dcfb31dcb2da7f5ca354d01ea9b5bce04ca1210d1451`。
- Receiver：1,024,293 bytes、8 entries；SHA256 `fc5fbd863b586036925d7326e9edfafb22fbb4181b7374523c435b58c6ab5760`。

V2准备/验证根：
`<repo>\build-unified-release\g21-6e90643-remote-ready-v2-31405053c7db4979ac6a3dc85de05204`。
同根 `validated-local-decoder\PixelBridgeDecoder.exe` 保存原6e90643 Decoder与完整runtime依赖，SHA256
`8321da971527ebc9939bc31441636aaea9a56263e7f96df378c3bd37591c549c`；只验证加载/identity，不额外声称GUI live通过。
V1保留不变；后续远控使用V2。ZIP不含source、published payload或运行中oracle；这不是G22发行包。

## 3. 本机 actual-pixel 三档最终值

| 档位 | exact bytes | Segment | Receiver elapsed ms | unique frames | UniqueVisualFPS | B/unique |
|---|---:|---|---:|---:|---:|---:|
| 64 MiB | 67,108,864 | 8/8 | 250,754 | 3,700 | 14.921582942802349 | 18,137.53081081081 |
| 500 MiB | 524,288,000 | 63/63 | 2,121,922 | 31,630 | 14.936724736545933 | 16,575.6560227632 |
| 1 GiB | 1,073,741,824 | 128/128 | 4,328,785 | 64,577 | 14.938359707725137 | 16,627.31040463323 |

共同：Pass0完成、WholeFileDigest / safe publish / final reopen / frameCoverageComplete 全true、
外部exact bytes/SHA256/BLAKE3全相等、全部Segment、lane CRC/identity/resource/conflict/deferred/quota/orphan全0、
无.part/.resume/receiver error、两端exit0/0、无deadline/强杀。16,384 B/unique硬门全过；32,768工程目标未达到。
Sender configured=15Hz；submitted frames/FPS分别为3,739/15.003677845770426、31,766/14.99078004284591、
64,816/14.990193138810495，不把它们当作Receiver UniqueVisualFPS。

| 档位 | Receiver WS / private bytes | Encoder WS / private bytes |
|---|---:|---:|
| 64 MiB | 399,527,936 / 551,387,136 | 258,568,192 / 319,152,128 |
| 500 MiB | 400,359,424 / 611,368,960 | 277,176,320 / 320,421,888 |
| 1 GiB | 400,695,296 / 691,380,224 | 277,323,776 / 320,622,592 |

active decoder峰值8、reserved bytes=457,201,696，各档恒定。已核对完成payload释放链；WS基本稳定，但private
high-water跨fresh run有所增长，现有峰值不是allocator时间线，不能宣称private完全平坦或>=20GiB认证。

## 4. 原始证据索引与保留策略

三个原始root：

1. `<repo>\build-unified-release\g21-6e90643-staircase-64mib-8feb223231a84e828dab5c4e73e0c954`
2. `<repo>\build-unified-release\g21-6e90643-staircase-500mib-c13ee15e64c84925b896402deef3c3cb`
3. `<repo>\build-unified-release\g21-6e90643-staircase-1gib-4b966e20a4d748d39c8816387065ebbf`

每根保留 `receiver\final.json`、`receiver\receiver-checks.json`、`external-digest-audit.json`、
`staircase-verification.json`、`process-exits.json`、`pass0-completion-check.json`、source/published和全部日志。
外部SHA256/BLAKE3原值与完整命令见交接0.13和 `local-staircase-audit.json`，不重新哈希大文件或复制其payload组包。

开发/原始定向证据：
`<repo>\build-unified-release\g21-pass0-transition-d3d6e5ba640d4305b161a2e467b70896`。
`same-candidate-6e90643-staircase-audit.json`、`formal-6e90643`、`memory-ownership-review-6e90643.json`及旧失败均保留。

目录整理采用统一交付入口、源码工具分层README与证据索引，没有移动/删除历史build/evidence，避免破坏CMake缓存和
原始报告的绝对路径。根目录 `display_probe.obj` 早于本次任务（2026-08-30），未擅自认领或删除。
`docs\PHASE1_GATE_REPORT.md` 始终未跟踪、未修改、未暂存，SHA256仍为
`076EF4C9B9F89EABCCD323DBE4BFFC4DC125DDAF96E6EE437D2CF5B1B1CEA306`。

## 5. 收尾验证

正式6e90643产品测试：scheduler10 cases/247,055 assertions，Application G21+G17 13/2,744，telemetry9/8,345，
Gate self-test及16-Segment clean/phase3均PASS；完整命令在 `product-targeted-tests-6e90643.json`。

最终工具提交后实际执行：

```powershell
Set-Location -LiteralPath <repo>
& 'C:\Program Files\CMake\bin\cmake.exe' -S . -B build-unified-release -DVCPKG_MANIFEST_INSTALL=OFF
& 'C:\Program Files\CMake\bin\cmake.exe' --build build-unified-release --config Release --target PixelBridgeEncoder PixelBridgeDecoder PBUnifiedRemoteGate --parallel 2
& '<python>' -X utf8 tests\UnifiedRemoteGate\test_g21_wrappers.py --new-run-root '<repo>\build-unified-release\g21-6e90643-remote-ready-v2-31405053c7db4979ac6a3dc85de05204\checks\lifecycle-9596783' --baseline-kit-root '<repo>\build-unified-release\g21-6e90643-remote-ready-9fcfe8767919473badec2859e9154af1'
& '<repo>\build-unified-release\tests\UnifiedRemoteGate\Release\PBUnifiedRemoteGate.exe' --self-test
```

以上命令均exit0；重放fixture必须换新root，不能覆盖上述证据。
三EXE `--build-identity`均为9596783；记录在V2 `checks\binary-identities-9596783.json`。
`checks\postcommit-build-9596783.json`与configure/build日志保存提交后构建结果。

20项fixture包括正常退出、初始等待、partial JSON、exit7、coverage、overflow、九项资源计数、旧Receiver漏fail-fast、
旧Sender摘要失败丢exit、新Sender保留exit和只读source lease、新旧未启动Process清理。无窗口/无capture/无payload传输。
旧PS5.1 finally掩盖异常的假设未复现；已按实际行为校正断言，原失败回归记录保留，不删除测试制造通过。

组包验证：逐entry bytes/SHA256、无重复/越界路径、fresh extraction CheckOnly无run/source/window；
empty/abc/2,056,443-byte跨buffer双摘要与Python一致；错DLLhash、既有输出、篡改Gate拒绝且不生成run。
没有新增full CTest、ASan、GPU/native矩阵、20GiB、真实远控或新候选实屏长测。

## 6. 后续边界

- Base-only同actual-capture中和派生证明已经成立，保留独立authority，不在本次重复3.8GB回放。
- 独立live false-accepted codeword oracle仍为null，reason=`No independent sender truth supplied to receiver`。
- 旧2861beb一次coverage=false在6e90643三档未复现；诊断增强不是根因修复，不放宽coverage或unique分母。
- 后续由用户安排真实远控场景，再以冻结6e90643的V2包执行smoke/full及外部审计；G21真实满足退出标准并提交后才进入G22。
- 本次只完成获准的本机阶段和交付；结束任务不表示完整Unified路线或最终产品已认证。
