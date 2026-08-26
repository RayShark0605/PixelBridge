#include "pbreceiver/resume_replay.h"

#include "pbprotocol/descriptor_codec.h"
#include "pbprotocol/protocol_types.h"
#include "pbprotocol/resume_state.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <span>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace {

[[nodiscard]] constexpr std::byte Byte(const std::uint8_t value) noexcept
{
    return static_cast<std::byte>(value);
}

// PB_TEST_SCRATCH_ROOT is the suite CMAKE_CURRENT_BINARY_DIR (see the golden
// test precedent): a per-case root keeps concurrent ctest runs from touching
// each other's files. The directory lives in the build tree, so removing it
// on success never touches source or user data.
[[nodiscard]] std::filesystem::path MakeScratchRoot(const char* const caseName)
{
    std::error_code errorCode;
    const std::filesystem::path scratchRoot =
        std::filesystem::path(PB_TEST_SCRATCH_ROOT) / caseName;
    (void)std::filesystem::remove_all(scratchRoot, errorCode);
    REQUIRE(std::filesystem::create_directories(scratchRoot));
    return scratchRoot;
}

[[nodiscard]] pbprotocol::ReceiverResourcePolicy MakeSmallBudgetPolicy(
    const std::uint64_t maxResumeBytes) noexcept
{
    pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();
    resourcePolicy.maxResumeBytes = maxResumeBytes;
    return resourcePolicy;
}

// Writes raw bytes through plain file IO so tests can pre-place content the
// resume-state API itself never produced (e.g. over-budget garbage).
void WriteRawFile(const std::filesystem::path& filePath, const std::vector<std::byte>& bytes)
{
    std::ofstream fileStream(filePath, std::ios::binary | std::ios::trunc);
    REQUIRE(fileStream.is_open());
    fileStream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(!fileStream.fail());
}

[[nodiscard]] bool IsProtocolErrorWith(
    const pbreceiver::ReceiverError& receiverError,
    const pbprotocol::ProtocolErrorCode code) noexcept
{
    const auto* protocolError = std::get_if<pbprotocol::ProtocolError>(&receiverError);
    return protocolError != nullptr && protocolError->code == code;
}

} // namespace

