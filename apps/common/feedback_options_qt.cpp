#include "feedback_options_qt.h"
#include "feedback_file_store.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

namespace pbgui
{
FeedbackOptions::FeedbackOptions(const FeedbackRole role, QWidget* const parent)
    : QGroupBox(QStringLiteral("可选辅助反馈"), parent), role_(role)
{
    setObjectName(QStringLiteral("feedbackOptions"));
    auto* const layout = new QVBoxLayout(this);
    layout->setSpacing(4);
    enabled_ = new QCheckBox(role == FeedbackRole::EncoderReader ? QStringLiteral("启用辅助反馈（只读）") :
        QStringLiteral("启用辅助反馈（发布状态）"), this);
    enabled_->setObjectName(QStringLiteral("feedbackEnabled"));
    layout->addWidget(enabled_);
    inputs_ = new QWidget(this);
    auto* const row = new QHBoxLayout(inputs_);
    row->setContentsMargins(0, 0, 0, 0);
    kind_ = new QComboBox(this);
    kind_->setObjectName(QStringLiteral("feedbackLocationKind"));
    kind_->addItem(QStringLiteral("目录"));
    kind_->addItem(QStringLiteral("专用文件"));
    path_ = new QLineEdit(this);
    path_->setObjectName(QStringLiteral("feedbackPath"));
    path_->setMaxLength(32700);
    path_->setMinimumWidth(80);
    path_->setPlaceholderText(role == FeedbackRole::EncoderReader ? QStringLiteral("可粘贴 \\\\Client\\C$\\… 映射路径") : QStringLiteral("选择可写目录或新的专用反馈文件"));
    browse_ = new QPushButton(QStringLiteral("浏览…"), this);
    browse_->setObjectName(QStringLiteral("feedbackBrowse"));
    row->addWidget(kind_);
    row->addWidget(path_, 1);
    row->addWidget(browse_);
    layout->addWidget(inputs_);
    hint_ = new QLabel(role == FeedbackRole::EncoderReader ?
        QStringLiteral("选择一个 Decoder 所选目录/文件的本机只读映射。优先服务未完成段，接收方验证完成后自动停止。目录模式使用 PixelBridgeFeedback.pbfb；不写远端测试文件。失效时保持常规视觉传输。") :
        QStringLiteral("此处只写状态元数据，不写载荷。目录模式使用 PixelBridgeFeedback.pbfb；不会覆盖无关文件。完成后保留恢复日志并监听：文件可读取查看，先停止监听再编辑、移动或删除。"), this);
    hint_->setObjectName(QStringLiteral("feedbackHint"));
    hint_->setWordWrap(true);
    hint_->setTextFormat(Qt::PlainText);
    layout->addWidget(hint_);
    status_ = new QLabel(QStringLiteral("辅助反馈已关闭；常规视觉行为不变"), this);
    status_->setObjectName(QStringLiteral("feedbackStatus"));
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    layout->addWidget(status_);
    connect(enabled_, &QCheckBox::toggled, this, [this]() { warning_.clear(); UpdateEnabledState(); });
    connect(browse_, &QPushButton::clicked, this, [this]() { Browse(); });
    UpdateEnabledState();
}

QString FeedbackOptions::Key(const char* const suffix) const
{
    return (role_ == FeedbackRole::EncoderReader ? QStringLiteral("feedback/encoder/") : QStringLiteral("feedback/decoder/")) + QString::fromLatin1(suffix);
}

void FeedbackOptions::Load(QSettings& settings)
{
    const auto enabled = settings.value(Key("enabled"), false).toString().toLower();
    bool kindOk = false;
    const auto kind = settings.value(Key("locationKind"), 0).toString().toInt(&kindOk);
    const auto path = settings.value(Key("path")).toString();
    const bool requested = enabled == QStringLiteral("true") || enabled == QStringLiteral("1");
    bool valid = kindOk && (kind == 0 || kind == 1) && path.size() <= 32700 &&
        (requested || enabled == QStringLiteral("false") || enabled == QStringLiteral("0"));
    if (valid && requested)
    {
        std::filesystem::path resolved;
        valid = static_cast<bool>(pbapp::ResolveFeedbackFilePath({std::filesystem::path(path.toStdWString()),
            kind == 0 ? pbapp::FeedbackLocationKind::Directory : pbapp::FeedbackLocationKind::File}, resolved));
    }
    kind_->setCurrentIndex(valid ? kind : 0);
    path_->setText(path.left(32700));
    enabled_->setChecked(valid && requested);
    warning_ = valid ? QString() : QStringLiteral("保存的反馈设置无效，已关闭；请核对后重新启用。");
    UpdateEnabledState();
}

void FeedbackOptions::Save(QSettings& settings) const
{
    settings.setValue(Key("enabled"), enabled_->isChecked());
    settings.setValue(Key("locationKind"), kind_->currentIndex());
    settings.setValue(Key("path"), path_->text());
}

void FeedbackOptions::SetEditable(const bool editable)
{
    editable_ = editable;
    UpdateEnabledState();
}

void FeedbackOptions::UpdateEnabledState()
{
    enabled_->setEnabled(editable_);
    const bool editable = editable_ && enabled_->isChecked();
    inputs_->setVisible(enabled_->isChecked());
    hint_->setVisible(enabled_->isChecked());
    kind_->setEnabled(editable);
    path_->setEnabled(editable);
    browse_->setEnabled(editable);
    status_->setText(!warning_.isEmpty() ? warning_ : enabled_->isChecked() ?
        QStringLiteral("已启用；开始传输后连接反馈，路径不可用不会中断视觉通道") : QStringLiteral("辅助反馈已关闭；常规视觉行为不变"));
}

QString FeedbackOptions::Apply(std::optional<pbapp::FeedbackConfig>& config) const
{
    config.reset();
    if (!enabled_->isChecked())
    {
        return {};
    }
    if (path_->text().trimmed().isEmpty() || (kind_->currentIndex() != 0 && kind_->currentIndex() != 1))
    {
        return QStringLiteral("启用辅助反馈后，请选择目录或专用文件并填写绝对路径。");
    }
    const pbapp::FeedbackConfig selected{std::filesystem::path(path_->text().toStdWString()),
        kind_->currentIndex() == 0 ? pbapp::FeedbackLocationKind::Directory : pbapp::FeedbackLocationKind::File};
    std::filesystem::path resolved;
    if (!pbapp::ResolveFeedbackFilePath(selected, resolved))
    {
        return QStringLiteral("反馈路径无效：需要本机绝对路径或 UNC 映射路径，以及合法的专用文件名。");
    }
    config = selected;
    return {};
}

QString FeedbackOptions::Apply(pbapp::EncoderConfig& config) const
{
    return Apply(config.feedback);
}

QString FeedbackOptions::Apply(pbapp::DecoderConfig& config) const
{
    return Apply(config.feedback);
}

void FeedbackOptions::SetTelemetry(const pbapp::FeedbackTelemetry& telemetry)
{
    if (!telemetry.requested)
    {
        return;
    }
    QString state;
    switch (telemetry.state)
    {
    case pbapp::FeedbackConnectionState::Disabled: state = QStringLiteral("已关闭"); break;
    case pbapp::FeedbackConnectionState::Waiting: state = QStringLiteral("等待连接/匹配状态"); break;
    case pbapp::FeedbackConnectionState::Active: state = QStringLiteral("有效"); break;
    case pbapp::FeedbackConnectionState::Stale: state = QStringLiteral("已过期，恢复常规轮播"); break;
    case pbapp::FeedbackConnectionState::Unavailable: state = QStringLiteral("不可用，视觉通道继续"); break;
    case pbapp::FeedbackConnectionState::Rejected: state = QStringLiteral("状态被拒绝，恢复常规轮播"); break;
    case pbapp::FeedbackConnectionState::ReceiverReportedComplete: state = QStringLiteral("接收方已验证完成"); break;
    }
    status_->setText(QStringLiteral("本次/上次反馈：%1 · 已持久化确认段 %2 · I/O 超时 %3%4%5").arg(state)
        .arg(telemetry.confirmedSegments).arg(telemetry.ioTimeouts)
        .arg(telemetry.terminalPublished ? QStringLiteral(" · 完成状态已发布") : QString())
        .arg(telemetry.listening ? QStringLiteral(" · 已完成，反馈监听中") : QString()));
}

void FeedbackOptions::Browse()
{
    QString selected;
    if (kind_->currentIndex() == 0)
    {
        selected = QFileDialog::getExistingDirectory(this, QStringLiteral("选择对应的反馈目录"), path_->text());
    }
    else if (role_ == FeedbackRole::EncoderReader)
    {
        selected = QFileDialog::getOpenFileName(this, QStringLiteral("选择 Decoder 的专用反馈文件（可直接粘贴映射路径）"), path_->text(), QStringLiteral("PixelBridge feedback (*.pbfb);;所有文件 (*)"));
    }
    else
    {
        selected = QFileDialog::getSaveFileName(this, QStringLiteral("选择新的专用反馈文件，不会覆盖无关文件"), path_->text(), QStringLiteral("PixelBridge feedback (*.pbfb);;所有文件 (*)"));
    }
    if (!selected.isEmpty())
    {
        path_->setText(QDir::toNativeSeparators(selected));
    }
}
}
