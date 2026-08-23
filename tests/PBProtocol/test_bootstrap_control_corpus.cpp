#include "descriptor_test_helpers.h"

#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/control_fragment_codec.h"
#include "pbprotocol/descriptor_codec.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <span>
#include <string_view>

#if !defined(PB_BOOTSTRAP_CONTROL_CORPUS_DIRECTORY)
#error "PB_BOOTSTRAP_CONTROL_CORPUS_DIRECTORY must identify the corpus."
#endif

namespace {

template <std::size_t ByteCount>
[[nodiscard]] std::array<std::byte, ByteCount> ReadCorpusFile(
    const std::string_view fileName)
{
    const std::filesystem::path filePath =
        std::filesystem::path(PB_BOOTSTRAP_CONTROL_CORPUS_DIRECTORY) / fileName;
    REQUIRE(std::filesystem::file_size(filePath) == ByteCount);

    std::ifstream inputFile(filePath, std::ios::binary);
    REQUIRE(inputFile.is_open());

    std::array<std::byte, ByteCount> bytes{};
    inputFile.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    REQUIRE(inputFile.gcount() == static_cast<std::streamsize>(bytes.size()));
    REQUIRE_FALSE(inputFile.bad());
    return bytes;
}

} // namespace

TEST_CASE("Independent PB-Bootstrap-1 corpus seed has exact semantics",
          "[pbprotocol][bootstrap][corpus][conformance]")
{
    const auto bytes = ReadCorpusFile<pbprotocol::kBootstrapRecordBytes>(
        "valid-bootstrap.bin");
    const pbprotocol::BootstrapRecord expected{
        pbprotocol::kBootstrapVersion,
        pbprotocol::GetProtocolVersion(),
        1,
        0x0102030405060708ULL,
        pbprotocol::SessionTag{0x81DF204BD997BAD0ULL},
        0x1112131415161718ULL,
        0x21222324U,
        0};

    const auto parsedResult = pbprotocol::ParseBootstrapRecord(bytes);
    REQUIRE(parsedResult);
    REQUIRE(parsedResult.Value() == expected);

    std::array<std::byte, pbprotocol::kBootstrapRecordBytes> serialized{};
    REQUIRE(pbprotocol::SerializeBootstrapRecord(
        parsedResult.Value(),
        serialized));
    REQUIRE(serialized == bytes);
}

TEST_CASE("Independent PB-Control-1 corpus seed wraps the provisional Session payload",
          "[pbprotocol][control][corpus][conformance]")
{
    constexpr std::size_t controlGoldenBytes = 67;
    const auto bytes = ReadCorpusFile<controlGoldenBytes>(
        "valid-control-session.bin");
    const auto parsedResult = pbprotocol::ParseControlRecord(bytes);
    REQUIRE(parsedResult);
    REQUIRE(
        parsedResult.Value().recordType ==
        pbprotocol::ControlRecordType::SessionDescriptor);
    REQUIRE(parsedResult.Value().controlSequence == 0x0102030405060708ULL);
    REQUIRE(
        parsedResult.Value().sessionTag ==
        pbprotocol::SessionTag{0x81DF204BD997BAD0ULL});

    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::test::MakeResourcePolicy();
    const auto descriptorResult = pbprotocol::ParseSessionDescriptor(
        parsedResult.Value().payload,
        resourcePolicy);
    REQUIRE(descriptorResult);
    REQUIRE(
        descriptorResult.Value() ==
        pbprotocol::test::MakeSessionDescriptor(117, 1));

    std::array<std::byte, controlGoldenBytes> serialized{};
    REQUIRE(pbprotocol::SerializeControlRecord(
        parsedResult.Value(),
        serialized));
    REQUIRE(serialized == bytes);
}

TEST_CASE("Independent corrupted Bootstrap and Control corpus seeds fail CRC",
          "[pbprotocol][bootstrap][control][corpus][conformance][crc]")
{
    const auto bootstrapBytes =
        ReadCorpusFile<pbprotocol::kBootstrapRecordBytes>(
            "bad-bootstrap-crc.bin");
    const auto bootstrapResult = pbprotocol::ParseBootstrapRecord(
        bootstrapBytes);
    REQUIRE_FALSE(bootstrapResult);
    REQUIRE(
        bootstrapResult.Error() ==
        pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::CrcMismatch,
            40});

    constexpr std::size_t controlGoldenBytes = 67;
    const auto controlBytes = ReadCorpusFile<controlGoldenBytes>(
        "bad-control-crc.bin");
    const auto controlResult = pbprotocol::ParseControlRecord(controlBytes);
    REQUIRE_FALSE(controlResult);
    REQUIRE(
        controlResult.Error() ==
        pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::CrcMismatch,
            63});
}

