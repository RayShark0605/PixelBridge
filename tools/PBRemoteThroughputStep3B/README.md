# Step3-B：固定有状态码流与多帧恢复对照

> **维护范围 / Scope (2026-09-17):** 本文保留该模块的协议/工具/测试参考，不再作为项目当前路线或发布状态入口。当前模式、单屏接收、预算、日志与完整文件证据见 [中文文档](../../docs/README.md) / [English documentation](../../docs/README.en.md)。历史日期、Gate、现场坐标与阶段参数仅适用于当时记录；不应直接复制到新环境。
> This is a module/tool/test reference, not the current release roadmap. Use the bilingual index for current behavior and validation boundaries; historical gates/settings are not universal defaults.

## 范围与实际结论

本工具仅为 `SyntheticOfflineDiagnostic`，不注册到产品 CMake，不给产品 Decoder 增加文件／媒体输入，不修改 SC6-V3/layout 10、几何准入、FEC、CRC、冲突拒绝、资源策略或文件发布规则。

2026-09-08 已获准的唯一条件结果：**64 KiB 原始多帧恢复 PASS；两次固定 codec 重建精确一致，但均 NOT_RECOVERED**。码流符合本轮属性检查，原生产几何门拒绝全部 30 个 codec 观察，未进入数据解调。整个 Step3 仍为 `PARTIAL`，现场 `NOT_RUN`。这不是提速、Citrix 模型校准、Holdout 或 codec 恢复验收通过。

执行记录：`<repo>\docs\REMOTE_STEP3B_EXECUTION_2026-09-08.md`。

## 输入与单向边界

```text
64 KiB 确定性源文件
  → 原生产 AuditUnifiedSource / EncoderRuntime
  → 无窗口 FrozenPresentation，封存 30 张实际提交的 BGRA raster
      ├─ 原始像素 → RawSource
      └─ 独立 FFmpeg 软件编码 → 码流检查 → 原 RecordingMedia 软件解码
  → RecordedPixelSource（只有像素、PTS、duration）
  → 原 RunRecordedPixelReplay（OfflinePixels / D3D11 WARP）
  → 原 CaptureDemodulator / ReceiverPipeline
  → whole-file digest / safe publish / final reopen
  → 子进程结束后外部逐字节、SHA-256、BLAKE3 验证
```

Decoder 不接收源文件、Sender ledger、expected payload 或预知方程；外部真值比较不回馈生产接纳逻辑。进程标准输出管道只保存诊断，不承载 Decoder payload。没有 socket、剪贴板、共享内存、窗口消息或隐式 ACK。离线像素文件是显式实验输入，不是产品双端隐藏传输通道。

原恢复库只读来自 `<repo>\build-remote-step2-20260908-run01`；媒体源码直接编译原 `<repo>\tools\PBUnifiedRecordingReplay\recording_media.cpp`。软件 WARP 不捕获屏幕；fixture 不创建窗口，不操作显示器、焦点、鼠标或键盘。

## 冻结参数

| 项目 | 唯一固定值 |
| --- | --- |
| 文件内容 | 65,536 B；2048 个 BLAKE3 32 B 块；输入为 `b"PixelBridge.Step3B.Source.1\0" + LE64(0x535445503342) + LE64(blockOrdinal)` |
| 生产 ledger | 单 Segment，实际压缩为 Raw，raw/encoded 均 65,536 B；不强制压缩模式 |
| Outer | 生产 WirehairV2，block 1,314 B，K=50；不满足则停止，不改 seed |
| Session | 原 OS CSPRNG；生成一次后冻结像素，不声称重新生成相同 Session |
| 画布 | 1920×1080 BGRA8，rowPitch=7680，规范 SDR RGB/full |
| 短序列 | 30 张不同的生产提交 raster；Sender 配置 15 Hz；合成媒体 15/1 fps、2 秒 |
| 原始总长 | 248,832,000 B；按帧流式读写，不缓存整段 |
| Codec | 软件 libx264 / H.264 High / Level 4.0，Matroska，仅一条视频流 |
| 像素与颜色 | 8-bit yuv420p，limited，BT.709 primaries/transfer/matrix，left chroma，SAR 1:1，progressive |
| 码率与 VBV | target/min/max 均 8,000,000 bit/s；VBV 4,000,000 bit；init 0.90；nal-hrd=cbr |
| GOP | closed GOP 15；ordinal 0、15 为 IDR，其余 28 张 P-picture；B=0、ref=1、scenecut=0、intra-refresh=0 |
| 编码基线 | veryfast / zerolatency；deblock 开启，offset 0:0；rc/sync lookahead=0，mbtree=0 |
| 确定性设置 | encoder/lookahead/filter 单线程；sliced threading=0；x264 asm=0、FFmpeg cpuflags=0 |
| 重建次数 | 同一份冻结像素，两次相同条件；不是参数矩阵 |

