#pragma once
// Private helpers shared by the reference raster codec and the region
// manifest code. Not part of the public API.

#include "pbmodulation/modulation_result.h"
#include "pbmodulation/reference_visual_profile.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace pbmodulation::detail {

inline constexpr std::array<std::uint8_t, kReferenceLevelCount> kLevelValues = {
    8, 24, 40, 56, 72, 88, 104, 120, 136, 152, 168, 184, 200, 216, 232, 248};

struct Pixel
{
    std::uint8_t b = 0;
    std::uint8_t g = 0;
    std::uint8_t r = 0;
    std::uint8_t a = 0;

    bool operator==(const Pixel&) const = default;
};

[[nodiscard]] inline Pixel LoadPixel(
    const std::span<const std::byte> bgra,
    const std::size_t pixelIndex) noexcept
{
    const std::size_t base = pixelIndex * 4;
    return Pixel{
        std::to_integer<std::uint8_t>(bgra[base]),
        std::to_integer<std::uint8_t>(bgra[base + 1]),
        std::to_integer<std::uint8_t>(bgra[base + 2]),
        std::to_integer<std::uint8_t>(bgra[base + 3])};
}

inline void StorePixel(
    const std::span<std::byte> bgra,
    const std::size_t pixelIndex,
    const Pixel pixel) noexcept
{
    const std::size_t base = pixelIndex * 4;
    bgra[base] = std::byte{pixel.b};
    bgra[base + 1] = std::byte{pixel.g};
    bgra[base + 2] = std::byte{pixel.r};
    bgra[base + 3] = std::byte{pixel.a};
}

[[nodiscard]] inline std::uint32_t PixelIndex(
    const std::uint32_t x,
    const std::uint32_t y) noexcept
{
    return y * kReferenceCanvasWidth + x;
}

[[nodiscard]] inline std::pair<std::uint32_t, std::uint32_t> PixelCoordinate(
    const std::size_t pixelIndex) noexcept
{
    return {
        static_cast<std::uint32_t>(pixelIndex % kReferenceCanvasWidth),
        static_cast<std::uint32_t>(pixelIndex / kReferenceCanvasWidth)};
}

// Canonical content of the frozen (non-data-carrying) regions. This is the
// single source of truth for both the encoder and the decoder's byte-exact
// verification (design 16.1/16.5).
[[nodiscard]] inline Pixel ExpectedFrozenPixel(
    const ReferenceRegion& region,
    const std::uint32_t x,
    const std::uint32_t y) noexcept
{
    switch (region.type)
    {
        case ReferenceRegionType::Guard:
            return Pixel{0, 0, 0, kReferenceAlphaValue};
        case ReferenceRegionType::Sync:
        {
            // 8x8 checkerboard cells of level 0 / 255.
            const std::uint8_t level =
                (((x / 8) + (y / 8)) % 2) == 0 ? 0 : 255;
            return Pixel{level, level, level, kReferenceAlphaValue};
        }
        case ReferenceRegionType::Pilot:
        {
            const std::uint32_t localX = x - region.x;
            if (localX < 256)
            {
                // 16 ladder cells of 16x16 covering the full constellation.
                const std::uint8_t level = kLevelValues[localX / 16];
                return Pixel{level, level, level, kReferenceAlphaValue};
            }
            if (localX < 272)
            {
                // Black reference.
                return Pixel{0, 0, 0, kReferenceAlphaValue};
            }
            if (localX < 288)
            {
                // White reference.
                return Pixel{255, 255, 255, kReferenceAlphaValue};
            }
            if (localX < 304)
            {
                // Mid-gray reference.
                return Pixel{128, 128, 128, kReferenceAlphaValue};
            }
            if (localX < 336)
            {
                // 8x8 phase/checker reference (local coordinates so the top
                // and bottom pilots are identical).
                const std::uint32_t cellX = (localX - 304) / 8;
                const std::uint32_t cellY = (y - region.y) / 8;
                const std::uint8_t level = ((cellX + cellY) % 2) == 0 ? 0 : 255;
                return Pixel{level, level, level, kReferenceAlphaValue};
            }
            // Reserved pilot fill: 128 sits exactly at the L_7/L_8
            // tie point (distance 8 from both), so any demod
            // attempt of the reserved fill fails closed.
            return Pixel{128, 128, 128, kReferenceAlphaValue};
        }
        default:
            // Data-carrying regions are never "frozen"; the caller handles
            // them through the symbol demodulation path.
            return Pixel{0, 0, 0, kReferenceAlphaValue};
    }
}

