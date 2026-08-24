#pragma once
// Shared deterministic helpers for the PBInnerFec test suite.

#include "dvbs2_short_matrix.h"

#include "pbinnerfec/inner_fec_profile.h"
#include "pbinnerfec/qc_ldpc_codec.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/crc32c.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pbinnertectest {

// Deterministic PRNG (splitmix64) so every "random" pattern in the suite is
// exactly reproducible across runs and platforms.
class SplitMix64
{
public:
    explicit SplitMix64(std::uint64_t seed) noexcept
        : state_(seed)
    {
    }

    [[nodiscard]] std::uint64_t Next() noexcept
    {
        state_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

private:
    std::uint64_t state_;
};

[[nodiscard]] inline bool GetPackedBit(
    const std::span<const std::byte> bits,
    const std::uint32_t bitIndex) noexcept
{
    return (bits[bitIndex / 8] &
        std::byte{std::uint8_t{1u << (bitIndex % 8u)}}) != std::byte{0};
}

inline void SetPackedBit(
    const std::span<std::byte> bits,
    const std::uint32_t bitIndex,
    const bool value) noexcept
{
    const std::byte mask =
        std::byte{std::uint8_t{1u << (bitIndex % 8u)}};
    if (value)
    {
        bits[bitIndex / 8] |= mask;
    }
    else
    {
        bits[bitIndex / 8] &= ~mask;
    }
}

[[nodiscard]] inline std::string ToHex(
    const std::span<const std::byte> data)
{
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string result;
    result.reserve(data.size() * 2u);
    for (const std::byte value : data)
    {
        const auto byteValue = std::to_integer<std::uint8_t>(value);
        result.push_back(kHexDigits[(byteValue >> 4) & 0xFu]);
        result.push_back(kHexDigits[byteValue & 0xFu]);
    }
    return result;
}

// Deterministic pseudo-random information bytes (LSB-first packing is done
// byte-wise, so the bit pattern is fixed by the seed and byte count).
[[nodiscard]] inline std::vector<std::byte> MakeInfoBytes(
    const std::uint32_t byteCount,
    SplitMix64& random)
{
    std::vector<std::byte> info(byteCount);
    for (std::uint32_t byteIndex = 0; byteIndex < byteCount; byteIndex++)
    {
        info[byteIndex] =
            std::byte{std::uint8_t(random.Next() & 0xFFu)};
    }
    return info;
}
// Deterministic bit-wise pseudo-random information pattern: bit i is
// SplitMix64(seed).Next() & 1, packed LSB first. The first 128 bits are
// independent of the total bit count, so the leading bytes are shared by
// every profile for a fixed seed.
[[nodiscard]] inline std::vector<std::byte> MakePatternInfoBits(
    const std::uint32_t bitCount,
    const std::uint64_t seed)
{
    std::vector<std::byte> info(bitCount / 8u);
    SplitMix64 patternRng(seed);
    for (std::uint32_t bitIndex = 0; bitIndex < bitCount; bitIndex++)
    {
        if ((patternRng.Next() & 1u) != 0u)
        {
            SetPackedBit(info, bitIndex, true);
        }
    }
    return info;
}


// Hard-channel LLR from a codeword: bit 0 -> +magnitude, bit 1 ->
// -magnitude, following the frozen LLR sign convention.
[[nodiscard]] inline std::vector<std::int16_t> MakeCleanLlr(
    const std::span<const std::byte> codeword,
    const std::int16_t magnitude)
{
    std::vector<std::int16_t> llr;
    llr.reserve(codeword.size() * 8u);
    for (const std::byte value : codeword)
    {
        const auto byteValue = std::to_integer<std::uint8_t>(value);
        for (std::uint32_t bit = 0; bit < 8u; bit++)
        {
            const bool one = (byteValue & (1u << bit)) != 0u;
            llr.push_back(one ? -magnitude : magnitude);
        }
    }
    return llr;
}

// Distinct bit positions in [0, bound) drawn from the PRNG.
[[nodiscard]] inline std::vector<std::uint32_t> MakeDistinctBitIndices(
    SplitMix64& random,
    const std::uint32_t count,
    const std::uint32_t bound)
{
    std::vector<std::uint32_t> indices;
    indices.reserve(count);
    std::vector<bool> used(bound, false);
    while (indices.size() < count)
    {
        const std::uint32_t candidate =
            static_cast<std::uint32_t>(random.Next() % bound);
        if (!used[candidate])
        {
            used[candidate] = true;
            indices.push_back(candidate);
        }
    }
    return indices;
}

inline void FlipLlr(
    std::vector<std::int16_t>& llr,
    const std::span<const std::uint32_t> bitIndices) noexcept
{
    for (const std::uint32_t index : bitIndices)
    {
        llr[index] = -llr[index];
    }
}

[[nodiscard]] inline std::vector<std::byte> EncodeCodewordOrDie(
    const pbinnerfec::InnerFecProfileId profileId,
    const std::span<const std::byte> info)
{
    const pbinnerfec::InnerFecProfile* profile =
        pbinnerfec::GetInnerFecProfile(profileId);
    REQUIRE(profile != nullptr);
    std::vector<std::byte> codeword(profile->GetCodewordByteCount());
    const auto result = pbinnerfec::EncodeQcLdpcCodeword(
        profileId, info, codeword);
    REQUIRE(result);
    return codeword;
}

// Builds the explicit parity-check rows (row -> incident variable column
// list) directly from the embedded matrix table plus the staircase parity
// structure. This independent edge construction is the cross-check oracle
// for the library's structured syndrome computation.
[[nodiscard]] inline std::vector<std::vector<std::uint32_t>>
    BuildExplicitCheckRows(const pbinnerfec::InnerFecProfileId profileId)
{
    const pbinnerfec::InnerFecProfile* profile =
        pbinnerfec::GetInnerFecProfile(profileId);
    const pbinnerfec::DvbS2ShortMatrix* matrix =
        profile == nullptr ? nullptr
                           : pbinnerfec::GetDvbS2ShortMatrix(profile->kBits);
    if (matrix == nullptr)
    {
        return {};
    }
    const std::uint32_t numChecks = matrix->parityBits;
    std::vector<std::vector<std::uint32_t>> rows(numChecks);
    for (std::uint32_t lineIndex = 0;
        lineIndex < matrix->numLines; lineIndex++)
    {
        const std::uint8_t degree = matrix->lineDegrees[lineIndex];
        const std::uint16_t shiftOffset =
            matrix->lineShiftOffsets[lineIndex];
        for (std::uint32_t withinLine = 0;
            withinLine < matrix->mGroups; withinLine++)
        {
            for (std::uint32_t shiftIndex = 0;
                shiftIndex < degree; shiftIndex++)
            {
                const std::uint32_t row =
                    (static_cast<std::uint32_t>(
                        matrix->lineShifts[shiftOffset + shiftIndex]) +
                    withinLine * matrix->qShift) % numChecks;
                rows[row].push_back(
                    lineIndex * matrix->mGroups + withinLine);
            }
        }
    }
    for (std::uint32_t checkIndex = 0;
        checkIndex < numChecks; checkIndex++)
    {
        rows[checkIndex].push_back(matrix->kBits + checkIndex);
        if (checkIndex >= 1)
        {
            rows[checkIndex].push_back(matrix->kBits + checkIndex - 1);
        }
    }
    return rows;
}

// Brute-force H * c (mod 2) over explicit rows: true when every row parity
// is zero (i.e. c is a valid codeword).
[[nodiscard]] inline bool BruteForceSyndromeZero(
    const std::span<const std::byte> codeword,
    const std::vector<std::vector<std::uint32_t>>& rows)
{
    for (const std::vector<std::uint32_t>& row : rows)
    {
        std::uint32_t parity = 0;
        for (const std::uint32_t column : row)
        {
            parity ^= GetPackedBit(codeword, column) ? 1u : 0u;
        }
        if (parity != 0u)
        {
            return false;
        }
    }
    return true;
}

} // namespace pbinnertectest
