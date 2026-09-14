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

// Independent center-coordinate reference, without production row reuse or
// column-offset lookup. Used to exercise the real capture/GPU adapter at the
// display sizes covered by the production-composition whole-image oracle.
inline std::vector<std::byte> MakeUnifiedFullscreenPointFixture(const std::span<const std::byte> canonical,
    const std::uint32_t width, const std::uint32_t height)
{
    constexpr std::uint32_t sourceWidth = 1920;
    constexpr std::uint32_t sourceHeight = 1080;
    if (canonical.size() != static_cast<std::size_t>(sourceWidth) * sourceHeight * 4U ||
        width < sourceWidth || width > sourceWidth * 2U || height < sourceHeight || height > sourceHeight * 2U)
    {
        throw std::runtime_error("Fullscreen fixture outside fixed allocation bounds");
    }
    std::vector<std::byte> pixels(static_cast<std::size_t>(width) * height * 4U);
    for (std::uint32_t row = 0; row < height; row++)
    {
        const auto sourceY = static_cast<std::uint32_t>(std::floor((row + 0.5) * sourceHeight / height + 1.0e-10));
        for (std::uint32_t column = 0; column < width; column++)
        {
            const auto sourceX = static_cast<std::uint32_t>(std::floor((column + 0.5) * sourceWidth / width + 1.0e-10));
            const std::size_t sourceOffset = (static_cast<std::size_t>(sourceY) * sourceWidth + sourceX) * 4U;
            const std::size_t offset = (static_cast<std::size_t>(row) * width + column) * 4U;
            std::copy_n(canonical.begin() + sourceOffset, 4U, pixels.begin() + offset);
        }
    }
    return pixels;
}

}
