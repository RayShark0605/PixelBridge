#include "test_support.h"
#include "pbrenderd3d/data_window.h"
#include "presentation_backend.h"

#include <catch2/catch_test_macros.hpp>
#include <wrl/client.h>
#include <dxgi1_6.h>
#include <DirectXPackedVector.h>
#include <cstring>
#include <iostream>
#include <string_view>

using namespace dxgitest;
using Microsoft::WRL::ComPtr;

namespace
{

std::array<std::uint8_t, 4> DisplayPixel(const UINT x, const UINT y, const UINT phase)
{
    const UINT bluePhase = phase == 3 ? 2 : phase;
    return {static_cast<std::uint8_t>(((x / 7 + y / 11 + bluePhase) & 1) * 255),
            static_cast<std::uint8_t>(((x / 13 + phase) & 1) * 255),
            static_cast<std::uint8_t>(((y / 5 + phase) & 1) * 255), 255};
}

std::uint16_t ReadSdrWhiteHalf(const wchar_t* deviceName)
{
    std::array<DISPLAYCONFIG_PATH_INFO, 64> paths{};
    std::array<DISPLAYCONFIG_MODE_INFO, 128> modes{};
    UINT32 pathCount = static_cast<UINT32>(paths.size());
    UINT32 modeCount = static_cast<UINT32>(modes.size());
    REQUIRE(QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) == ERROR_SUCCESS);
    for (UINT32 index = 0; index < pathCount; index++)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source), paths[index].sourceInfo.adapterId, paths[index].sourceInfo.id};
        REQUIRE(DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS);
        if (std::wstring_view(source.viewGdiDeviceName) == deviceName)
        {
            DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
            white.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL, sizeof(white), paths[index].targetInfo.adapterId, paths[index].targetInfo.id};
            REQUIRE(DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS);
            REQUIRE(white.SDRWhiteLevel > 0);
            const auto result = DirectX::PackedVector::XMConvertFloatToHalf(static_cast<float>(white.SDRWhiteLevel) / 1000.0f);
            std::cout << "SDRWhiteLevel=" << white.SDRWhiteLevel << " expected scRGB white half=" << result << '\n';
            return result;
        }
    }
    FAIL("No display-configuration target for the captured output");
}

