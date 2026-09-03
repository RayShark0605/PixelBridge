# PixelBridge 统一视觉协议与超大文件双端恢复实施路线

> 文档性质：当前产品路线、Goal/目标模式执行手册、集成检查点与最终验收清单
> 路线版本：1.0
> 冻结日期：2026-09-03
> 代码起点：`1445f9b`（正式 Descriptor 与可恢复 Segment 基础）
> 目标程序：`PixelBridgeEncoder.exe`、`PixelBridgeDecoder.exe`
> 适用平台：Windows x64 / C++20 / Qt Widgets / D3D11
> 当前结论：**基础协议、流式发送和恢复存储已经落地一部分；统一视觉 Profile、完整产品接线及最终实屏门禁尚未完成。**

---

## 0. 如何使用本文

本文不是愿景清单，也不是要求一次性执行的超大任务。后续每次进入“目标模式”时，只选择一个编号目标 `G00..G22`，核对前置条件、限定修改范围、完成该目标的最小验证并单独提交。除本文明确标为“集成检查点”或“最终检查点”的目标外，不运行全量 CTest、不启动真实屏幕窗口、不执行长时大文件回归。

仓库文档导航见 [`README.md`](README.md)，首次推送 GitHub 的源码/许可/remote/artifact 检查见 [`GITHUB_PUBLISH_CHECKLIST.md`](GITHUB_PUBLISH_CHECKLIST.md)。

本文解决四个实际问题：

1. 把已冻结的产品决策转成有依赖顺序、可审查、可提交的实现单元；
2. 明确当前代码已经具备什么，避免重复实现或把历史实验入口误当成最终产品；
3. 给每个目标规定最小但足够的测试预算，避免“改一个函数就全量回归”；
4. 明确哪些证据才能关闭目标，防止把编译、CPU oracle、WARP 或 receiver-only 结果扩大解释为真实产品完成。

### 0.1 文档优先级

发生冲突时，按以下顺序处理：

1. 当次用户明确要求；
2. 根目录 `AGENTS.md`；
3. 本文冻结的产品合同与目标边界；
4. `docs/PixelBridge_最终技术路线与总体设计.md` 中未被本文明确取代的通用架构、安全与生命周期约束；
5. Golden、测试与当前实现。

旧文档中关于 Phase 1.5 单 Segment、手选 Profile、固定窗口、RemoteVisual `1..5 Hz`、Step 22 campaign 的内容继续作为历史实现与证据记录，但不是新产品合同。不得为了兼容旧 GUI 行为而破坏本路线已经冻结的正式 wire、统一 Profile、`1..60 Hz`、多 Segment 或双端恢复要求。

### 0.2 每次 Goal 模式的固定开场

每次开始一个目标前必须：

```powershell
Set-Location -LiteralPath D:\MyProjects\PixelBridge
Get-Content -LiteralPath .\AGENTS.md
git status --short --branch
git log -5 --oneline
```

然后只读取该目标列出的设计章节、公共接口和邻近测试。若工作区存在无关修改，先识别所有权并避开；不得覆盖、暂存或提交 `docs/PHASE1_GATE_REPORT.md`，除非用户日后对该文件给出明确指令。

### 0.3 每次 Goal 模式的固定收口

1. 只构建受影响 target；
2. 只运行该目标列出的测试或同等窄范围检查；
3. 执行 `git diff --check`；
4. 审查本目标 diff 的边界、算术、资源、生命周期、错误路径和协议兼容性；
5. 用显式路径 `git add -- <paths...>`，不得使用 `git add .`、`git add -A`；
6. 创建非 amend 的单独提交；
7. 报告：结果、关键证据、测试命令、未执行门禁、提交哈希、下一目标；
8. 如果退出条件没有全部满足，则报告 `PARTIAL`，不得把“能编译”写成 `DONE`。

---

## 1. 冻结的最终产品合同

### 1.1 Encoder

- 基础界面只提供源文件、逻辑刷新率 `1..60 Hz`、开始和停止，默认 `15 Hz`。
- 开始后先完整预扫描源文件；预扫描成功后才打开编码窗口。
- 编码窗口是普通、可拖动、可缩放的 Windows 窗口，规范编码 raster 始终为 `1920 x 1080`。
- 显示层只做 point-sampled 等比缩放与居中 letterbox，不改变规范 raster 内容。
- 用户运行中修改 FPS 时，新频率只从下一张完整逻辑帧生效。
- 逻辑帧更新与 Present 分离：同一 immutable raster 可以重复 Present，但不会推进 `FrameSequence`、`OuterBlockId` 或 Carousel。
- Encoder 持续循环广播，不依赖 Decoder ACK，不显示接收百分比或“发送完成”。
- 崩溃或重启后，只有源文件身份、内容、descriptor 和依赖基线全部一致时才继续原 Session；否则创建新 Session。

### 1.2 Decoder

- 基础界面只提供输出目录、ROI、开始和停止。
- 不要求用户指定 Encoder FPS、Visual Profile 或捕获 backend。
- 默认先尝试 WGC；仅在明确初始化失败、`AccessLost` 或设备重建失败时切换 DXGI，并创建新的 `CaptureEpoch`。
- 捕获按 backend 可用速率运行；根据唯一 `FrameSequence` 去重，不用 Encoder 的设置 FPS 假装接收速率。
- 首个有效正式 Session 建立后锁定它；其他 Session 不与当前状态合并。
- 暂时没有可解码画面时保持 Waiting/Stalled，不结束、不清空已验证状态。
- 所有 Segment 完成后计算 whole-file BLAKE3；只有摘要通过、安全发布并重新打开复验后才显示 Completed。
- 重启后从 `.part` 和持久状态恢复；最多恢复 4 个活动 Segment，持久 equation cache 总额不超过 256 MiB。
- 完成界面显示最终路径、大小、摘要和“打开目录”，不再弹出“另存为”。

### 1.3 文件与资源边界

- Segment target 固定为 `8 MiB`，总文件变大时不增大单 Segment。
- 产品不设置 20 GB 硬上限；默认服从 `ReceiverResourcePolicy` 的 `500 GiB` 上限。
- `20 GiB` 是必须验证的最低大文件能力，而不是最大值。
- 8 MiB Segment 下，500 GiB 为 64,000 个 Segment，必须保持在协议和资源上限内。
- 支持 0-byte 文件：`SegmentCount=0`，但仍需要正式 Session、FinalManifest、空文件 BLAKE3 与安全发布。
- 大于 4 GiB 的输出预分配只询问一次明确确认；拒绝后不得创建大 `.part` 或开始接收数据。
- 发送端内存为双 Segment 缓冲，接收端为最多 4 个活动 decoder；内存不得随总文件大小线性增长。
- 禁止 sockets、pipe、共享内存、COM、剪贴板、window message、临时文件交换或其他 payload 旁路。

### 1.4 正式 wire

- 正式产品只接受 Protocol 1.0 Descriptor Schema 1。
- Session、Segment 和 FinalManifest 均采用显式 little-endian、显式 schema/header/total length、内层 CRC；Control envelope 仍有外层 CRC。
- 旧 37-byte provisional SessionDescriptor 只能稳定返回 `UnsupportedDescriptorSchema`，不得按长度猜测。
- SessionDescriptor 必须绑定 `SessionVisualProfileId`、`OriginalFileSize`、`SourceSegmentTargetBytes`、`SegmentCount`、压缩策略、摘要算法、FeatureFlags 与安全 UTF-8 basename。
- 未知 mandatory feature/TLV 必须拒绝；未知 optional TLV 才能跳过。
- 相同 descriptor key 的重复内容必须语义一致；冲突 descriptor、区间 overlap/gap、越界、算术溢出、算法冲突或 Profile 冲突均使 Session fail closed。
- 相同 `(SessionTag, SegmentOrdinal, OuterBlockId)` 的一致重复幂等；冲突 payload 是错误，绝不 latest-wins。

### 1.5 唯一产品视觉 Profile

正式产品仅公开：

```text
Name                  PB-Unified-LC4-V1
VisualProfileId       0x5042554E494C4331
VisualLayoutVersion   8
Canvas                1920 x 1080 BGRA8 SDR
Inner FEC             Robust DVB-S2 Short QC-LDPC
Outer payload         1314 bytes
Inner codeword        2025 bytes
```

