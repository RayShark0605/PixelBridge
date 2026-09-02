#include "pbrealcapturereplay/replay_v2.h"

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{

class TemporaryReplayV2Files
{
public:
    TemporaryReplayV2Files()
    {
        static std::atomic<std::uint32_t> counter{0};
        root_ = std::filesystem::temp_directory_path() /
            (L"pixelbridge-replay-v2-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(counter.fetch_add(1)));
    }

    ~TemporaryReplayV2Files()
    {
        for (const auto& path : paths_)
        {
            std::error_code error;
            std::filesystem::remove(path, error);
            auto partial = path;
            partial += L".partial";
            error.clear();
            std::filesystem::remove(partial, error);
        }
    }

    std::filesystem::path Make(const wchar_t* const suffix)
    {
        auto path = root_;
        path += suffix;
        paths_.push_back(path);
        return path;
    }

private:
    std::filesystem::path root_;
    std::vector<std::filesystem::path> paths_;
};

pbrealcapturereplay::ReplayV2FileDescriptor MakeDescriptor()
{
    pbrealcapturereplay::ReplayV2FileDescriptor descriptor;
    descriptor.datasetClass = pbrealcapturereplay::ReplayDatasetClass::RemoteVisual;
    descriptor.datasetId[0] = std::byte{0x52};
    descriptor.datasetId[15] = std::byte{0x19};
    descriptor.runId = "0123456789abcdef0123456789abcdef";
    descriptor.visualProfileId = 0x0397572A;
    descriptor.createdUtc100ns = 133853184000000000LL;
    descriptor.remoteMetadataJsonUtf8 =
        R"({"schema":"PixelBridge.RemoteVisualRunMetadata.1","remoteProvider":"TestRemote","notes":"右屏 ROI"})";
    return descriptor;
}

struct CaptureFixture
{
    pbcapturenormalize::ScreenCaptureFrameMetadata metadata;
    std::string displayIdentity = R"(\\.\DISPLAY2|FIXTURE-SERIAL-B)";
    std::vector<std::byte> capturedPixels;
    std::vector<std::byte> senderPixels;
    std::vector<std::byte> bootstrap;
    bool includeSender = false;

    pbrealcapturereplay::ReplayV2CaptureView View() const
    {
        const pbrealcapturereplay::ReplayRasterView captured{4, 2, 16,
            DXGI_FORMAT_B8G8R8A8_UNORM, capturedPixels};
        const std::optional<pbrealcapturereplay::ReplayRasterView> sender = includeSender ?
            std::optional<pbrealcapturereplay::ReplayRasterView>{pbrealcapturereplay::ReplayRasterView{
                4, 2, 16, DXGI_FORMAT_B8G8R8A8_UNORM, senderPixels}} : std::nullopt;
        return {metadata, 144, 144, 1.0, 1.0, displayIdentity, captured, sender, bootstrap};
    }
};

CaptureFixture MakeCapture(const std::uint64_t observation, const bool includeSender)
{
    CaptureFixture fixture;
    fixture.capturedPixels.resize(32);
    for (std::size_t index = 0; index < fixture.capturedPixels.size(); index++)
    {
        fixture.capturedPixels[index] = static_cast<std::byte>((index * 17 + observation) & 0xFF);
    }
    fixture.includeSender = includeSender;
    if (includeSender)
    {
        fixture.senderPixels = fixture.capturedPixels;
        fixture.bootstrap = {std::byte{0x50}, std::byte{0x42}, std::byte{0x02}};
    }
    auto& metadata = fixture.metadata;
    metadata.domain.sourceId[0] = std::byte{0xA5};
    metadata.domain.sourceId[15] = std::byte{0x5A};
    metadata.domain.captureEpoch = 9;
    metadata.backend = pbcapturenormalize::CaptureBackendKind::Wgc;
    metadata.captureObservation = observation;
    metadata.sourceGeneration = 3;
    metadata.slotGeneration = 20 + observation;
    metadata.slotIndex = static_cast<std::uint32_t>(observation % 3);
    metadata.physicalRoi = {2560, 0, 2564, 2};
    metadata.sourceContentSize = {2560, 1440};
    metadata.sourceExtent = {2560, 1440};
    metadata.roiSize = {4, 2};
    metadata.displayRotation = DXGI_MODE_ROTATION_IDENTITY;
    metadata.sourceTransform = DXGI_MODE_ROTATION_IDENTITY;
    metadata.sourcePixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    metadata.pixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    metadata.adapterLuid.HighPart = 17;
    metadata.adapterLuid.LowPart = 42;
    metadata.bitsPerColor = 8;
    metadata.outputColorSpace = 0;
    metadata.signalEncoding = pbcapturenormalize::CaptureSignalEncoding::SdrRgb;
    metadata.timestamp.domain = pbcapturenormalize::CaptureTimestampDomain::WgcSystemRelative100ns;
    metadata.timestamp.rawValue = 10000000 + static_cast<std::int64_t>(observation) * 1000;
    metadata.timestamp.rawFrequency = 10000000;
    metadata.timestamp.monotonic100ns = metadata.timestamp.rawValue;
    metadata.timestamp.arrivalQpc100ns = metadata.timestamp.rawValue + 10;
    metadata.roiCopyTime100ns = 55;
    metadata.isCursorExcluded = true;
    metadata.sourceCursorState = pbcapturenormalize::CursorState::Excluded;
    return fixture;
}

pbrealcapturereplay::ReplayV2DemodObservationView MakeObservation(const std::uint64_t captureObservation,
    const std::uint64_t frameSequence)
{
    pbrealcapturereplay::ReplayV2DemodObservationView observation;
    observation.captureEpoch = 9;
    observation.captureObservation = captureObservation;
    observation.visualProfileId = 0x0397572A;
    observation.disposition = pbrealcapturereplay::ReplayV2DemodDisposition::Accepted;
    observation.bootstrapAttempted = true;
    observation.bootstrapSucceeded = true;
    observation.frameSequenceAvailable = true;
    observation.frameSequence = frameSequence;
    observation.transportProduced = true;
    observation.receiverAdmitted = true;
    return observation;
}

pbrealcapturereplay::ReplayV2DemodObservationView MakeDetailedObservation(
    const std::uint64_t captureObservation, const std::uint64_t frameSequence)
{
    auto observation = MakeObservation(captureObservation, frameSequence);
    observation.productionDetailAvailable = true;
    observation.layoutAvailable = true;
    observation.visualLayoutVersion = 7;
    observation.resultKind = pbrealcapturereplay::ReplayV2DemodResultKind::Transport;
    observation.geometryAvailable = true;
    observation.geometryStatus = pbrealcapturereplay::ReplayV2GeometryStatus::Scaled;
    observation.geometryOriginX = 17.25;
    observation.geometryOriginY = 9.5;
    observation.geometryScaleX = 1.25;
    observation.geometryScaleY = 1.25;
    observation.temporalDisposition = pbrealcapturereplay::ReplayV2TemporalDisposition::Unique;
    observation.evaluationAvailable = true;
    observation.paddingValid = true;
    observation.codewords = 4;
    observation.fecFailures = 1;
    observation.acceptedTransportBlocks = 3;
    observation.admittedTransportBlocks = 2;
    observation.iterationsTotal = 24;
    observation.iterationsMaximum = 10;
    observation.metricSummaryAvailable = true;
    observation.metricSamples = 64800;
    observation.zeroMagnitudeMetrics = 12;
    observation.minimumAbsoluteMetric = 0.125;
    observation.meanAbsoluteMetric = 1.75;
    observation.freshnessRegions = 32;
    observation.staleRegions = 1;
    observation.freshnessTagMismatches = 1;
    observation.freshnessTagErasures = 1;
    observation.freshnessErasedDataMetrics = 100;
    observation.unreliableSymbols = 120;
    observation.metricReadbackBytes = 261040;
    observation.gpuTimingAvailable = true;
    observation.gpuTime100ns = 12345;
    observation.carrierAccepted = true;
    observation.receiverStateAdvanced = true;
    return observation;
}

void FlipFileByte(const std::filesystem::path& path, const std::uint64_t offset)
{
    std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
    stream.seekg(static_cast<std::streamoff>(offset));
    char value = 0;
    REQUIRE(stream.read(&value, 1));
    value ^= 0x40;
    stream.seekp(static_cast<std::streamoff>(offset));
    REQUIRE(stream.write(&value, 1));
    stream.flush();
    REQUIRE(stream.good());
}

void WriteSingleCapture(const std::filesystem::path& path)
{
    const auto descriptor = MakeDescriptor();
    const auto capture = MakeCapture(1, false);
    std::unique_ptr<pbrealcapturereplay::ReplayV2Writer> writer;
    REQUIRE(pbrealcapturereplay::ReplayV2Writer::Create(path, descriptor, {}, writer));
    REQUIRE(writer->AppendCapture(capture.View()));
    REQUIRE(writer->Finalize());
}

} // namespace

TEST_CASE("Replay v2 preserves receiver-only RemoteVisual captures and asynchronous live observations",
    "[replay][v2][remote-visual][receiver-only]")
{
    TemporaryReplayV2Files temporary;
    const auto path = temporary.Make(L"-roundtrip.pbrv2");
    const auto descriptor = MakeDescriptor();
    const auto first = MakeCapture(1, false);
    const auto second = MakeCapture(2, true);

    std::unique_ptr<pbrealcapturereplay::ReplayV2Writer> writer;
    REQUIRE(pbrealcapturereplay::ReplayV2Writer::Create(path, descriptor, {}, writer));
    REQUIRE(writer != nullptr);
    REQUIRE(writer->AppendCapture(first.View()));
    REQUIRE(writer->AppendCapture(second.View()));
    REQUIRE(writer->AppendDemodObservation(MakeObservation(1, 101)));
    REQUIRE(writer->AppendDemodObservation(MakeDetailedObservation(2, 102)));
    REQUIRE(writer->Finalize());
    const auto writerSnapshot = writer->GetSnapshot();
    REQUIRE(writerSnapshot.complete);
    REQUIRE(writerSnapshot.captureFrames == 2);
    REQUIRE(writerSnapshot.demodObservations == 2);
    REQUIRE(writerSnapshot.recordsProcessed == 4);
    REQUIRE(writerSnapshot.fileBytes == std::filesystem::file_size(path));

    std::unique_ptr<pbrealcapturereplay::ReplayV2Reader> reader;
    REQUIRE(pbrealcapturereplay::ReplayV2Reader::Open(path, {}, reader));
    REQUIRE(reader != nullptr);
    REQUIRE(reader->GetSnapshot().descriptor == descriptor);

    pbrealcapturereplay::ReplayV2Record record;
    REQUIRE(reader->ReadNext(record));
    REQUIRE(record.type == pbrealcapturereplay::ReplayV2RecordType::Capture);
    REQUIRE(record.capture.capture.captureObservation == 1);
    REQUIRE(record.capture.capture.domain.captureEpoch == 9);
    REQUIRE(record.capture.capture.physicalRoi.left == 2560);
    REQUIRE(record.capture.capturedRoi.pixels == first.capturedPixels);
    REQUIRE_FALSE(record.capture.senderCanonicalRaster.has_value());
    REQUIRE(record.capture.canonicalBootstrap.empty());
    REQUIRE(record.capture.displayIdentityUtf8 == first.displayIdentity);

    REQUIRE(reader->ReadNext(record));
    REQUIRE(record.type == pbrealcapturereplay::ReplayV2RecordType::Capture);
    REQUIRE(record.capture.capture.captureObservation == 2);
    REQUIRE(record.capture.senderCanonicalRaster.has_value());
    REQUIRE(record.capture.senderCanonicalRaster->pixels == second.senderPixels);
    REQUIRE(record.capture.canonicalBootstrap == second.bootstrap);

    REQUIRE(reader->ReadNext(record));
    REQUIRE(record.type == pbrealcapturereplay::ReplayV2RecordType::DemodObservation);
    REQUIRE(record.demodObservation.captureObservation == 1);
    REQUIRE(record.demodObservation.frameSequence == 101);
    REQUIRE(record.demodObservation.receiverAdmitted);
    REQUIRE_FALSE(record.demodObservation.productionDetailAvailable);
    REQUIRE(record.demodObservation.resultKind == pbrealcapturereplay::ReplayV2DemodResultKind::Unavailable);

    REQUIRE(reader->ReadNext(record));
    REQUIRE(record.type == pbrealcapturereplay::ReplayV2RecordType::DemodObservation);
    REQUIRE(record.demodObservation.captureObservation == 2);
    REQUIRE(record.demodObservation.frameSequence == 102);
    REQUIRE(record.demodObservation.productionDetailAvailable);
    REQUIRE(record.demodObservation.layoutAvailable);
    REQUIRE(record.demodObservation.visualLayoutVersion == 7);
    REQUIRE(record.demodObservation.resultKind == pbrealcapturereplay::ReplayV2DemodResultKind::Transport);
    REQUIRE(record.demodObservation.geometryStatus == pbrealcapturereplay::ReplayV2GeometryStatus::Scaled);
    REQUIRE(record.demodObservation.geometryOriginX == 17.25);
    REQUIRE(record.demodObservation.geometryScaleX == 1.25);
    REQUIRE(record.demodObservation.temporalDisposition == pbrealcapturereplay::ReplayV2TemporalDisposition::Unique);
    REQUIRE(record.demodObservation.evaluationAvailable);
    REQUIRE(record.demodObservation.paddingValid);
    REQUIRE_FALSE(record.demodObservation.senderTruthAvailable);
    REQUIRE(record.demodObservation.codewords == 4);
    REQUIRE(record.demodObservation.fecFailures == 1);
    REQUIRE(record.demodObservation.acceptedTransportBlocks == 3);
    REQUIRE(record.demodObservation.admittedTransportBlocks == 2);
    REQUIRE(record.demodObservation.metricSummaryAvailable);
    REQUIRE(record.demodObservation.metricSamples == 64800);
    REQUIRE(record.demodObservation.minimumAbsoluteMetric == 0.125);
    REQUIRE(record.demodObservation.meanAbsoluteMetric == 1.75);
    REQUIRE(record.demodObservation.metricReadbackBytes == 261040);
    REQUIRE(record.demodObservation.gpuTimingAvailable);
    REQUIRE(record.demodObservation.gpuTime100ns == 12345);
    REQUIRE(record.demodObservation.carrierAccepted);
    REQUIRE(record.demodObservation.receiverStateAdvanced);
    const auto unchanged = record;
    REQUIRE(reader->ReadNext(record).code == pbrealcapturereplay::ReplayError::EndOfFile);
    REQUIRE(record.demodObservation.captureObservation == unchanged.demodObservation.captureObservation);
    const auto readerSnapshot = reader->GetSnapshot();
    REQUIRE(readerSnapshot.complete);
    REQUIRE(readerSnapshot.captureFrames == 2);
    REQUIRE(readerSnapshot.demodObservations == 2);
    REQUIRE(readerSnapshot.totalRasterBytes == 96);
}

TEST_CASE("Replay v2 enforces linkage, receiver-only presence, frame caps and no-overwrite publication",
    "[replay][v2][bounds][publish]")
{
    TemporaryReplayV2Files temporary;
    const auto path = temporary.Make(L"-bounds.pbrv2");
    const auto descriptor = MakeDescriptor();
    auto limits = pbrealcapturereplay::ReplayV2Limits{};
    limits.maximumCaptureFrames = 1;
    const auto first = MakeCapture(1, false);
    const auto second = MakeCapture(2, false);

    std::unique_ptr<pbrealcapturereplay::ReplayV2Writer> writer;
    REQUIRE(pbrealcapturereplay::ReplayV2Writer::Create(path, descriptor, limits, writer));
    REQUIRE(writer->AppendDemodObservation(MakeObservation(1, 101)).code ==
        pbrealcapturereplay::ReplayError::InvalidArgument);
    REQUIRE(writer->AppendCapture(first.View()));
    auto absentDetailWithValue = MakeObservation(1, 101);
    absentDetailWithValue.visualLayoutVersion = 7;
    REQUIRE(writer->AppendDemodObservation(absentDetailWithValue).code ==
        pbrealcapturereplay::ReplayError::InvalidArgument);
    auto invalidDetail = MakeDetailedObservation(1, 101);
    invalidDetail.metricSamples = 0;
    REQUIRE(writer->AppendDemodObservation(invalidDetail).code ==
        pbrealcapturereplay::ReplayError::InvalidArgument);
    invalidDetail = MakeDetailedObservation(1, 101);
    invalidDetail.comparedCodedBits = 1;
    REQUIRE(writer->AppendDemodObservation(invalidDetail).code ==
        pbrealcapturereplay::ReplayError::InvalidArgument);
    invalidDetail = MakeDetailedObservation(1, 101);
    invalidDetail.receiverStateAdvanced = false;
    REQUIRE(writer->AppendDemodObservation(invalidDetail).code ==
        pbrealcapturereplay::ReplayError::InvalidArgument);
    auto wrongEpoch = MakeObservation(1, 101);
    wrongEpoch.captureEpoch = 10;
    REQUIRE(writer->AppendDemodObservation(wrongEpoch).code == pbrealcapturereplay::ReplayError::InvalidArgument);
    REQUIRE(writer->AppendDemodObservation(MakeObservation(1, 101)));
    REQUIRE(writer->AppendDemodObservation(MakeObservation(1, 101)).code ==
        pbrealcapturereplay::ReplayError::InvalidArgument);
    REQUIRE(writer->AppendCapture(first.View()).code == pbrealcapturereplay::ReplayError::ResourceLimit);
    REQUIRE(writer->AppendCapture(second.View()).code == pbrealcapturereplay::ReplayError::ResourceLimit);
    REQUIRE(writer->Finalize());

    std::unique_ptr<pbrealcapturereplay::ReplayV2Writer> unchanged;
    REQUIRE(pbrealcapturereplay::ReplayV2Writer::Create(path, descriptor, limits, unchanged).code ==
        pbrealcapturereplay::ReplayError::AlreadyExists);
    REQUIRE(unchanged == nullptr);

    const auto incompletePath = temporary.Make(L"-incomplete.pbrv2");
    {
        std::unique_ptr<pbrealcapturereplay::ReplayV2Writer> incomplete;
        REQUIRE(pbrealcapturereplay::ReplayV2Writer::Create(incompletePath, descriptor, {}, incomplete));
        REQUIRE(incomplete->AppendCapture(first.View()));
        REQUIRE_FALSE(std::filesystem::exists(incompletePath));
        auto partial = incompletePath;
        partial += L".partial";
        REQUIRE(std::filesystem::exists(partial));
    }
    auto partial = incompletePath;
    partial += L".partial";
    REQUIRE_FALSE(std::filesystem::exists(incompletePath));
    REQUIRE_FALSE(std::filesystem::exists(partial));

    auto invalidLimits = pbrealcapturereplay::ReplayV2Limits{};
    invalidLimits.maximumFileBytes = pbrealcapturereplay::kReplayV2HardMaximumFileBytes + 1;
    REQUIRE(pbrealcapturereplay::ReplayV2Writer::Create(temporary.Make(L"-invalid-limit.pbrv2"), descriptor,
        invalidLimits, unchanged).code == pbrealcapturereplay::ReplayError::InvalidArgument);
    auto invalidDescriptor = descriptor;
    invalidDescriptor.runId = "ABCDEF0123456789ABCDEF0123456789";
    REQUIRE(pbrealcapturereplay::ReplayV2Writer::Create(temporary.Make(L"-invalid-id.pbrv2"), invalidDescriptor,
        {}, unchanged).code == pbrealcapturereplay::ReplayError::InvalidArgument);
    invalidDescriptor = descriptor;
    invalidDescriptor.remoteMetadataJsonUtf8 = std::string("\xC0\xAF", 2);
    REQUIRE(pbrealcapturereplay::ReplayV2Writer::Create(temporary.Make(L"-invalid-utf8.pbrv2"), invalidDescriptor,
        {}, unchanged).code == pbrealcapturereplay::ReplayError::InvalidArgument);
}

TEST_CASE("Replay v2 rejects metadata corruption, truncation and tighter read resource policies",
    "[replay][v2][corrupt][resource]")
{
    TemporaryReplayV2Files temporary;
    const auto sourcePath = temporary.Make(L"-source.pbrv2");
    WriteSingleCapture(sourcePath);

    const auto corruptPath = temporary.Make(L"-corrupt.pbrv2");
    std::filesystem::copy_file(sourcePath, corruptPath);
    FlipFileByte(corruptPath, pbrealcapturereplay::kReplayV2FileHeaderBytes + 3);
    std::unique_ptr<pbrealcapturereplay::ReplayV2Reader> reader;
    REQUIRE(pbrealcapturereplay::ReplayV2Reader::Open(corruptPath, {}, reader).code ==
        pbrealcapturereplay::ReplayError::DigestMismatch);

    const auto truncatedPath = temporary.Make(L"-truncated.pbrv2");
    std::filesystem::copy_file(sourcePath, truncatedPath);
    std::filesystem::resize_file(truncatedPath, std::filesystem::file_size(truncatedPath) - 1);
    const auto truncatedStatus = pbrealcapturereplay::ReplayV2Reader::Open(truncatedPath, {}, reader);
    REQUIRE((truncatedStatus.code == pbrealcapturereplay::ReplayError::MalformedHeader ||
        truncatedStatus.code == pbrealcapturereplay::ReplayError::ChecksumMismatch ||
        truncatedStatus.code == pbrealcapturereplay::ReplayError::TruncatedInput));

    auto fileLimit = pbrealcapturereplay::ReplayV2Limits{};
    fileLimit.maximumFileBytes = std::filesystem::file_size(sourcePath) - 1;
    fileLimit.maximumTotalRasterBytes = fileLimit.maximumFileBytes;
    REQUIRE(pbrealcapturereplay::ReplayV2Reader::Open(sourcePath, fileLimit, reader).code ==
        pbrealcapturereplay::ReplayError::ResourceLimit);

    auto rasterLimit = pbrealcapturereplay::ReplayV2Limits{};
    rasterLimit.maximumRasterBytesPerFrame = 16;
    REQUIRE(pbrealcapturereplay::ReplayV2Reader::Open(sourcePath, rasterLimit, reader));
    pbrealcapturereplay::ReplayV2Record unchanged;
    unchanged.ordinal = 77;
    REQUIRE(reader->ReadNext(unchanged).code == pbrealcapturereplay::ReplayError::ResourceLimit);
    REQUIRE(unchanged.ordinal == 77);
}
