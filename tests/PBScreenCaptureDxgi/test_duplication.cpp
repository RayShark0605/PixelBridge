#include "test_support.h"

#include <limits>
#include <type_traits>

using namespace dxgitest;

static_assert(!std::is_copy_constructible_v<DuplicationFrameSource>);
static_assert(!std::is_copy_assignable_v<DuplicationFrameSource>);
static_assert(!std::is_move_constructible_v<DuplicationFrameSource>);
static_assert(!std::is_move_assignable_v<DuplicationFrameSource>);

TEST_CASE("DXGI timeout and unsuccessful acquire never call ReleaseFrame")
{
    SourceFixture fixture;
    REQUIRE(fixture.source.Acquire());
    REQUIRE(fixture.control->acquireCalls == 1);
    REQUIRE(fixture.control->releases == 0);
    REQUIRE_FALSE(fixture.source.Outstanding());
    auto acquisition = Image();
    acquisition.acquireResult = E_FAIL;
    fixture.control->Push(acquisition);
    REQUIRE(fixture.source.Acquire() == CaptureStatus::Failure(CaptureError::NativeFailure, CaptureStage::Callback, E_FAIL));
    REQUIRE(fixture.control->releases == 0);
    REQUIRE(fixture.inbox->counters->live == 0);
    CaptureSnapshot snapshot;
    fixture.source.UpdateSnapshot(snapshot);
    REQUIRE(snapshot.acquireTimeouts == 1);
    REQUIRE_FALSE(fixture.control->nonzeroTimeout);
}

TEST_CASE("DXGI single outstanding source lease is move only and released exactly once")
{
    SourceFixture fixture;
    fixture.control->Push(Image(100, 100, true));
    fixture.control->Push(Image(200, 200, false));
    REQUIRE(fixture.source.Acquire());
    REQUIRE(fixture.source.Outstanding());
    REQUIRE(fixture.source.Acquire());
    REQUIRE(fixture.control->acquireCalls == 1);
    FrameLease first;
    REQUIRE(fixture.inbox->TakeNewest(first));
    REQUIRE(first.timestamp100ns == 1000000);
    REQUIRE(first.rawTimestamp == 100);
    REQUIRE(first.rawFrequency == 1000);
    REQUIRE(first.timestampDomain == CaptureTimestampDomain::DxgiQpcTicks);
    REQUIRE(first.cursorState == CursorState::SeparatePointer);
    REQUIRE(first.pointer.positionKnown);
    REQUIRE(first.pointer.physicalLeft == -93);
    REQUIRE(first.pointer.physicalTop == 31);
    REQUIRE_FALSE(first.pointer.shapeKnown);
    FrameLease moved(std::move(first));
    REQUIRE_FALSE(first);
    REQUIRE(moved.pointer.rawUpdateTimestamp == 100);
    REQUIRE_FALSE(fixture.source.Reset());
    REQUIRE(fixture.control->releases == 0);
    moved.Reset();
    moved.Reset();
    REQUIRE(fixture.control->releases == 1);
    REQUIRE(fixture.source.Acquire());
    FrameLease second;
    REQUIRE(fixture.inbox->TakeNewest(second));
    REQUIRE(second.cursorState == CursorState::KnownAbsent);
    second.Reset();
    REQUIRE(fixture.control->releases == 2);
    REQUIRE_FALSE(fixture.control->unsafeAcquire);
    REQUIRE(fixture.inbox->counters->highWater == 1);
    REQUIRE(fixture.inbox->counters->live == 0);
}

