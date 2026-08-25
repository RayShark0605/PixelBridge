# PixelBridge parser fuzzing

`PBProtocolBootstrapControlFuzz` sends every bounded input to the fixed 44-byte
PB-Bootstrap-1 parser, the variable PB-Control-1 parser, and the
PB-Control-Fragment-1 parser. A successful parse must reserialize byte-for-byte
to the same canonical input or the driver aborts. Raw mutation is supplemented
by structured modes that repair exact length and CRC-32C after mutating covered
semantic fields, plus bounded out-of-order/duplicate/conflict/quota/expiry
sequences through `ControlPlaneReceiver`. A separate structured mode admits one
valid SessionDescriptor and verifies that a second CRC-valid record with the
same key latches terminal `DescriptorConflict`. Another CRC-repairing mode drives
`FragmentCount` through `0/1/2/4096/65534/65535` and `TotalRecordBytes` through
`0/29/30/31/65535/65536/65537/UINT32_MAX`; it also sends sparse maximum-index
fragments through a receiver policy that permits the full wire count. The input
boundary is one byte beyond the maximum fragment envelope; accepted complete
Control records remain capped at 65,536 bytes and accepted fragment payloads at
65,535 bytes.

`PBProtocolDescriptorResourceFuzz` exercises all three descriptor parsers,
Session admission/routing, Segment Map insertion, overlap/conflict paths, and
receiver resource limits. Each input creates only a fixed number of bounded
states and at most 16 generated bind operations.

`PBProtocolOrphanResourceFuzz` exercises resume envelopes, output-reservation
boundaries, and the orphan cache as a state machine rather than merely checking
for crashes. A bounded reference model verifies every Admit/Drain/Clear result,
occupancy, idempotent duplicate, terminal conflict, and quota counter after each
operation. Its startup self-test fixes the full-cache duplicate/conflict and
same-padded-bytes/different-declared-length counterexamples. A valid resume
record is mutated only in its stored CRC, and the driver requires the result to
reach `CrcMismatch` while magic/version/length remain valid. Factory failure or
any semantic divergence aborts the process.

The fuzzer and the benchmark targets are mutually exclusive in one build tree:
the fuzz build instruments `PBProtocol` with AddressSanitizer, which breaks a
non-instrumented benchmark link. CMake rejects configuring both at once; use
separate build directories as shown below.

## MSVC x64 deterministic mutation + AddressSanitizer

```powershell
cmake -S . -B build-fuzz-msvc -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DPB_BUILD_APPS=OFF `
  -DPB_BUILD_FUZZERS=ON `
  -DPB_BUILD_BENCHMARKS=OFF
cmake --build build-fuzz-msvc --config RelWithDebInfo --target PBProtocolDescriptorResourceFuzz --parallel
.\build-fuzz-msvc\fuzz\RelWithDebInfo\PBProtocolDescriptorResourceFuzz.exe 100000 13464654573299691533
cmake --build build-fuzz-msvc --config RelWithDebInfo --target PBProtocolBootstrapControlFuzz PBProtocolBootstrapControlStructuredSelfTest --parallel
.\build-fuzz-msvc\fuzz\RelWithDebInfo\PBProtocolBootstrapControlFuzz.exe 100000 5783258900934164481
.\build-fuzz-msvc\fuzz\RelWithDebInfo\PBProtocolBootstrapControlStructuredSelfTest.exe --self-test
cmake --build build-fuzz-msvc --config RelWithDebInfo --target PBProtocolOrphanResourceFuzz --parallel
.\build-fuzz-msvc\fuzz\RelWithDebInfo\PBProtocolOrphanResourceFuzz.exe 100000 7263948150273648113
```

The two arguments are deterministic iteration count and PRNG seed. The final
line records both values so a run is replayable. A non-zero exit, sanitizer
diagnostic, or missing `FUZZ_COMPLETED` line is a failed run.

The mutation count is `0..8`, so exact valid seeds reach parser and state-
machine success paths instead of every generated descriptor being corrupted.
The self-test must print `STRUCTURED_SELF_TEST_COMPLETED`; it asserts that CRC-
valid version/type/flags failures, min/max/truncation/trailing boundaries, typed
admission, SessionTag rejection, no-fallback dispatch, out-of-order completion,
once-only submission, conflict, quota, expiry, and reset branches are genuinely
reachable. It also asserts that a typed descriptor conflict is terminal rather
than latest-wins, and that extreme fragment count/record-size metadata is
rejected or stored without dense index allocation as appropriate.

