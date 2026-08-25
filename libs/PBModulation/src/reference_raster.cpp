#include "pbmodulation/reference_raster.h"

#include "raster_internal.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <vector>

namespace pbmodulation {
namespace {

using detail::LoadPixel;
using detail::NearestLevelIndex;
using detail::Pixel;
using detail::PixelIndex;
using detail::SetSymbolNibble;
using detail::StorePixel;

[[nodiscard]] std::uint32_t LaneSymbolsPerRow(
    const ReferenceRegion& region) noexcept
{
    return region.width / kReferenceLaneSymbolWidth;
}

[[nodiscard]] std::size_t LaneSymbolCount(
    const ReferenceRegion& region) noexcept
{
    return static_cast<std::size_t>(LaneSymbolsPerRow(region)) *
        (region.height / kReferenceLaneSymbolHeight);
}

[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> LaneSymbolOrigin(
    const ReferenceRegion& region,
    const std::size_t laneSymbolIndex) noexcept
{
    const std::uint32_t columns = LaneSymbolsPerRow(region);
    return {
        region.x + static_cast<std::uint32_t>(laneSymbolIndex % columns) *
            kReferenceLaneSymbolWidth,
        region.y + static_cast<std::uint32_t>(laneSymbolIndex / columns) *
            kReferenceLaneSymbolHeight};
}

[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> DataTileOrigin(
    const ReferenceRegion& region,
    const std::size_t tileIndex) noexcept
{
    return {
        region.x +
        static_cast<std::uint32_t>(tileIndex % kReferenceDataGridColumns) *
            kReferenceDataTileWidth,
        region.y +
        static_cast<std::uint32_t>(tileIndex / kReferenceDataGridColumns) *
            kReferenceDataTileHeight};
}

void FillSolidBlock(
    const std::span<std::byte> bgra,
    const std::uint32_t originX,
    const std::uint32_t originY,
    const std::uint32_t blockWidth,
    const std::uint32_t blockHeight,
    const Pixel pixel) noexcept
{
    for (std::uint32_t row = 0; row < blockHeight; row++)
    {
        for (std::uint32_t col = 0; col < blockWidth; col++)
        {
            StorePixel(bgra, PixelIndex(originX + col, originY + row), pixel);
        }
    }
}

void FillRegionContent(
    const std::span<std::byte> bgra,
    const ReferenceRegion& region,
    const Pixel fixedPixel) noexcept
{
    for (std::uint32_t row = 0; row < region.height; row++)
    {
        for (std::uint32_t col = 0; col < region.width; col++)
        {
            StorePixel(bgra, PixelIndex(region.x + col, region.y + row),
                fixedPixel);
        }
    }
}

void FillFrozenRegion(
    const std::span<std::byte> bgra,
    const ReferenceRegion& region) noexcept
{
    if (region.type == ReferenceRegionType::Guard)
    {
        FillRegionContent(bgra, region, Pixel{0, 0, 0, kReferenceAlphaValue});
        return;
    }
    for (std::uint32_t row = 0; row < region.height; row++)
    {
        for (std::uint32_t col = 0; col < region.width; col++)
        {
            StorePixel(
                bgra,
                PixelIndex(region.x + col, region.y + row),
                detail::ExpectedFrozenPixel(region, region.x + col,
                    region.y + row));
        }
    }
}

// Writes a lane of symbols (row-major). The record symbols [0..recordCount)
// of the stream occupy lane positions [laneSymbolOffset..), and every other
// lane position carries the reserved symbol 0.
void FillLaneSymbols(
    const std::span<std::byte> bgra,
    const ReferenceRegion& region,
    const std::span<const std::byte> stream,
    const std::size_t laneSymbolOffset) noexcept
{
    const std::size_t totalSymbols = LaneSymbolCount(region);
    const std::size_t recordCount = stream.size() * 2;
    for (std::size_t laneSymbolIndex = 0;
        laneSymbolIndex < totalSymbols; laneSymbolIndex++)
    {
        const std::uint8_t symbol =
            laneSymbolIndex >= laneSymbolOffset &&
            laneSymbolIndex - laneSymbolOffset < recordCount
                ? detail::GetSymbolNibble(
                    stream, laneSymbolIndex - laneSymbolOffset)
                : 0;
        const std::uint8_t level = detail::kLevelValues[GrayCode4(symbol)];
        const Pixel pixel{level, level, level, kReferenceAlphaValue};
        const auto [originX, originY] =
            LaneSymbolOrigin(region, laneSymbolIndex);
        FillSolidBlock(bgra, originX, originY, kReferenceLaneSymbolWidth,
            kReferenceLaneSymbolHeight, pixel);
    }
}

void FillDataTiles(
    const std::span<std::byte> bgra,
    const ReferenceRegion& region,
    const std::span<const std::byte> stream) noexcept
{
    const std::size_t tileCount = stream.size() * 2;
    for (std::size_t tileIndex = 0; tileIndex < tileCount; tileIndex++)
    {
        const std::uint8_t level =
            detail::kLevelValues[GrayCode4(
                detail::GetSymbolNibble(stream, tileIndex))];
        const Pixel pixel{level, level, level, kReferenceAlphaValue};
        const auto [originX, originY] = DataTileOrigin(region, tileIndex);
        FillSolidBlock(bgra, originX, originY, kReferenceDataTileWidth,
            kReferenceDataTileHeight, pixel);
    }
}

// Demodulates one 16-pixel solid block to a 4-bit symbol. Every pixel is
// checked against the frozen contracts (alpha, luma-only, constellation
// margin) before the block mean is reduced; on failure the error offset is
// the first violating pixel (for level decisions: the block origin).
[[nodiscard]] ModulationStatus DemodSolidBlock(
    const std::span<const std::byte> bgra,
    const std::uint32_t originX,
    const std::uint32_t originY,
    const std::uint32_t blockWidth,
    const std::uint32_t blockHeight,
    const std::size_t blockOriginPixel,
    std::uint8_t& outSymbol) noexcept
{
    std::uint32_t lumaSum = 0;
    for (std::uint32_t row = 0; row < blockHeight; row++)
    {
        for (std::uint32_t col = 0; col < blockWidth; col++)
        {
            const std::size_t pixelIndex =
                PixelIndex(originX + col, originY + row);
            const Pixel pixel = LoadPixel(bgra, pixelIndex);
            if (pixel.a != kReferenceAlphaValue)
            {
                return ModulationStatus::Failure(
                    ModulationErrorCode::AlphaChannelViolation,
                    pixelIndex);
            }
            if (pixel.b != pixel.g || pixel.g != pixel.r)
            {
                return ModulationStatus::Failure(
                    ModulationErrorCode::ChromaChannelMismatch,
                    pixelIndex);
            }
            lumaSum += pixel.b;
        }
    }
    const std::uint32_t meanLuma =
        lumaSum / (blockWidth * blockHeight);
    std::uint8_t levelIndex = 0;
    const auto levelResult = NearestLevelIndex(meanLuma, blockOriginPixel,
        levelIndex);
    if (!levelResult)
    {
        return levelResult;
    }
    outSymbol = GrayDecode4(levelIndex);
    return ModulationStatus::Success();
}

// Demodulates every symbol of a lane. The record symbols at lane positions
// [laneSymbolOffset..laneSymbolOffset+recordCount) fill outStream (which
// holds exactly recordCount symbols); every other lane position is reserved
// and must carry the frozen symbol 0, otherwise the frame is rejected with
// NonZeroReservedByte. This keeps the reference demod a strict inverse of
// the canonical encoder: every canvas pixel is verified by exactly one
// rule (byte-exact frozen check or symbol demodulation).
[[nodiscard]] ModulationStatus DemodLaneSymbols(
    const std::span<const std::byte> bgra,
    const ReferenceRegion& region,
    const std::span<std::byte> outStream,
    const std::size_t laneSymbolOffset) noexcept
{
    const std::size_t totalSymbols = LaneSymbolCount(region);
    const std::size_t recordCount = outStream.size() * 2;
    if (laneSymbolOffset + recordCount > totalSymbols)
    {
        return ModulationStatus::Failure(
            ModulationErrorCode::InternalInvariantViolation,
            0);
    }
    for (std::size_t laneSymbolIndex = 0;
        laneSymbolIndex < totalSymbols; laneSymbolIndex++)
    {
        const auto [originX, originY] =
            LaneSymbolOrigin(region, laneSymbolIndex);
        std::uint8_t symbol = 0;
        const auto symbolResult = DemodSolidBlock(
            bgra,
            originX,
            originY,
            kReferenceLaneSymbolWidth,
            kReferenceLaneSymbolHeight,
            PixelIndex(originX, originY),
            symbol);
        if (!symbolResult)
        {
            return symbolResult;
        }
        if (laneSymbolIndex >= laneSymbolOffset &&
            laneSymbolIndex - laneSymbolOffset < recordCount)
        {
            SetSymbolNibble(
                outStream, laneSymbolIndex - laneSymbolOffset, symbol);
        }
        else if (symbol != 0)
        {
            return ModulationStatus::Failure(
                ModulationErrorCode::NonZeroReservedByte,
                PixelIndex(originX, originY));
        }
    }
    return ModulationStatus::Success();
}

[[nodiscard]] ModulationStatus DemodDataTiles(
    const std::span<const std::byte> bgra,
    const ReferenceRegion& region,
    const std::span<std::byte> outStream) noexcept
{
    const std::size_t tileCount = outStream.size() * 2;
    for (std::size_t tileIndex = 0; tileIndex < tileCount; tileIndex++)
    {
        const auto [originX, originY] = DataTileOrigin(region, tileIndex);
        std::uint8_t symbol = 0;
        const auto symbolResult = DemodSolidBlock(
            bgra,
            originX,
            originY,
            kReferenceDataTileWidth,
            kReferenceDataTileHeight,
            PixelIndex(originX, originY),
            symbol);
        if (!symbolResult)
        {
            return symbolResult;
        }
        SetSymbolNibble(outStream, tileIndex, symbol);
    }
    return ModulationStatus::Success();
}

[[nodiscard]] bool IsFrozenRegion(
    const ReferenceRegionType type) noexcept
{
    return type == ReferenceRegionType::Guard ||
        type == ReferenceRegionType::Sync ||
        type == ReferenceRegionType::Pilot;
}

[[nodiscard]] ModulationStatus VerifyFrozenRegions(
    const std::span<const std::byte> bgra) noexcept
{
    for (const ReferenceRegion& region : kReferenceRegions)
    {
        if (!IsFrozenRegion(region.type))
        {
            continue;
        }
        for (std::uint32_t row = 0; row < region.height; row++)
        {
            for (std::uint32_t col = 0; col < region.width; col++)
            {
                const std::uint32_t x = region.x + col;
                const std::uint32_t y = region.y + row;
                const std::size_t pixelIndex = PixelIndex(x, y);
                if (LoadPixel(bgra, pixelIndex) !=
                    detail::ExpectedFrozenPixel(region, x, y))
                {
                    return ModulationStatus::Failure(
                        ModulationErrorCode::FrozenRegionMismatch,
                        pixelIndex);
                }
            }
        }
    }
    return ModulationStatus::Success();
}

} // namespace

ModulationStatus EncodeReferenceFrame(
    const ReferenceFrameInput& input,
    const std::span<std::byte> outBgra) noexcept
{
    if (outBgra.size() != kReferenceFrameBgraBytes)
    {
        return ModulationStatus::Failure(
            ModulationErrorCode::OutputBufferTooSmall,
            0);
    }
    if (input.controlWindow.size() != kReferenceControlWindowBytes ||
        input.data.size() != kReferenceDataRegionBytes)
    {
        return ModulationStatus::Failure(
            ModulationErrorCode::InvalidInput,
            0);
    }

    // The region fills below overwrite every pixel exactly once; the initial
    // zero fill keeps the output deterministic under any future refactor.
    std::fill(outBgra.begin(), outBgra.end(), std::byte{0});

    for (const ReferenceRegion& region : kReferenceRegions)
    {
        switch (region.type)
        {
            case ReferenceRegionType::Guard:
            case ReferenceRegionType::Sync:
            case ReferenceRegionType::Pilot:
                FillFrozenRegion(outBgra, region);
                break;
            case ReferenceRegionType::BootstrapA:
                FillLaneSymbols(outBgra, region,
                    std::span<const std::byte>(input.bootstrapRecord),
                    kReferenceBootstrapASymbolOffset);
                break;
            case ReferenceRegionType::BootstrapB:
                FillLaneSymbols(outBgra, region,
                    std::span<const std::byte>(input.bootstrapRecord),
                    kReferenceBootstrapBSymbolOffset);
                break;
            case ReferenceRegionType::Control:
                FillLaneSymbols(outBgra, region, input.controlWindow, 0);
                break;
            case ReferenceRegionType::DataGrid:
                FillDataTiles(outBgra, region, input.data);
                break;
        }
    }
    return ModulationStatus::Success();
}

ModulationStatus DecodeReferenceFrameInto(
    const std::span<const std::byte> bgra,
    const std::span<std::byte> outBootstrap,
    const std::span<std::byte> outControl,
    const std::span<std::byte> outData) noexcept
{
    if (bgra.size() != kReferenceFrameBgraBytes)
    {
        return ModulationStatus::Failure(
            ModulationErrorCode::InvalidInput,
            0);
    }
    if (outBootstrap.size() != kReferenceBootstrapRecordBytes ||
        outControl.size() != kReferenceControlWindowBytes ||
        outData.size() != kReferenceDataRegionBytes)
    {
        return ModulationStatus::Failure(
            ModulationErrorCode::OutputBufferTooSmall,
            0);
    }

    // Pass 1: byte-exact verification of every frozen region.
    const auto frozenResult = VerifyFrozenRegions(bgra);
    if (!frozenResult)
    {
        return frozenResult;
    }

    // Pass 2: locate the data-carrying regions in the frozen table.
    const ReferenceRegion* bootstrapARegion = nullptr;
    const ReferenceRegion* bootstrapBRegion = nullptr;
    const ReferenceRegion* controlRegion = nullptr;
    const ReferenceRegion* dataRegion = nullptr;
    for (const ReferenceRegion& region : kReferenceRegions)
    {
        switch (region.type)
        {
            case ReferenceRegionType::BootstrapA:
                bootstrapARegion = &region;
                break;
            case ReferenceRegionType::BootstrapB:
                bootstrapBRegion = &region;
                break;
            case ReferenceRegionType::Control:
                controlRegion = &region;
                break;
            case ReferenceRegionType::DataGrid:
                dataRegion = &region;
                break;
            default:
                break;
        }
    }
    // The canonical table always contains all four regions; reaching this
    // branch means the frozen table itself was modified.
    if (bootstrapARegion == nullptr || bootstrapBRegion == nullptr ||
        controlRegion == nullptr || dataRegion == nullptr)
    {
        return ModulationStatus::Failure(
            ModulationErrorCode::InternalInvariantViolation,
            0);
    }

    // Pass 3: demodulate the data-carrying regions into local storage and
    // publish to the caller's spans only after every region has demodulated
    // cleanly. The documented contract is that a failed frame leaves all
    // three outputs unmodified (whole-frame erasure, design 4.3); writing
    // incrementally into the caller buffers would leak partially demodulated
    // lanes when a late symbol fails. The local storage costs roughly 56 KiB
    // of stack, which is acceptable for this CPU reference path whose
    // callers already hold the full 8.3 MiB frame buffer.
    std::array<std::byte, kReferenceBootstrapRecordBytes> bootstrapA{};
    std::array<std::byte, kReferenceBootstrapRecordBytes> bootstrapB{};
    std::array<std::byte, kReferenceControlWindowBytes> control{};
    std::array<std::byte, kReferenceDataRegionBytes> data{};

    const auto decodeA = DemodLaneSymbols(bgra, *bootstrapARegion,
        std::span<std::byte>(bootstrapA),
        kReferenceBootstrapASymbolOffset);
    if (!decodeA)
    {
        return decodeA;
    }
    const auto decodeB = DemodLaneSymbols(bgra, *bootstrapBRegion,
        std::span<std::byte>(bootstrapB),
        kReferenceBootstrapBSymbolOffset);
    if (!decodeB)
    {
        return decodeB;
    }
    // Torn-frame detection (design 16.4): the two spatially separated
    // bootstrap copies must agree byte-for-byte, otherwise the whole frame
    // is rejected as an erasure.
    if (bootstrapA != bootstrapB)
    {
        const auto [originX, originY] = LaneSymbolOrigin(
            *bootstrapBRegion,
            kReferenceBootstrapBSymbolOffset);
        return ModulationStatus::Failure(
            ModulationErrorCode::TornFrame,
            PixelIndex(originX, originY));
    }
    const auto decodeControl = DemodLaneSymbols(bgra, *controlRegion,
        std::span<std::byte>(control), 0);
    if (!decodeControl)
    {
        return decodeControl;
    }
    const auto decodeData = DemodDataTiles(bgra, *dataRegion,
        std::span<std::byte>(data));
    if (!decodeData)
    {
        return decodeData;
    }

    std::copy(bootstrapA.begin(), bootstrapA.end(), outBootstrap.begin());
    std::copy(control.begin(), control.end(), outControl.begin());
    std::copy(data.begin(), data.end(), outData.begin());
    return ModulationStatus::Success();
}

ModulationResult<DecodedReferenceFrame> DecodeReferenceFrame(
    const std::span<const std::byte> bgra) noexcept
{
    if (bgra.size() != kReferenceFrameBgraBytes)
    {
        return ModulationResult<DecodedReferenceFrame>::Failure(
            ModulationErrorCode::InvalidInput,
            0);
    }

    DecodedReferenceFrame decoded;
    try
    {
        decoded.data.resize(kReferenceDataRegionBytes);
    }
    catch (const std::bad_alloc&)
    {
        // The entry point is noexcept: an allocation failure must be
        // reported as a fail-closed error, never terminate the process.
        return ModulationResult<DecodedReferenceFrame>::Failure(
            ModulationErrorCode::MemoryAllocationFailure,
            0);
    }
    const auto status = DecodeReferenceFrameInto(
        bgra,
        std::span<std::byte>(decoded.bootstrapRecord),
        std::span<std::byte>(decoded.controlWindow),
        std::span<std::byte>(decoded.data));
    if (!status)
    {
        return ModulationResult<DecodedReferenceFrame>::Failure(
            status.Error().code,
            status.Error().offset);
    }
    return ModulationResult<DecodedReferenceFrame>::Success(
        std::move(decoded));
}

} // namespace pbmodulation