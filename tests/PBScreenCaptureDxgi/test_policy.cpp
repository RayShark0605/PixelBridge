#include "test_support.h"

#include <limits>

using namespace dxgitest;

TEST_CASE("DXGI declares all supported scanout formats without silently narrowing high color")
{
    constexpr UINT supported = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_LOAD | D3D11_FORMAT_SUPPORT_DISPLAY;
    for (const auto preferred : {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT})
    {
        DuplicationFormatPlan plan;
        REQUIRE(BuildDuplicationFormatPlan(preferred, false, plan));
        REQUIRE(plan.formatCount == 3);
        REQUIRE(plan.formats[0] == preferred);
        for (const auto format : {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT})
        {
            REQUIRE(std::count(plan.formats.begin(), plan.formats.end(), format) == 1);
        }
        DuplicationFormatPlan filtered;
        REQUIRE(FilterDuplicationFormatPlan(plan, {supported, supported, supported}, filtered));
        REQUIRE(filtered == plan);
        REQUIRE_FALSE(MayUseLegacyDuplication(plan, DXGI_ERROR_ACCESS_LOST));
        REQUIRE_FALSE(MayUseLegacyDuplication(plan, E_ACCESSDENIED));
        REQUIRE_FALSE(MayUseLegacyDuplication(plan, DXGI_ERROR_UNSUPPORTED));
        REQUIRE(MayUseLegacyDuplication(plan, E_NOINTERFACE) == (preferred == DXGI_FORMAT_B8G8R8A8_UNORM));
        REQUIRE(BuildDuplicationFormatPlan(preferred, true, plan));
        REQUIRE_FALSE(MayUseLegacyDuplication(plan, E_NOTIMPL));
    }
    DuplicationFormatPlan plan;
    REQUIRE(BuildDuplicationFormatPlan(DXGI_FORMAT_B8G8R8A8_UNORM, false, plan));
    auto filtered = plan;
    REQUIRE(FilterDuplicationFormatPlan(plan, {supported, D3D11_FORMAT_SUPPORT_TEXTURE2D, supported}, filtered));
    REQUIRE(filtered.formatCount == 2);
    REQUIRE(filtered.formats[0] == DXGI_FORMAT_B8G8R8A8_UNORM);
    REQUIRE(filtered.formats[1] == DXGI_FORMAT_R16G16B16A16_FLOAT);
    filtered = plan;
    REQUIRE_FALSE(FilterDuplicationFormatPlan(plan, {0, supported, supported}, filtered));
    REQUIRE(filtered == plan);
    REQUIRE_FALSE(BuildDuplicationFormatPlan(DXGI_FORMAT_R8_UNORM, false, filtered));
    REQUIRE(filtered == plan);
}

TEST_CASE("DXGI confirmed environment change alone takes priority over the same stale output query failure")
{
    constexpr HRESULT queryResult = E_FAIL;
    constexpr bool waiting = false;
    REQUIRE(ClassifyOutputQueryResult(queryResult, false, waiting) == OutputQueryDisposition::Failure);
    REQUIRE(ClassifyOutputQueryResult(queryResult, true, waiting) == OutputQueryDisposition::Rebuild);
    REQUIRE(FromHresult(queryResult, CaptureStage::Region) == CaptureStatus::Failure(CaptureError::NativeFailure, CaptureStage::Region, E_FAIL));

    EnvironmentRetryBudget budget;
    const EnvironmentRetryBudget::Clock::time_point start{};
    for (std::uint32_t attempt = 0; attempt < maximumEnvironmentAttempts; attempt++)
    {
        budget.RecordAttempt(start + std::chrono::seconds(attempt));
    }
    REQUIRE_FALSE(budget.CanRetry(start + std::chrono::hours(24)));
    REQUIRE(ClassifyOutputQueryResult(queryResult, false, false) == OutputQueryDisposition::Failure);
    REQUIRE(budget.Attempts() == maximumEnvironmentAttempts);

    // The same production classification opens only the existing finite retry
    // series; it does not reclassify later unconfirmed E_FAILs as new changes.
    REQUIRE(ClassifyOutputQueryResult(queryResult, true, false) == OutputQueryDisposition::Rebuild);
    budget.Reset();
    for (std::uint32_t attempt = 0; attempt < maximumEnvironmentAttempts; attempt++)
    {
        const auto now = start + std::chrono::hours(24) + std::chrono::seconds(attempt);
        REQUIRE(budget.CanRetry(now));
        budget.RecordAttempt(now);
        REQUIRE(ClassifyOutputQueryResult(queryResult, false, true) == OutputQueryDisposition::Unchanged);
    }
    REQUIRE(budget.Attempts() == maximumEnvironmentAttempts);
    REQUIRE_FALSE(budget.CanRetry(start + std::chrono::hours(48)));
}

