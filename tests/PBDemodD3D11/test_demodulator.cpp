#include "pbdemodd3d11/demodulator.h"
#include "pbdemodd3d11/capture_demodulator.h"

#include "../PBModulation/local_desktop_resample_fixtures.h"

#include "pbinnerfec/qc_ldpc_codec.h"
#include "pbmodulation/desktop_levels.h"
#include "pbmodulation/reference_raster.h"
#include "pbmodulation/remote_visual.h"
#include "pbmodulation/remote_visual_low_fps.h"
#include "pbmodulation/shape_chroma.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/transport_block_codec.h"

#include <catch2/catch_test_macros.hpp>

#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{

using Microsoft::WRL::ComPtr;

struct D3DEnvironment
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    LUID adapterLuid{};
    D3D_FEATURE_LEVEL featureLevel{};
};

D3DEnvironment CreateWarpEnvironment()
{
    D3DEnvironment environment;
    const std::array featureLevels{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL selectedLevel{};
    HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        featureLevels.data(), static_cast<UINT>(featureLevels.size()), D3D11_SDK_VERSION,
        &environment.device, &selectedLevel, &environment.context);
    if (result == E_INVALIDARG)
    {
        result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            featureLevels.data() + 1, 1, D3D11_SDK_VERSION, &environment.device, &selectedLevel, &environment.context);
    }
    REQUIRE(SUCCEEDED(result));
    REQUIRE(selectedLevel >= D3D_FEATURE_LEVEL_11_0);
    environment.featureLevel = selectedLevel;

    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapterDescription{};
    REQUIRE(SUCCEEDED(environment.device.As(&dxgiDevice)));
    REQUIRE(SUCCEEDED(dxgiDevice->GetAdapter(&adapter)));
    REQUIRE(SUCCEEDED(adapter->GetDesc(&adapterDescription)));
    environment.adapterLuid = adapterDescription.AdapterLuid;
    return environment;
}

std::array<std::byte, 44> MakeRecord(const std::uint64_t profileId, const std::uint8_t layoutVersion,
    const std::uint64_t sequence, const std::uint64_t sessionTag)
{
    pbprotocol::BootstrapRecord record;
    record.protocolVersion = pbprotocol::GetProtocolVersion();
    record.visualLayoutVersion = layoutVersion;
    record.visualProfileId = profileId;
    record.sessionTag.value = sessionTag;
    record.frameSequence = sequence;
    std::array<std::byte, 44> bytes{};
    REQUIRE(pbprotocol::SerializeBootstrapRecord(record, bytes));
    return bytes;
}

std::vector<std::byte> MakeTransportData(const std::size_t dataBytes, const std::uint32_t codewords,
    const pbprotocol::SessionTag sessionTag)
{
    std::vector<std::byte> data(dataBytes);
    for (std::uint32_t slot = 0; slot < codewords; slot++)
    {
        std::vector<std::byte> payload(31 + slot);
        for (std::size_t index = 0; index < payload.size(); index++)
        {
            payload[index] = static_cast<std::byte>((slot * 17 + index * 29 + 11) & 255);
        }
        const pbprotocol::TransportBlockHeader header{pbprotocol::kTransportBlockTypeData, pbprotocol::kTransportProtocolMinor, 0,
            sessionTag, 700 + slot, 900 + slot, static_cast<std::uint16_t>(payload.size())};
        std::vector<std::byte> serialized(pbprotocol::GetTransportSerializedSize(header));
        REQUIRE(pbprotocol::SerializeTransportBlock(header, payload, serialized));
        std::array<std::byte, pbdesktoplevels::kInfoBytes> information{};
        REQUIRE(pbprotocol::FrameTransportBlockIntoInfoBlock(serialized, information.size(), information));
        REQUIRE(pbinnerfec::EncodeQcLdpcCodeword(pbinnerfec::kInnerFecProfileIdRobust, information,
            std::span(data).subspan(slot * pbdesktoplevels::kCodewordBytes, pbdesktoplevels::kCodewordBytes)));
    }
    return data;
}

std::vector<std::byte> MakeRemoteControlData(const std::span<const std::byte> controlWindow)
{
    REQUIRE(controlWindow.size() == pbmodulation::kReferenceControlWindowBytes);
    std::array<std::byte, pbdesktoplevels::kInfoBytes> information{};
    std::copy(controlWindow.begin(), controlWindow.end(), information.begin());
    std::vector<std::byte> data(pbmodulation::kRemoteVisualDataBytes);
    REQUIRE(pbinnerfec::EncodeQcLdpcCodeword(pbinnerfec::kInnerFecProfileIdRobust, information, data));
    return data;
}

std::vector<std::byte> MakeRemoteVisualLowFpsControlData(const std::span<const std::byte> controlWindow)
{
    REQUIRE(controlWindow.size() == pbmodulation::kReferenceControlWindowBytes);
    std::array<std::byte, pbdesktoplevels::kInfoBytes> information{};
    std::copy(controlWindow.begin(), controlWindow.end(), information.begin());
    std::vector<std::byte> data(pbmodulation::kRemoteVisualLowFpsDataBytes);
    for (std::uint32_t slot = 0; slot < pbmodulation::kRemoteVisualLowFpsCodewords; slot++)
    {
        REQUIRE(pbinnerfec::EncodeQcLdpcCodeword(pbinnerfec::kInnerFecProfileIdRobust, information,
            std::span(data).subspan(static_cast<std::size_t>(slot) * pbdesktoplevels::kCodewordBytes,
                pbdesktoplevels::kCodewordBytes)));
    }
    return data;
}

pbmodulation::LumaView MakeView(const std::span<const std::byte> pixels)
{
    return {pixels, 1920, 1080, 1920 * 4, pbmodulation::LumaPixelFormat::Bgra8};
}

ComPtr<ID3D11Texture2D> UploadRoiTexture(ID3D11Device* device, const std::span<const std::byte> pixels)
{
    REQUIRE(device != nullptr);
    REQUIRE(pixels.size() == pbmodulation::kLocalDesktopFrameBgraBytes);
    D3D11_TEXTURE2D_DESC description{};
    description.Width = 1920;
    description.Height = 1080;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA initial{pixels.data(), 1920 * 4, 0};
    ComPtr<ID3D11Texture2D> texture;
    REQUIRE(SUCCEEDED(device->CreateTexture2D(&description, &initial, &texture)));
    return texture;
}

ComPtr<ID3D11Texture2D> UploadBgraTexture(ID3D11Device* device, const std::span<const std::byte> pixels,
    const std::uint32_t width, const std::uint32_t height, const std::size_t rowPitch)
{
    REQUIRE(device != nullptr);
    REQUIRE(width > 0);
    REQUIRE(height > 0);
    REQUIRE(rowPitch >= static_cast<std::size_t>(width) * 4);
    REQUIRE(rowPitch <= UINT32_MAX);
    REQUIRE(pixels.size() >= rowPitch * height);
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA initial{pixels.data(), static_cast<UINT>(rowPitch), 0};
    ComPtr<ID3D11Texture2D> texture;
    REQUIRE(SUCCEEDED(device->CreateTexture2D(&description, &initial, &texture)));
    return texture;
}

pbcapturenormalize::ScreenCaptureFrame MakeFrame(ID3D11Texture2D* texture, const LUID adapterLuid,
    const pbcapturenormalize::ScreenCaptureDomain& domain, const std::uint64_t observation)
{
    D3D11_TEXTURE2D_DESC description{};
    REQUIRE(texture != nullptr);
    texture->GetDesc(&description);
    REQUIRE(description.Width <= INT32_MAX);
    REQUIRE(description.Height <= INT32_MAX);
    const auto width = static_cast<std::int32_t>(description.Width);
    const auto height = static_cast<std::int32_t>(description.Height);
    pbcapturenormalize::ScreenCaptureFrame frame;
    frame.texture = texture;
    frame.metadata.domain = domain;
    frame.metadata.captureObservation = observation;
    frame.metadata.sourceGeneration = 1;
    frame.metadata.slotGeneration = observation;
    frame.metadata.slotIndex = static_cast<std::uint32_t>(observation % 3);
    frame.metadata.physicalRoi = {0, 0, width, height};
    frame.metadata.sourceContentSize = {width, height};
    frame.metadata.sourceExtent = {width, height};
    frame.metadata.roiSize = {width, height};
    frame.metadata.sourcePixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    frame.metadata.pixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    frame.metadata.adapterLuid = adapterLuid;
    frame.metadata.bitsPerColor = 8;
    frame.metadata.signalEncoding = pbcapturenormalize::CaptureSignalEncoding::SdrRgb;
    frame.metadata.isCursorExcluded = true;
    frame.metadata.sourceCursorState = pbcapturenormalize::CursorState::Excluded;
    return frame;
}

void StampCurrent(pbcapturenormalize::ScreenCaptureFrame& frame)
{
    LARGE_INTEGER frequency{};
    LARGE_INTEGER counter{};
    std::int64_t now100ns = 0;
    REQUIRE(QueryPerformanceFrequency(&frequency));
    REQUIRE(QueryPerformanceCounter(&counter));
    REQUIRE(pbcapturenormalize::ConvertQpcTo100ns(counter.QuadPart, frequency.QuadPart, now100ns));
    frame.metadata.timestamp.rawValue = now100ns;
    frame.metadata.timestamp.rawFrequency = 10000000;
    frame.metadata.timestamp.monotonic100ns = now100ns;
    frame.metadata.timestamp.arrivalQpc100ns = now100ns;
}

