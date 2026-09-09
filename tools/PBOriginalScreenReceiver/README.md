# PBOriginalScreenReceiver

## 2026-09-09 有界对照增量

新增显式 `--comparison-run NEW_SHORT_ROOT DEVICE SECONDS`（5..900秒），与Sender共用 `<repo>\tools\PBExperimentalVisualSender\run_contract.h` 的严格时长解析；旧 `--run` 仍5..60秒。只改变工具宿主的运行窗口，不改原DecoderRuntime/Receiver/GPU/捕获算法、资源策略、摘要、发布、reopen或冲突拒绝。外层使用同目录的独立 `comparison_process_runner.py::invoke_comparison`，固定预算加30秒cleanup（最大930秒）；到达窗口未恢复就记录失败，不延长或自动重启，不反向停止Sender。

新构建：`<repo>\build-remote-comparison-receiver-20260909-run01`。时长/已有根负例和只读PMv2/ROI检查通过。900秒只通过入口与容量检查，未跑900秒实屏或40MB/远控验证；本轮两Segment实屏结果以 `<repo>\artifacts\remote-bounded-comparison-20260909-run01\AUDIT.json` 为准。工具仍不接收源文件、摘要、Session或预生成payload参数，不是现场认证包。

以下保留原短运行入口的历史合同。

仅供有界、本机实际像素接入验证的无 GUI launcher，不是新的 Decoder 算法或认证 Profile。

- 直接链接显式原构建的 `PBApplication`、原 Receiver、WGC/DXGI、D3D demod 库。
- 默认构造 `DecoderRuntime`，调用 `MakeUnifiedDecoderConfig`，固定产品 Auto capture。
- 不注入服务、Replay、source、摘要、文件名、Session 或预生成帧；Decoder 只从显式设备对应的整屏 ROI 捕获 payload。
- `--preflight DEVICE` 只读拓扑，不创建接收器、窗口或捕获器。
- `--run NEW_SHORT_ROOT DEVICE SECONDS` 要求 create-only 本地短路径、5..60 秒、2 GiB 或更小的 process/Job memory 和 kill-on-close。外层 runner 必须设置独立 cleanup timeout。
- 不接受大的输出确认；保留原 Receiver 的策略、摘要、安全发布、reopen、冲突拒绝和资源限制，不调整阈值。
- 没有键鼠/焦点操作，不给 Sender 发送结束反馈。Sender 必须自行按预定时长结束。
- 成功必须是原 runtime 的整文件摘要、发布和 reopen；外部源/输出双摘要只能在进程结束后另做审计。
- 本机接入成功不是非本机速度认证。原静态库 build identity 也不是本工具源码身份，需额外封存 CL 输入、MAP、库和二进制摘要。

构建只使用显式原库，禁止隐式全局 vcpkg 头文件：

```powershell
cmake -S <repo>\tools\PBOriginalScreenReceiver -B <NEW_BUILD> -G "Visual Studio 17 2022" -A x64 -DPB_STEP3B_REPO=<repo> -DPB_STEP3B_BASE_BUILD=<repo>\build-remote-throughput-observation-20260909-run01 '-DCMAKE_VS_GLOBALS=VcpkgEnabled=false;VcpkgApplocalDeps=false'
cmake --build <NEW_BUILD> --config Release --parallel 1
```