TEST_CASE("DXGI pointer-only updates never become image arrivals and shape metadata is epoch-local")
{
    SourceFixture fixture;
    auto pointerOnly = Image(0, 50, true);
    pointerOnly.info.AccumulatedFrames = 0;
    pointerOnly.info.PointerShapeBufferSize = 64;
    fixture.control->Push(pointerOnly);
    REQUIRE(fixture.source.Acquire());
    REQUIRE(fixture.control->releases == 1);
    REQUIRE(fixture.inbox->GetSnapshot().arrivedFrames == 0);
    fixture.control->Push(Image(100));
    REQUIRE(fixture.source.Acquire());
    FrameLease frame;
    REQUIRE(fixture.inbox->TakeNewest(frame));
    REQUIRE(frame.cursorState == CursorState::SeparatePointer);
    REQUIRE(frame.pointer.shapeKnown);
    REQUIRE(frame.pointer.shapeType == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR);
    REQUIRE(frame.pointer.shapeWidth == 4);
    REQUIRE(frame.pointer.shapeVisibleHeight == 4);
    REQUIRE(frame.pointer.shapeBytes == 64);
    REQUIRE(frame.pointer.hotspotX == 1);
    REQUIRE(frame.pointer.hotspotY == 2);
    REQUIRE(frame.pointer.physicalLeft == -93);
    REQUIRE(frame.pointer.physicalTop == 31);
    frame.Reset();
    REQUIRE(fixture.control->shapes == 1);
    REQUIRE(fixture.source.Reset());
    REQUIRE(fixture.source.Initialize(std::make_unique<ScriptedDuplication>(fixture.control), fixture.inbox, MakeEnvironment(), 1000));
    fixture.inbox->Resume({100, 80}, 8);
    fixture.source.Start(8);
    fixture.control->Push(Image(200));
    REQUIRE(fixture.source.Acquire());
    REQUIRE(fixture.inbox->TakeNewest(frame));
    REQUIRE(frame.captureEpoch == 8);
    REQUIRE(frame.cursorState == CursorState::Unknown);
    REQUIRE_FALSE(frame.pointer.positionKnown);
    REQUIRE_FALSE(frame.pointer.shapeKnown);
    frame.Reset();
    CaptureSnapshot snapshot;
    fixture.source.UpdateSnapshot(snapshot);
    REQUIRE(snapshot.pointerOnlyFrames == 1);
    REQUIRE(snapshot.accumulatedFrames == 2);
}

TEST_CASE("DXGI absent pointer reports stay unknown and false visibility proves the frame cursor-free")
{
    for (const auto acquisition : {Image(100), Image(100, 100, false)})
    {
        SourceFixture fixture;
        fixture.control->Push(acquisition);
        REQUIRE(fixture.source.Acquire());
        FrameLease frame;
        REQUIRE(fixture.inbox->TakeNewest(frame));
        // A zero LastMouseUpdateTime is an unspecified report and stays
        // Unknown. A well-formed report with Visible==FALSE is the compositor
        // stating the pointer is not in the frame, so it is KnownAbsent and
        // downstream keeps it; neither ever claims backend-level exclusion or
        // a composited pointer box.
        const bool wellFormed = acquisition.info.LastMouseUpdateTime.QuadPart != 0;
        CHECK(frame.cursorState == (wellFormed ? CursorState::KnownAbsent : CursorState::Unknown));
        REQUIRE(frame.cursorState != CursorState::Excluded);
        REQUIRE(frame.cursorState != CursorState::SeparatePointer);
        REQUIRE_FALSE(frame.pointer.positionKnown);
        REQUIRE_FALSE(frame.pointer.separateVisible);
        frame.Reset();
    }
}

TEST_CASE("DXGI invisible pointer discards unspecified coordinates until a later visible update")
{
    SourceFixture fixture;
    auto first = Image(100, 100, true);
    first.info.PointerShapeBufferSize = 64;
    auto invisible = Image(200, 200, false);
    invisible.info.PointerPosition.Position = {std::numeric_limits<LONG>::min(), std::numeric_limits<LONG>::max()};
    auto unchanged = Image(300);
    unchanged.info.PointerPosition.Visible = TRUE;
    unchanged.info.PointerPosition.Position = {123, 456};
    auto restored = Image(400, 400, true);
    restored.info.PointerPosition.Position = {20, 30};
    for (const auto& acquisition : {first, invisible, unchanged, restored})
    {
        fixture.control->Push(acquisition);
        REQUIRE(fixture.source.Acquire());
        FrameLease frame;
        REQUIRE(fixture.inbox->TakeNewest(frame));
        REQUIRE(frame.pointer.shapeKnown);
        REQUIRE(frame.pointer.shapeWidth == 4);
        if (acquisition.info.LastPresentTime.QuadPart == 200 || acquisition.info.LastPresentTime.QuadPart == 300)
        {
            REQUIRE_FALSE(frame.pointer.positionKnown);
            REQUIRE_FALSE(frame.pointer.separateVisible);
            REQUIRE(frame.pointer.physicalLeft == 0);
            REQUIRE(frame.pointer.physicalTop == 0);
            REQUIRE(frame.pointer.rawUpdateTimestamp == 200);
            REQUIRE(frame.cursorState == CursorState::KnownAbsent);
        }
        else
        {
            REQUIRE(frame.pointer.positionKnown);
            REQUIRE(frame.pointer.separateVisible);
            REQUIRE(frame.cursorState == CursorState::SeparatePointer);
        }
        if (acquisition.info.LastPresentTime.QuadPart == 400)
        {
            REQUIRE(frame.pointer.physicalLeft == -80);
            REQUIRE(frame.pointer.physicalTop == 50);
            REQUIRE(frame.pointer.rawUpdateTimestamp == 400);
        }
        frame.Reset();
    }
    REQUIRE(fixture.control->releases == 4);
    REQUIRE(fixture.control->acquired == 4);
    REQUIRE(fixture.control->shapes == 1);
}

