#include "capture_runtime.h"
#include "d3d_roi_ring.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstring>
#include <limits>
#include <span>
#include <thread>
#include <vector>

using namespace pbcapturenormalize;
using namespace pbcapturenormalize::detail;
using Microsoft::WRL::ComPtr;

namespace
{
constexpr std::array rotations{DXGI_MODE_ROTATION_IDENTITY, DXGI_MODE_ROTATION_ROTATE90, DXGI_MODE_ROTATION_ROTATE180, DXGI_MODE_ROTATION_ROTATE270};
constexpr std::array formats{DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT};

CaptureEnvironment MakeEnvironment(const DXGI_MODE_ROTATION rotation, const DXGI_FORMAT format, const CaptureSize rawSize, const RECT relativeRoi)
{
    CaptureEnvironment environment;
    environment.backendKind = CaptureBackendKind::Dxgi;
    environment.sourceSize = rawSize;
    environment.sourceRotation = rotation;
    environment.pixelFormat = format;
    const bool swapsAxes = rotation == DXGI_MODE_ROTATION_ROTATE90 || rotation == DXGI_MODE_ROTATION_ROTATE270;
    environment.contentSize = swapsAxes ? CaptureSize{rawSize.height, rawSize.width} : rawSize;
    environment.region.monitor = reinterpret_cast<HMONITOR>(std::uintptr_t{1});
    environment.region.monitorPhysicalRect = {-107, -211, -107 + environment.contentSize.width, -211 + environment.contentSize.height};
    environment.region.physicalRect = {relativeRoi.left - 107, relativeRoi.top - 211, relativeRoi.right - 107, relativeRoi.bottom - 211};
    environment.region.dpiX = 144;
    environment.region.dpiY = 192;
    environment.region.rotation = rotation;
    return environment;
}

CaptureConfig MakeConfig(const CaptureEnvironment& environment)
{
    CaptureConfig config;
    config.region = environment.region;
    config.pixelFormat = environment.pixelFormat;
    return config;
}

bool SameLayout(const CaptureLayout& left, const CaptureLayout& right)
{
    return left.sourceBox.left == right.sourceBox.left && left.sourceBox.top == right.sourceBox.top && left.sourceBox.front == right.sourceBox.front &&
           left.sourceBox.right == right.sourceBox.right && left.sourceBox.bottom == right.sourceBox.bottom && left.sourceBox.back == right.sourceBox.back &&
           left.roiWidth == right.roiWidth && left.roiHeight == right.roiHeight && left.poolBufferCount == right.poolBufferCount && left.totalBytes == right.totalBytes;
}

std::size_t PixelBytes(const DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u : 4u;
}

std::array<std::byte, 8> Pixel(const std::uint32_t label, const DXGI_FORMAT format)
{
    std::array<std::byte, 8> pixel{};
    if (format == DXGI_FORMAT_B8G8R8A8_UNORM)
    {
        pixel[0] = static_cast<std::byte>((label * 53) & 255);
        pixel[1] = static_cast<std::byte>((label * 101 + 7) & 255);
        pixel[2] = static_cast<std::byte>((label * 13 + 231) & 255);
        pixel[3] = static_cast<std::byte>((label * 29) & 255);
    }
    else if (format == DXGI_FORMAT_R10G10B10A2_UNORM)
    {
        constexpr std::array<std::uint32_t, 16> levels{0, 1, 2, 3, 511, 512, 513, 1022, 1023, 37, 137, 731, 1001, 17, 255, 256};
        const auto packed = levels[label % levels.size()] | (levels[(label + 5) % levels.size()] << 10) |
                            (levels[(label * 3) % levels.size()] << 20) | ((label & 3) << 30);
        for (std::size_t index = 0; index < 4; index++)
        {
            pixel[index] = static_cast<std::byte>((packed >> (index * 8)) & 255);
        }
    }
    else
    {
        // Independent finite FP16 bytes: signed zeros, subnormals, both signs,
        // SDR values and values above one. No reference float conversion.
        constexpr std::array<std::uint16_t, 16> levels{0x0000, 0x8000, 0x0001, 0x03ff, 0x0400, 0x3c00, 0x4000, 0x4200,
                                                     0x7bff, 0xbc00, 0xc000, 0x8400, 0x3555, 0x3bff, 0x0401, 0x83ff};
        for (std::size_t channel = 0; channel < 4; channel++)
        {
            const auto value = levels[(label + channel * 5) % levels.size()];
            pixel[channel * 2] = static_cast<std::byte>(value & 255);
            pixel[channel * 2 + 1] = static_cast<std::byte>(value >> 8);
        }
    }
    return pixel;
}

std::vector<std::byte> ExpectedPixels(const std::span<const std::uint32_t> labels, const DXGI_FORMAT format, const std::uint32_t seed)
{
    const auto pixelBytes = PixelBytes(format);
    std::vector<std::byte> pixels(labels.size() * pixelBytes);
    for (std::size_t index = 0; index < labels.size(); index++)
    {
        const auto pixel = Pixel(labels[index] + seed, format);
        std::memcpy(pixels.data() + index * pixelBytes, pixel.data(), pixelBytes);
    }
    return pixels;
}

struct SourceTexture
{
    ComPtr<ID3D11Texture2D> texture;
    std::shared_ptr<std::atomic<std::uint32_t>> closes;
};

HRESULT CloseSource(void* const pointer) noexcept
{
    const std::unique_ptr<SourceTexture> source(static_cast<SourceTexture*>(pointer));
    source->closes->fetch_add(1);
    return S_OK;
}

HRESULT GetSourceTexture(void* const pointer, ID3D11Texture2D** const texture) noexcept
{
    return static_cast<SourceTexture*>(pointer)->texture.CopyTo(texture);
}

FrameLease MakeSource(ID3D11Device& device, const CaptureEnvironment& environment, const std::uint32_t seed,
                      const std::shared_ptr<LeaseCounters>& counters, const std::shared_ptr<std::atomic<std::uint32_t>>& closes)
{
    const CaptureSize rawSize = environment.sourceSize == CaptureSize{} ? environment.contentSize : environment.sourceSize;
    const auto numPixels = static_cast<std::size_t>(rawSize.width) * rawSize.height;
    std::vector<std::uint32_t> labels(numPixels);
    for (std::size_t index = 0; index < labels.size(); index++)
    {
        labels[index] = static_cast<std::uint32_t>(index + 1);
    }
    const auto pixels = ExpectedPixels(labels, environment.pixelFormat, seed);
    D3D11_TEXTURE2D_DESC description{};
    description.Width = static_cast<UINT>(rawSize.width);
    description.Height = static_cast<UINT>(rawSize.height);
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = environment.pixelFormat;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    // Deliberately no SRV bind: capture surfaces need not be shader-readable.
    description.BindFlags = 0;
    const D3D11_SUBRESOURCE_DATA data{pixels.data(), description.Width * static_cast<UINT>(PixelBytes(environment.pixelFormat)), 0};
    auto source = std::make_unique<SourceTexture>();
    source->closes = closes;
    REQUIRE(SUCCEEDED(device.CreateTexture2D(&description, &data, &source->texture)));
    return FrameLease(source.release(), CloseSource, GetSourceTexture, counters, environment.contentSize, seed, 1);
}

bool WaitForSlot(D3dRoiRing& ring, const std::size_t slot, CompletionResult* const output = nullptr)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do
    {
        const auto completion = ring.Poll(slot);
        if (!completion.status)
        {
            return false;
        }
        if (completion.complete)
        {
            if (output != nullptr)
            {
                *output = completion;
            }
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

class PixelReadback final : public RawRoiConsumer
{
public:
    CaptureStatus EpochStarted(std::uint64_t, const CaptureEnvironment&, ID3D11Device*) override
    {
        return {};
    }

    CaptureStatus Submit(const RawRoiFrameMetadata&, ID3D11Texture2D* const texture, ID3D11DeviceContext* const context) override
    {
        texture->GetDesc(&description);
        ComPtr<ID3D11Device> device;
        texture->GetDevice(&device);
        auto stagingDescription = description;
        stagingDescription.Usage = D3D11_USAGE_STAGING;
        stagingDescription.BindFlags = 0;
        stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        HRESULT result = device->CreateTexture2D(&stagingDescription, nullptr, &staging);
        if (FAILED(result))
        {
            return FromHresult(result, CaptureStage::Consumer);
        }
        const auto rowBytes = static_cast<std::size_t>(description.Width) * PixelBytes(description.Format);
        pixels.assign(rowBytes * description.Height, std::byte{0});
        context->CopyResource(staging.Get(), texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        // Blocking Map is confined to this independent test oracle, not capture.
        result = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(result))
        {
            return FromHresult(result, CaptureStage::Consumer);
        }
        rowPitch = mapped.RowPitch;
        if (mapped.RowPitch < rowBytes)
        {
            context->Unmap(staging.Get(), 0);
            return CaptureStatus::Failure(CaptureError::InvalidFrame, CaptureStage::Consumer);
        }
        for (std::size_t row = 0; row < description.Height; row++)
        {
            std::memcpy(pixels.data() + row * rowBytes, static_cast<const std::byte*>(mapped.pData) + row * mapped.RowPitch, rowBytes);
        }
        context->Unmap(staging.Get(), 0);
        return {};
    }

    D3D11_TEXTURE2D_DESC description{};
    std::uint32_t rowPitch = 0;
    std::vector<std::byte> pixels;
};

struct GraphicsFixture
{
    GraphicsFixture()
    {
        D3D_FEATURE_LEVEL feature{};
        REQUIRE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_DEBUG,
                                          nullptr, 0, D3D11_SDK_VERSION, &device, &feature, &context)));
    }
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
};

