#include "pbmodulation/local_desktop_bootstrap.h"

#include "local_desktop_internal.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/bootstrap_control_codec.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace pbmodulation
{
namespace
{

constexpr std::array<LocalDesktopRegion, 15> AllRegions() noexcept
{
    std::array<LocalDesktopRegion, 15> regions{};
    std::size_t index = 0;
    for (const auto region : kLocalDesktopMarkerRegions)
    {
        regions[index++] = region;
    }
    for (const auto region : kLocalDesktopBootstrapRegions)
    {
        regions[index++] = region;
    }
    for (const auto region : kLocalDesktopTimingRegions)
    {
        regions[index++] = region;
    }
    return regions;
}

constexpr bool ValidFrozenRegions() noexcept
{
    const auto regions = AllRegions();
    for (std::size_t index = 0; index < regions.size(); index++)
    {
        const auto& region = regions[index];
        const auto right = static_cast<std::uint64_t>(region.x) + region.width;
        const auto bottom = static_cast<std::uint64_t>(region.y) + region.height;
        if (region.width == 0 || region.height == 0 || right > kLocalDesktopCanvasWidth || bottom > kLocalDesktopCanvasHeight)
        {
            return false;
        }
        for (std::size_t otherIndex = 0; otherIndex < index; otherIndex++)
        {
            const auto& other = regions[otherIndex];
            if (region.x < static_cast<std::uint64_t>(other.x) + other.width && other.x < right &&
                region.y < static_cast<std::uint64_t>(other.y) + other.height && other.y < bottom)
            {
                return false;
            }
        }
    }
    return true;
}

static_assert(ValidFrozenRegions());
static_assert(pbprotocol::kBootstrapRecordBytes == kLocalDesktopBootstrapRecordBytes);

void FillBlock(const std::span<std::byte> pixels, const LocalDesktopRegion& region, const std::uint8_t level) noexcept
{
    // All callers use fixed, compile-time bounds checked above; no untrusted
    // geometry or multiplication enters the renderer after output validation.
    for (std::uint32_t row = 0; row < region.height; row++)
    {
        for (std::uint32_t column = 0; column < region.width; column++)
        {
            const std::size_t offset = ((static_cast<std::size_t>(region.y) + row) * kLocalDesktopCanvasWidth + region.x + column) * 4;
            pixels[offset] = static_cast<std::byte>(level);
            pixels[offset + 1] = static_cast<std::byte>(level);
            pixels[offset + 2] = static_cast<std::byte>(level);
            pixels[offset + 3] = std::byte{255};
        }
    }
}

void FillCells(const std::span<std::byte> pixels, const LocalDesktopRegion& region, const std::span<const std::uint8_t> bits) noexcept
{
    const std::uint32_t columns = region.width / kLocalDesktopCellPixels;
    for (std::size_t index = 0; index < bits.size(); index++)
    {
        const LocalDesktopRegion cell{region.x + static_cast<std::uint32_t>(index % columns) * kLocalDesktopCellPixels,
                                      region.y + static_cast<std::uint32_t>(index / columns) * kLocalDesktopCellPixels,
                                      kLocalDesktopCellPixels, kLocalDesktopCellPixels};
        FillBlock(pixels, cell, bits[index] != 0 ? kLocalDesktopWhite : kLocalDesktopBlack);
    }
}

ModulationStatus FromProtocolError(const pbprotocol::ProtocolError& error) noexcept
{
    switch (error.code)
    {
    case pbprotocol::ProtocolErrorCode::InvalidMagic:
        return ModulationStatus::Failure(ModulationErrorCode::InvalidMagic, error.offset);
    case pbprotocol::ProtocolErrorCode::CrcMismatch:
        return ModulationStatus::Failure(ModulationErrorCode::CrcMismatch, error.offset);
    case pbprotocol::ProtocolErrorCode::UnsupportedBootstrapVersion:
    case pbprotocol::ProtocolErrorCode::UnsupportedProtocolMajor:
    case pbprotocol::ProtocolErrorCode::UnsupportedProtocolMinor:
        return ModulationStatus::Failure(ModulationErrorCode::UnsupportedVersion, error.offset);
    default:
        return ModulationStatus::Failure(ModulationErrorCode::InvalidInput, error.offset);
    }
}

} // namespace

namespace detail
{

std::uint8_t MarkerModule(const std::size_t markerIndex, const std::uint32_t column, const std::uint32_t row) noexcept
{
    if (markerIndex >= kLocalDesktopMarkerRegions.size() || column >= kLocalDesktopMarkerModules || row >= kLocalDesktopMarkerModules)
    {
        return 0xFF;
    }
    constexpr std::array<std::uint8_t, 4> roles{0x00, 0x0F, 0x33, 0x55};
    if ((row == 0 || row == 6) && (column <= 1 || column >= 5))
    {
        const std::uint32_t bitIndex = (row == 0 ? 0u : 4u) + (column <= 1 ? column : column - 3u);
        return static_cast<std::uint8_t>((roles[markerIndex] >> bitIndex) & 1u);
    }
    const bool black = column == 0 || column == 6 || row == 0 || row == 6 ||
                       (column >= 2 && column <= 4 && row >= 2 && row <= 4);
    return black ? 0 : 1;
}

bool BuildTimingBits(const std::span<const std::byte> record, const std::size_t pilotIndex, const std::span<std::uint8_t> output) noexcept
{
    if (record.size() != kLocalDesktopBootstrapRecordBytes || pilotIndex >= kLocalDesktopTimingRegions.size() || output.size() != kLocalDesktopTimingBits)
    {
        return false;
    }
    constexpr char domain[] = "PB-LDBS-X1-Pilot";
    static_assert(sizeof(domain) - 1 == 16);
    const std::array<std::byte, 1> position{static_cast<std::byte>(pilotIndex)};
    pbprotocol::Blake3Hasher hasher;
    hasher.Update(std::as_bytes(std::span(domain).first(sizeof(domain) - 1)));
    hasher.Update(record);
    hasher.Update(position);
    const auto digest = hasher.Finalize();
    std::array<std::uint8_t, kLocalDesktopTimingBits> bits{};
    for (std::size_t bitIndex = 0; bitIndex < kLocalDesktopTimingBits / 2; bitIndex++)
    {
        const auto value = static_cast<std::uint8_t>((std::to_integer<std::uint8_t>(digest[bitIndex / 8]) >> (bitIndex % 8)) & 1u);
        bits[2 * bitIndex] = value;
        bits[2 * bitIndex + 1] = static_cast<std::uint8_t>(value ^ 1u);
    }
    std::copy(bits.begin(), bits.end(), output.begin());
    return true;
}

} // namespace detail

ModulationStatus EncodeLocalDesktopBootstrapFrame(const std::span<const std::byte> bootstrapRecord, const std::span<std::byte> outBgra) noexcept
{
    if (bootstrapRecord.size() != kLocalDesktopBootstrapRecordBytes)
    {
        return ModulationStatus::Failure(ModulationErrorCode::InvalidInput, 0);
    }
    if (outBgra.size() != kLocalDesktopFrameBgraBytes)
    {
        return ModulationStatus::Failure(outBgra.size() < kLocalDesktopFrameBgraBytes ? ModulationErrorCode::OutputBufferTooSmall : ModulationErrorCode::InvalidInput, 0);
    }
    std::array<std::byte, kLocalDesktopBootstrapRecordBytes> canonical{};
    std::copy(bootstrapRecord.begin(), bootstrapRecord.end(), canonical.begin());
    const auto parsed = pbprotocol::ParseBootstrapRecord(canonical);
    if (!parsed)
    {
        return FromProtocolError(parsed.Error());
    }
    if (parsed.Value().visualLayoutVersion != kLocalDesktopLayoutVersion)
    {
        return ModulationStatus::Failure(ModulationErrorCode::UnsupportedVersion, 7);
    }
    if (parsed.Value().visualProfileId != kLocalDesktopVisualProfileId)
    {
        return ModulationStatus::Failure(ModulationErrorCode::InvalidInput, 8);
    }
    std::array<std::byte, kLocalDesktopRsCodewordBytes> codeword{};
    if (!detail::EncodeBootstrapRs(canonical, codeword))
    {
        return ModulationStatus::Failure(ModulationErrorCode::InternalInvariantViolation, 0);
    }
    std::array<std::uint8_t, kLocalDesktopRsCodewordBytes * 8> bootstrapBits{};
    for (std::size_t bitIndex = 0; bitIndex < bootstrapBits.size(); bitIndex++)
    {
        bootstrapBits[bitIndex] = static_cast<std::uint8_t>((std::to_integer<std::uint8_t>(codeword[bitIndex / 8]) >> (bitIndex % 8)) & 1u);
    }
    std::array<std::array<std::uint8_t, kLocalDesktopTimingBits>, kLocalDesktopTimingRegions.size()> timing{};
    for (std::size_t index = 0; index < timing.size(); index++)
    {
        if (!detail::BuildTimingBits(canonical, index, timing[index]))
        {
            return ModulationStatus::Failure(ModulationErrorCode::InternalInvariantViolation, 0);
        }
    }

    // Only infallible, fixed-bounds stores remain. Every output pixel is redrawn;
    // neither previous back-buffer contents nor aliased input are read again.
    FillBlock(outBgra, {0, 0, kLocalDesktopCanvasWidth, kLocalDesktopCanvasHeight}, kLocalDesktopBackground);
    for (std::size_t markerIndex = 0; markerIndex < kLocalDesktopMarkerRegions.size(); markerIndex++)
    {
        const auto& marker = kLocalDesktopMarkerRegions[markerIndex];
        FillBlock(outBgra, marker, kLocalDesktopWhite);
        for (std::uint32_t row = 0; row < kLocalDesktopMarkerModules; row++)
        {
            for (std::uint32_t column = 0; column < kLocalDesktopMarkerModules; column++)
            {
                const LocalDesktopRegion module{marker.x + kLocalDesktopMarkerQuietPixels + column * kLocalDesktopCellPixels,
                                                marker.y + kLocalDesktopMarkerQuietPixels + row * kLocalDesktopCellPixels,
                                                kLocalDesktopCellPixels, kLocalDesktopCellPixels};
                FillBlock(outBgra, module, detail::MarkerModule(markerIndex, column, row) != 0 ? kLocalDesktopWhite : kLocalDesktopBlack);
            }
        }
    }
    for (const auto region : kLocalDesktopBootstrapRegions)
    {
        FillCells(outBgra, region, bootstrapBits);
    }
    for (std::size_t index = 0; index < timing.size(); index++)
    {
        FillCells(outBgra, kLocalDesktopTimingRegions[index], timing[index]);
    }
    return ModulationStatus::Success();
}

} // namespace pbmodulation
