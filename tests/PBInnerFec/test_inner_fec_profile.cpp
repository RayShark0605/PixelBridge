#include "inner_fec_test_helpers.h"

#include "pbinnerfec/inner_fec_profile.h"
#include "pbprotocol/blake3_digest.h"

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using namespace pbinnertectest;
using namespace pbinnerfec;

// BLAKE3-256 over a NUL-terminated UTF-8 string (identity derivation).
[[nodiscard]] std::array<std::byte, 32> Blake3OfUtf8(const char* text)
{
    const std::size_t length = std::char_traits<char>::length(text);
    return pbprotocol::ComputeBlake3Digest(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(text), length));
}

[[nodiscard]] std::uint64_t FirstEightBytesLittleEndian(
    const std::array<std::byte, 32>& digest) noexcept
{
    std::uint64_t value = 0;
    for (std::uint32_t byteIndex = 0; byteIndex < 8u; byteIndex++)
    {
        value |= static_cast<std::uint64_t>(
            std::to_integer<std::uint8_t>(digest[byteIndex]))
            << (8u * byteIndex);
    }
    return value;
}

struct ProfileCase
{
    InnerFecProfileId profileId;
    InnerFecMatrixId matrixId;
    std::uint32_t kBits;
    std::uint32_t parityBits;
    std::uint32_t qShift;
    std::uint32_t numLines;
    std::size_t serializationBytes;
    const char* pinnedDigestHex;
};

const ProfileCase kCases[] = {
    {kInnerFecProfileIdRobust, kInnerFecMatrixIdRobust,
     10800, 5400, 15, 30, 302,
     "c6d8eabe59d8bc85b2076766d745bca6182f16e019117653121f1e247be34dd3"},
    {kInnerFecProfileIdBalanced, kInnerFecMatrixIdBalanced,
     11880, 4320, 12, 33, 281,
     "2a491986b312158c16bcdf54887acf2872e58d5a2888a41be38bf05cf5342da3"},
    {kInnerFecProfileIdFast, kInnerFecMatrixIdFast,
     13320, 2880, 8, 37, 311,
     "720993e24ae118d569899957c0a8257e97a6950158a2a0d41903e4206e77e76c"},
};

} // namespace

TEST_CASE("InnerFecProfile registry resolves exactly the three frozen profiles",
    "[innerfec][profile]")
{
    for (const ProfileCase& testCase : kCases)
    {
        const InnerFecProfile* profile =
            GetInnerFecProfile(testCase.profileId);
        REQUIRE(profile != nullptr);
        CHECK(profile->profileId == testCase.profileId);
        CHECK(profile->matrixId == testCase.matrixId);
        CHECK(profile->nBits == kDvbS2ShortFrameNBits);
        CHECK(profile->kBits == testCase.kBits);
        CHECK(profile->parityBits == testCase.parityBits);
        CHECK(profile->mGroups == kDvbS2ShortFrameMGroups);
        CHECK(profile->qShift == testCase.qShift);
        CHECK(profile->numLines == testCase.numLines);
        CHECK(profile->systematicBitOrder ==
            static_cast<std::uint8_t>(
                SystematicBitOrder::DvbS2ShortNatural));
        CHECK(profile->puncturingRule ==
            static_cast<std::uint8_t>(PuncturingRule::None));
        CHECK(profile->parityStructure ==
            static_cast<std::uint8_t>(
                ParityStructure::StaircaseDual));
        CHECK(profile->GetInfoByteCount() ==
            testCase.kBits / 8u);
        CHECK(profile->GetCodewordByteCount() ==
            kDvbS2ShortFrameCodewordByteCount);
        CHECK(ValidateInnerFecProfile(*profile));
        // The matrix-id registry resolves to the same profile.
        const InnerFecProfile* byMatrix =
            GetInnerFecProfileByMatrixId(testCase.matrixId);
        CHECK(byMatrix == profile);
    }

    // Fail-closed for unknown identities.
    CHECK(GetInnerFecProfile(0) == nullptr);
    CHECK(GetInnerFecProfile(0xDEADBEEFCAFEULL) == nullptr);
    CHECK(GetInnerFecProfile(kInnerFecProfileIdRobust + 1) == nullptr);
    CHECK(GetInnerFecProfile(kInnerFecProfileIdFast - 1) == nullptr);
    CHECK(GetInnerFecProfileByMatrixId(0) == nullptr);
    CHECK(GetInnerFecProfileByMatrixId(kInnerFecMatrixIdBalanced + 1) ==
        nullptr);

    // The three identities are mutually distinct.
    CHECK(kInnerFecProfileIdRobust != kInnerFecProfileIdBalanced);
    CHECK(kInnerFecProfileIdBalanced != kInnerFecProfileIdFast);
    CHECK(kInnerFecProfileIdRobust != kInnerFecProfileIdFast);
    CHECK(kInnerFecMatrixIdRobust != kInnerFecMatrixIdBalanced);
    CHECK(kInnerFecMatrixIdBalanced != kInnerFecMatrixIdFast);
    CHECK(kInnerFecMatrixIdRobust != kInnerFecMatrixIdFast);
}

