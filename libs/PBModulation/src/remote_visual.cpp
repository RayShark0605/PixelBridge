#include "pbmodulation/remote_visual.h"

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

struct RemoteBand
{
    std::uint32_t y;
    std::uint32_t height;
    bool timingCorridors;
};

inline constexpr std::array<RemoteBand, 7> remoteBands{
    RemoteBand{96, 64, false}, RemoteBand{160, 128, true}, RemoteBand{288, 184, false},
    RemoteBand{476, 128, true}, RemoteBand{608, 184, false}, RemoteBand{792, 128, true},
    RemoteBand{920, 64, false}};

static_assert(kRemoteVisualDataBytes * 8 == kRemoteVisualCodedBits);
static_assert(kRemoteVisualCodewords * 2025 + kRemoteVisualPaddingBytes == kRemoteVisualDataBytes);
static_assert((8 + 23 + 23 + 8) * 216 + 3 * 16 * 168 == kRemoteVisualTileCount);

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

bool ValidPolicy(const RemoteVisualDecodePolicy& policy) noexcept
{
    const std::array<double, 8> values{policy.maximumScaleDriftPixels, policy.maximumPhaseErrorPixels,
        policy.maximumMarkerResidualPixels, policy.minimumEndpointSeparation, policy.maximumPilotStandardDeviation,
        policy.maximumPilotSpatialDeviation, policy.maximumTileStandardDeviation, policy.minimumFreshnessMetric};
    const auto ValidNumber = [](const double value)
    {
        return std::isfinite(value) && value >= 0;
    };
    return std::ranges::all_of(values, ValidNumber) && policy.maximumScaleDriftPixels <= 0.125 &&
        policy.maximumPhaseErrorPixels <= 0.125 && policy.maximumMarkerResidualPixels <= 0.125 &&
        policy.minimumEndpointSeparation >= 64 && policy.minimumEndpointSeparation <= 255 &&
        policy.maximumPilotStandardDeviation <= 32 && policy.maximumPilotSpatialDeviation <= 32 &&
        policy.maximumTileStandardDeviation <= 64 && policy.minimumFreshnessMetric > 0 &&
        policy.minimumFreshnessMetric <= 1 && policy.maximumDataWorkUnits > 0 &&
        policy.maximumDataWorkUnits <= 8000000;
}

constexpr LocalDesktopRegion TileRegion(std::uint32_t physicalIndex) noexcept
{
    for (const auto& band : remoteBands)
    {
        const std::uint32_t rows = band.height / kRemoteVisualTilePixels;
        const std::uint32_t columns = band.timingCorridors ? 168u : 216u;
        const std::uint32_t count = rows * columns;
        if (physicalIndex < count)
        {
            const std::uint32_t row = physicalIndex / columns;
            const std::uint32_t column = physicalIndex % columns;
            std::uint32_t x = 96 + column * kRemoteVisualTilePixels;
            if (band.timingCorridors)
            {
                x = column < 84 ? 224 + column * kRemoteVisualTilePixels :
                    1024 + (column - 84) * kRemoteVisualTilePixels;
            }
            return {x, band.y + row * kRemoteVisualTilePixels, kRemoteVisualTilePixels, kRemoteVisualTilePixels};
        }
        physicalIndex -= count;
    }
    return {};
}

constexpr std::uint16_t FreshnessRegionId(const LocalDesktopRegion region) noexcept
{
    return static_cast<std::uint16_t>(((region.y - 96) / 128) * kRemoteVisualFreshnessRegionColumns +
        (region.x - 96) / 128);
}

constexpr bool IsFreshnessTagCandidate(const LocalDesktopRegion region) noexcept
{
    const std::uint32_t localColumn = ((region.x - 96) / kRemoteVisualTilePixels) % 16;
    const std::uint32_t localRow = ((region.y - 96) / kRemoteVisualTilePixels) % 16;
    const std::uint32_t firstTagColumn = (localRow * 5 + 1) % 16;
    return localColumn == firstTagColumn || localColumn == (firstTagColumn + 8) % 16;
}

struct RemoteVisualManifest
{
    std::array<RemoteVisualTileMapping, kRemoteVisualTileCount> tiles{};
    std::array<std::uint8_t, kRemoteVisualFreshnessRegionCount> tagCounts{};
    std::array<bool, kRemoteVisualFreshnessRegionCount> eligibleRegions{};
    std::uint32_t dataTiles = 0;
    std::uint32_t eligibleRegionCount = 0;
};