载波复用当前 4x4 Shape 几何的 86,688 个数据 tile，但 codeword 不得跨可靠性 lane：

| Lane | Codeword 数 | 最大 Transport payload/逻辑帧 | 合同 |
| --- | ---: | ---: | --- |
| Base Luma | 17 | 22,338 B | 必选恢复层；chroma 完全中和时仍独立工作 |
| Fine Luma | 4 | 5,256 B | 模糊或缩放过强时可独立擦除 |
| Chroma | 10 | 13,140 B | 色度分离不足或 4:2:0 损伤时独立擦除 |
| 合计 | 31 | 40,734 B | 尚未扣除 mixed control 与 Outer repair 代价 |

硬约束：

- 16 个平衡 4x4 bitmap mask、label、lane mapping、交织置换和摘要一经选择即冻结；运行时不自学习。
- label/mapping 的搜索在固定 Train/Validation/Holdout 上确定性完成；查看 Holdout 前冻结选择规则。
- 优先最大化 Base Luma 最小 bit margin，再优化 Fine Luma，最后评估 Chroma；同分按字典序 tie-break。
- Base/Fine/Chroma 使用不同 `FrameSequence` 派生置换。
- Chroma pilot 失败只擦除 Chroma；局部 stale 只清零对应 soft metrics；不得跨 `FrameSequence` 拼接区域。
- 仅 locator/bootstrap 失败、画布裁切或身份冲突允许整帧擦除。
- Control 与 Transport 可在同一逻辑帧的不同 Base Luma codeword slot 中出现。
- 产品逻辑尺度为 `0.75x..2.0x`；小于 0.75x 时不生成有效 Bootstrap、不推进 FrameSequence。
- 远控软件名称、品牌、画质档位只能是 `NonDecodingOperatorMetadata`，不得进入协议或解码分支。

---

## 2. 当前实现基线与事实边界

### 2.1 `1445f9b` 已有基础

| 能力 | 状态 | 当前证据 | 仍需完成 |
| --- | --- | --- | --- |
| 正式 Session/Segment/Manifest schema | 已实现基础 | PBProtocol codec、正式 corpus、legacy rejection fixture；定向测试通过 | 公开规范表、完整 Golden 清单和产品入口最终收口 |
| UTF-8 basename 与 Windows 文件名校验 | 已实现基础 | parser/validation tests | 与最终发布冲突 UI 的产品级联动 |
| 0-byte descriptor/storage | 已实现基础 | PBProtocol/PBStorage 定向测试 | Encoder/Decoder GUI 完整端到端 |
| 8 MiB 多 Segment 预扫描 | 已实现基础 | Win32 稳定句柄、逐段 BLAKE3/zstd-or-RAW、descriptor table | 独立 headless 多 Segment 完整闭环与长文件门禁 |
| Sender durable lease | 已实现基础 | 4,096-ID `FrameSequence`/repair high-water 持久化 | 进程终止注入与所有 crash window 验证 |
| Sender resume | 已实现基础 | source/build/zstd/Wirehair/descriptor 一致性检查 | 预扫描中断、源变化、跨版本拒绝的进程级验证 |
| Random-access `.part` storage | 已实现基础 | `CreateOrResume`、verified range write/read/adopt、publish | 单 owner storage thread 的最终 controller 编排与大卷预分配策略 |
| Decoder append-only journal | 已实现基础 | record length/CRC/generation、active blocks、completed records、compaction | crash window、rename 后恢复、256 MiB budget 的进程级验证 |
| 活动 decoder 满时延迟 | 已实现 | `DeferredResourceBusy` tests | Carousel 端到端重试/收敛 |
| Whole-file digest 与安全发布 | 已有并扩展 | PBStorage 定向测试 | 多 Segment、大文件、rename 前后故障注入与外部哈希 |
| 历史 LF4/Direct/Shape 视觉路径 | 已存在实验实现 | 历史 Golden、CPU/GPU/Replay/实屏证据 | 仅可作为比较基础；不是统一产品 Profile |
| PB-Unified-LC4-V1 | 未实现 | 仅协议常量及本路线合同 | G06..G12 |
| 可缩放普通 Data Window | 未实现 | 旧窗口/呈现基础可复用 | G13 |
| WGC 自动转 DXGI | 未实现 | 两个 backend 各自存在 | G14 |
| 最终 Qt 产品流程 | 未实现 | Phase 1.5 GUI 仍暴露旧 Profile/backend/压缩项 | G15..G16 |

### 2.2 本轮已经执行的检查

基线提交前只运行了一个协议/流式/恢复定向检查点：

```powershell
cmake --build build-desktop-levels-release --config Release `
  --target PBProtocolTests PBStorageTests PBReceiverTests PBApplicationTests -- /m

ctest --test-dir build-desktop-levels-release -C Release `
  -R '^(PBProtocolTests|PBStorageTests|PBReceiverTests|PBApplicationTests)$' `
  --output-on-failure
```

结果：4/4 测试通过，总耗时约 33.55 秒。该证据只支持上述四个模块的当前定向检查，不代表完整 Release CTest、ASan、Qt、真实 GPU、真实屏幕、20 GiB 或远程链路通过。

### 2.3 当前已知阻塞与未关闭项

1. `PB-Unified-LC4-V1` 的 lane layout、codebook mapping、mixed slots 和 layout 8 尚未实现。
2. 统一 Profile 的 CPU oracle 与 D3D11 Compute accepted-byte parity 尚未建立。
3. Data Window 尚未满足普通 resizable、letterbox、0.75x 下限暂停、presentation epoch 原子替换合同。
4. GUI 仍是 Phase 1.5 过渡入口，仍可见旧 Profile/backend/压缩选择。
5. WGC→DXGI 自动 fallback 尚未接入统一 CaptureEpoch 状态机。
6. 大于 4 GiB 的一次性确认尚未成为产品状态机；当前自动接受策略不能作为最终产品依据。
7. 最终 rename 成功但 resume journal 尚未删除时的重启恢复未完全关闭。
8. 256 MiB、20 GiB、进程终止注入、完整 CTest、定向 ASan、右屏 native 和真实远程门禁均未在本基线执行。
9. 项目仓库尚未配置 Git remote；根目录尚未选择项目自身 LICENSE。LICENSE 属于维护者法律/发布决策，不在实现目标中擅自选择。

---

## 3. 不过度测试的工程节奏

### 3.1 三个集成检查点

全路线只设三个集中检查点：

| 检查点 | 目标 | 允许的主要验证 | 明确不做 |
| --- | --- | --- | --- |
| CP-A 协议/流式/恢复 | G04 | PBProtocol、PBOuterFec、PBReceiver、PBStorage、PBApplication 定向 build/CTest；headless 多 Segment | 全量 CTest、GUI、实屏、远程链路 |
| CP-B 统一 Profile/GPU | G12 | modulation、transform corpus、D3D11 parity、硬性能门槛候选 | 20 GiB、完整 GUI、真实远程 |
| CP-C 最终产品 | G20..G22 | 一次完整 Release CTest、定向 ASan、右屏 native、真实远程、打包复验 | 重复跑已通过的等价矩阵 |

### 3.2 普通目标的测试预算

- parser/state 变化：只构建对应库和 1..3 个对应测试 target；运行 test-name regex。
- GUI 变化：只构建两个应用和 GUI smoke；不自动打开真实 Data Window/ROI selector。
- shader/GPU 变化：先运行 WARP/离屏 parity；真实硬件只在 G12/G20。
- 文档/配置变化：只做 `git diff --check` 和本地链接检查。
- 只有出现共享基础设施回归迹象、ABI/公共头大范围变化或测试暴露跨模块故障时，才扩展测试范围，并在报告中说明原因。
- 不因一个目标通过而重复上一目标已经通过且未受影响的全套测试。

### 3.3 推荐的专用构建树

后续统一路线使用独立构建树，避免旧 Gate cache 选项污染：

```powershell
cmake -S . -B build-unified-release -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DPB_QT_ROOT=D:/Qt6.10.1/6.10.1/msvc2022_64 `
  -DBUILD_TESTING=ON `
  -DPB_BUILD_TESTS=ON `
  -DPB_BUILD_APPS=ON `
  -DPB_BUILD_TOOLS=ON `
  -DPB_BUILD_FUZZERS=OFF `
  -DPB_BUILD_BENCHMARKS=OFF `
  -DPB_BUILD_PHASE0_GATE=OFF `
  -DPB_BUILD_PRESENTATION_GATE=OFF `
  -DPB_BUILD_SCREEN_REGION_GATE=OFF `
  -DPB_BUILD_WGC_GATE=OFF `
  -DPB_BUILD_DXGI_GATE=OFF `
  -DPB_BUILD_LOCAL_DESKTOP_GATE=OFF `
  -DPB_BUILD_DESKTOP_LEVELS_GATE=OFF
```

