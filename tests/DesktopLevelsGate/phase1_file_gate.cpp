#include "phase1_file_gate_arguments.h"
#include "phase1_file_metrics.h"

#include "pbcapturenormalize/screen_capture_frame.h"
#include "pbcompression/segment_compression.h"
#include "pbdemodd3d11/capture_demodulator.h"
#include "pbinnerfec/qc_ldpc_codec.h"
#include "pbmodulation/desktop_levels.h"
#include "pbmodulation/local_desktop_bootstrap.h"
#include "pbmodulation/reference_raster.h"
#include "pbmodulation/shape_chroma.h"
#include "pbouterfec/direct_repeat.h"
#include "pbouterfec/wirehair_v2.h"
#include "pbreceiver/receiver_ingress.h"
#include "pbrenderd3d/data_window.h"
#include "pbscreencapturedxgi/dxgi_capture.h"
#include "pbscreencapturewgc/wgc_capture.h"
#include "pbscreenregion/screen_region.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/checked_integer.h"
#include "pbprotocol/descriptor_codec.h"
#include "pbprotocol/session_random.h"
#include "pbprotocol/transport_block_codec.h"
#include "../../libs/PBScreenCaptureDxgi/src/capture_internal.h"
#include "../../libs/PBScreenCaptureWgc/src/capture_internal.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace
{

using pbcapturenormalize::CaptureNormalizeConfig;
using pbcapturenormalize::CaptureSnapshot;
using pbcapturenormalize::CaptureStatus;

inline constexpr std::uint64_t mebibyte = 1024ULL * 1024ULL;
inline constexpr std::uint64_t sourceBytes = 8ULL * mebibyte;
inline constexpr std::uint64_t sourceSeed = 0x5048314741544531ULL;
inline constexpr std::uint32_t outerBlockBytes = 1314;
inline constexpr std::size_t informationBytes = 1350;
inline constexpr std::size_t codewordBytes = 2025;
inline constexpr std::uint32_t controlRepetitions = 4;
inline constexpr std::uint32_t tornFramesPerCycle = 4;
inline constexpr std::uint32_t normalDataFramesPerCycle = 116;
inline constexpr std::uint32_t controlFramesPerCycle = 3 * controlRepetitions;
inline constexpr std::uint32_t cycleFrames = controlFramesPerCycle + tornFramesPerCycle + normalDataFramesPerCycle;
inline constexpr std::uint32_t captureQueuedFrameLimit = 4;
inline constexpr std::uint32_t captureDemodulatorSlotCount = 4;
inline constexpr std::uint32_t captureResultQueueCapacity = 128;
inline constexpr std::uint32_t maximumFrameLeaseHighWater = 6;
inline constexpr std::uint64_t maximumForeignSessionErasedFrames = 16;
inline constexpr std::uint64_t maximumRetryableUnknownSessionControlDrops = 16;
inline constexpr std::uint64_t maximumUnboundSessionVisualFrames = 128;
inline constexpr std::uint32_t regularPostPublishObservationSeconds = 15;
inline constexpr std::int64_t wgcGateMinUpdateInterval100ns = 100000;
static_assert(cycleFrames == 132);
static_assert(pbdesktoplevels::kPayloadBytes == outerBlockBytes);
static_assert(pbdesktoplevels::kInfoBytes == informationBytes);
static_assert(pbdesktoplevels::kCodewordBytes == codewordBytes);

class GateFailure final : public std::runtime_error
{
public:
    explicit GateFailure(const std::string& message) : std::runtime_error(message)
    {
    }
};

void Require(const bool condition, const std::string_view message)
{
    if (!condition)
    {
        throw GateFailure(std::string(message));
    }
}

template <typename ResultType>
void RequireResult(const ResultType& result, const std::string_view message)
{
    Require(static_cast<bool>(result), message);
}

[[nodiscard]] std::string DescribeCaptureStatus(const CaptureStatus& status)
{
    std::ostringstream stream;
    stream << pbcapturenormalize::GetCaptureErrorName(status.code)
           << " stage=" << static_cast<unsigned int>(status.stage)
           << " native=" << status.nativeError;
    return stream.str();
}

void CheckedAdd(std::uint64_t& destination, const std::uint64_t value, const std::string_view message)
{
    const auto sum = pbprotocol::CheckedAddUint64(destination, value);
    RequireResult(sum, message);
    destination = sum.Value();
}

[[nodiscard]] std::string DescribeReceiverError(const pbreceiver::ReceiverError& error)
{
    std::ostringstream stream;
    if (const auto* protocol = std::get_if<pbprotocol::ProtocolError>(&error))
    {
        stream << "protocol code=" << static_cast<unsigned int>(protocol->code) << " offset=" << protocol->offset;
    }
    else if (const auto* outerFec = std::get_if<pbouterfec::OuterFecError>(&error))
    {
        stream << "outer-fec code=" << static_cast<unsigned int>(outerFec->code) << " detail=" << outerFec->detail;
    }
    else
    {
        const auto& compression = std::get<pbcompression::CompressionError>(error);
        stream << "compression code=" << static_cast<unsigned int>(compression.code) << " detail=" << compression.detail;
    }
    return stream.str();
}

void CheckedIncrement(std::uint64_t& destination, const std::string_view message)
{
    CheckedAdd(destination, 1, message);
}

class UniqueHandle
{
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(const HANDLE handle) noexcept : handle_(handle)
    {
    }
    ~UniqueHandle()
    {
        Reset();
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE))
    {
    }
    UniqueHandle& operator=(UniqueHandle&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }
    [[nodiscard]] HANDLE Get() const noexcept
    {
        return handle_;
    }
    [[nodiscard]] HANDLE Release() noexcept
    {
        return std::exchange(handle_, INVALID_HANDLE_VALUE);
    }
    void Reset(const HANDLE handle = INVALID_HANDLE_VALUE) noexcept
    {
        if (*this)
        {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

[[nodiscard]] std::string WindowsFailure(const std::string_view operation)
{
    return std::string(operation) + ": Win32=" + std::to_string(GetLastError());
}

void RequirePathAbsent(const std::wstring& path, const std::string_view name)
{
    SetLastError(ERROR_SUCCESS);
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES)
    {
        throw GateFailure(std::string(name) + " already exists");
    }
    const DWORD error = GetLastError();
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
    {
        throw GateFailure(std::string("cannot validate absence of ") + std::string(name) +
            ": Win32=" + std::to_string(error));
    }
}

void WriteAll(const HANDLE handle, const std::span<const std::byte> bytes)
{
    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        const std::size_t remaining = bytes.size() - offset;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        Require(WriteFile(handle, bytes.data() + offset, chunk, &written, nullptr) != FALSE && written == chunk,
            "bounded file write failed");
        offset += written;
    }
}

class NewJsonlFile
{
public:
    explicit NewJsonlFile(const std::wstring& path)
    {
        handle_.Reset(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
        if (!handle_)
        {
            throw GateFailure(WindowsFailure("create telemetry"));
        }
    }

    void Write(const std::string& line)
    {
        Require(line.find('\n') == std::string::npos && line.find('\r') == std::string::npos,
            "telemetry record contains an embedded newline");
        WriteAll(handle_.Get(), std::as_bytes(std::span(line.data(), line.size())));
        const std::array newline{std::byte{'\n'}};
        WriteAll(handle_.Get(), newline);
        Require(FlushFileBuffers(handle_.Get()) != FALSE, "telemetry flush failed");
    }

private:
    UniqueHandle handle_;
};

class SplitMix64
{
public:
    explicit SplitMix64(const std::uint64_t seed) noexcept : state_(seed)
    {
    }
    [[nodiscard]] std::uint64_t Next() noexcept
    {
        std::uint64_t value = (state_ += 0x9E3779B97F4A7C15ULL);
        value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
        return value ^ (value >> 31U);
    }

private:
    std::uint64_t state_ = 0;
};

struct ProfileBinding
{
    phase1gate::PhysicalProfile profile = phase1gate::PhysicalProfile::DesktopLevels2;
    const char* name = "desktop-levels-2x2";
    std::uint64_t visualProfileId = 0;
    std::uint8_t layoutVersion = 0;
    std::uint32_t dataBytes = 0;
    std::uint32_t codewords = 0;
};

[[nodiscard]] ProfileBinding GetProfileBinding(const phase1gate::PhysicalProfile profile)
{
    if (profile == phase1gate::PhysicalProfile::ShapeChroma)
    {
        return {profile, "shape-chroma", pbmodulation::kShapeChromaProfileId,
            pbmodulation::kShapeChromaLayoutVersion, static_cast<std::uint32_t>(pbmodulation::kShapeChromaDataBytes),
            pbmodulation::kShapeChromaCodewords};
    }
    const auto* const desktopProfile = pbmodulation::GetDesktopLevelsProfile(pbmodulation::kDesktopLevels2ProfileId);
    Require(desktopProfile != nullptr, "DesktopLevels 2x2 profile is unavailable");
    return {profile, "desktop-levels-2x2", desktopProfile->visualProfileId,
        pbmodulation::kDesktopLevelsLayoutVersion, desktopProfile->dataBytes, desktopProfile->codewords};
}

[[nodiscard]] std::string DigestHex(const std::array<std::byte, pbprotocol::kDigestBytes>& digest)
{
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (const std::byte value : digest)
    {
        stream << std::setw(2) << static_cast<unsigned int>(std::to_integer<std::uint8_t>(value));
    }
    return stream.str();
}

[[nodiscard]] std::array<std::byte, pbprotocol::kDigestBytes> HashBytes(const std::span<const std::byte> bytes) noexcept
{
    pbprotocol::Blake3Hasher hasher;
    constexpr std::size_t chunkBytes = 64U * 1024U;
    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        const std::size_t current = std::min(chunkBytes, bytes.size() - offset);
        hasher.Update(bytes.subspan(offset, current));
        offset += current;
    }
    return hasher.Finalize();
}

[[nodiscard]] std::array<std::byte, pbprotocol::kDigestBytes> HashFile(const std::wstring& path)
{
    UniqueHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file)
    {
        throw GateFailure(WindowsFailure("open file for digest"));
    }
    pbprotocol::Blake3Hasher hasher;
    // Keep the bounded digest scratch off the 1 MiB default Windows thread
    // stack. ASan redzones plus the live capture/result objects make a 1 MiB
    // local array fail in __chkstk at the authoritative final verification.
    std::vector<std::byte> buffer(64U * 1024U);
    for (;;)
    {
        DWORD readBytes = 0;
        Require(ReadFile(file.Get(), buffer.data(), static_cast<DWORD>(buffer.size()), &readBytes, nullptr) != FALSE,
            "file digest read failed");
        if (readBytes == 0)
        {
            break;
        }
        hasher.Update(std::span(buffer).first(readBytes));
    }
    return hasher.Finalize();
}

[[nodiscard]] std::uint64_t QpcNow()
{
    LARGE_INTEGER value{};
    Require(QueryPerformanceCounter(&value) != FALSE && value.QuadPart >= 0, "QueryPerformanceCounter failed");
    return static_cast<std::uint64_t>(value.QuadPart);
}

[[nodiscard]] std::uint64_t QpcFrequency()
{
    LARGE_INTEGER value{};
    Require(QueryPerformanceFrequency(&value) != FALSE && value.QuadPart > 0, "QueryPerformanceFrequency failed");
    return static_cast<std::uint64_t>(value.QuadPart);
}