TEST_CASE("ValidateInnerFecProfile rejects every field mutation",
    "[innerfec][profile][validation]")
{
    for (const ProfileCase& testCase : kCases)
    {
        const InnerFecProfile* base =
            GetInnerFecProfile(testCase.profileId);
        REQUIRE(base != nullptr);

        InnerFecProfile mutated = *base;
        mutated.nBits += 1;
        CHECK_FALSE(ValidateInnerFecProfile(mutated));

        mutated = *base;
        mutated.kBits += 1;
        CHECK_FALSE(ValidateInnerFecProfile(mutated));

        mutated = *base;
        mutated.parityBits += 1;
        CHECK_FALSE(ValidateInnerFecProfile(mutated));

        mutated = *base;
        mutated.mGroups += 1;
        CHECK_FALSE(ValidateInnerFecProfile(mutated));

        mutated = *base;
        mutated.qShift += 1;
        CHECK_FALSE(ValidateInnerFecProfile(mutated));

        mutated = *base;
        mutated.numLines += 1;
        CHECK_FALSE(ValidateInnerFecProfile(mutated));

        mutated = *base;
        mutated.matrixId += 1;
        CHECK_FALSE(ValidateInnerFecProfile(mutated));

        mutated = *base;
        mutated.profileId += 1;
        CHECK_FALSE(ValidateInnerFecProfile(mutated));

        mutated = *base;
        mutated.systematicBitOrder = 2;
        CHECK_FALSE(ValidateInnerFecProfile(mutated));

        mutated = *base;
        mutated.puncturingRule = 1;
        CHECK_FALSE(ValidateInnerFecProfile(mutated));

        mutated = *base;
        mutated.parityStructure = 1;
        CHECK_FALSE(ValidateInnerFecProfile(mutated));

        mutated = *base;
        mutated.matrixDigest[0] =
            static_cast<std::byte>(std::to_integer<std::uint8_t>(
                mutated.matrixDigest[0]) ^ 0x01u);
        CHECK_FALSE(ValidateInnerFecProfile(mutated));

        mutated = *base;
        mutated.matrixDigest[31] =
            static_cast<std::byte>(std::to_integer<std::uint8_t>(
                mutated.matrixDigest[31]) ^ 0x01u);
        CHECK_FALSE(ValidateInnerFecProfile(mutated));

        // A valid profile still passes after all mutations are reverted.
        CHECK(ValidateInnerFecProfile(*base));
    }
}

TEST_CASE("InnerFecProfile MatrixDigest golden vectors",
    "[innerfec][profile][golden]")
{
    for (const ProfileCase& testCase : kCases)
    {
        const InnerFecProfile* profile =
            GetInnerFecProfile(testCase.profileId);
        REQUIRE(profile != nullptr);
        const std::array<std::byte, 32> computed =
            ComputeInnerFecMatrixDigest(*profile);
        CHECK(ToHex(computed) == testCase.pinnedDigestHex);
        CHECK(profile->matrixDigest == computed);
    }
}

