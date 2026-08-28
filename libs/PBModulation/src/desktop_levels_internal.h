#pragma once

#include "pbmodulation/desktop_levels.h"

namespace pbmodulation::detail
{
struct DesktopLevelsDecision
{
    std::uint8_t label = 0;
    std::array<float, 2> metrics{};
    double margin = 0;
    bool unreliable = false;
};

// Preconditions: finite sample/variance, ordered finite calibrated centroids,
// minimumGap >= 32. No payload oracle or previous-frame state enters decisions.
[[nodiscard]] DesktopLevelsDecision DecideDesktopLevelsTile(double mean, double variance, bool clipped,
                                                            const DesktopLevelsCalibration& calibration) noexcept;
}
