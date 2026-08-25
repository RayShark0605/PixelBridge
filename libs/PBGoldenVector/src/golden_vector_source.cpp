#include "pbgolden/golden_vector_source.h"

#include "pbinnerfec/inner_fec_profile.h"
#include "pbinnerfec/qc_ldpc_codec.h"
#include "pbinterleave/interleave_reference.h"
#include "pbmodulation/frame_io.h"
#include "pbmodulation/reference_raster.h"
#include "pbmodulation/reference_visual_profile.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/control_fragment_codec.h"
#include "pbprotocol/descriptor_codec.h"
#include "pbprotocol/protocol_types.h"
#include "pbprotocol/transport_block_codec.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace pbgolden {

namespace {

constexpr std::size_t kBootstrapRecordBytes = 44;
constexpr std::size_t kControlWindowBytes = 240;
constexpr std::size_t kDataRegionBytes = 56168;
// The fixed frame geometry is owned by PBModulation; these local constants
// are cross-checked against the frozen reference profile at compile time so
// the library's public header stays dependency-free.
static_assert(kBootstrapRecordBytes ==
    pbmodulation::kReferenceBootstrapRecordBytes,
    "the golden bootstrap record size must match the reference profile");
static_assert(kControlWindowBytes ==
    pbmodulation::kReferenceControlWindowBytes,
    "the golden control window size must match the reference profile");
static_assert(kDataRegionBytes == pbmodulation::kReferenceDataRegionBytes,
    "the golden data region size must match the reference profile");

// constexpr so the frozen golden byte arrays below stay constant
// expressions (mirrors the PBModulation test helper).
[[nodiscard]] constexpr std::byte Byte(const std::uint8_t value) noexcept
{
    return std::byte{value};
}

void StoreUint16Le(
    const std::span<std::byte> output,
    const std::size_t offset,
    const std::uint16_t value) noexcept
{
    output[offset] = Byte(value & 0xFFU);
    output[offset + 1] = Byte(static_cast<std::uint8_t>(value >> 8));
}

void StoreUint32Le(
    const std::span<std::byte> output,
    const std::size_t offset,
    const std::uint32_t value) noexcept
{
    for (std::size_t byteIndex = 0; byteIndex < 4; byteIndex++)
    {
        output[offset + byteIndex] = Byte(static_cast<std::uint8_t>(
            (value >> static_cast<unsigned int>(byteIndex * 8U)) & 0xFFU));
    }
}

void StoreUint64Le(
    const std::span<std::byte> output,
    const std::size_t offset,
    const std::uint64_t value) noexcept
{
    for (std::size_t byteIndex = 0; byteIndex < 8; byteIndex++)
    {
        output[offset + byteIndex] = Byte(static_cast<std::uint8_t>(
            (value >> static_cast<unsigned int>(byteIndex * 8U)) & 0xFFU));
    }
}

// Deterministic byte pattern: byte i = SplitMix64(seed).Next() & 0xFF.
[[nodiscard]] std::vector<std::byte> MakeBytePattern(
    const std::uint64_t seed,
    const std::size_t byteCount)
{
    std::vector<std::byte> bytes(byteCount);
    SplitMix64 patternRng(seed);
    for (std::size_t byteIndex = 0; byteIndex < byteCount; byteIndex++)
    {
        bytes[byteIndex] =
            Byte(static_cast<std::uint8_t>(patternRng.Next() & 0xFFU));
    }
    return bytes;
}

// Deterministic bit pattern (LSB-first packing): bit i =
// SplitMix64(seed).Next() & 1. skipBits positions of the stream are
// consumed and discarded first. Existing PBInnerFec Golden vectors start
// each profile from bit zero; skipBits remains available for independently
// documented patterns but is zero for the three frozen LDPC vectors.
[[nodiscard]] std::vector<std::byte> TakePatternBits(
    const std::uint64_t seed,
    const std::uint32_t skipBits,
    const std::uint32_t bitCount)
{
    SplitMix64 patternRng(seed);
    patternRng.Skip(skipBits);
    std::vector<std::byte> bits(bitCount / 8u);
    for (std::uint32_t bitIndex = 0; bitIndex < bitCount; bitIndex++)
    {
        if ((patternRng.Next() & 1u) != 0u)
        {
            bits[bitIndex / 8u] |=
                Byte(static_cast<std::uint8_t>(1u << (bitIndex % 8u)));
        }
    }
    return bits;
}

[[noreturn]] void FailGeneration(const char* const detail)
{
    throw std::runtime_error(detail);
}

// Encodes one frozen-profile codeword. Internal failures are propagated to
// RecomputeGoldenVector instead of terminating the process.
[[nodiscard]] std::vector<std::byte> EncodeCodeword(
    const pbinnerfec::InnerFecProfileId profileId,
    const std::span<const std::byte> infoBits)
{
    const auto* profile = pbinnerfec::GetInnerFecProfile(profileId);
    if (profile == nullptr ||
        infoBits.size() != profile->GetInfoByteCount())
    {
        FailGeneration("invalid LDPC profile or information size");
    }
    std::vector<std::byte> codeword(profile->GetCodewordByteCount());
    const auto status = pbinnerfec::EncodeQcLdpcCodeword(
        profileId, infoBits, std::span<std::byte>(codeword));
    if (!status)
    {
        FailGeneration("LDPC encoder rejected the canonical information");
    }
    return codeword;
}

[[nodiscard]] pbprotocol::SessionId MakeGoldenSessionId() noexcept
{
    pbprotocol::SessionId sessionId{};
    for (std::size_t byteIndex = 0; byteIndex < sessionId.bytes.size();
         byteIndex++)
    {
        sessionId.bytes[byteIndex] =
            Byte(static_cast<std::uint8_t>(byteIndex));
    }
    return sessionId;
}

[[nodiscard]] std::array<std::byte, pbprotocol::kDigestBytes>
    MakeDigestBytes(const std::uint8_t firstByte) noexcept
{
    std::array<std::byte, pbprotocol::kDigestBytes> bytes{};
    for (std::size_t byteIndex = 0; byteIndex < bytes.size(); byteIndex++)
    {
        bytes[byteIndex] =
            Byte(static_cast<std::uint8_t>(firstByte + byteIndex));
    }
    return bytes;
}

[[nodiscard]] pbprotocol::BootstrapRecord MakeGoldenBootstrapRecord()
    noexcept
{
    return pbprotocol::BootstrapRecord{
        pbprotocol::kBootstrapVersion,
        pbprotocol::GetProtocolVersion(),
        1,
        0x0102030405060708ULL,
        pbprotocol::SessionTag{kGoldenSessionTag},
        0x1112131415161718ULL,
        0x21222324U,
        0U};
}

[[nodiscard]] std::array<std::byte, kBootstrapRecordBytes>
    MakeGoldenBootstrapBytes()
{
    const auto record = GenerateBootstrapRecord();
    std::array<std::byte, kBootstrapRecordBytes> bytes{};
    std::copy(record.begin(), record.end(), bytes.begin());
    return bytes;
}

[[nodiscard]] pbprotocol::SessionDescriptor MakeGoldenSessionDescriptor()
    noexcept
{
    return pbprotocol::SessionDescriptor{
        pbprotocol::GetProtocolVersion(),
        MakeGoldenSessionId(),
        117ULL,
        1ULL,
        pbprotocol::DigestAlgorithm::Blake3_256};
}

[[nodiscard]] std::array<std::byte, 32> MakeWirehairCanonicalProfile()
    noexcept
{
    std::array<std::byte, 32> profile{};
    // "WHV2" magic, version 1, reserved 0, layout tag 0x0020 (u16 LE).
    profile[0] = Byte(0x57);
    profile[1] = Byte(0x48);
    profile[2] = Byte(0x56);
    profile[3] = Byte(0x32);
    profile[4] = Byte(0x01);
    profile[5] = Byte(0x00);
    profile[6] = Byte(0x20);
    profile[7] = Byte(0x00);
    // Certified 117x16 profile id (little-endian).
    StoreUint64Le(
        std::span<std::byte>(profile), 8,
        pbprotocol::kWirehairV2CertifiedProfileId);
    // messageBytes = 117, blockBytes = 16.
    StoreUint64Le(std::span<std::byte>(profile), 16, 117ULL);
    StoreUint32Le(std::span<std::byte>(profile), 24, 16U);
    // seedAttempt = 0; bytes 29..31 reserved zero.
    return profile;
}

[[nodiscard]] pbprotocol::TransportBlockHeader MakeTransportHeader(
    const std::uint64_t sessionTag,
    const std::uint64_t segmentOrdinal,
    const std::uint32_t outerBlockId,
    const std::uint16_t payloadBytes) noexcept
{
    return pbprotocol::TransportBlockHeader{
        pbprotocol::kTransportBlockTypeData,
        pbprotocol::kTransportProtocolMinor,
        0,
        pbprotocol::SessionTag{sessionTag},
        segmentOrdinal,
        outerBlockId,
        payloadBytes};
}

[[nodiscard]] std::vector<std::byte> SerializeTransportBlockBytes(
    const pbprotocol::TransportBlockHeader& header,
    const std::span<const std::byte> payload)
{
    std::vector<std::byte> buffer(pbprotocol::GetTransportSerializedSize(
        header));
    const auto status = pbprotocol::SerializeTransportBlock(
        header, payload, std::span<std::byte>(buffer));
    if (!status)
    {
        FailGeneration("Transport serialization failed");
    }
    return buffer;
}

// The G1-Transport second block (ordinal 1): the canonical layout with the
// distinct second payload pattern.
[[nodiscard]] std::vector<std::byte> MakeG1TransportSecondBlock()
{
    const auto payload =
        MakeBytePattern(kTransportSecondPayloadSeed, kG1TransportPayloadBytes);
    return SerializeTransportBlockBytes(
        MakeTransportHeader(
            kGoldenSessionTag, 1, 0,
            static_cast<std::uint16_t>(kG1TransportPayloadBytes)),
        payload);
}

// Encodes one Transport block into a 2025-byte Robust codeword.
[[nodiscard]] std::vector<std::byte> MakeTransportRobustCodeword(
    const std::span<const std::byte> infoBlock)
{
    return EncodeCodeword(
        pbinnerfec::kInnerFecProfileIdRobust, infoBlock);
}

void FillDataRegion(
      std::vector<std::byte>& data,
      const std::span<const std::byte> codewordA,
      const std::span<const std::byte> codewordB)
{
    std::copy(codewordA.begin(), codewordA.end(), data.begin());
    if (!codewordB.empty())
    {
        std::copy(
            codewordB.begin(), codewordB.end(),
            data.begin() + codewordA.size());
    }
}

} // namespace

