# PAM4：独立 11 槽模式合同

**简体中文** | [English](EXPERIMENTAL_PAM4_PROFILE.en.md)


> 产品界面统一显示“标准 / 灰阶高速 / PAM4 / PAM4 Wide”，不带“实验”字样。下文 `Experimental`、`experimental-*` 等标识是已冻结的内部/CLI兼容名称，保留它们不代表界面或产品版本仍为实验版；验证范围仍按真实证据说明。

> 当前接口：双端 GUI 与 CLI 已接通，Decoder 支持显式单屏整屏/ROI 和有限内存预算。内部兼容身份、固定物理码面与下列数值合同不变。当前交付状态及实测以 [PROJECT_STATUS](PROJECT_STATUS.md) / [EVIDENCE_INDEX](EVIDENCE_INDEX.md) 为准；不把 Golden 当现场认证。

## 1. 目标、位置与身份

唯一目标是减少非本机条件下最终校验通过的整文件耗时，不以本机 FPS 或 codec 包体积替代。当前已验证远控环境为 ToDesk；完整文件证据及一小时目标未达的限制见 [证据索引](EVIDENCE_INDEX.md)。

- 名称 `PB-Experimental-Pam4-1`，`VisualProfileId=0x504250414D343031`，`VisualLayoutVersion=15`。
- 使用独立 `LocalDesktopBinding::ExperimentalPam4`，不是 Unified/GrayFast family 的别名。旧 15/18 槽入口继续拒绝它，`kProductVisualProfiles` 仍只有一个正式条目。
- layout 14 的旧 StructuredMultiCell 离线研究不复用、不改写；本次也不是把那一候选偷偷设为默认。
- 双端 CLI token `experimental-pam4`，GUI 显式选择 PAM4；不是默认模式。双方匹配同一身份，新 Session 由 OS CSPRNG 创建。
- 规范参数、完整 33-region 副本、映射/Golden 摘要在 [machine-readable manifest](../tests/golden/experimental-pam4/manifest.json)。实现入口为 [experimental_pam4.h](../libs/PBModulation/include/pbmodulation/experimental_pam4.h)。

## 2. 码面、容量与固定控制槽

规范画布仍为 1920×1080 BGRA8 SDR、alpha=255、中性背景128。复用现有 locator、两个 RS Bootstrap、九个 timing/freshness、四个 calibration 和两个 PhaseChecker 区域，所有非 Data/guard 几何均不挪用。Data 采用全局原点对齐的 4×4 均匀灰阶单元，只有完全位于 Data 内的单元可用；共 92,832 个，每个按 label 0..3 映射 `[0,85,170,255]`、LSB-first 两位。Data 边缘不完整单元保持128。

| 槽 | wire 对象 | Inner FEC | 信息字节 | 最大文件 payload |
|---|---|---|---:|---:|
| 0（固定且 active） | 一个 PB-Control-1 record | 既有 Robust，N=16200/K=10800 | 1350 | 0 |
| 1..10 | 一个 Transport Block，或 inactive 零 filler | 既有 Fast，N=16200/K=13320 | 1665 | 各1629 B |

Control envelope/schema/CRC、强纠错和描述符资源准入不交给 Data 参数推断；固定首槽直接决定 Robust，而不是尝试多个 FEC 后挑一个能过 CRC 的结果。最多1300 B 的正式 descriptor 加30 B envelope仍在1350 B内。Stage 2 scheduler 必须显式选择 `SenderMixedSlotLayout::ExperimentalPam4` 且精确11槽；仅填写11槽不能越过旧模式准入。

启动控制 burst 仍按 Session→Manifest→Segment 交错四次；周期控制沿用既有单调时钟节奏。只有一个控制槽，因此携带 Session/Manifest 的帧中十个 Transport 槽为 inactive 零 filler，不消耗 equation/repair ID；携带数据的帧，其唯一控制槽始终是当前 SegmentDescriptor。零字节 Session 只有 Session/Manifest。该本地调度选择保留 descriptor-before-data，未通过 PBBridge 或其它通道告诉 Receiver 调度计划；Receiver 仍独立从像素、CRC 和 typed state machine 判断。满帧信息容量为1350+10×1665=18000 B，不能错误地按11个Fast信息块或11个payload计吞吐。

11 个码字共178,200 bit，容量185,664 bit，余7,464位固定0。满载十个 Transport 的**上界**为16,290 B/frame；丢帧、Outer FEC、控制周期、访问重复与最终校验时间尚未扣除。先前 artifact 的控制槽也用 Fast，本合同改为固定 Robust，故先前6,864码字与10.88 ms证据不能直接充当本身份的完整认证。

## 3. 不变映射与 Golden

`experimental_pam4_mapping.h` 是只读生成表，不是 payload。cell sites 为全局 `row*480+column`；每个全局逻辑码字位映射到 `cellOrdinal*2+plane`。表值来源于此前隔离候选的已验证排列，**规范是完整表值，不是 NumPy PRNG 的隐式运行时行为**；本机/远程构建不依赖 NumPy，不重新随机排列。没有 FrameSequence permutation。

