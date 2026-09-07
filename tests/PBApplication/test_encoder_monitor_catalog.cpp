#include "encoder_monitor_catalog.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>

namespace
{

struct DisplayFixture
{
    MONITORINFOEXW information{};
    DEVMODEW mode{};
    pbapp::MonitorInfo output;

    DisplayFixture()
    {
        information.cbSize = sizeof(information);
        constexpr wchar_t name[] = L"\\\\.\\DISPLAY2";
        std::copy(std::begin(name), std::end(name), std::begin(information.szDevice));
        information.rcMonitor = {1920, -1, 3840, 1079};
        information.rcWork = {1920, -1, 3840, 1039};
        mode.dmSize = sizeof(mode);
        mode.dmFields = 449957536;
        mode.dmDisplayOrientation = DMDO_DEFAULT;
        mode.dmPelsWidth = 1920;
        mode.dmPelsHeight = 1080;
        mode.dmDisplayFrequency = 64;
        output.deviceName = L"unchanged-on-error";
    }

    [[nodiscard]] pbapp::MonitorCatalogStatus Build(const UINT dpiX = 96, const UINT dpiY = 96)
    {
        return pbapp::BuildEncoderMonitor(reinterpret_cast<HMONITOR>(2), information, mode, dpiX, dpiY, output);
    }
};

} // namespace

TEST_CASE("Encoder accepts the Citrix screenshot geometry without asserting a DXGI output", "[application][encoder-monitor]")
{
    DisplayFixture fixture;
    REQUIRE(fixture.Build());
    CHECK(fixture.output.deviceName == L"\\\\.\\DISPLAY2");
    CHECK_FALSE(fixture.output.dxgiOutputIdentityAvailable);
    CHECK(fixture.output.adapterLuid.LowPart == 0);
    CHECK(fixture.output.adapterLuid.HighPart == 0);
    CHECK(fixture.output.physicalRect.top == -1);
    CHECK(fixture.output.refreshRate == 64);
    CHECK(fixture.output.rotation == DXGI_MODE_ROTATION_IDENTITY);
    CHECK(fixture.output.supportsPhase1Canvas);
    CHECK_FALSE(fixture.output.phase1ReferenceGeometry);
    CHECK(pbapp::RectContains(fixture.output.physicalRect, {1920, -1, 3840, 1079}));
    const pbapp::MonitorInfo before = fixture.output;
    REQUIRE(fixture.Build());
    CHECK(pbapp::SameMonitorIdentity(before, fixture.output));
    fixture.information.rcMonitor.top = 0;
    REQUIRE(fixture.Build());
    CHECK_FALSE(pbapp::SameMonitorIdentity(before, fixture.output));
}

TEST_CASE("Encoder monitor validation retains DPI mode geometry and bounded-name checks", "[application][encoder-monitor]")
{
    DisplayFixture fixture;
    SECTION("missing DPI")
    {
        CHECK_FALSE(fixture.Build(0, 96));
    }
    SECTION("other DPI missing")
    {
        CHECK_FALSE(fixture.Build(96, 0));
    }
    SECTION("orientation not reported")
    {
        fixture.mode.dmFields &= ~DM_DISPLAYORIENTATION;
        CHECK_FALSE(fixture.Build());
    }
    SECTION("unknown orientation")
    {
        fixture.mode.dmDisplayOrientation = 4;
        CHECK_FALSE(fixture.Build());
    }
    SECTION("missing mode width")
    {
        fixture.mode.dmPelsWidth = 0;
        CHECK_FALSE(fixture.Build());
    }
    SECTION("missing mode height")
    {
        fixture.mode.dmPelsHeight = 0;
        CHECK_FALSE(fixture.Build());
    }
    SECTION("zero width")
    {
        fixture.information.rcMonitor.right = fixture.information.rcMonitor.left;
        CHECK_FALSE(fixture.Build());
    }
    SECTION("negative height")
    {
        fixture.information.rcMonitor.bottom = -2;
        CHECK_FALSE(fixture.Build());
    }
    SECTION("empty device name")
    {
        fixture.information.szDevice[0] = L'\0';
        CHECK_FALSE(fixture.Build());
    }
    SECTION("unterminated device name")
    {
        std::fill(std::begin(fixture.information.szDevice), std::end(fixture.information.szDevice), L'x');
        CHECK_FALSE(fixture.Build());
    }
    SECTION("default refresh remains out of this fix scope")
    {
        fixture.mode.dmDisplayFrequency = 1;
        CHECK_FALSE(fixture.Build());
    }
    CHECK(fixture.output.deviceName == L"unchanged-on-error");
}

TEST_CASE("Encoder keeps rotated monitor metadata distinct from capture DXGI provenance", "[application][encoder-monitor]")
{
    DisplayFixture fixture;
    constexpr std::array expected{DXGI_MODE_ROTATION_IDENTITY, DXGI_MODE_ROTATION_ROTATE270,
        DXGI_MODE_ROTATION_ROTATE180, DXGI_MODE_ROTATION_ROTATE90};
    for (DWORD orientation = 0; orientation < expected.size(); orientation++)
    {
        fixture.mode.dmDisplayOrientation = orientation;
        REQUIRE(fixture.Build());
        CHECK(fixture.output.rotation == expected[orientation]);
        CHECK_FALSE(fixture.output.dxgiOutputIdentityAvailable);
    }
    CHECK(pbapp::MonitorInfo{}.dxgiOutputIdentityAvailable);
}
