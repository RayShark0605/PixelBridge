# DXGI / CaptureNormalize / LocalDesktop Bootstrap implementation record

## Baseline and scope

- Repository baseline: `db47dd4` (`master`). The worktree was clean before implementation.
- Read `AGENTS.md` and the directly relevant capture, epoch, Bootstrap, LocalDesktop and telemetry sections of `PixelBridge_最终技术路线与总体设计.md`.
- Implementation order: DXGI and the shared capture core; strict normalization and asynchronous diagnostic readback; then the experimental LocalDesktop Bootstrap mapping and diagnostic CLIs.
- Existing `PB-ReferenceRaster-1`, the canonical 44-byte Bootstrap record, protocol/FEC recovery semantics and old Golden bytes remain compatibility requirements.
- No display-mode, HDR, orientation or desktop switching is performed by the default tests. Desktop pixel tests are opt-in, serialized and use the `PixelBridgeDesktop` CTest resource lock.
- No automatic WGC fallback, cursor repair, stale-pixel filling, implicit tone mapping or payload side channel is part of this work.

## Shared capture implementation

`PBCaptureNormalize/src` is the sole owner/inbox/OS-frame-lease/owned-ROI-ring implementation. `PBScreenCaptureWgc` retains its raw public API through a thin facade; DXGI uses the same runtime. Source-copy completion and consumer-work completion remain separate retirement points. A COM reference to a borrowed texture does not grant permission to use a recycled ring slot.

The raw surface extent, visual ContentSize, physical ROI, display rotation and source-to-visual transform are separate values. WGC's upright images have an identity source transform. DXGI rotation uses integer GPU loads from an explicitly budgeted PB-owned scratch surface, never assumes that the acquisition supports SRV binding, and never assembles dirty/move rectangles from multiple observations.

Native references used during implementation:

- [AcquireNextFrame, including finite waits and pointer-only updates](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgioutputduplication-acquirenextframe)
- [Desktop Duplication rotation and separate/composited pointer semantics](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/desktop-dup-api)
- [DuplicateOutput1 scan-out formats and environment recovery](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_5/nf-dxgi1_5-idxgioutput5-duplicateoutput1)

## Validation journal

Logs are generated, ignored build artifacts; the commands below are repository-root commands. A successful pre-change baseline is not evidence for the new features.

### Phase 1, first shared-core build

```powershell
cmake --build build-wgc-release --config Release --parallel 4
ctest --test-dir build-wgc-release -C Release -R '^(PBScreenCaptureWgcTests|PBWgcD3dTests|PBWgcCapabilitiesTests|PBWgcRejectsNonPmv2Host|PBWgcNativeTests|PBCaptureRotationTests)$' --output-on-failure
```

- Default Release build: PASS. Log: `build-wgc-release/stage1-build.log`.
- WGC state-machine, WARP, capability and PMv2 tests, and the new four-way rotation suite: 5/5 PASS.
- Native WGC: FAIL at the unchanged `REQUIRE(onTop)` fixture precondition in both cases. The visible window at the ROI was class `SpaceEngine`, not the renderer's data window. This is not counted as a passed capture test and the assertion was not relaxed. User notification requested temporarily removing the occluder.
- Full selected run: 5/6 PASS. Log: `build-wgc-release/stage1-capture-tests.log`.

### Phase 1, initial DXGI compilation and timestamps

```powershell
cmake --build build-wgc-release --config Release --parallel 4
ctest --test-dir build-wgc-release -C Release -R '^PBCaptureNormalizeTests$' --output-on-failure
```

- Default Release build including the initial DXGI sources: PASS. Log: `build-wgc-release/stage1-dxgi-build.log`.
- Timestamp suite: PASS, including 4096 deterministic comparisons against an independent 128-bit multiply/divide oracle and signed-limit/failure-immutability cases.

## Review ledger

| Finding | Treatment | Required regression |
|---|---|---|
| Output/adapter enumeration rejected exactly 64 entries without reading the terminating sentinel | In-scope correction | 63/64/65 entries through the real shared resolver; failure leaves outputs unchanged |
| Deferred native retirement helper permitted implicit copies of OS handles | In-scope ownership hardening | Static non-copy/non-move assertions and matching immediate-context/device validation |
| Slot-index delivery can present a newer observation before an older one | In-scope bounded-stream correction | Controlled slot reuse with an older source copy completing later |
| Waiting/rebuild must invalidate CPU results even without a new device/epoch-start callback | Phase 2 mandatory barrier | Old running CPU job finishes after invalidation and cannot publish |
| Device recovery cleared a cancelled consumer failure, or a first device Close error hid a later non-device Close error | Preserve the first non-device failure; bounded two-CAS Close latch | Return/throw completion matrix and two outstanding source leases with ordered Close failures |
| A stale Analyze exception could stop the replacement domain | Check original domain/revision before accepting either success or failure | Late old-domain return/throw is discarded; a new-domain job still commits |