RemoteVisualManifest BuildRemoteVisualManifest() noexcept
{
    RemoteVisualManifest manifest;
    for (std::uint32_t physical = 0; physical < kRemoteVisualTileCount; physical++)
    {
        const LocalDesktopRegion region = TileRegion(physical);
        const std::uint16_t regionId = FreshnessRegionId(region);
        manifest.tiles[physical].regionId = regionId;
        if (IsFreshnessTagCandidate(region))
        {
            manifest.tagCounts[regionId]++;
        }
    }
    for (std::uint32_t regionId = 0; regionId < manifest.tagCounts.size(); regionId++)
    {
        if (manifest.tagCounts[regionId] >= kRemoteVisualMinimumFreshnessTags)
        {
            manifest.eligibleRegions[regionId] = true;
            manifest.eligibleRegionCount++;
        }
    }
    for (std::uint32_t physical = 0; physical < kRemoteVisualTileCount; physical++)
    {
        RemoteVisualTileMapping& mapping = manifest.tiles[physical];
        if (!manifest.eligibleRegions[mapping.regionId])
        {
            continue;
        }
        if (IsFreshnessTagCandidate(TileRegion(physical)))
        {
            mapping.role = RemoteVisualTileRole::FreshnessTag;
        }
        else
        {
            mapping.role = RemoteVisualTileRole::Data;
            mapping.dataOrdinal = manifest.dataTiles++;
        }
    }
    return manifest;
}

const RemoteVisualManifest remoteVisualManifest = BuildRemoteVisualManifest();

constexpr std::uint64_t MixFreshness(std::uint64_t value) noexcept
{
    value ^= value >> 30;
    value *= 0xBF58476D1CE4E5B9ULL;
    value ^= value >> 27;
    value *= 0x94D049BB133111EBULL;
    return value ^ (value >> 31);
}

constexpr bool FreshnessBit(const std::uint64_t sessionTag, const std::uint64_t frameSequence,
    const std::uint32_t physicalIndex, const std::uint16_t regionId) noexcept
{
    const std::uint64_t seed = sessionTag ^ std::rotl(frameSequence, 23) ^
        (static_cast<std::uint64_t>(regionId) + 1) * 0x9E3779B97F4A7C15ULL ^
        (static_cast<std::uint64_t>(physicalIndex) + 1) * 0xD6E8FEB86659FD93ULL;
    return (MixFreshness(seed) & 1) != 0;
}

RemoteVisualErasure Calibrate(detail::LumaReader& reader, const std::uint32_t originX, const std::uint32_t originY,
    const RemoteVisualDecodePolicy& policy, RemoteVisualCalibration& calibration) noexcept
{
    constexpr std::uint32_t calibrationWidth = 32 - 2 * kRemoteVisualCalibrationSampleInset;
    constexpr std::uint32_t calibrationHeight = 64 - 2 * kRemoteVisualCalibrationSampleInset;
    constexpr double calibrationSamples = static_cast<double>(calibrationWidth * calibrationHeight);
    for (std::size_t ladder = 0; ladder < kRemoteVisualLadders.size(); ladder++)
    {
        const auto region = kRemoteVisualLadders[ladder];
        for (std::size_t endpoint = 0; endpoint < 2; endpoint++)
        {
            const std::uint32_t level = endpoint == 0 ? 0 : 3;
            double sum = 0;
            double squares = 0;
            for (std::uint32_t row = kRemoteVisualCalibrationSampleInset;
                row < 64 - kRemoteVisualCalibrationSampleInset; row++)
            {
                for (std::uint32_t column = kRemoteVisualCalibrationSampleInset;
                    column < 32 - kRemoteVisualCalibrationSampleInset; column++)
                {
                    double sample = 0;
                    bool clipped = false;
                    if (!reader.Pixel(originX + region.x + level * 32 + column, originY + region.y + row, sample, &clipped))
                    {
                        return RemoteVisualErasure::PixelReadFailure;
                    }
                    if (clipped)
                    {
                        return RemoteVisualErasure::PilotClipping;
                    }
                    sum += sample;
                    squares += sample * sample;
                }
            }
            const double mean = sum / calibrationSamples;
            const double variance = std::max(0.0, squares / calibrationSamples - mean * mean);
            if (variance > policy.maximumPilotStandardDeviation * policy.maximumPilotStandardDeviation)
            {
                return RemoteVisualErasure::PilotVariance;
            }
            calibration.ladderCentroids[ladder][endpoint] = mean;
            calibration.ladderVariances[ladder][endpoint] = variance;
            calibration.centroids[endpoint] += mean / static_cast<double>(kRemoteVisualLadders.size());
            calibration.variances[endpoint] += variance / static_cast<double>(kRemoteVisualLadders.size());
        }
        if (calibration.ladderCentroids[ladder][1] - calibration.ladderCentroids[ladder][0] < policy.minimumEndpointSeparation)
        {
            return RemoteVisualErasure::PilotOrder;
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
        return RemoteVisualErasure::PilotSpatialMismatch;
    }
    return calibration.separation < policy.minimumEndpointSeparation ? RemoteVisualErasure::PilotOrder : RemoteVisualErasure::None;
}

} // namespace

