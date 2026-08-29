#include "pbrealcapturereplay/replay_file.h"

#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/checked_integer.h"
#include "pbprotocol/crc32c.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace pbrealcapturereplay
{
namespace
{

constexpr char fileMagic[8] = {'P', 'B', 'R', 'C', 'R', '0', '0', '1'};
constexpr char frameMagic[8] = {'P', 'B', 'R', 'F', 'R', '0', '0', '1'};
constexpr char footerMagic[8] = {'P', 'B', 'R', 'F', 'T', '0', '0', '1'};
constexpr std::uint32_t endianSentinel = 0x01020304;
constexpr std::uint64_t noRoiCopyTime = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint32_t noPresentId = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint8_t flagHdr = 1u << 0;
constexpr std::uint8_t flagCursorExcluded = 1u << 1;
constexpr std::uint8_t flagPresentationAvailable = 1u << 2;

constexpr std::size_t fileHeaderCrcOffset = 92;
constexpr std::size_t frameHeaderCrcOffset = 508;
constexpr std::size_t footerCrcOffset = 36;

template<std::size_t Size>
void StoreMagic(std::array<std::byte, Size>& output, const char (&magic)[8]) noexcept
{
    static_assert(Size >= 8);
    std::memcpy(output.data(), magic, 8);
}

bool HasMagic(const std::span<const std::byte> input, const char (&magic)[8]) noexcept
{
    return input.size() >= 8 && std::memcmp(input.data(), magic, 8) == 0;
}

void StoreUint16(const std::span<std::byte> output, const std::size_t offset, const std::uint16_t value) noexcept
{
    output[offset] = static_cast<std::byte>(value & 0xFF);
    output[offset + 1] = static_cast<std::byte>(value >> 8);
}

void StoreUint32(const std::span<std::byte> output, const std::size_t offset, const std::uint32_t value) noexcept
{
    for (std::size_t index = 0; index < 4; index++)
    {
        output[offset + index] = static_cast<std::byte>((value >> (index * 8)) & 0xFF);
    }
}

void StoreUint64(const std::span<std::byte> output, const std::size_t offset, const std::uint64_t value) noexcept
{
    for (std::size_t index = 0; index < 8; index++)
    {
        output[offset + index] = static_cast<std::byte>((value >> (index * 8)) & 0xFF);
    }
}

void StoreInt32(const std::span<std::byte> output, const std::size_t offset, const std::int32_t value) noexcept
{
    StoreUint32(output, offset, std::bit_cast<std::uint32_t>(value));
}

void StoreInt64(const std::span<std::byte> output, const std::size_t offset, const std::int64_t value) noexcept
{
    StoreUint64(output, offset, std::bit_cast<std::uint64_t>(value));
}

void StoreDouble(const std::span<std::byte> output, const std::size_t offset, const double value) noexcept
{
    StoreUint64(output, offset, std::bit_cast<std::uint64_t>(value));
}

std::uint16_t LoadUint16(const std::span<const std::byte> input, const std::size_t offset) noexcept
{
    return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(input[offset]) |
        (std::to_integer<std::uint16_t>(input[offset + 1]) << 8));
}

std::uint32_t LoadUint32(const std::span<const std::byte> input, const std::size_t offset) noexcept
{
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; index++)
    {
        value |= std::to_integer<std::uint32_t>(input[offset + index]) << (index * 8);
    }
    return value;
}

std::uint64_t LoadUint64(const std::span<const std::byte> input, const std::size_t offset) noexcept
{
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; index++)
    {
        value |= std::to_integer<std::uint64_t>(input[offset + index]) << (index * 8);
    }
    return value;
}

std::int32_t LoadInt32(const std::span<const std::byte> input, const std::size_t offset) noexcept
{
    return std::bit_cast<std::int32_t>(LoadUint32(input, offset));
}

std::int64_t LoadInt64(const std::span<const std::byte> input, const std::size_t offset) noexcept
{
    return std::bit_cast<std::int64_t>(LoadUint64(input, offset));
}

double LoadDouble(const std::span<const std::byte> input, const std::size_t offset) noexcept
{
    return std::bit_cast<double>(LoadUint64(input, offset));
}

bool IsZero(const std::span<const std::byte> bytes) noexcept
{
    return std::ranges::all_of(bytes, [](const std::byte value) { return value == std::byte{0}; });
}

bool ValidDatasetClass(const ReplayDatasetClass value) noexcept
{
    switch (value)
    {
    case ReplayDatasetClass::LocalDesktopExactCandidate:
    case ReplayDatasetClass::LocalDesktopDegraded:
    case ReplayDatasetClass::LocalVideoCertifiedPlayer:
    case ReplayDatasetClass::LocalVideoGenericPlayer:
    case ReplayDatasetClass::FailureCases: return true;
    }
    return false;
}

bool ValidCaptureBackend(const pbcapturenormalize::CaptureBackendKind value) noexcept
{
    return value == pbcapturenormalize::CaptureBackendKind::Wgc || value == pbcapturenormalize::CaptureBackendKind::Dxgi;
}

bool ValidTimestampDomain(const pbcapturenormalize::CaptureTimestampDomain value) noexcept
{
    return value == pbcapturenormalize::CaptureTimestampDomain::WgcSystemRelative100ns ||
        value == pbcapturenormalize::CaptureTimestampDomain::DxgiQpcTicks;
}

bool ValidSignalEncoding(const pbcapturenormalize::CaptureSignalEncoding value) noexcept
{
    return value == pbcapturenormalize::CaptureSignalEncoding::Unknown ||
        value == pbcapturenormalize::CaptureSignalEncoding::SdrRgb ||
        value == pbcapturenormalize::CaptureSignalEncoding::LinearScRgb;
}

bool ValidCursorState(const pbcapturenormalize::CursorState value) noexcept
{
    return value >= pbcapturenormalize::CursorState::Unknown && value <= pbcapturenormalize::CursorState::KnownAbsent;
}

bool ValidRotation(const DXGI_MODE_ROTATION value) noexcept
{
    return value >= DXGI_MODE_ROTATION_IDENTITY && value <= DXGI_MODE_ROTATION_ROTATE270;
}

bool ValidPresentOutcome(const pbpresenttiming::PresentOutcome value) noexcept
{
    return value >= pbpresenttiming::PresentOutcome::Success && value <= pbpresenttiming::PresentOutcome::Failure;
}

std::optional<std::uint32_t> BytesPerPixel(const DXGI_FORMAT format) noexcept
{
    switch (format)
    {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM: return 4;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
    default: return std::nullopt;
    }
}

bool ValidUtf8(const std::string_view text) noexcept
{
    if (text.empty())
    {
        return false;
    }
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    std::size_t index = 0;
    while (index < text.size())
    {
        const unsigned char first = bytes[index];
        if (first >= 1 && first <= 0x7F)
        {
            index++;
            continue;
        }
        std::size_t continuationCount = 0;
        unsigned char secondMinimum = 0x80;
        unsigned char secondMaximum = 0xBF;
        if (first >= 0xC2 && first <= 0xDF)
        {
            continuationCount = 1;
        }
        else if (first >= 0xE0 && first <= 0xEF)
        {
            continuationCount = 2;
            if (first == 0xE0)
            {
                secondMinimum = 0xA0;
            }
            else if (first == 0xED)
            {
                secondMaximum = 0x9F;
            }
        }
        else if (first >= 0xF0 && first <= 0xF4)
        {
            continuationCount = 3;
            if (first == 0xF0)
            {
                secondMinimum = 0x90;
            }
            else if (first == 0xF4)
            {
                secondMaximum = 0x8F;
            }
        }
        else
        {
            return false;
        }
        if (continuationCount > text.size() - index - 1 || bytes[index + 1] < secondMinimum || bytes[index + 1] > secondMaximum)
        {
            return false;
        }
        for (std::size_t continuation = 2; continuation <= continuationCount; continuation++)
        {
            if (bytes[index + continuation] < 0x80 || bytes[index + continuation] > 0xBF)
            {
                return false;
            }
        }
        index += continuationCount + 1;
    }
    return true;
}

ReplayStatus ValidateLimits(const ReplayLimits& limits) noexcept
{
    const std::uint64_t minimumFileBytes = kReplayFileHeaderBytes + kReplayFileFooterBytes;
    if (limits.maximumFileBytes < minimumFileBytes || limits.maximumRasterBytesPerFrame == 0 ||
        limits.maximumTotalRasterBytes == 0 || limits.maximumFrames == 0 || limits.maximumDisplayIdentityBytes == 0 ||
        limits.maximumDimension == 0 || limits.maximumDimension > 65535 ||
        limits.maximumFileBytes > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max()))
    {
        return ReplayStatus::Failure(ReplayError::InvalidArgument, ReplayStage::Configuration);
    }
    return {};
}

