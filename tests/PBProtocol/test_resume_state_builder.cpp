#include "descriptor_test_helpers.h"

#include "pbprotocol/resume_state.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace {

using pbprotocol::LoadedResumeState;
using pbprotocol::LoadResumeState;
using pbprotocol::ProtocolErrorCode;
using pbprotocol::ReceiverResourcePolicy;

[[nodiscard]] constexpr std::byte Byte(const std::uint8_t value) noexcept
{
    return static_cast<std::byte>(value);
}

[[nodiscard]] ReceiverResourcePolicy MakeBuilderPolicy(
    const std::uint64_t maxResumeBytes) noexcept
{
    ReceiverResourcePolicy resourcePolicy = pbprotocol::test::MakeResourcePolicy();
    resourcePolicy.maxResumeBytes = maxResumeBytes;
    return resourcePolicy;
}

[[nodiscard]] std::array<std::byte, 16> MakeSid(const std::uint8_t base) noexcept
{
    std::array<std::byte, 16> bytes{};
    for (std::size_t byteIndex = 0; byteIndex < 16U; byteIndex++)
    {
        bytes[byteIndex] = Byte(static_cast<std::uint8_t>(base + static_cast<unsigned int>(byteIndex)));
    }
    return bytes;
}

[[nodiscard]] std::array<std::byte, 32> MakeDigest(const std::uint8_t firstByte) noexcept
{
    std::array<std::byte, 32> bytes{};
    for (std::size_t byteIndex = 0; byteIndex < 32U; byteIndex++)
    {
        bytes[byteIndex] = Byte(static_cast<std::uint8_t>(firstByte + static_cast<unsigned int>(byteIndex)));
    }
    return bytes;
}

[[nodiscard]] pbprotocol::ResumeCompletedSegmentRecord MakeCompleted(
    const std::array<std::byte, 16>& sidBytes,
    const std::uint64_t segmentOrdinal,
    const std::uint64_t rawSize) noexcept
{
    return pbprotocol::ResumeCompletedSegmentRecord{
        pbprotocol::SessionId{sidBytes},
        segmentOrdinal,
        0ULL,
        rawSize,
        pbprotocol::RawDigest{MakeDigest(0x20)}};
}

[[nodiscard]] pbprotocol::ResumeActiveWirehairCacheRecord MakeCache(
    const std::uint64_t segmentOrdinal)
{
    pbprotocol::ResumeActiveWirehairCacheRecord record{};
    record.segmentOrdinal = segmentOrdinal;
    for (std::size_t byteIndex = 0; byteIndex < record.wirehairProfile.bytes.size(); byteIndex++)
    {
        record.wirehairProfile.bytes[byteIndex] = Byte(
            static_cast<std::uint8_t>(static_cast<unsigned int>(byteIndex) * 5U));
    }
    pbprotocol::ResumeWirehairCacheEntry entry{};
    entry.outerBlockId = 11U;
    entry.payload = {Byte(0x7A), Byte(0x3C)};
    record.entries.push_back(std::move(entry));
    return record;
}

[[nodiscard]] pbprotocol::ResumeActiveDirectRepeatRecord MakeDirectRecord(
    const std::uint64_t segmentOrdinal)
{
    pbprotocol::ResumeActiveDirectRepeatRecord record{};
    record.segmentOrdinal = segmentOrdinal;
    record.directBlockCount = 4U;
    pbprotocol::ResumeDirectRepeatEntry entry{};
    entry.blockOrdinal = 1U;
    entry.realPayloadBytes = 2U;
    entry.paddedPayload = {Byte(0x5D), Byte(0x5E), std::byte{0}};
    record.entries.push_back(std::move(entry));
    return record;
}

[[nodiscard]] std::vector<std::byte> SnapshotDocument(
    const pbprotocol::ResumeStateBuilder& builder)
{
    const std::span<const std::byte> document = builder.GetDocument();
    return {document.begin(), document.end()};
}

