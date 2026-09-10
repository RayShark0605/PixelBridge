# PBRemoteOpsBridge 远程实验操作桥

> **文档性质：** 本文件是远程实验操作桥（PBRemoteOpsBridge）的**操作入口文档**，面向
> 后续接手的 AI 或人类操作者：桥是什么、如何运转、怎么用、出问题怎么排查。
> **wire 级协议细节、部署脚本与配置键的权威定义在
> [`tools/PBRemoteOpsBridge/README.md`](../tools/PBRemoteOpsBridge/README.md)**；本文件与
> 它冲突时，以该 README 与实现为准，并回写修正本文件。
>
> **本文件不承载项目级"当前状态"。** 项目能力边界见
> [`PROJECT_STATUS.md`](PROJECT_STATUS.md)；本文件只登记桥自身的部署与验证事实。

## 1. 这是什么、为什么存在

PixelBridge 的远程链路实验（G21、Step1 等历史流程）过去是**人工中继**：把封存 ZIP 手工
拷到远程机、手工解压、手工跑 `.bat`、人工在控制台按 Q 停止 Encoder、手工回传证据 JSON。
每个环节都需要用户在场，AI 无法自主完成实验循环。

本桥把这条人工链路替换为**基于 SMB 共享目录的文件协议**：

- **本机（RECEIVER-DESKTOP，控制端）** 把命令 JSON 原子写入共享目录；
- **远程实验机**（**任意**一台装有 Python 3.8+、能读写某个共享根的 Windows 机器——机制
  与具体机器无关，见第 2 节部署实例表）上常驻一个 Python 监听器轮询共享目录，执行命令
  （部署包、启动/停止 Encoder、切换显示器刷新率、截屏、采集证据、跑诊断脚本），并把
  结果与心跳原子写回共享目录；
- 本机直接读共享目录上的结果文件（共享就是本机磁盘 `J:`），完成闭环。

桥可以部署在**任意多台**远程机上：**每台机器一个独立的命令树（共享根）**，操作时用
`--root` 选择目标机器（多机部署规则见 §2，新增机器步骤见 §5.3）。

由此，"远程机跑 Encoder + 本机跑 Decoder"的整轮实验可以由 AI 全自动编排。

**两条不可逾越的红线**（违反即违规，详见第 7 节）：

1. 本通道**只做实验编排**：传递命令、实验包、脚本、日志与测量元数据；
   **绝不**向 Decoder 递送像素或 payload（AGENTS.md 第 1 节）。
2. **禁止跨主机时钟运算**：结果里的时间戳带产生它的机器后缀（`*AtRemote`/`*AtLocal`），
   跨主机关联只允许用命令 id，与项目测量纪律一致。

## 2. 部署实例与现场验证记录

桥的机制与具体机器无关；当前部署了哪些机器**只由下面的实例表登记**，不构成对桥的假设。

**多机规则（必须遵守）：一台远程机 = 一个独立共享根（命令树）。** 不要让两台监听器轮询
同一个 `inbox/`——取走虽然原子、不会重复执行，但**哪台机器抢到命令是不确定的**，会得到
不可预测来源的结果。新增机器按 §5.3 部署到自己的根（如 `<ShareRoot>-<机器或用途>`）。

### 2.1 当前部署实例

| 机器 | 共享根（本机路径 = 远程 UNC） | 监听器工作区 | 状态 |
| --- | --- | --- | --- |
| `SENDER-LAPTOP`（用户 <user>，Python 3.12.10） | `<ShareRoot>` = `\\<HOST>\<SHARE>\pbops` | `C:\Users\<user>\AppData\Local\PixelBridgeOps` | 已安装，Startup 自启；2026-09-10 全功能现场验证通过（见 2.2） |

增删机器时同步更新本表；随时可用 `beat` / `run ping` 核对某个根上的监听器身份
（`hostname` 应与表内一致）。

### 2.2 现场验证记录（机器：SENDER-LAPTOP，2026-09-10）

验证机显示器：`\\.\DISPLAY1`（Intel UHD），当时 2560×1440@240Hz，原生 2560×1600
模式可用（`display-set` 已实测可切换）。

