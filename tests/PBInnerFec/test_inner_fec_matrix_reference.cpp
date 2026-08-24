#include "inner_fec_test_helpers.h"

#include "dvbs2_short_matrix.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using namespace pbinnertectest;
using namespace pbinnerfec;

struct MatrixCase
{
    InnerFecProfileId profileId;
    std::uint32_t kBits;
    std::uint32_t parityBits;
    std::uint32_t qShift;
    std::uint32_t numLines;
    std::size_t totalShiftCount;
    // The first shift of line y equals (firstShiftBase + y) % qShift; a
    // transcription guard derived from the dual-source verified tables.
    std::uint32_t firstShiftBase;
    // Rows of odd total degree in the full (information + staircase)
    // parity-check matrix. Pinned structural fact of the embedded tables;
    // every profile has at least one, which is what makes the all-flipped
    // channel a syndrome failure.
    std::uint32_t oddDegreeRows;
};

const MatrixCase kMatrixCases[] = {
    {kInnerFecProfileIdRobust, 10800, 5400, 15, 30, 120, 0, 1},
    {kInnerFecProfileIdBalanced, 11880, 4320, 12, 33, 108, 3, 2161},
    {kInnerFecProfileIdFast, 13320, 2880, 8, 37, 121, 3, 1081},
};

// Pinned transcription guard: the structured embedded tables, re-flattened
// as [degree, shifts...] per line, must equal the AFF3CT EncValues fixtures
// (verbatim upstream data) value-for-value.
const std::int32_t* GetAff3ctFlat(
    const std::size_t kBits, std::size_t& sizeOut)
{
    if (kBits == 10800)
    {
        sizeOut = kDvbS2ShortAff3ctFlat_Robust.size();
        return kDvbS2ShortAff3ctFlat_Robust.data();
    }
    if (kBits == 11880)
    {
        sizeOut = kDvbS2ShortAff3ctFlat_Balanced.size();
        return kDvbS2ShortAff3ctFlat_Balanced.data();
    }
    if (kBits == 13320)
    {
        sizeOut = kDvbS2ShortAff3ctFlat_Fast.size();
        return kDvbS2ShortAff3ctFlat_Fast.data();
    }
    return nullptr;
}

} // namespace

TEST_CASE(
    "InnerFecMatrix embedded tables re-flatten to the AFF3CT flat fixture",
    "[innerfec][matrix][transcription]")
{
    for (const MatrixCase& testCase : kMatrixCases)
    {
        const DvbS2ShortMatrix* matrix =
            GetDvbS2ShortMatrix(testCase.kBits);
        REQUIRE(matrix != nullptr);
        REQUIRE(matrix->kBits == testCase.kBits);
        REQUIRE(matrix->nBits == kDvbS2ShortFrameNBits);
        REQUIRE(matrix->parityBits == testCase.parityBits);
        REQUIRE(matrix->mGroups == kDvbS2ShortFrameMGroups);
        REQUIRE(matrix->qShift == testCase.qShift);
        REQUIRE(matrix->numLines == testCase.numLines);

        std::size_t flatSize = 0;
        const std::int32_t* flatFixture =
            GetAff3ctFlat(testCase.kBits, flatSize);
        REQUIRE(flatFixture != nullptr);

        std::vector<std::int32_t> reFlattened;
        std::size_t shiftCount = 0;
        for (std::uint32_t lineIndex = 0;
            lineIndex < matrix->numLines; lineIndex++)
        {
            const std::uint8_t degree =
                matrix->lineDegrees[lineIndex];
            REQUIRE(degree >= 1u);
            REQUIRE(degree <= kDvbS2ShortMaxLineDegree);
            const std::uint16_t shiftOffset =
                matrix->lineShiftOffsets[lineIndex];
            if (lineIndex + 1 < matrix->numLines)
            {
                // Offsets must be consistent with the degrees.
                CHECK(matrix->lineShiftOffsets[lineIndex + 1] ==
                    shiftOffset + degree);
            }
            reFlattened.push_back(static_cast<std::int32_t>(degree));
            for (std::uint32_t shiftIndex = 0;
                shiftIndex < degree; shiftIndex++)
            {
                const std::uint16_t shift =
                    matrix->lineShifts[shiftOffset + shiftIndex];
                REQUIRE(shift < matrix->parityBits);
                reFlattened.push_back(shift);
                shiftCount++;
            }
        }
        CHECK(shiftCount == testCase.totalShiftCount);
        CHECK(reFlattened.size() == flatSize);
        CHECK(std::equal(reFlattened.begin(), reFlattened.end(),
            flatFixture));

        // Cyclic first-shift structure: first shift of line y is
        // (base + y) % qShift.
        for (std::uint32_t lineIndex = 0;
            lineIndex < matrix->numLines; lineIndex++)
        {
            const std::uint16_t shiftOffset =
                matrix->lineShiftOffsets[lineIndex];
            const std::uint32_t expectedFirstShift =
                (testCase.firstShiftBase + lineIndex) %
                testCase.qShift;
            CHECK(matrix->lineShifts[shiftOffset] == expectedFirstShift);
        }
    }
}

