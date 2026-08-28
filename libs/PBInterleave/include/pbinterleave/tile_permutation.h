#pragma once

#include <cstdint>

namespace pbinterleave
{

namespace detail
{
// Callers supply frozen, validated dimensions. uint64 intermediates are
// required even when every input and the final index fit uint32.
[[nodiscard]] constexpr std::uint64_t ApplyAffineTile(const std::uint64_t index, const std::uint64_t count,
                                                     const std::uint64_t multiplier, const std::uint64_t shift) noexcept
{
    return (index * multiplier + shift) % count;
}
}

struct DesktopLevelsPermutation
{
    std::uint32_t tilePixels;
    std::uint32_t tileCount;
    std::uint32_t rowTiles;

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return (tilePixels == 2 && tileCount == 346752 && rowTiles == 864) || (tilePixels == 4 && tileCount == 86688 && rowTiles == 432);
    }

    [[nodiscard]] constexpr std::uint32_t ToPhysical(const std::uint32_t logical, const std::uint64_t sequence) const noexcept
    {
        return !IsValid() || logical >= tileCount ? tileCount : static_cast<std::uint32_t>(detail::ApplyAffineTile(logical, tileCount, 65537, (sequence % 16) * rowTiles));
    }
    [[nodiscard]] constexpr std::uint32_t ToLogical(const std::uint32_t physical, const std::uint64_t sequence) const noexcept
    {
        if (!IsValid() || physical >= tileCount)
        {
            return tileCount;
        }
        const std::uint64_t unshifted = (physical + static_cast<std::uint64_t>(tileCount) - (sequence % 16) * rowTiles) % tileCount;
        return static_cast<std::uint32_t>(detail::ApplyAffineTile(unshifted, tileCount, 29825, 0));
    }
};

inline constexpr DesktopLevelsPermutation kDesktopLevelsPermutation2{2, 346752, 864};
inline constexpr DesktopLevelsPermutation kDesktopLevelsPermutation4{4, 86688, 432};
static_assert((65537ull * 29825) % 346752 == 1 && (65537ull * 29825) % 86688 == 1);
static_assert(15 * 864 < 346752 && 15 * 432 < 86688);

[[nodiscard]] constexpr const DesktopLevelsPermutation* GetDesktopLevelsPermutation(const std::uint32_t tilePixels) noexcept
{
    return tilePixels == 2 ? &kDesktopLevelsPermutation2 : tilePixels == 4 ? &kDesktopLevelsPermutation4 : nullptr;
}

} // namespace pbinterleave
