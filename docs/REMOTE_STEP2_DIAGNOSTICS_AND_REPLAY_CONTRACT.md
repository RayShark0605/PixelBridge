# Step2 — 逐阶段诊断与 OfflinePixels 回放合同

日期：2026-09-08。仅对应已确认的 Step2 本地实施，不授权 Step3、实屏或非本机测试。

## 1. 范围和证据等级

- 当前 wire/Profile 仍为 `PB-Unified-SC6-V3` / layout 10。无协议、FEC、Golden、quality gate 或 ReceiverResourcePolicy 修改。
- `PBUnifiedRecordingReplay` 是独立实验工具。产品 Encoder/Decoder 的 payload 输入和 GUI 参数不增加录像入口、旁路、反向反馈或隐式 ACK。
- FFmpeg 仅属于工具独立 manifest；主工程 `vcpkg.json` 和生产应用链接依赖不增加 FFmpeg。
- 所有结果标记 `OfflineRecordingDiagnostic`、`fieldStatus=NOT_RUN`。WARP 与软件视频解码耗时不是真实 GPU、远程链路或现场吞吐。
- 原始录像的 capture backend、光标状态、捕获时钟均为 `unknown/null`。不能从文件帧率或解码成功倒推这些属性。

## 2. 用户明确确认的 OfflinePixels 契约

`CaptureBackendKind::OfflinePixels` 描述**当前解码后的像素输入**，不冒称原始 WGC/DXGI 捕获。

工具同时设置消费者 `offlinePixelsOnly=true` 与输入 `backend=OfflinePixels`；原始 cursor 保持 `Unknown`，`isCursorExcluded=false`。画面中的光标像素和其他失真一样接受原有 Bootstrap、几何、freshness、FEC、CRC 检查。

默认 live 配置仍为 `offlinePixelsOnly=false`，只允许 WGC/DXGI 且必须证明光标排除。两层 Submit 均调用相同的严格输入契约检查；工具消费者不能通过 live `ValidateConfiguration`。工具选项不进入产品配置。

## 3. 两种时间不能混用

1. **录像时间**：逐观察保存未经插值的整数 PTS、time base 和 duration。PTS 可以相等，不能后退；time base 不得途中改变。不按画面身份去重，不把视频名 `30Hz` 当作采样真值。
2. **处理时间**：本机 steady/QPC 用于 GPU retirement、Receiver 操作、处理 watchdog 和分阶段耗时。当前流水线串行按解码后的呈现顺序处理，不模拟原始 live arrival/backlog。

保留已有 offline demod 的 60,000 ms 处理时效上限和每 GPU stage 3,000 ms retirement 等待；live 250 ms 上限未修改。这不是扩大录像信号的 freshness 容忍度。像素中携带的 sequence/freshness 判断保持原样。

回放输出的 live goodput、`uniqueVisualFps` 均为 null；内部接纳计数可保留。调用者不能将 `frames/processingMilliseconds` 当作链路吞吐。

## 4. 有界媒体适配

固定依赖：vcpkg baseline `75672db6bd812b060482b0f00b5a16b18a0c0f07`，FFmpeg `8.1.1#2`，features `avcodec,avformat,swscale`，无默认 feature、无 GPL feature。实际 ABI 和 DLL 哈希另存证据身份。

输入只允许本机 fixed-drive 文件。Win32 `FILE_SHARE_READ` 持有输入，禁止覆盖/修改；只提供自定义只读 AVIO，拒绝外部 `io_open`。只选一个 video stream；音频包跳过并计数，不进入 Receiver。

| 约束 | 硬上限或固定语义 |
| --- | --- |
| 输入文件 | 2 GiB |
| 容器/视频 | Matroska；H.264 或测试夹具 FFV1 |
| 几何 | 固定 1920×1080；不拉伸、不裁切补齐 |
| H.264 色彩 | YUV420P，limited、BT.709 primaries/transfer/matrix、left chroma；显式 swscale 到 full BGRA |
| FFV1 夹具 | BGRA/BGR0；BGR0 的 alpha 明确置 255 |
| 视频帧 / PTS 跨度 | 7200 / 120 秒 |
| CPU 解码 | 1 thread；不启用硬件视频解码 |
| Packet | 每包 16 MiB、总数 50,000 |
| 探测 | 4 MiB、128 probe packets、1 秒 analyze duration、最多 4 streams |
| 额外 AVIO 读取量 | 4 GiB；输入哈希另有 2 GiB 输入上限 |
| codec 单次分配 | replay 32 MiB；不是总内存配额 |
| 工具进程 commit | Win32 Job 强制 2 GiB；建立限制失败立即退出 |
| 夹具生产器 | FFV1 编码器单次分配 192 MiB；仍受同一 2 GiB Job 上限约束 |
| GPU 交接 | 串行、最多 2 slots、已有 bounded retirement；不保存整段像素数组 |
| 处理预算 | 900 秒，不自动增加 |
| trace | 64 MiB 总量、64 KiB/行、最多 7200 行 |

FFmpeg 的错误级日志也会使读取失败；这是为了捕获 Matroska 在截断后可能仍返回 EOF 的错误路径。仅收到 decoder EOF 不证明录像覆盖了完整传输。