[[nodiscard]] double ElapsedSeconds(const std::uint64_t begin, const std::uint64_t end,
    const std::uint64_t frequency)
{
    Require(end >= begin && frequency != 0, "invalid QPC interval");
    return static_cast<double>(end - begin) / static_cast<double>(frequency);
}

struct GateFileDescription
{
    pbprotocol::SessionDescriptor session;
    pbprotocol::SegmentDescriptor segment;
    pbprotocol::FinalManifest manifest;
    pbcompression::EncodedSegment encoded;
    std::vector<std::byte> sessionControl;
    std::vector<std::byte> manifestControl;
    std::vector<std::byte> segmentControl;
};

[[nodiscard]] std::vector<std::byte> WrapControlRecord(const pbprotocol::ControlRecordType recordType,
    const std::uint64_t controlSequence, const pbprotocol::SessionTag sessionTag,
    const std::span<const std::byte> payload)
{
    const pbprotocol::ControlRecordView record{pbprotocol::kControlVersion, recordType, controlSequence,
        sessionTag, payload};
    const auto size = pbprotocol::GetSerializedSize(record);
    RequireResult(size, "control record size failed");
    Require(size.Value() <= pbmodulation::kReferenceControlWindowBytes,
        "control record exceeds fixed reference window");
    std::vector<std::byte> bytes(size.Value());
    RequireResult(pbprotocol::SerializeControlRecord(record, bytes), "control record serialization failed");
    return bytes;
}

[[nodiscard]] GateFileDescription DescribeSource(const std::span<const std::byte> rawBytes)
{
    Require(rawBytes.size() == sourceBytes, "Gate source has a noncanonical byte count");
    const auto sessionIdResult = pbprotocol::GenerateRandomSessionId();
    RequireResult(sessionIdResult, "OS CSPRNG SessionId failed");
    const pbprotocol::SessionDescriptor session{pbprotocol::GetProtocolVersion(), sessionIdResult.Value(),
        rawBytes.size(), 1, pbprotocol::DigestAlgorithm::Blake3_256};
    const pbprotocol::SessionTag sessionTag = pbprotocol::DeriveSessionTag(session.sessionId);
    pbcompression::CompressionSettings settings;
    settings.maxOutputBytes = 16ULL * mebibyte;
    auto compression = pbcompression::CompressSegment(rawBytes, settings);
    RequireResult(compression, "Gate source compression failed");
    pbcompression::EncodedSegment encoded = std::move(compression).Value();
    Require(encoded.codec == pbprotocol::CompressionCodec::Raw && encoded.bytes.size() == rawBytes.size() &&
        std::equal(encoded.bytes.begin(), encoded.bytes.end(), rawBytes.begin()),
        "deterministic Gate source did not select byte-identical RAW encoding");
    const auto mode = pbouterfec::ChooseOuterFecMode(encoded.bytes.size(), outerBlockBytes);
    RequireResult(mode, "Outer FEC mode selection failed");
    Require(mode.Value() == pbprotocol::OuterFecMode::WirehairV2,
        "8 MiB Gate Segment did not select WirehairV2");
    auto encoder = pbouterfec::WirehairV2Encoder::Create(encoded.bytes, outerBlockBytes);
    RequireResult(encoder, "Wirehair descriptor encoder failed");
    pbprotocol::SegmentDescriptor segment;
    segment.sessionTag = sessionTag;
    segment.segmentOrdinal = 0;
    segment.rawOffset = 0;
    segment.rawSize = rawBytes.size();
    segment.encodedSize = encoded.bytes.size();
    segment.compressionCodec = encoded.codec;
    segment.outerFecMode = mode.Value();
    segment.outerBlockBytes = outerBlockBytes;
    segment.rawDigest = pbprotocol::RawDigest{HashBytes(rawBytes)};
    segment.encodedDigest = pbprotocol::EncodedDigest{pbprotocol::ComputeBlake3Digest(encoded.bytes)};
    segment.wirehairV2SerializedProfile = encoder.Value().GetSerializedProfile();
    const pbprotocol::FinalManifest manifest{session.sessionId, rawBytes.size(), 1,
        pbprotocol::WholeFileDigest{HashBytes(rawBytes)}, pbprotocol::DigestAlgorithm::Blake3_256};
    const auto policy = pbprotocol::GetDefaultReceiverResourcePolicy();
    std::array<std::byte, pbprotocol::kSessionDescriptorPayloadBytes> sessionPayload{};
    RequireResult(pbprotocol::SerializeSessionDescriptor(session, policy, sessionPayload),
        "SessionDescriptor serialization failed");
    std::array<std::byte, pbprotocol::kFinalManifestPayloadBytes> manifestPayload{};
    RequireResult(pbprotocol::SerializeFinalManifest(manifest, session, policy, manifestPayload),
        "FinalManifest serialization failed");
    const auto segmentSize = pbprotocol::GetSerializedSize(segment);
    RequireResult(segmentSize, "SegmentDescriptor size failed");
    std::vector<std::byte> segmentPayload(segmentSize.Value());
    RequireResult(pbprotocol::SerializeSegmentDescriptor(segment, session, policy, segmentPayload),
        "SegmentDescriptor serialization failed");
    GateFileDescription description;
    description.session = session;
    description.segment = segment;
    description.manifest = manifest;
    description.encoded = std::move(encoded);
    description.sessionControl = WrapControlRecord(pbprotocol::ControlRecordType::SessionDescriptor, 1,
        sessionTag, sessionPayload);
    description.manifestControl = WrapControlRecord(pbprotocol::ControlRecordType::FinalManifest, 2,
        sessionTag, manifestPayload);
    description.segmentControl = WrapControlRecord(pbprotocol::ControlRecordType::SegmentDescriptor, 3,
        sessionTag, segmentPayload);
    return description;
}

struct CreatedSource
{
    UniqueHandle handle;
    BY_HANDLE_FILE_INFORMATION identity{};
    std::vector<std::byte> bytes;
};

[[nodiscard]] CreatedSource CreateSource(const std::wstring& path)
{
    CreatedSource source;
    source.handle.Reset(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!source.handle)
    {
        throw GateFailure(WindowsFailure("create deterministic source"));
    }
    source.bytes.resize(static_cast<std::size_t>(sourceBytes));
    SplitMix64 random(sourceSeed);
    for (std::size_t offset = 0; offset < source.bytes.size(); offset += sizeof(std::uint64_t))
    {
        const std::uint64_t value = random.Next();
        const std::size_t count = std::min(sizeof(value), source.bytes.size() - offset);
        for (std::size_t index = 0; index < count; index++)
        {
            source.bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
        }
    }
    WriteAll(source.handle.Get(), source.bytes);
    Require(FlushFileBuffers(source.handle.Get()) != FALSE, "source FlushFileBuffers failed");
    Require(GetFileInformationByHandle(source.handle.Get(), &source.identity) != FALSE,
        "source identity query failed");
    Require((static_cast<std::uint64_t>(source.identity.nFileSizeHigh) << 32U | source.identity.nFileSizeLow) == sourceBytes,
        "created source size mismatch");
    return source;
}

void VerifySourceStable(const CreatedSource& source)
{
    BY_HANDLE_FILE_INFORMATION current{};
    Require(GetFileInformationByHandle(source.handle.Get(), &current) != FALSE,
        "source stability query failed");
    Require(current.dwVolumeSerialNumber == source.identity.dwVolumeSerialNumber &&
        current.nFileIndexHigh == source.identity.nFileIndexHigh && current.nFileIndexLow == source.identity.nFileIndexLow &&
        current.nFileSizeHigh == source.identity.nFileSizeHigh && current.nFileSizeLow == source.identity.nFileSizeLow &&
        CompareFileTime(&current.ftLastWriteTime, &source.identity.ftLastWriteTime) == 0,
        "source identity, size, or last-write time changed during Session");
}

enum class SenderFrameKind : std::uint8_t
{
    SessionControl,
    ManifestControl,
    SegmentControl,
    TornData,
    Data
};

class SenderFrameBuilder
{
public:
    SenderFrameBuilder(const ProfileBinding& profile, const GateFileDescription& description)
        : profile_(profile), description_(description), data_(profile.dataBytes), pixels_(pbmodulation::kLocalDesktopFrameBgraBytes),
          tornPixels_(pbmodulation::kLocalDesktopFrameBgraBytes)
    {
        auto encoder = pbouterfec::WirehairV2Encoder::Recreate(description_.encoded.bytes, description_.segment);
        RequireResult(encoder, "Wirehair sender recreation failed");
        encoder_ = std::make_unique<pbouterfec::WirehairV2Encoder>(std::move(encoder).Value());
        blockCount_ = encoder_->GetBlockCount();
        Require(blockCount_ >= 2 && blockCount_ <= pbouterfec::kWirehairV2MaximumBlockCount,
            "Wirehair sender block count is outside the certified range");
        const std::uint64_t repairAllowance = std::max<std::uint64_t>(4096, blockCount_ / 2U);
        const auto carouselLimit = pbprotocol::CheckedAddUint64(blockCount_, repairAllowance);
        RequireResult(carouselLimit, "Wirehair carousel ID range overflow");
        Require(carouselLimit.Value() <= std::numeric_limits<std::uint32_t>::max(),
            "Wirehair carousel ID range cannot narrow");
        carouselBlockLimit_ = static_cast<std::uint32_t>(carouselLimit.Value());
    }

    [[nodiscard]] SenderFrameKind GetCurrentKind() const noexcept
    {
        const std::uint32_t position = cyclePosition_;
        if (position < controlRepetitions)
        {
            return SenderFrameKind::SessionControl;
        }
        if (position < 2U * controlRepetitions)
        {
            return SenderFrameKind::ManifestControl;
        }
        if (position < 3U * controlRepetitions)
        {
            return SenderFrameKind::SegmentControl;
        }
        if (position < controlFramesPerCycle + tornFramesPerCycle)
        {
            return SenderFrameKind::TornData;
        }
        return SenderFrameKind::Data;
    }

    [[nodiscard]] const std::vector<std::byte>& Build(const std::uint64_t frameSequence)
    {
        const SenderFrameKind kind = GetCurrentKind();
        const auto bootstrap = MakeBootstrap(frameSequence);
        if (kind == SenderFrameKind::SessionControl || kind == SenderFrameKind::ManifestControl ||
            kind == SenderFrameKind::SegmentControl)
        {
            const auto& control = kind == SenderFrameKind::SessionControl ? description_.sessionControl :
                kind == SenderFrameKind::ManifestControl ? description_.manifestControl : description_.segmentControl;
            std::fill(controlWindow_.begin(), controlWindow_.end(), std::byte{0});
            std::copy(control.begin(), control.end(), controlWindow_.begin());
            std::fill(referenceData_.begin(), referenceData_.end(), std::byte{0});
            RequireResult(pbmodulation::EncodeReferenceFrame({bootstrap, controlWindow_, referenceData_}, pixels_),
                "fixed Control raster generation failed");
            return pixels_;
        }
        BuildTransportData();
        RequireResult(EncodePhysical(bootstrap, pixels_), "physical data raster generation failed");
        if (kind == SenderFrameKind::TornData)
        {
            Require(frameSequence != std::numeric_limits<std::uint64_t>::max(),
                "cannot inject a torn frame at the terminal FrameSequence");
            const auto conflictingBootstrap = MakeBootstrap(frameSequence + 1U);
            RequireResult(EncodePhysical(conflictingBootstrap, tornPixels_),
                "conflicting physical raster generation failed");
            const pbmodulation::LocalDesktopRegion region = pbmodulation::kLocalDesktopBootstrapRegions[1];
            const std::size_t rowBytes = static_cast<std::size_t>(region.width) * 4U;
            for (std::uint32_t row = 0; row < region.height; row++)
            {
                const std::size_t offset = (static_cast<std::size_t>(region.y + row) * pbmodulation::kLocalDesktopCanvasWidth + region.x) * 4U;
                std::copy_n(tornPixels_.begin() + static_cast<std::ptrdiff_t>(offset), rowBytes,
                    pixels_.begin() + static_cast<std::ptrdiff_t>(offset));
            }
        }
        return pixels_;
    }

