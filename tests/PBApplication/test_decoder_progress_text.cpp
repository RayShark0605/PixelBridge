#include "decoder_progress_text.h"

#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_CASE("Decoder GUI progress stays unavailable before a descriptor and never invents an ETA", "[application][g22][gui-progress]")
{
    pbapp::DecoderSnapshot snapshot;
    snapshot.state = pbapp::DecoderState::WaitingForBootstrap;
    const auto text = pbgui::FormatDecoderProgress(snapshot);
    CHECK(text.percent == QStringLiteral("—"));
    CHECK(text.speed == QStringLiteral("0 KB/s"));
    CHECK(text.remaining == QStringLiteral("估算中"));
}

TEST_CASE("Decoder GUI formats verified bytes and Windows binary KB per second without using visual FPS", "[application][g22][gui-progress]")
{
    pbapp::DecoderSnapshot snapshot;
    snapshot.state = pbapp::DecoderState::Receiving;
    snapshot.descriptorKnown = true;
    snapshot.originalFileBytes = 20000;
    snapshot.verifiedRawBytes = 5000;
    snapshot.recoveryProgress = 0.25;
    snapshot.smoothedVerifiedRawGoodputBytesPerSecond = 12800;
    snapshot.etaMilliseconds = 3661001;
    const auto text = pbgui::FormatDecoderProgress(snapshot);
    CHECK(text.percent == QStringLiteral("25.0%"));
    CHECK(text.speed == QStringLiteral("12.5 KB/s"));
    CHECK(text.remaining == QStringLiteral("01:01:02"));
    snapshot.captureStallActive = true;
    CHECK(pbgui::FormatDecoderProgress(snapshot).speed == QStringLiteral("0 KB/s"));
    CHECK(pbgui::FormatDecoderProgress(snapshot).remaining == QStringLiteral("估算中"));
    snapshot.captureStallActive = false;
    snapshot.state = pbapp::DecoderState::Stopped;
    CHECK(pbgui::FormatDecoderProgress(snapshot).percent == QStringLiteral("25.0%"));
    CHECK(pbgui::FormatDecoderProgress(snapshot).remaining == QStringLiteral("—"));
}

TEST_CASE("Decoder GUI requires final reopen before showing one hundred percent including empty files", "[application][g22][gui-progress]")
{
    pbapp::DecoderSnapshot snapshot;
    snapshot.state = pbapp::DecoderState::Verifying;
    snapshot.descriptorKnown = true;
    snapshot.recoveryProgress = 1;
    CHECK(pbgui::FormatDecoderProgress(snapshot).percent == QStringLiteral("99.9%"));
    snapshot.state = pbapp::DecoderState::Completed;
    snapshot.wholeFileDigestVerified = true;
    snapshot.finalPublishSucceeded = true;
    snapshot.outputPath = "empty.bin";
    CHECK_FALSE(pbgui::IsVerifiedCompletion(snapshot));
    snapshot.finalReopenVerified = false;
    CHECK_FALSE(pbgui::IsVerifiedCompletion(snapshot));
    snapshot.finalReopenVerified = true;
    CHECK(pbgui::IsVerifiedCompletion(snapshot));
    CHECK(pbgui::FormatDecoderProgress(snapshot).percent == QStringLiteral("100%"));
    CHECK(pbgui::FormatDecoderProgress(snapshot).remaining == QStringLiteral("00:00:00"));
    snapshot.finalPublishSucceeded = false;
    CHECK_FALSE(pbgui::IsVerifiedCompletion(snapshot));
}

TEST_CASE("Decoder GUI rejects nonfinite progress and speed and formats a bounded large ETA without overflow", "[application][g22][gui-progress]")
{
    pbapp::DecoderSnapshot snapshot;
    snapshot.state = pbapp::DecoderState::Receiving;
    snapshot.descriptorKnown = true;
    snapshot.originalFileBytes = 2;
    snapshot.verifiedRawBytes = 1;
    snapshot.recoveryProgress = (std::numeric_limits<double>::quiet_NaN)();
    snapshot.smoothedVerifiedRawGoodputBytesPerSecond = (std::numeric_limits<double>::infinity)();
    snapshot.etaMilliseconds = (std::numeric_limits<std::uint64_t>::max)();
    CHECK(pbgui::FormatDecoderProgress(snapshot).percent == QStringLiteral("—"));
    CHECK(pbgui::FormatDecoderProgress(snapshot).speed == QStringLiteral("0 KB/s"));
    CHECK(pbgui::FormatDecoderProgress(snapshot).remaining == QStringLiteral("估算中"));
    snapshot.smoothedVerifiedRawGoodputBytesPerSecond = 1;
    CHECK(pbgui::FormatDecoderProgress(snapshot).remaining.contains(QStringLiteral("天")));
    snapshot.recoveryProgress = -1;
    CHECK(pbgui::FormatDecoderProgress(snapshot).percent == QStringLiteral("—"));
    snapshot.recoveryProgress = 2;
    CHECK(pbgui::FormatDecoderProgress(snapshot).percent == QStringLiteral("—"));
}
