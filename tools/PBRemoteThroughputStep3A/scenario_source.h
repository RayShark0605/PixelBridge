#pragma once

#include "support.h"
#include "recorded_pixel_source.h"
#include "pbremotevisualsimulator/channel_transform.h"
#include <memory>
#include <ostream>

namespace pbstep3a
{
enum class Scenario : std::uint8_t
{
    Clean, RepeatEach, BurstLoss, NeutralChroma, QuantizeSix, BootstrapConflict, LocalFreshnessMix, MarkerPlusOne, ShiftRightOne
};

[[nodiscard]] Scenario ParseScenario(std::wstring_view value);
[[nodiscard]] const char* ScenarioName(Scenario value) noexcept;
[[nodiscard]] std::uint32_t ObservationCount(Scenario value) noexcept;
[[nodiscard]] std::uint32_t SourceOrdinal(Scenario value, std::uint32_t observation);
[[nodiscard]] std::int64_t SyntheticPts(Scenario value, std::uint32_t observation);

class ScenarioSource final : public pbapp::RecordedPixelSource
{
public:
    ScenarioSource(const std::filesystem::path& root, Scenario scenario, std::ostream& audit);
    [[nodiscard]] pbapp::RecordedPixelRead ReadNext(pbapp::RecordedPixelFrame& output, std::string& error) override;
private:
    Scenario scenario_;
    std::ostream& audit_;
    std::array<std::unique_ptr<LockedRaster>, kFixtureFrames> rasters_;
    pbremotevisualsimulator::BgraImage current_;
    std::uint32_t observations_ = 0;
    std::uint64_t traceBytes_ = 0;
};
}
