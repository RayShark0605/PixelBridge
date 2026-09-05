#include "pbmodulation/unified_visual_mapping.h"

#include "pbprotocol/blake3_digest.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <span>
#include <string>
#include <vector>

#ifndef PB_UNIFIED_SC6_GOLDEN_DIR
#error PB_UNIFIED_SC6_GOLDEN_DIR must name the independent Unified SC6 Golden directory
#endif

namespace
{

std::filesystem::path GoldenPath(const char* const name)
{
    return std::filesystem::path(PB_UNIFIED_SC6_GOLDEN_DIR) / name;
}

std::vector<std::byte> ReadGolden(const char* const name)
{
    const std::filesystem::path path = GoldenPath(name);
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    REQUIRE(input);
    const std::streampos end = input.tellg();
    REQUIRE(end >= 0);
    const auto size = static_cast<std::uint64_t>(end);
    REQUIRE(size <= std::numeric_limits<std::size_t>::max());
    std::vector<std::byte> content(static_cast<std::size_t>(size));
    input.seekg(0);
    if (!content.empty())
    {
        input.read(reinterpret_cast<char*>(content.data()), static_cast<std::streamsize>(content.size()));
        REQUIRE(input);
    }
    return content;
}

std::uint16_t ReadLe16(const std::span<const std::byte> bytes, const std::size_t offset)
{
    REQUIRE(offset <= bytes.size());
    REQUIRE(bytes.size() - offset >= 2);
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(bytes[offset]) |
        (std::to_integer<unsigned>(bytes[offset + 1]) << 8));
}

std::uint32_t ReadLe32(const std::span<const std::byte> bytes, const std::size_t offset)
{
    REQUIRE(offset <= bytes.size());
    REQUIRE(bytes.size() - offset >= 4);
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; index++)
    {
        value |= static_cast<std::uint32_t>(std::to_integer<unsigned>(bytes[offset + index])) << (index * 8);
    }
    return value;
}

std::string ToHex(const std::span<const std::byte> bytes)
{
    constexpr char alphabet[] = "0123456789abcdef";
    std::string output(bytes.size() * 2, '0');
    for (std::size_t index = 0; index < bytes.size(); index++)
    {
        const unsigned int value = std::to_integer<unsigned int>(bytes[index]);
        output[index * 2] = alphabet[value >> 4];
        output[index * 2 + 1] = alphabet[value & 0x0FU];
    }
    return output;
}

void AppendLe32(std::array<std::byte, 12>& record, const std::size_t offset, const std::uint32_t value) noexcept
{
    for (std::size_t index = 0; index < 4; index++)
    {
        record[offset + index] = static_cast<std::byte>((value >> (index * 8)) & 0xFFU);
    }
}

} // namespace

