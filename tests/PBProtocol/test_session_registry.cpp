#include "descriptor_test_helpers.h"

#include "pbprotocol/session_registry.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <utility>

namespace pbprotocol::test {

class SessionRegistryTestAccess
{
public:
    [[nodiscard]] static ProtocolResult<SessionRegistry>
    CreateWithSessionTagDeriver(
        ReceiverResourcePolicy resourcePolicy,
        SessionTag (*sessionTagDeriver)(const SessionId&) noexcept)
    {
        return SessionRegistry::CreateWithSessionTagDeriverForTesting(
            std::move(resourcePolicy),
            sessionTagDeriver);
    }

    static void SetReservedDescriptorStateBytes(
        SessionRegistry& registry,
        const std::uint64_t reservedDescriptorStateBytes) noexcept
    {
        registry.reservedDescriptorStateBytes_ = reservedDescriptorStateBytes;
    }

    static void EraseTagBinding(
        SessionRegistry& registry,
        const SessionTag sessionTag) noexcept
    {
        registry.bindingsByTag_.erase(sessionTag.value);
    }

    static void InsertOrphanTagBinding(
        SessionRegistry& registry,
        const SessionTag sessionTag,
        const SessionId& sessionId)
    {
        registry.bindingsByTag_.emplace(
            sessionTag.value,
            SessionRegistry::TagBinding{sessionId, false});
    }
};

} // namespace pbprotocol::test

namespace {

template <typename RegistryType>
concept HasRegistryFinalVerificationApi = requires(
    RegistryType& registry,
    const pbprotocol::SessionTag sessionTag,
    const pbprotocol::WholeFileDigest& digest)
{
    registry.ValidateReadyForFinalVerification(sessionTag);
    registry.VerifyWholeFileDigest(sessionTag, digest);
};

static_assert(!HasRegistryFinalVerificationApi<pbprotocol::SessionRegistry>);

constexpr std::uint64_t kForcedCollisionTagValue = 0x8877665544332211ULL;

[[nodiscard]] pbprotocol::SessionTag DeriveForcedCollisionTag(
    const pbprotocol::SessionId&) noexcept
{
    return pbprotocol::SessionTag{kForcedCollisionTagValue};
}

[[nodiscard]] pbprotocol::SessionDescriptor MakeDistinctSessionDescriptor(
    const std::uint8_t lastSessionIdByte)
{
    pbprotocol::SessionDescriptor descriptor =
        pbprotocol::test::MakeSessionDescriptor(1, 1);
    descriptor.sessionId.bytes.back() =
        static_cast<std::byte>(lastSessionIdByte);
    return descriptor;
}

} // namespace

TEST_CASE("Session registry rejects injected tag collisions without guessing",
          "[pbprotocol][session-registry][collision]")
{
    pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::test::MakeResourcePolicy();
    resourcePolicy.maxConcurrentSessions = 2;
    resourcePolicy.maxTotalDescriptorStateBytes =
        resourcePolicy.maxDescriptorStateBytes * 2ULL;

    auto registryResult = pbprotocol::test::SessionRegistryTestAccess::
        CreateWithSessionTagDeriver(
            resourcePolicy,
            &DeriveForcedCollisionTag);
    REQUIRE(registryResult);
    auto registry = std::move(registryResult).Value();

    const pbprotocol::SessionDescriptor firstSession =
        MakeDistinctSessionDescriptor(0xA1);
    const pbprotocol::SessionDescriptor secondSession =
        MakeDistinctSessionDescriptor(0xB2);
    REQUIRE(firstSession.sessionId != secondSession.sessionId);

    const auto firstResult = registry.BindSessionDescriptor(firstSession);
    REQUIRE(firstResult);
    REQUIRE(
        firstResult.Value() ==
        pbprotocol::DescriptorBindDisposition::Inserted);
    const auto repeatedResult = registry.BindSessionDescriptor(firstSession);
    REQUIRE(repeatedResult);
    REQUIRE(
        repeatedResult.Value() ==
        pbprotocol::DescriptorBindDisposition::Repeated);
    REQUIRE(registry.ActiveSessionCount() == 1);

    const auto collisionResult = registry.BindSessionDescriptor(secondSession);
    REQUIRE_FALSE(collisionResult);
    REQUIRE(
        collisionResult.Error().code ==
        pbprotocol::ProtocolErrorCode::SessionTagCollision);
    REQUIRE(registry.ActiveSessionCount() == 0);
    REQUIRE(registry.ReservedDescriptorStateBytes() == 0);
    REQUIRE(registry.IsTagAmbiguous(
        pbprotocol::SessionTag{kForcedCollisionTagValue}));

    for (const pbprotocol::SessionDescriptor* const session :
         {&firstSession, &secondSession})
    {
        pbprotocol::SegmentDescriptor segment =
            pbprotocol::test::MakeDirectRepeatSegment(
                *session, 0, 0, 1, 1);
        segment.sessionTag = pbprotocol::SessionTag{kForcedCollisionTagValue};
        const auto segmentResult = registry.BindSegmentDescriptor(segment);
        REQUIRE_FALSE(segmentResult);
        REQUIRE(
            segmentResult.Error().code ==
            pbprotocol::ProtocolErrorCode::SessionTagCollision);
    }

    const auto originalRetry = registry.BindSessionDescriptor(firstSession);
    REQUIRE_FALSE(originalRetry);
    REQUIRE(
        originalRetry.Error().code ==
        pbprotocol::ProtocolErrorCode::SessionTagCollision);
}

