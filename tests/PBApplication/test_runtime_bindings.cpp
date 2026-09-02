#include "local_desktop_runtime.h"
#include "monitor_catalog.h"
#include "pbmodulation/desktop_levels.h"
#include "pbmodulation/remote_visual_low_fps.h"
#include "pbmodulation/shape_chroma.h"
#include "pbrealcapturereplay/replay_v2.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace
{

class ScratchDirectory
{
public:
    explicit ScratchDirectory(const wchar_t* name)
    {
        path_ = std::filesystem::path(PB_TEST_SCRATCH_ROOT) / name;
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        error.clear();
        REQUIRE(std::filesystem::create_directories(path_, error));
        REQUIRE_FALSE(error);
    }
    ~ScratchDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    [[nodiscard]] const std::filesystem::path& Path() const noexcept
    {
        return path_;
    }
private:
    std::filesystem::path path_;
};

[[nodiscard]] pbapp::MonitorInfo MakeMonitor(const std::uintptr_t handle, const wchar_t* deviceName,
    const RECT rect, const bool primary)
{
    pbapp::MonitorInfo monitor;
    monitor.monitor = reinterpret_cast<HMONITOR>(handle);
    monitor.deviceName = deviceName;
    monitor.physicalRect = rect;
    monitor.workRect = rect;
    monitor.dpiX = 96;
    monitor.dpiY = 96;
    monitor.refreshRate = 60;
    monitor.rotation = DXGI_MODE_ROTATION_IDENTITY;
    monitor.adapterLuid = {static_cast<DWORD>(handle), 0};
    monitor.primary = primary;
    return monitor;
}

} // namespace

TEST_CASE("Compression disabled binds byte-identical RAW without invoking a new format", "[application][compression]")
{
    std::vector<std::byte> bytes(4096);
    for (std::size_t index = 0; index < bytes.size(); index++)
    {
        bytes[index] = static_cast<std::byte>((index * 71U) & 0xFFU);
    }
    auto result = pbapp::PrepareEncodedSegment(bytes, false, 3);
    REQUIRE(result);
    REQUIRE(result.Value().codec == pbprotocol::CompressionCodec::Raw);
    REQUIRE(result.Value().bytes == bytes);
}

TEST_CASE("Compression enabled uses existing zstd and RAW fallback binding", "[application][compression]")
{
    const std::vector<std::byte> compressible(64U * 1024U, std::byte{0x41});
    auto compressed = pbapp::PrepareEncodedSegment(compressible, true, 3);
    REQUIRE(compressed);
    REQUIRE(compressed.Value().codec == pbprotocol::CompressionCodec::Zstandard);
    REQUIRE(compressed.Value().bytes.size() < compressible.size());

    const std::vector<std::byte> tiny{std::byte{0x01}};
    auto fallback = pbapp::PrepareEncodedSegment(tiny, true, 3);
    REQUIRE(fallback);
    REQUIRE(fallback.Value().codec == pbprotocol::CompressionCodec::Raw);
    REQUIRE(fallback.Value().bytes == tiny);
}

