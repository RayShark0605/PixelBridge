#include "decoder_gui.h"
#include "decoder_application_controller.h"
#include "decoder_progress_text.h"
#include "product_gui_helpers.h"
#include "gui_native_smoke.h"
#include "gui_visual_mode.h"
#include "run_report.h"
#include "step1_gui_evidence_qt.h"
#include "pbcore/build_info.h"

#include <QApplication>
#include <QIcon>
#include <QCloseEvent>
#include <QCheckBox>
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
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSizePolicy>
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
#include <QJsonDocument>
#include <QJsonObject>
#include <fstream>
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
    case pbapp::DecoderState::Verifying:
        if (snapshot.activeRecoveryOperation == "VerifySegment")
        {
            return QStringLiteral("正在校验分段…");
        }
        if (snapshot.activeRecoveryOperation == "WriteSegmentAndCheckpoint" || snapshot.activeRecoveryOperation == "CheckpointResume")
        {
            return QStringLiteral("正在写入文件与断点…");
        }
        return snapshot.captureStallActive || snapshot.visualStallActive ? QStringLiteral("等待画面恢复 · 已保留进度") : QStringLiteral("正在恢复文件");
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
    std::function<pbapp::DecoderMemoryHostSnapshot()> queryMemory = pbapp::QueryDecoderMemoryHostSnapshot;
    std::function<pbapp::MonitorCatalogStatus(std::vector<pbapp::MonitorInfo>&)> enumerateMonitors = pbapp::EnumerateMonitors;
    std::function<pbscreenregion::ScreenRegionStatus(const RECT&, pbscreenregion::ScreenCaptureRegion&)> selectRegion = pbscreenregion::SelectScreenCaptureRegionOnMonitor;
    std::function<pbscreenregion::ScreenRegionStatus(const RECT&, pbscreenregion::ScreenCaptureRegion&)> resolveRegion = pbscreenregion::ResolveScreenCaptureRegion;
    std::function<bool(const QString&)> openDirectory = [](const QString& directory)
    {
        return QDesktopServices::openUrl(QUrl::fromLocalFile(directory));
    };
};

[[nodiscard]] pbapp::DecoderRuntimeServices BindCaptureCatalog(const WindowServices& services)
{
    auto runtime = services.runtime;
    runtime.enumerateMonitors = services.enumerateMonitors;
    return runtime;
}

