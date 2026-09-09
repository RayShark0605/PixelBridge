# Step3-A：确定性离线像素／时序最小测试工具

## 范围与权威边界

本工具是 **Step3-A 本地工具**，不是产品媒体输入，不注册到产品 CMake，不改变 SC6-V3/layout 10、几何门、FEC、CRC、冲突处理、资源策略或安全发布语义。

链路为：

```text
生产 EncoderRuntime → 注入无窗口 presentation → 4 张封存 BGRA8 raster
    → 固定 ordinal/PTS 清单 → 已有 PBRemoteVisualSimulator
    → RecordedPixelSource（仅 BGRA8 像素与必要时序）
    → 原 RunRecordedPixelReplay（OfflinePixels、D3D11 WARP 软件设备）
    → 原 CaptureDemodulator / ReceiverPipeline
    → 整文件摘要 + 安全发布 + final reopen
    → 进程结束后，Python 独立逐字节、SHA-256、BLAKE3 验证
```

512 字节源文件及期望 payload **不传给 Decoder**。独立验证只在解码后读取输出，不回馈发送端或接收端。没有 socket、pipe、共享内存、剪贴板、窗口消息、隐式 ACK 或其他 payload 旁路。

Session ID 仍由生产 OS CSPRNG 生成。确定性边界是“同一份封存像素再执行两次”，**不是重新生成相同 Session ID**。重新运行生产 fixture 会得到新 Session，必须新建证据根，不能与旧数据冒充逐字节同一输入。

WARP 是软件 D3D11，不是本次硬件 GPU 测试。fixture 的 presentation 不创建窗口；回放不捕获显示器，不需要右屏，也不操作焦点、鼠标或键盘。所有本工具编排启动的子进程使用 `CREATE_NO_WINDOW`。

## 固定样例

输入共 4 张 1920×1080 BGRA8，`rowPitch=7680`，每张 8,294,400 B。颜色合同为规范 SDR RGB/full，不进行未声明的 RGB/YUV 转换。种子为 `0x0000535445503341`。

| 名称 | 固定变化 | 每轮观察数 |
| --- | --- | ---: |
| `clean` | 原始 ordinal `0,1,2,3` | 4 |
| `repeat-each` | ordinal `0,0,1,1,2,2,3,3` | 8 |
| `burst-loss` | 只保留 ordinal `0,3`，丢掉中间连续两个源帧 | 2 |
| `neutral-chroma` | 已有 BT.709 integer 色度中和，保留 alpha | 4 |
| `quantize-6` | B/G/R 各 6-bit uniform round-nearest 量化 | 4 |
| `bootstrap-conflict` | 从 `(ordinal+1)%4` 替换 Bootstrap B，矩形 `(1216,1000,608,64)` | 4 |
| `local-freshness-mix` | 从同一 donor 规则替换 freshness region 0，矩形 `(96,160,128,128)` | 4 |
| `marker-plus-one` | `(20,47)` B/G/R 各加 1，alpha 不变；保留 G1/G1B 已知几何失败 | 4 |
| `shift-right-one` | 既有 bilinear resample，scale=1、origin=(1,0)，border BGRA=(128,128,128,255) | 4 |

区域替换是合成新旧像素混合，不声明为真实远控的物理延迟。没有 codec、码率限制、GOP、4:2:0、模糊矩阵或 Holdout；这些仍属于后续未启动范围。

时序固定为 `timeBase=1/30`；普通样例 `PTS=ordinal*2,duration=2`，重复样例 `PTS=observationIndex,duration=1`。PTS 是人工合成 ordinal 时序，**不是生产 FrameSequence 插值，也不是采到的 Sender 时钟**。`SpanSeconds` 沿用 Step2 的首末 PTS 差，不包括最后一帧 duration。它与运行耗时完全分开，所有 channel goodput 均为 null。

## 当前交付与复验