### Phase 1 full Release checkpoint

```powershell
cmake -S . -B build-wgc-release -DPB_BUILD_DXGI_GATE=ON
cmake --build build-wgc-release --config Release --parallel 4
ctest --test-dir build-wgc-release -C Release --parallel 4 --output-on-failure
```

- Full default build: PASS (`stage1-final-build.log`).
- Full CTest: **80/82 PASS** (`stage1-full-ctest.log`). Both `PBWgcNativeTests` and `PBDxgiNativeTests` fail at the unchanged visible-window precondition, reporting `occluder-class=SpaceEngine`. This checkpoint does not establish native pixel capture correctness.
- Four-way WARP rotation includes independent byte oracles for BGRA8, R10G10B10A2 and finite FP16 values, signed desktop origins, non-square/odd ROI, and acquisition textures without SRV bindings.

### Phase 2 normalization and diagnostic readback checkpoint

```powershell
cmake --build build-wgc-release --config Release --parallel 4
build-wgc-release/tests/PBScreenCaptureDxgi/Release/PBDxgiNativeTests.exe '[dxgi-environment]'
ctest --test-dir build-wgc-release -C Release --parallel 4 -R '^(PBCaptureNormalizeTests|PBCaptureRotationTests|PBScreenCaptureDxgiTests|PBScreenCaptureWgcTests|PBWgcD3dTests|PBWgcCapabilitiesTests)$' --output-on-failure
```

- Default builds: PASS (`stage2-core-build.log`, `stage2-contract-build.log`, `stage2-readback-build.log`).
- Selected unit/WARP suites: **6/6 PASS**, including strict normalized metadata, cursor proof/erasure, QPC provenance, independent CPU byte readback, two-marker cancellation, bounded queue/buffer ownership, stale success/failure admission, expiration, and nonblocking destructor (`stage2-readback-gate.log`).
- Real native environment notification/retry/teardown test: **1 case / 7314 assertions PASS** (`stage2-native-environment.log`). It uses a deliberately mismatched selected monitor identity, not fake recovery decisions; it never captures user pixels or modifies the desktop. Initial three attempts stop; explicit recreate cannot reset an exhausted budget; a message sent only to this test instance's hidden observer starts one new bounded attempt series. Source leases/copies/frames remain zero and epoch 17 does not churn while unavailable.
- The diagnostic path is explicitly `DiagnosticCpuReadback`: three independent CPU buffers, one latest-wins queued job, preallocated staging, owner-only nonblocking Map after the consumer marker. The CPU worker retains no device/context/texture/OS acquisition; request-stop/destruction never joins it on capture/deferred cleanup. Applications must use the finite explicit `Stop` and report a timeout rather than creating replacement workers in a loop.
- CPU `Analyze` builds a candidate only. `Commit` and domain invalidation share a short admission mutex; `Reset` and `Discard` run on the CPU worker. Capture/session/FEC protocol state is not reset by this visual barrier.
- Follow-up public-facade/real-WARP/normalizer/readback integration: **7/7 PASS** (`stage2-pipeline-build.log`, `stage2-pipeline-gate.log`). Both WGC and DXGI factories exercise the same owner/ring and actual WARP copies, independent expected bytes, a static final-frame Poll, in-flight epoch invalidation and source/consumer retirement.
- The separate MSVC ASan default build and selected capture suites also passed at this checkpoint (`build-wgc-asan/stage2-build.log`, `stage2-capture-gate.log`, 6/6). These are earlier checks, not substitutes for the final post-Bootstrap ASan run.

## Normalized contract and diagnostic lifetime

