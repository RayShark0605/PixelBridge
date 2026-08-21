#include "descriptor_test_helpers.h"

#include "pbprotocol/descriptor_codec.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {

using pbprotocol::test::Byte;

constexpr std::array<std::byte, pbprotocol::kSessionDescriptorPayloadBytes>
    kSessionDescriptorGolden{
        Byte(0x01), Byte(0x00), Byte(0x00), Byte(0x00),
        Byte(0x00), Byte(0x01), Byte(0x02), Byte(0x03),
        Byte(0x04), Byte(0x05), Byte(0x06), Byte(0x07),
        Byte(0x08), Byte(0x09), Byte(0x0A), Byte(0x0B),
        Byte(0x0C), Byte(0x0D), Byte(0x0E), Byte(0x0F),
        Byte(0x75), Byte(0x00), Byte(0x00), Byte(0x00),
        Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00),
        Byte(0x01), Byte(0x00), Byte(0x00), Byte(0x00),
        Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00),
        Byte(0x01)};

constexpr std::array<std::byte,
                     pbprotocol::kDirectRepeatSegmentDescriptorPayloadBytes>
    kDirectSegmentGolden{
        Byte(0xD0), Byte(0xBA), Byte(0x97), Byte(0xD9),
        Byte(0x4B), Byte(0x20), Byte(0xDF), Byte(0x81),
        Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00),
        Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00),
        Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00),
        Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00),
        Byte(0x75), Byte(0x00), Byte(0x00), Byte(0x00),
        Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00),
        Byte(0x75), Byte(0x00), Byte(0x00), Byte(0x00),
        Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00),
        Byte(0x01), Byte(0x02), Byte(0x10), Byte(0x00),
        Byte(0x00), Byte(0x00),
        Byte(0x20), Byte(0x21), Byte(0x22), Byte(0x23),
        Byte(0x24), Byte(0x25), Byte(0x26), Byte(0x27),
        Byte(0x28), Byte(0x29), Byte(0x2A), Byte(0x2B),
        Byte(0x2C), Byte(0x2D), Byte(0x2E), Byte(0x2F),
        Byte(0x30), Byte(0x31), Byte(0x32), Byte(0x33),
        Byte(0x34), Byte(0x35), Byte(0x36), Byte(0x37),
        Byte(0x38), Byte(0x39), Byte(0x3A), Byte(0x3B),
        Byte(0x3C), Byte(0x3D), Byte(0x3E), Byte(0x3F),
        Byte(0x20), Byte(0x21), Byte(0x22), Byte(0x23),
        Byte(0x24), Byte(0x25), Byte(0x26), Byte(0x27),
        Byte(0x28), Byte(0x29), Byte(0x2A), Byte(0x2B),
        Byte(0x2C), Byte(0x2D), Byte(0x2E), Byte(0x2F),
        Byte(0x30), Byte(0x31), Byte(0x32), Byte(0x33),
        Byte(0x34), Byte(0x35), Byte(0x36), Byte(0x37),
        Byte(0x38), Byte(0x39), Byte(0x3A), Byte(0x3B),
        Byte(0x3C), Byte(0x3D), Byte(0x3E), Byte(0x3F)};

