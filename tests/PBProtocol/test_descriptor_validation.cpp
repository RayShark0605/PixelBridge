#include "descriptor_test_helpers.h"

#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/descriptor_codec.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

static_assert(!std::is_aggregate_v<pbprotocol::ReceiverResourcePolicy>);

TEST_CASE("SessionDescriptor enforces empty-file and resource semantics",
          "[pbprotocol][descriptor][session][validation]")
{
    pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::test::MakeResourcePolicy();

    REQUIRE(pbprotocol::ValidateSessionDescriptor(
        pbprotocol::test::MakeSessionDescriptor(0, 0),
        resourcePolicy));

    auto invalidEmpty = pbprotocol::test::MakeSessionDescriptor(0, 1);
    REQUIRE_FALSE(pbprotocol::ValidateSessionDescriptor(
        invalidEmpty,
        resourcePolicy));

    auto invalidNonEmpty = pbprotocol::test::MakeSessionDescriptor(1, 0);
    REQUIRE_FALSE(pbprotocol::ValidateSessionDescriptor(
        invalidNonEmpty,
        resourcePolicy));

    auto tooManySegments = pbprotocol::test::MakeSessionDescriptor(3, 4);
    REQUIRE_FALSE(pbprotocol::ValidateSessionDescriptor(
        tooManySegments,
        resourcePolicy));

    auto unsupportedMinor = pbprotocol::test::MakeSessionDescriptor(1, 1);
    unsupportedMinor.protocolVersion.minor++;
    const auto minorStatus = pbprotocol::ValidateSessionDescriptor(
        unsupportedMinor,
        resourcePolicy);
    REQUIRE_FALSE(minorStatus);
    REQUIRE(
        minorStatus.Error().code ==
        pbprotocol::ProtocolErrorCode::UnsupportedProtocolMinor);

    resourcePolicy.maxAcceptedFileBytes = 116;
    const auto fileLimitStatus = pbprotocol::ValidateSessionDescriptor(
        pbprotocol::test::MakeSessionDescriptor(117, 1),
        resourcePolicy);
    REQUIRE_FALSE(fileLimitStatus);
    REQUIRE(
        fileLimitStatus.Error().code ==
        pbprotocol::ProtocolErrorCode::ResourceLimitExceeded);
}

TEST_CASE("Zero initialized ReceiverResourcePolicy fails closed",
          "[pbprotocol][descriptor][policy]")
{
    const pbprotocol::SessionDescriptor emptySession =
        pbprotocol::test::MakeSessionDescriptor(0, 0);
    const pbprotocol::ReceiverResourcePolicy zeroPolicy{};

    const pbprotocol::ProtocolStatus status =
        pbprotocol::ValidateSessionDescriptor(emptySession, zeroPolicy);

    REQUIRE_FALSE(status);
    REQUIRE(
        status.Error().code ==
        pbprotocol::ProtocolErrorCode::InvalidResourcePolicy);
}