class ScreenOracle final : public RawRoiConsumer
{
public:
    explicit ScreenOracle(const DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM, const std::uint16_t whiteHalf = 0x3c00)
        : format_(format), whiteHalf_(whiteHalf)
    {
    }
    CaptureStatus EpochStarted(const std::uint64_t epoch, const CaptureEnvironment& environment, ID3D11Device*) override
    {
        format_ = environment.pixelFormat;
        hdr_ = environment.hdr;
        lastEpoch = epoch;
        staging_.Reset();
        return {};
    }
    CaptureStatus Submit(const RawRoiFrameMetadata& metadata, ID3D11Texture2D* texture, ID3D11DeviceContext* context) override
    {
        std::int64_t convertedTimestamp = 0;
        if (metadata.timestampDomain != CaptureTimestampDomain::DxgiQpcTicks || metadata.rawTimestamp <= 0 ||
            !ConvertQpcTo100ns(metadata.rawTimestamp, metadata.rawFrequency, convertedTimestamp) || convertedTimestamp != metadata.systemRelativeTime100ns)
        {
            return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Consumer);
        }
        if (!staging_)
        {
            D3D11_TEXTURE2D_DESC description{};
            texture->GetDesc(&description);
            if (description.Width != 128 || description.Height != 96 || description.Format != format_)
            {
                return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Consumer);
            }
            description.Usage = D3D11_USAGE_STAGING;
            description.BindFlags = 0;
            description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Device> device;
            texture->GetDevice(&device);
            const HRESULT result = device->CreateTexture2D(&description, nullptr, &staging_);
            if (FAILED(result))
            {
                return FromHresult(result, CaptureStage::Consumer);
            }
        }
        // Readback exists only in this oracle, not in the production backend or AcquireNextFrame path.
        context->CopyResource(staging_.Get(), texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT result = context->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(result))
        {
            return FromHresult(result, CaptureStage::Consumer);
        }
        std::array<std::uint64_t, 3> mismatches{};
        for (UINT y = 0; y < 96; y++)
        {
            const auto* const row = static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch;
            for (UINT x = 0; x < 128; x++)
            {
                for (UINT phase = 1; phase <= 3; phase++)
                {
                    const auto expected = DisplayPixel(x + 23, y + 31, phase);
                    for (std::size_t channel = 0; channel < 4; channel++)
                    {
                        if (format_ == DXGI_FORMAT_B8G8R8A8_UNORM)
                        {
                            mismatches[phase - 1] += row[x * 4 + channel] != expected[channel] ? 1u : 0u;
                        }
                        else if (format_ == DXGI_FORMAT_R10G10B10A2_UNORM && !hdr_)
                        {
                            constexpr std::array<std::size_t, 4> bgraToRgba{2, 1, 0, 3};
                            std::uint32_t packedPixel = 0;
                            std::memcpy(&packedPixel, row + x * 4, sizeof(packedPixel));
                            const auto actual = channel == 3 ? packedPixel >> 30 : (packedPixel >> (channel * 10)) & 1023;
                            const auto expectedValue = channel == 3 ? 3u : expected[bgraToRgba[channel]] != 0 ? 1023u : 0u;
                            mismatches[phase - 1] += actual != expectedValue ? 1u : 0u;
                        }
                        else if (format_ == DXGI_FORMAT_R16G16B16A16_FLOAT)
                        {
                            constexpr std::array<std::size_t, 4> bgraToRgba{2, 1, 0, 3};
                            const std::uint16_t expectedHalf = channel == 3 ? 0x3c00 : expected[bgraToRgba[channel]] != 0 ? whiteHalf_ : 0;
                            const auto* const pixel = reinterpret_cast<const std::uint16_t*>(row) + x * 4;
                            mismatches[phase - 1] += pixel[channel] != expectedHalf ? 1u : 0u;
                            if (frames == 0 && x == 0 && y == 0 && phase == 1)
                            {
                                std::cout << "half channel=" << channel << " actual=" << pixel[channel] << " expected=" << expectedHalf << '\n';
                            }
                        }
                        else
                        {
                            // This exact-byte oracle does not infer an HDR PQ
                            // white level from the captured image under test.
                            mismatches[phase - 1]++;
                        }
                    }
                }
            }
        }
        context->Unmap(staging_.Get(), 0);
        lastMismatchCount = (std::min)({mismatches[0], mismatches[1], mismatches[2]});
        if (mismatches[0] == 0)
        {
            phaseOne++;
        }
        if (mismatches[1] == 0)
        {
            phaseTwo++;
        }
        if (mismatches[2] == 0)
        {
            phaseThreeEpoch = metadata.captureEpoch;
        }
        if (mismatches[0] == 0 || mismatches[1] == 0 || mismatches[2] == 0)
        {
            verifiedEpoch = metadata.captureEpoch;
        }
        frames++;
        return {};
    }
    std::atomic<std::uint64_t> lastEpoch{0};
    std::atomic<std::uint64_t> verifiedEpoch{0};
    std::atomic<std::uint64_t> frames{0};
    std::atomic<std::uint64_t> phaseOne{0};
    std::atomic<std::uint64_t> phaseTwo{0};
    std::atomic<std::uint64_t> phaseThreeEpoch{0};
    std::atomic<std::uint64_t> lastMismatchCount{0};

private:
    DXGI_FORMAT format_;
    const std::uint16_t whiteHalf_;
    bool hdr_ = false;
    ComPtr<ID3D11Texture2D> staging_;
};

