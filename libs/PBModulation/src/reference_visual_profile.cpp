#include "pbmodulation/reference_visual_profile.h"

#include "pbprotocol/byte_io.h"
#include "pbprotocol/crc32c.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace pbmodulation {
namespace {

using pbprotocol::ByteReader;
using pbprotocol::ByteWriter;

constexpr std::size_t kManifestCrcOffset = kReferenceManifestBytes - 4;

// Validates the frozen partition invariants on an explicit region table:
// every rectangle lies inside the canvas, no two rectangles overlap, and
// the total area equals the canvas area (which together with the bounds
// check proves an exact partition).
[[nodiscard]] bool ValidateRegionPartition(
    const std::span<const ReferenceRegion> regions) noexcept
{
    for (const ReferenceRegion& region : regions)
    {
        if (region.width == 0 || region.height == 0)
        {
            return false;
        }
        // 64-bit bounds arithmetic: a hostile (x, width) pair must not be
        // able to wrap the 32-bit comparison and pass the canvas bounds.
        const std::uint64_t regionRightEdge =
            static_cast<std::uint64_t>(region.x) + region.width;
        const std::uint64_t regionBottomEdge =
            static_cast<std::uint64_t>(region.y) + region.height;
        if (regionRightEdge > kReferenceCanvasWidth ||
            regionBottomEdge > kReferenceCanvasHeight)
        {
            return false;
        }
    }

    std::uint64_t totalArea = 0;
    for (const ReferenceRegion& region : regions)
    {
        totalArea +=
            static_cast<std::uint64_t>(region.width) * region.height;
        for (const ReferenceRegion& other : regions)
        {
            if (&other == &region)
            {
                continue;
            }
            const std::uint32_t overlapX1 =
                std::min(region.x + region.width, other.x + other.width);
            const std::uint32_t overlapX2 = std::max(region.x, other.x);
            const std::uint32_t overlapY1 =
                std::min(region.y + region.height, other.y + other.height);
            const std::uint32_t overlapY2 = std::max(region.y, other.y);
            if (overlapX2 < overlapX1 && overlapY2 < overlapY1)
            {
                return false;
            }
        }
    }
    return totalArea ==
        static_cast<std::uint64_t>(kReferenceCanvasWidth) *
        kReferenceCanvasHeight;
}

} // namespace

std::array<std::byte, kReferenceManifestBytes>
    SerializeReferenceRegionManifest() noexcept
{
    std::array<std::byte, kReferenceManifestBytes> bytes{};
    std::span<std::byte> manifestBytes{bytes};
    ByteWriter writer(manifestBytes);
    pbprotocol::ProtocolStatus status =
        writer.WriteFixedBytes(kReferenceManifestMagic);
    status = writer.WriteUint8(kReferenceManifestVersion);
    status = writer.WriteUint8(kReferenceManifestLayoutVersion);
    status = writer.WriteUint16(0);
    status = writer.WriteUint32(kReferenceCanvasWidth);
    status = writer.WriteUint32(kReferenceCanvasHeight);
    status = writer.WriteUint8(
        static_cast<std::uint8_t>(kReferenceRegions.size()));
    status = writer.WriteUint8(kReferenceBitsPerSymbol);
    status = writer.WriteUint8(kReferenceLevelCount);
    status = writer.WriteUint8(kReferenceLevelBase);
    status = writer.WriteUint8(kReferenceLevelStep);
    status = writer.WriteUint8(kReferenceDataTileWidth);
    status = writer.WriteUint8(kReferenceDataTileHeight);
    status = writer.WriteUint8(kReferenceLaneSymbolWidth);
    status = writer.WriteUint8(kReferenceLaneSymbolHeight);
    std::array<std::byte, 8> reserved{};
    status = writer.WriteFixedBytes(reserved);
    for (const ReferenceRegion& region : kReferenceRegions)
    {
        status = writer.WriteUint8(
            static_cast<std::uint8_t>(region.type));
        status = writer.WriteUint32(region.x);
        status = writer.WriteUint32(region.y);
        status = writer.WriteUint32(region.width);
        status = writer.WriteUint32(region.height);
    }
    // Defensive internal-invariant check: the layout is fixed-size, so every
    // write must have succeeded and landed exactly on the CRC offset.
    if (!status || writer.Position() != kManifestCrcOffset)
    {
        return {};
    }
    const std::uint32_t crc32c = pbprotocol::ComputeCrc32c(
        std::span<const std::byte>(bytes).first(kManifestCrcOffset));
    if (!writer.WriteUint32(crc32c) || writer.Position() != kReferenceManifestBytes)
    {
        return {};
    }
    return bytes;
}