SplitMix64::SplitMix64(const std::uint64_t seed) noexcept : state_(seed)
{
}

std::uint64_t SplitMix64::Next() noexcept
{
    state_ += 0x9E3779B97F4A7C15ULL;
    std::uint64_t mixed = state_;
    mixed = (mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9ULL;
    mixed = (mixed ^ (mixed >> 27)) * 0x94D049BB133111EBULL;
    return mixed ^ (mixed >> 31);
}

std::vector<std::byte> GenerateBootstrapRecord()
{
    std::vector<std::byte> buffer(pbprotocol::kBootstrapRecordBytes);
    const auto status = pbprotocol::SerializeBootstrapRecord(
        MakeGoldenBootstrapRecord(), std::span<std::byte>(buffer));
    if (!status)
    {
        FailGeneration("Bootstrap serialization failed");
    }
    return buffer;
}

std::vector<std::byte> GenerateControlSessionDescriptor()
{
    const auto payload = GenerateSessionDescriptorPayload();
    const pbprotocol::ControlRecordView record{
        pbprotocol::kControlVersion,
        pbprotocol::ControlRecordType::SessionDescriptor,
        0x0102030405060708ULL,
        pbprotocol::SessionTag{kGoldenSessionTag},
        std::span<const std::byte>(payload)};
    const auto sizeResult = pbprotocol::GetSerializedSize(record);
    if (!sizeResult)
    {
        FailGeneration("Control size calculation failed");
    }
    std::vector<std::byte> buffer(sizeResult.Value());
    const auto status =
        pbprotocol::SerializeControlRecord(record, std::span<std::byte>(buffer));
    if (!status)
    {
        FailGeneration("Control serialization failed");
    }
    return buffer;
}

std::vector<std::byte> GenerateControlEmpty()
{
    const pbprotocol::ControlRecordView record{
        pbprotocol::kControlVersion,
        pbprotocol::ControlRecordType::SessionDescriptor,
        0ULL,
        pbprotocol::SessionTag{0},
        std::span<const std::byte>{} };
    std::vector<std::byte> buffer(
        pbprotocol::kMinimumControlRecordBytes);
    const auto status =
        pbprotocol::SerializeControlRecord(record, std::span<std::byte>(buffer));
    if (!status)
    {
        FailGeneration("empty Control serialization failed");
    }
    return buffer;
}

std::vector<std::byte> GenerateControlMaximum()
{
    const std::vector<std::byte> payload(
        pbprotocol::kMaximumControlPayloadBytes);
    const pbprotocol::ControlRecordView record{
        pbprotocol::kControlVersion,
        pbprotocol::ControlRecordType::SessionDescriptor,
        0ULL,
        pbprotocol::SessionTag{0},
        std::span<const std::byte>(payload)};
    const auto sizeResult = pbprotocol::GetSerializedSize(record);
    if (!sizeResult)
    {
        FailGeneration("maximum Control size calculation failed");
    }
    std::vector<std::byte> buffer(sizeResult.Value());
    const auto status =
        pbprotocol::SerializeControlRecord(record, std::span<std::byte>(buffer));
    if (!status)
    {
        FailGeneration("maximum Control serialization failed");
    }
    return buffer;
}

// The fragment golden is defined at the design 8.5 fragment capacity:
// a 24-byte fragment payload splits the 67-byte golden record into the
// frozen 48/48/43-byte fragments (matching the committed corpus seeds).
constexpr std::uint16_t kGoldenFragmentMaxPayloadBytes = 24;

std::vector<std::byte> GenerateControlFragment(
    const std::uint16_t fragmentIndex)
{
    if (fragmentIndex > 2)
    {
        FailGeneration("invalid canonical Control fragment index");
    }
    const auto record = GenerateControlSessionDescriptor();
    const auto fragmentResult = pbprotocol::GetControlFragment(
        0x0102030405060708ULL,
        std::span<const std::byte>(record),
        fragmentIndex,
        kGoldenFragmentMaxPayloadBytes);
    if (!fragmentResult)
    {
        FailGeneration("Control fragmentation failed");
    }
    const auto fragment = fragmentResult.Value();
    const auto sizeResult =
        pbprotocol::GetSerializedSize(fragment);
    if (!sizeResult)
    {
        FailGeneration("Control fragment size calculation failed");
    }
    std::vector<std::byte> buffer(sizeResult.Value());
    const auto status = pbprotocol::SerializeControlFragment(
        fragment, std::span<std::byte>(buffer));
    if (!status)
    {
        FailGeneration("Control fragment serialization failed");
    }
    return buffer;
}

std::vector<std::byte> GenerateSessionDescriptorPayload()
{
    std::vector<std::byte> buffer(
        pbprotocol::kSessionDescriptorPayloadBytes);
    const auto status = pbprotocol::SerializeSessionDescriptor(
        MakeGoldenSessionDescriptor(), std::span<std::byte>(buffer));
    if (!status)
    {
        FailGeneration("SessionDescriptor serialization failed");
    }
    return buffer;
}

std::vector<std::byte> GenerateDirectRepeatSegmentPayload()
{
    const auto sessionDescriptor = MakeGoldenSessionDescriptor();
    const pbprotocol::SegmentDescriptor segment{
        pbprotocol::DeriveSessionTag(sessionDescriptor.sessionId),
        0ULL,
        0ULL,
        117ULL,
        117ULL,
        pbprotocol::CompressionCodec::Raw,
        pbprotocol::OuterFecMode::DirectRepeat,
        16U,
        pbprotocol::RawDigest{MakeDigestBytes(0x20)},
        pbprotocol::EncodedDigest{MakeDigestBytes(0x20)},
        std::nullopt};
    std::vector<std::byte> buffer(
        pbprotocol::kDirectRepeatSegmentDescriptorPayloadBytes);
    const auto status = pbprotocol::SerializeSegmentDescriptor(
        segment, sessionDescriptor, std::span<std::byte>(buffer));
    if (!status)
    {
        FailGeneration("DirectRepeat SegmentDescriptor serialization failed");
    }
    return buffer;
}

std::vector<std::byte> GenerateWirehairSegmentPayload()
{
    // The frozen Wirehair Segment Golden was authored against a one-segment
    // 200-byte Session; its SessionId remains the same as the other descriptor
    // vectors, so the derived SessionTag is byte-identical.
    auto sessionDescriptor = MakeGoldenSessionDescriptor();
    sessionDescriptor.originalFileSize = 200ULL;
    const pbprotocol::SegmentDescriptor segment{
        pbprotocol::DeriveSessionTag(sessionDescriptor.sessionId),
        0ULL,
        0ULL,
        200ULL,
        117ULL,
        pbprotocol::CompressionCodec::Zstandard,
        pbprotocol::OuterFecMode::WirehairV2,
        16U,
        pbprotocol::RawDigest{MakeDigestBytes(0x10)},
        pbprotocol::EncodedDigest{MakeDigestBytes(0x80)},
        pbprotocol::WirehairV2SerializedProfile{
            MakeWirehairCanonicalProfile()}};
    std::vector<std::byte> buffer(
        pbprotocol::kWirehairV2SegmentDescriptorPayloadBytes);
    const auto status = pbprotocol::SerializeSegmentDescriptor(
        segment, sessionDescriptor, std::span<std::byte>(buffer));
    if (!status)
    {
        FailGeneration("Wirehair SegmentDescriptor serialization failed");
    }
    return buffer;
}

std::vector<std::byte> GenerateFinalManifestPayload()
{
    const auto sessionDescriptor = MakeGoldenSessionDescriptor();
    const pbprotocol::FinalManifest manifest{
        sessionDescriptor.sessionId,
        117ULL,
        1ULL,
        pbprotocol::WholeFileDigest{MakeDigestBytes(0xA0)},
        pbprotocol::DigestAlgorithm::Blake3_256};
    std::vector<std::byte> buffer(
        pbprotocol::kFinalManifestPayloadBytes);
    const auto status = pbprotocol::SerializeFinalManifest(
        manifest, sessionDescriptor, std::span<std::byte>(buffer));
    if (!status)
    {
        FailGeneration("FinalManifest serialization failed");
    }
    return buffer;
}

std::vector<std::byte> GenerateWirehairCanonicalDescriptor()
{
    // Bind the profile once: the vector range constructor needs one
    // contiguous range; two separate temporaries would produce a
    // pointer distance across both stack slots.
    const auto profile = MakeWirehairCanonicalProfile();
    return std::vector<std::byte>(profile.begin(), profile.end());
}

std::vector<std::byte> GenerateTransportBlockMinimum()
{
    return SerializeTransportBlockBytes(
        MakeTransportHeader(0ULL, 0ULL, 0U, 0U), {});
}

std::vector<std::byte> GenerateTransportBlockCanonical()
{
    const auto payload =
        MakeBytePattern(kTransportCanonicalPayloadSeed,
            kG1TransportPayloadBytes);
    return SerializeTransportBlockBytes(
        MakeTransportHeader(
            kGoldenSessionTag, 0ULL, 0U,
            static_cast<std::uint16_t>(kG1TransportPayloadBytes)),
        payload);
}

std::vector<std::byte> GenerateTransportBlockMaxPayload()
{
    const auto payload = MakeBytePattern(
        kTransportMaxPayloadSeed, pbprotocol::kMaximumTransportPayloadBytes);
    return SerializeTransportBlockBytes(
        MakeTransportHeader(
            kGoldenSessionTag, 1ULL, 0xFFFFFFFFU,
            static_cast<std::uint16_t>(pbprotocol::kMaximumTransportPayloadBytes)),
        payload);
}

std::vector<std::byte> GenerateLdpcCodewordRobust()
{
    return EncodeCodeword(
        pbinnerfec::kInnerFecProfileIdRobust,
        TakePatternBits(kLdpcPatternSeed, 0, 10800));
}

std::vector<std::byte> GenerateLdpcCodewordBalanced()
{
    return EncodeCodeword(
        pbinnerfec::kInnerFecProfileIdBalanced,
        TakePatternBits(kLdpcPatternSeed, 0, 11880));
}

std::vector<std::byte> GenerateLdpcCodewordFast()
{
    return EncodeCodeword(
        pbinnerfec::kInnerFecProfileIdFast,
        TakePatternBits(kLdpcPatternSeed, 0, 13320));
}

std::vector<std::byte> GenerateInterleaveMappingSample()
{
    std::vector<std::byte> bytes;
    bytes.reserve(16 * 64 * 8);
    for (std::uint64_t phase = 0; phase < pbinterleave::kInterleavePhaseCount;
         phase++)
    {
        for (std::uint64_t logicalTile = 0; logicalTile < 64; logicalTile++)
        {
            const std::uint64_t physicalTile =
                pbinterleave::MapLogicalTileToPhysical(
                    logicalTile, phase);
            bytes.resize(bytes.size() + 4);
            StoreUint32Le(
                std::span<std::byte>(bytes), bytes.size() - 4,
                static_cast<std::uint32_t>(logicalTile));
            bytes.resize(bytes.size() + 4);
            StoreUint32Le(
                std::span<std::byte>(bytes), bytes.size() - 4,
                static_cast<std::uint32_t>(physicalTile));
        }
    }
    return bytes;
}

std::vector<std::byte> GenerateInterleaveRegionLogical()
{
    return MakeBytePattern(
        kInterleaveRegionSeed, pbinterleave::kInterleaveRegionBytes);
}

std::vector<std::byte> GenerateInterleaveRegionPhysicalPhase7()
{
    const auto logical = GenerateInterleaveRegionLogical();
    std::vector<std::byte> physical(pbinterleave::kInterleaveRegionBytes);
    const auto status = pbinterleave::ApplyInterleave(
        std::span<const std::byte>(logical),
        std::span<std::byte>(physical),
        7ULL);
    if (!status)
    {
        FailGeneration("interleave reference application failed");
    }
    return physical;
}

std::vector<std::byte> GenerateG1TransportInfoBlock()
{
    return GenerateTransportBlockCanonical();
}

std::vector<std::byte> GenerateReferenceRasterManifest()
{
    const auto manifest = pbmodulation::SerializeReferenceRegionManifest();
    return std::vector<std::byte>(manifest.begin(), manifest.end());
}

std::vector<std::byte> GenerateG1Transport2CwInfoBlock()
{
    std::vector<std::byte> blockA = GenerateG1TransportInfoBlock();
    const auto blockB = MakeG1TransportSecondBlock();
    blockA.insert(blockA.end(), blockB.begin(), blockB.end());
    return blockA;
}

std::vector<std::byte> GenerateG1TransportDecoded()
{
    const auto payload = MakeG1TransportFramePayload();
    const pbmodulation::ReferenceFrameInput input{
        payload.bootstrap,
        std::span<const std::byte>(payload.control),
        std::span<const std::byte>(payload.data)};
    std::vector<std::byte> bgra(pbmodulation::kReferenceFrameBgraBytes);
    const auto encodeStatus =
        pbmodulation::EncodeReferenceFrame(input, std::span<std::byte>(bgra));
    if (!encodeStatus)
    {
        FailGeneration("reference raster encoding failed");
    }
    const auto frameResult = pbmodulation::DecodeReferenceFrame(bgra);
    if (!frameResult)
    {
        FailGeneration("reference raster decoding failed");
    }
    const std::span<const std::byte> data = frameResult.Value().data;
    const auto codewordProfile =
        pbinnerfec::GetInnerFecProfile(pbinnerfec::kInnerFecProfileIdRobust);
    if (codewordProfile == nullptr ||
        data.size() < codewordProfile->GetCodewordByteCount())
    {
        FailGeneration("Robust codeword profile or raster data is invalid");
    }
    const std::span<const std::byte> codeword =
        data.first(codewordProfile->GetCodewordByteCount());
    const auto syndromeResult = pbinnerfec::ComputeQcLdpcSyndrome(
        pbinnerfec::kInnerFecProfileIdRobust, codeword);
    if (!syndromeResult || !syndromeResult.Value())
    {
        FailGeneration("decoded raster codeword has a nonzero syndrome");
    }
    // Non-const binding: Decode mutates the decoder workspace.
    auto decoderResult = pbinnerfec::QcLdpcDecoder::Create(
        pbinnerfec::kInnerFecProfileIdRobust);
    if (!decoderResult)
    {
        FailGeneration("Robust LDPC decoder creation failed");
    }
    auto& decoder = decoderResult.Value();
    // Perfect-LLR reference channel: the decoder input is the exact
    // transmitted codeword, so each LLR sample has the deterministic
    // reference magnitude 100 (int16, >0 decides bit 0) with the sign of
    // the codeword bit. This is a receiver-local reference choice, not a
    // wire field.
    const std::uint32_t codewordBits =
        codewordProfile->nBits;
    std::vector<std::int16_t> llr(codewordBits);
    for (std::uint32_t bitIndex = 0; bitIndex < codewordBits; bitIndex++)
    {
        const auto byteValue = std::to_integer<std::uint8_t>(
            codeword[bitIndex / 8u]);
        const bool bitSet =
            ((byteValue >> (bitIndex % 8u)) & 0x1U) != 0U;
        llr[bitIndex] = bitSet ? -100 : 100;
    }
    const pbinnerfec::InnerFecDecodeOptions options{16, 1, 2048, 1, 1};
    std::vector<std::byte> decodedCodeword(codewordProfile->GetCodewordByteCount());
    const auto decodeStatus = decoder.Decode(
        std::span<const std::int16_t>(llr),
        options,
        std::span<std::byte>(decodedCodeword));
    if (!decodeStatus)
    {
        FailGeneration("Robust LDPC clean-channel decode failed");
    }
    if (decodedCodeword.size() < kG1TransportInfoBlockBytes)
    {
        FailGeneration("decoded Robust codeword is shorter than its info block");
    }
    const auto blockResult = pbprotocol::ExtractTransportBlockFromInfoBlock(
        std::span<const std::byte>(decodedCodeword).first(
            kG1TransportInfoBlockBytes));
    if (!blockResult)
    {
        FailGeneration("decoded Transport information block extraction failed");
    }
    const auto blockView = blockResult.Value();
    const auto parseResult = pbprotocol::ParseTransportBlock(blockView);
    if (!parseResult)
    {
        FailGeneration("decoded Transport block parse failed");
    }
    return std::vector<std::byte>(blockView.begin(), blockView.end());
}

// The frozen 67-byte golden PB-Control-1 SessionDescriptor record, byte
// identical to kControlGolden in tests/PBModulation/modulation_test_helpers.h
// (itself reused from tests/PBProtocol/test_bootstrap_control_codec.cpp;
// type 1, seq 0x0102030405060708, tag 0x81DF204BD997BAD0, fileSize 117,
// segmentCount 1, CRC 0xA13883C8). The G1 frame pin in docs/REFERENCE_RASTER.md
// section 8.3 is bound to these exact bytes. The file-backed
// control-sessiondescriptor vector is the same canonical record; there is no
// second 64/8 convention to fall back to or re-pin.
constexpr std::array<std::byte, 67> kLegacyControlSessionDescriptorGolden{
    Byte(0x50), Byte(0x42), Byte(0x43), Byte(0x52),
    Byte(0x01), Byte(0x01),
    Byte(0x08), Byte(0x07), Byte(0x06), Byte(0x05),
    Byte(0x04), Byte(0x03), Byte(0x02), Byte(0x01),
    Byte(0xD0), Byte(0xBA), Byte(0x97), Byte(0xD9),
    Byte(0x4B), Byte(0x20), Byte(0xDF), Byte(0x81),
    Byte(0x43), Byte(0x00), Byte(0x00), Byte(0x00),
    Byte(0x01), Byte(0x00), Byte(0x00), Byte(0x00),
    Byte(0x00), Byte(0x01), Byte(0x02), Byte(0x03),
    Byte(0x04), Byte(0x05), Byte(0x06), Byte(0x07),
    Byte(0x08), Byte(0x09), Byte(0x0A), Byte(0x0B),
    Byte(0x0C), Byte(0x0D), Byte(0x0E), Byte(0x0F),
    Byte(0x75), Byte(0x00), Byte(0x00), Byte(0x00),
    Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00),
    Byte(0x01), Byte(0x00), Byte(0x00), Byte(0x00),
    Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00),
    Byte(0x01),
    Byte(0xC8), Byte(0x83), Byte(0x38), Byte(0xA1)};

