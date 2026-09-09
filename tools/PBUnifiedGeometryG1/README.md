# PBUnifiedGeometryG1

Standalone, CPU-only geometry regression fixtures. This directory is deliberately **not** registered in the production CMake graph. It links the explicit sealed Step2 Release libraries and does not modify product admission, wire, quality policy or Receiver behavior.

## 最终状态：G1 完成，候选不晋级

本工具只完成有界离线 G1，不是生产修复。最终结果为 `NOT_PROMOTED_NO_NEW_ADMISSION`；G2、Step3 和现场验证均未启动。

- 8 个既有数值边界回归；25 个生产 raster 像素夹具；11 项候选合同检查。
- 一种工具专用候选：仅在 1920×1080 上验证四个 marker 与全部 48 条 sharp adjacent-center 边缘，然后尝试精确 `(0,0,1,1)`。沿用原 `1e-9` roundoff，不引入新的噪声或收敛容差。
- 后续校准、局部 freshness、FEC、CRC、身份检查复用当前实现，期望 payload 只在解码后由夹具检查器比对。
- `prepare_reference.py` 在新构建目录生成带 diff/哈希的工具派生源，保留全部原函数，并新增实验入口；不改任何生产源或产品公共头。
- 录像仅取前 36 个观察。原路径仍为 16 个观察、240 blocks；候选 0 个观察、0 blocks，全部止于 `NonIntegerSharpEdge`。不把它替换为生产路径。
- 候选对精确像素和等价回填通过，对本次 must-reject 负例保持拒绝；局部 freshness 撕裂保留相同的 13 blocks；无新增接受。
- 原 `PBLocalDesktopBootstrapTests [fixed-canvas]` 为 1 case / 132 assertions；三个既有几何 tag 为 3 cases / 332 assertions。
- 21 项证据检查、5 项工具负例检查通过。未执行到的候选测量为 `null` 并带测量次数，不用初始零值冒充实测。

详细结果：`<repo>\docs\REMOTE_GEOMETRY_G1_EXECUTION_2026-09-08.md`。

用户已确认：裁切后同位置回填、最终整幅 BGRA 逐字节相同的四个样例归为像素等价例；不能根据不可观察的处理历史要求其拒绝。可观察的裁切、位移、非法尺度及冲突仍遵守既有规则。

规范 1×上的对称模糊与 1.125× point 后出现半像素边界的模糊分开验证：前者原路径可恢复，后者原路径仍拒绝。候选不能取代一般缩放/有损恢复路径。

## Build in an explicitly new directory

```powershell
& 'C:\Program Files\CMake\bin\cmake.exe' -S '<repo>\tools\PBUnifiedGeometryG1' -B '<new absolute build directory>' -G 'Visual Studio 17 2022' -A x64 -DPB_G1_REPO=<repo> -DPB_G1_BASE_BUILD=<repo>/build-remote-step2-20260908-run01 -DPB_G1_FFMPEG=<repo>/artifacts/remote-step2-20260908-run01/deps/installed/x64-windows
& 'C:\Program Files\CMake\bin\cmake.exe' --build '<new absolute build directory>' --config Release --target PBUnifiedGeometryG1 PBLocalDesktopBootstrapTests --parallel 2
```

最终构建：`<repo>\build-geometry-g1-20260908-run03`。`run01` 基线、`run02` 初版候选、原来的 Step2 构建均须保留。没有安装或下载新依赖。生成器遇到源身份变化或不同内容的已有生成文件会拒绝，不能覆盖重用。

`PBUnifiedGeometryG1.exe` 无参数运行夹具，`--recording <absolute local path>` 只读前 36 个观察。推荐使用 `run_checks.py --repo <repo> --build <build> --output-new <new artifact directory>`，录像额外传 `--recording <path>`。它以 `CREATE_NO_WINDOW` 启动、120 秒外层超时、create-only 输出；固定条数的 trace 超过 1 MiB 或 stderr 超过 64 KiB 时标为失败。工具内有 512 MiB Job、32 MiB FFmpeg 单分配上限、单解码线程，无窗口/捕获/GPU/Receiver 发布。

Exit 0 表示本次执行的断言成功，不表示候选有收益或现场通过。`analyze_results.py` 比较已封存的原 17 个夹具与 36 观察、逐块摘要，并将无效证据标为 `EVIDENCE_INVALID`。详见报告中的复现命令；所有结果输出必须是新路径。

The two narrow existing test commands are:

```text
PBLocalDesktopBootstrapTests.exe [fixed-canvas] --reporter console --rng-seed 2092026
PBUnifiedVisualCpuTests.exe [point-downscale],[point-coverage],[remote-coverage] --reporter console --rng-seed 2092026
```

最终证据：`<repo>\artifacts\geometry-g1-20260908-run02`，其中 `runs\fixtures-final-02`、`runs\recording-final-02` 是最终二进制运行；`RESULT_FINAL_CHECKED.json` 为最终只读分析。`runs\guards-final-01\altered-trace` 是人为篡改的负例，不能混入正常语料统计。