TEST_CASE("Builder rejects invalid appends without mutating state", "[pbprotocol][resume-state]")
{
    const ReceiverResourcePolicy resourcePolicy = MakeBuilderPolicy(4096);
    auto createResult = pbprotocol::ResumeStateBuilder::Create(resourcePolicy);
    REQUIRE(createResult.HasValue());
    pbprotocol::ResumeStateBuilder builder = std::move(createResult).Value();

    // A zero-sized policy is rejected before any state exists.
    const ReceiverResourcePolicy zeroPolicy{};
    const auto invalidCreate = pbprotocol::ResumeStateBuilder::Create(zeroPolicy);
    CHECK_FALSE(static_cast<bool>(invalidCreate));
    CHECK(invalidCreate.Error().code == ProtocolErrorCode::InvalidResourcePolicy);

    const std::vector<std::byte> initialSnapshot = SnapshotDocument(builder);
    REQUIRE(initialSnapshot.empty());

    // (a) Zero raw size: rejected, no key is claimed.
    {
        pbprotocol::ResumeCompletedSegmentRecord bad = MakeCompleted(MakeSid(0x10), 5, 0ULL);
        const auto status = builder.AppendCompletedSegment(bad);
        CHECK_FALSE(static_cast<bool>(status));
        CHECK(status.Error().code == ProtocolErrorCode::InvalidRecordSize);
        CHECK(SnapshotDocument(builder) == initialSnapshot);

        // The failed append must not have claimed the key: a valid record with
        // the same (sessionId, ordinal) is still accepted.
        REQUIRE(static_cast<bool>(builder.AppendCompletedSegment(
            MakeCompleted(MakeSid(0x10), 5, 64ULL))));
    }

    const std::vector<std::byte> afterFirst = SnapshotDocument(builder);
    REQUIRE(!afterFirst.empty());

    // (b) Checked-add overflow sentinel on rawOffset + rawSize.
    {
        pbprotocol::ResumeCompletedSegmentRecord overflow =
            MakeCompleted(MakeSid(0x11), 6, 8ULL);
        overflow.rawOffset = std::numeric_limits<std::uint64_t>::max();
        const auto status = builder.AppendCompletedSegment(overflow);
        CHECK_FALSE(static_cast<bool>(status));
        CHECK(status.Error().code == ProtocolErrorCode::LengthOverflow);
        CHECK(SnapshotDocument(builder) == afterFirst);
    }

    // (c) Same outerBlockId with conflicting payloads inside one cache record.
    {
        pbprotocol::ResumeActiveWirehairCacheRecord conflict = MakeCache(7);
        pbprotocol::ResumeWirehairCacheEntry duplicate{};
        duplicate.outerBlockId = 11U;
        duplicate.payload = {Byte(0xFF)};
        conflict.entries.push_back(std::move(duplicate));
        const auto status = builder.AppendActiveWirehairCache(conflict);
        CHECK_FALSE(static_cast<bool>(status));
        CHECK(status.Error().code == ProtocolErrorCode::ResumeRecordConflict);
        CHECK(SnapshotDocument(builder) == afterFirst);

        // Identical duplicate entries are rejected by the strict writer too.
        pbprotocol::ResumeActiveWirehairCacheRecord identical = MakeCache(7);
        pbprotocol::ResumeWirehairCacheEntry same{};
        same.outerBlockId = 11U;
        same.payload = {Byte(0x7A), Byte(0x3C)};
        identical.entries.push_back(std::move(same));
        const auto identicalStatus = builder.AppendActiveWirehairCache(identical);
        CHECK_FALSE(static_cast<bool>(identicalStatus));
        CHECK(identicalStatus.Error().code == ProtocolErrorCode::ResumeRecordConflict);
        CHECK(SnapshotDocument(builder) == afterFirst);

        // The rejected record did not claim the ordinal.
        REQUIRE(static_cast<bool>(builder.AppendActiveWirehairCache(MakeCache(7))));
    }

    const std::vector<std::byte> afterSecond = SnapshotDocument(builder);

    // (d) Cross-category ordinal claims in both directions.
    {
        pbprotocol::ResumeCompletedSegmentRecord claim =
            MakeCompleted(MakeSid(0x12), 7, 32ULL);
        const auto completedOnCacheOrdinal = builder.AppendCompletedSegment(claim);
        CHECK_FALSE(static_cast<bool>(completedOnCacheOrdinal));
        CHECK(completedOnCacheOrdinal.Error().code == ProtocolErrorCode::ResumeRecordConflict);

        pbprotocol::ResumeActiveDirectRepeatRecord claimDirect = MakeDirectRecord(7);
        const auto directOnCacheOrdinal = builder.AppendDirectRepeatReceivedBlocks(claimDirect);
        CHECK_FALSE(static_cast<bool>(directOnCacheOrdinal));
        CHECK(directOnCacheOrdinal.Error().code == ProtocolErrorCode::ResumeRecordConflict);

        CHECK(SnapshotDocument(builder) == afterSecond);

        // The rejected records did not claim their ordinals, and a fresh
        // unclaimed ordinal is still accepted (ordinal 8 is used by no
        // accepted record).
        REQUIRE(static_cast<bool>(builder.AppendDirectRepeatReceivedBlocks(MakeDirectRecord(8))));
    }

    const std::vector<std::byte> afterThird = SnapshotDocument(builder);

    // (e) Same completed key repeated with identical content is still a
    // conflict for the strict writer.
    {
        const auto duplicateKey = builder.AppendCompletedSegment(
            MakeCompleted(MakeSid(0x10), 5, 64ULL));
        CHECK_FALSE(static_cast<bool>(duplicateKey));
        CHECK(duplicateKey.Error().code == ProtocolErrorCode::ResumeRecordConflict);
        CHECK(SnapshotDocument(builder) == afterThird);
    }

    // (f) A DirectRepeat entry with blockOrdinal >= declared count.
    {
        pbprotocol::ResumeActiveDirectRepeatRecord outOfRange = MakeDirectRecord(8);
        for (auto& entry : outOfRange.entries)
        {
            entry.blockOrdinal = 4U;
        }
        const auto status = builder.AppendDirectRepeatReceivedBlocks(outOfRange);
        CHECK_FALSE(static_cast<bool>(status));
        CHECK(status.Error().code == ProtocolErrorCode::SegmentOrdinalOutOfRange);
        CHECK(SnapshotDocument(builder) == afterThird);

        pbprotocol::ResumeActiveDirectRepeatRecord zeroCount = MakeDirectRecord(8);
        zeroCount.directBlockCount = 0U;
        const auto zeroStatus = builder.AppendDirectRepeatReceivedBlocks(zeroCount);
        CHECK_FALSE(static_cast<bool>(zeroStatus));
        CHECK(zeroStatus.Error().code == ProtocolErrorCode::InvalidRecordSize);

        pbprotocol::ResumeActiveDirectRepeatRecord shortEntry = MakeDirectRecord(8);
        shortEntry.entries[0].realPayloadBytes = 4U;  // exceeds padded size 3
        const auto shortStatus = builder.AppendDirectRepeatReceivedBlocks(shortEntry);
        CHECK_FALSE(static_cast<bool>(shortStatus));
        CHECK(shortStatus.Error().code == ProtocolErrorCode::InvalidRecordSize);
    }

    // Final document loads and matches every accepted append.
    const std::vector<std::byte> finalDocument = SnapshotDocument(builder);
    const auto loadResult = LoadResumeState(
        std::span<const std::byte>(finalDocument), resourcePolicy);
    REQUIRE(loadResult.HasValue());
    CHECK_FALSE(loadResult.Value().hasTruncatedTail);

    LoadedResumeState expected{};
    expected.completedSegments.push_back(MakeCompleted(MakeSid(0x10), 5, 64ULL));
    expected.activeWirehairCaches.push_back(MakeCache(7));
    expected.activeDirectRepeatRecords.push_back(MakeDirectRecord(8));
    CHECK(loadResult.Value() == expected);
}

