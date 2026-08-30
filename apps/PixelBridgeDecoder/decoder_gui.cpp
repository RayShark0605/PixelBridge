#include "decoder_gui.h"

#include "decoder_application_controller.h"

#include "run_report.h"
#include "pbcore/build_info.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QScreen>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>
#include <cstdint>
#include <algorithm>
#include <string>
#include <string_view>

namespace
{

[[nodiscard]] QString FromUtf8(const std::string& value)
{
    return QString::fromUtf8(value.data(), static_cast<int>(value.size()));
}

[[nodiscard]] QString HumanBytes(const std::uint64_t bytes)
{
    static const QStringList units{QStringLiteral("B"), QStringLiteral("KiB"), QStringLiteral("MiB"),
        QStringLiteral("GiB")};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit + 1 < units.size())
    {
        value /= 1024.0;
        unit++;
    }
    return QStringLiteral("%1 %2").arg(value, 0, 'f', unit == 0 ? 0 : 2).arg(units[unit]);
}

[[nodiscard]] QString RateText(const double bytesPerSecond)
{
    if (!(bytesPerSecond > 0) || !std::isfinite(bytesPerSecond))
    {
        return QStringLiteral("等待有效数据");
    }
    return QStringLiteral("%1 MiB/s · %2 Mbps")
        .arg(bytesPerSecond / (1024.0 * 1024.0), 0, 'f', 2)
        .arg(bytesPerSecond * 8.0 / 1000000.0, 0, 'f', 2);
}

[[nodiscard]] QString BitRateText(const std::optional<double>& bitsPerSecond)
{
    if (!bitsPerSecond || !(*bitsPerSecond > 0) || !std::isfinite(*bitsPerSecond))
    {
        return QStringLiteral("等待 WholeFileDigest / final publish 权威样本");
    }
    return QStringLiteral("%1 MiB/s · %2 Mbps")
        .arg(*bitsPerSecond / 8.0 / (1024.0 * 1024.0), 0, 'f', 2)
        .arg(*bitsPerSecond / 1000000.0, 0, 'f', 2);
}

[[nodiscard]] QString OptionalMetric(const std::optional<double>& value, const int precision = 4)
{
    return value && std::isfinite(*value) ? QString::number(*value, 'g', precision) : QStringLiteral("—");
}

[[nodiscard]] QString DurationText(const std::uint64_t milliseconds)
{
    const std::uint64_t totalSeconds = milliseconds / 1000;
    const std::uint64_t hours = totalSeconds / 3600;
    const std::uint64_t minutes = (totalSeconds / 60) % 60;
    const std::uint64_t seconds = totalSeconds % 60;
    return QStringLiteral("%1:%2:%3.%4").arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0')).arg(seconds, 2, 10, QLatin1Char('0'))
        .arg(milliseconds % 1000, 3, 10, QLatin1Char('0'));
}

void PlaceOnRightmostScreen(QWidget& window)
{
    const QList<QScreen*> screens = QApplication::screens();
    if (screens.isEmpty())
    {
        return;
    }
    QScreen* const target = *std::max_element(screens.begin(), screens.end(), [](const QScreen* left,
        const QScreen* right)
    {
        return left->geometry().left() < right->geometry().left();
    });
    const QRect available = target->availableGeometry();
    const int maximumWidth = (std::max)(1, available.width() - 48);
    const int maximumHeight = (std::max)(1, available.height() - 48);
    window.resize((std::min)(window.width(), maximumWidth), (std::min)(window.height(), maximumHeight));
    window.move(available.left() + 24, available.top() + 24);
}

[[nodiscard]] QString EtaText(const std::optional<std::uint64_t> milliseconds)
{
    if (!milliseconds)
    {
        return QStringLiteral("计算中 / 等待有效数据");
    }
    const std::uint64_t totalSeconds = *milliseconds / 1000;
    const std::uint64_t hours = totalSeconds / 3600;
    const std::uint64_t minutes = (totalSeconds / 60) % 60;
    const std::uint64_t seconds = totalSeconds % 60;
    return QStringLiteral("%1:%2:%3").arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0')).arg(seconds, 2, 10, QLatin1Char('0'));
}

[[nodiscard]] QString StateColor(const pbapp::DecoderSnapshot& snapshot)
{
    if (snapshot.state == pbapp::DecoderState::Completed && snapshot.wholeFileDigestVerified &&
        snapshot.finalPublishSucceeded)
    {
        return QStringLiteral("#168a52");
    }
    if (snapshot.state == pbapp::DecoderState::Failed)
    {
        return QStringLiteral("#b42318");
    }
    if (snapshot.state == pbapp::DecoderState::Idle || snapshot.state == pbapp::DecoderState::Stopped)
    {
        return QStringLiteral("#475467");
    }
    return QStringLiteral("#175cd3");
}

[[nodiscard]] QString RotationText(const DXGI_MODE_ROTATION rotation)
{
    switch (rotation)
    {
    case DXGI_MODE_ROTATION_IDENTITY: return QStringLiteral("identity");
    case DXGI_MODE_ROTATION_ROTATE90: return QStringLiteral("90°");
    case DXGI_MODE_ROTATION_ROTATE180: return QStringLiteral("180°");
    case DXGI_MODE_ROTATION_ROTATE270: return QStringLiteral("270°");
    default: return QStringLiteral("unspecified");
    }
}

