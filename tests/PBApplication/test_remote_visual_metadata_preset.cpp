#include "remote_visual_metadata_preset_qt.h"
#include "run_report.h"

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

#include <cstdint>
#include <string>

namespace
{

[[nodiscard]] pbapp::RemoteRunMetadata MakeMetadata()
{
    pbapp::RemoteRunMetadata metadata;
    metadata.runId = "0123456789abcdef0123456789abcdef";
    metadata.channelType = pbapp::ChannelType::RemoteVisual;
    metadata.remoteProvider = "TestRemote";
    metadata.providerVersion = "15.8.3";
    metadata.remoteMode = "HighQuality";
    metadata.targetFps = 60.0;
    metadata.observedFps = 47.5;
    metadata.chromaMode = pbapp::ChromaMode::Chroma420;
    metadata.computerBDisplayResolution = "1920x1080";
    metadata.computerBRefreshRate = 60.0;
    metadata.computerADisplayResolution = "2560x1440";
    metadata.computerARefreshRate = 180.0;
    metadata.remoteResolution = "1920x1080";
    metadata.remoteWindowPhysicalRect = pbapp::MetadataPhysicalRect{2560, 0, 5120, 1440};
    metadata.selectedRoiPhysicalRect = pbapp::MetadataPhysicalRect{2880, 180, 4800, 1260};
    metadata.estimatedScaleX = 1.0;
    metadata.estimatedScaleY = 1.0;
    metadata.letterboxStatus = "Present";
    metadata.cropStatus = "None";
    metadata.geometryStatus = "CompatibleStrictPhysical1:1";
    metadata.networkType = "Ethernet";
    metadata.observedBandwidthMbps = 18.25;
    metadata.observedLatencyMilliseconds = 31.0;
    metadata.protectedMonitorIdentity = R"(\\.\DISPLAY1)";
    metadata.experimentMonitorIdentity = R"(\\.\DISPLAY2)";
    metadata.remoteUiProvenance = pbapp::MetadataProvenance::RemoteUiVisible;
    metadata.geometryProvenance = pbapp::MetadataProvenance::PixelBridgeObserved;
    metadata.networkProvenance = pbapp::MetadataProvenance::Manual;
    metadata.notes = "右侧 ROI only";
    return metadata;
}

[[nodiscard]] QByteArray SerializeMetadata(const pbapp::RemoteRunMetadata& metadata)
{
    const std::string json = pbapp::BuildRemoteVisualRunMetadataJson(metadata);
    return QByteArray::fromStdString(json);
}

[[nodiscard]] bool WriteBytes(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size() && file.flush();
}

[[nodiscard]] QJsonObject ParseObject(const QByteArray& bytes)
{
    const QJsonDocument document = QJsonDocument::fromJson(bytes);
    REQUIRE(document.isObject());
    return document.object();
}

[[nodiscard]] bool LoadFailsWithoutMutation(const QString& path, const QByteArray& bytes, QString& errorMessage)
{
    if (!WriteBytes(path, bytes))
    {
        return false;
    }
    pbapp::RemoteRunMetadata output = MakeMetadata();
    output.remoteProvider = "sentinel-provider";
    output.notes = "sentinel-notes";
    const QByteArray before = SerializeMetadata(output);
    const bool loaded = pbapp::LoadRemoteVisualMetadataPreset(path, output, errorMessage);
    return !loaded && !errorMessage.isEmpty() && SerializeMetadata(output) == before;
}

} // namespace

TEST_CASE("RemoteVisual metadata preset round-trips every versioned evidence field",
    "[application][remote-visual][metadata]")
{
    QTemporaryDir scratch;
    REQUIRE(scratch.isValid());
    const QString path = scratch.filePath(QStringLiteral("remote-metadata.json"));
    const pbapp::RemoteRunMetadata expected = MakeMetadata();
    REQUIRE(WriteBytes(path, SerializeMetadata(expected)));

    pbapp::RemoteRunMetadata actual;
    QString errorMessage;
    REQUIRE(pbapp::LoadRemoteVisualMetadataPreset(path, actual, errorMessage));
    REQUIRE(errorMessage.isEmpty());
    REQUIRE(SerializeMetadata(actual) == SerializeMetadata(expected));
}

