#include "descriptor_test_helpers.h"

#include "pbprotocol/crc32c.h"
#include "pbprotocol/descriptor_codec.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace {

using pbprotocol::test::Byte;

void WriteUint16(const std::span<std::byte> bytes, const std::size_t offset, const std::uint16_t value)
{
    REQUIRE(offset + sizeof(value) <= bytes.size());
    bytes[offset] = Byte(static_cast<std::uint8_t>(value & 0xFFU));
    bytes[offset + 1] = Byte(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

void WriteUint32(const std::span<std::byte> bytes, const std::size_t offset, const std::uint32_t value)
{
    REQUIRE(offset + sizeof(value) <= bytes.size());
    for (std::size_t byteIndex = 0; byteIndex < sizeof(value); byteIndex++)
    {
        bytes[offset + byteIndex] = Byte(static_cast<std::uint8_t>((value >> (byteIndex * 8U)) & 0xFFU));
    }
}

void WriteUint64(const std::span<std::byte> bytes, const std::size_t offset, const std::uint64_t value)
{
    REQUIRE(offset + sizeof(value) <= bytes.size());
    for (std::size_t byteIndex = 0; byteIndex < sizeof(value); byteIndex++)
    {
        bytes[offset + byteIndex] = Byte(static_cast<std::uint8_t>((value >> (byteIndex * 8U)) & 0xFFU));
    }
}

void RewriteDescriptorCrc(const std::span<std::byte> bytes)
{
    REQUIRE(bytes.size() >= pbprotocol::kDescriptorCrcBytes);
    const std::size_t crcOffset = bytes.size() - pbprotocol::kDescriptorCrcBytes;
    const std::uint32_t crc = pbprotocol::ComputeCrc32c(std::span<const std::byte>(bytes).first(crcOffset));
    WriteUint32(bytes, crcOffset, crc);
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

[[nodiscard]] std::array<std::byte, pbprotocol::kFinalManifestPayloadBytes> SerializeManifest(
    const pbprotocol::FinalManifest& manifest,
    const pbprotocol::SessionDescriptor& sessionDescriptor,
    const pbprotocol::ReceiverResourcePolicy& resourcePolicy)
{
    std::array<std::byte, pbprotocol::kFinalManifestPayloadBytes> bytes{};
    REQUIRE(pbprotocol::SerializeFinalManifest(manifest, sessionDescriptor, resourcePolicy, bytes));
    return bytes;
}

} // namespace

TEST_CASE("Formal Protocol 1.0 Schema 1 descriptors round-trip all record types",
          "[pbprotocol][descriptor][wire][formal][roundtrip]")
{
    STATIC_REQUIRE(pbprotocol::kDescriptorWireMaturity == pbprotocol::DescriptorWireMaturity::FormalProtocol1Schema1);
    const pbprotocol::ReceiverResourcePolicy resourcePolicy = pbprotocol::test::MakeResourcePolicy();
    const pbprotocol::SessionDescriptor sessionDescriptor = pbprotocol::test::MakeSessionDescriptor(200, 1);
    REQUIRE(pbprotocol::GetSerializedSize(sessionDescriptor));
    REQUIRE(pbprotocol::GetSerializedSize(sessionDescriptor).Value() == pbprotocol::kSessionDescriptorPayloadBytes);

    const std::vector<std::byte> sessionBytes = SerializeSession(sessionDescriptor, resourcePolicy);
    const auto parsedSession = pbprotocol::ParseSessionDescriptor(sessionBytes, resourcePolicy);
    REQUIRE(parsedSession);
    REQUIRE(parsedSession.Value() == sessionDescriptor);

    const pbprotocol::SegmentDescriptor directDescriptor =
        pbprotocol::test::MakeDirectRepeatSegment(sessionDescriptor, 0, 0, 200);
    const std::vector<std::byte> directBytes = SerializeSegment(directDescriptor, sessionDescriptor, resourcePolicy);
    REQUIRE(directBytes.size() == pbprotocol::kDirectRepeatSegmentDescriptorPayloadBytes);
    const auto parsedDirect = pbprotocol::ParseSegmentDescriptor(directBytes, sessionDescriptor, resourcePolicy);
    REQUIRE(parsedDirect);
    REQUIRE(parsedDirect.Value() == directDescriptor);

    const pbprotocol::SegmentDescriptor wirehairDescriptor =
        pbprotocol::test::MakeWirehairSegment(sessionDescriptor, 0, 0, 200);
    const std::vector<std::byte> wirehairBytes = SerializeSegment(wirehairDescriptor, sessionDescriptor, resourcePolicy);
    REQUIRE(wirehairBytes.size() == pbprotocol::kWirehairV2SegmentDescriptorPayloadBytes);
    const auto parsedWirehair = pbprotocol::ParseSegmentDescriptor(wirehairBytes, sessionDescriptor, resourcePolicy);
    REQUIRE(parsedWirehair);
    REQUIRE(parsedWirehair.Value() == wirehairDescriptor);

    const pbprotocol::FinalManifest manifest = pbprotocol::test::MakeFinalManifest(sessionDescriptor);
    const auto manifestBytes = SerializeManifest(manifest, sessionDescriptor, resourcePolicy);
    const auto parsedManifest = pbprotocol::ParseFinalManifest(manifestBytes, sessionDescriptor, resourcePolicy);
    REQUIRE(parsedManifest);
    REQUIRE(parsedManifest.Value() == manifest);
}

TEST_CASE("Formal SessionDescriptor carries filename profile policy and optional TLVs",
          "[pbprotocol][descriptor][wire][session][tlv]")
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy = pbprotocol::test::MakeResourcePolicy();
    pbprotocol::SessionDescriptor descriptor = pbprotocol::test::MakeSessionDescriptor(117, 1);
    descriptor.fileNameUtf8 = "像素桥-数据.bin";
    descriptor.featureFlags = 1ULL << 63U;
    descriptor.optionalExtensions = {
        Byte(0x01), Byte(0x00), Byte(0x01), Byte(0x00),
        Byte(0x03), Byte(0x00), Byte(0x00), Byte(0x00),
        Byte(0xAA), Byte(0xBB), Byte(0xCC)};

    const std::vector<std::byte> bytes = SerializeSession(descriptor, resourcePolicy);
    const auto parsedResult = pbprotocol::ParseSessionDescriptor(bytes, resourcePolicy);
    REQUIRE(parsedResult);
    REQUIRE(parsedResult.Value() == descriptor);

    auto mandatoryExtension = descriptor;
    mandatoryExtension.optionalExtensions[2] = Byte(0x00);
    const auto mandatoryStatus = pbprotocol::ValidateSessionDescriptor(mandatoryExtension, resourcePolicy);
    REQUIRE_FALSE(mandatoryStatus);
    REQUIRE(mandatoryStatus.Error().code == pbprotocol::ProtocolErrorCode::UnknownMandatoryFeature);

    auto duplicateExtension = descriptor;
    duplicateExtension.optionalExtensions.insert(
        duplicateExtension.optionalExtensions.end(),
        descriptor.optionalExtensions.begin(),
        descriptor.optionalExtensions.end());
    const auto duplicateStatus = pbprotocol::ValidateSessionDescriptor(duplicateExtension, resourcePolicy);
    REQUIRE_FALSE(duplicateStatus);
    REQUIRE(duplicateStatus.Error().code == pbprotocol::ProtocolErrorCode::InvalidDescriptor);
}

TEST_CASE("Session filename validation enforces strict UTF-8 Windows basename rules",
          "[pbprotocol][descriptor][filename][security]")
{
    const std::array<std::string, 13> invalidNames{
        "", ".", "..", "../x", "folder\\x", "C:x", "file:stream",
        "CON", "con.txt", "LPT9.bin", "bad?.bin", "trailing.", "trailing "};
    for (const std::string& invalidName : invalidNames)
    {
        CAPTURE(invalidName);
        const auto status = pbprotocol::ValidateFileNameUtf8(invalidName);
        REQUIRE_FALSE(status);
        REQUIRE(status.Error().code == pbprotocol::ProtocolErrorCode::InvalidFileName);
    }

    const std::string invalidUtf8{"bad\xC0\xAF.bin", 9};
    const auto invalidUtf8Status = pbprotocol::ValidateFileNameUtf8(invalidUtf8);
    REQUIRE_FALSE(invalidUtf8Status);
    REQUIRE(invalidUtf8Status.Error().code == pbprotocol::ProtocolErrorCode::InvalidUtf8);
    REQUIRE(pbprotocol::ValidateFileNameUtf8("合法 文件名.数据"));

    pbprotocol::SessionDescriptor maximumNameDescriptor = pbprotocol::test::MakeSessionDescriptor(1, 1);
    maximumNameDescriptor.fileNameUtf8.assign(pbprotocol::kMaximumFileNameUtf8Bytes, 'a');
    const auto maximumSize = pbprotocol::GetSerializedSize(maximumNameDescriptor);
    REQUIRE(maximumSize);
    REQUIRE(maximumSize.Value() == pbprotocol::kSessionDescriptorHeaderBytes +
        pbprotocol::kMaximumFileNameUtf8Bytes + pbprotocol::kDescriptorCrcBytes);
    maximumNameDescriptor.fileNameUtf8.push_back('b');
    const auto excessiveStatus = pbprotocol::ValidateSessionDescriptor(maximumNameDescriptor);
    REQUIRE_FALSE(excessiveStatus);
    REQUIRE(excessiveStatus.Error().code == pbprotocol::ProtocolErrorCode::InvalidFileName);
}

TEST_CASE("Historical 37-byte provisional SessionDescriptor is deterministically rejected",
          "[pbprotocol][descriptor][wire][legacy][regression]")
{
    constexpr std::array<std::byte, 37> phase0Descriptor{
        Byte(0x01), Byte(0x00), Byte(0x00), Byte(0x00),
        Byte(0x00), Byte(0x01), Byte(0x02), Byte(0x03), Byte(0x04), Byte(0x05),
        Byte(0x06), Byte(0x07), Byte(0x08), Byte(0x09), Byte(0x0A), Byte(0x0B),
        Byte(0x0C), Byte(0x0D), Byte(0x0E), Byte(0x0F),
        Byte(0x75), Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00),
        Byte(0x01), Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00),
        Byte(0x01)};
    const auto result = pbprotocol::ParseSessionDescriptor(phase0Descriptor, pbprotocol::test::MakeResourcePolicy());
    REQUIRE_FALSE(result);
    REQUIRE(result.Error() == (pbprotocol::ProtocolError{pbprotocol::ProtocolErrorCode::UnsupportedDescriptorSchema, 2}));
}

