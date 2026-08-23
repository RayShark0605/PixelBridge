# PBCompression zstd boundary corpus

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