TEST_CASE("RemoteVisual metadata preset rejects duplicate and escaped-equivalent keys atomically",
    "[application][remote-visual][metadata][duplicate]")
{
    QTemporaryDir scratch;
    REQUIRE(scratch.isValid());
    const QByteArray valid = SerializeMetadata(MakeMetadata());
    REQUIRE(valid.startsWith('{'));
    QString errorMessage;

    QByteArray duplicate = valid;
    duplicate.insert(1, R"("schema":"PixelBridge.RemoteVisualRunMetadata.1",)");
    REQUIRE(LoadFailsWithoutMutation(scratch.filePath(QStringLiteral("duplicate.json")), duplicate, errorMessage));
    REQUIRE(errorMessage.contains(QStringLiteral("duplicate"), Qt::CaseInsensitive));

    QByteArray escapedDuplicate = valid;
    escapedDuplicate.insert(1, R"("\u0073chema":"PixelBridge.RemoteVisualRunMetadata.1",)");
    REQUIRE(LoadFailsWithoutMutation(scratch.filePath(QStringLiteral("escaped-duplicate.json")),
        escapedDuplicate, errorMessage));
    REQUIRE(errorMessage.contains(QStringLiteral("duplicate"), Qt::CaseInsensitive));
}

TEST_CASE("RemoteVisual metadata preset enforces exact schema enums numbers and physical rectangles",
    "[application][remote-visual][metadata][schema]")
{
    QTemporaryDir scratch;
    REQUIRE(scratch.isValid());
    const QByteArray valid = SerializeMetadata(MakeMetadata());
    QString errorMessage;

    QJsonObject unknownKey = ParseObject(valid);
    unknownKey.insert(QStringLiteral("providerMagicThreshold"), 17);
    REQUIRE(LoadFailsWithoutMutation(scratch.filePath(QStringLiteral("unknown-key.json")),
        QJsonDocument(unknownKey).toJson(QJsonDocument::Compact), errorMessage));

    QJsonObject missingKey = ParseObject(valid);
    missingKey.remove(QStringLiteral("geometryStatus"));
    REQUIRE(LoadFailsWithoutMutation(scratch.filePath(QStringLiteral("missing-key.json")),
        QJsonDocument(missingKey).toJson(QJsonDocument::Compact), errorMessage));

    QJsonObject wrongSchema = ParseObject(valid);
    wrongSchema.insert(QStringLiteral("schema"), QStringLiteral("PixelBridge.RemoteVisualRunMetadata.2"));
    REQUIRE(LoadFailsWithoutMutation(scratch.filePath(QStringLiteral("wrong-schema.json")),
        QJsonDocument(wrongSchema).toJson(QJsonDocument::Compact), errorMessage));

    QJsonObject wrongEnum = ParseObject(valid);
    wrongEnum.insert(QStringLiteral("chromaMode"), QStringLiteral("RGB"));
    REQUIRE(LoadFailsWithoutMutation(scratch.filePath(QStringLiteral("wrong-enum.json")),
        QJsonDocument(wrongEnum).toJson(QJsonDocument::Compact), errorMessage));

    QJsonObject wrongRunId = ParseObject(valid);
    wrongRunId.insert(QStringLiteral("runId"), QStringLiteral("0123456789ABCDEF0123456789ABCDEF"));
    REQUIRE(LoadFailsWithoutMutation(scratch.filePath(QStringLiteral("wrong-run-id.json")),
        QJsonDocument(wrongRunId).toJson(QJsonDocument::Compact), errorMessage));

    QJsonObject wrongType = ParseObject(valid);
    wrongType.insert(QStringLiteral("targetFps"), QStringLiteral("60"));
    REQUIRE(LoadFailsWithoutMutation(scratch.filePath(QStringLiteral("wrong-type.json")),
        QJsonDocument(wrongType).toJson(QJsonDocument::Compact), errorMessage));

    QJsonObject invalidRectangle = ParseObject(valid);
    QJsonObject rectangle = invalidRectangle.value(QStringLiteral("selectedRoiPhysicalRect")).toObject();
    rectangle.insert(QStringLiteral("right"), rectangle.value(QStringLiteral("left")));
    invalidRectangle.insert(QStringLiteral("selectedRoiPhysicalRect"), rectangle);
    REQUIRE(LoadFailsWithoutMutation(scratch.filePath(QStringLiteral("invalid-rectangle.json")),
        QJsonDocument(invalidRectangle).toJson(QJsonDocument::Compact), errorMessage));

    QJsonObject invalidScale = ParseObject(valid);
    invalidScale.insert(QStringLiteral("estimatedScaleX"), 16.0001);
    REQUIRE(LoadFailsWithoutMutation(scratch.filePath(QStringLiteral("invalid-scale.json")),
        QJsonDocument(invalidScale).toJson(QJsonDocument::Compact), errorMessage));
}

