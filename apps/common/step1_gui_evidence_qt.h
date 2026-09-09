#pragma once

#include "local_desktop_runtime.h"
#include "run_report.h"

#include <QFile>
#include <QString>
#include <memory>

namespace pbgui
{
// Explicit experiment entry only. The GUI owner performs disk IO, never a
// payload/capture thread. Every launch/run uses a new directory and INI file.
class Step1GuiEvidence
{
  public:
    [[nodiscard]] static std::unique_ptr<Step1GuiEvidence> Create(const QString &root, const QString &role, QString &error);
    [[nodiscard]] QString SettingsPath() const;
    [[nodiscard]] QString RunDirectory() const;
    [[nodiscard]] QString RunId() const;
    [[nodiscard]] QString StatusText() const;
    [[nodiscard]] std::shared_ptr<pbapp::RunMeasurementRecorder> BeginRun(QString &error);
    void StartRejected(const QString &reason);
    void Observe(const pbapp::EncoderSnapshot &snapshot);
    void Observe(const pbapp::DecoderSnapshot &snapshot);

  private:
    Step1GuiEvidence(QString root, QString role);
    [[nodiscard]] bool WriteNew(const QString &name, const std::string &content);
    void Drain();
    [[nodiscard]] bool ShouldSample(const pbapp::RunMeasurementSnapshot &snapshot);
    void RecordReport(const std::string &report, const pbapp::RunMeasurementSnapshot &snapshot);
    [[nodiscard]] pbapp::RunReportContext Context() const;
    void Seal();
    QString root_;
    QString role_;
    QString runDirectory_;
    QString runId_;
    QFile events_;
    std::shared_ptr<pbapp::RunMeasurementRecorder> recorder_;
    std::uint64_t bytesWritten_ = 0;
    std::uint64_t lastSampleNanoseconds_ = 0;
    bool sealed_ = false;
    bool ioFailed_ = false;
};
} // namespace pbgui
