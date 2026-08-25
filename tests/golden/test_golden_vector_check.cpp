// PBGoldenVectorCheck negative-sample suite.
//
// Drives the check core directly (no file round-trip for most cases) so
// each failure class and the exact FAIL line format is pinned:
//   FileDigestMismatch: on-disk bytes differ from the registry pin
//   (committed-data corruption) and is reported as a DISTINCT class from
//   VectorMismatch (implementation recompute drift).
// Corrupted copies are generated in the build directory only; the committed
// golden files are never modified.

#include "golden_vector_check_core.h"

#include "pbgolden/golden_vector_registry.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <span>
#include <string>
#include <vector>

namespace {

constexpr const char* kGoldenRoot = PB_GOLDEN_ROOT;

[[nodiscard]] std::vector<std::byte> ReadGoldenFile(
    const pbgolden::GoldenVectorDescriptor& descriptor)
{
    const std::filesystem::path file =
        std::filesystem::path(kGoldenRoot) /
        std::string(descriptor.category) /
        (std::string(descriptor.name) + ".bin");
    std::ifstream stream(file, std::ios::binary);
    REQUIRE(static_cast<bool>(stream));
    stream.seekg(0, std::ios::end);
    const std::streamoff size = stream.tellg();
    REQUIRE(size >= 0);
    stream.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty())
    {
        stream.read(reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }
    REQUIRE(static_cast<bool>(stream));
    return bytes;
}

[[nodiscard]] const pbgolden::GoldenVectorDescriptor* RequireVector(
    const char* name)
{
    const auto* descriptor = pbgolden::FindGoldenVector(name);
    REQUIRE(descriptor != nullptr);
    return descriptor;
}

// Flips bit `bitInByte` of the byte at `offset` (in-memory copy).
void FlipBit(std::vector<std::byte>& bytes, std::size_t offset,
    std::uint8_t bitInByte)
{
    bytes[offset] = static_cast<std::byte>(
        std::to_integer<std::uint8_t>(bytes[offset]) ^
        static_cast<std::uint8_t>(1U << bitInByte));
}

// Corrupted copies live in the build directory only (never in the repo):
// PB_TEST_SCRATCH_ROOT is CMAKE_CURRENT_BINARY_DIR.
std::filesystem::path BuildCorruptRoot(const std::string& name)
{
    const std::filesystem::path root =
        std::filesystem::path(PB_TEST_SCRATCH_ROOT) / name;
    std::error_code errorCode;
    std::filesystem::remove_all(root, errorCode);
    std::filesystem::create_directories(root, errorCode);
    return root;
}

[[nodiscard]] std::string Hex2(const std::uint8_t value)
{
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string result;
    result.push_back(kHexDigits[(value >> 4) & 0xFu]);
    result.push_back(kHexDigits[value & 0xFu]);
    return result;
}

} // namespace

TEST_CASE("Golden check core: file digest gate distinguishes corruption "
    "classes", "[golden][core]")
{
    const auto* descriptor = RequireVector("bootstrap-record");
    const auto fileBytes = ReadGoldenFile(*descriptor);
    REQUIRE(fileBytes.size() == 44);

    SECTION("pristine file passes gate 1")
    {
        const auto report = pbgoldenchk::CheckFileDigest(
            *descriptor, std::span<const std::byte>(fileBytes));
        REQUIRE(report.kind == pbgoldenchk::CheckKind::Pass);
        const auto line = pbgoldenchk::FormatReportLine(report);
        REQUIRE(line.find("[GOLDEN] PASS vector=bootstrap-record") == 0);
        REQUIRE(line.find(" size=44 blake3=") != std::string::npos);
    }

    SECTION("single flipped byte is a FileDigestMismatch, not a "
        "VectorMismatch")
    {
        auto corrupted = fileBytes;
        FlipBit(corrupted, 7, 3);
        const auto report = pbgoldenchk::CheckFileDigest(
            *descriptor, std::span<const std::byte>(corrupted));
        REQUIRE(report.kind == pbgoldenchk::CheckKind::FileDigestMismatch);
        REQUIRE(report.sizeFile == 44);
        const auto line = pbgoldenchk::FormatReportLine(report);
        REQUIRE(line.find(
            "[GOLDEN] FAIL vector=bootstrap-record category=protocol "
            "kind=FileDigestMismatch size=44") == 0);
        REQUIRE(line.find("file=blake3:") != std::string::npos);
        REQUIRE(line.find("pinned=blake3:") != std::string::npos);
    }

    SECTION("empty file is a FileDigestMismatch")
    {
        const auto report = pbgoldenchk::CheckFileDigest(
            *descriptor, std::span<const std::byte>{});
        REQUIRE(report.kind == pbgoldenchk::CheckKind::FileDigestMismatch);
        REQUIRE(report.sizeFile == 0);
    }
}

