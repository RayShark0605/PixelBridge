#include "decoder_gui.h"

#include "decoder_application_controller.h"

#include "monitor_catalog.h"
#include "remote_visual_metadata_preset_qt.h"
#include "run_report.h"
#include "pbcore/build_info.h"

#include <QApplication>
#include <QClipboard>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
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
#include <QSettings>
#include <QStandardPaths>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>
#include <cstdint>
#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

[[nodiscard]] QString RotationText(DXGI_MODE_ROTATION rotation);

class PhysicalRoiDialog final : public QDialog
{
public:
    PhysicalRoiDialog(std::vector<pbapp::MonitorInfo> monitors, const QString& protectedDeviceName,
        const pbscreenregion::ScreenCaptureRegion* existingRegion, QWidget* parent) :
        QDialog(parent), monitors_(std::move(monitors)), protectedDeviceName_(protectedDeviceName)
    {
        setWindowTitle(QStringLiteral("输入 ExperimentMonitor 物理 ROI"));
        setModal(true);
        auto* const root = new QVBoxLayout(this);
        auto* const note = new QLabel(QStringLiteral(
            "此对话框不创建全桌面 overlay、不移动鼠标，也不捕获任何屏幕。请显式选择 ExperimentMonitor，并输入远控软件窗口内 PixelBridge Data Window 的物理像素 RECT。"));
        note->setWordWrap(true);
        root->addWidget(note);
        auto* const form = new QFormLayout();
        monitorCombo_ = new QComboBox();
        monitorCombo_->addItem(QStringLiteral("请选择 ExperimentMonitor"), -1);
        for (std::size_t index = 0; index < monitors_.size(); index++)
        {
            const pbapp::MonitorInfo& monitor = monitors_[index];
            const QString deviceName = QString::fromStdWString(monitor.deviceName);
            if (!protectedDeviceName_.isEmpty() && deviceName == protectedDeviceName_)
            {
                continue;
            }
            monitorCombo_->addItem(QStringLiteral("%1 · [%2,%3]-[%4,%5] · %6 Hz · DPI %7x%8 · rotation %9%10")
                .arg(deviceName).arg(monitor.physicalRect.left).arg(monitor.physicalRect.top)
                .arg(monitor.physicalRect.right).arg(monitor.physicalRect.bottom).arg(monitor.refreshRate)
                .arg(monitor.dpiX).arg(monitor.dpiY).arg(RotationText(monitor.rotation))
                .arg(monitor.primary ? QStringLiteral(" · primary") : QString()), static_cast<int>(index));
        }
        leftSpin_ = MakeCoordinateSpin();
        topSpin_ = MakeCoordinateSpin();
        widthSpin_ = MakeExtentSpin();
        heightSpin_ = MakeExtentSpin();
        form->addRow(QStringLiteral("ExperimentMonitor"), monitorCombo_);
        form->addRow(QStringLiteral("Physical left"), leftSpin_);
        form->addRow(QStringLiteral("Physical top"), topSpin_);
        form->addRow(QStringLiteral("Width"), widthSpin_);
        form->addRow(QStringLiteral("Height"), heightSpin_);
        root->addLayout(form);
        auto* const actionRow = new QHBoxLayout();
        auto* const snapButton = new QPushButton(QStringLiteral("在所选显示器内居中 Snap 1920×1080"));
        actionRow->addWidget(snapButton);
        actionRow->addStretch(1);
        root->addLayout(actionRow);
        validationLabel_ = new QLabel();
        validationLabel_->setWordWrap(true);
        root->addWidget(validationLabel_);
        buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        root->addWidget(buttons_);
        connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(monitorCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this]() { ConfigureSelectedMonitor(); });
        connect(snapButton, &QPushButton::clicked, this, [this]() { SnapPhase1Canvas(); });
        for (QSpinBox* const spin : {leftSpin_, topSpin_, widthSpin_, heightSpin_})
        {
            connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this]() { UpdateValidation(); });
        }
        if (existingRegion != nullptr)
        {
            for (int comboIndex = 1; comboIndex < monitorCombo_->count(); comboIndex++)
            {
                const int monitorIndex = monitorCombo_->itemData(comboIndex).toInt();
                if (monitorIndex >= 0 && static_cast<std::size_t>(monitorIndex) < monitors_.size() &&
                    monitors_[monitorIndex].monitor == existingRegion->monitor)
                {
                    existingRect_ = existingRegion->physicalRect;
                    monitorCombo_->setCurrentIndex(comboIndex);
                    break;
                }
            }
        }
        UpdateValidation();
    }

    [[nodiscard]] bool GetSelection(RECT& target, pbapp::MonitorInfo& monitor) const noexcept
    {
        const pbapp::MonitorInfo* const selected = SelectedMonitor();
        if (selected == nullptr)
        {
            return false;
        }
        const std::int64_t right = static_cast<std::int64_t>(leftSpin_->value()) + widthSpin_->value();
        const std::int64_t bottom = static_cast<std::int64_t>(topSpin_->value()) + heightSpin_->value();
        if (right > (std::numeric_limits<LONG>::max)() || bottom > (std::numeric_limits<LONG>::max)())
        {
            return false;
        }
        target = {leftSpin_->value(), topSpin_->value(), static_cast<LONG>(right), static_cast<LONG>(bottom)};
        if (!pbapp::RectContains(selected->physicalRect, target))
        {
            return false;
        }
        for (const pbapp::MonitorInfo& candidate : monitors_)
        {
            if (!protectedDeviceName_.isEmpty() && QString::fromStdWString(candidate.deviceName) == protectedDeviceName_ &&
                pbapp::RectIntersects(candidate.physicalRect, target))
            {
                return false;
            }
        }
        monitor = *selected;
        return true;
    }