TEST_CASE("Unified SC6 V3 codebook and region-local mapping match the independent Golden",
    "[pbmodulation][unified][mapping][golden]")
{
    using namespace pbmodulation;
    STATIC_REQUIRE(ValidateUnifiedVisualMappingStaticContract());

    const std::vector<std::byte> codebook = ReadGolden("codebook.bin");
    REQUIRE(codebook.size() == kUnifiedSymbolMasksByLabel.size() * 4);
    for (std::size_t label = 0; label < kUnifiedSymbolMasksByLabel.size(); label++)
    {
        REQUIRE(ReadLe32(codebook, label * 4) == kUnifiedSymbolMasksByLabel[label]);
    }

    const std::vector<std::byte> chroma = ReadGolden("chroma-states.bin");
    REQUIRE(chroma.size() == kUnifiedChromaStatesByLabel.size() * 4);
    for (std::size_t label = 0; label < kUnifiedChromaStatesByLabel.size(); label++)
    {
        const std::size_t offset = label * 4;
        const UnifiedChromaState& expected = kUnifiedChromaStatesByLabel[label];
        REQUIRE(std::to_integer<std::uint8_t>(chroma[offset]) == expected.blue);
        REQUIRE(std::to_integer<std::uint8_t>(chroma[offset + 1]) == expected.green);
        REQUIRE(std::to_integer<std::uint8_t>(chroma[offset + 2]) == expected.red);
        REQUIRE(std::to_integer<std::uint8_t>(chroma[offset + 3]) == expected.label);
    }

    const std::vector<std::byte> mapping = ReadGolden("mapping-contract.bin");
    REQUIRE(mapping.size() == 160);
    constexpr std::array<char, 8> magic{'P', 'B', 'U', 'S', 'C', '6', 'M', '3'};
    REQUIRE(std::equal(mapping.begin(), mapping.begin() + 8,
        reinterpret_cast<const std::byte*>(magic.data()), reinterpret_cast<const std::byte*>(magic.data()) + magic.size()));
    REQUIRE(ReadLe32(mapping, 8) == kUnifiedMappingVersion);
    REQUIRE(ReadLe32(mapping, 12) == kUnifiedVisualProfile.dataTileCount);
    REQUIRE(ReadLe32(mapping, 16) == kUnifiedMappingFreshnessRegionCount);
    REQUIRE(ReadLe32(mapping, 20) == kUnifiedLumaCarrierPlanes);
    REQUIRE(ReadLe32(mapping, 24) == kUnifiedChromaCarrierPlanes);
    REQUIRE(ReadLe32(mapping, 28) == kUnifiedMappingSequencePeriod);
    for (std::size_t region = 0; region < kUnifiedMappingFreshnessRegionCount; region++)
    {
        REQUIRE(ReadLe32(mapping, 32 + region * 4) == kUnifiedFreshnessTileCatalog.tileCounts[region]);
    }
    for (std::size_t boundary = 0; boundary < kUnifiedMappingFreshnessColumnBoundaries.size(); boundary++)
    {
        REQUIRE(ReadLe32(mapping, 68 + boundary * 4) == kUnifiedMappingFreshnessColumnBoundaries[boundary]);
        REQUIRE(ReadLe32(mapping, 76 + boundary * 4) == kUnifiedMappingFreshnessRowBoundaries[boundary]);
    }
    for (std::size_t region = 0; region < kUnifiedChromaRegionOrder.size(); region++)
    {
        REQUIRE(std::to_integer<std::uint8_t>(mapping[84 + region]) == kUnifiedChromaRegionOrder[region]);
    }
    REQUIRE(std::ranges::all_of(std::span<const std::byte>{mapping}.subspan(93, 3),
        [](const std::byte value) { return value == std::byte{0}; }));
    for (std::size_t laneIndex = 0; laneIndex < kUnifiedLaneMappings.size(); laneIndex++)
    {
        REQUIRE(ReadLe32(mapping, 96 + laneIndex * 4) == kUnifiedLaneMappings[laneIndex].logicalBits);
    }
    REQUIRE(ReadLe32(mapping, 108) == kUnifiedVisualProfile.innerCodewordBits);
    REQUIRE(ReadLe32(mapping, 112) == kUnifiedLumaDeficitBits);
    REQUIRE(ReadLe32(mapping, 116) == kUnifiedUnusedLumaCarrierBits);
    REQUIRE(ReadLe32(mapping, 120) == kUnifiedUnusedChromaCarrierBits);
    REQUIRE(ReadLe32(mapping, 124) == kUnifiedMappingContractFlags);
    REQUIRE(ReadLe32(mapping, 128) == kUnifiedCodewordInterleave.modulus);
    REQUIRE(ReadLe32(mapping, 132) == kUnifiedCodewordInterleave.multiplier);
    REQUIRE(ReadLe32(mapping, 136) == kUnifiedCodewordInterleave.inverse);
    REQUIRE(ReadLe32(mapping, 140) == kUnifiedCodewordInterleave.offset);
    REQUIRE(ReadLe32(mapping, 144) == kUnifiedCodewordInterleave.phaseStep);
    REQUIRE(ReadLe32(mapping, 148) == kUnifiedCodewordInterleave.phaseCount);
    REQUIRE(std::ranges::all_of(std::span<const std::byte>{mapping}.subspan(152, 8),
        [](const std::byte value) { return value == std::byte{0}; }));

    const std::vector<std::byte> digestFile = ReadGolden("mapping-stream.blake3");
    const std::string expectedDigest(kUnifiedMappingStreamFrameSequenceZeroBlake3);
    REQUIRE(std::string(reinterpret_cast<const char*>(digestFile.data()), digestFile.size()) == expectedDigest + "\n");
}