class DecoderWindow final : public QMainWindow
{
public:
    DecoderWindow()
    {
        setWindowTitle(QStringLiteral("PixelBridge Decoder"));
        setMinimumSize(980, 760);
        BuildUi();
        LoadSettings();
        connect(&controller_, &DecoderApplicationController::SnapshotChanged, this,
            &DecoderWindow::UpdateSnapshot);
        connect(&controller_, &DecoderApplicationController::TerminalStateReached, this,
            &DecoderWindow::HandleTerminalState);
        UpdateRoiDetails();
        UpdateSnapshot();
    }

protected:
    void closeEvent(QCloseEvent* event) override
    {
        const pbapp::WindowCloseAction action = pbapp::GetWindowCloseAction(controller_.IsActive(), closePending_);
        if (action != pbapp::WindowCloseAction::Accept)
        {
            event->ignore();
            if (action == pbapp::WindowCloseAction::RequestStopAndDefer)
            {
                closePending_ = true;
                SetControlsEnabled(false);
                controller_.RequestStop();
                statusMessageLabel_->setText(QStringLiteral("正在安全停止 capture/demod/Receiver/Storage…"));
            }
            return;
        }
        SaveSettings();
        event->accept();
    }

private:
    void BuildUi()
    {
        auto* const central = new QWidget(this);
        auto* const root = new QVBoxLayout(central);
        root->setContentsMargins(20, 18, 20, 18);
        root->setSpacing(12);

        auto* const header = new QHBoxLayout();
        auto* const titleColumn = new QVBoxLayout();
        auto* const title = new QLabel(QStringLiteral("PixelBridge Decoder"));
        QFont titleFont = title->font();
        titleFont.setPointSize(titleFont.pointSize() + 7);
        titleFont.setBold(true);
        title->setFont(titleFont);
        auto* const mode = new QLabel(QStringLiteral("Instant LocalDesktop · verified file recovery is authoritative here"));
        mode->setStyleSheet(QStringLiteral("color:#667085;"));
        titleColumn->addWidget(title);
        titleColumn->addWidget(mode);
        stateLabel_ = new QLabel(QStringLiteral("Idle"));
        stateLabel_->setAlignment(Qt::AlignCenter);
        stateLabel_->setMinimumWidth(170);
        stateLabel_->setStyleSheet(QStringLiteral("padding:8px 14px;border-radius:6px;background:#475467;color:white;font-weight:600;"));
        header->addLayout(titleColumn, 1);
        header->addWidget(stateLabel_);
        root->addLayout(header);

        auto* const settingsRow = new QHBoxLayout();
        auto* const outputGroup = new QGroupBox(QStringLiteral("输出目录"));
        auto* const outputLayout = new QGridLayout(outputGroup);
        outputDirectoryEdit_ = new QLineEdit();
        outputDirectoryEdit_->setReadOnly(true);
        outputDirectoryEdit_->setPlaceholderText(QStringLiteral("PBStorage 将创建 output.part 并在 WholeFileDigest PASS 后发布"));
        chooseOutputButton_ = new QPushButton(QStringLiteral("选择输出目录…"));
        outputValidationLabel_ = new QLabel();
        outputValidationLabel_->setWordWrap(true);
        auto* const filenameNote = new QLabel(QStringLiteral(
            "当前 provisional wire 不传原文件名；final name 为安全生成的 PixelBridge-<SessionTag>.bin。"));
        filenameNote->setWordWrap(true);
        filenameNote->setStyleSheet(QStringLiteral("color:#667085;"));
        outputLayout->addWidget(outputDirectoryEdit_, 0, 0);
        outputLayout->addWidget(chooseOutputButton_, 0, 1);
        outputLayout->addWidget(outputValidationLabel_, 1, 0, 1, 2);
        outputLayout->addWidget(filenameNote, 2, 0, 1, 2);
        connect(chooseOutputButton_, &QPushButton::clicked, this, &DecoderWindow::ChooseOutputDirectory);
        settingsRow->addWidget(outputGroup, 3);

        auto* const captureGroup = new QGroupBox(QStringLiteral("Capture / Profile"));
        auto* const captureLayout = new QFormLayout(captureGroup);
        backendCombo_ = new QComboBox();
        backendCombo_->addItem(QStringLiteral("WGC"), static_cast<int>(pbapp::CaptureBackend::Wgc));
        backendCombo_->addItem(QStringLiteral("DXGI Desktop Duplication"), static_cast<int>(pbapp::CaptureBackend::Dxgi));
        backendCombo_->setToolTip(QStringLiteral("显式 backend；当前没有 Auto，也不会 silent fallback。"));
        profileCombo_ = new QComboBox();
        profileCombo_->addItem(QStringLiteral("Direct-Level 2x2 · Experimental"),
            static_cast<int>(pbapp::VisualProfile::DirectLevels2x2));
        profileCombo_->addItem(QStringLiteral("Shape+Chroma · Experimental"),
            static_cast<int>(pbapp::VisualProfile::ShapeChroma));
        profileCombo_->setToolTip(QStringLiteral("必须与 Encoder 一致；当前 provisional wire 不传 Visual Profile。"));
        actualBackendLabel_ = new QLabel(QStringLiteral("Requested: WGC · Actual: —"));
        actualBackendLabel_->setWordWrap(true);
        captureLayout->addRow(QStringLiteral("Requested backend"), backendCombo_);
        captureLayout->addRow(QStringLiteral("Visual Profile"), profileCombo_);
        captureLayout->addRow(QStringLiteral("Binding"), actualBackendLabel_);
        settingsRow->addWidget(captureGroup, 2);
        root->addLayout(settingsRow);

        auto* const roiGroup = new QGroupBox(QStringLiteral("ROI · physical pixels · strict 1:1"));
        auto* const roiLayout = new QVBoxLayout(roiGroup);
        auto* const roiActions = new QHBoxLayout();
        selectRoiButton_ = new QPushButton(QStringLiteral("选择 ROI…"));
        reselectRoiButton_ = new QPushButton(QStringLiteral("重新选择"));
        clearRoiButton_ = new QPushButton(QStringLiteral("清除"));
        fullMonitorButton_ = new QPushButton(QStringLiteral("使用整个显示器"));
        roiActions->addWidget(selectRoiButton_);
        roiActions->addWidget(reselectRoiButton_);
        roiActions->addWidget(clearRoiButton_);
        roiActions->addWidget(fullMonitorButton_);
        roiActions->addStretch(1);
        roiDetailsLabel_ = new QLabel(QStringLiteral("未选择 ROI"));
        roiDetailsLabel_->setWordWrap(true);
        roiLayout->addLayout(roiActions);
        roiLayout->addWidget(roiDetailsLabel_);
        connect(selectRoiButton_, &QPushButton::clicked, this, &DecoderWindow::SelectRoi);
        connect(reselectRoiButton_, &QPushButton::clicked, this, &DecoderWindow::SelectRoi);
        connect(clearRoiButton_, &QPushButton::clicked, this, &DecoderWindow::ClearRoi);
        connect(fullMonitorButton_, &QPushButton::clicked, this, &DecoderWindow::UseFullMonitor);
        root->addWidget(roiGroup);

        auto* const actionRow = new QHBoxLayout();
        startButton_ = new QPushButton(QStringLiteral("开始接收"));
        stopButton_ = new QPushButton(QStringLiteral("停止接收"));
        startButton_->setMinimumHeight(40);
        stopButton_->setMinimumHeight(40);
        startButton_->setStyleSheet(QStringLiteral("font-weight:600;background:#1570ef;color:white;padding:8px 22px;border-radius:5px;"));
        stopButton_->setEnabled(false);
        actionRow->addWidget(startButton_);
        actionRow->addWidget(stopButton_);
        actionRow->addStretch(1);
        auto* const completionRule = new QLabel(QStringLiteral("Completed 仅在 WholeFileDigest PASS + final publish success 后成立"));
        completionRule->setStyleSheet(QStringLiteral("color:#175cd3;font-weight:600;"));
        actionRow->addWidget(completionRule);
        connect(startButton_, &QPushButton::clicked, this, &DecoderWindow::StartReceiving);
        connect(stopButton_, &QPushButton::clicked, &controller_, &DecoderApplicationController::RequestStop);
        root->addLayout(actionRow);

        auto* const progressGroup = new QGroupBox(QStringLiteral("File Recovery · authoritative verified bytes"));
        auto* const progressLayout = new QGridLayout(progressGroup);
        progressBar_ = new QProgressBar();
        progressBar_->setRange(0, 0);
        progressBar_->setFormat(QStringLiteral("等待 Session Descriptor…"));
        progressBar_->setToolTip(QStringLiteral("严格定义为 verifiedRawBytes / OriginalFileSize；当前单 Segment 路径以已验证 Segment 粒度推进。"));
        progressTextLabel_ = new QLabel(QStringLiteral("总大小未知"));
        QFont progressFont = progressTextLabel_->font();
        progressFont.setPointSize(progressFont.pointSize() + 3);
        progressFont.setBold(true);
        progressTextLabel_->setFont(progressFont);
        speedLabel_ = new QLabel(QStringLiteral("等待有效数据"));
        speedLabel_->setFont(progressFont);
        etaLabel_ = new QLabel(QStringLiteral("计算中 / 等待有效数据"));
        sessionLabel_ = new QLabel(QStringLiteral("Session —"));
        statusMessageLabel_ = new QLabel(QStringLiteral("Idle"));
        statusMessageLabel_->setWordWrap(true);
        completionDetailsLabel_ = new QLabel(QStringLiteral("尚未发布 final 文件"));
        completionDetailsLabel_->setWordWrap(true);
        completionDetailsLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        completionDetailsLabel_->setStyleSheet(QStringLiteral("color:#475467;"));
        progressLayout->addWidget(progressBar_, 0, 0, 1, 4);
        progressLayout->addWidget(new QLabel(QStringLiteral("文件恢复进度")), 1, 0);
        progressLayout->addWidget(progressTextLabel_, 2, 0);
        progressLayout->addWidget(new QLabel(QStringLiteral("VerifiedEncodedGoodput · digest-gated")), 1, 1);
        progressLayout->addWidget(speedLabel_, 2, 1);
        progressLayout->addWidget(new QLabel(QStringLiteral("预计剩余")), 1, 2);
        progressLayout->addWidget(etaLabel_, 2, 2);
        progressLayout->addWidget(sessionLabel_, 1, 3, 2, 1);
        progressLayout->addWidget(statusMessageLabel_, 3, 0, 1, 4);
        progressLayout->addWidget(completionDetailsLabel_, 4, 0, 1, 4);
        root->addWidget(progressGroup);

        advancedGroup_ = new QGroupBox(QStringLiteral("Advanced / PBTelemetry component snapshots / RemoteVisual metadata"));
        advancedGroup_->setCheckable(true);
        advancedGroup_->setChecked(false);
        auto* const advancedLayout = new QGridLayout(advancedGroup_);
        telemetryLabel_ = new QLabel(QStringLiteral("No runtime telemetry"));
        telemetryLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        telemetryLabel_->setWordWrap(true);
        channelCombo_ = new QComboBox();
        channelCombo_->addItems({QStringLiteral("LocalDesktop"), QStringLiteral("Sunlogin RemoteVisual"),
            QStringLiteral("Other")});
        networkNoteEdit_ = new QLineEdit();
        networkNoteEdit_->setPlaceholderText(QStringLiteral("仅 run metadata，不影响接受判定"));
        advancedLayout->addWidget(new QLabel(QStringLiteral("ChannelType")), 0, 0);
        advancedLayout->addWidget(channelCombo_, 0, 1);
        advancedLayout->addWidget(new QLabel(QStringLiteral("Network note")), 0, 2);
        advancedLayout->addWidget(networkNoteEdit_, 0, 3);
        advancedLayout->addWidget(telemetryLabel_, 1, 0, 1, 4);
        root->addWidget(advancedGroup_);

        auto* const diagnosticsGroup = new QGroupBox(QStringLiteral("Log / Diagnostics"));
        auto* const diagnosticsLayout = new QVBoxLayout(diagnosticsGroup);
        logEdit_ = new QPlainTextEdit();
        logEdit_->setReadOnly(true);
        logEdit_->setMaximumBlockCount(500);
        logEdit_->setMaximumHeight(120);
        auto* const diagnosticsActions = new QHBoxLayout();
        auto* const exportButton = new QPushButton(QStringLiteral("导出运行报告"));
        auto* const copyButton = new QPushButton(QStringLiteral("复制诊断信息"));
        diagnosticsActions->addWidget(exportButton);
        diagnosticsActions->addWidget(copyButton);
        diagnosticsActions->addStretch(1);
        diagnosticsLayout->addWidget(logEdit_);
        diagnosticsLayout->addLayout(diagnosticsActions);
        connect(exportButton, &QPushButton::clicked, this, &DecoderWindow::ExportReport);
        connect(copyButton, &QPushButton::clicked, this, &DecoderWindow::CopyDiagnostics);
        root->addWidget(diagnosticsGroup);

        setCentralWidget(central);
        setStyleSheet(QStringLiteral(
            "QMainWindow{background:#f7f8fa;} QGroupBox{font-weight:600;border:1px solid #d0d5dd;border-radius:7px;margin-top:10px;padding-top:9px;background:white;}"
            "QGroupBox::title{subcontrol-origin:margin;left:10px;padding:0 4px;} QLineEdit,QComboBox,QPlainTextEdit{padding:5px;border:1px solid #cfd4dc;border-radius:4px;background:white;}"
            "QPushButton{padding:6px 12px;} QProgressBar{height:25px;text-align:center;border:1px solid #b8c0cc;border-radius:4px;} QProgressBar::chunk{background:#1570ef;}"));
    }

