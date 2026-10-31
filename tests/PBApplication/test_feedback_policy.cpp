#include "feedback_policy.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>

namespace
{

pbapp::FeedbackSnapshot MakeSnapshot(const std::uint32_t segmentCount = 13)
{
    pbapp::FeedbackSnapshot snapshot;
    snapshot.identity.sessionId.bytes.fill(std::byte{1});
    snapshot.identity.profileId = 0x504250414D343031ULL;
    snapshot.identity.layoutVersion = 15;
    snapshot.identity.fileBytes = segmentCount;
    snapshot.identity.segmentCount = segmentCount;
    snapshot.identity.canonicalSessionDigest.fill(std::byte{2});
    snapshot.identity.canonicalManifestDigest.fill(std::byte{3});
    snapshot.identity.wholeFileDigest.bytes.fill(std::byte{4});
    snapshot.receiverId.fill(std::byte{5});
    snapshot.receiverEpoch.fill(std::byte{6});
    snapshot.snapshotSequence = 1;
    snapshot.identityBound = true;
    snapshot.observedFrameAvailable = true;
    snapshot.observedFrameSequence = 4096;
    snapshot.phase = pbapp::FeedbackPhase::Receiving;
    snapshot.durableCompleted.resize((segmentCount + 7U) / 8U);
    return snapshot;
}

TEST_CASE("Feedback I/O withdrawal preserves snapshot authority without extending witness freshness", "[feedback][policy]")
{
    auto snapshot = MakeSnapshot(6);
    snapshot.durableCompleted[0] = std::byte{1};
    pbapp::FeedbackSenderPolicy policy;
    REQUIRE(policy.Reset(snapshot.identity, 1));
    REQUIRE(policy.RecordSubmittedFrame(4096));
    REQUIRE(policy.Accept(snapshot, 0));
    REQUIRE(policy.GetTelemetry().effective);
    policy.MarkUnavailable(pbapp::FeedbackError::InvalidFormat);
    REQUIRE_FALSE(policy.GetTelemetry().effective);
    REQUIRE_FALSE(policy.IsSegmentConfirmed(0));
    REQUIRE_FALSE(policy.CanStop(1));
    REQUIRE(policy.Accept(snapshot, pbapp::feedbackFreshnessNanoseconds));
    REQUIRE_FALSE(policy.GetTelemetry().effective);
    REQUIRE(policy.RecordSubmittedFrame(4097));
    snapshot.snapshotSequence++;
    snapshot.observedFrameSequence = 4097;
    REQUIRE(policy.Accept(snapshot, pbapp::feedbackFreshnessNanoseconds + 1));
    REQUIRE(policy.GetTelemetry().effective);
    REQUIRE(policy.IsSegmentConfirmed(0));
}

void MakeCompleted(pbapp::FeedbackSnapshot& snapshot)
{
    snapshot.durableCompleted.assign((snapshot.identity.segmentCount + 7U) / 8U, std::byte{0xFF});
    const std::uint32_t remainder = snapshot.identity.segmentCount % 8U;
    if (remainder != 0)
    {
        snapshot.durableCompleted.back() = static_cast<std::byte>((1U << remainder) - 1U);
    }
    snapshot.phase = pbapp::FeedbackPhase::Completed;
    snapshot.wholeDigestVerified = true;
    snapshot.finalPublishSucceeded = true;
    snapshot.finalReopenVerified = true;
}

}

