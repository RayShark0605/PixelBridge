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
