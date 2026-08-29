#include "pbdesktoplevels/reference_channel.h"
#include "pbinnerfec/qc_ldpc_codec.h"
#include "pbinterleave/tile_permutation.h"
#include "pbmodulation/desktop_levels.h"
#include "pbmodulation/shape_chroma.h"
#include "pbprotocol/bootstrap_control_codec.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <vector>

namespace
{

std::array<std::byte, 44> Record(const std::uint64_t sequence = 0)
{
    pbprotocol::BootstrapRecord record;
    record.protocolVersion = pbprotocol::GetProtocolVersion();
    record.visualLayoutVersion = pbmodulation::kShapeChromaLayoutVersion;
    record.visualProfileId = pbmodulation::kShapeChromaProfileId;
    record.sessionTag.value = 0x1020304050607080ULL;
    record.frameSequence = sequence;
    std::array<std::byte, 44> bytes{};
    REQUIRE(pbprotocol::SerializeBootstrapRecord(record, bytes));
    return bytes;
}

pbmodulation::LumaView View(const std::span<const std::byte> pixels)
{
    return {pixels, 1920, 1080, 1920 * 4, pbmodulation::LumaPixelFormat::Bgra8};
}

void SetBit(const std::span<std::byte> bytes, const std::size_t bit, const bool value)
{
    const auto mask = static_cast<std::byte>(1u << (bit % 8));
    if (value)
    {
        bytes[bit / 8] |= mask;
    }
    else
    {
        bytes[bit / 8] &= ~mask;
    }
}

bool GetBit(const std::span<const std::byte> bytes, const std::size_t bit)
{
    return (bytes[bit / 8] & static_cast<std::byte>(1u << (bit % 8))) != std::byte{0};
}

struct LiteralColor
{
    int blue;
    int green;
    int red;
    unsigned label;
};

constexpr std::array<std::uint16_t, 16> literalShapes{
    0x00FF, 0xFF00, 0x3333, 0xCCCC, 0x0FF0, 0xF00F, 0x6666, 0x9999,
    0x7331, 0x8CCE, 0x1337, 0xECC8, 0x8C73, 0x738C, 0x13EC, 0xEC13};
constexpr std::array<LiteralColor, 4> literalColors{
    LiteralColor{-24, 7, -16, 0}, LiteralColor{24, 2, -16, 1},
    LiteralColor{24, -7, 16, 3}, LiteralColor{-24, -2, 16, 2}};

void PaintSymbol(const std::span<std::byte> pixels, const std::uint32_t logical, const std::uint64_t sequence,
                 const unsigned shape, const unsigned chromaLabel)
{
    const auto& permutation = *pbinterleave::GetDesktopLevelsPermutation(4);
    const std::uint32_t physical = permutation.ToPhysical(logical, sequence);
    pbmodulation::LocalDesktopRegion region;
    REQUIRE(pbmodulation::GetLocalDesktopDataTile(4, physical, region));
    const auto color = *std::find_if(literalColors.begin(), literalColors.end(),
        [chromaLabel](const LiteralColor candidate) { return candidate.label == chromaLabel; });
    for (std::uint32_t row = 0; row < 4; row++)
    {
        for (std::uint32_t column = 0; column < 4; column++)
        {
            const unsigned pixel = row * 4 + column;
            const int base = (literalShapes[shape] & (1u << pixel)) != 0 ? 176 : 80;
            const std::size_t offset = (static_cast<std::size_t>(region.y + row) * 1920 + region.x + column) * 4;
            pixels[offset] = static_cast<std::byte>(base + color.blue);
            pixels[offset + 1] = static_cast<std::byte>(base + color.green);
            pixels[offset + 2] = static_cast<std::byte>(base + color.red);
            pixels[offset + 3] = std::byte{255};
        }
    }
}

} // namespace

