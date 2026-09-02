#include "encoder_gui.h"

#include "encoder_application_controller.h"

#include "monitor_catalog.h"
#include "remote_visual_metadata_preset_qt.h"
#include "run_report.h"
#include "pbcore/build_info.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
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
#include <QSpinBox>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstdint>
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
    const int precision = unit == 0 ? 0 : 2;
    return QStringLiteral("%1 %2").arg(value, 0, 'f', precision).arg(units[unit]);
}

[[nodiscard]] QString DurationText(const std::uint64_t milliseconds)
{
    const std::uint64_t totalSeconds = milliseconds / 1000;
    const std::uint64_t hours = totalSeconds / 3600;
    const std::uint64_t minutes = (totalSeconds / 60) % 60;
    const std::uint64_t seconds = totalSeconds % 60;
    return QStringLiteral("%1:%2:%3").arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0')).arg(seconds, 2, 10, QLatin1Char('0'));
}

[[nodiscard]] QString RateText(const std::optional<double>& bytesPerSecond)
{
    if (!bytesPerSecond)
    {
        return QStringLiteral("—");
    }
    if (!(*bytesPerSecond > 0) || !std::isfinite(*bytesPerSecond))
    {
        return QStringLiteral("0 B/s");
    }
    return QStringLiteral("%1 MiB/s (%2 Mbps)")
        .arg(*bytesPerSecond / (1024.0 * 1024.0), 0, 'f', 2)
        .arg(*bytesPerSecond * 8.0 / 1000000.0, 0, 'f', 2);
}

[[nodiscard]] bool PlaceOnExperimentMonitor(QWidget& window, const std::wstring_view deviceName) noexcept
{
    std::vector<pbapp::MonitorInfo> monitors;
    if (!pbapp::EnumerateMonitors(monitors))
    {
        return false;
    }
    const auto match = std::find_if(monitors.begin(), monitors.end(), [deviceName](const pbapp::MonitorInfo& monitor)
    {
        return monitor.deviceName == deviceName;
    });
    if (match == monitors.end())
    {
        return false;
    }
    window.setAttribute(Qt::WA_ShowWithoutActivating);
    const HWND windowHandle = reinterpret_cast<HWND>(window.winId());
    RECT current{};
    const std::int64_t availableWidth = static_cast<std::int64_t>(match->workRect.right) - match->workRect.left;
    const std::int64_t availableHeight = static_cast<std::int64_t>(match->workRect.bottom) - match->workRect.top;
    if (windowHandle == nullptr || GetWindowRect(windowHandle, &current) == FALSE ||
        availableWidth <= 48 || availableHeight <= 48)
    {
        return false;
    }
    const std::int64_t currentWidth = static_cast<std::int64_t>(current.right) - current.left;
    const std::int64_t currentHeight = static_cast<std::int64_t>(current.bottom) - current.top;
    if (currentWidth <= 0 || currentHeight <= 0)
    {
        return false;
    }
    const int width = static_cast<int>((std::min)(currentWidth, availableWidth - 48));
    const int height = static_cast<int>((std::min)(currentHeight, availableHeight - 48));
    return SetWindowPos(windowHandle, nullptr, match->workRect.left + 24, match->workRect.top + 24,
        width, height, SWP_NOACTIVATE | SWP_NOZORDER) != FALSE;
}

[[nodiscard]] QString StateColor(const pbapp::EncoderState state)
{
    if (state == pbapp::EncoderState::Broadcasting)
    {
        return QStringLiteral("#168a52");
    }
    if (state == pbapp::EncoderState::Failed)
    {
        return QStringLiteral("#b42318");
    }
    if (state == pbapp::EncoderState::Preparing || state == pbapp::EncoderState::Stopping)
    {
        return QStringLiteral("#b06800");
    }
    return QStringLiteral("#475467");
}

