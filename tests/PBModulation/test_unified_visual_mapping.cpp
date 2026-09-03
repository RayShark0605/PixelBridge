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
#include <span>
#include <string>
#include <vector>

#ifndef PB_UNIFIED_LC4_GOLDEN_DIR
#error PB_UNIFIED_LC4_GOLDEN_DIR must name the independent Unified LC4 Golden directory
#endif

namespace
{

std::filesystem::path GoldenPath(const char* const name)
{
    return std::filesystem::path(PB_UNIFIED_LC4_GOLDEN_DIR) / name;
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

TEST_CASE("Unified LC4 codebook labels and compact mapping match the independent Golden",
    "[pbmodulation][unified][mapping][golden]")
{
    using namespace pbmodulation;
    STATIC_REQUIRE(ValidateUnifiedVisualMappingStaticContract());

    const std::vector<std::byte> codebook = ReadGolden("codebook.bin");
    REQUIRE(codebook.size() == kUnifiedSymbolMasksByLabel.size() * 2);
    for (std::size_t label = 0; label < kUnifiedSymbolMasksByLabel.size(); label++)
    {
        REQUIRE(ReadLe16(codebook, label * 2) == kUnifiedSymbolMasksByLabel[label]);
    }

    const std::vector<std::byte> chroma = ReadGolden("chroma-states.bin");
    REQUIRE(chroma.size() == kUnifiedChromaStatesByLabel.size() * 7);
    for (std::size_t label = 0; label < kUnifiedChromaStatesByLabel.size(); label++)
    {
        const std::size_t offset = label * 7;
        const UnifiedChromaState& expected = kUnifiedChromaStatesByLabel[label];
        REQUIRE(std::bit_cast<std::int16_t>(ReadLe16(chroma, offset)) == expected.blueOffset);
        REQUIRE(std::bit_cast<std::int16_t>(ReadLe16(chroma, offset + 2)) == expected.greenOffset);
        REQUIRE(std::bit_cast<std::int16_t>(ReadLe16(chroma, offset + 4)) == expected.redOffset);
        REQUIRE(std::to_integer<std::uint8_t>(chroma[offset + 6]) == expected.label);
    }

    const std::vector<std::byte> mapping = ReadGolden("mapping-contract.bin");
    REQUIRE(mapping.size() == 128);
    constexpr std::array<char, 8> magic{'P', 'B', 'U', 'L', 'C', '4', 'M', '1'};
    REQUIRE(std::equal(mapping.begin(), mapping.begin() + 8,
        reinterpret_cast<const std::byte*>(magic.data()), reinterpret_cast<const std::byte*>(magic.data()) + magic.size()));
    REQUIRE(ReadLe32(mapping, 8) == kUnifiedMappingVersion);
    REQUIRE(ReadLe32(mapping, 12) == kUnifiedVisualProfile.dataTileCount);
    REQUIRE(ReadLe32(mapping, 16) == kUnifiedPlaneThreeTileOrder.multiplier);
    REQUIRE(ReadLe32(mapping, 20) == kUnifiedPlaneThreeTileOrder.offset);
    REQUIRE(ReadLe32(mapping, 24) == kUnifiedChromaTileOrder.multiplier);
    REQUIRE(ReadLe32(mapping, 28) == kUnifiedChromaTileOrder.offset);
    for (std::size_t laneIndex = 0; laneIndex < kUnifiedLaneInterleaves.size(); laneIndex++)
    {
        const std::size_t offset = 32 + laneIndex * 32;
        const UnifiedLaneInterleaveContract& expected = kUnifiedLaneInterleaves[laneIndex];
        REQUIRE(ReadLe32(mapping, offset) == static_cast<std::uint32_t>(expected.lane));
        REQUIRE(ReadLe32(mapping, offset + 4) == expected.logicalBits);
        REQUIRE(ReadLe32(mapping, offset + 8) == expected.multiplier);
        REQUIRE(ReadLe32(mapping, offset + 12) == expected.inverse);
        REQUIRE(ReadLe32(mapping, offset + 16) == expected.offset);
        REQUIRE(ReadLe32(mapping, offset + 20) == expected.phaseStep);
        REQUIRE(ReadLe32(mapping, offset + 24) == expected.phaseCount);
        REQUIRE(ReadLe32(mapping, offset + 28) == expected.sequenceOffset);
    }

    const std::vector<std::byte> digestFile = ReadGolden("mapping-stream.blake3");
    const std::string expectedDigest(kUnifiedMappingStreamFrameSequenceZeroBlake3);
    REQUIRE(std::string(reinterpret_cast<const char*>(digestFile.data()), digestFile.size()) == expectedDigest + "\n");
}

TEST_CASE("Unified LC4 lane mapping is collision-free and exactly invertible in every phase",
    "[pbmodulation][unified][mapping][bijection]")
{
    using namespace pbmodulation;
    for (std::uint64_t frameSequence = 0; frameSequence < kUnifiedMappingPhaseCount; frameSequence++)
    {
        std::vector<std::uint8_t> lumaOwners(static_cast<std::size_t>(kUnifiedVisualProfile.dataTileCount) * 4);
        std::vector<std::uint8_t> chromaOwners(static_cast<std::size_t>(kUnifiedVisualProfile.dataTileCount) * 2);
        for (std::size_t laneIndex = 0; laneIndex < kUnifiedLaneInterleaves.size(); laneIndex++)
        {
            const UnifiedLane lane = static_cast<UnifiedLane>(laneIndex);
            const UnifiedLaneInterleaveContract& contract = kUnifiedLaneInterleaves[laneIndex];
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
        REQUIRE(std::ranges::count_if(lumaOwners, [](const std::uint8_t owner) { return owner != 0; }) == 340200);
        REQUIRE(std::ranges::count_if(chromaOwners, [](const std::uint8_t owner) { return owner != 0; }) == 162000);
        REQUIRE(std::ranges::count(lumaOwners, 0) == 6552);
        REQUIRE(std::ranges::count(chromaOwners, 0) == 11376);
    }

    REQUIRE_FALSE(GetUnifiedPhysicalCarrierSite(UnifiedLane::BaseLuma, 275400, 0).valid);
    REQUIRE_FALSE(GetUnifiedPhysicalCarrierSite(UnifiedLane::FineLuma, 64800, 0).valid);
    REQUIRE_FALSE(GetUnifiedPhysicalCarrierSite(UnifiedLane::Chroma, 162000, 0).valid);
    REQUIRE_FALSE(GetUnifiedPhysicalCarrierSite(static_cast<UnifiedLane>(0xFF), 0, 0).valid);
    REQUIRE_FALSE(GetUnifiedLogicalCarrierBit({true, UnifiedCarrier::Luma, 86688, 0}, 0).valid);
    REQUIRE_FALSE(GetUnifiedLogicalCarrierBit({true, UnifiedCarrier::Luma, 0, 4}, 0).valid);
    REQUIRE_FALSE(GetUnifiedLogicalCarrierBit({true, UnifiedCarrier::Chroma, 0, 2}, 0).valid);
    REQUIRE_FALSE(GetUnifiedLogicalCarrierBit({true, static_cast<UnifiedCarrier>(0xFF), 0, 0}, 0).valid);
}

TEST_CASE("Unified LC4 public mapping rebuilds the frozen frame-zero stream digest",
    "[pbmodulation][unified][mapping][digest]")
{
    using namespace pbmodulation;
    constexpr char domain[] = "PixelBridge.UnifiedLc4MappingStream.1";
    pbprotocol::Blake3Hasher hasher;
    hasher.Update(std::as_bytes(std::span{domain, sizeof(domain)}));
    for (std::size_t laneIndex = 0; laneIndex < kUnifiedLaneInterleaves.size(); laneIndex++)
    {
        const UnifiedLane lane = static_cast<UnifiedLane>(laneIndex);
        for (std::uint32_t logicalBit = 0; logicalBit < kUnifiedLaneInterleaves[laneIndex].logicalBits; logicalBit++)
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
