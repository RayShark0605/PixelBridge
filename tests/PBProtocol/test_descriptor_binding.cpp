#include "descriptor_test_helpers.h"

#include "pbprotocol/descriptor_binding.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace {

void RequireSegmentConflict(
    const pbprotocol::SessionDescriptor& sessionDescriptor,
    const pbprotocol::ReceiverResourcePolicy& resourcePolicy,
    const pbprotocol::SegmentDescriptor& firstDescriptor,
    const pbprotocol::SegmentDescriptor& conflictingDescriptor)
{
    REQUIRE(pbprotocol::ValidateSegmentDescriptor(
        firstDescriptor,
        sessionDescriptor,
        resourcePolicy));
    REQUIRE(pbprotocol::ValidateSegmentDescriptor(
        conflictingDescriptor,
        sessionDescriptor,
        resourcePolicy));

    auto stateResult = pbprotocol::DescriptorBindingState::Create(
        sessionDescriptor,
        resourcePolicy);
    REQUIRE(stateResult);
    auto state = std::move(stateResult).Value();

    const auto firstResult = state.BindSegmentDescriptor(firstDescriptor);
    REQUIRE(firstResult);
    REQUIRE(
        firstResult.Value() ==
        pbprotocol::DescriptorBindDisposition::Inserted);

    const auto conflictResult = state.BindSegmentDescriptor(
        conflictingDescriptor);
    REQUIRE_FALSE(conflictResult);
    REQUIRE(
        conflictResult.Error().code ==
        pbprotocol::ProtocolErrorCode::DescriptorConflict);
    REQUIRE(state.HasTerminalError());
    REQUIRE(
        state.TerminalError() ==
        pbprotocol::ProtocolErrorCode::DescriptorConflict);
}

void RequireOverlap(
    const pbprotocol::SegmentDescriptor& firstDescriptor,
    const pbprotocol::SegmentDescriptor& overlappingDescriptor)
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::test::MakeResourcePolicy();
    const pbprotocol::SessionDescriptor sessionDescriptor =
        pbprotocol::test::MakeSessionDescriptor(200, 2);
    REQUIRE(pbprotocol::ValidateSegmentDescriptor(
        firstDescriptor,
        sessionDescriptor,
        resourcePolicy));
    REQUIRE(pbprotocol::ValidateSegmentDescriptor(
        overlappingDescriptor,
        sessionDescriptor,
        resourcePolicy));

    auto stateResult = pbprotocol::DescriptorBindingState::Create(
        sessionDescriptor,
        resourcePolicy);
    REQUIRE(stateResult);
    auto state = std::move(stateResult).Value();

    REQUIRE(state.BindSegmentDescriptor(firstDescriptor));
    const auto overlapResult = state.BindSegmentDescriptor(
        overlappingDescriptor);
    REQUIRE_FALSE(overlapResult);
    REQUIRE(
        overlapResult.Error().code ==
        pbprotocol::ProtocolErrorCode::SegmentOverlap);
    REQUIRE(state.HasTerminalError());
}

void RequireCompletedGap(
    const pbprotocol::SegmentDescriptor& firstDescriptor,
    const pbprotocol::SegmentDescriptor& secondDescriptor)
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::test::MakeResourcePolicy();
    const pbprotocol::SessionDescriptor sessionDescriptor =
        pbprotocol::test::MakeSessionDescriptor(200, 2);

    auto stateResult = pbprotocol::DescriptorBindingState::Create(
        sessionDescriptor,
        resourcePolicy);
    REQUIRE(stateResult);
    auto state = std::move(stateResult).Value();

    REQUIRE(state.BindSegmentDescriptor(firstDescriptor));
    REQUIRE(state.BindSegmentDescriptor(secondDescriptor));

    const pbprotocol::ProtocolStatus mapStatus =
        state.ValidateCompleteSegmentMap();
    REQUIRE_FALSE(mapStatus);
    REQUIRE(
        mapStatus.Error().code ==
        pbprotocol::ProtocolErrorCode::SegmentGap);
    REQUIRE(state.HasTerminalError());
}

} // namespace