    [[nodiscard]] const std::vector<std::byte>& GetBuiltPixels() const noexcept
    {
        return pixels_;
    }

    void Advance()
    {
        const SenderFrameKind kind = GetCurrentKind();
        if (kind == SenderFrameKind::Data)
        {
            nextOuterBlockId_ = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(nextOuterBlockId_) + profile_.codewords) % carouselBlockLimit_);
            CheckedIncrement(dataFrames_, "sender data-frame counter overflow");
        }
        else if (kind == SenderFrameKind::TornData)
        {
            CheckedIncrement(tornFrames_, "sender torn-frame counter overflow");
        }
        else
        {
            CheckedIncrement(controlFrames_, "sender control-frame counter overflow");
        }
        cyclePosition_ = (cyclePosition_ + 1U) % cycleFrames;
    }

    [[nodiscard]] std::uint32_t GetBlockCount() const noexcept
    {
        return blockCount_;
    }
    [[nodiscard]] std::uint64_t GetDataFrames() const noexcept
    {
        return dataFrames_;
    }
    [[nodiscard]] std::uint64_t GetControlFrames() const noexcept
    {
        return controlFrames_;
    }
    [[nodiscard]] std::uint64_t GetTornFrames() const noexcept
    {
        return tornFrames_;
    }

private:
    [[nodiscard]] std::array<std::byte, pbprotocol::kBootstrapRecordBytes> MakeBootstrap(
        const std::uint64_t frameSequence) const
    {
        const pbprotocol::BootstrapRecord record{pbprotocol::kBootstrapVersion, pbprotocol::GetProtocolVersion(),
            profile_.layoutVersion, profile_.visualProfileId, description_.segment.sessionTag, frameSequence, 0, 0};
        std::array<std::byte, pbprotocol::kBootstrapRecordBytes> bytes{};
        RequireResult(pbprotocol::SerializeBootstrapRecord(record, bytes), "Bootstrap serialization failed");
        return bytes;
    }

    [[nodiscard]] pbmodulation::ModulationStatus EncodePhysical(
        const std::span<const std::byte> bootstrap, const std::span<std::byte> output) const noexcept
    {
        return profile_.profile == phase1gate::PhysicalProfile::ShapeChroma ?
            pbmodulation::EncodeShapeChromaFrame(bootstrap, data_, output) :
            pbmodulation::EncodeDesktopLevelsFrame(bootstrap, data_, output);
    }

    void BuildTransportData()
    {
        std::fill(data_.begin(), data_.end(), std::byte{0});
        for (std::uint32_t slot = 0; slot < profile_.codewords; slot++)
        {
            const std::uint32_t blockId = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(nextOuterBlockId_) + slot) % carouselBlockLimit_);
            std::fill(outerPayload_.begin(), outerPayload_.end(), std::byte{0});
            const auto encoded = encoder_->EncodeBlock(blockId, outerPayload_);
            RequireResult(encoded, "Wirehair block encoding failed");
            Require(encoded.Value() != 0 && encoded.Value() <= outerPayload_.size() &&
                encoded.Value() <= std::numeric_limits<std::uint16_t>::max(),
                "Wirehair block payload length is invalid");
            const pbprotocol::TransportBlockHeader header{pbprotocol::kTransportBlockTypeData,
                pbprotocol::kTransportProtocolMinor, 0, description_.segment.sessionTag, 0, blockId,
                static_cast<std::uint16_t>(encoded.Value())};
            const std::size_t serializedSize = pbprotocol::GetTransportSerializedSize(header);
            Require(serializedSize <= transport_.size(), "serialized Transport exceeds information block");
            RequireResult(pbprotocol::SerializeTransportBlock(header,
                std::span(outerPayload_).first(encoded.Value()), std::span(transport_).first(serializedSize)),
                "Transport serialization failed");
            RequireResult(pbprotocol::FrameTransportBlockIntoInfoBlock(std::span(transport_).first(serializedSize),
                information_.size(), information_), "Transport information framing failed");
            RequireResult(pbinnerfec::EncodeQcLdpcCodeword(pbinnerfec::kInnerFecProfileIdRobust, information_,
                std::span(data_).subspan(static_cast<std::size_t>(slot) * codewordBytes, codewordBytes)),
                "Robust QC-LDPC encoding failed");
        }
    }

    ProfileBinding profile_;
    const GateFileDescription& description_;
    std::unique_ptr<pbouterfec::WirehairV2Encoder> encoder_;
    std::vector<std::byte> data_;
    std::vector<std::byte> pixels_;
    std::vector<std::byte> tornPixels_;
    std::array<std::byte, pbmodulation::kReferenceControlWindowBytes> controlWindow_{};
    std::array<std::byte, pbmodulation::kReferenceDataRegionBytes> referenceData_{};
    std::array<std::byte, outerBlockBytes> outerPayload_{};
    std::array<std::byte, informationBytes> transport_{};
    std::array<std::byte, informationBytes> information_{};
    std::uint32_t blockCount_ = 0;
    std::uint32_t carouselBlockLimit_ = 0;
    std::uint32_t nextOuterBlockId_ = 0;
    std::uint32_t cyclePosition_ = 0;
    std::uint64_t dataFrames_ = 0;
    std::uint64_t controlFrames_ = 0;
    std::uint64_t tornFrames_ = 0;
};

[[nodiscard]] bool HasStablePresentationContract(const pbrenderd3d::DataWindowSnapshot& snapshot) noexcept
{
    return snapshot.state == pbrenderd3d::WindowState::Running && snapshot.candidateContractSatisfied &&
        snapshot.contract.bufferWidth == pbmodulation::kLocalDesktopCanvasWidth &&
        snapshot.contract.bufferHeight == pbmodulation::kLocalDesktopCanvasHeight && snapshot.contract.bufferCount == 2 &&
        snapshot.contract.maximumFrameLatency == 1 && snapshot.contract.flipEffect == pbrenderd3d::FlipEffect::Discard &&
        snapshot.contract.bgraUnorm && snapshot.contract.noMsaa && snapshot.contract.alphaIgnored &&
        snapshot.contract.scalingNone && snapshot.contract.tearingDisabled && snapshot.contract.latencyWaitable &&
        snapshot.contract.perMonitorV2 && !snapshot.softwareRasterizer;
}

[[nodiscard]] std::string SenderFinalJson(const ProfileBinding& profile, const GateFileDescription& description,
    const SenderFrameBuilder& builder, const pbrenderd3d::DataWindowSnapshot& contractSnapshot,
    const pbrenderd3d::DataWindowSnapshot& stoppedSnapshot,
    const std::uint64_t frameSequence, const std::uint64_t elapsedMilliseconds)
{
    std::ostringstream stream;
    stream << std::boolalpha << std::setprecision(17)
           << "{\"event\":\"phase1-sender-final\",\"status\":\"pass\",\"profile\":\"" << profile.name
           << "\",\"sessionTag\":\"" << description.segment.sessionTag.value
           << "\",\"sourceBytes\":" << description.session.originalFileSize
           << ",\"encodedBytes\":" << description.segment.encodedSize
           << ",\"wholeFileDigest\":\"" << DigestHex(description.manifest.wholeFileDigest.bytes)
           << "\",\"wirehairBlockCount\":" << builder.GetBlockCount()
           << ",\"submittedVisualFrames\":" << frameSequence
           << ",\"dataFrames\":" << builder.GetDataFrames()
           << ",\"controlFrames\":" << builder.GetControlFrames()
           << ",\"injectedTornFrames\":" << builder.GetTornFrames()
           << ",\"presentCalls\":" << stoppedSnapshot.totalPresentCalls
           << ",\"successfulPresents\":" << stoppedSnapshot.totalSuccessfulPresents
           << ",\"replacedPendingFrames\":" << stoppedSnapshot.replacedPendingFrames
           << ",\"discardedEpochFrames\":" << stoppedSnapshot.discardedEpochFrames
           << ",\"presentationEpoch\":" << stoppedSnapshot.timing.presentationEpoch
           << ",\"swapChainGeneration\":" << stoppedSnapshot.swapChainGeneration
           << ",\"bufferGeneration\":" << stoppedSnapshot.bufferGeneration
           << ",\"candidateContractSatisfied\":" << contractSnapshot.candidateContractSatisfied
           << ",\"tearingDisabled\":" << contractSnapshot.contract.tearingDisabled
           << ",\"latencyWaitable\":" << contractSnapshot.contract.latencyWaitable
           << ",\"maximumFrameLatency\":" << contractSnapshot.contract.maximumFrameLatency
           << ",\"cleanShutdown\":true,\"pendingAtShutdown\":" << stoppedSnapshot.pendingFrame
           << ",\"inFlightAtShutdown\":" << stoppedSnapshot.inFlightFrame
           << ",\"elapsedMilliseconds\":" << elapsedMilliseconds << '}';
    return stream.str();
}