TEST_CASE("Independent PB-Control-Fragment-1 corpus seeds have exact semantics",
          "[pbprotocol][control][fragment][corpus][conformance]")
{
    const auto fragmentZero = ReadCorpusFile<48>("valid-fragment-0.bin");
    const auto fragmentOne = ReadCorpusFile<48>("valid-fragment-1.bin");
    const auto fragmentTwo = ReadCorpusFile<43>("valid-fragment-2.bin");
    const std::array<std::span<const std::byte>, 3> fragments{
        fragmentZero,
        fragmentOne,
        fragmentTwo};
    const std::array<std::size_t, 3> payloadBytes{24, 24, 19};

    for (std::uint16_t fragmentIndex = 0;
         fragmentIndex < fragments.size();
         fragmentIndex++)
    {
        const auto parsedResult = pbprotocol::ParseControlFragment(
            fragments[fragmentIndex]);
        REQUIRE(parsedResult);
        REQUIRE(parsedResult.Value().controlRecordId ==
            0x0102030405060708ULL);
        REQUIRE(parsedResult.Value().fragmentIndex == fragmentIndex);
        REQUIRE(parsedResult.Value().fragmentCount == 3);
        REQUIRE(parsedResult.Value().totalRecordBytes == 67);
        REQUIRE(parsedResult.Value().flags == 0);
        REQUIRE(parsedResult.Value().payload.size() ==
            payloadBytes[fragmentIndex]);

        std::array<std::byte, 48> serialized{};
        const auto exactOutput = std::span<std::byte>(serialized).first(
            fragments[fragmentIndex].size());
        REQUIRE(pbprotocol::SerializeControlFragment(
            parsedResult.Value(),
            exactOutput));
        REQUIRE(std::ranges::equal(exactOutput, fragments[fragmentIndex]));
    }
}

TEST_CASE("CRC-valid semantic corpus seeds reach post-checksum validation",
          "[pbprotocol][bootstrap][control][fragment][corpus][semantic]")
{
    const auto bootstrapBytes =
        ReadCorpusFile<pbprotocol::kBootstrapRecordBytes>(
            "semantic-bootstrap-version.bin");
    const auto bootstrapResult = pbprotocol::ParseBootstrapRecord(
        bootstrapBytes);
    REQUIRE_FALSE(bootstrapResult);
    REQUIRE(
        bootstrapResult.Error() ==
        pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::UnsupportedBootstrapVersion,
            4});

    const auto controlBytes = ReadCorpusFile<67>(
        "semantic-control-type.bin");
    const auto controlResult = pbprotocol::ParseControlRecord(controlBytes);
    REQUIRE_FALSE(controlResult);
    REQUIRE(
        controlResult.Error() ==
        pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::InvalidEnumValue,
            5});

    const auto fragmentBytes = ReadCorpusFile<48>(
        "semantic-fragment-flags.bin");
    const auto fragmentResult = pbprotocol::ParseControlFragment(fragmentBytes);
    REQUIRE_FALSE(fragmentResult);
    REQUIRE(
        fragmentResult.Error() ==
        pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::NonZeroReservedBits,
            18});
}

TEST_CASE("Independent PB-Control-1 boundary corpus seeds round trip exactly",
          "[pbprotocol][control][corpus][conformance][boundary]")
{
    SECTION("empty payload")
    {
        const auto bytes = ReadCorpusFile<
            pbprotocol::kMinimumControlRecordBytes>(
                "valid-control-empty.bin");
        const auto parsedResult = pbprotocol::ParseControlRecord(bytes);
        REQUIRE(parsedResult);
        REQUIRE(parsedResult.Value().payload.empty());
        std::array<std::byte, pbprotocol::kMinimumControlRecordBytes>
            serialized{};
        REQUIRE(pbprotocol::SerializeControlRecord(
            parsedResult.Value(),
            serialized));
        REQUIRE(serialized == bytes);
    }

    SECTION("maximum record")
    {
        const auto bytes = ReadCorpusFile<
            pbprotocol::kMaximumControlRecordBytes>(
                "valid-control-maximum.bin");
        const auto parsedResult = pbprotocol::ParseControlRecord(bytes);
        REQUIRE(parsedResult);
        REQUIRE(parsedResult.Value().payload.size() ==
            pbprotocol::kMaximumControlPayloadBytes);
        std::array<std::byte, pbprotocol::kMaximumControlRecordBytes>
            serialized{};
        REQUIRE(pbprotocol::SerializeControlRecord(
            parsedResult.Value(),
            serialized));
        REQUIRE(serialized == bytes);
    }
}
