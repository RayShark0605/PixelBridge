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
    resourcePolicy.maxControlRecordBytes = 64U * 1024U;
    resourcePolicy.maxConcurrentControlReassemblies = 4;
    resourcePolicy.maxControlReassemblyBytes = 1024ULL * 1024ULL;
    resourcePolicy.maxControlFragmentsPerRecord = 4096;
    resourcePolicy.maxControlReassemblyInactivityObservations = 16384;
    resourcePolicy.maxOrphanTransportBytes = 4ULL * 1024ULL * 1024ULL;
    resourcePolicy.maxOrphanTransportBlocks = 64;
    resourcePolicy.maxZstdWindowBytes = 8ULL * 1024ULL * 1024ULL;
    resourcePolicy.maxResumeBytes = 256ULL * 1024ULL * 1024ULL;
    // Must stay <= maxAcceptedFileBytes (cross-field policy invariant).
    resourcePolicy.maxOutputPreallocationBytesWithoutPrompt =
        512ULL * 1024ULL;
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
            pbprotocol::kFormalWireSegmentDescriptorRawOffsetOffset});
}

TEST_CASE("Independent Wirehair V2 corpus seed has canonical formal schema",
          "[pbprotocol][descriptor][corpus][wirehair][conformance]")
{
    const auto payload = ReadCorpusFile<
        pbprotocol::kWirehairV2SegmentDescriptorPayloadBytes>(
            "valid-wirehair-segment.bin");
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        MakeCorpusResourcePolicy();
    const pbprotocol::SessionDescriptor sessionDescriptor =
        MakeFixedSessionDescriptor();
    const pbprotocol::RawDigest rawDigest{};
    const pbprotocol::SegmentDescriptor expected{
        pbprotocol::DeriveSessionTag(sessionDescriptor.sessionId),
        0,
        0,
        16,
        16,
        pbprotocol::CompressionCodec::Raw,
        pbprotocol::OuterFecMode::WirehairV2,
        8,
        rawDigest,
        pbprotocol::EncodedDigest{rawDigest.bytes},
        pbprotocol::test::MakeWirehairProfile(16, 8),
        0};

    const auto parsedResult = pbprotocol::ParseSegmentDescriptor(
        payload,
        sessionDescriptor,
        resourcePolicy);
    REQUIRE(parsedResult);
    REQUIRE(parsedResult.Value() == expected);
}

TEST_CASE("Historical Phase-0 descriptor fixtures are rejected as unsupported schema",
          "[pbprotocol][descriptor][corpus][legacy][regression]")
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        MakeCorpusResourcePolicy();
    const pbprotocol::SessionDescriptor sessionDescriptor =
        MakeFixedSessionDescriptor();
    const auto legacySession = ReadCorpusFile<37>("legacy-phase0-session.bin");
    const auto legacySegment = ReadCorpusFile<110>("legacy-phase0-direct-segment.bin");
    const auto legacyManifest = ReadCorpusFile<65>("legacy-phase0-final-manifest.bin");

    const auto sessionResult = pbprotocol::ParseSessionDescriptor(legacySession, resourcePolicy);
    const auto segmentResult = pbprotocol::ParseSegmentDescriptor(legacySegment, sessionDescriptor, resourcePolicy);
    const auto manifestResult = pbprotocol::ParseFinalManifest(legacyManifest, sessionDescriptor, resourcePolicy);
    REQUIRE_FALSE(sessionResult);
    REQUIRE_FALSE(segmentResult);
    REQUIRE_FALSE(manifestResult);
    REQUIRE(sessionResult.Error().code == pbprotocol::ProtocolErrorCode::UnsupportedDescriptorSchema);
    REQUIRE(segmentResult.Error().code == pbprotocol::ProtocolErrorCode::UnsupportedDescriptorSchema);
    REQUIRE(manifestResult.Error().code == pbprotocol::ProtocolErrorCode::UnsupportedDescriptorSchema);
}
