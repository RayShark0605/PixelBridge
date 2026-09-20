#include "decoder_progress_text.h"

#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_CASE("Decoder GUI progress stays unavailable before a descriptor and never invents an ETA", "[application][g22][gui-progress]")
{
    pbapp::DecoderSnapshot snapshot;
    snapshot.state = pbapp::DecoderState::WaitingForBootstrap;
    snapshot.runStartedUnixMilliseconds = 1000;
    const auto text = pbgui::FormatDecoderProgress(snapshot, 21000);
    CHECK(text.percent == QStringLiteral("—"));
    CHECK(text.received == QStringLiteral("—"));
    CHECK(text.elapsed == QStringLiteral("—"));
    CHECK(text.speed == QStringLiteral("0 KB/s"));
    CHECK(text.remaining == QStringLiteral("估算中"));
}

TEST_CASE("Decoder GUI progress advances from the continuous estimate between Segment verifications",
    "[application][g22][gui-progress]")
{
    pbapp::DecoderSnapshot snapshot;
    snapshot.state = pbapp::DecoderState::Recovering;
    snapshot.descriptorKnown = true;
    snapshot.originalFileBytes = 20000;
    snapshot.verifiedRawBytes = 0;
    snapshot.estimatedReceivedRawBytes = 6000;
    snapshot.runStartedUnixMilliseconds = 1000;
    snapshot.activeReceptionStartedUnixMilliseconds = 4000;
    const auto text = pbgui::FormatDecoderProgress(snapshot, 64000);
    CHECK(text.percent == QStringLiteral("30.0%"));
    CHECK(text.received == QStringLiteral("5.9 KB（接收中）"));
    CHECK(text.elapsed == QStringLiteral("00:01:00"));
    // The three-second pre-descriptor wait is excluded: 6000 B / 60 s =
    // 100 B/s = 0.1 KB/s; remaining 14000 B at that rate.
    CHECK(text.speed == QStringLiteral("0.1 KB/s"));
    CHECK(text.remaining == QStringLiteral("00:02:20"));
    // Without the estimate the display falls back to verified bytes; the
    // first second stays quiet instead of dividing by ~0 elapsed.
    snapshot.estimatedReceivedRawBytes = 0;
    const auto quiet = pbgui::FormatDecoderProgress(snapshot, 4500);
    CHECK(quiet.percent == QStringLiteral("0.0%"));
    CHECK(quiet.received == QStringLiteral("0 B"));
    CHECK(quiet.speed == QStringLiteral("0 KB/s"));
    CHECK(quiet.remaining == QStringLiteral("估算中"));
}

TEST_CASE("Decoder GUI received size caps the estimate at the file and annotates verified bytes",
    "[application][g22][gui-progress]")
{
    pbapp::DecoderSnapshot snapshot;
    snapshot.state = pbapp::DecoderState::Receiving;
    snapshot.descriptorKnown = true;
    snapshot.originalFileBytes = 10000;
    snapshot.verifiedRawBytes = 4000;
    snapshot.estimatedReceivedRawBytes = 9000;
    snapshot.runStartedUnixMilliseconds = 1000;
    snapshot.activeReceptionStartedUnixMilliseconds = 4000;
    const auto text = pbgui::FormatDecoderProgress(snapshot, 74000);
    CHECK(text.percent == QStringLiteral("90.0%"));
    CHECK(text.received == QStringLiteral("8.8 KB（已验证 3.9 KB）"));
    CHECK(text.speed == QStringLiteral("0.1 KB/s"));
    CHECK(text.remaining == QStringLiteral("00:00:08"));
    // A hostile over-estimate cannot cross the declared file size.
    snapshot.estimatedReceivedRawBytes = 999999;
    CHECK(pbgui::FormatDecoderProgress(snapshot, 74000).received ==
        QStringLiteral("9.8 KB（已验证 3.9 KB）"));
    CHECK(pbgui::FormatDecoderProgress(snapshot, 74000).percent == QStringLiteral("99.9%"));
}

