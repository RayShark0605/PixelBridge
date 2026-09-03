#include "descriptor_test_helpers.h"

#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/crc32c.h"
#include "pbprotocol/descriptor_codec.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if !defined(PB_DESCRIPTOR_CORPUS_DIRECTORY)
#error "PB_DESCRIPTOR_CORPUS_DIRECTORY must name the descriptor corpus"
#endif

#if !defined(PB_BOOTSTRAP_CONTROL_CORPUS_DIRECTORY)
#error "PB_BOOTSTRAP_CONTROL_CORPUS_DIRECTORY must name the Control corpus"
#endif

#if !defined(PB_FORMAL_DESCRIPTOR_GOLDEN_MANIFEST)
#error "PB_FORMAL_DESCRIPTOR_GOLDEN_MANIFEST must name the formal Golden manifest"
#endif

namespace {

using pbprotocol::test::Byte;

template <std::unsigned_integral ValueType>
[[nodiscard]] ValueType ReadLittleEndian(
    const std::span<const std::byte> bytes,
    const std::size_t offset)
{
    REQUIRE(offset <= bytes.size());
    REQUIRE(sizeof(ValueType) <= bytes.size() - offset);
    ValueType value = 0;
    for (std::size_t byteIndex = 0; byteIndex < sizeof(ValueType); byteIndex++)
    {
        value |= static_cast<ValueType>(std::to_integer<std::uint8_t>(bytes[offset + byteIndex])) <<
            static_cast<unsigned int>(byteIndex * 8U);
    }
    return value;
}

[[nodiscard]] std::vector<std::byte> ReadFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.is_open());
    const std::vector<char> characters{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    REQUIRE_FALSE(input.bad());

    std::vector<std::byte> bytes(characters.size());
    for (std::size_t byteIndex = 0; byteIndex < characters.size(); byteIndex++)
    {
        bytes[byteIndex] = static_cast<std::byte>(static_cast<unsigned char>(characters[byteIndex]));
    }
    return bytes;
}

