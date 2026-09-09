#include "step1_gui_evidence_qt.h"

#include <catch2/catch_test_macros.hpp>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUuid>

namespace
{
QString FreshParent()
{
    const auto path = QDir(QString::fromWCharArray(PB_TEST_SCRATCH_ROOT)).filePath(QStringLiteral("step1-gui-") + QUuid::createUuid().toString(QUuid::WithoutBraces));
    REQUIRE(QDir().mkdir(path));
    return path;
}

QJsonObject ReadJson(const QString &path)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    return QJsonDocument::fromJson(file.readAll()).object();
}

pbapp::DecoderSnapshot Completed(const std::shared_ptr<pbapp::RunMeasurementRecorder> &recorder)
{
    recorder->Begin(1, pbapp::MeasurementNowNanoseconds());
    for (const auto milestone : {pbapp::RunMilestone::FirstVisualObservation, pbapp::RunMilestone::CaptureReady,
                                 pbapp::RunMilestone::FirstAcceptedBootstrap, pbapp::RunMilestone::FirstControlAccepted,
                                 pbapp::RunMilestone::WholeDigestVerified, pbapp::RunMilestone::FinalRenameSucceeded, pbapp::RunMilestone::FinalReopenVerified})
    {
        recorder->Record(milestone);
    }
    recorder->Finish(true);
    pbapp::DecoderSnapshot snapshot;
    snapshot.visualProfile = pbapp::VisualProfile::UnifiedLc4;
    snapshot.runGeneration = 1;
    snapshot.state = pbapp::DecoderState::Completed;
    snapshot.descriptorKnown = true;
    snapshot.verifiedEncodedSegmentBytes = 0;
    snapshot.wholeFileDigestCheck = true;
    snapshot.finalRenameSucceeded = true;
    snapshot.finalReopenVerified = true;
    snapshot.finalPublishSucceeded = true;
    snapshot.measurement = recorder->GetSnapshot();
    return snapshot;
}
} // namespace