ModulationResult<ReferenceRegionManifest> ParseReferenceRegionManifest(
    const std::span<const std::byte> input) noexcept
{
    if (input.size() != kReferenceManifestBytes)
    {
        return ModulationResult<ReferenceRegionManifest>::Failure(
            ModulationErrorCode::InvalidInput,
            0);
    }

    ByteReader reader(input);
    const auto magicResult = reader.ReadFixedBytes<4>();
    if (!magicResult || magicResult.Value() != kReferenceManifestMagic)
    {
        return ModulationResult<ReferenceRegionManifest>::Failure(
            ModulationErrorCode::InvalidMagic,
            0);
    }
    const auto versionResult = reader.ReadUint8();
    if (!versionResult ||
        versionResult.Value() != kReferenceManifestVersion)
    {
        return ModulationResult<ReferenceRegionManifest>::Failure(
            ModulationErrorCode::UnsupportedVersion,
            4);
    }
    const auto layoutResult = reader.ReadUint8();
    if (!layoutResult ||
        layoutResult.Value() != kReferenceManifestLayoutVersion)
    {
        return ModulationResult<ReferenceRegionManifest>::Failure(
            ModulationErrorCode::UnsupportedVersion,
            5);
    }
    const auto reservedResult = reader.ReadUint16();
    if (!reservedResult || reservedResult.Value() != 0)
    {
        return ModulationResult<ReferenceRegionManifest>::Failure(
            ModulationErrorCode::NonZeroReservedByte,
            6);
    }

    ReferenceRegionManifest manifest;
    const auto canvasWidthResult = reader.ReadUint32();
    if (!canvasWidthResult)
    {
        return ModulationResult<ReferenceRegionManifest>::Failure(
            ModulationErrorCode::TruncatedInput,
            canvasWidthResult.Error().offset);
    }
    manifest.canvasWidth = canvasWidthResult.Value();
    const auto canvasHeightResult = reader.ReadUint32();
    if (!canvasHeightResult)
    {
        return ModulationResult<ReferenceRegionManifest>::Failure(
            ModulationErrorCode::TruncatedInput,
            canvasHeightResult.Error().offset);
    }
    manifest.canvasHeight = canvasHeightResult.Value();
    if (manifest.canvasWidth != kReferenceCanvasWidth ||
        manifest.canvasHeight != kReferenceCanvasHeight)
    {
        return ModulationResult<ReferenceRegionManifest>::Failure(
            ModulationErrorCode::ManifestValidationFailed,
            8);
    }

    const auto regionCountResult = reader.ReadUint8();
    if (!regionCountResult ||
        regionCountResult.Value() != kReferenceRegions.size())
    {
        return ModulationResult<ReferenceRegionManifest>::Failure(
            ModulationErrorCode::ManifestValidationFailed,
            16);
    }
    const auto parameterCheck = [&reader]()
    {
        const auto bitsResult = reader.ReadUint8();
        if (!bitsResult ||
            bitsResult.Value() != kReferenceBitsPerSymbol)
        {
            return false;
        }
        const auto levelCountResult = reader.ReadUint8();
        if (!levelCountResult ||
            levelCountResult.Value() != kReferenceLevelCount)
        {
            return false;
        }
        const auto levelBaseResult = reader.ReadUint8();
        if (!levelBaseResult ||
            levelBaseResult.Value() != kReferenceLevelBase)
        {
            return false;
        }
        const auto levelStepResult = reader.ReadUint8();
        if (!levelStepResult ||
            levelStepResult.Value() != kReferenceLevelStep)
        {
            return false;
        }
        const auto tileWidthResult = reader.ReadUint8();
        if (!tileWidthResult ||
            tileWidthResult.Value() != kReferenceDataTileWidth)
        {
            return false;
        }
        const auto tileHeightResult = reader.ReadUint8();
        if (!tileHeightResult ||
            tileHeightResult.Value() != kReferenceDataTileHeight)
        {
            return false;
        }
        const auto symbolWidthResult = reader.ReadUint8();
        if (!symbolWidthResult ||
            symbolWidthResult.Value() != kReferenceLaneSymbolWidth)
        {
            return false;
        }
        const auto symbolHeightResult = reader.ReadUint8();
        if (!symbolHeightResult ||
            symbolHeightResult.Value() != kReferenceLaneSymbolHeight)
        {
            return false;
        }
        std::array<std::byte, 8> reserved{};
        const auto reservedBytesResult =
            reader.ReadFixedBytes<8>();
        if (!reservedBytesResult)
        {
            return false;
        }
        reserved = reservedBytesResult.Value();
        return std::all_of(
            reserved.begin(), reserved.end(), [](const std::byte value)
            { return value == std::byte{0}; });
    }();
    if (!parameterCheck)
    {
        return ModulationResult<ReferenceRegionManifest>::Failure(
            ModulationErrorCode::ManifestValidationFailed,
            17);
    }
    manifest.bitsPerSymbol = kReferenceBitsPerSymbol;
    manifest.levelCount = kReferenceLevelCount;
    manifest.levelBase = kReferenceLevelBase;
    manifest.levelStep = kReferenceLevelStep;
    manifest.dataTileWidth = kReferenceDataTileWidth;
    manifest.dataTileHeight = kReferenceDataTileHeight;
    manifest.laneSymbolWidth = kReferenceLaneSymbolWidth;
    manifest.laneSymbolHeight = kReferenceLaneSymbolHeight;

    for (std::size_t regionIndex = 0;
        regionIndex < kReferenceRegions.size(); regionIndex++)
    {
        const auto typeResult = reader.ReadUint8();
        if (!typeResult)
        {
            return ModulationResult<ReferenceRegionManifest>::Failure(
                ModulationErrorCode::TruncatedInput,
                typeResult.Error().offset);
        }
        if (typeResult.Value() >
            static_cast<std::uint8_t>(ReferenceRegionType::BootstrapB))
        {
            return ModulationResult<ReferenceRegionManifest>::Failure(
                ModulationErrorCode::ManifestValidationFailed,
                typeResult.Error().offset);
        }
        const auto xResult = reader.ReadUint32();
        const auto yResult = reader.ReadUint32();
        const auto widthResult = reader.ReadUint32();
        const auto heightResult = reader.ReadUint32();
        if (!xResult || !yResult || !widthResult || !heightResult)
        {
            const auto firstFailure =
                !xResult ? xResult
                : (!yResult ? yResult
                : (!widthResult ? widthResult : heightResult));
            return ModulationResult<ReferenceRegionManifest>::Failure(
                ModulationErrorCode::TruncatedInput,
                firstFailure.Error().offset);
        }
        manifest.regions[regionIndex] = ReferenceRegion{
            static_cast<ReferenceRegionType>(typeResult.Value()),
            xResult.Value(),
            yResult.Value(),
            widthResult.Value(),
            heightResult.Value()};
    }
    // The reference manifest is a single frozen object: the parsed table
    // must equal the canonical table exactly.
    if (manifest.regions != kReferenceRegions)
    {
        return ModulationResult<ReferenceRegionManifest>::Failure(
            ModulationErrorCode::ManifestValidationFailed,
            33);
    }

    const auto storedCrcResult = reader.ReadUint32();
    if (!storedCrcResult)
    {
        return ModulationResult<ReferenceRegionManifest>::Failure(
            ModulationErrorCode::TruncatedInput,
            storedCrcResult.Error().offset);
    }
    const std::uint32_t computedCrc = pbprotocol::ComputeCrc32c(
        std::span<const std::byte>(input).first(kManifestCrcOffset));
    if (storedCrcResult.Value() != computedCrc)
    {
        return ModulationResult<ReferenceRegionManifest>::Failure(
            ModulationErrorCode::CrcMismatch,
            kManifestCrcOffset);
    }
    if (!ValidateRegionPartition(manifest.regions))
    {
        return ModulationResult<ReferenceRegionManifest>::Failure(
            ModulationErrorCode::ManifestValidationFailed,
            0);
    }
    return ModulationResult<ReferenceRegionManifest>::Success(
        std::move(manifest));
}

bool ValidateReferenceVisualProfile() noexcept
{
    if (!ValidateRegionPartition(kReferenceRegions))
    {
        return false;
    }
    // Constellation table must match the affine definition.
    for (std::uint8_t levelIndex = 0;
        levelIndex < kReferenceLevelCount; levelIndex++)
    {
        if (GetReferenceLevelValue(levelIndex) !=
            static_cast<std::uint8_t>(kReferenceLevelBase +
            kReferenceLevelStep * levelIndex))
        {
            return false;
        }
        if (GrayDecode4(GrayCode4(levelIndex)) != levelIndex)
        {
            return false;
        }
    }
    // The canonical manifest must round-trip through the parser.
    const std::array<std::byte, kReferenceManifestBytes> manifest =
        SerializeReferenceRegionManifest();
    const auto parsedResult = ParseReferenceRegionManifest(
        std::span<const std::byte>(manifest));
    if (!parsedResult)
    {
        return false;
    }
    return true;
}

} // namespace pbmodulation