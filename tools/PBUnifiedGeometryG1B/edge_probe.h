#pragma once

#include "pbmodulation/local_desktop_decode.h"

#include <array>
#include <cstdint>

namespace pbg1b
{
struct EdgeReading
{
    double logical = 0;
    double crossing = 0;
    double before = 0;
    double after = 0;
    double black = 0;
    double white = 0;
    bool crossingMeasured = false;
    bool endpointsMeasured = false;
};

struct EdgeProbe
{
    const char* reason = "Unmeasured";
    std::array<EdgeReading, 48> edges{};
    pbmodulation::LocalDesktopGeometry firstFit{};
    std::array<double, 4> markerResiduals{};
    std::uint32_t markersVerified = 0;
    std::uint32_t crossingsMeasured = 0;
    std::uint32_t endpointsMeasured = 0;
    std::uint64_t workUnits = 0;
    bool firstFitMeasured = false;
};

// Read-only instrument: there is deliberately no Bootstrap/payload result or
// alternative oracle entry. Canonical coordinates are a hypothesis, not proof.
[[nodiscard]] EdgeProbe InspectCanonicalEdges(const pbmodulation::LumaView& view,
    const pbmodulation::LocalDesktopDecodePolicy& policy = {}) noexcept;
}
