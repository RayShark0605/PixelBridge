#include "recorded_pixel_replay.h"
#include "pbdemodd3d11/demodulator.h"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("OfflinePixels cannot relax live cursor admission", "[step2][offline-pixels]")
{
    using namespace pbcapturenormalize;
    ScreenCaptureFrameMetadata metadata;
    for (const auto backend : {CaptureBackendKind::Wgc, CaptureBackendKind::Dxgi})
    {
        metadata.backend = backend;
        metadata.isCursorExcluded = false;
        metadata.sourceCursorState = CursorState::Unknown;
        REQUIRE_FALSE(pbdemodd3d11::MatchesPixelInputContract(metadata, false));
        REQUIRE_FALSE(pbdemodd3d11::MatchesPixelInputContract(metadata, true));
        metadata.isCursorExcluded = true;
        metadata.sourceCursorState = CursorState::Excluded;
        REQUIRE(pbdemodd3d11::MatchesPixelInputContract(metadata, false));
        REQUIRE_FALSE(pbdemodd3d11::MatchesPixelInputContract(metadata, true));
    }
    metadata.backend = CaptureBackendKind::OfflinePixels;
    REQUIRE_FALSE(pbdemodd3d11::MatchesPixelInputContract(metadata, false));
    REQUIRE_FALSE(pbdemodd3d11::MatchesPixelInputContract(metadata, true));
    metadata.isCursorExcluded = false;
    metadata.sourceCursorState = CursorState::Unknown;
    REQUIRE(pbdemodd3d11::MatchesPixelInputContract(metadata, true));
    REQUIRE_FALSE(pbdemodd3d11::MatchesPixelInputContract(metadata, false));
}

TEST_CASE("Recording PTS has an independent bounded timeline", "[step2][offline-pixels]")
{
    pbapp::RecordedTimeline timeline;
    pbapp::RecordedPixelFrame frame;
    frame.timeBaseNumerator = 1;
    frame.timeBaseDenominator = 1000;
    frame.pts = -17;
    REQUIRE(timeline.Observe(frame));
    REQUIRE(timeline.Observe(frame)); // identical PTS retained, not deduplicated
    frame.pts = 120000 - 17;
    REQUIRE(timeline.Observe(frame));
    REQUIRE(timeline.SpanSeconds() == 120);
    frame.pts++;
    REQUIRE_FALSE(timeline.Observe(frame));
    frame.pts = 0;
    REQUIRE_FALSE(timeline.Observe(frame));
    frame.pts = timeline.lastPts;
    frame.timeBaseDenominator = 999;
    REQUIRE_FALSE(timeline.Observe(frame));
    frame.timeBaseDenominator = 0;
    REQUIRE_FALSE(timeline.Observe(frame));
    frame.pts = INT64_MAX;
    REQUIRE_FALSE(timeline.Observe(frame));
}