TEST_CASE("Builder budget exhaustion is exact and atomic", "[pbprotocol][resume-state]")
{
    // One completed record serializes to exactly 18 + 73 = 91 bytes.
    const ReceiverResourcePolicy exactFitPolicy = MakeBuilderPolicy(91);
    auto builderResult = pbprotocol::ResumeStateBuilder::Create(exactFitPolicy);
    REQUIRE(builderResult.HasValue());
    pbprotocol::ResumeStateBuilder builder = std::move(builderResult).Value();

    REQUIRE(static_cast<bool>(builder.AppendCompletedSegment(MakeCompleted(MakeSid(0x30), 1, 4ULL))));
    CHECK(builder.GetByteCount() == 91U);

    const std::vector<std::byte> fullSnapshot = SnapshotDocument(builder);
    REQUIRE(fullSnapshot.size() == 91U);

    // The next append must fail on budget without touching the document.
    const auto overBudget = builder.AppendCompletedSegment(MakeCompleted(MakeSid(0x31), 2, 4ULL));
    CHECK_FALSE(static_cast<bool>(overBudget));
    CHECK(overBudget.Error().code == ProtocolErrorCode::ResourceLimitExceeded);
    CHECK(builder.GetByteCount() == 91U);
    CHECK(SnapshotDocument(builder) == fullSnapshot);

    // The exact-fit document loads cleanly.
    const auto loadResult = LoadResumeState(
        std::span<const std::byte>(fullSnapshot), exactFitPolicy);
    REQUIRE(loadResult.HasValue());
    CHECK_FALSE(loadResult.Value().hasTruncatedTail);
    CHECK(loadResult.Value().completedSegments.size() == 1U);
}

