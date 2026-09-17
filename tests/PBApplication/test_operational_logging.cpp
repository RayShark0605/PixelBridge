#include "operational_log_qt.h"

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

namespace
{

QByteArray Read(const QString& path)
{
    QFile input(path);
    REQUIRE(input.open(QIODevice::ReadOnly));
    return input.readAll();
}

pbapp::DecoderSnapshot Receiving()
{
    pbapp::DecoderSnapshot snapshot;
    snapshot.runGeneration = 1;
    snapshot.state = pbapp::DecoderState::Recovering;
    snapshot.descriptorKnown = true;
    snapshot.originalFileBytes = 10000;
    return snapshot;
}

} // namespace

TEST_CASE("Activity distinguishes normal repair waits from missing capture and resource backpressure", "[activity]")
{
    auto snapshot = Receiving();
    pbapp::DecoderActivityTracker tracker;
    CHECK(tracker.Observe(snapshot, 0).code == "Receiving");
    snapshot.outerUniqueSymbols++;
    const auto repair = tracker.Observe(snapshot, 6000);
    CHECK(repair.code == "CollectingRepairSymbols");
    CHECK(repair.normalWait);
    CHECK(pbgui::FormatDecoderActivity(repair).contains(QStringLiteral("不要暂停")));
    snapshot.outerDeferredResourceBusyCount++;
    const auto resource = tracker.Observe(snapshot, 6100);
    CHECK(resource.code == "ResourceBackpressure");
    CHECK_FALSE(resource.normalWait);
    snapshot.captureStallActive = true;
    CHECK(tracker.Observe(snapshot, 6200).code == "CaptureInterrupted");
    snapshot.captureStallActive = false;
    snapshot.visualStallActive = true;
    CHECK(tracker.Observe(snapshot, 6300).code == "NoFreshVisualData");
    snapshot.visualStallActive = false;
    snapshot.outerAlreadyCompletedSymbols++;
    CHECK(tracker.Observe(snapshot, 15000).code == "WaitingForCarousel");
    const auto noProgress = tracker.Observe(snapshot, 40000);
    CHECK(noProgress.code == "NoUsefulProgress");
    CHECK_FALSE(noProgress.normalWait);
    CHECK_FALSE(pbgui::FormatDecoderActivity(noProgress).contains(QStringLiteral("属于正常现象")));
    snapshot.estimatedReceivedRawBytes = 100;
    snapshot.outerUniqueSymbols++;
    CHECK(tracker.Observe(snapshot, 40100).code == "Receiving");
    snapshot.runGeneration++;
    CHECK(tracker.Observe(snapshot, 40200).noSizeGrowthMilliseconds == 0);
    CHECK(tracker.Observe(snapshot, 0).noNewSymbolMilliseconds == 0);
}

TEST_CASE("Verifying state alone is not evidence of active disk work; completion still requires reopen", "[activity]")
{
    auto snapshot = Receiving();
    pbapp::DecoderActivityTracker tracker;
    snapshot.state = pbapp::DecoderState::Verifying;
    CHECK(tracker.Observe(snapshot, 0).code == "Receiving");
    for (const std::string operation : {"VerifySegment", "WriteSegmentAndCheckpoint", "VerifyAndPublishFile", "CheckpointResume"})
    {
        snapshot.activeRecoveryOperation = operation;
        const auto activity = tracker.Observe(snapshot, 6000);
        CHECK(activity.code == operation);
        CHECK(activity.normalWait);
        CHECK(pbgui::FormatDecoderActivity(activity).contains(QStringLiteral("不要暂停")));
    }
    snapshot.activeRecoveryOperation = "UnknownOperation";
    const auto unknown = tracker.Observe(snapshot, 6400);
    CHECK_FALSE(unknown.normalWait);
    CHECK_FALSE(pbgui::FormatDecoderActivity(unknown).contains(QStringLiteral("属于正常现象")));
    snapshot.state = pbapp::DecoderState::Failed;
    CHECK_FALSE(tracker.Observe(snapshot, 6500).normalWait);
    snapshot.state = pbapp::DecoderState::Completed;
    snapshot.wholeFileDigestVerified = snapshot.finalPublishSucceeded = true;
    CHECK(tracker.Observe(snapshot, 7000).code == "CompletionUnverified");
    snapshot.finalReopenVerified = true;
    CHECK(tracker.Observe(snapshot, 7100).code == "Completed");
}

