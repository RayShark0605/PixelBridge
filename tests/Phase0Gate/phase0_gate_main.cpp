#include "pbcompression/segment_compression.h"
#include "pbinnerfec/qc_ldpc_codec.h"
#include "pbmodulation/reference_raster.h"
#include "pbouterfec/direct_repeat.h"
#include "pbouterfec/wirehair_v2.h"
#include "pbreceiver/receiver_ingress.h"
#include "pbreceiver/resume_replay.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/checked_integer.h"
#include "pbprotocol/control_fragment_codec.h"
#include "pbprotocol/descriptor_codec.h"
#include "pbprotocol/protocol_version.h"
#include "pbprotocol/resume_state.h"
#include "pbprotocol/transport_block_codec.h"
#include "reference_frame_receiver.h"
#include "reference_frame_tests.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace
{

constexpr std::uint64_t kMebibyte = 1024ULL * 1024ULL;
constexpr std::uint64_t kSourceSegmentBytes = 8ULL * kMebibyte;
constexpr std::uint32_t kOuterBlockBytes = 1314;
constexpr std::size_t kRobustInfoBytes = 1350;
constexpr std::size_t kRobustCodewordBytes = 2025;
constexpr std::size_t kCodewordsPerFrame =
    pbmodulation::kReferenceDataRegionBytes / kRobustCodewordBytes;
constexpr std::uint64_t kReferenceVisualProfileId =
    0x5042524546524153ULL;
static_assert(kCodewordsPerFrame == 27);
static_assert(kRobustInfoBytes == 1350);
static_assert(kRobustCodewordBytes == 2025);

class GateFailure final : public std::runtime_error
{
public:
    explicit GateFailure(const std::string& message)
        : std::runtime_error(message)
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

void CreateFreshCaseDirectory(const std::filesystem::path& caseRoot)
{
    // Preserve a failed run or an unrelated existing directory. Successful
    // runs remove only the case directory they created themselves.
    Require(!std::filesystem::exists(std::filesystem::symlink_status(caseRoot)),
        "case scratch already exists; preserve it and choose a fresh --scratch path");
    Require(std::filesystem::create_directory(caseRoot), "cannot create fresh case scratch");
}

template <typename ResultType>
void RequireResult(const ResultType& result, const std::string_view message)
{
    Require(static_cast<bool>(result), message);
}

[[nodiscard]] std::string EscapeJson(const std::string_view text)
{
    std::string escaped;
    escaped.reserve(text.size());
    for (const char character : text)
    {
        switch (character)
        {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped += character;
            break;
        }
    }
    return escaped;
}

[[nodiscard]] std::string DigestHex(
    const std::array<std::byte, pbprotocol::kDigestBytes>& digest)
{
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (const std::byte byteValue : digest)
    {
        stream << std::setw(2)
               << static_cast<unsigned int>(
                      std::to_integer<std::uint8_t>(byteValue));
    }
    return stream.str();
}

class EvidenceWriter
{
public:
    explicit EvidenceWriter(const std::filesystem::path& evidencePath)
    {
        const std::filesystem::path parentPath = evidencePath.parent_path();
        if (!parentPath.empty())
        {
            std::filesystem::create_directories(parentPath);
        }
        stream_.open(evidencePath, std::ios::binary | std::ios::trunc);
        Require(stream_.is_open(), "cannot create evidence JSONL");
    }

    void Write(const std::string& jsonLine)
    {
        stream_ << jsonLine << '\n';
        stream_.flush();
        Require(stream_.good(), "cannot flush evidence JSONL");
        std::cout << jsonLine << '\n';
    }

private:
    std::ofstream stream_;
};

class SplitMix64
{
public:
    explicit SplitMix64(const std::uint64_t seed) noexcept
        : state_(seed)
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

enum class SourcePattern : std::uint8_t
{
    DeterministicRandom,
    Compressible,
    CompressedCompletedResume
};

struct FileCaseSpec
{
    std::string name;
    std::uint64_t fileBytes = 0;
    SourcePattern pattern = SourcePattern::DeterministicRandom;
    std::uint64_t seed = 0;
    std::optional<pbprotocol::CompressionCodec> expectedFirstCodec;
    std::optional<pbprotocol::OuterFecMode> expectedFirstOuterMode;
};

struct GateMetrics
{
    std::uint64_t segmentCount = 0;
    std::uint64_t directRepeatSegmentCount = 0;
    std::uint64_t wirehairSegmentCount = 0;
    std::uint64_t frameCount = 0;
    std::uint64_t ldpcCodewordCount = 0;
    std::uint64_t transportBlockCount = 0;
    std::uint64_t duplicateBlockCount = 0;
    std::uint64_t duplicateFrameCount = 0;
    std::uint64_t droppedSystematicBlockCount = 0;
    std::uint64_t repairBlockCount = 0;
    std::uint64_t storedCommitCount = 0;
    std::uint64_t recoveredVerificationCount = 0;
    std::uint64_t resumedStoredVerificationCount = 0;
    std::uint64_t restoredCommitCount = 0;
    std::uint64_t maxActiveSegmentCount = 0;
    std::uint64_t maxTrackedWorkingBytes = 0;
    std::uint64_t maxActiveOuterDecoderCount = 0;
    std::uint64_t maxReservedOuterDecoderBytes = 0;
    std::uint64_t maxSenderActiveSegmentCount = 0;
    std::uint64_t currentSenderWorkingBytes = 0;
    std::uint64_t pipelineWorkingBytes = 0;
};

// This counter covers major caller-owned byte buffers, not allocator RSS,
// private zstd/Wirehair workspaces, or the separate decoder reservation.
void TrackCallerWorkingBytes(GateMetrics& metrics, const std::uint64_t additionalBytes)
{
    const auto baseResult = pbprotocol::CheckedAddUint64(
        metrics.currentSenderWorkingBytes, metrics.pipelineWorkingBytes);
    RequireResult(baseResult, "caller working byte counter overflow");
    const auto totalResult = pbprotocol::CheckedAddUint64(baseResult.Value(), additionalBytes);
    RequireResult(totalResult, "caller working byte counter overflow");
    metrics.maxTrackedWorkingBytes = std::max(metrics.maxTrackedWorkingBytes, totalResult.Value());
}

[[nodiscard]] std::streamoff CheckedStreamOffset(const std::uint64_t value)
{
    Require(value <= static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()),
        "file offset cannot narrow to streamoff");
    return static_cast<std::streamoff>(value);
}

struct DescribedFile
{
    pbprotocol::SessionDescriptor sessionDescriptor{};
    std::vector<pbprotocol::SegmentDescriptor> segmentDescriptors;
    pbprotocol::FinalManifest finalManifest{};
};

struct OuterSendBlock
{
    std::uint32_t outerBlockId = 0;
    std::vector<std::byte> payload;
};

struct FrameOutcome
{
    std::optional<pbreceiver::ReceiverControlAdmission> controlAdmission;
    std::optional<pbreceiver::ReceiverCompletedSegment> completedSegment;
    std::vector<OuterSendBlock> validatedBlocks;
    std::uint64_t alreadyCompletedCount = 0;
    std::uint64_t repeatedReadyCount = 0;
};

[[nodiscard]] std::array<std::byte, pbprotocol::kDigestBytes> HashSpanStreaming(
    const std::span<const std::byte> bytes) noexcept
{
    pbprotocol::Blake3Hasher hasher;
    constexpr std::size_t chunkBytes = 64U * 1024U;
    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        const std::size_t byteCount = std::min<std::size_t>(
            chunkBytes,
            bytes.size() - offset);
        hasher.Update(bytes.subspan(offset, byteCount));
        offset += byteCount;
    }
    return hasher.Finalize();
}

[[nodiscard]] std::uint64_t StableNameSeed(const std::string_view name) noexcept
{
    std::uint64_t value = 1469598103934665603ULL;
    for (const char character : name)
    {
        value ^= static_cast<std::uint8_t>(character);
        value *= 1099511628211ULL;
    }
    return value;
}

[[nodiscard]] pbprotocol::SessionId MakeSessionId(
    const std::string_view caseName) noexcept
{
    SplitMix64 random(StableNameSeed(caseName));
    pbprotocol::SessionId sessionId{};
    for (std::size_t byteIndex = 0;
        byteIndex < sessionId.bytes.size();
        byteIndex += sizeof(std::uint64_t))
    {
        const std::uint64_t value = random.Next();
        for (std::size_t valueIndex = 0;
            valueIndex < sizeof(value) &&
                byteIndex + valueIndex < sessionId.bytes.size();
            valueIndex++)
        {
            sessionId.bytes[byteIndex + valueIndex] =
                static_cast<std::byte>(static_cast<std::uint8_t>(
                    value >> static_cast<unsigned int>(valueIndex * 8U)));
        }
    }
    return sessionId;
}

void FillRandomBytes(
    SplitMix64& random,
    const std::span<std::byte> output) noexcept
{
    std::size_t outputIndex = 0;
    while (outputIndex < output.size())
    {
        const std::uint64_t value = random.Next();
        for (std::size_t valueIndex = 0;
            valueIndex < sizeof(value) && outputIndex < output.size();
            valueIndex++, outputIndex++)
        {
            output[outputIndex] = static_cast<std::byte>(
                static_cast<std::uint8_t>(
                    value >> static_cast<unsigned int>(valueIndex * 8U)));
        }
    }
}

void CreateSourceFile(
    const std::filesystem::path& filePath,
    const FileCaseSpec& caseSpec)
{
    std::ofstream output(filePath, std::ios::binary | std::ios::trunc);
    Require(output.is_open(), "cannot create source file");
    constexpr std::size_t chunkBytes = 1024U * 1024U;
    std::vector<std::byte> chunk(chunkBytes);
    SplitMix64 random(caseSpec.seed);
    std::uint64_t remainingBytes = caseSpec.fileBytes;
    while (remainingBytes != 0)
    {
        const std::size_t bytesThisPass = static_cast<std::size_t>(
            std::min<std::uint64_t>(remainingBytes, chunk.size()));
        const std::span<std::byte> activeChunk =
            std::span<std::byte>(chunk).first(bytesThisPass);
        if (caseSpec.pattern == SourcePattern::CompressedCompletedResume &&
            caseSpec.fileBytes - remainingBytes < kSourceSegmentBytes)
        {
            std::fill(activeChunk.begin(), activeChunk.end(), std::byte{0x5A});
        }
        else if (caseSpec.pattern == SourcePattern::Compressible)
        {
            for (std::size_t byteIndex = 0;
                byteIndex < activeChunk.size();
                byteIndex++)
            {
                activeChunk[byteIndex] = static_cast<std::byte>(
                    static_cast<std::uint8_t>((byteIndex / 4096U) & 0x03U));
            }
        }
        else
        {
            FillRandomBytes(random, activeChunk);
        }
        output.write(
            reinterpret_cast<const char*>(activeChunk.data()),
            static_cast<std::streamsize>(activeChunk.size()));
        Require(output.good(), "source file write failed");
        remainingBytes -= activeChunk.size();
    }
    output.flush();
    output.close();
    Require(!output.fail(), "source file flush or close failed");
    Require(std::filesystem::file_size(filePath) == caseSpec.fileBytes,
        "source file length mismatch");
}

[[nodiscard]] std::vector<std::byte> ReadFileRange(
    const std::filesystem::path& filePath,
    const std::uint64_t offset,
    const std::size_t byteCount)
{
    Require(byteCount <= kSourceSegmentBytes, "range read exceeds one Source Segment");
    const std::uint64_t fileBytes = std::filesystem::file_size(filePath);
    Require(offset <= fileBytes && byteCount <= fileBytes - offset,
        "range read exceeds the actual file size");
    std::ifstream input(filePath, std::ios::binary);
    Require(input.is_open(), "cannot open source range");
    input.seekg(CheckedStreamOffset(offset), std::ios::beg);
    Require(input.good(), "source range seek failed");
    std::vector<std::byte> bytes(byteCount);
    if (!bytes.empty())
    {
        input.read(
            reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        Require(input.gcount() == static_cast<std::streamsize>(bytes.size()),
            "source range short read");
    }
    return bytes;
}

[[nodiscard]] std::array<std::byte, pbprotocol::kDigestBytes> HashFile(
    const std::filesystem::path& filePath)
{
    std::ifstream input(filePath, std::ios::binary);
    Require(input.is_open(), "cannot open file for sequential digest");
    pbprotocol::Blake3Hasher hasher;
    std::vector<std::byte> buffer(1024U * 1024U);
    while (input)
    {
        input.read(
            reinterpret_cast<char*>(buffer.data()),
            static_cast<std::streamsize>(buffer.size()));
        const std::streamsize readBytes = input.gcount();
        Require(readBytes >= 0, "negative file read count");
        hasher.Update(std::span<const std::byte>(buffer).first(
            static_cast<std::size_t>(readBytes)));
    }
    Require(input.eof(), "sequential digest read failed");
    return hasher.Finalize();
}

[[nodiscard]] bool FilesEqual(
    const std::filesystem::path& firstPath,
    const std::filesystem::path& secondPath)
{
    if (std::filesystem::file_size(firstPath) !=
        std::filesystem::file_size(secondPath))
    {
        return false;
    }
    std::ifstream first(firstPath, std::ios::binary);
    std::ifstream second(secondPath, std::ios::binary);
    if (!first.is_open() || !second.is_open())
    {
        return false;
    }
    std::vector<char> firstBuffer(1024U * 1024U);
    std::vector<char> secondBuffer(1024U * 1024U);
    while (first && second)
    {
        first.read(firstBuffer.data(),
            static_cast<std::streamsize>(firstBuffer.size()));
        second.read(secondBuffer.data(),
            static_cast<std::streamsize>(secondBuffer.size()));
        if (first.gcount() != second.gcount() ||
            !std::equal(
                firstBuffer.begin(),
                firstBuffer.begin() + first.gcount(),
                secondBuffer.begin()))
        {
            return false;
        }
    }
    return first.eof() && second.eof();
}

[[nodiscard]] std::vector<std::byte> WrapControlPayload(
    const pbprotocol::ControlRecordType recordType,
    const std::uint64_t controlSequence,
    const pbprotocol::SessionTag sessionTag,
    const std::span<const std::byte> payload)
{
    const pbprotocol::ControlRecordView record{
        pbprotocol::kControlVersion,
        recordType,
        controlSequence,
        sessionTag,
        payload};
    const auto sizeResult = pbprotocol::GetSerializedSize(record);
    RequireResult(sizeResult, "control record size failed");
    Require(sizeResult.Value() <= pbmodulation::kReferenceControlWindowBytes,
        "control record does not fit reference control window");
    std::vector<std::byte> bytes(sizeResult.Value());
    RequireResult(pbprotocol::SerializeControlRecord(record, bytes),
        "control record serialization failed");
    return bytes;
}

[[nodiscard]] std::vector<std::byte> MakeSessionControlRecord(
    const pbprotocol::SessionDescriptor& descriptor,
    const pbprotocol::ReceiverResourcePolicy& resourcePolicy,
    const std::uint64_t controlSequence)
{
    std::array<std::byte, pbprotocol::kSessionDescriptorPayloadBytes> payload{};
    RequireResult(pbprotocol::SerializeSessionDescriptor(
        descriptor,
        resourcePolicy,
        payload),
        "SessionDescriptor serialization failed");
    return WrapControlPayload(
        pbprotocol::ControlRecordType::SessionDescriptor,
        controlSequence,
        pbprotocol::DeriveSessionTag(descriptor.sessionId),
        payload);
}

[[nodiscard]] std::vector<std::byte> MakeSegmentControlRecord(
    const pbprotocol::SegmentDescriptor& descriptor,
    const pbprotocol::SessionDescriptor& sessionDescriptor,
    const pbprotocol::ReceiverResourcePolicy& resourcePolicy,
    const std::uint64_t controlSequence)
{
    const auto sizeResult = pbprotocol::GetSerializedSize(descriptor);
    RequireResult(sizeResult, "SegmentDescriptor size failed");
    std::vector<std::byte> payload(sizeResult.Value());
    RequireResult(pbprotocol::SerializeSegmentDescriptor(
        descriptor,
        sessionDescriptor,
        resourcePolicy,
        payload),
        "SegmentDescriptor serialization failed");
    return WrapControlPayload(
        pbprotocol::ControlRecordType::SegmentDescriptor,
        controlSequence,
        descriptor.sessionTag,
        payload);
}

[[nodiscard]] std::vector<std::byte> MakeManifestControlRecord(
    const pbprotocol::FinalManifest& finalManifest,
    const pbprotocol::SessionDescriptor& sessionDescriptor,
    const pbprotocol::ReceiverResourcePolicy& resourcePolicy,
    const std::uint64_t controlSequence)
{
    std::array<std::byte, pbprotocol::kFinalManifestPayloadBytes> payload{};
    RequireResult(pbprotocol::SerializeFinalManifest(
        finalManifest,
        sessionDescriptor,
        resourcePolicy,
        payload),
        "FinalManifest serialization failed");
    return WrapControlPayload(
        pbprotocol::ControlRecordType::FinalManifest,
        controlSequence,
        pbprotocol::DeriveSessionTag(finalManifest.sessionId),
        payload);
}

[[nodiscard]] pbprotocol::SegmentDescriptor DescribeSegment(
    const pbprotocol::SessionDescriptor& sessionDescriptor,
    const std::uint64_t segmentOrdinal,
    const std::uint64_t rawOffset,
    const std::span<const std::byte> rawBytes,
    const pbcompression::EncodedSegment& encodedSegment)
{
    const auto modeResult = pbouterfec::ChooseOuterFecMode(
        encodedSegment.bytes.size(),
        kOuterBlockBytes);
    RequireResult(modeResult, "outer FEC mode selection failed");

    pbprotocol::SegmentDescriptor descriptor{};
    descriptor.sessionTag = pbprotocol::DeriveSessionTag(
        sessionDescriptor.sessionId);
    descriptor.segmentOrdinal = segmentOrdinal;
    descriptor.rawOffset = rawOffset;
    descriptor.rawSize = rawBytes.size();
    descriptor.encodedSize = encodedSegment.bytes.size();
    descriptor.compressionCodec = encodedSegment.codec;
    descriptor.outerFecMode = modeResult.Value();
    descriptor.outerBlockBytes = kOuterBlockBytes;
    descriptor.rawDigest = pbprotocol::RawDigest{
        HashSpanStreaming(rawBytes)};
    descriptor.encodedDigest = pbprotocol::EncodedDigest{
        pbprotocol::ComputeBlake3Digest(encodedSegment.bytes)};

    if (descriptor.outerFecMode == pbprotocol::OuterFecMode::WirehairV2)
    {
        auto encoderResult = pbouterfec::WirehairV2Encoder::Create(
            encodedSegment.bytes,
            kOuterBlockBytes);
        RequireResult(encoderResult, "Wirehair descriptor encoder failed");
        descriptor.wirehairV2SerializedProfile =
            encoderResult.Value().GetSerializedProfile();
        const auto recreateResult = pbouterfec::WirehairV2Encoder::Recreate(
            encodedSegment.bytes,
            descriptor);
        RequireResult(recreateResult, "Wirehair canonical recreate failed");
        Require(recreateResult.Value().GetSerializedProfile() ==
            *descriptor.wirehairV2SerializedProfile,
            "Wirehair profile drifted during recreate");
    }
    else
    {
        const auto encoderResult = pbouterfec::DirectRepeatEncoder::Create(
            encodedSegment.bytes,
            kOuterBlockBytes);
        RequireResult(encoderResult, "DirectRepeat descriptor encoder failed");
        const auto recreateResult = pbouterfec::DirectRepeatEncoder::Recreate(
            encodedSegment.bytes,
            descriptor);
        RequireResult(recreateResult, "DirectRepeat canonical recreate failed");
    }
    return descriptor;
}

[[nodiscard]] DescribedFile DescribeFile(
    const std::filesystem::path& sourcePath,
    const FileCaseSpec& caseSpec,
    GateMetrics& metrics)
{
    const std::uint64_t segmentCount = caseSpec.fileBytes == 0
        ? 0
        : 1ULL + ((caseSpec.fileBytes - 1ULL) / kSourceSegmentBytes);
    DescribedFile describedFile;
    describedFile.sessionDescriptor = pbprotocol::SessionDescriptor{
        pbprotocol::GetProtocolVersion(),
        MakeSessionId(caseSpec.name),
        caseSpec.fileBytes,
        segmentCount,
        pbprotocol::DigestAlgorithm::Blake3_256};
    describedFile.segmentDescriptors.reserve(
        static_cast<std::size_t>(segmentCount));

    pbprotocol::Blake3Hasher wholeFileHasher;
    pbcompression::CompressionSettings compressionSettings;
    compressionSettings.maxOutputBytes = 16ULL * kMebibyte;
    for (std::uint64_t segmentOrdinal = 0;
        segmentOrdinal < segmentCount;
        segmentOrdinal++)
    {
        const auto offsetResult = pbprotocol::CheckedMultiplyUint64(segmentOrdinal, kSourceSegmentBytes);
        RequireResult(offsetResult, "Source Segment offset overflow");
        const std::uint64_t rawOffset = offsetResult.Value();
        const std::size_t rawSize = static_cast<std::size_t>(
            std::min<std::uint64_t>(
                kSourceSegmentBytes,
                caseSpec.fileBytes - rawOffset));
        const std::vector<std::byte> rawBytes = ReadFileRange(
            sourcePath,
            rawOffset,
            rawSize);
        wholeFileHasher.Update(rawBytes);
        const auto compressionResult = pbcompression::CompressSegment(
            rawBytes,
            compressionSettings);
        RequireResult(compressionResult, "segment compression failed");
        describedFile.segmentDescriptors.push_back(DescribeSegment(
            describedFile.sessionDescriptor,
            segmentOrdinal,
            rawOffset,
            rawBytes,
            compressionResult.Value()));
        metrics.maxActiveSegmentCount = 1;
        metrics.maxSenderActiveSegmentCount = 1;
        metrics.maxTrackedWorkingBytes = std::max<std::uint64_t>(
            metrics.maxTrackedWorkingBytes,
            rawBytes.size() + compressionResult.Value().bytes.size());
    }

    describedFile.finalManifest = pbprotocol::FinalManifest{
        describedFile.sessionDescriptor.sessionId,
        caseSpec.fileBytes,
        segmentCount,
        pbprotocol::WholeFileDigest{wholeFileHasher.Finalize()},
        pbprotocol::DigestAlgorithm::Blake3_256};
    metrics.segmentCount = segmentCount;
    return describedFile;
}

class RasterTransportPipeline
{
public:
    RasterTransportPipeline(
        pbreceiver::ReceiverIngress& receiver,
        const pbprotocol::SessionTag sessionTag,
        GateMetrics& metrics)
        : receiver_(receiver),
          sessionTag_(sessionTag),
          metrics_(metrics),
          frameBgra_(pbmodulation::kReferenceFrameBgraBytes),
          dataRegion_(pbmodulation::kReferenceDataRegionBytes),
          sentCodeword_(kRobustCodewordBytes)
    {
        metrics_.pipelineWorkingBytes = GetPersistentWorkingBytes();
        TrackCallerWorkingBytes(metrics_, 0);
    }

    [[nodiscard]] FrameOutcome ProcessFrame(
        const std::span<const std::byte> controlRecord,
        const std::span<const OuterSendBlock> blocks,
        const bool admitData = true,
        const bool captureValidatedData = false,
        const bool repeatPreviousSequence = false)
    {
        if (repeatPreviousSequence)
        {
            Require(frameSequence_ > 0, "no prior frame sequence to repeat");
            frameSequence_--;
        }
        Require(controlRecord.size() <=
            pbmodulation::kReferenceControlWindowBytes,
            "control record exceeds reference window");
        Require(blocks.size() <= kCodewordsPerFrame,
            "too many LDPC codewords in one reference frame");

        std::fill(controlWindow_.begin(), controlWindow_.end(), std::byte{0});
        std::copy(
            controlRecord.begin(),
            controlRecord.end(),
            controlWindow_.begin());
        std::fill(dataRegion_.begin(), dataRegion_.end(), std::byte{0});
        SerializeBootstrap();

        for (std::size_t blockIndex = 0;
            blockIndex < blocks.size();
            blockIndex++)
        {
            const OuterSendBlock& block = blocks[blockIndex];
            Require(block.payload.size() <= kOuterBlockBytes,
                "outer payload exceeds frozen block bytes");
            Require(block.payload.size() <=
                std::numeric_limits<std::uint16_t>::max(),
                "outer payload cannot narrow to Transport u16");
            const pbprotocol::TransportBlockHeader header{
                pbprotocol::kTransportBlockTypeData,
                pbprotocol::kTransportProtocolMinor,
                0,
                sessionTag_,
                activeSegmentOrdinal_,
                block.outerBlockId,
                static_cast<std::uint16_t>(block.payload.size())};
            std::vector<std::byte> transportBytes(
                pbprotocol::GetTransportSerializedSize(header));
            RequireResult(pbprotocol::SerializeTransportBlock(
                header,
                block.payload,
                transportBytes),
                "Transport serialization failed");
            std::array<std::byte, kRobustInfoBytes> infoBlock{};
            RequireResult(pbprotocol::FrameTransportBlockIntoInfoBlock(
                transportBytes,
                infoBlock.size(),
                infoBlock),
                "Transport info-block framing failed");
            RequireResult(pbinnerfec::EncodeQcLdpcCodeword(
                pbinnerfec::kInnerFecProfileIdRobust,
                infoBlock,
                sentCodeword_),
                "Robust LDPC encoding failed");
            const std::size_t codewordOffset =
                blockIndex * kRobustCodewordBytes;
            std::copy(
                sentCodeword_.begin(),
                sentCodeword_.end(),
                dataRegion_.begin() +
                    static_cast<std::ptrdiff_t>(codewordOffset));
        }

        const pbmodulation::ReferenceFrameInput frameInput{
            bootstrapRecord_,
            controlWindow_,
            dataRegion_};
        RequireResult(pbmodulation::EncodeReferenceFrame(
            frameInput,
            frameBgra_),
            "reference raster encode failed");
        auto outcome = ReceiveFrame(frameBgra_, admitData, captureValidatedData);
        frameSequence_++;
        return outcome;
    }

private:
    // Receiver entrypoint: no sender length, block count, ordinal or ID list.
    // Decode validates the entire frame before any Receiver admission occurs.
    [[nodiscard]] FrameOutcome ReceiveFrame(const std::span<const std::byte> raster,
        const bool admitData, const bool captureValidatedData)
    {
        const auto decodedFrame = frameDecoder_.Decode(raster);
        metrics_.frameCount++;
        FrameOutcome outcome;
        if (decodedFrame.controlBytes != 0)
        {
            auto controlResult = receiver_.ReceiveControlRecord(
                std::span<const std::byte>(decodedFrame.control).first(decodedFrame.controlBytes));
            RequireResult(controlResult, "Receiver rejected demodulated Control");
            outcome.controlAdmission = std::move(controlResult).Value();
            if (outcome.controlAdmission->completedSegment)
            {
                outcome.completedSegment = std::move(*outcome.controlAdmission->completedSegment);
            }
        }
        for (std::size_t blockIndex = 0; blockIndex < decodedFrame.blockCount; blockIndex++)
        {
            const auto& transport = decodedFrame.blocks[blockIndex];
            metrics_.ldpcCodewordCount++;
            if (captureValidatedData)
            {
                const auto payload = std::span<const std::byte>(transport.paddedPayload).first(transport.header.payloadBytes);
                outcome.validatedBlocks.push_back(OuterSendBlock{
                    transport.header.outerBlockId,
                    std::vector<std::byte>(payload.begin(), payload.end())});
            }
            metrics_.transportBlockCount++;
            if (!admitData)
            {
                continue;
            }

            const pbreceiver::ReceivedTransportBlock receiverBlock{
                transport.header.sessionTag,
                transport.header.segmentOrdinal,
                transport.header.outerBlockId,
                transport.header.payloadBytes,
                transport.paddedPayload};
            auto dataResult = receiver_.ReceiveDataBlock(receiverBlock);
            RequireResult(dataResult, "Receiver rejected decoded Data block");
            const pbreceiver::ReceiverResourceTelemetrySnapshot telemetry = receiver_.GetTelemetry();
            // Ready decoders remain reserved until verified storage commits;
            // the returned encoded bytes do not represent another Segment.
            const std::uint64_t activeCount = telemetry.activeOuterFecDecoderCount;
            metrics_.maxActiveOuterDecoderCount = std::max(metrics_.maxActiveOuterDecoderCount, activeCount);
            metrics_.maxActiveSegmentCount = std::max(metrics_.maxActiveSegmentCount, activeCount);
            metrics_.maxReservedOuterDecoderBytes = std::max(
                metrics_.maxReservedOuterDecoderBytes, telemetry.reservedOuterFecDecoderBytes);
            if (dataResult.Value().disposition ==
                pbreceiver::ReceiverDataDisposition::AlreadyCompleted)
            {
                outcome.alreadyCompletedCount++;
            }
            if (dataResult.Value().completedSegment)
            {
                auto& recovered = *dataResult.Value().completedSegment;
                const auto retainedBytes = pbprotocol::CheckedAddUint64(recovered.encodedBytes.size(),
                    outcome.completedSegment ? outcome.completedSegment->encodedBytes.size() : 0U);
                RequireResult(retainedBytes, "recovered frame working byte count overflow");
                TrackCallerWorkingBytes(metrics_, retainedBytes.Value());
                if (outcome.completedSegment)
                {
                    // A duplicate after Ready can return the same verified
                    // encoded recovery again before the caller stores it.
                    // Deduplicate only exact bindings and bytes, never latest-wins.
                    Require(outcome.completedSegment->boundSegmentDescriptor == recovered.boundSegmentDescriptor &&
                        outcome.completedSegment->encodedBytes == recovered.encodedBytes,
                        "one frame produced distinct recovered Segments");
                    outcome.repeatedReadyCount++;
                }
                else
                {
                    outcome.completedSegment = std::move(recovered);
                }
            }
        }

        const pbreceiver::ReceiverResourceTelemetrySnapshot telemetry =
            receiver_.GetTelemetry();
        metrics_.maxActiveOuterDecoderCount = std::max<std::uint64_t>(
            metrics_.maxActiveOuterDecoderCount,
            telemetry.activeOuterFecDecoderCount);
        return outcome;
    }

public:
    void SetActiveSegmentOrdinal(const std::uint64_t segmentOrdinal) noexcept
    {
        activeSegmentOrdinal_ = segmentOrdinal;
    }

    [[nodiscard]] std::array<std::byte, pbprotocol::kDigestBytes> GetFrameDigest() const noexcept
    {
        return HashSpanStreaming(frameBgra_);
    }

    [[nodiscard]] std::uint64_t GetPersistentWorkingBytes() const noexcept
    {
        return frameBgra_.size() + dataRegion_.size() + sentCodeword_.size() +
            controlWindow_.size() + bootstrapRecord_.size() + frameDecoder_.GetWorkingBytes();
    }

private:
    void SerializeBootstrap()
    {
        const pbprotocol::BootstrapRecord bootstrap{
            pbprotocol::kBootstrapVersion,
            pbprotocol::GetProtocolVersion(),
            1,
            kReferenceVisualProfileId,
            sessionTag_,
            frameSequence_,
            0,
            0};
        RequireResult(pbprotocol::SerializeBootstrapRecord(
            bootstrap,
            bootstrapRecord_),
            "Bootstrap serialization failed");
    }

    pbreceiver::ReceiverIngress& receiver_;
    pbprotocol::SessionTag sessionTag_{};
    GateMetrics& metrics_;
    phase0gate::ReferenceFrameDecoder frameDecoder_;
    std::array<std::byte, pbmodulation::kReferenceBootstrapRecordBytes>
        bootstrapRecord_{};
    std::array<std::byte, pbmodulation::kReferenceControlWindowBytes>
        controlWindow_{};
    std::vector<std::byte> frameBgra_;
    std::vector<std::byte> dataRegion_;
    std::vector<std::byte> sentCodeword_;
    std::uint64_t activeSegmentOrdinal_ = 0;
    std::uint64_t frameSequence_ = 0;
};

void PreparePartFile(
    const std::filesystem::path& partPath,
    const std::uint64_t fileBytes)
{
    std::ofstream output(partPath, std::ios::binary | std::ios::trunc);
    Require(output.is_open(), "cannot create output.part");
    if (fileBytes != 0)
    {
        output.seekp(CheckedStreamOffset(fileBytes - 1ULL));
        Require(output.good(), "output.part preallocation seek failed");
        output.put('\0');
    }
    output.flush();
    output.close();
    Require(!output.fail(), "output.part preallocation flush failed");
    Require(std::filesystem::file_size(partPath) == fileBytes,
        "output.part preallocation length mismatch");
}

void StoreAndCommitSegment(
    pbreceiver::ReceiverIngress& receiver,
    pbreceiver::ReceiverCompletedSegment&& completedSegment,
    const std::filesystem::path& partPath,
    GateMetrics& metrics,
    std::ostream* faultStream = nullptr)
{
    auto verifiedResult = receiver.VerifyRecoveredSegment(
        std::move(completedSegment));
    RequireResult(verifiedResult,
        "encoded digest, decompression, or raw digest verification failed");
    metrics.recoveredVerificationCount++;
    pbreceiver::ReceiverVerifiedSegment verifiedSegment =
        std::move(verifiedResult).Value();
    const pbprotocol::SegmentDescriptor& descriptor =
        verifiedSegment.GetBoundSegmentDescriptor().GetDescriptor();
    Require(verifiedSegment.GetRawBytes().size() == descriptor.rawSize,
        "verified raw byte count differs from descriptor");
    const auto writeEndResult = pbprotocol::CheckedAddUint64(descriptor.rawOffset, descriptor.rawSize);
    RequireResult(writeEndResult, "output.part positional range overflow");
    Require(writeEndResult.Value() <= std::filesystem::file_size(partPath),
        "verified Segment exceeds output.part reservation");
    TrackCallerWorkingBytes(metrics, completedSegment.encodedBytes.size() + verifiedSegment.GetRawBytes().size());

    std::fstream partFile(
        partPath,
        std::ios::binary | std::ios::in | std::ios::out);
    Require(partFile.is_open(), "cannot open output.part for positional write");
    std::ostream& output = faultStream == nullptr ? partFile : *faultStream;
    output.seekp(CheckedStreamOffset(descriptor.rawOffset));
    Require(output.good(), "output.part positional seek failed");
    const std::span<const std::byte> rawBytes = verifiedSegment.GetRawBytes();
    output.write(
        reinterpret_cast<const char*>(rawBytes.data()),
        static_cast<std::streamsize>(rawBytes.size()));
    output.flush();
    Require(output.good(), "output.part write or flush failed");
    partFile.close();
    Require(!partFile.fail(), "output.part close failed");

    const auto commitResult = receiver.CommitStoredSegment(
        std::move(verifiedSegment));
    RequireResult(commitResult, "stored Segment commit failed");
    Require(commitResult.Value() ==
        pbreceiver::ReceiverSegmentCommitDisposition::Committed,
        "first stored Segment commit was not Committed");
    metrics.storedCommitCount++;
}

template <typename EncoderType>
[[nodiscard]] OuterSendBlock EncodeOuterBlock(
    EncoderType& encoder,
    const std::uint32_t outerBlockId)
{
    std::array<std::byte, kOuterBlockBytes> paddedPayload{};
    const auto encodeResult = encoder.EncodeBlock(
        outerBlockId,
        paddedPayload);
    RequireResult(encodeResult, "outer block encoding failed");
    OuterSendBlock block;
    block.outerBlockId = outerBlockId;
    block.payload.assign(
        paddedPayload.begin(),
        paddedPayload.begin() +
            static_cast<std::ptrdiff_t>(encodeResult.Value()));
    return block;
}

template <typename EncoderType>
void TransferWithEncoder(
    EncoderType& encoder,
    const pbprotocol::SegmentDescriptor& descriptor,
    const std::vector<std::byte>& segmentControlRecord,
    RasterTransportPipeline& pipeline,
    pbreceiver::ReceiverIngress& receiver,
    const std::filesystem::path& partPath,
    GateMetrics& metrics)
{
    const std::uint64_t blockCountWide = encoder.GetBlockCount();
    Require(blockCountWide > 0 &&
        blockCountWide <= std::numeric_limits<std::uint32_t>::max(),
        "outer block count is not representable");
    const std::uint32_t blockCount = static_cast<std::uint32_t>(blockCountWide);
    std::vector<std::uint32_t> scheduledBlockIds;
    scheduledBlockIds.reserve(static_cast<std::size_t>(blockCount) + 128U);

    // Deliberately drop systematic block zero and reverse every other block.
    // A byte-identical duplicate is injected before the missing block/repair.
    for (std::uint32_t blockId = blockCount; blockId > 1U; blockId--)
    {
        scheduledBlockIds.push_back(blockId - 1U);
    }
    metrics.droppedSystematicBlockCount++;
    if (!scheduledBlockIds.empty())
    {
        scheduledBlockIds.insert(
            scheduledBlockIds.begin() + 1,
            scheduledBlockIds.front());
        metrics.duplicateBlockCount++;
    }
    if (descriptor.outerFecMode == pbprotocol::OuterFecMode::DirectRepeat)
    {
        scheduledBlockIds.push_back(0);
    }

    bool committed = false;
    std::size_t scheduleIndex = 0;
    std::uint32_t nextRepairId = blockCount;
    while (!committed)
    {
        if (scheduleIndex == scheduledBlockIds.size())
        {
            Require(descriptor.outerFecMode ==
                pbprotocol::OuterFecMode::WirehairV2,
                "DirectRepeat did not recover after its next repetition");
            Require(nextRepairId < blockCount + 2048U,
                "Wirehair repair budget exhausted without recovery");
            scheduledBlockIds.push_back(nextRepairId);
            nextRepairId++;
        }

        const std::size_t batchCount = std::min<std::size_t>(
            kCodewordsPerFrame,
            scheduledBlockIds.size() - scheduleIndex);
        std::vector<OuterSendBlock> blocks;
        blocks.reserve(batchCount);
        for (std::size_t batchIndex = 0;
            batchIndex < batchCount;
            batchIndex++)
        {
            const std::uint32_t blockId =
                scheduledBlockIds[scheduleIndex + batchIndex];
            blocks.push_back(EncodeOuterBlock(encoder, blockId));
            if (blockId >= blockCount)
            {
                metrics.repairBlockCount++;
            }
        }
        scheduleIndex += batchCount;
        FrameOutcome outcome = pipeline.ProcessFrame(
            segmentControlRecord,
            blocks);
        if (outcome.completedSegment)
        {
            StoreAndCommitSegment(
                receiver,
                std::move(*outcome.completedSegment),
                partPath,
                metrics);
            committed = true;
        }
    }

    // Once storage is committed, the same valid block must be an idempotent
    // AlreadyCompleted result rather than recreating a decoder.
    const std::uint32_t repeatedBlockId = blockCount > 1U ? blockCount - 1U : 0;
    const std::array<OuterSendBlock, 1> repeatedBlock{
        EncodeOuterBlock(encoder, repeatedBlockId)};
    const FrameOutcome repeatedOutcome = pipeline.ProcessFrame(
        segmentControlRecord,
        repeatedBlock);
    Require(repeatedOutcome.alreadyCompletedCount == 1,
        "post-commit duplicate was not idempotent");
    metrics.duplicateBlockCount++;
    const auto priorFrameDigest = pipeline.GetFrameDigest();
    const FrameOutcome repeatedFrameOutcome = pipeline.ProcessFrame(
        segmentControlRecord, repeatedBlock, true, false, true);
    Require(repeatedFrameOutcome.alreadyCompletedCount == 1 &&
        pipeline.GetFrameDigest() == priorFrameDigest,
        "byte-identical reference frame replay was not idempotent");
    metrics.duplicateFrameCount++;
    metrics.duplicateBlockCount++;
}

void TransferSegment(
    const std::filesystem::path& sourcePath,
    const pbprotocol::SessionDescriptor& sessionDescriptor,
    const pbprotocol::SegmentDescriptor& descriptor,
    const std::uint64_t controlSequence,
    const pbprotocol::ReceiverResourcePolicy& resourcePolicy,
    RasterTransportPipeline& pipeline,
    pbreceiver::ReceiverIngress& receiver,
    const std::filesystem::path& partPath,
    GateMetrics& metrics)
{
    const std::vector<std::byte> rawBytes = ReadFileRange(
        sourcePath,
        descriptor.rawOffset,
        static_cast<std::size_t>(descriptor.rawSize));
    pbcompression::CompressionSettings compressionSettings;
    compressionSettings.maxOutputBytes = 16ULL * kMebibyte;
    const auto compressionResult = pbcompression::CompressSegment(
        rawBytes,
        compressionSettings);
    RequireResult(compressionResult,
        "second-pass segment compression failed");
    const pbprotocol::SegmentDescriptor recreatedDescriptor = DescribeSegment(
        sessionDescriptor,
        descriptor.segmentOrdinal,
        descriptor.rawOffset,
        rawBytes,
        compressionResult.Value());
    Require(recreatedDescriptor == descriptor,
        "second-pass descriptor or encoded bytes drifted");
    const std::vector<std::byte> segmentControlRecord = MakeSegmentControlRecord(
        descriptor,
        sessionDescriptor,
        resourcePolicy,
        controlSequence);
    pipeline.SetActiveSegmentOrdinal(descriptor.segmentOrdinal);
    Require(metrics.currentSenderWorkingBytes == 0, "sender retained a second active Segment");
    metrics.currentSenderWorkingBytes = rawBytes.size() + compressionResult.Value().bytes.size();
    metrics.maxSenderActiveSegmentCount = 1;
    TrackCallerWorkingBytes(metrics, 0);

    if (descriptor.outerFecMode == pbprotocol::OuterFecMode::DirectRepeat)
    {
        auto encoderResult = pbouterfec::DirectRepeatEncoder::Recreate(
            compressionResult.Value().bytes,
            descriptor);
        RequireResult(encoderResult,
            "second-pass DirectRepeat recreate failed");
        pbouterfec::DirectRepeatEncoder encoder =
            std::move(encoderResult).Value();
        TransferWithEncoder(
            encoder,
            descriptor,
            segmentControlRecord,
            pipeline,
            receiver,
            partPath,
            metrics);
        metrics.directRepeatSegmentCount++;
    }
    else
    {
        auto encoderResult = pbouterfec::WirehairV2Encoder::Recreate(
            compressionResult.Value().bytes,
            descriptor);
        RequireResult(encoderResult,
            "second-pass Wirehair recreate failed");
        pbouterfec::WirehairV2Encoder encoder =
            std::move(encoderResult).Value();
        TransferWithEncoder(
            encoder,
            descriptor,
            segmentControlRecord,
            pipeline,
            receiver,
            partPath,
            metrics);
        metrics.wirehairSegmentCount++;
    }
    metrics.currentSenderWorkingBytes = 0;
}

[[nodiscard]] bool IsPublishable(
    const std::filesystem::path& partPath,
    const std::filesystem::path& finalPath,
    const pbprotocol::FinalManifest& finalManifest)
{
    if (std::filesystem::exists(finalPath) ||
        !std::filesystem::is_regular_file(partPath) ||
        std::filesystem::file_size(partPath) != finalManifest.originalFileSize)
    {
        return false;
    }
    return HashFile(partPath) == finalManifest.wholeFileDigest.bytes;
}

void VerifyAndPublish(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& partPath,
    const std::filesystem::path& finalPath,
    const pbprotocol::FinalManifest& finalManifest)
{
    Require(IsPublishable(partPath, finalPath, finalManifest),
        "output.part failed final sequential WholeFileDigest gate");
    std::filesystem::rename(partPath, finalPath);
    Require(std::filesystem::is_regular_file(finalPath),
        "same-directory final rename did not publish a regular file");
    Require(HashFile(finalPath) == finalManifest.wholeFileDigest.bytes,
        "published final file digest changed after rename");
    Require(FilesEqual(sourcePath, finalPath),
        "published final file is not byte-identical to source");
}

[[nodiscard]] std::string MetricsJson(
    const FileCaseSpec& caseSpec,
    const GateMetrics& metrics,
    const pbprotocol::FinalManifest& finalManifest,
    const std::uint64_t elapsedMilliseconds)
{
    std::ostringstream stream;
    stream << "{\"case\":\"" << EscapeJson(caseSpec.name)
           << "\",\"status\":\"pass\",\"file_bytes\":"
           << caseSpec.fileBytes
           << ",\"segment_count\":" << metrics.segmentCount
           << ",\"direct_repeat_segments\":"
           << metrics.directRepeatSegmentCount
           << ",\"wirehair_segments\":" << metrics.wirehairSegmentCount
           << ",\"frames\":" << metrics.frameCount
           << ",\"ldpc_codewords\":" << metrics.ldpcCodewordCount
           << ",\"transport_blocks\":" << metrics.transportBlockCount
           << ",\"duplicates\":" << metrics.duplicateBlockCount
           << ",\"duplicate_frames\":" << metrics.duplicateFrameCount
           << ",\"dropped_systematic\":"
           << metrics.droppedSystematicBlockCount
           << ",\"repair_blocks\":" << metrics.repairBlockCount
           << ",\"stored_commits\":" << metrics.storedCommitCount
           << ",\"max_active_segments\":"
           << metrics.maxActiveSegmentCount
           << ",\"max_active_outer_decoders\":"
           << metrics.maxActiveOuterDecoderCount
           << ",\"max_sender_active_segments\":" << metrics.maxSenderActiveSegmentCount
           << ",\"max_reserved_outer_decoder_bytes\":" << metrics.maxReservedOuterDecoderBytes
           << ",\"max_tracked_working_bytes\":"
           << metrics.maxTrackedWorkingBytes
           << ",\"tracked_bytes_scope\":\"major_caller_buffers_not_rss\""
           << ",\"source_seed\":" << caseSpec.seed
           << ",\"encoded_digest_checks\":" << metrics.recoveredVerificationCount
           << ",\"raw_digest_checks\":" << metrics.recoveredVerificationCount + metrics.resumedStoredVerificationCount
           << ",\"digest_check_scope\":\"successful_receiver_capability_verifications\""
           << ",\"resumed_raw_verifications\":" << metrics.resumedStoredVerificationCount
           << ",\"restored_commits\":" << metrics.restoredCommitCount
           << ",\"final_sequential_digest\":true,\"final_byte_identical\":true"
           << ",\"whole_file_digest\":\""
           << DigestHex(finalManifest.wholeFileDigest.bytes)
           << "\",\"elapsed_ms\":" << elapsedMilliseconds << '}';
    return stream.str();
}

void WriteSegmentEvidence(const FileCaseSpec& caseSpec, const DescribedFile& describedFile, EvidenceWriter& evidence)
{
    for (const pbprotocol::SegmentDescriptor& descriptor : describedFile.segmentDescriptors)
    {
        std::ostringstream stream;
        stream << "{\"case\":\"" << EscapeJson(caseSpec.name) << "\",\"event\":\"verified-segment\",\"status\":\"pass\""
               << ",\"segment_ordinal\":" << descriptor.segmentOrdinal
               << ",\"raw_offset\":" << descriptor.rawOffset
               << ",\"raw_size\":" << descriptor.rawSize
               << ",\"encoded_size\":" << descriptor.encodedSize
               << ",\"codec\":\"" << (descriptor.compressionCodec == pbprotocol::CompressionCodec::Raw ? "RAW" : "Zstandard")
               << "\",\"outer_fec\":\"" << (descriptor.outerFecMode == pbprotocol::OuterFecMode::DirectRepeat ? "DirectRepeat" : "WirehairV2")
               << "\",\"raw_digest\":\"" << DigestHex(descriptor.rawDigest.bytes)
               << "\",\"encoded_digest\":\"" << DigestHex(descriptor.encodedDigest.bytes) << '"';
        if (descriptor.wirehairV2SerializedProfile)
        {
            stream << ",\"canonical_profile_digest\":\""
                   << DigestHex(HashSpanStreaming(descriptor.wirehairV2SerializedProfile->bytes)) << '"';
        }
        stream << '}';
        evidence.Write(stream.str());
    }
}

void RunFileCase(
    const FileCaseSpec& caseSpec,
    const std::filesystem::path& scratchRoot,
    EvidenceWriter& evidence)
{
    const auto started = std::chrono::steady_clock::now();
    const std::filesystem::path caseRoot = scratchRoot / caseSpec.name;
    CreateFreshCaseDirectory(caseRoot);
    const std::filesystem::path sourcePath = caseRoot / "source.bin";
    const std::filesystem::path partPath = caseRoot / "output.part";
    const std::filesystem::path finalPath = caseRoot / "output.bin";

    CreateSourceFile(sourcePath, caseSpec);
    GateMetrics metrics;
    const DescribedFile describedFile = DescribeFile(
        sourcePath,
        caseSpec,
        metrics);
    const std::uint64_t expectedSegmentCount = caseSpec.fileBytes == 0
        ? 0
        : 1ULL + ((caseSpec.fileBytes - 1ULL) / kSourceSegmentBytes);
    Require(describedFile.segmentDescriptors.size() == expectedSegmentCount,
        "Source Segment count mismatch");
    if (!describedFile.segmentDescriptors.empty())
    {
        if (caseSpec.expectedFirstCodec)
        {
            Require(describedFile.segmentDescriptors.front().compressionCodec ==
                *caseSpec.expectedFirstCodec,
                "unexpected first Segment compression codec");
        }
        if (caseSpec.expectedFirstOuterMode)
        {
            Require(describedFile.segmentDescriptors.front().outerFecMode ==
                *caseSpec.expectedFirstOuterMode,
                "unexpected first Segment outer FEC mode");
        }
    }

    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    auto receiverResult = pbreceiver::ReceiverIngress::Create(
        resourcePolicy,
        kOuterBlockBytes);
    RequireResult(receiverResult, "ReceiverIngress creation failed");
    pbreceiver::ReceiverIngress receiver = std::move(receiverResult).Value();
    const pbprotocol::SessionTag sessionTag = pbprotocol::DeriveSessionTag(
        describedFile.sessionDescriptor.sessionId);
    RasterTransportPipeline pipeline(receiver, sessionTag, metrics);
    metrics.maxTrackedWorkingBytes = std::max<std::uint64_t>(
        metrics.maxTrackedWorkingBytes,
        pipeline.GetPersistentWorkingBytes());

    const std::vector<std::byte> sessionControl = MakeSessionControlRecord(
        describedFile.sessionDescriptor,
        resourcePolicy,
        1);
    const FrameOutcome sessionOutcome = pipeline.ProcessFrame(
        sessionControl,
        {});
    Require(sessionOutcome.controlAdmission &&
        sessionOutcome.controlAdmission->outputReservationDecision ==
            pbprotocol::OutputReservationDecision::AutoAccept,
        "Session output reservation was not automatically accepted");
    Require(!std::filesystem::exists(partPath),
        "output.part existed before output reservation admission");
    PreparePartFile(partPath, caseSpec.fileBytes);

    const std::vector<std::byte> manifestControl = MakeManifestControlRecord(
        describedFile.finalManifest,
        describedFile.sessionDescriptor,
        resourcePolicy,
        2);
    static_cast<void>(pipeline.ProcessFrame(manifestControl, {}));

    const auto preTransferFinalization = receiver.PrepareFinalization(
        sessionTag);
    if (expectedSegmentCount == 0)
    {
        RequireResult(preTransferFinalization,
            "zero-byte Session was not ready for finalization");
    }
    else
    {
        Require(!preTransferFinalization,
            "Session finalized before Segment descriptors/storage commits");
    }

    // Deterministic Segment-level disorder: transfer the tail first.
    for (std::size_t reverseIndex =
            describedFile.segmentDescriptors.size();
        reverseIndex > 0;
        reverseIndex--)
    {
        const pbprotocol::SegmentDescriptor& descriptor =
            describedFile.segmentDescriptors[reverseIndex - 1U];
        TransferSegment(
            sourcePath,
            describedFile.sessionDescriptor,
            descriptor,
            100ULL + descriptor.segmentOrdinal,
            resourcePolicy,
            pipeline,
            receiver,
            partPath,
            metrics);
    }

    const auto finalizationResult = receiver.PrepareFinalization(sessionTag);
    RequireResult(finalizationResult,
        "Receiver authoritative finalization was not ready");
    Require(finalizationResult.Value() == describedFile.finalManifest,
        "Receiver returned a non-authoritative FinalManifest");
    Require(receiver.GetTelemetry().activeOuterFecDecoderCount == 0,
        "stored Segment commit left an active outer decoder");
    Require(metrics.storedCommitCount == expectedSegmentCount &&
        metrics.maxSenderActiveSegmentCount <= 1 && metrics.maxActiveOuterDecoderCount <= 1,
        "file Gate violated one-active-Segment or exact commit-count contract");
    VerifyAndPublish(
        sourcePath,
        partPath,
        finalPath,
        finalizationResult.Value());

    const auto finished = std::chrono::steady_clock::now();
    const std::uint64_t elapsedMilliseconds =
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                finished - started).count());
    evidence.Write(MetricsJson(
        caseSpec,
        metrics,
        describedFile.finalManifest,
        elapsedMilliseconds));
    WriteSegmentEvidence(caseSpec, describedFile, evidence);
    std::filesystem::remove_all(caseRoot);
}

