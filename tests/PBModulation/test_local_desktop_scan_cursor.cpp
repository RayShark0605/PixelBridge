#include "local_desktop_test_fixtures.h"
#include "pbmodulation/local_desktop_decode.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

TEST_CASE("BGRA marker cursor honors odd width unaligned rows and exact work boundary", "[localdesktop][scan-cursor][budget][pitch]")
{
    const auto canonical = localdesktoptest::MakeGoldenRaster();
    const auto expected = localdesktoptest::LoadGoldenRecord();
    constexpr std::uint32_t width = 1923;
    constexpr std::uint32_t height = 1080;
    constexpr std::size_t rowPitch = width * 4 + 13;
    constexpr std::size_t footprint = (height - 1) * rowPitch + width * 4;
    std::vector<std::byte> storage(footprint + 1, std::byte{0xcd});
    for (std::size_t row = 0; row < height; row++)
    {
        std::copy_n(canonical.begin() + row * 7680, 7680, storage.begin() + 1 + row * rowPitch);
        std::fill_n(storage.begin() + 1 + row * rowPitch + 7680, 12, std::byte{128});
    }
    const auto before = storage;
    const auto pixels = std::span<const std::byte>(storage).subspan(1);
    const pbmodulation::LumaView view{pixels, width, height, rowPitch, pbmodulation::LumaPixelFormat::Bgra8};
    const auto observation = pbmodulation::DecodeLocalDesktopBootstrap(view);
    REQUIRE(observation.IsAccepted());
    CHECK(observation.canonical44 == expected);
    REQUIRE(observation.workUnits > 1);
    pbmodulation::LocalDesktopDecodePolicy policy;
    policy.maximumWorkUnits = observation.workUnits;
    const auto exact = pbmodulation::DecodeLocalDesktopBootstrap(view, policy);
    REQUIRE(exact.IsAccepted());
    CHECK(exact.canonical44 == expected);
    CHECK(exact.workUnits == observation.workUnits);
    policy.maximumWorkUnits--;
    const auto exhausted = pbmodulation::DecodeLocalDesktopBootstrap(view, policy);
    CHECK(exhausted.erasure == pbmodulation::LocalDesktopErasureReason::WorkBudgetExceeded);
    CHECK(exhausted.workUnits <= policy.maximumWorkUnits);
    CHECK(exhausted.canonical44 == decltype(exhausted.canonical44){});
    CHECK(exhausted.quality == 0);
    const pbmodulation::LumaView shortView{pixels.first(pixels.size() - 1), width, height, rowPitch, pbmodulation::LumaPixelFormat::Bgra8};
    const auto invalid = pbmodulation::DecodeLocalDesktopBootstrap(shortView);
    CHECK(invalid.erasure == pbmodulation::LocalDesktopErasureReason::InvalidView);
    CHECK(invalid.workUnits == 0);
    CHECK(storage == before);
}

TEST_CASE("BGRA marker cursor scans every threshold before rejecting two visible frames", "[localdesktop][scan-cursor][ambiguity]")
{
    const auto canonical = localdesktoptest::MakeGoldenRaster();
    constexpr std::size_t rowPitch = 3840 * 4;
    std::vector<std::byte> pixels(rowPitch * 1080);
    for (std::size_t row = 0; row < 1080; row++)
    {
        std::copy_n(canonical.begin() + row * 7680, 7680, pixels.begin() + row * rowPitch);
        std::copy_n(canonical.begin() + row * 7680, 7680, pixels.begin() + row * rowPitch + 7680);
        // The second frame has legal contrast but neither black nor white
        // crosses 128; its markers must still be found in the threshold-64 pass.
        for (std::size_t column = 1920; column < 3840; column++)
        {
            for (std::size_t channel = 0; channel < 3; channel++)
            {
                auto& value = pixels[row * rowPitch + column * 4 + channel];
                value = value == std::byte{32} ? std::byte{8} : value == std::byte{224} ? std::byte{120} : std::byte{64};
            }
        }
    }
    const pbmodulation::LumaView view{pixels, 3840, 1080, rowPitch, pbmodulation::LumaPixelFormat::Bgra8};
    const auto observation = pbmodulation::DecodeLocalDesktopBootstrap(view);
    REQUIRE(observation.erasure == pbmodulation::LocalDesktopErasureReason::AmbiguousGeometry);
    CHECK(observation.markerCandidates == 8);
    CHECK(observation.geometryCandidates == 2);
    CHECK(observation.canonical44 == decltype(observation.canonical44){});
    CHECK(observation.quality == 0);
}
