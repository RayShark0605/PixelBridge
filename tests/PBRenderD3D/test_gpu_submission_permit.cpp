#include "gpu_submission_permit.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("GPU submission permits require completion and cannot be duplicated or survive reset", "[presentation][composed-blt]")
{
    pbrenderd3d::GpuSubmissionPermit permit;
    CHECK_FALSE(permit.Submit());
    CHECK_FALSE(permit.NeedsCompletion());
    REQUIRE(permit.TryAcquire(false));
    CHECK_FALSE(permit.TryAcquire(true));
    REQUIRE(permit.Submit());
    CHECK_FALSE(permit.Submit());
    REQUIRE(permit.NeedsCompletion());
    for (int index = 0; index < 100; index++)
    {
        CHECK_FALSE(permit.TryAcquire(false));
    }
    REQUIRE(permit.TryAcquire(true));
    CHECK_FALSE(permit.NeedsCompletion());
    CHECK_FALSE(permit.TryAcquire(true));
    permit.Reset();
    CHECK_FALSE(permit.Submit());
    REQUIRE(permit.TryAcquire(false));
    REQUIRE(permit.Submit());
    permit.Reset();
    CHECK_FALSE(permit.NeedsCompletion());
    REQUIRE(permit.TryAcquire(false));
}
