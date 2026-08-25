// PB-InterleaveReference-1 boundary matrix tests (task T2).
//
// Covers: frozen-constant proofs (including the u64 intermediate bound and
// the multiplier-inverse uniqueness), the full-table bijection for all 16
// phases (dedup + both composition identities), phase/period boundaries on
// extreme FrameSequence values, pairwise phase distinctness at the region
// digest level, directed single-nibble tile patterns at the region edges,
// the region content matrix with Apply/Reverse round-trips, the exact
// span-size contract, and overlap rejection with the no-write-on-failure
// guarantee. The tile/region sizes are cross-checked against the frozen
// pbmodulation reference raster constants.

#include "pbinterleave/interleave_reference.h"
#include "pbmodulation/reference_visual_profile.h"
#include "pbprotocol/blake3_digest.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace {

using pbinterleave::ApplyInterleave;
using pbinterleave::GetInterleavePhase;
using pbinterleave::InterleaveErrorCode;
using pbinterleave::kInterleaveMultiplier;
using pbinterleave::kInterleaveMultiplierInverse;
using pbinterleave::kInterleavePhaseCount;
using pbinterleave::kInterleavePhaseStepTiles;
using pbinterleave::kInterleaveProfileIdReferenceV1;
using pbinterleave::kInterleaveRegionBytes;
using pbinterleave::kInterleaveTileCount;
using pbinterleave::MapLogicalTileToPhysical;
using pbinterleave::MapPhysicalTileToLogical;
using pbinterleave::ReverseInterleave;

// Deterministic PRNG (splitmix64); identical formula to the PBInnerFec and
// PBModulation test helpers so shared patterns stay byte-compatible.
class SplitMix64
{
public:
    explicit SplitMix64(const std::uint64_t seed) noexcept
        : state_(seed)
    {
    }

    [[nodiscard]] std::uint64_t Next() noexcept
    {
        state_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

private:
    std::uint64_t state_;
};

[[nodiscard]] constexpr std::uint64_t Gcd(
    const std::uint64_t a, const std::uint64_t b) noexcept
{
    std::uint64_t x = a;
    std::uint64_t y = b;
    while (y != 0)
    {
        const std::uint64_t remainder = x % y;
        x = y;
        y = remainder;
    }
    return x;
}

[[nodiscard]] std::vector<std::byte> MakeFilledRegion(
    const std::uint8_t value)
{
    return std::vector<std::byte>(
        kInterleaveRegionBytes, std::byte{value});
}

[[nodiscard]] std::vector<std::byte> MakeIncrementRegion()
{
    std::vector<std::byte> region(kInterleaveRegionBytes);
    for (std::size_t i = 0; i < region.size(); i++)
    {
        region[i] = static_cast<std::byte>(static_cast<std::uint8_t>(i % 256U));
    }
    return region;
}

[[nodiscard]] std::vector<std::byte> MakeSplitMixRegion(
    const std::uint64_t seed)
{
    SplitMix64 rng(seed);
    std::vector<std::byte> region(kInterleaveRegionBytes);
    for (std::size_t i = 0; i < region.size(); i++)
    {
        region[i] =
            static_cast<std::byte>(static_cast<std::uint8_t>(rng.Next() & 0xFFU));
    }
    return region;
}

// Sets the four-bit tile at `tileIndex` (even -> low nibble, odd -> high
// nibble) to `tileValue` in a zero-filled region.
[[nodiscard]] std::vector<std::byte> MakeDirectedTileRegion(
    const std::uint64_t tileIndex, const std::uint8_t tileValue)
{
    std::vector<std::byte> region(kInterleaveRegionBytes);
    const std::size_t byteIndex = static_cast<std::size_t>(tileIndex / 2U);
    const std::uint8_t value =
        std::to_integer<std::uint8_t>(region[byteIndex]);
    if ((tileIndex % 2U) == 0U)
    {
        region[byteIndex] = static_cast<std::byte>(
            static_cast<std::uint8_t>((value & 0xF0U) |
                (tileValue & 0x0FU)));
    }
    else
    {
        region[byteIndex] = static_cast<std::byte>(
            static_cast<std::uint8_t>((value & 0x0FU) |
                ((tileValue & 0x0FU) << 4)));
    }
    return region;
}

// Reads back the four-bit tile at `tileIndex` from a region.
[[nodiscard]] std::uint8_t ReadTile(
    const std::span<const std::byte> region, const std::uint64_t tileIndex)
    noexcept
{
    const std::size_t byteIndex = static_cast<std::size_t>(tileIndex / 2U);
    const std::uint8_t value =
        std::to_integer<std::uint8_t>(region[byteIndex]);
    if ((tileIndex % 2U) == 0U)
    {
        return value & 0x0FU;
    }
    return (value >> 4) & 0x0FU;
}

bool RegionIsAllZero(const std::span<const std::byte> region) noexcept
{
    return std::all_of(region.begin(), region.end(),
        [](const std::byte value) noexcept
        {
            return value == std::byte{0};
        });
}

} // namespace