TEST_CASE("Receiver resource policy is finite and rejects impossible Session shapes",
          "[pbprotocol][descriptor][policy][feasibility]")
{
    const pbprotocol::ReceiverResourcePolicy defaultPolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    REQUIRE(pbprotocol::ValidateReceiverResourcePolicy(defaultPolicy));
    REQUIRE(
        defaultPolicy.maxDescriptorStateBytes <
        std::numeric_limits<std::uint64_t>::max());
    REQUIRE(
        defaultPolicy.maxConcurrentSessions <
        std::numeric_limits<std::uint64_t>::max());
    REQUIRE(
        defaultPolicy.maxTotalDescriptorStateBytes <
        std::numeric_limits<std::uint64_t>::max());
    REQUIRE(
        defaultPolicy.maxActiveOuterFecDecoders <
        std::numeric_limits<std::uint64_t>::max());
    REQUIRE(
        defaultPolicy.maxOuterFecDecoderBytes <
        std::numeric_limits<std::uint64_t>::max());
    REQUIRE(
        defaultPolicy.maxTotalOuterFecDecoderBytes <
        std::numeric_limits<std::uint64_t>::max());
    REQUIRE(defaultPolicy.maxOuterBlockBytes ==
        pbprotocol::kMaximumTransportPayloadBytes);
    REQUIRE(defaultPolicy.maxDirectRepeatBlockCount == 64);
    REQUIRE(defaultPolicy.maxActiveOuterFecDecoders == 4);
    REQUIRE(defaultPolicy.maxOuterFecDecoderBytes ==
        512ULL * 1024ULL * 1024ULL);
    REQUIRE(defaultPolicy.maxTotalOuterFecDecoderBytes ==
        1024ULL * 1024ULL * 1024ULL);
    REQUIRE(defaultPolicy.maxControlRecordBytes == 65536);
    REQUIRE(defaultPolicy.maxConcurrentControlReassemblies == 8);
    REQUIRE(defaultPolicy.maxControlReassemblyBytes ==
        1024ULL * 1024ULL);
    REQUIRE(defaultPolicy.maxControlFragmentsPerRecord == 4096);
    REQUIRE(defaultPolicy.maxControlReassemblyInactivityObservations == 16384);

    pbprotocol::ReceiverResourcePolicy impossiblePolicy =
        pbprotocol::test::MakeResourcePolicy();
    impossiblePolicy.maxRawSegmentBytes = 100;
    const pbprotocol::SessionDescriptor impossibleSession =
        pbprotocol::test::MakeSessionDescriptor(101, 1);

    REQUIRE(pbprotocol::ValidateSessionDescriptor(impossibleSession));
    const pbprotocol::ProtocolStatus impossibleStatus =
        pbprotocol::ValidateSessionDescriptor(
            impossibleSession,
            impossiblePolicy);
    REQUIRE_FALSE(impossibleStatus);
    REQUIRE(
        impossibleStatus.Error().code ==
        pbprotocol::ProtocolErrorCode::ResourceLimitExceeded);
    REQUIRE(impossibleStatus.Error().offset == 28);

    pbprotocol::ReceiverResourcePolicy unboundedPolicy{};
    unboundedPolicy.maxAcceptedFileBytes =
        std::numeric_limits<std::uint64_t>::max();
    unboundedPolicy.maxSegmentCount =
        std::numeric_limits<std::uint64_t>::max();
    unboundedPolicy.maxRawSegmentBytes =
        std::numeric_limits<std::uint64_t>::max();
    unboundedPolicy.maxEncodedSegmentBytes =
        std::numeric_limits<std::uint64_t>::max();
    unboundedPolicy.maxOuterBlockBytes =
        std::numeric_limits<std::uint32_t>::max();
    unboundedPolicy.maxDescriptorStateBytes =
        std::numeric_limits<std::uint64_t>::max();
    unboundedPolicy.maxConcurrentSessions =
        std::numeric_limits<std::uint64_t>::max();
    unboundedPolicy.maxTotalDescriptorStateBytes =
        std::numeric_limits<std::uint64_t>::max();
    unboundedPolicy.maxDirectRepeatBlockCount =
        std::numeric_limits<std::uint64_t>::max();
    unboundedPolicy.maxActiveOuterFecDecoders =
        std::numeric_limits<std::uint64_t>::max();
    unboundedPolicy.maxOuterFecDecoderBytes =
        std::numeric_limits<std::uint64_t>::max();
    unboundedPolicy.maxTotalOuterFecDecoderBytes =
        std::numeric_limits<std::uint64_t>::max();
    unboundedPolicy.maxControlRecordBytes =
        std::numeric_limits<std::uint32_t>::max();
    unboundedPolicy.maxConcurrentControlReassemblies =
        std::numeric_limits<std::uint64_t>::max();
    unboundedPolicy.maxControlReassemblyBytes =
        std::numeric_limits<std::uint64_t>::max();
    unboundedPolicy.maxControlFragmentsPerRecord =
        std::numeric_limits<std::uint64_t>::max();
    unboundedPolicy.maxControlReassemblyInactivityObservations =
        std::numeric_limits<std::uint64_t>::max();
    const pbprotocol::ProtocolStatus unboundedStatus =
        pbprotocol::ValidateReceiverResourcePolicy(unboundedPolicy);
    REQUIRE_FALSE(unboundedStatus);
    REQUIRE(
        unboundedStatus.Error().code ==
        pbprotocol::ProtocolErrorCode::InvalidResourcePolicy);

    auto zeroPerSessionBudget = defaultPolicy;
    zeroPerSessionBudget.maxDescriptorStateBytes = 0;
    REQUIRE_FALSE(pbprotocol::ValidateReceiverResourcePolicy(
        zeroPerSessionBudget));

    auto zeroSessionLimit = defaultPolicy;
    zeroSessionLimit.maxConcurrentSessions = 0;
    REQUIRE_FALSE(pbprotocol::ValidateReceiverResourcePolicy(
        zeroSessionLimit));

    auto zeroAggregateBudget = defaultPolicy;
    zeroAggregateBudget.maxTotalDescriptorStateBytes = 0;
    REQUIRE_FALSE(pbprotocol::ValidateReceiverResourcePolicy(
        zeroAggregateBudget));

    auto contradictoryBudgets = defaultPolicy;
    contradictoryBudgets.maxTotalDescriptorStateBytes =
        contradictoryBudgets.maxDescriptorStateBytes - 1ULL;
    REQUIRE_FALSE(pbprotocol::ValidateReceiverResourcePolicy(
        contradictoryBudgets));

    auto zeroOuterFecDecoderLimit = defaultPolicy;
    zeroOuterFecDecoderLimit.maxActiveOuterFecDecoders = 0;
    REQUIRE_FALSE(pbprotocol::ValidateReceiverResourcePolicy(
        zeroOuterFecDecoderLimit));

    auto zeroDirectRepeatBlockLimit = defaultPolicy;
    zeroDirectRepeatBlockLimit.maxDirectRepeatBlockCount = 0;
    REQUIRE_FALSE(pbprotocol::ValidateReceiverResourcePolicy(
        zeroDirectRepeatBlockLimit));

    auto zeroOuterFecDecoderBudget = defaultPolicy;
    zeroOuterFecDecoderBudget.maxOuterFecDecoderBytes = 0;
    REQUIRE_FALSE(pbprotocol::ValidateReceiverResourcePolicy(
        zeroOuterFecDecoderBudget));

    auto zeroAggregateOuterFecBudget = defaultPolicy;
    zeroAggregateOuterFecBudget.maxTotalOuterFecDecoderBytes = 0;
    REQUIRE_FALSE(pbprotocol::ValidateReceiverResourcePolicy(
        zeroAggregateOuterFecBudget));

    auto contradictoryOuterFecBudgets = defaultPolicy;
    contradictoryOuterFecBudgets.maxTotalOuterFecDecoderBytes =
        contradictoryOuterFecBudgets.maxOuterFecDecoderBytes - 1ULL;
    REQUIRE_FALSE(pbprotocol::ValidateReceiverResourcePolicy(
        contradictoryOuterFecBudgets));

    auto tooSmallControlRecord = defaultPolicy;
    tooSmallControlRecord.maxControlRecordBytes =
        static_cast<std::uint32_t>(pbprotocol::kMinimumControlRecordBytes - 1U);
    REQUIRE_FALSE(pbprotocol::ValidateReceiverResourcePolicy(
        tooSmallControlRecord));

    auto tooLargeControlRecord = defaultPolicy;
    tooLargeControlRecord.maxControlRecordBytes =
        static_cast<std::uint32_t>(pbprotocol::kMaximumControlRecordBytes + 1U);
    REQUIRE_FALSE(pbprotocol::ValidateReceiverResourcePolicy(
        tooLargeControlRecord));

    auto zeroControlConcurrency = defaultPolicy;
    zeroControlConcurrency.maxConcurrentControlReassemblies = 0;
    REQUIRE_FALSE(pbprotocol::ValidateReceiverResourcePolicy(
        zeroControlConcurrency));

    auto zeroControlBudget = defaultPolicy;
    zeroControlBudget.maxControlReassemblyBytes = 0;
    REQUIRE_FALSE(pbprotocol::ValidateReceiverResourcePolicy(
        zeroControlBudget));

    auto contradictoryControlBudget = defaultPolicy;
    contradictoryControlBudget.maxControlReassemblyBytes =
        contradictoryControlBudget.maxControlRecordBytes - 1ULL;
    REQUIRE_FALSE(pbprotocol::ValidateReceiverResourcePolicy(
        contradictoryControlBudget));

    auto zeroControlFragmentLimit = defaultPolicy;
    zeroControlFragmentLimit.maxControlFragmentsPerRecord = 0;
    REQUIRE_FALSE(pbprotocol::ValidateReceiverResourcePolicy(
        zeroControlFragmentLimit));

    auto impossibleControlFragmentLimit = defaultPolicy;
    impossibleControlFragmentLimit.maxControlFragmentsPerRecord =
        impossibleControlFragmentLimit.maxControlRecordBytes + 1ULL;
    REQUIRE_FALSE(pbprotocol::ValidateReceiverResourcePolicy(
        impossibleControlFragmentLimit));

    auto zeroControlInactivityWindow = defaultPolicy;
    zeroControlInactivityWindow.maxControlReassemblyInactivityObservations = 0;
    REQUIRE_FALSE(pbprotocol::ValidateReceiverResourcePolicy(
        zeroControlInactivityWindow));
}

