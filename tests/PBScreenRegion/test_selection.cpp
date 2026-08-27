#include "screen_region_internal.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <random>
#include <vector>

using namespace pbscreenregion;
using namespace pbscreenregion::detail;

namespace
{
HMONITOR MonitorHandle(const std::size_t identity)
{
    // Distinct immutable object addresses are model identities, never Win32
    // handles passed to an OS query. No integer-to-pointer assumption is needed.
    static const std::array<std::byte, 100> identities{};
    return reinterpret_cast<HMONITOR>(const_cast<std::byte*>(&identities.at(identity)));
}

MonitorSnapshot Topology()
{
    MonitorSnapshot snapshot;
    snapshot.count = 2;
    snapshot.monitors[0] = {MonitorHandle(1), {-1920, -1080, 0, 0}, 120, 120, DXGI_MODE_ROTATION_ROTATE90, {}};
    snapshot.monitors[1] = {MonitorHandle(2), {0, -200, 1920, 880}, 168, 168, DXGI_MODE_ROTATION_IDENTITY, {}};
    return snapshot;
}

ScreenCaptureRegion Sentinel()
{
    return {MonitorHandle(99), {1, 2, 3, 4}, {5, 6, 7, 8}, 123, 456, DXGI_MODE_ROTATION_ROTATE180};
}

bool IsSentinel(const ScreenCaptureRegion& region)
{
    const auto sentinel = Sentinel();
    return region.monitor == sentinel.monitor && EqualRect(region.physicalRect, sentinel.physicalRect) &&
           EqualRect(region.monitorPhysicalRect, sentinel.monitorPhysicalRect) && region.dpiX == sentinel.dpiX && region.dpiY == sentinel.dpiY &&
           region.rotation == sentinel.rotation;
}

class FakeBackend final : public ScreenRegionBackend
{
public:
    FakeBackend()
    {
        previews.reserve(65536);
    }
    ScreenRegionStatus CheckAwareness() noexcept override
    {
        return Fault(ScreenRegionStage::DpiAwareness);
    }
    ScreenRegionStatus ReadTopology(MonitorSnapshot& snapshot) noexcept override
    {
        readCalls++;
        const auto status = Fault(readCalls == 1 ? ScreenRegionStage::MonitorEnumeration : ScreenRegionStage::Revalidation);
        if (status)
        {
            snapshot = readCalls == 1 ? initialTopology : finalTopology;
        }
        return status;
    }
    ScreenRegionStatus OpenOverlay(const MonitorSnapshot& snapshot) noexcept override
    {
        openCalls++;
        liveWindows = snapshot.count;
        return Fault(ScreenRegionStage::Window);
    }
    ScreenRegionStatus GetEvent(SelectionEvent& event) noexcept override
    {
        auto status = Fault(ScreenRegionStage::MessageWait);
        if (status)
        {
            status = Fault(ScreenRegionStage::Cursor);
        }
        if (status)
        {
            event = eventIndex < events.size() ? events[eventIndex++] : SelectionEvent{SelectionEventType::Cancel, {}};
        }
        return status;
    }
    ScreenRegionStatus AcquirePointer(POINT) noexcept override
    {
        acquireCalls++;
        captured = true;
        return Fault(ScreenRegionStage::Capture);
    }
    ScreenRegionStatus ReleasePointer() noexcept override
    {
        releaseCalls++;
        captured = false;
        return Fault(ScreenRegionStage::Capture);
    }
    ScreenRegionStatus Draw(const SelectionPreview& preview) noexcept override
    {
        if (previews.size() == previews.capacity())
        {
            return ScreenRegionStatus::Failure(ScreenRegionErrorCode::ResourceLimit, ScreenRegionStage::Paint);
        }
        previews.push_back(preview);
        return Fault(ScreenRegionStage::Paint);
    }
    ScreenRegionStatus Close() noexcept override
    {
        closeCalls++;
        outputUnchangedAtClose = watchedOutput == nullptr || IsSentinel(*watchedOutput);
        liveWindows = 0;
        captured = false;
        return Fault(ScreenRegionStage::Cleanup);
    }
    ScreenRegionStatus Fault(const ScreenRegionStage stage) noexcept
    {
        if (failureStage == stage)
        {
            failureVisits++;
            if (failureVisits == failureOccurrence)
            {
                return ScreenRegionStatus::Failure(failureCode, stage, 5678);
            }
        }
        return ScreenRegionStatus::Success();
    }

