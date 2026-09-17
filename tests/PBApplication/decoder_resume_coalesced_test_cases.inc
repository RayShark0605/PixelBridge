// Shares the batch test fixture; this include is compiled only by PBDecoderResumeBatchTests.
// These are journal-layer fixtures, not claims of FEC or file recovery.
namespace
{
constexpr std::uint64_t largeFixtureMebibyte = 1048576;
constexpr std::uint64_t largeFixtureSegments = 15;
constexpr std::uint64_t largeFixtureSegmentBytes = 6 * largeFixtureMebibyte;
constexpr std::uint32_t largeFixtureBlockBytes = 65535;
constexpr std::uint32_t largeFixtureSeedBlocks = 96;

std::array<std::byte, pbprotocol::kDigestBytes> HashClosedJournal(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input);
    std::vector<std::byte> bytes(largeFixtureMebibyte);
    pbprotocol::Blake3Hasher hasher;
    while (input)
    {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        const auto count = input.gcount();
        REQUIRE(count >= 0);
        hasher.Update(std::span(bytes).first(static_cast<std::size_t>(count)));
    }
    REQUIRE(input.eof());
    return hasher.Finalize();
}

struct LargeJournalFixture
{
    ScratchDirectory scratch;
    pbapp::DecoderMemoryBudget budget{512 * largeFixtureMebibyte, 512 * largeFixtureMebibyte};
    pbprotocol::ReceiverResourcePolicy policy = pbprotocol::GetDefaultReceiverResourcePolicy();
    pbprotocol::SessionDescriptor session;
    pbprotocol::SessionTag tag;
    std::vector<std::byte> sessionControl;
    std::vector<pbprotocol::SegmentDescriptor> descriptors;
    std::vector<std::byte> basePayload = MakeBytes(largeFixtureBlockBytes);
    std::unique_ptr<pbapp::DecoderResumeStore> store;
    pbapp::DecoderResumeLoadedState loaded;
    std::filesystem::path journalPath;

    LargeJournalFixture()
    {
        policy.maxActiveOuterFecDecoders = policy.maxSegmentCount;
        REQUIRE(pbapp::ApplyDecoderMemoryBudget(budget, policy) == pbapp::DecoderMemoryBudgetError::None);
        REQUIRE(policy.maxResumeBytes == 128 * largeFixtureMebibyte);
        const auto raw = MakeBytes(largeFixtureSegmentBytes);
        const auto encoder = pbouterfec::WirehairV2Encoder::Create(raw, largeFixtureBlockBytes);
        REQUIRE(encoder);
        REQUIRE(encoder.Value().GetBlockCount() > largeFixtureSeedBlocks);
        const auto digest = pbprotocol::ComputeBlake3Digest(raw);
        session.protocolVersion = pbprotocol::GetProtocolVersion();
        session.sessionId.bytes[0] = std::byte{0x37};
        session.originalFileSize = largeFixtureSegments * largeFixtureSegmentBytes;
        session.segmentCount = largeFixtureSegments;
        session.digestAlgorithm = pbprotocol::DigestAlgorithm::Blake3_256;
        session.sessionVisualProfileId = pbprotocol::kUnifiedVisualProfileId;
        session.fileNameUtf8 = "coalesced-local-journal-only.bin";
        REQUIRE(session.originalFileSize < 100000000);
        tag = pbprotocol::DeriveSessionTag(session.sessionId);
        const auto size = pbprotocol::GetSerializedSize(session);
        REQUIRE(size);
        std::vector<std::byte> payload(size.Value());
        REQUIRE(pbprotocol::SerializeSessionDescriptor(session, policy, payload));
        sessionControl = WrapControl(pbprotocol::ControlRecordType::SessionDescriptor, 1, tag, payload);
        REQUIRE(pbapp::DecoderResumeStore::Open(scratch.GetPath(), tag, sessionControl, policy, store, loaded, true, &budget));
        journalPath = store->GetPath();
        for (std::uint64_t ordinal = 0; ordinal < largeFixtureSegments; ordinal++)
        {
            pbprotocol::SegmentDescriptor descriptor;
            descriptor.sessionTag = tag;
            descriptor.segmentOrdinal = ordinal;
            descriptor.rawOffset = ordinal * largeFixtureSegmentBytes;
            descriptor.rawSize = largeFixtureSegmentBytes;
            descriptor.encodedSize = largeFixtureSegmentBytes;
            descriptor.compressionCodec = pbprotocol::CompressionCodec::Raw;
            descriptor.outerFecMode = pbprotocol::OuterFecMode::WirehairV2;
            descriptor.outerBlockBytes = largeFixtureBlockBytes;
            descriptor.wirehairV2SerializedProfile = encoder.Value().GetSerializedProfile();
            descriptor.rawDigest = pbprotocol::RawDigest{digest};
            descriptor.encodedDigest = pbprotocol::EncodedDigest{digest};
            const auto descriptorSize = pbprotocol::GetSerializedSize(descriptor);
            REQUIRE(descriptorSize);
            payload.resize(descriptorSize.Value());
            REQUIRE(pbprotocol::SerializeSegmentDescriptor(descriptor, session, policy, payload));
            REQUIRE(store->RecordSegmentControl(WrapControl(pbprotocol::ControlRecordType::SegmentDescriptor, ordinal + 2, tag, payload)));
            descriptors.push_back(descriptor);
        }
    }