void VerifyPixels(GraphicsFixture& graphics, const CaptureEnvironment& environment, const std::span<const std::uint32_t> expectedLabels, const bool forceQuery)
{
    const auto config = MakeConfig(environment);
    D3dRoiRing ring;
    REQUIRE(ring.Initialize(graphics.device.Get(), graphics.context.Get(), config, environment, forceQuery));
    if (forceQuery)
    {
        REQUIRE_FALSE(ring.UsesFence());
    }
    const auto counters = std::make_shared<LeaseCounters>();
    const auto closes = std::make_shared<std::atomic<std::uint32_t>>(0);
    PixelReadback oracle;
    for (std::uint32_t iteration = 0; iteration < 3; iteration++)
    {
        const auto seed = iteration * 59;
        auto source = MakeSource(*graphics.device.Get(), environment, seed, counters, closes);
        const auto slot = iteration % config.roiTextureCount;
        bool submitted = false;
        const auto copied = ring.Copy(source, slot, submitted);
        REQUIRE(copied);
        REQUIRE(submitted);
        REQUIRE(counters->live == 1);
        REQUIRE(ring.Recreate(config, environment).code == CaptureError::InternalError);
        bool duplicateSubmitted = true;
        REQUIRE(ring.Copy(source, slot, duplicateSubmitted).code == CaptureError::InvalidFrame);
        REQUIRE_FALSE(duplicateSubmitted);
        REQUIRE(ring.Consume(oracle, {}, slot).code == CaptureError::InternalError);
        CompletionResult copyCompletion;
        REQUIRE(WaitForSlot(ring, slot, &copyCompletion));
        REQUIRE(copyCompletion.roiCopyTime100ns.has_value());
        REQUIRE_FALSE(copyCompletion.roiCopyTimingUnavailable);
        source.Reset();
        REQUIRE(counters->live == 0);
        REQUIRE(*closes == iteration + 1);
        REQUIRE(ring.Consume(oracle, {}, slot));
        REQUIRE(WaitForSlot(ring, slot));
        REQUIRE(oracle.description.Width == static_cast<UINT>(environment.region.physicalRect.right - environment.region.physicalRect.left));
        REQUIRE(oracle.description.Height == static_cast<UINT>(environment.region.physicalRect.bottom - environment.region.physicalRect.top));
        REQUIRE(oracle.description.Format == environment.pixelFormat);
        REQUIRE(oracle.description.SampleDesc.Count == 1);
        REQUIRE(oracle.rowPitch >= oracle.description.Width * PixelBytes(environment.pixelFormat));
        REQUIRE(oracle.pixels == ExpectedPixels(expectedLabels, environment.pixelFormat, seed));
        REQUIRE(ring.CheckDebug());
    }
    REQUIRE(ring.Recreate(config, environment));
    REQUIRE(ring.CheckDebug());
    ring.Reset();
}
}