TEST_CASE("Session registry routes only established unique sessions",
          "[pbprotocol][session-registry][routing]")
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::test::MakeResourcePolicy();
    auto registryResult = pbprotocol::SessionRegistry::Create(resourcePolicy);
    REQUIRE(registryResult);
    auto registry = std::move(registryResult).Value();

    const pbprotocol::SessionDescriptor sessionDescriptor =
        MakeDistinctSessionDescriptor(0xC3);
    const pbprotocol::SessionTag sessionTag =
        pbprotocol::DeriveSessionTag(sessionDescriptor.sessionId);
    const pbprotocol::SegmentDescriptor segmentDescriptor =
        pbprotocol::test::MakeDirectRepeatSegment(
            sessionDescriptor, 0, 0, 1, 1);
    const pbprotocol::FinalManifest finalManifest =
        pbprotocol::test::MakeFinalManifest(sessionDescriptor);

    const auto unknownResult = registry.BindSegmentDescriptor(segmentDescriptor);
    REQUIRE_FALSE(unknownResult);
    REQUIRE(
        unknownResult.Error().code ==
        pbprotocol::ProtocolErrorCode::UnknownSession);

    REQUIRE(registry.BindSessionDescriptor(sessionDescriptor));
    REQUIRE(registry.BindSegmentDescriptor(segmentDescriptor));
    REQUIRE(registry.BindFinalManifest(finalManifest));
    REQUIRE(registry.ValidateCompleteSegmentMap(sessionTag));

    const auto removalResult = registry.RemoveSession(
        sessionDescriptor.sessionId);
    REQUIRE(removalResult);
    REQUIRE(removalResult.Value());
    REQUIRE(registry.ActiveSessionCount() == 0);
    REQUIRE(registry.ReservedDescriptorStateBytes() == 0);
    const auto repeatedRemovalResult = registry.RemoveSession(
        sessionDescriptor.sessionId);
    REQUIRE(repeatedRemovalResult);
    REQUIRE_FALSE(repeatedRemovalResult.Value());
    const auto afterRemoval = registry.BindSegmentDescriptor(segmentDescriptor);
    REQUIRE_FALSE(afterRemoval);
    REQUIRE(
        afterRemoval.Error().code ==
        pbprotocol::ProtocolErrorCode::UnknownSession);
}

