// Appended only to a hash-checked tool copy of local_desktop_decode.cpp.
namespace pbmodulation
{
pbg1b::EdgeProbe InspectG1BCanonicalEdges(const LumaView& view, const LocalDesktopDecodePolicy& policy) noexcept
{
    pbg1b::EdgeProbe result;
    if (ValidateLumaView(view) != Erasure::None || view.pixelFormat != LumaPixelFormat::Bgra8)
    {
        result.reason = "InvalidView";
        return result;
    }
    if (!ValidateLocalDesktopDecodePolicy(policy))
    {
        result.reason = "InvalidPolicy";
        return result;
    }
    if (view.width != kLocalDesktopCanvasWidth || view.height != kLocalDesktopCanvasHeight)
    {
        result.reason = "NotCanonicalCanvas";
        return result;
    }
    if (policy.minimumScale > 1 || policy.maximumScale < 1 || policy.maximumMarkers < 4)
    {
        result.reason = "CallerPolicyExcludesHypothesis";
        return result;
    }
    LumaReader reader(view, policy.maximumWorkUnits);
    const auto Finish = [&reader, &result](const char* const reason)
    {
        result.reason = reason;
        result.workUnits = reader.WorkUnits();
        return result;
    };
    std::array<Marker, 4> markers{};
    std::array<const Marker*, 4> markerPointers{};
    for (std::size_t role = 0; role < markers.size(); role++)
    {
        auto& marker = markers[role];
        marker.centreX = LogicalCentreX(role);
        marker.centreY = LogicalCentreY(role);
        marker.scaleX = 1;
        marker.scaleY = 1;
        bool lowContrast = false;
        if (!VerifyMarker(reader, policy, marker, lowContrast) || marker.role != role)
        {
            return Finish(reader.Error() == Erasure::None ? "MarkerRejected" : "ReadBudgetOrPixelError");
        }
        markerPointers[role] = &marker;
        result.markerResiduals[role] = marker.residual;
        result.markersVerified++;
    }
    constexpr std::array<double, 6> offsets{-28, -20, -12, 12, 20, 28};
    for (std::size_t axis = 0; axis < 2; axis++)
    {
        const bool horizontal = axis == 0;
        for (std::size_t role = 0; role < markers.size(); role++)
        {
            const auto& marker = markers[role];
            const double centre = horizontal ? LogicalCentreX(role) : LogicalCentreY(role);
            const double perpendicular = horizontal ? LogicalCentreY(role) : LogicalCentreX(role);
            for (std::size_t edge = 0; edge < offsets.size(); edge++)
            {
                auto& reading = result.edges[axis * 24 + role * offsets.size() + edge];
                reading.logical = centre + offsets[edge];
                reading.black = marker.blackLevel;
                reading.white = marker.whiteLevel;
                reading.crossingMeasured = FindEdge(reader, horizontal, reading.logical, perpendicular,
                    (marker.blackLevel + marker.whiteLevel) * 0.5, edge % 2 != 0, reading.crossing);
                if (reading.crossingMeasured)
                {
                    result.crossingsMeasured++;
                    const double integerEdge = std::round(reading.crossing);
                    const double beforeX = horizontal ? integerEdge - 1 : perpendicular - 0.5;
                    const double beforeY = horizontal ? perpendicular - 0.5 : integerEdge - 1;
                    const double afterX = horizontal ? integerEdge : perpendicular - 0.5;
                    const double afterY = horizontal ? perpendicular - 0.5 : integerEdge;
                    reading.endpointsMeasured = reader.Contains(beforeX, beforeY) && reader.Contains(afterX, afterY) &&
                        reader.Sample(beforeX, beforeY, reading.before) && reader.Sample(afterX, afterY, reading.after);
                    result.endpointsMeasured += static_cast<std::uint32_t>(reading.endpointsMeasured);
                }
                if (reader.Error() != Erasure::None)
                {
                    return Finish("ReadBudgetOrPixelError");
                }
            }
        }
    }
    if (result.crossingsMeasured != result.edges.size() || result.endpointsMeasured != result.edges.size())
    {
        return Finish("IncompleteEdgeMeasurements");
    }
    const LocalDesktopGeometry seed{0, 0, 1, 1, 0};
    double horizontalResidual = 0;
    double verticalResidual = 0;
    result.firstFitMeasured = FitAxis(reader, markerPointers, seed, true, result.firstFit.originX, result.firstFit.scaleX, horizontalResidual) &&
        FitAxis(reader, markerPointers, seed, false, result.firstFit.originY, result.firstFit.scaleY, verticalResidual);
    if (!result.firstFitMeasured)
    {
        return Finish("FitReadFailed");
    }
    result.firstFit.markerResidualPixels = std::max(horizontalResidual, verticalResidual);
    return Finish("MeasuredOnlyNotAdmission");
}
}

namespace pbg1b
{
EdgeProbe InspectCanonicalEdges(const pbmodulation::LumaView& view, const pbmodulation::LocalDesktopDecodePolicy& policy) noexcept
{
    return pbmodulation::InspectG1BCanonicalEdges(view, policy);
}
}
