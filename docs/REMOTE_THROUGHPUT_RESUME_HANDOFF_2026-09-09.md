# 非本机吞吐优化：暂停交接与恢复入口

> **最新状态：用户要求停止本次任务，已停止新增实验，仅完成收尾。**
> 最后完成并封存的工作为夜间记录第22节；第23节的4/3显示适配只做了只读核对，**未实现、未构建、未运行**。
> 此文件是下次继续工作的导航，不是自动恢复执行的授权。只在用户重新开启目标或明确要求继续后推进。

## 1. 三十秒理解现状

- 总目标没有变化：**提高非本机条件下的整文件有效传输速度**。不是追求本机FPS、理论每帧容量或更多绿色检查。
- 通路始终为 Encoder画面→远控显示→Decoder实际捕获像素→文件恢复；无隐式ACK和其他payload旁路。
- 最近人工现场使用向日葵，远端Encoder、本机Decoder捕获右侧屏幕。远控品牌只是元数据，不选择解码分支。历史现场比例约4/3、ROI2560×1440；未来实屏前必须重新核对，不能把这条历史信息当当前拓扑。
- **尚无已确认的新非本机提速收益。** 接收端CPU成本优化已提交入库（Bootstrap顺序扫描游标 `bf1697e`、QC-LDPC每行归一化复用 `a3eae50`）并有本机/离线验证，但没有新现场收益认证。空白带是独立实验，没有进正式现场包。
- 当前主线是利用用户指出的右上/左下空白区域，且右上可能被部分覆盖。优先独立补充控制，不放唯一必需信息，不先撤主控制。
- 停止时无本轮仍待等待的native会话；PBBlankCodec/PBBlankFullFrame/PBFecRowReplay进程查询无输出。没有为新4/3实验创建源码/构建/结果目录。

## 2. 当前仓库与保护边界

仓库：`<repo>`。

HEAD：`8f74cd7af7895c8a1473d0fa05f9201224503954`。2026-09-09 用户另行要求的仓库整理已把暂停时工作树中累计的改动按主题入库，共 9 个提交（`6eb4563`…`8f74cd7`，基线 `4a36d0f1b84c94c7388a58b8249a3f1b6ec0634a`）。`git status --porcelain -uall` 现只剩受保护的 `docs/PHASE1_GATE_REPORT.md` 未跟踪，index 为空，工作树与 HEAD 逐字节一致；仓库无 remote，未推送、未重写历史。

**提交只改变记账位置，未改动任何源码字节**，因此不能据此升级任何证据身份：旧交付包、录像、raw 像素与实验封存目录仍按封存时记录的 build 身份解释，不得用新 HEAD 重新解释；`--measurement-build-identity` 输出的 `baseCommit` 来自 CMake 配置期 `git rev-parse HEAD`，`sourceFingerprintSha256` 来自 `PB_STEP1_SOURCE_FINGERPRINT` 缓存变量，两者都是编译期注入，只有重新构建后才会反映新 HEAD。若要以当前源码身份作为新的对照基线，必须重新 freeze、build、test、package、seal。

正式产品：`PB-Unified-SC6-V3`，ProfileId `0x5042554E49534333`，layout10。空白带独立实验：`PB-Experimental-BlankControl-1`，ProfileId `0x504242414E443031`，layout11；正式catalog拒绝新实验身份。

以下边界继续保留：

