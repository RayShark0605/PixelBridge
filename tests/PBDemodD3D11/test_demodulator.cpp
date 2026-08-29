#include "pbdemodd3d11/demodulator.h"

#include "pbmodulation/desktop_levels.h"
#include "pbmodulation/shape_chroma.h"
#include "pbprotocol/bootstrap_control_codec.h"

#include <catch2/catch_test_macros.hpp>

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <array>
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
    const auto gpu = PollUntilReady(demodulator, environment.context.Get(), submission);
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
    REQUIRE(snapshot.submittedFrames == 3);
    REQUIRE(snapshot.completedFrames == 3);
    REQUIRE(snapshot.pendingFrames == 0);
    REQUIRE(snapshot.metricReadbackBytes > 0);
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
    REQUIRE(pbdemodd3d11::Demodulator::Create(environment.device.Get(), config, output).code ==
        pbdemodd3d11::DemodError::InvalidConfiguration);
    REQUIRE(output == nullptr);
    config.readbackSlotCount = 2;
    config.maximumResidentBytes = 1;
    REQUIRE(pbdemodd3d11::Demodulator::Create(environment.device.Get(), config, output).code ==
        pbdemodd3d11::DemodError::ResourceLimit);
    REQUIRE(output == nullptr);
}