TEST_CASE("Decoder GUI keeps verified-only snapshots working after resume", "[application][g22][gui-progress]")
{
    pbapp::DecoderSnapshot snapshot;
    snapshot.state = pbapp::DecoderState::Receiving;
    snapshot.descriptorKnown = true;
    snapshot.originalFileBytes = 20000;
    snapshot.verifiedRawBytes = 5000;
    snapshot.recoveryProgress = 0.25;
    snapshot.estimatedReceivedRawBytes = 5000;
    snapshot.runStartedUnixMilliseconds = 1000;
    snapshot.activeReceptionStartedUnixMilliseconds = 4000;
    snapshot.smoothedVerifiedRawGoodputBytesPerSecond = 12800;
    snapshot.etaMilliseconds = 3661001;
    const auto text = pbgui::FormatDecoderProgress(snapshot, 44000);
    CHECK(text.percent == QStringLiteral("25.0%"));
    CHECK(text.received == QStringLiteral("4.9 KB"));
    CHECK(text.elapsed == QStringLiteral("00:00:40"));
    // Average from the estimate (5000 B / 40 s) replaces the smoothed figure.
    CHECK(text.speed == QStringLiteral("0.1 KB/s"));
    CHECK(text.remaining == QStringLiteral("00:02:00"));
    snapshot.captureStallActive = true;
    const auto stalled = pbgui::FormatDecoderProgress(snapshot, 44000);
    CHECK(stalled.speed == QStringLiteral("0 KB/s"));
    CHECK(stalled.remaining == QStringLiteral("画面停滞"));
    snapshot.captureStallActive = false;
    snapshot.state = pbapp::DecoderState::Stopped;
    const auto stopped = pbgui::FormatDecoderProgress(snapshot, 44000);
    CHECK(stopped.percent == QStringLiteral("25.0%"));
    CHECK(stopped.remaining == QStringLiteral("—"));
}

TEST_CASE("Decoder GUI requires final reopen before showing one hundred percent including empty files", "[application][g22][gui-progress]")
{
    pbapp::DecoderSnapshot snapshot;
    snapshot.state = pbapp::DecoderState::Verifying;
    snapshot.descriptorKnown = true;
    snapshot.originalFileBytes = 122880;
    snapshot.verifiedRawBytes = 122880;
    snapshot.estimatedReceivedRawBytes = 122880;
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
    snapshot.runStartedUnixMilliseconds = 1000;
    snapshot.runEndedUnixMilliseconds = 61000;
    snapshot.activeReceptionStartedUnixMilliseconds = 4000;
    snapshot.activeReceptionElapsedMilliseconds = 60000;
    snapshot.averageVerifiedRawGoodputBytesPerSecond = 999;
    const auto text = pbgui::FormatDecoderProgress(snapshot);
    CHECK(text.percent == QStringLiteral("100%"));
    CHECK(text.received == QStringLiteral("120.0 KB"));
    CHECK(text.elapsed == QStringLiteral("00:01:00"));
    CHECK(text.speed == QStringLiteral("2.0 KB/s"));
    CHECK(text.remaining == QStringLiteral("00:00:00"));
    snapshot.finalPublishSucceeded = false;
    CHECK_FALSE(pbgui::IsVerifiedCompletion(snapshot));
    snapshot.finalPublishSucceeded = true;
    snapshot.originalFileBytes = 0;
    CHECK(pbgui::FormatDecoderProgress(snapshot).received == QStringLiteral("0 B"));
    CHECK(pbgui::FormatDecoderProgress(snapshot).speed == QStringLiteral("0 KB/s"));
}

TEST_CASE("Decoder GUI rejects nonfinite speeds and formats bounded large durations without overflow", "[application][g22][gui-progress]")
{
    pbapp::DecoderSnapshot snapshot;
    snapshot.state = pbapp::DecoderState::Receiving;
    snapshot.descriptorKnown = true;
    snapshot.originalFileBytes = 2000000000ULL;
    snapshot.verifiedRawBytes = 1;
    snapshot.estimatedReceivedRawBytes = 1;
    snapshot.runStartedUnixMilliseconds = 1000;
    snapshot.activeReceptionStartedUnixMilliseconds = 4000;
    // 1 B over 20 s is a tiny but finite average; the bounded ETA is days.
    const auto text = pbgui::FormatDecoderProgress(snapshot, 24000);
    CHECK(text.percent == QStringLiteral("0.0%"));
    CHECK(text.speed == QStringLiteral("0.0 KB/s"));
    CHECK(text.remaining.contains(QStringLiteral("天")));
    // A past clock never produces a negative or garbage elapsed value.
    CHECK(pbgui::FormatDecoderProgress(snapshot, 0).elapsed == QStringLiteral("—"));
}