TEST_CASE("Capture layout pins inverse half-open rotation boxes and charges the raw ROI scratch", "[capture][rotation][layout]")
{
    constexpr std::array<D3D11_BOX, 4> expectedBoxes{{{1, 1, 0, 4, 3, 1}, {1, 2, 0, 3, 5, 1}, {4, 3, 0, 7, 5, 1}, {5, 1, 0, 7, 4, 1}}};
    for (std::size_t index = 0; index < rotations.size(); index++)
    {
        for (const auto format : formats)
        {
            INFO("rotation=" << rotations[index] << " format=" << format);
            const auto environment = MakeEnvironment(rotations[index], format, {8, 6}, {1, 1, 4, 3});
            auto config = MakeConfig(environment);
            CaptureLayout layout;
            REQUIRE(ValidateLayout(config, environment, layout));
            const auto& expectedBox = expectedBoxes[index];
            REQUIRE(layout.sourceBox.left == expectedBox.left);
            REQUIRE(layout.sourceBox.top == expectedBox.top);
            REQUIRE(layout.sourceBox.right == expectedBox.right);
            REQUIRE(layout.sourceBox.bottom == expectedBox.bottom);
            REQUIRE(layout.sourceBox.front == 0);
            REQUIRE(layout.sourceBox.back == 1);
            REQUIRE(layout.roiWidth == 3);
            REQUIRE(layout.roiHeight == 2);
            REQUIRE(layout.poolBufferCount == 1);
            const std::uint64_t ringBytes = 6 * PixelBytes(format) * config.roiTextureCount * (index == 0 ? 1 : 2);
            const std::uint64_t totalBytes = 48 * PixelBytes(format) + ringBytes;
            REQUIRE(layout.totalBytes == totalBytes);
            config.maximumRoiBytes = ringBytes;
            config.maximumCaptureBytes = totalBytes;
            REQUIRE(ValidateLayout(config, environment, layout));
            const auto sentinel = layout;
            config.maximumRoiBytes--;
            REQUIRE(ValidateLayout(config, environment, layout).code == CaptureError::ResourceLimit);
            REQUIRE(SameLayout(layout, sentinel));
            config.maximumRoiBytes++;
            config.maximumCaptureBytes--;
            REQUIRE(ValidateLayout(config, environment, layout).code == CaptureError::ResourceLimit);
            REQUIRE(SameLayout(layout, sentinel));
        }
    }
}