constexpr std::array<std::byte,
                     pbprotocol::kWirehairV2SegmentDescriptorPayloadBytes>
    kWirehairSegmentGolden{
        Byte(0xD0), Byte(0xBA), Byte(0x97), Byte(0xD9),
        Byte(0x4B), Byte(0x20), Byte(0xDF), Byte(0x81),
        Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00),
        Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00),
        Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00),
        Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00),
        Byte(0xC8), Byte(0x00), Byte(0x00), Byte(0x00),
        Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00),
        Byte(0x75), Byte(0x00), Byte(0x00), Byte(0x00),
        Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00),
        Byte(0x02), Byte(0x01), Byte(0x10), Byte(0x00),
        Byte(0x00), Byte(0x00),
        Byte(0x10), Byte(0x11), Byte(0x12), Byte(0x13),
        Byte(0x14), Byte(0x15), Byte(0x16), Byte(0x17),
        Byte(0x18), Byte(0x19), Byte(0x1A), Byte(0x1B),
        Byte(0x1C), Byte(0x1D), Byte(0x1E), Byte(0x1F),
        Byte(0x20), Byte(0x21), Byte(0x22), Byte(0x23),
        Byte(0x24), Byte(0x25), Byte(0x26), Byte(0x27),
        Byte(0x28), Byte(0x29), Byte(0x2A), Byte(0x2B),
        Byte(0x2C), Byte(0x2D), Byte(0x2E), Byte(0x2F),
        Byte(0x80), Byte(0x81), Byte(0x82), Byte(0x83),
        Byte(0x84), Byte(0x85), Byte(0x86), Byte(0x87),
        Byte(0x88), Byte(0x89), Byte(0x8A), Byte(0x8B),
        Byte(0x8C), Byte(0x8D), Byte(0x8E), Byte(0x8F),
        Byte(0x90), Byte(0x91), Byte(0x92), Byte(0x93),
        Byte(0x94), Byte(0x95), Byte(0x96), Byte(0x97),
        Byte(0x98), Byte(0x99), Byte(0x9A), Byte(0x9B),
        Byte(0x9C), Byte(0x9D), Byte(0x9E), Byte(0x9F),
        Byte(0x57), Byte(0x48), Byte(0x56), Byte(0x32),
        Byte(0x01), Byte(0x00), Byte(0x20), Byte(0x00),
        Byte(0xC9), Byte(0xF9), Byte(0xF4), Byte(0x47),
        Byte(0xBB), Byte(0x5B), Byte(0x29), Byte(0x4B),
        Byte(0x75), Byte(0x00), Byte(0x00), Byte(0x00),
        Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00),
        Byte(0x10), Byte(0x00), Byte(0x00), Byte(0x00),
        Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00)};

constexpr std::array<std::byte, pbprotocol::kFinalManifestPayloadBytes>
    kFinalManifestGolden{
        Byte(0x00), Byte(0x01), Byte(0x02), Byte(0x03),
        Byte(0x04), Byte(0x05), Byte(0x06), Byte(0x07),
        Byte(0x08), Byte(0x09), Byte(0x0A), Byte(0x0B),
        Byte(0x0C), Byte(0x0D), Byte(0x0E), Byte(0x0F),
        Byte(0x75), Byte(0x00), Byte(0x00), Byte(0x00),
        Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00),
        Byte(0x01), Byte(0x00), Byte(0x00), Byte(0x00),
        Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00),
        Byte(0xA0), Byte(0xA1), Byte(0xA2), Byte(0xA3),
        Byte(0xA4), Byte(0xA5), Byte(0xA6), Byte(0xA7),
        Byte(0xA8), Byte(0xA9), Byte(0xAA), Byte(0xAB),
        Byte(0xAC), Byte(0xAD), Byte(0xAE), Byte(0xAF),
        Byte(0xB0), Byte(0xB1), Byte(0xB2), Byte(0xB3),
        Byte(0xB4), Byte(0xB5), Byte(0xB6), Byte(0xB7),
        Byte(0xB8), Byte(0xB9), Byte(0xBA), Byte(0xBB),
        Byte(0xBC), Byte(0xBD), Byte(0xBE), Byte(0xBF),
        Byte(0x01)};

} // namespace

TEST_CASE("Descriptor payloads match canonical golden bytes",
          "[pbprotocol][descriptor][wire][golden]")
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::test::MakeResourcePolicy();
    const pbprotocol::SessionDescriptor sessionDescriptor =
        pbprotocol::test::MakeSessionDescriptor(117, 1);

    std::array<std::byte, pbprotocol::kSessionDescriptorPayloadBytes>
        sessionBytes{};
    REQUIRE(pbprotocol::SerializeSessionDescriptor(
        sessionDescriptor,
        resourcePolicy,
        sessionBytes));
    REQUIRE(sessionBytes == kSessionDescriptorGolden);
    const auto parsedSession = pbprotocol::ParseSessionDescriptor(
        sessionBytes,
        resourcePolicy);
    REQUIRE(parsedSession);
    REQUIRE(parsedSession.Value() == sessionDescriptor);

    const pbprotocol::SegmentDescriptor segmentDescriptor =
        pbprotocol::test::MakeDirectRepeatSegment(
            sessionDescriptor,
            0,
            0,
            117);
    std::array<std::byte,
               pbprotocol::kDirectRepeatSegmentDescriptorPayloadBytes>
        segmentBytes{};
    REQUIRE(pbprotocol::SerializeSegmentDescriptor(
        segmentDescriptor,
        sessionDescriptor,
        resourcePolicy,
        segmentBytes));
    REQUIRE(segmentBytes == kDirectSegmentGolden);
    const auto parsedSegment = pbprotocol::ParseSegmentDescriptor(
        segmentBytes,
        sessionDescriptor,
        resourcePolicy);
    REQUIRE(parsedSegment);
    REQUIRE(parsedSegment.Value() == segmentDescriptor);

    const pbprotocol::FinalManifest finalManifest =
        pbprotocol::test::MakeFinalManifest(sessionDescriptor);
    std::array<std::byte, pbprotocol::kFinalManifestPayloadBytes>
        finalManifestBytes{};
    REQUIRE(pbprotocol::SerializeFinalManifest(
        finalManifest,
        sessionDescriptor,
        resourcePolicy,
        finalManifestBytes));
    REQUIRE(finalManifestBytes == kFinalManifestGolden);
    const auto parsedFinalManifest = pbprotocol::ParseFinalManifest(
        finalManifestBytes,
        sessionDescriptor,
        resourcePolicy);
    REQUIRE(parsedFinalManifest);
    REQUIRE(parsedFinalManifest.Value() == finalManifest);
}

