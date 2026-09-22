#include "unified_two_stage_resample_fixture.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Test-only two-stage presentation comparison (point vs linear vs area stage
// one, independent fractional remote-view rescale as stage two). These tests
// prove only that the candidate stage-one filters keep the fail-closed
// geometric chain exactly decodable at unchanged thresholds; they are not a
// production filter change, a codec measurement, or a field certification.
// The metric aggregates printed per configuration are diagnostics for the
// candidate comparison and never gate the assertions.
namespace
{

using namespace pbmodulation;

const char* ModeName(const pbtest::UnifiedFullscreenSamplingMode mode)
{
    switch (mode)
    {
    case pbtest::UnifiedFullscreenSamplingMode::Point: return "point";
    case pbtest::UnifiedFullscreenSamplingMode::Linear: return "linear";
    case pbtest::UnifiedFullscreenSamplingMode::Area: return "area";
    default: return "unknown";
    }
}

struct StageTwoConfig
{
    std::uint32_t fullscreenWidth;
    std::uint32_t fullscreenHeight;
    int horizontalScalePermille;
    int verticalScalePermille;
    int originXFractionPermille;
    int originYFractionPermille;
    const char* label;
};

void RequireExactRecovery(const UnifiedVisualObservation& observation, const UnifiedVisualCpuOracle& oracle,
    const pbtest::UnifiedRandomFullLoadFrame& frame)
{
    REQUIRE(observation.IsFrameAvailable());
    REQUIRE(observation.acceptedBlocks == frame.slotCount);
    REQUIRE(observation.acceptedTransportBlocks == frame.slotCount - 1);
    REQUIRE(observation.acceptedControlRecords == 1);
    const std::span<const UnifiedAcceptedBlock> accepted = oracle.GetAcceptedBlocks();
    REQUIRE(accepted.size() == frame.slotCount);
    for (std::uint32_t slot = 0; slot < frame.slotCount; slot++)
    {
        REQUIRE(observation.slotObservations[slot].accepted);
        REQUIRE(accepted[slot].size == frame.blocks[slot].size());
        REQUIRE(std::equal(frame.blocks[slot].begin(), frame.blocks[slot].end(), accepted[slot].bytes.begin()));
    }
}

} // namespace

TEST_CASE("Two-stage stage-one sampling references are exact identity at the canonical screen size",
    "[unified][two-stage][sampling][reference]")
{
    const auto frame = pbtest::BuildUnifiedRandomFullLoadFrame(pbprotocol::kGrayFastExperimentalProfile,
        0x5EED0C0FFEE7ULL, 71);
    for (const auto mode : {pbtest::UnifiedFullscreenSamplingMode::Point,
        pbtest::UnifiedFullscreenSamplingMode::Linear, pbtest::UnifiedFullscreenSamplingMode::Area})
    {
        CAPTURE(ModeName(mode));
        const auto composed = pbtest::ComposeUnifiedFullscreenStage(frame.canonical, 1920, 1080, mode);
        REQUIRE(composed == frame.canonical);
    }
}

TEST_CASE("Remote-view resample fixture rejects transforms outside its fixed bounds",
    "[unified][two-stage][sampling][reference][negative]")
{
    const auto frame = pbtest::BuildUnifiedRandomFullLoadFrame(pbprotocol::kGrayFastExperimentalProfile,
        0x5EED0C0FFEE8ULL, 72);
    const auto stageOne = pbtest::ComposeUnifiedFullscreenStage(frame.canonical, 2560, 1600,
        pbtest::UnifiedFullscreenSamplingMode::Point);
    // Live-like geometry fits and returns the fixed capture footprint.
    const auto valid = pbtest::MakeUnifiedRemoteViewResampleFixture(stageOne, 2560, 1600, 851, 851, 137, 861);
    REQUIRE(valid.size() == static_cast<std::size_t>(pbtest::kTwoStageCaptureWidth) *
        pbtest::kTwoStageCaptureHeight * 4U);
    REQUIRE_THROWS_AS(pbtest::MakeUnifiedRemoteViewResampleFixture(stageOne, 2560, 1600, 699, 851, 137, 861),
        std::runtime_error);
    REQUIRE_THROWS_AS(pbtest::MakeUnifiedRemoteViewResampleFixture(stageOne, 2560, 1600, 851, 1251, 137, 861),
        std::runtime_error);
    REQUIRE_THROWS_AS(pbtest::MakeUnifiedRemoteViewResampleFixture(stageOne, 2560, 1600, 851, 851, 1000, 861),
        std::runtime_error);
    // 2560x1600 * 0.990 + 47.999 exceeds the 2560 capture width; the vertical
    // 0.890 variant exceeds 1440. Both must fail closed instead of cropping.
    REQUIRE_THROWS_AS(pbtest::MakeUnifiedRemoteViewResampleFixture(stageOne, 2560, 1600, 990, 850, 999, 861),
        std::runtime_error);
    REQUIRE_THROWS_AS(pbtest::MakeUnifiedRemoteViewResampleFixture(stageOne, 2560, 1600, 851, 890, 137, 999),
        std::runtime_error);
    REQUIRE_THROWS_AS(pbtest::MakeUnifiedRemoteViewResampleFixture(
        std::span<const std::byte>(stageOne).first(stageOne.size() - 1U), 2560, 1600, 851, 851, 137, 861),
        std::runtime_error);
}