- `WgcCapture::Create`/`RoiConsumer` remain the raw ROI entry with its existing errors. `WgcCapture::CreateNormalized` and `DxgiCapture::Create` are strict entries into the same `ScreenCaptureConsumer` contract. Strict delivery never falls back to raw delivery.
- `ScreenCaptureFrame` exposes only a PB-owned, callback-borrowed texture, plus the signed physical ROI, upright ROI extent, visual ContentSize, raw extent, both transforms, actual formats/color encoding/bit depth, LUID, cursor proof, original timestamp domain/frequency, capture observation and source/slot generations.
- The stream domain contains an OS-random 128-bit source identity plus `captureEpoch`. Reopening another source with local epoch 1 cannot reuse the old source's temporal state.
- The owner publishes/invalidate domains and cancels queues. CPU Analyze is outside the admission lock; its candidate retains its original domain/revision. Only a matching, current, fresh result can Commit. Worker Reset clears geometry, calibration, duplicate/session history and pending events before processing a replacement domain; a late old success or exception cannot contaminate the replacement.
- No LLR accumulator or cross-observation combine is implemented. The actual duplicate/retry admission checks full canonical Bootstrap identity and geometry/calibration generations; it does not call `ReceiverIngress::ResetCaptureEpoch` or erase verified protocol/FEC state.
- Identity crop is GPU copy. Rotated crop uses same-format owned scratch and integer-coordinate GPU loads; no whole-frame resize or GPU→CPU→GPU fast-path conversion is introduced.
- Readback reserves three staging textures and three separately leased CPU buffers, with one latest-wins pending CPU job. Submit only copies. The ordinary owner Poll reaches the second marker and performs `Map(DO_NOT_WAIT)` even if no later Submit occurs. Actual RowPitch is checked and copied to a packed CPU buffer; a busy worker's buffer cannot be reused. Cancelled/deferred callbacks never Map.
- GPU loss during staging creation or Map invalidates that domain and returns the original device-loss status to finite capture recovery, but does not permanently terminate the reusable CPU worker. Explicit Stop is sticky; non-device graphics errors and invalid mapped layouts remain terminal.
- DXGI admission reserves the largest advertised candidate (8 B/pixel), rotation scratch and 256 KiB pointer storage before creating duplication resources. The actual descriptor/ring still records and checks actual 4B/8B allocation. Budgets never remove a high-depth format to disguise a precision fallback.

## Encoder LocalDesktop Bootstrap diagnostic entry

The old no-argument banner is unchanged. `--data-window` without a visual option continues to generate the existing `PB-ReferenceRaster-1` bytes and binding. Only `--visual local-desktop-bootstrap` selects the experimental `PB-LocalDesktopBootstrap-X1` mapping (`VisualProfileId=0x50424C4442533031`, layout 2). This explicit option may enable the data window by itself or coexist with one `--data-window` in either order.

```powershell
# Existing reference mode, unchanged; each telemetry destination must be new.
.\build-wgc-release\apps\PixelBridgeEncoder\Release\PixelBridgeEncoder.exe --data-window --frames 12 --telemetry .\reference-new.jsonl

# Experimental SDR Bootstrap-only visual; no Control/Data payload is claimed.
.\build-wgc-release\apps\PixelBridgeEncoder\Release\PixelBridgeEncoder.exe --visual local-desktop-bootstrap --frames 120 --telemetry .\bootstrap-new.jsonl
```

Both modes use the same existing D3D data window, submission queue, presentation pacing and telemetry writer. The process generates a fresh SessionId using the OS CSPRNG and derives SessionTag through the protocol API; each accepted CPU submission advances FrameSequence. The selected raster is rebuilt in full from a canonically serialized 44-byte `PB-Bootstrap-1` record. The new mapping emits only 1920x1080 BGRA8 grayscale pixels, repeats the complete record in two separated areas and includes the new mapping's timing/locating patterns; it is not an HDR conversion or a certified payload profile.

`--frames N` still limits accepted CPU submissions (1..1000000), not observed visual frames. Omitting it keeps the existing Escape-to-stop behavior. `--telemetry` still uses create-new semantics, a 16 MiB bound and explicit flush/close; existing files are never overwritten. No-argument, old reference and new visual modes do not share mutable parser state. Unknown/missing/duplicate visual options and repeated singleton options are rejected before renderer or telemetry-file creation. The Decoder is not given the generated SessionTag, record or pixels through an auxiliary transport.

`tests/PresentationGate/test_encoder_arguments.cpp` exercises the production allocation-free argument parser without opening windows. `VerifyEncoderLocalDesktopEntry.cmake` is an opt-in, desktop-serialized native entry gate for both new option spellings, finite submission accounting, the unchanged data-window contract, Unicode telemetry and immutable overwrite rejection. That entry gate does **not** claim captured Bootstrap decoding; actual pixel decoding remains a separate capture gate.

## Decoder LocalDesktop Bootstrap diagnostic entry

```powershell
# Select a physical single-monitor ROI interactively; no sender metadata input.
.\build-wgc-release\apps\PixelBridgeDecoder\Release\PixelBridgeDecoder.exe --capture-bootstrap --backend wgc --seconds 10 --telemetry .\wgc-bootstrap-new.jsonl

# Use the actual visible data-window ROI, including a signed desktop origin.
# This example geometry must be replaced with the selected monitor/window ROI.
.\build-wgc-release\apps\PixelBridgeDecoder\Release\PixelBridgeDecoder.exe --capture-bootstrap --backend dxgi --roi -1920 0 0 1080 --seconds 10 --telemetry .\dxgi-bootstrap-new.jsonl
```