TEST_CASE("Wirehair Segment payload appends the canonical 32-byte profile",
          "[pbprotocol][descriptor][wire][wirehair][golden]")
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::test::MakeResourcePolicy();
    const pbprotocol::SessionDescriptor sessionDescriptor =
        pbprotocol::test::MakeSessionDescriptor(200, 1);
    const pbprotocol::SegmentDescriptor segmentDescriptor =
        pbprotocol::test::MakeWirehairSegment(
            sessionDescriptor,
            0,
            0,
            200);

    const auto serializedSize = pbprotocol::GetSerializedSize(segmentDescriptor);
    REQUIRE(serializedSize);
    REQUIRE(
        serializedSize.Value() ==
        pbprotocol::kWirehairV2SegmentDescriptorPayloadBytes);

    std::array<std::byte,
               pbprotocol::kWirehairV2SegmentDescriptorPayloadBytes>
        serializedBytes{};
    REQUIRE(pbprotocol::SerializeSegmentDescriptor(
        segmentDescriptor,
        sessionDescriptor,
        resourcePolicy,
        serializedBytes));
    REQUIRE(serializedBytes == kWirehairSegmentGolden);
    REQUIRE(std::equal(
        segmentDescriptor.wirehairV2SerializedProfile->bytes.begin(),
        segmentDescriptor.wirehairV2SerializedProfile->bytes.end(),
        serializedBytes.begin() +
            pbprotocol::kDirectRepeatSegmentDescriptorPayloadBytes));

    const auto parsedDescriptor = pbprotocol::ParseSegmentDescriptor(
        serializedBytes,
        sessionDescriptor,
        resourcePolicy);
    REQUIRE(parsedDescriptor);
    REQUIRE(parsedDescriptor.Value() == segmentDescriptor);
}

