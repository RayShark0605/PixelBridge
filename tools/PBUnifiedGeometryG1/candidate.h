#pragma once

#include "pbmodulation/local_desktop_decode.h"

#include <cstdint>

namespace pbg1
{
struct CanonicalCandidate
{
    pbmodulation::LocalDesktopObservation bootstrap;
    const char* reason = "NotExamined";
    std::uint32_t sharpEdges = 0;
    std::uint32_t crossingsMeasured = 0;
    std::uint32_t endpointsMeasured = 0;
    std::uint32_t originsMeasured = 0;
    double maximumCrossingRoundoff = 0;
    double maximumEndpointResidual = 0;
    double maximumOriginResidual = 0;
};

// Tool-only experiment. This function receives pixels and policy, never fixture
// truth or an external Bootstrap. It proposes only (0,0,1,1) and has no fallback.
[[nodiscard]] CanonicalCandidate ValidateCanonicalCandidate(const pbmodulation::LumaView& view,
    const pbmodulation::LocalDesktopDecodePolicy& policy = {}) noexcept;
}
