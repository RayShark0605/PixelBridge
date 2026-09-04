#include "decoder_gui.h"
#include "decoder_application_controller.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
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
QString FromUtf8(const std::string& value)
{
    return QString::fromUtf8(value.data(), static_cast<int>(value.size()));
}

QString HumanBytes(const std::uint64_t bytes)
{
    return QStringLiteral("%1 B (%2 MiB)").arg(bytes).arg(static_cast<double>(bytes) / (1024.0 * 1024.0), 0, 'f', 2);
}

QString StateText(const pbapp::DecoderSnapshot& snapshot)
{
    switch (snapshot.state)
    {
    case pbapp::DecoderState::Idle: return QStringLiteral("等待选择输出目录与 ROI");
    case pbapp::DecoderState::WaitingForBootstrap: return QStringLiteral("等待有效 Unified 画面");
    case pbapp::DecoderState::AwaitingLargeOutputConfirmation: return QStringLiteral("等待大输出确认");
    case pbapp::DecoderState::ReceivingControl: return QStringLiteral("接收正式描述信息");
    case pbapp::DecoderState::Receiving:
    case pbapp::DecoderState::Recovering:
        return snapshot.captureStallActive || snapshot.visualStallActive ?
            QStringLiteral("等待画面恢复（已验证状态保留）") : QStringLiteral("正在恢复文件");
    case pbapp::DecoderState::Verifying: return QStringLiteral("正在校验文件");
    case pbapp::DecoderState::Publishing: return QStringLiteral("正在校验并安全发布");
    case pbapp::DecoderState::Completed: return QStringLiteral("恢复完成");
    case pbapp::DecoderState::Failed: return QStringLiteral("接收失败");
    case pbapp::DecoderState::Stopping: return QStringLiteral("正在停止捕获并保留恢复状态");
    case pbapp::DecoderState::Stopped: return QStringLiteral("接收已停止");
    }
    return QStringLiteral("未知状态");
}

struct WindowServices
{
    pbapp::DecoderRuntimeServices runtime;
    std::function<pbscreenregion::ScreenRegionStatus(pbscreenregion::ScreenCaptureRegion&)> selectRegion = pbscreenregion::SelectScreenCaptureRegion;
    std::function<pbscreenregion::ScreenRegionStatus(const RECT&, pbscreenregion::ScreenCaptureRegion&)> resolveRegion = pbscreenregion::ResolveScreenCaptureRegion;
    std::function<bool(const pbapp::DecoderSnapshot&)> confirmLargeOutput;
    std::function<bool(const QString&)> openDirectory = [](const QString& directory)
    {
        return QDesktopServices::openUrl(QUrl::fromLocalFile(directory));
    };
};