static_assert(kInterleaveTileCount == 112336,
    "the reference interleave tile count is frozen at 472 x 238");
static_assert(kInterleaveRegionBytes == 56168,
    "the reference interleave region is frozen at tileCount / 2 bytes");
static_assert(kInterleavePhaseCount == 16,
    "the reference interleave phase count is frozen at 16");
static_assert(
    (kInterleaveMultiplier * kInterleaveMultiplierInverse) %
            kInterleaveTileCount ==
        1,
    "the frozen multiplier inverse must satisfy M * M^-1 = 1 mod n");
static_assert(kInterleaveMultiplierInverse < kInterleaveTileCount,
    "the multiplier inverse must be a canonical residue mod n");
static_assert(GetInterleavePhase(0) == 0 &&
    GetInterleavePhase(1) == 1 &&
    GetInterleavePhase(15) == 15 &&
    GetInterleavePhase(16) == 0 &&
    GetInterleavePhase(17) == 1,
    "the phase is the FrameSequence modulo the frozen phase count");
static_assert(MapLogicalTileToPhysical(0, 0) == 0,
    "tile 0 of phase 0 maps to itself (zero shift, unit multiplier)");
static_assert(MapPhysicalTileToLogical(0, 0) == 0,
    "the inverse of the phase-0 identity is the identity");

TEST_CASE("Interleave: frozen constants cross-check against the reference "
    "raster", "[interleave][constants]")
{
    // The interleave profile operates on the frozen PB-ReferenceRaster-1
    // data region; a drift in either constant is a protocol regression.
    REQUIRE(kInterleaveTileCount ==
        static_cast<std::uint64_t>(pbmodulation::kReferenceDataTileCount));
    REQUIRE(kInterleaveRegionBytes ==
        pbmodulation::kReferenceDataRegionBytes);
    REQUIRE(kInterleaveProfileIdReferenceV1 ==
        0xEA949F2D1F89B845ULL);
    // The translation cycle n / gcd(S, n) must cover all 16 phases so the
    // 16 phase shifts are pairwise distinct.
    REQUIRE(Gcd(kInterleavePhaseStepTiles, kInterleaveTileCount) ==
        472);
    REQUIRE(kInterleaveTileCount / 472 == 238);
    REQUIRE(238 >= kInterleavePhaseCount);
}

TEST_CASE("Interleave: checked arithmetic bounds", "[interleave][arithmetic]")
{
    // The largest intermediate product of the forward map must stay inside
    // uint64_t while exceeding uint32_t (the reason the map is u64-typed).
    constexpr std::uint64_t maxForwardProduct =
        (kInterleaveTileCount - 1) * kInterleaveMultiplier;
    REQUIRE(maxForwardProduct == 7362098895ULL);
    REQUIRE(maxForwardProduct >
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()));
    REQUIRE(maxForwardProduct <=
        std::numeric_limits<std::uint64_t>::max());
    // The reverse map's largest intermediate product (unshifted * M^-1) has
    // the same bound with M^-1 < M.
    constexpr std::uint64_t maxReverseProduct =
        (kInterleaveTileCount - 1) * kInterleaveMultiplierInverse;
    REQUIRE(maxReverseProduct >
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()));
    REQUIRE(maxReverseProduct <=
        std::numeric_limits<std::uint64_t>::max());
}

