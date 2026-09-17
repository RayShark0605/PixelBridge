# Decoder memory budgets

[简体中文](DECODER_MEMORY_BUDGET.md) | **English**

## Purpose and UI

Optimize correctly completed non-local file time, not RAM utilization. Budgets affect local admission only, never wire/FEC bytes, sender scheduling, or digest/publication rules. An active decoder is retained per-segment state, not a CPU thread.

Gray Fast, PAM4, and Wide expose memory-budget reception in advanced options. Disabled: the original eight slots. Enabled: finite byte-budget admission. Settings freeze during reception; use the available-memory recommendation or explicit total/per-instance values.

| Setting | Default | Validity |
| --- | ---: | --- |
| Total FEC budget | 1024 MiB | Integer MiB, 4 MiB–1 TiB, plus host-headroom admission |
| Per-instance cap | 512 MiB | At least 1 MiB, no greater than total |
| Resume budget | 256 MiB | Custom mode: `floor(totalMiB/4) MiB` |
| Active instances | 8 | Performance mode removes this fixed gate, not byte limits or finite 65536-segment metadata bounds |

Invalid preferences, overflow, or per-instance > total are rejected or reset to visible safe defaults when loading GUI preferences, never silently clamped. Standard/formal measurement do not use this policy.

```text
--budget-bound-decoders --decoder-memory-mib 2048 --decoder-instance-memory-mib 512
```

Only supported live GrayFast/PAM4/Wide configurations accept the flags; they do not enable Replay/capture-only. Unspecified values retain defaults, so a total below 512 MiB needs an explicitly smaller per-instance cap too.

## Host planning

```text
resumeMiB = floor(totalMiB/4)
planningMiB = totalMiB + 4*resumeMiB + 512
available = min(available physical memory, available process commit)
```

Startup reserves another `max(512 MiB, available/10)`. The recommendation reserves `max(512 MiB, available/5)` before solving for a budget. Planning includes resume copies/compaction and capture margin; it is neither measured RSS nor a hard whole-process cap.

Budget selection does not allocate everything immediately. Other programs may create later pressure; this is not continuous memory-pressure control or a guarantee against paging. Windows `ullAvailPageFile` describes commit available to the process, not free disk space in a pagefile. [Microsoft MEMORYSTATUSEX](https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/ns-sysinfoapi-memorystatusex)

## Resume and compaction

Resume Open checks explicit configuration against actual total/per-instance/resume limits. A smaller current budget can reject an older larger resume state while preserving its files. Persisted input cannot grant itself larger budgets.

Default compaction remains. Explicit performance mode may coalesce compaction when live snapshots exceed 64 MiB and active state remains, only after pending data is committed. Reclaimable garbage triggers compaction at 75% of quota, or at least 16 MiB garbage occupying at least 25% of the physical log. Empty-active/small-live state still compacts promptly. Completed records remain immediately durable; quota and crash-consistency semantics do not weaken. [Persistence reference](DECODER_RESUMABLE_RECOVERY.md)

The **resume budget** is distinct from the new operational JSONL cap of 64 MiB. Never delete `.resume` as an ordinary diagnostic log.

## Does more memory help?

Compare deferrals, quota/OOM, active/peak counts, reserved/total/per-instance bytes, resume size, and real process memory. A budget increase is justified only by observed admission pressure; the final metric remains whole-file digest/publication/reopen time.

Removing the eight-slot gate improved a historical small-file comparison. That does not establish unlimited-memory speedups or gains from every budget doubling. When admission is already unconstrained, more RAM does not improve delivered pixels. Historical cleanup warnings and large-file plateaus remain in the [evidence index](EVIDENCE_INDEX.md).

Implementation: `decoder_memory_budget.*`, `decoder_resume_store.*`, `local_desktop_runtime.cpp`. Tests cover bounds, persistence-policy consistency, actual FEC allocation, and pixel-derived application recovery. Core policy is Qt-free; GUI configures and displays it.