The backend is required and never auto-switched. Duration is 1..600 seconds (default 10), excluding separately bounded initialization/shutdown. Coordinate parsing is checked int32 and the positive ROI dimensions are capped at 16384 before the actual monitor and memory checks. Identity/profile/sequence input options do not exist: recovery uses only the captured pixels.

Diagnostic capture requests FP16 while retaining actual negotiated format metadata, uses a finite 1000 ms age policy, and reserves at most 512 MiB for owned ROI resources, 1 GiB total capture resources and 512 MiB readback resources. These are upper limits, not an allocation request: the staging/CPU/rotation/source footprint must fit before creation. Unsupported HDR or unknown signal is an explicit visual erasure; FP16 linear SDR uses the SDR transfer function, not HDR tone mapping. Capture, crop and rotation remain GPU operations.

JSONL events include backend, LUID, formats, color state, original timestamp/domain, observation/source/slot generations, geometry, A/B FEC and CRC outcomes, quality/erasure reasons and temporal generations. Snapshots report native timeouts/age/rebuilds/high-water, explicit `DiagnosticCpuReadback` bytes/time/drop counters and visual admission counters. uint64 identities are decimal strings to avoid JSON-number precision loss; non-finite diagnostic floats are null.

Snapshots are not an atomic sample of three threads. They therefore carry normalized, readback and visual domains independently, readback `active/resetPending`, and a `visual.temporalStateCurrent` compatibility flag. Pending Reset or a single different source-id byte makes the flag false; old worker state is never silently attributed to the new capture domain.

The actual worker maintains 64 retained sequence identities, 8 sessions and a 16-event latest-wins diagnostic queue. Full identity conflicts become tombstones. Geometry/phase/scale and black/white calibration changes have separate generations. Repeated pixels, same identity/new observation, changed geometry/calibration and stale evicted sequences are distinguished; none adds an independent confidence sample.

The optional telemetry file is create-new, max 16 MiB, with checked Write/Flush/Close. The final stdout flush is checked as well. Exit codes are 0 for at least one **committed pixel recovery** during this invocation, 4 for none, 3 for selector cancellation, 2 for bad arguments, and 1 for runtime/output/cleanup failure. A lost diagnostic Accepted event cannot turn successful recovery into exit 4: the final result uses the authoritative cumulative processor counter after shutdown, not the bounded log queue.

## Phase 3 implementation and review journal

The dedicated wire/layout/RS/locator/Golden specification is in `LOCAL_DESKTOP_BOOTSTRAP.md`. It documents the fixed experimental ID, two independent full words, nine full-identity timing patches, the finite detection boundary and the independent Python/C++ oracles.

| Checkpoint | Command / log | Result |
|---|---|---|
| New RS, raster, Encoder args, existing Golden compatibility | Full default Release build, then selected CTest; `stage3-rs-build.log` / `stage3-rs-gate.log` | 7/7 PASS |
| Locator, app processor, both CLIs and existing compatibility | Full default Release build, then selected CTest; `stage3-full-visual-build.log` / `stage3-full-visual-gate.log` | 39/39 PASS |
| Actual Encoder presentation entry and first native Bootstrap attempt | `stage3-native-build.log` / `stage3-native-gate.log` | Encoder entry PASS; Bootstrap native FAIL at required SDR precondition, not skipped |
| Independent luma-proposal regression before fix | `stage3-luma-red.log` | 2 cases / 4 failing assertions: valid 96-level contrast missed; another valid luma-range frame not reported ambiguous |
| Luma proposal fix, all portable Bootstrap cases | `stage3-luma-fix.log` | 36 cases / 12,604,393 assertions PASS; acceptance thresholds unchanged |
| Readback device recovery and original review set | `stage3-review-build.log` / `stage3-review-gate.log` | 7/8 PASS; JSON probe initially compared numeric formatting 2.0 vs parser's 2, corrected to exact numeric value plus NUMBER type |
| DXGI preflight red reproduction | `stage3-preflight-red.log` | 2 cases / 96 assertions, 39 failures, proving missing scratch/8B admission |
| Full Release checkpoint before final matrix expansion | `stage3-reviewed-build.log` / `stage3-reviewed-ctest.log` | Default build PASS; 102/105 CTest PASS; three native suites fail, detailed below |
| Independent complete axis/phase/torn/ghost matrix | `stage3-matrix-build.log` / `stage3-matrix-gate.log` | 5/6 selected suites PASS; 8/48 shifted quarter ghosts incorrectly accepted, 32 failing assertions |
| Original-point gray regression before fix | `stage3-core-samples-build.log` / `stage3-core-samples-red.log` | 5 cases / 3260 assertions; 84 expected failures = 21 incorrectly accepted observations × 4 assertions |
| Original-point gray fix and complete visual regression | `stage3-core-samples-fixed-build.log` / `stage3-core-samples-fixed-gate.log` | Full default build and 6/6 selected suites PASS, including the unchanged full matrix, original thresholds, allocation probe, actual processor and JSON/CLI |