struct RemoteVisualWorkspace::Implementation
{
    std::array<std::byte, kRemoteVisualDataBytes> hard{};
    std::array<float, kRemoteVisualCodedBits> soft{};
    std::array<float, kRemoteVisualTileCount> physicalMetrics{};
    std::array<std::uint64_t, kRemoteVisualMarginBins> histogram{};
    bool histogramValid = false;
};

RemoteVisualWorkspace::RemoteVisualWorkspace() noexcept = default;
RemoteVisualWorkspace::RemoteVisualWorkspace(RemoteVisualWorkspace&&) noexcept = default;
RemoteVisualWorkspace& RemoteVisualWorkspace::operator=(RemoteVisualWorkspace&&) noexcept = default;
RemoteVisualWorkspace::~RemoteVisualWorkspace() = default;

std::uint64_t RemoteVisualWorkspace::RequiredBytes() noexcept
{
    return sizeof(Implementation);
}

ModulationResult<RemoteVisualWorkspace> RemoteVisualWorkspace::Create(const std::uint64_t maximumBytes) noexcept
{
    if (maximumBytes < RequiredBytes())
    {
        return ModulationResult<RemoteVisualWorkspace>::Failure(ModulationErrorCode::InvalidInput, 0);
    }
    try
    {
        RemoteVisualWorkspace workspace;
        workspace.implementation_ = std::make_unique<Implementation>();
        return ModulationResult<RemoteVisualWorkspace>::Success(std::move(workspace));
    }
    catch (const std::bad_alloc&)
    {
        return ModulationResult<RemoteVisualWorkspace>::Failure(ModulationErrorCode::MemoryAllocationFailure, 0);
    }
}

std::span<const std::uint64_t> RemoteVisualWorkspace::GetMarginHistogram() const noexcept
{
    return implementation_ && implementation_->histogramValid ? std::span<const std::uint64_t>(implementation_->histogram) :
        std::span<const std::uint64_t>{};
}

bool GetRemoteVisualTile(const std::uint32_t physicalIndex, LocalDesktopRegion& output) noexcept
{
    if (physicalIndex >= kRemoteVisualTileCount)
    {
        return false;
    }
    output = TileRegion(physicalIndex);
    return true;
}

bool GetRemoteVisualTileMapping(const std::uint32_t physicalIndex, RemoteVisualTileMapping& output) noexcept
{
    if (physicalIndex >= kRemoteVisualTileCount)
    {
        return false;
    }
    output = remoteVisualManifest.tiles[physicalIndex];
    return true;
}

bool GetRemoteVisualFreshnessBit(const std::uint64_t sessionTag, const std::uint64_t frameSequence,
    const std::uint32_t physicalIndex, bool& output) noexcept
{
    if (physicalIndex >= kRemoteVisualTileCount ||
        remoteVisualManifest.tiles[physicalIndex].role != RemoteVisualTileRole::FreshnessTag)
    {
        return false;
    }
    const RemoteVisualTileMapping mapping = remoteVisualManifest.tiles[physicalIndex];
    output = FreshnessBit(sessionTag, frameSequence, physicalIndex, mapping.regionId);
    return true;
}

