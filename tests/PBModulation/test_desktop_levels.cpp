#include "desktop_levels_test_support.h"
#include "pbinterleave/tile_permutation.h"
#include "../../libs/PBModulation/src/desktop_levels_internal.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <sstream>

using namespace pbmodulation;

TEST_CASE("DesktopLevels independent layout and both affine bindings are exact bijections", "[desktop-levels][profile]")
{
    for (const unsigned tile : {2u, 4u})
    {
        const auto positions = desktoptest::Coordinates(tile);
        const auto id = tile == 2 ? kDesktopLevels2ProfileId : kDesktopLevels4ProfileId;
        const auto* profile = GetDesktopLevelsProfile(id);
        REQUIRE(profile != nullptr);
        REQUIRE(profile->tileCount == positions.size());
        REQUIRE(profile->codewords * 2025 + profile->paddingBytes == profile->dataBytes);
        const auto& permutation = *pbinterleave::GetDesktopLevelsPermutation(tile);
        for (std::size_t index = 0; index < positions.size(); index++)
        {
            LocalDesktopRegion region;
            REQUIRE(GetDesktopLevelsTile(id, static_cast<std::uint32_t>(index), region));
            REQUIRE(region.x == positions[index][0]);
            REQUIRE(region.y == positions[index][1]);
        }
        for (std::uint64_t phase = 0; phase < 16; phase++)
        {
            std::vector<bool> seen(positions.size());
            for (std::uint32_t index = 0; index < profile->tileCount; index++)
            {
                const auto physical = permutation.ToPhysical(index, phase);
                REQUIRE(physical < profile->tileCount);
                REQUIRE_FALSE(seen[physical]);
                seen[physical] = true;
                REQUIRE(permutation.ToLogical(physical, phase) == index);
            }
        }
        LocalDesktopRegion sentinel{1, 2, 3, 4};
        REQUIRE_FALSE(GetDesktopLevelsTile(id, profile->tileCount, sentinel));
        CHECK(sentinel == LocalDesktopRegion{1, 2, 3, 4});
        CHECK(permutation.ToPhysical(UINT32_MAX, UINT64_MAX) == profile->tileCount);
        CHECK(permutation.ToLogical(UINT32_MAX, UINT64_MAX) == profile->tileCount);
    }
    CHECK(GetDesktopLevelsProfile(0) == nullptr);
    CHECK(pbinterleave::GetDesktopLevelsPermutation(3) == nullptr);
}

TEST_CASE("DesktopLevels every phase matches independent full-frame pins and decodes independent pixels", "[desktop-levels][golden]")
{
    auto created = DesktopLevelsWorkspace::Create(16 * 1024 * 1024);
    REQUIRE(created);
    auto workspace = std::move(created).Value();
    std::vector<std::byte> hard(kDesktopLevelsMaximumDataBytes + 1, std::byte{0xAC});
    std::vector<float> soft(kDesktopLevelsMaximumBits + 1, 42);
    std::ifstream manifest(std::filesystem::path(PB_DESKTOP_LEVELS_GOLDEN_DIR) / "manifest.txt");
    REQUIRE(manifest);
    std::string line;
    std::getline(manifest, line);
    unsigned checked = 0;
    while (std::getline(manifest, line))
    {
        std::istringstream input(line);
        unsigned tile = 0, phase = 0, tiles = 0, bytes = 0, codewords = 0, padding = 0;
        std::string profileId, dataHash, rasterHash;
        REQUIRE(input >> tile >> phase >> profileId >> tiles >> bytes >> codewords >> padding >> dataHash >> rasterHash);
        INFO("tile=" << tile << " phase=" << phase);
        const auto expectedData = desktoptest::Data(tile, phase);
        const auto expectedPixels = desktoptest::Raster(tile, phase);
        CHECK(desktoptest::Hex(pbprotocol::ComputeBlake3Digest(expectedData)) == dataHash);
        CHECK(desktoptest::Hex(pbprotocol::ComputeBlake3Digest(expectedPixels)) == rasterHash);
        std::vector<std::byte> encoded(expectedPixels.size(), std::byte{0xED});
        REQUIRE(EncodeDesktopLevelsFrame(desktoptest::Record(tile, phase), expectedData, encoded));
        CHECK(encoded == expectedPixels);
        const auto observation = DecodeDesktopLevelsFrame(desktoptest::View(expectedPixels), workspace, hard, soft);
        INFO(GetDesktopLevelsErasureName(observation.erasure) << " bootstrap=" << GetLocalDesktopErasureName(observation.bootstrap.erasure));
        REQUIRE(observation.IsAccepted());
        REQUIRE(observation.dataBytes == expectedData.size());
        CHECK(std::equal(expectedData.begin(), expectedData.end(), hard.begin()));
        for (std::size_t bit = 0; bit < expectedData.size() * 8; bit++)
        {
            CHECK((soft[bit] < 0) == (((std::to_integer<unsigned>(expectedData[bit / 8]) >> (bit % 8)) & 1u) != 0));
        }
        CHECK(hard.back() == std::byte{0xAC});
        CHECK(soft.back() == 42);
        CHECK(observation.margin.minimum == Catch::Approx(1));
        CHECK(observation.margin.samples == tiles);
        CHECK(observation.unreliableTiles == 0);
        CHECK_FALSE(DecodeLocalDesktopBootstrap(desktoptest::View(expectedPixels)).IsAccepted());
        checked++;
    }
    CHECK(checked == 32);
}