ASan 使用另一个 `build-unified-asan`，仅在 G18/G20 配置和运行。真实屏幕 Gate 也必须使用独立 build tree，且只有 G20 才开启。

---

## 4. 目标依赖图与建议工期

```text
G00
 └─ G01 ─ G02 ─ G03 ─ G04 ─ G05
                         │
                         └─ G06 ─ G07 ─ G08 ─ G09 ─ G10 ─ G11 ─ G12
                                                              │
                         G13 ─ G14 ─ G15 ─ G16 ─ G17 ─────────┤
                                                              └─ G18 ─ G19 ─ G20 ─ G21 ─ G22
```

估算以单人连续、环境已具备、每个目标单独审查提交为前提：

| 范围 | 预计有效工程时间 |
| --- | ---: |
| G00..G05：协议/流式/恢复闭环 | 4..7 个工作日 |
| G06..G12：统一视觉 Profile、映射、CPU/GPU | 8..14 个工作日 |
| G13..G17：窗口、capture、Qt、telemetry | 5..9 个工作日 |
| G18..G22：大文件、故障注入、native/remote、发布 | 5..10 个工作日 |
| 合计 | **22..40 个有效工作日，约 4..8 周** |

真实远程端可用性、GPU/显示环境、0.75x Base Luma 是否达到硬门槛可能延长日历时间。这个估算不包括等待人工操作、远程机器排期或发现需要回到产品决策的物理层阻塞。目标模式若采用较短的单次执行预算，建议每个 Goal 预留 1 次实现轮次，G07、G11、G18、G20 可各预留 2 次。

---

## 5. Goal 逐步实施手册

## G00 — 基线、文档和仓库状态对齐

**状态：** 本文创建时已完成。
**目的：** 让唯一当前路线、历史证据和 GitHub 发布边界明确，不再同时存在两套互相冲突的产品合同。

**读取：** `AGENTS.md`、本文、权威总体设计顶部、`docs/README.md`、`docs/CURRENT_RUNTIME_OPTION_INVENTORY.md`。
**修改范围：** 文档、`.gitignore`、`.gitattributes`、根 `README.md`。
**非目标：** 不修改协议/视觉代码，不启动 GUI，不执行全量测试。

**检查清单：**

- [x] 记录代码起点 `1445f9b`。
- [x] 明确统一 Profile、`1..60 Hz`、多 Segment、0-byte、500 GiB policy 与恢复合同。
- [x] 将旧 RemoteVisual/Phase 1.5 文档标为历史实现/证据，而不是当前产品规范。
- [x] 建立 `docs/README.md` 文档索引与 GitHub 发布检查清单。
- [x] 保护用户本地 `docs/PHASE1_GATE_REPORT.md`，不纳入提交。

**最小验证：** `git diff --check`；本地 Markdown 相对链接存在性检查；`git status --short`。
**退出：** 新贡献者从根 README 能定位当前路线、构建方式、事实边界和发布检查表。
**提交建议：** `docs: add goal-mode implementation and publish roadmap`

## G01 — Protocol 1.0 Descriptor 收口

**状态：** 已完成（2026-09-03）。
**前置：** G00。
**目的：** 将 `1445f9b` 的正式 descriptor 基础审查为可供后续产品使用的唯一 schema，并补齐规范/Golden 边界。

**读取：** 总体设计 §7..§10、§15、§33；`libs/PBProtocol/include/pbprotocol/descriptor_codec.h`、`protocol_types.h`；相关 tests/corpus。
**主要范围：** `libs/PBProtocol`、`tests/PBProtocol`、`fuzz/corpus/bootstrap-control`、`fuzz/corpus/descriptor-resource`、协议文档。
**非目标：** 不实现视觉 Profile、不改 Qt、不做大文件 I/O。

**实现清单：**

- [x] 把 Session/Segment/Manifest 每个字段的 offset、宽度、端序、长度和 CRC 覆盖范围写成机器可核对表。
- [x] 确认 `DescriptorTotalBytes` 与 Control payload 精确相等；禁止 trailing bytes 和部分解析成功。
- [x] 确认未知 mandatory feature/TLV 与 optional TLV 的分支和资源上限。
- [x] 确认 UTF-8 basename：无 NUL、路径/UNC/盘符/ADS、设备名、尾点/空格和非法字符。
- [x] 覆盖 0、1、8 MiB±1、20,000,000,000、20 GiB、500 GiB/64,000 Segment 的 checked arithmetic。
- [x] 冻结新正式 Golden；旧 provisional fixtures 只断言 `UnsupportedDescriptorSchema`。
- [x] 保持 PBProtocol 只验证 wire 结构和非零 profile；具体产品只允许 Unified 的规则留在 application admission。

**最小验证：**

```powershell
cmake --build build-unified-release --config Release --target PBProtocolTests -- /m
ctest --test-dir build-unified-release -C Release -R '^PBProtocolTests$' --output-on-failure
```

**验证结果（2026-09-03）：** `PBProtocolTests` Release 目标构建成功；上述 CTest 正则命中 1 个测试，1/1 通过，0 失败。

**退出：** malformed/conflict/resource/Golden 全通过；协议表与 bytes 一致；旧 schema 确定拒绝。
**产物：** 正式 descriptor 字节表、Golden manifest、测试结果。
**提交建议：** `fix(protocol): close formal descriptor schema one`

## G02 — 流式 Encoder 与 durable Carousel 收口

**状态：** 已完成（2026-09-03）。
**前置：** G01。
**目的：** 完成不随总文件大小增长的预扫描/广播/恢复发送器，证明 ID 不回退且 equation 可复现。

**读取：** 总体设计 §10..§13、§31..§32；`apps/common/encoder_session_store.*`、`local_desktop_runtime.*`；PBCompression/PBOuterFec。
**主要范围：** `apps/common`、`libs/PBCompression`、必要的 PBOuterFec 接口、`tests/PBApplication`。
**非目标：** 不改视觉 mapping，不打开 Data Window，不实现 Decoder UI。

**实现清单：**

- [x] 稳定 Win32 文件句柄禁止 write/delete sharing，记录 volume serial、file ID、size、last-write。
- [x] 逐 8 MiB Segment 扫描，累计 whole BLAKE3，计算 raw/encoded digest，zstd level 3 无收益回退 RAW。
- [x] 预扫描仅保存 descriptor table；广播只保存 current/next 两个 Segment。
- [x] 重新压缩必须与预扫描 `EncodedDigest` 相同后才 `WirehairV2Encoder::Recreate`。
- [x] 每轮发送 systematic `[0,K)` 与 `max(16, ceil(K*20%))` repair；后续轮使用新 repair IDs。
- [x] DirectRepeat 保留 tiny/small 决策且重复语义固定。
- [x] Session/Manifest 约 10 秒重复，SegmentDescriptor 在 Segment 开始和期间重复；调度接口先独立于最终 mixed slot 物理布局。
- [x] 4,096-ID lease 必须先原子持久化终点再使用；重启跳过未用 ID。
- [x] 仅当 source、digest、descriptor、build/zstd/Wirehair identity 全一致才恢复旧 Session。
- [x] 预扫描/广播过程中检测源身份变化并停止，不创建“继续但内容变了”的 Session。