TEST_CASE("Receiver resource policy rejects every unbounded sentinel independently",
          "[pbprotocol][descriptor][policy][boundary]")
{
    SECTION("accepted file bytes")
    {
        auto resourcePolicy = pbprotocol::test::MakeResourcePolicy();
        resourcePolicy.maxAcceptedFileBytes =
            std::numeric_limits<std::uint64_t>::max();
        REQUIRE_FALSE(pbprotocol::ValidateReceiverResourcePolicy(resourcePolicy));
    }

    SECTION("segment count")
    {
        auto resourcePolicy = pbprotocol::test::MakeResourcePolicy();
        resourcePolicy.maxSegmentCount =
            std::numeric_limits<std::uint64_t>::max();
        REQUIRE_FALSE(pbprotocol::ValidateReceiverResourcePolicy(resourcePolicy));
    }

    SECTION("raw segment bytes")
    {
        auto resourcePolicy = pbprotocol::test::MakeResourcePolicy();
        resourcePolicy.maxRawSegmentBytes =
            std::numeric_limits<std::uint64_t>::max();
        REQUIRE_FALSE(pbprotocol::ValidateReceiverResourcePolicy(resourcePolicy));
    }

    SECTION("encoded segment bytes")
    {
        auto resourcePolicy = pbprotocol::test::MakeResourcePolicy();
        resourcePolicy.maxEncodedSegmentBytes =
            std::numeric_limits<std::uint64_t>::max();
        REQUIRE_FALSE(pbprotocol::ValidateReceiverResourcePolicy(resourcePolicy));
    }

    SECTION("outer block bytes")
    {
        auto resourcePolicy = pbprotocol::test::MakeResourcePolicy();
        resourcePolicy.maxOuterBlockBytes =
            std::numeric_limits<std::uint32_t>::max();
        REQUIRE_FALSE(pbprotocol::ValidateReceiverResourcePolicy(resourcePolicy));
    }

    SECTION("direct repeat block count")
    {
        auto resourcePolicy = pbprotocol::test::MakeResourcePolicy();
        resourcePolicy.maxDirectRepeatBlockCount =
            std::numeric_limits<std::uint64_t>::max();
        REQUIRE_FALSE(pbprotocol::ValidateReceiverResourcePolicy(resourcePolicy));
    }

    SECTION("descriptor state bytes")
    {
        auto resourcePolicy = pbprotocol::test::MakeResourcePolicy();
        resourcePolicy.maxDescriptorStateBytes =
            std::numeric_limits<std::uint64_t>::max();
        REQUIRE_FALSE(pbprotocol::ValidateReceiverResourcePolicy(resourcePolicy));
    }

    SECTION("concurrent sessions")
    {
        auto resourcePolicy = pbprotocol::test::MakeResourcePolicy();
        resourcePolicy.maxConcurrentSessions =
            std::numeric_limits<std::uint64_t>::max();
        REQUIRE_FALSE(pbprotocol::ValidateReceiverResourcePolicy(resourcePolicy));
    }

    SECTION("total descriptor state bytes")
    {
        auto resourcePolicy = pbprotocol::test::MakeResourcePolicy();
        resourcePolicy.maxTotalDescriptorStateBytes =
            std::numeric_limits<std::uint64_t>::max();
        REQUIRE_FALSE(pbprotocol::ValidateReceiverResourcePolicy(resourcePolicy));
    }

    SECTION("active outer FEC decoders")
    {
        auto resourcePolicy = pbprotocol::test::MakeResourcePolicy();
        resourcePolicy.maxActiveOuterFecDecoders =
            std::numeric_limits<std::uint64_t>::max();
        REQUIRE_FALSE(pbprotocol::ValidateReceiverResourcePolicy(resourcePolicy));
    }

    SECTION("outer FEC decoder bytes")
    {
        auto resourcePolicy = pbprotocol::test::MakeResourcePolicy();
        resourcePolicy.maxOuterFecDecoderBytes =
            std::numeric_limits<std::uint64_t>::max();
        REQUIRE_FALSE(pbprotocol::ValidateReceiverResourcePolicy(resourcePolicy));
    }

    SECTION("total outer FEC decoder bytes")
    {
        auto resourcePolicy = pbprotocol::test::MakeResourcePolicy();
        resourcePolicy.maxTotalOuterFecDecoderBytes =
            std::numeric_limits<std::uint64_t>::max();
        REQUIRE_FALSE(pbprotocol::ValidateReceiverResourcePolicy(resourcePolicy));
    }

    SECTION("Control record bytes")
    {
        auto resourcePolicy = pbprotocol::test::MakeResourcePolicy();
        resourcePolicy.maxControlRecordBytes =
            std::numeric_limits<std::uint32_t>::max();
        REQUIRE_FALSE(pbprotocol::ValidateReceiverResourcePolicy(resourcePolicy));
    }

    SECTION("concurrent Control reassemblies")
    {
        auto resourcePolicy = pbprotocol::test::MakeResourcePolicy();
        resourcePolicy.maxConcurrentControlReassemblies =
            std::numeric_limits<std::uint64_t>::max();
        REQUIRE_FALSE(pbprotocol::ValidateReceiverResourcePolicy(resourcePolicy));
    }

    SECTION("Control reassembly bytes")
    {
        auto resourcePolicy = pbprotocol::test::MakeResourcePolicy();
        resourcePolicy.maxControlReassemblyBytes =
            std::numeric_limits<std::uint64_t>::max();
        REQUIRE_FALSE(pbprotocol::ValidateReceiverResourcePolicy(resourcePolicy));
    }

    SECTION("Control fragments per record")
    {
        auto resourcePolicy = pbprotocol::test::MakeResourcePolicy();
        resourcePolicy.maxControlFragmentsPerRecord =
            std::numeric_limits<std::uint64_t>::max();
        REQUIRE_FALSE(pbprotocol::ValidateReceiverResourcePolicy(resourcePolicy));
    }

    SECTION("Control inactivity observations")
    {
        auto resourcePolicy = pbprotocol::test::MakeResourcePolicy();
        resourcePolicy.maxControlReassemblyInactivityObservations =
            std::numeric_limits<std::uint64_t>::max();
        REQUIRE_FALSE(pbprotocol::ValidateReceiverResourcePolicy(resourcePolicy));
    }
}

