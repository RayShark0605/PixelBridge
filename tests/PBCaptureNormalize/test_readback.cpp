#include "pbcapturenormalize/diagnostic_readback.h"
#include "diagnostic_readback_internal.h"

#include <catch2/catch_test_macros.hpp>
#include <d3d11_4.h>
#include <d3d11sdklayers.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

using namespace pbcapturenormalize;
using Microsoft::WRL::ComPtr;

namespace
{
constexpr CaptureSize testRoiSize{5, 3};
constexpr std::size_t maximumTestBytes = 256;

template<typename Predicate>
bool WaitFor(Predicate predicate, const std::uint32_t milliseconds = 3000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    do
    {
        if (predicate())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    return predicate();
}

std::int64_t Now100ns()
{
    LARGE_INTEGER frequency{};
    LARGE_INTEGER counter{};
    std::int64_t result = 0;
    REQUIRE(QueryPerformanceFrequency(&frequency));
    REQUIRE(QueryPerformanceCounter(&counter));
    REQUIRE(ConvertQpcTo100ns(counter.QuadPart, frequency.QuadPart, result));
    return result;
}

DiagnosticReadbackConfig MakeConfig()
{
    DiagnosticReadbackConfig config;
    config.maximumRoiSize = testRoiSize;
    config.maximumFrameAgeMilliseconds = 60000;
    config.maximumReadbackBytes = 720;
    return config;
}

ScreenCaptureDomain MakeDomain(const std::uint8_t identity, const std::uint64_t epoch)
{
    ScreenCaptureDomain domain;
    domain.sourceId[0] = static_cast<std::byte>(identity);
    domain.sourceId[15] = static_cast<std::byte>(identity + 17u);
    domain.captureEpoch = epoch;
    return domain;
}

CaptureEnvironment MakeEnvironment(const DXGI_FORMAT format)
{
    CaptureEnvironment environment;
    environment.region = {reinterpret_cast<HMONITOR>(std::uintptr_t{1}), {-105, -203, -100, -200}, {-120, -220, 80, -20}, 144, 192, DXGI_MODE_ROTATION_ROTATE90};
    environment.contentSize = {200, 200};
    environment.sourceSize = {200, 200};
    environment.sourceRotation = DXGI_MODE_ROTATION_ROTATE90;
    environment.backendKind = CaptureBackendKind::Dxgi;
    environment.pixelFormat = format;
    environment.adapterLuid = {0x31415926u, -37};
    environment.bitsPerColor = format == DXGI_FORMAT_B8G8R8A8_UNORM ? 8u : format == DXGI_FORMAT_R10G10B10A2_UNORM ? 10u : 16u;
    return environment;
}

struct Graphics
{
    Graphics()
    {
        D3D_FEATURE_LEVEL feature{};
        REQUIRE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_DEBUG,
                                          nullptr, 0, D3D11_SDK_VERSION, &device, &feature, &context)));
        const D3D11_QUERY_DESC description{D3D11_QUERY_EVENT, 0};
        REQUIRE(SUCCEEDED(device->CreateQuery(&description, &completion)));
        REQUIRE(SUCCEEDED(device.As(&debug)));
    }
    void CompleteGpu() const
    {
        context->End(completion.Get());
        context->Flush();
        REQUIRE(WaitFor([&]
        {
            BOOL complete = FALSE;
            const HRESULT result = context->GetData(completion.Get(), &complete, sizeof(complete), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            REQUIRE(SUCCEEDED(result));
            return result == S_OK && complete != FALSE;
        }, 5000));
    }
    void CheckDebug() const
    {
        for (UINT64 index = 0; index < debug->GetNumStoredMessagesAllowedByRetrievalFilter(); index++)
        {
            SIZE_T bytes = 0;
            REQUIRE(SUCCEEDED(debug->GetMessage(index, nullptr, &bytes)));
            std::vector<std::byte> storage(bytes);
            auto* const message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
            REQUIRE(SUCCEEDED(debug->GetMessage(index, message, &bytes)));
            INFO(message->pDescription);
            REQUIRE(message->Severity != D3D11_MESSAGE_SEVERITY_CORRUPTION);
            REQUIRE(message->Severity != D3D11_MESSAGE_SEVERITY_ERROR);
        }
    }
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Query> completion;
    ComPtr<ID3D11InfoQueue> debug;
};

struct FrameData
{
    ComPtr<ID3D11Texture2D> texture;
    std::vector<std::byte> pixels;
    ScreenCaptureFrame frame;
};

