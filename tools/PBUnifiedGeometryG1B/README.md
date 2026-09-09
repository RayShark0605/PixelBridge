# PBUnifiedGeometryG1B

**Read-only CPU edge instrumentation, not an admission candidate.** This directory is deliberately absent from the production CMake graph. It appends a diagnostic function to a hash-checked copy of the sealed locator, keeps every original function unchanged, and links the sealed production CPU oracle. No product/public header, profile, threshold, decoder entry or payload route is changed.

## Result and scope

The authorized G1B criterion audit is complete. Result: `CRITERION_INSUFFICIENT_NO_ADMISSION_CANDIDATE`. Admission candidate count is **zero**; G2/Step3/field work is not started.

- Same 25 G1 pixel fixtures and 36 recording observations; same baseline geometry, acceptance, pixel hashes and accepted-block hashes.
- Inspect all 48 canonical marker edges instead of stopping at the first non-sharp edge. Export crossing positions, adjacent-center levels, marker black/white and the original first unconstrained `FitAxis` result.
- On a canonical-size ROI, scale >= 1 plus full-canvas containment algebraically forces origin 0 / scale 1. This is a constraint on a hypothesis, NOT evidence that the pixels have that geometry.
- All four observable one-pixel translations have residual exactly 1 against this forced hypothesis, within the existing 1.25 residual budget, while the unchanged production path correctly rejects them. Therefore full-canvas constraints plus this residual budget are not sufficient for admission.
- The one-pixel photometric fixture has 47 sharp edges and one displaced midpoint. Every recorded observation has all 48 edges measured but zero edges satisfying the old exact sharp-endpoint proof. These observations define no new noise/error bound.

There is intentionally no alternative Bootstrap/FEC decoder, no publication, no Receiver, no output-file recovery, no screen/capture/GPU or field-goodput claim. Full replay and full CTest are not run. The existing `OfflinePixels` tool boundary and unknown original capture properties remain unchanged.

Detailed report: [G1B execution and next decision](../../docs/REMOTE_GEOMETRY_G1B_EXECUTION_2026-09-08.md).

## Files

| File | Purpose |
| --- | --- |
| `edge_probe.h`, `edge_probe_impl.inc` | Fixed-size, bounded, read-only edge measurements |
| `prepare_reference.py` | Verify the sealed locator source and append diagnostics in a new build; refuse different existing generated files |
| `CMakeLists.txt`, `main.cpp` | Standalone MSVC target, original baseline oracle, reused unchanged G1 fixtures and pinned recording reader |
| `run_checks.py` | Manual, create-only, no-window fixture/prefix execution |
| `analyze_results.py` | Bounded evidence parser, exact historical comparison, independent ordered OLS and counterexample proof |
| `test_tool_guards.py` | Ten tool failure/corrupt-evidence/no-overwrite checks |

The analyzer's `geometryHypothesisFitsExistingResidualBudget` is **not** a Bootstrap, frame, codeword or payload acceptance flag. The four geometry counterexamples disprove only that incomplete predicate; no alternative decoder was run on them. Do not describe this as a confirmed end-to-end false-accept vulnerability.

## Replay, only when explicitly requested

Final binary: `<repo>\build-geometry-g1b-20260908-run01\Release\PBUnifiedGeometryG1B.exe`.

SHA-256: `d6f145225c022d5925b2f07d673864d52851be2a882a39a297e641f217a8510f`.

Evidence root: `<repo>\artifacts\geometry-g1b-20260908-run01`. Use `RESULT_FINAL.json` and `FINAL_STATUS.json`, not intermediate or intentionally corrupt guard traces.

Before a new build, verify the source/library/recording identities recorded in the final manifest and start checkpoint. The generator checks the copied locator specifically; that check alone does not certify all linked libraries or the entire checkout. Stop if a referenced sealed input changed. Do not rebuild a historical directory.

```powershell
$repo = '<repo>'
$build = Join-Path $repo ('build-geometry-g1b-' + [Guid]::NewGuid().ToString('N'))
& 'C:\Program Files\CMake\bin\cmake.exe' -S "$repo\tools\PBUnifiedGeometryG1B" -B $build -G 'Visual Studio 17 2022' -A x64 -DPB_G1B_REPO=<repo> -DPB_G1B_BASE_BUILD=<repo>/build-remote-step2-20260908-run01 -DPB_G1B_FFMPEG=<repo>/artifacts/remote-step2-20260908-run01/deps/installed/x64-windows
if ($LASTEXITCODE -ne 0) { throw 'Configure failed; preserve evidence' }
& 'C:\Program Files\CMake\bin\cmake.exe' --build $build --config Release --target PBUnifiedGeometryG1B --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'Build failed; preserve evidence' }
```

To replay the final binary without rebuilding:

```powershell
$repo = '<repo>'
$build = "$repo\build-geometry-g1b-20260908-run01"
$newRoot = Join-Path $repo ('artifacts\geometry-g1b-replay-' + [Guid]::NewGuid().ToString('N'))
& '<python>' "$repo\tools\PBUnifiedGeometryG1B\run_checks.py" --repo $repo --build $build --output-new "$newRoot\fixtures"
if ($LASTEXITCODE -ne 0) { throw 'Fixture run failed; preserve evidence' }
& '<python>' "$repo\tools\PBUnifiedGeometryG1B\run_checks.py" --repo $repo --build $build --output-new "$newRoot\recording" --recording "$repo\30Hz_Remote.mkv"
if ($LASTEXITCODE -ne 0) { throw 'Prefix run failed; preserve evidence' }
& '<python>' "$repo\tools\PBUnifiedGeometryG1B\analyze_results.py" --repo $repo --fixtures "$newRoot\fixtures\fixtures.jsonl" --recording "$newRoot\recording\recording-prefix.jsonl" --output-new "$newRoot\RESULT.json"
if ($LASTEXITCODE -ne 0) { throw 'Evidence invalid; preserve failed result' }
```

Resources: native 512 MiB process Job, FFmpeg 32 MiB single-allocation cap and one software decode thread, at most 36 observations, 120-second outer timeout. Probe has fixed 48 slots and the original `LumaReader` work budget. Runner checks output <=1 MiB and stderr <=64 KiB; analyzer caps each file at 1 MiB, 128 records and 64 KiB/record. Missing measurements are null, not measured zero. Exit 0 means valid diagnostic evidence, not a promoted candidate or field PASS.

The source `.mkv`, all Step2/G1 builds and evidence, and `docs/PHASE1_GATE_REPORT.md` remain untouched. No automatic execution, scheduler, Git staging or commits are provided.