[[nodiscard]] std::string ToLowerHex(const std::span<const std::byte> bytes)
{
    constexpr std::array<char, 16> digits{
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string text(bytes.size() * 2U, '0');
    for (std::size_t byteIndex = 0; byteIndex < bytes.size(); byteIndex++)
    {
        const std::uint8_t value = std::to_integer<std::uint8_t>(bytes[byteIndex]);
        text[byteIndex * 2U] = digits[value >> 4U];
        text[byteIndex * 2U + 1U] = digits[value & 0x0FU];
    }
    return text;
}

[[nodiscard]] std::array<std::string_view, 6> SplitManifestEntry(const std::string_view line)
{
    std::array<std::string_view, 6> fields{};
    std::size_t fieldStart = 0;
    for (std::size_t fieldIndex = 0; fieldIndex < fields.size(); fieldIndex++)
    {
        const std::size_t separator = line.find('|', fieldStart);
        if (fieldIndex + 1U == fields.size())
        {
            REQUIRE(separator == std::string_view::npos);
            fields[fieldIndex] = line.substr(fieldStart);
            fieldStart = line.size();
        }
        else
        {
            REQUIRE(separator != std::string_view::npos);
            fields[fieldIndex] = line.substr(fieldStart, separator - fieldStart);
            fieldStart = separator + 1U;
        }
    }
    REQUIRE(fieldStart == line.size());
    return fields;
}

[[nodiscard]] std::uint64_t ParseDecimalUint64(const std::string_view text)
{
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    REQUIRE(result.ec == std::errc{});
    REQUIRE(result.ptr == text.data() + text.size());
    return value;
}

[[nodiscard]] std::filesystem::path ResolveCorpusPath(
    const std::string_view corpus,
    const std::string_view fileName)
{
    REQUIRE(fileName.find('/') == std::string_view::npos);
    REQUIRE(fileName.find('\\') == std::string_view::npos);
    if (corpus == "descriptor-resource")
    {
        return std::filesystem::path(PB_DESCRIPTOR_CORPUS_DIRECTORY) / fileName;
    }
    REQUIRE(corpus == "bootstrap-control");
    return std::filesystem::path(PB_BOOTSTRAP_CONTROL_CORPUS_DIRECTORY) / fileName;
}

void RequireZeroBytes(
    const std::span<const std::byte> bytes,
    const std::size_t offset,
    const std::size_t byteCount)
{
    REQUIRE(offset <= bytes.size());
    REQUIRE(byteCount <= bytes.size() - offset);
    for (std::size_t byteIndex = 0; byteIndex < byteCount; byteIndex++)
    {
        REQUIRE(bytes[offset + byteIndex] == std::byte{0});
    }
}

void RequireDescriptorCrc(const std::span<const std::byte> bytes)
{
    REQUIRE(bytes.size() >= pbprotocol::kDescriptorCrcBytes);
    const std::size_t crcOffset = bytes.size() - pbprotocol::kDescriptorCrcBytes;
    REQUIRE(ReadLittleEndian<std::uint32_t>(bytes, crcOffset) ==
        pbprotocol::ComputeCrc32c(bytes.first(crcOffset)));
}

[[nodiscard]] std::vector<std::byte> SerializeSession(
    const pbprotocol::SessionDescriptor& descriptor,
    const pbprotocol::ReceiverResourcePolicy& resourcePolicy)
{
    const auto sizeResult = pbprotocol::GetSerializedSize(descriptor);
    REQUIRE(sizeResult);
    std::vector<std::byte> bytes(sizeResult.Value());
    REQUIRE(pbprotocol::SerializeSessionDescriptor(descriptor, resourcePolicy, bytes));
    return bytes;
}

[[nodiscard]] std::vector<std::byte> SerializeSegment(
    const pbprotocol::SegmentDescriptor& descriptor,
    const pbprotocol::SessionDescriptor& sessionDescriptor,
    const pbprotocol::ReceiverResourcePolicy& resourcePolicy)
{
    const auto sizeResult = pbprotocol::GetSerializedSize(descriptor);
    REQUIRE(sizeResult);
    std::vector<std::byte> bytes(sizeResult.Value());
    REQUIRE(pbprotocol::SerializeSegmentDescriptor(descriptor, sessionDescriptor, resourcePolicy, bytes));
    return bytes;
}

[[nodiscard]] std::vector<std::byte> SerializeManifest(
    const pbprotocol::FinalManifest& finalManifest,
    const pbprotocol::SessionDescriptor& sessionDescriptor,
    const pbprotocol::ReceiverResourcePolicy& resourcePolicy)
{
    std::vector<std::byte> bytes(pbprotocol::kFinalManifestPayloadBytes);
    REQUIRE(pbprotocol::SerializeFinalManifest(finalManifest, sessionDescriptor, resourcePolicy, bytes));
    return bytes;
}

[[nodiscard]] std::vector<std::byte> WrapControlPayload(
    const pbprotocol::ControlRecordType recordType,
    const pbprotocol::SessionTag sessionTag,
    const std::span<const std::byte> payload)
{
    const pbprotocol::ControlRecordView record{
        pbprotocol::kControlVersion,
        recordType,
        0x0102030405060708ULL,
        sessionTag,
        payload};
    const auto sizeResult = pbprotocol::GetSerializedSize(record);
    REQUIRE(sizeResult);
    std::vector<std::byte> bytes(sizeResult.Value());
    REQUIRE(pbprotocol::SerializeControlRecord(record, bytes));
    return bytes;
}

template <typename ParseFunction>
void RequireExactControlPayload(
    const std::vector<std::byte>& descriptorBytes,
    const pbprotocol::ControlRecordType recordType,
    const pbprotocol::SessionTag sessionTag,
    ParseFunction&& parseFunction)
{
    const std::vector<std::byte> controlBytes = WrapControlPayload(recordType, sessionTag, descriptorBytes);
    const auto controlResult = pbprotocol::ParseControlRecord(controlBytes);
    REQUIRE(controlResult);
    REQUIRE(controlResult.Value().payload.size() == descriptorBytes.size());
    REQUIRE(std::equal(controlResult.Value().payload.begin(), controlResult.Value().payload.end(), descriptorBytes.begin()));
    REQUIRE(ReadLittleEndian<std::uint32_t>(controlResult.Value().payload, pbprotocol::kDescriptorTotalBytesOffset) ==
        controlResult.Value().payload.size());
    REQUIRE(parseFunction(controlResult.Value().payload));

    std::vector<std::byte> trailingDescriptor = descriptorBytes;
    trailingDescriptor.push_back(Byte(0xA5));
    const std::vector<std::byte> trailingControl = WrapControlPayload(recordType, sessionTag, trailingDescriptor);
    const auto trailingControlResult = pbprotocol::ParseControlRecord(trailingControl);
    REQUIRE(trailingControlResult);
    const auto trailingDescriptorResult = parseFunction(trailingControlResult.Value().payload);
    REQUIRE_FALSE(trailingDescriptorResult);
    REQUIRE(trailingDescriptorResult.Error().code == pbprotocol::ProtocolErrorCode::InvalidRecordSize);

    std::vector<std::byte> partialDescriptor = descriptorBytes;
    partialDescriptor.pop_back();
    const std::vector<std::byte> partialControl = WrapControlPayload(recordType, sessionTag, partialDescriptor);
    const auto partialControlResult = pbprotocol::ParseControlRecord(partialControl);
    REQUIRE(partialControlResult);
    const auto partialDescriptorResult = parseFunction(partialControlResult.Value().payload);
    REQUIRE_FALSE(partialDescriptorResult);
    REQUIRE(partialDescriptorResult.Error().code == pbprotocol::ProtocolErrorCode::InvalidRecordSize);
}

} // namespace

