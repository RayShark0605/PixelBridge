#include "remote_visual_metadata_preset_qt.h"

#include <QByteArray>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSet>
#include <QStringList>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace pbapp
{
namespace
{

inline constexpr qint64 maximumPresetBytes = 256 * 1024;
inline constexpr std::size_t maximumJsonNodes = 4096;
inline constexpr std::size_t maximumJsonDepth = 8;
using ByteIndex = decltype(std::declval<const QByteArray&>().size());

[[nodiscard]] bool IsStrictUtf8(const QByteArray& bytes) noexcept
{
    const auto* const data = reinterpret_cast<const unsigned char*>(bytes.constData());
    const std::size_t size = static_cast<std::size_t>(bytes.size());
    std::size_t position = 0;
    while (position < size)
    {
        const unsigned char lead = data[position];
        if (lead <= 0x7FU)
        {
            position++;
            continue;
        }
        std::size_t trailingBytes = 0;
        std::uint32_t codePoint = 0;
        std::uint32_t minimumCodePoint = 0;
        if (lead >= 0xC2U && lead <= 0xDFU)
        {
            trailingBytes = 1;
            codePoint = lead & 0x1FU;
            minimumCodePoint = 0x80U;
        }
        else if (lead >= 0xE0U && lead <= 0xEFU)
        {
            trailingBytes = 2;
            codePoint = lead & 0x0FU;
            minimumCodePoint = 0x800U;
        }
        else if (lead >= 0xF0U && lead <= 0xF4U)
        {
            trailingBytes = 3;
            codePoint = lead & 0x07U;
            minimumCodePoint = 0x10000U;
        }
        else
        {
            return false;
        }
        if (trailingBytes > size - position - 1)
        {
            return false;
        }
        for (std::size_t index = 1; index <= trailingBytes; index++)
        {
            const unsigned char continuation = data[position + index];
            if ((continuation & 0xC0U) != 0x80U)
            {
                return false;
            }
            codePoint = (codePoint << 6U) | (continuation & 0x3FU);
        }
        if (codePoint < minimumCodePoint || codePoint > 0x10FFFFU ||
            (codePoint >= 0xD800U && codePoint <= 0xDFFFU))
        {
            return false;
        }
        position += trailingBytes + 1;
    }
    return true;
}

class DuplicateKeyScanner
{
public:
    explicit DuplicateKeyScanner(const QByteArray& bytes) noexcept : bytes_(bytes)
    {
    }

    [[nodiscard]] bool Scan(QString& errorMessage)
    {
        if (!ScanValue(0, errorMessage))
        {
            return false;
        }
        SkipWhitespace();
        if (position_ != bytes_.size())
        {
            errorMessage = QStringLiteral("metadata preset contains trailing JSON input");
            return false;
        }
        return true;
    }

private:
    void SkipWhitespace() noexcept
    {
        while (position_ < bytes_.size() && (bytes_[position_] == ' ' || bytes_[position_] == '\t' ||
            bytes_[position_] == '\r' || bytes_[position_] == '\n'))
        {
            position_++;
        }
    }

    [[nodiscard]] bool Charge(const std::size_t depth, QString& errorMessage)
    {
        nodes_++;
        if (nodes_ > maximumJsonNodes || depth > maximumJsonDepth)
        {
            errorMessage = QStringLiteral("metadata preset exceeds its JSON depth/node limit");
            return false;
        }
        return true;
    }

    [[nodiscard]] bool ScanString(QByteArray& token, QString& errorMessage)
    {
        SkipWhitespace();
        if (position_ >= bytes_.size() || bytes_[position_] != '"')
        {
            errorMessage = QStringLiteral("metadata preset object key is not a JSON string");
            return false;
        }
        const ByteIndex start = position_++;
        bool escaped = false;
        while (position_ < bytes_.size())
        {
            const char character = bytes_[position_++];
            if (escaped)
            {
                escaped = false;
                continue;
            }
            if (character == '\\')
            {
                escaped = true;
                continue;
            }
            if (character == '"')
            {
                token = bytes_.mid(start, position_ - start);
                return true;
            }
        }
        errorMessage = QStringLiteral("metadata preset contains an unterminated JSON string");
        return false;
    }

    [[nodiscard]] static bool DecodeKey(const QByteArray& token, QString& output, QString& errorMessage)
    {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(QByteArray("[") + token + QByteArray("]"), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isArray() || document.array().size() != 1 ||
            !document.array().at(0).isString())
        {
            errorMessage = QStringLiteral("metadata preset contains an invalid escaped JSON key");
            return false;
        }
        output = document.array().at(0).toString();
        return true;
    }

    [[nodiscard]] bool ScanObject(const std::size_t depth, QString& errorMessage)
    {
        position_++;
        QSet<QString> keys;
        SkipWhitespace();
        if (position_ < bytes_.size() && bytes_[position_] == '}')
        {
            position_++;
            return true;
        }
        for (;;)
        {
            QByteArray token;
            QString key;
            if (!ScanString(token, errorMessage) || !DecodeKey(token, key, errorMessage))
            {
                return false;
            }
            if (keys.contains(key))
            {
                errorMessage = QStringLiteral("metadata preset contains duplicate JSON key: %1").arg(key);
                return false;
            }
            keys.insert(key);
            SkipWhitespace();
            if (position_ >= bytes_.size() || bytes_[position_] != ':')
            {
                errorMessage = QStringLiteral("metadata preset object key is missing ':'");
                return false;
            }
            position_++;
            if (!ScanValue(depth + 1, errorMessage))
            {
                return false;
            }
            SkipWhitespace();
            if (position_ >= bytes_.size())
            {
                errorMessage = QStringLiteral("metadata preset object is truncated");
                return false;
            }
            const char delimiter = bytes_[position_++];
            if (delimiter == '}')
            {
                return true;
            }
            if (delimiter != ',')
            {
                errorMessage = QStringLiteral("metadata preset object has an invalid delimiter");
                return false;
            }
        }
    }

    [[nodiscard]] bool ScanArray(const std::size_t depth, QString& errorMessage)
    {
        position_++;
        SkipWhitespace();
        if (position_ < bytes_.size() && bytes_[position_] == ']')
        {
            position_++;
            return true;
        }
        for (;;)
        {
            if (!ScanValue(depth + 1, errorMessage))
            {
                return false;
            }
            SkipWhitespace();
            if (position_ >= bytes_.size())
            {
                errorMessage = QStringLiteral("metadata preset array is truncated");
                return false;
            }
            const char delimiter = bytes_[position_++];
            if (delimiter == ']')
            {
                return true;
            }
            if (delimiter != ',')
            {
                errorMessage = QStringLiteral("metadata preset array has an invalid delimiter");
                return false;
            }
        }
    }

    [[nodiscard]] bool ScanValue(const std::size_t depth, QString& errorMessage)
    {
        if (!Charge(depth, errorMessage))
        {
            return false;
        }
        SkipWhitespace();
        if (position_ >= bytes_.size())
        {
            errorMessage = QStringLiteral("metadata preset JSON value is truncated");
            return false;
        }
        if (bytes_[position_] == '{')
        {
            return ScanObject(depth, errorMessage);
        }
        if (bytes_[position_] == '[')
        {
            return ScanArray(depth, errorMessage);
        }
        if (bytes_[position_] == '"')
        {
            QByteArray token;
            return ScanString(token, errorMessage);
        }
        const ByteIndex start = position_;
        while (position_ < bytes_.size() && bytes_[position_] != ',' && bytes_[position_] != '}' &&
            bytes_[position_] != ']' && bytes_[position_] != ' ' && bytes_[position_] != '\t' &&
            bytes_[position_] != '\r' && bytes_[position_] != '\n')
        {
            position_++;
        }
        if (position_ == start)
        {
            errorMessage = QStringLiteral("metadata preset contains an empty JSON value");
            return false;
        }
        return true;
    }

    const QByteArray& bytes_;
    ByteIndex position_ = 0;
    std::size_t nodes_ = 0;
};

[[nodiscard]] bool HasExactKeys(const QJsonObject& object, const QSet<QString>& expected, const QString& name,
    QString& errorMessage)
{
    const QStringList keys = object.keys();
    const QSet<QString> actual(keys.begin(), keys.end());
    if (actual == expected)
    {
        return true;
    }
    errorMessage = QStringLiteral("%1 schema key set mismatch").arg(name);
    return false;
}

[[nodiscard]] bool ReadString(const QJsonObject& object, const QString& name, std::string& output,
    QString& errorMessage)
{
    const QJsonValue value = object.value(name);
    if (!value.isString())
    {
        errorMessage = QStringLiteral("metadata preset field %1 must be a string").arg(name);
        return false;
    }
    const QByteArray utf8 = value.toString().toUtf8();
    if (utf8.size() > 1024 || utf8.contains('\0'))
    {
        errorMessage = QStringLiteral("metadata preset field %1 exceeds 1024 UTF-8 bytes or contains U+0000").arg(name);
        return false;
    }
    output.assign(utf8.constData(), static_cast<std::size_t>(utf8.size()));
    return true;
}

[[nodiscard]] bool ReadNullableNumber(const QJsonObject& object, const QString& name, const double maximum,
    const bool allowZero, std::optional<double>& output, QString& errorMessage)
{
    const QJsonValue value = object.value(name);
    if (value.isNull())
    {
        output.reset();
        return true;
    }
    if (!value.isDouble())
    {
        errorMessage = QStringLiteral("metadata preset field %1 must be a finite number or null").arg(name);
        return false;
    }
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < 0 || number > maximum || (!allowZero && number == 0))
    {
        errorMessage = QStringLiteral("metadata preset field %1 is outside its bounded range").arg(name);
        return false;
    }
    output = number;
    return true;
}

[[nodiscard]] bool ReadRect(const QJsonObject& object, const QString& name,
    std::optional<MetadataPhysicalRect>& output, QString& errorMessage)
{
    const QJsonValue value = object.value(name);
    if (value.isNull())
    {
        output.reset();
        return true;
    }
    if (!value.isObject())
    {
        errorMessage = QStringLiteral("metadata preset field %1 must be a physical rectangle or null").arg(name);
        return false;
    }
    const QJsonObject rectangle = value.toObject();
    if (!HasExactKeys(rectangle, {QStringLiteral("left"), QStringLiteral("top"), QStringLiteral("right"),
        QStringLiteral("bottom")}, name, errorMessage))
    {
        return false;
    }
    std::array<std::int32_t, 4> coordinates{};
    const std::array<QString, 4> names{QStringLiteral("left"), QStringLiteral("top"), QStringLiteral("right"),
        QStringLiteral("bottom")};
    for (std::size_t index = 0; index < names.size(); index++)
    {
        const QJsonValue coordinate = rectangle.value(names[index]);
        const double number = coordinate.toDouble((std::numeric_limits<double>::quiet_NaN)());
        if (!coordinate.isDouble() || !std::isfinite(number) || std::floor(number) != number ||
            number < (std::numeric_limits<std::int32_t>::min)() || number > (std::numeric_limits<std::int32_t>::max)())
        {
            errorMessage = QStringLiteral("metadata preset rectangle %1.%2 is not a signed 32-bit integer")
                .arg(name, names[index]);
            return false;
        }
        coordinates[index] = static_cast<std::int32_t>(number);
    }
    const std::int64_t width = static_cast<std::int64_t>(coordinates[2]) - coordinates[0];
    const std::int64_t height = static_cast<std::int64_t>(coordinates[3]) - coordinates[1];
    if (width <= 0 || height <= 0 || width > 32768 || height > 32768)
    {
        errorMessage = QStringLiteral("metadata preset rectangle %1 is empty or exceeds 32768 pixels").arg(name);
        return false;
    }
    output = MetadataPhysicalRect{coordinates[0], coordinates[1], coordinates[2], coordinates[3]};
    return true;
}

[[nodiscard]] bool ParseProvenance(const std::string& value, MetadataProvenance& output) noexcept
{
    if (value == "NotProvided")
    {
        output = MetadataProvenance::NotProvided;
    }
    else if (value == "Manual")
    {
        output = MetadataProvenance::Manual;
    }
    else if (value == "PixelBridgeObserved")
    {
        output = MetadataProvenance::PixelBridgeObserved;
    }
    else if (value == "RemoteUiVisible")
    {
        output = MetadataProvenance::RemoteUiVisible;
    }
    else
    {
        return false;
    }
    return true;
}

[[nodiscard]] bool IsRunId(const std::string_view value) noexcept
{
    if (value.size() != 32)
    {
        return false;
    }
    for (const char character : value)
    {
        if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f')))
        {
            return false;
        }
    }
    return true;
}

} // namespace

