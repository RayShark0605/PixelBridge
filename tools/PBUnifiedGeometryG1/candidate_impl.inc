// Included only after the sealed local_desktop_decode.cpp and its mechanically
// extracted post-geometry validation body in the standalone G1 translation unit.
namespace pbmodulation
{
pbg1::CanonicalCandidate EvaluateG1CanonicalCandidate(const LumaView& view, const LocalDesktopDecodePolicy& policy) noexcept
{
    pbg1::CanonicalCandidate result;
    const auto Fail = [&result](const char* const reason, const Erasure erasure, const std::uint64_t workUnits)
    {
        result.reason = reason;
        result.bootstrap.erasure = erasure;
        result.bootstrap.canonical44.fill(std::byte{0});
        result.bootstrap.quality = 0;
        result.bootstrap.workUnits = workUnits;
        return result;
    };
    const auto viewStatus = ValidateLumaView(view);
    if (viewStatus != Erasure::None || view.pixelFormat != LumaPixelFormat::Bgra8)
    {
        return Fail("InvalidView", viewStatus == Erasure::None ? Erasure::UnsupportedFormat : viewStatus, 0);
    }
    if (!ValidateLocalDesktopDecodePolicy(policy))
    {
        return Fail("InvalidPolicy", Erasure::InvalidPolicy, 0);
    }
    if (view.width != kLocalDesktopCanvasWidth || view.height != kLocalDesktopCanvasHeight)
    {
        return Fail("NotCanonicalCanvas", Erasure::InvalidGeometry, 0);
    }
    if (policy.minimumScale > 1 || policy.maximumScale < 1)
    {
        return Fail("CanonicalScaleOutsideCallerPolicy", Erasure::InvalidGeometry, 0);
    }
    if (policy.maximumMarkers < 4)
    {
        return Fail("MarkerBudgetTooSmall", Erasure::MarkerBudgetExceeded, 0);
    }
    LumaReader reader(view, policy.maximumWorkUnits);
    std::array<Marker, 4> markers{};
    for (std::size_t role = 0; role < markers.size(); role++)
    {
        const auto& region = kLocalDesktopMarkerRegions[role];
        auto& marker = markers[role];
        marker.centreX = region.x + static_cast<double>(region.width) * 0.5;
        marker.centreY = region.y + static_cast<double>(region.height) * 0.5;
        marker.scaleX = 1;
        marker.scaleY = 1;
        bool lowContrast = false;
        if (!VerifyMarker(reader, policy, marker, lowContrast) || marker.role != role)
        {
            return Fail("MarkerRejected", reader.Error() != Erasure::None ? reader.Error() : lowContrast ? Erasure::LowContrast : Erasure::MarkersNotFound, reader.WorkUnits());
        }
    }
    // The only proof attempted here is the existing sharp adjacent-center
    // condition. No compression/noise tolerance or convergence epsilon is added.
    constexpr std::array<double, 6> offsets{-28, -20, -12, 12, 20, 28};
    for (const bool horizontal : {true, false})
    {
        for (std::size_t role = 0; role < markers.size(); role++)
        {
            const auto& marker = markers[role];
            const double centre = horizontal ? LogicalCentreX(role) : LogicalCentreY(role);
            const double perpendicular = horizontal ? LogicalCentreY(role) : LogicalCentreX(role);
            const double midpoint = (marker.blackLevel + marker.whiteLevel) * 0.5;
            const double roundoff = (marker.whiteLevel - marker.blackLevel) * LocatorLimits::scaleRoundoffTolerance;
            for (std::size_t edge = 0; edge < offsets.size(); edge++)
            {
                const double logical = centre + offsets[edge];
                const bool rising = edge % 2 != 0;
                double crossing = 0;
                if (!FindEdge(reader, horizontal, logical, perpendicular, midpoint, rising, crossing))
                {
                    return Fail("EdgeNotFound", reader.Error() != Erasure::None ? reader.Error() : Erasure::InvalidGeometry, reader.WorkUnits());
                }
                const double integerEdge = std::round(crossing);
                const double crossingRoundoff = std::abs(crossing - integerEdge);
                result.crossingsMeasured++;
                result.maximumCrossingRoundoff = std::max(result.maximumCrossingRoundoff, crossingRoundoff);
                if (crossingRoundoff > LocatorLimits::scaleRoundoffTolerance)
                {
                    return Fail("NonIntegerSharpEdge", Erasure::InvalidGeometry, reader.WorkUnits());
                }
                double before = 0;
                double after = 0;
                const double beforeX = horizontal ? integerEdge - 1 : perpendicular - 0.5;
                const double beforeY = horizontal ? perpendicular - 0.5 : integerEdge - 1;
                const double afterX = horizontal ? integerEdge : perpendicular - 0.5;
                const double afterY = horizontal ? perpendicular - 0.5 : integerEdge;
                if (!reader.Contains(beforeX, beforeY) || !reader.Contains(afterX, afterY) ||
                    !reader.Sample(beforeX, beforeY, before) || !reader.Sample(afterX, afterY, after))
                {
                    return Fail("EdgeReadRejected", reader.Error() != Erasure::None ? reader.Error() : Erasure::InvalidGeometry, reader.WorkUnits());
                }
                const double endpointResidual = std::max(std::abs(before - (rising ? marker.blackLevel : marker.whiteLevel)),
                    std::abs(after - (rising ? marker.whiteLevel : marker.blackLevel)));
                result.endpointsMeasured++;
                result.maximumEndpointResidual = std::max(result.maximumEndpointResidual, endpointResidual);
                if (endpointResidual > roundoff)
                {
                    return Fail("NonSharpAdjacentPixels", Erasure::InvalidGeometry, reader.WorkUnits());
                }
                const double edgeOrigin = integerEdge - logical;
                result.originsMeasured++;
                result.maximumOriginResidual = std::max(result.maximumOriginResidual, std::abs(edgeOrigin));
                // For a canonical ROI and unit scale the complete-canvas origin
                // interval is {0}; each measured point-support interval must contain it.
                if (edgeOrigin - 0.5 > 0 || edgeOrigin + 0.5 < 0)
                {
                    return Fail("CanonicalOriginNotSupported", Erasure::InvalidGeometry, reader.WorkUnits());
                }
                if (result.maximumOriginResidual > policy.maximumGeometryResidualPixels)
                {
                    return Fail("CallerResidualExceeded", Erasure::InvalidGeometry, reader.WorkUnits());
                }
                result.sharpEdges++;
            }
        }
    }
    const LocalDesktopGeometry geometry{0, 0, 1, 1, result.maximumOriginResidual};
    result.bootstrap = EvaluateG1ValidatedGeometry(reader, policy, geometry, detail::LocalDesktopBinding::UnifiedVisual);
    result.bootstrap.markerCandidates = 4;
    result.bootstrap.geometryCandidates = 1;
    result.bootstrap.workUnits = reader.WorkUnits();
    if (!result.bootstrap.IsAccepted())
    {
        return Fail("BootstrapRejected", result.bootstrap.erasure, reader.WorkUnits());
    }
    const auto parsed = pbprotocol::ParseBootstrapRecord(result.bootstrap.canonical44);
    if (!parsed || parsed.Value().visualProfileId != kUnifiedVisualProfile.productProfile.visualProfileId ||
        parsed.Value().visualLayoutVersion != kUnifiedVisualProfile.productProfile.visualLayoutVersion)
    {
        return Fail("UnsupportedRecord", Erasure::UnsupportedRecord, reader.WorkUnits());
    }
    result.reason = "AcceptedSharpCanonicalCandidate";
    return result;
}
}

namespace pbg1
{
CanonicalCandidate ValidateCanonicalCandidate(const pbmodulation::LumaView& view, const pbmodulation::LocalDesktopDecodePolicy& policy) noexcept
{
    return pbmodulation::EvaluateG1CanonicalCandidate(view, policy);
}
}