TEST_CASE("DesktopLevels direct metric has independently calculated Gray signs midpoint zeros and reliability", "[desktop-levels][metric]")
{
    const DesktopLevelsCalibration calibration{{32, 96, 160, 224}, {}, 64};
    constexpr std::array<std::array<float, 2>, 4> metrics{{{1, 4}, {-1, 1}, {-1, -1}, {1, -4}}};
    constexpr std::array<unsigned, 4> labels{0, 1, 3, 2};
    for (unsigned index = 0; index < 4; index++)
    {
        const auto decision = detail::DecideDesktopLevelsTile(32 + 64 * index, 0, false, calibration);
        CHECK(decision.label == labels[index]);
        CHECK(decision.metrics == metrics[index]);
        CHECK(decision.margin == 1);
        CHECK_FALSE(decision.unreliable);
    }
    for (const double midpoint : {64.0, 128.0, 192.0})
    {
        const auto decision = detail::DecideDesktopLevelsTile(midpoint, 0, false, calibration);
        CHECK(decision.margin == 0);
        CHECK((decision.metrics[0] == 0 || decision.metrics[1] == 0));
        const auto left = detail::DecideDesktopLevelsTile(midpoint - 0.25, 0, false, calibration);
        const auto right = detail::DecideDesktopLevelsTile(midpoint + 0.25, 0, false, calibration);
        CHECK(left.label != right.label);
    }
    CHECK_FALSE(detail::DecideDesktopLevelsTile(96, 256, false, calibration).unreliable);
    CHECK(detail::DecideDesktopLevelsTile(96, std::nextafter(256.0, 300.0), false, calibration).metrics == std::array<float, 2>{0, 0});
    CHECK(detail::DecideDesktopLevelsTile(96, 0, true, calibration).metrics == std::array<float, 2>{0, 0});
}

TEST_CASE("DesktopLevels strict geometry never rounds before checking either scale or phase", "[desktop-levels][geometry]")
{
    LocalDesktopGeometry geometry{12, 13, 1, 1, 0};
    REQUIRE(ValidateDesktopLevelsGeometry(geometry) == DesktopLevelsErasure::None);
    for (const double scale : {0.5, 0.75, 0.999, 1.001, 1.25, 2.0})
    {
        auto changed = geometry;
        changed.scaleX = scale;
        CHECK(ValidateDesktopLevelsGeometry(changed) == DesktopLevelsErasure::ScaleOutOfRange);
        changed = geometry;
        changed.scaleY = scale;
        CHECK(ValidateDesktopLevelsGeometry(changed) == DesktopLevelsErasure::ScaleOutOfRange);
    }
    geometry.originX += 0.125;
    CHECK(ValidateDesktopLevelsGeometry(geometry) == DesktopLevelsErasure::None);
    geometry.originX = std::nextafter(geometry.originX, 14.0);
    CHECK(ValidateDesktopLevelsGeometry(geometry) == DesktopLevelsErasure::AlignmentOutOfRange);
    geometry.originX = std::numeric_limits<double>::quiet_NaN();
    CHECK(ValidateDesktopLevelsGeometry(geometry) == DesktopLevelsErasure::InvalidInput);
    DesktopLevelsDecodePolicy invalid;
    invalid.maximumScaleDriftPixels = std::nextafter(0.125, 1.0);
    CHECK(ValidateDesktopLevelsGeometry({0, 0, 1, 1, 0}, invalid) == DesktopLevelsErasure::InvalidPolicy);
}

TEST_CASE("DesktopLevels max-log distance and histogram quantiles have independent numeric oracles", "[desktop-levels][metric]")
{
    const DesktopLevelsCalibration calibration{{32, 96, 160, 224}, {}, 64};
    const auto decision = detail::DecideDesktopLevelsTile(80, 0, false, calibration);
    // Distances are 2304,256,6400,20736: no production centroid or bit mapping
    // helper participates in these independently calculated expectations.
    CHECK(decision.label == 1);
    CHECK(decision.metrics == std::array<float, 2>{-0.5f, 1.5f});
    CHECK(decision.margin == Catch::Approx(0.8));
    constexpr std::array<unsigned, 3> ties{0, 1, 2};
    constexpr std::array<unsigned, 4> labels{0, 1, 3, 2};
    for (unsigned index = 0; index < 3; index++)
    {
        const double midpoint = 64 + 64 * index;
        CHECK(detail::DecideDesktopLevelsTile(midpoint, 0, false, calibration).label == ties[index]);
        CHECK(detail::DecideDesktopLevelsTile(std::nextafter(midpoint, 0.0), 0, false, calibration).label == labels[index]);
        CHECK(detail::DecideDesktopLevelsTile(std::nextafter(midpoint, 255.0), 0, false, calibration).label == labels[index + 1]);
    }
    std::array<std::uint64_t, 4096> histogram{};
    histogram[0] = 1;
    histogram[10] = 9;
    histogram[2047] = 490;
    histogram[4095] = 500;
    const auto margin = SummarizeDesktopLevelsMargin(histogram, 0);
    CHECK(margin.samples == 1000);
    CHECK(margin.minimum == 0);
    CHECK(margin.p001 == 0);
    CHECK(margin.p01 == 10.0 / 4095);
    CHECK(margin.p50 == 2047.0 / 4095);
    histogram[0] = std::numeric_limits<std::uint64_t>::max();
    CHECK(SummarizeDesktopLevelsMargin(histogram, 0).samples == 0);
    CHECK(SummarizeDesktopLevelsMargin({}, 0).samples == 0);
    CHECK(SummarizeDesktopLevelsMargin(histogram, std::numeric_limits<double>::infinity()).samples == 0);
}
