# 第三方组件与许可 / Third-party components and licensing

PixelBridge 自有代码与随附项目文档采用 [MIT License](LICENSE)。第三方代码、库及工具并不因此变成 MIT，仍受各自许可证约束。正式包包含逐文件 SHA-256 清单、SPDX SBOM、第三方许可原文与来源信息。

PixelBridge-owned code and project documentation use the [MIT License](LICENSE). This does not relicense third-party code, libraries, or tools. Release packages include a file-level SHA-256 inventory, an SPDX SBOM, original dependency notices, and source information.

## Qt

正式 Windows 包使用动态链接的 Qt 6.10.1 Core、Gui、Widgets 及必要的 Qt Base 平台/样式插件。使用的 Qt 库按 GNU LGPL v3 及其包含的第三方条款分发。包内提供 LGPL v3、GPL v3、Qt Base 的许可/归属文件和对应源代码归档；不以项目 MIT 声明替代 Qt 的许可。

The Windows release dynamically links Qt 6.10.1 Core, Gui, Widgets, and the required Qt Base platform/style plugins. The Qt libraries are distributed under GNU LGPL v3 and the applicable terms of their bundled third-party components. Each product package includes LGPL v3, GPL v3, Qt Base licensing/attribution files, and a corresponding-source archive. PixelBridge's MIT license does not replace these terms.

允许用户以接口兼容的自行修改版本替换 Qt DLL，并为调试这类修改进行必要的逆向工程。便携程序没有签名白名单、加密库加载或强制在线校验限制这种替换；包验证器仅检查原始发布包的完整性，替换 DLL 后校验不匹配并不阻止正常启动。

Users may replace Qt DLLs with interface-compatible modified versions and reverse engineer as necessary to debug those modifications. The portable applications do not enforce signed-library allowlists, encrypted loading, or online checks that prevent replacement. The package verifier checks the original release inventory only; a mismatch after replacing a DLL does not block ordinary application launch.

官方许可说明：[Qt 6.10 licensing](https://doc.qt.io/qt-6.10/licensing.html)。源码与构建方式见产品包内 `QT_SOURCE.md` 和 `sources/`。分发本产品时请保留这些文件与许可原文。

For source and rebuilding instructions, see `QT_SOURCE.md` and `sources/` in the product package. Preserve these files and the original notices when redistributing the product. Official information: [Qt 6.10 licensing](https://doc.qt.io/qt-6.10/licensing.html).

## 其它依赖 / Other dependencies

| Component | Release baseline | Licensing / scope |
| --- | --- | --- |
| Wirehair | 2.0.0, commit `067ca7cdb66aed424ec23f97557429bf791c6f0c` | BSD-3-Clause; statically linked codec |
| BLAKE3 | 1.8.5 | Apache-2.0 / CC0-1.0 alternatives and upstream notices; runtime |
| Zstandard | 1.5.7 | BSD-3-Clause / GPL-2.0 alternatives; BSD option used |
| libpng | 1.6.58 | Upstream libpng license; dependency baseline |
| zlib | 1.3.2, port revision 1 | Zlib; dependency baseline |
| Catch2 | 3.15.0 | BSL-1.0; tests, not a shipped runtime DLL |
| Microsoft Visual C++ Runtime | Exact installed revision in release SBOM | Microsoft redistributable terms, not MIT; original redistributable context/notices included |

SBOM 的 vcpkg inventory 记录完整构建基线，不能据此认定每一项都是运行时 DLL。Qt Base 自带第三方组件的许可和版本由包内 Qt 官方 SBOM、源文件归属与许可文本补充说明。

The vcpkg inventory records the complete build baseline, not a claim that every entry is a runtime DLL. Qt's official SBOM, source attributions, and license texts additionally describe third-party components bundled into Qt Base.

libcimbar 为灵感来源，不是本项目打包的运行时依赖；致谢不改变任一项目的许可证。模型名称同样仅用于致谢，不是软件依赖或许可授予。

libcimbar is acknowledged as an inspiration, not a bundled runtime dependency. Acknowledgement does not change either project's license. Model names identify acknowledgements, not software dependencies or license grants.