FrameData MakeFrame(const Graphics& graphics, const CaptureEnvironment& environment, const ScreenCaptureDomain& domain,
                    const std::uint64_t observation, const std::uint32_t slotIndex = 0, const std::uint64_t slotGeneration = 1, const std::uint8_t seed = 11)
{
    FrameData result;
    const auto pixelBytes = environment.pixelFormat == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u : 4u;
    result.pixels.resize(static_cast<std::size_t>(testRoiSize.width) * testRoiSize.height * pixelBytes);
    // Byte oracle deliberately includes arbitrary FP16 bit patterns, alpha,
    // and 10-bit packed channels. The readback is not a color conversion.
    for (std::size_t index = 0; index < result.pixels.size(); index++)
    {
        result.pixels[index] = static_cast<std::byte>((index * 73u + seed) & 255u);
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = testRoiSize.width;
    description.Height = testRoiSize.height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = environment.pixelFormat;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA data{result.pixels.data(), static_cast<UINT>(testRoiSize.width) * pixelBytes, 0};
    REQUIRE(SUCCEEDED(graphics.device->CreateTexture2D(&description, &data, &result.texture)));
    result.frame.texture = result.texture.Get();
    auto& metadata = result.frame.metadata;
    metadata.domain = domain;
    metadata.backend = environment.backendKind;
    metadata.captureObservation = observation;
    metadata.sourceGeneration = 71;
    metadata.slotIndex = slotIndex;
    metadata.slotGeneration = slotGeneration;
    metadata.physicalRoi = environment.region.physicalRect;
    metadata.sourceContentSize = environment.contentSize;
    metadata.sourceExtent = environment.sourceSize;
    metadata.roiSize = testRoiSize;
    metadata.displayRotation = environment.region.rotation;
    metadata.sourceTransform = environment.sourceRotation;
    metadata.sourcePixelFormat = environment.pixelFormat;
    metadata.pixelFormat = environment.pixelFormat;
    metadata.adapterLuid = environment.adapterLuid;
    metadata.bitsPerColor = environment.bitsPerColor;
    metadata.timestamp.monotonic100ns = Now100ns();
    metadata.timestamp.rawValue = metadata.timestamp.monotonic100ns;
    metadata.isCursorExcluded = true;
    metadata.sourceCursorState = CursorState::SeparatePointer;
    return result;
}

struct Result
{
    ScreenCaptureFrameMetadata metadata;
    std::array<std::byte, maximumTestBytes> pixels{};
    std::size_t byteCount = 0;
    std::size_t rowPitch = 0;
};

struct ProcessorObservations
{
    std::uint32_t resets = 0;
    std::uint32_t nullResets = 0;
    std::uint32_t analyzes = 0;
    std::uint32_t commits = 0;
    std::uint32_t discards = 0;
    bool inputChanged = false;
    bool badCommit = false;
    bool callbackThreadChanged = false;
    bool blockedWaitExpired = false;
    std::thread::id callbackThread;
    std::optional<ScreenCaptureDomain> currentDomain;
    std::array<Result, 16> committed;
};

struct ProcessorControl
{
    ProcessorObservations Get() const
    {
        const std::lock_guard lock(mutex);
        return observations;
    }
    void Release()
    {
        {
            const std::lock_guard lock(mutex);
            released = true;
        }
        wake.notify_all();
    }
    void CheckThreadLocked()
    {
        if (observations.callbackThread == std::thread::id{})
        {
            observations.callbackThread = std::this_thread::get_id();
        }
        else if (observations.callbackThread != std::this_thread::get_id())
        {
            observations.callbackThreadChanged = true;
        }
    }
    mutable std::mutex mutex;
    std::condition_variable wake;
    ProcessorObservations observations;
    bool blockFirst = false;
    bool released = false;
    bool failAnalyze = false;
    bool throwAnalyze = false;
    bool failFirstOnly = false;
    std::atomic<std::uint32_t> destructions{0};
};

class Processor final : public CpuFrameProcessor
{
public:
    explicit Processor(std::shared_ptr<ProcessorControl> control) : control_(std::move(control))
    {
    }
    ~Processor() override
    {
        control_->destructions++;
    }
    void Reset(std::optional<ScreenCaptureDomain> domain) noexcept override
    {
        const std::lock_guard lock(control_->mutex);
        control_->CheckThreadLocked();
        control_->observations.resets++;
        control_->observations.nullResets += domain ? 0u : 1u;
        control_->observations.currentDomain = std::move(domain);
        candidateValid_ = false;
    }
    CaptureStatus Analyze(const ScreenCaptureFrameMetadata& metadata, const std::span<const std::byte> pixels, const std::size_t rowPitch) override
    {
        if (pixels.size() > candidate_.pixels.size())
        {
            return CaptureStatus::Failure(CaptureError::ResourceLimit, CaptureStage::Consumer);
        }
        std::array<std::byte, maximumTestBytes> before{};
        std::memcpy(before.data(), pixels.data(), pixels.size());
        std::unique_lock lock(control_->mutex);
        control_->CheckThreadLocked();
        control_->observations.analyzes++;
        if (control_->blockFirst && control_->observations.analyzes == 1 &&
            !control_->wake.wait_for(lock, std::chrono::seconds(5), [&] { return control_->released; }))
        {
            control_->observations.blockedWaitExpired = true;
            return CaptureStatus::Failure(CaptureError::Timeout, CaptureStage::Consumer);
        }
        control_->observations.inputChanged = control_->observations.inputChanged || !std::equal(pixels.begin(), pixels.end(), before.begin());
        if (control_->throwAnalyze && (!control_->failFirstOnly || control_->observations.analyzes == 1))
        {
            throw std::runtime_error("readback processor test failure");
        }
        if (control_->failAnalyze && (!control_->failFirstOnly || control_->observations.analyzes == 1))
        {
            return CaptureStatus::Failure(CaptureError::ConsumerFailure, CaptureStage::Consumer);
        }
        candidate_.metadata = metadata;
        candidate_.byteCount = pixels.size();
        candidate_.rowPitch = rowPitch;
        std::memcpy(candidate_.pixels.data(), pixels.data(), pixels.size());
        candidateValid_ = true;
        return {};
    }
    void Commit(const ScreenCaptureFrameMetadata& metadata) noexcept override
    {
        const std::lock_guard lock(control_->mutex);
        control_->CheckThreadLocked();
        auto& observations = control_->observations;
        if (!candidateValid_ || candidate_.metadata.domain != metadata.domain || candidate_.metadata.captureObservation != metadata.captureObservation ||
            observations.currentDomain != metadata.domain || observations.commits >= observations.committed.size())
        {
            observations.badCommit = true;
            return;
        }
        observations.committed[observations.commits] = candidate_;
        observations.commits++;
        candidateValid_ = false;
    }
    void Discard() noexcept override
    {
        const std::lock_guard lock(control_->mutex);
        control_->CheckThreadLocked();
        control_->observations.discards++;
        candidateValid_ = false;
    }

private:
    std::shared_ptr<ProcessorControl> control_;
    Result candidate_;
    bool candidateValid_ = false;
};

struct ReleaseWorkerOnExit
{
    std::shared_ptr<ProcessorControl> control;
    ~ReleaseWorkerOnExit()
    {
        control->Release();
    }
};

std::shared_ptr<DiagnosticCpuReadback> MakeReadback(const std::shared_ptr<ProcessorControl>& control, const DiagnosticReadbackConfig& config = MakeConfig())
{
    std::shared_ptr<DiagnosticCpuReadback> readback;
    REQUIRE(DiagnosticCpuReadback::Create(config, std::make_shared<Processor>(control), readback));
    return readback;
}

void Deliver(const Graphics& graphics, DiagnosticCpuReadback& readback, const FrameData& frame)
{
    REQUIRE(readback.Submit(frame.frame, graphics.context.Get()));
    graphics.CompleteGpu();
    REQUIRE(readback.Completed(frame.frame.metadata, graphics.context.Get(), false));
}

std::uint64_t Drops(const DiagnosticReadbackSnapshot& snapshot, const DiagnosticReadbackDropReason reason)
{
    return snapshot.drops[static_cast<std::size_t>(reason)];
}

void RequireResult(const Result& actual, const FrameData& expected)
{
    REQUIRE(actual.metadata.domain == expected.frame.metadata.domain);
    REQUIRE(actual.metadata.captureObservation == expected.frame.metadata.captureObservation);
    REQUIRE(actual.metadata.sourceGeneration == expected.frame.metadata.sourceGeneration);
    REQUIRE(actual.metadata.slotGeneration == expected.frame.metadata.slotGeneration);
    REQUIRE(actual.metadata.slotIndex == expected.frame.metadata.slotIndex);
    REQUIRE(actual.metadata.roiSize == testRoiSize);
    REQUIRE(actual.metadata.physicalRoi.left == -105);
    REQUIRE(actual.metadata.physicalRoi.top == -203);
    REQUIRE(actual.metadata.sourceTransform == DXGI_MODE_ROTATION_ROTATE90);
    REQUIRE(actual.metadata.adapterLuid.LowPart == 0x31415926u);
    REQUIRE(actual.metadata.adapterLuid.HighPart == -37);
    REQUIRE(actual.metadata.pixelFormat == expected.frame.metadata.pixelFormat);
    REQUIRE(actual.metadata.timestamp == expected.frame.metadata.timestamp);
    REQUIRE(actual.byteCount == expected.pixels.size());
    REQUIRE(actual.rowPitch == expected.pixels.size() / static_cast<std::size_t>(testRoiSize.height));
    REQUIRE(std::equal(expected.pixels.begin(), expected.pixels.end(), actual.pixels.begin()));
}

void RequireCleanProcessor(const ProcessorObservations& observations)
{
    REQUIRE_FALSE(observations.badCommit);
    REQUIRE_FALSE(observations.inputChanged);
    REQUIRE_FALSE(observations.callbackThreadChanged);
    REQUIRE_FALSE(observations.blockedWaitExpired);
    REQUIRE(observations.callbackThread != std::this_thread::get_id());
}

// Keep the actual WARP resources, CopyResource, completion query, and native
// Map/Unmap. Faults exist only at the two OS-return boundaries under review.
class BoundaryGraphics final : public pbcapturenormalize::detail::DiagnosticReadbackGraphicsApi
{
public:
    HRESULT CreateStaging(ID3D11Device* const device, const D3D11_TEXTURE2D_DESC& description, ID3D11Texture2D** const output) noexcept override
    {
        createCalls++;
        if (createCalls == failCreateAt)
        {
            if (stopBeforeCreateFailure != nullptr)
            {
                stopBeforeCreateFailure->RequestStop();
            }
            *output = nullptr;
            return createFailure;
        }
        return device->CreateTexture2D(&description, nullptr, output);
    }

    HRESULT MapStaging(ID3D11DeviceContext* const context, ID3D11Texture2D* const texture, D3D11_MAPPED_SUBRESOURCE& output,
                       bool& usedBlockingRetry) noexcept override
    {
        usedBlockingRetry = false;
        mapCalls++;
        if (stopBeforeMap != nullptr)
        {
            std::exchange(stopBeforeMap, nullptr)->RequestStop();
        }
        const HRESULT failure = std::exchange(nextMapFailure, S_OK);
        if (FAILED(failure))
        {
            return failure;
        }
        const HRESULT result = context->Map(texture, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &output);
        if (SUCCEEDED(result) && corruptMappedPitch)
        {
            lastMappedTexture = texture;
            output.RowPitch = 1;
        }
        return result;
    }

    std::uint32_t createCalls = 0;
    std::uint32_t mapCalls = 0;
    std::uint32_t failCreateAt = 0;
    HRESULT createFailure = S_OK;
    HRESULT nextMapFailure = S_OK;
    bool corruptMappedPitch = false;
    DiagnosticCpuReadback* stopBeforeCreateFailure = nullptr;
    DiagnosticCpuReadback* stopBeforeMap = nullptr;
    ComPtr<ID3D11Texture2D> lastMappedTexture;
};

std::shared_ptr<DiagnosticCpuReadback> MakeBoundaryReadback(const std::shared_ptr<ProcessorControl>& control, const std::shared_ptr<BoundaryGraphics>& graphics)
{
    std::shared_ptr<DiagnosticCpuReadback> readback;
    REQUIRE(pbcapturenormalize::detail::DiagnosticReadbackTestAccess::Create(MakeConfig(), std::make_shared<Processor>(control), graphics, readback));
    return readback;
}
}

