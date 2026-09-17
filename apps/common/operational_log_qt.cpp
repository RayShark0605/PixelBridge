#include "operational_log_qt.h"
#include "pbcore/build_info.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUuid>

#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <filesystem>
#include <vector>

namespace pbgui
{
namespace
{

[[nodiscard]] bool IsOrdinaryFile(const QString& path)
{
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()));
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) == 0;
}

// Delete only our exact flat log filename pairs, never directories, links,
// explicit evidence journals, user files or active writers (FILE_SHARE_READ).
[[nodiscard]] QString PruneLogs(const QString& root)
{
    const QRegularExpression pattern(QStringLiteral("^run-[0-9a-f]{32}\\.events\\.jsonl$"));
    std::vector<QFileInfo> files;
    QDirIterator iterator(root, QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden);
    std::size_t visited = 0;
    while (iterator.hasNext())
    {
        if (visited++ >= 2048)
        {
            return QStringLiteral("日志目录条目过多；请手动整理旧日志。");
        }
        const QString path = iterator.next();
        if (pattern.match(QFileInfo(path).fileName()).hasMatch() && IsOrdinaryFile(path))
        {
            files.push_back(QFileInfo(path));
        }
    }
    std::sort(files.begin(), files.end(), [](const QFileInfo& left, const QFileInfo& right)
    {
        return left.lastModified() > right.lastModified();
    });
    bool skipped = false;
    for (std::size_t index = 7; index < files.size(); index++)
    {
        const QString path = files[index].absoluteFilePath();
        const QString summary = path.left(path.size() - QStringLiteral(".events.jsonl").size()) + QStringLiteral(".summary.json");
        if (IsOrdinaryFile(path) && QFile::remove(path))
        {
            if (IsOrdinaryFile(summary) && !QFile::remove(summary))
            {
                skipped = true;
            }
        }
        else
        {
            skipped = true;
        }
    }
    return skipped ? QStringLiteral("部分旧日志正在使用或不可删除，已保留。") : QString();
}

[[nodiscard]] std::string Decorate(const std::string& record, const qint64 elapsed, const bool terminal,
    const pbapp::RunReportContext& context, const pbapp::DecoderActivity* activity)
{
    // Only our bounded serializer's JSON is parsed here; no visual input.
    auto object = QJsonDocument::fromJson(QByteArray::fromStdString(record)).object();
    object.insert(QStringLiteral("logKind"), QStringLiteral("OperationalDiagnostic"));
    object.insert(QStringLiteral("elapsedMs"), elapsed);
    object.insert(QStringLiteral("terminal"), terminal);
    object.insert(QStringLiteral("version"), QString::fromStdString(context.applicationVersion));
    object.insert(QStringLiteral("gitCommit"), QString::fromStdString(context.gitCommit));
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)))
    {
        object.insert(QStringLiteral("processPrivateBytes"), static_cast<qint64>(memory.PrivateUsage));
        object.insert(QStringLiteral("processWorkingSetBytes"), static_cast<qint64>(memory.WorkingSetSize));
    }
    if (activity)
    {
        object.insert(QStringLiteral("activity"), QString::fromStdString(activity->code));
        object.insert(QStringLiteral("normalWait"), activity->normalWait);
        object.insert(QStringLiteral("noSizeGrowthMs"), static_cast<qint64>(activity->noSizeGrowthMilliseconds));
        object.insert(QStringLiteral("noNewSymbolMs"), static_cast<qint64>(activity->noNewSymbolMilliseconds));
    }
    return QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
}

} // namespace

OperationalLog::OperationalLog(const QString& role, const QString& root) : role_(role), root_(root)
{
    if (root_.isEmpty())
    {
        // GenericDataLocation is LocalAppData on Windows, independent of CLI's
        // lack of a QCoreApplication and the GUI's applicationName.
        const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
        if (!base.isEmpty())
        {
            root_ = QDir(base).filePath(QStringLiteral("PixelBridge/Logs/") + role_);
        }
    }
}

void OperationalLog::Begin() noexcept
{
    try
    {
        if (journal_)
        {
            static_cast<void>(journal_->Finish());
        }
        journal_.reset();
        error_.clear();
        finished_ = false;
        lastState_ = -1;
        lastSample_ = -2000;
        activityTracker_ = {};
        activity_ = {};
        timer_.start();
        if ((role_ != QStringLiteral("Encoder") && role_ != QStringLiteral("Decoder")) || root_.isEmpty() || !QDir().mkpath(root_))
        {
            Fail(QStringLiteral("无法创建本机日志目录。"));
            return;
        }
        retentionWarning_ = PruneLogs(root_);
        stem_ = QDir(root_).filePath(QStringLiteral("run-") + QUuid::createUuid().toString(QUuid::Id128));
        pbapp::RunJournalLimits limits;
        limits.samplingIntervalMilliseconds = 1; // Due() throttles before serialization.
        limits.maximumDurationMilliseconds = 48ULL * 60 * 60 * 1000;
        limits.maximumBytes = 64ULL * 1024 * 1024;
        journalStatus_ = pbapp::RunEvidenceJournal::Create(std::filesystem::path((stem_ + QStringLiteral(".events.jsonl")).toStdWString()), limits, journal_);
        if (!journalStatus_.valid)
        {
            Fail(QString::fromStdString(journalStatus_.invalidReason));
        }
    }
    catch (...)
    {
        Fail(QStringLiteral("日志初始化异常；传输仍可独立进行。"));
    }
}

