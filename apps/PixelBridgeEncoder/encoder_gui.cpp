#include "encoder_gui.h"
#include "encoder_application_controller.h"
#include "encoder_monitor_catalog.h"
#include "product_gui_helpers.h"
#include "gui_native_smoke.h"
#include "run_report.h"
#include "step1_gui_evidence_qt.h"
#include "pbcore/build_info.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QSpinBox>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QVBoxLayout>

#include <atomic>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <string_view>

namespace
{

using TargetConfigurator = std::function<QString(pbapp::EncoderConfig&, QWidget&)>;

[[nodiscard]] QString ConfigureCurrentMonitor(pbapp::EncoderConfig& config, QWidget& window)
{
    const HMONITOR selected = MonitorFromWindow(reinterpret_cast<HWND>(window.winId()), MONITOR_DEFAULTTONULL);
    std::vector<pbapp::MonitorInfo> monitors;
    if (selected == nullptr)
    {
        return QStringLiteral("无法确认当前屏幕；请将 Encoder 窗口移到目标屏幕后重试。");
    }
    const pbapp::MonitorCatalogStatus catalog = pbapp::EnumerateEncoderMonitors(monitors);
    if (!catalog)
    {
        return QStringLiteral("无法读取屏幕位置、DPI或显示模式（code=%1，native=%2）。请保留此错误信息用于诊断。")
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

class EncoderWindow final : public QMainWindow
{
public:
    explicit EncoderWindow(pbapp::EncoderPresentationFactory presentationFactory = {}, const QString& settingsFile = {},
        TargetConfigurator configureTarget = ConfigureCurrentMonitor, std::function<bool()> confirmDeletion = {},
        pbgui::Step1GuiEvidence* evidence = nullptr) :
        controller_(nullptr, std::move(presentationFactory)), configureTarget_(std::move(configureTarget)),
        confirmDeletion_(std::move(confirmDeletion)), evidence_(evidence)
    {
        settings_ = settingsFile.isEmpty() ? std::make_unique<QSettings>() :
            std::make_unique<QSettings>(settingsFile, QSettings::IniFormat);
        setWindowTitle(QStringLiteral("PixelBridge Encoder"));
        setMinimumSize(640, 460);
        resize(740, 520);
        BuildUi();
        // Do not import the legacy geometry, profiles, active state or 240 Hz preference.
        sourceEdit_->setText(settings_->value(QStringLiteral("g22/sourcePath")).toString());
        cacheEdit_->setText(settings_->value(QStringLiteral("g22/sessionRoot")).toString());
        const int savedFps = settings_->value(QStringLiteral("g22/logicalFps"), 15).toInt();
        fpsSpin_->setValue(savedFps >= 1 && savedFps <= 60 ? savedFps : 15);
        connect(&controller_, &EncoderApplicationController::SnapshotChanged, this, &EncoderWindow::UpdateSnapshot);
        connect(&controller_, &EncoderApplicationController::TerminalStateReached, this, [this]()
        {
            if (closePending_)
            {
                close();
            }
        });
        UpdateSnapshot();
        if (evidence_)
        {
            const auto timer = new QTimer(this);
            timer->setInterval(100);
            connect(timer, &QTimer::timeout, this, [this]()
            {
                evidence_->Observe(controller_.GetSnapshot());
                setWindowTitle(evidence_->StatusText());
            });
            timer->start();
        }
    }

    ~EncoderWindow() override
    {
        if (evidence_)
        {
            evidence_->Observe(controller_.StopAndGetSnapshot());
        }
    }

    [[nodiscard]] bool RunSmoke(const QString& sourcePath, const QString& sessionRoot,
        const std::function<bool()>& presentationCreated, bool& confirmDeletion)
    {
        if (pbgui::HumanBytes(1023) != QStringLiteral("1023 B") || pbgui::HumanBytes(1024) != QStringLiteral("1.00 KB") ||
            pbgui::HumanBytes(1024ULL * 1024) != QStringLiteral("1.00 MB") ||
            pbgui::HumanBytes(1024ULL * 1024 * 1024) != QStringLiteral("1.00 GB") ||
            pbgui::HumanBytes(pbapp::maximumInstantFileBytes) != QStringLiteral("500.00 GB"))
        {
            return false;
        }
        if (isVisible() || tabs_->count() != 2 || fpsSpin_->minimum() != 1 || fpsSpin_->maximum() != 60 ||
            fpsSpin_->value() != 15 || startButton_->isEnabled() || escapeShortcut_->context() != Qt::WindowShortcut ||
            escapeShortcut_->autoRepeat() || !details_->isReadOnly() || deleteButton_->isEnabled())
        {
            return false;
        }
        sourceEdit_->setText(sourcePath);
        cacheEdit_->setText(sessionRoot);
        if (!startButton_->isEnabled() || !pbgui::SaveTabPreviews(*this, *tabs_, QStringLiteral("encoder-idle")))
        {
            return false;
        }
        startButton_->click();
        // Lock controls immediately, including the entire prescan phase.
        if (sourceEdit_->isEnabled() || fpsSpin_->isEnabled() || cacheEdit_->isEnabled() ||
            !escapeShortcut_->isEnabled() || controller_.SetLogicalVisualFps(60).isEmpty())
        {
            return false;
        }
        // Even a programmatic change of the disabled widget cannot reconfigure this run.
        fpsSpin_->setValue(60);
        if (!WaitFor(presentationCreated))
        {
            return false;
        }
        UpdateSnapshot();
        const pbapp::EncoderSnapshot prepared = controller_.GetSnapshot();
        const std::filesystem::path directory(std::u8string(prepared.sessionStateDirectory.begin(), prepared.sessionStateDirectory.end()));
        if (!prepared.preparationComplete || prepared.configuredLogicalVisualFps != 15 ||
            prepared.visualProfile != pbapp::VisualProfile::UnifiedLc4 || prepared.sessionIdHex.empty() ||
            directory.parent_path() != std::filesystem::path(sessionRoot.toStdWString()) || !std::filesystem::exists(directory))
        {
            return false;
        }
        // Invoke the local shortcut signal inside this offscreen process; no OS input is sent.
        if (!QMetaObject::invokeMethod(escapeShortcut_, "activated", Qt::DirectConnection) ||
            !WaitFor([this]()
            {
                return controller_.GetSnapshot().state == pbapp::EncoderState::Stopped;
            }))
        {
            return false;
        }
        UpdateSnapshot();
        if (!fpsSpin_->isEnabled() || !cacheEdit_->isEnabled() || !startButton_->isEnabled() ||
            !deleteButton_->isEnabled() || !std::filesystem::exists(directory))
        {
            return false;
        }
        confirmDeletion = false;
        deleteButton_->click();
        if (!std::filesystem::exists(directory) || controller_.GetSnapshot().sessionDeleted)
        {
            return false;
        }
        confirmDeletion = true;
        deleteButton_->click();
        if (!controller_.GetSnapshot().sessionDeleted || std::filesystem::exists(directory) || !QFileInfo::exists(sourcePath))
        {
            return false;
        }
        fpsSpin_->setValue(1);
        SavePreferences();
        return !deleteButton_->isEnabled() && settings_->value(QStringLiteral("g22/logicalFps")).toInt() == 1 &&
            settings_->value(QStringLiteral("g22/sessionRoot")).toString() == sessionRoot &&
            pbgui::SaveTabPreviews(*this, *tabs_, QStringLiteral("encoder-stopped"));
    }

    [[nodiscard]] bool RunMeasurementStartSmoke(const QString& sourcePath, const std::function<bool()>& presentationCreated)
    {
        if (!evidence_ || isVisible())
        {
            return false;
        }
        sourceEdit_->setText(sourcePath);
        startButton_->click();
        if (!WaitFor(presentationCreated))
        {
            return false;
        }
        const auto started = controller_.GetSnapshot();
        const bool accepted = started.runId == evidence_->RunId().toStdString() && started.runId.size() == 32 &&
            started.preparationComplete && started.measurement && started.measurement->runGeneration != 0 &&
            !QFileInfo::exists(QDir(evidence_->RunDirectory()).filePath(QStringLiteral("start-rejected.json")));
        const auto stopped = controller_.StopAndGetSnapshot();
        evidence_->Observe(stopped);
        return accepted && stopped.state == pbapp::EncoderState::Stopped;
    }

    [[nodiscard]] int RunNativeSmoke(const pbgui::NativeSmokeOptions& options)
    {
        if (QFileInfo(options.inputPath).size() != 1024 * 1024 || !pbgui::ShowNativeSmoke(*this, options))
        {
            return 2;
        }
        sourceEdit_->setText(options.inputPath);
        cacheEdit_->setText(QDir(options.evidenceDirectory).filePath(QStringLiteral("sessions")));
        fpsSpin_->setValue(15);
        startButton_->click();
        const bool lockedAtStart = controller_.IsActive() && !fpsSpin_->isEnabled() && !sourceEdit_->isEnabled();
        bool safetyHeld = true;
        QJsonObject safetyFailure;
        QElapsedTimer elapsed;
        elapsed.start();
        while (controller_.IsActive() && elapsed.elapsed() < options.durationSeconds * 1000)
        {
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            if (!pbgui::NativeWindowIsContained(*this, options, &safetyFailure))
            {
                safetyHeld = false;
                break;
            }
            QThread::msleep(20);
        }
        const auto beforeStop = controller_.GetSnapshot();
        // Exercise the same local Qt shortcut connection without synthesizing
        // any keyboard input. Only this explicit test has a bounded deadline.
        const bool localShortcutInvoked = QMetaObject::invokeMethod(escapeShortcut_, "activated", Qt::DirectConnection);
        controller_.RequestStop();
        const bool stopped = WaitFor([this]()
        {
            return !controller_.IsActive();
        });
        UpdateSnapshot();
        const auto snapshot = controller_.GetSnapshot();
        const bool broadcastUntilDeadline = elapsed.elapsed() >= options.durationSeconds * 1000 &&
            beforeStop.state == pbapp::EncoderState::Broadcasting && beforeStop.submittedFrames > 0;
        const bool passed = safetyHeld && lockedAtStart && broadcastUntilDeadline && localShortcutInvoked && stopped &&
            snapshot.state == pbapp::EncoderState::Stopped && snapshot.configuredLogicalVisualFps == 15 &&
            snapshot.singleMonitorFullscreen && snapshot.sourceBytes == 1024 * 1024 && fpsSpin_->isEnabled();
        const pbapp::RunReportContext context{"PixelBridgeEncoder", std::string(pbcore::GetBuildInfo().version), PB_GIT_COMMIT,
            QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString()};
        const QJsonObject checks{{"controlsLockedAtStart", lockedAtStart}, {"broadcastUntilDeadline", broadcastUntilDeadline},
            {"safetyFailure", safetyFailure},
            {"localShortcutInvoked", localShortcutInvoked}, {"physicalEscKeyTested", false}, {"stopped", stopped},
            {"configuredFps", static_cast<int>(snapshot.configuredLogicalVisualFps)},
            {"singleMonitorFullscreen", snapshot.singleMonitorFullscreen},
            {"dataWindow", QJsonObject{{"left", beforeStop.dataWindowLeft}, {"top", beforeStop.dataWindowTop},
                {"width", static_cast<int>(beforeStop.dataWindowWidth)}, {"height", static_cast<int>(beforeStop.dataWindowHeight)}}}};
        const bool saved = pbgui::WriteNativeSmokeResult(options, QStringLiteral("Encoder"), passed, safetyHeld, checks,
            pbapp::BuildEncoderRunReportJson(context, snapshot));
        hide();
        return passed && saved ? 0 : 1;
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
    [[nodiscard]] static bool WaitFor(const std::function<bool()>& predicate)
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

    void BuildUi()
    {
        QWidget* const central = new QWidget(this);
        QVBoxLayout* const outer = new QVBoxLayout(central);
        outer->setContentsMargins(24, 20, 24, 20);
        outer->setSpacing(16);
        QLabel* const heading = pbgui::TextLabel(QStringLiteral("文件传输"));
        heading->setStyleSheet(QStringLiteral("font-size:24px;font-weight:600;"));
        outer->addWidget(heading);
        tabs_ = new QTabWidget();
        tabs_->setObjectName(QStringLiteral("encoderTabs"));
        QWidget* const mainPage = new QWidget();
        QVBoxLayout* const mainLayout = new QVBoxLayout(mainPage);
        mainLayout->setContentsMargins(20, 22, 20, 20);
        mainLayout->setSpacing(16);
        mainLayout->addWidget(pbgui::TextLabel(QStringLiteral("选择一个文件，在当前屏幕持续传输。")));
        mainLayout->addWidget(pbgui::TextLabel(QStringLiteral("源文件")));
        QHBoxLayout* const fileRow = new QHBoxLayout();
        sourceEdit_ = new QLineEdit();
        sourceEdit_->setObjectName(QStringLiteral("sourcePath"));
        sourceEdit_->setPlaceholderText(QStringLiteral("选择文件（0 B 至 500 GB，按 Windows 单位）"));
        browseButton_ = new QPushButton(QStringLiteral("选择文件…"));
        fileRow->addWidget(sourceEdit_, 1);
        fileRow->addWidget(browseButton_);
        mainLayout->addLayout(fileRow);
        QHBoxLayout* const rateRow = new QHBoxLayout();
        rateRow->addWidget(pbgui::TextLabel(QStringLiteral("刷新帧率")));
        fpsSpin_ = new QSpinBox();
        fpsSpin_->setObjectName(QStringLiteral("logicalFps"));
        fpsSpin_->setRange(1, 60);
        fpsSpin_->setValue(15);
        fpsSpin_->setSuffix(QStringLiteral(" Hz"));
        fpsSpin_->setToolTip(QStringLiteral("默认 15 Hz。开始准备后锁定；停止后才能修改。"));
        rateRow->addWidget(fpsSpin_);
        rateRow->addStretch();
        startButton_ = new QPushButton(QStringLiteral("开始传输"));
        startButton_->setObjectName(QStringLiteral("startTransmission"));
        startButton_->setMinimumSize(136, 38);
        rateRow->addWidget(startButton_);
        mainLayout->addLayout(rateRow);
        stateLabel_ = pbgui::TextLabel();
        stateLabel_->setStyleSheet(QStringLiteral("font-size:16px;font-weight:600;"));
        mainLayout->addWidget(stateLabel_);
        messageLabel_ = pbgui::TextLabel();
        mainLayout->addWidget(messageLabel_);
        mainLayout->addStretch();
        mainLayout->addWidget(pbgui::TextLabel(QStringLiteral("准备完成后全屏显示 · 按 Esc 停止传输\nEsc 仅在 Encoder 持有键盘焦点时生效，停止后保留恢复状态。")));
        tabs_->addTab(mainPage, QStringLiteral("传输"));

        QWidget* const advancedPage = new QWidget();
        QVBoxLayout* const advancedLayout = new QVBoxLayout(advancedPage);
        advancedLayout->setContentsMargins(20, 22, 20, 20);
        advancedLayout->setSpacing(12);
        advancedLayout->addWidget(pbgui::TextLabel(QStringLiteral("会话缓存目录")));
        QHBoxLayout* const cacheRow = new QHBoxLayout();
        cacheEdit_ = new QLineEdit();
        cacheEdit_->setObjectName(QStringLiteral("sessionCacheDirectory"));
        cacheEdit_->setPlaceholderText(QStringLiteral("留空使用本机默认 EncoderSessions 目录"));
        cacheBrowse_ = new QPushButton(QStringLiteral("选择目录…"));
        cacheRow->addWidget(cacheEdit_, 1);
        cacheRow->addWidget(cacheBrowse_);
        advancedLayout->addLayout(cacheRow);
        advancedLayout->addWidget(pbgui::TextLabel(QStringLiteral("缓存保存本端会话和断点索引，不向 Decoder 传递文件。更改目录只影响下一次开始。")));
        QHBoxLayout* const toolsRow = new QHBoxLayout();
        deleteButton_ = new QPushButton(QStringLiteral("结束并删除当前会话…"));
        deleteButton_->setObjectName(QStringLiteral("endAndDeleteSession"));
        exportButton_ = new QPushButton(QStringLiteral("导出诊断报告…"));
        toolsRow->addWidget(deleteButton_);
        toolsRow->addWidget(exportButton_);
        toolsRow->addStretch();
        advancedLayout->addLayout(toolsRow);
        details_ = new QPlainTextEdit();
        details_->setObjectName(QStringLiteral("encoderDiagnostics"));
        details_->setReadOnly(true);
        advancedLayout->addWidget(details_, 1);
        tabs_->addTab(advancedPage, QStringLiteral("高级选项"));
        outer->addWidget(tabs_, 1);
        setCentralWidget(central);

        escapeShortcut_ = new QShortcut(QKeySequence(Qt::Key_Escape), this);
        escapeShortcut_->setContext(Qt::WindowShortcut);
        escapeShortcut_->setAutoRepeat(false);
        connect(escapeShortcut_, &QShortcut::activated, &controller_, &EncoderApplicationController::RequestStop);
        connect(sourceEdit_, &QLineEdit::textChanged, this, &EncoderWindow::UpdateActions);
        connect(startButton_, &QPushButton::clicked, this, &EncoderWindow::StartTransmission);
        connect(browseButton_, &QPushButton::clicked, this, [this]()
        {
            const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择要传输的文件"), sourceEdit_->text());
            if (!path.isEmpty())
            {
                sourceEdit_->setText(QDir::toNativeSeparators(path));
            }
        });
        connect(cacheBrowse_, &QPushButton::clicked, this, [this]()
        {
            const QString directory = QFileDialog::getExistingDirectory(this, QStringLiteral("选择会话缓存目录"), cacheEdit_->text());
            if (!directory.isEmpty())
            {
                cacheEdit_->setText(QDir::toNativeSeparators(directory));
            }
        });
        connect(deleteButton_, &QPushButton::clicked, this, &EncoderWindow::EndAndDeleteSession);
        connect(exportButton_, &QPushButton::clicked, this, &EncoderWindow::ExportReport);
    }

    void SavePreferences()
    {
        settings_->setValue(QStringLiteral("g22/sourcePath"), sourceEdit_->text());
        settings_->setValue(QStringLiteral("g22/sessionRoot"), cacheEdit_->text());
        settings_->setValue(QStringLiteral("g22/logicalFps"), fpsSpin_->value());
        settings_->sync();
    }

    void UpdateActions()
    {
        const auto snapshot = controller_.GetSnapshot();
        const bool active = pbapp::IsEncoderStateActive(snapshot.state);
        const bool editable = !active && !closePending_;
        sourceEdit_->setEnabled(editable);
        browseButton_->setEnabled(editable);
        fpsSpin_->setEnabled(editable);
        cacheEdit_->setEnabled(editable);
        cacheBrowse_->setEnabled(editable);
        const QFileInfo source(sourceEdit_->text());
        startButton_->setEnabled(editable && source.isFile() && source.isReadable() && source.size() >= 0 &&
            static_cast<std::uint64_t>(source.size()) <= pbapp::maximumInstantFileBytes);
        escapeShortcut_->setEnabled(active && !closePending_ && snapshot.state != pbapp::EncoderState::Stopping);
        deleteButton_->setEnabled(editable && !snapshot.sessionIdHex.empty() && !snapshot.sessionDeleted);
        exportButton_->setEnabled(!active && !closePending_ && snapshot.runGeneration != 0);
    }

    void StartTransmission()
    {
        if (controller_.IsActive() || closePending_)
        {
            return;
        }
        auto config = pbapp::MakeUnifiedEncoderConfig(sourceEdit_->text().toStdWString(), static_cast<std::uint32_t>(fpsSpin_->value()));
        config.sessionStateRoot = cacheEdit_->text().toStdWString();
        const QString targetError = configureTarget_(config, *this);
        if (!targetError.isEmpty())
        {
            messageLabel_->setText(targetError);
            return;
        }
        if (evidence_)
        {
            QString evidenceError;
            config.measurement = evidence_->BeginRun(evidenceError);
            if (!config.measurement)
            {
                messageLabel_->setText(evidenceError);
                return;
            }
            config.runId = evidence_->RunId().toStdString();
            const auto sessionRoot = QDir(evidence_->RunDirectory()).filePath(QStringLiteral("sessions"));
            config.sessionStateRoot = sessionRoot.toStdWString();
            cacheEdit_->setText(sessionRoot);
        }
        SavePreferences();
        const QString error = controller_.Start(config);
        UpdateSnapshot();
        if (!error.isEmpty())
        {
            if (evidence_)
            {
                evidence_->StartRejected(error);
            }
            messageLabel_->setText(error);
        }
    }

    void EndAndDeleteSession()
    {
        const auto snapshot = controller_.GetSnapshot();
        if (controller_.IsActive() || snapshot.sessionIdHex.empty() || snapshot.sessionDeleted)
        {
            return;
        }
        const bool confirmed = confirmDeletion_ ? confirmDeletion_() : QMessageBox::question(this,
            QStringLiteral("结束并删除会话"), QStringLiteral("仅删除本端当前会话的恢复索引，不删除源文件。再次传输将建立新会话。\n\n确定删除？"),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes;
        if (!confirmed)
        {
            return;
        }
        const QString error = controller_.EndAndDeleteSession(snapshot.runGeneration);
        UpdateSnapshot();
        if (!error.isEmpty())
        {
            messageLabel_->setText(error);
        }
    }

    void ExportReport()
    {
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出 Encoder 诊断报告"), QStringLiteral("encoder-report.json"), QStringLiteral("JSON (*.json)"));
        if (path.isEmpty())
        {
            return;
        }
        const pbapp::RunReportContext context{"PixelBridgeEncoder", std::string(pbcore::GetBuildInfo().version), PB_GIT_COMMIT,
            QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString()};
        const std::string report = pbapp::BuildEncoderRunReportJson(context, controller_.GetSnapshot());
        QFile output(path);
        if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
            output.write(report.data(), static_cast<qint64>(report.size())) != static_cast<qint64>(report.size()) || !output.flush())
        {
            messageLabel_->setText(QStringLiteral("报告未保存：请选择一个尚不存在、可写的文件路径。"));
        }
    }

    void UpdateSnapshot()
    {
        const auto snapshot = controller_.GetSnapshot();
        stateLabel_->setText(StateText(snapshot.state));
        QString message;
        if (snapshot.state == pbapp::EncoderState::Preparing)
        {
            const double progress = snapshot.sourceBytes == 0 ? 0.0 :
                100.0 * static_cast<double>(snapshot.preparedSourceBytes) / static_cast<double>(snapshot.sourceBytes);
            message = QStringLiteral("预扫描 %1% · %2 / %3").arg(progress, 0, 'f', 1)
                .arg(pbgui::HumanBytes(snapshot.preparedSourceBytes)).arg(pbgui::HumanBytes(snapshot.sourceBytes));
        }
        else if (snapshot.state == pbapp::EncoderState::Broadcasting)
        {
            message = QStringLiteral("持续循环发送，不等待对端确认。当前设置 %1 Hz。").arg(snapshot.configuredLogicalVisualFps);
        }
        else if (snapshot.state == pbapp::EncoderState::Failed)
        {
            message = pbgui::FromUtf8(snapshot.errorDetail.empty() ? snapshot.statusMessage : snapshot.errorDetail);
        }
        else if (snapshot.state == pbapp::EncoderState::Stopped)
        {
            message = snapshot.sessionDeleted ? QStringLiteral("会话索引已删除，源文件未改动。") : QStringLiteral("恢复状态已保留，可以重新开始。");
        }
        messageLabel_->setText(message);
        details_->setPlainText(QStringLiteral("PB-Unified-SC6-V3 · layout 10 · 1920 × 1080 BGRA8 SDR\n"
            "全屏：规范画布 1:1 居中；外围为中性背景，不拉伸数据\n"
            "固定：8 MB 分段 / 自动 RAW-zstd(level 3) / Robust LDPC / DirectRepeat-Wirehair V2\n"
            "文件上限：500 GB（1024 进位）；传输期间源文件保持只读锁定\n\n") + (snapshot.runGeneration == 0 ?
                QStringLiteral("尚未开始传输。运行详情将在建立会话后显示。") : pbgui::FromUtf8(pbapp::BuildEncoderDiagnostics(snapshot))));
        UpdateActions();
    }

    EncoderApplicationController controller_;
    std::unique_ptr<QSettings> settings_;
    TargetConfigurator configureTarget_;
    std::function<bool()> confirmDeletion_;
    pbgui::Step1GuiEvidence* evidence_ = nullptr;
    bool closePending_ = false;
    QTabWidget* tabs_ = nullptr;
    QLineEdit* sourceEdit_ = nullptr;
    QLineEdit* cacheEdit_ = nullptr;
    QSpinBox* fpsSpin_ = nullptr;
    QPushButton* browseButton_ = nullptr;
    QPushButton* cacheBrowse_ = nullptr;
    QPushButton* startButton_ = nullptr;
    QPushButton* deleteButton_ = nullptr;
    QPushButton* exportButton_ = nullptr;
    QLabel* stateLabel_ = nullptr;
    QLabel* messageLabel_ = nullptr;
    QPlainTextEdit* details_ = nullptr;
    QShortcut* escapeShortcut_ = nullptr;
};

class SmokeWaitingPresentation final : public pbapp::EncoderPresentation
{
public:
    [[nodiscard]] pbrenderd3d::DataWindowSnapshot GetSnapshot() const override
    {
        pbrenderd3d::DataWindowSnapshot snapshot;
        snapshot.state = stopped_ ? pbrenderd3d::WindowState::Stopped : pbrenderd3d::WindowState::Starting;
        return snapshot;
    }
    [[nodiscard]] pbrenderd3d::PresentationStatus SubmitFrame(const pbrenderd3d::CanonicalBgraFrameView&) override
    {
        return pbrenderd3d::PresentationStatus::Failure(pbrenderd3d::PresentationErrorCode::NotRunning, pbrenderd3d::PresentationStage::None);
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

} // namespace

int RunEncoderGui(const int argumentCount, wchar_t* arguments[])
{
    const bool measurement = argumentCount > 1 && std::wstring_view(arguments[1]) == L"--gui-measurement";
    std::unique_ptr<pbgui::Step1GuiEvidence> evidence;
    if (measurement)
    {
        if (argumentCount != 4 || std::wstring_view(arguments[2]) != L"--evidence-root")
        {
            return 2;
        }
        QString error;
        evidence = pbgui::Step1GuiEvidence::Create(QString::fromWCharArray(arguments[3]), QStringLiteral("Encoder"), error);
        if (!evidence)
        {
            std::cerr << error.toStdString() << '\n';
            return 2;
        }
    }
    const bool smoke = argumentCount == 2 && std::wstring_view(arguments[1]) == L"--gui-smoke";
    const bool nativeSmoke = pbgui::IsNativeSmoke(argumentCount, arguments);
    pbgui::NativeSmokeOptions nativeOptions;
    if (nativeSmoke && !pbgui::ParseNativeSmoke(argumentCount, arguments, nativeOptions))
    {
        return 2;
    }
    const bool integrationSmoke = argumentCount == 3 && std::wstring_view(arguments[1]) == L"--gui-integration-smoke";
    if (smoke && !pbgui::PrepareOffscreenPlatform())
    {
        std::cerr << "G22 Encoder GUI smoke: missing offscreen platform; no window started\n";
        return 2;
    }
    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    int guiArgumentCount = 1;
    char applicationName[] = "PixelBridgeEncoder";
    char* guiArguments[] = {applicationName, nullptr};
    QApplication application(guiArgumentCount, guiArguments);
    if (!pbgui::ConfigureFont(smoke))
    {
        std::cerr << "G22 Encoder GUI smoke: CJK font unavailable\n";
        return 2;
    }
    QCoreApplication::setOrganizationName(QStringLiteral("PixelBridge"));
    QCoreApplication::setApplicationName(QStringLiteral("PixelBridgeEncoder"));
    if (smoke)
    {
        QTemporaryDir scratch;
        if (!scratch.isValid())
        {
            return 2;
        }
        const QString sourcePath = scratch.filePath(QStringLiteral("empty.bin"));
        QFile source(sourcePath);
        if (!source.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        {
            return 2;
        }
        source.close();
        const QString settingsFile = scratch.filePath(QStringLiteral("settings.ini"));
        {
            QSettings legacy(settingsFile, QSettings::IniFormat);
            legacy.setValue(QStringLiteral("logicalVisualFps"), 240);
            legacy.setValue(QStringLiteral("visualProfile"), QStringLiteral("remote-lf4"));
            legacy.setValue(QStringLiteral("g22/logicalFps"), 240);
        }
        std::atomic<bool> created = false;
        bool targetConfigured = false;
        bool confirmDeletion = false;
        const pbapp::EncoderPresentationFactory presentationFactory = [&](const pbrenderd3d::DataWindowConfig& config) -> std::unique_ptr<pbapp::EncoderPresentation>
        {
            if (!config.repeatActiveFrame || config.width != 1920 || config.height != 1080 || config.topmost)
            {
                return nullptr;
            }
            created = true;
            return std::make_unique<SmokeWaitingPresentation>();
        };
        const TargetConfigurator configureTarget = [&](pbapp::EncoderConfig& config, QWidget&)
        {
            // Only the OS monitor selection is replaced. Runtime, preparation,
            // persistence and local shortcut wiring remain production code.
            targetConfigured = true;
            config.monitorClientOrigin = pbrenderd3d::PhysicalPoint{0, 0};
            return QString();
        };
        EncoderWindow window(presentationFactory, settingsFile, configureTarget, [&]()
        {
            return confirmDeletion;
        });
        const bool legacyPassed = window.RunSmoke(sourcePath, scratch.filePath(QStringLiteral("sessions")),
            [&]()
            {
                return created.load();
            }, confirmDeletion) && targetConfigured;
        created = false;
        QString evidenceError;
        const auto smokeEvidence = pbgui::Step1GuiEvidence::Create(scratch.filePath(QStringLiteral("measurement")), QStringLiteral("Encoder"), evidenceError);
        if (!smokeEvidence)
        {
            std::cerr << evidenceError.toStdString() << '\n';
            return 1;
        }
        EncoderWindow measuredWindow(presentationFactory, smokeEvidence->SettingsPath(), configureTarget, {}, smokeEvidence.get());
        const bool passed = measuredWindow.RunMeasurementStartSmoke(sourcePath, [&]()
        {
            return created.load();
        }) && legacyPassed;
        std::cout << "G22 Encoder GUI smoke: " << (passed ? "PASS" : "FAIL")
            << "; offscreen; tabs/real-cache-setting/fixed-run-FPS/local-Esc/retain/delete/measurement-Start-RunId; no desktop pixels\n";
        return passed ? 0 : 1;
    }
    if (nativeSmoke)
    {
        EncoderWindow window({}, QDir(nativeOptions.evidenceDirectory).filePath(QStringLiteral("settings.ini")));
        return window.RunNativeSmoke(nativeOptions);
    }
    EncoderWindow window({}, evidence ? evidence->SettingsPath() : QString(), ConfigureCurrentMonitor, {}, evidence.get());
    if (evidence)
    {
        window.setAttribute(Qt::WA_ShowWithoutActivating);
    }
    if (integrationSmoke && !pbgui::PlaceWithoutActivating(window, arguments[2]))
    {
        return 2;
    }
    window.show();
    return application.exec();
}
