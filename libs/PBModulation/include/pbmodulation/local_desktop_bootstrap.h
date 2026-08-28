#pragma once

#include "pbmodulation/modulation_result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace pbmodulation
{

// PB-LocalDesktopBootstrap-X1 is an experimental SDR diagnostic mapping, NOT
// PB-ReferenceRaster-1 or a certified payload profile. The canonical logical
// PB-Bootstrap-1 record remains 44 bytes; only its visual/FEC mapping is new.
inline constexpr std::uint64_t kLocalDesktopVisualProfileId = 0x50424C4442533031ULL;
inline constexpr std::uint8_t kLocalDesktopLayoutVersion = 2;
inline constexpr std::uint32_t kLocalDesktopCanvasWidth = 1920;
inline constexpr std::uint32_t kLocalDesktopCanvasHeight = 1080;
inline constexpr std::size_t kLocalDesktopFrameBgraBytes = std::size_t{1920} * 1080 * 4;
inline constexpr std::size_t kLocalDesktopBootstrapRecordBytes = 44;
inline constexpr std::size_t kLocalDesktopRsParityBytes = 32;
inline constexpr std::size_t kLocalDesktopRsCodewordBytes = 76;
inline constexpr std::uint32_t kLocalDesktopCellPixels = 8;
inline constexpr std::uint32_t kLocalDesktopMarkerModules = 7;
inline constexpr std::uint32_t kLocalDesktopMarkerQuietPixels = 4;
inline constexpr std::size_t kLocalDesktopTimingBits = 256;
inline constexpr std::uint8_t kLocalDesktopBlack = 32;
inline constexpr std::uint8_t kLocalDesktopWhite = 224;
inline constexpr std::uint8_t kLocalDesktopBackground = 128;

struct LocalDesktopRegion
{
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool operator==(const LocalDesktopRegion&) const = default;
};

// Marker order/role is TL, TR, BL, BR. Within each 64x64 region: a 4-pixel
// white quiet border surrounds 7x7 binary 8x8 modules. Roles alter only the
// eight noncentral modules documented in local_desktop_internal.h.
inline constexpr std::array<LocalDesktopRegion, 4> kLocalDesktopMarkerRegions{
    LocalDesktopRegion{16, 16, 64, 64}, LocalDesktopRegion{1840, 16, 64, 64},
    LocalDesktopRegion{16, 1000, 64, 64}, LocalDesktopRegion{1840, 1000, 64, 64}};

// Each independent copy contains 76 columns x 8 rows of binary cells.
// Cell i = row*76+column carries codeword[i/8] bit (i%8), LSB-first.
// A and B are never interleaved or combined into one RS codeword.
inline constexpr std::array<LocalDesktopRegion, 2> kLocalDesktopBootstrapRegions{
    LocalDesktopRegion{96, 16, 608, 64}, LocalDesktopRegion{1216, 1000, 608, 64}};

// Row-major positions. Each patch is 16x16 binary cells: Manchester-coded
// first 128 bits of the domain-separated digest specified by BuildTimingBits.
inline constexpr std::array<LocalDesktopRegion, 9> kLocalDesktopTimingRegions{
    LocalDesktopRegion{96, 160, 128, 128}, LocalDesktopRegion{896, 160, 128, 128}, LocalDesktopRegion{1696, 160, 128, 128},
    LocalDesktopRegion{96, 476, 128, 128}, LocalDesktopRegion{896, 476, 128, 128}, LocalDesktopRegion{1696, 476, 128, 128},
    LocalDesktopRegion{96, 792, 128, 128}, LocalDesktopRegion{896, 792, 128, 128}, LocalDesktopRegion{1696, 792, 128, 128}};

static_assert(kLocalDesktopBootstrapRecordBytes + kLocalDesktopRsParityBytes == kLocalDesktopRsCodewordBytes);
static_assert(608 * 64 / (kLocalDesktopCellPixels * kLocalDesktopCellPixels) == kLocalDesktopRsCodewordBytes * 8);
static_assert(kLocalDesktopMarkerModules * kLocalDesktopCellPixels + 2 * kLocalDesktopMarkerQuietPixels == 64);
static_assert(128 * 128 / (kLocalDesktopCellPixels * kLocalDesktopCellPixels) == kLocalDesktopTimingBits);

// Full, deterministic BGRA8 canvas (B=G=R, A=255). Untyped remaining pixels
// are diagnostic reserved background, not a claimed Control/Data payload.
// Input must be a valid canonical PB-Bootstrap-1 record with exactly this
// experimental profile/layout. No heap allocation. Both spans may overlap:
// input and all fallible preparation are staged before writing output.
// Failure leaves output unchanged; error offsets refer to input record bytes.
[[nodiscard]] ModulationStatus EncodeLocalDesktopBootstrapFrame(std::span<const std::byte> bootstrapRecord,
                                                                std::span<std::byte> outBgra) noexcept;

} // namespace pbmodulation