TEST_CASE("Binding creation rejects untrusted huge SegmentCount before state exists",
          "[pbprotocol][binding][resource]")
{
    pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::test::MakeResourcePolicy();
    resourcePolicy.maxSegmentCount = 1024;
    const pbprotocol::SessionDescriptor maliciousSession =
        pbprotocol::test::MakeSessionDescriptor(
            std::numeric_limits<std::uint64_t>::max(),
            std::numeric_limits<std::uint64_t>::max());

    const auto stateResult = pbprotocol::DescriptorBindingState::Create(
        maliciousSession,
        resourcePolicy);

    REQUIRE_FALSE(stateResult);
    REQUIRE(
        stateResult.Error().code ==
        pbprotocol::ProtocolErrorCode::ResourceLimitExceeded);
}

TEST_CASE("SessionDescriptor binding is immutable and rejects different keys",
          "[pbprotocol][binding][session][conflict]")
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::test::MakeResourcePolicy();
    const pbprotocol::SessionDescriptor sessionDescriptor =
        pbprotocol::test::MakeSessionDescriptor(200, 2);

    SECTION("identical repetition is idempotent")
    {
        auto stateResult = pbprotocol::DescriptorBindingState::Create(
            sessionDescriptor,
            resourcePolicy);
        REQUIRE(stateResult);
        auto state = std::move(stateResult).Value();

        const auto repeatedResult = state.BindSessionDescriptor(
            sessionDescriptor);
        REQUIRE(repeatedResult);
        REQUIRE(
            repeatedResult.Value() ==
            pbprotocol::DescriptorBindDisposition::Repeated);
        REQUIRE_FALSE(state.HasTerminalError());
    }

    SECTION("same SessionId with different valid size conflicts and locks")
    {
        auto stateResult = pbprotocol::DescriptorBindingState::Create(
            sessionDescriptor,
            resourcePolicy);
        REQUIRE(stateResult);
        auto state = std::move(stateResult).Value();
        auto conflict = sessionDescriptor;
        conflict.originalFileSize = 201;

        const auto conflictResult = state.BindSessionDescriptor(conflict);
        REQUIRE_FALSE(conflictResult);
        REQUIRE(
            conflictResult.Error().code ==
            pbprotocol::ProtocolErrorCode::DescriptorConflict);
        REQUIRE(state.HasTerminalError());

        const auto afterConflict = state.BindSessionDescriptor(
            sessionDescriptor);
        REQUIRE_FALSE(afterConflict);
        REQUIRE(
            afterConflict.Error().code ==
            pbprotocol::ProtocolErrorCode::DescriptorConflict);
    }

    SECTION("same SessionId with different valid count conflicts")
    {
        auto stateResult = pbprotocol::DescriptorBindingState::Create(
            sessionDescriptor,
            resourcePolicy);
        REQUIRE(stateResult);
        auto state = std::move(stateResult).Value();
        auto conflict = sessionDescriptor;
        conflict.segmentCount = 3;

        const auto conflictResult = state.BindSessionDescriptor(conflict);
        REQUIRE_FALSE(conflictResult);
        REQUIRE(
            conflictResult.Error().code ==
            pbprotocol::ProtocolErrorCode::DescriptorConflict);
    }

    SECTION("different key and invalid candidate do not poison the binding")
    {
        auto stateResult = pbprotocol::DescriptorBindingState::Create(
            sessionDescriptor,
            resourcePolicy);
        REQUIRE(stateResult);
        auto state = std::move(stateResult).Value();

        auto differentKey = sessionDescriptor;
        differentKey.sessionId.bytes[0] ^= std::byte{0x80};
        const auto differentKeyResult = state.BindSessionDescriptor(
            differentKey);
        REQUIRE_FALSE(differentKeyResult);
        REQUIRE(
            differentKeyResult.Error().code ==
            pbprotocol::ProtocolErrorCode::SessionMismatch);
        REQUIRE_FALSE(state.HasTerminalError());

        auto invalidCandidate = sessionDescriptor;
        invalidCandidate.protocolVersion.minor++;
        const auto invalidResult = state.BindSessionDescriptor(
            invalidCandidate);
        REQUIRE_FALSE(invalidResult);
        REQUIRE(
            invalidResult.Error().code ==
            pbprotocol::ProtocolErrorCode::UnsupportedProtocolMinor);
        REQUIRE_FALSE(state.HasTerminalError());

        REQUIRE(state.BindSessionDescriptor(sessionDescriptor));
    }
}

