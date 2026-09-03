# PixelBridge GitHub 首次发布检查清单

> 状态日期：2026-09-03
> 适用范围：源码仓库首次推送，不包含正式二进制 Release 认证
> 当前本地分支：`master`
> 当前 remote：未配置
> 当前项目 LICENSE：未选择，必须由仓库维护者决定

## 1. 当前发布准备结论

本仓库可以作为**开发中的源码项目**推送到 GitHub，但不能把当前状态描述为最终可用版本：

- 正式 Descriptor、多 Segment sender 和 decoder resume/storage 已有基础实现；
- 统一 `PB-Unified-LC4-V1` 视觉层、最终 Qt 收敛和完整产品 Gate 尚未完成；
- 构建目录、vcpkg 安装目录和本地运行 artifact 已由 `.gitignore` 排除；
- 当前最大受管文件远低于 GitHub 单文件限制，不需要为了现有源码/Golden 引入 Git LFS；
- Git remote 尚未配置，因此本次整理不会也不能自动 push；
- 项目自身 LICENSE 尚未选择。私有仓库可先推送；若公开供他人使用、修改或分发，应先明确许可。

## 2. 推送前必须确认的维护者决策

### 2.1 仓库可见性

- [ ] Private：可先保存开发历史，不对外授予额外权利；或
- [ ] Public：README 必须保持“开发中/未完成”事实边界，并处理 LICENSE。

### 2.2 项目自身 LICENSE

请选择并明确确认一种策略后再添加根目录 `LICENSE`：

- MIT：简短、宽松；
- BSD-3-Clause：宽松，和 Wirehair 的许可风格接近但不代表必须相同；
- Apache-2.0：宽松并包含明确专利条款；
- 暂不授权：不添加开源 LICENSE，并避免在 README/Release 中称为开源项目。

不得因为依赖库采用 BSD/MIT 就自动给 PixelBridge 选择同一许可。第三方许可义务与项目自身许可是两个独立问题。

### 2.3 默认分支

当前分支为 `master`。可以直接保留；如果维护者希望 GitHub 使用 `main`，应在首次 push 前明确执行并同步文档/保护规则：

```powershell
git branch -m master main
```

本文不自动改名，避免破坏现有本地引用或用户预期。

## 3. 受管源码清单检查

从仓库根目录运行：

```powershell
Set-Location -LiteralPath D:\MyProjects\PixelBridge

git status --short --branch
git log -5 --oneline
git ls-files
git diff --check
```

期望：

- 除明确保留的本地用户文件外，工作树 clean；
- 没有 `build*`、`.vs`、`vcpkg_installed`、`.part`、resume journal、日志、临时包；
- 没有误提交真实源文件、恢复输出、远程连接截图、主机身份或个人目录；
- 所有本次提交都能用 `git show --stat <commit>` 解释。

检查被忽略与未跟踪文件：

```powershell
git status --short --ignored
git clean -ndX
```

`git clean -ndX` 只能用于预览，**不得在发布准备中自动执行删除**。构建目录可能包含仍有价值的本地证据，是否删除由维护者决定。

检查受管文件尺寸：

```powershell
git ls-files | ForEach-Object {
    $item = Get-Item -LiteralPath $_ -ErrorAction SilentlyContinue
    if ($null -ne $item) {
        [PSCustomObject]@{ Bytes = $item.Length; Path = $_ }
    }
} | Sort-Object Bytes -Descending | Select-Object -First 30
```

若未来加入大型 Replay、MP4、安装包、20 GiB fixture 或 field evidence，不得直接提交。优先使用可重建 generator + digest manifest；确需分发的二进制放 GitHub Release/artifact storage，而不是源码历史。

## 4. 凭据与隐私检查

### 4.1 当前 tree 的窄范围搜索

以下命令只搜索 Git 已跟踪文本，不枚举用户目录或系统凭据：

```powershell
git grep -n -I -E "(BEGIN (RSA |EC |OPENSSH )?PRIVATE KEY|AKIA[0-9A-Z]{16}|gh[pousr]_[A-Za-z0-9_]{20,}|sk-[A-Za-z0-9_-]{20,})"
git grep -n -I -E "(password|passwd|api[_-]?key|client[_-]?secret|access[_-]?token)[[:space:]]*[:=]"
git grep -n -I -E "(C:\\Users\\|/Users/|/home/)[^ ]+"
```

解释规则：

- 搜索命中是审查线索，不自动等同于泄露；测试用占位符和检测正则可能合理存在；
- 真实 credential、签名私钥、访问 token、个人绝对路径和远程会话敏感 metadata 必须从当前 tree 与历史中移除/轮换；
- 不为“查漏”而扫描仓库外的 OS credential store、SSH 目录、浏览器或云账户。