显式 RGB→YUV 转换不额外做 gamma、HDR、锐化或降噪。最终固定 filter 为：

```text
scale=1920:1080:flags=bilinear+accurate_rnd+bitexact:in_range=full:out_range=limited:out_color_matrix=bt709:out_h_chr_pos=0:out_v_chr_pos=128,format=yuv420p,setsar=1,setparams=range=limited:color_primaries=bt709:color_trc=bt709:colorspace=bt709
```

完整 argv 由本目录 `codec.py::encode_arguments` 唯一生成，并保存在各次 `logs/*-encode.process.json`；实际 x264 有效参数、ffprobe、`trace_headers` 结果均保留。`context/parameters-v2.json` 是最终已确认参数，旧 `parameters.json` 不覆盖。

初次只设置输出颜色选项时，实际 SPS primaries/transfer 为 2（unspecified），检查失败。先暂停并得到用户“补齐颜色标记”的确认，再增加上述 `setparams`。失败 `codec-01` 完整保留，未送入 WARP；两次正式对照为 `codec-v2-01`、`codec-v2-02`。原媒体入口也对该错误颜色样例作了实际拒绝验证。

### FFmpeg 与 DLL 隔离

- 编码／检查：原 `<vcpkg-root>\buildtrees\ffmpeg\x64-windows-rel\ffmpeg.exe`、`ffprobe.exe`，FFmpeg 8.1.1；实际 x264 164 r3108 31e19f9。
- 编码依赖从 `<vcpkg-root>\installed\x64-windows\bin` 递归冻结非系统 PE 导入闭包，共 64 个文件；在证据根 `codec-runtime`，原路径和冻结 SHA-256 见 `context/codec-runtime.json`。
- 原 Step2 媒体依赖来自 `<repo>\artifacts\remote-step2-20260908-run01\deps\installed\x64-windows`，有 H.264 软件 decoder，但没有 libx264 encoder。本轮不替换它。
- 两套 DLL 分进程、分目录；编码子进程 PATH 仅冻结 codec 目录及 System32。Windows 系统 DLL 不打包，保留依赖清单。
- 本工具不是认证视频 Profile；选择 Matroska 复用原入口及本机 `nal-hrd=cbr` 能力，不新增 MP4 路线。

### PTS 与检查语义

输入 PTS=n、time base=1/15、duration=1。实际 Matroska time base=1/1000，PTS=`(n*1000+7)//15`（0、67、133、200…）；本轮 duration=66，合同仅接受 66/67。解码保留媒体实际 PTS/duration，不重新合成时钟。

标称时间为 2 秒；实际首末 PTS 差为 1.933 秒。两者都不是处理耗时或现场吞吐。`liveChannelGoodput`、`simulatedVerifiedGoodput`、`originalCaptureClock` 为 null。

检查器核对 stream/frame 颜色、帧数、PTS/DTS、I/P/IDR、SPS/VUI/HRD、packet 哈希、实际码率及短窗口 `bits <= rate*duration + CPB`。这只是必要的工程包络，**不是完整 Annex C HRD 标准认证**。x264 对 left/location 0 可省略 chroma location syntax；仅在 presence=0 且不存在相应字段时接受 H.264 默认 0，并要求实际 stream/frame 都为 left，不接受未知颜色。

## 恢复与失败含义

