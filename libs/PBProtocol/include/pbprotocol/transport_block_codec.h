#pragma once

#include "pbprotocol/protocol_result.h"
#include "pbprotocol/protocol_types.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace pbprotocol {

// ---------------------------------------------------------------------------
// Phase-0 provisional Transport Block wire codec (design document 9.4).
//
// Maturity: this is the same Phase-0 provisional implementation slice as the
// descriptor payloads (kDescriptorWireMaturity). The field layout follows the
// design 9.4 hot-path header (fixed 32-byte header, little-endian, uint16
// PayloadBytes), but it is deliberately NOT a v1 wire freeze: the design
// defers exact field-size freezing until after the POC, and the receiver
// local API (pbreceiver::ReceivedTransportBlock) intentionally does not bind
// to this serialized shape. No receiver-side admission path consumes this
// codec yet; it exists for tooling, golden vectors, and the deterministic
// end-to-end decoded-Transport reference chain.
//
// Wire layout (explicit little-endian, no ABI padding):
//
//   offset 0   size 1   BlockType        (1 = Data; only accepted value)
//   offset 1   size 1   ProtocolMinor    (0; unknown minor fails closed)
//   offset 2   size 2   Flags            (reserved, must be 0)
//   offset 4   size 8   SessionTag       (u64)
//   offset 12  size 8   SegmentOrdinal   (u64)
//   offset 20  size 4   OuterBlockId     (u32)
//   offset 24  size 2   PayloadBytes     (u16; 0 is a legal wire object)
//   offset 26  size 2   Reserved         (must be 0)
//   offset 28  size 4   HeaderCrc32c     (CRC-32C over bytes [0, 28),
//                                       every header field before
//                                       the CRC field itself)
//   offset 32  size N   Payload          (N = PayloadBytes)
//   offset 32+N size 4  PayloadCrc32c    (CRC-32C over the payload only)
//
// Wire length = 36 + PayloadBytes, in [36, 65571]. Note 36 + 65535 = 65571,
// which exceeds UINT16_MAX: all size arithmetic here is size_t-checked and
// must never narrow to u16.
// ---------------------------------------------------------------------------

inline constexpr std::size_t kTransportHeaderBytes = 32;
inline constexpr std::size_t kTransportPayloadCrcBytes = 4;
inline constexpr std::size_t kTransportMinimumBlockBytes =
    kTransportHeaderBytes + kTransportPayloadCrcBytes; // 36
// 36 + kMaximumTransportPayloadBytes; intentionally wider than u16.
inline constexpr std::size_t kTransportMaximumBlockBytes =
    kTransportMinimumBlockBytes + kMaximumTransportPayloadBytes; // 65571
static_assert(kTransportMinimumBlockBytes == 36,
    "the provisional transport layout is frozen at a 36-byte minimum");
static_assert(kTransportMaximumBlockBytes == 65571,
    "the provisional transport layout is frozen at a 65571-byte maximum");
static_assert(kTransportMaximumBlockBytes > UINT16_MAX,
    "the maximum wire length must not fit in a u16 (checked arithmetic)");

enum class TransportBlockType : std::uint8_t
{
    Data = 1
};

inline constexpr std::uint8_t kTransportBlockTypeData = 1;
inline constexpr std::uint8_t kTransportProtocolMinor = 0;

// Header field offsets inside the frozen 32-byte header.
inline constexpr std::size_t kTransportBlockTypeOffset = 0;
inline constexpr std::size_t kTransportProtocolMinorOffset = 1;
inline constexpr std::size_t kTransportFlagsOffset = 2;
inline constexpr std::size_t kTransportSessionTagOffset = 4;
inline constexpr std::size_t kTransportSegmentOrdinalOffset = 12;
inline constexpr std::size_t kTransportOuterBlockIdOffset = 20;
inline constexpr std::size_t kTransportPayloadBytesOffset = 24;
inline constexpr std::size_t kTransportReservedOffset = 26;
inline constexpr std::size_t kTransportHeaderCrcOffset = 28;
inline constexpr std::size_t kTransportPayloadOffset = 32;