pbapp::RunReportContext OperationalLog::Context() const
{
    return {("PixelBridge" + role_).toStdString(), pbcore::GetBuildInfo().version, PB_GIT_COMMIT,
        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString()};
}

bool OperationalLog::Due(const int state, const bool terminal)
{
    if (!timer_.isValid() || finished_ || !journal_ || (!terminal && (!error_.isEmpty() || journalStatus_.truncated)))
    {
        return false;
    }
    const qint64 now = timer_.elapsed();
    if (!terminal && state == lastState_ && now - lastSample_ < 2000)
    {
        return false;
    }
    lastState_ = state;
    lastSample_ = now;
    return true;
}

void OperationalLog::Write(const std::string& record, const std::string& report, const bool terminal)
{
    const auto elapsed = static_cast<std::uint64_t>(timer_.elapsed());
    // Due() owns cadence. State transitions at the same millisecond must not
    // be lost to the evidence writer's sample interval. This append bypasses
    // cadence only; it does not finish the file or bypass byte/time bounds.
    journalStatus_ = journal_->AppendTerminal(elapsed, record);
    if (terminal)
    {
        journalStatus_ = journal_->Finish();
        finished_ = true;
        const std::string logMetadata = ",\"operationalLog\":{\"valid\":" + std::string(journalStatus_.valid ? "true" : "false") +
            ",\"truncated\":" + (journalStatus_.truncated ? "true" : "false") + ",\"samples\":" + std::to_string(journalStatus_.samples) + "}";
        std::string annotatedReport = report;
        const auto end = annotatedReport.find_last_of('}');
        if (end != std::string::npos)
        {
            annotatedReport.insert(end, logMetadata);
        }
        QFile output(stem_ + QStringLiteral(".summary.json"));
        if (annotatedReport.size() > 1024 * 1024 || !output.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
            output.write(annotatedReport.data(), static_cast<qint64>(annotatedReport.size())) != static_cast<qint64>(annotatedReport.size()) || !output.flush())
        {
            Fail(QStringLiteral("最终诊断报告写入失败。"));
        }
    }
    if (!journalStatus_.valid)
    {
        Fail(QString::fromStdString(journalStatus_.invalidReason));
    }
}

void OperationalLog::Observe(const pbapp::EncoderSnapshot& snapshot, const bool final) noexcept
{
    try
    {
        const bool terminal = final && (snapshot.state == pbapp::EncoderState::Stopped || snapshot.state == pbapp::EncoderState::Failed);
        if (Due(static_cast<int>(snapshot.state), terminal))
        {
            const auto context = Context();
            Write(Decorate(pbapp::BuildEncoderJournalRecord(static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch()), snapshot),
                timer_.elapsed(), terminal, context, nullptr), terminal ? pbapp::BuildEncoderRunReportJson(context, snapshot) : std::string(), terminal);
        }
    }
    catch (...)
    {
        Fail(QStringLiteral("Encoder 日志记录异常。"));
    }
}

void OperationalLog::Observe(const pbapp::DecoderSnapshot& snapshot, const bool final) noexcept
{
    try
    {
        activity_ = activityTracker_.Observe(snapshot, timer_.isValid() ? static_cast<std::uint64_t>(timer_.elapsed()) : 0);
        const bool terminal = final && (snapshot.state == pbapp::DecoderState::Stopped || snapshot.state == pbapp::DecoderState::Failed || snapshot.state == pbapp::DecoderState::Completed);
        if (Due(static_cast<int>(snapshot.state), terminal))
        {
            const auto context = Context();
            Write(Decorate(pbapp::BuildDecoderJournalRecord(static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch()), snapshot),
                timer_.elapsed(), terminal, context, &activity_), terminal ? pbapp::BuildDecoderRunReportJson(context, snapshot) : std::string(), terminal);
        }
    }
    catch (...)
    {
        Fail(QStringLiteral("Decoder 日志记录异常。"));
    }
}

void OperationalLog::StartRejected(const QString& message) noexcept
{
    try
    {
        if (journal_ && !finished_)
        {
            const auto context = Context();
            const auto event = QJsonDocument(QJsonObject{{"event", "StartRejected"}, {"message", message}, {"role", role_},
                {"unixMs", QDateTime::currentMSecsSinceEpoch()}}).toJson(QJsonDocument::Compact).toStdString();
            const auto record = Decorate(event, timer_.elapsed(), true, context, nullptr);
            Write(record, record, true);
        }
    }
    catch (...)
    {
        Fail(QStringLiteral("无法记录启动失败原因。"));
    }
}