TEST_CASE("ShapeChroma literal codebook is balanced, unique and approximately iso-luma", "[shape-chroma][codebook]")
{
    REQUIRE(pbmodulation::kShapeChromaTemplates == literalShapes);
    for (std::size_t index = 0; index < literalShapes.size(); index++)
    {
        REQUIRE(std::popcount(literalShapes[index]) == 8);
        REQUIRE(std::find(literalShapes.begin(), literalShapes.begin() + index, literalShapes[index]) == literalShapes.begin() + index);
    }
    for (std::size_t index = 0; index < literalColors.size(); index++)
    {
        const auto expected = literalColors[index];
        const auto actual = pbmodulation::kShapeChromaStates[index];
        REQUIRE(actual.blueOffset == expected.blue);
        REQUIRE(actual.greenOffset == expected.green);
        REQUIRE(actual.redOffset == expected.red);
        REQUIRE(actual.label == expected.label);
        const double lumaOffset = 0.0722 * expected.blue + 0.7152 * expected.green + 0.2126 * expected.red;
        REQUIRE(std::abs(lumaOffset) < 0.25);
    }
    REQUIRE(pbmodulation::kShapeChromaTileCount * pbmodulation::kShapeChromaBitsPerTile == pbmodulation::kShapeChromaDataBytes * 8);
    REQUIRE(pbmodulation::kShapeChromaCodewords * pbdesktoplevels::kCodewordBytes + pbmodulation::kShapeChromaPaddingBytes ==
        pbmodulation::kShapeChromaDataBytes);
}

TEST_CASE("ShapeChroma renderer maps all 64 shape and chroma combinations through the shared grid", "[shape-chroma][raster]")
{
    const auto record = Record(5);
    std::vector<std::byte> data(pbmodulation::kShapeChromaDataBytes);
    for (std::uint32_t logical = 0; logical < 64; logical++)
    {
        const unsigned shape = logical % 16;
        const unsigned chromaLabel = logical / 16;
        const std::size_t firstBit = static_cast<std::size_t>(logical) * 6;
        for (unsigned bit = 0; bit < 4; bit++)
        {
            SetBit(data, firstBit + bit, (shape & (1u << bit)) != 0);
        }
        for (unsigned bit = 0; bit < 2; bit++)
        {
            SetBit(data, firstBit + 4 + bit, (chromaLabel & (1u << bit)) != 0);
        }
    }
    std::vector<std::byte> pixels(1920 * 1080 * 4, std::byte{0xA5});
    REQUIRE(pbmodulation::EncodeShapeChromaFrame(record, data, pixels));
    const auto& permutation = *pbinterleave::GetDesktopLevelsPermutation(4);
    for (std::uint32_t logical = 0; logical < 64; logical++)
    {
        pbmodulation::LocalDesktopRegion region;
        REQUIRE(pbmodulation::GetLocalDesktopDataTile(4, permutation.ToPhysical(logical, 5), region));
        const unsigned shape = logical % 16;
        const unsigned chromaLabel = logical / 16;
        const auto color = *std::find_if(literalColors.begin(), literalColors.end(),
            [chromaLabel](const LiteralColor candidate) { return candidate.label == chromaLabel; });
        for (std::uint32_t row = 0; row < 4; row++)
        {
            for (std::uint32_t column = 0; column < 4; column++)
            {
                const unsigned pixel = row * 4 + column;
                const int base = (literalShapes[shape] & (1u << pixel)) != 0 ? 176 : 80;
                const std::size_t offset = (static_cast<std::size_t>(region.y + row) * 1920 + region.x + column) * 4;
                REQUIRE(std::to_integer<int>(pixels[offset]) == base + color.blue);
                REQUIRE(std::to_integer<int>(pixels[offset + 1]) == base + color.green);
                REQUIRE(std::to_integer<int>(pixels[offset + 2]) == base + color.red);
                REQUIRE(pixels[offset + 3] == std::byte{255});
            }
        }
    }
}

