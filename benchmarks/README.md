# PBProtocol descriptor-state benchmark

`PBProtocolDescriptorStateBenchmark` measures the current reference
`DescriptorBindingState` implementation while inserting a complete map of
1-byte DirectRepeat Segments. It reports actual bounded PMR bytes, insertion
time, descriptors/second, and final map-validation time. The canonical
SessionTag is derived once before timing; the insertion interval covers
descriptor construction, budget accounting, and both map insertions, but not
repeated BLAKE3 tag derivation.

```powershell
cmake -S . -B build-bench -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake `
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
