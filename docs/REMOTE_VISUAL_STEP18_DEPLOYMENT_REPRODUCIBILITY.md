# RemoteVisual Step 18：便携包、source set、双端环境与 RunId 部署闭环

## 1. 目标与结论

Step 18 关闭的是“Computer A/B 是否实际运行同一份已测试实现、依赖和 LF4 identity”这一部署证据问题，而不是远控信道质量或文件恢复 Gate。正式运行不再分别拼装 Encoder/Decoder 包，而是把同一个 `-Role Both` ZIP 原样复制到两端；两端 environment artifact 必须指向各自解压目录内同一相对路径的 packaged Decoder，并记录完全相同的 package manifest SHA-256。

一条正式记录由一个 OS-CSPRNG 128-bit `RunId` 串起以下唯一对象：

1. 一个 `PixelBridge.PortablePackage.2` manifest 与外部 seal；
2. 一个 `PixelBridge.RemoteVisualSourceSet.2` manifest 与外部 seal；
3. 一个共享 `PixelBridge.RemoteVisualRunMetadata.1` preset；
4. 恰好一份 Encoder 和一份 Decoder `PixelBridge.EndpointEnvironment.2`；
5. 一个经独立验证的 `PixelBridge.RemoteVisualDeployment.1` manifest。

`Test-PBRemoteVisualDeploymentManifest.ps1` 从 RunId 反向验证上述全部 artifact 的 path/size/SHA-256、自指纹和交叉绑定。任何一个文件、manifest 字段、RunId、端点角色或依赖 identity 变化都会使验证失败。

## 2. 单一共享便携包

### 2.1 create-only 发布

`New-PBRemoteVisualPortablePackage.ps1` 使用 sibling `.partial` 目录、ZIP 和 seal 构建候选。最终目录/ZIP/seal 预先存在时直接拒绝；生成结束后只有在独立 verifier 通过时才标记为已发布，验证失败会只清理本次精确创建的路径。

正式包使用：

```powershell
.\tools\PBRemoteVisualEvidence\New-PBRemoteVisualPortablePackage.ps1 `
  -Role Both `
  -Label final-hardening `
  -BuildDirectory <clean-release-build> `
  -OutputRoot <new-package-output>
```

双角色包的固定布局为：

```text
Encoder/PixelBridgeEncoder.exe
Decoder/PixelBridgeDecoder.exe
licenses/qt/...
licenses/vcpkg/...
THIRD_PARTY_NOTICES.txt
SBOM.spdx.json
package-manifest.json
```

包中不包含 source-set payload、Receiver 输出、Replay、endpoint environment 或远控软件数据，因此不能形成 Encoder→Decoder 的隐藏文件旁路。

### 2.2 build/source/dependency identity

manifest 同时记录：

- HEAD commit/tree、当前 tracked+untracked source 文件的 path/size/SHA-256 与规范化总指纹；
- 冻结的 `phase1-gate-pass` annotated-tag object/peeled commit；
- CMake generator/cache、MSVC executable/version/x64 identity、Windows SDK；
- Qt version、deployed `Qt6Core.dll` identity、licenseInfo identity；
- vcpkg builtin baseline、triplet、status database、每个已安装 package 的 version/port-version/ABI；
- LF4 token、VisualProfileId、LayoutVersion、8100 coded bytes、4 codewords 和 1..5 Hz 范围；
- 两个 executable 及所有 DLL/plugin/resource/license/metadata 文件的 exact inventory。

唯一允许排除的是任务开始前已存在且由用户持有的 `docs/PHASE1_GATE_REPORT.md`。调用者不能通过 `-ExcludedSourcePath` 排除实现、测试或其它文档。

SPDX 2.3 SBOM 和 `THIRD_PARTY_NOTICES.txt` 覆盖 Qt 及 vcpkg status 中的全部 package。verifier 不只检查文件存在，还逐项比对 Qt/vcpkg version、ABI、license 文件和 notices 文本。

### 2.3 独立验证

`Test-PBRemoteVisualPortablePackage.ps1` 具有以下 fail-closed 边界：

- bounded JSON、file count、payload bytes 和 ZIP size；
- relative path、case-insensitive duplicate、path escape 和 reparse-point 拒绝；
- manifest 之外的目录文件或 ZIP entry 拒绝；
- manifest、payload inventory、application、Qt、license、SBOM 和 seal 交叉验证；
- ZIP 每个 entry 都流式重算 SHA-256，不把解压内容无界读入内存；
- 可选 `-ExpectedManifestSha256` 用于复制前后 exact identity 验证；
- `-OutputPath` 为 create-only，使用 `.partial` 后原子发布。

## 3. CSPRNG source set

`New-PBRemoteVisualSourceSet.ps1` 在 Computer B 生成：

| 文件 | 内容 | PixelBridge Segment compression |
| --- | --- | --- |
| `random-1MiB.bin` | 1 MiB OS-CSPRNG bytes | `RAW/OFF` |
| `random-8MiB.bin` | 8 MiB OS-CSPRNG bytes | `RAW/OFF` |
| `random-payload-4MiB.zip` | 恰好一个 4 MiB CSPRNG `payload.bin` | ZIP 本身作为 `RAW/OFF` source |

4 MiB 中间文件名带 `.partial`，其 SHA-256 在写 ZIP 前取得，ZIP 完成后立即删除；它不属于发布 inventory。source manifest 记录独立 CSPRNG `sourceSetId`、三个 source 的 exact identity、ZIP inner payload identity 和规范化 `sourceSetFingerprintSha256`。外部 seal 再绑定 manifest SHA-256。

