# Step3-B 固定有状态码流对照执行记录（2026-09-08）

## 0. 当前结论与授权范围

**本地最小固定 codec 诊断完成；原始多帧恢复 PASS；固定 codec 恢复 NOT_RECOVERED。整个 Step3 仍为 PARTIAL，现场 NOT_RUN，Step4–Step10 未启动。**

用户先要求只读核对与参数计划，选择“新增多帧恢复样例”，随后确认按计划以目标模式实施；实施期间对实际颜色标记失败再次提问，用户确认仅补齐转换后的 BT.709 元数据。后来重新提问时，用户再次选择多帧样例；该重新选择没有触发重复编码或正常解调。

本轮固定为一个 64 KiB 文件、30 张生产 raster、15 fps、2 秒、软件 libx264 H.264 High 4.0、8 Mbit/s、VBV 4 Mbit、closed GOP 15；原始对照和两次同条件 codec 重建。原始未通过应停止；codec 不恢复可形成负向诊断，但不能改称恢复 PASS，不自动提高码率或放宽几何门。

没有修改生产源码、协议/Profile/layout、Golden、原 Decoder／Receiver、Citrix／网络、资源与安全发布门；不操作显示器、焦点、鼠标或键盘，不创建子智能体，不自动提交或推送。`<repo>\docs\PHASE1_GATE_REPORT.md` 保持原状。

## 1. 项目背景与本轮依赖

PixelBridge 用可见像素做单向文件传输，生产链路已具备分段、压缩、内外 FEC、解调、Receiver、安全存储和整文件校验。当前合同是 PB-Unified-SC6-V3/layout 10、1920×1080 BGRA8。非本机路线追求正确整文件完成时间和 VerifiedGoodput，不以配置 FPS 或理论容量代替。

Step1/Step2 本地 PREPARED/PARTIAL、现场 NOT_RUN。Step3-A 的原 512 B 样例使用首帧即可完成的 DirectRepeat，本轮改用必须跨帧的 Wirehair 样例。G1B 的几何准入问题仍未修复，不能假设 codec 像素必然进入数据解调。

只读能力核对确认完整 FFmpeg 8.1.1 注册 libx264/yuv420p、Matroska、scale、trace_headers；原 Step2 avcodec 有 H.264 decoder、无 libx264 encoder。于是编码／检查使用隔离的完整 FFmpeg 进程，解码仍使用原 Step2 媒体依赖及原 `recording_media.cpp`，两套 DLL 不混装、不替换产品。

## 2. 实现与固定参数

新增目录：`<repo>\tools\PBRemoteThroughputStep3B`，独立 CMake、C++ 像素夹具／回放工具、Python 串行监管／codec 属性检查／证据分析／相关 guard。不注册到产品 CMake。使用说明和精确命令见该目录 `README.md`。

```text
生产 EncoderRuntime → 无窗口 presentation → 冻结30张 BGRA
  ├─ 原始像素
  └─ FFmpeg编码 → 实际码流检查 → 原 RecordingMedia解码
→ RecordedPixelSource → 原 RunRecordedPixelReplay
→ 原 CaptureDemodulator / ReceiverPipeline → digest / safe publish / reopen
→ 进程结束后外部 byte / SHA-256 / BLAKE3 校验
```

Decoder 只获取像素和必要时序；源文件、ledger、expected payload 不进入 Decoder。诊断 stdout 管道不承载 payload；外部真值比较不回馈接纳分支。

| 参数 | 冻结值／实际核对 |
| --- | --- |
| Source | 65,536 B，固定域分隔 BLAKE3 块；seed=0x535445503342 |
| 生产压缩/FEC | 实际 Raw，encoded=65,536 B；一个 Segment；WirehairV2，block=1314，K=50 |
| 像素 | 30 张不同的真实提交 raster；1920×1080 BGRA8，pitch=7680，总248,832,000 B |
| 身份 | OS CSPRNG Session 冻结在像素中；30个不同实际 `(SessionTag, FrameSequence)`，不假造 Sender 时钟 |
| Codec/容器 | libx264，H.264 High，level 4.0，Matroska，单视频流 |
| 色彩 | yuv420p/8-bit，limited，BT.709 primaries/transfer/matrix，left，SAR1:1 |
| 转换 | 明确 full→limited/BT709，bilinear+accurate_rnd+bitexact，chroma位置0/128；最终追加 setparams |
| RC/VBV | target/min/max=8,000,000 bit/s；CPB=4,000,000 bit；init=0.90；nal-hrd=cbr；不用CRF |
| GOP | closed15；实际IDR ordinal0/15、28张P；B0/ref1/scenecut0/intra-refresh0 |
| 编码 | veryfast/zerolatency；单线程；asm/cpuflags0；lookahead0/mbtree0；deblock开启0:0 |
| 输入时间 | PTS=n，TB1/15，duration1；nominal2秒 |
| 实际媒体时间 | TB1/1000，PTS=(n*1000+7)//15，duration66；首末PTS差1.933秒 |