class DecoderWindow final : public QMainWindow
{
public:
    explicit DecoderWindow(WindowServices services = {}, const QString& settingsPath = {}) :
        services_(std::move(services)), controller_(nullptr, services_.runtime)
    {
        settings_ = settingsPath.isEmpty() ? std::make_unique<QSettings>() :
            std::make_unique<QSettings>(settingsPath, QSettings::IniFormat);
        setWindowTitle(QStringLiteral("PixelBridge Decoder — 恢复文件"));
        setMinimumSize(720, 600);
        resize(900, 740);
        BuildUi();
        if (settingsPath.isEmpty())
        {
            restoreGeometry(settings_->value(QStringLiteral("windowGeometry")).toByteArray());
        }
        outputEdit_->setText(settings_->value(QStringLiteral("unifiedOutputDirectory"),
            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).toString());
        advancedToggle_->setChecked(settings_->value(QStringLiteral("unifiedAdvancedExpanded"), false).toBool());
        connect(&controller_, &DecoderApplicationController::SnapshotChanged, this, &DecoderWindow::UpdateSnapshot);
        connect(&controller_, &DecoderApplicationController::TerminalStateReached, this, [this]()
        {
            if (closePending_)
            {
                close();
            }
        });
        UpdateSnapshot();
    }

#ifdef PB_ENABLE_UNIFIED_GUI_SMOKE
    bool RunSmoke(const QString& outputDirectory, const std::shared_ptr<g16test::ReceiveState>& state,
        const std::vector<pbdemodd3d11::CaptureDemodulatorResult>& frames, const std::span<const std::byte> expected,
        int& selectorCalls, int& confirmations, int& openCalls, bool& accept)
    {
        if (isVisible() || selectorCalls != 0 || !details_->isReadOnly() || startButton_->isEnabled() || stopButton_->isEnabled())
        {
            return false;
        }
        outputEdit_->setText(outputDirectory);
        roiButton_->click();
        if (selectorCalls != 1 || !startButton_->isEnabled())
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
            std::cerr << FromUtf8(controller_.GetSnapshot().errorDetail).toStdString() << '\n';
            return false;
        }
        UpdateSnapshot();
        if (confirmations != 1 || outputEdit_->isEnabled() || roiButton_->isEnabled() || !stopButton_->isEnabled())
        {
            return false;
        }
        stopButton_->click();
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
            std::cerr << FromUtf8(controller_.GetSnapshot().errorDetail).toStdString() << '\n';
            return false;
        }
        UpdateSnapshot();
        if (confirmations != 2 || !g16test::VerifyOutput(controller_.GetSnapshot(), expected) || !openButton_->isEnabled())
        {
            return false;
        }
        openButton_->click();
        if (openCalls != 1)
        {
            return false;
        }
        const QString rejectDirectory = outputDirectory + QStringLiteral("/rejected");
        if (!QDir().mkdir(rejectDirectory))
        {
            return false;
        }
        outputEdit_->setText(rejectDirectory);
        accept = false;
        state->Push(frames[0]);
        startButton_->click();
        if (!WaitFor([&]()
        {
            return controller_.GetSnapshot().state == pbapp::DecoderState::Stopped;
        }))
        {
            return false;
        }
        return confirmations == 3 && controller_.GetSnapshot().largeOutputConfirmationState == pbapp::LargeOutputConfirmationState::Rejected &&
            QDir(rejectDirectory).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty() && selectorCalls == 1;
    }
#endif

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
        settings_->setValue(QStringLiteral("unifiedOutputDirectory"), outputEdit_->text());
        settings_->setValue(QStringLiteral("unifiedAdvancedExpanded"), advancedToggle_->isChecked());
        event->accept();
    }