bool LoadRemoteVisualMetadataPreset(const QString& path, RemoteRunMetadata& output, QString& errorMessage)
{
    errorMessage.clear();
    QFile file(path);
    if (path.trimmed().isEmpty() || !file.open(QIODevice::ReadOnly))
    {
        errorMessage = QStringLiteral("cannot open RemoteVisual metadata preset read-only");
        return false;
    }
    if (file.size() <= 0 || file.size() > maximumPresetBytes)
    {
        errorMessage = QStringLiteral("RemoteVisual metadata preset must be 1..262144 bytes");
        return false;
    }
    const qint64 expectedBytes = file.size();
    const QByteArray bytes = file.readAll();
    if (bytes.size() != expectedBytes || bytes.isEmpty() || bytes.size() > maximumPresetBytes)
    {
        errorMessage = QStringLiteral("RemoteVisual metadata preset changed while being read");
        return false;
    }
    if (!IsStrictUtf8(bytes))
    {
        errorMessage = QStringLiteral("RemoteVisual metadata preset is not strict UTF-8");
        return false;
    }
    DuplicateKeyScanner duplicateScanner(bytes);
    if (!duplicateScanner.Scan(errorMessage))
    {
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        errorMessage = QStringLiteral("RemoteVisual metadata preset is not strict UTF-8 JSON object: %1")
            .arg(parseError.errorString());
        return false;
    }
    const QJsonObject object = document.object();
    const QSet<QString> expectedKeys{QStringLiteral("schema"), QStringLiteral("runId"), QStringLiteral("channelType"),
        QStringLiteral("remoteProvider"), QStringLiteral("remoteProviderVersion"), QStringLiteral("remoteMode"),
        QStringLiteral("targetFps"), QStringLiteral("observedFps"), QStringLiteral("chromaMode"),
        QStringLiteral("computerBDisplayResolution"), QStringLiteral("computerBRefreshRate"),
        QStringLiteral("computerADisplayResolution"), QStringLiteral("computerARefreshRate"),
        QStringLiteral("remoteResolution"), QStringLiteral("remoteWindowPhysicalRect"),
        QStringLiteral("selectedRoiPhysicalRect"), QStringLiteral("estimatedScaleX"),
        QStringLiteral("estimatedScaleY"), QStringLiteral("letterboxStatus"), QStringLiteral("cropStatus"),
        QStringLiteral("geometryStatus"), QStringLiteral("networkType"), QStringLiteral("observedBandwidthMbps"),
        QStringLiteral("observedLatencyMilliseconds"), QStringLiteral("protectedMonitorIdentity"),
        QStringLiteral("experimentMonitorIdentity"), QStringLiteral("remoteUiProvenance"),
        QStringLiteral("geometryProvenance"), QStringLiteral("networkProvenance"), QStringLiteral("notes")};
    if (!HasExactKeys(object, expectedKeys, QStringLiteral("RemoteVisual metadata"), errorMessage))
    {
        return false;
    }

    RemoteRunMetadata candidate;
    std::string schema;
    std::string channelType;
    std::string chromaMode;
    std::string remoteUiProvenance;
    std::string geometryProvenance;
    std::string networkProvenance;
    if (!ReadString(object, QStringLiteral("schema"), schema, errorMessage) ||
        !ReadString(object, QStringLiteral("runId"), candidate.runId, errorMessage) ||
        !ReadString(object, QStringLiteral("channelType"), channelType, errorMessage) ||
        !ReadString(object, QStringLiteral("remoteProvider"), candidate.remoteProvider, errorMessage) ||
        !ReadString(object, QStringLiteral("remoteProviderVersion"), candidate.providerVersion, errorMessage) ||
        !ReadString(object, QStringLiteral("remoteMode"), candidate.remoteMode, errorMessage) ||
        !ReadString(object, QStringLiteral("chromaMode"), chromaMode, errorMessage) ||
        !ReadString(object, QStringLiteral("computerBDisplayResolution"), candidate.computerBDisplayResolution, errorMessage) ||
        !ReadString(object, QStringLiteral("computerADisplayResolution"), candidate.computerADisplayResolution, errorMessage) ||
        !ReadString(object, QStringLiteral("remoteResolution"), candidate.remoteResolution, errorMessage) ||
        !ReadString(object, QStringLiteral("letterboxStatus"), candidate.letterboxStatus, errorMessage) ||
        !ReadString(object, QStringLiteral("cropStatus"), candidate.cropStatus, errorMessage) ||
        !ReadString(object, QStringLiteral("geometryStatus"), candidate.geometryStatus, errorMessage) ||
        !ReadString(object, QStringLiteral("networkType"), candidate.networkType, errorMessage) ||
        !ReadString(object, QStringLiteral("protectedMonitorIdentity"), candidate.protectedMonitorIdentity, errorMessage) ||
        !ReadString(object, QStringLiteral("experimentMonitorIdentity"), candidate.experimentMonitorIdentity, errorMessage) ||
        !ReadString(object, QStringLiteral("remoteUiProvenance"), remoteUiProvenance, errorMessage) ||
        !ReadString(object, QStringLiteral("geometryProvenance"), geometryProvenance, errorMessage) ||
        !ReadString(object, QStringLiteral("networkProvenance"), networkProvenance, errorMessage) ||
        !ReadString(object, QStringLiteral("notes"), candidate.notes, errorMessage))
    {
        return false;
    }
    const QString provider = QString::fromUtf8(candidate.remoteProvider.data(),
        static_cast<int>(candidate.remoteProvider.size()));
    if (schema != "PixelBridge.RemoteVisualRunMetadata.1" || !IsRunId(candidate.runId) ||
        channelType != "RemoteVisual" || provider.trimmed().isEmpty())
    {
        errorMessage = QStringLiteral(
            "metadata preset must have schema PixelBridge.RemoteVisualRunMetadata.1, a 128-bit lowercase hex RunId, RemoteVisual, and a provider");
        return false;
    }
    candidate.channelType = ChannelType::RemoteVisual;
    if (chromaMode == "Unknown")
    {
        candidate.chromaMode = ChromaMode::Unknown;
    }
    else if (chromaMode == "4:4:4")
    {
        candidate.chromaMode = ChromaMode::Chroma444;
    }
    else if (chromaMode == "4:2:0")
    {
        candidate.chromaMode = ChromaMode::Chroma420;
    }
    else
    {
        errorMessage = QStringLiteral("metadata preset chromaMode is invalid");
        return false;
    }
    if (!ParseProvenance(remoteUiProvenance, candidate.remoteUiProvenance) ||
        !ParseProvenance(geometryProvenance, candidate.geometryProvenance) ||
        !ParseProvenance(networkProvenance, candidate.networkProvenance))
    {
        errorMessage = QStringLiteral("metadata preset contains an invalid provenance enum");
        return false;
    }
    if (!ReadNullableNumber(object, QStringLiteral("targetFps"), 1000, false, candidate.targetFps, errorMessage) ||
        !ReadNullableNumber(object, QStringLiteral("observedFps"), 1000, true, candidate.observedFps, errorMessage) ||
        !ReadNullableNumber(object, QStringLiteral("computerBRefreshRate"), 1000, false,
            candidate.computerBRefreshRate, errorMessage) ||
        !ReadNullableNumber(object, QStringLiteral("computerARefreshRate"), 1000, false,
            candidate.computerARefreshRate, errorMessage) ||
        !ReadNullableNumber(object, QStringLiteral("estimatedScaleX"), 16, false, candidate.estimatedScaleX,
            errorMessage) ||
        !ReadNullableNumber(object, QStringLiteral("estimatedScaleY"), 16, false, candidate.estimatedScaleY,
            errorMessage) ||
        !ReadNullableNumber(object, QStringLiteral("observedBandwidthMbps"), 1000000, true,
            candidate.observedBandwidthMbps, errorMessage) ||
        !ReadNullableNumber(object, QStringLiteral("observedLatencyMilliseconds"), 10000000, true,
            candidate.observedLatencyMilliseconds, errorMessage) ||
        !ReadRect(object, QStringLiteral("remoteWindowPhysicalRect"), candidate.remoteWindowPhysicalRect,
            errorMessage) ||
        !ReadRect(object, QStringLiteral("selectedRoiPhysicalRect"), candidate.selectedRoiPhysicalRect,
            errorMessage))
    {
        return false;
    }
    std::size_t totalStringBytes = 0;
    const std::array<std::string_view, 14> strings{candidate.runId, candidate.remoteProvider, candidate.providerVersion,
        candidate.remoteMode, candidate.computerBDisplayResolution, candidate.computerADisplayResolution,
        candidate.remoteResolution, candidate.letterboxStatus, candidate.cropStatus, candidate.geometryStatus,
        candidate.networkType, candidate.protectedMonitorIdentity, candidate.experimentMonitorIdentity, candidate.notes};
    for (const std::string_view value : strings)
    {
        if (totalStringBytes > 8192 - value.size())
        {
            errorMessage = QStringLiteral("metadata preset exceeds its total UTF-8 string budget");
            return false;
        }
        totalStringBytes += value.size();
    }
    output = std::move(candidate);
    return true;
}

} // namespace pbapp