class EncoderWindow final : public QMainWindow
{
public:
    EncoderWindow()
    {
        setWindowTitle(QStringLiteral("PixelBridge Encoder"));
        setMinimumSize(960, 720);
        BuildUi();
        LoadSettings();
        RefreshMonitors();
        connect(&controller_, &EncoderApplicationController::SnapshotChanged, this,
            &EncoderWindow::UpdateSnapshot);
        connect(&controller_, &EncoderApplicationController::TerminalStateReached, this,
            &EncoderWindow::HandleTerminalState);
        UpdateFileDetails();
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
                statusMessageLabel_->setText(QStringLiteral("正在安全停止广播并回收 Data Window…"));
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
        auto* const title = new QLabel(QStringLiteral("PixelBridge Encoder"));
        QFont titleFont = title->font();
        titleFont.setPointSize(titleFont.pointSize() + 7);
        titleFont.setBold(true);
        title->setFont(titleFont);
        auto* const mode = new QLabel(QStringLiteral("Instant LocalDesktop · Encoder broadcasts; Decoder converges"));
        mode->setStyleSheet(QStringLiteral("color:#667085;"));
        titleColumn->addWidget(title);
        titleColumn->addWidget(mode);
        stateLabel_ = new QLabel(QStringLiteral("Idle"));
        stateLabel_->setAlignment(Qt::AlignCenter);
        stateLabel_->setMinimumWidth(150);
        stateLabel_->setStyleSheet(QStringLiteral("padding:8px 14px;border-radius:6px;background:#475467;color:white;font-weight:600;"));
        header->addLayout(titleColumn, 1);
        header->addWidget(stateLabel_);
        root->addLayout(header);

        auto* const settingsRow = new QHBoxLayout();
        settingsRow->setSpacing(12);
        auto* const sourceGroup = new QGroupBox(QStringLiteral("源文件"));
        auto* const sourceLayout = new QGridLayout(sourceGroup);
        sourcePathEdit_ = new QLineEdit();
        sourcePathEdit_->setReadOnly(true);
        sourcePathEdit_->setPlaceholderText(QStringLiteral("当前 production path：1 byte–8 MiB，单 Segment"));
        chooseFileButton_ = new QPushButton(QStringLiteral("选择文件…"));
        fileDetailsLabel_ = new QLabel(QStringLiteral("未选择文件"));
        fileDetailsLabel_->setWordWrap(true);
        validationLabel_ = new QLabel();
        validationLabel_->setWordWrap(true);
        sourceLayout->addWidget(sourcePathEdit_, 0, 0);
        sourceLayout->addWidget(chooseFileButton_, 0, 1);
        sourceLayout->addWidget(fileDetailsLabel_, 1, 0, 1, 2);
        sourceLayout->addWidget(validationLabel_, 2, 0, 1, 2);
        connect(chooseFileButton_, &QPushButton::clicked, this, &EncoderWindow::ChooseFile);
        settingsRow->addWidget(sourceGroup, 3);

        auto* const compressionGroup = new QGroupBox(QStringLiteral("Segment 压缩"));
        auto* const compressionLayout = new QVBoxLayout(compressionGroup);
        compressionCheck_ = new QCheckBox(QStringLiteral("启用 Segment 压缩"));
        compressionCheck_->setChecked(false);
        auto* const compressionAdvice = new QLabel(QStringLiteral(
            "默认关闭。对 .7z/.rar/.zip/.gz/.zst 及多数视频/图片通常建议关闭；启用后使用现有 zstd/RAW fallback。"));
        compressionAdvice->setWordWrap(true);
        compressionAdvice->setStyleSheet(QStringLiteral("color:#667085;"));
        compressionLayout->addWidget(compressionCheck_);
        compressionLayout->addWidget(compressionAdvice);
        connect(compressionCheck_, &QCheckBox::toggled, this, &EncoderWindow::UpdateFileDetails);
        settingsRow->addWidget(compressionGroup, 2);
        root->addLayout(settingsRow);

        auto* const bindingGroup = new QGroupBox(QStringLiteral("Profile / FEC / 输出显示器"));
        auto* const bindingLayout = new QGridLayout(bindingGroup);
        profileCombo_ = new QComboBox();
        for (const pbapp::VisualProfileOption& option : pbapp::GetVisualProfileOptions())
        {
            profileCombo_->addItem(QString::fromUtf8(option.displayName.data(),
                static_cast<int>(option.displayName.size())), static_cast<int>(option.profile));
        }
        profileCombo_->setToolTip(QStringLiteral(
            "旧 remote 与新的 remote-lf4 是不同 wire identity。LF4 使用 4x4 Walsh tile、四 codeword、区域 freshness soft erasure 和 1..5 Hz 稳定驻留；仍为 Experimental。"));
        outerFecLabel_ = new QLabel(QStringLiteral("Automatic: DirectRepeat or Wirehair V2"));
        outerFecLabel_->setToolTip(QStringLiteral("由已编码 Segment 大小和现有 ChooseOuterFecMode 决定；不提供非法 override。"));
        auto* const innerFecLabel = new QLabel(QStringLiteral("Robust DVB-S2 Short QC-LDPC (fixed)"));
        monitorCombo_ = new QComboBox();
        protectedMonitorCombo_ = new QComboBox();
        protectedMonitorCombo_->addItem(QStringLiteral("LF4：请选择 ProtectedMonitor（不得显示 Data Window）"), -1);
        protectedMonitorCombo_->setEnabled(false);
        refreshMonitorButton_ = new QPushButton(QStringLiteral("刷新"));
        monitorInfoLabel_ = new QLabel();
        monitorInfoLabel_->setWordWrap(true);
        bindingLayout->addWidget(new QLabel(QStringLiteral("Visual Profile")), 0, 0);
        bindingLayout->addWidget(profileCombo_, 0, 1);
        bindingLayout->addWidget(new QLabel(QStringLiteral("Outer FEC")), 0, 2);
        bindingLayout->addWidget(outerFecLabel_, 0, 3);
        bindingLayout->addWidget(new QLabel(QStringLiteral("Inner FEC")), 1, 0);
        bindingLayout->addWidget(innerFecLabel, 1, 1);
        bindingLayout->addWidget(new QLabel(QStringLiteral("目标 monitor")), 1, 2);
        bindingLayout->addWidget(monitorCombo_, 1, 3);
        bindingLayout->addWidget(refreshMonitorButton_, 1, 4);
        bindingLayout->addWidget(new QLabel(QStringLiteral("ProtectedMonitor")), 2, 0);
        bindingLayout->addWidget(protectedMonitorCombo_, 2, 1, 1, 4);
        bindingLayout->addWidget(monitorInfoLabel_, 3, 0, 1, 5);
        connect(refreshMonitorButton_, &QPushButton::clicked, this, &EncoderWindow::RefreshMonitors);
        connect(monitorCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &EncoderWindow::UpdateMonitorDetails);
        connect(protectedMonitorCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &EncoderWindow::UpdateMonitorDetails);
        root->addWidget(bindingGroup);

        auto* const actionRow = new QHBoxLayout();
        startButton_ = new QPushButton(QStringLiteral("开始广播"));
        stopButton_ = new QPushButton(QStringLiteral("停止广播"));
        startButton_->setMinimumHeight(40);
        stopButton_->setMinimumHeight(40);
        startButton_->setStyleSheet(QStringLiteral("font-weight:600;background:#1570ef;color:white;padding:8px 22px;border-radius:5px;"));
        stopButton_->setEnabled(false);
        actionRow->addWidget(startButton_);
        actionRow->addWidget(stopButton_);
        actionRow->addStretch(1);
        auto* const semantics = new QLabel(QStringLiteral("不会自然“发送完成”；广播持续到用户 Stop。接收完成请查看 Decoder。"));
        semantics->setStyleSheet(QStringLiteral("color:#b54708;font-weight:600;"));
        actionRow->addWidget(semantics);
        connect(startButton_, &QPushButton::clicked, this, &EncoderWindow::StartBroadcast);
        connect(stopButton_, &QPushButton::clicked, &controller_, &EncoderApplicationController::RequestStop);
        root->addLayout(actionRow);

        auto* const statusGroup = new QGroupBox(QStringLiteral("Broadcast Status"));
        auto* const statusLayout = new QGridLayout(statusGroup);
        runtimeLabel_ = new QLabel(QStringLiteral("00:00:00"));
        sessionLabel_ = new QLabel(QStringLiteral("—"));
        frameLabel_ = new QLabel(QStringLiteral("0"));
        fpsLabel_ = new QLabel(QStringLiteral("Presented — / PresentCall —"));
        rateLabel_ = new QLabel(QStringLiteral("0 B/s"));
        cycleProgress_ = new QProgressBar();
        cycleProgress_->setRange(0, 10000);
        cycleProgress_->setValue(0);
        cycleProgress_->setFormat(QStringLiteral("当前轮播周期位置 %p%"));
        cycleProgress_->setToolTip(QStringLiteral("此百分比不是接收端文件恢复进度；到 100% 后开始下一轮广播。"));
        cycleLabel_ = new QLabel(QStringLiteral("Cycle 0 · frame 0/0"));
        statusMessageLabel_ = new QLabel(QStringLiteral("Idle"));
        statusMessageLabel_->setWordWrap(true);
        statusLayout->addWidget(new QLabel(QStringLiteral("广播运行时间")), 0, 0);
        statusLayout->addWidget(runtimeLabel_, 0, 1);
        statusLayout->addWidget(new QLabel(QStringLiteral("Session identity")), 0, 2);
        statusLayout->addWidget(sessionLabel_, 0, 3);
        statusLayout->addWidget(new QLabel(QStringLiteral("FrameSequence")), 1, 0);
        statusLayout->addWidget(frameLabel_, 1, 1);
        statusLayout->addWidget(new QLabel(QStringLiteral("真实 Present 指标")), 1, 2);
        statusLayout->addWidget(fpsLabel_, 1, 3);
        statusLayout->addWidget(new QLabel(QStringLiteral("Generated Payload Rate")), 2, 0);
        statusLayout->addWidget(rateLabel_, 2, 1);
        statusLayout->addWidget(cycleLabel_, 2, 2, 1, 2);
        statusLayout->addWidget(cycleProgress_, 3, 0, 1, 4);
        statusLayout->addWidget(statusMessageLabel_, 4, 0, 1, 4);
        root->addWidget(statusGroup);

        advancedGroup_ = new QGroupBox(QStringLiteral("Advanced / Telemetry / RemoteVisual metadata"));
        advancedGroup_->setCheckable(true);
        advancedGroup_->setChecked(false);
        auto* const advancedLayout = new QGridLayout(advancedGroup_);
        telemetryLabel_ = new QLabel(QStringLiteral("No runtime telemetry"));
        telemetryLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        telemetryLabel_->setWordWrap(true);
        compressionLevel_ = new QSpinBox();
        compressionLevel_->setRange(1, 22);
        compressionLevel_->setValue(3);
        compressionLevel_->setToolTip(QStringLiteral("现有 zstd local tuning，范围 1..22；不写入 wire。"));
        logicalFpsSpin_ = new QSpinBox();
        logicalFpsSpin_->setRange(0, 240);
        logicalFpsSpin_->setValue(0);
        logicalFpsSpin_->setSpecialValueText(QStringLiteral("Presentation-driven"));
        logicalFpsSpin_->setSuffix(QStringLiteral(" fps"));
        logicalFpsSpin_->setToolTip(QStringLiteral(
            "限制完整逻辑 raster 的更新频率，使远程视频编码器获得稳定驻留时间；RemoteVisual 强制 1..5 fps，默认 2 fps（500 ms），0 仅保留给 LocalDesktop。"));
        controlRepetitionsSpin_ = new QSpinBox();
        controlRepetitionsSpin_->setRange(1, 64);
        controlRepetitionsSpin_->setValue(4);
        controlRepetitionsSpin_->setToolTip(QStringLiteral(
            "每个 Carousel 中 Session/Manifest/Segment Control 各自的重复次数；不改变 Control wire。"));
        channelCombo_ = new QComboBox();
        channelCombo_->addItems({QStringLiteral("LocalDesktop"), QStringLiteral("RemoteVisual (provider-agnostic)"),
            QStringLiteral("Other")});
        remoteProviderEdit_ = new QLineEdit();
        remoteProviderEdit_->setMaxLength(128);
        remoteProviderEdit_->setPlaceholderText(QStringLiteral("当前远控软件名称；仅 metadata，不选择阈值"));
        networkNoteEdit_ = new QLineEdit();
        networkNoteEdit_->setPlaceholderText(QStringLiteral("仅 run metadata；不影响 CRC/FEC/digest acceptance"));
        runIdEdit_ = new QLineEdit();
        runIdEdit_->setMaxLength(32);
        runIdEdit_->setPlaceholderText(QStringLiteral("可选：两端共享的 32 字符 lowercase hex RunId"));
        metadataPresetPathEdit_ = new QLineEdit();
        metadataPresetPathEdit_->setReadOnly(true);
        metadataPresetPathEdit_->setPlaceholderText(QStringLiteral("可选：两端共享的 PixelBridge.RemoteVisualRunMetadata.1 JSON"));
        chooseMetadataPresetButton_ = new QPushButton(QStringLiteral("选择 Metadata…"));
        clearMetadataPresetButton_ = new QPushButton(QStringLiteral("清除"));
        advancedLayout->addWidget(new QLabel(QStringLiteral("Compression level")), 0, 0);
        advancedLayout->addWidget(compressionLevel_, 0, 1);
        advancedLayout->addWidget(new QLabel(QStringLiteral("ChannelType")), 0, 2);
        advancedLayout->addWidget(channelCombo_, 0, 3);
        advancedLayout->addWidget(new QLabel(QStringLiteral("Logical Visual FPS")), 1, 0);
        advancedLayout->addWidget(logicalFpsSpin_, 1, 1);
        advancedLayout->addWidget(new QLabel(QStringLiteral("Control repetitions")), 1, 2);
        advancedLayout->addWidget(controlRepetitionsSpin_, 1, 3);
        advancedLayout->addWidget(new QLabel(QStringLiteral("Shared RunId")), 2, 0);
        advancedLayout->addWidget(runIdEdit_, 2, 1, 1, 3);
        advancedLayout->addWidget(new QLabel(QStringLiteral("Network note")), 3, 0);
        advancedLayout->addWidget(networkNoteEdit_, 3, 1, 1, 3);
        advancedLayout->addWidget(new QLabel(QStringLiteral("Remote provider")), 4, 0);
        advancedLayout->addWidget(remoteProviderEdit_, 4, 1, 1, 3);
        advancedLayout->addWidget(new QLabel(QStringLiteral("Metadata preset")), 5, 0);
        advancedLayout->addWidget(metadataPresetPathEdit_, 5, 1);
        advancedLayout->addWidget(chooseMetadataPresetButton_, 5, 2);
        advancedLayout->addWidget(clearMetadataPresetButton_, 5, 3);
        advancedLayout->addWidget(telemetryLabel_, 6, 0, 1, 4);
        connect(profileCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &EncoderWindow::ApplyProfileDefaults);
        connect(runIdEdit_, &QLineEdit::textChanged, this, &EncoderWindow::UpdateActionButtons);
        connect(channelCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &EncoderWindow::UpdateActionButtons);
        connect(remoteProviderEdit_, &QLineEdit::textChanged, this, &EncoderWindow::UpdateActionButtons);
        connect(metadataPresetPathEdit_, &QLineEdit::textChanged, this, &EncoderWindow::UpdateActionButtons);
        connect(chooseMetadataPresetButton_, &QPushButton::clicked, this, &EncoderWindow::ChooseMetadataPreset);
        connect(clearMetadataPresetButton_, &QPushButton::clicked, metadataPresetPathEdit_, &QLineEdit::clear);
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
        connect(exportButton, &QPushButton::clicked, this, &EncoderWindow::ExportReport);
        connect(copyButton, &QPushButton::clicked, this, &EncoderWindow::CopyDiagnostics);
        root->addWidget(diagnosticsGroup);

        setCentralWidget(central);
        setStyleSheet(QStringLiteral(
            "QMainWindow{background:#f7f8fa;} QGroupBox{font-weight:600;border:1px solid #d0d5dd;border-radius:7px;margin-top:10px;padding-top:9px;background:white;}"
            "QGroupBox::title{subcontrol-origin:margin;left:10px;padding:0 4px;} QLineEdit,QComboBox,QSpinBox,QPlainTextEdit{padding:5px;border:1px solid #cfd4dc;border-radius:4px;background:white;}"
            "QPushButton{padding:6px 12px;} QProgressBar{height:22px;text-align:center;border:1px solid #b8c0cc;border-radius:4px;} QProgressBar::chunk{background:#1570ef;}"));
    }