- 不修改、暂存或提交 `<repo>\docs\PHASE1_GATE_REPORT.md`。该文件仍未跟踪，不是待清理垃圾；SHA-256：`076ef4c9b9f89eabccd323dbe4bffc4dc125ddaf96e6ee437d2cf5b1b1cea306`。
- 不删除、覆盖或搬走旧交付包、raw像素、录像、源文件、build、实验记录；所有新实验用新root/build/output，create-only历史driver不可原地重跑。
- 不引入旁路/ACK，不削弱摘要、安全发布、reopen、冲突拒绝、资源及GPU生命周期规则。
- 不自动调整向日葵/Citrix/网络/显示设置，不做焦点或输入自动化。以前的睡前授权不代表现在用户停止后仍可继续实屏；恢复后先确认当时任务范围并核对拓扑。
- 不创建子智能体、不委派新任务。普通验证只跑受影响检查，不默认完整回归/矩阵/压力测试。
- 用户已让代理自行处理合理技术细节；不要重复询问编码参数。真正无法由现有证据解决的高影响决策仍按当时有效交互规则处理。

## 3. 已有改进和证据，哪些不能当作现场提速

| 工作 | 当前状态 | 关键边界 |
| --- | --- | --- |
| 控制相位A/B现场对照 | A1 238.3155474s；B1 211.5549573s；B2 212.477132s；A2 205.0048921s | 反序观察不支持稳定B优于A，且A2Encoder残留后被人工结束，退出/预算不合规；不晋级默认 |
| Bootstrap扫描游标 | `libs/PBModulation/src/local_desktop_decode.cpp` 的改动已提交于 `bf1697e`；两个固定CPU对照中位成本约下降12.2%/9.6% | 仅扫描阶段，不是远控吞吐百分比；49个受影响用例及原WGC整文件接线已验证 |
| QC-LDPC每行归一化复用 | `libs/PBInnerFec/src/qc_ldpc_codec.cpp` 的改动已提交于 `a3eae50`；固定75组软判决的阶段均值1.8984→1.4471ms，约下降23.8% | 675对输出/状态/迭代一致；40个相关用例通过，不是远控速度提升23.8% |
| 新扫描＋新FEC组合 | 第19节实际WGC两段8454144B，39.4210093s，摘要/发布/reopen及外部双摘要通过 | 本机无codec，FEC迭代0；没有替换现场包 |
| 空白带短控制原型 | 第20节完整121/194/114B控制记录；61组检查及独立像素/RS/CRC审计通过 | 只证明短原始patch；不是文件修复方程，不支持所有合法长度控制 |
| 空白带原始整帧 | 第21节30帧×5组（基线、无遮挡、右上/左下/双遮挡），256KiB整文件均通过 | 主区记录一致、都在ordinal14完成，无提速证据；CPU reference＋原Ingress/Storage，不是原应用/GPU |
| 空白带固定codec | **第22节完成并封存，两组均NOT_RECOVERED** | 见下一节；不把有效Bootstrap数增加当吞吐 |

更多历史结果按节查看：`<repo>\docs\REMOTE_OVERNIGHT_THROUGHPUT_2026-09-09.md`。文档中的“下一步”是各历史时点记录，**本交接的停止状态和第5节优先**；不要机械执行旧第4/7等节的计划。

## 4. 最后一个完整里程碑：第22节

证据根：`<repo>\artifacts\remote-blank-codec-20260909-run01`。

独立build：`<repo>\build-remote-blank-codec-20260909-run01`；实际exe：`Release\PBBlankCodec.exe`。

封存308文件、26,918,995B；本次暂停收尾再次只读复验通过。复验输出在本次新收尾目录，没有改写旧封存包。

### 4.1 固定参数和恢复行为

- 原始输入复用第21节`generated\baseline.bgra` / `candidate.bgra`，各248832000B；同Session/同主区，只两条带不同。源262144B，Wirehair K200/1314B。
- libx264 High4.0、veryfast/zerolatency、单线程、1920×1080、30帧/15fps/2秒；8Mbit target/min/max、VBV4Mbit/init0.9、GOP15/IDR0与15、无B/ref1；原BGRA-full→BT709-limited/yuv420p/left。实际全量argv在`logs\*-encode.process.json`。
- 直接原RecordingMedia解码→pbblankvisual CPU参考→原ReceiverIngress/Storage；**没有原DecoderRuntime/GPU/WGC/GUI/resume**。
- 新宿主正确处理控制晚到：非Session控制UnknownSession等待；Session建立前数据不进orphan缓存；分段commit后仍等实际FinalManifest才发布。两个raw回归与第21节trace/result精确一致，独立晚到guard整文件通过。
- 主区每帧仍1个完整控制槽＋14个数据槽；补充带独立、匹配实际父Bootstrap会话/帧，不传source/descriptor/真值给Receiver。