struct Fixture
{
    Fixture()
    {
        windowConfig.width = 384;
        windowConfig.height = 256;
        windowConfig.clientOrigin = pbrenderd3d::PhysicalPoint{PB_TEST_MONITOR_ORIGIN_X, PB_TEST_MONITOR_ORIGIN_Y};
        auto created = pbrenderd3d::DataWindow::Create(windowConfig);
        REQUIRE(created);
        window = std::move(created).Value();
        windowHandle = reinterpret_cast<HWND>(pbrenderd3d::DataWindowTestAccess::GetWindowToken(*window));
        REQUIRE(SetWindowPos(windowHandle, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE));
        REQUIRE(WaitFor([&]
        {
            return window->GetSnapshot().candidateContractSatisfied;
        }, 5000));
        const auto environment = window->GetSnapshot().environment;
        const RECT rectangle{environment.clientOrigin.x + 23, environment.clientOrigin.y + 31,
                             environment.clientOrigin.x + 23 + 128, environment.clientOrigin.y + 31 + 96};
        REQUIRE(pbscreenregion::ResolveScreenCaptureRegion(rectangle, captureConfig.region));
        ComPtr<IDXGIFactory1> factory;
        REQUIRE(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
        for (UINT adapterIndex = 0; adapterIndex < 64; adapterIndex++)
        {
            ComPtr<IDXGIAdapter1> adapter;
            const HRESULT adapterResult = factory->EnumAdapters1(adapterIndex, &adapter);
            if (adapterResult == DXGI_ERROR_NOT_FOUND)
            {
                break;
            }
            REQUIRE(SUCCEEDED(adapterResult));
            for (UINT outputIndex = 0; outputIndex < 64; outputIndex++)
            {
                ComPtr<IDXGIOutput> output;
                const HRESULT outputResult = adapter->EnumOutputs(outputIndex, &output);
                if (outputResult == DXGI_ERROR_NOT_FOUND)
                {
                    break;
                }
                REQUIRE(SUCCEEDED(outputResult));
                DXGI_OUTPUT_DESC description{};
                REQUIRE(SUCCEEDED(output->GetDesc(&description)));
                if (description.Monitor == captureConfig.region.monitor)
                {
                    ComPtr<IDXGIOutput6> output6;
                    REQUIRE(SUCCEEDED(output.As(&output6)));
                    DXGI_OUTPUT_DESC1 color{};
                    REQUIRE(SUCCEEDED(output6->GetDesc1(&color)));
                    DXGI_ADAPTER_DESC1 adapterDescription{};
                    REQUIRE(SUCCEEDED(adapter->GetDesc1(&adapterDescription)));
                    std::cout << "monitor colorspace=" << static_cast<int>(color.ColorSpace) << " bits=" << color.BitsPerColor
                              << " adapterFlags=" << adapterDescription.Flags << '\n';
                    if (color.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 || color.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709)
                    {
                        captureConfig.pixelFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
                        whiteHalf = ReadSdrWhiteHalf(description.DeviceName);
                    }
                }
            }
        }
        captureConfig.requestBorderless = true;
        captureConfig.minUpdateInterval100ns = 100000;
        Present(1);
    }
    void Present(const UINT phase)
    {
        REQUIRE(SetWindowPos(windowHandle, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW));
        std::vector<std::byte> pixels(static_cast<std::size_t>(windowConfig.width) * windowConfig.height * 4);
        for (UINT y = 0; y < windowConfig.height; y++)
        {
            for (UINT x = 0; x < windowConfig.width; x++)
            {
                const auto pixel = DisplayPixel(x, y, phase);
                const auto offset = (static_cast<std::size_t>(y) * windowConfig.width + x) * 4;
                for (std::size_t channel = 0; channel < 4; channel++)
                {
                    pixels[offset + channel] = static_cast<std::byte>(pixel[channel]);
                }
            }
        }
        const auto before = window->GetSnapshot();
        REQUIRE(window->SubmitFrame({pixels, windowConfig.width, windowConfig.height, static_cast<std::size_t>(windowConfig.width) * 4,
                                     phase, before.timing.presentationEpoch}));
        REQUIRE(WaitFor([&]
        {
            return window->GetSnapshot().totalSuccessfulPresents > before.totalSuccessfulPresents;
        }, 5000));
        const POINT point{captureConfig.region.physicalRect.left, captureConfig.region.physicalRect.top};
        const bool onTop = WaitFor([&]
        {
            return GetAncestor(WindowFromPoint(point), GA_ROOT) == windowHandle;
        }, 3000);
        const HWND visible = GetAncestor(WindowFromPoint(point), GA_ROOT);
        std::cout << "data-window=" << windowHandle << " visible-window=" << visible << " roi=(" << point.x << ',' << point.y << ")\n";
        if (visible != windowHandle)
        {
            RECT rectangle{};
            GetWindowRect(windowHandle, &rectangle);
            std::array<wchar_t, 128> className{};
            GetClassNameW(visible, className.data(), static_cast<int>(className.size()));
            std::wcout << L"occluder-class=" << className.data() << L" data-rect=" << rectangle.left << L',' << rectangle.top << L','
                       << rectangle.right << L',' << rectangle.bottom << L" visible=" << IsWindowVisible(windowHandle)
                       << L" exstyle=" << GetWindowLongPtrW(windowHandle, GWL_EXSTYLE) << L'\n';
        }
        REQUIRE(onTop);
    }
    pbrenderd3d::DataWindowConfig windowConfig;
    std::unique_ptr<pbrenderd3d::DataWindow> window;
    HWND windowHandle = nullptr;
    DxgiCaptureConfig captureConfig;
    std::uint16_t whiteHalf = 0x3c00;
};

void PrintSnapshot(const DxgiCaptureSnapshot& snapshot)
{
    std::cout << "state=" << static_cast<int>(snapshot.state) << " error=" << GetCaptureErrorName(snapshot.error.code)
              << " stage=" << static_cast<int>(snapshot.error.stage) << " native=" << snapshot.error.nativeError
              << " epoch=" << snapshot.captureEpoch << " arrived=" << snapshot.arrivedFrames << " delivered=" << snapshot.deliveredFrames
              << " leases=" << snapshot.liveFrameLeases << " highWater=" << snapshot.frameLeaseHighWater
              << " cursorAvailable=" << snapshot.capabilities.cursorDisableAvailable << " cursorExcluded=" << snapshot.capabilities.cursorExcluded
              << " borderAuthorized=" << snapshot.capabilities.borderlessAccessGranted
              << " intervalAvailable=" << snapshot.capabilities.minUpdateIntervalAvailable
              << " intervalApplied=" << snapshot.capabilities.minUpdateIntervalApplied
              << " fence=" << snapshot.capabilities.fenceRetirement << '\n';
    std::cout << "backend=DXGI format=" << static_cast<int>(snapshot.environment.pixelFormat)
              << " rotation=" << static_cast<int>(snapshot.environment.sourceRotation)
              << " timeouts=" << snapshot.acquireTimeouts << " pointerOnly=" << snapshot.pointerOnlyFrames
              << " accumulated=" << snapshot.accumulatedFrames << " accessLost=" << snapshot.accessLostEvents
              << " rebuildReason=" << static_cast<int>(snapshot.lastRebuildReason) << " waitingNative=" << snapshot.waitingNativeError
              << " environmentAttempts=" << snapshot.environmentAttempts << '\n';
}

}

