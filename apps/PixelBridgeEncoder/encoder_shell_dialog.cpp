#include "encoder_shell_dialog.h"

#include "encoder_application_controller.h"
#include "encoder_display_options.h"
#include "encoder_rate_prior_options.h"
#include "feedback_options_qt.h"
#include <QScrollArea>
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
#include <QFile>
#include <QImage>
#include <QElapsedTimer>
#include <QThread>
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
#include <functional>
#include <atomic>
#include <iostream>
#include <vector>

namespace
{

using TargetConfigurator = std::function<QString(pbapp::EncoderConfig&, QWidget&)>;

[[nodiscard]] bool WaitForSmoke(const std::function<bool()>& predicate)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 5000)
    {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    return predicate();
}

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
    explicit ShellTransferDialog(const QString& selectedPath, const QString& settingsFile,
        TargetConfigurator configureTarget = ConfigureCurrentMonitor, pbapp::EncoderPresentationFactory presentationFactory = {}) :
        selectedPath_(QDir::toNativeSeparators(selectedPath)), controller_(this, std::move(presentationFactory),
            settingsFile.isEmpty() ? QString() : QFileInfo(settingsFile).dir().filePath(QStringLiteral("logs"))), configureTarget_(std::move(configureTarget))
    {
        settings_ = settingsFile.isEmpty() ? std::make_unique<QSettings>() :
            std::make_unique<QSettings>(settingsFile, QSettings::IniFormat);
        setWindowTitle(QStringLiteral("PixelBridgeEncoder"));
        setMinimumSize(460, 430);
        resize(460, 430);
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

    [[nodiscard]] bool RunDisplaySelectionSmoke(const bool savePreviews)
    {
        auto* const button = findChild<QPushButton*>(QStringLiteral("shellDisplayMode"));
        auto* const priorEnabled = findChild<QCheckBox*>(QStringLiteral("expectedReceptionRateEnabled"));
        auto* const priorMinimum = findChild<QSpinBox*>(QStringLiteral("expectedReceptionRateMinimum"));
        auto* const priorHint = findChild<QLabel*>(QStringLiteral("expectedReceptionRateHint"));
        if (button == nullptr || priorEnabled == nullptr || priorMinimum == nullptr || priorHint == nullptr || isVisible())
        {
            return false;
        }
        priorEnabled->setChecked(true);
        const auto layoutFits = [&](const QString& previewName)
        {
            ensurePolished();
            QCoreApplication::processEvents();
            static_cast<void>(grab());
            QImage image(size() * devicePixelRatioF(), QImage::Format_ARGB32);
            image.setDevicePixelRatio(devicePixelRatioF());
            image.fill(Qt::transparent);
            render(&image);
            const bool fits = !image.isNull() && rect().contains(QRect(button->mapTo(this, QPoint(0, 0)), button->size())) &&
                rect().contains(QRect(ratePriorOptions_->mapTo(this, QPoint(0, 0)), ratePriorOptions_->size())) &&
                priorHint->height() >= priorHint->heightForWidth(priorHint->width()) &&
                button->width() >= button->fontMetrics().horizontalAdvance(button->text()) + 16 &&
                button->height() >= button->fontMetrics().height() + 4;
            if (!fits)
            {
                std::cout << "Shell prior layout: hint height=" << priorHint->height() << "; required=" << priorHint->heightForWidth(priorHint->width())
                    << "; container height=" << ratePriorOptions_->height() << "; required=" << ratePriorOptions_->heightForWidth(ratePriorOptions_->width())
                    << "; hasHeightForWidth=" << ratePriorOptions_->hasHeightForWidth() << '\n';
            }
            const auto directory = qEnvironmentVariable("PB_GUI_SMOKE_EVIDENCE_DIR");
            if (savePreviews && !previewName.isEmpty() && !directory.isEmpty())
            {
                QFile output(QDir(directory).filePath(previewName + QStringLiteral(".png")));
                return fits && output.open(QIODevice::WriteOnly | QIODevice::NewOnly) && image.save(&output, "PNG");
            }
            return fits;
        };
        for (int index = 0; index < 4; index++)
        {
            modeCombo_->setCurrentIndex(index);
            pbapp::EncoderConfig nativeConfig;
            if (!BuildConfiguration(nativeConfig).isEmpty() || nativeConfig.presentationModel != pbrenderd3d::PresentationModel::Flip ||
                !button->isEnabled() || !button->text().contains(QStringLiteral("D3D11")) ||
                !layoutFits(index == 2 ? QStringLiteral("qt-display-shell-native") : QString()))
            {
                return false;
            }
            if (nativeConfig.expectedReceptionRate.has_value() != (index == 2) || priorEnabled->isEnabled() != (index == 2) ||
                priorMinimum->isEnabled() != (index == 2))
            {
                return false;
            }
            button->click();
            pbapp::EncoderConfig config;
            if (!BuildConfiguration(config).isEmpty() || config.presentationModel != pbrenderd3d::PresentationModel::QtRaster ||
                config.visualProfile != nativeConfig.visualProfile || config.logicalVisualFps != nativeConfig.logicalVisualFps ||
                config.fullscreenRasterWidth != nativeConfig.fullscreenRasterWidth || config.segmentTargetBytes != nativeConfig.segmentTargetBytes ||
                config.expectedReceptionRate != nativeConfig.expectedReceptionRate ||
                !button->text().contains(QStringLiteral("Qt")) || !layoutFits(index == 2 ? QStringLiteral("qt-display-shell-qt") : QString()))
            {
                return false;
            }
            button->click();
        }
        closePending_ = true;
        UpdateSnapshot();
        pbapp::EncoderConfig config;
        const bool closeLocked = !button->isEnabled() && QMetaObject::invokeMethod(button, "clicked", Qt::DirectConnection, Q_ARG(bool, false)) &&
            BuildConfiguration(config).isEmpty() && config.presentationModel == pbrenderd3d::PresentationModel::Flip;
        closePending_ = false;
        UpdateSnapshot();
        button->click();
        SavePreferences();
        return closeLocked && controller_.GetSnapshot().runGeneration == 0;
    }

    [[nodiscard]] bool RunDisplayActiveSmoke(const std::function<bool()>& presentationCreated)
    {
        modeCombo_->setCurrentIndex(0);
        fpsSpin_->setValue(30);
        displayButton_->click();
        buttons_->button(QDialogButtonBox::Ok)->click();
        const bool created = WaitForSmoke(presentationCreated);
        pbapp::EncoderConfig config;
        const auto generation = controller_.GetSnapshot().runGeneration;
        const bool locked = created && controller_.IsActive() && !displayButton_->isEnabled() &&
            QMetaObject::invokeMethod(displayButton_, "clicked", Qt::DirectConnection, Q_ARG(bool, false)) &&
            BuildConfiguration(config).isEmpty() && config.presentationModel == pbrenderd3d::PresentationModel::QtRaster &&
            QMetaObject::invokeMethod(buttons_->button(QDialogButtonBox::Ok), "clicked", Qt::DirectConnection, Q_ARG(bool, false)) &&
            controller_.GetSnapshot().runGeneration == generation;
        controller_.RequestStop();
        const bool stopped = WaitForSmoke([this] { return !controller_.IsActive(); });
        UpdateSnapshot();
        displayButton_->click();
        return locked && stopped && displayButton_->isEnabled() && BuildConfiguration(config).isEmpty() &&
            config.presentationModel == pbrenderd3d::PresentationModel::Flip;
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
        auto* const rootLayout = new QVBoxLayout(this);
        rootLayout->setContentsMargins(0, 0, 0, 0);
        auto* const content = new QWidget();
        QVBoxLayout* const outer = new QVBoxLayout(content);
        auto* const scroll = new QScrollArea(this);
        scroll->setObjectName(QStringLiteral("shellOptionsScroll"));
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setWidget(content);
        rootLayout->addWidget(scroll, 1);
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
        displayButton_ = new QPushButton();
        displayButton_->setObjectName(QStringLiteral("shellDisplayMode"));
        displayButton_->setMinimumHeight(32);
        displayButton_->setToolTip(QStringLiteral("停止后可切换。只改变发送端画面绘制；Decoder 仍须选择相同传输模式。每次新开窗口默认 D3D11。"));
        connect(displayButton_, &QPushButton::clicked, this, &ShellTransferDialog::ToggleDisplayMode);
        form->addRow(displayButton_);
        timeoutEdit_ = new QLineEdit();
        timeoutEdit_->setObjectName(QStringLiteral("shellMaximumRunDurationSeconds"));
        timeoutEdit_->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[0-9]*")), timeoutEdit_));
        timeoutEdit_->setPlaceholderText(QStringLiteral("0"));
        form->addRow(QStringLiteral("超时（秒）"), timeoutEdit_);
        outer->addLayout(form);
        ratePriorOptions_ = new pbencoder::EncoderRatePriorOptions();
        outer->addWidget(ratePriorOptions_);
        feedbackOptions_ = new pbgui::FeedbackOptions(pbgui::FeedbackRole::EncoderReader);
        outer->addWidget(feedbackOptions_);
        connect(modeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { UpdateSnapshot(); });

        statusLabel_ = new QLabel();
        statusLabel_->setWordWrap(true);
        outer->addWidget(statusLabel_);

        buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        buttons_->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确定并开始"));
        buttons_->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
        rootLayout->addWidget(buttons_);
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
        ratePriorOptions_->Load(*settings_);
        feedbackOptions_->Load(*settings_);
    }

    void SavePreferences(const QString& sourcePath = {})
    {
        if (!sourcePath.isEmpty())
        {
            settings_->setValue(QStringLiteral("g22/sourcePath"), sourcePath);
        }
        settings_->setValue(QStringLiteral("g22/logicalFps"), fpsSpin_->value());
        settings_->setValue(QStringLiteral("g22/carrier"), modeCombo_->currentIndex());
        ratePriorOptions_->Save(*settings_);
        feedbackOptions_->Save(*settings_);
        std::uint64_t timeout = 0;
        settings_->setValue(QStringLiteral("g22/maximumRunDurationSeconds"),
            ParseMaximumRunDurationSeconds(timeoutEdit_->text(), timeout) ? QString::number(static_cast<qulonglong>(timeout)) : QStringLiteral("0"));
        settings_->sync();
    }

    [[nodiscard]] QString BuildConfiguration(pbapp::EncoderConfig& config)
    {
        // Folder preflight is synchronous but can emit process callbacks.
        // Freeze this run's selection before any source/archive work begins.
        const auto displayMode = displayMode_;
        const auto selectedProfile = pbapp::GetGuiVisualProfile(modeCombo_->currentIndex());
        pbapp::EncoderConfig priorConfig;
        priorConfig.visualProfile = selectedProfile.value_or(pbapp::VisualProfile::UnifiedLc4);
        const QString priorError = ratePriorOptions_->Apply(priorConfig);
        if (!priorError.isEmpty())
        {
            return priorError;
        }
        const QString feedbackError = feedbackOptions_->Apply(priorConfig);
        if (!feedbackError.isEmpty())
        {
            return feedbackError;
        }
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
        config.presentationModel = displayMode;
        config.visualProfile = *profile;
        config.maximumRunDurationSeconds = timeout;
        config.expectedReceptionRate = priorConfig.expectedReceptionRate;
        config.feedback = priorConfig.feedback;
        const QString monitorError = configureTarget_(config, *this);
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
        displayButton_->setEnabled(false);
        pbapp::EncoderConfig config;
        ratePriorOptions_->setEnabled(false);
        feedbackOptions_->setEnabled(false);
        const QString configurationError = BuildConfiguration(config);
        ratePriorOptions_->setEnabled(true);
        feedbackOptions_->setEnabled(true);
        if (!configurationError.isEmpty())
        {
            UpdateSnapshot();
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
            UpdateSnapshot();
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

    void ToggleDisplayMode()
    {
        if (!displayButton_->isEnabled() || controller_.IsActive() ||
            !pbencoder::CanChangeEncoderDisplayMode(controller_.GetSnapshot().state, closePending_, false, false))
        {
            return;
        }
        const auto next = pbencoder::GetNextEncoderDisplayMode(displayMode_);
        if (next)
        {
            displayMode_ = *next;
            UpdateSnapshot();
        }
    }

    void UpdateSnapshot()
    {
        const pbapp::EncoderSnapshot snapshot = controller_.GetSnapshot();
        if (snapshot.state == pbapp::EncoderState::Stopped || snapshot.state == pbapp::EncoderState::Failed)
        {
            statusLabel_->setText(snapshot.state == pbapp::EncoderState::Failed ?
                pbgui::FromUtf8(snapshot.errorDetail.empty() ? snapshot.statusMessage : snapshot.errorDetail) :
                (snapshot.stoppedByTimeout ? QStringLiteral("已按超时设置安全停止；这不表示 Decoder 已完成接收。") :
                    snapshot.stopReason == pbapp::EncoderStopReason::ReceiverVerified ? QStringLiteral("接收方报告整文件验证与发布均通过；广播已自动安全停止。") :
                    QStringLiteral("传输已停止，恢复状态已保留。")));
        }
        else if (snapshot.state != pbapp::EncoderState::Idle)
        {
            statusLabel_->setText(StateText(snapshot.state));
        }
        const bool active = controller_.IsActive();
        displayButton_->setText(QString::fromUtf8(pbencoder::GetEncoderDisplayModeLabel(displayMode_)));
        displayButton_->setEnabled(!active && pbencoder::CanChangeEncoderDisplayMode(snapshot.state, closePending_, false, false));
        fpsSpin_->setEnabled(!active && !closePending_);
        modeCombo_->setEnabled(!active && !closePending_);
        timeoutEdit_->setEnabled(!active && !closePending_);
        ratePriorOptions_->SetContext(pbapp::GetGuiVisualProfile(modeCombo_->currentIndex()).value_or(pbapp::VisualProfile::UnifiedLc4), !active && !closePending_);
        feedbackOptions_->SetEditable(!active && !closePending_);
        feedbackOptions_->SetTelemetry(snapshot.feedback);
        buttons_->button(QDialogButtonBox::Ok)->setEnabled(!active && !closePending_);
    }

    QString selectedPath_;
    QString archivePath_;
    std::unique_ptr<QSettings> settings_;
    EncoderApplicationController controller_;
    TargetConfigurator configureTarget_;
    pbrenderd3d::PresentationModel displayMode_ = pbrenderd3d::PresentationModel::Flip;
    bool closePending_ = false;
    bool inputWasFolder_ = false;
    QLabel* sourceLabel_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QSpinBox* fpsSpin_ = nullptr;
    QComboBox* modeCombo_ = nullptr;
    QPushButton* displayButton_ = nullptr;
    QLineEdit* timeoutEdit_ = nullptr;
    pbencoder::EncoderRatePriorOptions* ratePriorOptions_ = nullptr;
    pbgui::FeedbackOptions* feedbackOptions_ = nullptr;
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

bool RunEncoderShellDisplaySmoke()
{
    QTemporaryDir scratch;
    if (!scratch.isValid())
    {
        return false;
    }
    const auto sourcePath = scratch.filePath(QStringLiteral("shell-display.bin"));
    QFile source(sourcePath);
    if (!source.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    {
        return false;
    }
    source.close();
    const auto settingsPath = scratch.filePath(QStringLiteral("settings.ini"));
    {
        QSettings injected(settingsPath, QSettings::IniFormat);
        injected.setValue(QStringLiteral("g22/displayMode"), QStringLiteral("qt-raster"));
        injected.setValue(QStringLiteral("g22/presentationModel"), QStringLiteral("qt-raster"));
    }
    const TargetConfigurator configureProfiles = [](pbapp::EncoderConfig& config, QWidget&)
    {
        pbapp::MonitorInfo monitor;
        monitor.monitor = reinterpret_cast<HMONITOR>(std::uintptr_t{1});
        monitor.deviceName = L"Offscreen shell-selection fixture";
        monitor.physicalRect = config.visualProfile == pbapp::VisualProfile::ExperimentalPam4 ? RECT{0, 0, 1920, 1080} : RECT{0, 0, 2560, 1600};
        monitor.workRect = monitor.physicalRect;
        monitor.dpiX = 96;
        monitor.dpiY = 96;
        monitor.refreshRate = 60;
        monitor.rotation = DXGI_MODE_ROTATION_IDENTITY;
        config.singleMonitorFullscreen = monitor;
        config.monitorClientOrigin = pbrenderd3d::PhysicalPoint{0, 0};
        config.remoteMetadata.experimentMonitorIdentity = "Offscreen shell-selection fixture";
        return QString();
    };
    bool selectionPassed = true;
    for (int windowIndex = 0; windowIndex < 2; windowIndex++)
    {
        ShellTransferDialog dialog(sourcePath, settingsPath, configureProfiles);
        selectionPassed = dialog.RunDisplaySelectionSmoke(windowIndex == 0) && selectionPassed;
    }
    class WaitingPresentation final : public pbapp::EncoderPresentation
    {
    public:
        pbrenderd3d::DataWindowSnapshot GetSnapshot() const override
        {
            pbrenderd3d::DataWindowSnapshot snapshot;
            snapshot.state = stopped_ ? pbrenderd3d::WindowState::Stopped : pbrenderd3d::WindowState::Starting;
            return snapshot;
        }
        pbrenderd3d::PresentationStatus SubmitFrame(const pbrenderd3d::CanonicalBgraFrameView&) override
        {
            return pbrenderd3d::PresentationStatus::Failure(pbrenderd3d::PresentationErrorCode::NotRunning, pbrenderd3d::PresentationStage::Upload);
        }
        void RequestStop() noexcept override
        {
            stopped_ = true;
        }
        void Stop() noexcept override
        {
            stopped_ = true;
        }
    private:
        bool stopped_ = false;
    };
    std::atomic<bool> created = false;
    const pbapp::EncoderPresentationFactory factory = [&](const pbrenderd3d::DataWindowConfig& config)
    {
        created = config.presentationModel == pbrenderd3d::PresentationModel::QtRaster;
        return std::make_unique<WaitingPresentation>();
    };
    const TargetConfigurator configureWindowed = [&scratch](pbapp::EncoderConfig& config, QWidget&)
    {
        config.monitorClientOrigin = pbrenderd3d::PhysicalPoint{0, 0};
        config.sessionStateRoot = scratch.filePath(QStringLiteral("sessions")).toStdWString();
        return QString();
    };
    ShellTransferDialog active(sourcePath, settingsPath, configureWindowed, factory);
    const bool activePassed = active.RunDisplayActiveSmoke([&] { return created.load(); });
    std::cout << "Qt display shell smoke: selection/layout=" << selectionPassed << "; active-handler=" << activePassed << '\n';
    return selectionPassed && activePassed;
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
    // The added prior controls require the taller initial dialog; the
    // selection/layout smoke independently checks wrapped text visibility.
    return escapeInvoked && !dialog.isVisible() && dialog.result() == QDialog::Rejected && dialog.size() == QSize(460, 430) &&
        fpsSpin != nullptr && fpsSpin->minimum() == 1 && fpsSpin->maximum() == 60 &&
        modeCombo != nullptr && modeCombo->count() == 4 && modeCombo->currentIndex() == pbapp::defaultGuiVisualModeIndex &&
        timeoutEdit != nullptr && timeoutEdit->text() == QStringLiteral("0") && sourceLabel != nullptr &&
        sourceLabel->text().contains(QStringLiteral("文件夹"));
}
