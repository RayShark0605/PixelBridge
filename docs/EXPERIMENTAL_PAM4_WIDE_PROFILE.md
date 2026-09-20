# PAM4 Wide：独立 20 槽模式合同

**简体中文** | [English](EXPERIMENTAL_PAM4_WIDE_PROFILE.en.md)


> 产品界面统一显示“标准 / 灰阶高速 / PAM4 / PAM4 Wide”，不带“实验”字样。下文 `Experimental`、`experimental-*` 等标识是已冻结的内部/CLI兼容名称，保留它们不代表界面或产品版本仍为实验版；验证范围仍按真实证据说明。

> 当前接口：双端 GUI 与 CLI 已接通，Decoder 支持显式单屏整屏/ROI 和有限内存预算。内部兼容身份、固定物理码面与下列数值合同不变。当前交付状态及实测以 [PROJECT_STATUS](PROJECT_STATUS.md) / [EVIDENCE_INDEX](EVIDENCE_INDEX.md) 为准；不把 Golden 当现场认证。

## 1. 目标与兼容性

目标仍是非本机纯视觉单向传输的**最终正确文件完成时间最短**。当前验收环境是右屏ToDesk，不改远控/网络/显示设置，不操作左屏或输入。扩大每帧容量需要同时承受视频编码成本，收益必须由后续整文件digest/publish/reopen后的时间验证。

- 独立名称：`PB-Experimental-Pam4-Wide-1`。
- `VisualProfileId = 0x504250414D345731`，`VisualLayoutVersion = 16`。
- 独立`LocalDesktopBinding::ExperimentalPam4Wide`和`ExperimentalPam4WideCpuDecoder`，不把layout15/GrayFast重命名为新模式。
- 正式`kProductVisualProfiles`仍只有`PB-Unified-SC6-V3`。旧11槽PAM4、旧15/18槽Unified/Gray的数组、语义与Golden不变；正式目录和 CLI/core 的默认选择边界不变。
- 双端 CLI token `experimental-pam4-wide`；GUI 新安装时默认是 PAM4（索引 2），PAM4 Wide 仍需选择索引 3，且已保存的合法索引继续原样恢复。GUI 默认不改变 Wide 的独立 Profile 身份、正式目录或 CLI/core 的显式 opt-in 边界。不同身份必须建立新 Session，不自动回退。
- Bootstrap/Control/Transport/Descriptor格式、FEC矩阵、SessionId生成与文件恢复不变量不变；不同Profile必须是新Session，不支持中途改变码面。

## 2. 逻辑码面与固定呈现

逻辑画布仍为1920×1080 BGRA8 SDR，alpha255、背景128。Marker、两个RS Bootstrap、九个timing/freshness、四个calibration、两个PhaseChecker及所有保留/guard区域不被Data侵占。

Data采用全局原点0,0的**逻辑3×3像素**灰阶cell，仅完整落在Data内的cell可用；共165192个，row-major sites为`row*640+column`。每cell两位，label0..3对应`[0,85,170,255]`，LSB-first。partial Data cell为128，未映射位固定0。

设计呈现宽高固定为2560×1440，通过像素中心point采样生成，在不改变桌面分辨率的前提下居中呈现，剩余边缘静态128。公式为：

```text
sourceX = floor((2*x+1)*1920/(2*2560))
sourceY = floor((2*y+1)*1080/(2*1440))
```

因此3个逻辑像素精确展开为4个源桌面物理像素。**不是3px物理单元实验，不允许任意native-size、任意viewport或linear/area插值悄悄代替。** Stage28已用原样当前runtime的实际点合成函数，证明该表示与扩展4px参考逐像素相同；这不证明GPU shader、现场远控内部缩放或任意屏幕设置。

核心输出canonical画布；Stage30应用配置已强制固定point、2560宽和目标屏可容纳2560×1440。CLI省略宽度时填2560，显式仅接受`--fullscreen-raster-width 2560`，其它宽度/native-size/显式采样实验拒绝；API配置必须明确2560。实际全屏窗口使用当前ComposeRemoteVisualFullscreenBgra并报告width，源屏幕不够大时拒绝，不改变桌面设置。

## 3. 固定槽位与只读位表

| 槽 | 内容 | Inner FEC | 信息字节 | 文件payload上界 |
| --- | --- | --- | ---: | ---: |
| 0 | 一个active PB-Control-1 | Robust N16200/K10800 | 1350 | 0 |
| 1..19 | Transport或inactive零filler | Fast N16200/K13320 | 各1665 | 各1629 |

