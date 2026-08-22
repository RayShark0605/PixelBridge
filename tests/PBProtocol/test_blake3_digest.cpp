#include "pbprotocol/blake3_digest.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
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

TEST_CASE("BLAKE3-256 matches the official test vectors",
          "[pbprotocol][blake3][kat]")
{
    // Vectors from blake3 1.8.5 test_vectors/test_vectors.json (the pinned
    // vcpkg revision): input bytes are i mod 251 and the expected value is
    // the first 32 bytes of the XOF output. The empty vector also matches
    // the WholeFileDigest golden in test_session_identifiers.cpp.
    const std::array<std::byte, pbprotocol::kDigestBytes> emptyExpected =
        ParseHex(
            "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262");
    REQUIRE(pbprotocol::ComputeBlake3Digest({}) == emptyExpected);

    const std::array<std::byte, pbprotocol::kDigestBytes> threeByteExpected =
        ParseHex(
            "e1be4d7a8ab5560aa4199eea339849ba8e293d55ca0a81006726d184519e647f");
    const std::array<std::byte, 3> threeBytes{
        std::byte{0x00}, std::byte{0x01}, std::byte{0x02}};
    REQUIRE(pbprotocol::ComputeBlake3Digest(threeBytes) == threeByteExpected);

    // Exactly one 64-byte BLAKE3 chunk: exercises the final-chunk path.
    const std::array<std::byte, pbprotocol::kDigestBytes> chunkExpected =
        ParseHex(
            "4eed7141ea4a5cd4b788606bd23f46e212af9cacebacdc7d1f4c6dc7f2511b98");
    std::array<std::byte, 64> chunkInput{};
    for (std::size_t index = 0; index < chunkInput.size(); index++)
    {
        chunkInput[index] = static_cast<std::byte>(index % 251u);
    }
    REQUIRE(pbprotocol::ComputeBlake3Digest(chunkInput) == chunkExpected);
}

TEST_CASE("BLAKE3-256 streaming equals one-shot for arbitrary chunking",
          "[pbprotocol][blake3][streaming]")
{
    const auto pattern = MakePattern(64 * 1024);
    const std::array<std::byte, pbprotocol::kDigestBytes> reference =
        pbprotocol::ComputeBlake3Digest(pattern);

    const std::size_t chunkSizes[] = {1u, 2u, 3u, 1000u};
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