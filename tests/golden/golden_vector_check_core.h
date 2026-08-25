#pragma once
// PBGoldenVectorCheck core: deterministic golden vector verification.
//
// Every committed golden file (tests/golden/<category>/<name>.bin) is
// checked against the on-disk bytes, a recomputation, and the frozen pin.
// The three-way result defines the failure class without promoting either
// unpinned byte stream to authoritative expected data:
//
//   1. FileDigestMismatch: the on-disk file's BLAKE3-256 differs from the
//      pinned registry digest while recomputation still matches the pin.
//   2. RecomputeFailed: the current implementation could not recompute the
//      canonical bytes at all (internal decode/encode chain failure).
//   3. VectorMismatch: the file matches the registry pin, but the current
//      implementation recomputes different bytes (implementation drift /
//      regression). The report carries the first differing byte offset
//      (hex + decimal), the expected (file) and actual (recomputed) byte
//      values, and the total differing-byte count.
//
// Full-frame vectors (G0/G1/G1-Transport/G1-Transport-2CW/G2) use raw PBRW
// and PNG digest pins; the 8.3 MB frames themselves are not committed. The
// 275-byte PBVM manifest is an ordinary file-backed registry vector.
//
// The core is allocation-bounded by design: it loads at most one golden
// file (max 65,571 bytes) plus the recomputed bytes at a time, and the
// frame check paths use a fixed, bounded number of 8.3 MB canvases for the
// independent oracle, production raster, and decoded PNG comparison. All
// output is deterministic (no timestamps, stable registry ordering).

#include "pbgolden/golden_vector_registry.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <ostream>
#include <span>
#include <string>

namespace pbgoldenchk {

enum class CheckKind
{
    Pass,
    // The golden file is missing or unreadable.
    FileUnreadable,
    // The file bytes differ from the pinned registry digest.
    FileDigestMismatch,
    // Neither committed bytes nor current recompute match the pin. No
    // unpinned byte stream is labeled authoritative.
    FileAndVectorMismatch,
    // The implementation recompute failed (no bytes produced).
    RecomputeFailed,
    // The file matches the registry but the recompute differs.
    VectorMismatch,
    // Production raw/decoded-PNG raster bytes differ from the independent
    // literal-geometry oracle. This reports a byte offset and byte values.
    FrameRasterMismatch,
    // A frame raw/PNG digest differs from its pin.
    FrameDigestMismatch
};

[[nodiscard]] const char* ToString(const CheckKind kind) noexcept;

struct CheckReport
{
    // Vector name, frame name, or "manifest".
    std::string subject;
    // Category label, or "frame" for frame vectors.
    std::string category;
    CheckKind kind = CheckKind::Pass;
    // Gate 1 artifacts (file vs registry pin).
    std::array<std::byte, 32> fileDigest{};
    // Gate 3 artifacts (recomputed vs file).
    std::array<std::byte, 32> recomputedDigest{};
    std::array<std::byte, 32> pinnedDigest{};
    std::size_t sizeFile = 0;
    std::size_t sizeRecomputed = 0;
    std::size_t firstDifferingOffset = 0;
    std::size_t differingBytes = 0;
    std::size_t comparedBytes = 0;
    std::byte expectedByte{};
    std::byte actualByte{};
    bool hasByteDifference = false;
    bool expectedEof = false;
    bool actualEof = false;
    // RecomputeFailed detail or FileUnreadable diagnostic.
    std::string detail;
};

// Lowercase hex of a 32-byte digest (64 chars).
[[nodiscard]] std::string ToHexLower(
    const std::array<std::byte, 32>& digest);

// Gate 1: file bytes vs the pinned registry digest. Returns Pass (with
// fileDigest set) or FileDigestMismatch.
CheckReport CheckFileDigest(
    const pbgolden::GoldenVectorDescriptor& descriptor,
    const std::span<const std::byte> fileBytes);

// Gate 3: file bytes vs recomputed bytes (both must already exist).
// Returns Pass or VectorMismatch with offset/expected/actual/differing
// count. A size difference is a VectorMismatch reported via sizeFile /
// sizeRecomputed (comparedBytes == 0).
CheckReport CompareVectorBytes(
    const pbgolden::GoldenVectorDescriptor& descriptor,
    const std::span<const std::byte> fileBytes,
    const std::span<const std::byte> recomputedBytes);

// Full check of one committed vector file:
//   <goldenRoot>/<descriptor.category>/<descriptor.name>.bin
// Missing/unreadable file -> FileUnreadable (no digest gate is run).
CheckReport CheckVectorFile(
    const std::filesystem::path& goldenRoot,
    const pbgolden::GoldenVectorDescriptor& descriptor);

// Full-frame check for one pin: compares production PBRW bytes and decoded
// PNG pixels to the independent literal-geometry oracle before comparing the
// raw + PNG stream digests against the pins.
CheckReport CheckFrameVector(const pbgolden::FrameVectorPin& pin);

// Direct manifest pin helper retained for unit-level cross-checks. The full
// harness verifies the PBVM bytes through the ordinary file-backed registry.
CheckReport CheckManifestVector();

// Deterministic one-line report. The exact format is pinned by the
// negative-sample tests:
//   [GOLDEN] PASS vector=<name> category=<cat> size=<n> blake3=<hex>
//   [GOLDEN] FAIL vector=<name> category=<cat> kind=<Kind> ...
//   [GOLDEN] PASS frame=<name> raw=blake3:<hex> png=blake3:<hex>
//   [GOLDEN] FAIL frame=<name> kind=FrameDigestMismatch channel=<c> ...
//   [GOLDEN] PASS manifest blake3=<hex>
//   [GOLDEN] FAIL manifest kind=VectorMismatch computed=blake3:<hex> ...
// Not noexcept: builds strings (allocation).
[[nodiscard]] std::string FormatReportLine(const CheckReport& report);

// Runs the entire file-backed registry (25 vectors, including the PBVM
// manifest) + frame registry (5 frames) in stable order, printing one line
// per subject to `out`.
// Returns the failure count (0 == all match).
[[nodiscard]] int RunFullCheck(
    const std::filesystem::path& goldenRoot, std::ostream& out);

} // namespace pbgoldenchk