TEST_CASE("Feedback policy binds full identity before enabling confirmed segments", "[feedback][policy]")
{
    auto snapshot = MakeSnapshot();
    snapshot.durableCompleted = {std::byte{0x09}, std::byte{0x10}};
    pbapp::FeedbackSenderPolicy policy;
    REQUIRE(policy.Reset(snapshot.identity, 11));
    REQUIRE(policy.RecordSubmittedFrame(4096));
    REQUIRE(policy.Accept(snapshot, 1000));
    REQUIRE(policy.GetTelemetry().effective);
    REQUIRE(policy.GetTelemetry().confirmedSegments == 3);
    REQUIRE(policy.IsSegmentConfirmed(0));
    REQUIRE_FALSE(policy.IsSegmentConfirmed(1));
    REQUIRE(policy.IsSegmentConfirmed(3));
    REQUIRE(policy.IsSegmentConfirmed(12));
    REQUIRE_FALSE(policy.IsSegmentConfirmed(13));
    for (const int changedField : {0, 1, 2, 3, 4, 5})
    {
        auto wrong = snapshot;
        wrong.snapshotSequence = 2;
        if (changedField == 0)
        {
            wrong.identity.sessionId.bytes[15] ^= std::byte{1};
        }
        if (changedField == 1)
        {
            wrong.identity.profileId++;
        }
        if (changedField == 2)
        {
            wrong.identity.layoutVersion++;
        }
        if (changedField == 3)
        {
            wrong.identity.fileBytes++;
        }
        if (changedField == 4)
        {
            wrong.identity.canonicalManifestDigest[0] ^= std::byte{1};
        }
        if (changedField == 5)
        {
            wrong.identity.wholeFileDigest.bytes[0] ^= std::byte{1};
        }
        REQUIRE_FALSE(policy.Accept(wrong, 2000));
        REQUIRE_FALSE(policy.GetTelemetry().effective);
        REQUIRE_FALSE(policy.IsSegmentConfirmed(0));
    }
}

TEST_CASE("Feedback duplicates do not renew the fifteen second visual freshness deadline", "[feedback][policy]")
{
    auto snapshot = MakeSnapshot();
    pbapp::FeedbackSenderPolicy policy;
    REQUIRE(policy.Reset(snapshot.identity, 11));
    REQUIRE(policy.RecordSubmittedFrame(4096));
    REQUIRE(policy.Accept(snapshot, 1000));
    REQUIRE(policy.Accept(snapshot, 10000001000ULL));
    REQUIRE(policy.GetTelemetry().duplicateSnapshots == 1);
    REQUIRE_FALSE(policy.Expire(15000000999ULL));
    REQUIRE(policy.Expire(15000001000ULL));
    REQUIRE_FALSE(policy.GetTelemetry().effective);
    snapshot.snapshotSequence = 2;
    REQUIRE(policy.Accept(snapshot, 16000001000ULL));
    REQUIRE_FALSE(policy.GetTelemetry().effective);
    REQUIRE(policy.RecordSubmittedFrame(4097));
    snapshot.snapshotSequence = 3;
    snapshot.observedFrameSequence = 4097;
    REQUIRE(policy.Accept(snapshot, 17000001000ULL));
    REQUIRE(policy.GetTelemetry().effective);
}

TEST_CASE("Feedback current Start membership excludes empty next leased and future sequences", "[feedback][policy]")
{
    const auto snapshot = MakeSnapshot();
    pbapp::FeedbackSenderPolicy policy;
    REQUIRE(policy.Reset(snapshot.identity, 11));
    REQUIRE_FALSE(policy.Accept(snapshot, 1));
    REQUIRE(policy.RecordSubmittedFrame(4096));
    auto future = snapshot;
    future.observedFrameSequence = 4097;
    REQUIRE_FALSE(policy.Accept(future, 2));
    future.observedFrameSequence = 8191;
    REQUIRE_FALSE(policy.Accept(future, 3));
    REQUIRE(policy.Accept(snapshot, 4));
    REQUIRE(policy.Reset(snapshot.identity, 12));
    REQUIRE(policy.RecordSubmittedFrame(8192));
    REQUIRE_FALSE(policy.Accept(snapshot, 5));
    REQUIRE_FALSE(policy.RecordSubmittedFrame((std::numeric_limits<std::uint64_t>::max)()));
}