**最小验证：** 只构建/运行 `PBApplicationTests` 中 sender/session/persistence 标签；增加一个 >8 MiB headless scheduler 测试，不运行屏幕链。
**验证结果（2026-09-03）：** `PBApplicationTests` Release 目标构建成功；直接执行 `[sender]` 标签命中 zstd/RAW、scheduler、session persistence、既有 sender raster 回归和 25,169,920-byte/4-Segment headless 五个测试，5/5 通过。未创建 Data Window，未执行屏幕链。
**退出：** 三个以上 Segment 可持续轮播；内存是双 Segment；重启后 FrameSequence/repair ID 单调；相同输入 equation 可复现。
**产物：** [`ENCODER_STREAMING_CAROUSEL.md`](ENCODER_STREAMING_CAROUSEL.md) 中的 sender 状态 schema、lease crash-window 表与报告说明；本地生成 `build-unified-release/tests/PBApplication/g02-sender-headless-report.json`。
**提交建议：** `feat(sender): finish streaming carousel persistence`

## G03 — Decoder journal、乱序存储与恢复收口

**状态：** 已完成（2026-09-03）。
**前置：** G01、G02 的 descriptor/carousel 合同。
**目的：** 完成无序 Segment 恢复、持久 checkpoint、重启重放和最终发布的核心状态机。

**读取：** 总体设计 §15、§29..§33；`decoder_resume_store.*`、PBReceiver、PBStorage。
**主要范围：** `apps/common/decoder_resume_store.*`、`local_desktop_runtime.*`、`libs/PBReceiver`、`libs/PBStorage`、对应 tests。
**非目标：** 不做 UI、不做真实 capture、不做 20 GiB 长运行。

**实现清单：**

- [x] `.part` 固定内部名 `PixelBridge-<SessionTag>.part`，不直接使用不可信文件名。
- [x] descriptor 通过资源/区间校验后才能分配、创建 codec、写 journal 或预分配。
- [x] activity 满 4 个时返回 `DeferredResourceBusy`，不分配、不驱逐、不使 Session 失败。
- [x] 完成顺序固定为 encoded digest → bounded decompress → raw digest → random write → `FlushFileBuffers` → completed journal flush → `CommitStoredSegment`。
- [x] active accepted blocks 每 1 秒或 Segment 完成 checkpoint；同 key 冲突 payload fail closed。
- [x] record 带 generation/length/CRC；只忽略最后一个明确截断尾 record，内部损坏拒绝。
- [x] journal 到 16 MiB 或 Segment 完成时 compact snapshot，temp flush 后原子替换。
- [x] 重启重验 `.part` 中 completed Segment 的 raw digest，再 adopt；最多重建 4 个 decoder 并重放 block cache。
- [x] 全 Segment 完成后顺序 whole BLAKE3、安全 rename、重新打开复验长度/摘要，再删 journal。
- [x] 最终 basename 冲突采用确定性 SessionTag 候选；第二候选也存在时停止且不覆盖。

**最小验证：** `PBReceiverTests`、`PBStorageTests`、`PBApplicationTests` 中 resume 标签；不跑 modulation/native。
**验证结果（2026-09-03）：** Release 增量构建三个目标成功；固定 seed 直接执行 `PBReceiverTests` 36/36（1,696 assertions）、`PBStorageTests` 12/12（144 assertions）、`PBApplicationTests [resume]` 5/5（275 assertions）全部通过。未创建窗口，未运行 modulation、GPU、capture、native 或完整 CTest。
**退出：** torn tail 可恢复；内部 CRC/长度/cache 超限拒绝；completed segment 重启重验；活动 decoder 资源满后可由后续 Carousel 重试。
**产物：** [`DECODER_RESUMABLE_RECOVERY.md`](DECODER_RESUMABLE_RECOVERY.md) 中的 PBJH/PBJR schema、checkpoint/compaction 与 Segment/publish 状态转换、crash-window 边界和故障分类；本地测试日志为 `build-unified-release/tests/PBReceiver/g03-pbreceiver-tests.txt`、`build-unified-release/tests/PBStorage/g03-pbstorage-tests.txt`、`build-unified-release/tests/PBApplication/g03-resume-tests.txt`。
**提交建议：** `feat(receiver): close resumable random access recovery`

## G04 — CP-A：多 Segment headless 端到端检查点

**状态：** 已完成（2026-09-03）。
**前置：** G01..G03。
**目的：** 用真实 application controller/runtime 在无屏幕路径证明正式 descriptor、Outer、Receiver、storage/publish 的窄闭环。

**主要范围：** application headless harness、integration tests、少量诊断报告。
**非目标：** 不实现统一视觉 raster，不启动 GPU/GUI，不跑全量 CTest。

**用例：**

- [x] 0-byte。
- [x] 1 byte。
- [x] `8 MiB-1`、`8 MiB`、`8 MiB+1`。
- [x] 至少一个 24..32 MiB CSPRNG RAW 文件。
- [x] 压缩有收益与无收益各一个小 fixture。
- [x] descriptor 乱序、重复、repair、4-active busy 后重试。
- [x] 最终文件外部长度、SHA-256、BLAKE3 与源一致。

**测试命令：**

```powershell
cmake --build build-unified-release --config Release `
  --target PBProtocolTests PBOuterFecTests PBReceiverTests PBStorageTests PBApplicationTests -- /m
ctest --test-dir build-unified-release -C Release `
  -R '^(PBProtocolTests|PBOuterFecTests|PBReceiverTests|PBStorageTests|PBApplicationTests)$' `
  --output-on-failure