TEST_CASE("Capture layout rejects oversized source geometry before scratch accounting atomically", "[capture][conversion][layout]")
{
    const auto validEnvironment = MakeEnvironment(DXGI_MODE_ROTATION_IDENTITY, DXGI_FORMAT_B8G8R8A8_UNORM,
                                                  {8, 6}, {1, 1, 4, 3});
    auto config = MakeConfig(validEnvironment);
    config.roiTextureCount = 4;
    CaptureLayout layout;
    REQUIRE(ValidateLayout(config, validEnvironment, layout));
    const auto sentinel = layout;

    // The physical-desktop contract caps either monitor axis at 16K before
    // any byte accounting. Keep that decisive bound ahead of defensive
    // checked scratch arithmetic and preserve the caller's previous layout.
    const auto sourceEnvironment = MakeEnvironment(DXGI_MODE_ROTATION_IDENTITY, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                                   {16385, 1}, {0, 0, 16385, 1});
    config.region = sourceEnvironment.region;
    config.pixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    config.maximumRoiBytes = std::numeric_limits<std::uint64_t>::max();
    config.maximumCaptureBytes = std::numeric_limits<std::uint64_t>::max();
    REQUIRE(ValidateLayout(config, sourceEnvironment, layout) ==
            CaptureStatus::Failure(CaptureError::InvalidConfiguration, CaptureStage::Region));
    REQUIRE(SameLayout(layout, sentinel));
}