TEST_CASE("Every mutable SegmentDescriptor field participates in binding",
          "[pbprotocol][binding][segment][conflict]")
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::test::MakeResourcePolicy();
    const pbprotocol::SessionDescriptor sessionDescriptor =
        pbprotocol::test::MakeSessionDescriptor(1000, 1);
    const pbprotocol::SegmentDescriptor wirehairDescriptor =
        pbprotocol::test::MakeWirehairSegment(
            sessionDescriptor,
            0,
            0,
            117);

    SECTION("RawOffset")
    {
        auto conflict = wirehairDescriptor;
        conflict.rawOffset = 1;
        RequireSegmentConflict(
            sessionDescriptor,
            resourcePolicy,
            wirehairDescriptor,
            conflict);
    }

    SECTION("RawSize")
    {
        auto conflict = wirehairDescriptor;
        conflict.rawSize = 118;
        RequireSegmentConflict(
            sessionDescriptor,
            resourcePolicy,
            wirehairDescriptor,
            conflict);
    }

    SECTION("EncodedSize")
    {
        auto directDescriptor = wirehairDescriptor;
        directDescriptor.outerFecMode = pbprotocol::OuterFecMode::DirectRepeat;
        directDescriptor.wirehairV2SerializedProfile.reset();
        auto conflict = directDescriptor;
        conflict.encodedSize = 118;
        RequireSegmentConflict(
            sessionDescriptor,
            resourcePolicy,
            directDescriptor,
            conflict);
    }

    SECTION("CompressionCodec")
    {
        auto codecDescriptor = wirehairDescriptor;
        codecDescriptor.rawDigest.bytes = codecDescriptor.encodedDigest.bytes;
        auto conflict = codecDescriptor;
        conflict.compressionCodec = pbprotocol::CompressionCodec::Raw;
        RequireSegmentConflict(
            sessionDescriptor,
            resourcePolicy,
            codecDescriptor,
            conflict);
    }

    SECTION("OuterFecMode and required profile presence")
    {
        auto directDescriptor = wirehairDescriptor;
        directDescriptor.outerFecMode = pbprotocol::OuterFecMode::DirectRepeat;
        directDescriptor.wirehairV2SerializedProfile.reset();
        RequireSegmentConflict(
            sessionDescriptor,
            resourcePolicy,
            wirehairDescriptor,
            directDescriptor);
    }

    SECTION("OuterBlockBytes")
    {
        auto directDescriptor = wirehairDescriptor;
        directDescriptor.outerFecMode = pbprotocol::OuterFecMode::DirectRepeat;
        directDescriptor.wirehairV2SerializedProfile.reset();
        auto conflict = directDescriptor;
        conflict.outerBlockBytes = 17;
        RequireSegmentConflict(
            sessionDescriptor,
            resourcePolicy,
            directDescriptor,
            conflict);
    }

    SECTION("RawDigest")
    {
        auto conflict = wirehairDescriptor;
        conflict.rawDigest.bytes[0] ^= std::byte{0x01};
        RequireSegmentConflict(
            sessionDescriptor,
            resourcePolicy,
            wirehairDescriptor,
            conflict);
    }

    SECTION("EncodedDigest")
    {
        auto conflict = wirehairDescriptor;
        conflict.encodedDigest.bytes[0] ^= std::byte{0x01};
        RequireSegmentConflict(
            sessionDescriptor,
            resourcePolicy,
            wirehairDescriptor,
            conflict);
    }

    SECTION("full canonical Wirehair profile")
    {
        auto conflict = wirehairDescriptor;
        conflict.wirehairV2SerializedProfile->bytes[28] = std::byte{0x01};
        RequireSegmentConflict(
            sessionDescriptor,
            resourcePolicy,
            wirehairDescriptor,
            conflict);
    }
}

