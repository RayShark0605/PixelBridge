#include "replay_inspector_core.h"

#include "pbdesktoplevels/reference_channel.h"
#include "pbmodulation/desktop_levels.h"
#include "pbmodulation/local_desktop_bootstrap.h"
#include "pbmodulation/remote_visual.h"
#include "pbmodulation/remote_visual_low_fps.h"
#include "pbmodulation/shape_chroma.h"
#include "pbprotocol/bootstrap_control_codec.h"

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace
{

class TemporaryReplayFiles
{
public:
    TemporaryReplayFiles()
    {
        static std::atomic<std::uint64_t> nextId{1};
        root_ = std::filesystem::temp_directory_path() /
            (L"pixelbridge-replay-inspector-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(nextId.fetch_add(1)));
        std::filesystem::create_directories(root_);
    }

    ~TemporaryReplayFiles()
    {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    [[nodiscard]] std::filesystem::path Make(const std::wstring& name) const
    {
        return root_ / name;
    }

private:
    std::filesystem::path root_;
};

struct ProfileFixture
{
    std::uint64_t profileId = 0;
    std::uint8_t layoutVersion = 0;
    std::uint32_t dataBytes = 0;
    std::uint32_t codewords = 0;
    const wchar_t* fileName = nullptr;
    const char* profileName = nullptr;
};

constexpr std::uint64_t testSessionTag = 0x123456789ABCDEF0ULL;

std::array<std::byte, pbprotocol::kBootstrapRecordBytes> MakeBootstrap(
    const ProfileFixture& profile)
{
    pbprotocol::BootstrapRecord record;
    record.protocolVersion = pbprotocol::GetProtocolVersion();
    record.visualLayoutVersion = profile.layoutVersion;
    record.visualProfileId = profile.profileId;
    record.sessionTag.value = testSessionTag;
    record.frameSequence = 17;
    std::array<std::byte, pbprotocol::kBootstrapRecordBytes> output{};
    REQUIRE(pbprotocol::SerializeBootstrapRecord(record, output));
    return output;
}

std::vector<std::byte> MakeRaster(const ProfileFixture& profile,
    const std::span<const std::byte> bootstrap, const bool corruptFirstCodeword = false)
{
    std::vector<std::byte> logicalData(profile.dataBytes);
    REQUIRE(pbdesktoplevels::GenerateDiagnosticData(bootstrap, logicalData));
    if (corruptFirstCodeword)
    {
        REQUIRE(logicalData.size() >= pbdesktoplevels::kCodewordBytes);
        std::transform(logicalData.begin(), logicalData.begin() + pbdesktoplevels::kCodewordBytes,
            logicalData.begin(), [](const std::byte value) { return value ^ std::byte{0xFF}; });
    }
    std::vector<std::byte> raster(static_cast<std::size_t>(pbmodulation::kLocalDesktopCanvasWidth) *
        pbmodulation::kLocalDesktopCanvasHeight * 4);
    if (profile.profileId == pbmodulation::kDesktopLevels2ProfileId)
    {
        REQUIRE(pbmodulation::EncodeDesktopLevelsFrame(bootstrap, logicalData, raster));
    }
    else if (profile.profileId == pbmodulation::kShapeChromaProfileId)
    {
        REQUIRE(pbmodulation::EncodeShapeChromaFrame(bootstrap, logicalData, raster));
    }
    else if (profile.profileId == pbmodulation::kRemoteVisualProfileId)
    {
        REQUIRE(pbmodulation::EncodeRemoteVisualFrame(bootstrap, logicalData, raster));
    }
    else
    {
        REQUIRE(profile.profileId == pbmodulation::kRemoteVisualLowFpsProfileId);
        REQUIRE(pbmodulation::EncodeRemoteVisualLowFpsFrame(bootstrap, logicalData, raster));
    }
    return raster;
}

pbcapturenormalize::ScreenCaptureFrameMetadata MakeMetadata()
{
    pbcapturenormalize::ScreenCaptureFrameMetadata metadata;
    metadata.domain.sourceId[0] = std::byte{0xA5};
    metadata.domain.sourceId[15] = std::byte{0x5A};
    metadata.domain.captureEpoch = 3;
    metadata.backend = pbcapturenormalize::CaptureBackendKind::Wgc;
    metadata.captureObservation = 9;
    metadata.sourceGeneration = 2;
    metadata.slotGeneration = 7;
    metadata.slotIndex = 1;
    metadata.physicalRoi = {2880, 172, 4800, 1252};
    metadata.sourceContentSize = {2560, 1440};
    metadata.sourceExtent = {2560, 1440};
    metadata.roiSize = {static_cast<std::int32_t>(pbmodulation::kLocalDesktopCanvasWidth),
        static_cast<std::int32_t>(pbmodulation::kLocalDesktopCanvasHeight)};
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
    metadata.timestamp.rawValue = 10000000;
    metadata.timestamp.rawFrequency = 10000000;
    metadata.timestamp.monotonic100ns = 10000000;
    metadata.timestamp.arrivalQpc100ns = 10000010;
    metadata.roiCopyTime100ns = 55;
    metadata.isCursorExcluded = true;
    metadata.sourceCursorState = pbcapturenormalize::CursorState::Excluded;
    return metadata;
}

void WriteReplay(const std::filesystem::path& path, const ProfileFixture& profile,
    const bool includeObservation, const bool corruptFirstCodeword = false)
{
    const auto bootstrap = MakeBootstrap(profile);
    const std::vector<std::byte> raster = MakeRaster(profile, bootstrap, corruptFirstCodeword);
    pbrealcapturereplay::ReplayV2FileDescriptor descriptor;
    descriptor.datasetClass = pbrealcapturereplay::ReplayDatasetClass::RemoteVisual;
    descriptor.datasetId[0] = std::byte{0x52};
    descriptor.datasetId[15] = std::byte{0x19};
    descriptor.runId = "0123456789abcdef0123456789abcdef";
    descriptor.visualProfileId = profile.profileId;
    descriptor.createdUtc100ns = 133853184000000000LL;
    descriptor.remoteMetadataJsonUtf8 =
        R"({"schema":"PixelBridge.RemoteVisualRunMetadata.1","remoteProvider":"Fixture","notes":"receiver-only"})";
    std::unique_ptr<pbrealcapturereplay::ReplayV2Writer> writer;
    REQUIRE(pbrealcapturereplay::ReplayV2Writer::Create(path, descriptor, {}, writer));
    const auto metadata = MakeMetadata();
    const pbrealcapturereplay::ReplayRasterView capturedRoi{pbmodulation::kLocalDesktopCanvasWidth,
        pbmodulation::kLocalDesktopCanvasHeight, pbmodulation::kLocalDesktopCanvasWidth * 4,
        DXGI_FORMAT_B8G8R8A8_UNORM, raster};
    const pbrealcapturereplay::ReplayV2CaptureView capture{metadata, 96, 96, 1.0, 1.0,
        R"(\\.\DISPLAY2|FIXTURE-SERIAL-B)", capturedRoi, std::nullopt, {}};
    REQUIRE(writer->AppendCapture(capture));
    if (includeObservation)
    {
        pbrealcapturereplay::ReplayV2DemodObservationView observation;
        observation.captureEpoch = metadata.domain.captureEpoch;
        observation.captureObservation = metadata.captureObservation;
        observation.visualProfileId = profile.profileId;
        observation.disposition = pbrealcapturereplay::ReplayV2DemodDisposition::Accepted;
        observation.bootstrapAttempted = true;
        observation.bootstrapSucceeded = true;
        observation.frameSequenceAvailable = true;
        observation.frameSequence = 17;
        observation.transportProduced = true;
        observation.receiverAdmitted = true;
        REQUIRE(writer->AppendDemodObservation(observation));
    }
    REQUIRE(writer->Finalize());
}

std::vector<ProfileFixture> GetProfiles()
{
    const auto* const direct = pbmodulation::GetDesktopLevelsProfile(
        pbmodulation::kDesktopLevels2ProfileId);
    REQUIRE(direct != nullptr);
    return {
        {pbmodulation::kDesktopLevels2ProfileId, pbmodulation::kDesktopLevelsLayoutVersion,
            direct->dataBytes, direct->codewords, L"direct.pbrv2", "PB-Mod-DesktopLevels-2x2"},
        {pbmodulation::kShapeChromaProfileId, pbmodulation::kShapeChromaLayoutVersion,
            pbmodulation::kShapeChromaDataBytes, pbmodulation::kShapeChromaCodewords,
            L"shape.pbrv2", "PB-Mod-ShapeChroma-1"},
        {pbmodulation::kRemoteVisualProfileId, pbmodulation::kRemoteVisualLayoutVersion,
            pbmodulation::kRemoteVisualDataBytes, pbmodulation::kRemoteVisualCodewords,
            L"remote.pbrv2", "PB-RemoteVisual-Resilient-1"},
        {pbmodulation::kRemoteVisualLowFpsProfileId, pbmodulation::kRemoteVisualLowFpsLayoutVersion,
            pbmodulation::kRemoteVisualLowFpsDataBytes, pbmodulation::kRemoteVisualLowFpsCodewords,
            L"lf4.pbrv2", "PB-RemoteVisual-LF4-X1"}};
}

} // namespace

TEST_CASE("Replay inspector deterministically evaluates receiver-only captures for every established profile",
    "[tools][remote-visual][replay][receiver-only][transport][determinism]")
{
    TemporaryReplayFiles temporary;
    for (const auto& profile : GetProfiles())
    {
        CAPTURE(profile.profileName);
        const auto path = temporary.Make(profile.fileName);
        WriteReplay(path, profile, profile.profileId == pbmodulation::kRemoteVisualLowFpsProfileId);
        pbremotevisualreplayinspector::ReplayInspection first;
        pbremotevisualreplayinspector::ReplayInspection second;
        std::string error;
        REQUIRE(pbremotevisualreplayinspector::InspectReplay(path, {}, first, error));
        REQUIRE(error.empty());
        REQUIRE(pbremotevisualreplayinspector::InspectReplay(path, {}, second, error));
        REQUIRE(error.empty());
        REQUIRE(first.canonicalJson == second.canonicalJson);
        REQUIRE(first.captureFrames == 1);
        REQUIRE(first.bootstrapAcceptedFrames == 1);
        REQUIRE(first.modulationAcceptedFrames == 1);
        REQUIRE(first.transportAcceptedFrames == 1);
        REQUIRE(first.acceptedTransportBlocks == profile.codewords);
        REQUIRE(first.canonicalJson.starts_with(
            "{\"schema\":\"PixelBridge.RemoteVisualReplayInspection.1\",\"version\":1,"));
        REQUIRE(first.canonicalJson.find(profile.profileName) != std::string::npos);
        REQUIRE(first.canonicalJson.find("\"falseAcceptedCodewords\":null") != std::string::npos);
        REQUIRE(first.canonicalJson.find("\"senderTruthAvailable\":false") != std::string::npos);
        REQUIRE(first.canonicalJson.find("\"finalFileDisposition\":\"NotEvaluated\"") != std::string::npos);
    }
}

TEST_CASE("Replay inspector fails closed on unsupported profile identity without mutating output",
    "[tools][remote-visual][replay][profile][negative]")
{
    TemporaryReplayFiles temporary;
    auto profile = GetProfiles().front();
    profile.profileId = 0xDEADBEEF12345678ULL;
    profile.layoutVersion = 99;
    const auto path = temporary.Make(L"unknown.pbrv2");
    const std::vector<std::byte> pixels(static_cast<std::size_t>(pbmodulation::kLocalDesktopCanvasWidth) *
        pbmodulation::kLocalDesktopCanvasHeight * 4, std::byte{0});
    pbrealcapturereplay::ReplayV2FileDescriptor descriptor;
    descriptor.datasetClass = pbrealcapturereplay::ReplayDatasetClass::RemoteVisual;
    descriptor.datasetId[0] = std::byte{1};
    descriptor.runId = "fedcba9876543210fedcba9876543210";
    descriptor.visualProfileId = profile.profileId;
    descriptor.createdUtc100ns = 133853184000000000LL;
    descriptor.remoteMetadataJsonUtf8 =
        R"({"schema":"PixelBridge.RemoteVisualRunMetadata.1","remoteProvider":"Fixture","notes":"unknown-profile"})";
    std::unique_ptr<pbrealcapturereplay::ReplayV2Writer> writer;
    REQUIRE(pbrealcapturereplay::ReplayV2Writer::Create(path, descriptor, {}, writer));
    const auto metadata = MakeMetadata();
    const pbrealcapturereplay::ReplayRasterView raster{pbmodulation::kLocalDesktopCanvasWidth,
        pbmodulation::kLocalDesktopCanvasHeight, pbmodulation::kLocalDesktopCanvasWidth * 4,
        DXGI_FORMAT_B8G8R8A8_UNORM, pixels};
    REQUIRE(writer->AppendCapture({metadata, 96, 96, 1.0, 1.0, "DISPLAY2", raster, std::nullopt, {}}));
    REQUIRE(writer->Finalize());

    pbremotevisualreplayinspector::ReplayInspection output;
    output.captureFrames = 77;
    output.canonicalJson = "sentinel";
    std::string error;
    REQUIRE_FALSE(pbremotevisualreplayinspector::InspectReplay(path, {}, output, error));
    REQUIRE(error.find("VisualProfileId") != std::string::npos);
    REQUIRE(output.captureFrames == 77);
    REQUIRE(output.canonicalJson == "sentinel");
}

TEST_CASE("Replay inspector does not classify a partially accepted Transport frame as accepted",
    "[tools][remote-visual][replay][transport][negative]")
{
    TemporaryReplayFiles temporary;
    const auto profile = GetProfiles().front();
    const auto path = temporary.Make(L"partial-transport.pbrv2");
    WriteReplay(path, profile, false, true);

    pbremotevisualreplayinspector::ReplayInspection output;
    std::string error;
    REQUIRE(pbremotevisualreplayinspector::InspectReplay(path, {}, output, error));
    REQUIRE(error.empty());
    REQUIRE(output.captureFrames == 1);
    REQUIRE(output.bootstrapAcceptedFrames == 1);
    REQUIRE(output.modulationAcceptedFrames == 1);
    REQUIRE(output.transportAcceptedFrames == 0);
    REQUIRE(output.acceptedTransportBlocks > 0);
    REQUIRE(output.acceptedTransportBlocks < profile.codewords);
}
