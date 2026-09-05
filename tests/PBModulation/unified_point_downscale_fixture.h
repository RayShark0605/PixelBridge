#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace pbtest
{

// Independent integer projection: each four canonical pixels become three. The
// center source coordinate is exactly on a texel boundary, so exercise both
// legal tie directions on each axis rather than copying receiver arithmetic.
inline std::vector<std::byte> DownscaleUnifiedPoint(const std::span<const std::byte> source,
    const bool upperHorizontalTie, const bool upperVerticalTie)
{
    if (source.size() != 1920U * 1080U * 4U)
    {
        throw std::invalid_argument("point fixture requires the canonical Unified raster");
    }
    const std::array<std::uint32_t, 3> columns{0, upperHorizontalTie ? 2U : 1U, 3};
    const std::array<std::uint32_t, 3> rows{0, upperVerticalTie ? 2U : 1U, 3};
    std::vector<std::byte> result(1440U * 810U * 4U);
    for (std::uint32_t row = 0; row < 810; row++)
    {
        for (std::uint32_t column = 0; column < 1440; column++)
        {
            const std::uint32_t sourceX = column / 3 * 4 + columns[column % 3];
            const std::uint32_t sourceY = row / 3 * 4 + rows[row % 3];
            std::copy_n(source.begin() + (static_cast<std::size_t>(sourceY) * 1920 + sourceX) * 4, 4,
                result.begin() + (static_cast<std::size_t>(row) * 1440 + column) * 4);
        }
    }
    return result;
}

// At 9/8 scale every ninth destination center lands exactly on a source
// boundary. Integer division independently selects either legal point tie.
inline std::vector<std::byte> UpscaleUnifiedPoint(const std::span<const std::byte> source,
    const bool upperHorizontalTie, const bool upperVerticalTie)
{
    if (source.size() != 1920U * 1080U * 4U)
    {
        throw std::invalid_argument("point fixture requires the canonical Unified raster");
    }
    std::vector<std::byte> result(2160U * 1215U * 4U);
    for (std::uint32_t row = 0; row < 1215; row++)
    {
        const std::uint32_t sourceY = ((2 * row + 1) * 4 - (upperVerticalTie ? 0U : 1U)) / 9;
        for (std::uint32_t column = 0; column < 2160; column++)
        {
            const std::uint32_t sourceX = ((2 * column + 1) * 4 - (upperHorizontalTie ? 0U : 1U)) / 9;
            std::copy_n(source.begin() + (static_cast<std::size_t>(sourceY) * 1920 + sourceX) * 4, 4,
                result.begin() + (static_cast<std::size_t>(row) * 2160 + column) * 4);
        }
    }
    return result;
}

} // namespace pbtest