TEST_CASE("WriteResumeStateFile then ReadResumeStateFile round-trips binary bytes exactly",
          "[resume-state-file][roundtrip]")
{
    const std::filesystem::path scratchRoot = MakeScratchRoot("resume_state_file_roundtrip");
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();

    // (a) Empty document: zero-byte write, zero-byte read.
    const std::filesystem::path emptyPath = scratchRoot / "empty.state";
    {
        const auto writeResult = pbreceiver::WriteResumeStateFile(emptyPath, {});
        REQUIRE(writeResult);
        CHECK(writeResult.Value() == 0U);
        REQUIRE(std::filesystem::is_regular_file(emptyPath));
        CHECK(std::filesystem::file_size(emptyPath) == 0U);

        const auto readResult = pbreceiver::ReadResumeStateFile(
            emptyPath, resourcePolicy);
        REQUIRE(readResult);
        CHECK(readResult.Value().empty());
    }

    // (b) Mixed binary sequence containing NUL / LF / CR / 0xFF bytes: proves
    // the read/write path is truly binary (no text-mode translation).
    const std::filesystem::path mixedPath = scratchRoot / "mixed.state";
    {
        std::vector<std::byte> document;
        for (std::uint32_t roundIndex = 0U; roundIndex < 17U; roundIndex++)
        {
            document.push_back(Byte(0x00)); // NUL: text-mode corruption probe.
            document.push_back(Byte(0x0A)); // LF: CRLF translation probe (Windows).
            document.push_back(Byte(0x0D)); // CR pair with the preceding LF.
            document.push_back(Byte(0xFF)); // high-bit byte.
        }

        const auto writeResult = pbreceiver::WriteResumeStateFile(
            mixedPath, std::span<const std::byte>(document));
        REQUIRE(writeResult);
        CHECK(static_cast<std::uint64_t>(document.size()) == writeResult.Value());

        const auto readResult = pbreceiver::ReadResumeStateFile(
            mixedPath, resourcePolicy);
        REQUIRE(readResult);
        CHECK(readResult.Value() == document); // bit-exact, byte by byte.
    }

    // (c) Truncation semantics: re-writing a shorter document over a longer
    // one must leave exactly the new content on disk.
    const std::filesystem::path truncPath = scratchRoot / "truncated.state";
    {
        std::vector<std::byte> longDocument(32U);
        for (std::size_t byteIndex = 0; byteIndex < longDocument.size(); byteIndex++)
        {
            longDocument[byteIndex] = Byte(static_cast<std::uint8_t>(byteIndex + 1));
        }
        REQUIRE(pbreceiver::WriteResumeStateFile(
            truncPath, std::span<const std::byte>(longDocument)));

        const std::array<std::byte, 4> shortDocument{Byte(0xDE), Byte(0xAD),
                                                     Byte(0xBE), Byte(0xEF)};
        const auto rewriteResult = pbreceiver::WriteResumeStateFile(
            truncPath, std::span<const std::byte>(shortDocument));
        REQUIRE(rewriteResult);
        CHECK(rewriteResult.Value() == shortDocument.size());

        const auto readResult = pbreceiver::ReadResumeStateFile(
            truncPath, resourcePolicy);
        REQUIRE(readResult);
        CHECK(static_cast<std::uint64_t>(shortDocument.size()) ==
              static_cast<std::uint64_t>(readResult.Value().size()));
        CHECK(std::equal(readResult.Value().begin(), readResult.Value().end(),
                         shortDocument.begin(), shortDocument.end()));
    }

    std::error_code cleanupErrorCode;
    (void)std::filesystem::remove_all(scratchRoot, cleanupErrorCode);
}