TEST_CASE("Unified SC6 V3 mapping is collision-free, invertible and sequence-stable",
    "[pbmodulation][unified][mapping][bijection]")
{
    using namespace pbmodulation;
    for (std::uint64_t frameSequence = 0; frameSequence < kUnifiedMappingSequencePeriod; frameSequence++)
    {
        std::vector<std::uint8_t> lumaOwners(static_cast<std::size_t>(kUnifiedVisualProfile.dataTileCount) * 4);
        std::vector<std::uint8_t> chromaOwners(static_cast<std::size_t>(kUnifiedVisualProfile.dataTileCount) * 2);
        for (std::size_t laneIndex = 0; laneIndex < kUnifiedLaneMappings.size(); laneIndex++)
        {
            const UnifiedLane lane = static_cast<UnifiedLane>(laneIndex);
            const UnifiedLaneMappingContract& contract = kUnifiedLaneMappings[laneIndex];
            for (std::uint32_t logicalBit = 0; logicalBit < contract.logicalBits; logicalBit++)
            {
                const UnifiedPhysicalCarrierSite site = GetUnifiedPhysicalCarrierSite(lane, logicalBit, frameSequence);
                REQUIRE(site.valid);
                REQUIRE(site.tileOrdinal < kUnifiedVisualProfile.dataTileCount);
                std::vector<std::uint8_t>& owners = site.carrier == UnifiedCarrier::Luma ? lumaOwners : chromaOwners;
                const std::size_t planes = site.carrier == UnifiedCarrier::Luma ? 4 : 2;
                REQUIRE(site.bitPlane < planes);
                const std::size_t siteIndex = static_cast<std::size_t>(site.tileOrdinal) * planes + site.bitPlane;
                REQUIRE(owners[siteIndex] == 0);
                owners[siteIndex] = static_cast<std::uint8_t>(laneIndex + 1);
                const UnifiedLogicalCarrierBit inverse = GetUnifiedLogicalCarrierBit(site, frameSequence);
                REQUIRE(inverse == UnifiedLogicalCarrierBit{true, lane, logicalBit});
            }
        }
        REQUIRE(std::ranges::count_if(lumaOwners, [](const std::uint8_t owner) { return owner != 0; }) == 162000);
        REQUIRE(std::ranges::count_if(chromaOwners, [](const std::uint8_t owner) { return owner != 0; }) == 81000);
        REQUIRE(std::ranges::count(lumaOwners, 0) == 5488);
        REQUIRE(std::ranges::count(chromaOwners, 0) == 2744);
    }

    REQUIRE_FALSE(GetUnifiedPhysicalCarrierSite(UnifiedLane::BaseLuma, 145800, 0).valid);
    REQUIRE_FALSE(GetUnifiedPhysicalCarrierSite(UnifiedLane::FineLuma, 16200, 0).valid);
    REQUIRE_FALSE(GetUnifiedPhysicalCarrierSite(UnifiedLane::Chroma, 81000, 0).valid);
    REQUIRE_FALSE(GetUnifiedPhysicalCarrierSite(static_cast<UnifiedLane>(0xFF), 0, 0).valid);
    REQUIRE_FALSE(GetUnifiedLogicalCarrierBit({true, UnifiedCarrier::Luma, 41872, 0}, 0).valid);
    REQUIRE_FALSE(GetUnifiedLogicalCarrierBit({true, UnifiedCarrier::Luma, 0, 4}, 0).valid);
    REQUIRE_FALSE(GetUnifiedLogicalCarrierBit({true, UnifiedCarrier::Chroma, 0, 2}, 0).valid);
    REQUIRE_FALSE(GetUnifiedLogicalCarrierBit({true, static_cast<UnifiedCarrier>(0xFF), 0, 0}, 0).valid);
    REQUIRE(GetUnifiedPhysicalCarrierSite(UnifiedLane::BaseLuma, 0, 0) !=
        GetUnifiedPhysicalCarrierSite(UnifiedLane::BaseLuma, 0, 1));
    REQUIRE(GetUnifiedPhysicalCarrierSite(UnifiedLane::BaseLuma, 0, 0) ==
        GetUnifiedPhysicalCarrierSite(UnifiedLane::BaseLuma, 0, kUnifiedMappingSequencePeriod));
}