TEST_CASE("Formal Descriptor Schema 1 field tables are contiguous and width exact",
          "[pbprotocol][descriptor][schema][layout]")
{
    STATIC_REQUIRE(pbprotocol::kDescriptorSchemaVersionOffset == 0);
    STATIC_REQUIRE(pbprotocol::kDescriptorSchemaVersionOffset + pbprotocol::kDescriptorSchemaVersionBytes ==
        pbprotocol::kDescriptorHeaderBytesOffset);
    STATIC_REQUIRE(pbprotocol::kDescriptorHeaderBytesOffset + pbprotocol::kDescriptorHeaderBytesFieldBytes ==
        pbprotocol::kDescriptorTotalBytesOffset);
    STATIC_REQUIRE(pbprotocol::kDescriptorTotalBytesOffset + pbprotocol::kDescriptorTotalBytesFieldBytes ==
        pbprotocol::kDescriptorSchemaPrefixBytes);

    STATIC_REQUIRE(pbprotocol::kFormalWireSessionDescriptorProtocolMajorOffset == pbprotocol::kDescriptorSchemaPrefixBytes);
    STATIC_REQUIRE(pbprotocol::kFormalWireSessionDescriptorProtocolMajorOffset + pbprotocol::kFormalWireSessionDescriptorProtocolMajorBytes ==
        pbprotocol::kFormalWireSessionDescriptorProtocolMinorOffset);
    STATIC_REQUIRE(pbprotocol::kFormalWireSessionDescriptorProtocolMinorOffset + pbprotocol::kFormalWireSessionDescriptorProtocolMinorBytes ==
        pbprotocol::kFormalWireSessionDescriptorSessionIdOffset);
    STATIC_REQUIRE(pbprotocol::kFormalWireSessionDescriptorSessionIdOffset + pbprotocol::kFormalWireSessionDescriptorSessionIdBytes ==
        pbprotocol::kFormalWireSessionDescriptorVisualProfileIdOffset);
    STATIC_REQUIRE(pbprotocol::kFormalWireSessionDescriptorVisualProfileIdOffset + pbprotocol::kFormalWireSessionDescriptorVisualProfileIdBytes ==
        pbprotocol::kFormalWireSessionDescriptorOriginalFileSizeOffset);
    STATIC_REQUIRE(pbprotocol::kFormalWireSessionDescriptorOriginalFileSizeOffset + pbprotocol::kFormalWireSessionDescriptorOriginalFileSizeBytes ==
        pbprotocol::kFormalWireSessionDescriptorSourceSegmentTargetBytesOffset);
    STATIC_REQUIRE(pbprotocol::kFormalWireSessionDescriptorSourceSegmentTargetBytesOffset + pbprotocol::kFormalWireSessionDescriptorSourceSegmentTargetBytesFieldBytes ==
        pbprotocol::kFormalWireSessionDescriptorSegmentCountOffset);
    STATIC_REQUIRE(pbprotocol::kFormalWireSessionDescriptorSegmentCountOffset + pbprotocol::kFormalWireSessionDescriptorSegmentCountBytes ==
        pbprotocol::kFormalWireSessionDescriptorCompressionPolicyOffset);
    STATIC_REQUIRE(pbprotocol::kFormalWireSessionDescriptorCompressionPolicyOffset + pbprotocol::kFormalWireSessionDescriptorCompressionPolicyBytes ==
        pbprotocol::kFormalWireSessionDescriptorDigestAlgorithmOffset);
    STATIC_REQUIRE(pbprotocol::kFormalWireSessionDescriptorDigestAlgorithmOffset + pbprotocol::kFormalWireSessionDescriptorDigestAlgorithmBytes ==
        pbprotocol::kFormalWireSessionDescriptorReservedOffset);
    STATIC_REQUIRE(pbprotocol::kFormalWireSessionDescriptorReservedOffset + pbprotocol::kFormalWireSessionDescriptorReservedBytes ==
        pbprotocol::kFormalWireSessionDescriptorFeatureFlagsOffset);
    STATIC_REQUIRE(pbprotocol::kFormalWireSessionDescriptorFeatureFlagsOffset + pbprotocol::kFormalWireSessionDescriptorFeatureFlagsBytes ==
        pbprotocol::kFormalWireSessionDescriptorFileNameUtf8BytesOffset);
    STATIC_REQUIRE(pbprotocol::kFormalWireSessionDescriptorFileNameUtf8BytesOffset + pbprotocol::kFormalWireSessionDescriptorFileNameUtf8BytesFieldBytes ==
        pbprotocol::kFormalWireSessionDescriptorFileNameUtf8Offset);
    STATIC_REQUIRE(pbprotocol::kFormalWireSessionDescriptorFileNameUtf8Offset == pbprotocol::kSessionDescriptorHeaderBytes);

    STATIC_REQUIRE(pbprotocol::kFormalWireSegmentDescriptorSessionTagOffset == pbprotocol::kDescriptorSchemaPrefixBytes);
    STATIC_REQUIRE(pbprotocol::kFormalWireSegmentDescriptorSessionTagOffset + pbprotocol::kFormalWireSegmentDescriptorSessionTagBytes ==
        pbprotocol::kFormalWireSegmentDescriptorOrdinalOffset);
    STATIC_REQUIRE(pbprotocol::kFormalWireSegmentDescriptorOrdinalOffset + pbprotocol::kFormalWireSegmentDescriptorOrdinalBytes ==
        pbprotocol::kFormalWireSegmentDescriptorRawOffsetOffset);
    STATIC_REQUIRE(pbprotocol::kFormalWireSegmentDescriptorRawOffsetOffset + pbprotocol::kFormalWireSegmentDescriptorRawOffsetBytes ==
        pbprotocol::kFormalWireSegmentDescriptorRawSizeOffset);
    STATIC_REQUIRE(pbprotocol::kFormalWireSegmentDescriptorRawSizeOffset + pbprotocol::kFormalWireSegmentDescriptorRawSizeBytes ==
        pbprotocol::kFormalWireSegmentDescriptorEncodedSizeOffset);
    STATIC_REQUIRE(pbprotocol::kFormalWireSegmentDescriptorEncodedSizeOffset + pbprotocol::kFormalWireSegmentDescriptorEncodedSizeBytes ==
        pbprotocol::kFormalWireSegmentDescriptorCompressionCodecOffset);
    STATIC_REQUIRE(pbprotocol::kFormalWireSegmentDescriptorCompressionCodecOffset + pbprotocol::kFormalWireSegmentDescriptorCompressionCodecBytes ==
        pbprotocol::kFormalWireSegmentDescriptorOuterFecModeOffset);
    STATIC_REQUIRE(pbprotocol::kFormalWireSegmentDescriptorOuterFecModeOffset + pbprotocol::kFormalWireSegmentDescriptorOuterFecModeBytes ==
        pbprotocol::kFormalWireSegmentDescriptorReservedOffset);
    STATIC_REQUIRE(pbprotocol::kFormalWireSegmentDescriptorReservedOffset + pbprotocol::kFormalWireSegmentDescriptorReservedBytes ==
        pbprotocol::kFormalWireSegmentDescriptorOuterBlockBytesOffset);
    STATIC_REQUIRE(pbprotocol::kFormalWireSegmentDescriptorOuterBlockBytesOffset + pbprotocol::kFormalWireSegmentDescriptorOuterBlockBytesFieldBytes ==
        pbprotocol::kFormalWireSegmentDescriptorRawDigestOffset);
    STATIC_REQUIRE(pbprotocol::kFormalWireSegmentDescriptorRawDigestOffset + pbprotocol::kFormalWireSegmentDescriptorRawDigestBytes ==
        pbprotocol::kFormalWireSegmentDescriptorEncodedDigestOffset);
    STATIC_REQUIRE(pbprotocol::kFormalWireSegmentDescriptorEncodedDigestOffset + pbprotocol::kFormalWireSegmentDescriptorEncodedDigestBytes ==
        pbprotocol::kFormalWireSegmentDescriptorFlagsOffset);
    STATIC_REQUIRE(pbprotocol::kFormalWireSegmentDescriptorFlagsOffset + pbprotocol::kFormalWireSegmentDescriptorFlagsBytes ==
        pbprotocol::kFormalWireSegmentDescriptorWirehairProfileOffset);
    STATIC_REQUIRE(pbprotocol::kFormalWireSegmentDescriptorWirehairProfileOffset == pbprotocol::kSegmentDescriptorHeaderBytes);
    STATIC_REQUIRE(pbprotocol::kSegmentDescriptorHeaderBytes + pbprotocol::kDescriptorCrcBytes ==
        pbprotocol::kDirectRepeatSegmentDescriptorPayloadBytes);
    STATIC_REQUIRE(pbprotocol::kSegmentDescriptorHeaderBytes + pbprotocol::kFormalWireSegmentDescriptorWirehairProfileBytes +
        pbprotocol::kDescriptorCrcBytes == pbprotocol::kWirehairV2SegmentDescriptorPayloadBytes);

    STATIC_REQUIRE(pbprotocol::kFormalWireFinalManifestSessionIdOffset == pbprotocol::kDescriptorSchemaPrefixBytes);
    STATIC_REQUIRE(pbprotocol::kFormalWireFinalManifestSessionIdOffset + pbprotocol::kFormalWireFinalManifestSessionIdBytes ==
        pbprotocol::kFormalWireFinalManifestOriginalFileSizeOffset);
    STATIC_REQUIRE(pbprotocol::kFormalWireFinalManifestOriginalFileSizeOffset + pbprotocol::kFormalWireFinalManifestOriginalFileSizeBytes ==
        pbprotocol::kFormalWireFinalManifestSegmentCountOffset);
    STATIC_REQUIRE(pbprotocol::kFormalWireFinalManifestSegmentCountOffset + pbprotocol::kFormalWireFinalManifestSegmentCountBytes ==
        pbprotocol::kFormalWireFinalManifestWholeFileDigestOffset);
    STATIC_REQUIRE(pbprotocol::kFormalWireFinalManifestWholeFileDigestOffset + pbprotocol::kFormalWireFinalManifestWholeFileDigestBytes ==
        pbprotocol::kFormalWireFinalManifestDigestAlgorithmOffset);
    STATIC_REQUIRE(pbprotocol::kFormalWireFinalManifestDigestAlgorithmOffset + pbprotocol::kFormalWireFinalManifestDigestAlgorithmBytes ==
        pbprotocol::kFormalWireFinalManifestReservedOffset);
    STATIC_REQUIRE(pbprotocol::kFormalWireFinalManifestReservedOffset + pbprotocol::kFormalWireFinalManifestReservedBytes ==
        pbprotocol::kFinalManifestHeaderBytes);
    STATIC_REQUIRE(pbprotocol::kFinalManifestHeaderBytes + pbprotocol::kDescriptorCrcBytes ==
        pbprotocol::kFinalManifestPayloadBytes);

    STATIC_REQUIRE(pbprotocol::kDescriptorTlvTypeOffset + pbprotocol::kDescriptorTlvTypeBytes ==
        pbprotocol::kDescriptorTlvFlagsOffset);
    STATIC_REQUIRE(pbprotocol::kDescriptorTlvFlagsOffset + pbprotocol::kDescriptorTlvFlagsBytes ==
        pbprotocol::kDescriptorTlvValueBytesOffset);
    STATIC_REQUIRE(pbprotocol::kDescriptorTlvValueBytesOffset + pbprotocol::kDescriptorTlvValueBytesFieldBytes ==
        pbprotocol::kDescriptorTlvHeaderBytes);
    STATIC_REQUIRE((pbprotocol::kSessionMandatoryFeatureMask & pbprotocol::kSessionOptionalFeatureMask) == 0);
    STATIC_REQUIRE((pbprotocol::kSessionMandatoryFeatureMask | pbprotocol::kSessionOptionalFeatureMask) ==
        std::numeric_limits<std::uint64_t>::max());
}