TEST_CASE(
    "InnerFecMatrix structured syndrome matches brute-force H*c on random vectors",
    "[innerfec][matrix][syndrome]")
{
    for (const MatrixCase& testCase : kMatrixCases)
    {
        const InnerFecProfile* profile =
            GetInnerFecProfile(testCase.profileId);
        REQUIRE(profile != nullptr);
        const std::vector<std::vector<std::uint32_t>> rows =
            BuildExplicitCheckRows(testCase.profileId);
        REQUIRE(rows.size() == testCase.parityBits);

        // Structural pin: the number of odd-degree rows of the full
        // matrix (information lines + staircase parity structure).
        std::uint32_t oddDegreeRows = 0;
        for (const std::vector<std::uint32_t>& row : rows)
        {
            if (row.size() % 2u == 1u)
            {
                oddDegreeRows++;
            }
        }
        CHECK(oddDegreeRows == testCase.oddDegreeRows);

        SplitMix64 random(0xA11CE);
        const std::uint32_t codewordBytes =
            profile->GetCodewordByteCount();
        const std::uint32_t infoBytes = profile->GetInfoByteCount();

        for (std::uint32_t vectorIndex = 0; vectorIndex < 300;
            vectorIndex++)
        {
            std::vector<std::byte> codeword(codewordBytes);
            for (std::uint32_t byteIndex = 0;
                byteIndex < codewordBytes; byteIndex++)
            {
                codeword[byteIndex] = std::byte{
                    std::uint8_t(random.Next() & 0xFFu)};
            }
            const bool structuredValid =
                ComputeQcLdpcSyndrome(testCase.profileId, codeword).Value();
            const bool bruteForceValid =
                BruteForceSyndromeZero(codeword, rows);
            CHECK(structuredValid == bruteForceValid);
            CHECK_FALSE(structuredValid);

            // A corrupted encoded codeword must be rejected by both.
            if (vectorIndex % 10u == 0u)
            {
                const std::vector<std::byte> info =
                    MakeInfoBytes(infoBytes, random);
                const std::vector<std::byte> encoded =
                    EncodeCodewordOrDie(testCase.profileId, info);
                REQUIRE(
                    ComputeQcLdpcSyndrome(testCase.profileId, encoded)
                        .Value());
                REQUIRE(BruteForceSyndromeZero(encoded, rows));
                const std::uint32_t flippedBit =
                    static_cast<std::uint32_t>(
                        random.Next() % kDvbS2ShortFrameNBits);
                std::vector<std::byte> corrupted = encoded;
                SetPackedBit(corrupted, flippedBit, !GetPackedBit(
                    corrupted, flippedBit));
                CHECK_FALSE(
                    ComputeQcLdpcSyndrome(testCase.profileId, corrupted)
                        .Value());
                CHECK_FALSE(BruteForceSyndromeZero(corrupted, rows));
            }
        }

        // All-zero is a codeword; the all-one vector is not (an odd-degree
        // row exists for every profile).
        const std::vector<std::byte> zeroCodeword(codewordBytes);
        CHECK(
            ComputeQcLdpcSyndrome(testCase.profileId, zeroCodeword).Value());
        CHECK(BruteForceSyndromeZero(zeroCodeword, rows));
        const std::vector<std::byte> onesVector(codewordBytes,
            std::byte{0xFF});
        CHECK_FALSE(
            ComputeQcLdpcSyndrome(testCase.profileId, onesVector).Value());
        CHECK_FALSE(BruteForceSyndromeZero(onesVector, rows));
    }
}