TEST_CASE("Automatic logs sample before serialization and preserve terminal report and escaped errors", "[operational-log]")
{
    QTemporaryDir root;
    REQUIRE(root.isValid());
    pbgui::OperationalLog log(QStringLiteral("Decoder"), root.path());
    log.Begin();
    auto snapshot = Receiving();
    snapshot.errorDetail = "quote\" newline\n control\t";
    snapshot.pam4DecodedObservations = 17;
    for (int index = 0; index < 100; index++)
    {
        log.Observe(snapshot);
    }
    snapshot.state = pbapp::DecoderState::Stopped;
    // Intermediate terminal-looking state is not a final cleanup report.
    log.Observe(snapshot);
    CHECK(QDir(root.path()).entryList({QStringLiteral("*.summary.json")}, QDir::Files).isEmpty());
    snapshot.errorDetail = "post-publish cleanup warning";
    log.Observe(snapshot, true);
    log.Observe(snapshot, true);
    const auto events = QDir(root.path()).entryList({QStringLiteral("*.events.jsonl")}, QDir::Files);
    const auto summaries = QDir(root.path()).entryList({QStringLiteral("*.summary.json")}, QDir::Files);
    REQUIRE(events.size() == 1);
    REQUIRE(summaries.size() == 1);
    const auto lines = Read(QDir(root.path()).filePath(events.front())).trimmed().split('\n');
    REQUIRE(lines.size() == 3);
    for (const auto& line : lines)
    {
        const auto document = QJsonDocument::fromJson(line);
        REQUIRE(document.isObject());
        CHECK(document.object()["errorDetail"].toString() == (line == lines.back() ? QString::fromStdString(snapshot.errorDetail) : QStringLiteral("quote\" newline\n control\t")));
        CHECK(document.object()["pam4DecodedObservations"].toInt() == 17);
        CHECK(document.object().contains("elapsedMs"));
        CHECK(document.object().contains("activity"));
        CHECK(document.object().contains("processPrivateBytes"));
    }
    CHECK(QJsonDocument::fromJson(lines.back()).object()["terminal"].toBool());
    const auto report = QJsonDocument::fromJson(Read(QDir(root.path()).filePath(summaries.front()))).object();
    CHECK(report["operationalLog"].toObject()["samples"].toInt() == 3);
    CHECK(report["operationalLog"].toObject()["valid"].toBool());
}

TEST_CASE("Automatic Encoder logs retain only owned completed logs; unrelated files survive", "[operational-log]")
{
    QTemporaryDir root;
    REQUIRE(root.isValid());
    QFile foreign(root.filePath(QStringLiteral("do-not-delete.txt")));
    REQUIRE(foreign.open(QIODevice::WriteOnly));
    foreign.write("preserve");
    foreign.close();
    for (int index = 0; index < 10; index++)
    {
        pbgui::OperationalLog log(QStringLiteral("Encoder"), root.path());
        log.Begin();
        pbapp::EncoderSnapshot snapshot;
        snapshot.state = pbapp::EncoderState::Preparing;
        log.Observe(snapshot);
        snapshot.state = pbapp::EncoderState::Stopped;
        log.Observe(snapshot, true);
    }
    CHECK(QDir(root.path()).entryList({QStringLiteral("*.events.jsonl")}, QDir::Files).size() == 8);
    CHECK(QDir(root.path()).entryList({QStringLiteral("*.summary.json")}, QDir::Files).size() == 8);
    CHECK(Read(foreign.fileName()) == "preserve");
}

TEST_CASE("Log path failures do not suppress independent activity and rejected starts get diagnostics", "[operational-log]")
{
    QTemporaryDir root;
    REQUIRE(root.isValid());
    QFile obstruction(root.filePath(QStringLiteral("not-a-directory")));
    REQUIRE(obstruction.open(QIODevice::WriteOnly));
    obstruction.close();
    pbgui::OperationalLog broken(QStringLiteral("Decoder"), obstruction.fileName());
    broken.Begin();
    auto snapshot = Receiving();
    snapshot.captureStallActive = true;
    broken.Observe(snapshot);
    CHECK(broken.GetDecoderActivity().code == "CaptureInterrupted");
    CHECK(broken.StatusText().contains(QStringLiteral("日志异常")));
    pbgui::OperationalLog rejected(QStringLiteral("Encoder"), root.path());
    rejected.Begin();
    rejected.StartRejected(QStringLiteral("invalid configuration"));
    const auto summaries = QDir(root.path()).entryList({QStringLiteral("*.summary.json")}, QDir::Files);
    REQUIRE(summaries.size() == 1);
    const auto report = QJsonDocument::fromJson(Read(root.filePath(summaries.front()))).object();
    CHECK(report["event"].toString() == QStringLiteral("StartRejected"));
    CHECK(report["role"].toString() == QStringLiteral("Encoder"));
    CHECK(report["terminal"].toBool());
    CHECK(report["unixMs"].toInteger() > 0);
    CHECK_FALSE(report["version"].toString().isEmpty());
}