// Copies the legacy golden control record into the 240-byte window with a
// zero-padded tail, exactly as the documented G1 frame payload does.
static void FillLegacyControlWindow(
    std::array<std::byte, kControlWindowBytes>& control) noexcept
{
    for (std::size_t i = 0;
         i < kLegacyControlSessionDescriptorGolden.size(); i++)
    {
        control[i] = kLegacyControlSessionDescriptorGolden[i];
    }
}

GoldenFramePayload MakeZeroFramePayload()
{
    return GoldenFramePayload{
        std::array<std::byte, kBootstrapRecordBytes>{},
        std::array<std::byte, kControlWindowBytes>{},
        std::vector<std::byte>(kDataRegionBytes)};
}

GoldenFramePayload MakeCanonicalFramePayload()
{
    GoldenFramePayload payload = MakeZeroFramePayload();
    payload.bootstrap = MakeGoldenBootstrapBytes();
    FillLegacyControlWindow(payload.control);
    // The G1 data codeword is the Robust golden codeword over the
    // bit-wise SplitMix64(0xC0FFEE) first 10,800 bits - identical to the
    // existing PBInnerFec/PBModulation golden convention.
    const auto codeword = GenerateLdpcCodewordRobust();
    std::copy(codeword.begin(), codeword.end(), payload.data.begin());
    return payload;
}