    void LoadSettings()
    {
        QSettings settings;
        restoreGeometry(settings.value(QStringLiteral("ui/windowGeometry")).toByteArray());
        outputDirectoryEdit_->setText(settings.value(QStringLiteral("ui/lastOutputDirectory"),
            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).toString());
        backendCombo_->setCurrentIndex(settings.value(QStringLiteral("ui/backend"), 0).toInt() == 1 ? 1 : 0);
        advancedGroup_->setChecked(settings.value(QStringLiteral("ui/advancedExpanded"), false).toBool());
        UpdateOutputValidation();
    }

    void SaveSettings()
    {
        QSettings settings;
        settings.setValue(QStringLiteral("ui/windowGeometry"), saveGeometry());
        settings.setValue(QStringLiteral("ui/lastOutputDirectory"), outputDirectoryEdit_->text());
        settings.setValue(QStringLiteral("ui/backend"), backendCombo_->currentIndex());
        settings.setValue(QStringLiteral("ui/advancedExpanded"), advancedGroup_->isChecked());
    }

    void ChooseOutputDirectory()
    {
        const QString directory = QFileDialog::getExistingDirectory(this, QStringLiteral("选择 Decoder 输出目录"),
            outputDirectoryEdit_->text());
        if (!directory.isEmpty())
        {
            outputDirectoryEdit_->setText(QDir::toNativeSeparators(directory));
            UpdateOutputValidation();
        }
    }