TEST_CASE("Formal descriptor serialization matches every published offset width and CRC range",
          "[pbprotocol][descriptor][schema][wire]")
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy = pbprotocol::test::MakeResourcePolicy();
    pbprotocol::SessionDescriptor sessionDescriptor = pbprotocol::test::MakeSessionDescriptor(117, 1);
    sessionDescriptor.sessionVisualProfileId = 0x0102030405060708ULL;
    sessionDescriptor.featureFlags = 1ULL << 63U;
    sessionDescriptor.fileNameUtf8 = "A.bin";
    sessionDescriptor.optionalExtensions = {
        Byte(0x34), Byte(0x12), Byte(0x01), Byte(0x00),
        Byte(0x01), Byte(0x00), Byte(0x00), Byte(0x00), Byte(0xAB)};

    const std::vector<std::byte> sessionBytes = SerializeSession(sessionDescriptor, resourcePolicy);
    REQUIRE(ReadLittleEndian<std::uint16_t>(sessionBytes, pbprotocol::kDescriptorSchemaVersionOffset) ==
        pbprotocol::kDescriptorSchemaVersion);
    REQUIRE(ReadLittleEndian<std::uint16_t>(sessionBytes, pbprotocol::kDescriptorHeaderBytesOffset) ==
        pbprotocol::kSessionDescriptorHeaderBytes);
    REQUIRE(ReadLittleEndian<std::uint32_t>(sessionBytes, pbprotocol::kDescriptorTotalBytesOffset) == sessionBytes.size());
    REQUIRE(ReadLittleEndian<std::uint16_t>(sessionBytes, pbprotocol::kFormalWireSessionDescriptorProtocolMajorOffset) == 1);
    REQUIRE(ReadLittleEndian<std::uint16_t>(sessionBytes, pbprotocol::kFormalWireSessionDescriptorProtocolMinorOffset) == 0);
    REQUIRE(std::equal(sessionDescriptor.sessionId.bytes.begin(), sessionDescriptor.sessionId.bytes.end(),
        sessionBytes.begin() + pbprotocol::kFormalWireSessionDescriptorSessionIdOffset));
    REQUIRE(ReadLittleEndian<std::uint64_t>(sessionBytes, pbprotocol::kFormalWireSessionDescriptorVisualProfileIdOffset) ==
        sessionDescriptor.sessionVisualProfileId);
    REQUIRE(ReadLittleEndian<std::uint64_t>(sessionBytes, pbprotocol::kFormalWireSessionDescriptorOriginalFileSizeOffset) ==
        sessionDescriptor.originalFileSize);
    REQUIRE(ReadLittleEndian<std::uint32_t>(sessionBytes, pbprotocol::kFormalWireSessionDescriptorSourceSegmentTargetBytesOffset) ==
        sessionDescriptor.sourceSegmentTargetBytes);
    REQUIRE(ReadLittleEndian<std::uint64_t>(sessionBytes, pbprotocol::kFormalWireSessionDescriptorSegmentCountOffset) ==
        sessionDescriptor.segmentCount);
    REQUIRE(sessionBytes[pbprotocol::kFormalWireSessionDescriptorCompressionPolicyOffset] == Byte(1));
    REQUIRE(sessionBytes[pbprotocol::kFormalWireSessionDescriptorDigestAlgorithmOffset] == Byte(1));
    RequireZeroBytes(sessionBytes, pbprotocol::kFormalWireSessionDescriptorReservedOffset,
        pbprotocol::kFormalWireSessionDescriptorReservedBytes);
    REQUIRE(ReadLittleEndian<std::uint64_t>(sessionBytes, pbprotocol::kFormalWireSessionDescriptorFeatureFlagsOffset) ==
        sessionDescriptor.featureFlags);
    REQUIRE(ReadLittleEndian<std::uint16_t>(sessionBytes, pbprotocol::kFormalWireSessionDescriptorFileNameUtf8BytesOffset) ==
        sessionDescriptor.fileNameUtf8.size());
    REQUIRE(std::equal(sessionDescriptor.fileNameUtf8.begin(), sessionDescriptor.fileNameUtf8.end(),
        reinterpret_cast<const char*>(sessionBytes.data() + pbprotocol::kFormalWireSessionDescriptorFileNameUtf8Offset)));
    const std::size_t extensionOffset = pbprotocol::kFormalWireSessionDescriptorFileNameUtf8Offset +
        sessionDescriptor.fileNameUtf8.size();
    REQUIRE(ReadLittleEndian<std::uint16_t>(sessionBytes, extensionOffset + pbprotocol::kDescriptorTlvTypeOffset) == 0x1234U);
    REQUIRE(ReadLittleEndian<std::uint16_t>(sessionBytes, extensionOffset + pbprotocol::kDescriptorTlvFlagsOffset) ==
        pbprotocol::kDescriptorTlvOptionalFlag);
    REQUIRE(ReadLittleEndian<std::uint32_t>(sessionBytes, extensionOffset + pbprotocol::kDescriptorTlvValueBytesOffset) == 1U);
    REQUIRE(sessionBytes[extensionOffset + pbprotocol::kDescriptorTlvHeaderBytes] == Byte(0xAB));
    RequireDescriptorCrc(sessionBytes);

    const pbprotocol::SegmentDescriptor directDescriptor =
        pbprotocol::test::MakeDirectRepeatSegment(sessionDescriptor, 0, 0, 117);
    const std::vector<std::byte> directBytes = SerializeSegment(directDescriptor, sessionDescriptor, resourcePolicy);
    REQUIRE(ReadLittleEndian<std::uint16_t>(directBytes, pbprotocol::kDescriptorHeaderBytesOffset) ==
        pbprotocol::kSegmentDescriptorHeaderBytes);
    REQUIRE(ReadLittleEndian<std::uint32_t>(directBytes, pbprotocol::kDescriptorTotalBytesOffset) == directBytes.size());
    REQUIRE(ReadLittleEndian<std::uint64_t>(directBytes, pbprotocol::kFormalWireSegmentDescriptorSessionTagOffset) ==
        directDescriptor.sessionTag.value);
    REQUIRE(ReadLittleEndian<std::uint64_t>(directBytes, pbprotocol::kFormalWireSegmentDescriptorOrdinalOffset) == 0);
    REQUIRE(ReadLittleEndian<std::uint64_t>(directBytes, pbprotocol::kFormalWireSegmentDescriptorRawOffsetOffset) == 0);
    REQUIRE(ReadLittleEndian<std::uint64_t>(directBytes, pbprotocol::kFormalWireSegmentDescriptorRawSizeOffset) == 117);
    REQUIRE(ReadLittleEndian<std::uint64_t>(directBytes, pbprotocol::kFormalWireSegmentDescriptorEncodedSizeOffset) == 117);
    REQUIRE(directBytes[pbprotocol::kFormalWireSegmentDescriptorCompressionCodecOffset] == Byte(1));
    REQUIRE(directBytes[pbprotocol::kFormalWireSegmentDescriptorOuterFecModeOffset] == Byte(2));
    RequireZeroBytes(directBytes, pbprotocol::kFormalWireSegmentDescriptorReservedOffset,
        pbprotocol::kFormalWireSegmentDescriptorReservedBytes);
    REQUIRE(ReadLittleEndian<std::uint32_t>(directBytes, pbprotocol::kFormalWireSegmentDescriptorOuterBlockBytesOffset) == 16);
    REQUIRE(std::equal(directDescriptor.rawDigest.bytes.begin(), directDescriptor.rawDigest.bytes.end(),
        directBytes.begin() + pbprotocol::kFormalWireSegmentDescriptorRawDigestOffset));
    REQUIRE(std::equal(directDescriptor.encodedDigest.bytes.begin(), directDescriptor.encodedDigest.bytes.end(),
        directBytes.begin() + pbprotocol::kFormalWireSegmentDescriptorEncodedDigestOffset));
    REQUIRE(ReadLittleEndian<std::uint64_t>(directBytes, pbprotocol::kFormalWireSegmentDescriptorFlagsOffset) == 0);
    RequireDescriptorCrc(directBytes);

    const pbprotocol::SegmentDescriptor wirehairDescriptor =
        pbprotocol::test::MakeWirehairSegment(sessionDescriptor, 0, 0, 117);
    const std::vector<std::byte> wirehairBytes = SerializeSegment(wirehairDescriptor, sessionDescriptor, resourcePolicy);
    REQUIRE(ReadLittleEndian<std::uint32_t>(wirehairBytes, pbprotocol::kDescriptorTotalBytesOffset) == wirehairBytes.size());
    REQUIRE(wirehairDescriptor.wirehairV2SerializedProfile.has_value());
    REQUIRE(std::equal(wirehairDescriptor.wirehairV2SerializedProfile->bytes.begin(),
        wirehairDescriptor.wirehairV2SerializedProfile->bytes.end(),
        wirehairBytes.begin() + pbprotocol::kFormalWireSegmentDescriptorWirehairProfileOffset));
    RequireDescriptorCrc(wirehairBytes);

    const pbprotocol::FinalManifest finalManifest = pbprotocol::test::MakeFinalManifest(sessionDescriptor);
    const std::vector<std::byte> manifestBytes = SerializeManifest(finalManifest, sessionDescriptor, resourcePolicy);
    REQUIRE(ReadLittleEndian<std::uint16_t>(manifestBytes, pbprotocol::kDescriptorHeaderBytesOffset) ==
        pbprotocol::kFinalManifestHeaderBytes);
    REQUIRE(ReadLittleEndian<std::uint32_t>(manifestBytes, pbprotocol::kDescriptorTotalBytesOffset) == manifestBytes.size());
    REQUIRE(std::equal(finalManifest.sessionId.bytes.begin(), finalManifest.sessionId.bytes.end(),
        manifestBytes.begin() + pbprotocol::kFormalWireFinalManifestSessionIdOffset));
    REQUIRE(ReadLittleEndian<std::uint64_t>(manifestBytes, pbprotocol::kFormalWireFinalManifestOriginalFileSizeOffset) == 117);
    REQUIRE(ReadLittleEndian<std::uint64_t>(manifestBytes, pbprotocol::kFormalWireFinalManifestSegmentCountOffset) == 1);
    REQUIRE(std::equal(finalManifest.wholeFileDigest.bytes.begin(), finalManifest.wholeFileDigest.bytes.end(),
        manifestBytes.begin() + pbprotocol::kFormalWireFinalManifestWholeFileDigestOffset));
    REQUIRE(manifestBytes[pbprotocol::kFormalWireFinalManifestDigestAlgorithmOffset] == Byte(1));
    RequireZeroBytes(manifestBytes, pbprotocol::kFormalWireFinalManifestReservedOffset,
        pbprotocol::kFormalWireFinalManifestReservedBytes);
    RequireDescriptorCrc(manifestBytes);
}