GoldenFramePayload MakeMaxFramePayload()
{
    GoldenFramePayload payload;
    for (auto& element : payload.bootstrap)
    {
        element = Byte(0xFF);
    }
    for (auto& element : payload.control)
    {
        element = Byte(0xFF);
    }
    payload.data.assign(kDataRegionBytes, Byte(0xFF));
    return payload;
}

GoldenFramePayload MakeG1TransportFramePayload()
{
    GoldenFramePayload payload = MakeZeroFramePayload();
    payload.bootstrap = MakeGoldenBootstrapBytes();
    FillLegacyControlWindow(payload.control);
    const auto infoBlock = GenerateG1TransportInfoBlock();
    const auto codeword = MakeTransportRobustCodeword(infoBlock);
    FillDataRegion(payload.data, codeword, {});
    return payload;
}

GoldenFramePayload MakeG1Transport2CwFramePayload()
{
    GoldenFramePayload payload = MakeZeroFramePayload();
    payload.bootstrap = MakeGoldenBootstrapBytes();
    FillLegacyControlWindow(payload.control);
    const auto infoBlock = GenerateG1TransportInfoBlock();
    const auto secondBlock = MakeG1TransportSecondBlock();
    const auto codewordA = MakeTransportRobustCodeword(infoBlock);
    const auto codewordB =
        MakeTransportRobustCodeword(std::span<const std::byte>(secondBlock));
    FillDataRegion(payload.data, codewordA, codewordB);
    return payload;
}

