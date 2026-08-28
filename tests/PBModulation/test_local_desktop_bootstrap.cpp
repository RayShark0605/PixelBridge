#include "local_desktop_test_fixtures.h"
#include "pbmodulation/frame_io.h"
#include "pbmodulation/local_desktop_bootstrap.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "../../libs/PBModulation/src/local_desktop_internal.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace
{

using namespace pbmodulation;
using Record = std::array<std::byte, 44>;

void RepairRecordCrc(Record& record) noexcept
{
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t offset = 0; offset < 40; offset++)
    {
        crc ^= std::to_integer<std::uint8_t>(record[offset]);
        for (std::uint32_t bit = 0; bit < 8; bit++)
        {
            crc = (crc >> 1) ^ ((crc & 1u) != 0 ? 0x82F63B78u : 0u);
        }
    }
    crc ^= 0xFFFFFFFFu;
    for (std::size_t index = 0; index < 4; index++)
    {
        record[40 + index] = static_cast<std::byte>((crc >> (index * 8)) & 255u);
    }
}

std::array<std::byte, 76> ReadBootstrapCells(const std::span<const std::byte> pixels, const bool second)
{
    REQUIRE(pixels.size() == std::size_t{1920} * 1080 * 4);
    const std::uint32_t originX = second ? 1216 : 96;
    const std::uint32_t originY = second ? 1000 : 16;
    std::array<std::byte, 76> result{};
    for (std::uint32_t index = 0; index < 608; index++)
    {
        const std::uint32_t x = originX + (index % 76) * 8 + 4;
        const std::uint32_t y = originY + (index / 76) * 8 + 4;
        const std::size_t offset = (std::size_t{y} * 1920 + x) * 4;
        const auto level = std::to_integer<std::uint8_t>(pixels[offset]);
        REQUIRE((level == 32 || level == 224));
        REQUIRE(pixels[offset] == pixels[offset + 1]);
        REQUIRE(pixels[offset] == pixels[offset + 2]);
        REQUIRE(pixels[offset + 3] == std::byte{255});
        result[index / 8] |= static_cast<std::byte>((level == 224 ? 1u : 0u) << (index % 8));
    }
    return result;
}

void RequireSamePixels(const std::span<const std::byte> actual, const std::span<const std::byte> expected)
{
    REQUIRE(actual.size() == expected.size());
    const auto difference = std::mismatch(actual.begin(), actual.end(), expected.begin());
    const auto firstDifferentByte = static_cast<std::size_t>(difference.first - actual.begin());
    INFO("First different BGRA byte: " << firstDifferentByte);
    REQUIRE(firstDifferentByte == actual.size());
}

} // namespace

TEST_CASE("LocalDesktop raster matches three independent raw and PBRW digest oracles", "[pbmodulation][local-desktop][raster][golden]")
{
    struct Golden
    {
        std::string_view stem;
        std::string_view rawDigest;
        std::string_view pbrwDigest;
    };
    constexpr std::array<Golden, 3> goldens{{
        {"a", "248b075d46be224f982633732960cd13e80be8eea4f7329554179216d5ef293f", "57d95004025a05d659fc8d4d58948e1f9f565fa8c04e579af03632cdf547e753"},
        {"b", "20717a03aa695e0e2f324fba1fadbb04b63fcd6a9d019bce09de07180dfbafac", "65cdf778f9aad77a84485952ae8f003e68f8e2bff910e83d2558c79734e18b9a"},
        {"c", "4fabf998261ce02c4e034ca8bbb5f2279484c90d7d984411f43a69d7189afaa5", "266b864e95df984f1cd82fa830962024dc63d66e53e1b5b58d06a4cb1681fdda"}}};
    for (const auto& golden : goldens)
    {
        CAPTURE(golden.stem);
        const auto record = localdesktoptest::LoadGoldenRecord(golden.stem);
        const auto expected = localdesktoptest::MakeGoldenRaster(golden.stem);
        REQUIRE(localdesktoptest::ToHex(pbprotocol::ComputeBlake3Digest(expected)) == golden.rawDigest);
        std::vector<std::byte> rendered(kLocalDesktopFrameBgraBytes, std::byte{0xB7});
        REQUIRE(EncodeLocalDesktopBootstrapFrame(record, rendered));
        RequireSamePixels(rendered, expected);
        CHECK(localdesktoptest::ToHex(pbprotocol::ComputeBlake3Digest(rendered)) == golden.rawDigest);
        const auto container = EncodeRawFrame(rendered, 1920, 1080);
        REQUIRE(container);
        CHECK(container.Value().size() == 8294428);
        CHECK(localdesktoptest::ToHex(pbprotocol::ComputeBlake3Digest(container.Value())) == golden.pbrwDigest);
        const auto first = ReadBootstrapCells(rendered, false);
        const auto second = ReadBootstrapCells(rendered, true);
        CHECK(first == second);
        const auto codeword = localdesktoptest::LoadGoldenBytes(std::string(golden.stem) + "-rs76.bin", 76);
        CHECK(std::equal(first.begin(), first.end(), codeword.begin()));
    }
}

