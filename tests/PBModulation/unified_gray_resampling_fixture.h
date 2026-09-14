#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace pbtest
{

inline std::vector<std::byte> MakeUnifiedGrayResampledFixture(const std::span<const std::byte> canonical,
    const int scalePermille, const bool linear, const int originFractionPermille)
{
    constexpr int sourceWidth = 1920;
    constexpr int sourceHeight = 1080;
    constexpr int targetWidth = 2560;
    constexpr int targetHeight = 1440;
    if (canonical.size() != static_cast<std::size_t>(sourceWidth) * sourceHeight * 4 ||
        scalePermille < 700 || scalePermille > 1250 || originFractionPermille < 0 || originFractionPermille > 999)
    {
        throw std::runtime_error("Transform fixture shape or scale outside fixed allocation bound");
    }
    const double scale = static_cast<double>(scalePermille) / 1000.0;
    const double originX = 47.0 + static_cast<double>(originFractionPermille) / 1000.0;
    const double originY = 31.0 + static_cast<double>(originFractionPermille) / 1000.0;
    const auto Read = [canonical](const int column, const int row, const std::size_t channel) -> double
    {
        if (column < 0 || row < 0 || column >= sourceWidth || row >= sourceHeight)
        {
            return 0.0;
        }
        return std::to_integer<unsigned int>(canonical[(static_cast<std::size_t>(row) * sourceWidth + column) * 4 + channel]);
    };
    std::vector<std::byte> result(static_cast<std::size_t>(targetWidth) * targetHeight * 4);
    for (int row = 0; row < targetHeight; row++)
    {
        const double logicalY = (row + 0.5 - originY) / scale - 0.5;
        const int top = static_cast<int>(std::floor(logicalY));
        const double fractionY = logicalY - top;
        for (int column = 0; column < targetWidth; column++)
        {
            const double logicalX = (column + 0.5 - originX) / scale - 0.5;
            const int left = static_cast<int>(std::floor(logicalX));
            const double fractionX = logicalX - left;
            const std::size_t offset = (static_cast<std::size_t>(row) * targetWidth + column) * 4;
            for (std::size_t channel = 0; channel < 3; channel++)
            {
                const double value = linear ?
                    Read(left, top, channel) * (1 - fractionX) * (1 - fractionY) +
                    Read(left + 1, top, channel) * fractionX * (1 - fractionY) +
                    Read(left, top + 1, channel) * (1 - fractionX) * fractionY +
                    Read(left + 1, top + 1, channel) * fractionX * fractionY :
                    Read(static_cast<int>(std::floor(logicalX + 0.5)), static_cast<int>(std::floor(logicalY + 0.5)), channel);
                result[offset + channel] = static_cast<std::byte>(static_cast<unsigned int>(std::clamp(std::round(value), 0.0, 255.0)));
            }
            result[offset + 3] = std::byte{255};
        }
    }
    return result;
}

} // namespace pbtest
