#pragma once

#include "pbouterfec/outer_fec_decoder_resource.h"
#include "pbouterfec/outer_fec_result.h"
#include "pbprotocol/descriptor_binding.h"
#include "pbprotocol/protocol_types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace pbouterfec
{

namespace detail
{
struct DirectRepeatDecoderImplementation;
}

namespace test
{
class DecoderTestAccess;
}

// Phase-0's conservative default keeps K=2 on the DirectRepeat path without
// creating a Wirehair codec. Certified profiles may provide a measured
// threshold, but the choice is made before the SegmentDescriptor is frozen
// and never as a fallback after Wirehair creation fails.
inline constexpr std::uint64_t kDefaultDirectRepeatEfficiencyBlockCount = 2;

struct OuterFecModeSelectionPolicy
{
    std::uint64_t maximumEfficientDirectRepeatBlockCount =
        kDefaultDirectRepeatEfficiencyBlockCount;
};

[[nodiscard]] OuterFecResult<pbprotocol::OuterFecMode> ChooseOuterFecMode(
    std::uint64_t encodedSize,
    std::uint32_t outerBlockBytes,
    const OuterFecModeSelectionPolicy& selectionPolicy = {});

// Owns one stable Encoded Segment copy. A single instance has one owner and
// must not be called concurrently.
class DirectRepeatEncoder
{
public:
    DirectRepeatEncoder(const DirectRepeatEncoder&) = delete;
    DirectRepeatEncoder& operator=(const DirectRepeatEncoder&) = delete;
    DirectRepeatEncoder(DirectRepeatEncoder&& other) noexcept;
    DirectRepeatEncoder& operator=(DirectRepeatEncoder&& other) noexcept;
    ~DirectRepeatEncoder() = default;

    // Empty input is valid and yields DirectBlockCount == 0. The empty-file
    // protocol path emits no SegmentDescriptor and no Data Block.
    [[nodiscard]] static OuterFecResult<DirectRepeatEncoder> Create(
        std::span<const std::byte> encodedSegment,
        std::uint32_t outerBlockBytes);

    // Carousel recreation accepts only exact bytes whose BLAKE3-256 equals the
    // already-bound DirectRepeat SegmentDescriptor::encodedDigest.
    [[nodiscard]] static OuterFecResult<DirectRepeatEncoder> Recreate(
        std::span<const std::byte> exactEncodedSegment,
        const pbprotocol::SegmentDescriptor& segmentDescriptor);

    // paddedOutput must provide at least OuterBlockBytes bytes. On success the
    // first OuterBlockBytes bytes are canonical: real payload followed by zero
    // padding. The returned value is the real PayloadBytes field.
    [[nodiscard]] OuterFecResult<std::uint32_t> EncodeBlock(
        std::uint32_t outerBlockId,
        std::span<std::byte> paddedOutput);

    [[nodiscard]] std::uint64_t GetBlockCount() const noexcept;
    [[nodiscard]] std::uint64_t GetEncodedSize() const noexcept;
    [[nodiscard]] std::uint32_t GetOuterBlockBytes() const noexcept;

private:
    DirectRepeatEncoder() noexcept = default;

    std::vector<std::byte> encodedSegment_;
    std::uint64_t encodedSize_ = 0;
    std::uint64_t blockCount_ = 0;
    std::uint32_t outerBlockBytes_ = 0;
    bool moved_ = false;
};

// Reassembles one descriptor-bound DirectRepeat Segment. DecodeBlock() accepts
// only the fixed OuterBlockBytes payload region after upstream Descriptor,
// Inner-FEC, and Transport-CRC validation. It revalidates PayloadBytes and
// canonical zero padding before mutating state. A single instance has one
// owner and must not be called concurrently.
class DirectRepeatDecoder
{
public:
    DirectRepeatDecoder(const DirectRepeatDecoder&) = delete;
    DirectRepeatDecoder& operator=(const DirectRepeatDecoder&) = delete;
    DirectRepeatDecoder(DirectRepeatDecoder&& other) noexcept;
    DirectRepeatDecoder& operator=(DirectRepeatDecoder&& other) noexcept;
    ~DirectRepeatDecoder();

    [[nodiscard]] static OuterFecResult<DirectRepeatDecoder> Create(
        const pbprotocol::BoundSegmentDescriptor& boundSegmentDescriptor,
        std::uint32_t expectedOuterBlockBytes,
        const OuterFecDecoderResourceManager& resourceManager);

    [[nodiscard]] OuterFecResult<DecodeDisposition> DecodeBlock(
        std::uint32_t outerBlockId,
        std::uint32_t payloadBytes,
        std::span<const std::byte> paddedPayload);

    [[nodiscard]] OuterFecResult<std::uint64_t> Recover(
        std::span<std::byte> output);

    // The descriptor-derived block count the decoder was bound to at Create
    // time, for cross-checking replay sources that persist their own block
    // count (resume.state records, design doc section 31.3). Returns 0 for an
    // empty (moved-from) decoder.
    [[nodiscard]] std::uint64_t GetBoundBlockCount() const noexcept;
    // Telemetry/admission introspection only. This is the number of distinct
    // OuterBlockIds retained by the decoder; it does not change decode or wire
    // semantics and returns zero for an empty decoder.
    [[nodiscard]] std::uint64_t GetAcceptedBlockCount() const noexcept;

private:
    friend class test::DecoderTestAccess;

    DirectRepeatDecoder() noexcept = default;

    [[nodiscard]] static OuterFecResult<DirectRepeatDecoder>
    CreateFromDescriptor(
        const pbprotocol::SegmentDescriptor& segmentDescriptor,
        std::uint32_t expectedOuterBlockBytes,
        const OuterFecDecoderResourceManager& resourceManager);
    [[nodiscard]] static OuterFecResult<DirectRepeatDecoder>
    CreateFromDescriptor(
        const pbprotocol::SegmentDescriptor& segmentDescriptor,
        std::uint32_t expectedOuterBlockBytes,
        const OuterFecDecoderResourceManager& resourceManager,
        bool forceAllocationFailureAfterReservation);

    std::unique_ptr<detail::DirectRepeatDecoderImplementation> implementation_;
};

} // namespace pbouterfec
