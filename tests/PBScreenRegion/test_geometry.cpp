#include "screen_region_internal.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <iomanip>
#include <limits>
#include <locale>
#include <random>
#include <sstream>
#include <string>

using namespace pbscreenregion;
using namespace pbscreenregion::detail;

TEST_CASE("PMv2 manifest priority list cannot be replaced by thread-only DPI awareness", "[screen-region][dpi]")
{
    CHECK(DeclaresPerMonitorV2(L"PerMonitorV2"));
    CHECK(DeclaresPerMonitorV2(L" future-mode, PeRmOnItOrV2 , permonitor"));
    CHECK(DeclaresPerMonitorV2(L"\t\r\nPerMonitorV2\n"));
    CHECK_FALSE(DeclaresPerMonitorV2(L"system, PerMonitorV2"));
    CHECK_FALSE(DeclaresPerMonitorV2(L"unaware, PerMonitorV2"));
    CHECK_FALSE(DeclaresPerMonitorV2(L"permonitor, PerMonitorV2"));
    CHECK_FALSE(DeclaresPerMonitorV2(L""));
    CHECK_FALSE(DeclaresPerMonitorV2(L", , unknown-mode"));
    CHECK_FALSE(DeclaresPerMonitorV2(L"PerMonitorV2junk"));
}

namespace
{
HMONITOR Handle(const std::uintptr_t value)
{
    return reinterpret_cast<HMONITOR>(value);
}

MonitorMetadata Monitor(const std::uintptr_t identity, const RECT rect, const UINT dpi = 96, const DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_IDENTITY)
{
    return {Handle(identity), rect, dpi, dpi, rotation, {}};
}

bool SameRegion(const ScreenCaptureRegion& first, const ScreenCaptureRegion& second)
{
    return first.monitor == second.monitor && EqualRect(first.physicalRect, second.physicalRect) &&
           EqualRect(first.monitorPhysicalRect, second.monitorPhysicalRect) && first.dpiX == second.dpiX && first.dpiY == second.dpiY &&
           first.rotation == second.rotation;
}

ScreenCaptureRegion Sentinel()
{
    return {Handle(0x1234), {-3, -2, 4, 5}, {-100, -100, 1920, 1080}, 120, 120, DXGI_MODE_ROTATION_ROTATE90};
}
} // namespace

TEST_CASE("Physical ROI is invariant across four DPI scales and all DXGI rotations", "[screen-region][dpi]")
{
    const std::array<UINT, 4> dpiValues{96, 120, 144, 168};
    const std::array<DXGI_MODE_ROTATION, 4> rotations{DXGI_MODE_ROTATION_IDENTITY, DXGI_MODE_ROTATION_ROTATE90, DXGI_MODE_ROTATION_ROTATE180,
                                                      DXGI_MODE_ROTATION_ROTATE270};
    const std::array<std::array<POINT, 2>, 4> drags{
        std::array<POINT, 2>{POINT{-2400, -1300}, POINT{-2201, -1201}}, std::array<POINT, 2>{POINT{-2201, -1201}, POINT{-2400, -1300}},
        std::array<POINT, 2>{POINT{-2400, -1201}, POINT{-2201, -1300}}, std::array<POINT, 2>{POINT{-2201, -1300}, POINT{-2400, -1201}}};
    for (const UINT dpi : dpiValues)
    {
        for (const auto rotation : rotations)
        {
            MonitorSnapshot snapshot;
            snapshot.count = 3;
            snapshot.monitors[0] = Monitor(1, {-2560, -1440, 0, 0}, dpi, rotation);
            snapshot.monitors[1] = Monitor(2, {0, 0, 1920, 1080}, 168);
            snapshot.monitors[2] = Monitor(3, {1920, -300, 3000, 1620}, 120, DXGI_MODE_ROTATION_ROTATE90);
            for (const auto& drag : drags)
            {
                INFO("dpi=" << dpi << " rotation=" << rotation);
                RECT rect{};
                REQUIRE(BuildDragRect(drag[0], drag[1], rect));
                // Independent literal oracle: endpoints are inclusive pixels,
                // output RECT is half-open, with no DPI or rotation transform.
                REQUIRE(EqualRect(rect, RECT{-2400, -1300, -2200, -1200}));
                ScreenCaptureRegion region;
                REQUIRE(ResolveFromTopology(snapshot, rect, region));
                CHECK(region.monitor == Handle(1));
                CHECK(EqualRect(region.physicalRect, RECT{-2400, -1300, -2200, -1200}));
                CHECK(EqualRect(region.monitorPhysicalRect, RECT{-2560, -1440, 0, 0}));
                CHECK(region.dpiX == dpi);
                CHECK(region.dpiY == dpi);
                CHECK(region.rotation == rotation);
                CHECK(static_cast<std::int64_t>(region.physicalRect.left) - region.monitorPhysicalRect.left == 160);
                CHECK(static_cast<std::int64_t>(region.physicalRect.top) - region.monitorPhysicalRect.top == 140);
                CHECK(static_cast<std::int64_t>(region.physicalRect.right) - region.monitorPhysicalRect.left == 360);
                CHECK(static_cast<std::int64_t>(region.physicalRect.bottom) - region.monitorPhysicalRect.top == 240);
            }
        }
    }
}