RemoteVisualMetricResolution ResolveRemoteVisualPhysicalMetrics(const std::span<const float> physicalMetrics,
    const std::uint64_t sessionTag, const std::uint64_t frameSequence, const std::span<float> logicalMetrics,
    const double minimumFreshnessMetric) noexcept
{
    RemoteVisualMetricResolution result;
    if (physicalMetrics.size() < kRemoteVisualTileCount || logicalMetrics.size() < kRemoteVisualCodedBits ||
        physicalMetrics.data() == nullptr || logicalMetrics.data() == nullptr || !std::isfinite(minimumFreshnessMetric) ||
        minimumFreshnessMetric <= 0 || minimumFreshnessMetric > 1 ||
        Overlap(std::as_bytes(physicalMetrics), std::as_bytes(logicalMetrics)) ||
        !std::ranges::all_of(physicalMetrics.first(kRemoteVisualTileCount),
            [](const float value) { return std::isfinite(value); }))
    {
        return result;
    }
    std::array<bool, kRemoteVisualFreshnessRegionCount> regionFresh = remoteVisualManifest.eligibleRegions;
    result.freshnessRegions = remoteVisualManifest.eligibleRegionCount;
    for (std::uint32_t physical = 0; physical < kRemoteVisualTileCount; physical++)
    {
        const RemoteVisualTileMapping mapping = remoteVisualManifest.tiles[physical];
        if (mapping.role != RemoteVisualTileRole::FreshnessTag)
        {
            continue;
        }
        const float metric = physicalMetrics[physical];
        const bool expectedOne = FreshnessBit(sessionTag, frameSequence, physical, mapping.regionId);
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
    for (std::uint32_t regionId = 0; regionId < remoteVisualManifest.eligibleRegions.size(); regionId++)
    {
        result.staleRegions += static_cast<std::uint32_t>(remoteVisualManifest.eligibleRegions[regionId] &&
            !regionFresh[regionId]);
    }
    std::fill_n(logicalMetrics.begin(), kRemoteVisualCodedBits, 0.0f);
    for (std::uint32_t physical = 0; physical < kRemoteVisualTileCount; physical++)
    {
        const RemoteVisualTileMapping mapping = remoteVisualManifest.tiles[physical];
        if (mapping.role != RemoteVisualTileRole::Data)
        {
            continue;
        }
        const std::uint32_t logical = pbinterleave::kRemoteVisualPermutation.ToLogical(mapping.dataOrdinal,
            frameSequence);
        if (logical >= kRemoteVisualCodedBits)
        {
            continue;
        }
        if (regionFresh[mapping.regionId])
        {
            logicalMetrics[logical] = physicalMetrics[physical];
        }
        else
        {
            result.erasedDataMetrics++;
        }
    }
    result.valid = true;
    return result;
}

RemoteVisualErasure ValidateRemoteVisualGeometry(const LocalDesktopGeometry& geometry, const RemoteVisualDecodePolicy& policy) noexcept
{
    if (!ValidPolicy(policy))
    {
        return RemoteVisualErasure::InvalidPolicy;
    }
    const std::array<double, 5> values{geometry.originX, geometry.originY, geometry.scaleX, geometry.scaleY,
        geometry.markerResidualPixels};
    if (!std::ranges::all_of(values, [](const double value) { return std::isfinite(value); }) || geometry.markerResidualPixels < 0)
    {
        return RemoteVisualErasure::InvalidInput;
    }
    if (std::abs(geometry.scaleX - 1) * 1920 > policy.maximumScaleDriftPixels ||
        std::abs(geometry.scaleY - 1) * 1080 > policy.maximumScaleDriftPixels)
    {
        return RemoteVisualErasure::ScaleOutOfRange;
    }
    if (std::abs(geometry.originX - std::round(geometry.originX)) > policy.maximumPhaseErrorPixels ||
        std::abs(geometry.originY - std::round(geometry.originY)) > policy.maximumPhaseErrorPixels ||
        geometry.markerResidualPixels > policy.maximumMarkerResidualPixels)
    {
        return RemoteVisualErasure::AlignmentOutOfRange;
    }
    return RemoteVisualErasure::None;
}

RemoteVisualMargin SummarizeRemoteVisualMargin(const std::span<const std::uint64_t> histogram, const double minimum) noexcept
{
    RemoteVisualMargin result;
    if (histogram.size() != kRemoteVisualMarginBins || histogram.data() == nullptr || !std::isfinite(minimum) || minimum < 0 || minimum > 1)
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
                return static_cast<double>(index) / static_cast<double>(kRemoteVisualMarginBins - 1);
            }
        }
        return 1.0;
    };
    result.p50 = Quantile(2);
    result.p01 = Quantile(100);
    result.p001 = Quantile(1000);
    return result;
}

