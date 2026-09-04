#include "encoder_gui.h"
#include "encoder_application_controller.h"
#include "monitor_catalog.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenuBar>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QThread>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <string_view>

namespace
{

[[nodiscard]] QString FromUtf8(const std::string& value)
{
    return QString::fromUtf8(value.data(), static_cast<int>(value.size()));
}

[[nodiscard]] QString HumanBytes(const std::uint64_t bytes)
{
    static const QStringList units{QStringLiteral("B"), QStringLiteral("KiB"), QStringLiteral("MiB"), QStringLiteral("GiB")};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit + 1 < units.size())
    {
        value /= 1024.0;
        unit++;
    }
    return QStringLiteral("%1 %2").arg(value, 0, 'f', unit == 0 ? 0 : 2).arg(units[unit]);
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

[[nodiscard]] QString StateText(const pbapp::EncoderState state)
{
    switch (state)
    {
    case pbapp::EncoderState::Idle: return QStringLiteral("等待选择文件");
    case pbapp::EncoderState::Preparing: return QStringLiteral("准备广播");
    case pbapp::EncoderState::Broadcasting: return QStringLiteral("正在广播");
    case pbapp::EncoderState::Stopping: return QStringLiteral("正在停止广播");
    case pbapp::EncoderState::Stopped: return QStringLiteral("广播已停止");
    case pbapp::EncoderState::Failed: return QStringLiteral("广播失败");
    }
    return QStringLiteral("未知状态");
}

class EncoderWindow final : public QMainWindow
{
public:
    explicit EncoderWindow(pbapp::EncoderPresentationFactory presentationFactory = {},
        const QString& settingsFile = {}, std::filesystem::path sessionRoot = {},
        std::function<bool()> confirmDeletion = {}) : controller_(nullptr, std::move(presentationFactory)),
        sessionRoot_(std::move(sessionRoot)), confirmDeletion_(std::move(confirmDeletion))
    {
        settings_ = settingsFile.isEmpty() ? std::make_unique<QSettings>() :
            std::make_unique<QSettings>(settingsFile, QSettings::IniFormat);
        setWindowTitle(QStringLiteral("PixelBridge Encoder — 广播"));
        setMinimumSize(700, 560);
        resize(850, 660);
        BuildUi();
        if (settingsFile.isEmpty())
        {
            restoreGeometry(settings_->value(QStringLiteral("windowGeometry")).toByteArray());
        }
        sourceEdit_->setText(settings_->value(QStringLiteral("lastInputPath")).toString());
        const int savedFps = settings_->value(QStringLiteral("unifiedLogicalFps"), 15).toInt();
        fpsSpin_->setValue(savedFps >= 1 && savedFps <= 60 ? savedFps : 15);
        advancedToggle_->setChecked(settings_->value(QStringLiteral("unifiedAdvancedExpanded"), false).toBool());
        connect(&controller_, &EncoderApplicationController::SnapshotChanged, this, &EncoderWindow::UpdateSnapshot);
        connect(&controller_, &EncoderApplicationController::TerminalStateReached, this, [this]()
        {
            if (closePending_)
            {
                close();
            }
        });
        UpdateSnapshot();
    }