## Independent corpus replay and conformance

`fuzz/corpus/descriptor-resource` contains independently authored bytes rather
than files emitted at runtime by the serializer under test:

- `valid-session.bin`: SessionId `00..0F`, 64-byte file, 8 Segments;
- `valid-direct-segment.bin`: ordinal 0, raw range `[0, 8)`;
- `valid-final-manifest.bin`: matching fixed Session and zero digest;
- `overflow-direct-segment.bin`: `RawOffset=UINT64_MAX`, `RawSize=1`.

`fuzz/corpus/bootstrap-control` likewise contains independent canonical
Bootstrap/Control/Fragment bytes, CRC-corrupted variants, CRC-valid semantic
failures, empty/max Control boundaries, and small fuzz-harness sequence seeds.
The 67-byte Control seed contains the current provisional SessionDescriptor
regression payload; it freezes the envelope only, not the incomplete descriptor
schema. The three Fragment Golden files use payload sizes `24/24/19` and CRCs
`0x9DCD5402`, `0xF3FF94D8`, and `0x40106F66`.

`fuzz/corpus/orphan-resource` contains deterministic semantic sequences for
valid admission/drain, count-full and byte-full identical/conflicting
duplicates, equal padded bytes with different declared lengths, non-canonical
padding, an oversized orphan region, valid/corrupt resume records, and an exact
1,024-byte resume-policy boundary record.

Replay each seed through the bounded MSVC runner for a crash/sanitizer check:

```powershell
$runner = '.\build-fuzz-msvc\fuzz\RelWithDebInfo\PBProtocolDescriptorResourceFuzz.exe'
Get-ChildItem .\fuzz\corpus\descriptor-resource\*.bin | ForEach-Object {
  & $runner --input $_.FullName
  if ($LASTEXITCODE -ne 0) { throw "corpus replay failed: $($_.Name)" }
}
$bootstrapControlRunner = '.\build-fuzz-msvc\fuzz\RelWithDebInfo\PBProtocolBootstrapControlFuzz.exe'
Get-ChildItem .\fuzz\corpus\bootstrap-control\*.bin | ForEach-Object {
  & $bootstrapControlRunner --input $_.FullName
  if ($LASTEXITCODE -ne 0) { throw "corpus replay failed: $($_.Name)" }
}
$orphanRunner = '.\build-fuzz-msvc\fuzz\RelWithDebInfo\PBProtocolOrphanResourceFuzz.exe'
Get-ChildItem .\fuzz\corpus\orphan-resource\*.bin | ForEach-Object {
  & $orphanRunner --input $_.FullName
  if ($LASTEXITCODE -ne 0) { throw "orphan semantic replay failed: $($_.Name)" }
}
```

The descriptor and Bootstrap/Control drivers report
`CORPUS_REPLAY_NO_CRASH`; replay alone does not claim a specific semantic
outcome for those drivers. The orphan driver reports
`CORPUS_REPLAY_VALIDATED` only after its reference-model and boundary assertions
pass. `PBProtocolTests` independently asserts exact parsed fields, post-CRC
semantic errors, byte-for-byte reserialization, and boundary results. The
dedicated Bootstrap/Control structured self-test executable is built from the
same driver source and is registered on every compiler backend. CTest also
registers every pinned corpus file on non-Clang fuzz builds.

## Clang/libFuzzer + ASan/UBSan

When CMake identifies Clang, the same target uses `LLVMFuzzerTestOneInput` with
`-fsanitize=fuzzer,address,undefined`:

```powershell
.\build-fuzz-clang\fuzz\PBProtocolDescriptorResourceFuzz.exe `
  .\fuzz\corpus\descriptor-resource `
  -max_total_time=300 `
  -rss_limit_mb=1024 `
  -artifact_prefix=.\build-fuzz-clang\fuzz-artifacts\

.\build-fuzz-clang\fuzz\PBProtocolBootstrapControlFuzz.exe `
  .\fuzz\corpus\bootstrap-control `
  -max_total_time=300 `
  -rss_limit_mb=1024 `
  -artifact_prefix=.\build-fuzz-clang\bootstrap-control-artifacts\

.\build-fuzz-clang\fuzz\PBProtocolOrphanResourceFuzz.exe `
  .\fuzz\corpus\orphan-resource `
  -max_total_time=300 `
  -rss_limit_mb=1024 `
  -artifact_prefix=.\build-fuzz-clang\orphan-resource-artifacts\
```