TEST_CASE("Encoder validation rejects missing empty and over-limit files without truncation", "[application][validation]")
{
    pbapp::EncoderConfig config;
    REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
    ScratchDirectory scratch(L"encoder-validation");
    const auto empty = scratch.Path() / L"empty.bin";
    std::ofstream(empty, std::ios::binary).close();
    config.sourcePath = empty.wstring();
    config.monitorClientOrigin = pbrenderd3d::PhysicalPoint{};
    REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
    const auto tooLarge = scratch.Path() / L"large.bin";
    const HANDLE file = CreateFileW(tooLarge.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(file != INVALID_HANDLE_VALUE);
    LARGE_INTEGER size{};
    size.QuadPart = static_cast<LONGLONG>(pbapp::maximumInstantFileBytes + 1);
    REQUIRE(SetFilePointerEx(file, size, nullptr, FILE_BEGIN));
    REQUIRE(SetEndOfFile(file));
    REQUIRE(CloseHandle(file));
    config.sourcePath = tooLarge.wstring();
    REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
}

TEST_CASE("Encoder validation accepts only inventory profiles with an explicit monitor origin",
    "[application][validation][profile]")
{
    ScratchDirectory scratch(L"encoder-valid-profile");
    const auto source = scratch.Path() / L"one-byte.bin";
    std::ofstream output(source, std::ios::binary);
    output.put('x');
    output.close();
    pbapp::EncoderConfig config;
    config.sourcePath = source.wstring();
    REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
    config.monitorClientOrigin = pbrenderd3d::PhysicalPoint{};
    REQUIRE(pbapp::ValidateEncoderConfig(config));
    config.logicalVisualFps = 240;
    REQUIRE(pbapp::ValidateEncoderConfig(config));
    config.visualProfile = pbapp::VisualProfile::RemoteVisualResilient;
    config.logicalVisualFps = 5;
    config.controlRepetitions = 12;
    config.runId = "0123456789abcdef0123456789abcdef";
    REQUIRE(pbapp::ValidateEncoderConfig(config));
    config.logicalVisualFps = 0;
    REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
    config.logicalVisualFps = 6;
    REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
    config.logicalVisualFps = 5;
    config.controlRepetitions = 0;
    REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
    config.controlRepetitions = 12;
    config.visualProfile = pbapp::VisualProfile::RemoteVisualLowFps;
    config.remoteMetadata.channelType = pbapp::ChannelType::RemoteVisual;
    config.remoteMetadata.remoteProvider = "TestRemote";
    config.remoteMetadata.protectedMonitorIdentity = R"(\\.\DISPLAY1)";
    config.remoteMetadata.experimentMonitorIdentity = R"(\\.\DISPLAY2)";
    config.monitorSafety = pbapp::MonitorSafetySelection{
        MakeMonitor(1, L"\\\\.\\DISPLAY1", {-2560, 0, 0, 1440}, true),
        MakeMonitor(2, L"\\\\.\\DISPLAY2", {0, 0, 2560, 1440}, false)};
    REQUIRE(pbapp::ValidateEncoderConfig(config));
    config.runId = "0123456789ABCDEF0123456789ABCDEF";
    REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
    config.runId.clear();
    config.visualProfile = static_cast<pbapp::VisualProfile>(255);
    REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
}

TEST_CASE("remote-lf4 Encoder validation binds channel monitor identities and a contained physical Data Window",
    "[application][validation][remote-lf4][encoder][monitor]")
{
    ScratchDirectory scratch(L"encoder-lf4-monitor-validation");
    const auto source = scratch.Path() / L"source.bin";
    std::ofstream(source, std::ios::binary).put('x');
    pbapp::EncoderConfig config;
    config.sourcePath = source.wstring();
    config.visualProfile = pbapp::VisualProfile::RemoteVisualLowFps;
    config.logicalVisualFps = 2;
    config.controlRepetitions = 12;
    config.monitorClientOrigin = pbrenderd3d::PhysicalPoint{320, 180};
    config.remoteMetadata.channelType = pbapp::ChannelType::RemoteVisual;
    config.remoteMetadata.remoteProvider = "TestRemote";
    config.remoteMetadata.protectedMonitorIdentity = R"(\\.\DISPLAY1)";
    config.remoteMetadata.experimentMonitorIdentity = R"(\\.\DISPLAY2)";
    config.monitorSafety = pbapp::MonitorSafetySelection{
        MakeMonitor(1, L"\\\\.\\DISPLAY1", {-2560, 0, 0, 1440}, true),
        MakeMonitor(2, L"\\\\.\\DISPLAY2", {0, 0, 2560, 1440}, false)};
    REQUIRE(pbapp::ValidateEncoderConfig(config));

    config.remoteMetadata.channelType = pbapp::ChannelType::LocalDesktop;
    REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
    config.remoteMetadata.channelType = pbapp::ChannelType::RemoteVisual;
    const auto safety = config.monitorSafety;
    config.monitorSafety.reset();
    REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
    config.monitorSafety = safety;
    config.remoteMetadata.experimentMonitorIdentity = R"(\\.\DISPLAY3)";
    REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
    config.remoteMetadata.experimentMonitorIdentity = R"(\\.\DISPLAY2)";
    config.monitorClientOrigin = pbrenderd3d::PhysicalPoint{-2240, 180};
    REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
    config.monitorClientOrigin = pbrenderd3d::PhysicalPoint{320, 180};
    config.monitorSafety->protectedMonitor.physicalRect = {-100, 0, 100, 1440};
    REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
    config.monitorSafety = safety;
    config.monitorSafety->experimentMonitor.rotation = DXGI_MODE_ROTATION_ROTATE90;
    REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
    config.monitorSafety = safety;
    config.monitorClientOrigin = pbrenderd3d::PhysicalPoint{(std::numeric_limits<std::int32_t>::max)() - 100, 180};
    REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
}

TEST_CASE("remote-lf4 single-monitor fullscreen requires explicit exact bounded sender authority",
    "[application][validation][remote-lf4][encoder][single-monitor][fullscreen]")
{
    ScratchDirectory scratch(L"encoder-lf4-single-monitor-fullscreen");
    const auto source = scratch.Path() / L"source.bin";
    std::ofstream(source, std::ios::binary).put('x');
    pbapp::EncoderConfig config;
    config.sourcePath = source.wstring();
    config.visualProfile = pbapp::VisualProfile::RemoteVisualLowFps;
    config.logicalVisualFps = 2;
    config.controlRepetitions = 12;
    config.monitorClientOrigin = pbrenderd3d::PhysicalPoint{0, 0};
    config.remoteMetadata.channelType = pbapp::ChannelType::RemoteVisual;
    config.remoteMetadata.remoteProvider = "UserProvidedFullscreenLink";
    config.remoteMetadata.experimentMonitorIdentity = R"(\\.\DISPLAY1)";
    config.singleMonitorFullscreen = MakeMonitor(1, L"\\\\.\\DISPLAY1", {0, 0, 2560, 1600}, true);
    REQUIRE(pbapp::ValidateEncoderConfig(config));

    config.monitorSafety = pbapp::MonitorSafetySelection{
        MakeMonitor(2, L"\\\\.\\DISPLAY2", {-1920, 0, 0, 1080}, false), *config.singleMonitorFullscreen};
    REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
    config.monitorSafety.reset();
    config.remoteMetadata.protectedMonitorIdentity = R"(\\.\DISPLAY2)";
    REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
    config.remoteMetadata.protectedMonitorIdentity.clear();
    config.remoteMetadata.experimentMonitorIdentity = R"(\\.\DISPLAY3)";
    REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
    config.remoteMetadata.experimentMonitorIdentity = R"(\\.\DISPLAY1)";
    config.monitorClientOrigin = pbrenderd3d::PhysicalPoint{1, 0};
    REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
    config.monitorClientOrigin = pbrenderd3d::PhysicalPoint{0, 0};
    config.singleMonitorFullscreen->rotation = DXGI_MODE_ROTATION_ROTATE90;
    REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
    config.singleMonitorFullscreen->rotation = DXGI_MODE_ROTATION_IDENTITY;
    config.singleMonitorFullscreen->physicalRect = {0, 0, 1600, 900};
    REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
    config.singleMonitorFullscreen->physicalRect = {0, 0, 3841, 1600};
    REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
    config.singleMonitorFullscreen->physicalRect = {0, 0, 2560, 1600};
    config.visualProfile = pbapp::VisualProfile::RemoteVisualResilient;
    REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
}

TEST_CASE("remote-lf4 fullscreen composition centers an exact canvas inside non-16:9 surfaces",
    "[application][remote-lf4][encoder][single-monitor][fullscreen][composition]")
{
    constexpr std::size_t sourceBytes =
        static_cast<std::size_t>(pbapp::phase1CanvasWidth) * pbapp::phase1CanvasHeight * 4U;
    std::vector<std::byte> source(sourceBytes, std::byte{0});
    source[0] = std::byte{0x11};
    source[1] = std::byte{0x22};
    source[2] = std::byte{0x33};
    source[3] = std::byte{0x44};
    source[source.size() - 4U] = std::byte{0x55};
    source[source.size() - 3U] = std::byte{0x66};
    source[source.size() - 2U] = std::byte{0x77};
    source[source.size() - 1U] = std::byte{0x88};

    constexpr std::uint32_t destinationWidth = 2560;
    constexpr std::uint32_t destinationHeight = 1600;
    std::vector<std::byte> destination;
    REQUIRE(pbapp::EncoderRuntimeTestAccess::ProbeRemoteVisualFullscreenComposition(
        source, destinationWidth, destinationHeight, destination));
    REQUIRE(destination.size() == static_cast<std::size_t>(destinationWidth) * destinationHeight * 4U);
    const auto ReadPixel = [&destination](const std::uint32_t width, const std::uint32_t x, const std::uint32_t y)
    {
        const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4U;
        return std::array{destination[offset], destination[offset + 1U],
            destination[offset + 2U], destination[offset + 3U]};
    };
    const std::array neutral{std::byte{0x80}, std::byte{0x80}, std::byte{0x80}, std::byte{0xFF}};
    const std::array firstSource{std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}};
    const std::array lastSource{std::byte{0x55}, std::byte{0x66}, std::byte{0x77}, std::byte{0x88}};
    constexpr std::uint32_t canvasLeft = 320;
    constexpr std::uint32_t canvasTop = 260;
    REQUIRE(ReadPixel(destinationWidth, 0, 0) == neutral);
    REQUIRE(ReadPixel(destinationWidth, canvasLeft - 1U, canvasTop) == neutral);
    REQUIRE(ReadPixel(destinationWidth, canvasLeft, canvasTop - 1U) == neutral);
    REQUIRE(ReadPixel(destinationWidth, canvasLeft, canvasTop) == firstSource);
    REQUIRE(ReadPixel(destinationWidth, canvasLeft + pbapp::phase1CanvasWidth - 1U,
        canvasTop + pbapp::phase1CanvasHeight - 1U) == lastSource);
    REQUIRE(ReadPixel(destinationWidth, canvasLeft + pbapp::phase1CanvasWidth, canvasTop) == neutral);
    REQUIRE(ReadPixel(destinationWidth, canvasLeft, canvasTop + pbapp::phase1CanvasHeight) == neutral);
    REQUIRE(ReadPixel(destinationWidth, destinationWidth - 1U, destinationHeight - 1U) == neutral);

    std::vector<std::byte> rejected{std::byte{0xA5}};
    REQUIRE_FALSE(pbapp::EncoderRuntimeTestAccess::ProbeRemoteVisualFullscreenComposition(
        source, pbapp::phase1CanvasWidth - 1U, pbapp::phase1CanvasHeight, rejected));
    REQUIRE(rejected == std::vector<std::byte>{std::byte{0xA5}});
    REQUIRE_FALSE(pbapp::EncoderRuntimeTestAccess::ProbeRemoteVisualFullscreenComposition(
        source, pbapp::phase1CanvasWidth * 2U + 1U, pbapp::phase1CanvasHeight, rejected));
    REQUIRE(rejected == std::vector<std::byte>{std::byte{0xA5}});
}

