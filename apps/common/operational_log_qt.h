#pragma once

#include "decoder_activity.h"
#include "evidence_journal.h"
#include "run_report.h"

#include <QElapsedTimer>
#include <QString>

namespace pbgui
{

// Application adapter, not part of Qt-free PBApplication. Polling-side only:
// no payload, pixels, feedback, unbounded queue, or per-frame durable flush.
class OperationalLog final
{
public:
    explicit OperationalLog(const QString& role, const QString& root = {});
    void Begin() noexcept;
    void Observe(const pbapp::EncoderSnapshot& snapshot, bool final = false) noexcept;
    void Observe(const pbapp::DecoderSnapshot& snapshot, bool final = false) noexcept;
    void StartRejected(const QString& message) noexcept;
    [[nodiscard]] QString StatusText() const;
    [[nodiscard]] QString Directory() const;
    [[nodiscard]] pbapp::DecoderActivity GetDecoderActivity() const;

private:
    void Write(const std::string& record, const std::string& report, bool terminal);
    [[nodiscard]] bool Due(int state, bool terminal);
    [[nodiscard]] pbapp::RunReportContext Context() const;
    void Fail(const QString& message) noexcept;

    QString role_;
    QString root_;
    QString stem_;
    QString error_;
    QString retentionWarning_;
    std::unique_ptr<pbapp::RunEvidenceJournal> journal_;
    pbapp::RunJournalSnapshot journalStatus_;
    QElapsedTimer timer_;
    qint64 lastSample_ = -2000;
    int lastState_ = -1;
    bool finished_ = false;
    pbapp::DecoderActivityTracker activityTracker_;
    pbapp::DecoderActivity activity_;
};

[[nodiscard]] QString FormatDecoderActivity(const pbapp::DecoderActivity& activity);

} // namespace pbgui