    void UpdateOutputValidation()
    {
        const QDir directory(outputDirectoryEdit_->text());
        const bool valid = directory.exists();
        outputValidationLabel_->setText(valid ? QStringLiteral("✓ 输出目录存在；existing final/.part 将 fail closed，不会覆盖") :
            QStringLiteral("✕ 输出目录不存在"));
        outputValidationLabel_->setStyleSheet(valid ? QStringLiteral("color:#168a52;") : QStringLiteral("color:#b42318;"));
        UpdateActionButtons();
    }

    void SelectRoi()
    {
        pbscreenregion::ScreenCaptureRegion selected = region_;
        const pbscreenregion::ScreenRegionStatus status = pbscreenregion::SelectScreenCaptureRegion(selected);
        if (!status)
        {
            if (status.code != pbscreenregion::ScreenRegionErrorCode::Cancelled)
            {
                QMessageBox::warning(this, QStringLiteral("ROI 选择失败"),
                    QStringLiteral("%1 · stage=%2 · native=%3")
                        .arg(QString::fromLatin1(pbscreenregion::GetScreenRegionErrorName(status.code)))
                        .arg(static_cast<unsigned int>(status.stage)).arg(status.nativeError));
            }
            return;
        }
        region_ = selected;
        hasRegion_ = true;
        UpdateRoiDetails();
    }

