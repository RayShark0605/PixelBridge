#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pbgolden {

// ---------------------------------------------------------------------------
// Golden vector registry (frozen digests).
//
// Each file-backed vector pins the BLAKE3-256 of its canonical bytes. The
// harness compares the file, current recomputation, and pin as three inputs:
// FileDigestMismatch means only the file differs, VectorMismatch means only
// recomputation differs, and FileAndVectorMismatch means neither unpinned
// stream matches the pin.
// ---------------------------------------------------------------------------

struct GoldenVectorDescriptor
{
    // Registry name (== <name>.bin under tests/golden/<category>).
    std::string_view name;
    // Category directory under tests/golden/ (coverage category label).
    std::string_view category;
    std::size_t sizeBytes = 0;
    // Pinned BLAKE3-256 of the canonical bytes.
    std::array<std::byte, 32> blake3{};
};

// All committed file-backed vectors, in stable registry order.
[[nodiscard]] const std::vector<GoldenVectorDescriptor>&
    GetGoldenVectorRegistry();

// nullptr when the name is unknown.
[[nodiscard]] const GoldenVectorDescriptor* FindGoldenVector(
    const std::string_view name);

enum class RecomputeStatus
{
    Ok,
    UnknownVector,
    // The implementation recompute failed (never a silent substitution).
    RecomputeFailed
};

struct RecomputeOutcome
{
    RecomputeStatus status = RecomputeStatus::UnknownVector;
    // Error diagnostic when status == RecomputeFailed.
    std::string detail;
    // Canonical bytes when status == Ok.
    std::vector<std::byte> bytes;
};

// Recomputes the canonical bytes of a file-backed vector with the current
// implementation.
[[nodiscard]] RecomputeOutcome RecomputeGoldenVector(
    const std::string_view name);

// ---------------------------------------------------------------------------
// Full-frame digest pins. The 8.3 MB frames themselves are not committed
// (repository size convention); the raw/PNG BLAKE3 pairs are the pins.
// ---------------------------------------------------------------------------

enum class FrameVectorId
{
    G0Zero,
    G1Canonical,
    G1Transport,
    G1Transport2Cw,
    G2Max
};

struct FrameVectorPin
{
    std::string_view name;
    FrameVectorId id = FrameVectorId::G0Zero;
    std::array<std::byte, 32> rawBlake3{};
    std::array<std::byte, 32> pngBlake3{};
};

// Stable registry order: G0, G1, G1-Transport, G1-Transport-2CW, G2.
[[nodiscard]] const std::vector<FrameVectorPin>&
    GetFrameVectorRegistry();

// Pinned BLAKE3-256 of the 275-byte canonical reference region manifest
// (identical to the existing pin in docs/REFERENCE_RASTER.md section 8.3).
[[nodiscard]] const std::array<std::byte, 32>&
    GetManifestDigestPin() noexcept;

} // namespace pbgolden