TEST_CASE("ReadResumeStateFile rejects over-budget files on size alone",
          "[resume-state-file][budget]")
{
    const std::filesystem::path scratchRoot = MakeScratchRoot("resume_state_file_budget");

    // (a) Stat-first gate: a garbage file of size maxResumeBytes + 1 must be
    // rejected as ResourceLimitExceeded before any read/parse can happen, so
    // no parse-family code may ever surface here.
    {
        const std::uint64_t smallBudget = 64ULL;
        const pbprotocol::ReceiverResourcePolicy smallPolicy = MakeSmallBudgetPolicy(smallBudget);
        const std::filesystem::path overBudgetPath = scratchRoot / "over-budget.state";

        std::vector<std::byte> garbage(smallBudget + 1U);
        for (std::size_t byteIndex = 0; byteIndex < garbage.size(); byteIndex++)
        {
            garbage[byteIndex] = Byte(0xAB); // deliberately unparseable content.
        }
        WriteRawFile(overBudgetPath, garbage);

        const auto readResult = pbreceiver::ReadResumeStateFile(
            overBudgetPath, smallPolicy);
        REQUIRE_FALSE(readResult);
        CHECK(IsProtocolErrorWith(
            readResult.Error(), pbprotocol::ProtocolErrorCode::ResourceLimitExceeded));
    }

    // (b) Exact budget size is allowed: size == maxResumeBytes passes the gate.
    {
        const std::uint64_t smallBudget = 64ULL;
        const pbprotocol::ReceiverResourcePolicy smallPolicy = MakeSmallBudgetPolicy(smallBudget);
        const std::filesystem::path exactPath = scratchRoot / "exact-budget.state";

        std::vector<std::byte> exactContent(smallBudget, Byte(0x5A));
        WriteRawFile(exactPath, exactContent);

        const auto readResult = pbreceiver::ReadResumeStateFile(
            exactPath, smallPolicy);
        REQUIRE(readResult);
        CHECK(static_cast<std::uint64_t>(exactContent.size()) ==
              static_cast<std::uint64_t>(readResult.Value().size()));
    }

    // (c) Fully valid content still loses to the size gate: a builder-produced
    // document read under a smaller budget fails closed on size alone.
    {
        const pbprotocol::ReceiverResourcePolicy generousPolicy = MakeSmallBudgetPolicy(4096ULL);
        auto builderResult = pbprotocol::ResumeStateBuilder::Create(generousPolicy);
        REQUIRE(builderResult);
        pbprotocol::ResumeStateBuilder builder = std::move(builderResult).Value();

        const pbprotocol::SessionId sessionId{};
        pbprotocol::ResumeCompletedSegmentRecord completedRecord;
        completedRecord.sessionId = sessionId;
        completedRecord.segmentOrdinal = 0U;
        completedRecord.rawOffset = 0U;
        completedRecord.rawSize = 10U;
        REQUIRE(builder.AppendCompletedSegment(completedRecord));
        const std::vector<std::byte> documentBytes(builder.GetDocument().begin(), builder.GetDocument().end());
        CHECK(!documentBytes.empty());

        const std::filesystem::path validPath = scratchRoot / "valid-over-budget.state";
        REQUIRE(pbreceiver::WriteResumeStateFile(
            validPath, std::span<const std::byte>(documentBytes)));

        // A 90-byte budget cannot hold this record; the file must be rejected
        // even though every byte inside is structurally valid.
        const pbprotocol::ReceiverResourcePolicy tightPolicy = MakeSmallBudgetPolicy(90ULL);
        CHECK(documentBytes.size() > 90U);
        const auto readResult = pbreceiver::ReadResumeStateFile(validPath, tightPolicy);
        REQUIRE_FALSE(readResult);
        CHECK(IsProtocolErrorWith(
            readResult.Error(), pbprotocol::ProtocolErrorCode::ResourceLimitExceeded));
    }

    // (d) The policy gate runs before any file access: an all-zero policy is
    // invalid and must win over everything else.
    {
        const std::filesystem::path scratchFile = scratchRoot / "policy-gate.state";
        WriteRawFile(scratchFile, {Byte(0x01)});

        const pbprotocol::ReceiverResourcePolicy zeroedPolicy{};
        const auto readResult = pbreceiver::ReadResumeStateFile(scratchFile, zeroedPolicy);
        REQUIRE_FALSE(readResult);
        CHECK(IsProtocolErrorWith(
            readResult.Error(), pbprotocol::ProtocolErrorCode::InvalidResourcePolicy));
    }

    std::error_code cleanupErrorCode;
    (void)std::filesystem::remove_all(scratchRoot, cleanupErrorCode);
}