TEST_CASE("RemoteVisual metadata preset rejects invalid UTF-8 and bounded-resource violations",
    "[application][remote-visual][metadata][resources]")
{
    QTemporaryDir scratch;
    REQUIRE(scratch.isValid());
    QString errorMessage;

    QByteArray invalidUtf8 = SerializeMetadata(MakeMetadata());
    const auto notesOffset = invalidUtf8.indexOf("right-monitor");
    if (notesOffset >= 0)
    {
        invalidUtf8[static_cast<int>(notesOffset)] = static_cast<char>(0xC0);
    }
    else
    {
        invalidUtf8.insert(1, QByteArray("\"bad\":\"") + QByteArray(1, static_cast<char>(0xC0)) + QByteArray("\","));
    }
    REQUIRE(LoadFailsWithoutMutation(scratch.filePath(QStringLiteral("invalid-utf8.json")), invalidUtf8,
        errorMessage));
    REQUIRE(errorMessage.contains(QStringLiteral("UTF-8"), Qt::CaseInsensitive));

    QJsonObject longString = ParseObject(SerializeMetadata(MakeMetadata()));
    longString.insert(QStringLiteral("notes"), QString(1025, QLatin1Char('x')));
    REQUIRE(LoadFailsWithoutMutation(scratch.filePath(QStringLiteral("long-string.json")),
        QJsonDocument(longString).toJson(QJsonDocument::Compact), errorMessage));

    QJsonObject embeddedNul = ParseObject(SerializeMetadata(MakeMetadata()));
    embeddedNul.insert(QStringLiteral("notes"), QString::fromUtf8("left\0right", 10));
    REQUIRE(LoadFailsWithoutMutation(scratch.filePath(QStringLiteral("embedded-nul.json")),
        QJsonDocument(embeddedNul).toJson(QJsonDocument::Compact), errorMessage));
    REQUIRE(errorMessage.contains(QStringLiteral("U+0000")));

    const QByteArray oversized(256 * 1024 + 1, ' ');
    REQUIRE(LoadFailsWithoutMutation(scratch.filePath(QStringLiteral("oversized.json")), oversized,
        errorMessage));
    REQUIRE(errorMessage.contains(QStringLiteral("1..262144")));
}

TEST_CASE("RemoteVisual metadata preset requires a visible provider name",
    "[application][remote-visual][metadata][provider]")
{
    QTemporaryDir scratch;
    REQUIRE(scratch.isValid());
    QString errorMessage;
    QJsonObject whitespaceProvider = ParseObject(SerializeMetadata(MakeMetadata()));
    whitespaceProvider.insert(QStringLiteral("remoteProvider"), QStringLiteral(" \t\r\n "));
    REQUIRE(LoadFailsWithoutMutation(scratch.filePath(QStringLiteral("whitespace-provider.json")),
        QJsonDocument(whitespaceProvider).toJson(QJsonDocument::Compact), errorMessage));
    REQUIRE(errorMessage.contains(QStringLiteral("provider"), Qt::CaseInsensitive));
}