    pbapp::DecoderResumeAcceptedBlock Block(const std::uint64_t ordinal, const std::uint32_t blockId) const
    {
        pbapp::DecoderResumeAcceptedBlock block;
        block.segmentOrdinal = ordinal;
        block.outerBlockId = blockId;
        block.declaredPayloadBytes = largeFixtureBlockBytes;
        block.paddedPayload = basePayload;
        block.paddedPayload[0] = static_cast<std::byte>(ordinal);
        for (std::size_t index = 0; index < 4; index++)
        {
            block.paddedPayload[index + 1] = static_cast<std::byte>((blockId >> (8 * index)) & 255);
        }
        return block;
    }

    void Accept(const std::uint64_t ordinal, const std::uint32_t first, const std::uint32_t count)
    {
        for (std::uint32_t index = 0; index < count; index++)
        {
            REQUIRE(store->RecordAcceptedBlock(Block(ordinal, first + index)));
        }
    }

    void Seed()
    {
        for (std::uint64_t ordinal = 0; ordinal < largeFixtureSegments; ordinal++)
        {
            Accept(ordinal, 0, largeFixtureSeedBlocks);
        }
        const auto initialGeneration = store->GetGeneration();
        REQUIRE(store->Checkpoint());
        REQUIRE(store->GetGeneration() == initialGeneration + largeFixtureSegments * largeFixtureSeedBlocks);
        REQUIRE(store->GetFileBytes() > 64 * largeFixtureMebibyte);
        REQUIRE(store->GetFileBytes() < policy.maxResumeBytes * 3 / 4);
    }

    pbprotocol::ResumeCompletedSegmentRecord Completed(const std::uint64_t ordinal) const
    {
        const auto& descriptor = descriptors[static_cast<std::size_t>(ordinal)];
        return {session.sessionId, ordinal, descriptor.rawOffset, descriptor.rawSize, descriptor.rawDigest};
    }

    void Reopen(const std::size_t expectedBlocks, const std::size_t expectedCompleted)
    {
        store.reset();
        loaded = {};
        REQUIRE(pbapp::DecoderResumeStore::Open(scratch.GetPath(), tag, sessionControl, policy, store, loaded, true, &budget));
        REQUIRE(loaded.resumed);
        REQUIRE_FALSE(loaded.hadTruncatedTail);
        REQUIRE(loaded.activeBlocks.size() == expectedBlocks);
        REQUIRE(loaded.completedSegments.size() == expectedCompleted);
        for (const auto& restored : loaded.activeBlocks)
        {
            REQUIRE(restored == Block(restored.segmentOrdinal, restored.outerBlockId));
            REQUIRE(std::ranges::none_of(loaded.completedSegments, [&](const auto& value) { return value.segmentOrdinal == restored.segmentOrdinal; }));
        }
        for (const auto& completed : loaded.completedSegments)
        {
            REQUIRE(completed == Completed(completed.segmentOrdinal));
        }
        loaded = {};
    }
};
}