TEST_CASE("LocalDesktop experimental identity and every fixed region are distinct from old reference", "[pbmodulation][local-desktop][layout]")
{
    CHECK(kLocalDesktopVisualProfileId == 0x50424C4442533031ULL);
    CHECK(kLocalDesktopLayoutVersion == 2);
    CHECK(kLocalDesktopCanvasWidth == 1920);
    CHECK(kLocalDesktopCanvasHeight == 1080);
    CHECK(kLocalDesktopCellPixels == 8);
    CHECK(kLocalDesktopBootstrapRecordBytes == pbprotocol::kBootstrapRecordBytes);
    CHECK(kLocalDesktopRsCodewordBytes == 76);
    constexpr std::array<LocalDesktopRegion, 4> markers{{{16, 16, 64, 64}, {1840, 16, 64, 64}, {16, 1000, 64, 64}, {1840, 1000, 64, 64}}};
    constexpr std::array<LocalDesktopRegion, 2> copies{{{96, 16, 608, 64}, {1216, 1000, 608, 64}}};
    constexpr std::array<LocalDesktopRegion, 9> pilots{{{96, 160, 128, 128}, {896, 160, 128, 128}, {1696, 160, 128, 128},
        {96, 476, 128, 128}, {896, 476, 128, 128}, {1696, 476, 128, 128}, {96, 792, 128, 128}, {896, 792, 128, 128}, {1696, 792, 128, 128}}};
    CHECK(kLocalDesktopMarkerRegions == markers);
    CHECK(kLocalDesktopBootstrapRegions == copies);
    CHECK(kLocalDesktopTimingRegions == pilots);
    const auto record = localdesktoptest::LoadGoldenRecord();
    const auto parsed = pbprotocol::ParseBootstrapRecord(record);
    REQUIRE(parsed);
    CHECK(parsed.Value().visualProfileId == kLocalDesktopVisualProfileId);
    CHECK(parsed.Value().visualLayoutVersion == kLocalDesktopLayoutVersion);
    CHECK(parsed.Value().sessionTag.value == 0x81DF204BD997BAD0ULL);
    CHECK(parsed.Value().frameSequence == 0x1112131415161718ULL);
}

TEST_CASE("LocalDesktop full redraw is deterministic and permits overlapped canonical input", "[pbmodulation][local-desktop][raster][atomicity]")
{
    const auto record = localdesktoptest::LoadGoldenRecord();
    const auto expected = localdesktoptest::MakeGoldenRaster();
    std::vector<std::byte> output(kLocalDesktopFrameBgraBytes + 32, std::byte{0xCA});
    auto raster = std::span(output).subspan(16, kLocalDesktopFrameBgraBytes);
    for (const std::size_t inputOffset : {std::size_t{0}, std::size_t{57}, kLocalDesktopFrameBgraBytes - 44})
    {
        CAPTURE(inputOffset);
        std::fill(raster.begin(), raster.end(), std::byte{0xA6});
        std::copy(record.begin(), record.end(), raster.begin() + static_cast<std::ptrdiff_t>(inputOffset));
        REQUIRE(EncodeLocalDesktopBootstrapFrame(raster.subspan(inputOffset, 44), raster));
        RequireSamePixels(raster, expected);
        REQUIRE(std::all_of(output.begin(), output.begin() + 16, [](const auto value) { return value == std::byte{0xCA}; }));
        REQUIRE(std::all_of(output.end() - 16, output.end(), [](const auto value) { return value == std::byte{0xCA}; }));
    }
    std::fill(raster.begin(), raster.end(), std::byte{0x33});
    REQUIRE(EncodeLocalDesktopBootstrapFrame(record, raster));
    RequireSamePixels(raster, expected);
}