TEST_CASE("Feedback same sequence conflict latches off instead of latest wins", "[feedback][policy]")
{
    auto snapshot = MakeSnapshot();
    pbapp::FeedbackSenderPolicy policy;
    REQUIRE(policy.Reset(snapshot.identity, 11));
    REQUIRE(policy.RecordSubmittedFrame(4096));
    REQUIRE(policy.Accept(snapshot, 1));
    snapshot.durableCompleted[0] = std::byte{1};
    REQUIRE_FALSE(policy.Accept(snapshot, 2));
    REQUIRE_FALSE(policy.GetTelemetry().effective);
    snapshot.snapshotSequence = 2;
    REQUIRE_FALSE(policy.Accept(snapshot, 3));
    REQUIRE(policy.Reset(snapshot.identity, 12));
    REQUIRE(policy.RecordSubmittedFrame(4096));
    REQUIRE(policy.Accept(snapshot, 4));
}

TEST_CASE("Feedback restarts replace epoch state without OR and retired epochs cannot return", "[feedback][policy]")
{
    auto snapshot = MakeSnapshot();
    snapshot.durableCompleted[0] = std::byte{1};
    const auto old = snapshot;
    pbapp::FeedbackSenderPolicy policy;
    REQUIRE(policy.Reset(snapshot.identity, 11));
    REQUIRE(policy.RecordSubmittedFrame(4096));
    REQUIRE(policy.Accept(snapshot, 1));
    snapshot.receiverEpoch[0] = std::byte{7};
    snapshot.durableCompleted[0] = std::byte{0};
    REQUIRE(policy.Accept(snapshot, 2));
    REQUIRE_FALSE(policy.IsSegmentConfirmed(0));
    REQUIRE_FALSE(policy.Accept(old, 3));
    REQUIRE_FALSE(policy.GetTelemetry().effective);
}

TEST_CASE("Feedback epoch and writer tracking has a finite conflict boundary", "[feedback][policy]")
{
    auto snapshot = MakeSnapshot();
    pbapp::FeedbackSenderPolicy policy;
    REQUIRE(policy.Reset(snapshot.identity, 11));
    REQUIRE(policy.RecordSubmittedFrame(4096));
    REQUIRE(policy.Accept(snapshot, 1));
    for (std::uint8_t epoch = 7; epoch <= 14; epoch++)
    {
        snapshot.receiverEpoch[0] = static_cast<std::byte>(epoch);
        REQUIRE(policy.Accept(snapshot, epoch));
    }
    snapshot.receiverEpoch[0] = std::byte{15};
    REQUIRE_FALSE(policy.Accept(snapshot, 15));
    REQUIRE_FALSE(policy.GetTelemetry().effective);
    REQUIRE(policy.Reset(snapshot.identity, 12));
    REQUIRE(policy.RecordSubmittedFrame(4096));
    REQUIRE(policy.Accept(snapshot, 16));
    snapshot.receiverId[0] ^= std::byte{1};
    snapshot.snapshotSequence++;
    REQUIRE_FALSE(policy.Accept(snapshot, 17));
    REQUIRE_FALSE(policy.GetTelemetry().effective);
}

TEST_CASE("Feedback final stop requires one complete receipt and the current run token", "[feedback][policy]")
{
    auto snapshot = MakeSnapshot();
    snapshot.durableCompleted.assign(2, std::byte{0xFF});
    snapshot.durableCompleted.back() = std::byte{0x1F};
    pbapp::FeedbackSenderPolicy policy;
    REQUIRE(policy.Reset(snapshot.identity, 11));
    REQUIRE(policy.RecordSubmittedFrame(4096));
    REQUIRE(policy.Accept(snapshot, 1));
    REQUIRE_FALSE(policy.CanStop(11));
    MakeCompleted(snapshot);
    snapshot.snapshotSequence++;
    snapshot.reportedCompletionMilliseconds = 300000;
    REQUIRE(policy.Accept(snapshot, 120000000000ULL));
    REQUIRE(policy.CanStop(11));
    REQUIRE_FALSE(policy.CanStop(12));
    REQUIRE_FALSE(policy.CanStop(0));
    REQUIRE_FALSE(policy.Expire(240000000000ULL));
    REQUIRE(policy.CanStop(11));
}