- 只读数据1,084,128 B；cell sites LE32 SHA-256 `c9d75bc0e0cf39b7b894857e23d1aff348c8d3b74d77ab9d38bd5d5cea953333`。
- bit positions LE32 SHA-256 `efda94191dbe0976baa921d8e29ae44a47ab8cf29d8f71f2ddff70ca72c65164`。
- 原型 provenance：NumPy1.26.4/PCG64，seed `0x50424D434643`；后续只读表逐字节冻结，不以不同版本 PRNG 代替。
- 新 Golden 在独立身份首次闭环后冻结：`test_experimental_pam4.cpp::MakeFixture` 的固定 SessionId、真实 SessionDescriptor、十个不同 Transport、FrameSequence3267、ControlSequence7。BLAKE3 为 raster `02cd7469aaf4c85ba8d70a0006bc915d2fd533f0b29ef246b7b36f8e8054b7b9`、coded frame `8f3d75fd41f1c1ab891e35ecf2d54e24ccc9924741c19089e4302e47fe14eabc`。
- Golden 防止后续无意改变本模式码面；它不是现场信道认证，也没有重写旧模式的任何 Golden。

## 4. 像素接收与有界资源

`ExperimentalPam4CpuDecoder` 单 owner，公开输入仅 borrowed Gray8/BGRA8 SDR `LumaView` 与可选的已知身份约束；支持 rowPitch，不读取 padding，不接受 HDR/其它格式的隐式转换。没有整图 resize 或全帧复制。Common locator 要求四个角色 marker、双 Bootstrap 完全一致、精确新 Profile/layout，保留原 timing 强门；**不使用 Unified 的局部 timing 失败豁免**。本模式还复核九区 freshness，任一不合格整帧擦除。

上一帧像素测得的几何只用于有界重定位提示；每帧仍检查 marker、Bootstrap 和 freshness，失败回 full-ROI。任何 frame erasure清缓存；capture epoch/ROI/设备变化必须由 owning capture adapter显式 `Reset()`，Stage 2 已接入域启动/失效的 Reset。命中时没有全 ROI 第二幅完整码面的歧义搜索，与现有定位缓存合同一致；不宣称与每帧全搜在所有场景等价。

可见校准由四区域粗灰阶 `[8,64,160,232]` 的局部中位数拟合，端点只做有界线性外推；细灰阶 `[56,112,168,248]` 与128中性条纹是 held-out 验证，不能反向调参追逐 payload 真值。空间偏差/held-out残差≤8，粗灰阶及推算四级间隔≥16，总对比度≥96。数据在4px cell中心按实际几何双线性取样。BGRA使用 `(722*B+7152*G+2126*R)/10000`，中性RGB与Gray8保持同值；max-log软度量、16384缩放、±32767裁剪、ties-to-even。MSVC该源文件显式 `/fp:precise`，不修改进程浮点环境。PhaseChecker区域仍渲染既有模式，但不把旧 glyph 的锐化/相位接纳方法冒充为 PAM4 检查；实际几何来自独立四 marker 与 Bootstrap。

实例一次分配固定物理/逻辑度量、一个码字 scratch、11份有界输出、两个复用 FEC workspace。`RequiredBytes()` 是 `sizeof(Implementation)+2 MiB` 的保守 reservation，创建前检查，不是RSS实测。采样 reader另外限制1,000,000次像素读取；locator保持已有24,000,000 work-unit上限，freshness为固定九区有限循环。FEC最多12轮、offset2048、scale1/1；这些是局部实现选择，不改FEC矩阵。

所有接受块继续通过CRC、规范零填充、槽类型和像素SessionTag。模块只接受控制 envelope，**不能替代 ControlPlaneReceiver 的 typed/schema/resource/conflict 准入或应用的固定 Profile/Session绑定**。文件路径继续必须经过原 ReceiverIngress、Outer FEC、Segment摘要、whole-file摘要、安全publish和reopen；不得以此API的`acceptedBlocks`发布文件。

观测类型独立为 `ExperimentalPam4Observation`，不能隐式交给旧15/18槽的lane遥测/分母；不报告不存在的Fine/Chroma lane。全帧擦除和逐槽FEC/CRC拒绝明确分开；Stage 2 接入独立观察计数，Stage 3 已完成真实入口的 live CPU capture 成本汇总并在右屏记录。

## 5. 应用接入与兼容边界

- 接收端仅以实际捕获的像素恢复。PAM4 为 CPU reference readback 路径，独立计数，不伪造 Unified lane。
- Sender GUI 使用 7 MiB Segment、固定 point 呈现，实际发送屏必须可容纳整个码面。
- Decoder 的 `--single-monitor-capture DEVICE` 与旧 protected/experiment 双屏权限互斥；GUI 无需保护屏，不预留 Decoder 界面面积。
- 单屏入口是 live RemoteVisual/Auto；不借此开放 Replay、capture-only 或正式 measurement。
- 资源预算、descriptor/conflict、摘要/安全发布/最终重开不变。参数详见 [预算](DECODER_MEMORY_BUDGET.md) 与 [当前路线](PixelBridge_最终技术路线与总体设计.md)。
- 最新日志/UI变更不产生新的 Profile/Golden。原始源表与 machine-readable manifest 为逐字节权威，不用历史阶段文字覆盖它们。