### 4.2 真实失败与新发现

| 指标 | 无补充A | 有补充B |
| --- | ---: | ---: |
| 码流属性/PTS/HRD/包字节VBV检查 | PASS | PASS |
| 可接受Bootstrap | 15/30 | 30/30 |
| 定位失败 / CanvasClipped | 15 / 15 | 0 / 30 |
| 主帧可用、主FEC迭代、Outer接纳 | 全0 | 全0 |
| Session、最终文件 | 无 | 无 |

`CanvasClipped`是原几何准入拒绝枚举；本轮没有记录具体浮点geometry，不能声称真实ROI一定被裁剪。1:1模型复现此前的几何限制，在主数据/FEC之前就停了，因此不能评价补充带是否拖累主区。

每帧保存两个608×64局部patch，候选共60条。独立Python采样发现21条硬判决码字与raw精确一致，每条错误最多6byte，但当前“任一96<luma<160单元即丢整条”的条件仅3/60条通过，57条被拦截。这只是离线真值诊断，**没有执行RS纠正、没有绕过父帧接纳，也不是非本机收益**。

### 4.3 快速复验和权威文件

```powershell
Set-Location -LiteralPath '<repo>'
& '<python>' -B -X utf8 'artifacts\remote-blank-codec-20260909-run01\verify_saved.py'
```

只读，不启动native、不编码、不碰屏幕。使用冻结helper/编译输入和运行身份；不要强制未来工作树等于旧SOURCE_BEFORE。

优先读：`CONTRACT.md`、`CODEC_RESULT.json`、`PATCH_DIAGNOSTIC.json`、`AUDIT.json`、`RUNTIME_IDENTITY.json`、`REPLAY.md`。两个输入负例和五个inspector负例已通过；17个子进程记录包含2个预期exit1。诊断exe exit0只说明诊断正常结束，不代表恢复成功。

## 5. 下次恢复：准确从这里开始，不重做前面实验

**第一项仍是显示采样适配，不是改码率/调门限/重编码。**

1. 阅读本交接，检查live HEAD/status/index/保护文件和上节封存状态；明确没有本轮活跃进程需要继续等待。
2. 阅读第22节宿主与第5节已验证4/3显示适配。创建全新的实验root/build；从第22节源码派生，保留旧源码和结果。
3. 复用已有两份codec文件，不重编码、不修改任何Session/帧身份；单独改变输入显示变换为2560×1440的4/3。这只是匹配现场观察比例的确定性模型，实际向日葵缩放核仍未知，不能声称完全模拟远控。
4. 对新增补充带采用**原主区已验证后的geometry**采样，而不是将已知4/3比例反填为检测结果。使用原`ResolveUnifiedVisualSamplingGeometry`准入，保留主区定位/pilot/FEC/CRC/资源规则。采样的坐标、插值/取整和边界需要在新实验合同明确并有raw/边界/身份对照。
5. 保留原1:1参考入口作为回归；先证明缩放raw主区/控制/整文件不被适配破坏，再对已有两份codec执行单组接收诊断。记录实际geometry、父帧拒绝、slot、补充带和原Receiver最终状态，避免只得到宽泛枚举。
6. **不要同时修改短带置信度或RS算法。** 先隔离几何采样变量；如仍被短带置信度条件限制，再评估有明确字节擦除预算的独立候选，不能直接降门限。
7. 主区完整控制继续保留。通过参考/codec只是筛选；后续仍需接入原应用/GPU和最终非本机场景整文件验证，不能把工具CPU路径认作产品默认已完成。

