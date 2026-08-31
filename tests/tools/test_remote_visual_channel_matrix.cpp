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
    REQUIRE(first.cases.size() == 15);
    REQUIRE(first.canonicalJson.starts_with("{\"schema\":\"PixelBridge.RemoteVisualChannelMatrix.1\""));
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
        REQUIRE(summary.classification != pbremotevisualmatrix::ChannelMatrixClassification::FalseAcceptance);
        REQUIRE(summary.classification != pbremotevisualmatrix::ChannelMatrixClassification::ExecutionFailure);
        REQUIRE(summary.manifestBlake3.size() == 64);
        REQUIRE(summary.outputBlake3.size() == 64);
    }

    const auto& identity = FindCase(first, "identity");
    REQUIRE(identity.classification == pbremotevisualmatrix::ChannelMatrixClassification::Verified);
    REQUIRE(identity.acceptedTransportBlocks == 4);

    const auto& crop = FindCase(first, "illegal-crop-8px");
    REQUIRE(crop.classification == pbremotevisualmatrix::ChannelMatrixClassification::ErasureNoFalseAccept);
    REQUIRE(crop.acceptedTransportBlocks == 0);

    const auto& stale = FindCase(first, "stale-region-replacement");
    REQUIRE(stale.classification == pbremotevisualmatrix::ChannelMatrixClassification::Verified);
    REQUIRE(stale.staleRegions == 1);

    const auto& temporalBlend = FindCase(first, "temporal-blend-96");
    REQUIRE(temporalBlend.classification == pbremotevisualmatrix::ChannelMatrixClassification::ErasureNoFalseAccept);
    REQUIRE(temporalBlend.acceptedTransportBlocks == 0);
}