TEST_CASE("DXGI output query policy preserves success waiting and unconfirmed error classifications")
{
    for (const bool waiting : {false, true})
    {
        REQUIRE(ClassifyOutputQueryResult(S_OK, false, waiting) == OutputQueryDisposition::Unchanged);
        REQUIRE(ClassifyOutputQueryResult(S_OK, true, waiting) == OutputQueryDisposition::Rebuild);
        for (const HRESULT result : {E_FAIL, E_OUTOFMEMORY, DXGI_ERROR_DEVICE_REMOVED})
        {
            REQUIRE(ClassifyOutputQueryResult(result, false, waiting) == (waiting ? OutputQueryDisposition::Unchanged : OutputQueryDisposition::Failure));
            REQUIRE(ClassifyOutputQueryResult(result, true, waiting) == OutputQueryDisposition::Rebuild);
        }
        for (const HRESULT result : {DXGI_ERROR_ACCESS_LOST, DXGI_ERROR_UNSUPPORTED, DXGI_ERROR_NOT_CURRENTLY_AVAILABLE, DXGI_ERROR_SESSION_DISCONNECTED, E_ACCESSDENIED})
        {
            REQUIRE(ClassifyOutputQueryResult(result, false, waiting) == (waiting ? OutputQueryDisposition::Unchanged : OutputQueryDisposition::Rebuild));
            REQUIRE(ClassifyOutputQueryResult(result, true, waiting) == OutputQueryDisposition::Rebuild);
        }
    }
}

TEST_CASE("DXGI actual format rotation and byte quotas are verified before allocation")
{
    for (const auto rotation : {DXGI_MODE_ROTATION_IDENTITY, DXGI_MODE_ROTATION_ROTATE90, DXGI_MODE_ROTATION_ROTATE180, DXGI_MODE_ROTATION_ROTATE270})
    {
        auto config = MakeConfig();
        auto environment = MakeEnvironment();
        config.region.rotation = rotation;
        environment.region.rotation = rotation;
        DXGI_OUTDUPL_DESC description{};
        description.Rotation = rotation;
        description.ModeDesc = {100, 80, {60, 1}, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED, DXGI_MODE_SCALING_UNSPECIFIED};
        REQUIRE(ResolveDuplicationEnvironment(config, description, true, environment));
        REQUIRE(environment.pixelFormat == DXGI_FORMAT_R10G10B10A2_UNORM);
        REQUIRE(environment.contentSize == CaptureSize{100, 80});
        const bool swapsAxes = rotation == DXGI_MODE_ROTATION_ROTATE90 || rotation == DXGI_MODE_ROTATION_ROTATE270;
        REQUIRE(environment.sourceSize == (swapsAxes ? CaptureSize{80, 100} : CaptureSize{100, 80}));
        REQUIRE(environment.sourceRotation == rotation);
        description.ModeDesc.Width = 101;
        REQUIRE_FALSE(ResolveDuplicationEnvironment(config, description, true, environment));
        REQUIRE(environment.pixelFormat == DXGI_FORMAT_R10G10B10A2_UNORM);
    }
    auto config = MakeConfig();
    auto environment = MakeEnvironment();
    DXGI_OUTDUPL_DESC description{};
    description.Rotation = DXGI_MODE_ROTATION_IDENTITY;
    description.ModeDesc.Width = 100;
    description.ModeDesc.Height = 80;
    description.ModeDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    REQUIRE(ResolveDuplicationEnvironment(config, description, false, environment));
    environment.bitsPerColor = 10;
    REQUIRE_FALSE(ResolveDuplicationEnvironment(config, description, false, environment));
    environment.hdr = true;
    REQUIRE_FALSE(ResolveDuplicationEnvironment(config, description, true, environment));
    description.ModeDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    REQUIRE(ResolveDuplicationEnvironment(config, description, true, environment));
    constexpr std::uint64_t exactBytes = 100 * 80 * 8 + 30 * 20 * 8 * 3 + maximumPointerShapeBytes;
    config.maximumCaptureBytes = exactBytes - 1;
    REQUIRE(ResolveDuplicationEnvironment(config, description, true, environment).code == CaptureError::ResourceLimit);
    config.maximumCaptureBytes++;
    REQUIRE(ResolveDuplicationEnvironment(config, description, true, environment));
    config.pixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    config.maximumCaptureBytes = 100 * 80 * 4 + 30 * 20 * 4 * 3 + maximumPointerShapeBytes;
    REQUIRE(ValidateDxgiCaptureConfig(config).code == CaptureError::ResourceLimit);
    environment = MakeEnvironment();
    description.ModeDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    REQUIRE(ResolveDuplicationEnvironment(config, description, true, environment));
    config.maximumCaptureBytes = exactBytes;
    REQUIRE(ValidateDxgiCaptureConfig(config));
    config.maximumCaptureBytes--;
    REQUIRE(ValidateDxgiCaptureConfig(config).code == CaptureError::ResourceLimit);
}

