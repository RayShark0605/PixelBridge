# PB-Bootstrap-1 / PB-Control-1 corpus

These files are independently authored wire bytes. They are not emitted by
the PixelBridge serializer at test runtime.

- `valid-bootstrap.bin`: the fixed 44-byte PB-Bootstrap-1 Golden Vector;
- `valid-control-session.bin`: a 67-byte PB-Control-1 envelope containing the
  current 37-byte Phase-0 SessionDescriptor regression payload;
- `bad-bootstrap-crc.bin`: the Bootstrap Golden Vector with bit 0 of the final
  CRC byte inverted;
- `bad-control-crc.bin`: the Control Golden Vector with bit 0 of the final CRC
  byte inverted.

The Control envelope is canonical v1. Its embedded descriptor remains the
explicitly provisional Phase-0 payload and is not promoted to a formal v1
descriptor schema by this corpus.