    void ClearRoi()
    {
        region_ = {};
        hasRegion_ = false;
        UpdateRoiDetails();
    }

    void UseFullMonitor()
    {
        if (!hasRegion_ || region_.monitor == nullptr)
        {
            QMessageBox::information(this, QStringLiteral("需要 monitor"),
                QStringLiteral("请先选择一次 ROI，以确定目标 monitor。"));
            return;
        }
        pbscreenregion::ScreenCaptureRegion resolved;
        const auto status = pbscreenregion::ResolveScreenCaptureRegion(region_.monitorPhysicalRect, resolved);
        if (!status)
        {
            QMessageBox::warning(this, QStringLiteral("使用整个显示器失败"),
                QString::fromLatin1(pbscreenregion::GetScreenRegionErrorName(status.code)));
            return;
        }
        region_ = resolved;
        hasRegion_ = true;
        UpdateRoiDetails();
    }

    void UpdateRoiDetails()
    {
        if (!hasRegion_)
        {
            roiDetailsLabel_->setText(QStringLiteral("未选择 ROI。RemoteVisual：先打开远程桌面窗口，再框选其中 1920x1080 PixelBridge Data Window。"));
            roiDetailsLabel_->setStyleSheet(QStringLiteral("color:#b42318;"));
            reselectRoiButton_->setEnabled(false);
            clearRoiButton_->setEnabled(false);
            fullMonitorButton_->setEnabled(false);
            UpdateActionButtons();
            return;
        }
        const std::int64_t width = static_cast<std::int64_t>(region_.physicalRect.right) - region_.physicalRect.left;
        const std::int64_t height = static_cast<std::int64_t>(region_.physicalRect.bottom) - region_.physicalRect.top;
        const bool compatible = width == pbapp::phase1CanvasWidth && height == pbapp::phase1CanvasHeight &&
            region_.rotation == DXGI_MODE_ROTATION_IDENTITY;
        roiDetailsLabel_->setText(QStringLiteral("Physical RECT: X=%1 Y=%2 · %3x%4 · DPI %5x%6 · rotation %7 · %8")
            .arg(region_.physicalRect.left).arg(region_.physicalRect.top).arg(width).arg(height)
            .arg(region_.dpiX).arg(region_.dpiY).arg(RotationText(region_.rotation))
            .arg(compatible ? QStringLiteral("✓ strict Phase-1 1:1 geometry compatible") :
                QStringLiteral("✕ incompatible; no bilinear resize or silent scaling will be used")));
        roiDetailsLabel_->setStyleSheet(compatible ? QStringLiteral("color:#168a52;") : QStringLiteral("color:#b42318;"));
        reselectRoiButton_->setEnabled(!controller_.IsActive());
        clearRoiButton_->setEnabled(!controller_.IsActive());
        fullMonitorButton_->setEnabled(!controller_.IsActive());
        UpdateActionButtons();
    }

    void StartReceiving()
    {
        pbapp::DecoderConfig config;
        config.outputDirectory = outputDirectoryEdit_->text().toStdWString();
        config.captureBackend = static_cast<pbapp::CaptureBackend>(backendCombo_->currentData().toInt());
        config.visualProfile = static_cast<pbapp::VisualProfile>(profileCombo_->currentData().toInt());
        config.region = region_;
        config.remoteMetadata.channelType = channelCombo_->currentIndex() == 0 ? pbapp::ChannelType::LocalDesktop :
            channelCombo_->currentIndex() == 1 ? pbapp::ChannelType::SunloginRemoteVisual : pbapp::ChannelType::Other;
        config.remoteMetadata.networkNote = networkNoteEdit_->text().toUtf8().toStdString();
        SetControlsEnabled(false);
        const QString error = controller_.Start(config);
        if (!error.isEmpty())
        {
            SetControlsEnabled(true);
            UpdateActionButtons();
            QMessageBox::warning(this, QStringLiteral("无法开始接收"), error);
            return;
        }
        AppendLog(QStringLiteral("Start accepted; explicit capture backend is starting."));
        UpdateSnapshot();
    }