ReplayStatus ValidateDescriptor(const ReplayFileDescriptor& descriptor, const ReplayLimits& limits) noexcept
{
    if (!ValidDatasetClass(descriptor.datasetClass) || descriptor.expectedFrameCount == 0 ||
        descriptor.expectedFrameCount > limits.maximumFrames || IsZero(descriptor.datasetId))
    {
        return ReplayStatus::Failure(ReplayError::InvalidArgument, ReplayStage::Configuration);
    }
    return {};
}

ReplayStatus ValidateRaster(const ReplayRasterView& raster, const ReplayLimits& limits,
    const bool senderCanonical) noexcept
{
    const auto pixelBytes = BytesPerPixel(raster.pixelFormat);
    if (raster.width == 0 || raster.height == 0 || raster.width > limits.maximumDimension ||
        raster.height > limits.maximumDimension || !pixelBytes ||
        (senderCanonical && raster.pixelFormat != DXGI_FORMAT_B8G8R8A8_UNORM))
    {
        return ReplayStatus::Failure(ReplayError::InvalidArgument, ReplayStage::FrameHeader);
    }
    const auto expectedRowPitch = pbprotocol::CheckedMultiplyUnsigned<std::uint64_t>(raster.width, *pixelBytes);
    const auto expectedBytes = expectedRowPitch ?
        pbprotocol::CheckedMultiplyUnsigned<std::uint64_t>(expectedRowPitch.Value(), raster.height) : expectedRowPitch;
    if (!expectedRowPitch || !expectedBytes || expectedRowPitch.Value() != raster.rowPitch ||
        expectedBytes.Value() != raster.pixels.size())
    {
        return ReplayStatus::Failure(ReplayError::InvalidArgument, ReplayStage::FramePayload);
    }
    return {};
}

ReplayStatus ValidateEncodedRasterHeader(const std::uint32_t width, const std::uint32_t height,
    const std::uint32_t rowPitch, const DXGI_FORMAT pixelFormat, const std::uint64_t serializedBytes,
    const ReplayLimits& limits, const bool senderCanonical) noexcept
{
    const auto pixelBytes = BytesPerPixel(pixelFormat);
    if (width == 0 || height == 0 || width > limits.maximumDimension || height > limits.maximumDimension ||
        !pixelBytes || (senderCanonical && pixelFormat != DXGI_FORMAT_B8G8R8A8_UNORM))
    {
        return ReplayStatus::Failure(ReplayError::MalformedFrame, ReplayStage::FrameHeader);
    }
    const auto expectedRowPitch = pbprotocol::CheckedMultiplyUnsigned<std::uint64_t>(width, *pixelBytes);
    const auto expectedBytes = expectedRowPitch ?
        pbprotocol::CheckedMultiplyUnsigned<std::uint64_t>(expectedRowPitch.Value(), height) : expectedRowPitch;
    if (!expectedRowPitch || !expectedBytes || expectedRowPitch.Value() != rowPitch || expectedBytes.Value() != serializedBytes)
    {
        return ReplayStatus::Failure(ReplayError::MalformedFrame, ReplayStage::FrameHeader);
    }
    return {};
}

ReplayStatus ValidateFrameView(const ReplayFrameView& frame, const ReplayLimits& limits) noexcept
{
    if (frame.canonicalBootstrap.size() != 44)
    {
        return ReplayStatus::Failure(ReplayError::InvalidArgument, ReplayStage::FrameHeader);
    }
    const auto bootstrap = pbprotocol::ParseBootstrapRecord(frame.canonicalBootstrap);
    if (!bootstrap || bootstrap.Value().frameSequence != frame.frameSequence)
    {
        return ReplayStatus::Failure(ReplayError::InvalidArgument, ReplayStage::FrameHeader);
    }
    const auto senderStatus = ValidateRaster(frame.senderCanonicalRaster, limits, true);
    if (!senderStatus)
    {
        return senderStatus;
    }
    const auto capturedStatus = ValidateRaster(frame.capturedRoi, limits, false);
    if (!capturedStatus)
    {
        return capturedStatus;
    }
    const auto rasterBytes = pbprotocol::CheckedAddUnsigned<std::uint64_t>(frame.senderCanonicalRaster.pixels.size(), frame.capturedRoi.pixels.size());
    if (!rasterBytes || rasterBytes.Value() > limits.maximumRasterBytesPerFrame)
    {
        return ReplayStatus::Failure(ReplayError::ResourceLimit, ReplayStage::FramePayload);
    }
    if (frame.displayIdentityUtf8.size() > limits.maximumDisplayIdentityBytes || !ValidUtf8(frame.displayIdentityUtf8) ||
        frame.dpiX == 0 || frame.dpiY == 0 || frame.dpiX > 9600 || frame.dpiY > 9600 ||
        !std::isfinite(frame.scaleX) || !std::isfinite(frame.scaleY) || frame.scaleX <= 0 || frame.scaleY <= 0 ||
        frame.scaleX > 64 || frame.scaleY > 64)
    {
        return ReplayStatus::Failure(ReplayError::InvalidArgument, ReplayStage::FrameHeader);
    }
    const auto& capture = frame.capture;
    if (capture.domain.captureEpoch == 0 || IsZero(capture.domain.sourceId) || capture.captureObservation == 0 ||
        capture.sourceGeneration == 0 || capture.slotGeneration == 0 || capture.slotIndex >= 65536 ||
        !ValidCaptureBackend(capture.backend) || !ValidTimestampDomain(capture.timestamp.domain) ||
        !ValidSignalEncoding(capture.signalEncoding) || !ValidCursorState(capture.sourceCursorState) ||
        !ValidRotation(capture.displayRotation) || !ValidRotation(capture.sourceTransform) ||
        capture.timestamp.rawValue < 0 || capture.timestamp.rawFrequency <= 0 || capture.timestamp.monotonic100ns < 0 ||
        capture.timestamp.arrivalQpc100ns < -1 || capture.bitsPerColor == 0 || capture.bitsPerColor > 32 ||
        capture.sourceContentSize.width <= 0 || capture.sourceContentSize.height <= 0 ||
        capture.sourceExtent.width <= 0 || capture.sourceExtent.height <= 0 ||
        capture.sourceContentSize.width > static_cast<std::int32_t>(limits.maximumDimension) ||
        capture.sourceContentSize.height > static_cast<std::int32_t>(limits.maximumDimension) ||
        capture.sourceExtent.width > static_cast<std::int32_t>(limits.maximumDimension) ||
        capture.sourceExtent.height > static_cast<std::int32_t>(limits.maximumDimension) ||
        capture.roiSize.width != static_cast<std::int32_t>(frame.capturedRoi.width) ||
        capture.roiSize.height != static_cast<std::int32_t>(frame.capturedRoi.height) ||
        capture.pixelFormat != frame.capturedRoi.pixelFormat || !BytesPerPixel(capture.sourcePixelFormat))
    {
        return ReplayStatus::Failure(ReplayError::InvalidArgument, ReplayStage::FrameHeader);
    }
    const auto physicalWidth = static_cast<std::int64_t>(capture.physicalRoi.right) - capture.physicalRoi.left;
    const auto physicalHeight = static_cast<std::int64_t>(capture.physicalRoi.bottom) - capture.physicalRoi.top;
    if (physicalWidth != capture.roiSize.width || physicalHeight != capture.roiSize.height ||
        (capture.pointer.separateVisible && !capture.pointer.positionKnown) || capture.pointer.shapeBytes > 16 * 1024 * 1024)
    {
        return ReplayStatus::Failure(ReplayError::InvalidArgument, ReplayStage::FrameHeader);
    }
    if (frame.presentation.available)
    {
        const auto& presentation = frame.presentation;
        if (presentation.presentationEpoch == 0 || presentation.qpcFrequency <= 0 ||
            presentation.sample.frameSequence != frame.frameSequence || presentation.sample.beginQpc < 0 ||
            presentation.sample.endQpc < presentation.sample.beginQpc || !ValidPresentOutcome(presentation.sample.outcome))
        {
            return ReplayStatus::Failure(ReplayError::InvalidArgument, ReplayStage::FrameHeader);
        }
    }
    else if (frame.presentation.presentationEpoch != 0 || frame.presentation.qpcFrequency != 0 ||
        frame.presentation.sample.frameSequence != 0 || frame.presentation.sample.beginQpc != 0 ||
        frame.presentation.sample.endQpc != 0 || frame.presentation.sample.outcome != pbpresenttiming::PresentOutcome::Failure ||
        frame.presentation.sample.presentId)
    {
        return ReplayStatus::Failure(ReplayError::InvalidArgument, ReplayStage::FrameHeader);
    }
    return {};
}