    void LoadSettings()
    {
        QSettings settings;
        restoreGeometry(settings.value(QStringLiteral("ui/windowGeometry")).toByteArray());
        sourcePathEdit_->setText(settings.value(QStringLiteral("ui/lastInputPath")).toString());
        compressionCheck_->setChecked(settings.value(QStringLiteral("ui/compressionEnabled"), false).toBool());
        advancedGroup_->setChecked(settings.value(QStringLiteral("ui/advancedExpanded"), false).toBool());
        preferredMonitorDevice_ = settings.value(QStringLiteral("ui/lastMonitorDevice")).toString();
        preferredProtectedMonitorDevice_ = settings.value(QStringLiteral("ui/lastProtectedMonitorDevice")).toString();
    }

    void SaveSettings()
    {
        QSettings settings;
        settings.setValue(QStringLiteral("ui/windowGeometry"), saveGeometry());
        settings.setValue(QStringLiteral("ui/lastInputPath"), sourcePathEdit_->text());
        settings.setValue(QStringLiteral("ui/compressionEnabled"), compressionCheck_->isChecked());
        settings.setValue(QStringLiteral("ui/advancedExpanded"), advancedGroup_->isChecked());
        const int monitorIndex = monitorCombo_->currentIndex();
        if (monitorIndex >= 0 && static_cast<std::size_t>(monitorIndex) < monitors_.size())
        {
            settings.setValue(QStringLiteral("ui/lastMonitorDevice"),
                QString::fromStdWString(monitors_[static_cast<std::size_t>(monitorIndex)].deviceName));
        }
        const int protectedMonitorIndex = protectedMonitorCombo_->currentData().toInt();
        if (protectedMonitorIndex >= 0 && static_cast<std::size_t>(protectedMonitorIndex) < monitors_.size())
        {
            settings.setValue(QStringLiteral("ui/lastProtectedMonitorDevice"),
                QString::fromStdWString(monitors_[static_cast<std::size_t>(protectedMonitorIndex)].deviceName));
        }
    }