namespace {

constexpr std::uint32_t kOracleCanvasWidth = 1920;
constexpr std::uint32_t kOracleCanvasHeight = 1080;
constexpr std::size_t kOraclePixelBytes = 8294400;
constexpr std::size_t kOracleRawHeaderBytes = 28;
constexpr std::uint32_t kOracleDataColumns = 472;
constexpr std::uint32_t kOracleDataRows = 238;
static_assert(kOracleCanvasWidth == pbmodulation::kReferenceCanvasWidth);
static_assert(kOracleCanvasHeight == pbmodulation::kReferenceCanvasHeight);
static_assert(kOraclePixelBytes == pbmodulation::kReferenceFrameBgraBytes);
static_assert(kOracleDataColumns == pbmodulation::kReferenceDataGridColumns);
static_assert(kOracleDataRows == pbmodulation::kReferenceDataGridRows);

void StoreOraclePixel(const std::span<std::byte> bgra,
    const std::uint32_t x, const std::uint32_t y,
    const std::uint8_t luma) noexcept
{
    const std::size_t offset =
        (static_cast<std::size_t>(y) * kOracleCanvasWidth + x) * 4u;
    bgra[offset] = std::byte{luma};
    bgra[offset + 1] = std::byte{luma};
    bgra[offset + 2] = std::byte{luma};
    bgra[offset + 3] = std::byte{255};
}

void FillOracleBlock(const std::span<std::byte> bgra,
    const std::uint32_t originX, const std::uint32_t originY,
    const std::uint32_t width, const std::uint32_t height,
    const std::uint8_t luma) noexcept
{
    for (std::uint32_t row = 0; row < height; row++)
    {
        for (std::uint32_t column = 0; column < width; column++)
        {
            StoreOraclePixel(bgra, originX + column, originY + row, luma);
        }
    }
}

[[nodiscard]] std::uint8_t OracleSymbol(
    const std::span<const std::byte> stream,
    const std::size_t symbolIndex) noexcept
{
    const std::uint8_t value =
        std::to_integer<std::uint8_t>(stream[symbolIndex / 2u]);
    return static_cast<std::uint8_t>((symbolIndex % 2u) == 0u
        ? value & 0x0Fu : (value >> 4u) & 0x0Fu);
}

[[nodiscard]] constexpr std::uint8_t OracleLevel(
    const std::uint8_t symbol) noexcept
{
    const std::uint8_t gray =
        static_cast<std::uint8_t>((symbol ^ (symbol >> 1u)) & 0x0Fu);
    return static_cast<std::uint8_t>(8u + 16u * gray);
}

void FillOracleLane(const std::span<std::byte> bgra,
    const std::uint32_t originY, const std::size_t totalSymbols,
    const std::span<const std::byte> stream,
    const std::size_t symbolOffset) noexcept
{
    const std::size_t streamSymbols = stream.size() * 2u;
    for (std::size_t laneSymbol = 0; laneSymbol < totalSymbols; laneSymbol++)
    {
        const bool carriesStream = laneSymbol >= symbolOffset &&
            laneSymbol - symbolOffset < streamSymbols;
        const std::uint8_t symbol = carriesStream
            ? OracleSymbol(stream, laneSymbol - symbolOffset) : 0;
        const std::uint32_t originX =
            static_cast<std::uint32_t>(laneSymbol % 240u) * 8u;
        const std::uint32_t symbolY = originY +
            static_cast<std::uint32_t>(laneSymbol / 240u) * 8u;
        FillOracleBlock(bgra, originX, symbolY, 8, 8, OracleLevel(symbol));
    }
}

void FillOraclePilot(const std::span<std::byte> bgra,
    const std::uint32_t originY) noexcept
{
    for (std::uint32_t row = 0; row < 16; row++)
    {
        for (std::uint32_t x = 0; x < kOracleCanvasWidth; x++)
        {
            std::uint8_t luma = 128;
            if (x < 256)
            {
                luma = static_cast<std::uint8_t>(8u + 16u * (x / 16u));
            }
            else if (x < 272)
            {
                luma = 0;
            }
            else if (x < 288)
            {
                luma = 255;
            }
            else if (x >= 304 && x < 336)
            {
                luma = ((((x - 304u) / 8u) + (row / 8u)) % 2u) == 0u
                    ? 0 : 255;
            }
            StoreOraclePixel(bgra, x, originY + row, luma);
        }
    }
}

void FillOracleSync(const std::span<std::byte> bgra,
    const std::uint32_t originY) noexcept
{
    for (std::uint32_t row = 0; row < 8; row++)
    {
        const std::uint32_t y = originY + row;
        for (std::uint32_t x = 0; x < kOracleCanvasWidth; x++)
        {
            const std::uint8_t luma = (((x / 8u) + (y / 8u)) % 2u) == 0u
                ? 0 : 255;
            StoreOraclePixel(bgra, x, y, luma);
        }
    }
}

[[nodiscard]] std::vector<std::byte> MakeOracleRaw(
    const std::span<const std::byte> bgra)
{
    std::vector<std::byte> raw(kOracleRawHeaderBytes + bgra.size());
    raw[0] = Byte(0x50);
    raw[1] = Byte(0x42);
    raw[2] = Byte(0x52);
    raw[3] = Byte(0x57);
    raw[4] = std::byte{1};
    StoreUint32Le(raw, 8, kOracleCanvasWidth);
    StoreUint32Le(raw, 12, kOracleCanvasHeight);
    StoreUint32Le(raw, 16, static_cast<std::uint32_t>(bgra.size()));
    std::copy(bgra.begin(), bgra.end(), raw.begin() + kOracleRawHeaderBytes);
    return raw;
}

void RecordFirstDifference(const std::span<const std::byte> expected,
    const std::span<const std::byte> actual, bool& matches,
    std::size_t& offset, std::size_t& differingBytes,
    std::byte& expectedByte, std::byte& actualByte,
    bool& expectedEof, bool& actualEof) noexcept
{
    const std::size_t compared = std::min(expected.size(), actual.size());
    for (std::size_t byteIndex = 0; byteIndex < compared; byteIndex++)
    {
        if (expected[byteIndex] != actual[byteIndex])
        {
            if (differingBytes == 0)
            {
                offset = byteIndex;
                expectedByte = expected[byteIndex];
                actualByte = actual[byteIndex];
            }
            differingBytes++;
        }
    }
    if (expected.size() != actual.size())
    {
        if (differingBytes == 0)
        {
            offset = compared;
            expectedEof = expected.size() == compared;
            actualEof = actual.size() == compared;
            if (!expectedEof)
            {
                expectedByte = expected[compared];
            }
            if (!actualEof)
            {
                actualByte = actual[compared];
            }
        }
        differingBytes += expected.size() > actual.size()
            ? expected.size() - actual.size() : actual.size() - expected.size();
    }
    matches = differingBytes == 0;
}

} // namespace

