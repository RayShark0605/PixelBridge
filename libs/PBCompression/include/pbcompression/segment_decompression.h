#pragma once

#include "pbcompression/compression_error.h"
#include "pbprotocol/protocol_types.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace pbcompression {

inline constexpr std::uint64_t kDefaultDecompressionMaxOutputBytes =
    16 * 1024 * 1024;

// Receiver-side limits. The encoder windowLog used for a frame must be at or
// below maxWindowLog, otherwise decompression fails through the fixed zstd
// error mapping. maxOutputBytes of 0 is a valid strict policy (no output
// allowed); the UINT64_MAX sentinel is rejected because it would defeat the
// checked bound.
struct DecompressionLimits
{
    std::uint64_t maxOutputBytes = kDefaultDecompressionMaxOutputBytes;
    std::uint32_t maxWindowLog = 23;
};

[[nodiscard]] CompressionStatus ValidateDecompressionLimits(
    const DecompressionLimits& limits) noexcept;

// Streams one zstd frame from accumulated input into exactly
// expectedRawSize output bytes. Invariants:
//   1. Policy checks run before any allocation (33.1): expectedRawSize is
//      checked against maxOutputBytes before the output buffer exists.
//   2. The output buffer holds expectedRawSize plus one probe byte (when the
//      budget allows) so a frame that overruns the expected size fails with
//      OutputLimitExceeded instead of writing past the bound.
//   3. The input must be exactly one complete frame: truncated input fails
//      with IncompleteFrame and trailing bytes with TrailingInput.
//   4. The produced byte count must equal expectedRawSize exactly
//      (RawSizeMismatch otherwise).
// Single owner, no internal synchronization. Terminal after the first error
// or after Finish() succeeds.
class SegmentDecompressor
{
public:
    SegmentDecompressor(const SegmentDecompressor&) = delete;
    SegmentDecompressor& operator=(const SegmentDecompressor&) = delete;
    SegmentDecompressor(SegmentDecompressor&&) noexcept;
    SegmentDecompressor& operator=(SegmentDecompressor&&) = delete;
    ~SegmentDecompressor();

    [[nodiscard]] static CompressionResult<SegmentDecompressor> Create(
        const DecompressionLimits& limits,
        const std::uint64_t expectedRawSize);

    CompressionStatus Update(const std::span<const std::byte> input);
    [[nodiscard]] CompressionResult<std::vector<std::byte>> Finish();

private:
    SegmentDecompressor() noexcept;

    [[nodiscard]] CompressionStatus CheckWritableState() const noexcept;
    [[nodiscard]] CompressionStatus CheckFrameContentSizePrefix();

    DecompressionLimits limits_{};
    std::uint64_t expectedRawSize_ = 0;
    // Opaque ZSTD_DStream handle; the DStream layout keeps the DCtx as its
    // first member, which is the layout contract zstd itself relies on when
    // it treats a DStream as a DCtx internally.
    void* decompressContext_ = nullptr;
    // Accumulates all input. [0, inputPosition_) is consumed but kept so
    // Finish() can detect trailing bytes; consumed bytes are never fed to
    // the decoder again.
    std::vector<std::byte> inputBuffer_{};
    std::size_t inputPosition_ = 0;
    // Produced bytes live in [0, producedBytes_).
    std::vector<std::byte> outputBuffer_{};
    std::uint64_t producedBytes_ = 0;
    bool frameComplete_ = false;
    bool frameFinished_ = false;
    bool frameContentSizeChecked_ = false;
    // Exact frame header size once the buffer holds at least
    // ZSTD_FRAMEHEADERSIZE_MAX; 0 means not yet known. The header is fed
    // to the decoder in its own call so the streaming window-limit check
    // runs (the single-pass shortcut taken when a whole frame arrives at
    // once bypasses it).
    std::size_t frameHeaderBytes_ = 0;
    bool moved_ = false;
    std::optional<CompressionError> terminalError_;
};

// Decodes one encoded segment. Raw requires encodedBytes to be exactly
// expectedRawSize and copies it; Zstandard uses the streaming path above;
// any other codec value is a protocol conflict and fails closed.
[[nodiscard]] CompressionResult<std::vector<std::byte>> DecompressSegment(
    const pbprotocol::CompressionCodec codec,
    const std::span<const std::byte> encodedBytes,
    const std::uint64_t expectedRawSize,
    const DecompressionLimits& limits);

} // namespace pbcompression