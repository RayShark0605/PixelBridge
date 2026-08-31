#include "temporal_corpus_core.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <string>

TEST_CASE("RemoteVisual temporal corpus is deterministic and enforces production identity admission",
    "[tools][remote-visual][temporal][duplicate][reorder][epoch][identity]")
{
    pbremotevisualtemporalcorpus::TemporalCorpusReport first;
    pbremotevisualtemporalcorpus::TemporalCorpusReport second;
    std::string firstError;
    std::string secondError;
    REQUIRE(pbremotevisualtemporalcorpus::BuildDefaultTemporalCorpus(first, firstError));
    REQUIRE(pbremotevisualtemporalcorpus::BuildDefaultTemporalCorpus(second, secondError));
    REQUIRE(firstError.empty());
    REQUIRE(secondError.empty());
    REQUIRE(first.canonicalJson == second.canonicalJson);
    REQUIRE(first.payloadBlake3 == second.payloadBlake3);
    REQUIRE(std::ranges::any_of(first.payloadBlake3, [](const std::byte value) { return value != std::byte{0}; }));
    REQUIRE(first.eventCount == 11);
    REQUIRE(first.admittedTransportBlocks == 16);
    REQUIRE(first.suppressedDuplicateEvents == 3);
    REQUIRE(first.suppressedReorderedEvents == 1);
    REQUIRE(first.duplicateRefinementRecoveries == 1);
    REQUIRE(first.wrongIdentityAcceptedTransportBlocks == 0);
    REQUIRE(first.wrongIdentityDiagnosticCandidates == 4);
    REQUIRE(first.productionAdmissionSafe);
    REQUIRE(first.expectationsMatched);
    REQUIRE(first.canonicalJson.starts_with(
        "{\"schema\":\"PixelBridge.RemoteVisualTemporalCorpus.1\",\"version\":1,"));
    REQUIRE(first.canonicalJson.ends_with("\n"));
    REQUIRE(first.canonicalJson.find("\"name\":\"duplicate-gap-reorder-epoch\"") != std::string::npos);
    REQUIRE(first.canonicalJson.find("\"name\":\"duplicate-refinement-after-erasure\"") != std::string::npos);
    REQUIRE(first.canonicalJson.find("\"name\":\"valid-crc-wrong-identity\"") != std::string::npos);
    REQUIRE(first.canonicalJson.find("\"identityFailures\":4") != std::string::npos);
    REQUIRE(first.canonicalJson.find("\"wrongIdentityAcceptedTransportBlocks\":0") != std::string::npos);
    REQUIRE(first.canonicalJson.find("\"capture\":{\"count\":1,\"totalMilliseconds\":1400") !=
        std::string::npos);
    REQUIRE(first.canonicalJson.find("\"visual\":{\"count\":1,\"totalMilliseconds\":1300") !=
        std::string::npos);
}
