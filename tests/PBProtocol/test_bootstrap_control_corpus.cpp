#include "descriptor_test_helpers.h"

#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/descriptor_codec.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ranges>
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