private:
#ifdef PB_ENABLE_UNIFIED_GUI_SMOKE
    static bool WaitFor(const std::function<bool()>& predicate)
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
#endif
    static QLabel* Label()
    {
        QLabel* const label = new QLabel();
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        return label;
    }

    void BuildUi()
    {
        QWidget* const central = new QWidget(this);
        QVBoxLayout* const layout = new QVBoxLayout(central);
        layout->setContentsMargins(24, 20, 24, 20);
        layout->setSpacing(12);
        QLabel* const title = new QLabel(QStringLiteral("从选定画面恢复文件"));
        title->setStyleSheet(QStringLiteral("font-size:24px;font-weight:600;"));
        layout->addWidget(title);
        QLabel* const explanation = new QLabel(QStringLiteral("自动识别 Unified，优先 WGC；仅在明确捕获故障时切换 DXGI。停止保留恢复状态。"));
        explanation->setWordWrap(true);
        layout->addWidget(explanation);
        QHBoxLayout* const outputRow = new QHBoxLayout();
        outputEdit_ = new QLineEdit();
        outputEdit_->setObjectName(QStringLiteral("outputDirectory"));
        browseButton_ = new QPushButton(QStringLiteral("输出目录…"));
        outputRow->addWidget(outputEdit_, 1);
        outputRow->addWidget(browseButton_);
        layout->addLayout(outputRow);
        QHBoxLayout* const actionRow = new QHBoxLayout();
        roiButton_ = new QPushButton(QStringLiteral("选择 ROI…"));
        roiButton_->setObjectName(QStringLiteral("selectRoi"));
        startButton_ = new QPushButton(QStringLiteral("开始接收"));
        startButton_->setObjectName(QStringLiteral("startReceive"));
        stopButton_ = new QPushButton(QStringLiteral("停止接收"));
        stopButton_->setObjectName(QStringLiteral("stopReceive"));
        actionRow->addWidget(roiButton_);
        actionRow->addStretch();
        actionRow->addWidget(startButton_);
        actionRow->addWidget(stopButton_);
        layout->addLayout(actionRow);
        roiLabel_ = Label();
        layout->addWidget(roiLabel_);
        stateLabel_ = Label();
        stateLabel_->setStyleSheet(QStringLiteral("font-size:18px;font-weight:600;color:#175CD3;"));
        layout->addWidget(stateLabel_);
        progress_ = new QProgressBar();
        progress_->setRange(0, 1000);
        progress_->setFormat(QStringLiteral("已验证原始字节 %p%"));
        layout->addWidget(progress_);
        QFormLayout* const status = new QFormLayout();
        fileLabel_ = Label();
        recoveryLabel_ = Label();
        goodputLabel_ = Label();
        resumeLabel_ = Label();
        backendLabel_ = Label();
        geometryLabel_ = Label();
        status->addRow(QStringLiteral("文件"), fileLabel_);
        status->addRow(QStringLiteral("已验证 Segment / 字节"), recoveryLabel_);
        status->addRow(QStringLiteral("恢复速度 / ETA"), goodputLabel_);
        status->addRow(QStringLiteral("恢复状态"), resumeLabel_);
        status->addRow(QStringLiteral("实际捕获 backend"), backendLabel_);
        status->addRow(QStringLiteral("末次有效 geometry"), geometryLabel_);
        layout->addLayout(status);
        messageLabel_ = Label();
        layout->addWidget(messageLabel_);
        completionLabel_ = Label();
        layout->addWidget(completionLabel_);
        openButton_ = new QPushButton(QStringLiteral("打开完成文件所在目录"));
        layout->addWidget(openButton_);
        advancedToggle_ = new QPushButton(QStringLiteral("高级信息 / 物理坐标后备"));
        advancedToggle_->setCheckable(true);
        layout->addWidget(advancedToggle_);
        advancedPanel_ = new QWidget();
        QVBoxLayout* const advancedLayout = new QVBoxLayout(advancedPanel_);
        details_ = new QPlainTextEdit();
        details_->setReadOnly(true);
        details_->setObjectName(QStringLiteral("readOnlyRuntimeDetails"));
        advancedLayout->addWidget(details_);
        physicalButton_ = new QPushButton(QStringLiteral("输入物理坐标…"));
        advancedLayout->addWidget(physicalButton_);
        advancedPanel_->hide();
        layout->addWidget(advancedPanel_, 1);
        setCentralWidget(central);
        connect(outputEdit_, &QLineEdit::textChanged, this, &DecoderWindow::UpdateActions);
        connect(browseButton_, &QPushButton::clicked, this, [this]()
        {
            const QString directory = QFileDialog::getExistingDirectory(this, QStringLiteral("选择输出目录"), outputEdit_->text());
            if (!directory.isEmpty())
            {
                outputEdit_->setText(QDir::toNativeSeparators(directory));
            }
        });
        connect(roiButton_, &QPushButton::clicked, this, &DecoderWindow::SelectRoi);
        connect(physicalButton_, &QPushButton::clicked, this, &DecoderWindow::EnterPhysicalRoi);
        connect(startButton_, &QPushButton::clicked, this, &DecoderWindow::StartReceiving);
        connect(stopButton_, &QPushButton::clicked, &controller_, &DecoderApplicationController::RequestStop);
        connect(advancedToggle_, &QPushButton::toggled, advancedPanel_, &QWidget::setVisible);
        connect(openButton_, &QPushButton::clicked, this, [this]()
        {
            const auto snapshot = controller_.GetSnapshot();
            if (IsComplete(snapshot) && !services_.openDirectory(QFileInfo(FromUtf8(snapshot.outputPath)).absolutePath()))
            {
                messageLabel_->setText(QStringLiteral("无法打开目录；请使用上方最终路径手动打开。"));
            }
        });
    }

    static bool IsComplete(const pbapp::DecoderSnapshot& snapshot)
    {
        return snapshot.state == pbapp::DecoderState::Completed && snapshot.wholeFileDigestVerified &&
            snapshot.finalPublishSucceeded && !snapshot.outputPath.empty();
    }

    void SelectRoi()
    {
        if (controller_.IsActive())
        {
            return;
        }
        pbscreenregion::ScreenCaptureRegion selected;
        const auto status = services_.selectRegion(selected);
        if (status)
        {
            region_ = selected;
            hasRegion_ = true;
            UpdateActions();
        }
        else if (status.code != pbscreenregion::ScreenRegionErrorCode::Cancelled)
        {
            messageLabel_->setText(QStringLiteral("ROI 选择失败：%1").arg(QString::fromLatin1(pbscreenregion::GetScreenRegionErrorName(status.code))));
        }
    }

    void EnterPhysicalRoi()
    {
        if (controller_.IsActive())
        {
            return;
        }
        QDialog dialog(this);
        dialog.setWindowTitle(QStringLiteral("物理坐标后备（单显示器内，右/下边界不包含）"));
        QFormLayout layout(&dialog);
        std::array<QSpinBox*, 4> fields{};
        const std::array<int, 4> values{region_.physicalRect.left, region_.physicalRect.top, region_.physicalRect.right, region_.physicalRect.bottom};
        const std::array<QString, 4> names{QStringLiteral("Left"), QStringLiteral("Top"), QStringLiteral("Right"), QStringLiteral("Bottom")};
        for (std::size_t index = 0; index < fields.size(); index++)
        {
            fields[index] = new QSpinBox(&dialog);
            fields[index]->setRange((std::numeric_limits<int>::min)(), (std::numeric_limits<int>::max)());
            fields[index]->setValue(values[index]);
            layout.addRow(names[index], fields[index]);
        }
        QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        layout.addRow(&buttons);
        connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() != QDialog::Accepted)
        {
            return;
        }
        const RECT rectangle{fields[0]->value(), fields[1]->value(), fields[2]->value(), fields[3]->value()};
        pbscreenregion::ScreenCaptureRegion selected;
        const auto status = services_.resolveRegion(rectangle, selected);
        if (status)
        {
            region_ = selected;
            hasRegion_ = true;
            UpdateActions();
        }
        else
        {
            messageLabel_->setText(QStringLiteral("物理 ROI 无效：%1").arg(QString::fromLatin1(pbscreenregion::GetScreenRegionErrorName(status.code))));
        }
    }

    void StartReceiving()
    {
        if (!hasRegion_ || controller_.IsActive())
        {
            return;
        }
        pbscreenregion::ScreenCaptureRegion current;
        const auto resolution = services_.resolveRegion(region_.physicalRect, current);
        if (!resolution)
        {
            messageLabel_->setText(QStringLiteral("显示器布局已变化或 ROI 无效，请重新选择 ROI。"));
            return;
        }
        region_ = current;
        const auto config = pbapp::MakeUnifiedDecoderConfig(outputEdit_->text().toStdWString(), region_);
        const QString error = controller_.Start(config);
        UpdateSnapshot();
        if (!error.isEmpty())
        {
            messageLabel_->setText(error);
        }
    }

    void PromptLargeOutput(const pbapp::DecoderSnapshot& requested)
    {
        const auto current = controller_.GetSnapshot();
        if (current.runGeneration != requested.runGeneration || current.largeOutputConfirmationRequestId != requested.largeOutputConfirmationRequestId ||
            current.state != pbapp::DecoderState::AwaitingLargeOutputConfirmation ||
            current.largeOutputConfirmationState != pbapp::LargeOutputConfirmationState::AwaitingDecision || closePending_)
        {
            return;
        }
        bool accepted = false;
        if (services_.confirmLargeOutput)
        {
            accepted = services_.confirmLargeOutput(current);
        }
        else
        {
            QMessageBox question(QMessageBox::Question, QStringLiteral("确认大文件输出"), QString(), QMessageBox::Yes | QMessageBox::No, this);
            question.setTextFormat(Qt::PlainText);
            question.setText(QStringLiteral("是否为此会话创建或恢复输出文件？\n文件：%1\n大小：%2\n目录：%3\n\n确认前不会创建或扩展 .part，也不会接收 Outer payload。")
                .arg(FromUtf8(current.originalFileNameUtf8)).arg(HumanBytes(current.originalFileBytes)).arg(outputEdit_->text()));
            question.setDefaultButton(QMessageBox::No);
            question.setEscapeButton(QMessageBox::No);
            accepted = question.exec() == QMessageBox::Yes;
        }
        const QString error = controller_.ResolveLargeOutputConfirmation(current.runGeneration, current.largeOutputConfirmationRequestId, accepted);
        UpdateSnapshot();
        if (!error.isEmpty())
        {
            messageLabel_->setText(error);
        }
    }

    void UpdateActions()
    {
        const auto snapshot = controller_.GetSnapshot();
        const bool idle = !controller_.IsActive() && !closePending_;
        outputEdit_->setEnabled(idle);
        browseButton_->setEnabled(idle);
        roiButton_->setEnabled(idle);
        physicalButton_->setEnabled(idle);
        startButton_->setEnabled(idle && hasRegion_ && QFileInfo(outputEdit_->text()).isDir());
        stopButton_->setEnabled(!idle && !closePending_ && snapshot.state != pbapp::DecoderState::Stopping);
        openButton_->setEnabled(IsComplete(snapshot) && !closePending_);
        roiLabel_->setText(!hasRegion_ ? QStringLiteral("尚未选择 ROI；仅点击“选择 ROI”才启动选区器。") :
            QStringLiteral("物理 ROI [%1, %2) × [%3, %4)；DPI %5 × %6")
                .arg(region_.physicalRect.left).arg(region_.physicalRect.right).arg(region_.physicalRect.top)
                .arg(region_.physicalRect.bottom).arg(region_.dpiX).arg(region_.dpiY));
    }

    void UpdateSnapshot()
    {
        const auto snapshot = controller_.GetSnapshot();
        stateLabel_->setText(StateText(snapshot));
        fileLabel_->setText(snapshot.descriptorKnown ? QStringLiteral("%1 — %2").arg(FromUtf8(snapshot.originalFileNameUtf8))
            .arg(HumanBytes(snapshot.originalFileBytes)) : QStringLiteral("等待正式 SessionDescriptor"));
        progress_->setValue(snapshot.recoveryProgress ? static_cast<int>(*snapshot.recoveryProgress * 1000) : 0);
        recoveryLabel_->setText(QStringLiteral("%1 / %2 Segment；%3 / %4 B").arg(snapshot.verifiedSegmentCount)
            .arg(snapshot.segmentCount).arg(snapshot.verifiedRawBytes).arg(snapshot.originalFileBytes));
        goodputLabel_->setText(!controller_.IsActive() ? QStringLiteral("未在接收；已验证结果保留") : QStringLiteral("%1 MiB/s（已验证字节）；ETA %2")
            .arg(snapshot.smoothedVerifiedRawGoodputBytesPerSecond / (1024.0 * 1024.0), 0, 'f', 2)
            .arg(snapshot.etaMilliseconds ? QStringLiteral("%1 s").arg(*snapshot.etaMilliseconds / 1000) : QStringLiteral("等待足够有效数据")));
        resumeLabel_->setText(IsComplete(snapshot) ? QStringLiteral("文件已发布；恢复日志已清理") :
            snapshot.resumeStateLoaded ? QStringLiteral("已加载并校验恢复状态") :
            snapshot.sessionIdHex.empty() ? QStringLiteral("尚未锁定会话") :
            snapshot.resumeStatePath.empty() ? QStringLiteral("已锁定会话；尚未建立恢复文件") : QStringLiteral("已锁定会话；普通停止保留恢复状态"));
        backendLabel_->setText(snapshot.actualBackend ? QString::fromLatin1(pbapp::GetCaptureBackendName(*snapshot.actualBackend)) : QStringLiteral("尚未启动"));
        const auto& geometry = snapshot.observedLocatorGeometry;
        geometryLabel_->setText(FromUtf8(snapshot.geometryStatus) + (geometry.lastScaleX && geometry.lastScaleY ?
            QStringLiteral("；scale %1 × %2").arg(*geometry.lastScaleX, 0, 'f', 3).arg(*geometry.lastScaleY, 0, 'f', 3) : QString()));
        messageLabel_->setText(FromUtf8(snapshot.statusMessage) + (snapshot.errorDetail.empty() ? QString() : QStringLiteral("\n") + FromUtf8(snapshot.errorDetail)));
        completionLabel_->setText(IsComplete(snapshot) ? QStringLiteral("最终路径：%1\n长度：%2 B\nBLAKE3：%3\nWholeFileDigest、安全发布和重新打开复验均通过。")
            .arg(FromUtf8(snapshot.outputPath)).arg(snapshot.originalFileBytes).arg(FromUtf8(snapshot.wholeFileDigestHex)) : QString());
        details_->setPlainText(QStringLiteral("Profile: PB-Unified-LC4-V1 / layout 8（只读）\nCapture policy: Auto（不依赖 Encoder FPS）\n%1\nSession: %2\nCaptureEpoch: %3\nResume generation: %4\nResume path: %5\nUniqueVisualFPS: %6\n停止不会删除恢复状态；丢帧或等待不会自动停止。")
            .arg(FromUtf8(snapshot.backendReason)).arg(FromUtf8(snapshot.sessionIdHex)).arg(snapshot.captureEpoch)
            .arg(snapshot.resumeStateGeneration).arg(FromUtf8(snapshot.resumeStatePath))
            .arg(snapshot.uniqueVisualFps ? QString::number(*snapshot.uniqueVisualFps, 'f', 2) : QStringLiteral("不可用")));
        UpdateActions();
        if (snapshot.state == pbapp::DecoderState::AwaitingLargeOutputConfirmation &&
            snapshot.largeOutputConfirmationState == pbapp::LargeOutputConfirmationState::AwaitingDecision &&
            (lastPromptRun_ != snapshot.runGeneration || lastPromptRequest_ != snapshot.largeOutputConfirmationRequestId))
        {
            lastPromptRun_ = snapshot.runGeneration;
            lastPromptRequest_ = snapshot.largeOutputConfirmationRequestId;
            QTimer::singleShot(0, this, [this, snapshot]()
            {
                PromptLargeOutput(snapshot);
            });
        }
    }

    WindowServices services_;
    DecoderApplicationController controller_;
    std::unique_ptr<QSettings> settings_;
    pbscreenregion::ScreenCaptureRegion region_;
    bool hasRegion_ = false;
    bool closePending_ = false;
    std::uint64_t lastPromptRun_ = 0;
    std::uint64_t lastPromptRequest_ = 0;
    QLineEdit* outputEdit_ = nullptr;
    QPushButton* browseButton_ = nullptr;
    QPushButton* roiButton_ = nullptr;
    QPushButton* startButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QPushButton* openButton_ = nullptr;
    QPushButton* advancedToggle_ = nullptr;
    QPushButton* physicalButton_ = nullptr;
    QWidget* advancedPanel_ = nullptr;
    QPlainTextEdit* details_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* roiLabel_ = nullptr;
    QLabel* stateLabel_ = nullptr;
    QLabel* fileLabel_ = nullptr;
    QLabel* recoveryLabel_ = nullptr;
    QLabel* goodputLabel_ = nullptr;
    QLabel* resumeLabel_ = nullptr;
    QLabel* backendLabel_ = nullptr;
    QLabel* geometryLabel_ = nullptr;
    QLabel* messageLabel_ = nullptr;
    QLabel* completionLabel_ = nullptr;
};
} // namespace

