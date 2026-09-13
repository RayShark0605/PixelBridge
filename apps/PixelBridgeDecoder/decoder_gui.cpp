#include "decoder_gui.h"
#include "decoder_application_controller.h"
#include "decoder_progress_text.h"
#include "product_gui_helpers.h"
#include "gui_native_smoke.h"
#include "run_report.h"
#include "step1_gui_evidence_qt.h"
#include "pbcore/build_info.h"

#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <array>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <string_view>

#ifdef PB_ENABLE_UNIFIED_GUI_SMOKE
#include "unified_decoder_test_support.h"
#endif

namespace
{

[[nodiscard]] QString StateText(const pbapp::DecoderSnapshot& snapshot)
{
    switch (snapshot.state)
    {
    case pbapp::DecoderState::Idle: return QStringLiteral("准备接收");
    case pbapp::DecoderState::WaitingForBootstrap: return QStringLiteral("等待有效数据画面…");
    case pbapp::DecoderState::AwaitingLargeOutputConfirmation: return QStringLiteral("接收策略不匹配，请停止后重新启动");
    case pbapp::DecoderState::ReceivingControl: return QStringLiteral("正在识别文件…");
    case pbapp::DecoderState::Receiving:
    case pbapp::DecoderState::Recovering:
        return snapshot.captureStallActive || snapshot.visualStallActive ? QStringLiteral("等待画面恢复 · 已保留进度") : QStringLiteral("正在恢复文件");
    case pbapp::DecoderState::Verifying: return QStringLiteral("正在校验文件…");
    case pbapp::DecoderState::Publishing: return QStringLiteral("正在安全保存文件…");
    case pbapp::DecoderState::Completed: return pbgui::IsVerifiedCompletion(snapshot) ? QStringLiteral("已完成") : QStringLiteral("完成状态尚未通过复验");
    case pbapp::DecoderState::Failed: return QStringLiteral("接收未能继续");
    case pbapp::DecoderState::Stopping: return QStringLiteral("正在安全停止…");
    case pbapp::DecoderState::Stopped: return QStringLiteral("已停止 · 断点已保留");
    }
    return QStringLiteral("等待状态");
}

struct WindowServices
{
    pbapp::DecoderRuntimeServices runtime;
    std::function<pbapp::MonitorCatalogStatus(std::vector<pbapp::MonitorInfo>&)> enumerateMonitors = pbapp::EnumerateMonitors;
    std::function<pbscreenregion::ScreenRegionStatus(const RECT&, pbscreenregion::ScreenCaptureRegion&)> selectRegion = pbscreenregion::SelectScreenCaptureRegionOnMonitor;
    std::function<pbscreenregion::ScreenRegionStatus(const RECT&, pbscreenregion::ScreenCaptureRegion&)> resolveRegion = pbscreenregion::ResolveScreenCaptureRegion;
    std::function<bool(const QString&)> openDirectory = [](const QString& directory)
    {
        return QDesktopServices::openUrl(QUrl::fromLocalFile(directory));
    };
};

class DecoderWindow final : public QMainWindow
{
public:
    explicit DecoderWindow(WindowServices services = {}, const QString& settingsPath = {}, pbgui::Step1GuiEvidence* evidence = nullptr) :
        services_(std::move(services)), controller_(nullptr, services_.runtime), evidence_(evidence)
    {
        settings_ = settingsPath.isEmpty() ? std::make_unique<QSettings>() : std::make_unique<QSettings>(settingsPath, QSettings::IniFormat);
        setWindowTitle(QStringLiteral("PixelBridge Decoder v%1").arg(QString::fromStdString(pbcore::GetBuildInfo().version)));
        setMinimumSize(680, 550);
        resize(780, 600);
        BuildUi();
        outputEdit_->setText(settings_->value(QStringLiteral("g22/outputDirectory"),
            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).toString());
        const int savedCarrier = settings_->value(QStringLiteral("g22/carrier"), 0).toInt();
        carrierCombo_->setCurrentIndex(savedCarrier == 1 ? 1 : 0);
        const int interval = settings_->value(QStringLiteral("g22/statusRefreshMilliseconds"), 250).toInt();
        refreshSpin_->setValue(interval >= 100 && interval <= 2000 ? interval : 250);
        static_cast<void>(controller_.SetStatusRefreshMilliseconds(refreshSpin_->value()));
        connect(&controller_, &DecoderApplicationController::SnapshotChanged, this, &DecoderWindow::UpdateSnapshot);
        connect(&controller_, &DecoderApplicationController::TerminalStateReached, this, [this]()
        {
            if (closePending_)
            {
                close();
            }
        });
        ReloadMonitors();
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

    ~DecoderWindow() override
    {
        if (evidence_)
        {
            evidence_->Observe(controller_.StopAndGetSnapshot());
        }
    }

#ifdef PB_ENABLE_UNIFIED_GUI_SMOKE
    [[nodiscard]] bool RunSmoke(const QString& outputDirectory, const std::shared_ptr<g16test::ReceiveState>& state,
        const std::vector<pbdemodd3d11::CaptureDemodulatorResult>& frames, const std::span<const std::byte> expected,
        int& selectorCalls, int& openCalls)
    {
        if (isVisible() || selectorCalls != 0 || tabs_->count() != 2 || !details_->isReadOnly() || startButton_->isEnabled() ||
            monitorCombo_->currentIndex() != 0 || roiButton_->isEnabled() || openButton_->isEnabled() ||
            controller_.GetStatusRefreshMilliseconds() != 250)
        {
            return false;
        }
        outputEdit_->setText(outputDirectory);
        monitorCombo_->setCurrentIndex(1);
        wholeButton_->click();
        if (!hasRegion_ || !EqualRect(&region_.physicalRect, &monitors_[0].physicalRect) || selectorCalls != 0)
        {
            return false;
        }
        roiButton_->click();
        if (selectorCalls != 1 || !startButton_->isEnabled())
        {
            return false;
        }
        refreshSpin_->setValue(100);
        if (controller_.GetStatusRefreshMilliseconds() != 100 ||
            controller_.SetStatusRefreshMilliseconds(99) || controller_.SetStatusRefreshMilliseconds(2001) ||
            !pbgui::SaveTabPreviews(*this, *tabs_, QStringLiteral("decoder-idle")))
        {
            return false;
        }
        state->Push(frames[0]);
        startButton_->click();
        if (!WaitFor([&]()
        {
            return controller_.GetSnapshot().verifiedSegmentCount == 1;
        }))
        {
            std::cerr << controller_.GetSnapshot().errorDetail << '\n';
            return false;
        }
        UpdateSnapshot();
        if (controller_.GetSnapshot().largeOutputConfirmationState != pbapp::LargeOutputConfirmationState::NotRequired ||
            controller_.GetSnapshot().largeOutputConfirmationRequestId != 0 || outputEdit_->isEnabled() || roiButton_->isEnabled() || monitorCombo_->isEnabled() ||
            coordinateApply_->isEnabled() || startButton_->text() != QStringLiteral("停止接收") || openCalls != 0 ||
            progressLabel_->text() == QStringLiteral("100%"))
        {
            return false;
        }
        startButton_->click();
        if (!WaitFor([&]()
        {
            return controller_.GetSnapshot().state == pbapp::DecoderState::Stopped;
        }))
        {
            return false;
        }
        const auto stopped = controller_.GetSnapshot();
        const auto resumePath = std::filesystem::path(std::u8string(stopped.resumeStatePath.begin(), stopped.resumeStatePath.end()));
        if (stopped.verifiedSegmentCount != 1 || !std::filesystem::exists(resumePath))
        {
            return false;
        }
        UpdateSnapshot();
        state->Push(frames[1]);
        startButton_->click();
        if (!WaitFor([&]()
        {
            return controller_.GetSnapshot().state == pbapp::DecoderState::Completed;
        }))
        {
            std::cerr << controller_.GetSnapshot().errorDetail << '\n';
            return false;
        }
        UpdateSnapshot();
        if (controller_.GetSnapshot().largeOutputConfirmationState != pbapp::LargeOutputConfirmationState::NotRequired ||
            controller_.GetSnapshot().largeOutputConfirmationRequestId != 0 || !g16test::VerifyOutput(controller_.GetSnapshot(), expected) ||
            progressLabel_->text() != QStringLiteral("100%") || etaLabel_->text() != QStringLiteral("00:00:00") ||
            !openButton_->isEnabled() || openCalls != 0 || !pbgui::SaveTabPreviews(*this, *tabs_, QStringLiteral("decoder-completed")))
        {
            return false;
        }
        openButton_->click();
        if (openCalls != 1)
        {
            return false;
        }
        // Selecting an out-of-scope physical rectangle must disable start,
        // rather than leave an earlier valid selection deceptively active.
        coordinates_[0]->setValue(-1);
        coordinateApply_->click();
        return selectorCalls == 1 && !QApplication::activeModalWidget() &&
            !startButton_->isEnabled() && !hasRegion_;
    }
    [[nodiscard]] bool RunMeasurementStartSmoke(const std::shared_ptr<g16test::ReceiveState>& state)
    {
        if (!evidence_ || isVisible())
        {
            return false;
        }
        outputEdit_->setText(QFileInfo(evidence_->SettingsPath()).absolutePath());
        monitorCombo_->setCurrentIndex(1);
        wholeButton_->click();
        startButton_->click();
        if (!WaitFor([&]()
        {
            const std::scoped_lock lock(state->mutex);
            return state->sessionStarts != 0;
        }))
        {
            return false;
        }
        const auto started = controller_.GetSnapshot();
        const bool accepted = started.runId == evidence_->RunId().toStdString() && started.runId.size() == 32 &&
            started.measurement && started.measurement->runGeneration != 0 &&
            !QFileInfo::exists(QDir(evidence_->RunDirectory()).filePath(QStringLiteral("start-rejected.json")));
        const auto stopped = controller_.StopAndGetSnapshot();
        evidence_->Observe(stopped);
        return accepted && stopped.state == pbapp::DecoderState::Stopped && stopped.captureDeliveredFrames == 0;
    }
#endif

    [[nodiscard]] int RunNativeSmoke(const pbgui::NativeSmokeOptions& options)
    {
        if (!QFileInfo(options.inputPath).isDir() ||
            !QDir(options.inputPath).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System).isEmpty() ||
            !pbgui::ShowNativeSmoke(*this, options))
        {
            return 2;
        }
        outputEdit_->setText(options.inputPath);
        const auto selected = std::find_if(monitors_.begin(), monitors_.end(), [&](const pbapp::MonitorInfo& monitor)
        {
            return pbapp::SameMonitorIdentity(monitor, options.safety.experimentMonitor);
        });
        if (selected == monitors_.end())
        {
            return 2;
        }
        monitorCombo_->setCurrentIndex(static_cast<int>(std::distance(monitors_.begin(), selected)) + 1);
        wholeButton_->click();
        startButton_->click();
        const bool lockedAtStart = controller_.IsActive() && !outputEdit_->isEnabled() && !monitorCombo_->isEnabled();
        bool safetyHeld = true;
        QJsonObject safetyFailure;
        bool captureReadyWritten = false;
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
            if (!captureReadyWritten && controller_.GetSnapshot().captureArrivedFrames > 0)
            {
                captureReadyWritten = pbgui::WriteNativeSmokeFile(options, QStringLiteral("capture-ready.json"), "{\"captureStarted\":true}\n");
                if (!captureReadyWritten)
                {
                    break;
                }
            }
            QThread::msleep(20);
        }
        const bool automaticCompletion = pbgui::IsVerifiedCompletion(controller_.GetSnapshot()) && !controller_.IsActive();
        controller_.RequestStop();
        const bool stopped = WaitFor([this]()
        {
            return !controller_.IsActive();
        });
        UpdateSnapshot();
        const auto snapshot = controller_.GetSnapshot();
        const bool completedText = progressLabel_->text() == QStringLiteral("100%") && etaLabel_->text() == QStringLiteral("00:00:00");
        const bool passed = safetyHeld && lockedAtStart && captureReadyWritten && automaticCompletion && stopped && completedText &&
            !QApplication::activeModalWidget() && snapshot.captureArrivedFrames > 0 && snapshot.verifiedRawBytes == 1024 * 1024 &&
            snapshot.largeOutputConfirmationState == pbapp::LargeOutputConfirmationState::NotRequired;
        const pbapp::RunReportContext context{"PixelBridgeDecoder", std::string(pbcore::GetBuildInfo().version), PB_GIT_COMMIT,
            QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString()};
        const QJsonObject checks{{"controlsLockedAtStart", lockedAtStart}, {"captureReady", captureReadyWritten},
            {"safetyFailure", safetyFailure},
            {"automaticVerifiedCompletion", automaticCompletion}, {"completedText", completedText},
            {"captureArrivedFrames", static_cast<qint64>(snapshot.captureArrivedFrames)},
            {"wholeMonitorSelected", EqualRect(&region_.physicalRect, &options.safety.experimentMonitor.physicalRect) != FALSE},
            {"physicalRoiDragTested", false}, {"progress", progressLabel_->text()}, {"speed", speedLabel_->text()}, {"eta", etaLabel_->text()}};
        const bool saved = pbgui::WriteNativeSmokeResult(options, QStringLiteral("Decoder"), passed, safetyHeld, checks,
            pbapp::BuildDecoderRunReportJson(context, snapshot));
        hide();
        return passed && saved ? 0 : 1;
    }

protected:
    void closeEvent(QCloseEvent* event) override
    {
        if (selectingRoi_)
        {
            closePending_ = true;
            event->ignore();
            return;
        }
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
        while (!predicate() && timer.elapsed() < 4000)
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
        QLabel* const heading = pbgui::TextLabel(QStringLiteral("文件接收"));
        heading->setStyleSheet(QStringLiteral("font-size:24px;font-weight:600;"));
        outer->addWidget(heading);
        tabs_ = new QTabWidget();
        tabs_->setObjectName(QStringLiteral("decoderTabs"));
        QWidget* const mainPage = new QWidget();
        QVBoxLayout* const mainLayout = new QVBoxLayout(mainPage);
        mainLayout->setContentsMargins(20, 20, 20, 20);
        mainLayout->setSpacing(14);
        mainLayout->addWidget(pbgui::TextLabel(QStringLiteral("接收模式")));
        carrierCombo_ = new QComboBox();
        carrierCombo_->setObjectName(QStringLiteral("carrierMode"));
        carrierCombo_->addItem(QStringLiteral("标准（PB-Unified-SC6-V3）"));
        carrierCombo_->addItem(QStringLiteral("灰阶高速 v4（实验，远控链路推荐）"));
        carrierCombo_->setToolTip(QStringLiteral("必须与编码端选择同一模式，否则无法建立会话。"));
        mainLayout->addWidget(carrierCombo_);
        mainLayout->addWidget(pbgui::TextLabel(QStringLiteral("文件保存目录")));
        QHBoxLayout* const outputRow = new QHBoxLayout();
        outputEdit_ = new QLineEdit();
        outputEdit_->setObjectName(QStringLiteral("outputDirectory"));
        browseButton_ = new QPushButton(QStringLiteral("选择目录…"));
        outputRow->addWidget(outputEdit_, 1);
        outputRow->addWidget(browseButton_);
        mainLayout->addLayout(outputRow);
        mainLayout->addWidget(pbgui::TextLabel(QStringLiteral("接收屏幕与区域")));
        QHBoxLayout* const monitorRow = new QHBoxLayout();
        monitorCombo_ = new QComboBox();
        monitorCombo_->setObjectName(QStringLiteral("captureMonitor"));
        monitorCombo_->setMinimumContentsLength(20);
        monitorCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        wholeButton_ = new QPushButton(QStringLiteral("整屏"));
        roiButton_ = new QPushButton(QStringLiteral("框选区域…"));
        roiButton_->setObjectName(QStringLiteral("selectRoi"));
        monitorRow->addWidget(monitorCombo_, 1);
        monitorRow->addWidget(wholeButton_);
        monitorRow->addWidget(roiButton_);
        mainLayout->addLayout(monitorRow);
        roiLabel_ = pbgui::TextLabel();
        mainLayout->addWidget(roiLabel_);
        QHBoxLayout* const actionRow = new QHBoxLayout();
        stateLabel_ = pbgui::TextLabel();
        stateLabel_->setStyleSheet(QStringLiteral("font-size:16px;font-weight:600;"));
        actionRow->addWidget(stateLabel_, 1);
        startButton_ = new QPushButton(QStringLiteral("开始接收"));
        startButton_->setObjectName(QStringLiteral("startReceive"));
        startButton_->setMinimumSize(136, 38);
        actionRow->addWidget(startButton_);
        mainLayout->addLayout(actionRow);
        QFormLayout* const progress = new QFormLayout();
        progress->setVerticalSpacing(12);
        progressLabel_ = pbgui::TextLabel();
        progressLabel_->setObjectName(QStringLiteral("recoveryPercent"));
        speedLabel_ = pbgui::TextLabel();
        speedLabel_->setToolTip(QStringLiteral("已通过 Segment 校验的原始字节恢复速度；按 Windows 单位换算，1 KB = 1024 B，不是屏幕帧率或收包速率。"));
        etaLabel_ = pbgui::TextLabel();
        progress->addRow(QStringLiteral("当前进度"), progressLabel_);
        progress->addRow(QStringLiteral("恢复速度"), speedLabel_);
        progress->addRow(QStringLiteral("剩余时间"), etaLabel_);
        mainLayout->addLayout(progress);
        messageLabel_ = pbgui::TextLabel();
        mainLayout->addWidget(messageLabel_);
        mainLayout->addStretch();
        mainLayout->addWidget(pbgui::TextLabel(QStringLiteral("暂时没有有效画面时会持续等待；停止接收后保留断点。")));
        tabs_->addTab(mainPage, QStringLiteral("接收"));

        QWidget* const advancedPage = new QWidget();
        QVBoxLayout* const advancedLayout = new QVBoxLayout(advancedPage);
        advancedLayout->setContentsMargins(20, 20, 20, 20);
        advancedLayout->setSpacing(12);
        QHBoxLayout* const refreshRow = new QHBoxLayout();
        refreshRow->addWidget(pbgui::TextLabel(QStringLiteral("界面状态刷新间隔")));
        refreshSpin_ = new QSpinBox();
        refreshSpin_->setRange(100, 2000);
        refreshSpin_->setSingleStep(50);
        refreshSpin_->setValue(250);
        refreshSpin_->setSuffix(QStringLiteral(" ms"));
        refreshSpin_->setToolTip(QStringLiteral("只影响界面更新，不限速捕获、解码或数据恢复。"));
        refreshRow->addWidget(refreshSpin_);
        refreshRow->addStretch();
        reloadButton_ = new QPushButton(QStringLiteral("刷新显示器列表"));
        refreshRow->addWidget(reloadButton_);
        advancedLayout->addLayout(refreshRow);
        advancedLayout->addWidget(pbgui::TextLabel(QStringLiteral("精确 ROI（桌面物理像素，右/下边界不包含；必须位于所选屏幕内）")));
        QHBoxLayout* const coordinateRow = new QHBoxLayout();
        const std::array<QString, 4> names{QStringLiteral("左"), QStringLiteral("上"), QStringLiteral("右"), QStringLiteral("下")};
        for (std::size_t index = 0; index < coordinates_.size(); index++)
        {
            coordinateRow->addWidget(pbgui::TextLabel(names[index]));
            coordinates_[index] = new QSpinBox();
            coordinates_[index]->setRange((std::numeric_limits<int>::min)(), (std::numeric_limits<int>::max)());
            coordinateRow->addWidget(coordinates_[index], 1);
        }
        coordinateApply_ = new QPushButton(QStringLiteral("应用"));
        coordinateRow->addWidget(coordinateApply_);
        advancedLayout->addLayout(coordinateRow);
        QHBoxLayout* const toolsRow = new QHBoxLayout();
        openButton_ = new QPushButton(QStringLiteral("打开完成文件所在目录"));
        exportButton_ = new QPushButton(QStringLiteral("导出诊断报告…"));
        toolsRow->addWidget(openButton_);
        toolsRow->addWidget(exportButton_);
        toolsRow->addStretch();
        advancedLayout->addLayout(toolsRow);
        advancedMessage_ = pbgui::TextLabel();
        advancedLayout->addWidget(advancedMessage_);
        details_ = new QPlainTextEdit();
        details_->setObjectName(QStringLiteral("decoderDiagnostics"));
        details_->setReadOnly(true);
        advancedLayout->addWidget(details_, 1);
        tabs_->addTab(advancedPage, QStringLiteral("高级选项"));
        outer->addWidget(tabs_, 1);
        setCentralWidget(central);

        connect(outputEdit_, &QLineEdit::textChanged, this, &DecoderWindow::UpdateActions);
        connect(monitorCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]()
        {
            hasRegion_ = false;
            if (const auto* monitor = SelectedMonitor())
            {
                SetCoordinateFields(monitor->physicalRect);
            }
            UpdateActions();
        });
        connect(browseButton_, &QPushButton::clicked, this, [this]()
        {
            const QString directory = QFileDialog::getExistingDirectory(this, QStringLiteral("选择文件保存目录"), outputEdit_->text());
            if (!directory.isEmpty())
            {
                outputEdit_->setText(QDir::toNativeSeparators(directory));
            }
        });
        connect(reloadButton_, &QPushButton::clicked, this, &DecoderWindow::ReloadMonitors);
        connect(wholeButton_, &QPushButton::clicked, this, [this]()
        {
            if (const auto* monitor = SelectedMonitor())
            {
                ApplyPhysicalRect(monitor->physicalRect);
            }
        });
        connect(roiButton_, &QPushButton::clicked, this, &DecoderWindow::SelectRoi);
        connect(coordinateApply_, &QPushButton::clicked, this, [this]()
        {
            ApplyPhysicalRect(RECT{coordinates_[0]->value(), coordinates_[1]->value(), coordinates_[2]->value(), coordinates_[3]->value()});
        });
        connect(startButton_, &QPushButton::clicked, this, &DecoderWindow::StartOrStop);
        connect(refreshSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](const int milliseconds)
        {
            static_cast<void>(controller_.SetStatusRefreshMilliseconds(milliseconds));
        });
        connect(openButton_, &QPushButton::clicked, this, [this]()
        {
            const auto snapshot = controller_.GetSnapshot();
            if (pbgui::IsVerifiedCompletion(snapshot) && !services_.openDirectory(QFileInfo(pbgui::FromUtf8(snapshot.outputPath)).absolutePath()))
            {
                SetMessage(QStringLiteral("无法打开目录，请使用高级详情中的完整路径。"));
            }
        });
        connect(exportButton_, &QPushButton::clicked, this, &DecoderWindow::ExportReport);
    }

    [[nodiscard]] const pbapp::MonitorInfo* SelectedMonitor() const
    {
        const int index = monitorCombo_->currentIndex() - 1;
        return index >= 0 && static_cast<std::size_t>(index) < monitors_.size() ? &monitors_[static_cast<std::size_t>(index)] : nullptr;
    }

    void SetMessage(const QString& message)
    {
        messageLabel_->setText(message);
        advancedMessage_->setText(message);
    }

    void ReloadMonitors()
    {
        if (controller_.IsActive())
        {
            return;
        }
        std::vector<pbapp::MonitorInfo> current;
        const bool available = static_cast<bool>(services_.enumerateMonitors(current));
        monitorCombo_->clear();
        monitors_.clear();
        hasRegion_ = false;
        monitorCombo_->addItem(QStringLiteral("请选择显示器…"));
        if (!available)
        {
            SetMessage(QStringLiteral("无法读取显示器列表，请检查显示器连接后重试。"));
            UpdateActions();
            return;
        }
        monitors_ = std::move(current);
        for (std::size_t index = 0; index < monitors_.size(); index++)
        {
            const auto& monitor = monitors_[index];
            monitorCombo_->addItem(QStringLiteral("%1 · %2 × %3%4").arg(QString::fromStdWString(monitor.deviceName))
                .arg(static_cast<qint64>(monitor.physicalRect.right) - monitor.physicalRect.left)
                .arg(static_cast<qint64>(monitor.physicalRect.bottom) - monitor.physicalRect.top)
                .arg(monitor.primary ? QStringLiteral(" · 主屏幕") : QString()));
        }
        monitorCombo_->setCurrentIndex(0);
        UpdateActions();
    }

    void SetCoordinateFields(const RECT& rectangle)
    {
        const std::array<LONG, 4> values{rectangle.left, rectangle.top, rectangle.right, rectangle.bottom};
        for (std::size_t index = 0; index < values.size(); index++)
        {
            coordinates_[index]->setValue(values[index]);
        }
    }

    [[nodiscard]] bool IsWithinSelectedMonitor(const pbscreenregion::ScreenCaptureRegion& region) const
    {
        const auto* monitor = SelectedMonitor();
        return monitor != nullptr && region.monitor == monitor->monitor && EqualRect(&region.monitorPhysicalRect, &monitor->physicalRect) &&
            pbapp::RectContains(monitor->physicalRect, region.physicalRect);
    }

    void ApplyPhysicalRect(const RECT& rectangle)
    {
        if (controller_.IsActive())
        {
            return;
        }
        const auto* monitor = SelectedMonitor();
        pbscreenregion::ScreenCaptureRegion selected;
        hasRegion_ = false;
        if (monitor == nullptr || !pbapp::RectContains(monitor->physicalRect, rectangle) ||
            !services_.resolveRegion(rectangle, selected) || !IsWithinSelectedMonitor(selected))
        {
            SetMessage(QStringLiteral("ROI 无效或越过所选屏幕，请重新选择。"));
        }
        else
        {
            region_ = selected;
            hasRegion_ = true;
            SetCoordinateFields(region_.physicalRect);
            SetMessage({});
        }
        UpdateActions();
    }

    void SelectRoi()
    {
        const auto* monitor = SelectedMonitor();
        if (controller_.IsActive() || monitor == nullptr || selectingRoi_)
        {
            return;
        }
        pbscreenregion::ScreenCaptureRegion selected;
        const RECT scope = monitor->physicalRect;
        selectingRoi_ = true;
        UpdateActions();
        const auto status = services_.selectRegion(scope, selected);
        selectingRoi_ = false;
        if (status && IsWithinSelectedMonitor(selected))
        {
            region_ = selected;
            hasRegion_ = true;
            SetCoordinateFields(region_.physicalRect);
            SetMessage({});
        }
        else if (status.code != pbscreenregion::ScreenRegionErrorCode::Cancelled)
        {
            hasRegion_ = false;
            SetMessage(QStringLiteral("框选失败或显示器已变化，请重新选择。"));
        }
        UpdateActions();
        if (closePending_)
        {
            close();
        }
    }

    void SavePreferences()
    {
        settings_->setValue(QStringLiteral("g22/outputDirectory"), outputEdit_->text());
        settings_->setValue(QStringLiteral("g22/carrier"), carrierCombo_->currentIndex());
        settings_->setValue(QStringLiteral("g22/statusRefreshMilliseconds"), refreshSpin_->value());
        settings_->sync();
    }

    void StartOrStop()
    {
        if (controller_.IsActive())
        {
            controller_.RequestStop();
            return;
        }
        const auto* expected = SelectedMonitor();
        if (!hasRegion_ || expected == nullptr || closePending_)
        {
            return;
        }
        std::vector<pbapp::MonitorInfo> currentMonitors;
        const bool enumerated = static_cast<bool>(services_.enumerateMonitors(currentMonitors));
        const auto current = std::find_if(currentMonitors.begin(), currentMonitors.end(), [expected](const pbapp::MonitorInfo& monitor)
        {
            return pbapp::SameMonitorIdentity(*expected, monitor);
        });
        pbscreenregion::ScreenCaptureRegion region;
        if (!enumerated || current == currentMonitors.end() || !services_.resolveRegion(region_.physicalRect, region) || !IsWithinSelectedMonitor(region))
        {
            hasRegion_ = false;
            SetMessage(QStringLiteral("显示器布局或 ROI 已变化，请刷新显示器列表并重新选择。"));
            UpdateActions();
            return;
        }
        region_ = region;
        auto config = pbapp::MakeUnifiedDecoderConfig(outputEdit_->text().toStdWString(), region_);
        config.visualProfile = carrierCombo_->currentIndex() == 1 ?
            pbapp::VisualProfile::UnifiedGrayFast : pbapp::VisualProfile::UnifiedLc4;
        if (evidence_)
        {
            QString evidenceError;
            config.measurement = evidence_->BeginRun(evidenceError);
            if (!config.measurement)
            {
                SetMessage(evidenceError);
                return;
            }
            config.runId = evidence_->RunId().toStdString();
            const auto outputDirectory = QDir(evidence_->RunDirectory()).filePath(QStringLiteral("output"));
            config.outputDirectory = outputDirectory.toStdWString();
            outputEdit_->setText(outputDirectory);
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
            SetMessage(error);
        }
    }

    void ExportReport()
    {
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出 Decoder 诊断报告"), QStringLiteral("decoder-report.json"), QStringLiteral("JSON (*.json)"));
        if (path.isEmpty())
        {
            return;
        }
        const pbapp::RunReportContext context{"PixelBridgeDecoder", std::string(pbcore::GetBuildInfo().version), PB_GIT_COMMIT,
            QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString()};
        const std::string report = pbapp::BuildDecoderRunReportJson(context, controller_.GetSnapshot());
        QFile output(path);
        if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
            output.write(report.data(), static_cast<qint64>(report.size())) != static_cast<qint64>(report.size()) || !output.flush())
        {
            SetMessage(QStringLiteral("报告未保存：请选择一个尚不存在、可写的文件路径。"));
        }
    }

    void UpdateActions()
    {
        const auto snapshot = controller_.GetSnapshot();
        const bool active = controller_.IsActive();
        const bool idle = !active && !closePending_ && !selectingRoi_;
        const bool selected = SelectedMonitor() != nullptr;
        outputEdit_->setEnabled(idle);
        carrierCombo_->setEnabled(idle);
        browseButton_->setEnabled(idle);
        monitorCombo_->setEnabled(idle);
        reloadButton_->setEnabled(idle);
        wholeButton_->setEnabled(idle && selected);
        roiButton_->setEnabled(idle && selected);
        coordinateApply_->setEnabled(idle && selected);
        for (auto* field : coordinates_)
        {
            field->setEnabled(idle && selected);
        }
        startButton_->setText(active ? QStringLiteral("停止接收") : QStringLiteral("开始接收"));
        startButton_->setEnabled(!closePending_ && !selectingRoi_ && snapshot.state != pbapp::DecoderState::Stopping &&
            (active || (hasRegion_ && QFileInfo(outputEdit_->text()).isDir())));
        openButton_->setEnabled(pbgui::IsVerifiedCompletion(snapshot) && !closePending_);
        exportButton_->setEnabled(idle && snapshot.runGeneration != 0);
        roiLabel_->setText(hasRegion_ ? QStringLiteral("ROI：%1 × %2 像素 · 左上角 (%3, %4)")
            .arg(static_cast<qint64>(region_.physicalRect.right) - region_.physicalRect.left)
            .arg(static_cast<qint64>(region_.physicalRect.bottom) - region_.physicalRect.top)
            .arg(region_.physicalRect.left).arg(region_.physicalRect.top) : QStringLiteral("请先选择显示器，再指定整屏或框选区域。"));
    }

    void UpdateSnapshot()
    {
        const auto snapshot = controller_.GetSnapshot();
        stateLabel_->setText(StateText(snapshot));
        const auto progress = pbgui::FormatDecoderProgress(snapshot);
        progressLabel_->setText(progress.percent);
        speedLabel_->setText(progress.speed);
        etaLabel_->setText(progress.remaining);
        if (snapshot.state == pbapp::DecoderState::Failed)
        {
            SetMessage(pbgui::FromUtf8(snapshot.errorDetail.empty() ? snapshot.statusMessage : snapshot.errorDetail));
        }
        else if (pbgui::IsVerifiedCompletion(snapshot))
        {
            SetMessage(QStringLiteral("已保存：%1").arg(QFileInfo(pbgui::FromUtf8(snapshot.outputPath)).fileName()));
        }
        else
        {
            SetMessage({});
        }
        details_->setPlainText(QStringLiteral("PB-Unified-SC6-V3 · layout 10\n"
            "捕获：Auto，WGC 优先；只在明确的后端故障时切换 DXGI\n"
            "资源上限：500 GB；所有支持的文件大小均无需再次确认\n"
            "进度仅统计已验证原始字节，100% 还需要整文件摘要、安全发布和最终重新打开复验\n"
            "1 KB = 1024 B；样本不足或画面停滞时不猜测剩余时间\n\n") +
            (snapshot.runGeneration == 0 ? QStringLiteral("尚未开始接收。捕获及恢复详情将在启动后显示。") :
                pbgui::FromUtf8(pbapp::BuildDecoderDiagnostics(snapshot))));
        UpdateActions();

    }

    WindowServices services_;
    DecoderApplicationController controller_;
    std::unique_ptr<QSettings> settings_;
    pbgui::Step1GuiEvidence* evidence_ = nullptr;
    std::vector<pbapp::MonitorInfo> monitors_;
    pbscreenregion::ScreenCaptureRegion region_;
    bool hasRegion_ = false;
    bool selectingRoi_ = false;
    bool closePending_ = false;
    QTabWidget* tabs_ = nullptr;
    QLineEdit* outputEdit_ = nullptr;
    QComboBox* carrierCombo_ = nullptr;
    QComboBox* monitorCombo_ = nullptr;
    QPushButton* browseButton_ = nullptr;
    QPushButton* wholeButton_ = nullptr;
    QPushButton* roiButton_ = nullptr;
    QPushButton* reloadButton_ = nullptr;
    QPushButton* startButton_ = nullptr;
    QPushButton* coordinateApply_ = nullptr;
    QPushButton* openButton_ = nullptr;
    QPushButton* exportButton_ = nullptr;
    QSpinBox* refreshSpin_ = nullptr;
    std::array<QSpinBox*, 4> coordinates_{};
    QLabel* roiLabel_ = nullptr;
    QLabel* stateLabel_ = nullptr;
    QLabel* progressLabel_ = nullptr;
    QLabel* speedLabel_ = nullptr;
    QLabel* etaLabel_ = nullptr;
    QLabel* messageLabel_ = nullptr;
    QLabel* advancedMessage_ = nullptr;
    QPlainTextEdit* details_ = nullptr;
};

} // namespace

