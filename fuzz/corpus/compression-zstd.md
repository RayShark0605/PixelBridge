# PBCompression zstd boundary corpus

> **维护范围 / Scope (2026-09-17):** 本文保留该模块的协议/工具/测试参考，不再作为项目当前路线或发布状态入口。当前模式、单屏接收、预算、日志与完整文件证据见 [中文文档](../../docs/README.md) / [English documentation](../../docs/README.en.md)。历史日期、Gate、现场坐标与阶段参数仅适用于当时记录；不应直接复制到新环境。
> This is a module/tool/test reference, not the current release roadmap. Use the bilingual index for current behavior and validation boundaries; historical gates/settings are not universal defaults.

Each `.bin` file under `compression-zstd/` starts with the 24-byte control
record consumed by `PBCompressionZstdBoundaryFuzz`, followed by the encoded
Segment bytes:

| Offset | Meaning |
| --- | --- |
| 0 | codec selector |
| 1 | expected encoded-size selector |
| 2 | expected raw-size selector |
| 3 | maximum input-size selector |
| 4 | maximum output-size selector |
| 5 | maximum window-log selector |
| 6 | streaming partition selector |
| 7 | API call-sequence selector |
| 8..15 | little-endian raw-size hint / framing-margin input |
| 16..23 | little-endian explicit boundary value |

The valid frames were generated with the pinned zstd 1.5.7 dependency. They
use compression level 3 and a checksum. `unknown-fcs.bin` disables the frame
content-size field, while `wide-window.bin` has a 65,536-byte window and asks
the harness to enforce a 1,024-byte decoder window. The remaining seeds cover
truncation, checksum corruption, trailing or concatenated frames, non-standard
frame types, invalid magic, and quota boundaries.
