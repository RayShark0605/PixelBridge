#include "pbrenderd3d/data_window.h"
#include "presentation_backend.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace
{
using namespace pbrenderd3d;

[[nodiscard]] std::vector<std::byte> MakeCanonicalPixels(const DataWindowConfig& config)
{
    std::vector<std::byte> pixels(static_cast<std::size_t>(config.width) * config.height * 4);
    for (std::uint32_t y = 0; y < config.height; y++)
    {
        for (std::uint32_t x = 0; x < config.width; x++)
        {
            const std::size_t offset = (static_cast<std::size_t>(y) * config.width + x) * 4;
            pixels[offset] = static_cast<std::byte>((x * 17U + y * 3U) & 0xFFU);
            pixels[offset + 1] = static_cast<std::byte>((x * 5U + y * 29U) & 0xFFU);
            pixels[offset + 2] = static_cast<std::byte>((x * 11U + y * 7U) & 0xFFU);
            pixels[offset + 3] = static_cast<std::byte>(dataWindowNeutralMatteAlpha);
        }
    }
    return pixels;
}

[[nodiscard]] std::vector<std::byte> MakeExpectedPointSampledPixels(const DataWindowConfig& config,
    const std::span<const std::byte> canonicalPixels, const std::uint32_t targetWidth,
    const std::uint32_t targetHeight, const PresentationViewportGeometry& viewport)
{
    std::vector<std::byte> pixels(static_cast<std::size_t>(targetWidth) * targetHeight * 4);
    for (std::size_t pixel = 0; pixel < pixels.size(); pixel += 4)
    {
        pixels[pixel] = static_cast<std::byte>(dataWindowNeutralMatteCodeValue);
        pixels[pixel + 1] = static_cast<std::byte>(dataWindowNeutralMatteCodeValue);
        pixels[pixel + 2] = static_cast<std::byte>(dataWindowNeutralMatteCodeValue);
        pixels[pixel + 3] = static_cast<std::byte>(dataWindowNeutralMatteAlpha);
    }
    if (!CanPresentData(viewport.disposition))
    {
        return pixels;
    }
    for (std::uint32_t y = 0; y < targetHeight; y++)
    {
        const double sampleY = static_cast<double>(y) + 0.5;
        if (sampleY < viewport.originY || sampleY >= viewport.originY + viewport.height)
        {
            continue;
        }
        const std::uint32_t sourceY = (std::min)(config.height - 1,
            static_cast<std::uint32_t>(std::floor((sampleY - viewport.originY) * config.height / viewport.height)));
        for (std::uint32_t x = 0; x < targetWidth; x++)
        {
            const double sampleX = static_cast<double>(x) + 0.5;
            if (sampleX < viewport.originX || sampleX >= viewport.originX + viewport.width)
            {
                continue;
            }
            const std::uint32_t sourceX = (std::min)(config.width - 1,
                static_cast<std::uint32_t>(std::floor((sampleX - viewport.originX) * config.width / viewport.width)));
            const std::size_t sourceOffset = (static_cast<std::size_t>(sourceY) * config.width + sourceX) * 4;
            const std::size_t targetOffset = (static_cast<std::size_t>(y) * targetWidth + x) * 4;
            std::copy_n(canonicalPixels.data() + sourceOffset, 4, pixels.data() + targetOffset);
        }
    }
    return pixels;
}

}

TEST_CASE("WARP renders the canonical raster through the production point-sampled letterbox path without a window",
    "[presentation][g13][warp][offscreen]")
{
    DataWindowConfig config;
    const std::vector<std::byte> canonicalPixels = MakeCanonicalPixels(config);

    SECTION("arbitrary aspect ratio keeps one complete immutable 1920x1080 raster")
    {
        constexpr std::uint32_t targetWidth = 2560;
        constexpr std::uint32_t targetHeight = 1600;
        auto rendered = RenderWarpOffscreenForTest(config, canonicalPixels, targetWidth, targetHeight, false);
        REQUIRE(rendered);
        const WarpOffscreenRenderResult& result = rendered.Value();
        REQUIRE(result.viewport == PresentationViewportGeometry{
            PresentationViewportDisposition::Active, 0, 80, 2560, 1440, 4.0 / 3.0});
        REQUIRE(result.diagnostics.warp);
        REQUIRE(result.diagnostics.immutableSourceCreations == 1);
        REQUIRE(result.diagnostics.sourceRendersToBackBuffer == 1);
        REQUIRE(result.diagnostics.neutralMattePresentCalls == 0);
        REQUIRE(result.pixels == MakeExpectedPointSampledPixels(
            config, canonicalPixels, targetWidth, targetHeight, result.viewport));
    }

    SECTION("below 0.75x contains neutral matte and no source raster")
    {
        constexpr std::uint32_t targetWidth = 1439;
        constexpr std::uint32_t targetHeight = 810;
        auto rendered = RenderWarpOffscreenForTest(config, {}, targetWidth, targetHeight, true);
        REQUIRE(rendered);
        const WarpOffscreenRenderResult& result = rendered.Value();
        REQUIRE(result.viewport.disposition == PresentationViewportDisposition::PausedBelowMinimumScale);
        REQUIRE(result.diagnostics.warp);
        REQUIRE(result.diagnostics.immutableSourceCreations == 0);
        REQUIRE(result.diagnostics.sourceRendersToBackBuffer == 0);
        REQUIRE(result.diagnostics.neutralMattePresentCalls == 1);
        REQUIRE(result.pixels == MakeExpectedPointSampledPixels(
            config, {}, targetWidth, targetHeight, result.viewport));
    }
}
