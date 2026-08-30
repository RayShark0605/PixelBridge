#include "local_desktop_runtime.h"

#include "pbinnerfec/qc_ldpc_codec.h"
#include "pbmodulation/desktop_levels.h"
#include "pbmodulation/local_desktop_bootstrap.h"
#include "pbmodulation/reference_raster.h"
#include "pbmodulation/shape_chroma.h"
#include "pbouterfec/direct_repeat.h"
#include "pbouterfec/wirehair_v2.h"
#include "pbreceiver/receiver_ingress.h"
#include "pbscreencapturedxgi/dxgi_capture.h"
#include "pbscreencapturewgc/wgc_capture.h"
#include "pbdemodd3d11/capture_demodulator.h"
#include "pbstorage/output_file.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/checked_integer.h"
#include "pbprotocol/descriptor_codec.h"
#include "pbprotocol/session_random.h"
#include "pbprotocol/transport_block_codec.h"
#include "pbtelemetry/telemetry.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace pbapp
{
namespace
{

inline constexpr std::uint32_t outerBlockBytes = 1314;
inline constexpr std::size_t informationBytes = 1350;
inline constexpr std::size_t codewordBytes = 2025;
inline constexpr std::uint32_t controlRepetitions = 4;
inline constexpr std::uint32_t controlFramesPerCycle = 3 * controlRepetitions;
inline constexpr std::uint32_t captureQueuedFrameLimit = 4;
inline constexpr std::uint32_t captureDemodulatorSlotCount = 4;
inline constexpr std::uint32_t captureResultQueueCapacity = 128;
inline constexpr std::uint64_t maximumDemodulatorResidentBytes = 128ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t mebibyte = 1024ULL * 1024ULL;
inline constexpr std::uint64_t maximumCaptureResidentBytes = 512ULL * mebibyte;
inline constexpr std::uint64_t maximumRoiResidentBytes = 128ULL * mebibyte;
static_assert(pbdesktoplevels::kPayloadBytes == outerBlockBytes);
static_assert(pbdesktoplevels::kInfoBytes == informationBytes);
static_assert(pbdesktoplevels::kCodewordBytes == codewordBytes);

class RuntimeFailure final : public std::runtime_error
{
public:
    explicit RuntimeFailure(const std::string& message) : std::runtime_error(message)
    {
    }
};

class WorkerRunningGuard
{
public:
    explicit WorkerRunningGuard(std::atomic<bool>& running) noexcept : running_(running)
    {
    }

    ~WorkerRunningGuard()
    {
        running_ = false;
    }

    WorkerRunningGuard(const WorkerRunningGuard&) = delete;
    WorkerRunningGuard& operator=(const WorkerRunningGuard&) = delete;

private:
    std::atomic<bool>& running_;
};

void Require(const bool condition, const std::string& message)
{
    if (!condition)
    {
        throw RuntimeFailure(message);
    }
}

template <typename ResultType>
void RequireResult(const ResultType& result, const std::string& message)
{
    Require(static_cast<bool>(result), message);
}

[[nodiscard]] std::string Utf8FromWide(const std::wstring& value)
{
    if (value.empty())
    {
        return {};
    }
    Require(value.size() <= static_cast<std::size_t>((std::numeric_limits<int>::max)()),
        "UTF-16 path exceeds the Win32 conversion limit");
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    Require(count > 0, "UTF-16 path cannot be represented as UTF-8");
    std::string output(static_cast<std::size_t>(count), '\0');
    Require(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), output.data(), count, nullptr, nullptr) == count,
        "UTF-16 path conversion changed between passes");
    return output;
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

[[nodiscard]] std::string SessionIdHex(const pbprotocol::SessionId& sessionId)
{
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (const std::byte value : sessionId.bytes)
    {
        stream << std::setw(2) << static_cast<unsigned int>(std::to_integer<std::uint8_t>(value));
    }
    return stream.str();
}

[[nodiscard]] std::string GenerateRunId()
{
    const auto sessionId = pbprotocol::GenerateRandomSessionId();
    RequireResult(sessionId, "OS CSPRNG RunId generation failed");
    return SessionIdHex(sessionId.Value());
}

[[nodiscard]] std::uint64_t ElapsedMilliseconds(const std::chrono::steady_clock::time_point started) noexcept
{
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    return elapsed < 0 ? 0 : static_cast<std::uint64_t>(elapsed);
}

[[nodiscard]] std::uint64_t GetUnixTimeMilliseconds() noexcept
{
    FILETIME fileTime{};
    GetSystemTimeAsFileTime(&fileTime);
    ULARGE_INTEGER ticks{};
    ticks.LowPart = fileTime.dwLowDateTime;
    ticks.HighPart = fileTime.dwHighDateTime;
    constexpr std::uint64_t windowsToUnixEpoch100ns = 116444736000000000ULL;
    return ticks.QuadPart < windowsToUnixEpoch100ns ? 0 :
        (ticks.QuadPart - windowsToUnixEpoch100ns) / 10000ULL;
}

[[nodiscard]] std::string DescribePresentationStatus(const pbrenderd3d::PresentationStatus& status)
{
    std::ostringstream stream;
    stream << pbrenderd3d::GetPresentationErrorName(status.code) << " stage="
           << pbrenderd3d::GetPresentationStageName(status.stage) << " native=" << status.nativeError;
    return stream.str();
}

[[nodiscard]] std::string DescribeCaptureStatus(const pbcapturenormalize::CaptureStatus& status)
{
    std::ostringstream stream;
    stream << pbcapturenormalize::GetCaptureErrorName(status.code) << " stage="
           << static_cast<unsigned int>(status.stage) << " native=" << status.nativeError;
    return stream.str();
}

[[nodiscard]] std::string DescribeStorageStatus(const pbstorage::StorageStatus& status)
{
    std::ostringstream stream;
    stream << pbstorage::GetStorageErrorName(status.code) << " stage="
           << pbstorage::GetStorageStageName(status.stage) << " native=" << status.nativeError;
    return stream.str();
}

[[nodiscard]] std::string DescribeReceiverError(const pbreceiver::ReceiverError& error)
{
    std::ostringstream stream;
    if (const auto* protocol = std::get_if<pbprotocol::ProtocolError>(&error))
    {
        stream << "protocol code=" << static_cast<unsigned int>(protocol->code)
               << " offset=" << protocol->offset;
    }
    else if (const auto* outerFec = std::get_if<pbouterfec::OuterFecError>(&error))
    {
        stream << "outer-fec code=" << static_cast<unsigned int>(outerFec->code)
               << " detail=" << outerFec->detail;
    }
    else
    {
        const auto& compression = std::get<pbcompression::CompressionError>(error);
        stream << "compression code=" << static_cast<unsigned int>(compression.code)
               << " detail=" << compression.detail;
    }
    return stream.str();
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

struct SourceFile
{
    UniqueHandle handle;
    BY_HANDLE_FILE_INFORMATION identity{};
    std::vector<std::byte> bytes;
};

[[nodiscard]] SourceFile ReadSourceFile(const std::wstring& path)
{
    SourceFile source;
    source.handle.Reset(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    Require(static_cast<bool>(source.handle), "无法以只读方式打开源文件，Win32=" + std::to_string(GetLastError()));
    Require(GetFileInformationByHandle(source.handle.Get(), &source.identity) != FALSE,
        "无法读取源文件身份，Win32=" + std::to_string(GetLastError()));
    const std::uint64_t fileBytes = (static_cast<std::uint64_t>(source.identity.nFileSizeHigh) << 32U) |
        source.identity.nFileSizeLow;
    Require(fileBytes >= minimumInstantFileBytes && fileBytes <= maximumInstantFileBytes,
        "当前单 Segment production path 仅支持 1 byte 到 8 MiB，源文件不会被截断");
    source.bytes.resize(static_cast<std::size_t>(fileBytes));
    std::size_t offset = 0;
    while (offset < source.bytes.size())
    {
        const DWORD chunk = static_cast<DWORD>((std::min)(source.bytes.size() - offset,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD readBytes = 0;
        Require(ReadFile(source.handle.Get(), source.bytes.data() + offset, chunk, &readBytes, nullptr) != FALSE,
            "读取源文件失败，Win32=" + std::to_string(GetLastError()));
        Require(readBytes != 0 && readBytes <= chunk, "源文件在 Session 准备期间提前结束");
        offset += readBytes;
    }
    std::byte extra{};
    DWORD extraBytes = 0;
    Require(ReadFile(source.handle.Get(), &extra, 1, &extraBytes, nullptr) != FALSE && extraBytes == 0,
        "源文件大小在 Session 准备期间发生变化");
    return source;
}

[[nodiscard]] bool IsSourceStable(const SourceFile& source) noexcept
{
    BY_HANDLE_FILE_INFORMATION current{};
    if (GetFileInformationByHandle(source.handle.Get(), &current) == FALSE)
    {
        return false;
    }
    return current.dwVolumeSerialNumber == source.identity.dwVolumeSerialNumber &&
        current.nFileIndexHigh == source.identity.nFileIndexHigh &&
        current.nFileIndexLow == source.identity.nFileIndexLow &&
        current.nFileSizeHigh == source.identity.nFileSizeHigh &&
        current.nFileSizeLow == source.identity.nFileSizeLow &&
        CompareFileTime(&current.ftLastWriteTime, &source.identity.ftLastWriteTime) == 0;
}

struct ProfileBinding
{
    VisualProfile profile = VisualProfile::DirectLevels2x2;
    std::uint64_t visualProfileId = 0;
    std::uint8_t layoutVersion = 0;
    std::uint32_t dataBytes = 0;
    std::uint32_t codewords = 0;
};

[[nodiscard]] ProfileBinding GetProfileBinding(const VisualProfile profile)
{
    if (profile == VisualProfile::ShapeChroma)
    {
        return {profile, pbmodulation::kShapeChromaProfileId, pbmodulation::kShapeChromaLayoutVersion,
            pbmodulation::kShapeChromaDataBytes, pbmodulation::kShapeChromaCodewords};
    }
    const auto* const direct = pbmodulation::GetDesktopLevelsProfile(pbmodulation::kDesktopLevels2ProfileId);
    Require(direct != nullptr, "Direct-Level 2x2 profile is unavailable");
    return {profile, direct->visualProfileId, pbmodulation::kDesktopLevelsLayoutVersion,
        direct->dataBytes, direct->codewords};
}

struct TransferDescription
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
    const std::uint64_t sequence, const pbprotocol::SessionTag sessionTag,
    const std::span<const std::byte> payload)
{
    const pbprotocol::ControlRecordView record{pbprotocol::kControlVersion, recordType, sequence,
        sessionTag, payload};
    const auto size = pbprotocol::GetSerializedSize(record);
    RequireResult(size, "ControlRecord size calculation failed");
    Require(size.Value() <= pbmodulation::kReferenceControlWindowBytes,
        "ControlRecord does not fit the fixed Control window");
    std::vector<std::byte> bytes(size.Value());
    RequireResult(pbprotocol::SerializeControlRecord(record, bytes), "ControlRecord serialization failed");
    return bytes;
}

[[nodiscard]] TransferDescription DescribeSource(const std::span<const std::byte> rawBytes,
    const bool compressionEnabled, const int compressionLevel)
{
    const auto sessionId = pbprotocol::GenerateRandomSessionId();
    RequireResult(sessionId, "OS CSPRNG SessionId generation failed");
    const pbprotocol::SessionDescriptor session{pbprotocol::GetProtocolVersion(), sessionId.Value(),
        rawBytes.size(), 1, pbprotocol::DigestAlgorithm::Blake3_256};
    const pbprotocol::SessionTag sessionTag = pbprotocol::DeriveSessionTag(session.sessionId);
    auto prepared = PrepareEncodedSegment(rawBytes, compressionEnabled, compressionLevel);
    RequireResult(prepared, "Segment compression/RAW preparation failed");
    pbcompression::EncodedSegment encoded = std::move(prepared).Value();
    const auto selectedMode = pbouterfec::ChooseOuterFecMode(encoded.bytes.size(), outerBlockBytes);
    RequireResult(selectedMode, "Outer FEC mode selection failed");
    pbprotocol::SegmentDescriptor segment;
    segment.sessionTag = sessionTag;
    segment.segmentOrdinal = 0;
    segment.rawOffset = 0;
    segment.rawSize = rawBytes.size();
    segment.encodedSize = encoded.bytes.size();
    segment.compressionCodec = encoded.codec;
    segment.outerFecMode = selectedMode.Value();
    segment.outerBlockBytes = outerBlockBytes;
    segment.rawDigest = pbprotocol::RawDigest{pbprotocol::ComputeBlake3Digest(rawBytes)};
    segment.encodedDigest = pbprotocol::EncodedDigest{pbprotocol::ComputeBlake3Digest(encoded.bytes)};
    if (segment.outerFecMode == pbprotocol::OuterFecMode::WirehairV2)
    {
        const auto encoder = pbouterfec::WirehairV2Encoder::Create(encoded.bytes, outerBlockBytes);
        RequireResult(encoder, "Wirehair V2 descriptor creation failed");
        segment.wirehairV2SerializedProfile = encoder.Value().GetSerializedProfile();
    }
    else
    {
        const auto encoder = pbouterfec::DirectRepeatEncoder::Create(encoded.bytes, outerBlockBytes);
        RequireResult(encoder, "DirectRepeat descriptor validation failed");
    }
    const pbprotocol::FinalManifest manifest{session.sessionId, rawBytes.size(), 1,
        pbprotocol::WholeFileDigest{pbprotocol::ComputeBlake3Digest(rawBytes)},
        pbprotocol::DigestAlgorithm::Blake3_256};
    const pbprotocol::ReceiverResourcePolicy policy = pbprotocol::GetDefaultReceiverResourcePolicy();
    std::array<std::byte, pbprotocol::kSessionDescriptorPayloadBytes> sessionPayload{};
    RequireResult(pbprotocol::SerializeSessionDescriptor(session, policy, sessionPayload),
        "SessionDescriptor serialization failed");
    std::array<std::byte, pbprotocol::kFinalManifestPayloadBytes> manifestPayload{};
    RequireResult(pbprotocol::SerializeFinalManifest(manifest, session, policy, manifestPayload),
        "FinalManifest serialization failed");
    const auto segmentSize = pbprotocol::GetSerializedSize(segment);
    RequireResult(segmentSize, "SegmentDescriptor size calculation failed");
    std::vector<std::byte> segmentPayload(segmentSize.Value());
    RequireResult(pbprotocol::SerializeSegmentDescriptor(segment, session, policy, segmentPayload),
        "SegmentDescriptor serialization failed");
    TransferDescription description;
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

enum class FrameKind : std::uint8_t
{
    SessionControl,
    ManifestControl,
    SegmentControl,
    Data
};

class SenderFrameBuilder
{
public:
    SenderFrameBuilder(ProfileBinding profile, const TransferDescription& description)
        : profile_(profile), description_(description), data_(profile.dataBytes),
          pixels_(pbmodulation::kLocalDesktopFrameBgraBytes)
    {
        if (description_.segment.outerFecMode == pbprotocol::OuterFecMode::WirehairV2)
        {
            auto encoder = pbouterfec::WirehairV2Encoder::Recreate(description_.encoded.bytes,
                description_.segment);
            RequireResult(encoder, "Wirehair V2 sender recreation failed");
            wirehair_ = std::make_unique<pbouterfec::WirehairV2Encoder>(std::move(encoder).Value());
            blockCount_ = wirehair_->GetBlockCount();
            const std::uint64_t repairAllowance = (std::max)(4096ULL,
                static_cast<std::uint64_t>(blockCount_) / 2ULL);
            const auto blocksPerCycle = pbprotocol::CheckedAddUint64(blockCount_, repairAllowance);
            RequireResult(blocksPerCycle, "Wirehair carousel block count overflow");
            targetBlocksPerCycle_ = blocksPerCycle.Value();
        }
        else
        {
            auto encoder = pbouterfec::DirectRepeatEncoder::Recreate(description_.encoded.bytes,
                description_.segment);
            RequireResult(encoder, "DirectRepeat sender recreation failed");
            directRepeat_ = std::make_unique<pbouterfec::DirectRepeatEncoder>(std::move(encoder).Value());
            Require(directRepeat_->GetBlockCount() > 0 &&
                directRepeat_->GetBlockCount() <= (std::numeric_limits<std::uint32_t>::max)(),
                "DirectRepeat block count is invalid");
            blockCount_ = static_cast<std::uint32_t>(directRepeat_->GetBlockCount());
            targetBlocksPerCycle_ = blockCount_;
        }
        Require(profile_.codewords != 0 && targetBlocksPerCycle_ != 0,
            "Profile or outer FEC produced an empty carousel");
        const auto rounded = pbprotocol::CheckedAddUint64(targetBlocksPerCycle_, profile_.codewords - 1U);
        RequireResult(rounded, "Carousel frame count overflow");
        const std::uint64_t dataFrames = rounded.Value() / profile_.codewords;
        Require(dataFrames <= (std::numeric_limits<std::uint32_t>::max)() - controlFramesPerCycle,
            "Carousel frame count exceeds the bounded UI model");
        cycleFrameCount_ = controlFramesPerCycle + static_cast<std::uint32_t>(dataFrames);
        Require(carousel_.Reset(cycleFrameCount_), "Carousel counter initialization failed");
    }

    [[nodiscard]] FrameKind GetCurrentKind() const noexcept
    {
        const std::uint32_t position = carousel_.GetSnapshot().cyclePosition;
        if (position < controlRepetitions)
        {
            return FrameKind::SessionControl;
        }
        if (position < 2U * controlRepetitions)
        {
            return FrameKind::ManifestControl;
        }
        if (position < 3U * controlRepetitions)
        {
            return FrameKind::SegmentControl;
        }
        return FrameKind::Data;
    }

    [[nodiscard]] const std::vector<std::byte>& Build(const std::uint64_t frameSequence)
    {
        const FrameKind kind = GetCurrentKind();
        const auto bootstrap = MakeBootstrap(frameSequence);
        if (kind != FrameKind::Data)
        {
            const std::vector<std::byte>& control = kind == FrameKind::SessionControl ?
                description_.sessionControl : kind == FrameKind::ManifestControl ?
                description_.manifestControl : description_.segmentControl;
            std::fill(controlWindow_.begin(), controlWindow_.end(), std::byte{0});
            std::copy(control.begin(), control.end(), controlWindow_.begin());
            std::fill(referenceData_.begin(), referenceData_.end(), std::byte{0});
            RequireResult(pbmodulation::EncodeReferenceFrame({bootstrap, controlWindow_, referenceData_}, pixels_),
                "fixed Control raster generation failed");
            generatedPayloadBytesInFrame_ = 0;
            return pixels_;
        }
        BuildTransportData();
        const auto status = profile_.profile == VisualProfile::ShapeChroma ?
            pbmodulation::EncodeShapeChromaFrame(bootstrap, data_, pixels_) :
            pbmodulation::EncodeDesktopLevelsFrame(bootstrap, data_, pixels_);
        RequireResult(status, "physical data raster generation failed");
        return pixels_;
    }

    void Advance()
    {
        if (GetCurrentKind() == FrameKind::Data)
        {
            if (wirehair_)
            {
                const std::uint64_t next = static_cast<std::uint64_t>(nextOuterBlockId_) + profile_.codewords;
                Require(next <= (std::numeric_limits<std::uint32_t>::max)(),
                    "Wirehair repair ID space exhausted; start a new Session");
                nextOuterBlockId_ = static_cast<std::uint32_t>(next);
            }
            else
            {
                nextOuterBlockId_ = static_cast<std::uint32_t>(
                    (static_cast<std::uint64_t>(nextOuterBlockId_) + profile_.codewords) % blockCount_);
            }
        }
        Require(carousel_.Advance(), "Carousel counter overflow");
    }

    [[nodiscard]] CarouselSnapshot GetCarouselSnapshot() const noexcept
    {
        return carousel_.GetSnapshot();
    }
    [[nodiscard]] const std::vector<std::byte>& GetBuiltPixels() const noexcept
    {
        return pixels_;
    }
    [[nodiscard]] std::uint32_t GetCurrentOuterBlockId() const noexcept
    {
        return nextOuterBlockId_;
    }
    [[nodiscard]] std::uint32_t GetBlockCount() const noexcept
    {
        return blockCount_;
    }
    [[nodiscard]] std::uint64_t GetGeneratedPayloadBytesInFrame() const noexcept
    {
        return generatedPayloadBytesInFrame_;
    }

private:
    [[nodiscard]] std::array<std::byte, pbprotocol::kBootstrapRecordBytes> MakeBootstrap(
        const std::uint64_t frameSequence) const
    {
        const pbprotocol::BootstrapRecord record{pbprotocol::kBootstrapVersion,
            pbprotocol::GetProtocolVersion(), profile_.layoutVersion, profile_.visualProfileId,
            description_.segment.sessionTag, frameSequence, 0, 0};
        std::array<std::byte, pbprotocol::kBootstrapRecordBytes> bytes{};
        RequireResult(pbprotocol::SerializeBootstrapRecord(record, bytes), "Bootstrap serialization failed");
        return bytes;
    }

    void BuildTransportData()
    {
        std::fill(data_.begin(), data_.end(), std::byte{0});
        generatedPayloadBytesInFrame_ = 0;
        for (std::uint32_t slot = 0; slot < profile_.codewords; slot++)
        {
            const std::uint64_t candidate = static_cast<std::uint64_t>(nextOuterBlockId_) + slot;
            Require(!wirehair_ || candidate <= (std::numeric_limits<std::uint32_t>::max)(),
                "Wirehair repair ID space exhausted inside a visual frame");
            const std::uint32_t blockId = wirehair_ ? static_cast<std::uint32_t>(candidate) :
                static_cast<std::uint32_t>(candidate % blockCount_);
            std::fill(outerPayload_.begin(), outerPayload_.end(), std::byte{0});
            const auto encoded = wirehair_ ? wirehair_->EncodeBlock(blockId, outerPayload_) :
                directRepeat_->EncodeBlock(blockId, outerPayload_);
            RequireResult(encoded, "Outer FEC block encoding failed");
            Require(encoded.Value() > 0 && encoded.Value() <= outerPayload_.size() &&
                encoded.Value() <= (std::numeric_limits<std::uint16_t>::max)(),
                "Outer FEC produced an invalid payload length");
            const pbprotocol::TransportBlockHeader header{pbprotocol::kTransportBlockTypeData,
                pbprotocol::kTransportProtocolMinor, 0, description_.segment.sessionTag, 0, blockId,
                static_cast<std::uint16_t>(encoded.Value())};
            const std::size_t serializedBytes = pbprotocol::GetTransportSerializedSize(header);
            Require(serializedBytes <= transport_.size(), "Transport block exceeds the robust information block");
            RequireResult(pbprotocol::SerializeTransportBlock(header,
                std::span(outerPayload_).first(encoded.Value()), std::span(transport_).first(serializedBytes)),
                "Transport serialization failed");
            RequireResult(pbprotocol::FrameTransportBlockIntoInfoBlock(std::span(transport_).first(serializedBytes),
                information_.size(), information_), "Transport information framing failed");
            RequireResult(pbinnerfec::EncodeQcLdpcCodeword(pbinnerfec::kInnerFecProfileIdRobust, information_,
                std::span(data_).subspan(static_cast<std::size_t>(slot) * codewordBytes, codewordBytes)),
                "Robust QC-LDPC encoding failed");
            generatedPayloadBytesInFrame_ += encoded.Value();
        }
    }

    ProfileBinding profile_;
    const TransferDescription& description_;
    std::unique_ptr<pbouterfec::WirehairV2Encoder> wirehair_;
    std::unique_ptr<pbouterfec::DirectRepeatEncoder> directRepeat_;
    std::vector<std::byte> data_;
    std::vector<std::byte> pixels_;
    std::array<std::byte, pbmodulation::kReferenceControlWindowBytes> controlWindow_{};
    std::array<std::byte, pbmodulation::kReferenceDataRegionBytes> referenceData_{};
    std::array<std::byte, outerBlockBytes> outerPayload_{};
    std::array<std::byte, informationBytes> transport_{};
    std::array<std::byte, informationBytes> information_{};
    CarouselCounter carousel_;
    std::uint32_t blockCount_ = 0;
    std::uint64_t targetBlocksPerCycle_ = 0;
    std::uint32_t cycleFrameCount_ = 0;
    std::uint32_t nextOuterBlockId_ = 0;
    std::uint64_t generatedPayloadBytesInFrame_ = 0;
};

[[nodiscard]] bool HasStablePresentationContract(const pbrenderd3d::DataWindowSnapshot& snapshot) noexcept
{
    return snapshot.state == pbrenderd3d::WindowState::Running && snapshot.candidateContractSatisfied &&
        snapshot.contract.bufferWidth == phase1CanvasWidth && snapshot.contract.bufferHeight == phase1CanvasHeight &&
        snapshot.contract.bufferCount == 2 && snapshot.contract.maximumFrameLatency == 1 &&
        snapshot.contract.flipEffect == pbrenderd3d::FlipEffect::Discard && snapshot.contract.bgraUnorm &&
        snapshot.contract.noMsaa && snapshot.contract.alphaIgnored && snapshot.contract.scalingNone &&
        snapshot.contract.tearingDisabled && snapshot.contract.latencyWaitable && snapshot.contract.perMonitorV2 &&
        !snapshot.softwareRasterizer;
}

class NativeCaptureSession
{
public:
    NativeCaptureSession(const CaptureBackend backend,
        const pbcapturenormalize::CaptureNormalizeConfig& config,
        const std::shared_ptr<pbcapturenormalize::ScreenCaptureConsumer>& consumer)
        : backend_(backend)
    {
        const pbcapturenormalize::CaptureStatus status = backend == CaptureBackend::Wgc ?
            pbscreencapturewgc::WgcCapture::CreateNormalized(config, consumer, wgc_) :
            pbscreencapturedxgi::DxgiCapture::Create(config, consumer, dxgi_);
        Require(static_cast<bool>(status), std::string(GetCaptureBackendName(backend)) +
            " capture creation failed: " + DescribeCaptureStatus(status));
    }

    ~NativeCaptureSession()
    {
        static_cast<void>(Stop());
    }
    NativeCaptureSession(const NativeCaptureSession&) = delete;
    NativeCaptureSession& operator=(const NativeCaptureSession&) = delete;

    [[nodiscard]] pbcapturenormalize::CaptureSnapshot GetSnapshot() const noexcept
    {
        return wgc_ ? wgc_->GetSnapshot() : dxgi_->GetSnapshot();
    }
    [[nodiscard]] pbcapturenormalize::CaptureNormalizeSnapshot GetNormalizationSnapshot() const noexcept
    {
        return wgc_ ? wgc_->GetNormalizationSnapshot() : dxgi_->GetNormalizationSnapshot();
    }
    void RequestStop() noexcept
    {
        if (wgc_)
        {
            wgc_->RequestStop();
        }
        else if (dxgi_)
        {
            dxgi_->RequestStop();
        }
    }
    [[nodiscard]] pbcapturenormalize::CaptureStatus Stop() noexcept
    {
        if (stopped_)
        {
            return stopStatus_;
        }
        stopStatus_ = wgc_ ? wgc_->Stop() : dxgi_->Stop();
        stopped_ = true;
        return stopStatus_;
    }

private:
    CaptureBackend backend_ = CaptureBackend::Wgc;
    std::unique_ptr<pbscreencapturewgc::WgcCapture> wgc_;
    std::unique_ptr<pbscreencapturedxgi::DxgiCapture> dxgi_;
    pbcapturenormalize::CaptureStatus stopStatus_;
    bool stopped_ = false;
};

[[nodiscard]] pbcapturenormalize::CaptureNormalizeConfig MakeCaptureConfig(const DecoderConfig& config)
{
    pbcapturenormalize::CaptureNormalizeConfig captureConfig;
    captureConfig.capture.region = config.region;
    captureConfig.capture.initialCaptureEpoch = 1;
    captureConfig.capture.queuedFrameLimit = captureQueuedFrameLimit;
    captureConfig.capture.roiTextureCount = captureDemodulatorSlotCount;
    captureConfig.capture.maximumCaptureBytes = maximumCaptureResidentBytes;
    captureConfig.capture.maximumRoiBytes = maximumRoiResidentBytes;
    captureConfig.capture.gpuTimeoutMilliseconds = 3000;
    captureConfig.capture.maximumDeviceRecoveries = 1;
    captureConfig.capture.pixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    captureConfig.capture.maximumFrameAgeMilliseconds = 250;
    captureConfig.capture.requestBorderless = true;
    return captureConfig;
}

struct AuthoritativeCompletion
{
    bool published = false;
    std::string outputPath;
    std::string wholeFileDigestHex;
    std::uint64_t recoveryRuntimeMilliseconds = 0;
};

class ReceiverPipeline
{
public:
    ReceiverPipeline(pbreceiver::ReceiverIngress& receiver, std::wstring outputDirectory,
        const pbprotocol::ReceiverResourcePolicy& policy, SnapshotStore<DecoderSnapshot>& snapshot,
        AuthoritativeCompletion& completion, const std::uint64_t runGeneration,
        const std::chrono::steady_clock::time_point started)
        : receiver_(receiver), outputDirectory_(std::move(outputDirectory)), policy_(policy), snapshot_(snapshot),
          completion_(completion), runGeneration_(runGeneration), started_(started)
    {
    }

    void Process(const pbdemodd3d11::CaptureDemodulatorResult& result)
    {
        RecordCaptureTelemetry(result);
        if (!result.bootstrap.IsAccepted())
        {
            UpdateTelemetrySnapshot();
            return;
        }
        const auto parsedBootstrap = pbprotocol::ParseBootstrapRecord(result.bootstrapRecord);
        RequireResult(parsedBootstrap, "CaptureDemodulator published an invalid Bootstrap");
        const pbprotocol::BootstrapRecord& bootstrap = parsedBootstrap.Value();
        if (session_ && bootstrap.sessionTag != pbprotocol::DeriveSessionTag(session_->sessionId))
        {
            UpdateTelemetrySnapshot();
            return;
        }

        bool identityObserved = false;
        VisualIdentityDisposition identityDisposition = VisualIdentityDisposition::Invalid;
        if (session_)
        {
            identityDisposition = visualRate_.Observe(bootstrap.frameSequence,
                result.metadata.domain.captureEpoch, result.metadata.timestamp.monotonic100ns);
            identityObserved = true;
        }
        if (result.kind == pbdemodd3d11::CaptureDemodulatorResultKind::Transport &&
            identityDisposition == VisualIdentityDisposition::Unique)
        {
            RecordFecTelemetry(result);
        }

        if (result.kind == pbdemodd3d11::CaptureDemodulatorResultKind::TelemetryOnly)
        {
            UpdateVisualSnapshot();
            UpdateTelemetrySnapshot();
            return;
        }
        if (result.kind == pbdemodd3d11::CaptureDemodulatorResultKind::ControlRecord)
        {
            Require(result.controlByteCount <= result.controlBytes.size(),
                "CaptureDemodulator ControlRecord byte count is out of bounds");
            ProcessControlRecord(std::span(result.controlBytes).first(result.controlByteCount),
                bootstrap.sessionTag, result.metadata.timestamp.monotonic100ns);
        }
        else if (result.kind == pbdemodd3d11::CaptureDemodulatorResultKind::ControlFragment)
        {
            Require(result.controlByteCount <= result.controlBytes.size(),
                "CaptureDemodulator ControlFragment byte count is out of bounds");
            ProcessControlFragment(std::span(result.controlBytes).first(result.controlByteCount),
                result.metadata.captureObservation);
        }
        else
        {
            ProcessTransport(result);
        }
        if (!identityObserved && session_ && bootstrap.sessionTag == pbprotocol::DeriveSessionTag(session_->sessionId))
        {
            static_cast<void>(visualRate_.Observe(bootstrap.frameSequence, result.metadata.domain.captureEpoch,
                result.metadata.timestamp.monotonic100ns));
        }
        UpdateVisualSnapshot();
        UpdateTelemetrySnapshot();
    }

    void CaptureEpochReset(const std::uint64_t monotonicMilliseconds)
    {
        telemetry_.EndCaptureEpoch();
        receiverSessionBound_ = false;
        storage_.reset();
        session_.reset();
        segment_.reset();
        manifest_.reset();
        stored_ = false;
        storedEncodedBytes_ = 0;
        acceptedTransportBlocks_ = 0;
        identityFailures_ = 0;
        falseAcceptedCodewords_ = 0;
        progress_.ResetForCaptureEpoch(monotonicMilliseconds);
        snapshot_.Update([this](DecoderSnapshot& value)
        {
            if (value.runGeneration != runGeneration_)
            {
                return;
            }
            pbprotocol::SaturatingIncrementUnsigned(value.captureEpochResets);
            if (value.state != DecoderState::Stopping)
            {
                value.state = DecoderState::WaitingForBootstrap;
            }
            value.descriptorKnown = false;
            value.originalFileBytes = 0;
            value.verifiedRawBytes = 0;
            value.remainingRawBytes = 0;
            value.recoveryProgress.reset();
            value.instantVerifiedRawGoodputBytesPerSecond = 0;
            value.smoothedVerifiedRawGoodputBytesPerSecond = 0;
            value.averageVerifiedRawGoodputBytesPerSecond = 0;
            value.verifiedEncodedBytes = 0;
            value.verifiedEncodedGoodputBitsPerSecond.reset();
            value.etaMilliseconds.reset();
            value.sessionIdHex.clear();
            value.sessionTag = 0;
            value.segmentCount = 0;
            value.currentSegmentOrdinal = 0;
            value.outputPath.clear();
            value.wholeFileDigestHex.clear();
            value.wholeFileDigestVerified = false;
            value.finalPublishSucceeded = false;
            value.telemetryCapturedFrames = 0;
            value.telemetryDroppedFrames = 0;
            value.fingerprintedFrames = 0;
            value.captureFps.reset();
            value.uniqueVisualFps.reset();
            value.telemetryBootstrapAttempts = 0;
            value.telemetryBootstrapSuccesses = 0;
            value.bootstrapSuccessRate.reset();
            value.evaluatedDataFrames = 0;
            value.postFecFailedFrames = 0;
            value.acceptedTransportBlocks = 0;
            value.comparedCodedBits = 0;
            value.erroneousCodedBits = 0;
            value.fecFailures = 0;
            value.crcFailures = 0;
            value.identityFailures = 0;
            value.falseAcceptedCodewords = 0;
            value.preFecBerEstimate.reset();
            value.fecFrameErrorRate.reset();
            value.statusMessage = "CaptureEpoch changed; discarded unpublished state and waiting for authoritative descriptor rebind";
        });
    }

    void ObserveDroppedFrames(const std::uint64_t captureDroppedFrames,
        const std::uint64_t resultQueueDrops, const std::uint64_t staleResultDrops)
    {
        const auto Delta = [](const std::uint64_t current, const std::uint64_t previous) noexcept
        {
            return current >= previous ? current - previous : current;
        };
        const std::uint64_t captureDelta = Delta(captureDroppedFrames, lastCaptureDroppedFrames_);
        const std::uint64_t queueDelta = Delta(resultQueueDrops, lastResultQueueDrops_);
        const std::uint64_t staleDelta = Delta(staleResultDrops, lastStaleResultDrops_);
        lastCaptureDroppedFrames_ = captureDroppedFrames;
        lastResultQueueDrops_ = resultQueueDrops;
        lastStaleResultDrops_ = staleResultDrops;
        const auto captureAndQueueDelta = pbprotocol::CheckedAddUint64(captureDelta, queueDelta);
        RequireResult(captureAndQueueDelta, "capture/result dropped-frame delta overflow");
        const auto totalDelta = pbprotocol::CheckedAddUint64(captureAndQueueDelta.Value(), staleDelta);
        RequireResult(totalDelta, "capture/result/stale dropped-frame delta overflow");
        const std::uint64_t delta = totalDelta.Value();
        if (delta == 0)
        {
            return;
        }
        if (telemetry_.GetSnapshot().active)
        {
            telemetry_.RecordDroppedFrames(delta);
        }
        else
        {
            const auto pending = pbprotocol::CheckedAddUint64(pendingDroppedFrames_, delta);
            RequireResult(pending, "pending PBTelemetry dropped-frame counter overflow");
            pendingDroppedFrames_ = pending.Value();
        }
        Require(!telemetry_.GetSnapshot().counterSaturated,
            "PBTelemetry dropped-frame counter overflow");
        UpdateTelemetrySnapshot();
    }

    void EndCaptureTelemetry()
    {
        telemetry_.EndCaptureEpoch();
        UpdateTelemetrySnapshot();
    }

    void ObserveStall(const std::uint64_t monotonicMilliseconds)
    {
        progress_.ObserveStall(monotonicMilliseconds);
        ApplyProgress();
    }

    [[nodiscard]] bool IsCompleted() const noexcept
    {
        return published_;
    }

private:
    static void RequireTelemetry(const pbtelemetry::TelemetryStatus status, const char* const operation)
    {
        if (!status)
        {
            throw RuntimeFailure(std::string(operation) + " failed: " +
                pbtelemetry::GetTelemetryErrorName(status.code));
        }
    }

    void RecordCaptureTelemetry(const pbdemodd3d11::CaptureDemodulatorResult& result)
    {
        pbtelemetry::TelemetrySnapshot telemetry = telemetry_.GetSnapshot();
        if (!telemetry.active)
        {
            RequireTelemetry(telemetry_.BeginCaptureEpoch(result.metadata.domain,
                result.metadata.timestamp.monotonic100ns), "PBTelemetry BeginCaptureEpoch");
            if (pendingDroppedFrames_ != 0)
            {
                telemetry_.RecordDroppedFrames(pendingDroppedFrames_);
                pendingDroppedFrames_ = 0;
            }
        }
        else
        {
            Require(telemetry.domain == result.metadata.domain,
                "PBTelemetry received a capture domain without an explicit CaptureEpoch reset");
        }

        const pbtelemetry::CaptureSample captureSample{result.metadata.domain,
            result.metadata.captureObservation, result.metadata.timestamp.monotonic100ns,
            result.metadata.roiCopyTime100ns, std::nullopt};
        RequireTelemetry(telemetry_.RecordCapture(captureSample), "PBTelemetry RecordCapture");

        pbtelemetry::BootstrapSample bootstrapSample;
        bootstrapSample.domain = result.metadata.domain;
        bootstrapSample.captureObservation = result.metadata.captureObservation;
        bootstrapSample.success = result.bootstrap.IsAccepted();
        if (bootstrapSample.success)
        {
            bootstrapSample.scaleX = result.bootstrap.geometry.scaleX;
            bootstrapSample.scaleY = result.bootstrap.geometry.scaleY;
            bootstrapSample.phaseX = result.bootstrap.geometry.originX - std::floor(result.bootstrap.geometry.originX);
            bootstrapSample.phaseY = result.bootstrap.geometry.originY - std::floor(result.bootstrap.geometry.originY);
        }
        RequireTelemetry(telemetry_.RecordBootstrap(bootstrapSample), "PBTelemetry RecordBootstrap");
    }

    void RecordFecTelemetry(const pbdemodd3d11::CaptureDemodulatorResult& result)
    {
        Require(result.demodulation.evaluation.evaluated,
            "CaptureDemodulator Transport result did not carry a FEC evaluation");
        const pbtelemetry::FecSample fecSample{result.metadata.domain,
            result.metadata.captureObservation, result.demodulation.evaluation};
        RequireTelemetry(telemetry_.RecordFec(fecSample), "PBTelemetry RecordFec");
    }

    [[nodiscard]] bool IsRetryableUnknownSession(const pbreceiver::ReceiverError& error) const noexcept
    {
        const auto* protocol = std::get_if<pbprotocol::ProtocolError>(&error);
        return !receiverSessionBound_ && protocol != nullptr &&
            protocol->code == pbprotocol::ProtocolErrorCode::UnknownSession;
    }

    void ProcessControlRecord(const std::span<const std::byte> bytes,
        const pbprotocol::SessionTag bootstrapSessionTag, const std::int64_t timestamp100ns)
    {
        const auto parsedRecord = pbprotocol::ParseControlRecord(bytes);
        RequireResult(parsedRecord, "captured ControlRecord failed an independent parse");
        const pbprotocol::ControlRecordView& record = parsedRecord.Value();
        Require(record.sessionTag == bootstrapSessionTag,
            "captured ControlRecord SessionTag disagrees with the same-frame Bootstrap");
        auto admission = receiver_.ReceiveControlRecord(bytes);
        if (!admission)
        {
            if (record.recordType != pbprotocol::ControlRecordType::SessionDescriptor &&
                IsRetryableUnknownSession(admission.Error()))
            {
                return;
            }
            throw RuntimeFailure("ReceiverIngress rejected ControlRecord: " +
                DescribeReceiverError(admission.Error()));
        }
        HandleControlAdmission(admission.Value(), record, timestamp100ns);
    }

    void ProcessControlFragment(const std::span<const std::byte> bytes,
        const std::uint64_t captureObservation)
    {
        auto fragment = receiver_.ReceiveControlFragment(bytes, captureObservation);
        if (!fragment)
        {
            if (IsRetryableUnknownSession(fragment.Error()))
            {
                return;
            }
            throw RuntimeFailure("ReceiverIngress rejected ControlFragment: " +
                DescribeReceiverError(fragment.Error()));
        }
        if (fragment.Value().admission)
        {
            throw RuntimeFailure(
                "completed fragmented ControlRecord is outside the current fixed-Control-window application inventory");
        }
    }

    void HandleControlAdmission(pbreceiver::ReceiverControlAdmission& admission,
        const std::optional<pbprotocol::ControlRecordView> record, const std::int64_t timestamp100ns)
    {
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
                    "current bounded output reservation was not AutoAccept");
                if (!session_)
                {
                    session_ = parsed.Value();
                    Require(session_->segmentCount == 1 &&
                        session_->originalFileSize >= minimumInstantFileBytes &&
                        session_->originalFileSize <= maximumInstantFileBytes,
                        "received Session is outside the current one-Segment 1 byte..8 MiB product boundary");
                    Require(progress_.BindDescriptor(session_->originalFileSize, ElapsedMilliseconds(started_)),
                        "Decoder progress descriptor binding failed");
                    pbstorage::OutputFileConfig storageConfig;
                    storageConfig.outputDirectory = outputDirectory_;
                    storageConfig.sessionTag = record->sessionTag;
                    storageConfig.fileBytes = session_->originalFileSize;
                    storageConfig.maximumFileBytes = maximumInstantFileBytes;
                    const auto storageStatus = pbstorage::OutputFile::Create(storageConfig, storage_);
                    Require(static_cast<bool>(storageStatus), "PBStorage output reservation failed: " +
                        DescribeStorageStatus(storageStatus));
                    const auto storageSnapshot = storage_->GetSnapshot();
                    snapshot_.Update([this, &storageSnapshot](DecoderSnapshot& value)
                    {
                        if (value.runGeneration != runGeneration_)
                        {
                            return;
                        }
                        value.state = DecoderState::ReceivingControl;
                        value.descriptorKnown = true;
                        value.originalFileBytes = session_->originalFileSize;
                        value.remainingRawBytes = session_->originalFileSize;
                        value.recoveryProgress = 0.0;
                        value.sessionIdHex = SessionIdHex(session_->sessionId);
                        value.sessionTag = pbprotocol::DeriveSessionTag(session_->sessionId).value;
                        value.segmentCount = session_->segmentCount;
                        value.outputPath = Utf8FromWide(storageSnapshot.finalPath);
                        value.statusMessage = "Session Descriptor accepted; receiving Control";
                    });
                }
                else
                {
                    Require(*session_ == parsed.Value(), "conflicting repeated SessionDescriptor");
                }
                receiverSessionBound_ = true;
            }
            else if (record->recordType == pbprotocol::ControlRecordType::SegmentDescriptor)
            {
                Require(session_.has_value(), "SegmentDescriptor arrived before authoritative Session binding");
                const auto parsed = pbprotocol::ParseSegmentDescriptor(record->payload, *session_, policy_);
                RequireResult(parsed, "SegmentDescriptor independent parse failed");
                Require(admission.controlAdmission.boundSegmentDescriptor &&
                    admission.controlAdmission.boundSegmentDescriptor->GetDescriptor() == parsed.Value(),
                    "Receiver binding differs from independently parsed SegmentDescriptor");
                if (!segment_)
                {
                    segment_ = parsed.Value();
                    snapshot_.Update([this](DecoderSnapshot& value)
                    {
                        if (value.runGeneration != runGeneration_)
                        {
                            return;
                        }
                        value.state = DecoderState::Receiving;
                        value.currentSegmentOrdinal = segment_->segmentOrdinal;
                        value.compressionCodec = segment_->compressionCodec;
                        value.outerFecMode = segment_->outerFecMode;
                        value.statusMessage = "Segment Descriptor accepted; receiving verified Transport blocks";
                    });
                }
                else
                {
                    Require(*segment_ == parsed.Value(), "conflicting repeated SegmentDescriptor");
                }
            }
            else if (record->recordType == pbprotocol::ControlRecordType::FinalManifest)
            {
                Require(session_.has_value(), "FinalManifest arrived before authoritative Session binding");
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
            StoreCompleted(std::move(*admission.completedSegment), timestamp100ns);
        }
        TryPublish(timestamp100ns);
    }

    void ProcessTransport(const pbdemodd3d11::CaptureDemodulatorResult& result)
    {
        Require(result.demodulation.acceptedTransportBlockCount <=
            result.demodulation.acceptedTransportBlocks.size(),
            "CaptureDemodulator accepted Transport block count is out of bounds");
        acceptedTransportBlocks_ = pbprotocol::SaturatingAddUnsigned(acceptedTransportBlocks_,
            static_cast<std::uint64_t>(result.demodulation.acceptedTransportBlockCount));
        const pbdesktoplevels::FrameEvaluation& evaluation = result.demodulation.evaluation;
        identityFailures_ = pbprotocol::SaturatingAddUnsigned(identityFailures_,
            static_cast<std::uint64_t>(evaluation.identityFailures));
        falseAcceptedCodewords_ = pbprotocol::SaturatingAddUnsigned(falseAcceptedCodewords_,
            static_cast<std::uint64_t>(evaluation.falseAcceptedCodewords));
        for (std::uint32_t index = 0; index < result.demodulation.acceptedTransportBlockCount; index++)
        {
            const auto& accepted = result.demodulation.acceptedTransportBlocks[index];
            Require(accepted.byteCount >= pbprotocol::kTransportMinimumBlockBytes &&
                accepted.byteCount <= accepted.bytes.size(), "accepted Transport byte count is invalid");
            const auto parsed = pbprotocol::ParseTransportBlock(
                std::span(accepted.bytes).first(accepted.byteCount));
            RequireResult(parsed, "accepted Transport failed an independent parse");
            const auto& transport = parsed.Value();
            if (session_ && transport.header.sessionTag != pbprotocol::DeriveSessionTag(session_->sessionId))
            {
                continue;
            }
            Require(transport.header.payloadBytes == transport.payload.size() &&
                transport.payload.size() <= paddedPayload_.size(), "Transport payload is out of bounds");
            std::fill(paddedPayload_.begin(), paddedPayload_.end(), std::byte{0});
            std::copy(transport.payload.begin(), transport.payload.end(), paddedPayload_.begin());
            const pbreceiver::ReceivedTransportBlock block{transport.header.sessionTag,
                transport.header.segmentOrdinal, transport.header.outerBlockId,
                transport.header.payloadBytes, paddedPayload_};
            auto admission = receiver_.ReceiveDataBlock(block, result.metadata.captureObservation);
            if (!admission)
            {
                const auto* protocol = std::get_if<pbprotocol::ProtocolError>(&admission.Error());
                if (protocol != nullptr &&
                    (protocol->code == pbprotocol::ProtocolErrorCode::ResourceLimitExceeded ||
                     protocol->code == pbprotocol::ProtocolErrorCode::UnknownSession))
                {
                    continue;
                }
                throw RuntimeFailure("ReceiverIngress rejected Transport block: " +
                    DescribeReceiverError(admission.Error()));
            }
            if (admission.Value().completedSegment)
            {
                StoreCompleted(std::move(*admission.Value().completedSegment),
                    result.metadata.timestamp.monotonic100ns);
            }
        }
        snapshot_.Update([this](DecoderSnapshot& value)
        {
            if (value.runGeneration != runGeneration_)
            {
                return;
            }
            if (value.state == DecoderState::Receiving)
            {
                value.state = DecoderState::Recovering;
                value.statusMessage = "Outer FEC is converging; progress advances only after Segment verification";
            }
            value.acceptedTransportBlocks = acceptedTransportBlocks_;
            value.identityFailures = identityFailures_;
            value.falseAcceptedCodewords = falseAcceptedCodewords_;
        });
    }

    void StoreCompleted(pbreceiver::ReceiverCompletedSegment&& completed, const std::int64_t timestamp100ns)
    {
        Require(!stored_, "Receiver emitted a duplicate completed Segment after authoritative storage");
        const std::uint64_t encodedBytes = completed.encodedBytes.size();
        snapshot_.Update([this](DecoderSnapshot& value)
        {
            if (value.runGeneration == runGeneration_)
            {
                value.state = DecoderState::Verifying;
                value.statusMessage = "Verifying EncodedDigest, bounded decompression, and RawDigest";
            }
        });
        auto verified = receiver_.VerifyRecoveredSegment(std::move(completed));
        RequireResult(verified, "encoded digest, decompression, or raw digest verification failed");
        pbreceiver::ReceiverVerifiedSegment verifiedSegment = std::move(verified).Value();
        const pbprotocol::SegmentDescriptor& descriptor =
            verifiedSegment.GetBoundSegmentDescriptor().GetDescriptor();
        Require(storage_ && session_ && descriptor.rawOffset == 0 &&
            descriptor.rawSize == verifiedSegment.GetRawBytes().size() &&
            descriptor.rawSize == session_->originalFileSize && encodedBytes == descriptor.encodedSize,
            "verified Segment does not match the bounded output reservation");
        const auto writeStatus = storage_->Write(descriptor.rawOffset, verifiedSegment.GetRawBytes());
        Require(static_cast<bool>(writeStatus), "PBStorage Segment write failed: " +
            DescribeStorageStatus(writeStatus));
        const auto committed = receiver_.CommitStoredSegment(std::move(verifiedSegment));
        RequireResult(committed, "Receiver stored Segment commit failed");
        Require(committed.Value() == pbreceiver::ReceiverSegmentCommitDisposition::Committed,
            "first stored Segment commit was not Committed");
        stored_ = true;
        storedEncodedBytes_ = encodedBytes;
        Require(progress_.ObserveVerifiedRawBytes(descriptor.rawSize, ElapsedMilliseconds(started_)),
            "verified raw-byte progress update failed");
        ApplyProgress();
        TryPublish(timestamp100ns);
    }

    void TryPublish(const std::int64_t timestamp100ns)
    {
        if (published_ || !stored_ || !segment_ || !manifest_ || !storage_)
        {
            return;
        }
        const auto finalized = receiver_.PrepareFinalization(segment_->sessionTag);
        RequireResult(finalized, "Receiver finalization was not authoritative after stored commit");
        Require(finalized.Value() == *manifest_, "Receiver returned a different FinalManifest");
        snapshot_.Update([this](DecoderSnapshot& value)
        {
            if (value.runGeneration == runGeneration_)
            {
                value.state = DecoderState::Publishing;
                value.statusMessage = "WholeFileDigest verification and same-directory final publish";
            }
        });
        const auto storageSnapshot = storage_->GetSnapshot();
        std::string outputPath = Utf8FromWide(storageSnapshot.finalPath);
        std::string wholeFileDigestHex = DigestHex(manifest_->wholeFileDigest.bytes);
        const auto publishStatus = storage_->Publish(manifest_->wholeFileDigest);
        Require(static_cast<bool>(publishStatus), "WholeFileDigest/final publish failed: " +
            DescribeStorageStatus(publishStatus));
        completion_.outputPath = std::move(outputPath);
        completion_.wholeFileDigestHex = std::move(wholeFileDigestHex);
        completion_.recoveryRuntimeMilliseconds = ElapsedMilliseconds(started_);
        completion_.published = true;
        published_ = true;
        Require(storedEncodedBytes_ != 0, "published Segment has no verified encoded-byte accounting");
        RequireTelemetry(telemetry_.RecordVerifiedEncodedBytes(storedEncodedBytes_, timestamp100ns),
            "PBTelemetry RecordVerifiedEncodedBytes");
        UpdateTelemetrySnapshot();
        snapshot_.Update([this](DecoderSnapshot& value)
        {
            if (value.runGeneration != runGeneration_)
            {
                return;
            }
            value.state = DecoderState::Completed;
            value.runEndedUnixMilliseconds = GetUnixTimeMilliseconds();
            value.wholeFileDigestVerified = true;
            value.finalPublishSucceeded = true;
            value.wholeFileDigestHex = completion_.wholeFileDigestHex;
            value.outputPath = completion_.outputPath;
            value.recoveryRuntimeMilliseconds = completion_.recoveryRuntimeMilliseconds;
            value.statusMessage = "文件接收完成：WholeFileDigest PASS 且 final publish 成功";
        });
    }

    void ApplyProgress()
    {
        const ProgressSnapshot progress = progress_.GetSnapshot();
        snapshot_.Update([this, &progress](DecoderSnapshot& value)
        {
            if (value.runGeneration != runGeneration_)
            {
                return;
            }
            value.descriptorKnown = progress.descriptorKnown;
            value.originalFileBytes = progress.totalRawBytes;
            value.verifiedRawBytes = progress.verifiedRawBytes;
            value.remainingRawBytes = progress.remainingRawBytes;
            value.recoveryProgress = progress.progress;
            value.instantVerifiedRawGoodputBytesPerSecond = progress.instantBytesPerSecond;
            value.smoothedVerifiedRawGoodputBytesPerSecond = progress.smoothedBytesPerSecond;
            value.averageVerifiedRawGoodputBytesPerSecond = progress.averageBytesPerSecond;
            value.etaMilliseconds = progress.etaMilliseconds;
        });
    }

    void UpdateVisualSnapshot()
    {
        const VisualIdentitySnapshot visual = visualRate_.GetSnapshot();
        snapshot_.Update([this, &visual](DecoderSnapshot& value)
        {
            if (value.runGeneration != runGeneration_)
            {
                return;
            }
            value.admittedFrameSequenceFps = visual.framesPerSecond;
            value.duplicateFrameSequences = visual.duplicateFrames;
            value.reorderedFrameSequences = visual.reorderedFrames;
            value.frameSequenceGapEvents = visual.gapEvents;
            value.skippedFrameSequences = visual.skippedSequences;
        });
    }

    void UpdateTelemetrySnapshot()
    {
        const pbtelemetry::TelemetrySnapshot telemetry = telemetry_.GetSnapshot();
        snapshot_.Update([this, &telemetry](DecoderSnapshot& value)
        {
            if (value.runGeneration != runGeneration_)
            {
                return;
            }
            value.telemetryCapturedFrames = telemetry.capturedFrames;
            value.telemetryDroppedFrames = telemetry.droppedFrames;
            value.fingerprintedFrames = telemetry.fingerprintedFrames;
            value.captureFps = telemetry.captureFps;
            value.uniqueVisualFps = telemetry.uniqueVisualFps;
            value.telemetryBootstrapAttempts = telemetry.bootstrapAttempts;
            value.telemetryBootstrapSuccesses = telemetry.bootstrapSuccesses;
            value.bootstrapSuccessRate = telemetry.bootstrapSuccessRate;
            value.evaluatedDataFrames = telemetry.fecEvaluatedFrames;
            value.postFecFailedFrames = telemetry.postFecFailedFrames;
            value.comparedCodedBits = telemetry.comparedCodedBits;
            value.erroneousCodedBits = telemetry.erroneousCodedBits;
            value.fecFailures = telemetry.fecFailures;
            value.crcFailures = telemetry.crcFailures;
            value.preFecBerEstimate = telemetry.preFecBerEstimate;
            value.fecFrameErrorRate = telemetry.fecFrameErrorRate;
            value.verifiedEncodedBytes = telemetry.verifiedEncodedBytes;
            value.verifiedEncodedGoodputBitsPerSecond = telemetry.verifiedEncodedGoodputBitsPerSecond;
        });
    }

    pbreceiver::ReceiverIngress& receiver_;
    std::wstring outputDirectory_;
    pbprotocol::ReceiverResourcePolicy policy_;
    SnapshotStore<DecoderSnapshot>& snapshot_;
    AuthoritativeCompletion& completion_;
    std::uint64_t runGeneration_ = 0;
    std::chrono::steady_clock::time_point started_;
    std::unique_ptr<pbstorage::OutputFile> storage_;
    std::optional<pbprotocol::SessionDescriptor> session_;
    std::optional<pbprotocol::SegmentDescriptor> segment_;
    std::optional<pbprotocol::FinalManifest> manifest_;
    std::array<std::byte, outerBlockBytes> paddedPayload_{};
    DecoderProgressTracker progress_;
    VisualIdentityTracker visualRate_;
    pbtelemetry::TelemetryAccumulator telemetry_;
    std::uint64_t storedEncodedBytes_ = 0;
    std::uint64_t pendingDroppedFrames_ = 0;
    std::uint64_t lastCaptureDroppedFrames_ = 0;
    std::uint64_t lastResultQueueDrops_ = 0;
    std::uint64_t lastStaleResultDrops_ = 0;
    std::uint64_t acceptedTransportBlocks_ = 0;
    std::uint64_t identityFailures_ = 0;
    std::uint64_t falseAcceptedCodewords_ = 0;
    bool receiverSessionBound_ = false;
    bool stored_ = false;
    bool published_ = false;
};

} // namespace

RuntimeStatus ValidateEncoderConfig(const EncoderConfig& config)
{
    if (config.sourcePath.empty())
    {
        return RuntimeStatus::Failure("请选择源文件");
    }
    if (config.compressionLevel < 1 || config.compressionLevel > 22)
    {
        return RuntimeStatus::Failure("Compression level 必须位于当前支持范围 1..22");
    }
    if ((config.visualProfile != VisualProfile::DirectLevels2x2 &&
         config.visualProfile != VisualProfile::ShapeChroma) || !config.monitorClientOrigin)
    {
        return RuntimeStatus::Failure("请选择当前真实存在的 Visual Profile 和目标 monitor");
    }
    try
    {
        std::error_code error;
        const std::filesystem::path path(config.sourcePath);
        if (!std::filesystem::is_regular_file(path, error) || error)
        {
            return RuntimeStatus::Failure("源文件不存在或不是常规文件");
        }
        const std::uint64_t fileBytes = std::filesystem::file_size(path, error);
        if (error || fileBytes < minimumInstantFileBytes || fileBytes > maximumInstantFileBytes)
        {
            return RuntimeStatus::Failure("当前单 Segment production path 仅支持 1 byte 到 8 MiB");
        }
    }
    catch (...)
    {
        return RuntimeStatus::Failure("源文件路径无效");
    }
    return {};
}

RuntimeStatus ValidateDecoderConfig(const DecoderConfig& config)
{
    if (config.outputDirectory.empty())
    {
        return RuntimeStatus::Failure("请选择输出目录");
    }
    try
    {
        std::error_code error;
        if (!std::filesystem::is_directory(std::filesystem::path(config.outputDirectory), error) || error)
        {
            return RuntimeStatus::Failure("输出目录不存在或不可用");
        }
    }
    catch (...)
    {
        return RuntimeStatus::Failure("输出目录路径无效");
    }
    if ((config.captureBackend != CaptureBackend::Wgc && config.captureBackend != CaptureBackend::Dxgi) ||
        (config.visualProfile != VisualProfile::DirectLevels2x2 && config.visualProfile != VisualProfile::ShapeChroma))
    {
        return RuntimeStatus::Failure("Capture backend 或 Visual Profile 不在当前 Runtime Option Inventory 中");
    }
    const RECT& rect = config.region.physicalRect;
    const std::int64_t width = static_cast<std::int64_t>(rect.right) - rect.left;
    const std::int64_t height = static_cast<std::int64_t>(rect.bottom) - rect.top;
    const RECT& monitorRect = config.region.monitorPhysicalRect;
    const std::int64_t monitorWidth = static_cast<std::int64_t>(monitorRect.right) - monitorRect.left;
    const std::int64_t monitorHeight = static_cast<std::int64_t>(monitorRect.bottom) - monitorRect.top;
    if (config.region.monitor == nullptr || width != phase1CanvasWidth || height != phase1CanvasHeight ||
        config.region.dpiX == 0 || config.region.dpiY == 0 ||
        config.region.rotation != DXGI_MODE_ROTATION_IDENTITY || monitorWidth <= 0 || monitorHeight <= 0 ||
        rect.left < monitorRect.left || rect.top < monitorRect.top || rect.right > monitorRect.right ||
        rect.bottom > monitorRect.bottom)
    {
        return RuntimeStatus::Failure("当前 ROI 不满足 Phase-1 1920x1080、单显示器、identity rotation 的严格 1:1 contract");
    }
    const auto captureConfig = MakeCaptureConfig(config);
    const auto captureStatus = config.captureBackend == CaptureBackend::Wgc ?
        pbscreencapturewgc::ValidateWgcCaptureConfig(captureConfig.capture) :
        pbscreencapturedxgi::ValidateDxgiCaptureConfig(captureConfig.capture);
    if (!captureStatus)
    {
        return RuntimeStatus::Failure(std::string(GetCaptureBackendName(config.captureBackend)) +
            " capture resource contract invalid: " + DescribeCaptureStatus(captureStatus));
    }
    return {};
}

pbcompression::CompressionResult<pbcompression::EncodedSegment> PrepareEncodedSegment(
    const std::span<const std::byte> rawBytes, const bool compressionEnabled,
    const int compressionLevel)
{
    if (!compressionEnabled)
    {
        pbcompression::EncodedSegment encoded;
        encoded.codec = pbprotocol::CompressionCodec::Raw;
        encoded.bytes.assign(rawBytes.begin(), rawBytes.end());
        return pbcompression::CompressionResult<pbcompression::EncodedSegment>::Success(std::move(encoded));
    }
    pbcompression::CompressionSettings settings;
    settings.compressionLevel = compressionLevel;
    settings.maxOutputBytes = 16ULL * mebibyte;
    return pbcompression::CompressSegment(rawBytes, settings);
}

EncoderRuntime::~EncoderRuntime()
{
    Stop();
}

RuntimeStatus EncoderRuntime::Start(const EncoderConfig& config)
{
    const RuntimeStatus validation = ValidateEncoderConfig(config);
    if (!validation)
    {
        return validation;
    }
    std::string sourcePathUtf8;
    try
    {
        sourcePathUtf8 = Utf8FromWide(config.sourcePath);
    }
    catch (const std::exception& exception)
    {
        return RuntimeStatus::Failure(std::string("源文件路径编码无效：") + exception.what());
    }
    std::unique_lock lock(lifecycleMutex_);
    if (workerRunning_)
    {
        return RuntimeStatus::Failure("Encoder 已处于 Preparing/Broadcasting/Stopping，拒绝重复 Start");
    }
    if (worker_.joinable())
    {
        worker_.join();
    }
    if (nextRunGeneration_ == 0)
    {
        return RuntimeStatus::Failure("run generation exhausted");
    }
    const std::uint64_t runGeneration = nextRunGeneration_++;
    stopRequested_ = false;
    EncoderSnapshot initial;
    initial.state = EncoderState::Preparing;
    initial.runGeneration = runGeneration;
    initial.runId = config.runId.empty() ? "pending" : config.runId;
    initial.runStartedUnixMilliseconds = GetUnixTimeMilliseconds();
    initial.sourcePath = std::move(sourcePathUtf8);
    initial.visualProfile = config.visualProfile;
    initial.dataWindowLeft = config.monitorClientOrigin->x;
    initial.dataWindowTop = config.monitorClientOrigin->y;
    initial.statusMessage = "Preparing source, descriptors, compression, and outer FEC";
    initial.remoteMetadata = config.remoteMetadata;
    try
    {
        snapshot_.Replace(std::move(initial));
    }
    catch (const std::exception& exception)
    {
        return RuntimeStatus::Failure(std::string("无法初始化 Encoder snapshot：") + exception.what());
    }
    catch (...)
    {
        return RuntimeStatus::Failure("无法初始化 Encoder snapshot");
    }
    workerRunning_ = true;
    try
    {
        worker_ = std::thread(&EncoderRuntime::Run, this, config, runGeneration);
    }
    catch (const std::exception& exception)
    {
        workerRunning_ = false;
        try
        {
            snapshot_.Update([runGeneration, &exception](EncoderSnapshot& value)
            {
                if (value.runGeneration == runGeneration)
                {
                    value.state = EncoderState::Failed;
                    value.runEndedUnixMilliseconds = GetUnixTimeMilliseconds();
                    value.errorDetail = exception.what();
                }
            });
        }
        catch (...)
        {
        }
        return RuntimeStatus::Failure("无法创建 Encoder worker thread");
    }
    return {};
}

void EncoderRuntime::RequestStop() noexcept
{
    stopRequested_ = true;
    try
    {
        const std::scoped_lock lock(lifecycleMutex_);
        stopRequested_ = true;
        snapshot_.Update([](EncoderSnapshot& value)
        {
            if (value.state == EncoderState::Preparing || value.state == EncoderState::Broadcasting)
            {
                value.state = EncoderState::Stopping;
            }
        });
    }
    catch (...)
    {
    }
}

void EncoderRuntime::Stop() noexcept
{
    RequestStop();
    try
    {
        const std::scoped_lock lock(lifecycleMutex_);
        stopRequested_ = true;
        if (worker_.joinable())
        {
            worker_.join();
        }
    }
    catch (...)
    {
        std::terminate();
    }
}

EncoderSnapshot EncoderRuntime::GetSnapshot() const
{
    return snapshot_.Get();
}

void EncoderRuntime::Run(EncoderConfig config, const std::uint64_t runGeneration) noexcept
{
    WorkerRunningGuard runningGuard(workerRunning_);
    bool sourceStable = true;
    try
    {
        const auto workerStarted = std::chrono::steady_clock::now();
        SourceFile source = ReadSourceFile(config.sourcePath);
        if (stopRequested_)
        {
            snapshot_.Update([runGeneration](EncoderSnapshot& value)
            {
                if (value.runGeneration == runGeneration)
                {
                    value.state = EncoderState::Stopped;
                    value.runEndedUnixMilliseconds = GetUnixTimeMilliseconds();
                    value.statusMessage = "Stopped during source preparation";
                }
            });
            return;
        }
        const TransferDescription description = DescribeSource(source.bytes, config.compressionEnabled,
            config.compressionLevel);
        if (stopRequested_)
        {
            snapshot_.Update([runGeneration](EncoderSnapshot& value)
            {
                if (value.runGeneration == runGeneration)
                {
                    value.state = EncoderState::Stopped;
                    value.runEndedUnixMilliseconds = GetUnixTimeMilliseconds();
                    value.statusMessage = "Stopped during descriptor/FEC preparation";
                }
            });
            return;
        }
        const ProfileBinding profile = GetProfileBinding(config.visualProfile);
        SenderFrameBuilder builder(profile, description);
        const std::string runId = config.runId.empty() ? GenerateRunId() : config.runId;
        const CarouselSnapshot initialCarousel = builder.GetCarouselSnapshot();
        snapshot_.Update([&](EncoderSnapshot& value)
        {
            if (value.runGeneration != runGeneration)
            {
                return;
            }
            value.runId = runId;
            value.sourceBytes = source.bytes.size();
            value.sessionIdHex = SessionIdHex(description.session.sessionId);
            value.sessionTag = description.segment.sessionTag.value;
            value.wholeFileDigestHex = DigestHex(description.manifest.wholeFileDigest.bytes);
            value.compressionCodec = description.segment.compressionCodec;
            value.outerFecMode = description.segment.outerFecMode;
            value.outerBlockCount = builder.GetBlockCount();
            value.cycleFrameCount = initialCarousel.cycleFrameCount;
            value.segmentCount = 1;
            value.statusMessage = "Creating the production D3D11 Data Window";
        });
        if (stopRequested_)
        {
            snapshot_.Update([runGeneration](EncoderSnapshot& value)
            {
                if (value.runGeneration == runGeneration)
                {
                    value.state = EncoderState::Stopped;
                    value.runEndedUnixMilliseconds = GetUnixTimeMilliseconds();
                    value.statusMessage = "Stopped during preparation";
                }
            });
            return;
        }
        pbrenderd3d::DataWindowConfig windowConfig;
        windowConfig.width = phase1CanvasWidth;
        windowConfig.height = phase1CanvasHeight;
        windowConfig.clientOrigin = config.monitorClientOrigin;
        auto created = pbrenderd3d::DataWindow::Create(windowConfig);
        if (!created)
        {
            throw RuntimeFailure("DataWindow creation failed: " + DescribePresentationStatus(created.Error()));
        }
        std::unique_ptr<pbrenderd3d::DataWindow> window = std::move(created).Value();
        std::uint64_t frameSequence = 0;
        std::uint64_t generatedPayloadBytes = 0;
        bool frameBuilt = false;
        bool broadcastStarted = false;
        auto nextStabilityCheck = workerStarted;
        std::optional<std::chrono::steady_clock::time_point> broadcastStartedAt;
        for (;;)
        {
            const auto now = std::chrono::steady_clock::now();
            const pbrenderd3d::DataWindowSnapshot windowSnapshot = window->GetSnapshot();
            if (windowSnapshot.state == pbrenderd3d::WindowState::Failed)
            {
                throw RuntimeFailure("DataWindow failed: " + DescribePresentationStatus(windowSnapshot.error));
            }
            if (stopRequested_)
            {
                window->RequestStop();
            }
            if (windowSnapshot.state == pbrenderd3d::WindowState::Stopped)
            {
                break;
            }
            if (now >= nextStabilityCheck)
            {
                sourceStable = IsSourceStable(source);
                Require(sourceStable, "源文件在 Session 广播期间发生变化，Session 已中止");
                nextStabilityCheck = now + std::chrono::seconds(1);
            }
            const bool presentationStable = HasStablePresentationContract(windowSnapshot);
            if (!broadcastStarted && presentationStable)
            {
                broadcastStarted = true;
                broadcastStartedAt = now;
                snapshot_.Update([runGeneration](EncoderSnapshot& value)
                {
                    if (value.runGeneration == runGeneration && value.state == EncoderState::Preparing)
                    {
                        value.state = EncoderState::Broadcasting;
                        value.statusMessage = "Broadcasting continuously; receiver completion is visible only on Decoder";
                    }
                });
            }
            const bool canBuild = presentationStable && windowSnapshot.state == pbrenderd3d::WindowState::Running &&
                !frameBuilt;
            if (canBuild)
            {
                static_cast<void>(builder.Build(frameSequence));
                frameBuilt = true;
            }
            if (presentationStable && windowSnapshot.state == pbrenderd3d::WindowState::Running && frameBuilt &&
                !windowSnapshot.pendingFrame && !stopRequested_)
            {
                const std::uint32_t outerBlockId = builder.GetCurrentOuterBlockId();
                const auto& pixels = builder.GetBuiltPixels();
                const auto submit = window->SubmitFrame({pixels, phase1CanvasWidth, phase1CanvasHeight,
                    static_cast<std::size_t>(phase1CanvasWidth) * 4U, frameSequence,
                    windowSnapshot.timing.presentationEpoch});
                if (submit)
                {
                    const auto nextGeneratedPayloadBytes = pbprotocol::CheckedAddUint64(generatedPayloadBytes,
                        builder.GetGeneratedPayloadBytesInFrame());
                    RequireResult(nextGeneratedPayloadBytes, "generated payload telemetry overflow");
                    generatedPayloadBytes = nextGeneratedPayloadBytes.Value();
                    Require(frameSequence != (std::numeric_limits<std::uint64_t>::max)(),
                        "FrameSequence exhausted");
                    builder.Advance();
                    frameSequence++;
                    frameBuilt = false;
                    const CarouselSnapshot carouselAfter = builder.GetCarouselSnapshot();
                    Require(broadcastStartedAt.has_value(), "stable presentation has no broadcast start timestamp");
                    const std::uint64_t broadcastMilliseconds = ElapsedMilliseconds(*broadcastStartedAt);
                    const double elapsedSeconds = static_cast<double>(broadcastMilliseconds) / 1000.0;
                    snapshot_.Update([&](EncoderSnapshot& value)
                    {
                        if (value.runGeneration != runGeneration)
                        {
                            return;
                        }
                        value.broadcastRuntimeMilliseconds = broadcastMilliseconds;
                        value.cycleCount = carouselAfter.cycleCount;
                        value.cyclePosition = carouselAfter.cyclePosition;
                        value.cycleFrameCount = carouselAfter.cycleFrameCount;
                        value.currentOuterBlockId = outerBlockId;
                        value.frameSequence = frameSequence;
                        value.presentationEpoch = windowSnapshot.timing.presentationEpoch;
                        value.presentedVisualFps = windowSnapshot.timing.presentedVisualFps;
                        value.presentCallFps = windowSnapshot.timing.presentCallFps;
                        value.generatedVisualFramesPerSecond = elapsedSeconds > 0 ?
                            static_cast<double>(frameSequence) / elapsedSeconds : 0;
                        value.generatedPayloadBytesPerSecond = elapsedSeconds > 0 ?
                            static_cast<double>(generatedPayloadBytes) / elapsedSeconds : 0;
                        value.submittedFrames = windowSnapshot.submittedFrames;
                        value.replacedPendingFrames = windowSnapshot.replacedPendingFrames;
                        value.pendingFrames = 1;
                        value.pendingHighWater = 1;
                        value.candidateContractSatisfied = windowSnapshot.candidateContractSatisfied;
                    });
                }
                else if (submit.code != pbrenderd3d::PresentationErrorCode::EpochMismatch &&
                    submit.code != pbrenderd3d::PresentationErrorCode::Paused &&
                    submit.code != pbrenderd3d::PresentationErrorCode::NotRunning)
                {
                    throw RuntimeFailure("DataWindow SubmitFrame failed: " + DescribePresentationStatus(submit));
                }
            }
            else
            {
                const std::uint64_t broadcastMilliseconds = broadcastStartedAt ?
                    ElapsedMilliseconds(*broadcastStartedAt) : 0;
                snapshot_.Update([&](EncoderSnapshot& value)
                {
                    if (value.runGeneration != runGeneration)
                    {
                        return;
                    }
                    value.broadcastRuntimeMilliseconds = broadcastMilliseconds;
                    value.presentationEpoch = windowSnapshot.timing.presentationEpoch;
                    value.presentedVisualFps = windowSnapshot.timing.presentedVisualFps;
                    value.presentCallFps = windowSnapshot.timing.presentCallFps;
                    value.submittedFrames = windowSnapshot.submittedFrames;
                    value.replacedPendingFrames = windowSnapshot.replacedPendingFrames;
                    value.pendingFrames = windowSnapshot.pendingFrame ? 1U : 0U;
                    value.pendingHighWater = windowSnapshot.pendingFrame ? 1U : value.pendingHighWater;
                    value.candidateContractSatisfied = windowSnapshot.candidateContractSatisfied;
                });
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        window->Stop();
        const pbrenderd3d::DataWindowSnapshot stopped = window->GetSnapshot();
        Require(stopped.state == pbrenderd3d::WindowState::Stopped &&
            !stopped.pendingFrame && !stopped.inFlightFrame,
            "DataWindow did not complete a clean bounded shutdown");
        Require(stopRequested_, "DataWindow stopped without an explicit user stop request");
        sourceStable = IsSourceStable(source);
        Require(sourceStable, "源文件在 Session 广播期间发生变化，Session 已中止");
        const std::uint64_t broadcastMilliseconds = broadcastStartedAt ?
            ElapsedMilliseconds(*broadcastStartedAt) : 0;
        snapshot_.Update([&](EncoderSnapshot& value)
        {
            if (value.runGeneration != runGeneration)
            {
                return;
            }
            value.state = EncoderState::Stopped;
            value.runEndedUnixMilliseconds = GetUnixTimeMilliseconds();
            value.broadcastRuntimeMilliseconds = broadcastMilliseconds;
            value.sourceStable = true;
            value.presentationEpoch = stopped.timing.presentationEpoch;
            value.presentedVisualFps = stopped.timing.presentedVisualFps;
            value.presentCallFps = stopped.timing.presentCallFps;
            value.submittedFrames = stopped.submittedFrames;
            value.replacedPendingFrames = stopped.replacedPendingFrames;
            value.statusMessage = "Broadcast stopped by user; no sender-side receiver completion was inferred";
        });
    }
    catch (const std::exception& exception)
    {
        try
        {
            snapshot_.Update([&](EncoderSnapshot& value)
            {
                if (value.runGeneration != runGeneration)
                {
                    return;
                }
                value.state = EncoderState::Failed;
                value.runEndedUnixMilliseconds = GetUnixTimeMilliseconds();
                value.statusMessage = "Encoder failed";
                value.errorDetail = exception.what();
                value.sourceStable = sourceStable;
            });
        }
        catch (...)
        {
        }
    }
    catch (...)
    {
        try
        {
            snapshot_.Update([runGeneration](EncoderSnapshot& value)
            {
                if (value.runGeneration == runGeneration)
                {
                    value.state = EncoderState::Failed;
                    value.runEndedUnixMilliseconds = GetUnixTimeMilliseconds();
                    value.statusMessage = "Encoder failed";
                    value.errorDetail = "unknown non-standard exception";
                }
            });
        }
        catch (...)
        {
        }
    }
}

DecoderRuntime::~DecoderRuntime()
{
    Stop();
}

RuntimeStatus DecoderRuntime::Start(const DecoderConfig& config)
{
    const RuntimeStatus validation = ValidateDecoderConfig(config);
    if (!validation)
    {
        return validation;
    }
    std::unique_lock lock(lifecycleMutex_);
    if (workerRunning_)
    {
        return RuntimeStatus::Failure("Decoder 已处于接收/停止流程，拒绝重复 Start");
    }
    if (worker_.joinable())
    {
        worker_.join();
    }
    if (nextRunGeneration_ == 0)
    {
        return RuntimeStatus::Failure("run generation exhausted");
    }
    const std::uint64_t runGeneration = nextRunGeneration_++;
    stopRequested_ = false;
    DecoderSnapshot initial;
    initial.state = DecoderState::WaitingForBootstrap;
    initial.runGeneration = runGeneration;
    initial.runId = config.runId.empty() ? "pending" : config.runId;
    initial.runStartedUnixMilliseconds = GetUnixTimeMilliseconds();
    initial.requestedBackend = config.captureBackend;
    initial.backendReason = "Starting the explicitly requested backend; no fallback policy is enabled";
    initial.visualProfile = config.visualProfile;
    initial.roiLeft = config.region.physicalRect.left;
    initial.roiTop = config.region.physicalRect.top;
    initial.roiWidth = static_cast<std::uint32_t>(static_cast<std::int64_t>(config.region.physicalRect.right) -
        config.region.physicalRect.left);
    initial.roiHeight = static_cast<std::uint32_t>(static_cast<std::int64_t>(config.region.physicalRect.bottom) -
        config.region.physicalRect.top);
    initial.monitorLeft = config.region.monitorPhysicalRect.left;
    initial.monitorTop = config.region.monitorPhysicalRect.top;
    initial.monitorWidth = static_cast<std::uint32_t>(static_cast<std::int64_t>(config.region.monitorPhysicalRect.right) -
        config.region.monitorPhysicalRect.left);
    initial.monitorHeight = static_cast<std::uint32_t>(static_cast<std::int64_t>(config.region.monitorPhysicalRect.bottom) -
        config.region.monitorPhysicalRect.top);
    initial.dpiX = config.region.dpiX;
    initial.dpiY = config.region.dpiY;
    initial.rotation = static_cast<std::uint32_t>(config.region.rotation);
    initial.statusMessage = "Waiting for same-profile Bootstrap pixels";
    initial.remoteMetadata = config.remoteMetadata;
    try
    {
        snapshot_.Replace(std::move(initial));
    }
    catch (const std::exception& exception)
    {
        return RuntimeStatus::Failure(std::string("无法初始化 Decoder snapshot：") + exception.what());
    }
    catch (...)
    {
        return RuntimeStatus::Failure("无法初始化 Decoder snapshot");
    }
    workerRunning_ = true;
    try
    {
        worker_ = std::thread(&DecoderRuntime::Run, this, config, runGeneration);
    }
    catch (const std::exception& exception)
    {
        workerRunning_ = false;
        try
        {
            snapshot_.Update([runGeneration, &exception](DecoderSnapshot& value)
            {
                if (value.runGeneration == runGeneration)
                {
                    value.state = DecoderState::Failed;
                    value.runEndedUnixMilliseconds = GetUnixTimeMilliseconds();
                    value.errorDetail = exception.what();
                }
            });
        }
        catch (...)
        {
        }
        return RuntimeStatus::Failure("无法创建 Decoder worker thread");
    }
    return {};
}

void DecoderRuntime::RequestStop() noexcept
{
    stopRequested_ = true;
    try
    {
        const std::scoped_lock lock(lifecycleMutex_);
        stopRequested_ = true;
        snapshot_.Update([](DecoderSnapshot& value)
        {
            if (value.state != DecoderState::Idle && value.state != DecoderState::Completed &&
                value.state != DecoderState::Failed && value.state != DecoderState::Stopped)
            {
                value.state = DecoderState::Stopping;
            }
        });
    }
    catch (...)
    {
    }
}

void DecoderRuntime::Stop() noexcept
{
    RequestStop();
    try
    {
        const std::scoped_lock lock(lifecycleMutex_);
        stopRequested_ = true;
        if (worker_.joinable())
        {
            worker_.join();
        }
    }
    catch (...)
    {
        std::terminate();
    }
}

DecoderSnapshot DecoderRuntime::GetSnapshot() const
{
    return snapshot_.Get();
}

void DecoderRuntime::Run(const DecoderConfig& config, const std::uint64_t runGeneration) noexcept
{
    WorkerRunningGuard runningGuard(workerRunning_);
    AuthoritativeCompletion completion;
    try
    {
        const auto started = std::chrono::steady_clock::now();
        const std::string runId = config.runId.empty() ? GenerateRunId() : config.runId;
        snapshot_.Update([&](DecoderSnapshot& value)
        {
            if (value.runGeneration == runGeneration)
            {
                value.runId = runId;
            }
        });
        if (stopRequested_)
        {
            snapshot_.Update([runGeneration](DecoderSnapshot& value)
            {
                if (value.runGeneration == runGeneration)
                {
                    value.state = DecoderState::Stopped;
                    value.runEndedUnixMilliseconds = GetUnixTimeMilliseconds();
                    value.statusMessage = "Stopped before capture startup";
                }
            });
            return;
        }
        const ProfileBinding profile = GetProfileBinding(config.visualProfile);
        const pbprotocol::ReceiverResourcePolicy policy = pbprotocol::GetDefaultReceiverResourcePolicy();
        auto receiverResult = pbreceiver::ReceiverIngress::Create(policy, outerBlockBytes);
        RequireResult(receiverResult, "ReceiverIngress creation failed");
        pbreceiver::ReceiverIngress receiver = std::move(receiverResult).Value();
        ReceiverPipeline pipeline(receiver, config.outputDirectory, policy, snapshot_, completion, runGeneration,
            started);

        pbdemodd3d11::CaptureDemodulatorConfig demodConfig;
        demodConfig.visualProfileId = profile.visualProfileId;
        demodConfig.slotCount = captureDemodulatorSlotCount;
        demodConfig.maximumFrameAgeMilliseconds = 250;
        demodConfig.resultQueueCapacity = captureResultQueueCapacity;
        demodConfig.maximumResidentBytes = maximumDemodulatorResidentBytes;
        demodConfig.evaluationMode = pbdesktoplevels::EvaluationMode::Transport;
        std::shared_ptr<pbdemodd3d11::CaptureDemodulator> demodulator;
        const auto demodStatus = pbdemodd3d11::CaptureDemodulator::Create(demodConfig, demodulator);
        Require(static_cast<bool>(demodStatus), "CaptureDemodulator creation failed: " +
            DescribeCaptureStatus(demodStatus));

        const auto captureConfig = MakeCaptureConfig(config);
        std::shared_ptr<pbcapturenormalize::ScreenCaptureConsumer> consumer = demodulator;
        NativeCaptureSession capture(config.captureBackend, captureConfig, consumer);
        snapshot_.Update([&](DecoderSnapshot& value)
        {
            if (value.runGeneration == runGeneration)
            {
                value.actualBackend = config.captureBackend;
                value.backendReason = "Explicit backend selected; no fallback policy was used";
            }
        });
        std::uint64_t receiverCaptureEpoch = 1;
        for (;;)
        {
            if (stopRequested_)
            {
                capture.RequestStop();
                break;
            }
            std::uint32_t drained = 0;
            pbdemodd3d11::CaptureDemodulatorResult result;
            while (!stopRequested_ && drained < pbdemodd3d11::maximumCaptureDemodResultQueue &&
                demodulator->TakeResult(result))
            {
                if (stopRequested_)
                {
                    break;
                }
                const std::uint64_t resultEpoch = result.metadata.domain.captureEpoch;
                Require(resultEpoch >= receiverCaptureEpoch,
                    "stale CaptureEpoch result escaped the bounded CaptureDemodulator queue");
                if (resultEpoch > receiverCaptureEpoch)
                {
                    const auto reset = receiver.ResetCaptureEpoch(outerBlockBytes);
                    RequireResult(reset, "ReceiverIngress CaptureEpoch reset failed");
                    Require(reset.Value(), "ReceiverIngress CaptureEpoch reset made no state transition");
                    receiverCaptureEpoch = resultEpoch;
                    pipeline.CaptureEpochReset(ElapsedMilliseconds(started));
                }
                pipeline.Process(result);
                drained++;
                if (pipeline.IsCompleted())
                {
                    capture.RequestStop();
                    break;
                }
            }
            const pbcapturenormalize::CaptureSnapshot captureSnapshot = capture.GetSnapshot();
            const pbdemodd3d11::CaptureDemodulatorSnapshot demodSnapshot = demodulator->GetSnapshot();
            pipeline.ObserveDroppedFrames(captureSnapshot.droppedFrames,
                demodSnapshot.resultQueueDrops, demodSnapshot.staleResultDrops);
            if (captureSnapshot.state == pbcapturenormalize::CaptureState::Failed ||
                (demodSnapshot.error.code != pbcapturenormalize::CaptureError::None &&
                 captureSnapshot.state != pbcapturenormalize::CaptureState::Recreating))
            {
                throw RuntimeFailure("capture/demod entered terminal failure: capture=" +
                    DescribeCaptureStatus(captureSnapshot.error) + " consumer=" +
                    DescribeCaptureStatus(demodSnapshot.error));
            }
            snapshot_.Update([&](DecoderSnapshot& value)
            {
                if (value.runGeneration != runGeneration)
                {
                    return;
                }
                value.captureEpoch = captureSnapshot.captureEpoch;
                value.captureArrivedFrames = captureSnapshot.arrivedFrames;
                value.captureDeliveredFrames = captureSnapshot.deliveredFrames;
                value.captureDroppedFrames = captureSnapshot.droppedFrames;
                value.captureRecreates = captureSnapshot.recreates;
                value.captureDeviceRecoveries = captureSnapshot.deviceRecoveries;
                value.bootstrapAcceptedFrames = demodSnapshot.bootstrapAcceptedFrames;
                value.bootstrapRejectedFrames = demodSnapshot.bootstrapRejectedFrames;
                value.bootstrapMismatchFrames = demodSnapshot.bootstrapErasures[
                    static_cast<std::size_t>(pbmodulation::LocalDesktopErasureReason::BootstrapMismatch)];
                value.bootstrapControlFrameFailures = demodSnapshot.controlFrameFailures;
                value.frameLeaseHighWater = captureSnapshot.frameLeaseHighWater;
                value.demodPendingHighWater = demodSnapshot.pendingHighWater;
                value.resultQueueHighWater = demodSnapshot.resultQueueHighWater;
                value.staleResultDrops = demodSnapshot.staleResultDrops;
                value.roiGpuTimeTotal100ns = captureSnapshot.roiCopyTimeTotal100ns;
                value.demodGpuTimeTotal100ns = demodSnapshot.demodulator.gpuTimeTotal100ns;
                value.bootstrapCpuTimeTotal100ns = demodSnapshot.bootstrapCpuTimeTotal100ns;
                value.postGpuFecCpuTimeTotal100ns = demodSnapshot.demodulationCpuTimeTotal100ns;
                value.recoveryRuntimeMilliseconds = ElapsedMilliseconds(started);
            });
            pipeline.ObserveStall(ElapsedMilliseconds(started));
            if (pipeline.IsCompleted())
            {
                capture.RequestStop();
                break;
            }
            if (stopRequested_)
            {
                capture.RequestStop();
                break;
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
        const auto stopStatus = capture.Stop();
        Require(static_cast<bool>(stopStatus), "capture shutdown failed: " + DescribeCaptureStatus(stopStatus));
        const pbcapturenormalize::CaptureSnapshot stoppedCapture = capture.GetSnapshot();
        const pbdemodd3d11::CaptureDemodulatorSnapshot stoppedDemod = demodulator->GetSnapshot();
        pipeline.ObserveDroppedFrames(stoppedCapture.droppedFrames,
            stoppedDemod.resultQueueDrops, stoppedDemod.staleResultDrops);
        pipeline.EndCaptureTelemetry();
        Require(stoppedCapture.shutdownComplete && !stoppedCapture.deferredCleanup &&
            stoppedCapture.liveFrameLeases == 0 && stoppedCapture.busyRoiTextures == 0 &&
            stoppedDemod.demodulator.shutdown && stoppedDemod.pendingFrames == 0 &&
            stoppedDemod.queuedResults == 0,
            "capture/demod shutdown did not retire all bounded resources");
        if (!pipeline.IsCompleted())
        {
            snapshot_.Update([&](DecoderSnapshot& value)
            {
                if (value.runGeneration == runGeneration)
                {
                    value.state = DecoderState::Stopped;
                    value.runEndedUnixMilliseconds = GetUnixTimeMilliseconds();
                    value.recoveryRuntimeMilliseconds = ElapsedMilliseconds(started);
                    value.statusMessage = "Receive stopped by user; no final file was published";
                }
            });
        }
    }
    catch (const std::exception& exception)
    {
        try
        {
            snapshot_.Update([&](DecoderSnapshot& value)
            {
                if (value.runGeneration != runGeneration)
                {
                    return;
                }
                value.runEndedUnixMilliseconds = GetUnixTimeMilliseconds();
                if (completion.published)
                {
                    value.state = DecoderState::Completed;
                    value.wholeFileDigestVerified = true;
                    value.finalPublishSucceeded = true;
                    value.wholeFileDigestHex = completion.wholeFileDigestHex;
                    value.outputPath = completion.outputPath;
                    value.recoveryRuntimeMilliseconds = completion.recoveryRuntimeMilliseconds;
                    value.statusMessage = "文件已权威发布；后续 capture/demod 清理或完成快照记录存在警告";
                    value.errorDetail = std::string("Post-publish cleanup warning: ") + exception.what();
                    return;
                }
                value.state = DecoderState::Failed;
                value.statusMessage = "Decoder failed; final file was not accepted";
                value.errorDetail = exception.what();
                if (!value.actualBackend)
                {
                    value.backendReason = std::string(GetCaptureBackendName(value.requestedBackend)) +
                        " startup failed; no fallback was attempted: " + exception.what();
                }
            });
        }
        catch (...)
        {
        }
    }
    catch (...)
    {
        try
        {
            snapshot_.Update([&](DecoderSnapshot& value)
            {
                if (value.runGeneration != runGeneration)
                {
                    return;
                }
                value.runEndedUnixMilliseconds = GetUnixTimeMilliseconds();
                if (completion.published)
                {
                    value.state = DecoderState::Completed;
                    value.wholeFileDigestVerified = true;
                    value.finalPublishSucceeded = true;
                    value.wholeFileDigestHex = completion.wholeFileDigestHex;
                    value.outputPath = completion.outputPath;
                    value.recoveryRuntimeMilliseconds = completion.recoveryRuntimeMilliseconds;
                    value.statusMessage = "文件已权威发布；后续发生未知清理警告";
                    value.errorDetail = "Post-publish unknown non-standard exception";
                }
                else
                {
                    value.state = DecoderState::Failed;
                    value.statusMessage = "Decoder failed; final file was not accepted";
                    value.errorDetail = "unknown non-standard exception";
                }
            });
        }
        catch (...)
        {
        }
    }
}

} // namespace pbapp
