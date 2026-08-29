#include "normalize_consumer.h"
#include "capture_runtime.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>
#include <wrl/client.h>

using namespace pbcapturenormalize;
using namespace pbcapturenormalize::detail;
using Microsoft::WRL::ComPtr;

namespace
{

class RecordingConsumer final : public ScreenCaptureConsumer
{
public:
    std::uint64_t reservedBytes = 0;
    bool throwOnStart = false;
    bool throwOnSubmit = false;
    bool failSubmit = false;
    std::vector<ScreenCaptureDomain> started;
    std::vector<ScreenCaptureDomain> invalidated;
    std::vector<ScreenCaptureFrameMetadata> submitted;
    std::vector<ScreenCaptureFrameMetadata> completed;
    std::vector<bool> cancelled;
    std::vector<CaptureErasure> erased;
    bool cancelledHasContext = false;
    ID3D11Texture2D* borrowedTexture = nullptr;

    std::uint64_t ReservedBytes() const noexcept override { return reservedBytes; }
    CaptureStatus DomainStarted(const ScreenCaptureDomain& domain, const CaptureEnvironment&, ID3D11Device*) override
    {
        started.push_back(domain);
        if (throwOnStart)
        {
            throw std::runtime_error("injected start failure");
        }
        return {};
    }
    void DomainInvalidated(const ScreenCaptureDomain& domain) noexcept override
    {
        invalidated.push_back(domain);
    }
    CaptureStatus Submit(const ScreenCaptureFrame& frame, ID3D11DeviceContext*) override
    {
        submitted.push_back(frame.metadata);
        borrowedTexture = frame.texture;
        if (throwOnSubmit)
        {
            throw std::runtime_error("injected submit failure");
        }
        return failSubmit ? CaptureStatus::Failure(CaptureError::ConsumerFailure, CaptureStage::Consumer) : CaptureStatus{};
    }
    CaptureStatus Completed(const ScreenCaptureFrameMetadata& metadata, ID3D11DeviceContext* context, const bool isCancelled) override
    {
        completed.push_back(metadata);
        cancelled.push_back(isCancelled);
        cancelledHasContext = cancelledHasContext || (isCancelled && context != nullptr);
        return {};
    }
    void Erased(const CaptureErasure& erasure) noexcept override
    {
        erased.push_back(erasure);
    }
};

struct Fixture
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> texture;
    CaptureNormalizeConfig config;
    CaptureEnvironment environment;
    std::shared_ptr<RecordingConsumer> receiver = std::make_shared<RecordingConsumer>();
    std::shared_ptr<NormalizeConsumer> normalizer;

