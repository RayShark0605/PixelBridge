# PixelBridge v1.0.0 — 本地正式版 / Local product release

日期 / Date: 2026-09-17. Windows x64, Encoder / Decoder 分别打包。This is a local release; creating it does not publish or push to a remote host.

## 面向使用者 / For users

- **建议 Decoder 先开始接收，再启动 Encoder 发送。** **Start Decoder before Encoder.**
- **大文件“已接收大小”短时不涨可能正常，恢复/轮播/校验/写盘期间不要暂停。** **Brief size plateaus can be normal during repair, carousel waiting, verification or disk writes: do not pause.** Explicit errors, capture interruptions and persistent absence of useful data must still be investigated.
- 1080P 非本机场景优先尝试 PAM4，25 Hz 起步；可容纳 2560×1440 源码面的环境可选择 Wide。两端模式一致。For 1080P non-local use, try PAM4 at 25 Hz first; use Wide only where its 2560×1440 source raster fits. Match modes at both ends.
- 解压整个目录，阅读随包 `USER_GUIDE.md` / `USER_GUIDE.en.md`。Extract the whole directory and read the role-specific instructions; do not copy the EXE alone.

## 本版内容 / Contents

- Standard / Gray Fast / PAM4 / PAM4 Wide GUI entries; frozen protocol/profile identities remain unchanged by the product-version bump.
- Selected-screen whole-screen/ROI receiving, configurable finite decoder memory budgets, bounded resume checkpoints, full-file digest/publication/reopen checks.
- Distinct embedded application icons and UI logos; automatic endpoint logs and explanations of current recovery activity.
- Bilingual user/technical documentation; MIT project license, original dependency terms, SPDX inventory, Qt corresponding source and build information.
- Separate `PixelBridge-v1.0.0-Encoder-win64.zip` and `PixelBridge-v1.0.0-Decoder-win64.zip`, matching external `.seal.json` files, and optional read-only package verifier.

## 验证与可追溯性 / Validation and traceability

The package's `package-manifest.json` records the exact application version, Git commit/tree, source inventory, toolchain/dependency identities and per-file hashes. The external seal binds its manifest and ZIP. Release evidence is retained outside Git under `artifacts/v1-release-20260917/`, with build/test commands, results and independent unpacked checks. Do not infer a fresh remote throughput measurement from a packaging or offscreen test.

Validation covers release compilation, the non-interactive regression/Golden suite, endpoint identity/GUI-offscreen checks, isolated unpacked DLL resolution and file integrity. Native display/remote/single-physical-monitor testing is not silently enabled. Any final result is reported separately in the release evidence; the manifest is the authority for the delivered binary identity.

## 限制 / Limits

- **发布回归不是全绿：完整非交互 CTest 为 172/173 通过。** 唯一失败是保留的灰阶 spatial late-join 合成性能门：加入后 12,505 帧完成，未达到 `< 12,000` 帧要求。分段/整文件校验、安全发布及重开通过；2026-09-15 留存二进制与 v1.0 初次构建复现相同结果，v0.6 交接已经登记此项。测试与阈值不修改、不跳过、不标成通过。**The regression suite is not fully green: 172/173 CTest entries pass.** The retained Gray Fast spatial late-join performance gate finishes after 12,505 post-join frames, exceeding its `< 12,000` requirement. Final-file correctness passes. This is a reproduced historical limit, not a new v1.0 regression; the failing test remains intact.
- 维护者已明确选择“如实注明已知性能限制，继续交付”。此决定仅接受本次 v1.0 的已登记限制，不意味着该性能门通过或以后可以忽略。The maintainer explicitly accepted delivery with this documented limit; this is not a passing performance result or a blanket waiver for future work.
- 单文件准入上限 **500 GiB**；采用分段流式设计，不要求整个文件驻留内存。Current admission limit is **500 GiB**, not a tested maximum; streaming keeps RAM dependent on the active segment set.
- 特定旧封存候选的大文件平均速度约 **279 KB/s**（1 KB = 1024 B）。这是留存环境成绩，不是本次新构建重新测速，也不是所有硬件/远控环境保证。The retained result belongs to its original candidate/environment, not a fresh benchmark of these binaries.
- 一小时与全程无停滞目标未达成。One-hour and zero-plateau objectives remain unmet; no universal maximum-throughput claim is made.
- 单物理屏电脑没有独立实机验收；双屏机器中的指定单屏入口已验证。No independent qualification on a computer having only one physical display.
- 没有反向 ACK，Encoder 不会自动知道接收完成。No receiver completion ACK; stop Encoder after Decoder confirms success.
- 未签名本地发布，未执行远程 push。Hashes/seals establish integrity, not publisher authentication. No remote push is part of this release.

## 许可 / Licensing

PixelBridge-owned code/documentation: MIT, see root `LICENSE`. Qt and other dependencies retain separate terms; see `THIRD_PARTY_NOTICES.md`, `licenses/`, `SBOM.spdx.json`, `QT_SOURCE.md` and the included corresponding source. Acknowledgements to libcimbar and GPT-5.6 Sol, GPT-6 Astra, GLM-5.3 and Qwen 3.8 Flash Next appear in `ACKNOWLEDGEMENTS.md`.