Review findings were triaged before changes:

| Severity | Finding | Fix / regression |
|---|---|---|
| Medium | One fixed 128 luma threshold misses legitimate calibrated frames and can miss a second candidate | Three finite proposals 128/64/192 share the same candidate/work budget; scan all proposals; independent contrast96/95 and two-location fixtures |
| Medium | DXGI preflight misses rotation scratch or negotiable FP16 footprint | One shared private preflight uses current resolved geometry and conservative 8B before any duplication/device creation; exact/minus-one, 3-format×4-rotation and same-monitor geometry regressions |
| Medium | Readback Map/Create device loss permanently stops the CPU worker | GPU-domain invalidation is recoverable; three native error codes at Map/Create, partial staging rollback, new real WARP device, invalid pitch Unmap, non-device failure and Stop races |
| Medium | CLI exits early on transient device-loss snapshot during finite recovery | Terminal predicate checks lifecycle/worker terminal state, not transient capture error alone; Starting/Running/Draining/Recreating/Waiting matrix |
| Medium | Lossy event queue incorrectly decides whether CLI succeeded | Authoritative committed counter after Stop; real raster Analyze/Commit 17 times gives accepted1/duplicate16/dropped1 despite no Accepted event remaining |
| Medium | Snapshot labels old worker temporal state with only the new capture domain | Independent domains/resetPending/compatibility; actual blocked Analyze/WARP test and independent JSON parser cases |
| Medium | Averaging five core points hides within-cell shifted 25%/75% double images | Retain the original five points in a fixed array; independently apply the existing .18/.06 middle-gray classification to each A/B copy and each timing patch, in addition to all old mean/residual gates. No additional pixel reads, heap allocation, resized image or threshold change |
| Low | Successful buffered stdout insertion hides a later flush error | Checked final flush with a streambuf that fails only sync |
| Gate configuration | Clang coverage/UBSan settings cannot be inferred from driver flags | Production PBModulation/PBProtocol get fuzzer-no-link; UBSan failures in those TUs and the new driver are non-recovering; Clang execution remains separately unverified |
| Documentation | libFuzzer example could mutate fixed source seeds | Writable corpus copied to the build tree; source seeds remain independent immutable inputs |

### Native environment observed during implementation

The unchanged WGC and DXGI pixel fixtures initially identified `SpaceEngine`, and later `Chrome_WidgetWin_1`, above the test ROI (example ROI point 1111,623; fixture window rect 1088,592,1472,848). The new Bootstrap fixture independently queried the primary adapter (LUID 0:94762), color space 12 and 10 bits/color, i.e. HDR, and failed its required SDR/G22/P709 precondition before presenting its pattern. Neither assertion was relaxed or converted to a skip, and no display, HDR, rotation, desktop or cursor setting was changed.

In `stage3-reviewed-ctest.log`, the actual DXGI raw FP16 capture/recreate/retirement case passed: format10, actual and expected red half-float bits17152, epoch1→2, both fence and query-marker variants, and clean final lease/ring retirement. The native environment-notification case also passed. The other DXGI native case still failed visibility, so the suite remained FAIL. This is positive evidence for that actual raw FP16 case, not SDR Bootstrap decoding or a fully passed native suite.

The user was asked to make the data window visible and temporarily select SDR if convenient. Native WGC/DXGI Bootstrap tests also include the actual Decoder CLI with only physical ROI/backend arguments, but an HDR-precondition failure is not evidence that any of those positive capture paths passed. Real portrait/hybrid/TDR/desktop-switch and the full 2× native matrix remain conditional manual hardware checks; WARP/fault injection does not substitute for them.

The direct Bootstrap native cases now also require actual same-instance WGC pool / DXGI duplication recreation, a new domain and source generation, and a once-presented independent second Golden pattern recovered through ordinary Poll with no subsequent Present. A thin processor observer checks that the real worker Reset clears geometry/calibration/history/events before accepting the new pattern. Seeing the first still-visible pattern under the genuinely new domain is allowed; relabelling an old queued observation is not. These stronger positive cases retain their SDR/visibility prerequisites.

