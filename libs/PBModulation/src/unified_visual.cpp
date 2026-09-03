#include "pbmodulation/unified_visual.h"

#include "local_desktop_internal.h"
#include "pbinnerfec/qc_ldpc_codec.h"
#include "pbprotocol/transport_block_codec.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace pbmodulation
{

struct UnifiedVisualCpuOracle::Implementation
{
    explicit Implementation(pbinnerfec::QcLdpcDecoder&& decoderValue) noexcept : decoder(std::move(decoderValue))
    {
    }

    std::array<UnifiedSoftMetric, kUnifiedSoftMetricCount> metrics{};
    std::array<UnifiedAcceptedBlock, kUnifiedCodewordCount> accepted{};
    std::array<std::int16_t, kUnifiedVisualProfile.innerCodewordBits> slotMetrics{};
    std::array<std::byte, kUnifiedCodewordBytes> decodedCodeword{};
    pbinnerfec::QcLdpcDecoder decoder;
    std::uint32_t acceptedCount = 0;
    bool metricsValid = false;
};

namespace
{

struct Sample
{
    double blue = 0;
    double green = 0;
    double red = 0;
};

struct ScalarStatistics
{
    double sum = 0;
    double squareSum = 0;
    std::uint64_t count = 0;

    void Add(const double value) noexcept
    {
        sum += value;
        squareSum += value * value;
        count++;
    }

    [[nodiscard]] double Mean() const noexcept
    {
        return count == 0 ? 0 : sum / static_cast<double>(count);
    }

    [[nodiscard]] double StandardDeviation() const noexcept
    {
        if (count == 0)
        {
            return std::numeric_limits<double>::infinity();
        }
        const double mean = Mean();
        return std::sqrt(std::max(0.0, squareSum / static_cast<double>(count) - mean * mean));
    }
};

struct UnifiedCalibration
{
    std::array<double, 4> lumaLevels{};
    std::array<std::array<double, 2>, 4> chromaCentroids{};
    bool lumaValid = false;
    bool chromaValid = false;
};

[[nodiscard]] bool SpansOverlap(const std::span<const std::byte> first,
    const std::span<const std::byte> second) noexcept
{
    if (first.empty() || second.empty())
    {
        return false;
    }
    const std::uintptr_t firstAddress = reinterpret_cast<std::uintptr_t>(first.data());
    const std::uintptr_t secondAddress = reinterpret_cast<std::uintptr_t>(second.data());
    if (first.size() > std::numeric_limits<std::uintptr_t>::max() - firstAddress ||
        second.size() > std::numeric_limits<std::uintptr_t>::max() - secondAddress)
    {
        return true;
    }
    return firstAddress <= secondAddress ? secondAddress - firstAddress < first.size() :
        firstAddress - secondAddress < second.size();
}

[[nodiscard]] std::uint8_t CodeValue(const std::uint8_t base, const std::int16_t offset) noexcept
{
    return static_cast<std::uint8_t>(static_cast<std::int32_t>(base) + offset);
}

[[nodiscard]] bool ReadBit(const std::span<const std::byte> bytes, const std::size_t bitIndex) noexcept
{
    return ((std::to_integer<std::uint8_t>(bytes[bitIndex / 8]) >> (bitIndex % 8)) & 1U) != 0;
}

[[nodiscard]] bool ReadCodedBit(const std::span<const std::byte> codedFrame, const UnifiedLane lane,
    const std::uint32_t laneLogicalBit) noexcept
{
    const UnifiedLaneCapacity capacity = GetUnifiedLaneCapacity(lane);
    const std::size_t globalBit = static_cast<std::size_t>(capacity.firstCodewordSlot) *
        kUnifiedVisualProfile.innerCodewordBits + laneLogicalBit;
    return ReadBit(codedFrame, globalBit);
}

void FillPixel(const std::span<std::byte> pixels, const std::uint32_t x, const std::uint32_t y,
    const std::uint8_t blue, const std::uint8_t green, const std::uint8_t red) noexcept
{
    std::byte* const pixel = pixels.data() +
        (static_cast<std::size_t>(y) * kUnifiedVisualProfile.canvasWidth + x) * 4;
    pixel[0] = static_cast<std::byte>(blue);
    pixel[1] = static_cast<std::byte>(green);
    pixel[2] = static_cast<std::byte>(red);
    pixel[3] = std::byte{255};
}

void RenderCalibrationPilots(const std::span<std::byte> pixels) noexcept
{
    for (const UnifiedRegionContract& contract : kUnifiedVisualProfile.regions)
    {
        if (contract.kind != UnifiedRegionKind::Pilot ||
            !HasUnifiedPilotContent(contract.pilotContent, UnifiedPilotContent::CalibrationReferences))
        {
            continue;
        }
        const UnifiedPixelRegion region = contract.bounds;
        for (std::uint32_t row = 0; row < kUnifiedCalibrationLumaRows; row++)
        {
            for (std::uint32_t column = 0; column < region.width; column++)
            {
                const std::uint32_t levelIndex = column / (region.width / 4);
                const std::uint8_t level = kUnifiedLumaPilotLevels[levelIndex];
                FillPixel(pixels, region.x + column, region.y + row, level, level, level);
            }
        }
        for (std::uint32_t row = kUnifiedCalibrationLumaRows;
             row < kUnifiedCalibrationLumaRows + kUnifiedCalibrationNeutralRows; row++)
        {
            for (std::uint32_t column = 0; column < region.width; column++)
            {
                FillPixel(pixels, region.x + column, region.y + row,
                    kUnifiedNeutralLuma, kUnifiedNeutralLuma, kUnifiedNeutralLuma);
            }
        }
        for (std::uint32_t row = kUnifiedCalibrationLumaRows + kUnifiedCalibrationNeutralRows;
             row < region.height; row++)
        {
            for (std::uint32_t column = 0; column < region.width; column++)
            {
                const std::uint32_t label = column / (region.width / 4);
                const UnifiedChromaState state = kUnifiedChromaStatesByLabel[label];
                FillPixel(pixels, region.x + column, region.y + row,
                    CodeValue(kUnifiedNeutralLuma, state.blueOffset),
                    CodeValue(kUnifiedNeutralLuma, state.greenOffset),
                    CodeValue(kUnifiedNeutralLuma, state.redOffset));
            }
        }
    }
}

void RenderPhasePilots(const std::span<std::byte> pixels, const std::uint64_t frameSequence) noexcept
{
    bool finePilot = false;
    for (const UnifiedRegionContract& contract : kUnifiedVisualProfile.regions)
    {
        if (contract.kind != UnifiedRegionKind::Pilot ||
            !HasUnifiedPilotContent(contract.pilotContent, UnifiedPilotContent::PhaseChecker))
        {
            continue;
        }
        const UnifiedPixelRegion region = contract.bounds;
        const std::uint32_t tilesPerRow = region.width / kUnifiedVisualProfile.tileWidth;
        const std::uint32_t tileRows = region.height / kUnifiedVisualProfile.tileHeight;
        for (std::uint32_t tileRow = 0; tileRow < tileRows; tileRow++)
        {
            for (std::uint32_t tileColumn = 0; tileColumn < tilesPerRow; tileColumn++)
            {
                const std::uint32_t tileOrdinal = tileRow * tilesPerRow + tileColumn;
                const std::uint8_t label = GetUnifiedPhasePilotLabel(finePilot, tileOrdinal, frameSequence);
                const std::uint16_t mask = kUnifiedSymbolMasksByLabel[label];
                for (std::uint32_t row = 0; row < kUnifiedVisualProfile.tileHeight; row++)
                {
                    for (std::uint32_t column = 0; column < kUnifiedVisualProfile.tileWidth; column++)
                    {
                        const std::uint32_t chip = row * kUnifiedVisualProfile.tileWidth + column;
                        const std::uint8_t level = ((mask >> chip) & 1U) != 0 ?
                            kUnifiedDataHighLuma : kUnifiedDataLowLuma;
                        FillPixel(pixels, region.x + tileColumn * kUnifiedVisualProfile.tileWidth + column,
                            region.y + tileRow * kUnifiedVisualProfile.tileHeight + row, level, level, level);
                    }
                }
            }
        }
        finePilot = true;
    }
}

[[nodiscard]] Sample ReadSample(const LumaView& view, const std::uint32_t x, const std::uint32_t y) noexcept
{
    const std::byte* const pixel = view.pixels.data() + static_cast<std::size_t>(y) * view.rowPitch +
        static_cast<std::size_t>(x) * 4;
    return Sample{static_cast<double>(std::to_integer<std::uint8_t>(pixel[0])),
        static_cast<double>(std::to_integer<std::uint8_t>(pixel[1])),
        static_cast<double>(std::to_integer<std::uint8_t>(pixel[2]))};
}

[[nodiscard]] double Luma(const Sample& sample) noexcept
{
    return 0.0722 * sample.blue + 0.7152 * sample.green + 0.2126 * sample.red;
}

[[nodiscard]] std::array<double, 2> Opponent(const Sample& sample) noexcept
{
    const double luma = Luma(sample);
    return {sample.blue - luma, sample.red - luma};
}

[[nodiscard]] double SquaredDistance(const std::array<double, 2>& left,
    const std::array<double, 2>& right) noexcept
{
    const double blue = left[0] - right[0];
    const double red = left[1] - right[1];
    return blue * blue + red * red;
}

[[nodiscard]] bool IsClipped(const Sample& sample) noexcept
{
    return sample.blue <= 0 || sample.blue >= 255 || sample.green <= 0 || sample.green >= 255 ||
        sample.red <= 0 || sample.red >= 255;
}

[[nodiscard]] bool ValidPolicy(const UnifiedVisualDecodePolicy& policy) noexcept
{
    const std::array<double, 5> values{policy.minimumLumaLevelGap, policy.maximumPilotDeviation,
        policy.minimumChromaSeparation, policy.maximumPhasePilotResidual,
        policy.maximumTimingBitErrorFraction};
    return std::ranges::all_of(values, [](const double value)
    {
        return std::isfinite(value) && value >= 0;
    }) && policy.minimumLumaLevelGap >= 16 && policy.minimumLumaLevelGap <= 96 &&
        policy.maximumPilotDeviation <= 16 && policy.minimumChromaSeparation >= 8 &&
        policy.minimumChromaSeparation <= 128 && policy.maximumPhasePilotResidual <= 0.5 &&
        policy.maximumTimingBitErrorFraction <= 0.25 && policy.minimumDecisionMetric >= 0 &&
        policy.maximumFecIterations >= pbinnerfec::kQcLdpcMinIterations &&
        policy.maximumFecIterations <= pbinnerfec::kQcLdpcMaxIterations;
}

[[nodiscard]] UnifiedCalibration Calibrate(const LumaView& view,
    const UnifiedVisualDecodePolicy& policy) noexcept
{
    std::array<ScalarStatistics, 4> lumaStatistics{};
    std::array<ScalarStatistics, 4> chromaBlueStatistics{};
    std::array<ScalarStatistics, 4> chromaRedStatistics{};
    ScalarStatistics neutralBlueStatistics;
    ScalarStatistics neutralRedStatistics;
    std::array<std::array<double, 4>, 4> localLumaMeans{};
    std::array<std::array<std::array<double, 2>, 4>, 4> localChromaMeans{};
    std::size_t pilotIndex = 0;
    bool clipped = false;
    for (const UnifiedRegionContract& contract : kUnifiedVisualProfile.regions)
    {
        if (contract.kind != UnifiedRegionKind::Pilot ||
            !HasUnifiedPilotContent(contract.pilotContent, UnifiedPilotContent::CalibrationReferences))
        {
            continue;
        }
        const UnifiedPixelRegion region = contract.bounds;
        std::array<ScalarStatistics, 4> localLuma{};
        std::array<ScalarStatistics, 4> localChromaBlue{};
        std::array<ScalarStatistics, 4> localChromaRed{};
        for (std::uint32_t row = 0; row < region.height; row++)
        {
            for (std::uint32_t column = 0; column < region.width; column++)
            {
                const Sample sample = ReadSample(view, region.x + column, region.y + row);
                if (row < kUnifiedCalibrationLumaRows)
                {
                    const std::size_t label = column / (region.width / 4);
                    const double luma = Luma(sample);
                    lumaStatistics[label].Add(luma);
                    localLuma[label].Add(luma);
                }
                else if (row < kUnifiedCalibrationLumaRows + kUnifiedCalibrationNeutralRows)
                {
                    const std::array<double, 2> opponent = Opponent(sample);
                    neutralBlueStatistics.Add(opponent[0]);
                    neutralRedStatistics.Add(opponent[1]);
                }
                else
                {
                    const std::size_t label = column / (region.width / 4);
                    const std::array<double, 2> opponent = Opponent(sample);
                    chromaBlueStatistics[label].Add(opponent[0]);
                    chromaRedStatistics[label].Add(opponent[1]);
                    localChromaBlue[label].Add(opponent[0]);
                    localChromaRed[label].Add(opponent[1]);
                    clipped = clipped || IsClipped(sample);
                }
            }
        }
        for (std::size_t label = 0; label < 4; label++)
        {
            localLumaMeans[pilotIndex][label] = localLuma[label].Mean();
            localChromaMeans[pilotIndex][label] =
                {localChromaBlue[label].Mean(), localChromaRed[label].Mean()};
        }
        pilotIndex++;
    }

    UnifiedCalibration calibration;
    calibration.lumaValid = pilotIndex == 4;
    calibration.chromaValid = pilotIndex == 4 && !clipped;
    for (std::size_t label = 0; label < 4; label++)
    {
        calibration.lumaLevels[label] = lumaStatistics[label].Mean();
        calibration.chromaCentroids[label] =
            {chromaBlueStatistics[label].Mean(), chromaRedStatistics[label].Mean()};
        calibration.lumaValid = calibration.lumaValid &&
            lumaStatistics[label].StandardDeviation() <= policy.maximumPilotDeviation;
        calibration.chromaValid = calibration.chromaValid &&
            chromaBlueStatistics[label].StandardDeviation() <= policy.maximumPilotDeviation &&
            chromaRedStatistics[label].StandardDeviation() <= policy.maximumPilotDeviation;
        for (std::size_t localPilot = 0; localPilot < pilotIndex; localPilot++)
        {
            calibration.lumaValid = calibration.lumaValid &&
                std::abs(localLumaMeans[localPilot][label] - calibration.lumaLevels[label]) <= policy.maximumPilotDeviation;
            calibration.chromaValid = calibration.chromaValid &&
                std::sqrt(SquaredDistance(localChromaMeans[localPilot][label],
                    calibration.chromaCentroids[label])) <= policy.maximumPilotDeviation;
        }
        if (label > 0)
        {
            calibration.lumaValid = calibration.lumaValid &&
                calibration.lumaLevels[label] - calibration.lumaLevels[label - 1] >= policy.minimumLumaLevelGap;
        }
    }
    calibration.chromaValid = calibration.chromaValid &&
        std::hypot(neutralBlueStatistics.Mean(), neutralRedStatistics.Mean()) <= policy.maximumPilotDeviation;
    for (std::size_t label = 0; label < 4; label++)
    {
        for (std::size_t other = 0; other < label; other++)
        {
            calibration.chromaValid = calibration.chromaValid &&
                std::sqrt(SquaredDistance(calibration.chromaCentroids[label],
                    calibration.chromaCentroids[other])) >= policy.minimumChromaSeparation;
        }
    }
    return calibration;
}

[[nodiscard]] bool CheckPhasePilot(const LumaView& view, const UnifiedPixelRegion& region,
    const bool finePilot, const std::uint64_t frameSequence, const UnifiedCalibration& calibration,
    const UnifiedVisualDecodePolicy& policy) noexcept
{
    const std::uint32_t tilesPerRow = region.width / kUnifiedVisualProfile.tileWidth;
    const std::uint32_t tileRows = region.height / kUnifiedVisualProfile.tileHeight;
    const double low = calibration.lumaLevels[1];
    const double high = calibration.lumaLevels[2];
    const double gap = high - low;
    if (!(gap > 0))
    {
        return false;
    }
    double residual = 0;
    std::uint64_t samples = 0;
    for (std::uint32_t tileRow = 0; tileRow < tileRows; tileRow++)
    {
        for (std::uint32_t tileColumn = 0; tileColumn < tilesPerRow; tileColumn++)
        {
            const std::uint32_t tileOrdinal = tileRow * tilesPerRow + tileColumn;
            const std::uint8_t label = GetUnifiedPhasePilotLabel(finePilot, tileOrdinal, frameSequence);
            const std::uint16_t mask = kUnifiedSymbolMasksByLabel[label];
            for (std::uint32_t row = 0; row < kUnifiedVisualProfile.tileHeight; row++)
            {
                for (std::uint32_t column = 0; column < kUnifiedVisualProfile.tileWidth; column++)
                {
                    const std::uint32_t chip = row * kUnifiedVisualProfile.tileWidth + column;
                    const double expected = ((mask >> chip) & 1U) != 0 ? high : low;
                    residual += std::abs(Luma(ReadSample(view,
                        region.x + tileColumn * kUnifiedVisualProfile.tileWidth + column,
                        region.y + tileRow * kUnifiedVisualProfile.tileHeight + row)) - expected) / gap;
                    samples++;
                }
            }
        }
    }
    return samples != 0 && residual / static_cast<double>(samples) <= policy.maximumPhasePilotResidual;
}

void EvaluateFreshness(const LumaView& view, const LocalDesktopObservation& bootstrap,
    const std::span<const std::byte> canonicalRecord, const UnifiedVisualDecodePolicy& policy,
    std::array<UnifiedFreshnessObservation, kUnifiedFreshnessRegionCount>& observations) noexcept
{
    const double contrast = bootstrap.whiteLevel - bootstrap.blackLevel;
    for (std::size_t patch = 0; patch < observations.size(); patch++)
    {
        std::array<std::uint8_t, kLocalDesktopTimingBits> expected{};
        if (!detail::BuildTimingBits(canonicalRecord, patch, expected) || !(contrast > 0))
        {
            continue;
        }
        const UnifiedPixelRegion region = kUnifiedVisualProfile.regions[6 + patch].bounds;
        double residual = 0;
        std::uint16_t errors = 0;
        for (std::size_t bitIndex = 0; bitIndex < expected.size(); bitIndex++)
        {
            const std::uint32_t column = static_cast<std::uint32_t>(bitIndex % 16);
            const std::uint32_t row = static_cast<std::uint32_t>(bitIndex / 16);
            double sum = 0;
            for (std::uint32_t sampleRow = 2; sampleRow < 6; sampleRow++)
            {
                for (std::uint32_t sampleColumn = 2; sampleColumn < 6; sampleColumn++)
                {
                    sum += Luma(ReadSample(view, region.x + column * 8 + sampleColumn,
                        region.y + row * 8 + sampleRow));
                }
            }
            const double normalized = (sum / 16 - bootstrap.blackLevel) / contrast;
            residual += std::abs(normalized - expected[bitIndex]);
            errors += static_cast<std::uint16_t>((normalized >= 0.5) != (expected[bitIndex] != 0));
        }
        UnifiedFreshnessObservation& observation = observations[patch];
        observation.bitErrors = errors;
        observation.residual = residual / static_cast<double>(expected.size());
        observation.current = static_cast<double>(errors) / static_cast<double>(expected.size()) <=
            policy.maximumTimingBitErrorFraction && observation.residual <= policy.locator.maximumTimingResidual;
    }
}

[[nodiscard]] std::int16_t QuantizeMetric(const double value) noexcept
{
    if (!std::isfinite(value))
    {
        return 0;
    }
    const double rounded = std::round(value);
    const double bounded = std::clamp(rounded,
        static_cast<double>(std::numeric_limits<std::int16_t>::min()),
        static_cast<double>(std::numeric_limits<std::int16_t>::max()));
    return static_cast<std::int16_t>(bounded);
}

void InitializeMetrics(const std::uint64_t frameSequence, const UnifiedErasureReason reason,
    const std::span<UnifiedSoftMetric> metrics) noexcept
{
    for (const UnifiedLaneContract& laneContract : kUnifiedVisualProfile.lanes)
    {
        const UnifiedLaneCapacity capacity = GetUnifiedLaneCapacity(laneContract.lane);
        for (std::uint32_t logicalBit = 0; logicalBit < capacity.codedBits; logicalBit++)
        {
            const UnifiedPhysicalCarrierSite site = GetUnifiedPhysicalCarrierSite(laneContract.lane,
                logicalBit, frameSequence);
            const UnifiedDataTile tile = GetUnifiedDataTile(site.tileOrdinal);
            const std::size_t globalBit = static_cast<std::size_t>(capacity.firstCodewordSlot) *
                kUnifiedVisualProfile.innerCodewordBits + logicalBit;
            metrics[globalBit] = UnifiedSoftMetric{0, laneContract.lane,
                static_cast<std::uint8_t>(capacity.firstCodewordSlot +
                    logicalBit / kUnifiedVisualProfile.innerCodewordBits),
                tile.dataRegion, tile.freshnessRegion, reason};
        }
    }
}

[[nodiscard]] bool LaneAvailable(const UnifiedLane lane, const UnifiedBaseLumaObservation& base,
    const UnifiedFineLumaObservation& fine, const UnifiedChromaObservation& chroma) noexcept
{
    if (lane == UnifiedLane::BaseLuma)
    {
        return base.IsAvailable();
    }
    if (lane == UnifiedLane::FineLuma)
    {
        return fine.IsAvailable();
    }
    return lane == UnifiedLane::Chroma && chroma.IsAvailable();
}

[[nodiscard]] UnifiedErasureReason LaneReason(const UnifiedLane lane,
    const UnifiedBaseLumaObservation& base, const UnifiedFineLumaObservation& fine,
    const UnifiedChromaObservation& chroma) noexcept
{
    if (lane == UnifiedLane::BaseLuma)
    {
        return base.erasureReason;
    }
    if (lane == UnifiedLane::FineLuma)
    {
        return fine.erasureReason;
    }
    return chroma.erasureReason;
}

void StoreMetric(const UnifiedLogicalCarrierBit logical, const UnifiedDataTile& tile,
    const std::int16_t value, const bool clipped, const std::array<UnifiedFreshnessObservation,
    kUnifiedFreshnessRegionCount>& freshness, const UnifiedVisualDecodePolicy& policy,
    const UnifiedBaseLumaObservation& base, const UnifiedFineLumaObservation& fine,
    const UnifiedChromaObservation& chroma, const std::span<UnifiedSoftMetric> metrics) noexcept
{
    if (!logical.valid)
    {
        return;
    }
    const UnifiedLaneCapacity capacity = GetUnifiedLaneCapacity(logical.lane);
    const std::size_t globalBit = static_cast<std::size_t>(capacity.firstCodewordSlot) *
        kUnifiedVisualProfile.innerCodewordBits + logical.logicalBit;
    UnifiedSoftMetric& metric = metrics[globalBit];
    if (!LaneAvailable(logical.lane, base, fine, chroma))
    {
        metric.erasureReason = LaneReason(logical.lane, base, fine, chroma);
    }
    else if (!freshness[tile.freshnessRegion].current)
    {
        metric.erasureReason = UnifiedErasureReason::LocalStaleRegion;
    }
    else if (clipped)
    {
        metric.erasureReason = UnifiedErasureReason::LocalSamplingFailure;
    }
    else if (std::abs(static_cast<std::int32_t>(value)) < policy.minimumDecisionMetric)
    {
        metric.erasureReason = UnifiedErasureReason::LocalLowDecisionMargin;
    }
    else
    {
        metric.value = value;
        metric.erasureReason = UnifiedErasureReason::None;
    }
}

void DecodeDataTiles(const LumaView& view, const std::uint64_t frameSequence,
    const UnifiedCalibration& calibration, const UnifiedVisualDecodePolicy& policy,
    const std::array<UnifiedFreshnessObservation, kUnifiedFreshnessRegionCount>& freshness,
    const UnifiedBaseLumaObservation& base, const UnifiedFineLumaObservation& fine,
    const UnifiedChromaObservation& chroma, const std::span<UnifiedSoftMetric> metrics) noexcept
{
    const double low = calibration.lumaLevels[1];
    const double high = calibration.lumaLevels[2];
    const double lumaGap = high - low;
    const double lumaScale = calibration.lumaValid && lumaGap > 0 ? 2048 / (lumaGap * lumaGap) : 0;
    for (std::uint32_t tileOrdinal = 0; tileOrdinal < kUnifiedVisualProfile.dataTileCount; tileOrdinal++)
    {
        const UnifiedDataTile tile = GetUnifiedDataTile(tileOrdinal);
        std::array<Sample, 16> samples{};
        bool clipped = false;
        for (std::uint32_t row = 0; row < kUnifiedVisualProfile.tileHeight; row++)
        {
            for (std::uint32_t column = 0; column < kUnifiedVisualProfile.tileWidth; column++)
            {
                Sample& sample = samples[row * kUnifiedVisualProfile.tileWidth + column];
                sample = ReadSample(view, tile.bounds.x + column, tile.bounds.y + row);
                clipped = clipped || IsClipped(sample);
            }
        }

        std::array<double, 16> lumaDistances{};
        for (std::size_t label = 0; label < lumaDistances.size(); label++)
        {
            const std::uint16_t mask = kUnifiedSymbolMasksByLabel[label];
            for (std::size_t chip = 0; chip < samples.size(); chip++)
            {
                const double expected = ((mask >> chip) & 1U) != 0 ? high : low;
                const double difference = Luma(samples[chip]) - expected;
                lumaDistances[label] += difference * difference;
            }
        }
        for (std::uint8_t bitPlane = 0; bitPlane < 4; bitPlane++)
        {
            double zeroDistance = std::numeric_limits<double>::infinity();
            double oneDistance = std::numeric_limits<double>::infinity();
            for (std::size_t label = 0; label < lumaDistances.size(); label++)
            {
                double& destination = ((label >> bitPlane) & 1U) != 0 ? oneDistance : zeroDistance;
                destination = std::min(destination, lumaDistances[label]);
            }
            const std::int16_t metric = QuantizeMetric((oneDistance - zeroDistance) * lumaScale);
            const UnifiedLogicalCarrierBit logical = GetUnifiedLogicalCarrierBit(
                UnifiedPhysicalCarrierSite{true, UnifiedCarrier::Luma, tileOrdinal, bitPlane}, frameSequence);
            StoreMetric(logical, tile, metric, clipped, freshness, policy, base, fine, chroma, metrics);
        }

        std::array<double, 2> averageOpponent{};
        for (const Sample& sample : samples)
        {
            const std::array<double, 2> opponent = Opponent(sample);
            averageOpponent[0] += opponent[0] / static_cast<double>(samples.size());
            averageOpponent[1] += opponent[1] / static_cast<double>(samples.size());
        }
        std::array<double, 4> chromaDistances{};
        for (std::size_t label = 0; label < chromaDistances.size(); label++)
        {
            chromaDistances[label] = SquaredDistance(averageOpponent, calibration.chromaCentroids[label]);
        }
        for (std::uint8_t bitPlane = 0; bitPlane < 2; bitPlane++)
        {
            double zeroDistance = std::numeric_limits<double>::infinity();
            double oneDistance = std::numeric_limits<double>::infinity();
            for (std::size_t label = 0; label < chromaDistances.size(); label++)
            {
                double& destination = ((label >> bitPlane) & 1U) != 0 ? oneDistance : zeroDistance;
                destination = std::min(destination, chromaDistances[label]);
            }
            const std::int16_t metric = QuantizeMetric((oneDistance - zeroDistance) * 4);
            const UnifiedLogicalCarrierBit logical = GetUnifiedLogicalCarrierBit(
                UnifiedPhysicalCarrierSite{true, UnifiedCarrier::Chroma, tileOrdinal, bitPlane}, frameSequence);
            StoreMetric(logical, tile, metric, clipped, freshness, policy, base, fine, chroma, metrics);
        }
    }
}

[[nodiscard]] std::uint32_t ReadLe32(const std::byte* bytes) noexcept
{
    return std::to_integer<std::uint32_t>(bytes[0]) |
        (std::to_integer<std::uint32_t>(bytes[1]) << 8U) |
        (std::to_integer<std::uint32_t>(bytes[2]) << 16U) |
        (std::to_integer<std::uint32_t>(bytes[3]) << 24U);
}

[[nodiscard]] bool ControlTypeMatchesPriority(const pbprotocol::ControlRecordType type,
    const UnifiedControlPriority priority) noexcept
{
    return (priority == UnifiedControlPriority::SessionDescriptor &&
            type == pbprotocol::ControlRecordType::SessionDescriptor) ||
        (priority == UnifiedControlPriority::FinalManifest && type == pbprotocol::ControlRecordType::FinalManifest) ||
        (priority == UnifiedControlPriority::CurrentSegmentDescriptor &&
            type == pbprotocol::ControlRecordType::SegmentDescriptor);
}

void AcceptBlock(const std::uint32_t slot, const UnifiedSlotKind kind,
    const std::span<const std::byte> bytes, UnifiedSlotObservation& slotObservation,
    const std::span<UnifiedAcceptedBlock> accepted, std::uint32_t& acceptedCount) noexcept
{
    UnifiedAcceptedBlock& output = accepted[acceptedCount];
    output = {};
    output.codewordSlot = static_cast<std::uint8_t>(slot);
    output.kind = kind;
    output.size = static_cast<std::uint32_t>(bytes.size());
    std::copy(bytes.begin(), bytes.end(), output.bytes.begin());
    acceptedCount++;
    slotObservation.accepted = true;
    slotObservation.acceptedBytes = static_cast<std::uint32_t>(bytes.size());
    slotObservation.rejection = UnifiedSlotRejection::None;
}

void EvaluateAcceptedInformation(const std::uint32_t slot, const UnifiedSlotAssignment& assignment,
    const pbprotocol::SessionTag sessionTag, const std::span<const std::byte> information,
    UnifiedSlotObservation& slotObservation, const std::span<UnifiedAcceptedBlock> accepted,
    std::uint32_t& acceptedCount) noexcept
{
    if (assignment.kind == UnifiedSlotKind::Transport)
    {
        const auto extracted = pbprotocol::ExtractTransportBlockFromInfoBlock(information);
        if (!extracted)
        {
            if (extracted.Error().code == pbprotocol::ProtocolErrorCode::NonCanonicalPadding)
            {
                slotObservation.rejection = UnifiedSlotRejection::NonCanonicalPadding;
            }
            else if (extracted.Error().code == pbprotocol::ProtocolErrorCode::CrcMismatch)
            {
                slotObservation.rejection = UnifiedSlotRejection::TransportCrcFailure;
            }
            else
            {
                slotObservation.rejection = UnifiedSlotRejection::InvalidInformation;
            }
            return;
        }
        slotObservation.paddingValid = true;
        slotObservation.crcValid = true;
        const auto parsed = pbprotocol::ParseTransportBlock(extracted.Value());
        if (!parsed)
        {
            slotObservation.rejection = UnifiedSlotRejection::InvalidInformation;
            return;
        }
        if (parsed.Value().header.sessionTag != sessionTag)
        {
            slotObservation.rejection = UnifiedSlotRejection::IdentityFailure;
            return;
        }
        slotObservation.identityValid = true;
        AcceptBlock(slot, assignment.kind, extracted.Value(), slotObservation, accepted, acceptedCount);
        return;
    }

    constexpr std::size_t controlRecordBytesOffset = pbprotocol::kControlRecordPrefixBytes - sizeof(std::uint32_t);
    const std::uint32_t recordBytes = ReadLe32(information.data() + controlRecordBytesOffset);
    if (recordBytes < pbprotocol::kMinimumControlRecordBytes || recordBytes > information.size())
    {
        slotObservation.rejection = UnifiedSlotRejection::InvalidInformation;
        return;
    }
    const std::span<const std::byte> padding = information.subspan(recordBytes);
    if (std::ranges::any_of(padding, [](const std::byte value) { return value != std::byte{0}; }))
    {
        slotObservation.rejection = UnifiedSlotRejection::NonCanonicalPadding;
        return;
    }
    slotObservation.paddingValid = true;
    const std::span<const std::byte> record = information.first(recordBytes);
    const auto parsed = pbprotocol::ParseControlRecord(record);
    if (!parsed)
    {
        slotObservation.rejection = parsed.Error().code == pbprotocol::ProtocolErrorCode::CrcMismatch ?
            UnifiedSlotRejection::ControlCrcFailure : UnifiedSlotRejection::InvalidInformation;
        return;
    }
    slotObservation.crcValid = true;
    if (!ControlTypeMatchesPriority(parsed.Value().recordType, assignment.controlPriority))
    {
        slotObservation.rejection = UnifiedSlotRejection::InvalidInformation;
        return;
    }
    if (parsed.Value().sessionTag != sessionTag)
    {
        slotObservation.rejection = UnifiedSlotRejection::IdentityFailure;
        return;
    }
    slotObservation.identityValid = true;
    AcceptBlock(slot, assignment.kind, record, slotObservation, accepted, acceptedCount);
}

} // namespace

UnifiedDataTile GetUnifiedDataTile(std::uint32_t tileOrdinal) noexcept
{
    LocalDesktopRegion historicalTile;
    if (!GetLocalDesktopDataTile(kUnifiedVisualProfile.tileWidth, tileOrdinal, historicalTile))
    {
        return {};
    }
    std::uint8_t dataRegion = 0;
    for (const UnifiedRegionContract& contract : kUnifiedVisualProfile.regions)
    {
        if (contract.kind != UnifiedRegionKind::Data)
        {
            continue;
        }
        if (historicalTile.x >= contract.bounds.x && historicalTile.y >= contract.bounds.y &&
            historicalTile.x + historicalTile.width <= contract.bounds.x + contract.bounds.width &&
            historicalTile.y + historicalTile.height <= contract.bounds.y + contract.bounds.height)
        {
            const std::uint32_t x = historicalTile.x;
            const std::uint32_t y = historicalTile.y;
            const std::uint32_t centerX = x + kUnifiedVisualProfile.tileWidth / 2;
            const std::uint32_t centerY = y + kUnifiedVisualProfile.tileHeight / 2;
            const std::uint8_t freshnessColumn = centerX < kUnifiedFreshnessColumnBoundaries[0] ? 0 :
                centerX < kUnifiedFreshnessColumnBoundaries[1] ? 1 : 2;
            const std::uint8_t freshnessRow = centerY < kUnifiedFreshnessRowBoundaries[0] ? 0 :
                centerY < kUnifiedFreshnessRowBoundaries[1] ? 1 : 2;
            return UnifiedDataTile{true,
                UnifiedPixelRegion{x, y, kUnifiedVisualProfile.tileWidth, kUnifiedVisualProfile.tileHeight},
                dataRegion, static_cast<std::uint8_t>(freshnessRow * 3 + freshnessColumn)};
        }
        dataRegion++;
    }
    return {};
}

std::uint8_t GetUnifiedPhasePilotLabel(const bool finePilot, const std::uint32_t tileOrdinal,
    const std::uint64_t frameSequence) noexcept
{
    const std::uint8_t phase = static_cast<std::uint8_t>(frameSequence % kUnifiedMappingPhaseCount);
    if (!finePilot)
    {
        return static_cast<std::uint8_t>((tileOrdinal + phase) & 7U);
    }
    const std::uint8_t base = static_cast<std::uint8_t>(((tileOrdinal / 2) + phase) & 7U);
    const std::uint8_t fine = static_cast<std::uint8_t>(((tileOrdinal + phase) & 1U) << 3U);
    return static_cast<std::uint8_t>(base | fine);
}

ModulationStatus EncodeUnifiedVisualFrame(const std::span<const std::byte> bootstrapRecord,
    const std::span<const std::byte> codedFrame, const std::span<std::byte> outBgra) noexcept
{
    if (bootstrapRecord.data() == nullptr || codedFrame.data() == nullptr || outBgra.data() == nullptr ||
        bootstrapRecord.size() != kLocalDesktopBootstrapRecordBytes || codedFrame.size() != kUnifiedCodedFrameBytes)
    {
        return ModulationStatus::Failure(ModulationErrorCode::InvalidInput, 0);
    }
    if (outBgra.size() != kUnifiedFrameBgraBytes)
    {
        return ModulationStatus::Failure(outBgra.size() < kUnifiedFrameBgraBytes ?
            ModulationErrorCode::OutputBufferTooSmall : ModulationErrorCode::InvalidInput, 0);
    }
    if (SpansOverlap(bootstrapRecord, outBgra) || SpansOverlap(codedFrame, outBgra))
    {
        return ModulationStatus::Failure(ModulationErrorCode::InvalidInput, 0);
    }
    const auto parsed = pbprotocol::ParseBootstrapRecord(bootstrapRecord);
    if (!parsed)
    {
        return ModulationStatus::Failure(parsed.Error().code == pbprotocol::ProtocolErrorCode::CrcMismatch ?
            ModulationErrorCode::CrcMismatch : ModulationErrorCode::InvalidInput, parsed.Error().offset);
    }
    if (parsed.Value().visualProfileId != kUnifiedVisualProfile.productProfile.visualProfileId ||
        parsed.Value().visualLayoutVersion != kUnifiedVisualProfile.productProfile.visualLayoutVersion)
    {
        return ModulationStatus::Failure(ModulationErrorCode::InvalidInput, 8);
    }
    const ModulationStatus scaffold = detail::EncodeLocalDesktopScaffold(
        bootstrapRecord, outBgra, detail::LocalDesktopBinding::UnifiedVisual);
    if (!scaffold)
    {
        return scaffold;
    }
    RenderCalibrationPilots(outBgra);
    RenderPhasePilots(outBgra, parsed.Value().frameSequence);
    for (std::uint32_t tileOrdinal = 0; tileOrdinal < kUnifiedVisualProfile.dataTileCount; tileOrdinal++)
    {
        std::uint8_t lumaLabel = 0;
        for (std::uint8_t bitPlane = 0; bitPlane < 4; bitPlane++)
        {
            const UnifiedLogicalCarrierBit logical = GetUnifiedLogicalCarrierBit(
                UnifiedPhysicalCarrierSite{true, UnifiedCarrier::Luma, tileOrdinal, bitPlane},
                parsed.Value().frameSequence);
            if (logical.valid && ReadCodedBit(codedFrame, logical.lane, logical.logicalBit))
            {
                lumaLabel = static_cast<std::uint8_t>(lumaLabel | (1U << bitPlane));
            }
        }
        std::uint8_t chromaLabel = 0;
        for (std::uint8_t bitPlane = 0; bitPlane < 2; bitPlane++)
        {
            const UnifiedLogicalCarrierBit logical = GetUnifiedLogicalCarrierBit(
                UnifiedPhysicalCarrierSite{true, UnifiedCarrier::Chroma, tileOrdinal, bitPlane},
                parsed.Value().frameSequence);
            if (logical.valid && ReadCodedBit(codedFrame, logical.lane, logical.logicalBit))
            {
                chromaLabel = static_cast<std::uint8_t>(chromaLabel | (1U << bitPlane));
            }
        }
        const UnifiedDataTile tile = GetUnifiedDataTile(tileOrdinal);
        const std::uint16_t mask = kUnifiedSymbolMasksByLabel[lumaLabel];
        const UnifiedChromaState state = kUnifiedChromaStatesByLabel[chromaLabel];
        for (std::uint32_t row = 0; row < kUnifiedVisualProfile.tileHeight; row++)
        {
            for (std::uint32_t column = 0; column < kUnifiedVisualProfile.tileWidth; column++)
            {
                const std::uint32_t chip = row * kUnifiedVisualProfile.tileWidth + column;
                const std::uint8_t base = ((mask >> chip) & 1U) != 0 ?
                    kUnifiedDataHighLuma : kUnifiedDataLowLuma;
                FillPixel(outBgra, tile.bounds.x + column, tile.bounds.y + row,
                    CodeValue(base, state.blueOffset), CodeValue(base, state.greenOffset),
                    CodeValue(base, state.redOffset));
            }
        }
    }
    return ModulationStatus::Success();
}

UnifiedVisualCpuOracle::UnifiedVisualCpuOracle() noexcept = default;
UnifiedVisualCpuOracle::UnifiedVisualCpuOracle(UnifiedVisualCpuOracle&& other) noexcept = default;
UnifiedVisualCpuOracle& UnifiedVisualCpuOracle::operator=(UnifiedVisualCpuOracle&& other) noexcept = default;
UnifiedVisualCpuOracle::~UnifiedVisualCpuOracle() = default;

std::uint64_t UnifiedVisualCpuOracle::RequiredBytes() noexcept
{
    // PBInnerFec documents a worst-case decoder workspace below 1 MiB. Keep
    // that separately charged from the fixed implementation object.
    return sizeof(Implementation) + 1024ULL * 1024ULL;
}

ModulationResult<UnifiedVisualCpuOracle> UnifiedVisualCpuOracle::Create(const std::uint64_t maximumBytes) noexcept
{
    if (maximumBytes < RequiredBytes())
    {
        return ModulationResult<UnifiedVisualCpuOracle>::Failure(ModulationErrorCode::InvalidInput, 0);
    }
    auto decoderResult = pbinnerfec::QcLdpcDecoder::Create(pbinnerfec::kInnerFecProfileIdRobust);
    if (!decoderResult)
    {
        return ModulationResult<UnifiedVisualCpuOracle>::Failure(
            decoderResult.Error().code == pbinnerfec::InnerFecErrorCode::OutOfMemory ?
                ModulationErrorCode::MemoryAllocationFailure : ModulationErrorCode::InternalInvariantViolation, 0);
    }
    try
    {
        UnifiedVisualCpuOracle oracle;
        oracle.implementation_ = std::make_unique<Implementation>(std::move(decoderResult).Value());
        return ModulationResult<UnifiedVisualCpuOracle>::Success(std::move(oracle));
    }
    catch (const std::bad_alloc&)
    {
        return ModulationResult<UnifiedVisualCpuOracle>::Failure(ModulationErrorCode::MemoryAllocationFailure, 0);
    }
}

UnifiedVisualObservation UnifiedVisualCpuOracle::Decode(const LumaView& view,
    const std::span<const UnifiedSlotAssignment> slotPlan, const UnifiedExpectedFrameIdentity& expectedIdentity,
    const UnifiedVisualDecodePolicy& policy) noexcept
{
    UnifiedVisualObservation observation;
    if (!implementation_)
    {
        return observation;
    }
    Implementation& state = *implementation_;
    state.acceptedCount = 0;
    state.metricsValid = false;
    InitializeMetrics(0, UnifiedErasureReason::CanvasClipped, state.metrics);
    state.metricsValid = true;
    if (!ValidPolicy(policy) || !ValidateUnifiedMixedSlotPlan(slotPlan) ||
        ValidateLumaView(view) != LocalDesktopErasureReason::None || view.pixelFormat != LumaPixelFormat::Bgra8 ||
        view.width != kUnifiedVisualProfile.canvasWidth || view.height != kUnifiedVisualProfile.canvasHeight)
    {
        return observation;
    }
    observation.inputValid = true;
    std::array<UnifiedSlotAssignment, kUnifiedCodewordCount> assignmentsBySlot{};
    for (const UnifiedSlotAssignment& assignment : slotPlan)
    {
        assignmentsBySlot[assignment.codewordSlot] = assignment;
        UnifiedSlotObservation& slotObservation = observation.slots[assignment.codewordSlot];
        slotObservation.kind = assignment.kind;
        const UnifiedLaneContract* const lane = FindUnifiedLaneForCodewordSlot(assignment.codewordSlot);
        slotObservation.lane = lane == nullptr ? UnifiedLane::BaseLuma : lane->lane;
    }

    const LocalDesktopBootstrapBinding binding{kUnifiedVisualProfile.productProfile.visualProfileId,
        kUnifiedVisualProfile.productProfile.visualLayoutVersion};
    observation.bootstrap = detail::DecodeLocalDesktopFixedCanvasScaffold(
        view, policy.locator, detail::LocalDesktopBinding::UnifiedVisual, binding);
    if (!observation.bootstrap.IsAccepted())
    {
        switch (observation.bootstrap.erasure)
        {
        case LocalDesktopErasureReason::MarkersNotFound:
        case LocalDesktopErasureReason::IncompleteMarkers:
        case LocalDesktopErasureReason::InvalidGeometry:
        case LocalDesktopErasureReason::AmbiguousGeometry:
        case LocalDesktopErasureReason::LowContrast:
            observation.frameErasure = UnifiedErasureReason::LocatorFailure;
            break;
        default:
            observation.frameErasure = UnifiedErasureReason::BootstrapFailure;
            break;
        }
        InitializeMetrics(0, observation.frameErasure, state.metrics);
        return observation;
    }
    const auto parsedBootstrap = pbprotocol::ParseBootstrapRecord(observation.bootstrap.canonical44);
    if (!parsedBootstrap)
    {
        observation.frameErasure = UnifiedErasureReason::BootstrapFailure;
        InitializeMetrics(0, observation.frameErasure, state.metrics);
        return observation;
    }
    observation.bootstrapRecord = parsedBootstrap.Value();
    InitializeMetrics(observation.bootstrapRecord.frameSequence, UnifiedErasureReason::LocalSamplingFailure,
        state.metrics);
    if ((expectedIdentity.requireSessionTag && expectedIdentity.sessionTag != observation.bootstrapRecord.sessionTag) ||
        (expectedIdentity.requireFrameSequence && expectedIdentity.frameSequence != observation.bootstrapRecord.frameSequence))
    {
        observation.frameErasure = UnifiedErasureReason::IdentityConflict;
        InitializeMetrics(observation.bootstrapRecord.frameSequence, observation.frameErasure, state.metrics);
        return observation;
    }
    observation.frameErasure = UnifiedErasureReason::None;
    EvaluateFreshness(view, observation.bootstrap, observation.bootstrap.canonical44, policy, observation.freshness);
    const UnifiedCalibration calibration = Calibrate(view, policy);
    if (!calibration.lumaValid)
    {
        observation.baseLuma = {UnifiedErasureScope::Lane, UnifiedErasureReason::BaseLumaPilotFailure};
        observation.fineLuma = {UnifiedErasureScope::Lane, UnifiedErasureReason::FineLumaPilotFailure};
    }
    if (!calibration.chromaValid)
    {
        observation.chroma = {UnifiedErasureScope::Lane, UnifiedErasureReason::ChromaPilotFailure};
    }
    bool finePilot = false;
    for (const UnifiedRegionContract& contract : kUnifiedVisualProfile.regions)
    {
        if (contract.kind != UnifiedRegionKind::Pilot ||
            !HasUnifiedPilotContent(contract.pilotContent, UnifiedPilotContent::PhaseChecker))
        {
            continue;
        }
        const bool phaseValid = calibration.lumaValid && CheckPhasePilot(view, contract.bounds, finePilot,
            observation.bootstrapRecord.frameSequence, calibration, policy);
        if (!phaseValid && finePilot)
        {
            observation.fineLuma = {UnifiedErasureScope::Lane, UnifiedErasureReason::FineLumaPilotFailure};
        }
        else if (!phaseValid)
        {
            observation.baseLuma = {UnifiedErasureScope::Lane, UnifiedErasureReason::BaseLumaPilotFailure};
        }
        finePilot = true;
    }
    DecodeDataTiles(view, observation.bootstrapRecord.frameSequence, calibration, policy, observation.freshness,
        observation.baseLuma, observation.fineLuma, observation.chroma, state.metrics);

    const pbinnerfec::InnerFecDecodeOptions decodeOptions{policy.maximumFecIterations, 1, 2048, 3, 4};
    for (std::uint32_t slot = 0; slot < kUnifiedCodewordCount; slot++)
    {
        UnifiedSlotObservation& slotObservation = observation.slots[slot];
        if (!LaneAvailable(slotObservation.lane, observation.baseLuma, observation.fineLuma, observation.chroma))
        {
            slotObservation.rejection = UnifiedSlotRejection::LaneErasure;
            continue;
        }
        state.decodedCodeword.fill(std::byte{0});
        const std::size_t firstMetric = static_cast<std::size_t>(slot) * kUnifiedVisualProfile.innerCodewordBits;
        for (std::size_t bit = 0; bit < state.slotMetrics.size(); bit++)
        {
            const std::int16_t metric = state.metrics[firstMetric + bit].value;
            state.slotMetrics[bit] = metric;
            if (metric < 0)
            {
                state.decodedCodeword[bit / 8] |= static_cast<std::byte>(1U << (bit % 8));
            }
        }
        const auto syndrome = pbinnerfec::ComputeQcLdpcSyndrome(
            pbinnerfec::kInnerFecProfileIdRobust, state.decodedCodeword);
        if (!syndrome)
        {
            slotObservation.rejection = UnifiedSlotRejection::InnerFecFailure;
            continue;
        }
        if (!syndrome.Value())
        {
            const auto decoded = state.decoder.Decode(state.slotMetrics, decodeOptions, state.decodedCodeword);
            if (!decoded)
            {
                slotObservation.iterationsUsed = static_cast<std::uint32_t>(decoded.Error().detail);
                slotObservation.rejection = UnifiedSlotRejection::InnerFecFailure;
                continue;
            }
            slotObservation.iterationsUsed = decoded.Value().iterationsUsed;
        }
        slotObservation.fecValid = true;
        EvaluateAcceptedInformation(slot, assignmentsBySlot[slot], observation.bootstrapRecord.sessionTag,
            std::span<const std::byte>(state.decodedCodeword).first(kUnifiedInformationBytes), slotObservation,
            state.accepted, state.acceptedCount);
        if (slotObservation.accepted)
        {
            observation.acceptedBlocks++;
            if (slotObservation.kind == UnifiedSlotKind::Transport)
            {
                observation.acceptedTransportBlocks++;
            }
            else
            {
                observation.acceptedControlRecords++;
            }
        }
    }
    return observation;
}

std::span<const UnifiedSoftMetric> UnifiedVisualCpuOracle::GetSoftMetrics() const noexcept
{
    return implementation_ && implementation_->metricsValid ?
        std::span<const UnifiedSoftMetric>(implementation_->metrics) : std::span<const UnifiedSoftMetric>{};
}

std::span<const UnifiedAcceptedBlock> UnifiedVisualCpuOracle::GetAcceptedBlocks() const noexcept
{
    return implementation_ ? std::span<const UnifiedAcceptedBlock>(implementation_->accepted).first(
        implementation_->acceptedCount) : std::span<const UnifiedAcceptedBlock>{};
}

} // namespace pbmodulation
