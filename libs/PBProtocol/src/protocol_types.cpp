#include "pbprotocol/protocol_types.h"

#include "pbprotocol/blake3_digest.h"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace pbprotocol {

namespace {

constexpr std::string_view kSessionTagDomain = "PixelBridge SessionTag v1";

} // namespace

ReceiverResourcePolicy GetDefaultReceiverResourcePolicy() noexcept
{
    constexpr std::uint64_t kibibyte = 1024ULL;
    constexpr std::uint64_t mebibyte = 1024ULL * kibibyte;
    constexpr std::uint64_t gibibyte = 1024ULL * mebibyte;

    return ReceiverResourcePolicy{
        500ULL * gibibyte,
        65536ULL,
        16ULL * mebibyte,
        32ULL * mebibyte,
        65535U,
        64ULL * mebibyte,
        4ULL,
        256ULL * mebibyte};
}

SessionTag DeriveSessionTag(const SessionId& sessionId) noexcept
{
    const auto domainBytes = std::as_bytes(std::span(kSessionTagDomain));
    Blake3Hasher hasher;
    hasher.Update(domainBytes);
    hasher.Update(sessionId.bytes);
    const std::array<std::byte, kDigestBytes> digest = hasher.Finalize();

    std::uint64_t tagValue = 0;
    for (std::size_t byteIndex = 0; byteIndex < sizeof(tagValue); byteIndex++)
    {
        tagValue |= static_cast<std::uint64_t>(
            std::to_integer<std::uint8_t>(digest[byteIndex]))
            << static_cast<unsigned int>(byteIndex * 8U);
    }

    return SessionTag{tagValue};
}

WholeFileDigest GetEmptyBlake3WholeFileDigest() noexcept
{
    return WholeFileDigest{ComputeBlake3Digest({})};
}

} // namespace pbprotocol