- 源码：本目录；执行说明：[文档整理与历史取回记录](../../docs/DOC_HISTORY.md)。
- 新工具构建：`<repo>\build-remote-step3a-20260908-run01\Release\PBRemoteThroughputStep3A.exe`。
- 现存 Step2 构建依赖：`<repo>\build-remote-step2-20260908-run01`，只读复用；本次没有重建或替换产品双端、B0/M1 或现场包。
- 当前证据根：`<repo>\artifacts\remote-step3a-20260908-run01`。
- 冻结输入：证据根的 `frozen-pixels`；主试验 `suite-01`；最终分析 `analysis-04\analysis.json`；最终定向 guard 结果 `guards-03\RESULT.json`。
- `FINAL_MANIFEST.json`、`SOURCE_IDENTITY_FINAL.json`、`RUNTIME_IDENTITY_FINAL.json`、`FINAL_STATUS.json` 与 `POST_SEAL_VERIFY.json` 绑定最终交付；封存包为证据根的 `STEP3A_REPLAY_EVIDENCE.zip`，内含封存像素和独立工具运行文件，不是产品发布包。

包内的历史 RunReport 保留当时的绝对输出路径，不能为了迁移而改写。迁移后可使用包内 `runtime` 的工具和同一份 `frozen-pixels`，在新目录重新回放，再对新报告执行分析；原机既有证据的只读分析使用下面的原始路径。编排会拒绝最终路径与报告不一致的旧报告。

### 只复核已有证据，不启动 Decoder

以下 PowerShell 命令的新输出路径必须尚不存在；若已经存在，换用另一个明确的新目录，**不要删除旧目录**。

```powershell
& '<python>' -B '<repo>\tools\PBRemoteThroughputStep3A\run_suite.py' analyze `
  --fixture '<repo>\artifacts\remote-step3a-20260908-run01\frozen-pixels' `
  --suite '<repo>\artifacts\remote-step3a-20260908-run01\suite-01' `
  --output '<repo>\artifacts\remote-step3a-20260908-verify-new01'
```

### 从相同封存像素重新回放

`run` 固定串行执行 9 个样例×2 轮、合计 76 个观察；不延长预算、不自动重试失败项。随后单独运行 `analyze`。路径可改为新建的本地固定磁盘目录，但不能置于输入目录内，不接受 reparse/UNC 路径。

```powershell
& '<python>' -B '<repo>\tools\PBRemoteThroughputStep3A\run_suite.py' run `
  --exe '<repo>\build-remote-step3a-20260908-run01\Release\PBRemoteThroughputStep3A.exe' `
  --fixture '<repo>\artifacts\remote-step3a-20260908-run01\frozen-pixels' `
  --output '<repo>\artifacts\remote-step3a-20260908-replay-new01'

& '<python>' -B '<repo>\tools\PBRemoteThroughputStep3A\run_suite.py' analyze `
  --fixture '<repo>\artifacts\remote-step3a-20260908-run01\frozen-pixels' `
  --suite '<repo>\artifacts\remote-step3a-20260908-replay-new01' `
  --output '<repo>\artifacts\remote-step3a-20260908-replay-analysis-new01'
```

原生 `--run-case` 是低层像素入口，只检查固定 raster 尺寸与进程资源，不负责证明调用者提供的 provenance。正式复验应使用上面的 Python 编排：它核对 fixture 清单／哈希，C++ 在回放中以禁止写入/删除共享的句柄锁定 4 张输入，结束后再次核对输入和 EXE 身份。不能拿单独 CLI exit 0 代替完整分析或文件恢复 PASS。

### 构建新工具，不覆盖历史 build

```powershell
$build = '<repo>\build-remote-step3a-rebuild-new01'
if (Test-Path -LiteralPath $build) { throw 'Choose a new build root; preserve the existing build' }
& 'C:\Program Files\CMake\bin\cmake.exe' -S '<repo>\tools\PBRemoteThroughputStep3A' -B $build `
  -G 'Visual Studio 17 2022' -A x64 `
  -DPB_STEP3A_REPO=<repo> `
  -DPB_STEP3A_BASE_BUILD=<repo>/build-remote-step2-20260908-run01
if ($LASTEXITCODE -ne 0) { throw 'Configure failed' }
& 'C:\Program Files\CMake\bin\cmake.exe' --build $build --config Release --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
```