    void UpdateSnapshot()
    {
        const pbapp::DecoderSnapshot snapshot = controller_.GetSnapshot();
        stateLabel_->setText(QString::fromLatin1(pbapp::GetDecoderStateName(snapshot.state)));
        stateLabel_->setStyleSheet(QStringLiteral("padding:8px 14px;border-radius:6px;background:%1;color:white;font-weight:600;")
            .arg(StateColor(snapshot)));
        actualBackendLabel_->setText(QStringLiteral("Requested: %1 · Actual: %2\n%3")
            .arg(QString::fromLatin1(pbapp::GetCaptureBackendName(snapshot.requestedBackend)))
            .arg(snapshot.actualBackend ? QString::fromLatin1(pbapp::GetCaptureBackendName(*snapshot.actualBackend)) : QStringLiteral("—"))
            .arg(FromUtf8(snapshot.backendReason)));
        if (!snapshot.descriptorKnown)
        {
            progressBar_->setRange(0, 0);
            progressBar_->setFormat(QStringLiteral("等待 Session Descriptor…"));
            progressTextLabel_->setText(QStringLiteral("总大小未知"));
        }
        else
        {
            progressBar_->setRange(0, 10000);
            const double progress = snapshot.recoveryProgress.value_or(0.0);
            const int progressValue = static_cast<int>((std::clamp)(progress, 0.0, 1.0) * 10000.0);
            progressBar_->setValue(progressValue);
            progressBar_->setFormat(QStringLiteral("%1% · verified raw bytes").arg(progress * 100.0, 0, 'f', 1));
            progressTextLabel_->setText(QStringLiteral("%1% · %2 / %3")
                .arg(progress * 100.0, 0, 'f', 1).arg(HumanBytes(snapshot.verifiedRawBytes))
                .arg(HumanBytes(snapshot.originalFileBytes)));
        }
        speedLabel_->setText(QStringLiteral("%1\nETA basis · smoothed verified raw: %2")
            .arg(BitRateText(snapshot.verifiedEncodedGoodputBitsPerSecond))
            .arg(RateText(snapshot.smoothedVerifiedRawGoodputBytesPerSecond)));
        etaLabel_->setText(EtaText(snapshot.etaMilliseconds));
        sessionLabel_->setText(!snapshot.descriptorKnown ? QStringLiteral("Session —") :
            QStringLiteral("SessionTag %1\nSessionId %2\nSegment %3/%4\n%5 · %6")
                .arg(snapshot.sessionTag, 16, 16, QLatin1Char('0')).arg(FromUtf8(snapshot.sessionIdHex))
                .arg(snapshot.currentSegmentOrdinal + 1).arg(snapshot.segmentCount)
                .arg(QString::fromLatin1(pbapp::GetCompressionCodecName(snapshot.compressionCodec)))
                .arg(QString::fromLatin1(pbapp::GetOuterFecModeName(snapshot.outerFecMode))));
        statusMessageLabel_->setText(FromUtf8(snapshot.statusMessage));
        if (snapshot.state == pbapp::DecoderState::Completed && snapshot.wholeFileDigestVerified &&
            snapshot.finalPublishSucceeded)
        {
            completionDetailsLabel_->setText(QStringLiteral(
                "✓ Published output: %1\nWholeFileDigest (BLAKE3-256): %2\nTotal recovery: %3 · VerifiedEncodedGoodput: %4 · average verified raw goodput: %5")
                .arg(FromUtf8(snapshot.outputPath)).arg(FromUtf8(snapshot.wholeFileDigestHex))
                .arg(DurationText(snapshot.recoveryRuntimeMilliseconds))
                .arg(BitRateText(snapshot.verifiedEncodedGoodputBitsPerSecond))
                .arg(RateText(snapshot.averageVerifiedRawGoodputBytesPerSecond)));
            completionDetailsLabel_->setStyleSheet(QStringLiteral("color:#168a52;font-weight:600;"));
        }
        else
        {
            completionDetailsLabel_->setText(QStringLiteral("尚未发布 final 文件；Digest=%1 · Publish=%2")
                .arg(snapshot.wholeFileDigestVerified ? QStringLiteral("PASS") : QStringLiteral("not accepted"))
                .arg(snapshot.finalPublishSucceeded ? QStringLiteral("success") : QStringLiteral("not published")));
            completionDetailsLabel_->setStyleSheet(QStringLiteral("color:#475467;"));
        }
        telemetryLabel_->setText(QStringLiteral(
            "PBTelemetry current epoch: CaptureFPS=%1 · captured/drop=%2/%3 · BootstrapSuccessRate=%4 (%5/%6) · PreFecBER=%7 · FER=%8\n"
            "EndToEndUniqueVisualFPS=%9 · fingerprinted=%10 (生产 D3D11 fast path 不做 raw-pixel readback/digest；缺少像素 identity 时严格不可用)\n"
            "FrameSequence diagnostics（不是 UniqueVisualFPS）: admitted FPS=%11 · gap/skipped/duplicate/reordered=%12/%13/%14/%15\n"
            "Capture component: epoch=%16 resets=%17 recreates=%18 deviceRecoveries=%19 · arrived/delivered/drop=%20/%21/%22 · "
            "Bootstrap LocalDesktop accepted/erasure/mismatch/controlFailure=%23/%24/%25/%26\n"
            "FEC: failed/evaluated=%27/%28 · coded errors/bits=%29/%30 · FEC/CRC/identity/falseAccept=%31/%32/%33/%34 · accepted outer blocks=%35\n"
            "HWM Lease/Demod/Result=%36/%37/%38 · stale=%39 · ROI/Demod GPU=%40/%41 100ns · Bootstrap/Post GPU CPU=%42/%43 100ns · "
            "verified encoded=%44 B · Digest=%45 · Publish=%46")
            .arg(OptionalMetric(snapshot.captureFps)).arg(snapshot.telemetryCapturedFrames).arg(snapshot.telemetryDroppedFrames)
            .arg(OptionalMetric(snapshot.bootstrapSuccessRate)).arg(snapshot.telemetryBootstrapSuccesses)
            .arg(snapshot.telemetryBootstrapAttempts).arg(OptionalMetric(snapshot.preFecBerEstimate, 6))
            .arg(OptionalMetric(snapshot.fecFrameErrorRate, 6)).arg(OptionalMetric(snapshot.uniqueVisualFps))
            .arg(snapshot.fingerprintedFrames).arg(OptionalMetric(snapshot.admittedFrameSequenceFps))
            .arg(snapshot.frameSequenceGapEvents).arg(snapshot.skippedFrameSequences).arg(snapshot.duplicateFrameSequences)
            .arg(snapshot.reorderedFrameSequences).arg(snapshot.captureEpoch).arg(snapshot.captureEpochResets)
            .arg(snapshot.captureRecreates).arg(snapshot.captureDeviceRecoveries).arg(snapshot.captureArrivedFrames)
            .arg(snapshot.captureDeliveredFrames).arg(snapshot.captureDroppedFrames).arg(snapshot.bootstrapAcceptedFrames)
            .arg(snapshot.bootstrapRejectedFrames).arg(snapshot.bootstrapMismatchFrames)
            .arg(snapshot.bootstrapControlFrameFailures).arg(snapshot.postFecFailedFrames).arg(snapshot.evaluatedDataFrames)
            .arg(snapshot.erroneousCodedBits).arg(snapshot.comparedCodedBits).arg(snapshot.fecFailures)
            .arg(snapshot.crcFailures).arg(snapshot.identityFailures).arg(snapshot.falseAcceptedCodewords)
            .arg(snapshot.acceptedTransportBlocks).arg(snapshot.frameLeaseHighWater).arg(snapshot.demodPendingHighWater)
            .arg(snapshot.resultQueueHighWater).arg(snapshot.staleResultDrops).arg(snapshot.roiGpuTimeTotal100ns)
            .arg(snapshot.demodGpuTimeTotal100ns).arg(snapshot.bootstrapCpuTimeTotal100ns)
            .arg(snapshot.postGpuFecCpuTimeTotal100ns).arg(snapshot.verifiedEncodedBytes)
            .arg(snapshot.wholeFileDigestVerified ? QStringLiteral("PASS") : QStringLiteral("not accepted"))
            .arg(snapshot.finalPublishSucceeded ? QStringLiteral("success") : QStringLiteral("not published")));
        if (snapshot.state != lastLoggedState_ || (!snapshot.errorDetail.empty() && snapshot.errorDetail != lastError_))
        {
            AppendLog(QStringLiteral("%1 — %2%3")
                .arg(QString::fromLatin1(pbapp::GetDecoderStateName(snapshot.state)))
                .arg(FromUtf8(snapshot.statusMessage))
                .arg(snapshot.errorDetail.empty() ? QString() : QStringLiteral(" | %1").arg(FromUtf8(snapshot.errorDetail))));
            lastLoggedState_ = snapshot.state;
            lastError_ = snapshot.errorDetail;
        }
        if (!controller_.IsActive())
        {
            SetControlsEnabled(true);
        }
        UpdateActionButtons();
    }

