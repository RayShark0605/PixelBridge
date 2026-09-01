#include "local_desktop_resample_fixtures.h"

#include "pbdesktoplevels/reference_channel.h"
#include "pbmodulation/remote_visual_low_fps.h"
#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/bootstrap_control_codec.h"

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

#ifndef PB_REMOTE_VISUAL_LF4_GOLDEN_DIR
#error PB_REMOTE_VISUAL_LF4_GOLDEN_DIR must name the independent LF4 Golden directory
#endif

namespace
{

constexpr std::uint64_t kGoldenSessionTag = 0x5354455030325244ULL;
constexpr std::uint64_t kGoldenFrameSequence = 17;

std::filesystem::path GoldenPath(const char* const name)
{
    return std::filesystem::path(PB_REMOTE_VISUAL_LF4_GOLDEN_DIR) / name;
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

std::uint64_t ReadLe64(const std::span<const std::byte> bytes, const std::size_t offset)
{
    REQUIRE(offset <= bytes.size());
    REQUIRE(bytes.size() - offset >= 8);
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; index++)
    {
        value |= static_cast<std::uint64_t>(std::to_integer<unsigned>(bytes[offset + index])) << (index * 8);
    }
    return value;
}

std::string ToHex(const std::span<const std::byte> bytes)
{
    constexpr char alphabet[] = "0123456789abcdef";
    std::string result(bytes.size() * 2, '0');
    for (std::size_t index = 0; index < bytes.size(); index++)
    {
        const unsigned value = std::to_integer<unsigned>(bytes[index]);
        result[index * 2] = alphabet[value >> 4];
        result[index * 2 + 1] = alphabet[value & 15];
    }
    return result;
}

std::array<std::byte, pbprotocol::kBootstrapRecordBytes> MakeGoldenRecord()
{
    pbprotocol::BootstrapRecord record;
    record.protocolVersion = pbprotocol::GetProtocolVersion();
    record.visualLayoutVersion = pbmodulation::kRemoteVisualLowFpsLayoutVersion;
    record.visualProfileId = pbmodulation::kRemoteVisualLowFpsProfileId;
    record.sessionTag.value = kGoldenSessionTag;
    record.frameSequence = kGoldenFrameSequence;
    std::array<std::byte, pbprotocol::kBootstrapRecordBytes> bytes{};
    REQUIRE(pbprotocol::SerializeBootstrapRecord(record, bytes));
    return bytes;
}

void RequireMagic(const std::span<const std::byte> content, const char* const expected)
{
    REQUIRE(content.size() >= 8);
    REQUIRE(std::equal(content.begin(), content.begin() + 8,
        reinterpret_cast<const std::byte*>(expected), reinterpret_cast<const std::byte*>(expected) + 8));
}

} // namespace

TEST_CASE("RemoteVisual LF4 independent Golden binds Bootstrap, codebook and every physical tile mapping",
    "[remote-visual][low-fps][golden][mapping]")
{
    const auto expectedRecord = MakeGoldenRecord();
    const auto goldenRecord = ReadGolden("lf4-bootstrap.bin");
    REQUIRE(std::ranges::equal(goldenRecord, expectedRecord));

    const auto codebook = ReadGolden("lf4-codebook.bin");
    REQUIRE(codebook.size() == pbmodulation::kRemoteVisualLowFpsSymbolMasks.size() * 2);
    for (std::size_t symbol = 0; symbol < pbmodulation::kRemoteVisualLowFpsSymbolMasks.size(); symbol++)
    {
        REQUIRE(ReadLe16(codebook, symbol * 2) == pbmodulation::kRemoteVisualLowFpsSymbolMasks[symbol]);
    }

    const auto mapping = ReadGolden("lf4-mapping.bin");
    RequireMagic(mapping, "PBLF4M01");
    REQUIRE(ReadLe32(mapping, 8) == 1);
    REQUIRE(ReadLe32(mapping, 12) == 20);
    REQUIRE(ReadLe32(mapping, 16) == pbmodulation::kRemoteVisualTileCount);
    REQUIRE(mapping.size() == 20 + static_cast<std::size_t>(pbmodulation::kRemoteVisualTileCount) * 20);
    std::uint32_t unusedTiles = 0;
    std::uint32_t freshnessTiles = 0;
    std::uint32_t dataTiles = 0;
    for (std::uint32_t physical = 0; physical < pbmodulation::kRemoteVisualTileCount; physical++)
    {
        const std::size_t offset = 20 + static_cast<std::size_t>(physical) * 20;
        REQUIRE(ReadLe32(mapping, offset) == physical);
        pbmodulation::LocalDesktopRegion region;
        pbmodulation::RemoteVisualTileMapping productionMapping;
        REQUIRE(pbmodulation::GetRemoteVisualTile(physical, region));
        REQUIRE(pbmodulation::GetRemoteVisualTileMapping(physical, productionMapping));
        REQUIRE(ReadLe16(mapping, offset + 4) == region.x);
        REQUIRE(ReadLe16(mapping, offset + 6) == region.y);
        REQUIRE(std::to_integer<unsigned>(mapping[offset + 8]) == region.width);
        REQUIRE(std::to_integer<unsigned>(mapping[offset + 9]) == region.height);
        REQUIRE(std::to_integer<unsigned>(mapping[offset + 10]) == static_cast<unsigned>(productionMapping.role));
        REQUIRE(std::to_integer<unsigned>(mapping[offset + 11]) == 0);
        REQUIRE(ReadLe16(mapping, offset + 12) == productionMapping.regionId);
        REQUIRE(ReadLe16(mapping, offset + 14) == 0);
        REQUIRE(ReadLe32(mapping, offset + 16) == productionMapping.dataOrdinal);
        unusedTiles += static_cast<std::uint32_t>(productionMapping.role == pbmodulation::RemoteVisualTileRole::Unused);
        freshnessTiles += static_cast<std::uint32_t>(productionMapping.role == pbmodulation::RemoteVisualTileRole::FreshnessTag);
        dataTiles += static_cast<std::uint32_t>(productionMapping.role == pbmodulation::RemoteVisualTileRole::Data);
    }
    REQUIRE(unusedTiles == 2344);
    REQUIRE(freshnessTiles == 2389);
    REQUIRE(dataTiles == pbmodulation::kRemoteVisualDataTileCount);
}