TEST_CASE("Interleave: full-table bijection for all 16 phases",
    "[interleave][bijection]")
{
    constexpr std::size_t tileCount =
        static_cast<std::size_t>(kInterleaveTileCount);
    std::vector<std::uint64_t> physicalTable(tileCount);
    for (std::uint64_t frameSequence = 0;
         frameSequence < kInterleavePhaseCount; frameSequence++)
    {
        // Forward table: every logical tile maps to a physical tile.
        for (std::uint64_t tile = 0; tile < tileCount; tile++)
        {
            physicalTable[tile] = MapLogicalTileToPhysical(tile, frameSequence);
        }
        // Deduplication: the forward map is injective (hence bijective on
        // the finite tile set).
        std::vector<std::uint64_t> sortedTable = physicalTable;
        std::sort(sortedTable.begin(), sortedTable.end());
        REQUIRE(std::unique(sortedTable.begin(), sortedTable.end()) ==
            sortedTable.end());
        // Both composition identities on the full table.
        for (std::uint64_t tile = 0; tile < tileCount; tile++)
        {
            REQUIRE(MapPhysicalTileToLogical(physicalTable[tile],
                frameSequence) == tile);
            REQUIRE(MapLogicalTileToPhysical(
                MapPhysicalTileToLogical(tile, frameSequence),
                frameSequence) == tile);
        }
        for (std::uint64_t physical = 0; physical < tileCount; physical++)
        {
            REQUIRE(MapPhysicalTileToLogical(physical, frameSequence) <
                kInterleaveTileCount);
            REQUIRE(MapLogicalTileToPhysical(
                MapPhysicalTileToLogical(physical, frameSequence),
                frameSequence) == physical);
        }
    }
}

TEST_CASE("Interleave: phase boundaries on extreme FrameSequence values",
    "[interleave][phase-boundary]")
{
    const std::vector<std::uint64_t> sequences{
        0,
        1,
        15,
        16,
        17,
        1000000000000000000ULL, // 10^18
        (std::numeric_limits<std::uint64_t>::max() / 2) - 1, // 2^63 - 1
        std::numeric_limits<std::uint64_t>::max() / 2, // 2^63
        std::numeric_limits<std::uint64_t>::max() // 2^64 - 1
    };
    for (const std::uint64_t sequence : sequences)
    {
        REQUIRE(GetInterleavePhase(sequence) == sequence % 16U);
        // Period 16: f and f + 16 produce the identical permutation.
        const std::uint64_t wrapped = sequence % 16U;
        for (const std::uint64_t tile : {0ULL, 1ULL, 12345ULL, 112335ULL})
        {
            REQUIRE(MapLogicalTileToPhysical(tile, sequence) ==
                MapLogicalTileToPhysical(tile, wrapped));
            REQUIRE(MapPhysicalTileToLogical(tile, sequence) ==
                MapPhysicalTileToLogical(tile, wrapped));
        }
        // 2^64 - 1 wraps to phase 15 without any overflow: the shift term
        // is (phase * S) mod n, and phase is already reduced.
    }
    // 2^64 - 1 is phase 15; the max FrameSequence and 15 must agree on
    // every sampled tile of the full period boundary pair.
    for (std::uint64_t tile = 0; tile < kInterleaveTileCount; tile += 7919U)
    {
        REQUIRE(MapLogicalTileToPhysical(tile,
            std::numeric_limits<std::uint64_t>::max()) ==
            MapLogicalTileToPhysical(tile, 15));
    }
}

