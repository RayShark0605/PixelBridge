# PixelBridge 正式包工具 / Product packaging tools

`New-PBUnifiedPortablePackage.ps1` 从干净提交的 Release 构建生成独立 Encoder / Decoder 便携包，`Test-PBUnifiedPortablePackage.ps1` 独立只读验证完整性。历史候选参数仍可使用；v1.0 使用显式 `-ProductRelease`，不会更改旧候选 schema 的语义。

The producer packages a clean committed Release build. The independent verifier never executes packaged applications. The optional product-release branch adds the MIT license, minimal runtime, role-specific bilingual instructions and corresponding Qt source; legacy candidate behavior remains available.

## 前置条件 / Prerequisites

- Windows x64, PowerShell 7, Python 3.12+ (source bundling only), CMake/VS2022 and the pinned Qt 6.10.1 MSVC installation with its **matching Src/qtbase component**.
- Release source committed and clean; reconfigure/rebuild **after the commit** so both `--build-identity` values equal HEAD. Do not rename an old binary as a new release.
- Installed vcpkg baseline, resolved from `VCPKG_INSTALLED_DIR` in the build's CMake cache; Qt/VC runtime deployment present.
- Explicit MSVC x64 redistributable and notices directories. No payload, personal logs, recovery state or private reports in the source inventory.
- GUI offscreen / affected regressions / Golden tests passed. Native display tests are opt-in; packaging does not implicitly run them.

## 对应源码 / Corresponding source

```powershell
python tools/PBUnifiedRelease/New-PBQtSourceBundle.py `
  --qt-root <qt-msvc-install>/6.10.1/msvc2022_64 --output-directory artifacts/release/qt-source
```

This read-only helper takes the complete matching Qt Base source, licenses and build scripts plus the installed SDK configuration. It produces a create-only deterministic ZIP and SHA-256 source manifest. No download, source mutation or executable launch occurs. `--output-directory` must not exist. See the bilingual [Qt source instructions](../../docs/QT_SOURCE.md).

## 生成 v1.0 / Build v1.0 packages

Adapt the following paths to the actual build and installed toolchain; none are universal defaults.

```powershell
$parameters = @{
  Label = 'current-head'
  BuildDirectory = '<repo>/build-release'
  OutputRoot = '<repo>/dist/v1.0.0'
  VersionedPackageName = $true
  ProductRelease = $true
  QtSourceBundleDirectory = '<repo>/artifacts/release/qt-source'
  VcRuntimeDirectory = 'C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Redist/MSVC/14.44.35112/x64/Microsoft.VC143.CRT'
  VcNoticesDirectory = 'C:/Program Files/Microsoft Visual Studio/2022/Community/Licenses/1033'
}
& tools/PBUnifiedRelease/New-PBUnifiedPortablePackage.ps1 -Role Encoder @parameters
& tools/PBUnifiedRelease/New-PBUnifiedPortablePackage.ps1 -Role Decoder @parameters
```

The two directories, ZIPs and external seals are create-only. Failed staging is retained, not silently overwritten. The producer runs only `--version`, `--build-identity` and `--unified-profile`, which do not start transmission or capture.

Product runtime: role EXE; Qt Core/Gui/Widgets; BLAKE3/Zstd; Qt Base's Windows/offscreen platforms and modern Windows style; app-local VC runtime. Windows supplies ICU/D3DCompiler_47. Unrelated PDF/SVG/network plugins, DXC, debug symbols, bridge tools and user settings are not packaged. `qt.conf` uses relative plugin paths. All included files, notices, source ZIP and official Qt SBOM are sealed. The top-level vcpkg SBOM records the whole installed build baseline, including test-only entries, not just runtime DLLs.

`unified-profile.json` binds the retained default SC6-V3/layout10 contract. It does **not** relabel PAM4/Wide as that protocol: their frozen independent identities remain in source and the mode documentation. Product-version changes do not change wire semantics.

## 独立验证 / Independent verification

Extract the archive into a new directory retaining its package name. Example:

```powershell
pwsh -NoProfile -File Test-PBUnifiedPortablePackage.ps1 `
  -PackageDirectory <fresh-unpacked-directory> `
  -PackageSealPath <external-seal.json> -ArchivePath <original.zip> `
  -ExpectedManifestSha256 <hash-from-a-trusted-channel> -OutputPath <new-result.json>
```

Rejects missing/extra/duplicate/unsafe paths, reparse points/ZIP symlinks, wrong bytes/hashes, malformed or ambiguous JSON, non-integer sizes, incorrect x64 GUI PE, missing VC runtime, inconsistent profile/SBOM/notices/seal, and unexpected product runtime/license/source content. File count/total size and ZIP expansion are bounded. It does not execute the EXE or unpack the nested corresponding-source archive.

Then test **freshly unpacked** application version/identity and `--gui-smoke` with developer Qt/vcpkg paths removed. These are local/offscreen correctness checks, not physical remote-channel or performance certification. The negative suite in `tests/tools/test_unified_package.py` mutates only a create-only derived copy and verifies that original artifacts remain unchanged.

源码清单通过 NUL 分隔的 UTF-8 Git 输出读取，不依赖 Windows 控制台代码页。可用 `tests/tools/VerifyUnifiedSourceInventory.ps1 -EvidenceDirectory <new-directory>` 回归中文/日文/带空格路径、GB936/UTF-8 两种代码页、忽略文件及无效仓库拒绝；不运行打包入口或应用程序。

Source enumeration reads NUL-delimited UTF-8 from Git independently of the console code page. The standalone regression above covers non-ASCII/space-containing paths, GB936/UTF-8 consoles, ignored files and invalid-repository rejection without running the packager or applications.

## 发布边界 / Release boundaries

- [MIT project license](../../LICENSE), [dependency notices](../../THIRD_PARTY_NOTICES.md), [v1.0 release notes](../../docs/RELEASE_V1.0.md).
- Hashes and unsigned seals provide integrity, not publisher authentication. The optional verifier is not an application launch gate and does not prevent compatible Qt replacement.
- No remote push, code signing, display setting change, input automation or new throughput claim is implied by successful packaging.
- Legacy `-CompactPackageName`, `-Role Both`, and up to eight `-ReleaseDocumentation` Markdown files remain supported outside `-ProductRelease`. They produce candidate inventories, not the v1.0 product package contract.