`--replay-prefix` 是固定前 36 帧的小窗口诊断；输出 `prefixLimitReached=true`，不冒称 EOF。不循环录像，不跳帧后继续，不向 Decoder 提供源文件、预知 Session/Descriptor、expected payload 或方程。

## 5. 诊断合同

`PixelBridge.StageDiagnostics.1` 使用固定大小计数器与 steady nanoseconds。采样 stride 为 1..4096；每 stage 最多 2,000,000 calls；单次持续时间至多 1800 秒；非法配置/计时/溢出导致 sticky invalid 和 null 耗时，不改变 payload 接纳。

计时为 **inclusive duration**，嵌套项不能求和。零次观察输出 `StageNotObserved`，不是 0 ms。失败尝试也计入 calls。

| 名称 | 精确范围 |
| --- | --- |
| outerGenerate | Sender 单 slot 的 Outer payload 生成及 Transport 组装/序列化 |
| innerPackEncode | Unified slot pack 与 Inner 编码 |
| raster | 从已编码数据生成规范画面 |
| fullscreenCompose | 全屏承载画面合成 |
| submitCall | SubmitFrame 调用；不是最终 Present |
| senderBackpressure | pending frame 导致的 1 ms 等待片段；不是所有不可用 frame-permit 时间 |
| presentationUpload / presentCall | 实际 backend Upload / Present 调用耗时；不能证明实屏唯一帧率 |
| mediaDecode / replayUpload / replayDemod | 工具媒体读取转换、纹理上传、完整生产 demod staged 执行 |
| baseFec / fineFec / chromaFec | 相应 lane 的逐 slot metric 准备、syndrome 和 Inner FEC 处理 |
| slotProtocol | 已恢复 information 的协议、padding、CRC、身份检查 |
| receiverProcess / outerReceive | ReceiverPipeline Process / ReceiverIngress ReceiveDataBlock，包含内部恢复 |
| segmentVerifyDecompress | encoded digest、解压与 raw digest；不冒称 Outer 求解时间 |
| segmentWrite / storageFlushCheckpoint | verified raw 数据写入 / storage flush 与 checkpoint |
| finalPublish | 原有整文件验证、安全发布与最终重开；不改错误处理 |

Decoder instrumented report 额外输出 `stageCounters`，直接来自生产 snapshot：新方程、重复方程、已完成、冲突、资源拒绝/延后、orphan、高水位以及现有 Bootstrap CPU、demod GPU、post-GPU CPU 总量。无法单独测得的 capture 或 GPU 子阶段不制造数据。

工具 trace 保存逐 slot FEC/CRC/rejection/iterations、accepted payload BLAKE3、freshness 和 Bootstrap 原始拒绝诊断；仅输出摘要，不将摘要或真值回送解码器。

## 6. 入口与复现

在 `<repo>` 使用 PowerShell。每次证据路径必须是**不存在的新目录**，不要复用下列示例中的历史目录。

```powershell
# 单独准备 FFmpeg，完整命令见本次 evidence build-logs/ffmpeg-install-01.log 对应记录。
<vcpkg-root>\vcpkg.exe install --triplet x64-windows `
  --x-manifest-root=<repo>\tools\PBUnifiedRecordingReplay\deps `
  --x-install-root=<new-deps-installed> --x-buildtrees-root=<new-deps-buildtrees> --x-packages-root=<new-deps-packages>

cmake -S <repo> -B <new-build> `
  -DPB_BUILD_TESTS=ON -DPB_BUILD_TOOLS=ON -DPB_BUILD_UNIFIED_RECORDING_REPLAY=ON `
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake `
  -DPB_QT_ROOT=<qt>6.10.1/6.10.1/msvc2022_64 `
  -DPB_STEP2_FFMPEG_ROOT=<new-deps-installed>/x64-windows
cmake --build <new-build> --config Release --target PBUnifiedRecordingReplay

& <tool.exe> --make-fixture <new-fixture-root>
& <python> <repo>\tools\PBUnifiedRecordingReplay\verify_fixture.py `
  --tool <tool.exe> --fixture <fixture-root> --new-root <new-check-root>
& <tool.exe> --replay <repo>\30Hz_Remote.mkv <new-recording-run-root>
& <tool.exe> --replay-prefix <repo>\30Hz_Remote.mkv <new-prefix-run-root>
& <python> <repo>\tools\PBUnifiedRecordingReplay\analyze_trace.py `
  --run <recording-run-root> --new-output <new-analysis.json>
```

`--replay-no-diagnostics` 仅关闭可选 stage timers，不关闭 trace、准入或正确性检查，用于小夹具开关一致性验证。没有实屏操作、捕获窗口或输入自动化。

## 7. 未运行与不外推的部分

- 未运行实屏、右屏拓扑探测、Citrix、非本机对照、native GPU 矩阵、长压力或完整 CTest。
- 未测现场诊断开销；诊断默认关闭。生产 GUI 未新增现场激活入口，不能把独立工具冒称为重新封存的 M1 包。
- 输入与错误检查有定向测试；2 GiB 文件、7200 帧满额、进程内存耗尽和 GPU timeout 不通过制造大文件/长压力默认重测，保留静态边界审查限制。
- 任何 future live 测试、门限/架构改变或 Step3 均需重新计划与主人确认。
