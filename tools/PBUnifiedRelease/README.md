# G22 Unified 本地便携候选

本目录为 SC6-V3 / layout 10 的独立发布工具。保留的 `PBRemoteVisualEvidence` schema-2 工具仍用于历史实验；
不能用旧 LF4 Profile manifest 为新版 Unified 作身份声明。当前工具由已有 inventory/seal/SBOM 逻辑派生，
另行冻结 `PixelBridge.UnifiedPortablePackage.1` / `UnifiedPortablePackageSeal.1`，不改旧 schema 的含义。

## 生成

需要 PowerShell 7、干净提交（唯一例外是既有 `docs/PHASE1_GATE_REPORT.md`）、重新配置并构建的 Release 双端、
已部署 Qt runtime/plugins、installed vcpkg 状态，以及明确指定的本机 MSVC x64 redistributable 和 notices 目录。

```powershell
cmake -S . -B build-unified-release
cmake --build build-unified-release --config Release --target PixelBridgeEncoder PixelBridgeDecoder --parallel 4

pwsh -NoProfile -File tools/PBUnifiedRelease/New-PBUnifiedPortablePackage.ps1 -Role Both -Label current-head -CompactPackageName `
  -BuildDirectory <repo>\build-unified-release `
  -OutputRoot <repo>\artifacts\g22-package-fresh `
  -VcRuntimeDirectory 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Redist\MSVC\14.44.35112\x64\Microsoft.VC143.CRT' `
  -VcNoticesDirectory 'C:\Program Files\Microsoft Visual Studio\2022\Community\Licenses\1033'
```

产物包括双端 EXE 及依赖、`USER_GUIDE.md`、完整编译 Profile JSON、独立 verifier、SPDX SBOM、Qt/vcpkg/MSVC notices、
package manifest、ZIP 和外部 seal。输入根与输出 artifact 不修改；输出 create-only，失败时保留 partial 以便检查。
当前仓库没有选定项目自身 LICENSE，SBOM 保留 `NOASSERTION`。这是本地候选，不代表公开分发、签名或许可选择已获批准。
installed vcpkg inventory 包括 build/test 工具，不声称这些包全都运行时动态链接。

生成器实际执行双端 `--version` / `--build-identity` / `--unified-profile`，核对当前 commit 与完整 Profile digest。
这些是只读诊断命令，不开启 Qt 窗口、捕获或传输。

## 独立验证

先在新目录解压 ZIP，保留原 package 目录名。以下命令只读验证，不运行包内 EXE：

```powershell
pwsh -NoProfile -File Test-PBUnifiedPortablePackage.ps1 `
  -PackageDirectory <新解压的package目录> -PackageSealPath <外部seal.json> -ArchivePath <原始zip> `
  -ExpectedManifestSha256 <可信渠道获得的manifest-hash> -OutputPath <尚不存在的验证结果.json>
```

Verifier 拒绝缺失/额外/重复/不安全路径、Windows reparse 或 ZIP symlink、错误大小/哈希、重复或大小写歧义 JSON key、
非整数 size、错误 GUI/x64 PE、缺失 VC runtime、错误 Profile/SBOM/notices/工具链绑定和不一致的外部 seal。
ZIP 解压校验以封印长度加单字节 sentinel 限制实际读出，不能信任 ZIP 自报长度而无限处理解压内容。

无签名的哈希/seal 只用于完整性与身份一致性，不证明发布者真实性。
包完整性通过后仍需从新解压目录执行版本检查、双端 `--gui-smoke` 和 G22 右屏最小实际像素恢复；
本工具本身不产生 GUI/实屏、性能或完整 G22 通过声明。

## 版本化交付（v0.6起）

`-VersionedPackageName`可替代`-CompactPackageName`，从双端实际runtime identity的一致版本生成`PixelBridge-v0.6.0-win64`类名称；二者不能同时指定。SBOM项目版本同样来自实际应用版本，不再硬编码0.1.0。可用`-ReleaseDocumentation docs/RELEASE_V0.6.md,docs/SESSION_HANDOFF_20260914_V0.6.md,docs/NEXT_TASK_PROMPT_V0.6.md`将最多8个、每个不超过2MiB的仓库docs Markdown纳入同一哈希清单。PowerShell命令行可用数组调用传参。源码干净提交、tag、预算、profile、SBOM/notices、封印和独立verifier等原检查不变。