- 前 3 帧：实际已取得有效 Bootstrap 和 32 个 Outer unique symbols，但不得恢复／发布；3×15×1314=59,130 B 小于实际 encoded 65,536 B。
- 完整原始 30 帧：通过全部整文件发布门与外部 byte/SHA-256/BLAKE3 校验，证明不止首帧恢复。
- 两次 codec：实际媒体完整有效，容器、访问单元／参数集、解码像素／PTS、非时钟解调和 Receiver 结果相同；文件都未恢复。
- codec 30/30 双份 Bootstrap 字节有效，但原准入返回 BootstrapErasure=12（InvalidGeometry）、frameErasure=3、geometry=4（Rejected）。没有修改几何门，也不宣称已定位每帧同一个具体数值公式。
- 后续 freshness/FEC/CRC 未到达，报告为 null，而不是零错误。codec accepted payload=0，因此外部 payload 对照次数=0，不能宣称 false-accept 安全认证或 BER=0。
- 此 A/B 同时包含显式颜色转换、4:2:0 和有损编码，不能把所有差异单独归因为码率或量化。
- 原生 exit 0 只表示完整处理且没有媒体／运行错误，不等于恢复成功；必须另查 `publishedAndReopened`、Receiver 发布门及外部文件验证。发生媒体尾部错误时，保留此前真实发布状态，整轮仍视为媒体失败，不能伪造撤销。

## 资源与测试边界

| 资源 | 上限／实际控制 |
| --- | --- |
| 正常解调观察 | 3 prefix + 30 raw + 30 + 30 codec = **93／93**，本轮不再追加 |
| 合法原始像素 | 精确 248,832,000 B；外层 256 MiB 上限，单 BGRA 8,294,400 B，流式 |
| 单码流 | 16 MiB；媒体 AV 单次分配 32 MiB |
| 子进程 | 串行；启动 suspended，加入 2 GiB process/job commit 限制后 resume；kill-on-close；CREATE_NO_WINDOW |
| 超时 | fixture/encode 各 60 s；prefix 30 s；完整 replay 90 s；检查各 15 s；整个受监管试验／负例子进程账本累计 600 s |
| 诊断 | 单 trace 行读入≤64 KiB；frame trace 写入≤4 MiB；pixel trace/JSON/普通日志≤1 MiB；header trace≤8 MiB |
| 证据输入树 | 合法输入≤1024文件、2048条目、深度16；≤1 GiB；归档另计≤1 GiB |
| 输出 | create-only；拒绝现存运行目录、reparse 与非固定本地磁盘；raw 持有禁止写／删共享的文件句柄 |

文件大小使用采样终止和退出后硬性准入检查，**不是文件系统硬配额**；峰值 commit 也不是 GPU 显存测量。600 s 是受监管子进程的累计耗时预算，不是包括编辑、哈希、打包和人工确认的整个研发墙钟时间。原 Receiver 资源策略保持不变。

负例树故意包含深度17的拒绝样例。最终归档可保留这个非法输入；归档清单的有界目录扫描不等于允许该树进入产品或合法实验输入。不得为封存而把合法输入深度上限调大。

已执行 34 个独立 guard 方法，分 31/5/3 三批、共39次方法执行；另有 init 创建／重复拒绝2项短检查。均不增加 WARP 观察。没有完整 CTest、长压力、参数矩阵、大文件／多 Segment、新录屏、旧录像整段重放、硬件 GPU、实屏或远程测试。

## 证据及运行身份

证据根：`<repo>\artifacts\remote-step3b-20260908-run01`。

- 最新只读重分析：`ANALYSIS-03.json`；原 `ANALYSIS-01/02.json` 保留。
- 实际93观察运行文件：`runtime-verified-run\PBRemoteThroughputStep3B.exe`，SHA-256 `2062ed2b12cecba2f363e8989d9261a51fd50da23e18dc984dda856d5c5fb324`。
- 最终源代码构建：`runtime-final\PBRemoteThroughputStep3B.exe`，SHA-256 `b8aa6bc4aed07def5328f80c0da9cb194d593574fc78a7ebb03b6e412b515dcb`。
- 运行后仅把 C++ 大括号格式改为 Allman，词法 tokens 相同；新旧 PE `.text` 及其他非 `.rdata` section 相同，`.rdata` 排除 debug metadata 后相同，见 `context/format-build-equivalence.json`。
- 最终 EXE 另做30帧 media-only 校验，像素/PTS 与原 codec 回放一致；不是再次 WARP／整文件恢复。因此完整恢复证据仍明确归于旧运行 EXE，不能混称最终 EXE 已重跑正常93观察。
- `FINAL_STATUS.json`、`SOURCE_IDENTITY_FINAL.json`、`RUNTIME_IDENTITY_FINAL.json`、`FINAL_MANIFEST.json`、`POST_SEAL_VERIFY.json` 绑定交付；包为 `STEP3B_REPLAY_EVIDENCE.zip`，不是产品发布包。

### 只读复核现存证据：不启动 Encoder/Decoder