std::vector<std::byte> GenerateReferenceRasterOracle(
    const GoldenFramePayload& payload)
{
    if (payload.data.size() != kDataRegionBytes)
    {
        throw std::invalid_argument(
            "reference raster oracle requires exactly 56168 data bytes");
    }
    std::vector<std::byte> bgra(kOraclePixelBytes, std::byte{0});
    for (std::size_t pixelIndex = 0;
        pixelIndex < kOraclePixelBytes / 4u; pixelIndex++)
    {
        bgra[pixelIndex * 4u + 3u] = std::byte{255};
    }

    FillOracleSync(bgra, 8);
    FillOracleLane(bgra, 16, 240, payload.bootstrap, 0);
    FillOracleLane(bgra, 24, 480, payload.control, 0);
    FillOraclePilot(bgra, 40);
    for (std::size_t tileIndex = 0; tileIndex < 112336; tileIndex++)
    {
        const std::uint8_t symbol = OracleSymbol(payload.data, tileIndex);
        const std::uint32_t originX = 16u +
            static_cast<std::uint32_t>(tileIndex % kOracleDataColumns) * 4u;
        const std::uint32_t originY = 64u +
            static_cast<std::uint32_t>(tileIndex / kOracleDataColumns) * 4u;
        FillOracleBlock(bgra, originX, originY, 4, 4, OracleLevel(symbol));
    }
    FillOracleSync(bgra, 1024);
    FillOracleLane(bgra, 1032, 240, payload.bootstrap, 152);
    FillOraclePilot(bgra, 1040);
    return bgra;
}

