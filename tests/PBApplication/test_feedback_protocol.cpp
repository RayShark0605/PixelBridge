#include "feedback_protocol.h"

#include "pbprotocol/crc32c.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace
{

pbapp::FeedbackSnapshot MakeSnapshot(const std::uint32_t segmentCount = 13)
{
    pbapp::FeedbackSnapshot snapshot;
    snapshot.identity.sessionId.bytes.fill(std::byte{0x11});
    snapshot.identity.profileId = 0x504250414D343031ULL;
    snapshot.identity.layoutVersion = 15;
    snapshot.identity.fileBytes = segmentCount;
    snapshot.identity.segmentCount = segmentCount;
    snapshot.identity.canonicalSessionDigest.fill(std::byte{0x44});
    snapshot.identity.canonicalManifestDigest.fill(std::byte{0x55});
    snapshot.identity.wholeFileDigest.bytes.fill(std::byte{0x66});
    snapshot.receiverId.fill(std::byte{0x22});
    snapshot.receiverEpoch.fill(std::byte{0x33});
    snapshot.snapshotSequence = 7;
    snapshot.observedFrameAvailable = true;
    snapshot.observedFrameSequence = 4097;
    snapshot.identityBound = true;
    snapshot.phase = pbapp::FeedbackPhase::Receiving;
    snapshot.durableCompleted.resize((segmentCount + 7U) / 8U);
    return snapshot;
}

void RewriteCrc(std::vector<std::byte>& bytes)
{
    REQUIRE(bytes.size() >= 4);
    const std::uint32_t crc = pbprotocol::ComputeCrc32c(std::span<const std::byte>(bytes).first(bytes.size() - 4));
    for (std::size_t index = 0; index < 4; index++)
    {
        bytes[bytes.size() - 4 + index] = static_cast<std::byte>((crc >> (8U * index)) & 0xFFU);
    }
}

}

TEST_CASE("Feedback snapshot round-trips exact identities and a bounded completion map", "[feedback][protocol]")
{
    auto snapshot = MakeSnapshot();
    snapshot.durableCompleted = {std::byte{0x09}, std::byte{0x10}};
    std::vector<std::byte> encoded;
    REQUIRE(pbapp::SerializeFeedbackSnapshot(snapshot, encoded));
    REQUIRE(encoded.size() == 222);
    const std::array<std::uint8_t, 24> expectedPrefix{
        0x50, 0x42, 0x46, 0x42, 0x01, 0x00, 0xD8, 0x00,
        0xDE, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
        0x02, 0x0F, 0x00, 0x00, 0x0D, 0x00, 0x00, 0x00};
    for (std::size_t index = 0; index < expectedPrefix.size(); index++)
    {
        REQUIRE(std::to_integer<std::uint8_t>(encoded[index]) == expectedPrefix[index]);
    }
    pbapp::FeedbackSnapshot decoded;
    REQUIRE(pbapp::ParseFeedbackSnapshot(encoded, decoded));
    REQUIRE(decoded == snapshot);
}

TEST_CASE("Feedback snapshot supports zero and 65536 segments without unbounded allocation", "[feedback][protocol]")
{
    for (const std::uint32_t segmentCount : {0U, 1U, 65536U})
    {
        const auto snapshot = MakeSnapshot(segmentCount);
        std::vector<std::byte> encoded;
        REQUIRE(pbapp::SerializeFeedbackSnapshot(snapshot, encoded));
        REQUIRE(encoded.size() == 220U + (segmentCount + 7U) / 8U);
        REQUIRE(encoded.size() <= 65536);
        pbapp::FeedbackSnapshot decoded;
        REQUIRE(pbapp::ParseFeedbackSnapshot(encoded, decoded));
        REQUIRE(decoded == snapshot);
        if (segmentCount == 65536)
        {
            REQUIRE(decoded.durableCompleted.size() == 8192);
        }
    }
    auto tooMany = MakeSnapshot();
    tooMany.identity.segmentCount = 65537;
    REQUIRE_FALSE(pbapp::ValidateFeedbackSnapshot(tooMany));
    auto tooLarge = MakeSnapshot();
    tooLarge.identity.fileBytes = 500ULL * 1024ULL * 1024ULL * 1024ULL + 1;
    REQUIRE_FALSE(pbapp::ValidateFeedbackSnapshot(tooLarge));
}