    void HandleTerminalState()
    {
        UpdateSnapshot();
        const pbapp::DecoderSnapshot snapshot = controller_.GetSnapshot();
        if (snapshot.state == pbapp::DecoderState::Completed && snapshot.wholeFileDigestVerified &&
            snapshot.finalPublishSucceeded)
        {
            AppendLog(QStringLiteral("✓ 文件接收完成：%1").arg(FromUtf8(snapshot.outputPath)));
        }
        if (closePending_)
        {
            QTimer::singleShot(0, this, &QWidget::close);
        }
    }

    void SetControlsEnabled(const bool enabled)
    {
        outputDirectoryEdit_->setEnabled(enabled);
        chooseOutputButton_->setEnabled(enabled);
        backendCombo_->setEnabled(enabled);
        profileCombo_->setEnabled(enabled);
        selectRoiButton_->setEnabled(enabled);
        reselectRoiButton_->setEnabled(enabled && hasRegion_);
        clearRoiButton_->setEnabled(enabled && hasRegion_);
        fullMonitorButton_->setEnabled(enabled && hasRegion_);
        channelCombo_->setEnabled(enabled);
        networkNoteEdit_->setEnabled(enabled);
    }

    void UpdateActionButtons()
    {
        pbapp::DecoderConfig config;
        config.outputDirectory = outputDirectoryEdit_->text().toStdWString();
        config.captureBackend = static_cast<pbapp::CaptureBackend>(backendCombo_->currentData().toInt());
        config.visualProfile = static_cast<pbapp::VisualProfile>(profileCombo_->currentData().toInt());
        config.region = region_;
        const bool valid = hasRegion_ && static_cast<bool>(pbapp::ValidateDecoderConfig(config));
        const bool active = controller_.IsActive();
        startButton_->setEnabled(valid && !active);
        stopButton_->setEnabled(active);
    }