TEST_CASE("Diagnostic readback budgets fixed CPU and worst-format staging before allocation")
{
    auto config = MakeConfig();
    DiagnosticReadbackBudget budget;
    REQUIRE(CalculateDiagnosticReadbackBudget(config, budget));
    REQUIRE(budget == DiagnosticReadbackBudget{120, 360, 360, 720});
    const auto sentinel = budget;
    config.maximumReadbackBytes = 719;
    const auto limited = CalculateDiagnosticReadbackBudget(config, budget);
    REQUIRE(limited.code == CaptureError::ResourceLimit);
    REQUIRE(budget == sentinel);
    for (std::uint32_t mutation = 0; mutation < 10; mutation++)
    {
        CAPTURE(mutation);
        config = MakeConfig();
        switch (mutation)
        {
        case 0: config.maximumRoiSize.width = 0; break;
        case 1: config.maximumRoiSize.height = -1; break;
        case 2: config.maximumRoiSize.width = std::numeric_limits<std::int32_t>::max(); break;
        case 3: config.maximumRoiSize.height = 16385; break;
        case 4: config.stagingTextureCount = 1; break;
        case 5: config.stagingTextureCount = std::numeric_limits<std::uint32_t>::max(); break;
        case 6: config.maximumFrameAgeMilliseconds = 0; break;
        case 7: config.maximumFrameAgeMilliseconds = 60001; break;
        case 8: config.maximumReadbackBytes = 0; break;
        case 9: config.maximumReadbackBytes = std::numeric_limits<std::uint64_t>::max(); break;
        }
        REQUIRE(CalculateDiagnosticReadbackBudget(config, budget).code == CaptureError::InvalidConfiguration);
        REQUIRE(budget == sentinel);
    }
    config = MakeConfig();
    config.maximumRoiSize = {16384, 16384};
    config.stagingTextureCount = 8;
    config.maximumReadbackBytes = std::numeric_limits<std::uint64_t>::max() - 1;
    REQUIRE(CalculateDiagnosticReadbackBudget(config, budget));
    REQUIRE(budget == DiagnosticReadbackBudget{2147483648ull, 6442450944ull, 17179869184ull, 23622320128ull});
    const auto control = std::make_shared<ProcessorControl>();
    auto readback = MakeReadback(control);
    const auto original = readback;
    REQUIRE(DiagnosticCpuReadback::Create(MakeConfig(), nullptr, readback).code == CaptureError::InvalidConfiguration);
    REQUIRE(readback == original);
    config = MakeConfig();
    config.maximumReadbackBytes = 719;
    REQUIRE(DiagnosticCpuReadback::Create(config, std::make_shared<Processor>(control), readback).code == CaptureError::ResourceLimit);
    REQUIRE(readback == original);
    REQUIRE(readback->ReservedBytes() == 720);
    REQUIRE(readback->Stop());
}

TEST_CASE("Diagnostic readback maps the completed last frame with real pitch and preserves all three formats")
{
    Graphics graphics;
    const auto control = std::make_shared<ProcessorControl>();
    const auto readback = MakeReadback(control);
    std::uint64_t epoch = 1;
    for (const auto format : {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT})
    {
        CAPTURE(format);
        const auto domain = MakeDomain(31, epoch);
        const auto environment = MakeEnvironment(format);
        REQUIRE(readback->DomainStarted(domain, environment, graphics.device.Get()));
        const auto frame = MakeFrame(graphics, environment, domain, 1);
        REQUIRE(readback->Submit(frame.frame, graphics.context.Get()));
        REQUIRE(readback->GetSnapshot().mapCalls == epoch - 1);
        REQUIRE(readback->GetSnapshot().pendingStagingFrames == 1);
        graphics.CompleteGpu();
        REQUIRE(readback->Completed(frame.frame.metadata, graphics.context.Get(), false));
        // No next capture is supplied. Completion itself must wake the worker.
        REQUIRE(WaitFor([&] { return readback->GetSnapshot().committedFrames == epoch; }));
        const auto snapshot = readback->GetSnapshot();
        REQUIRE(snapshot.diagnosticCpuReadback);
        REQUIRE(snapshot.mapCalls == epoch);
        REQUIRE(snapshot.mappedFrames == epoch);
        REQUIRE(snapshot.pendingStagingFrames == 0);
        REQUIRE(snapshot.lastMappedRowPitch >= frame.pixels.size() / testRoiSize.height);
        REQUIRE(snapshot.reservation.totalBytes == 720);
        REQUIRE(snapshot.residentStagingBytes == frame.pixels.size() * 3);
        REQUIRE(snapshot.dropEvents == 0);
        const auto observations = control->Get();
        RequireResult(observations.committed[static_cast<std::size_t>(epoch - 1)], frame);
        RequireCleanProcessor(observations);
        readback->DomainInvalidated(domain);
        REQUIRE(WaitFor([&] { return !readback->GetSnapshot().resetPending; }));
        epoch++;
    }
    REQUIRE(readback->GetSnapshot().readbackBytes == 240);
    REQUIRE(readback->GetSnapshot().invalidations == 3);
    REQUIRE(readback->Stop());
    graphics.CheckDebug();
}

TEST_CASE("Diagnostic readback validates capture slots ROI and age before capture allocation")
{
    const auto control = std::make_shared<ProcessorControl>();
    const auto readback = MakeReadback(control);
    CaptureConfig capture;
    capture.region = MakeEnvironment(DXGI_FORMAT_B8G8R8A8_UNORM).region;
    capture.maximumFrameAgeMilliseconds = 60000;
    REQUIRE(readback->ValidateConfiguration(capture));
    capture.roiTextureCount = 2;
    REQUIRE(readback->ValidateConfiguration(capture));
    for (std::uint32_t mutation = 0; mutation < 7; mutation++)
    {
        auto invalid = capture;
        switch (mutation)
        {
        case 0: invalid.roiTextureCount = 4; break;
        case 1: invalid.region.physicalRect.right++; break;
        case 2: invalid.region.physicalRect.bottom++; break;
        case 3: invalid.region.physicalRect.right = invalid.region.physicalRect.left; break;
        case 4: invalid.region.physicalRect.left = std::numeric_limits<LONG>::min(); invalid.region.physicalRect.right = std::numeric_limits<LONG>::max(); break;
        case 5: invalid.maximumFrameAgeMilliseconds = 0; break;
        case 6: invalid.maximumFrameAgeMilliseconds = 59999; break;
        }
        CAPTURE(mutation);
        const auto status = readback->ValidateConfiguration(invalid);
        REQUIRE(status.code == CaptureError::InvalidConfiguration);
        REQUIRE(status.stage == CaptureStage::Configuration);
        REQUIRE(readback->GetSnapshot().residentStagingBytes == 0);
        REQUIRE(readback->GetSnapshot().domainStarts == 0);
    }
    REQUIRE(readback->Stop());
}

TEST_CASE("Diagnostic readback replaces its single queued frame without overwriting the running CPU lease")
{
    Graphics graphics;
    const auto control = std::make_shared<ProcessorControl>();
    control->blockFirst = true;
    const auto readback = MakeReadback(control);
    const ReleaseWorkerOnExit releaseOnExit{control};
    const auto domain = MakeDomain(9, 1);
    const auto environment = MakeEnvironment(DXGI_FORMAT_B8G8R8A8_UNORM);
    REQUIRE(readback->DomainStarted(domain, environment, graphics.device.Get()));
    const auto first = MakeFrame(graphics, environment, domain, 1, 0, 1, 17);
    const auto second = MakeFrame(graphics, environment, domain, 2, 1, 1, 73);
    const auto third = MakeFrame(graphics, environment, domain, 3, 0, 2, 193);
    Deliver(graphics, *readback, first);
    REQUIRE(WaitFor([&] { return control->Get().analyzes == 1; }));
    Deliver(graphics, *readback, second);
    REQUIRE(readback->GetSnapshot().queuedFrames == 1);
    Deliver(graphics, *readback, third);
    const auto pending = readback->GetSnapshot();
    REQUIRE(pending.queuedFrames == 1);
    REQUIRE(pending.queueHighWater == 1);
    REQUIRE(pending.cpuBuffersInUse == 2);
    REQUIRE(pending.cpuBufferHighWater == 3);
    REQUIRE(Drops(pending, DiagnosticReadbackDropReason::QueueReplaced) == 1);
    control->Release();
    REQUIRE(WaitFor([&] { return readback->GetSnapshot().committedFrames == 2; }));
    const auto observations = control->Get();
    REQUIRE(observations.analyzes == 2);
    RequireResult(observations.committed[0], first);
    RequireResult(observations.committed[1], third);
    RequireCleanProcessor(observations);
    REQUIRE(readback->GetSnapshot().cpuBuffersInUse == 0);
    REQUIRE(readback->Stop());
    graphics.CheckDebug();
}