TEST_CASE("Resume state file IO failure paths fail clean without crashing",
          "[resume-state-file][errors]")
{
    const std::filesystem::path scratchRoot = MakeScratchRoot("resume_state_file_errors");
    const pbprotocol::ReceiverResourcePolicy resourcePolicy =
        pbprotocol::GetDefaultReceiverResourcePolicy();

    // (a) Missing path: clean ResumeStateIoFailure, no crash.
    {
        const auto readResult = pbreceiver::ReadResumeStateFile(
            scratchRoot / "does-not-exist.state", resourcePolicy);
        REQUIRE_FALSE(readResult);
        CHECK(IsProtocolErrorWith(
            readResult.Error(), pbprotocol::ProtocolErrorCode::ResumeStateIoFailure));
    }

    // (b) Directory as the read target: not a regular file -> IO failure.
    {
        const std::filesystem::path directory = scratchRoot / "a-directory";
        REQUIRE(std::filesystem::create_directories(directory));
        const auto readResult = pbreceiver::ReadResumeStateFile(
            directory, resourcePolicy);
        REQUIRE_FALSE(readResult);
        CHECK(IsProtocolErrorWith(
            readResult.Error(), pbprotocol::ProtocolErrorCode::ResumeStateIoFailure));
    }

    // (c) Directory as the write target: open must fail cleanly.
    {
        const std::filesystem::path directory = scratchRoot / "a-directory";
        const std::array<std::byte, 2> probeBytes{Byte(0x42), Byte(0x43)};
        const auto writeResult = pbreceiver::WriteResumeStateFile(
            directory, std::span<const std::byte>(probeBytes));
        REQUIRE_FALSE(writeResult);
        CHECK(IsProtocolErrorWith(
            writeResult.Error(), pbprotocol::ProtocolErrorCode::ResumeStateIoFailure));
    }

    // (d) Missing parent directory: the plain FastResume writer does not create
    // intermediate directories and must fail clean.
    {
        const std::array<std::byte, 1> probeBytes{Byte(0x42)};
        const auto writeResult = pbreceiver::WriteResumeStateFile(
            scratchRoot / "no-such-parent" / "resume.state",
            std::span<const std::byte>(probeBytes));
        REQUIRE_FALSE(writeResult);
        CHECK(IsProtocolErrorWith(
            writeResult.Error(), pbprotocol::ProtocolErrorCode::ResumeStateIoFailure));
    }

    std::error_code cleanupErrorCode;
    (void)std::filesystem::remove_all(scratchRoot, cleanupErrorCode);
}

TEST_CASE("WriteResumeStateFile success implies fully flushed on-disk content",
          "[resume-state-file][flush]")
{
    const std::filesystem::path scratchRoot = MakeScratchRoot("resume_state_file_flush");

    // (a) A document larger than typical stream buffers must be verifiably on
    // disk through plain IO before the API returns success.
    {
        std::vector<std::byte> document(1024U);
        for (std::size_t byteIndex = 0; byteIndex < document.size(); byteIndex++)
        {
            document[byteIndex] = Byte(static_cast<std::uint8_t>(byteIndex * 31U + 7U));
        }
        const std::filesystem::path largePath = scratchRoot / "large.state";
        REQUIRE(pbreceiver::WriteResumeStateFile(
            largePath, std::span<const std::byte>(document)));

        std::ifstream readBack(largePath, std::ios::binary);
        REQUIRE(readBack.is_open());
        std::vector<std::byte> observed(document.size());
        readBack.read(
            reinterpret_cast<char*>(observed.data()),
            static_cast<std::streamsize>(document.size()));
        REQUIRE(static_cast<std::uint64_t>(readBack.gcount()) == document.size());
        CHECK(observed == document);
    }

    // (b) A small document (single flush at close) must also be complete on
    // disk at the point the API reports success. ENOSPC-class close failures
    // cannot be forced deterministically in CI; the explicit flush/close
    // checks in the writer are the mitigation.
    {
        const std::array<std::byte, 16> smallDocument{
            Byte(0x01), Byte(0x02), Byte(0x03), Byte(0x04),
            Byte(0x05), Byte(0x06), Byte(0x07), Byte(0x08),
            Byte(0x09), Byte(0x0A), Byte(0x0B), Byte(0x0C),
            Byte(0x0D), Byte(0x0E), Byte(0x0F), Byte(0x10)};
        const std::filesystem::path smallPath = scratchRoot / "small.state";
        REQUIRE(pbreceiver::WriteResumeStateFile(
            smallPath, std::span<const std::byte>(smallDocument)));

        std::ifstream readBack(smallPath, std::ios::binary);
        REQUIRE(readBack.is_open());
        std::array<std::byte, 16> observed{};
        readBack.read(
            reinterpret_cast<char*>(observed.data()),
            static_cast<std::streamsize>(observed.size()));
        REQUIRE(readBack.gcount() == 16);
        CHECK(observed == smallDocument);
    }

    std::error_code cleanupErrorCode;
    (void)std::filesystem::remove_all(scratchRoot, cleanupErrorCode);
}