TEST_CASE("Builder cache record budget fit is exact", "[pbprotocol][resume-state]")
{
    // MakeCache(7) serializes to 18 + (45 + 8 + 2) = 73 bytes.
    const ReceiverResourcePolicy policy = MakeBuilderPolicy(73);
    auto firstResult = pbprotocol::ResumeStateBuilder::Create(policy);
    REQUIRE(firstResult.HasValue());
    pbprotocol::ResumeStateBuilder builder = std::move(firstResult).Value();

    REQUIRE(static_cast<bool>(builder.AppendActiveWirehairCache(MakeCache(1))));
    CHECK(builder.GetByteCount() == 73U);
    const std::vector<std::byte> snapshot = SnapshotDocument(builder);

    // A second cache record (identical size) cannot fit the budget.
    const auto overBudget = builder.AppendActiveWirehairCache(MakeCache(2));
    CHECK_FALSE(static_cast<bool>(overBudget));
    CHECK(overBudget.Error().code == ProtocolErrorCode::ResourceLimitExceeded);
    CHECK(builder.GetByteCount() == 73U);
    CHECK(SnapshotDocument(builder) == snapshot);

    const auto loadResult = LoadResumeState(
        std::span<const std::byte>(snapshot), policy);
    REQUIRE(loadResult.HasValue());
    CHECK_FALSE(loadResult.Value().hasTruncatedTail);
    CHECK(loadResult.Value().activeWirehairCaches.size() == 1U);
}

TEST_CASE("Empty builder produces a loadable empty document", "[pbprotocol][resume-state]")
{
    const ReceiverResourcePolicy policy = MakeBuilderPolicy(64);
    auto builderResult = pbprotocol::ResumeStateBuilder::Create(policy);
    REQUIRE(builderResult.HasValue());
    pbprotocol::ResumeStateBuilder builder = std::move(builderResult).Value();

    CHECK(builder.GetByteCount() == 0U);
    CHECK(builder.GetDocument().empty());

    const std::vector<std::byte> emptyDocument = SnapshotDocument(builder);
    REQUIRE(emptyDocument.empty());
    const auto loadResult = LoadResumeState(
        std::span<const std::byte>(emptyDocument), policy);
    REQUIRE(loadResult.HasValue());
    CHECK_FALSE(loadResult.Value().hasTruncatedTail);
    CHECK(loadResult.Value().completedSegments.empty());
    CHECK(loadResult.Value().activeWirehairCaches.empty());
    CHECK(loadResult.Value().activeDirectRepeatRecords.empty());
}