TEST_CASE("DescriptorTotalBytes equals the complete Control payload and rejects partial or trailing bytes",
          "[pbprotocol][descriptor][schema][control][exact]")
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy = pbprotocol::test::MakeResourcePolicy();
    const pbprotocol::SessionDescriptor sessionDescriptor = pbprotocol::test::MakeSessionDescriptor(117, 1);
    const pbprotocol::SessionTag sessionTag = pbprotocol::DeriveSessionTag(sessionDescriptor.sessionId);
    const std::vector<std::byte> sessionBytes = SerializeSession(sessionDescriptor, resourcePolicy);
    RequireExactControlPayload(sessionBytes, pbprotocol::ControlRecordType::SessionDescriptor, sessionTag,
        [&resourcePolicy](const std::span<const std::byte> payload)
        {
            return pbprotocol::ParseSessionDescriptor(payload, resourcePolicy);
        });

    const pbprotocol::SegmentDescriptor directDescriptor =
        pbprotocol::test::MakeDirectRepeatSegment(sessionDescriptor, 0, 0, 117);
    const std::vector<std::byte> segmentBytes = SerializeSegment(directDescriptor, sessionDescriptor, resourcePolicy);
    RequireExactControlPayload(segmentBytes, pbprotocol::ControlRecordType::SegmentDescriptor, sessionTag,
        [&sessionDescriptor, &resourcePolicy](const std::span<const std::byte> payload)
        {
            return pbprotocol::ParseSegmentDescriptor(payload, sessionDescriptor, resourcePolicy);
        });

    const pbprotocol::FinalManifest finalManifest = pbprotocol::test::MakeFinalManifest(sessionDescriptor);
    const std::vector<std::byte> manifestBytes = SerializeManifest(finalManifest, sessionDescriptor, resourcePolicy);
    RequireExactControlPayload(manifestBytes, pbprotocol::ControlRecordType::FinalManifest, sessionTag,
        [&sessionDescriptor, &resourcePolicy](const std::span<const std::byte> payload)
        {
            return pbprotocol::ParseFinalManifest(payload, sessionDescriptor, resourcePolicy);
        });
}