TEST_CASE("DXGI acquire metadata and release ACCESS_LOST each request nonfatal recreation")
{
    for (int failurePoint = 0; failurePoint < 3; failurePoint++)
    {
        SourceFixture fixture;
        auto acquisition = Image(100, 100, true);
        if (failurePoint == 0)
        {
            acquisition.acquireResult = DXGI_ERROR_ACCESS_LOST;
        }
        else if (failurePoint == 1)
        {
            acquisition.info.PointerShapeBufferSize = 64;
            acquisition.shapeResult = DXGI_ERROR_ACCESS_LOST;
        }
        else
        {
            acquisition.releaseResult = DXGI_ERROR_ACCESS_LOST;
        }
        fixture.control->Push(acquisition);
        REQUIRE(fixture.source.Acquire());
        if (failurePoint == 2)
        {
            FrameLease frame;
            REQUIRE(fixture.inbox->TakeNewest(frame));
            frame.Reset();
        }
        REQUIRE(fixture.source.AccessLost());
        REQUIRE(fixture.inbox->GetSnapshot().recreateRequested);
        REQUIRE(fixture.inbox->GetSnapshot().error);
        REQUIRE(fixture.inbox->counters->closeError == S_OK);
        REQUIRE(fixture.control->releases == (failurePoint == 0 ? 0u : 1u));
        REQUIRE(fixture.inbox->counters->live == 0);
        CaptureSnapshot snapshot;
        fixture.source.UpdateSnapshot(snapshot);
        REQUIRE(snapshot.accessLostEvents == 1);
        REQUIRE(fixture.source.Acquire());
        REQUIRE(fixture.control->acquireCalls == 1);
    }
}

TEST_CASE("DXGI malformed metadata returns every successful acquisition without allocation or admission")
{
    for (int corruption = 0; corruption < 9; corruption++)
    {
        SourceFixture fixture;
        auto acquisition = Image(100, 100, true);
        CaptureError expected = CaptureError::InvalidFrame;
        switch (corruption)
        {
        case 0: acquisition.info.LastPresentTime.QuadPart = -1; break;
        case 1: acquisition.info.LastMouseUpdateTime.QuadPart = -1; break;
        case 2: acquisition.info.PointerShapeBufferSize = maximumPointerShapeBytes + 1; expected = CaptureError::ResourceLimit; break;
        case 3: acquisition.info.PointerShapeBufferSize = 64; acquisition.shapeBytes = 63; break;
        case 4: acquisition.info.PointerShapeBufferSize = 64; acquisition.shape.Type = 0; break;
        case 5: acquisition.resourcePresent = false; break;
        case 6: acquisition.info.AccumulatedFrames = 0; break;
        case 7: acquisition.info.PointerShapeBufferSize = 64; acquisition.shapeResult = DXGI_ERROR_MORE_DATA; acquisition.shapeBytes = maximumPointerShapeBytes + 1; expected = CaptureError::ResourceLimit; break;
        case 8: acquisition.info.LastPresentTime.QuadPart = 0; acquisition.info.TotalMetadataBufferSize = 1; break;
        }
        fixture.control->Push(acquisition);
        const auto status = fixture.source.Acquire();
        REQUIRE(status.code == expected);
        REQUIRE(status.stage == CaptureStage::Callback);
        REQUIRE(fixture.control->releases == 1);
        REQUIRE(fixture.inbox->GetSnapshot().arrivedFrames == 0);
        REQUIRE(fixture.inbox->counters->live == 0);
        REQUIRE_FALSE(fixture.source.Outstanding());
        REQUIRE(fixture.control->shapes <= 1);
    }
}

TEST_CASE("DXGI failed texture query and non-access release failure preserve once-only cleanup")
{
    SourceFixture fixture;
    auto acquisition = Image();
    acquisition.releaseResult = E_FAIL;
    fixture.control->Push(acquisition);
    REQUIRE(fixture.source.Acquire());
    FrameLease frame;
    REQUIRE(fixture.inbox->TakeNewest(frame));
    ComPtr<ID3D11Texture2D> texture;
    REQUIRE(frame.GetTexture(&texture) == E_NOINTERFACE);
    frame.Reset();
    REQUIRE(frame.GetTexture(&texture) == E_UNEXPECTED);
    REQUIRE(fixture.control->releases == 1);
    REQUIRE(fixture.inbox->counters->closeError == E_FAIL);
    REQUIRE_FALSE(fixture.inbox->GetSnapshot().recreateRequested);
}