TEST_CASE("Finite values immediately below protocol policy sentinels are accepted",
          "[pbprotocol][descriptor][policy][boundary]")
{
    auto resourcePolicy = pbprotocol::test::MakeResourcePolicy();
    resourcePolicy.maxAcceptedFileBytes =
        std::numeric_limits<std::uint64_t>::max() - 1ULL;
    resourcePolicy.maxSegmentCount =
        std::numeric_limits<std::uint64_t>::max() - 1ULL;
    resourcePolicy.maxRawSegmentBytes =
        std::numeric_limits<std::uint64_t>::max() - 1ULL;
    resourcePolicy.maxEncodedSegmentBytes =
        std::numeric_limits<std::uint64_t>::max() - 1ULL;
    resourcePolicy.maxOuterBlockBytes =
        pbprotocol::kMaximumTransportPayloadBytes;
    resourcePolicy.maxDirectRepeatBlockCount =
        pbprotocol::kMaximumRepresentableDirectRepeatBlockCount;
    resourcePolicy.maxActiveOuterFecDecoders =
        std::numeric_limits<std::uint64_t>::max() - 1ULL;
    resourcePolicy.maxOuterFecDecoderBytes =
        std::numeric_limits<std::uint64_t>::max() - 1ULL;
    resourcePolicy.maxTotalOuterFecDecoderBytes =
        std::numeric_limits<std::uint64_t>::max() - 1ULL;

    REQUIRE(pbprotocol::ValidateReceiverResourcePolicy(resourcePolicy));
}

