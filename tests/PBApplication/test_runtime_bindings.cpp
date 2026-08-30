#include "local_desktop_runtime.h"
#include "monitor_catalog.h"
#include "pbrealcapturereplay/replay_v2.h"

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

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
    config.visualProfile = pbapp::VisualProfile::RemoteVisualResilient;
    config.logicalVisualFps = 15;
    config.controlRepetitions = 12;
    config.runId = "0123456789abcdef0123456789abcdef";
    REQUIRE(pbapp::ValidateEncoderConfig(config));
    config.logicalVisualFps = 241;
    REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
    config.logicalVisualFps = 15;
    config.controlRepetitions = 0;
    REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
    config.controlRepetitions = 12;
    config.runId = "0123456789ABCDEF0123456789ABCDEF";
    REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
    config.runId.clear();
    config.visualProfile = static_cast<pbapp::VisualProfile>(255);
    REQUIRE_FALSE(pbapp::ValidateEncoderConfig(config));
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

    config.replayOutputPath = (scratch.Path() / L"output.pbrv2").wstring();
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.replayOutputPath.clear();
    config.visualProfile = pbapp::VisualProfile::DirectLevels2x2;
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
