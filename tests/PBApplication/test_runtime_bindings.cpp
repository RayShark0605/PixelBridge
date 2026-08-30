#include "local_desktop_runtime.h"
#include "monitor_catalog.h"

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
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
    config.captureBackend = pbapp::CaptureBackend::Dxgi;
    REQUIRE(pbapp::ValidateDecoderConfig(config));
    config.region.physicalRect.left = -1;
    config.region.physicalRect.right = 1919;
    REQUIRE_FALSE(pbapp::ValidateDecoderConfig(config));
    config.region.physicalRect = {0, 0, 1920, 1080};
    config.captureBackend = static_cast<pbapp::CaptureBackend>(255);
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
