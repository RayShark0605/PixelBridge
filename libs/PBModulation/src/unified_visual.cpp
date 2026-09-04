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
    std::array<std::array<std::byte, kUnifiedInformationBytes>, kUnifiedCodewordCount> decodedInformation{};
    std::array<bool, kUnifiedCodewordCount> decodedInformationValid{};
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

[[nodiscard]] Sample ReadPixel(const LumaView& view, const std::uint32_t x, const std::uint32_t y) noexcept
{
    const std::byte* const pixel = view.pixels.data() + static_cast<std::size_t>(y) * view.rowPitch +
        static_cast<std::size_t>(x) * 4;
    return Sample{static_cast<double>(std::to_integer<std::uint8_t>(pixel[0])),
        static_cast<double>(std::to_integer<std::uint8_t>(pixel[1])),
        static_cast<double>(std::to_integer<std::uint8_t>(pixel[2]))};
}

[[nodiscard]] bool ReadSample(const LumaView& view, const LocalDesktopGeometry& geometry,
    const double logicalX, const double logicalY, Sample& output) noexcept
{
    const double physicalX = geometry.originX + geometry.scaleX * (logicalX + 0.5) - 0.5;
    const double physicalY = geometry.originY + geometry.scaleY * (logicalY + 0.5) - 0.5;
    if (!std::isfinite(physicalX) || !std::isfinite(physicalY) || physicalX < 0 || physicalY < 0 ||
        physicalX > static_cast<double>(view.width - 1) || physicalY > static_cast<double>(view.height - 1))
    {
        return false;
    }
    // A provider downscale has already integrated source support into each
    // captured pixel. Bilinear interpolation on that axis would apply a second
    // low-pass filter and corrupt the frozen 4x4 chip decisions; select the
    // nearest captured coordinate while retaining continuous geometry on an
    // independently magnified axis.
    const bool horizontalDownscale = geometry.scaleX < 1;
    const bool verticalDownscale = geometry.scaleY < 1;
    const std::uint32_t left = horizontalDownscale ?
        std::min(static_cast<std::uint32_t>(std::floor(physicalX + 0.5)), view.width - 1) :
        static_cast<std::uint32_t>(std::floor(physicalX));
    const std::uint32_t top = verticalDownscale ?
        std::min(static_cast<std::uint32_t>(std::floor(physicalY + 0.5)), view.height - 1) :
        static_cast<std::uint32_t>(std::floor(physicalY));
    const double horizontal = horizontalDownscale ? 0 : physicalX - left;
    const double vertical = verticalDownscale ? 0 : physicalY - top;
    const std::uint32_t right = horizontal == 0 ? left : left + 1;
    const std::uint32_t bottom = vertical == 0 ? top : top + 1;
    const Sample topLeft = ReadPixel(view, left, top);
    const Sample topRight = right == left ? topLeft : ReadPixel(view, right, top);
    const Sample bottomLeft = bottom == top ? topLeft : ReadPixel(view, left, bottom);
    const Sample bottomRight = bottom == top ? topRight :
        (right == left ? bottomLeft : ReadPixel(view, right, bottom));
    const auto Interpolate = [horizontal, vertical](const double topLeftValue, const double topRightValue,
        const double bottomLeftValue, const double bottomRightValue) noexcept
    {
        const double upper = topLeftValue + horizontal * (topRightValue - topLeftValue);
        const double lower = bottomLeftValue + horizontal * (bottomRightValue - bottomLeftValue);
        return upper + vertical * (lower - upper);
    };
    output = Sample{Interpolate(topLeft.blue, topRight.blue, bottomLeft.blue, bottomRight.blue),
        Interpolate(topLeft.green, topRight.green, bottomLeft.green, bottomRight.green),
        Interpolate(topLeft.red, topRight.red, bottomLeft.red, bottomRight.red)};
    return true;
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

[[nodiscard]] double GetUnifiedMinimumScale() noexcept
{
    return static_cast<double>(kUnifiedVisualProfile.presentation.minimumScaleNumerator) /
        kUnifiedVisualProfile.presentation.minimumScaleDenominator;
}

[[nodiscard]] double GetUnifiedMaximumScale() noexcept
{
    return static_cast<double>(kUnifiedVisualProfile.presentation.maximumScaleNumerator) /
        kUnifiedVisualProfile.presentation.maximumScaleDenominator;
}

[[nodiscard]] LocalDesktopDecodePolicy GetUnifiedLocatorPolicy(const UnifiedVisualDecodePolicy& policy) noexcept
{
    LocalDesktopDecodePolicy locator = policy.locator;
    locator.minimumScale = std::max(locator.minimumScale, GetUnifiedMinimumScale());
    locator.maximumScale = std::min(locator.maximumScale, GetUnifiedMaximumScale());
    return locator;
}

[[nodiscard]] bool ResolveUnifiedSamplingGeometryInternal(const LocalDesktopGeometry& geometry,
    const std::uint32_t frameWidth, const std::uint32_t frameHeight,
    const LocalDesktopDecodePolicy& locatorPolicy, LocalDesktopGeometry& output) noexcept
{
    const std::array<double, 5> values{geometry.originX, geometry.originY, geometry.scaleX, geometry.scaleY,
        geometry.markerResidualPixels};
    const double scaleTolerance = kLocalDesktopGeometryRefinementConvergencePixels /
        static_cast<double>(std::min(kUnifiedVisualProfile.canvasWidth, kUnifiedVisualProfile.canvasHeight));
    if (frameWidth == 0 || frameHeight == 0 || !std::ranges::all_of(values, [](const double value)
        {
            return std::isfinite(value);
        }) || geometry.scaleX < GetUnifiedMinimumScale() - scaleTolerance ||
        geometry.scaleX > GetUnifiedMaximumScale() + scaleTolerance ||
        geometry.scaleY < GetUnifiedMinimumScale() - scaleTolerance ||
        geometry.scaleY > GetUnifiedMaximumScale() + scaleTolerance ||
        geometry.markerResidualPixels < 0)
    {
        return false;
    }

    LocalDesktopGeometry candidate = geometry;
    candidate.scaleX = std::clamp(candidate.scaleX, GetUnifiedMinimumScale(), GetUnifiedMaximumScale());
    candidate.scaleY = std::clamp(candidate.scaleY, GetUnifiedMinimumScale(), GetUnifiedMaximumScale());
    // Candidate residual tolerance must not become crop tolerance. Only absorb
    // sub-pixel refinement noise at a frame edge; even a one-pixel crop remains
    // a decisive CanvasClipped failure.
    const double boundaryTolerance = std::min(locatorPolicy.maximumGeometryResidualPixels,
        kLocalDesktopGeometryRefinementConvergencePixels);
    const auto SnapAxisToFrame = [boundaryTolerance](const double framePixels, const double canvasPixels,
        double& origin, double& scale) noexcept
    {
        const double farBoundary = origin + scale * canvasPixels;
        if (origin < -boundaryTolerance || farBoundary > framePixels + boundaryTolerance ||
            !std::isfinite(farBoundary))
        {
            return false;
        }
        const double boundedOrigin = std::max(0.0, origin);
        const double boundedFarBoundary = std::min(framePixels, farBoundary);
        if (!(boundedFarBoundary > boundedOrigin))
        {
            return false;
        }
        origin = boundedOrigin;
        scale = (boundedFarBoundary - boundedOrigin) / canvasPixels;
        return std::isfinite(scale) && scale > 0;
    };
    if (!SnapAxisToFrame(frameWidth, kUnifiedVisualProfile.canvasWidth, candidate.originX, candidate.scaleX) ||
        !SnapAxisToFrame(frameHeight, kUnifiedVisualProfile.canvasHeight, candidate.originY, candidate.scaleY) ||
        candidate.scaleX < GetUnifiedMinimumScale() || candidate.scaleX > GetUnifiedMaximumScale() ||
        candidate.scaleY < GetUnifiedMinimumScale() || candidate.scaleY > GetUnifiedMaximumScale())
    {
        return false;
    }
    output = candidate;
    return true;
}

[[nodiscard]] UnifiedCalibration Calibrate(const LumaView& view, const LocalDesktopGeometry& geometry,
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
    bool samplesValid = true;
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
                const std::uint32_t stripeWidth = region.width / 4;
                const std::uint32_t stripeColumn = column % stripeWidth;
                Sample sample;
                if (!ReadSample(view, geometry, region.x + column, region.y + row, sample))
                {
                    samplesValid = false;
                    continue;
                }
                if (row < kUnifiedCalibrationLumaRows)
                {
                    if (row < 4 || row + 4 >= kUnifiedCalibrationLumaRows || stripeColumn < 4 ||
                        stripeColumn + 4 >= stripeWidth)
                    {
                        continue;
                    }
                    const std::size_t label = column / (region.width / 4);
                    const double luma = Luma(sample);
                    lumaStatistics[label].Add(luma);
                    localLuma[label].Add(luma);
                }
                else if (row < kUnifiedCalibrationLumaRows + kUnifiedCalibrationNeutralRows)
                {
                    const std::uint32_t neutralRow = row - kUnifiedCalibrationLumaRows;
                    if (neutralRow == 0 || neutralRow + 1 >= kUnifiedCalibrationNeutralRows ||
                        column < 4 || column + 4 >= region.width)
                    {
                        continue;
                    }
                    const std::array<double, 2> opponent = Opponent(sample);
                    neutralBlueStatistics.Add(opponent[0]);
                    neutralRedStatistics.Add(opponent[1]);
                }
                else
                {
                    const std::uint32_t chromaRow = row - kUnifiedCalibrationLumaRows -
                        kUnifiedCalibrationNeutralRows;
                    if (chromaRow < 4 || chromaRow + 4 >= kUnifiedCalibrationChromaRows ||
                        stripeColumn < 4 || stripeColumn + 4 >= stripeWidth)
                    {
                        continue;
                    }
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
    calibration.lumaValid = pilotIndex == 4 && samplesValid;
    calibration.chromaValid = pilotIndex == 4 && samplesValid && !clipped;
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

[[nodiscard]] bool CheckPhasePilot(const LumaView& view, const LocalDesktopGeometry& geometry,
    const UnifiedPixelRegion& region, const bool finePilot, const std::uint64_t frameSequence,
    const UnifiedCalibration& calibration, const UnifiedVisualDecodePolicy& policy) noexcept
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
    std::array<double, 8> phaseDistances{};
    std::uint64_t tiles = 0;
    for (std::uint32_t tileRow = 0; tileRow < tileRows; tileRow++)
    {
        for (std::uint32_t tileColumn = 0; tileColumn < tilesPerRow; tileColumn++)
        {
            const std::uint32_t tileOrdinal = tileRow * tilesPerRow + tileColumn;
            std::array<double, 16> samples{};
            for (std::uint32_t row = 0; row < kUnifiedVisualProfile.tileHeight; row++)
            {
                for (std::uint32_t column = 0; column < kUnifiedVisualProfile.tileWidth; column++)
                {
                    const std::uint32_t chip = row * kUnifiedVisualProfile.tileWidth + column;
                    Sample sample;
                    if (!ReadSample(view, geometry,
                        region.x + tileColumn * kUnifiedVisualProfile.tileWidth + column,
                        region.y + tileRow * kUnifiedVisualProfile.tileHeight + row, sample))
                    {
                        return false;
                    }
                    samples[chip] = Luma(sample);
                }
            }
            std::array<double, 16> distances{};
            for (std::size_t label = 0; label < distances.size(); label++)
            {
                const std::uint16_t mask = kUnifiedSymbolMasksByLabel[label];
                for (std::size_t chip = 0; chip < samples.size(); chip++)
                {
                    const double expected = ((mask >> chip) & 1U) != 0 ? high : low;
                    const double difference = samples[chip] - expected;
                    distances[label] += difference * difference;
                }
            }
            const std::uint64_t phaseBase = frameSequence - frameSequence % phaseDistances.size();
            for (std::size_t phase = 0; phase < phaseDistances.size(); phase++)
            {
                const std::uint8_t label = GetUnifiedPhasePilotLabel(
                    finePilot, tileOrdinal, phaseBase + phase);
                phaseDistances[phase] += distances[label];
            }
            tiles++;
        }
    }
    if (tiles == 0)
    {
        return false;
    }
    const std::size_t expectedPhase = static_cast<std::size_t>(frameSequence % phaseDistances.size());
    double nearestOther = std::numeric_limits<double>::infinity();
    for (std::size_t phase = 0; phase < phaseDistances.size(); phase++)
    {
        if (phase != expectedPhase)
        {
            nearestOther = std::min(nearestOther, phaseDistances[phase]);
        }
    }
    const double expectedDistance = phaseDistances[expectedPhase];
    // The nearest alternative is the phase-identity gate. Residual quality is
    // normalized independently as mean squared error over the measured luma
    // gap, so the same threshold remains meaningful when downsampling changes
    // how many chip edges are blended without granting a wrong phase a pass.
    const double squaredGap = gap * gap;
    const double normalizedResidual = expectedDistance /
        (static_cast<double>(tiles) * 16 * squaredGap);
    return expectedDistance < nearestOther && normalizedResidual <= policy.maximumPhasePilotResidual;
}

void EvaluateFreshness(const LumaView& view, const LocalDesktopGeometry& geometry,
    const LocalDesktopObservation& bootstrap, const std::span<const std::byte> canonicalRecord,
    const UnifiedVisualDecodePolicy& policy,
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
        bool samplesValid = true;
        for (std::size_t bitIndex = 0; bitIndex < expected.size(); bitIndex++)
        {
            const std::uint32_t column = static_cast<std::uint32_t>(bitIndex % 16);
            const std::uint32_t row = static_cast<std::uint32_t>(bitIndex / 16);
            double sum = 0;
            for (std::uint32_t sampleRow = 2; sampleRow < 6; sampleRow++)
            {
                for (std::uint32_t sampleColumn = 2; sampleColumn < 6; sampleColumn++)
                {
                    Sample sample;
                    if (!ReadSample(view, geometry, region.x + column * 8 + sampleColumn,
                        region.y + row * 8 + sampleRow, sample))
                    {
                        samplesValid = false;
                        break;
                    }
                    sum += Luma(sample);
                }
                if (!samplesValid)
                {
                    break;
                }
            }
            if (!samplesValid)
            {
                break;
            }
            const double normalized = (sum / 16 - bootstrap.blackLevel) / contrast;
            residual += std::abs(normalized - expected[bitIndex]);
            errors += static_cast<std::uint16_t>((normalized >= 0.5) != (expected[bitIndex] != 0));
        }
        if (!samplesValid)
        {
            continue;
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

void DecodeDataTiles(const LumaView& view, const LocalDesktopGeometry& geometry,
    const std::uint64_t frameSequence, const UnifiedCalibration& calibration, const UnifiedVisualDecodePolicy& policy,
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
        bool samplingFailed = false;
        for (std::uint32_t row = 0; row < kUnifiedVisualProfile.tileHeight; row++)
        {
            for (std::uint32_t column = 0; column < kUnifiedVisualProfile.tileWidth; column++)
            {
                Sample& sample = samples[row * kUnifiedVisualProfile.tileWidth + column];
                if (!ReadSample(view, geometry, tile.bounds.x + column, tile.bounds.y + row, sample))
                {
                    samplingFailed = true;
                    continue;
                }
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
            StoreMetric(logical, tile, metric, clipped || samplingFailed, freshness, policy, base, fine, chroma, metrics);
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
            StoreMetric(logical, tile, metric, clipped || samplingFailed, freshness, policy, base, fine, chroma, metrics);
        }
    }
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

[[nodiscard]] bool IsControlInformationBlock(
    const std::span<const std::byte> information) noexcept
{
    return information.size() >= pbprotocol::kControlRecordMagic.size() &&
        std::equal(pbprotocol::kControlRecordMagic.begin(), pbprotocol::kControlRecordMagic.end(),
            information.begin());
}

[[nodiscard]] UnifiedControlPriority GetControlPriorityFromInformation(
    const std::span<const std::byte> information) noexcept
{
    constexpr std::size_t recordTypeOffset = 5;
    if (information.size() <= recordTypeOffset)
    {
        return UnifiedControlPriority::SessionDescriptor;
    }
    const auto recordType = static_cast<pbprotocol::ControlRecordType>(
        std::to_integer<std::uint8_t>(information[recordTypeOffset]));
    if (recordType == pbprotocol::ControlRecordType::FinalManifest)
    {
        return UnifiedControlPriority::FinalManifest;
    }
    if (recordType == pbprotocol::ControlRecordType::SegmentDescriptor)
    {
        return UnifiedControlPriority::CurrentSegmentDescriptor;
    }
    return UnifiedControlPriority::SessionDescriptor;
}

[[nodiscard]] ModulationErrorCode MapProtocolPackingError(
    const pbprotocol::ProtocolErrorCode code) noexcept
{
    if (code == pbprotocol::ProtocolErrorCode::CrcMismatch)
    {
        return ModulationErrorCode::CrcMismatch;
    }
    if (code == pbprotocol::ProtocolErrorCode::OutputBufferTooSmall)
    {
        return ModulationErrorCode::OutputBufferTooSmall;
    }
    if (code == pbprotocol::ProtocolErrorCode::InternalInvariantViolation)
    {
        return ModulationErrorCode::InternalInvariantViolation;
    }
    return ModulationErrorCode::InvalidInput;
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

    const auto extracted = pbprotocol::ExtractControlRecordFromInfoBlock(information);
    if (!extracted)
    {
        slotObservation.rejection = extracted.Error().code == pbprotocol::ProtocolErrorCode::NonCanonicalPadding ?
            UnifiedSlotRejection::NonCanonicalPadding :
            extracted.Error().code == pbprotocol::ProtocolErrorCode::CrcMismatch ?
                UnifiedSlotRejection::ControlCrcFailure : UnifiedSlotRejection::InvalidInformation;
        return;
    }
    slotObservation.paddingValid = true;
    const std::span<const std::byte> record = extracted.Value();
    const auto parsed = pbprotocol::ParseControlRecord(record);
    if (!parsed)
    {
        slotObservation.rejection = UnifiedSlotRejection::InvalidInformation;
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

bool ValidateUnifiedVisualDecodePolicy(const UnifiedVisualDecodePolicy& policy) noexcept
{
    const std::array<double, 5> values{policy.minimumLumaLevelGap, policy.maximumPilotDeviation,
        policy.minimumChromaSeparation, policy.maximumPhasePilotResidual,
        policy.maximumTimingBitErrorFraction};
    return ValidateLocalDesktopDecodePolicy(policy.locator) &&
        std::ranges::all_of(values, [](const double value)
    {
        return std::isfinite(value) && value >= 0;
    }) && policy.minimumLumaLevelGap >= 16 && policy.minimumLumaLevelGap <= 96 &&
        policy.maximumPilotDeviation <= 16 && policy.minimumChromaSeparation >= 8 &&
        policy.minimumChromaSeparation <= 128 && policy.maximumPhasePilotResidual <= 0.5 &&
        policy.maximumTimingBitErrorFraction <= 0.25 && policy.minimumDecisionMetric >= 0 &&
        policy.maximumFecIterations >= pbinnerfec::kQcLdpcMinIterations &&
        policy.maximumFecIterations <= pbinnerfec::kQcLdpcMaxIterations;
}

bool ResolveUnifiedVisualSamplingGeometry(const LocalDesktopGeometry& geometry,
    const std::uint32_t frameWidth, const std::uint32_t frameHeight,
    const UnifiedVisualDecodePolicy& policy, LocalDesktopGeometry& output) noexcept
{
    if (!ValidateUnifiedVisualDecodePolicy(policy))
    {
        return false;
    }
    return ResolveUnifiedSamplingGeometryInternal(
        geometry, frameWidth, frameHeight, GetUnifiedLocatorPolicy(policy), output);
}

bool BuildUnifiedFreshnessBits(const std::span<const std::byte> canonicalRecord,
    const std::uint32_t freshnessRegion, const std::span<std::uint8_t> output) noexcept
{
    return freshnessRegion < kUnifiedFreshnessRegionCount && output.size() == kLocalDesktopTimingBits &&
        detail::BuildTimingBits(canonicalRecord, freshnessRegion, output);
}

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

ModulationStatus PackUnifiedVisualFrame(
    const UnifiedVisualFrameInput& input, const std::span<std::byte> outCodedFrame) noexcept
{
    if (input.bootstrapRecord.data() == nullptr || input.bootstrapRecord.size() != pbprotocol::kBootstrapRecordBytes ||
        input.slots.data() == nullptr || input.slots.size() != kUnifiedCodewordCount ||
        outCodedFrame.data() == nullptr)
    {
        return ModulationStatus::Failure(ModulationErrorCode::InvalidInput, 0);
    }
    if (outCodedFrame.size() != kUnifiedCodedFrameBytes)
    {
        return ModulationStatus::Failure(outCodedFrame.size() < kUnifiedCodedFrameBytes ?
            ModulationErrorCode::OutputBufferTooSmall : ModulationErrorCode::InvalidInput, 0);
    }
    const auto bootstrap = pbprotocol::ParseBootstrapRecord(input.bootstrapRecord);
    if (!bootstrap)
    {
        return ModulationStatus::Failure(MapProtocolPackingError(bootstrap.Error().code), bootstrap.Error().offset);
    }
    if (bootstrap.Value().visualProfileId != kUnifiedVisualProfile.productProfile.visualProfileId ||
        bootstrap.Value().visualLayoutVersion != kUnifiedVisualProfile.productProfile.visualLayoutVersion)
    {
        return ModulationStatus::Failure(ModulationErrorCode::InvalidInput, 8);
    }

    std::array<UnifiedSlotAssignment, kUnifiedCodewordCount> assignments{};
    for (std::size_t inputIndex = 0; inputIndex < input.slots.size(); inputIndex++)
    {
        assignments[inputIndex] = input.slots[inputIndex].assignment;
    }
    if (!ValidateUnifiedMixedSlotPlan(assignments))
    {
        return ModulationStatus::Failure(ModulationErrorCode::InvalidInput, 0);
    }

    for (const UnifiedFrameSlotInput& slotInput : input.slots)
    {
        const UnifiedSlotAssignment& assignment = slotInput.assignment;
        if (!slotInput.active)
        {
            if (assignment.kind != UnifiedSlotKind::Transport || !slotInput.block.empty())
            {
                return ModulationStatus::Failure(ModulationErrorCode::InvalidInput, assignment.codewordSlot);
            }
            continue;
        }
        if (slotInput.block.data() == nullptr || slotInput.block.empty() ||
            slotInput.block.size() > kUnifiedInformationBytes)
        {
            return ModulationStatus::Failure(ModulationErrorCode::InvalidInput, assignment.codewordSlot);
        }
        if (assignment.kind == UnifiedSlotKind::Transport)
        {
            const auto transport = pbprotocol::ParseTransportBlock(slotInput.block);
            if (!transport)
            {
                return ModulationStatus::Failure(
                    MapProtocolPackingError(transport.Error().code), assignment.codewordSlot);
            }
            if (transport.Value().header.sessionTag != bootstrap.Value().sessionTag)
            {
                return ModulationStatus::Failure(ModulationErrorCode::InvalidInput, assignment.codewordSlot);
            }
            continue;
        }
        const auto control = pbprotocol::ParseControlRecord(slotInput.block);
        if (!control)
        {
            return ModulationStatus::Failure(MapProtocolPackingError(control.Error().code), assignment.codewordSlot);
        }
        if (!ControlTypeMatchesPriority(control.Value().recordType, assignment.controlPriority) ||
            control.Value().sessionTag != bootstrap.Value().sessionTag)
        {
            return ModulationStatus::Failure(ModulationErrorCode::InvalidInput, assignment.codewordSlot);
        }
    }

    std::array<std::byte, kUnifiedCodedFrameBytes> codedFrame{};
    std::array<std::byte, kUnifiedInformationBytes> information{};
    for (const UnifiedFrameSlotInput& slotInput : input.slots)
    {
        const UnifiedSlotAssignment& assignment = slotInput.assignment;
        if (!slotInput.active)
        {
            std::fill(information.begin(), information.end(), std::byte{0});
        }
        else
        {
            const pbprotocol::ProtocolStatus packingStatus = assignment.kind == UnifiedSlotKind::Control ?
                pbprotocol::FrameControlRecordIntoInfoBlock(slotInput.block, information.size(), information) :
                pbprotocol::FrameTransportBlockIntoInfoBlock(slotInput.block, information.size(), information);
            if (!packingStatus)
            {
                return ModulationStatus::Failure(
                    MapProtocolPackingError(packingStatus.Error().code), assignment.codewordSlot);
            }
        }
        const std::span<std::byte> codeword = std::span(codedFrame).subspan(
            static_cast<std::size_t>(assignment.codewordSlot) * kUnifiedCodewordBytes, kUnifiedCodewordBytes);
        const pbinnerfec::InnerFecStatus fecStatus = pbinnerfec::EncodeQcLdpcCodeword(
            pbinnerfec::kInnerFecProfileIdRobust, information, codeword);
        if (!fecStatus)
        {
            return ModulationStatus::Failure(ModulationErrorCode::InternalInvariantViolation,
                assignment.codewordSlot);
        }
    }
    std::copy(codedFrame.begin(), codedFrame.end(), outCodedFrame.begin());
    return ModulationStatus::Success();
}

ModulationStatus EncodeUnifiedVisualFrame(
    const UnifiedVisualFrameInput& input, const std::span<std::byte> outBgra) noexcept
{
    std::array<std::byte, kUnifiedCodedFrameBytes> codedFrame{};
    const ModulationStatus packingStatus = PackUnifiedVisualFrame(input, codedFrame);
    if (!packingStatus)
    {
        return packingStatus;
    }
    return EncodeUnifiedVisualFrame(input.bootstrapRecord, codedFrame, outBgra);
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

UnifiedVisualObservation UnifiedVisualCpuOracle::DecodeMixedFrame(const LumaView& view,
    const UnifiedExpectedFrameIdentity& expectedIdentity,
    const UnifiedVisualDecodePolicy& policy) noexcept
{
    return DecodeInternal(view, {}, true, expectedIdentity, policy);
}

UnifiedVisualObservation UnifiedVisualCpuOracle::DecodePreparedMixedFrame(const UnifiedPreparedMetricFrame& input,
    const UnifiedExpectedFrameIdentity& expectedIdentity, const UnifiedVisualDecodePolicy& policy) noexcept
{
    UnifiedVisualObservation observation;
    if (!implementation_)
    {
        return observation;
    }
    Implementation& state = *implementation_;
    state.acceptedCount = 0;
    state.decodedInformationValid.fill(false);
    state.metricsValid = false;
    InitializeMetrics(0, UnifiedErasureReason::CanvasClipped, state.metrics);
    state.metricsValid = true;
    const bool metricsValid = input.logicalMetrics.size() == kUnifiedSoftMetricCount &&
        std::ranges::all_of(input.logicalMetrics, [](const float value) { return std::isfinite(value); });
    const bool samplingValid = input.tileSamplingFailures.size() == kUnifiedVisualProfile.dataTileCount &&
        std::ranges::all_of(input.tileSamplingFailures, [](const std::uint8_t value) { return value <= 1; });
    const bool lanesValid = ValidateUnifiedLaneObservation(input.baseLuma) &&
        ValidateUnifiedLaneObservation(input.fineLuma) && ValidateUnifiedLaneObservation(input.chroma);
    bool freshnessValid = true;
    for (const UnifiedFreshnessObservation& freshness : input.freshness)
    {
        const bool thresholdsPass = static_cast<double>(freshness.bitErrors) / kLocalDesktopTimingBits <=
            policy.maximumTimingBitErrorFraction && freshness.residual <= policy.locator.maximumTimingResidual;
        freshnessValid = freshnessValid && freshness.bitErrors <= kLocalDesktopTimingBits &&
            std::isfinite(freshness.residual) && freshness.residual >= 0 && freshness.current == thresholdsPass;
    }
    if (!ValidateUnifiedVisualDecodePolicy(policy) || !input.bootstrap.IsAccepted() || !metricsValid ||
        !samplingValid || !lanesValid || !freshnessValid)
    {
        return observation;
    }
    const auto parsedBootstrap = pbprotocol::ParseBootstrapRecord(input.bootstrap.canonical44);
    if (!parsedBootstrap ||
        parsedBootstrap.Value().visualProfileId != kUnifiedVisualProfile.productProfile.visualProfileId ||
        parsedBootstrap.Value().visualLayoutVersion != kUnifiedVisualProfile.productProfile.visualLayoutVersion)
    {
        return observation;
    }
    observation.inputValid = true;
    observation.bootstrap = input.bootstrap;
    observation.bootstrapRecord = parsedBootstrap.Value();
    if ((expectedIdentity.requireSessionTag && expectedIdentity.sessionTag != observation.bootstrapRecord.sessionTag) ||
        (expectedIdentity.requireFrameSequence && expectedIdentity.frameSequence != observation.bootstrapRecord.frameSequence))
    {
        observation.frameErasure = UnifiedErasureReason::IdentityConflict;
        InitializeMetrics(observation.bootstrapRecord.frameSequence, observation.frameErasure, state.metrics);
        return observation;
    }
    observation.frameErasure = UnifiedErasureReason::None;
    observation.freshness = input.freshness;
    observation.baseLuma = input.baseLuma;
    observation.fineLuma = input.fineLuma;
    observation.chroma = input.chroma;
    InitializeMetrics(observation.bootstrapRecord.frameSequence, UnifiedErasureReason::LocalSamplingFailure,
        state.metrics);
    for (std::size_t globalBit = 0; globalBit < state.metrics.size(); globalBit++)
    {
        UnifiedSoftMetric& metric = state.metrics[globalBit];
        const UnifiedLaneCapacity capacity = GetUnifiedLaneCapacity(metric.lane);
        const std::uint32_t logicalBit = static_cast<std::uint32_t>(globalBit -
            static_cast<std::size_t>(capacity.firstCodewordSlot) * kUnifiedVisualProfile.innerCodewordBits);
        const UnifiedPhysicalCarrierSite site = GetUnifiedPhysicalCarrierSite(
            metric.lane, logicalBit, observation.bootstrapRecord.frameSequence);
        const std::int16_t value = QuantizeMetric(input.logicalMetrics[globalBit]);
        if (!site.valid || !LaneAvailable(metric.lane, observation.baseLuma, observation.fineLuma, observation.chroma))
        {
            metric.erasureReason = site.valid ? LaneReason(
                metric.lane, observation.baseLuma, observation.fineLuma, observation.chroma) :
                UnifiedErasureReason::LocalSamplingFailure;
        }
        else if (!observation.freshness[metric.freshnessRegion].current)
        {
            metric.erasureReason = UnifiedErasureReason::LocalStaleRegion;
        }
        else if (input.tileSamplingFailures[site.tileOrdinal] != 0)
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
    return FinalizeDecodedMetrics(std::move(observation), {}, true, policy);
}

UnifiedVisualObservation UnifiedVisualCpuOracle::Decode(const LumaView& view,
    const std::span<const UnifiedSlotAssignment> slotPlan,
    const UnifiedExpectedFrameIdentity& expectedIdentity,
    const UnifiedVisualDecodePolicy& policy) noexcept
{
    return DecodeInternal(view, slotPlan, false, expectedIdentity, policy);
}

UnifiedVisualObservation UnifiedVisualCpuOracle::DecodeInternal(const LumaView& view,
    const std::span<const UnifiedSlotAssignment> slotPlan, const bool inferSlotKinds,
    const UnifiedExpectedFrameIdentity& expectedIdentity,
    const UnifiedVisualDecodePolicy& policy) noexcept
{
    UnifiedVisualObservation observation;
    if (!implementation_)
    {
        return observation;
    }
    Implementation& state = *implementation_;
    state.acceptedCount = 0;
    state.decodedInformationValid.fill(false);
    state.metricsValid = false;
    InitializeMetrics(0, UnifiedErasureReason::CanvasClipped, state.metrics);
    state.metricsValid = true;
    if (!ValidateUnifiedVisualDecodePolicy(policy) || (!inferSlotKinds && !ValidateUnifiedMixedSlotPlan(slotPlan)) ||
        ValidateLumaView(view) != LocalDesktopErasureReason::None || view.pixelFormat != LumaPixelFormat::Bgra8)
    {
        return observation;
    }
    observation.inputValid = true;
    std::array<UnifiedSlotAssignment, kUnifiedCodewordCount> assignmentsBySlot{};
    for (std::uint32_t slot = 0; slot < kUnifiedCodewordCount; slot++)
    {
        assignmentsBySlot[slot] = {slot, UnifiedSlotKind::Transport, UnifiedControlPriority::NotApplicable};
        UnifiedSlotObservation& slotObservation = observation.slots[slot];
        const UnifiedLaneContract* const lane = FindUnifiedLaneForCodewordSlot(slot);
        slotObservation.lane = lane == nullptr ? UnifiedLane::BaseLuma : lane->lane;
    }
    if (!inferSlotKinds)
    {
        for (const UnifiedSlotAssignment& assignment : slotPlan)
        {
            assignmentsBySlot[assignment.codewordSlot] = assignment;
            observation.slots[assignment.codewordSlot].kind = assignment.kind;
        }
    }

    const LocalDesktopBootstrapBinding binding{kUnifiedVisualProfile.productProfile.visualProfileId,
        kUnifiedVisualProfile.productProfile.visualLayoutVersion};
    const LocalDesktopDecodePolicy locatorPolicy = GetUnifiedLocatorPolicy(policy);
    if (view.width == kUnifiedVisualProfile.canvasWidth && view.height == kUnifiedVisualProfile.canvasHeight)
    {
        observation.bootstrap = detail::DecodeLocalDesktopFixedCanvasScaffold(
            view, locatorPolicy, detail::LocalDesktopBinding::UnifiedVisual, binding);
    }
    if (!observation.bootstrap.IsAccepted())
    {
        observation.bootstrap = detail::DecodeLocalDesktopScaffold(
            view, locatorPolicy, detail::LocalDesktopBinding::UnifiedVisual);
    }
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
    LocalDesktopGeometry samplingGeometry;
    if (!ResolveUnifiedSamplingGeometryInternal(observation.bootstrap.geometry, view.width, view.height,
        locatorPolicy, samplingGeometry))
    {
        observation.frameErasure = UnifiedErasureReason::CanvasClipped;
        InitializeMetrics(observation.bootstrapRecord.frameSequence, observation.frameErasure, state.metrics);
        return observation;
    }
    if ((expectedIdentity.requireSessionTag && expectedIdentity.sessionTag != observation.bootstrapRecord.sessionTag) ||
        (expectedIdentity.requireFrameSequence && expectedIdentity.frameSequence != observation.bootstrapRecord.frameSequence))
    {
        observation.frameErasure = UnifiedErasureReason::IdentityConflict;
        InitializeMetrics(observation.bootstrapRecord.frameSequence, observation.frameErasure, state.metrics);
        return observation;
    }
    observation.frameErasure = UnifiedErasureReason::None;
    EvaluateFreshness(view, samplingGeometry, observation.bootstrap, observation.bootstrap.canonical44,
        policy, observation.freshness);
    const UnifiedCalibration calibration = Calibrate(view, samplingGeometry, policy);
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
        const bool phaseValid = calibration.lumaValid && CheckPhasePilot(view, samplingGeometry,
            contract.bounds, finePilot, observation.bootstrapRecord.frameSequence, calibration, policy);
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
    DecodeDataTiles(view, samplingGeometry, observation.bootstrapRecord.frameSequence, calibration, policy,
        observation.freshness, observation.baseLuma, observation.fineLuma, observation.chroma, state.metrics);

    return FinalizeDecodedMetrics(std::move(observation), slotPlan, inferSlotKinds, policy);
}

UnifiedVisualObservation UnifiedVisualCpuOracle::FinalizeDecodedMetrics(UnifiedVisualObservation observation,
    const std::span<const UnifiedSlotAssignment> slotPlan, const bool inferSlotKinds,
    const UnifiedVisualDecodePolicy& policy) noexcept
{
    Implementation& state = *implementation_;
    for (const UnifiedSoftMetric& metric : state.metrics)
    {
        const std::size_t laneIndex = metric.lane == UnifiedLane::BaseLuma ? 0 : metric.lane == UnifiedLane::FineLuma ? 1 : 2;
        auto& summary = observation.laneMetrics[laneIndex];
        const auto magnitude = static_cast<std::uint32_t>(std::abs(static_cast<std::int32_t>(metric.value)));
        summary.minimumAbsoluteMetric = summary.samples == 0 ? magnitude : std::min(summary.minimumAbsoluteMetric, magnitude);
        summary.available = true;
        summary.samples++;
        summary.zeroMetrics += static_cast<std::uint32_t>(magnitude == 0);
        summary.erasedMetrics += static_cast<std::uint32_t>(metric.erasureReason != UnifiedErasureReason::None);
        summary.absoluteMetricSum += magnitude;
    }
    state.acceptedCount = 0;
    state.decodedInformationValid.fill(false);
    std::array<UnifiedSlotAssignment, kUnifiedCodewordCount> assignmentsBySlot{};
    for (std::uint32_t slot = 0; slot < kUnifiedCodewordCount; slot++)
    {
        assignmentsBySlot[slot] = {slot, UnifiedSlotKind::Transport, UnifiedControlPriority::NotApplicable};
        UnifiedSlotObservation& slotObservation = observation.slots[slot];
        const UnifiedLaneContract* const lane = FindUnifiedLaneForCodewordSlot(slot);
        slotObservation.lane = lane == nullptr ? UnifiedLane::BaseLuma : lane->lane;
    }
    if (!inferSlotKinds)
    {
        for (const UnifiedSlotAssignment& assignment : slotPlan)
        {
            assignmentsBySlot[assignment.codewordSlot] = assignment;
            observation.slots[assignment.codewordSlot].kind = assignment.kind;
        }
    }

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
        std::copy_n(state.decodedCodeword.begin(), kUnifiedInformationBytes,
            state.decodedInformation[slot].begin());
        state.decodedInformationValid[slot] = true;
    }

    bool inferredPlanValid = true;
    if (inferSlotKinds)
    {
        for (std::uint32_t slot = 0; slot < kUnifiedCodewordCount; slot++)
        {
            if (!state.decodedInformationValid[slot])
            {
                continue;
            }
            const std::span<const std::byte> information = state.decodedInformation[slot];
            if (IsControlInformationBlock(information))
            {
                assignmentsBySlot[slot] = {slot, UnifiedSlotKind::Control,
                    GetControlPriorityFromInformation(information)};
                observation.slots[slot].kind = UnifiedSlotKind::Control;
            }
        }
        inferredPlanValid = ValidateUnifiedMixedSlotPlan(assignmentsBySlot);
    }

    for (std::uint32_t slot = 0; slot < kUnifiedCodewordCount; slot++)
    {
        if (!state.decodedInformationValid[slot])
        {
            continue;
        }
        UnifiedSlotObservation& slotObservation = observation.slots[slot];
        if (inferSlotKinds && !inferredPlanValid && assignmentsBySlot[slot].kind == UnifiedSlotKind::Control)
        {
            slotObservation.rejection = UnifiedSlotRejection::InvalidInformation;
            continue;
        }
        EvaluateAcceptedInformation(slot, assignmentsBySlot[slot], observation.bootstrapRecord.sessionTag,
            state.decodedInformation[slot], slotObservation, state.accepted, state.acceptedCount);
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