TEST_CASE("Unified SC6 V3 keeps Base and Chroma damage inside bounded freshness regions",
    "[pbmodulation][unified][mapping][locality]")
{
    using namespace pbmodulation;
    constexpr std::uint32_t codewordBits = kUnifiedVisualProfile.innerCodewordBits;
    std::array<std::array<std::uint32_t, kUnifiedMappingFreshnessRegionCount>, 9> baseRegionBits{};
    for (std::uint32_t logicalBit = 0; logicalBit < kUnifiedBaseLumaBits; logicalBit++)
    {
        const UnifiedPhysicalCarrierSite site = GetUnifiedPhysicalCarrierSite(UnifiedLane::BaseLuma, logicalBit, 0);
        REQUIRE(site.valid);
        const std::uint32_t slot = logicalBit / codewordBits;
        const std::uint8_t region = kUnifiedFreshnessTileCatalog.regionByTileOrdinal[site.tileOrdinal];
        baseRegionBits[slot][region]++;
    }
    for (std::uint32_t slot = 0; slot < baseRegionBits.size(); slot++)
    {
        const std::uint32_t regionCapacity = kUnifiedFreshnessTileCatalog.tileCounts[slot] *
            kUnifiedLumaCarrierPlanes;
        const std::uint32_t expectedPrimaryBits = std::min(codewordBits, regionCapacity);
        REQUIRE(baseRegionBits[slot][slot] == expectedPrimaryBits);
        REQUIRE(baseRegionBits[slot][slot] * 100 >= codewordBits * 93);
        REQUIRE(std::accumulate(baseRegionBits[slot].begin(), baseRegionBits[slot].end(), 0U) == codewordBits);
    }

    std::array<std::array<bool, kUnifiedMappingFreshnessRegionCount>, 5> chromaRegions{};
    for (std::uint32_t logicalBit = 0; logicalBit < kUnifiedChromaBits; logicalBit++)
    {
        const UnifiedPhysicalCarrierSite site = GetUnifiedPhysicalCarrierSite(UnifiedLane::Chroma, logicalBit, 0);
        REQUIRE(site.valid);
        const std::uint32_t slot = logicalBit / codewordBits;
        const std::uint8_t region = kUnifiedFreshnessTileCatalog.regionByTileOrdinal[site.tileOrdinal];
        chromaRegions[slot][region] = true;
    }
    for (const auto& regions : chromaRegions)
    {
        REQUIRE(std::ranges::count(regions, true) <= 3);
    }
}

TEST_CASE("Unified SC6 V3 public mapping rebuilds the frozen frame-zero stream digest",
    "[pbmodulation][unified][mapping][digest]")
{
    using namespace pbmodulation;
    constexpr char domain[] = "PixelBridge.UnifiedSc6MappingStream.3";
    pbprotocol::Blake3Hasher hasher;
    hasher.Update(std::as_bytes(std::span{domain, sizeof(domain)}));
    for (std::size_t laneIndex = 0; laneIndex < kUnifiedLaneMappings.size(); laneIndex++)
    {
        const UnifiedLane lane = static_cast<UnifiedLane>(laneIndex);
        for (std::uint32_t logicalBit = 0; logicalBit < kUnifiedLaneMappings[laneIndex].logicalBits; logicalBit++)
        {
            const UnifiedPhysicalCarrierSite site = GetUnifiedPhysicalCarrierSite(lane, logicalBit, 0);
            REQUIRE(site.valid);
            std::array<std::byte, 12> record{};
            record[0] = static_cast<std::byte>(laneIndex);
            record[1] = static_cast<std::byte>(site.carrier == UnifiedCarrier::Luma ? 0 : 1);
            record[2] = static_cast<std::byte>(site.bitPlane);
            AppendLe32(record, 4, logicalBit);
            AppendLe32(record, 8, site.tileOrdinal);
            hasher.Update(record);
        }
    }
    REQUIRE(ToHex(hasher.Finalize()) == kUnifiedMappingStreamFrameSequenceZeroBlake3);
}
