# PixelBridge parser fuzzing

`PBProtocolBootstrapControlFuzz` sends every bounded input to the fixed 44-byte
PB-Bootstrap-1 parser, the variable PB-Control-1 parser, and the
PB-Control-Fragment-1 parser. A successful parse must reserialize byte-for-byte
to the same canonical input or the driver aborts. Raw mutation is supplemented
by structured modes that repair exact length and CRC-32C after mutating covered
semantic fields, plus bounded out-of-order/duplicate/conflict/quota/expiry
sequences through `ControlPlaneReceiver`. A separate structured mode admits one
valid SessionDescriptor and verifies that a second CRC-valid record with the
same key latches terminal `DescriptorConflict`. The input boundary is one byte
beyond the maximum fragment envelope; accepted complete Control records remain
capped at 65,536 bytes and accepted fragment payloads at 65,535 bytes.

`PBProtocolDescriptorResourceFuzz` exercises all three descriptor parsers,
Session admission/routing, Segment Map insertion, overlap/conflict paths, and
receiver resource limits. Each input creates only a fixed number of bounded
states and at most 16 generated bind operations.

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
than latest-wins.

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
```

Each successful replay reports `CORPUS_REPLAY_NO_CRASH`. Replay alone deliberately
does not claim that a valid seed was accepted or that a malformed seed produced
the expected error. `PBProtocolTests` independently asserts exact parsed fields,
post-CRC semantic errors, byte-for-byte reserialization, and boundary results;
the structured self-test asserts the state-machine outcomes. The dedicated
structured self-test executable is built from the same driver source and is
registered on every compiler backend. CTest also registers one no-crash replay
per corpus file on non-Clang fuzz builds.

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
```

Compiler/sanitizer availability is part of the evidence. A deterministic MSVC
mutation run must not be reported as coverage-guided libFuzzer or UBSan.
