# PixelBridge 2026-09-14 收尾交接（v0.6）

## 0. 阅读本文件前必须知道

用户最后要求：**马上停止当前优化研究，尽快交付v0.6、详细现状文档和下一次任务prompt。** 用户另行明确允许只提交本任务的代码/测试/文档，不纳入`.zcode/`、`docs/PHASE1_GATE_REPORT.md`、`docs/V3_GRAY_STAGE_C_HANDOFF.md`，不推送。不要因旧`/goal`自动续做通知又启动远控、扫描或新实验。

下一任务获得用户继续授权后再研究。禁止创建子智能体。中文沟通，称呼主人；重要授权/需求不确定时提问并等待，普通可逆技术选择自行基于证据决定。

## 1. 最终目标、当前阶段

最终目标一直是缩短**非本机、纯视觉单向**传输TestMediumFile.bin及TestBigFile.bin的总时间，并消除长文件接收中途持续无进展。吞吐必须由完整文件的摘要、安全发布、重新打开验证及本机接收时间线证明。

当前阶段：已实现并验证多项候选；最新用户要求的“完整全屏”已落地，但扩大码面后实际有效帧率从约13.5下降到约7.5，主要优化重心仍是非本机视觉链路效率。R已消减不合适首轮策略造成的额外等待，没有解决这个帧率上限。**总体目标未完成，不能把v0.6打包视为目标完成。**

硬约束：不增加任何旁路payload或隐式ACK；不修改Citrix、网络、显示模式；不削弱摘要/发布/重开/冲突拒绝或资源预算；不碰左屏、焦点、鼠标、键盘。右屏测试需保持授权范围。频繁测试≤100000000B；完整指定文件只在有充分小样本证据的检查点运行。

## 2. 工作区与发布身份

- 工作区：`<repo>`（这里及下文路径以实际Windows反斜杠显示为准）。
- 本次原始HEAD：`110fce3b4b1cab485050b4a3fc7fefedc1604836`。v0.6获准提交后的确切HEAD不能从这段旧hash推断，读发布manifest和live git。
- 增量构建目录：`<repo>\build-nonlocal-stall-20260913-2111`。
- 会话证据根：`<repo>\artifacts\nonlocal-stall-20260913-2111`，下文简称ROOT。旧run/tag均create-only，不覆盖。
- v0.6交付根：`<repo>\artifacts\release-v0.6-20260914`。包、外部seal、解压验证、构建日志、来源匹配和最终receipt在此。
- v0.6是统一正常CMake构建，不继承旧私有替换object。封包应从隔离干净clone进行；增量build仍可在原工作区，但必须逐个比对提交源码与实际构建输入，不把不同树混装。
- 当前protected report基准SHA256：`076EF4C9B9F89EABCCD323DBE4BFFC4DC125DDAF96E6EE437D2CF5B1B1CEA306`。其余保护文件以发布stage的`protected-before.json`核对。

## 3. 用户提供的原954MB事故证据

原resume：`C:\Users/<user>\Desktop\PixelBridge-5e4e83e66150e401.resume`，82381527B，SHA256 `cc58c42c233b25ab94518a78a100440323606c814b0271b3116964d5dc4f67b8`。

文件描述为main.7z，1000667445B，64个15MiB分段；解析出48943个有效CRC记录、48883个已接受块、**0个完整分段**，8个未完成活动段ordinal24–30、32。48883×1629=79630407B=75.941474MiB，准确对应UI75.9。

因此“75.9MB已接收”不是6个完成段。证据支持未完成分段占用名额/调度压力的解释，但缺少原机runtime日志、FPS、启动顺序，不能宣称唯一根因已证明。原resume只读保留，不改写、不自动恢复、不将它当本轮100MB输出。

## 4. 今天关键演进（不要混淆候选）

### N：有限动态名额（实验开关）

