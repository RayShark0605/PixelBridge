#pragma once

#include "local_desktop_runtime.h"
#include <QGroupBox>
#include <QString>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSettings;

namespace pbgui
{
enum class FeedbackRole
{
    EncoderReader,
    DecoderWriter
};

// UI only. Validation is lexical; no preference load, Apply or status poll
// probes a redirector path. Blocking runtime I/O belongs to PBFeedbackIo.
class FeedbackOptions final : public QGroupBox
{
public:
    explicit FeedbackOptions(FeedbackRole role, QWidget* parent = nullptr);
    void Load(QSettings& settings);
    void Save(QSettings& settings) const;
    void SetEditable(bool editable);
    void SetTelemetry(const pbapp::FeedbackTelemetry& telemetry);
    [[nodiscard]] QString Apply(pbapp::EncoderConfig& config) const;
    [[nodiscard]] QString Apply(pbapp::DecoderConfig& config) const;
private:
    [[nodiscard]] QString Key(const char* suffix) const;
    [[nodiscard]] QString Apply(std::optional<pbapp::FeedbackConfig>& config) const;
    void UpdateEnabledState();
    void Browse();
    const FeedbackRole role_;
    QCheckBox* enabled_ = nullptr;
    QComboBox* kind_ = nullptr;
    QLineEdit* path_ = nullptr;
    QWidget* inputs_ = nullptr;
    QPushButton* browse_ = nullptr;
    QLabel* hint_ = nullptr;
    QLabel* status_ = nullptr;
    QString warning_;
    bool editable_ = true;
};
}