TEST_CASE("RemoteVisual LF4 independent Golden freezes the selected metric mapping and int16 adapter",
    "[remote-visual][low-fps][golden][metric]")
{
    const auto calibration = ReadGolden("lf4-metric-calibration.bin");
    RequireMagic(calibration, "PBLF4C01");
    REQUIRE(ReadLe32(calibration, 8) == pbmodulation::kRemoteVisualLowFpsMetricCalibrationVersion);
    REQUIRE(ReadLe32(calibration, 12) == 32);
    REQUIRE(ReadLe32(calibration, 16) == pbmodulation::kRemoteVisualLowFpsMetricCalibrationBins.size());
    REQUIRE(calibration.size() == 20 + pbmodulation::kRemoteVisualLowFpsMetricCalibrationBins.size() * 32);
    for (std::size_t index = 0; index < pbmodulation::kRemoteVisualLowFpsMetricCalibrationBins.size(); index++)
    {
        const std::size_t offset = 20 + index * 32;
        const auto& bin = pbmodulation::kRemoteVisualLowFpsMetricCalibrationBins[index];
        REQUIRE(std::bit_cast<double>(ReadLe64(calibration, offset)) == bin.rawMagnitudeUpper);
        REQUIRE(ReadLe32(calibration, offset + 8) == bin.calibratedMagnitudeFloatBits);
        REQUIRE(ReadLe32(calibration, offset + 12) == 0);
        REQUIRE(ReadLe64(calibration, offset + 16) == bin.trainingSamples);
        REQUIRE(ReadLe64(calibration, offset + 24) == bin.trainingErrors);
    }

    const auto probes = ReadGolden("lf4-metric-probes.bin");
    RequireMagic(probes, "PBLF4P01");
    REQUIRE(ReadLe32(probes, 8) == 1);
    REQUIRE(ReadLe32(probes, 12) == 16);
    const std::uint32_t probeCount = ReadLe32(probes, 16);
    REQUIRE(probes.size() == 20 + static_cast<std::size_t>(probeCount) * 16);
    REQUIRE(probeCount >= 100);
    for (std::uint32_t index = 0; index < probeCount; index++)
    {
        const std::size_t offset = 20 + static_cast<std::size_t>(index) * 16;
        const float raw = std::bit_cast<float>(ReadLe32(probes, offset));
        const bool expectedValid = std::to_integer<unsigned>(probes[offset + 4]) != 0;
        const bool expectedHardBit = std::to_integer<unsigned>(probes[offset + 5]) != 0;
        REQUIRE(ReadLe16(probes, offset + 6) == 0);
        const std::uint32_t expectedCalibratedBits = ReadLe32(probes, offset + 8);
        const std::int16_t expectedLlr = std::bit_cast<std::int16_t>(ReadLe16(probes, offset + 12));
        REQUIRE(ReadLe16(probes, offset + 14) == 0);
        float calibrated = std::bit_cast<float>(0x42280000U);
        const std::uint32_t originalOutputBits = std::bit_cast<std::uint32_t>(calibrated);
        REQUIRE(pbmodulation::CalibrateRemoteVisualLowFpsMetric(raw, calibrated) == expectedValid);
        if (!expectedValid)
        {
            REQUIRE(std::bit_cast<std::uint32_t>(calibrated) == originalOutputBits);
            continue;
        }
        REQUIRE(std::bit_cast<std::uint32_t>(calibrated) == expectedCalibratedBits);
        std::int16_t llr = 0;
        REQUIRE(pbdesktoplevels::AdaptSoftMetrics(std::span(&calibrated, 1), std::span(&llr, 1)));
        REQUIRE(llr == expectedLlr);
        constexpr float negativeDecisionThreshold = static_cast<float>(-0.5 / pbdesktoplevels::kSoftMetricScale);
        REQUIRE((calibrated <= negativeDecisionThreshold) == expectedHardBit);
    }
}