TEST_CASE("Two-stage sampling modes tolerate different bounded luma noise levels",
    "[unified][two-stage][sampling][diagnostic]")
{
    // Codec-noise proxy comparison, not a recovery gate: after the geometric
    // two-stage chain, chroma-neutral bounded luma noise stands in for
    // remote-codec quantization. Only structural invariants are asserted; the
    // printed accepted-block counts and metric aggregates are the comparison
    // evidence and never gate this suite.
    const auto frame = pbtest::BuildUnifiedRandomFullLoadFrame(pbprotocol::kGrayFastExperimentalProfile,
        0x5EED0C0FFEE9ULL, 71);
    for (const int amplitude : {0, 2, 4, 6, 8})
    {
        for (const auto mode : {pbtest::UnifiedFullscreenSamplingMode::Point,
            pbtest::UnifiedFullscreenSamplingMode::Linear, pbtest::UnifiedFullscreenSamplingMode::Area})
        {
            CAPTURE(amplitude, ModeName(mode));
            const auto stageOne = pbtest::ComposeUnifiedFullscreenStage(frame.canonical, 2560, 1600, mode);
            auto pixels = pbtest::MakeUnifiedRemoteViewResampleFixture(stageOne, 2560, 1600, 851, 851, 137, 861);
            pbtest::ApplyUnifiedBoundedLumaNoise(pixels, pbtest::kTwoStageCaptureWidth,
                pbtest::kTwoStageCaptureHeight, amplitude, 0x6015EED0BEFULL + static_cast<std::uint64_t>(amplitude));
            const LumaView view{pixels, pbtest::kTwoStageCaptureWidth, pbtest::kTwoStageCaptureHeight,
                static_cast<std::size_t>(pbtest::kTwoStageCaptureWidth) * 4U, LumaPixelFormat::Bgra8};
            auto created = UnifiedVisualCpuOracle::Create(UnifiedVisualCpuOracle::RequiredBytes());
            REQUIRE(created);
            auto oracle = std::move(created).Value();
            const UnifiedExpectedFrameIdentity identity{true, pbprotocol::SessionTag{frame.sessionTagValue},
                true, 71, pbprotocol::kGrayFastExperimentalProfile.visualProfileId,
                pbprotocol::kGrayFastExperimentalProfile.visualLayoutVersion};
            const auto observation = oracle.DecodeMixedFrame(view, identity);
            REQUIRE(observation.inputValid);
            std::uint64_t nonzeroMetrics = 0;
            std::uint64_t magnitudeSum = 0;
            for (const UnifiedSoftMetric& metric : oracle.GetSoftMetrics())
            {
                if (metric.value != 0)
                {
                    nonzeroMetrics++;
                    magnitudeSum += static_cast<std::uint64_t>(std::abs(static_cast<int>(metric.value)));
                }
            }
            std::cout << "[two-stage-noise] amplitude=" << amplitude << " mode=" << ModeName(mode)
                << " frameAvailable=" << (observation.IsFrameAvailable() ? 1 : 0)
                << " acceptedBlocks=" << observation.acceptedBlocks
                << " acceptedTransport=" << observation.acceptedTransportBlocks
                << " nonzeroMetrics=" << nonzeroMetrics
                << " meanMagnitude=" << (nonzeroMetrics == 0 ? 0.0 :
                    static_cast<double>(magnitudeSum) / static_cast<double>(nonzeroMetrics)) << '\n';
        }
    }
}