TEST_CASE("LocalDesktop rejects noncanonical semantic input before modifying any pixel", "[pbmodulation][local-desktop][raster][failure]")
{
    struct InvalidCase
    {
        std::size_t changedOffset = 0;
        std::uint8_t replacement = 0;
        ModulationErrorCode expectedError = ModulationErrorCode::None;
        std::size_t errorOffset = 0;
        bool repairCrc = false;
    };
    constexpr std::array<InvalidCase, 8> cases{{
        {0, 0, ModulationErrorCode::InvalidMagic, 0, true},
        {4, 2, ModulationErrorCode::UnsupportedVersion, 4, true},
        {5, 2, ModulationErrorCode::UnsupportedVersion, 5, true},
        {6, 1, ModulationErrorCode::UnsupportedVersion, 6, true},
        {7, 1, ModulationErrorCode::UnsupportedVersion, 7, true},
        {8, 0, ModulationErrorCode::InvalidInput, 8, true},
        {36, 1, ModulationErrorCode::InvalidInput, 36, true},
        {40, 0, ModulationErrorCode::CrcMismatch, 40, false}}};
    const auto canonical = localdesktoptest::LoadGoldenRecord();
    std::vector<std::byte> output(kLocalDesktopFrameBgraBytes, std::byte{0xD2});
    const auto before = output;
    for (const auto& test : cases)
    {
        CAPTURE(test.changedOffset);
        auto record = canonical;
        record[test.changedOffset] = static_cast<std::byte>(test.replacement);
        if (test.repairCrc)
        {
            RepairRecordCrc(record);
        }
        const auto status = EncodeLocalDesktopBootstrapFrame(record, output);
        REQUIRE_FALSE(status);
        CHECK(status.Error().code == test.expectedError);
        CHECK(status.Error().offset == test.errorOffset);
        RequireSamePixels(output, before);
    }

    // Logical PB-Bootstrap-1 remains valid with its old profile; the new raster
    // nevertheless requires the explicit new mapping and cannot silently bind it.
    const auto oldProtocol = localdesktoptest::LoadGoldenBytes("protocol-rs76.bin", 76);
    REQUIRE(pbprotocol::ParseBootstrapRecord(std::span(oldProtocol).first(44)));
    REQUIRE_FALSE(EncodeLocalDesktopBootstrapFrame(std::span(oldProtocol).first(44), output));
    RequireSamePixels(output, before);

    // Failure with input stored in the output must preserve even that input.
    std::copy(canonical.begin(), canonical.end(), output.begin() + 7);
    output[7 + 40] ^= std::byte{0x80};
    const auto overlappedBefore = output;
    const auto status = EncodeLocalDesktopBootstrapFrame(std::span(output).subspan(7, 44), output);
    REQUIRE_FALSE(status);
    CHECK(status.Error().code == ModulationErrorCode::CrcMismatch);
    RequireSamePixels(output, overlappedBefore);
}

TEST_CASE("LocalDesktop raster checks exact bounded input and output lengths transactionally", "[pbmodulation][local-desktop][raster][bounds]")
{
    const auto record = localdesktoptest::LoadGoldenRecord();
    std::array<std::byte, 45> malformed{};
    std::copy(record.begin(), record.end(), malformed.begin());
    std::vector<std::byte> output(kLocalDesktopFrameBgraBytes + 1, std::byte{0x92});
    const auto before = output;
    for (const std::size_t length : {std::size_t{0}, std::size_t{1}, std::size_t{43}, std::size_t{45}})
    {
        CAPTURE(length);
        const auto status = EncodeLocalDesktopBootstrapFrame(std::span(malformed).first(length), std::span(output).first(kLocalDesktopFrameBgraBytes));
        REQUIRE_FALSE(status);
        CHECK(status.Error() == ModulationError{ModulationErrorCode::InvalidInput, 0});
        RequireSamePixels(output, before);
    }
    for (const std::size_t length : {std::size_t{0}, std::size_t{44}, kLocalDesktopFrameBgraBytes - 1, kLocalDesktopFrameBgraBytes + 1})
    {
        CAPTURE(length);
        const auto status = EncodeLocalDesktopBootstrapFrame(record, std::span(output).first(length));
        REQUIRE_FALSE(status);
        CHECK(status.Error().code == (length < kLocalDesktopFrameBgraBytes ? ModulationErrorCode::OutputBufferTooSmall : ModulationErrorCode::InvalidInput));
        CHECK(status.Error().offset == 0);
        RequireSamePixels(output, before);
    }
}

TEST_CASE("Marker role bits match the oracle and preserve central finder cross", "[pbmodulation][local-desktop][markers]")
{
    const auto expected = localdesktoptest::LoadGoldenBytes("marker-bits.bin", 196);
    constexpr std::array<std::uint8_t, 7> cross{0, 1, 0, 0, 0, 1, 0};
    for (std::size_t marker = 0; marker < 4; marker++)
    {
        for (std::uint32_t row = 0; row < 7; row++)
        {
            for (std::uint32_t column = 0; column < 7; column++)
            {
                CAPTURE(marker, row, column);
                CHECK(detail::MarkerModule(marker, column, row) == std::to_integer<std::uint8_t>(expected[marker * 49 + row * 7 + column]));
            }
            CHECK(detail::MarkerModule(marker, 3, row) == cross[row]);
            CHECK(detail::MarkerModule(marker, row, 3) == cross[row]);
        }
        for (std::size_t other = 0; other < marker; other++)
        {
            std::uint32_t distance = 0;
            for (std::size_t cell = 0; cell < 49; cell++)
            {
                distance += expected[marker * 49 + cell] != expected[other * 49 + cell] ? 1u : 0u;
            }
            CHECK(distance == 4);
        }
    }
    CHECK(detail::MarkerModule(4, 0, 0) == 0xFF);
    CHECK(detail::MarkerModule(std::numeric_limits<std::size_t>::max(), 0, 0) == 0xFF);
    CHECK(detail::MarkerModule(0, 7, 0) == 0xFF);
    CHECK(detail::MarkerModule(0, 0, 7) == 0xFF);
    CHECK(detail::MarkerModule(0, std::numeric_limits<std::uint32_t>::max(), 0) == 0xFF);
}

