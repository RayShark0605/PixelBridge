#pragma once

#include <cstddef>
#include <optional>
#include <span>

namespace pbrenderd3d
{

struct HardwareAdapterCandidate
{
    bool software = false;
    bool matchesMonitorOutput = false;
};

struct HardwareAdapterSelection
{
    std::size_t index = 0;
    bool boundToMonitorOutput = false;
};

// The caller must finish native enumeration successfully before permitting
// an unmapped candidate. A failed/limited walk is not evidence of absence.
[[nodiscard]] inline std::optional<HardwareAdapterSelection> SelectHardwarePresentationAdapter(
    const std::span<const HardwareAdapterCandidate> candidates, const bool allowUnmapped, const bool enumerationComplete) noexcept
{
    for (std::size_t index = 0; index < candidates.size(); index++)
    {
        if (candidates[index].matchesMonitorOutput)
        {
            return candidates[index].software ? std::nullopt : std::optional{HardwareAdapterSelection{index, true}};
        }
    }
    if (allowUnmapped && enumerationComplete)
    {
        for (std::size_t index = 0; index < candidates.size(); index++)
        {
            if (!candidates[index].software)
            {
                return HardwareAdapterSelection{index, false};
            }
        }
    }
    return std::nullopt;
}

} // namespace pbrenderd3d