TEST_CASE("Single-monitor admission is full containment rather than nearest or largest intersection", "[screen-region][bounds]")
{
    MonitorSnapshot snapshot;
    snapshot.count = 3;
    snapshot.monitors[0] = Monitor(1, {-1920, 0, 0, 1080}, 120);
    snapshot.monitors[1] = Monitor(2, {0, -1080, 1920, 0}, 144);
    snapshot.monitors[2] = Monitor(3, {200, 200, 2120, 1280}, 168);
    const std::array<RECT, 5> accepted{{{-1920, 0, 0, 1080}, {-1, 1079, 0, 1080}, {0, -1080, 1, -1079}, {200, 200, 201, 201}, {2119, 1279, 2120, 1280}}};
    for (const RECT rect : accepted)
    {
        ScreenCaptureRegion output;
        REQUIRE(ResolveFromTopology(snapshot, rect, output));
        CHECK(EqualRect(output.physicalRect, rect));
    }
    const std::array<RECT, 8> rejected{{{-1920, 0, 1, 1080},
                                        {-1, -1, 1, 1},
                                        {0, 0, 1, 1},
                                        {100, 100, 200, 200},
                                        {-3000, 20, -2990, 30},
                                        {-1921, 1, -1910, 10},
                                        {200, 200, 2121, 1280},
                                        {-1900, -1000, 1900, 1000}}};
    for (const RECT rect : rejected)
    {
        auto output = Sentinel();
        const auto status = ResolveFromTopology(snapshot, rect, output);
        CHECK(status == ScreenRegionStatus::Failure(ScreenRegionErrorCode::NotSingleMonitor, ScreenRegionStage::Geometry));
        CHECK(SameRegion(output, Sentinel()));
    }
    snapshot.monitors[1] = Monitor(2, snapshot.monitors[0].physicalRect);
    auto output = Sentinel();
    CHECK(ResolveFromTopology(snapshot, RECT{-10, 10, 0, 20}, output).code == ScreenRegionErrorCode::AmbiguousMonitor);
    CHECK(SameRegion(output, Sentinel()));
}

TEST_CASE("Drag inclusive endpoints cover the final physical row and column", "[screen-region][bounds]")
{
    RECT rect{};
    REQUIRE(BuildDragRect(POINT{-1920, -1080}, POINT{-1, -1}, rect));
    CHECK(EqualRect(rect, RECT{-1920, -1080, 0, 0}));
    REQUIRE(BuildDragRect(POINT{-1, -1}, POINT{-1, -1}, rect));
    CHECK(EqualRect(rect, RECT{-1, -1, 0, 0}));
    REQUIRE(BuildDragRect(POINT{40000, -40000}, POINT{40002, -39998}, rect));
    CHECK(EqualRect(rect, RECT{40000, -40000, 40003, -39997}));
}

TEST_CASE("Rectangle and endpoint overflow fail before narrowing and preserve output", "[screen-region][overflow]")
{
    constexpr LONG minimum = (std::numeric_limits<LONG>::min)();
    constexpr LONG maximum = (std::numeric_limits<LONG>::max)();
    const std::array<RECT, 8> invalid{{{0, 0, 0, 1},
                                       {0, 0, 1, 0},
                                       {1, 0, 0, 1},
                                       {0, 1, 1, 0},
                                       {minimum, 0, 0, 1},
                                       {0, minimum, 1, 0},
                                       {minimum, minimum, maximum, maximum},
                                       {-1, 0, maximum, 1}}};
    for (const RECT rect : invalid)
    {
        CHECK(ValidatePhysicalRect(rect) == ScreenRegionStatus::Failure(ScreenRegionErrorCode::InvalidRectangle, ScreenRegionStage::Geometry));
    }
    CHECK(ValidatePhysicalRect(RECT{minimum, minimum, minimum + 1, minimum + 1}));
    CHECK(ValidatePhysicalRect(RECT{maximum - 1, maximum - 1, maximum, maximum}));
    CHECK(ValidatePhysicalRect(RECT{minimum, 0, -1, 1}));
    const std::array<std::array<POINT, 2>, 5> invalidDrags{
        std::array<POINT, 2>{POINT{0, 0}, POINT{maximum, 1}}, std::array<POINT, 2>{POINT{0, 0}, POINT{1, maximum}},
        std::array<POINT, 2>{POINT{minimum, 0}, POINT{0, 1}}, std::array<POINT, 2>{POINT{0, minimum}, POINT{1, 0}},
        std::array<POINT, 2>{POINT{minimum, minimum}, POINT{maximum, maximum}}};
    for (const auto& drag : invalidDrags)
    {
        RECT output{1, 2, 3, 4};
        CHECK(BuildDragRect(drag[0], drag[1], output).code == ScreenRegionErrorCode::InvalidRectangle);
        CHECK(EqualRect(output, RECT{1, 2, 3, 4}));
    }
}