[[nodiscard]] bool HasProtocolError(
    const pbreceiver::ReceiverError& error,
    const pbprotocol::ProtocolErrorCode expectedCode)
{
    const auto* protocolError = std::get_if<pbprotocol::ProtocolError>(&error);
    return protocolError != nullptr && protocolError->code == expectedCode;
}

void RunControlNegativeChecks(EvidenceWriter& evidence)
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    const pbprotocol::SessionDescriptor sessionDescriptor{
        pbprotocol::GetProtocolVersion(),
        MakeSessionId("negative-control"),
        9,
        1,
        pbprotocol::DigestAlgorithm::Blake3_256};
    const std::vector<std::byte> validRecord = MakeSessionControlRecord(
        sessionDescriptor,
        resourcePolicy,
        1);

    std::uint64_t rejectedCount = 0;
    for (std::size_t mutationIndex = 0; mutationIndex < 4; mutationIndex++)
    {
        auto receiverResult = pbreceiver::ReceiverIngress::Create(
            resourcePolicy,
            kOuterBlockBytes);
        RequireResult(receiverResult, "negative receiver creation failed");
        pbreceiver::ReceiverIngress receiver =
            std::move(receiverResult).Value();
        std::vector<std::byte> malformed = validRecord;
        if (mutationIndex == 0)
        {
            malformed.back() ^= std::byte{0x01};
        }
        else if (mutationIndex == 1)
        {
            malformed.pop_back();
        }
        else if (mutationIndex == 2)
        {
            malformed.push_back(std::byte{0});
        }
        else
        {
            malformed[22] ^= std::byte{0x01};
        }
        const auto result = receiver.ReceiveControlRecord(malformed);
        Require(!result, "malformed Control record was accepted");
        Require(receiver.GetTelemetry().activeSessionCount == 0,
            "malformed Control mutated Receiver session state");
        rejectedCount++;
    }

    auto fragmentReceiverResult = pbreceiver::ReceiverIngress::Create(
        resourcePolicy,
        kOuterBlockBytes);
    RequireResult(fragmentReceiverResult,
        "fragment conflict receiver creation failed");
    pbreceiver::ReceiverIngress fragmentReceiver =
        std::move(fragmentReceiverResult).Value();
    const pbprotocol::SessionDescriptor secondSession{
        pbprotocol::GetProtocolVersion(),
        MakeSessionId("negative-control-second"),
        9,
        1,
        pbprotocol::DigestAlgorithm::Blake3_256};
    const std::vector<std::byte> secondRecord = MakeSessionControlRecord(
        secondSession,
        resourcePolicy,
        1);
    const auto firstFragmentResult = pbprotocol::GetControlFragment(
        0x88776655ULL,
        validRecord,
        0,
        32);
    const auto conflictingFragmentResult = pbprotocol::GetControlFragment(
        0x88776655ULL,
        secondRecord,
        0,
        32);
    RequireResult(firstFragmentResult, "first Control fragment creation failed");
    RequireResult(conflictingFragmentResult,
        "conflicting Control fragment creation failed");
    const auto firstSizeResult = pbprotocol::GetSerializedSize(
        firstFragmentResult.Value());
    const auto conflictingSizeResult = pbprotocol::GetSerializedSize(
        conflictingFragmentResult.Value());
    RequireResult(firstSizeResult, "first fragment size failed");
    RequireResult(conflictingSizeResult, "conflicting fragment size failed");
    std::vector<std::byte> firstFragment(firstSizeResult.Value());
    std::vector<std::byte> conflictingFragment(conflictingSizeResult.Value());
    RequireResult(pbprotocol::SerializeControlFragment(
        firstFragmentResult.Value(),
        firstFragment),
        "first fragment serialization failed");
    RequireResult(pbprotocol::SerializeControlFragment(
        conflictingFragmentResult.Value(),
        conflictingFragment),
        "conflicting fragment serialization failed");
    RequireResult(fragmentReceiver.ReceiveControlFragment(firstFragment, 1),
        "first fragment admission failed");
    const auto conflictResult = fragmentReceiver.ReceiveControlFragment(
        conflictingFragment,
        2);
    Require(!conflictResult && HasProtocolError(
        conflictResult.Error(),
        pbprotocol::ProtocolErrorCode::ControlFragmentConflict),
        "same-key conflicting Control fragment did not fail closed");
    const auto repeatedConflict = fragmentReceiver.ReceiveControlFragment(firstFragment, 3);
    Require(!repeatedConflict && HasProtocolError(repeatedConflict.Error(),
        pbprotocol::ProtocolErrorCode::ControlFragmentConflict) &&
        fragmentReceiver.GetTelemetry().activeSessionCount == 0,
        "fragment conflict tombstone was not terminal");
    rejectedCount++;

    std::ostringstream stream;
    stream << "{\"case\":\"malformed-control\",\"status\":\"pass\""
           << ",\"rejected_inputs\":" << rejectedCount
           << ",\"no_unexpected_state_mutation\":true}";
    evidence.Write(stream.str());
}

