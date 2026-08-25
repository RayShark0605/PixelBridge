#pragma once
// Shared deterministic helpers for the PBModulation test suite.
//
// Every "random" pattern is SplitMix64-driven so the whole suite is exactly
// reproducible. The golden bootstrap/control records are reused byte-for-byte
// from tests/PBProtocol/test_bootstrap_control_codec.cpp, and the data-lane
// Inner-FEC information pattern reuses the PBInnerFec golden convention
// (bit-wise SplitMix64(0xC0FFEE), self-checked against the pinned first-16
// byte hex in test_inner_fec_encoder_golden.cpp).

#include "pbinnerfec/inner_fec_profile.h"
#include "pbinnerfec/qc_ldpc_codec.h"
#include "pbmodulation/frame_io.h"
#include "pbmodulation/reference_raster.h"
#include "pbmodulation/reference_visual_profile.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/crc32c.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace pbmodtest {

using pbmodulation::kReferenceBootstrapRecordBytes;
using pbmodulation::kReferenceControlWindowBytes;
using pbmodulation::kReferenceDataRegionBytes;
using pbmodulation::kReferenceFrameBgraBytes;

// Deterministic PRNG (splitmix64); identical to the PBInnerFec helper so the
// shared golden pattern is byte-identical across the two suites.
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

// Byte-wise comparison: std::byte has no element == that std::array or
// std::span comparisons can use, so the suite compares bytes explicitly.
[[nodiscard]] inline bool BytesEqual(
    const std::span<const std::byte> left,
    const std::span<const std::byte> right) noexcept
{
    if (left.size() != right.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); i++)
    {
        if (left[i] != right[i])
        {
            return false;
        }
    }
    return true;
}

// Deterministic bit-wise pseudo-random information pattern: bit i is
// SplitMix64(seed).Next() & 1, packed LSB first.
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
            info[bitIndex / 8u] |=
                std::byte{static_cast<std::uint8_t>(1u << (bitIndex % 8u))};
        }
    }
    return info;
}

// First 16 bytes of the bit-wise SplitMix64(0xC0FFEE) pattern; pinned in
// tests/PBInnerFec/test_inner_fec_encoder_golden.cpp, so the two suites stay
// provably in sync.
constexpr char kPatternInfoFirst16Hex[] = "e6e5215a64cb2a5199ac6341bd74ad28";

[[nodiscard]] inline std::vector<std::byte> MakeRobustGoldenCodeword()
{
    const auto info = MakePatternInfoBits(10800u, 0xC0FFEEu);
    REQUIRE(ToHex(std::span<const std::byte>(info).first(16)) ==
        kPatternInfoFirst16Hex);
    const auto* profile =
        pbinnerfec::GetInnerFecProfile(pbinnerfec::kInnerFecProfileIdRobust);
    REQUIRE(profile != nullptr);
    REQUIRE(profile->GetCodewordByteCount() == 2025u);
    std::vector<std::byte> codeword(profile->GetCodewordByteCount());
    const auto result = pbinnerfec::EncodeQcLdpcCodeword(
        pbinnerfec::kInnerFecProfileIdRobust, info, codeword);
    REQUIRE(result);
    const auto syndromeResult = pbinnerfec::ComputeQcLdpcSyndrome(
        pbinnerfec::kInnerFecProfileIdRobust, codeword);
    REQUIRE(syndromeResult);
    REQUIRE(syndromeResult.Value());
    return codeword;
}

// Byte helper (mirrors pbprotocol::test::Byte in the PBProtocol suite).
// constexpr so the frozen golden arrays below stay constant expressions.
[[nodiscard]] constexpr std::byte Byte(const std::uint8_t value) noexcept
{
    return std::byte{value};
}

// Filled byte array helper (std::array has no fill constructor).
template <std::size_t Count>
[[nodiscard]] constexpr std::array<std::byte, Count> MakeFilledBytes(
    const std::uint8_t value) noexcept
{
    std::array<std::byte, Count> result{};
    for (std::byte& element : result)
    {
        element = std::byte{value};
    }
    return result;
}