TEST_CASE("InnerFecMatrix unit information parity golden vectors",
    "[innerfec][matrix][golden]")
{
    struct UnitCase
    {
        InnerFecProfileId profileId;
        std::uint32_t kBits;
        std::uint32_t bitIndex;
        const char* parityDigestHex;
    };

    const UnitCase kUnitCases[] = {
        {kInnerFecProfileIdRobust, 10800, 0,
         "5d00445a6662048dea4ce385e967035c6aa55d9e114424db9cac0a7bf16bd70d"},
        {kInnerFecProfileIdRobust, 10800, 1,
         "0e5c348d31a9d2e15fe533dc1be235341a25932e71d3c6ba3094a04b9cbd1369"},
        {kInnerFecProfileIdRobust, 10800, 359,
         "9cf76d587c94ed16a2fb3923d651c1d00ad72e94ff8772ebab589b34735060a3"},
        {kInnerFecProfileIdRobust, 10800, 360,
         "74f3efa109a6bfde736cb5b616087f3456aa8fbc219cb4087430b6d56319486d"},
        {kInnerFecProfileIdRobust, 10800, 10799,
         "27d4984b6ce4a8730a19e993e7b6581963ae2c6ca178bb21521f968526e65c53"},
        {kInnerFecProfileIdBalanced, 11880, 0,
         "a4c9a2dbefd0f6883425f4cf1c79412816a9a584e68734a7762c92f8f53af560"},
        {kInnerFecProfileIdBalanced, 11880, 1,
         "e443efa1cc99cac6fb12d6ffb709d1a146e2e56c0506b263935fa9e81bd14406"},
        {kInnerFecProfileIdBalanced, 11880, 359,
         "cd1db735224dbddaea242e5aca07d65eb872f70eef08c172b5421cbe93ccf823"},
        {kInnerFecProfileIdBalanced, 11880, 360,
         "66f802ba0d4a4aed9c2fd0424c3a7f30366f444174b63f45837c332ca6d15604"},
        {kInnerFecProfileIdBalanced, 11880, 11879,
         "5a55257137a960004a8c27ea7671da68ec77cb9070c16a4a127cd56aa6807401"},
        {kInnerFecProfileIdFast, 13320, 0,
         "7bcbe15e7f49e91067e714b5d8ea0410b053921c2958631d789ed008b94395b6"},
        {kInnerFecProfileIdFast, 13320, 1,
         "354fd79a37da44ff43efa47504cdc713d1f84c5fec52578c1dc7bd28ecead52a"},
        {kInnerFecProfileIdFast, 13320, 359,
         "b7ba4f78097dd8b333e7340f592720c1c2f5c58bc23106815881b17b5c505cf2"},
        {kInnerFecProfileIdFast, 13320, 360,
         "a6285bb9ed046c0c8957ce2620746285e55ffb972c27c3cbefbb18594dee5749"},
        {kInnerFecProfileIdFast, 13320, 13319,
         "dce417a3ce01345f59e00e6950852d5e0728df6dc3551f948866be94168a049d"},
    };

    for (const UnitCase& testCase : kUnitCases)
    {
        const InnerFecProfile* profile =
            GetInnerFecProfile(testCase.profileId);
        REQUIRE(profile != nullptr);
        const std::uint32_t infoBytes = profile->GetInfoByteCount();
        std::vector<std::byte> info(infoBytes);
        SetPackedBit(info, testCase.bitIndex, true);

        const std::vector<std::byte> codeword = EncodeCodewordOrDie(
            testCase.profileId, info);
        const std::span<const std::byte> parityBytes =
            std::span<const std::byte>(
                codeword.data() + infoBytes,
                codeword.size() - infoBytes);
        CHECK(ToHex(pbprotocol::ComputeBlake3Digest(parityBytes)) ==
            testCase.parityDigestHex);
        CHECK(ComputeQcLdpcSyndrome(testCase.profileId, codeword).Value());
    }
}

