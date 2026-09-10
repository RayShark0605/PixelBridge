#pragma once

#include "pbmodulation/local_desktop_decode.h"
#include "pbmodulation/unified_visual_profile.h"
#include "pbprotocol/protocol_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace pbmodulation
{

// Supplemental control bands for the experimental blank-control identity
// (PB-Experimental-BlankControl-1, layout 11). Two 608x64 regions sit in the
// canvas areas the 33-region unified map leaves blank; each band mirrors one
// PB-Control-1 record per frame inside a full-length RS(255,223) codeword
// rendered as 4x4 cells. The main-region contract, its control slots and the
// product SC6-V3 catalog are unchanged; a receiver that does not opt into the
// experimental binding simply ignores these pixels.
inline constexpr std::array<UnifiedPixelRegion, 2> kSupplementalBands{
    UnifiedPixelRegion{1056, 16, 608, 64},
    UnifiedPixelRegion{256, 1000, 608, 64}};

[[nodiscard]] constexpr bool ValidateSupplementalBandRegions() noexcept
{
    const UnifiedPixelRegion canvas{0, 0, 1920, 1080};
    for (const auto& band : kSupplementalBands)
    {
        if (!unified_detail::IsRegionWithin(band, canvas))
        {
            return false;
        }
        for (const auto& existing : kUnifiedVisualProfile.regions)
        {
            if (unified_detail::RegionsOverlap(band, existing.bounds))
            {
                return false;
            }
        }
    }
    return !unified_detail::RegionsOverlap(kSupplementalBands[0], kSupplementalBands[1]);
}
static_assert(ValidateSupplementalBandRegions());

inline constexpr std::size_t kSupplementalBandPatchBytes = 608 * 64 * 4;
// Wire constants of the band message; see supplemental_band.cpp for the
// exact field layout (magic "PBB1", version, length, tag, frame, CRC32C).
inline constexpr std::size_t kSupplementalBandHeaderBytes = 28;
inline constexpr std::size_t kSupplementalBandMaximumRecordBytes = 195;
inline constexpr std::size_t kSupplementalBandMessageBytes = 223;
inline constexpr std::size_t kSupplementalBandCodewordBytes = 255;

enum class SupplementalBandDecodeStatus : std::uint8_t
{
    Admitted,
    // A center-luma hard decision fell into the ambiguous 96..160 band; the
    // whole codeword is rejected without attempting RS correction.
    AmbiguousCell,
    // RS decoding failed (locator degree, root count, magnitude or syndrome).
    RsFailure,
    // Magic, version, length, session tag or frame sequence did not match.
    IdentityRejected,
    // CRC32C mismatch over the sealed message.
    CrcRejected,
    // Message padding is non-zero or the embedded record fails control-plane
    // parsing / tag consistency.
    RecordRejected,
    // Geometry sampling left the frame bounds or the view is not BGRA8.
    SamplingRejected
};

struct SupplementalBandDecodeOutcome
{
    SupplementalBandDecodeStatus status = SupplementalBandDecodeStatus::RecordRejected;
    std::size_t recordBytes = 0;
    std::uint32_t correctedSymbols = 0;
    [[nodiscard]] bool IsAdmitted() const noexcept
    {
        return status == SupplementalBandDecodeStatus::Admitted;
    }
};

// Packs one PB-Control-1 record into the sealed 223-byte band message and
// renders its 255-symbol RS codeword into the 608x64 BGRA patch. Returns
// false when the record exceeds kSupplementalBandMaximumRecordBytes or is not
// a control record carrying the given session tag.
[[nodiscard]] bool RenderSupplementalBand(std::span<const std::byte> controlRecord,
    pbprotocol::SessionTag sessionTag, std::uint64_t frameSequence, std::span<std::byte> patch);

// Copies a rendered patch into the blank band region of a 1920x1080 BGRA
// frame. The untouched remainder of the frame is left alone so the overlay
// composes with EncodeUnifiedVisualFrame output.
[[nodiscard]] bool BlitSupplementalBand(std::span<const std::byte> patch,
    std::span<std::byte> frameBgra, std::size_t bandIndex);

// Rebuilds the band's logical 608x64 raster by sampling the physical frame
// through the given (already admission-verified) geometry. The mapping
// mirrors the main-region sampler: physical = origin + scale*(logical+0.5)
// - 0.5, bilinear on magnified axes, nearest on downscaled axes, non-finite
// or out-of-frame targets are rejected. At exact 1:1 this reproduces a fixed
// row-copy extraction byte-for-byte.
[[nodiscard]] bool SampleSupplementalBand(const LumaView& view, const LocalDesktopGeometry& geometry,
    std::size_t bandIndex, std::span<std::byte> patch);

// Samples and decodes one band. patchScratch must hold exactly
// kSupplementalBandPatchBytes and is caller-owned so the demodulation path
// stays allocation-free; recordOutput must hold at least
// kSupplementalBandMaximumRecordBytes.
[[nodiscard]] SupplementalBandDecodeOutcome DecodeSupplementalBand(const LumaView& view,
    const LocalDesktopGeometry& geometry, std::size_t bandIndex,
    pbprotocol::SessionTag expectedSessionTag, std::uint64_t expectedFrameSequence,
    std::span<std::byte> patchScratch, std::span<std::byte> recordOutput);

} // namespace pbmodulation
