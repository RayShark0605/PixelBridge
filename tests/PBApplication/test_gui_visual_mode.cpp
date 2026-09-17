#include "gui_visual_mode.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>

namespace
{

pbapp::EncoderConfig Configuration(const RECT rectangle = {0, 0, 2560, 1600})
{
    auto config = pbapp::MakeUnifiedEncoderConfig(L"source.bin", 25);
    pbapp::MonitorInfo monitor;
    monitor.monitor = reinterpret_cast<HMONITOR>(std::uintptr_t{1});
    monitor.deviceName = L"GUI configuration test only";
    monitor.physicalRect = rectangle;
    monitor.workRect = rectangle;
    monitor.dpiX = 120;
    monitor.dpiY = 120;
    monitor.rotation = DXGI_MODE_ROTATION_IDENTITY;
    config.singleMonitorFullscreen = monitor;
    config.monitorClientOrigin = pbrenderd3d::PhysicalPoint{rectangle.left, rectangle.top};
    config.remoteMetadata.experimentMonitorIdentity = "GUI configuration test only";
    return config;
}

} // namespace

TEST_CASE("GUI mode indices preserve old identities and reject unknown values", "[gui-mode]")
{
    constexpr std::array profiles{pbapp::VisualProfile::UnifiedLc4, pbapp::VisualProfile::UnifiedGrayFast,
        pbapp::VisualProfile::ExperimentalPam4, pbapp::VisualProfile::ExperimentalPam4Wide};
    for (int index = 0; index < static_cast<int>(profiles.size()); index++)
    {
        REQUIRE(pbapp::GetGuiVisualProfile(index) == profiles[static_cast<std::size_t>(index)]);
    }
    for (const int index : {-1, 4, (std::numeric_limits<int>::min)(), (std::numeric_limits<int>::max)()})
    {
        REQUIRE_FALSE(pbapp::GetGuiVisualProfile(index));
        auto config = Configuration();
        REQUIRE_FALSE(pbapp::ApplyGuiEncoderVisualMode(config, index, false));
        REQUIRE(config.visualProfile == pbapp::VisualProfile::UnifiedLc4);
        REQUIRE(config.fullscreenRasterWidth == 0);
    }
}

TEST_CASE("Legacy GUI factory settings and caller FPS are unchanged", "[gui-mode]")
{
    for (const int index : {0, 1})
    {
        auto config = pbapp::MakeUnifiedEncoderConfig(L"source.bin", 15);
        REQUIRE(pbapp::ApplyGuiEncoderVisualMode(config, index, false));
        REQUIRE(config.visualProfile == *pbapp::GetGuiVisualProfile(index));
        REQUIRE(config.logicalVisualFps == 15);
        REQUIRE(config.segmentTargetBytes == 0);
        REQUIRE(config.compressionEnabled);
        REQUIRE(config.compressionLevel == 3);
        REQUIRE(config.controlRepetitions == 4);
        REQUIRE(config.remoteMetadata.channelType == pbapp::ChannelType::LocalDesktop);
        REQUIRE(config.remoteMetadata.remoteProvider.empty());
        REQUIRE_FALSE(config.singleMonitorFullscreen);
    }
}

TEST_CASE("PAM4 GUI binds the existing fixed raster without claiming a provider", "[gui-mode]")
{
    for (const int index : {2, 3})
    {
        auto config = Configuration();
        config.logicalVisualFps = 17;
        REQUIRE(pbapp::ApplyGuiEncoderVisualMode(config, index, false));
        REQUIRE(config.visualProfile == *pbapp::GetGuiVisualProfile(index));
        REQUIRE(config.fullscreenRasterWidth == (index == 3 ? 2560U : 0U));
        REQUIRE(config.segmentTargetBytes == 7U * 1024U * 1024U);
        REQUIRE(config.logicalVisualFps == 17);
        REQUIRE(config.remoteMetadata.channelType == pbapp::ChannelType::RemoteVisual);
        REQUIRE(config.remoteMetadata.remoteProvider == "Unspecified");
        REQUIRE(config.remoteMetadata.experimentMonitorIdentity == "GUI configuration test only");
        REQUIRE(config.remoteMetadata.protectedMonitorIdentity.empty());
        REQUIRE(config.fullscreenSampling == pbapp::FullscreenSamplingMode::Point);
        REQUIRE_FALSE(config.fullscreenNativeSize);
        auto explicitConfig = Configuration();
        explicitConfig.remoteMetadata.remoteProvider = "OperatorProvided";
        explicitConfig.segmentTargetBytes = 6U * 1024U * 1024U;
        REQUIRE(pbapp::ApplyGuiEncoderVisualMode(explicitConfig, index, false));
        REQUIRE(explicitConfig.remoteMetadata.remoteProvider == "OperatorProvided");
        REQUIRE(explicitConfig.segmentTargetBytes == 6U * 1024U * 1024U);
    }
}

