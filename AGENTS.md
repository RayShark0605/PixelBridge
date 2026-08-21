# AGENTS.md — PixelBridge

## 1. Project mission

PixelBridge is a Windows x64 / C++20 project for high-performance one-way file transfer through visible desktop/video pixels.

The two applications are:

- `PixelBridgeEncoder.exe`
- `PixelBridgeDecoder.exe`

The Decoder must recover payload only from pixels it actually captures from the selected screen region. Do not introduce hidden payload transport through sockets, pipes, shared memory, COM, clipboard, window messages, temp-file exchange, or other IPC.

The canonical architecture document is `PixelBridge_最终技术路线与总体设计.md`. If it exists under `docs/`, prefer that copy. Before changing protocol semantics, FEC layout, visual profiles, capture lifetime rules, or file-recovery semantics, locate and read the relevant section of that document. Do not invent missing protocol rules.

## 2. Source-of-truth priority

When requirements conflict, use this order:

1. The current user task.
2. This `AGENTS.md`.
3. `PixelBridge_最终技术路线与总体设计.md`.
4. Existing tests and Golden Vectors.
5. Existing implementation.

If code disagrees with the design or Golden Vectors, do not silently preserve the code behavior. Report the conflict and fix it only within the current task scope.

## 3. Engineering principles

- Correctness before throughput.
- Measured `VerifiedEncodedGoodput` before theoretical bits/cell.
- No accepted output is valid until the required digest checks pass.
- Treat all visual/protocol input as untrusted input.
- Fail closed on ambiguity, conflicts, malformed descriptors, arithmetic overflow, or resource-limit violations.
- No unbounded queue, allocation, retry loop, packet cache, control reassembly, or active-session growth.
- No silent fallback that changes protocol semantics or weakens correctness.
- Do not weaken, delete, skip, or special-case a failing test merely to make the suite pass.
- Prefer a simple reference implementation before SIMD/GPU optimization.
- Keep wire semantics separate from implementation tuning.
- Any wire-compatible change requires tests and Golden Vector consideration.

## 4. Mandatory protocol invariants

- Wire format is explicitly serialized, little-endian, UTF-8 where applicable, and bounds checked.
- Never serialize C++ structs with raw `reinterpret_cast`/`sizeof(struct)` persistence.
- Session IDs come from an OS CSPRNG.
- A Session has a fixed Data Visual Profile. Changing an incompatible profile creates a new Session.
- Files are segmented; memory must remain `O(active segment set)`, not `O(total file size)`.
- Bootstrap and Control Plane are fixed and independent from experimental Data Plane modulation.
- Unknown mandatory features or unsupported protocol major versions are rejected.
- Same descriptor key with different valid content is a conflict and must fail closed.
- Same `(SessionTag, SegmentOrdinal, OuterBlockId)` with conflicting valid payloads is an error; never latest-wins.
- Unknown Segment data must not trigger large allocations or codec creation before descriptor/resource validation.
- Tiny/highly-compressible segments must have the DirectRepeat path where Wirehair dimensions or efficiency are unsuitable.
- Wirehair V2 uses its canonical serialized descriptor. The exact descriptor and exact encoded segment bytes must remain stable for that Segment across Carousel passes.
- Validate Wirehair dimensions before codec creation, including the supported `K` range.
- Handle Wirehair resource-exhaustion/error results explicitly; do not assume unlimited repair IDs can be accepted by one decoder instance.
- Transport CRC is not a cryptographic integrity or authentication mechanism.
- Whole-file digest is required before publishing the final file.
- In-band digest alone does not authenticate the sender.

## 5. File and resource safety

- All size, offset, count, multiplication, addition, and range arithmetic must use checked arithmetic.
- Enforce `ReceiverResourcePolicy` before large memory allocation, file preallocation, control reassembly, decompression, or FEC object creation.
- Sanitize untrusted filenames: basename only; reject traversal, absolute paths, ADS, reserved device names, embedded NUL, invalid trailing dot/space, and overlong names.
- Never overwrite an existing final target silently.
- `.part` must not be renamed to the final path before all required verification succeeds.
- Decompression must be output-bounded and window/resource-bounded.
- Source files used by the Encoder must remain immutable for the Session. Detect changes and abort the Session.
- Resume state is untrusted persistent input and must be length/checksum/bounds validated.

## 6. Windows graphics and capture invariants