TEST_CASE(
    "InnerFecMatrix all-ones information parity has the pinned structure",
    "[innerfec][matrix][allones]")
{
    struct AllOnesCase
    {
        InnerFecProfileId profileId;
        const char* parityDigestHex;
        // Residue-class oddness mask of the information part: with all
        // information bits set, check row j carries one information edge
        // per shift s with s == j (mod qShift), because each shift, as the
        // within-line index runs 0..359, covers exactly the rows of one
        // full residue class mod qShift. Bit r of the mask is 1 when the
        // total number of shifts in class r is odd (an independently
        // verified structural fact of the dual-source tables).
        std::uint32_t oddResidueMask;
    };

    const AllOnesCase kAllOnesCases[] = {
        {kInnerFecProfileIdRobust,
         "fbe81d01de303374edd0626d6c6cebea1b24ef94091f938ccbc0dcb234dd410d",
         0x0000u},
        {kInnerFecProfileIdBalanced,
         "81255e2b8f60043e14fda9d2964a784d5cd294a8d4c4fc8ea84644b9e88e5174",
         0x056Cu},
        {kInnerFecProfileIdFast,
         "b0b388781031d989a58c1378a9543bdcf18ee6242cb111fd529edc94c19d6680",
         0x00D0u},
    };

    for (const AllOnesCase& testCase : kAllOnesCases)
    {
        const InnerFecProfile* profile =
            GetInnerFecProfile(testCase.profileId);
        REQUIRE(profile != nullptr);
        const std::uint32_t infoBytes = profile->GetInfoByteCount();
        std::vector<std::byte> info(infoBytes, std::byte{0xFF});

        const std::vector<std::byte> codeword = EncodeCodewordOrDie(
            testCase.profileId, info);
        // Systematic: the information prefix is unchanged.
        CHECK(std::equal(info.begin(), info.end(), codeword.begin()));

        std::span<const std::byte> parityBytes =
            std::span<const std::byte>(
                codeword.data() + infoBytes,
                codeword.size() - infoBytes);
        CHECK(ToHex(pbprotocol::ComputeBlake3Digest(parityBytes)) ==
            testCase.parityDigestHex);

        // Structural pin of the parity itself: the all-ones row
        // contribution A[j] depends only on j mod qShift (the mask above),
        // and the staircase encoder chain is P[j] = A[j] XOR P[j-1]
        // (prefix XOR, the ETSI/AFF3CT sub-diagonal staircase), so the
        // expected parity bit is the running prefix XOR of the mask bits.
        std::uint32_t prefixXor = 0;
        for (std::uint32_t parityIndex = 0;
            parityIndex < profile->parityBits; parityIndex++)
        {
            prefixXor ^= (testCase.oddResidueMask >>
                (parityIndex % profile->qShift)) & 1u;
            const bool expectedBit = (prefixXor & 1u) != 0u;
            CHECK(GetPackedBit(parityBytes, parityIndex) == expectedBit);
        }
    }
}