TEST_CASE("Golden check core: byte comparison reports exact offset and "
    "values", "[golden][core]")
{
    const auto* descriptor = RequireVector("bootstrap-record");
    const auto fileBytes = ReadGoldenFile(*descriptor);

    SECTION("identical bytes pass")
    {
        const auto report = pbgoldenchk::CompareVectorBytes(
            *descriptor,
            std::span<const std::byte>(fileBytes),
            std::span<const std::byte>(fileBytes));
        REQUIRE(report.kind == pbgoldenchk::CheckKind::Pass);
    }

    SECTION("single flipped byte: exact offset/expected/actual/count")
    {
        auto recomputed = fileBytes;
        FlipBit(recomputed, 31, 0);
        const auto expectedValue =
            std::to_integer<std::uint8_t>(fileBytes[31]);
        const auto actualValue = static_cast<std::uint8_t>(expectedValue ^ 0x01U);
        const auto report = pbgoldenchk::CompareVectorBytes(
            *descriptor,
            std::span<const std::byte>(fileBytes),
            std::span<const std::byte>(recomputed));
        REQUIRE(report.kind == pbgoldenchk::CheckKind::VectorMismatch);
        REQUIRE(report.firstDifferingOffset == 31);
        REQUIRE(report.differingBytes == 1);
        REQUIRE(report.comparedBytes == 44);
        const auto line = pbgoldenchk::FormatReportLine(report);
        REQUIRE(line.find(
            "[GOLDEN] FAIL vector=bootstrap-record category=protocol "
            "kind=VectorMismatch size_file=44 size_recomputed=44 "
            "byte_offset=0x1f (31) expected=0x" +
            Hex2(expectedValue) + " actual=0x" + Hex2(actualValue) +
            " (1/44 compared bytes differ)") == 0);
    }

    SECTION("two differing bytes: differing count is 2")
    {
        auto recomputed = fileBytes;
        FlipBit(recomputed, 10, 1);
        FlipBit(recomputed, 40, 4);
        const auto report = pbgoldenchk::CompareVectorBytes(
            *descriptor,
            std::span<const std::byte>(fileBytes),
            std::span<const std::byte>(recomputed));
        REQUIRE(report.kind == pbgoldenchk::CheckKind::VectorMismatch);
        REQUIRE(report.firstDifferingOffset == 10);
        REQUIRE(report.differingBytes == 2);
        const auto line = pbgoldenchk::FormatReportLine(report);
        REQUIRE(line.find("byte_offset=0xa (10) ") != std::string::npos);
        REQUIRE(line.find(" (2/44 compared bytes differ)") != std::string::npos);
    }

    SECTION("size mismatch: reported via size fields")
    {
        auto recomputed = fileBytes;
        recomputed.pop_back();
        const auto report = pbgoldenchk::CompareVectorBytes(
            *descriptor,
            std::span<const std::byte>(fileBytes),
            std::span<const std::byte>(recomputed));
        REQUIRE(report.kind == pbgoldenchk::CheckKind::VectorMismatch);
        REQUIRE(report.sizeFile == 44);
        REQUIRE(report.sizeRecomputed == 43);
        REQUIRE(report.comparedBytes == 43);
        REQUIRE(report.firstDifferingOffset == 43);
        REQUIRE(report.actualEof);
        const auto line = pbgoldenchk::FormatReportLine(report);
        REQUIRE(line.find(
            "kind=VectorMismatch size_file=44 size_recomputed=43") !=
            std::string::npos);
        REQUIRE(line.find("byte_offset=0x2b (43)") != std::string::npos);
        REQUIRE(line.find("actual=<eof>") != std::string::npos);
    }

    SECTION("first byte difference: offset 0 hex format")
    {
        auto recomputed = fileBytes;
        FlipBit(recomputed, 0, 7);
        const auto report = pbgoldenchk::CompareVectorBytes(
            *descriptor,
            std::span<const std::byte>(fileBytes),
            std::span<const std::byte>(recomputed));
        REQUIRE(report.kind == pbgoldenchk::CheckKind::VectorMismatch);
        REQUIRE(report.firstDifferingOffset == 0);
        const auto line = pbgoldenchk::FormatReportLine(report);
        REQUIRE(line.find("byte_offset=0x0 (0) ") != std::string::npos);
    }
}