TEST_CASE("Segment binding distinguishes repetition invalid candidates and keys",
          "[pbprotocol][binding][segment]")
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::test::MakeResourcePolicy();
    const pbprotocol::SessionDescriptor sessionDescriptor =
        pbprotocol::test::MakeSessionDescriptor(200, 2);
    const pbprotocol::SegmentDescriptor firstDescriptor =
        pbprotocol::test::MakeDirectRepeatSegment(
            sessionDescriptor,
            0,
            0,
            100);

    auto stateResult = pbprotocol::DescriptorBindingState::Create(
        sessionDescriptor,
        resourcePolicy);
    REQUIRE(stateResult);
    auto state = std::move(stateResult).Value();

    const auto insertedResult = state.BindSegmentDescriptor(firstDescriptor);
    REQUIRE(insertedResult);
    REQUIRE(
        insertedResult.Value() ==
        pbprotocol::DescriptorBindDisposition::Inserted);

    const auto repeatedResult = state.BindSegmentDescriptor(firstDescriptor);
    REQUIRE(repeatedResult);
    REQUIRE(
        repeatedResult.Value() ==
        pbprotocol::DescriptorBindDisposition::Repeated);
    REQUIRE(state.BoundSegmentCount() == 1);

    auto invalidCandidate = firstDescriptor;
    invalidCandidate.sessionTag.value++;
    const auto invalidResult = state.BindSegmentDescriptor(invalidCandidate);
    REQUIRE_FALSE(invalidResult);
    REQUIRE(
        invalidResult.Error().code ==
        pbprotocol::ProtocolErrorCode::SessionTagMismatch);
    REQUIRE_FALSE(state.HasTerminalError());

    const pbprotocol::SegmentDescriptor secondKey =
        pbprotocol::test::MakeDirectRepeatSegment(
            sessionDescriptor,
            1,
            100,
            100);
    const auto secondResult = state.BindSegmentDescriptor(secondKey);
    REQUIRE(secondResult);
    REQUIRE(
        secondResult.Value() ==
        pbprotocol::DescriptorBindDisposition::Inserted);
    REQUIRE(state.ValidateCompleteSegmentMap());
}

TEST_CASE("Segment Map accepts arbitrary arrival and ordinal order",
          "[pbprotocol][binding][map]")
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::test::MakeResourcePolicy();
    const pbprotocol::SessionDescriptor sessionDescriptor =
        pbprotocol::test::MakeSessionDescriptor(300, 3);
    const pbprotocol::SegmentDescriptor segmentOrdinalTwo =
        pbprotocol::test::MakeDirectRepeatSegment(
            sessionDescriptor,
            2,
            0,
            100);
    const pbprotocol::SegmentDescriptor segmentOrdinalZero =
        pbprotocol::test::MakeDirectRepeatSegment(
            sessionDescriptor,
            0,
            100,
            100);
    const pbprotocol::SegmentDescriptor segmentOrdinalOne =
        pbprotocol::test::MakeDirectRepeatSegment(
            sessionDescriptor,
            1,
            200,
            100);

    auto stateResult = pbprotocol::DescriptorBindingState::Create(
        sessionDescriptor,
        resourcePolicy);
    REQUIRE(stateResult);
    auto state = std::move(stateResult).Value();

    REQUIRE(state.BindSegmentDescriptor(segmentOrdinalOne));
    REQUIRE(state.BindSegmentDescriptor(segmentOrdinalTwo));

    const pbprotocol::ProtocolStatus incompleteStatus =
        state.ValidateCompleteSegmentMap();
    REQUIRE_FALSE(incompleteStatus);
    REQUIRE(
        incompleteStatus.Error().code ==
        pbprotocol::ProtocolErrorCode::SegmentMapIncomplete);
    REQUIRE_FALSE(state.HasTerminalError());

    REQUIRE(state.BindSegmentDescriptor(segmentOrdinalZero));
    REQUIRE(state.ValidateCompleteSegmentMap());
    REQUIRE(state.BoundSegmentCount() == 3);
}

