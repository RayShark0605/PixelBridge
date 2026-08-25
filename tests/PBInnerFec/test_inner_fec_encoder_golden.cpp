#include "inner_fec_test_helpers.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using namespace pbinnertectest;
using namespace pbinnerfec;

// Known-codeword golden set. The digests were derived with an independent
// reference encoder (separate code path, verified against the explicit
// parity-check matrix H*c == 0 for every case) and pinned here as the
// bit-ordering and encoder golden vectors.
struct EncoderGoldenCase
{
    InnerFecProfileId profileId;
    std::uint32_t kBits;
    const char* allOnesCodewordDigestHex;
    const char* patternCodewordDigestHex;
    const char* randomCodewordDigestHexes[3]; // seeds 0xBEEF1 + v
};

const EncoderGoldenCase kEncoderGoldenCases[] = {
    {kInnerFecProfileIdRobust, 10800,
     "e5785c1cc49b8b43b6ba5702fc31f232c4f2feba82d7547038fc6f685b4a0a26",
     "84d43e6e4fb748df1de5f903f1f2bd77d672001784b40254788a8e93209117a9",
     {"0e1cd7a18bb855cd2fc6fbafe5a251f30ea89c4533600fb2d8acba8abaf48af3",
      "8db643c761c5760ef9029c2a0f357951779577e8a61c85b40c0a1330951cb4a0",
      "6093921c4042576cb37a308568742f665210d4a8aa5e7f8e172784351a1ee9c4"}},
    {kInnerFecProfileIdBalanced, 11880,
     "6a968d3ba6b755c65aff081eb128b0f62e4bb82fa67476b19a75d91adf78200e",
     "515edea20642ff96d7ece9dbcaf877ea135826ad9043736fa46a61ce78714594",
     {"a082b1ea2e3023f662afcdd872b75aa4d90f403e0aee6e26b74149f3116db243",
      "786e359a415e1b85a11b1d34d940a1e6da3de8ad5ed3e71a956909cf735d9ab4",
      "b1c640b7cc5f0ffdd6b81dfbacca6cc059522d76b821943db804b85fb61b72c7"}},
    {kInnerFecProfileIdFast, 13320,
     "659edf1082510790256c74f182a2f80f95e337d6bc51d5901981b8762b45d794",
     "bf428f59e26c7e715f79ae2fae191304c8501b82ea2bf1c5060b36d14650a667",
     {"b9ee57a136a2fd34b3db95680fc80aef4c94ff83d77226acc8bbec6fb27aa367",
      "93712b1dee9cad45cf0acde7d966bf57ec7aa7cac109cbedd4bc2141a67b552b",
      "6e862463f2a4e5fb6308dcd412fba4e940995054a62fa7902450255e3f452f59"}},
};

// BLAKE3-256 of 2025 zero bytes (the all-zero codeword, shared by every
// profile because encoding is systematic and the zero information vector
// has zero row contributions).
constexpr char kZeroCodewordDigestHex[] =
    "970ee4743d7b5327c63cd362a2ad1bf5a81d749c90b20b672042d042af205849";

// First 16 bytes of the bit-wise SplitMix64(0xC0FFEE) information pattern
// (identical for every profile because the first 128 pattern bits do not
// depend on K).
constexpr char kPatternInfoFirst16Hex[] = "e6e5215a64cb2a5199ac6341bd74ad28";

} // namespace

TEST_CASE("InnerFecEncoder zero codeword is the all-zero vector",
    "[innerfec][encoder][golden]")
{
    for (const EncoderGoldenCase& testCase : kEncoderGoldenCases)
    {
        const InnerFecProfile* profile =
            GetInnerFecProfile(testCase.profileId);
        REQUIRE(profile != nullptr);
        const std::vector<std::byte> zeroInfo(
            profile->GetInfoByteCount());
        const std::vector<std::byte> codeword = EncodeCodewordOrDie(
            testCase.profileId, zeroInfo);
        CHECK(codeword.size() == kDvbS2ShortFrameCodewordByteCount);
        for (const std::byte value : codeword)
        {
            CHECK(value == std::byte{0});
        }
        CHECK(ToHex(pbprotocol::ComputeBlake3Digest(codeword)) ==
            kZeroCodewordDigestHex);
        CHECK(ComputeQcLdpcSyndrome(testCase.profileId, codeword).Value());
    }
}

