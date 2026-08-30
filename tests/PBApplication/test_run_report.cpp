#include "run_report.h"

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <string>

TEST_CASE("Encoder report explicitly omits receiver progress goodput and ETA", "[application][report][encoder]")
{
    pbapp::EncoderSnapshot snapshot;
    snapshot.state = pbapp::EncoderState::Broadcasting;
    snapshot.runId = "run-1";
    snapshot.sessionTag = 7;
    snapshot.cycleCount = 3;
    snapshot.cyclePosition = 5;
    snapshot.cycleFrameCount = 10;
    snapshot.dataWindowLeft = 2560;
    snapshot.runStartedUnixMilliseconds = 1000;
    snapshot.runEndedUnixMilliseconds = 2000;
    snapshot.generatedVisualFramesPerSecond = (std::numeric_limits<double>::infinity)();
    const pbapp::RunReportContext context{"PixelBridgeEncoder", "0.1.0", "deadbeef", "2026-08-30T00:00:00Z"};
    const std::string json = pbapp::BuildEncoderRunReportJson(context, snapshot);
    REQUIRE(json.find("\"state\":\"Broadcasting\"") != std::string::npos);
    REQUIRE(json.find("\"cycleCount\":3") != std::string::npos);
    REQUIRE(json.find("\"receiverProgress\":null") != std::string::npos);
    REQUIRE(json.find("\"receiverEta\":null") != std::string::npos);
    REQUIRE(json.find("\"verifiedGoodput\":null") != std::string::npos);
    REQUIRE(json.find("\"runStartedUnixMilliseconds\":1000") != std::string::npos);
    REQUIRE(json.find("\"runEndedUnixMilliseconds\":2000") != std::string::npos);
    REQUIRE(json.find("\"dataWindow\":{\"left\":2560") != std::string::npos);
    REQUIRE(json.find("\"generatedVisualFramesPerSecond\":null") != std::string::npos);
    REQUIRE(json.find("transferProgress") == std::string::npos);
}

TEST_CASE("Decoder report preserves verified progress and final acceptance independently", "[application][report][decoder]")
{
    pbapp::DecoderSnapshot snapshot;
    snapshot.state = pbapp::DecoderState::Failed;
    snapshot.descriptorKnown = true;
    snapshot.originalFileBytes = 100;
    snapshot.verifiedRawBytes = 100;
    snapshot.remainingRawBytes = 0;
    snapshot.recoveryProgress = 1.0;
    snapshot.smoothedVerifiedRawGoodputBytesPerSecond = 4096.0;
    snapshot.verifiedEncodedBytes = 80;
    snapshot.verifiedEncodedGoodputBitsPerSecond = 512.0;
    snapshot.captureFps = 60.0;
    snapshot.fingerprintedFrames = 0;
    snapshot.uniqueVisualFps.reset();
    snapshot.admittedFrameSequenceFps = 59.5;
    snapshot.wholeFileDigestVerified = false;
    snapshot.finalPublishSucceeded = false;
    snapshot.backendReason = "WGC unavailable; no fallback was attempted";
    snapshot.roiLeft = 10;
    snapshot.roiTop = 20;
    snapshot.roiWidth = 1920;
    snapshot.roiHeight = 1080;
    snapshot.comparedCodedBits = 16200;
    snapshot.erroneousCodedBits = 7;
    const pbapp::RunReportContext context{"PixelBridgeDecoder", "0.1.0", "deadbeef", "2026-08-30T00:00:00Z"};
    const std::string json = pbapp::BuildDecoderRunReportJson(context, snapshot);
    REQUIRE(json.find("\"state\":\"Failed\"") != std::string::npos);
    REQUIRE(json.find("\"recoveryProgress\":1") != std::string::npos);
    REQUIRE(json.find("\"wholeFileDigestVerified\":false") != std::string::npos);
    REQUIRE(json.find("\"finalPublishSucceeded\":false") != std::string::npos);
    REQUIRE(json.find("\"actualBackend\":null") != std::string::npos);
    REQUIRE(json.find("no fallback was attempted") != std::string::npos);
    REQUIRE(json.find("\"roi\":{\"left\":10,\"top\":20,\"width\":1920,\"height\":1080") != std::string::npos);
    REQUIRE(json.find("\"comparedCodedBits\":16200") != std::string::npos);
    REQUIRE(json.find("\"erroneousCodedBits\":7") != std::string::npos);
    REQUIRE(json.find("\"smoothedVerifiedRawGoodputBytesPerSecond\":4096") != std::string::npos);
    REQUIRE(json.find("\"verifiedRawGoodputBasis\":\"verifiedRawBytes\"") != std::string::npos);
    REQUIRE(json.find("\"verifiedEncodedBytes\":80") != std::string::npos);
    REQUIRE(json.find("\"verifiedEncodedGoodputBitsPerSecond\":512") != std::string::npos);
    REQUIRE(json.find("\"verifiedEncodedGoodputGate\":\"WholeFileDigest+finalPublish\"") != std::string::npos);
    REQUIRE(json.find("\"captureFps\":60") != std::string::npos);
    REQUIRE(json.find("\"fingerprintedFrames\":0") != std::string::npos);
    REQUIRE(json.find("\"uniqueVisualFps\":null") != std::string::npos);
    REQUIRE(json.find("\"admittedFrameSequenceFps\":59.5") != std::string::npos);
    REQUIRE(json.find("smoothedVerifiedGoodputBytesPerSecond") == std::string::npos);
    REQUIRE(json.find("duplicateVisualFrames") == std::string::npos);
}