int RunDecoderGui(const int argumentCount, wchar_t* arguments[])
{
    const bool smoke = argumentCount == 2 && std::wstring_view(arguments[1]) == L"--gui-smoke";
    if (smoke)
    {
#ifndef PB_ENABLE_UNIFIED_GUI_SMOKE
        std::cerr << "G16 GUI smoke was not enabled in this build\n";
        return 2;
#else
        std::array<wchar_t, 32768> modulePath{};
        const DWORD length = GetModuleFileNameW(nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
#ifdef QT_DEBUG
        constexpr const wchar_t* plugin = L"qoffscreend.dll";
#else
        constexpr const wchar_t* plugin = L"qoffscreen.dll";
#endif
        std::error_code error;
        if (length == 0 || length >= modulePath.size() || !std::filesystem::is_regular_file(
            std::filesystem::path(modulePath.data()).parent_path() / L"platforms" / plugin, error) || error)
        {
            std::cerr << "G16 GUI smoke: offscreen platform missing; no QApplication started\n";
            return 2;
        }
        qputenv("QT_QPA_PLATFORM", "offscreen");
#endif
    }
    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    int count = 1;
    char name[] = "PixelBridgeDecoder";
    char* guiArguments[] = {name, nullptr};
    QApplication application(count, guiArguments);
    QCoreApplication::setOrganizationName(QStringLiteral("PixelBridge"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("pixelbridge.local"));
    QCoreApplication::setApplicationName(QStringLiteral("PixelBridgeDecoder"));
#ifdef PB_ENABLE_UNIFIED_GUI_SMOKE
    if (smoke)
    {
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
            services.runtime.outputConfirmationThresholdBytes = 4096;
            int selectorCalls = 0;
            int confirmations = 0;
            int openCalls = 0;
            bool accept = true;
            services.selectRegion = [&](pbscreenregion::ScreenCaptureRegion& region)
            {
                selectorCalls++;
                region = g16test::Region();
                return pbscreenregion::ScreenRegionStatus{};
            };
            services.resolveRegion = [](const RECT&, pbscreenregion::ScreenCaptureRegion& region)
            {
                region = g16test::Region();
                return pbscreenregion::ScreenRegionStatus{};
            };
            services.confirmLargeOutput = [&](const pbapp::DecoderSnapshot& snapshot)
            {
                confirmations++;
                g16test::Check(snapshot.largeOutputConfirmationState == pbapp::LargeOutputConfirmationState::AwaitingDecision,
                    "GUI confirmation bypassed controller request");
                if (accept)
                {
                    state->Push(confirmations == 1 ? frames[0] : frames[1]);
                }
                return accept;
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
            }
            DecoderWindow window(std::move(services), settingsPath);
            const bool passed = window.RunSmoke(outputDirectory, state, frames, bytes, selectorCalls, confirmations, openCalls, accept);
            std::cout << "G16 GUI smoke: " << (passed ? "PASS" : "FAIL")
                << "; offscreen; real controller/runtime; ROI action/confirmation/stop/resume/publish/open-directory/rejection; no selector or capture\n";
            return passed ? 0 : 1;
        }
        catch (const std::exception& exception)
        {
            std::cerr << "G16 GUI smoke failed: " << exception.what() << '\n';
            return 1;
        }
    }
#endif
    DecoderWindow window;
    window.show();
    return application.exec();
}