`--budget-bound-decoders`去掉固定8的额外实验计数瓶颈，以maxSegmentCount的65536上限配合现有1GiB共享FEC预算等约束；单实例512MiB等不变。6MiB段实际24实例可分配、第25拒绝；15MiB段10实例可分配、第11拒绝。所有分配前验证、resume位图边界、默认模式拒绝不兼容状态保留。**正式默认8不变。**

### O：空间交织及缩短首轮（历史较高接收率成绩）

`--grayfast-spatial-interleave`和`--grayfast-short-initial-airtime`是显式实验开关。首轮65%只影响>12段graduation，后续完整repair预算不缩短。旧居中场景可以省等待；在四分之一保留率却会让首轮不足并显著变慢。不能一律认为65%更快。

### P/P2：正常源码整合与真正Capture入口修复

P的CPU/直接GPU像素矩阵全部通过，但1MiB现场仍120秒WaitingForBootstrap。真实分阶段Capture调用还使用geometry-only最小尺度1，未走已校验Bootstrap的灰阶profile准入。

P2用真实Capture测试先红后绿，仅改`DecodeUnifiedBootstrap`调用为完整observation；BlankControl补充带保留geometry-only。低尺度灰阶必须先通过同帧bootstrap/CRC/profile/layout绑定；SC6默认尺度不放宽。整合CPU/HLSL forward灰阶采样而非依赖私有object。

P2正常Decoder SHA256 `c4b60245fdf04fdafa276bc251f1a273ee0a3d628b0284c3451a281fa442bb8d`；旧O Encoder `48b1d14b2af793a74805368fcc2c110d2ffe22054a08c88fbe31a5a5c1a8f46f`。原分辨率1MiB8488ms、100MB329033ms，最终门均通过。

### Q：用户确认的完整全屏

Unified-family单屏全屏将canonical1920×1080通过物理像素中心point映射铺满，横纵独立，不裁切。旧LF4居中及普通窗口路径保留；wire、每帧字节、FEC不变。当前有界尺寸为宽1920..3840、高1080..2160；不能宣称无条件支持任意尺寸/旋转/缩放。

Q Encoder SHA256 `1106eacf45760c6c4c722f14ef78e60224e0a5af74b91b4fcf1d520e4bfb519d`，现场Decoder冻结P2。1MiB9386ms；实测尺度1.134363×1.261013，面积约1.974666倍，符合远端2560×1600完整铺满。100MB采用65%首轮变为782615ms，活动峰16、有效帧约7.5；不是提速。

25MB同Q包15/30FPS分别125042/127357ms，有效帧7.65986/7.49392。差异很小，不能说降低发送FPS抬高通道上限；且4段小文件不触发>12段策略。

### R：按实际接收率验证首轮预算

新增test-only PeriodicHalf，与已有PeriodicQuarter分别模拟15FPS保留1/2、30FPS保留1/4，均7.5有效帧/秒。真实headless Sender/FEC/恢复/journal/安全发布与独立摘要处理13×6MiB；不是像素或远控性能证据。

| 合成配置 | 帧数 | 模拟发送毫秒 | 完成后重复块 | 活动峰 |
|---|---:|---:|---:|---:|
| 30FPS/65% | 20173 | 672433 | 35306 | 13 |
| 30FPS/100% | 12349 | 411633 | 2126 | 5 |
| 15FPS/65% | 10117 | 674466 | 35310 | 13 |
| 15FPS/100% | 6199 | 413266 | 2144 | 5 |

随后**同Q/P2二进制**实传100MB，只取消65%开关。`nl0914-fill-gray100-fullinitial-f30-s6`完整525968ms，比Q快32.793519%；有效帧仍7.509483、Sender29.186912fps，完成后重复块4784、活动峰4、预留178267864B，资源/deferred/orphanQuota/conflict0。

SessionId `3ba9237eb4fcd65c2ae6b44242bb11a7`，SessionTag16731198839444628556；源fresh/稳定、无resume、whole digest/rename/reopen/published和两次SHA通过。100MB SHA256 `4111eb5acba90ea84b8e1cce370699d682ac1460e9f841069c7445cd89a134eb`，BLAKE3 `32d85d4af2aca6c97e3a7104a47163701708dc554f24b47f900349afa1ea4afc`。

