#include "local_desktop_runtime.h"

#include "pbdesktoplevels/reference_channel.h"
#include "pbmodulation/remote_visual.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbrealcapturereplay/replay_v2.h"

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace
{

class OfflineReplayFiles
{
public:
    OfflineReplayFiles()
    {
        static std::atomic<std::uint32_t> counter{0};
        directory_ = std::filesystem::path(PB_TEST_SCRATCH_ROOT) /
            (L"offline-replay-runtime-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(counter.fetch_add(1)));
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
        error.clear();
        REQUIRE(std::filesystem::create_directories(directory_, error));
        REQUIRE_FALSE(error);
        replayPath_ = directory_ / L"receiver-only.pbrv2";
    }

    ~OfflineReplayFiles()
    {
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    [[nodiscard]] const std::filesystem::path& Directory() const noexcept
    {
        return directory_;
    }

    [[nodiscard]] const std::filesystem::path& ReplayPath() const noexcept
    {
        return replayPath_;
    }

private:
    std::filesystem::path directory_;
    std::filesystem::path replayPath_;
};

[[nodiscard]] bool CreateHardwareDevice(Microsoft::WRL::ComPtr<ID3D11Device>& device,
    LUID& adapterLuid) noexcept
{
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL actualFeatureLevel = D3D_FEATURE_LEVEL_9_1;
    const std::array<D3D_FEATURE_LEVEL, 1> featureLevels{D3D_FEATURE_LEVEL_11_0};
    const HRESULT deviceResult = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, featureLevels.data(), static_cast<UINT>(featureLevels.size()),
        D3D11_SDK_VERSION, &device, &actualFeatureLevel, &context);
    if (FAILED(deviceResult) || !device || actualFeatureLevel < D3D_FEATURE_LEVEL_11_0)
    {
        return false;
    }
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter1;
    DXGI_ADAPTER_DESC1 description{};
    if (FAILED(device.As(&dxgiDevice)) || !dxgiDevice || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
        !adapter || FAILED(adapter.As(&adapter1)) || !adapter1 || FAILED(adapter1->GetDesc1(&description)))
    {
        return false;
    }
    adapterLuid = description.AdapterLuid;
    return adapterLuid.HighPart != 0 || adapterLuid.LowPart != 0;
}

[[nodiscard]] std::vector<std::byte> MakeNeutralRemoteRaster()
{
    std::vector<std::byte> pixels(static_cast<std::size_t>(pbapp::phase1CanvasWidth) *
        pbapp::phase1CanvasHeight * 4);
    for (std::size_t offset = 0; offset < pixels.size(); offset += 4)
    {
        pixels[offset] = std::byte{128};
        pixels[offset + 1] = std::byte{128};
        pixels[offset + 2] = std::byte{128};
        pixels[offset + 3] = std::byte{255};
    }
    return pixels;
}

[[nodiscard]] std::vector<std::byte> MakeCanonicalRemoteRaster()
{
    pbprotocol::BootstrapRecord record;
    record.protocolVersion = pbprotocol::GetProtocolVersion();
    record.visualLayoutVersion = pbmodulation::kRemoteVisualLayoutVersion;
    record.visualProfileId = pbmodulation::kRemoteVisualProfileId;
    record.sessionTag.value = 0x1020304050607080ULL;
    record.frameSequence = 7;
    std::array<std::byte, pbprotocol::kBootstrapRecordBytes> bootstrap{};
    REQUIRE(pbprotocol::SerializeBootstrapRecord(record, bootstrap));
    std::array<std::byte, pbmodulation::kRemoteVisualDataBytes> data{};
    REQUIRE(pbdesktoplevels::GenerateDiagnosticData(bootstrap, data));
    std::vector<std::byte> pixels(static_cast<std::size_t>(pbapp::phase1CanvasWidth) *
        pbapp::phase1CanvasHeight * 4);
    REQUIRE(pbmodulation::EncodeRemoteVisualFrame(bootstrap, data, pixels));
    return pixels;
}

void EraseRemoteVisualCarrier(const std::span<std::byte> pixels)
{
    for (std::uint32_t physicalIndex = 0; physicalIndex < pbmodulation::kRemoteVisualTileCount;
        physicalIndex++)
    {
        pbmodulation::LocalDesktopRegion region;
        REQUIRE(pbmodulation::GetRemoteVisualTile(physicalIndex, region));
        for (std::uint32_t row = 0; row < region.height; row++)
        {
            const std::size_t rowOffset = (static_cast<std::size_t>(region.y + row) *
                pbapp::phase1CanvasWidth + region.x) * 4;
            for (std::uint32_t column = 0; column < region.width; column++)
            {
                const std::size_t pixelOffset = rowOffset + static_cast<std::size_t>(column) * 4;
                pixels[pixelOffset] = std::byte{pbmodulation::kRemoteVisualUnusedLuma};
                pixels[pixelOffset + 1] = std::byte{pbmodulation::kRemoteVisualUnusedLuma};
                pixels[pixelOffset + 2] = std::byte{pbmodulation::kRemoteVisualUnusedLuma};
            }
        }
    }
}

[[nodiscard]] pbcapturenormalize::ScreenCaptureFrameMetadata MakeReplayMetadata(const LUID& adapterLuid,
    const std::uint64_t captureObservation)
{
    pbcapturenormalize::ScreenCaptureFrameMetadata metadata;
    metadata.domain.sourceId[0] = std::byte{0x7C};
    metadata.domain.captureEpoch = 1;
    metadata.backend = pbcapturenormalize::CaptureBackendKind::Dxgi;
    metadata.captureObservation = captureObservation;
    metadata.sourceGeneration = 1;
    metadata.slotGeneration = captureObservation;
    metadata.physicalRoi = {2560, 0, 4480, 1080};
    metadata.sourceContentSize = {2560, 1440};
    metadata.sourceExtent = {2560, 1440};
    metadata.roiSize = {1920, 1080};
    metadata.displayRotation = DXGI_MODE_ROTATION_IDENTITY;
    metadata.sourceTransform = DXGI_MODE_ROTATION_IDENTITY;
    metadata.sourcePixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    metadata.pixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    metadata.adapterLuid = adapterLuid;
    metadata.bitsPerColor = 8;
    metadata.outputColorSpace = 0;
    metadata.signalEncoding = pbcapturenormalize::CaptureSignalEncoding::SdrRgb;
    metadata.hdr = false;
    metadata.timestamp.domain = pbcapturenormalize::CaptureTimestampDomain::DxgiQpcTicks;
    metadata.timestamp.rawValue = static_cast<std::int64_t>(captureObservation) * 1000000;
    metadata.timestamp.rawFrequency = 10000000;
    metadata.timestamp.monotonic100ns = static_cast<std::int64_t>(captureObservation) * 1000000;
    metadata.timestamp.arrivalQpc100ns = static_cast<std::int64_t>(captureObservation) * 1000000;
    metadata.isCursorExcluded = true;
    metadata.sourceCursorState = pbcapturenormalize::CursorState::Excluded;
    return metadata;
}

void AppendReplayCapture(pbrealcapturereplay::ReplayV2Writer& writer, const LUID& adapterLuid,
    const std::uint64_t captureObservation, const std::span<const std::byte> pixels)
{
    const auto metadata = MakeReplayMetadata(adapterLuid, captureObservation);
    const pbrealcapturereplay::ReplayV2CaptureView capture{metadata, 96, 96, 1.0, 1.0,
        R"(\\.\DISPLAY2|offline-test)",
        {1920, 1080, 1920 * 4, DXGI_FORMAT_B8G8R8A8_UNORM, pixels}, std::nullopt, {}};
    REQUIRE(writer.AppendCapture(capture));
    pbrealcapturereplay::ReplayV2DemodObservationView deliberatelyDifferentObservation;
    deliberatelyDifferentObservation.captureEpoch = 1;
    deliberatelyDifferentObservation.captureObservation = captureObservation;
    deliberatelyDifferentObservation.visualProfileId = pbmodulation::kRemoteVisualProfileId;
    deliberatelyDifferentObservation.disposition = pbrealcapturereplay::ReplayV2DemodDisposition::Unavailable;
    REQUIRE(writer.AppendDemodObservation(deliberatelyDifferentObservation));
}

void WriteReplay(const std::filesystem::path& path, const LUID& adapterLuid, const bool canonical)
{
    pbrealcapturereplay::ReplayV2FileDescriptor descriptor;
    descriptor.datasetClass = pbrealcapturereplay::ReplayDatasetClass::RemoteVisual;
    descriptor.datasetId[0] = std::byte{0xA5};
    descriptor.datasetId[15] = std::byte{0x5A};
    descriptor.runId = "0123456789abcdef0123456789abcdef";
    descriptor.visualProfileId = pbmodulation::kRemoteVisualProfileId;
    descriptor.createdUtc100ns = 133853184000000000LL;
    descriptor.remoteMetadataJsonUtf8 =
        R"({"schema":"PixelBridge.RemoteVisualRunMetadata.1","remoteProvider":"ReplayRuntimeTest"})";
    pbrealcapturereplay::ReplayV2Limits limits;
    limits.maximumFileBytes = 16ULL * 1024 * 1024;
    limits.maximumTotalRasterBytes = 16ULL * 1024 * 1024;
    limits.maximumCaptureFrames = 1;
    std::unique_ptr<pbrealcapturereplay::ReplayV2Writer> writer;
    REQUIRE(pbrealcapturereplay::ReplayV2Writer::Create(path, descriptor, limits, writer));

    const auto pixels = canonical ? MakeCanonicalRemoteRaster() : MakeNeutralRemoteRaster();
    AppendReplayCapture(*writer, adapterLuid, 1, pixels);
    REQUIRE(writer->Finalize());
}

void WriteProgressiveRefinementReplay(const std::filesystem::path& path, const LUID& adapterLuid)
{
    pbrealcapturereplay::ReplayV2FileDescriptor descriptor;
    descriptor.datasetClass = pbrealcapturereplay::ReplayDatasetClass::RemoteVisual;
    descriptor.datasetId[0] = std::byte{0xB6};
    descriptor.datasetId[15] = std::byte{0x6B};
    descriptor.runId = "fedcba9876543210fedcba9876543210";
    descriptor.visualProfileId = pbmodulation::kRemoteVisualProfileId;
    descriptor.createdUtc100ns = 133853184000000000LL;
    descriptor.remoteMetadataJsonUtf8 =
        R"({"schema":"PixelBridge.RemoteVisualRunMetadata.1","remoteProvider":"ReplayRefinementTest"})";
    pbrealcapturereplay::ReplayV2Limits limits;
    limits.maximumFileBytes = 32ULL * 1024 * 1024;
    limits.maximumTotalRasterBytes = 32ULL * 1024 * 1024;
    limits.maximumCaptureFrames = 2;
    std::unique_ptr<pbrealcapturereplay::ReplayV2Writer> writer;
    REQUIRE(pbrealcapturereplay::ReplayV2Writer::Create(path, descriptor, limits, writer));

    const auto cleanPixels = MakeCanonicalRemoteRaster();
    auto erasedPixels = cleanPixels;
    EraseRemoteVisualCarrier(erasedPixels);
    AppendReplayCapture(*writer, adapterLuid, 1, erasedPixels);
    AppendReplayCapture(*writer, adapterLuid, 2, cleanPixels);
    REQUIRE(writer->Finalize());
}

[[nodiscard]] std::vector<std::byte> ReadFileBytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    REQUIRE(input.good());
    const std::streamoff length = input.tellg();
    REQUIRE(length >= 0);
    std::vector<std::byte> bytes(static_cast<std::size_t>(length));
    input.seekg(0);
    if (!bytes.empty())
    {
        REQUIRE(input.read(reinterpret_cast<char*>(bytes.data()), length));
    }
    return bytes;
}

[[nodiscard]] std::optional<std::filesystem::path> GetRealReplayRoot()
{
    constexpr wchar_t variableName[] = L"PB_REMOTE_VISUAL_REAL_REPLAY_ROOT";
    const DWORD required = GetEnvironmentVariableW(variableName, nullptr, 0);
    if (required == 0)
    {
        return std::nullopt;
    }
    std::vector<wchar_t> value(required);
    const DWORD written = GetEnvironmentVariableW(variableName, value.data(), required);
    if (written == 0 || written >= required)
    {
        return std::nullopt;
    }
    return std::filesystem::path(value.data());
}

} // namespace

