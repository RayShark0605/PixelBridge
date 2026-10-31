#include "run_report.h"
#include "evidence_journal.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <catch2/catch_test_macros.hpp>

namespace
{
QJsonObject Parse(const std::string& json)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(json), &error);
    REQUIRE(error.error == QJsonParseError::NoError);
    return document.object();
}
const pbapp::RunReportContext context{"test", "test", "test", "test"};
}

TEST_CASE("Disabled feedback is explicit and does not invent receiver truth or expose status text", "[feedback][report]")
{
    pbapp::EncoderSnapshot snapshot;
    snapshot.feedback.statusMessage = "sensitive-feedback-path-and-payload-sentinel";
    for (const auto profile : {pbapp::VisualProfile::DirectLevels2x2, pbapp::VisualProfile::UnifiedLc4,
        pbapp::VisualProfile::UnifiedGrayFast, pbapp::VisualProfile::ExperimentalPam4, pbapp::VisualProfile::ExperimentalPam4Wide})
    {
        snapshot.visualProfile = profile;
        for (const auto& json : {pbapp::BuildEncoderRunReportJson(context, snapshot), pbapp::BuildEncoderJournalRecord(0, snapshot)})
        {
            REQUIRE(json.find("sensitive-feedback-path-and-payload-sentinel") == std::string::npos);
            const auto feedback = Parse(json).value(QStringLiteral("feedback")).toObject();
            REQUIRE_FALSE(feedback.isEmpty());
            REQUIRE_FALSE(feedback.value(QStringLiteral("requested")).toBool(true));
            REQUIRE_FALSE(feedback.value(QStringLiteral("effective")).toBool(true));
            REQUIRE(feedback.value(QStringLiteral("state")).toString() == QStringLiteral("Disabled"));
            REQUIRE(feedback.value(QStringLiteral("confirmedDurableSegments")).isNull());
            REQUIRE(feedback.value(QStringLiteral("observedFrameSequence")).isNull());
            REQUIRE(feedback.value(QStringLiteral("receiverReportedCompletionMilliseconds")).isNull());
            REQUIRE_FALSE(feedback.contains(QStringLiteral("path")));
        }
    }
}

TEST_CASE("Encoder reports receiver claims separately and gives automatic stop a typed reason", "[feedback][report]")
{
    pbapp::EncoderSnapshot snapshot;
    snapshot.visualProfile = pbapp::VisualProfile::ExperimentalPam4;
    snapshot.state = pbapp::EncoderState::Stopped;
    snapshot.stopReason = pbapp::EncoderStopReason::ReceiverVerified;
    snapshot.feedback.requested = true;
    snapshot.feedback.effective = true;
    snapshot.feedback.state = pbapp::FeedbackConnectionState::ReceiverReportedComplete;
    snapshot.feedback.confirmedSegments = 13;
    snapshot.feedback.observedFrameAvailable = true;
    snapshot.feedback.observedFrameSequence = 4096;
    snapshot.feedback.terminalAccepted = true;
    snapshot.feedback.receiverReportedCompletionMilliseconds = 12345;
    snapshot.feedback.skippedService = 9;
    const auto report = Parse(pbapp::BuildEncoderRunReportJson(context, snapshot));
    REQUIRE(report.value(QStringLiteral("receiverProgress")).isNull());
    REQUIRE(report.value(QStringLiteral("receiverEta")).isNull());
    REQUIRE(report.value(QStringLiteral("verifiedGoodput")).isNull());
    for (const auto& object : {report, Parse(pbapp::BuildEncoderJournalRecord(0, snapshot))})
    {
        REQUIRE(object.value(QStringLiteral("stopReason")).toString() == QStringLiteral("ReceiverVerified"));
        const auto feedback = object.value(QStringLiteral("feedback")).toObject();
        REQUIRE(feedback.value(QStringLiteral("terminalAccepted")).toBool());
        REQUIRE_FALSE(feedback.value(QStringLiteral("terminalPublished")).toBool(true));
        REQUIRE(feedback.value(QStringLiteral("confirmedDurableSegments")).toInt() == 13);
        REQUIRE(feedback.value(QStringLiteral("receiverReportedCompletionMilliseconds")).toInt() == 12345);
        REQUIRE(feedback.value(QStringLiteral("skippedSegmentServiceVisits")).toInt() == 9);
        REQUIRE(feedback.value(QStringLiteral("metadataOnly")).toBool());
        REQUIRE_FALSE(feedback.value(QStringLiteral("authenticated")).toBool(true));
    }
    for (const auto state : {pbapp::FeedbackConnectionState::Waiting, pbapp::FeedbackConnectionState::Stale, pbapp::FeedbackConnectionState::Rejected})
    {
        snapshot.feedback.state = state;
        snapshot.feedback.effective = false;
        snapshot.feedback.terminalAccepted = false;
        snapshot.feedback.lastError = pbapp::FeedbackError::Conflict;
        const auto feedback = Parse(pbapp::BuildEncoderRunReportJson(context, snapshot)).value(QStringLiteral("feedback")).toObject();
        REQUIRE_FALSE(feedback.value(QStringLiteral("effective")).toBool(true));
        REQUIRE(feedback.value(QStringLiteral("lastError")).toString() == QStringLiteral("Conflict"));
        REQUIRE(feedback.value(QStringLiteral("confirmedDurableSegments")).isNull());
        REQUIRE(feedback.value(QStringLiteral("receiverReportedCompletionMilliseconds")).isNull());
    }
}

TEST_CASE("Decoder reports terminal publication and listener time separately from frozen file completion", "[feedback][report]")
{
    pbapp::DecoderSnapshot snapshot;
    snapshot.visualProfile = pbapp::VisualProfile::UnifiedLc4;
    snapshot.state = pbapp::DecoderState::Completed;
    snapshot.recoveryRuntimeMilliseconds = 1500;
    snapshot.runEndedUnixMilliseconds = 2000;
    snapshot.feedback.requested = true;
    snapshot.feedback.effective = true;
    snapshot.feedback.state = pbapp::FeedbackConnectionState::Active;
    snapshot.feedback.listening = true;
    snapshot.feedback.terminalPublished = true;
    snapshot.feedback.listenerStartedUnixMilliseconds = 2000;
    snapshot.feedback.listenerElapsedMilliseconds = 9000;
    for (const auto& json : {pbapp::BuildDecoderRunReportJson(context, snapshot), pbapp::BuildDecoderJournalRecord(0, snapshot)})
    {
        const auto feedback = Parse(json).value(QStringLiteral("feedback")).toObject();
        REQUIRE(feedback.value(QStringLiteral("terminalPublished")).toBool());
        REQUIRE_FALSE(feedback.value(QStringLiteral("terminalAccepted")).toBool(true));
        REQUIRE(feedback.value(QStringLiteral("listening")).toBool());
        REQUIRE(feedback.value(QStringLiteral("listenerElapsedMilliseconds")).toInt() == 9000);
        REQUIRE(feedback.value(QStringLiteral("listenerStoppedUnixMilliseconds")).isNull());
        REQUIRE(feedback.value(QStringLiteral("receiverReportedCompletionMilliseconds")).isNull());
    }
    const auto report = Parse(pbapp::BuildDecoderRunReportJson(context, snapshot));
    REQUIRE(report.value(QStringLiteral("recoveryRuntimeMilliseconds")).toInt() == 1500);
    REQUIRE(report.value(QStringLiteral("runEndedUnixMilliseconds")).toInt() == 2000);
}