TEST_CASE("Transport payload width bounds every OuterBlockBytes entry point",
          "[pbprotocol][descriptor][transport][boundary]")
{
    pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::test::MakeResourcePolicy();
    resourcePolicy.maxOuterBlockBytes =
        pbprotocol::kMaximumTransportPayloadBytes;
    REQUIRE(pbprotocol::ValidateReceiverResourcePolicy(resourcePolicy));

    auto oversizedPolicy = resourcePolicy;
    oversizedPolicy.maxOuterBlockBytes =
        pbprotocol::kMaximumTransportPayloadBytes + 1U;
    REQUIRE_FALSE(pbprotocol::ValidateReceiverResourcePolicy(oversizedPolicy));

    const pbprotocol::SessionDescriptor sessionDescriptor =
        pbprotocol::test::MakeSessionDescriptor(1, 1);
    const pbprotocol::SegmentDescriptor maximumDescriptor =
        pbprotocol::test::MakeDirectRepeatSegment(
            sessionDescriptor,
            0,
            0,
            1,
            pbprotocol::kMaximumTransportPayloadBytes);
    REQUIRE(pbprotocol::ValidateSegmentDescriptor(
        maximumDescriptor, sessionDescriptor, resourcePolicy));

    auto oversizedDescriptor = maximumDescriptor;
    oversizedDescriptor.outerBlockBytes =
        pbprotocol::kMaximumTransportPayloadBytes + 1U;
    const auto descriptorStatus = pbprotocol::ValidateSegmentDescriptor(
        oversizedDescriptor, sessionDescriptor);
    REQUIRE_FALSE(descriptorStatus);
    REQUIRE(descriptorStatus.Error().code ==
        pbprotocol::ProtocolErrorCode::InvalidDescriptor);

    std::array<
        std::byte,
        pbprotocol::kDirectRepeatSegmentDescriptorPayloadBytes> output{};
    output.fill(static_cast<std::byte>(0xA5));
    const auto originalOutput = output;
    REQUIRE_FALSE(pbprotocol::SerializeSegmentDescriptor(
        oversizedDescriptor, sessionDescriptor, output));
    REQUIRE(output == originalOutput);

    REQUIRE(pbprotocol::GetDirectRepeatBlockCount(
        1, pbprotocol::kMaximumTransportPayloadBytes));
    REQUIRE_FALSE(pbprotocol::GetDirectRepeatBlockCount(
        1, pbprotocol::kMaximumTransportPayloadBytes + 1U));
}

TEST_CASE("DirectRepeat block-count policy is inclusive and checked in descriptor admission",
          "[pbprotocol][descriptor][direct-repeat][policy][boundary]")
{
    const pbprotocol::SessionDescriptor sessionDescriptor =
        pbprotocol::test::MakeSessionDescriptor(65, 1);
    const pbprotocol::SegmentDescriptor descriptor =
        pbprotocol::test::MakeDirectRepeatSegment(
            sessionDescriptor, 0, 0, 65, 16);

    pbprotocol::ReceiverResourcePolicy exactPolicy =
        pbprotocol::test::MakeResourcePolicy();
    exactPolicy.maxDirectRepeatBlockCount = 5;
    REQUIRE(pbprotocol::ValidateSegmentDescriptor(
        descriptor, sessionDescriptor, exactPolicy));

    auto tooSmallPolicy = exactPolicy;
    tooSmallPolicy.maxDirectRepeatBlockCount = 4;
    const auto status = pbprotocol::ValidateSegmentDescriptor(
        descriptor, sessionDescriptor, tooSmallPolicy);
    REQUIRE_FALSE(status);
    REQUIRE(status.Error().code ==
        pbprotocol::ProtocolErrorCode::ResourceLimitExceeded);
    REQUIRE(status.Error().offset == 42);
}

TEST_CASE("Every receiver resource limit is inclusive and checked before use",
          "[pbprotocol][descriptor][policy][boundary]")
{
    const pbprotocol::SessionDescriptor sessionDescriptor =
        pbprotocol::test::MakeSessionDescriptor(200, 2);
    pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::test::MakeResourcePolicy();
    resourcePolicy.maxAcceptedFileBytes = 200;
    resourcePolicy.maxSegmentCount = 2;
    resourcePolicy.maxRawSegmentBytes = 100;
    resourcePolicy.maxEncodedSegmentBytes = 101;
    resourcePolicy.maxOuterBlockBytes = 16;

    REQUIRE(pbprotocol::ValidateSessionDescriptor(
        sessionDescriptor,
        resourcePolicy));

    auto tooSmallFilePolicy = resourcePolicy;
    tooSmallFilePolicy.maxAcceptedFileBytes = 199;
    REQUIRE_FALSE(pbprotocol::ValidateSessionDescriptor(
        sessionDescriptor,
        tooSmallFilePolicy));

    auto tooSmallCountPolicy = resourcePolicy;
    tooSmallCountPolicy.maxSegmentCount = 1;
    REQUIRE_FALSE(pbprotocol::ValidateSessionDescriptor(
        sessionDescriptor,
        tooSmallCountPolicy));

    pbprotocol::SegmentDescriptor segmentDescriptor =
        pbprotocol::test::MakeDirectRepeatSegment(
            sessionDescriptor,
            0,
            0,
            100,
            16);
    segmentDescriptor.compressionCodec =
        pbprotocol::CompressionCodec::Zstandard;
    segmentDescriptor.encodedSize = 101;
    segmentDescriptor.encodedDigest = pbprotocol::EncodedDigest{
        pbprotocol::test::MakeDigestBytes(0x80)};
    REQUIRE(pbprotocol::ValidateSegmentDescriptor(
        segmentDescriptor,
        sessionDescriptor,
        resourcePolicy));

    auto tooSmallRawPolicy = resourcePolicy;
    tooSmallRawPolicy.maxRawSegmentBytes = 99;
    REQUIRE_FALSE(pbprotocol::ValidateSegmentDescriptor(
        segmentDescriptor,
        sessionDescriptor,
        tooSmallRawPolicy));

    auto tooSmallEncodedPolicy = resourcePolicy;
    tooSmallEncodedPolicy.maxEncodedSegmentBytes = 100;
    REQUIRE_FALSE(pbprotocol::ValidateSegmentDescriptor(
        segmentDescriptor,
        sessionDescriptor,
        tooSmallEncodedPolicy));

    auto tooSmallBlockPolicy = resourcePolicy;
    tooSmallBlockPolicy.maxOuterBlockBytes = 15;
    REQUIRE_FALSE(pbprotocol::ValidateSegmentDescriptor(
        segmentDescriptor,
        sessionDescriptor,
        tooSmallBlockPolicy));
}

