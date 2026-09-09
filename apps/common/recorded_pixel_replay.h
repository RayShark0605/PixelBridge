#pragma once

#include "application_model.h"
#include "recorded_pixel_source.h"
#include <filesystem>
#include <ostream>

namespace pbapp
{

// Original PTS is evidence only. GPU retirement, frame age and Receiver timeouts
// use local processing QPC; neither clock is certified live channel time.
struct RecordedTimeline
{
    bool started = false;
    std::int64_t firstPts = 0;
    std::int64_t lastPts = 0;
    std::int32_t numerator = 0;
    std::int32_t denominator = 0;
    [[nodiscard]] bool Observe(const RecordedPixelFrame& frame) noexcept;
    [[nodiscard]] double SpanSeconds() const noexcept;
};

struct RecordedPixelReplayResult
{
    DecoderSnapshot decoder;
    RecordedTimeline timeline;
    std::uint64_t frames = 0;
    std::uint64_t processingMilliseconds = 0;
    bool reachedEof = false;
    bool prefixLimitReached = false;
    bool publishedAndReopened = false;
    std::string error;
};

// Headless experiment API only. Creates a new output directory; never starts a
// capture backend or window. WARP is explicit, not a silent hardware fallback.
// Limits: 7200 frames, 120 s PTS span, 900 s processing, 64 MiB trace.
// Receiver decisions are opt-in, output-only, and contain at most 15 slots.
[[nodiscard]] RecordedPixelReplayResult RunRecordedPixelReplay(RecordedPixelSource& source, const std::filesystem::path& newOutputDirectory, std::ostream& trace,
    std::shared_ptr<pbcore::StageDiagnostics> diagnostics, std::uint32_t maximumObservations = 7200, bool receiverDecisionDiagnostics = false);

} // namespace pbapp
