#include "pbmodulation/remote_visual_low_fps.h"

#include "local_desktop_internal.h"
#include "luma_reader.h"
#include "pbinterleave/tile_permutation.h"
#include "pbprotocol/bootstrap_control_codec.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <new>
#include <ranges>
#include <utility>

namespace pbmodulation
{
namespace
{

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

constexpr bool ValidSymbolMasks() noexcept
{
    for (std::size_t first = 0; first < kRemoteVisualLowFpsSymbolMasks.size(); first++)
    {
        if (std::popcount(kRemoteVisualLowFpsSymbolMasks[first]) != 8)
        {
            return false;
        }
        for (std::size_t second = first + 1; second < kRemoteVisualLowFpsSymbolMasks.size(); second++)
        {
            if (std::popcount(static_cast<std::uint16_t>(kRemoteVisualLowFpsSymbolMasks[first] ^
                kRemoteVisualLowFpsSymbolMasks[second])) < 8)
            {
                return false;
            }
        }
    }
    return true;
}

static_assert(ValidSymbolMasks());

bool ValidPolicy(const RemoteVisualLowFpsDecodePolicy& policy) noexcept
{
    constexpr LocalDesktopDecodePolicy locatorDefaults;
    const auto& locator = policy.locator;
    const std::array<double, 11> locatorValues{locator.minimumScale, locator.maximumScale, locator.minimumContrast,
        locator.maximumMarkerResidual, locator.maximumBootstrapResidual, locator.maximumTimingResidual,
        locator.maximumTimingBitErrorFraction, locator.maximumMidGrayFraction, locator.midGrayBoundary,
        locator.maximumGeometryResidualPixels, static_cast<double>(locator.maximumWorkUnits)};
    const bool validLocatorNumbers = std::ranges::all_of(locatorValues, [](const double value)
    {
        return std::isfinite(value) && value >= 0;
    });
    const bool validLocator = validLocatorNumbers && locator.maximumWorkUnits > 0 &&
        locator.maximumWorkUnits <= locatorDefaults.maximumWorkUnits && locator.maximumMarkers > 0 &&
        locator.maximumMarkers <= locatorDefaults.maximumMarkers && locator.maximumGeometries > 0 &&
        locator.maximumGeometries <= locatorDefaults.maximumGeometries && locator.maximumRefinementIterations > 0 &&
        locator.maximumRefinementIterations <= locatorDefaults.maximumRefinementIterations &&
        locator.minimumScale >= locatorDefaults.minimumScale && locator.maximumScale <= locatorDefaults.maximumScale &&
        locator.minimumScale <= locator.maximumScale && locator.minimumContrast >= locatorDefaults.minimumContrast &&
        locator.minimumContrast <= 255 && locator.maximumMarkerResidual <= locatorDefaults.maximumMarkerResidual &&
        locator.maximumBootstrapResidual <= locatorDefaults.maximumBootstrapResidual &&
        locator.maximumTimingResidual <= locatorDefaults.maximumTimingResidual &&
        locator.maximumTimingBitErrorFraction <= locatorDefaults.maximumTimingBitErrorFraction &&
        locator.maximumMidGrayFraction <= locatorDefaults.maximumMidGrayFraction &&
        locator.midGrayBoundary <= locatorDefaults.midGrayBoundary && locator.maximumGeometryResidualPixels > 0 &&
        locator.maximumGeometryResidualPixels <= locatorDefaults.maximumGeometryResidualPixels;
    const std::array<double, 10> values{policy.minimumScale, policy.maximumScale, policy.maximumMarkerResidualPixels,
        policy.minimumEndpointSeparation, policy.maximumPilotStandardDeviation, policy.maximumPilotSpatialDeviation,
        policy.minimumSymbolRms, policy.maximumSymbolResidual, policy.minimumSymbolMargin, policy.minimumFreshnessMetric};
    const auto ValidNumber = [](const double value)
    {
        return std::isfinite(value) && value >= 0;
    };
    return validLocator && std::ranges::all_of(values, ValidNumber) && policy.minimumScale >= 0.5 &&
        policy.maximumScale <= 2.0 && policy.minimumScale <= policy.maximumScale &&
        policy.maximumMarkerResidualPixels <= 1.25 && policy.minimumEndpointSeparation >= 64 &&
        policy.minimumEndpointSeparation <= 255 && policy.maximumPilotStandardDeviation <= 32 &&
        policy.maximumPilotSpatialDeviation <= 32 && policy.minimumSymbolRms > 0 &&
        policy.minimumSymbolRms <= 1 && policy.maximumSymbolResidual > 0 &&
        policy.maximumSymbolResidual <= 2 && policy.minimumSymbolMargin > 0 &&
        policy.minimumSymbolMargin <= 1 && policy.minimumFreshnessMetric > 0 &&
        policy.minimumFreshnessMetric <= 1 && policy.maximumDataWorkUnits > 0 &&
        policy.maximumDataWorkUnits <= 8000000;
}

bool SampleLogical(detail::LumaReader& reader, const LocalDesktopGeometry& geometry, const double logicalX,
    const double logicalY, double& output, bool& clipped) noexcept
{
    const double physicalX = geometry.originX + geometry.scaleX * logicalX - 0.5;
    const double physicalY = geometry.originY + geometry.scaleY * logicalY - 0.5;
    if (!reader.Sample(physicalX, physicalY, output))
    {
        return false;
    }
    clipped = clipped || output <= 0 || output >= 255;
    return true;
}

RemoteVisualLowFpsErasure Calibrate(detail::LumaReader& reader, const LocalDesktopGeometry& geometry,
    const RemoteVisualLowFpsDecodePolicy& policy, RemoteVisualCalibration& calibration) noexcept
{
    for (std::size_t ladder = 0; ladder < kRemoteVisualLadders.size(); ladder++)
    {
        const LocalDesktopRegion region = kRemoteVisualLadders[ladder];
        for (std::size_t endpoint = 0; endpoint < 2; endpoint++)
        {
            const std::uint32_t level = endpoint == 0 ? 0 : 3;
            double sum = 0;
            double squares = 0;
            std::uint32_t samples = 0;
            bool clipped = false;
            for (std::uint32_t row = 4; row < 60; row += 7)
            {
                for (std::uint32_t column = 4; column < 28; column += 3)
                {
                    double sample = 0;
                    if (!SampleLogical(reader, geometry, region.x + level * 32 + column + 0.5,
                        region.y + row + 0.5, sample, clipped))
                    {
                        return RemoteVisualLowFpsErasure::PixelReadFailure;
                    }
                    sum += sample;
                    squares += sample * sample;
                    samples++;
                }
            }
            if (clipped)
            {
                return RemoteVisualLowFpsErasure::PilotClipping;
            }
            const double sampleCount = static_cast<double>(samples);
            const double mean = sum / sampleCount;
            const double variance = std::max(0.0, squares / sampleCount - mean * mean);
            if (variance > policy.maximumPilotStandardDeviation * policy.maximumPilotStandardDeviation)
            {
                return RemoteVisualLowFpsErasure::PilotVariance;
            }
            calibration.ladderCentroids[ladder][endpoint] = mean;
            calibration.ladderVariances[ladder][endpoint] = variance;
            calibration.centroids[endpoint] += mean / static_cast<double>(kRemoteVisualLadders.size());
            calibration.variances[endpoint] += variance / static_cast<double>(kRemoteVisualLadders.size());
        }
        if (calibration.ladderCentroids[ladder][1] - calibration.ladderCentroids[ladder][0] < policy.minimumEndpointSeparation)
        {
            return RemoteVisualLowFpsErasure::PilotOrder;
        }
    }
    for (std::size_t endpoint = 0; endpoint < 2; endpoint++)
    {
        double minimum = calibration.ladderCentroids[0][endpoint];
        double maximum = minimum;
        for (std::size_t ladder = 1; ladder < kRemoteVisualLadders.size(); ladder++)
        {
            minimum = std::min(minimum, calibration.ladderCentroids[ladder][endpoint]);
            maximum = std::max(maximum, calibration.ladderCentroids[ladder][endpoint]);
        }
        calibration.spatialDeviation = std::max(calibration.spatialDeviation, maximum - minimum);
    }
    calibration.separation = calibration.centroids[1] - calibration.centroids[0];
    if (calibration.spatialDeviation > policy.maximumPilotSpatialDeviation)
    {
        return RemoteVisualLowFpsErasure::PilotSpatialMismatch;
    }
    return calibration.separation < policy.minimumEndpointSeparation ? RemoteVisualLowFpsErasure::PilotOrder :
        RemoteVisualLowFpsErasure::None;
}

bool SampleTileChips(detail::LumaReader& reader, const LocalDesktopGeometry& geometry,
    const LocalDesktopRegion& region, std::array<double, 16>& chips, bool& clipped) noexcept
{
    constexpr std::array<std::array<double, 2>, 5> offsets{
        std::array<double, 2>{0, 0}, std::array<double, 2>{-0.45, 0}, std::array<double, 2>{0.45, 0},
        std::array<double, 2>{0, -0.45}, std::array<double, 2>{0, 0.45}};
    for (std::uint32_t chipRow = 0; chipRow < 4; chipRow++)
    {
        for (std::uint32_t chipColumn = 0; chipColumn < 4; chipColumn++)
        {
            double sum = 0;
            for (const auto& offset : offsets)
            {
                double sample = 0;
                if (!SampleLogical(reader, geometry, region.x + chipColumn * 2 + 1.0 + offset[0],
                    region.y + chipRow * 2 + 1.0 + offset[1], sample, clipped))
                {
                    return false;
                }
                sum += sample;
            }
            chips[chipRow * 4 + chipColumn] = sum / static_cast<double>(offsets.size());
        }
    }
    return true;
}

void DecodeSymbol(const std::array<double, 16>& chips, const RemoteVisualCalibration& calibration,
    const RemoteVisualLowFpsDecodePolicy& policy, std::array<float, 4>& metrics, bool& unreliable,
    double& symbolMargin) noexcept
{
    const double midpoint = (calibration.centroids[0] + calibration.centroids[1]) * 0.5;
    const double halfSeparation = calibration.separation * 0.5;
    std::array<double, 16> normalized{};
    double mean = 0;
    for (std::size_t chip = 0; chip < normalized.size(); chip++)
    {
        normalized[chip] = (chips[chip] - midpoint) / halfSeparation;
        mean += normalized[chip] / static_cast<double>(normalized.size());
    }
    double energy = 0;
    for (double& value : normalized)
    {
        value -= mean;
        energy += value * value / static_cast<double>(normalized.size());
    }
    const double rms = std::sqrt(energy);
    std::array<double, 16> distances{};
    for (std::size_t symbol = 0; symbol < distances.size(); symbol++)
    {
        double distance = 0;
        for (std::size_t chip = 0; chip < normalized.size(); chip++)
        {
            const double expected = (kRemoteVisualLowFpsSymbolMasks[symbol] & (1u << chip)) != 0 ? 1.0 : -1.0;
            const double difference = normalized[chip] - expected;
            distance += difference * difference / static_cast<double>(normalized.size());
        }
        distances[symbol] = distance;
    }
    std::array<double, 2> best{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
    for (const double distance : distances)
    {
        if (distance < best[0])
        {
            best[1] = best[0];
            best[0] = distance;
        }
        else if (distance < best[1])
        {
            best[1] = distance;
        }
    }
    symbolMargin = (best[1] - best[0]) / std::max(1e-9, best[1] + best[0]);
    unreliable = unreliable || rms < policy.minimumSymbolRms || best[0] > policy.maximumSymbolResidual ||
        symbolMargin < policy.minimumSymbolMargin;
    if (unreliable)
    {
        metrics.fill(0);
        return;
    }
    for (std::size_t plane = 0; plane < metrics.size(); plane++)
    {
        double zeroDistance = std::numeric_limits<double>::infinity();
        double oneDistance = std::numeric_limits<double>::infinity();
        for (std::size_t symbol = 0; symbol < distances.size(); symbol++)
        {
            if ((symbol & (std::size_t{1} << plane)) == 0)
            {
                zeroDistance = std::min(zeroDistance, distances[symbol]);
            }
            else
            {
                oneDistance = std::min(oneDistance, distances[symbol]);
            }
        }
        metrics[plane] = static_cast<float>((oneDistance - zeroDistance) * 0.5);
    }
}

} // namespace

struct RemoteVisualLowFpsWorkspace::Implementation
{
    std::array<std::byte, kRemoteVisualLowFpsDataBytes> hard{};
    std::array<float, kRemoteVisualLowFpsCodedBits> soft{};
    std::array<float, kRemoteVisualTileCount * kRemoteVisualLowFpsBitsPerTile> physicalBitMetrics{};
    std::array<float, kRemoteVisualTileCount> physicalFreshnessMetrics{};
    std::array<std::uint64_t, kRemoteVisualMarginBins> histogram{};
    bool histogramValid = false;
};

RemoteVisualLowFpsWorkspace::RemoteVisualLowFpsWorkspace() noexcept = default;
RemoteVisualLowFpsWorkspace::RemoteVisualLowFpsWorkspace(RemoteVisualLowFpsWorkspace&&) noexcept = default;
RemoteVisualLowFpsWorkspace& RemoteVisualLowFpsWorkspace::operator=(RemoteVisualLowFpsWorkspace&&) noexcept = default;
RemoteVisualLowFpsWorkspace::~RemoteVisualLowFpsWorkspace() = default;

std::uint64_t RemoteVisualLowFpsWorkspace::RequiredBytes() noexcept
{
    return sizeof(Implementation);
}

ModulationResult<RemoteVisualLowFpsWorkspace> RemoteVisualLowFpsWorkspace::Create(const std::uint64_t maximumBytes) noexcept
{
    if (maximumBytes < RequiredBytes())
    {
        return ModulationResult<RemoteVisualLowFpsWorkspace>::Failure(ModulationErrorCode::InvalidInput, 0);
    }
    try
    {
        RemoteVisualLowFpsWorkspace workspace;
        workspace.implementation_ = std::make_unique<Implementation>();
        return ModulationResult<RemoteVisualLowFpsWorkspace>::Success(std::move(workspace));
    }
    catch (const std::bad_alloc&)
    {
        return ModulationResult<RemoteVisualLowFpsWorkspace>::Failure(ModulationErrorCode::MemoryAllocationFailure, 0);
    }
}

std::span<const std::uint64_t> RemoteVisualLowFpsWorkspace::GetMarginHistogram() const noexcept
{
    return implementation_ && implementation_->histogramValid ? std::span<const std::uint64_t>(implementation_->histogram) :
        std::span<const std::uint64_t>{};
}

std::uint32_t GetRemoteVisualLowFpsLogicalBit(const std::uint32_t dataOrdinal, const std::uint32_t plane,
    const std::uint64_t frameSequence) noexcept
{
    if (dataOrdinal >= kRemoteVisualDataTileCount || plane >= kRemoteVisualLowFpsBitsPerTile)
    {
        return kRemoteVisualLowFpsCodedBits;
    }
    const std::uint32_t logical = pbinterleave::kRemoteVisualPermutation.ToLogical(dataOrdinal,
        frameSequence + kRemoteVisualLowFpsPlaneSequenceOffsets[plane]);
    return logical < kRemoteVisualLowFpsCodedBitsPerPlane ?
        plane * kRemoteVisualLowFpsCodedBitsPerPlane + logical : kRemoteVisualLowFpsCodedBits;
}

RemoteVisualLowFpsMetricResolution ResolveRemoteVisualLowFpsPhysicalMetrics(
    const std::span<const float> physicalBitMetrics, const std::span<const float> physicalFreshnessMetrics,
    const std::uint64_t sessionTag, const std::uint64_t frameSequence, const std::span<float> logicalMetrics,
    const double minimumFreshnessMetric) noexcept
{
    RemoteVisualLowFpsMetricResolution result;
    if (physicalBitMetrics.size() < kRemoteVisualTileCount * kRemoteVisualLowFpsBitsPerTile ||
        physicalFreshnessMetrics.size() < kRemoteVisualTileCount || logicalMetrics.size() < kRemoteVisualLowFpsCodedBits ||
        physicalBitMetrics.data() == nullptr || physicalFreshnessMetrics.data() == nullptr || logicalMetrics.data() == nullptr ||
        !std::isfinite(minimumFreshnessMetric) || minimumFreshnessMetric <= 0 || minimumFreshnessMetric > 1 ||
        Overlap(std::as_bytes(physicalBitMetrics), std::as_bytes(physicalFreshnessMetrics)) ||
        Overlap(std::as_bytes(physicalBitMetrics), std::as_bytes(logicalMetrics)) ||
        Overlap(std::as_bytes(physicalFreshnessMetrics), std::as_bytes(logicalMetrics)) ||
        !std::ranges::all_of(physicalBitMetrics.first(kRemoteVisualTileCount * kRemoteVisualLowFpsBitsPerTile),
            [](const float value) { return std::isfinite(value); }) ||
        !std::ranges::all_of(physicalFreshnessMetrics.first(kRemoteVisualTileCount),
            [](const float value) { return std::isfinite(value); }))
    {
        return result;
    }
    std::array<bool, kRemoteVisualFreshnessRegionCount> regionFresh{};
    std::array<bool, kRemoteVisualFreshnessRegionCount> eligibleRegions{};
    for (std::uint32_t physical = 0; physical < kRemoteVisualTileCount; physical++)
    {
        RemoteVisualTileMapping mapping;
        if (!GetRemoteVisualTileMapping(physical, mapping))
        {
            return result;
        }
        if (mapping.role != RemoteVisualTileRole::Unused)
        {
            eligibleRegions[mapping.regionId] = true;
            regionFresh[mapping.regionId] = true;
        }
    }
    for (std::uint32_t region = 0; region < eligibleRegions.size(); region++)
    {
        result.freshnessRegions += static_cast<std::uint32_t>(eligibleRegions[region]);
    }
    for (std::uint32_t physical = 0; physical < kRemoteVisualTileCount; physical++)
    {
        RemoteVisualTileMapping mapping;
        if (!GetRemoteVisualTileMapping(physical, mapping))
        {
            return result;
        }
        if (mapping.role != RemoteVisualTileRole::FreshnessTag)
        {
            continue;
        }
        bool expectedOne = false;
        if (!GetRemoteVisualFreshnessBit(sessionTag, frameSequence, physical, expectedOne))
        {
            return result;
        }
        const float metric = physicalFreshnessMetrics[physical];
        if (std::abs(metric) < minimumFreshnessMetric)
        {
            result.freshnessTagErasures++;
            regionFresh[mapping.regionId] = false;
        }
        else if ((metric < 0) != expectedOne)
        {
            result.freshnessTagMismatches++;
            regionFresh[mapping.regionId] = false;
        }
    }
    for (std::uint32_t region = 0; region < eligibleRegions.size(); region++)
    {
        result.staleRegions += static_cast<std::uint32_t>(eligibleRegions[region] && !regionFresh[region]);
    }
    std::fill_n(logicalMetrics.begin(), kRemoteVisualLowFpsCodedBits, 0.0f);
    for (std::uint32_t physical = 0; physical < kRemoteVisualTileCount; physical++)
    {
        RemoteVisualTileMapping mapping;
        if (!GetRemoteVisualTileMapping(physical, mapping))
        {
            return result;
        }
        if (mapping.role != RemoteVisualTileRole::Data)
        {
            continue;
        }
        for (std::uint32_t plane = 0; plane < kRemoteVisualLowFpsBitsPerTile; plane++)
        {
            const std::uint32_t logical = GetRemoteVisualLowFpsLogicalBit(mapping.dataOrdinal, plane, frameSequence);
            if (logical >= kRemoteVisualLowFpsCodedBits)
            {
                continue;
            }
            if (regionFresh[mapping.regionId])
            {
                logicalMetrics[logical] = physicalBitMetrics[physical * kRemoteVisualLowFpsBitsPerTile + plane];
            }
            else
            {
                result.erasedDataMetrics++;
            }
        }
    }
    result.valid = true;
    return result;
}

RemoteVisualLowFpsErasure ValidateRemoteVisualLowFpsGeometry(const LocalDesktopGeometry& geometry,
    const RemoteVisualLowFpsDecodePolicy& policy) noexcept
{
    if (!ValidPolicy(policy))
    {
        return RemoteVisualLowFpsErasure::InvalidPolicy;
    }
    const std::array<double, 5> values{geometry.originX, geometry.originY, geometry.scaleX, geometry.scaleY,
        geometry.markerResidualPixels};
    if (!std::ranges::all_of(values, [](const double value) { return std::isfinite(value); }) ||
        geometry.originX < 0 || geometry.originY < 0 || geometry.scaleX <= 0 || geometry.scaleY <= 0 ||
        geometry.markerResidualPixels < 0)
    {
        return RemoteVisualLowFpsErasure::InvalidInput;
    }
    if (geometry.scaleX < policy.minimumScale || geometry.scaleX > policy.maximumScale ||
        geometry.scaleY < policy.minimumScale || geometry.scaleY > policy.maximumScale)
    {
        return RemoteVisualLowFpsErasure::ScaleOutOfRange;
    }
    if (geometry.markerResidualPixels > policy.maximumMarkerResidualPixels)
    {
        return RemoteVisualLowFpsErasure::AlignmentOutOfRange;
    }
    return RemoteVisualLowFpsErasure::None;
}

ModulationStatus EncodeRemoteVisualLowFpsFrame(const std::span<const std::byte> bootstrapRecord,
    const std::span<const std::byte> logicalData, const std::span<std::byte> outBgra) noexcept
{
    if (bootstrapRecord.data() == nullptr || logicalData.data() == nullptr || outBgra.data() == nullptr ||
        logicalData.size() != kRemoteVisualLowFpsDataBytes || Overlap(logicalData, outBgra) || Overlap(bootstrapRecord, outBgra))
    {
        return ModulationStatus::Failure(ModulationErrorCode::InvalidInput, 0);
    }
    const auto parsed = pbprotocol::ParseBootstrapRecord(bootstrapRecord);
    if (!parsed)
    {
        return ModulationStatus::Failure(parsed.Error().code == pbprotocol::ProtocolErrorCode::CrcMismatch ?
            ModulationErrorCode::CrcMismatch : ModulationErrorCode::InvalidInput, parsed.Error().offset);
    }
    if (parsed.Value().visualProfileId != kRemoteVisualLowFpsProfileId ||
        parsed.Value().visualLayoutVersion != kRemoteVisualLowFpsLayoutVersion)
    {
        return ModulationStatus::Failure(ModulationErrorCode::InvalidInput, 0);
    }
    const auto scaffold = detail::EncodeLocalDesktopScaffold(bootstrapRecord, outBgra,
        detail::LocalDesktopBinding::RemoteVisualLowFps);
    if (!scaffold)
    {
        return scaffold;
    }
    for (const LocalDesktopRegion region : kRemoteVisualLadders)
    {
        for (std::uint32_t level = 0; level < 4; level++)
        {
            detail::FillLocalDesktopBlock(outBgra, {region.x + level * 32, region.y, 32, 64},
                static_cast<std::uint8_t>(32 + level * 64));
        }
    }
    for (std::uint32_t physical = 0; physical < kRemoteVisualTileCount; physical++)
    {
        LocalDesktopRegion region;
        RemoteVisualTileMapping mapping;
        if (!GetRemoteVisualTile(physical, region) || !GetRemoteVisualTileMapping(physical, mapping))
        {
            return ModulationStatus::Failure(ModulationErrorCode::InvalidInput, physical);
        }
        if (mapping.role == RemoteVisualTileRole::FreshnessTag)
        {
            bool one = false;
            if (!GetRemoteVisualFreshnessBit(parsed.Value().sessionTag.value, parsed.Value().frameSequence, physical, one))
            {
                return ModulationStatus::Failure(ModulationErrorCode::InvalidInput, physical);
            }
            detail::FillLocalDesktopBlock(outBgra, region, one ? kRemoteVisualOneLuma : kRemoteVisualZeroLuma);
            continue;
        }
        if (mapping.role != RemoteVisualTileRole::Data)
        {
            detail::FillLocalDesktopBlock(outBgra, region, kRemoteVisualUnusedLuma);
            continue;
        }
        std::uint32_t symbol = 0;
        for (std::uint32_t plane = 0; plane < kRemoteVisualLowFpsBitsPerTile; plane++)
        {
            const std::uint32_t logical = GetRemoteVisualLowFpsLogicalBit(mapping.dataOrdinal, plane,
                parsed.Value().frameSequence);
            if (logical < kRemoteVisualLowFpsCodedBits &&
                (std::to_integer<unsigned>(logicalData[logical / 8]) & (1u << (logical % 8))) != 0)
            {
                symbol |= 1u << plane;
            }
        }
        const std::uint16_t mask = kRemoteVisualLowFpsSymbolMasks[symbol];
        for (std::uint32_t chip = 0; chip < 16; chip++)
        {
            const LocalDesktopRegion chipRegion{region.x + (chip % 4) * 2, region.y + (chip / 4) * 2, 2, 2};
            detail::FillLocalDesktopBlock(outBgra, chipRegion,
                (mask & (1u << chip)) != 0 ? kRemoteVisualOneLuma : kRemoteVisualZeroLuma);
        }
    }
    return ModulationStatus::Success();
}

RemoteVisualLowFpsObservation DecodeRemoteVisualLowFpsFrame(const LumaView& view,
    RemoteVisualLowFpsWorkspace& workspace, const std::span<std::byte> hardBits, const std::span<float> softMetrics,
    const RemoteVisualLowFpsDecodePolicy& policy) noexcept
{
    RemoteVisualLowFpsObservation observation;
    if (!workspace.implementation_)
    {
        observation.erasure = RemoteVisualLowFpsErasure::WorkspaceUnavailable;
        return observation;
    }
    auto& scratch = *workspace.implementation_;
    scratch.histogramValid = false;
    if (!ValidPolicy(policy))
    {
        observation.erasure = RemoteVisualLowFpsErasure::InvalidPolicy;
        return observation;
    }
    observation.bootstrap.erasure = ValidateLumaView(view);
    if (observation.bootstrap.erasure != LocalDesktopErasureReason::None)
    {
        observation.erasure = RemoteVisualLowFpsErasure::InvalidInput;
        return observation;
    }
    if (hardBits.data() == nullptr || softMetrics.data() == nullptr ||
        softMetrics.size() > std::numeric_limits<std::size_t>::max() / sizeof(float))
    {
        return observation;
    }
    if (Overlap(hardBits, std::as_bytes(softMetrics)) || Overlap(view.pixels, hardBits) ||
        Overlap(view.pixels, std::as_bytes(softMetrics)))
    {
        observation.erasure = RemoteVisualLowFpsErasure::OverlappingSpans;
        return observation;
    }
    observation.bootstrap = detail::DecodeLocalDesktopScaffold(view, policy.locator,
        detail::LocalDesktopBinding::RemoteVisualLowFps);
    if (!observation.bootstrap.IsAccepted())
    {
        observation.erasure = observation.bootstrap.erasure == LocalDesktopErasureReason::UnsupportedRecord ?
            RemoteVisualLowFpsErasure::UnsupportedProfile : RemoteVisualLowFpsErasure::BootstrapErasure;
        return observation;
    }
    const auto parsed = pbprotocol::ParseBootstrapRecord(observation.bootstrap.canonical44);
    if (!parsed || parsed.Value().visualProfileId != kRemoteVisualLowFpsProfileId ||
        parsed.Value().visualLayoutVersion != kRemoteVisualLowFpsLayoutVersion)
    {
        observation.erasure = RemoteVisualLowFpsErasure::UnsupportedProfile;
        return observation;
    }
    observation.erasure = ValidateRemoteVisualLowFpsGeometry(observation.bootstrap.geometry, policy);
    if (observation.erasure != RemoteVisualLowFpsErasure::None)
    {
        return observation;
    }
    // ValidateRemoteVisualLowFpsGeometry already rejects a negative or non-finite origin, so
    // bounding the far corner of the fixed logical canvas keeps every scaled sample in-frame.
    const LocalDesktopGeometry& geometry = observation.bootstrap.geometry;
    const double farCornerX = geometry.originX + geometry.scaleX * kLocalDesktopCanvasWidth;
    const double farCornerY = geometry.originY + geometry.scaleY * kLocalDesktopCanvasHeight;
    if (farCornerX > view.width || farCornerY > view.height)
    {
        observation.erasure = RemoteVisualLowFpsErasure::FrameOutOfBounds;
        return observation;
    }
    if (hardBits.size() < kRemoteVisualLowFpsDataBytes || softMetrics.size() < kRemoteVisualLowFpsCodedBits)
    {
        observation.erasure = RemoteVisualLowFpsErasure::OutputBufferTooSmall;
        return observation;
    }
    detail::LumaReader reader(view, policy.maximumDataWorkUnits, true);
    observation.erasure = Calibrate(reader, observation.bootstrap.geometry, policy, observation.calibration);
    if (observation.erasure == RemoteVisualLowFpsErasure::None)
    {
        scratch.hard.fill(std::byte{0});
        scratch.soft.fill(0);
        scratch.physicalBitMetrics.fill(0);
        scratch.physicalFreshnessMetrics.fill(0);
        scratch.histogram.fill(0);
        const double midpoint = (observation.calibration.centroids[0] + observation.calibration.centroids[1]) * 0.5;
        const double halfSeparation = observation.calibration.separation * 0.5;
        for (std::uint32_t physical = 0; physical < kRemoteVisualTileCount; physical++)
        {
            RemoteVisualTileMapping mapping;
            if (!GetRemoteVisualTileMapping(physical, mapping))
            {
                observation.erasure = RemoteVisualLowFpsErasure::InvalidInput;
                break;
            }
            if (mapping.role == RemoteVisualTileRole::Unused)
            {
                continue;
            }
            LocalDesktopRegion region;
            if (!GetRemoteVisualTile(physical, region))
            {
                observation.erasure = RemoteVisualLowFpsErasure::InvalidInput;
                break;
            }
            std::array<double, 16> chips{};
            bool clipped = false;
            if (!SampleTileChips(reader, observation.bootstrap.geometry, region, chips, clipped))
            {
                observation.erasure = RemoteVisualLowFpsErasure::PixelReadFailure;
                break;
            }
            if (mapping.role == RemoteVisualTileRole::FreshnessTag)
            {
                double mean = 0;
                for (const double chip : chips)
                {
                    mean += chip / static_cast<double>(chips.size());
                }
                const double normalized = (midpoint - mean) / halfSeparation;
                scratch.physicalFreshnessMetrics[physical] = clipped ? 0.0f : static_cast<float>(normalized);
                continue;
            }
            std::array<float, 4> metrics{};
            bool unreliable = clipped;
            double symbolMargin = 0;
            DecodeSymbol(chips, observation.calibration, policy, metrics, unreliable, symbolMargin);
            if (unreliable)
            {
                observation.unreliableSymbols++;
            }
            for (std::uint32_t plane = 0; plane < metrics.size(); plane++)
            {
                scratch.physicalBitMetrics[physical * kRemoteVisualLowFpsBitsPerTile + plane] = metrics[plane];
            }
        }
        if (observation.erasure == RemoteVisualLowFpsErasure::None)
        {
            const RemoteVisualLowFpsMetricResolution resolution = ResolveRemoteVisualLowFpsPhysicalMetrics(
                scratch.physicalBitMetrics, scratch.physicalFreshnessMetrics, parsed.Value().sessionTag.value,
                parsed.Value().frameSequence, scratch.soft, policy.minimumFreshnessMetric);
            if (!resolution.valid)
            {
                // An internal metric-resolution invariant failed. Report the erasure but fall
                // through so the work/pixel accounting below still describes this attempt.
                observation.erasure = RemoteVisualLowFpsErasure::InvalidInput;
            }
            else
            {
                observation.freshnessRegions = resolution.freshnessRegions;
                observation.staleRegions = resolution.staleRegions;
                observation.freshnessTagMismatches = resolution.freshnessTagMismatches;
                observation.freshnessTagErasures = resolution.freshnessTagErasures;
                observation.erasedDataMetrics = resolution.erasedDataMetrics;
                double minimumMargin = 1;
                for (std::size_t logical = 0; logical < scratch.soft.size(); logical++)
                {
                    const float metric = scratch.soft[logical];
                    if (metric < 0)
                    {
                        scratch.hard[logical / 8] |= static_cast<std::byte>(1u << (logical % 8));
                    }
                    const double margin = std::min(1.0, std::abs(static_cast<double>(metric)));
                    const auto bin = static_cast<std::size_t>(margin * static_cast<double>(kRemoteVisualMarginBins - 1));
                    scratch.histogram[std::min(bin, kRemoteVisualMarginBins - 1)]++;
                    minimumMargin = std::min(minimumMargin, margin);
                }
                observation.dataBytes = kRemoteVisualLowFpsDataBytes;
                observation.margin = SummarizeRemoteVisualMargin(scratch.histogram, minimumMargin);
                std::copy(scratch.hard.begin(), scratch.hard.end(), hardBits.begin());
                std::copy(scratch.soft.begin(), scratch.soft.end(), softMetrics.begin());
                scratch.histogramValid = true;
            }
        }
    }
    observation.dataWorkUnits = reader.WorkUnits();
    observation.pixelError = reader.Error();
    if (reader.Error() == LocalDesktopErasureReason::WorkBudgetExceeded)
    {
        observation.erasure = RemoteVisualLowFpsErasure::WorkBudgetExceeded;
    }
    return observation;
}

const char* GetRemoteVisualLowFpsErasureName(const RemoteVisualLowFpsErasure reason) noexcept
{
    switch (reason)
    {
    case RemoteVisualLowFpsErasure::None: return "None";
    case RemoteVisualLowFpsErasure::InvalidInput: return "InvalidInput";
    case RemoteVisualLowFpsErasure::InvalidPolicy: return "InvalidPolicy";
    case RemoteVisualLowFpsErasure::WorkspaceUnavailable: return "WorkspaceUnavailable";
    case RemoteVisualLowFpsErasure::OutputBufferTooSmall: return "OutputBufferTooSmall";
    case RemoteVisualLowFpsErasure::OverlappingSpans: return "OverlappingSpans";
    case RemoteVisualLowFpsErasure::BootstrapErasure: return "BootstrapErasure";
    case RemoteVisualLowFpsErasure::UnsupportedProfile: return "UnsupportedProfile";
    case RemoteVisualLowFpsErasure::ScaleOutOfRange: return "ScaleOutOfRange";
    case RemoteVisualLowFpsErasure::AlignmentOutOfRange: return "AlignmentOutOfRange";
    case RemoteVisualLowFpsErasure::FrameOutOfBounds: return "FrameOutOfBounds";
    case RemoteVisualLowFpsErasure::PilotClipping: return "PilotClipping";
    case RemoteVisualLowFpsErasure::PilotOrder: return "PilotOrder";
    case RemoteVisualLowFpsErasure::PilotVariance: return "PilotVariance";
    case RemoteVisualLowFpsErasure::PilotSpatialMismatch: return "PilotSpatialMismatch";
    case RemoteVisualLowFpsErasure::PixelReadFailure: return "PixelReadFailure";
    case RemoteVisualLowFpsErasure::WorkBudgetExceeded: return "WorkBudgetExceeded";
    default: return "UnknownErasure";
    }
}

} // namespace pbmodulation
