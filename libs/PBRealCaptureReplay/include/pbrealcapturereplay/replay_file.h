#pragma once

#include "pbcapturenormalize/screen_capture_frame.h"
#include "pbpresenttiming/present_timing.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pbrealcapturereplay
{

inline constexpr std::uint16_t kReplayFormatVersion = 1;
inline constexpr std::size_t kReplayFileHeaderBytes = 96;
inline constexpr std::size_t kReplayFrameHeaderBytes = 512;
inline constexpr std::size_t kReplayFileFooterBytes = 40;

enum class ReplayDatasetClass : std::uint32_t
{
    LocalDesktopExactCandidate = 1,
    LocalDesktopDegraded = 2,
    LocalVideoCertifiedPlayer = 3,
    LocalVideoGenericPlayer = 4,
    FailureCases = 5
};

enum class ReplayError : std::uint8_t
{
    None,
    InvalidArgument,
    InvalidState,
    AlreadyExists,
    NotFound,
    IoFailure,
    UnsupportedVersion,
    MalformedHeader,
    MalformedFrame,
    TruncatedInput,
    ChecksumMismatch,
    DigestMismatch,
    ResourceLimit,
    OutOfMemory,
    FrameCountMismatch,
    EndOfFile
};

enum class ReplayStage : std::uint8_t
{
    None, Configuration, Open, Header, FrameHeader, FramePayload, Footer, Checksum, Flush, Publish, Read
};

struct ReplayStatus
{
    ReplayError code = ReplayError::None;
    ReplayStage stage = ReplayStage::None;
    std::int32_t nativeError = 0;
    std::uint64_t offset = 0;

    [[nodiscard]] explicit operator bool() const noexcept { return code == ReplayError::None; }
    [[nodiscard]] static ReplayStatus Failure(ReplayError code, ReplayStage stage,
        std::int32_t nativeError = 0, std::uint64_t offset = 0) noexcept
    {
        return {code == ReplayError::None ? ReplayError::InvalidState : code, stage, nativeError, offset};
    }
    bool operator==(const ReplayStatus&) const = default;
};

struct ReplayLimits
{
    std::uint64_t maximumFileBytes = 64ULL * 1024 * 1024 * 1024;
    std::uint64_t maximumRasterBytesPerFrame = 128ULL * 1024 * 1024;
    std::uint64_t maximumTotalRasterBytes = 64ULL * 1024 * 1024 * 1024;
    std::uint32_t maximumFrames = 65536;
    std::uint32_t maximumDisplayIdentityBytes = 4096;
    std::uint32_t maximumDimension = 16384;
};

struct ReplayFileDescriptor
{
    ReplayDatasetClass datasetClass = ReplayDatasetClass::LocalDesktopExactCandidate;
    std::array<std::byte, 16> datasetId{};
    std::uint32_t expectedFrameCount = 0;
    bool operator==(const ReplayFileDescriptor&) const = default;
};

struct ReplayRasterView
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t rowPitch = 0;
    DXGI_FORMAT pixelFormat = DXGI_FORMAT_UNKNOWN;
    std::span<const std::byte> pixels;
};

struct ReplayPresentationMetadata
{
    bool available = false;
    std::uint64_t presentationEpoch = 0;
    std::int64_t qpcFrequency = 0;
    pbpresenttiming::PresentSample sample;
};

struct ReplayFrameView
{
    std::uint64_t frameSequence = 0;
    std::span<const std::byte> canonicalBootstrap;
    pbcapturenormalize::ScreenCaptureFrameMetadata capture;
    std::uint32_t dpiX = 0;
    std::uint32_t dpiY = 0;
    double scaleX = 0;
    double scaleY = 0;
    std::string_view displayIdentityUtf8;
    ReplayPresentationMetadata presentation;
    ReplayRasterView senderCanonicalRaster;
    ReplayRasterView capturedRoi;
};

struct ReplayRaster
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t rowPitch = 0;
    DXGI_FORMAT pixelFormat = DXGI_FORMAT_UNKNOWN;
    std::vector<std::byte> pixels;
};

struct ReplayFrame
{
    std::uint64_t frameSequence = 0;
    std::array<std::byte, 44> canonicalBootstrap{};
    pbcapturenormalize::ScreenCaptureFrameMetadata capture;
    std::uint32_t dpiX = 0;
    std::uint32_t dpiY = 0;
    double scaleX = 0;
    double scaleY = 0;
    std::string displayIdentityUtf8;
    ReplayPresentationMetadata presentation;
    ReplayRaster senderCanonicalRaster;
    ReplayRaster capturedRoi;
};

struct ReplayFileSnapshot
{
    ReplayFileDescriptor descriptor;
    std::uint64_t fileBytes = 0;
    std::uint64_t totalRasterBytes = 0;
    std::uint32_t framesProcessed = 0;
    bool complete = false;
};

// Streaming writer. Create refuses an existing final path and uses CREATE_NEW
// for the sibling .partial file. Finalize flushes and publishes without
// replacement, closing the preflight race safely; destruction removes an
// incomplete .partial file. One owner thread only.
class ReplayWriter
{
public:
    struct Implementation;

    ReplayWriter() noexcept;
    ReplayWriter(ReplayWriter&&) noexcept;
    ReplayWriter& operator=(ReplayWriter&&) noexcept;
    ~ReplayWriter();
    ReplayWriter(const ReplayWriter&) = delete;
    ReplayWriter& operator=(const ReplayWriter&) = delete;

    [[nodiscard]] static ReplayStatus Create(const std::filesystem::path& targetPath,
        const ReplayFileDescriptor& descriptor, const ReplayLimits& limits,
        std::unique_ptr<ReplayWriter>& output) noexcept;
    [[nodiscard]] ReplayStatus Append(const ReplayFrameView& frame) noexcept;
    [[nodiscard]] ReplayStatus Finalize() noexcept;
    [[nodiscard]] ReplayFileSnapshot GetSnapshot() const noexcept;

private:
    explicit ReplayWriter(std::unique_ptr<Implementation> implementation) noexcept;
    std::unique_ptr<Implementation> implementation_;
};

// Open validates file/header/footer CRCs and a whole-stream CRC before exposing
// a reader. ReadNext remains O(one frame), bounds lengths before allocation,
// validates per-record CRC plus both raster BLAKE3 digests, and changes output
// only on success.
class ReplayReader
{
public:
    struct Implementation;

    ReplayReader() noexcept;
    ReplayReader(ReplayReader&&) noexcept;
    ReplayReader& operator=(ReplayReader&&) noexcept;
    ~ReplayReader();
    ReplayReader(const ReplayReader&) = delete;
    ReplayReader& operator=(const ReplayReader&) = delete;

    [[nodiscard]] static ReplayStatus Open(const std::filesystem::path& path,
        const ReplayLimits& limits, std::unique_ptr<ReplayReader>& output) noexcept;
    [[nodiscard]] ReplayStatus ReadNext(ReplayFrame& output) noexcept;
    [[nodiscard]] ReplayFileSnapshot GetSnapshot() const noexcept;

private:
    explicit ReplayReader(std::unique_ptr<Implementation> implementation) noexcept;
    std::unique_ptr<Implementation> implementation_;
};

[[nodiscard]] const char* GetReplayErrorName(ReplayError error) noexcept;

} // namespace pbrealcapturereplay
