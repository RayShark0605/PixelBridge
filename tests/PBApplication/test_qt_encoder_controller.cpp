#include "encoder_application_controller.h"
#include "qt_raster_test_access.h"
#include "qt_raster_test_support.h"

#include <catch2/catch_test_macros.hpp>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <chrono>
#include <thread>

class EncoderApplicationControllerTestAccess
{
public:
    static pbencoder::QtRasterPresentationHost& Host(EncoderApplicationController& controller)
    {
        return controller.qtPresentationHost_;
    }
    static void PausePolling(EncoderApplicationController& controller)
    {
        controller.pollTimer_.stop();
    }
};

namespace
{
using namespace pbrenderd3d;
using pbencoder::QtRasterPresentationTestAccess;

void ConfigureHiddenHost(EncoderApplicationController& controller)
{
    WindowEnvironment environment;
    environment.clientWidth = 1920;
    environment.clientHeight = 1080;
    environment.dpi = 96;
    environment.modeWidth = 1920;
    environment.modeHeight = 1080;
    environment.modeFrequency = 60;
    environment.singleMonitor = true;
    environment.monitorIdentity = 1;
    QtRasterPresentationTestAccess::SetEnvironment(EncoderApplicationControllerTestAccess::Host(controller), environment);
}

pbapp::EncoderConfig MakeConfig(QTemporaryDir& scratch)
{
    REQUIRE(scratch.isValid());
    const auto path = scratch.filePath(QStringLiteral("fixture.bin"));
    QFile source(path);
    REQUIRE(source.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    REQUIRE(source.write("Qt controller fixture") == 21);
    source.close();
    auto config = pbapp::MakeUnifiedEncoderConfig(path.toStdWString(), 30);
    config.sessionStateRoot = scratch.filePath(QStringLiteral("sessions")).toStdWString();
    config.presentationModel = PresentationModel::QtRaster;
    return config;
}

bool WaitForCreationWithoutGuiEvents(EncoderApplicationController& controller)
{
    auto& host = EncoderApplicationControllerTestAccess::Host(controller);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline && controller.GetSnapshot().state != pbapp::EncoderState::Failed)
    {
        if (QtRasterPresentationTestAccess::CreationQueued(host))
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}
}

TEST_CASE("GUI controller cancels Qt initialization before joining without event pumping", "[qt-controller]")
{
    pbencoder::test::EnsureApplication();
    QTemporaryDir scratch(QString::fromWCharArray(PB_TEST_SCRATCH_ROOT) + QStringLiteral("/qt-controller-XXXXXX"));
    const auto config = MakeConfig(scratch);
    EncoderApplicationController controller(nullptr, {}, scratch.filePath(QStringLiteral("logs")));
    ConfigureHiddenHost(controller);
    auto& host = EncoderApplicationControllerTestAccess::Host(controller);
    for (int iteration = 0; iteration < 2; iteration++)
    {
        REQUIRE(controller.Start(config).isEmpty());
        CHECK(WaitForCreationWithoutGuiEvents(controller));
        const auto start = std::chrono::steady_clock::now();
        const auto snapshot = controller.StopAndGetSnapshot();
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
        CHECK(snapshot.state == pbapp::EncoderState::Stopped);
        CHECK_FALSE(controller.IsActive());
        CHECK_FALSE(host.HasPendingWork());
        pbencoder::test::DrainEvents();
        CHECK(QtRasterPresentationTestAccess::WindowHandle(host) == 0);
    }
}

TEST_CASE("Controller terminal cleanup gap rejects restart until Qt objects are finalized", "[qt-controller]")
{
    pbencoder::test::EnsureApplication();
    QTemporaryDir scratch(QString::fromWCharArray(PB_TEST_SCRATCH_ROOT) + QStringLiteral("/qt-controller-gap-XXXXXX"));
    auto config = MakeConfig(scratch);
    config.maximumRunDurationSeconds = 1;
    EncoderApplicationController controller(nullptr, {}, scratch.filePath(QStringLiteral("logs")));
    ConfigureHiddenHost(controller);
    EncoderApplicationControllerTestAccess::PausePolling(controller);
    auto& host = EncoderApplicationControllerTestAccess::Host(controller);
    REQUIRE(controller.Start(config).isEmpty());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline && pbapp::IsEncoderStateActive(controller.GetSnapshot().state))
    {
        pbencoder::test::DrainEvents();
        if (QtRasterPresentationTestAccess::WindowHandle(host) != 0)
        {
            static_cast<void>(QtRasterPresentationTestAccess::Paint(host));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const auto terminal = controller.GetSnapshot();
    INFO(terminal.errorDetail);
    REQUIRE(terminal.state == pbapp::EncoderState::Stopped);
    CHECK(terminal.stoppedByTimeout);
    CHECK(terminal.softwareFramesPainted > 0);
    CHECK(terminal.successfulPresentCalls == 0);
    CHECK_FALSE(terminal.presentedVisualFps.has_value());
    CHECK(host.HasPendingWork());
    CHECK(controller.IsActive());
    CHECK_FALSE(controller.Start(config).isEmpty());
    CHECK(controller.GetSnapshot().runGeneration == terminal.runGeneration);
    const auto finalized = controller.StopAndGetSnapshot();
    CHECK(finalized.state == pbapp::EncoderState::Stopped);
    CHECK_FALSE(controller.IsActive());
    CHECK_FALSE(host.HasPendingWork());
    CHECK(QtRasterPresentationTestAccess::WindowHandle(host) == 0);
}

TEST_CASE("Controller rejection and destruction cannot leave Qt creation callbacks alive", "[qt-controller]")
{
    pbencoder::test::EnsureApplication();
    QTemporaryDir scratch(QString::fromWCharArray(PB_TEST_SCRATCH_ROOT) + QStringLiteral("/qt-controller-exit-XXXXXX"));
    const auto config = MakeConfig(scratch);
    auto controller = std::make_unique<EncoderApplicationController>(nullptr, pbapp::EncoderPresentationFactory{}, scratch.filePath(QStringLiteral("logs")));
    ConfigureHiddenHost(*controller);
    auto invalid = config;
    invalid.sourcePath.clear();
    CHECK_FALSE(controller->Start(invalid).isEmpty());
    CHECK_FALSE(controller->IsActive());
    CHECK_FALSE(EncoderApplicationControllerTestAccess::Host(*controller).HasPendingWork());
    const auto initialWindows = QApplication::topLevelWidgets().size();
    REQUIRE(controller->Start(config).isEmpty());
    CHECK(WaitForCreationWithoutGuiEvents(*controller));
    const auto start = std::chrono::steady_clock::now();
    controller.reset();
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
    pbencoder::test::DrainEvents();
    CHECK(QApplication::topLevelWidgets().size() == initialWindows);
}

TEST_CASE("Qt backend failures retain actual authority in terminal controller logs", "[qt-controller]")
{
    pbencoder::test::EnsureApplication();
    QTemporaryDir scratch(QString::fromWCharArray(PB_TEST_SCRATCH_ROOT) + QStringLiteral("/qt-controller-failure-XXXXXX"));
    const auto config = MakeConfig(scratch);
    EncoderApplicationController controller(nullptr, {}, scratch.filePath(QStringLiteral("logs")));
    ConfigureHiddenHost(controller);
    auto& host = EncoderApplicationControllerTestAccess::Host(controller);
    REQUIRE(controller.Start(config).isEmpty());
    const auto paintDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < paintDeadline && controller.GetSnapshot().softwareFramesPainted == 0 &&
        controller.GetSnapshot().state != pbapp::EncoderState::Failed)
    {
        pbencoder::test::DrainEvents();
        if (QtRasterPresentationTestAccess::WindowHandle(host) != 0)
        {
            static_cast<void>(QtRasterPresentationTestAccess::Paint(host));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(controller.GetSnapshot().softwareFramesPainted > 0);
    WindowEnvironment invalid;
    invalid.clientWidth = 16385;
    invalid.clientHeight = 1;
    invalid.dpi = 96;
    QtRasterPresentationTestAccess::SetEnvironment(host, invalid);
    static_cast<void>(QtRasterPresentationTestAccess::Paint(host));
    const auto failureDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < failureDeadline && controller.GetSnapshot().state != pbapp::EncoderState::Failed)
    {
        pbencoder::test::DrainEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto terminal = controller.StopAndGetSnapshot();
    REQUIRE(terminal.state == pbapp::EncoderState::Failed);
    CHECK(terminal.errorDetail.find("Qt raster Data Window failed:") != std::string::npos);
    REQUIRE(terminal.presentationContract);
    CHECK(terminal.presentationContract->presentationModel == PresentationModel::QtRaster);
    CHECK(terminal.successfulPresentCalls == 0);
    CHECK_FALSE(controller.IsActive());
    CHECK_FALSE(host.HasPendingWork());
    CHECK(QtRasterPresentationTestAccess::WindowHandle(host) == 0);
    const auto files = QDir(controller.LogDirectory()).entryList({QStringLiteral("*.summary.json")}, QDir::Files);
    REQUIRE(files.size() == 1);
    QFile summary(QDir(controller.LogDirectory()).filePath(files.front()));
    REQUIRE(summary.open(QIODevice::ReadOnly));
    const auto document = QJsonDocument::fromJson(summary.readAll());
    REQUIRE(document.isObject());
    CHECK(document.object()["state"].toString() == QStringLiteral("Failed"));
    CHECK(document.object()["errorDetail"].toString() == QString::fromStdString(terminal.errorDetail));
    const auto presentation = document.object()["presentation"].toObject();
    CHECK(presentation["actualModel"].toString() == QStringLiteral("qt-raster"));
    CHECK(presentation["presentedVisualFps"].isNull());
}
