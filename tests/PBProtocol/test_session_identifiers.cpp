#include "descriptor_test_helpers.h"

#include "pbprotocol/protocol_types.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

static_assert(!std::is_same_v<pbprotocol::RawDigest, pbprotocol::EncodedDigest>);
static_assert(!std::is_same_v<pbprotocol::RawDigest, pbprotocol::WholeFileDigest>);
static_assert(!std::is_same_v<pbprotocol::SessionId, pbprotocol::SessionTag>);

TEST_CASE("SessionTag has a fixed BLAKE3 domain-separated golden value",
          "[pbprotocol][session-tag][golden]")
{
    const pbprotocol::SessionId sessionId = pbprotocol::test::MakeSessionId();
    const pbprotocol::SessionTag sessionTag = pbprotocol::DeriveSessionTag(sessionId);

    REQUIRE(sessionTag.value == 0x81DF204BD997BAD0ULL);
}

TEST_CASE("Empty whole-file digest matches the BLAKE3-256 golden vector",
          "[pbprotocol][digest][golden]")
{
    const std::array<std::byte, pbprotocol::kDigestBytes> expected{
        pbprotocol::test::Byte(0xAF), pbprotocol::test::Byte(0x13),
        pbprotocol::test::Byte(0x49), pbprotocol::test::Byte(0xB9),
        pbprotocol::test::Byte(0xF5), pbprotocol::test::Byte(0xF9),
        pbprotocol::test::Byte(0xA1), pbprotocol::test::Byte(0xA6),
        pbprotocol::test::Byte(0xA0), pbprotocol::test::Byte(0x40),
        pbprotocol::test::Byte(0x4D), pbprotocol::test::Byte(0xEA),
        pbprotocol::test::Byte(0x36), pbprotocol::test::Byte(0xDC),
        pbprotocol::test::Byte(0xC9), pbprotocol::test::Byte(0x49),
        pbprotocol::test::Byte(0x9B), pbprotocol::test::Byte(0xCB),
        pbprotocol::test::Byte(0x25), pbprotocol::test::Byte(0xC9),
        pbprotocol::test::Byte(0xAD), pbprotocol::test::Byte(0xC1),
        pbprotocol::test::Byte(0x12), pbprotocol::test::Byte(0xB7),
        pbprotocol::test::Byte(0xCC), pbprotocol::test::Byte(0x9A),
        pbprotocol::test::Byte(0x93), pbprotocol::test::Byte(0xCA),
        pbprotocol::test::Byte(0xE4), pbprotocol::test::Byte(0x1F),
        pbprotocol::test::Byte(0x32), pbprotocol::test::Byte(0x62)};

    REQUIRE(pbprotocol::GetEmptyBlake3WholeFileDigest().bytes == expected);
}
