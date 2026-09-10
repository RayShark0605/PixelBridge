# PBRemoteOpsBridge — 基于 SMB 共享目录的远程实验命令桥

> **操作入口文档：** 面向接手者的机制/用法/排查指南在
> [`docs/REMOTE_OPS_BRIDGE.md`](../../docs/REMOTE_OPS_BRIDGE.md)；本文件是 wire 协议与
> 部署细节的权威定义。两处冲突时以本文件与实现为准。

本工具让控制端（本机，运行 `pbops.py`）通过一个共享目录树驱动远程机（运行
`pbops_listener.py`）上的实验操作：投递/部署实验包、启动与停止
`PixelBridgeEncoder.exe`、切换远程显示器模式（刷新率实验）、截屏、采集证据、执行
诊断脚本——全程无需人工在远程机上操作。

## 纪律红线（先读）

1. **本通道只做编排（orchestration-only）。** 它只传递命令、实验包、脚本、日志与
   测量元数据。**绝不**通过它向 Decoder 递送像素或 payload；Decoder 仍然只能从
   实际捕获的屏幕像素恢复数据（`AGENTS.md` 第 1 节）。任何把本通道用作第二条
   payload 通道的用法都是违规。
2. **禁止跨主机时钟运算。** 结果与心跳里的时间戳都带产生它的机器后缀
   (`startedAtRemote` / `issuedAtLocal`)，只作日志参考；跨主机关联只允许用命令 id。
3. 谁能写共享目录，谁就能驱动远程机（等效于远程代码执行）。共享 ACL 就是权限边界，
   不要把该目录开放给不可信账户。
4. 停止远程进程的默认对象是**监听器登记过的 PID**；未登记的 PID 必须显式传
   `allowImageName` 才允许触碰。强制终止永远被如实记录（`forced=true`），
   不冒充优雅退出。

## 目录结构

```
tools/PBRemoteOpsBridge/
  listener/pbops_listener.py    # 远程机监听器（Python 3.8+，纯 stdlib + ctypes）
  listener/pbops_listener.json  # 配置模板（shareRoot 为 null：必须显式提供，绝不写死）
  bootstrap/Install-PBOpsListener.cmd   # 一次性安装（共享根作为必填参数）
  bootstrap/Start-PBOpsListener.cmd     # 手动启动
  bootstrap/Uninstall-PBOpsListener.cmd # 卸载（可选 --purge 删除工作区）
  local/pbops.py                # 控制端助手（Python 3.8+）
```

本工具不进入产品构建；无 CMake 改动；不触碰任何产品源码、协议常量或 Golden Vector。

## 共享目录协议

控制端在共享根下维护（`pbops.py init` 创建）：

```
<ShareRoot>/            远程机上通常是 UNC（如 \\<HOST>\<SHARE>\pbops）；本机是盘符路径
  inbox/                cmd-<UTC时间戳>Z-<6hex>.json   命令（控制端 → 远程）
  processed/            监听器取走后移入（同卷原子 rename；先移动后执行）
  results/              res-<cmdId>.json + res-<cmdId>-artifacts/   结果（远程 → 控制端）
  status/heartbeat.json 每个心跳周期原子重写；>10 秒未更新即视为失联
  files/                大文件投递区：<name> + <name>.sha256 + <name>.ready（就绪标记）
  staging/              双方原子写临时区（唯一临时名，写完 rename）
```

- 一切文件写入都走 **临时文件 + rename**（服务端原子）；命令文件 ≤ 64 KiB。
- 文件名即身份：命令 id 必须等于文件名主干；重复 id 的第二个结果写为
  `res-<id>.dup-<hex>.json` 且状态 `rejected`。
- 监听器把命令从 `inbox/` 原子移动到 `processed/` **之后**才执行；若监听器中途死掉，
  重启时为所有"已取走但无结果"的命令补写 `interrupted` 结果（崩溃一致性）。
- 畸形 JSON、未知信封键、未知类型、未知 `schemaVersion` → `rejected` 结果，
  监听器进程本身永不因单条命令退出。

### 命令封套

