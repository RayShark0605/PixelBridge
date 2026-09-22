#include "encoder_shell_dialog.h"

#include "encoder_application_controller.h"
#include "encoder_monitor_catalog.h"
#include "folder_archive.h"
#include "gui_visual_mode.h"
#include "product_gui_helpers.h"

#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFormLayout>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QKeySequence>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSettings>
#include <QShortcut>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QVBoxLayout>

#include <cstdint>
#include <algorithm>
#include <limits>
#include <memory>
#include <vector>

namespace
{

[[nodiscard]] QString ConfigureCurrentMonitor(pbapp::EncoderConfig& config, QWidget& window)
{
    const HMONITOR selected = MonitorFromWindow(reinterpret_cast<HWND>(window.winId()), MONITOR_DEFAULTTONULL);
    std::vector<pbapp::MonitorInfo> monitors;
    if (selected == nullptr)
    {
        return QStringLiteral("无法确认当前屏幕；请将 Encoder 对话框移到目标屏幕后重试。");
    }
    const pbapp::MonitorCatalogStatus catalog = pbapp::EnumerateEncoderMonitors(monitors);
    if (!catalog)
    {
        return QStringLiteral("无法读取屏幕位置、DPI 或显示模式（code=%1，native=%2）。")
            .arg(static_cast<unsigned int>(catalog.code)).arg(catalog.nativeError);
    }
    const auto monitor = std::find_if(monitors.begin(), monitors.end(), [selected](const pbapp::MonitorInfo& candidate)
    {
        return candidate.monitor == selected;
    });
    if (monitor == monitors.end())
    {
        return QStringLiteral("显示器布局发生变化，请重新开始。");
    }
    config.singleMonitorFullscreen = *monitor;
    config.monitorClientOrigin = pbrenderd3d::PhysicalPoint{monitor->physicalRect.left, monitor->physicalRect.top};
    const QByteArray identity = QString::fromStdWString(monitor->deviceName).toUtf8();
    config.remoteMetadata.experimentMonitorIdentity.assign(identity.constData(), static_cast<std::size_t>(identity.size()));
    return {};
}

[[nodiscard]] bool ParseMaximumRunDurationSeconds(const QString& text, std::uint64_t& value) noexcept
{
    if (text.isEmpty())
    {
        return false;
    }
    for (const QChar character : text)
    {
        if (character < QLatin1Char('0') || character > QLatin1Char('9'))
        {
            return false;
        }
    }
    bool converted = false;
    const qulonglong parsed = text.toULongLong(&converted, 10);
    if (!converted || parsed > (std::numeric_limits<std::uint64_t>::max)())
    {
        return false;
    }
    value = static_cast<std::uint64_t>(parsed);
    return true;
}

[[nodiscard]] QString StateText(const pbapp::EncoderState state)
{
    switch (state)
    {
    case pbapp::EncoderState::Idle: return QStringLiteral("准备就绪");
    case pbapp::EncoderState::Preparing: return QStringLiteral("正在准备文件…");
    case pbapp::EncoderState::Broadcasting: return QStringLiteral("正在传输 · 按 Esc 停止");
    case pbapp::EncoderState::Stopping: return QStringLiteral("正在安全停止…");
    case pbapp::EncoderState::Stopped: return QStringLiteral("传输已停止");
    case pbapp::EncoderState::Failed: return QStringLiteral("传输未能继续");
    }
    return QStringLiteral("等待状态");
}

class ShellTransferDialog final : public QDialog
{
public:
    explicit ShellTransferDialog(const QString& selectedPath, const QString& settingsFile) :
        selectedPath_(QDir::toNativeSeparators(selectedPath)), controller_(this)
    {
        settings_ = settingsFile.isEmpty() ? std::make_unique<QSettings>() :
            std::make_unique<QSettings>(settingsFile, QSettings::IniFormat);
        setWindowTitle(QStringLiteral("PixelBridgeEncoder"));
        setFixedSize(460, 286);
        BuildUi();
        const QFileInfo input(selectedPath_);
        inputWasFolder_ = input.isDir();
        sourceLabel_->setText(QStringLiteral("对象：%1%2").arg(selectedPath_,
            inputWasFolder_ ? QStringLiteral("\n文件夹将先压缩为 ZIP") : QString()));
        LoadPreferences();
        UpdateSnapshot();
        connect(&controller_, &EncoderApplicationController::SnapshotChanged, this, &ShellTransferDialog::UpdateSnapshot);
        connect(&controller_, &EncoderApplicationController::TerminalStateReached, this, [this]()
        {
            UpdateSnapshot();
            if (closePending_)
            {
                close();
            }
        });
    }

