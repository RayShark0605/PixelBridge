#include "local_desktop_resample_fixtures.h"
#include "local_desktop_test_fixtures.h"

#include <catch2/catch_test_macros.hpp>

namespace
{
localdesktoptest::GrayImage MapLevels(const localdesktoptest::GrayImage& source, const std::uint8_t black, const std::uint8_t white)
{
    auto output = source;
    for (std::uint32_t row = 0; row < source.height; row++)
    {
        for (std::uint32_t column = 0; column < source.width; column++)
        {
            const auto value = source.Read(column, row);
            REQUIRE((value == 32 || value == 128 || value == 224));
            output.Write(column, row, value == 32 ? black : value == 224 ? white :
                static_cast<std::uint8_t>((static_cast<std::uint32_t>(black) + white) / 2));
        }
    }
    return output;
}
} // namespace

TEST_CASE("LocalDesktop calibrates current-frame luma ranges that do not straddle the canonical midpoint", "[localdesktop][luma-proposals]")
{
    const auto baseline = localdesktoptest::GrayFromGolden(localdesktoptest::MakeGoldenRaster());
    const auto record = localdesktoptest::LoadGoldenRecord();
    for (const auto levels : std::array<std::array<std::uint8_t, 2>, 4>{{{0, 96}, {159, 255}, {0, 95}, {160, 255}}})
    {
        const auto raster = MapLevels(baseline, levels[0], levels[1]);
        const auto decoded = pbmodulation::DecodeLocalDesktopBootstrap(raster.View());
        INFO("levels=" << static_cast<unsigned int>(levels[0]) << ',' << static_cast<unsigned int>(levels[1])
             << " erasure=" << pbmodulation::GetLocalDesktopErasureName(decoded.erasure) << " work=" << decoded.workUnits);
        if (levels[1] - levels[0] == 96)
        {
            REQUIRE(decoded.IsAccepted());
            CHECK(decoded.canonical44 == record);
            CHECK(decoded.copies[0].canonical44 == record);
            CHECK(decoded.copies[1].canonical44 == record);
            CHECK(decoded.blackLevel == levels[0]);
            CHECK(decoded.whiteLevel == levels[1]);
            CHECK(decoded.markerCandidates == 4);
            CHECK(decoded.geometryCandidates == 1);
        }
        else
        {
            REQUIRE_FALSE(decoded.IsAccepted());
            CHECK(decoded.erasure == pbmodulation::LocalDesktopErasureReason::LowContrast);
        }
    }
}

TEST_CASE("LocalDesktop must examine every luma proposal before deciding the unique image location", "[localdesktop][luma-proposals]")
{
    const auto baseline = localdesktoptest::GrayFromGolden(localdesktoptest::MakeGoldenRaster());
    const auto dark = MapLevels(baseline, 0, 96);
    const auto bright = MapLevels(baseline, 159, 255);
    const auto first = localdesktoptest::Resample(dark, 0.5, 0.5, 0, 0, localdesktoptest::FixtureFilter::Area, 0);
    const auto second = localdesktoptest::Resample(bright, 0.5, 0.5, 0, 0, localdesktoptest::FixtureFilter::Area, 0);
    localdesktoptest::GrayImage combined(1960, 564);
    localdesktoptest::CopyBlock(first, combined, 0, 0, 960, 540, 8, 12);
    localdesktoptest::CopyBlock(second, combined, 0, 0, 960, 540, 992, 12);
    const auto decoded = pbmodulation::DecodeLocalDesktopBootstrap(combined.View());
    REQUIRE_FALSE(decoded.IsAccepted());
    CHECK(decoded.erasure == pbmodulation::LocalDesktopErasureReason::AmbiguousGeometry);
    CHECK(decoded.markerCandidates == 8);
    CHECK(decoded.geometryCandidates == 2);
    CHECK(decoded.canonical44 == std::array<std::byte, 44>{});
}