TEST_CASE("Segment Map rejects all neighboring overlap shapes immediately",
          "[pbprotocol][binding][map][overlap]")
{
    const pbprotocol::SessionDescriptor sessionDescriptor =
        pbprotocol::test::MakeSessionDescriptor(200, 2);

    SECTION("same start")
    {
        RequireOverlap(
            pbprotocol::test::MakeDirectRepeatSegment(
                sessionDescriptor, 0, 0, 100),
            pbprotocol::test::MakeDirectRepeatSegment(
                sessionDescriptor, 1, 0, 50));
    }

    SECTION("partial overlap")
    {
        RequireOverlap(
            pbprotocol::test::MakeDirectRepeatSegment(
                sessionDescriptor, 0, 0, 120),
            pbprotocol::test::MakeDirectRepeatSegment(
                sessionDescriptor, 1, 100, 100));
    }

    SECTION("new interval nested inside existing interval")
    {
        RequireOverlap(
            pbprotocol::test::MakeDirectRepeatSegment(
                sessionDescriptor, 0, 0, 200),
            pbprotocol::test::MakeDirectRepeatSegment(
                sessionDescriptor, 1, 50, 50));
    }

    SECTION("new interval encloses existing interval")
    {
        RequireOverlap(
            pbprotocol::test::MakeDirectRepeatSegment(
                sessionDescriptor, 0, 50, 50),
            pbprotocol::test::MakeDirectRepeatSegment(
                sessionDescriptor, 1, 0, 200));
    }
}

TEST_CASE("Completed Segment Map rejects first middle and tail gaps",
          "[pbprotocol][binding][map][gap]")
{
    const pbprotocol::SessionDescriptor sessionDescriptor =
        pbprotocol::test::MakeSessionDescriptor(200, 2);

    SECTION("first gap")
    {
        RequireCompletedGap(
            pbprotocol::test::MakeDirectRepeatSegment(
                sessionDescriptor, 0, 10, 90),
            pbprotocol::test::MakeDirectRepeatSegment(
                sessionDescriptor, 1, 100, 100));
    }

    SECTION("middle gap")
    {
        RequireCompletedGap(
            pbprotocol::test::MakeDirectRepeatSegment(
                sessionDescriptor, 0, 0, 90),
            pbprotocol::test::MakeDirectRepeatSegment(
                sessionDescriptor, 1, 100, 100));
    }

    SECTION("tail gap")
    {
        RequireCompletedGap(
            pbprotocol::test::MakeDirectRepeatSegment(
                sessionDescriptor, 0, 0, 100),
            pbprotocol::test::MakeDirectRepeatSegment(
                sessionDescriptor, 1, 100, 90));
    }
}