    void AppendLog(const QString& message)
    {
        logEdit_->appendPlainText(QStringLiteral("[%1] %2")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"))).arg(message));
    }

    [[nodiscard]] pbapp::RunReportContext ReportContext() const
    {
        const pbcore::BuildInfo buildInfo = pbcore::GetBuildInfo();
        return {"PixelBridgeDecoder", buildInfo.version, PB_GIT_COMMIT,
            QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString()};
    }

    void ExportReport()
    {
        const QString defaultPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) +
            QStringLiteral("/PixelBridge-Decoder-RunReport.json");
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出 Decoder 运行报告"),
            defaultPath, QStringLiteral("JSON (*.json)"));
        if (path.isEmpty())
        {
            return;
        }
        const std::string report = pbapp::BuildDecoderRunReportJson(ReportContext(), controller_.GetSnapshot());
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(report.data(), static_cast<qint64>(report.size())) !=
            static_cast<qint64>(report.size()) || !file.commit())
        {
            QMessageBox::critical(this, QStringLiteral("导出失败"), file.errorString());
            return;
        }
        AppendLog(QStringLiteral("Run report exported: %1").arg(path));
    }

    void CopyDiagnostics()
    {
        const std::string diagnostics = pbapp::BuildDecoderDiagnostics(controller_.GetSnapshot());
        QApplication::clipboard()->setText(FromUtf8(diagnostics));
        AppendLog(QStringLiteral("Diagnostics copied to clipboard (no payload bytes)."));
    }

    DecoderApplicationController controller_;
    QLineEdit* outputDirectoryEdit_ = nullptr;
    QPushButton* chooseOutputButton_ = nullptr;
    QLabel* outputValidationLabel_ = nullptr;
    QComboBox* backendCombo_ = nullptr;
    QComboBox* profileCombo_ = nullptr;
    QLabel* actualBackendLabel_ = nullptr;
    QPushButton* selectRoiButton_ = nullptr;
    QPushButton* reselectRoiButton_ = nullptr;
    QPushButton* clearRoiButton_ = nullptr;
    QPushButton* fullMonitorButton_ = nullptr;
    QLabel* roiDetailsLabel_ = nullptr;
    QPushButton* startButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QLabel* stateLabel_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
    QLabel* progressTextLabel_ = nullptr;
    QLabel* speedLabel_ = nullptr;
    QLabel* etaLabel_ = nullptr;
    QLabel* sessionLabel_ = nullptr;
    QLabel* statusMessageLabel_ = nullptr;
    QLabel* completionDetailsLabel_ = nullptr;
    QGroupBox* advancedGroup_ = nullptr;
    QLabel* telemetryLabel_ = nullptr;
    QComboBox* channelCombo_ = nullptr;
    QLineEdit* networkNoteEdit_ = nullptr;
    QPlainTextEdit* logEdit_ = nullptr;
    pbscreenregion::ScreenCaptureRegion region_;
    pbapp::DecoderState lastLoggedState_ = pbapp::DecoderState::Idle;
    std::string lastError_;
    bool hasRegion_ = false;
    bool closePending_ = false;
};

} // namespace

int RunDecoderGui(const int argumentCount, wchar_t* arguments[])
{
    const bool smoke = argumentCount == 2 && std::wstring_view(arguments[1]) == L"--gui-smoke";
    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    int guiArgumentCount = 1;
    char applicationName[] = "PixelBridgeDecoder";
    char* guiArguments[] = {applicationName, nullptr};
    QApplication application(guiArgumentCount, guiArguments);
    QCoreApplication::setOrganizationName(QStringLiteral("PixelBridge"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("pixelbridge.local"));
    QCoreApplication::setApplicationName(QStringLiteral("PixelBridgeDecoder"));
    DecoderWindow window;
    if (smoke)
    {
        PlaceOnRightmostScreen(window);
    }
    window.show();
    if (smoke)
    {
        QTimer::singleShot(350, &application, &QCoreApplication::quit);
    }
    return application.exec();
}
