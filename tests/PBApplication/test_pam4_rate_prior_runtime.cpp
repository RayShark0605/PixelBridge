#include "gui_visual_mode.h"
#include "local_desktop_runtime.h"
#include "pbprotocol/blake3_digest.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

namespace
{

class RatePriorScratch final
{
public:
    RatePriorScratch()
    {
        const auto root = std::filesystem::absolute(PB_TEST_SCRATCH_ROOT).lexically_normal();
        path = root / (L"pam4-rate-prior-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        REQUIRE(path.parent_path() == root);
        REQUIRE(std::filesystem::create_directory(path));
    }
    ~RatePriorScratch()
    {
        const auto root = std::filesystem::absolute(PB_TEST_SCRATCH_ROOT).lexically_normal();
        if (path.parent_path() == root)
        {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    }
    std::filesystem::path path;
};

}

TEST_CASE("PAM4 rate prior is opt in and explicitly rejected for every other GUI profile", "[rate-prior][configuration]")
{
    RatePriorScratch scratch;
    const auto source = scratch.path / L"source.bin";
    std::ofstream(source, std::ios::binary).put('x');
    for (const int mode : {0, 1, 2, 3})
    {
        auto config = pbapp::MakeUnifiedEncoderConfig(source.wstring(), 30);
        REQUIRE_FALSE(config.expectedReceptionRate);
        if (mode >= 2)
        {
            pbapp::MonitorInfo monitor;
            monitor.monitor = reinterpret_cast<HMONITOR>(std::uintptr_t{1});
            monitor.physicalRect = {0, 0, 2560, 1600};
            monitor.workRect = monitor.physicalRect;
            monitor.dpiX = 96;
            monitor.dpiY = 96;
            monitor.rotation = DXGI_MODE_ROTATION_IDENTITY;
            config.singleMonitorFullscreen = monitor;
            config.monitorClientOrigin = pbrenderd3d::PhysicalPoint{0, 0};
        }
        REQUIRE(pbapp::ApplyGuiEncoderVisualMode(config, mode, false));
        const auto baselineStatus = pbapp::ValidateEncoderConfig(config);
        INFO(baselineStatus.message);
        REQUIRE(baselineStatus);
        config.expectedReceptionRate = pbapp::SenderRatePrior{200, 280};
        if (mode == 2)
        {
            REQUIRE(pbapp::ValidateEncoderConfig(config));
            config.expectedReceptionRate = pbapp::SenderRatePrior{281, 280};
            REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
        }
        else
        {
            REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
        }
    }
}

TEST_CASE("Optional PAM4 scheduling still publishes an independently verified file without feedback", "[rate-prior][recovery]")
{
    RatePriorScratch scratch;
    for (const bool enabled : {false, true})
    {
        const auto destination = scratch.path / (enabled ? L"enabled" : L"disabled");
        REQUIRE(std::filesystem::create_directory(destination));
        pbapp::UnifiedGraduationRecoveryProbeConfig config;
        config.visualProfile = pbapp::VisualProfile::ExperimentalPam4;
        config.segmentCount = 13;
        config.segmentBytes = 64U * 1024U;
        config.erasureModel = pbapp::UnifiedRecoveryErasureModel::PeriodicHalf;
        config.maximumSenderLogicalFrames = 12000;
        if (enabled)
        {
            config.expectedReceptionRate = pbapp::SenderRatePrior{200, 280};
        }
        pbapp::UnifiedGraduationRecoveryProbeSnapshot snapshot;
        const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeUnifiedGraduationRecovery(destination.wstring(), config, snapshot);
        INFO(status.message);
        REQUIRE(status);
        REQUIRE(snapshot.decoder.visualProfile == pbapp::VisualProfile::ExperimentalPam4);
        REQUIRE(snapshot.decoder.state == pbapp::DecoderState::Completed);
        REQUIRE(snapshot.decoder.verifiedRawBytes == 851968);
        REQUIRE(snapshot.decoder.wholeFileDigestCheck == true);
        REQUIRE(snapshot.decoder.finalRenameSucceeded == true);
        REQUIRE(snapshot.decoder.finalReopenVerified == true);
        REQUIRE(std::filesystem::is_regular_file(snapshot.publishedPath));
        std::ifstream published(snapshot.publishedPath, std::ios::binary);
        std::vector<std::byte> bytes(851968);
        published.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        REQUIRE(published.gcount() == static_cast<std::streamsize>(bytes.size()));
        REQUIRE(published.peek() == std::char_traits<char>::eof());
        REQUIRE(pbprotocol::ComputeBlake3Digest(bytes) == snapshot.expectedWholeFileDigest);
        REQUIRE(snapshot.ratePrior.configured.has_value() == enabled);
        REQUIRE(snapshot.ratePrior.effective == enabled);
        if (enabled)
        {
            REQUIRE(snapshot.ratePrior.activeNanoseconds > 0);
            REQUIRE(snapshot.ratePrior.beforeFrontierEquations + snapshot.ratePrior.nearFrontierEquations +
                snapshot.ratePrior.aheadOfFrontierEquations + snapshot.ratePrior.safetyRepairEquations > 0);
        }
    }
}

TEST_CASE("Wrong PAM4 rate priors and late joins preserve all-file repair and small-file behavior", "[rate-prior][recovery]")
{
    RatePriorScratch scratch;
    const std::array priors{pbapp::SenderRatePrior{1, 1}, pbapp::SenderRatePrior{1048576, 1048576}};
    for (std::size_t index = 0; index < priors.size(); index++)
    {
        const auto destination = scratch.path / std::to_wstring(index);
        REQUIRE(std::filesystem::create_directory(destination));
        pbapp::UnifiedGraduationRecoveryProbeConfig config;
        config.visualProfile = pbapp::VisualProfile::ExperimentalPam4;
        config.expectedReceptionRate = priors[index];
        config.segmentBytes = 64U * 1024U;
        config.firstObservedLogicalFrame = 600;
        config.erasureModel = pbapp::UnifiedRecoveryErasureModel::PeriodicQuarter;
        config.maximumSenderLogicalFrames = 12000;
        pbapp::UnifiedGraduationRecoveryProbeSnapshot snapshot;
        const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeUnifiedGraduationRecovery(destination.wstring(), config, snapshot);
        INFO(status.message);
        REQUIRE(status);
        REQUIRE(snapshot.ratePrior.effective);
        REQUIRE(snapshot.ratePrior.safetyRepairEquations > 0);
        REQUIRE(snapshot.decoder.verifiedRawBytes == 851968);
        REQUIRE(snapshot.decoder.wholeFileDigestCheck == true);
        REQUIRE(snapshot.decoder.finalReopenVerified == true);
        REQUIRE(snapshot.peakActiveDecoders <= 8);
    }
    const auto destination = scratch.path / L"small";
    REQUIRE(std::filesystem::create_directory(destination));
    pbapp::UnifiedGraduationRecoveryProbeConfig smallFileConfig;
    smallFileConfig.visualProfile = pbapp::VisualProfile::ExperimentalPam4;
    smallFileConfig.expectedReceptionRate = pbapp::SenderRatePrior{200, 280};
    smallFileConfig.segmentCount = 2;
    smallFileConfig.segmentBytes = 64U * 1024U;
    smallFileConfig.erasureModel = pbapp::UnifiedRecoveryErasureModel::None;
    pbapp::UnifiedGraduationRecoveryProbeSnapshot snapshot;
    const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeUnifiedGraduationRecovery(destination.wstring(), smallFileConfig, snapshot);
    INFO(status.message);
    REQUIRE(status);
    REQUIRE(snapshot.ratePrior.configured.has_value());
    REQUIRE_FALSE(snapshot.ratePrior.effective);
    REQUIRE(snapshot.ratePrior.activeNanoseconds == 0);
    REQUIRE(snapshot.decoder.verifiedRawBytes == 131072);
    REQUIRE(snapshot.decoder.finalReopenVerified == true);
}

TEST_CASE("PAM4 prior releases an old serviced window before the original two-K target", "[rate-prior][schedule]")
{
    RatePriorScratch scratch;
    std::array<pbapp::UnifiedGraduationRecoveryProbeSnapshot, 2> snapshots;
    for (std::size_t index = 0; index < snapshots.size(); index++)
    {
        const auto destination = scratch.path / std::to_wstring(index);
        REQUIRE(std::filesystem::create_directory(destination));
        pbapp::UnifiedGraduationRecoveryProbeConfig config;
        config.visualProfile = pbapp::VisualProfile::ExperimentalPam4;
        config.segmentBytes = 256U * 1024U;
        config.erasureModel = pbapp::UnifiedRecoveryErasureModel::None;
        config.collectSegmentTrace = true;
        if (index == 1)
        {
            config.expectedReceptionRate = pbapp::SenderRatePrior{200, 280};
        }
        const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeUnifiedGraduationRecovery(destination.wstring(), config, snapshots[index]);
        INFO(status.message);
        REQUIRE(status);
        REQUIRE(snapshots[index].decoder.finalReopenVerified == true);
        REQUIRE(snapshots[index].decoder.wholeFileDigestCheck == true);
        REQUIRE(snapshots[index].segmentTrace.size() == 13);
        REQUIRE(snapshots[index].segmentTrace[6].firstObservedFrame.has_value());
    }
    // This loss-free fixture checks an actual admission effect, not a speedup
    // promise for a remote channel. Merely reporting "effective" cannot pass.
    REQUIRE(*snapshots[1].segmentTrace[6].firstObservedFrame < *snapshots[0].segmentTrace[6].firstObservedFrame);
    REQUIRE(snapshots[0].expectedWholeFileDigest == snapshots[1].expectedWholeFileDigest);
}