TEST_CASE("RemoteVisual LF4 production raster and calibrated Transport admission stay bit-exact with independent Golden",
    "[remote-visual][low-fps][golden][transport][integration]")
{
    const auto record = MakeGoldenRecord();
    const auto goldenData = ReadGolden("lf4-coded-data.bin");
    REQUIRE(goldenData.size() == pbmodulation::kRemoteVisualLowFpsDataBytes);
    std::vector<std::byte> generatedData(pbmodulation::kRemoteVisualLowFpsDataBytes);
    REQUIRE(pbdesktoplevels::GenerateDiagnosticData(record, generatedData));
    REQUIRE(generatedData == goldenData);

    std::vector<std::byte> raster(static_cast<std::size_t>(pbmodulation::kLocalDesktopCanvasWidth) *
        pbmodulation::kLocalDesktopCanvasHeight * 4);
    REQUIRE(pbmodulation::EncodeRemoteVisualLowFpsFrame(record, generatedData, raster));
    const auto digest = pbprotocol::ComputeBlake3Digest(raster);
    const auto digestPin = ReadGolden("lf4-raster.blake3");
    REQUIRE(digestPin.size() == 65);
    REQUIRE(std::to_integer<unsigned>(digestPin.back()) == '\n');
    REQUIRE(ToHex(digest) == std::string(reinterpret_cast<const char*>(digestPin.data()), 64));

    const auto image = localdesktoptest::GrayFromGolden(raster);
    auto workspaceResult = pbmodulation::RemoteVisualLowFpsWorkspace::Create(
        pbmodulation::RemoteVisualLowFpsWorkspace::RequiredBytes());
    REQUIRE(workspaceResult);
    auto workspace = std::move(workspaceResult.Value());
    std::vector<std::byte> hard(pbmodulation::kRemoteVisualLowFpsDataBytes);
    std::vector<float> rawMetrics(pbmodulation::kRemoteVisualLowFpsCodedBits);
    const auto observation = pbmodulation::DecodeRemoteVisualLowFpsFrame(image.View(), workspace, hard, rawMetrics);
    INFO(pbmodulation::GetRemoteVisualLowFpsErasureName(observation.erasure));
    REQUIRE(observation.IsAccepted());
    REQUIRE(hard == goldenData);
    std::vector<float> calibratedMetrics(rawMetrics.size());
    for (std::size_t index = 0; index < rawMetrics.size(); index++)
    {
        REQUIRE(pbmodulation::CalibrateRemoteVisualLowFpsMetric(rawMetrics[index], calibratedMetrics[index]));
    }
    std::vector<std::int16_t> quantized(calibratedMetrics.size());
    REQUIRE(pbdesktoplevels::AdaptSoftMetrics(calibratedMetrics, quantized));
    REQUIRE(std::ranges::all_of(quantized, [](const std::int16_t value) { return value == -32767 || value == 32767; }));

    auto channelResult = pbdesktoplevels::ReferenceChannel::Create(pbdesktoplevels::kProcessingReservationBytes);
    REQUIRE(channelResult);
    auto channel = std::move(channelResult.Value());
    const auto evaluation = channel.EvaluateCodewords(record, hard, calibratedMetrics,
        pbdesktoplevels::EvaluationMode::DiagnosticTruth);
    REQUIRE(evaluation.IsVerified());
    REQUIRE(evaluation.acceptedTransportBlocks == pbmodulation::kRemoteVisualLowFpsCodewords);
    const auto accepted = channel.GetAcceptedTransportBlocks();
    REQUIRE(accepted.size() == pbmodulation::kRemoteVisualLowFpsCodewords);
    for (std::uint32_t slot = 0; slot < accepted.size(); slot++)
    {
        const std::string name = "lf4-accepted-transport-" + std::to_string(slot) + ".bin";
        const auto expected = ReadGolden(name.c_str());
        REQUIRE(accepted[slot].slot == slot);
        REQUIRE(accepted[slot].byteCount == expected.size());
        REQUIRE(std::ranges::equal(expected, std::span(accepted[slot].bytes).first(accepted[slot].byteCount)));
    }
}