TEST_CASE("Decoder GUI active timing handles missing, zero and frozen elapsed values", "[application][g22][gui-progress]")
{
    pbapp::DecoderSnapshot snapshot;
    snapshot.state = pbapp::DecoderState::Receiving;
    snapshot.descriptorKnown = true;
    snapshot.originalFileBytes = 4096;
    snapshot.estimatedReceivedRawBytes = 1024;
    snapshot.runStartedUnixMilliseconds = 1000;
    CHECK(pbgui::FormatDecoderProgress(snapshot, 5000).elapsed == QStringLiteral("—"));
    snapshot.activeReceptionElapsedMilliseconds = 60000;
    CHECK(pbgui::FormatDecoderProgress(snapshot, 65000).elapsed == QStringLiteral("—"));
    CHECK(pbgui::FormatDecoderProgress(snapshot, 65000).speed == QStringLiteral("0 KB/s"));
    snapshot.activeReceptionElapsedMilliseconds.reset();
    snapshot.activeReceptionStartedUnixMilliseconds = 5000;
    CHECK(pbgui::FormatDecoderProgress(snapshot, 5000).elapsed == QStringLiteral("00:00:00"));
    CHECK(pbgui::FormatDecoderProgress(snapshot, 5000).speed == QStringLiteral("0 KB/s"));
    snapshot.activeReceptionElapsedMilliseconds = 0;
    snapshot.state = pbapp::DecoderState::Completed;
    snapshot.wholeFileDigestVerified = true;
    snapshot.finalPublishSucceeded = true;
    snapshot.finalReopenVerified = true;
    snapshot.outputPath = "done.bin";
    CHECK(pbgui::FormatDecoderProgress(snapshot).elapsed == QStringLiteral("00:00:00"));
    CHECK(pbgui::FormatDecoderProgress(snapshot).speed == QStringLiteral("0 KB/s"));
}

TEST_CASE("Decoder GUI uses one numeric average and an authoritative monotonic active window", "[application][g22][gui-progress]")
{
    pbapp::DecoderSnapshot snapshot;
    snapshot.state = pbapp::DecoderState::Receiving;
    snapshot.descriptorKnown = true;
    snapshot.originalFileBytes = 122880;
    snapshot.estimatedReceivedRawBytes = 61440;
    snapshot.activeReceptionStartedUnixMilliseconds = 4000;
    snapshot.activeReceptionElapsedMilliseconds = 30000;
    // A worker elapsed value is authoritative even if the UTC clock jumps.
    const auto text = pbgui::FormatDecoderProgress(snapshot, 100);
    CHECK(text.elapsed == QStringLiteral("00:00:30"));
    CHECK(text.speed == QStringLiteral("2.0 KB/s"));
    CHECK(text.remaining == QStringLiteral("00:00:30"));
    snapshot.state = pbapp::DecoderState::Stopped;
    CHECK(pbgui::FormatDecoderProgress(snapshot, 999999).elapsed == text.elapsed);
    CHECK(pbgui::FormatDecoderProgress(snapshot, 999999).speed == text.speed);
    snapshot.state = pbapp::DecoderState::Failed;
    CHECK(pbgui::FormatDecoderProgress(snapshot, 999999).elapsed == text.elapsed);
    snapshot.state = pbapp::DecoderState::Completed;
    snapshot.wholeFileDigestVerified = true;
    snapshot.finalPublishSucceeded = true;
    snapshot.finalReopenVerified = true;
    snapshot.outputPath = "done.bin";
    snapshot.activeReceptionElapsedMilliseconds = 60001;
    snapshot.averageVerifiedRawGoodputBytesPerSecond = (std::numeric_limits<double>::infinity)();
    const auto complete = pbgui::FormatDecoderProgress(snapshot, 999999);
    CHECK(complete.elapsed == QStringLiteral("00:01:00"));
    CHECK(complete.speed == pbgui::FormatSpeed(122880.0 * 1000.0 / 60001.0));
    CHECK(complete.averageBytesPerSecond == 122880.0 * 1000.0 / 60001.0);
    snapshot.activeReceptionElapsedMilliseconds.reset();
    CHECK(pbgui::FormatDecoderProgress(snapshot, 999999).elapsed == QStringLiteral("—"));
    CHECK(pbgui::FormatDecoderProgress(snapshot, 999999).speed == QStringLiteral("0 KB/s"));
}
