#pragma once

#include "../../apps/PixelBridgeDecoder/bootstrap_diagnostic.h"
#include "pbmodulation/local_desktop_bootstrap.h"

#include <catch2/catch_test_macros.hpp>
#include <d3d11sdklayers.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace bootstrapdiagnostictest
{
using namespace pbcapturenormalize;
using namespace pbdecoder;
using Microsoft::WRL::ComPtr;

inline constexpr std::uint64_t sessionTag = 0x1122334455667788ULL;
inline constexpr CaptureSize smallCanvas{960, 540};

inline std::array<std::byte, 44> CanonicalRecord(const std::uint64_t sequence = 0, const std::uint64_t tag = sessionTag,
                                               const std::uint32_t controlEpoch = 0)
{
    // Independent wire fixture: explicit LE fields and a bitwise CRC32C oracle,
    // never the product serializer, product CRC or an accepted decode result.
    std::array<std::byte, 44> bytes{};
    constexpr std::array<std::uint8_t, 8> prefix{0x50, 0x42, 0x52, 0x47, 1, 1, 0, 2};
    for (std::size_t index = 0; index < prefix.size(); index++)
    {
        bytes[index] = static_cast<std::byte>(prefix[index]);
    }
    const std::array<std::uint64_t, 3> fields{0x50424C4442533031ULL, tag, sequence};
    for (std::size_t field = 0; field < fields.size(); field++)
    {
        for (std::size_t index = 0; index < 8; index++)
        {
            bytes[8 + field * 8 + index] = static_cast<std::byte>((fields[field] >> (index * 8)) & 255u);
        }
    }
    for (std::size_t index = 0; index < 4; index++)
    {
        bytes[32 + index] = static_cast<std::byte>((controlEpoch >> (index * 8)) & 255u);
    }
    std::uint32_t checksum = 0xffffffffu;
    for (std::size_t index = 0; index < 40; index++)
    {
        checksum ^= std::to_integer<std::uint32_t>(bytes[index]);
        for (std::size_t bit = 0; bit < 8; bit++)
        {
            checksum = (checksum >> 1) ^ ((checksum & 1u) != 0 ? 0x82f63b78u : 0u);
        }
    }
    checksum = ~checksum;
    for (std::size_t index = 0; index < 4; index++)
    {
        bytes[40 + index] = static_cast<std::byte>((checksum >> (index * 8)) & 255u);
    }
    return bytes;
}

inline std::string Hex(const std::span<const std::byte> bytes)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string output(bytes.size() * 2, '0');
    for (std::size_t index = 0; index < bytes.size(); index++)
    {
        const auto value = std::to_integer<std::uint8_t>(bytes[index]);
        output[index * 2] = digits[value >> 4];
        output[index * 2 + 1] = digits[value & 15];
    }
    return output;
}

struct Raster
{
    CaptureSize size;
    std::vector<std::byte> pixels;
    [[nodiscard]] std::size_t RowPitch() const noexcept
    {
        return static_cast<std::size_t>(size.width) * 4;
    }
};

inline Raster Render(const std::array<std::byte, 44>& record, const bool halfScale = true)
{
    Raster original{{1920, 1080}, std::vector<std::byte>(1920 * 1080 * 4)};
    REQUIRE(pbmodulation::EncodeLocalDesktopBootstrapFrame(record, original.pixels));
    if (!halfScale)
    {
        return original;
    }
    Raster scaled{smallCanvas, std::vector<std::byte>(960 * 540 * 4)};
    for (std::size_t y = 0; y < 540; y++)
    {
        for (std::size_t x = 0; x < 960; x++)
        {
            std::memcpy(scaled.pixels.data() + (y * 960 + x) * 4, original.pixels.data() + (y * 2 * 1920 + x * 2) * 4, 4);
        }
    }
    return scaled;
}

inline Raster Translate(const Raster& source)
{
    REQUIRE(source.size == smallCanvas);
    Raster translated{{1000, 580}, std::vector<std::byte>(1000 * 580 * 4, std::byte{128})};
    for (std::size_t index = 3; index < translated.pixels.size(); index += 4)
    {
        translated.pixels[index] = std::byte{255};
    }
    for (std::size_t y = 0; y < 540; y++)
    {
        std::memcpy(translated.pixels.data() + ((y + 11) * 1000 + 13) * 4, source.pixels.data() + y * source.RowPitch(), source.RowPitch());
    }
    return translated;
}

inline Raster Recalibrate(const Raster& source)
{
    Raster output = source;
    for (std::size_t index = 0; index < output.pixels.size(); index++)
    {
        if (index % 4 != 3)
        {
            if (output.pixels[index] == std::byte{32})
            {
                output.pixels[index] = std::byte{40};
            }
            else if (output.pixels[index] == std::byte{224})
            {
                output.pixels[index] = std::byte{216};
            }
        }
    }
    return output;
}

inline ScreenCaptureDomain Domain(const std::uint64_t epoch = 1, const std::uint8_t identity = 17)
{
    ScreenCaptureDomain domain;
    for (std::size_t index = 0; index < domain.sourceId.size(); index++)
    {
        domain.sourceId[index] = static_cast<std::byte>((identity + index * 13) & 255u);
    }
    domain.captureEpoch = epoch;
    return domain;
}

