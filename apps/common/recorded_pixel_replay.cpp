#include "recorded_pixel_replay.h"

namespace pbapp
{
bool RecordedTimeline::Observe(const RecordedPixelFrame& frame) noexcept
{
    // Bounds make subtraction exact in int64, including negative initial PTS.
    constexpr std::int64_t maximumAbsolutePts = 1000000000000LL;
    if (frame.pts < -maximumAbsolutePts || frame.pts > maximumAbsolutePts || frame.timeBaseNumerator <= 0 || frame.timeBaseNumerator > 1000000 ||
        frame.timeBaseDenominator <= 0 || frame.timeBaseDenominator > 1000000000 || frame.duration < 0)
    {
        return false;
    }
    if (started && (frame.timeBaseNumerator != numerator || frame.timeBaseDenominator != denominator || frame.pts < lastPts))
    {
        return false;
    }
    const auto delta = started ? frame.pts - firstPts : 0;
    if (delta * static_cast<std::int64_t>(frame.timeBaseNumerator) > 120LL * frame.timeBaseDenominator)
    {
        return false;
    }
    if (!started)
    {
        firstPts = frame.pts;
        numerator = frame.timeBaseNumerator;
        denominator = frame.timeBaseDenominator;
        started = true;
    }
    lastPts = frame.pts;
    return true;
}

double RecordedTimeline::SpanSeconds() const noexcept
{
    return started ? static_cast<double>(lastPts - firstPts) * numerator / denominator : 0;
}
} // namespace pbapp