int RunSender(const phase1gate::Arguments& options)
{
    NewJsonlFile telemetry(options.telemetryPath);
    CreatedSource source = CreateSource(options.sourcePath);
    const GateFileDescription description = DescribeSource(source.bytes);
    const ProfileBinding profile = GetProfileBinding(options.profile);
    SenderFrameBuilder builder(profile, description);
    pbrenderd3d::DataWindowConfig windowConfig;
    windowConfig.width = pbmodulation::kLocalDesktopCanvasWidth;
    windowConfig.height = pbmodulation::kLocalDesktopCanvasHeight;
    auto created = pbrenderd3d::DataWindow::Create(windowConfig);
    RequireResult(created, "DataWindow creation failed");
    std::unique_ptr<pbrenderd3d::DataWindow> window = std::move(created).Value();
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::seconds(options.maximumSenderSeconds);
    auto nextTelemetry = started;
    std::uint64_t frameSequence = 0;
    bool frameBuilt = false;
    std::optional<pbrenderd3d::DataWindowSnapshot> lastContractSnapshot;
    for (;;)
    {
        const auto now = std::chrono::steady_clock::now();
        const auto snapshot = window->GetSnapshot();
        if (snapshot.state == pbrenderd3d::WindowState::Failed)
        {
            throw GateFailure("DataWindow entered Failed state");
        }
        if (snapshot.state == pbrenderd3d::WindowState::Stopped)
        {
            break;
        }
        if (HasStablePresentationContract(snapshot))
        {
            lastContractSnapshot = snapshot;
        }
        if (now >= nextTelemetry)
        {
            std::ostringstream stream;
            pbrenderd3d::WriteDataWindowSnapshotJson(stream, snapshot);
            telemetry.Write(stream.str());
            nextTelemetry = now + std::chrono::seconds(1);
        }
        if (now >= deadline)
        {
            window->RequestStop();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        const auto preparation = phase1gate::GetSenderPreparationDecision(
            snapshot.state == pbrenderd3d::WindowState::Running, frameBuilt, snapshot.pendingFrame);
        if (preparation.buildFrame)
        {
            static_cast<void>(builder.Build(frameSequence));
            frameBuilt = true;
        }
        if (preparation.submitFrame)
        {
            const auto& pixels = builder.GetBuiltPixels();
            const auto submit = window->SubmitFrame({pixels, pbmodulation::kLocalDesktopCanvasWidth,
                pbmodulation::kLocalDesktopCanvasHeight, static_cast<std::size_t>(pbmodulation::kLocalDesktopCanvasWidth) * 4U,
                frameSequence, snapshot.timing.presentationEpoch});
            if (submit)
            {
                Require(frameSequence != std::numeric_limits<std::uint64_t>::max(),
                    "FrameSequence exhausted");
                builder.Advance();
                CheckedIncrement(frameSequence, "sender FrameSequence overflow");
                frameBuilt = false;
            }
            else if (submit.code != pbrenderd3d::PresentationErrorCode::EpochMismatch &&
                submit.code != pbrenderd3d::PresentationErrorCode::Paused &&
                submit.code != pbrenderd3d::PresentationErrorCode::NotRunning)
            {
                throw GateFailure("DataWindow frame submission failed");
            }
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    window->Stop();
    const auto stoppedSnapshot = window->GetSnapshot();
    VerifySourceStable(source);
    const auto finished = std::chrono::steady_clock::now();
    const std::uint64_t elapsedMilliseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(finished - started).count());
    Require(frameSequence != 0 && lastContractSnapshot && HasStablePresentationContract(*lastContractSnapshot) &&
        stoppedSnapshot.state == pbrenderd3d::WindowState::Stopped && static_cast<bool>(stoppedSnapshot.error) &&
        stoppedSnapshot.totalPresentCalls != 0 && stoppedSnapshot.totalSuccessfulPresents == stoppedSnapshot.totalPresentCalls &&
        !stoppedSnapshot.pendingFrame && !stoppedSnapshot.inFlightFrame,
        "sender presentation contract was not maintained");
    const std::string finalJson = SenderFinalJson(profile, description, builder, *lastContractSnapshot,
        stoppedSnapshot, frameSequence, elapsedMilliseconds);
    telemetry.Write(finalJson);
    std::cout << finalJson << '\n';
    return 0;
}

class NativeCaptureSession
{
public:
    NativeCaptureSession(const phase1gate::CaptureBackend backend, const CaptureNormalizeConfig& config,
        const std::shared_ptr<pbcapturenormalize::ScreenCaptureConsumer>& consumer)
    {
        if (backend == phase1gate::CaptureBackend::Wgc)
        {
            const auto status = pbscreencapturewgc::WgcCapture::CreateNormalized(config, consumer, wgc_);
            Require(static_cast<bool>(status), "WGC normalized capture creation failed: " + DescribeCaptureStatus(status));
        }
        else
        {
            const auto status = pbscreencapturedxgi::DxgiCapture::Create(config, consumer, dxgi_);
            Require(static_cast<bool>(status), "DXGI normalized capture creation failed: " + DescribeCaptureStatus(status));
        }
    }

    ~NativeCaptureSession()
    {
        if (!stopped_)
        {
            static_cast<void>(Stop());
        }
    }

    NativeCaptureSession(const NativeCaptureSession&) = delete;
    NativeCaptureSession& operator=(const NativeCaptureSession&) = delete;

    [[nodiscard]] CaptureSnapshot GetSnapshot() const noexcept
    {
        return wgc_ ? wgc_->GetSnapshot() : dxgi_->GetSnapshot();
    }

    [[nodiscard]] pbcapturenormalize::CaptureNormalizeSnapshot GetNormalizationSnapshot() const noexcept
    {
        return wgc_ ? wgc_->GetNormalizationSnapshot() : dxgi_->GetNormalizationSnapshot();
    }

    [[nodiscard]] CaptureStatus Stop() noexcept
    {
        if (stopped_)
        {
            return stopStatus_;
        }
        stopStatus_ = wgc_ ? wgc_->Stop() : dxgi_->Stop();
        stopped_ = true;
        return stopStatus_;
    }

    void RequestRecreate() noexcept
    {
        if (wgc_)
        {
            pbscreencapturewgc::WgcCaptureTestAccess::RequestRecreate(*wgc_);
        }
        else
        {
            pbscreencapturedxgi::DxgiCaptureTestAccess::RequestRecreate(*dxgi_);
        }
    }

private:
    std::unique_ptr<pbscreencapturewgc::WgcCapture> wgc_;
    std::unique_ptr<pbscreencapturedxgi::DxgiCapture> dxgi_;
    CaptureStatus stopStatus_{};
    bool stopped_ = false;
};

struct CaptureSessionEvidence
{
    CaptureSnapshot capture;
    pbcapturenormalize::CaptureNormalizeSnapshot normalization;
    pbdemodd3d11::CaptureDemodulatorSnapshot demodulation;
};

class FileReceiver
{
public:
    FileReceiver(pbreceiver::ReceiverIngress& receiver, std::wstring finalPath,
        const pbprotocol::ReceiverResourcePolicy& policy, const std::uint64_t qpcFrequency)
        : receiver_(receiver), finalPath_(std::move(finalPath)), partPath_(finalPath_ + L".part"),
          policy_(policy), qpcFrequency_(qpcFrequency)
    {
        RequirePathAbsent(finalPath_, "final output");
        RequirePathAbsent(partPath_, ".part output");
    }

    void ObserveVisual(const pbdemodd3d11::CaptureDemodulatorResult& result,
        const pbprotocol::BootstrapRecord& bootstrap)
    {
        phase1gate::VisualIdentityDisposition disposition = phase1gate::VisualIdentityDisposition::Unique;
        Require(visualRate_.Observe(bootstrap.frameSequence, result.metadata.domain.captureEpoch,
            result.metadata.timestamp.monotonic100ns, disposition),
            "unique visual cadence received an invalid or overflowing identity/timestamp");
        if (disposition == phase1gate::VisualIdentityDisposition::Unique)
        {
            const std::uint64_t now = QpcNow();
            if (!firstUniqueVisualQpc_)
            {
                firstUniqueVisualQpc_ = now;
            }
            lastUniqueVisualQpc_ = now;
        }
    }

    void Process(const pbdemodd3d11::CaptureDemodulatorResult& result)
    {
        const auto parsedBootstrap = pbprotocol::ParseBootstrapRecord(result.bootstrapRecord);
        RequireResult(parsedBootstrap, "CaptureDemodulator published an invalid Bootstrap");
        const pbprotocol::BootstrapRecord& bootstrap = parsedBootstrap.Value();
        if (session_)
        {
            const pbprotocol::SessionTag activeSessionTag = pbprotocol::DeriveSessionTag(session_->sessionId);
            if (phase1gate::ClassifySessionIdentity(true, activeSessionTag.value, bootstrap.sessionTag.value) ==
                phase1gate::SessionIdentityDisposition::Foreign)
            {
                CheckedIncrement(foreignSessionErasedFrames_, "foreign-session erasure counter overflow");
                Require(foreignSessionErasedFrames_ <= maximumForeignSessionErasedFrames,
                    "foreign-session erasures exceeded the Gate bound");
                return;
            }
        }
        if (result.kind == pbdemodd3d11::CaptureDemodulatorResultKind::ControlRecord)
        {
            ProcessControlRecord(std::span(result.controlBytes).first(result.controlByteCount), bootstrap.sessionTag);
        }
        else if (result.kind == pbdemodd3d11::CaptureDemodulatorResultKind::ControlFragment)
        {
            ProcessControlFragment(std::span(result.controlBytes).first(result.controlByteCount),
                result.metadata.captureObservation);
        }
        else
        {
            CheckedIncrement(evaluatedDataFrames_, "evaluated data-frame counter overflow");
            if (result.demodulation.evaluation.IsVerified())
            {
                CheckedIncrement(verifiedDataFrames_, "verified data-frame counter overflow");
            }
            else
            {
                CheckedIncrement(postFecFailedFrames_, "post-FEC failed-frame counter overflow");
            }
            CheckedAdd(acceptedTransportBlocks_, result.demodulation.acceptedTransportBlockCount,
                "accepted Transport-block counter overflow");
            for (std::uint32_t index = 0; index < result.demodulation.acceptedTransportBlockCount; index++)
            {
                const auto& accepted = result.demodulation.acceptedTransportBlocks[index];
                Require(accepted.byteCount >= pbprotocol::kTransportMinimumBlockBytes &&
                    accepted.byteCount <= accepted.bytes.size(), "accepted Transport byte count is invalid");
                const auto parsed = pbprotocol::ParseTransportBlock(
                    std::span(accepted.bytes).first(accepted.byteCount));
                RequireResult(parsed, "accepted Transport failed an independent parse");
                const auto& transport = parsed.Value();
                Require(transport.header.payloadBytes == transport.payload.size() &&
                    transport.payload.size() <= paddedPayload_.size(), "accepted Transport payload is out of bounds");
                std::fill(paddedPayload_.begin(), paddedPayload_.end(), std::byte{0});
                std::copy(transport.payload.begin(), transport.payload.end(), paddedPayload_.begin());
                const pbreceiver::ReceivedTransportBlock receiverBlock{transport.header.sessionTag,
                    transport.header.segmentOrdinal, transport.header.outerBlockId, transport.header.payloadBytes,
                    paddedPayload_};
                auto admission = receiver_.ReceiveDataBlock(receiverBlock, result.metadata.captureObservation);
                if (!admission)
                {
                    const auto* protocolError = std::get_if<pbprotocol::ProtocolError>(&admission.Error());
                    const auto nextDrop = pbprotocol::CheckedAddUint64(orphanQuotaDrops_, 1);
                    const auto resourceTelemetry = receiver_.GetTelemetry();
                    if (protocolError != nullptr && protocolError->code == pbprotocol::ProtocolErrorCode::ResourceLimitExceeded &&
                        nextDrop && resourceTelemetry.orphanDroppedByQuotaCount == nextDrop.Value())
                    {
                        orphanQuotaDrops_ = nextDrop.Value();
                        continue;
                    }
                    throw GateFailure("ReceiverIngress rejected an independently parsed Transport block: " +
                        DescribeReceiverError(admission.Error()) + " outerBlockId=" + std::to_string(transport.header.outerBlockId) +
                        " observation=" + std::to_string(result.metadata.captureObservation));
                }
                CheckedIncrement(receiverDataBlocks_, "Receiver data-block counter overflow");
                if (admission.Value().completedSegment)
                {
                    StoreCompleted(std::move(*admission.Value().completedSegment));
                }
            }
        }
        if (!session_)
        {
            CheckedIncrement(unboundSessionVisualFrames_, "unbound-session visual counter overflow");
            Require(unboundSessionVisualFrames_ <= maximumUnboundSessionVisualFrames,
                "unbound-session visuals exceeded the Gate bound");
            return;
        }
        Require(bootstrap.sessionTag == pbprotocol::DeriveSessionTag(session_->sessionId),
            "newly bound SessionDescriptor disagrees with the frame Bootstrap");
        ObserveVisual(result, bootstrap);
    }

    void CaptureEpochReset() noexcept
    {
        receiverSessionBound_ = false;
    }

    [[nodiscard]] bool IsFilePublished() const noexcept
    {
        return filePublished_;
    }

    [[nodiscard]] std::uint64_t GetFilePublishedQpc() const noexcept
    {
        return filePublishedQpc_;
    }

    [[nodiscard]] std::uint64_t GetUniqueVisualFrames() const noexcept
    {
        return visualRate_.GetSnapshot().uniqueFrames;
    }

    [[nodiscard]] std::uint64_t GetUniqueVisualCadenceIntervals() const noexcept
    {
        return visualRate_.GetSnapshot().cadenceIntervals;
    }

    [[nodiscard]] double GetUniqueVisualFps() const
    {
        return visualRate_.GetCadenceFps();
    }

    [[nodiscard]] double GetEndToEndUniqueVisualFps() const
    {
        const auto visual = visualRate_.GetSnapshot();
        if (!firstUniqueVisualQpc_ || lastUniqueVisualQpc_ <= *firstUniqueVisualQpc_ || visual.uniqueFrames < 2)
        {
            return 0;
        }
        return static_cast<double>(visual.uniqueFrames - 1U) /
            ElapsedSeconds(*firstUniqueVisualQpc_, lastUniqueVisualQpc_, qpcFrequency_);
    }

    [[nodiscard]] double GetVerifiedEncodedGoodputBytesPerSecond() const
    {
        if (!transferStartQpc_ || encodedVerifiedQpc_ <= *transferStartQpc_)
        {
            return 0;
        }
        return static_cast<double>(verifiedEncodedBytes_) /
            ElapsedSeconds(*transferStartQpc_, encodedVerifiedQpc_, qpcFrequency_);
    }

    [[nodiscard]] double GetPostFecFer() const noexcept
    {
        return evaluatedDataFrames_ == 0 ? std::numeric_limits<double>::quiet_NaN() :
            static_cast<double>(postFecFailedFrames_) / static_cast<double>(evaluatedDataFrames_);
    }

    [[nodiscard]] std::string FinalJson(const ProfileBinding& profile,
        const phase1gate::CaptureBackend backend, const std::span<const CaptureSessionEvidence> sessions,
        const std::uint64_t receiverEpochResets, const std::uint64_t elapsedMilliseconds,
        const std::uint32_t soakSeconds, const std::uint32_t requiredPostPublishObservationSeconds,
        const double postPublishObservationSeconds) const
    {
        Require(filePublished_ && session_ && segment_ && manifest_, "cannot serialize an incomplete receiver Gate");
        const auto visual = visualRate_.GetSnapshot();
        std::uint64_t captureArrivals = 0;
        std::uint64_t captureDelivered = 0;
        std::uint64_t captureDrops = 0;
        std::uint64_t captureExpired = 0;
        std::uint64_t roiTimingSamples = 0;
        std::uint64_t roiTimingUnavailable = 0;
        std::uint64_t roiTimeTotal100ns = 0;
        std::uint64_t bootstrapRejected = 0;
        std::uint64_t bootstrapMismatch = 0;
        std::uint64_t controlFailures = 0;
        std::uint64_t resultQueueDrops = 0;
        std::uint64_t staleResultDrops = 0;
        std::uint64_t demodGpuSamples = 0;
        std::uint64_t demodGpuUnavailable = 0;
        std::uint64_t demodGpuTime100ns = 0;
        std::uint64_t bootstrapCpuSamples = 0;
        std::uint64_t bootstrapCpuTime100ns = 0;
        std::uint64_t demodCpuSamples = 0;
        std::uint64_t demodCpuTime100ns = 0;
        std::uint64_t cpuTimingUnavailable = 0;
        std::uint64_t inPlaceCaptureRecreates = 0;
        std::uint64_t domainStarts = 0;
        std::uint64_t domainInvalidations = 0;
        std::uint32_t frameLeaseHighWater = 0;
        std::uint32_t pendingHighWater = 0;
        std::uint32_t resultQueueHighWater = 0;
        bool allShutdownComplete = true;
        bool anyDeferredCleanup = false;
        bool allConsumersShutdown = true;
        bool allDomainsInactive = true;
        for (const auto& evidence : sessions)
        {
            CheckedAdd(captureArrivals, evidence.capture.arrivedFrames, "capture-arrival evidence overflow");
            CheckedAdd(captureDelivered, evidence.capture.deliveredFrames, "capture-delivery evidence overflow");
            CheckedAdd(captureDrops, evidence.capture.droppedFrames, "capture-drop evidence overflow");
            CheckedAdd(captureExpired, evidence.capture.expiredFrames, "capture-expiry evidence overflow");
            CheckedAdd(roiTimingSamples, evidence.capture.roiCopyTimingSamples, "ROI timing-sample evidence overflow");
            CheckedAdd(roiTimingUnavailable, evidence.capture.roiCopyTimingUnavailable, "ROI unavailable-timing evidence overflow");
            CheckedAdd(roiTimeTotal100ns, evidence.capture.roiCopyTimeTotal100ns, "ROI GPU-time evidence overflow");
            frameLeaseHighWater = std::max(frameLeaseHighWater, evidence.capture.frameLeaseHighWater);
            CheckedAdd(bootstrapRejected, evidence.demodulation.bootstrapRejectedFrames,
                "Bootstrap-rejection evidence overflow");
            const std::size_t mismatchIndex = static_cast<std::size_t>(
                pbmodulation::LocalDesktopErasureReason::BootstrapMismatch);
            Require(mismatchIndex < evidence.demodulation.bootstrapErasures.size(),
                "BootstrapMismatch erasure index is outside telemetry");
            CheckedAdd(bootstrapMismatch, evidence.demodulation.bootstrapErasures[mismatchIndex],
                "Bootstrap-mismatch evidence overflow");
            CheckedAdd(controlFailures, evidence.demodulation.controlFrameFailures,
                "Control failure evidence overflow");
            CheckedAdd(resultQueueDrops, evidence.demodulation.resultQueueDrops,
                "result-queue drop evidence overflow");
            CheckedAdd(staleResultDrops, evidence.demodulation.staleResultDrops,
                "stale-result evidence overflow");
            CheckedAdd(demodGpuSamples, evidence.demodulation.demodulator.gpuTimingSamples,
                "demod GPU timing-sample evidence overflow");
            CheckedAdd(demodGpuUnavailable, evidence.demodulation.demodulator.gpuTimingUnavailable,
                "demod unavailable-timing evidence overflow");
            CheckedAdd(demodGpuTime100ns, evidence.demodulation.demodulator.gpuTimeTotal100ns,
                "demod GPU-time evidence overflow");
            CheckedAdd(bootstrapCpuSamples, evidence.demodulation.bootstrapCpuTimingSamples,
                "Bootstrap CPU timing-sample evidence overflow");
            CheckedAdd(bootstrapCpuTime100ns, evidence.demodulation.bootstrapCpuTimeTotal100ns,
                "Bootstrap CPU-time evidence overflow");
            CheckedAdd(demodCpuSamples, evidence.demodulation.demodulationCpuTimingSamples,
                "post-GPU CPU timing-sample evidence overflow");
            CheckedAdd(demodCpuTime100ns, evidence.demodulation.demodulationCpuTimeTotal100ns,
                "post-GPU CPU-time evidence overflow");
            CheckedAdd(cpuTimingUnavailable, evidence.demodulation.cpuTimingUnavailable,
                "CPU unavailable-timing evidence overflow");
            CheckedAdd(inPlaceCaptureRecreates, evidence.capture.recreates,
                "in-place capture recreation evidence overflow");
            CheckedAdd(domainStarts, evidence.demodulation.domainStarts,
                "capture domain-start evidence overflow");
            CheckedAdd(domainInvalidations, evidence.demodulation.invalidations,
                "capture domain-invalidation evidence overflow");
            pendingHighWater = std::max(pendingHighWater, evidence.demodulation.pendingHighWater);
            resultQueueHighWater = std::max(resultQueueHighWater, evidence.demodulation.resultQueueHighWater);
            allShutdownComplete = allShutdownComplete && evidence.capture.shutdownComplete &&
                evidence.capture.liveFrameLeases == 0 && evidence.capture.busyRoiTextures == 0;
            anyDeferredCleanup = anyDeferredCleanup || evidence.capture.deferredCleanup;
            allConsumersShutdown = allConsumersShutdown && evidence.demodulation.demodulator.shutdown &&
                evidence.demodulation.pendingFrames == 0 && evidence.demodulation.demodulator.pendingFrames == 0 &&
                evidence.demodulation.queuedResults == 0;
            allDomainsInactive = allDomainsInactive && !evidence.demodulation.active && !evidence.normalization.active;
        }
        Require(evaluatedDataFrames_ != 0 && verifiedDataFrames_ != 0 && acceptedTransportBlocks_ != 0,
            "no production Transport/FEC baseline was measured");
        const auto accountedTransportBlocks = pbprotocol::CheckedAddUint64(receiverDataBlocks_, orphanQuotaDrops_);
        RequireResult(accountedTransportBlocks, "Transport admission accounting overflow");
        const auto receiverResources = receiver_.GetTelemetry();
        Require(accountedTransportBlocks.Value() == acceptedTransportBlocks_ &&
            receiverResources.orphanDroppedByQuotaCount == orphanQuotaDrops_ &&
            receiverResources.totalResourcePolicyRejectedCount == orphanQuotaDrops_ &&
            receiverResources.orphanResourceExhaustedCount == 0 &&
            receiverResources.orphanConflictRejectionCount == 0 && receiverResources.orphanCachedBlockCount == 0,
            "bounded orphan-cache drops or receiver resource accounting are inconsistent");
        const auto terminalCaptureFrames = pbprotocol::CheckedAddUint64(captureDrops, captureExpired);
        RequireResult(terminalCaptureFrames, "capture terminal-frame accounting overflow");
        Require(captureArrivals != 0 && captureDelivered != 0 && captureDelivered <= captureArrivals &&
            terminalCaptureFrames.Value() <= captureArrivals,
            "capture arrival/delivery/drop/expiry accounting is inconsistent");
        const double verifiedGoodput = GetVerifiedEncodedGoodputBytesPerSecond();
        const double uniqueVisualFps = GetUniqueVisualFps();
        const double endToEndUniqueVisualFps = GetEndToEndUniqueVisualFps();
        if (!(verifiedGoodput > 0 && uniqueVisualFps > 0 && endToEndUniqueVisualFps > 0 &&
              phase1gate::HasMinimumUniqueVisualCadence(visual.cadenceIntervals)))
        {
            std::ostringstream error;
            error << std::setprecision(17) << "goodput or UniqueVisualFPS denominator is empty: goodput=" << verifiedGoodput
                  << " uniqueVisualFps=" << uniqueVisualFps << " endToEndUniqueVisualFps=" << endToEndUniqueVisualFps
                  << " cadenceIntervals=" << visual.cadenceIntervals;
            throw GateFailure(error.str());
        }
        Require(roiTimingSamples != 0 && demodGpuSamples != 0 && bootstrapCpuSamples != 0 && demodCpuSamples != 0,
            "GPU/CPU cost lacks a real timing sample");
        Require(cpuTimingUnavailable == 0, "CPU timing became unavailable during Gate");
        Require(resultQueueDrops == 0 && staleResultDrops <= 128,
            "bounded result queue overflowed or epoch retirement exceeded its fixed result capacity");
        Require(foreignSessionErasedFrames_ <= maximumForeignSessionErasedFrames &&
            retryableUnknownSessionControlDrops_ <= maximumRetryableUnknownSessionControlDrops &&
            unboundSessionVisualFrames_ <= maximumUnboundSessionVisualFrames,
            "foreign-session, pre-rebind Control, or unbound visual erasures exceeded the Gate bound");
        Require(controlFailures == 0, "fixed Control-plane raster failed to decode");
        Require(bootstrapRejected != 0 && bootstrapMismatch != 0,
            "no A/B BootstrapMismatch erasure was observed for an injected mixed/torn frame");
        Require(frameLeaseHighWater <= maximumFrameLeaseHighWater &&
            pendingHighWater <= captureDemodulatorSlotCount && resultQueueHighWater <= captureResultQueueCapacity,
            "FrameLease, GPU-pending, or result backlog exceeded its configured bound");
        Require(roiTimeTotal100ns != 0 && demodGpuTime100ns != 0,
            "GPU timing samples reported a zero total cost");
        Require(allShutdownComplete && !anyDeferredCleanup && allConsumersShutdown && allDomainsInactive,
            "capture/demod resource lifecycle was incomplete");
        Require(inPlaceCaptureRecreates != 0 && receiverEpochResets == 2 && domainStarts >= 3 && domainInvalidations >= 3,
            "requested recreate, external restart, or CaptureEpoch reset evidence is incomplete");
        const auto AverageMilliseconds = [](const std::uint64_t total100ns, const std::uint64_t samples)
        {
            return samples == 0 ? std::numeric_limits<double>::quiet_NaN() :
                static_cast<double>(total100ns) / static_cast<double>(samples) / 10000.0;
        };
        const double finalVerificationMilliseconds = encodedVerifiedQpc_ != 0 && filePublishedQpc_ >= encodedVerifiedQpc_ ?
            ElapsedSeconds(encodedVerifiedQpc_, filePublishedQpc_, qpcFrequency_) * 1000.0 :
            std::numeric_limits<double>::quiet_NaN();
        const double captureDeliveryRatio = captureArrivals == 0 ? std::numeric_limits<double>::quiet_NaN() :
            static_cast<double>(captureDelivered) / static_cast<double>(captureArrivals);
        std::ostringstream stream;
        stream << std::boolalpha << std::setprecision(17)
               << "{\"event\":\"phase1-receiver-final\",\"status\":\"pass\",\"profile\":\"" << profile.name
               << "\",\"backend\":\"" << (backend == phase1gate::CaptureBackend::Wgc ? "wgc" : "dxgi")
               << "\",\"sessionTag\":\"" << segment_->sessionTag.value
               << "\",\"sourceBytes\":" << session_->originalFileSize
               << ",\"verifiedEncodedBytes\":" << verifiedEncodedBytes_
               << ",\"wholeFileDigest\":\"" << DigestHex(manifest_->wholeFileDigest.bytes)
               << "\",\"wholeFileDigestVerified\":true,\"publishedFinalDigestVerified\":true"
               << ",\"storedSegments\":" << storedSegments_
               << ",\"verifiedEncodedGoodputBytesPerSecond\":" << GetVerifiedEncodedGoodputBytesPerSecond()
               << ",\"verifiedEncodedGoodputMiBPerSecond\":" << GetVerifiedEncodedGoodputBytesPerSecond() / static_cast<double>(mebibyte)
               << ",\"postFecFer\":" << GetPostFecFer()
               << ",\"evaluatedDataFrames\":" << evaluatedDataFrames_
               << ",\"verifiedDataFrames\":" << verifiedDataFrames_
               << ",\"postFecFailedFrames\":" << postFecFailedFrames_
               << ",\"acceptedTransportBlocks\":" << acceptedTransportBlocks_
               << ",\"receiverDataBlocks\":" << receiverDataBlocks_
               << ",\"orphanQuotaDrops\":" << orphanQuotaDrops_
               << ",\"resourcePolicyRejectedCount\":" << receiverResources.totalResourcePolicyRejectedCount
               << ",\"orphanResourceExhaustedCount\":" << receiverResources.orphanResourceExhaustedCount
               << ",\"orphanConflictRejectionCount\":" << receiverResources.orphanConflictRejectionCount
               << ",\"orphanCachedBlockCount\":" << receiverResources.orphanCachedBlockCount
               << ",\"controlRecords\":" << controlRecords_
               << ",\"controlFragments\":" << controlFragments_
               << ",\"foreignSessionErasedFrames\":" << foreignSessionErasedFrames_
               << ",\"retryableUnknownSessionControlDrops\":" << retryableUnknownSessionControlDrops_
               << ",\"unboundSessionVisualFrames\":" << unboundSessionVisualFrames_
               << ",\"uniqueVisualFrames\":" << visual.uniqueFrames
               << ",\"duplicateVisualFrames\":" << visual.duplicateFrames
               << ",\"reorderedVisualFrames\":" << visual.reorderedFrames
               << ",\"uniqueVisualFps\":" << GetUniqueVisualFps()
               << ",\"endToEndUniqueVisualFps\":" << GetEndToEndUniqueVisualFps()
               << ",\"uniqueVisualCadenceIntervals\":" << visual.cadenceIntervals
               << ",\"uniqueVisualCadenceTime100ns\":" << visual.cadenceTime100ns
               << ",\"uniqueVisualGapEvents\":" << visual.gapEvents
               << ",\"skippedVisualSequences\":" << visual.skippedSequences
               << ",\"visualCaptureEpochBoundaries\":" << visual.captureEpochBoundaries
               << ",\"captureSessions\":" << sessions.size()
               << ",\"captureRestarts\":" << (sessions.empty() ? 0 : sessions.size() - 1U)
               << ",\"inPlaceCaptureRecreates\":" << inPlaceCaptureRecreates
               << ",\"receiverCaptureEpochResets\":" << receiverEpochResets
               << ",\"captureDomainStarts\":" << domainStarts
               << ",\"captureDomainInvalidations\":" << domainInvalidations
               << ",\"captureArrivals\":" << captureArrivals
               << ",\"captureDelivered\":" << captureDelivered
               << ",\"captureDeliveryRatio\":" << captureDeliveryRatio
                << ",\"captureDrops\":" << captureDrops
                << ",\"captureExpired\":" << captureExpired
                << ",\"captureQueuedFrameLimit\":" << captureQueuedFrameLimit
                << ",\"roiTextureCount\":" << captureDemodulatorSlotCount
                << ",\"consumerSlotCount\":" << captureDemodulatorSlotCount
                << ",\"resultQueueCapacity\":" << captureResultQueueCapacity
                << ",\"frameLeaseHighWater\":" << frameLeaseHighWater
               << ",\"consumerPendingHighWater\":" << pendingHighWater
               << ",\"resultQueueHighWater\":" << resultQueueHighWater
               << ",\"resultQueueDrops\":" << resultQueueDrops
               << ",\"staleResultDrops\":" << staleResultDrops
               << ",\"bootstrapRejectedFrames\":" << bootstrapRejected
               << ",\"bootstrapMismatchFrames\":" << bootstrapMismatch
               << ",\"controlFrameFailures\":" << controlFailures
               << ",\"roiGpuTimingSamples\":" << roiTimingSamples
               << ",\"roiGpuTimingUnavailable\":" << roiTimingUnavailable
               << ",\"roiGpuAverageMilliseconds\":" << AverageMilliseconds(roiTimeTotal100ns, roiTimingSamples)
               << ",\"demodGpuTimingSamples\":" << demodGpuSamples
               << ",\"demodGpuTimingUnavailable\":" << demodGpuUnavailable
               << ",\"demodGpuAverageMilliseconds\":" << AverageMilliseconds(demodGpuTime100ns, demodGpuSamples)
               << ",\"bootstrapCpuTimingSamples\":" << bootstrapCpuSamples
               << ",\"bootstrapCpuAverageMilliseconds\":" << AverageMilliseconds(bootstrapCpuTime100ns, bootstrapCpuSamples)
               << ",\"postGpuFecCpuTimingSamples\":" << demodCpuSamples
               << ",\"postGpuFecCpuAverageMilliseconds\":" << AverageMilliseconds(demodCpuTime100ns, demodCpuSamples)
               << ",\"cpuTimingUnavailable\":" << cpuTimingUnavailable
               << ",\"finalVerificationMilliseconds\":" << finalVerificationMilliseconds
               << ",\"allCaptureShutdownComplete\":" << allShutdownComplete
               << ",\"anyDeferredCleanup\":" << anyDeferredCleanup
               << ",\"allConsumersShutdown\":" << allConsumersShutdown
               << ",\"allDomainsInactive\":" << allDomainsInactive
               << ",\"soakSeconds\":" << soakSeconds
               << ",\"requiredPostPublishObservationSeconds\":" << requiredPostPublishObservationSeconds
               << ",\"postPublishObservationSeconds\":" << postPublishObservationSeconds
               << ",\"elapsedMilliseconds\":" << elapsedMilliseconds << '}';
        return stream.str();
    }

private:
    void CreatePartFile(const pbprotocol::SessionDescriptor& session)
    {
        Require(session.originalFileSize == sourceBytes && session.segmentCount == 1,
            "Gate receiver admits exactly one bounded 8 MiB Segment");
        partHandle_.Reset(CreateFileW(partPath_.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
        if (!partHandle_)
        {
            throw GateFailure(WindowsFailure("create output.part"));
        }
        LARGE_INTEGER finalSize{};
        finalSize.QuadPart = static_cast<LONGLONG>(session.originalFileSize);
        Require(SetFilePointerEx(partHandle_.Get(), finalSize, nullptr, FILE_BEGIN) != FALSE &&
            SetEndOfFile(partHandle_.Get()) != FALSE && FlushFileBuffers(partHandle_.Get()) != FALSE,
            "output.part bounded preallocation failed");
    }

    [[nodiscard]] bool IsRetryableUnknownSessionControlError(const pbreceiver::ReceiverError& error) const noexcept
    {
        const auto* protocolError = std::get_if<pbprotocol::ProtocolError>(&error);
        return !receiverSessionBound_ && protocolError != nullptr &&
            protocolError->code == pbprotocol::ProtocolErrorCode::UnknownSession;
    }

    void ProcessControlRecord(const std::span<const std::byte> bytes,
        const pbprotocol::SessionTag bootstrapSessionTag)
    {
        const auto record = pbprotocol::ParseControlRecord(bytes);
        RequireResult(record, "captured ControlRecord failed an independent parse");
        Require(record.Value().sessionTag == bootstrapSessionTag,
            "captured ControlRecord SessionTag disagrees with its frame Bootstrap");
        auto admission = receiver_.ReceiveControlRecord(bytes);
        if (!admission)
        {
            if (record.Value().recordType != pbprotocol::ControlRecordType::SessionDescriptor &&
                IsRetryableUnknownSessionControlError(admission.Error()))
            {
                CheckedIncrement(retryableUnknownSessionControlDrops_,
                    "retryable unknown-session Control drop counter overflow");
                Require(retryableUnknownSessionControlDrops_ <= maximumRetryableUnknownSessionControlDrops,
                    "retryable unknown-session Control drops exceeded the Gate bound");
                return;
            }
            throw GateFailure("ReceiverIngress rejected captured ControlRecord: " +
                DescribeReceiverError(admission.Error()));
        }
        CheckedIncrement(controlRecords_, "control-record counter overflow");
        HandleControlAdmission(admission.Value(), record.Value());
    }

    void ProcessControlFragment(const std::span<const std::byte> bytes,
        const std::uint64_t captureObservation)
    {
        auto fragment = receiver_.ReceiveControlFragment(bytes, captureObservation);
        if (!fragment)
        {
            if (IsRetryableUnknownSessionControlError(fragment.Error()))
            {
                CheckedIncrement(retryableUnknownSessionControlDrops_,
                    "retryable unknown-session Control drop counter overflow");
                Require(retryableUnknownSessionControlDrops_ <= maximumRetryableUnknownSessionControlDrops,
                    "retryable unknown-session Control drops exceeded the Gate bound");
                return;
            }
            throw GateFailure("ReceiverIngress rejected captured ControlFragment: " +
                DescribeReceiverError(fragment.Error()));
        }
        CheckedIncrement(controlFragments_, "control-fragment counter overflow");
        if (fragment.Value().admission)
        {
            HandleControlAdmission(*fragment.Value().admission, std::nullopt);
        }
    }

    void HandleControlAdmission(pbreceiver::ReceiverControlAdmission& admission,
        const std::optional<pbprotocol::ControlRecordView> record)
    {
        if (!transferStartQpc_)
        {
            transferStartQpc_ = QpcNow();
        }
        if (record)
        {
            if (record->recordType == pbprotocol::ControlRecordType::SessionDescriptor)
            {
                const auto parsed = pbprotocol::ParseSessionDescriptor(record->payload, policy_);
                RequireResult(parsed, "SessionDescriptor independent parse failed");
                Require(pbprotocol::DeriveSessionTag(parsed.Value().sessionId) == record->sessionTag,
                    "SessionDescriptor tag mismatch");
                Require(admission.outputReservationDecision &&
                    *admission.outputReservationDecision == pbprotocol::OutputReservationDecision::AutoAccept,
                    "Gate Session output reservation was not AutoAccept");
                if (!session_)
                {
                    session_ = parsed.Value();
                    CreatePartFile(*session_);
                }
                else
                {
                    Require(*session_ == parsed.Value(), "conflicting repeated SessionDescriptor");
                }
                receiverSessionBound_ = true;
            }
            else if (record->recordType == pbprotocol::ControlRecordType::SegmentDescriptor)
            {
                Require(session_.has_value(), "SegmentDescriptor arrived before SessionDescriptor in Gate schedule");
                const auto parsed = pbprotocol::ParseSegmentDescriptor(record->payload, *session_, policy_);
                RequireResult(parsed, "SegmentDescriptor independent parse failed");
                Require(admission.controlAdmission.boundSegmentDescriptor &&
                    admission.controlAdmission.boundSegmentDescriptor->GetDescriptor() == parsed.Value(),
                    "Receiver binding differs from independently parsed SegmentDescriptor");
                if (!segment_)
                {
                    segment_ = parsed.Value();
                }
                else
                {
                    Require(*segment_ == parsed.Value(), "conflicting repeated SegmentDescriptor");
                }
            }
            else if (record->recordType == pbprotocol::ControlRecordType::FinalManifest)
            {
                Require(session_.has_value(), "FinalManifest arrived before SessionDescriptor in Gate schedule");
                const auto parsed = pbprotocol::ParseFinalManifest(record->payload, *session_, policy_);
                RequireResult(parsed, "FinalManifest independent parse failed");
                if (!manifest_)
                {
                    manifest_ = parsed.Value();
                }
                else
                {
                    Require(*manifest_ == parsed.Value(), "conflicting repeated FinalManifest");
                }
            }
        }
        if (admission.completedSegment)
        {
            StoreCompleted(std::move(*admission.completedSegment));
        }
        TryPublish();
    }

    void StoreCompleted(pbreceiver::ReceiverCompletedSegment&& completed)
    {
        Require(storedSegments_ == 0, "Receiver emitted a duplicate completed Segment after authoritative storage");
        const std::uint64_t encodedBytes = completed.encodedBytes.size();
        auto verified = receiver_.VerifyRecoveredSegment(std::move(completed));
        RequireResult(verified, "encoded digest, decompression, or raw digest verification failed");
        pbreceiver::ReceiverVerifiedSegment verifiedSegment = std::move(verified).Value();
        const pbprotocol::SegmentDescriptor& descriptor =
            verifiedSegment.GetBoundSegmentDescriptor().GetDescriptor();
        Require(partHandle_ && session_ && descriptor.rawOffset == 0 &&
            descriptor.rawSize == verifiedSegment.GetRawBytes().size() &&
            descriptor.rawSize == session_->originalFileSize && encodedBytes == descriptor.encodedSize,
            "verified Segment does not match the bounded output reservation");
        LARGE_INTEGER offset{};
        offset.QuadPart = static_cast<LONGLONG>(descriptor.rawOffset);
        Require(SetFilePointerEx(partHandle_.Get(), offset, nullptr, FILE_BEGIN) != FALSE,
            "output.part positional seek failed");
        WriteAll(partHandle_.Get(), verifiedSegment.GetRawBytes());
        Require(FlushFileBuffers(partHandle_.Get()) != FALSE, "output.part flush failed");
        const std::uint64_t verifiedEncodedSize = descriptor.encodedSize;
        const auto committed = receiver_.CommitStoredSegment(std::move(verifiedSegment));
        RequireResult(committed, "Receiver stored Segment commit failed");
        Require(committed.Value() == pbreceiver::ReceiverSegmentCommitDisposition::Committed,
            "first stored Segment commit was not Committed");
        verifiedEncodedBytes_ = verifiedEncodedSize;
        encodedVerifiedQpc_ = QpcNow();
        CheckedIncrement(storedSegments_, "stored Segment counter overflow");
        TryPublish();
    }

    void TryPublish()
    {
        if (filePublished_ || !session_ || !segment_ || !manifest_ || storedSegments_ != 1)
        {
            return;
        }
        const auto finalized = receiver_.PrepareFinalization(segment_->sessionTag);
        RequireResult(finalized, "Receiver finalization was not authoritative after stored commit");
        Require(finalized.Value() == *manifest_, "Receiver returned a different FinalManifest");
        Require(FlushFileBuffers(partHandle_.Get()) != FALSE, "final output.part flush failed");
        partHandle_.Reset();
        Require(HashFile(partPath_) == manifest_->wholeFileDigest.bytes,
            "output.part failed sequential WholeFileDigest verification");
        RequirePathAbsent(finalPath_, "final output before atomic publication");
        Require(MoveFileExW(partPath_.c_str(), finalPath_.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE,
            "same-directory atomic publication failed");
        Require(HashFile(finalPath_) == manifest_->wholeFileDigest.bytes,
            "published final file digest differs after rename");
        filePublishedQpc_ = QpcNow();
        filePublished_ = true;
    }

    pbreceiver::ReceiverIngress& receiver_;
    std::wstring finalPath_;
    std::wstring partPath_;
    pbprotocol::ReceiverResourcePolicy policy_;
    std::uint64_t qpcFrequency_ = 0;
    UniqueHandle partHandle_;
    std::optional<pbprotocol::SessionDescriptor> session_;
    std::optional<pbprotocol::SegmentDescriptor> segment_;
    std::optional<pbprotocol::FinalManifest> manifest_;
    std::array<std::byte, outerBlockBytes> paddedPayload_{};
    std::optional<std::uint64_t> transferStartQpc_;
    phase1gate::UniqueVisualRateAccumulator visualRate_;
    std::optional<std::uint64_t> firstUniqueVisualQpc_;
    std::uint64_t lastUniqueVisualQpc_ = 0;
    std::uint64_t encodedVerifiedQpc_ = 0;
    std::uint64_t filePublishedQpc_ = 0;
    std::uint64_t verifiedEncodedBytes_ = 0;
    std::uint64_t storedSegments_ = 0;
    std::uint64_t controlRecords_ = 0;
    std::uint64_t controlFragments_ = 0;
    std::uint64_t receiverDataBlocks_ = 0;
    std::uint64_t orphanQuotaDrops_ = 0;
    std::uint64_t evaluatedDataFrames_ = 0;
    std::uint64_t verifiedDataFrames_ = 0;
    std::uint64_t postFecFailedFrames_ = 0;
    std::uint64_t acceptedTransportBlocks_ = 0;
    std::uint64_t foreignSessionErasedFrames_ = 0;
    std::uint64_t retryableUnknownSessionControlDrops_ = 0;
    std::uint64_t unboundSessionVisualFrames_ = 0;
    bool receiverSessionBound_ = false;
    bool filePublished_ = false;
};

struct ActiveCapture
{
    std::shared_ptr<pbdemodd3d11::CaptureDemodulator> consumer;
    std::unique_ptr<NativeCaptureSession> session;
};

[[nodiscard]] ActiveCapture CreateActiveCapture(const phase1gate::Arguments& options,
    const ProfileBinding& profile, const pbscreenregion::ScreenCaptureRegion& region,
    const std::uint64_t initialEpoch)
{
    pbdemodd3d11::CaptureDemodulatorConfig consumerConfig;
    consumerConfig.visualProfileId = profile.visualProfileId;
    consumerConfig.slotCount = captureDemodulatorSlotCount;
    consumerConfig.maximumFrameAgeMilliseconds = 250;
    consumerConfig.resultQueueCapacity = captureResultQueueCapacity;
    consumerConfig.maximumResidentBytes = 192ULL * mebibyte;
    consumerConfig.evaluationMode = pbdesktoplevels::EvaluationMode::Transport;
    std::shared_ptr<pbdemodd3d11::CaptureDemodulator> consumer;
    RequireResult(pbdemodd3d11::CaptureDemodulator::Create(consumerConfig, consumer),
        "CaptureDemodulator creation failed");
    CaptureNormalizeConfig captureConfig;
    captureConfig.capture.region = region;
    captureConfig.capture.initialCaptureEpoch = initialEpoch;
    captureConfig.capture.queuedFrameLimit = captureQueuedFrameLimit;
    captureConfig.capture.roiTextureCount = consumerConfig.slotCount;
    captureConfig.capture.maximumFrameAgeMilliseconds = consumerConfig.maximumFrameAgeMilliseconds;
    captureConfig.capture.pixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    captureConfig.capture.maximumRoiBytes = 256ULL * mebibyte;
    captureConfig.capture.maximumCaptureBytes = 1024ULL * mebibyte;
    captureConfig.capture.maximumDeviceRecoveries = 1;
    if (options.backend == phase1gate::CaptureBackend::Wgc)
    {
        // This optional Win11 cadence hint is faster than the 60 Hz source, so
        // it cannot throttle or synthesize visuals; it only prevents the WGC
        // session from coalescing distinct stable-vsync updates under load.
        captureConfig.capture.minUpdateInterval100ns = wgcGateMinUpdateInterval100ns;
    }
    auto session = std::make_unique<NativeCaptureSession>(options.backend, captureConfig, consumer);
    return {std::move(consumer), std::move(session)};
}

[[nodiscard]] CaptureSessionEvidence StopAndCollect(ActiveCapture& active)
{
    Require(active.session && active.consumer, "active capture is incomplete");
    const auto stop = active.session->Stop();
    RequireResult(stop, "native capture Stop failed");
    CaptureSessionEvidence evidence;
    evidence.capture = active.session->GetSnapshot();
    evidence.normalization = active.session->GetNormalizationSnapshot();
    evidence.demodulation = active.consumer->GetSnapshot();
    Require(evidence.capture.shutdownComplete && !evidence.capture.deferredCleanup &&
        evidence.capture.liveFrameLeases == 0 && evidence.capture.busyRoiTextures == 0,
        "native capture did not retire all bounded resources");
    Require(!evidence.normalization.active && !evidence.demodulation.active &&
        evidence.demodulation.pendingFrames == 0 && evidence.demodulation.demodulator.pendingFrames == 0 &&
        evidence.demodulation.queuedResults == 0 && evidence.demodulation.demodulator.shutdown,
        "CaptureDemodulator did not retire its domain and pending GPU work");
    active.session.reset();
    active.consumer.reset();
    return evidence;
}

int RunReceiver(const phase1gate::Arguments& options)
{
    NewJsonlFile telemetry(options.telemetryPath);
    const ProfileBinding profile = GetProfileBinding(options.profile);
    const RECT physicalRoi{options.physicalRoi[0], options.physicalRoi[1],
        options.physicalRoi[2], options.physicalRoi[3]};
    pbscreenregion::ScreenCaptureRegion region;
    RequireResult(pbscreenregion::ResolveScreenCaptureRegion(physicalRoi, region),
        "physical ROI resolution failed");
    const auto policy = pbprotocol::GetDefaultReceiverResourcePolicy();
    auto receiverResult = pbreceiver::ReceiverIngress::Create(policy, outerBlockBytes);
    RequireResult(receiverResult, "ReceiverIngress creation failed");
    pbreceiver::ReceiverIngress receiver = std::move(receiverResult).Value();
    const std::uint64_t qpcFrequency = QpcFrequency();
    FileReceiver fileReceiver(receiver, options.outputPath, policy, qpcFrequency);
    std::array<CaptureSessionEvidence, 2> evidenceStorage{};
    std::size_t evidenceCount = 0;
    ActiveCapture active = CreateActiveCapture(options, profile, region, 1);
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::seconds(options.timeoutSeconds);
    const std::uint32_t requiredPostPublishSeconds = options.soakSeconds == 0 ?
        regularPostPublishObservationSeconds : options.soakSeconds;
    auto nextTelemetry = started;
    bool restarted = options.restartAfterUniqueFrames == 0;
    bool inPlaceRecreateRequested = options.restartAfterUniqueFrames == 0;
    bool inPlaceRecreateCompleted = options.restartAfterUniqueFrames == 0;
    std::uint64_t receiverCaptureEpoch = 1;
    std::uint64_t receiverEpochResets = 0;
    const auto externalRestartThresholdResult = pbprotocol::CheckedAddUint64(
        options.restartAfterUniqueFrames, 30);
    RequireResult(externalRestartThresholdResult, "external capture restart threshold overflow");
    const std::uint64_t externalRestartThreshold = externalRestartThresholdResult.Value();
    double postPublishObservationSeconds = 0;
    for (;;)
    {
        const auto now = std::chrono::steady_clock::now();
        pbdemodd3d11::CaptureDemodulatorResult result;
        std::uint32_t drained = 0;
        while (drained < pbdemodd3d11::maximumCaptureDemodResultQueue && active.consumer->TakeResult(result))
        {
            const std::uint64_t resultEpoch = result.metadata.domain.captureEpoch;
            Require(resultEpoch >= receiverCaptureEpoch,
                "a stale CaptureEpoch result escaped the bounded CaptureDemodulator queue");
            if (resultEpoch > receiverCaptureEpoch)
            {
                const auto reset = receiver.ResetCaptureEpoch(outerBlockBytes);
                RequireResult(reset, "ReceiverIngress CaptureEpoch reset failed");
                Require(reset.Value(), "ReceiverIngress CaptureEpoch reset made no state transition");
                fileReceiver.CaptureEpochReset();
                receiverCaptureEpoch = resultEpoch;
                CheckedIncrement(receiverEpochResets, "Receiver CaptureEpoch reset counter overflow");
            }
            fileReceiver.Process(result);
            drained++;
        }
        const CaptureSnapshot captureSnapshot = active.session->GetSnapshot();
        const auto demodSnapshot = active.consumer->GetSnapshot();
        if (captureSnapshot.state == pbcapturenormalize::CaptureState::Failed ||
            (demodSnapshot.error.code != pbcapturenormalize::CaptureError::None &&
             captureSnapshot.state != pbcapturenormalize::CaptureState::Recreating))
        {
            const auto normalizationSnapshot = active.session->GetNormalizationSnapshot();
            std::ostringstream terminal;
            terminal << std::boolalpha
                     << "{\"event\":\"phase1-receiver-terminal\",\"captureState\":"
                     << static_cast<unsigned int>(captureSnapshot.state)
                     << ",\"captureError\":\"" << pbcapturenormalize::GetCaptureErrorName(captureSnapshot.error.code)
                     << "\",\"captureStage\":" << static_cast<unsigned int>(captureSnapshot.error.stage)
                     << ",\"captureNativeError\":" << captureSnapshot.error.nativeError
                     << ",\"consumerError\":\"" << pbcapturenormalize::GetCaptureErrorName(demodSnapshot.error.code)
                     << "\",\"consumerStage\":" << static_cast<unsigned int>(demodSnapshot.error.stage)
                     << ",\"consumerNativeError\":" << demodSnapshot.error.nativeError
                     << ",\"demodError\":\"" << pbdemodd3d11::GetDemodErrorName(demodSnapshot.lastDemodStatus.code)
                     << "\",\"demodStage\":" << static_cast<unsigned int>(demodSnapshot.lastDemodStatus.stage)
                     << ",\"demodNativeError\":" << demodSnapshot.lastDemodStatus.nativeError
                     << ",\"arrivedFrames\":" << captureSnapshot.arrivedFrames
                     << ",\"deliveredFrames\":" << captureSnapshot.deliveredFrames
                     << ",\"normalizationAcceptedFrames\":" << normalizationSnapshot.acceptedFrames
                     << ",\"normalizationErasedFrames\":" << normalizationSnapshot.erasedFrames
                     << ",\"consumerSubmittedFrames\":" << demodSnapshot.submittedFrames
                     << ",\"consumerCompletedFrames\":" << demodSnapshot.completedFrames << '}';
            telemetry.Write(terminal.str());
            throw GateFailure("native capture or CaptureDemodulator entered a terminal failure: " + terminal.str());
        }
        if (now >= nextTelemetry)
        {
            std::ostringstream stream;
            stream << std::boolalpha << std::setprecision(17)
                   << "{\"event\":\"phase1-receiver-snapshot\",\"profile\":\"" << profile.name
                   << "\",\"backend\":\"" << (options.backend == phase1gate::CaptureBackend::Wgc ? "wgc" : "dxgi")
                   << "\",\"captureState\":" << static_cast<unsigned int>(captureSnapshot.state)
                   << ",\"captureEpoch\":" << captureSnapshot.captureEpoch
                   << ",\"arrivedFrames\":" << captureSnapshot.arrivedFrames
                   << ",\"deliveredFrames\":" << captureSnapshot.deliveredFrames
                   << ",\"droppedFrames\":" << captureSnapshot.droppedFrames
                   << ",\"acquireTimeouts\":" << captureSnapshot.acquireTimeouts
                   << ",\"accumulatedFrames\":" << captureSnapshot.accumulatedFrames
                   << ",\"liveFrameLeases\":" << captureSnapshot.liveFrameLeases
                   << ",\"busyRoiTextures\":" << captureSnapshot.busyRoiTextures
                   << ",\"consumerPendingFrames\":" << demodSnapshot.pendingFrames
                   << ",\"resultQueue\":" << demodSnapshot.queuedResults
                   << ",\"uniqueVisualFrames\":" << fileReceiver.GetUniqueVisualFrames()
                   << ",\"filePublished\":" << fileReceiver.IsFilePublished() << '}';
            telemetry.Write(stream.str());
            nextTelemetry = now + std::chrono::seconds(1);
        }
        if (!inPlaceRecreateRequested && fileReceiver.GetUniqueVisualFrames() >= options.restartAfterUniqueFrames)
        {
            active.session->RequestRecreate();
            inPlaceRecreateRequested = true;
        }
        if (inPlaceRecreateRequested && !inPlaceRecreateCompleted && captureSnapshot.captureEpoch > 1 &&
            captureSnapshot.state == pbcapturenormalize::CaptureState::Running)
        {
            inPlaceRecreateCompleted = true;
        }
        if (!restarted && inPlaceRecreateCompleted &&
            fileReceiver.GetUniqueVisualFrames() >= externalRestartThreshold)
        {
            Require(evidenceCount < evidenceStorage.size(), "capture restart evidence capacity exhausted");
            evidenceStorage[evidenceCount++] = StopAndCollect(active);
            const std::uint64_t previousEpoch = evidenceStorage[evidenceCount - 1U].capture.captureEpoch;
            Require(previousEpoch < std::numeric_limits<std::uint64_t>::max(),
                "CaptureEpoch overflow during Gate restart");
            const std::uint64_t nextEpoch = previousEpoch + 1U;
            active = CreateActiveCapture(options, profile, region, nextEpoch);
            restarted = true;
            continue;
        }
        if (fileReceiver.IsFilePublished() && restarted && inPlaceRecreateCompleted && receiverEpochResets == 2 &&
            phase1gate::HasMinimumUniqueVisualCadence(fileReceiver.GetUniqueVisualCadenceIntervals()))
        {
            const double publishedSeconds = ElapsedSeconds(fileReceiver.GetFilePublishedQpc(), QpcNow(), qpcFrequency);
            if (publishedSeconds >= requiredPostPublishSeconds)
            {
                postPublishObservationSeconds = publishedSeconds;
                break;
            }
        }
        if (now >= deadline)
        {
            throw GateFailure("Phase 1 physical file receiver exceeded its bounded deadline");
        }
        if (drained == 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        else
        {
            std::this_thread::yield();
        }
    }
    Require(restarted && inPlaceRecreateCompleted && receiverEpochResets == 2,
        "required requested recreate, external capture restart, or CaptureEpoch resets did not occur");
    Require(evidenceCount < evidenceStorage.size(), "final capture evidence capacity exhausted");
    evidenceStorage[evidenceCount++] = StopAndCollect(active);
    const auto receiverTelemetry = receiver.GetTelemetry();
    Require(receiverTelemetry.activeOuterFecDecoderCount == 0 &&
        receiverTelemetry.reservedOuterFecDecoderBytes == 0,
        "Receiver retained an Outer-FEC decoder after stored commit");
    const auto finished = std::chrono::steady_clock::now();
    const std::uint64_t elapsedMilliseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(finished - started).count());
    const std::span<const CaptureSessionEvidence> evidence(evidenceStorage.data(), evidenceCount);
    const std::string final = fileReceiver.FinalJson(profile, options.backend, evidence,
        receiverEpochResets, elapsedMilliseconds, options.soakSeconds, requiredPostPublishSeconds,
        postPublishObservationSeconds);
    telemetry.Write(final);
    std::cout << final << '\n';
    return 0;
}

void Usage()
{
    std::cout << "Usage:\n"
              << "  PBPhase1FileGate --sender --profile desktop-levels-2x2|shape-chroma --source-new FILE --telemetry-new FILE [--maximum-sender-seconds N]\n"
              << "  PBPhase1FileGate --receiver --profile desktop-levels-2x2|shape-chroma --backend wgc|dxgi --roi L T R B --output-new FILE --telemetry-new FILE\n"
              << "                   [--timeout-seconds N] [--restart-after-unique N] [--soak-seconds N]\n";
}

} // namespace

int wmain(const int argumentCount, wchar_t* arguments[])
{
    phase1gate::Arguments options;
    if (!phase1gate::ParseArguments(argumentCount, arguments, options))
    {
        Usage();
        return 2;
    }
    if (options.showHelp)
    {
        Usage();
        return 0;
    }
    try
    {
        return options.mode == phase1gate::ProcessMode::Sender ? RunSender(options) : RunReceiver(options);
    }
    catch (const std::exception& exception)
    {
        std::cerr << "PBPhase1FileGate: " << exception.what() << '\n';
        return 1;
    }
}
