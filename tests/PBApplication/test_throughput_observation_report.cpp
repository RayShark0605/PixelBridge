#include "run_report.h"

#include <catch2/catch_test_macros.hpp>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <limits>

namespace
{
const pbapp::RunReportContext context{"test", "throughput-observation", "fixture", "2026-09-08T00:00:00Z"};

QJsonObject ReadReport(const pbapp::DecoderSnapshot& snapshot)
{
    const std::string report = pbapp::BuildDecoderRunReportJson(context, snapshot);
    REQUIRE(report.size() < 65536);
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(report), &error);
    REQUIRE(error.error == QJsonParseError::NoError);
    REQUIRE(document.isObject());
    return document.object();
}
}

TEST_CASE("Throughput observation exposes existing counters without enabling new collectors", "[application][report][throughput-observation]")
{
    pbapp::DecoderSnapshot snapshot;
    snapshot.visualProfile = pbapp::VisualProfile::UnifiedLc4;
    snapshot.actualBackend = pbapp::CaptureBackend::Wgc;
    snapshot.captureEpoch = 3;
    snapshot.captureArrivedFrames = 120;
    snapshot.captureCopiedFrames = 100;
    snapshot.captureDeliveredFrames = 90;
    snapshot.captureDroppedFrames = 20;
    snapshot.captureAdmissionDrops = 2;
    snapshot.captureFrameAgeHighWater100ns = 123456;
    snapshot.bootstrapAcceptedFrames = 80;
    snapshot.bootstrapRejectedFrames = 10;
    snapshot.outerUniqueSymbols = 1000;
    snapshot.bootstrapCpuTimeTotal100ns = 700;
    snapshot.demodGpuTimeTotal100ns = 800;
    snapshot.postGpuFecCpuTimeTotal100ns = 900;
    const auto ordinary = ReadReport(snapshot);
    CHECK_FALSE(ordinary.contains("captureFlow"));
    CHECK_FALSE(ordinary.contains("stageCounters"));
    snapshot.measurement.emplace();
    auto measured = ReadReport(snapshot);
    CHECK_FALSE(snapshot.diagnostics);
    CHECK_FALSE(measured.contains("diagnostics"));
    const auto flow = measured["captureFlow"].toObject();
    CHECK(flow["available"].toBool());
    CHECK(flow["captureEpoch"].toInt() == 3);
    CHECK(flow["arrivedFrames"].toInt() == 120);
    CHECK(flow["copiedFrames"].toInt() == 100);
    CHECK(flow["deliveredFrames"].toInt() == 90);
    CHECK(flow["droppedFrames"].toInt() == 20);
    CHECK(flow["admissionDrops"].toInt() == 2);
    CHECK(flow["frameAgeHighWater100ns"].toInt() == 123456);
    CHECK(flow["lossRate"].isNull());
    CHECK_FALSE(flow["crossHostClockAligned"].toBool());
    const auto stages = measured["stageCounters"].toObject();
    CHECK(stages["bootstrapAcceptedFrames"].toInt() == 80);
    CHECK(stages["bootstrapRejectedFrames"].toInt() == 10);
    CHECK(stages["outerUniqueSymbols"].toInt() == 1000);
    CHECK(stages["bootstrapCpuTimeTotal100ns"].toInt() == 700);
    CHECK(stages["demodGpuTimeTotal100ns"].toInt() == 800);
    CHECK(stages["postGpuFecCpuTimeTotal100ns"].toInt() == 900);
    CHECK(measured["verifiedEncodedBytesPerUniqueFrame"].isNull());
    CHECK(measured["publish"].toObject()["published"] == ordinary["publish"].toObject()["published"]);
    measured.remove("captureFlow");
    measured.remove("stageCounters");
    measured.remove("measurement");
    CHECK(measured == ordinary);
    CHECK(snapshot.captureArrivedFrames == 120);
    CHECK(snapshot.outerUniqueSymbols == 1000);
}