ModulationStatus EncodeRemoteVisualFrame(const std::span<const std::byte> bootstrapRecord,
    const std::span<const std::byte> logicalData, const std::span<std::byte> outBgra) noexcept
{
    if (bootstrapRecord.data() == nullptr || logicalData.data() == nullptr || outBgra.data() == nullptr ||
        logicalData.size() != kRemoteVisualDataBytes || Overlap(logicalData, outBgra) || Overlap(bootstrapRecord, outBgra))
    {
        return ModulationStatus::Failure(ModulationErrorCode::InvalidInput, 0);
    }
    const auto parsed = pbprotocol::ParseBootstrapRecord(bootstrapRecord);
    if (!parsed)
    {
        return ModulationStatus::Failure(parsed.Error().code == pbprotocol::ProtocolErrorCode::CrcMismatch ?
            ModulationErrorCode::CrcMismatch : ModulationErrorCode::InvalidInput, parsed.Error().offset);
    }
    if (parsed.Value().visualProfileId != kRemoteVisualProfileId ||
        parsed.Value().visualLayoutVersion != kRemoteVisualLayoutVersion)
    {
        return ModulationStatus::Failure(ModulationErrorCode::InvalidInput, 0);
    }
    const auto scaffold = detail::EncodeLocalDesktopScaffold(bootstrapRecord, outBgra, detail::LocalDesktopBinding::RemoteVisual);
    if (!scaffold)
    {
        return scaffold;
    }
    for (const auto region : kRemoteVisualLadders)
    {
        for (std::uint32_t level = 0; level < 4; level++)
        {
            detail::FillLocalDesktopBlock(outBgra, {region.x + level * 32, region.y, 32, 64},
                static_cast<std::uint8_t>(32 + level * 64));
        }
    }
    for (std::uint32_t physical = 0; physical < kRemoteVisualTileCount; physical++)
    {
        const RemoteVisualTileMapping mapping = remoteVisualManifest.tiles[physical];
        std::uint8_t level = kRemoteVisualUnusedLuma;
        if (mapping.role == RemoteVisualTileRole::FreshnessTag)
        {
            level = FreshnessBit(parsed.Value().sessionTag.value, parsed.Value().frameSequence, physical,
                mapping.regionId) ? kRemoteVisualOneLuma : kRemoteVisualZeroLuma;
        }
        else if (mapping.role == RemoteVisualTileRole::Data)
        {
            const std::uint32_t logical = pbinterleave::kRemoteVisualPermutation.ToLogical(mapping.dataOrdinal,
                parsed.Value().frameSequence);
            if (logical < kRemoteVisualCodedBits)
            {
                const bool one = (std::to_integer<unsigned>(logicalData[logical / 8]) & (1u << (logical % 8))) != 0;
                level = one ? kRemoteVisualOneLuma : kRemoteVisualZeroLuma;
            }
        }
        detail::FillLocalDesktopBlock(outBgra, TileRegion(physical), level);
    }
    return ModulationStatus::Success();
}