TEST_CASE("Session registry reserves aggregate descriptor budgets before admission",
          "[pbprotocol][session-registry][resource]")
{
    pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::test::MakeResourcePolicy();
    resourcePolicy.maxConcurrentSessions = 2;
    resourcePolicy.maxTotalDescriptorStateBytes =
        resourcePolicy.maxDescriptorStateBytes;

    auto registryResult = pbprotocol::SessionRegistry::Create(resourcePolicy);
    REQUIRE(registryResult);
    auto registry = std::move(registryResult).Value();

    const pbprotocol::SessionDescriptor firstSession =
        MakeDistinctSessionDescriptor(0xD4);
    const pbprotocol::SessionDescriptor secondSession =
        MakeDistinctSessionDescriptor(0xE5);
    REQUIRE(registry.BindSessionDescriptor(firstSession));
    REQUIRE(
        registry.ReservedDescriptorStateBytes() ==
        resourcePolicy.maxDescriptorStateBytes);

    const auto aggregateLimitResult =
        registry.BindSessionDescriptor(secondSession);
    REQUIRE_FALSE(aggregateLimitResult);
    REQUIRE(
        aggregateLimitResult.Error().code ==
        pbprotocol::ProtocolErrorCode::ResourceLimitExceeded);
    REQUIRE(registry.ActiveSessionCount() == 1);

    const auto removalResult = registry.RemoveSession(firstSession.sessionId);
    REQUIRE(removalResult);
    REQUIRE(removalResult.Value());
    REQUIRE(registry.BindSessionDescriptor(secondSession));
    REQUIRE(registry.ActiveSessionCount() == 1);
}

TEST_CASE("Session removal distinguishes absence from registry corruption",
          "[pbprotocol][session-registry][resource][invariant]")
{
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::test::MakeResourcePolicy();
    const pbprotocol::SessionDescriptor sessionDescriptor =
        MakeDistinctSessionDescriptor(0xF6);
    const pbprotocol::SessionTag sessionTag =
        pbprotocol::DeriveSessionTag(sessionDescriptor.sessionId);

    SECTION("missing Session is a successful no-op")
    {
        auto registryResult = pbprotocol::SessionRegistry::Create(
            resourcePolicy);
        REQUIRE(registryResult);
        auto registry = std::move(registryResult).Value();

        const auto removalResult = registry.RemoveSession(
            sessionDescriptor.sessionId);
        REQUIRE(removalResult);
        REQUIRE_FALSE(removalResult.Value());
    }

    SECTION("reservation mismatch is an internal state error")
    {
        auto registryResult = pbprotocol::SessionRegistry::Create(
            resourcePolicy);
        REQUIRE(registryResult);
        auto registry = std::move(registryResult).Value();
        REQUIRE(registry.BindSessionDescriptor(sessionDescriptor));
        pbprotocol::test::SessionRegistryTestAccess::
            SetReservedDescriptorStateBytes(registry, 0);

        const auto removalResult = registry.RemoveSession(
            sessionDescriptor.sessionId);
        REQUIRE_FALSE(removalResult);
        REQUIRE(
            removalResult.Error().code ==
            pbprotocol::ProtocolErrorCode::InternalDescriptorStateError);
        REQUIRE(registry.ActiveSessionCount() == 1);
    }

    SECTION("missing tag binding is an internal state error")
    {
        auto registryResult = pbprotocol::SessionRegistry::Create(
            resourcePolicy);
        REQUIRE(registryResult);
        auto registry = std::move(registryResult).Value();
        REQUIRE(registry.BindSessionDescriptor(sessionDescriptor));
        pbprotocol::test::SessionRegistryTestAccess::EraseTagBinding(
            registry,
            sessionTag);

        const auto removalResult = registry.RemoveSession(
            sessionDescriptor.sessionId);
        REQUIRE_FALSE(removalResult);
        REQUIRE(
            removalResult.Error().code ==
            pbprotocol::ProtocolErrorCode::InternalDescriptorStateError);
        REQUIRE(registry.ActiveSessionCount() == 1);
    }

    SECTION("orphan tag binding is not hidden as an absent Session")
    {
        auto registryResult = pbprotocol::SessionRegistry::Create(
            resourcePolicy);
        REQUIRE(registryResult);
        auto registry = std::move(registryResult).Value();
        pbprotocol::test::SessionRegistryTestAccess::InsertOrphanTagBinding(
            registry,
            sessionTag,
            sessionDescriptor.sessionId);

        const auto removalResult = registry.RemoveSession(
            sessionDescriptor.sessionId);
        REQUIRE_FALSE(removalResult);
        REQUIRE(
            removalResult.Error().code ==
            pbprotocol::ProtocolErrorCode::InternalDescriptorStateError);
        REQUIRE(registry.ActiveSessionCount() == 0);
    }
}
