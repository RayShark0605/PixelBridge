#include "pbinnerfec/qc_ldpc_codec.h"
#include "dvbs2_short_matrix.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

TEST_CASE("Pinned QC-LDPC rows always provide a second edge for unique minima", "[innerfec][row-normalization][matrix-invariant]")
{
    for (const auto profileId : {pbinnerfec::kInnerFecProfileIdRobust, pbinnerfec::kInnerFecProfileIdBalanced, pbinnerfec::kInnerFecProfileIdFast})
    {
        const auto* profile = pbinnerfec::GetInnerFecProfile(profileId);
        REQUIRE(profile != nullptr);
        REQUIRE(pbinnerfec::ValidateInnerFecProfile(*profile));
        const auto* matrix = pbinnerfec::GetDvbS2ShortMatrix(profile->kBits);
        REQUIRE(matrix != nullptr);
        std::array<std::uint32_t, pbinnerfec::kDvbS2ShortMaxParityBits> degrees{};
        for (std::uint32_t row = 0; row < matrix->parityBits; row++)
        {
            degrees[row] = row == 0 ? 1 : 2;
        }
        for (std::uint32_t line = 0; line < matrix->numLines; line++)
        {
            for (std::uint32_t withinLine = 0; withinLine < matrix->mGroups; withinLine++)
            {
                for (std::uint32_t shift = 0; shift < matrix->lineDegrees[line]; shift++)
                {
                    const auto row = (matrix->lineShifts[matrix->lineShiftOffsets[line] + shift] + withinLine * matrix->qShift) % matrix->parityBits;
                    degrees[row]++;
                }
            }
        }
        CHECK(std::all_of(degrees.begin(), degrees.begin() + matrix->parityBits, [](const auto degree) { return degree >= 2; }));
    }
}

TEST_CASE("QC-LDPC tied zero and equal magnitudes never normalize an unused sentinel", "[innerfec][row-normalization][ties]")
{
    for (const auto profileId : {pbinnerfec::kInnerFecProfileIdRobust, pbinnerfec::kInnerFecProfileIdBalanced, pbinnerfec::kInnerFecProfileIdFast})
    {
        auto created = pbinnerfec::QcLdpcDecoder::Create(profileId);
        REQUIRE(created);
        auto decoder = std::move(created.Value());
        const auto* profile = decoder.GetProfile();
        REQUIRE(profile != nullptr);
        const std::vector<std::byte> expected(profile->GetCodewordByteCount(), std::byte{0});
        std::vector<std::int16_t> metrics(profile->nBits, 0);
        std::vector<std::byte> output(expected.size(), std::byte{0xa5});
        constexpr pbinnerfec::InnerFecDecodeOptions options{2, 1, 2048, 3, 4};
        const auto zero = decoder.Decode(metrics, options, output);
        REQUIRE(zero);
        CHECK(zero.Value().iterationsUsed == 1);
        CHECK(output == expected);
        std::fill(metrics.begin(), metrics.end(), std::int16_t{32767});
        std::fill(output.begin(), output.end(), std::byte{0xa5});
        const auto equal = decoder.Decode(metrics, options, output);
        REQUIRE(equal);
        CHECK(equal.Value().iterationsUsed == 1);
        CHECK(output == expected);
    }
}