TEST_CASE("Descriptor envelope rejects schema length CRC and reserved-field corruption",
          "[pbprotocol][descriptor][wire][malformed]")
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy = pbprotocol::test::MakeResourcePolicy();
    const pbprotocol::SessionDescriptor sessionDescriptor = pbprotocol::test::MakeSessionDescriptor(117, 1);
    const std::vector<std::byte> validBytes = SerializeSession(sessionDescriptor, resourcePolicy);

    for (std::size_t byteCount = 0; byteCount < validBytes.size(); byteCount++)
    {
        CAPTURE(byteCount);
        REQUIRE_FALSE(pbprotocol::ParseSessionDescriptor(
            std::span<const std::byte>(validBytes).first(byteCount), resourcePolicy));
    }

    SECTION("unsupported schema")
    {
        auto bytes = validBytes;
        WriteUint16(bytes, 0, 2);
        RewriteDescriptorCrc(bytes);
        const auto result = pbprotocol::ParseSessionDescriptor(bytes, resourcePolicy);
        REQUIRE_FALSE(result);
        REQUIRE(result.Error().code == pbprotocol::ProtocolErrorCode::UnsupportedDescriptorSchema);
    }
    SECTION("unsupported header")
    {
        auto bytes = validBytes;
        WriteUint16(bytes, 2, 69);
        RewriteDescriptorCrc(bytes);
        const auto result = pbprotocol::ParseSessionDescriptor(bytes, resourcePolicy);
        REQUIRE_FALSE(result);
        REQUIRE(result.Error().code == pbprotocol::ProtocolErrorCode::UnsupportedDescriptorSchema);
    }
    SECTION("total length mismatch")
    {
        auto bytes = validBytes;
        WriteUint32(bytes, 4, static_cast<std::uint32_t>(bytes.size() + 1));
        RewriteDescriptorCrc(bytes);
        const auto result = pbprotocol::ParseSessionDescriptor(bytes, resourcePolicy);
        REQUIRE_FALSE(result);
        REQUIRE(result.Error().code == pbprotocol::ProtocolErrorCode::InvalidRecordSize);
    }
    SECTION("inner CRC mismatch")
    {
        auto bytes = validBytes;
        bytes[pbprotocol::kFormalWireSessionDescriptorSessionIdOffset] ^= Byte(0x80);
        const auto result = pbprotocol::ParseSessionDescriptor(bytes, resourcePolicy);
        REQUIRE_FALSE(result);
        REQUIRE(result.Error().code == pbprotocol::ProtocolErrorCode::CrcMismatch);
    }
    SECTION("reserved bytes")
    {
        auto bytes = validBytes;
        bytes[58] = Byte(1);
        RewriteDescriptorCrc(bytes);
        const auto result = pbprotocol::ParseSessionDescriptor(bytes, resourcePolicy);
        REQUIRE_FALSE(result);
        REQUIRE(result.Error().code == pbprotocol::ProtocolErrorCode::NonZeroReservedByte);
    }
}

