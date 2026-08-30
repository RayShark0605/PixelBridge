#include "remote_visual_replay_recorder.h"

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace
{

class TemporaryRecorderFiles
{
public:
    TemporaryRecorderFiles()
    {
        static std::atomic<std::uint32_t> counter{0};
        path_ = std::filesystem::temp_directory_path() /
            (L"pixelbridge-recorder-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(counter.fetch_add(1)) + L".pbrv2");
    }

    ~TemporaryRecorderFiles()
    {
        std::error_code error;
        std::filesystem::remove(path_, error);
        auto partial = path_;
        partial += L".partial";
        error.clear();
        std::filesystem::remove(partial, error);
    }

    const std::filesystem::path& Path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

pbapp::RemoteVisualReplayRecorderConfig MakeConfig(const std::filesystem::path& path)
{
    pbapp::RemoteVisualReplayRecorderConfig config;
    config.outputPath = path;
    config.descriptor.datasetClass = pbrealcapturereplay::ReplayDatasetClass::RemoteVisual;
    config.descriptor.datasetId[0] = std::byte{0x33};
    config.descriptor.datasetId[15] = std::byte{0xCC};
    config.descriptor.runId = "fedcba9876543210fedcba9876543210";
    config.descriptor.visualProfileId = 0x0397572A;
    config.descriptor.createdUtc100ns = 133853184000000000LL;
    config.descriptor.remoteMetadataJsonUtf8 =
        R"({"schema":"PixelBridge.RemoteVisualRunMetadata.1","remoteProvider":"GenericRemote"})";
    config.limits.maximumCaptureFrames = 1;
    config.roiWidth = 4;
    config.roiHeight = 2;
    config.pixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    config.displayIdentityUtf8 = R"(\\.\DISPLAY2|FIXTURE-SERIAL-B)";
    config.dpiX = 144;
    config.dpiY = 144;
    config.queueCapacity = 1;
    return config;
}

pbcapturenormalize::ScreenCaptureFrameMetadata MakeMetadata(const std::uint64_t observation)
{
    pbcapturenormalize::ScreenCaptureFrameMetadata metadata;
    metadata.domain.sourceId[0] = std::byte{0xA5};
    metadata.domain.captureEpoch = 4;
    metadata.backend = pbcapturenormalize::CaptureBackendKind::Dxgi;
    metadata.captureObservation = observation;
    metadata.sourceGeneration = 1;
    metadata.slotGeneration = observation;
    metadata.physicalRoi = {2560, 0, 2564, 2};
    metadata.sourceContentSize = {2560, 1440};
    metadata.sourceExtent = {2560, 1440};
    metadata.roiSize = {4, 2};
    metadata.displayRotation = DXGI_MODE_ROTATION_IDENTITY;
    metadata.sourceTransform = DXGI_MODE_ROTATION_IDENTITY;
    metadata.sourcePixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    metadata.pixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    metadata.bitsPerColor = 8;
    metadata.signalEncoding = pbcapturenormalize::CaptureSignalEncoding::SdrRgb;
    metadata.timestamp.domain = pbcapturenormalize::CaptureTimestampDomain::DxgiQpcTicks;
    metadata.timestamp.rawValue = 1000 + static_cast<std::int64_t>(observation);
    metadata.timestamp.rawFrequency = 10000000;
    metadata.timestamp.monotonic100ns = metadata.timestamp.rawValue;
    metadata.timestamp.arrivalQpc100ns = metadata.timestamp.rawValue;
    metadata.isCursorExcluded = true;
    metadata.sourceCursorState = pbcapturenormalize::CursorState::Excluded;
    return metadata;
}

} // namespace

TEST_CASE("RemoteVisual replay recorder copies receiver ROI through a bounded nonblocking queue",
    "[application][replay][remote-visual][bounded]")
{
    TemporaryRecorderFiles temporary;
    const auto config = MakeConfig(temporary.Path());
    std::shared_ptr<pbapp::RemoteVisualReplayRecorder> recorder;
    REQUIRE(pbapp::RemoteVisualReplayRecorder::Create(config, recorder));
    REQUIRE(recorder != nullptr);
    REQUIRE(recorder->ProcessingReservedBytes() >= 64);

    std::vector<std::byte> pixels(32);
    for (std::size_t index = 0; index < pixels.size(); index++)
    {
        pixels[index] = static_cast<std::byte>(index * 7);
    }
    const auto first = MakeMetadata(1);
    REQUIRE(recorder->Analyze(first, pixels, 16));
    recorder->Commit(first);
    const auto second = MakeMetadata(2);
    REQUIRE(recorder->Analyze(second, pixels, 16));
    recorder->Commit(second);

    pbrealcapturereplay::ReplayV2DemodObservationView observation;
    observation.captureEpoch = 4;
    observation.captureObservation = 1;
    observation.visualProfileId = config.descriptor.visualProfileId;
    observation.disposition = pbrealcapturereplay::ReplayV2DemodDisposition::Accepted;
    observation.bootstrapAttempted = true;
    observation.bootstrapSucceeded = true;
    observation.frameSequenceAvailable = true;
    observation.frameSequence = 77;
    observation.transportProduced = true;
    observation.receiverAdmitted = true;
    recorder->RecordDemodObservation(observation);
    recorder->RequestStop();
    REQUIRE(recorder->Stop(5000));

    const auto snapshot = recorder->GetSnapshot();
    REQUIRE(snapshot.enabled);
    REQUIRE(snapshot.finalized);
    REQUIRE(snapshot.evidenceValid);
    REQUIRE(snapshot.analyzedFrames == 2);
    REQUIRE(snapshot.enqueuedFrames == 1);
    REQUIRE(snapshot.writtenFrames == 1);
    REQUIRE(snapshot.droppedFrames == 1);
    REQUIRE(snapshot.writtenDemodObservations == 1);
    REQUIRE(snapshot.queueHighWater == 1);
    REQUIRE(snapshot.fileBytes == std::filesystem::file_size(temporary.Path()));

    std::unique_ptr<pbrealcapturereplay::ReplayV2Reader> reader;
    REQUIRE(pbrealcapturereplay::ReplayV2Reader::Open(temporary.Path(), config.limits, reader));
    pbrealcapturereplay::ReplayV2Record record;
    REQUIRE(reader->ReadNext(record));
    REQUIRE(record.type == pbrealcapturereplay::ReplayV2RecordType::Capture);
    REQUIRE(record.capture.capture.captureObservation == 1);
    REQUIRE(record.capture.capturedRoi.pixels == pixels);
    REQUIRE_FALSE(record.capture.senderCanonicalRaster.has_value());
    REQUIRE(record.capture.canonicalBootstrap.empty());
    REQUIRE(reader->ReadNext(record));
    REQUIRE(record.type == pbrealcapturereplay::ReplayV2RecordType::DemodObservation);
    REQUIRE(record.demodObservation.frameSequence == 77);
    REQUIRE(reader->ReadNext(record).code == pbrealcapturereplay::ReplayError::EndOfFile);
}

TEST_CASE("RemoteVisual replay recorder rejects unbounded or malformed setup without publishing",
    "[application][replay][remote-visual][validation]")
{
    TemporaryRecorderFiles temporary;
    auto config = MakeConfig(temporary.Path());
    config.queueCapacity = 0;
    std::shared_ptr<pbapp::RemoteVisualReplayRecorder> recorder;
    const auto status = pbapp::RemoteVisualReplayRecorder::Create(config, recorder);
    REQUIRE(status.code == pbcapturenormalize::CaptureError::InvalidConfiguration);
    REQUIRE(recorder == nullptr);
    REQUIRE_FALSE(std::filesystem::exists(temporary.Path()));
    auto partial = temporary.Path();
    partial += L".partial";
    REQUIRE_FALSE(std::filesystem::exists(partial));
}