TEST_CASE("FinalManifest binding is immutable and gates digest verification",
          "[pbprotocol][binding][final][conflict]")
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::test::MakeResourcePolicy();
    const pbprotocol::SessionDescriptor sessionDescriptor =
        pbprotocol::test::MakeSessionDescriptor(100, 1);
    const pbprotocol::SegmentDescriptor segmentDescriptor =
        pbprotocol::test::MakeDirectRepeatSegment(
            sessionDescriptor,
            0,
            0,
            100);
    const pbprotocol::FinalManifest finalManifest =
        pbprotocol::test::MakeFinalManifest(sessionDescriptor);

    SECTION("insert repeat ready and verify")
    {
        auto stateResult = pbprotocol::DescriptorBindingState::Create(
            sessionDescriptor,
            resourcePolicy);
        REQUIRE(stateResult);
        auto state = std::move(stateResult).Value();
        REQUIRE(state.BindSegmentDescriptor(segmentDescriptor));

        const pbprotocol::ProtocolStatus missingFinalStatus =
            state.ValidateReadyForFinalVerification();
        REQUIRE_FALSE(missingFinalStatus);
        REQUIRE(
            missingFinalStatus.Error().code ==
            pbprotocol::ProtocolErrorCode::MissingFinalManifest);
        REQUIRE_FALSE(state.HasTerminalError());

        const auto insertedResult = state.BindFinalManifest(finalManifest);
        REQUIRE(insertedResult);
        REQUIRE(
            insertedResult.Value() ==
            pbprotocol::DescriptorBindDisposition::Inserted);
        const auto repeatedResult = state.BindFinalManifest(finalManifest);
        REQUIRE(repeatedResult);
        REQUIRE(
            repeatedResult.Value() ==
            pbprotocol::DescriptorBindDisposition::Repeated);
        REQUIRE(state.HasFinalManifest());
        REQUIRE(state.ValidateReadyForFinalVerification());
        REQUIRE(state.VerifyWholeFileDigest(finalManifest.wholeFileDigest));
    }

    SECTION("different valid digest conflicts and locks")
    {
        auto stateResult = pbprotocol::DescriptorBindingState::Create(
            sessionDescriptor,
            resourcePolicy);
        REQUIRE(stateResult);
        auto state = std::move(stateResult).Value();
        REQUIRE(state.BindFinalManifest(finalManifest));
        auto conflict = finalManifest;
        conflict.wholeFileDigest.bytes[0] ^= std::byte{0x01};

        const auto conflictResult = state.BindFinalManifest(conflict);
        REQUIRE_FALSE(conflictResult);
        REQUIRE(
            conflictResult.Error().code ==
            pbprotocol::ProtocolErrorCode::DescriptorConflict);
        REQUIRE(state.HasTerminalError());
        REQUIRE_FALSE(state.BindSegmentDescriptor(segmentDescriptor));
    }

    SECTION("invalid candidate keeps the existing binding usable")
    {
        auto stateResult = pbprotocol::DescriptorBindingState::Create(
            sessionDescriptor,
            resourcePolicy);
        REQUIRE(stateResult);
        auto state = std::move(stateResult).Value();
        REQUIRE(state.BindFinalManifest(finalManifest));
        auto invalidCandidate = finalManifest;
        invalidCandidate.originalFileSize++;

        const auto invalidResult = state.BindFinalManifest(invalidCandidate);
        REQUIRE_FALSE(invalidResult);
        REQUIRE(
            invalidResult.Error().code ==
            pbprotocol::ProtocolErrorCode::SessionMismatch);
        REQUIRE_FALSE(state.HasTerminalError());
        REQUIRE(state.BindFinalManifest(finalManifest));
    }

    SECTION("computed digest mismatch locks the session")
    {
        auto stateResult = pbprotocol::DescriptorBindingState::Create(
            sessionDescriptor,
            resourcePolicy);
        REQUIRE(stateResult);
        auto state = std::move(stateResult).Value();
        REQUIRE(state.BindSegmentDescriptor(segmentDescriptor));
        REQUIRE(state.BindFinalManifest(finalManifest));
        auto wrongDigest = finalManifest.wholeFileDigest;
        wrongDigest.bytes[0] ^= std::byte{0x01};

        const pbprotocol::ProtocolStatus digestStatus =
            state.VerifyWholeFileDigest(wrongDigest);
        REQUIRE_FALSE(digestStatus);
        REQUIRE(
            digestStatus.Error().code ==
            pbprotocol::ProtocolErrorCode::DigestMismatch);
        REQUIRE(state.HasTerminalError());
    }
}

TEST_CASE("Zero-byte session has an empty map and fixed BLAKE3 manifest",
          "[pbprotocol][binding][empty]")
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::test::MakeResourcePolicy();
    const pbprotocol::SessionDescriptor sessionDescriptor =
        pbprotocol::test::MakeSessionDescriptor(0, 0);
    const pbprotocol::FinalManifest finalManifest =
        pbprotocol::test::MakeFinalManifest(sessionDescriptor);

    std::array<std::byte, pbprotocol::kSessionDescriptorPayloadBytes>
        sessionPayload{};
    REQUIRE(pbprotocol::SerializeSessionDescriptor(
        sessionDescriptor,
        resourcePolicy,
        sessionPayload));
    const auto parsedSessionResult = pbprotocol::ParseSessionDescriptor(
        sessionPayload,
        resourcePolicy);
    REQUIRE(parsedSessionResult);
    const pbprotocol::SessionDescriptor parsedSession =
        parsedSessionResult.Value();

    auto stateResult = pbprotocol::DescriptorBindingState::Create(
        parsedSession,
        resourcePolicy);
    REQUIRE(stateResult);
    auto state = std::move(stateResult).Value();

    REQUIRE(state.ValidateCompleteSegmentMap());
    REQUIRE(state.BoundSegmentCount() == 0);

    const pbprotocol::SegmentDescriptor impossibleSegment =
        pbprotocol::test::MakeDirectRepeatSegment(
            parsedSession,
            0,
            0,
            1);
    const auto segmentResult = state.BindSegmentDescriptor(impossibleSegment);
    REQUIRE_FALSE(segmentResult);
    REQUIRE(
        segmentResult.Error().code ==
        pbprotocol::ProtocolErrorCode::SegmentOrdinalOutOfRange);
    REQUIRE_FALSE(state.HasTerminalError());

    std::array<std::byte, pbprotocol::kFinalManifestPayloadBytes>
        finalPayload{};
    REQUIRE(pbprotocol::SerializeFinalManifest(
        finalManifest,
        parsedSession,
        resourcePolicy,
        finalPayload));
    const auto parsedFinalResult = pbprotocol::ParseFinalManifest(
        finalPayload,
        parsedSession,
        resourcePolicy);
    REQUIRE(parsedFinalResult);
    REQUIRE(state.BindFinalManifest(parsedFinalResult.Value()));
    REQUIRE(state.ValidateReadyForFinalVerification());
    REQUIRE(state.VerifyWholeFileDigest(finalManifest.wholeFileDigest));
}

