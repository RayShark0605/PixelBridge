# Unified Decoder 产品工作流（G16）

> 状态：2026-09-04，G16 的定向 application/controller/offscreen GUI smoke 验证通过。
> 边界：按用户确认扩展了真实 Unified 接收接线，不只是 Qt 页面；未运行真实 selector/capture、GPU parity 重跑、实屏/远程或大文件门禁。

## 1. 产品入口与公共接口

无参数运行 `PixelBridgeDecoder.exe` 打开 Qt Widgets 界面。基础区只有输出目录、选择 ROI、开始/停止，以及真实文件恢复状态。

- `MakeUnifiedDecoderConfig(outputDirectory, region)` 固定 `PB-Unified-LC4-V1`、layout 8 和 `CaptureBackend::Auto`。没有 Encoder FPS、Profile、显式 backend、provider 或 Replay 选择。
- `DecoderApplicationController` 只负责启动/停止、转交大输出确认及轮询 `DecoderSnapshot`；Qt 不实现解调、协议、FEC、写入或发布。
- 只有点击“选择 ROI”才调用既有 PMv2 `SelectScreenCaptureRegion`；高级页的物理坐标后备调用同一个 `ResolveScreenCaptureRegion`。开始时重新解析 ROI，过期/跨屏/不合法区域拒绝启动。
- Unified 准入范围是单显示器、identity rotation、`1440..3840 × 810..2160` 物理像素的 ROI；这是资源和几何准入边界，不表示任意其中区域都能解码。缩放/letterbox/locator 的实际可用性仍由既有 Unified 解调规则判定。
- GUI 只读显示原始文件名/大小、verified Segment/bytes、已验证原始字节 goodput、ETA、Session/resume、实际 backend/fallback 原因和 geometry。进度不是收到的 Transport 数，也不是最终发布成功。
- `QSettings` 只保存窗口 geometry、`unifiedOutputDirectory`、`unifiedAdvancedExpanded`；不保存 HMONITOR、协议状态或确认授权，旧 Profile/backend 设置不影响产品策略。
- `DecoderConfig` 的历史默认、原显式 CLI/Replay 诊断入口保持兼容。本次迁移的是 GUI 及显式 `MakeUnifiedDecoderConfig` API；**没有声称 Decoder 默认 CLI 已切换 Unified**。

## 2. 真实接收链路与有界 admission

```text
选定物理 ROI
  -> G14 Auto: WGC / 明确失败后 DXGI
  -> CaptureNormalize / G11-G12 CaptureDemodulator
  -> UnifiedFrame 的 Bootstrap + compact accepted mixed blocks
  -> 同一个 ReceiverPipeline
  -> ReceiverIngress / 大输出确认 / resume journal
  -> 已验证 Segment 写入 PBStorage
  -> WholeFileDigest / 不覆盖式 publish / final reopen
  -> DecoderSnapshot -> Qt
```

1. 活动路径仍是原 `DecoderRuntime::Run` 和 `ReceiverPipeline`，只在 Unified 分支消费已有 `acceptedUnifiedBlocks`；没有平行解码器或隐藏 payload 通道。
2. 核对正式 Profile/layout、Bootstrap、capture domain/observation/source/slot generation，以及固定 accepted 数量上限 31。erased frame 不得携带 accepted bytes；过期、乱序或 TelemetryOnly 结果不向 Receiver 提交 payload。
3. 在 Receiver mutation 前检查 compact array 的 slot 唯一性、长度、carrier kind、Control Base Luma 区域、Control/Transport 独立解析和 SessionTag；SessionDescriptor 必须通过原 ReceiverResourcePolicy 并绑定 Unified。
4. 只保留一个 FrameSequence 的最多 31 个 accepted block 和 admission 标志，不缓存像素或帧历史。同一不可变帧的重复观测可以补足缺失 carrier，但相同 slot 的不同合法字节必须失败；旧重复帧不能替换当前缓存。跨帧相同 Outer identity 的冲突仍交由原 Receiver 拒绝。
5. 不依赖 compact array 的返回顺序：先 Session Control，再其他 Control，最后 Transport。首个正式 Session（包括等待大输出确认的 pending Session）锁定后，其他 Session 的帧不会改变当前恢复目标。
6. 大输出未获确认时不 admission Segment Control/Outer payload；同一帧中尚未 admission 的一致 carrier 可在确认后的后续观测中继续处理。最终输出仍必须满足原 Receiver/Storage 的全部摘要与发布条件。
7. capture epoch 切换只清除帧级/geometry 观测，不清除已验证文件状态；复用 G14 的旧 owner drain/join、lease 退出、新 source identity 和递增 CaptureEpoch。重建原 CaptureDemodulator，不迁移旧 GPU owner。