// Number of header bytes covered by HeaderCrc32c: every header field
// preceding the CRC field itself (a CRC cannot include its own stored
// value).
inline constexpr std::size_t kTransportHeaderCrcCoverageBytes =
    kTransportHeaderCrcOffset;
static_assert(kTransportHeaderCrcCoverageBytes == 28,
    "the header CRC covers exactly the 28 header fields before the CRC");

struct TransportBlockHeader
{
    std::uint8_t blockType = kTransportBlockTypeData;
    std::uint8_t protocolMinor = kTransportProtocolMinor;
    // Reserved for the current provisional layout; must be 0 on the wire.
    std::uint16_t flags = 0;
    SessionTag sessionTag{};
    std::uint64_t segmentOrdinal = 0;
    std::uint32_t outerBlockId = 0;
    // Declared payload length in [0, 65535]. Zero is a legal wire object
    // (an empty-payload block); the receiver policy constraint
    // 1 <= OuterBlockBytes <= 65535 is a policy rule and never enters wire
    // validation.
    std::uint16_t payloadBytes = 0;

    bool operator==(const TransportBlockHeader&) const = default;
};

// The payload view borrows the caller-owned input. It is valid only while
// that storage remains alive and unchanged.
struct TransportBlockView
{
    TransportBlockHeader header{};
    std::span<const std::byte> payload{};

    bool operator==(const TransportBlockView&) const;
};

// 36 + header.payloadBytes (always <= kTransportMaximumBlockBytes because
// payloadBytes is a u16).
[[nodiscard]] constexpr std::size_t GetTransportSerializedSize(
    const TransportBlockHeader& header) noexcept
{
    return kTransportMinimumBlockBytes + header.payloadBytes;
}

// Serializes one Transport Block. The header must be semantically valid
// (blockType 1, minor 0, flags 0) and payload must hold exactly
// header.payloadBytes bytes. output must hold exactly
// GetTransportSerializedSize(header) bytes; any other size fails closed.
// The payload may be empty (payloadBytes = 0); its CRC is then the CRC-32C
// of an empty input. payload and output must not overlap: the header is
// written before the payload is copied, so any intersection is rejected
// fail-closed with OverlappingSpans before a single byte is written.
[[nodiscard]] ProtocolStatus SerializeTransportBlock(
    const TransportBlockHeader& header,
    std::span<const std::byte> payload,
    std::span<std::byte> output) noexcept;

// Strict parse. The input must be exactly 36 + header.payloadBytes bytes:
// trailing bytes and implicit truncation are both rejected (no embedded
// record skipping). Validation order: header presence, field semantics,
// header CRC, exact length, payload CRC. The payload view borrows the input.
[[nodiscard]] ProtocolResult<TransportBlockView> ParseTransportBlock(
    std::span<const std::byte> input) noexcept;

// ---------------------------------------------------------------------------
// Inner-FEC information block framing (design 9.4 zero-padding rule).
//
// Every Transport Block enters a fixed-size Inner-FEC information block.
// When the serialized block is shorter than the information size, the unused
// trailing information bytes are canonical zero padding. The zero padding is
// checked on extraction so a short systematic tail cannot smuggle garbage
// into the codeword (mirrors the orphan-cache canonical-padding contract).
// ---------------------------------------------------------------------------

// Copies the serialized block into the start of outInfoBlock and zero-fills
// the remainder. infoSize must lie in [36, kTransportMaximumBlockBytes] and
// be >= the block size; outInfoBlock must hold exactly infoSize bytes.
// serializedBlock and outInfoBlock must not overlap (memcpy/memset
// semantics): any intersection is rejected fail-closed with OverlappingSpans
// before a single byte is written.
[[nodiscard]] ProtocolStatus FrameTransportBlockIntoInfoBlock(
    std::span<const std::byte> serializedBlock,
    std::size_t infoSize,
    std::span<std::byte> outInfoBlock) noexcept;

// Validates the canonical zero padding after the block and returns the
// block span. input must hold at least 36 bytes; the block length is taken
// from the parsed header, so a corrupt length field fails closed here
// instead of being trusted for a slice.
[[nodiscard]] ProtocolResult<std::span<const std::byte>>
    ExtractTransportBlockFromInfoBlock(
        std::span<const std::byte> infoBlock) noexcept;

} // namespace pbprotocol