TEST_CASE("Diagnostic readback invalidates delayed candidates and reuses one CPU pool across device and format generations")
{
    Graphics firstGraphics;
    Graphics secondGraphics;
    const auto control = std::make_shared<ProcessorControl>();
    control->blockFirst = true;
    const auto readback = MakeReadback(control);
    const ReleaseWorkerOnExit releaseOnExit{control};
    const auto firstDomain = MakeDomain(11, 1);
    const auto secondDomain = MakeDomain(12, 1);
    const auto firstEnvironment = MakeEnvironment(DXGI_FORMAT_B8G8R8A8_UNORM);
    const auto secondEnvironment = MakeEnvironment(DXGI_FORMAT_R16G16B16A16_FLOAT);
    REQUIRE(readback->DomainStarted(firstDomain, firstEnvironment, firstGraphics.device.Get()));
    const auto oldRunning = MakeFrame(firstGraphics, firstEnvironment, firstDomain, 1, 0, 1, 19);
    const auto oldQueued = MakeFrame(firstGraphics, firstEnvironment, firstDomain, 2, 1, 1, 37);
    Deliver(firstGraphics, *readback, oldRunning);
    REQUIRE(WaitFor([&] { return control->Get().analyzes == 1; }));
    Deliver(firstGraphics, *readback, oldQueued);
    readback->DomainInvalidated(firstDomain);
    REQUIRE_FALSE(readback->GetSnapshot().active);
    REQUIRE(readback->GetSnapshot().resetPending);
    REQUIRE(readback->GetSnapshot().queuedFrames == 0);
    REQUIRE(readback->DomainStarted(secondDomain, secondEnvironment, secondGraphics.device.Get()));
    const auto current = MakeFrame(secondGraphics, secondEnvironment, secondDomain, 1, 0, 1, 211);
    Deliver(secondGraphics, *readback, current);
    REQUIRE(readback->GetSnapshot().residentStagingBytes == 360);
    REQUIRE(readback->ReservedBytes() == 720);
    REQUIRE(readback->GetSnapshot().cpuBufferHighWater <= 3);
    control->Release();
    REQUIRE(WaitFor([&] { return readback->GetSnapshot().committedFrames == 1; }));
    const auto observations = control->Get();
    REQUIRE(observations.analyzes == 2);
    REQUIRE(observations.discards == 1);
    REQUIRE(observations.currentDomain == secondDomain);
    RequireResult(observations.committed[0], current);
    RequireCleanProcessor(observations);
    REQUIRE(Drops(readback->GetSnapshot(), DiagnosticReadbackDropReason::InactiveDomain) == 2);
    REQUIRE(readback->Stop());
    firstGraphics.CheckDebug();
    secondGraphics.CheckDebug();
}

TEST_CASE("Diagnostic readback resets an invalidated domain even when no later frame arrives")
{
    Graphics graphics;
    const auto control = std::make_shared<ProcessorControl>();
    const auto readback = MakeReadback(control);
    const auto domain = MakeDomain(27, 9);
    REQUIRE(readback->DomainStarted(domain, MakeEnvironment(DXGI_FORMAT_B8G8R8A8_UNORM), graphics.device.Get()));
    REQUIRE(WaitFor([&] { return !readback->GetSnapshot().resetPending; }));
    REQUIRE(control->Get().currentDomain == domain);
    readback->DomainInvalidated(domain);
    readback->DomainInvalidated(domain);
    REQUIRE(WaitFor([&] { return !readback->GetSnapshot().resetPending; }));
    const auto observations = control->Get();
    REQUIRE(observations.resets == 2);
    REQUIRE(observations.nullResets == 1);
    REQUIRE_FALSE(observations.currentDomain);
    REQUIRE(observations.analyzes == 0);
    REQUIRE(readback->GetSnapshot().invalidations == 1);
    REQUIRE_FALSE(readback->GetSnapshot().active);
    REQUIRE_FALSE(readback->GetSnapshot().workerStopped);
    REQUIRE(readback->Stop());
}

TEST_CASE("Diagnostic readback ignores stale CPU failures as well as stale successful candidates")
{
    Graphics graphics;
    for (const bool throwFailure : {false, true})
    {
        CAPTURE(throwFailure);
        const auto control = std::make_shared<ProcessorControl>();
        control->blockFirst = true;
        control->failFirstOnly = true;
        control->failAnalyze = !throwFailure;
        control->throwAnalyze = throwFailure;
        const auto readback = MakeReadback(control);
        const ReleaseWorkerOnExit releaseOnExit{control};
        const auto oldDomain = MakeDomain(23, 1);
        const auto currentDomain = MakeDomain(23, 2);
        const auto environment = MakeEnvironment(DXGI_FORMAT_B8G8R8A8_UNORM);
        REQUIRE(readback->DomainStarted(oldDomain, environment, graphics.device.Get()));
        const auto oldFrame = MakeFrame(graphics, environment, oldDomain, 1);
        Deliver(graphics, *readback, oldFrame);
        REQUIRE(WaitFor([&] { return control->Get().analyzes == 1; }));
        readback->DomainInvalidated(oldDomain);
        REQUIRE(readback->DomainStarted(currentDomain, environment, graphics.device.Get()));
        const auto currentFrame = MakeFrame(graphics, environment, currentDomain, 1, 0, 1, 197);
        Deliver(graphics, *readback, currentFrame);
        control->Release();
        REQUIRE(WaitFor([&] { return readback->GetSnapshot().committedFrames == 1; }));
        const auto snapshot = readback->GetSnapshot();
        REQUIRE(snapshot.error);
        REQUIRE(snapshot.active);
        REQUIRE_FALSE(snapshot.stopRequested);
        REQUIRE(snapshot.domain == currentDomain);
        REQUIRE(snapshot.discardedCandidates == 1);
        REQUIRE(Drops(snapshot, DiagnosticReadbackDropReason::ProcessorFailure) == 0);
        REQUIRE(Drops(snapshot, DiagnosticReadbackDropReason::InactiveDomain) == 1);
        RequireResult(control->Get().committed[0], currentFrame);
        REQUIRE(readback->Stop());
    }
    graphics.CheckDebug();
}

TEST_CASE("Diagnostic readback waits for staging retirement and cancellation never touches a context")
{
    Graphics graphics;
    const auto control = std::make_shared<ProcessorControl>();
    const auto readback = MakeReadback(control);
    const auto environment = MakeEnvironment(DXGI_FORMAT_B8G8R8A8_UNORM);
    const auto domain = MakeDomain(29, 1);
    const auto nextDomain = MakeDomain(29, 2);
    REQUIRE(readback->DomainStarted(domain, environment, graphics.device.Get()));
    const auto frame = MakeFrame(graphics, environment, domain, 1);
    REQUIRE(readback->Submit(frame.frame, graphics.context.Get()));
    readback->DomainInvalidated(domain);
    REQUIRE(readback->DomainStarted(nextDomain, environment, graphics.device.Get()).code == CaptureError::InvalidConfiguration);
    REQUIRE(readback->GetSnapshot().pendingStagingFrames == 1);
    graphics.CompleteGpu();
    CaptureStatus completionStatus;
    std::thread deferred([&]
    {
        auto* const forbiddenContext = reinterpret_cast<ID3D11DeviceContext*>(std::uintptr_t{1});
        completionStatus = readback->Completed(frame.frame.metadata, forbiddenContext, true);
    });
    deferred.join();
    REQUIRE(completionStatus);
    REQUIRE(readback->GetSnapshot().mapCalls == 0);
    REQUIRE(readback->GetSnapshot().pendingStagingFrames == 0);
    REQUIRE(Drops(readback->GetSnapshot(), DiagnosticReadbackDropReason::CancelledCompletion) == 1);
    REQUIRE(control->Get().analyzes == 0);
    REQUIRE(readback->DomainStarted(nextDomain, environment, graphics.device.Get()));
    REQUIRE(readback->Stop());
    graphics.CheckDebug();
}

