#include "pbprotocol/blake3_digest.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

[[nodiscard]] std::uint8_t HexNibble(const char character)
{
    if (character >= '0' && character <= '9')
    {
        return static_cast<std::uint8_t>(static_cast<unsigned int>(character - '0'));
    }
    return static_cast<std::uint8_t>(
        static_cast<unsigned int>(character - 'a' + 10));
}

// Parses a 64-character lowercase hex string into a fixed digest. A length
// mismatch is a test bug: returning an all-zero digest makes the comparison
// fail loudly instead of reading out of bounds.
[[nodiscard]] std::array<std::byte, pbprotocol::kDigestBytes> ParseHex(
    const std::string_view hexText)
{
    if (hexText.size() != 2 * pbprotocol::kDigestBytes)
    {
        return {};
    }

    std::array<std::byte, pbprotocol::kDigestBytes> digest{};
    for (std::size_t index = 0; index < digest.size(); index++)
    {
        const unsigned int high = HexNibble(hexText[index * 2]);
        const unsigned int low = HexNibble(hexText[index * 2 + 1]);
        digest[index] = static_cast<std::byte>((high << 4) | low);
    }
    return digest;
}

// Deterministic 64 KiB pattern for the streaming equivalence checks.
[[nodiscard]] std::vector<std::byte> MakePattern(const std::size_t byteCount)
{
    std::vector<std::byte> data;
    data.reserve(byteCount);
    for (std::uint32_t index = 0; index < byteCount; index++)
    {
        const std::uint8_t value = static_cast<std::uint8_t>(index * 31u + 7u);
        data.push_back(static_cast<std::byte>(value));
    }
    return data;
}

// Official BLAKE3 vector input: byte i is i modulo 251.
[[nodiscard]] std::vector<std::byte> MakeOfficialVectorInput(
    const std::size_t byteCount)
{
    std::vector<std::byte> data;
    data.reserve(byteCount);
    for (std::size_t index = 0; index < byteCount; index++)
    {
        data.push_back(static_cast<std::byte>(index % 251U));
    }
    return data;
}

[[nodiscard]] std::array<std::byte, pbprotocol::kDigestBytes> ComputeChunked(
    const std::span<const std::byte> data,
    const std::size_t chunkSize)
{
    pbprotocol::Blake3Hasher hasher;
    for (std::size_t offset = 0; offset < data.size(); offset += chunkSize)
    {
        const std::size_t remaining = data.size() - offset;
        const std::size_t count = std::min(chunkSize, remaining);
        hasher.Update(data.subspan(offset, count));
    }
    return hasher.Finalize();
}

} // namespace

static_assert(std::is_nothrow_default_constructible_v<pbprotocol::Blake3Hasher>);
static_assert(std::is_nothrow_destructible_v<pbprotocol::Blake3Hasher>);
static_assert(!std::is_copy_constructible_v<pbprotocol::Blake3Hasher>);
static_assert(!std::is_copy_assignable_v<pbprotocol::Blake3Hasher>);
static_assert(!std::is_move_constructible_v<pbprotocol::Blake3Hasher>);
static_assert(!std::is_move_assignable_v<pbprotocol::Blake3Hasher>);
static_assert(noexcept(pbprotocol::ComputeBlake3Digest({})));