TEST_CASE("Builder move semantics preserve the document", "[pbprotocol][resume-state]")
{
    const ReceiverResourcePolicy policy = MakeBuilderPolicy(4096);

    auto sourceResult = pbprotocol::ResumeStateBuilder::Create(policy);
    REQUIRE(sourceResult.HasValue());
    pbprotocol::ResumeStateBuilder source = std::move(sourceResult).Value();
    REQUIRE(static_cast<bool>(source.AppendCompletedSegment(MakeCompleted(MakeSid(0x40), 1, 8ULL))));
    REQUIRE(static_cast<bool>(source.AppendActiveWirehairCache(MakeCache(9))));

    // Move-construction transfers the accumulated document.
    pbprotocol::ResumeStateBuilder moved(std::move(source));
    CHECK(moved.GetByteCount() > 0U);
    const std::vector<std::byte> afterMove = SnapshotDocument(moved);

    LoadedResumeState expected{};
    expected.completedSegments.push_back(MakeCompleted(MakeSid(0x40), 1, 8ULL));
    expected.activeWirehairCaches.push_back(MakeCache(9));
    const auto loadResult = LoadResumeState(
        std::span<const std::byte>(afterMove), policy);
    REQUIRE(loadResult.HasValue());
    CHECK_FALSE(loadResult.Value().hasTruncatedTail);
    CHECK(loadResult.Value() == expected);

    // Move-assignment transfers from a second builder.
    auto donorResult = pbprotocol::ResumeStateBuilder::Create(policy);
    REQUIRE(donorResult.HasValue());
    pbprotocol::ResumeStateBuilder donor = std::move(donorResult).Value();
    REQUIRE(static_cast<bool>(donor.AppendDirectRepeatReceivedBlocks(MakeDirectRecord(3))));

    auto receiverResult = pbprotocol::ResumeStateBuilder::Create(policy);
    REQUIRE(receiverResult.HasValue());
    pbprotocol::ResumeStateBuilder receiver = std::move(receiverResult).Value();
    receiver = std::move(donor);
    const std::vector<std::byte> afterAssignment = SnapshotDocument(receiver);

    const auto assignedLoadResult = LoadResumeState(
        std::span<const std::byte>(afterAssignment), policy);
    REQUIRE(assignedLoadResult.HasValue());
    CHECK_FALSE(assignedLoadResult.Value().hasTruncatedTail);
    REQUIRE(assignedLoadResult.Value().activeDirectRepeatRecords.size() == 1U);
    CHECK(assignedLoadResult.Value().activeDirectRepeatRecords[0]
        == MakeDirectRecord(3));

    // Self-assignment must be a no-op (guard in the implementation).
    receiver = std::move(receiver);
    CHECK(SnapshotDocument(receiver) == afterAssignment);
}

TEST_CASE("Builder load equivalence on mixed multi-record documents", "[pbprotocol][resume-state]")
{
    const ReceiverResourcePolicy policy = MakeBuilderPolicy(8192);
    auto builderResult = pbprotocol::ResumeStateBuilder::Create(policy);
    REQUIRE(builderResult.HasValue());
    pbprotocol::ResumeStateBuilder builder = std::move(builderResult).Value();

    // Interleaved categories with distinct ordinals.
    REQUIRE(static_cast<bool>(builder.AppendDirectRepeatReceivedBlocks(MakeDirectRecord(101))));
    REQUIRE(static_cast<bool>(builder.AppendCompletedSegment(MakeCompleted(MakeSid(0x50), 202, 16ULL))));
    REQUIRE(static_cast<bool>(builder.AppendActiveWirehairCache(MakeCache(303))));
    REQUIRE(static_cast<bool>(builder.AppendDirectRepeatReceivedBlocks(MakeDirectRecord(404))));

    const std::vector<std::byte> document = SnapshotDocument(builder);
    const auto loadResult = LoadResumeState(std::span<const std::byte>(document), policy);
    REQUIRE(loadResult.HasValue());
    CHECK_FALSE(loadResult.Value().hasTruncatedTail);

    LoadedResumeState expected{};
    expected.completedSegments.push_back(MakeCompleted(MakeSid(0x50), 202, 16ULL));
    expected.activeWirehairCaches.push_back(MakeCache(303));
    pbprotocol::ResumeActiveDirectRepeatRecord firstDirect = MakeDirectRecord(101);
    pbprotocol::ResumeActiveDirectRepeatRecord secondDirect = MakeDirectRecord(404);
    expected.activeDirectRepeatRecords.push_back(std::move(firstDirect));
    expected.activeDirectRepeatRecords.push_back(std::move(secondDirect));
    CHECK(loadResult.Value() == expected);
}

