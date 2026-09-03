#pragma once

#include "pbmodulation/unified_visual.h"
#include "pbmodulation/visual_temporal.h"
#include "pbremotevisualsimulator/channel_transform.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace unifiedtransformtest
{

inline constexpr pbprotocol::SessionTag kCorpusSessionTag{0x4754313053455353ULL};
inline constexpr std::uint64_t kCorpusCaptureEpoch = 1;
inline constexpr std::size_t kMandatoryTransformCaseCount = 18;

enum class MandatoryTransformExpectation : std::uint8_t
{
    FullRecovery,
    BlurQuantized,
    BaseCapacity,
    NeutralChromaBaseCapacity,
    LocalizedStale,
    CropNoOutput,
    BootstrapConflictNoOutput,
    CallerIdentityNoOutput
};

struct MandatoryTransformCase
{
    std::string_view name;
    std::uint64_t sequence = 0;
    std::vector<pbremotevisualsimulator::ChannelTransform> transforms;
    std::optional<std::uint64_t> referenceSequence;
    pbmodulation::UnifiedExpectedFrameIdentity expectedIdentity;
    bool conflictExpected = false;
    std::optional<pbmodulation::VisualIdentityDisposition> expectedTemporalDisposition;
    MandatoryTransformExpectation expectation = MandatoryTransformExpectation::FullRecovery;
};

[[nodiscard]] inline std::uint64_t MakeCorpusSeed(const std::string_view name,
    const std::uint64_t sequence) noexcept
{
    std::uint64_t seed = 1469598103934665603ULL;
    for (const char character : name)
    {
        seed ^= static_cast<std::uint8_t>(character);
        seed *= 1099511628211ULL;
    }
    return seed ^ sequence;
}

[[nodiscard]] inline std::vector<MandatoryTransformCase> MakeMandatoryTransformCases()
{
    using namespace pbmodulation;
    using namespace pbremotevisualsimulator;

    const std::array<std::byte, 4> matte{
        std::byte{128}, std::byte{128}, std::byte{128}, std::byte{255}};
    std::vector<MandatoryTransformCase> cases;
    cases.reserve(kMandatoryTransformCaseCount);
    cases.push_back({"scale-075-area-centered", 41,
        {ResampleTransform{1920, 1080, 0.75, 0.75, 240, 135, ResampleFilter::Area, matte}}});
    cases.push_back({"scale-085-area-centered", 41,
        {ResampleTransform{1920, 1080, 0.85, 0.85, 144, 81, ResampleFilter::Area, matte}}});
    cases.push_back({"scale-100-area", 41,
        {ResampleTransform{1920, 1080, 1, 1, 0, 0, ResampleFilter::Area, matte}}});
    cases.push_back({"scale-150-bilinear", 41,
        {ResampleTransform{2880, 1620, 1.5, 1.5, 0, 0, ResampleFilter::Bilinear, matte}}});
    cases.push_back({"scale-200-bilinear", 41,
        {ResampleTransform{3840, 2160, 2, 2, 0, 0, ResampleFilter::Bilinear, matte}}});
    cases.push_back({"fractional-origin-arbitrary-letterbox", 41,
        {ResampleTransform{2560, 1600, 1.25, 1.25, 80.25, 125.5, ResampleFilter::Bilinear, matte}}});
    cases.push_back({"moderate-gaussian-blur-quant6", 41,
        {Kernel3x3Transform{FixedKernel3x3::GaussianBlur, 1}, ChannelQuantizationTransform{6}},
        std::nullopt, {}, false, std::nullopt, MandatoryTransformExpectation::BlurQuantized});
    cases.push_back({"chroma420-misaligned-phase-11", 41, {ChromaSubsample420Transform{1, 1}},
        std::nullopt, {}, false, std::nullopt, MandatoryTransformExpectation::BaseCapacity});
    cases.push_back({"neutral-chroma-scale-075", 41,
        {ResampleTransform{1920, 1080, 0.75, 0.75, 240, 135, ResampleFilter::Area, matte},
            NeutralChromaTransform{}},
        std::nullopt, {}, false, std::nullopt, MandatoryTransformExpectation::NeutralChromaBaseCapacity});
    cases.push_back({"localized-old-new-center", 41, {BlockReplacementTransform{560, 382, 800, 316}},
        40, {}, false, std::nullopt, MandatoryTransformExpectation::LocalizedStale});
    cases.push_back({"crop-right-locator-negative", 41, {CropTransform{0, 0, 1800, 1080}},
        std::nullopt, {}, false, std::nullopt, MandatoryTransformExpectation::CropNoOutput});
    cases.push_back({"bootstrap-copy-conflict-negative", 41,
        {BlockReplacementTransform{96, 16, 608, 64}}, 40,
        UnifiedExpectedFrameIdentity{true, kCorpusSessionTag, true, 41}, true, std::nullopt,
        MandatoryTransformExpectation::BootstrapConflictNoOutput});
    cases.push_back({"caller-identity-conflict-negative", 41, {}, std::nullopt,
        UnifiedExpectedFrameIdentity{true, pbprotocol::SessionTag{kCorpusSessionTag.value ^ 1ULL}, true, 41}, true,
        std::nullopt, MandatoryTransformExpectation::CallerIdentityNoOutput});
    cases.push_back({"temporal-sequence-50-unique", 50, {}, std::nullopt, {}, false,
        VisualIdentityDisposition::Unique});
    cases.push_back({"temporal-sequence-50-duplicate", 50, {}, std::nullopt, {}, false,
        VisualIdentityDisposition::Duplicate});
    cases.push_back({"temporal-sequence-52-gap", 52, {}, std::nullopt, {}, false,
        VisualIdentityDisposition::Unique});
    cases.push_back({"temporal-sequence-51-reordered", 51, {}, std::nullopt, {}, false,
        VisualIdentityDisposition::Reordered});
    cases.push_back({"temporal-sequence-54-gap", 54, {}, std::nullopt, {}, false,
        VisualIdentityDisposition::Unique});
    return cases;
}

} // namespace unifiedtransformtest