RemoteVisualObservation DecodeRemoteVisualFrame(const LumaView& view, RemoteVisualWorkspace& workspace,
    const std::span<std::byte> hardBits, const std::span<float> softMetrics, const RemoteVisualDecodePolicy& policy) noexcept
{
    RemoteVisualObservation observation;
    if (!workspace.implementation_)
    {
        observation.erasure = RemoteVisualErasure::WorkspaceUnavailable;
        return observation;
    }
    auto& scratch = *workspace.implementation_;
    scratch.histogramValid = false;
    if (!ValidPolicy(policy))
    {
        observation.erasure = RemoteVisualErasure::InvalidPolicy;
        return observation;
    }
    observation.bootstrap.erasure = ValidateLumaView(view);
    if (observation.bootstrap.erasure != LocalDesktopErasureReason::None)
    {
        observation.erasure = RemoteVisualErasure::InvalidInput;
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
        observation.erasure = RemoteVisualErasure::OverlappingSpans;
        return observation;
    }
    observation.bootstrap = detail::DecodeLocalDesktopScaffold(view, policy.locator, detail::LocalDesktopBinding::RemoteVisual);
    if (!observation.bootstrap.IsAccepted())
    {
        observation.erasure = observation.bootstrap.erasure == LocalDesktopErasureReason::UnsupportedRecord ?
            RemoteVisualErasure::UnsupportedProfile : RemoteVisualErasure::BootstrapErasure;
        return observation;
    }
    const auto parsed = pbprotocol::ParseBootstrapRecord(observation.bootstrap.canonical44);
    if (!parsed || parsed.Value().visualProfileId != kRemoteVisualProfileId ||
        parsed.Value().visualLayoutVersion != kRemoteVisualLayoutVersion)
    {
        observation.erasure = RemoteVisualErasure::UnsupportedProfile;
        return observation;
    }
    observation.erasure = ValidateRemoteVisualGeometry(observation.bootstrap.geometry, policy);
    if (observation.erasure != RemoteVisualErasure::None)
    {
        return observation;
    }
    const double roundedX = std::round(observation.bootstrap.geometry.originX);
    const double roundedY = std::round(observation.bootstrap.geometry.originY);
    if (roundedX < 0 || roundedY < 0 || roundedX + 1920 > view.width || roundedY + 1080 > view.height)
    {
        observation.erasure = RemoteVisualErasure::FrameOutOfBounds;
        return observation;
    }
    if (hardBits.size() < kRemoteVisualDataBytes || softMetrics.size() < kRemoteVisualCodedBits)
    {
        observation.erasure = RemoteVisualErasure::OutputBufferTooSmall;
        return observation;
    }
    const auto originX = static_cast<std::uint32_t>(roundedX);
    const auto originY = static_cast<std::uint32_t>(roundedY);
    detail::LumaReader reader(view, policy.maximumDataWorkUnits, true);
    observation.erasure = Calibrate(reader, originX, originY, policy, observation.calibration);
    if (observation.erasure == RemoteVisualErasure::None)
    {
        scratch.hard.fill(std::byte{0});
        scratch.soft.fill(0);
        scratch.physicalMetrics.fill(0);
        scratch.histogram.fill(0);
        const double separationSquared = observation.calibration.separation * observation.calibration.separation;
        const double maximumVariance = policy.maximumTileStandardDeviation * policy.maximumTileStandardDeviation;
        constexpr std::uint32_t sampleWidth = kRemoteVisualTilePixels - 2 * kRemoteVisualTileSampleInset;
        constexpr double tileSamples = static_cast<double>(sampleWidth * sampleWidth);
        for (std::uint32_t physical = 0; physical < kRemoteVisualTileCount; physical++)
        {
            const auto region = TileRegion(physical);
            double sum = 0;
            double squares = 0;
            bool clipped = false;
            for (std::uint32_t row = kRemoteVisualTileSampleInset;
                row < kRemoteVisualTilePixels - kRemoteVisualTileSampleInset; row++)
            {
                for (std::uint32_t column = kRemoteVisualTileSampleInset;
                    column < kRemoteVisualTilePixels - kRemoteVisualTileSampleInset; column++)
                {
                    double sample = 0;
                    bool sampleClipped = false;
                    if (!reader.Pixel(originX + region.x + column, originY + region.y + row, sample, &sampleClipped))
                    {
                        observation.erasure = RemoteVisualErasure::PixelReadFailure;
                        break;
                    }
                    sum += sample;
                    squares += sample * sample;
                    clipped = clipped || sampleClipped;
                }
                if (observation.erasure != RemoteVisualErasure::None)
                {
                    break;
                }
            }
            if (observation.erasure != RemoteVisualErasure::None)
            {
                break;
            }
            if (!reader.Charge(2))
            {
                observation.erasure = RemoteVisualErasure::PixelReadFailure;
                break;
            }
            const double mean = sum / tileSamples;
            const double variance = std::max(0.0, squares / tileSamples - mean * mean);
            const double zeroDifference = mean - observation.calibration.centroids[0];
            const double oneDifference = mean - observation.calibration.centroids[1];
            const double metric = (oneDifference * oneDifference - zeroDifference * zeroDifference) / separationSquared;
            const bool unreliable = clipped || variance > maximumVariance;
            scratch.physicalMetrics[physical] = unreliable ? 0.0f : static_cast<float>(metric);
        }
        if (observation.erasure == RemoteVisualErasure::None)
        {
            const RemoteVisualMetricResolution resolution = ResolveRemoteVisualPhysicalMetrics(scratch.physicalMetrics,
                parsed.Value().sessionTag.value, parsed.Value().frameSequence, scratch.soft,
                policy.minimumFreshnessMetric);
            if (!resolution.valid)
            {
                observation.erasure = RemoteVisualErasure::InvalidInput;
                return observation;
            }
            observation.freshnessRegions = resolution.freshnessRegions;
            observation.staleRegions = resolution.staleRegions;
            observation.freshnessTagMismatches = resolution.freshnessTagMismatches;
            observation.freshnessTagErasures = resolution.freshnessTagErasures;
            double minimumMargin = 1;
            for (std::size_t logical = 0; logical < scratch.soft.size(); logical++)
            {
                const float metric = scratch.soft[logical];
                if (metric < 0)
                {
                    scratch.hard[logical / 8] |= static_cast<std::byte>(1u << (logical % 8));
                }
                const double margin = std::min(1.0, std::abs(static_cast<double>(metric)));
                observation.unreliableTiles += static_cast<std::uint32_t>(metric == 0);
                const auto bin = static_cast<std::size_t>(margin * static_cast<double>(kRemoteVisualMarginBins - 1));
                scratch.histogram[std::min(bin, kRemoteVisualMarginBins - 1)]++;
                minimumMargin = std::min(minimumMargin, margin);
            }
            observation.dataBytes = kRemoteVisualDataBytes;
            observation.margin = SummarizeRemoteVisualMargin(scratch.histogram, minimumMargin);
            std::copy(scratch.hard.begin(), scratch.hard.end(), hardBits.begin());
            std::copy(scratch.soft.begin(), scratch.soft.end(), softMetrics.begin());
            scratch.histogramValid = true;
        }
    }
    observation.dataWorkUnits = reader.WorkUnits();
    observation.pixelError = reader.Error();
    if (reader.Error() == LocalDesktopErasureReason::WorkBudgetExceeded)
    {
        observation.erasure = RemoteVisualErasure::WorkBudgetExceeded;
    }
    return observation;
}