TEST_CASE("Golden check core: large vector middle corruption",
    "[golden][core]")
{
    const auto* descriptor = RequireVector("control-maximum");
    const auto fileBytes = ReadGoldenFile(*descriptor);
    REQUIRE(fileBytes.size() == 65536);

    SECTION("flip at offset 32767: large-offset hex format")
    {
        auto recomputed = fileBytes;
        FlipBit(recomputed, 32767, 2);
        const auto report = pbgoldenchk::CompareVectorBytes(
            *descriptor,
            std::span<const std::byte>(fileBytes),
            std::span<const std::byte>(recomputed));
        REQUIRE(report.kind == pbgoldenchk::CheckKind::VectorMismatch);
        REQUIRE(report.firstDifferingOffset == 32767);
        const auto line = pbgoldenchk::FormatReportLine(report);
        REQUIRE(line.find("byte_offset=0x7fff (32767) ") != std::string::npos);
        REQUIRE(line.find(" (1/65536 compared bytes differ)") != std::string::npos);
    }

    SECTION("flip at the final byte: offset 65535")
    {
        auto recomputed = fileBytes;
        FlipBit(recomputed, 65535, 0);
        const auto report = pbgoldenchk::CompareVectorBytes(
            *descriptor,
            std::span<const std::byte>(fileBytes),
            std::span<const std::byte>(recomputed));
        REQUIRE(report.firstDifferingOffset == 65535);
        const auto line = pbgoldenchk::FormatReportLine(report);
        REQUIRE(line.find("byte_offset=0xffff (65535) ") != std::string::npos);
    }

    SECTION("truncated maximum vector: size mismatch branch")
    {
        std::vector<std::byte> truncated(
            fileBytes.begin(), fileBytes.end() - 1);
        const auto report = pbgoldenchk::CompareVectorBytes(
            *descriptor,
            std::span<const std::byte>(fileBytes),
            std::span<const std::byte>(truncated));
        REQUIRE(report.kind == pbgoldenchk::CheckKind::VectorMismatch);
        REQUIRE(report.sizeRecomputed == 65535);
    }
}

TEST_CASE("Golden check core: end-to-end file check", "[golden][core][e2e]")
{
    SECTION("pristine committed file passes end to end")
    {
        const auto* descriptor = RequireVector("bootstrap-record");
        const auto report = pbgoldenchk::CheckVectorFile(
            std::filesystem::path(kGoldenRoot), *descriptor);
        REQUIRE(report.kind == pbgoldenchk::CheckKind::Pass);
    }

    SECTION("corrupted committed copy: FileDigestMismatch end to end")
    {
        const auto* descriptor = RequireVector("bootstrap-record");
        const auto fileBytes = ReadGoldenFile(*descriptor);
        auto corrupted = fileBytes;
        FlipBit(corrupted, 20, 5);
        const auto root = BuildCorruptRoot("digest-mismatch");
        const std::filesystem::path target =
            root / "protocol" / "bootstrap-record.bin";
        std::filesystem::create_directories(target.parent_path());
        {
            std::ofstream stream(target, std::ios::binary | std::ios::trunc);
            REQUIRE(static_cast<bool>(stream));
            stream.write(
                reinterpret_cast<const char*>(corrupted.data()),
                static_cast<std::streamsize>(corrupted.size()));
        }
        const auto report =
            pbgoldenchk::CheckVectorFile(root, *descriptor);
        REQUIRE(report.kind == pbgoldenchk::CheckKind::FileDigestMismatch);
        const auto line = pbgoldenchk::FormatReportLine(report);
        REQUIRE(line.find("kind=FileDigestMismatch") != std::string::npos);
    }

    SECTION("missing file: FileUnreadable with path detail")
    {
        const auto* descriptor = RequireVector("bootstrap-record");
        const auto report = pbgoldenchk::CheckVectorFile(
            std::filesystem::path(kGoldenRoot) / "does-not-exist",
            *descriptor);
        REQUIRE(report.kind == pbgoldenchk::CheckKind::FileUnreadable);
        REQUIRE(report.detail.find("does-not-exist") != std::string::npos);
        const auto line = pbgoldenchk::FormatReportLine(report);
        REQUIRE(line.find(
            "[GOLDEN] FAIL vector=bootstrap-record category=protocol "
            "kind=FileUnreadable detail=") == 0);
    }
}

TEST_CASE("Golden check core converts an invalid frame id into recompute failure",
    "[golden][core][error-path]")
{
    pbgolden::FrameVectorPin pin;
    pin.name = "invalid-frame-id";
    pin.id = static_cast<pbgolden::FrameVectorId>(255);
    const auto report = pbgoldenchk::CheckFrameVector(pin);
    REQUIRE(report.kind == pbgoldenchk::CheckKind::RecomputeFailed);
    REQUIRE(report.detail.find("unknown frame vector id") != std::string::npos);
}

