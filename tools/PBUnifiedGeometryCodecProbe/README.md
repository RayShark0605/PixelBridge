# Step3-B codec 几何归因探针

> **维护范围 / Scope (2026-09-17):** 本文保留该模块的协议/工具/测试参考，不再作为项目当前路线或发布状态入口。当前模式、单屏接收、预算、日志与完整文件证据见 [中文文档](../../docs/README.md) / [English documentation](../../docs/README.en.md)。历史日期、Gate、现场坐标与阶段参数仅适用于当时记录；不应直接复制到新环境。
> This is a module/tool/test reference, not the current release roadmap. Use the bilingual index for current behavior and validation boundaries; historical gates/settings are not universal defaults.

## 用途与状态

这是后续单独获准的 **只读几何归因工具**，不是几何修复或替代 Decoder，不注册产品 CMake。计划与结论见 `<repo>\docs\REMOTE_GEOMETRY_CODEC_ATTRIBUTION_2026-09-08.md`。

结论：`ATTRIBUTION_COMPLETE_CRITERION_NOT_ESTABLISHED`。确认了原几何拒绝分支及所选帧的边缘→拟合链条，但没有建立安全的有损边缘误差界，因此 **准入候选0，生产修复0**，整个Step3仍PARTIAL、现场NOT_RUN。

## 原实现与隔离

- `prepare_reference.py` 对封存 `local_desktop_decode.cpp` 核验SHA-256，在新build生成完整不变前缀，仅末尾追加 `probe_impl.inc`。原 LocateMarkers、FindEdge、FitAxis、RefineGeometry、RefinePointCanvasCoverage、SameGeometry、EvaluateGeometry 均不改。
- 初轮只运行 marker/geometry 观测，不调用 Bootstrap 或数据FEC。追加第15帧经过单独确认，才允许原 Bootstrap RS/CRC，始终不调用数据FEC、Receiver或WARP。
- 原 `ResolveUnifiedVisualSamplingGeometry` 链接已封存的Step2库；不是Python模型替代生产判定。分析器再重算分支和OLS作独立核对。
- 观测使用分离的有界LumaReader，不修改原reader状态、拟合、候选排序或生产返回值。原定位器工作量与额外只读测量分别记录。
- 媒体读取直接复用原 `<repo>\tools\PBUnifiedRecordingReplay\recording_media.cpp` 及原Step2 DLL。不重新编码，不重新采屏。
- 工具仅输入旧像素序列／codec，不输入原64KiB payload、ledger或expected payload。真值只由事后分析使用，绝不回馈候选判定。

## 实际执行与两次保留的失败

证据根：`<repo>\artifacts\geometry-codec-attribution-20260908-run01`。

1. `run-01`：原始ordinal0/1/15、codec ordinal0/1完成；codec15出现roleCounts=[1,1,1,2]，按禁止自行选候选的guard停止，exit1。共尝试6个像素样本，媒体顺序读16帧。
2. 用户确认后，仅 `ambiguity-01` 追加codec15一个CPU像素样本，顺序媒体再读16帧，枚举两个marker组合并调用原单候选Bootstrap校验。
3. 两个组合独立校验均通过且Bootstrap相同，但最终拟合不同。工具最初错误地把“独立通过的组合数”当作生产候选数，再次exit1。实际上原代码**先对seed执行SameGeometry去重，再EvaluateGeometry**，第二个组合根本不会进入生产评估。
4. 保留两轮exit1和当时源码／build，仅修正工具验证逻辑，按原去重顺序核对；在第三个新build执行4个纯数值SameGeometry guard。不再追加像素观测。

**不能把上述两轮native运行标成完整PASS，也不能把最终EXE称为已从头重跑成功。**最终结论来自保留的有效观测、原始WARP trace、源码分支、原SameGeometry数值验证及只读分析。被去重组合的独立EvaluateGeometry结果只是诊断性反事实，不是生产多候选接纳。

实际预算：6个不同像素样本，7次样本尝试（其中一次因额外marker提前停止），总计32帧软件媒体解码；新增WARP/数据FEC/Receiver调用0。补充样本最多4次Bootstrap候选评估、最多8个RS copy解码的上界，不用上界冒充实测精确调用数。

## 固定边缘记录与验证

每轮FitAxis记录两轴×4角色×6边缘，顺序为horizontal/vertical、TL/TR/BL/BR、offset -28/-20/-12/12/20/28。最多4轮。每条数组字段为：

```text
[logical, predicted, perpendicular, black, white, crossing,
 leftPosition, rightPosition, leftValue, rightValue,
 adjacentCenterBefore, adjacentCenterAfter]
```

`FindEdge` 的原插值结果与单独观测的原始采样括区精确相等；Python按C++逐项累加顺序重算OLS、残差、movement及0.005收敛分支，不加比较epsilon。

停止记录中没有完成的seed/fit字段不可作实测值使用，分析器依据reason/iterationCount明确排除；不拿默认零值推导几何。旧Step3-B trace缺少marker residual，新3个选定codec记录补齐该字段；对全部60个旧codec观察的分支重建仅用“原阶段已成功，因此残差有限且非负”的条件，不伪造60个实测残差。

## 运行、源码和构建身份

| 阶段 | EXE SHA-256 | 权威边界 |
| --- | --- | --- |
| 初轮build-run01 | `c2f436513191646e01657666e9adb867b64c51b07d4a56fdd122bad1db22b82c` | 5完整＋1提前停止样本 |
| 补充build-run02 | `23b043e4e6c7ca4b91eafb3516cff8571af9ddb97bbfbc438fc3ff623df3af8e` | codec15两组独立诊断；最终工具断言失败 |
| 最终build-final01 | `37337a6054aa273e8223d139e372362461ba5476ab73fb9627a9dc51067e955b` | 修正工具去重断言；仅4项数值guard，无像素重跑 |