```json
{
  "schemaVersion": 1,
  "id": "cmd-20260910T081500Z-a1b2c3",
  "type": "deploy",
  "params": { },
  "timeoutSeconds": 600,
  "issuedAtLocal": "2026-09-10T16:15:00+0800"
}
```

`timeoutSeconds` 缺省 600，上限 21600。`type` 与 `params` 见下表。未知信封键被拒绝
（防拼写错误）；`params` 内部键由各处理器自行校验。

### 结果封套（`results/res-<cmdId>.json`）

```json
{
  "schemaVersion": 1, "commandId": "...", "type": "...",
  "status": "ok | error | timeout | rejected | interrupted",
  "exitCode": null, "startedAtRemote": "...", "finishedAtRemote": "...",
  "payload": { }, "stdout": "", "stderr": "",
  "error": null
}
```

`stdout/stderr` 各截断至 64 KiB（截断事实记录在 payload 里）。

## 命令集（schemaVersion 1）

| type | 关键 params | 语义与要点 |
| --- | --- | --- |
| `ping` | — | 回显主机/用户/pid/监听器版本/shareRoot/workspace |
| `listener-shutdown` | `confirm: true` | 监听器写完本条 ok 结果后干净退出（写终态心跳） |
| `display-info` | `monitor?`（索引或 `\\.\DISPLAYx`） | 枚举显示器与全部可用模式（只读） |
| `display-set` | `monitor, width, height, refresh, bitsPerPixel?` | 先在枚举结果里确认模式存在，再 `ChangeDisplaySettingsEx` 动态切换（会话级，不写注册表）；前置模式存入恢复栈 |
| `display-restore` | `monitor?` | 恢复最近一次 `display-set` 之前的模式 |
| `deploy` | `runId, file, sha256?` | 校验 `files/<file>` 哈希（param 或 `.sha256` sidecar）后：`.zip` → 安全解包到 `<ws>/runs/<runId>/package/`（拒绝路径穿越/绝对路径/ADS/保留名/符号链接与 reparse/目录项/重复项/加密项，逐项流式哈希，解包后再查盘上 reparse）；其他 → 复制到 `runs/<runId>/input/`。runId 只能新建 |
| `start` | `runId, exe, args[], console?=true, waitSeconds?=3, cwdSub?` | 启动 run 内 exe。`console:true` 用独立控制台（Encoder `--manual-stop` 需要）；`console:false` 无窗口并把 stdout/stderr 重定向到 run 的 `logs/`。启动后登记 PID；`waitSeconds` 内存活检查。**监听器不发明 Encoder 参数**，args 由控制端按配方组装 |
| `stop` | `pid` 或 `runId` 或 `all:true`；`graceSeconds?=15`；`allowImageName?` | 优雅降级链：控制台注入 `q`（AttachConsole+WriteConsoleInput，作用于 `--manual-stop`）→ WM_CLOSE（GUI 优雅停止）→ 强制终止（如实记 `forced=true`）。默认只碰登记过的 PID |
| `run-script` | `runId, script, args?[], timeoutSeconds?`（≤3600） | 执行 run 内 `.ps1`：Job-object 有界运行（kill-on-close、2 GiB 内存上限），stdout/stderr 截断入结果 |
| `collect` | `runId, patterns[], maxBytes?=512 MiB` | 把 run 目录内匹配文件复制到 `results/res-<id>-artifacts/`，附 SHA-256 清单；超预算/超条数**跳过并注明**，绝不静默 |
| `screenshot` | `monitorIndex?=0` 或 `all:true` | GDI 全物理像素截屏（DPI aware），依赖无关 PNG 编码，入 artifacts |
| `list-runs` | — | run 清单（体积/时间）+ 当前登记的存活进程 |
| `cleanup` | `processedOlderThanDays?` / `resultsOlderThanDays?` / `filesOlderThanDays?` / `runs?[]` / `maxItems?=1000` | 有界清理，逐项报告删除/跳过；有存活登记进程的 run 拒绝删除 |

## 控制端助手 `pbops.py`

