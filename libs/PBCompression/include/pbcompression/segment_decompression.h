#pragma once

#include "pbcompression/compression_error.h"
#include "pbprotocol/protocol_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace pbcompression {

inline constexpr std::uint64_t kDefaultDecompressionMaxOutputBytes =
    16 * 1024 * 1024;
inline constexpr std::uint64_t kDefaultDecompressionMaxInputBytes =
    32 * 1024 * 1024;
inline constexpr std::size_t kZstdFrameHeaderMaximumBytes = 18;

// Receiver-side limits. The window advertised by a frame must not exceed
// 2^maxWindowLog bytes; decoder policy never depends on encoder tuning.
// maxOutputBytes of 0 is a valid strict policy (no output allowed). Input and
// output limits must both be representable by the backing byte container.
struct DecompressionLimits
{
    std::uint64_t maxOutputBytes = kDefaultDecompressionMaxOutputBytes;
    std::uint32_t maxWindowLog = 23;
    std::uint64_t maxInputBytes = kDefaultDecompressionMaxInputBytes;
};

// Builds the compression-specific portion of the local receiver policy. The
// window log is derived from maxZstdWindowBytes as floor(log2), so a frame may
// only use a window no larger than the policy allows; logs outside the codec's
// supported range fail closed in ValidateDecompressionLimits. Callers must
// still validate the ReceiverResourcePolicy itself before using it to accept
// an untrusted descriptor.
[[nodiscard]] DecompressionLimits MakeDecompressionLimits(
    const pbprotocol::ReceiverResourcePolicy& resourcePolicy) noexcept;

[[nodiscard]] CompressionStatus ValidateDecompressionLimits(
    const DecompressionLimits& limits) noexcept;

// Streams exactly expectedEncodedSize bytes containing one zstd frame into
// exactly expectedRawSize output bytes. Invariants:
//   1. Policy checks run before any allocation (33.1): both descriptor sizes
//      are checked against the local input/output limits first.
//   2. The output buffer holds expectedRawSize plus one probe byte (when the
//      budget allows) so a frame that overruns the expected size fails with
//      OutputLimitExceeded instead of writing past the bound.
//   3. Only the bounded frame header is staged; consumed encoded bytes are
//      never retained. The received byte count must equal expectedEncodedSize.
//   4. The input must be exactly one complete frame: truncated input fails
//      with IncompleteFrame and trailing bytes with TrailingInput.
//   5. The produced byte count must equal expectedRawSize exactly
//      (RawSizeMismatch otherwise).
// Single owner, no internal synchronization. Terminal after the first error
// or after Finish() succeeds.
class SegmentDecompressor
{
public:
    SegmentDecompressor(const SegmentDecompressor&) = delete;
    SegmentDecompressor& operator=(const SegmentDecompressor&) = delete;
    SegmentDecompressor(SegmentDecompressor&& other) noexcept;
    SegmentDecompressor& operator=(SegmentDecompressor&&) = delete;
    ~SegmentDecompressor();

    [[nodiscard]] static CompressionResult<SegmentDecompressor> Create(
        const DecompressionLimits& limits,
        std::uint64_t expectedEncodedSize,
        const std::uint64_t expectedRawSize);

    CompressionStatus Update(const std::span<const std::byte> input);
    [[nodiscard]] CompressionResult<std::vector<std::byte>> Finish();

private:
    SegmentDecompressor() noexcept;

    [[nodiscard]] CompressionStatus CheckWritableState() const noexcept;
    [[nodiscard]] CompressionStatus DecodeInput(
        std::span<const std::byte> input);
    [[nodiscard]] CompressionStatus ParseAndDecodeFrameHeader(
        std::span<const std::byte>& remainingInput);

    DecompressionLimits limits_{};
    std::uint64_t expectedEncodedSize_ = 0;
    std::uint64_t expectedRawSize_ = 0;
    std::uint64_t receivedEncodedBytes_ = 0;
    // Opaque ZSTD_DStream handle. This public header intentionally does not
    // expose zstd.h.
    void* decompressContext_ = nullptr;
    std::array<std::byte, kZstdFrameHeaderMaximumBytes> frameHeaderBuffer_{};
    std::size_t frameHeaderBufferedBytes_ = 0;
    // Produced bytes live in [0, producedBytes_).
    std::vector<std::byte> outputBuffer_{};
    std::uint64_t producedBytes_ = 0;
    bool frameComplete_ = false;
    bool frameFinished_ = false;
    bool frameHeaderParsed_ = false;
    bool frameContentSizeKnown_ = false;
    bool moved_ = false;
    std::optional<CompressionError> terminalError_;
};

// Decodes one encoded segment. expectedEncodedSize and expectedRawSize must
// come from a descriptor already validated against ReceiverResourcePolicy.
// Raw requires both sizes to match and copies the bytes; Zstandard uses the
// streaming path above; any other codec value fails closed.
[[nodiscard]] CompressionResult<std::vector<std::byte>> DecompressSegment(
    const pbprotocol::CompressionCodec codec,
    const std::span<const std::byte> encodedBytes,
    std::uint64_t expectedEncodedSize,
    const std::uint64_t expectedRawSize,
    const DecompressionLimits& limits);

// Canonical descriptor-bound overload. The descriptor must first pass
// pbprotocol::ValidateSegmentDescriptor(..., ReceiverResourcePolicy).
[[nodiscard]] CompressionResult<std::vector<std::byte>> DecompressSegment(
    const pbprotocol::SegmentDescriptor& descriptor,
    std::span<const std::byte> encodedBytes,
    const DecompressionLimits& limits);

} // namespace pbcompression