20个码字为40500B/324000bit，物理标签总容量330384bit，余6384bit固定0。满载文件payload上界为30951B/frame，不含控制等待、Outer FEC开销、视频/捕获丢失或最终校验耗时。

`ValidateExperimentalPam4WideSlotPlan`要求精确20个唯一槽，固定首槽Control及合法priority，其余仅Transport/NotApplicable。11/18/19槽不是可变长有效帧；inactive Control、非法kind/priority、冲突slot、错误CRC/SessionTag和非规范padding不能被弱化。

规范为[完整只读表](../libs/PBModulation/src/experimental_pam4_wide_mapping.h)的数值，不是运行时PRNG。表共1956768B：

| LE32序列 | 数量 | SHA-256 |
| --- | ---: | --- |
| cell sites | 165192 | `0acdedd881cf37b255ef7765ea9a5913aaa1fa54155dbd814bc0c2ffaed7e8d3` |
| bit positions | 324000 | `55ff14d0cd71246fd5ccf98935df3b865c7a0e10346e90685f660b594479703a` |

表来自Stage25冻结排列，和Stage27原生表逐值相同；无FrameSequence permutation。规范全量region副本、FEC身份、校准门与Golden见[machine-readable manifest](../tests/golden/experimental-pam4-wide/manifest.json)。

## 4. 完整原生接收核心与资源

输入仅borrowed Gray8/BGRA8 SDR像素及可选已知身份约束；无源payload、发送调度、桥状态或外部几何输入。完整原生路径包含：双Bootstrap定位/精确新identity → 全画布边界 → 九区freshness → 可见校准/held-out检查 → 双线性采样/软度量 → 固定Robust/Fast纠错 → CRC/零padding/SessionTag接纳。

旧Unified局部timing豁免没有开放给新模式。任何frame erasure清除几何缓存、接受块与度量可用标志；capture epoch/ROI/device改变时owner仍须Reset。旧帧的已接受几何只是有界定位提示，不提供payload真值。

校准沿用严格的四区粗灰阶`[8,64,160,232]`、held-out细灰阶`[56,112,168,248]`及中性128；粗阶/推算PAM4阶间隔≥16、总对比≥96、空间偏差及held-out残差≤8。数据采样中心为`(3*column+1.5,3*row+1.5)`。

新的独立模式采用Stage25/27参考的归一化：`denominator=1.5*median(adjacent captured PAM4 centroid gaps)`，max-log距离差乘16384、裁剪±32767、ties-to-even。分母仅由可见校准决定，不使用数据真值，且校准间隔门保证它严格正。**旧layout15固定127.5分母未改变。** 新源文件在MSVC显式`/fp:precise`；FEC最多12轮、offset2048、scale1/1不变。

每实例固定物理度量、逻辑度量、20份接受块、一个scratch及两个复用FEC workspace；`RequiredBytes=sizeof(Implementation)+2MiB`，创建前检查。采样预算仍1000000次读取，20槽最主要Data采样为660768次，没有无限队列/重试/分配。单owner，热Decode不引入整图resize或大帧复制。

新观测类型独立于Unified15/18槽，未改共享数组上界来容纳它。这个模块的accepted blocks只证明局部FEC/CRC，不能替代typed Control resource/conflict、Outer FEC、全文件摘要、publish或reopen。

## 5. 应用接入与兼容边界

- 接收端仅以实际捕获的像素恢复。PAM4 为 CPU reference readback 路径，独立计数，不伪造 Unified lane。
- Sender GUI 使用 7 MiB Segment、固定 point 呈现，实际发送屏必须可容纳整个码面。
- Decoder 的 `--single-monitor-capture DEVICE` 与旧 protected/experiment 双屏权限互斥；GUI 无需保护屏，不预留 Decoder 界面面积。
- 单屏入口是 live RemoteVisual/Auto；不借此开放 Replay、capture-only 或正式 measurement。
- 资源预算、descriptor/conflict、摘要/安全发布/最终重开不变。参数详见 [预算](DECODER_MEMORY_BUDGET.md) 与 [当前路线](PixelBridge_最终技术路线与总体设计.md)。
- 最新日志/UI变更不产生新的 Profile/Golden。原始源表与 machine-readable manifest 为逐字节权威，不用历史阶段文字覆盖它们。
