#include "local_desktop_runtime.h"
#include "run_report.h"

#include <catch2/catch_test_macros.hpp>

#include <QSettings>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QStringList>
#include <QTemporaryDir>

#include <limits>

TEST_CASE("QSettings persists only local UI preferences and cannot replace core Session state",
    "[application][qsettings]")
{
    QTemporaryDir scratch;
    REQUIRE(scratch.isValid());
    const QString path = scratch.filePath(QStringLiteral("preferences.ini"));
    {
        QSettings settings(path, QSettings::IniFormat);
        settings.setValue(QStringLiteral("ui/windowGeometry"), QByteArray("geometry"));
        settings.setValue(QStringLiteral("ui/lastInputPath"), QStringLiteral("C:/input.bin"));
        settings.setValue(QStringLiteral("ui/compressionEnabled"), true);
        settings.setValue(QStringLiteral("ui/backend"), 1);
        settings.setValue(QStringLiteral("ui/advancedExpanded"), true);
        settings.setValue(QStringLiteral("ui/lastMonitorDevice"), QStringLiteral("\\\\.\\DISPLAY2"));
        settings.sync();
        REQUIRE(settings.status() == QSettings::NoError);
    }
    QSettings settings(path, QSettings::IniFormat);
    const QStringList keys = settings.allKeys();
    REQUIRE(keys.contains(QStringLiteral("ui/windowGeometry")));
    REQUIRE(keys.contains(QStringLiteral("ui/lastInputPath")));
    REQUIRE(keys.contains(QStringLiteral("ui/compressionEnabled")));
    REQUIRE(keys.contains(QStringLiteral("ui/backend")));
    REQUIRE(keys.contains(QStringLiteral("ui/advancedExpanded")));
    REQUIRE(keys.contains(QStringLiteral("ui/lastMonitorDevice")));
    for (const QString& key : keys)
    {
        REQUIRE_FALSE(key.contains(QStringLiteral("session"), Qt::CaseInsensitive));
        REQUIRE_FALSE(key.contains(QStringLiteral("wirehair"), Qt::CaseInsensitive));
        REQUIRE_FALSE(key.contains(QStringLiteral("resume"), Qt::CaseInsensitive));
        REQUIRE_FALSE(key.contains(QStringLiteral("fecState"), Qt::CaseInsensitive));
        REQUIRE_FALSE(key.contains(QStringLiteral("protocol"), Qt::CaseInsensitive));
    }

    pbapp::EncoderConfig encoderConfig;
    encoderConfig.compressionEnabled = false;
    encoderConfig.compressionLevel = 3;
    encoderConfig.visualProfile = pbapp::VisualProfile::DirectLevels2x2;
    const pbapp::EncoderConfig coreBefore = encoderConfig;
    const bool localCompressionPreference = settings.value(QStringLiteral("ui/compressionEnabled")).toBool();
    REQUIRE(localCompressionPreference);
    REQUIRE(encoderConfig.compressionEnabled == coreBefore.compressionEnabled);
    REQUIRE(encoderConfig.compressionLevel == coreBefore.compressionLevel);
    REQUIRE(encoderConfig.visualProfile == coreBefore.visualProfile);
}

TEST_CASE("Run report remains valid JSON for non-finite optional application metadata",
    "[application][report][json]")
{
    pbapp::EncoderSnapshot snapshot;
    snapshot.remoteMetadata.observedFps = (std::numeric_limits<double>::quiet_NaN)();
    snapshot.presentedVisualFps = (std::numeric_limits<double>::infinity)();
    const pbapp::RunReportContext context{"PixelBridgeEncoder", "0.1.0", "test", "2026-08-30T00:00:00Z"};
    const std::string report = pbapp::BuildEncoderRunReportJson(context, snapshot);
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(QByteArray(report.data(),
        static_cast<int>(report.size())), &error);
    REQUIRE(error.error == QJsonParseError::NoError);
    REQUIRE(document.isObject());
}
