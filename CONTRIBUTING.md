# 构建与贡献 / Building and contributing

[简体中文 README](README.md) · [English README](README.en.md)

## 1. 当前范围 / Current scope

当前是非本机单向视觉传输的稳定性与交付收尾，不是重新开启 Profile/密度/FPS 扫描。先读 [AGENTS](AGENTS.md)、[总体设计](docs/PixelBridge_最终技术路线与总体设计.md) / [architecture](docs/ARCHITECTURE.en.md) 和 [现状](docs/PROJECT_STATUS.md)。

The current phase is stability and delivery for non-local, one-way visual transfer. Review AGENTS, architecture, and current status before changes. Preserve runtime evidence, compatibility, bounded resources and final-file correctness. Product mode labels have no experiment badges; frozen internal identifiers remain unchanged.

## 2. 工具链 / Toolchain

- Windows x64；Visual Studio 2022 / MSVC C++20、Windows SDK。
- CMake ≥3.24；本地验证使用 CMake 3.31.6、MSVC 19.44、Qt 6.10.1 MSVC x64。
- Qt Widgets；vcpkg manifest 与 `third_party` 固定依赖。参见 [dependency baselines](third_party/README.md)。
- Python 用于可选测试/构建工具，不是产品 payload 通道。

The repository does not contain installed dependencies or assume the maintainer's absolute paths. Set `VCPKG_ROOT` to your vcpkg checkout and `QT_ROOT` to the MSVC x64 Qt prefix. Do not use the MinGW Qt build with MSVC.

```powershell
cmake -S . -B build-dev -G "Visual Studio 17 2022" -A x64 `
  "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  "-DPB_QT_ROOT=$env:QT_ROOT" -DPB_BUILD_TESTS=ON
cmake --build build-dev --config Release --target PixelBridgeEncoder PixelBridgeDecoder
cmake --build build-dev --config Release --target PBOperationalLoggingTests PBQSettingsTests PBApplicationTests PBExperimentalPam4ApplicationTests PBExperimentalPam4WideApplicationTests
$env:PATH = "$env:QT_ROOT/bin;$env:PATH"
ctest --test-dir build-dev -C Release --output-on-failure -R "^(PBOperationalLoggingTests|PBQSettingsTests|PBExperimentalPam4ApplicationTests|PBExperimentalPam4WideApplicationTests)$"
```

如果使用自己的 vcpkg install tree，也需让依赖 DLL 可被测试进程加载。首次拉取依赖可能需要网络；offline/frozen installation 应显式配置，不更改第三方版本凑编译。

If using a separate vcpkg installation tree, ensure its runtime DLLs are discoverable by tests. Initial dependency resolution can require network access. A release uses a pinned installation and records its identity; do not change library versions merely to bypass a failing build.

## 3. 定向验证 / Targeted validation

`--gui-smoke` 使用离屏窗口、真实应用/存储与可控测试边界，不操作桌面鼠标、焦点或显示设置。其通过不等于真实远控文件认证。普通 test 配置中启用的 smoke 不是正式包一定包含的承诺，正式构建需检查配置。

Use affected unit/integration/Golden tests first. Offscreen GUI tests are not real remote qualification. Native capture/display tests are opt-in with explicit target coordinates and safety prerequisites. Do not launch them on a user's screen without the required scope. Frequent remote test files should be ≤100 MB.

重点回归 / Core checks:

- descriptor/CRC/FEC Golden 与 malformed/boundary/conflict 输入；
- bounded capture lease/epoch/backpressure、single-monitor identity changes；
- memory-budget admission、resume corruption/quota/partial-write/crash windows；
- final digest → safe publication → reopen; cleanup warnings remain separate;
- GUI mode/configuration locking, real text layout, automatic logs, normal vs abnormal wait reasons.

## 4. 图标 / Branding assets

`apps/PixelBridgeEncoder/resources/logo.png` 与 Decoder 对应文件为维护者提供的原始图片。PNG 原图保持不变；`tools/branding/Build-Icons.ps1` 只按确定尺寸生成 ICO，Windows RC 与 Qt resources 分别负责 EXE 图标和界面图标。资源由程序嵌入，不依赖开发机桌面路径。

The maintainer-provided original PNGs remain separate from derived multi-size ICOs. The script performs format/size conversion, not redesign. Native executable and Qt UI resources are embedded; no absolute desktop path is required at runtime.

## 5. 变更与发布纪律 / Changes and releases

- Review before modifying; never overwrite unrelated user work.
- Small scoped diffs, explicit files when staging; no history rewrite or automatic push.
- Do not change wire/Profile/Golden/persistence contracts without the task requiring it.
- Preserve tests; if diagnostics add fields, update positive fixtures and retain strict small-cap/invalid-input rejection tests.
- Never commit user payloads, recordings, `.part`/`.resume`, installed dependencies, logs, build trees or release ZIPs.
- Keep Chinese/English current docs consistent; retained historical references must say so.
- v1.0 uses separate Encoder/Decoder packages from a clean committed source tree. Source/build/package manifests, dependency notices, isolated unpack/load checks, instructions and explicit validation limits belong in the release checklist.
- The project uses MIT; retain third-party licensing and corresponding-source obligations. Local release approval does not authorize a remote push.
