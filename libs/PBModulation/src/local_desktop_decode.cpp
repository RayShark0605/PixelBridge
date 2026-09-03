#include "pbmodulation/local_desktop_decode.h"

#include "local_desktop_internal.h"
#include "luma_reader.h"
#include "pbprotocol/bootstrap_control_codec.h"
#include "pbprotocol/crc32c.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace pbmodulation
{
namespace
{

// These are bounded locator mechanics, not alternative acceptance thresholds.
// All final contrast/residual/ambiguity gates are in LocalDesktopDecodePolicy.
struct LocatorLimits
{
    static constexpr std::uint32_t scanLineStep = 2;
    static constexpr std::size_t markerCapacity = 64;
    static constexpr std::size_t geometryCapacity = 32;
    static constexpr std::uint32_t crossSearchPixels = 128;
    static constexpr double crossRelativeTolerance = 0.30;
    static constexpr double crossPixelTolerance = 1.25;
    static constexpr double seedScaleTolerance = 0.20;
    static constexpr double moduleScaleTolerance = 0.18;
    static constexpr double duplicateCentrePixels = 1.5;
    static constexpr std::uint32_t maximumRoleBitErrors = 1;
    static constexpr double quietSeedMinimum = 0.65;
    static constexpr double edgeSearchRadius = 1.5;
    static constexpr double edgeSearchStep = 0.5;
    static constexpr double refinementConvergence = kLocalDesktopGeometryRefinementConvergencePixels;
    static constexpr double scaleRoundoffTolerance = 1.0e-9;
    static constexpr std::uint64_t rsWorkUnits = 50000;
    static constexpr std::uint64_t timingWorkUnits = 1024;
};

using Erasure = LocalDesktopErasureReason;

using detail::BytesPerPixel;
using detail::Read32;
using detail::LumaReader;

} // namespace

bool ValidateLocalDesktopDecodePolicy(const LocalDesktopDecodePolicy& policy) noexcept
{
    constexpr LocalDesktopDecodePolicy defaults;
    const std::array<double, 11> values{policy.minimumScale, policy.maximumScale, policy.minimumContrast,
        policy.maximumMarkerResidual, policy.maximumBootstrapResidual, policy.maximumTimingResidual,
        policy.maximumTimingBitErrorFraction, policy.maximumMidGrayFraction, policy.midGrayBoundary,
        policy.maximumGeometryResidualPixels, static_cast<double>(policy.maximumWorkUnits)};
    if (!std::ranges::all_of(values, [](const double value)
    {
        return std::isfinite(value) && value >= 0;
    }))
    {
        return false;
    }
    return policy.maximumWorkUnits > 0 && policy.maximumWorkUnits <= defaults.maximumWorkUnits &&
           policy.maximumMarkers > 0 && policy.maximumMarkers <= defaults.maximumMarkers &&
           policy.maximumGeometries > 0 && policy.maximumGeometries <= defaults.maximumGeometries &&
           policy.maximumRefinementIterations > 0 && policy.maximumRefinementIterations <= defaults.maximumRefinementIterations &&
           policy.minimumScale >= defaults.minimumScale && policy.maximumScale <= defaults.maximumScale && policy.minimumScale <= policy.maximumScale &&
           policy.minimumContrast >= defaults.minimumContrast && policy.minimumContrast <= 255 &&
           policy.maximumMarkerResidual <= defaults.maximumMarkerResidual && policy.maximumBootstrapResidual <= defaults.maximumBootstrapResidual &&
           policy.maximumTimingResidual <= defaults.maximumTimingResidual && policy.maximumTimingBitErrorFraction <= defaults.maximumTimingBitErrorFraction &&
           policy.maximumMidGrayFraction <= defaults.maximumMidGrayFraction && policy.midGrayBoundary <= defaults.midGrayBoundary &&
           policy.maximumGeometryResidualPixels > 0 && policy.maximumGeometryResidualPixels <= defaults.maximumGeometryResidualPixels;
}

namespace
{

struct Run
{
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    bool black = false;
};

struct Marker
{
    std::size_t role = 0;
    double centreX = 0;
    double centreY = 0;
    double scaleX = 0;
    double scaleY = 0;
    double blackLevel = 0;
    double whiteLevel = 0;
    double residual = 0;
};

struct MarkerSet
{
    std::array<Marker, LocatorLimits::markerCapacity> markers{};
    std::size_t size = 0;
    bool lowContrast = false;
};

bool ValidCross(const std::array<std::uint32_t, 5>& lengths, const LocalDesktopDecodePolicy& policy, double& scale) noexcept
{
    std::uint64_t total = 0;
    for (const auto length : lengths)
    {
        total += length;
    }
    const double module = static_cast<double>(total) / 7.0;
    if (module < kLocalDesktopCellPixels * policy.minimumScale - LocatorLimits::seedScaleTolerance * kLocalDesktopCellPixels ||
        module > kLocalDesktopCellPixels * policy.maximumScale + LocatorLimits::seedScaleTolerance * kLocalDesktopCellPixels)
    {
        return false;
    }
    for (std::size_t index = 0; index < lengths.size(); index++)
    {
        const double expected = module * (index == 2 ? 3.0 : 1.0);
        const double tolerance = std::max(LocatorLimits::crossPixelTolerance, expected * LocatorLimits::crossRelativeTolerance);
        if (std::abs(static_cast<double>(lengths[index]) - expected) > tolerance)
        {
            return false;
        }
    }
    scale = module / kLocalDesktopCellPixels;
    return true;
}

bool VerticalCross(LumaReader& reader, const std::uint32_t column, const std::uint32_t seedRow,
                   const LocalDesktopDecodePolicy& policy, const double midpoint, double& centre, double& scale) noexcept
{
    std::array<std::uint32_t, 5> lengths{};
    std::int64_t upper = seedRow;
    std::int64_t lower = static_cast<std::int64_t>(seedRow) + 1;
    for (unsigned direction = 0; direction < 2; direction++)
    {
        std::int64_t& position = direction == 0 ? upper : lower;
        std::size_t section = 0;
        for (std::uint32_t distance = 0; distance < LocatorLimits::crossSearchPixels; distance++)
        {
            if (position < 0 || position >= reader.Height())
            {
                return false;
            }
            double level = 0;
            if (!reader.Pixel(column, static_cast<std::uint32_t>(position), level))
            {
                return false;
            }
            const bool black = level < midpoint;
            if (black != (section != 1))
            {
                section++;
                if (section == 3)
                {
                    break;
                }
            }
            const std::size_t index = direction == 0 ? 2 - section : 2 + section;
            lengths[index]++;
            position += direction == 0 ? -1 : 1;
            if (distance + 1 == LocatorLimits::crossSearchPixels)
            {
                return false;
            }
        }
    }
    if (!ValidCross(lengths, policy, scale))
    {
        return false;
    }
    centre = (static_cast<double>(upper + 1) + static_cast<double>(lower)) * 0.5;
    return true;
}

bool VerifyMarker(LumaReader& reader, const LocalDesktopDecodePolicy& policy, Marker& marker, bool& lowContrast) noexcept
{
    std::array<double, kLocalDesktopMarkerModules * kLocalDesktopMarkerModules> samples{};
    for (std::uint32_t row = 0; row < kLocalDesktopMarkerModules; row++)
    {
        for (std::uint32_t column = 0; column < kLocalDesktopMarkerModules; column++)
        {
            const double x = marker.centreX + (static_cast<double>(column) - 3.0) * kLocalDesktopCellPixels * marker.scaleX - 0.5;
            const double y = marker.centreY + (static_cast<double>(row) - 3.0) * kLocalDesktopCellPixels * marker.scaleY - 0.5;
            if (!reader.Contains(x, y) || !reader.Sample(x, y, samples[row * kLocalDesktopMarkerModules + column]))
            {
                return false;
            }
        }
    }
    std::size_t matchingRoles = 0;
    Marker accepted = marker;
    for (std::size_t role = 0; role < kLocalDesktopMarkerRegions.size(); role++)
    {
        double blackTotal = 0;
        double whiteTotal = 0;
        unsigned blackCount = 0;
        unsigned whiteCount = 0;
        for (std::uint32_t row = 0; row < kLocalDesktopMarkerModules; row++)
        {
            for (std::uint32_t column = 0; column < kLocalDesktopMarkerModules; column++)
            {
                const double value = samples[row * kLocalDesktopMarkerModules + column];
                if (detail::MarkerModule(role, column, row) == 0)
                {
                    blackTotal += value;
                    blackCount++;
                }
                else
                {
                    whiteTotal += value;
                    whiteCount++;
                }
            }
        }
        const double black = blackTotal / blackCount;
        const double white = whiteTotal / whiteCount;
        const double contrast = white - black;
        if (contrast < policy.minimumContrast)
        {
            // Merely report a plausible contrast failure; this is not a marker.
            lowContrast = true;
            continue;
        }
        double residual = 0;
        unsigned bitErrors = 0;
        for (std::uint32_t row = 0; row < kLocalDesktopMarkerModules; row++)
        {
            for (std::uint32_t column = 0; column < kLocalDesktopMarkerModules; column++)
            {
                const auto expected = detail::MarkerModule(role, column, row);
                const double normalized = (samples[row * kLocalDesktopMarkerModules + column] - black) / contrast;
                residual += std::abs(normalized - expected);
                bitErrors += static_cast<unsigned>((normalized >= 0.5) != (expected != 0));
            }
        }
        residual /= static_cast<double>(samples.size());
        if (residual <= policy.maximumMarkerResidual && bitErrors <= LocatorLimits::maximumRoleBitErrors)
        {
            matchingRoles++;
            accepted.role = role;
            accepted.blackLevel = black;
            accepted.whiteLevel = white;
            accepted.residual = residual;
        }
    }
    if (matchingRoles != 1)
    {
        return false;
    }
    constexpr std::array<std::array<double, 2>, 4> quietPoints{{{-30, 0}, {30, 0}, {0, -30}, {0, 30}}};
    for (const auto& point : quietPoints)
    {
        const double x = marker.centreX + point[0] * marker.scaleX - 0.5;
        const double y = marker.centreY + point[1] * marker.scaleY - 0.5;
        double value = 0;
        if (!reader.Contains(x, y) || !reader.Sample(x, y, value) ||
            (value - accepted.blackLevel) / (accepted.whiteLevel - accepted.blackLevel) < LocatorLimits::quietSeedMinimum)
        {
            return false;
        }
    }
    marker = accepted;
    return true;
}

Erasure ScanMarkers(LumaReader& reader, const LocalDesktopDecodePolicy& policy, MarkerSet& output, const double midpoint) noexcept
{
    for (std::uint64_t row = 0; row < reader.Height(); row += LocatorLimits::scanLineStep)
    {
        std::array<Run, 5> runs{};
        std::size_t runCount = 0;
        std::uint32_t runBegin = 0;
        double first = 0;
        if (!reader.Pixel(0, static_cast<std::uint32_t>(row), first))
        {
            return reader.Error();
        }
        bool runBlack = first < midpoint;
        for (std::uint64_t column = 1; column <= reader.Width(); column++)
        {
            double value = 0;
            if (column < reader.Width() && !reader.Pixel(static_cast<std::uint32_t>(column), static_cast<std::uint32_t>(row), value))
            {
                return reader.Error();
            }
            const bool black = column < reader.Width() ? value < midpoint : !runBlack;
            if (black == runBlack)
            {
                continue;
            }
            if (runCount == runs.size())
            {
                for (std::size_t index = 1; index < runs.size(); index++)
                {
                    runs[index - 1] = runs[index];
                }
                runCount--;
            }
            runs[runCount++] = {runBegin, static_cast<std::uint32_t>(column), runBlack};
            runBegin = static_cast<std::uint32_t>(column);
            runBlack = black;
            if (runCount != runs.size() || !runs[0].black)
            {
                continue;
            }
            std::array<std::uint32_t, 5> lengths{};
            for (std::size_t index = 0; index < lengths.size(); index++)
            {
                lengths[index] = runs[index].end - runs[index].begin;
            }
            Marker marker;
            if (!ValidCross(lengths, policy, marker.scaleX))
            {
                continue;
            }
            marker.centreX = (static_cast<double>(runs[2].begin) + runs[2].end) * 0.5;
            const auto crossColumn = static_cast<std::uint32_t>(std::floor(marker.centreX));
            if (!VerticalCross(reader, crossColumn, static_cast<std::uint32_t>(row), policy, midpoint, marker.centreY, marker.scaleY))
            {
                if (reader.Error() != Erasure::None)
                {
                    return reader.Error();
                }
                continue;
            }
            bool duplicate = false;
            for (std::size_t index = 0; index < output.size; index++)
            {
                if (std::abs(output.markers[index].centreX - marker.centreX) <= LocatorLimits::duplicateCentrePixels &&
                    std::abs(output.markers[index].centreY - marker.centreY) <= LocatorLimits::duplicateCentrePixels)
                {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate || !VerifyMarker(reader, policy, marker, output.lowContrast))
            {
                if (reader.Error() != Erasure::None)
                {
                    return reader.Error();
                }
                continue;
            }
            if (output.size == policy.maximumMarkers)
            {
                return Erasure::MarkerBudgetExceeded;
            }
            output.markers[output.size++] = marker;
        }
    }
    return Erasure::None;
}

Erasure LocateMarkers(LumaReader& reader, const LocalDesktopDecodePolicy& policy, MarkerSet& output) noexcept
{
    // A current-frame contrast of 96 need not straddle canonical midpoint 128.
    // These finite proposals cover the permitted 0..255 luma range; horizontal
    // and vertical cross checks use the SAME proposal. All passes share one
    // candidate set and total work budget. Never stop at the first four roles:
    // another valid frame may only be visible under a different threshold.
    constexpr std::array<double, 3> thresholds{128, 64, 192};
    for (const auto threshold : thresholds)
    {
        const auto status = ScanMarkers(reader, policy, output, threshold);
        if (status != Erasure::None)
        {
            return status;
        }
    }
    if (output.size == 0)
    {
        return output.lowContrast ? Erasure::LowContrast : Erasure::MarkersNotFound;
    }
    std::array<bool, 4> roles{};
    for (std::size_t index = 0; index < output.size; index++)
    {
        roles[output.markers[index].role] = true;
    }
    return std::ranges::all_of(roles, [](const bool present)
    {
        return present;
    }) ? Erasure::None : Erasure::IncompleteMarkers;
}

double LogicalCentreX(const std::size_t role) noexcept
{
    return kLocalDesktopMarkerRegions[role].x + kLocalDesktopMarkerRegions[role].width * 0.5;
}

double LogicalCentreY(const std::size_t role) noexcept
{
    return kLocalDesktopMarkerRegions[role].y + kLocalDesktopMarkerRegions[role].height * 0.5;
}

bool WithinScale(const double value, const LocalDesktopDecodePolicy& policy) noexcept
{
    return value >= policy.minimumScale - LocatorLimits::scaleRoundoffTolerance && value <= policy.maximumScale + LocatorLimits::scaleRoundoffTolerance;
}

bool MakeGeometry(const std::array<const Marker*, 4>& markers, const LocalDesktopDecodePolicy& policy, LocalDesktopGeometry& geometry) noexcept
{
    const double left = (markers[0]->centreX + markers[2]->centreX) * 0.5;
    const double right = (markers[1]->centreX + markers[3]->centreX) * 0.5;
    const double top = (markers[0]->centreY + markers[1]->centreY) * 0.5;
    const double bottom = (markers[2]->centreY + markers[3]->centreY) * 0.5;
    geometry.scaleX = (right - left) / (LogicalCentreX(1) - LogicalCentreX(0));
    geometry.scaleY = (bottom - top) / (LogicalCentreY(2) - LogicalCentreY(0));
    if (!WithinScale(geometry.scaleX, policy) || !WithinScale(geometry.scaleY, policy))
    {
        return false;
    }
    geometry.originX = left - geometry.scaleX * LogicalCentreX(0);
    geometry.originY = top - geometry.scaleY * LogicalCentreY(0);
    double maximumResidual = 0;
    for (std::size_t role = 0; role < markers.size(); role++)
    {
        const double horizontal = markers[role]->centreX - (geometry.originX + geometry.scaleX * LogicalCentreX(role));
        const double vertical = markers[role]->centreY - (geometry.originY + geometry.scaleY * LogicalCentreY(role));
        maximumResidual = std::max(maximumResidual, std::hypot(horizontal, vertical));
        if (std::abs(markers[role]->scaleX / geometry.scaleX - 1) > LocatorLimits::moduleScaleTolerance ||
            std::abs(markers[role]->scaleY / geometry.scaleY - 1) > LocatorLimits::moduleScaleTolerance)
        {
            return false;
        }
    }
    geometry.markerResidualPixels = maximumResidual;
    return maximumResidual <= policy.maximumGeometryResidualPixels;
}

bool FindEdge(LumaReader& reader, const bool horizontal, const double predicted, const double perpendicular,
              const double midpoint, const bool rising, double& crossing) noexcept
{
    const double start = predicted - LocatorLimits::edgeSearchRadius;
    constexpr auto intervals = static_cast<unsigned>(2 * LocatorLimits::edgeSearchRadius / LocatorLimits::edgeSearchStep);
    double previous = 0;
    const double startX = (horizontal ? start : perpendicular) - 0.5;
    const double startY = (horizontal ? perpendicular : start) - 0.5;
    if (!reader.Contains(startX, startY) || !reader.Sample(startX, startY, previous))
    {
        return false;
    }
    bool found = false;
    double result = 0;
    for (unsigned index = 1; index <= intervals; index++)
    {
        const double position = start + index * LocatorLimits::edgeSearchStep;
        const double x = (horizontal ? position : perpendicular) - 0.5;
        const double y = (horizontal ? perpendicular : position) - 0.5;
        double current = 0;
        if (!reader.Contains(x, y) || !reader.Sample(x, y, current))
        {
            return false;
        }
        const bool bracketed = rising ? previous <= midpoint && current >= midpoint && current > previous :
                                        previous >= midpoint && current <= midpoint && current < previous;
        if (bracketed)
        {
            const double candidate = position - LocatorLimits::edgeSearchStep + LocatorLimits::edgeSearchStep * (midpoint - previous) / (current - previous);
            if (found && std::abs(candidate - result) > LocatorLimits::scaleRoundoffTolerance)
            {
                return false;
            }
            found = true;
            result = candidate;
        }
        previous = current;
    }
    crossing = result;
    return found;
}

bool FitAxis(LumaReader& reader, const std::array<const Marker*, 4>& markers, const LocalDesktopGeometry& geometry,
             const bool horizontal, double& origin, double& scale, double& maximumResidual) noexcept
{
    constexpr std::array<double, 6> offsets{-28, -20, -12, 12, 20, 28};
    std::array<double, 24> logical{};
    std::array<double, 24> measured{};
    double logicalMean = 0;
    double measuredMean = 0;
    for (std::size_t role = 0; role < markers.size(); role++)
    {
        const double centre = horizontal ? LogicalCentreX(role) : LogicalCentreY(role);
        const double perpendicular = horizontal ? geometry.originY + geometry.scaleY * LogicalCentreY(role) :
                                                  geometry.originX + geometry.scaleX * LogicalCentreX(role);
        const double midpoint = (markers[role]->blackLevel + markers[role]->whiteLevel) * 0.5;
        for (std::size_t edge = 0; edge < offsets.size(); edge++)
        {
            const std::size_t index = role * offsets.size() + edge;
            logical[index] = centre + offsets[edge];
            const double predicted = horizontal ? geometry.originX + geometry.scaleX * logical[index] :
                                                  geometry.originY + geometry.scaleY * logical[index];
            if (!FindEdge(reader, horizontal, predicted, perpendicular, midpoint, edge % 2 != 0, measured[index]))
            {
                return false;
            }
            logicalMean += logical[index];
            measuredMean += measured[index];
        }
    }
    logicalMean /= static_cast<double>(logical.size());
    measuredMean /= static_cast<double>(measured.size());
    double numerator = 0;
    double denominator = 0;
    for (std::size_t index = 0; index < logical.size(); index++)
    {
        numerator += (logical[index] - logicalMean) * (measured[index] - measuredMean);
        denominator += (logical[index] - logicalMean) * (logical[index] - logicalMean);
    }
    scale = numerator / denominator;
    origin = measuredMean - scale * logicalMean;
    maximumResidual = 0;
    for (std::size_t index = 0; index < logical.size(); index++)
    {
        maximumResidual = std::max(maximumResidual, std::abs(measured[index] - origin - scale * logical[index]));
    }
    return std::isfinite(origin) && std::isfinite(scale) && std::isfinite(maximumResidual);
}

bool RefineGeometry(LumaReader& reader, const std::array<const Marker*, 4>& markers,
                    const LocalDesktopDecodePolicy& policy, LocalDesktopGeometry& geometry) noexcept
{
    for (std::uint32_t iteration = 0; iteration < policy.maximumRefinementIterations; iteration++)
    {
        auto refined = geometry;
        double horizontalResidual = 0;
        double verticalResidual = 0;
        if (!FitAxis(reader, markers, geometry, true, refined.originX, refined.scaleX, horizontalResidual) ||
            !FitAxis(reader, markers, geometry, false, refined.originY, refined.scaleY, verticalResidual) ||
            !WithinScale(refined.scaleX, policy) || !WithinScale(refined.scaleY, policy))
        {
            return false;
        }
        refined.markerResidualPixels = std::max(horizontalResidual, verticalResidual);
        if (refined.markerResidualPixels > policy.maximumGeometryResidualPixels)
        {
            return false;
        }
        const double movement = std::max(std::abs(refined.originX - geometry.originX) + kLocalDesktopCanvasWidth * std::abs(refined.scaleX - geometry.scaleX),
                                         std::abs(refined.originY - geometry.originY) + kLocalDesktopCanvasHeight * std::abs(refined.scaleY - geometry.scaleY));
        geometry = refined;
        if (movement <= LocatorLimits::refinementConvergence)
        {
            break;
        }
    }
    return true;
}

struct CoreSample
{
    static constexpr std::size_t count = 5;
    std::array<double, count> values{};
    double mean = 0;
};

bool SampleCore(LumaReader& reader, const LocalDesktopGeometry& geometry, const double logicalX, const double logicalY, CoreSample& output) noexcept
{
    constexpr std::array<std::array<double, 2>, CoreSample::count> offsets{{{0, 0}, {-1, 0}, {1, 0}, {0, -1}, {0, 1}}};
    CoreSample samples;
    double total = 0;
    for (std::size_t index = 0; index < offsets.size(); index++)
    {
        const auto& offset = offsets[index];
        if (!reader.Sample(geometry.originX + geometry.scaleX * (logicalX + offset[0]) - 0.5,
                           geometry.originY + geometry.scaleY * (logicalY + offset[1]) - 0.5, samples.values[index]))
        {
            return false;
        }
        total += samples.values[index];
    }
    samples.mean = total / static_cast<double>(offsets.size());
    output = samples;
    return true;
}

Erasure Calibrate(LumaReader& reader, const LocalDesktopDecodePolicy& policy, LocalDesktopObservation& observation) noexcept
{
    double blackTotal = 0;
    double whiteTotal = 0;
    unsigned blackCount = 0;
    unsigned whiteCount = 0;
    std::array<std::array<double, 49>, 4> levels{};
    for (std::size_t role = 0; role < kLocalDesktopMarkerRegions.size(); role++)
    {
        const auto& region = kLocalDesktopMarkerRegions[role];
        for (std::uint32_t row = 0; row < kLocalDesktopMarkerModules; row++)
        {
            for (std::uint32_t column = 0; column < kLocalDesktopMarkerModules; column++)
            {
                CoreSample sample;
                const double x = region.x + kLocalDesktopMarkerQuietPixels + (static_cast<double>(column) + 0.5) * kLocalDesktopCellPixels;
                const double y = region.y + kLocalDesktopMarkerQuietPixels + (static_cast<double>(row) + 0.5) * kLocalDesktopCellPixels;
                if (!SampleCore(reader, observation.geometry, x, y, sample))
                {
                    return reader.Error();
                }
                levels[role][row * kLocalDesktopMarkerModules + column] = sample.mean;
                if (detail::MarkerModule(role, column, row) == 0)
                {
                    blackTotal += sample.mean;
                    blackCount++;
                }
                else
                {
                    whiteTotal += sample.mean;
                    whiteCount++;
                }
            }
        }
    }
    observation.blackLevel = blackTotal / blackCount;
    observation.whiteLevel = whiteTotal / whiteCount;
    const double contrast = observation.whiteLevel - observation.blackLevel;
    if (contrast < policy.minimumContrast)
    {
        return Erasure::LowContrast;
    }
    for (std::size_t role = 0; role < levels.size(); role++)
    {
        double residual = 0;
        for (std::uint32_t row = 0; row < kLocalDesktopMarkerModules; row++)
        {
            for (std::uint32_t column = 0; column < kLocalDesktopMarkerModules; column++)
            {
                const double normalized = (levels[role][row * kLocalDesktopMarkerModules + column] - observation.blackLevel) / contrast;
                residual += std::abs(normalized - detail::MarkerModule(role, column, row));
            }
        }
        if (residual / static_cast<double>(levels[role].size()) > policy.maximumMarkerResidual)
        {
            return Erasure::ExcessResidual;
        }
    }
    return Erasure::None;
}

bool IsMidGray(const double normalized, const LocalDesktopDecodePolicy& policy) noexcept
{
    return normalized >= policy.midGrayBoundary && normalized <= 1 - policy.midGrayBoundary;
}

unsigned CountMidGraySamples(const CoreSample& sample, const double black, const double contrast, const LocalDesktopDecodePolicy& policy) noexcept
{
    unsigned count = 0;
    for (const auto value : sample.values)
    {
        count += static_cast<unsigned>(IsMidGray((value - black) / contrast, policy));
    }
    return count;
}

Erasure DecodeCopy(LumaReader& reader, const LocalDesktopDecodePolicy& policy, const LocalDesktopGeometry& geometry,
                   const double black, const double white, const std::size_t copyIndex, LocalDesktopCopyObservation& output, const detail::LocalDesktopBinding binding) noexcept
{
    const auto& region = kLocalDesktopBootstrapRegions[copyIndex];
    std::array<std::byte, kLocalDesktopRsCodewordBytes> codeword{};
    constexpr std::size_t bitCount = kLocalDesktopRsCodewordBytes * 8;
    unsigned midGrayCount = 0;
    unsigned sampleMidGrayCount = 0;
    const double contrast = white - black;
    double residual = 0;
    for (std::size_t bitIndex = 0; bitIndex < bitCount; bitIndex++)
    {
        const double x = region.x + (static_cast<double>(bitIndex % 76) + 0.5) * kLocalDesktopCellPixels;
        const double y = region.y + (static_cast<double>(bitIndex / 76) + 0.5) * kLocalDesktopCellPixels;
        CoreSample sample;
        if (!SampleCore(reader, geometry, x, y, sample))
        {
            return reader.Error();
        }
        const double normalized = (sample.mean - black) / contrast;
        residual += std::min(std::abs(normalized), std::abs(normalized - 1));
        midGrayCount += static_cast<unsigned>(IsMidGray(normalized, policy));
        sampleMidGrayCount += CountMidGraySamples(sample, black, contrast, policy);
        if (normalized >= 0.5)
        {
            codeword[bitIndex / 8] |= static_cast<std::byte>(1u << (bitIndex % 8));
        }
    }
    output.residual = residual / static_cast<double>(bitCount);
    output.midGrayFraction = static_cast<double>(midGrayCount) / static_cast<double>(bitCount);
    output.sampleMidGrayFraction = static_cast<double>(sampleMidGrayCount) / static_cast<double>(bitCount * CoreSample::count);
    if (!reader.Charge(LocatorLimits::rsWorkUnits))
    {
        return reader.Error();
    }
    const auto status = detail::DecodeBootstrapRs(codeword, output.canonical44);
    output.fecDecoded = static_cast<bool>(status);
    output.correctedSymbols = status.correctedSymbols;
    if (output.fecDecoded)
    {
        const auto record = std::span<const std::byte>(output.canonical44);
        output.crcValid = pbprotocol::ComputeCrc32c(record.first(40)) == Read32(record.data() + 40);
        if (output.crcValid)
        {
            const auto parsed = pbprotocol::ParseBootstrapRecord(record);
            output.recordValid = parsed && detail::MatchesLocalDesktopBinding(parsed.Value().visualProfileId, parsed.Value().visualLayoutVersion, binding);
        }
    }
    // Soft constellation distance deliberately does not compare against the
    // corrected codeword. A hard, full-contrast symbol error remains recoverable
    // by RS; combining A/B or spending RS parity to accept gray blends does not.
    if (output.midGrayFraction > policy.maximumMidGrayFraction || output.sampleMidGrayFraction > policy.maximumMidGrayFraction)
    {
        return Erasure::DoubleImage;
    }
    if (output.residual > policy.maximumBootstrapResidual)
    {
        return Erasure::ExcessResidual;
    }
    if (!output.fecDecoded)
    {
        return Erasure::BootstrapFecFailure;
    }
    if (!output.crcValid)
    {
        return Erasure::BootstrapCrcFailure;
    }
    return output.recordValid ? Erasure::None : Erasure::UnsupportedRecord;
}

Erasure CheckTiming(LumaReader& reader, const LocalDesktopDecodePolicy& policy, LocalDesktopObservation& observation) noexcept
{
    Erasure failure = Erasure::None;
    const double contrast = observation.whiteLevel - observation.blackLevel;
    for (std::size_t patch = 0; patch < kLocalDesktopTimingRegions.size(); patch++)
    {
        if (!reader.Charge(LocatorLimits::timingWorkUnits))
        {
            return reader.Error();
        }
        std::array<std::uint8_t, kLocalDesktopTimingBits> expected{};
        if (!detail::BuildTimingBits(observation.copies[0].canonical44, patch, expected))
        {
            return Erasure::UnsupportedRecord;
        }
        const auto& region = kLocalDesktopTimingRegions[patch];
        double residual = 0;
        unsigned errors = 0;
        unsigned midGrayCount = 0;
        unsigned sampleMidGrayCount = 0;
        for (std::size_t bitIndex = 0; bitIndex < expected.size(); bitIndex++)
        {
            const double x = region.x + (static_cast<double>(bitIndex % 16) + 0.5) * kLocalDesktopCellPixels;
            const double y = region.y + (static_cast<double>(bitIndex / 16) + 0.5) * kLocalDesktopCellPixels;
            CoreSample sample;
            if (!SampleCore(reader, observation.geometry, x, y, sample))
            {
                return reader.Error();
            }
            const double normalized = (sample.mean - observation.blackLevel) / contrast;
            residual += std::abs(normalized - expected[bitIndex]);
            errors += static_cast<unsigned>((normalized >= 0.5) != (expected[bitIndex] != 0));
            midGrayCount += static_cast<unsigned>(IsMidGray(normalized, policy));
            sampleMidGrayCount += CountMidGraySamples(sample, observation.blackLevel, contrast, policy);
        }
        const double patchResidual = residual / static_cast<double>(expected.size());
        const double errorFraction = static_cast<double>(errors) / static_cast<double>(expected.size());
        const double midGrayFraction = static_cast<double>(midGrayCount) / static_cast<double>(expected.size());
        const double sampleMidGrayFraction = static_cast<double>(sampleMidGrayCount) / static_cast<double>(expected.size() * CoreSample::count);
        observation.timingResidual = std::max(observation.timingResidual, patchResidual);
        observation.timingBitErrorFraction = std::max(observation.timingBitErrorFraction, errorFraction);
        observation.midGrayFraction = std::max(observation.midGrayFraction, midGrayFraction);
        observation.sampleMidGrayFraction = std::max(observation.sampleMidGrayFraction, sampleMidGrayFraction);
        // Gate every distributed patch independently. A replaced centre patch
        // must not disappear into an average over eight unchanged corners.
        if (midGrayFraction > policy.maximumMidGrayFraction || sampleMidGrayFraction > policy.maximumMidGrayFraction)
        {
            failure = Erasure::DoubleImage;
        }
        else if (failure != Erasure::DoubleImage && errorFraction > policy.maximumTimingBitErrorFraction)
        {
            failure = Erasure::TimingMismatch;
        }
        else if (failure == Erasure::None && patchResidual > policy.maximumTimingResidual)
        {
            failure = Erasure::ExcessResidual;
        }
    }
    return failure;
}

LocalDesktopObservation EvaluateGeometry(LumaReader& reader, const std::array<const Marker*, 4>& markers,
                                          const LocalDesktopDecodePolicy& policy, LocalDesktopGeometry geometry, const detail::LocalDesktopBinding binding) noexcept
{
    LocalDesktopObservation observation;
    observation.geometry = geometry;
    if (!RefineGeometry(reader, markers, policy, geometry))
    {
        observation.erasure = reader.Error() == Erasure::None ? Erasure::InvalidGeometry : reader.Error();
        return observation;
    }
    observation.geometry = geometry;
    observation.erasure = Calibrate(reader, policy, observation);
    if (observation.erasure != Erasure::None)
    {
        return observation;
    }
    std::array<Erasure, 2> copyErrors{};
    for (std::size_t copyIndex = 0; copyIndex < observation.copies.size(); copyIndex++)
    {
        copyErrors[copyIndex] = DecodeCopy(reader, policy, geometry, observation.blackLevel, observation.whiteLevel, copyIndex, observation.copies[copyIndex], binding);
        observation.midGrayFraction = std::max(observation.midGrayFraction, observation.copies[copyIndex].midGrayFraction);
        observation.sampleMidGrayFraction = std::max(observation.sampleMidGrayFraction, observation.copies[copyIndex].sampleMidGrayFraction);
        if (reader.Error() != Erasure::None)
        {
            observation.erasure = reader.Error();
            return observation;
        }
    }
    for (const auto reason : copyErrors)
    {
        if (reason == Erasure::DoubleImage)
        {
            observation.erasure = reason;
            return observation;
        }
    }
    for (const auto reason : copyErrors)
    {
        if (reason != Erasure::None)
        {
            observation.erasure = reason;
            return observation;
        }
    }
    if (observation.copies[0].canonical44 != observation.copies[1].canonical44)
    {
        observation.erasure = Erasure::BootstrapMismatch;
        return observation;
    }
    observation.erasure = CheckTiming(reader, policy, observation);
    const bool unifiedLocalTimingFailure = binding == detail::LocalDesktopBinding::UnifiedVisual &&
        (observation.erasure == Erasure::TimingMismatch || observation.erasure == Erasure::DoubleImage ||
            observation.erasure == Erasure::ExcessResidual);
    if (unifiedLocalTimingFailure)
    {
        // Unified timing patches protect only the data neighborhood assigned
        // to that patch. The owning decoder re-evaluates all nine patches and
        // zeros only those local metrics; the common scaffold must still fail
        // closed for reader/work errors and every Bootstrap/locator conflict.
        observation.erasure = Erasure::None;
    }
    if (observation.erasure != Erasure::None)
    {
        return observation;
    }
    observation.canonical44 = observation.copies[0].canonical44;
    const double worstResidual = std::max({observation.copies[0].residual, observation.copies[1].residual, observation.timingResidual});
    observation.quality = std::clamp(1.0 - worstResidual, 0.0, 1.0);
    return observation;
}

bool SameGeometry(const LocalDesktopGeometry& left, const LocalDesktopGeometry& right) noexcept
{
    return std::abs(left.originX - right.originX) < LocatorLimits::duplicateCentrePixels &&
           std::abs(left.originY - right.originY) < LocatorLimits::duplicateCentrePixels &&
           kLocalDesktopCanvasWidth * std::abs(left.scaleX - right.scaleX) < LocatorLimits::duplicateCentrePixels &&
           kLocalDesktopCanvasHeight * std::abs(left.scaleY - right.scaleY) < LocatorLimits::duplicateCentrePixels;
}

int DiagnosticDepth(const LocalDesktopObservation& observation) noexcept
{
    int depth = observation.geometry.scaleX > 0 ? 1 : 0;
    depth += observation.whiteLevel > observation.blackLevel ? 1 : 0;
    for (const auto& copy : observation.copies)
    {
        depth += copy.fecDecoded ? 1 : 0;
        depth += copy.crcValid ? 1 : 0;
        depth += copy.recordValid ? 1 : 0;
    }
    depth += observation.erasure == Erasure::TimingMismatch || observation.erasure == Erasure::DoubleImage ? 1 : 0;
    return depth;
}

bool ResolveLocalDesktopBinding(const LocalDesktopBootstrapBinding& binding,
    detail::LocalDesktopBinding& output) noexcept
{
    if (binding == LocalDesktopBootstrapBinding{kLocalDesktopVisualProfileId, kLocalDesktopLayoutVersion})
    {
        output = detail::LocalDesktopBinding::BootstrapOnly;
        return true;
    }
    if (binding.visualLayoutVersion == kDesktopLevelsLayoutVersion && GetDesktopLevelsProfile(binding.visualProfileId) != nullptr)
    {
        output = detail::LocalDesktopBinding::DesktopLevels;
        return true;
    }
    if (binding == LocalDesktopBootstrapBinding{kShapeChromaProfileId, kShapeChromaLayoutVersion})
    {
        output = detail::LocalDesktopBinding::ShapeChroma;
        return true;
    }
    if (binding == LocalDesktopBootstrapBinding{kRemoteVisualProfileId, kRemoteVisualLayoutVersion})
    {
        output = detail::LocalDesktopBinding::RemoteVisual;
        return true;
    }
    if (binding == LocalDesktopBootstrapBinding{kRemoteVisualLowFpsProfileId, kRemoteVisualLowFpsLayoutVersion})
    {
        output = detail::LocalDesktopBinding::RemoteVisualLowFps;
        return true;
    }
    if (binding == LocalDesktopBootstrapBinding{kUnifiedVisualProfile.productProfile.visualProfileId,
        kUnifiedVisualProfile.productProfile.visualLayoutVersion})
    {
        output = detail::LocalDesktopBinding::UnifiedVisual;
        return true;
    }
    return false;
}

} // namespace

Erasure ValidateLumaView(const LumaView& view) noexcept
{
    const std::size_t pixelBytes = BytesPerPixel(view.pixelFormat);
    if (pixelBytes == 0)
    {
        return Erasure::UnsupportedFormat;
    }
    if (view.width == 0 || view.height == 0 || view.pixels.data() == nullptr ||
        view.width > std::numeric_limits<std::size_t>::max() / pixelBytes)
    {
        return Erasure::InvalidView;
    }
    const std::size_t rowBytes = static_cast<std::size_t>(view.width) * pixelBytes;
    if (view.rowPitch < rowBytes || static_cast<std::size_t>(view.height - 1u) > (std::numeric_limits<std::size_t>::max() - rowBytes) / view.rowPitch)
    {
        return Erasure::InvalidView;
    }
    const std::size_t footprint = static_cast<std::size_t>(view.height - 1u) * view.rowPitch + rowBytes;
    return footprint <= view.pixels.size() ? Erasure::None : Erasure::InvalidView;
}

Erasure SampleLuma(const LumaView& view, const double x, const double y, double& output) noexcept
{
    const auto status = ValidateLumaView(view);
    if (status != Erasure::None)
    {
        return status;
    }
    LumaReader reader(view, 4);
    return reader.Sample(x, y, output) ? Erasure::None : reader.Error();
}

LocalDesktopObservation detail::DecodeLocalDesktopScaffold(const LumaView& view, const LocalDesktopDecodePolicy& policy, const LocalDesktopBinding binding) noexcept
{
    LocalDesktopObservation result;
    result.erasure = ValidateLumaView(view);
    if (result.erasure != Erasure::None)
    {
        return result;
    }
    if (!ValidateLocalDesktopDecodePolicy(policy))
    {
        result.erasure = Erasure::InvalidPolicy;
        return result;
    }
    LumaReader reader(view, policy.maximumWorkUnits);
    MarkerSet markerSet;
    result.erasure = LocateMarkers(reader, policy, markerSet);
    result.markerCandidates = static_cast<std::uint32_t>(markerSet.size);
    result.workUnits = reader.WorkUnits();
    if (result.erasure != Erasure::None)
    {
        return result;
    }
    std::array<std::array<std::size_t, LocatorLimits::markerCapacity>, 4> roleIndices{};
    std::array<std::size_t, 4> roleCounts{};
    for (std::size_t index = 0; index < markerSet.size; index++)
    {
        const auto role = markerSet.markers[index].role;
        roleIndices[role][roleCounts[role]++] = index;
    }
    std::array<LocalDesktopGeometry, LocatorLimits::geometryCapacity> geometries{};
    std::uint32_t geometryCount = 0;
    std::uint32_t acceptedCount = 0;
    result.erasure = Erasure::InvalidGeometry;
    bool abort = false;
    for (std::size_t topLeft = 0; topLeft < roleCounts[0] && !abort; topLeft++)
    {
        for (std::size_t topRight = 0; topRight < roleCounts[1] && !abort; topRight++)
        {
            for (std::size_t bottomLeft = 0; bottomLeft < roleCounts[2] && !abort; bottomLeft++)
            {
                for (std::size_t bottomRight = 0; bottomRight < roleCounts[3]; bottomRight++)
                {
                    if (!reader.Charge())
                    {
                        abort = true;
                        break;
                    }
                    const std::array<const Marker*, 4> markers{&markerSet.markers[roleIndices[0][topLeft]], &markerSet.markers[roleIndices[1][topRight]],
                                                              &markerSet.markers[roleIndices[2][bottomLeft]], &markerSet.markers[roleIndices[3][bottomRight]]};
                    LocalDesktopGeometry geometry;
                    if (!MakeGeometry(markers, policy, geometry))
                    {
                        continue;
                    }
                    bool duplicate = false;
                    for (std::uint32_t index = 0; index < geometryCount; index++)
                    {
                        duplicate = duplicate || SameGeometry(geometries[index], geometry);
                    }
                    if (duplicate)
                    {
                        continue;
                    }
                    if (geometryCount == policy.maximumGeometries)
                    {
                        result.erasure = Erasure::GeometryBudgetExceeded;
                        abort = true;
                        break;
                    }
                    geometries[geometryCount++] = geometry;
                    auto observation = EvaluateGeometry(reader, markers, policy, geometry, binding);
                    if (reader.Error() != Erasure::None)
                    {
                        result = observation;
                        abort = true;
                        break;
                    }
                    if (observation.IsAccepted())
                    {
                        acceptedCount++;
                        if (acceptedCount == 1)
                        {
                            result = observation;
                        }
                    }
                    else if (acceptedCount == 0 && DiagnosticDepth(observation) > DiagnosticDepth(result))
                    {
                        result = observation;
                    }
                }
            }
        }
    }
    if (reader.Error() != Erasure::None)
    {
        result.erasure = reader.Error();
    }
    else if (!abort && acceptedCount > 1)
    {
        result.erasure = Erasure::AmbiguousGeometry;
    }
    result.markerCandidates = static_cast<std::uint32_t>(markerSet.size);
    result.geometryCandidates = geometryCount;
    result.workUnits = reader.WorkUnits();
    if (!result.IsAccepted())
    {
        result.canonical44.fill(std::byte{0});
        result.quality = 0;
    }
    return result;
}

LocalDesktopObservation detail::DecodeLocalDesktopFixedCanvasScaffold(const LumaView& view, const LocalDesktopDecodePolicy& policy,
    const LocalDesktopBinding binding, const LocalDesktopBootstrapBinding& expectedBinding) noexcept
{
    LocalDesktopObservation result;
    result.erasure = ValidateLumaView(view);
    if (result.erasure != Erasure::None)
    {
        return result;
    }
    if (!ValidateLocalDesktopDecodePolicy(policy))
    {
        result.erasure = Erasure::InvalidPolicy;
        return result;
    }
    if (view.width != kLocalDesktopCanvasWidth || view.height != kLocalDesktopCanvasHeight)
    {
        result.erasure = Erasure::InvalidView;
        return result;
    }
    LumaReader reader(view, policy.maximumWorkUnits);
    std::array<Marker, 4> fixedMarkers{};
    std::array<const Marker*, 4> markerPointers{};
    bool lowContrast = false;
    for (std::size_t role = 0; role < fixedMarkers.size(); role++)
    {
        const auto& region = kLocalDesktopMarkerRegions[role];
        auto& marker = fixedMarkers[role];
        marker.centreX = static_cast<double>(region.x) + static_cast<double>(region.width) * 0.5;
        marker.centreY = static_cast<double>(region.y) + static_cast<double>(region.height) * 0.5;
        marker.scaleX = 1;
        marker.scaleY = 1;
        if (!VerifyMarker(reader, policy, marker, lowContrast) || marker.role != role)
        {
            result.erasure = reader.Error() != Erasure::None ? reader.Error() :
                lowContrast ? Erasure::LowContrast : Erasure::MarkersNotFound;
            result.markerCandidates = static_cast<std::uint32_t>(role);
            result.workUnits = reader.WorkUnits();
            return result;
        }
        markerPointers[role] = &marker;
    }
    result.markerCandidates = static_cast<std::uint32_t>(fixedMarkers.size());
    LocalDesktopGeometry geometry;
    if (!MakeGeometry(markerPointers, policy, geometry))
    {
        result.erasure = Erasure::InvalidGeometry;
        result.workUnits = reader.WorkUnits();
        return result;
    }
    result.geometryCandidates = 1;
    result = EvaluateGeometry(reader, markerPointers, policy, geometry, binding);
    result.markerCandidates = static_cast<std::uint32_t>(fixedMarkers.size());
    result.geometryCandidates = 1;
    result.workUnits = reader.WorkUnits();
    if (result.IsAccepted())
    {
        const auto parsed = pbprotocol::ParseBootstrapRecord(result.canonical44);
        if (!parsed || parsed.Value().visualProfileId != expectedBinding.visualProfileId ||
            parsed.Value().visualLayoutVersion != expectedBinding.visualLayoutVersion)
        {
            result.erasure = Erasure::UnsupportedRecord;
        }
        else if (binding == LocalDesktopBinding::DesktopLevels &&
            ValidateDesktopLevelsGeometry(result.geometry) != DesktopLevelsErasure::None)
        {
            result.erasure = Erasure::InvalidGeometry;
        }
        else if (binding == LocalDesktopBinding::ShapeChroma &&
            ValidateShapeChromaGeometry(result.geometry) != ShapeChromaErasure::None)
        {
            result.erasure = Erasure::InvalidGeometry;
        }
        else if (binding == LocalDesktopBinding::RemoteVisual &&
            ValidateRemoteVisualGeometry(result.geometry) != RemoteVisualErasure::None)
        {
            result.erasure = Erasure::InvalidGeometry;
        }
        else if (binding == LocalDesktopBinding::RemoteVisualLowFps &&
            ValidateRemoteVisualLowFpsGeometry(result.geometry) != RemoteVisualLowFpsErasure::None)
        {
            result.erasure = Erasure::InvalidGeometry;
        }
    }
    if (!result.IsAccepted())
    {
        result.canonical44.fill(std::byte{0});
        result.quality = 0;
    }
    return result;
}

LocalDesktopObservation DecodeLocalDesktopBootstrap(const LumaView& view, const LocalDesktopDecodePolicy& policy) noexcept
{
    return detail::DecodeLocalDesktopScaffold(view, policy, detail::LocalDesktopBinding::BootstrapOnly);
}

LocalDesktopObservation DecodeLocalDesktopBootstrap(const LumaView& view, const LocalDesktopBootstrapBinding& binding,
    const LocalDesktopDecodePolicy& policy) noexcept
{
    detail::LocalDesktopBinding family;
    if (!ResolveLocalDesktopBinding(binding, family))
    {
        LocalDesktopObservation result;
        result.erasure = LocalDesktopErasureReason::UnsupportedRecord;
        return result;
    }
    return detail::DecodeLocalDesktopScaffold(view, policy, family);
}

LocalDesktopObservation DecodeLocalDesktopFixedCanvasBootstrap(const LumaView& view, const LocalDesktopBootstrapBinding& binding,
    const LocalDesktopDecodePolicy& policy) noexcept
{
    detail::LocalDesktopBinding family;
    if (!ResolveLocalDesktopBinding(binding, family))
    {
        LocalDesktopObservation result;
        result.erasure = LocalDesktopErasureReason::UnsupportedRecord;
        return result;
    }
    return detail::DecodeLocalDesktopFixedCanvasScaffold(view, policy, family, binding);
}

const char* GetLocalDesktopErasureName(const Erasure reason) noexcept
{
    switch (reason)
    {
    case Erasure::None: return "None";
    case Erasure::InvalidView: return "InvalidView";
    case Erasure::InvalidPolicy: return "InvalidPolicy";
    case Erasure::UnsupportedFormat: return "UnsupportedFormat";
    case Erasure::NonFinitePixel: return "NonFinitePixel";
    case Erasure::InvalidPixelValue: return "InvalidPixelValue";
    case Erasure::SampleOutOfBounds: return "SampleOutOfBounds";
    case Erasure::WorkBudgetExceeded: return "WorkBudgetExceeded";
    case Erasure::MarkerBudgetExceeded: return "MarkerBudgetExceeded";
    case Erasure::GeometryBudgetExceeded: return "GeometryBudgetExceeded";
    case Erasure::MarkersNotFound: return "MarkersNotFound";
    case Erasure::IncompleteMarkers: return "IncompleteMarkers";
    case Erasure::InvalidGeometry: return "InvalidGeometry";
    case Erasure::AmbiguousGeometry: return "AmbiguousGeometry";
    case Erasure::LowContrast: return "LowContrast";
    case Erasure::BootstrapFecFailure: return "BootstrapFecFailure";
    case Erasure::BootstrapCrcFailure: return "BootstrapCrcFailure";
    case Erasure::UnsupportedRecord: return "UnsupportedRecord";
    case Erasure::BootstrapMismatch: return "BootstrapMismatch";
    case Erasure::TimingMismatch: return "TimingMismatch";
    case Erasure::DoubleImage: return "DoubleImage";
    case Erasure::ExcessResidual: return "ExcessResidual";
    default: return "UnknownErasure";
    }
}

} // namespace pbmodulation
