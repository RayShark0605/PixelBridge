#include "scenario_source.h"
#include "recorded_pixel_replay.h"
#include <algorithm>
#include <chrono>
#include <sstream>
#include <stdexcept>

namespace pbstep3a
{
namespace
{
constexpr std::array<const char*, 9> names{"clean", "repeat-each", "burst-loss", "neutral-chroma", "quantize-6", "bootstrap-conflict", "local-freshness-mix", "marker-plus-one", "shift-right-one"};
constexpr std::array<std::wstring_view, 9> wideNames{L"clean", L"repeat-each", L"burst-loss", L"neutral-chroma", L"quantize-6", L"bootstrap-conflict", L"local-freshness-mix", L"marker-plus-one", L"shift-right-one"};
}

Scenario ParseScenario(const std::wstring_view value)
{
    for (std::size_t index = 0; index < wideNames.size(); index++)
    {
        if (wideNames[index] == value)
        {
            return static_cast<Scenario>(index);
        }
    }
    throw std::runtime_error("Unknown fixed Step3-A scenario");
}

const char* ScenarioName(const Scenario value) noexcept
{
    const auto index = static_cast<std::size_t>(value);
    return index < names.size() ? names[index] : "invalid";
}

std::uint32_t ObservationCount(const Scenario value) noexcept
{
    return value == Scenario::RepeatEach ? 8 : value == Scenario::BurstLoss ? 2 : kFixtureFrames;
}

std::uint32_t SourceOrdinal(const Scenario value, const std::uint32_t observation)
{
    Require(observation < ObservationCount(value), "Observation index outside scenario budget");
    return value == Scenario::RepeatEach ? observation / 2 : value == Scenario::BurstLoss ? observation * 3 : observation;
}

std::int64_t SyntheticPts(const Scenario value, const std::uint32_t observation)
{
    const auto ordinal = SourceOrdinal(value, observation);
    return value == Scenario::RepeatEach ? observation : static_cast<std::int64_t>(ordinal) * 2;
}

ScenarioSource::ScenarioSource(const std::filesystem::path& root, const Scenario scenario, std::ostream& audit) : scenario_(scenario), audit_(audit)
{
    RequireLocalPath(root);
    for (std::uint32_t index = 0; index < kFixtureFrames; index++)
    {
        rasters_[index] = std::make_unique<LockedRaster>(FramePath(root, index));
    }
}

pbapp::RecordedPixelRead ScenarioSource::ReadNext(pbapp::RecordedPixelFrame& output, std::string& error)
{
    try
    {
        if (observations_ == ObservationCount(scenario_))
        {
            return pbapp::RecordedPixelRead::EndOfFile;
        }
        const auto ordinal = SourceOrdinal(scenario_, observations_);
        const auto pixels = rasters_[ordinal]->Read();
        const auto sourceDigest = Digest(pixels);
        const pbremotevisualsimulator::BgraImageView view{pixels, 1920, 1080, 7680};
        std::optional<pbremotevisualsimulator::BgraImageView> reference;
        std::vector<std::byte> referencePixels;
        std::optional<std::uint32_t> referenceOrdinal;
        std::array<pbremotevisualsimulator::ChannelTransform, 1> transforms{};
        std::size_t transformCount = 0;
        using namespace pbremotevisualsimulator;
        switch (scenario_)
        {
        case Scenario::NeutralChroma:
            transforms[transformCount++] = NeutralChromaTransform{};
            break;
        case Scenario::QuantizeSix:
            transforms[transformCount++] = ChannelQuantizationTransform{6};
            break;
        case Scenario::BootstrapConflict:
        case Scenario::LocalFreshnessMix:
        {
            referenceOrdinal = (ordinal + 1) % kFixtureFrames;
            referencePixels = rasters_[*referenceOrdinal]->Read();
            reference = BgraImageView{referencePixels, 1920, 1080, 7680};
            const auto& region = scenario_ == Scenario::BootstrapConflict ? pbmodulation::kLocalDesktopBootstrapRegions[1] : pbmodulation::kLocalDesktopTimingRegions[0];
            transforms[transformCount++] = BlockReplacementTransform{region.x, region.y, region.width, region.height};
            break;
        }
        case Scenario::MarkerPlusOne:
        {
            constexpr std::uint32_t column = 20;
            constexpr std::uint32_t row = 47;
            std::array<std::byte, 4> color{};
            const auto offset = (static_cast<std::size_t>(row) * 1920 + column) * 4;
            for (std::size_t channel = 0; channel < 4; channel++)
            {
                const auto value = std::to_integer<unsigned>(pixels[offset + channel]);
                color[channel] = static_cast<std::byte>(channel == 3 ? value : std::min(255U, value + 1));
            }
            transforms[transformCount++] = SolidOverlayTransform{column, row, 1, 1, color, 255};
            break;
        }
        case Scenario::ShiftRightOne:
            transforms[transformCount++] = ResampleTransform{1920, 1080, 1, 1, 1, 0, ResampleFilter::Bilinear,
                {std::byte{128}, std::byte{128}, std::byte{128}, std::byte{255}}};
            break;
        default:
            break;
        }
        const auto started = std::chrono::steady_clock::now();
        auto transformed = ExecuteChannelTransformPlan(view, reference, {kTransformSeed, std::span(transforms).first(transformCount)});
        Require(static_cast<bool>(transformed), std::string("Simulator rejected transform: ") + GetChannelTransformErrorName(transformed.Error().code));
        const auto transformNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count();
        auto& execution = transformed.Value();
        current_ = std::move(execution.output);
        Require(current_.width == 1920 && current_.height == 1080 && current_.rowPitch == 7680 && current_.pixels.size() == pbmodulation::kUnifiedFrameBgraBytes,
            "Transform output changed Step2 pixel input contract");
        const auto pts = SyntheticPts(scenario_, observations_);
        std::ostringstream row;
        row << "{\"observation\":" << observations_ + 1 << ",\"sourceOrdinal\":" << ordinal << ",\"referenceOrdinal\":";
        if (referenceOrdinal)
        {
            row << *referenceOrdinal;
        }
        else
        {
            row << "null";
        }
        row << ",\"syntheticPts\":" << pts << ",\"timeBaseNumerator\":1,\"timeBaseDenominator\":30,\"duration\":" << (scenario_ == Scenario::RepeatEach ? 1 : 2)
            << ",\"sourceRawBlake3\":\"" << sourceDigest << "\",\"outputRawBlake3\":\"" << Digest(current_.pixels) << "\",\"transformProcessingNanoseconds\":" << transformNanoseconds
            << ",\"canonicalTransformManifestBlake3\":\"" << ChannelDigestToHex(execution.manifestBlake3) << "\",\"transformManifest\":" << execution.canonicalManifestJson << "}\n";
        const auto text = row.str();
        Require(text.size() <= 65536 && traceBytes_ <= kMaximumTraceBytes - text.size(), "Transform trace limit");
        audit_ << text;
        Require(audit_.good(), "Transform trace write failed");
        traceBytes_ += text.size();
        output = {current_.pixels, current_.width, current_.height, static_cast<std::uint32_t>(current_.rowPitch), pts, 1, 30, scenario_ == Scenario::RepeatEach ? 1 : 2};
        observations_++;
        return pbapp::RecordedPixelRead::Frame;
    }
    catch (const std::exception& failure)
    {
        error = failure.what();
        return pbapp::RecordedPixelRead::Error;
    }
}

void RunSelfChecks()
{
    pbapp::RecordedTimeline timeline;
    pbapp::RecordedPixelFrame frame;
    frame.timeBaseNumerator = 1;
    frame.timeBaseDenominator = 30;
    frame.duration = 1;
    Require(timeline.Observe(frame) && timeline.Observe(frame), "Equal PTS should remain legal");
    frame.pts = -1;
    Require(!timeline.Observe(frame), "Backward PTS not rejected");
    frame.pts = 3601;
    Require(!timeline.Observe(frame), "PTS resource limit not enforced");
    frame.pts = 1;
    frame.timeBaseDenominator = 31;
    Require(!timeline.Observe(frame), "Changed timebase not rejected");
    for (std::uint32_t index = 0; index < 8; index++)
    {
        Require(SourceOrdinal(Scenario::RepeatEach, index) == index / 2 && SyntheticPts(Scenario::RepeatEach, index) == index, "Duplicate mapping changed");
    }
    Require(SourceOrdinal(Scenario::BurstLoss, 0) == 0 && SourceOrdinal(Scenario::BurstLoss, 1) == 3 && SyntheticPts(Scenario::BurstLoss, 1) == 6, "Burst mapping changed");
    bool rejected = false;
    try
    {
        static_cast<void>(SourceOrdinal(Scenario::Clean, 4));
    }
    catch (const std::exception&)
    {
        rejected = true;
    }
    Require(rejected, "Out-of-range observation not rejected");
    using namespace pbremotevisualsimulator;
    const std::array<std::byte, 16> pixels{};
    const BgraImageView valid{pixels, 2, 2, 8};
    Require(!ExecuteChannelTransformPlan({pixels, 2, 2, 4}, std::nullopt, {kTransformSeed, {}}), "Invalid row pitch not rejected");
    ChannelTransformPolicy tiny;
    tiny.maximumResidentBytes = 1;
    Require(!ExecuteChannelTransformPlan(valid, std::nullopt, {kTransformSeed, {}}, tiny), "Simulator byte limit not enforced");
    const std::array<ChannelTransform, 1> missingReference{BlockReplacementTransform{0, 0, 1, 1}};
    Require(!ExecuteChannelTransformPlan(valid, std::nullopt, {kTransformSeed, missingReference}), "Missing reference not rejected");
    const std::array<ChannelTransform, 1> invalidQuantization{ChannelQuantizationTransform{1}};
    Require(!ExecuteChannelTransformPlan(valid, std::nullopt, {kTransformSeed, invalidQuantization}), "Invalid quantization not rejected");
    const auto first = ExecuteChannelTransformPlan(valid, std::nullopt, {kTransformSeed, {}});
    const auto second = ExecuteChannelTransformPlan(valid, std::nullopt, {kTransformSeed, {}});
    Require(first && second && first.Value().canonicalManifestJson == second.Value().canonicalManifestJson &&
        std::ranges::equal(first.Value().output.pixels, pixels), "Simulator identity determinism failed");
}
}