    void ChooseFile()
    {
        const QString initial = sourcePathEdit_->text().isEmpty() ?
            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) : sourcePathEdit_->text();
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择 PixelBridge 源文件"), initial);
        if (!path.isEmpty())
        {
            sourcePathEdit_->setText(path);
            UpdateFileDetails();
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

    [[nodiscard]] bool BuildRemoteMetadata(const pbapp::MonitorInfo& monitor,
        pbapp::RemoteRunMetadata& output, QString& errorMessage) const
    {
        pbapp::RemoteRunMetadata candidate;
        candidate.channelType = channelCombo_->currentIndex() == 0 ? pbapp::ChannelType::LocalDesktop :
            channelCombo_->currentIndex() == 1 ? pbapp::ChannelType::RemoteVisual : pbapp::ChannelType::Other;
        const QString presetPath = metadataPresetPathEdit_->text().trimmed();
        if (!presetPath.isEmpty())
        {
            if (candidate.channelType != pbapp::ChannelType::RemoteVisual)
            {
                errorMessage = QStringLiteral("RemoteVisual metadata preset 只能用于 RemoteVisual channel");
                return false;
            }
            if (!pbapp::LoadRemoteVisualMetadataPreset(presetPath, candidate, errorMessage))
            {
                return false;
            }
        }
        const QString provider = remoteProviderEdit_->text().trimmed();
        if (candidate.channelType == pbapp::ChannelType::RemoteVisual)
        {
            if (!provider.isEmpty())
            {
                const std::string providerUtf8 = provider.toUtf8().toStdString();
                if (!candidate.remoteProvider.empty() && candidate.remoteProvider != providerUtf8)
                {
                    errorMessage = QStringLiteral("Remote provider 与 metadata preset 不一致");
                    return false;
                }
                candidate.remoteProvider = providerUtf8;
            }
            if (candidate.remoteProvider.empty())
            {
                errorMessage = QStringLiteral("RemoteVisual 必须提供 provider 或有效 metadata preset");
                return false;
            }
        }
        else
        {
            candidate.remoteProvider.clear();
        }
        const QString networkNote = networkNoteEdit_->text();
        if (!networkNote.isEmpty())
        {
            candidate.notes = networkNote.toUtf8().toStdString();
            candidate.networkProvenance = pbapp::MetadataProvenance::Manual;
        }
        const std::string experimentIdentity = monitor.deviceName.empty() ? "" :
            QString::fromWCharArray(monitor.deviceName.c_str()).toUtf8().toStdString();
        if (!candidate.experimentMonitorIdentity.empty() &&
            candidate.experimentMonitorIdentity != experimentIdentity)
        {
            errorMessage = QStringLiteral("ExperimentMonitor 与 metadata preset 不一致");
            return false;
        }
        candidate.experimentMonitorIdentity = experimentIdentity;
        candidate.computerBDisplayResolution = std::to_string(monitor.physicalRect.right - monitor.physicalRect.left) +
            "x" + std::to_string(monitor.physicalRect.bottom - monitor.physicalRect.top);
        candidate.computerBRefreshRate = static_cast<double>(monitor.refreshRate);
        output = std::move(candidate);
        errorMessage.clear();
        return true;
    }

    [[nodiscard]] bool BuildRemoteVisualLowFpsMonitorSafety(const bool revalidateTopology,
        const pbapp::MonitorInfo& experimentMonitor,
        std::optional<pbapp::MonitorSafetySelection>& output, pbapp::RemoteRunMetadata& metadata,
        QString& errorMessage) const
    {
        const auto profile = static_cast<pbapp::VisualProfile>(profileCombo_->currentData().toInt());
        if (profile != pbapp::VisualProfile::RemoteVisualLowFps)
        {
            output.reset();
            return true;
        }
        const int protectedMonitorIndex = protectedMonitorCombo_->currentData().toInt();
        if (protectedMonitorIndex < 0 || static_cast<std::size_t>(protectedMonitorIndex) >= monitors_.size())
        {
            errorMessage = QStringLiteral("remote-lf4 必须显式选择 ProtectedMonitor");
            return false;
        }
        pbapp::MonitorSafetySelection candidate{
            monitors_[static_cast<std::size_t>(protectedMonitorIndex)], experimentMonitor};
        const RECT target{experimentMonitor.phase1CanvasOrigin.x, experimentMonitor.phase1CanvasOrigin.y,
            static_cast<LONG>(static_cast<std::int64_t>(experimentMonitor.phase1CanvasOrigin.x) +
                pbapp::phase1CanvasWidth),
            static_cast<LONG>(static_cast<std::int64_t>(experimentMonitor.phase1CanvasOrigin.y) +
                pbapp::phase1CanvasHeight)};
        const pbapp::MonitorSafetyStatus topology = revalidateTopology ?
            pbapp::RevalidateMonitorSafetySelection(candidate) : pbapp::MonitorSafetyStatus{};
        const pbapp::MonitorSafetyStatus safety = pbapp::ValidateMonitorSafetyTarget(candidate, target,
            experimentMonitor.monitor);
        if (!topology || !safety || experimentMonitor.rotation != DXGI_MODE_ROTATION_IDENTITY)
        {
            const pbapp::MonitorSafetyError error = !topology ? topology.code : safety.code;
            errorMessage = QStringLiteral("remote-lf4 屏幕安全检查失败：%1")
                .arg(QString::fromLatin1(pbapp::GetMonitorSafetyErrorName(error)));
            return false;
        }
        const std::string protectedIdentity = QString::fromStdWString(candidate.protectedMonitor.deviceName)
            .toUtf8().toStdString();
        if (!metadata.protectedMonitorIdentity.empty() && metadata.protectedMonitorIdentity != protectedIdentity)
        {
            errorMessage = QStringLiteral("ProtectedMonitor 与 metadata preset 不一致");
            return false;
        }
        metadata.protectedMonitorIdentity = protectedIdentity;
        metadata.experimentMonitorIdentity = QString::fromStdWString(experimentMonitor.deviceName)
            .toUtf8().toStdString();
        output = std::move(candidate);
        errorMessage.clear();
        return true;
    }

    void ApplyProfileDefaults(const int)
    {
        const auto profile = static_cast<pbapp::VisualProfile>(profileCombo_->currentData().toInt());
        const pbapp::VisualProfileOption* const option = pbapp::FindVisualProfileOption(profile);
        if (option != nullptr && option->remoteVisual)
        {
            logicalFpsSpin_->setRange(1, 5);
            logicalFpsSpin_->setValue(static_cast<int>(option->defaultLogicalVisualFps));
            controlRepetitionsSpin_->setValue(static_cast<int>(option->defaultControlRepetitions));
            if (profile == pbapp::VisualProfile::RemoteVisualLowFps)
            {
                channelCombo_->setCurrentIndex(1);
            }
        }
        else
        {
            logicalFpsSpin_->setRange(0, 240);
            logicalFpsSpin_->setValue(0);
            controlRepetitionsSpin_->setValue(4);
        }
        protectedMonitorCombo_->setEnabled(profile == pbapp::VisualProfile::RemoteVisualLowFps &&
            !controller_.IsActive());
        UpdateActionButtons();
    }

    void UpdateFileDetails()
    {
        const QFileInfo info(sourcePathEdit_->text());
        pbapp::EncoderConfig config;
        config.sourcePath = sourcePathEdit_->text().toStdWString();
        config.compressionEnabled = compressionCheck_->isChecked();
        config.compressionLevel = compressionLevel_ == nullptr ? 3 : compressionLevel_->value();
        config.monitorClientOrigin = pbrenderd3d::PhysicalPoint{};
        const pbapp::RuntimeStatus validation = pbapp::ValidateEncoderConfig(config);
        if (info.exists() && info.isFile() && info.size() >= 0)
        {
            fileDetailsLabel_->setText(QStringLiteral("%1 · %2 bytes · %3 · %4")
                .arg(info.fileName()).arg(info.size()).arg(HumanBytes(static_cast<std::uint64_t>(info.size())))
                .arg(info.suffix().isEmpty() ? QStringLiteral("no extension") : info.suffix()));
        }
        else
        {
            fileDetailsLabel_->setText(QStringLiteral("未选择有效文件"));
        }
        validationLabel_->setText(validation ? QStringLiteral("✓ 当前单 Segment product boundary 内") :
            QStringLiteral("✕ %1").arg(FromUtf8(validation.message)));
        validationLabel_->setStyleSheet(validation ? QStringLiteral("color:#168a52;") : QStringLiteral("color:#b42318;"));
        compressionLevel_->setEnabled(compressionCheck_->isChecked() && !controller_.IsActive());
        UpdateActionButtons();
    }

    void RefreshMonitors()
    {
        std::vector<pbapp::MonitorInfo> monitors;
        const pbapp::MonitorCatalogStatus status = pbapp::EnumerateMonitors(monitors);
        monitorCombo_->clear();
        protectedMonitorCombo_->clear();
        protectedMonitorCombo_->addItem(QStringLiteral("LF4：请选择 ProtectedMonitor（不得显示 Data Window）"), -1);
        monitors_.clear();
        if (!status)
        {
            monitorInfoLabel_->setText(QStringLiteral("✕ 无法读取 PMv2 physical monitor metadata，code=%1 native=%2")
                .arg(static_cast<unsigned int>(status.code)).arg(status.nativeError));
            monitorInfoLabel_->setStyleSheet(QStringLiteral("color:#b42318;"));
            UpdateActionButtons();
            return;
        }
        monitors_ = std::move(monitors);
        for (std::size_t index = 0; index < monitors_.size(); index++)
        {
            const pbapp::MonitorInfo& monitor = monitors_[index];
            const std::int64_t width = static_cast<std::int64_t>(monitor.physicalRect.right) - monitor.physicalRect.left;
            const std::int64_t height = static_cast<std::int64_t>(monitor.physicalRect.bottom) - monitor.physicalRect.top;
            const QString label = QStringLiteral("%1 · %2x%3 @ %4 Hz%5")
                .arg(QString::fromWCharArray(monitor.deviceName.c_str())).arg(width).arg(height)
                .arg(monitor.refreshRate).arg(monitor.primary ? QStringLiteral(" · Primary") : QString());
            monitorCombo_->addItem(label);
            protectedMonitorCombo_->addItem(label, static_cast<int>(index));
        }
        const auto preferred = std::find_if(monitors_.begin(), monitors_.end(),
            [this](const pbapp::MonitorInfo& value)
            {
                return QString::fromStdWString(value.deviceName) == preferredMonitorDevice_;
            });
        const auto primary = std::find_if(monitors_.begin(), monitors_.end(),
            [](const pbapp::MonitorInfo& value)
            {
                return value.primary;
            });
        if (preferred != monitors_.end())
        {
            monitorCombo_->setCurrentIndex(static_cast<int>(std::distance(monitors_.begin(), preferred)));
        }
        else if (primary != monitors_.end())
        {
            monitorCombo_->setCurrentIndex(static_cast<int>(std::distance(monitors_.begin(), primary)));
        }
        const auto preferredProtected = std::find_if(monitors_.begin(), monitors_.end(),
            [this](const pbapp::MonitorInfo& value)
            {
                return QString::fromStdWString(value.deviceName) == preferredProtectedMonitorDevice_;
            });
        if (preferredProtected != monitors_.end())
        {
            protectedMonitorCombo_->setCurrentIndex(
                static_cast<int>(std::distance(monitors_.begin(), preferredProtected)) + 1);
        }
        UpdateMonitorDetails();
    }

    void UpdateMonitorDetails()
    {
        const int index = monitorCombo_->currentIndex();
        if (index < 0 || static_cast<std::size_t>(index) >= monitors_.size())
        {
            monitorInfoLabel_->setText(QStringLiteral("未选择输出 monitor"));
            UpdateActionButtons();
            return;
        }
        const pbapp::MonitorInfo& monitor = monitors_[static_cast<std::size_t>(index)];
        const std::int64_t width = static_cast<std::int64_t>(monitor.physicalRect.right) - monitor.physicalRect.left;
        const std::int64_t height = static_cast<std::int64_t>(monitor.physicalRect.bottom) - monitor.physicalRect.top;
        monitorInfoLabel_->setText(QStringLiteral("Physical [%1,%2] %3x%4 · Data Window origin [%5,%6] · DPI %7x%8 · %9")
            .arg(monitor.physicalRect.left).arg(monitor.physicalRect.top).arg(width).arg(height)
            .arg(monitor.phase1CanvasOrigin.x).arg(monitor.phase1CanvasOrigin.y)
            .arg(monitor.dpiX).arg(monitor.dpiY)
            .arg(!monitor.supportsPhase1Canvas ? QStringLiteral("✕ monitor is smaller than the fixed 1920x1080 Data Window") :
                monitor.phase1ReferenceGeometry ? QStringLiteral("✓ Matches Phase-1 1920x1080/60 reference geometry") :
                QStringLiteral("⚠ Different from tested Phase-1 reference; result is Experimental")));
        monitorInfoLabel_->setStyleSheet(!monitor.supportsPhase1Canvas ? QStringLiteral("color:#b42318;") :
            monitor.phase1ReferenceGeometry ? QStringLiteral("color:#168a52;") : QStringLiteral("color:#b54708;"));
        UpdateActionButtons();
    }

    void StartBroadcast()
    {
        const int monitorIndex = monitorCombo_->currentIndex();
        if (monitorIndex < 0 || static_cast<std::size_t>(monitorIndex) >= monitors_.size())
        {
            QMessageBox::warning(this, QStringLiteral("无法开始广播"), QStringLiteral("请选择有效的目标 monitor。"));
            return;
        }
        if (!monitors_[static_cast<std::size_t>(monitorIndex)].supportsPhase1Canvas)
        {
            QMessageBox::warning(this, QStringLiteral("无法开始广播"),
                QStringLiteral("目标 monitor 无法容纳固定 1920x1080 physical-pixel Data Window。"));
            return;
        }
        pbapp::EncoderConfig config;
        config.sourcePath = sourcePathEdit_->text().toStdWString();
        config.compressionEnabled = compressionCheck_->isChecked();
        config.compressionLevel = compressionLevel_->value();
        config.visualProfile = static_cast<pbapp::VisualProfile>(profileCombo_->currentData().toInt());
        config.logicalVisualFps = static_cast<std::uint32_t>(logicalFpsSpin_->value());
        config.controlRepetitions = static_cast<std::uint32_t>(controlRepetitionsSpin_->value());
        const pbapp::MonitorInfo& monitor = monitors_[static_cast<std::size_t>(monitorIndex)];
        config.monitorClientOrigin = pbrenderd3d::PhysicalPoint{monitor.phase1CanvasOrigin.x,
            monitor.phase1CanvasOrigin.y};
        QString metadataError;
        if (!BuildRemoteMetadata(monitor, config.remoteMetadata, metadataError))
        {
            QMessageBox::warning(this, QStringLiteral("无法开始广播"), metadataError);
            return;
        }
        if (!BuildRemoteVisualLowFpsMonitorSafety(true, monitor, config.monitorSafety,
            config.remoteMetadata, metadataError))
        {
            QMessageBox::warning(this, QStringLiteral("无法开始广播"), metadataError);
            return;
        }
        config.runId = runIdEdit_->text().toLatin1().toStdString();
        if (!config.remoteMetadata.runId.empty())
        {
            if (!config.runId.empty() && config.runId != config.remoteMetadata.runId)
            {
                QMessageBox::warning(this, QStringLiteral("无法开始广播"),
                    QStringLiteral("Shared RunId 与 metadata preset 不一致"));
                return;
            }
            config.runId = config.remoteMetadata.runId;
        }
        config.remoteMetadata.runId = config.runId;
        SetControlsEnabled(false);
        const QString error = controller_.Start(config);
        if (!error.isEmpty())
        {
            SetControlsEnabled(true);
            UpdateActionButtons();
            QMessageBox::warning(this, QStringLiteral("无法开始广播"), error);
            return;
        }
        AppendLog(QStringLiteral("Start accepted; background preparation began."));
        UpdateSnapshot();
    }

    void UpdateSnapshot()
    {
        const pbapp::EncoderSnapshot snapshot = controller_.GetSnapshot();
        stateLabel_->setText(QString::fromLatin1(pbapp::GetEncoderStateName(snapshot.state)));
        stateLabel_->setStyleSheet(QStringLiteral("padding:8px 14px;border-radius:6px;background:%1;color:white;font-weight:600;")
            .arg(StateColor(snapshot.state)));
        runtimeLabel_->setText(DurationText(snapshot.broadcastRuntimeMilliseconds));
        const bool sessionKnown = !snapshot.sessionIdHex.empty();
        sessionLabel_->setText(!sessionKnown ? QStringLiteral("—") :
            QStringLiteral("SessionTag %1\nSessionId %2")
                .arg(snapshot.sessionTag, 16, 16, QLatin1Char('0')).arg(FromUtf8(snapshot.sessionIdHex)));
        frameLabel_->setText(snapshot.segmentCount == 0 ? QStringLiteral("Frame — · OuterBlock — · Segment —") :
            QStringLiteral("%1 · OuterBlock %2 · Segment %3/%4")
                .arg(snapshot.frameSequence).arg(snapshot.currentOuterBlockId)
                .arg(snapshot.currentSegmentOrdinal + 1).arg(snapshot.segmentCount));
        fpsLabel_->setText(QStringLiteral("Presented %1 FPS / PresentCall %2 FPS")
            .arg(snapshot.presentedVisualFps ? QString::number(*snapshot.presentedVisualFps, 'f', 2) : QStringLiteral("—"))
            .arg(snapshot.presentCallFps ? QString::number(*snapshot.presentCallFps, 'f', 2) : QStringLiteral("—")));
        rateLabel_->setText(RateText(snapshot.generatedPayloadBytesPerSecond));
        const int cycleValue = snapshot.cycleFrameCount == 0 ? 0 : static_cast<int>(
            static_cast<std::uint64_t>(snapshot.cyclePosition) * 10000ULL / snapshot.cycleFrameCount);
        cycleProgress_->setValue(cycleValue);
        cycleLabel_->setText(QStringLiteral("Cycle %1 · frame %2/%3 (不是文件恢复进度)")
            .arg(snapshot.cycleCount).arg(snapshot.cyclePosition).arg(snapshot.cycleFrameCount));
        statusMessageLabel_->setText(FromUtf8(snapshot.statusMessage));
        outerFecLabel_->setText(!sessionKnown ? QStringLiteral("Automatic: DirectRepeat or Wirehair V2") :
            QString::fromLatin1(pbapp::GetOuterFecModeName(snapshot.outerFecMode)));
        telemetryLabel_->setText(QStringLiteral(
            "PresentationEpoch=%1 · submitted=%2 · replacedPending=%3 · sourceReplace/repeatPresent=%4/%5 · "
            "queue=%6/HWM %7 · active=%8(seq %9) · contract=%10 · GeneratedVisual=%11 fps · SourceStable=%12 · Digest=%13")
            .arg(snapshot.presentationEpoch).arg(snapshot.submittedFrames).arg(snapshot.replacedPendingFrames)
            .arg(snapshot.sourceTextureReplacements).arg(snapshot.repeatedPresentCalls)
            .arg(snapshot.pendingFrames).arg(snapshot.pendingHighWater)
            .arg(snapshot.activeFrame ? QStringLiteral("true") : QStringLiteral("false")).arg(snapshot.activeFrameSequence)
            .arg(snapshot.candidateContractSatisfied ? QStringLiteral("PASS") : QStringLiteral("warming/unavailable"))
            .arg(snapshot.generatedVisualFramesPerSecond ? QString::number(*snapshot.generatedVisualFramesPerSecond, 'f', 2) : QStringLiteral("—"))
            .arg(snapshot.sourceStable ? QStringLiteral("true") : QStringLiteral("false"))
            .arg(FromUtf8(snapshot.wholeFileDigestHex)) +
            QStringLiteral(" · LogicalFPS=%1 · configured/min dwell=%2/%3 ms · dwellViolations=%4 · ControlRepetitions=%5")
                .arg(snapshot.configuredLogicalVisualFps)
                .arg(snapshot.configuredLogicalDwellMilliseconds ? QString::number(*snapshot.configuredLogicalDwellMilliseconds, 'f', 3) : QStringLiteral("—"))
                .arg(snapshot.minimumObservedLogicalDwellMilliseconds ? QString::number(*snapshot.minimumObservedLogicalDwellMilliseconds, 'f', 3) : QStringLiteral("—"))
                .arg(snapshot.logicalDwellViolationCount).arg(snapshot.configuredControlRepetitions));
        if (snapshot.state != lastLoggedState_ || (!snapshot.errorDetail.empty() && snapshot.errorDetail != lastError_))
        {
            AppendLog(QStringLiteral("%1 — %2%3")
                .arg(QString::fromLatin1(pbapp::GetEncoderStateName(snapshot.state)))
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
        if (closePending_)
        {
            QTimer::singleShot(0, this, &QWidget::close);
        }
    }

    void SetControlsEnabled(const bool enabled)
    {
        sourcePathEdit_->setEnabled(enabled);
        chooseFileButton_->setEnabled(enabled);
        compressionCheck_->setEnabled(enabled);
        compressionLevel_->setEnabled(enabled && compressionCheck_->isChecked());
        profileCombo_->setEnabled(enabled);
        logicalFpsSpin_->setEnabled(enabled);
        controlRepetitionsSpin_->setEnabled(enabled);
        monitorCombo_->setEnabled(enabled);
        refreshMonitorButton_->setEnabled(enabled);
        protectedMonitorCombo_->setEnabled(enabled &&
            static_cast<pbapp::VisualProfile>(profileCombo_->currentData().toInt()) ==
                pbapp::VisualProfile::RemoteVisualLowFps);
        channelCombo_->setEnabled(enabled);
        remoteProviderEdit_->setEnabled(enabled);
        metadataPresetPathEdit_->setEnabled(enabled);
        chooseMetadataPresetButton_->setEnabled(enabled);
        clearMetadataPresetButton_->setEnabled(enabled && !metadataPresetPathEdit_->text().isEmpty());
        networkNoteEdit_->setEnabled(enabled);
        runIdEdit_->setEnabled(enabled);
    }

    void UpdateActionButtons()
    {
        pbapp::EncoderConfig config;
        config.sourcePath = sourcePathEdit_->text().toStdWString();
        config.compressionLevel = compressionLevel_->value();
        config.visualProfile = static_cast<pbapp::VisualProfile>(profileCombo_->currentData().toInt());
        config.logicalVisualFps = static_cast<std::uint32_t>(logicalFpsSpin_->value());
        config.controlRepetitions = static_cast<std::uint32_t>(controlRepetitionsSpin_->value());
        config.runId = runIdEdit_->text().toLatin1().toStdString();
        const int monitorIndex = monitorCombo_->currentIndex();
        bool monitorEligible = false;
        if (monitorIndex >= 0 && static_cast<std::size_t>(monitorIndex) < monitors_.size())
        {
            const pbapp::MonitorInfo& monitor = monitors_[static_cast<std::size_t>(monitorIndex)];
            if (monitor.supportsPhase1Canvas)
            {
                monitorEligible = true;
                config.monitorClientOrigin = pbrenderd3d::PhysicalPoint{monitor.phase1CanvasOrigin.x,
                    monitor.phase1CanvasOrigin.y};
            }
        }
        QString metadataError;
        const bool metadataValid = monitorEligible && BuildRemoteMetadata(
            monitors_[static_cast<std::size_t>(monitorIndex)], config.remoteMetadata, metadataError);
        const bool monitorSafetyValid = metadataValid && BuildRemoteVisualLowFpsMonitorSafety(false,
            monitors_[static_cast<std::size_t>(monitorIndex)], config.monitorSafety,
            config.remoteMetadata, metadataError);
        const bool runIdMatches = config.remoteMetadata.runId.empty() || config.runId.empty() ||
            config.remoteMetadata.runId == config.runId;
        if (config.runId.empty() && !config.remoteMetadata.runId.empty())
        {
            config.runId = config.remoteMetadata.runId;
        }
        config.remoteMetadata.runId = config.runId;
        const bool valid = monitorEligible && metadataValid && monitorSafetyValid && runIdMatches &&
            static_cast<bool>(pbapp::ValidateEncoderConfig(config));
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
        return {"PixelBridgeEncoder", buildInfo.version, PB_GIT_COMMIT,
            QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString()};
    }

    void ExportReport()
    {
        const QString defaultPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) +
            QStringLiteral("/PixelBridge-Encoder-RunReport.json");
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出 Encoder 运行报告"),
            defaultPath, QStringLiteral("JSON (*.json)"));
        if (path.isEmpty())
        {
            return;
        }
        const std::string report = pbapp::BuildEncoderRunReportJson(ReportContext(), controller_.GetSnapshot());
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
        const std::string diagnostics = pbapp::BuildEncoderDiagnostics(controller_.GetSnapshot());
        QApplication::clipboard()->setText(FromUtf8(diagnostics));
        AppendLog(QStringLiteral("Diagnostics copied to clipboard (no payload bytes)."));
    }

    EncoderApplicationController controller_;
    QLineEdit* sourcePathEdit_ = nullptr;
    QPushButton* chooseFileButton_ = nullptr;
    QLabel* fileDetailsLabel_ = nullptr;
    QLabel* validationLabel_ = nullptr;
    QCheckBox* compressionCheck_ = nullptr;
    QSpinBox* compressionLevel_ = nullptr;
    QSpinBox* logicalFpsSpin_ = nullptr;
    QSpinBox* controlRepetitionsSpin_ = nullptr;
    QComboBox* profileCombo_ = nullptr;
    QLabel* outerFecLabel_ = nullptr;
    QComboBox* monitorCombo_ = nullptr;
    QComboBox* protectedMonitorCombo_ = nullptr;
    QPushButton* refreshMonitorButton_ = nullptr;
    QLabel* monitorInfoLabel_ = nullptr;
    QPushButton* startButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QLabel* stateLabel_ = nullptr;
    QLabel* runtimeLabel_ = nullptr;
    QLabel* sessionLabel_ = nullptr;
    QLabel* frameLabel_ = nullptr;
    QLabel* fpsLabel_ = nullptr;
    QLabel* rateLabel_ = nullptr;
    QLabel* cycleLabel_ = nullptr;
    QProgressBar* cycleProgress_ = nullptr;
    QLabel* statusMessageLabel_ = nullptr;
    QGroupBox* advancedGroup_ = nullptr;
    QLabel* telemetryLabel_ = nullptr;
    QComboBox* channelCombo_ = nullptr;
    QLineEdit* remoteProviderEdit_ = nullptr;
    QLineEdit* metadataPresetPathEdit_ = nullptr;
    QPushButton* chooseMetadataPresetButton_ = nullptr;
    QPushButton* clearMetadataPresetButton_ = nullptr;
    QLineEdit* networkNoteEdit_ = nullptr;
    QLineEdit* runIdEdit_ = nullptr;
    QPlainTextEdit* logEdit_ = nullptr;
    std::vector<pbapp::MonitorInfo> monitors_;
    QString preferredMonitorDevice_;
    QString preferredProtectedMonitorDevice_;
    pbapp::EncoderState lastLoggedState_ = pbapp::EncoderState::Idle;
    std::string lastError_;
    bool closePending_ = false;
};

} // namespace

int RunEncoderGui(const int argumentCount, wchar_t* arguments[])
{
    const bool smoke = argumentCount == 2 && std::wstring_view(arguments[1]) == L"--gui-smoke";
    const bool integrationSmoke = argumentCount == 3 &&
        std::wstring_view(arguments[1]) == L"--gui-integration-smoke";
    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    int guiArgumentCount = 1;
    char applicationName[] = "PixelBridgeEncoder";
    char* guiArguments[] = {applicationName, nullptr};
    QApplication application(guiArgumentCount, guiArguments);
    QCoreApplication::setOrganizationName(QStringLiteral("PixelBridge"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("pixelbridge.local"));
    QCoreApplication::setApplicationName(QStringLiteral("PixelBridgeEncoder"));
    EncoderWindow window;
    if (integrationSmoke && !PlaceOnExperimentMonitor(window, arguments[2]))
    {
        return 2;
    }
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