void RunDescriptorConflictCheck(EvidenceWriter& evidence)
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    const std::vector<std::byte> rawBytes(8, std::byte{0x5A});
    const pbprotocol::SessionDescriptor sessionDescriptor{
        pbprotocol::GetProtocolVersion(),
        MakeSessionId("descriptor-conflict"),
        9,
        1,
        pbprotocol::DigestAlgorithm::Blake3_256};
    const pbcompression::EncodedSegment encodedSegment{
        rawBytes,
        pbprotocol::CompressionCodec::Raw};
    const pbprotocol::SegmentDescriptor descriptor = DescribeSegment(
        sessionDescriptor,
        0,
        0,
        rawBytes,
        encodedSegment);
    pbprotocol::SegmentDescriptor conflictingDescriptor = descriptor;
    conflictingDescriptor.rawOffset = 1;

    auto receiverResult = pbreceiver::ReceiverIngress::Create(
        resourcePolicy,
        kOuterBlockBytes);
    RequireResult(receiverResult, "descriptor conflict receiver failed");
    pbreceiver::ReceiverIngress receiver = std::move(receiverResult).Value();
    RequireResult(receiver.ReceiveControlRecord(MakeSessionControlRecord(
        sessionDescriptor,
        resourcePolicy,
        1)),
        "descriptor conflict Session admission failed");
    RequireResult(receiver.ReceiveControlRecord(MakeSegmentControlRecord(
        descriptor,
        sessionDescriptor,
        resourcePolicy,
        2)),
        "first descriptor admission failed");
    const auto conflictResult = receiver.ReceiveControlRecord(
        MakeSegmentControlRecord(
            conflictingDescriptor,
            sessionDescriptor,
            resourcePolicy,
            3));
    Require(!conflictResult && HasProtocolError(
        conflictResult.Error(),
        pbprotocol::ProtocolErrorCode::DescriptorConflict),
        "CRC-valid descriptor conflict was not terminal");
    const auto finalizationResult = receiver.PrepareFinalization(
        descriptor.sessionTag);
    Require(!finalizationResult && HasProtocolError(
        finalizationResult.Error(),
        pbprotocol::ProtocolErrorCode::DescriptorConflict),
        "descriptor conflict did not block finalization");
    evidence.Write(
        "{\"case\":\"descriptor-conflict\",\"status\":\"pass\","
        "\"terminal\":true,\"published\":false}");
}