TEST_CASE("D3D SDR FP16 normalization applies the pinned sRGB transfer into BGRA8", "[capture][conversion][d3d]")
{
    GraphicsFixture graphics;
    const auto environment = MakeEnvironment(DXGI_MODE_ROTATION_IDENTITY, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                             {1, 1}, {0, 0, 1, 1});
    auto config = MakeConfig(environment);
    config.pixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    CaptureLayout layout;
    REQUIRE(ValidateLayout(config, environment, layout));
    constexpr std::uint64_t sourcePoolBytes = 8;
    constexpr std::uint64_t outputRingBytes = 4 * 3;
    constexpr std::uint64_t sourceScratchBytes = 8 * 3;
    REQUIRE(layout.totalBytes == sourcePoolBytes + outputRingBytes + sourceScratchBytes);
    config.maximumRoiBytes = outputRingBytes + sourceScratchBytes;
    config.maximumCaptureBytes = layout.totalBytes;
    REQUIRE(ValidateLayout(config, environment, layout));
    const auto sentinel = layout;
    config.maximumRoiBytes--;
    REQUIRE(ValidateLayout(config, environment, layout) ==
            CaptureStatus::Failure(CaptureError::ResourceLimit, CaptureStage::Configuration));
    REQUIRE(SameLayout(layout, sentinel));
    config.maximumRoiBytes++;

    D3dRoiRing ring;
    REQUIRE(ring.Initialize(graphics.device.Get(), graphics.context.Get(), config, environment, true));
    // Independent IEEE-754 binary16 constants: R=.25, G=.5, B=1, A=1.
    // Standard sRGB OETF and UNORM rounding produce BGRA {255,188,137,255}.
    constexpr std::array<std::uint16_t, 4> fp16Pixel{0x3400, 0x3800, 0x3c00, 0x3c00};
    D3D11_TEXTURE2D_DESC sourceDescription{};
    sourceDescription.Width = 1;
    sourceDescription.Height = 1;
    sourceDescription.MipLevels = 1;
    sourceDescription.ArraySize = 1;
    sourceDescription.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    sourceDescription.SampleDesc.Count = 1;
    sourceDescription.Usage = D3D11_USAGE_DEFAULT;
    const D3D11_SUBRESOURCE_DATA sourceData{fp16Pixel.data(), 8, 0};
    const auto counters = std::make_shared<LeaseCounters>();
    const auto closes = std::make_shared<std::atomic<std::uint32_t>>(0);
    auto sourceStorage = std::make_unique<SourceTexture>();
    sourceStorage->closes = closes;
    REQUIRE(SUCCEEDED(graphics.device->CreateTexture2D(&sourceDescription, &sourceData, &sourceStorage->texture)));
    FrameLease source(sourceStorage.release(), CloseSource, GetSourceTexture, counters, environment.contentSize, 1, 1);
    bool submitted = false;
    REQUIRE(ring.Copy(source, 0, submitted));
    REQUIRE(submitted);
    REQUIRE(WaitForSlot(ring, 0));
    source.Reset();
    REQUIRE(*closes == 1);
    PixelReadback oracle;
    REQUIRE(ring.Consume(oracle, {}, 0));
    REQUIRE(WaitForSlot(ring, 0));
    REQUIRE(oracle.description.Format == DXGI_FORMAT_B8G8R8A8_UNORM);
    REQUIRE(oracle.pixels == std::vector<std::byte>{std::byte{255}, std::byte{188}, std::byte{137}, std::byte{255}});
    REQUIRE(ring.CheckDebug());
    ring.Reset();
}

TEST_CASE("Capture layout rejects ambiguous source geometry without changing its output", "[capture][rotation][layout]")
{
    const auto baseline = MakeEnvironment(DXGI_MODE_ROTATION_ROTATE90, DXGI_FORMAT_B8G8R8A8_UNORM, {8, 6}, {1, 1, 4, 3});
    const auto config = MakeConfig(baseline);
    CaptureLayout output;
    REQUIRE(ValidateLayout(config, baseline, output));
    const auto sentinel = output;
    for (const auto sourceSize : {CaptureSize{}, CaptureSize{6, 8}, CaptureSize{8, 0}, CaptureSize{-8, 6}, CaptureSize{16385, 6}})
    {
        auto changed = baseline;
        changed.sourceSize = sourceSize;
        REQUIRE(ValidateLayout(config, changed, output).stage == CaptureStage::Surface);
        REQUIRE(SameLayout(output, sentinel));
    }
    for (const auto invalidRotation : {DXGI_MODE_ROTATION_UNSPECIFIED, static_cast<DXGI_MODE_ROTATION>(5)})
    {
        auto changed = baseline;
        changed.sourceRotation = invalidRotation;
        REQUIRE(ValidateLayout(config, changed, output).stage == CaptureStage::Region);
        REQUIRE(SameLayout(output, sentinel));
    }
    auto changed = baseline;
    changed.region.physicalRect.left = std::numeric_limits<LONG>::min();
    changed.region.physicalRect.right = std::numeric_limits<LONG>::max();
    REQUIRE(ValidateLayout(config, changed, output).stage == CaptureStage::Region);
    REQUIRE(SameLayout(output, sentinel));
    changed = baseline;
    changed.contentSize.width++;
    REQUIRE(ValidateLayout(config, changed, output).stage == CaptureStage::Region);
    REQUIRE(SameLayout(output, sentinel));
    changed = baseline;
    changed.backendKind = static_cast<CaptureBackendKind>(255);
    REQUIRE(ValidateLayout(config, changed, output).stage == CaptureStage::Configuration);
    REQUIRE(SameLayout(output, sentinel));
}