```

再运行一次专用 headless multi-segment executable/test；不得额外运行 full CTest。
**验证结果（2026-09-03）：** Release 精确构建五个规定目标成功；规定 CTest 正则命中 5 个测试，5/5 通过，0 失败（31.94 秒）。专用 `PBHeadlessMultiSegmentCheckpoint` 目标构建成功，专用 CTest 1/1 通过（16.98 秒），内部 9/9 fixture 全部 authoritative publish 且 byte-exact。Python 3.12.9 `hashlib` + `blake3 1.0.9` 对全部 source/output 与 JSON 记录再次独立复验，9/9 通过。未运行 full CTest、raster、Inner FEC、modulation、GPU、capture、GUI 或屏幕链。
**退出：** 上述用例发布 byte-exact；峰值 sender/receiver working set 符合 active-segment 预算；CP-A 报告注明未覆盖视觉链。
**产物：** [`UNIFIED_VISUAL_CP_A_HEADLESS.md`](UNIFIED_VISUAL_CP_A_HEADLESS.md) 记录实际 application-runtime 闭环、用例、working-set 高水位、证据哈希和边界；本地报告为 `build-unified-release/tests/PBApplication/g04-cp-a-headless-report.json`，其中 `visualChainCovered=false`。
**提交建议：** `test(application): prove headless multi segment recovery`

## G05 — 大输出确认与 rename 后恢复

**状态：** 已完成（2026-09-03）。
**前置：** G04。
**目的：** 关闭两个会影响大文件产品安全的状态机缺口。

**主要范围：** application model/controller、PBStorage、resume store、对应 tests。
**非目标：** 不改 Qt 具体布局；先定义 Qt-free controller 状态与回调。

**实现清单：**

- [x] `OriginalFileSize > 4 GiB` 时进入 `AwaitingLargeOutputConfirmation`，且一个 Session 只询问一次。
- [x] 确认前不创建/扩展 `.part`，不接收 Outer payload；拒绝后保持可解释终态。
- [x] 检查目标卷可用空间与实际 allocation/preallocation 结果；稀疏/压缩卷语义要记录，不把逻辑长度当已分配空间。
- [x] rename 成功、journal 删除前崩溃时，重启能识别已发布且摘要一致的最终文件，并安全清理状态；冲突或摘要不一致则 fail closed。
- [x] rename 前、rename 后、final reopen 前后的重复操作保持幂等且不覆盖已有文件。

**最小验证：** application/storage 定向状态测试，使用小 fixture 模拟阈值和 crash marker；不创建真实 4 GiB 文件。
**验证结果（2026-09-03）：** Release 增量构建 `PBStorageTests`、`PBApplicationTests` 成功。固定 seed 直接执行 PBStorage 全部 17/17（219 assertions）；Application G05 3/3（99 assertions）、既有 decoder resume 5/5（284 assertions）、decoder-state 3/3（43 assertions）、G04 headless checkpoint 1/1（31 assertions）、decoder report 1/1（112 assertions）全部通过。确认门使用 4,097-byte fixture 与 4,096-byte policy 阈值，不创建真实 4 GiB 文件；未运行 full CTest、GPU、capture、GUI、实屏或真实进程终止注入。
**退出：** 大输出确认有明确公共状态；post-rename crash 可恢复；现有文件永不被覆盖/删除。
**产物：** [`DECODER_RESUMABLE_RECOVERY.md`](DECODER_RESUMABLE_RECOVERY.md) 已加入 Qt-free 确认门、`OutputReservation`/`PublishIntent` PBJR 记录、allocation 事实和 post-rename 重启分支；本地测试日志为 `build-unified-release/tests/PBStorage/g05-storage-tests.txt` 与 `build-unified-release/tests/PBApplication/g05-application-tests.txt`。
**提交建议：** `feat(storage): gate large output and recover publish commit`

## G06 — Unified Profile 常量与 lane 合同

**前置：** G04；G05 可并行但合并前必须均完成。
**目的：** 建立唯一产品 Profile 的静态容量、slot/lane、pilot、geometry 和 erasure 公共合同，不立即写搜索算法或 shader。

**读取：** 总体设计 §14、§16..§19、§26..§28、§34..§35；历史 LF4/Shape 实现仅作参考。
**主要范围：** PBModulation 公共 profile catalog、PBProtocol product admission、tests。
**非目标：** 不选择最终 mask mapping，不写 GPU shader，不删除历史实验 profile。

**实现清单：**

- [ ] 固定 ID、layout 8、canvas、BGRA8 SDR、tile count、31 codewords、lane 容量。
- [ ] 定义 locator/bootstrap/control/pilot/data 保留区，checked capacity 必须一致。
- [ ] 定义 Base/Fine/Chroma observation 与 erasure reason 类型。
- [ ] 定义 mixed slot 的抽象类型和优先级，不让 Control 占满整帧。
- [ ] 定义 0.75x..2.0x 和 letterbox/viewport 几何合同。
- [ ] application product admission 只接受 Unified；历史 profile 仍可由内部工具显式调用。
- [ ] 为每个常量添加 compile-time/static contract tests，避免 magic number 分散。

**最小验证：** PBModulation/PBProtocol profile contract tests；不跑历史 transform matrix。
**退出：** 所有模块共享一个 manifest/constant source；容量恰好可证明且无跨 lane codeword。
**提交建议：** `feat(modulation): define unified lc4 profile contract`

## G07 — 确定性 codebook/mapping 搜索与冻结

**前置：** G06。
**目的：** 在固定数据集与不可窥视 Holdout 纪律下选择并冻结 16 个平衡 4x4 mask、labels、lane mapping、interleave seeds。

**主要范围：** PBRemoteVisualSimulator、独立搜索工具、测试 fixtures、`tests/golden/unified-lc4`。
**非目标：** 不把 provider 名称传入评分；不手调某品牌阈值；不写 production GPU。

**实现清单：**

- [ ] 先封存 Train/Validation/Holdout manifest、样本摘要、transform 参数与 split 算法。
- [ ] 复用历史 LF4/Shape 样本，补齐 neutral chroma、4:2:0 类损伤、0.75/0.85/1.0/1.5/2.0、fractional origin、letterbox、moderate blur/quantization。
- [ ] 实现确定性候选枚举/搜索，排序键为 Base minimum margin → Fine → Chroma → lexicographic。
- [ ] 在查看 Holdout 结果前冻结搜索程序版本、seed、评分规则和 Validation winner。
- [ ] 仅对 winner 运行一次 Holdout；失败时报告阻塞，不基于品牌反复调参。
- [ ] 生成独立可重建 Golden manifest，记录 mask/label/lane/interleave digest。

**最小验证：** 搜索工具 deterministic rerun 两次摘要相同；Validation 与一次 Holdout 报告；不跑产品 GUI/完整 CTest。
**退出：** 独立重建得到相同 mapping；Holdout 无 false acceptance；所有选择理由可审计。
**提交建议：** `feat(modulation): freeze deterministic unified mapping`

## G08 — CPU encoder/oracle、逐 lane soft metric 与 freshness

**前置：** G07。
**目的：** 先建立语义权威 CPU 路径，再允许 GPU 优化。

**主要范围：** PBModulation、PBInterleave、PBInnerFec、对应 tests/golden。
**非目标：** 不写 D3D11 Compute，不启动真实 capture。

**实现清单：**

- [ ] 实现规范 raster encoder，使用冻结 mask/labels/pilots。
- [ ] 为三 lane 使用不同的 `FrameSequence` 派生 permutation，提供正反映射。
- [ ] soft metric 输出带 lane、slot、region 与 erasure reason。
- [ ] chroma pilot 失败仅擦除 Chroma；Fine 失败不清空 Base。
- [ ] 扩展 freshness region：局部旧新混合仅清零受影响 metrics；禁止跨 sequence 拼帧。
- [ ] bootstrap/locator 裁切或身份冲突才整帧擦除。
- [ ] CPU accepted output 只有 Inner FEC/padding/Transport CRC/identity 全通过才产生。
- [ ] 对每个 lane 建立 clean、neutralized、localized stale、wrong-sequence negative Golden。

**最小验证：** PBModulation、PBInnerFec 的 Unified test subset；一次 independent Golden regeneration compare。
**退出：** CPU oracle 对 mandatory synthetic transforms 无 false accepted Control/Transport；lane erasure 互不拖累。
**提交建议：** `feat(modulation): implement unified cpu oracle`

## G09 — mixed Control/Transport 帧调度

**前置：** G08、G02。
**目的：** 在同一逻辑帧分配 Base Luma control slots 和其余 Transport，消除 1 Hz 下整帧 Control 浪费。

**主要范围：** application carousel scheduler、PBModulation frame input、protocol packing、tests。
**非目标：** 不改变 Control envelope/wire CRC；不依赖 Decoder ACK。

**实现清单：**

- [ ] slot 类型明确标识 Control/Transport，不允许 ambiguity。
- [ ] 优先级：Session/Manifest/当前 SegmentDescriptor；其余 slots 继续 Transport。
- [ ] Session/Manifest 约每 10 秒，SegmentDescriptor 在开始与传输中重复。
- [ ] 控制重组/resource budget 不因 mixed slots 放宽。
- [ ] Control 重复不推进 Carousel payload ID；逻辑帧只在完整 raster ready 后推进一次。
- [ ] 1、15、60 Hz 的调度以逻辑 frame tick 为上限，错过 tick 丢弃而不排队追赶。
- [ ] 对 0-byte Session 仍能发送 Session+Manifest 并被恢复。

**最小验证：** scheduler/reference raster tests；用 1 Hz 模拟 30 秒，确认 Control cadence 和 Transport 占用。
**退出：** 每帧 slot accounting 精确；无整帧 Control 特例；0-byte 和多 Segment 均可收敛。
**提交建议：** `feat(application): mix control and transport slots`

## G10 — provider-generic transform corpus 检查点

**前置：** G08、G09。
**目的：** 在 GPU 前用 CPU oracle 关闭统一 Profile 的必须失真集合和 16 KiB Base Luma 可行性。

**主要范围：** PBRemoteVisualSimulator、corpus generation/manifest、PBModulation tests。
**非目标：** 不记录或分支远控品牌；不把 synthetic 结果称为真实远程认证。

**必须集合：**

- chroma 完全中和；
- 4:2:0 类色度降采样/错位；
- 0.75x、0.85x、1.0x、1.5x、2.0x；
- fractional origin 与任意宽高比 letterbox；
- moderate blur 与 quantization；
- duplicate/drop/light reorder；
- 局部旧新帧混合；
- crop/bootstrap identity 冲突 negative cases。

**最小验证：** 仅 CPU transform corpus；每个样本记录 accepted bytes、lane erasure、FEC/CRC、conflict、false acceptance。
**退出：** mandatory corpus false accepted 为 0，冲突输出为 0；chroma-neutralized 的 Base Luma 有能力达到 `>=16 KiB/unique logical frame`。若 0.75x 失败，立即回到产品决策，不能降低门槛、改 provider 参数或偷偷提高最小尺度。
**提交建议：** `test(modulation): close unified transform corpus`

## G11 — D3D11 Compute 统一解调

**前置：** G10。
**目的：** 从 PB-owned capture texture 直接产生 compact lane metrics，复用 CPU FEC/Transport/Receiver。

**读取：** 总体设计 §22..§24、§26..§30；CaptureNormalize 和现有 LF4 shader lifetime。
**主要范围：** `libs/PBDemodD3D11`、CaptureNormalize 接口、shader embedding、tests。
**非目标：** 不做 raw ROI GPU→CPU→GPU fast-path 往返；不要求 float metric bitwise 相等。

**实现清单：**

- [ ] 输入必须是当前 CaptureEpoch 的 PB-owned texture，不能在 WGC frame lease 归还后访问 source surface。
- [ ] scale/origin aware sampling 支持 0.75x..2.0x、fractional origin、letterbox。
- [ ] 输出 compact Base/Fine/Chroma metrics、freshness mask、pilot/erasure reason。
- [ ] D3D11 immediate context 保持单 owner thread。
- [ ] resize/device/capture epoch 变化排空旧 GPU work；结果携带 epoch 并在 admission 再核对。
- [ ] query/fence 语义证明 GPU 完成；`Flush` 不当 completion。
- [ ] shader/resource 数量有界，失败明确回到等待/重建而非 silent CPU pixel fallback。

**最小验证：** WARP 离屏 unit/integration + 一个当前硬件 adapter smoke；不打开 Data Window。
**退出：** 对固定 corpus 产出可供 CPU FEC 的 metrics；无 lease/use-after-return、跨 epoch admission 或隐式 readback。
**提交建议：** `feat(demod): add unified d3d11 compute path`

## G12 — CP-B：CPU/GPU 语义 parity 与性能门槛

**前置：** G11。
**目的：** 证明 CPU/GPU 在最终接受语义上一致，并关闭统一 Profile 的第二检查点。

**比较内容：**

- 最终 accepted Control/Transport bytes；
- FEC disposition、padding、CRC、Session/Frame identity；
- Base/Fine/Chroma erasure reason；
- conflict/false acceptance；
- 不比较逐 bit 浮点 soft metric 的精确数值。

**最小验证：** PBModulation、PBDemodD3D11、CaptureNormalize、transform corpus；WARP + 当前 AMD/NVIDIA 中实际可枚举的 adapter 各一次，不做显示窗口。
**性能：** CSPRNG/RAW，全链最终发布后才计算 `VerifiedEncodedBytesPerUniqueFrame`；硬门槛 16 KiB，clean/颜色可用工程目标 32 KiB。
**退出：** mandatory corpus CPU/GPU accepted-byte parity；false accepted/conflict 为 0；Base neutralized >=16 KiB。未达 32 KiB 可继续首版，但必须明确记录为 engineering target miss。
**提交建议：** `test(demod): prove unified cpu gpu semantic parity`

## G13 — 可缩放 Data Window 与 presentation epoch

**前置：** G08/G09；建议在 G12 后进行。
**目的：** 把旧固定画布窗口改为普通 resizable chrome，同时保持规范 raster 完整替换和 cadence 语义。

**主要范围：** PBRenderD3D、Encoder runtime/controller、presentation tests。
**非目标：** 默认测试不显示窗口；不修改系统分辨率/DPI/HDR/排列。

**实现清单：**

- [ ] `WS_OVERLAPPEDWINDOW`；允许拖动、最小化和 resize。
- [ ] canonical `1920x1080` immutable texture；point sampling 等比缩放、居中 letterbox。
- [ ] 有效 viewport 上限 2.0x，超大窗口仍居中；小于 0.75x 显示 neutral matte。
- [ ] 小于 0.75x 不生成有效 Bootstrap、不推进 FrameSequence/Carousel，并显示“窗口过小，广播已暂停”。
- [ ] resize/DPI/monitor/device/swap-chain 变化创建 presentation epoch，排空旧 epoch，后台完成整 raster 后原子替换。
- [ ] stable dwell 可重复 Present，不产生新 equation。
- [ ] 运行中 FPS 改变在下一完整逻辑帧应用；过期 tick 丢弃而不积压。

**最小验证：** PBRenderD3D headless/mock unit + WARP offscreen；真实 DISPLAY2 resize 只留给 G20。
**退出：** back-buffer 永不出现半旧半新 raster；暂停/恢复保持同 Session 且 IDs 不回退。
**提交建议：** `feat(presentation): add resizable atomic data window`

## G14 — WGC 默认捕获与 DXGI 明确 fallback

**前置：** G11/G12。
**目的：** 用户不选 backend，由 controller 执行可解释、有 epoch 边界的自动策略。

**主要范围：** WGC、DXGI、CaptureNormalize、Decoder controller/model、tests。
**非目标：** 不基于 provider 名称选择 backend；不在普通 decode miss 时切换。

**实现清单：**

- [ ] 首选 WGC；只在初始化失败、AccessLost 或设备重建失败时切换 DXGI。
- [ ] 每次切换创建新 CaptureEpoch，停止/排空旧 backend 和 GPU work。
- [ ] UI/report 记录 requested policy=Auto、actual backend、切换原因和时间。
- [ ] 两者均失败才进入 Failed；临时无可解码画面只是 Waiting/Stalled。
- [ ] 相同 FrameSequence 去重、轻微乱序有界处理、stale capture 丢弃。
- [ ] capture 可用速率独立于 Encoder FPS，不做人为 FPS 节流。

**最小验证：** mocked backend failure state tests + WGC/DXGI non-display unit tests；不启动选区器。
**退出：** fallback 只发生于冻结条件；跨 epoch 结果无法进入 Receiver；状态/报告可解释。
**提交建议：** `feat(capture): add explicit wgc to dxgi fallback`

## G15 — Encoder Qt 产品收敛

**前置：** G05、G09、G13。
**目的：** 最终基础界面只保留文件、FPS、开始/停止，所有选项绑定真实 runtime。

**主要范围：** `apps/PixelBridgeEncoder`、application controller/model、GUI tests/docs。
**非目标：** 不在 Qt 中实现协议/FEC/render；不显示 Decoder 进度。

**实现清单：**

- [ ] 基础页：源文件、1..60 Hz（默认 15）、开始/停止、准备进度、广播状态。
- [ ] 状态区：文件大小、Segment 数、预扫描速度、Carousel pass/ordinal、实际逻辑 FPS、source stability。
- [ ] 高级页只读显示 Unified Profile、Outer/Inner FEC、RAW/zstd 决策与 durable lease。
- [ ] 删除/隐藏产品级 Profile、compression tuning、backend、monitor safety experiment choices。
- [ ] 先预扫描后开窗口；失败不留下伪 Session/空窗口。
- [ ] Encoder 文案始终为 Broadcast/广播，不显示“已发送完成”或对端 ETA。
- [ ] “结束并删除会话”必须是显式动作；普通停止保留 Session。

**最小验证：** build `PixelBridgeEncoder`、controller/model tests、`PixelBridgeEncoderGuiSmoke`；不显示真实 Data Window。
**退出：** 基础流程不需理解协议；所有可点控件有真实绑定；CLI/GUI 使用同 controller/runtime。
**提交建议：** `feat(gui): converge unified encoder workflow`

## G16 — Decoder Qt 产品收敛

**前置：** G05、G12、G14。
**目的：** 最终基础界面只保留输出目录、ROI、开始/停止，并展示真实恢复状态。

**主要范围：** `apps/PixelBridgeDecoder`、application controller/model、GUI tests/docs。
**非目标：** 不在 Qt 中解码或写文件；默认测试不启动 ROI selector。

**实现清单：**

- [ ] 基础页：输出目录、选择 ROI、开始/停止。
- [ ] 点击“选择 ROI”才调用现有 PMv2 selector；物理坐标仅作为高级后备。
- [ ] 删除 FPS、Profile、显式 backend 选择；显示实际 backend/fallback 原因但不可调。
- [ ] 首个正式 Unified Session 锁定；显示文件名、大小、verified Segment/bytes、goodput、ETA、resume 状态、geometry。
- [ ] `AwaitingLargeOutputConfirmation` 用一次明确对话框；拒绝不创建大文件。
- [ ] Waiting/Stalled 不清空状态、不自动停止；Stop 只停止捕获并保留可恢复状态。
- [ ] Completed 显示路径、长度、BLAKE3、打开目录，不弹另存为。

**最小验证：** build `PixelBridgeDecoder`、controller/model tests、`PixelBridgeDecoderGuiSmoke`；不启动真实 selector/capture。
**退出：** GUI 所有状态来自 runtime；无假 Profile/backend/FPS 选项；恢复/完成路径可解释。
**提交建议：** `feat(gui): converge automatic decoder workflow`

## G17 — Telemetry、报告与状态真实性

**前置：** G12、G15、G16。
**目的：** 统一指标命名和证据来源，避免 Present/FPS/中间解码冒充最终 goodput。

**主要范围：** PBTelemetry、run_report、GUI status adapters、report tests。
**实现清单：**

- [ ] Base/Fine/Chroma metric/FEC/CRC/accepted/erased 计数。
- [ ] control-slot 占用、Carousel pass/ordinal、repair lease。
- [ ] preparation/resume verification 时间、verified segments/bytes。
- [ ] `UniqueVisualFPS` 只来自观测到的唯一逻辑帧；重复 capture 不增加分母。
- [ ] `VerifiedEncodedBytesPerUniqueFrame` 只有 whole digest + publish 成功才非空。
- [ ] 最终 publish 状态、whole digest、final reopen 验证单独记录。
- [ ] provider 字段只能进入 `NonDecodingOperatorMetadata`，并以测试证明不影响 decode 参数。

**最小验证：** PBTelemetry + application report tests；固定 JSON Golden 只在 schema 有意变更时更新。
**退出：** 指标有明确 numerator/denominator/source；失败和 unavailable 不被填 0 或估算值。
**提交建议：** `feat(telemetry): report unified lane and publish truth`

## G18 — 256 MiB 与进程级故障注入

**前置：** G17。
**目的：** 用真实 256 MiB CSPRNG/RAW 文件关闭双端恢复 crash windows，是第一次集中可靠性检查。

**注入点：**

- Encoder 预扫描中；
- Encoder lease 持久化后、显示前；
- Encoder Segment 中途；
- Decoder active FEC 中；
- `.part` flush 后、Completed record 前；
- Completed record 后、compaction 前；
- whole-file digest 中；
- rename 前、rename 后。

**验证：**

- [ ] 每个点使用真实进程终止与重启，不仅是函数异常模拟。
- [ ] 已 durable completed Segment 不丢；未 flush 尾部允许 Carousel 重收。
- [ ] FrameSequence/repair ID 不复用。
- [ ] source 变化拒绝旧 Session。
- [ ] torn final tail 可忽略；内部 CRC、伪长度、超额 active cache 拒绝。
- [ ] 输出外部长度、SHA-256、BLAKE3 与源一致。
- [ ] sender/receiver working set 记录且不接近 256 MiB 文件总量的线性副本。

**测试预算：** 只运行故障注入 harness 和受影响 parser/resume/storage 的定向 ASan；不跑 full CTest、GPU/GUI/native。
**退出：** 所有注入点从干净基线可复现；失败路径无错误发布；报告包含峰值内存与恢复时间。
**提交建议：** `test(resume): prove process restart recovery at 256 mib`

## G19 — 20 GiB+ headless 大文件能力

**前置：** G18。
**目的：** 用真实触达每个 Segment 的稀疏结构化文件证明 64-bit offset、预扫描、随机写、resume 与 publish。

**fixture 合同：**

- 文件大小至少 20 GiB；
- 每个 8 MiB Segment 都含确定性非零区域和 ordinal marker；
- 不能用全零高压缩文件替代全部 Segment 触达；
- fixture 生成规则、seed、length、BLAKE3/SHA-256 记录在 create-only manifest；
- 输出目录空间在运行前检查，不覆盖已有 artifact。

**执行：** 完整 headless payload simulation，可跳过真实视觉耗时；至少一次中断恢复；记录预扫描速度、SegmentCount、max ordinal/offset、working set、journal 峰值、whole digest 和外部哈希。
**测试预算：** 只运行专用 20 GiB harness；不同时运行 full CTest、ASan 或实屏。
**退出：** 所有 Segment 实际触达；最终发布 byte-exact；内存仍为双 Segment/4 active；500 GiB 只做 checked-arithmetic/serialization，不实际分配。
**提交建议：** `test(application): prove twenty gibibyte headless capability`

## G20 — CP-C：最终 Release、ASan 与 DISPLAY2 native Gate

**前置：** G19，所有 Critical/High 已清零。
**目的：** 只在此时执行一次完整本机构建/回归与真实右屏产品链。

**执行顺序：**

1. 干净或重新 configure 的 Release build；
2. 一次完整 CTest；
3. parser/resume/storage 定向 ASan；
4. Qt GUI smoke；
5. 真实 native：1 Hz、15 Hz、60 Hz、0.75x、fractional scale、arbitrary aspect letterbox、2.0x、小于 0.75x 暂停与恢复；
6. 记录实际逻辑 FPS、observed unique rate、lane erasure、digest/publish。

**屏幕硬约束：**

- 所有显示窗口和 ROI 必须完全位于 `\\.\DISPLAY2`；与 `\\.\DISPLAY1` 相交立即拒绝。
- 不移动/关闭外部窗口，不改变分辨率、DPI、HDR、刷新率或显示排列。
- 不发送鼠标键盘输入；ROI 选择等需要输入的步骤由用户手动完成。
- Gate 前重新枚举 monitor identity/physical rect；不能按历史坐标猜测。

**退出：** 两个 Qt EXE 可运行；full CTest 通过；定向 ASan 通过；右屏所有尺度/cadence 合同成立；未执行项精确列出。
**提交建议：** `test(product): close local unified release gate`

## G21 — 真实远程像素链

**前置：** G20。
**目的：** 在 Decoder 不透明的真实远程像素链上完成快速 smoke 和完整文件恢复，不按品牌修改参数。

**用例：**

- 1 MiB CSPRNG 快速 smoke；
- 64 MiB CSPRNG/RAW 完整恢复；
- 默认 15 Hz，按实际 unique FPS 统计；
- 至少包含 chroma 可用与 Base Luma 独立恢复的可观察证据；若链路不能人为中和 chroma，则使用同一实际 capture 的离线 neutralized 派生验证，并清楚区分证据类型。

**用户配合：** 用户只负责远程端连接、窗口放置和必要点击；自动化不控制鼠标键盘。
**记录：** scale/origin、capture backend/fallback、unique FPS、lane erasure、FER、control occupancy、goodput、whole digest、final publish、外部 SHA-256/BLAKE3。
**退出：** live 最终发布成功、错误文件为 0、false accepted/conflict 为 0；同一参数不含 provider-specific 分支。未达到 32 KiB 工程目标可以发布首版，但必须明确记录；低于 16 KiB 硬门槛不能关闭。
**提交建议：** `test(remote): verify unified end to end recovery`

## G22 — 包、SBOM、用户文档与发布候选

**前置：** G21。
**目的：** 生成可独立验证的 Windows 发布候选，同时保留源码仓库整洁。

**主要范围：** packaging scripts、install rules、third-party notices、SBOM、操作文档、version metadata。
**实现清单：**

- [ ] 两个 EXE、Qt runtime/plugins、必要 DLL/resource 在一个版本化 package 中。
- [ ] package manifest 封印 Git commit/tree、编译器/SDK/CMake/Qt/vcpkg ABI、Unified profile digest、文件 hashes。
- [ ] SPDX SBOM 与 `THIRD_PARTY_NOTICES` 覆盖 Qt 和全部 installed vcpkg package。
- [ ] 独立 verifier 拒绝缺失、额外、重复、越界、reparse 或 tampered entry。
- [ ] 用户指南只展示最终简化流程；历史实验 CLI 单独归档为开发者诊断。
- [ ] 项目自身 LICENSE 必须由维护者明确选择；未选择时不得在发布说明中声称开源许可。
- [ ] Release artifact 不提交到源码 Git，使用 GitHub Release 或外部 artifact 存储。

**最小验证：** 从 clean package 运行 `--version`、两个 GUI smoke、1 MiB 本地右屏恢复；验证 SBOM/notices/manifest；不重复 full CTest 和 64 MiB remote。
**退出：** package 可在目标机独立启动和验证；源码 tree clean；release notes 精确列出已执行与未执行门禁。
**提交建议：** `build(release): package unified pixelbridge candidate`

---

## 6. 关键实现不变量

### 6.1 Sender

- SessionId 来自 OS CSPRNG。
- 源文件在整个 Session 不可变；任何身份或内容变化立即停止。
- descriptor table 可以随 SegmentCount 增长，但 raw/encoded Segment bytes 仅双缓冲。
- canonical Wirehair descriptor 与 exact encoded bytes 在所有 Carousel pass 保持一致。
- durable lease 的高水位写入必须先于 ID 对外可见；崩溃可浪费 ID，不能复用。
- Present 不是 equation generation；重复显示不推进逻辑身份。

### 6.2 Receiver/storage

- 未绑定 SegmentDescriptor 的 Transport 不触发大分配/codec 创建；orphan cache 必须有界。
- active decoder 满只延迟，不驱逐和不污染 Session。
- 只有 raw digest 已验证的 Segment 才写 `.part`。
- 只有 `.part` flush + completed journal flush 后才释放 active decoder。
- resume 输入始终按不可信数据解析。
- Whole-file digest 与 final reopen 验证是 Completed 的必要条件。
- 永不覆盖、截断、删除已有最终文件。

### 6.3 Visual/capture/GPU

- Base/Fine/Chroma 的 erasure 相互独立。
- mixed old/new region 不跨 sequence 拼接。
- WGC frame 是 lease；归还后不能访问 surface。
- PB-owned texture、CaptureEpoch、adapter/device identity 必须贯穿 GPU work 与 admission。
- D3D11 immediate context 单 owner；`Flush` 不等于完成。
- provider 名称和模式永远不是解码输入。

### 6.4 真实性

- `VerifiedEncodedBytesPerUniqueFrame`：

```text
sum(最终成功发布的所有 Segment EncodedSize)
-------------------------------------------------
从正式 Session 建立到发布期间观测到的唯一逻辑帧数
```

- 只有 whole digest + safe publish + final reopen 成功的运行才产生该指标。
- 重复 capture 不增加分母；Control、repair、erasure、drop 的代价自然包含在结果中。
- Present rate、Encoder configured FPS、capture callback rate 都不能替代 UniqueVisualFPS。

---

## 7. 必须提问或停止的条件

遇到以下情况，不得自行选择一个高影响方案继续：

1. Base Luma 在正式 0.75x mandatory corpus 无法达到 16 KiB/unique frame；
2. 两种 wire/layout 方案都会改变公开 schema 或已冻结 ID；
3. 要删除历史 Profile、Golden 或证据，而不是从产品入口隐藏；
4. 大文件预分配策略会改变磁盘实际占用、用户确认或恢复语义；
5. Qt 的大文件确认、完成交互存在多种显著不同产品体验且需求未覆盖；
6. 需要更换 Qt/Wirehair/zstd/BLAKE3 基线或引入新生产依赖；
7. 项目 LICENSE、GitHub 可见性、release signing 或分发许可尚未由维护者决定；
8. 实屏目标无法严格限制在 `\\.\DISPLAY2`；
9. 真实远程环境要求 provider-specific decoder 分支才能通过；
10. 发现协议/持久状态向后兼容与当前正式 schema 冲突。

可先继续与上述决策无关的独立工作，但必须停止所有依赖该选择的修改。

---

## 8. Goal 模式提示词模板

后续可直接复制以下模板，只替换目标编号：

```text
请按 docs/UNIFIED_VISUAL_LARGE_FILE_IMPLEMENTATION_ROADMAP.md 执行 Gxx。

