#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace pbmodulation::band_detail
{
// Full-length RS(255,223) codec for the supplemental control bands. The
// product Bootstrap codec (bootstrap_rs.cpp) is the same mathematics over a
// 179-symbol shortened prefix; the supplemental band transmits all 255
// symbols, so the shortening bookkeeping degenerates to the constants below.
inline constexpr std::size_t kBandRsRecordBytes = 223;
inline constexpr std::size_t kBandRsParityBytes = 32;
inline constexpr std::size_t kBandRsCodewordBytes = 255;
inline constexpr std::uint16_t kBandRsFieldPolynomial = 0x11D;
inline constexpr std::uint32_t kBandRsFullSymbols = 255;
inline constexpr std::uint32_t kBandRsMaximumErrors = 16;

enum class BandRsError : std::uint8_t
{
    None,
    InvalidInputSize,
    InvalidOutputSize,
    LocatorDegree,
    LocatorRootCount,
    MagnitudeFailure,
    SyndromeMismatch
};

struct BandRsStatus
{
    BandRsError error = BandRsError::None;
    std::uint32_t correctedSymbols = 0;
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return error == BandRsError::None;
    }
};

[[nodiscard]] BandRsStatus EncodeBandRs(std::span<const std::byte> record, std::span<std::byte> codeword) noexcept;
[[nodiscard]] BandRsStatus DecodeBandRs(std::span<const std::byte> codeword, std::span<std::byte> record) noexcept;
[[nodiscard]] std::uint8_t MultiplyBandField(std::uint8_t left, std::uint8_t right) noexcept;
} // namespace pbmodulation::band_detail
