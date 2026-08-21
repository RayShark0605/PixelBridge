# PixelBridge

Windows x64 / C++20：通过可见桌面/视频像素进行的高性能单向文件传输。
规范设计文档：`docs/PixelBridge_最终技术路线与总体设计.md`；工程规则见 `AGENTS.md`。

## 目录

| 路径 | 用途 |
| --- | --- |
| `apps/PixelBridgeEncoder`、`apps/PixelBridgeDecoder` | 应用（当前为控制台空壳，Qt 6 UI 在后续里程碑接入） |
| `libs/PBCore`、`libs/PBProtocol` | 核心库（禁止依赖 Qt） |
| `tools`、`fuzz`、`benchmarks` | 占位目录（骨架阶段无代码） |
| `tests` | Catch2 v3 单元测试（CTest） |
| `docs` | 设计文档 |

## Qt 边界

Qt 只允许用于应用 UI 层（界面、配置、生命周期、状态展示）；`libs/` 下的核心库一律禁止依赖 Qt。

## 构建（MSVC x64）

依赖：Visual Studio 2022（C++ 工作负载）、CMake >= 3.24、vcpkg（本机 `D:\vcpkg`）。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release --parallel
ctest --test-dir build --build-config Release --output-on-failure
```

输出 target 依赖图：

```powershell
cmake -S . -B build --graphviz=target-dependency-graph.dot
Move-Item target-dependency-graph.dot build\ -Force
```

Ninja 替代（单配置）：

```powershell
cmake -S . -B build-ninja -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake
```

## 告警策略

所有目标统一使用 `/utf-8 /W4 /permissive- /EHsc /Zc:__cplusplus /Zc:preprocessor`，默认附加 `/WX`（可用 `-DPB_TREAT_WARNINGS_AS_ERRORS=OFF` 关闭）。
自有头文件使用引号包含，第三方头文件（如 Catch2）使用尖括号包含，并通过 `/external:anglebrackets /external:W0` 豁免第三方头文件的告警。

## 第三方依赖

依赖由 vcpkg manifest `vcpkg.json` 管理（当前仅测试用 Catch2：版本下限 3.15.0，端口注册表基线由 manifest 的 `builtin-baseline` pin 定），安装产物位于 `build/vcpkg_installed/`，不入库。