TEST_CASE("Two-stage point, linear and area presentation chains recover full-load random gray frames exactly",
    "[unified][two-stage][sampling][channel][profile-scoped]")
{
    const std::array<StageTwoConfig, 3> configs{
        StageTwoConfig{2560, 1600, 851, 851, 137, 861, "live-like-851x851"},
        StageTwoConfig{2560, 1600, 937, 742, 0, 500, "anisotropic-937x742"},
        StageTwoConfig{1920, 1080, 1000, 1000, 0, 0, "identity-1000x1000"}};
    for (const auto& profile : {pbprotocol::kGrayFastExperimentalProfile,
        pbprotocol::kGrayStatesExperimentalProfile})
    {
        const auto frame = pbtest::BuildUnifiedRandomFullLoadFrame(profile, 0x5EED0C0FFEE9ULL, 71);
        for (const auto& config : configs)
        {
            for (const auto mode : {pbtest::UnifiedFullscreenSamplingMode::Point,
                pbtest::UnifiedFullscreenSamplingMode::Linear, pbtest::UnifiedFullscreenSamplingMode::Area})
            {
                CAPTURE(profile.visualProfileId, config.label, ModeName(mode));
                const auto stageOne = pbtest::ComposeUnifiedFullscreenStage(frame.canonical,
                    config.fullscreenWidth, config.fullscreenHeight, mode);
                const auto pixels = pbtest::MakeUnifiedRemoteViewResampleFixture(stageOne,
                    config.fullscreenWidth, config.fullscreenHeight,
                    config.horizontalScalePermille, config.verticalScalePermille,
                    config.originXFractionPermille, config.originYFractionPermille);
                const LumaView view{pixels, pbtest::kTwoStageCaptureWidth, pbtest::kTwoStageCaptureHeight,
                    static_cast<std::size_t>(pbtest::kTwoStageCaptureWidth) * 4U, LumaPixelFormat::Bgra8};
                auto created = UnifiedVisualCpuOracle::Create(UnifiedVisualCpuOracle::RequiredBytes());
                REQUIRE(created);
                auto oracle = std::move(created).Value();
                const UnifiedExpectedFrameIdentity identity{true, pbprotocol::SessionTag{frame.sessionTagValue},
                    true, 71, profile.visualProfileId, profile.visualLayoutVersion};
                const auto observation = oracle.DecodeMixedFrame(view, identity);
                RequireExactRecovery(observation, oracle, frame);
                REQUIRE(observation.bootstrap.geometry.scaleX == Catch::Approx(
                    config.fullscreenWidth * config.horizontalScalePermille / 1000.0 / 1920.0).margin(0.002));
                REQUIRE(observation.bootstrap.geometry.scaleY == Catch::Approx(
                    config.fullscreenHeight * config.verticalScalePermille / 1000.0 / 1080.0).margin(0.002));
                const auto metrics = oracle.GetSoftMetrics();
                std::uint64_t nonzeroMetrics = 0;
                std::uint64_t magnitudeSum = 0;
                std::uint64_t minimumMagnitude = 32768;
                for (const UnifiedSoftMetric& metric : metrics)
                {
                    const std::uint64_t magnitude = static_cast<std::uint64_t>(
                        std::abs(static_cast<int>(metric.value)));
                    if (magnitude == 0)
                    {
                        continue;
                    }
                    nonzeroMetrics++;
                    magnitudeSum += magnitude;
                    minimumMagnitude = std::min(minimumMagnitude, magnitude);
                }
                std::cout << "[two-stage] profile=" << profile.visualProfileId << " config=" << config.label
                    << " mode=" << ModeName(mode) << " nonzeroMetrics=" << nonzeroMetrics
                    << " meanMagnitude=" << (nonzeroMetrics == 0 ? 0.0 :
                        static_cast<double>(magnitudeSum) / static_cast<double>(nonzeroMetrics))
                    << " minMagnitude=" << (nonzeroMetrics == 0 ? 0 : minimumMagnitude) << '\n';
            }
        }
    }
}