const char* GetRemoteVisualErasureName(const RemoteVisualErasure reason) noexcept
{
    switch (reason)
    {
    case RemoteVisualErasure::None: return "None";
    case RemoteVisualErasure::InvalidInput: return "InvalidInput";
    case RemoteVisualErasure::InvalidPolicy: return "InvalidPolicy";
    case RemoteVisualErasure::WorkspaceUnavailable: return "WorkspaceUnavailable";
    case RemoteVisualErasure::OutputBufferTooSmall: return "OutputBufferTooSmall";
    case RemoteVisualErasure::OverlappingSpans: return "OverlappingSpans";
    case RemoteVisualErasure::BootstrapErasure: return "BootstrapErasure";
    case RemoteVisualErasure::UnsupportedProfile: return "UnsupportedProfile";
    case RemoteVisualErasure::ScaleOutOfRange: return "ScaleOutOfRange";
    case RemoteVisualErasure::AlignmentOutOfRange: return "AlignmentOutOfRange";
    case RemoteVisualErasure::FrameOutOfBounds: return "FrameOutOfBounds";
    case RemoteVisualErasure::PilotClipping: return "PilotClipping";
    case RemoteVisualErasure::PilotOrder: return "PilotOrder";
    case RemoteVisualErasure::PilotVariance: return "PilotVariance";
    case RemoteVisualErasure::PilotSpatialMismatch: return "PilotSpatialMismatch";
    case RemoteVisualErasure::PixelReadFailure: return "PixelReadFailure";
    case RemoteVisualErasure::WorkBudgetExceeded: return "WorkBudgetExceeded";
    default: return "UnknownErasure";
    }
}

} // namespace pbmodulation