ReplayStatus ReadExactAt(const HANDLE file, const std::uint64_t offset, const std::span<std::byte> output,
    const ReplayStage stage) noexcept
{
    LARGE_INTEGER position{};
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max()))
    {
        return ReplayStatus::Failure(ReplayError::ResourceLimit, stage, 0, offset);
    }
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN))
    {
        return ReplayStatus::Failure(ReplayError::IoFailure, stage, static_cast<std::int32_t>(GetLastError()), offset);
    }
    std::size_t completed = 0;
    while (completed < output.size())
    {
        const auto remaining = output.size() - completed;
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(remaining, 1u << 30));
        DWORD bytesRead = 0;
        if (!ReadFile(file, output.data() + completed, request, &bytesRead, nullptr))
        {
            return ReplayStatus::Failure(ReplayError::IoFailure, stage, static_cast<std::int32_t>(GetLastError()), offset + completed);
        }
        if (bytesRead == 0)
        {
            return ReplayStatus::Failure(ReplayError::TruncatedInput, stage, 0, offset + completed);
        }
        completed += bytesRead;
    }
    return {};
}

ReplayStatus AddWithinLimit(const std::uint64_t left, const std::uint64_t right, const std::uint64_t limit,
    std::uint64_t& output, const ReplayStage stage, const std::uint64_t offset = 0) noexcept
{
    const auto result = pbprotocol::CheckedAddUint64WithinLimit(left, right, limit);
    if (!result)
    {
        return ReplayStatus::Failure(ReplayError::ResourceLimit, stage, 0, offset);
    }
    output = result.Value();
    return {};
}

std::array<std::byte, kReplayFileHeaderBytes> MakeFileHeader(const ReplayFileDescriptor& descriptor) noexcept
{
    std::array<std::byte, kReplayFileHeaderBytes> header{};
    StoreMagic(header, fileMagic);
    StoreUint16(header, 8, kReplayFormatVersion);
    StoreUint16(header, 10, static_cast<std::uint16_t>(kReplayFileHeaderBytes));
    StoreUint32(header, 12, endianSentinel);
    StoreUint32(header, 16, static_cast<std::uint32_t>(descriptor.datasetClass));
    StoreUint32(header, 20, descriptor.expectedFrameCount);
    std::copy(descriptor.datasetId.begin(), descriptor.datasetId.end(), header.begin() + 24);
    StoreUint64(header, 40, kReplayFileHeaderBytes);
    StoreUint32(header, fileHeaderCrcOffset, pbprotocol::ComputeCrc32c(std::span<const std::byte>(header).first(fileHeaderCrcOffset)));
    return header;
}

std::array<std::byte, kReplayFileFooterBytes> MakeFooter(const std::uint32_t frames, const std::uint64_t bytesBeforeFooter,
    const std::uint32_t streamCrc) noexcept
{
    std::array<std::byte, kReplayFileFooterBytes> footer{};
    StoreMagic(footer, footerMagic);
    StoreUint16(footer, 8, kReplayFormatVersion);
    StoreUint16(footer, 10, static_cast<std::uint16_t>(kReplayFileFooterBytes));
    StoreUint32(footer, 12, frames);
    StoreUint64(footer, 16, bytesBeforeFooter);
    StoreUint32(footer, 24, streamCrc);
    StoreUint32(footer, footerCrcOffset, pbprotocol::ComputeCrc32c(std::span<const std::byte>(footer).first(footerCrcOffset)));
    return footer;
}

} // namespace

struct ReplayWriter::Implementation
{
    Implementation() = default;
    Implementation(const Implementation&) = delete;
    Implementation& operator=(const Implementation&) = delete;

    ~Implementation()
    {
        if (file != INVALID_HANDLE_VALUE)
        {
            CloseHandle(file);
        }
        if (!complete && !partialPath.empty())
        {
            DeleteFileW(partialPath.c_str());
        }
    }

    HANDLE file = INVALID_HANDLE_VALUE;
    std::filesystem::path targetPath;
    std::filesystem::path partialPath;
    ReplayFileDescriptor descriptor;
    ReplayLimits limits;
    pbprotocol::Crc32c streamCrc;
    std::uint64_t fileBytes = 0;
    std::uint64_t totalRasterBytes = 0;
    std::uint32_t frames = 0;
    bool failed = false;
    bool complete = false;
};

struct ReplayReader::Implementation
{
    Implementation() = default;
    Implementation(const Implementation&) = delete;
    Implementation& operator=(const Implementation&) = delete;

    ~Implementation()
    {
        if (file != INVALID_HANDLE_VALUE)
        {
            CloseHandle(file);
        }
    }

    HANDLE file = INVALID_HANDLE_VALUE;
    ReplayFileDescriptor descriptor;
    ReplayLimits limits;
    std::uint64_t fileBytes = 0;
    std::uint64_t bytesBeforeFooter = 0;
    std::uint64_t currentOffset = kReplayFileHeaderBytes;
    std::uint64_t totalRasterBytes = 0;
    std::uint32_t frames = 0;
    bool failed = false;
};

namespace
{

ReplayStatus FailWriter(ReplayWriter::Implementation& implementation, const ReplayStatus& status) noexcept
{
    implementation.failed = true;
    return status;
}

ReplayStatus WriteTracked(ReplayWriter::Implementation& implementation, const std::span<const std::byte> bytes,
    const ReplayStage stage, const bool includeInStreamCrc = true) noexcept
{
    std::uint64_t nextFileBytes = 0;
    const auto sizeStatus = AddWithinLimit(implementation.fileBytes, bytes.size(), implementation.limits.maximumFileBytes,
        nextFileBytes, stage, implementation.fileBytes);
    if (!sizeStatus)
    {
        return FailWriter(implementation, sizeStatus);
    }
    std::size_t completed = 0;
    while (completed < bytes.size())
    {
        const auto remaining = bytes.size() - completed;
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(remaining, 1u << 30));
        DWORD bytesWritten = 0;
        if (!WriteFile(implementation.file, bytes.data() + completed, request, &bytesWritten, nullptr))
        {
            return FailWriter(implementation, ReplayStatus::Failure(ReplayError::IoFailure, stage,
                static_cast<std::int32_t>(GetLastError()), implementation.fileBytes + completed));
        }
        if (bytesWritten == 0)
        {
            return FailWriter(implementation, ReplayStatus::Failure(ReplayError::IoFailure, stage, 0,
                implementation.fileBytes + completed));
        }
        if (includeInStreamCrc)
        {
            implementation.streamCrc.Update(bytes.subspan(completed, bytesWritten));
        }
        completed += bytesWritten;
    }
    implementation.fileBytes = nextFileBytes;
    return {};
}