MSVC C++20 `/W4 /WX`，Qt-free；直接编译既有 `PBRemoteVisualSimulator/src/channel_transform.cpp`，链接现存 Step2 Release 库。运行需要配套 `blake3.dll`、`zstd.dll` 和现有 Windows/MSVC runtime。构建不下载依赖，不开启 vcpkg manifest 安装。

Python 使用已存在的 `blake3` 包，并核对空输入 KAT。生成新 fixture 必须通过无窗口进程启动，例如复用 `run_suite.invoke(exe, ["--make-fixture", newRoot], newLog, 60)`；不要通过会激活新控制台的 `Start-Process` 启动。

## 报告、资源与失败含义

- 两轮比较源 ordinal、PTS、完整 Simulator manifest、raw/domain BLAKE3、全部非时钟 frame trace、accepted payload digest、Receiver 非时间计数和阶段调用数，要求精确相等，不使用 epsilon。
- 仅排除实际 processing QPC、transform processing ns、阶段 duration，以及报告中的随机 run ID/绝对输出路径；不会删除几何、FEC 迭代、CRC、接受结果或 Receiver 状态差异。
- 分阶段分析保留 Bootstrap、geometry 原始代码、freshness、Base/Fine/Chroma FEC、显式 CRC 决定、Outer 以及最终发布门。早期拒绝后未到达的 FEC/CRC/Outer 是 **null**，不是“零错误”。GeometryStatus 的 `Rejected` 也可能由 Bootstrap 冲突导致；原始 frame erasure 代码不能被直接解释为物理裁切事实。
- `mediaDecode` 在本工具中计入读取与变换，不是 codec decode；StageDiagnostics 各阶段为 inclusive duration，不能相加当总耗时。
- tiny source 使用 DirectRepeat，第一张可用帧就完成恢复。后续帧继续解调，但 Receiver 的完成态不会继续积累完整输入流 telemetry；重复/突发丢帧样例**不证明多帧 Wirehair 恢复、接收器去重负载、迟加入或大文件性能**。
- 原生进程 2 GiB Job 限制；fixture 30 s 内完成；每个源固定 4 个输入文件，每场景最多 8 个观察；原 Step2 参数上限设为 16，从而真正读到 EOF 而非 prefix stop。
- 每条 trace ≤64 KiB，transform trace ≤1 MiB；原 Step2 frame trace 保留其 64 MiB 硬上限且本工具至多 8 行，Python 只接受 ≤1 MiB。JSON ≤1 MiB；输入/试验文件树 ≤1024 文件/128 MiB、总目录项≤2048、深度≤16；增量扫描空目录、增长中的文件也有界。guard 目录有刻意违反这些界限的负例，不得当作合法输入/试验树。所有大分配和 Receiver 文件发布仍使用原生产资源策略。
- 编排每场景处理 timeout≤120 s，全 18 次回放合计≤600 s；预算到期退出并保留部分证据，不重试、不增加限额。工具不保存全套失真 raster，只保存 4 张输入、逐帧 digest 和清单。
- `marker-plus-one` 的 `NOT_RECOVERED` 是已知几何失败复现，**不是产品通过，也没有被豁免**。Bootstrap 冲突及真实 1-pixel shift 负例也不得发布。
- `analysis-01` 的失败属于工具判定层级错误，原因、修正前源码和日志保留在 `context/analysis-01-failure.json`、`context/tool-source-before-analysis-correction.zip`、`logs/analysis-01.log`；现存回放数据未更改，未为修正分析重新解码。

当前结论仅为 `LOCAL_STEP3A_COMPLETE`；整个 Step3 为 `PARTIAL`，现场 `NOT_RUN`。后续 Step3-B、G2、Step4 或实屏工作都需要新的明确确认。