TEST_CASE("DXGI public preflight charges every rotated slot's raw scratch storage", "[dxgi-preflight]")
{
    constexpr std::uint64_t outputRingBytes = 30 * 20 * 8 * 3;
    constexpr std::uint64_t rotatedRingBytes = outputRingBytes * 2;
    constexpr std::uint64_t exactTotalBytes = 100 * 80 * 8 + rotatedRingBytes + maximumPointerShapeBytes;
    for (const auto rotation : {DXGI_MODE_ROTATION_ROTATE90, DXGI_MODE_ROTATION_ROTATE180, DXGI_MODE_ROTATION_ROTATE270})
    {
        INFO("rotation=" << static_cast<unsigned>(rotation));
        auto config = MakeConfig();
        // FP16 makes the missing scratch charge independent of the negotiated
        // bytes-per-pixel issue: even this explicitly requested format needs it.
        config.pixelFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
        config.region.rotation = rotation;
        config.maximumCaptureBytes = exactTotalBytes;
        config.maximumRoiBytes = outputRingBytes;
        CHECK(ValidateDxgiCaptureConfig(config) == CaptureStatus::Failure(CaptureError::ResourceLimit, CaptureStage::Configuration));
        config.maximumRoiBytes = rotatedRingBytes - 1;
        CHECK(ValidateDxgiCaptureConfig(config) == CaptureStatus::Failure(CaptureError::ResourceLimit, CaptureStage::Configuration));
        config.maximumRoiBytes = rotatedRingBytes;
        CHECK(ValidateDxgiCaptureConfig(config));
        config.maximumCaptureBytes--;
        CHECK(ValidateDxgiCaptureConfig(config) == CaptureStatus::Failure(CaptureError::ResourceLimit, CaptureStage::Configuration));
    }
}

