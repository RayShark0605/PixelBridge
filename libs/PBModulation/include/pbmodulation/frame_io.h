#pragma once

#include "pbmodulation/modulation_result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace pbmodulation {

// ---------------------------------------------------------------------------
// Raw frame container "PBRW" v1 (design Phase-0 "PNG/raw-frame
// encode/decode"): a self-describing header plus raw BGRA pixels
// (row-major, 4 bytes per pixel, no padding).
//
//   offset 0   size 4   magic "PBRW"
//   offset 4   size 1   version = 1
//   offset 5   size 3   reserved = 0
//   offset 8   size 4   width  (u32 LE)
//   offset 12  size 4   height (u32 LE)
//   offset 16  size 4   pixelBytes (u32 LE, must equal width*height*4)
//   offset 20  size 8   reserved = 0
//   offset 28  ...      pixel bytes
// ---------------------------------------------------------------------------
inline constexpr std::array<std::byte, 4> kRawFrameMagic{
    std::byte{0x50},
    std::byte{0x42},
    std::byte{0x52},
    std::byte{0x57}};
inline constexpr std::uint8_t kRawFrameVersion = 1;
inline constexpr std::size_t kRawFrameHeaderBytes = 28;
// Decode-side bound so hostile headers cannot drive unbounded allocation.
inline constexpr std::uint32_t kMaximumFrameDimension = 16384;

// Encodes raw BGRA pixels into a PBRW container. width/height must be in
// [1, kMaximumFrameDimension] and bgra must hold exactly width*height*4
// bytes.
[[nodiscard]] ModulationResult<std::vector<std::byte>> EncodeRawFrame(
    std::span<const std::byte> bgra,
    const std::uint32_t width,
    const std::uint32_t height) noexcept;

// Decodes a PBRW container into outBgra (which must hold width*height*4
// bytes) and reports the dimensions. Every header field is validated;
// trailing bytes after the declared pixel data are rejected.
[[nodiscard]] ModulationStatus DecodeRawFrame(
    std::span<const std::byte> raw,
    std::span<std::byte> outBgra,
    std::uint32_t& outWidth,
    std::uint32_t& outHeight) noexcept;

// ---------------------------------------------------------------------------
// PNG frame I/O. The reference frame format is 8-bit BGRA; PNG stores the
// byte-swapped 8-bit RGBA channel order, and the conversion happens at this
// boundary only.
//
// Encoder (deterministic, frozen for the Golden Vector): non-interlaced,
// PNG_FILTER_NONE, fixed compression level, libpng pinned to 1.6.58 (see
// docs/REFERENCE_RASTER.md).
// ---------------------------------------------------------------------------

// Encodes BGRA pixels into an 8-bit RGBA PNG. width/height must be in
// [1, kMaximumFrameDimension] and bgra must hold exactly width*height*4
// bytes.
[[nodiscard]] ModulationResult<std::vector<std::byte>> EncodePngFrame(
    std::span<const std::byte> bgra,
    const std::uint32_t width,
    const std::uint32_t height) noexcept;

// Decodes a PNG, normalizing to 8-bit RGBA (gray/palette/16-bit/interlaced
// inputs are accepted and expanded), swaps back to BGRA in outBgra (which
// must hold expectedWidth*expectedHeight*4 bytes) and requires the decoded
// dimensions to match the expectation.
[[nodiscard]] ModulationStatus DecodePngFrame(
    std::span<const std::byte> png,
    const std::uint32_t expectedWidth,
    const std::uint32_t expectedHeight,
    std::span<std::byte> outBgra) noexcept;

} // namespace pbmodulation