TEST_CASE("Offline Replay v2 reuses the production D3D11 demodulator and reports metric drift",
    "[application][replay][offline][d3d11]")
{
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    LUID adapterLuid{};
    if (!CreateHardwareDevice(device, adapterLuid))
    {
        SKIP("A D3D11 hardware adapter is required for the production offline Replay test");
    }
    OfflineReplayFiles files;
    WriteReplay(files.ReplayPath(), adapterLuid, false);

    pbapp::DecoderConfig config;
    config.outputDirectory = files.Directory().wstring();
    config.visualProfile = pbapp::VisualProfile::RemoteVisualResilient;
    config.replayInputPath = files.ReplayPath().wstring();
    config.replayMaximumCaptureFrames = 1;
    config.replayMaximumFileBytes = 16ULL * 1024 * 1024;
    config.remoteMetadata.channelType = pbapp::ChannelType::RemoteVisual;
    config.remoteMetadata.remoteProvider = "ReplayRuntimeTest";
    pbapp::DecoderRuntime runtime;
    REQUIRE(runtime.Start(config));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    for (;;)
    {
        const auto state = runtime.GetSnapshot().state;
        if (state == pbapp::DecoderState::Completed || state == pbapp::DecoderState::Failed ||
            state == pbapp::DecoderState::Stopped)
        {
            break;
        }
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    runtime.Stop();
    const auto snapshot = runtime.GetSnapshot();
    INFO(snapshot.errorDetail);
    REQUIRE(snapshot.state == pbapp::DecoderState::Stopped);
    REQUIRE(snapshot.replayEnabled);
    REQUIRE(snapshot.replayDiagnosticOnly);
    REQUIRE(snapshot.replayOfflineMode);
    REQUIRE(snapshot.replayEvidenceValid);
    REQUIRE(snapshot.replayFinalized);
    REQUIRE(snapshot.replayOfflineCaptureFrames == 1);
    REQUIRE(snapshot.replayOfflineDemodResults == 1);
    REQUIRE(snapshot.replayOfflineObservationComparisons == 1);
    REQUIRE(snapshot.replayOfflineObservationMismatches == 1);
    REQUIRE(snapshot.captureArrivedFrames == 1);
    REQUIRE(snapshot.captureDeliveredFrames == 1);
    REQUIRE(snapshot.bootstrapRejectedFrames == 1);
    REQUIRE_FALSE(snapshot.wholeFileDigestVerified);
    REQUIRE_FALSE(snapshot.finalPublishSucceeded);
}

TEST_CASE("Offline Replay v2 reports RemoteVisual confidence from the production GPU path",
    "[application][replay][offline][d3d11][remote-metric]")
{
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    LUID adapterLuid{};
    if (!CreateHardwareDevice(device, adapterLuid))
    {
        SKIP("A D3D11 hardware adapter is required for the production offline Replay test");
    }
    OfflineReplayFiles files;
    WriteReplay(files.ReplayPath(), adapterLuid, true);

    pbapp::DecoderConfig config;
    config.outputDirectory = files.Directory().wstring();
    config.visualProfile = pbapp::VisualProfile::RemoteVisualResilient;
    config.replayInputPath = files.ReplayPath().wstring();
    config.replayMaximumCaptureFrames = 1;
    config.replayMaximumFileBytes = 16ULL * 1024 * 1024;
    config.remoteMetadata.channelType = pbapp::ChannelType::RemoteVisual;
    config.remoteMetadata.remoteProvider = "ReplayRuntimeTest";
    pbapp::DecoderRuntime runtime;
    REQUIRE(runtime.Start(config));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    for (;;)
    {
        const auto state = runtime.GetSnapshot().state;
        if (state == pbapp::DecoderState::Completed || state == pbapp::DecoderState::Failed ||
            state == pbapp::DecoderState::Stopped)
        {
            break;
        }
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    runtime.Stop();
    const auto snapshot = runtime.GetSnapshot();
    INFO(snapshot.errorDetail);
    REQUIRE(snapshot.state == pbapp::DecoderState::Stopped);
    REQUIRE(snapshot.bootstrapAcceptedFrames == 1);
    REQUIRE(snapshot.remoteMetricFrames == 1);
    REQUIRE(snapshot.remoteMetricSamples == pbmodulation::kRemoteVisualCodedBits);
    REQUIRE(snapshot.remoteZeroMagnitudeMetrics == 0);
    REQUIRE(snapshot.remoteZeroMagnitudeMetricRate == 0.0);
    REQUIRE(snapshot.remoteMinimumAbsoluteMetric);
    REQUIRE(*snapshot.remoteMinimumAbsoluteMetric > 0.99);
    REQUIRE(snapshot.remoteMeanAbsoluteMetric);
    REQUIRE(*snapshot.remoteMeanAbsoluteMetric > 0.99);
    REQUIRE(snapshot.remoteVerifiedMetricFrames == 1);
    REQUIRE(snapshot.remoteRejectedMetricFrames == 0);
    REQUIRE(snapshot.remoteVerifiedMeanAbsoluteMetric);
    REQUIRE(*snapshot.remoteVerifiedMeanAbsoluteMetric > 0.99);
    REQUIRE_FALSE(snapshot.remoteRejectedMeanAbsoluteMetric);
    REQUIRE_FALSE(snapshot.remoteRejectedZeroMagnitudeMetricRate);
    REQUIRE_FALSE(snapshot.wholeFileDigestVerified);
    REQUIRE_FALSE(snapshot.finalPublishSucceeded);
}

TEST_CASE("Offline Replay v2 refines a torn RemoteVisual duplicate without combining captured frames",
    "[application][replay][offline][d3d11][remote-refinement]")
{
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    LUID adapterLuid{};
    if (!CreateHardwareDevice(device, adapterLuid))
    {
        SKIP("A D3D11 hardware adapter is required for the production offline Replay test");
    }
    OfflineReplayFiles files;
    WriteProgressiveRefinementReplay(files.ReplayPath(), adapterLuid);

    pbapp::DecoderConfig config;
    config.outputDirectory = files.Directory().wstring();
    config.visualProfile = pbapp::VisualProfile::RemoteVisualResilient;
    config.replayInputPath = files.ReplayPath().wstring();
    config.replayMaximumCaptureFrames = 2;
    config.replayMaximumFileBytes = 32ULL * 1024 * 1024;
    config.remoteMetadata.channelType = pbapp::ChannelType::RemoteVisual;
    config.remoteMetadata.remoteProvider = "ReplayRefinementTest";
    pbapp::DecoderRuntime runtime;
    REQUIRE(runtime.Start(config));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    for (;;)
    {
        const auto state = runtime.GetSnapshot().state;
        if (state == pbapp::DecoderState::Completed || state == pbapp::DecoderState::Failed ||
            state == pbapp::DecoderState::Stopped)
        {
            break;
        }
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    runtime.Stop();
    const auto snapshot = runtime.GetSnapshot();
    INFO(snapshot.errorDetail);
    REQUIRE(snapshot.state == pbapp::DecoderState::Stopped);
    REQUIRE(snapshot.replayOfflineCaptureFrames == 2);
    REQUIRE(snapshot.captureArrivedFrames == 2);
    REQUIRE(snapshot.captureDeliveredFrames == 2);
    REQUIRE(snapshot.bootstrapAcceptedFrames == 2);
    REQUIRE(snapshot.duplicateFrameSequences == 1);
    REQUIRE(snapshot.remoteMetricFrames == 2);
    REQUIRE(snapshot.remoteRejectedMetricFrames == 1);
    REQUIRE(snapshot.remoteVerifiedMetricFrames == 1);
    REQUIRE(snapshot.remoteDuplicateRefinementAttempts == 1);
    REQUIRE(snapshot.remoteDuplicateRefinementRecoveries == 1);
    REQUIRE(snapshot.endToEndUniqueFrameSequences == 1);
    REQUIRE_FALSE(snapshot.wholeFileDigestVerified);
    REQUIRE_FALSE(snapshot.finalPublishSucceeded);
}

TEST_CASE("LF4 receiver-only Replay reproduces production demod metrics Receiver digest and publish",
    "[application][replay][offline][lf4][production][step15]")
{
    OfflineReplayFiles files;
    const auto liveOutputDirectory = files.Directory() / L"live-output";
    const auto offlineOutputDirectory = files.Directory() / L"offline-output";
    std::error_code error;
    REQUIRE(std::filesystem::create_directory(liveOutputDirectory, error));
    REQUIRE_FALSE(error);
    error.clear();
    REQUIRE(std::filesystem::create_directory(offlineOutputDirectory, error));
    REQUIRE_FALSE(error);
    const std::array<std::byte, 1> source{std::byte{0x5A}};

    pbapp::DecoderReplayProbeSnapshot live;
    const auto liveStatus = pbapp::DecoderRuntimeTestAccess::ProbeRemoteVisualLowFpsReplay(source,
        files.ReplayPath().wstring(), liveOutputDirectory.wstring(), 2, live);
    INFO(liveStatus.message);
    REQUIRE(liveStatus);
    REQUIRE(live.decoder.state == pbapp::DecoderState::Completed);
    REQUIRE(live.decoder.wholeFileDigestVerified);
    REQUIRE(live.decoder.finalPublishSucceeded);
    REQUIRE(live.decoder.verifiedRawBytes == source.size());
    REQUIRE(live.decoder.acceptedTransportBlocks == 4);
    REQUIRE(live.decoder.temporallyAdmittedTransportBlocks == 4);
    REQUIRE(live.decoder.evaluatedDataFrames == 1);
    REQUIRE(live.decoder.evaluatedCodewords == pbmodulation::kRemoteVisualLowFpsCodewords);
    REQUIRE(live.decoder.fecAcceptedTransportBlocks == pbmodulation::kRemoteVisualLowFpsCodewords);
    REQUIRE(live.decoder.fecAcceptedTransportBlockRate == 1.0);
    REQUIRE(live.decoder.remoteMetricFrames == 4);
    REQUIRE(live.decoder.remoteMetricSamples == 4ULL * pbmodulation::kRemoteVisualLowFpsCodedBits);
    REQUIRE(live.decoder.remoteSymbolSamples == 4ULL * pbmodulation::kRemoteVisualDataTileCount);
    REQUIRE(live.decoder.remoteFreshnessRegions == 4ULL * pbmodulation::kRemoteVisualEligibleFreshnessRegions);
    REQUIRE(live.decoder.remoteFreshRegions + live.decoder.remoteStaleRegions == live.decoder.remoteFreshnessRegions);
    REQUIRE(live.decoder.remoteVerifiedMetricFrames == 1);
    REQUIRE(live.decoder.remoteRejectedMetricFrames == 0);
    REQUIRE(live.decoder.remoteUnreliableSymbolRate);
    REQUIRE(live.decoder.remoteFreshnessErasedDataMetricRate);
    REQUIRE(live.decoder.outerUniqueSymbols == 1);
    REQUIRE(live.decoder.duplicateFrameSequences == 2);
    REQUIRE(live.captureFrames == 6);
    REQUIRE(live.captureFrames == live.demodObservations);
    REQUIRE(live.droppedCaptureFrames == 0);
    REQUIRE(live.droppedDemodObservations == 0);
    REQUIRE(live.duplicateSuppressedResults == 2);
    REQUIRE(live.recorderQueueHighWater > 0);
    REQUIRE(live.recorderQueueHighWater <= 2);
    REQUIRE(live.replayFileBytes == std::filesystem::file_size(files.ReplayPath()));

    pbrealcapturereplay::ReplayV2Limits readerLimits;
    readerLimits.maximumFileBytes = 128ULL * 1024 * 1024;
    readerLimits.maximumTotalRasterBytes = 128ULL * 1024 * 1024;
    readerLimits.maximumCaptureFrames = static_cast<std::uint32_t>(live.captureFrames);
    std::unique_ptr<pbrealcapturereplay::ReplayV2Reader> reader;
    REQUIRE(pbrealcapturereplay::ReplayV2Reader::Open(files.ReplayPath(), readerLimits, reader));
    REQUIRE(reader->GetSnapshot().descriptor.visualProfileId == pbmodulation::kRemoteVisualLowFpsProfileId);
    std::uint64_t capturedRecords = 0;
    std::uint64_t detailedObservations = 0;
    std::uint64_t duplicateObservations = 0;
    std::uint64_t metricObservations = 0;
    for (;;)
    {
        pbrealcapturereplay::ReplayV2Record record;
        const auto readStatus = reader->ReadNext(record);
        if (!readStatus)
        {
            REQUIRE(readStatus.code == pbrealcapturereplay::ReplayError::EndOfFile);
            break;
        }
        if (record.type == pbrealcapturereplay::ReplayV2RecordType::Capture)
        {
            capturedRecords++;
            REQUIRE(record.capture.capturedRoi.width == pbapp::phase1CanvasWidth);
            REQUIRE(record.capture.capturedRoi.height == pbapp::phase1CanvasHeight);
            REQUIRE_FALSE(record.capture.senderCanonicalRaster);
            REQUIRE(record.capture.canonicalBootstrap.empty());
            continue;
        }
        detailedObservations++;
        const auto& observation = record.demodObservation;
        REQUIRE(observation.productionDetailAvailable);
        REQUIRE_FALSE(observation.senderTruthAvailable);
        REQUIRE(observation.falseAcceptedCodewords == 0);
        REQUIRE(observation.comparedCodedBits == 0);
        REQUIRE(observation.erroneousCodedBits == 0);
        REQUIRE(observation.visualProfileId == pbmodulation::kRemoteVisualLowFpsProfileId);
        REQUIRE(observation.layoutAvailable);
        REQUIRE(observation.visualLayoutVersion == pbmodulation::kRemoteVisualLowFpsLayoutVersion);
        REQUIRE(observation.geometryAvailable);
        REQUIRE(observation.geometryStatus == pbrealcapturereplay::ReplayV2GeometryStatus::ExactCanvas);
        REQUIRE(observation.geometryScaleX == 1.0);
        REQUIRE(observation.geometryScaleY == 1.0);
        if (observation.temporalDisposition ==
            pbrealcapturereplay::ReplayV2TemporalDisposition::DuplicateSuppressed)
        {
            duplicateObservations++;
        }
        if (observation.metricSummaryAvailable)
        {
            metricObservations++;
            REQUIRE(observation.metricSamples == pbmodulation::kRemoteVisualLowFpsCodedBits);
            REQUIRE(observation.metricReadbackBytes != 0);
        }
    }
    REQUIRE(reader->GetSnapshot().complete);
    REQUIRE(capturedRecords == live.captureFrames);
    REQUIRE(detailedObservations == live.demodObservations);
    REQUIRE(duplicateObservations == 2);
    REQUIRE(metricObservations == 4);

    pbapp::DecoderConfig config;
    config.outputDirectory = offlineOutputDirectory.wstring();
    config.visualProfile = pbapp::VisualProfile::RemoteVisualLowFps;
    config.replayInputPath = files.ReplayPath().wstring();
    config.replayMaximumCaptureFrames = static_cast<std::uint32_t>(live.captureFrames);
    config.replayMaximumFileBytes = 128ULL * 1024 * 1024;
    config.remoteMetadata.channelType = pbapp::ChannelType::RemoteVisual;
    config.remoteMetadata.remoteProvider = "Step15HeadlessWARP";
    pbapp::DecoderRuntime runtime;
    const auto startStatus = runtime.Start(config);
    INFO(startStatus.message);
    REQUIRE(startStatus);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
    for (;;)
    {
        const auto state = runtime.GetSnapshot().state;
        if (state == pbapp::DecoderState::Completed || state == pbapp::DecoderState::Failed ||
            state == pbapp::DecoderState::Stopped)
        {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            FAIL("LF4 production Replay did not reach a terminal state within 180 seconds");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    runtime.Stop();
    const auto offline = runtime.GetSnapshot();
    INFO(offline.errorDetail);
    REQUIRE(offline.state == pbapp::DecoderState::Completed);
    REQUIRE(offline.replayEvidenceValid);
    REQUIRE(offline.replayFinalized);
    REQUIRE(offline.replayOfflineCaptureFrames == live.captureFrames);
    REQUIRE(offline.replayOfflineDemodResults == live.demodObservations);
    REQUIRE(offline.replayOfflineObservationComparisons == live.demodObservations);
    REQUIRE(offline.replayOfflineObservationMismatches == 0);
    REQUIRE(offline.duplicateFrameSequences == 2);
    REQUIRE(offline.wholeFileDigestVerified);
    REQUIRE(offline.finalPublishSucceeded);
    REQUIRE(offline.verifiedRawBytes == source.size());
    REQUIRE(offline.wholeFileDigestHex == live.decoder.wholeFileDigestHex);
    REQUIRE(offline.acceptedTransportBlocks == live.decoder.acceptedTransportBlocks);
    REQUIRE(offline.temporallyAdmittedTransportBlocks == live.decoder.temporallyAdmittedTransportBlocks);
    REQUIRE(offline.evaluatedDataFrames == live.decoder.evaluatedDataFrames);
    REQUIRE(offline.evaluatedCodewords == live.decoder.evaluatedCodewords);
    REQUIRE(offline.fecAcceptedTransportBlocks == live.decoder.fecAcceptedTransportBlocks);
    REQUIRE(offline.fecAcceptedTransportBlockRate == live.decoder.fecAcceptedTransportBlockRate);
    REQUIRE(offline.remoteMetricFrames == live.decoder.remoteMetricFrames);
    REQUIRE(offline.remoteMetricSamples == live.decoder.remoteMetricSamples);
    REQUIRE(offline.remoteSymbolSamples == live.decoder.remoteSymbolSamples);
    REQUIRE(offline.remoteUnreliableSymbols == live.decoder.remoteUnreliableSymbols);
    REQUIRE(offline.remoteUnreliableSymbolRate == live.decoder.remoteUnreliableSymbolRate);
    REQUIRE(offline.remoteFreshnessRegions == live.decoder.remoteFreshnessRegions);
    REQUIRE(offline.remoteFreshRegions == live.decoder.remoteFreshRegions);
    REQUIRE(offline.remoteStaleRegions == live.decoder.remoteStaleRegions);
    REQUIRE(offline.remoteFreshnessErasedDataMetricRate == live.decoder.remoteFreshnessErasedDataMetricRate);
    REQUIRE(offline.outerUniqueSymbols == live.decoder.outerUniqueSymbols);
    REQUIRE_FALSE(offline.falseAcceptedCodewordsAvailable);
    REQUIRE(std::filesystem::path(offline.outputPath) != std::filesystem::path(live.decoder.outputPath));
    REQUIRE(ReadFileBytes(std::filesystem::path(live.decoder.outputPath)) ==
        std::vector<std::byte>(source.begin(), source.end()));
    REQUIRE(ReadFileBytes(std::filesystem::path(offline.outputPath)) ==
        std::vector<std::byte>(source.begin(), source.end()));
}

TEST_CASE("Sealed real Direct Shape and LF4 receiver-only datasets preserve production failure classification",
    "[application][replay][offline][real-replay][step15]")
{
    const auto replayRoot = GetRealReplayRoot();
    if (!replayRoot)
    {
        SKIP("PB_REMOTE_VISUAL_REAL_REPLAY_ROOT is not set");
    }
    struct DatasetCase
    {
        const wchar_t* relativePath = nullptr;
        pbapp::VisualProfile profile = pbapp::VisualProfile::DirectLevels2x2;
        std::uint64_t admittedTransportBlocks = 0;
    };
    const std::array<DatasetCase, 3> cases{{
        {L"direct/direct-receiver-only-v2.pbrv2", pbapp::VisualProfile::DirectLevels2x2, 42},
        {L"shape/shape-receiver-only.pbrv2", pbapp::VisualProfile::ShapeChroma, 32},
        {L"lf4/lf4-receiver-only.pbrv2", pbapp::VisualProfile::RemoteVisualLowFps, 4}}};
    OfflineReplayFiles files;
    std::uint32_t caseIndex = 0;
    for (const auto& dataset : cases)
    {
        CAPTURE(dataset.relativePath);
        const auto replayPath = *replayRoot / dataset.relativePath;
        REQUIRE(std::filesystem::is_regular_file(replayPath));
        const auto outputDirectory = files.Directory() / (L"real-output-" + std::to_wstring(caseIndex));
        std::error_code error;
        REQUIRE(std::filesystem::create_directory(outputDirectory, error));
        REQUIRE_FALSE(error);

        pbapp::DecoderConfig config;
        config.outputDirectory = outputDirectory.wstring();
        config.visualProfile = dataset.profile;
        config.replayInputPath = replayPath.wstring();
        config.replayMaximumCaptureFrames = 8;
        config.replayMaximumFileBytes = 128ULL * 1024 * 1024;
        config.remoteMetadata.channelType = pbapp::ChannelType::RemoteVisual;
        config.remoteMetadata.remoteProvider = "SealedStep02RDP";
        pbapp::DecoderRuntime runtime;
        const auto startStatus = runtime.Start(config);
        INFO(startStatus.message);
        REQUIRE(startStatus);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        for (;;)
        {
            const auto state = runtime.GetSnapshot().state;
            if (state == pbapp::DecoderState::Completed || state == pbapp::DecoderState::Failed ||
                state == pbapp::DecoderState::Stopped)
            {
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                FAIL("real receiver-only Replay did not reach a terminal state within 120 seconds");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        runtime.Stop();
        const auto snapshot = runtime.GetSnapshot();
        INFO(snapshot.errorDetail);
        REQUIRE(snapshot.state == pbapp::DecoderState::Stopped);
        REQUIRE(snapshot.replayEvidenceValid);
        REQUIRE(snapshot.replayFinalized);
        REQUIRE(snapshot.replayOfflineCaptureFrames == 8);
        REQUIRE(snapshot.replayOfflineDemodResults == 8);
        REQUIRE(snapshot.replayOfflineObservationComparisons == 0);
        REQUIRE(snapshot.replayOfflineObservationMismatches == 0);
        REQUIRE(snapshot.bootstrapAcceptedFrames == 8);
        REQUIRE(snapshot.bootstrapRejectedFrames == 0);
        REQUIRE(snapshot.duplicateFrameSequences == 7);
        REQUIRE(snapshot.acceptedTransportBlocks == dataset.admittedTransportBlocks);
        REQUIRE(snapshot.fecFailures == 0);
        REQUIRE(snapshot.crcFailures == 0);
        REQUIRE(snapshot.identityFailures == 0);
        if (dataset.profile == pbapp::VisualProfile::RemoteVisualLowFps)
        {
            REQUIRE(snapshot.visualProfileId == pbmodulation::kRemoteVisualLowFpsProfileId);
            REQUIRE(snapshot.visualLayoutVersion == pbmodulation::kRemoteVisualLowFpsLayoutVersion);
            REQUIRE(snapshot.codedDataBytesPerFrame == pbmodulation::kRemoteVisualLowFpsDataBytes);
            REQUIRE(snapshot.codewordsPerFrame == pbmodulation::kRemoteVisualLowFpsCodewords);
            REQUIRE(snapshot.evaluatedDataFrames == 1);
            REQUIRE(snapshot.evaluatedCodewords == pbmodulation::kRemoteVisualLowFpsCodewords);
            REQUIRE(snapshot.fecAcceptedTransportBlocks == pbmodulation::kRemoteVisualLowFpsCodewords);
            REQUIRE(snapshot.fecAcceptedTransportBlockRate == 1.0);
            REQUIRE(snapshot.remoteMetricFrames == 1);
            REQUIRE(snapshot.remoteMetricSamples == pbmodulation::kRemoteVisualLowFpsCodedBits);
            REQUIRE(snapshot.remoteSymbolSamples == pbmodulation::kRemoteVisualDataTileCount);
            REQUIRE(snapshot.remoteFreshnessRegions == pbmodulation::kRemoteVisualEligibleFreshnessRegions);
            REQUIRE(snapshot.remoteFreshRegions + snapshot.remoteStaleRegions == snapshot.remoteFreshnessRegions);
            REQUIRE(snapshot.remoteFreshnessErasedDataMetricRate);
        }
        else
        {
            REQUIRE(snapshot.remoteMetricFrames == 0);
        }
        REQUIRE_FALSE(snapshot.descriptorKnown);
        REQUIRE_FALSE(snapshot.wholeFileDigestVerified);
        REQUIRE_FALSE(snapshot.finalPublishSucceeded);
        REQUIRE_FALSE(snapshot.falseAcceptedCodewordsAvailable);
        caseIndex++;
    }
}