三个build分别为：

- `<repo>\build-geometry-codec-attribution-20260908-run01`
- `<repo>\build-geometry-codec-attribution-20260908-run02`
- `<repo>\build-geometry-codec-attribution-20260908-final01`

原输入位于 `<repo>\artifacts\remote-step3b-20260908-run01`，不复制237.30MiB raw进入本轮128MiB证据预算，也不重新生成Session。最终包记录外部封存输入的绝对路径和hash，需要原Step3-B包才能重播像素。

本轮证据包按内容去重：44个相同的app-local DLL只保存于证据根的`runtime/common`，三个EXE分别位于`runtime/initial`、`runtime/supplement`、`runtime/final`。`RUNTIME_IDENTITY_FINAL.json`逐项绑定原路径和包内副本。DLL列表不是已加载模块列表。将来获准重播时，应在全新目录组合一份阶段EXE和公共DLL；本轮不做组合或启动，不改系统PATH。旧Step3-B输入、Step2静态库、系统DLL与编译工具链是明确的外部依赖，不声称本包可以脱离原环境直接从头重建和恢复文件。

MSBuild的全局vcpkg集成曾优先提供38个FFmpeg头文件，三次实际`CL.read`记录均如此。最终逐文件核验它们与封存Step2对应文件字节相同。封存包含该环境影响与身份，不把CMake显式路径当作实际头文件来源，也不把这次构建称作完全隔离的干净环境构建。

## 最小复验：只读取已有证据

```powershell
& '<python>' -B '<repo>\tools\PBUnifiedGeometryCodecProbe\analyze.py' `
  --root '<repo>\artifacts\geometry-codec-attribution-20260908-run01' `
  --output '<repo>\artifacts\geometry-codec-attribution-verify-new01.json'
if ($LASTEXITCODE -ne 0) { throw 'Attribution evidence invalid' }
```

新输出必须不存在并放在封存根之外；不改原报告，不启动媒体/Bootstrap/Decoder。预期结论是归因完成、判据未建立，而不是codec恢复成功。

封存后还可执行仅核对现有清单、包成员、源码和外部输入身份的检查（无输出文件写入、无媒体解码）：

```powershell
& '<python>' -B '<repo>\artifacts\geometry-codec-attribution-20260908-run01\seal_final.py' --verify
if ($LASTEXITCODE -ne 0) { throw 'Sealed attribution package verification failed' }
```

该命令验证当前封存工作区，不自动安装依赖、不修复差异。未来源码或外部输入改变应报告身份不符，不能为使检查通过改写此清单。

`run_probe.py` 是受预算约束的历史运行编排。`--ambiguity` 只执行单独确认的补充；需要独立新root、`context/start.json`和`context/ambiguity-decision.json`，不能复用此封存root。脚本最后的成功校验未因本次native exit1而执行，不冒充已有 `AMBIGUITY_RUN_CHECK.json`。未来再次重播必须另外确认新的样本预算；本轮不自动重试。

### 只重建工具

```powershell
$build = '<repo>\build-geometry-codec-probe-rebuild-new01'
if (Test-Path -LiteralPath $build) { throw 'Choose a new build directory' }
# CMake 通过环境变量 PB_PYTHON_DIR 定位 Python（本工具不内置任何机器相关路径）
$env:PB_PYTHON_DIR = '<python 所在目录>'
& 'C:\Program Files\CMake\bin\cmake.exe' -S '<repo>\tools\PBUnifiedGeometryCodecProbe' -B $build `
  -G 'Visual Studio 17 2022' -A x64 `
  -DPB_GEOM_REPO=<repo> `
  -DPB_GEOM_BASE_BUILD=<repo>/build-remote-step2-20260908-run01 `
  -DPB_GEOM_MEDIA=<repo>/artifacts/remote-step2-20260908-run01/deps/installed/x64-windows
if ($LASTEXITCODE -ne 0) { throw 'Configure failed' }
& 'C:\Program Files\CMake\bin\cmake.exe' --build $build --config Release --target PBUnifiedGeometryCodecProbe --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
```

不注册产品目标，不安装依赖。MSVC C++20 `/W4 /WX`，没有Qt、WARP或图形窗口入口。原库仍含生产实现，不代表工具调用了其数据FEC或Receiver；调用边界以本目录源码与封存命令为准。

## 资源与测试范围

- native自身强制512MiB process commit；FFmpeg单allocation32MiB、原软件decoder单线程。
- 外层复用Step3-B无窗口Job监管器（2GiB process/job），native启动后再收紧自身为512MiB。编译器使用外层2GiB，不把编译峰值冒称为512MiB内。
- 配置、三次构建、两次像素观测和数值guard累计37.938秒，计入同一120秒预算；native观测峰值40,218,624 B。静态分析/编辑/哈希非信道时间。
- 合法输入raw长度精确248,832,000 B，codec≤16MiB；每次最多16媒体帧、固定样本ordinal；每reader24,000,000 work units，固定4轮/48槽。
- 每记录≤64KiB，每trace≤1MiB；本轮证据连同最终ZIP总计≤128MiB。stdout仅诊断，不作为payload传输。
- 4项原SameGeometry纯数值guard＋5项分析器篡改拒绝检查通过。没有完整CTest、参数搜索、压力、额外payload恢复、实屏或远程测试。