TEST_CASE("InnerFecEncoder all-ones codeword golden vectors",
    "[innerfec][encoder][golden]")
{
    for (const EncoderGoldenCase& testCase : kEncoderGoldenCases)
    {
        const InnerFecProfile* profile =
            GetInnerFecProfile(testCase.profileId);
        REQUIRE(profile != nullptr);
        const std::vector<std::byte> onesInfo(
            profile->GetInfoByteCount(), std::byte{0xFF});
        const std::vector<std::byte> codeword = EncodeCodewordOrDie(
            testCase.profileId, onesInfo);
        CHECK(ToHex(pbprotocol::ComputeBlake3Digest(codeword)) ==
            testCase.allOnesCodewordDigestHex);
        CHECK(ComputeQcLdpcSyndrome(testCase.profileId, codeword).Value());
    }
}

TEST_CASE("InnerFecEncoder pattern codeword golden vectors",
    "[innerfec][encoder][golden]")
{
    for (const EncoderGoldenCase& testCase : kEncoderGoldenCases)
    {
        const InnerFecProfile* profile =
            GetInnerFecProfile(testCase.profileId);
        REQUIRE(profile != nullptr);
        const std::vector<std::byte> info =
            MakePatternInfoBits(testCase.kBits, 0xC0FFEEu);
        CHECK(ToHex(std::span<const std::byte>(info.data(), 16)) ==
            kPatternInfoFirst16Hex);

        const std::vector<std::byte> codeword = EncodeCodewordOrDie(
            testCase.profileId, info);
        // Systematic bit order: the codeword starts with the information.
        CHECK(std::equal(info.begin(), info.end(), codeword.begin()));
        CHECK(ToHex(pbprotocol::ComputeBlake3Digest(codeword)) ==
            testCase.patternCodewordDigestHex);
        CHECK(ComputeQcLdpcSyndrome(testCase.profileId, codeword).Value());
    }
}

TEST_CASE("InnerFecEncoder random codeword golden vectors",
    "[innerfec][encoder][golden]")
{
    for (const EncoderGoldenCase& testCase : kEncoderGoldenCases)
    {
        const InnerFecProfile* profile =
            GetInnerFecProfile(testCase.profileId);
        REQUIRE(profile != nullptr);
        for (std::uint32_t version = 0; version < 3u; version++)
        {
            SplitMix64 random(0xBEEF1u + version);
            const std::vector<std::byte> info = MakeInfoBytes(
                profile->GetInfoByteCount(), random);
            const std::vector<std::byte> codeword = EncodeCodewordOrDie(
                testCase.profileId, info);
            CHECK(ToHex(pbprotocol::ComputeBlake3Digest(codeword)) ==
                testCase.randomCodewordDigestHexes[version]);
            CHECK(ComputeQcLdpcSyndrome(testCase.profileId,
                codeword).Value());
        }
    }
}

TEST_CASE(
    "InnerFecEncoder 1000 random information vectors have zero syndrome on both paths",
    "[innerfec][encoder][syndrome][regression]")
{
    for (const EncoderGoldenCase& testCase : kEncoderGoldenCases)
    {
        const InnerFecProfile* profile =
            GetInnerFecProfile(testCase.profileId);
        REQUIRE(profile != nullptr);
        const std::vector<std::vector<std::uint32_t>> rows =
            BuildExplicitCheckRows(testCase.profileId);
        REQUIRE(rows.size() == profile->parityBits);

        SplitMix64 random(0x100D + testCase.kBits);
        for (std::uint32_t vectorIndex = 0; vectorIndex < 1000;
            vectorIndex++)
        {
            const std::vector<std::byte> info = MakeInfoBytes(
                profile->GetInfoByteCount(), random);
            const std::vector<std::byte> codeword = EncodeCodewordOrDie(
                testCase.profileId, info);
            CHECK(
                ComputeQcLdpcSyndrome(testCase.profileId, codeword).Value());
            CHECK(BruteForceSyndromeZero(codeword, rows));
        }
    }
}

TEST_CASE("InnerFecEncoder is deterministic and systematic",
    "[innerfec][encoder][determinism]")
{
    for (const EncoderGoldenCase& testCase : kEncoderGoldenCases)
    {
        const InnerFecProfile* profile =
            GetInnerFecProfile(testCase.profileId);
        REQUIRE(profile != nullptr);
        SplitMix64 random(0xD43);
        const std::vector<std::byte> info = MakeInfoBytes(
            profile->GetInfoByteCount(), random);
        const std::vector<std::byte> first = EncodeCodewordOrDie(
            testCase.profileId, info);
        const std::vector<std::byte> second = EncodeCodewordOrDie(
            testCase.profileId, info);
        CHECK(first == second);
        CHECK(std::equal(info.begin(), info.end(), first.begin()));
    }
}