TEST_CASE("InnerFecProfile canonical serialization is deterministic, sized, and versioned",
    "[innerfec][profile][serialization]")
{
    for (const ProfileCase& testCase : kCases)
    {
        const InnerFecProfile* profile =
            GetInnerFecProfile(testCase.profileId);
        REQUIRE(profile != nullptr);

        const std::vector<std::byte> first =
            SerializeInnerFecMatrix(*profile);
        const std::vector<std::byte> second =
            SerializeInnerFecMatrix(*profile);
        REQUIRE(!first.empty());
        CHECK(first.size() == testCase.serializationBytes);
        CHECK(first == second);

        // Pinned layout prefix: magic, version, bit order, puncturing,
        // parity structure, then N and K little-endian.
        CHECK(first[0] == std::byte{'P'});
        CHECK(first[1] == std::byte{'B'});
        CHECK(first[2] == std::byte{'M'});
        CHECK(first[3] == std::byte{'X'});
        CHECK(first[4] == std::byte{1});
        CHECK(first[5] == std::byte{1});
        CHECK(first[6] == std::byte{0});
        CHECK(first[7] == std::byte{0});
        // N = 16200 = 0x3F48 -> 48 3F 00 00
        CHECK(first[8] == std::byte{0x48});
        CHECK(first[9] == std::byte{0x3F});
        CHECK(first[10] == std::byte{0});
        CHECK(first[11] == std::byte{0});
        const std::uint32_t expectedK = testCase.kBits;
        CHECK(first[12] ==
            static_cast<std::byte>(expectedK & 0xFFu));
        CHECK(first[13] ==
            static_cast<std::byte>((expectedK >> 8) & 0xFFu));
        CHECK(first[14] ==
            static_cast<std::byte>((expectedK >> 16) & 0xFFu));
        CHECK(first[15] ==
            static_cast<std::byte>((expectedK >> 24) & 0xFFu));
        const std::uint32_t expectedParity = testCase.parityBits;
        CHECK(first[16] ==
            static_cast<std::byte>(expectedParity & 0xFFu));
        CHECK(first[17] ==
            static_cast<std::byte>((expectedParity >> 8) & 0xFFu));

        // SerializeInnerFecMatrixInto agrees with the vector wrapper.
        std::vector<std::byte> scratch(kMaxInnerFecMatrixSerializationBytes);
        const std::span<const std::byte> filled =
            SerializeInnerFecMatrixInto(*profile, scratch);
        const std::span<const std::byte> reference =
            std::span<const std::byte>(first.data(), first.size());
        REQUIRE(filled.size() == reference.size());
        CHECK(std::equal(filled.begin(), filled.end(), reference.begin()));

        // Too-small output fails closed with an empty span.
        std::vector<std::byte> tiny(16);
        const std::span<const std::byte> rejected =
            SerializeInnerFecMatrixInto(*profile, tiny);
        CHECK(rejected.empty());

        // An unknown K serializes nothing.
        InnerFecProfile unknownK = *profile;
        unknownK.kBits = 14400;
        const std::span<const std::byte> unknownSerialization =
            SerializeInnerFecMatrixInto(unknownK, scratch);
        CHECK(unknownSerialization.empty());
        CHECK(ComputeInnerFecMatrixDigest(unknownK) ==
            std::array<std::byte, 32>{});
    }
}

TEST_CASE(
    "InnerFecProfile identity constants follow the documented BLAKE3 derivation",
    "[innerfec][profile][derivation]")
{
    // The header documents both identity families as the first 8 bytes,
    // read little-endian, of BLAKE3-256 over fixed UTF-8 strings. This
    // pins the auditability rule so a future re-number cannot drift from
    // the documented derivation silently.
    const struct
    {
        const char* profileString = nullptr;
        const char* matrixString = nullptr;
        InnerFecProfileId profileId = 0;
        InnerFecMatrixId matrixId = 0;
    }
    kDerivations[] = {
        {"PixelBridge/InnerFecProfile/DVB-S2-Short-N16200-K10800",
         "PixelBridge/InnerFecMatrix/DVB-S2-Short-N16200-K10800",
         kInnerFecProfileIdRobust, kInnerFecMatrixIdRobust},
        {"PixelBridge/InnerFecProfile/DVB-S2-Short-N16200-K11880",
         "PixelBridge/InnerFecMatrix/DVB-S2-Short-N16200-K11880",
         kInnerFecProfileIdBalanced, kInnerFecMatrixIdBalanced},
        {"PixelBridge/InnerFecProfile/DVB-S2-Short-N16200-K13320",
         "PixelBridge/InnerFecMatrix/DVB-S2-Short-N16200-K13320",
         kInnerFecProfileIdFast, kInnerFecMatrixIdFast},
    };
    for (const auto& testCase : kDerivations)
    {
        CHECK(FirstEightBytesLittleEndian(
            Blake3OfUtf8(testCase.profileString)) ==
            testCase.profileId);
        CHECK(FirstEightBytesLittleEndian(
            Blake3OfUtf8(testCase.matrixString)) ==
            testCase.matrixId);
    }
}
