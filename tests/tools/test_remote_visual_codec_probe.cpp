#include "codec_probe_core.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace
{

std::vector<std::byte> ConvertBgraToGray(const pbremotevisualcodecprobe::CodecSourceSequence& source)
{
    std::vector<std::byte> output(pbremotevisualcodecprobe::kGrayFrameBytes *
        pbremotevisualcodecprobe::kSourceFrameCount);
    for (std::size_t index = 0; index < output.size(); index++)
    {
        const std::size_t sourceOffset = index * 4;
        REQUIRE(source.bgraFrames[sourceOffset] == source.bgraFrames[sourceOffset + 1]);
        REQUIRE(source.bgraFrames[sourceOffset] == source.bgraFrames[sourceOffset + 2]);
        REQUIRE(source.bgraFrames[sourceOffset + 3] == std::byte{255});
        output[index] = source.bgraFrames[sourceOffset];
    }
    return output;
}

} // namespace

TEST_CASE("RemoteVisual codec probe source sequence is deterministic and production encoded",
    "[tools][remote-visual][codec][source][determinism]")
{
    pbremotevisualcodecprobe::CodecSourceSequence first;
    pbremotevisualcodecprobe::CodecSourceSequence second;
    std::string firstError;
    std::string secondError;
    REQUIRE(pbremotevisualcodecprobe::BuildCanonicalSourceSequence(first, firstError));
    REQUIRE(pbremotevisualcodecprobe::BuildCanonicalSourceSequence(second, secondError));
    REQUIRE(firstError.empty());
    REQUIRE(secondError.empty());
    REQUIRE(first.bgraFrames == second.bgraFrames);
    REQUIRE(first.canonicalManifestJson == second.canonicalManifestJson);
    REQUIRE(first.bgraFrames.size() == pbremotevisualcodecprobe::kBgraFrameBytes *
        pbremotevisualcodecprobe::kSourceFrameCount);
    REQUIRE(first.canonicalManifestJson.starts_with(
        "{\"schema\":\"PixelBridge.RemoteVisualCodecSource.1\",\"version\":1,"));
    REQUIRE(first.canonicalManifestJson.find("\"frameSequence\":900") != std::string::npos);
    REQUIRE(first.canonicalManifestJson.find("\"frameSequence\":902") != std::string::npos);
}

TEST_CASE("RemoteVisual codec probe evaluates Gray8 sequences through the production truth boundary",
    "[tools][remote-visual][codec][gray8][fec][transport]")
{
    pbremotevisualcodecprobe::CodecSourceSequence source;
    std::string error;
    REQUIRE(pbremotevisualcodecprobe::BuildCanonicalSourceSequence(source, error));
    const std::vector<std::byte> gray = ConvertBgraToGray(source);
    pbremotevisualcodecprobe::CodecSequenceEvaluation first;
    pbremotevisualcodecprobe::CodecSequenceEvaluation second;
    REQUIRE(pbremotevisualcodecprobe::EvaluateGray8Sequence(gray, first, error));
    REQUIRE(pbremotevisualcodecprobe::EvaluateGray8Sequence(gray, second, error));
    REQUIRE(first.canonicalJson == second.canonicalJson);
    REQUIRE(first.truthBoundaryValid);
    REQUIRE(first.allFramesVerified);
    REQUIRE(first.frames.size() == pbremotevisualcodecprobe::kSourceFrameCount);
    for (std::size_t index = 0; index < first.frames.size(); index++)
    {
        const auto& frame = first.frames[index];
        REQUIRE(frame.index == index);
        REQUIRE(frame.classification == "Verified");
        REQUIRE(frame.bootstrapAccepted);
        REQUIRE(frame.frameSequence == 900 + index);
        REQUIRE(frame.acceptedTransportBlocks == 4);
        REQUIRE(frame.falseAcceptedCodewords == 0);
    }
    REQUIRE(first.canonicalJson.find("\"acceptedTransportBlocks\":12") != std::string::npos);
    REQUIRE(first.canonicalJson.find("\"falseAcceptedCodewords\":0") != std::string::npos);
}