本次停止前只读了相关代码，尚未选择和实现新的补充带采样函数，没有新4/3实验root或启动脚本。

### 关键代码与可复用资产

- 第22节宿主：`<repo>\artifacts\remote-blank-codec-20260909-run01\source\main.cpp`、`source\media_replay.inc`、`source\band_primitive.inc`、`source\CMakeLists.txt`。
- 已验证4/3确定性缩放：`<repo>\artifacts\remote-fec-row-integration-20260909-run01\source\scale_four_thirds.inc`；同目录`main.cpp`中的`ScaledSource`展示包装接法。
- 原采样/几何实现：`<repo>\artifacts\remote-blank-fullframe-20260909-run01\source04\visual\src\unified_visual.cpp`中的`ReadSample`、`ResolveUnifiedVisualSamplingGeometry`；对应`visual\include\pbblankvisual`头文件。
- 继续复用第21节14个实验object，列表和hash在第22节`EXPERIMENT_OBJECTS.json`。不要重编巨大视觉TU来做宿主改动。
- 原接收和存储库基线：`<repo>\build-remote-throughput-observation-20260909-run01`。FFmpeg媒体依赖：`<repo>\artifacts\remote-step2-20260908-run01\deps\installed\x64-windows`。以运行身份和MAP为准，不猜当前某个build已带最新CPU优化。

## 6. 不要重新走的路径

- 同一1:1 codec几何失败入口：不得加帧、改码率或绕门制造成功；下一变量已经明确为显示适配。
- H2保持的首GOP小文件成功不代表持续收益，后启动检查已失败，不能据此把实际15Hz降到7.5Hz。
- held-neutral候选在原弱模型可恢复，但按既有B2现场约190186B/s和有效Chroma数据，其理想稳态容量不足；已停止当前现场提速晋级。不要再做40MB/900s中和实验。
- 原始空白区面积约3.75%，直接延长现有carrier仍不足一个完整16200bit码字；可能释放控制槽的7.14%仅是普通帧理想容量，不是实测整文件收益。
- 第21节巨大`metrics{}`聚合初始化曾导致MSVC前端超过2GiB。拆TU或关闭优化均无效；`source04`默认初始化且各字段有DMI后已验证/O2通过。不要重复OOM探针或提高资源预算。
- B2/A2等旧现场证据不重新分类为新候选现场PASS；A2人工结束残留进程的失败记录不能抹掉。

## 7. 暂停收尾证据与恢复提示词

收尾目录：`<repo>\artifacts\remote-throughput-pause-20260909-closeout01`，保存暂停请求状态、Git快照、保护文件hash、上一里程碑只读复验、新文档快照和收尾清单。没有移动/删除历史文件，没有自动提交。

2026-09-09 整理补充：同一日按用户单独要求执行了目录整理与文档记账，`6eb4563`…`8f74cd7` 共 9 个提交只暂存显式路径，未移动、改名或删除任何历史 artifact、build、录像或封存包；`docs/PHASE1_GATE_REPORT.md` 仍未跟踪且 SHA-256 保持 `076ef4c9b9f89eabccd323dbe4bffc4dc125ddaf96e6ee437d2cf5b1b1cea306`。整理未执行任何构建或测试，因此不构成新的验证结论。

建议下次用户消息：

> 继续以目标模式优化非本机整文件吞吐。先阅读 `<repo>\docs\REMOTE_THROUGHPUT_RESUME_HANDOFF_2026-09-09.md` 并核对当前工作树和封存证据，然后从第5节的“复用已有两份码流，做4/3显示采样适配”继续。不要重编码或重跑已否定的路径，不改主控制和安全门，不自动提交，不创建子智能体。先给出简短执行范围，再做受影响的有界验证。

**总体目标未完成；此次是用户主动停止，不是技术阻塞，也不是提速任务完成。**