输出必须尚不存在；若存在则选择另一个明确的新文件，不能删除旧结果。

```powershell
& '<python>' -B '<repo>\tools\PBRemoteThroughputStep3B\analyze.py' `
  --root '<repo>\artifacts\remote-step3b-20260908-run01' `
  --output '<repo>\artifacts\remote-step3b-20260908-run01\ANALYSIS-VERIFY-NEW01.json'
if ($LASTEXITCODE -ne 0) { throw 'Evidence verification failed' }
```

预期输出 `NOT_RECOVERED`，JSON 同时给出 raw PASS、codec NOT_RECOVERED、重复一致性。命令会重新读像素、码流与文件并检查已有报告，不重新运行媒体／解调。

历史证据保留原绝对路径；不要为迁移改写历史报告。迁移包可以独立核验成员哈希；原分析器在原路径上检查历史报告，迁移后需要新目录、新报告的复验。Python 使用现有 `<python>`，需要 `blake3`；冻结依赖还需要 `pefile`，本轮不安装依赖。

### 新生成一组对照（需另确认新的93观察预算；本轮未重跑）

以下调用全部使用有界无窗口进程。选择未存在的新根后依次执行；任一步失败停止，不复用半完成根、不自动重试。

```powershell
$python = '<python>'
$script = '<repo>\tools\PBRemoteThroughputStep3B\run.py'
$root = '<repo>\artifacts\remote-step3b-replay-new01'
$sealed = '<repo>\artifacts\remote-step3b-20260908-run01'
$exe = "$sealed\runtime-verified-run\PBRemoteThroughputStep3B.exe"
if (Test-Path -LiteralPath $root) { throw 'Choose a new evidence root' }
& $python -B $script init --root $root
if ($LASTEXITCODE -ne 0) { throw 'Init failed' }
& $python -B $script prepare --root $root --exe $exe
if ($LASTEXITCODE -ne 0) { throw 'Raw proof failed; stop before codec' }
& $python -B $script freeze-codec --root $root --ffmpeg "$sealed\codec-runtime\ffmpeg.exe" --ffprobe "$sealed\codec-runtime\ffprobe.exe" --dll-root "$sealed\codec-runtime"
if ($LASTEXITCODE -ne 0) { throw 'Codec runtime freeze failed' }
& $python -B $script codec --root $root --exe $exe
if ($LASTEXITCODE -ne 0) { throw 'Codec processing failed; preserve evidence' }
& $python -B '<repo>\tools\PBRemoteThroughputStep3B\analyze.py' --root $root --output "$root\ANALYSIS-01.json"
if ($LASTEXITCODE -ne 0) { throw 'Analysis failed' }
```

新 fixture 会产生新 CSPRNG Session，不能要求新码流与本次 SHA-256 相同；同一新冻结输入的两轮必须一致。不自动重新触发已修正的颜色失败。若另外运行 guard，真实 unspecified-color 负例用 `test_guards.py --bad-color-media` 显式指向原 `codec-01\channel.mkv`；不要伪造或修改旧失败。

低层 `--raw/--raw-prefix/--codec/--media-check` 只负责像素媒体和运行边界，不能代替编排的 provenance／发布验证；`--make-fixture` 只给 Encoder 源文件，`--audit` 只执行原源文件审计。两者都不把该文件传入 Decoder。

### 新构建（不覆盖已验证 build）

```powershell
$build = '<repo>\build-remote-step3b-rebuild-new01'
if (Test-Path -LiteralPath $build) { throw 'Choose a new build root' }
& 'C:\Program Files\CMake\bin\cmake.exe' -S '<repo>\tools\PBRemoteThroughputStep3B' -B $build `
  -G 'Visual Studio 17 2022' -A x64 `
  -DPB_STEP3B_REPO=<repo> `
  -DPB_STEP3B_BASE_BUILD=<repo>/build-remote-step2-20260908-run01 `
  -DPB_STEP3B_MEDIA_ROOT=<repo>/artifacts/remote-step2-20260908-run01/deps/installed/x64-windows
if ($LASTEXITCODE -ne 0) { throw 'Configure failed' }
& 'C:\Program Files\CMake\bin\cmake.exe' --build $build --config Release --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
```

MSVC C++20 `/W4 /WX`，仅新工具，Qt-free，不下载／更新依赖。源码快照不是 Step2 静态库的可移植完整重建包；重建需要清单所列原封存库，像素复验可用包内已编译 runtime。