TEST_CASE("Production RemoteVisual sender builds LF4 four-codeword carousels past an external completion marker",
    "[application][encoder][remote-visual][lf4][carousel]")
{
    const std::array<std::byte, 1> source{std::byte{0x5A}};
    pbapp::EncoderCarouselProbeSnapshot probe;
    REQUIRE(pbapp::EncoderRuntimeTestAccess::ProbeRemoteVisualLowFpsCarousel(source, 1, 2, probe));
    REQUIRE(probe.visualProfileId == pbmodulation::kRemoteVisualLowFpsProfileId);
    REQUIRE(probe.layoutVersion == pbmodulation::kRemoteVisualLowFpsLayoutVersion);
    REQUIRE(probe.codedDataBytes == pbmodulation::kRemoteVisualLowFpsDataBytes);
    REQUIRE(probe.codewords == pbmodulation::kRemoteVisualLowFpsCodewords);
    REQUIRE(probe.cycleFrameCount == 4);
    REQUIRE(probe.completedCarouselCycles == 2);
    REQUIRE(probe.framesBuilt == 9);
    REQUIRE(probe.controlFrames == 7);
    REQUIRE(probe.dataFrames == 2);
    REQUIRE(probe.acceptedRemoteControlCopies == 28);
    REQUIRE(probe.acceptedTransportBlocks == 8);
    REQUIRE(probe.framesBuiltAfterExternalCompletionMarker == 1);

    pbapp::EncoderCarouselProbeSnapshot unchanged;
    unchanged.framesBuilt = 91;
    REQUIRE_FALSE(pbapp::EncoderRuntimeTestAccess::ProbeRemoteVisualLowFpsCarousel({}, 1, 2, unchanged));
    REQUIRE(unchanged.framesBuilt == 91);
    REQUIRE_FALSE(pbapp::EncoderRuntimeTestAccess::ProbeRemoteVisualLowFpsCarousel(source, 0, 2, unchanged));
    REQUIRE_FALSE(pbapp::EncoderRuntimeTestAccess::ProbeRemoteVisualLowFpsCarousel(source, 1, 0, unchanged));
}