private:
    [[nodiscard]] static QSpinBox* MakeCoordinateSpin()
    {
        auto* const spin = new QSpinBox();
        spin->setRange((std::numeric_limits<int>::min)(), (std::numeric_limits<int>::max)());
        return spin;
    }

    [[nodiscard]] static QSpinBox* MakeExtentSpin()
    {
        auto* const spin = new QSpinBox();
        spin->setRange(1, (std::numeric_limits<int>::max)());
        spin->setValue(1);
        return spin;
    }

    [[nodiscard]] const pbapp::MonitorInfo* SelectedMonitor() const noexcept
    {
        const int monitorIndex = monitorCombo_->currentData().toInt();
        return monitorIndex >= 0 && static_cast<std::size_t>(monitorIndex) < monitors_.size() ?
            &monitors_[monitorIndex] : nullptr;
    }

    void ConfigureSelectedMonitor()
    {
        const pbapp::MonitorInfo* const monitor = SelectedMonitor();
        if (monitor == nullptr)
        {
            UpdateValidation();
            return;
        }
        leftSpin_->setRange(monitor->physicalRect.left, monitor->physicalRect.right - 1);
        topSpin_->setRange(monitor->physicalRect.top, monitor->physicalRect.bottom - 1);
        widthSpin_->setMaximum(monitor->physicalRect.right - monitor->physicalRect.left);
        heightSpin_->setMaximum(monitor->physicalRect.bottom - monitor->physicalRect.top);
        if (existingRect_ && pbapp::RectContains(monitor->physicalRect, *existingRect_))
        {
            leftSpin_->setValue(existingRect_->left);
            topSpin_->setValue(existingRect_->top);
            widthSpin_->setValue(existingRect_->right - existingRect_->left);
            heightSpin_->setValue(existingRect_->bottom - existingRect_->top);
            existingRect_.reset();
        }
        else
        {
            SnapPhase1Canvas();
        }
        UpdateValidation();
    }

    void SnapPhase1Canvas()
    {
        const pbapp::MonitorInfo* const monitor = SelectedMonitor();
        if (monitor == nullptr || !monitor->supportsPhase1Canvas)
        {
            UpdateValidation();
            return;
        }
        leftSpin_->setValue(monitor->phase1CanvasOrigin.x);
        topSpin_->setValue(monitor->phase1CanvasOrigin.y);
        widthSpin_->setValue(pbapp::phase1CanvasWidth);
        heightSpin_->setValue(pbapp::phase1CanvasHeight);
        UpdateValidation();
    }

    void UpdateValidation()
    {
        RECT target{};
        pbapp::MonitorInfo monitor;
        const bool contained = GetSelection(target, monitor);
        const bool strict = contained && target.right - target.left == pbapp::phase1CanvasWidth &&
            target.bottom - target.top == pbapp::phase1CanvasHeight && monitor.rotation == DXGI_MODE_ROTATION_IDENTITY;
        if (!contained)
        {
            validationLabel_->setText(QStringLiteral("✕ 必须显式选择 ExperimentMonitor，且 RECT 必须完全包含在该显示器内。"));
            validationLabel_->setStyleSheet(QStringLiteral("color:#b42318;"));
        }
        else if (!strict)
        {
            validationLabel_->setText(QStringLiteral(
                "△ RECT 位于所选显示器内，但不满足当前 Phase-1 1920×1080 / identity-rotation strict gate；可保存诊断选择，正常 receive 会 fail closed。"));
            validationLabel_->setStyleSheet(QStringLiteral("color:#b54708;"));
        }
        else
        {
            validationLabel_->setText(QStringLiteral("✓ 所选 ROI 满足当前 Phase-1 物理像素 strict geometry。"));
            validationLabel_->setStyleSheet(QStringLiteral("color:#168a52;"));
        }
        buttons_->button(QDialogButtonBox::Ok)->setEnabled(contained);
    }

    std::vector<pbapp::MonitorInfo> monitors_;
    QString protectedDeviceName_;
    std::optional<RECT> existingRect_;
    QComboBox* monitorCombo_ = nullptr;
    QSpinBox* leftSpin_ = nullptr;
    QSpinBox* topSpin_ = nullptr;
    QSpinBox* widthSpin_ = nullptr;
    QSpinBox* heightSpin_ = nullptr;
    QLabel* validationLabel_ = nullptr;
    QDialogButtonBox* buttons_ = nullptr;
};

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
        profileCombo_->addItem(QStringLiteral("RemoteVisual Resilient 8x8 Luma · Experimental"),
            static_cast<int>(pbapp::VisualProfile::RemoteVisualResilient));
        profileCombo_->setToolTip(QStringLiteral(
            "必须与 Encoder 一致；RemoteVisual X2 使用 8x8 二值亮度、中央采样、128x128 区域新鲜度标签和同帧软擦除，仍不是 Certified Profile。"));
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
        selectRoiButton_ = new QPushButton(QStringLiteral("输入物理 ROI…"));
        reselectRoiButton_ = new QPushButton(QStringLiteral("重新输入"));
        selectRoiButton_->setToolTip(QStringLiteral("显式选择单个 monitor 和物理 RECT；不使用全桌面 overlay，不移动鼠标，不抢占系统输入。"));
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
        channelCombo_->addItems({QStringLiteral("LocalDesktop"), QStringLiteral("RemoteVisual (provider-agnostic)"),
            QStringLiteral("Other")});
        remoteProviderEdit_ = new QLineEdit();
        remoteProviderEdit_->setMaxLength(128);
        remoteProviderEdit_->setPlaceholderText(QStringLiteral("当前远控软件名称；仅 metadata，不选择阈值"));
        protectedMonitorCombo_ = new QComboBox();
        protectedMonitorCombo_->addItem(QStringLiteral("请选择受保护显示器（不得捕获）"), QString());
        std::vector<pbapp::MonitorInfo> monitorCatalog;
        if (pbapp::EnumerateMonitors(monitorCatalog))
        {
            for (const pbapp::MonitorInfo& monitor : monitorCatalog)
            {
                const QString deviceName = QString::fromStdWString(monitor.deviceName);
                protectedMonitorCombo_->addItem(QStringLiteral("%1 · [%2,%3]-[%4,%5] · %6 Hz")
                    .arg(deviceName).arg(monitor.physicalRect.left).arg(monitor.physicalRect.top)
                    .arg(monitor.physicalRect.right).arg(monitor.physicalRect.bottom).arg(monitor.refreshRate),
                    deviceName);
            }
        }
        networkNoteEdit_ = new QLineEdit();
        networkNoteEdit_->setPlaceholderText(QStringLiteral("仅 run metadata，不影响接受判定"));
        runIdEdit_ = new QLineEdit();
        runIdEdit_->setMaxLength(32);
        runIdEdit_->setPlaceholderText(QStringLiteral("可选：与 Encoder 相同的 32 字符 lowercase hex RunId"));
        replayCheck_ = new QCheckBox(QStringLiteral("保存 receiver-only RemoteVisual Replay v2（diagnostic-only）"));
        replayCheck_->setChecked(false);
        replayCheck_->setToolTip(QStringLiteral(
            "默认关闭；仅保存右屏 selected ROI，默认最多 256 帧/2 GiB。启用该项的 run 不计入主 goodput baseline。"));
        replayPathEdit_ = new QLineEdit();
        replayPathEdit_->setReadOnly(true);
        replayPathEdit_->setPlaceholderText(QStringLiteral("选择一个不存在的 .pbrv2 最终路径；使用 .partial 后 no-overwrite publish"));
        chooseReplayButton_ = new QPushButton(QStringLiteral("选择 Replay…"));
        metadataPresetPathEdit_ = new QLineEdit();
        metadataPresetPathEdit_->setReadOnly(true);
        metadataPresetPathEdit_->setPlaceholderText(QStringLiteral("可选：与 Encoder 共享的 RemoteVisual metadata JSON"));
        chooseMetadataPresetButton_ = new QPushButton(QStringLiteral("选择 Metadata…"));
        clearMetadataPresetButton_ = new QPushButton(QStringLiteral("清除"));
        advancedLayout->addWidget(new QLabel(QStringLiteral("ChannelType")), 0, 0);
        advancedLayout->addWidget(channelCombo_, 0, 1);
        advancedLayout->addWidget(new QLabel(QStringLiteral("Network note")), 0, 2);
        advancedLayout->addWidget(networkNoteEdit_, 0, 3);
        advancedLayout->addWidget(new QLabel(QStringLiteral("Shared RunId")), 1, 0);
        advancedLayout->addWidget(runIdEdit_, 1, 1, 1, 3);
        advancedLayout->addWidget(new QLabel(QStringLiteral("Remote provider")), 2, 0);
        advancedLayout->addWidget(remoteProviderEdit_, 2, 1, 1, 3);
        advancedLayout->addWidget(new QLabel(QStringLiteral("ProtectedMonitor")), 3, 0);
        advancedLayout->addWidget(protectedMonitorCombo_, 3, 1, 1, 3);
        advancedLayout->addWidget(replayCheck_, 4, 0, 1, 2);
        advancedLayout->addWidget(replayPathEdit_, 4, 2);
        advancedLayout->addWidget(chooseReplayButton_, 4, 3);
        advancedLayout->addWidget(new QLabel(QStringLiteral("Metadata preset")), 5, 0);
        advancedLayout->addWidget(metadataPresetPathEdit_, 5, 1);
        advancedLayout->addWidget(chooseMetadataPresetButton_, 5, 2);
        advancedLayout->addWidget(clearMetadataPresetButton_, 5, 3);
        advancedLayout->addWidget(telemetryLabel_, 6, 0, 1, 4);
        connect(runIdEdit_, &QLineEdit::textChanged, this, &DecoderWindow::UpdateActionButtons);
        connect(backendCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &DecoderWindow::UpdateActionButtons);
        connect(profileCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this]() { UpdateRoiDetails(); });
        connect(channelCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this]() { UpdateRoiDetails(); });
        connect(protectedMonitorCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]()
        {
            monitorSafetySelection_.reset();
            UpdateRoiDetails();
        });
        connect(remoteProviderEdit_, &QLineEdit::textChanged, this, &DecoderWindow::UpdateActionButtons);
        connect(metadataPresetPathEdit_, &QLineEdit::textChanged, this, &DecoderWindow::UpdateActionButtons);
        connect(chooseMetadataPresetButton_, &QPushButton::clicked, this, &DecoderWindow::ChooseMetadataPreset);
        connect(clearMetadataPresetButton_, &QPushButton::clicked, metadataPresetPathEdit_, &QLineEdit::clear);
        connect(replayCheck_, &QCheckBox::toggled, this, [this]() { UpdateRoiDetails(); });
        connect(chooseReplayButton_, &QPushButton::clicked, this, &DecoderWindow::ChooseReplayOutput);
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

    void ChooseReplayOutput()
    {
        const QString defaultPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) +
            QStringLiteral("/PixelBridge-RemoteVisual-Replay.pbrv2");
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("选择新的 Replay v2 最终路径"),
            defaultPath, QStringLiteral("PixelBridge Replay v2 (*.pbrv2)"));
        if (!path.isEmpty())
        {
            replayPathEdit_->setText(QDir::toNativeSeparators(path));
            replayCheck_->setChecked(true);
            UpdateActionButtons();
        }
    }

    void ChooseMetadataPreset()
    {
        const QString initial = metadataPresetPathEdit_->text().isEmpty() ?
            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) : metadataPresetPathEdit_->text();
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择 RemoteVisual metadata preset"),
            initial, QStringLiteral("PixelBridge RemoteVisual metadata (*.json)"));
        if (!path.isEmpty())
        {
            metadataPresetPathEdit_->setText(QDir::toNativeSeparators(path));
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
        const bool remoteChannel = channelCombo_->currentIndex() == 1;
        const QString protectedDeviceName = protectedMonitorCombo_->currentIndex() > 0 ?
            protectedMonitorCombo_->currentData().toString() : QString();
        if (remoteChannel && protectedDeviceName.isEmpty())
        {
            QMessageBox::warning(this, QStringLiteral("显示器安全 preflight 未完成"),
                QStringLiteral("请先在 Advanced 中显式选择左侧 ProtectedMonitor；它不会出现在 ExperimentMonitor 候选中。"));
            return;
        }
        std::vector<pbapp::MonitorInfo> monitors;
        const pbapp::MonitorCatalogStatus catalog = pbapp::EnumerateMonitors(monitors);
        if (!catalog)
        {
            QMessageBox::warning(this, QStringLiteral("显示器枚举失败"),
                QStringLiteral("MonitorCatalog error=%1 native=%2")
                    .arg(static_cast<unsigned int>(catalog.code)).arg(catalog.nativeError));
            return;
        }
        if (remoteChannel)
        {
            RECT windowRect{};
            const HWND windowHandle = reinterpret_cast<HWND>(winId());
            const HMONITOR windowMonitor = MonitorFromWindow(windowHandle, MONITOR_DEFAULTTONULL);
            const auto experiment = std::find_if(monitors.begin(), monitors.end(), [windowMonitor](const pbapp::MonitorInfo& monitor)
            {
                return monitor.monitor == windowMonitor;
            });
            pbapp::MonitorSafetySelection safetySelection;
            const pbapp::MonitorSafetyStatus safety = experiment == monitors.end() ||
                GetWindowRect(windowHandle, &windowRect) == FALSE ?
                pbapp::MonitorSafetyStatus{pbapp::MonitorSafetyError::TargetOutsideExperimentMonitor, {}} :
                pbapp::ResolveMonitorSafetySelection(protectedDeviceName.toStdWString(),
                    experiment->deviceName, safetySelection);
            const pbapp::MonitorSafetyStatus windowSafety = safety ?
                pbapp::ValidateMonitorSafetyTarget(safetySelection, windowRect, windowMonitor) : safety;
            if (!windowSafety)
            {
                QMessageBox::warning(this, QStringLiteral("Decoder 尚未完全位于 ExperimentMonitor"),
                    QStringLiteral("请手动把整个 Decoder 窗口移到右侧 ExperimentMonitor 后重试；不会自动移动窗口。preflight=%1")
                        .arg(QString::fromLatin1(pbapp::GetMonitorSafetyErrorName(windowSafety.code))));
                return;
            }
        }
        PhysicalRoiDialog dialog(std::move(monitors), protectedDeviceName, hasRegion_ ? &region_ : nullptr, this);
        if (dialog.exec() != QDialog::Accepted)
        {
            return;
        }
        RECT target{};
        pbapp::MonitorInfo experimentMonitor;
        if (!dialog.GetSelection(target, experimentMonitor))
        {
            QMessageBox::warning(this, QStringLiteral("ROI 选择失败"),
                QStringLiteral("目标 RECT 不再完全位于显式 ExperimentMonitor，或与 ProtectedMonitor 相交。"));
            return;
        }
        pbscreenregion::ScreenCaptureRegion selected;
        const pbscreenregion::ScreenRegionStatus status = pbscreenregion::ResolveScreenCaptureRegion(target, selected);
        if (!status || selected.monitor != experimentMonitor.monitor)
        {
            QMessageBox::warning(this, QStringLiteral("ROI 选择失败"),
                QStringLiteral("%1 · stage=%2 · native=%3")
                    .arg(QString::fromLatin1(pbscreenregion::GetScreenRegionErrorName(status.code)))
                    .arg(static_cast<unsigned int>(status.stage)).arg(status.nativeError));
            return;
        }
        if (!protectedDeviceName.isEmpty())
        {
            pbapp::MonitorSafetySelection safetySelection;
            const pbapp::MonitorSafetyStatus safety = pbapp::ResolveMonitorSafetySelection(
                protectedDeviceName.toStdWString(), experimentMonitor.deviceName, safetySelection);
            const pbapp::MonitorSafetyStatus targetSafety = safety ?
                pbapp::ValidateMonitorSafetyTarget(safetySelection, target, selected.monitor) : safety;
            if (!targetSafety)
            {
                QMessageBox::warning(this, QStringLiteral("ROI 选择失败"),
                    QString::fromLatin1(pbapp::GetMonitorSafetyErrorName(targetSafety.code)));
                return;
            }
            monitorSafetySelection_ = safetySelection;
        }
        else
        {
            monitorSafetySelection_.reset();
        }
        region_ = selected;
        hasRegion_ = true;
        UpdateRoiDetails();
    }

    void ClearRoi()
    {
        region_ = {};
        hasRegion_ = false;
        monitorSafetySelection_.reset();
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
        const bool diagnosticCaptureOnlyReady = !compatible && channelCombo_->currentIndex() == 1 &&
            static_cast<pbapp::VisualProfile>(profileCombo_->currentData().toInt()) ==
                pbapp::VisualProfile::RemoteVisualResilient && replayCheck_->isChecked() &&
            !replayPathEdit_->text().isEmpty();
        roiDetailsLabel_->setText(QStringLiteral("Physical RECT: X=%1 Y=%2 · %3x%4 · scale estimate %5x%6 · DPI %7x%8 · rotation %9 · %10")
            .arg(region_.physicalRect.left).arg(region_.physicalRect.top).arg(width).arg(height)
            .arg(static_cast<double>(width) / pbapp::phase1CanvasWidth, 0, 'f', 6)
            .arg(static_cast<double>(height) / pbapp::phase1CanvasHeight, 0, 'f', 6)
            .arg(region_.dpiX).arg(region_.dpiY).arg(RotationText(region_.rotation))
            .arg(compatible ? QStringLiteral("✓ strict Phase-1 1:1 geometry compatible") :
                diagnosticCaptureOnlyReady ?
                    QStringLiteral("△ incompatible; bounded replay capture-only is enabled; Bootstrap/demod/FEC/Receiver/publish are disabled") :
                    QStringLiteral("✕ incompatible; enable RemoteVisual Resilient + receiver-only Replay to capture diagnostics, or select exact 1920x1080; no silent resize")));
        roiDetailsLabel_->setStyleSheet(compatible ? QStringLiteral("color:#168a52;") :
            diagnosticCaptureOnlyReady ? QStringLiteral("color:#b54708;") : QStringLiteral("color:#b42318;"));
        reselectRoiButton_->setEnabled(!controller_.IsActive());
        clearRoiButton_->setEnabled(!controller_.IsActive());
        fullMonitorButton_->setEnabled(!controller_.IsActive());
        UpdateActionButtons();
    }

    void StartReceiving()
    {
        pbapp::DecoderConfig config;
        QString validationError;
        if (!BuildDecoderConfig(true, config, validationError))
        {
            QMessageBox::warning(this, QStringLiteral("无法开始接收"), validationError);
            return;
        }
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
        const QString telemetryText = QStringLiteral(
            "PBTelemetry current epoch: CaptureFPS=%1 · captured/drop=%2/%3 · BootstrapSuccessRate=%4 (%5/%6) · PreFecBER=%7 · FER=%8\n"
            "UniqueVisualFPS=%9 · EndToEndUniqueVisualFPS=%82 · ROI pixel fingerprint frames/FPS=%10/%83 "
            "(生产 D3D11 fast path 不做 full-ROI raw-pixel readback/digest；该诊断严格不可用)\n"
            "FrameSequence diagnostics（不是 UniqueVisualFPS）: admitted FPS=%11 · gap/skipped/duplicate/reordered=%12/%13/%14/%15\n"
            "Capture component: epoch=%16 resets=%17 recreates=%18 deviceRecoveries=%19 · arrived/delivered/drop=%20/%21/%22 · "
            "Bootstrap LocalDesktop accepted/erasure/mismatch/controlFailure=%23/%24/%25/%26\n"
            "FEC: failed/evaluated=%27/%28 · coded errors/bits=%29/%30 · FEC/CRC/identity/falseAccept=%31/%32/%33/%34 · accepted outer blocks=%35\n"
            "HWM Lease/Demod/Result=%36/%37/%38 · stale=%39 · ROI/Demod GPU=%40/%41 100ns · Bootstrap/Post GPU CPU=%42/%43 100ns · "
            "verified encoded=%44 B · Digest=%45 · Publish=%46\n"
            "Replay v2 diagnostic-only: enabled/valid/finalized=%47/%48/%49 · written/drop=%50/%51 · observations=%52/%53 · queueHWM=%54 · bytes=%55\n"
            "RemoteVisual metric confidence（telemetry-only）: frames/samples/zero=%56/%57/%58 · zeroRate=%59 · minAbs=%60 · meanAbs=%61 · "
            "same-sequence retry attempts/recoveries=%62/%63\n"
            "Stall（1 s threshold）: capture count/total/max/active=%64/%65/%66/%67 · visual=%68/%69/%70/%71\n"
            "Offline Replay production path: mode=%72 · capture/demod=%73/%74 · recorded-observation compare/mismatch=%75/%76\n"
            "RemoteVisual verified/rejected metric frames=%77/%78 · verifiedMeanAbs=%79 · rejectedMeanAbs=%80 · rejectedZeroRate=%81 "
            "（是否 high-confidence-wrong 仍需 Replay truth，生产 run 不伪报）")
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
            .arg(snapshot.crcFailures).arg(snapshot.identityFailures)
            .arg(snapshot.falseAcceptedCodewordsAvailable ? QString::number(snapshot.falseAcceptedCodewords) :
                QStringLiteral("— (truth unavailable)"))
            .arg(snapshot.acceptedTransportBlocks).arg(snapshot.frameLeaseHighWater).arg(snapshot.demodPendingHighWater)
            .arg(snapshot.resultQueueHighWater).arg(snapshot.staleResultDrops).arg(snapshot.roiGpuTimeTotal100ns)
            .arg(snapshot.demodGpuTimeTotal100ns).arg(snapshot.bootstrapCpuTimeTotal100ns)
            .arg(snapshot.postGpuFecCpuTimeTotal100ns).arg(snapshot.verifiedEncodedBytes)
            .arg(snapshot.wholeFileDigestVerified ? QStringLiteral("PASS") : QStringLiteral("not accepted"))
            .arg(snapshot.finalPublishSucceeded ? QStringLiteral("success") : QStringLiteral("not published"))
            .arg(snapshot.replayEnabled).arg(snapshot.replayEvidenceValid).arg(snapshot.replayFinalized)
            .arg(snapshot.replayWrittenFrames).arg(snapshot.replayDroppedFrames)
            .arg(snapshot.replayWrittenDemodObservations).arg(snapshot.replayDroppedDemodObservations)
            .arg(snapshot.replayQueueHighWater).arg(snapshot.replayFileBytes)
            .arg(snapshot.remoteMetricFrames).arg(snapshot.remoteMetricSamples)
            .arg(snapshot.remoteZeroMagnitudeMetrics).arg(OptionalMetric(snapshot.remoteZeroMagnitudeMetricRate, 6))
            .arg(OptionalMetric(snapshot.remoteMinimumAbsoluteMetric, 6))
            .arg(OptionalMetric(snapshot.remoteMeanAbsoluteMetric, 6))
            .arg(snapshot.remoteDuplicateRefinementAttempts).arg(snapshot.remoteDuplicateRefinementRecoveries)
            .arg(snapshot.captureStallCount).arg(snapshot.captureStallTotalMilliseconds)
            .arg(snapshot.captureStallMaximumMilliseconds).arg(snapshot.captureStallActive)
            .arg(snapshot.visualStallCount).arg(snapshot.visualStallTotalMilliseconds)
            .arg(snapshot.visualStallMaximumMilliseconds).arg(snapshot.visualStallActive)
            .arg(snapshot.replayOfflineMode).arg(snapshot.replayOfflineCaptureFrames)
            .arg(snapshot.replayOfflineDemodResults).arg(snapshot.replayOfflineObservationComparisons)
            .arg(snapshot.replayOfflineObservationMismatches)
            .arg(snapshot.remoteVerifiedMetricFrames).arg(snapshot.remoteRejectedMetricFrames)
            .arg(OptionalMetric(snapshot.remoteVerifiedMeanAbsoluteMetric, 6))
            .arg(OptionalMetric(snapshot.remoteRejectedMeanAbsoluteMetric, 6))
            .arg(OptionalMetric(snapshot.remoteRejectedZeroMagnitudeMetricRate, 6))
            .arg(OptionalMetric(snapshot.endToEndUniqueVisualFps))
            .arg(OptionalMetric(snapshot.roiPixelDigestUniqueVisualFps));
        telemetryLabel_->setText(telemetryText + QStringLiteral(
            "\nReplay capture-only=%1（true 时绝不运行 Bootstrap/demod/FEC/Receiver/publish）"
            "\nOuter admission: unique/identical-duplicate/ready-duplicate/completed=%19/%20/%21/%22 · "
            "recovery-ready=%23 · resource/conflict rejection=%24/%25"
            "\nCapture detail: copied=%9 · acquireTimeout/pointerOnly/accumulated=%10/%11/%12 · "
            "accessLost/expired/stale/cursorErase=%13/%14/%15/%16 · ageHWM=%17 100ns · replayReadbackDrop=%18"
            "\nRemoteVisual freshness tags: observed/stale regions=%2/%3 · staleRate=%4 · framesWithStale=%5 · "
            "tag mismatch/erasure=%6/%7 · data metrics erased before LDPC=%8")
            .arg(snapshot.replayCaptureOnly).arg(snapshot.remoteFreshnessRegions).arg(snapshot.remoteStaleRegions)
            .arg(OptionalMetric(snapshot.remoteStaleRegionRate, 6)).arg(snapshot.remoteFramesWithStaleRegions)
            .arg(snapshot.remoteFreshnessTagMismatches).arg(snapshot.remoteFreshnessTagErasures)
            .arg(snapshot.remoteFreshnessErasedDataMetrics).arg(snapshot.captureCopiedFrames)
            .arg(snapshot.captureAcquireTimeouts).arg(snapshot.capturePointerOnlyFrames)
            .arg(snapshot.captureAccumulatedFrames).arg(snapshot.captureAccessLostEvents)
            .arg(snapshot.captureExpiredFrames).arg(snapshot.captureStaleFrames)
            .arg(snapshot.captureCursorErasures).arg(snapshot.captureFrameAgeHighWater100ns)
            .arg(snapshot.captureReadbackDropEvents).arg(snapshot.outerUniqueSymbols)
            .arg(snapshot.outerIdenticalDuplicateSymbols).arg(snapshot.outerRecoveryAlreadyReadySymbols)
            .arg(snapshot.outerAlreadyCompletedSymbols).arg(snapshot.outerRecoveryReadyEvents)
            .arg(snapshot.outerResourceRejections).arg(snapshot.outerConflictRejections));
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
        remoteProviderEdit_->setEnabled(enabled);
        metadataPresetPathEdit_->setEnabled(enabled);
        chooseMetadataPresetButton_->setEnabled(enabled);
        clearMetadataPresetButton_->setEnabled(enabled && !metadataPresetPathEdit_->text().isEmpty());
        protectedMonitorCombo_->setEnabled(enabled);
        networkNoteEdit_->setEnabled(enabled);
        runIdEdit_->setEnabled(enabled);
        replayCheck_->setEnabled(enabled);
        replayPathEdit_->setEnabled(enabled);
        chooseReplayButton_->setEnabled(enabled);
    }

    void UpdateActionButtons()
    {
        pbapp::DecoderConfig config;
        QString validationError;
        const bool valid = BuildDecoderConfig(false, config, validationError);
        const bool active = controller_.IsActive();
        startButton_->setEnabled(valid && !active);
        stopButton_->setEnabled(active);
    }

    [[nodiscard]] bool BuildDecoderConfig(const bool validateWindow, pbapp::DecoderConfig& config,
        QString& validationError)
    {
        if (!hasRegion_)
        {
            validationError = QStringLiteral("尚未选择 physical ROI");
            return false;
        }
        config.outputDirectory = outputDirectoryEdit_->text().toStdWString();
        config.captureBackend = static_cast<pbapp::CaptureBackend>(backendCombo_->currentData().toInt());
        config.visualProfile = static_cast<pbapp::VisualProfile>(profileCombo_->currentData().toInt());
        config.region = region_;
        config.runId = runIdEdit_->text().toLatin1().toStdString();
        config.remoteMetadata.channelType = channelCombo_->currentIndex() == 0 ? pbapp::ChannelType::LocalDesktop :
            channelCombo_->currentIndex() == 1 ? pbapp::ChannelType::RemoteVisual : pbapp::ChannelType::Other;
        const QString metadataPresetPath = metadataPresetPathEdit_->text().trimmed();
        if (!metadataPresetPath.isEmpty())
        {
            if (config.remoteMetadata.channelType != pbapp::ChannelType::RemoteVisual)
            {
                validationError = QStringLiteral("RemoteVisual metadata preset 只能用于 RemoteVisual channel");
                return false;
            }
            if (!pbapp::LoadRemoteVisualMetadataPreset(metadataPresetPath, config.remoteMetadata, validationError))
            {
                return false;
            }
            if (!config.runId.empty() && config.runId != config.remoteMetadata.runId)
            {
                validationError = QStringLiteral("Shared RunId 与 metadata preset 不一致");
                return false;
            }
            config.runId = config.remoteMetadata.runId;
        }
        const QString provider = remoteProviderEdit_->text().trimmed();
        if (config.remoteMetadata.channelType == pbapp::ChannelType::RemoteVisual)
        {
            if (!provider.isEmpty())
            {
                const std::string providerUtf8 = provider.toUtf8().toStdString();
                if (!config.remoteMetadata.remoteProvider.empty() && config.remoteMetadata.remoteProvider != providerUtf8)
                {
                    validationError = QStringLiteral("Remote provider 与 metadata preset 不一致");
                    return false;
                }
                config.remoteMetadata.remoteProvider = providerUtf8;
            }
            if (config.remoteMetadata.remoteProvider.empty())
            {
                validationError = QStringLiteral("RemoteVisual 必须提供 provider 或有效 metadata preset");
                return false;
            }
        }
        else
        {
            config.remoteMetadata.remoteProvider.clear();
        }
        config.remoteMetadata.runId = config.runId;
        const QString networkNote = networkNoteEdit_->text();
        if (!networkNote.isEmpty())
        {
            config.remoteMetadata.notes = networkNote.toUtf8().toStdString();
            config.remoteMetadata.networkProvenance = pbapp::MetadataProvenance::Manual;
        }
        config.remoteMetadata.selectedRoiPhysicalRect = pbapp::MetadataPhysicalRect{region_.physicalRect.left,
            region_.physicalRect.top, region_.physicalRect.right, region_.physicalRect.bottom};
        config.remoteMetadata.geometryProvenance = pbapp::MetadataProvenance::PixelBridgeObserved;
        const std::int64_t width = static_cast<std::int64_t>(region_.physicalRect.right) - region_.physicalRect.left;
        const std::int64_t height = static_cast<std::int64_t>(region_.physicalRect.bottom) - region_.physicalRect.top;
        const bool strictGeometry = width == pbapp::phase1CanvasWidth && height == pbapp::phase1CanvasHeight &&
            region_.rotation == DXGI_MODE_ROTATION_IDENTITY;
        config.remoteMetadata.estimatedScaleX = static_cast<double>(width) / pbapp::phase1CanvasWidth;
        config.remoteMetadata.estimatedScaleY = static_cast<double>(height) / pbapp::phase1CanvasHeight;
        config.remoteMetadata.letterboxStatus = "Unknown";
        config.remoteMetadata.cropStatus = "Unknown";
        if (replayCheck_->isChecked())
        {
            config.replayOutputPath = replayPathEdit_->text().toStdWString();
        }
        config.diagnosticCaptureOnly = !strictGeometry && !config.replayOutputPath.empty() &&
            config.remoteMetadata.channelType == pbapp::ChannelType::RemoteVisual &&
            config.visualProfile == pbapp::VisualProfile::RemoteVisualResilient;
        config.remoteMetadata.geometryStatus = strictGeometry ? "CompatibleStrictPhysical1:1" :
            config.diagnosticCaptureOnly ? "DiagnosticOnlyIncompatibleROI; no resampling/decode/publish" :
            "IncompatiblePhysicalROI; no resampling permitted";

        MONITORINFOEXW monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (GetMonitorInfoW(region_.monitor, &monitorInfo) == FALSE)
        {
            validationError = QStringLiteral("无法读取 ROI 所在显示器 identity");
            return false;
        }
        config.remoteMetadata.experimentMonitorIdentity =
            QString::fromWCharArray(monitorInfo.szDevice).toUtf8().toStdString();
        config.remoteMetadata.computerADisplayResolution = std::to_string(
            monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left) + "x" +
            std::to_string(monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top);

        if (config.remoteMetadata.channelType == pbapp::ChannelType::RemoteVisual)
        {
            if (protectedMonitorCombo_->currentIndex() <= 0)
            {
                validationError = QStringLiteral("请显式选择左侧 ProtectedMonitor；ROI 所在显示器将作为 ExperimentMonitor");
                return false;
            }
            const QString protectedDeviceName = protectedMonitorCombo_->currentData().toString();
            pbapp::MonitorSafetySelection safetySelection;
            pbapp::MonitorSafetyStatus safety;
            if (!validateWindow && monitorSafetySelection_ &&
                monitorSafetySelection_->protectedMonitor.deviceName == protectedDeviceName.toStdWString() &&
                monitorSafetySelection_->experimentMonitor.monitor == region_.monitor)
            {
                safetySelection = *monitorSafetySelection_;
                safety = pbapp::ValidateMonitorSafetyTarget(safetySelection, region_.physicalRect, region_.monitor);
            }
            else
            {
                safety = pbapp::ResolveMonitorSafetySelection(
                    protectedDeviceName.toStdWString(), monitorInfo.szDevice, safetySelection);
            }
            const pbapp::MonitorSafetyStatus targetSafety = safety ? pbapp::ValidateMonitorSafetyTarget(
                safetySelection, region_.physicalRect, region_.monitor) : safety;
            if (!targetSafety)
            {
                validationError = QStringLiteral("ROI/monitor safety preflight failed: %1")
                    .arg(QString::fromLatin1(pbapp::GetMonitorSafetyErrorName(targetSafety.code)));
                return false;
            }
            monitorSafetySelection_ = safetySelection;
            if (validateWindow)
            {
                RECT windowRect{};
                const HWND windowHandle = reinterpret_cast<HWND>(winId());
                const HMONITOR windowMonitor = MonitorFromWindow(windowHandle, MONITOR_DEFAULTTONULL);
                const pbapp::MonitorSafetyStatus windowSafety = GetWindowRect(windowHandle, &windowRect) == FALSE ?
                    pbapp::MonitorSafetyStatus{pbapp::MonitorSafetyError::TargetOutsideExperimentMonitor, {}} :
                    pbapp::ValidateMonitorSafetyTarget(safetySelection, windowRect, windowMonitor);
                if (!windowSafety)
                {
                    validationError = QStringLiteral(
                        "Decoder 窗口必须完全位于右侧 ExperimentMonitor 且不得接触 ProtectedMonitor；请手动移动后重试。preflight=%1")
                        .arg(QString::fromLatin1(pbapp::GetMonitorSafetyErrorName(windowSafety.code)));
                    return false;
                }
            }
            config.monitorSafety = safetySelection;
            config.remoteMetadata.protectedMonitorIdentity = protectedDeviceName.toUtf8().toStdString();
            config.remoteMetadata.experimentMonitorIdentity =
                QString::fromWCharArray(monitorInfo.szDevice).toUtf8().toStdString();
            config.remoteMetadata.computerARefreshRate = safetySelection.experimentMonitor.refreshRate;
        }
        const pbapp::RuntimeStatus status = pbapp::ValidateDecoderConfig(config);
        if (!status)
        {
            validationError = FromUtf8(status.message);
            return false;
        }
        validationError.clear();
        return true;
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
    QLineEdit* remoteProviderEdit_ = nullptr;
    QLineEdit* metadataPresetPathEdit_ = nullptr;
    QPushButton* chooseMetadataPresetButton_ = nullptr;
    QPushButton* clearMetadataPresetButton_ = nullptr;
    QComboBox* protectedMonitorCombo_ = nullptr;
    QLineEdit* networkNoteEdit_ = nullptr;
    QLineEdit* runIdEdit_ = nullptr;
    QCheckBox* replayCheck_ = nullptr;
    QLineEdit* replayPathEdit_ = nullptr;
    QPushButton* chooseReplayButton_ = nullptr;
    QPlainTextEdit* logEdit_ = nullptr;
    pbscreenregion::ScreenCaptureRegion region_;
    std::optional<pbapp::MonitorSafetySelection> monitorSafetySelection_;
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
    if (!smoke)
    {
        window.show();
    }
    if (smoke)
    {
        QTimer::singleShot(350, &application, &QCoreApplication::quit);
    }
    return application.exec();
}