完整 argv、有效参数、原依赖与冻结哈希均在证据根；`context/parameters-v2.json` 是最终参数。8 Mbit/s 是开发实验约束，不是测得的 Citrix 带宽，实验也未构成认证视频 Profile。

## 3. 关键结果

### 3.1 原始多帧闭环

| 检查 | 结果 |
| --- | --- |
| 3帧前缀、新 Receiver 状态 | Bootstrap 3/3 有效，Outer unique symbols=32；未恢复、未发布，明确 prefix stop而非EOF |
| 多帧必要性 | 3×15×1314=59,130 B < encoded 65,536 B，结合前缀实际接纳，排除仅缺control的假证明 |
| 完整30帧、新输出目录 | EOF完整，30 Bootstrap接受，450 accepted slots/blocks；Receiver完成前unique admission identities=5 |
| 整文件 | verified Raw/Encoded各65,536 B，1 Segment；whole digest、rename、final reopen、外部逐字节与双摘要全部通过 |

450 是继续解调后的接受块数，包含重复／冗余，不是已验证吞吐或450个有用新方程。报告不生成现场 UniqueVisualFPS/goodput。

源文件与最终发布文件 SHA-256：

```text
40f1859cccb1f6949e8ff0a0c157c97571759a020d7bc3c801bcdc457394276c
```

BLAKE3：

```text
d38ef0993b8f097aee9ad93267387bdef83c49e1f0c54e35be8c87d23974aebf
```

### 3.2 真实颜色失败及经确认的修正

初始 `codec-01/channel.mkv` 的实际 SPS primaries=2、transfer=2、matrix=1。仅命令行请求 BT.709 不足以证明实际正确；检查器拒绝且不送 WARP。用户随后确认“补齐颜色标记”，仅在原显式转换之后添加 `setparams`，再做两次正式重建，所有其他条件和93观察上限不变。

首个错误码流、日志、旧参数和批准记录全部保留；真实错误颜色媒体也被原 RecordingMedia 拒绝（media-only，0解调观察）。不是删除失败后挑选成功参数。

### 3.3 两次正式固定 codec

| 项目 | 两轮相同结果 |
| --- | --- |
| 码流／像素完整性 | 30帧、30包、正确PTS/DTS/EOF；属性检查通过 |
| 重复性 | 容器逐字节一致；访问单元／参数集、解码像素／PTS、非时钟frame/Receiver/stage calls精确一致 |
| Container bytes | 2,195,347 B／份 |
| Video packet bytes | 2,194,486 B／份 |
| Packet平均码率 | 8,777,944 bit/s（按nominal2秒） |
| Container平均码率 | 8,781,388 bit/s |
| Peak packet | 254,955 B |
| HRD | bitrate8,000,000、CPB4,000,000；必要packet包络通过；不是完整标准HRD认证 |
| Filler | 实际NAL count=0；不能把编码器启用filler的选项当作实际出现 |
| 容器/原文件膨胀比 | 33.49833679199219 |
| Bootstrap字节 | 30/30双份有效 |
| 原生产准入 | 30/30 BootstrapErasure12 InvalidGeometry；frameErasure3，geometry4 Rejected |
| 数据/Receiver | accepted payload=0，Receiver admission=0，无整文件发布；NOT_RECOVERED |

容器 SHA-256（两份相同）：`af43f252e87ce7b30aca79dbd5b8ab0e5a99e6db8fb3cb2fcc2a400e3ca0f119`。

不能把短序列实际平均8.778 Mbit/s写成精确8 Mbit/s；CPB允许短时波动，已同时保存目标、HRD声明与实际字节量。不能把颜色转换、4:2:0及压缩组合效果单独归因为量化。

freshness/FEC/CRC未到达，使用null；pre-FEC BER没有独立测量也为null。codec accepted payload为0，raw payload oracle比较次数0，不构成零误接纳认证。没有修改几何数值门或证明每帧拒绝必然来自同一具体算式。

## 4. 验证与资源

- 两个独立新目录的Release工具构建成功，MSVC C++20 `/W4 /WX`，不重建产品、不安装依赖。
- 正常WARP观察 **93／93**：prefix3 + raw30 + codec30+30；没有追加正常回放。
- guard：`guards-01` 31/31，`guards-02` 5/5（受最终格式构建／解析修正影响的定向子集），`guards-03` 3/3；共34个不同方法、39次执行，全部通过，新增解调观察0。
- 负例覆盖codec不存在、启动／退出／超时／日志／输出超限、已存在目录、首部损坏、真实截断、错误颜色、帧数／PTS／DTS／duration／关键帧／HRD／包哈希／JSON异常、发布门篡改、错误accepted payload、未到达阶段null。
- 最终EXE另做30帧media-only像素/PTS一致性验证，不调用WARP/Receiver。
- 最终新增 `init` CLI做新根创建与重复根拒绝2项短检查；5个Python文件AST检查通过。没有重跑prepare或codec。
- 最新 `ANALYSIS-03.json` 再次只读核对封存输入、实际argv、码流、原报告与最终文件；没有启动Decoder。

