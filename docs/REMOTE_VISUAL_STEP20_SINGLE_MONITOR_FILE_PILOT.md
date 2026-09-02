# RemoteVisual Step 20：单屏全屏发送端的真实文件级 pilot

## 1. 结论与适用边界

2026-09-02 的 FINAL4 运行已经证明以下最小文件链可以在真实 Computer B / Computer A 之间闭合：

1. Computer B 的 production `PixelBridgeEncoder.exe` 读取固定 1 MiB RAW source；
2. Encoder 以 `PB-RemoteVisual-LF4-X1`、2 Hz、Control repetitions 12 持续生成可见像素；
3. B 端唯一物理显示器由用户明确授权给 `UserAuthorizedSingleMonitorFullscreen` 模式，外层 surface 使用该显示器的实际 2560x1600 尺寸，内部精确 1920x1080 LF4 canvas 居中，四周填充中性背景；
4. 像素经过用户提供但对 Decoder 不透明的真实桌面视频链路，显示在 Computer A 的右侧 2560x1440 ExperimentMonitor；
5. A 端 Decoder 只通过该右屏的 WGC captured pixels 进入 production LF4 demod、temporal admission、现有 Receiver/Outer、WholeFileDigest 和安全发布；
6. live Receiver 发布的 1 MiB 文件与 source 做外部 length/SHA-256 比较完全一致；
7. 同一封口 Replay v2 又由 production offline Decoder receiver-only 重放，再次完成 WholeFileDigest、安全发布和外部 byte-exact 比较；
8. Decoder 完成后 Encoder 继续广播，最后由用户在 B 端按 Q/Enter 人工停止；Encoder report 明确没有从发送端推断接收完成。

这条结果是**真实文件恢复成功证据**，不是正式双屏 sender safety Gate、provider/mode 矩阵、Certified Profile 或任意 geometry 合同。正式 `PilotPlan.1/2` 所要求的双屏 ProtectedMonitor、provider UI evidence、冻结 plan/process-result、独立跨机时钟校准和同一 clean committed source identity，本轮均没有补造；相应字段保持 `NotApplicable`、`NotProvided` 或未验证。

## 2. 为什么增加单屏全屏发送模式

Computer B 只有一块物理显示器。原有 public `remote-lf4` Encoder 正确要求不同的 ProtectedMonitor 和 ExperimentMonitor，因此会 fail closed，不能在该机器上启动正式双屏 Gate。不能通过伪造第二块显示器或把同一 identity 同时填入两个角色来绕过这个约束。

本轮增加的是显式、sender-only 的 kiosk authority：

- CLI 必须使用 `--single-monitor-fullscreen primary|DEVICE --manual-stop --loop`；
- 只允许 `remote-lf4`、RemoteVisual、1..5 Hz；
- 不能同时给出 `--origin`、`--seconds` 或双屏 monitor 参数；
- runtime 每秒重新验证 exact monitor identity、physical rectangle、DPI、rotation 和 refresh；
- window 使用 `WS_EX_NOACTIVATE`，不会抢占控制台键盘焦点，同时只在此显式模式下使用 topmost；
- report 固定记录 `singleMonitorFullscreen=true`、`monitorSafety.preflightPassed=false` 和 `NotApplicableSingleMonitorFullscreen`，绝不把单屏用户授权冒充双屏保护通过。

外层画面不要求 16:9。2560x1600 的 B 端实例使用完整 16:10 surface，1920x1080 canvas 原点为 `(320,260)`。Decoder 仍以 locator 从实际像素估计独立 X/Y geometry；本轮实际 A 端 scale 为 X/Y 都等于 4/3。该一次成功不能证明裁切、旋转、透视、非均匀极端 scale 或 0.5..2.0 之外的输入可恢复。

## 3. 可交付物与启动顺序

FINAL4 archive：

- 文件：`PixelBridge-RemoteVisual-LF4-SingleMonitorFullscreen-FINAL4-afec931f-2fb7c7c27bb9.zip`
- bytes：27,889,795
- SHA-256：`b9d67078ca8bf47e3fed1a2ae02ecf20d1b4b9e18882d7ac2439271b18690a6c`
- shared RunId：`d82f167c98ec4451abc269cae3b1bc4f`
- source bytes：1,048,576
- source SHA-256：`93f85aa63ed348ef4d565cd0bb942b2417ba4bb6e5ffafd223239ddfd83d3587`
- packaged Encoder SHA-256：`789f8bb2f8ff6bb14fea2d3b555d0a60e4616de86e4010c427cbfac2a9633109`
- tested-source fingerprint：`2fb7c7c27bb943101cfd9307462662789e7e3a537f918280191e465baaea4525`

正确顺序是两阶段握手：