TEST_CASE("DXGI native monitor capture verifies actual screen pixels, Recreate and cleanup")
{
    Fixture fixture;
    for (const bool forceQuery : {false, true})
    {
        const auto consumer = std::make_shared<ScreenOracle>(fixture.captureConfig.pixelFormat, fixture.whiteHalf);
        NativeDxgiOptions options;
        options.forceQuery = forceQuery;
        options.debugLayer = true;
        std::unique_ptr<DxgiCapture> capture;
        const auto status = DxgiCaptureTestAccess::CreateWithBackend(fixture.captureConfig, consumer, MakeNativeDxgiBackend(options), capture);
        INFO("native create error=" << GetCaptureErrorName(status.code) << " stage=" << static_cast<int>(status.stage) << " HRESULT=" << status.nativeError);
        REQUIRE(status);
        fixture.Present(1);
        const bool firstVerified = WaitFor([&]
        {
            return consumer->phaseOne > 0 || !capture->GetSnapshot().error;
        }, 8000);
        PrintSnapshot(capture->GetSnapshot());
        INFO("frames=" << consumer->frames << " mismatches=" << consumer->lastMismatchCount);
        REQUIRE(firstVerified);
        REQUIRE(consumer->phaseOne > 0);
        fixture.Present(2);
        REQUIRE(WaitFor([&]
        {
            return consumer->phaseTwo > 0;
        }, 8000));
        DxgiCaptureTestAccess::RequestRecreate(*capture);
        REQUIRE(WaitFor([&]
        {
            return capture->GetSnapshot().captureEpoch == 2;
        }, 8000));
        fixture.Present(3);
        const bool recreatedPixelsVerified = WaitFor([&]
        {
            return consumer->phaseThreeEpoch == 2 || !capture->GetSnapshot().error;
        }, 8000);
        PrintSnapshot(capture->GetSnapshot());
        INFO("post-Recreate frames=" << consumer->frames << " matchedEpoch=" << consumer->verifiedEpoch << " lastEpoch=" << consumer->lastEpoch);
        REQUIRE(recreatedPixelsVerified);
        REQUIRE(consumer->phaseThreeEpoch == 2);
        const auto beforeStop = capture->GetSnapshot();
        REQUIRE(beforeStop.error);
        REQUIRE(beforeStop.recreates == 1);
        REQUIRE(beforeStop.frameLeaseHighWater == 1);
        REQUIRE(beforeStop.environment.backendKind == CaptureBackendKind::Dxgi);
        REQUIRE_FALSE(beforeStop.capabilities.cursorDisableAvailable);
        REQUIRE_FALSE(beforeStop.capabilities.cursorExcluded);
        REQUIRE_FALSE(beforeStop.capabilities.borderlessAccessGranted);
        REQUIRE_FALSE(beforeStop.capabilities.borderlessSettingApplied);
        if (beforeStop.capabilities.cursorDisableAvailable)
        {
            REQUIRE(beforeStop.capabilities.cursorExcluded);
        }
        if (forceQuery)
        {
            REQUIRE_FALSE(beforeStop.capabilities.fenceRetirement);
        }
        REQUIRE(capture->Stop());
        const auto stopped = capture->GetSnapshot();
        PrintSnapshot(stopped);
        REQUIRE(stopped.shutdownComplete);
        REQUIRE(stopped.liveFrameLeases == 0);
        REQUIRE(stopped.busyRoiTextures == 0);
        REQUIRE_FALSE(stopped.deferredCleanup);
    }
}

