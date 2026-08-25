#include "modulation_test_helpers.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using namespace pbmodtest;
using namespace pbmodulation;

namespace {

// Golden 37-byte SessionDescriptor payload (identical to
// kSessionDescriptorGolden in tests/PBProtocol/
// test_bootstrap_control_codec.cpp).
constexpr std::array<std::byte, 37> kSessionDescriptorGolden{
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

struct GoldenFrameDigests
{
    // Pinned once from the reference implementation after the
    // independent cross-checks (see docs/REFERENCE_RASTER.md): the raw
    // container digest is the version-independent primary pin, the PNG
    // digest is the secondary pin bound to libpng 1.6.58 + filter-none +
    // fixed compression level.
    const char* rawDigestHex = "29ed5c8725a5d939685249be82ef5e3717c5ceb3545001ab0bedddf7e2500b0b";
    const char* pngDigestHex = "20ea184adc00f164b1c8fe71b7c568f240b1e8428991e867e0e0b88829144002";
};

void VerifyGoldenFrame(
    const GoldenFramePayload& payload,
    const GoldenFrameDigests& digests)
{
    const std::vector<std::byte> frame =
        EncodeFrameOrDie(payload.MakeInput());

    // Raw container channel.
    const auto rawResult = EncodeRawFrame(frame, 1920, 1080);
    REQUIRE(rawResult);
    const std::vector<std::byte>& raw = rawResult.Value();
    CHECK(ToHex(pbprotocol::ComputeBlake3Digest(
        std::span<const std::byte>(raw))) == digests.rawDigestHex);

    // PNG channel.
    const auto pngResult = EncodePngFrame(frame, 1920, 1080);
    REQUIRE(pngResult);
    const std::vector<std::byte>& png = pngResult.Value();
    CHECK(ToHex(pbprotocol::ComputeBlake3Digest(
        std::span<const std::byte>(png))) == digests.pngDigestHex);

    // Both channels decode back to the exact same raster.
    std::vector<std::byte> decodedRaw(frame.size());
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    const auto rawStatus = DecodeRawFrame(
        std::span<const std::byte>(raw),
        std::span<std::byte>(decodedRaw), width, height);
    REQUIRE(rawStatus);
    REQUIRE(width == 1920);
    REQUIRE(height == 1080);

    std::vector<std::byte> decodedPng(frame.size());
    const auto pngStatus = DecodePngFrame(
        std::span<const std::byte>(png), 1920, 1080,
        std::span<std::byte>(decodedPng));
    REQUIRE(pngStatus);
    CHECK(decodedRaw == frame);
    CHECK(decodedPng == frame);

    // The raster demodulates back to the exact protocol payload.
    const DecodedReferenceFrame decoded =
        DecodeFrameOrDie(decodedPng);
    CHECK(decoded.bootstrapRecord == payload.bootstrap);
    CHECK(decoded.controlWindow == payload.control);
    CHECK(decoded.data == payload.data);
}

} // namespace

TEST_CASE("Reference manifest golden digest is pinned",
    "[pbmodulation][golden][manifest]")
{
    const std::array<std::byte, kReferenceManifestBytes> manifest =
        SerializeReferenceRegionManifest();
    constexpr char kExpectedManifestDigestHex[] =
        "a7e31cbd7cfa6865f8bc029a78d58d065990e732f6386613329f2dd07602003b";
    CHECK(ToHex(pbprotocol::ComputeBlake3Digest(
        std::span<const std::byte>(manifest))) ==
        kExpectedManifestDigestHex);
    CHECK(manifest.size() == 275);
}

TEST_CASE("G0 zero payload golden vector",
    "[pbmodulation][golden][g0]")
{
    const GoldenFrameDigests digests{
        "29ed5c8725a5d939685249be82ef5e3717c5ceb3545001ab0bedddf7e2500b0b",
        "20ea184adc00f164b1c8fe71b7c568f240b1e8428991e867e0e0b88829144002"};
    VerifyGoldenFrame(MakeZeroGoldenPayload(), digests);
}

TEST_CASE("G1 canonical payload golden vector",
    "[pbmodulation][golden][g1]")
{
    const GoldenFrameDigests digests{
        "54c23b3e558bf4805afadb6193217d9ef111bcb5975ee1336a633ad569bf0d57",
        "f65997df4b612412cb2ce50de3c0b9f9fd542bc84842b9f084b76c5f6e290a89"};
    VerifyGoldenFrame(MakeCanonicalGoldenPayload(), digests);
}

TEST_CASE("G2 maximum payload golden vector",
    "[pbmodulation][golden][g2]")
{
    const GoldenFrameDigests digests{
        "ccd24926d8e578fc65dcc4aaeafbeed1da3ce19c43a4d5709471344ddc3ffdcc",
        "919510014ecab9fc5b56246a8c71d47e01f8b9824c90debb4ade116d86d544cf"};
    VerifyGoldenFrame(MakeMaxGoldenPayload(), digests);
}

TEST_CASE("G1 payload cross-validates against the protocol and inner FEC",
    "[pbmodulation][golden][g1][cross]")
{
    const GoldenFramePayload payload = MakeCanonicalGoldenPayload();
    const std::vector<std::byte> frame =
        EncodeFrameOrDie(payload.MakeInput());
    const DecodedReferenceFrame decoded = DecodeFrameOrDie(frame);

    // The bootstrap lane demodulates to the golden PB-Bootstrap-1 record.
    const auto bootstrapResult = pbprotocol::ParseBootstrapRecord(
        std::span<const std::byte>(decoded.bootstrapRecord));
    REQUIRE(bootstrapResult);
    const auto& record = bootstrapResult.Value();
    CHECK(record.bootstrapVersion == pbprotocol::kBootstrapVersion);
    CHECK(record.protocolVersion == pbprotocol::GetProtocolVersion());
    CHECK(record.visualLayoutVersion == 1);
    CHECK(record.visualProfileId == 0x0102030405060708ULL);
    CHECK(record.sessionTag ==
        pbprotocol::SessionTag{0x81DF204BD997BAD0ULL});
    CHECK(record.frameSequence == 0x1112131415161718ULL);
    CHECK(record.controlEpoch == 0x21222324U);
    CHECK(record.flags == 0);

    // The control window demodulates to the golden SessionDescriptor
    // record in its first 67 bytes, zero-padded after.
    {
        const std::span<const std::byte> controlWindow =
            std::span<const std::byte>(decoded.controlWindow);
        const auto controlResult =
            pbprotocol::ParseControlRecord(controlWindow.first(67));
        REQUIRE(controlResult);
        const auto& controlRecord = controlResult.Value();
        CHECK(controlRecord.controlVersion ==
            pbprotocol::kControlVersion);
        CHECK(controlRecord.recordType ==
            pbprotocol::ControlRecordType::SessionDescriptor);
        CHECK(controlRecord.controlSequence ==
            0x0102030405060708ULL);
        CHECK(controlRecord.sessionTag ==
            pbprotocol::SessionTag{0x81DF204BD997BAD0ULL});
        CHECK(BytesEqual(controlRecord.payload,
            std::span<const std::byte>(kSessionDescriptorGolden)));
        std::size_t nonZeroTail = 0;
        for (std::size_t i = 67; i < kReferenceControlWindowBytes; i++)
        {
            if (decoded.controlWindow[i] != std::byte{0})
            {
                nonZeroTail++;
            }
        }
        CHECK(nonZeroTail == 0);
    }

    // The data lane carries the Robust QC-LDPC golden codeword in its
    // first 2025 bytes (end-to-end Inner-FEC carrier proof), with the
    // shared 0xC0FFEE pattern as systematic prefix and zero padding after.
    {
        const std::span<const std::byte> data =
            std::span<const std::byte>(decoded.data);
        CHECK(ToHex(data.first(16)) == kPatternInfoFirst16Hex);
        const auto syndromeResult = pbinnerfec::ComputeQcLdpcSyndrome(
            pbinnerfec::kInnerFecProfileIdRobust,
            data.first(2025));
        REQUIRE(syndromeResult);
        CHECK(syndromeResult.Value());
        std::size_t nonZeroTail = 0;
        for (std::size_t i = 2025; i < kReferenceDataRegionBytes; i++)
        {
            if (decoded.data[i] != std::byte{0})
            {
                nonZeroTail++;
            }
        }
        CHECK(nonZeroTail == 0);
    }
}