void OperationalLog::Fail(const QString& message) noexcept
{
    error_ = message;
}

QString OperationalLog::Directory() const
{
    return root_;
}

pbapp::DecoderActivity OperationalLog::GetDecoderActivity() const
{
    return activity_;
}

QString OperationalLog::StatusText() const
{
    if (!error_.isEmpty())
    {
        return QStringLiteral("日志异常（不等于传输失败）：%1\n%2").arg(error_, root_);
    }
    const QString warning = journalStatus_.truncated ? QStringLiteral("日志达到 64 MiB / 48 小时上限，时间序列已截断；结束时仍保存最终报告。\n") : QString();
    return warning + QStringLiteral("自动日志：%1\n%2%3").arg(stem_.isEmpty() ? root_ : stem_ + QStringLiteral(".events.jsonl"),
        finished_ ? QStringLiteral("已保存本次最终报告。") : QStringLiteral("每 2 秒采样，状态切换追加；不记录文件内容或屏幕像素。"), retentionWarning_);
}

QString FormatDecoderActivity(const pbapp::DecoderActivity& activity)
{
    const auto& code = activity.code;
    QString text;
    if (code == "Idle") text = QStringLiteral("建议先开始接收，再启动 Encoder 发送。");
    else if (code == "VerifySegment") text = QStringLiteral("正在校验分段、解压并验证原始摘要。");
    else if (code == "WriteSegmentAndCheckpoint") text = QStringLiteral("正在写入已验证分段并保存断点。");
    else if (code == "CheckpointResume") text = QStringLiteral("正在保存接收断点，必要时整理恢复日志。");
    else if (code == "VerifyAndPublishFile") text = QStringLiteral("正在校验整文件、保存最终文件并重开复验。");
    else if (code == "Finalizing") text = QStringLiteral("正在完成最终收尾。");
    else if (code == "CollectingRepairSymbols") text = QStringLiteral("仍在收到新的纠删修复块，正在补齐未完成分段；接收估算暂时不变。");
    else if (code == "WaitingForCarousel") text = QStringLiteral("目前收到重复或已完成分段，正在等待轮播中的缺失修复块。");
    else if (code == "ResourceBackpressure") text = QStringLiteral("解码资源正在占用，部分新分段暂缓接纳。继续等待活动分段完成；若频繁出现，下次可按可用内存调整预算，不要盲目设满物理内存。");
    else if (code == "CaptureInterrupted") text = QStringLiteral("暂未获得新的捕获帧；这不是正常校验等待。请检查远控连接及传输画面是否仍可见，已验证进度保留。");
    else if (code == "NoFreshVisualData") text = QStringLiteral("捕获仍在等待新的有效码面；请检查 Encoder 是否发送、画面是否被遮挡或冻结，不必为此暂停接收。");
    else if (code == "NoUsefulProgress") text = QStringLiteral("至少 30 秒没有新增有效修复块或验证分段，暂不能判定为正常等待。请保持接收并检查码面遮挡、连接与发送模式；日志可用于定位。");
    else if (code == "WaitingForPicture") text = QStringLiteral("等待有效码面。请确认双端模式一致，并让远控传输画面完整可见。");
    else if (code == "ReceivingDescriptors") text = QStringLiteral("正在等待并识别文件、分段描述信息；请保持发送画面可见。");
    else if (code == "WaitingForUsefulData") text = QStringLiteral("正在等待新的有效数据，尚无足够证据确定等待原因；请保持接收，长期不变时查看日志。");
    else if (code == "Receiving") text = QStringLiteral("正在收集有效数据并恢复分段。大文件按分段完成验证，显示大小不一定每秒增加。");
    else if (code == "Completed") text = QStringLiteral("整文件摘要、安全保存和最终重开复验已通过，可以停止发送端。");
    else if (code == "CompletionUnverified") text = QStringLiteral("尚未满足完整文件验收条件，请查看诊断报告，不要使用未验证文件。");
    else if (code == "Failed") text = QStringLiteral("接收发生错误，请查看下方错误和日志；不能将其视为正常等待。");
    else if (code == "Stopped") text = QStringLiteral("接收已停止，可使用相同会话及目录继续断点恢复。");
    else if (code == "Stopping") text = QStringLiteral("正在安全停止并保存可恢复状态，请等待退出。");
    else text = QStringLiteral("接收策略需要确认，请检查当前状态与日志。");
    if (activity.normalWait)
    {
        text += QStringLiteral("\n已接收大小暂时不涨属于正常现象，请保持接收，不要暂停。");
    }
    if (activity.noSizeGrowthMilliseconds >= 5000 && code != "Idle" && code != "Completed" && code != "Stopped" && code != "Failed")
    {
        text += QStringLiteral("（大小已 %1 秒未增长）").arg(activity.noSizeGrowthMilliseconds / 1000);
    }
    return text;
}

} // namespace pbgui
