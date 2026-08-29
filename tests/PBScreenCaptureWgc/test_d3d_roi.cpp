#include "capture_test_support.h"
#include "d3d_roi_ring.h"

#include <catch2/catch_test_macros.hpp>
#include <iostream>

using namespace capturetest;
using Microsoft::WRL::ComPtr;

namespace
{

std::array<std::uint8_t, 4> Pixel(const UINT x, const UINT y, const UINT seed)
{
    return {static_cast<std::uint8_t>((x * 3 + y * 5 + seed) & 255), static_cast<std::uint8_t>((x ^ y ^ seed) & 255),
            static_cast<std::uint8_t>((x * 9 + y * 7 + seed * 13) & 255), 255};
}

struct TextureSource
{
    ComPtr<ID3D11Texture2D> texture;
    std::shared_ptr<std::atomic<int>> closes;
};

HRESULT CloseTextureSource(void* pointer) noexcept
{
    const std::unique_ptr<TextureSource> source(static_cast<TextureSource*>(pointer));
    source->closes->fetch_add(1);
    return S_OK;
}

HRESULT GetTextureSource(void* pointer, ID3D11Texture2D** texture) noexcept
{
    return static_cast<TextureSource*>(pointer)->texture.CopyTo(texture);
}

FrameLease MakeTextureSource(ID3D11Device& device, const CaptureSize size, const UINT seed, const std::shared_ptr<LeaseCounters>& counters,
                            const std::shared_ptr<std::atomic<int>>& closes, const DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM)
{
    std::vector<std::array<std::uint8_t, 4>> pixels(static_cast<std::size_t>(size.width) * size.height);
    for (UINT y = 0; y < static_cast<UINT>(size.height); y++)
    {
        for (UINT x = 0; x < static_cast<UINT>(size.width); x++)
        {
            pixels[static_cast<std::size_t>(y) * size.width + x] = Pixel(x, y, seed);
        }
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = static_cast<UINT>(size.width);
    description.Height = static_cast<UINT>(size.height);
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    const D3D11_SUBRESOURCE_DATA data{pixels.data(), description.Width * 4, 0};
    auto source = std::make_unique<TextureSource>();
    source->closes = closes;
    REQUIRE(SUCCEEDED(device.CreateTexture2D(&description, &data, &source->texture)));
    return FrameLease(source.release(), CloseTextureSource, GetTextureSource, counters, size, seed, 1);
}

class PixelOracle final : public RoiConsumer
{
public:
    CaptureStatus EpochStarted(std::uint64_t, const CaptureEnvironment&, ID3D11Device*) override
    {
        return {};
    }
    CaptureStatus Submit(const RoiFrameMetadata& metadata, ID3D11Texture2D* texture, ID3D11DeviceContext* context) override
    {
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        dimensionsCorrect = description.Width == 30 && description.Height == 20;
        ComPtr<ID3D11Device> device;
        texture->GetDevice(&device);
        description.Usage = D3D11_USAGE_STAGING;
        description.BindFlags = 0;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        const HRESULT created = device->CreateTexture2D(&description, nullptr, &staging);
        if (FAILED(created))
        {
            return FromHresult(created, CaptureStage::Consumer);
        }
        context->CopyResource(staging.Get(), texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT result = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(result))
        {
            return FromHresult(result, CaptureStage::Consumer);
        }
        mismatches = 0;
        for (UINT y = 0; y < description.Height; y++)
        {
            const auto* const row = static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch;
            for (UINT x = 0; x < description.Width; x++)
            {
                // Independent arithmetic oracle, not a copy of the implementation's box.
                const auto expected = Pixel(x + 10, y + 5, static_cast<UINT>(metadata.systemRelativeTime100ns));
                for (std::size_t channel = 0; channel < 4; channel++)
                {
                    mismatches += row[x * 4 + channel] != expected[channel] ? 1u : 0u;
                }
            }
        }
        context->Unmap(staging.Get(), 0);
        return {};
    }
    std::size_t mismatches = 0;
    bool dimensionsCorrect = false;
};

}

TEST_CASE("WGC real D3D11 crop has independently verified pixels and fence/query retirement")
{
    for (const bool forceQuery : {false, true})
    {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL feature{};
        REQUIRE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_DEBUG,
                                          nullptr, 0, D3D11_SDK_VERSION, &device, &feature, &context)));
        const auto config = MakeConfig();
        CaptureEnvironment environment;
        environment.region = config.region;
        environment.contentSize = {100, 80};
        environment.pixelFormat = config.pixelFormat;
        D3dRoiRing ring;
        REQUIRE_FALSE(ring.Recreate(config, environment));
        REQUIRE(ring.Initialize(device.Get(), context.Get(), config, environment, forceQuery));
        REQUIRE_FALSE(ring.Initialize(device.Get(), context.Get(), config, environment, forceQuery));
        if (forceQuery)
        {
            REQUIRE_FALSE(ring.UsesFence());
        }
        std::cout << "WARP retirement=" << (ring.UsesFence() ? "fence" : "event-query") << '\n';
        const auto counters = std::make_shared<LeaseCounters>();
        const auto closes = std::make_shared<std::atomic<int>>(0);
        PixelOracle oracle;
        REQUIRE(ring.Consume(oracle, {}, config.roiTextureCount).code == CaptureError::InternalError);
        auto wrongEnvironment = environment;
        wrongEnvironment.pixelFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        REQUIRE(ring.Recreate(config, wrongEnvironment).code == CaptureError::Unsupported);
        ComPtr<ID3D11Device> otherDevice;
        ComPtr<ID3D11DeviceContext> otherContext;
        REQUIRE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_DEBUG,
                                          nullptr, 0, D3D11_SDK_VERSION, &otherDevice, &feature, &otherContext)));
        D3dRoiRing mixedDeviceRing;
        REQUIRE(mixedDeviceRing.Initialize(device.Get(), otherContext.Get(), config, environment, forceQuery).code == CaptureError::InvalidConfiguration);
        auto foreignSource = MakeTextureSource(*otherDevice.Get(), environment.contentSize, 1, counters, closes);
        bool foreignSubmitted = true;
        REQUIRE(ring.Copy(foreignSource, 0, foreignSubmitted).stage == CaptureStage::Surface);
        REQUIRE_FALSE(foreignSubmitted);
        foreignSource.Reset();
        REQUIRE(counters->live == 0);
        for (UINT seed = 1; seed <= 12; seed++)
        {
            auto source = MakeTextureSource(*device.Get(), environment.contentSize, seed, counters, closes);
            const std::size_t slot = seed % config.roiTextureCount;
            bool submitted = false;
            REQUIRE(ring.Copy(source, slot, submitted));
            REQUIRE(submitted);
            REQUIRE(counters->live == 1);
            REQUIRE_FALSE(ring.Recreate(config, environment));
            REQUIRE(ring.Consume(oracle, {}, slot).code == CaptureError::InternalError);
            REQUIRE(WaitFor([&]
            {
                return ring.Poll(slot).complete;
            }));
            source.Reset();
            REQUIRE(counters->live == 0);
            RoiFrameMetadata metadata;
            metadata.systemRelativeTime100ns = seed;
            REQUIRE(ring.Consume(oracle, metadata, slot));
            REQUIRE(oracle.dimensionsCorrect);
            REQUIRE(oracle.mismatches == 0);
            REQUIRE(WaitFor([&]
            {
                return ring.Poll(slot).complete;
            }));
            REQUIRE(ring.CheckDebug());
        }
        REQUIRE(*closes == 13);
        auto badSource = MakeTextureSource(*device.Get(), {99, 80}, 1, counters, closes);
        badSource.contentSize = environment.contentSize;
        bool submitted = true;
        REQUIRE(ring.Copy(badSource, 0, submitted).code == CaptureError::InvalidFrame);
        REQUIRE_FALSE(submitted);
        badSource.Reset();
        auto wrongFormat = MakeTextureSource(*device.Get(), environment.contentSize, 1, counters, closes, DXGI_FORMAT_R8G8B8A8_UNORM);
        REQUIRE(ring.Copy(wrongFormat, 0, submitted).stage == CaptureStage::Surface);
        REQUIRE_FALSE(submitted);
        wrongFormat.Reset();
        REQUIRE(ring.CheckDebug());
        environment.contentSize = {120, 90};
        environment.region.monitorPhysicalRect.right = 20;
        environment.region.monitorPhysicalRect.bottom = 110;
        REQUIRE(ring.Recreate(config, environment));
        auto resizedSource = MakeTextureSource(*device.Get(), environment.contentSize, 42, counters, closes);
        REQUIRE(ring.Copy(resizedSource, 0, submitted));
        REQUIRE(WaitFor([&]
        {
            return ring.Poll(0).complete;
        }));
        resizedSource.Reset();
        RoiFrameMetadata metadata;
        metadata.systemRelativeTime100ns = 42;
        REQUIRE(ring.Consume(oracle, metadata, 0));
        REQUIRE(oracle.mismatches == 0);
        REQUIRE(WaitFor([&]
        {
            return ring.Poll(0).complete;
        }));
        REQUIRE(ring.CheckDebug());
        REQUIRE(counters->live == 0);
        ring.Reset();
    }
}