TEST_CASE("Unavailable capture counters are null instead of fabricated zero observations", "[application][report][throughput-observation]")
{
    pbapp::DecoderSnapshot snapshot;
    snapshot.visualProfile = pbapp::VisualProfile::UnifiedLc4;
    snapshot.measurement.emplace();
    snapshot.captureArrivedFrames = 999;
    const auto flow = ReadReport(snapshot)["captureFlow"].toObject();
    CHECK_FALSE(flow["available"].toBool());
    CHECK(flow["unavailableReason"].toString() == "CaptureBackendNotObserved");
    for (const auto& name : {"captureEpoch", "arrivedFrames", "copiedFrames", "deliveredFrames", "droppedFrames",
                            "acquireTimeouts", "pointerOnlyFrames", "accumulatedFrames", "expiredFrames", "staleFrames",
                            "cursorErasures", "frameAgeHighWater100ns", "readbackDropEvents", "admissionDrops", "captureEpochResets"})
    {
        CHECK(flow[name].isNull());
    }
}

TEST_CASE("Capture epoch changes report the current snapshot without inventing a lifetime sum", "[application][report][throughput-observation]")
{
    pbapp::DecoderSnapshot snapshot;
    snapshot.visualProfile = pbapp::VisualProfile::UnifiedLc4;
    snapshot.measurement.emplace();
    snapshot.actualBackend = pbapp::CaptureBackend::Wgc;
    snapshot.captureEpoch = 1;
    snapshot.captureArrivedFrames = 100;
    CHECK(ReadReport(snapshot)["captureFlow"].toObject()["arrivedFrames"].toInt() == 100);
    snapshot.captureEpoch = 2;
    snapshot.captureArrivedFrames = 4;
    snapshot.captureEpochResets = 1;
    const auto flow = ReadReport(snapshot)["captureFlow"].toObject();
    CHECK(flow["captureEpoch"].toInt() == 2);
    CHECK(flow["arrivedFrames"].toInt() == 4);
    CHECK(flow["captureEpochResets"].toInt() == 1);
    CHECK(flow["lossRate"].isNull());
}

TEST_CASE("Throughput observation preserves uint64 counter text and stays within the evidence record limit", "[application][report][throughput-observation]")
{
    pbapp::DecoderSnapshot snapshot;
    snapshot.visualProfile = pbapp::VisualProfile::UnifiedLc4;
    snapshot.measurement.emplace();
    snapshot.actualBackend = pbapp::CaptureBackend::Dxgi;
    snapshot.captureArrivedFrames = std::numeric_limits<std::uint64_t>::max();
    snapshot.outerUniqueSymbols = std::numeric_limits<std::uint64_t>::max();
    const auto report = pbapp::BuildDecoderRunReportJson(context, snapshot);
    CHECK(report.find("\"arrivedFrames\":18446744073709551615") != std::string::npos);
    CHECK(report.find("\"outerUniqueSymbols\":18446744073709551615") != std::string::npos);
    CHECK(report.size() < 65536);
    CHECK(ReadReport(snapshot)["measurement"].toObject()["elapsedNanoseconds"].isNull());
}

TEST_CASE("Existing explicit stage diagnostics and legacy report selection remain separate", "[application][report][throughput-observation]")
{
    pbapp::DecoderSnapshot snapshot;
    snapshot.visualProfile = pbapp::VisualProfile::UnifiedLc4;
    snapshot.diagnostics = std::make_shared<pbcore::StageDiagnostics>();
    const auto diagnostic = ReadReport(snapshot);
    CHECK(diagnostic.contains("diagnostics"));
    CHECK(diagnostic.contains("stageCounters"));
    CHECK_FALSE(diagnostic.contains("captureFlow"));
    snapshot.visualProfile = pbapp::VisualProfile::DirectLevels2x2;
    snapshot.measurement.emplace();
    const auto legacy = ReadReport(snapshot);
    CHECK(legacy["schema"].toString() == "PixelBridge.RunReport.2");
    CHECK_FALSE(legacy.contains("captureFlow"));
}