    // Runs against the actual controller/runtime with a non-displaying
    // presentation owner. No native window or external input is generated.
    [[nodiscard]] bool RunSmoke(const QString& sourcePath, const std::function<bool()>& presentationCreated,
        bool& confirmDeletion)
    {
        std::cerr << "G15 GUI smoke phase: initial controls\n";
        if (isVisible() || fpsSpin_->minimum() != 1 || fpsSpin_->maximum() != 60 || fpsSpin_->value() != 15 ||
            !advancedText_->isReadOnly() || stopButton_->isEnabled() || deleteAction_->isEnabled())
        {
            return false;
        }
        sourceEdit_->setText(sourcePath);
        if (!startButton_->isEnabled())
        {
            return false;
        }
        startButton_->click();
        std::cerr << "G15 GUI smoke phase: waiting for preparation\n";
        if (!WaitFor([&]()
        {
            return presentationCreated();
        }))
        {
            return false;
        }
        UpdateSnapshot();
        const pbapp::EncoderSnapshot prepared = controller_.GetSnapshot();
        if (!prepared.preparationComplete || prepared.visualProfile != pbapp::VisualProfile::UnifiedLc4 ||
            prepared.sessionIdHex.empty() || progress_->value() != 1000 || sourceEdit_->isEnabled() ||
            !fpsSpin_->isEnabled() || !stopButton_->isEnabled())
        {
            return false;
        }
        fpsSpin_->setValue(60);
        std::cerr << "G15 GUI smoke phase: waiting for FPS\n";
        if (!WaitFor([&]()
        {
            return controller_.GetSnapshot().configuredLogicalVisualFps == 60;
        }))
        {
            return false;
        }
        stopButton_->click();
        std::cerr << "G15 GUI smoke phase: waiting for stop\n";
        if (!WaitFor([&]()
        {
            return controller_.GetSnapshot().state == pbapp::EncoderState::Stopped;
        }))
        {
            return false;
        }
        UpdateSnapshot();
        const std::filesystem::path directory(std::u8string(prepared.sessionStateDirectory.begin(), prepared.sessionStateDirectory.end()));
        if (!deleteAction_->isEnabled() || !std::filesystem::exists(directory))
        {
            return false;
        }
        confirmDeletion = false;
        deleteAction_->trigger();
        if (!std::filesystem::exists(directory) || controller_.GetSnapshot().sessionDeleted)
        {
            return false;
        }
        confirmDeletion = true;
        std::cerr << "G15 GUI smoke phase: explicit deletion\n";
        deleteAction_->trigger();
        if (!controller_.GetSnapshot().sessionDeleted || std::filesystem::exists(directory) || !QFileInfo::exists(sourcePath))
        {
            return false;
        }
        return !deleteAction_->isEnabled() && startButton_->isEnabled() && !stopButton_->isEnabled();
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
        settings_->setValue(QStringLiteral("windowGeometry"), saveGeometry());
        settings_->setValue(QStringLiteral("lastInputPath"), sourceEdit_->text());
        settings_->setValue(QStringLiteral("unifiedLogicalFps"), fpsSpin_->value());
        settings_->setValue(QStringLiteral("unifiedAdvancedExpanded"), advancedToggle_->isChecked());
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
        QVBoxLayout* const layout = new QVBoxLayout(central);
        layout->setContentsMargins(24, 20, 24, 20);
        layout->setSpacing(14);
        QLabel* const title = new QLabel(QStringLiteral("广播文件"));
        title->setStyleSheet(QStringLiteral("font-size: 24px; font-weight: 600;"));
        layout->addWidget(title);
        QLabel* const explanation = new QLabel(QStringLiteral("先完整预扫描，再打开可拖动、可缩放的编码窗口。广播持续循环，停止后保留可恢复会话。"));
        explanation->setWordWrap(true);
        layout->addWidget(explanation);
        QHBoxLayout* const fileRow = new QHBoxLayout();
        sourceEdit_ = new QLineEdit();
        sourceEdit_->setObjectName(QStringLiteral("sourcePath"));
        sourceEdit_->setPlaceholderText(QStringLiteral("选择要广播的源文件"));
        browseButton_ = new QPushButton(QStringLiteral("选择文件…"));
        fileRow->addWidget(sourceEdit_, 1);
        fileRow->addWidget(browseButton_);
        layout->addLayout(fileRow);
        QHBoxLayout* const actionRow = new QHBoxLayout();
        actionRow->addWidget(new QLabel(QStringLiteral("逻辑刷新率")));
        fpsSpin_ = new QSpinBox();
        fpsSpin_->setObjectName(QStringLiteral("logicalFps"));
        fpsSpin_->setRange(1, 60);
        fpsSpin_->setValue(15);
        fpsSpin_->setSuffix(QStringLiteral(" Hz"));
        fpsSpin_->setToolTip(QStringLiteral("运行中可调整；新频率从下一张完整逻辑帧生效。重复 Present 不产生新数据。"));
        actionRow->addWidget(fpsSpin_);
        actionRow->addStretch();
        startButton_ = new QPushButton(QStringLiteral("开始广播"));
        startButton_->setObjectName(QStringLiteral("startBroadcast"));
        stopButton_ = new QPushButton(QStringLiteral("停止广播"));
        stopButton_->setObjectName(QStringLiteral("stopBroadcast"));
        actionRow->addWidget(startButton_);
        actionRow->addWidget(stopButton_);
        layout->addLayout(actionRow);
        stateLabel_ = new QLabel();
        stateLabel_->setStyleSheet(QStringLiteral("font-size: 18px; font-weight: 600; color: #175CD3;"));
        layout->addWidget(stateLabel_);
        progress_ = new QProgressBar();
        progress_->setObjectName(QStringLiteral("preparationProgress"));
        progress_->setRange(0, 1000);
        progress_->setValue(0);
        progress_->setFormat(QStringLiteral("本地预扫描 %p%"));
        layout->addWidget(progress_);
        QFormLayout* const status = new QFormLayout();
        fileLabel_ = new QLabel();
        scanLabel_ = new QLabel();
        carouselLabel_ = new QLabel();
        fpsLabel_ = new QLabel();
        stabilityLabel_ = new QLabel();
        status->addRow(QStringLiteral("文件 / Segment"), fileLabel_);
        status->addRow(QStringLiteral("预扫描"), scanLabel_);
        status->addRow(QStringLiteral("广播位置"), carouselLabel_);
        status->addRow(QStringLiteral("实际逻辑 FPS"), fpsLabel_);
        status->addRow(QStringLiteral("源文件稳定性"), stabilityLabel_);
        layout->addLayout(status);
        messageLabel_ = new QLabel();
        messageLabel_->setWordWrap(true);
        messageLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(messageLabel_);
        advancedToggle_ = new QPushButton(QStringLiteral("高级信息（只读）"));
        advancedToggle_->setCheckable(true);
        layout->addWidget(advancedToggle_);
        advancedText_ = new QPlainTextEdit();
        advancedText_->setObjectName(QStringLiteral("readOnlyRuntimeDetails"));
        advancedText_->setReadOnly(true);
        advancedText_->setVisible(false);
        layout->addWidget(advancedText_, 1);
        layout->addStretch();
        setCentralWidget(central);
        deleteAction_ = menuBar()->addMenu(QStringLiteral("会话"))->addAction(QStringLiteral("结束并删除会话…"));
        deleteAction_->setObjectName(QStringLiteral("endAndDeleteSession"));
        connect(browseButton_, &QPushButton::clicked, this, [this]()
        {
            const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择广播源文件"), sourceEdit_->text());
            if (!path.isEmpty())
            {
                sourceEdit_->setText(path);
            }
        });
        connect(sourceEdit_, &QLineEdit::textChanged, this, &EncoderWindow::UpdateActions);
        connect(startButton_, &QPushButton::clicked, this, &EncoderWindow::StartBroadcast);
        connect(stopButton_, &QPushButton::clicked, &controller_, &EncoderApplicationController::RequestStop);
        connect(fpsSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](const int fps)
        {
            if (controller_.IsActive())
            {
                const QString error = controller_.SetLogicalVisualFps(static_cast<std::uint32_t>(fps));
                if (!error.isEmpty())
                {
                    messageLabel_->setText(error);
                }
            }
        });
        connect(advancedToggle_, &QPushButton::toggled, advancedText_, &QWidget::setVisible);
        connect(deleteAction_, &QAction::triggered, this, &EncoderWindow::EndAndDeleteSession);
    }