TEST_CASE("Descriptor parsers reject every truncated prefix and trailing byte",
          "[pbprotocol][descriptor][wire][truncation]")
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::test::MakeResourcePolicy();
    const pbprotocol::SessionDescriptor directSession =
        pbprotocol::test::MakeSessionDescriptor(117, 1);
    const pbprotocol::SegmentDescriptor directSegment =
        pbprotocol::test::MakeDirectRepeatSegment(directSession, 0, 0, 117);
    const pbprotocol::FinalManifest finalManifest =
        pbprotocol::test::MakeFinalManifest(directSession);
    const pbprotocol::SessionDescriptor wirehairSession =
        pbprotocol::test::MakeSessionDescriptor(200, 1);
    const pbprotocol::SegmentDescriptor wirehairSegment =
        pbprotocol::test::MakeWirehairSegment(
            wirehairSession,
            0,
            0,
            200);

    std::array<std::byte, pbprotocol::kSessionDescriptorPayloadBytes>
        sessionBytes{};
    std::array<std::byte,
               pbprotocol::kDirectRepeatSegmentDescriptorPayloadBytes>
        segmentBytes{};
    std::array<std::byte, pbprotocol::kFinalManifestPayloadBytes> finalBytes{};
    std::array<std::byte,
               pbprotocol::kWirehairV2SegmentDescriptorPayloadBytes>
        wirehairBytes{};
    REQUIRE(pbprotocol::SerializeSessionDescriptor(
        directSession,
        resourcePolicy,
        sessionBytes));
    REQUIRE(pbprotocol::SerializeSegmentDescriptor(
        directSegment,
        directSession,
        resourcePolicy,
        segmentBytes));
    REQUIRE(pbprotocol::SerializeFinalManifest(
        finalManifest,
        directSession,
        resourcePolicy,
        finalBytes));
    REQUIRE(pbprotocol::SerializeSegmentDescriptor(
        wirehairSegment,
        wirehairSession,
        resourcePolicy,
        wirehairBytes));

    for (std::size_t prefixBytes = 0;
         prefixBytes < sessionBytes.size();
         prefixBytes++)
    {
        CAPTURE(prefixBytes);
        REQUIRE_FALSE(pbprotocol::ParseSessionDescriptor(
            std::span<const std::byte>(sessionBytes).first(prefixBytes),
            resourcePolicy));
    }
    for (std::size_t prefixBytes = 0;
         prefixBytes < segmentBytes.size();
         prefixBytes++)
    {
        CAPTURE(prefixBytes);
        REQUIRE_FALSE(pbprotocol::ParseSegmentDescriptor(
            std::span<const std::byte>(segmentBytes).first(prefixBytes),
            directSession,
            resourcePolicy));
    }
    for (std::size_t prefixBytes = 0;
         prefixBytes < finalBytes.size();
         prefixBytes++)
    {
        CAPTURE(prefixBytes);
        REQUIRE_FALSE(pbprotocol::ParseFinalManifest(
            std::span<const std::byte>(finalBytes).first(prefixBytes),
            directSession,
            resourcePolicy));
    }
    for (std::size_t prefixBytes = 0;
         prefixBytes < wirehairBytes.size();
         prefixBytes++)
    {
        CAPTURE(prefixBytes);
        REQUIRE_FALSE(pbprotocol::ParseSegmentDescriptor(
            std::span<const std::byte>(wirehairBytes).first(prefixBytes),
            wirehairSession,
            resourcePolicy));
    }

    std::vector<std::byte> overlongSession(sessionBytes.begin(), sessionBytes.end());
    overlongSession.push_back(Byte(0));
    const auto overlongSessionResult = pbprotocol::ParseSessionDescriptor(
        overlongSession,
        resourcePolicy);
    REQUIRE_FALSE(overlongSessionResult);
    REQUIRE(
        overlongSessionResult.Error().code ==
        pbprotocol::ProtocolErrorCode::TrailingBytes);

    std::vector<std::byte> overlongSegment(segmentBytes.begin(), segmentBytes.end());
    overlongSegment.push_back(Byte(0));
    const auto overlongSegmentResult = pbprotocol::ParseSegmentDescriptor(
        overlongSegment,
        directSession,
        resourcePolicy);
    REQUIRE_FALSE(overlongSegmentResult);
    REQUIRE(
        overlongSegmentResult.Error().code ==
        pbprotocol::ProtocolErrorCode::TrailingBytes);

    std::vector<std::byte> overlongFinal(finalBytes.begin(), finalBytes.end());
    overlongFinal.push_back(Byte(0));
    const auto overlongFinalResult = pbprotocol::ParseFinalManifest(
        overlongFinal,
        directSession,
        resourcePolicy);
    REQUIRE_FALSE(overlongFinalResult);
    REQUIRE(
        overlongFinalResult.Error().code ==
        pbprotocol::ProtocolErrorCode::TrailingBytes);

    std::vector<std::byte> overlongWirehair(
        wirehairBytes.begin(),
        wirehairBytes.end());
    overlongWirehair.push_back(Byte(0));
    const auto overlongWirehairResult =
        pbprotocol::ParseSegmentDescriptor(
            overlongWirehair,
            wirehairSession,
            resourcePolicy);
    REQUIRE_FALSE(overlongWirehairResult);
    REQUIRE(
        overlongWirehairResult.Error().code ==
        pbprotocol::ProtocolErrorCode::TrailingBytes);
}