std::array<std::byte, kReplayFrameHeaderBytes> MakeFrameHeader(const ReplayFrameView& frame,
    const std::uint64_t frameOrdinal, const std::uint64_t recordBytes,
    const std::array<std::byte, 32>& senderDigest, const std::array<std::byte, 32>& capturedDigest) noexcept
{
    std::array<std::byte, kReplayFrameHeaderBytes> header{};
    StoreMagic(header, frameMagic);
    StoreUint16(header, 8, kReplayFormatVersion);
    StoreUint16(header, 10, static_cast<std::uint16_t>(kReplayFrameHeaderBytes));
    StoreUint64(header, 12, recordBytes);
    StoreUint64(header, 20, frameOrdinal);
    StoreUint64(header, 28, frame.frameSequence);
    StoreUint32(header, 36, frame.senderCanonicalRaster.width);
    StoreUint32(header, 40, frame.senderCanonicalRaster.height);
    StoreUint32(header, 44, frame.senderCanonicalRaster.rowPitch);
    StoreUint32(header, 48, static_cast<std::uint32_t>(frame.senderCanonicalRaster.pixelFormat));
    StoreUint64(header, 52, frame.senderCanonicalRaster.pixels.size());
    StoreUint32(header, 60, frame.capturedRoi.width);
    StoreUint32(header, 64, frame.capturedRoi.height);
    StoreUint32(header, 68, frame.capturedRoi.rowPitch);
    StoreUint32(header, 72, static_cast<std::uint32_t>(frame.capturedRoi.pixelFormat));
    StoreUint64(header, 76, frame.capturedRoi.pixels.size());
    StoreUint32(header, 84, static_cast<std::uint32_t>(frame.displayIdentityUtf8.size()));
    std::copy(frame.canonicalBootstrap.begin(), frame.canonicalBootstrap.end(), header.begin() + 88);
    std::copy(frame.capture.domain.sourceId.begin(), frame.capture.domain.sourceId.end(), header.begin() + 132);
    StoreUint64(header, 148, frame.capture.domain.captureEpoch);
    StoreUint64(header, 156, frame.capture.captureObservation);
    StoreUint64(header, 164, frame.capture.sourceGeneration);
    StoreUint64(header, 172, frame.capture.slotGeneration);
    StoreUint32(header, 180, frame.capture.slotIndex);
    header[184] = static_cast<std::byte>(frame.capture.backend);
    header[185] = static_cast<std::byte>(frame.capture.timestamp.domain);
    header[186] = static_cast<std::byte>(frame.capture.signalEncoding);
    header[187] = static_cast<std::byte>((frame.capture.hdr ? flagHdr : 0) |
        (frame.capture.isCursorExcluded ? flagCursorExcluded : 0) |
        (frame.presentation.available ? flagPresentationAvailable : 0));
    header[188] = static_cast<std::byte>(frame.capture.sourceCursorState);
    StoreInt32(header, 192, frame.capture.physicalRoi.left);
    StoreInt32(header, 196, frame.capture.physicalRoi.top);
    StoreInt32(header, 200, frame.capture.physicalRoi.right);
    StoreInt32(header, 204, frame.capture.physicalRoi.bottom);
    StoreInt32(header, 208, frame.capture.sourceContentSize.width);
    StoreInt32(header, 212, frame.capture.sourceContentSize.height);
    StoreInt32(header, 216, frame.capture.sourceExtent.width);
    StoreInt32(header, 220, frame.capture.sourceExtent.height);
    StoreInt32(header, 224, frame.capture.roiSize.width);
    StoreInt32(header, 228, frame.capture.roiSize.height);
    StoreUint32(header, 232, static_cast<std::uint32_t>(frame.capture.displayRotation));
    StoreUint32(header, 236, static_cast<std::uint32_t>(frame.capture.sourceTransform));
    StoreUint32(header, 240, static_cast<std::uint32_t>(frame.capture.sourcePixelFormat));
    StoreUint32(header, 244, static_cast<std::uint32_t>(frame.capture.pixelFormat));
    StoreInt32(header, 248, frame.capture.adapterLuid.HighPart);
    StoreUint32(header, 252, frame.capture.adapterLuid.LowPart);
    StoreUint32(header, 256, frame.capture.bitsPerColor);
    StoreUint32(header, 260, frame.capture.outputColorSpace);
    StoreInt64(header, 264, frame.capture.timestamp.rawValue);
    StoreInt64(header, 272, frame.capture.timestamp.rawFrequency);
    StoreInt64(header, 280, frame.capture.timestamp.monotonic100ns);
    StoreInt64(header, 288, frame.capture.timestamp.arrivalQpc100ns);
    StoreUint64(header, 296, frame.capture.roiCopyTime100ns.value_or(noRoiCopyTime));
    StoreUint32(header, 304, frame.dpiX);
    StoreUint32(header, 308, frame.dpiY);
    StoreDouble(header, 312, frame.scaleX);
    StoreDouble(header, 320, frame.scaleY);
    if (frame.presentation.available)
    {
        StoreUint64(header, 328, frame.presentation.presentationEpoch);
        StoreUint64(header, 336, frame.presentation.sample.frameSequence);
        StoreInt64(header, 344, frame.presentation.qpcFrequency);
        StoreInt64(header, 352, frame.presentation.sample.beginQpc);
        StoreInt64(header, 360, frame.presentation.sample.endQpc);
        StoreUint32(header, 368, frame.presentation.sample.presentId.value_or(noPresentId));
        StoreUint32(header, 372, static_cast<std::uint32_t>(frame.presentation.sample.outcome));
    }
    else
    {
        StoreUint32(header, 368, noPresentId);
    }
    std::copy(senderDigest.begin(), senderDigest.end(), header.begin() + 376);
    std::copy(capturedDigest.begin(), capturedDigest.end(), header.begin() + 408);
    const auto& pointer = frame.capture.pointer;
    header[440] = static_cast<std::byte>((pointer.positionKnown ? 1u : 0u) |
        (pointer.shapeKnown ? 2u : 0u) | (pointer.separateVisible ? 4u : 0u));
    StoreInt64(header, 444, pointer.physicalLeft);
    StoreInt64(header, 452, pointer.physicalTop);
    StoreInt64(header, 460, pointer.rawUpdateTimestamp);
    StoreUint32(header, 468, pointer.shapeType);
    StoreUint32(header, 472, pointer.shapeWidth);
    StoreUint32(header, 476, pointer.shapeRawHeight);
    StoreUint32(header, 480, pointer.shapeVisibleHeight);
    StoreUint32(header, 484, pointer.shapePitch);
    StoreUint32(header, 488, pointer.shapeBytes);
    StoreInt32(header, 492, pointer.hotspotX);
    StoreInt32(header, 496, pointer.hotspotY);
    StoreUint32(header, frameHeaderCrcOffset,
        pbprotocol::ComputeCrc32c(std::span<const std::byte>(header).first(frameHeaderCrcOffset)));
    return header;
}

ReplayStatus ParseFileHeader(const std::span<const std::byte> header, const ReplayLimits& limits,
    ReplayFileDescriptor& output) noexcept
{
    if (!HasMagic(header, fileMagic) || LoadUint16(header, 10) != kReplayFileHeaderBytes ||
        LoadUint32(header, 12) != endianSentinel || LoadUint64(header, 40) != kReplayFileHeaderBytes ||
        !IsZero(header.subspan(48, fileHeaderCrcOffset - 48)))
    {
        return ReplayStatus::Failure(ReplayError::MalformedHeader, ReplayStage::Header);
    }
    if (LoadUint16(header, 8) != kReplayFormatVersion)
    {
        return ReplayStatus::Failure(ReplayError::UnsupportedVersion, ReplayStage::Header);
    }
    if (LoadUint32(header, fileHeaderCrcOffset) != pbprotocol::ComputeCrc32c(header.first(fileHeaderCrcOffset)))
    {
        return ReplayStatus::Failure(ReplayError::ChecksumMismatch, ReplayStage::Header);
    }
    ReplayFileDescriptor descriptor;
    descriptor.datasetClass = static_cast<ReplayDatasetClass>(LoadUint32(header, 16));
    descriptor.expectedFrameCount = LoadUint32(header, 20);
    std::copy(header.begin() + 24, header.begin() + 40, descriptor.datasetId.begin());
    const auto descriptorStatus = ValidateDescriptor(descriptor, limits);
    if (!descriptorStatus)
    {
        return ReplayStatus::Failure(ReplayError::MalformedHeader, ReplayStage::Header);
    }
    output = descriptor;
    return {};
}

ReplayStatus ParseFooter(const std::span<const std::byte> footer, const std::uint64_t fileBytes,
    std::uint32_t& frameCount, std::uint64_t& bytesBeforeFooter, std::uint32_t& streamCrc) noexcept
{
    if (!HasMagic(footer, footerMagic) || LoadUint16(footer, 10) != kReplayFileFooterBytes ||
        !IsZero(footer.subspan(28, footerCrcOffset - 28)))
    {
        return ReplayStatus::Failure(ReplayError::MalformedHeader, ReplayStage::Footer, 0,
            fileBytes - kReplayFileFooterBytes);
    }
    if (LoadUint16(footer, 8) != kReplayFormatVersion)
    {
        return ReplayStatus::Failure(ReplayError::UnsupportedVersion, ReplayStage::Footer);
    }
    if (LoadUint32(footer, footerCrcOffset) != pbprotocol::ComputeCrc32c(footer.first(footerCrcOffset)))
    {
        return ReplayStatus::Failure(ReplayError::ChecksumMismatch, ReplayStage::Footer, 0,
            fileBytes - kReplayFileFooterBytes);
    }
    frameCount = LoadUint32(footer, 12);
    bytesBeforeFooter = LoadUint64(footer, 16);
    streamCrc = LoadUint32(footer, 24);
    std::uint64_t expectedFileBytes = 0;
    if (!AddWithinLimit(bytesBeforeFooter, kReplayFileFooterBytes, std::numeric_limits<std::uint64_t>::max(),
        expectedFileBytes, ReplayStage::Footer) || expectedFileBytes != fileBytes)
    {
        return ReplayStatus::Failure(ReplayError::TruncatedInput, ReplayStage::Footer, 0, bytesBeforeFooter);
    }
    return {};
}