    Fixture(const CaptureBackendKind backend = CaptureBackendKind::Wgc, const DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM,
            const DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_IDENTITY)
    {
        D3D_FEATURE_LEVEL featureLevel{};
        REQUIRE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                          D3D11_SDK_VERSION, &device, &featureLevel, &context)));
        environment.region = {reinterpret_cast<HMONITOR>(std::uintptr_t{1}), {-45, -18, -38, -13}, {-50, -20, -18, 0}, 144, 192, rotation};
        environment.backendKind = backend;
        environment.contentSize = {32, 20};
        environment.sourceRotation = backend == CaptureBackendKind::Wgc ? DXGI_MODE_ROTATION_IDENTITY : rotation;
        const bool swapsAxes = environment.sourceRotation == DXGI_MODE_ROTATION_ROTATE90 || environment.sourceRotation == DXGI_MODE_ROTATION_ROTATE270;
        environment.sourceSize = swapsAxes ? CaptureSize{20, 32} : CaptureSize{32, 20};
        environment.pixelFormat = format;
        environment.bitsPerColor = format == DXGI_FORMAT_B8G8R8A8_UNORM ? 8u : 10u;
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC description{};
        REQUIRE(SUCCEEDED(device.As(&dxgiDevice)));
        REQUIRE(SUCCEEDED(dxgiDevice->GetAdapter(&adapter)));
        REQUIRE(SUCCEEDED(adapter->GetDesc(&description)));
        environment.adapterLuid = description.AdapterLuid;
        config.capture.region = environment.region;
        config.capture.pixelFormat = format;
        config.capture.initialCaptureEpoch = 7;
        config.capture.maximumFrameAgeMilliseconds = 1000;
        D3D11_TEXTURE2D_DESC textureDescription{};
        textureDescription.Width = 7;
        textureDescription.Height = 5;
        textureDescription.MipLevels = 1;
        textureDescription.ArraySize = 1;
        textureDescription.Format = format;
        textureDescription.SampleDesc.Count = 1;
        textureDescription.Usage = D3D11_USAGE_DEFAULT;
        REQUIRE(SUCCEEDED(device->CreateTexture2D(&textureDescription, nullptr, &texture)));
        REQUIRE(NormalizeConsumer::Create(config, backend, receiver, normalizer));
    }

    void Start()
    {
        REQUIRE(normalizer->EpochStarted(7, environment, device.Get()));
    }
    RawRoiFrameMetadata Frame(const std::uint64_t observation = 1, const std::uint64_t epoch = 7, const std::uint64_t sourceGeneration = 1)
    {
        LARGE_INTEGER counter{};
        LARGE_INTEGER frequency{};
        REQUIRE(QueryPerformanceCounter(&counter));
        REQUIRE(QueryPerformanceFrequency(&frequency));
        RawRoiFrameMetadata frame;
        frame.captureEpoch = epoch;
        frame.arrivalOrdinal = observation;
        frame.sourceGeneration = sourceGeneration;
        frame.slotGeneration = observation;
        frame.environment = environment;
        frame.capabilities.cursorExcluded = true;
        REQUIRE(ConvertQpcTo100ns(counter.QuadPart, frequency.QuadPart, frame.systemRelativeTime100ns));
        frame.timestampDomain = environment.backendKind == CaptureBackendKind::Wgc ? CaptureTimestampDomain::WgcSystemRelative100ns : CaptureTimestampDomain::DxgiQpcTicks;
        frame.rawTimestamp = environment.backendKind == CaptureBackendKind::Wgc ? frame.systemRelativeTime100ns : counter.QuadPart;
        frame.rawFrequency = environment.backendKind == CaptureBackendKind::Wgc ? 10000000 : frequency.QuadPart;
        frame.cursorState = environment.backendKind == CaptureBackendKind::Wgc ? CursorState::Excluded : CursorState::SeparatePointer;
        frame.pointer.positionKnown = true;
        frame.pointer.separateVisible = true;
        frame.pointer.physicalLeft = -43;
        frame.pointer.physicalTop = -17;
        return frame;
    }
};

} // namespace