    void UpdateActions()
    {
        const pbapp::EncoderSnapshot snapshot = controller_.GetSnapshot();
        const bool active = pbapp::IsEncoderStateActive(snapshot.state);
        sourceEdit_->setEnabled(!active && !closePending_);
        browseButton_->setEnabled(!active && !closePending_);
        const QFileInfo source(sourceEdit_->text());
        startButton_->setEnabled(!active && !closePending_ && source.isFile() && source.isReadable() &&
            source.size() >= 0 && static_cast<std::uint64_t>(source.size()) <= pbapp::maximumInstantFileBytes);
        stopButton_->setEnabled(active && snapshot.state != pbapp::EncoderState::Stopping && !closePending_);
        fpsSpin_->setEnabled(snapshot.state != pbapp::EncoderState::Stopping && !closePending_);
        deleteAction_->setEnabled(!active && !closePending_ && !snapshot.sessionIdHex.empty() && !snapshot.sessionDeleted);
    }

    void StartBroadcast()
    {
        pbapp::EncoderConfig config = pbapp::MakeUnifiedEncoderConfig(sourceEdit_->text().toStdWString(),
            static_cast<std::uint32_t>(fpsSpin_->value()));
        config.sessionStateRoot = sessionRoot_;
        const QString error = controller_.Start(config);
        UpdateSnapshot();
        if (!error.isEmpty())
        {
            messageLabel_->setText(error);
        }
    }