TEST_CASE("DXGI public preflight reserves the largest advertised format without narrowing the plan", "[dxgi-preflight]")
{
    for (const auto preferred : {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT})
    {
        for (const auto rotation : {DXGI_MODE_ROTATION_IDENTITY, DXGI_MODE_ROTATION_ROTATE90, DXGI_MODE_ROTATION_ROTATE180, DXGI_MODE_ROTATION_ROTATE270})
        {
            INFO("preferred=" << static_cast<unsigned>(preferred) << ", rotation=" << static_cast<unsigned>(rotation));
            auto config = MakeConfig();
            config.pixelFormat = preferred;
            config.region.rotation = rotation;
            const std::uint64_t scratchFactor = rotation == DXGI_MODE_ROTATION_IDENTITY ? 1 : 2;
            const std::uint64_t ringBytes = 30 * 20 * 8 * 3 * scratchFactor;
            const std::uint64_t totalBytes = 100 * 80 * 8 + ringBytes + maximumPointerShapeBytes;
            config.maximumRoiBytes = ringBytes;
            config.maximumCaptureBytes = totalBytes;
            CHECK(ValidateDxgiCaptureConfig(config));
            config.maximumCaptureBytes = totalBytes - 1;
            CHECK(ValidateDxgiCaptureConfig(config) == CaptureStatus::Failure(CaptureError::ResourceLimit, CaptureStage::Configuration));
            config.maximumCaptureBytes = totalBytes;
            config.maximumRoiBytes = ringBytes - 1;
            CHECK(ValidateDxgiCaptureConfig(config) == CaptureStatus::Failure(CaptureError::ResourceLimit, CaptureStage::Configuration));
            config.maximumRoiBytes = ringBytes;
            config.maximumCaptureBytes = 100 * 80 * 4 + 30 * 20 * 4 * 3 * scratchFactor + maximumPointerShapeBytes;
            CHECK(ValidateDxgiCaptureConfig(config) == CaptureStatus::Failure(CaptureError::ResourceLimit, CaptureStage::Configuration));

            DuplicationFormatPlan plan;
            REQUIRE(BuildDuplicationFormatPlan(preferred, false, plan));
            REQUIRE(plan.formatCount == 3);
            REQUIRE(std::count(plan.formats.begin(), plan.formats.end(), DXGI_FORMAT_R16G16B16A16_FLOAT) == 1);
        }
    }
}

TEST_CASE("DXGI native preflight rechecks current same-monitor geometry instead of the initial selection snapshot", "[dxgi-preflight]")
{
    auto config = MakeConfig();
    constexpr std::uint64_t outputRingBytes = 30 * 20 * 8 * 3;
    config.maximumRoiBytes = outputRingBytes;
    config.maximumCaptureBytes = 100 * 80 * 8 + outputRingBytes + maximumPointerShapeBytes;
    REQUIRE(ValidateDxgiCaptureConfig(config));
    auto current = config.region;
    SECTION("rotation changes while the physical ROI still fits")
    {
        current.rotation = DXGI_MODE_ROTATION_ROTATE90;
        config.maximumCaptureBytes += outputRingBytes;
        REQUIRE(ValidateDxgiCaptureConfig(config));
        REQUIRE(ValidateDuplicationPreflight(config, current) == CaptureStatus::Failure(CaptureError::ResourceLimit, CaptureStage::Configuration));
        config.maximumRoiBytes = outputRingBytes * 2;
        REQUIRE(ValidateDuplicationPreflight(config, current));
        config.maximumRoiBytes--;
        REQUIRE(ValidateDuplicationPreflight(config, current) == CaptureStatus::Failure(CaptureError::ResourceLimit, CaptureStage::Configuration));
    }
    SECTION("mode dimensions grow without moving the selected monitor or physical ROI")
    {
        current.monitorPhysicalRect.right = 100;
        current.monitorPhysicalRect.bottom = 120;
        REQUIRE(ValidateDxgiCaptureConfig(config));
        REQUIRE(ValidateDuplicationPreflight(config, current) == CaptureStatus::Failure(CaptureError::ResourceLimit, CaptureStage::Configuration));
        config.maximumCaptureBytes = 200 * 100 * 8 + outputRingBytes + maximumPointerShapeBytes;
        REQUIRE(ValidateDuplicationPreflight(config, current));
        config.maximumCaptureBytes--;
        REQUIRE(ValidateDuplicationPreflight(config, current) == CaptureStatus::Failure(CaptureError::ResourceLimit, CaptureStage::Configuration));
    }
    REQUIRE(config.region.rotation == DXGI_MODE_ROTATION_IDENTITY);
    REQUIRE(config.region.monitorPhysicalRect.right == 0);
    REQUIRE(config.region.monitorPhysicalRect.bottom == 100);
}