class DecoderWindow final : public QMainWindow
{
public:
    explicit DecoderWindow(WindowServices services = {}, const QString& settingsPath = {}, pbgui::Step1GuiEvidence* evidence = nullptr) :
        services_(std::move(services)), controller_(nullptr, BindCaptureCatalog(services_)), evidence_(evidence)
    {
        settings_ = settingsPath.isEmpty() ? std::make_unique<QSettings>() : std::make_unique<QSettings>(settingsPath, QSettings::IniFormat);
        setWindowTitle(QStringLiteral("PixelBridge Decoder v%1").arg(QString::fromStdString(pbcore::GetBuildInfo().version)));
        setMinimumSize(680, 550);
        resize(780, 600);
        BuildUi();
        outputEdit_->setText(settings_->value(QStringLiteral("g22/outputDirectory"),
            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).toString());
        LoadCarrierPreference();
        const int interval = settings_->value(QStringLiteral("g22/statusRefreshMilliseconds"), 250).toInt();
        refreshSpin_->setValue(interval >= 100 && interval <= 2000 ? interval : 250);
        LoadMemoryPreferences();
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
    [[nodiscard]] bool VerifyMainLayout(const bool expectAllVisible = true)
    {
        const int previousTab = tabs_->currentIndex();
        tabs_->setCurrentIndex(0);
        // Exercise layout even when optional preview export is disabled. grab()
        // renders only this offscreen widget, never the user's desktop.
        ensurePolished();
        QCoreApplication::processEvents();
        static_cast<void>(grab());
        auto* const scroll = qobject_cast<QScrollArea*>(tabs_->widget(0));
        if (!scroll || !scroll->widget())
        {
            return false;
        }
        const auto* const page = scroll->widget();
        bool passed = !expectAllVisible || (scroll->verticalScrollBar()->maximum() == 0 && scroll->horizontalScrollBar()->maximum() == 0);
        const auto labels = page->findChildren<QLabel*>(QString(), Qt::FindDirectChildrenOnly);
        for (const auto* label : labels)
        {
            if (label->text().isEmpty())
            {
                continue;
            }
            const int requiredHeight = (std::max)({label->fontMetrics().height(), label->minimumSizeHint().height(), label->heightForWidth(label->width())});
            if (label->height() < requiredHeight || !page->rect().contains(label->geometry()))
            {
                std::cerr << "Decoder layout clipped: " << label->text().toStdString() << "; size=" << label->width() << 'x'
                    << label->height() << "; requiredHeight=" << requiredHeight << '\n';
                passed = false;
            }
        }
        if (!passed)
        {
            std::cerr << "Decoder layout: window=" << width() << 'x' << height() << "; page=" << page->width() << 'x' << page->height()
                << "; scroll=" << scroll->horizontalScrollBar()->maximum() << ',' << scroll->verticalScrollBar()->maximum() << '\n';
        }
        tabs_->setCurrentIndex(previousTab);
        return passed;
    }

    [[nodiscard]] bool RunCarrierPreferenceSmoke()
    {
        if (carrierCombo_->currentIndex() != pbapp::defaultGuiVisualModeIndex)
        {
            return false;
        }
        for (int index = 0; index < 4; index++)
        {
            carrierCombo_->setCurrentIndex(index);
            SavePreferences();
            carrierCombo_->setCurrentIndex(index == 0 ? 1 : 0);
            LoadCarrierPreference();
            if (carrierCombo_->currentIndex() != index || !carrierPreferencesWarning_.isEmpty())
            {
                return false;
            }
        }
        for (const auto& invalid : {QStringLiteral("invalid"), QStringLiteral("-1"), QStringLiteral("4"),
            QStringLiteral("2147483648"), QStringLiteral("2.5"), QString()})
        {
            settings_->setValue(QStringLiteral("g22/carrier"), invalid);
            LoadCarrierPreference();
            UpdateSnapshot();
            if (carrierCombo_->currentIndex() != pbapp::defaultGuiVisualModeIndex ||
                !modeHintLabel_->text().contains(QStringLiteral("保存的模式无效")))
            {
                return false;
            }
        }
        const bool preview = VerifyMainLayout() && pbgui::SaveTabPreviews(*this, *tabs_, QStringLiteral("decoder-invalid-preference"));
        carrierCombo_->setCurrentIndex(1);
        carrierCombo_->setCurrentIndex(0);
        return preview && carrierPreferencesWarning_.isEmpty() && !isVisible() && controller_.GetSnapshot().runGeneration == 0;
    }

    void RunActivityLayoutSmoke()
    {
        pbapp::DecoderSnapshot snapshot;
        snapshot.state = pbapp::DecoderState::Verifying;
        g16test::Check(StateText(snapshot) == QStringLiteral("正在恢复文件"), "stale Verifying must not claim active verification");
        snapshot.activeRecoveryOperation = "VerifySegment";
        g16test::Check(StateText(snapshot) == QStringLiteral("正在校验分段…"), "active segment verification heading missing");
        snapshot.activeRecoveryOperation.clear();
        snapshot.captureStallActive = true;
        g16test::Check(StateText(snapshot).contains(QStringLiteral("等待画面恢复")), "capture interruption hidden by stale Verifying");
        const std::array<pbapp::DecoderActivity, 6> activities{{
            {"CollectingRepairSymbols", true, 12000, 0}, {"WaitingForCarousel", true, 12000, 6000},
            {"CheckpointResume", true, 12000, 6000}, {"CaptureInterrupted", false, 12000, 6000},
            {"NoUsefulProgress", false, 40000, 40000}, {"ResourceBackpressure", false, 12000, 6000}}};
        const QSize originalSize = size();
        for (const auto& activity : activities)
        {
            stateLabel_->setText(QStringLiteral("正在恢复文件"));
            activityLabel_->setText(pbgui::FormatDecoderActivity(activity));
            for (const bool minimum : {false, true})
            {
                resize(minimum ? QSize(680, 550) : originalSize);
                // Long diagnostic explanations may scroll at the minimum
                // size, but every line must retain its full height.
                g16test::Check(VerifyMainLayout(!minimum), "Decoder activity explanation clipped");
                if (!minimum && (activity.code == "CollectingRepairSymbols" || activity.code == "CaptureInterrupted"))
                {
                    g16test::Check(pbgui::SaveTabPreviews(*this, *tabs_, QStringLiteral("decoder-activity-") + QString::fromStdString(activity.code)),
                        "Decoder activity preview failed");
                }
            }
        }
        resize(originalSize);
        UpdateSnapshot();
    }

    [[nodiscard]] bool RunPam4Smoke(const QString& scratchDirectory, const int index, const std::shared_ptr<g16test::ReceiveState>& state, int& openCalls)
    {
        const auto profile = pbapp::GetGuiVisualProfile(index);
        g16test::Check(profile && pbapp::IsExperimentalPam4Family(*profile), "GUI PAM4 profile missing");
        g16test::Check(!isVisible() && carrierCombo_->currentIndex() == pbapp::defaultGuiVisualModeIndex &&
            memoryMode_->isEnabled() && monitors_.size() == 1,
            "GUI PAM4 default or single-monitor fixture mismatch");
        const auto root = std::filesystem::path(scratchDirectory.toStdWString());
        for (const auto* name : {L"output", L"probe-output", L"state"})
        {
            g16test::Check(std::filesystem::create_directory(root / name), "GUI PAM4 scratch child unavailable");
        }
        outputEdit_->setText(QString::fromStdWString((root / L"output").wstring()));
        monitorCombo_->setCurrentIndex(1);
        wholeButton_->click();
        carrierCombo_->setCurrentIndex(-1);
        StartOrStop();
        g16test::Check(!startButton_->isEnabled() && controller_.GetSnapshot().runGeneration == 0 &&
            messageLabel_->text().contains(QStringLiteral("接收模式无效")), "GUI invalid selection must not start");
        carrierCombo_->setCurrentIndex(index);
        g16test::Check(hasRegion_ && EqualRect(&region_.physicalRect, &monitors_[0].physicalRect) && memoryMode_->isEnabled(),
            "GUI PAM4 full-screen selection unavailable");
        memoryMode_->setChecked(true);
        instanceMemorySpin_->setValue(64);
        totalMemorySpin_->setValue(256);
        g16test::Check(startButton_->isEnabled(), "GUI PAM4 finite budget rejected");
        const QString prefix = index == 3 ? QStringLiteral("decoder-pam4-wide") : QStringLiteral("decoder-pam4");
        const QSize originalSize = size();
        for (const bool minimum : {false, true})
        {
            resize(minimum ? QSize(680, 550) : originalSize);
            g16test::Check(VerifyMainLayout(), "GUI PAM4 idle layout clipped");
            g16test::Check(pbgui::SaveTabPreviews(*this, *tabs_, prefix + (minimum ? QStringLiteral("-minimum") : QStringLiteral("-idle"))),
                "GUI PAM4 idle preview failed");
        }
        resize(originalSize);
        if (index == 2)
        {
            RunActivityLayoutSmoke();
        }
        const auto sourceBytes = g16test::RawBytes(192U * 1024U);
        const auto source = root / L"gui-single-screen-pixels.bin";
        {
            std::ofstream file(source, std::ios::binary);
            file.write(reinterpret_cast<const char*>(sourceBytes.data()), static_cast<std::streamsize>(sourceBytes.size()));
            g16test::Check(file.good(), "GUI PAM4 source write failed");
        }
        const int previousOpens = openCalls;
        startButton_->click();
        g16test::Check(WaitFor([&]() { return controller_.GetSnapshot().singleMonitorCaptureRevalidationCount >= 2; }),
            "GUI PAM4 capture did not start/revalidate");
        UpdateSnapshot();
        const auto started = controller_.GetSnapshot();
        g16test::Check(started.visualProfile == *profile && started.singleMonitorCapturePreflightPassed &&
            !started.monitorSafetyPreflightPassed && started.customDecoderMemoryBudget && started.budgetBoundDecoderAdmission &&
            started.outerTotalDecoderByteLimit == 256 * pbapp::decoderMemoryMebibyte && started.outerPerDecoderByteLimit == 64 * pbapp::decoderMemoryMebibyte &&
            !carrierCombo_->isEnabled() && !monitorCombo_->isEnabled() && !wholeButton_->isEnabled() && !roiButton_->isEnabled() &&
            !outputEdit_->isEnabled() && !memoryMode_->isEnabled() && !totalMemorySpin_->isEnabled() && !instanceMemorySpin_->isEnabled() &&
            !recommendMemoryButton_->isEnabled() && !coordinateApply_->isEnabled(), "GUI PAM4 runtime binding or active lock mismatch");
        {
            const std::scoped_lock lock(state->mutex);
            g16test::Check(state->sessionStarts == 1 && state->requestedDemod.visualProfileId == started.visualProfileId &&
                EqualRect(&state->requestedCapture.capture.region.physicalRect, &region_.physicalRect), "GUI full-screen capture binding mismatch");
        }
        g16test::Check(VerifyMainLayout() && pbgui::SaveTabPreviews(*this, *tabs_, prefix + QStringLiteral("-active")), "GUI PAM4 active layout failed");
        pbapp::ExperimentalPam4FileProbeOptions options;
        options.visualProfile = *profile;
        options.maximumFrames = 96;
        std::uint64_t delivered = 0;
        options.decodedFrameForTest = [&](const pbdemodd3d11::CaptureDemodulatorResult& frame)
        {
            // The live GUI receiver receives only decoded raster observations
            // through its OS/GPU test edge, not source bytes or expected hashes.
            g16test::Check(frame.bootstrap.IsAccepted(), "GUI pixel probe rejected Bootstrap");
            state->Push(frame);
            delivered++;
            g16test::Check(WaitFor([&]() { return state->Delivered() == delivered; }), "GUI did not consume pixel observation");
        };
        pbapp::ExperimentalPam4FileProbeSnapshot probe;
        const auto status = pbapp::ApplicationRuntimeTestAccess::ProbeExperimentalPam4File(source.wstring(), root / L"state",
            (root / L"probe-output").wstring(), options, probe);
        g16test::Check(status && probe.independentlyReopenedEqual, status.message.c_str());
        g16test::Check(WaitFor([&]() { return controller_.GetSnapshot().state == pbapp::DecoderState::Completed; }), "GUI PAM4 file did not complete");
        const auto completed = controller_.StopAndGetSnapshot();
        UpdateSnapshot();
        g16test::Check(g16test::VerifyOutput(completed, sourceBytes) && completed.finalReopenVerified == true &&
            delivered == probe.decodedFrames && completed.visualProfile == *profile && completed.singleMonitorCaptureStatus == "PASS" &&
            progressLabel_->text() == QStringLiteral("100%") && etaLabel_->text() == QStringLiteral("00:00:00") &&
            openCalls == previousOpens && openButton_->isEnabled() && carrierCombo_->isEnabled() && monitorCombo_->isEnabled() &&
            memoryMode_->isEnabled() && totalMemorySpin_->isEnabled() && instanceMemorySpin_->isEnabled(), "GUI PAM4 completion or unlock mismatch");
        const auto report = QJsonDocument::fromJson(QByteArray::fromStdString(pbapp::BuildDecoderRunReportJson({}, completed))).object();
        g16test::Check(report.value("singleMonitorCapture").toObject().value("enabled").toBool() &&
            report.value("singleMonitorCapture").toObject().value("preflightPassed").toBool() &&
            report.value("monitorSafety").toObject().value("status").toString() == QStringLiteral("NotApplicableSingleMonitorCapture"),
            "GUI PAM4 report capture authority mismatch");
        for (const bool minimum : {false, true})
        {
            resize(minimum ? QSize(680, 550) : originalSize);
            g16test::Check(VerifyMainLayout() && pbgui::SaveTabPreviews(*this, *tabs_, prefix +
                (minimum ? QStringLiteral("-completed-minimum") : QStringLiteral("-completed"))), "GUI PAM4 completed layout failed");
        }
        SavePreferences();
        DecoderWindow restored(services_, settings_->fileName());
        g16test::Check(restored.carrierCombo_->currentIndex() == index && restored.memoryMode_->isChecked() &&
            restored.totalMemorySpin_->value() == 256 && restored.instanceMemorySpin_->value() == 64 &&
            !restored.hasRegion_ && !restored.controller_.IsActive(), "GUI PAM4 preference reload must not auto-start or trust old ROI");
        return !isVisible() && !QApplication::activeModalWidget();
    }

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
        // The legacy smoke exercises the preserved Standard path explicitly;
        // it must not depend on the fresh-install PAM4 default.
        carrierCombo_->setCurrentIndex(0);
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
            !VerifyMainLayout() || !pbgui::SaveTabPreviews(*this, *tabs_, QStringLiteral("decoder-idle")))
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
            !openButton_->isEnabled() || openCalls != 0 || !VerifyMainLayout() || !pbgui::SaveTabPreviews(*this, *tabs_, QStringLiteral("decoder-completed")))
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
    [[nodiscard]] bool RunMemoryBudgetSmoke(const QString& outputDirectory, const std::shared_ptr<g16test::ReceiveState>& state)
    {
        if (isVisible() || memoryMode_->isChecked() || totalMemorySpin_->value() != 1024 || instanceMemorySpin_->value() != 512 || memoryMode_->isEnabled())
        {
            return false;
        }
        outputEdit_->setText(outputDirectory);
        carrierCombo_->setCurrentIndex(1);
        wholeButton_->click();
        memoryMode_->setChecked(true);
        totalMemorySpin_->setValue(256);
        if (startButton_->isEnabled() || !memoryBudgetLabel_->text().contains(QStringLiteral("不能超过")))
        {
            return false;
        }
        recommendMemoryButton_->click();
        if (totalMemorySpin_->value() <= 1024 || instanceMemorySpin_->value() != 512 || !startButton_->isEnabled())
        {
            return false;
        }
        totalMemorySpin_->setValue(2048);
        instanceMemorySpin_->setValue(1024);
        SavePreferences();
        totalMemorySpin_->setValue(4096);
        LoadMemoryPreferences();
        if (totalMemorySpin_->value() != 2048 || instanceMemorySpin_->value() != 1024 || !memoryMode_->isChecked())
        {
            return false;
        }
        settings_->setValue(QStringLiteral("receiverMemory/totalMiB"), QStringLiteral("18446744073709551615"));
        LoadMemoryPreferences();
        UpdateActions();
        if (memoryMode_->isChecked() || totalMemorySpin_->value() != 1024 || !memoryBudgetLabel_->text().contains(QStringLiteral("保存的内存设置无效")))
        {
            return false;
        }
        memoryMode_->setChecked(true);
        totalMemorySpin_->setValue(2048);
        instanceMemorySpin_->setValue(1024);
        if (!pbgui::SaveTabPreviews(*this, *tabs_, QStringLiteral("decoder-memory-budget")))
        {
            return false;
        }
        std::uint64_t previousStarts = 0;
        {
            const std::scoped_lock lock(state->mutex);
            previousStarts = state->sessionStarts;
        }
        startButton_->click();
        if (!WaitFor([&]()
        {
            const std::scoped_lock lock(state->mutex);
            return state->sessionStarts > previousStarts;
        }))
        {
            std::cerr << "memory GUI start: " << controller_.GetSnapshot().errorDetail << '\n';
            return false;
        }
        UpdateSnapshot();
        const auto started = controller_.GetSnapshot();
        const bool bound = started.budgetBoundDecoderAdmission && started.customDecoderMemoryBudget &&
            started.outerTotalDecoderByteLimit == 2048 * pbapp::decoderMemoryMebibyte &&
            started.outerPerDecoderByteLimit == 1024 * pbapp::decoderMemoryMebibyte &&
            started.receiverResumeByteLimit == 512 * pbapp::decoderMemoryMebibyte && started.memoryHostSnapshotAvailable &&
            started.memoryHostAvailablePhysicalBytes != 0 && started.memoryPlanningBytes == 4608 * pbapp::decoderMemoryMebibyte &&
            !memoryMode_->isEnabled() && !totalMemorySpin_->isEnabled() && !instanceMemorySpin_->isEnabled() && !recommendMemoryButton_->isEnabled();
        const bool preview = bound && pbgui::SaveTabPreviews(*this, *tabs_, QStringLiteral("decoder-memory-active"));
        const auto stopped = controller_.StopAndGetSnapshot();
        UpdateSnapshot();
        return preview && stopped.state == pbapp::DecoderState::Stopped && totalMemorySpin_->isEnabled() &&
            !QApplication::activeModalWidget() && !isVisible();
    }
    [[nodiscard]] bool RunMeasurementStartSmoke(const std::shared_ptr<g16test::ReceiveState>& state)
    {
        if (!evidence_ || isVisible() || memoryMode_->isEnabled() || recommendMemoryButton_->isEnabled())
        {
            return false;
        }
        outputEdit_->setText(QFileInfo(evidence_->SettingsPath()).absolutePath());
        monitorCombo_->setCurrentIndex(1);
        wholeButton_->click();
        for (const int index : {2, 3})
        {
            carrierCombo_->setCurrentIndex(index);
            if (startButton_->isEnabled() || memoryMode_->isEnabled())
            {
                return false;
            }
            StartOrStop();
            if (controller_.GetSnapshot().runGeneration != 0 || !evidence_->RunId().isEmpty() ||
                !messageLabel_->text().contains(QStringLiteral("不支持正式测量")))
            {
                return false;
            }
        }
        carrierCombo_->setCurrentIndex(0);
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
            started.measurement && started.measurement->runGeneration != 0 && !started.budgetBoundDecoderAdmission && !started.customDecoderMemoryBudget &&
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
        outer->setContentsMargins(24, 16, 24, 16);
        outer->setSpacing(12);
        QLabel* const heading = pbgui::TextLabel(QStringLiteral("文件接收"));
        heading->setStyleSheet(QStringLiteral("font-size:24px;font-weight:600;"));
        QHBoxLayout* const branding = new QHBoxLayout();
        QLabel* const logo = new QLabel();
        logo->setObjectName(QStringLiteral("applicationLogo"));
        logo->setPixmap(QPixmap(QStringLiteral(":/branding/logo.png")).scaled(40, 40, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        logo->setFixedSize(40, 40);
        branding->addWidget(logo);
        branding->addWidget(heading, 1);
        outer->addLayout(branding);
        tabs_ = new QTabWidget();
        tabs_->setObjectName(QStringLiteral("decoderTabs"));
        QWidget* const mainPage = new QWidget();
        QVBoxLayout* const mainLayout = new QVBoxLayout(mainPage);
        mainLayout->setContentsMargins(12, 12, 12, 12);
        mainLayout->setSpacing(6);
        mainLayout->setSizeConstraint(QLayout::SetMinAndMaxSize);
        QHBoxLayout* const carrierRow = new QHBoxLayout();
        carrierRow->addWidget(pbgui::TextLabel(QStringLiteral("接收模式")));
        carrierCombo_ = new QComboBox();
        carrierCombo_->setObjectName(QStringLiteral("carrierMode"));
        carrierCombo_->addItem(QStringLiteral("标准"));
        carrierCombo_->addItem(QStringLiteral("灰阶高速"));
        carrierCombo_->addItem(QStringLiteral("PAM4"));
        carrierCombo_->addItem(QStringLiteral("PAM4 Wide"));
        carrierCombo_->setToolTip(QStringLiteral("必须与编码端选择同一模式，否则无法建立会话。"));
        carrierRow->addWidget(carrierCombo_, 1);
        mainLayout->addLayout(carrierRow);
        modeHintLabel_ = pbgui::TextLabel();
        modeHintLabel_->setObjectName(QStringLiteral("visualModeHint"));
        modeHintLabel_->setToolTip(QStringLiteral("单屏也可以整屏接收：先启动 Decoder，再让远控传输画面完整可见；无需预留 Decoder 界面区域。程序不自动移动或最小化窗口。Wide 发送端码面仍为 2560×1440，接收端接受某个 ROI 尺寸不等于已认证所有缩放链路。"));
        mainLayout->addWidget(modeHintLabel_);
        QHBoxLayout* const outputRow = new QHBoxLayout();
        outputRow->addWidget(pbgui::TextLabel(QStringLiteral("保存目录")));
        outputEdit_ = new QLineEdit();
        outputEdit_->setObjectName(QStringLiteral("outputDirectory"));
        browseButton_ = new QPushButton(QStringLiteral("选择目录…"));
        outputRow->addWidget(outputEdit_, 1);
        outputRow->addWidget(browseButton_);
        mainLayout->addLayout(outputRow);
        QHBoxLayout* const monitorRow = new QHBoxLayout();
        monitorRow->addWidget(pbgui::TextLabel(QStringLiteral("接收屏幕")));
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
        progress->setVerticalSpacing(6);
        progressLabel_ = pbgui::TextLabel();
        progressLabel_->setObjectName(QStringLiteral("recoveryPercent"));
        receivedLabel_ = pbgui::TextLabel();
        receivedLabel_->setObjectName(QStringLiteral("receivedSize"));
        receivedLabel_->setToolTip(QStringLiteral("已验证字节 + 按已接纳唯一传输块估算的接收中字节；整文件摘要验证通过前不会达到文件大小。"));
        elapsedLabel_ = pbgui::TextLabel();
        elapsedLabel_->setObjectName(QStringLiteral("elapsedTime"));
        speedLabel_ = pbgui::TextLabel();
        speedLabel_->setToolTip(QStringLiteral("本次运行的平均接收速度（含接收中估算）；按 Windows 单位换算，1 KB = 1024 B，不是屏幕帧率或收包速率。"));
        etaLabel_ = pbgui::TextLabel();
        const std::array<QLabel*, 5> values{progressLabel_, receivedLabel_, elapsedLabel_, speedLabel_, etaLabel_};
        const std::array<QString, 5> captions{QStringLiteral("当前进度"), QStringLiteral("已接收大小"),
            QStringLiteral("已花费时间"), QStringLiteral("当前平均速度"), QStringLiteral("预估剩余时间")};
        for (std::size_t index = 0; index < values.size(); index++)
        {
            QLabel* const caption = pbgui::TextLabel(captions[index]);
            caption->setWordWrap(false);
            for (auto* label : {caption, values[index]})
            {
                auto policy = label->sizePolicy();
                policy.setVerticalPolicy(QSizePolicy::Minimum);
                label->setSizePolicy(policy);
            }
            progress->addRow(caption, values[index]);
        }
        mainLayout->addLayout(progress);
        activityLabel_ = pbgui::TextLabel();
        activityLabel_->setObjectName(QStringLiteral("receiverActivity"));
        // Wrapped text uses heightForWidth. Minimum would instead retain the
        // narrow preferred-width height and force an unnecessary scrollbar.
        mainLayout->addWidget(activityLabel_);
        messageLabel_ = pbgui::TextLabel();
        mainLayout->addWidget(messageLabel_);
        mainLayout->addStretch();
        // Long errors or larger fonts must remain reachable instead of
        // squeezing the verified progress rows below their text height.
        QScrollArea* const mainScroll = new QScrollArea();
        mainScroll->setFrameShape(QFrame::NoFrame);
        mainScroll->setWidgetResizable(true);
        mainScroll->setWidget(mainPage);
        tabs_->addTab(mainScroll, QStringLiteral("接收"));

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
        QHBoxLayout* const memoryModeRow = new QHBoxLayout();
        memoryMode_ = new QCheckBox(QStringLiteral("按内存预算接收（性能模式）"));
        memoryMode_->setObjectName(QStringLiteral("budgetBoundDecoders"));
        memoryMode_->setToolTip(QStringLiteral("取消固定 8 个活动解码器名额，按有限内存预算接纳未完成分段；不增加发送数据量，不是线程数。适用于灰阶高速、PAM4 和 PAM4 Wide，默认关闭。"));
        recommendMemoryButton_ = new QPushButton(QStringLiteral("按本机可用内存建议"));
        memoryModeRow->addWidget(memoryMode_);
        memoryModeRow->addStretch();
        memoryModeRow->addWidget(recommendMemoryButton_);
        advancedLayout->addLayout(memoryModeRow);
        QHBoxLayout* const memoryBudgetRow = new QHBoxLayout();
        memoryBudgetRow->addWidget(pbgui::TextLabel(QStringLiteral("活动解码器总预算")));
        totalMemorySpin_ = new QSpinBox();
        totalMemorySpin_->setObjectName(QStringLiteral("decoderMemoryMib"));
        totalMemorySpin_->setRange(4, static_cast<int>(pbapp::maximumDecoderMemoryBudgetBytes / pbapp::decoderMemoryMebibyte));
        totalMemorySpin_->setValue(1024);
        totalMemorySpin_->setSuffix(QStringLiteral(" MiB"));
        memoryBudgetRow->addWidget(totalMemorySpin_, 1);
        memoryBudgetRow->addWidget(pbgui::TextLabel(QStringLiteral("单实例上限")));
        instanceMemorySpin_ = new QSpinBox();
        instanceMemorySpin_->setObjectName(QStringLiteral("decoderInstanceMemoryMib"));
        instanceMemorySpin_->setRange(1, totalMemorySpin_->maximum());
        instanceMemorySpin_->setValue(512);
        instanceMemorySpin_->setSuffix(QStringLiteral(" MiB"));
        instanceMemorySpin_->setToolTip(QStringLiteral("通常保留 512 MiB 即可；提高单实例上限不会自动提高吞吐。必须不超过总预算。"));
        memoryBudgetRow->addWidget(instanceMemorySpin_, 1);
        advancedLayout->addLayout(memoryBudgetRow);
        memoryBudgetLabel_ = pbgui::TextLabel();
        memoryBudgetLabel_->setWordWrap(true);
        advancedLayout->addWidget(memoryBudgetLabel_);
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
        QPushButton* const logsButton = new QPushButton(QStringLiteral("打开日志目录"));
        logsButton->setObjectName(QStringLiteral("openLogs"));
        connect(logsButton, &QPushButton::clicked, this, [this]()
        {
            QDesktopServices::openUrl(QUrl::fromLocalFile(controller_.LogDirectory()));
        });
        advancedLayout->addWidget(logsButton);
        details_ = new QPlainTextEdit();
        details_->setObjectName(QStringLiteral("decoderDiagnostics"));
        details_->setReadOnly(true);
        advancedLayout->addWidget(details_, 1);
        tabs_->addTab(advancedPage, QStringLiteral("高级选项"));
        outer->addWidget(tabs_, 1);
        setCentralWidget(central);

        connect(outputEdit_, &QLineEdit::textChanged, this, &DecoderWindow::UpdateActions);
        connect(carrierCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]()
        {
            carrierPreferencesWarning_.clear();
            UpdateSnapshot();
        });
        connect(memoryMode_, &QCheckBox::toggled, this, [this]() { memoryPreferencesWarning_.clear(); UpdateActions(); });
        for (auto* field : {totalMemorySpin_, instanceMemorySpin_})
        {
            connect(field, QOverload<int>::of(&QSpinBox::valueChanged), this, [this]() { memoryPreferencesWarning_.clear(); UpdateActions(); });
        }
        connect(recommendMemoryButton_, &QPushButton::clicked, this, [this]()
        {
            pbapp::DecoderMemoryBudget budget;
            const auto status = pbapp::RecommendDecoderMemoryBudget(services_.queryMemory(), budget);
            if (status != pbapp::DecoderMemoryBudgetError::None)
            {
                SetMessage(QString::fromUtf8(pbapp::DescribeDecoderMemoryBudgetError(status)));
                return;
            }
            totalMemorySpin_->setValue(static_cast<int>(budget.totalDecoderBytes / pbapp::decoderMemoryMebibyte));
            instanceMemorySpin_->setValue(static_cast<int>(budget.perDecoderBytes / pbapp::decoderMemoryMebibyte));
            memoryMode_->setChecked(true);
            SetMessage(QStringLiteral("已按当前可用物理内存和 commit 建议预算；启动时将重新检查，未预分配这些内存。"));
        });
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

    [[nodiscard]] pbapp::DecoderMemoryBudget GetMemoryBudget() const
    {
        return {static_cast<std::uint64_t>(totalMemorySpin_->value()) * pbapp::decoderMemoryMebibyte,
            static_cast<std::uint64_t>(instanceMemorySpin_->value()) * pbapp::decoderMemoryMebibyte};
    }

    void LoadMemoryPreferences()
    {
        const QString enabled = settings_->value(QStringLiteral("receiverMemory/enabled"), false).toString();
        bool totalValid = false;
        bool instanceValid = false;
        const auto totalMib = settings_->value(QStringLiteral("receiverMemory/totalMiB"), 1024).toULongLong(&totalValid);
        const auto instanceMib = settings_->value(QStringLiteral("receiverMemory/instanceMiB"), 512).toULongLong(&instanceValid);
        const auto maximumMib = pbapp::maximumDecoderMemoryBudgetBytes / pbapp::decoderMemoryMebibyte;
        const bool rangeValid = totalValid && instanceValid && totalMib <= maximumMib && instanceMib <= maximumMib;
        const bool valid = rangeValid && (enabled == QStringLiteral("true") || enabled == QStringLiteral("false") ||
            enabled == QStringLiteral("1") || enabled == QStringLiteral("0")) &&
            pbapp::ValidateDecoderMemoryBudget({totalMib * pbapp::decoderMemoryMebibyte, instanceMib * pbapp::decoderMemoryMebibyte}) == pbapp::DecoderMemoryBudgetError::None;
        totalMemorySpin_->setValue(valid ? static_cast<int>(totalMib) : 1024);
        instanceMemorySpin_->setValue(valid ? static_cast<int>(instanceMib) : 512);
        memoryMode_->setChecked(valid && (enabled == QStringLiteral("true") || enabled == QStringLiteral("1")));
        if (!valid)
        {
            memoryPreferencesWarning_ = QStringLiteral("保存的内存设置无效：已关闭性能模式并恢复 1024 / 512 MiB，请检查后重新选择。\n");
        }
    }

    void SavePreferences()
    {
        settings_->setValue(QStringLiteral("g22/outputDirectory"), outputEdit_->text());
        settings_->setValue(QStringLiteral("g22/carrier"), carrierCombo_->currentIndex());
        settings_->setValue(QStringLiteral("g22/statusRefreshMilliseconds"), refreshSpin_->value());
        settings_->setValue(QStringLiteral("receiverMemory/enabled"), memoryMode_->isChecked());
        settings_->setValue(QStringLiteral("receiverMemory/totalMiB"), totalMemorySpin_->value());
        settings_->setValue(QStringLiteral("receiverMemory/instanceMiB"), instanceMemorySpin_->value());
        settings_->sync();
    }

    void LoadCarrierPreference()
    {
        bool converted = false;
        const int savedCarrier = settings_->value(QStringLiteral("g22/carrier"), pbapp::defaultGuiVisualModeIndex).toString().toInt(&converted);
        const bool valid = converted && pbapp::GetGuiVisualProfile(savedCarrier).has_value();
        carrierCombo_->setCurrentIndex(valid ? savedCarrier : pbapp::defaultGuiVisualModeIndex);
        carrierPreferencesWarning_ = valid ? QString() : QStringLiteral("保存的模式无效，已恢复 PAM4 模式；请确认后再开始。\n");
        UpdateActions();
    }

    void StartOrStop()
    {
        if (controller_.IsActive())
        {
            controller_.RequestStop();
            return;
        }
        const auto profile = pbapp::GetGuiVisualProfile(carrierCombo_->currentIndex());
        if (!profile || (evidence_ && pbapp::IsExperimentalPam4Family(*profile)))
        {
            SetMessage(!profile ? QStringLiteral("接收模式无效，请重新选择。") :
                QStringLiteral("PAM4 模式不支持正式测量入口；请使用普通启动方式，或明确选择标准模式。"));
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
        config.visualProfile = *profile;
        if (pbapp::IsExperimentalPam4Family(*profile))
        {
            config.singleMonitorCapture = *current;
            config.remoteMetadata.channelType = pbapp::ChannelType::RemoteVisual;
            config.remoteMetadata.remoteProvider = "Unspecified";
            config.remoteMetadata.experimentMonitorIdentity = QString::fromStdWString(current->deviceName).toUtf8().toStdString();
        }
        if (!evidence_ && (*profile == pbapp::VisualProfile::UnifiedGrayFast || pbapp::IsExperimentalPam4Family(*profile)) && memoryMode_->isChecked())
        {
            config.budgetBoundDecoders = true;
            config.memoryBudget = GetMemoryBudget();
            const auto status = pbapp::ValidateDecoderMemoryBudget(*config.memoryBudget);
            if (status != pbapp::DecoderMemoryBudgetError::None)
            {
                SetMessage(QString::fromUtf8(pbapp::DescribeDecoderMemoryBudgetError(status)));
                return;
            }
        }
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
        const auto profile = pbapp::GetGuiVisualProfile(carrierCombo_->currentIndex());
        const bool pam4 = profile && pbapp::IsExperimentalPam4Family(*profile);
        const bool memoryModeAvailable = !evidence_ && profile && (*profile == pbapp::VisualProfile::UnifiedGrayFast || pam4);
        modeHintLabel_->setText(carrierPreferencesWarning_ + (pam4 ?
            QStringLiteral("双端选择同名模式。单屏可整屏接收；启动后让远控码面完整可见。") :
            QStringLiteral("双端选择同一模式；无有效画面时持续等待，停止后保留断点。")) +
            (profile == pbapp::VisualProfile::ExperimentalPam4Wide ? QStringLiteral("\nWide 发送端码面需 2560×1440；单屏接收不改变此要求。") : QString()) +
            (pam4 && evidence_ ? QStringLiteral("\n此模式不支持正式测量入口。") : QString()));
        const bool memoryModeSelected = memoryModeAvailable && memoryMode_->isChecked();
        const auto memoryStatus = pbapp::ValidateDecoderMemoryBudget(GetMemoryBudget());
        memoryMode_->setEnabled(idle && memoryModeAvailable);
        recommendMemoryButton_->setEnabled(idle && memoryModeAvailable);
        totalMemorySpin_->setEnabled(idle && memoryModeSelected);
        instanceMemorySpin_->setEnabled(idle && memoryModeSelected);
        memoryBudgetLabel_->setText(memoryPreferencesWarning_ + (!memoryModeAvailable ?
            QStringLiteral("性能模式适用于灰阶高速和 PAM4 模式，不参与正式测量；当前使用原有默认资源策略。") : !memoryModeSelected ?
            QStringLiteral("当前使用原有默认策略：最多 8 个活动解码器，总预算 1024 MiB，单实例 512 MiB。") :
            memoryStatus != pbapp::DecoderMemoryBudgetError::None ? QString::fromUtf8(pbapp::DescribeDecoderMemoryBudgetError(memoryStatus)) :
            QStringLiteral("断点日志上限 %1 MiB；含恢复副本和捕获余量的规划约 %2 MiB（不是实测内存或整个进程硬上限）。\n预算越大不一定越快；避免换页，设置在下次启动时生效。")
                .arg(pbapp::CalculateDecoderResumeBudgetBytes(GetMemoryBudget()) / pbapp::decoderMemoryMebibyte)
                .arg(pbapp::CalculateDecoderMemoryPlanningBytes(GetMemoryBudget()) / pbapp::decoderMemoryMebibyte)));
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
            (active || (profile && (!evidence_ || !pam4) && hasRegion_ && QFileInfo(outputEdit_->text()).isDir() &&
                (!memoryModeSelected || memoryStatus == pbapp::DecoderMemoryBudgetError::None))));
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
        activityLabel_->setText(pbgui::FormatDecoderActivity(controller_.GetActivity()));
        const auto progress = pbgui::FormatDecoderProgress(snapshot,
            QDateTime::currentMSecsSinceEpoch());
        progressLabel_->setText(progress.percent);
        receivedLabel_->setText(progress.received);
        elapsedLabel_->setText(progress.elapsed);
        speedLabel_->setText(progress.speed);
        etaLabel_->setText(progress.remaining);
        if (snapshot.state == pbapp::DecoderState::Failed)
        {
            SetMessage(pbgui::FromUtf8(snapshot.errorDetail.empty() ? snapshot.statusMessage : snapshot.errorDetail));
        }
        else if (pbgui::IsVerifiedCompletion(snapshot))
        {
            SetMessage(QStringLiteral("已保存：%1").arg(QFileInfo(pbgui::FromUtf8(snapshot.outputPath)).fileName()) +
                (snapshot.errorDetail.empty() ? QString() : QStringLiteral("\n收尾警告：") + pbgui::FromUtf8(snapshot.errorDetail)));
        }
        else
        {
            SetMessage({});
        }
        const QString identity = snapshot.runGeneration == 0 ? QString() :
            QStringLiteral("本次／上次接收：%1 · layout %2\n").arg(QString::fromUtf8(pbapp::GetVisualProfileName(snapshot.visualProfile))).arg(snapshot.visualLayoutVersion);
        details_->setPlainText(controller_.LogStatusText() + QStringLiteral("\n\n") + identity + QStringLiteral(
            "捕获：Auto，WGC 优先；只在明确的后端故障时切换 DXGI\n"
            "资源上限：500 GB；所有支持的文件大小均无需再次确认\n"
            "进度 = 已验证字节 + 接收中估算（唯一传输块 × 块字节 × 压缩比）；100% 仍需整文件摘要、安全发布与最终重开复验\n"
            "1 KB = 1024 B；样本不足或画面停滞时不猜测剩余时间\n\n") +
            QStringLiteral("单屏支持整屏或区域捕获；接收可在后台进行，但远控码面须保持可见。\n无有效画面时持续等待，停止后保留断点；程序不自动移动、最小化窗口或操作输入。\n\n") +
            (snapshot.runGeneration == 0 ? QStringLiteral("尚未开始接收。捕获及恢复详情将在启动后显示。") :
                QStringLiteral("活动解码器：%1 / %2（峰值 %3）；FEC 预算占用 %4 / %5 MiB\n单实例上限 %6 MiB；断点日志上限 %7 MiB；资源暂缓 %8 次\n\n")
                    .arg(snapshot.outerActiveDecoderCount).arg(snapshot.outerActiveDecoderLimit).arg(snapshot.outerPeakActiveDecoderCount)
                    .arg(snapshot.outerReservedDecoderBytes / pbapp::decoderMemoryMebibyte).arg(snapshot.outerTotalDecoderByteLimit / pbapp::decoderMemoryMebibyte)
                    .arg(snapshot.outerPerDecoderByteLimit / pbapp::decoderMemoryMebibyte).arg(snapshot.receiverResumeByteLimit / pbapp::decoderMemoryMebibyte)
                    .arg(snapshot.outerDeferredResourceBusyCount) + pbgui::FromUtf8(pbapp::BuildDecoderDiagnostics(snapshot))));
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
    QLabel* modeHintLabel_ = nullptr;
    QString carrierPreferencesWarning_;
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
    QCheckBox* memoryMode_ = nullptr;
    QPushButton* recommendMemoryButton_ = nullptr;
    QSpinBox* totalMemorySpin_ = nullptr;
    QSpinBox* instanceMemorySpin_ = nullptr;
    QLabel* memoryBudgetLabel_ = nullptr;
    QString memoryPreferencesWarning_;
    std::array<QSpinBox*, 4> coordinates_{};
    QLabel* roiLabel_ = nullptr;
    QLabel* stateLabel_ = nullptr;
    QLabel* activityLabel_ = nullptr;
    QLabel* progressLabel_ = nullptr;
    QLabel* receivedLabel_ = nullptr;
    QLabel* elapsedLabel_ = nullptr;
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
    QStandardPaths::setTestModeEnabled(smoke);
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
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/branding/logo.png")));
    if (QApplication::windowIcon().isNull())
    {
        std::cerr << "Application branding resource unavailable\n";
        return 2;
    }
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
            services.queryMemory = []()
            {
                return pbapp::DecoderMemoryHostSnapshot{true, 96 * 1024 * pbapp::decoderMemoryMebibyte,
                    64 * 1024 * pbapp::decoderMemoryMebibyte, 48 * 1024 * pbapp::decoderMemoryMebibyte};
            };
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
            const bool memoryPassed = legacyPassed && window.RunMemoryBudgetSmoke(outputDirectory, state);
            DecoderWindow preferenceWindow(services, scratch.filePath(QStringLiteral("carrier-preferences.ini")));
            const bool preferencePassed = preferenceWindow.RunCarrierPreferenceSmoke();
            for (const int index : {2, 3})
            {
                const auto singleState = std::make_shared<g16test::ReceiveState>();
                auto singleServices = services;
                singleServices.runtime = g16test::Services(singleState);
                pbapp::MonitorInfo monitor;
                monitor.monitor = g16test::Region().monitor;
                monitor.deviceName = L"GUI-SINGLE-SCREEN-NOT-A-REAL-MONITOR";
                monitor.physicalRect = index == 3 ? RECT{0, 0, 2560, 1600} : RECT{0, 0, 1920, 1080};
                monitor.workRect = monitor.physicalRect;
                monitor.dpiX = 96;
                monitor.dpiY = 96;
                monitor.refreshRate = 60;
                monitor.rotation = DXGI_MODE_ROTATION_IDENTITY;
                monitor.adapterLuid = {1, 0};
                monitor.primary = true;
                singleServices.enumerateMonitors = [monitor](std::vector<pbapp::MonitorInfo>& monitors)
                {
                    monitors = {monitor};
                    return pbapp::MonitorCatalogStatus{};
                };
                singleServices.resolveRegion = [monitor](const RECT& rectangle, pbscreenregion::ScreenCaptureRegion& region)
                {
                    region = {monitor.monitor, rectangle, monitor.physicalRect, monitor.dpiX, monitor.dpiY, monitor.rotation};
                    return pbscreenregion::ScreenRegionStatus{};
                };
                const QString singleRoot = scratch.filePath(QStringLiteral("single-%1").arg(index));
                g16test::Check(QDir().mkdir(singleRoot), "GUI PAM4 scratch unavailable");
                DecoderWindow singleWindow(std::move(singleServices), QDir(singleRoot).filePath(QStringLiteral("settings.ini")));
                g16test::Check(singleWindow.RunPam4Smoke(singleRoot, index, singleState, openCalls), "GUI PAM4 flow failed");
            }
            QString evidenceError;
            const auto smokeEvidence = pbgui::Step1GuiEvidence::Create(scratch.filePath(QStringLiteral("measurement")), QStringLiteral("Decoder"), evidenceError);
            g16test::Check(static_cast<bool>(smokeEvidence), evidenceError.toStdString().c_str());
            const auto measuredState = std::make_shared<g16test::ReceiveState>();
            services.runtime = g16test::Services(measuredState);
            DecoderWindow measuredWindow(std::move(services), smokeEvidence->SettingsPath(), smokeEvidence.get());
            const bool passed = measuredWindow.RunMeasurementStartSmoke(measuredState) && memoryPassed && preferencePassed;
            std::cout << "G22 Decoder GUI smoke: " << (passed ? "PASS" : "FAIL")
                << "; offscreen; real runtime/storage; tabs/monitor/scoped-ROI/status-interval/stop/resume/verified-completion/no-auto-open/no-size-confirmation/measurement-Start-RunId/memory-settings-recommend-validation-restore-runtime-lock/PAM4-Wide-single-monitor-whole-screen-pixel-file-reopen/strict-profile-preferences/experimental-measurement-rejection/default-minimum-layout\n";
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