TEST_CASE("Golden check core: remaining report line formats",
    "[golden][core]")
{
    SECTION("RecomputeFailed line carries the detail")
    {
        pbgoldenchk::CheckReport report;
        report.subject = "g1-transport-decoded";
        report.category = "raster";
        report.kind = pbgoldenchk::CheckKind::RecomputeFailed;
        report.detail = "the implementation recompute produced no bytes";
        const auto line = pbgoldenchk::FormatReportLine(report);
        REQUIRE(line ==
            "[GOLDEN] FAIL vector=g1-transport-decoded category=raster "
            "kind=RecomputeFailed detail=the implementation recompute "
            "produced no bytes");
    }

    SECTION("FrameDigestMismatch line names the channel")
    {
        pbgoldenchk::CheckReport report;
        report.subject = "g1-transport";
        report.category = "frame";
        report.kind = pbgoldenchk::CheckKind::FrameDigestMismatch;
        report.detail = "channel=png";
        const auto line = pbgoldenchk::FormatReportLine(report);
        REQUIRE(line.find(
            "[GOLDEN] FAIL frame=g1-transport kind=FrameDigestMismatch "
            "channel=png byte_offset=not-applicable expected=blake3:") == 0);
        REQUIRE(line.find(" actual=blake3:") != std::string::npos);
    }

    SECTION("FrameRasterMismatch line carries the oracle byte coordinate")
    {
        pbgoldenchk::CheckReport report;
        report.subject = "g1-transport";
        report.category = "frame";
        report.kind = pbgoldenchk::CheckKind::FrameRasterMismatch;
        report.detail = "space=decoded-png-bgra";
        report.sizeFile = 8294400;
        report.sizeRecomputed = 8294400;
        report.hasByteDifference = true;
        report.firstDifferingOffset = 1234;
        report.expectedByte = std::byte{0x18};
        report.actualByte = std::byte{0x28};
        report.differingBytes = 1;
        report.comparedBytes = 8294400;
        const auto line = pbgoldenchk::FormatReportLine(report);
        REQUIRE(line ==
            "[GOLDEN] FAIL frame=g1-transport kind=FrameRasterMismatch "
            "space=decoded-png-bgra size_expected=8294400 size_actual=8294400 "
            "byte_offset=0x4d2 (1234) expected=0x18 actual=0x28 "
            "(1/8294400 compared bytes differ)");
    }

    SECTION("manifest mismatch line")
    {
        pbgoldenchk::CheckReport report;
        report.subject = "manifest";
        report.category = "frame";
        report.kind = pbgoldenchk::CheckKind::VectorMismatch;
        const auto line = pbgoldenchk::FormatReportLine(report);
        REQUIRE(line.find(
            "[GOLDEN] FAIL manifest kind=VectorMismatch "
            "computed=blake3:") == 0);
    }

    SECTION("frame pass line carries both digests")
    {
        pbgoldenchk::CheckReport report;
        report.subject = "g0-zero";
        report.category = "frame";
        report.kind = pbgoldenchk::CheckKind::Pass;
        const auto line = pbgoldenchk::FormatReportLine(report);
        REQUIRE(line.find(
            "[GOLDEN] PASS frame=g0-zero raw=blake3:") == 0);
        REQUIRE(line.find(" png=blake3:") != std::string::npos);
    }
}

TEST_CASE("Golden check core: full-run determinism and summary",
    "[golden][core][determinism]")
{
    SECTION("two full runs produce identical stdout and zero failures")
    {
        std::ostringstream first;
        std::ostringstream second;
        const int firstFailures = pbgoldenchk::RunFullCheck(
            std::filesystem::path(kGoldenRoot), first);
        const int secondFailures = pbgoldenchk::RunFullCheck(
            std::filesystem::path(kGoldenRoot), second);
        REQUIRE(firstFailures == 0);
        REQUIRE(secondFailures == 0);
        REQUIRE(first.str() == second.str());
        const auto lines = first.str();
        REQUIRE(lines.find("[GOLDEN] total=30 failures=0") !=
            std::string::npos);
        const std::size_t passCount =
            lines.find("[GOLDEN] PASS") == std::string::npos
                ? 0
                : [&lines]() {
                    std::size_t count = 0;
                    std::size_t searchStart = 0;
                    while (true)
                    {
                        const std::size_t found =
                            lines.find("[GOLDEN] PASS", searchStart);
                        if (found == std::string::npos)
                        {
                            break;
                        }
                        count++;
                        searchStart = found + 1;
                    }
                    return count;
                }();
        REQUIRE(passCount == 30);
    }
}
