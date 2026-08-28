#include "pbmodulation/desktop_levels.h"

#include "desktop_levels_internal.h"
#include "local_desktop_internal.h"
#include "luma_reader.h"
#include "pbinterleave/tile_permutation.h"
#include "pbprotocol/bootstrap_control_codec.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace pbmodulation
{
namespace
{
constexpr DesktopLevelsProfile profile2{kDesktopLevels2ProfileId, 2, 346752, 86688, 42, 1638};
constexpr DesktopLevelsProfile profile4{kDesktopLevels4ProfileId, 4, 86688, 21672, 10, 1422};
constexpr std::array<std::uint32_t, 7> bandHeights{64, 128, 188, 128, 188, 128, 64};
static_assert((1728 * 888 - 9 * 128 * 128) / 4 == profile2.tileCount);
static_assert((1728 * 888 - 9 * 128 * 128) / 16 == profile4.tileCount);
static_assert(profile2.dataBytes * 4 == profile2.tileCount && profile4.dataBytes * 4 == profile4.tileCount);
static_assert(profile2.codewords * 2025 + profile2.paddingBytes == profile2.dataBytes);
static_assert(profile4.codewords * 2025 + profile4.paddingBytes == profile4.dataBytes);

constexpr bool ValidRegions() noexcept
{
    std::array<LocalDesktopRegion, 21> regions{};
    std::size_t count = 0;
    for (const auto region : kLocalDesktopMarkerRegions)
    {
        regions[count++] = region;
    }
    for (const auto region : kLocalDesktopBootstrapRegions)
    {
        regions[count++] = region;
    }
    for (const auto region : kLocalDesktopTimingRegions)
    {
        regions[count++] = region;
    }
    for (const auto region : kDesktopLevelsLadders)
    {
        regions[count++] = region;
    }
    for (const auto region : kDesktopLevelsPhasePilots)
    {
        regions[count++] = region;
    }
    for (std::size_t index = 0; index < regions.size(); index++)
    {
        const auto region = regions[index];
        if (region.width == 0 || region.height == 0 || region.x + region.width > 1920 || region.y + region.height > 1080)
        {
            return false;
        }
        for (std::size_t other = 0; other < index; other++)
        {
            const auto previous = regions[other];
            if (region.x < previous.x + previous.width && previous.x < region.x + region.width &&
                region.y < previous.y + previous.height && previous.y < region.y + region.height)
            {
                return false;
            }
        }
    }
    return count == regions.size();
}
static_assert(ValidRegions());

bool Overlap(const std::span<const std::byte> first, const std::span<const std::byte> second) noexcept
{
    if (first.empty() || second.empty())
    {
        return false;
    }
    const auto firstAddress = reinterpret_cast<std::uintptr_t>(first.data());
    const auto secondAddress = reinterpret_cast<std::uintptr_t>(second.data());
    // Reject wrapped declared spans before forming an out-of-range pointer.
    if (first.size() > std::numeric_limits<std::uintptr_t>::max() - firstAddress ||
        second.size() > std::numeric_limits<std::uintptr_t>::max() - secondAddress)
    {
        return true;
    }
    return firstAddress <= secondAddress ? secondAddress - firstAddress < first.size() : firstAddress - secondAddress < second.size();
}

bool ValidPolicy(const DesktopLevelsDecodePolicy& policy) noexcept
{
    const std::array<double, 7> values{policy.maximumScaleDriftPixels, policy.maximumPhaseErrorPixels, policy.maximumMarkerResidualPixels,
        policy.maximumPhasePilotResidual, policy.minimumLevelGap, policy.maximumPilotStandardDeviation, policy.maximumPilotSpatialDeviation};
    const auto ValidNumber = [](const double value)
    {
        return std::isfinite(value) && value >= 0;
    };
    return std::ranges::all_of(values, ValidNumber) &&
        policy.maximumScaleDriftPixels <= 0.125 && policy.maximumPhaseErrorPixels <= 0.125 && policy.maximumMarkerResidualPixels <= 0.125 &&
        policy.maximumPhasePilotResidual <= 0.125 && policy.minimumLevelGap >= 32 && policy.minimumLevelGap <= 255 &&
        policy.maximumPilotStandardDeviation <= 8 && policy.maximumPilotSpatialDeviation <= 8 &&
        policy.maximumDataWorkUnits > 0 && policy.maximumDataWorkUnits <= 8000000;
}

LocalDesktopRegion TileRegion(const DesktopLevelsProfile& profile, std::uint32_t physicalIndex) noexcept
{
    std::uint32_t y = kDesktopLevelsDataGrid.y;
    for (std::size_t band = 0; band < bandHeights.size(); band++)
    {
        const bool hasTiming = band % 2 != 0;
        const std::uint32_t rowTiles = (hasTiming ? 1344u : 1728u) / profile.tilePixels;
        const std::uint32_t count = rowTiles * (bandHeights[band] / profile.tilePixels);
        if (physicalIndex < count)
        {
            const std::uint32_t column = physicalIndex % rowTiles;
            std::uint32_t x = kDesktopLevelsDataGrid.x + column * profile.tilePixels;
            if (hasTiming)
            {
                x += column < 672 / profile.tilePixels ? 128 : 256;
            }
            return {x, y + (physicalIndex / rowTiles) * profile.tilePixels, profile.tilePixels, profile.tilePixels};
        }
        physicalIndex -= count;
        y += bandHeights[band];
    }
    return {};
}

DesktopLevelsErasure CalibrateLevels(detail::LumaReader& reader, const std::uint32_t originX, const std::uint32_t originY,
                                     const DesktopLevelsDecodePolicy& policy, DesktopLevelsCalibration& calibration) noexcept
{
    auto& means = calibration.ladderCentroids;
    for (std::size_t ladder = 0; ladder < kDesktopLevelsLadders.size(); ladder++)
    {
        const auto region = kDesktopLevelsLadders[ladder];
        for (std::size_t level = 0; level < 4; level++)
        {
            double sum = 0;
            double squares = 0;
            for (std::uint32_t row = 0; row < 64; row++)
            {
                for (std::uint32_t column = 0; column < 32; column++)
                {
                    double sample = 0;
                    bool clipped = false;
                    if (!reader.Pixel(originX + region.x + static_cast<std::uint32_t>(level) * 32 + column, originY + region.y + row, sample, &clipped))
                    {
                        return DesktopLevelsErasure::PixelReadFailure;
                    }
                    if (clipped)
                    {
                        return DesktopLevelsErasure::PilotClipping;
                    }
                    sum += sample;
                    squares += sample * sample;
                }
            }
            const double mean = sum / 2048;
            const double variance = std::max(0.0, squares / 2048 - mean * mean);
            if (variance > policy.maximumPilotStandardDeviation * policy.maximumPilotStandardDeviation)
            {
                return DesktopLevelsErasure::PilotVariance;
            }
            means[ladder][level] = mean;
            calibration.ladderVariances[ladder][level] = variance;
            calibration.centroids[level] += mean / 4;
            calibration.variances[level] += variance / 4;
            if (level != 0 && mean - means[ladder][level - 1] < policy.minimumLevelGap)
            {
                return DesktopLevelsErasure::PilotOrder;
            }
        }
    }
    calibration.minimumGap = 255;
    for (std::size_t level = 0; level < 4; level++)
    {
        double lowest = means[0][level];
        double highest = lowest;
        for (std::size_t ladder = 1; ladder < 4; ladder++)
        {
            lowest = std::min(lowest, means[ladder][level]);
            highest = std::max(highest, means[ladder][level]);
        }
        calibration.spatialDeviation = std::max(calibration.spatialDeviation, highest - lowest);
        if (level != 0)
        {
            calibration.minimumGap = std::min(calibration.minimumGap, calibration.centroids[level] - calibration.centroids[level - 1]);
        }
    }
    if (calibration.spatialDeviation > policy.maximumPilotSpatialDeviation)
    {
        return DesktopLevelsErasure::PilotSpatialMismatch;
    }
    if (calibration.minimumGap < policy.minimumLevelGap)
    {
        return DesktopLevelsErasure::PilotOrder;
    }
    const double contrast = calibration.centroids[3] - calibration.centroids[0];
    for (std::size_t pilot = 0; pilot < kDesktopLevelsPhasePilots.size(); pilot++)
    {
        const auto region = kDesktopLevelsPhasePilots[pilot];
        for (std::uint32_t axis = 0; axis < 2; axis++)
        {
            double residual = 0;
            for (std::uint32_t row = 0; row < 64; row++)
            {
                for (std::uint32_t column = 0; column < 64; column++)
                {
                    double sample = 0;
                    bool clipped = false;
                    if (!reader.Pixel(originX + region.x + axis * 64 + column, originY + region.y + row, sample, &clipped))
                    {
                        return DesktopLevelsErasure::PixelReadFailure;
                    }
                    if (clipped)
                    {
                        return DesktopLevelsErasure::PilotClipping;
                    }
                    const auto level = (axis == 0 ? column : row) % 2 == 0 ? 0u : 3u;
                    residual += std::abs(sample - calibration.centroids[level]) / contrast;
                }
            }
            calibration.phaseResiduals[pilot][axis] = residual / 4096;
            calibration.phaseResidual = std::max(calibration.phaseResidual, calibration.phaseResiduals[pilot][axis]);
        }
    }
    return calibration.phaseResidual > policy.maximumPhasePilotResidual ? DesktopLevelsErasure::PhasePilotMismatch : DesktopLevelsErasure::None;
}
} // namespace

struct DesktopLevelsWorkspace::Implementation
{
    std::array<std::byte, kDesktopLevelsMaximumDataBytes> hard{};
    std::array<float, kDesktopLevelsMaximumBits> soft{};
    std::array<std::uint64_t, kDesktopLevelsMarginBins> histogram{};
    bool histogramValid = false;
};

DesktopLevelsWorkspace::DesktopLevelsWorkspace() noexcept = default;
DesktopLevelsWorkspace::DesktopLevelsWorkspace(DesktopLevelsWorkspace&&) noexcept = default;
DesktopLevelsWorkspace& DesktopLevelsWorkspace::operator=(DesktopLevelsWorkspace&&) noexcept = default;
DesktopLevelsWorkspace::~DesktopLevelsWorkspace() = default;

std::uint64_t DesktopLevelsWorkspace::RequiredBytes() noexcept
{
    return sizeof(Implementation);
}

ModulationResult<DesktopLevelsWorkspace> DesktopLevelsWorkspace::Create(const std::uint64_t maximumBytes) noexcept
{
    if (maximumBytes < RequiredBytes())
    {
        return ModulationResult<DesktopLevelsWorkspace>::Failure(ModulationErrorCode::InvalidInput, 0);
    }
    try
    {
        DesktopLevelsWorkspace workspace;
        workspace.implementation_ = std::make_unique<Implementation>();
        return ModulationResult<DesktopLevelsWorkspace>::Success(std::move(workspace));
    }
    catch (const std::bad_alloc&)
    {
        return ModulationResult<DesktopLevelsWorkspace>::Failure(ModulationErrorCode::MemoryAllocationFailure, 0);
    }
}

std::span<const std::uint64_t> DesktopLevelsWorkspace::GetMarginHistogram() const noexcept
{
    return implementation_ && implementation_->histogramValid ? std::span<const std::uint64_t>(implementation_->histogram) : std::span<const std::uint64_t>{};
}

const DesktopLevelsProfile* GetDesktopLevelsProfile(const std::uint64_t profileId) noexcept
{
    return profileId == profile2.visualProfileId ? &profile2 : profileId == profile4.visualProfileId ? &profile4 : nullptr;
}

bool GetDesktopLevelsTile(const std::uint64_t profileId, const std::uint32_t physicalIndex, LocalDesktopRegion& output) noexcept
{
    const auto* const profile = GetDesktopLevelsProfile(profileId);
    if (profile == nullptr || physicalIndex >= profile->tileCount)
    {
        return false;
    }
    output = TileRegion(*profile, physicalIndex);
    return true;
}

DesktopLevelsErasure ValidateDesktopLevelsGeometry(const LocalDesktopGeometry& geometry, const DesktopLevelsDecodePolicy& policy) noexcept
{
    if (!ValidPolicy(policy))
    {
        return DesktopLevelsErasure::InvalidPolicy;
    }
    const std::array<double, 5> values{geometry.originX, geometry.originY, geometry.scaleX, geometry.scaleY, geometry.markerResidualPixels};
    const auto Finite = [](const double value)
    {
        return std::isfinite(value);
    };
    if (!std::ranges::all_of(values, Finite) || geometry.markerResidualPixels < 0)
    {
        return DesktopLevelsErasure::InvalidInput;
    }
    if (std::abs(geometry.scaleX - 1) * 1920 > policy.maximumScaleDriftPixels || std::abs(geometry.scaleY - 1) * 1080 > policy.maximumScaleDriftPixels)
    {
        return DesktopLevelsErasure::ScaleOutOfRange;
    }
    if (std::abs(geometry.originX - std::round(geometry.originX)) > policy.maximumPhaseErrorPixels ||
        std::abs(geometry.originY - std::round(geometry.originY)) > policy.maximumPhaseErrorPixels || geometry.markerResidualPixels > policy.maximumMarkerResidualPixels)
    {
        return DesktopLevelsErasure::AlignmentOutOfRange;
    }
    return DesktopLevelsErasure::None;
}

detail::DesktopLevelsDecision detail::DecideDesktopLevelsTile(const double mean, const double variance, const bool clipped,
                                                              const DesktopLevelsCalibration& calibration) noexcept
{
    DesktopLevelsDecision decision;
    std::array<double, 4> distances{};
    for (std::size_t level = 0; level < 4; level++)
    {
        const double difference = mean - calibration.centroids[level];
        distances[level] = difference * difference;
    }
    const double gapSquared = calibration.minimumGap * calibration.minimumGap;
    decision.unreliable = clipped || variance > gapSquared / 16;
    for (std::size_t bit = 0; bit < 2; bit++)
    {
        double zeroDistance = std::numeric_limits<double>::max();
        double oneDistance = zeroDistance;
        for (std::size_t level = 0; level < 4; level++)
        {
            auto& candidate = (kDesktopLevelsLabels[level] & (1u << bit)) != 0 ? oneDistance : zeroDistance;
            candidate = std::min(candidate, distances[level]);
        }
        const double difference = oneDistance - zeroDistance;
        if (difference < 0)
        {
            decision.label |= static_cast<std::uint8_t>(1u << bit);
        }
        decision.metrics[bit] = decision.unreliable ? 0.0f : static_cast<float>(difference / gapSquared);
    }
    std::sort(distances.begin(), distances.end());
    decision.margin = (distances[1] - distances[0]) / (distances[1] + distances[0]);
    return decision;
}

DesktopLevelsMargin SummarizeDesktopLevelsMargin(const std::span<const std::uint64_t> histogram, const double minimum) noexcept
{
    DesktopLevelsMargin result;
    if (histogram.size() != kDesktopLevelsMarginBins || histogram.data() == nullptr || !std::isfinite(minimum) || minimum < 0 || minimum > 1)
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
    const auto quantile = [&](const std::uint64_t divisor)
    {
        const std::uint64_t rank = result.samples / divisor + static_cast<std::uint64_t>(result.samples % divisor != 0);
        std::uint64_t cumulative = 0;
        for (std::size_t index = 0; index < histogram.size(); index++)
        {
            cumulative += histogram[index];
            if (cumulative >= rank)
            {
                return static_cast<double>(index) / static_cast<double>(kDesktopLevelsMarginBins - 1);
            }
        }
        return 1.0;
    };
    result.p50 = quantile(2);
    result.p01 = quantile(100);
    result.p001 = quantile(1000);
    return result;
}

ModulationStatus EncodeDesktopLevelsFrame(const std::span<const std::byte> bootstrapRecord, const std::span<const std::byte> logicalData,
                                         const std::span<std::byte> outBgra) noexcept
{
    if (bootstrapRecord.data() == nullptr)
    {
        return ModulationStatus::Failure(ModulationErrorCode::InvalidInput, 0);
    }
    const auto parsed = pbprotocol::ParseBootstrapRecord(bootstrapRecord);
    if (!parsed)
    {
        return ModulationStatus::Failure(parsed.Error().code == pbprotocol::ProtocolErrorCode::CrcMismatch ? ModulationErrorCode::CrcMismatch : ModulationErrorCode::InvalidInput, parsed.Error().offset);
    }
    const auto* const profile = GetDesktopLevelsProfile(parsed.Value().visualProfileId);
    if (profile == nullptr || parsed.Value().visualLayoutVersion != kDesktopLevelsLayoutVersion || logicalData.size() != profile->dataBytes ||
        logicalData.data() == nullptr || outBgra.data() == nullptr || Overlap(logicalData, outBgra) || Overlap(bootstrapRecord, outBgra))
    {
        return ModulationStatus::Failure(ModulationErrorCode::InvalidInput, 0);
    }
    const auto padding = logicalData.last(profile->paddingBytes);
    const auto nonzero = std::ranges::find_if(padding, [](const std::byte value)
    {
        return value != std::byte{0};
    });
    if (nonzero != padding.end())
    {
        return ModulationStatus::Failure(ModulationErrorCode::NonZeroReservedByte,
            profile->dataBytes - profile->paddingBytes + static_cast<std::size_t>(nonzero - padding.begin()));
    }
    const auto scaffold = detail::EncodeLocalDesktopScaffold(bootstrapRecord, outBgra, detail::LocalDesktopBinding::DesktopLevels);
    if (!scaffold)
    {
        return scaffold;
    }
    // All remaining stores are infallible, use frozen bounds, and cannot alias
    // inputs. Existing scaffold pixels and Timing are never overwritten.
    for (const auto region : kDesktopLevelsLadders)
    {
        for (std::uint32_t level = 0; level < 4; level++)
        {
            detail::FillLocalDesktopBlock(outBgra, {region.x + level * 32, region.y, 32, 64}, kDesktopLevelsLuma[level]);
        }
    }
    for (const auto region : kDesktopLevelsPhasePilots)
    {
        for (std::uint32_t row = 0; row < 64; row++)
        {
            for (std::uint32_t column = 0; column < 128; column++)
            {
                const auto level = (column < 64 ? column : row) % 2 == 0 ? 0u : 3u;
                detail::FillLocalDesktopBlock(outBgra, {region.x + column, region.y + row, 1, 1}, kDesktopLevelsLuma[level]);
            }
        }
    }
    const auto& permutation = *pbinterleave::GetDesktopLevelsPermutation(profile->tilePixels);
    for (std::uint32_t physical = 0; physical < profile->tileCount; physical++)
    {
        const std::uint32_t logical = permutation.ToLogical(physical, parsed.Value().frameSequence);
        const auto label = (std::to_integer<unsigned>(logicalData[logical / 4]) >> ((logical % 4) * 2)) & 3u;
        detail::FillLocalDesktopBlock(outBgra, TileRegion(*profile, physical), kDesktopLevelsLuma[kDesktopLevelsLabels[label]]);
    }
    return ModulationStatus::Success();
}

DesktopLevelsObservation DecodeDesktopLevelsFrame(const LumaView& view, DesktopLevelsWorkspace& workspace, const std::span<std::byte> hardBits,
                                                  const std::span<float> softMetrics, const DesktopLevelsDecodePolicy& policy) noexcept
{
    DesktopLevelsObservation observation;
    if (!workspace.implementation_)
    {
        observation.erasure = DesktopLevelsErasure::WorkspaceUnavailable;
        return observation;
    }
    auto& scratch = *workspace.implementation_;
    scratch.histogramValid = false;
    if (!ValidPolicy(policy))
    {
        observation.erasure = DesktopLevelsErasure::InvalidPolicy;
        return observation;
    }
    observation.bootstrap.erasure = ValidateLumaView(view);
    if (observation.bootstrap.erasure != LocalDesktopErasureReason::None)
    {
        observation.erasure = DesktopLevelsErasure::InvalidInput;
        return observation;
    }
    if (softMetrics.size() > std::numeric_limits<std::size_t>::max() / sizeof(float) || hardBits.data() == nullptr || softMetrics.data() == nullptr)
    {
        return observation;
    }
    if (Overlap(hardBits, std::as_bytes(softMetrics)) || Overlap(view.pixels, hardBits) || Overlap(view.pixels, std::as_bytes(softMetrics)))
    {
        observation.erasure = DesktopLevelsErasure::OverlappingSpans;
        return observation;
    }
    observation.bootstrap = detail::DecodeLocalDesktopScaffold(view, policy.locator, detail::LocalDesktopBinding::DesktopLevels);
    if (!observation.bootstrap.IsAccepted())
    {
        observation.erasure = observation.bootstrap.erasure == LocalDesktopErasureReason::UnsupportedRecord ? DesktopLevelsErasure::UnsupportedProfile : DesktopLevelsErasure::BootstrapErasure;
        return observation;
    }
    const auto parsed = pbprotocol::ParseBootstrapRecord(observation.bootstrap.canonical44);
    const auto* const profile = parsed ? GetDesktopLevelsProfile(parsed.Value().visualProfileId) : nullptr;
    if (profile == nullptr)
    {
        observation.erasure = DesktopLevelsErasure::UnsupportedProfile;
        return observation;
    }
    observation.profileId = profile->visualProfileId;
    observation.erasure = ValidateDesktopLevelsGeometry(observation.bootstrap.geometry, policy);
    if (observation.erasure != DesktopLevelsErasure::None)
    {
        return observation;
    }
    const double roundedX = std::round(observation.bootstrap.geometry.originX);
    const double roundedY = std::round(observation.bootstrap.geometry.originY);
    if (roundedX < 0 || roundedY < 0 || roundedX + 1920 > view.width || roundedY + 1080 > view.height)
    {
        observation.erasure = DesktopLevelsErasure::FrameOutOfBounds;
        return observation;
    }
    if (hardBits.size() < profile->dataBytes || softMetrics.size() < profile->dataBytes * std::size_t{8})
    {
        observation.erasure = DesktopLevelsErasure::OutputBufferTooSmall;
        return observation;
    }
    const auto originX = static_cast<std::uint32_t>(roundedX);
    const auto originY = static_cast<std::uint32_t>(roundedY);
    detail::LumaReader reader(view, policy.maximumDataWorkUnits, true);
    observation.erasure = CalibrateLevels(reader, originX, originY, policy, observation.calibration);
    if (observation.erasure == DesktopLevelsErasure::None)
    {
        std::fill_n(scratch.hard.begin(), profile->dataBytes, std::byte{0});
        scratch.histogram.fill(0);
        double minimumMargin = 1;
        const auto& permutation = *pbinterleave::GetDesktopLevelsPermutation(profile->tilePixels);
        for (std::uint32_t physical = 0; physical < profile->tileCount; physical++)
        {
            const auto region = TileRegion(*profile, physical);
            double sum = 0;
            double squares = 0;
            bool clipped = false;
            for (std::uint32_t row = 0; row < profile->tilePixels; row++)
            {
                for (std::uint32_t column = 0; column < profile->tilePixels; column++)
                {
                    double sample = 0;
                    bool sampleClipped = false;
                    if (!reader.Pixel(originX + region.x + column, originY + region.y + row, sample, &sampleClipped))
                    {
                        break;
                    }
                    sum += sample;
                    squares += sample * sample;
                    clipped = clipped || sampleClipped;
                }
            }
            if (!reader.Charge(4))
            {
                observation.erasure = DesktopLevelsErasure::PixelReadFailure;
                break;
            }
            const double count = static_cast<double>(profile->tilePixels * profile->tilePixels);
            const double mean = sum / count;
            const auto decision = detail::DecideDesktopLevelsTile(mean, std::max(0.0, squares / count - mean * mean), clipped, observation.calibration);
            const std::uint32_t logical = permutation.ToLogical(physical, parsed.Value().frameSequence);
            scratch.hard[logical / 4] |= static_cast<std::byte>(static_cast<unsigned>(decision.label) << ((logical % 4) * 2));
            scratch.soft[logical * 2] = decision.metrics[0];
            scratch.soft[logical * 2 + 1] = decision.metrics[1];
            observation.unreliableTiles += static_cast<std::uint32_t>(decision.unreliable);
            const auto bin = static_cast<std::size_t>(decision.margin * static_cast<double>(kDesktopLevelsMarginBins - 1));
            scratch.histogram[std::min(bin, kDesktopLevelsMarginBins - 1)]++;
            minimumMargin = std::min(minimumMargin, decision.margin);
        }
        if (observation.erasure == DesktopLevelsErasure::None)
        {
            observation.dataBytes = profile->dataBytes;
            observation.margin = SummarizeDesktopLevelsMargin(scratch.histogram, minimumMargin);
            std::copy_n(scratch.hard.begin(), profile->dataBytes, hardBits.begin());
            std::copy_n(scratch.soft.begin(), profile->dataBytes * std::size_t{8}, softMetrics.begin());
            scratch.histogramValid = true;
        }
    }
    observation.dataWorkUnits = reader.WorkUnits();
    observation.pixelError = reader.Error();
    if (reader.Error() == LocalDesktopErasureReason::WorkBudgetExceeded)
    {
        observation.erasure = DesktopLevelsErasure::WorkBudgetExceeded;
    }
    return observation;
}

const char* GetDesktopLevelsErasureName(const DesktopLevelsErasure reason) noexcept
{
    switch (reason)
    {
    case DesktopLevelsErasure::None: return "None";
    case DesktopLevelsErasure::InvalidInput: return "InvalidInput";
    case DesktopLevelsErasure::InvalidPolicy: return "InvalidPolicy";
    case DesktopLevelsErasure::WorkspaceUnavailable: return "WorkspaceUnavailable";
    case DesktopLevelsErasure::OutputBufferTooSmall: return "OutputBufferTooSmall";
    case DesktopLevelsErasure::OverlappingSpans: return "OverlappingSpans";
    case DesktopLevelsErasure::BootstrapErasure: return "BootstrapErasure";
    case DesktopLevelsErasure::UnsupportedProfile: return "UnsupportedProfile";
    case DesktopLevelsErasure::ScaleOutOfRange: return "ScaleOutOfRange";
    case DesktopLevelsErasure::AlignmentOutOfRange: return "AlignmentOutOfRange";
    case DesktopLevelsErasure::FrameOutOfBounds: return "FrameOutOfBounds";
    case DesktopLevelsErasure::PilotClipping: return "PilotClipping";
    case DesktopLevelsErasure::PilotOrder: return "PilotOrder";
    case DesktopLevelsErasure::PilotVariance: return "PilotVariance";
    case DesktopLevelsErasure::PilotSpatialMismatch: return "PilotSpatialMismatch";
    case DesktopLevelsErasure::PhasePilotMismatch: return "PhasePilotMismatch";
    case DesktopLevelsErasure::PixelReadFailure: return "PixelReadFailure";
    case DesktopLevelsErasure::WorkBudgetExceeded: return "WorkBudgetExceeded";
    default: return "Unknown";
    }
}
} // namespace pbmodulation