TEST_CASE("Diagnostic readback binds completion to every domain and slot key component")
{
    Graphics graphics;
    const auto control = std::make_shared<ProcessorControl>();
    const auto readback = MakeReadback(control);
    const auto environment = MakeEnvironment(DXGI_FORMAT_B8G8R8A8_UNORM);
    const auto domain = MakeDomain(7, 3);
    REQUIRE(readback->DomainStarted(domain, environment, graphics.device.Get()));
    const auto frame = MakeFrame(graphics, environment, domain, 1);
    REQUIRE(readback->Submit(frame.frame, graphics.context.Get()));
    for (std::uint32_t mutation = 0; mutation < 7; mutation++)
    {
        CAPTURE(mutation);
        auto stale = frame.frame.metadata;
        switch (mutation)
        {
        case 0: stale.domain.sourceId[0] ^= std::byte{1}; break;
        case 1: stale.domain.sourceId[15] ^= std::byte{1}; break;
        case 2: stale.domain.captureEpoch++; break;
        case 3: stale.sourceGeneration++; break;
        case 4: stale.captureObservation++; break;
        case 5: stale.slotIndex++; break;
        case 6: stale.slotGeneration++; break;
        }
        auto* const forbiddenContext = reinterpret_cast<ID3D11DeviceContext*>(std::uintptr_t{1});
        REQUIRE(readback->Completed(stale, forbiddenContext, mutation % 2 == 0));
        REQUIRE(readback->GetSnapshot().pendingStagingFrames == 1);
        REQUIRE(readback->GetSnapshot().mapCalls == 0);
    }
    graphics.CompleteGpu();
    REQUIRE(readback->Completed(frame.frame.metadata, graphics.context.Get(), false));
    REQUIRE(WaitFor([&] { return readback->GetSnapshot().committedFrames == 1; }));
    REQUIRE(Drops(readback->GetSnapshot(), DiagnosticReadbackDropReason::StaleCompletion) == 7);
    const auto replacement = MakeFrame(graphics, environment, domain, 2, 0, 2, 87);
    REQUIRE(readback->Submit(replacement.frame, graphics.context.Get()));
    REQUIRE(readback->Completed(frame.frame.metadata, nullptr, true));
    REQUIRE(readback->GetSnapshot().pendingStagingFrames == 1);
    graphics.CompleteGpu();
    REQUIRE(readback->Completed(replacement.frame.metadata, graphics.context.Get(), false));
    REQUIRE(WaitFor([&] { return readback->GetSnapshot().committedFrames == 2; }));
    RequireResult(control->Get().committed[1], replacement);
    REQUIRE(readback->Stop());
    graphics.CheckDebug();
}

TEST_CASE("Diagnostic readback rejects concurrent owners and malformed owned texture input before copy")
{
    Graphics graphics;
    Graphics foreignGraphics;
    const auto control = std::make_shared<ProcessorControl>();
    const auto readback = MakeReadback(control);
    const auto environment = MakeEnvironment(DXGI_FORMAT_B8G8R8A8_UNORM);
    const auto domain = MakeDomain(3, 1);
    REQUIRE(readback->DomainStarted(domain, environment, graphics.device.Get()));
    CaptureStatus duplicateStatus;
    std::thread otherOwner([&] { duplicateStatus = readback->DomainStarted(MakeDomain(4, 1), environment, foreignGraphics.device.Get()); });
    otherOwner.join();
    REQUIRE(duplicateStatus.code == CaptureError::InvalidConfiguration);
    REQUIRE(readback->GetSnapshot().domain == domain);
    const auto source = MakeFrame(graphics, environment, domain, 1);
    const auto foreignSource = MakeFrame(foreignGraphics, environment, domain, 1);
    ComPtr<ID3D11DeviceContext> deferredContext;
    REQUIRE(SUCCEEDED(graphics.device->CreateDeferredContext(0, &deferredContext)));
    for (std::uint32_t mutation = 0; mutation < 12; mutation++)
    {
        CAPTURE(mutation);
        auto frame = source.frame;
        ID3D11DeviceContext* context = graphics.context.Get();
        switch (mutation)
        {
        case 0: frame.texture = nullptr; break;
        case 1: frame.metadata.captureObservation = 0; break;
        case 2: frame.metadata.sourceGeneration = 0; break;
        case 3: frame.metadata.slotGeneration = 0; break;
        case 4: frame.metadata.slotIndex = 3; break;
        case 5: frame.metadata.roiSize.width++; break;
        case 6: frame.metadata.physicalRoi.left--; break;
        case 7: frame.metadata.isCursorExcluded = false; break;
        case 8: context = nullptr; break;
        case 9: context = foreignGraphics.context.Get(); break;
        case 10: context = deferredContext.Get(); break;
        case 11: frame.texture = foreignSource.texture.Get(); break;
        }
        const auto status = readback->Submit(frame, context);
        REQUIRE(status.code == CaptureError::InvalidFrame);
        REQUIRE(status.stage == CaptureStage::Consumer);
        REQUIRE(readback->GetSnapshot().submittedCopies == 0);
        REQUIRE(readback->GetSnapshot().pendingStagingFrames == 0);
    }
    Deliver(graphics, *readback, source);
    REQUIRE(WaitFor([&] { return readback->GetSnapshot().committedFrames == 1; }));
    REQUIRE(readback->Stop());
    graphics.CheckDebug();
    foreignGraphics.CheckDebug();
}

TEST_CASE("Diagnostic readback rejects expired and future timestamps before reading GPU bytes")
{
    Graphics graphics;
    for (const bool future : {false, true})
    {
        CAPTURE(future);
        const auto control = std::make_shared<ProcessorControl>();
        auto config = MakeConfig();
        config.maximumFrameAgeMilliseconds = 250;
        const auto readback = MakeReadback(control, config);
        const auto domain = MakeDomain(1, 1);
        const auto environment = MakeEnvironment(DXGI_FORMAT_B8G8R8A8_UNORM);
        REQUIRE(readback->DomainStarted(domain, environment, graphics.device.Get()));
        auto frame = MakeFrame(graphics, environment, domain, 1);
        REQUIRE(frame.frame.metadata.timestamp.monotonic100ns > 2510000);
        frame.frame.metadata.timestamp.monotonic100ns = future ? std::numeric_limits<std::int64_t>::max() : frame.frame.metadata.timestamp.monotonic100ns - 2510000;
        REQUIRE(readback->Submit(frame.frame, graphics.context.Get()));
        REQUIRE(readback->GetSnapshot().submittedCopies == 0);
        auto* const forbiddenContext = reinterpret_cast<ID3D11DeviceContext*>(std::uintptr_t{1});
        REQUIRE(readback->Completed(frame.frame.metadata, forbiddenContext, false));
        REQUIRE(readback->GetSnapshot().mapCalls == 0);
        REQUIRE(readback->GetSnapshot().pendingStagingFrames == 0);
        REQUIRE(Drops(readback->GetSnapshot(), future ? DiagnosticReadbackDropReason::InvalidTimestamp : DiagnosticReadbackDropReason::ExpiredBeforeReadback) == 1);
        REQUIRE(readback->Stop());
        REQUIRE(control->Get().analyzes == 0);
    }
}