    MonitorSnapshot initialTopology = Topology();
    MonitorSnapshot finalTopology = initialTopology;
    std::vector<SelectionEvent> events{
        {SelectionEventType::PointerDown, {-100, -100}}, {SelectionEventType::PointerMove, {-81, -71}}, {SelectionEventType::PointerUp, {-81, -71}}};
    std::vector<SelectionPreview> previews;
    ScreenRegionStage failureStage = ScreenRegionStage::None;
    ScreenRegionErrorCode failureCode = ScreenRegionErrorCode::NativeFailure;
    std::size_t failureOccurrence = 1;
    std::size_t failureVisits = 0;
    std::size_t eventIndex = 0;
    unsigned int readCalls = 0;
    unsigned int openCalls = 0;
    unsigned int closeCalls = 0;
    unsigned int acquireCalls = 0;
    unsigned int releaseCalls = 0;
    std::size_t liveWindows = 0;
    bool captured = false;
    bool outputUnchangedAtClose = false;
    const ScreenCaptureRegion* watchedOutput = nullptr;
};

void CheckClean(const FakeBackend& backend)
{
    CHECK(backend.closeCalls == 1);
    CHECK(backend.liveWindows == 0);
    CHECK_FALSE(backend.captured);
    CHECK(backend.outputUnchangedAtClose);
}
} // namespace

TEST_CASE("Selection commits only after release revalidation and complete cleanup", "[screen-region][selection]")
{
    FakeBackend backend;
    auto output = Sentinel();
    backend.watchedOutput = &output;
    REQUIRE(RunSelection(backend, output));
    CHECK(output.monitor == MonitorHandle(1));
    CHECK(EqualRect(output.physicalRect, RECT{-100, -100, -80, -70}));
    CHECK(EqualRect(output.monitorPhysicalRect, RECT{-1920, -1080, 0, 0}));
    CHECK(output.dpiX == 120);
    CHECK(output.dpiY == 120);
    CHECK(output.rotation == DXGI_MODE_ROTATION_ROTATE90);
    CHECK(backend.readCalls == 2);
    CHECK(backend.acquireCalls == 1);
    CHECK(backend.releaseCalls == 1);
    REQUIRE(backend.previews.size() == 4);
    CHECK_FALSE(backend.previews.back().dragging);
    CHECK(backend.previews.back().validation);
    CheckClean(backend);
}

TEST_CASE("Cross-monitor drag and pure click remain retryable without clipping or implicit acceptance", "[screen-region][selection]")
{
    FakeBackend backend;
    backend.events = {{SelectionEventType::PointerDown, {-10, -100}},  {SelectionEventType::PointerUp, {10, -90}},
                      {SelectionEventType::PointerDown, {-100, -100}}, {SelectionEventType::PointerUp, {-100, -100}},
                      {SelectionEventType::PointerDown, {-81, -71}},   {SelectionEventType::PointerUp, {-100, -100}}};
    auto output = Sentinel();
    backend.watchedOutput = &output;
    REQUIRE(RunSelection(backend, output));
    CHECK(EqualRect(output.physicalRect, RECT{-100, -100, -80, -70}));
    CHECK(backend.acquireCalls == 3);
    CHECK(backend.releaseCalls == 3);
    REQUIRE(backend.previews.size() == 7);
    CHECK(backend.previews[2].validation.code == ScreenRegionErrorCode::NotSingleMonitor);
    CHECK(EqualRect(backend.previews[2].physicalRect, RECT{-10, -100, 11, -89}));
    CHECK(backend.previews[4].validation.code == ScreenRegionErrorCode::InvalidRectangle);
    CHECK_FALSE(backend.previews[4].hasRectangle);
    CheckClean(backend);
}