    void reject() override
    {
        HandleCancel();
    }

protected:
    void closeEvent(QCloseEvent* event) override
    {
        if (controller_.IsActive())
        {
            closePending_ = true;
            controller_.RequestStop();
            event->ignore();
            return;
        }
        SavePreferences();
        event->accept();
    }

private:
    void BuildUi()
    {
        QVBoxLayout* const outer = new QVBoxLayout(this);
        outer->setContentsMargins(18, 16, 18, 14);
        outer->setSpacing(10);

        QLabel* const heading = new QLabel(QStringLiteral("发送到 Decoder"));
        heading->setStyleSheet(QStringLiteral("font-size:18px;font-weight:600;"));
        outer->addWidget(heading);

        sourceLabel_ = new QLabel();
        sourceLabel_->setObjectName(QStringLiteral("shellSource"));
        sourceLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        sourceLabel_->setWordWrap(true);
        outer->addWidget(sourceLabel_);

        QFormLayout* const form = new QFormLayout();
        form->setHorizontalSpacing(12);
        form->setVerticalSpacing(8);
        fpsSpin_ = new QSpinBox();
        fpsSpin_->setObjectName(QStringLiteral("shellLogicalFps"));
        fpsSpin_->setRange(1, 60);
        fpsSpin_->setSuffix(QStringLiteral(" Hz"));
        form->addRow(QStringLiteral("刷新率"), fpsSpin_);
        modeCombo_ = new QComboBox();
        modeCombo_->setObjectName(QStringLiteral("shellCarrierMode"));
        modeCombo_->addItems({QStringLiteral("标准"), QStringLiteral("灰阶高速"), QStringLiteral("PAM4"), QStringLiteral("PAM4 Wide")});
        form->addRow(QStringLiteral("模式"), modeCombo_);
        timeoutEdit_ = new QLineEdit();
        timeoutEdit_->setObjectName(QStringLiteral("shellMaximumRunDurationSeconds"));
        timeoutEdit_->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[0-9]*")), timeoutEdit_));
        timeoutEdit_->setPlaceholderText(QStringLiteral("0"));
        form->addRow(QStringLiteral("超时（秒）"), timeoutEdit_);
        outer->addLayout(form);

        statusLabel_ = new QLabel();
        statusLabel_->setWordWrap(true);
        outer->addWidget(statusLabel_);

        buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        buttons_->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确定并开始"));
        buttons_->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
        outer->addWidget(buttons_);
        connect(buttons_, &QDialogButtonBox::accepted, this, &ShellTransferDialog::StartTransmission);
        connect(buttons_, &QDialogButtonBox::rejected, this, &ShellTransferDialog::HandleCancel);

        escapeShortcut_ = new QShortcut(QKeySequence(Qt::Key_Escape), this);
        escapeShortcut_->setObjectName(QStringLiteral("shellEscape"));
        escapeShortcut_->setContext(Qt::WindowShortcut);
        escapeShortcut_->setAutoRepeat(false);
        connect(escapeShortcut_, &QShortcut::activated, this, &ShellTransferDialog::HandleCancel);
    }

    void LoadPreferences()
    {
        const int savedFps = settings_->value(QStringLiteral("g22/logicalFps"), 15).toInt();
        fpsSpin_->setValue(savedFps >= 1 && savedFps <= 60 ? savedFps : 15);
        bool converted = false;
        const int savedMode = settings_->value(QStringLiteral("g22/carrier"), pbapp::defaultGuiVisualModeIndex).toString().toInt(&converted);
        modeCombo_->setCurrentIndex(converted && pbapp::GetGuiVisualProfile(savedMode).has_value() ? savedMode : pbapp::defaultGuiVisualModeIndex);
        const QString savedTimeout = settings_->value(QStringLiteral("g22/maximumRunDurationSeconds"), QStringLiteral("0")).toString();
        std::uint64_t timeout = 0;
        timeoutEdit_->setText(ParseMaximumRunDurationSeconds(savedTimeout, timeout) ? QString::number(static_cast<qulonglong>(timeout)) : QStringLiteral("0"));
    }