TEST_CASE("Descriptor serializers require exact buffers and preserve failures",
          "[pbprotocol][descriptor][wire][atomic]")
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::test::MakeResourcePolicy();
    const pbprotocol::SessionDescriptor sessionDescriptor =
        pbprotocol::test::MakeSessionDescriptor(117, 1);
    const pbprotocol::SegmentDescriptor segmentDescriptor =
        pbprotocol::test::MakeDirectRepeatSegment(
            sessionDescriptor,
            0,
            0,
            117);
    const pbprotocol::SegmentDescriptor wirehairDescriptor =
        pbprotocol::test::MakeWirehairSegment(
            sessionDescriptor,
            0,
            0,
            117);
    const pbprotocol::FinalManifest finalManifest =
        pbprotocol::test::MakeFinalManifest(sessionDescriptor);

    std::array<std::byte, pbprotocol::kSessionDescriptorPayloadBytes - 1>
        shortSessionOutput{};
    shortSessionOutput.fill(Byte(0xA5));
    const auto originalShortSessionOutput = shortSessionOutput;
    REQUIRE_FALSE(pbprotocol::SerializeSessionDescriptor(
        sessionDescriptor,
        resourcePolicy,
        shortSessionOutput));
    REQUIRE(shortSessionOutput == originalShortSessionOutput);

    std::array<std::byte, pbprotocol::kSessionDescriptorPayloadBytes + 1>
        longSessionOutput{};
    longSessionOutput.fill(Byte(0x5A));
    const auto originalLongSessionOutput = longSessionOutput;
    REQUIRE_FALSE(pbprotocol::SerializeSessionDescriptor(
        sessionDescriptor,
        resourcePolicy,
        longSessionOutput));
    REQUIRE(longSessionOutput == originalLongSessionOutput);

    std::array<std::byte,
               pbprotocol::kDirectRepeatSegmentDescriptorPayloadBytes - 1>
        shortOutput{};
    shortOutput.fill(Byte(0xA5));
    const auto originalShortOutput = shortOutput;
    const auto shortStatus = pbprotocol::SerializeSegmentDescriptor(
        segmentDescriptor,
        sessionDescriptor,
        resourcePolicy,
        shortOutput);
    REQUIRE_FALSE(shortStatus);
    REQUIRE(
        shortStatus.Error().code ==
        pbprotocol::ProtocolErrorCode::InvalidRecordSize);
    REQUIRE(shortOutput == originalShortOutput);

    std::array<std::byte,
               pbprotocol::kDirectRepeatSegmentDescriptorPayloadBytes + 1>
        longOutput{};
    longOutput.fill(Byte(0x5A));
    const auto originalLongOutput = longOutput;
    const auto longStatus = pbprotocol::SerializeSegmentDescriptor(
        segmentDescriptor,
        sessionDescriptor,
        resourcePolicy,
        longOutput);
    REQUIRE_FALSE(longStatus);
    REQUIRE(
        longStatus.Error().code ==
        pbprotocol::ProtocolErrorCode::InvalidRecordSize);
    REQUIRE(longOutput == originalLongOutput);

    std::array<std::byte,
               pbprotocol::kWirehairV2SegmentDescriptorPayloadBytes - 1>
        shortWirehairOutput{};
    shortWirehairOutput.fill(Byte(0xA5));
    const auto originalShortWirehairOutput = shortWirehairOutput;
    REQUIRE_FALSE(pbprotocol::SerializeSegmentDescriptor(
        wirehairDescriptor,
        sessionDescriptor,
        resourcePolicy,
        shortWirehairOutput));
    REQUIRE(shortWirehairOutput == originalShortWirehairOutput);

    std::array<std::byte,
               pbprotocol::kWirehairV2SegmentDescriptorPayloadBytes + 1>
        longWirehairOutput{};
    longWirehairOutput.fill(Byte(0x5A));
    const auto originalLongWirehairOutput = longWirehairOutput;
    REQUIRE_FALSE(pbprotocol::SerializeSegmentDescriptor(
        wirehairDescriptor,
        sessionDescriptor,
        resourcePolicy,
        longWirehairOutput));
    REQUIRE(longWirehairOutput == originalLongWirehairOutput);

    std::array<std::byte, pbprotocol::kFinalManifestPayloadBytes - 1>
        shortFinalOutput{};
    shortFinalOutput.fill(Byte(0xA5));
    const auto originalShortFinalOutput = shortFinalOutput;
    REQUIRE_FALSE(pbprotocol::SerializeFinalManifest(
        finalManifest,
        sessionDescriptor,
        resourcePolicy,
        shortFinalOutput));
    REQUIRE(shortFinalOutput == originalShortFinalOutput);

    std::array<std::byte, pbprotocol::kFinalManifestPayloadBytes + 1>
        longFinalOutput{};
    longFinalOutput.fill(Byte(0x5A));
    const auto originalLongFinalOutput = longFinalOutput;
    REQUIRE_FALSE(pbprotocol::SerializeFinalManifest(
        finalManifest,
        sessionDescriptor,
        resourcePolicy,
        longFinalOutput));
    REQUIRE(longFinalOutput == originalLongFinalOutput);

    pbprotocol::SegmentDescriptor invalidDescriptor = segmentDescriptor;
    invalidDescriptor.encodedSize++;
    std::array<std::byte,
               pbprotocol::kDirectRepeatSegmentDescriptorPayloadBytes>
        invalidOutput{};
    invalidOutput.fill(Byte(0x3C));
    const auto originalInvalidOutput = invalidOutput;
    REQUIRE_FALSE(pbprotocol::SerializeSegmentDescriptor(
        invalidDescriptor,
        sessionDescriptor,
        resourcePolicy,
        invalidOutput));
    REQUIRE(invalidOutput == originalInvalidOutput);
}