- All screen-region coordinates are physical pixels. The process is Per-Monitor DPI Aware V2.
- Prefer a single-monitor ROI for certified performance paths.
- WGC and Desktop Duplication feed a common capture-normalization layer.
- A WGC `Direct3D11CaptureFrame` is a frame-pool lease. Do not access its surface after the lease is returned.
- Copy/crop into PixelBridge-owned textures using a bounded lease/ring model; retire leases only after GPU use of the source is complete.
- On `ContentSize`, mode, monitor, device, or capture-epoch change, drain stale work and rebuild dependent resources.
- Capture backlog drops stale frames rather than building latency.
- Treat cursor exclusion as normalized capture semantics where possible.
- D3D11 immediate-context submission should have one owner thread unless a measured alternative is explicitly justified.
- D3D11 Compute is the cross-vendor GPU baseline.
- CUDA is optional and only preferred when adapter compatibility and benchmark results justify it.
- Do not perform an implicit GPU→CPU→GPU round-trip on the fast path without reporting the fallback.
- LocalDesktop Direct-Level/quantized-raster modulation and Shape+Chroma must be compared by measured results; do not assume Shape+Chroma is always superior.
- `MinUpdateInterval`, refresh rate, or Present call rate is not proof of unique captured data FPS. Measure `UniqueVisualFPS`.

## 7. Video/offline invariants

- Offline MP4 is a data channel, not a perceptual-video-quality problem.
- Certified video profiles must explicitly define pixel format, range, color space, chroma siting, frame-rate/timing semantics, GOP/IDR policy, and relevant encoder settings.
- Prefer direct NV12 rasterization for the certified path; do not rely on an unspecified RGB→YUV conversion.
- Respect actual plane/row pitch; never assume a GPU NV12 resource is tightly packed only because the logical format is 4:2:0.
- A video loop repeats the same visual frames; it does not generate new Fountain equations.
- Verify produced bitstream/container semantics with an inspector rather than trusting encoder UI/API settings alone.
- Track BER/FER, bitrate, peak frame size, and offline artifact expansion.

## 8. C++ style

- C++ standard: C++20.
- Variables: lower camel case, e.g. `numImagePixels`.
- Functions: Upper Camel/Pascal style, e.g. `GetNumImagePixels`.
- Classes/structs/enums: clear descriptive PascalCase names.
- Opening braces are on their own line.
- Prefer `i++` rather than `++i` unless pre-increment is semantically required.
- A local variable that will not change after initialization should be `const`.
- Avoid cryptic abbreviations: use `geometry`, not `g`; `vertex`, not `v`.
- Prefer RAII and explicit ownership.
- Avoid general-purpose heap allocation on hot paths once the reference path is stable.
- Avoid global mutable state.
- Do not add comments that merely restate the code; document invariants, lifetime assumptions, wire semantics, and non-obvious trade-offs.

## 9. Architecture and dependencies

- Core libraries must not depend on Qt.
- Qt is for application UI/configuration/lifecycle/status presentation.
- Keep protocol, compression, FEC, modulation, capture, storage, telemetry, and platform backends modular.
- Pin third-party baselines used for releases and record them in build metadata/SBOM.
- A third-party library revision is an implementation baseline, not a wire-protocol version.

## 10. How to work on a task

Before editing:

1. Inspect `git status`; do not overwrite unrelated user changes.
2. Read this file.
3. Read only the relevant architecture sections and nearby code/tests.
4. State a short implementation plan internally before making a non-trivial change.
5. Identify the invariants and acceptance criteria for the task.

While editing:

- Keep the change scoped.
- Prefer small, reviewable units.
- Add or update tests with the implementation.
- For parsers/state machines, include malformed/boundary/conflict cases.
- For performance work, preserve a correctness/reference path.
- Do not change protocol constants, serialized layouts, Golden Vectors, public interfaces, or certified-profile semantics unless the task explicitly requires it.

Before finishing:

1. Build the affected targets.
2. Run the most relevant unit/integration/Golden Vector tests.
3. Run static checks/fuzz/benchmarks when the task calls for them.
4. Review the diff for lifetime, overflow, bounds, race, resource, and error-path problems.
5. Report exactly what was tested and any remaining limitation.
6. If a required check cannot be run, say why; do not claim success.

## 11. Review expectations

When asked to review code, review first and modify second.

The first review pass should produce findings ordered by severity: Critical / High / Medium / Low.

For each finding include:

- affected file/function,
- failure mechanism,
- reproducible scenario or reasoning,
- proposed fix,
- regression test to add.

Pay special attention to wire compatibility, descriptor/payload conflicts, integer overflow, resource exhaustion, WGC/D3D/CUDA lifetime and synchronization, queue/backpressure behavior, race/shutdown paths, partial/corrupt persistent state, color/range/pitch assumptions, duplicated correlated frames, and silent fallbacks.

Only after findings are understood should fixes be applied and re-tested.

## 12. Completion standard

A task is not complete because it compiles. A task is complete only when its acceptance criteria are met, relevant tests pass, no known Critical/High finding remains, output correctness is independently verifiable, error and cleanup paths are handled, architecture invariants remain true, and performance claims are backed by measurements rather than assumptions.