```text
pbops.py [--root PATH] init                          # 创建共享目录树
pbops.py [--root PATH] beat                          # 心跳新鲜度（0=新鲜 1=过期 2=缺失）
pbops.py [--root PATH] send <type> [--param k=v ...] [--params-json JSON] [--timeout S]
pbops.py [--root PATH] await <cmdId> [--timeout S]   # 退出码 0=ok 3=非ok 2=超时
pbops.py [--root PATH] run <type> [...]              # send + await
pbops.py [--root PATH] drop <localFile> [--name N]   # 大文件入 files/：流式哈希+sidecar+ready 标记
pbops.py [--root PATH] pull <cmdId> [--dest DIR]     # 把 artifacts 拉到本地工作目录
pbops.py selftest                                    # 临时根上的端到端冒烟（自动起/停监听器）
```

- `--root` / 环境变量 `PBOPS_ROOT` 覆盖默认的 `<ShareRoot>`（默认值只是本机侧便利，
  不是协议假设）。
- `drop` 是复制不是移动，源文件不动；`files/` 内同名文件已存在即拒绝（先 cleanup）。
- 大文件经 SMB 的复制速度受网络限制；1 GiB 量级通常几十秒。

## 远程机部署（持久机器，一次性）

前置：远程机能读写共享根（UNC 如 `\\<HOST>\<SHARE>\pbops`），装有 Python 3.8+。

1. 控制端先把本目录投到共享根（`bootstrap/` 与 `listener/` 两个文件夹）：
   ```text
   <ShareRoot>/bootstrap/Install-PBOpsListener.cmd
   <ShareRoot>/bootstrap/Start-PBOpsListener.cmd
   <ShareRoot>/bootstrap/Uninstall-PBOpsListener.cmd
   <ShareRoot>/listener/pbops_listener.py
   <ShareRoot>/listener/pbops_listener.json
   ```
2. 在远程机上运行一次：
   ```text
   Install-PBOpsListener.cmd \\<HOST>\<SHARE>\pbops
   ```
   安装器会：校验 Python ≥ 3.8 → 复制监听器到 `%LOCALAPPDATA%\PixelBridgeOps\listener`
   → 生成**部署配置** `pbops_listener.deploy.json`（写入你传入的共享根，任何路径都不
   写死）→ 创建 Startup 快捷方式（pythonw 无窗口自启）→ 立即启动。
3. 在控制端验证：
   ```text
   pbops.py --root \\<HOST>\<SHARE>\pbops beat        # 或本机路径 <ShareRoot>
   pbops.py --root <ShareRoot> run ping
   pbops.py --root <ShareRoot> run screenshot --param monitorIndex=0
   ```
   （`beat` 的新鲜度按共享文件 mtime 判定——共享就在控制端本机磁盘上，无跨机时钟问题。）

重新登录/重启后监听器由 Startup 快捷方式自启；手动补启用 `Start-PBOpsListener.cmd`。
换共享路径：重新跑安装器传新路径，或手改 `pbops_listener.deploy.json` 的 `shareRoot`。

## 远程 Encoder headless 实验配方

产品 CLI 契约见 `docs/CURRENT_RUNTIME_OPTION_INVENTORY.md`。典型一次实验：

```text
# 1) 控制端：投放实验包（封存 zip，含 PixelBridgeEncoder.exe 等）
pbops.py drop PixelBridge-Gxx-Encoder.zip
pbops.py run deploy --param runId=r20260910a --param file=PixelBridge-Gxx-Encoder.zip

# 2) （可选）刷新率实验：先查模式再切换；Encoder 侧另有 --logical-fps 档位
pbops.py run display-info --param monitor=0
pbops.py run display-set --param monitor=0 --param width=1920 --param height=1080 --param refresh=60

# 3) 启动 Encoder（headless 全自动配方；--seconds 到时自动优雅收尾）
pbops.py run start --params-json '{"runId":"r20260910a","exe":"package/Encoder/PixelBridgeEncoder.exe","args":["--headless-broadcast","--source","input/payload.bin","--profile","unified","--channel","remote","--remote-provider","UnknownRemoteLink","--single-monitor-fullscreen","primary","--logical-fps","15","--seconds","180","--report","encoder-report.json","--journal","encoder-evidence.jsonl"],"console":false,"waitSeconds":5}'

# 4) 控制端同时启动本机 Decoder 接收（由控制端自行驱动，与本桥无关）

# 5) 采集证据并停止（console:false 时 stop 走 WM_CLOSE/强制；提前停建议配方 A）
pbops.py run collect --param runId=r20260910a --param 'patterns=["*.json","*.jsonl"]'
pbops.py run stop --param runId=r20260910a
pbops.py pull <collect的cmdId>
```

