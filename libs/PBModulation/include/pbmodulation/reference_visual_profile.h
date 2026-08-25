#pragma once

#include "pbmodulation/modulation_result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace pbmodulation {

// ---------------------------------------------------------------------------
// PB-ReferenceRaster-1: frozen Phase-0 CPU reference visual profile
// (design document sections 16, 17.4 and 38.2). This is a deterministic
// reference candidate used to establish the protocol-bytes -> visual-frame
// -> protocol-bytes Golden Vector. It is not a certified profile (design
// 34.6); the certified freeze gate happens in a later phase.
//
// Canvas: fixed logical 1920 x 1080, 1:1 physical pixels.
//
// Constellation: 16 luma levels L_i = 8 + 16*i for i = 0..15, i.e.
// {8, 24, 40, ..., 248}. Every pixel of a data-carrying region is
// (B,G,R) = (L_i,L_i,L_i) and A = 255 everywhere on the canvas. The guard
// band keeps every level at least 7 steps from both range endpoints.
//
// Symbols: a 4-bit symbol s maps to level L_gray4(s) where gray4(s) =
// s ^ (s >> 1). The bit stream is LSB-first: stream bit b belongs to byte
// S[b/8] at position b%8, matching the PBInnerFec systematic bit order.
// Symbol k of a lane carries stream bits 4k..4k+3; symbol bit i (LSB first
// within the symbol) is stream bit 4k+i.
//
// The canvas is partitioned into 14 non-overlapping rectangles whose union
// is exactly the full canvas:
//   Guard      constant level 0 (pure black)
//   Sync       8x8 checkerboard of level 0 / 255
//   BootstrapA 240 8x8 luma symbols; symbol indices 0..87 carry the 44-byte
//              PB-Bootstrap-1 record, the remaining symbols are reserved
//              (symbol 0)
//   Control    480 8x8 luma symbols = a 240-byte per-frame control window
//   Pilot      fixed calibration raster (16-level ladder + references)
//   DataGrid   472 x 238 4x4 luma tiles = 56168 payload bytes per frame
//   BootstrapB 240 8x8 luma symbols; symbol indices 152..239 (x 1216..1919)
//              carry the same 44-byte record, spatially separated from A
// ---------------------------------------------------------------------------

inline constexpr std::uint32_t kReferenceCanvasWidth = 1920;
inline constexpr std::uint32_t kReferenceCanvasHeight = 1080;
inline constexpr std::size_t kReferenceCanvasPixelCount =
    kReferenceCanvasWidth * kReferenceCanvasHeight; // 2073600
inline constexpr std::size_t kReferenceFrameBgraBytes =
    kReferenceCanvasPixelCount * 4; // 8294400

// PB-Bootstrap-1 canonical record size (design 8.2). The raster layer
// treats these bytes as opaque; structural validation remains the
// caller's job (pbprotocol::ParseBootstrapRecord).
inline constexpr std::size_t kReferenceBootstrapRecordBytes = 44;

inline constexpr std::uint8_t kReferenceLevelCount = 16;
inline constexpr std::uint8_t kReferenceLevelBase = 8;
inline constexpr std::uint8_t kReferenceLevelStep = 16;
// Frozen alpha for every canvas pixel.
inline constexpr std::uint8_t kReferenceAlphaValue = 255;

inline constexpr std::uint8_t kReferenceBitsPerSymbol = 4;
inline constexpr std::uint8_t kReferenceDataTileWidth = 4;
inline constexpr std::uint8_t kReferenceDataTileHeight = 4;
inline constexpr std::uint8_t kReferenceLaneSymbolWidth = 8;
inline constexpr std::uint8_t kReferenceLaneSymbolHeight = 8;

// Lane geometry (lane width 1920 / 8 = 240 symbols per row).
inline constexpr std::uint32_t kReferenceLaneSymbolsPerRow = 240;
inline constexpr std::size_t kReferenceBootstrapLaneSymbols = 240;
inline constexpr std::size_t kReferenceBootstrapRecordSymbols = 88;
// Bootstrap A carries the record at symbol indices 0..87 (x 0..703);
// Bootstrap B carries it at 152..239 (x 1216..1919).
inline constexpr std::size_t kReferenceBootstrapASymbolOffset = 0;
inline constexpr std::size_t kReferenceBootstrapBSymbolOffset = 152;
inline constexpr std::size_t kReferenceControlLaneSymbols = 480;
inline constexpr std::size_t kReferenceControlWindowBytes = 240;