### Independent visual matrices and original-point quality boundary

`PBLocalDesktopMatrixTests` contains 298 specified observations plus 18 independently valid component preconditions: 50 complete axis-scale combinations, 128 extreme-scale/phase combinations, 56 nontrivial quadrant cuts, 16 observed timing subpatch replacements and 48 shifted ghosts. Area and bilinear fixture resampling are independent from production, and both mix→scale and scale→mix orders are exercised. The a/e pair differs only in sequence bit48 and the resulting CRC/timing. No production encoder creates expected pixels.

The failing ghosts had mean-middle-gray fraction .0390625 and timing residual about .06428, below the unchanged .06/.065 thresholds, but original-point middle-gray fraction about .0970395. The fix retains the same five samples and applies the already-defined middle-gray policy before averaging can hide them. Independent exact fixtures pin 76/1280 versus77/1280 samples per timing patch and182/3040 versus183/3040 per A/B copy. Physical byte deltas69/70 straddle the original .18 classification boundary. Telemetry exposes both mean and original-point fractions; nonfinite values are JSON null.

`PBLocalDesktopNoAllocationProbe` is a separate executable with positive controls for ordinary, array, aligned and nothrow C++ allocation replacements. It checks cold RS entry, correction/error/alias paths and actual raster/locator/Luma decoding under allocation denial. It proves no calls to the intercepted C++ allocation functions in those exercised paths, not the absence of arbitrary C `malloc` or OS `HeapAlloc` calls.

The final RS coverage audit added an independent, fixed root-count rejection fixture: `rootless-locator.bin`, 76 bytes, BLAKE3 `2e60801855d17a127f8362e85cdb93486e4f5ad3d7d97e4d4011565b969819bf`. Its 44-byte record and CRC are unchanged; a parity-only GF solve creates an exact degree-two syndrome recurrence whose locator has no GF roots. Tests verify that proof independently and require exact `LocatorRootCount` plus immutable output and four alias layouts. The authorized `--extend` appended one fixture/manifest file-entry only after verifying all existing pins; root independently reran `--check` and all32 files passed (`stage3-independent-golden-final.log`). No production RS change or old Golden re-pin was needed.

## Final Gate and commit policy

The final commands use independent build trees, full default builds before full CTest, and serialize the two configurations' real-desktop tests:

```powershell
cmake -S . -B build-wgc-release -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake -DBUILD_TESTING=ON -DPB_BUILD_TESTS=ON -DPB_BUILD_APPS=ON -DPB_BUILD_TOOLS=ON -DPB_BUILD_PHASE0_GATE=ON -DPB_BUILD_WGC_GATE=ON -DPB_BUILD_DXGI_GATE=ON -DPB_BUILD_LOCAL_DESKTOP_GATE=ON -DPB_BUILD_FUZZERS=OFF -DPB_BUILD_BENCHMARKS=OFF -DPB_BUILD_PRESENTATION_GATE=OFF -DPB_BUILD_SCREEN_REGION_GATE=OFF
cmake --build build-wgc-release --config Release --parallel 4
ctest --test-dir build-wgc-release -C Release --parallel 4 --output-on-failure

cmake -S . -B build-wgc-asan -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake -DBUILD_TESTING=ON -DPB_BUILD_TESTS=ON -DPB_BUILD_APPS=ON -DPB_BUILD_TOOLS=ON -DPB_BUILD_PHASE0_GATE=ON -DPB_BUILD_WGC_GATE=ON -DPB_BUILD_DXGI_GATE=ON -DPB_BUILD_LOCAL_DESKTOP_GATE=ON -DPB_BUILD_FUZZERS=ON -DPB_BUILD_BENCHMARKS=OFF -DPB_BUILD_PRESENTATION_GATE=OFF -DPB_BUILD_SCREEN_REGION_GATE=OFF
cmake --build build-wgc-asan --config RelWithDebInfo --parallel 4
ctest --test-dir build-wgc-asan -C RelWithDebInfo --parallel 4 --output-on-failure
```

Both trees use Visual Studio 17 2022 x64 and the pinned vcpkg manifest through `D:/vcpkg/scripts/buildsystems/vcpkg.cmake`, with apps/tools/tests/Phase0 and native WGC/DXGI/LocalDesktop gates ON. The ASan tree additionally has `PB_BUILD_FUZZERS=ON`; the Release tree has it OFF. Native presentation/interactive selector certification gates stay OFF. These are opt-in evidence trees: new native gates remain OFF by default, as enforced by configure/subproject tests.