TEST_CASE("Unknown descriptor enum values fail closed",
          "[pbprotocol][descriptor][wire][enum]")
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::test::MakeResourcePolicy();
    const pbprotocol::SessionDescriptor sessionDescriptor =
        pbprotocol::test::MakeSessionDescriptor(117, 1);

    constexpr std::array<std::uint8_t, 2> invalidEnumValues{0, 0xFF};
    for (const std::uint8_t invalidEnumValue : invalidEnumValues)
    {
        CAPTURE(invalidEnumValue);

        auto invalidSessionBytes = kSessionDescriptorGolden;
        invalidSessionBytes[36] = Byte(invalidEnumValue);
        const auto invalidSessionResult = pbprotocol::ParseSessionDescriptor(
            invalidSessionBytes,
            resourcePolicy);
        REQUIRE_FALSE(invalidSessionResult);
        REQUIRE(
            invalidSessionResult.Error().code ==
            pbprotocol::ProtocolErrorCode::InvalidEnumValue);

        auto invalidCompressionBytes = kDirectSegmentGolden;
        invalidCompressionBytes[40] = Byte(invalidEnumValue);
        const auto invalidCompressionResult =
            pbprotocol::ParseSegmentDescriptor(
                invalidCompressionBytes,
                sessionDescriptor,
                resourcePolicy);
        REQUIRE_FALSE(invalidCompressionResult);
        REQUIRE(
            invalidCompressionResult.Error().code ==
            pbprotocol::ProtocolErrorCode::InvalidEnumValue);

        auto invalidModeBytes = kDirectSegmentGolden;
        invalidModeBytes[41] = Byte(invalidEnumValue);
        const auto invalidModeResult = pbprotocol::ParseSegmentDescriptor(
            invalidModeBytes,
            sessionDescriptor,
            resourcePolicy);
        REQUIRE_FALSE(invalidModeResult);
        REQUIRE(
            invalidModeResult.Error().code ==
            pbprotocol::ProtocolErrorCode::InvalidEnumValue);

        auto invalidFinalBytes = kFinalManifestGolden;
        invalidFinalBytes[64] = Byte(invalidEnumValue);
        const auto invalidFinalResult = pbprotocol::ParseFinalManifest(
            invalidFinalBytes,
            sessionDescriptor,
            resourcePolicy);
        REQUIRE_FALSE(invalidFinalResult);
        REQUIRE(
            invalidFinalResult.Error().code ==
            pbprotocol::ProtocolErrorCode::InvalidEnumValue);
    }
}

TEST_CASE("SessionDescriptor payload accepts only protocol version 1.0",
          "[pbprotocol][descriptor][wire][version]")
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::test::MakeResourcePolicy();

    auto wrongMajorBytes = kSessionDescriptorGolden;
    wrongMajorBytes[0] = Byte(0x02);
    const auto wrongMajorResult = pbprotocol::ParseSessionDescriptor(
        wrongMajorBytes,
        resourcePolicy);
    REQUIRE_FALSE(wrongMajorResult);
    REQUIRE(
        wrongMajorResult.Error().code ==
        pbprotocol::ProtocolErrorCode::UnsupportedProtocolMajor);

    auto wrongMinorBytes = kSessionDescriptorGolden;
    wrongMinorBytes[2] = Byte(0x01);
    const auto wrongMinorResult = pbprotocol::ParseSessionDescriptor(
        wrongMinorBytes,
        resourcePolicy);
    REQUIRE_FALSE(wrongMinorResult);
    REQUIRE(
        wrongMinorResult.Error().code ==
        pbprotocol::ProtocolErrorCode::UnsupportedProtocolMinor);
}
