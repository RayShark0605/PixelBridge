#pragma once

#include "pbmodulation/unified_visual.h"

#include <array>
#include <cstdint>
#include <vector>

namespace pbg1
{
struct PixelFrame
{
    std::uint32_t width = pbmodulation::kUnifiedVisualProfile.canvasWidth;
    std::uint32_t height = pbmodulation::kUnifiedVisualProfile.canvasHeight;
    std::vector<std::byte> pixels;
    [[nodiscard]] pbmodulation::LumaView View() const noexcept;
};

struct Fixture
{
    PixelFrame frame;
    std::array<std::vector<std::byte>, pbmodulation::kUnifiedCodewordCount> expectedBlocks;
};

void Require(bool condition, const char* message);
[[nodiscard]] Fixture BuildFixture(std::uint64_t sequence);
[[nodiscard]] PixelFrame CropEdge(const PixelFrame& source, std::uint32_t edge);
[[nodiscard]] PixelFrame PadEdge(const PixelFrame& cropped, std::uint32_t edge);
[[nodiscard]] PixelFrame OffsetIntensity(const PixelFrame& source);
[[nodiscard]] PixelFrame BlurAxis(const PixelFrame& source, bool horizontal);
[[nodiscard]] PixelFrame DownscalePoint(const PixelFrame& source);
[[nodiscard]] PixelFrame UpscalePoint(const PixelFrame& source);
[[nodiscard]] PixelFrame ReplaceRegion(const PixelFrame& source, const PixelFrame& donor, const pbmodulation::LocalDesktopRegion& region);
[[nodiscard]] PixelFrame PerturbMarkerEdge(const PixelFrame& source);
[[nodiscard]] PixelFrame EraseBootstrap(const PixelFrame& source);
}