void RunQuotaChecks(
    const std::filesystem::path& scratchRoot,
    EvidenceWriter& evidence)
{
    pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    resourcePolicy.maxAcceptedFileBytes = 64;
    resourcePolicy.maxOutputPreallocationBytesWithoutPrompt = 32;
    resourcePolicy.maxResumeBytes = 32;
    auto receiverResult = pbreceiver::ReceiverIngress::Create(
        resourcePolicy,
        kOuterBlockBytes);
    RequireResult(receiverResult, "quota receiver creation failed");
    pbreceiver::ReceiverIngress receiver = std::move(receiverResult).Value();
    const std::filesystem::path forbiddenPart =
        scratchRoot / "quota-output.part";
    Require(!std::filesystem::exists(std::filesystem::symlink_status(forbiddenPart)),
        "quota test requires a fresh scratch path");
    const auto outputResult = receiver.EvaluateOutputReservation(65);
    Require(!outputResult && HasProtocolError(
        outputResult.Error(),
        pbprotocol::ProtocolErrorCode::OutputReservationDenied),
        "output reservation quota did not reject");
    Require(!std::filesystem::exists(forbiddenPart),
        "quota rejection created output.part");

    const std::vector<std::byte> oversizedResume(33, std::byte{0});
    const auto resumeResult = pbprotocol::LoadResumeState(
        oversizedResume,
        resourcePolicy);
    Require(!resumeResult && resumeResult.Error().code ==
        pbprotocol::ProtocolErrorCode::ResourceLimitExceeded,
        "resume quota did not reject before parsing");

    const pbreceiver::ReceiverResourceTelemetrySnapshot telemetry =
        receiver.GetTelemetry();
    Require(telemetry.outputReservationDeniedCount == 1 &&
        telemetry.totalResourcePolicyRejectedCount == 1,
        "quota telemetry mismatch");
    evidence.Write(
        "{\"case\":\"quota-exceed\",\"status\":\"pass\","
        "\"output_precreation_rejected\":true,"
        "\"resume_preparse_rejected\":true,\"telemetry_checked\":true}");
}