TEST_CASE("Metadata and bounded topology admission never substitute defaults", "[screen-region][metadata]")
{
    MonitorSnapshot snapshot;
    CHECK(ValidateTopology(snapshot).code == ScreenRegionErrorCode::MetadataUnavailable);
    snapshot.count = maximumMonitors;
    for (std::size_t index = 0; index < maximumMonitors; index++)
    {
        const auto left = static_cast<LONG>(index * 20);
        snapshot.monitors[index] = Monitor(index + 1, {left, 0, left + 20, 20}, 192);
    }
    REQUIRE(ValidateTopology(snapshot));
    ScreenCaptureRegion output;
    REQUIRE(ResolveFromTopology(snapshot, RECT{1260, 0, 1280, 20}, output));
    CHECK(output.monitor == Handle(64));
    snapshot.count = 65;
    CHECK(ValidateTopology(snapshot) == ScreenRegionStatus::Failure(ScreenRegionErrorCode::ResourceLimit, ScreenRegionStage::MonitorEnumeration));
    snapshot.count = 1;
    for (const UINT dpi : {0u, 1u, 96u, 120u, 144u, 168u, 192u, 240u})
    {
        snapshot.monitors[0].dpiX = dpi;
        snapshot.monitors[0].dpiY = dpi;
        CHECK(static_cast<bool>(ValidateTopology(snapshot)) == (dpi != 0));
    }
    snapshot.monitors[0].dpiX = 96;
    snapshot.monitors[0].dpiY = 0;
    CHECK(ValidateTopology(snapshot).stage == ScreenRegionStage::DpiQuery);
    snapshot.monitors[0].dpiY = 96;
    for (const auto rotation : {DXGI_MODE_ROTATION_UNSPECIFIED, static_cast<DXGI_MODE_ROTATION>(5), static_cast<DXGI_MODE_ROTATION>(999)})
    {
        snapshot.monitors[0].rotation = rotation;
        CHECK(ValidateTopology(snapshot) == ScreenRegionStatus::Failure(ScreenRegionErrorCode::MetadataUnavailable, ScreenRegionStage::OutputQuery));
    }
    snapshot.monitors[0].rotation = DXGI_MODE_ROTATION_IDENTITY;
    snapshot.monitors[0].monitor = nullptr;
    CHECK(ValidateTopology(snapshot).stage == ScreenRegionStage::Geometry);
    snapshot.monitors[0].monitor = Handle(2);
    snapshot.count = 2;
    CHECK(ValidateTopology(snapshot).code == ScreenRegionErrorCode::AmbiguousMonitor);
}

TEST_CASE("Topology equality is order independent and identity and metadata sensitive", "[screen-region][metadata]")
{
    MonitorSnapshot first;
    first.count = 2;
    first.monitors[0] = Monitor(1, {-100, -100, 0, 0});
    first.monitors[1] = Monitor(2, {0, 0, 100, 100}, 120, DXGI_MODE_ROTATION_ROTATE270);
    auto second = first;
    std::swap(second.monitors[0], second.monitors[1]);
    CHECK(EqualTopology(first, second));
    for (int mutation = 0; mutation < 7; mutation++)
    {
        second = first;
        switch (mutation)
        {
        case 0:
            second.count = 1;
            break;
        case 1:
            second.monitors[0].monitor = Handle(3);
            break;
        case 2:
            second.monitors[0].physicalRect.left--;
            break;
        case 3:
            second.monitors[0].dpiX++;
            break;
        case 4:
            second.monitors[0].dpiY++;
            break;
        case 5:
            second.monitors[0].rotation = DXGI_MODE_ROTATION_ROTATE180;
            break;
        case 6:
            second.monitors[0].deviceName[0] = L'X';
            break;
        }
        CHECK_FALSE(EqualTopology(first, second));
    }
    second = first;
    second.count = 65;
    CHECK_FALSE(EqualTopology(first, second));
}

