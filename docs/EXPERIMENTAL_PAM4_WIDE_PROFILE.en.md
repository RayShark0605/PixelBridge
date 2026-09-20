# PAM4 Wide mode contract

[简体中文](EXPERIMENTAL_PAM4_WIDE_PROFILE.md) | **English**


> Product labels are Standard / Gray Fast / PAM4 / PAM4 Wide, without experiment badges. Frozen `Experimental` / `experimental-*` identifiers below remain for internal/CLI compatibility, not as product-version labels. Validation scope still follows actual evidence.

## Identity and current integration

`PB-Experimental-Pam4-Wide-1`, Profile ID `0x504250414D345731`, layout 16. The GUI defaults fresh, missing, or invalid preferences to PAM4 (index 2); PAM4 Wide remains index 3 and must be selected explicitly, while valid saved indices are restored unchanged. The GUI default does not change Wide's independent Profile identity, formal catalog, or explicit CLI/core opt-in boundary. Both endpoints must match; profile changes require a new Session. The formal catalog still contains only SC6 V3.

GUI and CLI are integrated. Decoder supports selected-monitor whole-screen/ROI with finite budgets, no protected second screen and no reserved Decoder area. This is not arbitrary-scaling or physical-single-monitor certification. [Status](PROJECT_STATUS.en.md), [evidence](EVIDENCE_INDEX.md).

## Raster and coding

Logical canvas 1920×1080 BGRA8 SDR, alpha 255, neutral background 128. Only whole eligible Data cells are used; reserved markers, Bootstrap, timing/freshness, calibration, PhaseChecker and guard regions remain intact.

| Parameter | Value |
| --- | --- |
| Logical cell | 3×3 pixels |
| Fixed physical sender raster | 2560×1440 |
| Effective sender cell | 4×4 pixels |
| Eligible cells | 165192 |
| Levels / labels | `[0,85,170,255]`, two bits per cell, LSB first |
| Slot count | 20, exact, unique indices |
| Slot 0 | Active Control, Robust N16200/K10800, 1350 information bytes |
| Remaining slots | 19 Transport/inactive slots, Fast N16200/K13320, 1665 information / 1629 payload bytes each |
| Coded bits | 324000 |
| Unmapped zero bits | 6384 |
| Maximum file payload | 30951 B/frame, before overhead/loss/verification |

The Control slot's FEC is fixed by its role, not selected by trial decoding. Each data frame carries the current SegmentDescriptor. Session/Manifest refresh frames use inactive Transport filler. Invalid kind/priority, length, CRC, SessionTag, conflicting slot, and noncanonical zero padding fail closed.

Wide maps pixel centers using `floor((2*x+1)*1920/(2*2560))` and the corresponding y formula to a fixed 2560×1440 point-sampled raster. Logical 3px cells become physical 4px cells. Native-size, arbitrary width, linear, and area alternatives are not the contract. A 1080P sender cannot fit this raster.

## Frozen mapping and geometry

The mapping arrays are the specification, not a PRNG run at startup. No FrameSequence permutation is used. Sites are row-major `row*640+column`; codeword bits map to cell bit planes. Incomplete cells remain neutral; unmapped bits are zero.

- Mapping source: [`experimental_pam4_wide_mapping.h`](../libs/PBModulation/src/experimental_pam4_wide_mapping.h).
- Exact counts, reserved regions, FEC identity and byte-level digests: [independent Golden manifest](../tests/golden/experimental-pam4-wide/manifest.json).
- Mapping digests remain those in the [Chinese numeric reference](EXPERIMENTAL_PAM4_WIDE_PROFILE.md); numerical tables and machine-readable manifests are language-independent.

The CPU reference consumes borrowed Gray8/BGRA8 SDR pixels with actual row pitch. Geometry from markers and two matching Bootstrap records is checked with nine-region freshness. Erasures clear accepted outputs and geometry cache. Epoch/ROI/device changes require reset. A previous accepted geometry is only a bounded hint, not payload truth or universal equivalence to exhaustive search.

## Calibration and soft metrics

Coarse visible levels `[8,64,160,232]`; held-out `[56,112,168,248]` and neutral 128. Adjacent fitted level gaps must be at least 16, total contrast at least 96, spatial/held-out residuals no larger than 8. Data cannot tune calibration using known payload.

BGRA luma uses `(722*B+7152*G+2126*R)/10000`; neutral RGB agrees with Gray8. Sampling is bilinear at the measured cell centers. Max-log metric scaling is 16384, clipping ±32767, ties-to-even rounding. The denominator is 1.5 times the median observed adjacent PAM4 centroid spacing; layout15 retains its original 127.5 denominator. MSVC uses `/fp:precise` for the source. Inner FEC allows at most 12 iterations, offset 2048, scale 1/1.

## Bounded implementation and acceptance

One owner, preallocated metrics/scratch/accepted blocks, two reused FEC workspaces; conservative reservation `sizeof(Implementation)+2 MiB` is not measured RSS. Pixel-reader budget remains one million accesses; locator work remains bounded. No whole-image resize, hidden input, unbounded queue, or payload-truth oracle enters the core decoder.

Live capture uses PB-owned GPU staging plus explicit CPU readback/reference decoding; report its cost separately. Accepted local slots are only intermediate FEC/CRC results. Typed Control/schema/resource/conflict admission, Outer FEC, segment digests, whole-file digest, safe publication and final reopen are still mandatory.

GUI chooses 7 MiB segments. Finite memory tuning affects admission only. The selected-monitor live RemoteVisual/Auto entry does not grant Replay/capture-only/formal-measurement eligibility. [Architecture](ARCHITECTURE.en.md), [budgets](DECODER_MEMORY_BUDGET.en.md).