TEST_CASE("Feedback parser rejects truncation trailing bytes unknown flags and CRC", "[feedback][protocol]")
{
    std::vector<std::byte> encoded;
    REQUIRE(pbapp::SerializeFeedbackSnapshot(MakeSnapshot(), encoded));
    const auto sentinel = MakeSnapshot(1);
    pbapp::FeedbackSnapshot output = sentinel;
    for (std::size_t length = 0; length < encoded.size(); length++)
    {
        REQUIRE_FALSE(pbapp::ParseFeedbackSnapshot(std::span<const std::byte>(encoded).first(length), output));
        REQUIRE(output == sentinel);
    }
    auto trailing = encoded;
    trailing.push_back(std::byte{0});
    REQUIRE_FALSE(pbapp::ParseFeedbackSnapshot(trailing, output));
    auto unknownFlags = encoded;
    unknownFlags[15] = std::byte{0x80};
    RewriteCrc(unknownFlags);
    REQUIRE_FALSE(pbapp::ParseFeedbackSnapshot(unknownFlags, output));
    auto corrupt = encoded;
    corrupt[64] ^= std::byte{1};
    REQUIRE_FALSE(pbapp::ParseFeedbackSnapshot(corrupt, output));
    auto wrongVersion = encoded;
    wrongVersion[4] = std::byte{2};
    RewriteCrc(wrongVersion);
    REQUIRE_FALSE(pbapp::ParseFeedbackSnapshot(wrongVersion, output));
    auto hugeCount = encoded;
    hugeCount[20] = std::byte{0xFF};
    hugeCount[21] = std::byte{0xFF};
    hugeCount[22] = std::byte{0xFF};
    hugeCount[23] = std::byte{0xFF};
    RewriteCrc(hugeCount);
    REQUIRE_FALSE(pbapp::ParseFeedbackSnapshot(hugeCount, output));
    const std::vector<std::byte> huge(65537, std::byte{0});
    REQUIRE_FALSE(pbapp::ParseFeedbackSnapshot(huge, output));
    REQUIRE(output == sentinel);
}

TEST_CASE("Feedback snapshot rejects inconsistent final claims and unused completion bits", "[feedback][protocol]")
{
    auto snapshot = MakeSnapshot(1);
    snapshot.durableCompleted[0] = std::byte{0x80};
    REQUIRE_FALSE(pbapp::ValidateFeedbackSnapshot(snapshot));
    std::vector<std::byte> encoded{std::byte{0xA5}};
    REQUIRE_FALSE(pbapp::SerializeFeedbackSnapshot(snapshot, encoded));
    REQUIRE(encoded == std::vector<std::byte>{std::byte{0xA5}});
    snapshot.durableCompleted[0] = std::byte{1};
    snapshot.phase = pbapp::FeedbackPhase::Completed;
    REQUIRE_FALSE(pbapp::ValidateFeedbackSnapshot(snapshot));
    snapshot.wholeDigestVerified = true;
    snapshot.finalPublishSucceeded = true;
    snapshot.finalReopenVerified = true;
    REQUIRE(pbapp::ValidateFeedbackSnapshot(snapshot));
    REQUIRE(pbapp::SerializeFeedbackSnapshot(snapshot, encoded));
    pbapp::FeedbackSnapshot decoded;
    REQUIRE(pbapp::ParseFeedbackSnapshot(encoded, decoded));
    REQUIRE(decoded == snapshot);
    snapshot.durableCompleted[0] = std::byte{0};
    REQUIRE_FALSE(pbapp::ValidateFeedbackSnapshot(snapshot));
    snapshot.durableCompleted[0] = std::byte{1};
    snapshot.phase = pbapp::FeedbackPhase::Receiving;
    REQUIRE_FALSE(pbapp::ValidateFeedbackSnapshot(snapshot));
    snapshot.phase = pbapp::FeedbackPhase::Completed;
    snapshot.identityBound = false;
    REQUIRE_FALSE(pbapp::ValidateFeedbackSnapshot(snapshot));
}

TEST_CASE("Feedback snapshot rejects invalid epochs sequence and reserved bytes", "[feedback][protocol]")
{
    auto snapshot = MakeSnapshot();
    snapshot.snapshotSequence = 0;
    REQUIRE_FALSE(pbapp::ValidateFeedbackSnapshot(snapshot));
    snapshot.snapshotSequence = 1;
    snapshot.receiverEpoch.fill(std::byte{0});
    REQUIRE_FALSE(pbapp::ValidateFeedbackSnapshot(snapshot));
    snapshot.receiverEpoch.fill(std::byte{1});
    std::vector<std::byte> encoded;
    REQUIRE(pbapp::SerializeFeedbackSnapshot(snapshot, encoded));
    encoded[212] = std::byte{1};
    RewriteCrc(encoded);
    pbapp::FeedbackSnapshot output;
    REQUIRE_FALSE(pbapp::ParseFeedbackSnapshot(encoded, output));
}