TEST_CASE("Normalized contract preserves physical ROI, actual formats, rotation and timestamp provenance", "[normalize-contract]")
{
    constexpr std::array rotations{DXGI_MODE_ROTATION_IDENTITY, DXGI_MODE_ROTATION_ROTATE90, DXGI_MODE_ROTATION_ROTATE180, DXGI_MODE_ROTATION_ROTATE270};
    constexpr std::array formats{DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT};
    for (const auto backend : {CaptureBackendKind::Wgc, CaptureBackendKind::Dxgi})
    {
        for (const auto format : formats)
        {
            if (backend == CaptureBackendKind::Wgc && format == DXGI_FORMAT_R10G10B10A2_UNORM)
            {
                continue; // Not a WGC frame-pool format; tested as a rejected configuration below.
            }
            for (const auto rotation : rotations)
            {
                CAPTURE(backend, format, rotation);
                Fixture fixture(backend, format, rotation);
                fixture.Start();
                const auto raw = fixture.Frame();
                REQUIRE(fixture.normalizer->Submit(raw, fixture.texture.Get(), fixture.context.Get()));
                REQUIRE(fixture.receiver->submitted.size() == 1);
                const auto& frame = fixture.receiver->submitted.front();
                CHECK(frame.physicalRoi.left == -45);
                CHECK(frame.physicalRoi.top == -18);
                CHECK(frame.physicalRoi.right == -38);
                CHECK(frame.physicalRoi.bottom == -13);
                CHECK(frame.roiSize == CaptureSize{7, 5});
                CHECK(frame.sourceContentSize == CaptureSize{32, 20});
                CHECK(frame.sourceExtent == fixture.environment.sourceSize);
                CHECK(frame.displayRotation == rotation);
                CHECK(frame.sourceTransform == (backend == CaptureBackendKind::Wgc ? DXGI_MODE_ROTATION_IDENTITY : rotation));
                CHECK(frame.sourcePixelFormat == format);
                CHECK(frame.pixelFormat == format);
                CHECK(frame.adapterLuid.LowPart == fixture.environment.adapterLuid.LowPart);
                CHECK(frame.adapterLuid.HighPart == fixture.environment.adapterLuid.HighPart);
                CHECK(frame.timestamp == ScreenCaptureTimestamp{raw.timestampDomain, raw.rawTimestamp, raw.rawFrequency, raw.systemRelativeTime100ns, raw.arrivalQpc100ns});
                CHECK(frame.captureObservation == 1);
                CHECK(frame.sourceGeneration == 1);
                CHECK(frame.slotGeneration == 1);
                CHECK(frame.domain.captureEpoch == 7);
                CHECK(frame.isCursorExcluded);
                CHECK(frame.pointer == raw.pointer);
                CHECK(fixture.receiver->borrowedTexture == fixture.texture.Get());
                REQUIRE(fixture.normalizer->Completed(raw, fixture.context.Get(), false));
                REQUIRE(fixture.receiver->completed.size() == 1);
                CHECK_FALSE(fixture.receiver->cancelled.front());
                CHECK(fixture.normalizer->GetSnapshot().acceptedFrames == 1);
            }
        }
    }
}

TEST_CASE("Cursor proof failures are frame erasures, not fatal status or raw fallback", "[normalize-contract]")
{
    for (const auto backend : {CaptureBackendKind::Wgc, CaptureBackendKind::Dxgi})
    {
        Fixture fixture(backend);
        fixture.Start();
        std::uint64_t observation = 0;
        for (const auto cursor : {CursorState::Unknown, CursorState::PossiblyComposited})
        {
            observation++;
            auto raw = fixture.Frame(observation);
            raw.cursorState = cursor;
            REQUIRE(fixture.normalizer->Submit(raw, fixture.texture.Get(), fixture.context.Get()));
            REQUIRE(fixture.receiver->erased.size() == observation);
            CHECK(fixture.receiver->erased.back().reason == (cursor == CursorState::Unknown ? CaptureErasureReason::CursorUnknown : CaptureErasureReason::CursorPossiblyComposited));
            REQUIRE(fixture.normalizer->Completed(raw, fixture.context.Get(), false));
            CHECK(fixture.receiver->submitted.empty());
            CHECK(fixture.receiver->completed.empty());
        }
        observation++;
        auto unproven = fixture.Frame(observation);
        unproven.capabilities.cursorExcluded = false;
        unproven.pointer.separateVisible = false;
        REQUIRE(fixture.normalizer->Submit(unproven, fixture.texture.Get(), fixture.context.Get()));
        CHECK(fixture.receiver->erased.back().reason == CaptureErasureReason::CursorUnknown);
        observation++;
        const auto valid = fixture.Frame(observation);
        REQUIRE(fixture.normalizer->Submit(valid, fixture.texture.Get(), fixture.context.Get()));
        REQUIRE(fixture.receiver->submitted.size() == 1);
        REQUIRE(fixture.normalizer->Completed(valid, fixture.context.Get(), false));
        observation++;
        auto knownAbsent = fixture.Frame(observation);
        knownAbsent.cursorState = CursorState::KnownAbsent;
        knownAbsent.capabilities.cursorExcluded = false;
        knownAbsent.pointer.separateVisible = false;
        knownAbsent.pointer.positionKnown = false;
        // Per-frame absence proof needs no capability and survives both
        // backends in the outer loop.
        REQUIRE(fixture.normalizer->Submit(knownAbsent, fixture.texture.Get(), fixture.context.Get()));
        REQUIRE(fixture.receiver->submitted.size() == 2);
        CHECK(fixture.receiver->submitted.back().isCursorExcluded);
        REQUIRE(fixture.normalizer->Completed(knownAbsent, fixture.context.Get(), false));
        CHECK(fixture.normalizer->GetSnapshot().active);
        CHECK(fixture.normalizer->GetSnapshot().erasedFrames == 3);
    }
}