TEST_CASE("Descriptor semantic fields are validated after a valid inner CRC",
          "[pbprotocol][descriptor][wire][semantic]")
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy = pbprotocol::test::MakeResourcePolicy();
    const pbprotocol::SessionDescriptor sessionDescriptor = pbprotocol::test::MakeSessionDescriptor(117, 1);

    SECTION("protocol major")
    {
        auto bytes = SerializeSession(sessionDescriptor, resourcePolicy);
        WriteUint16(bytes, pbprotocol::kFormalWireSessionDescriptorProtocolMajorOffset, 2);
        RewriteDescriptorCrc(bytes);
        const auto result = pbprotocol::ParseSessionDescriptor(bytes, resourcePolicy);
        REQUIRE_FALSE(result);
        REQUIRE(result.Error().code == pbprotocol::ProtocolErrorCode::UnsupportedProtocolMajor);
    }
    SECTION("compression policy")
    {
        auto bytes = SerializeSession(sessionDescriptor, resourcePolicy);
        bytes[pbprotocol::kFormalWireSessionDescriptorCompressionPolicyOffset] = Byte(0xFF);
        RewriteDescriptorCrc(bytes);
        const auto result = pbprotocol::ParseSessionDescriptor(bytes, resourcePolicy);
        REQUIRE_FALSE(result);
        REQUIRE(result.Error().code == pbprotocol::ProtocolErrorCode::InvalidEnumValue);
    }
    SECTION("zero visual profile identity")
    {
        auto bytes = SerializeSession(sessionDescriptor, resourcePolicy);
        WriteUint64(bytes, pbprotocol::kFormalWireSessionDescriptorVisualProfileIdOffset, 0);
        RewriteDescriptorCrc(bytes);
        const auto result = pbprotocol::ParseSessionDescriptor(bytes, resourcePolicy);
        REQUIRE_FALSE(result);
        REQUIRE(result.Error().code == pbprotocol::ProtocolErrorCode::InvalidDescriptor);
    }
    SECTION("unknown mandatory feature")
    {
        auto bytes = SerializeSession(sessionDescriptor, resourcePolicy);
        WriteUint64(bytes, pbprotocol::kFormalWireSessionDescriptorFeatureFlagsOffset, 1);
        RewriteDescriptorCrc(bytes);
        const auto result = pbprotocol::ParseSessionDescriptor(bytes, resourcePolicy);
        REQUIRE_FALSE(result);
        REQUIRE(result.Error().code == pbprotocol::ProtocolErrorCode::UnknownMandatoryFeature);
    }
    SECTION("segment checked addition")
    {
        const pbprotocol::SegmentDescriptor segmentDescriptor =
            pbprotocol::test::MakeDirectRepeatSegment(sessionDescriptor, 0, 0, 1, 1);
        auto bytes = SerializeSegment(segmentDescriptor, sessionDescriptor, resourcePolicy);
        WriteUint64(bytes, pbprotocol::kFormalWireSegmentDescriptorRawOffsetOffset,
            std::numeric_limits<std::uint64_t>::max());
        RewriteDescriptorCrc(bytes);
        const auto result = pbprotocol::ParseSegmentDescriptor(bytes, sessionDescriptor, resourcePolicy);
        REQUIRE_FALSE(result);
        REQUIRE(result.Error().code == pbprotocol::ProtocolErrorCode::LengthOverflow);
        REQUIRE(result.Error().offset == pbprotocol::kFormalWireSegmentDescriptorRawOffsetOffset);
    }
    SECTION("segment flags")
    {
        const pbprotocol::SegmentDescriptor segmentDescriptor =
            pbprotocol::test::MakeDirectRepeatSegment(sessionDescriptor, 0, 0, 117);
        auto bytes = SerializeSegment(segmentDescriptor, sessionDescriptor, resourcePolicy);
        WriteUint64(bytes, pbprotocol::kFormalWireSegmentDescriptorFlagsOffset, 1);
        RewriteDescriptorCrc(bytes);
        const auto result = pbprotocol::ParseSegmentDescriptor(bytes, sessionDescriptor, resourcePolicy);
        REQUIRE_FALSE(result);
        REQUIRE(result.Error().code == pbprotocol::ProtocolErrorCode::UnknownMandatoryFeature);
    }
    SECTION("manifest reserved bytes")
    {
        const pbprotocol::FinalManifest manifest = pbprotocol::test::MakeFinalManifest(sessionDescriptor);
        auto bytes = SerializeManifest(manifest, sessionDescriptor, resourcePolicy);
        bytes[73] = Byte(1);
        RewriteDescriptorCrc(bytes);
        const auto result = pbprotocol::ParseFinalManifest(bytes, sessionDescriptor, resourcePolicy);
        REQUIRE_FALSE(result);
        REQUIRE(result.Error().code == pbprotocol::ProtocolErrorCode::NonZeroReservedByte);
    }
}

