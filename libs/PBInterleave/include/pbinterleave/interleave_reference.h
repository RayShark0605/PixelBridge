#pragma once

#include "pbinterleave/tile_permutation.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace pbinterleave {

// ---------------------------------------------------------------------------
// PB-InterleaveReference-1: frozen Phase-0 CPU reference spatial interleave
// (design document 19.2).
//
// The reference permutation maps the 112,336 four-bit tiles of the
// PB-ReferenceRaster-1 data region (472 x 238 tiles, 56,168 payload bytes)
// to physical tile positions. It is a pure, allocation-free, deterministic
// bijection that is completely determined by the profile id and the frame
// sequence; no random seed is sent or stored, and the single-frame FEC
// codeword content is never changed (positions only).
//
// Frozen constants (this reference profile):
//   n    = 112,336 tiles (= 472 x 238)
//   M    = 65,537 (a multiplicative unit mod n; n = 2^6 x 17 x 103 and
//            gcd(M, n) = 1)
//   M^-1 = 108,673 (M * M^-1 = 1 mod n; verified by unit tests)
//   S    = 472 tiles (one data-grid tile row; gcd(S, n) = 472 so the
//            phase shift cycle is n / 472 = 238 >= 16)
//   phase = FrameSequence % 16
//
//   physical = (logical * M + phase * S) mod n
//   logical  = ((physical - phase * S) mod n) * M^-1 mod n
//
// Bijection proof sketch: multiplication by a unit in Z/nZ is a group
// permutation; translation is a permutation; a composition of permutations
// is a permutation. The 16 phase shifts are pairwise distinct because the
// translation cycle (238) is at least the phase count (16).
//
// All intermediate products are computed in unsigned 64-bit arithmetic:
// (n - 1) * M = 7,362,098,895 exceeds UINT32_MAX and is verified by unit
// tests to stay inside uint64_t.
//
// This profile is a Phase-0 reference candidate, not a certified profile:
// a certified InterleaveProfileId may define a different permutation.
// ---------------------------------------------------------------------------

// First 8 bytes (little-endian) of BLAKE3-256 over the UTF-8 string
// "PixelBridge/InterleaveProfile/ReferenceTileV1".
inline constexpr std::uint64_t kInterleaveProfileIdReferenceV1 =
    0xEA949F2D1F89B845ULL;
inline constexpr std::uint64_t kInterleavePhaseCount = 16;
// Frozen reference data region tile count (472 x 238 tiles of the
// PB-ReferenceRaster-1 data grid). Unit tests cross-check this value against
// pbmodulation::kReferenceDataTileCount.
inline constexpr std::uint64_t kInterleaveTileCount = 112336;
inline constexpr std::uint64_t kInterleaveMultiplier = 65537;
inline constexpr std::uint64_t kInterleaveMultiplierInverse = 108673;
inline constexpr std::uint64_t kInterleavePhaseStepTiles = 472;
// Four bits per tile: the region holds exactly tileCount / 2 bytes.
inline constexpr std::size_t kInterleaveRegionBytes =
    kInterleaveTileCount / 2; // 56,168
static_assert(kInterleaveTileCount % 2 == 0,
    "each region byte must carry exactly two four-bit tiles");
static_assert(kInterleaveRegionBytes == 56168,
    "the reference interleave region is frozen at 56,168 bytes");
static_assert(kInterleavePhaseCount <= kInterleaveTileCount,
    "the phase count must fit the translation cycle");

enum class InterleaveErrorCode : std::uint8_t
{
    None,
    // A region span does not hold exactly kInterleaveRegionBytes bytes.
    // detail carries the received size.
    InvalidInput,
    // Input and output spans overlap. The reference is defined only for
    // disjoint spans (mirrors the LDPC encoder overlap rejection).
    OverlappingSpans
};

struct InterleaveError
{
    InterleaveErrorCode code = InterleaveErrorCode::None;
    std::uint64_t detail = 0;

    bool operator==(const InterleaveError&) const = default;
};

class InterleaveStatus
{
public:
    [[nodiscard]] static InterleaveStatus Success() noexcept
    {
        return InterleaveStatus(InterleaveError{});
    }

    [[nodiscard]] static InterleaveStatus Failure(
        const InterleaveErrorCode code,
        const std::uint64_t detail = 0) noexcept
    {
        const InterleaveErrorCode failureCode =
            code == InterleaveErrorCode::None
                ? InterleaveErrorCode::InvalidInput
                : code;
        return InterleaveStatus(InterleaveError{failureCode, detail});
    }

    [[nodiscard]] bool HasValue() const noexcept
    {
        return error_.code == InterleaveErrorCode::None;
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return HasValue();
    }

    [[nodiscard]] const InterleaveError& Error() const noexcept
    {
        return error_;
    }

private:
    explicit InterleaveStatus(const InterleaveError error) noexcept
        : error_(error)
    {
    }

    InterleaveError error_;
};

// frameSequence % kInterleavePhaseCount.
[[nodiscard]] constexpr std::uint64_t GetInterleavePhase(
    const std::uint64_t frameSequence) noexcept
{
    return frameSequence % kInterleavePhaseCount;
}

// physical = (logical * M + phase * S) mod n. logicalTile must be < n.
[[nodiscard]] constexpr std::uint64_t MapLogicalTileToPhysical(
    const std::uint64_t logicalTile,
    const std::uint64_t frameSequence) noexcept
{
    const std::uint64_t shift =
        (GetInterleavePhase(frameSequence) * kInterleavePhaseStepTiles) %
        kInterleaveTileCount;
    return detail::ApplyAffineTile(logicalTile, kInterleaveTileCount, kInterleaveMultiplier, shift);
}

// logical = ((physical - phase * S) mod n) * M^-1 mod n. physicalTile must
// be < n.
[[nodiscard]] constexpr std::uint64_t MapPhysicalTileToLogical(
    const std::uint64_t physicalTile,
    const std::uint64_t frameSequence) noexcept
{
    const std::uint64_t shift =
        (GetInterleavePhase(frameSequence) * kInterleavePhaseStepTiles) %
        kInterleaveTileCount;
    const std::uint64_t unshifted =
        (physicalTile + kInterleaveTileCount - shift) % kInterleaveTileCount;
    return detail::ApplyAffineTile(unshifted, kInterleaveTileCount, kInterleaveMultiplierInverse, 0);
}

// Moves every four-bit tile from logical to its physical position for
// frameSequence. Both spans must hold exactly kInterleaveRegionBytes bytes
// and must not overlap. Nibble order matches the reference raster lane
// packing: even tile index -> low nibble, odd tile index -> high nibble of
// the same byte. On failure nothing is written to physical.
[[nodiscard]] InterleaveStatus ApplyInterleave(
    std::span<const std::byte> logical,
    std::span<std::byte> physical,
    const std::uint64_t frameSequence) noexcept;

// The exact inverse of ApplyInterleave for the same frameSequence.
[[nodiscard]] InterleaveStatus ReverseInterleave(
    std::span<const std::byte> physical,
    std::span<std::byte> logical,
    const std::uint64_t frameSequence) noexcept;

} // namespace pbinterleave
