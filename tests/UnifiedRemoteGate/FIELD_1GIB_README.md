# G21 真实远控 15 Hz / 1 GiB 手动循环 Sender + 6 小时 Receiver 单次复验

> **维护范围 / Scope (2026-09-17):** 本文保留该模块的协议/工具/测试参考，不再作为项目当前路线或发布状态入口。当前模式、单屏接收、预算、日志与完整文件证据见 [中文文档](../../docs/README.md) / [English documentation](../../docs/README.en.md)。历史日期、Gate、现场坐标与阶段参数仅适用于当时记录；不应直接复制到新环境。
> This is a module/tool/test reference, not the current release roadmap. Use the bilingual index for current behavior and validation boundaries; historical gates/settings are not universal defaults.

本入口按用户 2026-09-07 的明确决定，复用已经通过本机右屏 64 MiB、500 MiB、1 GiB 阶梯的冻结产品候选
`6e9064319a51bedcd403d74d47dd45c067928013`，跳过新的远控 smoke/64 MiB，直接执行一次 1 GiB。
这不会改变 PB-Unified-SC6-V3/layout 10、FEC、产品 admission、摘要、coverage、真实资源拒绝门或
16 KiB/unique 硬门；只把验证报告中的“最终可恢复完成”和“严格 Pass-0 零背压”拆开记录。

## 运行顺序

1. 将 RemoteEncoder ZIP **完整解压**到远程机的新短路径；`.bat` 不能脱离同目录的 PowerShell、`expected-build.json`
   和完整 `bin` 依赖单独使用。
2. 远程机先运行 `00_Check.bat`。它只核对冻结 Encoder 的 SHA-256、加载和 build identity，不生成 source，
   不显示发送窗口。检查通过后不要启动正式脚本，等待本机 Receiver 已启动的明确通知。
3. 本机操作员从 LocalReceiver 包运行 `Start-G21Receiver.ps1 -Stage 1gib6h`。它先执行只读 monitor preflight，
   再仅捕获完整右侧物理屏幕；不接收 source、摘要、SessionId 或 sender report，不控制鼠标键盘。
4. 收到本机操作员通知后，远程机运行 `04_Start_Full_1GiB_Manual.bat`。保持 Encoder 的完整无边框画布持续显示在
   本机右侧屏幕的远控画面中；禁止最小化、遮挡、裁切、缩放切换或冻结远控画面。
5. 本机 Receiver 报告 Gate 成功后，远程机聚焦 Encoder console，按 `Q` 或 Enter 正常停止。不得用任务管理器
   强杀。停止后等待独立 source 双摘要完成。
6. 只回传该次 `runs\1gib-<GUID>` 中的 `source-manifest.json`、`source-poststop-digests.json`、
   `encoder-report.json`、`process-exit.json`；不要回传 `.bin` source，也不要在 Receiver 运行时提供任何 oracle。

## 固定参数与成功条件

- source：远程机 OS CSPRNG，精确 `1,073,741,824` bytes，1 MiB 有界 buffer；每次 fresh GUID/source/Session。
- source lease：广播前开始以 `FileShare.Read` 只读持有，直到 Encoder 退出。
- Profile：`PB-Unified-SC6-V3`，layout 10，1920x1080 canonical canvas。
- Sender：primary monitor 无边框全屏，15 Hz，`--manual-stop --loop`，无自动 deadline；仅在操作员按 Enter/Q 后停止。
- Receiver：完整右侧物理屏幕，21,600 秒硬上限，1 秒内存采样；左屏受保护且无输入自动化。
- 两端必须正常 exit 0；timeout、强杀、coverage fail-fast、真实 resource rejection、conflict、orphan 丢弃/耗尽都失败。
- 配对且单调的 `DeferredResourceBusy`/`OuterFecQuotaExceeded` 可以继续等待 Carousel：必须证明历史峰值达到
  声明的 8-slot active-decoder window，且当前/峰值计数及预留字节从未越过声明上限；缺字段、不配对、回退或
  越界仍立即失败。最终报告必须同时给出 `eventualRecoveryPassed` 和 `strictPass0ZeroPressurePassed`，后者在
  busy/quota 非零时必须保持 false。
- Receiver 必须 128/128 Segment、WholeFileDigest、安全发布、final reopen、frameCoverageComplete 全部成功，
  无 `.part/.resume` 残留，最终 `VerifiedEncodedBytesPerUniqueFrame >= 16,384`。
- Receiver 终止后才独立比对双方 exact bytes、SHA-256、BLAKE3、Session 和 build identity。
- 独立 live false-accepted codeword oracle 因未向 Receiver 提供 sender truth 而保持 `null`；不得伪造为 0。

本入口是 G21 现场验证夹具，不是 G22 Release、SBOM 或安装包。

## 为什么不是原 7200 秒

首轮 RDP 运行已证明第二个 8-Segment window 出现配对的有界 busy，Pass-0 后半段因此没有被 Receiver 保留。
按冻结调度，Pass-0 每段 `7056` equations，后续 FullRepair 每段 `6385 + 1277 = 7662` fresh repair equations；
即使假定每帧 14 个 Transport slot 全部可用，128 段走到下一轮末尾也至少需要
`(64,512 + 70,144) / 15 ≈ 8,977` 秒，严格超过旧的 7,200 秒。用户明确要求 Encoder 不限时、由人工停止；
对应产品路径是省略 `--seconds` 并使用 `--manual-stop --loop`，不是把 `0` 猜成无限。Decoder 的 21,600 秒仍是
独立有界验证期限。这些变化不改变发送速率、字节数、FEC、活动窗口、资源预算或成功条件。

## 2026-09-07 现场终态

本入口已在 Windows 远程桌面场景执行。唯一 Session `daba04b1c7c8c22a31604ea68dd61f8f` 完成精确
1,073,741,824 bytes、128/128 Segment、whole digest、安全发布、final reopen、外部 SHA-256/BLAKE3 和清理；
Sender 由用户按 Q/Enter 正常停止并 exit 0。配对有界 busy/quota 为 970,220/970,220，后续 Carousel 最终完成，
`eventualRecoveryPassed=true`、`strictPass0ZeroPressurePassed=false`，真实 resource/conflict/orphan/lane failure 为 0。

原始性能结果保持 FAIL：124,470 unique frames、13.753410 Hz、8,626.511 B/unique，Receiver Gate 因第36行的
16 KiB硬门 exit 1。用户在完整披露后明确选择“仅本次明确豁免”，所以该唯一 Run 以
`PASS_WITH_SINGLE_RUN_USER_WAIVER` 关闭 G21；这不修改本文件第36行的未来成功条件，不改原始 evidence、分母、
协议或产品代码。完整结果见 `docs/UNIFIED_G21_REMOTE_1GIB_RESULT_2026-09-07.md`。
