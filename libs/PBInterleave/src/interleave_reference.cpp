#include "pbinterleave/interleave_reference.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

namespace pbinterleave {

namespace {

[[nodiscard]] bool SpansOverlap(
    const std::span<const std::byte> source,
    const std::span<std::byte> destination) noexcept
{
    if (source.empty() || destination.empty())
    {
        return false;
    }
    const std::byte* const sourceBegin = source.data();
    const std::byte* const sourceEnd = sourceBegin + source.size();
    const std::byte* const destinationBegin = destination.data();
    const std::byte* const destinationEnd =
        destinationBegin + destination.size();
    const std::less<const std::byte*> addressLess;
    return addressLess(sourceBegin, destinationEnd) &&
        addressLess(destinationBegin, sourceEnd);
}

// Writes one four-bit tile nibble at the tile position implied by tileIndex
// (even tile -> low nibble, odd tile -> high nibble).
void WriteTileNibble(
    const std::span<std::byte> region,
    const std::uint64_t tileIndex,
    const std::uint8_t nibble) noexcept
{
    const std::size_t byteIndex = static_cast<std::size_t>(tileIndex / 2);
    const std::uint8_t current =
        static_cast<std::uint8_t>(region[byteIndex]);
    const std::uint8_t masked =
        static_cast<std::uint8_t>(
            (tileIndex % 2 == 0) ? (current & 0xF0U) : (current & 0x0FU));
    region[byteIndex] = static_cast<std::byte>(
        masked | static_cast<std::uint8_t>(nibble << ((tileIndex % 2) * 4)));
}

[[nodiscard]] std::uint8_t ReadTileNibble(
    const std::span<const std::byte> region,
    const std::uint64_t tileIndex) noexcept
{
    const std::size_t byteIndex = static_cast<std::size_t>(tileIndex / 2);
    const std::uint8_t current =
        static_cast<std::uint8_t>(region[byteIndex]);
    return static_cast<std::uint8_t>(
        (current >> ((tileIndex % 2) * 4)) & 0x0FU);
}

} // namespace

InterleaveStatus ApplyInterleave(
    const std::span<const std::byte> logical,
    const std::span<std::byte> physical,
    const std::uint64_t frameSequence) noexcept
{
    if (logical.size() != kInterleaveRegionBytes)
    {
        return InterleaveStatus::Failure(
            InterleaveErrorCode::InvalidInput,
            static_cast<std::uint64_t>(logical.size()));
    }
    if (physical.size() != kInterleaveRegionBytes)
    {
        return InterleaveStatus::Failure(
            InterleaveErrorCode::InvalidInput,
            static_cast<std::uint64_t>(physical.size()));
    }
    if (SpansOverlap(logical, physical))
    {
        return InterleaveStatus::Failure(
            InterleaveErrorCode::OverlappingSpans,
            kInterleaveRegionBytes);
    }

    for (std::uint64_t logicalTile = 0; logicalTile < kInterleaveTileCount;
         logicalTile++)
    {
        const std::uint8_t nibble = ReadTileNibble(logical, logicalTile);
        WriteTileNibble(
            physical,
            MapLogicalTileToPhysical(logicalTile, frameSequence),
            nibble);
    }
    return InterleaveStatus::Success();
}

InterleaveStatus ReverseInterleave(
    const std::span<const std::byte> physical,
    const std::span<std::byte> logical,
    const std::uint64_t frameSequence) noexcept
{
    if (physical.size() != kInterleaveRegionBytes)
    {
        return InterleaveStatus::Failure(
            InterleaveErrorCode::InvalidInput,
            static_cast<std::uint64_t>(physical.size()));
    }
    if (logical.size() != kInterleaveRegionBytes)
    {
        return InterleaveStatus::Failure(
            InterleaveErrorCode::InvalidInput,
            static_cast<std::uint64_t>(logical.size()));
    }
    if (SpansOverlap(physical, logical))
    {
        return InterleaveStatus::Failure(
            InterleaveErrorCode::OverlappingSpans,
            kInterleaveRegionBytes);
    }

    for (std::uint64_t physicalTile = 0; physicalTile < kInterleaveTileCount;
         physicalTile++)
    {
        const std::uint8_t nibble = ReadTileNibble(physical, physicalTile);
        WriteTileNibble(
            logical,
            MapPhysicalTileToLogical(physicalTile, frameSequence),
            nibble);
    }
    return InterleaveStatus::Success();
}

} // namespace pbinterleave