Both final configure commands and both complete default builds passed. Release CTest finished before ASan CTest started; their native tests never ran concurrently. The final logs are `stage3-acceptance-configure.log`, `stage3-acceptance-build.log` and `stage3-acceptance-ctest.log` in each build tree.

| Final check | Release | MSVC ASan / RelWithDebInfo |
|---|---|---|
| Complete default build | PASS | PASS |
| Complete corresponding CTest | **106/107 PASS; 1 FAIL**, 242.73s | **232/233 PASS; 1 FAIL**, 222.30s |
| `PBLocalDesktopBootstrapTests` | 42 cases / 12,607,718 assertions PASS | Same 42 cases / 12,607,718 assertions PASS |
| `PBLocalDesktopMatrixTests` | 5 cases / 4748 assertions PASS, 10.81s | Same cases/assertions PASS, 43.68s |
| `PBLocalDesktopNoAllocationProbe` | PASS | PASS |
| `PBWgcNativeTests` | 2 cases / 143 assertions PASS, 2.07s | 2 cases / 143 assertions PASS, 3.07s |
| `PBDxgiNativeTests` | 3 cases / 7169 assertions PASS, 10.31s | 3 cases / 7509 assertions PASS, 11.89s |
| `PBLocalDesktopEncoderEntry` | PASS, 1.37s | PASS, 2.05s |
| `PBLocalDesktopNativeTests` | **4 cases FAIL at SDR precondition**, 68 assertions / 4 failures | **Same 4 cases FAIL at SDR precondition**, 68 assertions / 4 failures |

The different DXGI assertion totals reflect finite native polling paths, not different test expectations. In both final configurations the real WGC and DXGI raw-pixel suites passed actual acquisition, epoch1→2 recreation and retirement with both fence and query markers, plus initialization/shutdown/error-path checks. The earlier occlusion failures did **not** recur. This final result supersedes the earlier failed native-suite checkpoints without deleting their evidence. The real DXGI environment-notification/retry test also passed. No assertion was removed, relaxed or converted to a skip.

The **only final failing CTest entry in each configuration** is `PBLocalDesktopNativeTests`. All four cases report the same live primary-output prerequisite: adapter LUID `0:94762`, `colorSpace=12`, `hdr=1`, `bits=10`. They fail `REQUIRE_FALSE(environment.hdr)` before presenting/capturing their independent SDR pattern. Consequently, actual SDR Bootstrap recovery, its real pool/duplication recreation/reset path and both Decoder CLI pixel-recovery positive cases remain **unverified**, not passed or skipped. This is an environment prerequisite failure; it is not evidence that those unexecuted positive branches are defect-free.

Successful raw capture must not be confused with the strict normalized cursor/color contract: the final DXGI raw snapshots have `cursorAvailable=0/cursorExcluded=0`. They do not prove cursor exclusion on this hardware. WGC reports its supported exclusion capability, but neither result bypasses the strict Bootstrap Gate. Likewise, WGC FP16 has a documented linear-scRGB signal contract; a DXGI FP16 descriptor alone does not establish that encoding, so the normalizer records an unknown signal rather than inferring linear SDR from the texture type. Unknown/HDR diagnostic inputs remain erasures.

All registered legacy Golden, raw-WGC compatibility, no-Qt/static-link/configuration, parser/FEC/receiver and Phase-0 tests passed in the final runs. `PBPhase0GateLarge` passed in Release (228.72s); its pre-existing `CONFIGURATIONS Release` registration excludes it from RelWithDebInfo, rather than this task skipping or weakening it. Phase-0 Fast and Resume passed in both applicable configurations. The final ASan `PBOuterFecTests` also passed; older-run failures are not attributed to this run.

**Acceptance status: NOT ALL GATES PASSED. No Git commit was created.** The branch remains `master` at `db47dd4`, with an empty index and task changes left in the worktree. Creating the requested atomic commit is conditional on the missing required positive native Gate passing and a fresh final review; portable/WARP success is not a substitute. No HDR/display/rotation/desktop/cursor setting was changed. Real portrait, hybrid-adapter, hardware TDR/desktop-switch and screen-size-limited 2× native tests remain explicitly unexecuted manual checks, not simulated hardware passes.

### Post-fix ASan, mutation and static checks

The complete default ASan build after the original-point fix passed (`build-wgc-asan/stage3-core-samples-build.log`). The production PBModulation, PBProtocol, PBCaptureNormalize and capture backend projects were inspected for actual `/fsanitize=address` flags; this is not wrapper-only instrumentation. The subsequent selected capture/visual/CLI/mutation/corpus run passed **25/25**, 45.68s (`stage3-core-samples-gate.log`), including 42.60s for the full independent visual matrix. This selected run does not replace the full final CTest.