TEST_CASE("WGC visual-oriented source is not rotated twice by monitor metadata", "[capture][rotation][wgc]")
{
    GraphicsFixture graphics;
    auto environment = MakeEnvironment(DXGI_MODE_ROTATION_IDENTITY, DXGI_FORMAT_B8G8R8A8_UNORM, {8, 6}, {1, 1, 4, 3});
    environment.backendKind = CaptureBackendKind::Wgc;
    environment.sourceSize = {};
    environment.region.rotation = DXGI_MODE_ROTATION_ROTATE90;
    const auto config = MakeConfig(environment);
    CaptureLayout layout;
    REQUIRE(ValidateLayout(config, environment, layout));
    REQUIRE(layout.poolBufferCount == config.queuedFrameLimit + config.roiTextureCount + 1);
    REQUIRE(layout.totalBytes == 48 * 4 * layout.poolBufferCount + 6 * 4 * config.roiTextureCount);
    constexpr std::array<std::uint32_t, 6> expected{10, 11, 12, 18, 19, 20};
    VerifyPixels(graphics, environment, expected, true);
}

TEST_CASE("D3D rotation preserves independent corner and high-color bytes without SRV acquisition", "[capture][rotation][d3d]")
{
    // Source labels are [1 2 3; 4 5 6]. Literal expected canvases distinguish
    // clockwise from counterclockwise and catch width/height or alpha mistakes.
    constexpr std::array<std::array<std::uint32_t, 6>, 4> expected{{{1, 2, 3, 4, 5, 6}, {4, 1, 5, 2, 6, 3}, {6, 5, 4, 3, 2, 1}, {3, 6, 2, 5, 1, 4}}};
    GraphicsFixture graphics;
    for (const bool forceQuery : {false, true})
    {
        for (const auto format : formats)
        {
            for (std::size_t index = 0; index < rotations.size(); index++)
            {
                INFO("rotation=" << rotations[index] << " format=" << format << " forceQuery=" << forceQuery);
                const bool swapsAxes = index == 1 || index == 3;
                const auto environment = MakeEnvironment(rotations[index], format, {3, 2}, {0, 0, swapsAxes ? 2 : 3, swapsAxes ? 3 : 2});
                VerifyPixels(graphics, environment, expected[index], forceQuery);
            }
        }
    }
}

TEST_CASE("D3D rotated ROI pins negative desktop origins odd offsets and actual row pitch", "[capture][rotation][d3d]")
{
    constexpr std::array<std::array<std::uint32_t, 6>, 4> expected{{{10, 11, 12, 18, 19, 20}, {34, 26, 18, 35, 27, 19},
                                                                {39, 38, 37, 31, 30, 29}, {15, 23, 31, 14, 22, 30}}};
    GraphicsFixture graphics;
    for (const auto format : formats)
    {
        for (std::size_t index = 0; index < rotations.size(); index++)
        {
            INFO("rotation=" << rotations[index] << " format=" << format);
            const auto environment = MakeEnvironment(rotations[index], format, {8, 6}, {1, 1, 4, 3});
            VerifyPixels(graphics, environment, expected[index], true);
        }
    }
}

