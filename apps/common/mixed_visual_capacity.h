#pragma once

#include "pbmodulation/experimental_pam4_wide.h"
#include <cstddef>

namespace pbapp
{

// Application-owned mixed-frame storage only. The Unified/Gray wire and
// spatial-interleave bounds remain eighteen; count alone never selects PAM4.
inline constexpr std::size_t maximumMixedFrameSlotCount = pbmodulation::kExperimentalPam4WideCodewordCount;
inline constexpr std::size_t maximumMixedCodedFrameBytes = maximumMixedFrameSlotCount * pbmodulation::kUnifiedCodewordBytes;
static_assert(maximumMixedFrameSlotCount > pbmodulation::kUnifiedMaximumFrameSlotCount);
static_assert(maximumMixedFrameSlotCount <= 256);

}