```powershell
ctest --test-dir build-wgc-asan -C RelWithDebInfo --parallel 3 -R '^(PBLocalDesktopBootstrapTests|PBLocalDesktopMatrixTests|PBLocalDesktopNoAllocationProbe|PBCaptureNormalizeTests|PBCaptureRotationTests|PBCapturePipelineTests|PBScreenCaptureDxgiTests|PBBootstrapDiagnosticTests|PBDecoderCliTests|PBDecoderTelemetryJson|PBModulationLocalDesktopBootstrapFuzzSmoke|PBModulationLocalDesktopCorpus\..*)$' --output-on-failure
& './build-wgc-asan/fuzz/RelWithDebInfo/PBModulationLocalDesktopBootstrapFuzz.exe' 4096 5783543126721007665
```

The extra deterministic mutation run passed, reporting `smoke=8 iterations=4096 seed=5783543126721007665` (`stage3-local-desktop-mutation-4096.log`). The 14 exact semantic corpus replays and the 64-iteration CTest smoke also passed. This is **MSVC ASan deterministic mutation**, not an executed libFuzzer/UBSan run.

```powershell
cppcheck --language=c++ --std=c++20 --platform=win64 --library=windows --enable=warning,performance,portability --inline-suppr --suppress=missingIncludeSystem --error-exitcode=1 --file-list=build-wgc-release/stage3-cppcheck-files.txt -Ilibs/PBCaptureNormalize/include -Ilibs/PBCaptureNormalize/src -Ilibs/PBProtocol/include -Ilibs/PBModulation/include -Ilibs/PBScreenCaptureWgc/include -Ilibs/PBScreenCaptureDxgi/include -Ilibs/PBScreenRegion/include -Ilibs/PBRenderD3D/include -Ilibs/PBPresentTiming/include -Ilibs/PBCore/include -D_WIN32 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DUNICODE -D_UNICODE
git diff --check
git -c core.quotepath=false diff --stat
git -c core.quotepath=false status --short
git diff --cached --name-only
& 'D:\Python3.12.9\python.exe' tests/PBModulation/generate_local_desktop_golden.py --check
& 'D:\Python3.12.9\python.exe' build-wgc-release/stage3-final-audit.py
```

Cppcheck 2.21.0 completed all 22 listed changed production/driver translation units with exit0 and no enabled warning/performance/portability diagnostics (`stage3-cppcheck-final.log`). Earlier advisory findings were fixed without suppressing them: immutable normalization config returns a const reference, and the already mutex-noncopyable WGC callback gate explicitly deletes copy/move operations with static trait assertions.

`git diff --check` passed. A separate UTF-8 audit also checks all modified/untracked task text for trailing whitespace, missing final newline, conflict/reference residue, unexpected protocol/recovery edits and old Golden edits (`stage3-scope-whitespace-audit.json`); `git diff` alone does not inspect untracked files. The index is empty. LF→CRLF notices are Git conversion advisories, not whitespace failures.

CTest JSON and both caches were checked independently: 107 Release tests / 233 ASan tests, the required native gate switches ON in these two evidence trees, default-disabled presentation/selector gates still OFF, and all four native capture/LocalDesktop entries have both `RUN_SERIAL` and the `PixelBridgeDesktop` resource lock (`stage3-options-audit.json` in each tree). Temporary misspelled, unused command-line cache options were removed; required real options were already ON, and the final command above spells all options explicitly.

### Final diff review

The final review covered the actual backend facades and shared owner/ring, output/adapter resolution, GPU rotation mapping, Acquire/Release cleanup, source and consumer markers, deferred cleanup, staging/CPU lease separation, full-domain completion checks, worker-owned temporal state, RS/locator failure immutability, bounded search and history, CLI parsing/output/exit semantics, and CMake/Golden compatibility. Fixed findings and their red/green regressions are recorded above. A separate read-only review of the production readback → processor → telemetry/CLI path found no remaining actionable finding. No known unresolved Critical/High finding remains in the reviewed code; that review does not replace the still-failing native positive Gate.

The read-only final audit script resides only in the ignored build tree, so it is not a new product dependency. It records hashes of every modified/untracked task file, checks whitespace and frozen-library/old-Golden scope, and verifies that the index remains empty. Generated logs/build products are not staging candidates. No test was deleted, skipped, re-pinned or weakened to turn the HDR precondition or earlier code regressions green.