Compiler/sanitizer availability is part of the evidence. A deterministic MSVC
mutation run must not be reported as coverage-guided libFuzzer or UBSan.

## Phase-0 focused parser harness

`PBParserFuzzHarness` 是聚合 build target，依赖既有 Bootstrap/Control、
Descriptor、Wirehair V2、ReferenceRaster drivers，以及以下三个 focused drivers：

- `PBProtocolTransportFuzz`：严格 Transport parse、accepted bytes 精确重序列化、
  info-block frame/extract round-trip、canonical padding 和失败无写入；输入上限
  65,571 bytes。
- `PBInterleaveReferenceFuzz`：56,168-byte region 的 apply/reverse、16-phase
  bijection、round-trip、错误大小/overlap failure atomicity；输入上限 56,169 bytes。
- `PBInnerFecCodewordFuzz`：三种 DVB-S2 Short profile syndrome；合法 2,025-byte
  codeword 执行 perfect-LLR decode 与 re-encode；Robust information prefix 继续
  尝试 canonical Transport extraction；输入上限 2,026 bytes。

`PBProtocolTransportStructuredSelfTest --self-test` 在所有 compiler backend 上
修复 header CRC 后分别触达 type/minor/flags/reserved/declared-length 语义分支，
并验证损坏 declared length 未修 CRC 时先返回 header CRC mismatch。成功必须输出
`TRANSPORT_STRUCTURED_SELF_TEST_COMPLETED`。

新增 corpus 目录：

- `fuzz/corpus/transport`：minimum/canonical/maximum、header/payload CRC、
  truncated/trailing、CRC-repaired semantic、zero/dirty info padding；
- `fuzz/corpus/interleave`：zero/logical/phase-7 physical 与 short/long size；
- `fuzz/corpus/ldpc`：三个 profile codeword、systematic/K-1/N-1 single-bit、
  parity segment flip 与 truncated codeword。

`PBVectorGen write-corpus` 直接返回命名 artifact，不依赖拼接后的手写 size table；
相同 seed 的目录必须逐文件 byte-identical。Transport semantic seeds 的 header CRC
只覆盖 `[0,28)`，确保 mutation 真正越过 CRC gate。

### MSVC deterministic runner + ASan

```powershell
cmake -S . -B build-phase0-tooling-asan -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows `
  -DBUILD_TESTING=ON -DPB_BUILD_TESTS=ON -DPB_BUILD_TOOLS=ON `
  -DPB_BUILD_APPS=OFF -DPB_BUILD_FUZZERS=ON -DPB_BUILD_BENCHMARKS=OFF

cmake --build build-phase0-tooling-asan --config RelWithDebInfo `
  --target PBParserFuzzHarness --parallel

ctest --test-dir build-phase0-tooling-asan --build-config RelWithDebInfo `
  -L parser-harness --output-on-failure
```

CTest 固定运行 Transport 100,000 次、Interleave 1,000 次、LDPC 1,000 次
deterministic mutation，并用 `--input` 重放全部三个新 corpus。MSVC 模式仅声明
deterministic mutation + ASan，不声明 coverage-guided libFuzzer 或 UBSan。

### Clang/libFuzzer + ASan/UBSan

Clang 配置对三个 focused driver 定义 `PB_USE_LIBFUZZER`，并把
`PBProtocol/PBInterleave/PBInnerFec` 及 driver 一起以
`-fsanitize=fuzzer,address,undefined`（structured self-test 不含 fuzzer main）
instrument。CTest 对每个新 corpus 注册 `-runs=2000` smoke。若当前环境未提供
可用 Clang/vcpkg toolchain，必须在验证报告中明确写为未运行，不能用 MSVC
deterministic 结果代替。

所有上述 parser smoke/corpus/self-test 统一带 `parser-harness` CTest label。
