#pragma once

#include "pbmodulation/desktop_levels.h"
#include "pbmodulation/shape_chroma.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace pbmodulation::detail
{

enum class LocalDesktopBinding { BootstrapOnly, DesktopLevels, ShapeChroma };

[[nodiscard]] inline bool MatchesLocalDesktopBinding(const std::uint64_t profileId, const std::uint8_t layout, const LocalDesktopBinding binding) noexcept
{
    switch (binding)
    {
    case LocalDesktopBinding::BootstrapOnly: return profileId == kLocalDesktopVisualProfileId && layout == kLocalDesktopLayoutVersion;
    case LocalDesktopBinding::DesktopLevels: return layout == kDesktopLevelsLayoutVersion && GetDesktopLevelsProfile(profileId) != nullptr;
    case LocalDesktopBinding::ShapeChroma: return profileId == kShapeChromaProfileId && layout == kShapeChromaLayoutVersion;
    }
    return false;
}
[[nodiscard]] inline std::uint8_t GetLocalDesktopBindingLayoutVersion(const LocalDesktopBinding binding) noexcept
{
    switch (binding)
    {
    case LocalDesktopBinding::BootstrapOnly: return kLocalDesktopLayoutVersion;
    case LocalDesktopBinding::DesktopLevels: return kDesktopLevelsLayoutVersion;
    case LocalDesktopBinding::ShapeChroma: return kShapeChromaLayoutVersion;
    }
    return 0;
}
[[nodiscard]] ModulationStatus EncodeLocalDesktopScaffold(std::span<const std::byte> record, std::span<std::byte> pixels, LocalDesktopBinding binding) noexcept;
[[nodiscard]] LocalDesktopObservation DecodeLocalDesktopScaffold(const LumaView& view, const LocalDesktopDecodePolicy& policy, LocalDesktopBinding binding) noexcept;
void FillLocalDesktopBlock(std::span<std::byte> pixels, const LocalDesktopRegion& region, std::uint8_t level) noexcept;
void FillLocalDesktopColorBlock(std::span<std::byte> pixels, const LocalDesktopRegion& region,
    std::uint8_t blue, std::uint8_t green, std::uint8_t red) noexcept;

inline constexpr std::uint16_t kBootstrapRsFieldPolynomial = 0x11D;
inline constexpr std::uint32_t kBootstrapRsFullSymbols = 255;
inline constexpr std::uint32_t kBootstrapRsShorteningSymbols = 179;
inline constexpr std::uint32_t kBootstrapRsMaximumErrors = 16;

enum class BootstrapRsError : std::uint8_t
{
    None, InvalidInputSize, InvalidOutputSize, LocatorDegree, LocatorRootCount,
    ShorteningViolation, MagnitudeFailure, SyndromeMismatch
};

struct BootstrapRsStatus
{
    BootstrapRsError error = BootstrapRsError::None;
    std::uint32_t correctedSymbols = 0;
    // Full 255-symbol position for ShorteningViolation; zero otherwise.
    std::uint32_t errorPosition = 0;
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return error == BootstrapRsError::None;
    }
};

// GF(256), polynomial 0x11d, alpha=2, roots alpha^0..alpha^31. Generator and
// codeword coefficients are highest-degree first. Prepend 179 known zero
// symbols to 44 data bytes, encode RS(255,223), then omit those same zeros.
// Errors only, at most 16 corrected byte symbols; no erasure decoder or heap.
// These functions operate on opaque bytes, not Bootstrap protocol validity.
// All failure paths leave caller output unchanged, including overlapping spans.
[[nodiscard]] BootstrapRsStatus EncodeBootstrapRs(std::span<const std::byte> record, std::span<std::byte> codeword) noexcept;
[[nodiscard]] BootstrapRsStatus DecodeBootstrapRs(std::span<const std::byte> codeword, std::span<std::byte> record) noexcept;
[[nodiscard]] std::uint8_t MultiplyBootstrapField(std::uint8_t left, std::uint8_t right) noexcept;

// A module value is a luma bit: 0=32 (black), 1=224 (white).
// Baseline finder: black outer perimeter and central 3x3, white intervening
// ring. Role bits overwrite these positions, in this exact bit order:
// (0,0),(1,0),(5,0),(6,0),(0,6),(1,6),(5,6),(6,6).
// TL/TR/BL/BR patterns are 0x00/0x0F/0x33/0x55, each LSB-first. All pairs
// have Hamming distance 4, and the central cross retains 1:1:3:1:1 runs.
// Invalid marker/module indices return 0xff, never a valid luma bit.
[[nodiscard]] std::uint8_t MarkerModule(std::size_t markerIndex, std::uint32_t column, std::uint32_t row) noexcept;

// Exact digest input: ASCII "PB-LDBS-X1-Pilot" (16 bytes, no NUL), canonical
// 44 record bytes, then one byte pilotIndex (0..8). BLAKE3-256, first 16 bytes,
// byte/bit LSB-first; each bit b maps to cells [b, b^1] (Manchester).
// Output holds exactly 256 bytes, each 0 or 1, in row-major 16x16 cell order.
// No allocation. False for wrong size/index; failure leaves output unchanged.
[[nodiscard]] bool BuildTimingBits(std::span<const std::byte> record, std::size_t pilotIndex,
                                   std::span<std::uint8_t> output) noexcept;

} // namespace pbmodulation::detail