`DecoderRuntimeServices` / `DecoderDemodulator` 是应用层的 OS/GPU 依赖边界。默认 factory 是既有 native capture 和 CaptureDemodulator 的薄适配器；没有 GUI/CLI mock 输入开关。非显示测试只替换这两处边界，使用同一生产 Receiver、确认、journal 和 PBStorage 路径。

### 固定资源预算

- demod：4 slots、128 个结果的有界队列、256 MiB resident cap；最大 ROI 的预算计算为 **216,628,760 bytes**。
- Unified capture resident cap：1 GiB；ROI ring/scratch cap：384 MiB。Auto 必须同时预检 WGC 和 DXGI，DXGI 还预留其支持的高位深源格式转换空间。
- 最大 ROI 的四份 BGRA 输出加四份 R16G16B16A16 scratch 为 `3840 × 2160 × (4 + 8) × 4 = 398,131,200 bytes`，在 384 MiB 上限内；不会沿用不够用的旧 128 MiB ROI cap。
- 上述都是 admission/reservation，不是运行时测得的显存峰值；本次没有实际高位深或硬件 capture 验证。其他历史 Profile 的预算不变。

## 3. 确认、停止、恢复与完成

- 正式 Session 文件大小超过产品默认 4 GiB 确认阈值时，进入 `AwaitingLargeOutputConfirmation`。显示实际文件名、大小、目录；默认和 Escape 都为拒绝。
- Qt 对 `(RunGeneration, RequestId)` 只弹一次，并在展示和提交时核对当前请求；重复 descriptor 不重复弹窗，过期 run/request 不生效。沿用 G05 授权范围：显式 Stop 后重新 Start 是新 run，会重新确认，不跨 run/进程或输出目录复用许可。
- 拒绝会停止该 run，不创建 `.part` 或 `.resume`，不接收 Outer payload。已有恢复文件不会被拒绝动作删除。测试服务只允许降低阈值以使用小 fixture，不能提高默认阈值或禁用必需确认；产品无此设置。
- Waiting/Stalled 保留 Session、verified Segment/bytes 和 journal，不根据无画面/停滞时间自动停止。正常 Stop/关闭停止捕获并 join owner，保留可恢复状态；新 run 验证 journal 与已完成 Segment 后继续。
- `Completed` 来自原 Storage 的完整成功路径：Segment digest/bounded decompression、WholeFileDigest、同目录不覆盖式发布、最终文件重新打开并核对长度和摘要。GUI 不以 FEC 成功或进度 100% 代替完成。
- 完成区显示最终路径、长度、BLAKE3，并提供显式“打开完成文件所在目录”；不弹另存为，不自动启动 Explorer。文件名、错误、路径与确认文本均按纯文本显示。
- 新增快照字段仅为 G16 需要的 `originalFileNameUtf8`、`verifiedSegmentCount`、`geometryStatus`。没有修改 run_report/JSON Golden/PBTelemetry schema；Unified 不再进入历史 LF4 专用 metric 汇总，逐 lane 指标与最终报告真实性仍由 G17 收口。

更细的 journal、确认门及 rename 恢复合同见 [Decoder 可恢复乱序写入](DECODER_RESUMABLE_RECOVERY.md)。

## 4. 最小验证与可复现证据

前置提交均已核对为当前 HEAD 的祖先，且原证据存在：G05 `5d51d0ef0f7ac78bb69bbb17e7e5338c7fdb4504`、G12 `3044f2330655f157ec40ebcd0007c2ec2b5902b8`、G14 `38911d60a6757bd5112185089d6bc630385d0443`。本次没有重复执行这些目标的硬件门禁。