TEST_CASE("Feedback selection is bounded fair and keeps sparse old holes", "[feedback][policy]")
{
    auto snapshot = MakeSnapshot(13);
    snapshot.durableCompleted = {std::byte{0xFD}, std::byte{0x0F}};
    pbapp::FeedbackSenderPolicy policy;
    std::uint64_t cursor = 0;
    REQUIRE_FALSE(pbapp::SelectFeedbackSegments(policy, cursor).overrideSelection);
    REQUIRE(policy.Reset(snapshot.identity, 11));
    REQUIRE(policy.RecordSubmittedFrame(4096));
    REQUIRE(policy.Accept(snapshot, 1));
    const auto sparse = pbapp::SelectFeedbackSegments(policy, cursor);
    REQUIRE(sparse.overrideSelection);
    REQUIRE_FALSE(sparse.keepalive);
    REQUIRE(sparse.count == 2);
    REQUIRE(sparse.segmentOrdinals[0] == 1);
    REQUIRE(sparse.segmentOrdinals[1] == 12);
    snapshot.receiverEpoch[0] = std::byte{9};
    snapshot.durableCompleted.assign(2, std::byte{0});
    REQUIRE(policy.Accept(snapshot, 2));
    cursor = 0;
    const auto first = pbapp::SelectFeedbackSegments(policy, cursor);
    const auto second = pbapp::SelectFeedbackSegments(policy, cursor);
    const auto third = pbapp::SelectFeedbackSegments(policy, cursor);
    REQUIRE(first.count == 6);
    REQUIRE(second.count == 6);
    REQUIRE(third.count == 6);
    REQUIRE(first.segmentOrdinals[0] == 0);
    REQUIRE(second.segmentOrdinals[0] == 6);
    REQUIRE(third.segmentOrdinals[0] == 12);
}

TEST_CASE("Feedback selection handles zero small and maximum segment sets without inventing stop", "[feedback][policy]")
{
    for (const std::uint32_t count : {0U, 1U, 6U, 12U, 13U, 65536U})
    {
        auto snapshot = MakeSnapshot(count);
        pbapp::FeedbackSenderPolicy policy;
        REQUIRE(policy.Reset(snapshot.identity, 11));
        REQUIRE(policy.RecordSubmittedFrame(4096));
        REQUIRE(policy.Accept(snapshot, 1));
        std::uint64_t cursor = 0;
        const auto selected = pbapp::SelectFeedbackSegments(policy, cursor);
        REQUIRE(selected.overrideSelection);
        REQUIRE(selected.count == (count < 6 ? count : 6));
        REQUIRE(selected.keepalive == (count == 0));
        REQUIRE_FALSE(policy.CanStop(11));
        MakeCompleted(snapshot);
        snapshot.phase = pbapp::FeedbackPhase::Verifying;
        snapshot.wholeDigestVerified = false;
        snapshot.finalPublishSucceeded = false;
        snapshot.finalReopenVerified = false;
        snapshot.snapshotSequence++;
        REQUIRE(policy.Accept(snapshot, 2));
        const auto keepalive = pbapp::SelectFeedbackSegments(policy, cursor);
        REQUIRE(keepalive.keepalive);
        REQUIRE(keepalive.count == 0);
        REQUIRE_FALSE(policy.CanStop(11));
    }
}

TEST_CASE("Feedback newer receiver failure cannot be undone by a cached earlier active snapshot", "[feedback][policy]")
{
    const auto snapshot = MakeSnapshot();
    pbapp::FeedbackSenderPolicy policy;
    REQUIRE(policy.Reset(snapshot.identity, 11));
    REQUIRE(policy.RecordSubmittedFrame(4096));
    REQUIRE(policy.Accept(snapshot, 1));
    auto failed = snapshot;
    failed.phase = pbapp::FeedbackPhase::Failed;
    failed.snapshotSequence = 2;
    static_cast<void>(policy.Accept(failed, 2));
    REQUIRE_FALSE(policy.GetTelemetry().effective);
    REQUIRE(policy.GetTelemetry().state == pbapp::FeedbackConnectionState::Unavailable);
    static_cast<void>(policy.Accept(snapshot, 3));
    REQUIRE_FALSE(policy.GetTelemetry().effective);
    REQUIRE_FALSE(policy.CanStop(11));
}
