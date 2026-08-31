#include "receiver_evidence.h"

#include "pbprotocol/transport_block_codec.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace
{

std::vector<pbdesktoplevels::AcceptedTransportBlock> MakeCanonicalBlocks()
{
    std::vector<pbdesktoplevels::AcceptedTransportBlock> blocks;
    blocks.reserve(4);
    for (std::uint32_t slot = 0; slot < 4; slot++)
    {
        pbdesktoplevels::AcceptedTransportBlock block;
        std::string error;
        REQUIRE(pbremotevisualreceiverevidence::MakeReceiverEvidenceExpectedTransportBlock(
            0, slot, block, error));
        REQUIRE(error.empty());
        blocks.push_back(block);
    }
    return blocks;
}

pbdesktoplevels::AcceptedTransportBlock MakeConflictingBlock(
    const pbdesktoplevels::AcceptedTransportBlock& canonical)
{
    const auto parsed = pbprotocol::ParseTransportBlock(
        std::span(canonical.bytes).first(canonical.byteCount));
    REQUIRE(parsed);
    std::vector<std::byte> payload(parsed.Value().payload.begin(), parsed.Value().payload.end());
    payload[0] ^= std::byte{0x01};
    pbdesktoplevels::AcceptedTransportBlock conflicting;
    conflicting.slot = canonical.slot;
    conflicting.byteCount = static_cast<std::uint32_t>(
        pbprotocol::GetTransportSerializedSize(parsed.Value().header));
    REQUIRE(pbprotocol::SerializeTransportBlock(parsed.Value().header, payload,
        std::span(conflicting.bytes).first(conflicting.byteCount)));
    return conflicting;
}

} // namespace

TEST_CASE("RemoteVisual Receiver evidence reaches unique Outer admission and WholeFileDigest",
    "[tools][remote-visual][receiver][outer][digest]")
{
    const auto canonical = MakeCanonicalBlocks();
    std::vector<pbdesktoplevels::AcceptedTransportBlock> input{
        canonical[0], canonical[0], canonical[1], canonical[2], canonical[3]};
    pbremotevisualreceiverevidence::ReceiverEvidenceSummary summary;
    std::string error;
    REQUIRE(pbremotevisualreceiverevidence::EvaluateReceiverEvidence(1, input, summary, error));
    REQUIRE(error.empty());
    REQUIRE(summary.IsSafe());
    REQUIRE(summary.inputTransportBlocks == 5);
    REQUIRE(summary.parsedTransportBlocks == 5);
    REQUIRE(summary.uniqueOuterSymbols == 4);
    REQUIRE(summary.identicalDuplicateOuterSymbols == 1);
    REQUIRE(summary.verifiedSegments == 1);
    REQUIRE(summary.verifiedRawBytes == pbremotevisualreceiverevidence::kReceiverEvidenceSegmentBytes);
    REQUIRE(summary.finalizationPrepared);
    REQUIRE(summary.wholeFileDigestDisposition ==
        pbremotevisualreceiverevidence::WholeFileDigestDisposition::Pass);
    REQUIRE(summary.expectedWholeFileBlake3.size() == 64);
    REQUIRE(summary.observedWholeFileBlake3 == summary.expectedWholeFileBlake3);
    REQUIRE(pbremotevisualreceiverevidence::CountProductionTruthMismatches(0, input) == 0);
}

TEST_CASE("RemoteVisual Receiver evidence reports incomplete and conflicting Outer state fail closed",
    "[tools][remote-visual][receiver][outer][conflict][resource]")
{
    const auto canonical = MakeCanonicalBlocks();
    pbremotevisualreceiverevidence::ReceiverEvidenceSummary incomplete;
    std::string error;
    REQUIRE(pbremotevisualreceiverevidence::EvaluateReceiverEvidence(1,
        std::span<const pbdesktoplevels::AcceptedTransportBlock>{}, incomplete, error));
    REQUIRE(incomplete.IsSafe());
    REQUIRE(incomplete.wholeFileDigestDisposition ==
        pbremotevisualreceiverevidence::WholeFileDigestDisposition::NotReady);
    REQUIRE_FALSE(incomplete.finalizationPrepared);

    const auto conflicting = MakeConflictingBlock(canonical[0]);
    const std::array input{canonical[0], conflicting};
    pbremotevisualreceiverevidence::ReceiverEvidenceSummary rejected;
    REQUIRE(pbremotevisualreceiverevidence::EvaluateReceiverEvidence(1, input, rejected, error));
    REQUIRE_FALSE(rejected.IsSafe());
    REQUIRE(rejected.receiverRejections == 1);
    REQUIRE(rejected.outerConflictRejections == 1);
    REQUIRE(rejected.wholeFileDigestDisposition ==
        pbremotevisualreceiverevidence::WholeFileDigestDisposition::ReceiverRejected);
    REQUIRE(pbremotevisualreceiverevidence::CountProductionTruthMismatches(0, input) == 1);

    auto wrongSlot = canonical[0];
    wrongSlot.slot = 1;
    REQUIRE(pbremotevisualreceiverevidence::CountProductionTruthMismatches(0,
        std::span<const pbdesktoplevels::AcceptedTransportBlock>(&wrongSlot, 1)) == 1);

    pbremotevisualreceiverevidence::ReceiverEvidenceSummary untouched;
    untouched.configuredSegments = 123;
    REQUIRE_FALSE(pbremotevisualreceiverevidence::EvaluateReceiverEvidence(0, canonical, untouched, error));
    REQUIRE(untouched.configuredSegments == 123);
    REQUIRE_FALSE(pbremotevisualreceiverevidence::EvaluateReceiverEvidence(
        pbremotevisualreceiverevidence::kMaximumReceiverEvidenceSegments + 1, canonical, untouched, error));
    REQUIRE(untouched.configuredSegments == 123);
}