TEST_CASE("Normalizer rejects epoch, generation, time, environment and texture corruption independently", "[normalize-contract]")
{
    Fixture fixture;
    fixture.Start();
    for (std::uint64_t test = 0; test < 12; test++)
    {
        auto raw = fixture.Frame(test + 1);
        auto expected = CaptureErasureReason::InvalidMetadata;
        ID3D11Texture2D* texture = fixture.texture.Get();
        switch (test)
        {
        case 0: raw.captureEpoch--; expected = CaptureErasureReason::InactiveDomain; break;
        case 1: raw.environment.region.physicalRect.left--; break;
        case 2: raw.environment.sourceRotation = DXGI_MODE_ROTATION_ROTATE180; break;
        case 3: raw.environment.adapterLuid.HighPart ^= 1; break;
        case 4: raw.environment.outputColorSpace = 12; break;
        case 5: raw.slotIndex = std::numeric_limits<std::uint32_t>::max(); break;
        case 6: raw.rawFrequency = 0; expected = CaptureErasureReason::InvalidTimestamp; break;
        case 7: raw.rawTimestamp = std::numeric_limits<std::int64_t>::max(); expected = CaptureErasureReason::InvalidTimestamp; break;
        case 8: raw.rawTimestamp = -1; expected = CaptureErasureReason::InvalidTimestamp; break;
        case 9: raw.rawTimestamp = 1; raw.systemRelativeTime100ns = 1; expected = CaptureErasureReason::Expired; break;
        case 10: raw.sourceGeneration = 0; break;
        case 11: texture = nullptr; expected = CaptureErasureReason::InvalidOwnedTexture; break;
        default: FAIL("missing corruption case");
        }
        CAPTURE(test);
        REQUIRE(fixture.normalizer->Submit(raw, texture, fixture.context.Get()));
        REQUIRE(fixture.receiver->erased.size() == test + 1);
        CHECK(fixture.receiver->erased.back().reason == expected);
        CHECK(fixture.receiver->submitted.empty());
    }
    const auto good = fixture.Frame(13);
    REQUIRE(fixture.normalizer->Submit(good, fixture.texture.Get(), fixture.context.Get()));
    REQUIRE(fixture.receiver->submitted.size() == 1);
    REQUIRE(fixture.normalizer->Completed(good, fixture.context.Get(), false));
    REQUIRE(fixture.normalizer->Submit(good, fixture.texture.Get(), fixture.context.Get()));
    CHECK(fixture.receiver->erased.back().reason == CaptureErasureReason::StaleObservation);
    CHECK(fixture.receiver->submitted.size() == 1);
}

