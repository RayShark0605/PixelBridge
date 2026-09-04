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

// Independent integer projection: each canonical 4x4 tile becomes 3x3. The
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

} // namespace pbtest
