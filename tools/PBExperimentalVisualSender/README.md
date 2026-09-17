# 工具专用实验发送入口：色度中和与有界保持

> **维护范围 / Scope (2026-09-17):** 本文保留该模块的协议/工具/测试参考，不再作为项目当前路线或发布状态入口。当前模式、单屏接收、预算、日志与完整文件证据见 [中文文档](../../docs/README.md) / [English documentation](../../docs/README.en.md)。历史日期、Gate、现场坐标与阶段参数仅适用于当时记录；不应直接复制到新环境。
> This is a module/tool/test reference, not the current release roadmap. Use the bilingual index for current behavior and validation boundaries; historical gates/settings are not universal defaults.

## 2026-09-09 独立有界对照增量

新增 `--comparison-run original|neutral SOURCE NEW_SHORT_ROOT DEVICE SECONDS`，显式允许5..900秒；旧 `--run color|neutral` 仍仅5..60秒。时长只接受无前导零的ASCII十进制，不自动延长。`original` 使用空 `EncoderPresentationFactory`，直接进入当前冻结原runtime的native呈现，不加变换或保持；**不是历史现场A版**。`neutral`仍是既有中和加保持，两项一起作为冻结候选，不冒充单变量色度对照。当前已冻结runtime保留其原有控制相位实现，工具没有改调度。

新共享 `run_contract.h`、`comparison_process_runner.py` 只属于工具。后者是旧Step3B Job runner的独立派生，内存2GiB/process+Job、先入Job再resume、kill-on-close、日志create-only/限额和进程句柄归属不变。公开 `invoke_comparison(argv, log_path)` 只接受这两个工具的显式comparison入口，固定运行预算加30秒cleanup，最大930秒；不读取Receiver报告，也不从接收完成反向控制Sender。需要可用的Windows Python，不是免依赖的远端操作包。

每行原submitted serializer以全部uint64最大值测试，含换行268B；新增硬限14000行，合计3,752,000B，仍在4MiB内。held8192条最大宽度记录3,088,998B，另留三份固定native快照196,608B保守预算，也不扩4MiB。虚拟时钟6750帧检查不等于900秒实屏认证；原8192记录、两次drained统计epoch重建、16MiB帧、64MiB源与全部原接收安全门不变。

新增构建：`<repo>\build-remote-comparison-sender-20260909-run01`。6组无屏幕检查通过；本轮实屏与容量筛选证据在 `<repo>\artifacts\remote-bounded-comparison-20260909-run01`，以最终 `AUDIT.json` 为准。

**候选选择边界：** 已封存的正常B2现场编码吞吐190,186B/s，高于当前held-neutral的理想稳态容量98,550B/s；B2进入FEC的Chroma槽11949/11955通过。因而不把本候选打包为当前现场提速方案，保留作弱信道恢复参考。理论容量只用于排除缺少容量余量的候选，不作实际goodput或通用Receiver时长下界；原远控codec/带宽仍未知，固定8M模型不代表该现场。下一项速度候选应保留有效Chroma与足够cadence容量，不能继续围绕低容量模型成功盲扩样本。

以下为前一里程碑的历史实现/验证记录；已验证范围与当前结论以上述新根及夜间记录为准。

目标是筛选有利于**非本机整文件吞吐**的发送画面，而不是提高 Present 计数。当前状态为 `EXPERIMENTAL_SENDER_NATIVE_ENTRY_VERIFIED_NOT_REMOTE_GAIN`；不注册产品默认构建，不新增认证 Profile，不改变原 Decoder/Receiver。

## 当前实现

原 `EncoderRuntime` 继续负责源不可变检查、RAW/zstd、Segment、FEC、规范 V3 raster、durable lease、Carousel、15 Hz 逻辑时钟和原测量。工具只通过已有 `EncoderPresentationFactory` 插入后置画面处理，最终仍由原硬件 `DataWindow` 展示像素。