inline std::int64_t Now100ns()
{
    LARGE_INTEGER counter{};
    LARGE_INTEGER frequency{};
    std::int64_t timestamp = 0;
    REQUIRE(QueryPerformanceCounter(&counter));
    REQUIRE(QueryPerformanceFrequency(&frequency));
    REQUIRE(ConvertQpcTo100ns(counter.QuadPart, frequency.QuadPart, timestamp));
    return timestamp;
}

inline ScreenCaptureFrameMetadata Metadata(const CaptureSize size, const std::uint64_t observation = 1, const ScreenCaptureDomain& domain = Domain())
{
    REQUIRE(size.width > 0);
    REQUIRE(size.width <= 4096);
    REQUIRE(size.height > 0);
    REQUIRE(size.height <= 2160);
    ScreenCaptureFrameMetadata metadata;
    metadata.domain = domain;
    metadata.captureObservation = observation;
    metadata.sourceGeneration = 23;
    metadata.slotGeneration = observation;
    metadata.physicalRoi = {-37, -53, -37 + size.width, -53 + size.height};
    metadata.sourceContentSize = size;
    metadata.sourceExtent = size;
    metadata.roiSize = size;
    metadata.sourcePixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    metadata.pixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    metadata.adapterLuid = {73, -9};
    metadata.bitsPerColor = 8;
    metadata.signalEncoding = CaptureSignalEncoding::SdrRgb;
    metadata.timestamp.monotonic100ns = Now100ns();
    metadata.timestamp.rawValue = metadata.timestamp.monotonic100ns;
    metadata.isCursorExcluded = true;
    metadata.sourceCursorState = CursorState::Excluded;
    return metadata;
}

inline BootstrapDiagnosticEvent Observe(BootstrapDiagnosticProcessor& processor, const Raster& raster, const ScreenCaptureFrameMetadata& metadata)
{
    REQUIRE(processor.Analyze(metadata, raster.pixels, raster.RowPitch()));
    processor.Commit(metadata);
    BootstrapDiagnosticEvent event;
    REQUIRE(processor.TakeEvent(event));
    REQUIRE(event.capture.domain == metadata.domain);
    REQUIRE(event.capture.captureObservation == metadata.captureObservation);
    return event;
}

inline void RequireAccepted(const BootstrapDiagnosticEvent& event, const std::array<std::byte, 44>& record)
{
    INFO("disposition=" << GetBootstrapDispositionName(event.disposition));
    INFO("visual erasure=" << pbmodulation::GetLocalDesktopErasureName(event.visual.erasure));
    REQUIRE(event.disposition == BootstrapDisposition::Accepted);
    REQUIRE(event.visual.IsAccepted());
    REQUIRE(event.visual.canonical44 == record);
    REQUIRE(event.visual.copies[0].canonical44 == record);
    REQUIRE(event.visual.copies[1].canonical44 == record);
    REQUIRE(event.visual.copies[0].crcValid);
    REQUIRE(event.visual.copies[1].crcValid);
}

template<typename Predicate>
bool Await(Predicate predicate, const std::uint32_t milliseconds = 10000)
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
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC adapterDescription{};
        REQUIRE(SUCCEEDED(device.As(&dxgiDevice)));
        REQUIRE(SUCCEEDED(dxgiDevice->GetAdapter(&adapter)));
        REQUIRE(SUCCEEDED(adapter->GetDesc(&adapterDescription)));
        adapterLuid = adapterDescription.AdapterLuid;
    }
    void Complete() const
    {
        context->End(completion.Get());
        context->Flush();
        REQUIRE(Await([&]
        {
            BOOL ready = FALSE;
            const HRESULT result = context->GetData(completion.Get(), &ready, sizeof(ready), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            REQUIRE(SUCCEEDED(result));
            return result == S_OK && ready != FALSE;
        }));
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
    LUID adapterLuid{};
};

inline CaptureEnvironment Environment(const ScreenCaptureFrameMetadata& metadata)
{
    CaptureEnvironment environment;
    environment.region = {reinterpret_cast<HMONITOR>(std::uintptr_t{1}), metadata.physicalRoi, metadata.physicalRoi, 144, 144, DXGI_MODE_ROTATION_IDENTITY};
    environment.contentSize = metadata.roiSize;
    environment.sourceSize = metadata.roiSize;
    environment.backendKind = metadata.backend;
    environment.pixelFormat = metadata.pixelFormat;
    environment.adapterLuid = metadata.adapterLuid;
    environment.bitsPerColor = metadata.bitsPerColor;
    return environment;
}

inline void Deliver(const Graphics& graphics, DiagnosticCpuReadback& readback, const Raster& raster, ScreenCaptureFrameMetadata metadata)
{
    D3D11_TEXTURE2D_DESC description{};
    description.Width = static_cast<UINT>(raster.size.width);
    description.Height = static_cast<UINT>(raster.size.height);
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA data{raster.pixels.data(), static_cast<UINT>(raster.RowPitch()), 0};
    ComPtr<ID3D11Texture2D> texture;
    REQUIRE(SUCCEEDED(graphics.device->CreateTexture2D(&description, &data, &texture)));
    metadata.adapterLuid = graphics.adapterLuid;
    metadata.timestamp.monotonic100ns = Now100ns();
    metadata.timestamp.rawValue = metadata.timestamp.monotonic100ns;
    REQUIRE(readback.Submit({metadata, texture.Get()}, graphics.context.Get()));
    graphics.Complete();
    REQUIRE(readback.Completed(metadata, graphics.context.Get(), false));
}

} // namespace bootstrapdiagnostictest