TEST_CASE("Interleave: the 16 phases are pairwise distinct at the region "
    "digest level", "[interleave][phase-distinct]")
{
    const std::vector<std::byte> logical = MakeSplitMixRegion(0xBEEFCAFE);
    std::vector<std::array<std::byte, 32>> digests;
    digests.reserve(kInterleavePhaseCount);
    for (std::uint64_t phase = 0; phase < kInterleavePhaseCount; phase++)
    {
        std::vector<std::byte> physical(kInterleaveRegionBytes);
        const auto status = ApplyInterleave(
            std::span<const std::byte>(logical),
            std::span<std::byte>(physical), phase);
        REQUIRE(static_cast<bool>(status));
        digests.push_back(pbprotocol::ComputeBlake3Digest(
            std::span<const std::byte>(physical)));
    }
    for (std::size_t first = 0; first < digests.size(); first++)
    {
        for (std::size_t second = first + 1; second < digests.size();
             second++)
        {
            REQUIRE(digests[first] != digests[second]);
        }
    }
}

TEST_CASE("Interleave: directed single-nibble tile patterns at region "
    "edges", "[interleave][nibble-boundary]")
{
    // Edge tiles: 0 (first low nibble), 1 (first high nibble), n/2 and
    // n-1 (last high nibble of the region).
    const std::vector<std::uint64_t> edgeTiles{0, 1, 56168, 112335};
    const std::vector<std::uint8_t> tileValues{1, 8, 0xF};
    for (const std::uint64_t tile : edgeTiles)
    {
        for (const std::uint8_t value : tileValues)
        {
            for (const std::uint64_t phase : {0ULL, 7ULL, 15ULL})
            {
                const std::vector<std::byte> logical =
                    MakeDirectedTileRegion(tile, value);
                std::vector<std::byte> physical(kInterleaveRegionBytes);
                const auto status = ApplyInterleave(
                    std::span<const std::byte>(logical),
                    std::span<std::byte>(physical), phase);
                REQUIRE(static_cast<bool>(status));
                const std::uint64_t expectedPhysicalTile =
                    MapLogicalTileToPhysical(tile, phase);
                REQUIRE(ReadTile(std::span<const std::byte>(physical),
                    expectedPhysicalTile) == value);
                // Every other tile must remain exactly zero.
                for (std::uint64_t other = 0; other < kInterleaveTileCount;
                     other++)
                {
                    if (other == expectedPhysicalTile)
                    {
                        continue;
                    }
                    REQUIRE(ReadTile(std::span<const std::byte>(physical),
                        other) == 0);
                }
                // Reverse brings the directed pattern back to its logical
                // edge position.
                std::vector<std::byte> back(kInterleaveRegionBytes);
                const auto reverseStatus = ReverseInterleave(
                    std::span<const std::byte>(physical),
                    std::span<std::byte>(back), phase);
                REQUIRE(static_cast<bool>(reverseStatus));
                REQUIRE(back == logical);
            }
        }
    }
}

TEST_CASE("Interleave: region content matrix with Apply/Reverse round "
    "trips", "[interleave][content-matrix]")
{
    const std::vector<std::vector<std::byte>> patterns{
        MakeFilledRegion(0x00),
        MakeFilledRegion(0xFF),
        MakeIncrementRegion(),
        MakeSplitMixRegion(0x123456789ABCDEF0ULL),
        MakeDirectedTileRegion(56167, 0xA) // single tile mid-region
    };
    const std::vector<std::uint64_t> sequences{
        0, 1, 15, 16, 17, 1000000000000000000ULL,
        std::numeric_limits<std::uint64_t>::max()};
    for (const auto& pattern : patterns)
    {
        for (const std::uint64_t sequence : sequences)
        {
            std::vector<std::byte> physical(kInterleaveRegionBytes);
            const auto applyStatus = ApplyInterleave(
                std::span<const std::byte>(pattern),
                std::span<std::byte>(physical), sequence);
            REQUIRE(static_cast<bool>(applyStatus));
            std::vector<std::byte> restored(kInterleaveRegionBytes,
                std::byte{0x5A});
            const auto reverseStatus = ReverseInterleave(
                std::span<const std::byte>(physical),
                std::span<std::byte>(restored), sequence);
            REQUIRE(static_cast<bool>(reverseStatus));
            REQUIRE(restored == pattern);
            // Reverse first, then apply: the other composition order.
            std::vector<std::byte> logicalCopy(kInterleaveRegionBytes);
            const auto reverseFirst = ReverseInterleave(
                std::span<const std::byte>(pattern),
                std::span<std::byte>(logicalCopy), sequence);
            REQUIRE(static_cast<bool>(reverseFirst));
            std::vector<std::byte> physicalCopy(kInterleaveRegionBytes);
            const auto applySecond = ApplyInterleave(
                std::span<const std::byte>(logicalCopy),
                std::span<std::byte>(physicalCopy), sequence);
            REQUIRE(static_cast<bool>(applySecond));
            REQUIRE(physicalCopy == pattern);
        }
    }
    // The zero region maps to the zero region for every phase (a sanity
    // pin of the nibble bookkeeping with the least informative content).
    const std::vector<std::byte> zeros = MakeFilledRegion(0x00);
    for (std::uint64_t phase = 0; phase < kInterleavePhaseCount; phase++)
    {
        std::vector<std::byte> physical(kInterleaveRegionBytes,
            std::byte{0x5A});
        const auto status = ApplyInterleave(
            std::span<const std::byte>(zeros),
            std::span<std::byte>(physical), phase);
        REQUIRE(static_cast<bool>(status));
        REQUIRE(RegionIsAllZero(std::span<const std::byte>(physical)));
    }
}