void RunPendingPayloadConflictChecks(EvidenceWriter& evidence)
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy = pbprotocol::GetDefaultReceiverResourcePolicy();
    for (const std::size_t byteCount : {2628U, 2629U})
    {
        const std::string caseName = "pending-payload-conflict-" + std::to_string(byteCount);
        std::vector<std::byte> rawBytes(byteCount);
        SplitMix64 random(0xC0F11C7ULL);
        FillRandomBytes(random, rawBytes);
        const pbcompression::EncodedSegment encodedSegment{rawBytes, pbprotocol::CompressionCodec::Raw};
        const pbprotocol::SessionDescriptor sessionDescriptor{
            pbprotocol::GetProtocolVersion(), MakeSessionId(caseName), byteCount, 1,
            pbprotocol::DigestAlgorithm::Blake3_256};
        const pbprotocol::SegmentDescriptor descriptor = DescribeSegment(sessionDescriptor, 0, 0, rawBytes, encodedSegment);
        const pbprotocol::FinalManifest manifest{
            sessionDescriptor.sessionId, byteCount, 1,
            pbprotocol::WholeFileDigest{HashSpanStreaming(rawBytes)}, pbprotocol::DigestAlgorithm::Blake3_256};
        std::vector<OuterSendBlock> blocks;
        if (descriptor.outerFecMode == pbprotocol::OuterFecMode::DirectRepeat)
        {
            auto encoderResult = pbouterfec::DirectRepeatEncoder::Recreate(rawBytes, descriptor);
            RequireResult(encoderResult, "pending DirectRepeat conflict encoder failed");
            for (std::uint32_t blockId = 0; blockId < encoderResult.Value().GetBlockCount(); blockId++)
            {
                blocks.push_back(EncodeOuterBlock(encoderResult.Value(), blockId));
            }
        }
        else
        {
            auto encoderResult = pbouterfec::WirehairV2Encoder::Recreate(rawBytes, descriptor);
            RequireResult(encoderResult, "pending Wirehair conflict encoder failed");
            for (std::uint32_t blockId = 0; blockId < encoderResult.Value().GetBlockCount(); blockId++)
            {
                blocks.push_back(EncodeOuterBlock(encoderResult.Value(), blockId));
            }
        }
        auto receiverResult = pbreceiver::ReceiverIngress::Create(resourcePolicy, kOuterBlockBytes);
        RequireResult(receiverResult, "pending conflict receiver creation failed");
        pbreceiver::ReceiverIngress receiver = std::move(receiverResult).Value();
        GateMetrics metrics;
        RasterTransportPipeline pipeline(receiver, descriptor.sessionTag, metrics);
        static_cast<void>(pipeline.ProcessFrame(MakeSessionControlRecord(sessionDescriptor, resourcePolicy, 1), {}));
        static_cast<void>(pipeline.ProcessFrame(MakeManifestControlRecord(manifest, sessionDescriptor, resourcePolicy, 2), {}));
        const auto segmentControl = MakeSegmentControlRecord(descriptor, sessionDescriptor, resourcePolicy, 3);
        FrameOutcome readyOutcome = pipeline.ProcessFrame(segmentControl, blocks);
        Require(readyOutcome.completedSegment.has_value(), "pending conflict setup did not recover through raster");
        auto verifiedResult = receiver.VerifyRecoveredSegment(std::move(*readyOutcome.completedSegment));
        RequireResult(verifiedResult, "pending conflict setup did not verify recovered bytes");
        const auto incompleteResult = receiver.PrepareFinalization(descriptor.sessionTag);
        Require(!incompleteResult && HasProtocolError(incompleteResult.Error(),
            pbprotocol::ProtocolErrorCode::SegmentRecoveryIncomplete), "pending conflict setup prematurely finalized");

        blocks[0].payload[0] ^= std::byte{1};
        bool conflictRejected = false;
        try
        {
            // This regenerates valid Transport CRCs, LDPC, and raster pixels;
            // rejection must come from the retained accepted-ID fingerprint.
            static_cast<void>(pipeline.ProcessFrame(segmentControl, std::span<const OuterSendBlock>(blocks).first(1)));
        }
        catch (const GateFailure& error)
        {
            conflictRejected = std::string_view(error.what()) == "Receiver rejected decoded Data block";
        }
        const auto commitResult = receiver.CommitStoredSegment(std::move(verifiedResult).Value());
        const auto finalizationResult = receiver.PrepareFinalization(descriptor.sessionTag);
        Require(conflictRejected && !commitResult && !finalizationResult,
            "CRC-valid pending payload conflict did not block commit and finalization");
        const auto* commitError = std::get_if<pbouterfec::OuterFecError>(&commitResult.Error());
        const auto* finalizationError = std::get_if<pbouterfec::OuterFecError>(&finalizationResult.Error());
        Require(commitError != nullptr && finalizationError != nullptr &&
            commitError->code == pbouterfec::OuterFecErrorCode::OuterBlockConflict &&
            finalizationError->code == pbouterfec::OuterFecErrorCode::OuterBlockConflict &&
            receiver.GetTelemetry().activeOuterFecDecoderCount == 0,
            "pending conflict was not terminal or retained a decoder reservation");
        evidence.Write("{\"case\":\"" + caseName + "\",\"status\":\"pass\","
            "\"crc_valid_raster_conflict\":true,\"verified_before_conflict\":true,"
            "\"commit_blocked\":true,\"finalization_blocked\":true,\"reservation_released\":true}");
    }
}