### 4.2 历史检查

如果该仓库此前曾暂存或提交真实 secret，只从当前 tree 删除是不够的。先确定具体 blob/commit 和影响，再由维护者决定是否重写尚未发布的历史并轮换 secret。不要在普通整理任务中擅自 `filter-repo`、rebase 或 force-push。

## 5. 文档完整性检查

- [ ] 根 [`../README.md`](../README.md) 明确项目处于开发中，不声称 Unified/20 GiB/远程 Gate 已完成。
- [ ] [`README.md`](README.md) 可把当前规范、历史证据和模块文档区分开。
- [ ] [`UNIFIED_VISUAL_LARGE_FILE_IMPLEMENTATION_ROADMAP.md`](UNIFIED_VISUAL_LARGE_FILE_IMPLEMENTATION_ROADMAP.md) 是唯一 Goal 模式路线。
- [ ] 总体设计顶部注明旧 ChannelClass/RemoteVisual 产品路线的解释规则。
- [ ] [`CURRENT_RUNTIME_OPTION_INVENTORY.md`](CURRENT_RUNTIME_OPTION_INVENTORY.md) 清楚标注为过渡实现清单。
- [ ] [`PHASE0_PROTOCOL_STATUS.md`](PHASE0_PROTOCOL_STATUS.md) 清楚标注 provisional descriptor 为历史记录。
- [ ] 构建命令、依赖版本、Qt 根路径示例和可选 Gate 风险准确。
- [ ] 未执行的 full CTest、ASan、native、remote、20 GiB Gate 没有被写成已通过。

可以使用下面的轻量相对链接检查；它只检查本地 Markdown 文件链接，不访问网络、不验证锚点：

```powershell
@'
from pathlib import Path
import re
from urllib.parse import unquote

root = Path.cwd()
missing = []
pattern = re.compile(r"\[[^\]]*\]\(([^)]+)\)")
for markdown in [root / "README.md", *sorted((root / "docs").glob("*.md"))]:
    text = markdown.read_text(encoding="utf-8")
    for target in pattern.findall(text):
        target = target.strip().split("#", 1)[0]
        if not target or "://" in target or target.startswith("mailto:"):
            continue
        candidate = (markdown.parent / unquote(target)).resolve()
        if not candidate.exists():
            missing.append(f"{markdown.relative_to(root)} -> {target}")
if missing:
    raise SystemExit("Missing local Markdown links:\n" + "\n".join(missing))
print("Local Markdown file links: OK")
'@ | D:\Python3.12.9\python.exe -
```

## 6. 最小源码构建检查

首次推送源码前不需要重复完整硬件矩阵。若最新代码提交已经有对应定向结果，执行以下最小检查即可：

```powershell
cmake --build build-desktop-levels-release --config Release `
  --target PBProtocolTests PBStorageTests PBReceiverTests PBApplicationTests -- /m

ctest --test-dir build-desktop-levels-release -C Release `
  -R '^(PBProtocolTests|PBStorageTests|PBReceiverTests|PBApplicationTests)$' `
  --output-on-failure
```

当前 foundation 提交 `1445f9b` 在提交前执行过等价检查并得到 4/4 PASS。若代码未再变化，纯文档提交不需要重复运行。最终产品发布所需的完整 Release CTest、定向 ASan、右屏 native 和远程像素链按统一路线 G20/G21 执行。

## 7. `.gitignore` 与 artifact 策略

必须忽略：

- CMake/build tree 和 IDE 数据库；
- vcpkg installed tree；
- MSVC 中间产物和调试符号；
- `.part`、resume/partial journal、临时原子写文件；
- 日志、压缩包、安装/发布输出；
- Python cache。

必须受管：

- CMake/vcpkg manifest 与 overlay port；
- 源码、公共头、shader source/template；
- 最小 parser/fuzz corpus；
- 可重建、尺寸合理、身份冻结的 Golden；
- 架构、协议、操作和 Gate 文档；
- package/SBOM 生成与验证脚本，但不是生成结果。

不要把某个本地用户文件加入 `.gitignore` 来掩盖它；保持未跟踪并在显式暂存时排除即可。

## 8. Git 提交检查

### 8.1 禁止 broad-stage

始终显式暂存：

```powershell
git add -- README.md .gitignore .gitattributes docs/README.md `
  docs/UNIFIED_VISUAL_LARGE_FILE_IMPLEMENTATION_ROADMAP.md `
  docs/GITHUB_PUBLISH_CHECKLIST.md