| 验证项 | 结果 |
| --- | --- |
| 心跳 / ping | FRESH；远程身份回显正确；跨卷拓扑（共享=UNC，工作区=C 盘）正常 |
| display-info / display-set / display-restore | 枚举 52 模式；同值切换+恢复均 ok（**虚拟适配器接受刷新率切换**） |
| drop → deploy | 344 KB zip 哈希校验+安全解包 |
| run-script | 远程 PowerShell 5.1，exit 0，stdout 完整回传 |
| start + screenshot | 控制台进程启动存活；2560×1440 真实像素截屏回传 |
| **stop 优雅链** | **`method=console-q, forced=false, exitCode=0`** —— 控制台 Q 注入在真实远程机验证成功 |
| collect + pull | 证据文件带 SHA-256 清单回传，哈希核对一致 |
| cleanup | 远程 `runs/`、`files/`、`processed/` 清理干净，心跳仍 FRESH |

两点如实说明：

1. console-q 注入是用 `cmd /c pause` 替身验证的**机制级**验证；Encoder 的 `--manual-stop`
   用的是同一条 `ReadConsoleInputW` 控制台轮询路径，但**首次用于真机 Encoder 时应小规模
   复核一次**再当常规手段。
2. 2026-09-10 验证期间远程桌面出现过一次标题为 `ClientWebApiApplication`、内容为
   "DNS 名称不存在。"的 CMD 弹窗——经截屏取证与代码核对，系**远程机自身的程序**所弹，
   与本桥无关（桥与验证包内无任何 DNS/网络操作）。

## 3. 架构与机制

### 3.1 组件

