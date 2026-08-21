#include "pbprotocol/protocol_types.h"

#include <blake3.h>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace pbprotocol {

namespace {

constexpr std::string_view kSessionTagDomain = "PixelBridge SessionTag v1";

[[nodiscard]] std::array<std::byte, kDigestBytes> ComputeBlake3(
    const std::span<const std::byte> first,
    const std::span<const std::byte> second = {}) noexcept
{
    blake3_hasher hasher{};
    blake3_hasher_init(&hasher);
    if (!first.empty())
    {
        blake3_hasher_update(&hasher, first.data(), first.size());
    }
    if (!second.empty())
    {
        blake3_hasher_update(&hasher, second.data(), second.size());
    }

    std::array<std::byte, kDigestBytes> digest{};
    blake3_hasher_finalize(
        &hasher,
        reinterpret_cast<std::uint8_t*>(digest.data()),
        digest.size());
    return digest;
}

} // namespace

SessionTag DeriveSessionTag(const SessionId& sessionId) noexcept
{
    const auto domainBytes = std::as_bytes(std::span(kSessionTagDomain));
    const std::array<std::byte, kDigestBytes> digest = ComputeBlake3(
        domainBytes,
        sessionId.bytes);

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
    return WholeFileDigest{ComputeBlake3({})};
}

} // namespace pbprotocol