要求：
1. 先读取 AGENTS.md、路线中 Gxx 的前置条件/范围/退出标准，并检查 git status；
2. 核对前置目标是否已有提交和证据，若没有则停止并报告，不要越级；
3. 只修改 Gxx 允许的文件和公共接口，不顺手实现后续目标；
4. 遵守本文的测试预算，只运行 Gxx 指定的最小验证，不跑全量回归或实屏 Gate，除非 Gxx 明确要求；
5. 不覆盖、不暂存 docs/PHASE1_GATE_REPORT.md，不 broad-stage，不 amend/rebase/reset/push；
6. 退出前审查 diff，使用显式路径暂存并创建一次独立提交；
7. 报告完成项、关键证据、测试命令与结果、未执行门禁、提交哈希和推荐下一目标；
8. 若发现影响架构/协议/UI/兼容性的高影响不确定性，先向我提问并停止依赖该决策的实现。
```

### 8.1 目标完成报告模板

```markdown
## Gxx 结果

- 状态：DONE / PARTIAL / BLOCKED
- 提交：<hash 或未提交原因>
- 完成内容：
- 关键事实证据：
- 执行的测试：
- 明确未执行：
- 新发现风险：
- 工作区状态：
- 下一目标：Gyy
- 需要用户配合：无 / 具体步骤
```

---

## 9. 最终验收清单

### 9.1 功能

- [ ] `PixelBridgeEncoder.exe` 和 `PixelBridgeDecoder.exe` 可在目标 Windows x64 环境运行。
- [ ] Encoder 基础页仅文件/FPS/开始/停止；Decoder 基础页仅目录/ROI/开始/停止。
- [ ] 0-byte、跨 8 MiB、多 Segment 与 20 GiB+ 能力成立。
- [ ] 双端跨进程恢复成立；source 变化和 corrupt resume fail closed。
- [ ] Unified 是唯一产品 Profile；旧 provisional descriptor 明确拒绝。
- [ ] WGC 默认且只在明确失败时 DXGI fallback。
- [ ] 0.75x..2.0x、letterbox、resize epoch、低于 0.75x 暂停合同成立。
- [ ] WholeFileDigest、安全发布、final reopen 与外部哈希一致。

### 9.2 正确性与性能

- [ ] mandatory transforms false accepted Control/Transport = 0。
- [ ] conflicting output = 0；错误最终文件 = 0。
- [ ] Base Luma 在 chroma-neutralized 条件达到 >=16 KiB/unique frame。
- [ ] clean/颜色可用路径是否达到 32 KiB 被准确记录。
- [ ] 实际 remote 1 MiB 与 64 MiB 恢复成功。
- [ ] 20 GiB headless 文件每个 Segment 均被真实触达。
- [ ] sender/receiver working set 不随总文件大小线性增长。

### 9.3 工程与发布

- [ ] 最终完整 Release CTest 仅在代码冻结后执行并通过。
- [ ] parser/resume/storage 定向 ASan 通过。
- [ ] 无已知 Critical/High 问题。
- [ ] 包含准确 SBOM、third-party notices、build identity 与可复验 package manifest。
- [ ] 项目 LICENSE/仓库可见性由维护者明确决定。
- [ ] 源码仓库没有 build tree、`.part`、resume、日志、包或私密运行证据。
- [ ] 所有未执行硬件/远程/长时门禁在发布说明中列出，未被写成已通过。

---

## 10. 路线维护规则

- 完成一个 Goal 后，只更新其状态、提交哈希、实测证据链接和必要风险；不要重写历史结果。
- 若 Goal 被拆分，用 `Gxx-a/Gxx-b`，不得改变后续目标的原始语义。
- 若产品决策改变，在本文顶部增加带日期的 supersession note，再同步总体设计；不能只改代码或某个测试。
- 新增测试应归入最近的三个检查点之一；没有明确故障模型的重复矩阵默认不加入。
- 性能阈值只能由用户明确改变，不能通过放宽断言、删样本、重分类 erasure 或改变分母来“通过”。
- 所有正式证据应记录 Git commit、build identity、fixture digest、环境边界和未覆盖项。