合法raw精确248,832,000 B（外层256 MiB），单码流16 MiB；单AV allocation32 MiB；子进程在resume前被2 GiB process/job commit硬限制。trace4 MiB、普通JSON/日志1 MiB、header trace8 MiB；合法输入树1024文件/2048条目/深度16。证据目录≤1 GiB，ZIP另计≤1 GiB，最终实测大小及每成员哈希由封存记录核对。

受监管试验／负例进程累计 **16.935秒**，峰值job commit **201,240,576 B**；限额600秒/2 GiB。该累计不含人工确认、代码编辑、构建、证据哈希／压缩时间，不是整个研发墙钟或通道时间。磁盘输出用采样与退出后准入，非文件系统硬配额。归档保留深度17负例，不放宽合法输入深度。

未运行完整CTest、压力、参数矩阵、大文件、多Segment、旧录像整段、新录屏、硬件GPU、实屏或远程；不宣称现场吞吐或Holdout泛化。

## 5. 身份、产物与复验

工作树不是干净HEAD；开始时已存在37项tracked修改。HEAD始终 `4a36d0f1b84c94c7388a58b8249a3f1b6ec0634a`；不能把它单独当作本轮产品／工具源码身份。基线工作树、原diff和1159项源文件身份在 `context/preflight.json`、`context/pre-existing-source.zip`、`context/pre-existing-tracked.diff`；本轮只新增独立工具／执行记录并更新两个当前文档入口。

| 路径（证据根内） | 内容 |
| --- | --- |
| `source` / `frozen-pixels` | 源文件、双摘要、生产ledger、30帧、CSPRNG状态与Sender报告 |
| `raw-prefix` / `raw-full` | 多帧前缀证明、完整恢复与真正发布文件 |
| `codec-01` | 保留的首个颜色失败，不进入WARP |
| `codec-v2-01` / `codec-v2-02` | 正式两轮码流、检查、像素trace、原Receiver报告 |
| `ANALYSIS-03.json` | 最新综合只读分析；旧01/02保留 |
| `guards-01/02/03` / `logs` | 相关负例、实际命令、退出码、进程预算与日志 |
| `runtime-verified-run` | 真正执行93观察的EXE与原媒体DLL |
| `runtime-final` | 最终Allman格式源代码构建；不冒充重跑恢复 |
| `codec-runtime` | 64项完整FFmpeg／x264非系统依赖闭包 |
| `SOURCE_IDENTITY_FINAL.json` / `SOURCE_SNAPSHOT.zip` | 最终源树身份与快照，不替代原Step2静态库 |
| `RUNTIME_IDENTITY_FINAL.json` | 运行／最终build／依赖分离、原Step3-A依赖重新核验 |
| `FINAL_STATUS.json` / `FINAL_MANIFEST.json` | 逐要求审计、明确负结果、封存清单 |
| `STEP3B_REPLAY_EVIDENCE.zip` / `POST_SEAL_VERIFY.json` | create-only归档和逐成员读回hash验证；不是产品发布包 |

完整证据根：`<repo>\artifacts\remote-step3b-20260908-run01`。

原运行EXE SHA-256 `2062ed2b12cecba2f363e8989d9261a51fd50da23e18dc984dda856d5c5fb324`；最终构建EXE SHA-256 `b8aa6bc4aed07def5328f80c0da9cb194d593574fc78a7ebb03b6e412b515dcb`。正常实验后仅调整C++大括号格式，词法tokens和非debug机器section已核对；最终EXE仅另做media-only及定向负例。所有正常恢复证据仍明确绑定原运行EXE。

最小复验使用工具README中的 `analyze.py --root <原证据根> --output <新JSON>`，预期raw PASS、codec NOT_RECOVERED及重复性true，不再调用Decoder。真正重播需要另确认新的观察预算、全新输出；新生成fixture会有新Session。历史绝对路径不可重写成新的运行事实。

保护文件SHA-256仍为 `076ef4c9b9f89eabccd323dbe4bffc4dc125ddaf96e6ee437d2cf5b1b1cea306`；最终逐项旧文件hash核对以 `SOURCE_IDENTITY_FINAL.json` 为准。index保持空，无Git提交／推送／历史改写，无历史artifact/build/录像删除或覆盖。

## 6. 停止点

本轮交付的是可复验的最小固定codec对照平台和明确失败定位阶段，不是“该8 Mbit/s条件可传文件”。整体Step3尚缺现场校准／独立批次Holdout等工作，codec恢复本身未通过；不能据此自动晋级Step4、修改几何准入或开展实屏。任何新参数、几何修复、扩大样例或现场动作均需独立计划与确认。