TEST_CASE("Ahead-of-time source timestamp falls back to the measured QPC arrival", "[normalize-contract]")
{
    for (const auto backend : {CaptureBackendKind::Wgc, CaptureBackendKind::Dxgi})
    {
        CAPTURE(backend);
        Fixture fixture(backend);
        fixture.Start();
        std::int64_t arrival100ns = 0;
        LARGE_INTEGER counter{};
        LARGE_INTEGER frequency{};
        REQUIRE(QueryPerformanceCounter(&counter));
        REQUIRE(QueryPerformanceFrequency(&frequency));
        REQUIRE(ConvertQpcTo100ns(counter.QuadPart, frequency.QuadPart, arrival100ns));
        // Six seconds ahead in the backend's own domain units: the claim alone
        // must stay invalid...
        auto ahead = fixture.Frame(1);
        const std::int64_t advance = 6 * ahead.rawFrequency;
        ahead.rawTimestamp += advance;
        ahead.systemRelativeTime100ns += advance;
        REQUIRE(fixture.normalizer->Submit(ahead, fixture.texture.Get(), fixture.context.Get()));
        REQUIRE(fixture.receiver->erased.size() == 1);
        CHECK(fixture.receiver->erased.back().reason == CaptureErasureReason::InvalidTimestamp);
        CHECK(fixture.receiver->submitted.empty());
        // ...but the measured inbox arrival supersedes the impossible claim.
        auto rescued = fixture.Frame(2);
        const std::int64_t rescuedAdvance = 6 * rescued.rawFrequency;
        rescued.rawTimestamp += rescuedAdvance;
        rescued.systemRelativeTime100ns += rescuedAdvance;
        rescued.arrivalQpc100ns = arrival100ns;
        REQUIRE(fixture.normalizer->Submit(rescued, fixture.texture.Get(), fixture.context.Get()));
        REQUIRE(fixture.receiver->submitted.size() == 1);
        CHECK(fixture.receiver->submitted.back().timestamp.arrivalQpc100ns == arrival100ns);
        REQUIRE(fixture.normalizer->Completed(rescued, fixture.context.Get(), false));
        CHECK(fixture.receiver->completed.size() == 1);
        CHECK(fixture.normalizer->GetSnapshot().erasedFrames == 1);
    }
}

TEST_CASE("Original pending domain survives invalidation and completion cannot be relabelled", "[normalize-contract]")
{
    Fixture fixture;
    fixture.Start();
    const auto old = fixture.Frame();
    REQUIRE(fixture.normalizer->Submit(old, fixture.texture.Get(), fixture.context.Get()));
    fixture.normalizer->EpochInvalidated(7);
    fixture.normalizer->EpochInvalidated(7);
    REQUIRE(fixture.receiver->invalidated.size() == 1);
    CHECK_FALSE(fixture.normalizer->GetSnapshot().active);
    CHECK(fixture.normalizer->EpochStarted(8, fixture.environment, fixture.device.Get()).code == CaptureError::InvalidFrame);
    auto wrongCompletion = old;
    wrongCompletion.slotGeneration++;
    CHECK(fixture.normalizer->Completed(wrongCompletion, fixture.context.Get(), false).code == CaptureError::InvalidFrame);
    CHECK(fixture.receiver->completed.empty());
    REQUIRE(fixture.normalizer->Completed(old, fixture.context.Get(), false));
    REQUIRE(fixture.receiver->completed.size() == 1);
    CHECK(fixture.receiver->cancelled.front());
    CHECK(fixture.receiver->completed.front().domain.captureEpoch == 7);
    CHECK_FALSE(fixture.receiver->cancelledHasContext);
    REQUIRE(fixture.normalizer->EpochStarted(8, fixture.environment, fixture.device.Get()));
    const auto stale = fixture.Frame(2, 7, 1);
    REQUIRE(fixture.normalizer->Submit(stale, fixture.texture.Get(), fixture.context.Get()));
    CHECK(fixture.receiver->erased.back().domain.captureEpoch == 7);
    CHECK(fixture.receiver->erased.back().reason == CaptureErasureReason::InactiveDomain);
    const auto fresh = fixture.Frame(3, 8, 2);
    REQUIRE(fixture.normalizer->Submit(fresh, fixture.texture.Get(), fixture.context.Get()));
    REQUIRE(fixture.normalizer->Completed(fresh, fixture.context.Get(), false));
    REQUIRE(fixture.receiver->completed.size() == 2);
    CHECK_FALSE(fixture.receiver->cancelled.back());
    CHECK(fixture.receiver->completed.back().domain.captureEpoch == 8);
    CHECK(fixture.receiver->completed.front().domain.sourceId == fixture.receiver->completed.back().domain.sourceId);
    CHECK(fixture.normalizer->GetSnapshot().epochStarts == 2);
}