TEST_CASE("Diagnostic readback checks age after GPU completion and before and after CPU Analyze")
{
    Graphics graphics;
    SECTION("GPU marker delay does not permit an expired Map")
    {
        const auto control = std::make_shared<ProcessorControl>();
        auto config = MakeConfig();
        config.maximumFrameAgeMilliseconds = 100;
        const auto readback = MakeReadback(control, config);
        const auto domain = MakeDomain(2, 1);
        const auto environment = MakeEnvironment(DXGI_FORMAT_B8G8R8A8_UNORM);
        REQUIRE(readback->DomainStarted(domain, environment, graphics.device.Get()));
        const auto frame = MakeFrame(graphics, environment, domain, 1);
        REQUIRE(readback->Submit(frame.frame, graphics.context.Get()));
        graphics.CompleteGpu();
        REQUIRE(WaitFor([&] { return ClassifyFrameAge(Now100ns(), frame.frame.metadata.timestamp.monotonic100ns, 100).disposition == CaptureFrameAgeDisposition::Expired; }));
        REQUIRE(readback->Completed(frame.frame.metadata, graphics.context.Get(), false));
        REQUIRE(readback->GetSnapshot().submittedCopies == 1);
        REQUIRE(readback->GetSnapshot().mapCalls == 0);
        REQUIRE(Drops(readback->GetSnapshot(), DiagnosticReadbackDropReason::ExpiredBeforeReadback) == 1);
        REQUIRE(readback->Stop());
    }
    SECTION("running candidate expires and queued frame expires before Analyze")
    {
        const auto control = std::make_shared<ProcessorControl>();
        control->blockFirst = true;
        auto config = MakeConfig();
        config.maximumFrameAgeMilliseconds = 250;
        const auto readback = MakeReadback(control, config);
        const ReleaseWorkerOnExit releaseOnExit{control};
        const auto domain = MakeDomain(2, 1);
        const auto environment = MakeEnvironment(DXGI_FORMAT_B8G8R8A8_UNORM);
        REQUIRE(readback->DomainStarted(domain, environment, graphics.device.Get()));
        const auto first = MakeFrame(graphics, environment, domain, 1);
        Deliver(graphics, *readback, first);
        REQUIRE(WaitFor([&] { return control->Get().analyzes == 1; }));
        const auto second = MakeFrame(graphics, environment, domain, 2, 1, 1, 93);
        Deliver(graphics, *readback, second);
        REQUIRE(WaitFor([&] { return ClassifyFrameAge(Now100ns(), second.frame.metadata.timestamp.monotonic100ns, 250).disposition == CaptureFrameAgeDisposition::Expired; }));
        control->Release();
        REQUIRE(WaitFor([&] { return readback->GetSnapshot().cpuBuffersInUse == 0; }));
        const auto snapshot = readback->GetSnapshot();
        REQUIRE(snapshot.analyzedFrames == 1);
        REQUIRE(snapshot.committedFrames == 0);
        REQUIRE(snapshot.discardedCandidates == 1);
        REQUIRE(Drops(snapshot, DiagnosticReadbackDropReason::ExpiredBeforeAnalyze) == 1);
        REQUIRE(Drops(snapshot, DiagnosticReadbackDropReason::ExpiredBeforeCommit) == 1);
        REQUIRE(control->Get().discards == 1);
        REQUIRE(readback->Stop());
    }
    graphics.CheckDebug();
}

TEST_CASE("Diagnostic readback rolls back processor failures and reports a terminal CPU error")
{
    Graphics graphics;
    for (const bool throwFailure : {false, true})
    {
        CAPTURE(throwFailure);
        const auto control = std::make_shared<ProcessorControl>();
        control->failAnalyze = !throwFailure;
        control->throwAnalyze = throwFailure;
        const auto readback = MakeReadback(control);
        const auto domain = MakeDomain(13, 1);
        const auto environment = MakeEnvironment(DXGI_FORMAT_B8G8R8A8_UNORM);
        REQUIRE(readback->DomainStarted(domain, environment, graphics.device.Get()));
        const auto frame = MakeFrame(graphics, environment, domain, 1);
        Deliver(graphics, *readback, frame);
        REQUIRE(WaitFor([&] { return readback->GetSnapshot().workerStopped; }));
        const auto snapshot = readback->GetSnapshot();
        REQUIRE(snapshot.error.code == CaptureError::ConsumerFailure);
        REQUIRE(snapshot.stopRequested);
        REQUIRE_FALSE(snapshot.active);
        REQUIRE(snapshot.cpuBuffersInUse == 0);
        REQUIRE(snapshot.committedFrames == 0);
        REQUIRE(snapshot.discardedCandidates == 1);
        REQUIRE(Drops(snapshot, DiagnosticReadbackDropReason::ProcessorFailure) == 1);
        REQUIRE(control->Get().discards == 1);
        REQUIRE_FALSE(control->Get().currentDomain);
        REQUIRE(readback->Stop().code == CaptureError::ConsumerFailure);
    }
}

TEST_CASE("Diagnostic readback destruction cancels without joining a slow CPU processor")
{
    Graphics graphics;
    const auto control = std::make_shared<ProcessorControl>();
    control->blockFirst = true;
    auto readback = MakeReadback(control);
    const ReleaseWorkerOnExit releaseOnExit{control};
    const auto domain = MakeDomain(17, 1);
    const auto environment = MakeEnvironment(DXGI_FORMAT_B8G8R8A8_UNORM);
    REQUIRE(readback->DomainStarted(domain, environment, graphics.device.Get()));
    const auto frame = MakeFrame(graphics, environment, domain, 1);
    Deliver(graphics, *readback, frame);
    REQUIRE(WaitFor([&] { return control->Get().analyzes == 1; }));
    REQUIRE(readback->Stop(5).code == CaptureError::Timeout);
    REQUIRE(readback->GetSnapshot().stopRequested);
    REQUIRE_FALSE(readback->GetSnapshot().active);
    REQUIRE_FALSE(readback->GetSnapshot().workerStopped);
    REQUIRE(control->destructions == 0);
    std::atomic<bool> destructionReturned{false};
    std::thread captureCleanup([owned = std::move(readback), &destructionReturned]() mutable
    {
        owned.reset();
        destructionReturned = true;
    });
    const bool returnedBeforeRelease = WaitFor([&] { return destructionReturned.load(); }, 1000);
    // Always release and join on test failure; the assertion must not strand a
    // blocked worker or turn unwinding into a detached test thread.
    control->Release();
    captureCleanup.join();
    REQUIRE(returnedBeforeRelease);
    REQUIRE(WaitFor([&] { return control->destructions.load() == 1; }));
    const auto observations = control->Get();
    REQUIRE(observations.commits == 0);
    REQUIRE(observations.discards == 1);
    REQUIRE_FALSE(observations.currentDomain);
    RequireCleanProcessor(observations);
    graphics.CheckDebug();
}

TEST_CASE("Diagnostic readback cancellation does not turn later capture submission into a fatal error")
{
    Graphics graphics;
    const auto control = std::make_shared<ProcessorControl>();
    const auto readback = MakeReadback(control);
    const auto domain = MakeDomain(19, 1);
    const auto environment = MakeEnvironment(DXGI_FORMAT_B8G8R8A8_UNORM);
    REQUIRE(readback->DomainStarted(domain, environment, graphics.device.Get()));
    auto frame = MakeFrame(graphics, environment, domain, 1);
    REQUIRE(readback->Stop());
    frame.frame.texture = reinterpret_cast<ID3D11Texture2D*>(std::uintptr_t{1});
    auto* const forbiddenContext = reinterpret_cast<ID3D11DeviceContext*>(std::uintptr_t{1});
    REQUIRE(readback->Submit(frame.frame, forbiddenContext));
    REQUIRE(readback->GetSnapshot().pendingStagingFrames == 1);
    REQUIRE(readback->GetSnapshot().submittedCopies == 0);
    REQUIRE(readback->Completed(frame.frame.metadata, forbiddenContext, false));
    const auto snapshot = readback->GetSnapshot();
    REQUIRE(snapshot.error);
    REQUIRE(snapshot.pendingStagingFrames == 0);
    REQUIRE(snapshot.mapCalls == 0);
    REQUIRE(snapshot.workerStopped);
    REQUIRE(Drops(snapshot, DiagnosticReadbackDropReason::Stopping) == 1);
    REQUIRE(Drops(snapshot, DiagnosticReadbackDropReason::StaleCompletion) == 0);
    REQUIRE(control->Get().analyzes == 0);
}