// Data grid geometry: origin (16, 64), size 1888 x 952.
inline constexpr std::uint32_t kReferenceDataGridColumns = 472;
inline constexpr std::uint32_t kReferenceDataGridRows = 238;
inline constexpr std::size_t kReferenceDataTileCount =
    kReferenceDataGridColumns * kReferenceDataGridRows; // 112336
inline constexpr std::size_t kReferenceDataRegionBytes =
    kReferenceDataTileCount * kReferenceBitsPerSymbol / 8; // 56168

// Per-frame protocol payload carried by one canonical frame: the 44-byte
// bootstrap record (counted once although duplicated in A/B), the 240-byte
// control window and the 56168-byte data region.
inline constexpr std::size_t kReferenceFrameProtocolBytes =
    kReferenceBootstrapRecordBytes + kReferenceControlWindowBytes +
    kReferenceDataRegionBytes; // 56452

// Compile-time proof of the frozen invariant relations (the runtime
// ValidateReferenceVisualProfile() additionally proves the table-level
// partition, constellation loop and manifest round-trip invariants).
static_assert(kReferenceLevelStep / 2 == 8,
    "the nearest-level margin must be exactly the half step of 8");
static_assert(kReferenceLevelBase == 8 &&
    kReferenceLevelBase + kReferenceLevelStep * (kReferenceLevelCount - 1) ==
        248,
    "the constellation must span exactly 8..248");
static_assert(kReferenceLaneSymbolsPerRow ==
    kReferenceCanvasWidth / kReferenceLaneSymbolWidth,
    "the lane rows must tile the canvas width exactly");
static_assert(kReferenceBootstrapRecordSymbols * kReferenceBitsPerSymbol / 8 ==
    kReferenceBootstrapRecordBytes,
    "88 record symbols must carry exactly the 44-byte bootstrap record");
static_assert(kReferenceBootstrapBSymbolOffset +
    kReferenceBootstrapRecordSymbols == kReferenceBootstrapLaneSymbols,
    "bootstrap B must carry the record at the lane tail");
static_assert(kReferenceControlLaneSymbols * kReferenceBitsPerSymbol / 8 ==
    kReferenceControlWindowBytes,
    "the control lane must carry exactly the 240-byte window");
static_assert(kReferenceDataGridColumns * kReferenceDataTileWidth == 1888 &&
    kReferenceDataGridRows * kReferenceDataTileHeight == 952,
    "the data grid must tile the 1888x952 region exactly");
static_assert(kReferenceDataTileCount ==
    kReferenceDataGridColumns * kReferenceDataGridRows,
    "the tile count must equal columns x rows");
static_assert(kReferenceDataRegionBytes ==
    kReferenceDataTileCount * kReferenceBitsPerSymbol / 8,
    "the tiles must carry exactly 56168 data bytes");
static_assert(kReferenceFrameProtocolBytes ==
    kReferenceBootstrapRecordBytes + kReferenceControlWindowBytes +
    kReferenceDataRegionBytes,
    "the per-frame protocol byte count must be bootstrap + control + data");

enum class ReferenceRegionType : std::uint8_t
{
    Guard = 0,
    Sync = 1,
    BootstrapA = 2,
    Control = 3,
    Pilot = 4,
    DataGrid = 5,
    BootstrapB = 6
};

struct ReferenceRegion
{
    ReferenceRegionType type = ReferenceRegionType::Guard;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    bool operator==(const ReferenceRegion&) const = default;
};