ReplayStatus ValidateWholeStream(const HANDLE file, const std::uint64_t bytesBeforeFooter,
    const std::uint32_t expectedCrc) noexcept
{
    std::array<std::byte, 64 * 1024> buffer{};
    pbprotocol::Crc32c hasher;
    std::uint64_t offset = 0;
    while (offset < bytesBeforeFooter)
    {
        const auto remaining = bytesBeforeFooter - offset;
        const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
        const auto status = ReadExactAt(file, offset, std::span<std::byte>(buffer).first(chunk), ReplayStage::Checksum);
        if (!status)
        {
            return status;
        }
        hasher.Update(std::span<const std::byte>(buffer).first(chunk));
        offset += chunk;
    }
    return hasher.Finalize() == expectedCrc ? ReplayStatus{} :
        ReplayStatus::Failure(ReplayError::ChecksumMismatch, ReplayStage::Checksum);
}

ReplayStatus FailReader(ReplayReader::Implementation& implementation, const ReplayStatus& status) noexcept
{
    implementation.failed = true;
    return status;
}

} // namespace

ReplayWriter::ReplayWriter() noexcept = default;
ReplayWriter::ReplayWriter(std::unique_ptr<Implementation> implementation) noexcept : implementation_(std::move(implementation)) {}
ReplayWriter::ReplayWriter(ReplayWriter&&) noexcept = default;
ReplayWriter& ReplayWriter::operator=(ReplayWriter&&) noexcept = default;
ReplayWriter::~ReplayWriter() = default;