TEST_CASE("Diagnostic Map device loss invalidates only its domain and permits the same worker on a replacement device")
{
    Graphics oldGraphics;
    Graphics replacementGraphics;
    for (const HRESULT failure : {DXGI_ERROR_DEVICE_REMOVED, DXGI_ERROR_DEVICE_RESET, DXGI_ERROR_DEVICE_HUNG})
    {
        CAPTURE(failure);
        const auto control = std::make_shared<ProcessorControl>();
        const auto boundary = std::make_shared<BoundaryGraphics>();
        const auto readback = MakeBoundaryReadback(control, boundary);
        const auto environment = MakeEnvironment(DXGI_FORMAT_B8G8R8A8_UNORM);
        const auto oldDomain = MakeDomain(41, 1);
        const auto replacementDomain = MakeDomain(41, 2);
        REQUIRE(readback->DomainStarted(oldDomain, environment, oldGraphics.device.Get()));
        const auto oldFrame = MakeFrame(oldGraphics, environment, oldDomain, 1);
        REQUIRE(readback->Submit(oldFrame.frame, oldGraphics.context.Get()));
        oldGraphics.CompleteGpu();
        boundary->nextMapFailure = failure;
        const auto status = readback->Completed(oldFrame.frame.metadata, oldGraphics.context.Get(), false);
        REQUIRE(status.code == CaptureError::NativeFailure);
        REQUIRE(status.nativeError == failure);
        auto snapshot = readback->GetSnapshot();
        REQUIRE(snapshot.error);
        REQUIRE_FALSE(snapshot.stopRequested);
        REQUIRE_FALSE(snapshot.active);
        REQUIRE(snapshot.deviceLossEvents == 1);
        REQUIRE(snapshot.lastGraphicsFailure.nativeError == failure);
        REQUIRE(snapshot.pendingStagingFrames == 0);
        REQUIRE(snapshot.cpuBuffersInUse == 0);
        REQUIRE(snapshot.mappedFrames == 0);
        REQUIRE(snapshot.mapCalls == 1);
        REQUIRE(WaitFor([&] { return !readback->GetSnapshot().resetPending && control->Get().nullResets != 0; }));
        REQUIRE_FALSE(readback->GetSnapshot().workerStopped);

        REQUIRE(readback->DomainStarted(replacementDomain, environment, replacementGraphics.device.Get()));
        const auto replacement = MakeFrame(replacementGraphics, environment, replacementDomain, 1, 0, 1, 201);
        REQUIRE(readback->Submit(replacement.frame, replacementGraphics.context.Get()));
        auto* const forbiddenContext = reinterpret_cast<ID3D11DeviceContext*>(std::uintptr_t{1});
        REQUIRE(readback->Completed(oldFrame.frame.metadata, forbiddenContext, true));
        REQUIRE(readback->GetSnapshot().pendingStagingFrames == 1);
        replacementGraphics.CompleteGpu();
        REQUIRE(readback->Completed(replacement.frame.metadata, replacementGraphics.context.Get(), false));
        REQUIRE(WaitFor([&] { return readback->GetSnapshot().committedFrames == 1; }));
        snapshot = readback->GetSnapshot();
        REQUIRE(snapshot.error);
        REQUIRE(snapshot.active);
        REQUIRE(snapshot.domain == replacementDomain);
        REQUIRE(snapshot.deviceLossEvents == 1);
        REQUIRE(snapshot.lastGraphicsFailure.nativeError == failure);
        REQUIRE(snapshot.pendingStagingFrames == 0);
        REQUIRE(snapshot.mappedFrames == 1);
        REQUIRE(control->Get().analyzes == 1);
        RequireResult(control->Get().committed[0], replacement);
        REQUIRE(readback->Stop());
        RequireCleanProcessor(control->Get());
    }
    oldGraphics.CheckDebug();
    replacementGraphics.CheckDebug();
}

TEST_CASE("Diagnostic staging allocation device loss never permanently stops the CPU worker")
{
    Graphics oldGraphics;
    Graphics replacementGraphics;
    for (const HRESULT failure : {DXGI_ERROR_DEVICE_REMOVED, DXGI_ERROR_DEVICE_RESET, DXGI_ERROR_DEVICE_HUNG})
    {
        for (const bool afterOldDomain : {false, true})
        {
            CAPTURE(failure, afterOldDomain);
            const auto control = std::make_shared<ProcessorControl>();
            const auto boundary = std::make_shared<BoundaryGraphics>();
            const auto readback = MakeBoundaryReadback(control, boundary);
            const auto environment = MakeEnvironment(DXGI_FORMAT_B8G8R8A8_UNORM);
            const auto oldDomain = MakeDomain(47, 1);
            if (afterOldDomain)
            {
                REQUIRE(readback->DomainStarted(oldDomain, environment, oldGraphics.device.Get()));
                const auto oldFrame = MakeFrame(oldGraphics, environment, oldDomain, 1);
                Deliver(oldGraphics, *readback, oldFrame);
                REQUIRE(WaitFor([&] { return readback->GetSnapshot().committedFrames == 1; }));
                readback->DomainInvalidated(oldDomain);
            }
            boundary->failCreateAt = boundary->createCalls + 2;
            boundary->createFailure = failure;
            const auto failedDomain = MakeDomain(47, 2);
            const auto status = readback->DomainStarted(failedDomain, environment, replacementGraphics.device.Get());
            REQUIRE(status.nativeError == failure);
            auto snapshot = readback->GetSnapshot();
            REQUIRE(snapshot.error);
            REQUIRE_FALSE(snapshot.stopRequested);
            REQUIRE_FALSE(snapshot.active);
            REQUIRE(snapshot.residentStagingBytes == 0);
            REQUIRE(snapshot.pendingStagingFrames == 0);
            REQUIRE(snapshot.deviceLossEvents == 1);
            REQUIRE(snapshot.lastGraphicsFailure.nativeError == failure);
            REQUIRE(WaitFor([&] { return !readback->GetSnapshot().resetPending && control->Get().nullResets != 0; }));
            REQUIRE_FALSE(readback->GetSnapshot().workerStopped);

            const auto replacementDomain = MakeDomain(47, 3);
            REQUIRE(readback->DomainStarted(replacementDomain, environment, replacementGraphics.device.Get()));
            const auto replacement = MakeFrame(replacementGraphics, environment, replacementDomain, 1, 0, 1, 179);
            Deliver(replacementGraphics, *readback, replacement);
            const std::uint64_t expectedCommits = afterOldDomain ? 2u : 1u;
            REQUIRE(WaitFor([&] { return readback->GetSnapshot().committedFrames == expectedCommits; }));
            snapshot = readback->GetSnapshot();
            REQUIRE(snapshot.domain == replacementDomain);
            REQUIRE(snapshot.active);
            REQUIRE(snapshot.error);
            REQUIRE(snapshot.deviceLossEvents == 1);
            REQUIRE(snapshot.residentStagingBytes == 180);
            RequireResult(control->Get().committed[afterOldDomain ? 1u : 0u], replacement);
            REQUIRE(readback->Stop());
            RequireCleanProcessor(control->Get());
        }
    }
    oldGraphics.CheckDebug();
    replacementGraphics.CheckDebug();
}

TEST_CASE("Diagnostic non-device Map and staging failures remain terminal")
{
    Graphics graphics;
    for (const bool createFailure : {false, true})
    {
        CAPTURE(createFailure);
        const auto control = std::make_shared<ProcessorControl>();
        const auto boundary = std::make_shared<BoundaryGraphics>();
        const auto readback = MakeBoundaryReadback(control, boundary);
        const auto environment = MakeEnvironment(DXGI_FORMAT_B8G8R8A8_UNORM);
        const auto domain = MakeDomain(59, 1);
        CaptureStatus status;
        if (createFailure)
        {
            boundary->failCreateAt = 2;
            boundary->createFailure = E_FAIL;
            status = readback->DomainStarted(domain, environment, graphics.device.Get());
        }
        else
        {
            REQUIRE(readback->DomainStarted(domain, environment, graphics.device.Get()));
            const auto frame = MakeFrame(graphics, environment, domain, 1);
            REQUIRE(readback->Submit(frame.frame, graphics.context.Get()));
            graphics.CompleteGpu();
            boundary->nextMapFailure = E_FAIL;
            status = readback->Completed(frame.frame.metadata, graphics.context.Get(), false);
        }
        REQUIRE(status.code == CaptureError::NativeFailure);
        REQUIRE(status.nativeError == E_FAIL);
        REQUIRE(WaitFor([&] { return readback->GetSnapshot().workerStopped; }));
        const auto snapshot = readback->GetSnapshot();
        REQUIRE(snapshot.error.nativeError == E_FAIL);
        REQUIRE(snapshot.lastGraphicsFailure.nativeError == E_FAIL);
        REQUIRE(snapshot.deviceLossEvents == 0);
        REQUIRE(snapshot.stopRequested);
        REQUIRE_FALSE(snapshot.active);
        REQUIRE(snapshot.pendingStagingFrames == 0);
        REQUIRE(snapshot.cpuBuffersInUse == 0);
        REQUIRE(snapshot.committedFrames == 0);
        const auto callsBefore = boundary->createCalls;
        REQUIRE(readback->DomainStarted(MakeDomain(59, 2), environment, graphics.device.Get()).nativeError == E_FAIL);
        REQUIRE(boundary->createCalls == callsBefore);
        REQUIRE(readback->Stop().nativeError == E_FAIL);
    }
    graphics.CheckDebug();
}