| 组件 | 位置 | 实现 |
| --- | --- | --- |
| 监听器 `pbops_listener.py` | 远程机 `%LOCALAPPDATA%\PixelBridgeOps\listener\` | Python 3.8+，纯 stdlib + ctypes；单线程主循环 + 心跳线程；pythonw 无窗口常驻 |
| 本地助手 `pbops.py` | 本机仓库 `tools/PBRemoteOpsBridge/local/` | Python；`init/beat/send/await/run/drop/pull/selftest` |
| 共享目录树 | `<ShareRoot>`（远程视角 `\\<HOST>\<SHARE>\pbops`） | 双方唯一通信介质；协议目录见 3.2 |
| 部署脚本 `bootstrap/*.cmd` | 共享根 `bootstrap\` | Install（一次性）/ Start（手动补启）/ Uninstall |

所有路径**不写死**：监听器 `shareRoot` 为空且无 `--share-root` 时拒绝启动（fail closed）；
本地助手用 `--root` 或环境变量 `PBOPS_ROOT` 覆盖默认 `<ShareRoot>`。

### 3.2 共享目录树

```text
<ShareRoot>/
  inbox/       cmd-<UTC时间戳>Z-<6hex>.json   命令（本机→远程，原子写）
  processed/   监听器取走后移入（同卷原子 rename；先移动后执行）
  results/     res-<cmdId>.json + res-<cmdId>-artifacts/   结果（远程→本机）
  status/      heartbeat.json（每周期原子重写）+ 无
  files/       大文件投递区：<name> + <name>.sha256 + <name>.ready（就绪标记）
  staging/     共享侧原子写临时区
  bootstrap/   部署脚本（Install/Start/Uninstall .cmd）
  listener/    监听器源码与配置模板（部署母本）
```

要点：

- 一切写入走**临时文件 + rename**（服务端原子）；`files/` 投递以 `.ready` 标记就绪，
  防止读到半写文件。
- 命令文件名即身份：信封 `id` 必须等于文件名主干；只 pickup 匹配
  `cmd-\d{8}T\d{6}Z-[0-9a-f]{6}.json` 的文件。
- `results/` 与 `status/` 就在本机 `J:` 盘上，**本机可直接当本地文件读**，无需 SMB 客户端。

### 3.3 命令生命周期（原子性与崩溃一致性）

```text
本机: 构造信封 → 原子写 inbox/<id>.json
远程: 轮询 inbox → os.replace 到 processed/<id>.json（原子取走）→ 执行
    → 原子写 results/res-<id>.json → 下一轮
本机: await 轮询 results/res-<id>.json → 打印/退出码
```

崩溃一致性规则：

- **先移动后执行**：命令进入 `processed/` 而无对应 result，说明监听器中途死过；
  监听器重启时为这些命令补写 `status=interrupted` 的结果，本机不会永远等待。
- **重复 id**：第二个结果写为 `res-<id>.dup-<hex>.json` 且 `status=rejected`；
  `pbops.py await` 检测到 dup 会在 stderr 告警。
- **畸形输入 fail closed**：坏 JSON、未知信封键、未知 `schemaVersion`、未知类型 →
  `rejected` 结果；**监听器进程永不因单条命令退出**。
- **心跳**：默认每 2 秒原子重写 `heartbeat.json`（含 `bootId` 可识别重启、`busyWith`
  当前命令、`queueDepth`）；本机 `beat` 以 >10 秒未更新判为失联（退出码 0/1/2 =
  新鲜/过期/缺失）。
- 每条命令有超时（默认 600 s，上限 21600 s，信封 `timeoutSeconds`）；超时产生
  `status=timeout` 的结果。

结果封套：`status ∈ {ok, error, timeout, rejected, interrupted}`，含 `exitCode`、
`stdout`/`stderr`（各截断 64 KiB，截断事实在 payload）、`payload`、`error{code,message}`、
`startedAtRemote`/`finishedAtRemote`。完整字段定义见
[`tools/PBRemoteOpsBridge/README.md`](../tools/PBRemoteOpsBridge/README.md)。

### 3.4 命令集（schemaVersion 1，共 13 类）

| type | 关键 params | 语义 |
| --- | --- | --- |
| `ping` | — | 远程身份回显（主机/用户/pid/版本/shareRoot） |
| `display-info` | `monitor?` | 枚举显示器当前模式与全部可用模式（只读） |
| `display-set` | `monitor, width, height, refresh, bitsPerPixel?` | 先确认模式在枚举列表内，再动态切换（不写注册表）；前置模式入恢复栈 |
| `display-restore` | `monitor?` | 恢复最近一次 set 之前的模式 |
| `deploy` | `runId, file, sha256?` | 校验 `files/<file>` 哈希（param 或 `.sha256` sidecar）；`.zip` → 安全解包到 `<工作区>/runs/<runId>/package/`（拒绝穿越/绝对路径/保留名/symlink/reparse/目录项/重复项/加密项，逐项流式哈希，盘上复查 reparse）；其他文件 → 复制到 `runs/<runId>/input/`。**runId 只能新建** |
| `start` | `runId, exe, args[], console?=true, waitSeconds?=3, cwdSub?` | 启动 run 内 exe（`exe` 是 **run 目录相对路径**，zip 部署的在 `package/` 下）；`console:true` 独立控制台（`--manual-stop` 需要）；`console:false` 无窗口、stdout/stderr 落 run 的 `logs/`；登记 PID；存活检查后返回 pid |
| `stop` | `pid` / `runId` / `all:true`；`graceSeconds?=15`；`allowImageName?` | 三级降级链（见 3.5）；默认只碰监听器登记过的 PID |
| `run-script` | `runId, script, args?[], timeoutSeconds?`（≤3600） | Job-object 有界执行 run 内 `.ps1`（kill-on-close、2 GiB 内存上限、输出截断） |
| `collect` | `runId, patterns[], maxBytes?=512 MiB` | 匹配文件复制到 `results/res-<id>-artifacts/`，附 SHA-256 清单；超限**跳过并注明** |
| `screenshot` | `monitorIndex?=0` 或 `all:true` | GDI 物理像素截屏（DPI aware），PNG 入 artifacts |
| `list-runs` | — | run 清单 + 当前登记的存活进程 |
| `cleanup` | `processed/results/filesOlderThanDays?`、`runs?[]`、`maxItems?` | 有界清理，逐项报告；有存活登记进程的 run 拒删 |
| `listener-shutdown` | `confirm: true` | 监听器写完 ok 结果后干净退出（写终态心跳） |

### 3.5 停止语义（三级降级链，如实报告）

对每个目标 PID 依次尝试，任一级成功即止：

1. **控制台 Q 注入**（仅 `console:true` 启动的进程）：监听器 `AttachConsole` 到目标控制
   台，向其输入缓冲区注入 `q` 键按下事件——等价于人工在 Encoder `--manual-stop` 控制台
   按 Q。已通过真实远程机现场验证（`method=console-q, forced=false`）。
2. **WM_CLOSE**：向目标可见顶层窗口投递关闭消息（GUI 优雅停止路径）。
3. **强制终止**：`taskkill /T /F` 整树终止，结果里如实记 `forced=true`——**不冒充优雅
   退出**（G21 纪律：强杀只算 fail-fast，绝不算正常收尾）。

默认只允许停止**监听器登记过的 PID**；未登记 PID 必须显式传 `allowImageName` 才允许按
映像名匹配。`stop` 拒绝作用于监听器自身。

### 3.6 安全模型与边界

- **信任边界 = 共享目录写权限。** 能写 `<ShareRoot>` 的人本来就能对远程机投放任意程序，
  本桥不扩大实际权限；不要把共享开放给不可信账户。
- **部署即校验**：`deploy` 强制哈希校验 + ZIP 攻击面全量拒绝（复用 Step1 的
  `verify_zip`/`safe_relative` 规则族）；解包后逐项哈希入清单，盘上复查 reparse。
- **一切有界**：命令文件 ≤64 KiB、stdout/stderr 各 ≤64 KiB、collect 预算、脚本超时与
  内存上限、日志轮转；无无界队列/分配/重试。
- **监听器健壮性**：单实例锁；无 shareRoot 拒绝启动；单条命令异常只产生 `error` 结果，
  进程存活；崩溃后重启自动补 interrupted 结果。
- 本机侧注意：**控制端一律用 `<ShareRoot>` 路径**，不要用 `\\<HOST>\<SHARE>`（本机 SMB 回环被
  防火墙拦截，cmd/PowerShell 访问会报"找不到路径"；Git Bash 的 `//RECEIVER-DESKTOP/j` 表现不同，
  容易误导——统一用盘符路径最稳）。

## 4. 使用方法（控制端，本机）

### 4.1 前置与常用检查

本工作区使用的解释器与助手（Git Bash 路径写法）：

```bash
PY=/d/Python3/python.exe
BR=<repo>/tools/PBRemoteOpsBridge/local/pbops.py
$PY -X utf8 $BR beat      # 心跳：退出码 0=新鲜 1=过期 2=缺失
$PY -X utf8 $BR run ping  # 完整往返检查
```

- 默认根 `<ShareRoot>`（`--root` 或 `PBOPS_ROOT` 覆盖）。
- 首次接触先跑 `beat` + `run ping` 确认远程监听器活着；`beat` 输出里的 `hostname`
  应与 §2.1 实例表中的目标机器一致，`bootId` 变化表示监听器重启过。
- **部署了多台远程机时，必须显式 `--root` 选择目标**（助手默认 `<ShareRoot>` 只是实例表中
  第一台机器的根）。
- 结果文件是本机本地文件，可直接 `Read <ShareRoot>\results\res-<id>.json`。

### 4.2 本地助手子命令

```text
pbops.py [--root PATH] init                 # 创建共享协议目录树（已建过）
pbops.py [--root PATH] beat | status        # 心跳新鲜度
pbops.py [--root PATH] send <type> [--param KEY=VALUE ...] [--params-json JSON] [--timeout S]   # 只发，打印 cmdId
pbops.py [--root PATH] await <cmdId> [--timeout S]            # 等结果；退出码 0=ok 3=非ok
pbops.py [--root PATH] run <type> [...]     # send+await 一步到位
pbops.py [--root PATH] drop <localFile> [--name N]            # 大文件入 files/（复制+哈希+ready）
pbops.py [--root PATH] pull <cmdId> [--dest DIR]              # 拉 artifacts 到本地
pbops.py selftest                            # 临时根端到端冒烟（本机模拟，不碰 J:）
```

- `--param` 语法是 **`KEY=VALUE` 单参数**（如 `--param monitorIndex=0`）；
  复杂参数用 `--params-json '{...}'`。
- `drop` 是复制不是移动；`files/` 内同名已存在即拒绝（先 `cleanup` 或换名）。
- `await` 超时会以非零退出并打印消息；若出现 `.dup-*.json` 会在 stderr 告警。

### 4.3 远程 Encoder 实验配方

Decoder 始终在本机由控制端直接驱动（GUI 或 `--headless-receive` CLI），不经此桥；
桥只编排远程机侧。Encoder 参数契约见
[`CURRENT_RUNTIME_OPTION_INVENTORY.md`](CURRENT_RUNTIME_OPTION_INVENTORY.md)。

**配方 A（推荐，全自动）**：`--seconds N` 到时自动优雅收尾，`stop` 仅作异常中断兜底。

```bash
RUN=r$(date +%Y%m%d-%H%M)
$PY -X utf8 $BR drop PixelBridge-<封存包>.zip --name $RUN.zip
$PY -X utf8 $BR run deploy --params-json "{\"runId\":\"$RUN\",\"file\":\"$RUN.zip\"}"
$PY -X utf8 $BR run start --params-json '{
  "runId":"RUN", "exe":"package/Encoder/PixelBridgeEncoder.exe",
  "args":["--headless-broadcast","--source","package/payload.bin",
          "--profile","unified","--channel","remote","--remote-provider","UnknownRemoteLink",
          "--single-monitor-fullscreen","primary","--logical-fps","15",
          "--seconds","180",
          "--report","encoder-report.json","--journal","encoder-evidence.jsonl"],
  "console":false,"waitSeconds":5}'
# …本机跑 Decoder 接收；结束后：
$PY -X utf8 $BR run collect --params-json "{\"runId\":\"RUN\",\"patterns\":[\"*.json\",\"*.jsonl\"]}"
$PY -X utf8 $BR pull <collect的cmdId> --dest <本地证据目录>
$PY -X utf8 $BR run cleanup --params-json "{\"runs\":[\"RUN\"],\"filesOlderThanDays\":0}"
```

（示例中 `RUN` 记得替换为实际 runId。）`console:false` 时 Encoder stdout 进 run 的
`logs/`，可一并 collect。

**配方 B（人工停止语义复刻）**：`--loop --manual-stop`（要求
`--single-monitor-fullscreen`）+ **`console:true`** 启动；提前停止发
`stop --params-json '{"runId":"...","graceSeconds":15}'`，第一级即控制台 Q 注入——
等价于 G21 现场里"人在 Encoder 控制台按 Q"。注意 `console:false` 启动的进程没有控制台，
`stop` 只能走 WM_CLOSE/强杀。

**首次把配方 B 用于真机 Encoder 时**，先用短 `--seconds` 小规模复核 console-q 生效
（对照 stop 结果的 `method` 字段）。

### 4.4 刷新率实验

两层独立可组合：

- **远程显示器物理模式**：`display-info` 查模式 → `display-set {monitor,width,height,refresh}`
  → 实验后 `display-restore`。远程虚拟适配器已实测接受切换（2026-09-10）；失败时错误码
  如实返回。
- **Encoder 逻辑帧率**：启动参数 `--logical-fps 1..60`。

### 4.5 证据采集与清理

- `collect` 把 run 内匹配文件复制进 `results/res-<id>-artifacts/` 并附 SHA-256 清单；
  `pull` 拉回本地后应核对清单哈希。
- 截屏证据：`run screenshot --param all=true`（或 `--param monitorIndex=N`）。
- 实验收尾务必 `cleanup`（删 run、`files/` 残留、旧 processed），避免共享目录无限增长。
- 远程机当前状态随时可查：`run list-runs`。

## 5. 远程机侧管理

表中路径以 §2.1 首个部署实例的根 `<ShareRoot>` 为例；其它机器换成自己的根（见 §5.3）。

### 5.1 日常操作

| 操作 | 命令（在远程机上执行） |
| --- | --- |
| 安装（一次性） | `\\<HOST>\<SHARE>\pbops\bootstrap\Install-PBOpsListener.cmd \\<HOST>\<SHARE>\pbops` |
| 手动启动/补启监听器 | `\\<HOST>\<SHARE>\pbops\bootstrap\Start-PBOpsListener.cmd`（自动读部署配置；单实例锁防重复） |
| 卸载（停止 + 取消自启） | `\\<HOST>\<SHARE>\pbops\bootstrap\Uninstall-PBOpsListener.cmd`（`--purge` 连工作区与 run 证据一起删） |
| 更新监听器版本 | 把新 `pbops_listener.py` 覆盖到 `<ShareRoot>\listener\`，让远程机重跑 Install；或直接覆盖其工作区副本后按 §5.2 重启 |
| 换共享根/工作区 | 重跑 Install 传新路径（配置写在远程 `%LOCALAPPDATA%\PixelBridgeOps\listener\pbops_listener.deploy.json`） |

### 5.2 停止与启动监听器

**停止：**

| 场景 | 做法 |
| --- | --- |
| 临时停止（保留安装与自启）——**推荐经桥** | 本机执行：`pbops.py run listener-shutdown --params-json '{"confirm":true}'`（优雅退出：写完 ok 结果与终态心跳再退出） |
| 临时停止——在远程机上 | 任务管理器结束 `pythonw.exe`；或 `taskkill /PID <pid> /F`，pid 读远程机 `%LOCALAPPDATA%\PixelBridgeOps\state\listener-instance.json` |
| 只取消开机自启（不卸载） | 删除远程机 `shell:startup`（Win+R 输入）下的 `PBOpsListener.lnk` |
| 彻底停用 | 运行 Uninstall（停止 + 删自启快捷方式；`--purge` 再删工作区） |

**启动：**

| 场景 | 做法 |
| --- | --- |
| 手动启动 | 远程机运行 `\\<HOST>\<SHARE>\pbops\bootstrap\Start-PBOpsListener.cmd` |
| 自动启动 | Startup 快捷方式随登录自启；远程机重启/注销后再登录**无需手动** |

注意事项：

1. **监听器停着时无法经桥启动**——桥自身不可用就没有通信通道，必须在远程机本地执行
   Start（或重新登录触发自启）。这是文件协议的固有循环依赖，不是缺陷；所以"停止"随时
   可远程，"启动"必须本地。
2. 临时停止**不取消自启**：下次登录/重启监听器会自己回来；要真正停用必须 Uninstall 或
   删自启快捷方式。
3. 停/启后在本机验证：`beat`（心跳新鲜即已启动；`bootId` 变化 = 监听器重启过）或
   `run ping` 看回显 `hostname` 是否为目标机器。

### 5.3 新增一台远程机

每台远程机一个独立命令树，步骤：

1. 本机建新根并初始化协议目录：`pbops.py --root '<ShareRoot>-<机器或用途>' init`；
2. 从现有母本（如 `<ShareRoot>\`）把 `bootstrap\` 与 `listener\` 两个文件夹整体复制进新根
   （安装器按脚本自身位置解析监听器源码，每个根应自包含一份）；
3. 在新远程机上运行一次：
   `\\<HOST>\<SHARE>\pbops-<机器或用途>\bootstrap\Install-PBOpsListener.cmd \\<HOST>\<SHARE>\pbops-<机器或用途>`
4. 本机验证：`pbops.py --root '<ShareRoot>-<机器或用途>' beat` 与 `run ping`，确认 `hostname`
   是新机器；
5. 把新机器登记进 §2.1 实例表。

之后对该机的所有操作都带 `--root <ShareRoot>-<机器或用途>`（或临时 `PBOPS_ROOT`）。

## 6. 故障排查

| 症状 | 判断与处置 |
| --- | --- |
| `beat` 退出码 2（无心跳） | 监听器未运行：远程机重启后自启失败或被杀。需在远程机本地运行 `Start-PBOpsListener.cmd`（桥已死时无法经桥自愈，属预期） |
| `beat` 退出码 1（过期） | 监听器卡死或共享写入受阻：先 `run ping` 探活；长时间无响应则远程机本地重启监听器 |
| `await` 一直超时 | 查 `processed/` 有无该 cmdId：有 → 监听器死过，重启后应补 `interrupted` 结果；无 → 命令从未被取走（监听器早死了） |
| `drop` 报已存在 | `files/` 同名残留：`cleanup --params-json '{"filesOlderThanDays":0}'` 或换名 |
| `deploy` 报 `run-exists` | runId 已用：换新 runId，或 `cleanup` 删旧 run |
| `stop` 走到 `forced=true` | 目标没有控制台（`console:false` 启动）或没及时响应；优雅提前停止请用配方 B；强杀结果不能当正常收尾记入实验结论 |
| `start` 报 `process-died` | 启动参数/依赖问题：`collect` 拉 run 的 `logs/`（console:false 时）或 `screenshot` 看控制台输出（console:true 时） |
| `run-script` 超时 | Job-object 已强制终止（`status=timeout`）；检查脚本或加大 `timeoutSeconds`（≤3600） |
| 本机访问 `\\<HOST>\<SHARE>` 报路径不存在 | 正常现象（本机 SMB 回环被防火墙拦截）；控制端一律用 `<ShareRoot>` |
| `beat`/`ping` 回显的 hostname 与预期不符 | `--root` 指到了另一台机器的命令树：对照 §2.1 实例表换正确的根 |
| 远程桌面出现来路不明的弹窗 | 先截屏取证再判断；2026-09-10 曾出现远程机自身程序（`ClientWebApiApplication` 的 DNS 报错）弹窗，与桥无关 |

## 7. 纪律红线（任何使用者必须遵守）

1. **编排专用**：本通道只传递命令、实验包、脚本、日志与测量元数据；不得经它向 Decoder
   传送像素/payload，不得建立任何第二条 payload 通道（AGENTS.md 第 1 节）。
2. **不做输入自动化**：桥的 `q` 注入只作用于**自己启动的实验进程的控制台**（等价人工按
   Q），不触碰远程桌面的鼠标/键盘/其它窗口——沿用"不干扰现场、不做 UI 自动化"纪律。
3. **测量纪律不变**：跨主机不做时钟运算；吞吐真值仍以最终发布并 reopen 的整文件时间 +
   `VerifiedRawGoodput`/`VerifiedEncodedGoodput` 为准（见
   [`UNIFIED_TELEMETRY_REPORT.md`](UNIFIED_TELEMETRY_REPORT.md)）；桥的 `timeoutSeconds`
   等编排参数不得冒充测量计时。
4. **强杀不是收尾**：`forced=true` 的停止只算 fail-fast；实验结论必须基于优雅收尾的
   run（G21 豁免教训）。
5. **部署即校验**：远程只运行经 `deploy` 哈希校验过的包内文件；`allowImageName` 扩权
   仅限明确的清理场景并在结果里留痕。

## 8. 相关文件索引

| 文件 | 责任 |
| --- | --- |
| [`tools/PBRemoteOpsBridge/README.md`](../tools/PBRemoteOpsBridge/README.md) | wire 协议权威：信封 schema、配置键全表、ZIP 安全规则、部署细节 |
| `tools/PBRemoteOpsBridge/listener/pbops_listener.py` | 监听器实现（含各命令 handler） |
| `tools/PBRemoteOpsBridge/local/pbops.py` | 本地助手实现 |
| `tools/PBRemoteOpsBridge/bootstrap/*.cmd` | 安装/补启/卸载 |
| `tests/tools/test_pbops_bridge.py` | 31 用例回归（本机临时根模拟远程侧；改桥后必跑） |
| `tools/PBRemoteOpsBridge/local/pbops.py selftest` | 端到端冒烟 |
