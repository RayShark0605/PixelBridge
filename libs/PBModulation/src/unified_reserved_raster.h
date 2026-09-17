#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace pbmodulation::detail
{

// Private stores shared by the existing renderer and an explicitly bound
// experiment. The caller has already validated the exact canonical canvas.
// This paints only the frozen calibration/phase regions, never Data/payload.
void RenderUnifiedReservedPilots(std::span<std::byte> pixels, bool grayStates, std::uint64_t frameSequence) noexcept;

}