void RunPublicationNegativeChecks(
    const std::filesystem::path& scratchRoot,
    EvidenceWriter& evidence)
{
    const std::array<std::byte, 4> bytes{
        std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    const pbprotocol::ReceiverResourcePolicy resourcePolicy = pbprotocol::GetDefaultReceiverResourcePolicy();
    for (std::size_t negativeIndex = 0; negativeIndex < 3; negativeIndex++)
    {
        const std::string caseName = "publish-negative-" + std::to_string(negativeIndex);
        const std::filesystem::path caseRoot = scratchRoot / caseName;
        CreateFreshCaseDirectory(caseRoot);
        const std::filesystem::path sourcePath = caseRoot / "source.bin";
        const std::filesystem::path partPath = caseRoot / "output.part";
        const std::filesystem::path finalPath = caseRoot / "output.bin";
        {
            std::ofstream source(sourcePath, std::ios::binary | std::ios::trunc);
            source.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            source.flush();
            Require(source.good(), "publication negative source write failed");
        }

        const pbprotocol::SessionDescriptor sessionDescriptor{
            pbprotocol::GetProtocolVersion(), MakeSessionId(caseName), bytes.size(), 1,
            pbprotocol::DigestAlgorithm::Blake3_256};
        const pbprotocol::SessionTag sessionTag = pbprotocol::DeriveSessionTag(sessionDescriptor.sessionId);
        pbprotocol::FinalManifest manifest{
            sessionDescriptor.sessionId, bytes.size(), 1,
            pbprotocol::WholeFileDigest{HashSpanStreaming(bytes)}, pbprotocol::DigestAlgorithm::Blake3_256};
        if (negativeIndex == 0)
        {
            manifest.wholeFileDigest.bytes[0] ^= std::byte{0x80};
        }
        auto receiverResult = pbreceiver::ReceiverIngress::Create(resourcePolicy, kOuterBlockBytes);
        RequireResult(receiverResult, "publication negative receiver creation failed");
        pbreceiver::ReceiverIngress receiver = std::move(receiverResult).Value();
        const auto sessionResult = receiver.ReceiveControlRecord(MakeSessionControlRecord(
            sessionDescriptor, resourcePolicy, 1));
        RequireResult(sessionResult, "publication negative Session admission failed");
        Require(sessionResult.Value().outputReservationDecision == pbprotocol::OutputReservationDecision::AutoAccept,
            "publication negative output reservation failed");
        PreparePartFile(partPath, bytes.size());
        RequireResult(receiver.ReceiveControlRecord(MakeManifestControlRecord(
            manifest, sessionDescriptor, resourcePolicy, 2)), "publication negative manifest admission failed");
        const auto missingSegmentResult = receiver.PrepareFinalization(sessionTag);
        Require(!missingSegmentResult && HasProtocolError(missingSegmentResult.Error(),
            pbprotocol::ProtocolErrorCode::SegmentMapIncomplete), "missing Segment did not block finalization");
        const pbcompression::EncodedSegment encodedSegment{
            std::vector<std::byte>(bytes.begin(), bytes.end()), pbprotocol::CompressionCodec::Raw};
        const pbprotocol::SegmentDescriptor descriptor = DescribeSegment(
            sessionDescriptor, 0, 0, bytes, encodedSegment);
        const auto segmentResult = receiver.ReceiveControlRecord(MakeSegmentControlRecord(
            descriptor, sessionDescriptor, resourcePolicy, 3));
        RequireResult(segmentResult, "publication negative Segment admission failed");
        Require(segmentResult.Value().controlAdmission.boundSegmentDescriptor.has_value(),
            "publication negative bound descriptor missing");
        const auto uncommittedResult = receiver.PrepareFinalization(sessionTag);
        Require(!uncommittedResult && HasProtocolError(uncommittedResult.Error(),
            pbprotocol::ProtocolErrorCode::SegmentRecoveryIncomplete), "uncommitted Segment did not block finalization");
        GateMetrics metrics;
        StoreAndCommitSegment(receiver, pbreceiver::ReceiverCompletedSegment(
            *segmentResult.Value().controlAdmission.boundSegmentDescriptor,
            std::vector<std::byte>(bytes.begin(), bytes.end())), partPath, metrics);
        const auto authoritativeResult = receiver.PrepareFinalization(sessionTag);
        RequireResult(authoritativeResult, "publication negative authoritative manifest unavailable");
        Require(authoritativeResult.Value() == manifest && metrics.storedCommitCount == 1,
            "publication negative did not reach the real post-commit publication branch");

        if (negativeIndex == 1)
        {
            std::filesystem::resize_file(partPath, bytes.size() - 1U);
        }
        if (negativeIndex == 2)
        {
            std::ofstream finalFile(finalPath, std::ios::binary | std::ios::trunc);
            finalFile.put('\0');
            finalFile.flush();
            Require(finalFile.good(), "publication negative existing target creation failed");
        }
        const auto partDigestBefore = HashFile(partPath);
        const std::uint64_t partBytesBefore = std::filesystem::file_size(partPath);
        const auto existingFinalDigest = negativeIndex == 2 ? HashFile(finalPath) : HashSpanStreaming({});
        bool publicationRejected = false;
        try
        {
            VerifyAndPublish(sourcePath, partPath, finalPath, authoritativeResult.Value());
        }
        catch (const GateFailure& error)
        {
            publicationRejected = std::string_view(error.what()) ==
                "output.part failed final sequential WholeFileDigest gate";
        }
        Require(publicationRejected && std::filesystem::exists(partPath) &&
            std::filesystem::file_size(partPath) == partBytesBefore && HashFile(partPath) == partDigestBefore,
            "rejected publication mutated or renamed output.part");
        Require(negativeIndex == 2
                ? HashFile(finalPath) == existingFinalDigest
                : !std::filesystem::exists(finalPath),
            "rejected publication created or overwrote final");
        std::filesystem::remove_all(caseRoot);
    }
    evidence.Write(
        "{\"case\":\"finalization-negative\",\"status\":\"pass\","
        "\"wrong_digest\":\"rejected\",\"wrong_part_length\":\"rejected\","
        "\"existing_final\":\"rejected\",\"missing_segment\":\"rejected\","
        "\"uncommitted_segment\":\"rejected\",\"authoritative_manifest\":true,"
        "\"publication_attempts\":3,\"failure_output_immutable\":true}");
}

class FailingStorageBuffer final : public std::stringbuf
{
public:
    explicit FailingStorageBuffer(const bool failWrite)
        : std::stringbuf(std::ios::in | std::ios::out | std::ios::binary), failWrite_(failWrite)
    {
    }

protected:
    std::streamsize xsputn(const char* bytes, const std::streamsize byteCount) override
    {
        return failWrite_ ? 0 : std::stringbuf::xsputn(bytes, byteCount);
    }

    int sync() override
    {
        return -1;
    }

private:
    bool failWrite_ = false;
};

void RunStorageFailureChecks(const std::filesystem::path& scratchRoot, EvidenceWriter& evidence)
{
    const FileCaseSpec caseSpec{"storage-failure-retry", 4,
        SourcePattern::DeterministicRandom, 0xFA11EDULL,
        pbprotocol::CompressionCodec::Raw, pbprotocol::OuterFecMode::DirectRepeat};
    const std::filesystem::path caseRoot = scratchRoot / caseSpec.name;
    CreateFreshCaseDirectory(caseRoot);
    const std::filesystem::path sourcePath = caseRoot / "source.bin";
    const std::filesystem::path partPath = caseRoot / "output.part";
    const std::filesystem::path finalPath = caseRoot / "output.bin";
    CreateSourceFile(sourcePath, caseSpec);
    GateMetrics metrics;
    const DescribedFile describedFile = DescribeFile(sourcePath, caseSpec, metrics);
    const pbprotocol::ReceiverResourcePolicy resourcePolicy = pbprotocol::GetDefaultReceiverResourcePolicy();
    const pbprotocol::SegmentDescriptor& descriptor = describedFile.segmentDescriptors[0];
    auto receiverResult = pbreceiver::ReceiverIngress::Create(resourcePolicy, kOuterBlockBytes);
    RequireResult(receiverResult, "storage negative receiver creation failed");
    pbreceiver::ReceiverIngress receiver = std::move(receiverResult).Value();
    RasterTransportPipeline pipeline(receiver, descriptor.sessionTag, metrics);
    static_cast<void>(pipeline.ProcessFrame(MakeSessionControlRecord(
        describedFile.sessionDescriptor, resourcePolicy, 1), {}));
    static_cast<void>(pipeline.ProcessFrame(MakeManifestControlRecord(
        describedFile.finalManifest, describedFile.sessionDescriptor, resourcePolicy, 2), {}));
    PreparePartFile(partPath, caseSpec.fileBytes);
    const auto originalPartDigest = HashFile(partPath);
    const std::vector<std::byte> rawBytes = ReadFileRange(sourcePath, 0, 4);
    auto encoderResult = pbouterfec::DirectRepeatEncoder::Recreate(rawBytes, descriptor);
    RequireResult(encoderResult, "storage negative encoder failed");
    pbouterfec::DirectRepeatEncoder encoder = std::move(encoderResult).Value();
    const std::array<OuterSendBlock, 1> blocks{EncodeOuterBlock(encoder, 0)};
    const std::vector<std::byte> segmentControl = MakeSegmentControlRecord(
        descriptor, describedFile.sessionDescriptor, resourcePolicy, 3);

    for (const bool failWrite : {true, false})
    {
        FrameOutcome outcome = pipeline.ProcessFrame(segmentControl, blocks);
        Require(outcome.completedSegment.has_value(), "failed storage prevented Carousel re-recovery");
        FailingStorageBuffer failingBuffer(failWrite);
        std::ostream failingStream(&failingBuffer);
        bool rejectedAtStorage = false;
        try
        {
            StoreAndCommitSegment(receiver, std::move(*outcome.completedSegment), partPath, metrics, &failingStream);
        }
        catch (const GateFailure& error)
        {
            rejectedAtStorage = std::string_view(error.what()) == "output.part write or flush failed";
        }
        Require(rejectedAtStorage && metrics.storedCommitCount == 0,
            "injected storage failure did not prevent commit after verification");
        const auto finalizationResult = receiver.PrepareFinalization(descriptor.sessionTag);
        Require(!finalizationResult && HasProtocolError(finalizationResult.Error(),
            pbprotocol::ProtocolErrorCode::SegmentRecoveryIncomplete),
            "failed write/flush left a completed Segment");
        Require(HashFile(partPath) == originalPartDigest && !std::filesystem::exists(finalPath),
            "failed storage mutated or published the file");
    }

    FrameOutcome retryOutcome = pipeline.ProcessFrame(segmentControl, blocks);
    Require(retryOutcome.completedSegment.has_value(), "post-failure Carousel could not recover");
    StoreAndCommitSegment(receiver, std::move(*retryOutcome.completedSegment), partPath, metrics);
    const auto finalizationResult = receiver.PrepareFinalization(descriptor.sessionTag);
    RequireResult(finalizationResult, "successful retry remained unfinalizable");
    VerifyAndPublish(sourcePath, partPath, finalPath, finalizationResult.Value());
    evidence.Write("{\"case\":\"storage-failure-retry\",\"status\":\"pass\","
        "\"write_failure_rejected\":true,\"flush_failure_rejected\":true,"
        "\"failure_output_immutable\":true,\"carousel_rerecovery\":true,"
        "\"successful_commits\":1,\"final_sequential_digest\":true,\"final_byte_identical\":true}");
    std::filesystem::remove_all(caseRoot);
}

void RunSameFrameReadyRepeatChecks(const std::filesystem::path& scratchRoot, EvidenceWriter& evidence)
{
    for (const std::uint64_t fileBytes : {1ULL, 2629ULL})
    {
        const auto expectedMode = fileBytes == 1 ? pbprotocol::OuterFecMode::DirectRepeat : pbprotocol::OuterFecMode::WirehairV2;
        const FileCaseSpec caseSpec{"same-frame-ready-repeat-" + std::to_string(fileBytes), fileBytes,
            SourcePattern::DeterministicRandom, 0xD001C47EULL + fileBytes, pbprotocol::CompressionCodec::Raw, expectedMode};
        const auto caseRoot = scratchRoot / caseSpec.name;
        CreateFreshCaseDirectory(caseRoot);
        const auto sourcePath = caseRoot / "source.bin";
        const auto partPath = caseRoot / "output.part";
        const auto finalPath = caseRoot / "output.bin";
        CreateSourceFile(sourcePath, caseSpec);
        GateMetrics metrics;
        const auto described = DescribeFile(sourcePath, caseSpec, metrics);
        const auto& descriptor = described.segmentDescriptors.front();
        Require(descriptor.compressionCodec == pbprotocol::CompressionCodec::Raw && descriptor.outerFecMode == expectedMode,
            "same-frame repeat fixture codec/FEC mismatch");
        const auto rawBytes = ReadFileRange(sourcePath, 0, static_cast<std::size_t>(fileBytes));
        const auto policy = pbprotocol::GetDefaultReceiverResourcePolicy();
        auto receiverResult = pbreceiver::ReceiverIngress::Create(policy, kOuterBlockBytes);
        RequireResult(receiverResult, "same-frame repeat receiver creation failed");
        auto receiver = std::move(receiverResult).Value();
        RasterTransportPipeline pipeline(receiver, descriptor.sessionTag, metrics);
        const auto sessionOutcome = pipeline.ProcessFrame(MakeSessionControlRecord(described.sessionDescriptor, policy, 1), {});
        Require(sessionOutcome.controlAdmission && sessionOutcome.controlAdmission->outputReservationDecision ==
            pbprotocol::OutputReservationDecision::AutoAccept, "same-frame repeat reservation failed");
        PreparePartFile(partPath, fileBytes);
        static_cast<void>(pipeline.ProcessFrame(MakeManifestControlRecord(described.finalManifest, described.sessionDescriptor, policy, 2), {}));
        const auto segmentControl = MakeSegmentControlRecord(descriptor, described.sessionDescriptor, policy, 3);
        std::vector<OuterSendBlock> blocks;
        if (expectedMode == pbprotocol::OuterFecMode::DirectRepeat)
        {
            auto encoder = pbouterfec::DirectRepeatEncoder::Recreate(rawBytes, descriptor);
            RequireResult(encoder, "same-frame repeat DirectRepeat encoder failed");
            blocks.push_back(EncodeOuterBlock(encoder.Value(), 0));
        }
        else
        {
            auto encoder = pbouterfec::WirehairV2Encoder::Recreate(rawBytes, descriptor);
            RequireResult(encoder, "same-frame repeat Wirehair encoder failed");
            for (std::uint32_t blockId = 0; blockId < encoder.Value().GetBlockCount(); blockId++)
            {
                blocks.push_back(EncodeOuterBlock(encoder.Value(), blockId));
            }
        }
        blocks.push_back(blocks.front());
        auto outcome = pipeline.ProcessFrame(segmentControl, blocks);
        Require(outcome.completedSegment && outcome.repeatedReadyCount == 1,
            "same-frame duplicate Ready did not coalesce one exact recovery");
        const auto incomplete = receiver.PrepareFinalization(descriptor.sessionTag);
        Require(!incomplete && HasProtocolError(incomplete.Error(), pbprotocol::ProtocolErrorCode::SegmentRecoveryIncomplete) &&
            receiver.GetTelemetry().activeOuterFecDecoderCount == 1, "same-frame duplicate prematurely committed or released reservation");
        const auto frameDigest = pipeline.GetFrameDigest();
        StoreAndCommitSegment(receiver, std::move(*outcome.completedSegment), partPath, metrics);
        const auto repeatedFrame = pipeline.ProcessFrame(segmentControl, blocks, true, false, true);
        Require(repeatedFrame.alreadyCompletedCount == blocks.size() && !repeatedFrame.completedSegment &&
            metrics.storedCommitCount == 1 && receiver.GetTelemetry().activeOuterFecDecoderCount == 0 &&
            pipeline.GetFrameDigest() == frameDigest, "same-frame repeat was not idempotent after commit");
        const auto manifest = receiver.PrepareFinalization(descriptor.sessionTag);
        RequireResult(manifest, "same-frame repeat finalization failed");
        VerifyAndPublish(sourcePath, partPath, finalPath, manifest.Value());
        evidence.Write("{\"case\":\"" + caseSpec.name + "\",\"status\":\"pass\",\"repeated_ready_results\":1,"
            "\"stored_commits\":1,\"postcommit_same_frame_idempotent\":true,\"final_sequential_digest\":true,\"final_byte_identical\":true}");
        std::filesystem::remove_all(caseRoot);
    }
}

void RunFastGate(
    const std::filesystem::path& scratchRoot,
    EvidenceWriter& evidence)
{
    const std::vector<FileCaseSpec> cases{
        {"file-0b", 0, SourcePattern::DeterministicRandom, 0x1000ULL,
            std::nullopt, std::nullopt},
        {"file-1b", 1, SourcePattern::DeterministicRandom, 0x1001ULL,
            pbprotocol::CompressionCodec::Raw,
            pbprotocol::OuterFecMode::DirectRepeat},
        {"outer-1313", 1313, SourcePattern::DeterministicRandom, 0x1313ULL,
            pbprotocol::CompressionCodec::Raw,
            pbprotocol::OuterFecMode::DirectRepeat},
        {"outer-1314", 1314, SourcePattern::DeterministicRandom, 0x1314ULL,
            pbprotocol::CompressionCodec::Raw,
            pbprotocol::OuterFecMode::DirectRepeat},
        {"outer-1315", 1315, SourcePattern::DeterministicRandom, 0x1315ULL,
            pbprotocol::CompressionCodec::Raw,
            pbprotocol::OuterFecMode::DirectRepeat},
        {"direct-threshold-2628", 2628, SourcePattern::DeterministicRandom,
            0x2628ULL, pbprotocol::CompressionCodec::Raw,
            pbprotocol::OuterFecMode::DirectRepeat},
        {"wirehair-threshold-2629", 2629,
            SourcePattern::DeterministicRandom, 0x2629ULL,
            pbprotocol::CompressionCodec::Raw,
            pbprotocol::OuterFecMode::WirehairV2},
        {"segment-8m-minus-1", kSourceSegmentBytes - 1ULL,
            SourcePattern::DeterministicRandom, 0x80000001ULL,
            pbprotocol::CompressionCodec::Raw,
            pbprotocol::OuterFecMode::WirehairV2},
        {"segment-8m", kSourceSegmentBytes,
            SourcePattern::DeterministicRandom, 0x80000002ULL,
            pbprotocol::CompressionCodec::Raw,
            pbprotocol::OuterFecMode::WirehairV2},
        {"segment-8m-plus-1", kSourceSegmentBytes + 1ULL,
            SourcePattern::DeterministicRandom, 0x80000003ULL,
            pbprotocol::CompressionCodec::Raw,
            pbprotocol::OuterFecMode::WirehairV2},
        {"random-1m", kMebibyte, SourcePattern::DeterministicRandom,
            0x10000001ULL, pbprotocol::CompressionCodec::Raw,
            pbprotocol::OuterFecMode::WirehairV2},
        {"compressed-1m", kMebibyte, SourcePattern::Compressible,
            0x10000002ULL, pbprotocol::CompressionCodec::Zstandard,
            pbprotocol::OuterFecMode::DirectRepeat}};
    for (const FileCaseSpec& caseSpec : cases)
    {
        RunFileCase(caseSpec, scratchRoot, evidence);
    }
    RunControlNegativeChecks(evidence);
    RunDescriptorConflictCheck(evidence);
    RunQuotaChecks(scratchRoot, evidence);
    RunPendingPayloadConflictChecks(evidence);
    RunPublicationNegativeChecks(scratchRoot, evidence);
    RunStorageFailureChecks(scratchRoot, evidence);
    RunSameFrameReadyRepeatChecks(scratchRoot, evidence);
}

void RunLargeGate(
    const std::filesystem::path& scratchRoot,
    EvidenceWriter& evidence)
{
    const FileCaseSpec largeCase{
        "random-100m",
        100ULL * kMebibyte,
        SourcePattern::DeterministicRandom,
        0x6400000000000001ULL,
        pbprotocol::CompressionCodec::Raw,
        pbprotocol::OuterFecMode::WirehairV2};
    RunFileCase(largeCase, scratchRoot, evidence);
}

template <typename EncoderType, typename DecoderType>
void CompleteResumedSegment(
    EncoderType& encoder,
    DecoderType& decoder,
    const pbprotocol::BoundSegmentDescriptor& boundDescriptor,
    const std::uint32_t cachedBlockId,
    const std::vector<std::byte>& segmentControlRecord,
    RasterTransportPipeline& pipeline,
    pbreceiver::ReceiverIngress& receiver,
    const std::filesystem::path& partPath,
    GateMetrics& metrics)
{
    const pbprotocol::SegmentDescriptor& descriptor =
        boundDescriptor.GetDescriptor();
    const std::uint64_t blockCountWide = encoder.GetBlockCount();
    Require(blockCountWide <= std::numeric_limits<std::uint32_t>::max(),
        "resume block count cannot narrow");
    const std::uint32_t blockCount = static_cast<std::uint32_t>(blockCountWide);
    pipeline.SetActiveSegmentOrdinal(descriptor.segmentOrdinal);
    std::vector<std::uint32_t> remainingBlockIds;
    remainingBlockIds.reserve(blockCount);
    for (std::uint32_t reverseId = blockCount; reverseId > 0; reverseId--)
    {
        const std::uint32_t blockId = reverseId - 1U;
        if (blockId != cachedBlockId)
        {
            remainingBlockIds.push_back(blockId);
        }
    }

    bool ready = false;
    std::size_t nextIndex = 0;
    while (nextIndex < remainingBlockIds.size())
    {
        const std::size_t batchCount = std::min<std::size_t>(
            kCodewordsPerFrame,
            remainingBlockIds.size() - nextIndex);
        std::vector<OuterSendBlock> blocks;
        blocks.reserve(batchCount);
        for (std::size_t batchIndex = 0;
            batchIndex < batchCount;
            batchIndex++)
        {
            blocks.push_back(EncodeOuterBlock(
                encoder,
                remainingBlockIds[nextIndex + batchIndex]));
        }
        nextIndex += batchCount;
        const FrameOutcome outcome = pipeline.ProcessFrame(
            segmentControlRecord,
            blocks,
            false,
            true);
        Require(outcome.validatedBlocks.size() == blocks.size(),
            "resume live path lost validated Transport blocks");
        for (const OuterSendBlock& block : outcome.validatedBlocks)
        {
            if constexpr (std::is_same_v<
                DecoderType,
                pbouterfec::DirectRepeatDecoder>)
            {
                std::array<std::byte, kOuterBlockBytes> paddedPayload{};
                std::copy(
                    block.payload.begin(),
                    block.payload.end(),
                    paddedPayload.begin());
                const auto decodeResult = decoder.DecodeBlock(
                    block.outerBlockId,
                    static_cast<std::uint32_t>(block.payload.size()),
                    paddedPayload);
                RequireResult(decodeResult,
                    "resumed DirectRepeat rejected a live block");
                ready = ready || decodeResult.Value() ==
                    pbouterfec::DecodeDisposition::Ready;
            }
            else
            {
                const auto decodeResult = decoder.DecodeBlock(
                    block.outerBlockId,
                    block.payload);
                RequireResult(decodeResult,
                    "resumed Wirehair rejected a live block");
                ready = ready || decodeResult.Value() ==
                    pbouterfec::DecodeDisposition::Ready;
            }
        }
    }
    Require(ready, "resumed decoder never reached Ready");
    std::vector<std::byte> encodedBytes(
        static_cast<std::size_t>(descriptor.encodedSize));
    const auto recoverResult = decoder.Recover(encodedBytes);
    RequireResult(recoverResult, "resumed outer recovery failed encoded digest");
    Require(recoverResult.Value() == descriptor.encodedSize,
        "resumed outer recovery returned a wrong byte count");
    StoreAndCommitSegment(
        receiver,
        pbreceiver::ReceiverCompletedSegment(
            boundDescriptor,
            std::move(encodedBytes)),
        partPath,
        metrics);
    if (descriptor.outerFecMode == pbprotocol::OuterFecMode::WirehairV2)
    {
        metrics.wirehairSegmentCount++;
    }
    else
    {
        metrics.directRepeatSegmentCount++;
    }
}

void RunResumeGate(
    const std::filesystem::path& scratchRoot,
    EvidenceWriter& evidence,
    const bool compressedCompleted = false)
{
    const auto started = std::chrono::steady_clock::now();
    const FileCaseSpec caseSpec{
        compressedCompleted ? "resume-compressed-completed" : "resume-file-restart",
        2ULL * kSourceSegmentBytes + 2ULL * kOuterBlockBytes,
        compressedCompleted ? SourcePattern::CompressedCompletedResume : SourcePattern::DeterministicRandom,
        0x52534D4500000001ULL,
        compressedCompleted ? pbprotocol::CompressionCodec::Zstandard : pbprotocol::CompressionCodec::Raw,
        compressedCompleted ? pbprotocol::OuterFecMode::DirectRepeat : pbprotocol::OuterFecMode::WirehairV2};
    const std::filesystem::path caseRoot = scratchRoot / caseSpec.name;
    CreateFreshCaseDirectory(caseRoot);
    const std::filesystem::path sourcePath = caseRoot / "source.bin";
    const std::filesystem::path partPath = caseRoot / "output.part";
    const std::filesystem::path finalPath = caseRoot / "output.bin";
    const std::filesystem::path resumePath = caseRoot / "resume.state";
    CreateSourceFile(sourcePath, caseSpec);
    GateMetrics metrics;
    const DescribedFile describedFile = DescribeFile(
        sourcePath,
        caseSpec,
        metrics);
    Require(describedFile.segmentDescriptors.size() == 3,
        "resume Gate must have exactly three Source Segments");
    Require(describedFile.segmentDescriptors[0].compressionCodec == *caseSpec.expectedFirstCodec &&
        describedFile.segmentDescriptors[0].outerFecMode == *caseSpec.expectedFirstOuterMode,
        "resume completed Segment did not take the required compression/FEC path");
    Require(describedFile.segmentDescriptors[2].outerFecMode ==
        pbprotocol::OuterFecMode::DirectRepeat,
        "resume tail must select DirectRepeat");
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    const pbprotocol::SessionTag sessionTag = pbprotocol::DeriveSessionTag(
        describedFile.sessionDescriptor.sessionId);
    const std::vector<std::byte> sessionControl = MakeSessionControlRecord(
        describedFile.sessionDescriptor,
        resourcePolicy,
        1);
    const std::vector<std::byte> manifestControl = MakeManifestControlRecord(
        describedFile.finalManifest,
        describedFile.sessionDescriptor,
        resourcePolicy,
        2);
    std::vector<std::vector<std::byte>> segmentControls;
    std::transform(describedFile.segmentDescriptors.begin(), describedFile.segmentDescriptors.end(),
        std::back_inserter(segmentControls), [&](const pbprotocol::SegmentDescriptor& descriptor)
        {
            return MakeSegmentControlRecord(descriptor, describedFile.sessionDescriptor,
                resourcePolicy, 100ULL + descriptor.segmentOrdinal);
        });

    // Receiver A persists only committed raw metadata and blocks that have
    // actually passed raster, LDPC, canonical Transport padding and both CRCs.
    {
        auto receiverResult = pbreceiver::ReceiverIngress::Create(
            resourcePolicy,
            kOuterBlockBytes);
        RequireResult(receiverResult, "resume receiver A creation failed");
        pbreceiver::ReceiverIngress receiver = std::move(receiverResult).Value();
        RasterTransportPipeline pipeline(receiver, sessionTag, metrics);
        const FrameOutcome sessionOutcome = pipeline.ProcessFrame(
            sessionControl,
            {});
        Require(sessionOutcome.controlAdmission &&
            sessionOutcome.controlAdmission->outputReservationDecision ==
                pbprotocol::OutputReservationDecision::AutoAccept,
            "resume output reservation failed");
        PreparePartFile(partPath, caseSpec.fileBytes);
        static_cast<void>(pipeline.ProcessFrame(manifestControl, {}));
        TransferSegment(
            sourcePath,
            describedFile.sessionDescriptor,
            describedFile.segmentDescriptors[0],
            100,
            resourcePolicy,
            pipeline,
            receiver,
            partPath,
            metrics);

        pbprotocol::ResumeActiveWirehairCacheRecord wirehairCache;
        wirehairCache.segmentOrdinal = 1;
        wirehairCache.wirehairProfile =
            *describedFile.segmentDescriptors[1].wirehairV2SerializedProfile;
        {
            const pbprotocol::SegmentDescriptor& descriptor =
                describedFile.segmentDescriptors[1];
            const std::vector<std::byte> encodedBytes = ReadFileRange(
                sourcePath,
                descriptor.rawOffset,
                static_cast<std::size_t>(descriptor.rawSize));
            auto encoderResult = pbouterfec::WirehairV2Encoder::Recreate(
                encodedBytes,
                descriptor);
            RequireResult(encoderResult, "resume A Wirehair recreate failed");
            pbouterfec::WirehairV2Encoder encoder =
                std::move(encoderResult).Value();
            const std::array<OuterSendBlock, 1> blocks{
                EncodeOuterBlock(encoder, 1)};
            pipeline.SetActiveSegmentOrdinal(1);
            const FrameOutcome outcome = pipeline.ProcessFrame(
                segmentControls[1],
                blocks,
                true,
                true);
            Require(!outcome.completedSegment && outcome.validatedBlocks.size() == 1,
                "resume A Wirehair partial capture shape mismatch");
            wirehairCache.entries.push_back(pbprotocol::ResumeWirehairCacheEntry{
                outcome.validatedBlocks[0].outerBlockId,
                outcome.validatedBlocks[0].payload});
        }

        pbprotocol::ResumeActiveDirectRepeatRecord directCache;
        directCache.segmentOrdinal = 2;
        directCache.directBlockCount = 2;
        {
            const pbprotocol::SegmentDescriptor& descriptor =
                describedFile.segmentDescriptors[2];
            const std::vector<std::byte> encodedBytes = ReadFileRange(
                sourcePath,
                descriptor.rawOffset,
                static_cast<std::size_t>(descriptor.rawSize));
            auto encoderResult = pbouterfec::DirectRepeatEncoder::Recreate(
                encodedBytes,
                descriptor);
            RequireResult(encoderResult, "resume A DirectRepeat recreate failed");
            pbouterfec::DirectRepeatEncoder encoder =
                std::move(encoderResult).Value();
            const std::array<OuterSendBlock, 1> blocks{
                EncodeOuterBlock(encoder, 0)};
            pipeline.SetActiveSegmentOrdinal(2);
            const FrameOutcome outcome = pipeline.ProcessFrame(
                segmentControls[2],
                blocks,
                true,
                true);
            Require(!outcome.completedSegment && outcome.validatedBlocks.size() == 1,
                "resume A DirectRepeat partial capture shape mismatch");
            std::vector<std::byte> paddedPayload(kOuterBlockBytes, std::byte{0});
            std::copy(
                outcome.validatedBlocks[0].payload.begin(),
                outcome.validatedBlocks[0].payload.end(),
                paddedPayload.begin());
            directCache.entries.push_back(pbprotocol::ResumeDirectRepeatEntry{
                outcome.validatedBlocks[0].outerBlockId,
                static_cast<std::uint32_t>(
                    outcome.validatedBlocks[0].payload.size()),
                std::move(paddedPayload)});
        }

        auto builderResult = pbprotocol::ResumeStateBuilder::Create(resourcePolicy);
        RequireResult(builderResult, "resume builder creation failed");
        pbprotocol::ResumeStateBuilder builder = std::move(builderResult).Value();
        const pbprotocol::SegmentDescriptor& completedDescriptor =
            describedFile.segmentDescriptors[0];
        RequireResult(builder.AppendCompletedSegment(
            pbprotocol::ResumeCompletedSegmentRecord{
                describedFile.sessionDescriptor.sessionId,
                0,
                completedDescriptor.rawOffset,
                completedDescriptor.rawSize,
                completedDescriptor.rawDigest}),
            "completed resume record append failed");
        RequireResult(builder.AppendActiveWirehairCache(wirehairCache),
            "Wirehair resume cache append failed");
        RequireResult(builder.AppendDirectRepeatReceivedBlocks(directCache),
            "DirectRepeat resume cache append failed");
        const auto writeResult = pbreceiver::WriteResumeStateFile(
            resumePath,
            builder.GetDocument());
        RequireResult(writeResult, "resume.state write failed");
        Require(writeResult.Value() == builder.GetByteCount(),
            "resume.state write byte count mismatch");
        const auto finalizationResult = receiver.PrepareFinalization(sessionTag);
        Require(!finalizationResult,
            "partial resume receiver A was incorrectly finalizable");
    }

    const auto resumeBytesResult = pbreceiver::ReadResumeStateFile(
        resumePath,
        resourcePolicy);
    RequireResult(resumeBytesResult, "resume.state read failed after restart");
    const auto loadedResult = pbprotocol::LoadResumeState(
        resumeBytesResult.Value(),
        resourcePolicy);
    RequireResult(loadedResult, "resume.state document load failed");
    const pbprotocol::LoadedResumeState& loaded = loadedResult.Value();
    Require(loaded.completedSegments.size() == 1 &&
        loaded.activeWirehairCaches.size() == 1 &&
        loaded.activeDirectRepeatRecords.size() == 1 &&
        !loaded.hasTruncatedTail,
        "resume.state record counts differ from persisted state");
    std::vector<std::byte> tornDocument = resumeBytesResult.Value();
    tornDocument.push_back(std::byte{0x50});
    tornDocument.push_back(std::byte{0x42});
    const auto tornResult = pbprotocol::LoadResumeState(
        tornDocument,
        resourcePolicy);
    RequireResult(tornResult, "valid resume prefix was lost to a torn EOF tail");
    Require(tornResult.Value().hasTruncatedTail,
        "torn EOF tail was not reported");

    // Receiver B has no in-process state from A. Standalone production replay
    // decoders consume persisted caches; subsequent live bytes still cross the
    // complete visual chain before entering those decoders.
    {
        auto receiverResult = pbreceiver::ReceiverIngress::Create(
            resourcePolicy,
            kOuterBlockBytes);
        RequireResult(receiverResult, "resume receiver B creation failed");
        pbreceiver::ReceiverIngress receiver = std::move(receiverResult).Value();
        RasterTransportPipeline pipeline(receiver, sessionTag, metrics);
        static_cast<void>(pipeline.ProcessFrame(sessionControl, {}));
        static_cast<void>(pipeline.ProcessFrame(manifestControl, {}));
        std::vector<pbprotocol::BoundSegmentDescriptor> boundDescriptors;
        for (const std::vector<std::byte>& segmentControl : segmentControls)
        {
            FrameOutcome outcome = pipeline.ProcessFrame(segmentControl, {});
            Require(outcome.controlAdmission &&
                outcome.controlAdmission->controlAdmission.boundSegmentDescriptor,
                "replayed Control did not bind a SegmentDescriptor");
            boundDescriptors.push_back(
                *outcome.controlAdmission->controlAdmission.boundSegmentDescriptor);
        }

        {
            const pbprotocol::ResumeCompletedSegmentRecord& completedRecord = loaded.completedSegments[0];
            const pbprotocol::SegmentDescriptor& completedDescriptor = boundDescriptors[0].GetDescriptor();
            Require(completedRecord.sessionId == describedFile.sessionDescriptor.sessionId &&
                completedRecord.segmentOrdinal == completedDescriptor.segmentOrdinal &&
                completedRecord.rawOffset == completedDescriptor.rawOffset &&
                completedRecord.rawSize == completedDescriptor.rawSize &&
                completedRecord.rawDigest == completedDescriptor.rawDigest,
                "completed resume metadata differs from bound descriptor");
            const auto rawSizeResult = pbprotocol::CheckedUint64ToSize(completedDescriptor.rawSize);
            RequireResult(rawSizeResult, "completed resume raw size cannot narrow");
            Require(completedDescriptor.rawSize <= resourcePolicy.maxRawSegmentBytes,
                "completed resume raw size exceeds policy before file read");
            std::vector<std::byte> committedRaw = ReadFileRange(
                partPath, completedDescriptor.rawOffset, rawSizeResult.Value());
            auto completedVerifyResult = receiver.VerifyResumedStoredSegment(
                completedRecord, std::move(committedRaw));
            RequireResult(completedVerifyResult, "completed resume Segment failed Receiver verification");
            metrics.resumedStoredVerificationCount++;
            RequireResult(receiver.CommitStoredSegment(std::move(completedVerifyResult).Value()),
                "completed resume Segment restoration commit failed");
            metrics.restoredCommitCount++;
        }

        auto managerResult = pbouterfec::OuterFecDecoderResourceManager::Create(
            resourcePolicy);
        RequireResult(managerResult, "resume decoder resource manager failed");
        pbouterfec::OuterFecDecoderResourceManager resourceManager =
            std::move(managerResult).Value();
        {
            const pbprotocol::SegmentDescriptor& descriptor =
                boundDescriptors[1].GetDescriptor();
            auto decoderResult = pbouterfec::WirehairV2Decoder::Create(
                boundDescriptors[1],
                kOuterBlockBytes,
                resourceManager);
            RequireResult(decoderResult, "restart Wirehair decoder failed");
            pbouterfec::WirehairV2Decoder decoder = std::move(decoderResult).Value();
            const auto replayResult = pbreceiver::ReplayActiveWirehairCache(
                loaded.activeWirehairCaches[0],
                decoder);
            RequireResult(replayResult, "Wirehair persisted cache replay failed");
            Require(replayResult.Value().replayedEntryCount == 1 &&
                !replayResult.Value().decoderReady,
                "Wirehair partial replay outcome mismatch");
            const std::vector<std::byte> encodedBytes = ReadFileRange(
                sourcePath,
                descriptor.rawOffset,
                static_cast<std::size_t>(descriptor.rawSize));
            auto encoderResult = pbouterfec::WirehairV2Encoder::Recreate(
                encodedBytes,
                descriptor);
            RequireResult(encoderResult, "restart Wirehair encoder recreate failed");
            pbouterfec::WirehairV2Encoder encoder = std::move(encoderResult).Value();
            CompleteResumedSegment(
                encoder,
                decoder,
                boundDescriptors[1],
                1,
                segmentControls[1],
                pipeline,
                receiver,
                partPath,
                metrics);
        }
        {
            const pbprotocol::SegmentDescriptor& descriptor =
                boundDescriptors[2].GetDescriptor();
            auto decoderResult = pbouterfec::DirectRepeatDecoder::Create(
                boundDescriptors[2],
                kOuterBlockBytes,
                resourceManager);
            RequireResult(decoderResult, "restart DirectRepeat decoder failed");
            pbouterfec::DirectRepeatDecoder decoder = std::move(decoderResult).Value();
            const auto replayResult = pbreceiver::ReplayDirectRepeatBlocks(
                loaded.activeDirectRepeatRecords[0],
                decoder);
            RequireResult(replayResult, "DirectRepeat persisted cache replay failed");
            Require(replayResult.Value().replayedEntryCount == 1 &&
                !replayResult.Value().decoderReady,
                "DirectRepeat partial replay outcome mismatch");
            const std::vector<std::byte> encodedBytes = ReadFileRange(
                sourcePath,
                descriptor.rawOffset,
                static_cast<std::size_t>(descriptor.rawSize));
            auto encoderResult = pbouterfec::DirectRepeatEncoder::Recreate(
                encodedBytes,
                descriptor);
            RequireResult(encoderResult, "restart DirectRepeat encoder recreate failed");
            pbouterfec::DirectRepeatEncoder encoder = std::move(encoderResult).Value();
            CompleteResumedSegment(
                encoder,
                decoder,
                boundDescriptors[2],
                0,
                segmentControls[2],
                pipeline,
                receiver,
                partPath,
                metrics);
        }
        Require(resourceManager.GetActiveDecoderCount() == 0 &&
            resourceManager.GetReservedDecoderBytes() == 0,
            "resume decoder reservations did not roll back on destruction");
        const auto finalizationResult = receiver.PrepareFinalization(sessionTag);
        RequireResult(finalizationResult,
            "restarted receiver did not reach authoritative finalization");
        VerifyAndPublish(
            sourcePath,
            partPath,
            finalPath,
            finalizationResult.Value());
    }

    const auto finished = std::chrono::steady_clock::now();
    const std::uint64_t elapsedMilliseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            finished - started).count());
    evidence.Write(
        "{\"case\":\"resume-state-replay\",\"status\":\"pass\","
        "\"destroyed_receiver_state\":true,\"completed_rehashed\":1,"
        "\"wirehair_cache_entries\":1,\"direct_repeat_cache_entries\":1,"
        "\"torn_tail_reported\":true,\"final_sequential_digest\":true,"
        "\"completed_restore_source\":\"part_raw_bytes\","
        "\"completed_retransmitted\":false,\"restore_redecompressed\":false,"
        "\"completed_codec\":\"" + std::string(compressedCompleted ? "Zstandard" : "RAW") + "\"}");
    evidence.Write(MetricsJson(
        caseSpec,
        metrics,
        describedFile.finalManifest,
        elapsedMilliseconds));
    WriteSegmentEvidence(caseSpec, describedFile, evidence);
    std::filesystem::remove_all(caseRoot);
}