TEST_CASE("Every OS seam failure preserves exact stage native error and caller output", "[screen-region][failure]")
{
    const std::array<ScreenRegionStage, 10> stages{ScreenRegionStage::DpiAwareness, ScreenRegionStage::MonitorEnumeration,
                                                   ScreenRegionStage::Window,       ScreenRegionStage::MessageWait,
                                                   ScreenRegionStage::Cursor,       ScreenRegionStage::Capture,
                                                   ScreenRegionStage::Paint,        ScreenRegionStage::Revalidation,
                                                   ScreenRegionStage::Cleanup,      ScreenRegionStage::Capture};
    for (std::size_t index = 0; index < stages.size(); index++)
    {
        FakeBackend backend;
        backend.failureStage = stages[index];
        backend.failureOccurrence = index == stages.size() - 1 ? 2 : 1;
        auto output = Sentinel();
        backend.watchedOutput = &output;
        const auto status = RunSelection(backend, output);
        INFO("stage=" << static_cast<unsigned int>(stages[index]));
        CHECK(status == ScreenRegionStatus::Failure(ScreenRegionErrorCode::NativeFailure, stages[index], 5678));
        CHECK(IsSentinel(output));
        if (stages[index] == ScreenRegionStage::DpiAwareness)
        {
            CHECK(backend.readCalls == 0);
            CHECK(backend.openCalls == 0);
        }
        CheckClean(backend);
    }
    FakeBackend exhausted;
    exhausted.failureStage = ScreenRegionStage::Window;
    exhausted.failureCode = ScreenRegionErrorCode::OutOfMemory;
    auto output = Sentinel();
    CHECK(RunSelection(exhausted, output).code == ScreenRegionErrorCode::OutOfMemory);
    CHECK(IsSentinel(output));
}

TEST_CASE("Cancellation at every gesture boundary cannot publish a partial region", "[screen-region][cancel]")
{
    for (std::size_t prefix = 0; prefix < 3; prefix++)
    {
        FakeBackend backend;
        backend.events.resize(prefix);
        backend.events.push_back({SelectionEventType::Cancel, {}});
        auto output = Sentinel();
        backend.watchedOutput = &output;
        CHECK(RunSelection(backend, output) == ScreenRegionStatus::Failure(ScreenRegionErrorCode::Cancelled, ScreenRegionStage::Input));
        CHECK(IsSentinel(output));
        CheckClean(backend);
    }
}

TEST_CASE("DPI rotation monitor and layout changes invalidate in-flight selection", "[screen-region][revalidation]")
{
    for (int mutation = 0; mutation < 6; mutation++)
    {
        FakeBackend backend;
        switch (mutation)
        {
        case 0:
            backend.finalTopology.monitors[0].dpiX = backend.finalTopology.monitors[0].dpiY = 144;
            break;
        case 1:
            backend.finalTopology.monitors[0].rotation = DXGI_MODE_ROTATION_ROTATE270;
            break;
        case 2:
            backend.finalTopology.monitors[0].physicalRect.left--;
            break;
        case 3:
            backend.finalTopology.monitors[0].monitor = MonitorHandle(42);
            break;
        case 4:
            backend.finalTopology.count = 1;
            break;
        case 5:
            backend.finalTopology.monitors[0].deviceName[0] = L'X';
            break;
        }
        auto output = Sentinel();
        backend.watchedOutput = &output;
        CHECK(RunSelection(backend, output) == ScreenRegionStatus::Failure(ScreenRegionErrorCode::DisplayChanged, ScreenRegionStage::Revalidation));
        CHECK(IsSentinel(output));
        CheckClean(backend);
    }
    for (const auto event : {SelectionEventType::DisplayChanged, SelectionEventType::CheckEnvironment})
    {
        FakeBackend backend;
        backend.events.insert(backend.events.begin() + 1, {event, {}});
        if (event == SelectionEventType::CheckEnvironment)
        {
            backend.finalTopology.count = 0;
        }
        auto output = Sentinel();
        CHECK(RunSelection(backend, output).code == ScreenRegionErrorCode::DisplayChanged);
        CHECK(IsSentinel(output));
        CHECK(backend.releaseCalls == 0);
        CHECK_FALSE(backend.captured);
    }
    FakeBackend reordered;
    std::swap(reordered.finalTopology.monitors[0], reordered.finalTopology.monitors[1]);
    reordered.events.insert(reordered.events.begin() + 1, {SelectionEventType::CheckEnvironment, {}});
    ScreenCaptureRegion output;
    CHECK(RunSelection(reordered, output));
}