TEST_CASE("SegmentDescriptor checks identity ranges resources and RAW invariants",
          "[pbprotocol][descriptor][segment][validation]")
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::test::MakeResourcePolicy();
    const pbprotocol::SessionDescriptor sessionDescriptor =
        pbprotocol::test::MakeSessionDescriptor(117, 1);
    const pbprotocol::SegmentDescriptor validDescriptor =
        pbprotocol::test::MakeDirectRepeatSegment(
            sessionDescriptor,
            0,
            0,
            117);
    REQUIRE(pbprotocol::ValidateSegmentDescriptor(
        validDescriptor,
        sessionDescriptor,
        resourcePolicy));

    SECTION("wrong SessionTag")
    {
        auto descriptor = validDescriptor;
        descriptor.sessionTag.value++;
        const auto status = pbprotocol::ValidateSegmentDescriptor(
            descriptor,
            sessionDescriptor,
            resourcePolicy);
        REQUIRE_FALSE(status);
        REQUIRE(
            status.Error().code ==
            pbprotocol::ProtocolErrorCode::SessionTagMismatch);
    }
    SECTION("ordinal at SegmentCount")
    {
        auto descriptor = validDescriptor;
        descriptor.segmentOrdinal = sessionDescriptor.segmentCount;
        const auto status = pbprotocol::ValidateSegmentDescriptor(
            descriptor,
            sessionDescriptor,
            resourcePolicy);
        REQUIRE_FALSE(status);
        REQUIRE(
            status.Error().code ==
            pbprotocol::ProtocolErrorCode::SegmentOrdinalOutOfRange);
    }
    SECTION("zero RawSize")
    {
        auto descriptor = validDescriptor;
        descriptor.rawSize = 0;
        descriptor.encodedSize = 0;
        REQUIRE_FALSE(pbprotocol::ValidateSegmentDescriptor(
            descriptor,
            sessionDescriptor,
            resourcePolicy));
    }
    SECTION("range outside file")
    {
        auto descriptor = validDescriptor;
        descriptor.rawOffset = 1;
        const auto status = pbprotocol::ValidateSegmentDescriptor(
            descriptor,
            sessionDescriptor,
            resourcePolicy);
        REQUIRE_FALSE(status);
        REQUIRE(
            status.Error().code ==
            pbprotocol::ProtocolErrorCode::SegmentRangeOutOfBounds);
    }
    SECTION("RAW encoded size differs")
    {
        auto descriptor = validDescriptor;
        descriptor.encodedSize--;
        REQUIRE_FALSE(pbprotocol::ValidateSegmentDescriptor(
            descriptor,
            sessionDescriptor,
            resourcePolicy));
    }
    SECTION("RAW digest differs")
    {
        auto descriptor = validDescriptor;
        descriptor.encodedDigest.bytes[0] = pbprotocol::test::Byte(0xFF);
        const auto status = pbprotocol::ValidateSegmentDescriptor(
            descriptor,
            sessionDescriptor,
            resourcePolicy);
        REQUIRE_FALSE(status);
        REQUIRE(status.Error().offset == 78);
    }
    SECTION("DirectRepeat carries Wirehair bytes")
    {
        auto descriptor = validDescriptor;
        descriptor.wirehairV2SerializedProfile =
            pbprotocol::test::MakeWirehairProfile();
        REQUIRE_FALSE(pbprotocol::ValidateSegmentDescriptor(
            descriptor,
            sessionDescriptor,
            resourcePolicy));
    }
    SECTION("resource limits")
    {
        auto smallPolicy = resourcePolicy;
        smallPolicy.maxRawSegmentBytes = 116;
        const auto status = pbprotocol::ValidateSegmentDescriptor(
            validDescriptor,
            sessionDescriptor,
            smallPolicy);
        REQUIRE_FALSE(status);
        REQUIRE(
            status.Error().code ==
            pbprotocol::ProtocolErrorCode::ResourceLimitExceeded);
    }
}

TEST_CASE("RawOffset plus RawSize uses checked uint64 arithmetic",
          "[pbprotocol][descriptor][segment][overflow]")
{
    auto resourcePolicy = pbprotocol::test::MakeResourcePolicy();
    resourcePolicy.maxSegmentCount = 1;
    const pbprotocol::SessionDescriptor sessionDescriptor =
        pbprotocol::test::MakeSessionDescriptor(
            std::numeric_limits<std::uint64_t>::max() - 1ULL,
            1);
    auto descriptor = pbprotocol::test::MakeDirectRepeatSegment(
        sessionDescriptor,
        0,
        std::numeric_limits<std::uint64_t>::max() - 1ULL,
        2);

    const auto status = pbprotocol::ValidateSegmentDescriptor(
        descriptor,
        sessionDescriptor,
        resourcePolicy);
    REQUIRE_FALSE(status);
    REQUIRE(
        status.Error().code ==
        pbprotocol::ProtocolErrorCode::LengthOverflow);
}

