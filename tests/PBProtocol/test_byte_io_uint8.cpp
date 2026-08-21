#include "pbprotocol/byte_io.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>

TEST_CASE("Uint8 primitive is exact and atomic", "[pbprotocol][wire][uint8]")
{
    std::array<std::byte, 1> output{std::byte{0xA5}};
    pbprotocol::ByteWriter writer(output);
    REQUIRE(writer.WriteUint8(0x7BU));
    REQUIRE(output[0] == std::byte{0x7B});

    const std::array<std::byte, 1> beforeFailure = output;
    const auto writeFailure = writer.WriteUint8(0x11U);
    REQUIRE_FALSE(writeFailure);
    REQUIRE(
        writeFailure.Error().code ==
        pbprotocol::ProtocolErrorCode::OutputBufferTooSmall);
    REQUIRE(output == beforeFailure);

    pbprotocol::ByteReader reader(output);
    const auto valueResult = reader.ReadUint8();
    REQUIRE(valueResult);
    REQUIRE(valueResult.Value() == 0x7BU);

    const auto readFailure = reader.ReadUint8();
    REQUIRE_FALSE(readFailure);
    REQUIRE(
        readFailure.Error().code ==
        pbprotocol::ProtocolErrorCode::TruncatedInput);
    REQUIRE(reader.Position() == output.size());
}