void WaitForDownstreamMarker(ID3D11Device* device, ID3D11DeviceContext* context)
{
    D3D11_QUERY_DESC description{};
    description.Query = D3D11_QUERY_EVENT;
    ComPtr<ID3D11Query> marker;
    REQUIRE(SUCCEEDED(device->CreateQuery(&description, &marker)));
    context->End(marker.Get());
    context->Flush();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    for (;;)
    {
        BOOL complete = FALSE;
        const HRESULT result = context->GetData(marker.Get(), &complete, sizeof(complete), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        REQUIRE(SUCCEEDED(result));
        if (result == S_OK && complete)
        {
            return;
        }
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

pbcapturenormalize::CaptureEnvironment MakeCaptureEnvironment(const LUID adapterLuid,
    const std::int32_t width = 1920, const std::int32_t height = 1080)
{
    pbcapturenormalize::CaptureEnvironment environment;
    environment.region.physicalRect = {0, 0, width, height};
    environment.region.monitorPhysicalRect = environment.region.physicalRect;
    environment.contentSize = {width, height};
    environment.sourceSize = {width, height};
    environment.pixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    environment.adapterLuid = adapterLuid;
    environment.displayFrequency = 60;
    environment.bitsPerColor = 8;
    environment.outputColorSpace = 0;
    environment.hdr = false;
    return environment;
}

pbcapturenormalize::CaptureConfig MakeCaptureConfig(const std::uint32_t slotCount, const std::uint32_t maximumFrameAgeMilliseconds,
    const std::int32_t width = 1920, const std::int32_t height = 1080)
{
    pbcapturenormalize::CaptureConfig config;
    config.region.physicalRect = {0, 0, width, height};
    config.region.monitorPhysicalRect = config.region.physicalRect;
    config.roiTextureCount = slotCount;
    config.pixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    config.maximumFrameAgeMilliseconds = maximumFrameAgeMilliseconds;
    return config;
}

std::vector<std::byte> EncodeTransportPixels(const std::uint64_t profileId, const std::span<const std::byte> record,
    const std::span<const std::byte> data)
{
    std::vector<std::byte> pixels(pbmodulation::kLocalDesktopFrameBgraBytes);
    if (profileId == pbmodulation::kShapeChromaProfileId)
    {
        REQUIRE(pbmodulation::EncodeShapeChromaFrame(record, data, pixels));
    }
    else if (profileId == pbmodulation::kRemoteVisualProfileId)
    {
        REQUIRE(pbmodulation::EncodeRemoteVisualFrame(record, data, pixels));
    }
    else
    {
        REQUIRE(pbmodulation::EncodeDesktopLevelsFrame(record, data, pixels));
    }
    return pixels;
}

void CopyBgraRegion(const std::span<const std::byte> source, const std::span<std::byte> destination,
    const std::uint32_t left, const std::uint32_t top, const std::uint32_t width, const std::uint32_t height)
{
    REQUIRE(source.size() == pbmodulation::kLocalDesktopFrameBgraBytes);
    REQUIRE(destination.size() == source.size());
    REQUIRE(left <= 1920);
    REQUIRE(top <= 1080);
    REQUIRE(width <= 1920 - left);
    REQUIRE(height <= 1080 - top);
    for (std::uint32_t row = 0; row < height; row++)
    {
        const std::size_t offset = (static_cast<std::size_t>(top + row) * 1920 + left) * 4;
        std::copy_n(source.begin() + offset, static_cast<std::size_t>(width) * 4, destination.begin() + offset);
    }
}

void CorruptBgraBlockPerimeter(const std::span<std::byte> pixels, const pbmodulation::LocalDesktopRegion region,
    const std::uint32_t inset)
{
    REQUIRE(region.x + region.width <= pbmodulation::kLocalDesktopCanvasWidth);
    REQUIRE(region.y + region.height <= pbmodulation::kLocalDesktopCanvasHeight);
    REQUIRE(inset * 2 < region.width);
    REQUIRE(inset * 2 < region.height);
    for (std::uint32_t row = 0; row < region.height; row++)
    {
        for (std::uint32_t column = 0; column < region.width; column++)
        {
            if (row >= inset && row < region.height - inset && column >= inset && column < region.width - inset)
            {
                continue;
            }
            const std::uint8_t level = ((row + column) & 1) == 0 ? 0 : 255;
            const std::size_t offset = (static_cast<std::size_t>(region.y + row) *
                pbmodulation::kLocalDesktopCanvasWidth + region.x + column) * 4;
            pixels[offset] = static_cast<std::byte>(level);
            pixels[offset + 1] = static_cast<std::byte>(level);
            pixels[offset + 2] = static_cast<std::byte>(level);
            pixels[offset + 3] = std::byte{255};
        }
    }
}

void AddLumaNeutralChromaNoise(const std::span<std::byte> pixels, const pbmodulation::LocalDesktopRegion region,
    const std::uint32_t inset)
{
    REQUIRE(region.x + region.width <= pbmodulation::kLocalDesktopCanvasWidth);
    REQUIRE(region.y + region.height <= pbmodulation::kLocalDesktopCanvasHeight);
    REQUIRE(inset * 2 < region.width);
    REQUIRE(inset * 2 < region.height);
    for (std::uint32_t row = inset; row < region.height - inset; row++)
    {
        for (std::uint32_t column = inset; column < region.width - inset; column++)
        {
            const std::size_t offset = (static_cast<std::size_t>(region.y + row) *
                pbmodulation::kLocalDesktopCanvasWidth + region.x + column) * 4;
            const int level = std::to_integer<std::uint8_t>(pixels[offset + 1]);
            const int phase = ((row + column) & 1) == 0 ? 1 : -1;
            pixels[offset] = static_cast<std::byte>(std::clamp(level + phase * 24, 1, 254));
            pixels[offset + 2] = static_cast<std::byte>(std::clamp(level - phase * 8, 1, 254));
        }
    }
}

void CopyRemoteVisualRegionTiles(const std::span<const std::byte> source, const std::span<std::byte> destination,
    const std::uint16_t regionId)
{
    for (std::uint32_t physical = 0; physical < pbmodulation::kRemoteVisualTileCount; physical++)
    {
        pbmodulation::RemoteVisualTileMapping mapping;
        pbmodulation::LocalDesktopRegion region;
        REQUIRE(pbmodulation::GetRemoteVisualTileMapping(physical, mapping));
        REQUIRE(pbmodulation::GetRemoteVisualTile(physical, region));
        if (mapping.regionId == regionId && mapping.role != pbmodulation::RemoteVisualTileRole::Unused)
        {
            CopyBgraRegion(source, destination, region.x, region.y, region.width, region.height);
        }
    }
}

pbdemodd3d11::DemodFrameResult PollUntilReady(pbdemodd3d11::Demodulator& demodulator, ID3D11DeviceContext* context,
    const pbdemodd3d11::DemodSubmission& submission)
{
    pbdemodd3d11::DemodFrameResult result;
    context->Flush();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    for (;;)
    {
        const auto poll = demodulator.Poll(context, submission, result);
        if (poll.ready)
        {
            REQUIRE(poll.status);
            return result;
        }
        REQUIRE(poll.status);
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

pbdemodd3d11::DemodFrameResult PollUnboundUntilReady(pbdemodd3d11::Demodulator& demodulator, ID3D11DeviceContext* context,
    const pbdemodd3d11::DemodSubmission& submission, const std::span<const std::byte> bootstrapRecord)
{
    pbdemodd3d11::DemodFrameResult result;
    context->Flush();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    for (;;)
    {
        const auto poll = demodulator.PollUnbound(context, submission, bootstrapRecord, result);
        if (poll.ready)
        {
            REQUIRE(poll.status);
            return result;
        }
        REQUIRE(poll.status);
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void RequireSameEvaluation(const pbdesktoplevels::FrameEvaluation& expected,
    const pbdesktoplevels::FrameEvaluation& actual)
{
    REQUIRE(actual.evaluated == expected.evaluated);
    REQUIRE(actual.paddingValid == expected.paddingValid);
    REQUIRE(actual.codewords == expected.codewords);
    REQUIRE(actual.fecFailures == expected.fecFailures);
    REQUIRE(actual.crcFailures == expected.crcFailures);
    REQUIRE(actual.identityFailures == expected.identityFailures);
    REQUIRE(actual.falseAcceptedCodewords == expected.falseAcceptedCodewords);
    REQUIRE(actual.acceptedTransportBlocks == expected.acceptedTransportBlocks);
    REQUIRE(actual.comparedCodedBits == expected.comparedCodedBits);
    REQUIRE(actual.erroneousCodedBits == expected.erroneousCodedBits);
    REQUIRE(actual.IsVerified() == expected.IsVerified());
}

struct OracleResult
{
    pbdesktoplevels::FrameEvaluation evaluation;
    std::vector<pbdesktoplevels::AcceptedTransportBlock> acceptedTransportBlocks;
};

void RequireSameGpuResult(const OracleResult& oracle, const pbdemodd3d11::DemodFrameResult& gpu,
    const pbcapturenormalize::ScreenCaptureFrame& frame, const std::uint64_t profileId)
{
    REQUIRE(gpu.visualProfileId == profileId);
    REQUIRE(gpu.metadata.domain == frame.metadata.domain);
    REQUIRE(gpu.metadata.captureObservation == frame.metadata.captureObservation);
    REQUIRE(gpu.metadata.sourceGeneration == frame.metadata.sourceGeneration);
    REQUIRE(gpu.metadata.slotGeneration == frame.metadata.slotGeneration);
    REQUIRE(gpu.metadata.roiSize.width == frame.metadata.roiSize.width);
    REQUIRE(gpu.metadata.roiSize.height == frame.metadata.roiSize.height);
    REQUIRE(gpu.metadata.pixelFormat == frame.metadata.pixelFormat);
    REQUIRE(gpu.metadata.adapterLuid.LowPart == frame.metadata.adapterLuid.LowPart);
    REQUIRE(gpu.metadata.adapterLuid.HighPart == frame.metadata.adapterLuid.HighPart);
    if (profileId == pbmodulation::kRemoteVisualProfileId)
    {
        REQUIRE(gpu.remoteMetricSummaryAvailable);
        REQUIRE(gpu.remoteMetricSamples == pbmodulation::kRemoteVisualCodedBits);
        REQUIRE(gpu.remoteZeroMagnitudeMetrics == 0);
        REQUIRE(gpu.remoteMinimumAbsoluteMetric > 0.99);
        REQUIRE(gpu.remoteMeanAbsoluteMetric > 0.99);
    }
    else
    {
        REQUIRE_FALSE(gpu.remoteMetricSummaryAvailable);
        REQUIRE(gpu.remoteMetricSamples == 0);
        REQUIRE(gpu.remoteZeroMagnitudeMetrics == 0);
    }
    INFO("profileId=" << profileId << " actual padding=" << gpu.evaluation.paddingValid <<
        " fec=" << gpu.evaluation.fecFailures << " crc=" << gpu.evaluation.crcFailures <<
        " identity=" << gpu.evaluation.identityFailures << " accepted=" << gpu.evaluation.acceptedTransportBlocks <<
        " bitErrors=" << gpu.evaluation.erroneousCodedBits);
    RequireSameEvaluation(oracle.evaluation, gpu.evaluation);
    REQUIRE(gpu.evaluation.IsVerified());
    REQUIRE(gpu.acceptedTransportBlockCount == oracle.acceptedTransportBlocks.size());
    for (std::size_t index = 0; index < oracle.acceptedTransportBlocks.size(); index++)
    {
        REQUIRE(gpu.acceptedTransportBlocks[index] == oracle.acceptedTransportBlocks[index]);
    }
}

OracleResult RunCpuOracle(const std::uint64_t profileId, const std::span<const std::byte> pixels)
{
    auto channelResult = pbdesktoplevels::ReferenceChannel::Create(pbdesktoplevels::kProcessingReservationBytes);
    REQUIRE(channelResult);
    auto channel = std::move(channelResult).Value();
    OracleResult result;
    if (profileId == pbmodulation::kShapeChromaProfileId)
    {
        const auto observation = channel.DecodeShapeChroma(MakeView(pixels));
        REQUIRE(observation.modulation.IsAccepted());
        result.evaluation = observation.evaluation;
    }
    else if (profileId == pbmodulation::kRemoteVisualProfileId)
    {
        const auto observation = channel.DecodeRemoteVisual(MakeView(pixels));
        REQUIRE(observation.modulation.IsAccepted());
        result.evaluation = observation.evaluation;
    }
    else
    {
        const auto observation = channel.Decode(MakeView(pixels));
        REQUIRE(observation.modulation.IsAccepted());
        result.evaluation = observation.evaluation;
    }
    const auto accepted = channel.GetAcceptedTransportBlocks();
    result.acceptedTransportBlocks.assign(accepted.begin(), accepted.end());
    return result;
}

void RunOracleProfile(D3DEnvironment& environment, pbdemodd3d11::Demodulator& demodulator,
    const std::uint64_t profileId, const std::uint8_t layoutVersion, const std::uint64_t sequence,
    const std::uint64_t sessionTag, const std::size_t dataBytes,
    const pbcapturenormalize::ScreenCaptureDomain& domain, const std::uint64_t observation)
{
    const auto record = MakeRecord(profileId, layoutVersion, sequence, sessionTag);
    std::vector<std::byte> logicalData(dataBytes);
    REQUIRE(pbdesktoplevels::GenerateDiagnosticData(record, logicalData));
    std::vector<std::byte> pixels(pbmodulation::kLocalDesktopFrameBgraBytes);
    if (profileId == pbmodulation::kShapeChromaProfileId)
    {
        REQUIRE(pbmodulation::EncodeShapeChromaFrame(record, logicalData, pixels));
    }
    else if (profileId == pbmodulation::kRemoteVisualProfileId)
    {
        REQUIRE(pbmodulation::EncodeRemoteVisualFrame(record, logicalData, pixels));
    }
    else
    {
        REQUIRE(pbmodulation::EncodeDesktopLevelsFrame(record, logicalData, pixels));
    }
    const auto oracle = RunCpuOracle(profileId, pixels);
    REQUIRE(oracle.evaluation.IsVerified());

    const auto texture = UploadRoiTexture(environment.device.Get(), pixels);
    const auto frame = MakeFrame(texture.Get(), environment.adapterLuid, domain, observation);
    pbdemodd3d11::DemodSubmission submission;
    REQUIRE(demodulator.Submit(frame, environment.context.Get(), record, submission));
    pbdemodd3d11::DemodFrameResult unchanged;
    unchanged.visualProfileId = 0xDEADBEEF;
    unchanged.acceptedTransportBlockCount = 17;
    const auto wrongBoundApi = demodulator.PollUnbound(environment.context.Get(), submission, record, unchanged);
    REQUIRE(wrongBoundApi.status.code == pbdemodd3d11::DemodError::InvalidSubmission);
    REQUIRE_FALSE(wrongBoundApi.ready);
    REQUIRE(unchanged.visualProfileId == 0xDEADBEEF);
    REQUIRE(unchanged.acceptedTransportBlockCount == 17);
    const auto gpu = PollUntilReady(demodulator, environment.context.Get(), submission);
    RequireSameGpuResult(oracle, gpu, frame, profileId);

    pbdemodd3d11::DemodSubmission unboundSubmission;
    REQUIRE(demodulator.SubmitUnbound(frame, environment.context.Get(), profileId, unboundSubmission));
    unchanged.visualProfileId = 0xCAFEBABE;
    unchanged.acceptedTransportBlockCount = 19;
    const auto wrongUnboundApi = demodulator.Poll(environment.context.Get(), unboundSubmission, unchanged);
    REQUIRE(wrongUnboundApi.status.code == pbdemodd3d11::DemodError::InvalidSubmission);
    REQUIRE_FALSE(wrongUnboundApi.ready);
    REQUIRE(unchanged.visualProfileId == 0xCAFEBABE);
    REQUIRE(unchanged.acceptedTransportBlockCount == 19);
    const auto unboundGpu = PollUnboundUntilReady(demodulator, environment.context.Get(), unboundSubmission, record);
    RequireSameGpuResult(oracle, unboundGpu, frame, profileId);
}

void PollCancelled(pbdemodd3d11::Demodulator& demodulator, ID3D11DeviceContext* context,
    const pbdemodd3d11::DemodSubmission& submission)
{
    pbdemodd3d11::DemodFrameResult output;
    output.visualProfileId = 0xDEADBEEF;
    context->Flush();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    for (;;)
    {
        const auto poll = demodulator.Poll(context, submission, output);
        if (poll.ready)
        {
            REQUIRE(poll.status.code == pbdemodd3d11::DemodError::Cancelled);
            REQUIRE(output.visualProfileId == 0xDEADBEEF);
            return;
        }
        REQUIRE(poll.status);
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

struct RemoteVisualLowFpsOracle
{
    pbmodulation::LocalDesktopGeometry geometry;
    pbdesktoplevels::FrameEvaluation evaluation;
    std::vector<pbdesktoplevels::AcceptedTransportBlock> acceptedTransportBlocks;
    std::uint32_t freshnessRegions = 0;
    std::uint32_t staleRegions = 0;
    std::uint32_t freshnessTagMismatches = 0;
    std::uint32_t freshnessTagErasures = 0;
    std::uint32_t erasedDataMetrics = 0;
    std::uint32_t unreliableSymbols = 0;
};

RemoteVisualLowFpsOracle RunRemoteVisualLowFpsOracle(const pbmodulation::LumaView& view,
    const pbmodulation::RemoteVisualLowFpsDecodePolicy& policy = {})
{
    auto created = pbdesktoplevels::ReferenceChannel::Create(pbdesktoplevels::kProcessingReservationBytes);
    REQUIRE(created);
    auto channel = std::move(created.Value());
    const auto observation = channel.DecodeRemoteVisualLowFps(view, policy);
    INFO(pbmodulation::GetRemoteVisualLowFpsErasureName(observation.modulation.erasure));
    INFO(observation.modulation.bootstrap.geometry.originX);
    INFO(observation.modulation.bootstrap.geometry.originY);
    INFO(observation.modulation.bootstrap.geometry.scaleX);
    INFO(observation.modulation.bootstrap.geometry.scaleY);
    INFO(observation.modulation.staleRegions);
    REQUIRE(observation.modulation.IsAccepted());
    REQUIRE(observation.evaluation.IsVerified());
    const auto accepted = channel.GetAcceptedTransportBlocks();
    RemoteVisualLowFpsOracle result;
    result.geometry = observation.modulation.bootstrap.geometry;
    result.evaluation = observation.evaluation;
    result.acceptedTransportBlocks.assign(accepted.begin(), accepted.end());
    result.freshnessRegions = observation.modulation.freshnessRegions;
    result.staleRegions = observation.modulation.staleRegions;
    result.freshnessTagMismatches = observation.modulation.freshnessTagMismatches;
    result.freshnessTagErasures = observation.modulation.freshnessTagErasures;
    result.erasedDataMetrics = observation.modulation.erasedDataMetrics;
    result.unreliableSymbols = observation.modulation.unreliableSymbols;
    return result;
}

pbdemodd3d11::DemodFrameResult RunRemoteVisualLowFpsGpu(D3DEnvironment& environment,
    pbdemodd3d11::Demodulator& demodulator, const localdesktoptest::EncodedFixture& fixture,
    const std::span<const std::byte> bootstrapRecord, const pbmodulation::LocalDesktopGeometry& geometry,
    const pbmodulation::RemoteVisualLowFpsDecodePolicy& policy,
    const pbcapturenormalize::ScreenCaptureDomain& domain, const std::uint64_t observation)
{
    REQUIRE(fixture.format == pbmodulation::LumaPixelFormat::Bgra8);
    const auto texture = UploadBgraTexture(environment.device.Get(), fixture.pixels, fixture.width, fixture.height, fixture.pitch);
    const auto frame = MakeFrame(texture.Get(), environment.adapterLuid, domain, observation);
    pbdemodd3d11::DemodSubmission submission;
    const auto submit = demodulator.SubmitRemoteVisualLowFps(frame, environment.context.Get(), bootstrapRecord,
        geometry, policy, submission);
    INFO("Submit status=" << pbdemodd3d11::GetDemodErrorName(submit.code) <<
        " stage=" << static_cast<unsigned>(submit.stage) << " native=" << submit.nativeError);
    REQUIRE(submit);
    return PollUntilReady(demodulator, environment.context.Get(), submission);
}

void RequireSameRemoteVisualLowFpsTruth(const RemoteVisualLowFpsOracle& oracle,
    const pbdemodd3d11::DemodFrameResult& gpu)
{
    REQUIRE(gpu.visualProfileId == pbmodulation::kRemoteVisualLowFpsProfileId);
    RequireSameEvaluation(oracle.evaluation, gpu.evaluation);
    REQUIRE(gpu.evaluation.acceptedRemoteControlBlocks == oracle.evaluation.acceptedRemoteControlBlocks);
    REQUIRE(gpu.evaluation.IsVerified());
    REQUIRE(gpu.evaluation.falseAcceptedCodewords == 0);
    REQUIRE(gpu.acceptedTransportBlockCount == oracle.acceptedTransportBlocks.size());
    for (std::size_t index = 0; index < oracle.acceptedTransportBlocks.size(); index++)
    {
        REQUIRE(gpu.acceptedTransportBlocks[index] == oracle.acceptedTransportBlocks[index]);
    }
    REQUIRE(gpu.remoteMetricSummaryAvailable);
    REQUIRE(gpu.remoteMetricSamples == pbmodulation::kRemoteVisualLowFpsCodedBits);
    REQUIRE(gpu.remoteFreshnessRegions == oracle.freshnessRegions);
    REQUIRE(gpu.remoteStaleRegions == oracle.staleRegions);
    REQUIRE(gpu.remoteFreshnessTagMismatches == oracle.freshnessTagMismatches);
    REQUIRE(gpu.remoteFreshnessTagErasures == oracle.freshnessTagErasures);
    REQUIRE(gpu.remoteFreshnessErasedDataMetrics == oracle.erasedDataMetrics);
    REQUIRE(gpu.remoteUnreliableSymbols == oracle.unreliableSymbols);
}

void RequireSameRemoteVisualLowFpsTransport(const RemoteVisualLowFpsOracle& oracle,
    const pbdemodd3d11::DemodFrameResult& gpu)
{
    REQUIRE(gpu.visualProfileId == pbmodulation::kRemoteVisualLowFpsProfileId);
    REQUIRE(gpu.evaluation.evaluated);
    REQUIRE(gpu.evaluation.paddingValid == oracle.evaluation.paddingValid);
    REQUIRE(gpu.evaluation.codewords == oracle.evaluation.codewords);
    REQUIRE(gpu.evaluation.fecFailures == oracle.evaluation.fecFailures);
    REQUIRE(gpu.evaluation.crcFailures == oracle.evaluation.crcFailures);
    REQUIRE(gpu.evaluation.identityFailures == oracle.evaluation.identityFailures);
    REQUIRE(gpu.evaluation.falseAcceptedCodewords == 0);
    REQUIRE(gpu.evaluation.acceptedTransportBlocks == oracle.evaluation.acceptedTransportBlocks);
    REQUIRE(gpu.evaluation.comparedCodedBits == 0);
    REQUIRE(gpu.evaluation.erroneousCodedBits == 0);
    REQUIRE(gpu.evaluation.IsVerified());
    REQUIRE(gpu.acceptedTransportBlockCount == oracle.acceptedTransportBlocks.size());
    for (std::size_t index = 0; index < oracle.acceptedTransportBlocks.size(); index++)
    {
        REQUIRE(gpu.acceptedTransportBlocks[index] == oracle.acceptedTransportBlocks[index]);
    }
    REQUIRE(gpu.remoteMetricSummaryAvailable);
    REQUIRE(gpu.remoteMetricSamples == pbmodulation::kRemoteVisualLowFpsCodedBits);
    REQUIRE(gpu.remoteFreshnessRegions == oracle.freshnessRegions);
    REQUIRE(gpu.remoteStaleRegions == oracle.staleRegions);
    REQUIRE(gpu.remoteFreshnessTagMismatches == oracle.freshnessTagMismatches);
    REQUIRE(gpu.remoteFreshnessTagErasures == oracle.freshnessTagErasures);
    REQUIRE(gpu.remoteFreshnessErasedDataMetrics == oracle.erasedDataMetrics);
    REQUIRE(gpu.remoteUnreliableSymbols == oracle.unreliableSymbols);
}

localdesktoptest::GrayImage GaussianBlur(const localdesktoptest::GrayImage& source)
{
    constexpr std::array<std::array<unsigned, 3>, 3> weights{
        std::array<unsigned, 3>{1, 2, 1}, std::array<unsigned, 3>{2, 4, 2}, std::array<unsigned, 3>{1, 2, 1}};
    localdesktoptest::GrayImage result(source.width, source.height);
    for (std::uint32_t row = 0; row < source.height; row++)
    {
        for (std::uint32_t column = 0; column < source.width; column++)
        {
            unsigned sum = 0;
            for (std::int32_t kernelRow = -1; kernelRow <= 1; kernelRow++)
            {
                for (std::int32_t kernelColumn = -1; kernelColumn <= 1; kernelColumn++)
                {
                    sum += weights[kernelRow + 1][kernelColumn + 1] * source.Read(
                        static_cast<std::int64_t>(column) + kernelColumn, static_cast<std::int64_t>(row) + kernelRow);
                }
            }
            result.Write(column, row, static_cast<std::uint8_t>((sum + 8) / 16));
        }
    }
    return result;
}

struct AdapterFingerprint
{
    std::string backend;
    std::uint32_t adapterIndex = UINT32_MAX;
    DXGI_ADAPTER_DESC1 description{};
    bool softwareRasterizer = false;
    bool driverVersionAvailable = false;
    std::uint64_t driverVersion = 0;
};

struct HardwareAdapterCandidate
{
    ComPtr<IDXGIAdapter1> adapter;
    AdapterFingerprint fingerprint;
    HRESULT deviceCreateStatus = E_FAIL;
    D3D_FEATURE_LEVEL featureLevel{};
};

HRESULT CreateHardwareEnvironment(IDXGIAdapter1* const adapter, D3DEnvironment& output) noexcept
{
    if (adapter == nullptr)
    {
        return E_INVALIDARG;
    }
    D3DEnvironment environment;
    const std::array featureLevels{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT result = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        featureLevels.data(), static_cast<UINT>(featureLevels.size()), D3D11_SDK_VERSION,
        &environment.device, &environment.featureLevel, &environment.context);
    if (result == E_INVALIDARG)
    {
        result = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            featureLevels.data() + 1, 1, D3D11_SDK_VERSION,
            &environment.device, &environment.featureLevel, &environment.context);
    }
    if (FAILED(result))
    {
        return result;
    }
    DXGI_ADAPTER_DESC1 description{};
    result = adapter->GetDesc1(&description);
    if (FAILED(result))
    {
        return result;
    }
    environment.adapterLuid = description.AdapterLuid;
    output = std::move(environment);
    return S_OK;
}

AdapterFingerprint GetAdapterFingerprint(IDXGIAdapter1* const adapter, const std::string_view backend,
    const std::uint32_t adapterIndex, const bool softwareRasterizer)
{
    REQUIRE(adapter != nullptr);
    AdapterFingerprint fingerprint;
    fingerprint.backend = backend;
    fingerprint.adapterIndex = adapterIndex;
    fingerprint.softwareRasterizer = softwareRasterizer;
    REQUIRE(SUCCEEDED(adapter->GetDesc1(&fingerprint.description)));
    LARGE_INTEGER version{};
    if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version)))
    {
        fingerprint.driverVersionAvailable = true;
        fingerprint.driverVersion = static_cast<std::uint64_t>(version.QuadPart);
    }
    return fingerprint;
}

AdapterFingerprint GetEnvironmentFingerprint(const D3DEnvironment& environment, const std::string_view backend,
    const std::uint32_t adapterIndex, const bool softwareRasterizer)
{
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIAdapter1> adapter1;
    REQUIRE(SUCCEEDED(environment.device.As(&dxgiDevice)));
    REQUIRE(SUCCEEDED(dxgiDevice->GetAdapter(&adapter)));
    REQUIRE(SUCCEEDED(adapter.As(&adapter1)));
    return GetAdapterFingerprint(adapter1.Get(), backend, adapterIndex, softwareRasterizer);
}

std::vector<HardwareAdapterCandidate> EnumerateHardwareAdapters()
{
    ComPtr<IDXGIFactory1> factory;
    REQUIRE(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
    std::vector<HardwareAdapterCandidate> candidates;
    candidates.reserve(16);
    for (std::uint32_t adapterIndex = 0; adapterIndex <= 64; adapterIndex++)
    {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT enumerate = factory->EnumAdapters1(adapterIndex, &adapter);
        if (enumerate == DXGI_ERROR_NOT_FOUND)
        {
            break;
        }
        REQUIRE(adapterIndex < 64);
        REQUIRE(SUCCEEDED(enumerate));
        const auto fingerprint = GetAdapterFingerprint(adapter.Get(), "hardware", adapterIndex, false);
        if ((fingerprint.description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
        {
            continue;
        }
        HardwareAdapterCandidate candidate;
        candidate.adapter = adapter;
        candidate.fingerprint = fingerprint;
        D3DEnvironment probe;
        candidate.deviceCreateStatus = CreateHardwareEnvironment(adapter.Get(), probe);
        if (SUCCEEDED(candidate.deviceCreateStatus))
        {
            candidate.featureLevel = probe.featureLevel;
        }
        candidates.push_back(std::move(candidate));
    }
    REQUIRE(factory->IsCurrent() != FALSE);
    return candidates;
}

std::string WideToUtf8(const std::wstring_view value)
{
    if (value.empty())
    {
        return {};
    }
    REQUIRE(value.size() <= static_cast<std::size_t>(INT_MAX));
    const int length = static_cast<int>(value.size());
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length, nullptr, 0, nullptr, nullptr);
    REQUIRE(required > 0);
    std::string output(static_cast<std::size_t>(required), '\0');
    REQUIRE(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length,
        output.data(), required, nullptr, nullptr) == required);
    return output;
}

std::string JsonEscape(const std::string_view value)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string output;
    output.reserve(value.size() + 16);
    for (const unsigned char character : value)
    {
        switch (character)
        {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (character < 0x20)
            {
                output += "\\u00";
                output.push_back(hex[(character >> 4) & 0x0F]);
                output.push_back(hex[character & 0x0F]);
            }
            else
            {
                output.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    return output;
}

std::string HexBytes(const std::span<const std::byte> bytes)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string output(bytes.size() * 2, '0');
    for (std::size_t index = 0; index < bytes.size(); index++)
    {
        const auto value = std::to_integer<unsigned>(bytes[index]);
        output[index * 2] = hex[(value >> 4) & 0x0F];
        output[index * 2 + 1] = hex[value & 0x0F];
    }
    return output;
}

std::string Hex32(const std::uint32_t value)
{
    std::ostringstream output;
    output << "0x" << std::hex << std::setfill('0') << std::setw(8) << value;
    return output.str();
}

std::string AdapterJson(const AdapterFingerprint& fingerprint)
{
    const std::wstring_view wideDescription(fingerprint.description.Description);
    std::ostringstream output;
    output << "{\"backend\":\"" << JsonEscape(fingerprint.backend) << "\",\"adapterIndex\":";
    if (fingerprint.adapterIndex == UINT32_MAX)
    {
        output << "null";
    }
    else
    {
        output << fingerprint.adapterIndex;
    }
    output << ",\"description\":\"" << JsonEscape(WideToUtf8(wideDescription)) << "\""
           << ",\"softwareRasterizer\":" << (fingerprint.softwareRasterizer ? "true" : "false")
           << ",\"vendorId\":" << fingerprint.description.VendorId
           << ",\"deviceId\":" << fingerprint.description.DeviceId
           << ",\"subSystemId\":" << fingerprint.description.SubSysId
           << ",\"revision\":" << fingerprint.description.Revision
           << ",\"dedicatedVideoMemory\":" << static_cast<std::uint64_t>(fingerprint.description.DedicatedVideoMemory)
           << ",\"dedicatedSystemMemory\":" << static_cast<std::uint64_t>(fingerprint.description.DedicatedSystemMemory)
           << ",\"sharedSystemMemory\":" << static_cast<std::uint64_t>(fingerprint.description.SharedSystemMemory)
           << ",\"luidLow\":\"" << Hex32(fingerprint.description.AdapterLuid.LowPart) << "\""
           << ",\"luidHigh\":\"" << Hex32(static_cast<std::uint32_t>(fingerprint.description.AdapterLuid.HighPart)) << "\""
           << ",\"driverVersionRaw\":";
    if (fingerprint.driverVersionAvailable)
    {
        output << '\"' << fingerprint.driverVersion << '\"';
    }
    else
    {
        output << "null";
    }
    output << '}';
    return output.str();
}

std::array<std::byte, 32> AcceptedSetDigest(const std::span<const pbdesktoplevels::AcceptedTransportBlock> blocks)
{
    pbprotocol::Blake3Hasher hasher;
    for (const auto& block : blocks)
    {
        REQUIRE(block.byteCount <= block.bytes.size());
        std::array<std::byte, 8> header{};
        for (std::size_t byte = 0; byte < 4; byte++)
        {
            header[byte] = static_cast<std::byte>((block.slot >> (byte * 8)) & 0xFF);
            header[byte + 4] = static_cast<std::byte>((block.byteCount >> (byte * 8)) & 0xFF);
        }
        hasher.Update(header);
        hasher.Update(std::span(block.bytes).first(block.byteCount));
    }
    return hasher.Finalize();
}

std::string AcceptedBlocksJson(const std::span<const pbdesktoplevels::AcceptedTransportBlock> blocks)
{
    std::ostringstream output;
    output << '[';
    for (std::size_t index = 0; index < blocks.size(); index++)
    {
        if (index != 0)
        {
            output << ',';
        }
        const auto& block = blocks[index];
        REQUIRE(block.byteCount <= block.bytes.size());
        const auto digest = pbprotocol::ComputeBlake3Digest(std::span(block.bytes).first(block.byteCount));
        output << "{\"slot\":" << block.slot << ",\"byteCount\":" << block.byteCount
               << ",\"blake3\":\"" << HexBytes(digest) << "\"}";
    }
    output << ']';
    return output.str();
}

struct GpuParityScenario
{
    std::string name;
    std::array<std::byte, pbprotocol::kBootstrapRecordBytes> bootstrapRecord{};
    localdesktoptest::EncodedFixture fixture;
    RemoteVisualLowFpsOracle oracle;
};

std::vector<GpuParityScenario> MakeGpuParityScenarios()
{
    std::vector<GpuParityScenario> scenarios;
    scenarios.reserve(4);

    constexpr std::uint64_t baseSessionTag = 0xA74C35E29180DB6FULL;
    const auto baseRecord = MakeRecord(pbmodulation::kRemoteVisualLowFpsProfileId,
        pbmodulation::kRemoteVisualLowFpsLayoutVersion, 401, baseSessionTag);
    std::vector<std::byte> baseData(pbmodulation::kRemoteVisualLowFpsDataBytes);
    REQUIRE(pbdesktoplevels::GenerateDiagnosticData(baseRecord, baseData));
    std::vector<std::byte> baseRaster(pbmodulation::kLocalDesktopFrameBgraBytes);
    REQUIRE(pbmodulation::EncodeRemoteVisualLowFpsFrame(baseRecord, baseData, baseRaster));
    const auto original = localdesktoptest::GrayFromGolden(baseRaster);

    auto exactFixture = localdesktoptest::ConvertFormat(original, pbmodulation::LumaPixelFormat::Bgra8);
    auto exactOracle = RunRemoteVisualLowFpsOracle(exactFixture.View());
    scenarios.push_back({"exact", baseRecord, std::move(exactFixture), std::move(exactOracle)});

    const auto scaled = localdesktoptest::Resample(original, 1.259375, 1.2592592592592593, 11.25, 13.5,
        localdesktoptest::FixtureFilter::Area);
    auto scaledFixture = localdesktoptest::ConvertFormat(scaled, pbmodulation::LumaPixelFormat::Bgra8);
    auto scaledOracle = RunRemoteVisualLowFpsOracle(scaledFixture.View());
    REQUIRE(std::abs(scaledOracle.geometry.scaleX - 1.259375) < 0.01);
    REQUIRE(std::abs(scaledOracle.geometry.scaleY - 1.2592592592592593) < 0.01);
    scenarios.push_back({"scale-area", baseRecord, std::move(scaledFixture), std::move(scaledOracle)});

    const auto blurred = GaussianBlur(original);
    auto blurredFixture = localdesktoptest::ConvertFormat(blurred, pbmodulation::LumaPixelFormat::Bgra8);
    auto blurredOracle = RunRemoteVisualLowFpsOracle(blurredFixture.View());
    scenarios.push_back({"blur-gaussian-3x3", baseRecord, std::move(blurredFixture), std::move(blurredOracle)});

    constexpr std::uint64_t staleSessionTag = 0xBC913E4075A26D8FULL;
    constexpr std::uint64_t previousSequence = 510;
    constexpr std::uint64_t currentSequence = 511;
    const auto previousRecord = MakeRecord(pbmodulation::kRemoteVisualLowFpsProfileId,
        pbmodulation::kRemoteVisualLowFpsLayoutVersion, previousSequence, staleSessionTag);
    const auto currentRecord = MakeRecord(pbmodulation::kRemoteVisualLowFpsProfileId,
        pbmodulation::kRemoteVisualLowFpsLayoutVersion, currentSequence, staleSessionTag);
    std::vector<std::byte> previousData(pbmodulation::kRemoteVisualLowFpsDataBytes);
    std::vector<std::byte> currentData(pbmodulation::kRemoteVisualLowFpsDataBytes);
    REQUIRE(pbdesktoplevels::GenerateDiagnosticData(previousRecord, previousData));
    REQUIRE(pbdesktoplevels::GenerateDiagnosticData(currentRecord, currentData));
    std::vector<std::byte> previousRaster(pbmodulation::kLocalDesktopFrameBgraBytes);
    std::vector<std::byte> currentRaster(pbmodulation::kLocalDesktopFrameBgraBytes);
    REQUIRE(pbmodulation::EncodeRemoteVisualLowFpsFrame(previousRecord, previousData, previousRaster));
    REQUIRE(pbmodulation::EncodeRemoteVisualLowFpsFrame(currentRecord, currentData, currentRaster));
    std::uint16_t staleRegion = pbmodulation::kRemoteVisualFreshnessRegionCount;
    for (std::uint32_t physical = 0; physical < pbmodulation::kRemoteVisualTileCount; physical++)
    {
        pbmodulation::RemoteVisualTileMapping mapping;
        REQUIRE(pbmodulation::GetRemoteVisualTileMapping(physical, mapping));
        if (mapping.role != pbmodulation::RemoteVisualTileRole::FreshnessTag)
        {
            continue;
        }
        bool previousBit = false;
        bool currentBit = false;
        REQUIRE(pbmodulation::GetRemoteVisualFreshnessBit(staleSessionTag, previousSequence, physical, previousBit));
        REQUIRE(pbmodulation::GetRemoteVisualFreshnessBit(staleSessionTag, currentSequence, physical, currentBit));
        if (previousBit != currentBit)
        {
            staleRegion = mapping.regionId;
            break;
        }
    }
    REQUIRE(staleRegion < pbmodulation::kRemoteVisualFreshnessRegionCount);
    CopyRemoteVisualRegionTiles(previousRaster, currentRaster, staleRegion);
    const auto staleGray = localdesktoptest::GrayFromGolden(currentRaster);
    auto staleFixture = localdesktoptest::ConvertFormat(staleGray, pbmodulation::LumaPixelFormat::Bgra8);
    auto staleOracle = RunRemoteVisualLowFpsOracle(staleFixture.View());
    REQUIRE(staleOracle.staleRegions == 1);
    REQUIRE(staleOracle.erasedDataMetrics > 0);
    scenarios.push_back({"stale-region", currentRecord, std::move(staleFixture), std::move(staleOracle)});

    REQUIRE(scenarios.size() == 4);
    for (const auto& scenario : scenarios)
    {
        REQUIRE(scenario.oracle.acceptedTransportBlocks.size() == pbmodulation::kRemoteVisualLowFpsCodewords);
    }
    return scenarios;
}

void PrintAdapterAvailability(const AdapterFingerprint& fingerprint, const HRESULT createStatus,
    const D3D_FEATURE_LEVEL featureLevel)
{
    std::ostringstream output;
    output << "PB_GPU_PARITY_JSON={\"schema\":\"PixelBridge.RemoteVisualGpuParity.AdapterAvailability.1\""
           << ",\"adapter\":" << AdapterJson(fingerprint)
           << ",\"available\":" << (SUCCEEDED(createStatus) ? "true" : "false")
           << ",\"deviceCreateHresult\":\"" << Hex32(static_cast<std::uint32_t>(createStatus)) << "\""
           << ",\"featureLevel\":";
    if (SUCCEEDED(createStatus))
    {
        output << static_cast<std::uint32_t>(featureLevel);
    }
    else
    {
        output << "null";
    }
    output << '}';
    std::cout << output.str() << '\n';
}

void PrintGpuParityResult(const AdapterFingerprint& fingerprint, const D3DEnvironment& environment,
    const std::uint32_t deviceGeneration, const GpuParityScenario& scenario,
    const pbdemodd3d11::DemodFrameResult& gpu)
{
    const auto parsed = pbprotocol::ParseBootstrapRecord(scenario.bootstrapRecord);
    REQUIRE(parsed);
    const auto gpuBlocks = std::span(gpu.acceptedTransportBlocks).first(gpu.acceptedTransportBlockCount);
    const auto cpuBlocks = std::span(scenario.oracle.acceptedTransportBlocks);
    const auto bootstrapDigest = pbprotocol::ComputeBlake3Digest(scenario.bootstrapRecord);
    const auto cpuAcceptedDigest = AcceptedSetDigest(cpuBlocks);
    const auto gpuAcceptedDigest = AcceptedSetDigest(gpuBlocks);
    std::ostringstream output;
    output << std::setprecision(17)
           << "PB_GPU_PARITY_JSON={\"schema\":\"PixelBridge.RemoteVisualGpuParity.Result.1\""
           << ",\"adapter\":" << AdapterJson(fingerprint)
           << ",\"featureLevel\":" << static_cast<std::uint32_t>(environment.featureLevel)
           << ",\"deviceGeneration\":" << deviceGeneration
           << ",\"scenario\":\"" << JsonEscape(scenario.name) << "\""
           << ",\"bootstrapBlake3\":\"" << HexBytes(bootstrapDigest) << "\""
           << ",\"sessionTag\":\"0x" << std::hex << std::setfill('0') << std::setw(16)
           << parsed.Value().sessionTag.value << std::dec << "\""
           << ",\"frameSequence\":" << parsed.Value().frameSequence
           << ",\"geometry\":{\"originX\":" << scenario.oracle.geometry.originX
           << ",\"originY\":" << scenario.oracle.geometry.originY
           << ",\"scaleX\":" << scenario.oracle.geometry.scaleX
           << ",\"scaleY\":" << scenario.oracle.geometry.scaleY
           << ",\"markerResidualPixels\":" << scenario.oracle.geometry.markerResidualPixels << '}'
           << ",\"evaluation\":{\"evaluated\":" << (gpu.evaluation.evaluated ? "true" : "false")
           << ",\"paddingValid\":" << (gpu.evaluation.paddingValid ? "true" : "false")
           << ",\"codewords\":" << gpu.evaluation.codewords
           << ",\"fecFailures\":" << gpu.evaluation.fecFailures
           << ",\"crcFailures\":" << gpu.evaluation.crcFailures
           << ",\"identityFailures\":" << gpu.evaluation.identityFailures
            << ",\"falseAcceptedCodewords\":" << gpu.evaluation.falseAcceptedCodewords
            << ",\"acceptedTransportBlocks\":" << gpu.evaluation.acceptedTransportBlocks
            << ",\"acceptedRemoteControlBlocks\":" << gpu.evaluation.acceptedRemoteControlBlocks
            << ",\"iterationsTotal\":" << gpu.evaluation.iterationsTotal
            << ",\"iterationsMaximum\":" << gpu.evaluation.iterationsMaximum
            << ",\"comparedCodedBits\":" << gpu.evaluation.comparedCodedBits
            << ",\"erroneousCodedBits\":" << gpu.evaluation.erroneousCodedBits << '}'
           << ",\"acceptedManifest\":{\"cpuSetBlake3\":\"" << HexBytes(cpuAcceptedDigest)
           << "\",\"gpuSetBlake3\":\"" << HexBytes(gpuAcceptedDigest)
           << "\",\"blocks\":" << AcceptedBlocksJson(gpuBlocks) << '}'
           << ",\"metrics\":{\"samples\":" << gpu.remoteMetricSamples
           << ",\"zeroMagnitude\":" << gpu.remoteZeroMagnitudeMetrics
           << ",\"minimumAbsolute\":" << gpu.remoteMinimumAbsoluteMetric
           << ",\"meanAbsolute\":" << gpu.remoteMeanAbsoluteMetric
           << ",\"unreliableSymbols\":" << gpu.remoteUnreliableSymbols
           << ",\"freshnessRegions\":" << gpu.remoteFreshnessRegions
           << ",\"staleRegions\":" << gpu.remoteStaleRegions
           << ",\"freshnessTagMismatches\":" << gpu.remoteFreshnessTagMismatches
           << ",\"freshnessTagErasures\":" << gpu.remoteFreshnessTagErasures
           << ",\"freshnessErasedDataMetrics\":" << gpu.remoteFreshnessErasedDataMetrics << '}'
           << ",\"readbackBytes\":" << gpu.metricReadbackBytes
           << ",\"gpuTimingValid\":" << (gpu.gpuTimingValid ? "true" : "false")
           << ",\"gpuTime100ns\":" << gpu.gpuTime100ns << '}';
    std::cout << output.str() << '\n';
}

std::vector<std::array<std::byte, 32>> RunGpuParityCorpus(D3DEnvironment& environment,
    const AdapterFingerprint& fingerprint, const std::uint32_t deviceGeneration,
    const std::span<const GpuParityScenario> scenarios)
{
    REQUIRE_FALSE(scenarios.empty());
    REQUIRE(environment.featureLevel >= D3D_FEATURE_LEVEL_11_0);
    REQUIRE(environment.adapterLuid.LowPart == fingerprint.description.AdapterLuid.LowPart);
    REQUIRE(environment.adapterLuid.HighPart == fingerprint.description.AdapterLuid.HighPart);
    std::unique_ptr<pbdemodd3d11::Demodulator> demodulator;
    REQUIRE(pbdemodd3d11::Demodulator::Create(environment.device.Get(), {}, demodulator));
    const auto initialSnapshot = demodulator->GetSnapshot();
    REQUIRE(initialSnapshot.adapterLuid.LowPart == environment.adapterLuid.LowPart);
    REQUIRE(initialSnapshot.adapterLuid.HighPart == environment.adapterLuid.HighPart);

    pbcapturenormalize::ScreenCaptureDomain domain;
    domain.sourceId[0] = std::byte{0x7B};
    domain.sourceId[1] = static_cast<std::byte>(fingerprint.adapterIndex & 0xFF);
    domain.sourceId[2] = static_cast<std::byte>(deviceGeneration & 0xFF);
    domain.captureEpoch = 1000 + deviceGeneration;
    const auto wrongTexture = UploadBgraTexture(environment.device.Get(), scenarios.front().fixture.pixels,
        scenarios.front().fixture.width, scenarios.front().fixture.height, scenarios.front().fixture.pitch);
    LUID wrongLuid = environment.adapterLuid;
    wrongLuid.LowPart ^= 1u;
    const auto wrongFrame = MakeFrame(wrongTexture.Get(), wrongLuid, domain, 9000);
    pbdemodd3d11::DemodSubmission unchanged{17, 23, domain, 29};
    const auto savedSubmission = unchanged;
    const auto mismatch = demodulator->SubmitRemoteVisualLowFps(wrongFrame, environment.context.Get(),
        scenarios.front().bootstrapRecord, scenarios.front().oracle.geometry, {}, unchanged);
    REQUIRE(mismatch.code == pbdemodd3d11::DemodError::AdapterMismatch);
    REQUIRE(mismatch.stage == pbdemodd3d11::DemodStage::Submission);
    REQUIRE(unchanged == savedSubmission);
    REQUIRE(demodulator->GetSnapshot().submittedFrames == 0);

    std::vector<std::array<std::byte, 32>> acceptedDigests;
    acceptedDigests.reserve(scenarios.size());
    std::uint64_t expectedReadbackBytes = 0;
    for (std::size_t index = 0; index < scenarios.size(); index++)
    {
        const auto& scenario = scenarios[index];
        const auto gpu = RunRemoteVisualLowFpsGpu(environment, *demodulator, scenario.fixture,
            scenario.bootstrapRecord, scenario.oracle.geometry, {}, domain, index + 1);
        RequireSameRemoteVisualLowFpsTruth(scenario.oracle, gpu);
        const auto gpuBlocks = std::span(gpu.acceptedTransportBlocks).first(gpu.acceptedTransportBlockCount);
        const auto cpuBlocks = std::span(scenario.oracle.acceptedTransportBlocks);
        const auto gpuDigest = AcceptedSetDigest(gpuBlocks);
        REQUIRE(gpuDigest == AcceptedSetDigest(cpuBlocks));
        acceptedDigests.push_back(gpuDigest);
        REQUIRE(expectedReadbackBytes <= std::numeric_limits<std::uint64_t>::max() - gpu.metricReadbackBytes);
        expectedReadbackBytes += gpu.metricReadbackBytes;
        PrintGpuParityResult(fingerprint, environment, deviceGeneration, scenario, gpu);
    }
    const auto snapshot = demodulator->GetSnapshot();
    REQUIRE(snapshot.submittedFrames == scenarios.size());
    REQUIRE(snapshot.completedFrames == scenarios.size());
    REQUIRE(snapshot.pendingFrames == 0);
    REQUIRE(snapshot.failedFrames == 0);
    REQUIRE(snapshot.cancelledFrames == 0);
    REQUIRE(snapshot.metricReadbackBytes == expectedReadbackBytes);
    REQUIRE(snapshot.rawPixelReadbackBytes == 0);
    REQUIRE(snapshot.gpuTimingSamples + snapshot.gpuTimingUnavailable == snapshot.completedFrames);
    const HRESULT deviceRemoved = environment.device->GetDeviceRemovedReason();
    REQUIRE(deviceRemoved == S_OK);
    REQUIRE(demodulator->Shutdown(environment.context.Get()));
    std::cout << "PB_GPU_PARITY_JSON={\"schema\":\"PixelBridge.RemoteVisualGpuParity.Run.1\""
              << ",\"adapter\":" << AdapterJson(fingerprint)
              << ",\"deviceGeneration\":" << deviceGeneration
              << ",\"scenarios\":" << scenarios.size()
              << ",\"wrongAdapterLuidRejected\":true"
              << ",\"submittedFrames\":" << snapshot.submittedFrames
              << ",\"completedFrames\":" << snapshot.completedFrames
              << ",\"failedFrames\":" << snapshot.failedFrames
              << ",\"cancelledFrames\":" << snapshot.cancelledFrames
              << ",\"metricReadbackBytes\":" << snapshot.metricReadbackBytes
              << ",\"rawPixelReadbackBytes\":" << snapshot.rawPixelReadbackBytes
              << ",\"gpuTimingSamples\":" << snapshot.gpuTimingSamples
              << ",\"gpuTimingUnavailable\":" << snapshot.gpuTimingUnavailable
              << ",\"deviceRemovedHresult\":\"" << Hex32(static_cast<std::uint32_t>(deviceRemoved)) << "\""
              << ",\"shutdownCompleted\":true}\n";
    return acceptedDigests;
}

} // namespace

TEST_CASE("RemoteVisual LF4 accepted Transport truth is identical across WARP and every available hardware adapter",
    "[.gpu-parity]")
{
    const auto scenarios = MakeGpuParityScenarios();
    std::vector<std::array<std::byte, 32>> warpBaseline;
    {
        auto environment = CreateWarpEnvironment();
        const auto fingerprint = GetEnvironmentFingerprint(environment, "warp", UINT32_MAX, true);
        PrintAdapterAvailability(fingerprint, S_OK, environment.featureLevel);
        warpBaseline = RunGpuParityCorpus(environment, fingerprint, 0, scenarios);
    }
    {
        auto environment = CreateWarpEnvironment();
        const auto fingerprint = GetEnvironmentFingerprint(environment, "warp", UINT32_MAX, true);
        PrintAdapterAvailability(fingerprint, S_OK, environment.featureLevel);
        const auto recreated = RunGpuParityCorpus(environment, fingerprint, 1, std::span(scenarios).first(1));
        REQUIRE(recreated.front() == warpBaseline.front());
    }

    auto candidates = EnumerateHardwareAdapters();
    std::uint32_t availableHardwareAdapters = 0;
    bool nvidiaAvailable = false;
    bool amdAvailable = false;
    for (auto& candidate : candidates)
    {
        PrintAdapterAvailability(candidate.fingerprint, candidate.deviceCreateStatus, candidate.featureLevel);
        if (FAILED(candidate.deviceCreateStatus))
        {
            continue;
        }
        availableHardwareAdapters++;
        D3DEnvironment environment;
        REQUIRE(SUCCEEDED(CreateHardwareEnvironment(candidate.adapter.Get(), environment)));
        const auto fingerprint = GetEnvironmentFingerprint(environment, "hardware", candidate.fingerprint.adapterIndex, false);
        REQUIRE(fingerprint.description.VendorId == candidate.fingerprint.description.VendorId);
        REQUIRE(fingerprint.description.DeviceId == candidate.fingerprint.description.DeviceId);
        REQUIRE(fingerprint.description.SubSysId == candidate.fingerprint.description.SubSysId);
        REQUIRE(fingerprint.description.Revision == candidate.fingerprint.description.Revision);
        REQUIRE(fingerprint.description.AdapterLuid.LowPart == candidate.fingerprint.description.AdapterLuid.LowPart);
        REQUIRE(fingerprint.description.AdapterLuid.HighPart == candidate.fingerprint.description.AdapterLuid.HighPart);
        const auto hardwareResult = RunGpuParityCorpus(environment, fingerprint, 0, scenarios);
        REQUIRE(hardwareResult == warpBaseline);
        environment = {};

        D3DEnvironment recreatedEnvironment;
        REQUIRE(SUCCEEDED(CreateHardwareEnvironment(candidate.adapter.Get(), recreatedEnvironment)));
        const auto recreatedFingerprint = GetEnvironmentFingerprint(recreatedEnvironment, "hardware",
            candidate.fingerprint.adapterIndex, false);
        REQUIRE(recreatedFingerprint.description.AdapterLuid.LowPart == fingerprint.description.AdapterLuid.LowPart);
        REQUIRE(recreatedFingerprint.description.AdapterLuid.HighPart == fingerprint.description.AdapterLuid.HighPart);
        const auto recreated = RunGpuParityCorpus(recreatedEnvironment, recreatedFingerprint, 1, std::span(scenarios).first(1));
        REQUIRE(recreated.front() == warpBaseline.front());

        nvidiaAvailable = nvidiaAvailable || fingerprint.description.VendorId == 0x10DE;
        amdAvailable = amdAvailable || fingerprint.description.VendorId == 0x1002;
    }

    std::cout << "PB_GPU_PARITY_JSON={\"schema\":\"PixelBridge.RemoteVisualGpuParity.Summary.1\""
              << ",\"corpusScenarios\":" << scenarios.size()
              << ",\"hardwareAdaptersEnumerated\":" << candidates.size()
              << ",\"hardwareAdaptersAvailable\":" << availableHardwareAdapters
              << ",\"nvidia\":\"" << (nvidiaAvailable ? "pass" : "unavailable") << "\""
              << ",\"amd\":\"" << (amdAvailable ? "pass" : "unavailable") << "\""
              << ",\"allAvailableAdaptersPassed\":true}\n";
    std::cout.flush();
}

TEST_CASE("D3D11 compute demod agrees with the CPU oracle on every accepted Transport block",
    "[demod][d3d11][warp][oracle][fec]")
{
    auto environment = CreateWarpEnvironment();
    std::unique_ptr<pbdemodd3d11::Demodulator> demodulator;
    const auto createStatus = pbdemodd3d11::Demodulator::Create(environment.device.Get(), {}, demodulator);
    INFO("Create status=" << pbdemodd3d11::GetDemodErrorName(createStatus.code) <<
        " stage=" << static_cast<unsigned>(createStatus.stage) << " native=" << createStatus.nativeError);
    REQUIRE(createStatus);
    REQUIRE(demodulator != nullptr);

    pbcapturenormalize::ScreenCaptureDomain domain;
    domain.sourceId[0] = std::byte{0x5A};
    domain.captureEpoch = 7;
    RunOracleProfile(environment, *demodulator, pbmodulation::kShapeChromaProfileId,
        pbmodulation::kShapeChromaLayoutVersion, 7, 0x1020304050607080ULL, pbmodulation::kShapeChromaDataBytes,
        domain, 1);
    RunOracleProfile(environment, *demodulator, pbmodulation::kDesktopLevels4ProfileId,
        pbmodulation::kDesktopLevelsLayoutVersion, 5, 0x1122334455667788ULL, 21672, domain, 2);
    RunOracleProfile(environment, *demodulator, pbmodulation::kDesktopLevels2ProfileId,
        pbmodulation::kDesktopLevelsLayoutVersion, 3, 0x8877665544332211ULL, 86688, domain, 3);
    RunOracleProfile(environment, *demodulator, pbmodulation::kRemoteVisualProfileId,
        pbmodulation::kRemoteVisualLayoutVersion, 13, 0xA1B2C3D4E5F60718ULL,
        pbmodulation::kRemoteVisualDataBytes, domain, 4);

    const auto snapshot = demodulator->GetSnapshot();
    REQUIRE(snapshot.submittedFrames == 8);
    REQUIRE(snapshot.completedFrames == 8);
    REQUIRE(snapshot.pendingFrames == 0);
    REQUIRE(snapshot.metricReadbackBytes > 0);
    REQUIRE(snapshot.rawPixelReadbackBytes == 0);
    REQUIRE(demodulator->Shutdown(environment.context.Get()));
}

TEST_CASE("RemoteVisual LF4 D3D11 demod produces compact metrics and the same accepted Transport blocks as CPU",
    "[demod][d3d11][warp][remote-visual][low-fps][fec][transport]")
{
    constexpr std::uint64_t sessionTag = 0x6D3C2B1A90785634ULL;
    const auto record = MakeRecord(pbmodulation::kRemoteVisualLowFpsProfileId,
        pbmodulation::kRemoteVisualLowFpsLayoutVersion, 117, sessionTag);
    std::vector<std::byte> logicalData(pbmodulation::kRemoteVisualLowFpsDataBytes);
    REQUIRE(pbdesktoplevels::GenerateDiagnosticData(record, logicalData));
    std::vector<std::byte> raster(pbmodulation::kLocalDesktopFrameBgraBytes);
    REQUIRE(pbmodulation::EncodeRemoteVisualLowFpsFrame(record, logicalData, raster));
    const auto gray = localdesktoptest::GrayFromGolden(raster);
    const auto fixture = localdesktoptest::ConvertFormat(gray, pbmodulation::LumaPixelFormat::Bgra8);
    const auto oracle = RunRemoteVisualLowFpsOracle(fixture.View());
    REQUIRE(oracle.acceptedTransportBlocks.size() == pbmodulation::kRemoteVisualLowFpsCodewords);

    auto environment = CreateWarpEnvironment();
    std::unique_ptr<pbdemodd3d11::Demodulator> demodulator;
    REQUIRE(pbdemodd3d11::Demodulator::Create(environment.device.Get(), {}, demodulator));
    pbcapturenormalize::ScreenCaptureDomain domain;
    domain.sourceId[0] = std::byte{0x4C};
    domain.captureEpoch = 10;
    // A real exact-ROI WGC capture can fit a boundary a few ten-thousandths of a
    // pixel outside the frame while retaining essentially zero marker residual.
    // The GPU must use the same bounded inward snap as the CPU reference path.
    const pbmodulation::LocalDesktopGeometry exactCanvasFit{-7.4024239893333288e-07,
        -1.4539924904966028e-05, 1.0000000007710859, 1.0000002254867735, 0.0027374946912459563};
    const auto gpu = RunRemoteVisualLowFpsGpu(environment, *demodulator, fixture, record, exactCanvasFit, {}, domain, 1);
    RequireSameRemoteVisualLowFpsTruth(oracle, gpu);
    REQUIRE(gpu.remoteZeroMagnitudeMetrics == 0);
    REQUIRE(gpu.remoteMinimumAbsoluteMetric > 7.9);
    REQUIRE(gpu.metricReadbackBytes == static_cast<std::uint64_t>(pbmodulation::kRemoteVisualLowFpsCodedBits) * sizeof(float) +
        16 * sizeof(float) * 4 + (pbmodulation::kRemoteVisualFreshnessRegionCount + 1) * sizeof(std::uint32_t) * 4);
    REQUIRE(gpu.metricReadbackBytes * 16 < static_cast<std::uint64_t>(fixture.width) * fixture.height * 4);
    const auto snapshot = demodulator->GetSnapshot();
    REQUIRE(snapshot.submittedFrames == 1);
    REQUIRE(snapshot.completedFrames == 1);
    REQUIRE(snapshot.pendingFrames == 0);
    REQUIRE(snapshot.rawPixelReadbackBytes == 0);
    REQUIRE(snapshot.metricReadbackBytes == gpu.metricReadbackBytes);
    REQUIRE(demodulator->Shutdown(environment.context.Get()));
}

TEST_CASE("RemoteVisual LF4 D3D11 continuous geometry survives independent scale and blur fixtures",
    "[demod][d3d11][warp][remote-visual][low-fps][geometry][blur]")
{
    const auto record = MakeRecord(pbmodulation::kRemoteVisualLowFpsProfileId,
        pbmodulation::kRemoteVisualLowFpsLayoutVersion, 211, 0xC47A29E105D86B3FULL);
    std::vector<std::byte> logicalData(pbmodulation::kRemoteVisualLowFpsDataBytes);
    REQUIRE(pbdesktoplevels::GenerateDiagnosticData(record, logicalData));
    std::vector<std::byte> raster(pbmodulation::kLocalDesktopFrameBgraBytes);
    REQUIRE(pbmodulation::EncodeRemoteVisualLowFpsFrame(record, logicalData, raster));
    const auto original = localdesktoptest::GrayFromGolden(raster);
    const auto scaled = localdesktoptest::Resample(original, 1.259375, 1.2592592592592593, 11.25, 13.5,
        localdesktoptest::FixtureFilter::Area);
    const auto scaledFixture = localdesktoptest::ConvertFormat(scaled, pbmodulation::LumaPixelFormat::Bgra8);
    const auto scaledOracle = RunRemoteVisualLowFpsOracle(scaledFixture.View());
    REQUIRE(std::abs(scaledOracle.geometry.scaleX - 1.259375) < 0.01);
    REQUIRE(std::abs(scaledOracle.geometry.scaleY - 1.2592592592592593) < 0.01);

    const auto blurred = GaussianBlur(original);
    const auto blurredFixture = localdesktoptest::ConvertFormat(blurred, pbmodulation::LumaPixelFormat::Bgra8);
    const auto blurredOracle = RunRemoteVisualLowFpsOracle(blurredFixture.View());

    auto environment = CreateWarpEnvironment();
    std::unique_ptr<pbdemodd3d11::Demodulator> demodulator;
    REQUIRE(pbdemodd3d11::Demodulator::Create(environment.device.Get(), {}, demodulator));
    pbcapturenormalize::ScreenCaptureDomain domain;
    domain.sourceId[0] = std::byte{0x57};
    domain.captureEpoch = 12;
    const auto scaledGpu = RunRemoteVisualLowFpsGpu(environment, *demodulator, scaledFixture, record,
        scaledOracle.geometry, {}, domain, 1);
    RequireSameRemoteVisualLowFpsTruth(scaledOracle, scaledGpu);
    const auto blurredGpu = RunRemoteVisualLowFpsGpu(environment, *demodulator, blurredFixture, record,
        blurredOracle.geometry, {}, domain, 2);
    RequireSameRemoteVisualLowFpsTruth(blurredOracle, blurredGpu);

    const auto snapshot = demodulator->GetSnapshot();
    REQUIRE(snapshot.submittedFrames == 2);
    REQUIRE(snapshot.completedFrames == 2);
    REQUIRE(snapshot.pendingFrames == 0);
    REQUIRE(snapshot.rawPixelReadbackBytes == 0);
    REQUIRE(demodulator->Shutdown(environment.context.Get()));
}

TEST_CASE("RemoteVisual LF4 D3D11 erases a temporally stale region before the unchanged FEC gate",
    "[demod][d3d11][warp][remote-visual][low-fps][freshness][stale]")
{
    constexpr std::uint64_t sessionTag = 0xDEADBEEF31415926ULL;
    constexpr std::uint64_t previousSequence = 200;
    constexpr std::uint64_t currentSequence = 201;
    const auto previousRecord = MakeRecord(pbmodulation::kRemoteVisualLowFpsProfileId,
        pbmodulation::kRemoteVisualLowFpsLayoutVersion, previousSequence, sessionTag);
    const auto currentRecord = MakeRecord(pbmodulation::kRemoteVisualLowFpsProfileId,
        pbmodulation::kRemoteVisualLowFpsLayoutVersion, currentSequence, sessionTag);
    std::vector<std::byte> previousData(pbmodulation::kRemoteVisualLowFpsDataBytes);
    std::vector<std::byte> currentData(pbmodulation::kRemoteVisualLowFpsDataBytes);
    REQUIRE(pbdesktoplevels::GenerateDiagnosticData(previousRecord, previousData));
    REQUIRE(pbdesktoplevels::GenerateDiagnosticData(currentRecord, currentData));
    std::vector<std::byte> previousRaster(pbmodulation::kLocalDesktopFrameBgraBytes);
    std::vector<std::byte> currentRaster(pbmodulation::kLocalDesktopFrameBgraBytes);
    REQUIRE(pbmodulation::EncodeRemoteVisualLowFpsFrame(previousRecord, previousData, previousRaster));
    REQUIRE(pbmodulation::EncodeRemoteVisualLowFpsFrame(currentRecord, currentData, currentRaster));
    std::uint16_t staleRegion = pbmodulation::kRemoteVisualFreshnessRegionCount;
    for (std::uint32_t physical = 0; physical < pbmodulation::kRemoteVisualTileCount; physical++)
    {
        pbmodulation::RemoteVisualTileMapping mapping;
        REQUIRE(pbmodulation::GetRemoteVisualTileMapping(physical, mapping));
        if (mapping.role != pbmodulation::RemoteVisualTileRole::FreshnessTag)
        {
            continue;
        }
        bool previousBit = false;
        bool currentBit = false;
        REQUIRE(pbmodulation::GetRemoteVisualFreshnessBit(sessionTag, previousSequence, physical, previousBit));
        REQUIRE(pbmodulation::GetRemoteVisualFreshnessBit(sessionTag, currentSequence, physical, currentBit));
        if (previousBit != currentBit)
        {
            staleRegion = mapping.regionId;
            break;
        }
    }
    REQUIRE(staleRegion < pbmodulation::kRemoteVisualFreshnessRegionCount);
    CopyRemoteVisualRegionTiles(previousRaster, currentRaster, staleRegion);
    const auto gray = localdesktoptest::GrayFromGolden(currentRaster);
    const auto fixture = localdesktoptest::ConvertFormat(gray, pbmodulation::LumaPixelFormat::Bgra8);
    const auto oracle = RunRemoteVisualLowFpsOracle(fixture.View());
    REQUIRE(oracle.staleRegions == 1);
    REQUIRE(oracle.erasedDataMetrics > 0);

    auto environment = CreateWarpEnvironment();
    std::unique_ptr<pbdemodd3d11::Demodulator> demodulator;
    REQUIRE(pbdemodd3d11::Demodulator::Create(environment.device.Get(), {}, demodulator));
    pbcapturenormalize::ScreenCaptureDomain domain;
    domain.sourceId[0] = std::byte{0x63};
    domain.captureEpoch = 14;
    const auto gpu = RunRemoteVisualLowFpsGpu(environment, *demodulator, fixture, currentRecord,
        oracle.geometry, {}, domain, 1);
    RequireSameRemoteVisualLowFpsTruth(oracle, gpu);
    REQUIRE(gpu.remoteZeroMagnitudeMetrics == gpu.remoteFreshnessErasedDataMetrics);
    REQUIRE(gpu.evaluation.erroneousCodedBits > 0);
    REQUIRE(demodulator->GetSnapshot().rawPixelReadbackBytes == 0);
    REQUIRE(demodulator->Shutdown(environment.context.Get()));
}

TEST_CASE("RemoteVisual LF4 D3D11 rejects invalid geometry and bounds its epoch-scoped readback ring",
    "[demod][d3d11][warp][remote-visual][low-fps][errors][lifetime][ring]")
{
    const auto record = MakeRecord(pbmodulation::kRemoteVisualLowFpsProfileId,
        pbmodulation::kRemoteVisualLowFpsLayoutVersion, 313, 0x82D41F90C65AB37EULL);
    std::vector<std::byte> logicalData(pbmodulation::kRemoteVisualLowFpsDataBytes);
    REQUIRE(pbdesktoplevels::GenerateDiagnosticData(record, logicalData));
    std::vector<std::byte> raster(pbmodulation::kLocalDesktopFrameBgraBytes);
    REQUIRE(pbmodulation::EncodeRemoteVisualLowFpsFrame(record, logicalData, raster));
    const auto gray = localdesktoptest::GrayFromGolden(raster);
    const auto fixture = localdesktoptest::ConvertFormat(gray, pbmodulation::LumaPixelFormat::Bgra8);

    auto environment = CreateWarpEnvironment();
    pbdemodd3d11::DemodConfig config;
    config.readbackSlotCount = 2;
    std::unique_ptr<pbdemodd3d11::Demodulator> demodulator;
    REQUIRE(pbdemodd3d11::Demodulator::Create(environment.device.Get(), config, demodulator));
    const auto texture = UploadBgraTexture(environment.device.Get(), fixture.pixels, fixture.width, fixture.height, fixture.pitch);
    pbcapturenormalize::ScreenCaptureDomain domain;
    domain.sourceId[0] = std::byte{0x71};
    domain.captureEpoch = 16;
    auto frame = MakeFrame(texture.Get(), environment.adapterLuid, domain, 1);
    const pbmodulation::LocalDesktopGeometry geometry{0, 0, 1, 1, 0};
    pbdemodd3d11::DemodSubmission unchanged{17, 23, domain, 29};
    const auto savedSubmission = unchanged;

    REQUIRE(demodulator->Submit(frame, environment.context.Get(), record, unchanged).code ==
        pbdemodd3d11::DemodError::UnsupportedProfile);
    REQUIRE(unchanged == savedSubmission);
    auto invalidGeometry = geometry;
    invalidGeometry.originX = std::numeric_limits<double>::quiet_NaN();
    auto status = demodulator->SubmitRemoteVisualLowFps(frame, environment.context.Get(), record,
        invalidGeometry, {}, unchanged);
    REQUIRE(status.code == pbdemodd3d11::DemodError::InvalidBinding);
    REQUIRE(status.stage == pbdemodd3d11::DemodStage::Binding);
    REQUIRE(status.nativeError == static_cast<std::int32_t>(pbmodulation::RemoteVisualLowFpsErasure::InvalidInput));
    REQUIRE(unchanged == savedSubmission);
    invalidGeometry = geometry;
    invalidGeometry.originX = 0.5;
    status = demodulator->SubmitRemoteVisualLowFps(frame, environment.context.Get(), record,
        invalidGeometry, {}, unchanged);
    REQUIRE(status.code == pbdemodd3d11::DemodError::InvalidFrame);
    REQUIRE(status.nativeError == static_cast<std::int32_t>(pbmodulation::RemoteVisualLowFpsErasure::FrameOutOfBounds));
    REQUIRE(unchanged == savedSubmission);
    pbmodulation::RemoteVisualLowFpsDecodePolicy invalidPolicy;
    invalidPolicy.minimumSymbolMargin = std::numeric_limits<double>::quiet_NaN();
    status = demodulator->SubmitRemoteVisualLowFps(frame, environment.context.Get(), record,
        geometry, invalidPolicy, unchanged);
    REQUIRE(status.code == pbdemodd3d11::DemodError::InvalidBinding);
    REQUIRE(status.nativeError == static_cast<std::int32_t>(pbmodulation::RemoteVisualLowFpsErasure::InvalidPolicy));
    REQUIRE(unchanged == savedSubmission);
    pbmodulation::RemoteVisualLowFpsDecodePolicy insufficientWorkPolicy;
    insufficientWorkPolicy.maximumDataWorkUnits = 2'000'000;
    status = demodulator->SubmitRemoteVisualLowFps(frame, environment.context.Get(), record,
        geometry, insufficientWorkPolicy, unchanged);
    REQUIRE(status.code == pbdemodd3d11::DemodError::ResourceLimit);
    REQUIRE(status.nativeError == static_cast<std::int32_t>(pbmodulation::RemoteVisualLowFpsErasure::WorkBudgetExceeded));
    REQUIRE(unchanged == savedSubmission);

    pbdemodd3d11::DemodSubmission first;
    pbdemodd3d11::DemodSubmission second;
    REQUIRE(demodulator->SubmitRemoteVisualLowFps(frame, environment.context.Get(), record, geometry, {}, first));
    frame = MakeFrame(texture.Get(), environment.adapterLuid, domain, 2);
    REQUIRE(demodulator->SubmitRemoteVisualLowFps(frame, environment.context.Get(), record, geometry, {}, second));
    frame = MakeFrame(texture.Get(), environment.adapterLuid, domain, 3);
    unchanged = savedSubmission;
    status = demodulator->SubmitRemoteVisualLowFps(frame, environment.context.Get(), record, geometry, {}, unchanged);
    REQUIRE(status.code == pbdemodd3d11::DemodError::Busy);
    REQUIRE(unchanged == savedSubmission);
    REQUIRE(demodulator->InvalidateDomain(domain));
    PollCancelled(*demodulator, environment.context.Get(), first);
    PollCancelled(*demodulator, environment.context.Get(), second);

    const auto snapshot = demodulator->GetSnapshot();
    REQUIRE(snapshot.submittedFrames == 2);
    REQUIRE(snapshot.completedFrames == 0);
    REQUIRE(snapshot.cancelledFrames == 2);
    REQUIRE(snapshot.failedFrames == 0);
    REQUIRE(snapshot.pendingFrames == 0);
    REQUIRE(snapshot.highWater == 2);
    REQUIRE(snapshot.metricReadbackBytes == 0);
    REQUIRE(snapshot.rawPixelReadbackBytes == 0);
    REQUIRE(demodulator->Shutdown(environment.context.Get()));
}

TEST_CASE("RemoteVisual D3D11 demod samples codec-safe block interiors exactly like the CPU oracle",
    "[demod][d3d11][warp][remote-visual][ringing]")
{
    auto environment = CreateWarpEnvironment();
    std::unique_ptr<pbdemodd3d11::Demodulator> demodulator;
    REQUIRE(pbdemodd3d11::Demodulator::Create(environment.device.Get(), {}, demodulator));

    constexpr std::uint64_t sequence = 18;
    constexpr std::uint64_t sessionTag = 0x6750552D72696E67ULL;
    const auto record = MakeRecord(pbmodulation::kRemoteVisualProfileId, pbmodulation::kRemoteVisualLayoutVersion,
        sequence, sessionTag);
    std::vector<std::byte> logicalData(pbmodulation::kRemoteVisualDataBytes);
    REQUIRE(pbdesktoplevels::GenerateDiagnosticData(record, logicalData));
    auto pixels = EncodeTransportPixels(pbmodulation::kRemoteVisualProfileId, record, logicalData);
    for (std::uint32_t physical = 0; physical < pbmodulation::kRemoteVisualTileCount; physical++)
    {
        pbmodulation::LocalDesktopRegion region;
        REQUIRE(pbmodulation::GetRemoteVisualTile(physical, region));
        CorruptBgraBlockPerimeter(pixels, region, pbmodulation::kRemoteVisualTileSampleInset);
    }
    for (const auto ladder : pbmodulation::kRemoteVisualLadders)
    {
        for (std::uint32_t level = 0; level < 4; level++)
        {
            CorruptBgraBlockPerimeter(pixels, {ladder.x + level * 32, ladder.y, 32, 64},
                pbmodulation::kRemoteVisualCalibrationSampleInset);
        }
    }
    for (std::uint32_t physical = 0; physical < pbmodulation::kRemoteVisualTileCount; physical++)
    {
        pbmodulation::LocalDesktopRegion region;
        REQUIRE(pbmodulation::GetRemoteVisualTile(physical, region));
        AddLumaNeutralChromaNoise(pixels, region, pbmodulation::kRemoteVisualTileSampleInset);
    }
    for (const auto ladder : pbmodulation::kRemoteVisualLadders)
    {
        for (std::uint32_t level = 0; level < 4; level++)
        {
            AddLumaNeutralChromaNoise(pixels, {ladder.x + level * 32, ladder.y, 32, 64},
                pbmodulation::kRemoteVisualCalibrationSampleInset);
        }
    }
    const auto oracle = RunCpuOracle(pbmodulation::kRemoteVisualProfileId, pixels);
    REQUIRE(oracle.evaluation.IsVerified());

    const auto texture = UploadRoiTexture(environment.device.Get(), pixels);
    pbcapturenormalize::ScreenCaptureDomain domain;
    domain.sourceId[0] = std::byte{0x6B};
    domain.captureEpoch = 8;
    const auto frame = MakeFrame(texture.Get(), environment.adapterLuid, domain, 1);
    pbdemodd3d11::DemodSubmission submission;
    REQUIRE(demodulator->Submit(frame, environment.context.Get(), record, submission));
    const auto gpu = PollUntilReady(*demodulator, environment.context.Get(), submission);
    RequireSameGpuResult(oracle, gpu, frame, pbmodulation::kRemoteVisualProfileId);

    const auto previousRecord = MakeRecord(pbmodulation::kRemoteVisualProfileId,
        pbmodulation::kRemoteVisualLayoutVersion, sequence - 1, sessionTag);
    std::vector<std::byte> previousData(pbmodulation::kRemoteVisualDataBytes);
    REQUIRE(pbdesktoplevels::GenerateDiagnosticData(previousRecord, previousData));
    const auto previousPixels = EncodeTransportPixels(pbmodulation::kRemoteVisualProfileId, previousRecord,
        previousData);
    auto stalePixels = pixels;
    std::array<bool, pbmodulation::kRemoteVisualFreshnessRegionCount> selectedRegions{};
    std::uint32_t selectedRegionCount = 0;
    for (std::uint32_t physical = 0; physical < pbmodulation::kRemoteVisualTileCount && selectedRegionCount < 6; physical++)
    {
        pbmodulation::RemoteVisualTileMapping mapping;
        REQUIRE(pbmodulation::GetRemoteVisualTileMapping(physical, mapping));
        if (mapping.role == pbmodulation::RemoteVisualTileRole::FreshnessTag && !selectedRegions[mapping.regionId])
        {
            selectedRegions[mapping.regionId] = true;
            selectedRegionCount++;
            CopyRemoteVisualRegionTiles(previousPixels, stalePixels, mapping.regionId);
        }
    }
    REQUIRE(selectedRegionCount == 6);
    const auto staleOracle = RunCpuOracle(pbmodulation::kRemoteVisualProfileId, stalePixels);
    REQUIRE(staleOracle.evaluation.IsVerified());
    const auto staleTexture = UploadRoiTexture(environment.device.Get(), stalePixels);
    const auto staleFrame = MakeFrame(staleTexture.Get(), environment.adapterLuid, domain, 2);
    pbdemodd3d11::DemodSubmission staleSubmission;
    REQUIRE(demodulator->Submit(staleFrame, environment.context.Get(), record, staleSubmission));
    const auto staleGpu = PollUntilReady(*demodulator, environment.context.Get(), staleSubmission);
    RequireSameEvaluation(staleOracle.evaluation, staleGpu.evaluation);
    REQUIRE(staleGpu.evaluation.IsVerified());
    REQUIRE(staleGpu.remoteMetricSummaryAvailable);
    REQUIRE(staleGpu.remoteFreshnessRegions == pbmodulation::kRemoteVisualEligibleFreshnessRegions);
    REQUIRE(staleGpu.remoteStaleRegions >= 6);
    REQUIRE(staleGpu.remoteFreshnessTagMismatches > 0);
    REQUIRE(staleGpu.remoteFreshnessErasedDataMetrics > 0);
    REQUIRE(staleGpu.remoteZeroMagnitudeMetrics >= staleGpu.remoteFreshnessErasedDataMetrics);
    REQUIRE(demodulator->Shutdown(environment.context.Get()));
}

TEST_CASE("Unbound D3D11 production mode publishes arbitrary CRC-valid Transport identities without diagnostic truth",
    "[demod][d3d11][transport][receiver][warp]")
{
    auto environment = CreateWarpEnvironment();
    pbdemodd3d11::DemodConfig config;
    config.evaluationMode = pbdesktoplevels::EvaluationMode::Transport;
    std::unique_ptr<pbdemodd3d11::Demodulator> demodulator;
    REQUIRE(pbdemodd3d11::Demodulator::Create(environment.device.Get(), config, demodulator));

    constexpr std::uint64_t sessionTagValue = 0x1029384756ABCDEFULL;
    const auto record = MakeRecord(pbmodulation::kDesktopLevels4ProfileId, pbmodulation::kDesktopLevelsLayoutVersion,
        13, sessionTagValue);
    const auto data = MakeTransportData(21672, 10, pbprotocol::SessionTag{sessionTagValue});
    std::vector<std::byte> pixels(pbmodulation::kLocalDesktopFrameBgraBytes);
    REQUIRE(pbmodulation::EncodeDesktopLevelsFrame(record, data, pixels));
    const auto texture = UploadRoiTexture(environment.device.Get(), pixels);
    pbcapturenormalize::ScreenCaptureDomain domain;
    domain.sourceId[0] = std::byte{0x37};
    domain.captureEpoch = 21;
    const auto frame = MakeFrame(texture.Get(), environment.adapterLuid, domain, 1);
    pbdemodd3d11::DemodSubmission submission;
    REQUIRE(demodulator->SubmitUnbound(frame, environment.context.Get(), pbmodulation::kDesktopLevels4ProfileId, submission));
    const auto result = PollUnboundUntilReady(*demodulator, environment.context.Get(), submission, record);
    REQUIRE(result.evaluation.evaluated);
    REQUIRE(result.evaluation.IsVerified());
    REQUIRE(result.evaluation.comparedCodedBits == 0);
    REQUIRE(result.evaluation.erroneousCodedBits == 0);
    REQUIRE(result.evaluation.acceptedTransportBlocks == 10);
    REQUIRE(result.acceptedTransportBlockCount == 10);
    for (std::uint32_t slot = 0; slot < result.acceptedTransportBlockCount; slot++)
    {
        const auto& accepted = result.acceptedTransportBlocks[slot];
        REQUIRE(accepted.slot == slot);
        REQUIRE(accepted.byteCount == pbprotocol::kTransportMinimumBlockBytes + 31 + slot);
        const auto parsed = pbprotocol::ParseTransportBlock(std::span(accepted.bytes).first(accepted.byteCount));
        REQUIRE(parsed);
        REQUIRE(parsed.Value().header.sessionTag.value == sessionTagValue);
        REQUIRE(parsed.Value().header.segmentOrdinal == 700 + slot);
        REQUIRE(parsed.Value().header.outerBlockId == 900 + slot);
    }
    const auto snapshot = demodulator->GetSnapshot();
    REQUIRE(snapshot.submittedFrames == 1);
    REQUIRE(snapshot.completedFrames == 1);
    REQUIRE(snapshot.failedFrames == 0);
    REQUIRE(snapshot.pendingFrames == 0);
    REQUIRE(demodulator->Shutdown(environment.context.Get()));
}

TEST_CASE("Capture demodulator stages same-frame LF4 Bootstrap geometry before direct-texture GPU admission",
    "[demod][d3d11][capture][remote-visual][low-fps][geometry][lifetime][warp]")
{
    const auto scenarios = MakeGpuParityScenarios();
    REQUIRE(scenarios.size() >= 2);
    for (const std::size_t scenarioIndex : {std::size_t{0}, std::size_t{1}})
    {
        const auto& scenario = scenarios[scenarioIndex];
        CAPTURE(scenario.name, scenario.fixture.width, scenario.fixture.height);
        auto environment = CreateWarpEnvironment();
        pbdemodd3d11::CaptureDemodulatorConfig consumerConfig;
        consumerConfig.visualProfileId = pbmodulation::kRemoteVisualLowFpsProfileId;
        consumerConfig.slotCount = 2;
        consumerConfig.maximumFrameAgeMilliseconds = 60000;
        consumerConfig.resultQueueCapacity = 2;
        consumerConfig.maximumRoiWidth = scenario.fixture.width;
        consumerConfig.maximumRoiHeight = scenario.fixture.height;
        pbdemodd3d11::CaptureDemodulatorBudget budget;
        REQUIRE(pbdemodd3d11::CalculateCaptureDemodulatorBudget(consumerConfig, budget));
        const std::uint64_t frameBytes = static_cast<std::uint64_t>(scenario.fixture.width) * scenario.fixture.height * 4;
        CHECK(budget.bootstrapStagingBytes == frameBytes * consumerConfig.slotCount);
        CHECK(budget.referenceScratchBytes == 0);
        std::shared_ptr<pbdemodd3d11::CaptureDemodulator> consumer;
        REQUIRE(pbdemodd3d11::CaptureDemodulator::Create(consumerConfig, consumer));
        const auto captureConfig = MakeCaptureConfig(2, 60000, static_cast<std::int32_t>(scenario.fixture.width),
            static_cast<std::int32_t>(scenario.fixture.height));
        REQUIRE(consumer->ValidateConfiguration(captureConfig));
        pbcapturenormalize::ScreenCaptureDomain domain;
        domain.sourceId[0] = static_cast<std::byte>(0x70 + scenarioIndex);
        domain.captureEpoch = 40 + scenarioIndex;
        REQUIRE(consumer->DomainStarted(domain, MakeCaptureEnvironment(environment.adapterLuid,
            static_cast<std::int32_t>(scenario.fixture.width), static_cast<std::int32_t>(scenario.fixture.height)), environment.device.Get()));
        const auto texture = UploadBgraTexture(environment.device.Get(), scenario.fixture.pixels,
            scenario.fixture.width, scenario.fixture.height, scenario.fixture.pitch);
        auto frame = MakeFrame(texture.Get(), environment.adapterLuid, domain, scenarioIndex + 1);
        frame.metadata.slotIndex = 0;
        StampCurrent(frame);
        REQUIRE(consumer->Submit(frame, environment.context.Get()));
        WaitForDownstreamMarker(environment.device.Get(), environment.context.Get());
        const auto bootstrapStage = consumer->CompleteStage(frame.metadata, texture.Get(), environment.context.Get(), false);
        REQUIRE(bootstrapStage.status);
        REQUIRE(bootstrapStage.gpuWorkSubmitted);
        pbdemodd3d11::CaptureDemodulatorResult result;
        CHECK_FALSE(consumer->TakeResult(result));
        auto snapshot = consumer->GetSnapshot();
        CHECK(snapshot.pendingFrames == 1);
        CHECK(snapshot.bootstrapMapCalls == 1);
        CHECK(snapshot.bootstrapReadbackBytes == frameBytes);
        CHECK(snapshot.bootstrapAcceptedFrames == 1);
        CHECK(snapshot.stagedGpuSubmissions == 1);
        CHECK(snapshot.stagedGpuCompletions == 0);
        CHECK(snapshot.demodulator.pendingFrames == 1);

        WaitForDownstreamMarker(environment.device.Get(), environment.context.Get());
        const auto demodStage = consumer->CompleteStage(frame.metadata, texture.Get(), environment.context.Get(), false);
        REQUIRE(demodStage.status);
        CHECK_FALSE(demodStage.gpuWorkSubmitted);
        REQUIRE(consumer->TakeResult(result));
        CHECK(result.kind == pbdemodd3d11::CaptureDemodulatorResultKind::Transport);
        CHECK(result.bootstrap.IsAccepted());
        CHECK(result.bootstrap.canonical44 == scenario.bootstrapRecord);
        CHECK(result.bootstrap.geometry == scenario.oracle.geometry);
        RequireSameRemoteVisualLowFpsTransport(scenario.oracle, result.demodulation);
        CHECK(result.temporalDisposition == pbdemodd3d11::CaptureDemodulatorTemporalDisposition::Unique);
        REQUIRE(result.admittedTransportBlockCount == pbmodulation::kRemoteVisualLowFpsCodewords);
        for (std::uint32_t index = 0; index < result.admittedTransportBlockCount; index++)
        {
            CHECK(result.admittedTransportBlockIndices[index] == index);
        }
        const auto expectedGeometryStatus = scenarioIndex == 0 ? pbdemodd3d11::CaptureDemodulatorGeometryStatus::ExactCanvas :
            pbdemodd3d11::CaptureDemodulatorGeometryStatus::Letterboxed;
        CHECK(result.geometryStatus == expectedGeometryStatus);
        CHECK_FALSE(consumer->TakeResult(result));
        snapshot = consumer->GetSnapshot();
        CHECK(snapshot.pendingFrames == 0);
        CHECK(snapshot.completedFrames == 1);
        CHECK(snapshot.bootstrapMapCalls == 1);
        CHECK(snapshot.bootstrapAcceptedFrames == 1);
        CHECK(snapshot.stagedGpuSubmissions == 1);
        CHECK(snapshot.stagedGpuCompletions == 1);
        CHECK(snapshot.lastGeometryStatus == expectedGeometryStatus);
        CHECK(snapshot.exactGeometryFrames == static_cast<std::uint64_t>(scenarioIndex == 0));
        CHECK(snapshot.letterboxedGeometryFrames == static_cast<std::uint64_t>(scenarioIndex == 1));
        CHECK(snapshot.scaledGeometryFrames == 0);
        CHECK(snapshot.rejectedGeometryFrames == 0);
        CHECK(snapshot.temporalUniqueFrames == 1);
        CHECK(snapshot.temporallyAdmittedTransportBlocks == pbmodulation::kRemoteVisualLowFpsCodewords);
        CHECK(snapshot.demodulator.pendingFrames == 0);
        consumer->DomainInvalidated(domain);
        snapshot = consumer->GetSnapshot();
        CHECK_FALSE(snapshot.active);
        CHECK(snapshot.demodulator.shutdown);
    }
}

TEST_CASE("Capture demodulator bounds LF4 duplicate refinement and admits only newly recovered codeword slots",
    "[demod][d3d11][capture][remote-visual][low-fps][temporal][duplicate][bounded][warp]")
{
    constexpr std::uint64_t sessionTag = 0x7294B1D8E5063ACFULL;
    auto environment = CreateWarpEnvironment();
    pbdemodd3d11::CaptureDemodulatorConfig consumerConfig;
    consumerConfig.visualProfileId = pbmodulation::kRemoteVisualLowFpsProfileId;
    consumerConfig.slotCount = 2;
    consumerConfig.maximumFrameAgeMilliseconds = 60000;
    consumerConfig.resultQueueCapacity = 2;
    consumerConfig.maximumRoiWidth = pbmodulation::kLocalDesktopCanvasWidth;
    consumerConfig.maximumRoiHeight = pbmodulation::kLocalDesktopCanvasHeight;
    consumerConfig.maximumDuplicateRefinementAttempts = 1;
    std::shared_ptr<pbdemodd3d11::CaptureDemodulator> consumer;
    REQUIRE(pbdemodd3d11::CaptureDemodulator::Create(consumerConfig, consumer));
    pbcapturenormalize::ScreenCaptureDomain domain;
    domain.sourceId[0] = std::byte{0x79};
    domain.captureEpoch = 63;
    REQUIRE(consumer->DomainStarted(domain, MakeCaptureEnvironment(environment.adapterLuid), environment.device.Get()));

    const auto acceptedData = MakeTransportData(pbmodulation::kRemoteVisualLowFpsDataBytes,
        pbmodulation::kRemoteVisualLowFpsCodewords, pbprotocol::SessionTag{sessionTag});
    const auto wrongIdentityData = MakeTransportData(pbmodulation::kRemoteVisualLowFpsDataBytes,
        pbmodulation::kRemoteVisualLowFpsCodewords, pbprotocol::SessionTag{sessionTag + 1});
    const auto MakePixels = [&](const std::uint64_t frameSequence, const bool validIdentity)
    {
        const auto record = MakeRecord(pbmodulation::kRemoteVisualLowFpsProfileId,
            pbmodulation::kRemoteVisualLowFpsLayoutVersion, frameSequence, sessionTag);
        std::vector<std::byte> pixels(pbmodulation::kLocalDesktopFrameBgraBytes);
        REQUIRE(pbmodulation::EncodeRemoteVisualLowFpsFrame(record,
            validIdentity ? std::span<const std::byte>(acceptedData) : std::span<const std::byte>(wrongIdentityData), pixels));
        return pixels;
    };
    const auto MakePartiallyAcceptedPixels = [&](const std::uint64_t frameSequence,
        const std::uint32_t acceptedCodewords)
    {
        REQUIRE(acceptedCodewords <= pbmodulation::kRemoteVisualLowFpsCodewords);
        auto data = wrongIdentityData;
        std::copy_n(acceptedData.begin(), static_cast<std::size_t>(acceptedCodewords) * pbdesktoplevels::kCodewordBytes,
            data.begin());
        const auto record = MakeRecord(pbmodulation::kRemoteVisualLowFpsProfileId,
            pbmodulation::kRemoteVisualLowFpsLayoutVersion, frameSequence, sessionTag);
        std::vector<std::byte> pixels(pbmodulation::kLocalDesktopFrameBgraBytes);
        REQUIRE(pbmodulation::EncodeRemoteVisualLowFpsFrame(record, data, pixels));
        return pixels;
    };

    auto result = std::make_unique<pbdemodd3d11::CaptureDemodulatorResult>();
    std::uint64_t captureObservation = 0;
    const auto Deliver = [&](const std::span<const std::byte> pixels, const bool expectGpuSubmission,
        const pbdemodd3d11::CaptureDemodulatorTemporalDisposition expectedDisposition)
    {
        captureObservation++;
        const auto texture = UploadRoiTexture(environment.device.Get(), pixels);
        auto frame = MakeFrame(texture.Get(), environment.adapterLuid, domain, captureObservation);
        frame.metadata.slotIndex = static_cast<std::uint32_t>((captureObservation - 1) % consumerConfig.slotCount);
        StampCurrent(frame);
        REQUIRE(consumer->Submit(frame, environment.context.Get()));
        WaitForDownstreamMarker(environment.device.Get(), environment.context.Get());
        const auto firstStage = consumer->CompleteStage(frame.metadata, texture.Get(), environment.context.Get(), false);
        REQUIRE(firstStage.status);
        CHECK(firstStage.gpuWorkSubmitted == expectGpuSubmission);
        if (firstStage.gpuWorkSubmitted)
        {
            WaitForDownstreamMarker(environment.device.Get(), environment.context.Get());
            const auto secondStage = consumer->CompleteStage(frame.metadata, texture.Get(), environment.context.Get(), false);
            REQUIRE(secondStage.status);
            CHECK_FALSE(secondStage.gpuWorkSubmitted);
        }
        REQUIRE(consumer->TakeResult(*result));
        CHECK(result->temporalDisposition == expectedDisposition);
        CHECK_FALSE(consumer->TakeResult(*result));
    };

    Deliver(MakePixels(100, true), true, pbdemodd3d11::CaptureDemodulatorTemporalDisposition::Unique);
    CHECK(result->kind == pbdemodd3d11::CaptureDemodulatorResultKind::Transport);
    CHECK(result->admittedTransportBlockCount == pbmodulation::kRemoteVisualLowFpsCodewords);
    Deliver(MakePixels(100, true), false, pbdemodd3d11::CaptureDemodulatorTemporalDisposition::DuplicateSuppressed);
    CHECK(result->kind == pbdemodd3d11::CaptureDemodulatorResultKind::TelemetryOnly);
    CHECK(result->admittedTransportBlockCount == 0);

    Deliver(MakePixels(103, true), true, pbdemodd3d11::CaptureDemodulatorTemporalDisposition::Unique);
    CHECK(result->admittedTransportBlockCount == pbmodulation::kRemoteVisualLowFpsCodewords);
    Deliver(MakePixels(102, true), false, pbdemodd3d11::CaptureDemodulatorTemporalDisposition::Reordered);
    CHECK(result->kind == pbdemodd3d11::CaptureDemodulatorResultKind::TelemetryOnly);

    Deliver(MakePixels(104, false), true, pbdemodd3d11::CaptureDemodulatorTemporalDisposition::Unique);
    CHECK(result->kind == pbdemodd3d11::CaptureDemodulatorResultKind::Transport);
    CHECK(result->demodulation.evaluation.identityFailures == pbmodulation::kRemoteVisualLowFpsCodewords);
    Deliver(MakePixels(104, true), true, pbdemodd3d11::CaptureDemodulatorTemporalDisposition::DuplicateRefinement);
    CHECK(result->kind == pbdemodd3d11::CaptureDemodulatorResultKind::Transport);
    CHECK(result->admittedTransportBlockCount == pbmodulation::kRemoteVisualLowFpsCodewords);
    Deliver(MakePixels(104, true), false, pbdemodd3d11::CaptureDemodulatorTemporalDisposition::DuplicateSuppressed);
    CHECK(result->admittedTransportBlockCount == 0);
    Deliver(MakePixels(105, true), true, pbdemodd3d11::CaptureDemodulatorTemporalDisposition::Unique);
    CHECK(result->admittedTransportBlockCount == pbmodulation::kRemoteVisualLowFpsCodewords);

    Deliver(MakePixels(106, false), true, pbdemodd3d11::CaptureDemodulatorTemporalDisposition::Unique);
    CHECK(result->kind == pbdemodd3d11::CaptureDemodulatorResultKind::Transport);
    CHECK(result->admittedTransportBlockCount == 0);
    Deliver(MakePixels(106, false), true, pbdemodd3d11::CaptureDemodulatorTemporalDisposition::DuplicateRefinement);
    CHECK(result->admittedTransportBlockCount == 0);
    Deliver(MakePixels(106, true), false, pbdemodd3d11::CaptureDemodulatorTemporalDisposition::DuplicateSuppressed);
    CHECK(result->admittedTransportBlockCount == 0);
    Deliver(MakePixels(107, true), true, pbdemodd3d11::CaptureDemodulatorTemporalDisposition::Unique);
    CHECK(result->admittedTransportBlockCount == pbmodulation::kRemoteVisualLowFpsCodewords);

    const auto repeatedPixels = MakePixels(107, true);
    for (std::uint32_t duplicate = 0; duplicate < 32; duplicate++)
    {
        Deliver(repeatedPixels, false, pbdemodd3d11::CaptureDemodulatorTemporalDisposition::DuplicateSuppressed);
        CHECK(result->kind == pbdemodd3d11::CaptureDemodulatorResultKind::TelemetryOnly);
        CHECK(result->admittedTransportBlockCount == 0);
    }

    const auto snapshot = consumer->GetSnapshot();
    CHECK(snapshot.submittedFrames == 44);
    CHECK(snapshot.completedFrames == 44);
    CHECK(snapshot.temporalUniqueFrames == 6);
    CHECK(snapshot.temporalDuplicateFrames == 37);
    CHECK(snapshot.temporalReorderedFrames == 1);
    CHECK(snapshot.temporalGapEvents == 1);
    CHECK(snapshot.temporalSkippedSequences == 2);
    CHECK(snapshot.duplicateRefinementAttempts == 2);
    CHECK(snapshot.duplicateRefinementRecoveries == 1);
    CHECK(snapshot.duplicateRefinementLimitDrops == 1);
    CHECK(snapshot.temporalSuppressedFrames == 37);
    CHECK(snapshot.temporalStaleCompletionDrops == 0);
    CHECK(snapshot.temporallyAdmittedTransportBlocks == 20);
    CHECK(snapshot.acceptedTransportBlocks == 20);
    CHECK(snapshot.verifiedFrames == 4);
    CHECK(snapshot.postFecFailedFrames == 2);
    CHECK(snapshot.stagedGpuSubmissions == 8);
    CHECK(snapshot.stagedGpuCompletions == 8);
    CHECK(snapshot.pendingFrames == 0);
    CHECK(snapshot.pendingHighWater <= consumerConfig.slotCount);
    CHECK(snapshot.queuedResults == 0);
    CHECK(snapshot.resultQueueHighWater == 1);
    CHECK(snapshot.resultQueueDrops == 0);
    CHECK(snapshot.demodulator.submittedFrames == 8);
    CHECK(snapshot.demodulator.completedFrames == 8);
    CHECK(snapshot.demodulator.pendingFrames == 0);
    consumer->DomainInvalidated(domain);
    CHECK(consumer->GetSnapshot().demodulator.shutdown);

    domain.captureEpoch++;
    REQUIRE(consumer->DomainStarted(domain, MakeCaptureEnvironment(environment.adapterLuid), environment.device.Get()));
    Deliver(repeatedPixels, true, pbdemodd3d11::CaptureDemodulatorTemporalDisposition::Unique);
    CHECK(result->admittedTransportBlockCount == pbmodulation::kRemoteVisualLowFpsCodewords);
    const auto recreatedSnapshot = consumer->GetSnapshot();
    CHECK(recreatedSnapshot.domainStarts == 2);
    CHECK(recreatedSnapshot.temporalUniqueFrames == 7);
    CHECK(recreatedSnapshot.temporalDuplicateFrames == 37);
    CHECK(recreatedSnapshot.temporallyAdmittedTransportBlocks == 24);
    CHECK(recreatedSnapshot.acceptedTransportBlocks == 24);
    CHECK(recreatedSnapshot.stagedGpuSubmissions == 9);
    CHECK(recreatedSnapshot.stagedGpuCompletions == 9);
    CHECK(recreatedSnapshot.pendingFrames == 0);

    for (std::uint32_t acceptedCodewords = 0; acceptedCodewords <= pbmodulation::kRemoteVisualLowFpsCodewords;
        acceptedCodewords++)
    {
        Deliver(MakePartiallyAcceptedPixels(200 + acceptedCodewords, acceptedCodewords), true,
            pbdemodd3d11::CaptureDemodulatorTemporalDisposition::Unique);
        CHECK(result->kind == pbdemodd3d11::CaptureDemodulatorResultKind::Transport);
        CHECK(result->demodulation.acceptedTransportBlockCount == acceptedCodewords);
        CHECK(result->demodulation.evaluation.identityFailures ==
            pbmodulation::kRemoteVisualLowFpsCodewords - acceptedCodewords);
        REQUIRE(result->admittedTransportBlockCount == acceptedCodewords);
        for (std::uint32_t index = 0; index < acceptedCodewords; index++)
        {
            CHECK(result->admittedTransportBlockIndices[index] == index);
            CHECK(result->demodulation.acceptedTransportBlocks[index].slot == index);
        }
    }

    std::array<std::byte, pbmodulation::kReferenceControlWindowBytes> controlWindow{};
    const std::array<std::byte, 5> controlPayload{
        std::byte{9}, std::byte{8}, std::byte{7}, std::byte{6}, std::byte{5}};
    const pbprotocol::ControlRecordView controlView{pbprotocol::kControlVersion,
        pbprotocol::ControlRecordType::FinalManifest, 93, pbprotocol::SessionTag{sessionTag}, controlPayload};
    const auto controlSize = pbprotocol::GetSerializedSize(controlView);
    REQUIRE(controlSize);
    REQUIRE(pbprotocol::SerializeControlRecord(controlView, std::span(controlWindow).first(controlSize.Value())));
    const auto controlData = MakeRemoteVisualLowFpsControlData(controlWindow);
    const auto controlRecord = MakeRecord(pbmodulation::kRemoteVisualLowFpsProfileId,
        pbmodulation::kRemoteVisualLowFpsLayoutVersion, 205, sessionTag);
    std::vector<std::byte> controlPixels(pbmodulation::kLocalDesktopFrameBgraBytes);
    REQUIRE(pbmodulation::EncodeRemoteVisualLowFpsFrame(controlRecord, controlData, controlPixels));
    Deliver(controlPixels, true, pbdemodd3d11::CaptureDemodulatorTemporalDisposition::Unique);
    CHECK(result->kind == pbdemodd3d11::CaptureDemodulatorResultKind::ControlRecord);
    CHECK(result->demodulation.acceptedRemoteControlBlockCount == pbmodulation::kRemoteVisualLowFpsCodewords);
    CHECK(result->admittedRemoteControlBlockCount == 1);
    CHECK(result->admittedRemoteControlBlockIndices[0] == 0);
    CHECK(result->controlByteCount == controlSize.Value());
    CHECK(std::equal(result->controlBytes.begin(), result->controlBytes.begin() + result->controlByteCount,
        controlWindow.begin(), controlWindow.begin() + result->controlByteCount));
    Deliver(controlPixels, false, pbdemodd3d11::CaptureDemodulatorTemporalDisposition::DuplicateSuppressed);
    CHECK(result->kind == pbdemodd3d11::CaptureDemodulatorResultKind::TelemetryOnly);
    CHECK(result->admittedRemoteControlBlockCount == 0);

    const auto matrixSnapshot = consumer->GetSnapshot();
    CHECK(matrixSnapshot.temporallyAdmittedTransportBlocks == 34);
    CHECK(matrixSnapshot.acceptedTransportBlocks == 34);
    CHECK(matrixSnapshot.controlFrames == 1);
    CHECK(matrixSnapshot.stagedGpuSubmissions == 15);
    CHECK(matrixSnapshot.stagedGpuCompletions == 15);
    CHECK(matrixSnapshot.pendingFrames == 0);
    consumer->DomainInvalidated(domain);
    CHECK(consumer->GetSnapshot().demodulator.shutdown);
}

TEST_CASE("Capture demodulator withholds later LF4 results until an earlier staged completion retires",
    "[demod][d3d11][capture][remote-visual][low-fps][result-order][telemetry][warp]")
{
    const auto scenarios = MakeGpuParityScenarios();
    REQUIRE_FALSE(scenarios.empty());
    const auto& scenario = scenarios.front();
    auto environment = CreateWarpEnvironment();
    pbdemodd3d11::CaptureDemodulatorConfig consumerConfig;
    consumerConfig.visualProfileId = pbmodulation::kRemoteVisualLowFpsProfileId;
    consumerConfig.slotCount = 2;
    consumerConfig.maximumFrameAgeMilliseconds = 60000;
    consumerConfig.resultQueueCapacity = 4;
    consumerConfig.maximumRoiWidth = scenario.fixture.width;
    consumerConfig.maximumRoiHeight = scenario.fixture.height;
    std::shared_ptr<pbdemodd3d11::CaptureDemodulator> consumer;
    REQUIRE(pbdemodd3d11::CaptureDemodulator::Create(consumerConfig, consumer));
    pbcapturenormalize::ScreenCaptureDomain domain;
    domain.sourceId[0] = std::byte{0x7A};
    domain.captureEpoch = 64;
    REQUIRE(consumer->DomainStarted(domain, MakeCaptureEnvironment(environment.adapterLuid,
        static_cast<std::int32_t>(scenario.fixture.width), static_cast<std::int32_t>(scenario.fixture.height)),
        environment.device.Get()));

    const auto firstTexture = UploadBgraTexture(environment.device.Get(), scenario.fixture.pixels,
        scenario.fixture.width, scenario.fixture.height, scenario.fixture.pitch);
    auto firstFrame = MakeFrame(firstTexture.Get(), environment.adapterLuid, domain, 1);
    firstFrame.metadata.slotIndex = 0;
    StampCurrent(firstFrame);
    REQUIRE(consumer->Submit(firstFrame, environment.context.Get()));
    WaitForDownstreamMarker(environment.device.Get(), environment.context.Get());
    const auto firstBootstrapStage = consumer->CompleteStage(firstFrame.metadata, firstTexture.Get(),
        environment.context.Get(), false);
    REQUIRE(firstBootstrapStage.status);
    REQUIRE(firstBootstrapStage.gpuWorkSubmitted);

    const auto laterTexture = UploadBgraTexture(environment.device.Get(), scenario.fixture.pixels,
        scenario.fixture.width, scenario.fixture.height, scenario.fixture.pitch);
    auto laterFrame = MakeFrame(laterTexture.Get(), environment.adapterLuid, domain, 2);
    laterFrame.metadata.slotIndex = 1;
    StampCurrent(laterFrame);
    REQUIRE(consumer->Submit(laterFrame, environment.context.Get()));
    WaitForDownstreamMarker(environment.device.Get(), environment.context.Get());
    const auto laterBootstrapStage = consumer->CompleteStage(laterFrame.metadata, laterTexture.Get(),
        environment.context.Get(), false);
    REQUIRE(laterBootstrapStage.status);
    REQUIRE_FALSE(laterBootstrapStage.gpuWorkSubmitted);

    auto result = std::make_unique<pbdemodd3d11::CaptureDemodulatorResult>();
    REQUIRE_FALSE(consumer->TakeResult(*result));
    REQUIRE(consumer->GetSnapshot().queuedResults == 1);

    WaitForDownstreamMarker(environment.device.Get(), environment.context.Get());
    const auto firstDemodStage = consumer->CompleteStage(firstFrame.metadata, firstTexture.Get(),
        environment.context.Get(), false);
    REQUIRE(firstDemodStage.status);
    REQUIRE_FALSE(firstDemodStage.gpuWorkSubmitted);
    REQUIRE(consumer->TakeResult(*result));
    REQUIRE(result->metadata.captureObservation == 1);
    REQUIRE(result->kind == pbdemodd3d11::CaptureDemodulatorResultKind::Transport);
    REQUIRE(result->temporalDisposition == pbdemodd3d11::CaptureDemodulatorTemporalDisposition::Unique);
    REQUIRE(consumer->TakeResult(*result));
    REQUIRE(result->metadata.captureObservation == 2);
    REQUIRE(result->kind == pbdemodd3d11::CaptureDemodulatorResultKind::TelemetryOnly);
    REQUIRE(result->temporalDisposition ==
        pbdemodd3d11::CaptureDemodulatorTemporalDisposition::DuplicateSuppressed);
    REQUIRE_FALSE(consumer->TakeResult(*result));

    const auto snapshot = consumer->GetSnapshot();
    REQUIRE(snapshot.pendingFrames == 0);
    REQUIRE(snapshot.queuedResults == 0);
    REQUIRE(snapshot.resultsTaken == 2);
    REQUIRE(snapshot.resultQueueDrops == 0);
    consumer->DomainInvalidated(domain);
}

TEST_CASE("Capture demodulator cancels staged LF4 work before epoch replacement and rejects legacy completion",
    "[demod][d3d11][capture][remote-visual][low-fps][epoch][negative][warp]")
{
    const auto scenarios = MakeGpuParityScenarios();
    const auto& scenario = scenarios.front();
    auto environment = CreateWarpEnvironment();
    pbdemodd3d11::CaptureDemodulatorConfig consumerConfig;
    consumerConfig.visualProfileId = pbmodulation::kRemoteVisualLowFpsProfileId;
    consumerConfig.slotCount = 2;
    consumerConfig.maximumFrameAgeMilliseconds = 60000;
    consumerConfig.maximumRoiWidth = scenario.fixture.width;
    consumerConfig.maximumRoiHeight = scenario.fixture.height;
    std::shared_ptr<pbdemodd3d11::CaptureDemodulator> consumer;
    REQUIRE(pbdemodd3d11::CaptureDemodulator::Create(consumerConfig, consumer));
    const auto captureEnvironment = MakeCaptureEnvironment(environment.adapterLuid,
        static_cast<std::int32_t>(scenario.fixture.width), static_cast<std::int32_t>(scenario.fixture.height));
    pbcapturenormalize::ScreenCaptureDomain firstDomain;
    firstDomain.sourceId[0] = std::byte{0x7A};
    firstDomain.captureEpoch = 71;
    REQUIRE(consumer->DomainStarted(firstDomain, captureEnvironment, environment.device.Get()));
    const auto texture = UploadBgraTexture(environment.device.Get(), scenario.fixture.pixels,
        scenario.fixture.width, scenario.fixture.height, scenario.fixture.pitch);
    auto frame = MakeFrame(texture.Get(), environment.adapterLuid, firstDomain, 1);
    frame.metadata.slotIndex = 0;
    StampCurrent(frame);
    REQUIRE(consumer->Submit(frame, environment.context.Get()));
    WaitForDownstreamMarker(environment.device.Get(), environment.context.Get());
    const auto firstStage = consumer->CompleteStage(frame.metadata, texture.Get(), environment.context.Get(), false);
    REQUIRE(firstStage.status);
    REQUIRE(firstStage.gpuWorkSubmitted);
    WaitForDownstreamMarker(environment.device.Get(), environment.context.Get());
    consumer->DomainInvalidated(firstDomain);
    const auto cancelled = consumer->CompleteStage(frame.metadata, nullptr, nullptr, true);
    REQUIRE(cancelled.status);
    CHECK_FALSE(cancelled.gpuWorkSubmitted);
    auto snapshot = consumer->GetSnapshot();
    CHECK(snapshot.cancelledFrames == 1);
    CHECK(snapshot.pendingFrames == 0);
    CHECK(snapshot.stagedGpuSubmissions == 1);
    CHECK(snapshot.stagedGpuCompletions == 1);
    CHECK(snapshot.demodulator.pendingFrames == 0);
    CHECK(snapshot.demodulator.shutdown);
    pbdemodd3d11::CaptureDemodulatorResult result;
    CHECK_FALSE(consumer->TakeResult(result));

    pbcapturenormalize::ScreenCaptureDomain secondDomain = firstDomain;
    secondDomain.captureEpoch = 72;
    REQUIRE(consumer->DomainStarted(secondDomain, captureEnvironment, environment.device.Get()));
    auto replacement = MakeFrame(texture.Get(), environment.adapterLuid, secondDomain, 2);
    replacement.metadata.slotIndex = 0;
    StampCurrent(replacement);
    REQUIRE(consumer->Submit(replacement, environment.context.Get()));
    WaitForDownstreamMarker(environment.device.Get(), environment.context.Get());
    const auto legacy = consumer->Completed(replacement.metadata, environment.context.Get(), false);
    CHECK(legacy.code == pbcapturenormalize::CaptureError::InvalidFrame);
    snapshot = consumer->GetSnapshot();
    CHECK(snapshot.pendingFrames == 0);
    CHECK(snapshot.error.code == pbcapturenormalize::CaptureError::InvalidFrame);
    CHECK_FALSE(consumer->TakeResult(result));
    consumer->DomainInvalidated(secondDomain);
    CHECK(consumer->GetSnapshot().demodulator.shutdown);
}

TEST_CASE("Capture demodulator reports cropped LF4 geometry as telemetry without starting data GPU work",
    "[demod][d3d11][capture][remote-visual][low-fps][geometry][crop][negative][warp]")
{
    constexpr std::uint32_t croppedWidth = 1800;
    constexpr std::uint32_t croppedHeight = pbmodulation::kLocalDesktopCanvasHeight;
    const auto record = MakeRecord(pbmodulation::kRemoteVisualLowFpsProfileId,
        pbmodulation::kRemoteVisualLowFpsLayoutVersion, 83, 0x8D5B1742A690CE3FULL);
    std::vector<std::byte> logicalData(pbmodulation::kRemoteVisualLowFpsDataBytes);
    REQUIRE(pbdesktoplevels::GenerateDiagnosticData(record, logicalData));
    std::vector<std::byte> raster(pbmodulation::kLocalDesktopFrameBgraBytes);
    REQUIRE(pbmodulation::EncodeRemoteVisualLowFpsFrame(record, logicalData, raster));
    std::vector<std::byte> cropped(static_cast<std::size_t>(croppedWidth) * croppedHeight * 4);
    for (std::uint32_t row = 0; row < croppedHeight; row++)
    {
        std::copy_n(raster.begin() + static_cast<std::size_t>(row) * pbmodulation::kLocalDesktopCanvasWidth * 4,
            static_cast<std::size_t>(croppedWidth) * 4, cropped.begin() + static_cast<std::size_t>(row) * croppedWidth * 4);
    }

    auto environment = CreateWarpEnvironment();
    pbdemodd3d11::CaptureDemodulatorConfig consumerConfig;
    consumerConfig.visualProfileId = pbmodulation::kRemoteVisualLowFpsProfileId;
    consumerConfig.slotCount = 2;
    consumerConfig.maximumFrameAgeMilliseconds = 60000;
    consumerConfig.resultQueueCapacity = 2;
    consumerConfig.maximumRoiWidth = croppedWidth;
    consumerConfig.maximumRoiHeight = croppedHeight;
    std::shared_ptr<pbdemodd3d11::CaptureDemodulator> consumer;
    REQUIRE(pbdemodd3d11::CaptureDemodulator::Create(consumerConfig, consumer));
    const auto captureConfig = MakeCaptureConfig(consumerConfig.slotCount, consumerConfig.maximumFrameAgeMilliseconds,
        static_cast<std::int32_t>(croppedWidth), static_cast<std::int32_t>(croppedHeight));
    REQUIRE(consumer->ValidateConfiguration(captureConfig));
    pbcapturenormalize::ScreenCaptureDomain domain;
    domain.sourceId[0] = std::byte{0x7C};
    domain.captureEpoch = 84;
    REQUIRE(consumer->DomainStarted(domain, MakeCaptureEnvironment(environment.adapterLuid,
        static_cast<std::int32_t>(croppedWidth), static_cast<std::int32_t>(croppedHeight)), environment.device.Get()));
    const auto texture = UploadBgraTexture(environment.device.Get(), cropped, croppedWidth, croppedHeight, croppedWidth * 4);
    auto frame = MakeFrame(texture.Get(), environment.adapterLuid, domain, 1);
    frame.metadata.slotIndex = 0;
    StampCurrent(frame);
    REQUIRE(consumer->Submit(frame, environment.context.Get()));
    WaitForDownstreamMarker(environment.device.Get(), environment.context.Get());
    const auto completion = consumer->CompleteStage(frame.metadata, texture.Get(), environment.context.Get(), false);
    REQUIRE(completion.status);
    CHECK_FALSE(completion.gpuWorkSubmitted);

    pbdemodd3d11::CaptureDemodulatorResult result;
    REQUIRE(consumer->TakeResult(result));
    CHECK(result.kind == pbdemodd3d11::CaptureDemodulatorResultKind::TelemetryOnly);
    CHECK_FALSE(result.bootstrap.IsAccepted());
    CHECK(result.geometryStatus == pbdemodd3d11::CaptureDemodulatorGeometryStatus::Rejected);
    CHECK(result.demodulation.acceptedTransportBlockCount == 0);
    CHECK_FALSE(consumer->TakeResult(result));
    const auto snapshot = consumer->GetSnapshot();
    CHECK(snapshot.completedFrames == 1);
    CHECK(snapshot.bootstrapRejectedFrames == 1);
    CHECK(snapshot.rejectedGeometryFrames == 1);
    CHECK(snapshot.stagedGpuSubmissions == 0);
    CHECK(snapshot.stagedGpuCompletions == 0);
    CHECK(snapshot.acceptedTransportBlocks == 0);
    CHECK(snapshot.pendingFrames == 0);
    CHECK(snapshot.demodulator.submittedFrames == 0);
    CHECK(snapshot.demodulator.pendingFrames == 0);
    consumer->DomainInvalidated(domain);
    CHECK(consumer->GetSnapshot().demodulator.shutdown);
}

TEST_CASE("Capture demodulator validates LF4 hard ROI reservation and policy before capture allocation",
    "[demod][d3d11][capture][remote-visual][low-fps][budget][negative]")
{
    pbdemodd3d11::CaptureDemodulatorConfig config;
    config.visualProfileId = pbmodulation::kRemoteVisualLowFpsProfileId;
    config.slotCount = 2;
    config.maximumFrameAgeMilliseconds = 60000;
    config.resultQueueCapacity = 2;
    config.maximumRoiWidth = 2442;
    config.maximumRoiHeight = 1386;
    pbdemodd3d11::CaptureDemodulatorBudget expected;
    REQUIRE(pbdemodd3d11::CalculateCaptureDemodulatorBudget(config, expected));
    REQUIRE(expected.bootstrapStagingBytes == static_cast<std::uint64_t>(config.maximumRoiWidth) *
        config.maximumRoiHeight * 4 * config.slotCount);
    REQUIRE(expected.referenceScratchBytes == 0);

    pbdemodd3d11::CaptureDemodulatorBudget unchanged;
    unchanged.totalBytes = 0x12345678;
    const auto sentinel = unchanged;
    auto invalid = config;
    invalid.maximumRoiWidth = 0;
    auto status = pbdemodd3d11::CalculateCaptureDemodulatorBudget(invalid, unchanged);
    CHECK(status.code == pbcapturenormalize::CaptureError::InvalidConfiguration);
    CHECK(unchanged == sentinel);

    invalid = config;
    invalid.maximumRoiWidth = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION + 1;
    status = pbdemodd3d11::CalculateCaptureDemodulatorBudget(invalid, unchanged);
    CHECK(status.code == pbcapturenormalize::CaptureError::InvalidConfiguration);
    CHECK(unchanged == sentinel);

    invalid = config;
    invalid.remoteVisualLowFpsPolicy.minimumSymbolMargin = std::numeric_limits<double>::quiet_NaN();
    status = pbdemodd3d11::CalculateCaptureDemodulatorBudget(invalid, unchanged);
    CHECK(status.code == pbcapturenormalize::CaptureError::InvalidConfiguration);
    CHECK(unchanged == sentinel);

    invalid = config;
    invalid.maximumDuplicateRefinementAttempts = pbdemodd3d11::maximumCaptureDemodDuplicateRefinementAttempts + 1;
    status = pbdemodd3d11::CalculateCaptureDemodulatorBudget(invalid, unchanged);
    CHECK(status.code == pbcapturenormalize::CaptureError::InvalidConfiguration);
    CHECK(unchanged == sentinel);

    invalid = config;
    invalid.maximumResidentBytes = expected.totalBytes - 1;
    status = pbdemodd3d11::CalculateCaptureDemodulatorBudget(invalid, unchanged);
    CHECK(status.code == pbcapturenormalize::CaptureError::ResourceLimit);
    CHECK(unchanged == sentinel);
    invalid.maximumResidentBytes = expected.totalBytes;
    pbdemodd3d11::CaptureDemodulatorBudget exact;
    REQUIRE(pbdemodd3d11::CalculateCaptureDemodulatorBudget(invalid, exact));
    CHECK(exact == expected);

    std::shared_ptr<pbdemodd3d11::CaptureDemodulator> consumer;
    REQUIRE(pbdemodd3d11::CaptureDemodulator::Create(invalid, consumer));
    CHECK(consumer->ReservedBytes() == expected.totalBytes);
    CHECK(consumer->ValidateConfiguration(MakeCaptureConfig(config.slotCount, config.maximumFrameAgeMilliseconds,
        static_cast<std::int32_t>(config.maximumRoiWidth), static_cast<std::int32_t>(config.maximumRoiHeight))));
    const auto tooWide = consumer->ValidateConfiguration(MakeCaptureConfig(config.slotCount, config.maximumFrameAgeMilliseconds,
        static_cast<std::int32_t>(config.maximumRoiWidth + 1), static_cast<std::int32_t>(config.maximumRoiHeight)));
    CHECK(tooWide.code == pbcapturenormalize::CaptureError::InvalidConfiguration);

    auto environment = CreateWarpEnvironment();
    auto wrongAdapterEnvironment = MakeCaptureEnvironment(environment.adapterLuid,
        static_cast<std::int32_t>(config.maximumRoiWidth), static_cast<std::int32_t>(config.maximumRoiHeight));
    wrongAdapterEnvironment.adapterLuid.LowPart ^= 1;
    pbcapturenormalize::ScreenCaptureDomain domain;
    domain.sourceId[0] = std::byte{0x62};
    domain.captureEpoch = 9;
    status = consumer->DomainStarted(domain, wrongAdapterEnvironment, environment.device.Get());
    CHECK(status.code == pbcapturenormalize::CaptureError::InvalidFrame);
    CHECK(status.stage == pbcapturenormalize::CaptureStage::Adapter);
    CHECK_FALSE(consumer->GetSnapshot().active);
}

TEST_CASE("Capture demodulator binds fixed Bootstrap and GPU Transport to the same retired frame for both physical layers",
    "[demod][d3d11][capture][bootstrap][transport][shape-chroma][warp]")
{
    auto environment = CreateWarpEnvironment();
    struct ProfileCase
    {
        std::uint64_t profileId;
        std::uint8_t layoutVersion;
        std::size_t dataBytes;
        std::uint32_t codewords;
    };
    const std::array cases{
        ProfileCase{pbmodulation::kDesktopLevels4ProfileId, pbmodulation::kDesktopLevelsLayoutVersion, 21672, 10},
        ProfileCase{pbmodulation::kShapeChromaProfileId, pbmodulation::kShapeChromaLayoutVersion,
            pbmodulation::kShapeChromaDataBytes, pbmodulation::kShapeChromaCodewords},
        ProfileCase{pbmodulation::kRemoteVisualProfileId, pbmodulation::kRemoteVisualLayoutVersion,
            pbmodulation::kRemoteVisualDataBytes, pbmodulation::kRemoteVisualCodewords}};
    std::uint64_t domainByte = 0x41;
    for (const auto& profile : cases)
    {
        CAPTURE(profile.profileId);
        pbdemodd3d11::CaptureDemodulatorConfig consumerConfig;
        consumerConfig.visualProfileId = profile.profileId;
        consumerConfig.slotCount = 2;
        consumerConfig.maximumFrameAgeMilliseconds = 60000;
        consumerConfig.resultQueueCapacity = 2;
        pbdemodd3d11::CaptureDemodulatorBudget budget;
        REQUIRE(pbdemodd3d11::CalculateCaptureDemodulatorBudget(consumerConfig, budget));
        REQUIRE(budget.demodulatorBytes > 0);
        REQUIRE(budget.bootstrapStagingBytes == 2ULL * pbmodulation::kLocalDesktopFrameBgraBytes);
        REQUIRE(budget.referenceScratchBytes == pbmodulation::kLocalDesktopFrameBgraBytes);
        REQUIRE(budget.resultQueueBytes == 2ULL * sizeof(pbdemodd3d11::CaptureDemodulatorResult));
        REQUIRE(budget.totalBytes == budget.demodulatorBytes + budget.bootstrapStagingBytes +
            budget.referenceScratchBytes + budget.resultQueueBytes + budget.fixedOverheadBytes);
        std::shared_ptr<pbdemodd3d11::CaptureDemodulator> consumer;
        REQUIRE(pbdemodd3d11::CaptureDemodulator::Create(consumerConfig, consumer));
        REQUIRE(consumer->ReservedBytes() == budget.totalBytes);
        const auto captureConfig = MakeCaptureConfig(2, 60000);
        REQUIRE(consumer->ValidateConfiguration(captureConfig));

        pbcapturenormalize::ScreenCaptureDomain domain;
        domain.sourceId[0] = static_cast<std::byte>(domainByte++);
        domain.captureEpoch = 1;
        REQUIRE(consumer->DomainStarted(domain, MakeCaptureEnvironment(environment.adapterLuid), environment.device.Get()));
        constexpr std::uint64_t sessionTag = 0x73616D652D667261ULL;
        const auto record = MakeRecord(profile.profileId, profile.layoutVersion, 13, sessionTag);
        const auto data = MakeTransportData(profile.dataBytes, profile.codewords, pbprotocol::SessionTag{sessionTag});
        const auto pixels = EncodeTransportPixels(profile.profileId, record, data);
        const auto texture = UploadRoiTexture(environment.device.Get(), pixels);
        auto frame = MakeFrame(texture.Get(), environment.adapterLuid, domain, 1);
        StampCurrent(frame);
        REQUIRE(consumer->Submit(frame, environment.context.Get()));
        WaitForDownstreamMarker(environment.device.Get(), environment.context.Get());
        REQUIRE(consumer->Completed(frame.metadata, environment.context.Get(), false));

        pbdemodd3d11::CaptureDemodulatorResult result;
        REQUIRE(consumer->TakeResult(result));
        REQUIRE(result.bootstrap.IsAccepted());
        REQUIRE(result.bootstrap.canonical44 == record);
        REQUIRE(result.demodulation.metadata.domain == domain);
        REQUIRE(result.demodulation.metadata.captureObservation == 1);
        REQUIRE(result.demodulation.visualProfileId == profile.profileId);
        REQUIRE(result.demodulation.evaluation.IsVerified());
        REQUIRE(result.demodulation.evaluation.comparedCodedBits == 0);
        REQUIRE(result.demodulation.acceptedTransportBlockCount == profile.codewords);
        if (result.demodulation.gpuTimingValid)
        {
            REQUIRE(result.demodulation.gpuTime100ns > 0);
        }
        const bool dataGpuTimingValid = result.demodulation.gpuTimingValid;
        const std::uint64_t dataGpuTime100ns = result.demodulation.gpuTime100ns;
        REQUIRE_FALSE(consumer->TakeResult(result));

        std::array<std::byte, pbmodulation::kReferenceControlWindowBytes> controlWindow{};
        const std::array<std::byte, 7> controlPayload{
            std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}, std::byte{6}, std::byte{7}};
        const pbprotocol::ControlRecordView controlView{pbprotocol::kControlVersion,
            pbprotocol::ControlRecordType::FinalManifest, 91, pbprotocol::SessionTag{sessionTag}, controlPayload};
        const auto controlSize = pbprotocol::GetSerializedSize(controlView);
        REQUIRE(controlSize);
        REQUIRE(controlSize.Value() < controlWindow.size());
        REQUIRE(pbprotocol::SerializeControlRecord(controlView, std::span(controlWindow).first(controlSize.Value())));
        const bool remoteControlCarrier = profile.profileId == pbmodulation::kRemoteVisualProfileId;
        std::vector<std::byte> controlPixels;
        if (remoteControlCarrier)
        {
            const auto controlData = MakeRemoteControlData(controlWindow);
            controlPixels = EncodeTransportPixels(profile.profileId, record, controlData);
        }
        else
        {
            std::array<std::byte, pbmodulation::kReferenceDataRegionBytes> referenceData{};
            controlPixels.resize(pbmodulation::kReferenceFrameBgraBytes);
            REQUIRE(pbmodulation::EncodeReferenceFrame({record, controlWindow, referenceData}, controlPixels));
        }
        const auto controlTexture = UploadRoiTexture(environment.device.Get(), controlPixels);
        auto controlFrame = MakeFrame(controlTexture.Get(), environment.adapterLuid, domain, 2);
        controlFrame.metadata.slotIndex = 0;
        StampCurrent(controlFrame);
        REQUIRE(consumer->Submit(controlFrame, environment.context.Get()));
        WaitForDownstreamMarker(environment.device.Get(), environment.context.Get());
        REQUIRE(consumer->Completed(controlFrame.metadata, environment.context.Get(), false));
        REQUIRE(consumer->TakeResult(result));
        REQUIRE(result.kind == pbdemodd3d11::CaptureDemodulatorResultKind::ControlRecord);
        REQUIRE(result.metadata.captureObservation == 2);
        REQUIRE(result.bootstrapRecord == record);
        REQUIRE(result.bootstrap.IsAccepted());
        REQUIRE(result.bootstrap.canonical44 == record);
        REQUIRE(result.bootstrap.geometry.originX == 0.0);
        REQUIRE(result.bootstrap.geometry.originY == 0.0);
        REQUIRE(result.bootstrap.geometry.scaleX == 1.0);
        REQUIRE(result.bootstrap.geometry.scaleY == 1.0);
        REQUIRE(result.controlByteCount == controlSize.Value());
        REQUIRE(std::equal(result.controlBytes.begin(), result.controlBytes.begin() + result.controlByteCount,
            controlWindow.begin(), controlWindow.begin() + result.controlByteCount));
        REQUIRE(pbprotocol::ParseControlRecord(std::span(result.controlBytes).first(result.controlByteCount)));
        REQUIRE(result.demodulation.acceptedRemoteControlBlockCount == (remoteControlCarrier ? 1U : 0U));
        REQUIRE(result.demodulation.acceptedTransportBlockCount == 0);
        REQUIRE_FALSE(consumer->TakeResult(result));
        auto snapshot = consumer->GetSnapshot();
        REQUIRE(snapshot.active);
        REQUIRE(snapshot.submittedFrames == 2);
        REQUIRE(snapshot.completedFrames == 2);
        REQUIRE(snapshot.bootstrapAcceptedFrames == (remoteControlCarrier ? 2 : 1));
        REQUIRE(snapshot.bootstrapRejectedFrames == 0);
        REQUIRE(snapshot.controlFrames == 1);
        REQUIRE(snapshot.controlFrameFailures == 0);
        REQUIRE(snapshot.verifiedFrames == 1);
        REQUIRE(snapshot.acceptedTransportBlocks == profile.codewords);
        REQUIRE(snapshot.bootstrapMapCalls == 2);
        REQUIRE(snapshot.bootstrapReadbackBytes == 2ULL * pbmodulation::kLocalDesktopFrameBgraBytes);
        REQUIRE(snapshot.bootstrapCpuTimingSamples == 2);
        REQUIRE(snapshot.demodulationCpuTimingSamples == (remoteControlCarrier ? 2 : 1));
        REQUIRE(snapshot.cpuTimingUnavailable == 0);
        REQUIRE(snapshot.pendingFrames == 0);
        REQUIRE(snapshot.queuedResults == 0);
        REQUIRE(snapshot.resultsTaken == 2);
        REQUIRE(snapshot.resultQueueDrops == 0);
        REQUIRE(snapshot.demodulator.rawPixelReadbackBytes == 0);
        REQUIRE(snapshot.demodulator.metricReadbackBytes > 0);
        const std::uint64_t expectedDemodulations = remoteControlCarrier ? 2 : 1;
        REQUIRE(snapshot.demodulator.gpuTimingSamples + snapshot.demodulator.gpuTimingUnavailable == expectedDemodulations);
        if (!remoteControlCarrier)
        {
            REQUIRE(snapshot.demodulator.gpuTimingSamples == (dataGpuTimingValid ? 1 : 0));
            REQUIRE(snapshot.demodulator.gpuTimingUnavailable == (dataGpuTimingValid ? 0 : 1));
            if (dataGpuTimingValid)
            {
                REQUIRE(snapshot.demodulator.gpuTimeTotal100ns == dataGpuTime100ns);
                REQUIRE(snapshot.demodulator.gpuTimeHighWater100ns == dataGpuTime100ns);
            }
        }
        else if (dataGpuTimingValid)
        {
            REQUIRE(snapshot.demodulator.gpuTimeTotal100ns >= dataGpuTime100ns);
            REQUIRE(snapshot.demodulator.gpuTimeHighWater100ns >= dataGpuTime100ns);
        }
        consumer->DomainInvalidated(domain);
        snapshot = consumer->GetSnapshot();
        REQUIRE_FALSE(snapshot.active);
        REQUIRE(snapshot.invalidations == 1);
        REQUIRE(snapshot.demodulator.shutdown);
    }
}

TEST_CASE("Capture demodulator bounds result backlog and erases wrong-session torn and cancelled frames",
    "[demod][d3d11][capture][bounded][mixed][lifetime][warp]")
{
    auto environment = CreateWarpEnvironment();
    pbdemodd3d11::CaptureDemodulatorConfig consumerConfig;
    consumerConfig.visualProfileId = pbmodulation::kDesktopLevels4ProfileId;
    consumerConfig.slotCount = 2;
    consumerConfig.maximumFrameAgeMilliseconds = 60000;
    consumerConfig.resultQueueCapacity = 1;
    std::shared_ptr<pbdemodd3d11::CaptureDemodulator> consumer;
    REQUIRE(pbdemodd3d11::CaptureDemodulator::Create(consumerConfig, consumer));
    pbcapturenormalize::ScreenCaptureDomain domain;
    domain.sourceId[0] = std::byte{0x5C};
    domain.captureEpoch = 3;
    REQUIRE(consumer->DomainStarted(domain, MakeCaptureEnvironment(environment.adapterLuid), environment.device.Get()));

    constexpr std::uint64_t sessionTag = 0x1122334455667788ULL;
    const auto data = MakeTransportData(21672, 10, pbprotocol::SessionTag{sessionTag});
    const auto Deliver = [&](const std::uint64_t observation, const std::span<const std::byte> pixels)
    {
        const auto texture = UploadRoiTexture(environment.device.Get(), pixels);
        auto frame = MakeFrame(texture.Get(), environment.adapterLuid, domain, observation);
        frame.metadata.slotIndex = static_cast<std::uint32_t>((observation - 1) % 2);
        StampCurrent(frame);
        REQUIRE(consumer->Submit(frame, environment.context.Get()));
        WaitForDownstreamMarker(environment.device.Get(), environment.context.Get());
        REQUIRE(consumer->Completed(frame.metadata, environment.context.Get(), false));
    };
    const auto firstRecord = MakeRecord(pbmodulation::kDesktopLevels4ProfileId, pbmodulation::kDesktopLevelsLayoutVersion, 5, sessionTag);
    const auto firstPixels = EncodeTransportPixels(pbmodulation::kDesktopLevels4ProfileId, firstRecord, data);
    const auto rejectedTexture = UploadRoiTexture(environment.device.Get(), firstPixels);
    auto rejectedFrame = MakeFrame(rejectedTexture.Get(), environment.adapterLuid, domain, 99);
    rejectedFrame.metadata.adapterLuid.LowPart ^= 1U;
    StampCurrent(rejectedFrame);
    REQUIRE_FALSE(consumer->Submit(rejectedFrame, environment.context.Get()));
    auto snapshot = consumer->GetSnapshot();
    REQUIRE(snapshot.submittedFrames == 0);
    REQUIRE(snapshot.pendingFrames == 0);
    REQUIRE(snapshot.demodulator.pendingFrames == 0);
    Deliver(1, firstPixels);
    const auto secondRecord = MakeRecord(pbmodulation::kDesktopLevels4ProfileId, pbmodulation::kDesktopLevelsLayoutVersion, 6, sessionTag);
    const auto secondPixels = EncodeTransportPixels(pbmodulation::kDesktopLevels4ProfileId, secondRecord, data);
    Deliver(2, secondPixels);
    pbdemodd3d11::CaptureDemodulatorResult result;
    REQUIRE(consumer->TakeResult(result));
    REQUIRE(result.demodulation.metadata.captureObservation == 2);
    REQUIRE(consumer->GetSnapshot().resultQueueDrops == 1);

    const auto wrongSessionData = MakeTransportData(21672, 10, pbprotocol::SessionTag{sessionTag + 1});
    const auto wrongSessionPixels = EncodeTransportPixels(pbmodulation::kDesktopLevels4ProfileId, secondRecord, wrongSessionData);
    Deliver(3, wrongSessionPixels);
    REQUIRE(consumer->TakeResult(result));
    REQUIRE_FALSE(result.demodulation.evaluation.IsVerified());
    REQUIRE(result.demodulation.evaluation.identityFailures == 10);
    REQUIRE(result.demodulation.evaluation.acceptedTransportBlocks == 0);
    REQUIRE(result.demodulation.acceptedTransportBlockCount == 0);

    auto tornPixels = firstPixels;
    CopyBgraRegion(secondPixels, tornPixels, 1216, 1000, 608, 64);
    Deliver(4, tornPixels);
    REQUIRE(consumer->TakeResult(result));
    REQUIRE(result.kind == pbdemodd3d11::CaptureDemodulatorResultKind::TelemetryOnly);
    REQUIRE(result.metadata.captureObservation == 4);
    REQUIRE_FALSE(result.bootstrap.IsAccepted());
    REQUIRE_FALSE(consumer->TakeResult(result));
    snapshot = consumer->GetSnapshot();
    REQUIRE(snapshot.bootstrapRejectedFrames == 1);
    REQUIRE(snapshot.completedFrames == 4);
    REQUIRE(snapshot.postFecFailedFrames == 1);
    REQUIRE(snapshot.acceptedTransportBlocks == 20);

    const auto cancellationTexture = UploadRoiTexture(environment.device.Get(), firstPixels);
    auto cancellationFrame = MakeFrame(cancellationTexture.Get(), environment.adapterLuid, domain, 5);
    cancellationFrame.metadata.slotIndex = 0;
    StampCurrent(cancellationFrame);
    REQUIRE(consumer->Submit(cancellationFrame, environment.context.Get()));
    WaitForDownstreamMarker(environment.device.Get(), environment.context.Get());
    consumer->DomainInvalidated(domain);
    REQUIRE(consumer->Completed(cancellationFrame.metadata, nullptr, true));
    snapshot = consumer->GetSnapshot();
    REQUIRE_FALSE(snapshot.active);
    REQUIRE(snapshot.cancelledFrames == 1);
    REQUIRE(snapshot.pendingFrames == 0);
    REQUIRE(snapshot.queuedResults == 0);
    REQUIRE(snapshot.demodulator.pendingFrames == 0);
    REQUIRE(snapshot.demodulator.shutdown);
    REQUIRE(snapshot.demodulator.cancelledFrames >= 2);

    pbcapturenormalize::ScreenCaptureDomain nonCancelledDomain;
    nonCancelledDomain.sourceId[0] = std::byte{0x5D};
    nonCancelledDomain.captureEpoch = 4;
    REQUIRE(consumer->DomainStarted(nonCancelledDomain,
        MakeCaptureEnvironment(environment.adapterLuid), environment.device.Get()));
    auto nonCancelledFrame = MakeFrame(cancellationTexture.Get(), environment.adapterLuid, nonCancelledDomain, 6);
    nonCancelledFrame.metadata.slotIndex = 0;
    StampCurrent(nonCancelledFrame);
    REQUIRE(consumer->Submit(nonCancelledFrame, environment.context.Get()));
    WaitForDownstreamMarker(environment.device.Get(), environment.context.Get());
    consumer->DomainInvalidated(nonCancelledDomain);
    const auto invalidCompletion = consumer->Completed(nonCancelledFrame.metadata, environment.context.Get(), false);
    REQUIRE(invalidCompletion.code == pbcapturenormalize::CaptureError::InvalidFrame);
    snapshot = consumer->GetSnapshot();
    REQUIRE(snapshot.pendingFrames == 0);
    REQUIRE(snapshot.demodulator.pendingFrames == 0);
    REQUIRE(snapshot.demodulator.shutdown);
}

TEST_CASE("Capture demodulator snapshots remain safe while the owner replaces capture domains",
    "[demod][d3d11][capture][lifetime][concurrency][warp]")
{
    auto environment = CreateWarpEnvironment();
    pbdemodd3d11::CaptureDemodulatorConfig consumerConfig;
    consumerConfig.visualProfileId = pbmodulation::kDesktopLevels4ProfileId;
    consumerConfig.slotCount = 2;
    consumerConfig.maximumFrameAgeMilliseconds = 60000;
    consumerConfig.resultQueueCapacity = 2;
    std::shared_ptr<pbdemodd3d11::CaptureDemodulator> consumer;
    REQUIRE(pbdemodd3d11::CaptureDemodulator::Create(consumerConfig, consumer));
    const auto captureEnvironment = MakeCaptureEnvironment(environment.adapterLuid);
    const std::uint64_t reservedBytes = consumer->ReservedBytes();
    std::atomic<std::uint64_t> snapshots{0};
    std::atomic<bool> invalidSnapshot{false};
    std::jthread snapshotReader([&](const std::stop_token stopToken)
    {
        while (!stopToken.stop_requested())
        {
            const auto snapshot = consumer->GetSnapshot();
            if (snapshot.reservation.totalBytes != reservedBytes || snapshot.pendingFrames > consumerConfig.slotCount ||
                snapshot.queuedResults > consumerConfig.resultQueueCapacity)
            {
                invalidSnapshot.store(true, std::memory_order_relaxed);
            }
            snapshots.fetch_add(1, std::memory_order_relaxed);
        }
    });
    // WARP recompiles/recreates all fixed GPU resources for each domain. Four
    // transitions cover three destructive replacements while keeping this
    // lifetime regression bounded on software adapters.
    constexpr std::uint64_t iterations = 4;
    for (std::uint64_t index = 0; index < iterations; index++)
    {
        pbcapturenormalize::ScreenCaptureDomain domain;
        domain.sourceId[0] = static_cast<std::byte>(index + 1U);
        domain.captureEpoch = index + 1U;
        REQUIRE(consumer->DomainStarted(domain, captureEnvironment, environment.device.Get()));
        std::this_thread::yield();
        consumer->DomainInvalidated(domain);
    }
    snapshotReader.request_stop();
    snapshotReader.join();
    const auto finalSnapshot = consumer->GetSnapshot();
    REQUIRE(snapshots.load(std::memory_order_relaxed) > 0);
    REQUIRE_FALSE(invalidSnapshot.load(std::memory_order_relaxed));
    REQUIRE_FALSE(finalSnapshot.active);
    REQUIRE(finalSnapshot.domainStarts == iterations);
    REQUIRE(finalSnapshot.invalidations == iterations);
    REQUIRE(finalSnapshot.demodulator.shutdown);
}

TEST_CASE("Unbound D3D11 demod rejects wrong or malformed same-frame Bootstrap without publishing output",
    "[demod][d3d11][bootstrap][binding][errors]")
{
    auto environment = CreateWarpEnvironment();
    std::unique_ptr<pbdemodd3d11::Demodulator> demodulator;
    REQUIRE(pbdemodd3d11::Demodulator::Create(environment.device.Get(), {}, demodulator));

    const auto record = MakeRecord(pbmodulation::kDesktopLevels4ProfileId, pbmodulation::kDesktopLevelsLayoutVersion,
        5, 0x123456789ABCDEF0ULL);
    const auto wrongProfile = MakeRecord(pbmodulation::kShapeChromaProfileId, pbmodulation::kShapeChromaLayoutVersion,
        5, 0x123456789ABCDEF0ULL);
    std::vector<std::byte> logicalData(21672);
    std::vector<std::byte> pixels(pbmodulation::kLocalDesktopFrameBgraBytes);
    REQUIRE(pbdesktoplevels::GenerateDiagnosticData(record, logicalData));
    REQUIRE(pbmodulation::EncodeDesktopLevelsFrame(record, logicalData, pixels));
    const auto texture = UploadRoiTexture(environment.device.Get(), pixels);
    pbcapturenormalize::ScreenCaptureDomain domain;
    domain.sourceId[0] = std::byte{0x6D};
    domain.captureEpoch = 11;
    auto frame = MakeFrame(texture.Get(), environment.adapterLuid, domain, 1);

    pbdemodd3d11::DemodSubmission unchanged{17, 23, domain, 29};
    const auto savedSubmission = unchanged;
    REQUIRE(demodulator->SubmitUnbound(frame, environment.context.Get(), 0xFFFFFFFFFFFFFFFFULL, unchanged).code ==
        pbdemodd3d11::DemodError::UnsupportedProfile);
    REQUIRE(unchanged == savedSubmission);

    pbdemodd3d11::DemodSubmission wrongProfileSubmission;
    REQUIRE(demodulator->SubmitUnbound(frame, environment.context.Get(), pbmodulation::kDesktopLevels4ProfileId,
        wrongProfileSubmission));
    pbdemodd3d11::DemodFrameResult output;
    output.visualProfileId = 0xDEADBEEF;
    output.acceptedTransportBlockCount = 17;
    environment.context->Flush();
    const auto wrongProfileDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    for (;;)
    {
        const auto poll = demodulator->PollUnbound(environment.context.Get(), wrongProfileSubmission, wrongProfile, output);
        if (poll.ready)
        {
            REQUIRE(poll.status.code == pbdemodd3d11::DemodError::InvalidBinding);
            REQUIRE(poll.status.stage == pbdemodd3d11::DemodStage::Binding);
            break;
        }
        REQUIRE(poll.status);
        REQUIRE(std::chrono::steady_clock::now() < wrongProfileDeadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(output.visualProfileId == 0xDEADBEEF);
    REQUIRE(output.acceptedTransportBlockCount == 17);
    const auto retiredPoll = demodulator->PollUnbound(environment.context.Get(), wrongProfileSubmission, record, output);
    REQUIRE(retiredPoll.status.code == pbdemodd3d11::DemodError::InvalidSubmission);
    REQUIRE_FALSE(retiredPoll.ready);

    frame.metadata.captureObservation = 2;
    frame.metadata.slotGeneration = 2;
    pbdemodd3d11::DemodSubmission truncatedSubmission;
    REQUIRE(demodulator->SubmitUnbound(frame, environment.context.Get(), pbmodulation::kDesktopLevels4ProfileId,
        truncatedSubmission));
    environment.context->Flush();
    const auto truncatedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    const auto truncatedRecord = std::span(record).first(record.size() - 1);
    for (;;)
    {
        const auto poll = demodulator->PollUnbound(environment.context.Get(), truncatedSubmission, truncatedRecord, output);
        if (poll.ready)
        {
            REQUIRE(poll.status.code == pbdemodd3d11::DemodError::InvalidBinding);
            REQUIRE(poll.status.stage == pbdemodd3d11::DemodStage::Binding);
            REQUIRE(poll.status.nativeError != 0);
            break;
        }
        REQUIRE(poll.status);
        REQUIRE(std::chrono::steady_clock::now() < truncatedDeadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(output.visualProfileId == 0xDEADBEEF);
    REQUIRE(output.acceptedTransportBlockCount == 17);

    frame.metadata.captureObservation = 3;
    frame.metadata.slotGeneration = 3;
    pbdemodd3d11::DemodSubmission recoveredSubmission;
    REQUIRE(demodulator->SubmitUnbound(frame, environment.context.Get(), pbmodulation::kDesktopLevels4ProfileId,
        recoveredSubmission));
    const auto recovered = PollUnboundUntilReady(*demodulator, environment.context.Get(), recoveredSubmission, record);
    REQUIRE(recovered.visualProfileId == pbmodulation::kDesktopLevels4ProfileId);
    REQUIRE(recovered.evaluation.IsVerified());

    const auto snapshot = demodulator->GetSnapshot();
    REQUIRE(snapshot.submittedFrames == 3);
    REQUIRE(snapshot.completedFrames == 1);
    REQUIRE(snapshot.failedFrames == 2);
    REQUIRE(snapshot.pendingFrames == 0);
    REQUIRE(snapshot.rawPixelReadbackBytes == 0);
    REQUIRE(demodulator->Shutdown(environment.context.Get()));
}

TEST_CASE("D3D11 demod rejects wrong ownership and retires cancelled slots before shutdown",
    "[demod][d3d11][lifetime][errors]")
{
    auto environment = CreateWarpEnvironment();
    std::unique_ptr<pbdemodd3d11::Demodulator> demodulator;
    pbdemodd3d11::DemodConfig config;
    config.readbackSlotCount = 2;
    REQUIRE(pbdemodd3d11::Demodulator::Create(environment.device.Get(), config, demodulator));

    const auto record = MakeRecord(pbmodulation::kDesktopLevels4ProfileId, pbmodulation::kDesktopLevelsLayoutVersion,
        0, 0x123456789ABCDEF0ULL);
    std::vector<std::byte> logicalData(21672);
    std::vector<std::byte> pixels(pbmodulation::kLocalDesktopFrameBgraBytes);
    REQUIRE(pbdesktoplevels::GenerateDiagnosticData(record, logicalData));
    REQUIRE(pbmodulation::EncodeDesktopLevelsFrame(record, logicalData, pixels));
    const auto texture = UploadRoiTexture(environment.device.Get(), pixels);
    pbcapturenormalize::ScreenCaptureDomain domain;
    domain.sourceId[0] = std::byte{0x91};
    domain.captureEpoch = 9;
    auto frame = MakeFrame(texture.Get(), environment.adapterLuid, domain, 1);

    pbdemodd3d11::DemodSubmission unchanged{17, 23, domain, 29};
    const auto savedSubmission = unchanged;
    frame.metadata.adapterLuid.LowPart++;
    const auto wrongAdapter = demodulator->Submit(frame, environment.context.Get(), record, unchanged);
    REQUIRE(wrongAdapter.code == pbdemodd3d11::DemodError::AdapterMismatch);
    REQUIRE(unchanged == savedSubmission);
    frame.metadata.adapterLuid = environment.adapterLuid;

    const auto validDomain = frame.metadata.domain;
    frame.metadata.domain.sourceId = {};
    unchanged = savedSubmission;
    REQUIRE(demodulator->Submit(frame, environment.context.Get(), record, unchanged).code ==
        pbdemodd3d11::DemodError::InvalidFrame);
    REQUIRE(unchanged == savedSubmission);
    frame.metadata.domain = validDomain;
    frame.metadata.physicalRoi.right--;
    REQUIRE(demodulator->Submit(frame, environment.context.Get(), record, unchanged).code ==
        pbdemodd3d11::DemodError::InvalidFrame);
    frame.metadata.physicalRoi.right++;

    pbdemodd3d11::DemodStatus wrongThread;
    std::thread worker([&]
    {
        pbdemodd3d11::DemodSubmission workerOutput;
        wrongThread = demodulator->Submit(frame, environment.context.Get(), record, workerOutput);
    });
    worker.join();
    REQUIRE(wrongThread.code == pbdemodd3d11::DemodError::WrongThread);

    pbdemodd3d11::DemodSubmission first;
    pbdemodd3d11::DemodSubmission second;
    REQUIRE(demodulator->Submit(frame, environment.context.Get(), record, first));
    frame.metadata.captureObservation = 2;
    frame.metadata.slotGeneration = 2;
    REQUIRE(demodulator->Submit(frame, environment.context.Get(), record, second));
    unchanged = savedSubmission;
    const auto busy = demodulator->Submit(frame, environment.context.Get(), record, unchanged);
    REQUIRE(busy.code == pbdemodd3d11::DemodError::Busy);
    REQUIRE(unchanged == savedSubmission);
    REQUIRE(demodulator->Shutdown(environment.context.Get()).code == pbdemodd3d11::DemodError::ShutdownRequired);
    REQUIRE(demodulator->InvalidateDomain(domain));
    PollCancelled(*demodulator, environment.context.Get(), first);
    PollCancelled(*demodulator, environment.context.Get(), second);

    const auto snapshot = demodulator->GetSnapshot();
    REQUIRE(snapshot.submittedFrames == 2);
    REQUIRE(snapshot.cancelledFrames == 2);
    REQUIRE(snapshot.completedFrames == 0);
    REQUIRE(snapshot.failedFrames == 0);
    REQUIRE(snapshot.pendingFrames == 0);
    REQUIRE(snapshot.rawPixelReadbackBytes == 0);
    REQUIRE(demodulator->Shutdown(environment.context.Get()));
    REQUIRE(demodulator->Submit(frame, environment.context.Get(), record, unchanged).code ==
        pbdemodd3d11::DemodError::ShutdownRequired);
}

TEST_CASE("D3D11 demod configuration is bounded and failure leaves output ownership unchanged",
    "[demod][d3d11][resource][errors]")
{
    auto environment = CreateWarpEnvironment();
    std::unique_ptr<pbdemodd3d11::Demodulator> output;
    pbdemodd3d11::DemodConfig config;
    config.readbackSlotCount = 1;
    std::uint64_t unchangedBudget = 0x123456789ABCDEF0ULL;
    REQUIRE(pbdemodd3d11::CalculateDemodulatorResidentBytes(config, unchangedBudget).code ==
        pbdemodd3d11::DemodError::InvalidConfiguration);
    REQUIRE(unchangedBudget == 0x123456789ABCDEF0ULL);
    REQUIRE(pbdemodd3d11::Demodulator::Create(environment.device.Get(), config, output).code ==
        pbdemodd3d11::DemodError::InvalidConfiguration);
    REQUIRE(output == nullptr);
    config.readbackSlotCount = 2;
    config.maximumResidentBytes = 1;
    REQUIRE(pbdemodd3d11::Demodulator::Create(environment.device.Get(), config, output).code ==
        pbdemodd3d11::DemodError::ResourceLimit);
    REQUIRE(output == nullptr);

    config.maximumResidentBytes = 64ULL * 1024 * 1024;
    config.evaluationMode = static_cast<pbdesktoplevels::EvaluationMode>(255);
    REQUIRE(pbdemodd3d11::Demodulator::Create(environment.device.Get(), config, output).code ==
        pbdemodd3d11::DemodError::InvalidConfiguration);
    REQUIRE(output == nullptr);
    config.evaluationMode = pbdesktoplevels::EvaluationMode::Transport;
    std::uint64_t calculatedResidentBytes = 0;
    REQUIRE(pbdemodd3d11::CalculateDemodulatorResidentBytes(config, calculatedResidentBytes));
    REQUIRE(pbdemodd3d11::Demodulator::Create(environment.device.Get(), config, output));
    const std::uint64_t exactResidentBytes = output->GetSnapshot().residentBytes;
    REQUIRE(exactResidentBytes == calculatedResidentBytes);
    REQUIRE(exactResidentBytes > 1);
    REQUIRE(output->Shutdown(environment.context.Get()));
    output.reset();
    config.maximumResidentBytes = exactResidentBytes - 1;
    REQUIRE(pbdemodd3d11::Demodulator::Create(environment.device.Get(), config, output).code ==
        pbdemodd3d11::DemodError::ResourceLimit);
    REQUIRE(output == nullptr);
    config.maximumResidentBytes = exactResidentBytes;
    REQUIRE(pbdemodd3d11::Demodulator::Create(environment.device.Get(), config, output));
    REQUIRE(output->GetSnapshot().residentBytes == exactResidentBytes);
    REQUIRE(output->Shutdown(environment.context.Get()));
}