TEST_CASE("Wirehair canonical profile validation covers identity and K bounds",
          "[pbprotocol][wirehair][validation]")
{
    REQUIRE(pbprotocol::ValidateWirehairV2SerializedProfile(
        pbprotocol::test::MakeWirehairProfile(117, 16),
        117,
        16));
    REQUIRE(pbprotocol::ValidateWirehairV2SerializedProfile(
        pbprotocol::test::MakeWirehairProfile(17, 16, 255),
        17,
        16));
    REQUIRE(pbprotocol::ValidateWirehairV2SerializedProfile(
        pbprotocol::test::MakeWirehairProfile(1'024'000, 16),
        1'024'000,
        16));

    SECTION("K below two")
    {
        REQUIRE_FALSE(pbprotocol::ValidateWirehairV2SerializedProfile(
            pbprotocol::test::MakeWirehairProfile(16, 16),
            16,
            16));
    }
    SECTION("K above 64000")
    {
        REQUIRE_FALSE(pbprotocol::ValidateWirehairV2SerializedProfile(
            pbprotocol::test::MakeWirehairProfile(1'024'001, 16),
            1'024'001,
            16));
    }
    SECTION("invalid magic")
    {
        auto profile = pbprotocol::test::MakeWirehairProfile();
        profile.bytes[0] = pbprotocol::test::Byte(0);
        REQUIRE_FALSE(pbprotocol::ValidateWirehairV2SerializedProfile(
            profile,
            117,
            16));
    }
    SECTION("unsupported encoding version")
    {
        auto profile = pbprotocol::test::MakeWirehairProfile();
        profile.bytes[4] = pbprotocol::test::Byte(2);
        REQUIRE_FALSE(pbprotocol::ValidateWirehairV2SerializedProfile(
            profile,
            117,
            16));
    }
    SECTION("wrong encoded length")
    {
        auto profile = pbprotocol::test::MakeWirehairProfile();
        profile.bytes[6] = pbprotocol::test::Byte(31);
        REQUIRE_FALSE(pbprotocol::ValidateWirehairV2SerializedProfile(
            profile,
            117,
            16));
    }
    SECTION("unknown profile ID")
    {
        auto profile = pbprotocol::test::MakeWirehairProfile();
        profile.bytes[8] = pbprotocol::test::Byte(0);
        REQUIRE_FALSE(pbprotocol::ValidateWirehairV2SerializedProfile(
            profile,
            117,
            16));
    }
    SECTION("message mismatch")
    {
        REQUIRE_FALSE(pbprotocol::ValidateWirehairV2SerializedProfile(
            pbprotocol::test::MakeWirehairProfile(),
            118,
            16));
    }
    SECTION("block mismatch")
    {
        REQUIRE_FALSE(pbprotocol::ValidateWirehairV2SerializedProfile(
            pbprotocol::test::MakeWirehairProfile(),
            117,
            17));
    }
    SECTION("zero message bytes")
    {
        REQUIRE_FALSE(pbprotocol::ValidateWirehairV2SerializedProfile(
            pbprotocol::test::MakeWirehairProfile(0, 16),
            0,
            16));
    }
    SECTION("zero block bytes")
    {
        REQUIRE_FALSE(pbprotocol::ValidateWirehairV2SerializedProfile(
            pbprotocol::test::MakeWirehairProfile(117, 0),
            117,
            0));
    }
    SECTION("block bytes exceed the certified signed range")
    {
        constexpr std::uint32_t tooLargeBlockBytes = 0x80000000U;
        REQUIRE_FALSE(pbprotocol::ValidateWirehairV2SerializedProfile(
            pbprotocol::test::MakeWirehairProfile(
                static_cast<std::uint64_t>(tooLargeBlockBytes) + 1ULL,
                tooLargeBlockBytes),
            static_cast<std::uint64_t>(tooLargeBlockBytes) + 1ULL,
            tooLargeBlockBytes));
    }
    SECTION("every reserved byte is zero")
    {
        for (std::size_t reservedIndex = 29;
             reservedIndex < 32;
             reservedIndex++)
        {
            CAPTURE(reservedIndex);
            auto profile = pbprotocol::test::MakeWirehairProfile();
            profile.bytes[reservedIndex] = pbprotocol::test::Byte(1);
            const auto status =
                pbprotocol::ValidateWirehairV2SerializedProfile(
                    profile,
                    117,
                    16,
                    110);
            REQUIRE_FALSE(status);
            REQUIRE(
                status.Error().code ==
                pbprotocol::ProtocolErrorCode::InvalidWirehairProfile);
            REQUIRE(status.Error().offset == 110 + reservedIndex);
        }
    }
    SECTION("reserved offset is absolute")
    {
        auto profile = pbprotocol::test::MakeWirehairProfile();
        profile.bytes[31] = pbprotocol::test::Byte(1);
        const auto status = pbprotocol::ValidateWirehairV2SerializedProfile(
            profile,
            117,
            16,
            110);
        REQUIRE_FALSE(status);
        REQUIRE(
            status.Error().code ==
            pbprotocol::ProtocolErrorCode::InvalidWirehairProfile);
        REQUIRE(status.Error().offset == 141);
    }
}

