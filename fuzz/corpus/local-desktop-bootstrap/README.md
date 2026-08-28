# Bounded LocalDesktop Bootstrap semantic corpus

These files are **harness inputs**, not captured images or new protocol records.
Every replay checks semantic outcomes and aborts on a violation; a successful
replay prints `CORPUS_REPLAY_VALIDATED`. No test helper, sender-side IPC or mutable
cross-input raster cache is used.

The first byte, modulo 3, selects the real production path:

| Mode | Controls after mode byte | Checked outcome |
|---|---|---|
| 0, RS | encode output length, decode output length, error count, first error position, nonzero-mask selector; remaining bytes are opaque RS input | Wrong sizes leave the entire output/guards unchanged; every successful decode re-encodes within symbol Hamming distance 16; known inputs with 0..16 distinct symbol errors recover all 44 original bytes. |
| 1, small LumaView | variant, width, height, format, stride, sample X, sample Y; remaining bytes fill a bounded 4096-byte pixel array | Independent footprint oracle, real row pitch and Gray8 bilinear sampling, immutable failed sample, finite SDR result, explicit FP16 NaN/Inf/out-of-range rejection, and erasure for every too-small view. |
| 2, actual visual pixels | variant, errors A, errors B, first A, first B, mask A, mask B, tear direction, sequence LE64, tag LE64, control epoch LE32 | Independent canonical44 construction → actual visual encoder → nearest-neighbour 960×540 Gray8 → public decoder, without passing record metadata to the decoder. |

Missing controls/payload bytes are zero. Integer fields and CRC32C for the visual
fixture are written independently of the product serializer/checksum function.
Visual tag zero is mapped to one; the foreign frame changes only sequence using
`sequence XOR 1`, so the two frames are distinct even at UINT64_MAX.

Visual variants are fixed: `0` corrects independent A/B hard symbol errors
(`errors % 17`, unique positions, nonzero masks), `1` makes a horizontal/vertical
tear with two intact but different-sequence copies, `2` averages every pixel
50/50, and `3` replaces only the distant B copy. Variant 0 requires all 44 bytes
from both copies and their exact correction counts. Variants 1/3 require actual
independent valid A/B decodes and `BootstrapMismatch`; all mixed/blended variants
must expose an erasure, zero accepted canonical44 and zero quality.

| Fixture | Purpose |
|---|---|
| `rs-short-input.bin` / `rs-long-input.bin` | Received RS input lengths 75/77, failure atomicity. |
| `rs-short-output.bin` | Exact opaque input44 with encode output75; valid word with decode output43; both rejected without writes. |
| `rs-correction16.bin` | Valid all-zero received word; generated codeword with 16 distinct hard symbol errors; exact recovery. |
| `view-padded-gray.bin` | Real 4×3 Gray8 view, pitch9, fractional sample (1.5,1), exact interpolation oracle. |
| `view-overflow.bin` | UINT32_MAX extents and SIZE_MAX pitch over a tiny actual allocation. |
| `view-fp16-nan.bin` | Actual sampled half NaN, not merely an unsupported-format label. |
| `view-coordinate-nonfinite.bin` | Valid single-pixel allocation with NaN/Inf sample coordinates. |
| `visual-clean.bin` | Positive decode of both complete records from real rendered pixels. |
| `visual-correction16.bin` | 16 independent corrupted byte symbols in each spatial copy. |
| `visual-torn-horizontal.bin` / `visual-torn-vertical.bin` | Intact A/B from different FrameSequences; never assemble one codeword. |
| `visual-blend50.bin` | 50/50 pixel blend must erase, even if RS can decode a hard decision. |
| `visual-foreign-copy.bin` | Different sequence in B alone, with original markers/timing. |

Each input is at most 4096 bytes. There is at most one public visual Decode call
per input, using the unmodified default 24,000,000 work-unit limit. RS calls have
fixed 44/76-symbol work bounds; the structured renderer uses at most 9,331,200
bytes of local raster storage, all input-independent sizes. There is no arbitrary
resize, unbounded input-sized allocation, retry loop or persistent cache.