TEST_CASE("Step1 evidence RunId passes both production Start validators without using the directory name", "[step1][gui-evidence][run-id]")
{
    const auto parent = FreshParent();
    const auto sourcePath = QDir(parent).filePath(QStringLiteral("fixture.bin"));
    QFile source(sourcePath);
    REQUIRE(source.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    source.close();
    const pbscreenregion::ScreenCaptureRegion region{reinterpret_cast<HMONITOR>(std::uintptr_t{1}),
        {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, 96, 96, DXGI_MODE_ROTATION_IDENTITY};
    for (const auto &role : {QStringLiteral("Encoder"), QStringLiteral("Decoder")})
    {
        QString error;
        const auto evidence = pbgui::Step1GuiEvidence::Create(QDir(parent).filePath(role), role, error);
        REQUIRE(evidence);
        CHECK(evidence->RunId().isEmpty());
        for (int runIndex = 0; runIndex < 2; runIndex++)
        {
            const auto previousRunId = evidence->RunId();
            const auto recorder = evidence->BeginRun(error);
            REQUIRE(recorder);
            const auto runId = evidence->RunId();
            const auto directoryName = QFileInfo(evidence->RunDirectory()).fileName();
            CHECK(QRegularExpression(QStringLiteral("\\A[0-9a-f]{32}\\z")).match(runId).hasMatch());
            CHECK(runId != previousRunId);
            CHECK(directoryName == QStringLiteral("run-") + QUuid::fromRfc4122(QByteArray::fromHex(runId.toLatin1())).toString(QUuid::WithoutBraces));
            REQUIRE_FALSE(evidence->BeginRun(error));
            CHECK(evidence->RunId() == runId);
            auto encoder = pbapp::MakeUnifiedEncoderConfig(sourcePath.toStdWString(), 15);
            auto decoder = pbapp::MakeUnifiedDecoderConfig(QDir(evidence->RunDirectory()).filePath(QStringLiteral("output")).toStdWString(), region);
            encoder.measurement = recorder;
            decoder.measurement = recorder;
            for (const auto &value : {std::string(), runId.toStdString()})
            {
                encoder.runId = value;
                decoder.runId = value;
                const auto encoderStatus = pbapp::ValidateEncoderConfig(encoder);
                const auto decoderStatus = pbapp::ValidateDecoderConfig(decoder);
                INFO(encoderStatus.message);
                INFO(decoderStatus.message);
                CHECK(static_cast<bool>(encoderStatus));
                CHECK(static_cast<bool>(decoderStatus));
            }
            for (const auto &invalid : {directoryName, runId.left(31), runId + QChar('0'),
                                       QString(32, QChar('A')), QString(32, QChar('g'))})
            {
                encoder.runId = invalid.toStdString();
                decoder.runId = invalid.toStdString();
                const auto encoderStatus = pbapp::ValidateEncoderConfig(encoder);
                const auto decoderStatus = pbapp::ValidateDecoderConfig(decoder);
                CHECK_FALSE(static_cast<bool>(encoderStatus));
                CHECK_FALSE(static_cast<bool>(decoderStatus));
                CHECK(encoderStatus.message.find("RunId") != std::string::npos);
                CHECK(decoderStatus.message.find("RunId") != std::string::npos);
            }
            evidence->StartRejected(QStringLiteral("fixture cleanup; no runtime was started"));
        }
    }
}

TEST_CASE("Step1 GUI evidence creates isolated runs and seals without starting any UI", "[step1][gui-evidence]")
{
    const auto parent = FreshParent();
    const auto root = QDir(parent).filePath(QStringLiteral("entry"));
    QString error;
    const auto evidence = pbgui::Step1GuiEvidence::Create(root, QStringLiteral("Decoder"), error);
    REQUIRE(evidence);
    CHECK(evidence->SettingsPath() == QDir(root).filePath(QStringLiteral("settings.ini")));
    CHECK(ReadJson(QDir(root).filePath(QStringLiteral("entry.json")))["manualStartRequired"].toBool());
    REQUIRE_FALSE(pbgui::Step1GuiEvidence::Create(root, QStringLiteral("Decoder"), error));
    const auto recorder = evidence->BeginRun(error);
    REQUIRE(recorder);
    const auto first = evidence->RunDirectory();
    REQUIRE_FALSE(evidence->BeginRun(error));
    evidence->Observe(Completed(recorder));
    const auto report = ReadJson(QDir(first).filePath(QStringLiteral("final.json")));
    CHECK(report["measurement"].toObject()["receiverTimingEligible"].toBool());
    CHECK(ReadJson(QDir(first).filePath(QStringLiteral("evidence-seal.json")))["complete"].toBool());
    REQUIRE(evidence->BeginRun(error));
    CHECK(evidence->RunDirectory() != first);
    evidence->StartRejected(QStringLiteral("fixture rejection"));
    CHECK_FALSE(ReadJson(QDir(evidence->RunDirectory()).filePath(QStringLiteral("evidence-seal.json")))["complete"].toBool());
}

TEST_CASE("Step1 GUI evidence preserves a conflicting final file and invalidates the seal", "[step1][gui-evidence]")
{
    QString error;
    const auto evidence = pbgui::Step1GuiEvidence::Create(QDir(FreshParent()).filePath(QStringLiteral("entry")), QStringLiteral("Decoder"), error);
    REQUIRE(evidence);
    const auto recorder = evidence->BeginRun(error);
    REQUIRE(recorder);
    const auto path = QDir(evidence->RunDirectory()).filePath(QStringLiteral("final.json"));
    QFile existing(path);
    REQUIRE(existing.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    REQUIRE(existing.write("preserve") == 8);
    existing.close();
    evidence->Observe(Completed(recorder));
    CHECK_FALSE(ReadJson(QDir(evidence->RunDirectory()).filePath(QStringLiteral("evidence-seal.json")))["complete"].toBool());
    REQUIRE(existing.open(QIODevice::ReadOnly));
    CHECK(existing.readAll() == "preserve");
    CHECK(recorder->GetSnapshot().failure == pbapp::MeasurementFailure::IoFailure);
}

TEST_CASE("Step1 GUI entry rejects relative network missing-parent and invalid roles without UI", "[step1][gui-evidence]")
{
    QString error;
    REQUIRE_FALSE(pbgui::Step1GuiEvidence::Create(QStringLiteral("relative"), QStringLiteral("Decoder"), error));
    REQUIRE_FALSE(pbgui::Step1GuiEvidence::Create(QStringLiteral("\\\\server\\share\\new"), QStringLiteral("Decoder"), error));
    const auto parent = FreshParent();
    REQUIRE_FALSE(pbgui::Step1GuiEvidence::Create(QDir(parent).filePath(QStringLiteral("absent/new")), QStringLiteral("Decoder"), error));
    REQUIRE_FALSE(pbgui::Step1GuiEvidence::Create(QDir(parent).filePath(QStringLiteral("new")), QStringLiteral("Unexpected"), error));
}