TEST_CASE("RemoteVisual codec probe rejects malformed sequences and fails closed on a destroyed frame",
    "[tools][remote-visual][codec][negative][resource]")
{
    std::string error;
    pbremotevisualcodecprobe::CodecSequenceEvaluation untouched;
    untouched.canonicalJson = "sentinel";
    const std::vector<std::byte> empty;
    REQUIRE_FALSE(pbremotevisualcodecprobe::EvaluateGray8Sequence(empty, untouched, error));
    REQUIRE(untouched.canonicalJson == "sentinel");
    const std::vector<std::byte> truncated(pbremotevisualcodecprobe::kGrayFrameBytes - 1);
    REQUIRE_FALSE(pbremotevisualcodecprobe::EvaluateGray8Sequence(truncated, untouched, error));
    REQUIRE(untouched.canonicalJson == "sentinel");
    const std::vector<std::byte> tooMany(pbremotevisualcodecprobe::kGrayFrameBytes *
        (pbremotevisualcodecprobe::kMaximumEvaluationFrames + 1));
    REQUIRE_FALSE(pbremotevisualcodecprobe::EvaluateGray8Sequence(tooMany, untouched, error));
    REQUIRE(untouched.canonicalJson == "sentinel");

    pbremotevisualcodecprobe::CodecSourceSequence source;
    REQUIRE(pbremotevisualcodecprobe::BuildCanonicalSourceSequence(source, error));
    std::vector<std::byte> gray = ConvertBgraToGray(source);
    std::fill_n(gray.begin() + static_cast<std::ptrdiff_t>(pbremotevisualcodecprobe::kGrayFrameBytes),
        pbremotevisualcodecprobe::kGrayFrameBytes, std::byte{128});
    pbremotevisualcodecprobe::CodecSequenceEvaluation evaluation;
    REQUIRE(pbremotevisualcodecprobe::EvaluateGray8Sequence(gray, evaluation, error));
    REQUIRE(evaluation.truthBoundaryValid);
    REQUIRE_FALSE(evaluation.allFramesVerified);
    REQUIRE(evaluation.frames[0].classification == "Verified");
    REQUIRE(evaluation.frames[1].classification == "ErasureNoFalseAccept");
    REQUIRE(evaluation.frames[1].acceptedTransportBlocks == 0);
    REQUIRE(evaluation.frames[1].falseAcceptedCodewords == 0);
    REQUIRE(evaluation.frames[2].classification == "Verified");
}

TEST_CASE("RemoteVisual codec probe distinguishes FEC erasure from valid-CRC wrong identity",
    "[tools][remote-visual][codec][adversarial][identity][crc]")
{
    std::string error;
    std::vector<std::byte> transportErasure;
    REQUIRE(pbremotevisualcodecprobe::BuildAdversarialGrayFrame(
        pbremotevisualcodecprobe::CodecAdversarialFrameKind::TransportErasure, transportErasure, error));
    REQUIRE(transportErasure.size() == pbremotevisualcodecprobe::kGrayFrameBytes);
    pbremotevisualcodecprobe::CodecSequenceEvaluation erasureEvaluation;
    REQUIRE(pbremotevisualcodecprobe::EvaluateGray8Sequence(transportErasure, erasureEvaluation, error));
    REQUIRE(erasureEvaluation.truthBoundaryValid);
    REQUIRE_FALSE(erasureEvaluation.allFramesVerified);
    REQUIRE(erasureEvaluation.frames.size() == 1);
    CAPTURE(erasureEvaluation.frames[0].classification, erasureEvaluation.frames[0].fecFailures,
        erasureEvaluation.frames[0].crcFailures, erasureEvaluation.frames[0].identityFailures);
    REQUIRE(erasureEvaluation.frames[0].bootstrapAccepted);
    REQUIRE(erasureEvaluation.frames[0].fecFailures == 4);
    REQUIRE(erasureEvaluation.frames[0].crcFailures == 0);
    REQUIRE(erasureEvaluation.frames[0].identityFailures == 0);
    REQUIRE(erasureEvaluation.frames[0].acceptedTransportBlocks == 0);
    REQUIRE(erasureEvaluation.frames[0].falseAcceptedCodewords == 0);

    std::vector<std::byte> wrongIdentity;
    REQUIRE(pbremotevisualcodecprobe::BuildAdversarialGrayFrame(
        pbremotevisualcodecprobe::CodecAdversarialFrameKind::ValidCrcWrongIdentity, wrongIdentity, error));
    REQUIRE(wrongIdentity.size() == pbremotevisualcodecprobe::kGrayFrameBytes);
    pbremotevisualcodecprobe::CodecSequenceEvaluation identityEvaluation;
    REQUIRE(pbremotevisualcodecprobe::EvaluateGray8Sequence(wrongIdentity, identityEvaluation, error));
    REQUIRE_FALSE(identityEvaluation.truthBoundaryValid);
    REQUIRE_FALSE(identityEvaluation.allFramesVerified);
    REQUIRE(identityEvaluation.frames.size() == 1);
    CAPTURE(identityEvaluation.frames[0].classification, identityEvaluation.frames[0].fecFailures,
        identityEvaluation.frames[0].crcFailures, identityEvaluation.frames[0].identityFailures,
        identityEvaluation.frames[0].falseAcceptedCodewords);
    REQUIRE(identityEvaluation.frames[0].bootstrapAccepted);
    REQUIRE(identityEvaluation.frames[0].fecFailures == 0);
    REQUIRE(identityEvaluation.frames[0].crcFailures == 0);
    REQUIRE(identityEvaluation.frames[0].identityFailures == 4);
    REQUIRE(identityEvaluation.frames[0].acceptedTransportBlocks == 0);
    REQUIRE(identityEvaluation.frames[0].falseAcceptedCodewords == 4);
    REQUIRE(identityEvaluation.canonicalJson.find("\"identityFailures\":4") != std::string::npos);

    std::vector<std::byte> unchanged{std::byte{0x5A}};
    REQUIRE_FALSE(pbremotevisualcodecprobe::BuildAdversarialGrayFrame(
        static_cast<pbremotevisualcodecprobe::CodecAdversarialFrameKind>(0xFF), unchanged, error));
    REQUIRE(unchanged == std::vector<std::byte>{std::byte{0x5A}});
}
