# PB-Bootstrap-1 / PB-Control-1 / PB-Control-Fragment-1 corpus

The wire files are independently authored bytes and are not emitted by the
PixelBridge serializer at test runtime. The explicitly named structured files
are fuzz-harness operation sequences rather than wire records.

- `valid-bootstrap.bin`: the fixed 44-byte PB-Bootstrap-1 Golden Vector;
- `valid-control-session.bin`: a 67-byte PB-Control-1 envelope containing the
  current 37-byte Phase-0 SessionDescriptor regression payload;
- `bad-bootstrap-crc.bin`: the Bootstrap Golden Vector with bit 0 of the final
  CRC byte inverted;
- `bad-control-crc.bin`: the Control Golden Vector with bit 0 of the final CRC
  byte inverted;
- `valid-fragment-{0,1,2}.bin`: the frozen three-fragment Golden Vector for
  `ControlRecordId=0x0102030405060708`, with payload sizes `24/24/19`, serialized
  sizes `48/48/43`, and CRC-32C values `0x9DCD5402`, `0xF3FF94D8`, and
  `0x40106F66`;
- `semantic-bootstrap-version.bin`, `semantic-control-type.bin`, and
  `semantic-fragment-flags.bin`: CRC-valid inputs that prove post-checksum
  semantic validation remains reachable;
- `valid-control-empty.bin` and `valid-control-maximum.bin`: exact 30-byte and
  65,536-byte PB-Control-1 envelope boundaries;
- `structured-out-of-order.bin`, `structured-duplicate.bin`,
  `structured-conflict.bin`, `structured-descriptor-conflict.bin`,
  `structured-extreme-metadata.bin`, and `structured-quota-expiry.bin`: tiny
  fuzz-only operation sequences. They are
  harness controls, not protocol wire records. The descriptor-conflict mode
  admits one valid SessionDescriptor and requires a second CRC-valid record
  with the same descriptor key to latch terminal `DescriptorConflict`. The
  extreme-metadata mode repairs CRC while exercising the `uint16` fragment-count
  ceiling, exact 65,536-byte record limit, `UINT32_MAX`, and sparse maximum
  index through the bounded receiver.

The Control envelope is canonical v1. Its embedded descriptor remains the
explicitly provisional Phase-0 payload and is not promoted to a formal v1
descriptor schema by this corpus.