// Frozen margin rule (design 27.4 reference form): the integer luma mean of
// a symbol tile must map to a unique nearest constellation level within a
// distance of at most 7 (strictly inside the half-step of 8). A tie or a
// larger distance fails closed; no value is ever guessed.
[[nodiscard]] inline ModulationStatus NearestLevelIndex(
    const std::uint32_t meanLuma,
    const std::size_t errorOffset,
    std::uint8_t& outLevelIndex) noexcept
{
    const std::uint32_t minLevel = kLevelValues[0];
    const std::uint32_t maxLevel = kLevelValues[kReferenceLevelCount - 1];
    if (meanLuma < minLevel)
    {
        if (minLevel - meanLuma > 7)
        {
            return ModulationStatus::Failure(
                ModulationErrorCode::OffConstellationLevel,
                errorOffset);
        }
        outLevelIndex = 0;
        return ModulationStatus::Success();
    }
    if (meanLuma > maxLevel)
    {
        if (meanLuma - maxLevel > 7)
        {
            return ModulationStatus::Failure(
                ModulationErrorCode::OffConstellationLevel,
                errorOffset);
        }
        outLevelIndex = static_cast<std::uint8_t>(kReferenceLevelCount - 1);
        return ModulationStatus::Success();
    }
    const std::uint32_t diff = meanLuma - minLevel; // 0..240
    const std::uint32_t index = diff / kReferenceLevelStep; // 0..15
    const std::uint32_t remainder = diff % kReferenceLevelStep; // 0..15
    if (remainder == kReferenceLevelStep / 2)
    {
        return ModulationStatus::Failure(
            ModulationErrorCode::AmbiguousLevel,
            errorOffset);
    }
    outLevelIndex = static_cast<std::uint8_t>(
        remainder < kReferenceLevelStep / 2 ? index : index + 1);
    return ModulationStatus::Success();
}

// LSB-first bit access on a byte stream (matches the PBInnerFec systematic
// bit order). bitIndex must be < stream.size() * 8.
[[nodiscard]] inline bool GetStreamBit(
    const std::span<const std::byte> stream,
    const std::size_t bitIndex) noexcept
{
    return (stream[bitIndex / 8] &
        std::byte{static_cast<std::uint8_t>(1u << (bitIndex % 8u))}) !=
        std::byte{0};
}

inline void SetStreamBit(
    const std::span<std::byte> stream,
    const std::size_t bitIndex,
    const bool value) noexcept
{
    const std::byte mask =
        std::byte{static_cast<std::uint8_t>(1u << (bitIndex % 8u))};
    if (value)
    {
        stream[bitIndex / 8] |= mask;
    }
    else
    {
        stream[bitIndex / 8] &= ~mask;
    }
}

// Nibble access for the 2-symbols-per-byte lane packing: even symbol index
// is the low nibble of its byte, odd symbol index is the high nibble.
[[nodiscard]] inline std::uint8_t GetSymbolNibble(
    const std::span<const std::byte> stream,
    const std::size_t symbolIndex) noexcept
{
    const std::uint8_t value = std::to_integer<std::uint8_t>(
        stream[symbolIndex / 2]);
    return (symbolIndex % 2) == 0 ? (value & 0x0Fu)
                                  : ((value >> 4) & 0x0Fu);
}

inline void SetSymbolNibble(
    const std::span<std::byte> stream,
    const std::size_t symbolIndex,
    const std::uint8_t symbol) noexcept
{
    std::uint8_t value = std::to_integer<std::uint8_t>(
        stream[symbolIndex / 2]);
    if ((symbolIndex % 2) == 0)
    {
        value = static_cast<std::uint8_t>(
            (value & 0xF0u) | (symbol & 0x0Fu));
    }
    else
    {
        value = static_cast<std::uint8_t>(
            (value & 0x0Fu) | ((symbol & 0x0Fu) << 4));
    }
    stream[symbolIndex / 2] = std::byte{value};
}

} // namespace pbmodulation::detail
