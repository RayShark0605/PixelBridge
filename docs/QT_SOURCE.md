# Qt 对应源码 / Corresponding Qt source

## 中文

PixelBridge v1.0 的 GUI 动态链接 Qt 6.10.1 Core、Gui、Widgets，使用 Qt Base 的 `qwindows`、`qoffscreen`、`qmodernwindowsstyle` 插件。它们不是 PixelBridge 的 MIT 代码，按 LGPL v3 及所含第三方条款提供。许可原文在产品包 `licenses/qt/LICENSES/`，其中包含 `LGPL-3.0-only.txt` 和 `GPL-3.0-only.txt`。组件归属见 `licenses/qt/qtbase-6.10.1.spdx.json` 与完整源码。

每个产品包都附有 `sources/qtbase-6.10.1-source.zip`。该归档来自与本次二进制 SDK 配套安装的 Qt Online Installer 6.10.1 `Src/qtbase`，保留完整模块（包括第三方源码、许可证、头文件、配置和构建脚本）；没有修改 Qt 源码。`sources/qt-source-manifest.json` 记录逐文件 SHA-256、数量与归档哈希。归档还包含 SDK 的 `config_qtbase.opt` 和 `config_qtbase.summary`。Qt 官方二进制由其自身工具链构建，本项目未把“对应源码”声称为所有环境下可逐字节复现官方 DLL。

无需解压这些源码即可运行 PixelBridge。重新分发时请同时保留对应源码、许可证、声明与本文件，不能只取本项目的 MIT 文字覆盖第三方权利。

### 自行构建与替换 Qt

1. 将源码 ZIP 解压到开发目录；准备 Windows x64、Visual Studio 2022 C++ 开发环境、Windows SDK、CMake 和 Ninja。
2. 在 x64 Native Tools 命令环境中建立独立 build/install 目录；例如：

```powershell
cmake -S <qt-source>/qtbase -B <qt-build> -G Ninja `
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON `
  -DQT_BUILD_TESTS=OFF -DQT_BUILD_EXAMPLES=OFF `
  -DCMAKE_INSTALL_PREFIX=<qt-install>
cmake --build <qt-build> --parallel 4
cmake --install <qt-build>
```

3. 以上是适用于修改/重建的基本示例，不是官方二进制逐字节复现命令。原始 `build-info/config_qtbase.opt`/summary 记录更多特性；其中 Qt 构建农场路径不应照抄，按本机依赖调整。完整重建指导见 [Qt for Windows — building from source](https://doc.qt.io/qt-6.10/windows-building.html)。
4. 在一个备份副本中，用 ABI/接口兼容、相同架构的修改版 `Qt6Core.dll`、`Qt6Gui.dll`、`Qt6Widgets.dll` 和配套平台/样式插件替换，不混用不同配置的库。若修改打破 ABI，应同时重新构建 MIT 许可的 PixelBridge。
5. 程序没有强制签名检查或在线激活来限制替换；允许为调试这种库修改而进行必要的逆向工程。包校验器会因字节改变报告原始清单不匹配，但它不是启动门，不阻止使用修改版。

`qt.conf` 把正常插件搜索固定到包内相对目录，不嵌入开发机安装位置；用户可随兼容替换调整它。系统 ICU、D3DCompiler_47 和 Windows API 不由本包重分发；未使用的 Qt PDF、SVG、网络等插件及 DXC 工具链没有装入正式包。

## English

PixelBridge v1.0 dynamically links Qt 6.10.1 Core, Gui and Widgets with Qt Base's `qwindows`, `qoffscreen` and `qmodernwindowsstyle` plugins. These are not PixelBridge's MIT-licensed code. LGPL v3 and bundled third-party terms apply. Original license texts, including LGPL v3 and GPL v3, are in `licenses/qt/LICENSES/`; attributions are in the official `licenses/qt/qtbase-6.10.1.spdx.json` and source files.

Each product archive contains `sources/qtbase-6.10.1-source.zip`, taken from the matching Qt Online Installer 6.10.1 `Src/qtbase` component. It preserves the complete unmodified module, including third-party sources, licenses, headers and build scripts. The manifest in `sources/qt-source-manifest.json` identifies every file and the archive by SHA-256. The ZIP also includes the SDK's original configure options/summary in `build-info/`. Corresponding source is not a claim of byte-identical reproduction of Qt's official binaries under arbitrary toolchains.

Source extraction is unnecessary to run PixelBridge. Preserve the source, original licenses, notices and this document on redistribution. The MIT notice does not replace third-party rights.

To rebuild, extract the archive, use an x64 Visual Studio 2022 developer environment with Windows SDK, CMake and Ninja, and adapt the standalone Qt Base commands above. They are a modification/rebuild starting point, not an exact reproduction recipe. Refer to the recorded configuration for features but replace build-farm-specific paths with local dependencies. The linked official Qt build guide explains the full procedure.

Replace Qt DLLs and their matching plugins in a backup of the application directory with interface/ABI-compatible x64 shared libraries. If changes break ABI, rebuild the MIT-licensed PixelBridge too. Necessary reverse engineering to debug library modifications is permitted. There is no mandatory signature allowlist or online activation blocking replacement. The optional package verifier reports a changed inventory, but is not an application-launch gate.

The relative `qt.conf` avoids embedding the developer's plugin path and can be adjusted with a compatible replacement. Windows provides ICU, D3DCompiler_47 and OS APIs; this package does not redistribute them. Unused Qt PDF, SVG and networking plugins and the DXC toolchain are excluded from the product runtime.
