#include "pbmodulation/shape_chroma.h"

#include "local_desktop_internal.h"
#include "raster_internal.h"
#include "shape_chroma_internal.h"
#include "pbinterleave/tile_permutation.h"
#include "pbmodulation/desktop_levels.h"
#include "pbprotocol/bootstrap_control_codec.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace pbmodulation
{
namespace
{

constexpr bool ValidCodebook() noexcept
{
    for (std::size_t index = 0; index < kShapeChromaTemplates.size(); index++)
    {
        if (std::popcount(kShapeChromaTemplates[index]) != 8)
        {
            return false;
        }
        for (std::size_t previous = 0; previous < index; previous++)
        {
            if (kShapeChromaTemplates[index] == kShapeChromaTemplates[previous])
            {
                return false;
            }
        }
    }
    std::uint8_t labels = 0;
    for (const auto state : kShapeChromaStates)
    {
        if (state.label >= 4 || (labels & (1u << state.label)) != 0)
        {
            return false;
        }
        labels = static_cast<std::uint8_t>(labels | (1u << state.label));
        constexpr std::array<std::uint8_t, 3> baseValues{kShapeChromaLowLuma, kShapeChromaHighLuma, 128};
        const auto ValueWithinRange = [state](const std::uint8_t base)
        {
            return static_cast<int>(base) + state.blueOffset > 0 && static_cast<int>(base) + state.blueOffset < 255 &&
                static_cast<int>(base) + state.greenOffset > 0 && static_cast<int>(base) + state.greenOffset < 255 &&
                static_cast<int>(base) + state.redOffset > 0 && static_cast<int>(base) + state.redOffset < 255;
        };
        if (!std::ranges::all_of(baseValues, ValueWithinRange))
        {
            return false;
        }
    }
    return labels == 0x0F;
}

static_assert(ValidCodebook());
static_assert(kShapeChromaTileCount * kShapeChromaBitsPerTile == kShapeChromaMaximumBits);
static_assert(kShapeChromaCodewords * 2025 + kShapeChromaPaddingBytes == kShapeChromaDataBytes);

bool Overlap(const std::span<const std::byte> first, const std::span<const std::byte> second) noexcept
{
    if (first.empty() || second.empty())
    {
        return false;
    }
    const auto firstAddress = reinterpret_cast<std::uintptr_t>(first.data());
    const auto secondAddress = reinterpret_cast<std::uintptr_t>(second.data());
    if (first.size() > std::numeric_limits<std::uintptr_t>::max() - firstAddress ||
        second.size() > std::numeric_limits<std::uintptr_t>::max() - secondAddress)
    {
        return true;
    }
    return firstAddress <= secondAddress ? secondAddress - firstAddress < first.size() : firstAddress - secondAddress < second.size();
}

bool ValidPolicy(const ShapeChromaDecodePolicy& policy) noexcept
{
    const std::array<double, 9> values{policy.maximumScaleDriftPixels, policy.maximumPhaseErrorPixels,
        policy.maximumMarkerResidualPixels, policy.minimumShapeAmplitude, policy.maximumShapeResidual,
        policy.minimumChromaSeparation, policy.maximumChromaPilotStandardDeviation,
        policy.maximumChromaPilotSpatialDeviation, policy.maximumChromaResidual};
    const auto ValidNumber = [](const double value)
    {
        return std::isfinite(value) && value >= 0;
    };
    return std::ranges::all_of(values, ValidNumber) && policy.maximumScaleDriftPixels <= 0.125 &&
        policy.maximumPhaseErrorPixels <= 0.125 && policy.maximumMarkerResidualPixels <= 0.125 &&
        policy.minimumShapeAmplitude >= 24 && policy.minimumShapeAmplitude <= 96 && policy.maximumShapeResidual <= 0.40 &&
        policy.minimumChromaSeparation >= 16 && policy.minimumChromaSeparation <= 128 &&
        policy.maximumChromaPilotStandardDeviation <= 8 && policy.maximumChromaPilotSpatialDeviation <= 8 &&
        policy.maximumChromaResidual <= 1.5 && policy.maximumDataWorkUnits > 0 && policy.maximumDataWorkUnits <= 8000000;
}

std::uint8_t CodeValue(const std::uint8_t base, const std::int16_t offset) noexcept
{
    return static_cast<std::uint8_t>(static_cast<int>(base) + offset);
}

std::array<double, 2> Opponent(const std::array<double, 3>& blueGreenRed) noexcept
{
    const double luma = 0.0722 * blueGreenRed[0] + 0.7152 * blueGreenRed[1] + 0.2126 * blueGreenRed[2];
    return {blueGreenRed[0] - luma, blueGreenRed[2] - luma};
}

double SquaredDistance(const std::array<double, 2>& left, const std::array<double, 2>& right) noexcept
{
    const double blue = left[0] - right[0];
    const double red = left[1] - right[1];
    return blue * blue + red * red;
}

double DecisionMargin(std::array<double, 16> distances) noexcept
{
    std::sort(distances.begin(), distances.end());
    const double denominator = distances[0] + distances[1];
    return denominator == 0 ? 0 : std::clamp((distances[1] - distances[0]) / denominator, 0.0, 1.0);
}

double DecisionMargin(std::array<double, 4> distances) noexcept
{
    std::sort(distances.begin(), distances.end());
    const double denominator = distances[0] + distances[1];
    return denominator == 0 ? 0 : std::clamp((distances[1] - distances[0]) / denominator, 0.0, 1.0);
}

class BgraReader
{
public:
    BgraReader(const LumaView& view, const std::uint64_t maximumWork) noexcept : view_(view), maximumWork_(maximumWork)
    {
    }

    [[nodiscard]] bool Pixel(const std::uint32_t x, const std::uint32_t y, std::array<double, 3>& output, bool& clipped) noexcept
    {
        if (x >= view_.width || y >= view_.height)
        {
            error_ = LocalDesktopErasureReason::SampleOutOfBounds;
            return false;
        }
        if (workUnits_ == maximumWork_)
        {
            error_ = LocalDesktopErasureReason::WorkBudgetExceeded;
            return false;
        }
        workUnits_++;
        const auto* const pixel = view_.pixels.data() + static_cast<std::size_t>(y) * view_.rowPitch + static_cast<std::size_t>(x) * 4;
        output = {static_cast<double>(std::to_integer<std::uint8_t>(pixel[0])),
            static_cast<double>(std::to_integer<std::uint8_t>(pixel[1])), static_cast<double>(std::to_integer<std::uint8_t>(pixel[2]))};
        clipped = pixel[0] == std::byte{0} || pixel[0] == std::byte{255} || pixel[1] == std::byte{0} ||
            pixel[1] == std::byte{255} || pixel[2] == std::byte{0} || pixel[2] == std::byte{255};
        return true;
    }

    [[nodiscard]] std::uint64_t WorkUnits() const noexcept
    {
        return workUnits_;
    }

    [[nodiscard]] LocalDesktopErasureReason Error() const noexcept
    {
        return error_;
    }

private:
    const LumaView& view_;
    const std::uint64_t maximumWork_;
    std::uint64_t workUnits_ = 0;
    LocalDesktopErasureReason error_ = LocalDesktopErasureReason::None;
};

ShapeChromaErasure CalibrateChroma(BgraReader& reader, const std::uint32_t originX, const std::uint32_t originY,
                                   const ShapeChromaDecodePolicy& policy, ShapeChromaCalibration& calibration) noexcept
{
    for (std::size_t pilot = 0; pilot < kDesktopLevelsLadders.size(); pilot++)
    {
        const auto region = kDesktopLevelsLadders[pilot];
        for (std::size_t state = 0; state < kShapeChromaStates.size(); state++)
        {
            std::array<double, 2> sum{};
            std::array<double, 2> squares{};
            for (std::uint32_t row = 0; row < region.height; row++)
            {
                for (std::uint32_t column = 0; column < region.width / 4; column++)
                {
                    std::array<double, 3> sample{};
                    bool clipped = false;
                    if (!reader.Pixel(originX + region.x + static_cast<std::uint32_t>(state) * (region.width / 4) + column,
                        originY + region.y + row, sample, clipped))
                    {
                        return ShapeChromaErasure::PixelReadFailure;
                    }
                    if (clipped)
                    {
                        return ShapeChromaErasure::ChromaPilotClipping;
                    }
                    const auto opponent = Opponent(sample);
                    for (std::size_t axis = 0; axis < 2; axis++)
                    {
                        sum[axis] += opponent[axis];
                        squares[axis] += opponent[axis] * opponent[axis];
                    }
                }
            }
            constexpr double samples = 2048;
            double combinedVariance = 0;
            for (std::size_t axis = 0; axis < 2; axis++)
            {
                const double mean = sum[axis] / samples;
                const double variance = std::max(0.0, squares[axis] / samples - mean * mean);
                calibration.spatialCentroids[pilot][state][axis] = mean;
                combinedVariance += variance;
                calibration.centroids[state][axis] += mean / 4;
            }
            calibration.spatialVariances[pilot][state] = combinedVariance;
            if (combinedVariance > policy.maximumChromaPilotStandardDeviation * policy.maximumChromaPilotStandardDeviation)
            {
                return ShapeChromaErasure::ChromaPilotVariance;
            }
        }
    }
    calibration.minimumSeparation = std::numeric_limits<double>::max();
    for (std::size_t state = 0; state < kShapeChromaStates.size(); state++)
    {
        for (std::size_t other = 0; other < state; other++)
        {
            calibration.minimumSeparation = std::min(calibration.minimumSeparation,
                std::sqrt(SquaredDistance(calibration.centroids[state], calibration.centroids[other])));
        }
        for (std::size_t pilot = 0; pilot < kDesktopLevelsLadders.size(); pilot++)
        {
            calibration.spatialDeviation = std::max(calibration.spatialDeviation,
                std::sqrt(SquaredDistance(calibration.spatialCentroids[pilot][state], calibration.centroids[state])));
        }
    }
    if (calibration.minimumSeparation < policy.minimumChromaSeparation)
    {
        return ShapeChromaErasure::ChromaPilotSeparation;
    }
    return calibration.spatialDeviation > policy.maximumChromaPilotSpatialDeviation ?
        ShapeChromaErasure::ChromaPilotSpatialMismatch : ShapeChromaErasure::None;
}

} // namespace

struct ShapeChromaWorkspace::Implementation
{
    std::array<std::byte, kShapeChromaDataBytes> hard{};
    std::array<float, kShapeChromaMaximumBits> soft{};
    std::array<std::uint64_t, kShapeChromaMetricBins> shapeHistogram{};
    std::array<std::uint64_t, kShapeChromaMetricBins> chromaHistogram{};
    bool histogramValid = false;
};

ShapeChromaWorkspace::ShapeChromaWorkspace() noexcept = default;
ShapeChromaWorkspace::ShapeChromaWorkspace(ShapeChromaWorkspace&&) noexcept = default;
ShapeChromaWorkspace& ShapeChromaWorkspace::operator=(ShapeChromaWorkspace&&) noexcept = default;
ShapeChromaWorkspace::~ShapeChromaWorkspace() = default;

std::uint64_t ShapeChromaWorkspace::RequiredBytes() noexcept
{
    return sizeof(Implementation);
}

ModulationResult<ShapeChromaWorkspace> ShapeChromaWorkspace::Create(const std::uint64_t maximumBytes) noexcept
{
    if (maximumBytes < RequiredBytes())
    {
        return ModulationResult<ShapeChromaWorkspace>::Failure(ModulationErrorCode::InvalidInput, 0);
    }
    try
    {
        ShapeChromaWorkspace workspace;
        workspace.implementation_ = std::make_unique<Implementation>();
        return ModulationResult<ShapeChromaWorkspace>::Success(std::move(workspace));
    }
    catch (const std::bad_alloc&)
    {
        return ModulationResult<ShapeChromaWorkspace>::Failure(ModulationErrorCode::MemoryAllocationFailure, 0);
    }
}

std::span<const std::uint64_t> ShapeChromaWorkspace::GetShapeMarginHistogram() const noexcept
{
    return implementation_ && implementation_->histogramValid ? std::span<const std::uint64_t>(implementation_->shapeHistogram) : std::span<const std::uint64_t>{};
}

std::span<const std::uint64_t> ShapeChromaWorkspace::GetChromaMarginHistogram() const noexcept
{
    return implementation_ && implementation_->histogramValid ? std::span<const std::uint64_t>(implementation_->chromaHistogram) : std::span<const std::uint64_t>{};
}

ShapeChromaErasure ValidateShapeChromaGeometry(const LocalDesktopGeometry& geometry, const ShapeChromaDecodePolicy& policy) noexcept
{
    if (!ValidPolicy(policy))
    {
        return ShapeChromaErasure::InvalidPolicy;
    }
    const std::array<double, 5> values{geometry.originX, geometry.originY, geometry.scaleX, geometry.scaleY, geometry.markerResidualPixels};
    if (!std::ranges::all_of(values, [](const double value) { return std::isfinite(value); }) || geometry.markerResidualPixels < 0)
    {
        return ShapeChromaErasure::InvalidInput;
    }
    if (std::abs(geometry.scaleX - 1) * 1920 > policy.maximumScaleDriftPixels ||
        std::abs(geometry.scaleY - 1) * 1080 > policy.maximumScaleDriftPixels)
    {
        return ShapeChromaErasure::ScaleOutOfRange;
    }
    if (std::abs(geometry.originX - std::round(geometry.originX)) > policy.maximumPhaseErrorPixels ||
        std::abs(geometry.originY - std::round(geometry.originY)) > policy.maximumPhaseErrorPixels ||
        geometry.markerResidualPixels > policy.maximumMarkerResidualPixels)
    {
        return ShapeChromaErasure::AlignmentOutOfRange;
    }
    return ShapeChromaErasure::None;
}

detail::ShapeChromaDecision detail::DecideShapeChromaTile(const std::array<std::array<double, 3>, 16>& samples,
    const bool clipped, const ShapeChromaCalibration& calibration, const ShapeChromaDecodePolicy& policy) noexcept
{
    ShapeChromaDecision decision;
    std::array<double, 16> luma{};
    std::array<double, 2> meanOpponent{};
    double meanLuma = 0;
    for (std::size_t pixel = 0; pixel < samples.size(); pixel++)
    {
        luma[pixel] = 0.0722 * samples[pixel][0] + 0.7152 * samples[pixel][1] + 0.2126 * samples[pixel][2];
        meanLuma += luma[pixel] / 16;
        const auto opponent = Opponent(samples[pixel]);
        meanOpponent[0] += opponent[0] / 16;
        meanOpponent[1] += opponent[1] / 16;
    }
    double totalEnergy = 0;
    for (auto& value : luma)
    {
        value -= meanLuma;
        totalEnergy += value * value;
    }

    std::array<double, 16> shapeDistances{};
    std::array<double, 16> shapeAmplitudes{};
    for (std::size_t shape = 0; shape < kShapeChromaTemplates.size(); shape++)
    {
        double correlation = 0;
        for (std::size_t pixel = 0; pixel < luma.size(); pixel++)
        {
            const double sign = (kShapeChromaTemplates[shape] & (1u << pixel)) != 0 ? 1.0 : -1.0;
            correlation += luma[pixel] * sign;
        }
        const double amplitude = std::max(0.0, correlation / 16);
        shapeAmplitudes[shape] = amplitude;
        double residual = 0;
        for (std::size_t pixel = 0; pixel < luma.size(); pixel++)
        {
            const double sign = (kShapeChromaTemplates[shape] & (1u << pixel)) != 0 ? 1.0 : -1.0;
            const double difference = luma[pixel] - amplitude * sign;
            residual += difference * difference;
        }
        shapeDistances[shape] = residual / std::max(totalEnergy, 1.0);
    }
    const auto bestShape = static_cast<std::size_t>(std::min_element(shapeDistances.begin(), shapeDistances.end()) - shapeDistances.begin());
    decision.shapeUnreliable = clipped || shapeAmplitudes[bestShape] < policy.minimumShapeAmplitude ||
        shapeDistances[bestShape] > policy.maximumShapeResidual;
    for (std::size_t bit = 0; bit < kShapeChromaShapeBits; bit++)
    {
        double zeroDistance = std::numeric_limits<double>::max();
        double oneDistance = zeroDistance;
        for (std::size_t shape = 0; shape < shapeDistances.size(); shape++)
        {
            auto& candidate = (shape & (std::size_t{1} << bit)) != 0 ? oneDistance : zeroDistance;
            candidate = std::min(candidate, shapeDistances[shape]);
        }
        const double difference = oneDistance - zeroDistance;
        decision.label = static_cast<std::uint8_t>(decision.label | (static_cast<std::uint8_t>(difference < 0) << bit));
        decision.metrics[bit] = decision.shapeUnreliable ? 0.0f : static_cast<float>(difference);
    }
    decision.shapeMargin = DecisionMargin(shapeDistances);

    std::array<double, 4> chromaDistances{};
    const double normalization = calibration.minimumSeparation * calibration.minimumSeparation;
    for (std::size_t state = 0; state < chromaDistances.size(); state++)
    {
        chromaDistances[state] = SquaredDistance(meanOpponent, calibration.centroids[state]) / normalization;
    }
    const double minimumDistance = *std::min_element(chromaDistances.begin(), chromaDistances.end());
    decision.chromaUnreliable = clipped || std::sqrt(minimumDistance) > policy.maximumChromaResidual;
    for (std::size_t bit = 0; bit < kShapeChromaChromaBits; bit++)
    {
        double zeroDistance = std::numeric_limits<double>::max();
        double oneDistance = zeroDistance;
        for (std::size_t state = 0; state < chromaDistances.size(); state++)
        {
            auto& candidate = (kShapeChromaStates[state].label & (1u << bit)) != 0 ? oneDistance : zeroDistance;
            candidate = std::min(candidate, chromaDistances[state]);
        }
        const double difference = oneDistance - zeroDistance;
        decision.label = static_cast<std::uint8_t>(decision.label | (static_cast<std::uint8_t>(difference < 0) << (kShapeChromaShapeBits + bit)));
        decision.metrics[kShapeChromaShapeBits + bit] = decision.chromaUnreliable ? 0.0f : static_cast<float>(difference);
    }
    decision.chromaMargin = DecisionMargin(chromaDistances);
    return decision;
}

ShapeChromaMargin SummarizeShapeChromaMargin(const std::span<const std::uint64_t> histogram, const double minimum) noexcept
{
    ShapeChromaMargin result;
    if (histogram.size() != kShapeChromaMetricBins || histogram.data() == nullptr || !std::isfinite(minimum) || minimum < 0 || minimum > 1)
    {
        return result;
    }
    for (const auto count : histogram)
    {
        if (count > std::numeric_limits<std::uint64_t>::max() - result.samples)
        {
            return {};
        }
        result.samples += count;
    }
    if (result.samples == 0)
    {
        return result;
    }
    result.minimum = minimum;
    const auto Quantile = [&](const std::uint64_t divisor)
    {
        const std::uint64_t rank = result.samples / divisor + static_cast<std::uint64_t>(result.samples % divisor != 0);
        std::uint64_t cumulative = 0;
        for (std::size_t index = 0; index < histogram.size(); index++)
        {
            cumulative += histogram[index];
            if (cumulative >= rank)
            {
                return static_cast<double>(index) / static_cast<double>(kShapeChromaMetricBins - 1);
            }
        }
        return 1.0;
    };
    result.p50 = Quantile(2);
    result.p01 = Quantile(100);
    result.p001 = Quantile(1000);
    return result;
}

ModulationStatus EncodeShapeChromaFrame(const std::span<const std::byte> bootstrapRecord,
    const std::span<const std::byte> logicalData, const std::span<std::byte> outBgra) noexcept
{
    if (bootstrapRecord.data() == nullptr)
    {
        return ModulationStatus::Failure(ModulationErrorCode::InvalidInput, 0);
    }
    const auto parsed = pbprotocol::ParseBootstrapRecord(bootstrapRecord);
    if (!parsed)
    {
        return ModulationStatus::Failure(parsed.Error().code == pbprotocol::ProtocolErrorCode::CrcMismatch ?
            ModulationErrorCode::CrcMismatch : ModulationErrorCode::InvalidInput, parsed.Error().offset);
    }
    if (parsed.Value().visualProfileId != kShapeChromaProfileId || parsed.Value().visualLayoutVersion != kShapeChromaLayoutVersion ||
        logicalData.size() != kShapeChromaDataBytes || logicalData.data() == nullptr || outBgra.data() == nullptr ||
        Overlap(logicalData, outBgra) || Overlap(bootstrapRecord, outBgra))
    {
        return ModulationStatus::Failure(ModulationErrorCode::InvalidInput, 0);
    }
    const auto padding = logicalData.last(kShapeChromaPaddingBytes);
    const auto nonzero = std::ranges::find_if(padding, [](const std::byte value) { return value != std::byte{0}; });
    if (nonzero != padding.end())
    {
        return ModulationStatus::Failure(ModulationErrorCode::NonZeroReservedByte,
            kShapeChromaDataBytes - kShapeChromaPaddingBytes + static_cast<std::size_t>(nonzero - padding.begin()));
    }
    const auto scaffold = detail::EncodeLocalDesktopScaffold(bootstrapRecord, outBgra, detail::LocalDesktopBinding::ShapeChroma);
    if (!scaffold)
    {
        return scaffold;
    }
    for (const auto region : kDesktopLevelsLadders)
    {
        for (std::size_t state = 0; state < kShapeChromaStates.size(); state++)
        {
            const auto& color = kShapeChromaStates[state];
            detail::FillLocalDesktopColorBlock(outBgra,
                {region.x + static_cast<std::uint32_t>(state) * (region.width / 4), region.y, region.width / 4, region.height},
                CodeValue(128, color.blueOffset), CodeValue(128, color.greenOffset), CodeValue(128, color.redOffset));
        }
    }
    const auto& permutation = *pbinterleave::GetDesktopLevelsPermutation(kShapeChromaTilePixels);
    for (std::uint32_t physical = 0; physical < kShapeChromaTileCount; physical++)
    {
        LocalDesktopRegion region;
        static_cast<void>(GetLocalDesktopDataTile(kShapeChromaTilePixels, physical, region));
        const std::uint32_t logical = permutation.ToLogical(physical, parsed.Value().frameSequence);
        const std::size_t firstBit = static_cast<std::size_t>(logical) * kShapeChromaBitsPerTile;
        std::uint8_t shape = 0;
        std::uint8_t chromaLabel = 0;
        for (std::size_t bit = 0; bit < kShapeChromaShapeBits; bit++)
        {
            shape = static_cast<std::uint8_t>(shape | (static_cast<std::uint8_t>(detail::GetStreamBit(logicalData, firstBit + bit)) << bit));
        }
        for (std::size_t bit = 0; bit < kShapeChromaChromaBits; bit++)
        {
            chromaLabel = static_cast<std::uint8_t>(chromaLabel |
                (static_cast<std::uint8_t>(detail::GetStreamBit(logicalData, firstBit + kShapeChromaShapeBits + bit)) << bit));
        }
        const auto state = static_cast<std::size_t>(std::find_if(kShapeChromaStates.begin(), kShapeChromaStates.end(),
            [chromaLabel](const ShapeChromaState candidate) { return candidate.label == chromaLabel; }) - kShapeChromaStates.begin());
        const auto& color = kShapeChromaStates[state];
        for (std::uint32_t row = 0; row < kShapeChromaTilePixels; row++)
        {
            for (std::uint32_t column = 0; column < kShapeChromaTilePixels; column++)
            {
                const std::size_t pixel = static_cast<std::size_t>(row) * kShapeChromaTilePixels + column;
                const std::uint8_t base = (kShapeChromaTemplates[shape] & (1u << pixel)) != 0 ? kShapeChromaHighLuma : kShapeChromaLowLuma;
                detail::FillLocalDesktopColorBlock(outBgra, {region.x + column, region.y + row, 1, 1},
                    CodeValue(base, color.blueOffset), CodeValue(base, color.greenOffset), CodeValue(base, color.redOffset));
            }
        }
    }
    return ModulationStatus::Success();
}

ShapeChromaObservation DecodeShapeChromaFrame(const LumaView& view, ShapeChromaWorkspace& workspace,
    const std::span<std::byte> hardBits, const std::span<float> softMetrics, const ShapeChromaDecodePolicy& policy) noexcept
{
    ShapeChromaObservation observation;
    if (!workspace.implementation_)
    {
        observation.erasure = ShapeChromaErasure::WorkspaceUnavailable;
        return observation;
    }
    auto& scratch = *workspace.implementation_;
    scratch.histogramValid = false;
    if (!ValidPolicy(policy))
    {
        observation.erasure = ShapeChromaErasure::InvalidPolicy;
        return observation;
    }
    observation.bootstrap.erasure = ValidateLumaView(view);
    if (observation.bootstrap.erasure != LocalDesktopErasureReason::None)
    {
        observation.erasure = ShapeChromaErasure::InvalidInput;
        return observation;
    }
    if (view.pixelFormat != LumaPixelFormat::Bgra8)
    {
        observation.erasure = ShapeChromaErasure::UnsupportedFormat;
        return observation;
    }
    if (softMetrics.size() > std::numeric_limits<std::size_t>::max() / sizeof(float) || hardBits.data() == nullptr ||
        softMetrics.data() == nullptr || Overlap(hardBits, std::as_bytes(softMetrics)) || Overlap(view.pixels, hardBits) ||
        Overlap(view.pixels, std::as_bytes(softMetrics)))
    {
        observation.erasure = ShapeChromaErasure::OverlappingSpans;
        return observation;
    }
    observation.bootstrap = detail::DecodeLocalDesktopScaffold(view, policy.locator, detail::LocalDesktopBinding::ShapeChroma);
    if (!observation.bootstrap.IsAccepted())
    {
        observation.erasure = observation.bootstrap.erasure == LocalDesktopErasureReason::UnsupportedRecord ?
            ShapeChromaErasure::UnsupportedProfile : ShapeChromaErasure::BootstrapErasure;
        return observation;
    }
    const auto parsed = pbprotocol::ParseBootstrapRecord(observation.bootstrap.canonical44);
    if (!parsed || parsed.Value().visualProfileId != kShapeChromaProfileId || parsed.Value().visualLayoutVersion != kShapeChromaLayoutVersion)
    {
        observation.erasure = ShapeChromaErasure::UnsupportedProfile;
        return observation;
    }
    observation.erasure = ValidateShapeChromaGeometry(observation.bootstrap.geometry, policy);
    if (observation.erasure != ShapeChromaErasure::None)
    {
        return observation;
    }
    const double roundedX = std::round(observation.bootstrap.geometry.originX);
    const double roundedY = std::round(observation.bootstrap.geometry.originY);
    if (roundedX < 0 || roundedY < 0 || roundedX + 1920 > view.width || roundedY + 1080 > view.height)
    {
        observation.erasure = ShapeChromaErasure::FrameOutOfBounds;
        return observation;
    }
    if (hardBits.size() < kShapeChromaDataBytes || softMetrics.size() < kShapeChromaMaximumBits)
    {
        observation.erasure = ShapeChromaErasure::OutputBufferTooSmall;
        return observation;
    }
    const auto originX = static_cast<std::uint32_t>(roundedX);
    const auto originY = static_cast<std::uint32_t>(roundedY);
    BgraReader reader(view, policy.maximumDataWorkUnits);
    observation.erasure = CalibrateChroma(reader, originX, originY, policy, observation.calibration);
    if (observation.erasure == ShapeChromaErasure::None)
    {
        scratch.hard.fill(std::byte{0});
        scratch.shapeHistogram.fill(0);
        scratch.chromaHistogram.fill(0);
        double minimumShapeMargin = 1;
        double minimumChromaMargin = 1;
        const auto& permutation = *pbinterleave::GetDesktopLevelsPermutation(kShapeChromaTilePixels);
        for (std::uint32_t physical = 0; physical < kShapeChromaTileCount; physical++)
        {
            LocalDesktopRegion region;
            static_cast<void>(GetLocalDesktopDataTile(kShapeChromaTilePixels, physical, region));
            std::array<std::array<double, 3>, 16> samples{};
            bool clipped = false;
            for (std::uint32_t row = 0; row < kShapeChromaTilePixels; row++)
            {
                for (std::uint32_t column = 0; column < kShapeChromaTilePixels; column++)
                {
                    bool sampleClipped = false;
                    if (!reader.Pixel(originX + region.x + column, originY + region.y + row,
                        samples[static_cast<std::size_t>(row) * kShapeChromaTilePixels + column], sampleClipped))
                    {
                        observation.erasure = ShapeChromaErasure::PixelReadFailure;
                        break;
                    }
                    clipped = clipped || sampleClipped;
                }
                if (observation.erasure != ShapeChromaErasure::None)
                {
                    break;
                }
            }
            if (observation.erasure != ShapeChromaErasure::None)
            {
                break;
            }
            const auto decision = detail::DecideShapeChromaTile(samples, clipped, observation.calibration, policy);
            const std::uint32_t logical = permutation.ToLogical(physical, parsed.Value().frameSequence);
            const std::size_t firstBit = static_cast<std::size_t>(logical) * kShapeChromaBitsPerTile;
            for (std::size_t bit = 0; bit < kShapeChromaBitsPerTile; bit++)
            {
                detail::SetStreamBit(scratch.hard, firstBit + bit, (decision.label & (1u << bit)) != 0);
                scratch.soft[firstBit + bit] = decision.metrics[bit];
            }
            observation.unreliableShapeTiles += static_cast<std::uint32_t>(decision.shapeUnreliable);
            observation.unreliableChromaTiles += static_cast<std::uint32_t>(decision.chromaUnreliable);
            const auto shapeBin = static_cast<std::size_t>(decision.shapeMargin * static_cast<double>(kShapeChromaMetricBins - 1));
            const auto chromaBin = static_cast<std::size_t>(decision.chromaMargin * static_cast<double>(kShapeChromaMetricBins - 1));
            scratch.shapeHistogram[std::min(shapeBin, kShapeChromaMetricBins - 1)]++;
            scratch.chromaHistogram[std::min(chromaBin, kShapeChromaMetricBins - 1)]++;
            minimumShapeMargin = std::min(minimumShapeMargin, decision.shapeMargin);
            minimumChromaMargin = std::min(minimumChromaMargin, decision.chromaMargin);
        }
        if (observation.erasure == ShapeChromaErasure::None)
        {
            observation.dataBytes = kShapeChromaDataBytes;
            observation.shapeMargin = SummarizeShapeChromaMargin(scratch.shapeHistogram, minimumShapeMargin);
            observation.chromaMargin = SummarizeShapeChromaMargin(scratch.chromaHistogram, minimumChromaMargin);
            std::copy(scratch.hard.begin(), scratch.hard.end(), hardBits.begin());
            std::copy(scratch.soft.begin(), scratch.soft.end(), softMetrics.begin());
            scratch.histogramValid = true;
        }
    }
    observation.dataWorkUnits = reader.WorkUnits();
    observation.pixelError = reader.Error();
    if (reader.Error() == LocalDesktopErasureReason::WorkBudgetExceeded)
    {
        observation.erasure = ShapeChromaErasure::WorkBudgetExceeded;
    }
    return observation;
}

const char* GetShapeChromaErasureName(const ShapeChromaErasure reason) noexcept
{
    switch (reason)
    {
    case ShapeChromaErasure::None: return "None";
    case ShapeChromaErasure::InvalidInput: return "InvalidInput";
    case ShapeChromaErasure::InvalidPolicy: return "InvalidPolicy";
    case ShapeChromaErasure::WorkspaceUnavailable: return "WorkspaceUnavailable";
    case ShapeChromaErasure::OutputBufferTooSmall: return "OutputBufferTooSmall";
    case ShapeChromaErasure::OverlappingSpans: return "OverlappingSpans";
    case ShapeChromaErasure::BootstrapErasure: return "BootstrapErasure";
    case ShapeChromaErasure::UnsupportedProfile: return "UnsupportedProfile";
    case ShapeChromaErasure::UnsupportedFormat: return "UnsupportedFormat";
    case ShapeChromaErasure::ScaleOutOfRange: return "ScaleOutOfRange";
    case ShapeChromaErasure::AlignmentOutOfRange: return "AlignmentOutOfRange";
    case ShapeChromaErasure::FrameOutOfBounds: return "FrameOutOfBounds";
    case ShapeChromaErasure::ChromaPilotClipping: return "ChromaPilotClipping";
    case ShapeChromaErasure::ChromaPilotVariance: return "ChromaPilotVariance";
    case ShapeChromaErasure::ChromaPilotSeparation: return "ChromaPilotSeparation";
    case ShapeChromaErasure::ChromaPilotSpatialMismatch: return "ChromaPilotSpatialMismatch";
    case ShapeChromaErasure::PixelReadFailure: return "PixelReadFailure";
    case ShapeChromaErasure::WorkBudgetExceeded: return "WorkBudgetExceeded";
    }
    return "Unknown";
}

} // namespace pbmodulation