TEST_CASE("Canonical payloads integrate through parsing binding and verification",
          "[pbprotocol][binding][integration]")
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::test::MakeResourcePolicy();
    const pbprotocol::SessionDescriptor sourceSession =
        pbprotocol::test::MakeSessionDescriptor(300, 3);
    std::array<std::byte, pbprotocol::kSessionDescriptorPayloadBytes>
        sessionPayload{};
    REQUIRE(pbprotocol::SerializeSessionDescriptor(
        sourceSession,
        resourcePolicy,
        sessionPayload));
    const auto parsedSessionResult = pbprotocol::ParseSessionDescriptor(
        sessionPayload,
        resourcePolicy);
    REQUIRE(parsedSessionResult);
    const pbprotocol::SessionDescriptor parsedSession =
        parsedSessionResult.Value();

    auto stateResult = pbprotocol::DescriptorBindingState::Create(
        parsedSession,
        resourcePolicy);
    REQUIRE(stateResult);
    auto state = std::move(stateResult).Value();

    const std::array<pbprotocol::SegmentDescriptor, 3> sourceSegments{
        pbprotocol::test::MakeDirectRepeatSegment(
            parsedSession, 2, 200, 100),
        pbprotocol::test::MakeDirectRepeatSegment(
            parsedSession, 0, 0, 100),
        pbprotocol::test::MakeDirectRepeatSegment(
            parsedSession, 1, 100, 100)};
    for (const pbprotocol::SegmentDescriptor& sourceSegment : sourceSegments)
    {
        std::array<
            std::byte,
            pbprotocol::kDirectRepeatSegmentDescriptorPayloadBytes>
            segmentPayload{};
        REQUIRE(pbprotocol::SerializeSegmentDescriptor(
            sourceSegment,
            parsedSession,
            resourcePolicy,
            segmentPayload));
        const auto parsedSegmentResult = pbprotocol::ParseSegmentDescriptor(
            segmentPayload,
            parsedSession,
            resourcePolicy);
        REQUIRE(parsedSegmentResult);
        const auto bindResult = state.BindSegmentDescriptor(
            parsedSegmentResult.Value());
        REQUIRE(bindResult);
        REQUIRE(
            bindResult.Value() ==
            pbprotocol::DescriptorBindDisposition::Inserted);
    }

    REQUIRE(state.ValidateCompleteSegmentMap());

    const pbprotocol::FinalManifest sourceFinal =
        pbprotocol::test::MakeFinalManifest(parsedSession);
    std::array<std::byte, pbprotocol::kFinalManifestPayloadBytes>
        finalPayload{};
    REQUIRE(pbprotocol::SerializeFinalManifest(
        sourceFinal,
        parsedSession,
        resourcePolicy,
        finalPayload));
    const auto parsedFinalResult = pbprotocol::ParseFinalManifest(
        finalPayload,
        parsedSession,
        resourcePolicy);
    REQUIRE(parsedFinalResult);
    REQUIRE(state.BindFinalManifest(parsedFinalResult.Value()));
    REQUIRE(state.ValidateReadyForFinalVerification());
    REQUIRE(state.VerifyWholeFileDigest(sourceFinal.wholeFileDigest));
}