struct CommandLine
{
    std::string mode;
    std::filesystem::path scratchRoot;
    std::filesystem::path evidencePath;
};

[[nodiscard]] CommandLine ParseCommandLine(
    const int argumentCount,
    const char* const arguments[])
{
    CommandLine commandLine;
    for (int argumentIndex = 1;
        argumentIndex < argumentCount;
        argumentIndex++)
    {
        const std::string_view argument(arguments[argumentIndex]);
        Require(argumentIndex + 1 < argumentCount,
            "every Phase 0 Gate option requires a value");
        argumentIndex++;
        const std::string_view value(arguments[argumentIndex]);
        if (argument == "--mode")
        {
            commandLine.mode = value;
        }
        else if (argument == "--scratch")
        {
            commandLine.scratchRoot = value;
        }
        else if (argument == "--evidence")
        {
            commandLine.evidencePath = value;
        }
        else
        {
            throw GateFailure("unknown Phase 0 Gate option: " +
                std::string(argument));
        }
    }
    Require(commandLine.mode == "fast" || commandLine.mode == "large" ||
        commandLine.mode == "resume",
        "--mode must be fast, large, or resume");
    Require(!commandLine.scratchRoot.empty(), "--scratch is required");
    Require(!commandLine.evidencePath.empty(), "--evidence is required");
    return commandLine;
}

} // namespace

