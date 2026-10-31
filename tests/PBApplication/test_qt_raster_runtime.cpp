#include "encoder_monitor_catalog.h"
#include "gui_visual_mode.h"
#include "operational_log_qt.h"
#include "qt_raster_test_access.h"
#include "qt_raster_test_support.h"

#include <catch2/catch_test_macros.hpp>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>
#include <thread>

namespace
{
using namespace pbrenderd3d;
using pbencoder::QtRasterPresentationTestAccess;

struct FrameRecord
{
    std::uint64_t sequence = 0;
    std::uint64_t epoch = 0;
    std::vector<std::byte> pixels;
};

struct RuntimeEvidence
{
    std::mutex mutex;
    std::shared_ptr<pbapp::EncoderPresentation> actual;
    std::array<FrameRecord, 2> frames;
    std::size_t nextFrame = 0;
};

class RecordingAdapter final : public pbapp::EncoderPresentation
{
public:
    RecordingAdapter(std::shared_ptr<pbapp::EncoderPresentation> actual, RuntimeEvidence& evidence) : actual_(std::move(actual)), evidence_(evidence)
    {
    }
    DataWindowSnapshot GetSnapshot() const override
    {
        return actual_->GetSnapshot();
    }
    PresentationStatus SubmitFrame(const CanonicalBgraFrameView& frame) override
    {
        const std::lock_guard lock(evidence_.mutex);
        const auto status = actual_->SubmitFrame(frame);
        if (status)
        {
            // Observe the real worker's input, never the renderer's readback.
            // Two bounded records cover current/pending without an image queue.
            auto& recorded = evidence_.frames[evidence_.nextFrame];
            recorded.sequence = frame.frameSequence;
            recorded.epoch = frame.presentationEpoch;
            recorded.pixels.assign(frame.pixels.begin(), frame.pixels.end());
            evidence_.nextFrame = (evidence_.nextFrame + 1) % evidence_.frames.size();
        }
        return status;
    }
    void RequestStop() noexcept override
    {
        actual_->RequestStop();
    }
    void Stop() noexcept override
    {
        actual_->Stop();
    }
private:
    const std::shared_ptr<pbapp::EncoderPresentation> actual_;
    RuntimeEvidence& evidence_;
};

struct RuntimeHarness
{
    RuntimeEvidence evidence;
    pbencoder::QtRasterPresentationHost host;
    pbapp::EncoderRuntime runtime{[this](const DataWindowConfig& config)
    {
        std::shared_ptr<pbapp::EncoderPresentation> actual = host.CreatePresentation(config);
        {
            const std::lock_guard lock(evidence.mutex);
            evidence.actual = actual;
        }
        return std::make_unique<RecordingAdapter>(std::move(actual), evidence);
    }};
    ~RuntimeHarness()
    {
        Stop();
    }
    void Stop()
    {
        runtime.RequestStop();
        host.RequestStop();
        runtime.Stop();
        host.FinalizeStop();
    }
};

QJsonObject ReadObject(const QString& path)
{
    QFile input(path);
    REQUIRE(input.open(QIODevice::ReadOnly));
    const auto document = QJsonDocument::fromJson(input.readAll());
    REQUIRE(document.isObject());
    return document.object();
}
}

