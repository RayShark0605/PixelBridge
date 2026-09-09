#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace pbexperiment
{

inline constexpr std::uint32_t maximumShortRunSeconds = 60;
inline constexpr std::uint32_t maximumComparisonSeconds = 900;
inline constexpr std::size_t maximumSubmittedEvidenceRecords = 14000;
inline constexpr std::size_t maximumSubmittedEvidenceRowBytes = 288;
inline constexpr std::size_t maximumEvidenceFileBytes = 4 * 1024 * 1024;
static_assert(maximumSubmittedEvidenceRecords * maximumSubmittedEvidenceRowBytes < maximumEvidenceFileBytes);

// Separate opt-in budget. Neither mode can extend itself from capture results.
[[nodiscard]] inline std::uint32_t ParseRunSeconds(const std::wstring_view text, const bool comparison)
{
    if (text.empty() || text.size() > 3 || text.front() == L'0')
    {
        throw std::invalid_argument("Duration requires canonical positive decimal without leading zero");
    }
    std::uint32_t seconds = 0;
    for (const auto character : text)
    {
        if (character < L'0' || character > L'9')
        {
            throw std::invalid_argument("Duration must contain only decimal digits");
        }
        seconds = seconds * 10 + static_cast<std::uint32_t>(character - L'0');
    }
    if (seconds < 5 || seconds > (comparison ? maximumComparisonSeconds : maximumShortRunSeconds))
    {
        throw std::invalid_argument(comparison ? "Comparison duration outside 5..900 seconds" : "Short duration outside 5..60 seconds");
    }
    return seconds;
}

} // namespace pbexperiment