两种停止配方：

- **配方 A（推荐，全自动优雅）**：`--seconds N` 启动，Encoder 到时自行优雅收尾，
  退出码 0/1/2 有完整语义；桥的 `stop` 只用于异常中断。
- **配方 B（人工停止语义复刻）**：`--loop --manual-stop`（要求
  `--single-monitor-fullscreen`）+ `console:true` 启动；`stop` 命令的第一级
  （控制台注入 `q`）即等价于 G21 现场里"人在控制台按 Q"。注入失败会如实降级。
- GUI 产品模式（`--gui-measurement`）只建议用 `start` 拉起、`screenshot` 观察；
  开始传输与 Esc 停止仍是 GUI 人工合同，本桥不做远程 UI 自动化。

刷新率的两个层面：`display-set` 改的是**远程显示器物理模式**（虚拟适配器可能拒绝，
错误码如实返回）；`--logical-fps` 是 **Encoder 逻辑帧率**（1..60）。两者独立可组合。

## 监听器配置（`pbops_listener.json` / deploy 配置）

| 键 | 默认 | 说明 |
| --- | --- | --- |
| `shareRoot` | **null** | 共享根；null 且无 `--share-root` 时**拒绝启动**（fail closed） |
| `workspace` | `%LOCALAPPDATA%/PixelBridgeOps` | 本地工作区（runs/state/日志） |
| `pollSeconds` / `heartbeatSeconds` | 2 / 2 | 轮询与心跳周期 |
| `commandTimeoutDefaultSeconds` / `commandTimeoutMaxSeconds` | 600 / 21600 | 命令超时缺省与上限 |
| `stdoutCapBytes` | 65536 | 结果内 stdout/stderr 各自上限 |
| `artifactDefaultCapBytes` / `artifactMaxCapBytes` | 512 MiB / 4 GiB | collect 缺省与硬上限 |
| `maxDeployFileBytes` / `maxZipEntryBytes` / `maxZipTotalBytes` / `maxZipEntries` | 16 GiB / 2 GiB / 16 GiB / 20000 | deploy 边界 |
| `runScriptTimeoutMaxSeconds` / `childMemoryCapBytes` | 3600 / 2 GiB | run-script 边界 |
| `stopGraceDefaultSeconds` | 15 | stop 每级宽限基准 |

CLI 覆盖：`--share-root` / `--workspace` / `--poll-seconds` / `--heartbeat-seconds`
/ `--config PATH`。单实例锁在 `<workspace>/state/listener-instance.json`。

## 测试

```text
python tools/PBRemoteOpsBridge/local/pbops.py selftest          # 冒烟（临时根，自动起停监听器）
python -X utf8 tests/tools/test_pbops_bridge.py                 # 完整协议/安全/回归测试
```

测试在本机临时目录上模拟远程侧（监听器支持任意根路径，这正是"路径不写死"的原因），
覆盖：往返、畸形/未知/重复命令、崩溃恢复、ZIP 攻击面、有界运行器、collect 上限、
截屏、显示器只读枚举等。两套测试均为独立手动脚本，不入 CTest（与本仓 tools 测试
惯例一致）。

## 已知边界

- 控制台 `q` 注入依赖目标进程有真实控制台（`console:true` 启动）；依赖
  AttachConsole，监听器自己的控制台（若有）会被释放——监听器只写文件日志。
- RDP/Citrix 虚拟显示适配器对 `display-set` 的支持不一，失败时如实返回
  `ChangeDisplaySettingsEx` 结果码；`--logical-fps` 始终可用。
- 共享根的原子性依赖 SMB 同卷 rename（SMB2 服务端语义），不要把 `staging/` 与目标
  目录拆到不同卷。
- 远程机上的控制台窗口（`console:true`）会出现在远程桌面；Fullscreen 数据窗口盖住
  主屏时一般无影响，Decoder 的 ROI 在本机端选取。