TEST_CASE("ShapeChroma CPU oracle and shared Transport FEC interface recover exact accepted blocks", "[shape-chroma][fec][integration]")
{
    for (const std::uint64_t sequence : {0ULL, 7ULL, 15ULL})
    {
        const auto record = Record(sequence);
        std::vector<std::byte> data(pbmodulation::kShapeChromaDataBytes);
        REQUIRE(pbdesktoplevels::GenerateDiagnosticData(record, data));
        std::vector<std::byte> pixels(1920 * 1080 * 4);
        REQUIRE(pbmodulation::EncodeShapeChromaFrame(record, data, pixels));
        auto created = pbdesktoplevels::ReferenceChannel::Create(pbdesktoplevels::kProcessingReservationBytes);
        REQUIRE(created);
        auto channel = std::move(created).Value();
        const auto observation = channel.DecodeShapeChroma(View(pixels));
        REQUIRE(observation.modulation.IsAccepted());
        REQUIRE(observation.modulation.unreliableShapeTiles == 0);
        REQUIRE(observation.modulation.unreliableChromaTiles == 0);
        REQUIRE(observation.modulation.shapeMargin.samples == pbmodulation::kShapeChromaTileCount);
        REQUIRE(observation.modulation.chromaMargin.samples == pbmodulation::kShapeChromaTileCount);
        REQUIRE(observation.evaluation.IsVerified());
        REQUIRE(observation.evaluation.codewords == pbmodulation::kShapeChromaCodewords);
        REQUIRE(observation.evaluation.acceptedTransportBlocks == pbmodulation::kShapeChromaCodewords);
        REQUIRE(observation.evaluation.erroneousCodedBits == 0);
        const auto accepted = channel.GetAcceptedTransportBlocks();
        REQUIRE(accepted.size() == pbmodulation::kShapeChromaCodewords);
        for (std::size_t slot = 0; slot < accepted.size(); slot++)
        {
            REQUIRE(accepted[slot].slot == slot);
            REQUIRE(std::equal(accepted[slot].bytes.begin(), accepted[slot].bytes.end(), data.begin() + slot * pbdesktoplevels::kCodewordBytes));
        }
        REQUIRE(channel.GetShapeMarginHistogram().size() == pbmodulation::kShapeChromaMetricBins);
        REQUIRE(channel.GetChromaMarginHistogram().size() == pbmodulation::kShapeChromaMetricBins);
    }
}

TEST_CASE("ShapeChroma soft signs follow LSB-first bits and one visual bit error is corrected without truth injection", "[shape-chroma][soft][fec]")
{
    const auto record = Record(3);
    std::vector<std::byte> data(pbmodulation::kShapeChromaDataBytes);
    REQUIRE(pbdesktoplevels::GenerateDiagnosticData(record, data));
    std::vector<std::byte> pixels(1920 * 1080 * 4);
    REQUIRE(pbmodulation::EncodeShapeChromaFrame(record, data, pixels));
    const unsigned originalShape = std::to_integer<unsigned>(data[0]) & 15u;
    const unsigned originalChroma = (std::to_integer<unsigned>(data[0]) >> 4) & 3u;
    PaintSymbol(pixels, 0, 3, originalShape ^ 1u, originalChroma);

    auto workspaceResult = pbmodulation::ShapeChromaWorkspace::Create(pbmodulation::ShapeChromaWorkspace::RequiredBytes());
    REQUIRE(workspaceResult);
    auto workspace = std::move(workspaceResult).Value();
    std::vector<std::byte> hard(pbmodulation::kShapeChromaDataBytes, std::byte{0x5A});
    std::vector<float> soft(pbmodulation::kShapeChromaMaximumBits, std::numeric_limits<float>::quiet_NaN());
    const auto demod = pbmodulation::DecodeShapeChromaFrame(View(pixels), workspace, hard, soft);
    REQUIRE(demod.IsAccepted());
    REQUIRE(GetBit(hard, 0) != GetBit(data, 0));
    for (std::size_t bit = 0; bit < 256; bit++)
    {
        REQUIRE(std::isfinite(soft[bit]));
        REQUIRE((soft[bit] < 0) == GetBit(hard, bit));
    }
    auto channelResult = pbdesktoplevels::ReferenceChannel::Create(pbdesktoplevels::kProcessingReservationBytes);
    REQUIRE(channelResult);
    auto channel = std::move(channelResult).Value();
    const auto evaluation = channel.EvaluateCodewords(record, hard, soft);
    REQUIRE(evaluation.IsVerified());
    REQUIRE(evaluation.erroneousCodedBits == 1);
    REQUIRE(evaluation.iterationsMaximum > 0);
    REQUIRE(evaluation.falseAcceptedCodewords == 0);
}