TEST_CASE("Real worker rasters reach Qt unchanged for every GUI visual profile", "[qt-runtime]")
{
    pbencoder::test::EnsureApplication();
    std::vector<pbapp::MonitorInfo> monitors;
    REQUIRE(pbapp::EnumerateEncoderMonitors(monitors));
    const auto monitor = std::find_if(monitors.begin(), monitors.end(), [](const pbapp::MonitorInfo& candidate)
    {
        return candidate.physicalRect.right - candidate.physicalRect.left >= 2560 && candidate.physicalRect.bottom - candidate.physicalRect.top >= 1440;
    });
    REQUIRE(monitor != monitors.end());
    QTemporaryDir scratch(QString::fromWCharArray(PB_TEST_SCRATCH_ROOT) + QStringLiteral("/qt-runtime-XXXXXX"));
    REQUIRE(scratch.isValid());
    const auto sourcePath = scratch.filePath(QStringLiteral("immutable.bin"));
    QFile source(sourcePath);
    REQUIRE(source.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    REQUIRE(source.write("real encoding worker to Qt raster") == 33);
    source.close();

    for (int mode = 0; mode < 4; mode++)
    {
        INFO(mode);
        auto config = pbapp::MakeUnifiedEncoderConfig(sourcePath.toStdWString(), 1, PhysicalPoint{monitor->physicalRect.left, monitor->physicalRect.top});
        CHECK(config.presentationModel == PresentationModel::Flip);
        config.singleMonitorFullscreen = *monitor;
        config.sessionStateRoot = scratch.filePath(QStringLiteral("sessions-%1").arg(mode)).toStdWString();
        for (const wchar_t character : monitor->deviceName)
        {
            REQUIRE(character >= 0);
            REQUIRE(character <= 127);
            config.remoteMetadata.experimentMonitorIdentity.push_back(static_cast<char>(character));
        }
        REQUIRE(pbapp::ApplyGuiEncoderVisualMode(config, mode, false));
        config.presentationModel = PresentationModel::QtRaster;
        REQUIRE(pbapp::ValidateEncoderConfig(config));
        WindowEnvironment environment;
        environment.clientWidth = static_cast<std::uint32_t>(monitor->physicalRect.right - monitor->physicalRect.left);
        environment.clientHeight = static_cast<std::uint32_t>(monitor->physicalRect.bottom - monitor->physicalRect.top);
        environment.clientOrigin = *config.monitorClientOrigin;
        environment.dpi = 96;
        environment.modeWidth = environment.clientWidth;
        environment.modeHeight = environment.clientHeight;
        environment.modeFrequency = 60;
        environment.singleMonitor = true;
        environment.monitorIdentity = 1;
        RuntimeHarness harness;
        QtRasterPresentationTestAccess::SetEnvironment(harness.host, environment);
        harness.host.BeginRun();
        pbgui::OperationalLog log(QStringLiteral("Encoder"), scratch.filePath(QStringLiteral("logs-%1").arg(mode)));
        log.Begin();
        REQUIRE(harness.runtime.Start(config));
        const auto creationDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!QtRasterPresentationTestAccess::CreationQueued(harness.host) && std::chrono::steady_clock::now() < creationDeadline &&
            harness.runtime.GetSnapshot().state != pbapp::EncoderState::Failed)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(harness.runtime.GetSnapshot().statusMessage == "Creating the Qt raster Data Window");
        QImage painted;
        std::shared_ptr<pbapp::EncoderPresentation> actual;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline && harness.runtime.GetSnapshot().state != pbapp::EncoderState::Failed)
        {
            pbencoder::test::DrainEvents();
            if (QtRasterPresentationTestAccess::WindowHandle(harness.host) != 0)
            {
                painted = QtRasterPresentationTestAccess::Paint(harness.host);
            }
            {
                const std::lock_guard lock(harness.evidence.mutex);
                actual = harness.evidence.actual;
            }
            if (actual && actual->GetSnapshot().softwareFramesPainted != 0)
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        INFO(harness.runtime.GetSnapshot().errorDetail);
        REQUIRE(actual);
        const auto window = actual->GetSnapshot();
        REQUIRE(window.softwareFramesPainted > 0);
        REQUIRE_FALSE(painted.isNull());
        REQUIRE(painted.width() == static_cast<int>(environment.clientWidth));
        REQUIRE(painted.height() == static_cast<int>(environment.clientHeight));
        REQUIRE(window.contract.presentationModel == PresentationModel::QtRaster);
        CHECK(window.contract.rasterPaintEngine);
        CHECK(window.contract.bufferCount == 0);
        {
            const std::lock_guard lock(harness.evidence.mutex);
            const auto expected = std::find_if(harness.evidence.frames.begin(), harness.evidence.frames.end(), [&](const FrameRecord& frame)
            {
                return frame.sequence == window.activeFrameSequence && frame.epoch == window.activeFramePresentationEpoch && !frame.pixels.empty();
            });
            REQUIRE(expected != harness.evidence.frames.end());
            REQUIRE(expected->pixels.size() == static_cast<std::size_t>(painted.width()) * painted.height() * 4);
            bool identical = true;
            for (int row = 0; row < painted.height() && identical; row++)
            {
                const auto* const rendered = painted.constScanLine(row);
                const auto* const canonical = expected->pixels.data() + static_cast<std::size_t>(row) * painted.width() * 4;
                for (int column = 0; column < painted.width() * 4; column++)
                {
                    const auto value = column % 4 == 3 ? 255 : std::to_integer<unsigned int>(canonical[column]);
                    if (rendered[column] != value)
                    {
                        identical = false;
                        break;
                    }
                }
            }
            CHECK(identical);
            // Holding the observer mutex blocks further submissions, not paint.
            // Repeated OS damage must not claim a new logical frame/equation.
            static_cast<void>(QtRasterPresentationTestAccess::Paint(harness.host));
            const auto repeated = actual->GetSnapshot();
            CHECK(repeated.softwarePaintCalls == window.softwarePaintCalls + 1);
            CHECK(repeated.softwareFramesPainted == window.softwareFramesPainted);
            CHECK(repeated.activeFrameSequence == window.activeFrameSequence);
        }
        log.Observe(harness.runtime.GetSnapshot());
        harness.Stop();
        const auto terminal = harness.runtime.GetSnapshot();
        INFO(terminal.errorDetail);
        REQUIRE(terminal.state == pbapp::EncoderState::Stopped);
        REQUIRE(terminal.presentationContract);
        CHECK(terminal.requestedPresentationModel == PresentationModel::QtRaster);
        CHECK(terminal.presentationContract->presentationModel == PresentationModel::QtRaster);
        CHECK(terminal.softwareFramesPainted > 0);
        CHECK(terminal.successfulPresentCalls == 0);
        CHECK_FALSE(terminal.presentCallFps);
        CHECK_FALSE(terminal.presentedVisualFps);
        CHECK_FALSE(terminal.presentationAdapter);
        CHECK(terminal.presentationWindowState == WindowState::Stopped);
        CHECK_FALSE(actual->GetSnapshot().pendingFrame);
        CHECK_FALSE(actual->GetSnapshot().inFlightFrame);
        CHECK_FALSE(harness.host.HasPendingWork());
        CHECK(QtRasterPresentationTestAccess::WindowHandle(harness.host) == 0);
        log.Observe(terminal, true);
        const auto files = QDir(log.Directory()).entryList({QStringLiteral("*.summary.json")}, QDir::Files);
        REQUIRE(files.size() == 1);
        const auto report = ReadObject(QDir(log.Directory()).filePath(files.front()));
        const auto presentation = report["presentation"].toObject();
        CHECK(report["state"].toString() == QStringLiteral("Stopped"));
        CHECK(presentation["actualModel"].toString() == QStringLiteral("qt-raster"));
        CHECK(presentation["statisticsUnavailableReason"].toString() == QStringLiteral("QtRasterHasNoDxgiStatistics"));
        CHECK(presentation["adapterUnavailableReason"].toString() == QStringLiteral("QtRasterDoesNotUseDxgiAdapter"));
        CHECK(presentation["softwareFramesPainted"].toInteger() > 0);
        CHECK(presentation["presentedVisualFps"].isNull());
        CHECK(presentation["presentCallFps"].isNull());
        CHECK(report["verifiedGoodput"].isNull());
        CHECK(report["receiverProgress"].isNull());
    }
}