1. A 端先启动 Decoder，并确认其已进入 WaitingForBootstrap；
2. 再让用户在 B 端双击 `Start-PBRemoteVisualSingleMonitorFullscreen.bat`；
3. Decoder 完成 WholeFileDigest、publish 和 Replay footer 后，保留 A 端证据；
4. 至少继续保持 B 端广播一段明确窗口，然后用户在 B 端按 Q 或 Enter；
5. 最后把 B 端 `runs/<RunId>` 中的 report/journal/used RunId 复制回 A 端做事后汇总。B 端证据复制发生在 Receiver 完成以后，不进入 payload 链。

BAT 会先解析并检查实际 runtime executable、source identity 和 create-only RunId；支持 `PB_PACKAGE_PREFLIGHT_ONLY=1` 做不消耗 RunId 的包探针。它不会调用远控文件传输、clipboard、共享目录、socket、pipe 或任何隐藏 payload IPC。

## 4. 失败轮保留与根因

失败轮没有删除或改写：

| 轮次 | 观察 | 分类 | 处理 |
| --- | --- | --- | --- |
| 初始包 | BAT 报 `RuntimePackage\\Encoder\\PixelBridgeEncoder.exe is missing` | packaging path | resolver 同时支持实际 root layout 和兼容 layout，并用真实解压目录预检 |
| 下一包 | Windows PowerShell 5.1 缺少 `Get-FileHash` | launcher compatibility | source SHA 改用 `.NET SHA256`，保留旧 PowerShell 可运行性 |
| FINAL2 | live 完成，但 2 Hz sender 的 Replay 也只采样 2 fps；761/799 unique，offline 无输出 | Replay sampling / temporal coverage | 不弱化 Receiver；新 run 把 Replay 采样提高到 5 fps |
| FINAL2 reattach | 在长 Carousel 中段重新附着时没有及时拿到 descriptor | scheduler / late join | 不伪装成功；留给 Step 26 的 descriptor repetition 和 late-join scheduler |
| FINAL3 | B 端先于 A 端 WaitingForBootstrap 启动，错过部分 Control 窗口 | operator coordination | 固定 Decoder-first 两阶段握手 |
| FINAL4 | live 与同 Replay offline 均完成 | success | 封存 A/B exact artifacts |

FINAL2 的失败尤其说明“live 成功”不能替代同 Replay offline reproduction；Replay 只是同一画面的采样记录，采样不足不会自动生成遗漏的 Fountain equations。

## 5. FINAL4 B 端证据

B 端证据目录包含三个事后复制文件：

| artifact | bytes | SHA-256 |
| --- | ---: | --- |
| `encoder-report.json` | 4,272 | `d6c49c643668236bf8d635967ad2ee88cb1bb143e7f08c6a6c6c383906ba44b9` |
| `encoder-journal.ndjson` | 209,958 | `c600a1b47f667bf7cd245fe753c6282c6a8327d473078139fefcf83b537a1187` |
| `shared-run-id.used.txt` | 32 | `92fe2094c54c95847958be9c9624a9051049ee44027029db367a8b75770ff736` |

严格解析结果：

- schema/role：`PixelBridge.RunReport.2` / `Encoder`；
- state：`Stopped`，status 为用户停止且没有 sender-side completion inference；
- RunId：`d82f167c98ec4451abc269cae3b1bc4f`；
- SessionId：`0838aa24c8f231d83cde38e86d1de57e`；
- SessionTag：`8688970835869787731`；
- WholeFileDigest：`cb26a8de0131d377a57a11aa77d9508c02b7ca74b3133574bdc1bf631c729c0e`；
- source：1,048,576 bytes，`sourceStable=true`；
- configured/generated visual FPS：2 / 1.9681186872；
- configured/minimum observed dwell：500 / 500.0213 ms；
- dwell violation：0；
- submitted/source replacements：408 / 408；
- repeated Present：23,343；pending HWM：1；
- report evidence：203 journal samples、209,958 bytes、finished、valid、not truncated；
- single-monitor truth：2560x1600、`singleMonitorFullscreen=true`、formal monitor preflight false、`NotApplicableSingleMonitorFullscreen`。

Journal 第一条是 Preparing，最后一条是 Stopped；全部记录使用同一 RunId，时间和累计 counter 单调，terminal counters 与 report 一致。

## 6. FINAL4 A 端 live Receiver 证据

权威 evidence root：

`build-p1_5-evidence/step20-user-fullscreen-final4-d82f167c98ec4451abc269cae3b1bc4f`

关键结果：