TEST_CASE("Interleave: exact span-size contract", "[interleave][contract]")
{
    // Every wrong size is rejected with InvalidInput and a detail equal to
    // the received size; the exact size is accepted.
    const std::vector<std::size_t> wrongSizes{0, 1, 56167, 56169};
    const std::vector<std::byte> logical = MakeSplitMixRegion(0x0DEADBEEF);
    for (const std::size_t wrongSize : wrongSizes)
    {
        std::vector<std::byte> shortLogical(wrongSize);
        std::vector<std::byte> physical(kInterleaveRegionBytes);
        const auto status = ApplyInterleave(
            std::span<const std::byte>(shortLogical),
            std::span<std::byte>(physical), 3);
        REQUIRE(static_cast<bool>(status) == false);
        REQUIRE(status.Error().code == InterleaveErrorCode::InvalidInput);
        REQUIRE(status.Error().detail == wrongSize);
        // The wrong logical size must also be rejected on the physical
        // side of the same call.
        std::vector<std::byte> shortPhysical(wrongSize);
        const auto physicalSideStatus = ApplyInterleave(
            std::span<const std::byte>(logical),
            std::span<std::byte>(shortPhysical), 3);
        REQUIRE(static_cast<bool>(physicalSideStatus) == false);
        REQUIRE(physicalSideStatus.Error().code ==
            InterleaveErrorCode::InvalidInput);
        REQUIRE(physicalSideStatus.Error().detail == wrongSize);
        // Reverse carries the same contract.
        const auto reverseStatus = ReverseInterleave(
            std::span<const std::byte>(shortLogical),
            std::span<std::byte>(physical), 3);
        REQUIRE(reverseStatus.Error().code ==
            InterleaveErrorCode::InvalidInput);
        const auto reversePhysicalStatus = ReverseInterleave(
            std::span<const std::byte>(logical),
            std::span<std::byte>(shortPhysical), 3);
        REQUIRE(reversePhysicalStatus.Error().code ==
            InterleaveErrorCode::InvalidInput);
    }
    // The exact frozen size is accepted (round-trip identity).
    std::vector<std::byte> physical(kInterleaveRegionBytes);
    REQUIRE(static_cast<bool>(ApplyInterleave(
        std::span<const std::byte>(logical),
        std::span<std::byte>(physical), 3)));
    std::vector<std::byte> back(kInterleaveRegionBytes);
    REQUIRE(static_cast<bool>(ReverseInterleave(
        std::span<const std::byte>(physical),
        std::span<std::byte>(back), 3)));
    REQUIRE(back == logical);
}

