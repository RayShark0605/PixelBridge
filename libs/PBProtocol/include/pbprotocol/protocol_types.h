#pragma once

#include "pbprotocol/protocol_version.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace pbprotocol {

inline constexpr std::size_t kSessionIdBytes = 16;
inline constexpr std::size_t kDigestBytes = 32;
inline constexpr std::size_t kWirehairV2SerializedProfileBytes = 32;
// TransportBlock::PayloadBytes is a provisional little-endian uint16 field.
// OuterBlockBytes describes that same fixed payload region, so values above
// this limit cannot be represented without changing the wire format.
inline constexpr std::uint32_t kMaximumTransportPayloadBytes = 65535U;
inline constexpr std::uint64_t kMaximumRepresentableDirectRepeatBlockCount =
    static_cast<std::uint64_t>(UINT32_MAX) + 1ULL;

struct SessionId
{
    std::array<std::byte, kSessionIdBytes> bytes{};

    bool operator==(const SessionId&) const = default;
};

struct SessionTag
{
    // Compact deterministic routing metadata. It is neither secret nor sender
    // authentication, and collisions must be handled by SessionId binding.
    std::uint64_t value = 0;

    bool operator==(const SessionTag&) const = default;
};

struct RawDigest
{
    std::array<std::byte, kDigestBytes> bytes{};

    bool operator==(const RawDigest&) const = default;
};

struct EncodedDigest
{
    std::array<std::byte, kDigestBytes> bytes{};

    bool operator==(const EncodedDigest&) const = default;
};

struct WholeFileDigest
{
    // Unkeyed BLAKE3-256 integrity metadata. When received in-band with the
    // payload it does not authenticate the sender. Ordinary equality is not
    // an authentication verifier; a future MAC or signature must use a
    // distinct type and verification API.
    std::array<std::byte, kDigestBytes> bytes{};

    bool operator==(const WholeFileDigest&) const = default;
};

struct WirehairV2SerializedProfile
{
    std::array<std::byte, kWirehairV2SerializedProfileBytes> bytes{};

    bool operator==(const WirehairV2SerializedProfile&) const = default;
};

enum class DigestAlgorithm : std::uint8_t
{
    Blake3_256 = 1
};

enum class CompressionCodec : std::uint8_t
{
    Raw = 1,
    Zstandard = 2
};

enum class OuterFecMode : std::uint8_t
{
    WirehairV2 = 1,
    DirectRepeat = 2
};

struct SessionDescriptor
{
    ProtocolVersion protocolVersion{};
    SessionId sessionId{};
    std::uint64_t originalFileSize = 0;
    std::uint64_t segmentCount = 0;
    DigestAlgorithm digestAlgorithm = static_cast<DigestAlgorithm>(0);

    bool operator==(const SessionDescriptor&) const = default;
};

struct SegmentDescriptor
{
    SessionTag sessionTag{};
    std::uint64_t segmentOrdinal = 0;
    std::uint64_t rawOffset = 0;
    std::uint64_t rawSize = 0;
    std::uint64_t encodedSize = 0;
    CompressionCodec compressionCodec = static_cast<CompressionCodec>(0);
    OuterFecMode outerFecMode = static_cast<OuterFecMode>(0);
    std::uint32_t outerBlockBytes = 0;
    RawDigest rawDigest{};
    EncodedDigest encodedDigest{};
    std::optional<WirehairV2SerializedProfile> wirehairV2SerializedProfile;

    bool operator==(const SegmentDescriptor&) const = default;
};

struct FinalManifest
{
    SessionId sessionId{};
    std::uint64_t originalFileSize = 0;
    std::uint64_t segmentCount = 0;
    WholeFileDigest wholeFileDigest{};
    DigestAlgorithm digestAlgorithm = static_cast<DigestAlgorithm>(0);

    bool operator==(const FinalManifest&) const = default;
};

struct ReceiverResourcePolicy
{
    // A user-declared constructor deliberately keeps this safety policy from
    // being a positional aggregate. Adding a required quota must make stale
    // initializers fail at compile time instead of silently zero-initializing
    // the new field and failing later at runtime.
    ReceiverResourcePolicy() noexcept = default;

    std::uint64_t maxAcceptedFileBytes = 0;
    std::uint64_t maxSegmentCount = 0;
    std::uint64_t maxRawSegmentBytes = 0;
    std::uint64_t maxEncodedSegmentBytes = 0;
    std::uint32_t maxOuterBlockBytes = 0;
    // Both descriptor-state limits cover dynamic storage owned by Segment Map
    // containers. The fixed state object is bounded separately by
    // maxConcurrentSessions.
    std::uint64_t maxDescriptorStateBytes = 0;
    std::uint64_t maxConcurrentSessions = 0;
    std::uint64_t maxTotalDescriptorStateBytes = 0;
    // Decoder admission reserves a conservative charge before constructing
    // either wrapper state or the third-party codec. These are receiver-local
    // limits and never enter a serialized descriptor.
    std::uint64_t maxDirectRepeatBlockCount = 0;
    std::uint64_t maxActiveOuterFecDecoders = 0;
    std::uint64_t maxOuterFecDecoderBytes = 0;
    std::uint64_t maxTotalOuterFecDecoderBytes = 0;

    bool operator==(const ReceiverResourcePolicy&) const = default;
};

// This is a conservative local Phase-0 receiver policy, not a wire constant or
// a certified throughput profile. Products may choose stricter validated
// values, but must not replace these finite budgets with unbounded sentinels.
[[nodiscard]] ReceiverResourcePolicy GetDefaultReceiverResourcePolicy() noexcept;
[[nodiscard]] SessionTag DeriveSessionTag(const SessionId& sessionId) noexcept;
[[nodiscard]] WholeFileDigest GetEmptyBlake3WholeFileDigest() noexcept;

} // namespace pbprotocol