    void EndAndDeleteSession()
    {
        const pbapp::EncoderSnapshot snapshot = controller_.GetSnapshot();
        if (controller_.IsActive() || snapshot.sessionIdHex.empty() || snapshot.sessionDeleted)
        {
            return;
        }
        const bool confirmed = confirmDeletion_ ? confirmDeletion_() :
            QMessageBox::question(this, QStringLiteral("结束并删除会话"),
                QStringLiteral("删除当前 Encoder 会话的恢复状态和对应源文件索引？\n源文件不会删除；再次开始广播会建立新 Session。\n\nSession: %1\n会话源文件: %2")
                    .arg(FromUtf8(snapshot.sessionIdHex)).arg(FromUtf8(snapshot.sourcePath)),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes;
        if (!confirmed)
        {
            return;
        }
        const QString error = controller_.EndAndDeleteSession(snapshot.runGeneration);
        UpdateSnapshot();
        if (!error.isEmpty())
        {
            messageLabel_->setText(QStringLiteral("删除未完成：%1").arg(error));
        }
    }

    void UpdateSnapshot()
    {
        const pbapp::EncoderSnapshot snapshot = controller_.GetSnapshot();
        stateLabel_->setText(StateText(snapshot.state));
        fileLabel_->setText(snapshot.runGeneration == 0 ? QStringLiteral("—") :
            QStringLiteral("%1 / %2 个").arg(HumanBytes(snapshot.sourceBytes)).arg(snapshot.segmentCount));
        const int progress = snapshot.preparationComplete ? 1000 : snapshot.sourceBytes != 0 ?
            static_cast<int>((snapshot.preparedSourceBytes * 1000ULL) / snapshot.sourceBytes) : 0;
        progress_->setValue(progress);
        const QString speed = snapshot.preparationBytesPerSecond ?
            QStringLiteral("%1 MiB/s").arg(*snapshot.preparationBytesPerSecond / (1024.0 * 1024.0), 0, 'f', 2) : QStringLiteral("—");
        scanLabel_->setText(QStringLiteral("%1 / %2，%3，%4 s").arg(HumanBytes(snapshot.preparedSourceBytes))
            .arg(HumanBytes(snapshot.sourceBytes)).arg(speed).arg(snapshot.preparationMilliseconds / 1000.0, 0, 'f', 2));
        carouselLabel_->setText(snapshot.sessionIdHex.empty() ? QStringLiteral("尚未开始") :
            QStringLiteral("Carousel pass %1 / Segment ordinal %2（从 0 计数）")
                .arg(snapshot.cycleCount).arg(snapshot.segmentCount == 0 ? QStringLiteral("—") : QString::number(snapshot.currentSegmentOrdinal)));
        fpsLabel_->setText(snapshot.generatedVisualFramesPerSecond ?
            QStringLiteral("本次广播平均 %1 Hz（当前生效设置 %2 Hz）").arg(*snapshot.generatedVisualFramesPerSecond, 0, 'f', 2)
                .arg(snapshot.configuredLogicalVisualFps) : QStringLiteral("等待足够逻辑帧；不使用 Present 速率代替"));
        stabilityLabel_->setText(!snapshot.sourceStabilityVerified ? QStringLiteral("尚未完成校验") :
            !snapshot.sourceStable ? QStringLiteral("已检测到变化，广播中止") : pbapp::IsEncoderStateActive(snapshot.state) ?
                QStringLiteral("身份与摘要已校验；广播期间保持只读锁定并持续检查") : QStringLiteral("末次校验稳定；重新开始时会再次完整校验"));
        messageLabel_->setText(FromUtf8(snapshot.statusMessage) + (snapshot.errorDetail.empty() ? QString() :
            QStringLiteral("\n") + FromUtf8(snapshot.errorDetail)));
        const QString outer = snapshot.segmentCount == 0 ? QStringLiteral("无 Segment") :
            snapshot.outerFecMode == pbprotocol::OuterFecMode::WirehairV2 ? QStringLiteral("Wirehair V2") : QStringLiteral("DirectRepeat");
        advancedText_->setPlainText(QStringLiteral(
            "Profile: PB-Unified-LC4-V1 / layout 8 / 1920×1080 BGRA8 SDR\n"
            "Inner FEC: Robust DVB-S2 Short QC-LDPC；Base 17 / Fine 4 / Chroma 10\n"
            "Outer FEC（当前 Segment）: %1\n"
            "自动压缩: RAW %2 Segment / zstd %3 Segment（固定 level 3；预扫描决定）\n"
            "Session: %4\n恢复已有会话: %5\n持久状态: %6\n"
            "Durable lease（exclusive）: FrameSequence %7 / 当前 repair ID %8\n"
            "Whole-file BLAKE3: %9\n"
            "缩放: point sampling + letterbox；小于 0.75×暂停，不推进逻辑帧。\n"
            "停止保留 Session；需删除时使用“会话 → 结束并删除会话”。")
            .arg(outer).arg(snapshot.rawSegmentCount).arg(snapshot.zstdSegmentCount)
            .arg(snapshot.sessionIdHex.empty() ? QStringLiteral("尚未建立") : FromUtf8(snapshot.sessionIdHex))
            .arg(snapshot.resumedSession ? QStringLiteral("是") : QStringLiteral("否"))
            .arg(snapshot.sessionDeleted ? QStringLiteral("已删除") : FromUtf8(snapshot.sessionStateDirectory))
            .arg(snapshot.durableFrameSequenceLeaseEnd).arg(snapshot.durableRepairIdLeaseEnd).arg(FromUtf8(snapshot.wholeFileDigestHex)));
        UpdateActions();
    }

    EncoderApplicationController controller_;
    std::unique_ptr<QSettings> settings_;
    std::filesystem::path sessionRoot_;
    std::function<bool()> confirmDeletion_;
    bool closePending_ = false;
    QLineEdit* sourceEdit_ = nullptr;
    QSpinBox* fpsSpin_ = nullptr;
    QPushButton* browseButton_ = nullptr;
    QPushButton* startButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QPushButton* advancedToggle_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* stateLabel_ = nullptr;
    QLabel* fileLabel_ = nullptr;
    QLabel* scanLabel_ = nullptr;
    QLabel* carouselLabel_ = nullptr;
    QLabel* fpsLabel_ = nullptr;
    QLabel* stabilityLabel_ = nullptr;
    QLabel* messageLabel_ = nullptr;
    QPlainTextEdit* advancedText_ = nullptr;
    QAction* deleteAction_ = nullptr;
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
        return pbrenderd3d::PresentationStatus::Failure(pbrenderd3d::PresentationErrorCode::NotRunning,
            pbrenderd3d::PresentationStage::None);
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
    const bool smoke = argumentCount == 2 && std::wstring_view(arguments[1]) == L"--gui-smoke";
    const bool integrationSmoke = argumentCount == 3 && std::wstring_view(arguments[1]) == L"--gui-integration-smoke";
    if (smoke)
    {
        // Fail before QApplication rather than let Qt's missing-platform
        // fatal path show a native error dialog on an operator's desktop.
        std::array<wchar_t, 32768> modulePath{};
        const DWORD length = GetModuleFileNameW(nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
#ifdef QT_DEBUG
        constexpr const wchar_t* offscreenPlugin = L"qoffscreend.dll";
#else
        constexpr const wchar_t* offscreenPlugin = L"qoffscreen.dll";
#endif
        std::error_code error;
        if (length == 0 || length >= modulePath.size() || !std::filesystem::is_regular_file(
            std::filesystem::path(modulePath.data()).parent_path() / L"platforms" / offscreenPlugin, error) || error)
        {
            std::cerr << "G15 GUI smoke: offscreen platform is not deployed; no QApplication started\n";
            return 2;
        }
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    int guiArgumentCount = 1;
    char applicationName[] = "PixelBridgeEncoder";
    char* guiArguments[] = {applicationName, nullptr};
    QApplication application(guiArgumentCount, guiArguments);
    QCoreApplication::setOrganizationName(QStringLiteral("PixelBridge"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("pixelbridge.local"));
    QCoreApplication::setApplicationName(QStringLiteral("PixelBridgeEncoder"));
    if (smoke)
    {
        std::cerr << "G15 GUI smoke phase: Qt initialized\n";
        QTemporaryDir scratch;
        if (!scratch.isValid())
        {
            return 2;
        }
        const QString sourcePath = scratch.filePath(QStringLiteral("empty.bin"));
        QFile source(sourcePath);
        if (!source.open(QIODevice::WriteOnly))
        {
            return 2;
        }
        source.close();
        const QString settingsFile = scratch.filePath(QStringLiteral("settings.ini"));
        {
            QSettings legacySettings(settingsFile, QSettings::IniFormat);
            legacySettings.setValue(QStringLiteral("compressionEnabled"), false);
            legacySettings.setValue(QStringLiteral("logicalVisualFps"), 240);
            legacySettings.setValue(QStringLiteral("visualProfile"), QStringLiteral("remote-lf4"));
        }
        std::atomic<bool> created = false;
        bool confirmDeletion = false;
        EncoderWindow window([&](const pbrenderd3d::DataWindowConfig& config) -> std::unique_ptr<pbapp::EncoderPresentation>
        {
            if (!config.repeatActiveFrame || config.width != 1920 || config.height != 1080 || config.topmost)
            {
                return nullptr;
            }
            created = true;
            return std::make_unique<SmokeWaitingPresentation>();
        }, settingsFile, std::filesystem::path(scratch.path().toStdWString()) / L"sessions",
            [&]()
            {
                return confirmDeletion;
            });
        std::cerr << "G15 GUI smoke phase: widget constructed\n";
        const bool passed = window.RunSmoke(sourcePath, [&]()
        {
            return created.load();
        }, confirmDeletion);
        std::cout << "G15 GUI smoke: " << (passed ? "PASS" : "FAIL")
            << "; offscreen; actual controller; prepare/FPS/stop/retain/cancel-delete/confirmed-delete; no DataWindow\n";
        return passed ? 0 : 1;
    }
    EncoderWindow window;
    if (integrationSmoke && !PlaceOnExperimentMonitor(window, arguments[2]))
    {
        return 2;
    }
    window.show();
    return application.exec();
}