```

实际命令应列出当次所有预期路径，并在提交前复核：

```powershell
git diff --cached --name-status
git diff --cached --stat
git diff --cached --check
```

不得使用 `git add .`、`git add -A`，不得因为“准备 push”而顺带提交用户未授权文件。

### 8.2 提交历史

- [ ] 每个提交有单一可解释目的；
- [ ] 不 amend 已完成提交；
- [ ] 不 reset/rebase/rewrite 历史；
- [ ] 不包含生成 build 输出；
- [ ] 测试与事实边界写在提交/最终报告中；
- [ ] 推送前 `git status --short --branch` 结果已人工确认。

## 9. 配置 GitHub remote 与首次 push

在 GitHub 创建空仓库后，复制其准确 HTTPS 或 SSH URL。不要让 GitHub 同时初始化 README/LICENSE/.gitignore，否则首次 push 前还需要处理两个独立历史。

查看当前 remote：

```powershell
git remote -v
```

添加 remote（将占位符替换为实际 URL）：

```powershell
git remote add origin <repository-url>
git remote -v
```

若保留 `master`：

```powershell
git push -u origin master
```

若维护者已明确改为 `main`：

```powershell
git push -u origin main
```

push 前再次检查 remote URL，避免推送到错误 owner/organization。不要使用 `--force`。如果远端不是空仓库，先停止并检查其历史，不要自动 `pull --rebase` 或覆盖。

## 10. GitHub 页面建议

首次源码推送后可在 GitHub 手动设置：

- Description：`Windows C++20 visual-channel file transfer research and implementation (work in progress)`；
- Topics：`windows`、`cpp20`、`qt`、`d3d11`、`forward-error-correction`、`visual-channel`；
- Default branch：与本地最终决定一致；
- Branch protection：最终产品开发开始协作后再启用 required review/checks；
- Issues/Discussions：按维护方式决定，不自动开放；
- Releases：只有 G22 的可验证 package 才创建正式 Release；开发中可使用 pre-release。

README 的醒目状态必须保持“开发中”，直到统一路线全部关闭。不要用历史 LF4 成功截图或 Phase 1.5 EXE 将仓库标为 production-ready。

## 11. CI 的现实边界

当前仓库没有已经验证的 GitHub Actions workflow。本次首次推送不仓促加入未经验证的 Windows/Qt/vcpkg CI，避免产生长期红灯或把 native Gate 错放到云 runner。

未来新增 CI 时建议分层：

1. Windows CPU/core build + protocol/receiver/storage unit；
2. 可缓存的 Qt GUI build + GUI smoke；
3. fuzz/ASan 独立 schedule；
4. D3D11/WGC/DXGI/native/remote 只在自托管、明确显示器和硬件身份的 runner 上运行。

云 CI 通过不能替代真实 DISPLAY2 或远程像素链 Gate。

## 12. 正式二进制 Release 额外要求

源码首次 push 不等于正式产品发布。正式 Release 还需要统一路线 G20..G22：

- 一次完整 Release CTest；
- parser/resume/storage 定向 ASan；
- DISPLAY2 native 尺度/cadence；
- 1 MiB 与 64 MiB 真实远程恢复；
- 20 GiB headless 能力；
- 两个 EXE 与 Qt/plugin/DLL 的完整 package；
- Git/tree/toolchain/dependency/profile/file hashes manifest；
- SPDX SBOM 和 `THIRD_PARTY_NOTICES`；
- clean-package smoke 和独立 verifier；
- 准确列出未执行环境与已知限制。

Release ZIP、PDB、Replay、MP4、大 fixture 和恢复输出不进入源码 Git 历史。

## 13. 最终人工确认单

在真正运行 `git push` 前逐项确认：

- [ ] `git remote -v` 指向正确的个人/组织仓库；
- [ ] 仓库 visibility 已决定；
- [ ] 项目 LICENSE 策略已决定或明确保持无授权；
- [ ] 当前分支名已决定；
- [ ] `git status --short --branch` 没有意外 staged/modified/untracked 内容；
- [ ] 受管文件无 secret、私有 key、个人 token 或真实用户数据；
- [ ] 本地 Markdown 链接通过；
- [ ] foundation 的定向测试证据仍对应当前代码提交；
- [ ] README 明确 Unified 和最终产品 Gate 尚未完成；
- [ ] 不需要 `--force`，远端为空或历史已人工理解；
- [ ] push 后检查 GitHub Files、README 渲染、默认分支和 commit 数量。