FrameDigestResult ComputeFrameDigests(
    const GoldenFramePayload& payload)
{
    FrameDigestResult result;
    try
    {
        const auto oracleBgra = GenerateReferenceRasterOracle(payload);
        const pbmodulation::ReferenceFrameInput input{
            payload.bootstrap,
            std::span<const std::byte>(payload.control),
            std::span<const std::byte>(payload.data)};
        std::vector<std::byte> bgra(pbmodulation::kReferenceFrameBgraBytes);
        const auto encodeStatus = pbmodulation::EncodeReferenceFrame(
            input, std::span<std::byte>(bgra));
        if (!encodeStatus)
        {
            result.detail = "EncodeReferenceFrame failed: code=" +
                std::to_string(static_cast<int>(encodeStatus.Error().code)) +
                " offset=" + std::to_string(encodeStatus.Error().offset);
            return result;
        }
        const auto rawResult = pbmodulation::EncodeRawFrame(
            std::span<const std::byte>(bgra),
            pbmodulation::kReferenceCanvasWidth,
            pbmodulation::kReferenceCanvasHeight);
        if (!rawResult)
        {
            result.detail = "EncodeRawFrame failed: code=" +
                std::to_string(static_cast<int>(rawResult.Error().code)) +
                " offset=" + std::to_string(rawResult.Error().offset);
            return result;
        }
        const auto pngResult = pbmodulation::EncodePngFrame(
            std::span<const std::byte>(bgra),
            pbmodulation::kReferenceCanvasWidth,
            pbmodulation::kReferenceCanvasHeight);
        if (!pngResult)
        {
            result.detail = "EncodePngFrame failed: code=" +
                std::to_string(static_cast<int>(pngResult.Error().code)) +
                " offset=" + std::to_string(pngResult.Error().offset);
            return result;
        }
        const auto oracleRaw = MakeOracleRaw(oracleBgra);
        result.rawExpectedSize = oracleRaw.size();
        result.rawActualSize = rawResult.Value().size();
        RecordFirstDifference(oracleRaw, rawResult.Value(),
            result.rawOracleMatches, result.rawFirstDifferingOffset,
            result.rawDifferingBytes,
            result.rawExpectedByte, result.rawActualByte,
            result.rawExpectedEof, result.rawActualEof);

        std::vector<std::byte> decodedPng(kOraclePixelBytes);
        const auto decodePngStatus = pbmodulation::DecodePngFrame(
            pngResult.Value(), kOracleCanvasWidth, kOracleCanvasHeight,
            decodedPng);
        if (!decodePngStatus)
        {
            result.detail = "DecodePngFrame failed: code=" +
                std::to_string(static_cast<int>(decodePngStatus.Error().code)) +
                " offset=" + std::to_string(decodePngStatus.Error().offset);
            return result;
        }
        bool ignoredExpectedEof = false;
        bool ignoredActualEof = false;
        RecordFirstDifference(oracleBgra, decodedPng,
            result.pngPixelsMatch, result.pngPixelFirstDifferingOffset,
            result.pngPixelDifferingBytes,
            result.pngPixelExpectedByte, result.pngPixelActualByte,
            ignoredExpectedEof, ignoredActualEof);
        result.rawBlake3 = pbprotocol::ComputeBlake3Digest(
            std::span<const std::byte>(rawResult.Value()));
        result.pngBlake3 = pbprotocol::ComputeBlake3Digest(
            std::span<const std::byte>(pngResult.Value()));
        result.success = true;
        return result;
    }
    catch (const std::exception& exception)
    {
        result.detail = std::string("frame recompute exception: ") + exception.what();
        return result;
    }
    catch (...)
    {
        result.detail = "frame recompute exception: unknown exception";
        return result;
    }
}

std::array<std::byte, 32> ComputeManifestDigest()
{
    const auto manifest =
        pbmodulation::SerializeReferenceRegionManifest();
    return pbprotocol::ComputeBlake3Digest(
        std::span<const std::byte>(manifest));
}

} // namespace pbgolden