TEST_CASE("Descriptor serializers are atomic for wrong output sizes and invalid input",
          "[pbprotocol][descriptor][wire][atomic]")
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy = pbprotocol::test::MakeResourcePolicy();
    const pbprotocol::SessionDescriptor sessionDescriptor = pbprotocol::test::MakeSessionDescriptor(117, 1);
    std::array<std::byte, pbprotocol::kSessionDescriptorPayloadBytes - 1> shortOutput{};
    shortOutput.fill(Byte(0xA5));
    const auto originalShortOutput = shortOutput;
    const auto shortStatus = pbprotocol::SerializeSessionDescriptor(sessionDescriptor, resourcePolicy, shortOutput);
    REQUIRE_FALSE(shortStatus);
    REQUIRE(shortStatus.Error().code == pbprotocol::ProtocolErrorCode::InvalidRecordSize);
    REQUIRE(shortOutput == originalShortOutput);

    auto invalidDescriptor = sessionDescriptor;
    invalidDescriptor.fileNameUtf8 = "CON.txt";
    std::array<std::byte, pbprotocol::kSessionDescriptorPayloadBytes> invalidOutput{};
    invalidOutput.fill(Byte(0x5A));
    const auto originalInvalidOutput = invalidOutput;
    const auto invalidStatus = pbprotocol::SerializeSessionDescriptor(invalidDescriptor, resourcePolicy, invalidOutput);
    REQUIRE_FALSE(invalidStatus);
    REQUIRE(invalidStatus.Error().code == pbprotocol::ProtocolErrorCode::InvalidFileName);
    REQUIRE(invalidOutput == originalInvalidOutput);
}