ReplayStatus ReplayWriter::Create(const std::filesystem::path& targetPath, const ReplayFileDescriptor& descriptor,
    const ReplayLimits& limits, std::unique_ptr<ReplayWriter>& output) noexcept
{
    const auto limitsStatus = ValidateLimits(limits);
    if (!limitsStatus)
    {
        return limitsStatus;
    }
    const auto descriptorStatus = ValidateDescriptor(descriptor, limits);
    if (!descriptorStatus)
    {
        return descriptorStatus;
    }
    if (targetPath.empty())
    {
        return ReplayStatus::Failure(ReplayError::InvalidArgument, ReplayStage::Configuration);
    }
    try
    {
        const DWORD finalAttributes = GetFileAttributesW(targetPath.c_str());
        if (finalAttributes != INVALID_FILE_ATTRIBUTES)
        {
            return ReplayStatus::Failure(ReplayError::AlreadyExists, ReplayStage::Open);
        }
        const DWORD attributesError = GetLastError();
        if (attributesError != ERROR_FILE_NOT_FOUND && attributesError != ERROR_PATH_NOT_FOUND)
        {
            return ReplayStatus::Failure(ReplayError::IoFailure, ReplayStage::Open, static_cast<std::int32_t>(attributesError));
        }
        auto implementation = std::make_unique<Implementation>();
        implementation->targetPath = targetPath;
        implementation->partialPath = targetPath;
        implementation->partialPath += L".partial";
        implementation->descriptor = descriptor;
        implementation->limits = limits;
        implementation->file = CreateFileW(implementation->partialPath.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (implementation->file == INVALID_HANDLE_VALUE)
        {
            const DWORD error = GetLastError();
            return ReplayStatus::Failure(error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS ? ReplayError::AlreadyExists : ReplayError::IoFailure,
                ReplayStage::Open, static_cast<std::int32_t>(error));
        }
        const auto header = MakeFileHeader(descriptor);
        const auto writeStatus = WriteTracked(*implementation, header, ReplayStage::Header);
        if (!writeStatus)
        {
            return writeStatus;
        }
        auto writer = std::unique_ptr<ReplayWriter>(new ReplayWriter(std::move(implementation)));
        output = std::move(writer);
        return {};
    }
    catch (const std::bad_alloc&)
    {
        return ReplayStatus::Failure(ReplayError::OutOfMemory, ReplayStage::Open);
    }
    catch (const std::length_error&)
    {
        return ReplayStatus::Failure(ReplayError::ResourceLimit, ReplayStage::Open);
    }
    catch (const std::filesystem::filesystem_error& error)
    {
        return ReplayStatus::Failure(ReplayError::IoFailure, ReplayStage::Open, error.code().value());
    }
}

ReplayStatus ReplayWriter::Append(const ReplayFrameView& frame) noexcept
{
    if (!implementation_ || implementation_->failed || implementation_->complete || implementation_->file == INVALID_HANDLE_VALUE)
    {
        return ReplayStatus::Failure(ReplayError::InvalidState, ReplayStage::FrameHeader);
    }
    auto& implementation = *implementation_;
    if (implementation.frames >= implementation.descriptor.expectedFrameCount)
    {
        return ReplayStatus::Failure(ReplayError::FrameCountMismatch, ReplayStage::FrameHeader,
            0, implementation.fileBytes);
    }
    const auto validationStatus = ValidateFrameView(frame, implementation.limits);
    if (!validationStatus)
    {
        return validationStatus;
    }
    const auto displayBytesResult = pbprotocol::CheckedNarrowUnsigned<std::uint32_t>(frame.displayIdentityUtf8.size());
    if (!displayBytesResult)
    {
        return ReplayStatus::Failure(ReplayError::ResourceLimit, ReplayStage::FramePayload, 0, implementation.fileBytes);
    }
    std::uint64_t recordBytes = kReplayFrameHeaderBytes;
    const std::array sizes{static_cast<std::uint64_t>(frame.displayIdentityUtf8.size()),
        static_cast<std::uint64_t>(frame.senderCanonicalRaster.pixels.size()),
        static_cast<std::uint64_t>(frame.capturedRoi.pixels.size()), std::uint64_t{4}};
    for (const auto size : sizes)
    {
        const auto addResult = pbprotocol::CheckedAddUint64(recordBytes, size);
        if (!addResult)
        {
            return ReplayStatus::Failure(ReplayError::ResourceLimit, ReplayStage::FramePayload, 0, implementation.fileBytes);
        }
        recordBytes = addResult.Value();
    }
    const auto recordAndFooter = pbprotocol::CheckedAddUint64(recordBytes, kReplayFileFooterBytes);
    std::uint64_t nextFileBytes = 0;
    const auto fileLimitStatus = recordAndFooter ?
        AddWithinLimit(implementation.fileBytes, recordAndFooter.Value(), implementation.limits.maximumFileBytes,
            nextFileBytes, ReplayStage::FramePayload, implementation.fileBytes) :
        ReplayStatus::Failure(ReplayError::ResourceLimit, ReplayStage::FramePayload, 0, implementation.fileBytes);
    if (!fileLimitStatus)
    {
        return fileLimitStatus;
    }
    const auto rasterBytesResult = pbprotocol::CheckedAddUint64(frame.senderCanonicalRaster.pixels.size(), frame.capturedRoi.pixels.size());
    if (!rasterBytesResult)
    {
        return ReplayStatus::Failure(ReplayError::ResourceLimit, ReplayStage::FramePayload, 0, implementation.fileBytes);
    }
    const auto rasterBytes = rasterBytesResult.Value();
    std::uint64_t nextRasterBytes = 0;
    const auto rasterLimitStatus = AddWithinLimit(implementation.totalRasterBytes, rasterBytes,
        implementation.limits.maximumTotalRasterBytes, nextRasterBytes, ReplayStage::FramePayload, implementation.fileBytes);
    if (!rasterLimitStatus)
    {
        return rasterLimitStatus;
    }
    const auto senderDigest = pbprotocol::ComputeBlake3Digest(frame.senderCanonicalRaster.pixels);
    const auto capturedDigest = pbprotocol::ComputeBlake3Digest(frame.capturedRoi.pixels);
    const auto header = MakeFrameHeader(frame, implementation.frames, recordBytes, senderDigest, capturedDigest);
    pbprotocol::Crc32c recordCrc;
    recordCrc.Update(header);
    const auto identityBytes = std::as_bytes(std::span(frame.displayIdentityUtf8.data(), frame.displayIdentityUtf8.size()));
    recordCrc.Update(identityBytes);
    recordCrc.Update(frame.senderCanonicalRaster.pixels);
    recordCrc.Update(frame.capturedRoi.pixels);
    std::array<std::byte, 4> trailer{};
    StoreUint32(trailer, 0, recordCrc.Finalize());

    for (const auto [bytes, stage] : std::array<std::pair<std::span<const std::byte>, ReplayStage>, 5>{
        std::pair<std::span<const std::byte>, ReplayStage>{header, ReplayStage::FrameHeader},
        {identityBytes, ReplayStage::FramePayload},
        {frame.senderCanonicalRaster.pixels, ReplayStage::FramePayload},
        {frame.capturedRoi.pixels, ReplayStage::FramePayload},
        {trailer, ReplayStage::FramePayload}})
    {
        const auto writeStatus = WriteTracked(implementation, bytes, stage);
        if (!writeStatus)
        {
            return writeStatus;
        }
    }
    implementation.totalRasterBytes = nextRasterBytes;
    implementation.frames++;
    return {};
}

ReplayStatus ReplayWriter::Finalize() noexcept
{
    if (!implementation_ || implementation_->failed || implementation_->complete || implementation_->file == INVALID_HANDLE_VALUE)
    {
        return ReplayStatus::Failure(ReplayError::InvalidState, ReplayStage::Footer);
    }
    auto& implementation = *implementation_;
    if (implementation.frames != implementation.descriptor.expectedFrameCount)
    {
        return ReplayStatus::Failure(ReplayError::FrameCountMismatch, ReplayStage::Footer, 0, implementation.fileBytes);
    }
    const auto footer = MakeFooter(implementation.frames, implementation.fileBytes, implementation.streamCrc.Finalize());
    const auto footerStatus = WriteTracked(implementation, footer, ReplayStage::Footer, false);
    if (!footerStatus)
    {
        return footerStatus;
    }
    if (!FlushFileBuffers(implementation.file))
    {
        return FailWriter(implementation, ReplayStatus::Failure(ReplayError::IoFailure, ReplayStage::Flush,
            static_cast<std::int32_t>(GetLastError()), implementation.fileBytes));
    }
    const HANDLE file = std::exchange(implementation.file, INVALID_HANDLE_VALUE);
    if (!CloseHandle(file))
    {
        return FailWriter(implementation, ReplayStatus::Failure(ReplayError::IoFailure, ReplayStage::Flush,
            static_cast<std::int32_t>(GetLastError()), implementation.fileBytes));
    }
    if (!MoveFileExW(implementation.partialPath.c_str(), implementation.targetPath.c_str(), MOVEFILE_WRITE_THROUGH))
    {
        const DWORD error = GetLastError();
        return FailWriter(implementation, ReplayStatus::Failure(error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS ?
            ReplayError::AlreadyExists : ReplayError::IoFailure, ReplayStage::Publish, static_cast<std::int32_t>(error), implementation.fileBytes));
    }
    implementation.complete = true;
    implementation.partialPath.clear();
    return {};
}

ReplayFileSnapshot ReplayWriter::GetSnapshot() const noexcept
{
    if (!implementation_)
    {
        return {};
    }
    return {implementation_->descriptor, implementation_->fileBytes, implementation_->totalRasterBytes,
        implementation_->frames, implementation_->complete};
}

ReplayReader::ReplayReader() noexcept = default;
ReplayReader::ReplayReader(std::unique_ptr<Implementation> implementation) noexcept : implementation_(std::move(implementation)) {}
ReplayReader::ReplayReader(ReplayReader&&) noexcept = default;
ReplayReader& ReplayReader::operator=(ReplayReader&&) noexcept = default;
ReplayReader::~ReplayReader() = default;

ReplayStatus ReplayReader::Open(const std::filesystem::path& path, const ReplayLimits& limits,
    std::unique_ptr<ReplayReader>& output) noexcept
{
    const auto limitsStatus = ValidateLimits(limits);
    if (!limitsStatus)
    {
        return limitsStatus;
    }
    if (path.empty())
    {
        return ReplayStatus::Failure(ReplayError::InvalidArgument, ReplayStage::Configuration);
    }
    try
    {
        auto implementation = std::make_unique<Implementation>();
        implementation->limits = limits;
        implementation->file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (implementation->file == INVALID_HANDLE_VALUE)
        {
            const DWORD error = GetLastError();
            return ReplayStatus::Failure(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? ReplayError::NotFound : ReplayError::IoFailure,
                ReplayStage::Open, static_cast<std::int32_t>(error));
        }
        LARGE_INTEGER fileSize{};
        if (!GetFileSizeEx(implementation->file, &fileSize))
        {
            return ReplayStatus::Failure(ReplayError::IoFailure, ReplayStage::Open,
                static_cast<std::int32_t>(GetLastError()));
        }
        if (fileSize.QuadPart < 0)
        {
            return ReplayStatus::Failure(ReplayError::MalformedHeader, ReplayStage::Open);
        }
        implementation->fileBytes = static_cast<std::uint64_t>(fileSize.QuadPart);
        if (implementation->fileBytes > limits.maximumFileBytes)
        {
            return ReplayStatus::Failure(ReplayError::ResourceLimit, ReplayStage::Open);
        }
        if (implementation->fileBytes < kReplayFileHeaderBytes + kReplayFileFooterBytes)
        {
            return ReplayStatus::Failure(ReplayError::TruncatedInput, ReplayStage::Open);
        }
        std::array<std::byte, kReplayFileHeaderBytes> header{};
        auto status = ReadExactAt(implementation->file, 0, header, ReplayStage::Header);
        if (!status)
        {
            return status;
        }
        status = ParseFileHeader(header, limits, implementation->descriptor);
        if (!status)
        {
            return status;
        }
        std::array<std::byte, kReplayFileFooterBytes> footer{};
        status = ReadExactAt(implementation->file, implementation->fileBytes - kReplayFileFooterBytes, footer, ReplayStage::Footer);
        if (!status)
        {
            return status;
        }
        std::uint32_t footerFrameCount = 0;
        std::uint32_t streamCrc = 0;
        status = ParseFooter(footer, implementation->fileBytes, footerFrameCount, implementation->bytesBeforeFooter, streamCrc);
        if (!status)
        {
            return status;
        }
        if (footerFrameCount != implementation->descriptor.expectedFrameCount ||
            implementation->bytesBeforeFooter < kReplayFileHeaderBytes)
        {
            return ReplayStatus::Failure(ReplayError::FrameCountMismatch, ReplayStage::Footer);
        }
        status = ValidateWholeStream(implementation->file, implementation->bytesBeforeFooter, streamCrc);
        if (!status)
        {
            return status;
        }
        auto reader = std::unique_ptr<ReplayReader>(new ReplayReader(std::move(implementation)));
        output = std::move(reader);
        return {};
    }
    catch (const std::bad_alloc&)
    {
        return ReplayStatus::Failure(ReplayError::OutOfMemory, ReplayStage::Open);
    }
    catch (const std::length_error&)
    {
        return ReplayStatus::Failure(ReplayError::ResourceLimit, ReplayStage::Open);
    }
    catch (const std::filesystem::filesystem_error& error)
    {
        return ReplayStatus::Failure(ReplayError::IoFailure, ReplayStage::Open, error.code().value());
    }
}

ReplayStatus ReplayReader::ReadNext(ReplayFrame& output) noexcept
{
    if (!implementation_ || implementation_->failed || implementation_->file == INVALID_HANDLE_VALUE)
    {
        return ReplayStatus::Failure(ReplayError::InvalidState, ReplayStage::Read);
    }
    auto& implementation = *implementation_;
    if (implementation.frames == implementation.descriptor.expectedFrameCount)
    {
        if (implementation.currentOffset != implementation.bytesBeforeFooter)
        {
            return FailReader(implementation, ReplayStatus::Failure(ReplayError::MalformedFrame, ReplayStage::Read,
                0, implementation.currentOffset));
        }
        return ReplayStatus::Failure(ReplayError::EndOfFile, ReplayStage::Read, 0, implementation.currentOffset);
    }
    std::uint64_t minimumEnd = 0;
    if (!AddWithinLimit(implementation.currentOffset, kReplayFrameHeaderBytes + 4, implementation.bytesBeforeFooter,
        minimumEnd, ReplayStage::FrameHeader, implementation.currentOffset))
    {
        return FailReader(implementation, ReplayStatus::Failure(ReplayError::TruncatedInput, ReplayStage::FrameHeader,
            0, implementation.currentOffset));
    }
    std::array<std::byte, kReplayFrameHeaderBytes> header{};
    auto status = ReadExactAt(implementation.file, implementation.currentOffset, header, ReplayStage::FrameHeader);
    if (!status)
    {
        return FailReader(implementation, status);
    }
    if (!HasMagic(header, frameMagic) || LoadUint16(header, 10) != kReplayFrameHeaderBytes ||
        !IsZero(std::span<const std::byte>(header).subspan(189, 3)) ||
        !IsZero(std::span<const std::byte>(header).subspan(441, 3)) ||
        !IsZero(std::span<const std::byte>(header).subspan(500, frameHeaderCrcOffset - 500)))
    {
        return FailReader(implementation, ReplayStatus::Failure(ReplayError::MalformedFrame, ReplayStage::FrameHeader,
            0, implementation.currentOffset));
    }
    if (LoadUint16(header, 8) != kReplayFormatVersion)
    {
        return FailReader(implementation, ReplayStatus::Failure(ReplayError::UnsupportedVersion, ReplayStage::FrameHeader,
            0, implementation.currentOffset));
    }
    if (LoadUint32(header, frameHeaderCrcOffset) != pbprotocol::ComputeCrc32c(std::span<const std::byte>(header).first(frameHeaderCrcOffset)))
    {
        return FailReader(implementation, ReplayStatus::Failure(ReplayError::ChecksumMismatch, ReplayStage::FrameHeader,
            0, implementation.currentOffset));
    }
    const std::uint64_t recordBytes = LoadUint64(header, 12);
    const std::uint64_t senderBytes = LoadUint64(header, 52);
    const std::uint64_t capturedBytes = LoadUint64(header, 76);
    const std::uint32_t displayBytes = LoadUint32(header, 84);
    std::uint64_t computedRecordBytes = kReplayFrameHeaderBytes;
    for (const auto size : std::array{static_cast<std::uint64_t>(displayBytes), senderBytes, capturedBytes, std::uint64_t{4}})
    {
        const auto addResult = pbprotocol::CheckedAddUint64(computedRecordBytes, size);
        if (!addResult)
        {
            return FailReader(implementation, ReplayStatus::Failure(ReplayError::ResourceLimit, ReplayStage::FrameHeader,
                0, implementation.currentOffset));
        }
        computedRecordBytes = addResult.Value();
    }
    std::uint64_t nextOffset = 0;
    if (recordBytes != computedRecordBytes || LoadUint64(header, 20) != implementation.frames ||
        !AddWithinLimit(implementation.currentOffset, recordBytes, implementation.bytesBeforeFooter,
            nextOffset, ReplayStage::FrameHeader, implementation.currentOffset))
    {
        return FailReader(implementation, ReplayStatus::Failure(ReplayError::MalformedFrame, ReplayStage::FrameHeader,
            0, implementation.currentOffset));
    }
    if (displayBytes == 0 || displayBytes > implementation.limits.maximumDisplayIdentityBytes)
    {
        return FailReader(implementation, ReplayStatus::Failure(ReplayError::ResourceLimit, ReplayStage::FrameHeader,
            0, implementation.currentOffset));
    }
    status = ValidateEncodedRasterHeader(LoadUint32(header, 36), LoadUint32(header, 40), LoadUint32(header, 44),
        static_cast<DXGI_FORMAT>(LoadUint32(header, 48)), senderBytes, implementation.limits, true);
    if (!status)
    {
        return FailReader(implementation, ReplayStatus::Failure(status.code, status.stage, status.nativeError,
            implementation.currentOffset));
    }
    status = ValidateEncodedRasterHeader(LoadUint32(header, 60), LoadUint32(header, 64), LoadUint32(header, 68),
        static_cast<DXGI_FORMAT>(LoadUint32(header, 72)), capturedBytes, implementation.limits, false);
    if (!status)
    {
        return FailReader(implementation, ReplayStatus::Failure(status.code, status.stage, status.nativeError,
            implementation.currentOffset));
    }
    const auto frameRasterBytes = pbprotocol::CheckedAddUint64(senderBytes, capturedBytes);
    if (!frameRasterBytes || frameRasterBytes.Value() > implementation.limits.maximumRasterBytesPerFrame)
    {
        return FailReader(implementation, ReplayStatus::Failure(ReplayError::ResourceLimit, ReplayStage::FramePayload,
            0, implementation.currentOffset));
    }
    std::uint64_t nextTotalRasterBytes = 0;
    if (!AddWithinLimit(implementation.totalRasterBytes, frameRasterBytes.Value(), implementation.limits.maximumTotalRasterBytes,
        nextTotalRasterBytes, ReplayStage::FramePayload, implementation.currentOffset))
    {
        return FailReader(implementation, ReplayStatus::Failure(ReplayError::ResourceLimit, ReplayStage::FramePayload,
            0, implementation.currentOffset));
    }
    const auto senderSize = pbprotocol::CheckedUint64ToSize(senderBytes);
    const auto capturedSize = pbprotocol::CheckedUint64ToSize(capturedBytes);
    if (!senderSize || !capturedSize)
    {
        return FailReader(implementation, ReplayStatus::Failure(ReplayError::ResourceLimit, ReplayStage::FramePayload,
            0, implementation.currentOffset));
    }

    ReplayFrame candidate;
    try
    {
        candidate.displayIdentityUtf8.resize(displayBytes);
        candidate.senderCanonicalRaster.pixels.resize(senderSize.Value());
        candidate.capturedRoi.pixels.resize(capturedSize.Value());
    }
    catch (const std::bad_alloc&)
    {
        return FailReader(implementation, ReplayStatus::Failure(ReplayError::OutOfMemory, ReplayStage::FramePayload,
            0, implementation.currentOffset));
    }
    catch (const std::length_error&)
    {
        return FailReader(implementation, ReplayStatus::Failure(ReplayError::ResourceLimit, ReplayStage::FramePayload,
            0, implementation.currentOffset));
    }

    candidate.frameSequence = LoadUint64(header, 28);
    std::copy(header.begin() + 88, header.begin() + 132, candidate.canonicalBootstrap.begin());
    auto& capture = candidate.capture;
    std::copy(header.begin() + 132, header.begin() + 148, capture.domain.sourceId.begin());
    capture.domain.captureEpoch = LoadUint64(header, 148);
    capture.captureObservation = LoadUint64(header, 156);
    capture.sourceGeneration = LoadUint64(header, 164);
    capture.slotGeneration = LoadUint64(header, 172);
    capture.slotIndex = LoadUint32(header, 180);
    capture.backend = static_cast<pbcapturenormalize::CaptureBackendKind>(std::to_integer<std::uint8_t>(header[184]));
    capture.timestamp.domain = static_cast<pbcapturenormalize::CaptureTimestampDomain>(std::to_integer<std::uint8_t>(header[185]));
    capture.signalEncoding = static_cast<pbcapturenormalize::CaptureSignalEncoding>(std::to_integer<std::uint8_t>(header[186]));
    const std::uint8_t flags = std::to_integer<std::uint8_t>(header[187]);
    if ((flags & ~(flagHdr | flagCursorExcluded | flagPresentationAvailable)) != 0)
    {
        return FailReader(implementation, ReplayStatus::Failure(ReplayError::MalformedFrame, ReplayStage::FrameHeader,
            0, implementation.currentOffset));
    }
    capture.hdr = (flags & flagHdr) != 0;
    capture.isCursorExcluded = (flags & flagCursorExcluded) != 0;
    capture.sourceCursorState = static_cast<pbcapturenormalize::CursorState>(std::to_integer<std::uint8_t>(header[188]));
    capture.physicalRoi = {LoadInt32(header, 192), LoadInt32(header, 196), LoadInt32(header, 200), LoadInt32(header, 204)};
    capture.sourceContentSize = {LoadInt32(header, 208), LoadInt32(header, 212)};
    capture.sourceExtent = {LoadInt32(header, 216), LoadInt32(header, 220)};
    capture.roiSize = {LoadInt32(header, 224), LoadInt32(header, 228)};
    capture.displayRotation = static_cast<DXGI_MODE_ROTATION>(LoadUint32(header, 232));
    capture.sourceTransform = static_cast<DXGI_MODE_ROTATION>(LoadUint32(header, 236));
    capture.sourcePixelFormat = static_cast<DXGI_FORMAT>(LoadUint32(header, 240));
    capture.pixelFormat = static_cast<DXGI_FORMAT>(LoadUint32(header, 244));
    capture.adapterLuid.HighPart = LoadInt32(header, 248);
    capture.adapterLuid.LowPart = LoadUint32(header, 252);
    capture.bitsPerColor = LoadUint32(header, 256);
    capture.outputColorSpace = LoadUint32(header, 260);
    capture.timestamp.rawValue = LoadInt64(header, 264);
    capture.timestamp.rawFrequency = LoadInt64(header, 272);
    capture.timestamp.monotonic100ns = LoadInt64(header, 280);
    capture.timestamp.arrivalQpc100ns = LoadInt64(header, 288);
    const auto roiCopyTime = LoadUint64(header, 296);
    if (roiCopyTime != noRoiCopyTime)
    {
        capture.roiCopyTime100ns = roiCopyTime;
    }
    candidate.dpiX = LoadUint32(header, 304);
    candidate.dpiY = LoadUint32(header, 308);
    candidate.scaleX = LoadDouble(header, 312);
    candidate.scaleY = LoadDouble(header, 320);
    candidate.presentation.available = (flags & flagPresentationAvailable) != 0;
    if (candidate.presentation.available)
    {
        candidate.presentation.presentationEpoch = LoadUint64(header, 328);
        candidate.presentation.sample.frameSequence = LoadUint64(header, 336);
        candidate.presentation.qpcFrequency = LoadInt64(header, 344);
        candidate.presentation.sample.beginQpc = LoadInt64(header, 352);
        candidate.presentation.sample.endQpc = LoadInt64(header, 360);
        const auto presentId = LoadUint32(header, 368);
        if (presentId != noPresentId)
        {
            candidate.presentation.sample.presentId = presentId;
        }
        const std::uint32_t presentOutcome = LoadUint32(header, 372);
        if (presentOutcome > static_cast<std::uint32_t>(pbpresenttiming::PresentOutcome::Failure))
        {
            return FailReader(implementation, ReplayStatus::Failure(ReplayError::MalformedFrame, ReplayStage::FrameHeader,
                0, implementation.currentOffset));
        }
        candidate.presentation.sample.outcome = static_cast<pbpresenttiming::PresentOutcome>(presentOutcome);
    }
    else if (!IsZero(std::span<const std::byte>(header).subspan(328, 40)) || LoadUint32(header, 368) != noPresentId ||
        !IsZero(std::span<const std::byte>(header).subspan(372, 4)))
    {
        return FailReader(implementation, ReplayStatus::Failure(ReplayError::MalformedFrame, ReplayStage::FrameHeader,
            0, implementation.currentOffset));
    }
    const std::uint8_t pointerFlags = std::to_integer<std::uint8_t>(header[440]);
    if ((pointerFlags & ~7u) != 0)
    {
        return FailReader(implementation, ReplayStatus::Failure(ReplayError::MalformedFrame, ReplayStage::FrameHeader,
            0, implementation.currentOffset));
    }
    capture.pointer.positionKnown = (pointerFlags & 1u) != 0;
    capture.pointer.shapeKnown = (pointerFlags & 2u) != 0;
    capture.pointer.separateVisible = (pointerFlags & 4u) != 0;
    capture.pointer.physicalLeft = LoadInt64(header, 444);
    capture.pointer.physicalTop = LoadInt64(header, 452);
    capture.pointer.rawUpdateTimestamp = LoadInt64(header, 460);
    capture.pointer.shapeType = LoadUint32(header, 468);
    capture.pointer.shapeWidth = LoadUint32(header, 472);
    capture.pointer.shapeRawHeight = LoadUint32(header, 476);
    capture.pointer.shapeVisibleHeight = LoadUint32(header, 480);
    capture.pointer.shapePitch = LoadUint32(header, 484);
    capture.pointer.shapeBytes = LoadUint32(header, 488);
    capture.pointer.hotspotX = LoadInt32(header, 492);
    capture.pointer.hotspotY = LoadInt32(header, 496);
    candidate.senderCanonicalRaster.width = LoadUint32(header, 36);
    candidate.senderCanonicalRaster.height = LoadUint32(header, 40);
    candidate.senderCanonicalRaster.rowPitch = LoadUint32(header, 44);
    candidate.senderCanonicalRaster.pixelFormat = static_cast<DXGI_FORMAT>(LoadUint32(header, 48));
    candidate.capturedRoi.width = LoadUint32(header, 60);
    candidate.capturedRoi.height = LoadUint32(header, 64);
    candidate.capturedRoi.rowPitch = LoadUint32(header, 68);
    candidate.capturedRoi.pixelFormat = static_cast<DXGI_FORMAT>(LoadUint32(header, 72));

    std::uint64_t payloadOffset = implementation.currentOffset + kReplayFrameHeaderBytes;
    auto identitySpan = std::as_writable_bytes(std::span(candidate.displayIdentityUtf8.data(), candidate.displayIdentityUtf8.size()));
    status = ReadExactAt(implementation.file, payloadOffset, identitySpan, ReplayStage::FramePayload);
    if (!status)
    {
        return FailReader(implementation, status);
    }
    payloadOffset += displayBytes;
    status = ReadExactAt(implementation.file, payloadOffset, candidate.senderCanonicalRaster.pixels, ReplayStage::FramePayload);
    if (!status)
    {
        return FailReader(implementation, status);
    }
    payloadOffset += senderBytes;
    status = ReadExactAt(implementation.file, payloadOffset, candidate.capturedRoi.pixels, ReplayStage::FramePayload);
    if (!status)
    {
        return FailReader(implementation, status);
    }
    payloadOffset += capturedBytes;
    std::array<std::byte, 4> trailer{};
    status = ReadExactAt(implementation.file, payloadOffset, trailer, ReplayStage::FramePayload);
    if (!status)
    {
        return FailReader(implementation, status);
    }
    pbprotocol::Crc32c recordCrc;
    recordCrc.Update(header);
    recordCrc.Update(identitySpan);
    recordCrc.Update(candidate.senderCanonicalRaster.pixels);
    recordCrc.Update(candidate.capturedRoi.pixels);
    if (LoadUint32(trailer, 0) != recordCrc.Finalize())
    {
        return FailReader(implementation, ReplayStatus::Failure(ReplayError::ChecksumMismatch, ReplayStage::FramePayload,
            0, payloadOffset));
    }
    std::array<std::byte, 32> expectedSenderDigest{};
    std::array<std::byte, 32> expectedCapturedDigest{};
    std::copy(header.begin() + 376, header.begin() + 408, expectedSenderDigest.begin());
    std::copy(header.begin() + 408, header.begin() + 440, expectedCapturedDigest.begin());
    if (pbprotocol::ComputeBlake3Digest(candidate.senderCanonicalRaster.pixels) != expectedSenderDigest ||
        pbprotocol::ComputeBlake3Digest(candidate.capturedRoi.pixels) != expectedCapturedDigest)
    {
        return FailReader(implementation, ReplayStatus::Failure(ReplayError::DigestMismatch, ReplayStage::FramePayload,
            0, implementation.currentOffset));
    }
    const ReplayFrameView view{candidate.frameSequence, candidate.canonicalBootstrap, candidate.capture,
        candidate.dpiX, candidate.dpiY, candidate.scaleX, candidate.scaleY, candidate.displayIdentityUtf8,
        candidate.presentation,
        {candidate.senderCanonicalRaster.width, candidate.senderCanonicalRaster.height, candidate.senderCanonicalRaster.rowPitch,
            candidate.senderCanonicalRaster.pixelFormat, candidate.senderCanonicalRaster.pixels},
        {candidate.capturedRoi.width, candidate.capturedRoi.height, candidate.capturedRoi.rowPitch,
            candidate.capturedRoi.pixelFormat, candidate.capturedRoi.pixels}};
    status = ValidateFrameView(view, implementation.limits);
    if (!status)
    {
        return FailReader(implementation, ReplayStatus::Failure(ReplayError::MalformedFrame,
            status.stage, status.nativeError, implementation.currentOffset));
    }
    if (implementation.frames + 1 == implementation.descriptor.expectedFrameCount && nextOffset != implementation.bytesBeforeFooter)
    {
        return FailReader(implementation, ReplayStatus::Failure(ReplayError::MalformedFrame, ReplayStage::Read,
            0, nextOffset));
    }
    implementation.currentOffset = nextOffset;
    implementation.totalRasterBytes = nextTotalRasterBytes;
    implementation.frames++;
    output = std::move(candidate);
    return {};
}

ReplayFileSnapshot ReplayReader::GetSnapshot() const noexcept
{
    if (!implementation_)
    {
        return {};
    }
    return {implementation_->descriptor, implementation_->fileBytes, implementation_->totalRasterBytes,
        implementation_->frames, implementation_->frames == implementation_->descriptor.expectedFrameCount &&
        implementation_->currentOffset == implementation_->bytesBeforeFooter};
}

const char* GetReplayErrorName(const ReplayError error) noexcept
{
    switch (error)
    {
    case ReplayError::None: return "None";
    case ReplayError::InvalidArgument: return "InvalidArgument";
    case ReplayError::InvalidState: return "InvalidState";
    case ReplayError::AlreadyExists: return "AlreadyExists";
    case ReplayError::NotFound: return "NotFound";
    case ReplayError::IoFailure: return "IoFailure";
    case ReplayError::UnsupportedVersion: return "UnsupportedVersion";
    case ReplayError::MalformedHeader: return "MalformedHeader";
    case ReplayError::MalformedFrame: return "MalformedFrame";
    case ReplayError::TruncatedInput: return "TruncatedInput";
    case ReplayError::ChecksumMismatch: return "ChecksumMismatch";
    case ReplayError::DigestMismatch: return "DigestMismatch";
    case ReplayError::ResourceLimit: return "ResourceLimit";
    case ReplayError::OutOfMemory: return "OutOfMemory";
    case ReplayError::FrameCountMismatch: return "FrameCountMismatch";
    case ReplayError::EndOfFile: return "EndOfFile";
    }
    return "Unknown";
}

} // namespace pbrealcapturereplay