// Golden 44-byte PB-Bootstrap-1 record (design 8.2), identical to
// kBootstrapGolden in tests/PBProtocol/test_bootstrap_control_codec.cpp:
// layout 1, profile 0x0102030405060708, tag 0x81DF204BD997BAD0,
// sequence 0x1112131415161718, epoch 0x21222324, flags 0, CRC 0xD488E1EA.
constexpr std::array<std::byte, kReferenceBootstrapRecordBytes>
    kBootstrapGolden{
        Byte(0x50), Byte(0x42), Byte(0x52), Byte(0x47),
        Byte(0x01), Byte(0x01), Byte(0x00), Byte(0x01),
        Byte(0x08), Byte(0x07), Byte(0x06), Byte(0x05),
        Byte(0x04), Byte(0x03), Byte(0x02), Byte(0x01),
        Byte(0xD0), Byte(0xBA), Byte(0x97), Byte(0xD9),
        Byte(0x4B), Byte(0x20), Byte(0xDF), Byte(0x81),
        Byte(0x18), Byte(0x17), Byte(0x16), Byte(0x15),
        Byte(0x14), Byte(0x13), Byte(0x12), Byte(0x11),
        Byte(0x24), Byte(0x23), Byte(0x22), Byte(0x21),
        Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00),
        Byte(0xEA), Byte(0xE1), Byte(0x88), Byte(0xD4)};

// Golden 67-byte PB-Control-1 SessionDescriptor record, identical to
// kControlGolden in tests/PBProtocol/test_bootstrap_control_codec.cpp
// (type 1, seq 0x0102030405060708, tag 0x81DF204BD997BAD0, 37-byte payload,
// CRC 0xA13883C8).
constexpr std::array<std::byte, 67> kControlGolden{
    Byte(0x50), Byte(0x42), Byte(0x43), Byte(0x52),
    Byte(0x01), Byte(0x01),
    Byte(0x08), Byte(0x07), Byte(0x06), Byte(0x05),
    Byte(0x04), Byte(0x03), Byte(0x02), Byte(0x01),
    Byte(0xD0), Byte(0xBA), Byte(0x97), Byte(0xD9),
    Byte(0x4B), Byte(0x20), Byte(0xDF), Byte(0x81),
    Byte(0x43), Byte(0x00), Byte(0x00), Byte(0x00),
    Byte(0x01), Byte(0x00), Byte(0x00), Byte(0x00),
    Byte(0x00), Byte(0x01), Byte(0x02), Byte(0x03),
    Byte(0x04), Byte(0x05), Byte(0x06), Byte(0x07),
    Byte(0x08), Byte(0x09), Byte(0x0A), Byte(0x0B),
    Byte(0x0C), Byte(0x0D), Byte(0x0E), Byte(0x0F),
    Byte(0x75), Byte(0x00), Byte(0x00), Byte(0x00),
    Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00),
    Byte(0x01), Byte(0x00), Byte(0x00), Byte(0x00),
    Byte(0x00), Byte(0x00), Byte(0x00), Byte(0x00),
    Byte(0x01),
    Byte(0xC8), Byte(0x83), Byte(0x38), Byte(0xA1)};

// One canonical PB-ReferenceRaster-1 frame payload.
struct GoldenFramePayload
{
    std::array<std::byte, kReferenceBootstrapRecordBytes> bootstrap{};
    std::array<std::byte, kReferenceControlWindowBytes> control{};
    std::vector<std::byte> data;

    [[nodiscard]] pbmodulation::ReferenceFrameInput MakeInput() const
    {
        return pbmodulation::ReferenceFrameInput{
            bootstrap,
            std::span<const std::byte>(control),
            std::span<const std::byte>(data)};
    }
};

// G0: all-zero payload (every data lane at level L_0 = 8).
[[nodiscard]] inline GoldenFramePayload MakeZeroGoldenPayload()
{
    return GoldenFramePayload{
        std::array<std::byte, kReferenceBootstrapRecordBytes>{},
        std::array<std::byte, kReferenceControlWindowBytes>{},
        std::vector<std::byte>(kReferenceDataRegionBytes)};
}

// G1: canonical documented payload - golden bootstrap record, golden
// 67-byte control record (zero-padded to the 240-byte window) and the 2025
// byte Robust QC-LDPC golden codeword (zero-padded to the data region).
[[nodiscard]] inline GoldenFramePayload MakeCanonicalGoldenPayload()
{
    GoldenFramePayload payload = MakeZeroGoldenPayload();
    payload.bootstrap = kBootstrapGolden;
    for (std::size_t i = 0; i < kControlGolden.size(); i++)
    {
        payload.control[i] = kControlGolden[i];
    }
    const std::vector<std::byte> codeword = MakeRobustGoldenCodeword();
    std::copy(codeword.begin(), codeword.end(), payload.data.begin());
    return payload;
}