在仓库根目录执行：

```powershell
cmake --build build-unified-release --config Release --target PixelBridgeDecoder PBApplicationTests --parallel 6
.\build-unified-release\tests\PBApplication\Release\PBApplicationTests.exe '[application][g16]' --rng-seed 16092026 --durations yes
.\build-unified-release\tests\PBApplication\Release\PBApplicationTests.exe '[g05],[application][decoder-state],[application][g14]' --rng-seed 16092026
ctest --test-dir build-unified-release -C Release -R '^PixelBridgeDecoderGuiSmoke$' --output-on-failure -V
```

| 验证 | 最终结果 | 证据边界 |
| --- | --- | --- |
| Release 定向构建 | 成功 | PixelBridgeDecoder、PBApplicationTests；Qt 部署有 `VCINSTALLDIR` 未设置警告，退出码 0 |
| G16 application | 5 cases / 170 assertions | 0-byte、20,000-byte RAW/Wirehair、8 MiB+1-byte zstd/RAW 双 Segment；真实 Encoder raster 经 CPU oracle 的 accepted bytes 进入真实 Decoder runtime |
| G05/decoder-state/G14 窄兼容组 | 17 cases / 296 assertions | 原确认/恢复状态机与 mock capture policy/controller，无全量回归 |
| Decoder GUI smoke | 1/1，0.85 s | offscreen、真实 widget/controller/runtime，替换 selector/capture/demod/目录打开边界；不显示原生窗口 |

G16 定向覆盖：最大/最小 ROI 与预算、非法配置/阈值拒绝、compact array 逆序、最终文件独立 reopen/完整字节/BLAKE3、pending Session 锁定、重复/过期确认、拒绝无文件、重复帧和外来 Session、确认后同帧剩余 carrier、Stop/resume、WGC 初始化失败，以及已验证首个 Segment 后的 AccessLost→DXGI。停滞用实际无新观测等待触发，确认活动状态和已验证字节不变。负例覆盖 count 越界、slot 重复、Profile 不符、同帧合法 Transport 冲突及后续帧的同 Outer identity 冲突。

GUI smoke 使用隔离的 QSettings、临时源文件/输出目录和降低到 4096 bytes 的确认阈值。覆盖默认不启动 selector、显式 ROI 动作、确认一次、首 Segment 后停止、保留 journal、新 run 再确认并恢复发布、显式打开目录动作、另一个输出目录拒绝且为空。`PB_BUILD_TESTS` 才编入测试支持；offscreen 插件在 QApplication 前检查，缺失时直接失败而不弹平台错误窗口。没有提高原 15 s smoke 超时。

最终本地日志（构建树产物，不纳入提交）：

- `build-unified-release/g16-build-final.txt`
- `build-unified-release/tests/PBApplication/g16-workflow-final.txt`
- `build-unified-release/tests/PBApplication/g16-controller-compatibility.txt`
- `build-unified-release/g16-gui-smoke-final.txt`

初次构建遇到 `ScreenCaptureFrameMetadata` 无整体相等运算符，已改为明确的 capture identity 字段比较。首轮最大 ROI 验证失败后，一度误判为 demod cap 不够；精确预算确认阻塞来自 DXGI 高位深 ROI scratch，已撤销不必要的 512 MiB demod 调整，仅增加 Unified ROI 固定预算。随后新增 AccessLost 用例发现 mock 的 delivered 计数在 backend 重建后清零，已把等待条件改为当前 backend 基线加本次提交帧数，而非跨 backend 累计值。最终日志均通过；没有放宽协议/冲突断言或跳过用例。

未执行：全量 CTest、真实 ROI selector/capture、WARP/GPU parity 重跑、native/实屏/双屏 Gate、实际进程 crash 注入、远程 provider、20 GiB 或安装包/提交后嵌入身份复验。测试中的像素由实际 Encoder runtime 生成，但没有经过显示器、视频传输或 GPU capture；因此本结论不是现场端到端或性能认证。下一目标为 G17。
