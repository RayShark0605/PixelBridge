#include "pbprotocol/crc32c.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace {

[[nodiscard]] std::span<const std::byte> AsBytes(
    const std::string_view text) noexcept
{
    return std::as_bytes(std::span(text.data(), text.size()));
}

// Deterministic pattern for the streaming equivalence checks.
[[nodiscard]] std::vector<std::byte> MakePattern(const std::size_t byteCount)
{
    std::vector<std::byte> data;
    data.reserve(byteCount);
    for (std::uint32_t index = 0; index < byteCount; index++)
    {
        const std::uint8_t value = static_cast<std::uint8_t>(index * 7u + 13u);
        data.push_back(static_cast<std::byte>(value));
    }
    return data;
}

[[nodiscard]] std::uint32_t ComputeChunked(
    const std::span<const std::byte> data,
    const std::size_t chunkSize)
{
    pbprotocol::Crc32c hasher;
    for (std::size_t offset = 0; offset < data.size(); offset += chunkSize)
    {
        const std::size_t remaining = data.size() - offset;
        const std::size_t count = std::min(chunkSize, remaining);
        hasher.Update(data.subspan(offset, count));
    }
    return hasher.Finalize();
}

} // namespace

TEST_CASE("CRC-32C matches the published check values",
          "[pbprotocol][crc32c][kat]")
{
    REQUIRE(pbprotocol::ComputeCrc32c({}) == 0x00000000u);
    REQUIRE(pbprotocol::ComputeCrc32c(AsBytes("123456789")) == 0xE3069283u);

    // Single zero byte, verified against an independent bit-level reference.
    const std::array<std::byte, 1> zeroByte{std::byte{0x00}};
    REQUIRE(pbprotocol::ComputeCrc32c(zeroByte) == 0x527D5351u);
}

TEST_CASE("CRC-32C streaming equals one-shot for arbitrary chunking",
          "[pbprotocol][crc32c][streaming]")
{
    const auto pattern = MakePattern(4096);
    const std::uint32_t reference = pbprotocol::ComputeCrc32c(pattern);

    const std::size_t chunkSizes[] = {1u, 2u, 3u, 5u, 7u};
    for (const std::size_t chunkSize : chunkSizes)
    {
        REQUIRE(ComputeChunked(pattern, chunkSize) == reference);
    }
}

TEST_CASE("CRC-32C empty updates are no-ops and Finalize is repeatable",
          "[pbprotocol][crc32c][streaming]")
{
    pbprotocol::Crc32c hasher;
    const std::uint32_t emptyValue = hasher.Finalize();
    REQUIRE(emptyValue == 0x00000000u);

    hasher.Update({});
    REQUIRE(hasher.Finalize() == emptyValue);

    // Continuation after Finalize equals one-shot over the concatenation.
    const auto first = MakePattern(1000);
    const auto second = MakePattern(777);
    std::vector<std::byte> concatenated;
    concatenated.reserve(first.size() + second.size());
    for (const std::byte element : first)
    {
        concatenated.push_back(element);
    }
    for (const std::byte element : second)
    {
        concatenated.push_back(element);
    }

    hasher.Update(first);
    REQUIRE(hasher.Finalize() == pbprotocol::ComputeCrc32c(first));
    hasher.Update(second);
    REQUIRE(hasher.Finalize() == pbprotocol::ComputeCrc32c(concatenated));
}