TEST_CASE("Production LF4 Receiver admits bounded partial codewords and preserves digest-gated publish",
    "[application][decoder][remote-visual][lf4][temporal][receiver][outer][publish]")
{
    ScratchDirectory scratch(L"lf4-production-receiver");
    const auto ReadPublished = [](const std::string& utf8Path)
    {
        const std::filesystem::path path(utf8Path);
        std::ifstream input(path, std::ios::binary);
        REQUIRE(input.good());
        const std::vector<char> raw((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        std::vector<std::byte> bytes(raw.size());
        for (std::size_t index = 0; index < raw.size(); index++)
        {
            bytes[index] = static_cast<std::byte>(static_cast<unsigned char>(raw[index]));
        }
        return bytes;
    };
    const auto Run = [&](const std::span<const std::byte> source, const wchar_t* directoryName,
        const pbprotocol::OuterFecMode expectedMode, const std::uint64_t expectedOuterUnique)
    {
        const auto directory = scratch.Path() / directoryName;
        REQUIRE(std::filesystem::create_directories(directory));
        pbapp::DecoderAdmissionProbeSnapshot probe;
        const auto status = pbapp::DecoderRuntimeTestAccess::ProbeRemoteVisualLowFpsReceiver(source,
            directory.wstring(), 32, probe);
        INFO(status.message);
        REQUIRE(status);
        CHECK(probe.outerFecMode == expectedMode);
        CHECK(probe.processedResults == 37);
        CHECK(probe.suppressedDuplicateResults == 32);
        CHECK(probe.duplicateRefinementResults == 1);
        CHECK(probe.rawAcceptedTransportBlocks == 6);
        CHECK(probe.temporallyAdmittedTransportBlocks == 4);
        CHECK(probe.decoder.state == pbapp::DecoderState::Completed);
        CHECK(probe.decoder.visualProfile == pbapp::VisualProfile::RemoteVisualLowFps);
        CHECK(probe.decoder.wholeFileDigestVerified);
        CHECK(probe.decoder.finalPublishSucceeded);
        CHECK(probe.decoder.originalFileBytes == source.size());
        CHECK(probe.decoder.verifiedRawBytes == source.size());
        CHECK(probe.decoder.remainingRawBytes == 0);
        CHECK(probe.decoder.acceptedTransportBlocks == 4);
        CHECK(probe.decoder.outerUniqueSymbols == expectedOuterUnique);
        CHECK(probe.decoder.outerConflictRejections == 0);
        CHECK(probe.decoder.outerResourceRejections == 0);
        CHECK(probe.decoder.duplicateFrameSequences == 33);
        CHECK(probe.decoder.endToEndUniqueFrameSequences == 1);
        CHECK(probe.decoder.evaluatedDataFrames == 1);
        CHECK(probe.decoder.postFecFailedFrames == 1);
        const auto& geometry = probe.decoder.observedLocatorGeometry;
        CHECK(geometry.samples == probe.decoder.telemetryBootstrapSuccesses);
        CHECK(geometry.samples == probe.processedResults);
        REQUIRE(geometry.lastScaleX);
        REQUIRE(geometry.lastScaleY);
        REQUIRE(geometry.minimumScaleX);
        REQUIRE(geometry.maximumScaleX);
        REQUIRE(geometry.minimumMarkerResidualPixels);
        REQUIRE(geometry.maximumMarkerResidualPixels);
        CHECK(*geometry.lastScaleX == Catch::Approx(1.0));
        CHECK(*geometry.lastScaleY == Catch::Approx(1.0));
        CHECK(*geometry.minimumScaleX <= *geometry.lastScaleX);
        CHECK(*geometry.maximumScaleX >= *geometry.lastScaleX);
        CHECK(*geometry.minimumMarkerResidualPixels <= *geometry.maximumMarkerResidualPixels);
        CHECK(ReadPublished(probe.decoder.outputPath) == std::vector<std::byte>(source.begin(), source.end()));
    };

    const std::array<std::byte, 1> directSource{std::byte{0x5A}};
    Run(directSource, L"direct-repeat", pbprotocol::OuterFecMode::DirectRepeat, 1);
    std::vector<std::byte> wirehairSource(4096);
    for (std::size_t index = 0; index < wirehairSource.size(); index++)
    {
        wirehairSource[index] = static_cast<std::byte>((index * 73 + index / 7 + 19) & 0xFF);
    }
    Run(wirehairSource, L"wirehair", pbprotocol::OuterFecMode::WirehairV2, 4);

    pbapp::DecoderAdmissionProbeSnapshot unchanged;
    unchanged.processedResults = 91;
    REQUIRE_FALSE(pbapp::DecoderRuntimeTestAccess::ProbeRemoteVisualLowFpsReceiver({},
        scratch.Path().wstring(), 32, unchanged));
    CHECK(unchanged.processedResults == 91);
    REQUIRE_FALSE(pbapp::DecoderRuntimeTestAccess::ProbeRemoteVisualLowFpsReceiver(directSource,
        scratch.Path().wstring(), 0, unchanged));
}

TEST_CASE("Decoder validation rejects invalid output directory ROI scaling and rotation", "[application][validation][roi]")
{
    pbapp::DecoderConfig config;
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    ScratchDirectory scratch(L"decoder-validation");
    config.outputDirectory = scratch.Path().wstring();
    config.region.monitor = reinterpret_cast<HMONITOR>(1);
    config.region.physicalRect = {0, 0, 1919, 1080};
    config.region.monitorPhysicalRect = {0, 0, 1920, 1080};
    config.region.dpiX = 96;
    config.region.dpiY = 96;
    config.region.rotation = DXGI_MODE_ROTATION_IDENTITY;
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.region.physicalRect.right = 1920;
    config.region.rotation = DXGI_MODE_ROTATION_ROTATE90;
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.region.rotation = DXGI_MODE_ROTATION_IDENTITY;
    REQUIRE(pbapp::ValidateDecoderConfig(config));
    config.visualProfile = pbapp::VisualProfile::RemoteVisualResilient;
    config.runId = "fedcba9876543210fedcba9876543210";
    REQUIRE(pbapp::ValidateDecoderConfig(config));
    config.visualProfile = pbapp::VisualProfile::RemoteVisualLowFps;
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.visualProfile = pbapp::VisualProfile::RemoteVisualResilient;
    config.runId = "short";
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.runId.clear();
    config.captureBackend = pbapp::CaptureBackend::Dxgi;
    REQUIRE(pbapp::ValidateDecoderConfig(config));
    config.region.physicalRect.left = -1;
    config.region.physicalRect.right = 1919;
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.region.physicalRect = {0, 0, 1920, 1080};
    config.captureBackend = static_cast<pbapp::CaptureBackend>(255);
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
}

TEST_CASE("remote-lf4 live Decoder admits only bounded physical scale and explicit monitor safety",
    "[application][validation][remote-lf4][decoder][geometry][monitor]")
{
    ScratchDirectory scratch(L"decoder-lf4-monitor-validation");
    pbapp::DecoderConfig config;
    config.outputDirectory = scratch.Path().wstring();
    config.captureBackend = pbapp::CaptureBackend::Wgc;
    config.visualProfile = pbapp::VisualProfile::RemoteVisualLowFps;
    config.region.monitor = reinterpret_cast<HMONITOR>(2);
    config.region.physicalRect = {320, 180, 2240, 1260};
    config.region.monitorPhysicalRect = {0, 0, 5120, 2880};
    config.region.dpiX = 96;
    config.region.dpiY = 96;
    config.region.rotation = DXGI_MODE_ROTATION_IDENTITY;
    config.remoteMetadata.channelType = pbapp::ChannelType::RemoteVisual;
    config.remoteMetadata.remoteProvider = "TestRemote";
    config.remoteMetadata.protectedMonitorIdentity = R"(\\.\DISPLAY1)";
    config.remoteMetadata.experimentMonitorIdentity = R"(\\.\DISPLAY2)";
    config.monitorSafety = pbapp::MonitorSafetySelection{
        MakeMonitor(1, L"\\\\.\\DISPLAY1", {-2560, 0, 0, 1440}, true),
        MakeMonitor(2, L"\\\\.\\DISPLAY2", {0, 0, 5120, 2880}, false)};
    REQUIRE(pbapp::ValidateDecoderConfig(config));

    config.region.physicalRect = {320, 180, 1280, 720};
    REQUIRE(pbapp::ValidateDecoderConfig(config));
    config.region.physicalRect = {320, 180, 4160, 2340};
    REQUIRE(pbapp::ValidateDecoderConfig(config));
    config.region.physicalRect = {320, 180, 1279, 720};
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.region.physicalRect = {320, 180, 4161, 2340};
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.region.physicalRect = {320, 180, 2240, 1260};

    config.remoteMetadata.channelType = pbapp::ChannelType::LocalDesktop;
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.remoteMetadata.channelType = pbapp::ChannelType::RemoteVisual;
    const auto safety = config.monitorSafety;
    config.monitorSafety.reset();
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.monitorSafety = safety;
    config.remoteMetadata.protectedMonitorIdentity = R"(\\.\DISPLAY9)";
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.remoteMetadata.protectedMonitorIdentity = R"(\\.\DISPLAY1)";
    config.region.rotation = DXGI_MODE_ROTATION_ROTATE90;
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.region.rotation = DXGI_MODE_ROTATION_IDENTITY;

    config.replayOutputPath = (scratch.Path() / L"lf4-production.pbrv2").wstring();
    REQUIRE(pbapp::ValidateDecoderConfig(config));
    config.region.physicalRect = {320, 180, 4160, 2340};
    const std::uint64_t maximumScaleRecorderPixels = 3840ULL * 2160 * 4 * 3;
    config.replayMaximumFileBytes = maximumScaleRecorderPixels - 1;
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.replayMaximumFileBytes = maximumScaleRecorderPixels;
    REQUIRE(pbapp::ValidateDecoderConfig(config));
    config.replayMaximumFileBytes = pbrealcapturereplay::kReplayV2DefaultMaximumFileBytes;
    config.region.physicalRect = {320, 180, 2240, 1260};
    config.replayMaximumCaptureFramesPerSecond = 10;
    REQUIRE(pbapp::ValidateDecoderConfig(config));
    config.replayMaximumCaptureFramesPerSecond = 60;
    REQUIRE(pbapp::ValidateDecoderConfig(config));
    config.replayMaximumCaptureFramesPerSecond = 61;
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.replayMaximumCaptureFramesPerSecond = 10;
    config.diagnosticCaptureOnly = true;
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.diagnosticCaptureOnly = false;
    config.replayOutputPath.clear();
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
}

TEST_CASE("Decoder offline Replay v2 validation is bounded and independent from live monitor geometry",
    "[application][validation][replay][offline]")
{
    ScratchDirectory scratch(L"decoder-offline-replay-validation");
    const auto replayPath = scratch.Path() / L"input.pbrv2";
    {
        std::ofstream output(replayPath, std::ios::binary);
        REQUIRE(output.good());
        output.seekp(static_cast<std::streamoff>(pbrealcapturereplay::kReplayV2FileHeaderBytes +
            pbrealcapturereplay::kReplayV2FileFooterBytes - 1));
        output.put('\0');
    }
    pbapp::DecoderConfig config;
    config.outputDirectory = scratch.Path().wstring();
    config.visualProfile = pbapp::VisualProfile::RemoteVisualResilient;
    config.replayInputPath = replayPath.wstring();
    config.replayMaximumCaptureFrames = 1;
    config.replayMaximumFileBytes = 16ULL * 1024 * 1024;
    REQUIRE(pbapp::ValidateDecoderConfig(config));
    config.replayMaximumCaptureFramesPerSecond = 10;
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.replayMaximumCaptureFramesPerSecond = 0;

    config.replayOutputPath = (scratch.Path() / L"output.pbrv2").wstring();
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.replayOutputPath.clear();
    config.visualProfile = pbapp::VisualProfile::DirectLevels2x2;
    REQUIRE(pbapp::ValidateDecoderConfig(config));
    config.visualProfile = pbapp::VisualProfile::ShapeChroma;
    REQUIRE(pbapp::ValidateDecoderConfig(config));
    config.visualProfile = pbapp::VisualProfile::RemoteVisualLowFps;
    REQUIRE(pbapp::ValidateDecoderConfig(config));
    config.visualProfile = static_cast<pbapp::VisualProfile>(255);
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.visualProfile = pbapp::VisualProfile::RemoteVisualResilient;
    config.replayMaximumCaptureFrames = 0;
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.replayMaximumCaptureFrames = 1;
    config.replayMaximumFileBytes = 15ULL * 1024 * 1024;
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.replayMaximumFileBytes = 16ULL * 1024 * 1024;
    config.replayInputPath = (scratch.Path() / L"missing.pbrv2").wstring();
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
}

TEST_CASE("RemoteVisual metadata validation is bounded finite UTF-8 and provider explicit",
    "[application][validation][remote-metadata]")
{
    ScratchDirectory scratch(L"remote-metadata-validation");
    pbapp::DecoderConfig config;
    config.outputDirectory = scratch.Path().wstring();
    config.visualProfile = pbapp::VisualProfile::RemoteVisualResilient;
    config.region.monitor = reinterpret_cast<HMONITOR>(1);
    config.region.physicalRect = {2560, 0, 4480, 1080};
    config.region.monitorPhysicalRect = {2560, 0, 5120, 1440};
    config.region.dpiX = 96;
    config.region.dpiY = 96;
    config.region.rotation = DXGI_MODE_ROTATION_IDENTITY;
    config.remoteMetadata.channelType = pbapp::ChannelType::RemoteVisual;
    config.remoteMetadata.remoteProvider = "GenericRemote";
    config.remoteMetadata.targetFps = 60.0;
    config.remoteMetadata.observedFps = 27.5;
    config.remoteMetadata.selectedRoiPhysicalRect = pbapp::MetadataPhysicalRect{2560, 0, 4480, 1080};
    config.remoteMetadata.estimatedScaleX = 1.0;
    config.remoteMetadata.estimatedScaleY = 1.0;
    config.remoteMetadata.notes = "人工可见诊断";
    config.remoteMetadata.remoteUiProvenance = pbapp::MetadataProvenance::RemoteUiVisible;
    config.monitorSafety = pbapp::MonitorSafetySelection{
        MakeMonitor(2, L"\\\\.\\DISPLAY1", {0, 0, 2560, 1440}, true),
        MakeMonitor(1, L"\\\\.\\DISPLAY2", {2560, 0, 5120, 1440}, false)};
    REQUIRE(pbapp::ValidateDecoderConfig(config));

    config.remoteMetadata.remoteProvider.clear();
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.remoteMetadata.remoteProvider = " \t\r\n ";
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.remoteMetadata.remoteProvider = "TestRemote";
    REQUIRE(pbapp::ValidateDecoderConfig(config));
    config.remoteMetadata.observedFps = (std::numeric_limits<double>::quiet_NaN)();
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.remoteMetadata.observedFps = 30.0;
    config.remoteMetadata.selectedRoiPhysicalRect = pbapp::MetadataPhysicalRect{0, 0, 0, 1080};
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.remoteMetadata.selectedRoiPhysicalRect = pbapp::MetadataPhysicalRect{2560, 0, 4480, 1080};
    config.remoteMetadata.notes = std::string("\xC0\xAF", 2);
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.remoteMetadata.notes = std::string("left\0right", 10);
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.remoteMetadata.notes.assign(1025, 'x');
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
}

TEST_CASE("RemoteVisual production Replay fan-out supports Direct and Shape with the same bounded live contract",
    "[application][validation][remote-visual][production-replay]")
{
    ScratchDirectory scratch(L"remote-production-replay-validation");
    pbapp::DecoderConfig config;
    config.outputDirectory = scratch.Path().wstring();
    config.captureBackend = pbapp::CaptureBackend::Wgc;
    config.visualProfile = pbapp::VisualProfile::DirectLevels2x2;
    config.region.monitor = reinterpret_cast<HMONITOR>(2);
    config.region.physicalRect = {2560, 180, 4480, 1260};
    config.region.monitorPhysicalRect = {2560, 0, 5120, 1440};
    config.region.dpiX = 96;
    config.region.dpiY = 96;
    config.region.rotation = DXGI_MODE_ROTATION_IDENTITY;
    config.remoteMetadata.channelType = pbapp::ChannelType::RemoteVisual;
    config.remoteMetadata.remoteProvider = "GenericRemote";
    config.monitorSafety = pbapp::MonitorSafetySelection{
        MakeMonitor(1, L"\\\\.\\DISPLAY1", {0, 0, 2560, 1440}, true),
        MakeMonitor(2, L"\\\\.\\DISPLAY2", {2560, 0, 5120, 1440}, false)};
    config.replayOutputPath = (scratch.Path() / L"direct-production.pbrv2").wstring();
    config.replayMaximumCaptureFramesPerSecond = 10;
    REQUIRE(pbapp::ValidateDecoderConfig(config));

    config.visualProfile = pbapp::VisualProfile::ShapeChroma;
    config.replayOutputPath = (scratch.Path() / L"shape-production.pbrv2").wstring();
    REQUIRE(pbapp::ValidateDecoderConfig(config));
    config.captureBackend = pbapp::CaptureBackend::Dxgi;
    REQUIRE(pbapp::ValidateDecoderConfig(config));

    config.diagnosticCaptureOnly = true;
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.diagnosticCaptureOnly = false;
    config.remoteMetadata.channelType = pbapp::ChannelType::LocalDesktop;
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.remoteMetadata.channelType = pbapp::ChannelType::RemoteVisual;
    config.replayMaximumCaptureFramesPerSecond = 61;
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
}

TEST_CASE("Incompatible RemoteVisual ROI is admitted only for explicit bounded replay capture-only",
    "[application][validation][remote-visual][capture-only]")
{
    ScratchDirectory scratch(L"remote-capture-only-validation");
    pbapp::DecoderConfig config;
    config.outputDirectory = scratch.Path().wstring();
    config.captureBackend = pbapp::CaptureBackend::Wgc;
    config.visualProfile = pbapp::VisualProfile::RemoteVisualResilient;
    config.region.monitor = reinterpret_cast<HMONITOR>(2);
    config.region.physicalRect = {2880, 180, 4416, 1044};
    config.region.monitorPhysicalRect = {2560, 0, 5120, 1440};
    config.region.dpiX = 96;
    config.region.dpiY = 96;
    config.region.rotation = DXGI_MODE_ROTATION_IDENTITY;
    config.remoteMetadata.channelType = pbapp::ChannelType::RemoteVisual;
    config.remoteMetadata.remoteProvider = "TestRemote";
    config.remoteMetadata.selectedRoiPhysicalRect = pbapp::MetadataPhysicalRect{2880, 180, 4416, 1044};
    config.remoteMetadata.estimatedScaleX = 0.8;
    config.remoteMetadata.estimatedScaleY = 0.8;
    config.monitorSafety = pbapp::MonitorSafetySelection{
        MakeMonitor(1, L"\\\\.\\DISPLAY1", {0, 0, 2560, 1440}, true),
        MakeMonitor(2, L"\\\\.\\DISPLAY2", {2560, 0, 5120, 1440}, false)};

    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.diagnosticCaptureOnly = true;
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.replayOutputPath = (scratch.Path() / L"scaled-remote.pbrv2").wstring();
    REQUIRE(pbapp::ValidateDecoderConfig(config));
    config.replayMaximumCaptureFramesPerSecond = 10;
    REQUIRE(pbapp::ValidateDecoderConfig(config));
    config.replayMaximumCaptureFramesPerSecond = 60;
    REQUIRE(pbapp::ValidateDecoderConfig(config));
    config.replayMaximumCaptureFramesPerSecond = 61;
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.replayMaximumCaptureFramesPerSecond = 10;

    config.replayEvidenceVisualProfileId = pbmodulation::kDesktopLevels2ProfileId;
    REQUIRE(pbapp::ValidateDecoderConfig(config));
    config.replayEvidenceVisualProfileId = pbmodulation::kShapeChromaProfileId;
    REQUIRE(pbapp::ValidateDecoderConfig(config));
    config.replayEvidenceVisualProfileId = pbmodulation::kRemoteVisualLowFpsProfileId;
    REQUIRE(pbapp::ValidateDecoderConfig(config));
    config.replayEvidenceVisualProfileId = 0xDEADBEEF12345678ULL;
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.replayEvidenceVisualProfileId.reset();

    config.visualProfile = pbapp::VisualProfile::DirectLevels2x2;
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.visualProfile = pbapp::VisualProfile::RemoteVisualResilient;
    config.remoteMetadata.channelType = pbapp::ChannelType::LocalDesktop;
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.remoteMetadata.channelType = pbapp::ChannelType::RemoteVisual;
    config.region.rotation = DXGI_MODE_ROTATION_ROTATE90;
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
}

TEST_CASE("Capture backend inventory exposes explicit WGC and DXGI only", "[application][backend]")
{
    REQUIRE(std::string(pbapp::GetCaptureBackendName(pbapp::CaptureBackend::Wgc)) == "WGC");
    REQUIRE(std::string(pbapp::GetCaptureBackendName(pbapp::CaptureBackend::Dxgi)) ==
        "DXGI Desktop Duplication");
    REQUIRE(static_cast<unsigned int>(pbapp::CaptureBackend::Dxgi) == 1U);
}

TEST_CASE("Monitor geometry distinguishes canvas capacity from exact Phase 1 reference",
    "[application][monitor]")
{
    REQUIRE_FALSE(pbapp::CanHostPhase1Canvas({0, 0, 1919, 1080}));
    REQUIRE(pbapp::CanHostPhase1Canvas({0, 0, 2560, 1440}));
    REQUIRE_FALSE(pbapp::IsPhase1ReferenceMonitor({0, 0, 2560, 1440}, 60));
    REQUIRE(pbapp::IsPhase1ReferenceMonitor({0, 0, 1920, 1080}, 60));
}

TEST_CASE("Monitor canvas origin prefers centered work area and remains inside the physical monitor",
    "[application][monitor][origin]")
{
    const auto centered = pbapp::GetPhase1CanvasOrigin({2560, 0, 5120, 1440}, {2560, 0, 5120, 1392});
    REQUIRE(centered);
    REQUIRE(centered->x == 2880);
    REQUIRE(centered->y == 156);

    const auto physicalFallback = pbapp::GetPhase1CanvasOrigin({-1920, 0, 0, 1080}, {-1920, 0, 0, 1040});
    REQUIRE(physicalFallback);
    REQUIRE(physicalFallback->x == -1920);
    REQUIRE(physicalFallback->y == 0);

    REQUIRE_FALSE(pbapp::GetPhase1CanvasOrigin({0, 0, 1919, 1080}, {0, 0, 1919, 1040}));
    const auto invalidWorkArea = pbapp::GetPhase1CanvasOrigin({0, 0, 2560, 1440}, {-100, -100, 2460, 1300});
    REQUIRE(invalidWorkArea);
    REQUIRE(invalidWorkArea->x == 320);
    REQUIRE(invalidWorkArea->y == 180);
}

TEST_CASE("RemoteVisual monitor safety requires full ExperimentMonitor containment and zero ProtectedMonitor intersection",
    "[application][monitor][remote-safety]")
{
    const pbapp::MonitorInfo protectedMonitor = MakeMonitor(1, L"\\\\.\\DISPLAY1", {0, 0, 2560, 1440}, true);
    const pbapp::MonitorInfo experimentMonitor = MakeMonitor(2, L"\\\\.\\DISPLAY2", {2560, 0, 5120, 1440}, false);
    const pbapp::MonitorSafetySelection selection{protectedMonitor, experimentMonitor};
    REQUIRE(pbapp::ValidateMonitorSafetyTarget(selection, {2880, 180, 4800, 1260}, experimentMonitor.monitor));
    REQUIRE(pbapp::ValidateMonitorSafetyTarget(selection, {0, 0, 1920, 1080}, protectedMonitor.monitor).code ==
        pbapp::MonitorSafetyError::TargetOutsideExperimentMonitor);
    REQUIRE(pbapp::ValidateMonitorSafetyTarget(selection, {2400, 0, 4320, 1080}).code ==
        pbapp::MonitorSafetyError::TargetOutsideExperimentMonitor);

    pbapp::MonitorSafetySelection overlapping = selection;
    overlapping.protectedMonitor.physicalRect = {2500, 0, 3000, 1440};
    REQUIRE(pbapp::ValidateMonitorSafetyTarget(overlapping, {2880, 180, 4800, 1260}).code ==
        pbapp::MonitorSafetyError::SameMonitor);
    REQUIRE(pbapp::RectIntersects({0, 0, 2560, 1440}, {2560, 0, 5120, 1440}) == false);
    REQUIRE(pbapp::RectContains({2560, 0, 5120, 1440}, {2880, 180, 4800, 1260}));
}

TEST_CASE("Monitor identity detects topology DPI rotation refresh adapter and primary changes",
    "[application][monitor][identity]")
{
    const pbapp::MonitorInfo baseline = MakeMonitor(2, L"\\\\.\\DISPLAY2", {2560, 0, 5120, 1440}, false);
    pbapp::MonitorInfo changed = baseline;
    REQUIRE(pbapp::SameMonitorIdentity(baseline, changed));
    changed.dpiX = 120;
    REQUIRE_FALSE(pbapp::SameMonitorIdentity(baseline, changed));
    changed = baseline;
    changed.rotation = DXGI_MODE_ROTATION_ROTATE90;
    REQUIRE_FALSE(pbapp::SameMonitorIdentity(baseline, changed));
    changed = baseline;
    changed.adapterLuid.LowPart++;
    REQUIRE_FALSE(pbapp::SameMonitorIdentity(baseline, changed));
}
