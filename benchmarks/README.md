# PBProtocol descriptor-state benchmark

> **维护范围 / Scope (2026-09-17):** 本文保留该模块的协议/工具/测试参考，不再作为项目当前路线或发布状态入口。当前模式、单屏接收、预算、日志与完整文件证据见 [中文文档](../docs/README.md) / [English documentation](../docs/README.en.md)。历史日期、Gate、现场坐标与阶段参数仅适用于当时记录；不应直接复制到新环境。
> This is a module/tool/test reference, not the current release roadmap. Use the bilingual index for current behavior and validation boundaries; historical gates/settings are not universal defaults.

`PBProtocolDescriptorStateBenchmark` measures the current reference
`DescriptorBindingState` implementation while inserting a complete map of
1-byte DirectRepeat Segments. It reports actual bounded PMR bytes, insertion
time, descriptors/second, and final map-validation time. The canonical
SessionTag is derived once before timing; the insertion interval covers
descriptor construction, budget accounting, and both map insertions, but not
repeated BLAKE3 tag derivation.

The benchmark and the fuzzer targets are mutually exclusive in one build tree
(the fuzz build instruments `PBProtocol` with AddressSanitizer); CMake rejects
configuring both at once.

```powershell
cmake -S . -B build-bench -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake `
  -DBUILD_TESTING=OFF `
  -DPB_BUILD_TESTS=OFF `
  -DPB_BUILD_APPS=OFF `
  -DPB_BUILD_BENCHMARKS=ON
cmake --build build-bench --config Release --target PBProtocolDescriptorStateBenchmark --parallel
.\build-bench\benchmarks\Release\PBProtocolDescriptorStateBenchmark.exe 65536
```

The optional count is restricted to `1..65536`. This benchmark characterizes
descriptor-state cost only; it is not a `VerifiedEncodedGoodput`, capture, FEC,
or certified backend performance claim.