- state：`Completed`；requested/actual backend：WGC/WGC，无 fallback；
- ROI：右侧物理屏幕 `[2560,0]-[5120,1440]`；monitor safety PASS；
- SessionId、SessionTag、WholeFileDigest 与 B 端完全一致；
- `uniqueVisualFPS=1.9681975991`；`endToEndUniqueVisualFPS=1.9670808854`；
- 880 evaluated codewords，96 FEC failures，800 temporally admitted Transport blocks；
- Outer unique symbols 799，resource/conflict rejection 都为 0；
- CRC failure 0，identity failure 0；
- 1,048,576 verified encoded bytes；
- WholeFileDigest PASS，final publish PASS；
- live output SHA-256 与 source 完全一致；
- recovery runtime 254,201 ms；该口径包含约 125 秒 Decoder-first 等待，权威 `VerifiedEncodedGoodput=34,115.9835 bit/s`，不能用剔除等待后的局部速率替换；
- Replay：906 frames、0 dropped、5 fps sample ceiling、13,359,993,339 bytes、footer finalized、evidence valid；
- Replay SHA-256：`ac3606501728b2ee0a70e38f8a97e11518af5b806483104c817a81b6374e27f2`。

## 7. 同一 Replay 的 production offline reproduction

offline Decoder 在 fresh output directory 中读取上述同一 Replay：

- state：`Completed`；
- 906 capture frames、906 production demod results、0 observation mismatch；
- SessionId、SessionTag、WholeFileDigest 与 live/B 完全一致；
- 836 evaluated codewords、36 FEC failures；
- 812 temporally admitted Transport blocks、799 Outer unique symbols；
- CRC/identity/resource/conflict rejection 都为 0；
- WholeFileDigest PASS，final publish PASS；
- output 为 1,048,576 bytes，SHA-256 与 source/live 完全一致；
- runtime 95,349 ms，process exit code 0。

live/offline FEC failure 数不同不是 Receiver truth 冲突：live primary demod 按实时 WGC arrival 运行，Replay 是独立的 5 fps capture fan-out；权威一致性是相同 sealed Replay 由 production receiver-only offline path 独立完成相同 Session/file digest/publish，而不是伪造逐帧 live observation parity。

## 8. 严格合并与尚未满足的正式条件

现有 strict `PBRemoteVisualReport` 已直接重新读取并封印 B Encoder report 与 A live Decoder report，外部重算 source/live output，并纳入 Replay 和两端 journals。其 create-only combined report 得到：

- `formalMergedRun=true`；
- `identityStatus=Complete`；
- `evidenceValid=true`；
- `successfulRun=true`；
- `certifiedRemoteVisualProfile=false`。

最新 create-only 文件链验证记录为 `file-pilot-verification-v3.json`，9,715 bytes，SHA-256 `52d229048c10f8350228bd1ad26ed097551e018276d6c544665ebc19f09d3b0b`；其顶层结果保持 `verifiedFileRecoveryChain=true`、`formalStep20PilotAccepted=false`。先前 v1/v2 记录继续原样保留，没有被覆盖。

这里的 `successfulRun` 只表示所提供两个 endpoint report 的 Receiver/file 合并合同通过。它不能自动补齐正式 Step 20 plan 的现场 provenance。尤其要保留：

1. Decoder 不知道用户采用了哪一种远控/本地视频播放策略，provider/mode/chroma/network 仍为未知；
2. B 端单屏模式有意不满足 ProtectedMonitor/ExperimentMonitor 双屏 safety Gate；
3. 本轮没有正式 UI evidence、endpoint environment/deployment/pilot plan/process-result 链；
4. 跨机时钟 offset 没有独立测量。报告中的 raw wall-clock 窗口相交，B 端 raw end timestamp 晚于 A live end 87,545 ms，但不能把默认 0 ms offset/5,000 ms uncertainty 当作现场校准；
5. 2 Hz 正式计划原本冻结 4 fps Replay，本轮为修复 FINAL2 coverage 使用 5 fps，因此是可复核的用户 pilot 参数，不是原 plan 的无偏差执行；
6. package 精确封印 executable、全部 payload 和当时 911 个 source 文件 fingerprint；之后的 CLI/parser/test 收紧属于新的 working-tree 状态，不能反向改变 FINAL4 artifact identity；
7. 一次 1 MiB/2 Hz/4:3 locator-scale 成功不等于 Step 21 provider matrix、Step 22 6/6 smoke、Step 23 QoS、Step 24 soak 或 Step 27 certification。

用户随后明确选择“按例外关闭”：路线表 Step 20 记为 `DONE`，完成语义严格限定为`UserAuthorizedSingleMonitorFullscreen`核心文件 Gate。正式双屏 sender safety、PilotPlan/process-result、provider UI provenance、独立跨机时钟校准和任意 geometry 支持继续作为独立未满足限制；机器verification保留`formalStep20PilotAccepted=false`与`certifiedRemoteVisualProfile=false`，防止路线记账决定被误读成field certification。Step 21仍必须用逐run真实矩阵建立provider/mode/scale/FPS/backend适用域，不能从FINAL4一次成功外推。