TEST_CASE("DXGI native initialization faults and asynchronous timeout cleanup are reversible")
{
    Fixture fixture;
    for (const auto stage : {CaptureStage::Adapter, CaptureStage::Device, CaptureStage::CaptureItem, CaptureStage::TextureRing,
                            CaptureStage::Session})
    {
        NativeDxgiOptions options;
        options.failAfterStage = stage;
        std::cout << "inject native initialization failure after stage=" << static_cast<int>(stage) << std::endl;
        std::unique_ptr<DxgiCapture> capture;
        const auto status = DxgiCaptureTestAccess::CreateWithBackend(fixture.captureConfig, std::make_shared<ScreenOracle>(), MakeNativeDxgiBackend(options), capture);
        REQUIRE_FALSE(status);
        REQUIRE(status.stage == stage);
        REQUIRE_FALSE(capture);
    }
    NativeDxgiOptions options;
    options.holdCompletionPolling = true;
    auto config = fixture.captureConfig;
    config.gpuTimeoutMilliseconds = 200;
    std::unique_ptr<DxgiCapture> capture;
    REQUIRE(DxgiCaptureTestAccess::CreateWithBackend(config, std::make_shared<ScreenOracle>(), MakeNativeDxgiBackend(options), capture));
    fixture.Present(2);
    REQUIRE(WaitFor([&]
    {
        return capture->GetSnapshot().copiedFrames > 0;
    }, 8000));
    REQUIRE(capture->Stop().code == CaptureError::Timeout);
    REQUIRE(WaitFor([&]
    {
        return capture->GetSnapshot().shutdownComplete;
    }, 8000));
    PrintSnapshot(capture->GetSnapshot());
    REQUIRE(capture->GetSnapshot().liveFrameLeases == 0);
    REQUIRE_FALSE(capture->GetSnapshot().deferredCleanup);
}
