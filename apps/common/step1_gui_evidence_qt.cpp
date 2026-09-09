#include "step1_gui_evidence_qt.h"

#include "pbcore/build_info.h"
#include <Windows.h>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <utility>

namespace pbgui
{
namespace
{
constexpr std::uint64_t finalEvidenceReserveBytes = 1024ULL * 1024ULL;

bool IsLocalUnlinkedPath(const QString &path)
{
    if (path.size() < 3 || !path[0].isLetter() || path[1] != QChar(':') || !QDir::isAbsolutePath(path))
    {
        return false;
    }
    auto current = std::filesystem::path(path.toStdWString()).lexically_normal();
    while (!current.empty())
    {
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        {
            return false;
        }
        const auto parent = current.parent_path();
        if (parent == current)
        {
            break;
        }
        current = parent;
    }
    return GetDriveTypeW(std::filesystem::path(path.toStdWString()).root_path().c_str()) == DRIVE_FIXED;
}

std::string Json(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
}
} // namespace

Step1GuiEvidence::Step1GuiEvidence(QString root, QString role)
    : root_(std::move(root)), role_(std::move(role))
{
}

std::unique_ptr<Step1GuiEvidence> Step1GuiEvidence::Create(const QString &root, const QString &role, QString &error)
{
    if ((role != QStringLiteral("Encoder") && role != QStringLiteral("Decoder")) || !IsLocalUnlinkedPath(root) ||
        QFileInfo::exists(root) || !QDir().mkdir(root))
    {
        error = QStringLiteral("M1 evidence root must be a new directory on a fixed local drive, with no reparse ancestors; parent must exist.");
        return {};
    }
    auto result = std::unique_ptr<Step1GuiEvidence>(new Step1GuiEvidence(QDir(root).absolutePath(), role));
    std::array<wchar_t, 32768> modulePath{};
    const DWORD moduleLength = GetModuleFileNameW(nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
    if (moduleLength == 0 || moduleLength >= modulePath.size())
    {
        error = QStringLiteral("Unable to establish executing M1 binary path.");
        return {};
    }
    QFile executable(QString::fromWCharArray(modulePath.data(), static_cast<int>(moduleLength)));
    QCryptographicHash executableHasher(QCryptographicHash::Sha256);
    if (!executable.open(QIODevice::ReadOnly) || !executableHasher.addData(&executable))
    {
        error = QStringLiteral("Unable to establish executing M1 binary hash.");
        return {};
    }
    QFile entry(QDir(root).filePath(QStringLiteral("entry.json")));
    const std::string identity = pbapp::GetMeasurementBuildIdentityJson();
    const auto data = QJsonDocument(QJsonObject{{"schema", "PixelBridge.Step1.GuiEntry.1"}, {"role", role}, {"build", QJsonDocument::fromJson(QByteArray::fromStdString(identity)).object()}, {"executable", QJsonObject{{"path", executable.fileName()}, {"size", executable.size()}, {"sha256", QString::fromLatin1(executableHasher.result().toHex())}}}, {"manualStartRequired", true}, {"automaticInput", false}, {"payloadSideChannel", false}}).toJson();
    if (!entry.open(QIODevice::WriteOnly | QIODevice::NewOnly) || entry.write(data) != data.size() || !entry.flush())
    {
        error = QStringLiteral("Unable to seal M1 GUI entry; partial directory retained.");
        return {};
    }
    return result;
}

QString Step1GuiEvidence::SettingsPath() const
{
    return QDir(root_).filePath(QStringLiteral("settings.ini"));
}
QString Step1GuiEvidence::RunDirectory() const
{
    return runDirectory_;
}
QString Step1GuiEvidence::RunId() const
{
    return runId_;
}
QString Step1GuiEvidence::StatusText() const
{
    if (!recorder_)
    {
        return QStringLiteral("M1 experiment: manual start; isolated evidence/settings");
    }
    const auto snapshot = recorder_->GetSnapshot();
    return QStringLiteral("M1 evidence: %1 | %2").arg(QString::fromLatin1(pbapp::GetMeasurementFailureName(snapshot.failure)), runDirectory_);
}

std::shared_ptr<pbapp::RunMeasurementRecorder> Step1GuiEvidence::BeginRun(QString &error)
{
    if (recorder_ && !sealed_)
    {
        error = QStringLiteral("Previous M1 evidence has not reached terminal; wait for clean shutdown.");
        return {};
    }
    // A human-readable directory name is not the runtime's 128-bit hex RunId.
    const auto runUuid = QUuid::createUuid();
    const auto name = QStringLiteral("run-") + runUuid.toString(QUuid::WithoutBraces);
    runDirectory_ = QDir(root_).filePath(name);
    if (!QDir().mkdir(runDirectory_) || !QDir().mkdir(QDir(runDirectory_).filePath(QStringLiteral("sessions"))) ||
        !QDir().mkdir(QDir(runDirectory_).filePath(QStringLiteral("output"))))
    {
        error = QStringLiteral("Unable to create fresh M1 run directories; retained partial evidence.");
        return {};
    }
    events_.setFileName(QDir(runDirectory_).filePath(QStringLiteral("events.jsonl")));
    if (!events_.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    {
        error = QStringLiteral("Unable to create M1 journal.");
        return {};
    }
    recorder_ = std::make_shared<pbapp::RunMeasurementRecorder>();
    runId_ = runUuid.toString(QUuid::Id128);
    bytesWritten_ = 0;
    lastSampleNanoseconds_ = 0;
    sealed_ = false;
    ioFailed_ = false;
    return recorder_;
}

bool Step1GuiEvidence::WriteNew(const QString &name, const std::string &content)
{
    if (content.size() > 65536 || content.size() > pbapp::step1MaximumEvidenceBytes - bytesWritten_)
    {
        recorder_->Fail(pbapp::MeasurementFailure::EvidenceByteLimit);
        return false;
    }
    QFile output(QDir(runDirectory_).filePath(name));
    const bool success = output.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
                         output.write(content.data(), static_cast<qint64>(content.size())) == static_cast<qint64>(content.size()) && output.flush();
    bytesWritten_ += content.size();
    if (!success)
    {
        ioFailed_ = true;
        recorder_->Fail(pbapp::MeasurementFailure::IoFailure);
    }
    return success;
}

void Step1GuiEvidence::StartRejected(const QString &reason)
{
    if (!recorder_ || sealed_)
    {
        return;
    }
    recorder_->Fail(pbapp::MeasurementFailure::RuntimeFailure);
    static_cast<void>(WriteNew(QStringLiteral("start-rejected.json"), Json({{"reason", reason.left(2048)}})));
    events_.close();
    Seal();
}

void Step1GuiEvidence::Drain()
{
    pbapp::SubmittedFrameIdentity identity;
    for (std::size_t count = 0; count < pbapp::step1SubmittedQueueCapacity && recorder_->TakeSubmitted(identity); count++)
    {
        if (ioFailed_)
        {
            continue;
        }
        const auto record = pbapp::BuildSubmittedFrameJson(identity) + '\n';
        if (record.size() > 65536 || bytesWritten_ + record.size() > pbapp::step1MaximumEvidenceBytes - finalEvidenceReserveBytes)
        {
            recorder_->Fail(pbapp::MeasurementFailure::EvidenceByteLimit);
            ioFailed_ = true;
            continue;
        }
        if (events_.write(record.data(), static_cast<qint64>(record.size())) != static_cast<qint64>(record.size()))
        {
            recorder_->Fail(pbapp::MeasurementFailure::IoFailure);
            ioFailed_ = true;
        }
        bytesWritten_ += record.size();
    }
}

bool Step1GuiEvidence::ShouldSample(const pbapp::RunMeasurementSnapshot &snapshot)
{
    if (sealed_ || snapshot.runGeneration == 0)
    {
        return false;
    }
    if (snapshot.terminalSucceeded.has_value())
    {
        return true;
    }
    const auto now = pbapp::MeasurementNowNanoseconds();
    recorder_->CheckDuration(now);
    if (now - lastSampleNanoseconds_ < 1000000000ULL)
    {
        return false;
    }
    lastSampleNanoseconds_ = now;
    return !ioFailed_;
}

pbapp::RunReportContext Step1GuiEvidence::Context() const
{
    const auto identity = QJsonDocument::fromJson(QByteArray::fromStdString(pbapp::GetMeasurementBuildIdentityJson())).object();
    return {(QStringLiteral("PixelBridge") + role_).toStdString(), pbcore::GetBuildInfo().version,
            identity["baseCommit"].toString().toStdString(), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString()};
}

void Step1GuiEvidence::Observe(const pbapp::EncoderSnapshot &snapshot)
{
    if (!recorder_ || sealed_ || !snapshot.measurement)
    {
        return;
    }
    Drain();
    if (!ShouldSample(*snapshot.measurement))
    {
        return;
    }
    auto current = snapshot;
    current.measurement = recorder_->GetSnapshot();
    RecordReport(pbapp::BuildEncoderRunReportJson(Context(), current), *current.measurement);
}

void Step1GuiEvidence::Observe(const pbapp::DecoderSnapshot &snapshot)
{
    if (!recorder_ || sealed_ || !snapshot.measurement)
    {
        return;
    }
    Drain();
    if (!ShouldSample(*snapshot.measurement))
    {
        return;
    }
    auto current = snapshot;
    current.measurement = recorder_->GetSnapshot();
    RecordReport(pbapp::BuildDecoderRunReportJson(Context(), current), *current.measurement);
}

void Step1GuiEvidence::RecordReport(const std::string &report, const pbapp::RunMeasurementSnapshot &snapshot)
{
    if (snapshot.terminalSucceeded.has_value())
    {
        if (!events_.flush())
        {
            recorder_->Fail(pbapp::MeasurementFailure::IoFailure);
            ioFailed_ = true;
        }
        events_.close();
        static_cast<void>(WriteNew(QStringLiteral("final.json"), report));
        if (role_ == QStringLiteral("Encoder"))
        {
            static_cast<void>(WriteNew(QStringLiteral("source-ledger.json"), pbapp::BuildSourceLedgerJson(snapshot)));
        }
        Seal();
        return;
    }
    const auto record = "{\"kind\":\"sample\",\"report\":" + report + "}\n";
    if (record.size() > 65536 || bytesWritten_ + record.size() > pbapp::step1MaximumEvidenceBytes - finalEvidenceReserveBytes)
    {
        recorder_->Fail(pbapp::MeasurementFailure::EvidenceByteLimit);
        ioFailed_ = true;
        return;
    }
    if (events_.write(record.data(), static_cast<qint64>(record.size())) != static_cast<qint64>(record.size()))
    {
        recorder_->Fail(pbapp::MeasurementFailure::IoFailure);
        ioFailed_ = true;
    }
    bytesWritten_ += record.size();
}

void Step1GuiEvidence::Seal()
{
    QJsonArray files;
    for (const auto &name : {QStringLiteral("events.jsonl"), QStringLiteral("final.json"),
                             QStringLiteral("source-ledger.json"), QStringLiteral("start-rejected.json")})
    {
        QFile input(QDir(runDirectory_).filePath(name));
        if (!input.exists())
        {
            continue;
        }
        if (input.size() < 0 || static_cast<std::uint64_t>(input.size()) > pbapp::step1MaximumEvidenceBytes)
        {
            recorder_->Fail(pbapp::MeasurementFailure::EvidenceByteLimit);
            ioFailed_ = true;
            continue;
        }
        QCryptographicHash hasher(QCryptographicHash::Sha256);
        if (!input.open(QIODevice::ReadOnly) || !hasher.addData(&input))
        {
            recorder_->Fail(pbapp::MeasurementFailure::IoFailure);
            ioFailed_ = true;
            continue;
        }
        files.append(QJsonObject{{"path", name}, {"bytes", input.size()}, {"sha256", QString::fromLatin1(hasher.result().toHex())}});
    }
    QFile entry(QDir(root_).filePath(QStringLiteral("entry.json")));
    QCryptographicHash entryHasher(QCryptographicHash::Sha256);
    if (!entry.open(QIODevice::ReadOnly) || entry.size() > 65536 || !entryHasher.addData(&entry))
    {
        recorder_->Fail(pbapp::MeasurementFailure::IoFailure);
        ioFailed_ = true;
    }
    const auto snapshot = recorder_->GetSnapshot();
    const auto seal = Json({{"schema", "PixelBridge.Step1.RunEvidenceSeal.1"}, {"role", role_}, {"files", files}, {"entrySha256", QString::fromLatin1(entryHasher.result().toHex())}, {"complete", snapshot.terminalSucceeded.has_value() && !ioFailed_ && snapshot.failure == pbapp::MeasurementFailure::None}, {"failure", QString::fromLatin1(pbapp::GetMeasurementFailureName(snapshot.failure))}, {"payloadSideChannel", false}, {"authenticityClaim", false}});
    static_cast<void>(WriteNew(QStringLiteral("evidence-seal.json"), seal));
    sealed_ = true;
}
} // namespace pbgui