相邻本机journal至少10秒的仅重复无新unique区间总和288156→20971ms，支持等待浪费减少，但这是辅助指标，不能替代完整native计时。Sender最后carouselPass为1，不能强说所有数据都在首轮完成。

## 5. 完整指定文件的唯一正确引用

历史最好O（旧居中/私有GPU候选）而非Q/R/v0.6：

| 文件 | 字节数 | 同本机harness耗时 | SHA256 |
|---|---:|---:|---|
| TestMediumFile.bin | 273806498 | 860828ms | 28fd5ead99bf7fe3526e38cd1a44364b3c02792323d01840f2f4bdc38ebc1bb0 |
| TestBigFile.bin | 1059917774 | 3451641ms | 20b000afa567a66dd536b22946102a465575f77299a42ffc2fcc2116f9808e76 |

合计4312469ms=1小时11分52.469秒；对H-clean合计6578282ms下降34.4438%。两个run：`nl0914-shortinit-medium-f30-s15`、`nl0914-shortinit-big-f30-s15`。Q/R没有重测这两个完整文件。上述是不同配置组合/时段的一次观察，不是单因子或统计认证。

## 6. 代码地图

- `apps/common/local_desktop_runtime.cpp/.h`：SenderFrameBuilder、graduation/repair预算、空间银行、CLI运行配置、全屏合成、ReceiverPipeline、test seam。
- `apps/common/sender_carousel_scheduler.cpp/.h`：原wire兼容调度预算及窗口。
- `apps/common/decoder_resume_store.cpp/.h`：预算约束动态活动位图及恢复校验。
- `apps/PixelBridgeEncoder/encoder_runtime_cli.cpp`、`apps/PixelBridgeDecoder/decoder_runtime_cli.cpp`：显式实验/分段/会话根选项；不自动打开GUI默认。
- `libs/PBModulation/src/unified_visual.cpp`：CPU灰阶前向采样及Bootstrap绑定geometry准入。
- `libs/PBDemodD3D11/src/{capture_demodulator.cpp,demodulator.cpp,unified_visual_compute.hlsl}`：真实Capture入口、直接GPU绑定和灰阶前向模型。
- `tests/PBApplication/test_unified_encoder_workflow.cpp`：N/O/R真实FEC无像素恢复、晚加入和预算反例；`test_runtime_bindings.cpp`：全屏生产合成逐像素/逐块。
- `tests/PBDemodD3D11/test_demodulator.cpp`：真实阶段Capture/GPU尺度正负例、全屏四种尺寸。
- `tests/PBModulation/unified_gray_resampling_fixture.h`、`unified_fullscreen_fixture.h`：独立有界测试生成器。

## 7. 验证记录与未关闭问题

- P2 Capture先红后绿；红退出42，原日志保留。正常源码回归：Gray13例2181断言、SC6 CPU18例495140、Capture/Unified GPU19例139184、shader1例71。
- Q全像素/CPU24组；GPU24组。最初GPU测试误用库128MiB而不是产品既有256MiB，4K拒绝；修正测试配置并新增预算不足拒绝后通过，没有提高产品预算。
- 最近Q受影响应用4例1521断言、Capture/Unified GPU20例141919断言通过。
- R新增恢复1例121断言、边界/首轮配置5例51断言通过，应用双端重新构建通过。没有本轮全量CTest/ASan全绿声明。
- **已知未关闭**：M late-join12505帧未达12000门；O初轮65%在1/4保留率会退化；旧扩展缩放矩阵仅49/64完整，通过样例不能覆盖失败相位；任意远控缩放/分辨率未认证。
- R现场两端已退出，原显示模式未变，liveProcesses=[]。Encoder包外WM_CLOSE退出被程序分类Failed/exit1、`DataWindow stopped without an explicit user stop request`，这是既有停止分类，不隐藏成正常Stop。Receiver完整文件exit0是最终恢复证据。
- R前台句柄端点不同，未执行输入或焦点操作，不能声称全程前台不变。
- 本版新commit/0.6.0二进制身份不同；发行检查只证明其明确覆盖的项目，旧候选的100MB/完整文件结果不能改名继承。