TEST_CASE("PAM4 GUI physical fit is independent of logical DPI and never clips Wide", "[gui-mode]")
{
    struct FitCase
    {
        RECT rectangle;
        bool pam4;
        bool wide;
    };
    constexpr std::array cases{FitCase{{0, 0, 1920, 1080}, true, false}, FitCase{{0, 0, 2560, 1440}, true, true},
        FitCase{{0, 0, 2560, 1600}, true, true}, FitCase{{-3840, 0, 0, 2160}, true, true},
        FitCase{{0, 0, 2559, 1440}, true, false}, FitCase{{0, 0, 2560, 1439}, true, false},
        FitCase{{0, 0, 1919, 1080}, false, false}, FitCase{{0, 0, 1920, 1079}, false, false},
        FitCase{{0, 0, 3841, 2160}, false, false}, FitCase{{0, 0, 3840, 2161}, false, false},
        FitCase{{100, 100, 0, 0}, false, false},
        FitCase{{(std::numeric_limits<LONG>::min)(), 0, (std::numeric_limits<LONG>::max)(), 1440}, false, false}};
    for (const auto& fit : cases)
    {
        for (const int index : {2, 3})
        {
            for (const std::uint32_t dpi : {96U, 120U, 144U, 192U})
            {
                auto config = Configuration(fit.rectangle);
                config.singleMonitorFullscreen->dpiX = dpi;
                config.singleMonitorFullscreen->dpiY = dpi;
                const auto status = pbapp::ApplyGuiEncoderVisualMode(config, index, false);
                REQUIRE(static_cast<bool>(status) == (index == 2 ? fit.pam4 : fit.wide));
                REQUIRE(EqualRect(&config.singleMonitorFullscreen->physicalRect, &fit.rectangle));
                REQUIRE(config.singleMonitorFullscreen->dpiX == dpi);
                if (!status)
                {
                    REQUIRE(config.visualProfile == pbapp::VisualProfile::UnifiedLc4);
                    REQUIRE(config.segmentTargetBytes == 0);
                }
            }
        }
    }
}

TEST_CASE("PAM4 GUI rejects measurement and conflicting presentation before mutation", "[gui-mode]")
{
    for (const int index : {2, 3})
    {
        auto measured = Configuration();
        REQUIRE_FALSE(pbapp::ApplyGuiEncoderVisualMode(measured, index, true));
        REQUIRE(measured.visualProfile == pbapp::VisualProfile::UnifiedLc4);
        for (int invalid = 0; invalid < 10; invalid++)
        {
            auto config = Configuration();
            switch (invalid)
            {
            case 0: config.singleMonitorFullscreen.reset(); break;
            case 1: config.fullscreenNativeSize = true; break;
            case 2: config.fullscreenSampling = pbapp::FullscreenSamplingMode::Linear; break;
            case 3: config.grayFastSpatialInterleave = true; break;
            case 4: config.grayFastShortInitialAirtime = true; break;
            case 5: config.grayFastExtendedVisitBudget = true; break;
            case 6: config.fullscreenRasterWidth = 2304; break;
            case 7: config.singleMonitorFullscreen->monitor = nullptr; break;
            case 8: config.singleMonitorFullscreen->rotation = DXGI_MODE_ROTATION_ROTATE90; break;
            case 9: config.singleMonitorFullscreen->dpiX = 0; break;
            }
            REQUIRE_FALSE(pbapp::ApplyGuiEncoderVisualMode(config, index, false));
            REQUIRE(config.visualProfile == pbapp::VisualProfile::UnifiedLc4);
            REQUIRE(config.remoteMetadata.channelType == pbapp::ChannelType::LocalDesktop);
            REQUIRE(config.segmentTargetBytes == 0);
        }
    }
}