TEST_CASE("Normalized factories reserve all consumer memory and keep failure outputs unchanged", "[normalize-contract]")
{
    Fixture fixture;
    auto output = fixture.normalizer;
    const auto* const sentinel = output.get();
    auto config = fixture.config;
    config.capture.maximumFrameAgeMilliseconds = 0;
    CHECK(NormalizeConsumer::Create(config, CaptureBackendKind::Wgc, fixture.receiver, output).code == CaptureError::InvalidConfiguration);
    CHECK(output.get() == sentinel);
    config = fixture.config;
    config.capture.pixelFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
    CHECK(NormalizeConsumer::Create(config, CaptureBackendKind::Wgc, fixture.receiver, output).code == CaptureError::InvalidConfiguration);
    CHECK(output.get() == sentinel);
    config = fixture.config;
    // Independent byte arithmetic: six 32x20 BGRA pool surfaces + three 7x5 ROI textures.
    constexpr std::uint64_t captureBytes = 6 * 32 * 20 * 4 + 3 * 7 * 5 * 4;
    fixture.receiver->reservedBytes = config.capture.maximumCaptureBytes - captureBytes;
    REQUIRE(NormalizeConsumer::Create(config, CaptureBackendKind::Wgc, fixture.receiver, output));
    CHECK(output->GetRuntimeConfig().maximumCaptureBytes == captureBytes);
    CHECK(output->GetSnapshot().reservedConsumerBytes == fixture.receiver->reservedBytes);
    const auto saved = output;
    fixture.receiver->reservedBytes++;
    CHECK(NormalizeConsumer::Create(config, CaptureBackendKind::Wgc, fixture.receiver, output).code == CaptureError::ResourceLimit);
    CHECK(output == saved);
    fixture.receiver->reservedBytes = std::numeric_limits<std::uint64_t>::max();
    CHECK(NormalizeConsumer::Create(config, CaptureBackendKind::Wgc, fixture.receiver, output).code == CaptureError::ResourceLimit);
    CHECK(output == saved);
    CHECK(fixture.normalizer->GetSnapshot().domain.sourceId != saved->GetSnapshot().domain.sourceId);
}

TEST_CASE("High-depth normalization reports true encoding and rejects lossy HDR BGRA", "[normalize-contract]")
{
    Fixture lossy;
    lossy.environment.hdr = true;
    CHECK(lossy.normalizer->EpochStarted(7, lossy.environment, lossy.device.Get()).code == CaptureError::Unsupported);
    CHECK(lossy.receiver->started.empty());
    for (const auto backend : {CaptureBackendKind::Wgc, CaptureBackendKind::Dxgi})
    {
        Fixture fixture(backend, DXGI_FORMAT_R16G16B16A16_FLOAT);
        fixture.environment.hdr = true;
        fixture.environment.outputColorSpace = 12;
        fixture.Start();
        const auto raw = fixture.Frame();
        REQUIRE(fixture.normalizer->Submit(raw, fixture.texture.Get(), fixture.context.Get()));
        REQUIRE(fixture.receiver->submitted.size() == 1);
        const auto& metadata = fixture.receiver->submitted.front();
        CHECK(metadata.hdr);
        CHECK(metadata.pixelFormat == DXGI_FORMAT_R16G16B16A16_FLOAT);
        CHECK(metadata.outputColorSpace == 12);
        CHECK(metadata.signalEncoding == (backend == CaptureBackendKind::Wgc ? CaptureSignalEncoding::LinearScRgb : CaptureSignalEncoding::Unknown));
        REQUIRE(fixture.normalizer->Completed(raw, fixture.context.Get(), false));
    }
}

