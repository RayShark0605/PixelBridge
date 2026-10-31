#include "run_report.h"
#include "evidence_journal.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Run reports and event journals distinguish rate predictions from receiver progress", "[rate-prior][report]")
{
    pbapp::EncoderSnapshot snapshot;
    snapshot.visualProfile = pbapp::VisualProfile::ExperimentalPam4;
    snapshot.ratePrior.configured = pbapp::SenderRatePrior{200, 280};
    snapshot.ratePrior.effective = true;
    snapshot.ratePrior.activeNanoseconds = 100000000000;
    snapshot.ratePrior.predictedLowerRawBytes = 20480000;
    snapshot.ratePrior.predictedUpperRawBytes = 28672000;
    snapshot.ratePrior.nearFrontierEquations = 123;
    snapshot.ratePrior.beforeFrontierEquations = 17;
    snapshot.ratePrior.clampedSenderGaps = 1;
    for (const auto& json : {pbapp::BuildEncoderRunReportJson({"Encoder", "test", "test", "test"}, snapshot),
        pbapp::BuildEncoderJournalRecord(0, snapshot)})
    {
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(json), &error);
        REQUIRE(error.error == QJsonParseError::NoError);
        const auto prior = document.object().value(QStringLiteral("senderRatePrior")).toObject();
        REQUIRE(prior.value(QStringLiteral("effective")).toBool());
        REQUIRE(prior.value(QStringLiteral("requested")).toObject().value(QStringLiteral("minimumKibPerSecond")).toInt() == 200);
        REQUIRE(prior.value(QStringLiteral("requested")).toObject().value(QStringLiteral("maximumKibPerSecond")).toInt() == 280);
        REQUIRE(prior.value(QStringLiteral("activeNanoseconds")).toDouble() == 100000000000.0);
        REQUIRE(prior.value(QStringLiteral("predictedLowerRawBytes")).toDouble() == 20480000);
        REQUIRE(prior.value(QStringLiteral("predictedUpperRawBytes")).toDouble() == 28672000);
        REQUIRE(prior.value(QStringLiteral("nearFrontierEquations")).toInt() == 123);
        REQUIRE(prior.value(QStringLiteral("beforeFrontierEquations")).toInt() == 17);
        REQUIRE(prior.value(QStringLiteral("clampedSenderGaps")).toInt() == 1);
        REQUIRE(prior.value(QStringLiteral("receiverAcknowledged")).toBool(true) == false);
    }
    snapshot.ratePrior = {};
    const auto disabled = QJsonDocument::fromJson(QByteArray::fromStdString(pbapp::BuildEncoderJournalRecord(0, snapshot)))
        .object().value(QStringLiteral("senderRatePrior")).toObject();
    REQUIRE(disabled.value(QStringLiteral("requested")).isNull());
    REQUIRE_FALSE(disabled.value(QStringLiteral("effective")).toBool(true));
}