// Canonical region table order. The union of these 14 rectangles is exactly
// the 1920x1080 canvas with no overlap (design 16.1).
inline constexpr std::array<ReferenceRegion, 14> kReferenceRegions = {
    ReferenceRegion{ReferenceRegionType::Guard, 0, 0, 1920, 8},
    ReferenceRegion{ReferenceRegionType::Sync, 0, 8, 1920, 8},
    ReferenceRegion{ReferenceRegionType::BootstrapA, 0, 16, 1920, 8},
    ReferenceRegion{ReferenceRegionType::Control, 0, 24, 1920, 16},
    ReferenceRegion{ReferenceRegionType::Pilot, 0, 40, 1920, 16},
    ReferenceRegion{ReferenceRegionType::Guard, 0, 56, 1920, 8},
    ReferenceRegion{ReferenceRegionType::Guard, 0, 64, 16, 952},
    ReferenceRegion{ReferenceRegionType::DataGrid, 16, 64, 1888, 952},
    ReferenceRegion{ReferenceRegionType::Guard, 1904, 64, 16, 952},
    ReferenceRegion{ReferenceRegionType::Guard, 0, 1016, 1920, 8},
    ReferenceRegion{ReferenceRegionType::Sync, 0, 1024, 1920, 8},
    ReferenceRegion{ReferenceRegionType::BootstrapB, 0, 1032, 1920, 8},
    ReferenceRegion{ReferenceRegionType::Pilot, 0, 1040, 1920, 16},
    ReferenceRegion{ReferenceRegionType::Guard, 0, 1056, 1920, 24}};

[[nodiscard]] constexpr std::uint8_t GetReferenceLevelValue(
    const std::uint8_t levelIndex) noexcept
{
    return static_cast<std::uint8_t>(kReferenceLevelBase +
        kReferenceLevelStep * levelIndex);
}

[[nodiscard]] constexpr std::uint8_t GrayCode4(const std::uint8_t symbol) noexcept
{
    return static_cast<std::uint8_t>((symbol ^ (symbol >> 1)) & 0x0Fu);
}

// Inverse of GrayCode4 for 4-bit values.
[[nodiscard]] constexpr std::uint8_t GrayDecode4(const std::uint8_t gray) noexcept
{
    const std::uint8_t clamped = gray & 0x0Fu;
    return static_cast<std::uint8_t>(
        (clamped ^ (clamped >> 1) ^ (clamped >> 2) ^ (clamped >> 3)) & 0x0Fu);
}

struct ReferenceRegionManifest
{
    std::uint32_t canvasWidth = 0;
    std::uint32_t canvasHeight = 0;
    std::array<ReferenceRegion, 14> regions{};
    std::uint8_t bitsPerSymbol = 0;
    std::uint8_t levelCount = 0;
    std::uint8_t levelBase = 0;
    std::uint8_t levelStep = 0;
    std::uint8_t dataTileWidth = 0;
    std::uint8_t dataTileHeight = 0;
    std::uint8_t laneSymbolWidth = 0;
    std::uint8_t laneSymbolHeight = 0;

    bool operator==(const ReferenceRegionManifest&) const = default;
};

// Canonical machine-readable region/capacity manifest (design 16.2/38.2):
// "PBVM" magic, version/layout, canvas size, per-region rectangles and
// symbol/constellation parameters, explicit little-endian, with a trailing
// CRC-32C over every preceding byte.
inline constexpr std::array<std::byte, 4> kReferenceManifestMagic{
    std::byte{0x50},
    std::byte{0x42},
    std::byte{0x56},
    std::byte{0x4D}};
inline constexpr std::uint8_t kReferenceManifestVersion = 1;
inline constexpr std::uint8_t kReferenceManifestLayoutVersion = 1;
// 25-byte header + 8 reserved + 14 x 17-byte region entries + 4 CRC.
inline constexpr std::size_t kReferenceManifestBytes = 275;

// The region table and the manifest layout are frozen at exactly these
// shapes (see the compile-time invariant block above the region table).
static_assert(kReferenceRegions.size() == 14,
    "the reference canvas must be partitioned into exactly 14 regions");
static_assert(kReferenceManifestBytes == 275,
    "the reference manifest layout is frozen at 275 bytes");

[[nodiscard]] std::array<std::byte, kReferenceManifestBytes>
    SerializeReferenceRegionManifest() noexcept;

// Accepts exactly the canonical PB-ReferenceRaster-1 manifest; any deviation
// (geometry, parameters, order, reserved bytes or CRC) fails closed.
[[nodiscard]] ModulationResult<ReferenceRegionManifest>
    ParseReferenceRegionManifest(std::span<const std::byte> input) noexcept;

// Fail-closed validation of every frozen invariant: exact canvas coverage
// with no overlap, symbol/tile alignment, constellation parameters and the
// derived capacity equalities.
[[nodiscard]] bool ValidateReferenceVisualProfile() noexcept;

} // namespace pbmodulation