int RunDecoderGui(const int argumentCount, wchar_t* arguments[])
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
        evidence = pbgui::Step1GuiEvidence::Create(QString::fromWCharArray(arguments[3]), QStringLiteral("Decoder"), error);
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
    if (smoke && !pbgui::PrepareOffscreenPlatform())
    {
        std::cerr << "G22 Decoder GUI smoke: missing offscreen platform; no window started\n";
        return 2;
    }
    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    int count = 1;
    char name[] = "PixelBridgeDecoder";
    char* args[] = {name, nullptr};
    QApplication application(count, args);
    if (!pbgui::ConfigureFont(smoke))
    {
        return 2;
    }
    QCoreApplication::setOrganizationName(QStringLiteral("PixelBridge"));
    QCoreApplication::setApplicationName(QStringLiteral("PixelBridgeDecoder"));
    if (smoke)
    {
#ifndef PB_ENABLE_UNIFIED_GUI_SMOKE
        std::cerr << "G22 Decoder GUI smoke was not enabled in this build\n";
        return 2;
#else
        try
        {
            QTemporaryDir scratch;
            g16test::Check(scratch.isValid(), "GUI scratch unavailable");
            const auto root = std::filesystem::path(scratch.path().toStdWString());
            const std::vector<std::byte> bytes(8ULL * 1024 * 1024 + 1, std::byte{0x31});
            const auto frames = g16test::MakeFrames(root / L"tx", bytes, 2);
            const QString outputDirectory = scratch.filePath(QStringLiteral("out"));
            g16test::Check(QDir().mkdir(outputDirectory), "GUI output scratch unavailable");
            const auto state = std::make_shared<g16test::ReceiveState>();
            WindowServices services;
            services.runtime = g16test::Services(state);
            int selectorCalls = 0;
            int openCalls = 0;
            services.enumerateMonitors = [](std::vector<pbapp::MonitorInfo>& monitors)
            {
                pbapp::MonitorInfo monitor;
                const auto region = g16test::Region();
                monitor.monitor = region.monitor;
                monitor.deviceName = L"GUI smoke monitor";
                monitor.physicalRect = region.monitorPhysicalRect;
                monitor.workRect = region.monitorPhysicalRect;
                monitor.dpiX = region.dpiX;
                monitor.dpiY = region.dpiY;
                monitor.rotation = region.rotation;
                monitors = {monitor};
                return pbapp::MonitorCatalogStatus{};
            };
            services.selectRegion = [&](const RECT& scope, pbscreenregion::ScreenCaptureRegion& region)
            {
                selectorCalls++;
                region = g16test::Region();
                g16test::Check(EqualRect(&scope, &region.monitorPhysicalRect) != FALSE, "Selector scope mismatch");
                return pbscreenregion::ScreenRegionStatus{};
            };
            services.resolveRegion = [](const RECT& rectangle, pbscreenregion::ScreenCaptureRegion& region)
            {
                region = g16test::Region();
                region.physicalRect = rectangle;
                return pbscreenregion::ScreenRegionStatus{};
            };
            services.openDirectory = [&](const QString& directory)
            {
                openCalls++;
                return QFileInfo(directory).isDir();
            };
            const QString settingsPath = scratch.filePath(QStringLiteral("settings.ini"));
            {
                QSettings oldSettings(settingsPath, QSettings::IniFormat);
                oldSettings.setValue(QStringLiteral("visualProfile"), QStringLiteral("remote-lf4"));
                oldSettings.setValue(QStringLiteral("captureBackend"), QStringLiteral("dxgi"));
                oldSettings.setValue(QStringLiteral("g22/statusRefreshMilliseconds"), 0);
            }
            DecoderWindow window(services, settingsPath);
            const bool legacyPassed = window.RunSmoke(outputDirectory, state, frames, bytes, selectorCalls, openCalls);
            QString evidenceError;
            const auto smokeEvidence = pbgui::Step1GuiEvidence::Create(scratch.filePath(QStringLiteral("measurement")), QStringLiteral("Decoder"), evidenceError);
            g16test::Check(static_cast<bool>(smokeEvidence), evidenceError.toStdString().c_str());
            const auto measuredState = std::make_shared<g16test::ReceiveState>();
            services.runtime = g16test::Services(measuredState);
            DecoderWindow measuredWindow(std::move(services), smokeEvidence->SettingsPath(), smokeEvidence.get());
            const bool passed = measuredWindow.RunMeasurementStartSmoke(measuredState) && legacyPassed;
            std::cout << "G22 Decoder GUI smoke: " << (passed ? "PASS" : "FAIL")
                << "; offscreen; real runtime/storage; tabs/monitor/scoped-ROI/status-interval/stop/resume/verified-completion/no-auto-open/no-size-confirmation/measurement-Start-RunId\n";
            return passed ? 0 : 1;
        }
        catch (const std::exception& exception)
        {
            std::cerr << "G22 Decoder GUI smoke failed: " << exception.what() << '\n';
            return 1;
        }
#endif
    }
    if (nativeSmoke)
    {
        DecoderWindow window({}, QDir(nativeOptions.evidenceDirectory).filePath(QStringLiteral("settings.ini")));
        return window.RunNativeSmoke(nativeOptions);
    }
    DecoderWindow window({}, evidence ? evidence->SettingsPath() : QString(), evidence.get());
    if (evidence)
    {
        window.setAttribute(Qt::WA_ShowWithoutActivating);
    }
    window.show();
    return application.exec();
}
