#pragma once
#include "pbmodulation/local_desktop_decode.h"
#include <array>

namespace pbgeomprobe
{
struct Edge
{
    double logical = 0;
    double predicted = 0;
    double perpendicular = 0;
    double black = 0;
    double white = 0;
    double crossing = 0;
    double leftPosition = 0;
    double rightPosition = 0;
    double leftValue = 0;
    double rightValue = 0;
    double before = 0;
    double after = 0;
    bool measured = false;
};

struct Iteration
{
    pbmodulation::LocalDesktopGeometry input;
    pbmodulation::LocalDesktopGeometry fitted;
    std::array<Edge, 48> edges{};
    double movement = 0;
    bool measured = false;
};

struct Probe
{
    const char* reason = "Unmeasured";
    std::array<std::uint32_t, 4> roleCounts{};
    std::array<std::array<double, 8>, 4> markers{};
    pbmodulation::LocalDesktopGeometry seed;
    pbmodulation::LocalDesktopGeometry refined;
    pbmodulation::LocalDesktopGeometry finalGeometry;
    std::array<Iteration, 4> iterations{};
    std::uint32_t iterationCount = 0;
    std::uint64_t locateWork = 0;
    std::uint64_t referenceWork = 0;
    std::uint64_t fitWork = 0;
    std::uint64_t edgeWork = 0;
    bool originalRefineMatched = false;
    bool originalCoverageMatched = false;
    bool evaluationAttempted = false;
    pbmodulation::LocalDesktopObservation evaluation;
};

// No Bootstrap decoding, FEC, candidate geometry admission or payload path.
[[nodiscard]] Probe Inspect(const pbmodulation::LumaView& view) noexcept;
// Separately approved ambiguity audit: enumerate both, never choose by a truth file.
[[nodiscard]] Probe InspectCandidate(const pbmodulation::LumaView& view, std::uint32_t candidateOrdinal) noexcept;
[[nodiscard]] bool SameSeed(const pbmodulation::LocalDesktopGeometry& left, const pbmodulation::LocalDesktopGeometry& right) noexcept;
}