TEST_CASE("ShapeChroma fails atomically on malformed bindings, padding, format, policy and output bounds", "[shape-chroma][errors]")
{
    const auto record = Record();
    std::vector<std::byte> data(pbmodulation::kShapeChromaDataBytes);
    std::vector<std::byte> pixels(1920 * 1080 * 4, std::byte{0x3C});
    const auto savedPixels = pixels;
    data.back() = std::byte{1};
    const auto padding = pbmodulation::EncodeShapeChromaFrame(record, data, pixels);
    REQUIRE_FALSE(padding);
    REQUIRE(padding.Error().code == pbmodulation::ModulationErrorCode::NonZeroReservedByte);
    REQUIRE(pixels == savedPixels);
    data.back() = std::byte{0};
    REQUIRE(pbmodulation::EncodeShapeChromaFrame(record, data, pixels));

    auto workspaceResult = pbmodulation::ShapeChromaWorkspace::Create(pbmodulation::ShapeChromaWorkspace::RequiredBytes());
    REQUIRE(workspaceResult);
    auto workspace = std::move(workspaceResult).Value();
    std::vector<std::byte> hard(pbmodulation::kShapeChromaDataBytes, std::byte{0x6D});
    std::vector<float> soft(pbmodulation::kShapeChromaMaximumBits, 123.0f);
    const auto savedHard = hard;
    const auto savedSoft = soft;

    auto grayView = View(pixels);
    grayView.pixelFormat = pbmodulation::LumaPixelFormat::Gray8;
    grayView.rowPitch = 1920;
    grayView.pixels = std::span(pixels).first(1920 * 1080);
    REQUIRE(pbmodulation::DecodeShapeChromaFrame(grayView, workspace, hard, soft).erasure == pbmodulation::ShapeChromaErasure::UnsupportedFormat);
    REQUIRE(hard == savedHard);
    REQUIRE(soft == savedSoft);

    pbmodulation::ShapeChromaDecodePolicy invalidPolicy;
    invalidPolicy.maximumShapeResidual = std::numeric_limits<double>::quiet_NaN();
    REQUIRE(pbmodulation::DecodeShapeChromaFrame(View(pixels), workspace, hard, soft, invalidPolicy).erasure == pbmodulation::ShapeChromaErasure::InvalidPolicy);
    REQUIRE(hard == savedHard);
    REQUIRE(soft == savedSoft);

    REQUIRE(pbmodulation::DecodeShapeChromaFrame(View(pixels), workspace, std::span(hard).first(hard.size() - 1), soft).erasure ==
        pbmodulation::ShapeChromaErasure::OutputBufferTooSmall);
    REQUIRE(hard == savedHard);
    REQUIRE(soft == savedSoft);

    pbmodulation::ShapeChromaDecodePolicy workPolicy;
    workPolicy.maximumDataWorkUnits = 1;
    const auto exhausted = pbmodulation::DecodeShapeChromaFrame(View(pixels), workspace, hard, soft, workPolicy);
    REQUIRE(exhausted.erasure == pbmodulation::ShapeChromaErasure::WorkBudgetExceeded);
    REQUIRE(hard == savedHard);
    REQUIRE(soft == savedSoft);
}
