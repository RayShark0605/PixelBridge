#pragma once

#include "pbinnerfec/inner_fec_result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace pbinnerfec {

// ---------------------------------------------------------------------------
// DVB-S2 Short Frame (N=16200) QC-LDPC inner FEC reference
// (PixelBridge design document section 14).
//
// Three frozen profiles. The identity is the tuple
// InnerFecProfileId + exact N/K + InnerFecMatrixId/MatrixDigest +
// SystematicBitOrder; rate labels ("2/3", "11/15", "37/45") are never parsed
// back into K. The matrices are the DVB-S2 Short FECFRAME parity-check
// matrices of ETSI EN 302 307-1 V1.4.1 (Table 5b). PixelBridge does not apply
// the DVB-S2 BCH outer code on top.
//
//   profile        K     N-K    actual LDPC rate
//   Robust        10800  5400   2/3
//   Balanced      11880  4320   11/15
//   Fast          13320  2880   37/45
// ---------------------------------------------------------------------------

using InnerFecProfileId = std::uint64_t;
using InnerFecMatrixId = std::uint64_t;

// Stable profile identifiers. Each constant is the first 8 bytes, read as
// little-endian, of BLAKE3-256 over the UTF-8 string
// "PixelBridge/InnerFecProfile/DVB-S2-Short-N16200-K<exact K>". The derivation
// keeps the constants auditable and non-arbitrary.
inline constexpr InnerFecProfileId kInnerFecProfileIdRobust =
    0x36BC661265E826C3ULL;
inline constexpr InnerFecProfileId kInnerFecProfileIdBalanced =
    0xF01CACD38B344350ULL;
inline constexpr InnerFecProfileId kInnerFecProfileIdFast =
    0x24B794EB5A445D58ULL;

// Stable matrix identifiers, derived the same way from
// "PixelBridge/InnerFecMatrix/DVB-S2-Short-N16200-K<exact K>".
inline constexpr InnerFecMatrixId kInnerFecMatrixIdRobust =
    0xB3F9EFAD6196DE85ULL;
inline constexpr InnerFecMatrixId kInnerFecMatrixIdBalanced =
    0x2275E3218AF6565DULL;
inline constexpr InnerFecMatrixId kInnerFecMatrixIdFast =
    0x0194708C6CBA030AULL;

// Frozen short-frame dimensions shared by all three profiles.
inline constexpr std::uint32_t kDvbS2ShortFrameNBits = 16200;
inline constexpr std::uint32_t kDvbS2ShortFrameCodewordByteCount =
    kDvbS2ShortFrameNBits / 8; // 2025
inline constexpr std::uint32_t kDvbS2ShortFrameMGroups = 360;

enum class SystematicBitOrder : std::uint8_t
{
    // Codeword = [K information bits][N-K parity bits] in the natural DVB-S2
    // Short index order. Bit i of the codeword maps to packed byte i/8,
    // bit position i%8 within that byte (LSB first). Information bit i belongs
    // to matrix line i/360, within-line index i%360. Parity bit j connects
    // check rows j and j+1 (staircase).
    DvbS2ShortNatural = 1
};

enum class PuncturingRule : std::uint8_t
{
    // The three supported K values are exact: no puncturing or shortening.
    // Any other K is rejected, not derived.
    None = 0
};

enum class ParityStructure : std::uint8_t
{
    // Check row j receives parity bit j (identity) and, for j >= 1, parity
    // bit j-1 (staircase). This structure makes the standard recursive
    // encoder possible: row contributions followed by a prefix-XOR chain.
    StaircaseDual = 0
};

struct InnerFecProfile
{
    InnerFecProfileId profileId = 0;
    std::uint32_t nBits = 0;
    std::uint32_t kBits = 0;
    std::uint32_t parityBits = 0;
    std::uint32_t mGroups = 0;
    std::uint32_t qShift = 0;
    std::uint32_t numLines = 0;
    InnerFecMatrixId matrixId = 0;
    // BLAKE3-256 of SerializeInnerFecMatrix output for this profile.
    std::array<std::byte, 32> matrixDigest{};
    std::uint8_t systematicBitOrder = 0;
    std::uint8_t puncturingRule = 0;
    std::uint8_t parityStructure = 0;

    [[nodiscard]] std::uint32_t GetInfoByteCount() const noexcept
    {
        return kBits / 8;
    }

    [[nodiscard]] std::uint32_t GetCodewordByteCount() const noexcept
    {
        return nBits / 8;
    }
};

// Registry over the three frozen profiles. Returns nullptr (fail-closed) for
// unknown ids. The returned pointer is stable for the process lifetime.
[[nodiscard]] const InnerFecProfile* GetInnerFecProfile(
    const InnerFecProfileId profileId) noexcept;
[[nodiscard]] const InnerFecProfile* GetInnerFecProfileByMatrixId(
    const InnerFecMatrixId matrixId) noexcept;

// Validates every frozen consistency relation of a profile, including that
// its matrixDigest still matches the BLAKE3-256 of the embedded matrix table
// serialization. Returns false on any mismatch (fail-closed).
[[nodiscard]] bool ValidateInnerFecProfile(const InnerFecProfile& profile) noexcept;

// Canonical matrix serialization (explicit little-endian, versioned):
//   [0..4)    "PBMX" magic
//   [4)       version = 1
//   [5)       systematicBitOrder
//   [6)       puncturingRule
//   [7)       parityStructure
//   [8..32)   nBits, kBits, parityBits, mGroups, qShift, numLines (u32 LE)
//   then per line: degree (u8) followed by degree shift values (u16 LE).
// Returns an empty span when no embedded table exists for profile.kBits or
// out is too small.
[[nodiscard]] std::span<const std::byte> SerializeInnerFecMatrixInto(
    const InnerFecProfile& profile,
    const std::span<std::byte> out) noexcept;

// Convenience wrapper around SerializeInnerFecMatrixInto (allocates).
[[nodiscard]] std::vector<std::byte> SerializeInnerFecMatrix(
    const InnerFecProfile& profile) noexcept;

// BLAKE3-256 of the canonical serialization. Empty when no embedded table
// exists for profile.kBits.
[[nodiscard]] std::array<std::byte, 32> ComputeInnerFecMatrixDigest(
    const InnerFecProfile& profile) noexcept;

// Maximum canonical serialization size across the frozen profiles
// (header 32 + 37 lines x (1 + 2 x max degree 16) bound).
inline constexpr std::size_t kMaxInnerFecMatrixSerializationBytes =
    32 + 37 * (1 + 2 * 16);

} // namespace pbinnerfec