TEST_CASE("SDR G22 FP16 classifies identically across backends", "[normalize-contract]")
{
    // The DXGI duplication backend converts the negotiated SDR scan-out into the FP16 ROI
    // contract without tone mapping, while WGC may deliver the same SDR content natively.
    // Identical SDR G22 metadata must therefore classify identically across backends.
    for (const auto backend : {CaptureBackendKind::Wgc, CaptureBackendKind::Dxgi})
    {
        Fixture fixture(backend, DXGI_FORMAT_R16G16B16A16_FLOAT);
        REQUIRE_FALSE(fixture.environment.hdr);
        REQUIRE(fixture.environment.outputColorSpace == 0);
        fixture.Start();
        const auto raw = fixture.Frame();
        REQUIRE(fixture.normalizer->Submit(raw, fixture.texture.Get(), fixture.context.Get()));
        REQUIRE(fixture.receiver->submitted.size() == 1);
        const auto& metadata = fixture.receiver->submitted.front();
        CHECK(metadata.pixelFormat == DXGI_FORMAT_R16G16B16A16_FLOAT);
        CHECK(metadata.signalEncoding == CaptureSignalEncoding::LinearScRgb);
        REQUIRE(fixture.normalizer->Completed(raw, fixture.context.Get(), false));
    }
    // Control: non-FP16 SDR G22 stays SdrRgb on both backends.
    for (const auto backend : {CaptureBackendKind::Wgc, CaptureBackendKind::Dxgi})
    {
        Fixture fixture(backend);
        fixture.Start();
        const auto raw = fixture.Frame();
        REQUIRE(fixture.normalizer->Submit(raw, fixture.texture.Get(), fixture.context.Get()));
        REQUIRE(fixture.receiver->submitted.size() == 1);
        const auto& metadata = fixture.receiver->submitted.front();
        CHECK(metadata.pixelFormat == DXGI_FORMAT_B8G8R8A8_UNORM);
        CHECK(metadata.signalEncoding == CaptureSignalEncoding::SdrRgb);
        REQUIRE(fixture.normalizer->Completed(raw, fixture.context.Get(), false));
    }
}

TEST_CASE("Consumer failures retain a cancellable completion and wrong-thread use is rejected", "[normalize-contract]")
{
    for (const bool throwing : {false, true})
    {
        Fixture fixture;
        fixture.Start();
        fixture.receiver->throwOnSubmit = throwing;
        fixture.receiver->failSubmit = !throwing;
        const auto raw = fixture.Frame();
        CHECK(fixture.normalizer->Submit(raw, fixture.texture.Get(), fixture.context.Get()).code == CaptureError::ConsumerFailure);
        REQUIRE(fixture.normalizer->Completed(raw, fixture.context.Get(), false));
        REQUIRE(fixture.receiver->completed.size() == 1);
        CHECK(fixture.receiver->cancelled.front());
        CHECK_FALSE(fixture.receiver->cancelledHasContext);
        CHECK(fixture.normalizer->GetSnapshot().acceptedFrames == 0);
    }
    Fixture fixture;
    fixture.Start();
    const auto raw = fixture.Frame();
    CaptureStatus wrongThread;
    std::thread caller([&] { wrongThread = fixture.normalizer->Submit(raw, fixture.texture.Get(), fixture.context.Get()); });
    caller.join();
    CHECK(wrongThread.code == CaptureError::WrongThread);
    CHECK(fixture.receiver->submitted.empty());
    fixture.normalizer->EpochInvalidated(7);
    fixture.receiver->throwOnStart = true;
    CHECK(fixture.normalizer->EpochStarted(8, fixture.environment, fixture.device.Get()).code == CaptureError::ConsumerFailure);
    CHECK_FALSE(fixture.normalizer->GetSnapshot().active);
    REQUIRE(fixture.receiver->invalidated.size() == 2);
    CHECK(fixture.receiver->invalidated.back().captureEpoch == 8);
}