    void SavePreferences(const QString& sourcePath = {})
    {
        if (!sourcePath.isEmpty())
        {
            settings_->setValue(QStringLiteral("g22/sourcePath"), sourcePath);
        }
        settings_->setValue(QStringLiteral("g22/logicalFps"), fpsSpin_->value());
        settings_->setValue(QStringLiteral("g22/carrier"), modeCombo_->currentIndex());
        std::uint64_t timeout = 0;
        settings_->setValue(QStringLiteral("g22/maximumRunDurationSeconds"),
            ParseMaximumRunDurationSeconds(timeoutEdit_->text(), timeout) ? QString::number(static_cast<qulonglong>(timeout)) : QStringLiteral("0"));
        settings_->sync();
    }

    [[nodiscard]] QString BuildConfiguration(pbapp::EncoderConfig& config)
    {
        const QFileInfo input(selectedPath_);
        if (!input.exists() || (!input.isFile() && !input.isDir()))
        {
            return QStringLiteral("右键选择的路径已不存在或不是受支持的文件系统对象。");
        }
        QString sourcePath = selectedPath_;
        if (input.isDir())
        {
            inputWasFolder_ = true;
            const pbencoder::FolderArchiveResult archive = pbencoder::CreateFolderArchive(selectedPath_);
            if (!archive.success)
            {
                return archive.errorMessage;
            }
            archivePath_ = archive.archivePath;
            sourcePath = archive.archivePath;
        }
        else if (!input.isReadable() || input.size() < 0 || static_cast<std::uint64_t>(input.size()) > 500ULL * 1024ULL * 1024ULL * 1024ULL)
        {
            inputWasFolder_ = false;
            return QStringLiteral("源文件不可读或超过当前 500 GiB 有界上限。");
        }
        const auto profile = pbapp::GetGuiVisualProfile(modeCombo_->currentIndex());
        if (!profile)
        {
            return QStringLiteral("传输模式无效，请重新选择。");
        }
        std::uint64_t timeout = 0;
        if (!ParseMaximumRunDurationSeconds(timeoutEdit_->text(), timeout))
        {
            return QStringLiteral("超时关闭必须是 uint64 范围内的非负整数秒；0 表示无超时关闭。");
        }
        config = pbapp::MakeUnifiedEncoderConfig(sourcePath.toStdWString(), static_cast<std::uint32_t>(fpsSpin_->value()));
        config.visualProfile = *profile;
        config.maximumRunDurationSeconds = timeout;
        const QString monitorError = ConfigureCurrentMonitor(config, *this);
        if (!monitorError.isEmpty())
        {
            return monitorError;
        }
        const auto modeStatus = pbapp::ApplyGuiEncoderVisualMode(config, modeCombo_->currentIndex(), false);
        return modeStatus ? QString() : pbgui::FromUtf8(modeStatus.message);
    }

    void StartTransmission()
    {
        if (controller_.IsActive() || closePending_)
        {
            return;
        }
        pbapp::EncoderConfig config;
        const QString configurationError = BuildConfiguration(config);
        if (!configurationError.isEmpty())
        {
            statusLabel_->setText(configurationError);
            if (!archivePath_.isEmpty())
            {
                QFile::remove(archivePath_);
                archivePath_.clear();
            }
            return;
        }
        SavePreferences(QString::fromStdWString(config.sourcePath));
        const QString error = controller_.Start(config);
        if (!error.isEmpty())
        {
            statusLabel_->setText(error);
            if (!archivePath_.isEmpty())
            {
                QFile::remove(archivePath_);
                archivePath_.clear();
            }
            return;
        }
        buttons_->button(QDialogButtonBox::Ok)->setEnabled(false);
        buttons_->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("关闭"));
        statusLabel_->setText(inputWasFolder_ ? QStringLiteral("文件夹已压缩为 ZIP，正在按普通文件发送；Decoder 完成后请自行解压。") :
            QStringLiteral("普通文件正在发送；Decoder 完成后再按 Esc 停止。"));
    }

