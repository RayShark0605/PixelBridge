#pragma once

#include "pbcompression/compression_error.h"
#include "pbprotocol/protocol_types.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace pbcompression {

// ZSTD_compressStream2 can make progress with a one-byte destination. Keeping
// this minimum at one allows CompressSegment() to prove that a tiny zstd frame
// cannot fit and safely select Raw when the raw payload itself fits.
inline constexpr std::uint64_t kMinFrameBytes = 1;

// Starting output capacity when the expected raw size is unknown.
inline constexpr std::uint64_t kInitialStreamBytes = 65536;

// Per-Segment encoder settings. compressionLevel and maxWindowLog are local
// Encoder tuning only (design doc 32.2): they never appear on the wire, and a
// Decoder relies only on the standard zstd frame plus protocol limits.
// Encoded artifacts must not be produced with different settings inside one
// Session.
struct CompressionSettings
{
    int compressionLevel = 3;
    std::uint32_t maxWindowLog = 23;
    std::uint64_t maxOutputBytes = 32 * 1024 * 1024;
    // Extra descriptor/framing/FEC overhead added to the compressed size in
    // the 12.1 RAW fallback rule. Zero keeps the pure rule.
    std::uint64_t framingMarginBytes = 0;
};

// 12.1 RAW fallback decision: compressed + margin (checked) >= raw means the
// frame does not pay for its own framing, so the segment stays Raw.
// Overflow is treated as Raw (fail closed).
[[nodiscard]] pbprotocol::CompressionCodec ChooseSegmentCodec(
    const std::uint64_t compressedBytes,
    const std::uint64_t rawBytes,
    const std::uint64_t framingMarginBytes) noexcept;

struct EncodedSegment
{
    std::vector<std::byte> bytes{};
    pbprotocol::CompressionCodec codec = pbprotocol::CompressionCodec::Raw;
};

// Streams one Source Segment into one self-contained zstd frame. Single
// owner, no internal synchronization. After the first error or after
// Finish() succeeds the object is terminal and repeats the latched result.
class SegmentCompressor
{
public:
    SegmentCompressor(const SegmentCompressor&) = delete;
    SegmentCompressor& operator=(const SegmentCompressor&) = delete;
    SegmentCompressor(SegmentCompressor&& other) noexcept;
    SegmentCompressor& operator=(SegmentCompressor&&) = delete;
    ~SegmentCompressor();

    // expectedRawBytes of 0 means unknown; a nonzero value is pledged into
    // the frame header (FCS) so a byte-count mismatch fails at Finish().
    [[nodiscard]] static CompressionResult<SegmentCompressor> Create(
        const CompressionSettings& settings,
        const std::uint64_t expectedRawBytes);

    CompressionStatus Update(const std::span<const std::byte> input);
    [[nodiscard]] CompressionResult<std::vector<std::byte>> Finish();

private:
    SegmentCompressor() noexcept;

    [[nodiscard]] CompressionStatus CheckWritableState() const noexcept;
    [[nodiscard]] CompressionStatus GrowOutputBuffer(
        const std::uint64_t minimumFreeBytes);

    CompressionSettings settings_{};
    std::uint64_t expectedRawBytes_ = 0;
    std::uint64_t fedBytes_ = 0;
    // Opaque ZSTD_CCtx handle; this header intentionally does not expose
    // <zstd.h>.
    void* compressContext_ = nullptr;
    // Frame bytes live in [0, outputSize_); the tail is scratch space.
    std::vector<std::byte> outputBuffer_{};
    std::size_t outputSize_ = 0;
    bool frameFinished_ = false;
    bool moved_ = false;
    std::optional<CompressionError> terminalError_;
};

// Compresses one segment: empty input is Raw with zero bytes (no context is
// created); otherwise the stream is compressed and the 12.1 rule selects the
// codec. Frames carry a 32-bit checksum.
[[nodiscard]] CompressionResult<EncodedSegment> CompressSegment(
    const std::span<const std::byte> input,
    const CompressionSettings& settings);

} // namespace pbcompression