`Test-PBRemoteVisualSourceSet.ps1` 要求目录平坦且只有三个 payload 加 manifest，ZIP 只有一个 `payload.bin`，并流式验证 inner hash。它拒绝错误 size/hash、额外文件/目录/entry、reparse point、错误 seal、错误 expected manifest hash 和 verification overwrite。

这些 source 仍受 Encoder 已有 Session immutable-source 检查约束；Step 18 的 manifest 不能替代运行期 file handle/size/last-write revalidation。

## 4. shared metadata 与双端 environment

`New-PBRemoteVisualRunPreset.ps1` 生成共享 metadata 和 128-bit RunId。provider 名称、版本、模式、chroma/网络等字段只是证据，不选择 demod threshold、FEC、Receiver admission 或发布语义。

`Get-PBRemoteVisualEnvironment.ps1` 必须同时收到 package/source manifest+seal 和 shared metadata。每次调用会先运行 package/source verifier，然后要求 `PixelBridgeDecoderPath` 精确等于共享包中 manifest 指定的 `Decoder/PixelBridgeDecoder.exe`，再执行两个只读命令：

```text
PixelBridgeDecoder.exe --version
PixelBridgeDecoder.exe --list-monitors
```

它还读取 OS/build、CPU、GPU/driver、显示适配器和 EDID 元数据。不会捕获屏幕像素、截图、安装 input hook、改变显示设置、移动窗口、访问私有 provider API 或启动 GUI。processor/GPU/EDID 各自最多 256 项，monitor JSON 最多 1 MiB，最终 environment 最多 2 MiB。

PowerShell/JSON 会把部分 CIM 数值在持久化往返时规范化为不同 CLR numeric type。environment 自指纹因此明确计算在“实际序列化后重新解析”的有序 JSON 表示上，而不是瞬时 CIM object 上；独立 verifier 读取磁盘 artifact 后可得到相同指纹。

## 5. deployment manifest 与反查语义

`New-PBRemoteVisualDeploymentManifest.ps1` 接受 exact package/source/seal/metadata 和两端 environment 文件，构造两层记录：

- `logicalIdentity`：RunId、package build/source/payload identity、sourceSet identity、provider metadata 和两端 environment hash/fingerprint；
- `artifacts`：所有输入文件的 absolute path/size/SHA-256。

`deploymentFingerprintSha256` 是有序 `logicalIdentity` 的 SHA-256。生成器先对 `.partial` deployment 执行独立验证，原子改名后再验证最终路径；任一阶段失败都不会保留正式输出。

`Test-PBRemoteVisualDeploymentManifest.ps1` 会重新运行 package/source verifier，重新读取 metadata 和 endpoint environment，移除 environment 自指纹字段后复算其余完整内容，并要求：

- package 必须同时且仅绑定 Encoder/Decoder 两角色；
- metadata、两端 environment 和 deployment 使用同一 RunId；
- 两端 package manifest/seal、source manifest/seal 和 metadata 的 size/SHA-256 完全相同；
- package name/build/source/payload fingerprint 与 sourceSetId/source fingerprint 全部一致；
- endpoint roles 恰好为 Encoder、Decoder 各一份；
- deployment logical endpoint hash 与磁盘 environment artifact 完全一致。

因此正式 run 可以由 RunId 唯一定位 package、source 和两端环境；不同路径上的同一份包以 manifest/payload hash 判断，而不是以路径字符串判断。

## 6. 自动化和 adversarial 验证

Windows CTest `PBRemoteVisualDeploymentEvidenceContracts` 以独立 `pwsh` 子进程覆盖：

- 八个 Step-18 PowerShell 脚本的 parser closure；
- source-set 完整生成/验证、inner ZIP identity 和无 intermediate/partial；
- payload 单 byte tamper、错误 expected manifest SHA、重复 verification output；
- source-set 与 metadata create-only 重入拒绝且原 artifact hash 不变；
- 每轮只在测试专属 GUID 目录中工作并在边界核对后删除。

正式 hardening 还必须用 clean Release build 生成真实 Both-role package，把同一个 ZIP 解压到两个干净目录，分别以同一个 expected manifest SHA 验证并执行 packaged `--version`/`--list-monitors`。另复制一份 package 修改 DLL 单 byte，要求 verifier 非零且不能生成 verification artifact。exact hashes、工具输出和失败诊断保存在 create-only `build-p1_5-evidence/step18-*` evidence 目录及其中的 `deployment-summary.json`，不提交大体积 package/source/environment artifact。

## 7. 真值边界

Step 18 的成功只证明 deployment identity 和 artifact integrity：

- 不证明两台物理电脑已通过远控链路传输文件；
- 不证明 provider mode/chroma/scale/UI 声明真实生效；
- 不产生 UniqueVisualFPS、VerifiedEncodedGoodput、WholeFileDigest 或 safe-publish 成功；
- 本机创建两份 endpoint snapshot 只验证 schema/identity pipeline，不能替代 Step 20 的真实 Computer A/B 环境；
- package/source/metadata/environment 均不进入 Receiver acceptance，不能改变 FEC、CRC、Outer、WholeFileDigest 或发布判决。

真实双机 LF4 pilot、provider matrix、重复恢复、QoS 和 soak 仍分别由 Step 20..24 关闭。
