#include "pbinnerfec/inner_fec_profile.h"
#include "pbinnerfec/qc_ldpc_codec.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

TEST_CASE("Robust Inner FEC preserves all 31 Unified lane slot boundaries", "[inner-fec][unified]")
{
    constexpr std::uint32_t unifiedCodewordCount = 31;
    constexpr std::uint32_t codewordBytes = 2025;
    constexpr std::uint32_t informationBytes = 1350;
    std::array<std::byte, static_cast<std::size_t>(unifiedCodewordCount) * codewordBytes> coded{};
    for (std::uint32_t slot = 0; slot < unifiedCodewordCount; slot++)
    {
        std::array<std::byte, informationBytes> information{};
        for (std::size_t index = 0; index < information.size(); index++)
        {
            information[index] = static_cast<std::byte>((slot * 73 + index * 29 + 17) & 0xFFU);
        }
        const std::span<std::byte> codeword = std::span(coded).subspan(
            static_cast<std::size_t>(slot) * codewordBytes, codewordBytes);
        REQUIRE(pbinnerfec::EncodeQcLdpcCodeword(pbinnerfec::kInnerFecProfileIdRobust, information, codeword));
        REQUIRE(std::equal(information.begin(), information.end(), codeword.begin()));
        const auto syndrome = pbinnerfec::ComputeQcLdpcSyndrome(pbinnerfec::kInnerFecProfileIdRobust, codeword);
        REQUIRE(syndrome);
        REQUIRE(syndrome.Value());
    }
}
