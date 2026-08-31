#include "channel_matrix_core.h"

#include "pbremotevisualsimulator/channel_transform.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>
#include <string>

namespace
{

const pbremotevisualmatrix::ChannelMatrixCaseSummary& FindCase(
    const pbremotevisualmatrix::ChannelMatrixReport& report, const std::string& name)
{
    const auto iterator = std::find_if(report.cases.begin(), report.cases.end(), [&name](const auto& summary)
    {
        return summary.name == name;
    });
    REQUIRE(iterator != report.cases.end());
    return *iterator;
}

} // namespace

TEST_CASE("RemoteVisual channel matrix is deterministic and preserves the production truth boundary",
    "[tools][remote-visual][low-fps][matrix][truth]")
{
    pbremotevisualmatrix::ChannelMatrixReport first;
    pbremotevisualmatrix::ChannelMatrixReport second;
    std::string firstError;
    std::string secondError;
    REQUIRE(pbremotevisualmatrix::BuildDefaultChannelMatrix(first, firstError));
    REQUIRE(pbremotevisualmatrix::BuildDefaultChannelMatrix(second, secondError));
    REQUIRE(firstError.empty());
    REQUIRE(secondError.empty());
    REQUIRE(first.canonicalJson == second.canonicalJson);
    REQUIRE(first.payloadBlake3 == second.payloadBlake3);
    REQUIRE(first.truthBoundaryValid);
    REQUIRE(first.expectationsMatched);
    REQUIRE(first.cases.size() == 22);
    REQUIRE(first.canonicalJson.starts_with("{\"schema\":\"PixelBridge.RemoteVisualChannelMatrix.2\""));
    REQUIRE(first.canonicalJson.ends_with("\n"));
    REQUIRE(first.canonicalJson.find("\"payloadBlake3\":\"" +
        pbremotevisualsimulator::ChannelDigestToHex(first.payloadBlake3) + "\"") != std::string::npos);
    REQUIRE(first.canonicalJson.find("nan") == std::string::npos);
    REQUIRE(first.canonicalJson.find("inf") == std::string::npos);

    std::set<std::string> names;
    for (const auto& summary : first.cases)
    {
        REQUIRE(names.insert(summary.name).second);
        REQUIRE(summary.expectationMatched);
        REQUIRE(summary.falseAcceptedCodewords == 0);
        REQUIRE(summary.receiverEvidence.IsSafe());
        REQUIRE(summary.receiverEvidence.inputTransportBlocks == summary.acceptedTransportBlocks);
        REQUIRE(summary.receiverEvidence.parsedTransportBlocks == summary.acceptedTransportBlocks);
        REQUIRE(summary.receiverEvidence.uniqueOuterSymbols == summary.acceptedTransportBlocks);
        REQUIRE(summary.receiverEvidence.receiverRejections == 0);
        REQUIRE(summary.receiverEvidence.outerConflictRejections == 0);
        REQUIRE(summary.receiverEvidence.resourcePolicyRejections == 0);
        if (summary.acceptedTransportBlocks == 4)
        {
            REQUIRE(summary.receiverEvidence.verifiedSegments == 1);
            REQUIRE(summary.receiverEvidence.verifiedRawBytes ==
                pbremotevisualreceiverevidence::kReceiverEvidenceSegmentBytes);
            REQUIRE(summary.receiverEvidence.finalizationPrepared);
            REQUIRE(summary.receiverEvidence.wholeFileDigestDisposition ==
                pbremotevisualreceiverevidence::WholeFileDigestDisposition::Pass);
            REQUIRE(summary.receiverEvidence.observedWholeFileBlake3 ==
                summary.receiverEvidence.expectedWholeFileBlake3);
        }
        else
        {
            REQUIRE(summary.receiverEvidence.verifiedSegments == 0);
            REQUIRE(summary.receiverEvidence.verifiedRawBytes == 0);
            REQUIRE_FALSE(summary.receiverEvidence.finalizationPrepared);
            REQUIRE(summary.receiverEvidence.wholeFileDigestDisposition ==
                pbremotevisualreceiverevidence::WholeFileDigestDisposition::NotReady);
            REQUIRE(summary.receiverEvidence.observedWholeFileBlake3.empty());
        }
        REQUIRE(summary.classification != pbremotevisualmatrix::ChannelMatrixClassification::FalseAcceptance);
        REQUIRE(summary.classification != pbremotevisualmatrix::ChannelMatrixClassification::ExecutionFailure);
        REQUIRE(summary.manifestBlake3.size() == 64);
        REQUIRE(summary.outputBlake3.size() == 64);
    }

    const auto& identity = FindCase(first, "identity");
    REQUIRE(identity.classification == pbremotevisualmatrix::ChannelMatrixClassification::Verified);
    REQUIRE(identity.acceptedTransportBlocks == 4);
    REQUIRE(identity.receiverEvidence.wholeFileDigestDisposition ==
        pbremotevisualreceiverevidence::WholeFileDigestDisposition::Pass);

    const auto& crop = FindCase(first, "illegal-crop-8px");
    REQUIRE(crop.classification == pbremotevisualmatrix::ChannelMatrixClassification::ErasureNoFalseAccept);
    REQUIRE(crop.acceptedTransportBlocks == 0);

    const auto& bootstrapMismatch = FindCase(first, "bootstrap-a-mismatch");
    REQUIRE(bootstrapMismatch.classification ==
        pbremotevisualmatrix::ChannelMatrixClassification::ErasureNoFalseAccept);
    REQUIRE(bootstrapMismatch.acceptedTransportBlocks == 0);

    const auto& freshnessLowConfidence = FindCase(first, "freshness-low-confidence");
    REQUIRE(freshnessLowConfidence.freshnessTagErasures >= 1);

    REQUIRE(FindCase(first, "full-range-444-identity").classification ==
        pbremotevisualmatrix::ChannelMatrixClassification::Verified);
    REQUIRE(FindCase(first, "reference-block-8x8").expectationMatched);
    REQUIRE(FindCase(first, "reference-block-16x16").expectationMatched);
    REQUIRE(FindCase(first, "reference-block-64x64").expectationMatched);
    REQUIRE(FindCase(first, "alpha-overlay-128").expectationMatched);

    const auto& stale = FindCase(first, "stale-region-replacement");
    REQUIRE(stale.classification == pbremotevisualmatrix::ChannelMatrixClassification::Verified);
    REQUIRE(stale.staleRegions == 1);

    const auto& temporalBlend = FindCase(first, "temporal-blend-96");
    REQUIRE(temporalBlend.classification == pbremotevisualmatrix::ChannelMatrixClassification::ErasureNoFalseAccept);
    REQUIRE(temporalBlend.acceptedTransportBlocks == 0);
}