## 8. 远程桥复现及清洁基线

桥仅用于包、脚本、启动/停止及日志/元数据。**从不向Decoder传输像素文件或payload，也不根据Receiver状态在线调整Sender参数。** 人工/测试编排最终停止Sender不是生产协议ACK。

- 本机RECEIVER-DESKTOP，右屏物理ROI `DISPLAY2 [2560,0,5120,1440]`。
- 远端SENDER-LAPTOP/<user>，原模式2560×1600@240，工作根`C:\Users\<user>\AppData\Local\PixelBridgeOps`。
- 共享根`<ShareRoot>`；remote UNC在当前桥README。先读`docs/REMOTE_OPS_BRIDGE.md`与`tools/PBRemoteOpsBridge/README.md`，核对live身份、display-info、list-runs。不要执行display-set、输入或全局进程终止。
- 实验由`ROOT/field_run_short_initial_original.py`执行；`run_fullscreen_full_initial_100.py`已使用固定tag，不应原样重复。`replay_fullscreen_full_initial_100.py --tag <新tag>`是同配置的create-only复跑入口，先检查其guard验证记录；仍绑定历史Q包，不是v0.6包切换器。
- Receiver-first；远端从TestMedium自行生成100MB前缀；每run全新Session状态和本机输出目录；只有SHA/BLAKE3元数据过桥。120秒无进展保护，R总截止1200秒。
- 每轮核对同Session、源稳定/fresh、无resume、最终四门、第二次独立SHA、两端退出、原显示模式未变。不可只凭UI进度或中间服务状态算成功。

## 9. 下一步（本次不执行）

先读发布receipt和live状态，确认无残留进程和未预料diff，再继续非本机效率目标。不要再盲扫FPS或马上重跑整份Big。

候选方向：保持完整全屏，先做**test-only线性/面积采样对照**，覆盖canonical→远端全屏→独立远控缩放的两阶段链、分数相位、满载随机transport数据、CPU及真实Capture/GPU逐字节验证。过滤可能减少高频编码负担，也可能加重灰阶模糊；目前只是未验证假设，不允许调低验收阈值强过。通过后才建显式候选、正常封存，再≤25MB实屏；保留point参考实现和当前R首轮100%对照。

外部参考已读：Citrix官方Thinwire资源自适应说明（https://docs.citrix.com/en-us/citrix-virtual-apps-desktops/2407/graphics/thinwire.html）；Microsoft bilinear邻点加权说明（https://learn.microsoft.com/en-us/windows/win32/direct3d9/bilinear-texture-filtering）。它们只支持提出假设，不证明本会话具体codec/唯一瓶颈，更不授权改Citrix设置。详情见ROOT/candidate-r-fullscreen-cadence-native/RESEARCH_NEXT.md。

## 10. 快速证据索引

- 当前总日志：`docs/REMOTE_NONLOCAL_THROUGHPUT_SESSION_FINDINGS_20260911.md` §21.33–21.37。
- Q/R100MB：`ROOT/candidate-q-r-fullscreen-initial-100-comparison.json`及`candidate-q-r-fullscreen-100-journal-attribution.json`。
- 最新现场：`ROOT/nl0914-fill-gray100-fullinitial-f30-s6/`的report、result、independent-verification和对应stage。
- R离线：`ROOT/candidate-r-fullscreen-cadence-native/`；`run_native.py`的旧log输出create-only，直接Catch test可在新证据目录重新留档。
- 历史双文件：`ROOT/candidate-hclean-o-named-files-comparison.json`及O两个run。
- 发布最终身份：交付根中的manifest/seal、`DELIVERY_VERIFICATION.json`及`FINAL_HANDOFF_RECEIPT.md`，优先于本文件的准备阶段表述。