TEST_CASE("DirectRepeat block count avoids ceil-division overflow",
          "[pbprotocol][direct-repeat][overflow]")
{
    constexpr std::uint32_t outerBlockBytes = 16;
    const auto emptyResult = pbprotocol::GetDirectRepeatBlockCount(0, 16);
    REQUIRE(emptyResult);
    REQUIRE(emptyResult.Value() == 0);

    const auto zeroBlockResult = pbprotocol::GetDirectRepeatBlockCount(0, 0);
    REQUIRE_FALSE(zeroBlockResult);
    REQUIRE(
        zeroBlockResult.Error().code ==
        pbprotocol::ProtocolErrorCode::InvalidDescriptor);

    const std::array<std::pair<std::uint64_t, std::uint64_t>, 7>
        countBoundaries{{
            {1, 1},
            {outerBlockBytes - 1, 1},
            {outerBlockBytes, 1},
            {outerBlockBytes + 1, 2},
            {3ULL * outerBlockBytes - 1ULL, 3},
            {3ULL * outerBlockBytes, 3},
            {3ULL * outerBlockBytes + 1ULL, 4}}};
    for (const auto& [encodedSize, expectedBlockCount] : countBoundaries)
    {
        const auto countResult = pbprotocol::GetDirectRepeatBlockCount(
            encodedSize, outerBlockBytes);
        REQUIRE(countResult);
        REQUIRE(countResult.Value() == expectedBlockCount);
    }

    constexpr std::uint64_t ordinalSpace =
        static_cast<std::uint64_t>(
            std::numeric_limits<std::uint32_t>::max()) + 1ULL;
    const auto maximumCountResult = pbprotocol::GetDirectRepeatBlockCount(
        ordinalSpace, 1);
    REQUIRE(maximumCountResult);
    REQUIRE(maximumCountResult.Value() == ordinalSpace);

    const auto ordinalOverflowResult =
        pbprotocol::GetDirectRepeatBlockCount(ordinalSpace + 1ULL, 1);
    REQUIRE_FALSE(ordinalOverflowResult);
    REQUIRE(
        ordinalOverflowResult.Error().code ==
        pbprotocol::ProtocolErrorCode::LengthOverflow);

    constexpr std::uint64_t maximumRepresentableEncodedSize =
        ordinalSpace * pbprotocol::kMaximumTransportPayloadBytes;
    const auto maximumEncodedSizeResult =
        pbprotocol::GetDirectRepeatBlockCount(
            maximumRepresentableEncodedSize,
            pbprotocol::kMaximumTransportPayloadBytes);
    REQUIRE(maximumEncodedSizeResult);
    REQUIRE(maximumEncodedSizeResult.Value() == ordinalSpace);

    const auto maximumEncodedSizeOverflowResult =
        pbprotocol::GetDirectRepeatBlockCount(
            maximumRepresentableEncodedSize + 1ULL,
            pbprotocol::kMaximumTransportPayloadBytes);
    REQUIRE_FALSE(maximumEncodedSizeOverflowResult);
    REQUIRE(
        maximumEncodedSizeOverflowResult.Error().code ==
        pbprotocol::ProtocolErrorCode::LengthOverflow);

    const auto overflowResult = pbprotocol::GetDirectRepeatBlockCount(
        std::numeric_limits<std::uint64_t>::max(),
        1);
    REQUIRE_FALSE(overflowResult);
    REQUIRE(
        overflowResult.Error().code ==
        pbprotocol::ProtocolErrorCode::LengthOverflow);
}

TEST_CASE("FinalManifest binds session metadata and empty digest",
          "[pbprotocol][descriptor][final][validation]")
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::test::MakeResourcePolicy();
    const pbprotocol::SessionDescriptor emptySession =
        pbprotocol::test::MakeSessionDescriptor(0, 0);
    const pbprotocol::FinalManifest validEmptyManifest =
        pbprotocol::test::MakeFinalManifest(emptySession);
    REQUIRE(pbprotocol::ValidateFinalManifest(
        validEmptyManifest,
        emptySession,
        resourcePolicy));

    auto wrongEmptyDigest = validEmptyManifest;
    wrongEmptyDigest.wholeFileDigest.bytes[0] ^= std::byte{0x01};
    const auto digestStatus = pbprotocol::ValidateFinalManifest(
        wrongEmptyDigest,
        emptySession,
        resourcePolicy);
    REQUIRE_FALSE(digestStatus);
    REQUIRE(
        digestStatus.Error().code ==
        pbprotocol::ProtocolErrorCode::DigestMismatch);

    const pbprotocol::SessionDescriptor nonEmptySession =
        pbprotocol::test::MakeSessionDescriptor(117, 1);
    auto mismatchedManifest = pbprotocol::test::MakeFinalManifest(nonEmptySession);
    mismatchedManifest.originalFileSize++;
    const auto mismatchStatus = pbprotocol::ValidateFinalManifest(
        mismatchedManifest,
        nonEmptySession,
        resourcePolicy);
    REQUIRE_FALSE(mismatchStatus);
    REQUIRE(
        mismatchStatus.Error().code ==
        pbprotocol::ProtocolErrorCode::SessionMismatch);
    REQUIRE(mismatchStatus.Error().offset == 16);
}
