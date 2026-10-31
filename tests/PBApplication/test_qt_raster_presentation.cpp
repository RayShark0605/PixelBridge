#include "qt_raster_test_access.h"
#include "qt_raster_test_support.h"
#include "encoder_monitor_catalog.h"

#include <catch2/catch_test_macros.hpp>
#include <Windows.h>
#include <chrono>
#include <future>
#include <thread>
#include <vector>
#include <limits>

namespace
{
using namespace pbencoder;
using namespace pbrenderd3d;

DataWindowConfig MakeConfig()
{
    DataWindowConfig config;
    config.presentationModel = PresentationModel::QtRaster;
    config.width = 4;
    config.height = 2;
    config.waitTimeoutMilliseconds = 1000;
    return config;
}

WindowEnvironment MakeEnvironment()
{
    WindowEnvironment environment;
    environment.clientOrigin = {-400, -200};
    environment.clientWidth = 4;
    environment.clientHeight = 2;
    environment.dpi = 96;
    environment.modeWidth = 1920;
    environment.modeHeight = 1080;
    environment.modeFrequency = 60;
    environment.singleMonitor = true;
    environment.monitorIdentity = 1;
    return environment;
}

bool WaitQueued(const QtRasterPresentationHost& host)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (QtRasterPresentationTestAccess::CreationQueued(host))
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

std::unique_ptr<pbapp::EncoderPresentation> Create(QtRasterPresentationHost& host, const DataWindowConfig& config)
{
    auto creation = std::async(std::launch::async, [&] { return host.CreatePresentation(config); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (creation.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready && std::chrono::steady_clock::now() < deadline)
    {
        pbencoder::test::DrainEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (creation.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
    {
        host.RequestStop();
    }
    REQUIRE(creation.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    return creation.get();
}
}

TEST_CASE("Stop before queued Qt creation never blocks GUI join", "[qt-lifecycle]")
{
    pbencoder::test::EnsureApplication();
    QtRasterPresentationHost host;
    QtRasterPresentationTestAccess::SetEnvironment(host, MakeEnvironment());
    host.BeginRun();
    const auto config = MakeConfig();
    auto creation = std::async(std::launch::async, [&] { return host.CreatePresentation(config); });
    CHECK(WaitQueued(host));
    host.RequestStop();
    REQUIRE(creation.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    auto adapter = creation.get();
    REQUIRE(adapter);
    CHECK(adapter->GetSnapshot().state == WindowState::Stopped);
    CHECK(host.HasPendingWork());
    host.FinalizeStop();
    host.FinalizeStop();
    pbencoder::test::DrainEvents();
    CHECK_FALSE(host.HasPendingWork());
    CHECK(QtRasterPresentationTestAccess::WindowHandle(host) == 0);
    CHECK(QtRasterPresentationTestAccess::OwnedFrameBytes(host) == 0);
}

TEST_CASE("Qt creation timeout cannot create a late window and restart stays isolated", "[qt-lifecycle]")
{
    pbencoder::test::EnsureApplication();
    QtRasterPresentationHost host;
    QtRasterPresentationTestAccess::SetEnvironment(host, MakeEnvironment());
    host.BeginRun();
    auto config = MakeConfig();
    config.waitTimeoutMilliseconds = 100;
    auto creation = std::async(std::launch::async, [&] { return host.CreatePresentation(config); });
    REQUIRE(creation.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    auto oldAdapter = creation.get();
    REQUIRE(oldAdapter);
    CHECK(oldAdapter->GetSnapshot().error.code == PresentationErrorCode::Timeout);
    host.FinalizeStop();
    host.BeginRun();
    config.waitTimeoutMilliseconds = 1000;
    auto adapter = Create(host, config);
    REQUIRE(adapter->GetSnapshot().state == WindowState::Running);
    oldAdapter->RequestStop();
    CHECK(adapter->GetSnapshot().state == WindowState::Running);
    host.RequestStop();
    adapter->Stop();
    host.FinalizeStop();
    pbencoder::test::DrainEvents();
    CHECK_FALSE(host.HasPendingWork());
}

TEST_CASE("Qt presentation owns submitted pixels and bounds wakeups", "[qt-lifecycle]")
{
    pbencoder::test::EnsureApplication();
    QtRasterPresentationHost host;
    QtRasterPresentationTestAccess::SetEnvironment(host, MakeEnvironment());
    host.BeginRun();
    auto adapter = Create(host, MakeConfig());
    REQUIRE(adapter->GetSnapshot().state == WindowState::Running);
    const auto epoch = adapter->GetSnapshot().timing.presentationEpoch;
    std::vector<std::byte> pixels(40, std::byte{0x20});
    for (std::uint64_t sequence = 1; sequence <= 1000; sequence++)
    {
        REQUIRE(adapter->SubmitFrame({pixels, 4, 2, 20, sequence, epoch}));
    }
    std::fill(pixels.begin(), pixels.end(), std::byte{0xee});
    CHECK(QtRasterPresentationTestAccess::PendingWakeups(host) == 1);
    CHECK(QtRasterPresentationTestAccess::OwnedFrameBytes(host) == 64);
    CHECK(adapter->GetSnapshot().replacedPendingFrames == 999);
    const auto image = QtRasterPresentationTestAccess::Paint(host);
    REQUIRE(image.size() == QSize(4, 2));
    for (int y = 0; y < 2; y++)
    {
        for (int x = 0; x < 4; x++)
        {
            CHECK(image.pixel(x, y) == 0xff202020U);
        }
    }
    const auto painted = adapter->GetSnapshot();
    CHECK(painted.activeFrameSequence == 1000);
    CHECK(painted.softwareFramesPainted == 1);
    CHECK_FALSE(painted.pendingFrame);
    static_cast<void>(QtRasterPresentationTestAccess::Paint(host));
    CHECK(adapter->GetSnapshot().softwareFramesPainted == 1);
    CHECK(adapter->GetSnapshot().softwarePaintCalls > painted.softwarePaintCalls);
    CHECK(adapter->GetSnapshot().totalSuccessfulPresents == 0);
    REQUIRE(adapter->SubmitFrame({pixels, 4, 2, 20, 1000, epoch}));
    static_cast<void>(QtRasterPresentationTestAccess::Paint(host));
    CHECK(adapter->GetSnapshot().softwareFramesPainted == 1);
    host.RequestStop();
    CHECK_FALSE(adapter->SubmitFrame({pixels, 4, 2, 20, 1001, epoch}));
    host.FinalizeStop();
    CHECK(QtRasterPresentationTestAccess::OwnedFrameBytes(host) == 0);
}

TEST_CASE("Closing an old Qt run cannot cancel a restart from its stop callback", "[qt-lifecycle]")
{
    pbencoder::test::EnsureApplication();
    QtRasterPresentationHost host;
    QtRasterPresentationTestAccess::SetEnvironment(host, MakeEnvironment());
    host.BeginRun();
    auto oldAdapter = Create(host, MakeConfig());
    REQUIRE(oldAdapter->GetSnapshot().state == WindowState::Running);
    QObject::connect(&host, &QtRasterPresentationHost::StopRequested, &host, [&]
    {
        host.RequestStop();
        host.FinalizeStop();
        host.BeginRun();
    });
    QtRasterPresentationTestAccess::CloseWindow(host);
    pbencoder::test::DrainEvents();
    auto adapter = Create(host, MakeConfig());
    CHECK(adapter->GetSnapshot().state == WindowState::Running);
    CHECK(oldAdapter->GetSnapshot().state == WindowState::Stopped);
    host.RequestStop();
    host.FinalizeStop();
}

TEST_CASE("Partial QWidget damage cannot promote an incomplete new logical frame", "[qt-lifecycle]")
{
    pbencoder::test::EnsureApplication();
    QtRasterPresentationHost host;
    QtRasterPresentationTestAccess::SetEnvironment(host, MakeEnvironment());
    host.BeginRun();
    auto adapter = Create(host, MakeConfig());
    REQUIRE(adapter->GetSnapshot().state == WindowState::Running);
    const auto epoch = adapter->GetSnapshot().timing.presentationEpoch;
    std::vector<std::byte> pixels(32, std::byte{0x22});
    REQUIRE(adapter->SubmitFrame({pixels, 4, 2, 16, 1, epoch}));
    static_cast<void>(QtRasterPresentationTestAccess::Paint(host));
    std::fill(pixels.begin(), pixels.end(), std::byte{0x66});
    REQUIRE(adapter->SubmitFrame({pixels, 4, 2, 16, 2, epoch}));
    const auto partial = QtRasterPresentationTestAccess::PaintRegion(host, QRect(0, 0, 2, 2));
    CHECK(adapter->GetSnapshot().pendingFrame);
    CHECK(adapter->GetSnapshot().activeFrameSequence == 1);
    CHECK(adapter->GetSnapshot().softwareFramesPainted == 1);
    CHECK(partial.pixel(0, 0) == 0xff222222U);
    const auto complete = QtRasterPresentationTestAccess::Paint(host);
    CHECK_FALSE(adapter->GetSnapshot().pendingFrame);
    CHECK(adapter->GetSnapshot().activeFrameSequence == 2);
    CHECK(adapter->GetSnapshot().softwareFramesPainted == 2);
    CHECK(complete.pixel(3, 1) == 0xff666666U);
    host.RequestStop();
    host.FinalizeStop();
}

TEST_CASE("Qt malformed configuration and duplicate creation never disturb a live generation", "[qt-lifecycle]")
{
    pbencoder::test::EnsureApplication();
    QtRasterPresentationHost host;
    QtRasterPresentationTestAccess::SetEnvironment(host, MakeEnvironment());
    for (int mutation = 0; mutation < 10; mutation++)
    {
        host.BeginRun();
        auto invalid = MakeConfig();
        switch (mutation)
        {
        case 0: invalid.width = 0; break;
        case 1: invalid.height = 16385; break;
        case 2: invalid.maximumFrameBytes = 31; break;
        case 3: invalid.waitTimeoutMilliseconds = 0; break;
        case 4: invalid.waitTimeoutMilliseconds = 60001; break;
        case 5: invalid.clientOrigin = PhysicalPoint{(std::numeric_limits<std::int32_t>::min)(), 0}; break;
        case 6: invalid.presentationModel = PresentationModel::Flip; break;
        case 7: invalid.maximumFrameBytes = (std::numeric_limits<std::uint64_t>::max)(); break;
        case 8: invalid.clientOrigin = PhysicalPoint{(std::numeric_limits<std::int32_t>::max)(), 0}; break;
        case 9: invalid.clientOrigin = PhysicalPoint{0, (std::numeric_limits<std::int32_t>::max)()}; break;
        }
        if (mutation != 3 && mutation != 4)
        {
            invalid.waitTimeoutMilliseconds = 100;
        }
        auto creation = std::async(std::launch::async, [&] { return host.CreatePresentation(invalid); });
        REQUIRE(creation.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
        auto adapter = creation.get();
        CHECK(adapter->GetSnapshot().state == WindowState::Failed);
        CHECK(adapter->GetSnapshot().error.stage == PresentationStage::Configuration);
        CHECK(QtRasterPresentationTestAccess::WindowHandle(host) == 0);
        CHECK(QtRasterPresentationTestAccess::OwnedFrameBytes(host) == 0);
        host.FinalizeStop();
    }
    host.BeginRun();
    auto adapter = Create(host, MakeConfig());
    REQUIRE(adapter->GetSnapshot().state == WindowState::Running);
    {
        auto duplicate = Create(host, MakeConfig());
        CHECK(duplicate->GetSnapshot().state == WindowState::Failed);
    }
    CHECK(adapter->GetSnapshot().state == WindowState::Running);
    const std::vector<std::byte> pixels(32);
    const auto epoch = adapter->GetSnapshot().timing.presentationEpoch;
    CHECK(adapter->SubmitFrame({pixels, 4, 2, 15, 1, epoch}).code == PresentationErrorCode::InvalidFrame);
    CHECK(adapter->SubmitFrame({pixels, 4, 2, 16, 1, epoch}));
    host.RequestStop();
    host.FinalizeStop();
}

TEST_CASE("Qt environment transitions discard old frames before accepting the new epoch", "[qt-lifecycle]")
{
    pbencoder::test::EnsureApplication();
    QtRasterPresentationHost host;
    auto environment = MakeEnvironment();
    QtRasterPresentationTestAccess::SetEnvironment(host, environment);
    host.BeginRun();
    auto adapter = Create(host, MakeConfig());
    REQUIRE(adapter->GetSnapshot().state == WindowState::Running);
    const auto epoch = adapter->GetSnapshot().timing.presentationEpoch;
    const std::vector<std::byte> pixels(32, std::byte{0x22});
    REQUIRE(adapter->SubmitFrame({pixels, 4, 2, 16, 1, epoch}));
    environment.clientWidth = 2;
    QtRasterPresentationTestAccess::SetEnvironment(host, environment);
    static_cast<void>(QtRasterPresentationTestAccess::Paint(host));
    CHECK(adapter->GetSnapshot().state == WindowState::Paused);
    CHECK_FALSE(adapter->GetSnapshot().pendingFrame);
    CHECK_FALSE(adapter->GetSnapshot().activeFrame);
    environment.clientWidth = 4;
    environment.dpi = 120;
    environment.dpiChangeSerial = 1;
    QtRasterPresentationTestAccess::SetEnvironment(host, environment);
    static_cast<void>(QtRasterPresentationTestAccess::Paint(host));
    const auto restored = adapter->GetSnapshot();
    CHECK(restored.state == WindowState::Running);
    CHECK(restored.timing.presentationEpoch > epoch);
    CHECK(adapter->SubmitFrame({pixels, 4, 2, 16, 2, epoch}).code == PresentationErrorCode::EpochMismatch);
    CHECK(adapter->SubmitFrame({pixels, 4, 2, 16, 2, restored.timing.presentationEpoch}));
    auto previousEpoch = restored.timing.presentationEpoch;
    for (int transition = 0; transition < 5; transition++)
    {
        environment.minimized = transition == 0;
        environment.singleMonitor = transition != 1;
        environment.occluded = transition == 2;
        if (transition == 3)
        {
            environment.modeChangeSerial++;
            environment.modeFrequency = 75;
        }
        if (transition == 4)
        {
            environment.monitorIdentity = 2;
        }
        QtRasterPresentationTestAccess::SetEnvironment(host, environment);
        static_cast<void>(QtRasterPresentationTestAccess::Paint(host));
        const auto changed = adapter->GetSnapshot();
        CHECK(changed.timing.presentationEpoch > previousEpoch);
        CHECK(changed.state == (transition < 3 ? WindowState::Paused : WindowState::Running));
        CHECK_FALSE(changed.activeFrame);
        CHECK_FALSE(changed.pendingFrame);
        previousEpoch = changed.timing.presentationEpoch;
    }
    host.RequestStop();
    host.FinalizeStop();
}

TEST_CASE("Qt window close requests cancellation and old callbacks cannot survive destruction", "[qt-lifecycle]")
{
    pbencoder::test::EnsureApplication();
    std::unique_ptr<pbapp::EncoderPresentation> adapter;
    bool stopRequested = false;
    {
        QtRasterPresentationHost host;
        QtRasterPresentationTestAccess::SetEnvironment(host, MakeEnvironment());
        QObject::connect(&host, &QtRasterPresentationHost::StopRequested, &host, [&] { stopRequested = true; host.RequestStop(); });
        host.BeginRun();
        adapter = Create(host, MakeConfig());
        REQUIRE(adapter->GetSnapshot().state == WindowState::Running);
        QtRasterPresentationTestAccess::CloseWindow(host);
        pbencoder::test::DrainEvents();
        CHECK(stopRequested);
        CHECK(adapter->GetSnapshot().state == WindowState::Stopped);
    }
    pbencoder::test::DrainEvents();
    CHECK(adapter->GetSnapshot().state == WindowState::Stopped);
    adapter->Stop();
}

TEST_CASE("Hidden native Qt client has real physical geometry and never becomes visible", "[.qt-hidden]")
{
    auto& application = pbencoder::test::EnsureApplication();
    REQUIRE(application.platformName() == QStringLiteral("windows"));
    for (int iteration = 0; iteration < 3; iteration++)
    {
        QtRasterPresentationHost host;
        QtRasterPresentationTestAccess::UseHiddenWindow(host);
        host.BeginRun();
        auto config = MakeConfig();
        config.width = 320;
        config.height = 240;
        auto adapter = Create(host, config);
        const auto snapshot = adapter->GetSnapshot();
        INFO(GetPresentationErrorName(snapshot.error.code));
        INFO(GetPresentationStageName(snapshot.error.stage));
        INFO(QtRasterPresentationTestAccess::DescribeGeometry(host).toStdString());
        REQUIRE(snapshot.state == WindowState::Running);
        const auto window = reinterpret_cast<HWND>(QtRasterPresentationTestAccess::WindowHandle(host));
        REQUIRE(IsWindow(window));
        CHECK_FALSE(IsWindowVisible(window));
        RECT client{};
        REQUIRE(GetClientRect(window, &client));
        CHECK(client.right == 320);
        CHECK(client.bottom == 240);
        CHECK(snapshot.contract.rasterPaintEngine);
        CHECK(snapshot.contract.perMonitorV2);
        CHECK_FALSE(snapshot.environment.adapterAvailable);
        host.RequestStop();
        adapter->Stop();
        host.FinalizeStop();
        CHECK_FALSE(IsWindow(window));
        CHECK_FALSE(host.HasPendingWork());
    }
}

TEST_CASE("Qt rejects oversized native resizing before changing the backing-store dimensions", "[.qt-hidden]")
{
    REQUIRE(pbencoder::test::EnsureApplication().platformName() == QStringLiteral("windows"));
    for (const bool borderless : {false, true})
    {
        QtRasterPresentationHost host;
        QtRasterPresentationTestAccess::UseHiddenWindow(host);
        host.BeginRun();
        auto config = MakeConfig();
        config.width = 320;
        config.height = 240;
        config.topmost = borderless;
        auto adapter = Create(host, config);
        REQUIRE(adapter->GetSnapshot().state == WindowState::Running);
        const auto window = reinterpret_cast<HWND>(QtRasterPresentationTestAccess::WindowHandle(host));
        RECT allowed{0, 0, 400, 300};
        REQUIRE(AdjustWindowRectExForDpi(&allowed, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)), FALSE,
            static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE)), GetDpiForWindow(window)));
        REQUIRE(SetWindowPos(window, nullptr, 0, 0, allowed.right - allowed.left, allowed.bottom - allowed.top,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE));
        pbencoder::test::DrainEvents();
        static_cast<void>(QtRasterPresentationTestAccess::Paint(host));
        REQUIRE(adapter->GetSnapshot().state == WindowState::Running);
        REQUIRE(SetWindowPos(window, nullptr, 0, 0, 5000, 5000, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE));
        RECT client{};
        REQUIRE(GetClientRect(window, &client));
        // Inspect immediately: no timer, render, or GUI event pumping can be
        // the mechanism that eventually notices the oversized request.
        CHECK(client.right == 400);
        CHECK(client.bottom == 300);
        CHECK(adapter->GetSnapshot().state == WindowState::Failed);
        CHECK(adapter->GetSnapshot().error.code == PresentationErrorCode::ResourceLimit);
        CHECK_FALSE(IsWindowVisible(window));
        host.RequestStop();
        host.FinalizeStop();
    }
}

TEST_CASE("Fractional Qt scaling does not resample or truncate physical client edge pixels", "[.qt-hidden-dpr]")
{
    REQUIRE(pbencoder::test::EnsureApplication().platformName() == QStringLiteral("windows"));
    for (const auto dimensions : {QSize(320, 240), QSize(322, 242), QSize(321, 241)})
    {
        QtRasterPresentationHost host;
        // QT_SCALE_FACTOR is confined to this hidden test process. The seam
        // simulates the corresponding DPI reading, not an OS display change.
        QtRasterPresentationTestAccess::UseNativeDpiScaleSimulation(host);
        host.BeginRun();
        auto config = MakeConfig();
        config.width = static_cast<std::uint32_t>(dimensions.width());
        config.height = static_cast<std::uint32_t>(dimensions.height());
        auto adapter = Create(host, config);
        INFO(QtRasterPresentationTestAccess::DescribeGeometry(host).toStdString());
        INFO(GetPresentationErrorName(adapter->GetSnapshot().error.code));
        REQUIRE(adapter->GetSnapshot().state == WindowState::Running);
        std::vector<std::byte> pixels(static_cast<std::size_t>(config.width) * config.height * 4);
        for (std::uint32_t y = 0; y < config.height; y++)
        {
            for (std::uint32_t x = 0; x < config.width; x++)
            {
                const auto offset = (static_cast<std::size_t>(y) * config.width + x) * 4;
                pixels[offset] = static_cast<std::byte>((x * 13 + y * 7) % 251);
                pixels[offset + 1] = static_cast<std::byte>((x * 3 + y * 11) % 251);
                pixels[offset + 2] = static_cast<std::byte>((x + y) % 251);
            }
        }
        REQUIRE(adapter->SubmitFrame({pixels, config.width, config.height, static_cast<std::size_t>(config.width) * 4, 1,
            adapter->GetSnapshot().timing.presentationEpoch}));
        const auto image = QtRasterPresentationTestAccess::Paint(host);
        const HWND handle = reinterpret_cast<HWND>(QtRasterPresentationTestAccess::WindowHandle(host));
        RECT client{};
        REQUIRE(GetClientRect(handle, &client));
        CHECK(client.right == dimensions.width());
        CHECK(client.bottom == dimensions.height());
        REQUIRE(image.width() >= dimensions.width());
        REQUIRE(image.height() >= dimensions.height());
        std::size_t mismatches = 0;
        for (int y = 0; y < dimensions.height(); y++)
        {
            for (int x = 0; x < dimensions.width(); x++)
            {
                const auto offset = (static_cast<std::size_t>(y) * config.width + x) * 4;
                const auto expected = 0xff000000U | std::to_integer<std::uint32_t>(pixels[offset]) |
                    (std::to_integer<std::uint32_t>(pixels[offset + 1]) << 8) | (std::to_integer<std::uint32_t>(pixels[offset + 2]) << 16);
                if (image.pixel(x, y) != expected)
                {
                    mismatches++;
                }
            }
        }
        CHECK(mismatches == 0);
        CHECK_FALSE(IsWindowVisible(handle));
        host.RequestStop();
        host.FinalizeStop();
    }
}

TEST_CASE("Hidden Qt positioning preserves the requested monitor and physical origin", "[.qt-hidden]")
{
    pbencoder::test::EnsureApplication();
    std::vector<pbapp::MonitorInfo> monitors;
    REQUIRE(pbapp::EnumerateEncoderMonitors(monitors));
    REQUIRE_FALSE(monitors.empty());
    for (const auto& monitor : monitors)
    {
        for (const bool borderless : {false, true})
        {
            QtRasterPresentationHost host;
            QtRasterPresentationTestAccess::UseHiddenWindow(host);
            host.BeginRun();
            auto config = MakeConfig();
            config.width = 320;
            config.height = 240;
            config.topmost = borderless;
            const auto left = static_cast<std::int64_t>(monitor.physicalRect.left) + 50;
            const auto top = static_cast<std::int64_t>(monitor.physicalRect.top) + 50;
            REQUIRE(left <= (std::numeric_limits<std::int32_t>::max)());
            REQUIRE(top <= (std::numeric_limits<std::int32_t>::max)());
            config.clientOrigin = PhysicalPoint{static_cast<std::int32_t>(left), static_cast<std::int32_t>(top)};
            auto adapter = Create(host, config);
            INFO(QtRasterPresentationTestAccess::DescribeGeometry(host).toStdString());
            REQUIRE(adapter->GetSnapshot().state == WindowState::Running);
            CHECK(adapter->GetSnapshot().environment.clientOrigin == config.clientOrigin.value());
            CHECK(adapter->GetSnapshot().environment.singleMonitor);
            const HWND handle = reinterpret_cast<HWND>(QtRasterPresentationTestAccess::WindowHandle(host));
            CHECK_FALSE(IsWindowVisible(handle));
            CHECK((GetWindowLongPtrW(handle, GWL_EXSTYLE) & WS_EX_NOACTIVATE) != 0);
            host.RequestStop();
            host.FinalizeStop();
        }
    }
}
