#pragma once

#include "pbmodulation/shape_chroma.h"

namespace pbmodulation::detail
{

struct ShapeChromaDecision
{
    std::uint8_t label = 0;
    std::array<float, kShapeChromaBitsPerTile> metrics{};
    double shapeMargin = 0;
    double chromaMargin = 0;
    bool shapeUnreliable = false;
    bool chromaUnreliable = false;
};

// Testable fixed baseline decision. Samples are row-major BGRA code values in
// capture space; no sender payload, previous frame, or FEC state is consulted.
[[nodiscard]] ShapeChromaDecision DecideShapeChromaTile(const std::array<std::array<double, 3>, 16>& samples,
    bool clipped, const ShapeChromaCalibration& calibration, const ShapeChromaDecodePolicy& policy) noexcept;

} // namespace pbmodulation::detail
