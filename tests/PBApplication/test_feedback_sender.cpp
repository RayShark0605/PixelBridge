#include "local_desktop_runtime.h"
#include "pbprotocol/session_random.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <filesystem>

namespace
{
class Scratch
{
public:
    Scratch()
    {
        const auto random = pbprotocol::GenerateRandomSessionId();
        REQUIRE(random);
        std::wstring suffix;
        for (const auto value : random.Value().bytes)
        {
            suffix += std::to_wstring(std::to_integer<unsigned int>(value));
        }
        path_ = std::filesystem::path(PB_TEST_SCRATCH_ROOT) / (L"feedback-sender-" + suffix);
        REQUIRE(std::filesystem::create_directory(path_));
    }
    ~Scratch()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    const std::filesystem::path& GetPath() const noexcept
    {
        return path_;
    }
private:
    std::filesystem::path path_;
};
}

TEST_CASE("Feedback sender recovers all four profiles through real FEC and storage at every window boundary", "[feedback][runtime][sender]")
{
    const auto profile = GENERATE(pbapp::VisualProfile::UnifiedLc4, pbapp::VisualProfile::UnifiedGrayFast,
        pbapp::VisualProfile::ExperimentalPam4, pbapp::VisualProfile::ExperimentalPam4Wide);
    const auto segmentCount = GENERATE(0U, 1U, 6U, 12U, 13U);
    const auto withholdTail = GENERATE(false, true);
    INFO("profile=" << static_cast<int>(profile) << " segments=" << segmentCount << " tail=" << withholdTail);
    Scratch scratch;
    pbapp::FeedbackTransferProbeOptions options;
    options.visualProfile = profile;
    options.segmentCount = segmentCount;
    options.withholdTail = withholdTail;
    pbapp::FeedbackTransferProbeSnapshot result;
    const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeFeedbackTransfer(scratch.GetPath().wstring(), options, result);
    INFO(status.message);
    REQUIRE(status);
    REQUIRE(result.decoder.wholeFileDigestCheck == true);
    REQUIRE(result.decoder.finalReopenVerified == true);
    REQUIRE(result.peakEncodedSegments <= 6);
    REQUIRE(result.repairIdentitiesUnique);
    REQUIRE(result.preparedRetriesFrozen);
    REQUIRE(result.disabledTraceEquivalent);
    REQUIRE(result.allConfirmedWithoutTerminalContinued);
    REQUIRE(result.terminalAccepted);
    REQUIRE_FALSE(result.staleTokenCanStop);
    if (segmentCount >= 6)
    {
        REQUIRE(result.acknowledgedRetirements > 0);
        REQUIRE(result.acknowledgedServiceAfterSettling == 0);
    }
}

TEST_CASE("Spatial banks and expired feedback retain fresh repair identity and full fallback coverage", "[feedback][runtime][sender]")
{
    const auto spatial = GENERATE(false, true);
    Scratch scratch;
    pbapp::FeedbackTransferProbeOptions options;
    options.visualProfile = pbapp::VisualProfile::UnifiedGrayFast;
    options.segmentCount = 13;
    options.spatialInterleave = spatial;
    options.expireFeedback = true;
    options.mixedCompression = false;
    pbapp::FeedbackTransferProbeSnapshot result;
    const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeFeedbackTransfer(scratch.GetPath().wstring(), options, result);
    INFO(status.message);
    REQUIRE(status);
    REQUIRE(result.decoder.finalReopenVerified == true);
    REQUIRE(result.feedbackExpired);
    REQUIRE(result.expiredCoveredAllSegments);
    REQUIRE(result.peakEncodedSegments <= 6);
    REQUIRE(result.repairIdentitiesUnique);
    REQUIRE(result.preparedRetriesFrozen);
}

TEST_CASE("Encoder feedback timeout remains recorded after subsequent inactive pumps", "[feedback][runtime][timeout]")
{
    Scratch scratch;
    pbapp::FeedbackTelemetry feedback;
    const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeFeedbackReaderTimeout(PB_FEEDBACK_FIXTURE_PATH,
        scratch.GetPath() / L"blocked.pbfb", feedback);
    INFO(status.message);
    REQUIRE(status);
    REQUIRE(feedback.requested);
    REQUIRE_FALSE(feedback.effective);
    REQUIRE(feedback.ioTimeouts == 1);
}

TEST_CASE("Feedback early retirement preserves durable Wirehair repair leases across Encoder restart", "[feedback][runtime][sender][persistence]")
{
    const auto profile = GENERATE(pbapp::VisualProfile::UnifiedLc4, pbapp::VisualProfile::UnifiedGrayFast,
        pbapp::VisualProfile::ExperimentalPam4, pbapp::VisualProfile::ExperimentalPam4Wide);
    Scratch scratch;
    pbapp::FeedbackTransferProbeOptions options;
    options.visualProfile = profile;
    options.segmentCount = 13;
    options.mixedCompression = false;
    options.withholdTail = true;
    options.durableRepairRestart = true;
    pbapp::FeedbackTransferProbeSnapshot result;
    const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeFeedbackTransfer(scratch.GetPath().wstring(), options, result);
    INFO(status.message);
    REQUIRE(status);
    REQUIRE(result.decoder.finalReopenVerified == true);
    REQUIRE(result.acknowledgedRetirements > 0);
    REQUIRE(result.repairIdentitiesUnique);
    REQUIRE(result.durableRestartVerified);
    REQUIRE(result.terminalAccepted);
}