TEST_CASE("Budget-bound resume large completed marker stays durable while low-garbage compaction is deferred", "[application][resume][coalesced][durable]")
{
    LargeJournalFixture fixture;
    fixture.Seed();
    const auto initialBytes = fixture.store->GetFileBytes();
    const auto initialGeneration = fixture.store->GetGeneration();
    IoState state;
    {
        ScopedIo observe(state);
        REQUIRE(fixture.store->RecordCompletedSegment(fixture.Completed(0)));
    }
    REQUIRE(state.writeCalls == 1);
    REQUIRE(state.flushCalls == 1);
    REQUIRE(fixture.store->GetFileBytes() == initialBytes + 108);
    REQUIRE(fixture.store->GetGeneration() == initialGeneration + 1);
    REQUIRE(fixture.store->GetActiveBlockCount() == 14 * largeFixtureSeedBlocks);
    REQUIRE_FALSE(fixture.store->RecordAcceptedBlock(fixture.Block(0, 0)));
    fixture.Reopen(14 * largeFixtureSeedBlocks, 1);
}

TEST_CASE("Budget-bound resume garbage fraction eventually compacts and retires all completed payload", "[application][resume][coalesced][threshold]")
{
    LargeJournalFixture fixture;
    fixture.Seed();
    const auto initialBytes = fixture.store->GetFileBytes();
    for (std::uint64_t ordinal = 0; ordinal < 3; ordinal++)
    {
        REQUIRE(fixture.store->RecordCompletedSegment(fixture.Completed(ordinal)));
        REQUIRE(fixture.store->GetFileBytes() == initialBytes + (ordinal + 1) * 108);
    }
    const auto beforeFourth = fixture.store->GetGeneration();
    REQUIRE(fixture.store->RecordCompletedSegment(fixture.Completed(3)));
    REQUIRE(fixture.store->GetGeneration() > beforeFourth + 1);
    REQUIRE(fixture.store->GetFileBytes() > 64 * largeFixtureMebibyte);
    REQUIRE(fixture.store->GetFileBytes() < initialBytes - 20 * largeFixtureMebibyte);
    fixture.Reopen(11 * largeFixtureSeedBlocks, 4);
}

TEST_CASE("Budget-bound resume pending quota pressure commits one complete snapshot instead of a nonfitting append", "[application][resume][coalesced][quota]")
{
    LargeJournalFixture fixture;
    fixture.Seed();
    REQUIRE(fixture.store->RecordCompletedSegment(fixture.Completed(0)));
    const auto oldBytes = fixture.store->GetFileBytes();
    constexpr std::uint32_t pendingCount = 640;
    fixture.Accept(14, largeFixtureSeedBlocks, pendingCount);
    REQUIRE(oldBytes + static_cast<std::uint64_t>(pendingCount) * (56 + largeFixtureBlockBytes) > fixture.policy.maxResumeBytes);
    IoState state;
    {
        ScopedIo observe(state);
        REQUIRE(fixture.store->Checkpoint());
    }
    REQUIRE(state.writeCalls == 0);
    REQUIRE(fixture.store->GetPendingBlockCount() == 0);
    REQUIRE(fixture.store->GetFileBytes() <= fixture.policy.maxResumeBytes);
    REQUIRE(fixture.store->GetActiveBlockCount() == 14 * largeFixtureSeedBlocks + pendingCount);
    fixture.Reopen(14 * largeFixtureSeedBlocks + pendingCount, 1);
}

TEST_CASE("Budget-bound resume over-quota live snapshot fails before modifying the prior durable journal", "[application][resume][coalesced][quota][failure]")
{
    LargeJournalFixture fixture;
    fixture.Seed();
    REQUIRE(fixture.store->RecordCompletedSegment(fixture.Completed(0)));
    fixture.store.reset();
    const auto originalHash = HashClosedJournal(fixture.journalPath);
    fixture.Reopen(14 * largeFixtureSeedBlocks, 1);
    const auto originalGeneration = fixture.store->GetGeneration();
    const auto originalBytes = fixture.store->GetFileBytes();
    fixture.Accept(14, largeFixtureSeedBlocks, 800);
    REQUIRE_FALSE(fixture.store->Checkpoint());
    REQUIRE(fixture.store->GetGeneration() == originalGeneration);
    REQUIRE(fixture.store->GetFileBytes() == originalBytes);
    REQUIRE(fixture.store->GetPendingBlockCount() == 800);
    fixture.store.reset();
    REQUIRE(HashClosedJournal(fixture.journalPath) == originalHash);
    fixture.Reopen(14 * largeFixtureSeedBlocks, 1);
}

