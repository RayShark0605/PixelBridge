namespace pbmodulation
{
namespace
{
bool MeasureProbeEdge(LumaReader& reader, const bool horizontal, const bool rising, pbgeomprobe::Edge& edge) noexcept
{
    const double midpoint = (edge.black + edge.white) * 0.5;
    if (!FindEdge(reader, horizontal, edge.predicted, edge.perpendicular, midpoint, rising, edge.crossing))
    {
        return false;
    }
    const double start = edge.predicted - LocatorLimits::edgeSearchRadius;
    constexpr auto intervals = static_cast<unsigned>(2 * LocatorLimits::edgeSearchRadius / LocatorLimits::edgeSearchStep);
    double previous = 0;
    const auto Sample = [&](const double position, double& value)
    {
        const double x = (horizontal ? position : edge.perpendicular) - 0.5;
        const double y = (horizontal ? edge.perpendicular : position) - 0.5;
        return reader.Contains(x, y) && reader.Sample(x, y, value);
    };
    if (!Sample(start, previous))
    {
        return false;
    }
    bool found = false;
    double lastCrossing = 0;
    for (unsigned index = 1; index <= intervals; index++)
    {
        const double position = start + index * LocatorLimits::edgeSearchStep;
        double current = 0;
        if (!Sample(position, current))
        {
            return false;
        }
        const bool bracketed = rising ? previous <= midpoint && current >= midpoint && current > previous : previous >= midpoint && current <= midpoint && current < previous;
        if (bracketed)
        {
            const double crossing = position - LocatorLimits::edgeSearchStep + LocatorLimits::edgeSearchStep * (midpoint - previous) / (current - previous);
            if (found && std::abs(crossing - lastCrossing) > LocatorLimits::scaleRoundoffTolerance)
            {
                return false;
            }
            edge.leftPosition = position - LocatorLimits::edgeSearchStep;
            edge.rightPosition = position;
            edge.leftValue = previous;
            edge.rightValue = current;
            lastCrossing = crossing;
            found = true;
        }
        previous = current;
    }
    if (!found || lastCrossing != edge.crossing)
    {
        return false;
    }
    const double integerEdge = std::round(edge.crossing);
    if (!Sample(integerEdge - 0.5, edge.before) || !Sample(integerEdge + 0.5, edge.after))
    {
        return false;
    }
    edge.measured = true;
    return true;
}
}

pbgeomprobe::Probe InspectCodecGeometry(const LumaView& view, const bool inspectAmbiguity = false, const std::uint32_t candidateOrdinal = 0) noexcept
{
    pbgeomprobe::Probe result;
    const LocalDesktopDecodePolicy policy;
    if (ValidateLumaView(view) != Erasure::None || view.width != 1920 || view.height != 1080 || view.rowPitch != 7680 || view.pixelFormat != LumaPixelFormat::Bgra8)
    {
        result.reason = "InvalidFixedView";
        return result;
    }
    LumaReader locator(view, policy.maximumWorkUnits);
    MarkerSet set;
    const auto locateStatus = LocateMarkers(locator, policy, set);
    result.locateWork = locator.WorkUnits();
    if (locateStatus != Erasure::None)
    {
        result.reason = "OriginalLocateRejected";
        return result;
    }
    std::array<const Marker*, 4> markers{};
    std::array<const Marker*, 2> bottomRightCandidates{};
    for (std::size_t index = 0; index < set.size; index++)
    {
        const auto& marker = set.markers[index];
        if (marker.role >= markers.size())
        {
            result.reason = "UnexpectedMarkerRole";
            return result;
        }
        result.roleCounts[marker.role]++;
        markers[marker.role] = &marker;
        if (marker.role == 3 && result.roleCounts[3] <= bottomRightCandidates.size())
        {
            bottomRightCandidates[result.roleCounts[3] - 1] = &marker;
        }
        result.markers[marker.role] = {marker.centreX, marker.centreY, marker.scaleX, marker.scaleY, marker.blackLevel, marker.whiteLevel, marker.residual, static_cast<double>(marker.role)};
    }
    const bool single = std::ranges::all_of(result.roleCounts, [](const auto count)
    {
        return count == 1;
    });
    if (inspectAmbiguity)
    {
        if (result.roleCounts != std::array<std::uint32_t, 4>{1, 1, 1, 2} || candidateOrdinal >= 2)
        {
            result.reason = "UnexpectedAmbiguityShape_Stop";
            return result;
        }
        markers[3] = bottomRightCandidates[candidateOrdinal];
        const auto& marker = *markers[3];
        result.markers[3] = {marker.centreX, marker.centreY, marker.scaleX, marker.scaleY, marker.blackLevel, marker.whiteLevel, marker.residual, 3};
    }
    else if (!single)
    {
        result.reason = "NotOneMarkerPerRole_StopNoCandidateSelection";
        return result;
    }
    if (!MakeGeometry(markers, policy, result.seed))
    {
        result.reason = "OriginalMakeGeometryRejected";
        return result;
    }
    if (inspectAmbiguity)
    {
        LumaReader evaluator(view, policy.maximumWorkUnits);
        result.evaluation = EvaluateGeometry(evaluator, markers, policy, result.seed, detail::LocalDesktopBinding::UnifiedVisual);
        result.evaluationAttempted = true;
    }
    // Detached observation readers cannot consume or mutate the original reader.
    LumaReader reference(view, policy.maximumWorkUnits);
    auto originalRefined = result.seed;
    if (!RefineGeometry(reference, markers, policy, originalRefined))
    {
        result.reason = "OriginalRefineRejected";
        return result;
    }
    auto originalCovered = originalRefined;
    RefinePointCanvasCoverage(reference, markers, policy, originalCovered);
    result.referenceWork = reference.WorkUnits();
    if (reference.Error() != Erasure::None)
    {
        result.reason = "OriginalCoverageReaderError";
        return result;
    }
    LumaReader fitter(view, policy.maximumWorkUnits);
    LumaReader observer(view, policy.maximumWorkUnits);
    auto geometry = result.seed;
    constexpr std::array<double, 6> offsets{-28, -20, -12, 12, 20, 28};
    for (std::uint32_t index = 0; index < policy.maximumRefinementIterations; index++)
    {
        auto& iteration = result.iterations[index];
        iteration.input = geometry;
        auto refined = geometry;
        double horizontalResidual = 0;
        double verticalResidual = 0;
        if (!FitAxis(fitter, markers, geometry, true, refined.originX, refined.scaleX, horizontalResidual) ||
            !FitAxis(fitter, markers, geometry, false, refined.originY, refined.scaleY, verticalResidual) ||
            !WithinScale(refined.scaleX, policy) || !WithinScale(refined.scaleY, policy))
        {
            result.reason = "OriginalFitRejected";
            return result;
        }
        refined.markerResidualPixels = std::max(horizontalResidual, verticalResidual);
        if (refined.markerResidualPixels > policy.maximumGeometryResidualPixels)
        {
            result.reason = "OriginalResidualRejected";
            return result;
        }
        for (std::size_t axis = 0; axis < 2; axis++)
        {
            const bool horizontal = axis == 0;
            for (std::size_t role = 0; role < markers.size(); role++)
            {
                const double centre = horizontal ? LogicalCentreX(role) : LogicalCentreY(role);
                for (std::size_t edgeIndex = 0; edgeIndex < offsets.size(); edgeIndex++)
                {
                    auto& edge = iteration.edges[axis * 24 + role * 6 + edgeIndex];
                    edge.logical = centre + offsets[edgeIndex];
                    edge.predicted = horizontal ? geometry.originX + geometry.scaleX * edge.logical : geometry.originY + geometry.scaleY * edge.logical;
                    edge.perpendicular = horizontal ? geometry.originY + geometry.scaleY * LogicalCentreY(role) : geometry.originX + geometry.scaleX * LogicalCentreX(role);
                    edge.black = markers[role]->blackLevel;
                    edge.white = markers[role]->whiteLevel;
                    if (!MeasureProbeEdge(observer, horizontal, edgeIndex % 2 != 0, edge))
                    {
                        result.reason = "EdgeObservationFailedOrDoesNotMatchOriginalFindEdge";
                        return result;
                    }
                }
            }
        }
        iteration.fitted = refined;
        iteration.movement = std::max(std::abs(refined.originX - geometry.originX) + kLocalDesktopCanvasWidth * std::abs(refined.scaleX - geometry.scaleX),
            std::abs(refined.originY - geometry.originY) + kLocalDesktopCanvasHeight * std::abs(refined.scaleY - geometry.scaleY));
        iteration.measured = true;
        result.iterationCount++;
        geometry = refined;
        if (iteration.movement <= LocatorLimits::refinementConvergence)
        {
            break;
        }
    }
    result.refined = geometry;
    result.originalRefineMatched = geometry == originalRefined;
    RefinePointCanvasCoverage(fitter, markers, policy, geometry);
    result.finalGeometry = geometry;
    result.originalCoverageMatched = geometry == originalCovered;
    result.fitWork = fitter.WorkUnits();
    result.edgeWork = observer.WorkUnits();
    result.reason = fitter.Error() == Erasure::None && observer.Error() == Erasure::None && result.originalRefineMatched && result.originalCoverageMatched ? "MeasuredOnly_NoAdmissionCandidate" : "ObservationDivergedFromOriginal";
    return result;
}

bool SameCodecProbeSeed(const LocalDesktopGeometry& left, const LocalDesktopGeometry& right) noexcept
{
    return SameGeometry(left, right);
}
}

namespace pbgeomprobe
{
Probe Inspect(const pbmodulation::LumaView& view) noexcept
{
    return pbmodulation::InspectCodecGeometry(view);
}

Probe InspectCandidate(const pbmodulation::LumaView& view, const std::uint32_t candidateOrdinal) noexcept
{
    return pbmodulation::InspectCodecGeometry(view, true, candidateOrdinal);
}

bool SameSeed(const pbmodulation::LocalDesktopGeometry& left, const pbmodulation::LocalDesktopGeometry& right) noexcept
{
    return pbmodulation::SameCodecProbeSeed(left, right);
}
}
