#pragma once

#include "pbprotocol/byte_io.h"
#include "pbprotocol/descriptor_codec.h"
#include "pbprotocol/protocol_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace pbprotocol::test {

[[nodiscard]] constexpr std::byte Byte(const std::uint8_t value) noexcept
{
    return static_cast<std::byte>(value);
}

[[nodiscard]] inline SessionId MakeSessionId() noexcept
{
    SessionId sessionId{};
    for (std::size_t byteIndex = 0; byteIndex < sessionId.bytes.size(); byteIndex++)
    {
        sessionId.bytes[byteIndex] = Byte(static_cast<std::uint8_t>(byteIndex));
    }
    return sessionId;
}

[[nodiscard]] inline std::array<std::byte, kDigestBytes> MakeDigestBytes(
    const std::uint8_t firstByte) noexcept
{
    std::array<std::byte, kDigestBytes> bytes{};
    for (std::size_t byteIndex = 0; byteIndex < bytes.size(); byteIndex++)
    {
        bytes[byteIndex] = Byte(static_cast<std::uint8_t>(firstByte + byteIndex));
    }
    return bytes;
}

[[nodiscard]] inline ReceiverResourcePolicy MakeResourcePolicy() noexcept
{
    return ReceiverResourcePolicy{
        std::numeric_limits<std::uint64_t>::max(),
        1024,
        std::numeric_limits<std::uint64_t>::max(),
        std::numeric_limits<std::uint64_t>::max(),
        std::numeric_limits<std::uint32_t>::max(),
        64ULL * 1024ULL * 1024ULL,
        4,
        256ULL * 1024ULL * 1024ULL};
}

[[nodiscard]] inline SessionDescriptor MakeSessionDescriptor(
    const std::uint64_t originalFileSize,
    const std::uint64_t segmentCount) noexcept
{
    return SessionDescriptor{
        GetProtocolVersion(),
        MakeSessionId(),
        originalFileSize,
        segmentCount,
        DigestAlgorithm::Blake3_256};
}

[[nodiscard]] inline WirehairV2SerializedProfile MakeWirehairProfile(
    const std::uint64_t messageBytes = 117,
    const std::uint32_t blockBytes = 16,
    const std::uint8_t seedAttempt = 0) noexcept
{
    WirehairV2SerializedProfile profile{{
        Byte(0x57), Byte(0x48), Byte(0x56), Byte(0x32),
        Byte(0x01), Byte(0x00), Byte(0x20), Byte(0x00),
        Byte(0xC9), Byte(0xF9), Byte(0xF4), Byte(0x47),
        Byte(0xBB), Byte(0x5B), Byte(0x29), Byte(0x4B),
        Byte(0x75), Byte(0x00), Byte(0x00), Byte(0x00),
        Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00),
        Byte(0x10), Byte(0x00), Byte(0x00), Byte(0x00),
        Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00)}};

    for (std::size_t byteIndex = 0; byteIndex < sizeof(messageBytes); byteIndex++)
    {
        profile.bytes[16 + byteIndex] = Byte(static_cast<std::uint8_t>(
            (messageBytes >> static_cast<unsigned int>(byteIndex * 8U)) & 0xFFU));
    }
    for (std::size_t byteIndex = 0; byteIndex < sizeof(blockBytes); byteIndex++)
    {
        profile.bytes[24 + byteIndex] = Byte(static_cast<std::uint8_t>(
            (blockBytes >> static_cast<unsigned int>(byteIndex * 8U)) & 0xFFU));
    }
    profile.bytes[28] = Byte(seedAttempt);
    return profile;
}

[[nodiscard]] inline SegmentDescriptor MakeDirectRepeatSegment(
    const SessionDescriptor& sessionDescriptor,
    const std::uint64_t segmentOrdinal,
    const std::uint64_t rawOffset,
    const std::uint64_t rawSize,
    const std::uint32_t outerBlockBytes = 16) noexcept
{
    const std::array<std::byte, kDigestBytes> digestBytes = MakeDigestBytes(0x20);
    return SegmentDescriptor{
        DeriveSessionTag(sessionDescriptor.sessionId),
        segmentOrdinal,
        rawOffset,
        rawSize,
        rawSize,
        CompressionCodec::Raw,
        OuterFecMode::DirectRepeat,
        outerBlockBytes,
        RawDigest{digestBytes},
        EncodedDigest{digestBytes},
        std::nullopt};
}

[[nodiscard]] inline SegmentDescriptor MakeWirehairSegment(
    const SessionDescriptor& sessionDescriptor,
    const std::uint64_t segmentOrdinal,
    const std::uint64_t rawOffset,
    const std::uint64_t rawSize,
    const std::uint64_t encodedSize = 117,
    const std::uint32_t outerBlockBytes = 16) noexcept
{
    return SegmentDescriptor{
        DeriveSessionTag(sessionDescriptor.sessionId),
        segmentOrdinal,
        rawOffset,
        rawSize,
        encodedSize,
        CompressionCodec::Zstandard,
        OuterFecMode::WirehairV2,
        outerBlockBytes,
        RawDigest{MakeDigestBytes(0x10)},
        EncodedDigest{MakeDigestBytes(0x80)},
        MakeWirehairProfile(encodedSize, outerBlockBytes)};
}

[[nodiscard]] inline FinalManifest MakeFinalManifest(
    const SessionDescriptor& sessionDescriptor,
    const std::uint8_t digestStart = 0xA0) noexcept
{
    WholeFileDigest wholeFileDigest{MakeDigestBytes(digestStart)};
    if (sessionDescriptor.originalFileSize == 0)
    {
        wholeFileDigest = GetEmptyBlake3WholeFileDigest();
    }

    return FinalManifest{
        sessionDescriptor.sessionId,
        sessionDescriptor.originalFileSize,
        sessionDescriptor.segmentCount,
        wholeFileDigest,
        sessionDescriptor.digestAlgorithm};
}

} // namespace pbprotocol::test