TEST_CASE("BLAKE3-256 matches the official test vectors",
          "[pbprotocol][blake3][kat]")
{
    // Vectors from blake3 1.8.5 test_vectors/test_vectors.json (the pinned
    // vcpkg revision). Inputs are i mod 251 and expected values are the first
    // 32 XOF bytes. Lengths cover compression-block, chunk, and tree edges.
    struct KnownAnswer
    {
        std::size_t inputByteCount;
        std::string_view expectedHex;
    };

    constexpr KnownAnswer knownAnswers[] = {
        {0,
         "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262"},
        {3,
         "e1be4d7a8ab5560aa4199eea339849ba8e293d55ca0a81006726d184519e647f"},
        {63,
         "e9bc37a594daad83be9470df7f7b3798297c3d834ce80ba85d6e207627b7db7b"},
        {64,
         "4eed7141ea4a5cd4b788606bd23f46e212af9cacebacdc7d1f4c6dc7f2511b98"},
        {65,
         "de1e5fa0be70df6d2be8fffd0e99ceaa8eb6e8c93a63f2d8d1c30ecb6b263dee"},
        {1023,
         "10108970eeda3eb932baac1428c7a2163b0e924c9a9e25b35bba72b28f70bd11"},
        {1024,
         "42214739f095a406f3fc83deb889744ac00df831c10daa55189b5d121c855af7"},
        {1025,
         "d00278ae47eb27b34faecf67b4fe263f82d5412916c1ffd97c8cb7fb814b8444"},
        {2048,
         "e776b6028c7cd22a4d0ba182a8bf62205d2ef576467e838ed6f2529b85fba24a"},
        {2049,
         "5f4d72f40d7a5f82b15ca2b2e44b1de3c2ef86c426c95c1af0b6879522563030"},
        {31744,
         "62b6960e1a44bcc1eb1a611a8d6235b6b4b78f32e7abc4fb4c6cdcce94895c47"}};

    for (const KnownAnswer& knownAnswer : knownAnswers)
    {
        CAPTURE(knownAnswer.inputByteCount);
        const std::vector<std::byte> input = MakeOfficialVectorInput(
            knownAnswer.inputByteCount);
        const std::array<std::byte, pbprotocol::kDigestBytes> expected =
            ParseHex(knownAnswer.expectedHex);
        REQUIRE(pbprotocol::ComputeBlake3Digest(input) == expected);
    }
}

TEST_CASE("BLAKE3-256 streaming equals one-shot for arbitrary chunking",
          "[pbprotocol][blake3][streaming]")
{
    const auto pattern = MakePattern(64 * 1024);
    const std::array<std::byte, pbprotocol::kDigestBytes> reference =
        pbprotocol::ComputeBlake3Digest(pattern);

    const std::size_t chunkSizes[] = {
        1U, 2U, 3U, 63U, 64U, 65U, 1000U, 1023U, 1024U, 1025U};
    for (const std::size_t chunkSize : chunkSizes)
    {
        REQUIRE(ComputeChunked(pattern, chunkSize) == reference);
    }

    // Two-half split equals one-shot over the whole buffer.
    const std::size_t half = pattern.size() / 2;
    const std::span<const std::byte> data = pattern;
    pbprotocol::Blake3Hasher hasher;
    hasher.Update(data.subspan(0, half));
    hasher.Update(data.subspan(half));
    REQUIRE(hasher.Finalize() == reference);
}

TEST_CASE("Whole-file digest values are derived from the file bytes",
          "[pbprotocol][blake3][whole-file][integrity]")
{
    std::vector<std::byte> originalBytes = MakeOfficialVectorInput(65);
    const pbprotocol::WholeFileDigest expectedDigest{
        ParseHex(
            "de1e5fa0be70df6d2be8fffd0e99ceaa8eb6e8c93a63f2d8d1c30ecb6b263dee")};
    const pbprotocol::WholeFileDigest computedDigest{
        pbprotocol::ComputeBlake3Digest(originalBytes)};
    REQUIRE(computedDigest == expectedDigest);

    originalBytes[32] ^= std::byte{0x01};
    const pbprotocol::WholeFileDigest mutatedDigest{
        pbprotocol::ComputeBlake3Digest(originalBytes)};
    REQUIRE(mutatedDigest != expectedDigest);
}

TEST_CASE("BLAKE3-256 empty updates are no-ops and Finalize is repeatable",
          "[pbprotocol][blake3][streaming]")
{
    pbprotocol::Blake3Hasher hasher;
    const std::array<std::byte, pbprotocol::kDigestBytes> emptyValue =
        hasher.Finalize();
    REQUIRE(pbprotocol::ComputeBlake3Digest({}) == emptyValue);

    hasher.Update({});
    REQUIRE(hasher.Finalize() == emptyValue);

    // Continuation after Finalize equals one-shot over the concatenation.
    const auto first = MakePattern(1024 + 57);
    const auto second = MakePattern(3 * 1024 - 1);
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
    REQUIRE(hasher.Finalize() == pbprotocol::ComputeBlake3Digest(first));
    hasher.Update(second);
    REQUIRE(hasher.Finalize() == pbprotocol::ComputeBlake3Digest(concatenated));
}