TEST_CASE("Fixed-seed pixel membership oracle agrees with monitor admission", "[screen-region][differential]")
{
    MonitorSnapshot snapshot;
    snapshot.count = 3;
    snapshot.monitors[0] = Monitor(1, {-100, -80, 0, 40}, 120);
    snapshot.monitors[1] = Monitor(2, {0, -20, 100, 80}, 144, DXGI_MODE_ROTATION_ROTATE90);
    snapshot.monitors[2] = Monitor(3, {130, 0, 200, 100}, 168, DXGI_MODE_ROTATION_ROTATE270);
    std::mt19937_64 random(0x504253435245454Eull);
    std::size_t accepted = 0;
    std::size_t rejected = 0;
    for (unsigned int iteration = 0; iteration < 6000; iteration++)
    {
        const LONG left = static_cast<LONG>(random() % 360) - 140;
        const LONG top = static_cast<LONG>(random() % 240) - 120;
        const LONG width = static_cast<LONG>(random() % 30) + 1;
        const LONG height = static_cast<LONG>(random() % 30) + 1;
        const RECT rect{left, top, left + width, top + height};
        unsigned int possibleOwners = 7;
        // Deliberately not a bounding-rectangle containment implementation:
        // independently intersect per-pixel membership masks over the ROI.
        for (LONG row = rect.top; row < rect.bottom; row++)
        {
            for (LONG column = rect.left; column < rect.right; column++)
            {
                unsigned int owners = 0;
                if (column >= -100 && column < 0 && row >= -80 && row < 40)
                {
                    owners |= 1;
                }
                if (column >= 0 && column < 100 && row >= -20 && row < 80)
                {
                    owners |= 2;
                }
                if (column >= 130 && column < 200 && row >= 0 && row < 100)
                {
                    owners |= 4;
                }
                possibleOwners &= owners;
            }
        }
        auto output = Sentinel();
        const auto status = ResolveFromTopology(snapshot, rect, output);
        INFO("iteration=" << iteration << " left=" << left << " top=" << top);
        if (possibleOwners != 0)
        {
            REQUIRE(status);
            CHECK(output.monitor == Handle(static_cast<std::uintptr_t>(std::countr_zero(possibleOwners)) + 1));
            CHECK(EqualRect(output.physicalRect, rect));
            accepted++;
        }
        else
        {
            CHECK(status.code == ScreenRegionErrorCode::NotSingleMonitor);
            CHECK(SameRegion(output, Sentinel()));
            rejected++;
        }
    }
    CHECK(accepted > 1000);
    CHECK(rejected > 1000);
}

TEST_CASE("Diagnostic JSON has explicit physical coordinates and independent formatting", "[screen-region][json]")
{
    class Grouping final : public std::numpunct<char>
    {
        char do_thousands_sep() const override
        {
            return ',';
        }
        std::string do_grouping() const override
        {
            return "\3";
        }
    };
    std::ostringstream stream;
    const std::locale locale(std::locale::classic(), new Grouping);
    stream.imbue(locale);
    stream << std::hex << std::showbase << std::uppercase;
    stream.width(17);
    const auto flags = stream.flags();
    WriteScreenCaptureRegionJson(stream, Sentinel());
    CHECK(stream.str() == "{\"coordinateSpace\":\"physical-desktop\",\"monitor\":\"0x1234\",\"physicalRect\":{\"left\":-3,\"top\":-2,\"right\":4,\"bottom\":5},"
                          "\"monitorPhysicalRect\":{\"left\":-100,\"top\":-100,\"right\":1920,\"bottom\":1080},\"dpiX\":120,\"dpiY\":120,\"rotation\":2}");
    CHECK(stream.flags() == flags);
    CHECK(stream.width() == 17);
    CHECK(stream.getloc() == locale);
    CHECK(ScreenRegionStatus::Failure(ScreenRegionErrorCode::None, ScreenRegionStage::None).code == ScreenRegionErrorCode::InternalError);
    CHECK(std::string(GetScreenRegionErrorName(static_cast<ScreenRegionErrorCode>(255))) == "unknown-error");
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    CHECK_THROWS(stream.setstate(std::ios::badbit));
    CHECK_THROWS(WriteScreenCaptureRegionJson(stream, Sentinel()));
}