TEST_CASE("Resolve is noninteractive validates before native allocation and preserves output on cleanup failure", "[screen-region][resolve]")
{
    FakeBackend backend;
    auto output = Sentinel();
    backend.watchedOutput = &output;
    REQUIRE(RunResolve(backend, RECT{-1, -1, 0, 0}, output));
    CHECK(EqualRect(output.physicalRect, RECT{-1, -1, 0, 0}));
    CHECK(backend.openCalls == 0);
    CHECK(backend.acquireCalls == 0);
    CHECK(backend.readCalls == 2);
    CheckClean(backend);
    for (const auto stage :
         {ScreenRegionStage::DpiAwareness, ScreenRegionStage::MonitorEnumeration, ScreenRegionStage::Revalidation, ScreenRegionStage::Cleanup})
    {
        FakeBackend failing;
        failing.failureStage = stage;
        output = Sentinel();
        failing.watchedOutput = &output;
        CHECK(RunResolve(failing, RECT{-1, -1, 0, 0}, output) == ScreenRegionStatus::Failure(ScreenRegionErrorCode::NativeFailure, stage, 5678));
        CHECK(IsSentinel(output));
        CheckClean(failing);
    }
    FakeBackend invalid;
    output = Sentinel();
    CHECK(RunResolve(invalid, RECT{}, output).code == ScreenRegionErrorCode::InvalidRectangle);
    CHECK(invalid.readCalls == 0);
    CHECK(invalid.openCalls == 0);
    CHECK(IsSentinel(output));
}

TEST_CASE("Malformed initial topology cannot create overlay windows", "[screen-region][failure]")
{
    for (int mutation = 0; mutation < 5; mutation++)
    {
        FakeBackend backend;
        switch (mutation)
        {
        case 0:
            backend.initialTopology.count = 65;
            break;
        case 1:
            backend.initialTopology.count = 0;
            break;
        case 2:
            backend.initialTopology.monitors[0].dpiY = 0;
            break;
        case 3:
            backend.initialTopology.monitors[0].rotation = DXGI_MODE_ROTATION_UNSPECIFIED;
            break;
        case 4:
            backend.initialTopology.monitors[0].physicalRect.right = backend.initialTopology.monitors[0].physicalRect.left;
            break;
        }
        auto output = Sentinel();
        CHECK_FALSE(RunSelection(backend, output));
        CHECK(backend.openCalls == 0);
        CHECK(IsSentinel(output));
    }
}

TEST_CASE("Fixed-seed event noise cannot fabricate a drag or leak capture across sessions", "[screen-region][state-stream]")
{
    std::mt19937_64 random(0x5042535253544154ull);
    for (unsigned int run = 0; run < 512; run++)
    {
        FakeBackend backend;
        backend.events.clear();
        const auto count = static_cast<std::size_t>(random() % 50) + 1;
        for (std::size_t index = 0; index < count; index++)
        {
            const auto type = (random() & 1) == 0 ? SelectionEventType::PointerMove : SelectionEventType::PointerUp;
            backend.events.push_back({type, {40000, -40000}});
        }
        backend.events.push_back({SelectionEventType::PointerDown, {-100, -100}});
        backend.events.push_back({SelectionEventType::PointerDown, {-500, -500}}); // duplicate down must not replace the anchor
        backend.events.push_back({SelectionEventType::PointerUp, {-81, -71}});
        auto output = Sentinel();
        backend.watchedOutput = &output;
        REQUIRE(RunSelection(backend, output));
        CHECK(EqualRect(output.physicalRect, RECT{-100, -100, -80, -70}));
        CHECK(backend.acquireCalls == 1);
        CheckClean(backend);
    }
}