TEST_CASE("InnerFecEncoder rejects malformed inputs",
    "[innerfec][encoder][errors]")
{
    for (const EncoderGoldenCase& testCase : kEncoderGoldenCases)
    {
        const InnerFecProfile* profile =
            GetInnerFecProfile(testCase.profileId);
        REQUIRE(profile != nullptr);
        const std::uint32_t infoBytes = profile->GetInfoByteCount();
        const std::uint32_t codewordBytes =
            profile->GetCodewordByteCount();
        const std::vector<std::byte> info(infoBytes);
        std::vector<std::byte> codeword(codewordBytes);

        const auto unknown =
            EncodeQcLdpcCodeword(0x1234ULL, info, codeword);
        CHECK_FALSE(unknown);
        CHECK(unknown.Error().code == InnerFecErrorCode::UnknownProfileId);
        CHECK(unknown.Error().detail == 0x1234ULL);

        std::vector<std::byte> shortInfo(infoBytes - 1u);
        const auto shortResult =
            EncodeQcLdpcCodeword(testCase.profileId, shortInfo, codeword);
        CHECK_FALSE(shortResult);
        CHECK(shortResult.Error().code ==
            InnerFecErrorCode::InvalidInput);
        CHECK(shortResult.Error().detail == infoBytes);

        std::vector<std::byte> longInfo(infoBytes + 1u);
        const auto longResult =
            EncodeQcLdpcCodeword(testCase.profileId, longInfo, codeword);
        CHECK_FALSE(longResult);
        CHECK(longResult.Error().code ==
            InnerFecErrorCode::InvalidInput);

        std::vector<std::byte> shortCodeword(codewordBytes - 1u);
        const auto shortOut =
            EncodeQcLdpcCodeword(testCase.profileId, info, shortCodeword);
        CHECK_FALSE(shortOut);
        CHECK(shortOut.Error().code == InnerFecErrorCode::InvalidInput);
        CHECK(shortOut.Error().detail == codewordBytes);

        std::vector<std::byte> longCodeword(codewordBytes + 1u);
        const auto longOut =
            EncodeQcLdpcCodeword(testCase.profileId, info, longCodeword);
        CHECK_FALSE(longOut);
        CHECK(longOut.Error().code == InnerFecErrorCode::InvalidInput);

        // A valid call succeeds and leaves the codeword written.
        CHECK(EncodeQcLdpcCodeword(testCase.profileId, info, codeword));
    }
}

TEST_CASE(
    "InnerFecEncoder rejects overlapping information and codeword spans",
    "[innerfec][encoder][errors][overlap]")
{
    // Contract: infoBits and codeword must not overlap (the information
    // prefix is copied forward with memcpy semantics, so overlapping
    // ranges would be undefined behavior). The rejection is overlap-based,
    // pinned here for the shifted-window overlap and the exact same-base
    // overlap, with a disjoint control case that must still succeed.
    for (const EncoderGoldenCase& testCase : kEncoderGoldenCases)
    {
        const InnerFecProfile* profile =
            GetInnerFecProfile(testCase.profileId);
        REQUIRE(profile != nullptr);
        const std::uint32_t infoBytes = profile->GetInfoByteCount();
        const std::uint32_t codewordBytes =
            profile->GetCodewordByteCount();

        std::vector<std::byte> buffer(codewordBytes);
        const std::span<std::byte> infoWindow(
            buffer.data() + 100, infoBytes);
        const std::span<std::byte> codewordWindow(
            buffer.data(), codewordBytes);

        const auto shifted =
            EncodeQcLdpcCodeword(testCase.profileId, infoWindow,
                codewordWindow);
        CHECK_FALSE(shifted);
        CHECK(shifted.Error().code ==
            InnerFecErrorCode::InvalidInput);

        const std::span<const std::byte> sameBase(
            buffer.data(), infoBytes);
        const auto sameBaseResult =
            EncodeQcLdpcCodeword(testCase.profileId, sameBase,
                codewordWindow);
        CHECK_FALSE(sameBaseResult);
        CHECK(sameBaseResult.Error().code ==
            InnerFecErrorCode::InvalidInput);

        std::vector<std::byte> info(infoBytes);
        std::vector<std::byte> codeword(codewordBytes);
        CHECK(EncodeQcLdpcCodeword(
            testCase.profileId,
            std::span<const std::byte>(info.data(), info.size()),
            std::span<std::byte>(codeword.data(), codeword.size())));
    }
}
