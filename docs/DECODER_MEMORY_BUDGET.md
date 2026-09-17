# Decoder 内存预算

**简体中文** | [English](DECODER_MEMORY_BUDGET.en.md)

## 目标

减少非本机整文件正确完成时间，而不是把 RAM 用满。配置只改变本机资源准入，不改变 wire、FEC 字节合同、发送调度或摘要/发布规则。活动解码器保存不同分段的恢复状态，不等于同样数量的线程。

## 界面设置

灰阶高速、PAM4、PAM4 Wide 的“高级选项”可开启“按内存预算接收”。不勾选保持原 8 名额；设置在开始前生效，运行中冻结。可点“按本机可用内存建议”，也可手动填写总预算和单实例上限。

| 参数 | 默认 | 规则 |
| --- | ---: | --- |
| FEC 总预算 | 1024 MiB | 整 MiB，4 MiB–1 TiB；另须通过实际主机余量检查 |
| 单实例上限 | 512 MiB | 至少 1 MiB，不大于总预算 |
| Resume 字节预算 | 256 MiB | 自定义时 `floor(totalMiB/4) MiB` |
| 活动名额 | 8 | 性能模式按字节预算准入，另有有限 65536 分段元数据上限，不是无限实例 |

无效偏好、溢出、单实例大于总预算会被拒绝或在读取界面偏好时关闭性能模式、恢复默认并提示，不静默夹紧。标准模式和正式 measurement 不启用该策略。CLI 追加：

```text
--budget-bound-decoders --decoder-memory-mib 2048 --decoder-instance-memory-mib 512
```

PAM4/Wide 与 GrayFast 仅在受支持的 live 配置启用；不是 Replay/capture-only 的通行证。只填一项时另一项仍用默认，因此总预算小于默认 512 MiB 单实例时需要同时设置合法单实例值。

## 主机规划

```text
resumeMiB   = floor(totalMiB/4)
planningMiB = totalMiB + 4*resumeMiB + 512
available   = min(当前可用物理内存, 当前进程可用 commit)
```

启动预检额外保留 `max(512 MiB, available/10)`；建议按钮保留 `max(512 MiB, available/5)` 后反推预算。规划包含 resume 的恢复/整理副本和捕获余量，不是 RSS 实测或整个进程的硬限制。

配置不会预分配全部预算。运行期其它程序抢占内存仍可能造成压力；当前不是持续内存压力控制器，不承诺永不换页。Win32 `ullAvailPageFile` 是当前进程还可提交的内存，不是分页文件磁盘空闲量。[Microsoft MEMORYSTATUSEX](https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/ns-sysinfoapi-memorystatusex)

## 恢复与持久化

Resume Open 必须核对显式配置和实际 FEC 总预算、单实例、日志预算一致。缩小预算使旧断点超限时拒绝打开并保留原文件，不暗中删除缓存或信任持久状态中的预算。

默认整理策略保留；显式性能模式在 live snapshot 大于 64 MiB 且 active 非空时允许合并整理，pending 必须已经提交。有可回收垃圾时，达到 quota 的 75%，或垃圾至少 16 MiB 且占物理日志至少 25% 时整理；active 清空/小 live 状态仍及时整理。completed 记录立即耐久、quota 和崩溃恢复规则不变。[恢复规范](DECODER_RESUMABLE_RECOVERY.md)

这里的 **resume 预算**与新增应用诊断 JSONL 的 64 MiB 上限完全不同，不能把 `.resume` 当普通日志删除。

## 怎样判断加内存有用？

对比同一 Session 的 `outerDeferredResourceBusyCount`、quota/OOM、active/peak、reserved/total/per-instance、resume 大小与实际进程内存。只有确实出现准入瓶颈时，加预算才有理由；最终仍看整个文件 digest/publish/reopen 完成时间。

历史解除固定 8 名额的小文件对照有明显收益，但没有证明预算无穷大或每次翻倍都能提速；资源已不紧张时，更大预算不会增加远控送来的有效像素。原来的 cleanup warning 和大文件平台期仍保留在 [证据索引](EVIDENCE_INDEX.md)。

源码与测试：`apps/common/decoder_memory_budget.*`、`decoder_resume_store.*`、`local_desktop_runtime.cpp`；`test_decoder_memory_budget.cpp`、`test_session_persistence.cpp`、PAM4 应用测试。核心 Qt-free；界面只做显式配置与展示。