TEST_CASE("Budget-bound resume failed pressure snapshot cannot continue and leaves original primary replayable", "[application][resume][coalesced][failure]")
{
    LargeJournalFixture fixture;
    fixture.Seed();
    REQUIRE(fixture.store->RecordCompletedSegment(fixture.Completed(0)));
    fixture.store.reset();
    const auto originalHash = HashClosedJournal(fixture.journalPath);
    fixture.Reopen(14 * largeFixtureSeedBlocks, 1);
    fixture.Accept(14, largeFixtureSeedBlocks, 640);
    const auto temporary = fixture.journalPath.wstring() + L".tmp";
    const HANDLE heldTemporary = CreateFileW(temporary.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(heldTemporary != INVALID_HANDLE_VALUE);
    const auto failed = fixture.store->Checkpoint();
    const auto retry = fixture.store->Checkpoint();
    const bool closed = CloseHandle(heldTemporary) != FALSE;
    REQUIRE(closed);
    REQUIRE_FALSE(failed);
    REQUIRE_FALSE(retry);
    REQUIRE(fixture.store->GetPendingBlockCount() == 640);
    fixture.store.reset();
    REQUIRE(HashClosedJournal(fixture.journalPath) == originalHash);
    fixture.Reopen(14 * largeFixtureSeedBlocks, 1);
}

TEST_CASE("Budget-bound resume completed flush failure does not report a live commit or permit continued appends", "[application][resume][coalesced][failure][flush]")
{
    LargeJournalFixture fixture;
    fixture.Seed();
    const auto originalGeneration = fixture.store->GetGeneration();
    IoState state;
    state.failFlushCall = 1;
    {
        ScopedIo observe(state);
        REQUIRE_FALSE(fixture.store->RecordCompletedSegment(fixture.Completed(0)));
    }
    REQUIRE(fixture.store->GetGeneration() == originalGeneration);
    REQUIRE(fixture.store->GetActiveBlockCount() == largeFixtureSegments * largeFixtureSeedBlocks);
    REQUIRE_FALSE(fixture.store->Checkpoint());
    REQUIRE_FALSE(fixture.store->RecordAcceptedBlock(fixture.Block(14, 1000)));
    // The injected error occurs after the native write-through record write.
    // Reopen must trust validated bytes, not the failed caller's old counters.
    fixture.Reopen(14 * largeFixtureSeedBlocks, 1);
}


#include <catch2/generators/catch_generators.hpp>

namespace
{
void OpenSmallBudgetedFixture(Fixture& fixture, const pbapp::DecoderMemoryBudget& budget)
{
    fixture.store.reset();
    fixture.loaded = {};
    REQUIRE(pbapp::ApplyDecoderMemoryBudget(budget, fixture.policy) == pbapp::DecoderMemoryBudgetError::None);
    REQUIRE(pbapp::DecoderResumeStore::Open(fixture.scratch.GetPath(), fixture.tag, fixture.sessionControl,
        fixture.policy, fixture.store, fixture.loaded, true, &budget));
    fixture.loaded = {};
}

std::vector<std::byte> SmallFixtureManifest(const Fixture& fixture)
{
    pbprotocol::FinalManifest manifest;
    manifest.sessionId = fixture.session.sessionId;
    manifest.originalFileSize = fixture.session.originalFileSize;
    manifest.segmentCount = fixture.session.segmentCount;
    manifest.digestAlgorithm = fixture.session.digestAlgorithm;
    manifest.wholeFileDigest = pbprotocol::WholeFileDigest{pbprotocol::ComputeBlake3Digest(MakeBytes(1048576))};
    std::vector<std::byte> bytes(pbprotocol::GetSerializedSize(manifest));
    REQUIRE(pbprotocol::SerializeFinalManifest(manifest, fixture.session, fixture.policy, bytes));
    return WrapControl(pbprotocol::ControlRecordType::FinalManifest, 3, fixture.tag, bytes);
}

std::vector<std::byte> CompletedJournalRecord(const pbprotocol::ResumeCompletedSegmentRecord& completed, const std::uint64_t generation)
{
    std::vector<std::byte> record(108);
    pbprotocol::ByteWriter writer(record);
    const std::array<std::byte, 4> magic{std::byte{'P'}, std::byte{'B'}, std::byte{'J'}, std::byte{'R'}};
    REQUIRE(writer.WriteFixedBytes(magic));
    REQUIRE(writer.WriteUint16(1));
    REQUIRE(writer.WriteUint16(4));
    REQUIRE(writer.WriteUint32(108));
    REQUIRE(writer.WriteUint32(72));
    REQUIRE(writer.WriteUint64(generation));
    REQUIRE(writer.WriteUint64(0));
    REQUIRE(writer.WriteFixedBytes(completed.sessionId.bytes));
    REQUIRE(writer.WriteUint64(completed.segmentOrdinal));
    REQUIRE(writer.WriteUint64(completed.rawOffset));
    REQUIRE(writer.WriteUint64(completed.rawSize));
    REQUIRE(writer.WriteFixedBytes(completed.rawDigest.bytes));
    REQUIRE(writer.WriteUint32(pbprotocol::ComputeCrc32c(std::span(record).first(104))));
    REQUIRE(writer.Remaining() == 0);
    return record;
}
}

TEST_CASE("Budget-bound resume metadata pressure regenerates its generation after snapshot and preflights exhaustion", "[application][resume][coalesced][metadata][generation]")
{
    const bool exhaustGeneration = GENERATE(false, true);
    Fixture fixture;
    const pbapp::DecoderMemoryBudget budget{8 * 1048576ULL, 8 * 1048576ULL};
    OpenSmallBudgetedFixture(fixture, budget);
    const auto control = pbprotocol::ParseControlRecord(fixture.segmentControl);
    REQUIRE(control);
    const auto descriptor = pbprotocol::ParseSegmentDescriptor(control.Value().payload, fixture.session, fixture.policy);
    REQUIRE(descriptor);
    const auto& segment = descriptor.Value();
    const pbprotocol::ResumeCompletedSegmentRecord completed{fixture.session.sessionId, 0, segment.rawOffset, segment.rawSize, segment.rawDigest};
    REQUIRE(fixture.store->RecordCompletedSegment(completed));
    const auto generation = fixture.store->GetGeneration();
    fixture.store.reset();
    auto document = ReadBytes(fixture.journalPath);
    const auto duplicateCount = (fixture.policy.maxResumeBytes - document.size()) / 108;
    REQUIRE(duplicateCount > 0);
    // Valid idempotent records exercise an untrusted-but-valid journal that
    // can be opened by either version, without lowering production limits.
    for (std::uint64_t index = 0; index < duplicateCount; index++)
    {
        const auto recordGeneration = exhaustGeneration && index + 1 == duplicateCount ?
            (std::numeric_limits<std::uint64_t>::max)() - 1 : generation + index + 1;
        const auto record = CompletedJournalRecord(completed, recordGeneration);
        document.insert(document.end(), record.begin(), record.end());
    }
    WriteBytes(fixture.journalPath, document);
    const auto originalHash = HashClosedJournal(fixture.journalPath);
    OpenSmallBudgetedFixture(fixture, budget);
    const auto before = fixture.store->GetGeneration();
    const auto manifest = SmallFixtureManifest(fixture);
    REQUIRE(document.size() + 36 + manifest.size() > fixture.policy.maxResumeBytes);
    const auto status = fixture.store->RecordManifestControl(manifest);
    if (exhaustGeneration)
    {
        REQUIRE_FALSE(status);
        REQUIRE(fixture.store->GetGeneration() == before);
        REQUIRE(fixture.store->GetFileBytes() == document.size());
        fixture.store.reset();
        REQUIRE(HashClosedJournal(fixture.journalPath) == originalHash);
    }
    else
    {
        REQUIRE(status);
        REQUIRE(fixture.store->GetGeneration() == before + 3);
        REQUIRE(fixture.store->GetFileBytes() < 4096);
        fixture.store.reset();
        pbapp::DecoderResumeLoadedState loaded;
        REQUIRE(pbapp::DecoderResumeStore::Open(fixture.scratch.GetPath(), fixture.tag, fixture.sessionControl,
            fixture.policy, fixture.store, loaded, true, &budget));
        REQUIRE(loaded.manifestControlRecord == manifest);
        REQUIRE(loaded.completedSegments.size() == 1);
        REQUIRE(loaded.completedSegments[0] == completed);
        REQUIRE(loaded.activeBlocks.empty());
    }
}

TEST_CASE("Budget-bound resume metadata that cannot fit even after compaction fails without a rewritten prefix", "[application][resume][coalesced][metadata][quota]")
{
    const pbapp::DecoderMemoryBudget budget{8 * 1048576ULL, 8 * 1048576ULL};
    std::uint32_t selectedBlockBytes = 0;
    std::uint64_t headerBytes = 0;
    std::uint64_t metadataBytes = 0;
    {
        Fixture dimensions;
        headerBytes = dimensions.store->GetFileBytes();
        metadataBytes = 36 + SmallFixtureManifest(dimensions).size();
    }
    const auto quota = pbapp::CalculateDecoderResumeBudgetBytes(budget);
    for (std::uint32_t blockBytes = 1024; blockBytes <= 65535; blockBytes++)
    {
        if ((quota - headerBytes) % (56 + blockBytes) < metadataBytes)
        {
            selectedBlockBytes = blockBytes;
            break;
        }
    }
    REQUIRE(selectedBlockBytes != 0);
    Fixture fixture(selectedBlockBytes);
    OpenSmallBudgetedFixture(fixture, budget);
    REQUIRE(fixture.store->GetFileBytes() == headerBytes);
    const auto count = (quota - headerBytes) / (56 + selectedBlockBytes);
    fixture.Accept(static_cast<std::size_t>(count));
    REQUIRE(fixture.store->Checkpoint());
    const auto initialBytes = fixture.store->GetFileBytes();
    const auto initialGeneration = fixture.store->GetGeneration();
    fixture.store.reset();
    const auto originalHash = HashClosedJournal(fixture.journalPath);
    OpenSmallBudgetedFixture(fixture, budget);
    REQUIRE(initialBytes + metadataBytes > quota);
    REQUIRE_FALSE(fixture.store->RecordManifestControl(SmallFixtureManifest(fixture)));
    REQUIRE(fixture.store->GetGeneration() == initialGeneration);
    REQUIRE(fixture.store->GetFileBytes() == initialBytes);
    fixture.store.reset();
    REQUIRE(HashClosedJournal(fixture.journalPath) == originalHash);
}


// Journal-only fault windows. No payload channel or test selector in product code.
#include "decoder_resume_snapshot_test_hook.h"
#include <catch2/generators/catch_generators.hpp>

TEST_CASE("Budget-bound resume pressure snapshot I/O errors preserve the authoritative primary and terminal state", "[application][resume][snapshot-fault]")
{
    using pbapp::test::snapshot::Fault;
    const auto fault = GENERATE(Fault::WriteFailure, Fault::ShortWrite, Fault::FlushFailure, Fault::ReplaceFailure, Fault::ReopenFailure);
    CAPTURE(static_cast<int>(fault));
    LargeJournalFixture fixture;
    fixture.Seed();
    REQUIRE(fixture.store->RecordCompletedSegment(fixture.Completed(0)));
    fixture.store.reset();
    const auto originalHash = HashClosedJournal(fixture.journalPath);
    fixture.Reopen(14 * largeFixtureSeedBlocks, 1);
    const auto oldGeneration = fixture.store->GetGeneration();
    const auto oldBytes = fixture.store->GetFileBytes();
    constexpr std::uint32_t pendingCount = 640;
    fixture.Accept(14, largeFixtureSeedBlocks, pendingCount);
    REQUIRE(oldBytes + static_cast<std::uint64_t>(pendingCount) * (56 + largeFixtureBlockBytes) > fixture.policy.maxResumeBytes);
    pbapp::test::snapshot::State state;
    state.fault = fault;
    {
        pbapp::test::snapshot::ScopedState scope(state);
        const auto failed = fixture.store->Checkpoint();
        CAPTURE(failed.message);
        REQUIRE_FALSE(failed);
        REQUIRE(state.writes == 1);
        REQUIRE(state.flushes == (fault == Fault::WriteFailure || fault == Fault::ShortWrite ? 0 : 1));
        REQUIRE(state.replaces == (fault == Fault::ReplaceFailure || fault == Fault::ReopenFailure ? 1 : 0));
        REQUIRE(state.reopens == (fault == Fault::ReopenFailure ? 1 : 0));
        const auto originalState = state;
        REQUIRE(fixture.store->GetPendingBlockCount() == pendingCount);
        REQUIRE(fixture.store->GetGeneration() == oldGeneration);
        REQUIRE(fixture.store->GetFileBytes() == oldBytes);
        REQUIRE_FALSE(fixture.store->Checkpoint());
        REQUIRE_FALSE(fixture.store->RecordCompletedSegment(fixture.Completed(1)));
        REQUIRE_FALSE(fixture.store->RecordAcceptedBlock(fixture.Block(14, 1000)));
        REQUIRE(state.writes == originalState.writes);
        REQUIRE(state.flushes == originalState.flushes);
        REQUIRE(state.replaces == originalState.replaces);
        REQUIRE(state.reopens == originalState.reopens);
    }
    fixture.store.reset();
    REQUIRE_FALSE(std::filesystem::exists(fixture.journalPath.wstring() + L".tmp"));
    if (fault == Fault::ReopenFailure)
    {
        // Replacement succeeded; failed caller state must not override the new primary.
        REQUIRE(HashClosedJournal(fixture.journalPath) != originalHash);
        fixture.Reopen(14 * largeFixtureSeedBlocks + pendingCount, 1);
        REQUIRE(fixture.store->GetGeneration() > oldGeneration);
    }
    else
    {
        REQUIRE(HashClosedJournal(fixture.journalPath) == originalHash);
        fixture.Reopen(14 * largeFixtureSeedBlocks, 1);
        REQUIRE(fixture.store->GetGeneration() == oldGeneration);
    }
}

TEST_CASE("Budget-bound resume native crash child", "[.resume-snapshot-crash-child]")
{
    // This hidden case is selected explicitly by the create-only Python supervisor.
    wchar_t selection[64]{};
    const DWORD count = GetEnvironmentVariableW(L"PB_RESUME_SNAPSHOT_TEST_CRASH", selection, 64);
    REQUIRE(count > 0);
    REQUIRE(count < 64);
    const std::wstring point(selection, count);
    REQUIRE((point == L"completed" || point == L"temporary-flushed" || point == L"replaced"));
    LargeJournalFixture fixture;
    fixture.Seed();
    const auto beforeCompletedBytes = fixture.store->GetFileBytes();
    REQUIRE(fixture.store->RecordCompletedSegment(fixture.Completed(0)));
    REQUIRE(fixture.store->GetFileBytes() == beforeCompletedBytes + 108);
    REQUIRE(fixture.store->GetPendingBlockCount() == 0);
    fixture.store.reset();
    const auto originalHash = HashClosedJournal(fixture.journalPath);
    fixture.Reopen(14 * largeFixtureSeedBlocks, 1);
    {
        const auto markerPath = fixture.scratch.GetPath() / L"before-crash.json";
        REQUIRE_FALSE(std::filesystem::exists(markerPath));
        std::ofstream marker(markerPath, std::ios::binary);
        REQUIRE(marker);
        marker << "{\"completedDeferred\":true,\"preCrashActiveBlocks\":1344,\"preCrashCompletedSegments\":1,\"preCrashGeneration\":"
            << fixture.store->GetGeneration() << ",\"primaryBlake3\":\"";
        constexpr char digits[] = "0123456789abcdef";
        for (const auto value : originalHash)
        {
            const auto byte = std::to_integer<unsigned int>(value);
            marker << digits[byte >> 4] << digits[byte & 15];
        }
        marker << "\"}\n";
        marker.close();
        REQUIRE(marker);
    }
    if (point == L"completed")
    {
        pbapp::test::snapshot::CrashNow();
    }
    constexpr std::uint32_t pendingCount = 640;
    fixture.Accept(14, largeFixtureSeedBlocks, pendingCount);
    REQUIRE(fixture.store->GetFileBytes() + static_cast<std::uint64_t>(pendingCount) * (56 + largeFixtureBlockBytes) > fixture.policy.maxResumeBytes);
    pbapp::test::snapshot::State state;
    state.fault = point == L"temporary-flushed" ? pbapp::test::snapshot::Fault::CrashAfterTemporaryFlush : pbapp::test::snapshot::Fault::CrashAfterReplace;
    pbapp::test::snapshot::ScopedState scope(state);
    const auto status = fixture.store->Checkpoint();
    CAPTURE(status.message);
    FAIL("Expected native crash point was not reached");
}