TEST_CASE("DXGI QPC error mapping preserves timestamps and rejects invalid or overflowing domains")
{
    std::int64_t output = 23;
    REQUIRE(QpcTo100ns(1, 3, output));
    REQUIRE(output == 3333333);
    REQUIRE(QpcTo100ns(std::numeric_limits<std::int64_t>::max(), 10000000, output));
    REQUIRE(output == std::numeric_limits<std::int64_t>::max());
    for (const auto input : {std::pair{-1ll, 1000ll}, {1ll, 0ll}, {1ll, -1ll}, {std::numeric_limits<std::int64_t>::max(), 1ll}})
    {
        output = 23;
        REQUIRE(QpcTo100ns(input.first, input.second, output) == CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Callback));
        REQUIRE(output == 23);
    }
}

TEST_CASE("DXGI pointer shape validates color monochrome masked pitch hotspot and finite bounds")
{
    for (const UINT type : {DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR, DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR})
    {
        const DXGI_OUTDUPL_POINTER_SHAPE_INFO shape{type, 4, 4, 16, {3, 3}};
        REQUIRE(ValidatePointerShape(shape, 64));
        REQUIRE(ValidatePointerShape(shape, 63).code == CaptureError::InvalidFrame);
    }
    auto shape = DXGI_OUTDUPL_POINTER_SHAPE_INFO{DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME, 16, 32, 2, {15, 15}};
    REQUIRE(ValidatePointerShape(shape, 64));
    shape.HotSpot.y = 16;
    REQUIRE(ValidatePointerShape(shape, 64).code == CaptureError::InvalidFrame);
    shape.HotSpot = {0, 0};
    shape.Height = 31;
    REQUIRE_FALSE(ValidatePointerShape(shape, 64));
    shape.Height = 32;
    shape.Pitch = 1;
    REQUIRE_FALSE(ValidatePointerShape(shape, 64));
    shape.Pitch = 2;
    shape.Width = 0;
    REQUIRE_FALSE(ValidatePointerShape(shape, 64));
    shape.Width = std::numeric_limits<UINT>::max();
    REQUIRE(ValidatePointerShape(shape, 64).code == CaptureError::ResourceLimit);
    shape.Width = 16;
    shape.HotSpot.x = -1;
    REQUIRE_FALSE(ValidatePointerShape(shape, 64));
    REQUIRE(ValidatePointerShape(shape, maximumPointerShapeBytes + 1).code == CaptureError::ResourceLimit);
}

TEST_CASE("DXGI unavailable environment retries are finite and reset only on a new recovery opportunity")
{
    EnvironmentRetryBudget budget;
    const EnvironmentRetryBudget::Clock::time_point start{};
    REQUIRE(budget.CanRetry(start));
    budget.RecordAttempt(start);
    REQUIRE(budget.Attempts() == 1);
    REQUIRE_FALSE(budget.CanRetry(start + std::chrono::milliseconds(249)));
    REQUIRE(budget.CanRetry(start + std::chrono::milliseconds(250)));
    budget.RecordAttempt(start + std::chrono::milliseconds(250));
    REQUIRE_FALSE(budget.CanRetry(start + std::chrono::milliseconds(1249)));
    REQUIRE(budget.CanRetry(start + std::chrono::milliseconds(1250)));
    budget.RecordAttempt(start + std::chrono::milliseconds(1250));
    REQUIRE(budget.Attempts() == 3);
    REQUIRE_FALSE(budget.CanRetry(start + std::chrono::hours(24)));
    budget.RecordAttempt(start + std::chrono::hours(24));
    REQUIRE(budget.Attempts() == 3);
    budget.Reset();
    REQUIRE(budget.CanRetry(start));
    for (const HRESULT result : {DXGI_ERROR_ACCESS_LOST, DXGI_ERROR_UNSUPPORTED, DXGI_ERROR_NOT_CURRENTLY_AVAILABLE, DXGI_ERROR_SESSION_DISCONNECTED, E_ACCESSDENIED})
    {
        REQUIRE(IsEnvironmentUnavailable(result));
    }
    REQUIRE_FALSE(IsEnvironmentUnavailable(E_OUTOFMEMORY));
    REQUIRE_FALSE(IsEnvironmentUnavailable(E_INVALIDARG));
    REQUIRE_FALSE(IsEnvironmentUnavailable(DXGI_ERROR_DEVICE_REMOVED));
}