// Mirror of LoadResumeState's completed-count quota gate (round-trip law): the
// builder must never emit a document whose load would fail on maxSegmentCount.
// Duplicate-key rejection keeps precedence over the quota, matching the load
// path where dedup/conflict scans run before the count check.
TEST_CASE("Builder mirrors the loader completed-count quota gate", "[pbprotocol][resume-state]")
{
    ReceiverResourcePolicy resourcePolicy = pbprotocol::test::MakeResourcePolicy();
    resourcePolicy.maxResumeBytes = 4096U;
    resourcePolicy.maxSegmentCount = 2U;

    auto createResult = pbprotocol::ResumeStateBuilder::Create(resourcePolicy);
    REQUIRE(createResult.HasValue());
    pbprotocol::ResumeStateBuilder builder = std::move(createResult).Value();

    const auto firstStatus = builder.AppendCompletedSegment(MakeCompleted(MakeSid(0x10), 1, 8ULL));
    CHECK(static_cast<bool>(firstStatus));
    const auto secondStatus = builder.AppendCompletedSegment(MakeCompleted(MakeSid(0x20), 2, 9ULL));
    CHECK(static_cast<bool>(secondStatus));

    // Third distinct key is rejected at exactly the loader's threshold.
    const std::vector<std::byte> snapshotAfterTwo = SnapshotDocument(builder);
    REQUIRE(snapshotAfterTwo.size() == 182U); // two fixed-layout 91-byte records.

    const auto thirdStatus = builder.AppendCompletedSegment(MakeCompleted(MakeSid(0x30), 3, 10ULL));
    CHECK_FALSE(static_cast<bool>(thirdStatus));
    CHECK(thirdStatus.Error().code == ProtocolErrorCode::ResourceLimitExceeded);
    // A failed append leaves the accumulated document byte-identical.
    CHECK(SnapshotDocument(builder) == snapshotAfterTwo);

    // Even over quota, a duplicate key is reported as conflict first: the error
    // precedence stays in lockstep with the load path's dedup/conflict scan.
    const auto duplicateStatus = builder.AppendCompletedSegment(MakeCompleted(MakeSid(0x10), 1, 8ULL));
    CHECK_FALSE(static_cast<bool>(duplicateStatus));
    CHECK(duplicateStatus.Error().code == ProtocolErrorCode::ResumeRecordConflict);
    CHECK(SnapshotDocument(builder) == snapshotAfterTwo);

    // The accumulated document loads with exactly two records: writer/reader agree.
    const auto loadResult = LoadResumeState(std::span<const std::byte>(snapshotAfterTwo), resourcePolicy);
    REQUIRE(static_cast<bool>(loadResult));
    CHECK_FALSE(loadResult.Value().hasTruncatedTail);
    CHECK(loadResult.Value().completedSegments.size() == 2U);
}

TEST_CASE("Builder mirrors the loader segment quota across categories", "[pbprotocol][resume-state]")
{
    ReceiverResourcePolicy resourcePolicy = MakeBuilderPolicy(4096);
    resourcePolicy.maxSegmentCount = 2U;
    auto createResult = pbprotocol::ResumeStateBuilder::Create(resourcePolicy);
    REQUIRE(static_cast<bool>(createResult));
    pbprotocol::ResumeStateBuilder builder = std::move(createResult).Value();

    // Active caches consume the same per-segment quota as completed records.
    const auto cacheStatus = builder.AppendActiveWirehairCache(MakeCache(21U));
    REQUIRE(static_cast<bool>(cacheStatus));
    const std::vector<std::byte> afterCache = SnapshotDocument(builder);
    CHECK(afterCache.size() > 0U);

    const auto directStatus = builder.AppendDirectRepeatReceivedBlocks(MakeDirectRecord(22U));
    REQUIRE(static_cast<bool>(directStatus));
    const std::vector<std::byte> afterDirect = SnapshotDocument(builder);
    CHECK(afterDirect.size() > afterCache.size());

    // The quota is exhausted across categories: any new ordinal of any category
    // is rejected with the document untouched.
    {
        const auto status = builder.AppendCompletedSegment(
            MakeCompleted(MakeSid(0x30), 23U, 64ULL));
        CHECK_FALSE(static_cast<bool>(status));
        CHECK(status.Error().code == ProtocolErrorCode::ResourceLimitExceeded);

        const auto secondStatus = builder.AppendActiveWirehairCache(MakeCache(24U));
        CHECK_FALSE(static_cast<bool>(secondStatus));
        CHECK(secondStatus.Error().code == ProtocolErrorCode::ResourceLimitExceeded);

        CHECK(SnapshotDocument(builder) == afterDirect);
    }

    // Conflict scans keep their precedence over the quota gate even at-quota.
    {
        const auto conflictStatus = builder.AppendActiveWirehairCache(MakeCache(21U));
        CHECK_FALSE(static_cast<bool>(conflictStatus));
        CHECK(conflictStatus.Error().code == ProtocolErrorCode::ResumeRecordConflict);
    }

    // The accumulated document still loads with exactly the two accepted records.
    const auto loadResult = LoadResumeState(builder.GetDocument(), resourcePolicy);
    REQUIRE(static_cast<bool>(loadResult));
    CHECK_FALSE(loadResult.Value().hasTruncatedTail);
    CHECK(loadResult.Value().activeWirehairCaches.size() == 1U);
    CHECK(loadResult.Value().activeDirectRepeatRecords.size() == 1U);
}

} // namespace