TEST_CASE("Interleave: overlapping spans are rejected without writes",
    "[interleave][overlap]")
{
    const std::vector<std::byte> logical = MakeSplitMixRegion(0xF00DCAFE);

    SECTION("exactly adjacent spans are accepted")
    {
        std::vector<std::byte> buffer(kInterleaveRegionBytes * 2);
        std::copy(logical.begin(), logical.end(), buffer.begin());
        const auto logicalSpan = std::span<std::byte>(buffer).first(
            kInterleaveRegionBytes);
        const auto physicalSpan =
            std::span<std::byte>(buffer).subspan(kInterleaveRegionBytes);
        const auto status = ApplyInterleave(
            std::span<const std::byte>(logicalSpan), physicalSpan, 5);
        REQUIRE(static_cast<bool>(status));
    }

    SECTION("one-byte overlap is rejected and nothing is written")
    {
        std::vector<std::byte> buffer(kInterleaveRegionBytes + 1,
            std::byte{0x5A});
        const auto logicalSpan = std::span<std::byte>(buffer).first(
            kInterleaveRegionBytes);
        const auto physicalSpan =
            std::span<std::byte>(buffer).subspan(1);
        const auto status = ApplyInterleave(
            std::span<const std::byte>(logicalSpan), physicalSpan, 5);
        REQUIRE(static_cast<bool>(status) == false);
        REQUIRE(status.Error().code == InterleaveErrorCode::OverlappingSpans);
        // No-write-on-failure: every byte keeps the sentinel.
        REQUIRE(std::all_of(buffer.begin(), buffer.end(),
            [](const std::byte value) noexcept
            {
                return value == std::byte{0x5A};
            }));
        // Same contract on the reverse direction.
        const auto reverseStatus = ReverseInterleave(
            std::span<const std::byte>(logicalSpan), physicalSpan, 5);
        REQUIRE(reverseStatus.Error().code ==
            InterleaveErrorCode::OverlappingSpans);
        REQUIRE(std::all_of(buffer.begin(), buffer.end(),
            [](const std::byte value) noexcept
            {
                return value == std::byte{0x5A};
            }));
    }

    SECTION("identical spans are rejected")
    {
        std::vector<std::byte> buffer = logical;
        const auto span = std::span<std::byte>(buffer);
        const auto status = ApplyInterleave(
            std::span<const std::byte>(span), span, 5);
        REQUIRE(status.Error().code == InterleaveErrorCode::OverlappingSpans);
        REQUIRE(buffer == logical);
    }

    SECTION("interior overlap deep in the region is rejected")
    {
        std::vector<std::byte> buffer(kInterleaveRegionBytes * 2);
        std::copy(logical.begin(), logical.end(), buffer.begin());
        const auto logicalSpan = std::span<std::byte>(buffer).first(
            kInterleaveRegionBytes);
        // Physical starts inside the logical span (last 100 bytes shared).
        const auto physicalSpan =
            std::span<std::byte>(buffer).subspan(
                kInterleaveRegionBytes - 100, kInterleaveRegionBytes);
        const auto status = ApplyInterleave(
            std::span<const std::byte>(logicalSpan), physicalSpan, 5);
        REQUIRE(status.Error().code == InterleaveErrorCode::OverlappingSpans);
    }
}

TEST_CASE("Interleave: deterministic pure-function contract",
    "[interleave][determinism]")
{
    const std::vector<std::byte> logical = MakeSplitMixRegion(0xCAFEBABE);
    std::vector<std::byte> first(kInterleaveRegionBytes);
    std::vector<std::byte> second(kInterleaveRegionBytes);
    REQUIRE(static_cast<bool>(ApplyInterleave(
        std::span<const std::byte>(logical),
        std::span<std::byte>(first), 9)));
    REQUIRE(static_cast<bool>(ApplyInterleave(
        std::span<const std::byte>(logical),
        std::span<std::byte>(second), 9)));
    REQUIRE(first == second);
    // Repeated inverse calls on the same physical input agree.
    std::vector<std::byte> backA(kInterleaveRegionBytes);
    std::vector<std::byte> backB(kInterleaveRegionBytes);
    REQUIRE(static_cast<bool>(ReverseInterleave(
        std::span<const std::byte>(first),
        std::span<std::byte>(backA), 9)));
    REQUIRE(static_cast<bool>(ReverseInterleave(
        std::span<const std::byte>(first),
        std::span<std::byte>(backB), 9)));
    REQUIRE(backA == backB);
    REQUIRE(backA == logical);
}
