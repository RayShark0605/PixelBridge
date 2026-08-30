#include "pbdemodd3d11/demodulator.h"
#include "pbdemodd3d11/capture_demodulator.h"

#include "pbinnerfec/qc_ldpc_codec.h"
#include "pbmodulation/desktop_levels.h"
#include "pbmodulation/reference_raster.h"
#include "pbmodulation/shape_chroma.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/transport_block_codec.h"

#include <catch2/catch_test_macros.hpp>

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
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

pbcapturenormalize::ScreenCaptureFrame MakeFrame(ID3D11Texture2D* texture, const LUID adapterLuid,
    const pbcapturenormalize::ScreenCaptureDomain& domain, const std::uint64_t observation)
{
    pbcapturenormalize::ScreenCaptureFrame frame;
    frame.texture = texture;
    frame.metadata.domain = domain;
    frame.metadata.captureObservation = observation;
    frame.metadata.sourceGeneration = 1;
    frame.metadata.slotGeneration = observation;
    frame.metadata.slotIndex = static_cast<std::uint32_t>(observation % 3);
    frame.metadata.physicalRoi = {0, 0, 1920, 1080};
    frame.metadata.sourceContentSize = {1920, 1080};
    frame.metadata.sourceExtent = {1920, 1080};
    frame.metadata.roiSize = {1920, 1080};
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

pbcapturenormalize::CaptureEnvironment MakeCaptureEnvironment(const LUID adapterLuid)
{
    pbcapturenormalize::CaptureEnvironment environment;
    environment.region.physicalRect = {0, 0, 1920, 1080};
    environment.region.monitorPhysicalRect = environment.region.physicalRect;
    environment.contentSize = {1920, 1080};
    environment.sourceSize = {1920, 1080};
    environment.pixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    environment.adapterLuid = adapterLuid;
    environment.displayFrequency = 60;
    environment.bitsPerColor = 8;
    environment.outputColorSpace = 0;
    environment.hdr = false;
    return environment;
}

pbcapturenormalize::CaptureConfig MakeCaptureConfig(const std::uint32_t slotCount, const std::uint32_t maximumFrameAgeMilliseconds)
{
    pbcapturenormalize::CaptureConfig config;
    config.region.physicalRect = {0, 0, 1920, 1080};
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

OracleResult RunCpuOracle(const bool shapeChroma, const std::span<const std::byte> pixels)
{
    auto channelResult = pbdesktoplevels::ReferenceChannel::Create(pbdesktoplevels::kProcessingReservationBytes);
    REQUIRE(channelResult);
    auto channel = std::move(channelResult).Value();
    OracleResult result;
    if (shapeChroma)
    {
        const auto observation = channel.DecodeShapeChroma(MakeView(pixels));
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
    const std::uint64_t sessionTag, const std::size_t dataBytes, const bool shapeChroma,
    const pbcapturenormalize::ScreenCaptureDomain& domain, const std::uint64_t observation)
{
    const auto record = MakeRecord(profileId, layoutVersion, sequence, sessionTag);
    std::vector<std::byte> logicalData(dataBytes);
    REQUIRE(pbdesktoplevels::GenerateDiagnosticData(record, logicalData));
    std::vector<std::byte> pixels(pbmodulation::kLocalDesktopFrameBgraBytes);
    if (shapeChroma)
    {
        REQUIRE(pbmodulation::EncodeShapeChromaFrame(record, logicalData, pixels));
    }
    else
    {
        REQUIRE(pbmodulation::EncodeDesktopLevelsFrame(record, logicalData, pixels));
    }
    const auto oracle = RunCpuOracle(shapeChroma, pixels);
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

} // namespace

TEST_CASE("D3D11 compute demod agrees with the CPU oracle on every accepted Transport block",
    "[demod][d3d11][warp][oracle][fec]")
{
    auto environment = CreateWarpEnvironment();
    std::unique_ptr<pbdemodd3d11::Demodulator> demodulator;
    REQUIRE(pbdemodd3d11::Demodulator::Create(environment.device.Get(), {}, demodulator));
    REQUIRE(demodulator != nullptr);

    pbcapturenormalize::ScreenCaptureDomain domain;
    domain.sourceId[0] = std::byte{0x5A};
    domain.captureEpoch = 7;
    RunOracleProfile(environment, *demodulator, pbmodulation::kShapeChromaProfileId,
        pbmodulation::kShapeChromaLayoutVersion, 7, 0x1020304050607080ULL, pbmodulation::kShapeChromaDataBytes,
        true, domain, 1);
    RunOracleProfile(environment, *demodulator, pbmodulation::kDesktopLevels4ProfileId,
        pbmodulation::kDesktopLevelsLayoutVersion, 5, 0x1122334455667788ULL, 21672, false, domain, 2);
    RunOracleProfile(environment, *demodulator, pbmodulation::kDesktopLevels2ProfileId,
        pbmodulation::kDesktopLevelsLayoutVersion, 3, 0x8877665544332211ULL, 86688, false, domain, 3);

    const auto snapshot = demodulator->GetSnapshot();
    REQUIRE(snapshot.submittedFrames == 6);
    REQUIRE(snapshot.completedFrames == 6);
    REQUIRE(snapshot.pendingFrames == 0);
    REQUIRE(snapshot.metricReadbackBytes > 0);
    REQUIRE(snapshot.rawPixelReadbackBytes == 0);
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
            pbmodulation::kShapeChromaDataBytes, pbmodulation::kShapeChromaCodewords}};
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
        std::array<std::byte, pbmodulation::kReferenceDataRegionBytes> referenceData{};
        std::vector<std::byte> controlPixels(pbmodulation::kReferenceFrameBgraBytes);
        REQUIRE(pbmodulation::EncodeReferenceFrame({record, controlWindow, referenceData}, controlPixels));
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
        REQUIRE_FALSE(consumer->TakeResult(result));
        auto snapshot = consumer->GetSnapshot();
        REQUIRE(snapshot.active);
        REQUIRE(snapshot.submittedFrames == 2);
        REQUIRE(snapshot.completedFrames == 2);
        REQUIRE(snapshot.bootstrapAcceptedFrames == 1);
        REQUIRE(snapshot.bootstrapRejectedFrames == 0);
        REQUIRE(snapshot.controlFrames == 1);
        REQUIRE(snapshot.controlFrameFailures == 0);
        REQUIRE(snapshot.verifiedFrames == 1);
        REQUIRE(snapshot.acceptedTransportBlocks == profile.codewords);
        REQUIRE(snapshot.bootstrapMapCalls == 2);
        REQUIRE(snapshot.bootstrapReadbackBytes == 2ULL * pbmodulation::kLocalDesktopFrameBgraBytes);
        REQUIRE(snapshot.bootstrapCpuTimingSamples == 2);
        REQUIRE(snapshot.demodulationCpuTimingSamples == 1);
        REQUIRE(snapshot.cpuTimingUnavailable == 0);
        REQUIRE(snapshot.pendingFrames == 0);
        REQUIRE(snapshot.queuedResults == 0);
        REQUIRE(snapshot.resultsTaken == 2);
        REQUIRE(snapshot.resultQueueDrops == 0);
        REQUIRE(snapshot.demodulator.rawPixelReadbackBytes == 0);
        REQUIRE(snapshot.demodulator.metricReadbackBytes > 0);
        REQUIRE(snapshot.demodulator.gpuTimingSamples + snapshot.demodulator.gpuTimingUnavailable == 1);
        REQUIRE(snapshot.demodulator.gpuTimingSamples == (dataGpuTimingValid ? 1 : 0));
        REQUIRE(snapshot.demodulator.gpuTimingUnavailable == (dataGpuTimingValid ? 0 : 1));
        if (dataGpuTimingValid)
        {
            REQUIRE(snapshot.demodulator.gpuTimeTotal100ns == dataGpuTime100ns);
            REQUIRE(snapshot.demodulator.gpuTimeHighWater100ns == dataGpuTime100ns);
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