TEST_CASE("D3D rotation includes the final one-pixel physical corner", "[capture][rotation][d3d]")
{
    constexpr std::array<std::uint32_t, 4> lastLabels{48, 8, 1, 41};
    GraphicsFixture graphics;
    for (std::size_t index = 0; index < rotations.size(); index++)
    {
        INFO("rotation=" << rotations[index]);
        const bool swapsAxes = index == 1 || index == 3;
        const LONG width = swapsAxes ? 6 : 8;
        const LONG height = swapsAxes ? 8 : 6;
        const auto environment = MakeEnvironment(rotations[index], DXGI_FORMAT_B8G8R8A8_UNORM, {8, 6}, {width - 1, height - 1, width, height});
        VerifyPixels(graphics, environment, std::span<const std::uint32_t>(&lastLabels[index], 1), true);
    }
}

TEST_CASE("D3D rotation rejects visual-sized source descriptors before GPU submission", "[capture][rotation][d3d]")
{
    GraphicsFixture graphics;
    const auto environment = MakeEnvironment(DXGI_MODE_ROTATION_ROTATE90, DXGI_FORMAT_B8G8R8A8_UNORM, {8, 6}, {1, 1, 4, 3});
    const auto config = MakeConfig(environment);
    D3dRoiRing ring;
    REQUIRE(ring.Initialize(graphics.device.Get(), graphics.context.Get(), config, environment, true));
    auto wrongSourceEnvironment = environment;
    wrongSourceEnvironment.sourceSize = environment.contentSize;
    const auto counters = std::make_shared<LeaseCounters>();
    const auto closes = std::make_shared<std::atomic<std::uint32_t>>(0);
    auto source = MakeSource(*graphics.device.Get(), wrongSourceEnvironment, 0, counters, closes);
    bool submitted = true;
    const auto result = ring.Copy(source, 0, submitted);
    REQUIRE(result.code == CaptureError::InvalidFrame);
    REQUIRE(result.stage == CaptureStage::Surface);
    REQUIRE_FALSE(submitted);
    source.Reset();
    REQUIRE(counters->live == 0);
    REQUIRE(*closes == 1);
    REQUIRE(ring.CheckDebug());
}

TEST_CASE("D3D recreate replaces rotation and format resources only after retirement", "[capture][rotation][d3d]")
{
    constexpr std::array<std::array<std::uint32_t, 6>, 3> expected{{{34, 26, 18, 35, 27, 19}, {10, 11, 12, 18, 19, 20}, {15, 23, 31, 14, 22, 30}}};
    constexpr std::array transitionRotations{DXGI_MODE_ROTATION_ROTATE90, DXGI_MODE_ROTATION_IDENTITY, DXGI_MODE_ROTATION_ROTATE270};
    constexpr std::array transitionFormats{DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R10G10B10A2_UNORM};
    GraphicsFixture graphics;
    D3dRoiRing ring;
    const auto counters = std::make_shared<LeaseCounters>();
    const auto closes = std::make_shared<std::atomic<std::uint32_t>>(0);
    PixelReadback oracle;
    for (std::size_t index = 0; index < transitionRotations.size(); index++)
    {
        const auto environment = MakeEnvironment(transitionRotations[index], transitionFormats[index], {8, 6}, {1, 1, 4, 3});
        const auto config = MakeConfig(environment);
        if (index == 0)
        {
            REQUIRE(ring.Initialize(graphics.device.Get(), graphics.context.Get(), config, environment, true));
        }
        else
        {
            REQUIRE(ring.Recreate(config, environment));
        }
        auto source = MakeSource(*graphics.device.Get(), environment, 0, counters, closes);
        bool submitted = false;
        REQUIRE(ring.Copy(source, 0, submitted));
        REQUIRE(submitted);
        REQUIRE(ring.Recreate(config, environment).code == CaptureError::InternalError);
        REQUIRE(WaitForSlot(ring, 0));
        source.Reset();
        REQUIRE(ring.Consume(oracle, {}, 0));
        REQUIRE(ring.Recreate(config, environment).code == CaptureError::InternalError);
        REQUIRE(WaitForSlot(ring, 0));
        REQUIRE(oracle.description.Format == environment.pixelFormat);
        REQUIRE(oracle.pixels == ExpectedPixels(expected[index], environment.pixelFormat, 0));
        REQUIRE(counters->live == 0);
        REQUIRE(*closes == index + 1);
        REQUIRE(ring.CheckDebug());
    }
}