- `color`：完整彩色对照。
- `neutral`：逐像素 `floor((722*B+7152*G+2126*R+5000)/10000)`，三个颜色通道取同值，alpha 不变。不是重新解释 Chroma bits，也不是宣称灰度 raster 符合原认证 Profile。
- 两者相同的保持策略：观察到本地对应成功 Present 返回后，至少等待133,333,334ns才转交下一帧。该时序**不等于**离线15fps/H2，也不是屏幕扫描时间、Receiver捕获或ACK。
- 一张真实排队图像，最多16MiB；固定8192条身份/时间记录。规范runtime与native各自原有有界缓冲不改变。快照的`pendingFrame`包含这张真实队列，原生Present/unique/counter不编造。
- Native `SubmitFrame`已有独立`pendingPixels`并同步复制，允许上一张active图仍在GPU处理中；工具与原runtime一样只检查pending，不等待GPU完全空闲。epoch排空仍要求无pending/inFlight/active。
- 原native `statistics-disjoint/recovered`可能切epoch并清空活动源。仅当物理环境、viewport、swapchain/buffer代次不变，native仍有效且已经完全排空时，工具最多接纳两次同类时序重建，丢弃自己的旧epoch队列，下一张由原runtime重新提供。**不重标或重放旧像素，不将未观察到的Present补成成功。** 模式/显示器/设备变化、未排空、未知原因或超出次数仍失败。
- 停止时记录被丢弃的尾队列；源前后ledger必须相同。没有Receiver成功推断。

## 已完成的窄验证

最终构建：`<repo>\build-remote-experimental-held-sender-20260909-run08`。

- MSVC `/W4 /WX`构建两个目标，4组确定性检查通过：精确颜色/row pitch/alias/长度/未知模式、真实单队列/保持/重复Present/停机、epoch/时钟/原生错误/资源上限、从现场快照复现的有界统计epoch排空。
- C++转换与独立源冻结的30张完整灰度画面逐字节一致，共62,208,000像素；没有新增codec或Receiver观察。
- 右屏`DISPLAY2`物理`[2560,0,5120,1440]`，两组各5秒实际硬件发送后正常退出。每组提交31帧、native转交30帧、观察到对应成功Present29帧，尾队列丢弃1帧，首张因统计epoch重建保留为未观察。实际提交约6.39/6.38Hz，不能冒充7.5Hz或远控unique FPS。
- 最小同epoch保持分别133,460,000/133,455,500ns；两组源前后ledger一致、未发生捕获或Decoder运行。native进程峰值约184MB，2GiB Job/进程和30秒外层硬上限；进程实际各5.125秒。

历史失败全部保留：初次CMake字符串生成错误、ASCII显式窄化编译修正、缺失monitor metadata被原校验拒绝、两次严格epoch启动失败，以及约4Hz的过度等待inFlight初版。不能只展示最终通过记录。

## 入口和资源

无参数、非法模式都拒绝，不默认启动窗口。只读拓扑与无屏幕检查：

```powershell
& '<repo>\build-remote-experimental-held-sender-20260909-run08\Release\PBExperimentalVisualSender.exe' --list-monitors
& '<repo>\build-remote-experimental-held-sender-20260909-run08\Release\PBExperimentalPresentationTests.exe' --self-test
```

真实发送语法为`--run color|neutral SOURCE NEW_SHORT_ROOT DEVICE SECONDS`，必须通过现有`<repo>\tools\PBRemoteThroughputStep3B\process_runner.py`的有界Job runner启动；程序检查2GiB以内内存限额和kill-on-close。初步入口仅允许5..60秒、64MiB以内源、固定磁盘新目录且根路径不超过96字符、明确的1920×1080..2560×1440未旋转显示器。不得直接拿这个60秒入口给40MB远控样本做完整吞吐结论。

证据根：`<repo>\artifacts\remote-experimental-held-sender-20260909-run01`。最终运行是`<repo>\artifacts\hl0909-color05`和`<repo>\artifacts\hl0909-neutral05`。`AUDIT.json`核对源码/CL输入/实际MAP函数归属/源ledger/全部时序记录；原`measurement.build`指纹只标识复用的应用库，不覆盖新工具，必须结合新工具manifest。

下一步：先将此新发送入口的实际屏幕像素交给原Decoder，验证整文件摘要/发布/reopen；再才考虑非本机对照。当前不是认证交付包，也没有远控收益结论。
