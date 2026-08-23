#include "descriptor_test_helpers.h"

#include "pbprotocol/descriptor_codec.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string_view>

#if !defined(PB_DESCRIPTOR_CORPUS_DIRECTORY)
#error "PB_DESCRIPTOR_CORPUS_DIRECTORY must identify the descriptor corpus."
#endif

namespace {

[[nodiscard]] pbprotocol::ReceiverResourcePolicy MakeCorpusResourcePolicy() noexcept
{
    pbprotocol::ReceiverResourcePolicy resourcePolicy{};
    resourcePolicy.maxAcceptedFileBytes = 1024ULL * 1024ULL;
    resourcePolicy.maxSegmentCount = 64;
    resourcePolicy.maxRawSegmentBytes = 64ULL * 1024ULL;
    resourcePolicy.maxEncodedSegmentBytes = 64ULL * 1024ULL;
    resourcePolicy.maxOuterBlockBytes = 4096;
    resourcePolicy.maxDescriptorStateBytes = 1024ULL * 1024ULL;
    resourcePolicy.maxConcurrentSessions = 2;
    resourcePolicy.maxTotalDescriptorStateBytes = 2ULL * 1024ULL * 1024ULL;
    resourcePolicy.maxDirectRepeatBlockCount = 64;
    resourcePolicy.maxActiveOuterFecDecoders = 2;
    resourcePolicy.maxOuterFecDecoderBytes = 16ULL * 1024ULL * 1024ULL;
    resourcePolicy.maxTotalOuterFecDecoderBytes =
        32ULL * 1024ULL * 1024ULL;
    return resourcePolicy;
}

[[nodiscard]] pbprotocol::SessionDescriptor MakeFixedSessionDescriptor() noexcept
{
    return pbprotocol::SessionDescriptor{
        pbprotocol::GetProtocolVersion(),
        pbprotocol::test::MakeSessionId(),
        64,
        8,
        pbprotocol::DigestAlgorithm::Blake3_256};
}

template <std::size_t ByteCount>
[[nodiscard]] std::array<std::byte, ByteCount> ReadCorpusFile(
    const std::string_view fileName)
{
    const std::filesystem::path filePath =
        std::filesystem::path(PB_DESCRIPTOR_CORPUS_DIRECTORY) / fileName;
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

TEST_CASE("Independent SessionDescriptor corpus seed has exact semantics",
          "[pbprotocol][descriptor][corpus][conformance]")
{
    const auto payload =
        ReadCorpusFile<pbprotocol::kSessionDescriptorPayloadBytes>(
            "valid-session.bin");
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        MakeCorpusResourcePolicy();
    const pbprotocol::SessionDescriptor expected = MakeFixedSessionDescriptor();

    const auto parsedResult = pbprotocol::ParseSessionDescriptor(
        payload,
        resourcePolicy);
    REQUIRE(parsedResult);
    REQUIRE(parsedResult.Value() == expected);

    std::array<std::byte, pbprotocol::kSessionDescriptorPayloadBytes>
        serialized{};
    REQUIRE(pbprotocol::SerializeSessionDescriptor(
        parsedResult.Value(),
        resourcePolicy,
        serialized));
    REQUIRE(serialized == payload);
}

TEST_CASE("Independent DirectRepeat corpus seed has exact semantics",
          "[pbprotocol][descriptor][corpus][conformance]")
{
    const auto payload = ReadCorpusFile<
        pbprotocol::kDirectRepeatSegmentDescriptorPayloadBytes>(
            "valid-direct-segment.bin");
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        MakeCorpusResourcePolicy();
    const pbprotocol::SessionDescriptor sessionDescriptor =
        MakeFixedSessionDescriptor();
    const pbprotocol::RawDigest rawDigest{};
    const pbprotocol::SegmentDescriptor expected{
        pbprotocol::DeriveSessionTag(sessionDescriptor.sessionId),
        0,
        0,
        8,
        8,
        pbprotocol::CompressionCodec::Raw,
        pbprotocol::OuterFecMode::DirectRepeat,
        8,
        rawDigest,
        pbprotocol::EncodedDigest{rawDigest.bytes},
        std::nullopt};

    const auto parsedResult = pbprotocol::ParseSegmentDescriptor(
        payload,
        sessionDescriptor,
        resourcePolicy);
    REQUIRE(parsedResult);
    REQUIRE(parsedResult.Value() == expected);

    std::array<
        std::byte,
        pbprotocol::kDirectRepeatSegmentDescriptorPayloadBytes> serialized{};
    REQUIRE(pbprotocol::SerializeSegmentDescriptor(
        parsedResult.Value(),
        sessionDescriptor,
        resourcePolicy,
        serialized));
    REQUIRE(serialized == payload);
}

TEST_CASE("Independent FinalManifest corpus seed is structural metadata",
          "[pbprotocol][descriptor][corpus][conformance]")
{
    const auto payload =
        ReadCorpusFile<pbprotocol::kFinalManifestPayloadBytes>(
            "valid-final-manifest.bin");
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        MakeCorpusResourcePolicy();
    const pbprotocol::SessionDescriptor sessionDescriptor =
        MakeFixedSessionDescriptor();
    const pbprotocol::FinalManifest expected{
        sessionDescriptor.sessionId,
        sessionDescriptor.originalFileSize,
        sessionDescriptor.segmentCount,
        pbprotocol::WholeFileDigest{},
        sessionDescriptor.digestAlgorithm};

    const auto parsedResult = pbprotocol::ParseFinalManifest(
        payload,
        sessionDescriptor,
        resourcePolicy);
    REQUIRE(parsedResult);
    REQUIRE(parsedResult.Value() == expected);

    std::array<std::byte, pbprotocol::kFinalManifestPayloadBytes> serialized{};
    REQUIRE(pbprotocol::SerializeFinalManifest(
        parsedResult.Value(),
        sessionDescriptor,
        resourcePolicy,
        serialized));
    REQUIRE(serialized == payload);
}

TEST_CASE("Independent overflow corpus seed reports the exact arithmetic error",
          "[pbprotocol][descriptor][corpus][conformance][overflow]")
{
    const auto payload = ReadCorpusFile<
        pbprotocol::kDirectRepeatSegmentDescriptorPayloadBytes>(
            "overflow-direct-segment.bin");
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        MakeCorpusResourcePolicy();
    const pbprotocol::SessionDescriptor sessionDescriptor =
        MakeFixedSessionDescriptor();

    const auto parsedResult = pbprotocol::ParseSegmentDescriptor(
        payload,
        sessionDescriptor,
        resourcePolicy);
    REQUIRE_FALSE(parsedResult);
    REQUIRE(
        parsedResult.Error() ==
        pbprotocol::ProtocolError{
            pbprotocol::ProtocolErrorCode::LengthOverflow,
            16});
}