TEST_CASE("Diagnostic malformed mapped pitch is terminal and still unmaps the real WARP resource")
{
    Graphics graphics;
    const auto control = std::make_shared<ProcessorControl>();
    const auto boundary = std::make_shared<BoundaryGraphics>();
    const auto readback = MakeBoundaryReadback(control, boundary);
    const auto environment = MakeEnvironment(DXGI_FORMAT_B8G8R8A8_UNORM);
    const auto domain = MakeDomain(61, 1);
    REQUIRE(readback->DomainStarted(domain, environment, graphics.device.Get()));
    const auto frame = MakeFrame(graphics, environment, domain, 1);
    REQUIRE(readback->Submit(frame.frame, graphics.context.Get()));
    graphics.CompleteGpu();
    boundary->corruptMappedPitch = true;
    const auto status = readback->Completed(frame.frame.metadata, graphics.context.Get(), false);
    REQUIRE(status.code == CaptureError::InvalidFrame);
    REQUIRE(WaitFor([&] { return readback->GetSnapshot().workerStopped; }));
    const auto snapshot = readback->GetSnapshot();
    REQUIRE(snapshot.error.code == CaptureError::InvalidFrame);
    REQUIRE(snapshot.stopRequested);
    REQUIRE(snapshot.deviceLossEvents == 0);
    REQUIRE(snapshot.lastGraphicsFailure.code == CaptureError::InvalidFrame);
    REQUIRE(snapshot.cpuBuffersInUse == 0);
    REQUIRE(snapshot.committedFrames == 0);
    REQUIRE(boundary->lastMappedTexture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    REQUIRE(SUCCEEDED(graphics.context->Map(boundary->lastMappedTexture.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)));
    graphics.context->Unmap(boundary->lastMappedTexture.Get(), 0);
    REQUIRE(readback->Stop().code == CaptureError::InvalidFrame);
    graphics.CheckDebug();
}

TEST_CASE("Diagnostic graphics loss racing explicit Stop cannot revive a stopped worker")
{
    Graphics graphics;
    for (const bool createFailure : {false, true})
    {
        for (const HRESULT failure : {DXGI_ERROR_DEVICE_REMOVED, DXGI_ERROR_DEVICE_RESET, DXGI_ERROR_DEVICE_HUNG})
        {
            CAPTURE(createFailure, failure);
            const auto control = std::make_shared<ProcessorControl>();
            const auto boundary = std::make_shared<BoundaryGraphics>();
            const auto readback = MakeBoundaryReadback(control, boundary);
            const auto environment = MakeEnvironment(DXGI_FORMAT_B8G8R8A8_UNORM);
            const auto domain = MakeDomain(67, 1);
            CaptureStatus status;
            if (createFailure)
            {
                boundary->failCreateAt = 2;
                boundary->createFailure = failure;
                boundary->stopBeforeCreateFailure = readback.get();
                status = readback->DomainStarted(domain, environment, graphics.device.Get());
            }
            else
            {
                REQUIRE(readback->DomainStarted(domain, environment, graphics.device.Get()));
                const auto frame = MakeFrame(graphics, environment, domain, 1);
                REQUIRE(readback->Submit(frame.frame, graphics.context.Get()));
                graphics.CompleteGpu();
                boundary->nextMapFailure = failure;
                boundary->stopBeforeMap = readback.get();
                status = readback->Completed(frame.frame.metadata, graphics.context.Get(), false);
            }
            REQUIRE(status.nativeError == failure);
            REQUIRE(WaitFor([&] { return readback->GetSnapshot().workerStopped; }));
            const auto snapshot = readback->GetSnapshot();
            REQUIRE(snapshot.error);
            REQUIRE(snapshot.stopRequested);
            REQUIRE_FALSE(snapshot.active);
            REQUIRE(snapshot.deviceLossEvents == 1);
            REQUIRE(snapshot.lastGraphicsFailure.nativeError == failure);
            REQUIRE(snapshot.committedFrames == 0);
            REQUIRE(snapshot.pendingStagingFrames == 0);
            const auto callsBefore = boundary->createCalls;
            REQUIRE(readback->DomainStarted(MakeDomain(67, 2), environment, graphics.device.Get()).code == CaptureError::InvalidConfiguration);
            REQUIRE(boundary->createCalls == callsBefore);
            REQUIRE(readback->Stop());
        }
    }
    graphics.CheckDebug();
}

TEST_CASE("Production readback MapStaging returns mapped bytes after fence-proven GPU completion")
{
    // Environment regression guard: on some D3D11 runtimes (observed on a
    // hardware adapter) a DO_NOT_WAIT Map keeps reporting WAS_STILL_DRAWING
    // even after the fence proved that all GPU work on the resource completed
    // (a blocking Map on the same resource then succeeded immediately). The
    // consumer completion path relies on exactly this contract: a MapStaging
    // call after fence completion must return mappable bytes, whatever the
    // runtime's fast-path readiness bookkeeping reports.
    D3D_FEATURE_LEVEL feature{};
    ID3D11Device* rawDevice = nullptr;
    ID3D11DeviceContext* rawContext = nullptr;
    REQUIRE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                        nullptr, 0, D3D11_SDK_VERSION, &rawDevice, &feature, &rawContext)));
    ComPtr<ID3D11Device> device(rawDevice);
    ComPtr<ID3D11DeviceContext> context(rawContext);
    ComPtr<ID3D11Device5> device5;
    REQUIRE(SUCCEEDED(device.As(&device5)));
    ComPtr<ID3D11DeviceContext4> context4;
    REQUIRE(SUCCEEDED(context.As(&context4)));
    ComPtr<ID3D11Fence> fence;
    REQUIRE(SUCCEEDED(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
    D3D11_TEXTURE2D_DESC sourceDescription{};
    sourceDescription.Width = 32;
    sourceDescription.Height = 16;
    sourceDescription.MipLevels = 1;
    sourceDescription.ArraySize = 1;
    sourceDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sourceDescription.SampleDesc.Count = 1;
    sourceDescription.Usage = D3D11_USAGE_DEFAULT;
    sourceDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> source;
    REQUIRE(SUCCEEDED(device->CreateTexture2D(&sourceDescription, nullptr, &source)));
    detail::NativeReadbackGraphicsApi api;
    D3D11_TEXTURE2D_DESC stagingDescription{};
    stagingDescription.Width = 32;
    stagingDescription.Height = 16;
    stagingDescription.MipLevels = 1;
    stagingDescription.ArraySize = 1;
    stagingDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    stagingDescription.SampleDesc.Count = 1;
    stagingDescription.Usage = D3D11_USAGE_STAGING;
    stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* stagingRaw = nullptr;
    REQUIRE(SUCCEEDED(api.CreateStaging(device.Get(), stagingDescription, &stagingRaw)));
    ComPtr<ID3D11Texture2D> staging(stagingRaw);
    for (UINT64 fenceValue = 1; fenceValue <= 2; fenceValue++)
    {
        context->CopyResource(staging.Get(), source.Get());
        REQUIRE(SUCCEEDED(context4->Signal(fence.Get(), fenceValue)));
        context->Flush();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (fence->GetCompletedValue() < fenceValue)
        {
            if (std::chrono::steady_clock::now() > deadline)
            {
                FAIL("fence did not complete within five seconds");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        bool usedBlockingRetry = true;
        REQUIRE(SUCCEEDED(api.MapStaging(context.Get(), staging.Get(), mapped, usedBlockingRetry)));
        REQUIRE(mapped.pData != nullptr);
        REQUIRE(mapped.RowPitch >= 32u * 4u);
        context->Unmap(staging.Get(), 0);
    }
}
