# PBBridge 开发编排入口 / Development orchestration

[中文文档](README.md) · [English documentation](README.en.md)

PBBridge 是测试工具，不是正式产品所需的文件传输通道。它通过每台远程机独立的共享命令树部署封存程序、执行有界命令、启动/查询本任务进程、收集日志及摘要元数据。真实 payload 仍只能由右屏等明确授权区域的可见像素进入 Decoder。

PBBridge is a development tool, not an additional product payload channel. A separate command tree per remote host supports deployment, bounded commands, owned-process orchestration, logs and digest metadata. Decoder still receives content only through actual captured pixels.

## 安全边界 / Boundaries

- 不通过桥把源文件、已解码块、像素或接收 ACK 输入 Decoder。
- 不注入鼠标键盘、不抢焦点；显示模式更改仅在当前任务明确授权时执行，并记录原状态/恢复结果。
- 不把“停止命令已发送”当作进程正常退出。Encoder 外部 WM_CLOSE 可能产生 Failed/exit1；应使用正确应用停止路径或自然有界运行期限，并验证最终退出。
- 每次新 run / 输出 / session，Receiver-first；不重用旧成功对象充当新测试。
- 跨机只关联 command/session/frame identity，不直接相减两台机器的 monotonic 时间。
- 对 PID/image/path 精确校验，不结束其它进程；不枚举无关账户或凭据。

No payload/pixels/ACK bypass; no input/focus manipulation; explicit authority for display changes with restoration; exact owned-process identity; fresh runs; receiver-first; no cross-host clock subtraction. A requested stop is not a verified clean exit.

## 操作规范 / Protocol and operation

实际命令 schema、原子写入、inbox/result/heartbeat、部署和 allowlist 设置见 [工具 README](../tools/PBRemoteOpsBridge/README.md) 与同目录实现。每个远程实例使用独立 root，避免多个 listener 抢同一个 inbox。公共文档不维护用户机器地址、实时会话号或共享凭据；当前部署应从当前任务授权配置只读核实。

See the tool README and implementation for exact schema, atomic command/result behavior, deployment and allowlists. Use one root per listener. Machine addresses, active sessions and shared credentials belong to local authorized configuration, not public documentation.

## 验证层级 / Evidence

桥部署成功≠图像传输成功。检查 Decoder 的完整 segment、quota/deferred/conflict、digest/publish/reopen，以及独立文件摘要；停止后核对没有遗留本任务进程、显示状态恢复、无输入操作。旧现场的失败、waiver 和环境差异见 [证据](EVIDENCE_INDEX.md)。

Successful deployment is not successful transfer. Verify final receiver acceptance and independent digest, process cleanup and restored display state. Preserve failures/waivers and environment differences.