    void HandleCancel()
    {
        if (pbencoder::GetShellDialogCancelAction(controller_.IsActive()) == pbencoder::ShellDialogCancelAction::RequestStop)
        {
            closePending_ = true;
            buttons_->button(QDialogButtonBox::Cancel)->setEnabled(false);
            controller_.RequestStop();
            return;
        }
        QDialog::reject();
    }

    void UpdateSnapshot()
    {
        const pbapp::EncoderSnapshot snapshot = controller_.GetSnapshot();
        if (snapshot.state == pbapp::EncoderState::Stopped || snapshot.state == pbapp::EncoderState::Failed)
        {
            statusLabel_->setText(snapshot.state == pbapp::EncoderState::Failed ?
                pbgui::FromUtf8(snapshot.errorDetail.empty() ? snapshot.statusMessage : snapshot.errorDetail) :
                (snapshot.stoppedByTimeout ? QStringLiteral("已按超时设置安全停止；这不表示 Decoder 已完成接收。") : QStringLiteral("传输已停止，恢复状态已保留。")));
        }
        else if (snapshot.state != pbapp::EncoderState::Idle)
        {
            statusLabel_->setText(StateText(snapshot.state));
        }
        const bool active = controller_.IsActive();
        fpsSpin_->setEnabled(!active && !closePending_);
        modeCombo_->setEnabled(!active && !closePending_);
        timeoutEdit_->setEnabled(!active && !closePending_);
        buttons_->button(QDialogButtonBox::Ok)->setEnabled(!active && !closePending_);
    }

    QString selectedPath_;
    QString archivePath_;
    std::unique_ptr<QSettings> settings_;
    EncoderApplicationController controller_;
    bool closePending_ = false;
    bool inputWasFolder_ = false;
    QLabel* sourceLabel_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QSpinBox* fpsSpin_ = nullptr;
    QComboBox* modeCombo_ = nullptr;
    QLineEdit* timeoutEdit_ = nullptr;
    QDialogButtonBox* buttons_ = nullptr;
    QShortcut* escapeShortcut_ = nullptr;
};

} // namespace

int RunEncoderShellDialog(const QString& selectedPath, const QString& settingsFile)
{
    ShellTransferDialog dialog(selectedPath, settingsFile);
    dialog.show();
    return dialog.exec();
}

bool RunEncoderShellDialogSmoke()
{
    QTemporaryDir scratch;
    if (!scratch.isValid())
    {
        return false;
    }
    const QString sourcePath = QDir(scratch.path()).filePath(QStringLiteral("shell-source"));
    if (!QDir().mkpath(sourcePath))
    {
        return false;
    }
    const QString settingsPath = QDir(scratch.path()).filePath(QStringLiteral("settings.ini"));
    ShellTransferDialog dialog(sourcePath, settingsPath);
    const auto* const fpsSpin = dialog.findChild<QSpinBox*>(QStringLiteral("shellLogicalFps"));
    const auto* const modeCombo = dialog.findChild<QComboBox*>(QStringLiteral("shellCarrierMode"));
    const auto* const timeoutEdit = dialog.findChild<QLineEdit*>(QStringLiteral("shellMaximumRunDurationSeconds"));
    const auto* const sourceLabel = dialog.findChild<QLabel*>(QStringLiteral("shellSource"));
    QShortcut* const escapeShortcut = dialog.findChild<QShortcut*>(QStringLiteral("shellEscape"));
    dialog.show();
    QApplication::processEvents();
    const bool escapeInvoked = escapeShortcut != nullptr && QMetaObject::invokeMethod(escapeShortcut, "activated", Qt::DirectConnection);
    QApplication::processEvents();
    return escapeInvoked && !dialog.isVisible() && dialog.result() == QDialog::Rejected && dialog.size() == QSize(460, 286) &&
        fpsSpin != nullptr && fpsSpin->minimum() == 1 && fpsSpin->maximum() == 60 &&
        modeCombo != nullptr && modeCombo->count() == 4 && modeCombo->currentIndex() == pbapp::defaultGuiVisualModeIndex &&
        timeoutEdit != nullptr && timeoutEdit->text() == QStringLiteral("0") && sourceLabel != nullptr &&
        sourceLabel->text().contains(QStringLiteral("文件夹"));
}