int main(const int argumentCount, char* arguments[])
{
    std::optional<CommandLine> commandLine;
    try
    {
        commandLine = ParseCommandLine(argumentCount, arguments);
        std::filesystem::create_directories(commandLine->scratchRoot);
        EvidenceWriter evidence(commandLine->evidencePath);
        evidence.Write(
            "{\"case\":\"phase0-gate-environment\",\"status\":\"pass\","
            "\"source_segment_bytes\":8388608,\"outer_block_bytes\":1314,"
            "\"inner_profile\":\"Robust-K10800\","
            "\"info_bytes\":1350,\"codeword_bytes\":2025,"
            "\"codewords_per_frame\":27,"
            "\"reference_raster\":\"PB-ReferenceRaster-1\"}");
        if (commandLine->mode == "fast")
        {
            for (const auto& record : phase0gate::RunReferenceFrameTests())
            {
                evidence.Write(record);
            }
            RunFastGate(commandLine->scratchRoot, evidence);
        }
        else if (commandLine->mode == "resume")
        {
            RunResumeGate(commandLine->scratchRoot, evidence);
            RunResumeGate(commandLine->scratchRoot, evidence, true);
        }
        else
        {
            RunLargeGate(commandLine->scratchRoot, evidence);
        }
        evidence.Write(
            "{\"case\":\"phase0-gate-summary\",\"status\":\"pass\","
            "\"mode\":\"" + EscapeJson(commandLine->mode) + "\"}");
        return 0;
    }
    catch (const std::exception& error)
    {
        const std::string scratchPath = commandLine
            ? commandLine->scratchRoot.string()
            : std::string("<unparsed>");
        std::cerr << "Phase 0 Gate failed: " << error.what() << '\n'
                  << "scratch retained: " << scratchPath << '\n';
        if (commandLine && !commandLine->evidencePath.empty())
        {
            try
            {
                std::ofstream evidence(
                    commandLine->evidencePath,
                    std::ios::binary | std::ios::app);
                evidence << "{\"case\":\"phase0-gate-summary\","
                         << "\"status\":\"fail\",\"mode\":\""
                         << EscapeJson(commandLine->mode)
                         << "\",\"error\":\""
                         << EscapeJson(error.what()) << "\"}\n";
            }
            catch (...)
            {
            }
        }
        return 1;
    }
}