TEST_CASE("Formal descriptor Golden manifest pins every current and legacy fixture identity",
          "[pbprotocol][descriptor][golden][manifest]")
{
    constexpr std::string_view expectedManifestBlake3 =
        "f1908203b4450d8225497e1e55589d7f720cec739eff81ea989f761090660ac3";
    const std::vector<std::byte> manifestBytes = ReadFile(PB_FORMAL_DESCRIPTOR_GOLDEN_MANIFEST);
    REQUIRE(ToLowerHex(pbprotocol::ComputeBlake3Digest(manifestBytes)) == expectedManifestBlake3);

    const std::string manifestText(
        reinterpret_cast<const char*>(manifestBytes.data()),
        manifestBytes.size());
    std::size_t entryCount = 0;
    std::size_t lineStart = 0;
    while (lineStart < manifestText.size())
    {
        const std::size_t lineEnd = manifestText.find('\n', lineStart);
        const std::size_t lineBytes = lineEnd == std::string::npos ? manifestText.size() - lineStart : lineEnd - lineStart;
        std::string_view line(manifestText.data() + lineStart, lineBytes);
        if (!line.empty() && line.back() == '\r')
        {
            line.remove_suffix(1);
        }
        if (!line.empty() && line.front() != '#')
        {
            const auto fields = SplitManifestEntry(line);
            REQUIRE_FALSE(fields[0].empty());
            REQUIRE_FALSE(fields[1].empty());
            REQUIRE_FALSE(fields[2].empty());
            REQUIRE(fields[4].size() == pbprotocol::kDigestBytes * 2U);
            const bool hasKnownDisposition = fields[5] == "Accepted" || fields[5] == "LengthOverflow" ||
                fields[5] == "UnsupportedDescriptorSchema";
            REQUIRE(hasKnownDisposition);
            const std::filesystem::path fixturePath = ResolveCorpusPath(fields[1], fields[2]);
            const std::vector<std::byte> fixtureBytes = ReadFile(fixturePath);
            REQUIRE(fixtureBytes.size() == ParseDecimalUint64(fields[3]));
            REQUIRE(ToLowerHex(pbprotocol::ComputeBlake3Digest(fixtureBytes)) == fields[4]);
            entryCount++;
        }
        if (lineEnd == std::string::npos)
        {
            break;
        }
        lineStart = lineEnd + 1U;
    }
    REQUIRE(entryCount == 11);
}