TEST_CASE("All distributed Manchester timing patches match independent digest bits", "[pbmodulation][local-desktop][timing][oracle]")
{
    for (const std::string_view stem : {"a", "b", "c"})
    {
        const auto record = localdesktoptest::LoadGoldenRecord(stem);
        const auto expected = localdesktoptest::LoadGoldenBytes(std::string(stem) + "-timing-bits.bin", 2304);
        for (std::size_t pilot = 0; pilot < 9; pilot++)
        {
            CAPTURE(stem, pilot);
            std::array<std::uint8_t, 256> actual{};
            REQUIRE(detail::BuildTimingBits(record, pilot, actual));
            for (std::size_t index = 0; index < actual.size(); index++)
            {
                REQUIRE(actual[index] == std::to_integer<std::uint8_t>(expected[pilot * 256 + index]));
            }
            for (std::size_t pair = 0; pair < 128; pair++)
            {
                CHECK((actual[pair * 2] ^ actual[pair * 2 + 1]) == 1);
            }
        }
    }
    const auto first = localdesktoptest::LoadGoldenBytes("a-timing-bits.bin", 2304);
    for (const std::string_view other : {"b-timing-bits.bin", "c-timing-bits.bin"})
    {
        const auto different = localdesktoptest::LoadGoldenBytes(other, 2304);
        for (std::size_t pilot = 0; pilot < 9; pilot++)
        {
            const auto offset = static_cast<std::ptrdiff_t>(pilot * 256);
            CHECK_FALSE(std::equal(first.begin() + offset, first.begin() + offset + 256, different.begin() + offset));
        }
    }
}

TEST_CASE("Timing preparation rejects invalid sizes and indices without writes", "[pbmodulation][local-desktop][timing][atomicity]")
{
    const auto record = localdesktoptest::LoadGoldenRecord();
    std::array<std::byte, 45> input{};
    std::copy(record.begin(), record.end(), input.begin());
    std::array<std::uint8_t, 257> output{};
    output.fill(0xE5);
    const auto before = output;
    for (const std::size_t length : {std::size_t{0}, std::size_t{43}, std::size_t{45}})
    {
        REQUIRE_FALSE(detail::BuildTimingBits(std::span(input).first(length), 0, std::span(output).first(256)));
        CHECK(output == before);
    }
    for (const std::size_t length : {std::size_t{0}, std::size_t{255}, std::size_t{257}})
    {
        REQUIRE_FALSE(detail::BuildTimingBits(record, 0, std::span(output).first(length)));
        CHECK(output == before);
    }
    for (const std::size_t pilot : {std::size_t{9}, std::numeric_limits<std::size_t>::max()})
    {
        REQUIRE_FALSE(detail::BuildTimingBits(record, pilot, std::span(output).first(256)));
        CHECK(output == before);
    }
}

TEST_CASE("Maximum sequence and session integers preserve both complete canonical copies", "[pbmodulation][local-desktop][raster][integers]")
{
    auto record = localdesktoptest::LoadGoldenRecord();
    std::fill(record.begin() + 16, record.begin() + 36, std::byte{255});
    RepairRecordCrc(record);
    const auto parsed = pbprotocol::ParseBootstrapRecord(record);
    REQUIRE(parsed);
    CHECK(parsed.Value().sessionTag.value == std::numeric_limits<std::uint64_t>::max());
    CHECK(parsed.Value().frameSequence == std::numeric_limits<std::uint64_t>::max());
    CHECK(parsed.Value().controlEpoch == std::numeric_limits<std::uint32_t>::max());
    std::vector<std::byte> output(kLocalDesktopFrameBgraBytes);
    REQUIRE(EncodeLocalDesktopBootstrapFrame(record, output));
    const auto first = ReadBootstrapCells(output, false);
    const auto second = ReadBootstrapCells(output, true);
    CHECK(first == second);
    Record decoded{};
    REQUIRE(detail::DecodeBootstrapRs(first, decoded));
    CHECK(decoded == record);
}