// G2: maximum payload (every byte 0xFF: symbol 0xF everywhere, Gray
// index 8, so every data tile renders at level L_8 = 136).
[[nodiscard]] inline GoldenFramePayload MakeMaxGoldenPayload()
{
    return GoldenFramePayload{
        MakeFilledBytes<kReferenceBootstrapRecordBytes>(0xFF),
        MakeFilledBytes<kReferenceControlWindowBytes>(0xFF),
        std::vector<std::byte>(kReferenceDataRegionBytes, Byte(0xFF))};
}

// Splits a payload into the exact frozen sizes (defensive precondition).
[[nodiscard]] inline pbmodulation::ReferenceFrameInput MakeFrameInput(
    const GoldenFramePayload& payload)
{
    return payload.MakeInput();
}

// Encodes a full canonical frame; dies on any encoder failure.
[[nodiscard]] inline std::vector<std::byte> EncodeFrameOrDie(
    const pbmodulation::ReferenceFrameInput& input)
{
    std::vector<std::byte> bgra(kReferenceFrameBgraBytes);
    const auto status = pbmodulation::EncodeReferenceFrame(input, bgra);
    REQUIRE(status);
    return bgra;
}

// Decodes a full canonical frame; dies on any demod failure.
[[nodiscard]] inline pbmodulation::DecodedReferenceFrame DecodeFrameOrDie(
    const std::span<const std::byte> bgra)
{
    const auto result = pbmodulation::DecodeReferenceFrame(bgra);
    REQUIRE(result);
    return result.Value();
}

// Sets the pixel at (x, y) of a BGRA frame to (b, g, r, a).
inline void SetFramePixel(
    std::span<std::byte> bgra,
    const std::uint32_t x,
    const std::uint32_t y,
    const std::uint8_t b,
    const std::uint8_t g,
    const std::uint8_t r,
    const std::uint8_t a)
{
    REQUIRE(bgra.size() == kReferenceFrameBgraBytes);
    const std::size_t base =
        (static_cast<std::size_t>(y) * 1920u + static_cast<std::size_t>(x)) *
        4u;
    bgra[base] = Byte(b);
    bgra[base + 1] = Byte(g);
    bgra[base + 2] = Byte(r);
    bgra[base + 3] = Byte(a);
}

// The unique region of a given type in the frozen table (unreachable miss:
// the table is compile-time validated by ValidateReferenceVisualProfile).
[[nodiscard]] inline const pbmodulation::ReferenceRegion& FindFrozenRegion(
    const pbmodulation::ReferenceRegionType type)
{
    for (const auto& region : pbmodulation::kReferenceRegions)
    {
        if (region.type == type)
        {
            return region;
        }
    }
    throw std::runtime_error("frozen region table is missing a region type");
}

// Data-lane helper: (col, row) of the 472x238 tile grid -> first pixel.
[[nodiscard]] inline std::pair<std::uint32_t, std::uint32_t>
    GetDataTileOrigin(
        const std::uint32_t tileColumn,
        const std::uint32_t tileRow)
{
    const auto& dataGrid =
        FindFrozenRegion(pbmodulation::ReferenceRegionType::DataGrid);
    return {
        dataGrid.x + tileColumn * pbmodulation::kReferenceDataTileWidth,
        dataGrid.y + tileRow * pbmodulation::kReferenceDataTileHeight};
}

// Lane helper: symbol index (row-major, 240 per row) of a lane whose first
// row starts at laneOriginY -> first pixel.


[[nodiscard]] inline std::pair<std::uint32_t, std::uint32_t>
    GetLaneSymbolOrigin(
        const std::uint32_t laneOriginY,
        const std::uint32_t symbolIndex) noexcept
{
    constexpr std::uint32_t kSymbolsPerRow = 240u;
    return {
        static_cast<std::uint32_t>(symbolIndex % kSymbolsPerRow) * 8u,
        laneOriginY + static_cast<std::uint32_t>(symbolIndex / kSymbolsPerRow) *
            8u};
}

} // namespace pbmodtest